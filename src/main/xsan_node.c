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


// --- Async I/O Test Context and Callbacks ---
typedef struct { /* ... */
    xsan_volume_manager_t *vm; xsan_disk_manager_t *dm; xsan_volume_id_t volume_id_to_test;
    char target_bdev_name_for_log[XSAN_MAX_NAME_LEN]; uint32_t io_block_size; size_t dma_alignment;
    void *write_buffer_dma1; void *read_buffer_dma1; void *write_buffer_dma2; void *read_buffer_dma2;
    xsan_async_io_test_phase_t current_phase; int outstanding_io_ops; bool test_finished_signal; int test_run_count;
} xsan_async_io_test_control_t;
static xsan_async_io_test_control_t g_async_io_test_controller;

// --- Node Communication Test Callbacks & State ---
typedef struct { /* ... */
    struct spdk_sock *client_sock_to_self; bool connected_to_self; bool ping_sent_successfully;
    bool pong_received_or_ping_handled; bool test_finished_signal;
} xsan_comm_test_state_t;
static xsan_comm_test_state_t g_comm_test_controller;

// --- Replica Write/Read Request Handling Context (for message handler) ---
typedef struct {
    struct spdk_sock *originating_sock;
    xsan_volume_manager_t *vm;
    xsan_message_header_t original_req_header;
    // Union for different request types, or separate contexts
    union {
        xsan_replica_write_req_payload_t write_req_payload;
        xsan_replica_read_req_payload_t read_req_payload;
    } req_payload_data;
    void *dma_buffer;            // DMA buffer for local I/O on replica
    uint64_t data_len_bytes;     // Length of data for the I/O operation
    bool is_read_op_on_replica;  // To differentiate in common callback
} xsan_replica_op_handler_ctx_t;


static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl);
static void _finalize_async_io_test_sequence(xsan_async_io_test_control_t *test_ctrl);
static void _replica_op_response_send_complete_cb(int status, void *cb_arg); // Unified send CB for RESP
static void _replica_op_local_io_done_cb(void *cb_arg, xsan_error_t local_io_status);


static void _async_io_test_read_complete_cb(void *cb_arg, xsan_error_t status) { /* ... as before ... */
    xsan_async_io_test_control_t *ctx = cb_arg; ctx->outstanding_io_ops--;
    if(status==XSAN_OK && memcmp(ctx->write_buffer_dma1,ctx->read_buffer_dma1,ctx->io_block_size)==0 && ctx->current_phase == ASYNC_IO_TEST_STATE_READ1_SUBMITTED) {
        XSAN_LOG_INFO("SUCCESS: [AsyncIOTest] R/W1 verify PASSED!"); ctx->current_phase = ASYNC_IO_TEST_STATE_VERIFY1_DONE;
        ctx->outstanding_io_ops++; snprintf((char*)ctx->write_buffer_dma2, ctx->io_block_size, "XSAN Async Test RUN 2!");
        if(xsan_volume_write_async(ctx->vm, ctx->volume_id_to_test,0,ctx->io_block_size,ctx->write_buffer_dma2,_async_io_test_phase_complete_cb,ctx)!=XSAN_OK){ctx->outstanding_io_ops--;ctx->current_phase=ASYNC_IO_TEST_STATE_FAILED;}
    } else if (status==XSAN_OK && memcmp(ctx->write_buffer_dma2,ctx->read_buffer_dma2,ctx->io_block_size)==0 && ctx->current_phase == ASYNC_IO_TEST_STATE_READ2_SUBMITTED) {
        XSAN_LOG_INFO("SUCCESS: [AsyncIOTest] R/W2 verify PASSED!"); ctx->current_phase = ASYNC_IO_TEST_STATE_VERIFY2_DONE;
    } else { ctx->current_phase=ASYNC_IO_TEST_STATE_FAILED; XSAN_LOG_ERROR("AsyncIOTest verify FAILED phase %d", ctx->current_phase); }
    _finalize_async_io_test_sequence(ctx);
}
static void _async_io_test_phase_complete_cb(void *cb_arg, xsan_error_t status) { /* Updated to manage sequence */
    xsan_async_io_test_control_t *ctx = cb_arg; ctx->outstanding_io_ops--;
    xsan_error_t submit_err = XSAN_OK;
    if(status!=XSAN_OK){ ctx->current_phase=ASYNC_IO_TEST_STATE_FAILED; goto end_phase_logic; }
    switch(ctx->current_phase){
        case ASYNC_IO_TEST_STATE_WRITE1_SUBMITTED: ctx->current_phase=ASYNC_IO_TEST_STATE_READ1_SUBMITTED; ctx->outstanding_io_ops++; submit_err=xsan_volume_read_async(ctx->vm,ctx->volume_id_to_test,0,ctx->io_block_size,ctx->read_buffer_dma1,_async_io_test_phase_complete_cb,ctx); break;
        case ASYNC_IO_TEST_STATE_READ1_SUBMITTED: /* Verification happens in _async_io_test_read_complete_cb (this func) */ _async_io_test_read_complete_cb(cb_arg, status); return; // Special case return
        case ASYNC_IO_TEST_STATE_WRITE2_SUBMITTED: ctx->current_phase=ASYNC_IO_TEST_STATE_READ2_SUBMITTED; ctx->outstanding_io_ops++; submit_err=xsan_volume_read_async(ctx->vm,ctx->volume_id_to_test,0,ctx->io_block_size,ctx->read_buffer_dma2,_async_io_test_phase_complete_cb,ctx); break;
        case ASYNC_IO_TEST_STATE_READ2_SUBMITTED: /* Verification */ _async_io_test_read_complete_cb(cb_arg, status); return; // Special case return
        default: break;
    }
    if(submit_err!=XSAN_OK){ctx->outstanding_io_ops--;ctx->current_phase=ASYNC_IO_TEST_STATE_FAILED;}
end_phase_logic: _finalize_async_io_test_sequence(ctx);
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
    snprintf((char*)test_ctrl->write_buffer_dma1,test_ctrl->io_block_size,"TestRun1 Vol%s",spdk_uuid_get_string((struct spdk_uuid*)&vol_to_test->id.data[0]));
    test_ctrl->current_phase=ASYNC_IO_TEST_STATE_WRITE1_SUBMITTED; test_ctrl->outstanding_io_ops++;
    if(xsan_volume_write_async(test_ctrl->vm,test_ctrl->volume_id_to_test,0,test_ctrl->io_block_size,test_ctrl->write_buffer_dma1,_async_io_test_phase_complete_cb,test_ctrl)!=XSAN_OK){
        test_ctrl->outstanding_io_ops--;test_ctrl->current_phase=ASYNC_IO_TEST_STATE_FAILED;_finalize_async_io_test_sequence(test_ctrl);
    }
}
static void _finalize_async_io_test_sequence(xsan_async_io_test_control_t *test_ctrl) { /* ... as before ... */
    if(test_ctrl && test_ctrl->outstanding_io_ops==0 && !test_ctrl->test_finished_signal){
        XSAN_LOG_INFO("[AsyncIOTest] Sequence finished, final phase: %d.", test_ctrl->current_phase);
        if(test_ctrl->write_buffer_dma1)xsan_bdev_dma_free(test_ctrl->write_buffer_dma1); if(test_ctrl->read_buffer_dma1)xsan_bdev_dma_free(test_ctrl->read_buffer_dma1);
        if(test_ctrl->write_buffer_dma2)xsan_bdev_dma_free(test_ctrl->write_buffer_dma2); if(test_ctrl->read_buffer_dma2)xsan_bdev_dma_free(test_ctrl->read_buffer_dma2);
        test_ctrl->write_buffer_dma1=NULL; test_ctrl->read_buffer_dma1=NULL; test_ctrl->write_buffer_dma2=NULL; test_ctrl->read_buffer_dma2=NULL;
        test_ctrl->test_finished_signal=true;
    }
}
static void _comm_test_send_ping_cb(int st, void *arg){xsan_comm_test_state_t*s=arg;if(st==0)s->ping_sent_successfully=true;else s->test_finished_signal=true;}
static void _comm_test_connect_cb(struct spdk_sock *sk, int st, void *arg){
    xsan_comm_test_state_t*s=arg;if(st==0){s->client_sock_to_self=sk;s->connected_to_self=true;
    xsan_message_t*m=xsan_protocol_message_create(XSAN_MSG_TYPE_HEARTBEAT,1,"SELF_PING",10);
    if(m){if(xsan_node_comm_send_msg(sk,m,_comm_test_send_ping_cb,s)!=XSAN_OK)s->test_finished_signal=true;xsan_protocol_message_destroy(m);}else s->test_finished_signal=true;
    }else s->test_finished_signal=true;
}

// --- Replica I/O Request Handling (Simulated on self) ---
static void _replica_op_response_send_complete_cb(int status, void *cb_arg) {
    xsan_replica_op_handler_ctx_t *handler_ctx = (xsan_replica_op_handler_ctx_t *)cb_arg;
    if (!handler_ctx) return;
    if (status != 0) { XSAN_LOG_ERROR("[ReplicaHandler] Failed to send %s RESP for TID %lu, status: %d", handler_ctx->is_read_op_on_replica ? "READ":"WRITE", handler_ctx->original_req_header.transaction_id, status); }
    else { XSAN_LOG_DEBUG("[ReplicaHandler] %s RESP for TID %lu sent.", handler_ctx->is_read_op_on_replica ? "READ":"WRITE", handler_ctx->original_req_header.transaction_id); }
    if(handler_ctx->dma_buffer) xsan_bdev_dma_free(handler_ctx->dma_buffer);
    XSAN_FREE(handler_ctx);
}

static void _replica_op_local_io_done_cb(void *cb_arg, xsan_error_t local_io_status) {
    xsan_replica_op_handler_ctx_t *handler_ctx = (xsan_replica_op_handler_ctx_t *)cb_arg;
    if (!handler_ctx) { XSAN_LOG_ERROR("NULL handler_ctx in _replica_op_local_io_done_cb"); return; }

    xsan_message_type_t resp_msg_type;
    size_t resp_struct_payload_size;
    unsigned char resp_struct_payload_buf[sizeof(xsan_replica_write_resp_payload_t) > sizeof(xsan_replica_read_resp_payload_t) ? sizeof(xsan_replica_write_resp_payload_t) : sizeof(xsan_replica_read_resp_payload_t)];
    void *data_for_resp_msg = NULL;
    uint32_t data_len_for_resp_msg = 0;

    if (handler_ctx->is_read_op_on_replica) {
        resp_msg_type = XSAN_MSG_TYPE_REPLICA_READ_BLOCK_RESP;
        xsan_replica_read_resp_payload_t *read_resp_pl = (xsan_replica_read_resp_payload_t*)resp_struct_payload_buf;
        resp_struct_payload_size = sizeof(xsan_replica_read_resp_payload_t);
        memcpy(&read_resp_pl->volume_id, &handler_ctx->req_payload_data.read_req_payload.volume_id, sizeof(xsan_volume_id_t));
        read_resp_pl->block_lba_on_volume = handler_ctx->req_payload_data.read_req_payload.block_lba_on_volume;
        read_resp_pl->status = local_io_status;
        if (local_io_status == XSAN_OK) {
            read_resp_pl->num_blocks = handler_ctx->req_payload_data.read_req_payload.num_blocks;
            data_for_resp_msg = handler_ctx->dma_buffer; // This is the data read from disk
            data_len_for_resp_msg = handler_ctx->data_len_bytes;
        } else {
            read_resp_pl->num_blocks = 0;
        }
        XSAN_LOG_INFO("[ReplicaHandler] Local read for REPLICA_READ_REQ (TID %lu) completed with status: %d", handler_ctx->original_req_header.transaction_id, local_io_status);
    } else { // Is Write Op
        resp_msg_type = XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP;
        xsan_replica_write_resp_payload_t *write_resp_pl = (xsan_replica_write_resp_payload_t*)resp_struct_payload_buf;
        resp_struct_payload_size = sizeof(xsan_replica_write_resp_payload_t);
        write_resp_pl->status = local_io_status;
        write_resp_pl->block_lba_on_volume = handler_ctx->req_payload_data.write_req_payload.block_lba_on_volume;
        write_resp_pl->num_blocks_processed = (local_io_status == XSAN_OK) ? handler_ctx->req_payload_data.write_req_payload.num_blocks : 0;
        XSAN_LOG_INFO("[ReplicaHandler] Local write for REPLICA_WRITE_BLOCK_REQ (TID %lu) completed with status: %d", handler_ctx->original_req_header.transaction_id, local_io_status);
    }

    // Prepare full payload: struct + data (if any)
    uint32_t total_resp_payload_len = resp_struct_payload_size + data_len_for_resp_msg;
    unsigned char *full_resp_payload_buf = NULL;
    if (total_resp_payload_len > 0) {
        full_resp_payload_buf = XSAN_MALLOC(total_resp_payload_len);
        if (!full_resp_payload_buf) { /* Log error, free handler_ctx and its dma_buf */ goto cleanup_handler_ctx; }
        memcpy(full_resp_payload_buf, resp_struct_payload_buf, resp_struct_payload_size);
        if (data_for_resp_msg && data_len_for_resp_msg > 0) {
            memcpy(full_resp_payload_buf + resp_struct_payload_size, data_for_resp_msg, data_len_for_resp_msg);
        }
    }

    xsan_message_t *resp_msg = xsan_protocol_message_create(resp_msg_type, handler_ctx->original_req_header.transaction_id, full_resp_payload_buf, total_resp_payload_len);
    if (full_resp_payload_buf) XSAN_FREE(full_resp_payload_buf);

    if (resp_msg) {
        XSAN_LOG_DEBUG("[ReplicaHandler] Sending %s (status %d) for TID %lu back to sock %p", resp_msg_type == XSAN_MSG_TYPE_REPLICA_READ_BLOCK_RESP ? "READ_RESP" : "WRITE_RESP", local_io_status, handler_ctx->original_req_header.transaction_id, (void*)handler_ctx->originating_sock);
        xsan_error_t send_err = xsan_node_comm_send_msg(handler_ctx->originating_sock, resp_msg, _replica_op_response_send_complete_cb, handler_ctx); // handler_ctx freed by cb
        if (send_err != XSAN_OK) {
            XSAN_LOG_ERROR("[ReplicaHandler] Failed to send %s for TID %lu: %s", resp_msg_type == XSAN_MSG_TYPE_REPLICA_READ_BLOCK_RESP ? "READ_RESP" : "WRITE_RESP", handler_ctx->original_req_header.transaction_id, xsan_error_string(send_err));
            goto cleanup_handler_ctx; // Send failed, cleanup now
        }
        xsan_protocol_message_destroy(resp_msg); // send_msg copies data or cb owns handler_ctx
    } else {
        XSAN_LOG_ERROR("[ReplicaHandler] Failed to create %s for TID %lu", resp_msg_type == XSAN_MSG_TYPE_REPLICA_READ_BLOCK_RESP ? "READ_RESP" : "WRITE_RESP", handler_ctx->original_req_header.transaction_id);
        goto cleanup_handler_ctx;
    }
    return; // handler_ctx will be freed by _replica_op_response_send_complete_cb

cleanup_handler_ctx:
    if(handler_ctx->dma_buffer) xsan_bdev_dma_free(handler_ctx->dma_buffer);
    XSAN_FREE(handler_ctx);
}

static void _xsan_node_test_message_handler(struct spdk_sock *sock, const char *peer, xsan_message_t *msg, void *cb_arg) {
    xsan_volume_manager_t *vm = cb_arg; if(!vm){xsan_protocol_message_destroy(msg);return;}
    XSAN_LOG_DEBUG("[MsgHandler] From %s: Type %u, TID %lu, PayloadLen %u", peer, msg->header.type, msg->header.transaction_id, msg->header.payload_length);

    if (msg->header.type == XSAN_MSG_TYPE_HEARTBEAT) { /* ... handle PING ... */
        g_comm_test_controller.pong_received_or_ping_handled = true; if (!g_comm_test_controller.test_finished_signal) g_comm_test_controller.test_finished_signal = true;
    } else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ) {
        if (!msg->payload || msg->header.payload_length < XSAN_REPLICA_WRITE_REQ_PAYLOAD_SIZE) goto handler_msg_destroy_exit;
        xsan_replica_write_req_payload_t *req_pl = (xsan_replica_write_req_payload_t*)msg->payload;
        uint8_t *data = msg->payload + XSAN_REPLICA_WRITE_REQ_PAYLOAD_SIZE;
        uint32_t data_len = msg->header.payload_length - XSAN_REPLICA_WRITE_REQ_PAYLOAD_SIZE;
        xsan_volume_t *vol = xsan_volume_get_by_id(vm, req_pl->volume_id); if(!vol){goto handler_msg_destroy_exit;}
        if(data_len != (uint64_t)req_pl->num_blocks * vol->block_size_bytes){goto handler_msg_destroy_exit;}

        xsan_replica_op_handler_ctx_t *h_ctx = XSAN_MALLOC(sizeof(*h_ctx)); if(!h_ctx){goto handler_msg_destroy_exit;}
        h_ctx->originating_sock=sock; h_ctx->vm=vm; memcpy(&h_ctx->original_req_header,&msg->header,sizeof(msg->header));
        memcpy(&h_ctx->req_payload_data.write_req_payload,req_pl,sizeof(*req_pl)); h_ctx->data_len_bytes=data_len; h_ctx->is_read_op_on_replica = false;
        size_t align=4096; /* get real align */ h_ctx->dma_buffer=xsan_bdev_dma_malloc(data_len,align); if(!h_ctx->dma_buffer){XSAN_FREE(h_ctx);goto handler_msg_destroy_exit;}
        memcpy(h_ctx->dma_buffer,data,data_len);

        if(xsan_volume_write_async(vm,req_pl->volume_id,req_pl->block_lba_on_volume*vol->block_size_bytes,data_len,h_ctx->dma_buffer,_replica_op_local_io_done_cb,h_ctx)!=XSAN_OK){
            if(h_ctx->dma_buffer)xsan_bdev_dma_free(h_ctx->dma_buffer); XSAN_FREE(h_ctx); /* TODO: Send error resp */
        }
    } else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_READ_BLOCK_REQ) {
        if (!msg->payload || msg->header.payload_length < XSAN_REPLICA_READ_REQ_PAYLOAD_SIZE) goto handler_msg_destroy_exit;
        xsan_replica_read_req_payload_t *req_pl = (xsan_replica_read_req_payload_t*)msg->payload;
        xsan_volume_t *vol = xsan_volume_get_by_id(vm, req_pl->volume_id); if(!vol){goto handler_msg_destroy_exit;}
        uint64_t data_len_to_read = (uint64_t)req_pl->num_blocks * vol->block_size_bytes;

        xsan_replica_op_handler_ctx_t *h_ctx = XSAN_MALLOC(sizeof(*h_ctx)); if(!h_ctx){goto handler_msg_destroy_exit;}
        h_ctx->originating_sock=sock; h_ctx->vm=vm; memcpy(&h_ctx->original_req_header,&msg->header,sizeof(msg->header));
        memcpy(&h_ctx->req_payload_data.read_req_payload,req_pl,sizeof(*req_pl)); h_ctx->data_len_bytes=data_len_to_read; h_ctx->is_read_op_on_replica = true;
        size_t align=4096; /* get real align */ h_ctx->dma_buffer=xsan_bdev_dma_malloc(data_len_to_read,align); if(!h_ctx->dma_buffer){XSAN_FREE(h_ctx);goto handler_msg_destroy_exit;}

        if(xsan_volume_read_async(vm,req_pl->volume_id,req_pl->block_lba_on_volume*vol->block_size_bytes,data_len_to_read,h_ctx->dma_buffer,_replica_op_local_io_done_cb,h_ctx)!=XSAN_OK){
            if(h_ctx->dma_buffer)xsan_bdev_dma_free(h_ctx->dma_buffer); XSAN_FREE(h_ctx); /* TODO: Send error resp */
        }
    } else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP) {
        xsan_replica_write_resp_payload_t *resp_pl = (xsan_replica_write_resp_payload_t*)msg->payload;
        xsan_node_id_t NID_placeholder; memset(&NID_placeholder,0,sizeof(NID_placeholder));
        xsan_volume_manager_process_replica_write_response(vm, msg->header.transaction_id, NID_placeholder, resp_pl->status);
    } else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_READ_BLOCK_RESP) {
        xsan_replica_read_resp_payload_t *resp_pl = (xsan_replica_read_resp_payload_t*)msg->payload;
        unsigned char *read_data = msg->payload + XSAN_REPLICA_READ_RESP_PAYLOAD_SIZE;
        uint32_t read_data_len = msg->header.payload_length - XSAN_REPLICA_READ_RESP_PAYLOAD_SIZE;
        xsan_node_id_t NID_placeholder; memset(&NID_placeholder,0,sizeof(NID_placeholder));
        xsan_volume_manager_process_replica_read_response(vm, msg->header.transaction_id, NID_placeholder, resp_pl->status, read_data, read_data_len);
    }
handler_msg_destroy_exit:
    xsan_protocol_message_destroy(msg);
}

static void xsan_node_main_spdk_thread_start(void *arg1, int rc) { /* ... as before ... */
    // ... (Initializations: dm, vm, node_comm, vhost_subsystem_init) ...
    // ... (Test DG and Vol creation for "MetaTestVolRep" with FTT=1 as before) ...
    // ... (Comm Test & Async I/O Test initiation as before) ...
    // ... (Polling loop: while(!g_async_io_test_controller.test_finished_signal || !g_comm_test_controller.test_finished_signal)) ...
    // ... (Cleanup Test Entities as before) ...
    // ... (Finalize managers in correct order) ...
    (void)arg1; xsan_error_t err = XSAN_OK; xsan_disk_manager_t *dm = NULL; xsan_volume_manager_t *vm = NULL;
    bool vbdev_exposed_flag = false; char test_vbdev_name[XSAN_MAX_NAME_LEN] = {0};
    XSAN_LOG_INFO("XSAN SPDK app thread started (rc: %d).", rc);
    if (rc != 0) { goto app_stop_sequence_start_no_cleanup; }
    if (xsan_bdev_subsystem_init() != XSAN_OK) { goto app_stop_sequence_start_no_cleanup; }
    if (xsan_disk_manager_init(&dm) != XSAN_OK) { goto app_stop_bdev_fini; }
    g_async_io_test_controller.dm = dm;
    if (xsan_volume_manager_init(dm, &vm) != XSAN_OK) { goto app_stop_disk_mgr_fini; }
    g_async_io_test_controller.vm = vm;
    const char *node_listen_ip = "0.0.0.0"; uint16_t node_listen_port = 7777;
    if (xsan_node_comm_init(node_listen_ip, node_listen_port, _xsan_node_test_message_handler, vm) != XSAN_OK) { goto app_stop_vol_mgr_fini; }
    if (mkdir(XSAN_VHOST_SOCKET_DIR, 0777) != 0 && errno != EEXIST) { XSAN_LOG_WARN("Could not create vhost socket dir %s", XSAN_VHOST_SOCKET_DIR); }
    if (xsan_vhost_subsystem_init(vm) != XSAN_OK) { XSAN_LOG_ERROR("XSAN vhost subsystem init failed."); }
    if (xsan_disk_manager_scan_and_register_bdevs(dm) != XSAN_OK && err != XSAN_ERROR_NOT_FOUND) { /* Log error */ }

    xsan_group_id_t test_dg_id; memset(&test_dg_id,0,sizeof(test_dg_id)); bool dg_created=false;
    xsan_volume_id_t test_vol_id; memset(&test_vol_id,0,sizeof(test_vol_id)); bool vol_created=false;
    xsan_volume_t *vol_for_async_test = NULL; xsan_disk_t *disk_for_vol_group = NULL;
    xsan_disk_t **disks_list = NULL; int disk_count = 0; xsan_disk_manager_get_all_disks(dm, &disks_list, &disk_count);
    if (disk_count > 0 && disks_list) disk_for_vol_group = disks_list[0];
    if(disk_for_vol_group){ const char *b[]={disk_for_vol_group->bdev_name}; if(xsan_disk_manager_find_disk_group_by_name(dm,"MetaTestDG")==NULL){if(xsan_disk_manager_disk_group_create(dm,"MetaTestDG",XSAN_DISK_GROUP_TYPE_PASSSTHROUGH,b,1,&test_dg_id)==XSAN_OK)dg_created=true;}else{xsan_disk_group_t*ex_dg=xsan_disk_manager_find_disk_group_by_name(dm,"MetaTestDG");if(ex_dg){memcpy(&test_dg_id,&ex_dg->id,sizeof(test_dg_id));dg_created=true;}}}
    if(disks_list) xsan_disk_manager_free_disk_pointer_list(disks_list);
    if(dg_created){ uint32_t ftt=1; uint64_t vs_mb=64; uint64_t vs_b=vs_mb*1024*1024; uint32_t v_bs=4096; if(vs_b<v_bs)vs_b=v_bs;
        if(xsan_volume_get_by_name(vm,"MetaTestVolRep")==NULL){if(xsan_volume_create(vm,"MetaTestVolRep",vs_b,test_dg_id,v_bs,false,ftt,&test_vol_id)==XSAN_OK)vol_created=true;}
        else{vol_for_async_test=xsan_volume_get_by_name(vm,"MetaTestVolRep");if(vol_for_async_test){memcpy(&test_vol_id,&vol_for_async_test->id,sizeof(test_vol_id));vol_created=true;}}}
    vol_for_async_test = xsan_volume_get_by_id(vm, test_vol_id);
    if (vol_created && vol_for_async_test) { snprintf(test_vbdev_name, sizeof(test_vbdev_name), "%s", XSAN_TEST_VBDEV_NAME_BASE); if(xsan_vhost_expose_volume_as_vbdev(vol_for_async_test->id, test_vbdev_name)==XSAN_OK) vbdev_exposed_flag=true;}

    memset(&g_comm_test_controller,0,sizeof(g_comm_test_controller)); memset(&g_async_io_test_controller,0,sizeof(g_async_io_test_controller));
    g_async_io_test_controller.dm=dm; g_async_io_test_controller.vm=vm;
    xsan_node_comm_connect("127.0.0.1",node_listen_port,_comm_test_connect_cb,&g_comm_test_controller);
    if(vol_created && vol_for_async_test && disk_for_vol_group) _start_async_io_test_on_volume(&g_async_io_test_controller,vol_for_async_test); else g_async_io_test_controller.test_finished_signal=true;
    XSAN_LOG_INFO("Waiting for tests..."); while(!g_async_io_test_controller.test_finished_signal || !g_comm_test_controller.test_finished_signal) spdk_thread_poll(spdk_get_thread(),0,0);
    XSAN_LOG_INFO("Tests done.");
    if(vbdev_exposed_flag) xsan_vhost_unexpose_volume_vbdev(test_vbdev_name);
    if(vol_created) xsan_volume_delete(vm,test_vol_id);
    if(dg_created && !spdk_uuid_is_null((struct spdk_uuid*)&test_dg_id.data[0])) xsan_disk_manager_disk_group_delete(dm,test_dg_id);

app_stop_vhost_fini: xsan_vhost_subsystem_fini();
app_stop_comm_fini: xsan_node_comm_fini();
app_stop_vol_mgr_fini: if (vm) xsan_volume_manager_fini(&vm);
app_stop_disk_mgr_fini: if (dm) xsan_disk_manager_fini(&dm);
app_stop_bdev_fini: xsan_bdev_subsystem_fini();
app_stop_sequence_start_no_cleanup: XSAN_LOG_INFO("Requesting SPDK app stop."); xsan_spdk_manager_request_app_stop();
}

int main(int argc, char **argv) { /* ... as before ... */
    xsan_log_config_t log_cfg=xsan_log_default_config(); log_cfg.level=XSAN_LOG_LEVEL_DEBUG; xsan_log_init(&log_cfg);
    XSAN_LOG_INFO("XSAN Node starting..."); const char *spdk_conf=NULL;
    if(argc>1){for(int i=1;i<argc;++i){if(strcmp(argv[i],"--spdk-conf")==0&&i+1<argc){spdk_conf=argv[++i];break;}else if(i==1&&argv[i][0]!='-'){spdk_conf=argv[i];}}}
    if(spdk_conf)XSAN_LOG_INFO("Using SPDK JSON conf: %s",spdk_conf);else XSAN_LOG_WARN("No SPDK JSON conf.");
    mkdir("./xsan_meta_db",0755); mkdir("./xsan_meta_db/disk_manager_db",0755); mkdir("./xsan_meta_db/volume_manager_db",0755);
    if(mkdir(XSAN_VHOST_SOCKET_DIR,0777)!=0&&errno!=EEXIST)XSAN_LOG_WARN("Failed create vhost socket dir %s: %s",XSAN_VHOST_SOCKET_DIR,strerror(errno));
    xsan_error_t err=xsan_spdk_manager_opts_init("xsan_node_main",spdk_conf,"0x1",false,NULL);
    if(err!=XSAN_OK){XSAN_LOG_FATAL("SPDK opts init failed:%s",xsan_error_string(err));xsan_log_shutdown();return EXIT_FAILURE;}
    err=xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start,NULL);
    if(err!=XSAN_OK)XSAN_LOG_FATAL("xsan_spdk_manager_start_app error:%s",xsan_error_string(err));
    // xsan_node_comm_fini(); // Now called from xsan_node_main_spdk_thread_start cleanup path via goto
    // xsan_vhost_subsystem_fini(); // Also called from cleanup path
    xsan_spdk_manager_app_fini();
    XSAN_LOG_INFO("XSAN Node has shut down."); xsan_log_shutdown();
    return (err==XSAN_OK)?EXIT_SUCCESS:EXIT_FAILURE;
}
