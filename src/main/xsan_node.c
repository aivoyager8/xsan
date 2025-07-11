#include "xsan_log.h"
#include "xsan_config.h"        // May not be used directly here, but good to have
#include "xsan_spdk_manager.h"
#include "xsan_bdev.h"
#include "xsan_disk_manager.h"  // Added
#include "xsan_volume_manager.h"// Added
#include "xsan_error.h"         // For error strings
#include "xsan_string_utils.h"  // For xsan_strcpy_safe if needed

#include <stdio.h>
#include <stdlib.h>             // For EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>             // For memset, strstr, memcmp
#include <unistd.h>             // For sleep (debugging)

// SPDK Headers needed for this test/main file specifically
#include "spdk/uuid.h"          // For spdk_uuid_fmt_lower, SPDK_UUID_STRING_LEN, spdk_uuid_is_null
#include "spdk/env.h"           // For spdk_dma_malloc/free (though wrapped by xsan_bdev)

/**
 * @brief Main application function executed on an SPDK reactor thread.
 * This function is passed to xsan_spdk_manager_start_app.
 *
 * @param arg1 Custom argument passed from xsan_spdk_manager_start_app (currently NULL).
 * @param rc Return code from the SPDK framework's internal start process. 0 on success.
 */
static void xsan_node_main_spdk_thread_start(void *arg1, int rc) {
    (void)arg1; // Unused argument
    xsan_error_t err = XSAN_OK;
    xsan_disk_manager_t *dm = NULL;
    xsan_volume_manager_t *vm = NULL;

    XSAN_LOG_INFO("XSAN SPDK application thread started (spdk_app_start internal rc: %d).", rc);

    if (rc != 0) {
        XSAN_LOG_FATAL("SPDK framework initialization failed prior to calling app main. Cannot proceed.");
        goto app_stop_no_cleanup;
    }

    // Initialize XSAN bdev subsystem (SPDK bdev interaction layer)
    if (xsan_bdev_subsystem_init() != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN bdev subsystem.");
        goto app_stop_no_cleanup;
    }

    // Initialize XSAN Disk Manager
    err = xsan_disk_manager_init(&dm);
    if (err != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN Disk Manager: %s (code %d)", xsan_error_string(err), err);
        goto app_stop_bdev_fini_only;
    }

    // Initialize XSAN Volume Manager
    err = xsan_volume_manager_init(dm, &vm);
    if (err != XSAN_OK) {
        XSAN_LOG_FATAL("Failed to initialize XSAN Volume Manager: %s (code %d)", xsan_error_string(err), err);
        goto app_stop_dm_fini;
    }

    // Scan and register SPDK bdevs into the Disk Manager
    err = xsan_disk_manager_scan_and_register_bdevs(dm);
    if (err != XSAN_OK && err != XSAN_ERROR_NOT_FOUND /* allow no bdevs found if it's a valid outcome */) {
        XSAN_LOG_ERROR("Error during bdev scan and registration: %s (code %d)", xsan_error_string(err), err);
    }

    // List all XSAN disks
    xsan_disk_t **xsan_disks_list_ptr = NULL;
    int xsan_disk_count = 0;
    err = xsan_disk_manager_get_all_disks(dm, &xsan_disks_list_ptr, &xsan_disk_count);
    if (err == XSAN_OK) {
        XSAN_LOG_INFO("XSAN Disk Manager discovered %d disk(s):", xsan_disk_count);
        for (int i = 0; i < xsan_disk_count; ++i) {
            char xsan_disk_id_str[SPDK_UUID_STRING_LEN];
            char bdev_uuid_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(xsan_disk_id_str, sizeof(xsan_disk_id_str), (struct spdk_uuid*)&xsan_disks_list_ptr[i]->id.data[0]);
            spdk_uuid_fmt_lower(bdev_uuid_str, sizeof(bdev_uuid_str), (struct spdk_uuid*)&xsan_disks_list_ptr[i]->bdev_uuid.data[0]);
            XSAN_LOG_INFO("  Disk [%d]: XSAN_ID=%s, BDevName='%s', Size=%.2f GiB, Product='%s'",
                          i, xsan_disk_id_str, xsan_disks_list_ptr[i]->bdev_name,
                          (double)xsan_disks_list_ptr[i]->capacity_bytes / (1024.0*1024.0*1024.0),
                          xsan_disks_list_ptr[i]->product_name);
        }
    } else {
        XSAN_LOG_ERROR("Failed to get all XSAN disks: %s (code %d)", xsan_error_string(err), err);
    }

    // --- Disk Group Test ---
    xsan_group_id_t test_group_id;
    memset(&test_group_id, 0, sizeof(xsan_group_id_t));
    bool group_created_flag = false;

    if (xsan_disk_count > 0 && xsan_disks_list_ptr) { // Ensure list_ptr is valid
        const char *bdev_names_for_group[1] = { xsan_disks_list_ptr[0]->bdev_name };
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

    // --- Volume Management Test ---
    xsan_volume_id_t test_volume_id;
    memset(&test_volume_id, 0, sizeof(xsan_volume_id_t));
    bool volume_created_flag = false;

    if (group_created_flag) {
        uint64_t vol_size_mb = 128;
        uint64_t vol_size_bytes_test = vol_size_mb * 1024 * 1024;
        uint32_t vol_block_size_test = 4096;
        XSAN_LOG_INFO("Attempting to create volume 'Vol1' (%.0f MiB, blocksize %uB) on group 'DG1'...",
                      (double)vol_size_mb, vol_block_size_test);
        err = xsan_volume_create(vm, "Vol1", vol_size_bytes_test, test_group_id, vol_block_size_test, false, &test_volume_id);
        if (err == XSAN_OK) {
            volume_created_flag = true;
            char vol_id_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&test_volume_id.data[0]);
            XSAN_LOG_INFO("Volume 'Vol1' created successfully, ID: %s", vol_id_str);

            xsan_volume_t *fetched_vol_by_id = xsan_volume_get_by_id(vm, test_volume_id);
            if (fetched_vol_by_id) {
                 XSAN_LOG_INFO("Successfully fetched volume 'Vol1' by ID.");
            } else {
                 XSAN_LOG_ERROR("Failed to fetch volume 'Vol1' by ID.");
            }
            xsan_volume_t *fetched_vol_by_name = xsan_volume_get_by_name(vm, "Vol1");
            if (fetched_vol_by_name) {
                 XSAN_LOG_INFO("Successfully fetched volume 'Vol1' by name.");
            } else {
                 XSAN_LOG_ERROR("Failed to fetch volume 'Vol1' by name.");
            }


            // Test LBA mapping
            xsan_disk_id_t mapped_disk_id;
            uint64_t mapped_physical_lba;
            uint64_t test_logical_lba_idx = 0;
            err = xsan_volume_map_lba_to_physical(vm, test_volume_id, test_logical_lba_idx, &mapped_disk_id, &mapped_physical_lba);
            if(err == XSAN_OK) {
                char mapped_disk_id_str[SPDK_UUID_STRING_LEN];
                spdk_uuid_fmt_lower(mapped_disk_id_str, sizeof(mapped_disk_id_str), (struct spdk_uuid*)&mapped_disk_id.data[0]);
                XSAN_LOG_INFO("Volume 'Vol1' LBA_idx %lu maps to XSAN Disk ID: %s, Physical LBA_idx: %lu",
                              test_logical_lba_idx, mapped_disk_id_str, mapped_physical_lba);
            } else {
                 XSAN_LOG_ERROR("Failed to map LBA %lu for 'Vol1': %s (code %d)", test_logical_lba_idx, xsan_error_string(err), err);
            }
            // Test another LBA
            test_logical_lba_idx = (vol_size_bytes_test / vol_block_size_test) - 1; // Last block
             err = xsan_volume_map_lba_to_physical(vm, test_volume_id, test_logical_lba_idx, &mapped_disk_id, &mapped_physical_lba);
            if(err == XSAN_OK) {
                char mapped_disk_id_str[SPDK_UUID_STRING_LEN];
                spdk_uuid_fmt_lower(mapped_disk_id_str, sizeof(mapped_disk_id_str), (struct spdk_uuid*)&mapped_disk_id.data[0]);
                XSAN_LOG_INFO("Volume 'Vol1' LBA_idx %lu maps to XSAN Disk ID: %s, Physical LBA_idx: %lu",
                              test_logical_lba_idx, mapped_disk_id_str, mapped_physical_lba);
            } else {
                 XSAN_LOG_ERROR("Failed to map LBA %lu for 'Vol1': %s (code %d)", test_logical_lba_idx, xsan_error_string(err), err);
            }

        } else {
            XSAN_LOG_ERROR("Failed to create volume 'Vol1': %s (code %d)", xsan_error_string(err), err);
        }
    } else {
        XSAN_LOG_WARN("Skipping volume creation test: disk group 'DG1' was not created.");
    }

    // List all volumes
    xsan_volume_t **volumes_list_ptr = NULL;
    int volume_count = 0;
    if (xsan_volume_list_all(vm, &volumes_list_ptr, &volume_count) == XSAN_OK) {
        XSAN_LOG_INFO("Found %d volume(s) via Volume Manager:", volume_count);
        for(int i=0; i<volume_count; ++i) {
            char v_id_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(v_id_str, sizeof(v_id_str), (struct spdk_uuid*)&volumes_list_ptr[i]->id.data[0]);
            XSAN_LOG_INFO("  Vol [%d]: Name='%s', ID=%s, Size=%.2f GiB, BlockSize=%uB", i,
                          volumes_list_ptr[i]->name, v_id_str,
                          (double)volumes_list_ptr[i]->size_bytes / (1024.0*1024.0*1024.0),
                          volumes_list_ptr[i]->block_size_bytes);
        }
        xsan_volume_manager_free_volume_pointer_list(volumes_list_ptr);
    }

    // --- BDEV R/W Test (Maintained) ---
    if (xsan_disk_count > 0 && xsan_disks_list_ptr) {
        xsan_disk_t* rw_test_disk = NULL;
        for(int i=0; i<xsan_disk_count; ++i) {
             if(xsan_disks_list_ptr[i] && strstr(xsan_disks_list_ptr[i]->bdev_name, "Malloc") != NULL) {
                 rw_test_disk = xsan_disks_list_ptr[i];
                 break;
             }
        }
        if (!rw_test_disk && xsan_disk_count > 0 && xsan_disks_list_ptr) rw_test_disk = xsan_disks_list_ptr[0];

        if (rw_test_disk && rw_test_disk->num_blocks > 0 && rw_test_disk->block_size_bytes > 0) {
            XSAN_LOG_INFO("--- Performing R/W Test on XSAN disk (bdev: '%s') ---", rw_test_disk->bdev_name);
            uint32_t blk_size_rw_test = rw_test_disk->block_size_bytes;
            size_t bdev_align_rw_test = xsan_bdev_get_buf_align(rw_test_disk->bdev_name);
            void *write_buf_rw = xsan_bdev_dma_malloc(blk_size_rw_test, bdev_align_rw_test);
            void *read_buf_rw = xsan_bdev_dma_malloc(blk_size_rw_test, bdev_align_rw_test);
            if (write_buf_rw && read_buf_rw) {
                snprintf((char*)write_buf_rw, blk_size_rw_test, "XSAN R/W Test on %s!", rw_test_disk->bdev_name);
                for(size_t k=strlen((char*)write_buf_rw); k<blk_size_rw_test; ++k) ((char*)write_buf_rw)[k] = (char)((k % 255) + 1);
                memset(read_buf_rw, 0xAA, blk_size_rw_test);

                err = xsan_bdev_write_sync(rw_test_disk->bdev_name, 0, 1, write_buf_rw, blk_size_rw_test, false);
                if (err == XSAN_OK) {
                    XSAN_LOG_DEBUG("Write to '%s' block 0 successful.", rw_test_disk->bdev_name);
                    err = xsan_bdev_read_sync(rw_test_disk->bdev_name, 0, 1, read_buf_rw, blk_size_rw_test, false);
                    if (err == XSAN_OK) {
                        XSAN_LOG_DEBUG("Read from '%s' block 0 successful.", rw_test_disk->bdev_name);
                        if (memcmp(write_buf_rw, read_buf_rw, blk_size_rw_test) == 0) {
                            XSAN_LOG_INFO("SUCCESS: R/W data verification for '%s' passed!", rw_test_disk->bdev_name);
                        } else { XSAN_LOG_ERROR("FAILURE: R/W data verification for '%s' FAILED!", rw_test_disk->bdev_name); }
                    } else { XSAN_LOG_ERROR("Read from '%s' failed: %s (code %d)", rw_test_disk->bdev_name, xsan_error_string(err), err); }
                } else { XSAN_LOG_ERROR("Write to '%s' failed: %s (code %d)", rw_test_disk->bdev_name, xsan_error_string(err), err); }
            } else { XSAN_LOG_ERROR("Failed to allocate DMA buffers for R/W test on '%s'.", rw_test_disk->bdev_name); }
            if(write_buf_rw) xsan_bdev_dma_free(write_buf_rw);
            if(read_buf_rw) xsan_bdev_dma_free(read_buf_rw);
            XSAN_LOG_INFO("--- R/W Test on '%s' finished ---", rw_test_disk->bdev_name);
        }
    }
    if (xsan_disks_list_ptr) {
        xsan_disk_manager_free_disk_pointer_list(xsan_disks_list_ptr);
        xsan_disks_list_ptr = NULL;
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
    // Finalize Volume Manager if it was initialized
    if (vm) xsan_volume_manager_fini(&vm);

    XSAN_LOG_INFO("XSAN SPDK application thread work complete or aborted. Requesting application stop.");
    xsan_spdk_manager_request_app_stop(); // Signal SPDK framework to shut down
}
