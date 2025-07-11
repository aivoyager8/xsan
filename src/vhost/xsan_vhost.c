#include "xsan_vhost.h"
#include "xsan_volume_manager.h" // For xsan_volume_get_by_id, xsan_volume_read/write_async
#include "xsan_io.h"             // For xsan_user_io_completion_cb_t (used by _xsan_vbdev_io_complete_cb)
#include "xsan_bdev.h"           // For xsan_bdev_dma_malloc/free, xsan_bdev_get_buf_align
#include "xsan_memory.h"
#include "xsan_log.h"
#include "xsan_error.h"
#include "xsan_string_utils.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/util.h"      // For spdk_min, spdk_iov_memcpy_from/to_iov
#include "spdk/vhost.h"     // For struct spdk_bdev_io (though usually from bdev.h)

#include <errno.h> // For ENOMEM etc.

// --- Module Globals & Context ---

static xsan_volume_manager_t *g_volume_manager = NULL;
static pthread_mutex_t g_xsan_vbdev_list_lock = PTHREAD_MUTEX_INITIALIZER;

// --- Internal Context Structures ---

typedef struct xsan_vbdev {
    struct spdk_bdev bdev;
    xsan_volume_id_t xsan_volume_id;
    xsan_volume_t *xsan_volume_ptr;
    char *name;
    struct xsan_vbdev *next;
} xsan_vbdev_t;

static xsan_vbdev_t *g_xsan_vbdev_head = NULL;

typedef struct xsan_vbdev_io_channel {
    // No specific members needed for now for basic passthrough
} xsan_vbdev_io_channel_t;

typedef struct xsan_vhost_io_ctx {
    struct spdk_bdev_io *bdev_io;
    xsan_volume_t *xsan_vol;
    void *dma_buf;
    uint64_t dma_buf_len;
    bool dma_buf_is_internal;
} xsan_vhost_io_ctx_t;


// --- Forward declarations for SPDK bdev module callbacks ---
static int _xsan_vbdev_init(void);
static void _xsan_vbdev_fini(void);
static void _xsan_vbdev_get_ctx_size(void);
static int _xsan_vbdev_destruct(void *ctx);
static void _xsan_vbdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static bool _xsan_vbdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
static struct spdk_io_channel *_xsan_vbdev_get_io_channel(void *ctx);
static void _xsan_vbdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);
static void _xsan_vbdev_destroy_io_channel(void *ctx, struct spdk_io_channel *ch);

// --- XSAN Vhost Internal I/O Completion Callback ---
static void _xsan_vbdev_io_complete_cb(void *cb_arg, xsan_error_t xsan_status);


static struct spdk_bdev_module xsan_vbdev_module = {
    .name = "xsan_vbdev",
    .module_init = _xsan_vbdev_init,
    .module_fini = _xsan_vbdev_fini,
    .get_ctx_size = _xsan_vbdev_get_ctx_size,
};

static const struct spdk_bdev_fn_table xsan_vbdev_fn_table = {
    .destruct = _xsan_vbdev_destruct,
    .submit_request = _xsan_vbdev_submit_request,
    .io_type_supported = _xsan_vbdev_io_type_supported,
    .get_io_channel = _xsan_vbdev_get_io_channel,
    .dump_info_json = _xsan_vbdev_dump_info_json,
    .destroy_io_channel = _xsan_vbdev_destroy_io_channel,
};

// --- SPDK Bdev Module Callbacks Implementation ---
static int _xsan_vbdev_init(void) {
    XSAN_LOG_INFO("XSAN vbdev module init.");
    if (!g_volume_manager) { XSAN_LOG_ERROR("XSAN vbdev init failed: Volume Manager NULL."); return -1; }
    // Register also the function table for our module's bdevs
    // spdk_bdev_module_set_fn_table(&xsan_vbdev_module, &xsan_vbdev_fn_table); // Not needed, fn_table is set on bdev instance
    return 0;
}
static void _xsan_vbdev_fini(void) { XSAN_LOG_INFO("XSAN vbdev module fini.");}
static void _xsan_vbdev_get_ctx_size(void) { /* Do nothing for now */ }

static int _xsan_vbdev_destruct(void *ctx) {
    xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)ctx; if (!xvbdev) return -1;
    XSAN_LOG_INFO("Destructing XSAN vbdev: %s", xvbdev->name);
    pthread_mutex_lock(&g_xsan_vbdev_list_lock);
    xsan_vbdev_t *p = NULL, *c = g_xsan_vbdev_head; while(c){if(c==xvbdev){if(p)p->next=c->next;else g_xsan_vbdev_head=c->next;break;}p=c;c=c->next;}
    pthread_mutex_unlock(&g_xsan_vbdev_list_lock);
    if (xvbdev->name) XSAN_FREE(xvbdev->name); XSAN_FREE(xvbdev); return 0;
}

static void _xsan_vbdev_io_complete_cb(void *cb_arg, xsan_error_t xsan_status) {
    xsan_vhost_io_ctx_t *vhost_io_ctx = (xsan_vhost_io_ctx_t *)cb_arg;
    if (!vhost_io_ctx || !vhost_io_ctx->bdev_io) {
        XSAN_LOG_ERROR("NULL context or bdev_io in _xsan_vbdev_io_complete_cb!");
        if (vhost_io_ctx && vhost_io_ctx->dma_buf_is_internal && vhost_io_ctx->dma_buf) xsan_bdev_dma_free(vhost_io_ctx->dma_buf);
        if (vhost_io_ctx) XSAN_FREE(vhost_io_ctx);
        return;
    }
    struct spdk_bdev_io *bdev_io = vhost_io_ctx->bdev_io;
    enum spdk_bdev_io_status spdk_status;

    if (xsan_status == XSAN_OK) {
        spdk_status = SPDK_BDEV_IO_STATUS_SUCCESS;
        if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ && vhost_io_ctx->dma_buf_is_internal && vhost_io_ctx->dma_buf) {
            if (bdev_io->u.bdev.iovs && bdev_io->u.bdev.iovcnt > 0) {
                size_t total_iov_len = 0; for(int i=0; i < bdev_io->u.bdev.iovcnt; ++i) total_iov_len += bdev_io->u.bdev.iovs[i].iov_len;
                size_t actual_copy_len = spdk_min(vhost_io_ctx->dma_buf_len, total_iov_len);
                size_t copied = spdk_iov_memcpy_to_iov(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, vhost_io_ctx->dma_buf, actual_copy_len);
                if (copied != actual_copy_len) { XSAN_LOG_ERROR("XSAN vbdev READ: spdk_iov_memcpy_to_iov partial copy (%zu/%zu).", copied, actual_copy_len); spdk_status = SPDK_BDEV_IO_STATUS_FAILED; }
                else { XSAN_LOG_TRACE("XSAN vbdev READ: Copied %zu bytes from DMA to iovs for bdev_io %p.", copied, (void*)bdev_io); }
            } else { XSAN_LOG_ERROR("XSAN vbdev READ: No iovs in bdev_io %p!", (void*)bdev_io); spdk_status = SPDK_BDEV_IO_STATUS_FAILED; }
        }
    } else {
        XSAN_LOG_ERROR("XSAN vbdev I/O for vol '%s' (bdev_io %p) failed xsan_status %d (%s)", vhost_io_ctx->xsan_vol ? vhost_io_ctx->xsan_vol->name : "UNKNOWN", (void*)bdev_io, xsan_status, xsan_error_string(xsan_status));
        spdk_status = SPDK_BDEV_IO_STATUS_FAILED;
    }
    if (vhost_io_ctx->dma_buf_is_internal && vhost_io_ctx->dma_buf) xsan_bdev_dma_free(vhost_io_ctx->dma_buf);
    XSAN_FREE(vhost_io_ctx);
    spdk_bdev_io_complete(bdev_io, spdk_status);
}

static void _xsan_vbdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io) {
    xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)bdev_io->bdev->ctxt;
    xsan_error_t err;

    xsan_vhost_io_ctx_t *vhost_io_ctx = XSAN_CALLOC(1, sizeof(xsan_vhost_io_ctx_t));
    if (!vhost_io_ctx) { spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM); return; }
    vhost_io_ctx->bdev_io = bdev_io;
    vhost_io_ctx->xsan_vol = xvbdev->xsan_volume_ptr;

    if (!vhost_io_ctx->xsan_vol) { XSAN_LOG_ERROR("vbdev %s no xsan_volume_ptr.", xvbdev->name); XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED); return; }

    uint64_t offset_bytes = bdev_io->u.bdev.offset_blocks * xvbdev->bdev.blocklen;
    uint64_t length_bytes = (uint64_t)bdev_io->u.bdev.num_blocks * xvbdev->bdev.blocklen;

    XSAN_LOG_TRACE("vbdev '%s': submit type %d, off_bytes %lu, len_bytes %lu, iovcnt %d", xvbdev->name, bdev_io->type, offset_bytes, length_bytes, bdev_io->u.bdev.iovcnt);

    switch (bdev_io->type) {
        case SPDK_BDEV_IO_TYPE_READ:
        case SPDK_BDEV_IO_TYPE_WRITE:
            if (length_bytes == 0) { XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS); return; }
            if (!bdev_io->u.bdev.iovs || bdev_io->u.bdev.iovcnt == 0) { XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED); return; }

            size_t dma_align = xsan_bdev_get_buf_align(xvbdev->name); // Use vbdev name for alignment, assuming it's consistent or first disk
            if(dma_align == 0) dma_align = 4096;

            vhost_io_ctx->dma_buf = xsan_bdev_dma_malloc(length_bytes, dma_align);
            if (!vhost_io_ctx->dma_buf) { XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM); return; }
            vhost_io_ctx->dma_buf_is_internal = true; vhost_io_ctx->dma_buf_len = length_bytes;

            if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
                size_t copied = spdk_iov_memcpy_from_iov(vhost_io_ctx->dma_buf, length_bytes, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt);
                if (copied != length_bytes) { /* Error, free bufs, complete failed */ xsan_bdev_dma_free(vhost_io_ctx->dma_buf); XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED); return; }
            }
            if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
                err = xsan_volume_read_async(g_volume_manager, xvbdev->xsan_volume_id, offset_bytes, length_bytes, vhost_io_ctx->dma_buf, _xsan_vbdev_io_complete_cb, vhost_io_ctx);
            } else {
                err = xsan_volume_write_async(g_volume_manager, xvbdev->xsan_volume_id, offset_bytes, length_bytes, vhost_io_ctx->dma_buf, _xsan_vbdev_io_complete_cb, vhost_io_ctx);
            }
            if (err != XSAN_OK) { /* Error, free bufs, complete failed */
                XSAN_LOG_ERROR("vbdev '%s': Failed submit to xsan_volume_async: %s", xvbdev->name, xsan_error_string(err));
                if (vhost_io_ctx->dma_buf_is_internal && vhost_io_ctx->dma_buf) xsan_bdev_dma_free(vhost_io_ctx->dma_buf);
                XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            } // Else, _xsan_vbdev_io_complete_cb handles completion
            break;
        case SPDK_BDEV_IO_TYPE_UNMAP: XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS); break;
        case SPDK_BDEV_IO_TYPE_FLUSH: XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS); break;
        case SPDK_BDEV_IO_TYPE_RESET: XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS); break;
        default: XSAN_FREE(vhost_io_ctx); spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOT_SUPPORTED); break;
    }
}

static bool _xsan_vbdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type) { /* ... as before ... */
    switch (io_type) { case SPDK_BDEV_IO_TYPE_READ: case SPDK_BDEV_IO_TYPE_WRITE: case SPDK_BDEV_IO_TYPE_UNMAP: case SPDK_BDEV_IO_TYPE_FLUSH: case SPDK_BDEV_IO_TYPE_RESET: return true; default: return false; }
}
static struct spdk_io_channel *_xsan_vbdev_get_io_channel(void *ctx) { /* ... as before ... */
    xsan_vbdev_io_channel_t *ch_ctx = XSAN_CALLOC(1, sizeof(xsan_vbdev_io_channel_t)); if (!ch_ctx) return NULL;
    return spdk_io_channel_from_ctx(ch_ctx);
}
static void _xsan_vbdev_destroy_io_channel(void *ctx, struct spdk_io_channel *ch) {
    xsan_vbdev_io_channel_t *ch_ctx = (xsan_vbdev_io_channel_t *)spdk_io_channel_get_ctx(ch);
    if (ch_ctx) XSAN_FREE(ch_ctx);
}
static void _xsan_vbdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w) { /* ... as before ... */
    xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)ctx; char uuid_str[SPDK_UUID_STRING_LEN];
    spdk_json_write_name(w, "xsan_vbdev"); spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "name", xvbdev->name);
    spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), (struct spdk_uuid*)&xvbdev->xsan_volume_id.data[0]);
    spdk_json_write_named_string(w, "xsan_volume_id", uuid_str);
    if (xvbdev->xsan_volume_ptr) {
        spdk_json_write_named_string(w, "xsan_volume_name", xvbdev->xsan_volume_ptr->name);
        spdk_json_write_named_uint64(w, "xsan_volume_size_bytes", xvbdev->xsan_volume_ptr->size_bytes);
    } spdk_json_write_object_end(w);
}

// --- Public API Implementation ---
xsan_error_t xsan_vhost_subsystem_init(struct xsan_volume_manager *vm) { /* ... as before ... */
    if(!vm) return XSAN_ERROR_INVALID_PARAM; g_volume_manager = vm;
    spdk_bdev_module_register(&xsan_vbdev_module); XSAN_LOG_INFO("XSAN vbdev module registered."); return XSAN_OK;
}
void xsan_vhost_subsystem_fini(void) { /* ... as before ... */
    XSAN_LOG_INFO("XSAN vhost subsystem finalizing...");
    pthread_mutex_lock(&g_xsan_vbdev_list_lock);
    xsan_vbdev_t *xvbdev = g_xsan_vbdev_head; xsan_vbdev_t *next;
    while(xvbdev != NULL) { next = xvbdev->next; XSAN_LOG_INFO("Unregistering XSAN vbdev '%s'", xvbdev->bdev.name); spdk_bdev_unregister(&xvbdev->bdev, NULL, NULL); xvbdev = next; }
    g_xsan_vbdev_head = NULL; pthread_mutex_unlock(&g_xsan_vbdev_list_lock);
    // spdk_bdev_module_release is not how modules are typically cleaned up; module_fini is called by SPDK.
    g_volume_manager = NULL; XSAN_LOG_INFO("XSAN vhost subsystem finalized.");
}

xsan_error_t xsan_vhost_expose_volume_as_vbdev(xsan_volume_id_t volume_id, const char *vbdev_name) { /* ... as before ... */
    if (spdk_get_thread() == NULL) return XSAN_ERROR_THREAD_CONTEXT;
    if (!g_volume_manager) return XSAN_ERROR_INVALID_STATE;
    if (!vbdev_name || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) return XSAN_ERROR_INVALID_PARAM;
    xsan_volume_t *vol = xsan_volume_get_by_id(g_volume_manager, volume_id);
    if (!vol || vol->block_size_bytes == 0 || vol->num_blocks == 0) return !vol ? XSAN_ERROR_NOT_FOUND : XSAN_ERROR_INVALID_STATE;
    pthread_mutex_lock(&g_xsan_vbdev_list_lock);
    xsan_vbdev_t *check = g_xsan_vbdev_head; while(check){if(strcmp(check->name,vbdev_name)==0){pthread_mutex_unlock(&g_xsan_vbdev_list_lock);return XSAN_ERROR_ALREADY_EXISTS;} check=check->next;}
    pthread_mutex_unlock(&g_xsan_vbdev_list_lock);
    xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)XSAN_CALLOC(1, sizeof(xsan_vbdev_t)); if (!xvbdev) return XSAN_ERROR_OUT_OF_MEMORY;
    xvbdev->name = xsan_strdup(vbdev_name); if (!xvbdev->name) { XSAN_FREE(xvbdev); return XSAN_ERROR_OUT_OF_MEMORY; }
    memcpy(&xvbdev->xsan_volume_id, &volume_id, sizeof(xsan_volume_id_t)); xvbdev->xsan_volume_ptr = vol;
    xvbdev->bdev.name = xvbdev->name; xvbdev->bdev.product_name = "XSAN Virtual Bdev";
    xvbdev->bdev.write_cache = 0; xvbdev->bdev.blocklen = vol->block_size_bytes; xvbdev->bdev.blockcnt = vol->num_blocks;
    spdk_uuid_copy(&xvbdev->bdev.uuid, (struct spdk_uuid *)&vol->id.data[0]);
    xvbdev->bdev.ctxt = xvbdev; xvbdev->bdev.fn_table = &xsan_vbdev_fn_table; xvbdev->bdev.module = &xsan_vbdev_module;
    int rc = spdk_bdev_register(&xvbdev->bdev);
    if (rc != 0) { XSAN_LOG_ERROR("Failed to register XSAN vbdev '%s': %s", vbdev_name, spdk_strerror(-rc)); if(xvbdev->name)XSAN_FREE(xvbdev->name); XSAN_FREE(xvbdev); return xsan_error_from_errno(-rc); }
    pthread_mutex_lock(&g_xsan_vbdev_list_lock); xvbdev->next = g_xsan_vbdev_head; g_xsan_vbdev_head = xvbdev; pthread_mutex_unlock(&g_xsan_vbdev_list_lock);
    XSAN_LOG_INFO("Exposed XSAN Vol '%s' as vbdev '%s'", vol->name, vbdev_name); return XSAN_OK;
}

xsan_error_t xsan_vhost_unexpose_volume_vbdev(const char *vbdev_name) { /* ... as before ... */
    if (spdk_get_thread() == NULL) return XSAN_ERROR_THREAD_CONTEXT;
    if (!vbdev_name) return XSAN_ERROR_INVALID_PARAM;
    pthread_mutex_lock(&g_xsan_vbdev_list_lock);
    xsan_vbdev_t *xvbdev = NULL, *prev = NULL, *curr = g_xsan_vbdev_head;
    while(curr){ if(strcmp(curr->name,vbdev_name)==0){xvbdev=curr; if(prev)prev->next=curr->next;else g_xsan_vbdev_head=curr->next;break;}prev=curr;curr=curr->next;}
    pthread_mutex_unlock(&g_xsan_vbdev_list_lock);
    if (!xvbdev) return XSAN_ERROR_NOT_FOUND;
    spdk_bdev_unregister(&xvbdev->bdev, NULL, NULL); // This will call _xsan_vbdev_destruct
    XSAN_LOG_INFO("Unregistered XSAN vbdev '%s'.", vbdev_name); return XSAN_OK;
}
