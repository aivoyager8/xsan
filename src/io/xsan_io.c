#include "xsan_io.h"
#include "xsan_bdev.h"         // For xsan_bdev_dma_malloc/free, xsan_bdev_get_buf_align
#include "xsan_memory.h"       // For XSAN_MALLOC, XSAN_FREE
#include "../../include/xsan_error.h"
#include "xsan_log.h"
#include "xsan_string_utils.h" // For xsan_strcpy_safe

#include "spdk/bdev.h"
#include "spdk/env.h"          // For spdk_dma_malloc/free (though wrapped)
#include "spdk/thread.h"       // For spdk_get_thread, spdk_io_channel related if not via bdev_desc
#include "spdk/bdev_module.h"  // For spdk_bdev_open_ext, spdk_bdev_close

xsan_io_request_t *xsan_io_request_create(
    xsan_volume_id_t target_volume_id,
    void *user_buffer,
    uint64_t offset_bytes,
    uint64_t length_bytes,
    uint32_t block_size_bytes, // Logical block size for this IO
    bool is_read,
    xsan_user_io_completion_cb_t user_cb,
    void *user_cb_arg) {

    if (!user_buffer || length_bytes == 0 || block_size_bytes == 0 || !user_cb) {
        XSAN_LOG_ERROR("Invalid parameters for xsan_io_request_create.");
        return NULL;
    }
    if ((offset_bytes % block_size_bytes != 0) || (length_bytes % block_size_bytes != 0)) {
        XSAN_LOG_ERROR("Offset (%lu) or length (%lu) not aligned to block size (%u).",
                       offset_bytes, length_bytes, block_size_bytes);
        // For simplicity, initial version requires alignment.
        // Future: Could handle partial block reads/writes by reading full blocks internally.
        return NULL;
    }

    xsan_io_request_t *io_req = (xsan_io_request_t *)XSAN_MALLOC(sizeof(xsan_io_request_t));
    if (!io_req) {
        XSAN_LOG_ERROR("Failed to allocate memory for xsan_io_request_t.");
        return NULL;
    }

    memset(io_req, 0, sizeof(xsan_io_request_t));

    memcpy(&io_req->target_volume_id, &target_volume_id, sizeof(xsan_volume_id_t));
    // target_disk_id and target_bdev_name will be populated by the volume manager before submission.

    io_req->is_read_op = is_read;
    io_req->user_buffer = user_buffer;
    io_req->offset_bytes = offset_bytes;
    io_req->length_bytes = length_bytes;

    io_req->block_size_bytes = block_size_bytes;
    io_req->offset_blocks = offset_bytes / block_size_bytes;
    io_req->num_blocks = length_bytes / block_size_bytes;

    io_req->user_cb = user_cb;
    io_req->user_cb_arg = user_cb_arg;

    io_req->status = XSAN_OK; // Initial status
    io_req->dma_buffer = NULL;
    io_req->dma_buffer_is_internal = false;
    io_req->dma_buffer_size = 0;
    io_req->bdev_desc = NULL;
    io_req->io_channel = NULL;
    io_req->own_spdk_resources = false; // Default to not owning, submitter will manage or set this

    return io_req;
}

void xsan_io_request_free(xsan_io_request_t *io_req) {
    if (!io_req) {
        return;
    }
    if (io_req->dma_buffer_is_internal && io_req->dma_buffer) {
        xsan_bdev_dma_free(io_req->dma_buffer);
    }
    // If own_spdk_resources was true, desc and channel should have been released in completion cb
    // or before freeing the request if submission failed.
    XSAN_FREE(io_req);
}

// Static SPDK I/O completion callback
static void _xsan_io_spdk_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    xsan_io_request_t *io_req = (xsan_io_request_t *)cb_arg;

    if (!io_req) {
        XSAN_LOG_ERROR("SPDK completion callback received NULL cb_arg (xsan_io_request_t).");
        if (bdev_io) spdk_bdev_free_io(bdev_io);
        return;
    }

    XSAN_LOG_DEBUG("SPDK IO completed for bdev '%s', op: %s, success: %d",
                   io_req->target_bdev_name, io_req->is_read_op ? "READ" : "WRITE", success);

    io_req->status = success ? XSAN_OK : XSAN_ERROR_IO;

    if (success && io_req->is_read_op && io_req->dma_buffer_is_internal && io_req->dma_buffer) {
        if (io_req->user_buffer) {
            // Assuming length_bytes in io_req is the actual amount to copy
            memcpy(io_req->user_buffer, io_req->dma_buffer, io_req->length_bytes);
        } else {
            XSAN_LOG_ERROR("Internal DMA read successful for bdev '%s' but user_buffer is NULL in io_req.", io_req->target_bdev_name);
            io_req->status = XSAN_ERROR_INVALID_PARAM; // Or internal error
        }
    }

    spdk_bdev_free_io(bdev_io); // Always free the SPDK bdev_io

    // Release SPDK resources if this IO request "owns" them
    if (io_req->own_spdk_resources) {
        if (io_req->io_channel) {
            spdk_put_io_channel(io_req->io_channel);
            io_req->io_channel = NULL;
        }
        if (io_req->bdev_desc) {
            spdk_bdev_close(io_req->bdev_desc);
            io_req->bdev_desc = NULL;
        }
    }

    // Call the user's completion callback
    if (io_req->user_cb) {
        io_req->user_cb(io_req->user_cb_arg, io_req->status);
    }

    // Free the xsan_io_request_t itself
    xsan_io_request_free(io_req);
}

xsan_error_t xsan_io_submit_request_to_bdev(xsan_io_request_t *io_req) {
    if (!io_req || !io_req->user_cb || !io_req->target_bdev_name[0] || io_req->num_blocks == 0) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_io_submit_request_to_bdev must be called from an SPDK thread.");
        return XSAN_ERROR_THREAD_CONTEXT;
    }

    xsan_error_t err = XSAN_OK;
    struct spdk_bdev *bdev = NULL;
    void *payload_buffer_for_spdk; // This will be the DMA safe buffer

    // Step 1: Get SPDK bdev handle
    bdev = spdk_bdev_get_by_name(io_req->target_bdev_name);
    if (!bdev) {
        XSAN_LOG_ERROR("Bdev '%s' not found for IO submission.", io_req->target_bdev_name);
        return XSAN_ERROR_NOT_FOUND;
    }

    // Step 2: Open bdev descriptor and get IO channel (if not already provided in io_req)
    // For this implementation, we open/close desc and get/put channel per request.
    // This can be optimized later by caching them (e.g., in xsan_disk_t or per-thread).
    if (!io_req->bdev_desc) {
        int rc_open = spdk_bdev_open_ext(io_req->target_bdev_name, true, NULL, NULL, &io_req->bdev_desc);
        if (rc_open != 0) {
            XSAN_LOG_ERROR("Failed to open bdev '%s': %s (rc=%d)", io_req->target_bdev_name, spdk_strerror(-rc_open), rc_open);
            return xsan_error_from_errno(-rc_open);
        }
        io_req->own_spdk_resources = true; // This request is responsible for closing desc
    }

    if (!io_req->io_channel) {
        io_req->io_channel = spdk_bdev_get_io_channel(io_req->bdev_desc);
        if (!io_req->io_channel) {
            XSAN_LOG_ERROR("Failed to get I/O channel for bdev '%s'", io_req->target_bdev_name);
            if (io_req->own_spdk_resources && io_req->bdev_desc) { // Clean up desc if we opened it
                spdk_bdev_close(io_req->bdev_desc);
                io_req->bdev_desc = NULL;
            }
            return XSAN_ERROR_IO;
        }
        // Note: If we got the channel, own_spdk_resources should also cover putting it back.
    }


    // Step 3: Prepare DMA buffer if needed
    uint32_t physical_bdev_block_size = spdk_bdev_get_block_size(bdev);
    size_t physical_io_size = (size_t)io_req->num_blocks * physical_bdev_block_size;
    // This assumes num_blocks in io_req is in terms of physical_bdev_block_size.
    // If io_req->block_size_bytes is different, num_blocks and offset_blocks need conversion
    // *before* this function, or this function needs to handle it.
    // For now, assume io_req->num_blocks and io_req->offset_blocks are already in terms of physical_bdev_block_size.
    // This means io_req->length_bytes should equal physical_io_size.
    if(io_req->length_bytes != physical_io_size) {
        XSAN_LOG_ERROR("Mismatch: io_req length %lu != calculated physical IO size %zu for bdev %s. Ensure num_blocks is based on physical block size.",
                        io_req->length_bytes, physical_io_size, io_req->target_bdev_name);
        // Clean up resources if owned
        if (io_req->own_spdk_resources) {
            if (io_req->io_channel) spdk_put_io_channel(io_req->io_channel);
            if (io_req->bdev_desc) spdk_bdev_close(io_req->bdev_desc);
        }
        return XSAN_ERROR_INVALID_PARAM;
    }


    // For simplicity, assume we always use an internal DMA buffer for now if not provided one.
    // A more advanced check would be: if (spdk_vtophys(io_req->user_buffer) == SPDK_VTOPHYS_ERROR || alignment_check_fails)
    // For now, let's use the 'dma_buffer_is_internal' field as a hint if caller wants internal alloc.
    // If io_req->dma_buffer is already set and dma_buffer_is_internal is false, use it.
    // Otherwise, allocate.

    if (io_req->dma_buffer && !io_req->dma_buffer_is_internal) { // User provided DMA buffer
        payload_buffer_for_spdk = io_req->dma_buffer;
    } else { // Need to allocate internal DMA buffer
        size_t bdev_align = spdk_bdev_get_buf_align(bdev);
        io_req->dma_buffer = xsan_bdev_dma_malloc(physical_io_size, bdev_align);
        if (!io_req->dma_buffer) {
            XSAN_LOG_ERROR("Failed to allocate DMA buffer (size %zu) for IO on bdev '%s'", physical_io_size, io_req->target_bdev_name);
            if (io_req->own_spdk_resources) {
                if (io_req->io_channel) spdk_put_io_channel(io_req->io_channel);
                if (io_req->bdev_desc) spdk_bdev_close(io_req->bdev_desc);
            }
            return XSAN_ERROR_NO_MEMORY;
        }
        io_req->dma_buffer_is_internal = true;
        io_req->dma_buffer_size = physical_io_size;
        payload_buffer_for_spdk = io_req->dma_buffer;

        if (!io_req->is_read_op) { // For write, copy user data to internal DMA buffer
            if (io_req->user_buffer) { // Should always be true for write
                memcpy(payload_buffer_for_spdk, io_req->user_buffer, io_req->length_bytes);
            } else {
                 XSAN_LOG_ERROR("User buffer is NULL for write operation with internal DMA buffer. Bdev: %s", io_req->target_bdev_name);
                 // Cleanup and error (as above)
                if (io_req->own_spdk_resources) {
                    if (io_req->io_channel) spdk_put_io_channel(io_req->io_channel);
                    if (io_req->bdev_desc) spdk_bdev_close(io_req->bdev_desc);
                }
                xsan_bdev_dma_free(io_req->dma_buffer); // Free the one we just allocated
                io_req->dma_buffer = NULL;
                return XSAN_ERROR_INVALID_PARAM;
            }
        }
    }

    // Step 4: Submit asynchronous I/O to SPDK
    int spdk_rc;
    if (io_req->is_read_op) {
        spdk_rc = spdk_bdev_read_blocks(io_req->bdev_desc, io_req->io_channel, payload_buffer_for_spdk,
                                        io_req->offset_blocks, io_req->num_blocks,
                                        _xsan_io_spdk_completion_cb, io_req);
    } else {
        spdk_rc = spdk_bdev_write_blocks(io_req->bdev_desc, io_req->io_channel, payload_buffer_for_spdk,
                                         io_req->offset_blocks, io_req->num_blocks,
                                         _xsan_io_spdk_completion_cb, io_req);
    }

    // Step 5: Handle SPDK submission result
    if (spdk_rc != 0) {
        XSAN_LOG_ERROR("Failed to submit SPDK %s request for bdev '%s': %s (rc=%d)",
                       io_req->is_read_op ? "read" : "write", io_req->target_bdev_name, spdk_strerror(-spdk_rc), spdk_rc);
        err = (spdk_rc == -ENOMEM) ? XSAN_ERROR_NO_MEMORY : XSAN_ERROR_IO;

        // Critical: If submission failed, the SPDK completion callback will NOT be called.
        // So, we must clean up resources here and then call the user's callback with error, then free io_req.
        if (io_req->dma_buffer_is_internal && io_req->dma_buffer) {
            xsan_bdev_dma_free(io_req->dma_buffer);
            io_req->dma_buffer = NULL;
        }
        if (io_req->own_spdk_resources) {
            if (io_req->io_channel) spdk_put_io_channel(io_req->io_channel);
            if (io_req->bdev_desc) spdk_bdev_close(io_req->bdev_desc);
        }
        // Set status before calling user callback
        io_req->status = err;
        if(io_req->user_cb) io_req->user_cb(io_req->user_cb_arg, io_req->status);
        xsan_io_request_free(io_req); // Free the request itself
        return err; // Return error to the submitter (e.g. volume manager)
    }

    // If spdk_rc == 0, submission was successful. SPDK completion callback will handle cleanup.
    return XSAN_OK;
}

// CMakeLists.txt in src/ (or new src/io/) will need to add xsan_io.c
// And src/main/CMakeLists.txt will link the new xsan_io library.
