#include "xsan_log.h"
#include "xsan_config.h"
#include "xsan_spdk_manager.h"
#include "xsan_bdev.h"
#include "xsan_disk_manager.h"
#include "xsan_volume_manager.h"
#include "xsan_io.h"
#include "xsan_node_comm.h"
#include "xsan_error.h"
#include "xsan_string_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "spdk/uuid.h"
#include "spdk/env.h"
#include "spdk/sock.h"
#include "spdk/thread.h"

// --- Async I/O Test Context and Callbacks ---
typedef struct {
    xsan_volume_manager_t *vm;
    xsan_disk_manager_t *dm;
    xsan_volume_id_t volume_id_to_test;
    char target_bdev_name_for_log[XSAN_MAX_NAME_LEN];
    uint32_t io_block_size;
    size_t dma_alignment;
    void *write_buffer_dma;
    void *read_buffer_dma;
    enum { ASYNC_IO_TEST_IDLE, ASYNC_IO_TEST_WRITE_SUBMITTED, ASYNC_IO_TEST_READ_SUBMITTED,
           ASYNC_IO_TEST_VERIFY_DONE, ASYNC_IO_TEST_FAILED } test_state;
    int outstanding_io_ops;
    bool test_finished_signal;
} xsan_async_io_test_control_t;
static xsan_async_io_test_control_t g_async_io_test_controller;

// --- Node Communication Test Callbacks & State ---
typedef struct {
    struct spdk_sock *client_sock_to_self; // Sock used by client side of self-connect
    bool connected_to_self;
    bool ping_sent_successfully;
    bool ping_response_received; // Changed from pong_received
    bool test_finished_signal;
} xsan_comm_test_state_t;
static xsan_comm_test_state_t g_comm_test_controller;

// --- Replica Write Request Handling Context (for message handler) ---
typedef struct {
    struct spdk_sock *originating_sock;
    xsan_volume_manager_t *vm;
    xsan_message_header_t original_req_header;
    xsan_replica_write_req_payload_t original_req_struct_payload;
    void *data_to_write_dma; // DMA buffer for the local write on replica side
    uint32_t data_len;       // Length of data_to_write_dma
} xsan_replica_request_handler_ctx_t;


static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl);
static void _finalize_async_io_test(xsan_async_io_test_control_t *test_ctrl);
static void _replica_response_send_complete_cb(int status, void *cb_arg);
static void _replica_request_local_write_done_cb(void *cb_arg, xsan_error_t local_write_status);


static void _async_io_test_read_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *test_ctrl = (xsan_async_io_test_control_t *)cb_arg;
    test_ctrl->outstanding_io_ops--;
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    if (status != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncIOTest] Read from volume %s FAILED: %s (code %d)", vol_id_str, xsan_error_string(status), status);
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
    } else {
        XSAN_LOG_INFO("[AsyncIOTest] Read from volume %s successful.", vol_id_str);
        if (memcmp(test_ctrl->write_buffer_dma, test_ctrl->read_buffer_dma, test_ctrl->io_block_size) == 0) {
            XSAN_LOG_INFO("SUCCESS: [AsyncIOTest] R/W data verification for volume %s PASSED!", vol_id_str);
            test_ctrl->test_state = ASYNC_IO_TEST_VERIFY_DONE;
        } else {
            XSAN_LOG_ERROR("FAILURE: [AsyncIOTest] R/W data verification for volume %s FAILED!", vol_id_str);
            test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        }
    }
    _finalize_async_io_test(test_ctrl);
}

static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl) {
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    XSAN_LOG_DEBUG("[AsyncIOTest] Submitting async read for volume %s...", vol_id_str);
    test_ctrl->test_state = ASYNC_IO_TEST_READ_SUBMITTED; test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_read_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->read_buffer_dma, _async_io_test_read_complete_cb, test_ctrl);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncIOTest] Failed to submit async read for vol %s: %s", vol_id_str, xsan_error_string(err));
        test_ctrl->outstanding_io_ops--; test_ctrl->test_state = ASYNC_IO_TEST_FAILED; _finalize_async_io_test(test_ctrl);
    }
}

static void _async_io_test_write_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *test_ctrl = (xsan_async_io_test_control_t *)cb_arg;
    test_ctrl->outstanding_io_ops--; // This is for the *overall* replicated write
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);

    if (status != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncIOTest] Replicated write to volume %s FAILED: %s (code %d)", vol_id_str, xsan_error_string(status), status);
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_async_io_test(test_ctrl); // Finalize test as replication failed
    } else {
        XSAN_LOG_INFO("[AsyncIOTest] Replicated write to volume %s reported overall SUCCESS. Proceeding to read back for verification...", vol_id_str);
        // Now that replicated write is "successful", initiate a read to verify content.
        // The read will only go to the local/primary replica for now.
        _run_async_io_test_read_phase(test_ctrl);
    }
}

static void _start_async_io_test_on_volume(xsan_async_io_test_control_t *test_ctrl, xsan_volume_t *vol_to_test) {
    if (!vol_to_test || vol_to_test->block_size_bytes == 0 || vol_to_test->num_blocks == 0) {
        XSAN_LOG_WARN("[AsyncIOTest] Invalid volume for test. Skipping.");
        _finalize_async_io_test(test_ctrl); return;
    }
    memcpy(&test_ctrl->volume_id_to_test, &vol_to_test->id, sizeof(xsan_volume_id_t));
    test_ctrl->io_block_size = vol_to_test->block_size_bytes;
    xsan_disk_group_t *group = xsan_disk_manager_find_disk_group_by_id(test_ctrl->dm, vol_to_test->source_group_id);
    if (!group || group->disk_count == 0) { XSAN_LOG_ERROR("[AsyncIOTest] Vol group invalid. Skipping."); _finalize_async_io_test(test_ctrl); return; }
    xsan_disk_t *first_disk_in_group = xsan_disk_manager_find_disk_by_id(test_ctrl->dm, group->disk_ids[0]);
    if (!first_disk_in_group) { XSAN_LOG_ERROR("[AsyncIOTest] Vol disk invalid. Skipping."); _finalize_async_io_test(test_ctrl); return; }

    xsan_strcpy_safe(test_ctrl->target_bdev_name_for_log, first_disk_in_group->bdev_name, XSAN_MAX_NAME_LEN);
    test_ctrl->dma_alignment = xsan_bdev_get_buf_align(first_disk_in_group->bdev_name);
    test_ctrl->write_buffer_dma = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);
    test_ctrl->read_buffer_dma = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);

    if (!test_ctrl->write_buffer_dma || !test_ctrl->read_buffer_dma) {
        XSAN_LOG_ERROR("[AsyncIOTest] DMA alloc failed. Skipping.");
        if(test_ctrl->write_buffer_dma) xsan_bdev_dma_free(test_ctrl->write_buffer_dma);
        if(test_ctrl->read_buffer_dma) xsan_bdev_dma_free(test_ctrl->read_buffer_dma);
        test_ctrl->write_buffer_dma = NULL; test_ctrl->read_buffer_dma = NULL;
        _finalize_async_io_test(test_ctrl); return;
    }

    char vol_id_str[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    snprintf((char*)test_ctrl->write_buffer_dma, test_ctrl->io_block_size, "XSAN Async Test! Vol %s", vol_id_str);
    for(size_t k=strlen((char*)test_ctrl->write_buffer_dma); k < test_ctrl->io_block_size; ++k) { ((char*)test_ctrl->write_buffer_dma)[k] = (char)((k % 250) + 5); }
    memset(test_ctrl->read_buffer_dma, 0xDD, test_ctrl->io_block_size);
    XSAN_LOG_INFO("--- Starting Async R/W Test on Volume ID %s (bdev: '%s', io_block_size: %u, FTT: %u) ---",
                  vol_id_str, test_ctrl->target_bdev_name_for_log, test_ctrl->io_block_size, vol_to_test->FTT);

    test_ctrl->test_state = ASYNC_IO_TEST_WRITE_SUBMITTED; test_ctrl->outstanding_io_ops++;
    // This write will now trigger the replicated write path if FTT > 0 for vol_to_test
    xsan_error_t err = xsan_volume_write_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->write_buffer_dma, _async_io_test_write_complete_cb, test_ctrl);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncIOTest] Failed to submit async (replicated) write for vol %s: %s", vol_id_str, xsan_error_string(err));
        test_ctrl->outstanding_io_ops--; test_ctrl->test_state = ASYNC_IO_TEST_FAILED; _finalize_async_io_test(test_ctrl);
    }
}

static void _finalize_async_io_test(xsan_async_io_test_control_t *test_ctrl) {
    if (test_ctrl->outstanding_io_ops == 0) {
        char vol_id_str[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
        XSAN_LOG_INFO("[AsyncIOTest] Sequence finished for volume %s, state: %d.", vol_id_str, test_ctrl->test_state);
        if(test_ctrl->write_buffer_dma) xsan_bdev_dma_free(test_ctrl->write_buffer_dma); test_ctrl->write_buffer_dma = NULL;
        if(test_ctrl->read_buffer_dma) xsan_bdev_dma_free(test_ctrl->read_buffer_dma); test_ctrl->read_buffer_dma = NULL;
        test_ctrl->test_finished_signal = true;
    }
}

// --- Node Comm Test Callbacks & State ---
static void _comm_test_send_ping_cb(int status, void *cb_arg) { /* ... as before ... */
    xsan_comm_test_state_t *cs = (xsan_comm_test_state_t *)cb_arg;
    if(status==0) cs->ping_sent_successfully = true; else cs->test_finished_signal = true;
}
static void _comm_test_connect_cb(struct spdk_sock *sock, int status, void *cb_arg) { /* ... as before ... */
    xsan_comm_test_state_t *cs = (xsan_comm_test_state_t *)cb_arg;
    if(status==0){ cs->client_sock_to_self = sock; cs->connected_to_self = true;
        xsan_message_t *m = xsan_protocol_message_create(XSAN_MSG_TYPE_HEARTBEAT, 1, "SELF_PING", 10);
        if(m){ if(xsan_node_comm_send_msg(sock,m,_comm_test_send_ping_cb,cs)!=XSAN_OK) cs->test_finished_signal=true; xsan_protocol_message_destroy(m); }
        else cs->test_finished_signal=true;
    } else cs->test_finished_signal=true;
}

// --- Replica Write Request Handling (Simulated on self) ---
static void _replica_response_send_complete_cb(int status, void *cb_arg) {
    xsan_replica_request_handler_ctx_t *handler_ctx = (xsan_replica_request_handler_ctx_t *)cb_arg;
    if (!handler_ctx) return;
    XSAN_LOG_DEBUG("[ReplicaHandler] REPLICA_WRITE_BLOCK_RESP for TID %lu (to sock %p) send status: %d",
                   handler_ctx->original_req_header.transaction_id, (void*)handler_ctx->originating_sock, status);
    if(handler_ctx->data_to_write_dma) xsan_bdev_dma_free(handler_ctx->data_to_write_dma);
    XSAN_FREE(handler_ctx);
}

static void _replica_request_local_write_done_cb(void *cb_arg, xsan_error_t local_write_status) {
    xsan_replica_request_handler_ctx_t *handler_ctx = (xsan_replica_request_handler_ctx_t *)cb_arg;
    if (!handler_ctx) { XSAN_LOG_ERROR("NULL handler_ctx in _replica_request_local_write_done_cb"); return; }

    char vol_id_s[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_s, sizeof(vol_id_s), (struct spdk_uuid*)&handler_ctx->original_req_struct_payload.volume_id.data[0]);
    XSAN_LOG_INFO("[ReplicaHandler] Local write for REPLICA_WRITE_BLOCK_REQ (Vol: %s, LBA: %lu, TID: %lu) completed with status: %d",
                  vol_id_s, handler_ctx->original_req_struct_payload.block_lba_on_volume,
                  handler_ctx->original_req_header.transaction_id, local_write_status);

    xsan_replica_write_resp_payload_t resp_payload;
    memset(&resp_payload, 0, sizeof(resp_payload));
    resp_payload.status = local_write_status;
    resp_payload.block_lba_on_volume = handler_ctx->original_req_struct_payload.block_lba_on_volume;
    resp_payload.num_blocks_processed = (local_write_status == XSAN_OK) ? handler_ctx->original_req_struct_payload.num_blocks : 0;

    xsan_message_t *resp_msg = xsan_protocol_message_create(
        XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP,
        handler_ctx->original_req_header.transaction_id,
        &resp_payload,
        sizeof(xsan_replica_write_resp_payload_t));

    if (resp_msg) {
        XSAN_LOG_DEBUG("[ReplicaHandler] Sending REPLICA_WRITE_BLOCK_RESP (status %d) for TID %lu back to sock %p",
                       local_write_status, handler_ctx->original_req_header.transaction_id,
                       (void*)handler_ctx->originating_sock);

        // Pass handler_ctx to send_cb so it can be freed along with DMA buffer
        // NOTE: If originating_sock is already closed by the time we try to send, this will fail.
        // Robust handling needs to check socket validity or use a connection manager.
        xsan_error_t send_err = xsan_node_comm_send_msg(handler_ctx->originating_sock, resp_msg,
                                                        _replica_response_send_complete_cb, handler_ctx); // handler_ctx freed by cb
        if (send_err != XSAN_OK) {
            XSAN_LOG_ERROR("[ReplicaHandler] Failed to send REPLICA_WRITE_BLOCK_RESP for TID %lu: %s",
                           handler_ctx->original_req_header.transaction_id, xsan_error_string(send_err));
            if(handler_ctx->data_to_write_dma) xsan_bdev_dma_free(handler_ctx->data_to_write_dma);
            XSAN_FREE(handler_ctx);
        }
        xsan_protocol_message_destroy(resp_msg);
    } else {
        XSAN_LOG_ERROR("[ReplicaHandler] Failed to create REPLICA_WRITE_BLOCK_RESP for TID %lu", handler_ctx->original_req_header.transaction_id);
        if(handler_ctx->data_to_write_dma) xsan_bdev_dma_free(handler_ctx->data_to_write_dma);
        XSAN_FREE(handler_ctx);
    }
}

// Main message handler for the node
static void _xsan_node_test_message_handler(struct spdk_sock *sock,
                                            const char *peer_addr_str,
                                            xsan_message_t *msg,
                                            void *cb_arg) {
    xsan_volume_manager_t *vm = (xsan_volume_manager_t *)cb_arg; // cb_arg is vm instance
    if (!vm) { XSAN_LOG_ERROR("Msg handler called with NULL vm context!"); xsan_protocol_message_destroy(msg); return;}

    XSAN_LOG_INFO("[TestMsgHandler] Received msg from %s: Type %u, PayloadLen %u, TID %lu",
                  peer_addr_str, msg->header.type, msg->header.payload_length, msg->header.transaction_id);

    if (msg->header.type == XSAN_MSG_TYPE_HEARTBEAT && msg->payload && strcmp((char*)msg->payload, "SELF_PING_PAYLOAD") == 0) {
        XSAN_LOG_INFO("[CommTest] PING (HEARTBEAT) received by handler from self!");
        g_comm_test_controller.pong_received_or_ping_handled = true;
        if (!g_comm_test_controller.test_finished_signal) {
            g_comm_test_controller.test_finished_signal = true;
        }
    } else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ) {
        if (!msg->payload || msg->header.payload_length < XSAN_REPLICA_WRITE_REQ_PAYLOAD_SIZE) {
            XSAN_LOG_ERROR("[ReplicaHandler] Invalid REPLICA_WRITE_BLOCK_REQ payload size from %s.", peer_addr_str);
            goto msg_destroy_exit;
        }
        xsan_replica_write_req_payload_t *req_struct = (xsan_replica_write_req_payload_t *)msg->payload;
        unsigned char *block_data_ptr = msg->payload + XSAN_REPLICA_WRITE_REQ_PAYLOAD_SIZE;
        uint32_t block_data_actual_len = msg->header.payload_length - XSAN_REPLICA_WRITE_REQ_PAYLOAD_SIZE;

        char vol_id_s[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(vol_id_s, sizeof(vol_id_s), (struct spdk_uuid*)&req_struct->volume_id.data[0]);
        XSAN_LOG_INFO("[ReplicaHandler] Processing REPLICA_WRITE_BLOCK_REQ for Vol: %s, LBA: %lu, NumBlocks: %u, DataLen: %u from %s (TID %lu)",
                      vol_id_s, req_struct->block_lba_on_volume, req_struct->num_blocks, block_data_actual_len, peer_addr_str, msg->header.transaction_id);

        xsan_volume_t *target_vol = xsan_volume_get_by_id(vm, req_struct->volume_id);
        if (!target_vol) { XSAN_LOG_ERROR("[ReplicaHandler] Target volume %s not found.", vol_id_s); goto msg_destroy_exit; }

        uint64_t expected_data_len = (uint64_t)req_struct->num_blocks * target_vol->block_size_bytes;
        if (block_data_actual_len != expected_data_len) {
             XSAN_LOG_ERROR("[ReplicaHandler] Data length %u mismatch for vol %s. Expected %lu. Corrupted?", block_data_actual_len, vol_id_s, expected_data_len);
             goto msg_destroy_exit;
        }

        xsan_replica_request_handler_ctx_t *handler_ctx = (xsan_replica_request_handler_ctx_t*)XSAN_MALLOC(sizeof(xsan_replica_request_handler_ctx_t));
        if (!handler_ctx) { XSAN_LOG_ERROR("[ReplicaHandler] Failed to MALLOC handler_ctx."); goto msg_destroy_exit; }

        handler_ctx->originating_sock = sock;
        handler_ctx->vm = vm;
        memcpy(&handler_ctx->original_req_header, &msg->header, sizeof(xsan_message_header_t));
        memcpy(&handler_ctx->original_req_struct_payload, req_struct, sizeof(xsan_replica_write_req_payload_t));
        handler_ctx->data_len = block_data_actual_len;

        size_t bdev_align = 4096; // Default, should get from actual target bdev
        xsan_disk_id_t mapped_disk_id; uint64_t mapped_phys_lba; uint32_t phys_bs;
        if (xsan_volume_map_lba_to_physical(vm, req_struct->volume_id, req_struct->block_lba_on_volume, &mapped_disk_id, &mapped_phys_lba, &phys_bs) == XSAN_OK) {
            xsan_disk_t* disk = xsan_disk_manager_find_disk_by_id(g_async_io_test_controller.dm, mapped_disk_id);
            if (disk) bdev_align = xsan_bdev_get_buf_align(disk->bdev_name);
        }

        handler_ctx->data_to_write_dma = xsan_bdev_dma_malloc(block_data_actual_len, bdev_align);
        if (!handler_ctx->data_to_write_dma) { XSAN_LOG_ERROR("[ReplicaHandler] Failed DMA alloc for replica write."); XSAN_FREE(handler_ctx); goto msg_destroy_exit; }
        memcpy(handler_ctx->data_to_write_dma, block_data_ptr, block_data_actual_len);

        XSAN_LOG_DEBUG("[ReplicaHandler] Submitting local async write for replica data (Vol: %s, LBA: %lu)", vol_id_s, req_struct->block_lba_on_volume);
        xsan_error_t local_write_err = xsan_volume_write_async(
            vm, req_struct->volume_id,
            req_struct->block_lba_on_volume * target_vol->block_size_bytes,
            block_data_actual_len,
            handler_ctx->data_to_write_dma,
            _replica_request_local_write_done_cb, handler_ctx);

        if (local_write_err != XSAN_OK) {
            XSAN_LOG_ERROR("[ReplicaHandler] Failed to submit local write for replica (Vol: %s): %s", vol_id_s, xsan_error_string(local_write_err));
            xsan_replica_write_resp_payload_t err_resp_pl; err_resp_pl.status = local_write_err; err_resp_pl.block_lba_on_volume = req_struct->block_lba_on_volume; err_resp_pl.num_blocks_processed = 0;
            xsan_message_t *err_resp_msg = xsan_protocol_message_create(XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP, msg->header.transaction_id, &err_resp_pl, sizeof(err_resp_pl));
            if (err_resp_msg) { xsan_node_comm_send_msg(sock, err_resp_msg, NULL, NULL); xsan_protocol_message_destroy(err_resp_msg); } // Fire and forget error response
            if(handler_ctx->data_to_write_dma) xsan_bdev_dma_free(handler_ctx->data_to_write_dma);
            XSAN_FREE(handler_ctx);
        }
        // If submit OK, _replica_request_local_write_done_cb will handle response and freeing handler_ctx
    } else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP) {
        if (!msg->payload || msg->header.payload_length < sizeof(xsan_replica_write_resp_payload_t)) {
            XSAN_LOG_ERROR("[ReplicaHandler] Received REPLICA_WRITE_BLOCK_RESP with invalid payload size from %s.", peer_addr_str);
            goto msg_destroy_exit;
        }
        xsan_replica_write_resp_payload_t *resp_payload = (xsan_replica_write_resp_payload_t *)msg->payload;
        XSAN_LOG_INFO("[ReplicaHandler] Received REPLICA_WRITE_BLOCK_RESP from %s for TID %lu, Status %d, LBA %lu, Num %u",
                      peer_addr_str, msg->header.transaction_id, resp_payload->status,
                      resp_payload->block_lba_on_volume, resp_payload->num_blocks_processed);
        // Pass to volume manager to process this response
        xsan_volume_manager_process_replica_write_response(vm, msg->header.transaction_id,
                                                           g_comm_test_controller.client_sock_to_self ? ((xsan_connection_ctx_t*)spdk_sock_get_cb_arg(g_comm_test_controller.client_sock_to_self))->replica_nodes[0].node_id : (xsan_node_id_t){{0}}, // Placeholder node ID
                                                           resp_payload->status);
    }

msg_destroy_exit:
    xsan_protocol_message_destroy(msg);
}


static void xsan_node_main_spdk_thread_start(void *arg1, int rc) {
    // ... (rest of the function, including test setups and the main polling loop) ...
    // Ensure that the call to xsan_node_comm_init passes `vm` as the cb_arg:
    // if (xsan_node_comm_init(node_listen_ip, node_listen_port, _xsan_node_test_message_handler, vm) != XSAN_OK) { ... }

    (void)arg1; xsan_error_t err = XSAN_OK; xsan_disk_manager_t *dm = NULL; xsan_volume_manager_t *vm = NULL;
    XSAN_LOG_INFO("XSAN SPDK app thread started (rc: %d).", rc);
    if (rc != 0) { goto app_stop_sequence_start; }
    if (xsan_bdev_subsystem_init() != XSAN_OK) { goto app_stop_sequence_start; }
    if (xsan_disk_manager_init(&dm) != XSAN_OK) { goto app_stop_bdev_fini; }
    g_async_io_test_controller.dm = dm;
    if (xsan_volume_manager_init(dm, &vm) != XSAN_OK) { goto app_stop_disk_mgr_fini; }
    g_async_io_test_controller.vm = vm;
    const char *node_listen_ip = "0.0.0.0"; uint16_t node_listen_port = 7777;
    if (xsan_node_comm_init(node_listen_ip, node_listen_port, _xsan_node_test_message_handler, vm /* Pass vm */) != XSAN_OK) {
        goto app_stop_vol_mgr_fini;
    }
    if (xsan_disk_manager_scan_and_register_bdevs(dm) != XSAN_OK) { /* Log, continue */ }
    xsan_disk_t **disks_list = NULL; int disk_count = 0;
    xsan_disk_manager_get_all_disks(dm, &disks_list, &disk_count);
    if(disks_list) { XSAN_LOG_DEBUG("Found %d XSAN disks.", disk_count); }

    xsan_group_id_t test_dg_id; memset(&test_dg_id, 0, sizeof(test_dg_id)); bool dg_created = false;
    xsan_volume_id_t test_vol_id; memset(&test_vol_id, 0, sizeof(test_vol_id)); bool vol_created = false;
    xsan_volume_t *vol_for_async_test = NULL; xsan_disk_t *disk_for_vol_group = NULL;

    if (disk_count > 0 && disks_list) {
        disk_for_vol_group = disks_list[0];
        const char *dg_bdevs[] = {disk_for_vol_group->bdev_name};
        if (xsan_disk_manager_find_disk_group_by_name(dm, "MetaTestDG") == NULL) {
            if (xsan_disk_manager_disk_group_create(dm, "MetaTestDG", XSAN_DISK_GROUP_TYPE_PASSSTHROUGH, dg_bdevs, 1, &test_dg_id) == XSAN_OK) {
                 dg_created = true; XSAN_LOG_INFO("Created 'MetaTestDG'.");
            } else { XSAN_LOG_ERROR("Failed to create 'MetaTestDG'");}
        } else {
            xsan_disk_group_t* existing_dg = xsan_disk_manager_find_disk_group_by_name(dm, "MetaTestDG");
            if(existing_dg) { memcpy(&test_dg_id, &existing_dg->id, sizeof(xsan_group_id_t)); dg_created = true; XSAN_LOG_INFO("'MetaTestDG' already exists, using it.");}
            else {XSAN_LOG_ERROR("Could not find existing 'MetaTestDG' by name after check.");}
        }
    } else { XSAN_LOG_WARN("No disks for 'MetaTestDG'.");}
    if(disks_list) { xsan_disk_manager_free_disk_pointer_list(disks_list); disks_list = NULL; }

    if (!spdk_uuid_is_null((struct spdk_uuid*)&test_dg_id.data[0])) {
        uint32_t test_ftt = 1; // Test with FTT=1 for replication test
        uint64_t vol_s_mb = (disk_for_vol_group && disk_for_vol_group->capacity_bytes / (1024*1024) > 256) ? 128 : (disk_for_vol_group ? disk_for_vol_group->capacity_bytes / (1024*1024*2) : 64);
        if(vol_s_mb < 16) vol_s_mb = 16; uint64_t vol_s_bytes = vol_s_mb * 1024 * 1024;
        uint32_t vol_bs = 4096; if(vol_s_bytes < vol_bs) vol_s_bytes = vol_bs;

        if (xsan_volume_get_by_name(vm, "MetaTestVolRep") == NULL) {
             XSAN_LOG_INFO("Attempting to create replicated volume 'MetaTestVolRep' (FTT=%u)...", test_ftt);
            err = xsan_volume_create(vm, "MetaTestVolRep", vol_s_bytes, test_dg_id, vol_bs, false, test_ftt, &test_vol_id);
            if (err == XSAN_OK) { vol_created = true; XSAN_LOG_INFO("Created 'MetaTestVolRep'."); }
            else { XSAN_LOG_ERROR("Failed to create 'MetaTestVolRep': %s", xsan_error_string(err));}
        } else {
            XSAN_LOG_INFO("Volume 'MetaTestVolRep' seems to exist from metadata load.");
            vol_for_async_test = xsan_volume_get_by_name(vm, "MetaTestVolRep");
            if(vol_for_async_test) { memcpy(&test_vol_id, &vol_for_async_test->id, sizeof(xsan_volume_id_t)); vol_created = true;}
        }
    } else { XSAN_LOG_WARN("Skipping 'MetaTestVolRep' creation, 'MetaTestDG' not available.");}

    vol_for_async_test = xsan_volume_get_by_id(vm, test_vol_id);
    if (vol_created && vol_for_async_test) { /* ... Log vol details ... */ }

    memset(&g_comm_test_controller, 0, sizeof(g_comm_test_controller));
    memset(&g_async_io_test_controller, 0, sizeof(g_async_io_test_controller));
    g_async_io_test_controller.dm = dm; g_async_io_test_controller.vm = vm;

    // Comm test (self-ping) - will run concurrently with async IO test
    XSAN_LOG_INFO("[CommTest] Initiating self-connect PING test...");
    err = xsan_node_comm_connect("127.0.0.1", node_listen_port, _comm_test_connect_cb, &g_comm_test_controller);
    if(err != XSAN_OK) { XSAN_LOG_ERROR("[CommTest] Failed to initiate self-connect: %s", xsan_error_string(err)); g_comm_test_controller.test_finished_signal = true; }

    if (vol_created && vol_for_async_test && disk_for_vol_group) {
        _start_async_io_test_on_volume(&g_async_io_test_controller, vol_for_async_test);
    } else {
        XSAN_LOG_WARN("Skipping async I/O test as test volume or its disk was not available.");
        g_async_io_test_controller.test_finished_signal = true;
    }

    XSAN_LOG_INFO("Waiting for all tests (Async I/O, Comm) to complete...");
    while(!g_async_io_test_controller.test_finished_signal || !g_comm_test_controller.test_finished_signal) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
    XSAN_LOG_INFO("All tests signaled completion.");

    if (vol_created) { xsan_volume_delete(vm, test_vol_id); XSAN_LOG_INFO("Cleaned up 'MetaTestVolRep'.");}
    if (dg_created && !spdk_uuid_is_null((struct spdk_uuid*)&test_dg_id.data[0])) { xsan_disk_manager_disk_group_delete(dm, test_dg_id); XSAN_LOG_INFO("Cleaned up 'MetaTestDG'.");}

app_stop_vol_mgr_fini:
    if (vm) xsan_volume_manager_fini(&vm);
app_stop_disk_mgr_fini:
    if (dm) xsan_disk_manager_fini(&dm);
app_stop_bdev_fini:
    xsan_bdev_subsystem_fini();
app_stop_sequence_start:
    XSAN_LOG_INFO("Requesting SPDK application stop.");
    xsan_spdk_manager_request_app_stop();
}

int main(int argc, char **argv) { /* ... as before ... */
    xsan_log_config_t log_cfg = xsan_log_default_config(); log_cfg.level = XSAN_LOG_LEVEL_DEBUG; xsan_log_init(&log_cfg);
    XSAN_LOG_INFO("XSAN Node starting..."); const char *spdk_json_conf_file = NULL;
    if (argc > 1) { for (int i = 1; i < argc; ++i) { if (strcmp(argv[i], "--spdk-conf") == 0 && i + 1 < argc) { spdk_json_conf_file = argv[++i]; break; } else if (i == 1 && argv[i][0] != '-') { spdk_json_conf_file = argv[i]; }}}
    if (spdk_json_conf_file) XSAN_LOG_INFO("Using SPDK JSON config: %s", spdk_json_conf_file); else XSAN_LOG_WARN("No SPDK JSON config.");
    mkdir("./xsan_meta_db", 0755); mkdir("./xsan_meta_db/disk_manager_db", 0755); mkdir("./xsan_meta_db/volume_manager_db", 0755);
    xsan_error_t err = xsan_spdk_manager_opts_init("xsan_node_main", spdk_json_conf_file, "0x1", false, NULL);
    if (err != XSAN_OK) { XSAN_LOG_FATAL("SPDK opts init failed: %s", xsan_error_string(err)); xsan_log_shutdown(); return EXIT_FAILURE; }
    err = xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start, NULL);
    if (err != XSAN_OK) { XSAN_LOG_FATAL("xsan_spdk_manager_start_app error: %s", xsan_error_string(err)); }
    xsan_node_comm_fini(); xsan_spdk_manager_app_fini();
    XSAN_LOG_INFO("XSAN Node has shut down."); xsan_log_shutdown();
    return (err == XSAN_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
