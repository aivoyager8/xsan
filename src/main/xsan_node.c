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
    struct spdk_sock *client_sock_to_self;
    bool connected_to_self;
    bool ping_sent_successfully;
    bool pong_received_or_ping_handled;
    bool test_finished_signal;
} xsan_comm_test_state_t;
static xsan_comm_test_state_t g_comm_test_controller;


static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl);
static void _finalize_async_io_test(xsan_async_io_test_control_t *test_ctrl);

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
    test_ctrl->outstanding_io_ops--;
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    if (status != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncIOTest] Write to volume %s FAILED: %s (code %d)", vol_id_str, xsan_error_string(status), status);
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED; _finalize_async_io_test(test_ctrl);
    }
    else {
        XSAN_LOG_INFO("[AsyncIOTest] Write to volume %s successful. Reading back...", vol_id_str);
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
    XSAN_LOG_INFO("--- Starting Async R/W Test on Volume ID %s (bdev: '%s', io_block_size: %u) ---", vol_id_str, test_ctrl->target_bdev_name_for_log, test_ctrl->io_block_size);

    test_ctrl->test_state = ASYNC_IO_TEST_WRITE_SUBMITTED; test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_write_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->write_buffer_dma, _async_io_test_write_complete_cb, test_ctrl);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncIOTest] Failed to submit async write for vol %s: %s", vol_id_str, xsan_error_string(err));
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
static void _comm_test_send_ping_cb(int status, void *cb_arg) {
    xsan_comm_test_state_t *comm_state = (xsan_comm_test_state_t *)cb_arg;
    if (status == 0) { XSAN_LOG_INFO("[CommTest] Successfully sent PING to self."); comm_state->ping_sent_successfully = true; }
    else { XSAN_LOG_ERROR("[CommTest] Failed to send PING to self, status: %d", status); comm_state->test_finished_signal = true; }
}
static void _comm_test_connect_cb(struct spdk_sock *sock, int status, void *cb_arg) {
    xsan_comm_test_state_t *comm_state = (xsan_comm_test_state_t *)cb_arg;
    if (status == 0) {
        XSAN_LOG_INFO("[CommTest] Connected to self (sock %p)!", sock);
        comm_state->client_sock_to_self = sock; comm_state->connected_to_self = true;
        xsan_message_t *ping_msg = xsan_protocol_message_create(XSAN_MSG_TYPE_HEARTBEAT, (uint64_t)spdk_env_get_current_core() << 32 | 0xABCD, "SELF_PING_PAYLOAD", strlen("SELF_PING_PAYLOAD") + 1);
        if (ping_msg) {
            XSAN_LOG_INFO("[CommTest] Sending PING to self...");
            if (xsan_node_comm_send_msg(sock, ping_msg, _comm_test_send_ping_cb, comm_state) != XSAN_OK) {
                XSAN_LOG_ERROR("[CommTest] Failed to initiate PING send."); comm_state->test_finished_signal = true;
            }
            xsan_protocol_message_destroy(ping_msg);
        } else { XSAN_LOG_ERROR("[CommTest] Failed to create PING msg."); comm_state->test_finished_signal = true; }
    } else { XSAN_LOG_ERROR("[CommTest] Failed to connect to self, status: %d", status); comm_state->test_finished_signal = true; }
}
static void _xsan_node_test_message_handler(struct spdk_sock *sock, const char *peer_addr_str, xsan_message_t *msg, void *cb_arg) {
    (void)sock; (void)cb_arg;
    XSAN_LOG_INFO("[TestMsgHandler] Received msg from %s: Type %u, PayloadLen %u", peer_addr_str, msg->header.type, msg->header.payload_length);
    if (msg->header.type == XSAN_MSG_TYPE_HEARTBEAT && msg->payload && strcmp((char*)msg->payload, "SELF_PING_PAYLOAD") == 0) {
        XSAN_LOG_INFO("[CommTest] PING (HEARTBEAT) received by handler from self!");
        g_comm_test_controller.pong_received_or_ping_handled = true;
        g_comm_test_controller.test_finished_signal = true;
    }
    xsan_protocol_message_destroy(msg);
}

static void xsan_node_main_spdk_thread_start(void *arg1, int rc) {
    (void)arg1;
    xsan_error_t err = XSAN_OK;
    xsan_disk_manager_t *dm = NULL;
    xsan_volume_manager_t *vm = NULL;

    XSAN_LOG_INFO("XSAN SPDK app thread started (rc: %d).", rc);
    if (rc != 0) { XSAN_LOG_FATAL("SPDK framework init failed."); goto app_stop_sequence_start; }

    if (xsan_bdev_subsystem_init() != XSAN_OK) { XSAN_LOG_FATAL("Bdev subsystem init failed."); goto app_stop_sequence_start; }
    if (xsan_disk_manager_init(&dm) != XSAN_OK) { XSAN_LOG_FATAL("Disk manager init failed."); goto app_stop_bdev_fini; }
    g_async_io_test_controller.dm = dm;
    if (xsan_volume_manager_init(dm, &vm) != XSAN_OK) { XSAN_LOG_FATAL("Volume manager init failed."); goto app_stop_disk_mgr_fini; }
    g_async_io_test_controller.vm = vm;

    const char *node_listen_ip = "0.0.0.0"; uint16_t node_listen_port = 7777;
    if (xsan_node_comm_init(node_listen_ip, node_listen_port, _xsan_node_test_message_handler, NULL) != XSAN_OK) {
        XSAN_LOG_FATAL("Node communication init failed."); goto app_stop_vol_mgr_fini;
    }

    if (xsan_disk_manager_scan_and_register_bdevs(dm) != XSAN_OK) { /* Log error, continue for test */ }

    xsan_disk_t **disks_list = NULL; int disk_count = 0;
    xsan_disk_manager_get_all_disks(dm, &disks_list, &disk_count);
    XSAN_LOG_INFO("=== XSAN Disks (Post-Init/Load & Scan) ===");
    for (int i = 0; i < disk_count; ++i) {
        char id_str[SPDK_UUID_STRING_LEN]; char bdev_id_str[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(id_str, sizeof(id_str), (struct spdk_uuid*)&disks_list[i]->id.data[0]);
        spdk_uuid_fmt_lower(bdev_id_str, sizeof(bdev_id_str), (struct spdk_uuid*)&disks_list[i]->bdev_uuid.data[0]);
        XSAN_LOG_INFO("  Disk[%d] ID: %s, BDev: %s, BDevUUID: %s, Size: %.2fG, State: %d", i, id_str, disks_list[i]->bdev_name, bdev_id_str, (double)disks_list[i]->capacity_bytes / (1024.0*1024*1024), disks_list[i]->state);
    }

    XSAN_LOG_INFO("=== XSAN Volumes (Post-Init/Load) ===");
    xsan_volume_t **vols_list = NULL; int vol_count = 0;
    xsan_volume_list_all(vm, &vols_list, &vol_count);
    for(int i=0; i<vol_count; ++i) {
        char vid_str[SPDK_UUID_STRING_LEN]; char gid_str[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(vid_str, sizeof(vid_str), (struct spdk_uuid*)&vols_list[i]->id.data[0]);
        spdk_uuid_fmt_lower(gid_str, sizeof(gid_str), (struct spdk_uuid*)&vols_list[i]->source_group_id.data[0]);
        XSAN_LOG_INFO("  Vol[%d] ID: %s, Name: %s, Size: %.2fG, FTT: %u, Reps: %u, Group: %s", i, vid_str, vols_list[i]->name, (double)vols_list[i]->size_bytes / (1024.0*1024*1024), vols_list[i]->FTT, vols_list[i]->actual_replica_count, gid_str);
    }
    if(vols_list) xsan_volume_manager_free_volume_pointer_list(vols_list);


    // --- Test Group & Volume Creation with FTT ---
    xsan_group_id_t test_dg_id; memset(&test_dg_id, 0, sizeof(test_dg_id)); bool dg_created = false;
    xsan_volume_id_t test_vol_id; memset(&test_vol_id, 0, sizeof(test_vol_id)); bool vol_created = false;
    xsan_volume_t *vol_for_async_test = NULL; xsan_disk_t *disk_for_vol_group = NULL;

    if (disk_count > 0 && disks_list) {
        disk_for_vol_group = disks_list[0]; // Use first available disk for the test group
        const char *dg_bdevs[] = {disk_for_vol_group->bdev_name};
        if (xsan_disk_manager_find_disk_group_by_name(dm, "MetaTestDG") == NULL && /* only create if not loaded */
            xsan_disk_manager_disk_group_create(dm, "MetaTestDG", XSAN_DISK_GROUP_TYPE_PASSSTHROUGH, dg_bdevs, 1, &test_dg_id) == XSAN_OK) {
            dg_created = true; XSAN_LOG_INFO("Created 'MetaTestDG'.");
        } else { XSAN_LOG_WARN("Could not create 'MetaTestDG' or it already exists."); if(!dg_created) test_dg_id = xsan_disk_manager_find_disk_group_by_name(dm, "MetaTestDG")->id; /* try to get existing */}
    } else { XSAN_LOG_WARN("No disks to create 'MetaTestDG'.");}
    if(disks_list) { xsan_disk_manager_free_disk_pointer_list(disks_list); disks_list = NULL; }


    if (!spdk_uuid_is_null((struct spdk_uuid*)&test_dg_id.data[0])) { // If group exists or was created
        uint32_t test_ftt = 0; // Test with FTT=0 (single replica) first for simplicity of async test target
        uint64_t vol_s_mb = (disk_for_vol_group && disk_for_vol_group->capacity_bytes / (1024*1024) > 128) ? 128 : (disk_for_vol_group ? disk_for_vol_group->capacity_bytes / (1024*1024*2) : 64);
        if(vol_s_mb < 16) vol_s_mb = 16; // Ensure some reasonable size
        uint64_t vol_s_bytes = vol_s_mb * 1024 * 1024;
        uint32_t vol_bs = 4096;
        if(vol_s_bytes < vol_bs) vol_s_bytes = vol_bs;

        if (xsan_volume_get_by_name(vm, "MetaTestVol") == NULL) { // Only create if not loaded
             XSAN_LOG_INFO("Attempting to create volume 'MetaTestVol' (FTT=%u)...", test_ftt);
            err = xsan_volume_create(vm, "MetaTestVol", vol_s_bytes, test_dg_id, vol_bs, false, test_ftt, &test_vol_id);
            if (err == XSAN_OK) { vol_created = true; XSAN_LOG_INFO("Created 'MetaTestVol'."); }
            else { XSAN_LOG_ERROR("Failed to create 'MetaTestVol': %s", xsan_error_string(err));}
        } else {
            XSAN_LOG_INFO("Volume 'MetaTestVol' seems to exist from metadata load.");
            vol_for_async_test = xsan_volume_get_by_name(vm, "MetaTestVol");
            if(vol_for_async_test) {
                 memcpy(&test_vol_id, &vol_for_async_test->id, sizeof(xsan_volume_id_t));
                 vol_created = true; // Treat as "created" for test flow
            }
        }
    } else { XSAN_LOG_WARN("Skipping 'MetaTestVol' creation, 'MetaTestDG' not available.");}

    vol_for_async_test = xsan_volume_get_by_id(vm, test_vol_id); // Get the definitive pointer
    if (vol_created && vol_for_async_test) {
        XSAN_LOG_INFO("Volume 'MetaTestVol' (FTT %u, Replicas %u) details after creation/load:", vol_for_async_test->FTT, vol_for_async_test->actual_replica_count);
        for(uint32_t r=0; r < vol_for_async_test->actual_replica_count; ++r) {
            char rn_id[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(rn_id, sizeof(rn_id), (struct spdk_uuid*)&vol_for_async_test->replica_nodes[r].node_id.data[0]);
            XSAN_LOG_INFO("  Replica[%u]: NodeID=%s, Addr=%s:%u", r, rn_id, vol_for_async_test->replica_nodes[r].node_ip_addr, vol_for_async_test->replica_nodes[r].node_comm_port);
        }
    }

    // --- Start Tests (Comm Test first, then Async I/O Test) ---
    memset(&g_comm_test_controller, 0, sizeof(g_comm_test_controller));
    memset(&g_async_io_test_controller, 0, sizeof(g_async_io_test_controller));
    g_async_io_test_controller.dm = dm; g_async_io_test_controller.vm = vm; // Pass managers to async IO test

    XSAN_LOG_INFO("[CommTest] Initiating self-connect PING test...");
    err = xsan_node_comm_connect("127.0.0.1", node_listen_port, _comm_test_connect_cb, &g_comm_test_controller);
    if(err != XSAN_OK) { XSAN_LOG_ERROR("[CommTest] Failed to initiate self-connect: %s", xsan_error_string(err)); g_comm_test_controller.test_finished_signal = true; }

    if (vol_created && vol_for_async_test && disk_for_vol_group) { // disk_for_vol_group is from DG creation
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

    // --- Cleanup Test Entities ---
    if (vol_created) { xsan_volume_delete(vm, test_vol_id); XSAN_LOG_INFO("Cleaned up 'MetaTestVol'.");}
    if (dg_created) { xsan_disk_manager_disk_group_delete(dm, test_dg_id); XSAN_LOG_INFO("Cleaned up 'MetaTestDG'.");}

app_stop_vm_fini:
    if (vm) xsan_volume_manager_fini(&vm);
app_stop_dm_fini:
    if (dm) xsan_disk_manager_fini(&dm);
app_stop_bdev_fini_only:
    xsan_bdev_subsystem_fini();
app_stop_no_cleanup:
    XSAN_LOG_INFO("Requesting SPDK application stop.");
    xsan_spdk_manager_request_app_stop();
}

int main(int argc, char **argv) {
    xsan_log_config_t log_cfg = xsan_log_default_config();
    log_cfg.level = XSAN_LOG_LEVEL_DEBUG;
    xsan_log_init(&log_cfg);
    XSAN_LOG_INFO("XSAN Node starting (main function)...");
    const char *spdk_json_conf_file = NULL;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--spdk-conf") == 0 && i + 1 < argc) { spdk_json_conf_file = argv[++i]; break; }
            else if (i == 1 && argv[i][0] != '-') { spdk_json_conf_file = argv[i]; }
        }
    }
    if (spdk_json_conf_file) XSAN_LOG_INFO("Using SPDK JSON config: %s", spdk_json_conf_file);
    else XSAN_LOG_WARN("No SPDK JSON config. Bdevs may not be available for tests.");

    mkdir("./xsan_meta_db", 0755);
    mkdir("./xsan_meta_db/disk_manager_db", 0755);
    mkdir("./xsan_meta_db/volume_manager_db", 0755);

    xsan_error_t err = xsan_spdk_manager_opts_init("xsan_node_main", spdk_json_conf_file, "0x1", false, NULL);
    if (err != XSAN_OK) { XSAN_LOG_FATAL("SPDK opts init failed: %s", xsan_error_string(err)); xsan_log_shutdown(); return EXIT_FAILURE; }
    err = xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start, NULL);
    if (err != XSAN_OK) { XSAN_LOG_FATAL("xsan_spdk_manager_start_app error: %s", xsan_error_string(err)); }

    xsan_node_comm_fini();
    xsan_spdk_manager_app_fini();
    XSAN_LOG_INFO("XSAN Node has shut down.");
    xsan_log_shutdown();
    return (err == XSAN_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
