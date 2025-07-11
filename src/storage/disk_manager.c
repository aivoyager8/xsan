#include "xsan_disk_manager.h"
#include "xsan_bdev.h"       // For xsan_bdev_list_get_all, xsan_bdev_info_t
#include "xsan_memory.h"   // For XSAN_MALLOC, XSAN_FREE, XSAN_CALLOC
#include "xsan_list.h"     // Using our own list for managing disks and groups
#include "xsan_string_utils.h" // For xsan_strcpy_safe
#include "xsan_log.h"      // For XSAN_LOG_INFO, XSAN_LOG_ERROR, etc.

#include "spdk/uuid.h"     // For spdk_uuid_generate, spdk_uuid_compare
#include <pthread.h>       // For pthread_mutex_t

// The internal structure for the disk manager
struct xsan_disk_manager {
    xsan_list_t *managed_disks;       // List of xsan_disk_t structures
    xsan_list_t *managed_disk_groups; // List of xsan_disk_group_t structures
    pthread_mutex_t lock;             // Mutex to protect access to the lists
    bool initialized;                 // Flag to indicate if the manager is initialized
};

// Global disk manager instance (singleton pattern)
// This simplifies access but means init/fini must be managed carefully.
static xsan_disk_manager_t *g_xsan_disk_manager_instance = NULL;

// Destructor callback for xsan_disk_t elements in xsan_list
static void _xsan_internal_disk_destroy_cb(void *disk_data) {
    if (disk_data) {
        xsan_disk_t *disk = (xsan_disk_t *)disk_data;
        XSAN_LOG_DEBUG("Disk Manager: Destroying xsan_disk (bdev: %s)", disk->bdev_name);
        // Any specific cleanup for xsan_disk_t itself, if not just memory.
        XSAN_FREE(disk);
    }
}

// Destructor callback for xsan_disk_group_t elements in xsan_list
static void _xsan_internal_disk_group_destroy_cb(void *group_data) {
    if (group_data) {
        xsan_disk_group_t *group = (xsan_disk_group_t *)group_data;
        XSAN_LOG_DEBUG("Disk Manager: Destroying xsan_disk_group (name: %s)", group->name);
        // Disks referenced by disk_ids are managed in the main 'managed_disks' list.
        // No need to free them here, just the group structure itself.
        XSAN_FREE(group);
    }
}

xsan_error_t xsan_disk_manager_init(xsan_disk_manager_t **dm_instance_out) {
    if (g_xsan_disk_manager_instance != NULL) {
        XSAN_LOG_WARN("XSAN Disk Manager already initialized.");
        if (dm_instance_out) {
            *dm_instance_out = g_xsan_disk_manager_instance;
        }
        // Consider if returning OK or an "already initialized" error is better.
        // For now, let's treat it as success to get the instance.
        return XSAN_OK;
    }

    XSAN_LOG_INFO("Initializing XSAN Disk Manager...");

    xsan_disk_manager_t *dm = (xsan_disk_manager_t *)XSAN_MALLOC(sizeof(xsan_disk_manager_t));
    if (!dm) {
        XSAN_LOG_ERROR("Failed to allocate memory for XSAN Disk Manager structure.");
        if (dm_instance_out) *dm_instance_out = NULL;
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    dm->managed_disks = xsan_list_create(_xsan_internal_disk_destroy_cb);
    if (!dm->managed_disks) {
        XSAN_LOG_ERROR("Failed to create list for managed disks.");
        XSAN_FREE(dm);
        if (dm_instance_out) *dm_instance_out = NULL;
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    dm->managed_disk_groups = xsan_list_create(_xsan_internal_disk_group_destroy_cb);
    if (!dm->managed_disk_groups) {
        XSAN_LOG_ERROR("Failed to create list for managed disk groups.");
        xsan_list_destroy(dm->managed_disks);
        XSAN_FREE(dm);
        if (dm_instance_out) *dm_instance_out = NULL;
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    if (pthread_mutex_init(&dm->lock, NULL) != 0) {
        XSAN_LOG_ERROR("Failed to initialize mutex for Disk Manager.");
        xsan_list_destroy(dm->managed_disk_groups);
        xsan_list_destroy(dm->managed_disks);
        XSAN_FREE(dm);
        if (dm_instance_out) *dm_instance_out = NULL;
        return XSAN_ERROR_SYSTEM; // Or a more specific mutex error
    }

    dm->initialized = true;
    g_xsan_disk_manager_instance = dm;
    if (dm_instance_out) {
        *dm_instance_out = g_xsan_disk_manager_instance;
    }

    XSAN_LOG_INFO("XSAN Disk Manager initialized successfully. Call xsan_disk_manager_scan_and_register_bdevs() to populate disks.");
    return XSAN_OK;
}

void xsan_disk_manager_fini(xsan_disk_manager_t **dm_ptr) {
    xsan_disk_manager_t *dm_to_fini = NULL;

    if (dm_ptr && *dm_ptr) { // User passed a pointer to their variable
        dm_to_fini = *dm_ptr;
    } else if (g_xsan_disk_manager_instance) { // User passed NULL, try global
        dm_to_fini = g_xsan_disk_manager_instance;
    }

    if (!dm_to_fini || !dm_to_fini->initialized) {
        XSAN_LOG_INFO("XSAN Disk Manager already finalized or was not initialized.");
        if (dm_ptr) *dm_ptr = NULL; // Ensure caller's pointer is NULL if they passed one
        g_xsan_disk_manager_instance = NULL; // Ensure global is NULL
        return;
    }

    XSAN_LOG_INFO("Finalizing XSAN Disk Manager...");
    pthread_mutex_lock(&dm_to_fini->lock);

    // xsan_list_destroy calls the destructor for each element, which frees xsan_disk_t and xsan_disk_group_t
    xsan_list_destroy(dm_to_fini->managed_disk_groups);
    dm_to_fini->managed_disk_groups = NULL;

    xsan_list_destroy(dm_to_fini->managed_disks);
    dm_to_fini->managed_disks = NULL;

    dm_to_fini->initialized = false;
    pthread_mutex_unlock(&dm_to_fini->lock);
    pthread_mutex_destroy(&dm_to_fini->lock);

    XSAN_FREE(dm_to_fini);
    if (dm_ptr && *dm_ptr) {
        *dm_ptr = NULL;
    }
    if (dm_to_fini == g_xsan_disk_manager_instance) {
        g_xsan_disk_manager_instance = NULL;
    }
    XSAN_LOG_INFO("XSAN Disk Manager finalized.");
}

// Implementation for xsan_disk_manager_scan_and_register_bdevs
xsan_error_t xsan_disk_manager_scan_and_register_bdevs(xsan_disk_manager_t *dm) {
    if (!dm || !dm->initialized) {
        XSAN_LOG_ERROR("Disk manager not initialized or NULL when trying to scan bdevs.");
        return XSAN_ERROR_INVALID_PARAM; // Or a more specific "not initialized" error
    }

    // This function MUST be called from an SPDK thread.
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_disk_manager_scan_and_register_bdevs must be called from an SPDK thread.");
        return XSAN_ERROR_THREAD_CONTEXT;
    }

    XSAN_LOG_INFO("Scanning for SPDK bdevs to register/update in XSAN Disk Manager...");

    xsan_bdev_info_t *bdev_info_list = NULL; // This will be an array of xsan_bdev_info_t structs
    int bdev_count = 0;
    xsan_error_t err = xsan_bdev_list_get_all(&bdev_info_list, &bdev_count);

    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("Failed to get list of SPDK bdevs from xsan_bdev module: %s (code %d)", xsan_error_string(err), err);
        return err;
    }

    if (bdev_count == 0) {
        XSAN_LOG_INFO("No SPDK bdevs found by xsan_bdev module.");
        // No need to free bdev_info_list as it would be NULL.
        return XSAN_OK;
    }

    XSAN_LOG_INFO("Found %d SPDK bdev(s). Processing for registration/update...", bdev_count);
    int new_registered_count = 0;
    int updated_count = 0;
    int skipped_count = 0; // For bdevs that are perhaps not suitable

    pthread_mutex_lock(&dm->lock);

    for (int i = 0; i < bdev_count; ++i) {
        xsan_bdev_info_t *current_bdev_info = &bdev_info_list[i]; // Direct struct from the array

        // Try to find if this bdev (by name) is already managed as an xsan_disk_t
        xsan_disk_t *existing_disk = NULL;
        xsan_list_node_t *list_node;
        XSAN_LIST_FOREACH(dm->managed_disks, list_node) {
            xsan_disk_t *disk_iter = (xsan_disk_t *)xsan_list_node_get_value(list_node);
            if (strncmp(disk_iter->bdev_name, current_bdev_info->name, XSAN_MAX_NAME_LEN) == 0) {
                existing_disk = disk_iter;
                break;
            }
        }

        if (existing_disk) {
            // Bdev already exists as an xsan_disk. Update its properties if necessary.
            // For now, we'll just log and update basic info. More sophisticated update logic
            // (e.g., handling state changes if SPDK could report them here) can be added.
            XSAN_LOG_DEBUG("Updating existing XSAN disk for bdev '%s'.", current_bdev_info->name);
            // Update fields that might change or need refreshing
            memcpy(&existing_disk->bdev_uuid, &current_bdev_info->uuid, sizeof(xsan_uuid_t));
            existing_disk->capacity_bytes = current_bdev_info->capacity_bytes;
            existing_disk->block_size_bytes = current_bdev_info->block_size;
            existing_disk->num_blocks = current_bdev_info->num_blocks;
            xsan_strcpy_safe(existing_disk->product_name, current_bdev_info->product_name, XSAN_MAX_NAME_LEN);
            existing_disk->is_rotational = current_bdev_info->is_rotational;
            existing_disk->optimal_io_boundary_blocks = current_bdev_info->optimal_io_boundary;
            existing_disk->has_write_cache = current_bdev_info->has_write_cache;
            // Re-infer type (simplified)
            if (strstr(current_bdev_info->name, "Nvme") != NULL || strstr(current_bdev_info->product_name, "NVMe") !=NULL) {
                existing_disk->type = XSAN_STORAGE_DISK_TYPE_NVME_SSD;
            } else if (current_bdev_info->is_rotational) {
                existing_disk->type = XSAN_STORAGE_DISK_TYPE_HDD_SATA;
            } else {
                existing_disk->type = XSAN_STORAGE_DISK_TYPE_SATA_SSD;
            }
            // Assume state remains ONLINE unless SPDK provides other info (not directly from bdev_info here)
            // existing_disk->state = XSAN_STORAGE_STATE_ONLINE;
            updated_count++;
        } else {
            // Bdev not found, register as a new xsan_disk_t
            xsan_disk_t *new_disk = (xsan_disk_t *)XSAN_MALLOC(sizeof(xsan_disk_t));
            if (!new_disk) {
                XSAN_LOG_ERROR("Failed to allocate memory for new xsan_disk_t for bdev '%s'.", current_bdev_info->name);
                err = XSAN_ERROR_OUT_OF_MEMORY; // Mark error, but try to continue for other bdevs
                continue;
            }
            memset(new_disk, 0, sizeof(xsan_disk_t));

            // Generate a new XSAN unique ID for this disk
            spdk_uuid_generate((struct spdk_uuid *)&new_disk->id.data[0]);

            xsan_strcpy_safe(new_disk->bdev_name, current_bdev_info->name, XSAN_MAX_NAME_LEN);
            memcpy(&new_disk->bdev_uuid, &current_bdev_info->uuid, sizeof(xsan_uuid_t));

            new_disk->capacity_bytes = current_bdev_info->capacity_bytes;
            new_disk->block_size_bytes = current_bdev_info->block_size;
            new_disk->num_blocks = current_bdev_info->num_blocks;
            xsan_strcpy_safe(new_disk->product_name, current_bdev_info->product_name, XSAN_MAX_NAME_LEN);

            new_disk->is_rotational = current_bdev_info->is_rotational;
            new_disk->optimal_io_boundary_blocks = current_bdev_info->optimal_io_boundary;
            new_disk->has_write_cache = current_bdev_info->has_write_cache;

            if (strstr(current_bdev_info->name, "Nvme") != NULL || strstr(current_bdev_info->product_name, "NVMe") !=NULL) {
                new_disk->type = XSAN_STORAGE_DISK_TYPE_NVME_SSD;
            } else if (current_bdev_info->is_rotational) {
                new_disk->type = XSAN_STORAGE_DISK_TYPE_HDD_SATA;
            } else {
                new_disk->type = XSAN_STORAGE_DISK_TYPE_SATA_SSD;
            }
            new_disk->state = XSAN_STORAGE_STATE_ONLINE; // Initial state

            if (xsan_list_append(dm->managed_disks, new_disk) == NULL) {
                XSAN_LOG_ERROR("Failed to append new disk (bdev '%s') to managed list.", new_disk->bdev_name);
                _xsan_internal_disk_destroy_cb(new_disk); // Free the allocated disk if append fails
                err = XSAN_ERROR_OUT_OF_MEMORY;
            } else {
                char xsan_id_str[SPDK_UUID_STRING_LEN];
                spdk_uuid_fmt_lower(xsan_id_str, sizeof(xsan_id_str), (struct spdk_uuid*)&new_disk->id.data[0]);
                XSAN_LOG_INFO("Registered new XSAN disk: Name='%s', XSAN_ID=%s, Type=%d, Size=%.2f GiB",
                              new_disk->bdev_name, xsan_id_str, new_disk->type,
                              (double)new_disk->capacity_bytes / (1024.0*1024.0*1024.0));
                new_registered_count++;
            }
        }
    }

    pthread_mutex_unlock(&dm->lock);

    // Free the bdev_info_list obtained from xsan_bdev_list_get_all()
    // The list itself is a single allocation containing all xsan_bdev_info_t structs.
    xsan_bdev_list_free(bdev_info_list, bdev_count);

    XSAN_LOG_INFO("SPDK bdev scan and registration complete. New: %d, Updated: %d, Skipped/No-change: %d.",
                  new_registered_count, updated_count, bdev_count - new_registered_count - updated_count);

    return err; // Returns XSAN_OK if all operations were successful, or the last error encountered.
}

xsan_error_t xsan_disk_manager_get_all_disks(xsan_disk_manager_t *dm, xsan_disk_t ***disks_array_out, int *count_out) {
    if (!dm || !dm->initialized || !disks_array_out || !count_out) {
        if (count_out) *count_out = 0;
        if (disks_array_out) *disks_array_out = NULL;
        return XSAN_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&dm->lock);
    size_t num_disks = xsan_list_size(dm->managed_disks);
    if (num_disks == 0) {
        *disks_array_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&dm->lock);
        return XSAN_OK;
    }

    *disks_array_out = (xsan_disk_t **)XSAN_MALLOC(sizeof(xsan_disk_t*) * num_disks);
    if (!*disks_array_out) {
        *count_out = 0;
        pthread_mutex_unlock(&dm->lock);
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    int i = 0;
    xsan_list_node_t *node;
    XSAN_LIST_FOREACH(dm->managed_disks, node) {
        if (i < (int)num_disks) { // Defensive check
            (*disks_array_out)[i++] = (xsan_disk_t *)xsan_list_node_get_value(node);
        } else {
            break; // Should not happen
        }
    }
    *count_out = i; // Actual count copied
    pthread_mutex_unlock(&dm->lock);
    return XSAN_OK;
}

void xsan_disk_manager_free_disk_pointer_list(xsan_disk_t **disk_ptr_array) {
    if (disk_ptr_array) {
        XSAN_FREE(disk_ptr_array);
    }
}

xsan_disk_t *xsan_disk_manager_find_disk_by_id(xsan_disk_manager_t *dm, xsan_disk_id_t disk_id_to_find) {
    if (!dm || !dm->initialized) return NULL;

    pthread_mutex_lock(&dm->lock);
    xsan_list_node_t *node;
    xsan_disk_t *found_disk = NULL;
    XSAN_LIST_FOREACH(dm->managed_disks, node) {
        xsan_disk_t *disk = (xsan_disk_t *)xsan_list_node_get_value(node);
        if (spdk_uuid_compare((struct spdk_uuid*)&disk->id.data[0], (struct spdk_uuid*)&disk_id_to_find.data[0]) == 0) {
            found_disk = disk;
            break;
        }
    }
    pthread_mutex_unlock(&dm->lock);
    return found_disk;
}

xsan_disk_t *xsan_disk_manager_find_disk_by_bdev_name(xsan_disk_manager_t *dm, const char *bdev_name) {
    if (!dm || !dm->initialized || !bdev_name) return NULL;

    pthread_mutex_lock(&dm->lock);
    xsan_list_node_t *node;
    xsan_disk_t *found_disk = NULL;
    XSAN_LIST_FOREACH(dm->managed_disks, node) {
        xsan_disk_t *disk = (xsan_disk_t *)xsan_list_node_get_value(node);
        if (strncmp(disk->bdev_name, bdev_name, XSAN_MAX_NAME_LEN) == 0) {
            found_disk = disk;
            break;
        }
    }
    pthread_mutex_unlock(&dm->lock);
    return found_disk;
}

xsan_error_t xsan_disk_manager_disk_group_create(xsan_disk_manager_t *dm,
                                                 const char *group_name,
                                                 xsan_disk_group_type_t group_type,
                                                 const char *bdev_names_list[],
                                                 int num_bdevs,
                                                 xsan_group_id_t *group_id_out) {
    if (!dm || !dm->initialized || !group_name || !bdev_names_list || num_bdevs <= 0 || num_bdevs > XSAN_MAX_DISKS_PER_GROUP) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_disk_manager_disk_group_create must be called from an SPDK thread (for potential future SPDK interactions).");
        // While this specific impl might not call SPDK, related functions often do.
        // return XSAN_ERROR_THREAD_CONTEXT; // Relaxing this for now as it's primarily list manipulation
    }

    pthread_mutex_lock(&dm->lock);
    xsan_error_t err = XSAN_OK;

    // Check if group name already exists
    if (xsan_disk_manager_find_disk_group_by_name(dm, group_name) != NULL) {
        XSAN_LOG_WARN("Disk group with name '%s' already exists.", group_name);
        pthread_mutex_unlock(&dm->lock);
        return XSAN_ERROR_ALREADY_EXISTS;
    }

    xsan_disk_t *member_disks[XSAN_MAX_DISKS_PER_GROUP];
    uint64_t total_capacity = 0;

    for (int i = 0; i < num_bdevs; ++i) {
        xsan_disk_t *disk = xsan_disk_manager_find_disk_by_bdev_name(dm, bdev_names_list[i]);
        if (!disk) {
            XSAN_LOG_ERROR("Disk with bdev name '%s' not found for group '%s'.", bdev_names_list[i], group_name);
            err = XSAN_ERROR_NOT_FOUND;
            goto cleanup_unlock;
        }
        if (!spdk_uuid_is_null((struct spdk_uuid*)&disk->assigned_to_group_id.data[0])) {
            char group_uuid_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(group_uuid_str, sizeof(group_uuid_str), (struct spdk_uuid*)&disk->assigned_to_group_id.data[0]);
            XSAN_LOG_ERROR("Disk '%s' (bdev: %s) is already assigned to group %s.",
                           spdk_uuid_get_string((struct spdk_uuid*)&disk->id.data[0]), disk->bdev_name, group_uuid_str);
            err = XSAN_ERROR_RESOURCE_BUSY; // Disk already in use
            goto cleanup_unlock;
        }
        member_disks[i] = disk;
        total_capacity += disk->capacity_bytes;
    }

    xsan_disk_group_t *new_group = (xsan_disk_group_t *)XSAN_MALLOC(sizeof(xsan_disk_group_t));
    if (!new_group) {
        err = XSAN_ERROR_OUT_OF_MEMORY;
        goto cleanup_unlock;
    }
    memset(new_group, 0, sizeof(xsan_disk_group_t));

    spdk_uuid_generate((struct spdk_uuid *)&new_group->id.data[0]);
    xsan_strcpy_safe(new_group->name, group_name, XSAN_MAX_NAME_LEN);
    new_group->type = group_type;
    new_group->state = XSAN_STORAGE_STATE_ONLINE; // Initial state
    new_group->disk_count = num_bdevs;
    new_group->total_capacity_bytes = total_capacity;
    // For PASSSTHROUGH/JBOD, usable might be same as total initially.
    // RAID would reduce this.
    new_group->usable_capacity_bytes = total_capacity;

    for (int i = 0; i < num_bdevs; ++i) {
        memcpy(&new_group->disk_ids[i], &member_disks[i]->id, sizeof(xsan_disk_id_t));
        // Mark disk as assigned to this group
        memcpy(&member_disks[i]->assigned_to_group_id, &new_group->id, sizeof(xsan_group_id_t));
    }

    if (xsan_list_append(dm->managed_disk_groups, new_group) == NULL) {
        XSAN_LOG_ERROR("Failed to append new disk group '%s' to managed list.", new_group->name);
        // Revert assigned_to_group_id for disks
        for (int i = 0; i < num_bdevs; ++i) {
             memset(&member_disks[i]->assigned_to_group_id, 0, sizeof(xsan_group_id_t));
        }
        _xsan_internal_disk_group_destroy_cb(new_group); // Frees new_group itself
        err = XSAN_ERROR_OUT_OF_MEMORY;
        goto cleanup_unlock;
    }

    if (group_id_out) {
        memcpy(group_id_out, &new_group->id, sizeof(xsan_group_id_t));
    }

    char group_uuid_str_log[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(group_uuid_str_log, sizeof(group_uuid_str_log), (struct spdk_uuid*)&new_group->id.data[0]);
    XSAN_LOG_INFO("Disk group '%s' (ID: %s, Type: %d) created successfully with %d disk(s).",
                  new_group->name, group_uuid_str_log, new_group->type, new_group->disk_count);

cleanup_unlock:
    pthread_mutex_unlock(&dm->lock);
    return err;
}

xsan_error_t xsan_disk_manager_disk_group_delete(xsan_disk_manager_t *dm, xsan_group_id_t group_id_to_delete) {
    if (!dm || !dm->initialized) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (spdk_uuid_is_null((struct spdk_uuid*)&group_id_to_delete.data[0])) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&dm->lock);
    xsan_error_t err = XSAN_ERROR_NOT_FOUND;
    xsan_list_node_t *list_node = xsan_list_get_head(dm->managed_disk_groups);
    xsan_disk_group_t *group_to_delete = NULL;

    while(list_node != NULL) {
        xsan_disk_group_t *current_group = (xsan_disk_group_t *)xsan_list_node_get_value(list_node);
        if (spdk_uuid_compare((struct spdk_uuid*)&current_group->id.data[0], (struct spdk_uuid*)&group_id_to_delete.data[0]) == 0) {
            group_to_delete = current_group;
            break;
        }
        list_node = xsan_list_node_next(list_node);
    }

    if (group_to_delete) {
        // TODO: Check if group is busy (e.g., has active volumes). For now, allow deletion.
        XSAN_LOG_INFO("Deleting disk group '%s' (ID: %s)...", group_to_delete->name, spdk_uuid_get_string((struct spdk_uuid*)&group_to_delete->id.data[0]));

        // Unassign disks from this group
        for (uint32_t i = 0; i < group_to_delete->disk_count; ++i) {
            xsan_disk_t *disk = xsan_disk_manager_find_disk_by_id(dm, group_to_delete->disk_ids[i]);
            if (disk) {
                // Check if the disk was indeed assigned to this group before clearing, to be safe
                if(spdk_uuid_compare((struct spdk_uuid*)&disk->assigned_to_group_id.data[0], (struct spdk_uuid*)&group_to_delete->id.data[0]) == 0) {
                    memset(&disk->assigned_to_group_id, 0, sizeof(xsan_group_id_t)); // Mark as unassigned
                    XSAN_LOG_DEBUG("Disk '%s' (bdev: %s) unassigned from deleted group.", spdk_uuid_get_string((struct spdk_uuid*)&disk->id.data[0]), disk->bdev_name);
                }
            }
        }

        // xsan_list_remove_node will call the _xsan_internal_disk_group_destroy_cb, which frees the group.
        xsan_list_remove_node(dm->managed_disk_groups, list_node);
        err = XSAN_OK;
        XSAN_LOG_INFO("Disk group (ID: %s) deleted successfully.", spdk_uuid_get_string((struct spdk_uuid*)&group_id_to_delete.data[0]));
    } else {
        XSAN_LOG_WARN("Disk group (ID: %s) not found for deletion.", spdk_uuid_get_string((struct spdk_uuid*)&group_id_to_delete.data[0]));
    }

    pthread_mutex_unlock(&dm->lock);
    return err;
}

xsan_error_t xsan_disk_manager_get_all_disk_groups(xsan_disk_manager_t *dm, xsan_disk_group_t ***groups_array_out, int *count_out) {
    if (!dm || !dm->initialized || !groups_array_out || !count_out) {
        if (count_out) *count_out = 0;
        if (groups_array_out) *groups_array_out = NULL;
        return XSAN_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&dm->lock);
    size_t num_groups = xsan_list_size(dm->managed_disk_groups);
    if (num_groups == 0) {
        *groups_array_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&dm->lock);
        return XSAN_OK;
    }

    *groups_array_out = (xsan_disk_group_t **)XSAN_MALLOC(sizeof(xsan_disk_group_t*) * num_groups);
    if (!*groups_array_out) {
        *count_out = 0;
        pthread_mutex_unlock(&dm->lock);
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    int i = 0;
    xsan_list_node_t *node;
    XSAN_LIST_FOREACH(dm->managed_disk_groups, node) {
         if (i < (int)num_groups) { // Defensive check
            (*groups_array_out)[i++] = (xsan_disk_group_t *)xsan_list_node_get_value(node);
        } else {
            break;
        }
    }
    *count_out = i;
    pthread_mutex_unlock(&dm->lock);
    return XSAN_OK;
}

void xsan_disk_manager_free_group_pointer_list(xsan_disk_group_t **group_ptr_array) {
    if (group_ptr_array) {
        XSAN_FREE(group_ptr_array);
    }
}

xsan_disk_group_t *xsan_disk_manager_find_disk_group_by_id(xsan_disk_manager_t *dm, xsan_group_id_t group_id_to_find) {
    if (!dm || !dm->initialized) return NULL;

    pthread_mutex_lock(&dm->lock);
    xsan_list_node_t *node;
    xsan_disk_group_t *found_group = NULL;
    XSAN_LIST_FOREACH(dm->managed_disk_groups, node) {
        xsan_disk_group_t *group = (xsan_disk_group_t *)xsan_list_node_get_value(node);
        if (spdk_uuid_compare((struct spdk_uuid*)&group->id.data[0], (struct spdk_uuid*)&group_id_to_find.data[0]) == 0) {
            found_group = group;
            break;
        }
    }
    pthread_mutex_unlock(&dm->lock);
    return found_group;
}

xsan_disk_group_t *xsan_disk_manager_find_disk_group_by_name(xsan_disk_manager_t *dm, const char *name) {
    if (!dm || !dm->initialized || !name) return NULL;

    pthread_mutex_lock(&dm->lock);
    xsan_list_node_t *node;
    xsan_disk_group_t *found_group = NULL;
    XSAN_LIST_FOREACH(dm->managed_disk_groups, node) {
        xsan_disk_group_t *group = (xsan_disk_group_t *)xsan_list_node_get_value(node);
        if (strncmp(group->name, name, XSAN_MAX_NAME_LEN) == 0) {
            found_group = group;
            break;
        }
    }
    pthread_mutex_unlock(&dm->lock);
    return found_group;
}

// A new directory `src/storage` will be implicitly created by this.
// Ensure CMakeLists.txt in `src` adds `add_subdirectory(storage)`
// and `src/storage/CMakeLists.txt` compiles this file into a library.
// (These CMake changes are part of a later step in the plan).
