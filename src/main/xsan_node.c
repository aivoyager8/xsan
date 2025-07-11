#include "xsan_log.h"
#include "xsan_config.h"
#include "xsan_spdk_manager.h"
#include "xsan_bdev.h"
#include "xsan_disk_manager.h"
#include "xsan_volume_manager.h"
#include "xsan_io.h"
#include "xsan_node_comm.h"
#include "xsan_vhost.h"         // Added for vhost
#include "xsan_error.h"
#include "xsan_string_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>           // For mkdir
#include <errno.h>              // For errno with mkdir

#include "spdk/uuid.h"
#include "spdk/env.h"
#include "spdk/sock.h"
#include "spdk/thread.h"

#define XSAN_VHOST_SOCKET_DIR "/var/tmp/xsan_vhost_sockets"
#define XSAN_TEST_VHOST_CTRLR_NAME "xsc0" // Example vhost controller name used in SPDK JSON config
#define XSAN_TEST_VBDEV_NAME "xnvb_MetaTestVolRep" // Name for our virtual bdev


// --- Async I/O Test Context and Callbacks ---
typedef struct { /* ... as before ... */
    xsan_volume_manager_t *vm; xsan_disk_manager_t *dm; xsan_volume_id_t volume_id_to_test;
    char target_bdev_name_for_log[XSAN_MAX_NAME_LEN]; uint32_t io_block_size; size_t dma_alignment;
    void *write_buffer_dma; void *read_buffer_dma;
    enum { ASYNC_IO_TEST_IDLE, ASYNC_IO_TEST_WRITE_SUBMITTED, ASYNC_IO_TEST_READ_SUBMITTED,
           ASYNC_IO_TEST_VERIFY_DONE, ASYNC_IO_TEST_FAILED } test_state;
    int outstanding_io_ops; bool test_finished_signal;
} xsan_async_io_test_control_t;
static xsan_async_io_test_control_t g_async_io_test_controller;

// --- Node Communication Test Callbacks & State ---
typedef struct { /* ... as before ... */
    struct spdk_sock *client_sock_to_self; bool connected_to_self; bool ping_sent_successfully;
    bool pong_received_or_ping_handled; bool test_finished_signal;
} xsan_comm_test_state_t;
static xsan_comm_test_state_t g_comm_test_controller;

typedef struct { /* ... as before ... */
    struct spdk_sock *originating_sock; xsan_volume_manager_t *vm;
    xsan_message_header_t original_req_header; xsan_replica_write_req_payload_t original_req_struct_payload;
    void *data_to_write_dma; uint32_t data_len;
} xsan_replica_request_handler_ctx_t;


static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl);
static void _finalize_async_io_test(xsan_async_io_test_control_t *test_ctrl);
static void _replica_response_send_complete_cb(int status, void *cb_arg);
static void _replica_request_local_write_done_cb(void *cb_arg, xsan_error_t local_write_status);

static void _async_io_test_read_complete_cb(void *cb_arg, xsan_error_t status) { /* ... as before ... */
    xsan_async_io_test_control_t *ctx = cb_arg; ctx->outstanding_io_ops--;
    if(status==XSAN_OK && memcmp(ctx->write_buffer_dma,ctx->read_buffer_dma,ctx->io_block_size)==0) ctx->test_state=ASYNC_IO_TEST_VERIFY_DONE;
    else ctx->test_state=ASYNC_IO_TEST_FAILED;
    _finalize_async_io_test(ctx);
}
static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl) { /* ... as before ... */
    test_ctrl->test_state = ASYNC_IO_TEST_READ_SUBMITTED; test_ctrl->outstanding_io_ops++;
    if(xsan_volume_read_async(test_ctrl->vm, test_ctrl->volume_id_to_test,0,test_ctrl->io_block_size,test_ctrl->read_buffer_dma,_async_io_test_read_complete_cb,test_ctrl)!=XSAN_OK){
        test_ctrl->outstanding_io_ops--;test_ctrl->test_state=ASYNC_IO_TEST_FAILED;_finalize_async_io_test(test_ctrl);
    }
}
static void _async_io_test_write_complete_cb(void *cb_arg, xsan_error_t status) { /* ... as before ... */
    xsan_async_io_test_control_t *ctx=cb_arg; ctx->outstanding_io_ops--;
    if(status!=XSAN_OK){ctx->test_state=ASYNC_IO_TEST_FAILED;_finalize_async_io_test(ctx);}
    else _run_async_io_test_read_phase(ctx);
}
static void _start_async_io_test_on_volume(xsan_async_io_test_control_t *test_ctrl, xsan_volume_t *vol) { /* ... as before ... */
    if(!vol || vol->block_size_bytes==0){_finalize_async_io_test(test_ctrl);return;}
    memcpy(&test_ctrl->volume_id_to_test, &vol->id, sizeof(xsan_volume_id_t)); test_ctrl->io_block_size = vol->block_size_bytes;
    xsan_disk_group_t *g=xsan_disk_manager_find_disk_group_by_id(test_ctrl->dm,vol->source_group_id); if(!g||g->disk_count==0){_finalize_async_io_test(test_ctrl);return;}
    xsan_disk_t *d0=xsan_disk_manager_find_disk_by_id(test_ctrl->dm,g->disk_ids[0]); if(!d0){_finalize_async_io_test(test_ctrl);return;}
    test_ctrl->dma_alignment=xsan_bdev_get_buf_align(d0->bdev_name);
    test_ctrl->write_buffer_dma=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment);
    test_ctrl->read_buffer_dma=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment);
    if(!test_ctrl->write_buffer_dma||!test_ctrl->read_buffer_dma){if(test_ctrl->write_buffer_dma)xsan_bdev_dma_free(test_ctrl->write_buffer_dma); if(test_ctrl->read_buffer_dma)xsan_bdev_dma_free(test_ctrl->read_buffer_dma); _finalize_async_io_test(test_ctrl);return;}
    snprintf((char*)test_ctrl->write_buffer_dma,test_ctrl->io_block_size,"AsyncTestVol%s",spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]));
    test_ctrl->test_state=ASYNC_IO_TEST_WRITE_SUBMITTED; test_ctrl->outstanding_io_ops++;
    if(xsan_volume_write_async(test_ctrl->vm,test_ctrl->volume_id_to_test,0,test_ctrl->io_block_size,test_ctrl->write_buffer_dma,_async_io_test_write_complete_cb,test_ctrl)!=XSAN_OK){
        test_ctrl->outstanding_io_ops--;test_ctrl->test_state=ASYNC_IO_TEST_FAILED;_finalize_async_io_test(test_ctrl);
    }
}
static void _finalize_async_io_test(xsan_async_io_test_control_t *test_ctrl) { /* ... as before ... */
    if(test_ctrl && test_ctrl->outstanding_io_ops==0 && !test_ctrl->test_finished_signal){
        if(test_ctrl->write_buffer_dma) xsan_bdev_dma_free(test_ctrl->write_buffer_dma); test_ctrl->write_buffer_dma=NULL;
        if(test_ctrl->read_buffer_dma) xsan_bdev_dma_free(test_ctrl->read_buffer_dma); test_ctrl->read_buffer_dma=NULL;
        test_ctrl->test_finished_signal=true; XSAN_LOG_INFO("[AsyncIOTest] Finished.");
    }
}
static void _comm_test_send_ping_cb(int status, void *cb_arg) { /* ... as before ... */
    xsan_comm_test_state_t*cs=cb_arg; if(status==0)cs->ping_sent_successfully=true; else cs->test_finished_signal=true;
}
static void _comm_test_connect_cb(struct spdk_sock *sock, int status, void *cb_arg) { /* ... as before ... */
    xsan_comm_test_state_t*cs=cb_arg; if(status==0){cs->client_sock_to_self=sock;cs->connected_to_self=true;
        xsan_message_t *m=xsan_protocol_message_create(XSAN_MSG_TYPE_HEARTBEAT,1,"SELF_PING",10);
        if(m){if(xsan_node_comm_send_msg(sock,m,_comm_test_send_ping_cb,cs)!=XSAN_OK)cs->test_finished_signal=true;xsan_protocol_message_destroy(m);} else cs->test_finished_signal=true;
    } else cs->test_finished_signal=true;
}
static void _replica_response_send_complete_cb(int status, void *cb_arg) { /* ... as before ... */
    xsan_replica_request_handler_ctx_t *hctx = cb_arg; if(!hctx) return;
    if(status!=0)XSAN_LOG_ERROR("[ReplicaHandler] Send RESP failed TID %lu",hctx->original_req_header.transaction_id);
    if(hctx->data_to_write_dma)xsan_bdev_dma_free(hctx->data_to_write_dma); XSAN_FREE(hctx);
}
static void _replica_request_local_write_done_cb(void *cb_arg, xsan_error_t status) { /* ... as before ... */
    xsan_replica_request_handler_ctx_t *hctx = cb_arg; if(!hctx) return;
    xsan_replica_write_resp_payload_t pl; pl.status=status; pl.block_lba_on_volume=hctx->original_req_struct_payload.block_lba_on_volume;
    pl.num_blocks_processed=(status==XSAN_OK)?hctx->original_req_struct_payload.num_blocks:0;
    xsan_message_t *msg = xsan_protocol_message_create(XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP,hctx->original_req_header.transaction_id,&pl,sizeof(pl));
    if(msg){if(xsan_node_comm_send_msg(hctx->originating_sock,msg,_replica_response_send_complete_cb,hctx)!=XSAN_OK){if(hctx->data_to_write_dma)xsan_bdev_dma_free(h_ctx->data_to_write_dma);XSAN_FREE(h_ctx);}}
    else {if(h_ctx->data_to_write_dma)xsan_bdev_dma_free(h_ctx->data_to_write_dma);XSAN_FREE(h_ctx);}
    if(msg) xsan_protocol_message_destroy(msg); // xsan_node_comm_send_msg copies it
}

static void _xsan_node_test_message_handler(struct spdk_sock *sock, const char *peer, xsan_message_t *msg, void *cb_arg) {
    xsan_volume_manager_t *vm = cb_arg; if(!vm){xsan_protocol_message_destroy(msg);return;}
    XSAN_LOG_DEBUG("[MsgHandler] From %s: Type %u, TID %lu", peer, msg->header.type, msg->header.transaction_id);
    if (msg->header.type == XSAN_MSG_TYPE_HEARTBEAT) { /* ... handle PING ... */
        g_comm_test_controller.pong_received_or_ping_handled = true; g_comm_test_controller.test_finished_signal = true;
    } else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ) { /* ... handle REQ ... */
        xsan_replica_write_req_payload_t *req_pl = (xsan_replica_write_req_payload_t*)msg->payload;
        uint8_t *data = msg->payload + sizeof(xsan_replica_write_req_payload_t);
        uint32_t data_len = msg->header.payload_length - sizeof(xsan_replica_write_req_payload_t);
        xsan_volume_t *vol = xsan_volume_get_by_id(vm, req_pl->volume_id); if(!vol){goto msg_hdl_destroy_exit;}
        if(data_len != (uint64_t)req_pl->num_blocks * vol->block_size_bytes){goto msg_hdl_destroy_exit;}
        xsan_replica_request_handler_ctx_t *h_ctx = XSAN_MALLOC(sizeof(*h_ctx)); if(!h_ctx){goto msg_hdl_destroy_exit;}
        h_ctx->originating_sock=sock; h_ctx->vm=vm; memcpy(&h_ctx->original_req_header,&msg->header,sizeof(msg->header));
        memcpy(&h_ctx->original_req_struct_payload,req_pl,sizeof(*req_pl)); h_ctx->data_len=data_len;
        size_t align=4096; /* get real align */ h_ctx->data_to_write_dma=xsan_bdev_dma_malloc(data_len,align); if(!h_ctx->data_to_write_dma){XSAN_FREE(h_ctx);goto msg_hdl_destroy_exit;}
        memcpy(h_ctx->data_to_write_dma,data,data_len);
        if(xsan_volume_write_async(vm,req_pl->volume_id,req_pl->block_lba_on_volume*vol->block_size_bytes,data_len,h_ctx->data_to_write_dma,_replica_request_local_write_done_cb,h_ctx)!=XSAN_OK){
            /* error, free dma, free h_ctx, send error resp */
            if(h_ctx->data_to_write_dma)xsan_bdev_dma_free(h_ctx->data_to_write_dma); XSAN_FREE(h_ctx);
        }
    } else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP) { /* ... handle RESP ... */
        xsan_replica_write_resp_payload_t *resp_pl = (xsan_replica_write_resp_payload_t*)msg->payload;
        xsan_node_id_t NID_placeholder; memset(&NID_placeholder,0,sizeof(NID_placeholder)); // TODO get actual responder ID
        xsan_volume_manager_process_replica_write_response(vm, msg->header.transaction_id, NID_placeholder, resp_pl->status);
    }
msg_hdl_destroy_exit:
    xsan_protocol_message_destroy(msg);
}

static void xsan_node_main_spdk_thread_start(void *arg1, int rc) {
    (void)arg1; xsan_error_t err = XSAN_OK;
    xsan_disk_manager_t *dm = NULL; xsan_volume_manager_t *vm = NULL;
    bool vbdev_exposed_flag = false; char test_vbdev_name[XSAN_MAX_NAME_LEN] = {0};

    XSAN_LOG_INFO("XSAN SPDK app thread started (rc: %d).", rc);
    if (rc != 0) { goto app_stop_sequence_start_no_cleanup; }

    if (xsan_bdev_subsystem_init() != XSAN_OK) { goto app_stop_sequence_start_no_cleanup; }
    if (xsan_disk_manager_init(&dm) != XSAN_OK) { goto app_stop_bdev_subsystem_fini; }
    g_async_io_test_controller.dm = dm;
    if (xsan_volume_manager_init(dm, &vm) != XSAN_OK) { goto app_stop_disk_manager_fini; }
    g_async_io_test_controller.vm = vm;
    const char *node_listen_ip = "0.0.0.0"; uint16_t node_listen_port = 7777;
    if (xsan_node_comm_init(node_listen_ip, node_listen_port, _xsan_node_test_message_handler, vm) != XSAN_OK) {
        goto app_stop_volume_manager_fini;
    }
    // Initialize Vhost Subsystem
    if (mkdir(XSAN_VHOST_SOCKET_DIR, 0777) != 0 && errno != EEXIST) {
        XSAN_LOG_WARN("Could not create vhost socket directory %s: %s. Vhost might fail.", XSAN_VHOST_SOCKET_DIR, strerror(errno));
    }
    if (xsan_vhost_subsystem_init(vm) != XSAN_OK) { // Pass vm
        XSAN_LOG_ERROR("XSAN vhost subsystem init failed.");
        // Not necessarily fatal for other tests, but vhost won't work.
    }

    if (xsan_disk_manager_scan_and_register_bdevs(dm) != XSAN_OK) { /* Log, continue */ }
    // ... (Disk listing as before) ...

    xsan_group_id_t test_dg_id; memset(&test_dg_id, 0, sizeof(test_dg_id)); bool dg_created = false;
    xsan_volume_id_t test_vol_id; memset(&test_vol_id, 0, sizeof(test_vol_id)); bool vol_created = false;
    xsan_volume_t *vol_for_async_test = NULL; xsan_disk_t *disk_for_vol_group = NULL;
    // ... (Test DG and Vol creation as before, using MetaTestDG, MetaTestVolRep, FTT=1) ...
    xsan_disk_t **tmp_disks_list = NULL; int tmp_disk_count = 0;
    xsan_disk_manager_get_all_disks(dm, &tmp_disks_list, &tmp_disk_count);
    if (tmp_disk_count > 0 && tmp_disks_list) disk_for_vol_group = tmp_disks_list[0];
    if(disk_for_vol_group){/* create DG1 */} // Simplified group creation logic here
    if(tmp_disks_list) xsan_disk_manager_free_disk_pointer_list(tmp_disks_list);
    // ... (create MetaTestVolRep with FTT=1, set vol_created, vol_for_async_test, test_vol_id) ...
    // For this test, ensure a volume named "MetaTestVolRep" is created if possible
    if (disk_for_vol_group) { /* ... create MetaTestDG ... */
        const char *dg_bdevs[] = {disk_for_vol_group->bdev_name};
        if (xsan_disk_manager_find_disk_group_by_name(dm, "MetaTestDG") == NULL) {
            if (xsan_disk_manager_disk_group_create(dm, "MetaTestDG", XSAN_DISK_GROUP_TYPE_PASSSTHROUGH, dg_bdevs, 1, &test_dg_id) == XSAN_OK) dg_created = true;
        } else { if( ( (test_dg_id = xsan_disk_manager_find_disk_group_by_name(dm, "MetaTestDG")->id), !spdk_uuid_is_null((struct spdk_uuid*)&test_dg_id.data[0]) ) ) dg_created = true;}
    }
    if(dg_created){
        uint32_t test_ftt = 1; uint64_t vol_s_mb = 64; uint64_t vol_s_bytes = vol_s_mb * 1024 * 1024; uint32_t vol_bs = 4096;
        if (xsan_volume_get_by_name(vm, "MetaTestVolRep") == NULL) {
            if (xsan_volume_create(vm, "MetaTestVolRep", vol_s_bytes, test_dg_id, vol_bs, false, test_ftt, &test_vol_id) == XSAN_OK) vol_created = true;
        } else { vol_for_async_test = xsan_volume_get_by_name(vm, "MetaTestVolRep"); if(vol_for_async_test) {memcpy(&test_vol_id, &vol_for_async_test->id, sizeof(test_vol_id)); vol_created = true;}}
    }
    vol_for_async_test = xsan_volume_get_by_id(vm, test_vol_id); // Refresh pointer

    // --- Vhost Expose Test ---
    if (vol_created && vol_for_async_test) {
        snprintf(test_vbdev_name, sizeof(test_vbdev_name), XSAN_TEST_VBDEV_NAME); // Use defined name
        XSAN_LOG_INFO("Attempting to expose XSAN volume '%s' as vbdev '%s'...", vol_for_async_test->name, test_vbdev_name);
        err = xsan_vhost_expose_volume_as_vbdev(vol_for_async_test->id, test_vbdev_name);
        if (err == XSAN_OK) {
            XSAN_LOG_INFO("Successfully exposed volume '%s' as vbdev '%s'. Socket for controller (e.g. %s) should be at %s/%s",
                          vol_for_async_test->name, test_vbdev_name, XSAN_TEST_VHOST_CTRLR_NAME, XSAN_VHOST_SOCKET_DIR, XSAN_TEST_VHOST_CTRLR_NAME);
            vbdev_exposed_flag = true;
        } else { XSAN_LOG_ERROR("Failed to expose volume '%s' as vbdev '%s': %s", vol_for_async_test->name, test_vbdev_name, xsan_error_string(err)); }
    } else { XSAN_LOG_WARN("Test volume 'MetaTestVolRep' not available, skipping vhost expose test."); }


    // --- Start Comm Test & Async I/O Test ---
    memset(&g_comm_test_controller, 0, sizeof(g_comm_test_controller));
    memset(&g_async_io_test_controller, 0, sizeof(g_async_io_test_controller));
    g_async_io_test_controller.dm = dm; g_async_io_test_controller.vm = vm;
    xsan_node_comm_connect("127.0.0.1", node_listen_port, _comm_test_connect_cb, &g_comm_test_controller);
    if (vol_created && vol_for_async_test && disk_for_vol_group) {
        _start_async_io_test_on_volume(&g_async_io_test_controller, vol_for_async_test);
    } else { g_async_io_test_controller.test_finished_signal = true; }

    XSAN_LOG_INFO("Waiting for tests to complete...");
    while(!g_async_io_test_controller.test_finished_signal || !g_comm_test_controller.test_finished_signal) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
    XSAN_LOG_INFO("All tests signaled completion.");

    // --- Cleanup Test Entities ---
    if (vbdev_exposed_flag) {
        XSAN_LOG_INFO("Unexposing vbdev '%s'...", test_vbdev_name);
        xsan_vhost_unexpose_volume_vbdev(test_vbdev_name);
    }
    if (vol_created) { xsan_volume_delete(vm, test_vol_id); XSAN_LOG_INFO("Cleaned up 'MetaTestVolRep'.");}
    if (dg_created && !spdk_uuid_is_null((struct spdk_uuid*)&test_dg_id.data[0])) { xsan_disk_manager_disk_group_delete(dm, test_dg_id); XSAN_LOG_INFO("Cleaned up 'MetaTestDG'.");}

app_stop_vhost_fini: // New label
    xsan_vhost_subsystem_fini();
app_stop_comm_fini:
    xsan_node_comm_fini();
app_stop_vol_mgr_fini:
    if (vm) xsan_volume_manager_fini(&vm);
app_stop_disk_mgr_fini:
    if (dm) xsan_disk_manager_fini(&dm);
app_stop_bdev_fini:
    xsan_bdev_subsystem_fini();
app_stop_sequence_start_no_cleanup:
    XSAN_LOG_INFO("Requesting SPDK application stop.");
    xsan_spdk_manager_request_app_stop();
}

int main(int argc, char **argv) { /* ... as before, ensure XSAN_VHOST_SOCKET_DIR is created ... */
    xsan_log_config_t log_cfg = xsan_log_default_config(); log_cfg.level = XSAN_LOG_LEVEL_DEBUG; xsan_log_init(&log_cfg);
    XSAN_LOG_INFO("XSAN Node starting..."); const char *spdk_json_conf_file = NULL;
    if (argc > 1) { for (int i = 1; i < argc; ++i) { if (strcmp(argv[i], "--spdk-conf") == 0 && i + 1 < argc) { spdk_json_conf_file = argv[++i]; break; } else if (i == 1 && argv[i][0] != '-') { spdk_json_conf_file = argv[i]; }}}
    if (spdk_json_conf_file) XSAN_LOG_INFO("Using SPDK JSON config: %s", spdk_json_conf_file); else XSAN_LOG_WARN("No SPDK JSON config.");

    // Ensure directories for metadata and vhost sockets exist
    mkdir("./xsan_meta_db", 0755);
    mkdir("./xsan_meta_db/disk_manager_db", 0755);
    mkdir("./xsan_meta_db/volume_manager_db", 0755);
    if (mkdir(XSAN_VHOST_SOCKET_DIR, 0777) != 0 && errno != EEXIST) {
        XSAN_LOG_WARN("Failed to create vhost socket directory %s: %s. Vhost may not work.", XSAN_VHOST_SOCKET_DIR, strerror(errno));
    }


    xsan_error_t err = xsan_spdk_manager_opts_init("xsan_node_main", spdk_json_conf_file, "0x1", false, NULL);
    if (err != XSAN_OK) { XSAN_LOG_FATAL("SPDK opts init failed: %s", xsan_error_string(err)); xsan_log_shutdown(); return EXIT_FAILURE; }
    err = xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start, NULL);
    if (err != XSAN_OK) { XSAN_LOG_FATAL("xsan_spdk_manager_start_app error: %s", xsan_error_string(err)); }

    // Fini order is roughly reverse of init in xsan_node_main_spdk_thread_start's error path
    // xsan_vhost_subsystem_fini(); // Already called in xsan_node_main_spdk_thread_start cleanup path
    // xsan_node_comm_fini();       // Already called in xsan_node_main_spdk_thread_start cleanup path
    // Managers are also finalized there.
    // bdev subsystem also finalized there.
    // So, only spdk_manager_app_fini remains for main.
    xsan_spdk_manager_app_fini();
    XSAN_LOG_INFO("XSAN Node has shut down.");
    xsan_log_shutdown();
    return (err == XSAN_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
