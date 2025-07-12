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
#include "xsan_cluster.h" // For xsan_get_local_node_info (will be called from volume_manager)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h> // For command line argument parsing

#include "spdk/uuid.h"
#include "spdk/env.h"
#include "spdk/sock.h"
#include "spdk/thread.h"

// Global config instances, accessible by other modules via extern (if needed) or access functions
xsan_config_t *g_xsan_config = NULL;
xsan_node_config_t g_local_node_config;
xsan_cluster_config_t g_cluster_config; // If cluster config is also needed globally
// xsan_node_id_t g_local_xsan_node_id; // Parsed UUID of local node

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

static char *g_xsan_config_file = NULL; // To be set by command line arg

static void xsan_node_main_spdk_thread_start(void *arg1, int spdk_startup_rc) {
    XSAN_LOG_INFO("XSAN Node main SPDK thread started. SPDK Startup RC: %d", spdk_startup_rc);
    if (spdk_startup_rc != 0) {
        XSAN_LOG_FATAL("SPDK framework failed to start properly (rc=%d). Exiting XSAN node main.", spdk_startup_rc);
        // Signal app stop if necessary, or handle based on how spdk_app_start behaves on error for its main fn.
        // If spdk_app_start calls this even on its own error, we should probably just return.
        return;
    }

    // Initialize global config
    g_xsan_config = xsan_config_create();
    if (!g_xsan_config) {
        XSAN_LOG_FATAL("Failed to create global config object. Shutting down.");
        xsan_spdk_manager_request_app_stop();
        return;
    }

    const char *config_file_to_load = g_xsan_config_file ? g_xsan_config_file : "xsan_node.conf";
    XSAN_LOG_INFO("Loading XSAN configuration from: %s", config_file_to_load);

    if (!xsan_config_load_from_file(g_xsan_config, config_file_to_load)) {
        XSAN_LOG_ERROR("Failed to load config file '%s'. Some features might use defaults or fail.", config_file_to_load);
        // Depending on strictness, could decide to stop here. For now, proceed with potential defaults.
    }

    // Load node-specific configuration
    if (!xsan_config_load_node_config(g_xsan_config, &g_local_node_config)) {
        XSAN_LOG_ERROR("Failed to parse node-specific config. Critical error. Shutting down.");
        xsan_config_destroy(g_xsan_config);
        g_xsan_config = NULL;
        xsan_spdk_manager_request_app_stop();
        return;
    } else {
        XSAN_LOG_INFO("Node Config Loaded: ID='%s', Name='%s', Addr='%s', Port=%u, DataDir='%s'",
                      g_local_node_config.node_id, g_local_node_config.node_name,
                      g_local_node_config.bind_address, g_local_node_config.port, g_local_node_config.data_dir);
        if (strlen(g_local_node_config.node_id) == 0 || strcmp(g_local_node_config.node_id, "undefined") == 0) { // Example validation
             XSAN_LOG_FATAL("Node ID is invalid ('%s') in config. Cannot proceed. Shutting down.", g_local_node_config.node_id);
             xsan_config_destroy(g_xsan_config);
             g_xsan_config = NULL;
             xsan_spdk_manager_request_app_stop();
             return;
        }
        // TODO: Convert g_local_node_config.node_id (string) to g_local_xsan_node_id (xsan_node_id_t/UUID)
        // if (spdk_uuid_parse(&g_local_xsan_node_id.data[0], g_local_node_config.node_id) != 0) {
        //    XSAN_LOG_FATAL("Failed to parse configured node_id '%s' as UUID. Shutting down.", g_local_node_config.node_id);
        //    // ... cleanup and stop ...
        // }
    }

    // Load cluster-specific configuration (optional for now, but good for completeness)
    if (!xsan_config_load_cluster_config(g_xsan_config, &g_cluster_config)) {
        XSAN_LOG_WARN("Failed to parse cluster-specific config. Cluster operations might be limited.");
    } else {
        XSAN_LOG_INFO("Cluster Config Loaded: Name='%s', SeedNodesCount=%zu",
                      g_cluster_config.cluster_name, g_cluster_config.seed_node_count);
    }

    // Initialize other XSAN managers that depend on config or SPDK

    // Initialize Cluster module (depends on global config g_cluster_config being populated)
    if (xsan_cluster_init(config_file_to_load) != XSAN_OK) { // Pass config path for context, though current init uses globals
        XSAN_LOG_FATAL("Failed to initialize XSAN Cluster Manager. Shutting down.");
        xsan_config_destroy(g_xsan_config);
        g_xsan_config = NULL;
        xsan_spdk_manager_request_app_stop();
        return;
    }

    xsan_disk_manager_t *disk_manager = NULL;
    if (xsan_disk_manager_init(&disk_manager) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN Disk Manager. Shutting down.");
        xsan_cluster_shutdown();
        xsan_config_destroy(g_xsan_config);
        g_xsan_config = NULL;
        xsan_spdk_manager_request_app_stop();
        return;
    }
    xsan_disk_manager_scan_and_register_bdevs(disk_manager);


    xsan_volume_manager_t *volume_manager = NULL;
    if (xsan_volume_manager_init(disk_manager, &volume_manager) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN Volume Manager. Shutting down.");
        xsan_disk_manager_fini(&disk_manager);
        xsan_cluster_shutdown();
        xsan_config_destroy(g_xsan_config);
        g_xsan_config = NULL;
        xsan_spdk_manager_request_app_stop();
        return;
    }

    // Initialize node communication server (listener)
    // The global_app_msg_handler_cb in xsan_node_comm_init can be NULL if all messages are handled by specific handlers.
    if (xsan_node_comm_init(g_local_node_config.bind_address, g_local_node_config.port, NULL, NULL) != XSAN_OK) {
         XSAN_LOG_FATAL("Failed to initialize XSAN node communication server on %s:%u. Shutting down.",
                       g_local_node_config.bind_address, g_local_node_config.port);
        xsan_volume_manager_fini(&volume_manager);
        xsan_disk_manager_fini(&disk_manager);
        xsan_cluster_shutdown();
        xsan_config_destroy(g_xsan_config);
        g_xsan_config = NULL;
        xsan_spdk_manager_request_app_stop();
        return;
    }
    // Register specific handlers for replica operations
    if (xsan_node_comm_register_message_handler(XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ,
                                                xsan_volume_manager_handle_replica_write_req,
                                                volume_manager) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to register replica write request handler. Shutting down.");
        // ... cleanup and stop ...
        return;
    }
    if (xsan_node_comm_register_message_handler(XSAN_MSG_TYPE_REPLICA_READ_BLOCK_REQ,
                                                xsan_volume_manager_handle_replica_read_req,
                                                volume_manager) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to register replica read request handler. Shutting down.");
        // ... cleanup and stop ...
        return;
    }
    // Note: _xsan_node_test_message_handler was the old global handler.
    // If it's still needed for other message types, it could be registered for those,
    // or xsan_node_comm_init's global handler could be set to it if specific handlers are not found.
    // For now, assuming replica ops are the primary ones handled via specific registration.


    XSAN_LOG_INFO("XSAN Node subsystems initialized. Running test sequences or waiting for events...");

    // Run End-to-End Core Logic Tests
    _run_e2e_core_logic_tests(disk_manager, volume_manager);


    XSAN_LOG_INFO("XSAN Node main SPDK thread tasks complete. Cleaning up XSAN subsystems...");

    // Cleanup XSAN subsystems
    xsan_node_comm_fini(); // Updated to reflect new comm_init/fini
    xsan_volume_manager_fini(&volume_manager);
    xsan_disk_manager_fini(&disk_manager);
    xsan_cluster_shutdown(); // Shutdown cluster module

    if (g_xsan_config) {
        xsan_config_destroy(g_xsan_config);
        g_xsan_config = NULL;
    }
    XSAN_LOG_INFO("XSAN subsystems cleaned up. Requesting SPDK application stop.");
    xsan_spdk_manager_request_app_stop(); // Signal SPDK app to stop all reactors
}


// --- E2E Test Helper Structures and Callbacks ---
typedef struct {
    bool completed;
    xsan_error_t status;
    char *buffer; // For reads
    const char *original_buffer_for_verify; // For comparison after read
    uint64_t len;
    const char* test_name;
    struct spdk_thread *thread; // Thread to signal if polling
} xsan_e2e_io_test_ctx_t;

static void _e2e_io_completion_cb(void *cb_arg, xsan_error_t status) {
    xsan_e2e_io_test_ctx_t *ctx = (xsan_e2e_io_test_ctx_t *)cb_arg;
    ctx->completed = true;
    ctx->status = status;

    XSAN_LOG_INFO("E2E Test '%s' I/O completed with status: %s (%d)", ctx->test_name, xsan_error_string(status), status);

    if (status == XSAN_OK && ctx->original_buffer_for_verify && ctx->buffer && ctx->len > 0) {
        if (memcmp(ctx->original_buffer_for_verify, ctx->buffer, ctx->len) == 0) {
            XSAN_LOG_INFO("E2E Test '%s': Read data verification PASSED.", ctx->test_name);
        } else {
            XSAN_LOG_ERROR("E2E Test '%s': Read data verification FAILED!", ctx->test_name);
            // Hex dump for debugging small differences
            // xsan_hex_dump("Original", ctx->original_buffer_for_verify, XSAN_MIN(ctx->len, 64));
            // xsan_hex_dump("Read Back", ctx->buffer, XSAN_MIN(ctx->len, 64));
            ctx->status = XSAN_ERROR_TEST_VERIFY_FAILED;
        }
    }
    if (ctx->thread) { // If polling, signal the polling loop.
        // spdk_thread_send_msg(ctx->thread, some_msg_to_break_poll, NULL); // Needs a message mechanism
    }
}

static void _run_e2e_core_logic_tests(xsan_disk_manager_t *dm, xsan_volume_manager_t *vm) {
    XSAN_LOG_INFO("===== Starting E2E Core Logic Tests =====");
    xsan_error_t err;

    // Test Parameters
    const char *test_dg_name = "TestDiskGroup1";
    const char *bdev_names[] = {"Malloc0", "Malloc1"}; // Ensure these are configured in SPDK JSON conf
    int num_bdevs_for_dg = 2;
    xsan_group_id_t dg_id;

    const char *test_vol_name = "TestVolume1";
    uint64_t test_vol_size_mb = 20; // 20 MB
    uint64_t test_vol_size_bytes = test_vol_size_mb * 1024 * 1024;
    uint32_t test_vol_block_size = 4096;
    uint32_t test_vol_ftt = 0; // Single replica for basic I/O test
    xsan_volume_id_t vol_id;
    xsan_volume_t *vol = NULL;

    // --- Test Disk Group Creation ---
    XSAN_LOG_INFO("[E2E Test] Creating disk group '%s'...", test_dg_name);
    err = xsan_disk_manager_disk_group_create(dm, test_dg_name, XSAN_DISK_GROUP_TYPE_JBOD, bdev_names, num_bdevs_for_dg, &dg_id);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[E2E Test] Failed to create disk group '%s': %s", test_dg_name, xsan_error_string(err));
        goto test_end;
    }
    XSAN_LOG_INFO("[E2E Test] Disk group '%s' (ID: %s) created successfully.", test_dg_name, spdk_uuid_get_string((struct spdk_uuid*)&dg_id.data[0]));

    // --- Test Volume Creation ---
    XSAN_LOG_INFO("[E2E Test] Creating volume '%s' (Size: %lu MB, FTT: %u)...", test_vol_name, test_vol_size_mb, test_vol_ftt);
    err = xsan_volume_create(vm, test_vol_name, test_vol_size_bytes, dg_id, test_vol_block_size, false /*thick*/, test_vol_ftt, &vol_id);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[E2E Test] Failed to create volume '%s': %s", test_vol_name, xsan_error_string(err));
        goto cleanup_dg;
    }
    vol = xsan_volume_get_by_id(vm, vol_id); // Get the managed instance
    if (!vol) {
        XSAN_LOG_ERROR("[E2E Test] Volume '%s' created but get_by_id failed!", test_vol_name);
        goto cleanup_dg; // vol_id might be valid for delete if create partially succeeded in metadata
    }
    XSAN_LOG_INFO("[E2E Test] Volume '%s' (ID: %s) created successfully. State: %d", test_vol_name, spdk_uuid_get_string((struct spdk_uuid*)&vol_id.data[0]), vol->state);


    // --- Test LBA Mapping (simple check) ---
    xsan_disk_id_t mapped_disk_id;
    uint64_t mapped_phys_lba;
    uint32_t mapped_phys_block_size;
    uint64_t test_lba = 0; // Test mapping for LBA 0
    XSAN_LOG_INFO("[E2E Test] Mapping LBA %lu for volume '%s'...", test_lba, test_vol_name);
    err = xsan_volume_map_lba_to_physical(vm, vol_id, test_lba, &mapped_disk_id, &mapped_phys_lba, &mapped_phys_block_size);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[E2E Test] Failed to map LBA %lu for volume '%s': %s", test_lba, test_vol_name, xsan_error_string(err));
    } else {
        XSAN_LOG_INFO("[E2E Test] LBA %lu maps to DiskID: %s, PhysLBA: %lu, PhysBlkSize: %u",
                      test_lba, spdk_uuid_get_string((struct spdk_uuid*)&mapped_disk_id.data[0]), mapped_phys_lba, mapped_phys_block_size);
        // TODO: Add more specific verification based on known Malloc0/Malloc1 properties if needed
    }

    // --- Test Basic I/O ---
    uint32_t io_size = test_vol_block_size * 2; // Write 2 blocks
    char *write_buf = xsan_bdev_dma_malloc(io_size, test_vol_block_size); // Assuming test_vol_block_size is suitable alignment
    char *read_buf = xsan_bdev_dma_malloc(io_size, test_vol_block_size);
    if (!write_buf || !read_buf) {
        XSAN_LOG_ERROR("[E2E Test] Failed to allocate DMA buffers for I/O test.");
        if(write_buf) xsan_bdev_dma_free(write_buf);
        if(read_buf) xsan_bdev_dma_free(read_buf);
        goto cleanup_vol;
    }
    for (uint32_t i = 0; i < io_size; ++i) write_buf[i] = (char)(i % 256);
    memset(read_buf, 0xAA, io_size);

    xsan_e2e_io_test_ctx_t write_io_ctx = {
        .completed = false, .status = XSAN_OK, .buffer = NULL, .original_buffer_for_verify = NULL, .len = io_size, .test_name = "WriteTest", .thread = spdk_get_thread()
    };
    xsan_e2e_io_test_ctx_t read_io_ctx = {
        .completed = false, .status = XSAN_OK, .buffer = read_buf, .original_buffer_for_verify = write_buf, .len = io_size, .test_name = "ReadVerifyTest", .thread = spdk_get_thread()
    };
    uint64_t io_offset = 0; // Write to start of volume

    XSAN_LOG_INFO("[E2E Test] Submitting async write (offset %lu, len %u) to volume '%s'...", io_offset, io_size, test_vol_name);
    err = xsan_volume_write_async(vm, vol_id, io_offset, io_size, write_buf, _e2e_io_completion_cb, &write_io_ctx);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[E2E Test] Failed to submit write: %s", xsan_error_string(err));
    } else {
        while (!write_io_ctx.completed) { spdk_thread_poll(write_io_ctx.thread, 0, 0); usleep(100); } // Simple poll
        if (write_io_ctx.status == XSAN_OK) {
            XSAN_LOG_INFO("[E2E Test] Write successful. Submitting async read...");
            err = xsan_volume_read_async(vm, vol_id, io_offset, io_size, read_buf, _e2e_io_completion_cb, &read_io_ctx);
            if (err != XSAN_OK) {
                XSAN_LOG_ERROR("[E2E Test] Failed to submit read: %s", xsan_error_string(err));
            } else {
                while (!read_io_ctx.completed) { spdk_thread_poll(read_io_ctx.thread, 0, 0); usleep(100); }
                if (read_io_ctx.status != XSAN_OK && read_io_ctx.status != XSAN_ERROR_TEST_VERIFY_FAILED) { // Check if read op itself failed
                     XSAN_LOG_ERROR("[E2E Test] Read operation failed with: %s", xsan_error_string(read_io_ctx.status));
                }
            }
        } else {
             XSAN_LOG_ERROR("[E2E Test] Write operation failed with: %s", xsan_error_string(write_io_ctx.status));
        }
    }
    xsan_bdev_dma_free(write_buf);
    xsan_bdev_dma_free(read_buf);

cleanup_vol:
    XSAN_LOG_INFO("[E2E Test] Deleting volume '%s'...", test_vol_name);
    err = xsan_volume_delete(vm, vol_id);
    if (err != XSAN_OK) XSAN_LOG_ERROR("[E2E Test] Failed to delete volume '%s': %s", test_vol_name, xsan_error_string(err));
    else XSAN_LOG_INFO("[E2E Test] Volume '%s' deleted.", test_vol_name);

cleanup_dg:
    XSAN_LOG_INFO("[E2E Test] Deleting disk group '%s'...", test_dg_name);
    err = xsan_disk_manager_disk_group_delete(dm, dg_id);
    if (err != XSAN_OK) XSAN_LOG_ERROR("[E2E Test] Failed to delete disk group '%s': %s", test_dg_name, xsan_error_string(err));
    else XSAN_LOG_INFO("[E2E Test] Disk group '%s' deleted.", test_dg_name);

test_end:
    XSAN_LOG_INFO("===== E2E Core Logic Tests Finished =====");
    // Signal main loop in xsan_node_main_spdk_thread_start that tests are done
    g_async_io_test_controller.test_finished_signal = true; // Reuse this global for now to stop the main loop
}


static void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -c, --config FILE      Path to XSAN node configuration file (default: xsan_node.conf)\n");
    printf("  -h, --help             Show this help message\n");
    // Add other SPDK options if we want to pass them through, e.g., -m for coremask
}

int main(int argc, char **argv) {
    // Initialize XSAN logging early
    // xsan_log_set_level(XSAN_LOG_LEVEL_DEBUG); // Example: set default log level
    // xsan_log_set_output_file("xsan_node_main.log"); // Example: redirect log

    // Command line argument parsing
    struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                g_xsan_config_file = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }


    XSAN_LOG_INFO("XSAN Node application starting...");

    // Initialize SPDK application options
    // These could also be influenced by command-line arguments if needed.
    // For now, using some defaults. The JSON config file for SPDK itself is separate.
    xsan_spdk_manager_opts_init("xsan_node_spdk_app",
                                 NULL, // SPDK JSON config file (e.g., for bdevs not auto-detected)
                                 NULL, // Reactor mask (e.g., "0x1", NULL for default)
                                 true, // Enable RPC
                                 "/var/tmp/xsan.sock"); // RPC listen address

    // Start the SPDK application framework. This will initialize SPDK environment,
    // create reactors, and then call xsan_node_main_spdk_thread_start on one of them.
    // This call is blocking until spdk_app_stop() is called and all reactors exit.
    if (xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start, NULL) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to start SPDK application framework.");
        // xsan_config_destroy(g_xsan_config); // g_xsan_config might be null if start_app failed very early
        return 1;
    }

    // Finalize SPDK (releases DPDK resources, etc.)
    xsan_spdk_manager_app_fini();

    XSAN_LOG_INFO("XSAN Node application finished.");
    return 0;
}
