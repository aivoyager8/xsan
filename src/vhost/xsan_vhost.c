#include "xsan_vhost.h"
#include "xsan_volume_manager.h"
#include "xsan_io.h" // For xsan_io_request_t and user completion callback
#include "xsan_memory.h"
#include "xsan_log.h"
#include "xsan_error.h"
#include "xsan_string_utils.h" // For xsan_strcpy_safe

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"      // For config parsing if we support it directly
#include "spdk/util.h"      // For spdk_util_roundup

// --- Module Globals & Context ---

static xsan_volume_manager_t *g_volume_manager = NULL; // Initialized by xsan_vhost_subsystem_init
static pthread_mutex_t g_xsan_vbdev_list_lock = PTHREAD_MUTEX_INITIALIZER;

// Context for each XSAN virtual bdev instance
typedef struct xsan_vbdev {
    struct spdk_bdev bdev;              // SPDK bdev structure must be first
    xsan_volume_id_t xsan_volume_id;    // ID of the XSAN volume this vbdev exposes
    xsan_volume_t *xsan_volume_ptr;     // Cached pointer to the XSAN volume (lifetime managed by volume_manager)
    char *name;                         // Name of this vbdev instance
    // Potentially a list of these vbdevs
    struct xsan_vbdev *next;
} xsan_vbdev_t;

static xsan_vbdev_t *g_xsan_vbdev_head = NULL; // Simple linked list of our vbdevs

// Context for an I/O channel for an XSAN vbdev
typedef struct xsan_vbdev_io_channel {
    // We might need to store things here if there's per-channel state,
    // e.g., for io_uring polling or other SPDK channel features.
    // For now, it might be simple.
    // struct spdk_poller *poller; // Example if a per-channel poller was needed
} xsan_vbdev_io_channel_t;


// --- Forward declarations for SPDK bdev module callbacks ---
static int _xsan_vbdev_init(void);
static void _xsan_vbdev_fini(void);
static void _xsan_vbdev_get_ctx_size(void); // Bdev I/O context size, not used by all modules
static int _xsan_vbdev_destruct(void *ctx);
static void _xsan_vbdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static bool _xsan_vbdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
static struct spdk_io_channel *_xsan_vbdev_get_io_channel(void *ctx);
static void _xsan_vbdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);
// static int _xsan_vbdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);


// SPDK bdev module interface structure
static struct spdk_bdev_module xsan_vbdev_module = {
    .name = "xsan_vbdev",
    .module_init = _xsan_vbdev_init,
    .module_fini = _xsan_vbdev_fini,
    .get_ctx_size = _xsan_vbdev_get_ctx_size,
    // .examine_config = NULL, // If we supported config file entries for our vbdevs
    // .config_json = _xsan_vbdev_write_config_json, // For writing current config
};


// --- SPDK Bdev Module Callbacks Implementation ---

static int _xsan_vbdev_init(void) {
    XSAN_LOG_INFO("XSAN Virtual Bdev module initializing.");
    // This function is called by SPDK when the module is loaded.
    // We can perform global setup for our vbdev type here.
    // g_volume_manager should already be set by xsan_vhost_subsystem_init.
    if (!g_volume_manager) {
        XSAN_LOG_ERROR("XSAN vbdev module init failed: Volume Manager not available.");
        return -1;
    }
    return 0;
}

static void _xsan_vbdev_fini(void) {
    XSAN_LOG_INFO("XSAN Virtual Bdev module finalizing.");
    // Clean up any global resources for the module.
    // Individual vbdev instances should be destroyed before this is called.
}

// Bdev I/O context size - this is for spdk_bdev_io's driver_ctx.
// We will use the cb_arg of spdk_bdev_io for our xsan_io_request_t.
static void _xsan_vbdev_get_ctx_size(void) {
    // If our module needed to store custom data per spdk_bdev_io,
    // spdk_bdev_module_get_ μεγάλο_ctx_size would be called, and we'd return a size.
    // For now, we don't need extra context in spdk_bdev_io itself.
    // Call this to inform SPDK that this module wants to use bdev_io->driver_ctx.
    // int ctx_size = sizeof(my_bdev_io_ctx); // If we had one
    // spdk_bdev_module_set_ctx_size(&xsan_vbdev_module, ctx_size);
    // For now, not setting it, meaning we won't use bdev_io->driver_ctx.
}


// Destructor for an xsan_vbdev_t instance (called by SPDK when a bdev is unregistered)
static int _xsan_vbdev_destruct(void *ctx) {
    xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)ctx;
    if (!xvbdev) {
        return -1;
    }
    XSAN_LOG_INFO("Destructing XSAN vbdev: %s (mapped to XSAN VolumeID: %s)",
                  xvbdev->name, spdk_uuid_get_string((struct spdk_uuid*)&xvbdev->xsan_volume_id.data[0]));

    pthread_mutex_lock(&g_xsan_vbdev_list_lock);
    xsan_vbdev_t *prev = NULL;
    xsan_vbdev_t *curr = g_xsan_vbdev_head;
    while(curr != NULL) {
        if (curr == xvbdev) {
            if (prev) prev->next = curr->next;
            else g_xsan_vbdev_head = curr->next;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_xsan_vbdev_list_lock);

    if (xvbdev->name) XSAN_FREE(xvbdev->name);
    XSAN_FREE(xvbdev);
    return 0;
}

// SPDK bdev I/O submission callback for XSAN vbdev
// This is the heart of the vbdev: translates SPDK bdev IOs to XSAN Volume IOs.
static void _xsan_vbdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io) {
    // xsan_vbdev_io_channel_t *xsan_ch = (xsan_vbdev_io_channel_t *)spdk_io_channel_get_ctx(ch);
    xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)bdev_io->bdev->ctxt;

    // Store bdev_io in our xsan_io_request_t to complete it later
    // The cb_arg for xsan_volume_read/write_async will be this bdev_io.
    bdev_io->internal.cb_arg = bdev_io; // Pass bdev_io itself to our xsan completion callback

    uint64_t offset_bytes = bdev_io->u.bdev.offset_blocks * xvbdev->bdev.blocklen;
    uint64_t length_bytes = bdev_io->u.bdev.num_blocks * xvbdev->bdev.blocklen;

    XSAN_LOG_DEBUG("XSAN vbdev '%s': submit_request type %d, offset_blocks %lu, num_blocks %u",
                   xvbdev->name, bdev_io->type, bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks);

    switch (bdev_io->type) {
        case SPDK_BDEV_IO_TYPE_READ:
            if (!bdev_io->u.bdev.iovs || bdev_io->u.bdev.iovcnt != 1) {
                XSAN_LOG_ERROR("XSAN vbdev READ: Complex I दुसरा (iovcnt %d != 1) not yet supported.", bdev_io->u.bdev.iovcnt);
                spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
            }
            // For simplicity, assume single iov for now. A real vbdev handles multiple iovs.
            // The user_buffer for xsan_volume_read_async will be bdev_io->u.bdev.iovs[0].iov_base
            // Length is bdev_io->u.bdev.iovs[0].iov_len (should match length_bytes)

            // We need a XSAN completion callback that calls spdk_bdev_io_complete.
            // Let's define it:
            // static void _xsan_vbdev_io_complete_cb(void *cb_arg, xsan_error_t status) {
            //    struct spdk_bdev_io *bdev_io_to_complete = (struct spdk_bdev_io *)cb_arg;
            //    spdk_bdev_io_complete(bdev_io_to_complete, status == XSAN_OK ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
            // }
            // This is tricky because xsan_volume_read_async takes its own cb_arg.
            // We need to chain these. The cb_arg for xsan_volume_read_async needs to be the spdk_bdev_io.

            // Simpler: Use xsan_io_request_t to manage this.
            // This requires xsan_volume_read_async to be adapted or a new layer.
            // For now, let's assume a direct call and a simple callback.
            // THIS IS A SIMPLIFICATION AND NEEDS REFINEMENT FOR ASYNC CHAINING.
            // The correct way is to have xsan_volume_read_async take a generic cb_arg,
            // and we pass spdk_bdev_io as that arg.
            XSAN_LOG_WARN("XSAN vbdev READ: Actual async chaining to xsan_volume_read_async and back to spdk_bdev_io_complete is complex and not fully implemented here.");
            // Placeholder: Directly complete for now, or use a simplified synchronous path if available.
            // To make it work, we'd need a proper context to pass to xsan_volume_read_async's callback.
            // For now, let's assume xsan_volume_read_async can somehow complete the bdev_io. This is not true with current API.
            // This part is the most complex: bridging SPDK's bdev_io completion with XSAN's async completion.

            // Let's assume we have a synchronous path for testing this stub for now, or just fail.
             spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED); // Placeholder until async chain is built
            break;

        case SPDK_BDEV_IO_TYPE_WRITE:
            if (!bdev_io->u.bdev.iovs || bdev_io->u.bdev.iovcnt != 1) {
                 XSAN_LOG_ERROR("XSAN vbdev WRITE: Complex I दुसरा (iovcnt %d != 1) not yet supported.", bdev_io->u.bdev.iovcnt);
                spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
            }
            XSAN_LOG_WARN("XSAN vbdev WRITE: Not fully implemented.");
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED); // Placeholder
            break;

        case SPDK_BDEV_IO_TYPE_UNMAP: // For TRIM/DISCARD
            XSAN_LOG_DEBUG("XSAN vbdev '%s': Received UNMAP/TRIM request.", xvbdev->name);
            // TODO: Translate to XSAN volume trim/unmap if supported
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS); // Assume success for now
            break;

        case SPDK_BDEV_IO_TYPE_FLUSH:
            XSAN_LOG_DEBUG("XSAN vbdev '%s': Received FLUSH request.", xvbdev->name);
            // TODO: Translate to XSAN volume flush if supported
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS); // Assume success
            break;

        case SPDK_BDEV_IO_TYPE_RESET:
             XSAN_LOG_WARN("XSAN vbdev '%s': Received RESET request. Not fully supported.", xvbdev->name);
             spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS); // Or FAILED if not supported
             break;

        default:
            XSAN_LOG_WARN("XSAN vbdev '%s': Unsupported IO type %d", xvbdev->name, bdev_io->type);
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            break;
    }
}

static bool _xsan_vbdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type) {
    // xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)ctx;
    switch (io_type) {
        case SPDK_BDEV_IO_TYPE_READ:
        case SPDK_BDEV_IO_TYPE_WRITE:
        case SPDK_BDEV_IO_TYPE_UNMAP:
        case SPDK_BDEV_IO_TYPE_FLUSH:
        case SPDK_BDEV_IO_TYPE_RESET: // Optional, but good to support
            return true;
        default:
            return false;
    }
}

static struct spdk_io_channel *_xsan_vbdev_get_io_channel(void *ctx) {
    // xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)ctx;
    // Create and return an xsan_vbdev_io_channel_t
    // This is called by SPDK per core that wants to submit IO to this bdev.
    xsan_vbdev_io_channel_t *ch_ctx = XSAN_CALLOC(1, sizeof(xsan_vbdev_io_channel_t));
    if (!ch_ctx) {
        XSAN_LOG_ERROR("Failed to allocate xsan_vbdev_io_channel context.");
        return NULL;
    }
    // If ch_ctx needed a poller or other per-thread resources, init them here.
    // For now, it's empty. SPDK will associate it with the spdk_io_channel.
    // We can retrieve it using spdk_io_channel_get_ctx(ch).
    XSAN_LOG_DEBUG("XSAN vbdev: get_io_channel called for bdev %s (ctx %p), created channel_ctx %p",
                  ((xsan_vbdev_t*)ctx)->name, ctx, ch_ctx);
    return spdk_io_channel_from_ctx(ch_ctx); // SPDK wraps our context
}

static void _xsan_vbdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w) {
    xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)ctx;
    char uuid_str[SPDK_UUID_STRING_LEN];

    spdk_json_write_name(w, "xsan_vbdev");
    spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "name", xvbdev->name);
    spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), (struct spdk_uuid*)&xvbdev->xsan_volume_id.data[0]);
    spdk_json_write_named_string(w, "xsan_volume_id", uuid_str);
    if (xvbdev->xsan_volume_ptr) {
        spdk_json_write_named_string(w, "xsan_volume_name", xvbdev->xsan_volume_ptr->name);
        spdk_json_write_named_uint64(w, "xsan_volume_size_bytes", xvbdev->xsan_volume_ptr->size_bytes);
    }
    spdk_json_write_object_end(w);
}


// --- Public API Implementation ---

xsan_error_t xsan_vhost_subsystem_init(struct xsan_volume_manager *vm) {
    if (!vm) {
        XSAN_LOG_ERROR("Volume Manager pointer is NULL for XSAN vhost init.");
        return XSAN_ERROR_INVALID_PARAM;
    }
    g_volume_manager = vm; // Store for vbdev callbacks

    // Register the XSAN vbdev module with SPDK's bdev subsystem
    spdk_bdev_module_register(&xsan_vbdev_module);
    XSAN_LOG_INFO("XSAN vhost subsystem (vbdev module) initialized and registered.");
    return XSAN_OK;
}

void xsan_vhost_subsystem_fini(void) {
    XSAN_LOG_INFO("XSAN vhost subsystem finalizing...");
    // Unregister vbdevs first (if any are still active)
    pthread_mutex_lock(&g_xsan_vbdev_list_lock);
    xsan_vbdev_t *xvbdev = g_xsan_vbdev_head;
    while(xvbdev != NULL) {
        xsan_vbdev_t *next = xvbdev->next;
        XSAN_LOG_INFO("Unregistering XSAN vbdev '%s' during subsystem fini.", xvbdev->bdev.name);
        spdk_bdev_unregister(&xvbdev->bdev, NULL, NULL); // Destructor _xsan_vbdev_destruct will be called
        xvbdev = next; // g_xsan_vbdev_head will be updated by _xsan_vbdev_destruct
    }
    g_xsan_vbdev_head = NULL; // Ensure list is cleared
    pthread_mutex_unlock(&g_xsan_vbdev_list_lock);

    spdk_bdev_module_release(&xsan_vbdev_module); // Not a real function, module_fini is callback based
    // The module_fini (_xsan_vbdev_fini) will be called by SPDK during spdk_app_fini or subsystem unload.
    g_volume_manager = NULL;
    XSAN_LOG_INFO("XSAN vhost subsystem finalized (module finish pending SPDK call).");
}

xsan_error_t xsan_vhost_expose_volume_as_vbdev(xsan_volume_id_t volume_id, const char *vbdev_name) {
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_vhost_expose_volume_as_vbdev must be called from an SPDK thread.");
        return XSAN_ERROR_THREAD_CONTEXT;
    }
    if (!g_volume_manager) {
        XSAN_LOG_ERROR("XSAN Volume Manager not available for exposing volume.");
        return XSAN_ERROR_INVALID_STATE;
    }
    if (!vbdev_name || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    xsan_volume_t *vol = xsan_volume_get_by_id(g_volume_manager, volume_id);
    if (!vol) {
        XSAN_LOG_ERROR("XSAN Volume with ID %s not found.", spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));
        return XSAN_ERROR_NOT_FOUND;
    }
    if (vol->block_size_bytes == 0 || vol->num_blocks == 0) {
        XSAN_LOG_ERROR("XSAN Volume '%s' has invalid size/block_size.", vol->name);
        return XSAN_ERROR_INVALID_STATE;
    }

    xsan_vbdev_t *xvbdev = (xsan_vbdev_t *)XSAN_CALLOC(1, sizeof(xsan_vbdev_t));
    if (!xvbdev) {
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    xvbdev->name = xsan_strdup(vbdev_name);
    if (!xvbdev->name) {
        XSAN_FREE(xvbdev);
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    memcpy(&xvbdev->xsan_volume_id, &volume_id, sizeof(xsan_volume_id_t));
    xvbdev->xsan_volume_ptr = vol; // Cache pointer to the volume

    // Populate the spdk_bdev structure within xsan_vbdev_t
    xvbdev->bdev.name = xvbdev->name;
    xvbdev->bdev.product_name = "XSAN Virtual Bdev"; // Product name for this vbdev type
    xvbdev->bdev.write_cache = 0; // No explicit write cache in the vbdev itself
    xvbdev->bdev.blocklen = vol->block_size_bytes;
    xvbdev->bdev.blockcnt = vol->num_blocks;
    // xvbdev->bdev.uuid = *(struct spdk_uuid *)&vol->id.data[0]; // Use volume's UUID for bdev UUID
    spdk_uuid_copy(&xvbdev->bdev.uuid, (struct spdk_uuid *)&vol->id.data[0]);

    xvbdev->bdev.ctxt = xvbdev; // SPDK bdev context points back to our xsan_vbdev_t
    xvbdev->bdev.fn_table = (struct spdk_bdev_fn_table*) & (struct spdk_bdev_fn_table) { // Cast to avoid const issue
        .destruct = _xsan_vbdev_destruct,
        .submit_request = _xsan_vbdev_submit_request,
        .io_type_supported = _xsan_vbdev_io_type_supported,
        .get_io_channel = _xsan_vbdev_get_io_channel,
        .dump_info_json = _xsan_vbdev_dump_info_json,
    };
    xvbdev->bdev.module = &xsan_vbdev_module;

    int rc = spdk_bdev_register(&xvbdev->bdev);
    if (rc != 0) {
        XSAN_LOG_ERROR("Failed to register XSAN vbdev '%s': %s", vbdev_name, spdk_strerror(-rc));
        if(xvbdev->name) XSAN_FREE(xvbdev->name);
        XSAN_FREE(xvbdev);
        return xsan_error_from_errno(-rc);
    }

    pthread_mutex_lock(&g_xsan_vbdev_list_lock);
    xvbdev->next = g_xsan_vbdev_head;
    g_xsan_vbdev_head = xvbdev;
    pthread_mutex_unlock(&g_xsan_vbdev_list_lock);

    XSAN_LOG_INFO("Successfully exposed XSAN Volume '%s' (ID: %s) as vbdev '%s'",
                  vol->name, spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), vbdev_name);
    return XSAN_OK;
}

xsan_error_t xsan_vhost_unexpose_volume_vbdev(const char *vbdev_name) {
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_vhost_unexpose_volume_vbdev must be called from an SPDK thread.");
        return XSAN_ERROR_THREAD_CONTEXT;
    }
    if (!vbdev_name) return XSAN_ERROR_INVALID_PARAM;

    // SPDK requires the bdev pointer to unregister. We find our xsan_vbdev_t first.
    pthread_mutex_lock(&g_xsan_vbdev_list_lock);
    xsan_vbdev_t *xvbdev = NULL;
    xsan_vbdev_t *prev = NULL;
    xsan_vbdev_t *curr = g_xsan_vbdev_head;
    while(curr != NULL) {
        if (strcmp(curr->name, vbdev_name) == 0) {
            xvbdev = curr;
            if (prev) prev->next = curr->next; // Unlink from our list
            else g_xsan_vbdev_head = curr->next;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_xsan_vbdev_list_lock);

    if (!xvbdev) {
        XSAN_LOG_ERROR("XSAN vbdev '%s' not found for unexposure.", vbdev_name);
        return XSAN_ERROR_NOT_FOUND;
    }

    // spdk_bdev_unregister will call the destruct function (_xsan_vbdev_destruct)
    // which will free xvbdev and its name.
    // The callback for unregister is optional if we don't need to do anything after it's confirmed.
    spdk_bdev_unregister(&xvbdev->bdev, NULL, NULL);
    XSAN_LOG_INFO("Unregistered XSAN vbdev '%s'. Destruction pending SPDK callback.", vbdev_name);
    return XSAN_OK;
}
