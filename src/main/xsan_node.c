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

    // Initialize our bdev subsystem wrapper
    if (xsan_bdev_subsystem_init() != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN bdev subsystem.");
        xsan_spdk_manager_request_app_stop();
        return;
    }

    XSAN_LOG_INFO("Listing available SPDK bdevs...");
    xsan_bdev_info_t *bdev_list = NULL;
    int bdev_count = 0;
    xsan_error_t err = xsan_bdev_list_get_all(&bdev_list, &bdev_count);

    if (err == XSAN_OK) {
        XSAN_LOG_INFO("Found %d SPDK bdev(s):", bdev_count);
        for (int i = 0; i < bdev_count; ++i) {
            char uuid_formatted_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(uuid_formatted_str, sizeof(uuid_formatted_str),
                                (struct spdk_uuid*)&bdev_list[i].uuid.data[0]);

            XSAN_LOG_INFO("  [%d] Name: '%s', Product: '%s', Size: %.2f GiB, UUID: %s, BlockSize: %u bytes",
                          i,
                          bdev_list[i].name,
                          bdev_list[i].product_name,
                          (double)bdev_list[i].capacity_bytes / (1024.0 * 1024.0 * 1024.0),
                          uuid_formatted_str,
                          bdev_list[i].block_size);

            // Simple Read/Write Test on the first Malloc bdev found
            if (strstr(bdev_list[i].name, "Malloc") != NULL && bdev_list[i].num_blocks > 0 && bdev_list[i].block_size > 0) {
                XSAN_LOG_INFO("--- Performing R/W Test on '%s' ---", bdev_list[i].name);
                uint32_t blk_size_test = bdev_list[i].block_size;
                size_t bdev_align = xsan_bdev_get_buf_align(bdev_list[i].name);

                void *write_buf = xsan_bdev_dma_malloc(blk_size_test, bdev_align);
                void *read_buf = xsan_bdev_dma_malloc(blk_size_test, bdev_align);

                if (write_buf && read_buf) {
                    snprintf((char*)write_buf, blk_size_test, "XSAN R/W Test on %s! Block 0.", bdev_list[i].name);
                    // Ensure the rest of the buffer is distinct for a robust test, e.g. fill with a pattern.
                    for(size_t k=strlen((char*)write_buf); k<blk_size_test; ++k) ((char*)write_buf)[k] = (char)(k % 256);

                    memset(read_buf, 0xAA, blk_size_test); // Fill read_buf to ensure it's overwritten

                    XSAN_LOG_DEBUG("Writing 1 block (offset 0) to '%s'...", bdev_list[i].name);
                    // Using use_internal_dma_alloc = false, as we've already allocated DMA-safe buffers
                    err = xsan_bdev_write_sync(bdev_list[i].name, 0, 1, write_buf, blk_size_test, false);
                    if (err == XSAN_OK) {
                        XSAN_LOG_INFO("Write to '%s' successful.", bdev_list[i].name);
                        XSAN_LOG_DEBUG("Reading 1 block (offset 0) from '%s'...", bdev_list[i].name);
                        err = xsan_bdev_read_sync(bdev_list[i].name, 0, 1, read_buf, blk_size_test, false);
                        if (err == XSAN_OK) {
                            XSAN_LOG_INFO("Read from '%s' successful.", bdev_list[i].name);
                            if (memcmp(write_buf, read_buf, blk_size_test) == 0) {
                                XSAN_LOG_INFO("SUCCESS: R/W data verification for '%s' passed!", bdev_list[i].name);
                            } else {
                                XSAN_LOG_ERROR("FAILURE: R/W data verification for '%s' FAILED!", bdev_list[i].name);
                                // Optionally log differing bytes for debug
                            }
                        } else {
                            XSAN_LOG_ERROR("Read from '%s' failed: %s (code %d)", bdev_list[i].name, xsan_error_string(err), err);
                        }
                    } else {
                        XSAN_LOG_ERROR("Write to '%s' failed: %s (code %d)", bdev_list[i].name, xsan_error_string(err), err);
                    }
                } else {
                    XSAN_LOG_ERROR("Failed to allocate DMA buffers for R/W test on '%s'.", bdev_list[i].name);
                }
                if(write_buf) xsan_bdev_dma_free(write_buf);
                if(read_buf) xsan_bdev_dma_free(read_buf);
                XSAN_LOG_INFO("--- R/W Test on '%s' finished ---", bdev_list[i].name);
                break; // Test only one Malloc bdev for now
            }
        }
        xsan_bdev_list_free(bdev_list, bdev_count);
    } else {
        XSAN_LOG_ERROR("Failed to list SPDK bdevs: %s (code %d)", xsan_error_string(err), err);
    }

    xsan_bdev_subsystem_fini();

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
