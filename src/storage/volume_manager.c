#include "xsan_volume_manager.h"
#include "xsan_disk_manager.h" // For accessing disk group info
#include "xsan_storage.h"      // For xsan_volume_t, xsan_disk_group_t, xsan_disk_t
#include "xsan_memory.h"       // For XSAN_MALLOC, XSAN_FREE, XSAN_CALLOC
#include "xsan_list.h"         // For managing the list of volumes
#include "xsan_string_utils.h" // For xsan_strcpy_safe
#include "xsan_log.h"
#include "xsan_error.h"

#include "spdk/uuid.h"         // For spdk_uuid_generate, spdk_uuid_compare, spdk_uuid_is_null
#include <pthread.h>           // For pthread_mutex_t

// Internal structure for the Volume Manager
struct xsan_volume_manager {
    xsan_list_t *managed_volumes;       // List of xsan_volume_t structures
    xsan_disk_manager_t *disk_manager;  // Pointer to the disk manager instance (dependency)
    pthread_mutex_t lock;               // Mutex for thread-safe access to the volume list
    bool initialized;
};

// Global instance (singleton) - simplifies access for now.
static xsan_volume_manager_t *g_xsan_volume_manager_instance = NULL;

// Destructor callback for xsan_volume_t elements in xsan_list
static void _xsan_internal_volume_destroy_cb(void *volume_data) {
    if (volume_data) {
        xsan_volume_t *vol = (xsan_volume_t *)volume_data;
        XSAN_LOG_DEBUG("Volume Manager: Destroying xsan_volume (Name: '%s', ID: %s)",
                       vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]));
        // If volume had allocated extents or other resources, free them here.
        XSAN_FREE(vol);
    }
}

xsan_error_t xsan_volume_manager_init(xsan_disk_manager_t *disk_manager, xsan_volume_manager_t **vm_instance_out) {
    if (g_xsan_volume_manager_instance != NULL) {
        XSAN_LOG_WARN("XSAN Volume Manager already initialized.");
        if (vm_instance_out) *vm_instance_out = g_xsan_volume_manager_instance;
        return XSAN_OK;
    }
    if (!disk_manager) {
        XSAN_LOG_ERROR("Disk Manager instance is required to initialize Volume Manager.");
        if (vm_instance_out) *vm_instance_out = NULL;
        return XSAN_ERROR_INVALID_PARAM;
    }

    XSAN_LOG_INFO("Initializing XSAN Volume Manager...");
    xsan_volume_manager_t *vm = (xsan_volume_manager_t *)XSAN_MALLOC(sizeof(xsan_volume_manager_t));
    if (!vm) {
        XSAN_LOG_ERROR("Failed to allocate memory for XSAN Volume Manager.");
        if (vm_instance_out) *vm_instance_out = NULL;
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    vm->managed_volumes = xsan_list_create(_xsan_internal_volume_destroy_cb);
    if (!vm->managed_volumes) {
        XSAN_LOG_ERROR("Failed to create list for managed volumes.");
        XSAN_FREE(vm);
        if (vm_instance_out) *vm_instance_out = NULL;
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    if (pthread_mutex_init(&vm->lock, NULL) != 0) {
        XSAN_LOG_ERROR("Failed to initialize mutex for Volume Manager.");
        xsan_list_destroy(vm->managed_volumes);
        XSAN_FREE(vm);
        if (vm_instance_out) *vm_instance_out = NULL;
        return XSAN_ERROR_SYSTEM;
    }

    vm->disk_manager = disk_manager; // Store reference to disk manager
    vm->initialized = true;
    g_xsan_volume_manager_instance = vm;
    if (vm_instance_out) {
        *vm_instance_out = g_xsan_volume_manager_instance;
    }

    XSAN_LOG_INFO("XSAN Volume Manager initialized successfully.");
    // In a real system, load existing volume metadata from persistent storage here.
    // e.g., xsan_volume_manager_load_metadata(vm);
    return XSAN_OK;
}

void xsan_volume_manager_fini(xsan_volume_manager_t **vm_ptr) {
    xsan_volume_manager_t *vm_to_fini = NULL;
    if (vm_ptr && *vm_ptr) {
        vm_to_fini = *vm_ptr;
    } else if (g_xsan_volume_manager_instance) {
        vm_to_fini = g_xsan_volume_manager_instance;
    }

    if (!vm_to_fini || !vm_to_fini->initialized) {
        XSAN_LOG_INFO("XSAN Volume Manager already finalized or was not initialized.");
        if (vm_ptr) *vm_ptr = NULL;
        g_xsan_volume_manager_instance = NULL;
        return;
    }

    XSAN_LOG_INFO("Finalizing XSAN Volume Manager...");
    pthread_mutex_lock(&vm_to_fini->lock);

    // In a real system, save volume metadata to persistent storage before destroying.
    // e.g., xsan_volume_manager_save_metadata(vm_to_fini);
    xsan_list_destroy(vm_to_fini->managed_volumes);
    vm_to_fini->managed_volumes = NULL;
    vm_to_fini->disk_manager = NULL;

    vm_to_fini->initialized = false;
    pthread_mutex_unlock(&vm_to_fini->lock);
    pthread_mutex_destroy(&vm_to_fini->lock);

    XSAN_FREE(vm_to_fini);
    if (vm_ptr && *vm_ptr) {
        *vm_ptr = NULL;
    }
    if (vm_to_fini == g_xsan_volume_manager_instance) {
        g_xsan_volume_manager_instance = NULL;
    }
    XSAN_LOG_INFO("XSAN Volume Manager finalized.");
}

xsan_error_t xsan_volume_create(xsan_volume_manager_t *vm,
                                const char *name,
                                uint64_t size_bytes,
                                xsan_group_id_t group_id,
                                uint32_t logical_block_size_bytes,
                                bool thin_provisioned,
                                xsan_volume_id_t *new_volume_id_out) {
    if (!vm || !vm->initialized || !name || size_bytes == 0 || logical_block_size_bytes == 0 ||
        (logical_block_size_bytes & (logical_block_size_bytes - 1)) != 0 || // Must be power of 2
        spdk_uuid_is_null((struct spdk_uuid*)&group_id.data[0])) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if ((size_bytes % logical_block_size_bytes) != 0) {
        XSAN_LOG_ERROR("Volume size %lu B is not a multiple of logical block size %u B.", size_bytes, logical_block_size_bytes);
        return XSAN_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&vm->lock);
    xsan_error_t err = XSAN_OK;

    // Check if volume name already exists (must be done while holding vm->lock)
    xsan_list_node_t *iter_node;
    XSAN_LIST_FOREACH(vm->managed_volumes, iter_node) {
        xsan_volume_t *vol_iter = (xsan_volume_t*)xsan_list_node_get_value(iter_node);
        if (strncmp(vol_iter->name, name, XSAN_MAX_NAME_LEN) == 0) {
            XSAN_LOG_WARN("Volume with name '%s' already exists.", name);
            pthread_mutex_unlock(&vm->lock);
            return XSAN_ERROR_ALREADY_EXISTS;
        }
    }
    // xsan_volume_get_by_name also locks, so direct iteration is better here.

    xsan_disk_group_t *group = xsan_disk_manager_find_disk_group_by_id(vm->disk_manager, group_id);
    if (!group) {
        XSAN_LOG_ERROR("Disk group ID for volume '%s' not found.", name);
        err = XSAN_ERROR_NOT_FOUND;
        goto cleanup_unlock;
    }
    if (group->state != XSAN_STORAGE_STATE_ONLINE) {
        char group_uuid_str[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(group_uuid_str, sizeof(group_uuid_str), (struct spdk_uuid*)&group->id.data[0]);
        XSAN_LOG_ERROR("Disk group '%s' (ID: %s) is not online for volume '%s'. State: %d",
                       group->name, group_uuid_str, name, group->state);
        err = XSAN_ERROR_RESOURCE_BUSY;
        goto cleanup_unlock;
    }

    // TODO: More sophisticated capacity check involving actual free space on group, not just usable.
    // For now, a simple check for thick provisioning.
    if (!thin_provisioned && size_bytes > group->usable_capacity_bytes) {
        XSAN_LOG_ERROR("Insufficient usable capacity in disk group '%s' for volume '%s'. Required: %lu, Usable: %lu",
                       group->name, name, size_bytes, group->usable_capacity_bytes);
        err = XSAN_ERROR_INSUFFICIENT_SPACE;
        goto cleanup_unlock;
    }

    xsan_volume_t *new_volume = (xsan_volume_t *)XSAN_MALLOC(sizeof(xsan_volume_t));
    if (!new_volume) {
        err = XSAN_ERROR_OUT_OF_MEMORY;
        goto cleanup_unlock;
    }
    memset(new_volume, 0, sizeof(xsan_volume_t));

    spdk_uuid_generate((struct spdk_uuid *)&new_volume->id.data[0]);
    xsan_strcpy_safe(new_volume->name, name, XSAN_MAX_NAME_LEN);
    new_volume->size_bytes = size_bytes;
    new_volume->block_size_bytes = logical_block_size_bytes;
    new_volume->num_blocks = size_bytes / logical_block_size_bytes;
    new_volume->state = XSAN_STORAGE_STATE_ONLINE; // Initial state
    memcpy(&new_volume->source_group_id, &group_id, sizeof(xsan_group_id_t));
    new_volume->thin_provisioned = thin_provisioned;
    new_volume->allocated_bytes = thin_provisioned ? 0 : size_bytes;

    if (xsan_list_append(vm->managed_volumes, new_volume) == NULL) {
        XSAN_LOG_ERROR("Failed to append new volume '%s' to managed list.", new_volume->name);
        _xsan_internal_volume_destroy_cb(new_volume);
        err = XSAN_ERROR_OUT_OF_MEMORY;
        goto cleanup_unlock;
    }

    if (new_volume_id_out) {
        memcpy(new_volume_id_out, &new_volume->id, sizeof(xsan_volume_id_t));
    }

    char vol_uuid_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_uuid_str, sizeof(vol_uuid_str), (struct spdk_uuid*)&new_volume->id.data[0]);
    XSAN_LOG_INFO("Volume '%s' (ID: %s) created. Size: %.2f GiB, BlockSize: %u B, Thin: %s, Group: %s",
                  new_volume->name, vol_uuid_str, (double)new_volume->size_bytes / (1024.0*1024.0*1024.0),
                  new_volume->block_size_bytes, new_volume->thin_provisioned ? "yes" : "no",
                  spdk_uuid_get_string((struct spdk_uuid*)&group->id.data[0]));

cleanup_unlock:
    pthread_mutex_unlock(&vm->lock);
    return err;
}

xsan_error_t xsan_volume_delete(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id) {
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&vm->lock);
    xsan_error_t err = XSAN_ERROR_NOT_FOUND;
    xsan_list_node_t *list_node = xsan_list_get_head(vm->managed_volumes);
    xsan_volume_t *volume_to_delete = NULL;

    while(list_node != NULL) {
        xsan_volume_t *current_volume = (xsan_volume_t *)xsan_list_node_get_value(list_node);
        if (spdk_uuid_compare((struct spdk_uuid*)&current_volume->id.data[0], (struct spdk_uuid*)&volume_id.data[0]) == 0) {
            volume_to_delete = current_volume;
            break;
        }
        list_node = xsan_list_node_next(list_node);
    }

    if (volume_to_delete) {
        // TODO: Check if volume is busy (e.g., mounted).
        // TODO: Release allocated space (needs extent tracking / block allocator for the group).
        char vol_uuid_str_log[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(vol_uuid_str_log, sizeof(vol_uuid_str_log), (struct spdk_uuid*)&volume_to_delete->id.data[0]);
        XSAN_LOG_INFO("Deleting volume '%s' (ID: %s)...", volume_to_delete->name, vol_uuid_str_log);

        xsan_list_remove_node(vm->managed_volumes, list_node); // This calls _xsan_internal_volume_destroy_cb
        err = XSAN_OK;
        XSAN_LOG_INFO("Volume (ID: %s) deleted successfully.", vol_uuid_str_log);
    } else {
        char vol_uuid_str_log[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(vol_uuid_str_log, sizeof(vol_uuid_str_log), (struct spdk_uuid*)&volume_id.data[0]);
        XSAN_LOG_WARN("Volume (ID: %s) not found for deletion.", vol_uuid_str_log);
    }

    pthread_mutex_unlock(&vm->lock);
    return err;
}

xsan_volume_t *xsan_volume_get_by_id(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id) {
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) {
        return NULL;
    }
    pthread_mutex_lock(&vm->lock);
    xsan_list_node_t *node;
    xsan_volume_t *found_volume = NULL;
    XSAN_LIST_FOREACH(vm->managed_volumes, node) {
        xsan_volume_t *vol = (xsan_volume_t *)xsan_list_node_get_value(node);
        if (spdk_uuid_compare((struct spdk_uuid*)&vol->id.data[0], (struct spdk_uuid*)&volume_id.data[0]) == 0) {
            found_volume = vol;
            break;
        }
    }
    pthread_mutex_unlock(&vm->lock);
    return found_volume;
}

xsan_volume_t *xsan_volume_get_by_name(xsan_volume_manager_t *vm, const char *name) {
    if (!vm || !vm->initialized || !name) {
        return NULL;
    }
    pthread_mutex_lock(&vm->lock);
    xsan_list_node_t *node;
    xsan_volume_t *found_volume = NULL;
    XSAN_LIST_FOREACH(vm->managed_volumes, node) {
        xsan_volume_t *vol = (xsan_volume_t *)xsan_list_node_get_value(node);
        if (strncmp(vol->name, name, XSAN_MAX_NAME_LEN) == 0) {
            found_volume = vol;
            break;
        }
    }
    pthread_mutex_unlock(&vm->lock);
    return found_volume;
}

xsan_error_t xsan_volume_list_all(xsan_volume_manager_t *vm, xsan_volume_t ***volumes_array_out, int *count_out) {
    if (!vm || !vm->initialized || !volumes_array_out || !count_out) {
        if(count_out) *count_out = 0;
        if(volumes_array_out) *volumes_array_out = NULL;
        return XSAN_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&vm->lock);
    size_t num_volumes = xsan_list_size(vm->managed_volumes);
    if (num_volumes == 0) {
        *volumes_array_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&vm->lock);
        return XSAN_OK;
    }

    *volumes_array_out = (xsan_volume_t **)XSAN_MALLOC(sizeof(xsan_volume_t*) * num_volumes);
    if (!*volumes_array_out) {
        *count_out = 0;
        pthread_mutex_unlock(&vm->lock);
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    int i = 0;
    xsan_list_node_t *node;
    XSAN_LIST_FOREACH(vm->managed_volumes, node) {
        if (i < (int)num_volumes) {
             (*volumes_array_out)[i++] = (xsan_volume_t *)xsan_list_node_get_value(node);
        } else {
            break;
        }
    }
    *count_out = i;
    pthread_mutex_unlock(&vm->lock);
    return XSAN_OK;
}

void xsan_volume_manager_free_volume_pointer_list(xsan_volume_t **volume_ptr_array) {
    if (volume_ptr_array) {
        XSAN_FREE(volume_ptr_array);
    }
}

xsan_error_t xsan_volume_map_lba_to_physical(xsan_volume_manager_t *vm,
                                             xsan_volume_id_t volume_id,
                                             uint64_t logical_block_idx,
                                             xsan_disk_id_t *out_disk_id,
                                             uint64_t *out_physical_block_idx) {
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0]) || !out_disk_id || !out_physical_block_idx) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&vm->lock);
    xsan_volume_t *vol = NULL;
    xsan_list_node_t *iter_node;
    XSAN_LIST_FOREACH(vm->managed_volumes, iter_node) {
        xsan_volume_t *v = (xsan_volume_t*)xsan_list_node_get_value(iter_node);
        if(spdk_uuid_compare((struct spdk_uuid*)&v->id.data[0], (struct spdk_uuid*)&volume_id.data[0]) == 0) {
            vol = v;
            break;
        }
    }

    if (!vol) {
        pthread_mutex_unlock(&vm->lock);
        return XSAN_ERROR_NOT_FOUND;
    }

    if (logical_block_idx >= vol->num_blocks) {
        pthread_mutex_unlock(&vm->lock);
        return XSAN_ERROR_OUT_OF_BOUNDS;
    }

    // Get the source disk group - xsan_disk_manager_find_disk_group_by_id is thread-safe itself
    // No need to hold vm->lock while calling disk_manager if disk_manager is also properly locked.
    // However, vol->source_group_id read should be under lock.
    xsan_group_id_t src_group_id;
    memcpy(&src_group_id, &vol->source_group_id, sizeof(xsan_group_id_t));
    pthread_mutex_unlock(&vm->lock); // Release vm lock before calling dm, if dm is also locked.
                                     // Or, ensure dm calls don't re-lock if already holding vm->lock (complex).
                                     // For now, assume dm calls are safe.

    xsan_disk_group_t *group = xsan_disk_manager_find_disk_group_by_id(vm->disk_manager, src_group_id);
    if (!group) {
        XSAN_LOG_ERROR("Volume '%s' (ID: %s) source group ID not found.", vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]));
        return XSAN_ERROR_NOT_FOUND;
    }

    if (group->disk_count == 0) {
        XSAN_LOG_ERROR("Volume '%s' source group '%s' has no disks.", vol->name, group->name);
        return XSAN_ERROR_STORAGE_GENERIC;
    }

    // --- Simple Mapping Logic (Phase 1: Volume maps linearly to the first disk in the group) ---
    // This is a highly simplified placeholder. Real mapping involves extent management,
    // striping, replication, tiering, etc.
    xsan_disk_id_t target_disk_id_from_group; // This is xsan_disk_id_t (UUID)
    memcpy(&target_disk_id_from_group, &group->disk_ids[0], sizeof(xsan_disk_id_t));

    xsan_disk_t *target_disk = xsan_disk_manager_find_disk_by_id(vm->disk_manager, target_disk_id_from_group);

    if (!target_disk) {
        XSAN_LOG_ERROR("Target disk (ID: %s) for volume '%s' not found in disk manager.", spdk_uuid_get_string((struct spdk_uuid*)&target_disk_id_from_group.data[0]), vol->name);
        return XSAN_ERROR_NOT_FOUND;
    }

    if (target_disk->block_size_bytes == 0) {
         XSAN_LOG_ERROR("Target disk '%s' has zero block size.", target_disk->bdev_name);
         return XSAN_ERROR_STORAGE_GENERIC;
    }

    // Calculate physical block index on the target disk.
    // This assumes the volume's logical blocks are mapped 1:1 or N:1 to target disk blocks
    // if block sizes differ.
    // Physical offset in bytes = logical_block_idx * vol->block_size_bytes
    // Physical block on target disk = (Physical offset in bytes) / target_disk->block_size_bytes
    *out_physical_block_idx = (logical_block_idx * vol->block_size_bytes) / target_disk->block_size_bytes;

    // Ensure this physical block index is within the target disk's capacity
    uint64_t required_physical_blocks_for_this_lba = (vol->block_size_bytes + target_disk->block_size_bytes -1) / target_disk->block_size_bytes;
    if (*out_physical_block_idx + (required_physical_blocks_for_this_lba -1) >= target_disk->num_blocks) {
         XSAN_LOG_ERROR("LBA mapping for volume '%s' (LBA %lu) exceeds capacity of target disk '%s'.",
                        vol->name, logical_block_idx, target_disk->bdev_name);
        return XSAN_ERROR_OUT_OF_BOUNDS;
    }

    memcpy(out_disk_id, &target_disk->id, sizeof(xsan_disk_id_t)); // Copy the XSAN ID of the disk

    return XSAN_OK;
}
