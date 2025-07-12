#include "xsan_log.h"
#include "xsan_config.h"
#include "xsan_spdk_manager.h"
#include "xsan_bdev.h"
#include "xsan_disk_manager.h"
#include "xsan_volume_manager.h"
#include "xsan_io.h"
#include "xsan_node_comm.h"
#include "xsan_vhost.h" // Included, though not actively used in current E2E test
#include "xsan_error.h"
#include "xsan_string_utils.h"
#include "xsan_cluster.h"
#include "xsan_nvmf_target.h"
#include "xsan_metadata_store.h" // For md_store access in e2e test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep, usleep
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>

#include "spdk/uuid.h"
#include "spdk/env.h"
#include "spdk/sock.h"
#include "spdk/thread.h"

// Global config instances
xsan_config_t *g_xsan_config = NULL;
xsan_node_config_t g_local_node_config;
xsan_cluster_config_t g_cluster_config;
static char *g_xsan_config_file = NULL;

// Test-related globals (from previous state, might be refactored/removed later)
static bool g_simulate_replica_local_write_failure = false;
typedef enum {
    ASYNC_IO_TEST_PHASE_IDLE, ASYNC_IO_TEST_PHASE_WRITE1_SUBMITTED, ASYNC_IO_TEST_PHASE_READ1_SUBMITTED,
    ASYNC_IO_TEST_PHASE_VERIFY1_DONE, ASYNC_IO_TEST_PHASE_WRITE2_FAILED_REPLICA_SUBMITTED,
    ASYNC_IO_TEST_PHASE_READ2_AFTER_FAILED_REPLICA_SUBMITTED, ASYNC_IO_TEST_PHASE_VERIFY2_DONE,
    ASYNC_IO_TEST_PHASE_ALL_COMPLETED, ASYNC_IO_TEST_PHASE_FAILED
} xsan_async_io_test_phase_t;
typedef struct {
    xsan_volume_manager_t *vm; xsan_disk_manager_t *dm; xsan_volume_id_t volume_id_to_test;
    char target_bdev_name_for_log[XSAN_MAX_NAME_LEN]; uint32_t io_block_size; size_t dma_alignment;
    void *write_buffer_dma1; void *read_buffer_dma1; void *write_buffer_dma2; void *read_buffer_dma2;
    xsan_async_io_test_phase_t current_phase; int outstanding_io_ops; bool test_finished_signal;
} xsan_async_io_test_control_t;
static xsan_async_io_test_control_t g_async_io_test_controller;

// Forward declaration for E2E test function and its helpers
static void _run_e2e_core_logic_tests(xsan_disk_manager_t *dm, xsan_volume_manager_t *vm);
typedef struct {
    bool completed;
    xsan_error_t status;
    char *buffer;
    const char *original_buffer_for_verify;
    uint64_t len;
    const char* test_name;
    struct spdk_thread *thread;
} xsan_e2e_io_test_ctx_t;
static void _e2e_io_completion_cb(void *cb_arg, xsan_error_t status);


// --- Original Test Callbacks & Stubs (might be refactored/removed later if E2E test replaces them) ---
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
    } else { XSAN_LOG_WARN("%s Volume ID %s not found for state logging.", log_prefix, spdk_uuid_get_string((struct spdk_uuid*)&vol_id.data[0]));}
}
static void _finalize_async_io_test_sequence(xsan_async_io_test_control_t *test_ctrl) {
    if (test_ctrl && test_ctrl->outstanding_io_ops == 0 && !test_ctrl->test_finished_signal) {
        XSAN_LOG_INFO("[AsyncIOTest] Sequence finished, final phase: %d.", test_ctrl->current_phase);
        if(test_ctrl->write_buffer_dma1) xsan_bdev_dma_free(test_ctrl->write_buffer_dma1); test_ctrl->write_buffer_dma1=NULL;
        if(test_ctrl->read_buffer_dma1) xsan_bdev_dma_free(test_ctrl->read_buffer_dma1); test_ctrl->read_buffer_dma1=NULL;
        if(test_ctrl->write_buffer_dma2) xsan_bdev_dma_free(test_ctrl->write_buffer_dma2); test_ctrl->write_buffer_dma2=NULL;
        if(test_ctrl->read_buffer_dma2) xsan_bdev_dma_free(test_ctrl->read_buffer_dma2); test_ctrl->read_buffer_dma2=NULL;
        if (!spdk_uuid_is_null((struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0])) {
            _log_volume_and_replica_states(test_ctrl->vm, test_ctrl->volume_id_to_test, "[AsyncIOTestFinalState]");
        }
        test_ctrl->test_finished_signal = true;
    }
}
static void _async_io_test_phase_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *ctx = (xsan_async_io_test_control_t *)cb_arg; ctx->outstanding_io_ops--;
    xsan_error_t submit_err = XSAN_OK; char vol_id_s[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_s, sizeof(vol_id_s), (struct spdk_uuid*)&ctx->volume_id_to_test.data[0]);
    if (status != XSAN_OK && (ctx->current_phase != ASYNC_IO_TEST_PHASE_WRITE2_FAILED_REPLICA_SUBMITTED) &&
        (ctx->current_phase != ASYNC_IO_TEST_PHASE_READ2_AFTER_FAILED_REPLICA_SUBMITTED && status == XSAN_ERROR_NOT_FOUND) ) {
        XSAN_LOG_ERROR("[AsyncIOTest] Phase %d for vol %s FAILED (Status: %d %s)", ctx->current_phase, vol_id_s, status, xsan_error_string(status));
        ctx->current_phase = ASYNC_IO_TEST_PHASE_FAILED; goto end_phase_logic_async_io_test;
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
                for(size_t k=strlen((char*)ctx->write_buffer_dma2); k < ctx->io_block_size; ++k) { ((char*)ctx->write_buffer_dma2)[k] = (char)((k % 240) + 10); }
                submit_err = xsan_volume_write_async(ctx->vm, ctx->volume_id_to_test, 0, ctx->io_block_size, ctx->write_buffer_dma2, _async_io_test_phase_complete_cb, ctx);
            } else { XSAN_LOG_ERROR("FAILURE: [AsyncIOTest] R/W1 data verify for vol %s FAILED!", vol_id_s); ctx->current_phase = ASYNC_IO_TEST_PHASE_FAILED; }
            break;
        case ASYNC_IO_TEST_PHASE_WRITE2_FAILED_REPLICA_SUBMITTED:
            if (status == XSAN_OK) { XSAN_LOG_WARN("[AsyncIOTest] Write2 (FTT=1, one replica simulated fail) reported OVERALL SUCCESS.");}
            else { XSAN_LOG_INFO("[AsyncIOTest] Write2 (FTT=1, one replica simulated fail) reported OVERALL FAILURE as expected: %s", xsan_error_string(status));}
            g_simulate_replica_local_write_failure = false;
            _log_volume_and_replica_states(ctx->vm, ctx->volume_id_to_test, "[AsyncIOTestAfterW2Fail]");
            ctx->current_phase = ASYNC_IO_TEST_PHASE_READ2_AFTER_FAILED_REPLICA_SUBMITTED; ctx->outstanding_io_ops++;
            submit_err = xsan_volume_read_async(ctx->vm, ctx->volume_id_to_test, 0, ctx->io_block_size, ctx->read_buffer_dma2, _async_io_test_phase_complete_cb, ctx);
            break;
        case ASYNC_IO_TEST_PHASE_READ2_AFTER_FAILED_REPLICA_SUBMITTED:
            if (status == XSAN_OK && memcmp(ctx->write_buffer_dma2, ctx->read_buffer_dma2, ctx->io_block_size) == 0) {
                XSAN_LOG_INFO("SUCCESS: [AsyncIOTest] R/W2 data (after simulated replica fail) verify for vol %s PASSED on primary!", vol_id_s);
                ctx->current_phase = ASYNC_IO_TEST_PHASE_VERIFY2_DONE;
            } else { XSAN_LOG_ERROR("FAILURE: [AsyncIOTest] R/W2 data (after simulated replica fail) verify for vol %s FAILED on primary! Read status: %d", vol_id_s, status); ctx->current_phase = ASYNC_IO_TEST_PHASE_FAILED; }
            break;
        default: break;
    }
    if(submit_err!=XSAN_OK && ctx->outstanding_io_ops > 0 && ctx->current_phase != ASYNC_IO_TEST_PHASE_FAILED){
        XSAN_LOG_ERROR("[AsyncIOTest] Failed to submit next phase %d for vol %s: %s", ctx->current_phase, vol_id_s, xsan_error_string(submit_err));
        ctx->outstanding_io_ops--; ctx->current_phase=ASYNC_IO_TEST_PHASE_FAILED;
    }
end_phase_logic_async_io_test: _finalize_async_io_test_sequence(ctx);
}
static void _start_async_io_test_on_volume(xsan_async_io_test_control_t *test_ctrl, xsan_volume_t *vol_to_test) {
    if(!vol_to_test || vol_to_test->block_size_bytes==0){_finalize_async_io_test_sequence(test_ctrl);return;}
    memcpy(&test_ctrl->volume_id_to_test, &vol_to_test->id, sizeof(xsan_volume_id_t)); test_ctrl->io_block_size = vol_to_test->block_size_bytes;
    xsan_disk_group_t *g=xsan_disk_manager_find_disk_group_by_id(test_ctrl->dm,vol_to_test->source_group_id); if (!g || g->disk_count == 0) {_finalize_async_io_test_sequence(test_ctrl); return; }
    xsan_disk_t *d0=xsan_disk_manager_find_disk_by_id(test_ctrl->dm,g->disk_ids[0]); if (!d0 || !d0->bdev_name[0]) {_finalize_async_io_test_sequence(test_ctrl); return; }
    test_ctrl->dma_alignment=xsan_bdev_get_buf_align(d0->bdev_name);
    test_ctrl->write_buffer_dma1=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment); test_ctrl->read_buffer_dma1=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment);
    test_ctrl->write_buffer_dma2=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment); test_ctrl->read_buffer_dma2=xsan_bdev_dma_malloc(test_ctrl->io_block_size,test_ctrl->dma_alignment);
    if(!test_ctrl->write_buffer_dma1||!test_ctrl->read_buffer_dma1||!test_ctrl->write_buffer_dma2||!test_ctrl->read_buffer_dma2){ _finalize_async_io_test_sequence(test_ctrl);return;}
    char vol_id_str[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    snprintf((char*)test_ctrl->write_buffer_dma1, test_ctrl->io_block_size, "XSAN Async Test RUN 1! Vol %s", vol_id_str);
    for(size_t k=strlen((char*)test_ctrl->write_buffer_dma1); k < test_ctrl->io_block_size; ++k) { ((char*)test_ctrl->write_buffer_dma1)[k] = (char)((k % 250) + 5); }
    memset(test_ctrl->read_buffer_dma1, 0xDD, test_ctrl->io_block_size); memset(test_ctrl->read_buffer_dma2, 0xEE, test_ctrl->io_block_size);
    XSAN_LOG_INFO("--- Starting Async R/W Test Sequence on Volume ID %s (FTT: %u) ---", vol_id_str, vol_to_test->FTT);
    _log_volume_and_replica_states(test_ctrl->vm, test_ctrl->volume_id_to_test, "[AsyncIOTestPreW1]");
    test_ctrl->current_phase=ASYNC_IO_TEST_PHASE_WRITE1_SUBMITTED; test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_write_async(test_ctrl->vm,test_ctrl->volume_id_to_test,0,test_ctrl->io_block_size,test_ctrl->write_buffer_dma1,_async_io_test_phase_complete_cb,test_ctrl);
    if(err!=XSAN_OK){ test_ctrl->outstanding_io_ops--;test_ctrl->current_phase=ASYNC_IO_TEST_PHASE_FAILED;_finalize_async_io_test_sequence(test_ctrl); }
}

static void _xsan_node_test_message_handler(struct xsan_connection_ctx *conn_ctx, xsan_message_t *msg, void *cb_arg_vol_mgr) {
    XSAN_LOG_INFO("Test Handler: Received msg type %u from %s", msg->header.type, conn_ctx->peer_addr_str);
    xsan_protocol_message_destroy(msg);
}

// --- E2E Test Helper Structures and Callbacks ---
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
            ctx->status = XSAN_ERROR_TEST_VERIFY_FAILED;
        }
    }
}

static void _run_e2e_core_logic_tests(xsan_disk_manager_t *dm, xsan_volume_manager_t *vm) {
    XSAN_LOG_INFO("===== Starting E2E Core Logic Tests =====");
    xsan_error_t err;
    const char *test_dg_name = "TestE2EDG1";
    const char *bdev_names[] = {"Malloc0", "Malloc1"};
    int num_bdevs_for_dg = (sizeof(bdev_names)/sizeof(bdev_names[0]));
    xsan_group_id_t dg_id = {0}; // Initialize to suppress maybe-uninitialized
    const char *test_vol_name = "TestE2EVol1";
    uint64_t test_vol_size_mb = 20;
    uint64_t test_vol_size_bytes = test_vol_size_mb * 1024 * 1024;
    uint32_t test_vol_block_size = 4096;
    uint32_t test_vol_ftt = 0;
    xsan_volume_id_t vol_id = {0}; // Initialize
    xsan_volume_t *vol = NULL;
    uint32_t test_nsid = 1;
    bool ns_added = false;

    XSAN_LOG_INFO("[E2E Test] Creating disk group '%s'...", test_dg_name);
    err = xsan_disk_manager_disk_group_create(dm, test_dg_name, XSAN_DISK_GROUP_TYPE_JBOD, bdev_names, num_bdevs_for_dg, &dg_id);
    if (err != XSAN_OK) { XSAN_LOG_ERROR("[E2E Test] Failed to create disk group '%s': %s", test_dg_name, xsan_error_string(err)); goto test_end_e2e; }
    XSAN_LOG_INFO("[E2E Test] Disk group '%s' (ID: %s) created.", test_dg_name, spdk_uuid_get_string((struct spdk_uuid*)&dg_id.data[0]));

    XSAN_LOG_INFO("[E2E Test] Creating volume '%s' (Size: %lu MB, FTT: %u)...", test_vol_name, test_vol_size_mb, test_vol_ftt);
    err = xsan_volume_create(vm, test_vol_name, test_vol_size_bytes, dg_id, test_vol_block_size, false, test_vol_ftt, &vol_id);
    if (err != XSAN_OK) { XSAN_LOG_ERROR("[E2E Test] Failed to create volume '%s': %s", test_vol_name, xsan_error_string(err)); goto cleanup_dg_e2e; }
    vol = xsan_volume_get_by_id(vm, vol_id);
    if (!vol) { XSAN_LOG_ERROR("[E2E Test] Volume '%s' created but get_by_id failed!", test_vol_name); goto cleanup_dg_e2e;}

    XSAN_LOG_INFO("[E2E Test] Volume '%s' (ID: %s) available for NVMe-oF export. State: %d",
                  test_vol_name, spdk_uuid_get_string((struct spdk_uuid*)&vol_id.data[0]), vol->state);
    xsan_volume_allocation_meta_t *e2e_alloc_meta = NULL;
    char e2e_alloc_meta_key[XSAN_MAX_NAME_LEN + SPDK_UUID_STRING_LEN];
    snprintf(e2e_alloc_meta_key, sizeof(e2e_alloc_meta_key), "%s%s", XSAN_VOL_ALLOC_META_PREFIX, spdk_uuid_get_string((struct spdk_uuid*)&vol_id.data[0]));
    char *e2e_alloc_meta_json = NULL; size_t e2e_alloc_meta_json_len = 0;
    if (g_xsan_volume_manager_instance && g_xsan_volume_manager_instance->md_store && // Check vm and md_store
        xsan_metadata_store_get(g_xsan_volume_manager_instance->md_store, e2e_alloc_meta_key, strlen(e2e_alloc_meta_key), &e2e_alloc_meta_json, &e2e_alloc_meta_json_len) == XSAN_OK && e2e_alloc_meta_json) {
        if (_xsan_json_string_to_volume_allocation_meta(e2e_alloc_meta_json, &e2e_alloc_meta) == XSAN_OK && e2e_alloc_meta && e2e_alloc_meta->num_extents > 0) {
            xsan_disk_t *first_disk_for_vol = xsan_disk_manager_find_disk_by_id(dm, e2e_alloc_meta->extents[0].disk_id);
            if (first_disk_for_vol && first_disk_for_vol->bdev_name[0] != '\0') {
                char vol_uuid_str_for_ns[SPDK_UUID_STRING_LEN];
                spdk_uuid_fmt_lower(vol_uuid_str_for_ns, sizeof(vol_uuid_str_for_ns), (struct spdk_uuid*)&vol_id.data[0]);
                XSAN_LOG_INFO("[E2E Test] Adding bdev '%s' as NVMe-oF namespace (NSID %u) for volume '%s'", first_disk_for_vol->bdev_name, test_nsid, test_vol_name);
                err = xsan_nvmf_target_add_namespace(first_disk_for_vol->bdev_name, test_nsid, vol_uuid_str_for_ns);
                if (err != XSAN_OK) { XSAN_LOG_ERROR("[E2E Test] Failed to add namespace for bdev '%s': %s", first_disk_for_vol->bdev_name, xsan_error_string(err));}
                else {
                    XSAN_LOG_INFO("[E2E Test] Namespace NSID %u added for bdev %s.", test_nsid, first_disk_for_vol->bdev_name);
                    XSAN_LOG_INFO("To test from initiator (same machine):");
                    XSAN_LOG_INFO("1. sudo nvme discover -t tcp -a %s -s %s", g_local_node_config.bind_address, g_local_node_config.nvmf_listen_port);
                    XSAN_LOG_INFO("2. sudo nvme connect -t tcp -n %s -a %s -s %s",
                                  g_local_node_config.nvmf_target_nqn[0] ? g_local_node_config.nvmf_target_nqn : "nqn.2024-01.org.xsan:tgt1",
                                  g_local_node_config.bind_address, g_local_node_config.nvmf_listen_port);
                    XSAN_LOG_INFO("3. sudo nvme list");
                    XSAN_LOG_INFO("Pausing for 60 seconds for manual NVMe-oF client tests...");
                    sleep(60);
                    ns_added = true;
                }
            } else { XSAN_LOG_WARN("[E2E Test] Could not find bdev name for first extent of volume '%s' to expose.", test_vol_name); }
        } else { XSAN_LOG_WARN("[E2E Test] Failed to deserialize alloc meta for vol %s.", test_vol_name); }
        if (e2e_alloc_meta) XSAN_FREE(e2e_alloc_meta);
        if (e2e_alloc_meta_json) XSAN_FREE(e2e_alloc_meta_json);
    } else { XSAN_LOG_WARN("[E2E Test] Could not load allocation metadata for volume '%s' to expose as namespace.", test_vol_name); }

    xsan_disk_id_t mapped_disk_id; uint64_t mapped_phys_lba; uint32_t mapped_phys_block_size;
    uint64_t test_lba = 0;
    XSAN_LOG_INFO("[E2E Test] Mapping LBA %lu for volume '%s'...", test_lba, test_vol_name);
    err = xsan_volume_map_lba_to_physical(vm, vol_id, test_lba, &mapped_disk_id, &mapped_phys_lba, &mapped_phys_block_size);
    if (err != XSAN_OK) { XSAN_LOG_ERROR("[E2E Test] Failed to map LBA %lu: %s", test_lba, xsan_error_string(err));}
    else { XSAN_LOG_INFO("[E2E Test] LBA %lu maps to DiskID: %s, PhysLBA: %lu, PhysBlkSize: %u", test_lba, spdk_uuid_get_string((struct spdk_uuid*)&mapped_disk_id.data[0]), mapped_phys_lba, mapped_phys_block_size);}

    uint32_t io_size = test_vol_block_size * 2;
    char *write_buf = xsan_bdev_dma_malloc(io_size, test_vol_block_size);
    char *read_buf = xsan_bdev_dma_malloc(io_size, test_vol_block_size);
    if (!write_buf || !read_buf) { XSAN_LOG_ERROR("[E2E Test] Failed to alloc DMA buffers."); if(write_buf) xsan_bdev_dma_free(write_buf); if(read_buf) xsan_bdev_dma_free(read_buf); goto cleanup_vol_e2e;}
    for (uint32_t i = 0; i < io_size; ++i) write_buf[i] = (char)((i + 5) % 256);
    memset(read_buf, 0xBB, io_size);

    xsan_e2e_io_test_ctx_t write_io_ctx = { .completed = false, .status = XSAN_OK, .len = io_size, .test_name = "E2E_WriteTest", .thread = spdk_get_thread()};
    xsan_e2e_io_test_ctx_t read_io_ctx = { .completed = false, .status = XSAN_OK, .buffer = read_buf, .original_buffer_for_verify = write_buf, .len = io_size, .test_name = "E2E_ReadVerifyTest", .thread = spdk_get_thread()};
    uint64_t io_offset = test_vol_block_size * 5;
    if (io_offset + io_size > test_vol_size_bytes) { XSAN_LOG_WARN("[E2E Test] IO offset+size exceeds vol size. Reducing IO size."); io_size = test_vol_block_size; write_io_ctx.len = read_io_ctx.len = io_size;}

    XSAN_LOG_INFO("[E2E Test] Submitting async write (offset %lu, len %u) to vol '%s'...", io_offset, io_size, test_vol_name);
    err = xsan_volume_write_async(vm, vol_id, io_offset, io_size, write_buf, _e2e_io_completion_cb, &write_io_ctx);
    if (err != XSAN_OK) { XSAN_LOG_ERROR("[E2E Test] Failed to submit write: %s", xsan_error_string(err));}
    else {
        while (!write_io_ctx.completed) { spdk_thread_poll(write_io_ctx.thread, 0, 0); usleep(500); }
        if (write_io_ctx.status == XSAN_OK) {
            XSAN_LOG_INFO("[E2E Test] Write successful. Submitting async read...");
            err = xsan_volume_read_async(vm, vol_id, io_offset, io_size, read_buf, _e2e_io_completion_cb, &read_io_ctx);
            if (err != XSAN_OK) { XSAN_LOG_ERROR("[E2E Test] Failed to submit read: %s", xsan_error_string(err));}
            else {
                while (!read_io_ctx.completed) { spdk_thread_poll(read_io_ctx.thread, 0, 0); usleep(500); }
                if (read_io_ctx.status != XSAN_OK) { XSAN_LOG_ERROR("[E2E Test] Read/Verify phase status: %s", xsan_error_string(read_io_ctx.status));}
            }
        } else { XSAN_LOG_ERROR("[E2E Test] Write operation failed: %s", xsan_error_string(write_io_ctx.status));}
    }
    xsan_bdev_dma_free(write_buf); xsan_bdev_dma_free(read_buf);

cleanup_vol_e2e:
    if (ns_added) {
        XSAN_LOG_INFO("[E2E Test] Removing NVMe-oF namespace NSID %u before deleting volume...", test_nsid);
        err = xsan_nvmf_target_remove_namespace(test_nsid);
        if (err != XSAN_OK) { XSAN_LOG_ERROR("[E2E Test] Failed to remove namespace NSID %u: %s", test_nsid, xsan_error_string(err));}
        else { XSAN_LOG_INFO("[E2E Test] Namespace NSID %u removed.", test_nsid); }
    }
    XSAN_LOG_INFO("[E2E Test] Deleting volume '%s'...", test_vol_name);
    if (!spdk_uuid_is_null((struct spdk_uuid*)&vol_id.data[0])) { // Only delete if vol_id is valid
        err = xsan_volume_delete(vm, vol_id);
        if (err != XSAN_OK) XSAN_LOG_ERROR("[E2E Test] Failed to delete volume '%s': %s", test_vol_name, xsan_error_string(err));
        else XSAN_LOG_INFO("[E2E Test] Volume '%s' deleted.", test_vol_name);
    } else { XSAN_LOG_WARN("[E2E Test] Volume ID was null, skipping delete for volume '%s'", test_vol_name); }
cleanup_dg_e2e:
    XSAN_LOG_INFO("[E2E Test] Deleting disk group '%s'...", test_dg_name);
    if (!spdk_uuid_is_null((struct spdk_uuid*)&dg_id.data[0])) { // Only delete if dg_id is valid
        err = xsan_disk_manager_disk_group_delete(dm, dg_id);
        if (err != XSAN_OK) XSAN_LOG_ERROR("[E2E Test] Failed to delete disk group '%s': %s", test_dg_name, xsan_error_string(err));
        else XSAN_LOG_INFO("[E2E Test] Disk group '%s' deleted.", test_dg_name);
    } else { XSAN_LOG_WARN("[E2E Test] Disk group ID was null, skipping delete for group '%s'", test_dg_name); }
test_end_e2e:
    XSAN_LOG_INFO("===== E2E Core Logic Tests Finished =====");
    g_async_io_test_controller.test_finished_signal = true;
}


static void xsan_node_main_spdk_thread_start(void *arg1, int spdk_startup_rc) {
    XSAN_LOG_INFO("XSAN Node main SPDK thread started. SPDK Startup RC: %d", spdk_startup_rc);
    if (spdk_startup_rc != 0) {
        XSAN_LOG_FATAL("SPDK framework failed to start properly (rc=%d). Exiting XSAN node main.", spdk_startup_rc);
        return;
    }
    g_xsan_config = xsan_config_create();
    if (!g_xsan_config) {
        XSAN_LOG_FATAL("Failed to create global config object. Shutting down.");
        xsan_spdk_manager_request_app_stop(); return;
    }
    const char *config_file_to_load = g_xsan_config_file ? g_xsan_config_file : "xsan_node.conf";
    XSAN_LOG_INFO("Loading XSAN configuration from: %s", config_file_to_load);
    if (!xsan_config_load_from_file(g_xsan_config, config_file_to_load)) {
        XSAN_LOG_ERROR("Failed to load config file '%s'.", config_file_to_load);
    }
    if (!xsan_config_load_node_config(g_xsan_config, &g_local_node_config)) {
        XSAN_LOG_FATAL("Failed to parse node-specific config. Shutting down.");
        goto app_cleanup_stop;
    } else {
        XSAN_LOG_INFO("Node Config Loaded: ID='%s', Addr='%s', Port=%u, NVMF NQN='%s', NVMF Port='%s'",
                      g_local_node_config.node_id, g_local_node_config.bind_address, g_local_node_config.port,
                      g_local_node_config.nvmf_target_nqn, g_local_node_config.nvmf_listen_port);
        if (strlen(g_local_node_config.node_id) == 0) {
             XSAN_LOG_FATAL("Node ID is empty. Shutting down."); goto app_cleanup_stop;
        }
    }
    if (!xsan_config_load_cluster_config(g_xsan_config, &g_cluster_config)) {
        XSAN_LOG_WARN("Failed to parse cluster-specific config.");
    }

    xsan_disk_manager_t *disk_manager = NULL;
    xsan_volume_manager_t *volume_manager = NULL;

    if (xsan_cluster_init(config_file_to_load) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to init XSAN Cluster Manager. Shutting down."); goto app_cleanup_stop;
    }
    if (xsan_disk_manager_init(&disk_manager) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to init XSAN Disk Manager. Shutting down."); goto cluster_cleanup_stop;
    }
    xsan_disk_manager_scan_and_register_bdevs(disk_manager);
    if (xsan_volume_manager_init(disk_manager, &volume_manager) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to init XSAN Volume Manager. Shutting down."); goto dm_cleanup_stop;
    }
    if (xsan_node_comm_init(g_local_node_config.bind_address, g_local_node_config.port, NULL, NULL) != XSAN_OK) {
         XSAN_LOG_FATAL("Failed to init XSAN node comm server on %s:%u. Shutting down.",
                       g_local_node_config.bind_address, g_local_node_config.port);
        goto vm_cleanup_stop;
    }
    if (xsan_node_comm_register_message_handler(XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ,
                                                xsan_volume_manager_handle_replica_write_req, volume_manager) != XSAN_OK ||
        xsan_node_comm_register_message_handler(XSAN_MSG_TYPE_REPLICA_READ_BLOCK_REQ,
                                                xsan_volume_manager_handle_replica_read_req, volume_manager) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to register replica op handlers. Shutting down.");
        goto comm_cleanup_stop;
    }
    if (xsan_nvmf_target_init(g_local_node_config.nvmf_target_nqn,
                               g_local_node_config.bind_address,
                               g_local_node_config.nvmf_listen_port) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN NVMe-oF Target. Shutting down.");
        goto comm_cleanup_stop;
    }
    XSAN_LOG_INFO("XSAN NVMe-oF Target initialized.");

    XSAN_LOG_INFO("XSAN Node subsystems initialized. Running E2E tests or waiting for events...");
    g_async_io_test_controller.test_finished_signal = false;
    _run_e2e_core_logic_tests(disk_manager, volume_manager);

    while (!g_async_io_test_controller.test_finished_signal) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
        usleep(10000);
    }

    XSAN_LOG_INFO("XSAN Node main SPDK thread tasks complete. Cleaning up XSAN subsystems...");
    xsan_nvmf_target_fini();
comm_cleanup_stop:
    xsan_node_comm_fini();
vm_cleanup_stop:
    xsan_volume_manager_fini(&volume_manager);
dm_cleanup_stop:
    xsan_disk_manager_fini(&disk_manager);
cluster_cleanup_stop:
    xsan_cluster_shutdown();
app_cleanup_stop:
    if (g_xsan_config) { xsan_config_destroy(g_xsan_config); g_xsan_config = NULL; }
    XSAN_LOG_INFO("XSAN subsystems cleaned up. Requesting SPDK application stop.");
    xsan_spdk_manager_request_app_stop();
}

static void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -c, --config FILE      Path to XSAN node configuration file (default: xsan_node.conf)\n");
    printf("  -h, --help             Show this help message\n");
}

int main(int argc, char **argv) {
    struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    g_xsan_config_file = "xsan_node.conf";

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
    xsan_spdk_manager_opts_init("xsan_node_spdk_app", NULL, NULL, true, "/var/tmp/xsan.sock");
    if (xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start, NULL) != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to start SPDK application framework.");
        if (g_xsan_config) xsan_config_destroy(g_xsan_config); // Ensure config is freed on early exit
        return 1;
    }
    xsan_spdk_manager_app_fini();
    XSAN_LOG_INFO("XSAN Node application finished.");
    return 0;
}
