#include "xsan_volume_manager.h"
#include "xsan_disk_manager.h"
#include "xsan_storage.h"
#include "xsan_memory.h"
#include "xsan_list.h"
#include "xsan_string_utils.h"
#include "xsan_log.h"
#include "xsan_error.h"
#include "xsan_io.h"
#include "xsan_metadata_store.h"
#include "json-c/json.h"

#include "spdk/uuid.h"
#include <pthread.h>

// --- Defines for RocksDB Keys ---
#define XSAN_VOLUME_META_PREFIX "v:"

struct xsan_volume_manager {
    xsan_list_t *managed_volumes;
    xsan_disk_manager_t *disk_manager;
    pthread_mutex_t lock;
    bool initialized;
    xsan_metadata_store_t *md_store;
    char metadata_db_path[XSAN_MAX_PATH_LEN];
};

static xsan_volume_manager_t *g_xsan_volume_manager_instance = NULL;

// Forward declarations
static xsan_error_t xsan_volume_manager_load_metadata(xsan_volume_manager_t *vm);
static xsan_error_t xsan_volume_manager_save_volume_meta(xsan_volume_manager_t *vm, xsan_volume_t *vol);
static xsan_error_t xsan_volume_manager_delete_volume_meta(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id);
static xsan_error_t _xsan_volume_to_json_string(const xsan_volume_t *vol, char **json_string_out);
static xsan_error_t _xsan_json_string_to_volume(const char *json_string, xsan_volume_manager_t *vm, xsan_volume_t **vol_out);


static void _xsan_internal_volume_destroy_cb(void *volume_data) {
    if (volume_data) {
        xsan_volume_t *vol = (xsan_volume_t *)volume_data;
        char vol_id_str[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&vol->id.data[0]);
        XSAN_LOG_DEBUG("Volume Manager: Destroying xsan_volume (Name: '%s', ID: %s)", vol->name, vol_id_str);
        XSAN_FREE(vol);
    }
}

xsan_error_t xsan_volume_manager_init(xsan_disk_manager_t *disk_manager, xsan_volume_manager_t **vm_instance_out) {
    const char *default_db_path_suffix = "xsan_meta_db/volume_manager";
    char actual_db_path[XSAN_MAX_PATH_LEN];
    snprintf(actual_db_path, sizeof(actual_db_path), "./%s", default_db_path_suffix);

    if (g_xsan_volume_manager_instance != NULL) { XSAN_LOG_WARN("XSAN Volume Manager already initialized."); if (vm_instance_out) *vm_instance_out = g_xsan_volume_manager_instance; return XSAN_OK; }
    if (!disk_manager) { XSAN_LOG_ERROR("Disk Manager instance is required for Volume Manager."); if (vm_instance_out) *vm_instance_out = NULL; return XSAN_ERROR_INVALID_PARAM; }

    XSAN_LOG_INFO("Initializing XSAN Volume Manager (DB path: %s)...", actual_db_path);
    xsan_volume_manager_t *vm = (xsan_volume_manager_t *)XSAN_MALLOC(sizeof(xsan_volume_manager_t));
    if (!vm) { XSAN_LOG_ERROR("Failed to MALLOC Volume Manager."); if (vm_instance_out) *vm_instance_out = NULL; return XSAN_ERROR_OUT_OF_MEMORY; }
    memset(vm, 0, sizeof(xsan_volume_manager_t));
    xsan_strcpy_safe(vm->metadata_db_path, actual_db_path, XSAN_MAX_PATH_LEN);

    vm->managed_volumes = xsan_list_create(_xsan_internal_volume_destroy_cb);
    if (!vm->managed_volumes) { XSAN_LOG_ERROR("Failed to create list for volumes."); XSAN_FREE(vm); if (vm_instance_out) *vm_instance_out = NULL; return XSAN_ERROR_OUT_OF_MEMORY; }
    if (pthread_mutex_init(&vm->lock, NULL) != 0) { XSAN_LOG_ERROR("Mutex init failed for Volume Manager."); xsan_list_destroy(vm->managed_volumes); XSAN_FREE(vm); if (vm_instance_out) *vm_instance_out = NULL; return XSAN_ERROR_SYSTEM; }

    vm->disk_manager = disk_manager;
    vm->md_store = xsan_metadata_store_open(vm->metadata_db_path, true);
    if (!vm->md_store) { XSAN_LOG_ERROR("Failed to open metadata store for Volume Manager: %s", vm->metadata_db_path); /* cleanup */ pthread_mutex_destroy(&vm->lock); xsan_list_destroy(vm->managed_volumes); XSAN_FREE(vm); if (vm_instance_out) *vm_instance_out = NULL; return XSAN_ERROR_STORAGE_GENERIC;}

    vm->initialized = true;
    g_xsan_volume_manager_instance = vm;
    if (vm_instance_out) *vm_instance_out = g_xsan_volume_manager_instance;

    xsan_volume_manager_load_metadata(vm);
    XSAN_LOG_INFO("XSAN Volume Manager initialized successfully.");
    return XSAN_OK;
}

void xsan_volume_manager_fini(xsan_volume_manager_t **vm_ptr) {
    xsan_volume_manager_t *vm_to_fini = (vm_ptr && *vm_ptr) ? *vm_ptr : g_xsan_volume_manager_instance;
    if (!vm_to_fini || !vm_to_fini->initialized) { /* Log already finalized */ if (vm_ptr) *vm_ptr = NULL; g_xsan_volume_manager_instance = NULL; return; }
    XSAN_LOG_INFO("Finalizing XSAN Volume Manager...");
    pthread_mutex_lock(&vm_to_fini->lock);
    xsan_list_destroy(vm_to_fini->managed_volumes); vm_to_fini->managed_volumes = NULL;
    if (vm_to_fini->md_store) xsan_metadata_store_close(vm_to_fini->md_store); vm_to_fini->md_store = NULL;
    vm_to_fini->initialized = false;
    pthread_mutex_unlock(&vm_to_fini->lock);
    pthread_mutex_destroy(&vm_to_fini->lock);
    XSAN_FREE(vm_to_fini);
    if (vm_ptr && *vm_ptr) *vm_ptr = NULL;
    if (vm_to_fini == g_xsan_volume_manager_instance) g_xsan_volume_manager_instance = NULL;
    XSAN_LOG_INFO("XSAN Volume Manager finalized.");
}

static xsan_error_t _xsan_volume_to_json_string(const xsan_volume_t *vol, char **json_string_out) {
    if (!vol || !json_string_out) return XSAN_ERROR_INVALID_PARAM;
    json_object *jobj = json_object_new_object();
    char uuid_buf[SPDK_UUID_STRING_LEN];

    spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&vol->id.data[0]);
    json_object_object_add(jobj, "id", json_object_new_string(uuid_buf));
    json_object_object_add(jobj, "name", json_object_new_string(vol->name));
    json_object_object_add(jobj, "size_bytes", json_object_new_int64(vol->size_bytes));
    json_object_object_add(jobj, "block_size_bytes", json_object_new_int(vol->block_size_bytes));
    json_object_object_add(jobj, "num_blocks", json_object_new_int64(vol->num_blocks));
    json_object_object_add(jobj, "state", json_object_new_int(vol->state));
    spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&vol->source_group_id.data[0]);
    json_object_object_add(jobj, "source_group_id", json_object_new_string(uuid_buf));
    json_object_object_add(jobj, "thin_provisioned", json_object_new_boolean(vol->thin_provisioned));
    json_object_object_add(jobj, "allocated_bytes", json_object_new_int64(vol->allocated_bytes));
    json_object_object_add(jobj, "FTT", json_object_new_int(vol->FTT));
    json_object_object_add(jobj, "actual_replica_count", json_object_new_int(vol->actual_replica_count));

    json_object *jarray_replicas = json_object_new_array();
    for (uint32_t i = 0; i < vol->actual_replica_count && i < XSAN_MAX_REPLICAS; ++i) {
        json_object *jreplica = json_object_new_object();
        spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&vol->replica_nodes[i].node_id.data[0]);
        json_object_object_add(jreplica, "node_id", json_object_new_string(uuid_buf));
        json_object_object_add(jreplica, "node_ip_addr", json_object_new_string(vol->replica_nodes[i].node_ip_addr));
        json_object_object_add(jreplica, "node_comm_port", json_object_new_int(vol->replica_nodes[i].node_comm_port));
        json_object_array_add(jarray_replicas, jreplica);
    }
    json_object_object_add(jobj, "replica_nodes", jarray_replicas);

    const char *tmp_str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    *json_string_out = xsan_strdup(tmp_str);
    json_object_put(jobj);
    return (*json_string_out) ? XSAN_OK : XSAN_ERROR_OUT_OF_MEMORY;
}

static xsan_error_t _xsan_json_string_to_volume(const char *json_string, xsan_volume_manager_t *vm, xsan_volume_t **vol_out) {
    if (!json_string || !vol_out) return XSAN_ERROR_INVALID_PARAM; // vm might not be needed if not resolving dependencies here
    struct json_object *jobj = json_tokener_parse(json_string);
    if (!jobj || is_error(jobj)) { XSAN_LOG_ERROR("Failed to parse JSON for volume: %s", json_string); if(jobj && !is_error(jobj)) json_object_put(jobj); return XSAN_ERROR_CONFIG_PARSE; }

    xsan_volume_t *vol = (xsan_volume_t *)XSAN_MALLOC(sizeof(xsan_volume_t));
    if (!vol) { json_object_put(jobj); return XSAN_ERROR_OUT_OF_MEMORY; }
    memset(vol, 0, sizeof(xsan_volume_t));

    struct json_object *val;
    // ... (parse all basic fields as before) ...
    if (json_object_object_get_ex(jobj, "id", &val) && json_object_is_type(val, json_type_string)) spdk_uuid_parse((struct spdk_uuid*)&vol->id.data[0], json_object_get_string(val));
    if (json_object_object_get_ex(jobj, "name", &val) && json_object_is_type(val, json_type_string)) xsan_strcpy_safe(vol->name, json_object_get_string(val), XSAN_MAX_NAME_LEN);
    if (json_object_object_get_ex(jobj, "size_bytes", &val)) vol->size_bytes = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "block_size_bytes", &val)) vol->block_size_bytes = (uint32_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "num_blocks", &val)) vol->num_blocks = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "state", &val)) vol->state = (xsan_storage_state_t)json_object_get_int(val); else vol->state = XSAN_STORAGE_STATE_OFFLINE;
    if (json_object_object_get_ex(jobj, "source_group_id", &val) && json_object_is_type(val, json_type_string)) spdk_uuid_parse((struct spdk_uuid*)&vol->source_group_id.data[0], json_object_get_string(val));
    if (json_object_object_get_ex(jobj, "thin_provisioned", &val)) vol->thin_provisioned = json_object_get_boolean(val);
    if (json_object_object_get_ex(jobj, "allocated_bytes", &val)) vol->allocated_bytes = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "FTT", &val)) vol->FTT = (uint32_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "actual_replica_count", &val)) vol->actual_replica_count = (uint32_t)json_object_get_int(val);

    struct json_object *jarray_replicas;
    if (json_object_object_get_ex(jobj, "replica_nodes", &jarray_replicas) && json_object_is_type(jarray_replicas, json_type_array)) {
        int array_len = json_object_array_length(jarray_replicas);
        if ((uint32_t)array_len > XSAN_MAX_REPLICAS) array_len = XSAN_MAX_REPLICAS; // Cap at max
        if ((uint32_t)array_len != vol->actual_replica_count) {
            XSAN_LOG_WARN("Volume '%s' actual_replica_count (%u) mismatch with persisted replica_nodes array len (%d). Using array len.", vol->name, vol->actual_replica_count, array_len);
            vol->actual_replica_count = array_len;
        }
        for (int i = 0; i < array_len; ++i) {
            struct json_object *jreplica = json_object_array_get_idx(jarray_replicas, i);
            if (jreplica && json_object_is_type(jreplica, json_type_object)) {
                if (json_object_object_get_ex(jreplica, "node_id", &val) && json_object_is_type(val, json_type_string)) spdk_uuid_parse((struct spdk_uuid*)&vol->replica_nodes[i].node_id.data[0], json_object_get_string(val));
                if (json_object_object_get_ex(jreplica, "node_ip_addr", &val) && json_object_is_type(val, json_type_string)) xsan_strcpy_safe(vol->replica_nodes[i].node_ip_addr, json_object_get_string(val), INET6_ADDRSTRLEN);
                if (json_object_object_get_ex(jreplica, "node_comm_port", &val)) vol->replica_nodes[i].node_comm_port = (uint16_t)json_object_get_int(val);
            }
        }
    }

    json_object_put(jobj);
    *vol_out = vol;
    return XSAN_OK;
}

static xsan_error_t xsan_volume_manager_save_volume_meta(xsan_volume_manager_t *vm, xsan_volume_t *vol) {
    if (!vm || !vol || !vm->md_store) return XSAN_ERROR_INVALID_PARAM;
    char key_buf[XSAN_MAX_NAME_LEN + SPDK_UUID_STRING_LEN];
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&vol->id.data[0]);
    snprintf(key_buf, sizeof(key_buf), "%s%s", XSAN_VOLUME_META_PREFIX, vol_id_str);
    char *json_val_str = NULL;
    xsan_error_t err = _xsan_volume_to_json_string(vol, &json_val_str);
    if (err != XSAN_OK) return err;
    err = xsan_metadata_store_put(vm->md_store, key_buf, strlen(key_buf), json_val_str, strlen(json_val_str));
    XSAN_FREE(json_val_str);
    if (err != XSAN_OK) XSAN_LOG_ERROR("Failed to PUT volume (ID: %s) meta: %s", vol_id_str, xsan_error_string(err));
    else XSAN_LOG_DEBUG("Saved metadata for volume ID: %s", vol_id_str);
    return err;
}

static xsan_error_t xsan_volume_manager_delete_volume_meta(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id) {
    if (!vm || !vm->md_store) return XSAN_ERROR_INVALID_PARAM;
    char key_buf[XSAN_MAX_NAME_LEN + SPDK_UUID_STRING_LEN];
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&volume_id.data[0]);
    snprintf(key_buf, sizeof(key_buf), "%s%s", XSAN_VOLUME_META_PREFIX, vol_id_str);
    xsan_error_t err = xsan_metadata_store_delete(vm->md_store, key_buf, strlen(key_buf));
    if (err != XSAN_OK) XSAN_LOG_ERROR("Failed to DELETE volume (ID: %s) meta: %s", vol_id_str, xsan_error_string(err));
    else XSAN_LOG_DEBUG("Deleted metadata for volume ID: %s", vol_id_str);
    return err;
}

xsan_error_t xsan_volume_manager_load_metadata(xsan_volume_manager_t *vm) {
    if (!vm || !vm->initialized || !vm->md_store) return XSAN_ERROR_INVALID_PARAM;
    XSAN_LOG_INFO("Loading volume metadata from RocksDB store: %s", vm->metadata_db_path);
    pthread_mutex_lock(&vm->lock);
    xsan_metadata_iterator_t *iter = xsan_metadata_iterator_create(vm->md_store);
    if (!iter) { XSAN_LOG_ERROR("Failed to create metadata iterator for volumes."); pthread_mutex_unlock(&vm->lock); return XSAN_ERROR_STORAGE_GENERIC; }
    xsan_metadata_iterator_seek(iter, XSAN_VOLUME_META_PREFIX, strlen(XSAN_VOLUME_META_PREFIX));
    while (xsan_metadata_iterator_is_valid(iter)) {
        size_t key_len, val_len;
        const char *key = xsan_metadata_iterator_key(iter, &key_len);
        if (!key || strncmp(key, XSAN_VOLUME_META_PREFIX, strlen(XSAN_VOLUME_META_PREFIX)) != 0) break;
        const char *value_str = xsan_metadata_iterator_value(iter, &val_len);
        xsan_volume_t *vol = NULL;
        if (_xsan_json_string_to_volume(value_str, vm, &vol) == XSAN_OK && vol) {
            bool exists = false; xsan_list_node_t *ln;
            XSAN_LIST_FOREACH(vm->managed_volumes, ln) { if(spdk_uuid_compare((struct spdk_uuid*)&((xsan_volume_t*)xsan_list_node_get_value(ln))->id.data[0], (struct spdk_uuid*)&vol->id.data[0])==0){ exists=true; break; } }
            if(!exists) {
                xsan_list_append(vm->managed_volumes, vol);
                XSAN_LOG_DEBUG("Loaded volume '%s' (ID: %s) from metadata.", vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]));
            } else { XSAN_LOG_WARN("Volume ID %s from metadata already in memory.", spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0])); XSAN_FREE(vol); }
        } else { XSAN_LOG_ERROR("Failed to deserialize volume from: %.*s", (int)val_len, value_str); }
        xsan_metadata_iterator_next(iter);
    }
    xsan_metadata_iterator_destroy(iter);
    pthread_mutex_unlock(&vm->lock);
    XSAN_LOG_INFO("Metadata loading for Volume Manager complete.");
    return XSAN_OK;
}

xsan_error_t xsan_volume_create(xsan_volume_manager_t *vm,
                                const char *name,
                                uint64_t size_bytes,
                                xsan_group_id_t group_id,
                                uint32_t logical_block_size_bytes,
                                bool thin_provisioned,
                                uint32_t ftt, // New FTT parameter
                                xsan_volume_id_t *new_volume_id_out) {
    if (!vm || !vm->initialized || !name || size_bytes == 0 || logical_block_size_bytes == 0 ||
        (logical_block_size_bytes & (logical_block_size_bytes - 1)) != 0 ||
        spdk_uuid_is_null((struct spdk_uuid*)&group_id.data[0]) ||
        (ftt + 1) > XSAN_MAX_REPLICAS ) {
        XSAN_LOG_ERROR("Invalid parameters for volume create. FTT %u, MAX_REPLICAS %d", ftt, XSAN_MAX_REPLICAS);
        return XSAN_ERROR_INVALID_PARAM;
    }
    if ((size_bytes % logical_block_size_bytes) != 0) {
        XSAN_LOG_ERROR("Volume size %lu B is not a multiple of logical block size %u B.", size_bytes, logical_block_size_bytes);
        return XSAN_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&vm->lock);
    xsan_error_t err = XSAN_OK;

    xsan_list_node_t *iter_node;
    XSAN_LIST_FOREACH(vm->managed_volumes, iter_node) {
        xsan_volume_t *vol_iter = (xsan_volume_t*)xsan_list_node_get_value(iter_node);
        if (strncmp(vol_iter->name, name, XSAN_MAX_NAME_LEN) == 0) {
             XSAN_LOG_WARN("Volume with name '%s' already exists.", name);
             pthread_mutex_unlock(&vm->lock);
             return XSAN_ERROR_ALREADY_EXISTS;
        }
    }

    xsan_disk_group_t *group = xsan_disk_manager_find_disk_group_by_id(vm->disk_manager, group_id);
    if (!group) { XSAN_LOG_ERROR("Disk group for volume '%s' not found.", name); err = XSAN_ERROR_NOT_FOUND; goto create_cleanup_unlock; }
    if (group->state != XSAN_STORAGE_STATE_ONLINE) { XSAN_LOG_ERROR("Disk group '%s' not online for vol '%s'.", group->name, name); err = XSAN_ERROR_RESOURCE_BUSY; goto create_cleanup_unlock; }
    if (!thin_provisioned && size_bytes > group->usable_capacity_bytes) { XSAN_LOG_ERROR("Insufficient space in group '%s' for vol '%s'.", group->name, name); err = XSAN_ERROR_INSUFFICIENT_SPACE; goto create_cleanup_unlock; }

    xsan_volume_t *new_volume = (xsan_volume_t *)XSAN_MALLOC(sizeof(xsan_volume_t));
    if (!new_volume) { err = XSAN_ERROR_OUT_OF_MEMORY; goto create_cleanup_unlock; }
    memset(new_volume, 0, sizeof(xsan_volume_t));

    spdk_uuid_generate((struct spdk_uuid *)&new_volume->id.data[0]);
    xsan_strcpy_safe(new_volume->name, name, XSAN_MAX_NAME_LEN);
    new_volume->size_bytes = size_bytes; new_volume->block_size_bytes = logical_block_size_bytes;
    new_volume->num_blocks = size_bytes / logical_block_size_bytes;
    new_volume->state = XSAN_STORAGE_STATE_ONLINE;
    memcpy(&new_volume->source_group_id, &group_id, sizeof(xsan_group_id_t));
    new_volume->thin_provisioned = thin_provisioned;
    new_volume->allocated_bytes = thin_provisioned ? 0 : size_bytes;
    new_volume->FTT = ftt;
    new_volume->actual_replica_count = 0;

    // Simplified Replica Node Assignment
    // TODO: Replace with actual cluster node discovery and placement logic.
    // For now, replica_nodes[0] is local (placeholder).
    if (XSAN_MAX_REPLICAS > 0) { // Ensure array bounds
        // Placeholder for self/local node_id and comm info
        spdk_uuid_generate((struct spdk_uuid*)&new_volume->replica_nodes[0].node_id.data[0]); // Should be this node's actual ID
        xsan_strcpy_safe(new_volume->replica_nodes[0].node_ip_addr, "127.0.0.1", INET6_ADDRSTRLEN); // This node's listen IP
        new_volume->replica_nodes[0].node_comm_port = 7777; // This node's listen port
        new_volume->actual_replica_count = 1;

        XSAN_LOG_DEBUG("Volume '%s': Local replica [0] assigned to self (NodeID placeholder, Addr: %s:%u)",
                       new_volume->name, new_volume->replica_nodes[0].node_ip_addr, new_volume->replica_nodes[0].node_comm_port);

        uint32_t target_total_replicas = ftt + 1;
        if (target_total_replicas > XSAN_MAX_REPLICAS) target_total_replicas = XSAN_MAX_REPLICAS;
        for (uint32_t i = 1; i < target_total_replicas; ++i) {
            XSAN_LOG_WARN("Volume '%s': Remote replica %u assignment is placeholder. Real node selection needed.", new_volume->name, i);
            // In a real system, you'd pick other nodes from a cluster list.
            // For now, leaving them zeroed or with dummy data.
            // If actual_replica_count does not reach ftt+1, it indicates partial configuration.
        }
        if (new_volume->actual_replica_count < (ftt + 1) && ftt > 0) {
             XSAN_LOG_WARN("Volume '%s': Configured with %u replicas, but FTT %u requires %u. Not enough remote nodes selected (placeholder).",
                      new_volume->name, new_volume->actual_replica_count, ftt, ftt + 1);
        }
    }


    if (xsan_list_append(vm->managed_volumes, new_volume) == NULL) { XSAN_LOG_ERROR("Failed to append new volume '%s'.", new_volume->name); XSAN_FREE(new_volume); err = XSAN_ERROR_OUT_OF_MEMORY; goto create_cleanup_unlock; }
    if (new_volume_id_out) memcpy(new_volume_id_out, &new_volume->id, sizeof(xsan_volume_id_t));

    XSAN_LOG_INFO("Volume '%s' (ID: %s) created in memory. FTT=%u, ActualReplicas=%u", new_volume->name, spdk_uuid_get_string((struct spdk_uuid*)&new_volume->id.data[0]), new_volume->FTT, new_volume->actual_replica_count);
    err = xsan_volume_manager_save_volume_meta(vm, new_volume);
    if(err != XSAN_OK) { XSAN_LOG_ERROR("Failed to save metadata for new volume '%s'.", new_volume->name); }

create_cleanup_unlock:
    pthread_mutex_unlock(&vm->lock);
    return err;
}

xsan_error_t xsan_volume_delete(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id) {
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) return XSAN_ERROR_INVALID_PARAM;
    pthread_mutex_lock(&vm->lock);
    xsan_error_t err = XSAN_ERROR_NOT_FOUND;
    xsan_list_node_t *list_node = xsan_list_get_head(vm->managed_volumes);
    xsan_volume_t *volume_to_delete = NULL;
    while(list_node != NULL) {
        xsan_volume_t *current_volume = (xsan_volume_t *)xsan_list_node_get_value(list_node);
        if (spdk_uuid_compare((struct spdk_uuid*)&current_volume->id.data[0], (struct spdk_uuid*)&volume_id.data[0]) == 0) {
            volume_to_delete = current_volume; break;
        }
        list_node = xsan_list_node_next(list_node);
    }
    if (volume_to_delete) {
        xsan_list_remove_node(vm->managed_volumes, list_node);
        err = xsan_volume_manager_delete_volume_meta(vm, volume_id);
        if (err != XSAN_OK) { XSAN_LOG_ERROR("Volume (ID: %s) deleted from memory, but failed to delete metadata.", spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));}
        else { XSAN_LOG_INFO("Volume (ID: %s) and its metadata deleted successfully.", spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));}
        err = XSAN_OK;
    } else { XSAN_LOG_WARN("Volume (ID: %s) not found for deletion.", spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0])); }
    pthread_mutex_unlock(&vm->lock);
    return err;
}

xsan_volume_t *xsan_volume_get_by_id(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id) { /* ... as before ... */
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) return NULL;
    pthread_mutex_lock(&vm->lock);
    xsan_list_node_t *node; xsan_volume_t *found_volume = NULL;
    XSAN_LIST_FOREACH(vm->managed_volumes, node) {
        xsan_volume_t *vol = (xsan_volume_t *)xsan_list_node_get_value(node);
        if (spdk_uuid_compare((struct spdk_uuid*)&vol->id.data[0], (struct spdk_uuid*)&volume_id.data[0]) == 0) {
            found_volume = vol; break;
        }
    }
    pthread_mutex_unlock(&vm->lock); return found_volume;
}
xsan_volume_t *xsan_volume_get_by_name(xsan_volume_manager_t *vm, const char *name) { /* ... as before ... */
    if (!vm || !vm->initialized || !name) return NULL;
    pthread_mutex_lock(&vm->lock);
    xsan_list_node_t *node; xsan_volume_t *found_volume = NULL;
    XSAN_LIST_FOREACH(vm->managed_volumes, node) {
        xsan_volume_t *vol = (xsan_volume_t *)xsan_list_node_get_value(node);
        if (strncmp(vol->name, name, XSAN_MAX_NAME_LEN) == 0) {
            found_volume = vol; break;
        }
    }
    pthread_mutex_unlock(&vm->lock); return found_volume;
}
xsan_error_t xsan_volume_list_all(xsan_volume_manager_t *vm, xsan_volume_t ***volumes_array_out, int *count_out) { /* ... as before ... */
    if (!vm || !vm->initialized || !volumes_array_out || !count_out) { if(count_out) *count_out = 0; if(volumes_array_out) *volumes_array_out = NULL; return XSAN_ERROR_INVALID_PARAM; }
    pthread_mutex_lock(&vm->lock);
    size_t num_volumes = xsan_list_size(vm->managed_volumes);
    if (num_volumes == 0) { *volumes_array_out = NULL; *count_out = 0; pthread_mutex_unlock(&vm->lock); return XSAN_OK; }
    *volumes_array_out = (xsan_volume_t **)XSAN_MALLOC(sizeof(xsan_volume_t*) * num_volumes);
    if (!*volumes_array_out) { *count_out = 0; pthread_mutex_unlock(&vm->lock); return XSAN_ERROR_OUT_OF_MEMORY; }
    int i = 0; xsan_list_node_t *node;
    XSAN_LIST_FOREACH(vm->managed_volumes, node) { if (i < (int)num_volumes) { (*volumes_array_out)[i++] = (xsan_volume_t *)xsan_list_node_get_value(node); } else break; }
    *count_out = i; pthread_mutex_unlock(&vm->lock); return XSAN_OK;
}
void xsan_volume_manager_free_volume_pointer_list(xsan_volume_t **volume_ptr_array) { /* ... as before ... */
    if (volume_ptr_array) XSAN_FREE(volume_ptr_array);
}

xsan_error_t xsan_volume_map_lba_to_physical(xsan_volume_manager_t *vm,
                                             xsan_volume_id_t volume_id,
                                             uint64_t logical_block_idx,
                                             xsan_disk_id_t *out_disk_id,
                                             uint64_t *out_physical_block_idx,
                                             uint32_t *out_physical_block_size) {
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])
        || !out_disk_id || !out_physical_block_idx || !out_physical_block_size) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    xsan_volume_t *vol = xsan_volume_get_by_id(vm, volume_id);
    if (!vol) return XSAN_ERROR_NOT_FOUND;
    if (logical_block_idx >= vol->num_blocks) return XSAN_ERROR_OUT_OF_BOUNDS;
    xsan_disk_group_t *group = xsan_disk_manager_find_disk_group_by_id(vm->disk_manager, vol->source_group_id);
    if (!group) { return XSAN_ERROR_NOT_FOUND; }
    if (group->disk_count == 0) { return XSAN_ERROR_STORAGE_GENERIC; }
    xsan_disk_id_t target_disk_id_from_group;
    memcpy(&target_disk_id_from_group, &group->disk_ids[0], sizeof(xsan_disk_id_t)); // Simple: first disk
    xsan_disk_t *target_disk = xsan_disk_manager_find_disk_by_id(vm->disk_manager, target_disk_id_from_group);
    if (!target_disk) { return XSAN_ERROR_NOT_FOUND; }
    if (target_disk->block_size_bytes == 0) { return XSAN_ERROR_STORAGE_GENERIC; }
    *out_physical_block_size = target_disk->block_size_bytes;
    *out_physical_block_idx = (logical_block_idx * vol->block_size_bytes) / target_disk->block_size_bytes;
    uint64_t num_phys_blks_for_log_blk = (vol->block_size_bytes + target_disk->block_size_bytes - 1) / target_disk->block_size_bytes;
    if ((*out_physical_block_idx + num_phys_blks_for_log_blk -1) >= target_disk->num_blocks) { return XSAN_ERROR_OUT_OF_BOUNDS; }
    memcpy(out_disk_id, &target_disk->id, sizeof(xsan_disk_id_t));
    return XSAN_OK;
}

static xsan_error_t _xsan_volume_submit_async_io(
    xsan_volume_manager_t *vm, xsan_volume_id_t volume_id, uint64_t logical_byte_offset,
    uint64_t length_bytes, void *user_buf, bool is_read_op,
    xsan_user_io_completion_cb_t user_cb, void *user_cb_arg) {
    // ... (Implementation as before) ...
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0]) || !user_buf || length_bytes == 0 || !user_cb) return XSAN_ERROR_INVALID_PARAM;
    xsan_volume_t *vol = xsan_volume_get_by_id(vm, volume_id);
    if (!vol) return XSAN_ERROR_NOT_FOUND;
    if (vol->block_size_bytes == 0 || (logical_byte_offset % vol->block_size_bytes != 0) || (length_bytes % vol->block_size_bytes != 0)) return XSAN_ERROR_INVALID_PARAM;
    uint64_t logical_start_block_idx = logical_byte_offset / vol->block_size_bytes;
    uint64_t num_logical_blocks_in_io = length_bytes / vol->block_size_bytes;
    if ((logical_start_block_idx + num_logical_blocks_in_io) > vol->num_blocks) return XSAN_ERROR_OUT_OF_BOUNDS;
    xsan_disk_id_t physical_disk_id; uint64_t physical_start_block_idx_on_disk; uint32_t physical_bdev_block_size;
    xsan_error_t map_err = xsan_volume_map_lba_to_physical(vm, volume_id, logical_start_block_idx, &physical_disk_id, &physical_start_block_idx_on_disk, &physical_bdev_block_size);
    if (map_err != XSAN_OK) return map_err;
    xsan_disk_t *target_disk = xsan_disk_manager_find_disk_by_id(vm->disk_manager, physical_disk_id);
    if (!target_disk) return XSAN_ERROR_NOT_FOUND;
    if (length_bytes % physical_bdev_block_size != 0) return XSAN_ERROR_INVALID_PARAM;
    xsan_io_request_t *io_req = xsan_io_request_create(volume_id, user_buf, physical_start_block_idx_on_disk * physical_bdev_block_size, length_bytes, physical_bdev_block_size, is_read_op, user_cb, user_cb_arg);
    if (!io_req) return XSAN_ERROR_OUT_OF_MEMORY;
    memcpy(&io_req->target_disk_id, &target_disk->id, sizeof(xsan_disk_id_t));
    xsan_strcpy_safe(io_req->target_bdev_name, target_disk->bdev_name, XSAN_MAX_NAME_LEN);
    io_req->bdev_desc = target_disk->bdev_descriptor; // Pass the opened descriptor
    xsan_error_t submit_err = xsan_io_submit_request_to_bdev(io_req);
    if (submit_err != XSAN_OK) return submit_err;
    return XSAN_OK;
}
xsan_error_t xsan_volume_read_async(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id, uint64_t logical_byte_offset, uint64_t length_bytes, void *user_buf, xsan_user_io_completion_cb_t user_cb, void *user_cb_arg) {
    return _xsan_volume_submit_async_io(vm, volume_id, logical_byte_offset, length_bytes, user_buf, true, user_cb, user_cb_arg);
}
xsan_error_t xsan_volume_write_async(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id, uint64_t logical_byte_offset, uint64_t length_bytes, const void *user_buf, xsan_user_io_completion_cb_t user_cb, void *user_cb_arg) {
    return _xsan_volume_submit_async_io(vm, volume_id, logical_byte_offset, length_bytes, (void*)user_buf, false, user_cb, user_cb_arg);
}
