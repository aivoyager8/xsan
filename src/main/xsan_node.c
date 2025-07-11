#include "xsan_log.h"
#include "xsan_config.h"
#include "xsan_spdk_manager.h"
#include "xsan_bdev.h"
#include "xsan_disk_manager.h"
#include "xsan_volume_manager.h"
#include "xsan_io.h" // For xsan_user_io_completion_cb_t
#include "xsan_error.h"
#include "xsan_string_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "spdk/uuid.h"
#include "spdk/env.h"


// --- Async I/O Test Context and Callbacks ---
typedef struct {
    xsan_volume_manager_t *vm;
    xsan_disk_manager_t *dm; // To find disk for bdev name if needed
    xsan_volume_id_t volume_id_to_test;
    char target_bdev_name_for_log[XSAN_MAX_NAME_LEN];
    uint32_t io_block_size; // Block size used for this specific I/O test
    size_t dma_alignment;

    void *write_buffer_dma;
    void *read_buffer_dma;

    enum {
        ASYNC_IO_TEST_IDLE,
        ASYNC_IO_TEST_WRITE_SUBMITTED,
        ASYNC_IO_TEST_READ_SUBMITTED,
        ASYNC_IO_TEST_VERIFY_DONE, // Successfully completed and verified
        ASYNC_IO_TEST_FAILED       // An error occurred
    } test_state;

    int outstanding_io_ops; // Counter for pending async operations for this test
    bool all_tests_finished_signal; // Signal to main SPDK thread function to proceed to cleanup
} xsan_async_io_test_control_t;

// Make it global for easy access in callbacks and the main SPDK app function.
// In a real app, this context would be managed more carefully, perhaps per-test-suite.
static xsan_async_io_test_control_t g_async_test_controller;

// Forward declaration for the read part of the test
static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl);
static void _finalize_and_stop_app_if_all_done(xsan_async_io_test_control_t *test_ctrl);


static void _async_io_test_read_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *test_ctrl = (xsan_async_io_test_control_t *)cb_arg;
    test_ctrl->outstanding_io_ops--;

    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);

    if (status != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncTest] Read from volume %s FAILED: %s (code %d)",
                       vol_id_str, xsan_error_string(status), status);
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
    } else {
        XSAN_LOG_INFO("[AsyncTest] Read from volume %s successful.", vol_id_str);
        if (memcmp(test_ctrl->write_buffer_dma, test_ctrl->read_buffer_dma, test_ctrl->io_block_size) == 0) {
            XSAN_LOG_INFO("SUCCESS: [AsyncTest] R/W data verification for volume %s PASSED!", vol_id_str);
            test_ctrl->test_state = ASYNC_IO_TEST_VERIFY_DONE;
        } else {
            XSAN_LOG_ERROR("FAILURE: [AsyncTest] R/W data verification for volume %s FAILED!", vol_id_str);
            // For debugging, one might print parts of the buffers here
            test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        }
    }
    _finalize_and_stop_app_if_all_done(test_ctrl);
}

static void _run_async_io_test_read_phase(xsan_async_io_test_control_t *test_ctrl) {
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    XSAN_LOG_DEBUG("[AsyncTest] Submitting async read for volume %s, offset 0, length %u...",
                   vol_id_str, test_ctrl->io_block_size);

    test_ctrl->test_state = ASYNC_IO_TEST_READ_SUBMITTED;
    test_ctrl->outstanding_io_ops++;

    xsan_error_t err = xsan_volume_read_async(test_ctrl->vm, test_ctrl->volume_id_to_test,
                                              0, // logical byte offset
                                              test_ctrl->io_block_size, // length bytes
                                              test_ctrl->read_buffer_dma,
                                              _async_io_test_read_complete_cb, test_ctrl);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncTest] Failed to submit async read for volume %s: %s", vol_id_str, xsan_error_string(err));
        test_ctrl->outstanding_io_ops--;
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_and_stop_app_if_all_done(test_ctrl);
    }
}

static void _async_io_test_write_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_async_io_test_control_t *test_ctrl = (xsan_async_io_test_control_t *)cb_arg;
    test_ctrl->outstanding_io_ops--;

    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);

    if (status != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncTest] Write to volume %s FAILED: %s (code %d)",
                       vol_id_str, xsan_error_string(status), status);
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_and_stop_app_if_all_done(test_ctrl);
    } else {
        XSAN_LOG_INFO("[AsyncTest] Write to volume %s successful. Proceeding to read phase.", vol_id_str);
        _run_async_io_test_read_phase(test_ctrl);
        // Note: _finalize_and_stop_app_if_all_done will be called by read's completion
    }
}

static void _start_async_io_test_on_volume(xsan_async_io_test_control_t *test_ctrl, xsan_volume_t *vol_to_test) {
    if (!vol_to_test || vol_to_test->block_size_bytes == 0 || vol_to_test->num_blocks == 0) {
        XSAN_LOG_ERROR("[AsyncTest] Invalid volume provided for test or volume has no space/zero block size.");
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_and_stop_app_if_all_done(test_ctrl);
        return;
    }

    memcpy(&test_ctrl->volume_id_to_test, &vol_to_test->id, sizeof(xsan_volume_id_t));
    test_ctrl->io_block_size = vol_to_test->block_size_bytes; // Use volume's logical block size for test I/O unit

    // Need bdev name for alignment info (could also get physical block size from mapping if needed)
    // This assumes simple mapping to first disk of the group for now to get a bdev name.
    xsan_disk_group_t *group = xsan_disk_manager_find_disk_group_by_id(test_ctrl->dm, vol_to_test->source_group_id);
    if (!group || group->disk_count == 0) {
        XSAN_LOG_ERROR("[AsyncTest] Volume '%s' has invalid or empty source group. Cannot get bdev for alignment.", vol_to_test->name);
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_and_stop_app_if_all_done(test_ctrl);
        return;
    }
    xsan_disk_t *first_disk_in_group = xsan_disk_manager_find_disk_by_id(test_ctrl->dm, group->disk_ids[0]);
    if (!first_disk_in_group) {
        XSAN_LOG_ERROR("[AsyncTest] Could not find first disk of group for volume '%s'.", vol_to_test->name);
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_and_stop_app_if_all_done(test_ctrl);
        return;
    }
    xsan_strcpy_safe(test_ctrl->target_bdev_name_for_log, first_disk_in_group->bdev_name, XSAN_MAX_NAME_LEN);
    test_ctrl->dma_alignment = xsan_bdev_get_buf_align(first_disk_in_group->bdev_name);


    test_ctrl->write_buffer_dma = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);
    test_ctrl->read_buffer_dma = xsan_bdev_dma_malloc(test_ctrl->io_block_size, test_ctrl->dma_alignment);

    if (!test_ctrl->write_buffer_dma || !test_ctrl->read_buffer_dma) {
        XSAN_LOG_ERROR("[AsyncTest] Failed to allocate DMA buffers for async I/O test.");
        if(test_ctrl->write_buffer_dma) xsan_bdev_dma_free(test_ctrl->write_buffer_dma);
        if(test_ctrl->read_buffer_dma) xsan_bdev_dma_free(test_ctrl->read_buffer_dma);
        test_ctrl->write_buffer_dma = NULL; test_ctrl->read_buffer_dma = NULL;
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_and_stop_app_if_all_done(test_ctrl);
        return;
    }

    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]);
    snprintf((char*)test_ctrl->write_buffer_dma, test_ctrl->io_block_size, "XSAN Async Test! Vol %s", vol_id_str);
    for(size_t k=strlen((char*)test_ctrl->write_buffer_dma); k < test_ctrl->io_block_size; ++k) {
        ((char*)test_ctrl->write_buffer_dma)[k] = (char)((k % 250) + 5);
    }
    memset(test_ctrl->read_buffer_dma, 0xDD, test_ctrl->io_block_size); // Fill with a different pattern

    XSAN_LOG_INFO("--- Starting Async R/W Test on Volume ID %s (bdev: '%s', io_block_size: %u) ---",
                  vol_id_str, test_ctrl->target_bdev_name_for_log, test_ctrl->io_block_size);
    XSAN_LOG_DEBUG("[AsyncTest] Submitting async write for volume %s, offset 0, length %u...", vol_id_str, test_ctrl->io_block_size);

    test_ctrl->test_state = ASYNC_IO_TEST_WRITE_SUBMITTED;
    test_ctrl->outstanding_io_ops++;
    xsan_error_t err = xsan_volume_write_async(test_ctrl->vm, test_ctrl->volume_id_to_test,
                                     0, // logical byte offset
                                     test_ctrl->io_block_size, // length bytes
                                     test_ctrl->write_buffer_dma,
                                     _async_io_test_write_complete_cb, test_ctrl);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("[AsyncTest] Failed to submit async write for volume %s: %s", vol_id_str, xsan_error_string(err));
        test_ctrl->outstanding_io_ops--;
        test_ctrl->test_state = ASYNC_IO_TEST_FAILED;
        _finalize_and_stop_app_if_all_done(test_ctrl);
    }
    // If submission OK, callbacks will drive the rest and eventually call _finalize_and_stop_app_if_all_done
}

static void _finalize_and_stop_app_if_all_done(xsan_async_io_test_control_t *test_ctrl) {
    if (test_ctrl->outstanding_io_ops == 0) {
        XSAN_LOG_INFO("[AsyncTest] Async I/O test sequence on volume %s finished with state: %d.",
                      spdk_uuid_get_string((struct spdk_uuid*)&test_ctrl->volume_id_to_test.data[0]),
                      test_ctrl->test_state);

        if(test_ctrl->write_buffer_dma) xsan_bdev_dma_free(test_ctrl->write_buffer_dma);
        if(test_ctrl->read_buffer_dma) xsan_bdev_dma_free(test_ctrl->read_buffer_dma);
        test_ctrl->write_buffer_dma = NULL;
        test_ctrl->read_buffer_dma = NULL;

        test_ctrl->all_tests_finished_signal = true; // Signal main SPDK app func
        // Do NOT call xsan_spdk_manager_request_app_stop() directly from a SPDK IO completion callback
        // if that callback might be on a different thread than the main app thread that needs to clean up.
        // Instead, signal the main app thread. For this simple case where xsan_node_main_spdk_thread_start
        // is the one waiting, it can check this flag. Or, if everything is on one reactor, can call stop.
        // For robustness, it's often better to send a message to the primary reactor to request shutdown.
        // Here, we set a flag and the main app loop will see it.
    }
}
// --- End of Async I/O Test ---


static void xsan_node_main_spdk_thread_start(void *arg1, int rc) {
    (void)arg1;
    xsan_error_t err = XSAN_OK;
    xsan_disk_manager_t *dm = NULL;
    xsan_volume_manager_t *vm = NULL;

    XSAN_LOG_INFO("XSAN SPDK application thread started (spdk_app_start internal rc: %d).", rc);

    if (rc != 0) {
        XSAN_LOG_FATAL("SPDK framework initialization failed prior to calling app main. Cannot proceed.");
        goto app_stop_no_cleanup;
    }

    if (xsan_bdev_subsystem_init() != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN bdev subsystem.");
        goto app_stop_no_cleanup;
    }

    err = xsan_disk_manager_init(&dm);
    if (err != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN Disk Manager: %s (code %d)", xsan_error_string(err), err);
        goto app_stop_bdev_fini_only;
    }
    g_async_test_controller.dm = dm; // Give test context access to disk manager

    err = xsan_volume_manager_init(dm, &vm);
    if (err != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN Volume Manager: %s (code %d)", xsan_error_string(err), err);
        goto app_stop_dm_fini;
    }
    g_async_test_controller.vm = vm; // Give test context access to volume manager

    err = xsan_disk_manager_scan_and_register_bdevs(dm);
    if (err != XSAN_OK && err != XSAN_ERROR_NOT_FOUND ) {
        XSAN_LOG_ERROR("Error during bdev scan and registration: %s (code %d)", xsan_error_string(err), err);
    }

    xsan_disk_t **xsan_disks_list_ptr = NULL;
    int xsan_disk_count = 0;
    err = xsan_disk_manager_get_all_disks(dm, &xsan_disks_list_ptr, &xsan_disk_count);
    if (err == XSAN_OK) {
        XSAN_LOG_INFO("XSAN Disk Manager discovered %d disk(s):", xsan_disk_count);
        for (int i = 0; i < xsan_disk_count; ++i) {
            char xsan_disk_id_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(xsan_disk_id_str, sizeof(xsan_disk_id_str), (struct spdk_uuid*)&xsan_disks_list_ptr[i]->id.data[0]);
            XSAN_LOG_INFO("  Disk [%d]: XSAN_ID=%s, BDevName='%s', Size=%.2f GiB, Product='%s'",
                          i, xsan_disk_id_str, xsan_disks_list_ptr[i]->bdev_name,
                          (double)xsan_disks_list_ptr[i]->capacity_bytes / (1024.0*1024.0*1024.0),
                          xsan_disks_list_ptr[i]->product_name);
        }
    } else {
        XSAN_LOG_ERROR("Failed to get all XSAN disks: %s (code %d)", xsan_error_string(err), err);
    }

    xsan_group_id_t test_group_id;
    memset(&test_group_id, 0, sizeof(xsan_group_id_t));
    bool group_created_flag = false;
    xsan_disk_t *first_disk_for_group = NULL;

    if (xsan_disk_count > 0 && xsan_disks_list_ptr) {
        first_disk_for_group = xsan_disks_list_ptr[0]; // Candidate for group
        const char *bdev_names_for_group[1] = { first_disk_for_group->bdev_name };
        XSAN_LOG_INFO("Attempting to create disk group 'DG1' with bdev '%s'...", bdev_names_for_group[0]);
        err = xsan_disk_manager_disk_group_create(dm, "DG1", XSAN_DISK_GROUP_TYPE_PASSSTHROUGH,
                                                  bdev_names_for_group, 1, &test_group_id);
        if (err == XSAN_OK) {
            group_created_flag = true;
            char group_id_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(group_id_str, sizeof(group_id_str), (struct spdk_uuid*)&test_group_id.data[0]);
            XSAN_LOG_INFO("Disk group 'DG1' created successfully, ID: %s", group_id_str);
        } else {
            XSAN_LOG_ERROR("Failed to create disk group 'DG1': %s (code %d)", xsan_error_string(err), err);
        }
    } else {
        XSAN_LOG_WARN("Skipping disk group creation test: no XSAN disks available.");
    }

    xsan_volume_id_t test_volume_id;
    memset(&test_volume_id, 0, sizeof(xsan_volume_id_t));
    bool volume_created_flag = false;
    xsan_volume_t *created_volume_for_test = NULL;

    if (group_created_flag) {
        uint64_t vol_size_mb = (first_disk_for_group->capacity_bytes / (1024*1024) > 256) ? 128 : (first_disk_for_group->capacity_bytes / (1024*1024))/2; // Ensure vol size fits disk
        if(vol_size_mb == 0) vol_size_mb = 1; // Min 1MB for test if disk is tiny
        uint64_t vol_size_bytes_test = vol_size_mb * 1024 * 1024;
        uint32_t vol_block_size_test = 4096;
        if (vol_size_bytes_test < vol_block_size_test) vol_size_bytes_test = vol_block_size_test;


        XSAN_LOG_INFO("Attempting to create volume 'Vol1' (%.0f MiB, blocksize %uB) on group 'DG1'...", (double)vol_size_mb, vol_block_size_test);
        err = xsan_volume_create(vm, "Vol1", vol_size_bytes_test, test_group_id, vol_block_size_test, false, &test_volume_id);
        if (err == XSAN_OK) {
            volume_created_flag = true;
            created_volume_for_test = xsan_volume_get_by_id(vm, test_volume_id); // Get the created volume struct
            char vol_id_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_volume_id.data[0]);
            XSAN_LOG_INFO("Volume 'Vol1' created successfully, ID: %s", vol_id_str);
        } else {
            XSAN_LOG_ERROR("Failed to create volume 'Vol1': %s (code %d)", xsan_error_string(err), err);
        }
    } else {
        XSAN_LOG_WARN("Skipping volume creation test: disk group 'DG1' was not created.");
    }

    if (xsan_disks_list_ptr) {
        xsan_disk_manager_free_disk_pointer_list(xsan_disks_list_ptr);
        xsan_disks_list_ptr = NULL;
    }

    // --- Async I/O Test ---
    if (volume_created_flag && created_volume_for_test && first_disk_for_group) {
        g_async_test_controller.all_tests_finished_signal = false;
        _start_async_io_test_on_volume(&g_async_test_controller, created_volume_for_test);
        // The rest of the cleanup and app_stop will be triggered by the async callbacks
        // via _finalize_and_stop_app_if_all_done setting all_tests_finished_signal.
        // This SPDK thread needs to keep polling for completions.
        // If _start_async_io_test_on_volume itself failed to submit, it might have already set the signal.
        while(!g_async_test_controller.all_tests_finished_signal) {
            // This is a busy wait if this thread isn't a reactor.
            // Assuming this main app function is on a reactor, completions will be polled.
            // If not, we must explicitly poll the thread this IO was submitted on.
            // For simplicity, we assume this thread is the one that will poll.
            // In a multi-reactor setup, IOs are submitted to a specific reactor.
            spdk_thread_poll(spdk_get_thread(), 0, 0); // Poll current thread for completions
        }
        // Now that async test is done (or failed early), proceed to cleanup the test volume/group
    }


    // --- Cleanup Tests ---
    if (volume_created_flag) {
        char vol_id_str_del[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(vol_id_str_del, sizeof(vol_id_str_del), (struct spdk_uuid*)&test_volume_id.data[0]);
        XSAN_LOG_INFO("Attempting to delete volume 'Vol1' (ID: %s)...", vol_id_str_del);
        err = xsan_volume_delete(vm, test_volume_id);
        if (err == XSAN_OK) XSAN_LOG_INFO("Volume 'Vol1' deleted successfully.");
        else XSAN_LOG_ERROR("Failed to delete volume 'Vol1': %s (code %d)", xsan_error_string(err), err);
    }

    if (group_created_flag) {
        char group_id_str_del[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(group_id_str_del, sizeof(group_id_str_del), (struct spdk_uuid*)&test_group_id.data[0]);
        XSAN_LOG_INFO("Attempting to delete disk group 'DG1' (ID: %s)...", group_id_str_del);
        err = xsan_disk_manager_disk_group_delete(dm, test_group_id);
        if (err == XSAN_OK) XSAN_LOG_INFO("Disk group 'DG1' deleted successfully.");
        else XSAN_LOG_ERROR("Failed to delete disk group 'DG1': %s (code %d)", xsan_error_string(err), err);
    }

app_stop_dm_fini:
    if (dm) xsan_disk_manager_fini(&dm);
app_stop_bdev_fini_only:
    xsan_bdev_subsystem_fini();
app_stop_no_cleanup:
    if (vm) xsan_volume_manager_fini(&vm);

    XSAN_LOG_INFO("XSAN SPDK application thread work complete or aborted. Requesting application stop.");
    xsan_spdk_manager_request_app_stop();
}

// main function remains largely the same
int main(int argc, char **argv) {
    xsan_log_config_t log_cfg = xsan_log_default_config();
    log_cfg.level = XSAN_LOG_LEVEL_DEBUG;
    xsan_log_init(&log_cfg);

    XSAN_LOG_INFO("XSAN Node starting (main function)...");

    const char *spdk_json_conf_file = NULL;
    if (argc > 1) {
        spdk_json_conf_file = argv[1];
        XSAN_LOG_INFO("Using SPDK JSON configuration file: %s", spdk_json_conf_file);
    } else {
        XSAN_LOG_WARN("No SPDK JSON configuration file provided via command line arguments.");
        XSAN_LOG_WARN("For testing with Malloc bdevs, create a JSON config (e.g., bdev_malloc.json) and pass it.");
        XSAN_LOG_WARN("Example JSON for Malloc bdev: {\"subsystems\":[{\"subsystem\":\"bdev\",\"config\":[{\"method\":\"bdev_malloc_create\",\"params\":{\"name\":\"Malloc0\",\"num_blocks\":131072,\"block_size\":4096}}]}]}");
    }

    xsan_error_t err = xsan_spdk_manager_opts_init("xsan_node_main", spdk_json_conf_file, "0x1", false, NULL);
    if (err != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize SPDK manager options: %s (code %d)", xsan_error_string(err), err);
        xsan_log_shutdown();
        return EXIT_FAILURE;
    }

    err = xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start, NULL);
    if (err != XSAN_OK) {
        XSAN_LOG_FATAL("xsan_spdk_manager_start_app returned with error: %s (code %d)", xsan_error_string(err), err);
    }

    xsan_spdk_manager_app_fini();
    XSAN_LOG_INFO("XSAN Node has shut down.");
    xsan_log_shutdown();
    return (err == XSAN_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
