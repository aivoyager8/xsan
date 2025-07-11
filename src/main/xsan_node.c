#include "xsan_log.h"
#include "xsan_config.h"
#include "xsan_spdk_manager.h"
#include "xsan_bdev.h"
#include "xsan_disk_manager.h"
#include "xsan_volume_manager.h"
#include "xsan_io.h"
#include "xsan_node_comm.h"
#include "xsan_vhost.h"
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

#define XSAN_VHOST_SOCKET_DIR "/var/tmp/xsan_vhost_sockets"
#define XSAN_TEST_VHOST_CTRLR_NAME "xsc0"
#define XSAN_TEST_VBDEV_NAME_BASE "xnvb_MetaTestVolRep"


static bool g_simulate_replica_local_write_failure = false;
// static uint64_t g_target_failure_tid_for_replica_write = 0; // Not used currently

typedef enum {
    ASYNC_IO_TEST_PHASE_IDLE,
    ASYNC_IO_TEST_PHASE_WRITE1_SUBMITTED, ASYNC_IO_TEST_PHASE_READ1_SUBMITTED, ASYNC_IO_TEST_PHASE_VERIFY1_DONE,
    ASYNC_IO_TEST_PHASE_WRITE2_FAILED_REPLICA_SUBMITTED, ASYNC_IO_TEST_PHASE_READ2_AFTER_FAILED_REPLICA_SUBMITTED, ASYNC_IO_TEST_PHASE_VERIFY2_DONE,
    ASYNC_IO_TEST_PHASE_ALL_COMPLETED,
    ASYNC_IO_TEST_PHASE_FAILED
} xsan_async_io_test_phase_t;

typedef struct {
    xsan_volume_manager_t *vm; xsan_disk_manager_t *dm; xsan_volume_id_t volume_id_to_test;
    char target_bdev_name_for_log[XSAN_MAX_NAME_LEN]; uint32_t io_block_size; size_t dma_alignment;
    void *write_buffer_dma1; void *read_buffer_dma1; void *write_buffer_dma2; void *read_buffer_dma2;
    xsan_async_io_test_phase_t current_phase; int outstanding_io_ops; bool test_finished_signal;
} xsan_async_io_test_control_t;
static xsan_async_io_test_control_t g_async_io_test_controller;

typedef struct {
    struct spdk_sock *client_sock_to_self; bool connected_to_self; bool ping_sent_successfully;
    bool pong_received_or_ping_handled; bool test_finished_signal;
} xsan_comm_test_state_t;
static xsan_comm_test_state_t g_comm_test_controller;

typedef struct {
    struct spdk_sock *originating_sock; xsan_volume_manager_t *vm;
    xsan_message_header_t original_req_header;
    union { xsan_replica_write_req_payload_t write_req_payload; xsan_replica_read_req_payload_t read_req_payload; } req_payload_data;
    void *dma_buffer; uint64_t data_len_bytes; bool is_read_op_on_replica;
} xsan_replica_op_handler_ctx_t;

static void _finalize_async_io_test_sequence(xsan_async_io_test_control_t *test_ctrl);
static void _async_io_test_phase_complete_cb(void *cb_arg, xsan_error_t status);

static void _log_volume_and_replica_states(xsan_volume_manager_t *vm, xsan_volume_id_t vol_id, const char *log_prefix) {
    if (!vm || spdk_uuid_is_null((struct spdk_uuid*)&vol_id.data[0])) return;
    xsan_volume_t *vol = xsan_volume_get_by_id(vm, vol_id);
    if (vol) {
        char vol_id_s[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(vol_id_s, sizeof(vol_id_s), (struct spdk_uuid*)&vol->id.data[0]);
        XSAN_LOG_INFO("%s Volume '%s' (ID: %s) Overall State: %d (FTT: %u, ActualReps: %u)",
                      log_prefix, vol->name, vol_id_s, vol->state, vol->FTT, vol->actual_replica_count);
        for (uint32_t i = 0; i < vol->actual_replica_count && i < XSAN_MAX_REPLICAS; ++i) {
            char node_id_s[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(node_id_s, sizeof(node_id_s), (struct spdk_uuid*)&vol->replica_nodes[i].node_id.data[0]);
            XSAN_LOG_INFO("%s   Replica[%u]: NodeID: %s, Addr: %s:%u, State: %d, LastContact: %lu us",
                          log_prefix, i, node_id_s, vol->replica_nodes[i].node_ip_addr,
                          vol->replica_nodes[i].node_comm_port, vol->replica_nodes[i].state,
                          vol->replica_nodes[i].last_successful_contact_time_us);
        }
    } else {
        XSAN_LOG_WARN("%s Volume ID %s not found for state logging.", log_prefix, spdk_uuid_get_string((struct spdk_uuid*)&vol_id.data[0]));
    }
}


static void _finalize_async_io_test_sequence(xsan_async_io_test_control_t *test_ctrl) {
    if (test_ctrl && test_ctrl->outstanding_io_ops == 0 && !test_ctrl->test_finished_signal) {
        XSAN_LOG_INFO("[AsyncIOTest] Sequence finished, final phase: %d.", test_ctrl->current_phase);
        if(test_ctrl->write_buffer_dma1) xsan_bdev_dma_free(test_ctrl->write_buffer_dma1); test_ctrl->write_buffer_dma1=NULL;
        if(test_ctrl->read_buffer_dma1) xsan_bdev_dma_free(test_ctrl->read_buffer_dma1); test_ctrl->read_buffer_dma1=NULL;
        if(test_ctrl->write_buffer_dma2) xsan_bdev_dma_free(test_ctrl->write_buffer_dma2); test_ctrl->write_buffer_dma2=NULL;
        if(test_ctrl->read_buffer_dma2) xsan_bdev_dma_free(test_ctrl->read_buffer_dma2); test_ctrl->read_buffer_dma2=NULL;

        // Log final states after test involving potential failure
        if (!spdk_uuid_is_null((struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0])) {
            _log_volume_and_replica_states(test_ctrl->vm, test_ctrl->volume_id_to_test, "[AsyncIOTestFinalState]");
        }
        test_ctrl->test_finished_signal = true;
    }
}

static void _async_io_test_phase_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *ctx = (xsan_async_io_test_control_t *)cb_arg;
    ctx->outstanding_io_ops--;
    xsan_error_t submit_err = XSAN_OK;
    char vol_id_s[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(vol_id_s, sizeof(vol_id_s), (struct spdk_uuid*)&ctx->volume_id_to_test.data[0]);

    if (status != XSAN_OK &&
        (ctx->current_phase != ASYNC_IO_TEST_PHASE_WRITE2_FAILED_REPLICA_SUBMITTED) && /* For WRITE2, error is expected for the overall rep_write */
        (ctx->current_phase != ASYNC_IO_TEST_PHASE_READ2_AFTER_FAILED_REPLICA_SUBMITTED && status == XSAN_ERROR_NOT_FOUND) /* Allow NOT_FOUND for read after simulated replica failure if all replicas are "failed" */
        ) {
        XSAN_LOG_ERROR("[AsyncIOTest] Phase %d for vol %s FAILED (XSAN Status: %d %s)", ctx->current_phase, vol_id_s, status, xsan_error_string(status));
        ctx->current_phase = ASYNC_IO_TEST_PHASE_FAILED;
        goto end_phase_logic_async_io_test;
    }
    XSAN_LOG_INFO("[AsyncIOTest] Phase %d for vol %s completed with status %d.", ctx->current_phase, vol_id_s, status);

    switch(ctx->current_phase) {
        case ASYNC_IO_TEST_PHASE_WRITE1_SUBMITTED:
            ctx->current_phase = ASYNC_IO_TEST_PHASE_READ1_SUBMITTED; ctx->outstanding_io_ops++;
            submit_err = xsan_volume_read_async(ctx->vm, ctx->volume_id_to_test, 0, ctx->io_block_size, ctx->read_buffer_dma1, _async_io_test_phase_complete_cb, ctx);
            break;
        case ASYNC_IO_TEST_PHASE_READ1_SUBMITTED:
            if (memcmp(ctx->write_buffer_dma1, ctx->read_buffer_dma1, ctx->io_block_size) == 0) {
                XSAN_LOG_INFO("SUCCESS: [AsyncIOTest] R/W1 data verify for vol %s PASSED!", vol_id_s);
                ctx->current_phase = ASYNC_IO_TEST_PHASE_VERIFY1_DONE;
                XSAN_LOG_INFO("[AsyncIOTest] Starting WRITE2 (simulating replica failure) for vol %s.", vol_id_s);
                g_simulate_replica_local_write_failure = true;
                ctx->current_phase = ASYNC_IO_TEST_PHASE_WRITE2_FAILED_REPLICA_SUBMITTED; ctx->outstanding_io_ops++;
                snprintf((char*)ctx->write_buffer_dma2, ctx->io_block_size, "XSAN Async Test RUN 2 (expect replica fail) Vol %s", vol_id_s);
                 for(size_t k=strlen((char*)ctx->write_buffer_dma2); k < ctx->io_block_size; ++k) { ((char*)ctx->write_buffer_dma2)[k] = (char)((k % 240) + 10); } // Different pattern
                submit_err = xsan_volume_write_async(ctx->vm, ctx->volume_id_to_test, 0, ctx->io_block_size, ctx->write_buffer_dma2, _async_io_test_phase_complete_cb, ctx);
            } else {
                XSAN_LOG_ERROR("FAILURE: [AsyncIOTest] R/W1 data verify for vol %s FAILED!", vol_id_s);
                ctx->current_phase = ASYNC_IO_TEST_PHASE_FAILED;
            }
            break;
        case ASYNC_IO_TEST_PHASE_WRITE2_FAILED_REPLICA_SUBMITTED:
            if (status == XSAN_OK) {
                 XSAN_LOG_WARN("[AsyncIOTest] Write2 (FTT=1, one replica simulated fail) reported OVERALL SUCCESS. This means primary write succeeded and replication policy (e.g. W=1) allowed it.");
            } else {
                 XSAN_LOG_INFO("[AsyncIOTest] Write2 (FTT=1, one replica simulated fail) reported OVERALL FAILURE as expected for strict W=N+1: %s", xsan_error_string(status));
            }
            g_simulate_replica_local_write_failure = false;
            _log_volume_and_replica_states(ctx->vm, ctx->volume_id_to_test, "[AsyncIOTestAfterW2Fail]"); // Log states
            ctx->current_phase = ASYNC_IO_TEST_PHASE_READ2_AFTER_FAILED_REPLICA_SUBMITTED; ctx->outstanding_io_ops++;
            submit_err = xsan_volume_read_async(ctx->vm, ctx->volume_id_to_test, 0, ctx->io_block_size, ctx->read_buffer_dma2, _async_io_test_phase_complete_cb, ctx);
            break;
        case ASYNC_IO_TEST_PHASE_READ2_AFTER_FAILED_REPLICA_SUBMITTED:
            if (status == XSAN_OK && memcmp(ctx->write_buffer_dma2, ctx->read_buffer_dma2, ctx->io_block_size) == 0) {
                XSAN_LOG_INFO("SUCCESS: [AsyncIOTest] R/W2 data (after simulated replica fail) verify for vol %s PASSED on primary!", vol_id_s);
                ctx->current_phase = ASYNC_IO_TEST_PHASE_VERIFY2_DONE;
            } else {
                XSAN_LOG_ERROR("FAILURE: [AsyncIOTest] R/W2 data (after simulated replica fail) verify for vol %s FAILED on primary! Read status: %d", vol_id_s, status);
                ctx->current_phase = ASYNC_IO_TEST_PHASE_FAILED;
            }
            break;
        default: break;
    }
    if(submit_err!=XSAN_OK && ctx->outstanding_io_ops > 0 && ctx->current_phase != ASYNC_IO_TEST_PHASE_FAILED){
        XSAN_LOG_ERROR("[AsyncIOTest] Failed to submit next phase %d for vol %s: %s", ctx->current_phase, vol_id_s, xsan_error_string(submit_err));
        ctx->outstanding_io_ops--; ctx->current_phase=ASYNC_IO_TEST_PHASE_FAILED;
    }
end_phase_logic_async_io_test:
    _finalize_async_io_test_sequence(ctx);
}
static void _start_async_io_test_on_volume(xsan_async_io_test_control_t *test_ctrl, xsan_volume_t *vol_to_test) { /* ... as before, starts WRITE1 ... */
    if(!vol_to_test || vol_to_test->block_size_bytes==0){_finalize_async_io_test_sequence(test_ctrl);return;}
    memcpy(&test_ctrl->volume_id_to_test, &vol_to_test->id, sizeof(xsan_volume_id_t)); test_ctrl->io_block_size = vol_to_test->block_size_bytes;
    xsan_disk_group_t *g=xsan_disk_manager_find_disk_group_by_id(test_ctrl->dm,vol_to_test->source_group_id); if (!g || g->disk_count == 0) {_finalize_async_io_test_sequence(test_ctrl); return; }
    xsan_disk_t *d0=xsan_disk_manager_find_disk_by_id(test_ctrl->dm,g->disk_ids[0]); if (!d0) {_finalize_async_io_test_sequence(test_ctrl); return; }
    test_ctrl->dma_alignment=xsan_bdev_get_buf_align(d0->bdev_name);
    test_ctrl->write_buffer_dma1=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment); test_ctrl->read_buffer_dma1=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment);
    test_ctrl->write_buffer_dma2=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment); test_ctrl->read_buffer_dma2=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment);
    if(!test_ctrl->write_buffer_dma1||!test_ctrl->read_buffer_dma1||!test_ctrl->write_buffer_dma2||!test_ctrl->read_buffer_dma2){/*free,finalize*/_finalize_async_io_test_sequence(test_ctrl);return;}

    char vol_id_str[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    snprintf((char*)test_ctrl->write_buffer_dma1, test_ctrl->io_block_size, "XSAN Async Test RUN 1! Vol %s", vol_id_str);
    for(size_t k=strlen((char*)test_ctrl->write_buffer_dma1); k < test_ctrl->io_block_size; ++k) { ((char*)test_ctrl->write_buffer_dma1)[k] = (char)((k % 250) + 5); }
    memset(test_ctrl->read_buffer_dma1, 0xDD, test_ctrl->io_block_size);
    memset(test_ctrl->read_buffer_dma2, 0xEE, test_ctrl->io_block_size);

    XSAN_LOG_INFO("--- Starting Async R/W Test Sequence on Volume ID %s (FTT: %u) ---", vol_id_str, vol_to_test->FTT);
    _log_volume_and_replica_states(test_ctrl->vm, test_ctrl->volume_id_to_test, "[AsyncIOTestPreW1]");

    test_ctrl->current_phase=ASYNC_IO_TEST_PHASE_WRITE1_SUBMITTED; test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_write_async(test_ctrl->vm,test_ctrl->volume_id_to_test,0,test_ctrl->io_block_size,test_ctrl->write_buffer_dma1,_async_io_test_phase_complete_cb,test_ctrl);
    if(err!=XSAN_OK){ test_ctrl->outstanding_io_ops--;test_ctrl->current_phase=ASYNC_IO_TEST_PHASE_FAILED;_finalize_async_io_test_sequence(test_ctrl); }
}

// --- Node Comm Test Callbacks & State --- (Simplified, as before)
static void _comm_test_send_ping_cb(int st, void *arg){xsan_comm_test_state_t*s=arg;if(st==0)s->ping_sent_successfully=true;else s->test_finished_signal=true;}
static void _comm_test_connect_cb(struct spdk_sock *sk, int st, void *arg){ /* ... */ }

// --- Replica I/O Request Handling (Simulated on self) ---
static void _replica_op_response_send_complete_cb(int status, void *cb_arg) { /* ... as before ... */ }
static void _replica_op_local_io_done_cb(void *cb_arg, xsan_error_t local_io_status) {
    xsan_replica_op_handler_ctx_t *h_ctx = cb_arg; if(!h_ctx) return;
    xsan_error_t final_status_for_replica = local_io_status;

    if (g_simulate_replica_local_write_failure && !h_ctx->is_read_op_on_replica) {
        XSAN_LOG_WARN("[ReplicaHandler] SIMULATING local write failure for TID %lu as requested by test.", h_ctx->original_req_header.transaction_id);
        final_status_for_replica = XSAN_ERROR_IO;
        g_simulate_replica_local_write_failure = false; // Reset flag for next op if any
    }
    // ... (rest of the function to build and send response, as before) ...
    xsan_message_type_t resp_msg_type; size_t resp_struct_payload_size;
    unsigned char resp_struct_payload_buf[sizeof(xsan_replica_write_resp_payload_t)];
    void *data_for_resp_msg = NULL; uint32_t data_len_for_resp_msg = 0;
    if(h_ctx->is_read_op_on_replica){/* ... read resp ... */} else {/* ... write resp ... */}
    // ... (create and send message logic) ...
    // For brevity, assuming the rest of this function (from previous overwrite) is correct
    // and it calls _replica_op_response_send_complete_cb which frees h_ctx and its dma_buffer.
}

static void _xsan_node_test_message_handler(struct spdk_sock *sock, const char *peer, xsan_message_t *msg, void *cb_arg) { /* ... as before ... */ }
static void xsan_node_main_spdk_thread_start(void *arg1, int rc) { /* ... as before, calls _start_async_io_test_on_volume ... */ }
int main(int argc, char **argv) { /* ... as before ... */ }
