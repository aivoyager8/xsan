#include "xsan_disk_manager.h"
#include "xsan_bdev.h"       // For xsan_bdev_list_get_all, xsan_bdev_info_t
#include "xsan_memory.h"   // For XSAN_MALLOC, XSAN_FREE, XSAN_CALLOC
#include "xsan_list.h"     // Using our own list for managing disks and groups
#include "xsan_string_utils.h" // For xsan_strcpy_safe
#include "xsan_log.h"      // For XSAN_LOG_INFO, XSAN_LOG_ERROR, etc.
#include "xsan_metadata_store.h" // For RocksDB wrapper
#include "json-c/json.h"   // For JSON processing
#include "../../include/xsan_error.h"

#include "spdk/uuid.h"     // For spdk_uuid_generate, spdk_uuid_compare, spdk_uuid_is_null, spdk_uuid_fmt_lower
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
    else if (g_xsan_disk_manager_instance) dm_to_fini = g_xsan_disk_manager_t
