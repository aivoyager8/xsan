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
typedef enum {
    ASYNC_IO_TEST_STATE_IDLE,
    ASYNC_IO_TEST_STATE_WRITE1_SUBMITTED,
    ASYNC_IO_TEST_STATE_READ1_SUBMITTED,
    ASYNC_IO_TEST_STATE_VERIFY1_DONE,
    ASYNC_IO_TEST_STATE_WRITE2_SUBMITTED,
    ASYNC_IO_TEST_STATE_READ2_SUBMITTED,
    ASYNC_IO_TEST_STATE_VERIFY2_DONE,
    ASYNC_IO_TEST_STATE_ALL_COMPLETED, // All ops in sequence done
    ASYNC_IO_TEST_STATE_FAILED
} xsan_async_io_test_phase_t;

typedef struct {
    xsan_volume_manager_t *vm;
    xsan_disk_manager_t *dm;
    xsan_volume_id_t volume_id_to_test;
    char target_bdev_name_for_log[XSAN_MAX_NAME_LEN];
    uint32_t io_block_size;
    size_t dma_alignment;
    void *write_buffer_dma1;  // Buffer for first write
    void *read_buffer_dma1;  // Buffer for first read
    void *write_buffer_dma2; // Buffer for second write
    void *read_buffer_dma2;  // Buffer for second read

    xsan_async_io_test_phase_t current_phase;
    int outstanding_io_ops;
    bool test_finished_signal;
    int test_run_count; // To make write patterns different
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


// Forward declarations
static void _start_async_io_test_on_volume(xsan_async_io_test_control_t *test_ctrl, xsan_volume_t *vol_to_test);
static void _async_io_test_phase_complete_cb(void *cb_arg, xsan_error_t status);
static void _finalize_async_io_test_sequence(xsan_async_io_test_control_t *test_ctrl);
static void _replica_response_send_complete_cb(int status, void *cb_arg);
static void _replica_request_local_write_done_cb(void *cb_arg, xsan_error_t local_write_status);


static void _finalize_async_io_test_sequence(xsan_async_io_test_control_t *test_ctrl) {
    if (test_ctrl->outstanding_io_ops == 0) {
        char vol_id_str[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
        XSAN_LOG_INFO("[AsyncIOTest] Sequence for volume %s fully finished, final phase: %d.", vol_id_str, test_ctrl->current_phase);

        if(test_ctrl->write_buffer_dma1) xsan_bdev_dma_free(test_ctrl->write_buffer_dma1); test_ctrl->write_buffer_dma1 = NULL;
        if(test_ctrl->read_buffer_dma1) xsan_bdev_dma_free(test_ctrl->read_buffer_dma1); test_ctrl->read_buffer_dma1 = NULL;
        if(test_ctrl->write_buffer_dma2) xsan_bdev_dma_free(test_ctrl->write_buffer_dma2); test_ctrl->write_buffer_dma2 = NULL;
        if(test_ctrl->read_buffer_dma2) xsan_bdev_dma_free(test_ctrl->read_buffer_dma2); test_ctrl->read_buffer_dma2 = NULL;

        test_ctrl->test_finished_signal = true;
    }
}

static void _async_io_test_phase_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *test_ctrl = (xsan_async_io_test_control_t *)cb_arg;
    test_ctrl->outstanding_io_ops--;
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);

    xsan_async_io_test_phase_t prev_phase = test_ctrl->current_phase;
    xsan_error_t submit_err = XSAN_OK;

    if (status != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncIOTest] Phase %d for vol %s FAILED: %s (code %d)", prev_phase, vol_id_str, xsan_error_string(status), status);
        test_ctrl->current_phase = ASYNC_IO_TEST_STATE_FAILED;
        _finalize_async_io_test_sequence(test_ctrl);
        return;
    }

    XSAN_LOG_INFO("[AsyncIOTest] Phase %d for vol %s successful.", prev_phase, vol_id_str);

    switch (prev_phase) {
        case ASYNC_IO_TEST_STATE_WRITE1_SUBMITTED: // Write1 done, start Read1
            test_ctrl->current_phase = ASYNC_IO_TEST_STATE_READ1_SUBMITTED;
            test_ctrl->outstanding_io_ops++;
            submit_err = xsan_volume_read_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->read_buffer_dma1, _async_io_test_phase_complete_cb, test_ctrl);
            break;
        case ASYNC_IO_TEST_STATE_READ1_SUBMITTED: // Read1 done, verify1 then start Write2
            if (memcmp(test_ctrl->write_buffer_dma1, test_ctrl->read_buffer_dma1, test_ctrl->io_block_size) == 0) {
                XSAN_LOG_INFO("SUCCESS: [AsyncIOTest] R/W1 data verification for vol %s PASSED!", vol_id_str);
                test_ctrl->current_phase = ASYNC_IO_TEST_STATE_VERIFY1_DONE;
                // Start Write2
                test_ctrl->current_phase = ASYNC_IO_TEST_STATE_WRITE2_SUBMITTED;
                test_ctrl->outstanding_io_ops++;
                snprintf((char*)test_ctrl->write_buffer_dma2, test_ctrl->io_block_size, "XSAN Async Test RUN 2! Vol %s", vol_id_str);
                submit_err = xsan_volume_write_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->write_buffer_dma2, _async_io_test_phase_complete_cb, test_ctrl);
            } else {
                XSAN_LOG_ERROR("FAILURE: [AsyncIOTest] R/W1 data verification for vol %s FAILED!", vol_id_str);
                test_ctrl->current_phase = ASYNC_IO_TEST_STATE_FAILED;
            }
            break;
        case ASYNC_IO_TEST_STATE_WRITE2_SUBMITTED: // Write2 done, start Read2
            test_ctrl->current_phase = ASYNC_IO_TEST_STATE_READ2_SUBMITTED;
            test_ctrl->outstanding_io_ops++;
            submit_err = xsan_volume_read_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->read_buffer_dma2, _async_io_test_phase_complete_cb, test_ctrl);
            break;
        case ASYNC_IO_TEST_STATE_READ2_SUBMITTED: // Read2 done, verify2
             if (memcmp(test_ctrl->write_buffer_dma2, test_ctrl->read_buffer_dma2, test_ctrl->io_block_size) == 0) {
                XSAN_LOG_INFO("SUCCESS: [AsyncIOTest] R/W2 data verification for vol %s PASSED!", vol_id_str);
                test_ctrl->current_phase = ASYNC_IO_TEST_STATE_VERIFY2_DONE;
            } else {
                XSAN_LOG_ERROR("FAILURE: [AsyncIOTest] R/W2 data verification for vol %s FAILED!", vol_id_str);
                test_ctrl->current_phase = ASYNC_IO_TEST_STATE_FAILED;
            }
            break;
        default: // Includes VERIFY1_DONE (if write2 failed submit), VERIFY2_DONE, FAILED
            break;
    }
    if (submit_err != XSAN_OK &&
        (prev_phase == ASYNC_IO_TEST_STATE_WRITE1_SUBMITTED || prev_phase == ASYNC_IO_TEST_STATE_READ1_SUBMITTED || prev_phase == ASYNC_IO_TEST_STATE_WRITE2_SUBMITTED)) {
        XSAN_LOG_ERROR("[AsyncIOTest] Failed to submit next phase %d for vol %s: %s", test_ctrl->current_phase, vol_id_str, xsan_error_string(submit_err));
        test_ctrl->outstanding_io_ops--;
        test_ctrl->current_phase = ASYNC_IO_TEST_STATE_FAILED;
    }
    _finalize_async_io_test_sequence(test_ctrl);
}

static void _start_async_io_test_on_volume(xsan_async_io_test_control_t *test_ctrl, xsan_volume_t *vol_to_test) {
    if (!vol_to_test || vol_to_test->block_size_bytes == 0 || vol_to_test->num_blocks == 0) { _finalize_async_io_test_sequence(test_ctrl); return; }
    memcpy(&test_ctrl->volume_id_to_test, &vol_to_test->id, sizeof(xsan_volume_id_t)); test_ctrl->io_block_size = vol_to_test->block_size_bytes;
    xsan_disk_group_t *g=xsan_disk_manager_find_disk_group_by_id(test_ctrl->dm,vol_to_test->source_group_id); if (!g || g->disk_count == 0) {_finalize_async_io_test_sequence(test_ctrl); return; }
    xsan_disk_t *d0=xsan_disk_manager_find_disk_by_id(test_ctrl->dm,g->disk_ids[0]); if (!d0) {_finalize_async_io_test_sequence(test_ctrl); return; }
    test_ctrl->dma_alignment=xsan_bdev_get_buf_align(d0->bdev_name);

    test_ctrl->write_buffer_dma1 = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);
    test_ctrl->read_buffer_dma1 = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);
    test_ctrl->write_buffer_dma2 = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);
    test_ctrl->read_buffer_dma2 = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);

    if (!test_ctrl->write_buffer_dma1 || !test_ctrl->read_buffer_dma1 || !test_ctrl->write_buffer_dma2 || !test_ctrl->read_buffer_dma2) {
        /* free any allocated, finalize */ _finalize_async_io_test_sequence(test_ctrl); return;
    }
    char vol_id_str[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    snprintf((char*)test_ctrl->write_buffer_dma1, test_ctrl->io_block_size, "XSAN Async Test RUN 1! Vol %s", vol_id_str);
    for(size_t k=strlen((char*)test_ctrl->write_buffer_dma1); k < test_ctrl->io_block_size; ++k) { ((char*)test_ctrl->write_buffer_dma1)[k] = (char)((k % 250) + 5); }
    memset(test_ctrl->read_buffer_dma1, 0xDD, test_ctrl->io_block_size);
    memset(test_ctrl->read_buffer_dma2, 0xEE, test_ctrl->io_block_size); // Prepare for second read

    XSAN_LOG_INFO("--- Starting Async R/W Test Sequence on Volume ID %s (FTT: %u) ---", vol_id_str, vol_to_test->FTT);

    test_ctrl->current_phase = ASYNC_IO_TEST_STATE_WRITE1_SUBMITTED; test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_write_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->write_buffer_dma1, _async_io_test_phase_complete_cb, test_ctrl);
    if (err != XSAN_OK) {
        test_ctrl->outstanding_io_ops--;test_ctrl->current_phase = ASYNC_IO_TEST_STATE_FAILED; _finalize_async_io_test_sequence(test_ctrl);
    }
}

// --- Node Comm Test Callbacks & State --- (Simplified, as before)
static void _comm_test_send_ping_cb(int status, void *cb_arg) { /* ... */ xsan_comm_test_state_t*cs=cb_arg;if(status==0)cs->ping_sent_successfully=true;else cs->test_finished_signal=true;}
static void _comm_test_connect_cb(struct spdk_sock *sock, int status, void *cb_arg) { /* ... */
    xsan_comm_test_state_t*cs=cb_arg; if(status==0){cs->client_sock_to_self=sock;cs->connected_to_self=true;
    xsan_message_t*m=xsan_protocol_message_create(XSAN_MSG_TYPE_HEARTBEAT,1,"SELF_PING",10); if(m){if(xsan_node_comm_send_msg(sock,m,_comm_test_send_ping_cb,cs)!=XSAN_OK)cs->test_finished_signal=true;xsan_protocol_message_destroy(m);}else cs->test_finished_signal=true;}else cs->test_finished_signal=true;
}
static void _replica_response_send_complete_cb(int status, void *cb_arg) { /* ... */ xsan_replica_request_handler_ctx_t*h=cb_arg;if(!h)return;if(h->data_to_write_dma)xsan_bdev_dma_free(h->data_to_write_dma);XSAN_FREE(h);}
static void _replica_request_local_write_done_cb(void *cb_arg, xsan_error_t status) { /* ... */
    xsan_replica_request_handler_ctx_t*h=cb_arg;if(!h)return;
    xsan_replica_write_resp_payload_t pl;pl.status=status;pl.block_lba_on_volume=h->original_req_struct_payload.block_lba_on_volume;pl.num_blocks_processed=(status==XSAN_OK)?h->original_req_struct_payload.num_blocks:0;
    xsan_message_t*m=xsan_protocol_message_create(XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP,h->original_req_header.transaction_id,&pl,sizeof(pl));
    if(m){if(xsan_node_comm_send_msg(h->originating_sock,m,_replica_response_send_complete_cb,h)!=XSAN_OK){if(h->data_to_write_dma)xsan_bdev_dma_free(h->data_to_write_dma);XSAN_FREE(h);}} else {if(h->data_to_write_dma)xsan_bdev_dma_free(h->data_to_write_dma);XSAN_FREE(h);}
    if(m)xsan_protocol_message_destroy(m);
}
static void _xsan_node_test_message_handler(struct spdk_sock *sock, const char *peer, xsan_message_t *msg, void *cb_arg) { /* ... as before, calls process_replica_write_response ... */
    xsan_volume_manager_t *vm = cb_arg; if(!vm){xsan_protocol_message_destroy(msg);return;}
    if (msg->header.type == XSAN_MSG_TYPE_HEARTBEAT && msg->payload && strcmp((char*)msg->payload, "SELF_PING_PAYLOAD") == 0) {
        g_comm_test_controller.pong_received_or_ping_handled = true; if (!g_comm_test_controller.test_finished_signal) g_comm_test_controller.test_finished_signal = true;
    } else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ) { /* ... as before, calls xsan_volume_write_async for local replica write ... */ }
    else if (msg->header.type == XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP) {
        xsan_replica_write_resp_payload_t *resp_pl = (xsan_replica_write_resp_payload_t*)msg->payload;
        xsan_node_id_t placeholder_id; memset(&placeholder_id,0,sizeof(placeholder_id)); // TODO: Get actual responder
        xsan_volume_manager_process_replica_write_response(vm, msg->header.transaction_id, placeholder_id, resp_pl->status);
    }
    xsan_protocol_message_destroy(msg);
}

static void xsan_node_main_spdk_thread_start(void *arg1, int rc) {
    // ... (Initializations as before: dm, vm, node_comm, vhost_subsystem_init) ...
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
    if (xsan_disk_manager_scan_and_register_bdevs(dm) != XSAN_OK) { /* Log, continue */ }

    // ... (Test DG and Vol creation for "MetaTestVolRep" with FTT=1 as before) ...
    xsan_group_id_t test_dg_id; memset(&test_dg_id, 0, sizeof(test_dg_id)); bool dg_created = false;
    xsan_volume_id_t test_vol_id; memset(&test_vol_id, 0, sizeof(test_vol_id)); bool vol_created = false;
    xsan_volume_t *vol_for_async_test = NULL; xsan_disk_t *disk_for_vol_group = NULL;
    xsan_disk_t **disks_list = NULL; int disk_count = 0;
    xsan_disk_manager_get_all_disks(dm, &disks_list, &disk_count);
    if (disk_count > 0 && disks_list) disk_for_vol_group = disks_list[0];
    if(disk_for_vol_group){ if (xsan_disk_manager_find_disk_group_by_name(dm, "MetaTestDG") == NULL) { const char *b[]={disk_for_vol_group->bdev_name}; if(xsan_disk_manager_disk_group_create(dm,"MetaTestDG",XSAN_DISK_GROUP_TYPE_PASSSTHROUGH,b,1,&test_dg_id)==XSAN_OK)dg_created=true;} else {xsan_disk_group_t*ex_dg=xsan_disk_manager_find_disk_group_by_name(dm,"MetaTestDG");if(ex_dg){memcpy(&test_dg_id,&ex_dg->id,sizeof(test_dg_id));dg_created=true;}}}
    if(disks_list) xsan_disk_manager_free_disk_pointer_list(disks_list);
    if(dg_created){ uint32_t ftt=1; uint64_t vs=(disk_for_vol_group?disk_for_vol_group->capacity_bytes/(1024*1024):64)>128?128:((disk_for_vol_group?disk_for_vol_group->capacity_bytes/(1024*1024):64)/2); if(vs<16)vs=16; uint64_t vsb=vs*1024*1024; uint32_t vbs=4096; if(vsb<vbs)vsb=vbs;
        if(xsan_volume_get_by_name(vm,XSAN_TEST_VBDEV_NAME+5)==NULL){if(xsan_volume_create(vm,XSAN_TEST_VBDEV_NAME+5,vsb,test_dg_id,vbs,false,ftt,&test_vol_id)==XSAN_OK)vol_created=true;}
        else {vol_for_async_test=xsan_volume_get_by_name(vm,XSAN_TEST_VBDEV_NAME+5);if(vol_for_async_test){memcpy(&test_vol_id,&vol_for_async_test->id,sizeof(test_vol_id));vol_created=true;}}}
    vol_for_async_test = xsan_volume_get_by_id(vm, test_vol_id);

    // --- Vhost Expose Test ---
    if (vol_created && vol_for_async_test) {
        snprintf(test_vbdev_name, sizeof(test_vbdev_name), "%s", XSAN_TEST_VBDEV_NAME);
        if (xsan_vhost_expose_volume_as_vbdev(vol_for_async_test->id, test_vbdev_name) == XSAN_OK) {
            vbdev_exposed_flag = true;
            XSAN_LOG_INFO("Exposed vol '%s' as vbdev '%s'. SPDK JSON config should map a LUN to this vbdev on controller '%s' (socket dir: %s)",
                vol_for_async_test->name, test_vbdev_name, XSAN_TEST_VHOST_CTRLR_NAME, XSAN_VHOST_SOCKET_DIR);
        } else { XSAN_LOG_ERROR("Failed to expose vol '%s' as vbdev.", vol_for_async_test->name); }
    } else { XSAN_LOG_WARN("Test volume not available, skipping vhost expose."); }

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
    if (vbdev_exposed_flag) { xsan_vhost_unexpose_volume_vbdev(test_vbdev_name); }
    if (vol_created) { xsan_volume_delete(vm, test_vol_id); }
    if (dg_created && !spdk_uuid_is_null((struct spdk_uuid*)&test_dg_id.data[0])) { xsan_disk_manager_disk_group_delete(dm, test_dg_id); }

app_stop_vhost_fini:
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

int main(int argc, char **argv) { /* ... as before ... */
    xsan_log_config_t log_cfg=xsan_log_default_config(); log_cfg.level=XSAN_LOG_LEVEL_DEBUG; xsan_log_init(&log_cfg);
    XSAN_LOG_INFO("XSAN Node starting..."); const char *spdk_conf=NULL;
    if(argc>1){for(int i=1;i<argc;++i){if(strcmp(argv[i],"--spdk-conf")==0&&i+1<argc){spdk_conf=argv[++i];break;}else if(i==1&&argv[i][0]!='-'){spdk_conf=argv[i];}}}
    if(spdk_conf)XSAN_LOG_INFO("Using SPDK JSON conf: %s",spdk_conf);else XSAN_LOG_WARN("No SPDK JSON conf.");
    mkdir("./xsan_meta_db",0755); mkdir("./xsan_meta_db/disk_manager_db",0755); mkdir("./xsan_meta_db/volume_manager_db",0755);
    if(mkdir(XSAN_VHOST_SOCKET_DIR,0777)!=0&&errno!=EEXIST)XSAN_LOG_WARN("Failed to create vhost socket dir %s: %s",XSAN_VHOST_SOCKET_DIR,strerror(errno));
    xsan_error_t err=xsan_spdk_manager_opts_init("xsan_node_main",spdk_conf,"0x1",false,NULL);
    if(err!=XSAN_OK){XSAN_LOG_FATAL("SPDK opts init failed:%s",xsan_error_string(err));xsan_log_shutdown();return EXIT_FAILURE;}
    err=xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start,NULL);
    if(err!=XSAN_OK)XSAN_LOG_FATAL("xsan_spdk_manager_start_app error:%s",xsan_error_string(err));
    // xsan_node_comm_fini(); // This is now called from xsan_node_main_spdk_thread_start's cleanup path
    // xsan_vhost_subsystem_fini(); // Also called from cleanup path
    xsan_spdk_manager_app_fini();
    XSAN_LOG_INFO("XSAN Node shut down."); xsan_log_shutdown();
    return (err==XSAN_OK)?EXIT_SUCCESS:EXIT_FAILURE;
}
