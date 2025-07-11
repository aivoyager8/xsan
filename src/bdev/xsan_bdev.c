#include "xsan_bdev.h"
#include "xsan_memory.h" // For XSAN_MALLOC, XSAN_FREE, XSAN_CALLOC
#include "xsan_error.h"
#include "xsan_string_utils.h" // For xsan_strcpy_safe
#include "xsan_log.h"

// SPDK Headers
#include "spdk/bdev.h"    // Main bdev interfaces
#include "spdk/env.h"     // For spdk_uuid_copy, spdk_dma_malloc, spdk_dma_free etc.
#include "spdk/thread.h"  // For spdk_get_thread(), spdk_io_channel
#include "spdk/bdev_module.h" // For spdk_bdev_open_ext, spdk_bdev_close, spdk_bdev_get_io_channel (older includes)
                               // Newer SPDK might have spdk_bdev_get_io_channel in spdk/bdev.h directly or spdk/io_channel.h

// Helper function to populate xsan_bdev_info_t from an spdk_bdev instance
static void _xsan_bdev_populate_info(xsan_bdev_info_t *xsan_info, struct spdk_bdev *bdev) {
    if (!xsan_info || !bdev) {
        return;
    }

    xsan_strcpy_safe(xsan_info->name, spdk_bdev_get_name(bdev), XSAN_MAX_NAME_LEN);

    const struct spdk_uuid *spdk_uuid_ptr = spdk_bdev_get_uuid(bdev);
    if (spdk_uuid_ptr && xsan_info->uuid.data) {
        spdk_uuid_copy((struct spdk_uuid *)&(xsan_info->uuid.data[0]), spdk_uuid_ptr);
    } else {
        memset(xsan_info->uuid.data, 0, sizeof(xsan_info->uuid.data));
    }

    xsan_info->num_blocks = spdk_bdev_get_num_blocks(bdev);
    xsan_info->block_size = spdk_bdev_get_block_size(bdev);
    xsan_info->capacity_bytes = xsan_info->num_blocks * xsan_info->block_size;
    xsan_strcpy_safe(xsan_info->product_name, spdk_bdev_get_product_name(bdev), XSAN_MAX_NAME_LEN);

    xsan_info->is_rotational = spdk_bdev_is_rotational(bdev);
    xsan_info->optimal_io_boundary = spdk_bdev_get_optimal_io_boundary(bdev);
    xsan_info->has_write_cache = spdk_bdev_has_write_cache(bdev);
}

xsan_error_t xsan_bdev_subsystem_init(void) {
    if (spdk_get_thread() == NULL) {
         XSAN_LOG_WARN("xsan_bdev_subsystem_init called from non-SPDK thread context! This might be ok if no SPDK calls are made here immediately.");
    }
    XSAN_LOG_INFO("XSAN bdev subsystem initialized (placeholder).");
    return XSAN_OK;
}

void xsan_bdev_subsystem_fini(void) {
    if (spdk_get_thread() == NULL) {
         XSAN_LOG_WARN("xsan_bdev_subsystem_fini called from non-SPDK thread context (might be okay if no SPDK calls made).");
    }
    XSAN_LOG_INFO("XSAN bdev subsystem finalized (placeholder).");
}

xsan_error_t xsan_bdev_list_get_all(xsan_bdev_info_t **bdev_list_out, int *count_out) {
    if (!bdev_list_out || !count_out) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_bdev_list_get_all must be called from an SPDK thread.");
        *bdev_list_out = NULL;
        *count_out = 0;
        return XSAN_ERROR_THREAD_CONTEXT;
    }

    struct spdk_bdev *bdev_iter = NULL;
    int current_bdev_count = 0;

    bdev_iter = spdk_bdev_first();
    while (bdev_iter != NULL) {
        current_bdev_count++;
        bdev_iter = spdk_bdev_next(bdev_iter);
    }

    if (current_bdev_count == 0) {
        *bdev_list_out = NULL;
        *count_out = 0;
        return XSAN_OK;
    }

    xsan_bdev_info_t *list = (xsan_bdev_info_t *)XSAN_CALLOC(current_bdev_count, sizeof(xsan_bdev_info_t));
    if (!list) {
        *bdev_list_out = NULL;
        *count_out = 0;
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    int idx = 0;
    bdev_iter = spdk_bdev_first();
    while (bdev_iter != NULL) {
        if (idx < current_bdev_count) {
            _xsan_bdev_populate_info(&list[idx], bdev_iter);
            idx++;
        } else {
            XSAN_LOG_WARN("Bdev count mismatch during population. Expected %d, got more.", current_bdev_count);
            break;
        }
        bdev_iter = spdk_bdev_next(bdev_iter);
    }

    *bdev_list_out = list;
    *count_out = idx;

    return XSAN_OK;
}

void xsan_bdev_list_free(xsan_bdev_info_t *bdev_list, int count) {
    if (!bdev_list || count <= 0) {
        return;
    }
    XSAN_FREE(bdev_list);
}

xsan_bdev_info_t *xsan_bdev_get_info_by_name(const char *bdev_name) {
    if (!bdev_name) {
        return NULL;
    }
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_bdev_get_info_by_name must be called from an SPDK thread.");
        return NULL;
    }

    struct spdk_bdev *bdev = spdk_bdev_get_by_name(bdev_name);
    if (!bdev) {
        return NULL;
    }

    xsan_bdev_info_t *info = (xsan_bdev_info_t *)XSAN_MALLOC(sizeof(xsan_bdev_info_t));
    if (!info) {
        return NULL;
    }
    memset(info, 0, sizeof(xsan_bdev_info_t));

    _xsan_bdev_populate_info(info, bdev);
    return info;
}

void xsan_bdev_info_free(xsan_bdev_info_t *bdev_info) {
    if (bdev_info) {
        XSAN_FREE(bdev_info);
    }
}

// --- DMA Memory Management Wrappers ---

void *xsan_bdev_dma_malloc(size_t size, size_t align) {
    if (size == 0) {
        XSAN_LOG_WARN("xsan_bdev_dma_malloc called with size 0.");
        return NULL;
    }
    // spdk_dma_malloc already zeros the memory if spdk_call_constructor is false (default)
    // or if the constructor is NULL.
    return spdk_dma_malloc(size, align, NULL);
}

void xsan_bdev_dma_free(void *buf) {
    if (buf) {
        spdk_dma_free(buf);
    }
}

size_t xsan_bdev_get_buf_align(const char *bdev_name) {
    if (!bdev_name) {
        XSAN_LOG_ERROR("bdev_name is NULL for xsan_bdev_get_buf_align.");
        return 0;
    }
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_bdev_get_buf_align must be called from an SPDK thread.");
        return 0;
    }
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(bdev_name);
    if (!bdev) {
        XSAN_LOG_WARN("Bdev '%s' not found for get_buf_align.", bdev_name);
        return 0;
    }
    return spdk_bdev_get_buf_align(bdev);
}

// --- Synchronous-like Read/Write Implementation ---

// Context for synchronous I/O completion
typedef struct {
    bool completed;
    int bdev_io_status; // From SPDK_BDEV_IO_STATUS_*
    struct spdk_thread *origin_thread; // Thread that initiated the I/O, for polling
} xsan_sync_io_ctx_t;

// Generic I/O completion callback for synchronous operations
static void _xsan_bdev_sync_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    xsan_sync_io_ctx_t *ctx = (xsan_sync_io_ctx_t *)cb_arg;

    ctx->bdev_io_status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
    ctx->completed = true;

    // Important: Free the spdk_bdev_io structure.
    spdk_bdev_free_io(bdev_io);

    // If we were using spdk_thread_send_msg to cross threads, we might signal here.
    // For same-thread polling, setting 'completed' is enough.
}

// Internal helper for read/write common logic
static xsan_error_t _xsan_bdev_do_sync_io(const char *bdev_name,
                                         uint64_t offset_blocks,
                                         uint32_t num_blocks,
                                         void *app_payload_buf, // User's application buffer
                                         size_t app_payload_buf_len,
                                         bool use_internal_dma_alloc,
                                         bool is_read_op) {
    if (!bdev_name || !app_payload_buf || num_blocks == 0) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    struct spdk_thread *current_thread = spdk_get_thread();
    if (current_thread == NULL) {
        XSAN_LOG_ERROR("Synchronous bdev I/O (bdev: %s) must be called from an SPDK thread.", bdev_name);
        return XSAN_ERROR_THREAD_CONTEXT;
    }

    struct spdk_bdev *bdev = spdk_bdev_get_by_name(bdev_name);
    if (!bdev) {
        XSAN_LOG_ERROR("Bdev '%s' not found for I/O operation.", bdev_name);
        return XSAN_ERROR_NOT_FOUND;
    }

    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    size_t required_io_len = (size_t)num_blocks * block_size;

    if (app_payload_buf_len < required_io_len) {
        XSAN_LOG_ERROR("User buffer (len %zu) too small for I/O on bdev '%s'. Required: %zu",
                       app_payload_buf_len, bdev_name, required_io_len);
        return XSAN_ERROR_INVALID_PARAM;
    }

    struct spdk_bdev_desc *desc = NULL;
    xsan_error_t err = XSAN_OK;
    int rc = spdk_bdev_open_ext(bdev_name, true, NULL, NULL, &desc); // writeable = true
    if (rc != 0) {
        XSAN_LOG_ERROR("Failed to open bdev '%s': %s (rc=%d)", bdev_name, spdk_strerror(-rc), rc);
        return xsan_error_from_errno(-rc); // Map SPDK error (-rc) to xsan_error
    }

    struct spdk_io_channel *ch = spdk_bdev_get_io_channel(desc);
    if (!ch) {
        XSAN_LOG_ERROR("Failed to get I/O channel for bdev '%s'", bdev_name);
        spdk_bdev_close(desc);
        return XSAN_ERROR_IO;
    }

    void *dma_buf_internal = NULL;
    void *actual_io_buf = NULL; // Buffer that will be passed to SPDK I/O function

    if (use_internal_dma_alloc) {
        size_t dma_align = spdk_bdev_get_buf_align(bdev);
        dma_buf_internal = xsan_bdev_dma_malloc(required_io_len, dma_align);
        if (!dma_buf_internal) {
            XSAN_LOG_ERROR("Failed to allocate internal DMA buffer (size %zu) for I/O on bdev '%s'", required_io_len, bdev_name);
            spdk_put_io_channel(ch); // Correct cleanup for channel
            spdk_bdev_close(desc);
            return XSAN_ERROR_OUT_OF_MEMORY;
        }
        actual_io_buf = dma_buf_internal;
        if (!is_read_op) { // For write, copy from user_buf to dma_buf_internal
            memcpy(dma_buf_internal, app_payload_buf, required_io_len);
        }
    } else {
        actual_io_buf = app_payload_buf; // User provides DMA-safe buffer
    }

    xsan_sync_io_ctx_t io_ctx = { .completed = false, .bdev_io_status = -1, .origin_thread = current_thread };

    if (is_read_op) {
        rc = spdk_bdev_read_blocks(desc, ch, actual_io_buf, offset_blocks, num_blocks, _xsan_bdev_sync_io_completion_cb, &io_ctx);
    } else { // Write operation
        rc = spdk_bdev_write_blocks(desc, ch, actual_io_buf, offset_blocks, num_blocks, _xsan_bdev_sync_io_completion_cb, &io_ctx);
    }

    if (rc != 0) {
        // -ENOMEM means SPDK couldn't allocate an spdk_bdev_io. Other errors are also possible.
        XSAN_LOG_ERROR("Failed to submit %s I/O for bdev '%s': %s (rc=%d)",
                       is_read_op ? "read" : "write", bdev_name, spdk_strerror(-rc), rc);
        err = (rc == -ENOMEM) ? XSAN_ERROR_OUT_OF_MEMORY : XSAN_ERROR_IO;
    } else {
        // Poll for completion. This is a very basic poll.
        // This function MUST be on an SPDK thread that is a poller, or it must call spdk_thread_poll().
        // If this is on a reactor thread, the reactor itself drives completions.
        // If this is on a generic SPDK app thread (not a reactor poller by default), we might need to manually poll.
        // For simplicity, let's assume this thread allows polling or is part of a reactor context.
        while (!io_ctx.completed) {
            // If this thread is not the one polling the IO channel's queue, this will hang.
            // In a typical SPDK app, this function would be a message passed to a reactor thread,
            // and that reactor thread would naturally poll its channels.
            // For a "synchronous" call on *any* SPDK app thread, we might need spdk_thread_poll.
             spdk_thread_poll(current_thread, 0, 0); // Poll current thread's reactors/messages once.
                                                    // The 0,0 arguments mean non-blocking poll of this thread only.
                                                    // This is critical if the current_thread isn't a continuously polling reactor.
        }

        if (io_ctx.bdev_io_status != SPDK_BDEV_IO_STATUS_SUCCESS) {
            XSAN_LOG_ERROR("SPDK I/O operation failed for bdev '%s' with bdev_io_status: %d", bdev_name, io_ctx.bdev_io_status);
            err = XSAN_ERROR_IO;
        } else {
            // If read and using internal DMA buffer, copy data back to user buffer
            if (is_read_op && use_internal_dma_alloc) {
                memcpy(app_payload_buf, dma_buf_internal, required_io_len);
            }
        }
    }

    if (dma_buf_internal) {
        xsan_bdev_dma_free(dma_buf_internal);
    }
    spdk_put_io_channel(ch); // Release the I/O channel
    spdk_bdev_close(desc);   // Close the bdev descriptor

    return err;
}

xsan_error_t xsan_bdev_read_sync(const char *bdev_name,
                                 uint64_t offset_blocks,
                                 uint32_t num_blocks,
                                 void *user_buf,
                                 size_t user_buf_len,
                                 bool use_internal_dma_alloc) {
    return _xsan_bdev_do_sync_io(bdev_name, offset_blocks, num_blocks, user_buf, user_buf_len, use_internal_dma_alloc, true);
}

xsan_error_t xsan_bdev_write_sync(const char *bdev_name,
                                  uint64_t offset_blocks,
                                  uint32_t num_blocks,
                                  const void *user_buf,
                                  size_t user_buf_len,
                                  bool use_internal_dma_alloc) {
    return _xsan_bdev_do_sync_io(bdev_name, offset_blocks, num_blocks, (void*)user_buf, user_buf_len, use_internal_dma_alloc, false);
}
