#include "xsan_log.h"
#include "xsan_config.h"
#include "xsan_spdk_manager.h"
#include "xsan_bdev.h"
#include "xsan_disk_manager.h"
#include "xsan_volume_manager.h"
#include "xsan_io.h"
#include "xsan_error.h"
#include "xsan_string_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>             // For rmdir, access
#include <sys/stat.h>           // For mkdir
#include <errno.h>              // For errno with rmdir

#include "spdk/uuid.h"
#include "spdk/env.h"

// --- Async I/O Test Context and Callbacks (from previous step, kept for completeness) ---
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
    bool all_tests_finished_signal;
} xsan_async_io_test_control_t;
static xsan_async_io_test_control_t g_async_test_controller;
// Forward declarations for async test helpers
static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl);
static void _finalize_and_set_app_finish_signal(xsan_async_io_test_control_t *test_ctrl, bool set_signal);
static void _async_io_test_read_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *test_ctrl = (xsan_async_io_test_control_t *)cb_arg;
    test_ctrl->outstanding_io_ops--;
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    if (status != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncTest] Read from volume %s FAILED: %s (code %d)", vol_id_str, xsan_error_string(status), status);
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
    } else {
        XSAN_LOG_INFO("[AsyncTest] Read from volume %s successful.", vol_id_str);
        if (memcmp(test_ctrl->write_buffer_dma, test_ctrl->read_buffer_dma, test_ctrl->io_block_size) == 0) {
            XSAN_LOG_INFO("SUCCESS: [AsyncTest] R/W data verification for volume %s PASSED!", vol_id_str);
            test_ctrl->test_state = ASYNC_IO_TEST_VERIFY_DONE;
        } else {
            XSAN_LOG_ERROR("FAILURE: [AsyncTest] R/W data verification for volume %s FAILED!", vol_id_str);
            test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        }
    }
    _finalize_and_set_app_finish_signal(test_ctrl, true); // Signal app to stop after this test
}
static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl) {
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    XSAN_LOG_DEBUG("[AsyncTest] Submitting async read for volume %s, offset 0, length %u...", vol_id_str, test_ctrl->io_block_size);
    test_ctrl->test_state = ASYNC_IO_TEST_READ_SUBMITTED;
    test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_read_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->read_buffer_dma, _async_io_test_read_complete_cb, test_ctrl);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncTest] Failed to submit async read for volume %s: %s", vol_id_str, xsan_error_string(err));
        test_ctrl->outstanding_io_ops--;
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_and_set_app_finish_signal(test_ctrl, true);
    }
}
static void _async_io_test_write_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *test_ctrl = (xsan_async_io_test_control_t *)cb_arg;
    test_ctrl->outstanding_io_ops--;
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    if (status != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncTest] Write to volume %s FAILED: %s (code %d)", vol_id_str, xsan_error_string(status), status);
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_and_set_app_finish_signal(test_ctrl, true);
    } else {
        XSAN_LOG_INFO("[AsyncTest] Write to volume %s successful. Proceeding to read phase.", vol_id_str);
        _run_async_io_test_read_phase(test_ctrl);
    }
}
static void _start_async_io_test_on_volume(xsan_async_io_test_control_t *test_ctrl, xsan_volume_t *vol_to_test) {
    if (!vol_to_test || vol_to_test->block_size_bytes == 0 || vol_to_test->num_blocks == 0) { /* ... error handling ... */ _finalize_and_set_app_finish_signal(test_ctrl, true); return; }
    memcpy(&test_ctrl->volume_id_to_test, &vol_to_test->id, sizeof(xsan_volume_id_t));
    test_ctrl->io_block_size = vol_to_test->block_size_bytes;
    xsan_disk_group_t *group = xsan_disk_manager_find_disk_group_by_id(test_ctrl->dm, vol_to_test->source_group_id);
    if (!group || group->disk_count == 0) { /* ... error handling ... */ _finalize_and_set_app_finish_signal(test_ctrl, true); return; }
    xsan_disk_t *first_disk_in_group = xsan_disk_manager_find_disk_by_id(test_ctrl->dm, group->disk_ids[0]);
    if (!first_disk_in_group) { /* ... error handling ... */ _finalize_and_set_app_finish_signal(test_ctrl, true); return; }
    xsan_strcpy_safe(test_ctrl->target_bdev_name_for_log, first_disk_in_group->bdev_name, XSAN_MAX_NAME_LEN);
    test_ctrl->dma_alignment = xsan_bdev_get_buf_align(first_disk_in_group->bdev_name);
    test_ctrl->write_buffer_dma = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);
    test_ctrl->read_buffer_dma = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);
    if (!test_ctrl->write_buffer_dma || !test_ctrl->read_buffer_dma) { /* ... error handling & free ... */ _finalize_and_set_app_finish_signal(test_ctrl, true); return; }
    char vol_id_str[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    snprintf((char*)test_ctrl->write_buffer_dma, test_ctrl->io_block_size, "XSAN Async Test! Vol %s", vol_id_str);
    for(size_t k=strlen((char*)test_ctrl->write_buffer_dma); k < test_ctrl->io_block_size; ++k) { ((char*)test_ctrl->write_buffer_dma)[k] = (char)((k % 250) + 5); }
    memset(test_ctrl->read_buffer_dma, 0xDD, test_ctrl->io_block_size);
    XSAN_LOG_INFO("--- Starting Async R/W Test on Volume ID %s (bdev: '%s', io_block_size: %u) ---", vol_id_str, test_ctrl->target_bdev_name_for_log, test_ctrl->io_block_size);
    test_ctrl->test_state = ASYNC_IO_TEST_WRITE_SUBMITTED;
    test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_write_async(test_ctrl->vm, test_ctrl->volume_id_to_test, 0, test_ctrl->io_block_size, test_ctrl->write_buffer_dma, _async_io_test_write_complete_cb, test_ctrl);
    if (err != XSAN_OK) { /* ... error handling & free ... */ test_ctrl->outstanding_io_ops--; test_ctrl->test_state = ASYNC_IO_TEST_FAILED; _finalize_and_set_app_finish_signal(test_ctrl, true); }
}
static void _finalize_and_set_app_finish_signal(xsan_async_io_test_control_t *test_ctrl, bool set_signal) {
    if (test_ctrl->outstanding_io_ops == 0) {
        XSAN_LOG_INFO("[AsyncTest] Async I/O test sequence on volume %s finished with state: %d.", spdk_uuid_get_string((struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]), test_ctrl->test_state);
        if(test_ctrl->write_buffer_dma) xsan_bdev_dma_free(test_ctrl->write_buffer_dma); test_ctrl->write_buffer_dma = NULL;
        if(test_ctrl->read_buffer_dma) xsan_bdev_dma_free(test_ctrl->read_buffer_dma); test_ctrl->read_buffer_dma = NULL;
        if(set_signal) test_ctrl->all_tests_finished_signal = true;
    }
}
// --- End of Async I/O Test ---

static void xsan_node_main_spdk_thread_start(void *arg1, int rc) {
    (void)arg1;
    xsan_error_t err = XSAN_OK;
    xsan_disk_manager_t *dm = NULL;
    xsan_volume_manager_t *vm = NULL;

    XSAN_LOG_INFO("XSAN SPDK application thread started (spdk_app_start internal rc: %d).", rc);
    if (rc != 0) { XSAN_LOG_FATAL("SPDK framework init failed."); goto app_stop_no_cleanup; }

    if (xsan_bdev_subsystem_init() != XSAN_OK) { /* ... */ goto app_stop_no_cleanup; }
    err = xsan_disk_manager_init(&dm);
    if (err != XSAN_OK) { /* ... */ goto app_stop_bdev_fini_only; }
    g_async_test_controller.dm = dm;

    err = xsan_volume_manager_init(dm, &vm);
    if (err != XSAN_OK) { /* ... */ goto app_stop_dm_fini; }
    g_async_test_controller.vm = vm;

    // Initial scan after loading metadata (load is called in _init)
    XSAN_LOG_INFO("=== Test Run: Initial Scan and List (after metadata load) ===");
    err = xsan_disk_manager_scan_and_register_bdevs(dm);
    if (err != XSAN_OK && err != XSAN_ERROR_NOT_FOUND) { XSAN_LOG_ERROR("Error during initial bdev scan: %s", xsan_error_string(err)); }

    xsan_disk_t **initial_disks = NULL; int initial_disk_count = 0;
    xsan_disk_manager_get_all_disks(dm, &initial_disks, &initial_disk_count);
    XSAN_LOG_INFO("Found %d XSAN disks after initial scan and metadata load:", initial_disk_count);
    for (int i = 0; i < initial_disk_count; ++i) { /* ... log disk info ... */
        XSAN_LOG_INFO("  Disk: %s, State: %d", initial_disks[i]->bdev_name, initial_disks[i]->state);
    }
    if(initial_disks) xsan_disk_manager_free_disk_pointer_list(initial_disks);

    xsan_volume_t **initial_volumes = NULL; int initial_volume_count = 0;
    xsan_volume_list_all(vm, &initial_volumes, &initial_volume_count);
    XSAN_LOG_INFO("Found %d XSAN volumes after initial metadata load:", initial_volume_count);
    for (int i = 0; i < initial_volume_count; ++i) { /* ... log vol info ... */
        XSAN_LOG_INFO("  Volume: %s, Size: %luMB", initial_volumes[i]->name, initial_volumes[i]->size_bytes / (1024*1024));
    }
    if(initial_volumes) xsan_volume_manager_free_volume_pointer_list(initial_volumes);


    XSAN_LOG_INFO("=== Test Run: Creating Test Entities ===");
    xsan_group_id_t test_dg_id; memset(&test_dg_id, 0, sizeof(test_dg_id));
    xsan_volume_id_t test_vol_id; memset(&test_vol_id, 0, sizeof(test_vol_id));
    bool dg_created = false; bool vol_created = false;
    xsan_disk_t* disk_for_group = NULL;

    if (initial_disk_count > 0) {
        // Try to find an unassigned Malloc disk for testing
        for (int i = 0; i < initial_disk_count; ++i) {
            xsan_disk_t* current_disk_check = xsan_disk_manager_find_disk_by_bdev_name(dm, initial_disks[i]->bdev_name);
            if (current_disk_check && strstr(current_disk_check->bdev_name, "Malloc") && spdk_uuid_is_null((struct spdk_uuid*)&current_disk_check->assigned_to_group_id.data[0])) {
                disk_for_group = current_disk_check;
                break;
            }
        }
        if(!disk_for_group) disk_for_group = xsan_disk_manager_find_disk_by_bdev_name(dm, initial_disks[0]->bdev_name); // Fallback
    }

    if (disk_for_group && spdk_uuid_is_null((struct spdk_uuid*)&disk_for_group->assigned_to_group_id.data[0])) {
        const char *dg_bdevs[] = {disk_for_group->bdev_name};
        err = xsan_disk_manager_disk_group_create(dm, "MetaTestDG", XSAN_DISK_GROUP_TYPE_PASSSTHROUGH, dg_bdevs, 1, &test_dg_id);
        if (err == XSAN_OK) { XSAN_LOG_INFO("Created 'MetaTestDG'."); dg_created = true; }
        else { XSAN_LOG_ERROR("Failed to create 'MetaTestDG': %s", xsan_error_string(err)); }
    } else { XSAN_LOG_WARN("No suitable unassigned disk found for 'MetaTestDG' or no disks available."); }

    if (dg_created) {
        uint64_t vol_s = disk_for_group->capacity_bytes > (128*1024*1024) ? (128*1024*1024) : disk_for_group->capacity_bytes/2;
        if (vol_s < 4096) vol_s = 4096; // Min 4k
        err = xsan_volume_create(vm, "MetaTestVol", vol_s, test_dg_id, 4096, false, &test_vol_id);
        if (err == XSAN_OK) { XSAN_LOG_INFO("Created 'MetaTestVol'."); vol_created = true; }
        else { XSAN_LOG_ERROR("Failed to create 'MetaTestVol': %s", xsan_error_string(err)); }
    }

    // --- Async I/O Test on the created volume if successful ---
    if (vol_created) {
        xsan_volume_t *vol_for_async_test = xsan_volume_get_by_id(vm, test_vol_id);
        if (vol_for_async_test && disk_for_group) { // disk_for_group is the disk backing this volume
             g_async_test_controller.all_tests_finished_signal = false;
            _start_async_io_test_on_volume(&g_async_test_controller, vol_for_async_test);

            XSAN_LOG_INFO("Waiting for async I/O test to complete...");
            while(!g_async_test_controller.all_tests_finished_signal) {
                spdk_thread_poll(spdk_get_thread(), 0, 0);
            }
            XSAN_LOG_INFO("Async I/O test completed or was skipped/failed early.");
        } else {
             XSAN_LOG_ERROR("Could not find created volume or its disk for async I/O test.");
        }
    } else {
        XSAN_LOG_WARN("Skipping async I/O test as 'MetaTestVol' was not created.");
        // If no async test, we need to signal app stop some other way or proceed.
        // For this test structure, if async IO test doesn't run, we'll proceed to cleanup.
        g_async_test_controller.all_tests_finished_signal = true; // Allow main loop to proceed to cleanup
    }


    // --- Cleanup Test Entities for Persistence Check ---
    if (vol_created) {
        XSAN_LOG_INFO("Deleting 'MetaTestVol' for persistence test...");
        xsan_volume_delete(vm, test_vol_id);
    }
    if (dg_created) {
        XSAN_LOG_INFO("Deleting 'MetaTestDG' for persistence test...");
        xsan_disk_manager_disk_group_delete(dm, test_dg_id);
    }

    // --- Finalization (will be called after loop breaks due to signal) ---
app_stop_dm_fini:
    if (dm) xsan_disk_manager_fini(&dm);
app_stop_bdev_fini_only:
    xsan_bdev_subsystem_fini();
app_stop_no_cleanup:
    if (vm) xsan_volume_manager_fini(&vm);

    XSAN_LOG_INFO("XSAN SPDK application thread work complete. Requesting application stop.");
    xsan_spdk_manager_request_app_stop();
}

int main(int argc, char **argv) {
    xsan_log_config_t log_cfg = xsan_log_default_config();
    log_cfg.level = XSAN_LOG_LEVEL_DEBUG;
    xsan_log_init(&log_cfg);

    XSAN_LOG_INFO("XSAN Node starting (main function)...");
    const char *spdk_json_conf_file = NULL;
    const char *db_path_override = NULL; // For testing, allow overriding DB path

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--spdk-conf") == 0 && i + 1 < argc) {
            spdk_json_conf_file = argv[++i];
        } else if (strcmp(argv[i], "--meta-db-path") == 0 && i + 1 < argc) {
            // This is a conceptual argument, not actually used by current init funcs directly.
            // xsan_disk_manager_init and xsan_volume_manager_init hardcode paths for now.
            // This would require passing the path to them.
            db_path_override = argv[++i];
            XSAN_LOG_INFO("User specified metadata DB path override (concept): %s", db_path_override);
        } else if (spdk_json_conf_file == NULL) { // Assume first non-flag arg is SPDK conf
            spdk_json_conf_file = argv[i];
        }
    }

    if (spdk_json_conf_file) {
        XSAN_LOG_INFO("Using SPDK JSON configuration file: %s", spdk_json_conf_file);
    } else { /* ... Log warnings about missing config ... */ }

    // To test persistence, one might want to clean the DB dir before the first run:
    // system("rm -rf ./xsan_meta_db"); // Use with extreme caution!
    // mkdir("./xsan_meta_db", 0755); // Ensure base dir exists
    // mkdir("./xsan_meta_db/disk_manager_db", 0755);
    // mkdir("./xsan_meta_db/volume_manager_db", 0755);


    xsan_error_t err = xsan_spdk_manager_opts_init("xsan_node_main", spdk_json_conf_file, "0x1", false, NULL);
    if (err != XSAN_OK) { /* ... fatal ... */ return EXIT_FAILURE; }

    err = xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start, NULL);
    if (err != XSAN_OK) { /* ... fatal ... */ }

    xsan_spdk_manager_app_fini();
    XSAN_LOG_INFO("XSAN Node has shut down.");
    xsan_log_shutdown();
    return (err == XSAN_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
