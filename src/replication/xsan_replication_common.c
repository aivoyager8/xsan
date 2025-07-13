#include "xsan_replication.h"
#include "../../include/xsan_storage.h" // 补充完整 struct xsan_volume 定义，消除 incomplete typedef 错误
#include "xsan_storage.h"
#include "xsan_memory.h"
#include "xsan_log.h"
#include "../../src/include/xsan_storage.h" // 确保引用完整定义

#include "spdk/uuid.h"   // For spdk_uuid_get_string, spdk_uuid_copy
#include <string.h>      // For memset, memcpy

xsan_replicated_io_ctx_t *xsan_replicated_io_ctx_create(
    xsan_user_io_completion_cb_t original_user_cb,
    void *original_user_cb_arg,
    xsan_volume_t *vol,
    const void *user_buffer,
    uint64_t offset_bytes,
    uint64_t length_bytes,
    uint64_t transaction_id) {

    if (!original_user_cb || !vol || (!user_buffer && length_bytes > 0) || length_bytes == 0) {
        XSAN_LOG_ERROR("Invalid parameters for xsan_replicated_io_ctx_create. Vol: %p, Buf: %p, Len: %lu, CB: %p",
                       (void*)vol, user_buffer, length_bytes, (void*)original_user_cb);
        return NULL;
    }
    if (vol->actual_replica_count == 0 && vol->FTT > 0) {
        // This case should ideally be prevented by volume creation logic,
        // but double check here. If FTT > 0, actual_replica_count should be > 0.
        XSAN_LOG_ERROR("Volume %s has FTT %u but actual_replica_count is 0. Cannot create replicated IO context.",
                       vol->name, vol->FTT);
        return NULL;
    }


    xsan_replicated_io_ctx_t *rep_ctx =
        (xsan_replicated_io_ctx_t *)XSAN_MALLOC(sizeof(xsan_replicated_io_ctx_t));
    if (!rep_ctx) {
        XSAN_LOG_ERROR("Failed to allocate xsan_replicated_io_ctx_t.");
        return NULL;
    }

    memset(rep_ctx, 0, sizeof(xsan_replicated_io_ctx_t));

    // Copy essential information from the original request and volume
    memcpy(&rep_ctx->volume_id, &vol->id, sizeof(xsan_volume_id_t));
    rep_ctx->user_buffer = (void*)user_buffer; // Store user buffer (cast away const for internal flexibility if needed)
    rep_ctx->logical_byte_offset = offset_bytes;
    rep_ctx->length_bytes = length_bytes;
    // rep_ctx->logical_block_size = vol->block_size_bytes; // Store if needed for calculations within callbacks

    rep_ctx->original_user_cb = original_user_cb;
    rep_ctx->original_user_cb_arg = original_user_cb_arg;

    // Determine how many replicas we are actually targeting for this operation.
    // If FTT=0, replica_count is 1 (local only). If FTT=1, replica_count is 2.
    rep_ctx->total_replicas_targeted = vol->FTT + 1;
    // However, ensure we don't target more than actual_replica_count or XSAN_MAX_REPLICAS
    if (rep_ctx->total_replicas_targeted > vol->actual_replica_count) {
        XSAN_LOG_WARN("Volume %s: FTT+1 (%u) > actual_replica_count (%u). Targeting %u replicas.",
                      vol->name, vol->FTT + 1, vol->actual_replica_count, vol->actual_replica_count);
        rep_ctx->total_replicas_targeted = vol->actual_replica_count;
    }
    if (rep_ctx->total_replicas_targeted > XSAN_MAX_REPLICAS) {
         XSAN_LOG_WARN("Volume %s: FTT+1 (%u) > XSAN_MAX_REPLICAS (%d). Capping at max.",
                      vol->name, vol->FTT + 1, XSAN_MAX_REPLICAS);
        rep_ctx->total_replicas_targeted = XSAN_MAX_REPLICAS;
    }
    if (rep_ctx->total_replicas_targeted == 0 && length_bytes > 0) { // Should not happen if vol->actual_replica_count is sane
        XSAN_LOG_ERROR("Volume %s has 0 targetable replicas for IO. FTT=%u, Actual=%u", vol->name, vol->FTT, vol->actual_replica_count);
        XSAN_FREE(rep_ctx);
        return NULL;
    }


    rep_ctx->successful_writes = 0;
    rep_ctx->failed_writes = 0;
    rep_ctx->final_status = XSAN_OK; // Initial optimistic status
    rep_ctx->transaction_id = transaction_id;

    rep_ctx->local_io_req = NULL;

    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&rep_ctx->volume_id.data[0]);
    XSAN_LOG_DEBUG("Replicated IO Ctx created for VolID %s, TID %lu, %u replicas targeted",
                   vol_id_str, rep_ctx->transaction_id, rep_ctx->total_replicas_targeted);

    return rep_ctx;
}

void xsan_replicated_io_ctx_free(xsan_replicated_io_ctx_t *rep_io_ctx) {
    if (!rep_io_ctx) {
        return;
    }
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&rep_io_ctx->volume_id.data[0]);
    XSAN_LOG_DEBUG("Freeing Replicated IO Ctx for VolID %s, TID %lu",
                   vol_id_str, rep_io_ctx->transaction_id);

    // The local_io_req should have been freed by its own completion callback (_xsan_io_spdk_completion_cb)
    // if it was successfully submitted. If rep_io_ctx is freed due to an error *before*
    // local_io_req was submitted or if local_io_req itself was never created properly,
    // local_io_req might be NULL or point to memory that needs freeing.
    // The current design is that xsan_io_request_free is called by the SPDK completion callback.
    // If xsan_volume_write_async fails before submitting local_io_req, it should free it there.
    if (rep_io_ctx->local_io_req) {
        XSAN_LOG_WARN("Replicated IO Ctx (Vol: %s, TID: %lu) being freed with non-NULL local_io_req. This might indicate an earlier error path where local_io_req was not submitted and thus not auto-freed by its callback. Freeing it now.", vol_id_str, rep_io_ctx->transaction_id);
        xsan_io_request_free(rep_io_ctx->local_io_req);
        rep_io_ctx->local_io_req = NULL;
    }

    // TODO: If remote_sends array (or similar) is added and contains allocated resources, free them here.

    XSAN_FREE(rep_io_ctx);
}
