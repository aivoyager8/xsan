#include "xsan_log.h"
#include "xsan_config.h"
#include "xsan_spdk_manager.h"
#include "xsan_bdev.h"
#include "xsan_disk_manager.h"
#include "xsan_volume_manager.h"
#include "xsan_io.h"
#include "xsan_node_comm.h"     // For node communication module
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
#include "spdk/thread.h" // For spdk_thread_get_id

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
    bool test_finished_signal; // Combined signal for this test path
} xsan_async_io_test_control_t;
static xsan_async_io_test_control_t g_async_io_test_controller;

// --- Node Communication Test Callbacks & State ---
typedef struct {
    struct spdk_sock *client_sock_to_self;
    bool connected_to_self;
    bool ping_sent_successfully;
    bool pong_received_or_ping_handled;
    bool test_finished_signal; // Combined signal for this test path
} xsan_comm_test_state_t;
static xsan_comm_test_state_t g_comm_test_controller;


// Forward declarations for async test helpers
static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl);
static void _finalize_async_io_test(xsan_async_io_test_control_t *test_ctrl);

static void _async_io_test_read_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *test_ctrl = (xsan_async_io_test_control_t *)cb_arg;
    test_ctrl->outstanding_io_ops--;
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    if (status != XSAN_OK) { /* ... log error ... */ test_ctrl->test_state = ASYNC_IO_TEST_FAILED; }
    else { /* ... log success, memcmp ... */
        if (memcmp(test_ctrl->write_buffer_dma, test_ctrl->read_buffer_dma, test_ctrl->io_block_size) == 0) {
            XSAN_LOG_INFO("SUCCESS: [AsyncIOTest] R/W data verification for volume %s PASSED!", vol_id_str);
            test_ctrl->test_state = ASYNC_IO_TEST_VERIFY_DONE;
        } else { XSAN_LOG_ERROR("FAILURE: [AsyncIOTest] R/W data verification for volume %s FAILED!", vol_id_str); test_ctrl->test_state = ASYNC_IO_TEST_FAILED; }
    }
    _finalize_async_io_test(test_ctrl);
}
static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl) {
    /* ... submit async read ... */
    test_ctrl->test_state = ASYNC_IO_TEST_READ_SUBMITTED; test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_read_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->read_buffer_dma, _async_io_test_read_complete_cb, test_ctrl);
    if (err != XSAN_OK) { /* ... log error, dec ops, set fail state, finalize ... */ test_ctrl->outstanding_io_ops--; test_ctrl->test_state = ASYNC_IO_TEST_FAILED; _finalize_async_io_test(test_ctrl); }
}
static void _async_io_test_write_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *test_ctrl = (xsan_async_io_test_control_t *)cb_arg;
    test_ctrl->outstanding_io_ops--;
    if (status != XSAN_OK) { /* ... log error, set fail state, finalize ... */ test_ctrl->test_state = ASYNC_IO_TEST_FAILED; _finalize_async_io_test(test_ctrl); }
    else { _run_async_io_test_read_phase(test_ctrl); }
}
static void _start_async_io_test_on_volume(xsan_async_io_test_control_t *test_ctrl, xsan_volume_t *vol_to_test) {
    /* ... setup test_ctrl, alloc DMA bufs, fill write_buf ... */
    if (!vol_to_test || vol_to_test->block_size_bytes == 0 || vol_to_test->num_blocks == 0) { _finalize_async_io_test(test_ctrl); return; }
    memcpy(&test_ctrl->volume_id_to_test, &vol_to_test->id, sizeof(xsan_volume_id_t));
    test_ctrl->io_block_size = vol_to_test->block_size_bytes;
    xsan_disk_group_t *group = xsan_disk_manager_find_disk_group_by_id(test_ctrl->dm, vol_to_test->source_group_id);
    if (!group || group->disk_count == 0) { _finalize_async_io_test(test_ctrl); return; }
    xsan_disk_t *first_disk_in_group = xsan_disk_manager_find_disk_by_id(test_ctrl->dm, group->disk_ids[0]);
    if (!first_disk_in_group) { _finalize_async_io_test(test_ctrl); return; }
    xsan_strcpy_safe(test_ctrl->target_bdev_name_for_log, first_disk_in_group->bdev_name, XSAN_MAX_NAME_LEN);
    test_ctrl->dma_alignment = xsan_bdev_get_buf_align(first_disk_in_group->bdev_name);
    test_ctrl->write_buffer_dma = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);
    test_ctrl->read_buffer_dma = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);
    if (!test_ctrl->write_buffer_dma || !test_ctrl->read_buffer_dma) { /* free, finalize */ _finalize_async_io_test(test_ctrl); return; }
    char vol_id_str[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    snprintf((char*)test_ctrl->write_buffer_dma, test_ctrl->io_block_size, "XSAN Async Test! Vol %s", vol_id_str);
    for(size_t k=strlen((char*)test_ctrl->write_buffer_dma); k < test_ctrl->io_block_size; ++k) { ((char*)test_ctrl->write_buffer_dma)[k] = (char)((k % 250) + 5); }
    memset(test_ctrl->read_buffer_dma, 0xDD, test_ctrl->io_block_size);
    XSAN_LOG_INFO("--- Starting Async R/W Test on Volume ID %s ---", vol_id_str);
    test_ctrl->test_state = ASYNC_IO_TEST_WRITE_SUBMITTED; test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_write_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->write_buffer_dma, _async_io_test_write_complete_cb, test_ctrl);
    if (err != XSAN_OK) { /* free bufs, dec ops, set fail, finalize */ test_ctrl->outstanding_io_ops--; test_ctrl->test_state = ASYNC_IO_TEST_FAILED; _finalize_async_io_test(test_ctrl); }
}
static void _finalize_async_io_test(xsan_async_io_test_control_t *test_ctrl) {
    if (test_ctrl->outstanding_io_ops == 0) {
        XSAN_LOG_INFO("[AsyncIOTest] Sequence finished, state: %d.", test_ctrl->test_state);
        if(test_ctrl->write_buffer_dma) xsan_bdev_dma_free(test_ctrl->write_buffer_dma); test_ctrl->write_buffer_dma = NULL;
        if(test_ctrl->read_buffer_dma) xsan_bdev_dma_free(test_ctrl->read_buffer_dma); test_ctrl->read_buffer_dma = NULL;
        test_ctrl->test_finished_signal = true; // Signal main SPDK app func
    }
}

// --- Node Comm Test Callbacks & State ---
static void _comm_test_send_ping_cb(int status, void *cb_arg) {
    xsan_comm_test_state_t *comm_state = (xsan_comm_test_state_t *)cb_arg;
    if (status == 0) {
        XSAN_LOG_INFO("[CommTest] Successfully sent PING message to self.");
        comm_state->ping_sent_successfully = true;
        // Now wait for handler to receive it and set pong_received_or_ping_handled
    } else {
        XSAN_LOG_ERROR("[CommTest] Failed to send PING message to self, status: %d", status);
        comm_state->test_finished_signal = true; // End test on send failure
    }
}

static void _comm_test_connect_cb(struct spdk_sock *sock, int status, void *cb_arg) {
    xsan_comm_test_state_t *comm_state = (xsan_comm_test_state_t *)cb_arg;
    if (status == 0) {
        XSAN_LOG_INFO("[CommTest] Successfully connected to self (sock %p)!", sock);
        comm_state->client_sock_to_self = sock;
        comm_state->connected_to_self = true;

        xsan_message_t *ping_msg = xsan_protocol_message_create(
            XSAN_MSG_TYPE_HEARTBEAT, // Use HEARTBEAT as PING
            (uint64_t)spdk_env_get_current_core() << 32 | 0x1234, // Example TID
            "SELF_PING", strlen("SELF_PING") + 1);

        if (ping_msg) {
            XSAN_LOG_INFO("[CommTest] Sending PING message to self...");
            xsan_error_t send_err = xsan_node_comm_send_msg(sock, ping_msg, _comm_test_send_ping_cb, comm_state);
            if (send_err != XSAN_OK) {
                XSAN_LOG_ERROR("[CommTest] Failed to initiate send for PING: %s", xsan_error_string(send_err));
                comm_state->test_finished_signal = true;
            }
            xsan_protocol_message_destroy(ping_msg);
        } else {
            XSAN_LOG_ERROR("[CommTest] Failed to create PING message.");
            comm_state->test_finished_signal = true;
        }
    } else {
        XSAN_LOG_ERROR("[CommTest] Failed to connect to self, status: %d", status);
        comm_state->test_finished_signal = true;
    }
}

// Main message handler for the node
static void _xsan_node_test_message_handler(struct spdk_sock *sock,
                                            const char *peer_addr_str,
                                            xsan_message_t *msg,
                                            void *cb_arg) {
    (void)sock; (void)cb_arg;
    XSAN_LOG_INFO("[TestMsgHandler] Received message from %s: Type %u", peer_addr_str, msg->header.type);

    if (msg->header.type == XSAN_MSG_TYPE_HEARTBEAT && msg->payload && strcmp((char*)msg->payload, "SELF_PING") == 0) {
        XSAN_LOG_INFO("[CommTest] PING message received by handler from self!");
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

    XSAN_LOG_INFO("XSAN SPDK application thread started (rc: %d).", rc);
    if (rc != 0) { goto app_stop_no_cleanup; }

    if (xsan_bdev_subsystem_init() != XSAN_OK) { goto app_stop_no_cleanup; }
    if (xsan_disk_manager_init(&dm) != XSAN_OK) { goto app_stop_bdev_fini_only; }
    g_async_io_test_controller.dm = dm;
    if (xsan_volume_manager_init(dm, &vm) != XSAN_OK) { goto app_stop_dm_fini; }
    g_async_io_test_controller.vm = vm;

    const char *node_listen_ip = "0.0.0.0";
    uint16_t node_listen_port = 7777;
    if (xsan_node_comm_init(node_listen_ip, node_listen_port, _xsan_node_test_message_handler, NULL) != XSAN_OK) {
        goto app_stop_vm_fini;
    }

    if (xsan_disk_manager_scan_and_register_bdevs(dm) != XSAN_OK) { /* Log error, continue */ }

    xsan_disk_t **disks = NULL; int disk_count = 0;
    xsan_disk_manager_get_all_disks(dm, &disks, &disk_count);
    /* ... log disks ... */
    if(disks) { XSAN_LOG_DEBUG("Found %d XSAN disks.", disk_count); }


    // --- Comm Test Init ---
    memset(&g_comm_test_controller, 0, sizeof(g_comm_test_controller));
    XSAN_LOG_INFO("[CommTest] Attempting to connect to self (%s:%u) for PING test...", node_listen_ip, node_listen_port);
    // Use 127.0.0.1 for self-connect even if listening on 0.0.0.0
    err = xsan_node_comm_connect("127.0.0.1", node_listen_port, _comm_test_connect_cb, &g_comm_test_controller);
    if(err != XSAN_OK) {
        XSAN_LOG_ERROR("[CommTest] Failed to initiate self-connect: %s", xsan_error_string(err));
        g_comm_test_controller.test_finished_signal = true; // Mark as finished (failed)
    }


    // --- Volume & Async I/O Test ---
    xsan_group_id_t dg_id; memset(&dg_id, 0, sizeof(dg_id)); bool dg_created = false;
    xsan_volume_id_t vol_id; memset(&vol_id, 0, sizeof(vol_id)); bool vol_created = false;
    xsan_volume_t *vol_for_test = NULL; xsan_disk_t *disk_for_vol = NULL;

    if (disk_count > 0 && disks) {
        disk_for_vol = disks[0];
        const char *dg_bdevs[] = {disk_for_vol->bdev_name};
        if (xsan_disk_manager_disk_group_create(dm, "MetaTestDG", XSAN_DISK_GROUP_TYPE_PASSSTHROUGH, dg_bdevs, 1, &dg_id) == XSAN_OK) {
            dg_created = true;
            uint64_t v_size = (disk_for_vol->capacity_bytes / (1024*1024) > 128) ? (128*1024*1024) : disk_for_vol->capacity_bytes/2;
            if (v_size < 4096) v_size = 4096;
            if (xsan_volume_create(vm, "MetaTestVol", v_size, dg_id, 4096, false, &vol_id) == XSAN_OK) {
                vol_created = true;
                vol_for_test = xsan_volume_get_by_id(vm, vol_id);
            } else { XSAN_LOG_ERROR("Failed to create MetaTestVol for async IO test.");}
        } else { XSAN_LOG_ERROR("Failed to create MetaTestDG for async IO test.");}
    }
    if(disks) { xsan_disk_manager_free_disk_pointer_list(disks); disks = NULL; }


    if (vol_created && vol_for_test && disk_for_vol) {
        g_async_io_test_controller.test_finished_signal = false;
        _start_async_io_test_on_volume(&g_async_io_test_controller, vol_for_test);
    } else {
        XSAN_LOG_WARN("Skipping async I/O test as test volume setup failed.");
        g_async_io_test_controller.test_finished_signal = true;
    }

    XSAN_LOG_INFO("Waiting for Async I/O and Comm tests to complete...");
    while(!g_async_io_test_controller.test_finished_signal || !g_comm_test_controller.test_finished_signal) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
    XSAN_LOG_INFO("All tests signaled completion.");

    // --- Cleanup Test Entities ---
    if (vol_created) { xsan_volume_delete(vm, vol_id); }
    if (dg_created) { xsan_disk_manager_disk_group_delete(dm, dg_id); }

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
    // ... (main function as before, ensuring xsan_node_comm_fini() is called before xsan_spdk_manager_app_fini()) ...
    xsan_log_config_t log_cfg = xsan_log_default_config();
    log_cfg.level = XSAN_LOG_LEVEL_DEBUG;
    xsan_log_init(&log_cfg);
    XSAN_LOG_INFO("XSAN Node starting...");
    const char *spdk_json_conf_file = NULL;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--spdk-conf") == 0 && i + 1 < argc) { spdk_json_conf_file = argv[++i]; break; }
            else if (i == 1 && argv[i][0] != '-') { spdk_json_conf_file = argv[i]; }
        }
    }
    if (spdk_json_conf_file) XSAN_LOG_INFO("Using SPDK JSON config: %s", spdk_json_conf_file);
    else XSAN_LOG_WARN("No SPDK JSON config. Bdevs may not be available.");

    // Ensure meta DB dirs exist
    mkdir("./xsan_meta_db", 0755);
    mkdir("./xsan_meta_db/disk_manager_db", 0755);
    mkdir("./xsan_meta_db/volume_manager_db", 0755);

    xsan_error_t err = xsan_spdk_manager_opts_init("xsan_node_main", spdk_json_conf_file, "0x1", false, NULL);
    if (err != XSAN_OK) { XSAN_LOG_FATAL("SPDK opts init failed: %s", xsan_error_string(err)); xsan_log_shutdown(); return EXIT_FAILURE; }
    err = xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start, NULL);
    if (err != XSAN_OK) { XSAN_LOG_FATAL("xsan_spdk_manager_start_app error: %s", xsan_error_string(err)); }

    xsan_node_comm_fini();
    xsan_spdk_manager_app_fini();
    XSAN_LOG_INFO("XSAN Node shut down.");
    xsan_log_shutdown();
    return (err == XSAN_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
