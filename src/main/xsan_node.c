#include "xsan_log.h"
#include "xsan_config.h"        // May not be used directly here, but good to have
#include "xsan_spdk_manager.h"
#include "xsan_bdev.h"
#include "xsan_error.h"         // For error strings
#include "xsan_string_utils.h"  // For xsan_strcpy_safe if needed

#include <stdio.h>
#include <stdlib.h>             // For EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>             // For memset, strstr, memcmp
#include <unistd.h>             // For sleep (debugging)

// SPDK Headers needed for this test/main file specifically
#include "spdk/uuid.h"          // For spdk_uuid_fmt_lower, SPDK_UUID_STRING_LEN
#include "spdk/env.h"           // For spdk_dma_malloc/free (though wrapped by xsan_bdev)

// Global flag to indicate if the main SPDK application part is still running.
// Useful if the main_start function is non-blocking and we need to wait.
// In this simple case, main_start itself will call for stop.
// static volatile bool g_xsan_app_is_running = false;

/**
 * @brief Main application function executed on an SPDK reactor thread.
 * This function is passed to xsan_spdk_manager_start_app.
 *
 * @param arg1 Custom argument passed from xsan_spdk_manager_start_app (currently NULL).
 * @param rc Return code from the SPDK framework's internal start process. 0 on success.
 */
static void xsan_node_main_spdk_thread_start(void *arg1, int rc) {
    (void)arg1; // Unused argument

    XSAN_LOG_INFO("XSAN SPDK application thread started (spdk_app_start internal rc: %d).", rc);

    if (rc != 0) {
        XSAN_LOG_FATAL("SPDK framework initialization failed prior to calling app main. Cannot proceed.");
        // xsan_spdk_manager_request_app_stop(); // spdk_app_start would have failed and returned.
                                               // No need to request stop if we didn't even start fully.
        return;
    }

    xsan_error_t err = XSAN_OK;
    xsan_disk_manager_t *dm = NULL;

    // Initialize XSAN bdev subsystem (SPDK bdev interaction layer)
    if (xsan_bdev_subsystem_init() != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN bdev subsystem.");
        goto app_stop;
    }

    // Initialize XSAN Disk Manager
    err = xsan_disk_manager_init(&dm);
    if (err != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN Disk Manager: %s (code %d)", xsan_error_string(err), err);
        xsan_bdev_subsystem_fini(); // Clean up bdev subsystem
        goto app_stop;
    }

    // Scan and register SPDK bdevs into the Disk Manager
    err = xsan_disk_manager_scan_and_register_bdevs(dm);
    if (err != XSAN_OK && err != XSAN_ERROR_NOT_FOUND /* allow no bdevs found */) {
        XSAN_LOG_ERROR("Error during bdev scan and registration: %s (code %d)", xsan_error_string(err), err);
        // Continue to see what was registered, but log error
    }

    // List all XSAN disks (which are based on SPDK bdevs)
    xsan_disk_t **xsan_disks_list = NULL;
    int xsan_disk_count = 0;
    err = xsan_disk_manager_get_all_disks(dm, &xsan_disks_list, &xsan_disk_count);
    if (err == XSAN_OK) {
        XSAN_LOG_INFO("XSAN Disk Manager found %d disk(s):", xsan_disk_count);
        for (int i = 0; i < xsan_disk_count; ++i) {
            char xsan_disk_id_str[SPDK_UUID_STRING_LEN];
            char bdev_uuid_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(xsan_disk_id_str, sizeof(xsan_disk_id_str), (struct spdk_uuid*)&xsan_disks_list[i]->id.data[0]);
            spdk_uuid_fmt_lower(bdev_uuid_str, sizeof(bdev_uuid_str), (struct spdk_uuid*)&xsan_disks_list[i]->bdev_uuid.data[0]);

            XSAN_LOG_INFO("  [%d] XSAN_ID: %s, BDevName: '%s', Product: '%s', Size: %.2f GiB, BDevUUID: %s, BlockSize: %u",
                          i, xsan_disk_id_str, xsan_disks_list[i]->bdev_name,
                          xsan_disks_list[i]->product_name,
                          (double)xsan_disks_list[i]->capacity_bytes / (1024.0 * 1024.0 * 1024.0),
                          bdev_uuid_str, xsan_disks_list[i]->block_size_bytes);
        }
        xsan_disk_manager_free_disk_pointer_list(xsan_disks_list); // Free the array of pointers
    } else {
        XSAN_LOG_ERROR("Failed to get all XSAN disks: %s (code %d)", xsan_error_string(err), err);
    }

    // Attempt to create a disk group if at least one disk is available
    xsan_group_id_t test_group_id;
    memset(&test_group_id, 0, sizeof(xsan_group_id_t)); // For checking if it's populated

    if (xsan_disk_count > 0) {
        const char *disks_for_group[1] = { xsan_disk_manager_find_disk_by_bdev_name(dm, xsan_disks_list[0]->bdev_name)->bdev_name }; // Use first disk's bdev name
        XSAN_LOG_INFO("Attempting to create disk group 'TestGroup1' with disk '%s'...", disks_for_group[0]);
        err = xsan_disk_manager_disk_group_create(dm, "TestGroup1", XSAN_DISK_GROUP_TYPE_PASSSTHROUGH,
                                                  disks_for_group, 1, &test_group_id);
        if (err == XSAN_OK) {
            char group_id_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(group_id_str, sizeof(group_id_str), (struct spdk_uuid*)&test_group_id.data[0]);
            XSAN_LOG_INFO("Disk group 'TestGroup1' created successfully with ID: %s", group_id_str);

            // List disk groups
            xsan_disk_group_t **groups_list = NULL;
            int group_count = 0;
            if (xsan_disk_manager_get_all_disk_groups(dm, &groups_list, &group_count) == XSAN_OK) {
                XSAN_LOG_INFO("Found %d disk group(s):", group_count);
                for (int i = 0; i < group_count; ++i) {
                     char grp_id_str_list[SPDK_UUID_STRING_LEN];
                     spdk_uuid_fmt_lower(grp_id_str_list, sizeof(grp_id_str_list), (struct spdk_uuid*)&groups_list[i]->id.data[0]);
                     XSAN_LOG_INFO("  - Name: '%s', ID: %s, Type: %d, DiskCount: %u",
                                  groups_list[i]->name, grp_id_str_list, groups_list[i]->type, groups_list[i]->disk_count);
                }
                xsan_disk_manager_free_group_pointer_list(groups_list);
            }
        } else {
            XSAN_LOG_ERROR("Failed to create disk group 'TestGroup1': %s (code %d)", xsan_error_string(err), err);
        }
    } else {
        XSAN_LOG_WARN("Skipping disk group creation test as no XSAN disks were found/registered.");
    }

    // Simple Read/Write Test on the first Malloc bdev found via xsan_disk_t
    if (xsan_disk_count > 0) {
        xsan_disk_t* test_disk = NULL;
        for(int i=0; i<xsan_disk_count; ++i) {
            // Find the first disk that is likely a Malloc bdev for safe testing
             xsan_disk_t* current_disk = xsan_disk_manager_find_disk_by_bdev_name(dm, xsan_disks_list[i]->bdev_name);
             if(current_disk && strstr(current_disk->bdev_name, "Malloc") != NULL) {
                 test_disk = current_disk;
                 break;
             }
        }

        if (test_disk && test_disk->num_blocks > 0 && test_disk->block_size_bytes > 0) {
            XSAN_LOG_INFO("--- Performing R/W Test on XSAN disk (bdev: '%s') ---", test_disk->bdev_name);
            uint32_t blk_size_test = test_disk->block_size_bytes;
            size_t bdev_align = xsan_bdev_get_buf_align(test_disk->bdev_name); // Use bdev_name

            void *write_buf = xsan_bdev_dma_malloc(blk_size_test, bdev_align);
            void *read_buf = xsan_bdev_dma_malloc(blk_size_test, bdev_align);

            if (write_buf && read_buf) {
                snprintf((char*)write_buf, blk_size_test, "XSAN R/W Test on %s! Block 0.", test_disk->bdev_name);
                for(size_t k=strlen((char*)write_buf); k<blk_size_test; ++k) ((char*)write_buf)[k] = (char)(k % 256);
                memset(read_buf, 0xAA, blk_size_test);

                XSAN_LOG_DEBUG("Writing 1 block (offset 0) to '%s'...", test_disk->bdev_name);
                err = xsan_bdev_write_sync(test_disk->bdev_name, 0, 1, write_buf, blk_size_test, false);
                if (err == XSAN_OK) {
                    XSAN_LOG_INFO("Write to '%s' successful.", test_disk->bdev_name);
                    XSAN_LOG_DEBUG("Reading 1 block (offset 0) from '%s'...", test_disk->bdev_name);
                    err = xsan_bdev_read_sync(test_disk->bdev_name, 0, 1, read_buf, blk_size_test, false);
                    if (err == XSAN_OK) {
                        XSAN_LOG_INFO("Read from '%s' successful.", test_disk->bdev_name);
                        if (memcmp(write_buf, read_buf, blk_size_test) == 0) {
                            XSAN_LOG_INFO("SUCCESS: R/W data verification for '%s' passed!", test_disk->bdev_name);
                        } else {
                            XSAN_LOG_ERROR("FAILURE: R/W data verification for '%s' FAILED!", test_disk->bdev_name);
                        }
                    } else {
                        XSAN_LOG_ERROR("Read from '%s' failed: %s (code %d)", test_disk->bdev_name, xsan_error_string(err), err);
                    }
                } else {
                    XSAN_LOG_ERROR("Write to '%s' failed: %s (code %d)", test_disk->bdev_name, xsan_error_string(err), err);
                }
            } else {
                XSAN_LOG_ERROR("Failed to allocate DMA buffers for R/W test on '%s'.", test_disk->bdev_name);
            }
            if(write_buf) xsan_bdev_dma_free(write_buf);
            if(read_buf) xsan_bdev_dma_free(read_buf);
            XSAN_LOG_INFO("--- R/W Test on '%s' finished ---", test_disk->bdev_name);
        }
    }


    // Attempt to delete the created disk group if it was created
    if (!spdk_uuid_is_null((struct spdk_uuid*)&test_group_id.data[0])) {
        XSAN_LOG_INFO("Attempting to delete disk group 'TestGroup1' (ID: %s)...", spdk_uuid_get_string((struct spdk_uuid*)&test_group_id.data[0]));
        err = xsan_disk_manager_disk_group_delete(dm, test_group_id);
        if (err == XSAN_OK) {
            XSAN_LOG_INFO("Disk group 'TestGroup1' deleted successfully.");
        } else {
            XSAN_LOG_ERROR("Failed to delete disk group 'TestGroup1': %s (code %d)", xsan_error_string(err), err);
        }
    }

    // Finalize XSAN subsystems
    xsan_disk_manager_fini(&dm); // dm will be set to NULL
    xsan_bdev_subsystem_fini();

app_stop:
    XSAN_LOG_INFO("XSAN SPDK application thread work complete. Requesting application stop.");
    xsan_spdk_manager_request_app_stop(); // Signal SPDK framework to shut down
}

int main(int argc, char **argv) {
    xsan_log_config_t log_cfg = xsan_log_default_config();
    log_cfg.level = XSAN_LOG_LEVEL_DEBUG; // Enable verbose logging for testing
    // log_cfg.console_output = true; // Already default
    xsan_log_init(&log_cfg);

    XSAN_LOG_INFO("XSAN Node starting (main function)...");

    const char *spdk_json_conf_file = NULL;
    if (argc > 1) {
        spdk_json_conf_file = argv[1];
        XSAN_LOG_INFO("Using SPDK JSON configuration file: %s", spdk_json_conf_file);
    } else {
        XSAN_LOG_WARN("No SPDK JSON configuration file provided via command line arguments.");
        XSAN_LOG_WARN("SPDK might not find bdevs unless auto-detected (e.g. NVMe) or created via RPC if enabled.");
        XSAN_LOG_WARN("For testing with Malloc bdevs, create a JSON config like:");
        XSAN_LOG_WARN("{\n  \"subsystems\": [\n    {\n      \"subsystem\": \"bdev\",\n      \"config\": [\n        {\n          \"method\": \"bdev_malloc_create\",\n          \"params\": {\n            \"name\": \"Malloc0\",\n            \"num_blocks\": 65536,\n            \"block_size\": 512\n          }\n        }\n      ]\n    }\n  ]\n}");
    }

    // Initialize SPDK application options.
    // Using "0x1" for reactor_mask means SPDK will try to use core 0.
    // RPC is disabled for this simple test.
    xsan_error_t err = xsan_spdk_manager_opts_init("xsan_node_main", spdk_json_conf_file, "0x1", false, NULL);
    if (err != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize SPDK manager options: %s (code %d)", xsan_error_string(err), err);
        xsan_log_shutdown();
        return EXIT_FAILURE;
    }

    // Start the SPDK application. This function blocks until SPDK is stopped.
    // xsan_node_main_spdk_thread_start will be called on an SPDK thread.
    err = xsan_spdk_manager_start_app(xsan_node_main_spdk_thread_start, NULL /* argument for start_fn */);
    if (err != XSAN_OK) {
        XSAN_LOG_FATAL("xsan_spdk_manager_start_app returned with error: %s (code %d)", xsan_error_string(err), err);
        // spdk_app_fini() is still needed for cleanup even if spdk_app_start fails after some point.
    }

    // Clean up SPDK resources. This is called after spdk_app_start has returned.
    xsan_spdk_manager_app_fini();

    XSAN_LOG_INFO("XSAN Node has shut down.");
    xsan_log_shutdown();

    return (err == XSAN_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
