#include "xsan_replication.h"
#include "xsan_memory.h"
#include "xsan_log.h"
#include "xsan_storage.h" // For xsan_volume_t definition
#include "xsan_bdev.h"    // For xsan_bdev_dma_free
#include "xsan_protocol.h" // For xsan_protocol_message_destroy (if per_replica_op_ctx is freed here)

#include <string.h> // For memset

xsan_replicated_io_ctx_t *xsan_replicated_io_ctx_create(
    xsan_user_io_completion_cb_t original_user_cb,
    void *original_user_cb_arg,
    struct xsan_volume *vol,
    const void *user_buffer,
    uint64_t offset,
    uint64_t length,
    uint64_t transaction_id) {

    if (!original_user_cb || !vol) {
        XSAN_LOG_ERROR("Invalid params for replicated_io_ctx_create: cb or vol is NULL.");
        return NULL;
    }

    xsan_replicated_io_ctx_t *rep_ctx = (xsan_replicated_io_ctx_t *)XSAN_MALLOC(sizeof(xsan_replicated_io_ctx_t));
    if (!rep_ctx) {
        XSAN_LOG_ERROR("Failed to allocate memory for xsan_replicated_io_ctx_t.");
        return NULL;
    }

    memset(rep_ctx, 0, sizeof(xsan_replicated_io_ctx_t));

    memcpy(&rep_ctx->volume_id, &vol->id, sizeof(xsan_volume_id_t));
    rep_ctx->user_buffer = (void*)user_buffer; // Const cast, buffer is source for write
    rep_ctx->logical_byte_offset = offset;
    rep_ctx->length_bytes = length;
    rep_ctx->original_user_cb = original_user_cb;
    rep_ctx->original_user_cb_arg = original_user_cb_arg;
    rep_ctx->transaction_id = transaction_id;

    rep_ctx->total_replicas_targeted = vol->actual_replica_count;
    if (rep_ctx->total_replicas_targeted == 0 || rep_ctx->total_replicas_targeted > XSAN_MAX_REPLICAS) {
        XSAN_LOG_ERROR("Volume %s has invalid actual_replica_count %u for TID %lu. Cannot create rep_ctx.",
            spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]), vol->actual_replica_count, transaction_id);
        XSAN_FREE(rep_ctx);
        return NULL;
    }

    rep_ctx->successful_writes = 0;
    rep_ctx->failed_writes = 0;
    rep_ctx->final_status = XSAN_OK; // Assume OK until a failure occurs
    rep_ctx->local_io_req = NULL; // Will be set by volume_manager if local IO is submitted

    return rep_ctx;
}

void xsan_replicated_io_ctx_free(xsan_replicated_io_ctx_t *rep_io_ctx) {
    if (!rep_io_ctx) {
        return;
    }
    // Note: rep_io_ctx->local_io_req (an xsan_io_request_t*) is typically freed by the
    // xsan_io layer's completion callback (_xsan_io_spdk_completion_cb calls xsan_io_request_free).
    // If there's an error path where local_io_req was created but not submitted or completed,
    // it would need explicit freeing here or before this function is called.
    // Current design in volume_manager seems to handle this: if submit_single_io_attempt fails,
    // it calls the local_replica_write_complete_cb which sets local_io_req to NULL before
    // _xsan_check_replicated_write_completion might free rep_io_ctx.
    if (rep_io_ctx->local_io_req) {
         XSAN_LOG_WARN("rep_io_ctx (TID %lu) freed while local_io_req was still set. This might be a leak if IO not completed/freed by xsan_io layer.", rep_io_ctx->transaction_id);
         // xsan_io_request_free(rep_io_ctx->local_io_req); // Potentially add if known to be safe and necessary
    }

    XSAN_FREE(rep_io_ctx);
}

xsan_replica_read_coordinator_ctx_t *xsan_replica_read_coordinator_ctx_create(
    struct xsan_volume *vol,
    void *user_buffer,
    uint64_t offset_bytes,
    uint64_t length_bytes,
    xsan_user_io_completion_cb_t original_user_cb,
    void *original_user_cb_arg,
    uint64_t transaction_id) {

    if (!vol || !user_buffer || !original_user_cb) {
        XSAN_LOG_ERROR("Invalid params for replica_read_coordinator_ctx_create: vol, buffer, or cb is NULL.");
        return NULL;
    }

    xsan_replica_read_coordinator_ctx_t *coord_ctx =
        (xsan_replica_read_coordinator_ctx_t *)XSAN_MALLOC(sizeof(xsan_replica_read_coordinator_ctx_t));
    if (!coord_ctx) {
        XSAN_LOG_ERROR("Failed to allocate memory for xsan_replica_read_coordinator_ctx_t.");
        return NULL;
    }

    memset(coord_ctx, 0, sizeof(xsan_replica_read_coordinator_ctx_t));

    coord_ctx->vol = vol; // Non-owning pointer
    coord_ctx->user_buffer = user_buffer;
    coord_ctx->logical_byte_offset = offset_bytes;
    coord_ctx->length_bytes = length_bytes;
    coord_ctx->original_user_cb = original_user_cb;
    coord_ctx->original_user_cb_arg = original_user_cb_arg;
    coord_ctx->transaction_id = transaction_id;

    coord_ctx->current_replica_idx_to_try = 0;
    coord_ctx->last_attempt_status = XSAN_OK; // Initial assumption
    coord_ctx->internal_dma_buffer = NULL;
    coord_ctx->internal_dma_buffer_size = 0;
    coord_ctx->internal_dma_buffer_allocated = false;
    coord_ctx->current_remote_op_ctx = NULL;

    return coord_ctx;
}

void xsan_replica_read_coordinator_ctx_free(xsan_replica_read_coordinator_ctx_t *read_coord_ctx) {
    if (!read_coord_ctx) {
        return;
    }

    if (read_coord_ctx->internal_dma_buffer_allocated && read_coord_ctx->internal_dma_buffer) {
        xsan_bdev_dma_free(read_coord_ctx->internal_dma_buffer);
        read_coord_ctx->internal_dma_buffer = NULL;
    }

    if (read_coord_ctx->current_remote_op_ctx) {
        // This context might contain a message that needs freeing
        if (read_coord_ctx->current_remote_op_ctx->request_msg_to_send) {
            xsan_protocol_message_destroy(read_coord_ctx->current_remote_op_ctx->request_msg_to_send);
        }
        XSAN_FREE(read_coord_ctx->current_remote_op_ctx);
        read_coord_ctx->current_remote_op_ctx = NULL;
    }
    XSAN_FREE(read_coord_ctx);
}

// xsan_per_replica_op_ctx_t is simple enough to be malloc'd/freed directly
// in volume_manager.c where it's used. If it becomes more complex,
// dedicated create/free functions can be added here.
// For example:
/*
xsan_per_replica_op_ctx_t *xsan_per_replica_op_ctx_create(void *parent_ctx, xsan_replica_location_t *loc_info) {
    xsan_per_replica_op_ctx_t *pctx = XSAN_MALLOC(sizeof(xsan_per_replica_op_ctx_t));
    if (pctx) {
        memset(pctx, 0, sizeof(xsan_per_replica_op_ctx_t));
        pctx->parent_rep_ctx = parent_ctx;
        if (loc_info) {
            memcpy(&pctx->replica_location_info, loc_info, sizeof(xsan_replica_location_t));
        }
    }
    return pctx;
}

void xsan_per_replica_op_ctx_free(xsan_per_replica_op_ctx_t *pctx) {
    if (pctx) {
        if (pctx->request_msg_to_send) {
            xsan_protocol_message_destroy(pctx->request_msg_to_send);
        }
        XSAN_FREE(pctx);
    }
}
*/
