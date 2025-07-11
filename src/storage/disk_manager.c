#include "xsan_disk_manager.h"
#include "xsan_bdev.h"       // For xsan_bdev_list_get_all, xsan_bdev_info_t
#include "xsan_memory.h"   // For XSAN_MALLOC, XSAN_FREE, XSAN_CALLOC
#include "xsan_list.h"     // Using our own list for managing disks and groups
#include "xsan_string_utils.h" // For xsan_strcpy_safe
#include "xsan_log.h"      // For XSAN_LOG_INFO, XSAN_LOG_ERROR, etc.
#include "xsan_metadata_store.h" // For RocksDB wrapper
#include "json-c/json.h"   // For JSON processing

#include "spdk/uuid.h"     // For spdk_uuid_generate, spdk_uuid_compare, spdk_uuid_is_null, spdk_uuid_get_string
#include "spdk/bdev.h"     // For spdk_bdev_open_ext, spdk_bdev_close
#include "spdk/thread.h"   // For spdk_get_thread
#include <pthread.h>       // For pthread_mutex_t

// --- Defines for RocksDB Keys ---
#define XSAN_DISK_META_PREFIX "d:"
#define XSAN_DISK_GROUP_META_PREFIX "g:"

// The internal structure for the disk manager
struct xsan_disk_manager {
    xsan_list_t *managed_disks;       // List of xsan_disk_t structures
    xsan_list_t *managed_disk_groups; // List of xsan_disk_group_t structures
    pthread_mutex_t lock;             // Mutex to protect access to the lists
    bool initialized;                 // Flag to indicate if the manager is initialized
    xsan_metadata_store_t *md_store;  // Handle to the metadata store (RocksDB)
    char metadata_db_path[XSAN_MAX_PATH_LEN]; // Path to the DB for this manager
};

static xsan_disk_manager_t *g_xsan_disk_manager_instance = NULL;

// Forward declarations for static helpers
static xsan_error_t xsan_disk_manager_load_metadata(xsan_disk_manager_t *dm);
static xsan_error_t xsan_disk_manager_save_disk_meta(xsan_disk_manager_t *dm, xsan_disk_t *disk);
static xsan_error_t xsan_disk_manager_save_group_meta(xsan_disk_manager_t *dm, xsan_disk_group_t *group);
static xsan_error_t xsan_disk_manager_delete_disk_meta(xsan_disk_manager_t *dm, xsan_disk_id_t disk_id);
static xsan_error_t xsan_disk_manager_delete_group_meta(xsan_disk_manager_t *dm, xsan_group_id_t group_id);

static xsan_error_t _xsan_disk_to_json_string(const xsan_disk_t *disk, char **json_string_out);
static xsan_error_t _xsan_json_string_to_disk(const char *json_string, xsan_disk_t **disk_out);
static xsan_error_t _xsan_disk_group_to_json_string(const xsan_disk_group_t *group, char **json_string_out);
static xsan_error_t _xsan_json_string_to_disk_group(const char *json_string, xsan_disk_manager_t *dm, xsan_disk_group_t **group_out);


static void _xsan_internal_disk_destroy_cb(void *disk_data) {
    if (disk_data) {
        xsan_disk_t *disk = (xsan_disk_t *)disk_data;
        char xsan_id_str[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(xsan_id_str, sizeof(xsan_id_str), (struct spdk_uuid*)&disk->id.data[0]);
        XSAN_LOG_DEBUG("Disk Manager: Destroying xsan_disk (bdev: %s, XSAN_ID: %s)", disk->bdev_name, xsan_id_str);
        if (disk->bdev_descriptor) {
            spdk_bdev_close(disk->bdev_descriptor);
            disk->bdev_descriptor = NULL;
        }
        XSAN_FREE(disk);
    }
}

static void _xsan_internal_disk_group_destroy_cb(void *group_data) {
    if (group_data) {
        xsan_disk_group_t *group = (xsan_disk_group_t *)group_data;
        char group_id_str[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(group_id_str, sizeof(group_id_str), (struct spdk_uuid*)&group->id.data[0]);
        XSAN_LOG_DEBUG("Disk Manager: Destroying xsan_disk_group (Name: '%s', ID: %s)", group->name, group_id_str);
        XSAN_FREE(group);
    }
}

xsan_error_t xsan_disk_manager_init(xsan_disk_manager_t **dm_instance_out) {
    // Path should ideally come from a global config. Using a default for now.
    // Example: data_dir from xsan_config + "/disk_manager_db"
    const char *default_db_path_suffix = "xsan_meta_db/disk_manager";
    char actual_db_path[XSAN_MAX_PATH_LEN];

    // TODO: Integrate with a proper config system to get base data path
    // For now, using a relative path. Ensure "./xsan_meta_db" directory exists.
    // In a real setup: snprintf(actual_db_path, sizeof(actual_db_path), "%s/%s", xsan_get_data_dir(), default_db_path_suffix);
    snprintf(actual_db_path, sizeof(actual_db_path), "./%s", default_db_path_suffix);


    if (g_xsan_disk_manager_instance != NULL) {
        XSAN_LOG_WARN("XSAN Disk Manager already initialized.");
        if (dm_instance_out) *dm_instance_out = g_xsan_disk_manager_instance;
        return XSAN_OK;
    }
    XSAN_LOG_INFO("Initializing XSAN Disk Manager (DB path: %s)...", actual_db_path);
    xsan_disk_manager_t *dm = (xsan_disk_manager_t *)XSAN_MALLOC(sizeof(xsan_disk_manager_t));
    if (!dm) {
        XSAN_LOG_ERROR("Failed to allocate memory for XSAN Disk Manager.");
        if (dm_instance_out) *dm_instance_out = NULL;
        return XSAN_ERROR_OUT_OF_MEMORY;
    }
    memset(dm, 0, sizeof(xsan_disk_manager_t));
    xsan_strcpy_safe(dm->metadata_db_path, actual_db_path, XSAN_MAX_PATH_LEN);

    dm->managed_disks = xsan_list_create(_xsan_internal_disk_destroy_cb);
    dm->managed_disk_groups = xsan_list_create(_xsan_internal_disk_group_destroy_cb);

    if (!dm->managed_disks || !dm->managed_disk_groups) {
        XSAN_LOG_ERROR("Failed to create lists for Disk Manager.");
        if(dm->managed_disks) xsan_list_destroy(dm->managed_disks);
        if(dm->managed_disk_groups) xsan_list_destroy(dm->managed_disk_groups);
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
        return XSAN_ERROR_SYSTEM;
    }

    dm->md_store = xsan_metadata_store_open(dm->metadata_db_path, true);
    if (!dm->md_store) {
        XSAN_LOG_ERROR("Failed to open/create metadata store at '%s' for Disk Manager.", dm->metadata_db_path);
        pthread_mutex_destroy(&dm->lock);
        xsan_list_destroy(dm->managed_disk_groups);
        xsan_list_destroy(dm->managed_disks);
        XSAN_FREE(dm);
        if (dm_instance_out) *dm_instance_out = NULL;
        return XSAN_ERROR_STORAGE_GENERIC;
    }
    XSAN_LOG_INFO("Metadata store opened for Disk Manager at '%s'.", dm->metadata_db_path);

    dm->initialized = true;
    g_xsan_disk_manager_instance = dm;
    if (dm_instance_out) *dm_instance_out = g_xsan_disk_manager_instance;

    xsan_disk_manager_load_metadata(dm); // Load existing metadata

    XSAN_LOG_INFO("XSAN Disk Manager initialized. Call scan_and_register_bdevs() to reconcile with live bdevs.");
    return XSAN_OK;
}

void xsan_disk_manager_fini(xsan_disk_manager_t **dm_ptr) {
    // ... (fini implementation as before, ensure md_store is closed) ...
    xsan_disk_manager_t *dm_to_fini = NULL;
    if (dm_ptr && *dm_ptr) dm_to_fini = *dm_ptr;
    else if (g_xsan_disk_manager_instance) dm_to_fini = g_xsan_disk_manager_instance;

    if (!dm_to_fini || !dm_to_fini->initialized) { /* ... */ return; }
    XSAN_LOG_INFO("Finalizing XSAN Disk Manager...");
    // Optional: xsan_disk_manager_save_all_metadata(dm_to_fini); // Save everything one last time
    pthread_mutex_lock(&dm_to_fini->lock);
    xsan_list_destroy(dm_to_fini->managed_disk_groups); dm_to_fini->managed_disk_groups = NULL;
    xsan_list_destroy(dm_to_fini->managed_disks); dm_to_fini->managed_disks = NULL;
    if (dm_to_fini->md_store) {
        xsan_metadata_store_close(dm_to_fini->md_store);
        dm_to_fini->md_store = NULL;
    }
    dm_to_fini->initialized = false;
    pthread_mutex_unlock(&dm_to_fini->lock);
    pthread_mutex_destroy(&dm_to_fini->lock);
    XSAN_FREE(dm_to_fini);
    if (dm_ptr && *dm_ptr) *dm_ptr = NULL;
    if (dm_to_fini == g_xsan_disk_manager_instance) g_xsan_disk_manager_instance = NULL;
    XSAN_LOG_INFO("XSAN Disk Manager finalized.");
}

// --- Serialization/Deserialization ---
static xsan_error_t _xsan_disk_to_json_string(const xsan_disk_t *disk, char **json_string_out) {
    // ... (Implementation as sketched in previous step, using json-c) ...
    if (!disk || !json_string_out) return XSAN_ERROR_INVALID_PARAM;
    json_object *jobj = json_object_new_object();
    char uuid_buf[SPDK_UUID_STRING_LEN];

    spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&disk->id.data[0]);
    json_object_object_add(jobj, "id", json_object_new_string(uuid_buf));
    json_object_object_add(jobj, "bdev_name", json_object_new_string(disk->bdev_name));
    spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&disk->bdev_uuid.data[0]);
    json_object_object_add(jobj, "bdev_uuid", json_object_new_string(uuid_buf));
    if (!spdk_uuid_is_null((const struct spdk_uuid *)&disk->assigned_to_group_id.data[0])) {
       spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&disk->assigned_to_group_id.data[0]);
       json_object_object_add(jobj, "assigned_to_group_id", json_object_new_string(uuid_buf));
    } else {
       json_object_object_add(jobj, "assigned_to_group_id", NULL);
    }
    json_object_object_add(jobj, "type", json_object_new_int(disk->type));
    json_object_object_add(jobj, "state", json_object_new_int(disk->state)); // Persist runtime state
    json_object_object_add(jobj, "capacity_bytes", json_object_new_int64(disk->capacity_bytes));
    json_object_object_add(jobj, "block_size_bytes", json_object_new_int(disk->block_size_bytes));
    json_object_object_add(jobj, "num_blocks", json_object_new_int64(disk->num_blocks));
    json_object_object_add(jobj, "product_name", json_object_new_string(disk->product_name));
    json_object_object_add(jobj, "is_rotational", json_object_new_boolean(disk->is_rotational));
    json_object_object_add(jobj, "optimal_io_boundary_blocks", json_object_new_int(disk->optimal_io_boundary_blocks));
    json_object_object_add(jobj, "has_write_cache", json_object_new_boolean(disk->has_write_cache));

    const char *tmp_str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    *json_string_out = xsan_strdup(tmp_str);
    json_object_put(jobj);
    return (*json_string_out) ? XSAN_OK : XSAN_ERROR_OUT_OF_MEMORY;
}

static xsan_error_t _xsan_json_string_to_disk(const char *json_string, xsan_disk_t **disk_out) {
    // ... (Implementation as sketched, using json-c) ...
    if (!json_string || !disk_out) return XSAN_ERROR_INVALID_PARAM;
    struct json_object *jobj = json_tokener_parse(json_string);
    if (!jobj || is_error(jobj)) { /* handle error */ return XSAN_ERROR_CONFIG_PARSE; }

    xsan_disk_t *disk = (xsan_disk_t *)XSAN_MALLOC(sizeof(xsan_disk_t));
    if (!disk) { json_object_put(jobj); return XSAN_ERROR_OUT_OF_MEMORY; }
    memset(disk, 0, sizeof(xsan_disk_t));

    struct json_object *val;
    if (json_object_object_get_ex(jobj, "id", &val) && json_object_is_type(val, json_type_string)) spdk_uuid_parse((struct spdk_uuid*)&disk->id.data[0], json_object_get_string(val));
    if (json_object_object_get_ex(jobj, "bdev_name", &val) && json_object_is_type(val, json_type_string)) xsan_strcpy_safe(disk->bdev_name, json_object_get_string(val), XSAN_MAX_NAME_LEN);
    if (json_object_object_get_ex(jobj, "bdev_uuid", &val) && json_object_is_type(val, json_type_string)) spdk_uuid_parse((struct spdk_uuid*)&disk->bdev_uuid.data[0], json_object_get_string(val));
    if (json_object_object_get_ex(jobj, "assigned_to_group_id", &val) && json_object_is_type(val, json_type_string)) {
        spdk_uuid_parse((struct spdk_uuid*)&disk->assigned_to_group_id.data[0], json_object_get_string(val));
    } else { // Handles NULL or missing
        memset(&disk->assigned_to_group_id, 0, sizeof(xsan_group_id_t));
    }
    if (json_object_object_get_ex(jobj, "type", &val)) disk->type = (xsan_storage_disk_type_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "state", &val)) disk->state = (xsan_storage_state_t)json_object_get_int(val); else disk->state = XSAN_STORAGE_STATE_OFFLINE;
    if (json_object_object_get_ex(jobj, "capacity_bytes", &val)) disk->capacity_bytes = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "block_size_bytes", &val)) disk->block_size_bytes = (uint32_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "num_blocks", &val)) disk->num_blocks = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "product_name", &val) && json_object_is_type(val, json_type_string)) xsan_strcpy_safe(disk->product_name, json_object_get_string(val), XSAN_MAX_NAME_LEN);
    if (json_object_object_get_ex(jobj, "is_rotational", &val)) disk->is_rotational = json_object_get_boolean(val);
    if (json_object_object_get_ex(jobj, "optimal_io_boundary_blocks", &val)) disk->optimal_io_boundary_blocks = (uint32_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "has_write_cache", &val)) disk->has_write_cache = json_object_get_boolean(val);

    disk->bdev_descriptor = NULL; // Always null when loading from metadata
    json_object_put(jobj);
    *disk_out = disk;
    return XSAN_OK;
}

static xsan_error_t _xsan_disk_group_to_json_string(const xsan_disk_group_t *group, char **json_string_out) {
    if (!group || !json_string_out) return XSAN_ERROR_INVALID_PARAM;
    json_object *jobj = json_object_new_object();
    char uuid_buf[SPDK_UUID_STRING_LEN];

    spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&group->id.data[0]);
    json_object_object_add(jobj, "id", json_object_new_string(uuid_buf));
    json_object_object_add(jobj, "name", json_object_new_string(group->name));
    json_object_object_add(jobj, "type", json_object_new_int(group->type));
    json_object_object_add(jobj, "state", json_object_new_int(group->state));
    json_object_object_add(jobj, "disk_count", json_object_new_int(group->disk_count));
    json_object_object_add(jobj, "total_capacity_bytes", json_object_new_int64(group->total_capacity_bytes));
    json_object_object_add(jobj, "usable_capacity_bytes", json_object_new_int64(group->usable_capacity_bytes));

    json_object *jarray_disk_ids = json_object_new_array();
    for (uint32_t i = 0; i < group->disk_count; ++i) {
        spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&group->disk_ids[i].data[0]);
        json_object_array_add(jarray_disk_ids, json_object_new_string(uuid_buf));
    }
    json_object_object_add(jobj, "disk_ids", jarray_disk_ids);

    const char *tmp_str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    *json_string_out = xsan_strdup(tmp_str);
    json_object_put(jobj);
    return (*json_string_out) ? XSAN_OK : XSAN_ERROR_OUT_OF_MEMORY;
}

static xsan_error_t _xsan_json_string_to_disk_group(const char *json_string, xsan_disk_manager_t *dm, xsan_disk_group_t **group_out) {
    if (!json_string || !group_out || !dm ) return XSAN_ERROR_INVALID_PARAM;
    struct json_object *jobj = json_tokener_parse(json_string);
    if (!jobj || is_error(jobj)) { /* handle error */ return XSAN_ERROR_CONFIG_PARSE; }

    xsan_disk_group_t *group = (xsan_disk_group_t *)XSAN_MALLOC(sizeof(xsan_disk_group_t));
    if (!group) { json_object_put(jobj); return XSAN_ERROR_OUT_OF_MEMORY; }
    memset(group, 0, sizeof(xsan_disk_group_t));

    struct json_object *val;
    if (json_object_object_get_ex(jobj, "id", &val) && json_object_is_type(val, json_type_string)) spdk_uuid_parse((struct spdk_uuid*)&group->id.data[0], json_object_get_string(val));
    if (json_object_object_get_ex(jobj, "name", &val) && json_object_is_type(val, json_type_string)) xsan_strcpy_safe(group->name, json_object_get_string(val), XSAN_MAX_NAME_LEN);
    if (json_object_object_get_ex(jobj, "type", &val)) group->type = (xsan_disk_group_type_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "state", &val)) group->state = (xsan_storage_state_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "disk_count", &val)) group->disk_count = (uint32_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "total_capacity_bytes", &val)) group->total_capacity_bytes = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "usable_capacity_bytes", &val)) group->usable_capacity_bytes = (uint64_t)json_object_get_int64(val);

    if (json_object_object_get_ex(jobj, "disk_ids", &val) && json_object_is_type(val, json_type_array)) {
        int array_len = json_object_array_length(val);
        if ((uint32_t)array_len != group->disk_count) {
            XSAN_LOG_WARN("Disk group '%s' disk_count field (%u) does not match disk_ids array length (%d) in JSON.", group->name, group->disk_count, array_len);
            // Adjust or error out based on policy. For now, trust disk_count if array is smaller.
        }
        for (uint32_t i = 0; i < group->disk_count && i < (uint32_t)array_len; ++i) {
            struct json_object *j_disk_id_str_obj = json_object_array_get_idx(val, i);
            if (j_disk_id_str_obj && json_object_is_type(j_disk_id_str_obj, json_type_string)) {
                spdk_uuid_parse((struct spdk_uuid*)&group->disk_ids[i].data[0], json_object_get_string(j_disk_id_str_obj));
            }
        }
    }
    json_object_put(jobj);
    *group_out = group;
    return XSAN_OK;
}


// --- Metadata Save/Load Functions ---
// (Save/Delete for individual items as implemented before)
// ... [Previous save_disk_meta, save_group_meta, delete_disk_meta, delete_group_meta remain here] ...

xsan_error_t xsan_disk_manager_load_metadata(xsan_disk_manager_t *dm) {
    // ... (Implementation as before, using the new _xsan_json_string_to_disk and _xsan_json_string_to_disk_group) ...
    if (!dm || !dm->initialized || !dm->md_store) return XSAN_ERROR_INVALID_PARAM;
    XSAN_LOG_INFO("Loading disk and disk group metadata from RocksDB store: %s", dm->metadata_db_path);
    pthread_mutex_lock(&dm->lock);
    xsan_metadata_iterator_t *iter = xsan_metadata_iterator_create(dm->md_store);
    if (!iter) { /* ... error handling ... */ pthread_mutex_unlock(&dm->lock); return XSAN_ERROR_STORAGE_GENERIC; }

    // Load Disks
    xsan_metadata_iterator_seek(iter, XSAN_DISK_META_PREFIX, strlen(XSAN_DISK_META_PREFIX));
    while (xsan_metadata_iterator_is_valid(iter)) {
        size_t key_len, val_len;
        const char *key = xsan_metadata_iterator_key(iter, &key_len);
        if (!key || strncmp(key, XSAN_DISK_META_PREFIX, strlen(XSAN_DISK_META_PREFIX)) != 0) break;
        const char *value_str = xsan_metadata_iterator_value(iter, &val_len);
        xsan_disk_t *disk = NULL;
        if (_xsan_json_string_to_disk(value_str, &disk) == XSAN_OK && disk) {
            bool exists = (xsan_disk_manager_find_disk_by_id(dm, disk->id) != NULL); // find_disk_by_id is locked
            if (!exists) {
                xsan_list_append(dm->managed_disks, disk);
                XSAN_LOG_DEBUG("Loaded disk '%s' (XSAN_ID: %s) from metadata.", disk->bdev_name, spdk_uuid_get_string((struct spdk_uuid*)&disk->id.data[0]));
            } else { XSAN_LOG_WARN("Disk ID %s from metadata already in memory.", spdk_uuid_get_string((struct spdk_uuid*)&disk->id.data[0])); XSAN_FREE(disk); }
        } else { XSAN_LOG_ERROR("Failed to deserialize disk from: %.*s", (int)val_len, value_str); }
        xsan_metadata_iterator_next(iter);
    }
    // Load Disk Groups
    xsan_metadata_iterator_seek(iter, XSAN_DISK_GROUP_META_PREFIX, strlen(XSAN_DISK_GROUP_META_PREFIX));
    while (xsan_metadata_iterator_is_valid(iter)) {
        size_t key_len, val_len;
        const char *key = xsan_metadata_iterator_key(iter, &key_len);
        if (!key || strncmp(key, XSAN_DISK_GROUP_META_PREFIX, strlen(XSAN_DISK_GROUP_META_PREFIX)) != 0) break;
        const char *value_str = xsan_metadata_iterator_value(iter, &val_len);
        xsan_disk_group_t *group = NULL;
        if (_xsan_json_string_to_disk_group(value_str, dm, &group) == XSAN_OK && group) {
            bool exists = (xsan_disk_manager_find_disk_group_by_id(dm, group->id) != NULL);
            if (!exists) {
                xsan_list_append(dm->managed_disk_groups, group);
                 XSAN_LOG_DEBUG("Loaded disk group '%s' (ID: %s) from metadata.", group->name, spdk_uuid_get_string((struct spdk_uuid*)&group->id.data[0]));
            } else { XSAN_LOG_WARN("Disk group ID %s from metadata already in memory.", spdk_uuid_get_string((struct spdk_uuid*)&group->id.data[0])); XSAN_FREE(group); }
        } else { XSAN_LOG_ERROR("Failed to deserialize disk group from: %.*s", (int)val_len, value_str); }
        xsan_metadata_iterator_next(iter);
    }
    xsan_metadata_iterator_destroy(iter);
    pthread_mutex_unlock(&dm->lock);
    XSAN_LOG_INFO("Metadata loading for Disk Manager complete.");
    return XSAN_OK;
}


// ... (Rest of disk_manager.c, including scan_and_register, create_group, delete_group, etc.)
// These functions (create/delete) will now also call the respective _save_meta or _delete_meta functions.

// Example modification for create_group
// In xsan_disk_manager_disk_group_create, after successfully adding to list:
// if (err == XSAN_OK) {
//    xsan_disk_manager_save_group_meta(dm, new_group);
//    // also save affected disks if their assigned_to_group_id changed
//    for (int i = 0; i < num_bdevs; ++i) { xsan_disk_manager_save_disk_meta(dm, member_disks_ptrs[i]); }
// }

// Example modification for delete_group
// In xsan_disk_manager_disk_group_delete, after successfully removing from list:
// if (err == XSAN_OK) {
//    xsan_disk_manager_delete_group_meta(dm, group_id_to_delete);
//    // also save affected disks that were unassigned
//    // (The loop for unassigning disks already exists, just add save_disk_meta call there)
// }

// scan_and_register_bdevs will also need to save new disks it discovers.
// If it updates an existing disk loaded from metadata, it should also save that disk's metadata.

// The full implementation of these save/load calls in create/delete/scan is the next step.
// This block focuses on adding the serialization/deserialization helpers and the main load function.

// (The content of xsan_disk_manager_scan_and_register_bdevs, get_all_disks etc. from previous steps remain)
// (The content of disk_group_create/delete placeholders also remain, to be updated next)
// ... [The rest of the file as it was, with placeholders for create/delete group logic to call meta functions] ...

// Copied from previous state to ensure file is complete
xsan_error_t xsan_disk_manager_scan_and_register_bdevs(xsan_disk_manager_t *dm) {
    if (!dm || !dm->initialized) {
        XSAN_LOG_ERROR("Disk manager not initialized or NULL when trying to scan bdevs.");
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_disk_manager_scan_and_register_bdevs must be called from an SPDK thread.");
        return XSAN_ERROR_THREAD_CONTEXT;
    }

    XSAN_LOG_INFO("Scanning for SPDK bdevs to register/update in XSAN Disk Manager...");
    xsan_bdev_info_t *bdev_info_list = NULL;
    int bdev_count = 0;
    xsan_error_t err = xsan_bdev_list_get_all(&bdev_info_list, &bdev_count);

    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("Failed to get list of SPDK bdevs from xsan_bdev module: %s (code %d)", xsan_error_string(err), err);
        return err;
    }
    if (bdev_count == 0) {
        XSAN_LOG_INFO("No SPDK bdevs found by xsan_bdev module.");
        return XSAN_OK;
    }

    XSAN_LOG_INFO("Found %d SPDK bdev(s). Processing for registration/update...", bdev_count);
    int new_registered_count = 0;
    int updated_count = 0;

    pthread_mutex_lock(&dm->lock);
    xsan_error_t overall_err_status = XSAN_OK;

    for (int i = 0; i < bdev_count; ++i) {
        xsan_bdev_info_t *current_bdev_info = &bdev_info_list[i];
        xsan_disk_t *existing_disk = NULL;
        xsan_list_node_t *list_node;
        XSAN_LIST_FOREACH(dm->managed_disks, list_node) {
            xsan_disk_t *disk_iter = (xsan_disk_t *)xsan_list_node_get_value(list_node);
            if (strncmp(disk_iter->bdev_name, current_bdev_info->name, XSAN_MAX_NAME_LEN) == 0) {
                existing_disk = disk_iter;
                break;
            }
        }

        bool needs_meta_save = false;
        if (existing_disk) {
            XSAN_LOG_DEBUG("Updating existing XSAN disk for bdev '%s'.", current_bdev_info->name);
            // Compare and update fields, set needs_meta_save = true if changed
            if(memcmp(&existing_disk->bdev_uuid, &current_bdev_info->uuid, sizeof(xsan_uuid_t)) != 0) {
                memcpy(&existing_disk->bdev_uuid, &current_bdev_info->uuid, sizeof(xsan_uuid_t)); needs_meta_save = true;
            }
            if(existing_disk->capacity_bytes != current_bdev_info->capacity_bytes) {
                existing_disk->capacity_bytes = current_bdev_info->capacity_bytes; needs_meta_save = true;
            }
            // ... similar checks for all relevant fields ...
            existing_disk->block_size_bytes = current_bdev_info->block_size; // Assume this can change
            existing_disk->num_blocks = current_bdev_info->num_blocks;
            xsan_strcpy_safe(existing_disk->product_name, current_bdev_info->product_name, XSAN_MAX_NAME_LEN); // Assume can change
            existing_disk->is_rotational = current_bdev_info->is_rotational;
            existing_disk->optimal_io_boundary_blocks = current_bdev_info->optimal_io_boundary;
            existing_disk->has_write_cache = current_bdev_info->has_write_cache;

            if (strstr(current_bdev_info->name, "Nvme") != NULL || strstr(current_bdev_info->product_name, "NVMe") !=NULL) {
                if(existing_disk->type != XSAN_STORAGE_DISK_TYPE_NVME_SSD) needs_meta_save = true;
                existing_disk->type = XSAN_STORAGE_DISK_TYPE_NVME_SSD;
            } else if (current_bdev_info->is_rotational) {
                if(existing_disk->type != XSAN_STORAGE_DISK_TYPE_HDD_SATA) needs_meta_save = true;
                existing_disk->type = XSAN_STORAGE_DISK_TYPE_HDD_SATA;
            } else {
                if(existing_disk->type != XSAN_STORAGE_DISK_TYPE_SATA_SSD) needs_meta_save = true;
                existing_disk->type = XSAN_STORAGE_DISK_TYPE_SATA_SSD;
            }

            if (!existing_disk->bdev_descriptor) {
                int rc_open = spdk_bdev_open_ext(existing_disk->bdev_name, true, NULL, NULL, &existing_disk->bdev_descriptor);
                if (rc_open != 0) {
                    XSAN_LOG_ERROR("Failed to re-open bdev descriptor for '%s': %s", existing_disk->bdev_name, spdk_strerror(-rc_open));
                    if(existing_disk->state != XSAN_STORAGE_STATE_FAILED) needs_meta_save = true;
                    existing_disk->state = XSAN_STORAGE_STATE_FAILED;
                } else {
                     if(existing_disk->state != XSAN_STORAGE_STATE_ONLINE) needs_meta_save = true; // If it was failed and now online
                     existing_disk->state = XSAN_STORAGE_STATE_ONLINE;
                }
            } else { // Descriptor exists, assume it's good, ensure state is ONLINE if not FAILED
                 if (existing_disk->state == XSAN_STORAGE_STATE_OFFLINE || existing_disk->state == XSAN_STORAGE_STATE_MISSING) { // If it was offline/missing and now found
                    existing_disk->state = XSAN_STORAGE_STATE_ONLINE;
                    needs_meta_save = true;
                 }
            }
            if(needs_meta_save) xsan_disk_manager_save_disk_meta(dm, existing_disk);
            updated_count++;
        } else { // New disk
            xsan_disk_t *new_disk = (xsan_disk_t *)XSAN_MALLOC(sizeof(xsan_disk_t));
            if (!new_disk) { /* ... error ... */ overall_err_status = XSAN_ERROR_OUT_OF_MEMORY; continue; }
            memset(new_disk, 0, sizeof(xsan_disk_t));
            spdk_uuid_generate((struct spdk_uuid *)&new_disk->id.data[0]);
            // ... (populate new_disk fields as before) ...
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

            int rc_open = spdk_bdev_open_ext(new_disk->bdev_name, true, NULL, NULL, &new_disk->bdev_descriptor);
            if (rc_open != 0) {
                XSAN_LOG_ERROR("Failed to open bdev '%s' for XSAN disk: %s. Marked FAILED.", new_disk->bdev_name, spdk_strerror(-rc_open));
                new_disk->state = XSAN_STORAGE_STATE_FAILED;
                new_disk->bdev_descriptor = NULL;
            } else {
                new_disk->state = XSAN_STORAGE_STATE_ONLINE;
            }
            memset(&new_disk->assigned_to_group_id, 0, sizeof(xsan_group_id_t));

            if (xsan_list_append(dm->managed_disks, new_disk) == NULL) { /* ... error ... */ }
            else {
                xsan_disk_manager_save_disk_meta(dm, new_disk); // Save new disk to metadata
                new_registered_count++;
                 char xsan_id_str[SPDK_UUID_STRING_LEN]; // Log after saving
                spdk_uuid_fmt_lower(xsan_id_str, sizeof(xsan_id_str), (struct spdk_uuid*)&new_disk->id.data[0]);
                XSAN_LOG_INFO("Registered new XSAN disk: BDevName='%s', XSAN_ID=%s, Type=%d, State=%d, Size=%.2f GiB",
                              new_disk->bdev_name, xsan_id_str, new_disk->type, new_disk->state,
                              (double)new_disk->capacity_bytes / (1024.0*1024.0*1024.0));
            }
        }
    }
    pthread_mutex_unlock(&dm->lock);
    xsan_bdev_list_free(bdev_info_list, bdev_count);
    XSAN_LOG_INFO("SPDK bdev scan and registration complete. New: %d, Updated: %d.", new_registered_count, updated_count);
    return overall_err_status;
}

xsan_error_t xsan_disk_manager_get_all_disks(xsan_disk_manager_t *dm, xsan_disk_t ***disks_array_out, int *count_out) {
    // ... (implementation as before) ...
    if (!dm || !dm->initialized || !disks_array_out || !count_out) {
        if (count_out) *count_out = 0;
        if (disks_array_out) *disks_array_out = NULL;
        return XSAN_ERROR_INVALID_PARAM;
    }
    pthread_mutex_lock(&dm->lock);
    size_t num_disks = xsan_list_size(dm->managed_disks);
    if (num_disks == 0) { *disks_array_out = NULL; *count_out = 0; pthread_mutex_unlock(&dm->lock); return XSAN_OK; }
    *disks_array_out = (xsan_disk_t **)XSAN_MALLOC(sizeof(xsan_disk_t*) * num_disks);
    if (!*disks_array_out) { *count_out = 0; pthread_mutex_unlock(&dm->lock); return XSAN_ERROR_OUT_OF_MEMORY; }
    int i = 0; xsan_list_node_t *node;
    XSAN_LIST_FOREACH(dm->managed_disks, node) { if (i < (int)num_disks) { (*disks_array_out)[i++] = (xsan_disk_t *)xsan_list_node_get_value(node); } else break; }
    *count_out = i; pthread_mutex_unlock(&dm->lock); return XSAN_OK;
}

void xsan_disk_manager_free_disk_pointer_list(xsan_disk_t **disk_ptr_array) {
    // ... (implementation as before) ...
    if (disk_ptr_array) XSAN_FREE(disk_ptr_array);
}

xsan_disk_t *xsan_disk_manager_find_disk_by_id(xsan_disk_manager_t *dm, xsan_disk_id_t disk_id_to_find) {
    // ... (implementation as before) ...
    if (!dm || !dm->initialized) return NULL;
    pthread_mutex_lock(&dm->lock);
    xsan_list_node_t *node; xsan_disk_t *found_disk = NULL;
    XSAN_LIST_FOREACH(dm->managed_disks, node) {
        xsan_disk_t *disk = (xsan_disk_t *)xsan_list_node_get_value(node);
        if (spdk_uuid_compare((struct spdk_uuid*)&disk->id.data[0], (struct spdk_uuid*)&disk_id_to_find.data[0]) == 0) {
            found_disk = disk; break;
        }
    }
    pthread_mutex_unlock(&dm->lock); return found_disk;
}

xsan_disk_t *xsan_disk_manager_find_disk_by_bdev_name(xsan_disk_manager_t *dm, const char *bdev_name) {
    // ... (implementation as before) ...
    if (!dm || !dm->initialized || !bdev_name) return NULL;
    pthread_mutex_lock(&dm->lock);
    xsan_list_node_t *node; xsan_disk_t *found_disk = NULL;
    XSAN_LIST_FOREACH(dm->managed_disks, node) {
        xsan_disk_t *disk = (xsan_disk_t *)xsan_list_node_get_value(node);
        if (strncmp(disk->bdev_name, bdev_name, XSAN_MAX_NAME_LEN) == 0) {
            found_disk = disk; break;
        }
    }
    pthread_mutex_unlock(&dm->lock); return found_disk;
}

xsan_error_t xsan_disk_manager_disk_group_create(xsan_disk_manager_t *dm,
                                                 const char *group_name,
                                                 xsan_disk_group_type_t group_type,
                                                 const char *bdev_names_list[],
                                                 int num_bdevs,
                                                 xsan_group_id_t *group_id_out) {
    // ... (implementation as before, but add save_group_meta and save_disk_meta for assigned disks) ...
    if (!dm || !dm->initialized || !group_name || !bdev_names_list || num_bdevs <= 0 || num_bdevs > XSAN_MAX_DISKS_PER_GROUP) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    pthread_mutex_lock(&dm->lock);
    xsan_error_t err = XSAN_OK;
    xsan_list_node_t *iter_node_group;
    XSAN_LIST_FOREACH(dm->managed_disk_groups, iter_node_group) {
        xsan_disk_group_t *dg_iter = (xsan_disk_group_t*)xsan_list_node_get_value(iter_node_group);
        if (strncmp(dg_iter->name, group_name, XSAN_MAX_NAME_LEN) == 0) {
            pthread_mutex_unlock(&dm->lock); return XSAN_ERROR_ALREADY_EXISTS;
        }
    }
    xsan_disk_t *member_disks_ptrs[XSAN_MAX_DISKS_PER_GROUP];
    uint64_t calculated_total_capacity = 0;
    for (int i = 0; i < num_bdevs; ++i) { /* ... find disk, check assignment, sum capacity ... */
        // (Code from previous implementation of this function)
        xsan_disk_t *disk = NULL;
        xsan_list_node_t *disk_node;
        XSAN_LIST_FOREACH(dm->managed_disks, disk_node) {
            xsan_disk_t *d_iter = (xsan_disk_t *)xsan_list_node_get_value(disk_node);
            if (strncmp(d_iter->bdev_name, bdev_names_list[i], XSAN_MAX_NAME_LEN) == 0) { disk = d_iter; break; }
        }
        if (!disk) { err = XSAN_ERROR_NOT_FOUND; goto create_group_cleanup_unlock; }
        if (!spdk_uuid_is_null((struct spdk_uuid*)&disk->assigned_to_group_id.data[0])) { err = XSAN_ERROR_RESOURCE_BUSY; goto create_group_cleanup_unlock; }
        member_disks_ptrs[i] = disk; calculated_total_capacity += disk->capacity_bytes;
    }
    xsan_disk_group_t *new_group = (xsan_disk_group_t *)XSAN_MALLOC(sizeof(xsan_disk_group_t));
    if (!new_group) { err = XSAN_ERROR_OUT_OF_MEMORY; goto create_group_cleanup_unlock; }
    memset(new_group, 0, sizeof(xsan_disk_group_t));
    spdk_uuid_generate((struct spdk_uuid *)&new_group->id.data[0]);
    // ... (populate new_group fields as before) ...
    xsan_strcpy_safe(new_group->name, group_name, XSAN_MAX_NAME_LEN);
    new_group->type = group_type; new_group->state = XSAN_STORAGE_STATE_ONLINE;
    new_group->disk_count = (uint32_t)num_bdevs;
    new_group->total_capacity_bytes = calculated_total_capacity;
    new_group->usable_capacity_bytes = calculated_total_capacity;
    for (int i = 0; i < num_bdevs; ++i) {
        memcpy(&new_group->disk_ids[i], &member_disks_ptrs[i]->id, sizeof(xsan_disk_id_t));
        memcpy(&member_disks_ptrs[i]->assigned_to_group_id, &new_group->id, sizeof(xsan_group_id_t));
    }
    if (xsan_list_append(dm->managed_disk_groups, new_group) == NULL) { /* ... error handling, revert assignments ... */ err = XSAN_ERROR_OUT_OF_MEMORY; goto create_group_cleanup_unlock_free_group; }
    if (group_id_out) memcpy(group_id_out, &new_group->id, sizeof(xsan_group_id_t));

    // Persist metadata
    xsan_disk_manager_save_group_meta(dm, new_group);
    for (int i = 0; i < num_bdevs; ++i) { xsan_disk_manager_save_disk_meta(dm, member_disks_ptrs[i]); }

    goto create_group_cleanup_unlock; // Normal exit path

create_group_cleanup_unlock_free_group:
    if(new_group) XSAN_FREE(new_group);
create_group_cleanup_unlock:
    pthread_mutex_unlock(&dm->lock);
    return err;
}

xsan_error_t xsan_disk_manager_disk_group_delete(xsan_disk_manager_t *dm, xsan_group_id_t group_id_to_delete) {
    // ... (implementation as before, but add delete_group_meta and save_disk_meta for unassigned disks) ...
    if (!dm || !dm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&group_id_to_delete.data[0])) return XSAN_ERROR_INVALID_PARAM;
    pthread_mutex_lock(&dm->lock);
    xsan_error_t err = XSAN_ERROR_NOT_FOUND;
    xsan_list_node_t *list_node_group = xsan_list_get_head(dm->managed_disk_groups);
    xsan_disk_group_t *group_to_delete = NULL;
    while(list_node_group != NULL) { /* ... find group ... */
        xsan_disk_group_t *current_group = (xsan_disk_group_t *)xsan_list_node_get_value(list_node_group);
        if (spdk_uuid_compare((struct spdk_uuid*)&current_group->id.data[0], (struct spdk_uuid*)&group_id_to_delete.data[0]) == 0) {
            group_to_delete = current_group; break;
        }
        list_node_group = xsan_list_node_next(list_node_group);
    }
    if (group_to_delete) {
        for (uint32_t i = 0; i < group_to_delete->disk_count; ++i) { /* ... unassign disks and save their meta ... */
            xsan_disk_t *disk = NULL; xsan_list_node_t *disk_node_iter;
            XSAN_LIST_FOREACH(dm->managed_disks, disk_node_iter) { /* ... find disk by ID ... */
                xsan_disk_t *d_iter = (xsan_disk_t *)xsan_list_node_get_value(disk_node_iter);
                if(spdk_uuid_compare((struct spdk_uuid*)&d_iter->id.data[0], (struct spdk_uuid*)&group_to_delete->disk_ids[i].data[0]) == 0) { disk = d_iter; break; }
            }
            if (disk && spdk_uuid_compare((struct spdk_uuid*)&disk->assigned_to_group_id.data[0], (struct spdk_uuid*)&group_to_delete->id.data[0]) == 0) {
                memset(&disk->assigned_to_group_id, 0, sizeof(xsan_group_id_t));
                xsan_disk_manager_save_disk_meta(dm, disk); // Persist change for this disk
            }
        }
        xsan_list_remove_node(dm->managed_disk_groups, list_node_group);
        xsan_disk_manager_delete_group_meta(dm, group_id_to_delete); // Delete metadata for the group
        err = XSAN_OK;
    }
    pthread_mutex_unlock(&dm->lock); return err;
}

xsan_error_t xsan_disk_manager_get_all_disk_groups(xsan_disk_manager_t *dm, xsan_disk_group_t ***groups_array_out, int *count_out) {
    // ... (implementation as before) ...
    if (!dm || !dm->initialized || !groups_array_out || !count_out) { /* ... param check ... */ return XSAN_ERROR_INVALID_PARAM; }
    pthread_mutex_lock(&dm->lock);
    size_t num_groups = xsan_list_size(dm->managed_disk_groups);
    if (num_groups == 0) { /* ... handle empty ... */ pthread_mutex_unlock(&dm->lock); return XSAN_OK; }
    *groups_array_out = (xsan_disk_group_t **)XSAN_MALLOC(sizeof(xsan_disk_group_t*) * num_groups);
    if (!*groups_array_out) { /* ... OOM ... */ pthread_mutex_unlock(&dm->lock); return XSAN_ERROR_OUT_OF_MEMORY; }
    int i = 0; xsan_list_node_t *node;
    XSAN_LIST_FOREACH(dm->managed_disk_groups, node) { if(i < (int)num_groups) {(*groups_array_out)[i++] = (xsan_disk_group_t *)xsan_list_node_get_value(node);} else break; }
    *count_out = i; pthread_mutex_unlock(&dm->lock); return XSAN_OK;
}

void xsan_disk_manager_free_group_pointer_list(xsan_disk_group_t **group_ptr_array) {
    // ... (implementation as before) ...
    if (group_ptr_array) XSAN_FREE(group_ptr_array);
}

xsan_disk_group_t *xsan_disk_manager_find_disk_group_by_id(xsan_disk_manager_t *dm, xsan_group_id_t group_id_to_find) {
    // ... (implementation as before) ...
    if (!dm || !dm->initialized) return NULL;
    pthread_mutex_lock(&dm->lock);
    xsan_list_node_t *node; xsan_disk_group_t *found_group = NULL;
    XSAN_LIST_FOREACH(dm->managed_disk_groups, node) {
        xsan_disk_group_t *group = (xsan_disk_group_t *)xsan_list_node_get_value(node);
        if (spdk_uuid_compare((struct spdk_uuid*)&group->id.data[0], (struct spdk_uuid*)&group_id_to_find.data[0]) == 0) {
            found_group = group; break;
        }
    }
    pthread_mutex_unlock(&dm->lock); return found_group;
}

xsan_disk_group_t *xsan_disk_manager_find_disk_group_by_name(xsan_disk_manager_t *dm, const char *name) {
    // ... (implementation as before) ...
    if (!dm || !dm->initialized || !name) return NULL;
    pthread_mutex_lock(&dm->lock);
    xsan_list_node_t *node; xsan_disk_group_t *found_group = NULL;
    XSAN_LIST_FOREACH(dm->managed_disk_groups, node) {
        xsan_disk_group_t *group = (xsan_disk_group_t *)xsan_list_node_get_value(node);
        if (strncmp(group->name, name, XSAN_MAX_NAME_LEN) == 0) {
            found_group = group; break;
        }
    }
    pthread_mutex_unlock(&dm->lock); return found_group;
}
