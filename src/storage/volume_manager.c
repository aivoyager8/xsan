#include "xsan_volume_manager.h"
#include "xsan_disk_manager.h"
#include "xsan_storage.h"
#include "xsan_memory.h"
#include "xsan_list.h"
#include "xsan_string_utils.h"
#include "xsan_log.h"
#include "xsan_error.h"
#include "xsan_io.h"       // For xsan_io_request_t and xsan_io_submit_request_to_bdev
#include "xsan_replication.h" // For xsan_replicated_io_ctx_t, xsan_replica_read_coordinator_ctx_t, xsan_per_replica_op_ctx_t
#include "xsan_node_comm.h"
#include "xsan_protocol.h"
#include "xsan_metadata_store.h"
#include "xsan_bdev.h" // For xsan_bdev_get_buf_align, xsan_bdev_dma_malloc
#include "xsan_cluster.h" // For xsan_get_local_node_info
#include "json-c/json.h"

#include "spdk/uuid.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/util.h" // For spdk_get_ticks, spdk_get_ticks_hz
#include <pthread.h>
#include <errno.h>

#define XSAN_VOLUME_META_PREFIX "v:"
#define XSAN_DEFAULT_COMM_PORT 8080 // Placeholder, should come from config

struct xsan_volume_manager {
    xsan_list_t *managed_volumes;
    xsan_disk_manager_t *disk_manager;
    pthread_mutex_t lock;
    bool initialized;
    xsan_metadata_store_t *md_store;
    char metadata_db_path[XSAN_MAX_PATH_LEN];
    xsan_hashtable_t *pending_replicated_ios;
    xsan_hashtable_t *pending_replica_reads;
    pthread_mutex_t pending_ios_lock;
};

static xsan_volume_manager_t *g_xsan_volume_manager_instance = NULL;

// Context for the _xsan_physical_io_complete_cb callback
typedef struct {
    xsan_io_request_t *io_req;          // The xsan_io_request sent to xsan_io_submit_request_to_bdev
    xsan_user_io_completion_cb_t actual_upper_cb; // e.g., _xsan_local_replica_write_complete_cb
    void *actual_upper_cb_arg;        // e.g., rep_ctx
    void* original_user_buffer;       // The very original buffer from the caller of read_async/write_async
    bool is_read_op;
    uint64_t length_bytes;
    xsan_volume_id_t volume_id_for_log; // Store the volume ID for logging/debugging
} xsan_vm_physical_io_ctx_t;


// Forward declarations
static xsan_error_t xsan_volume_manager_load_metadata(xsan_volume_manager_t *vm);
static xsan_error_t xsan_volume_manager_save_volume_meta(xsan_volume_manager_t *vm, xsan_volume_t *vol);
static xsan_error_t xsan_volume_manager_delete_volume_meta(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id);
static xsan_error_t _xsan_volume_to_json_string(const xsan_volume_t *vol, char **json_string_out);
static xsan_error_t _xsan_json_string_to_volume(const char *json_string, xsan_volume_manager_t *vm, xsan_volume_t **vol_out);
static xsan_error_t _xsan_volume_submit_single_io_attempt(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id, uint64_t logical_byte_offset, uint64_t length_bytes, void *original_user_buffer, bool is_read_op, xsan_user_io_completion_cb_t upper_completion_cb, void *upper_completion_cb_arg);

static void _xsan_check_replicated_write_completion(xsan_replicated_io_ctx_t *rep_ctx);
static void _xsan_local_replica_write_complete_cb(void *cb_arg, xsan_error_t status);
static void _xsan_remote_replica_connect_then_send_cb(struct spdk_sock *sock, int status, void *cb_arg);
static void _xsan_remote_replica_request_send_actual_cb(int comm_status, void *cb_arg);

static void _xsan_try_read_from_next_replica(xsan_replica_read_coordinator_ctx_t *coord_ctx);
static void _xsan_replica_read_attempt_complete_cb(void *cb_arg, xsan_error_t status);
static void _xsan_remote_replica_read_req_send_complete_cb(int comm_status, void *cb_arg);
static void _xsan_remote_replica_read_connect_then_send_cb(struct spdk_sock *sock, int status, void *cb_arg);
static void _xsan_volume_update_overall_state(xsan_volume_manager_t *vm, xsan_volume_t *vol);
static void _xsan_physical_io_complete_cb(void *cb_arg_from_io_layer, xsan_error_t status);


static uint64_t _get_current_time_us() {
    return spdk_get_ticks() * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
}

static void _xsan_internal_volume_destroy_cb(void *volume_data) {
    if (volume_data) { xsan_volume_t *v = (xsan_volume_t *)volume_data; XSAN_FREE(v); }
}
static uint32_t uint64_tid_hash_func(const void *key) { if(!key)return 0;uint64_t v=*(const uint64_t*)key;v=(~v)+(v<<21);v=v^(v>>24);v=(v+(v<<3))+(v<<8);v=v^(v>>14);v=(v+(v<<2))+(v<<4);v=v^(v>>28);v=v+(v<<31);return (uint32_t)v;}
static int uint64_tid_key_compare_func(const void *k1,const void *k2){ if(k1==k2)return 0;if(!k1)return-1;if(!k2)return 1;uint64_t v1=*(const uint64_t*)k1;uint64_t v2=*(const uint64_t*)k2;if(v1<v2)return-1;if(v1>v2)return 1;return 0;}


xsan_error_t xsan_volume_manager_init(xsan_disk_manager_t *dm, xsan_volume_manager_t **vm_out){
    const char *default_db_path_suffix = "xsan_meta_db/volume_manager";
    char actual_db_path[XSAN_MAX_PATH_LEN];
    snprintf(actual_db_path, sizeof(actual_db_path), "./%s", default_db_path_suffix);
    if (g_xsan_volume_manager_instance) { if(vm_out)*vm_out=g_xsan_volume_manager_instance; return XSAN_OK; }
    if (!dm) { if(vm_out)*vm_out=NULL; return XSAN_ERROR_INVALID_PARAM; }
    XSAN_LOG_INFO("Initializing Volume Manager (DB: %s)...", actual_db_path);
    xsan_volume_manager_t *vm = (xsan_volume_manager_t *)XSAN_MALLOC(sizeof(*vm));
    if (!vm) { if(vm_out)*vm_out=NULL;return XSAN_ERROR_OUT_OF_MEMORY; }
    memset(vm,0,sizeof(*vm)); xsan_strcpy_safe(vm->metadata_db_path,actual_db_path,XSAN_MAX_PATH_LEN);
    vm->managed_volumes=xsan_list_create(_xsan_internal_volume_destroy_cb);
    if(!vm->managed_volumes || pthread_mutex_init(&vm->lock,NULL)!=0 || pthread_mutex_init(&vm->pending_ios_lock,NULL)!=0){ XSAN_FREE(vm); return XSAN_ERROR_SYSTEM;}
    vm->pending_replicated_ios=xsan_hashtable_create(256,uint64_tid_hash_func,uint64_tid_key_compare_func,NULL, (void(*)(void*))xsan_replicated_io_ctx_free); // Added free func
    if(!vm->pending_replicated_ios){ pthread_mutex_destroy(&vm->lock); pthread_mutex_destroy(&vm->pending_ios_lock); xsan_list_destroy(vm->managed_volumes); XSAN_FREE(vm); return XSAN_ERROR_OUT_OF_MEMORY;}
    vm->pending_replica_reads = xsan_hashtable_create(256, uint64_tid_hash_func, uint64_tid_key_compare_func, NULL, (void(*)(void*))xsan_replica_read_coordinator_ctx_free); // Added free func
    if(!vm->pending_replica_reads){ xsan_hashtable_destroy(vm->pending_replicated_ios); pthread_mutex_destroy(&vm->lock); pthread_mutex_destroy(&vm->pending_ios_lock); xsan_list_destroy(vm->managed_volumes); XSAN_FREE(vm); return XSAN_ERROR_OUT_OF_MEMORY;}
    vm->disk_manager=dm; vm->md_store=xsan_metadata_store_open(vm->metadata_db_path,true);
    if(!vm->md_store){ xsan_hashtable_destroy(vm->pending_replica_reads); xsan_hashtable_destroy(vm->pending_replicated_ios); pthread_mutex_destroy(&vm->lock); pthread_mutex_destroy(&vm->pending_ios_lock); xsan_list_destroy(vm->managed_volumes); XSAN_FREE(vm); return XSAN_ERROR_STORAGE_GENERIC;}
    vm->initialized=true; g_xsan_volume_manager_instance=vm; if(vm_out)*vm_out=vm;
    xsan_volume_manager_load_metadata(vm); XSAN_LOG_INFO("Volume Manager initialized."); return XSAN_OK;
}

void xsan_volume_manager_fini(xsan_volume_manager_t **vm_ptr){
    xsan_volume_manager_t *vm = (vm_ptr && *vm_ptr) ? *vm_ptr : g_xsan_volume_manager_instance;
    if (!vm || !vm->initialized) { if(vm_ptr) *vm_ptr = NULL; if(vm==g_xsan_volume_manager_instance)g_xsan_volume_manager_instance=NULL; return; }
    XSAN_LOG_INFO("Finalizing Volume Manager...");
    pthread_mutex_lock(&vm->pending_ios_lock);
    if(vm->pending_replicated_ios){ xsan_hashtable_destroy(vm->pending_replicated_ios);vm->pending_replicated_ios=NULL;} // Free func in create handles elements
    if(vm->pending_replica_reads){ xsan_hashtable_destroy(vm->pending_replica_reads);vm->pending_replica_reads=NULL;} // Free func in create handles elements
    pthread_mutex_unlock(&vm->pending_ios_lock); pthread_mutex_destroy(&vm->pending_ios_lock);
    pthread_mutex_lock(&vm->lock); xsan_list_destroy(vm->managed_volumes); vm->managed_volumes = NULL; if(vm->md_store)xsan_metadata_store_close(vm->md_store); vm->md_store = NULL; pthread_mutex_unlock(&vm->lock); pthread_mutex_destroy(&vm->lock);
    XSAN_FREE(vm); if(vm_ptr)*vm_ptr=NULL; if(vm==g_xsan_volume_manager_instance)g_xsan_volume_manager_instance=NULL;
    XSAN_LOG_INFO("Volume Manager finalized.");
}

static xsan_error_t _xsan_volume_to_json_string(const xsan_volume_t *vol, char **json_s_out){
    if (!vol || !json_s_out) return XSAN_ERROR_INVALID_PARAM;
    json_object *jobj = json_object_new_object(); char uuid_buf[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&vol->id.data[0]); json_object_object_add(jobj, "id", json_object_new_string(uuid_buf));
    json_object_object_add(jobj, "name", json_object_new_string(vol->name));
    json_object_object_add(jobj, "size_bytes", json_object_new_int64(vol->size_bytes));
    json_object_object_add(jobj, "block_size_bytes", json_object_new_int(vol->block_size_bytes));
    json_object_object_add(jobj, "num_blocks", json_object_new_int64(vol->num_blocks));
    json_object_object_add(jobj, "state", json_object_new_int(vol->state));
    spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&vol->source_group_id.data[0]); json_object_object_add(jobj, "source_group_id", json_object_new_string(uuid_buf));
    json_object_object_add(jobj, "thin_provisioned", json_object_new_boolean(vol->thin_provisioned));
    json_object_object_add(jobj, "allocated_bytes", json_object_new_int64(vol->allocated_bytes));
    json_object_object_add(jobj, "FTT", json_object_new_int(vol->FTT));
    json_object_object_add(jobj, "actual_replica_count", json_object_new_int(vol->actual_replica_count));
    json_object *jarray_replicas = json_object_new_array();
    for (uint32_t i = 0; i < vol->actual_replica_count && i < XSAN_MAX_REPLICAS; ++i) {
        json_object *jreplica = json_object_new_object();
        spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&vol->replica_nodes[i].node_id.data[0]); json_object_object_add(jreplica, "node_id", json_object_new_string(uuid_buf));
        json_object_object_add(jreplica, "node_ip_addr", json_object_new_string(vol->replica_nodes[i].node_ip_addr));
        json_object_object_add(jreplica, "node_comm_port", json_object_new_int(vol->replica_nodes[i].node_comm_port));
        json_object_object_add(jreplica, "state", json_object_new_int(vol->replica_nodes[i].state));
        json_object_object_add(jreplica, "last_successful_contact_time_us", json_object_new_int64(vol->replica_nodes[i].last_successful_contact_time_us));
        json_object_array_add(jarray_replicas, jreplica);
    }
    json_object_object_add(jobj, "replica_nodes", jarray_replicas);
    const char *tmp_str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    *json_s_out = xsan_strdup(tmp_str); json_object_put(jobj);
    return (*json_s_out) ? XSAN_OK : XSAN_ERROR_OUT_OF_MEMORY;
}

static xsan_error_t _xsan_json_string_to_volume(const char *js, xsan_volume_manager_t *vm, xsan_volume_t **v_out){
    (void)vm; if (!js || !v_out) return XSAN_ERROR_INVALID_PARAM;
    struct json_object *jobj = json_tokener_parse(js); if (!jobj || is_error(jobj)) { if(jobj&&!is_error(jobj))json_object_put(jobj);return XSAN_ERROR_CONFIG_PARSE; }
    xsan_volume_t *vol = (xsan_volume_t *)XSAN_MALLOC(sizeof(*vol)); if (!vol) { json_object_put(jobj); return XSAN_ERROR_OUT_OF_MEMORY; }
    memset(vol, 0, sizeof(*vol)); struct json_object *val;
    if (json_object_object_get_ex(jobj, "id", &val)) spdk_uuid_parse((struct spdk_uuid*)&vol->id.data[0], json_object_get_string(val));
    if (json_object_object_get_ex(jobj, "name", &val)) xsan_strcpy_safe(vol->name, json_object_get_string(val), XSAN_MAX_NAME_LEN);
    if (json_object_object_get_ex(jobj, "size_bytes", &val)) vol->size_bytes = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "block_size_bytes", &val)) vol->block_size_bytes = (uint32_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "num_blocks", &val)) vol->num_blocks = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "state", &val)) vol->state = (xsan_storage_state_t)json_object_get_int(val); else vol->state = XSAN_STORAGE_STATE_OFFLINE;
    if (json_object_object_get_ex(jobj, "source_group_id", &val)) spdk_uuid_parse((struct spdk_uuid*)&vol->source_group_id.data[0], json_object_get_string(val));
    if (json_object_object_get_ex(jobj, "thin_provisioned", &val)) vol->thin_provisioned = json_object_get_boolean(val);
    if (json_object_object_get_ex(jobj, "allocated_bytes", &val)) vol->allocated_bytes = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "FTT", &val)) vol->FTT = (uint32_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "actual_replica_count", &val)) vol->actual_replica_count = (uint32_t)json_object_get_int(val);
    struct json_object *j_repl_nodes;
    if (json_object_object_get_ex(jobj, "replica_nodes", &j_repl_nodes) && json_object_is_type(j_repl_nodes, json_type_array)) {
        int arr_len = json_object_array_length(j_repl_nodes); if((uint32_t)arr_len > XSAN_MAX_REPLICAS) arr_len = XSAN_MAX_REPLICAS;
        if(vol->actual_replica_count != (uint32_t)arr_len && vol->actual_replica_count != 0) {XSAN_LOG_WARN("Vol %s: actual_replica_count %u != persisted array len %d. Using persisted.", vol->name?vol->name:"UNKNOWN", vol->actual_replica_count, arr_len);}
        vol->actual_replica_count = arr_len;
        for (uint32_t i = 0; i < vol->actual_replica_count; ++i) {
            struct json_object *j_repl_node = json_object_array_get_idx(j_repl_nodes, i);
            if (j_repl_node && json_object_is_type(j_repl_node, json_type_object)) {
                if (json_object_object_get_ex(j_repl_node, "node_id", &val)) spdk_uuid_parse((struct spdk_uuid*)&vol->replica_nodes[i].node_id.data[0], json_object_get_string(val));
                if (json_object_object_get_ex(j_repl_node, "node_ip_addr", &val)) xsan_strcpy_safe(vol->replica_nodes[i].node_ip_addr, json_object_get_string(val), INET6_ADDRSTRLEN);
                if (json_object_object_get_ex(j_repl_node, "node_comm_port", &val)) vol->replica_nodes[i].node_comm_port = (uint16_t)json_object_get_int(val);
                if (json_object_object_get_ex(j_repl_node, "state", &val)) vol->replica_nodes[i].state = (xsan_storage_state_t)json_object_get_int(val); else vol->replica_nodes[i].state = XSAN_STORAGE_STATE_UNKNOWN;
                if (json_object_object_get_ex(j_repl_node, "last_successful_contact_time_us", &val)) vol->replica_nodes[i].last_successful_contact_time_us = (uint64_t)json_object_get_int64(val);
            }
        }
    }
    json_object_put(jobj); *v_out = vol; return XSAN_OK;
}

static void _xsan_volume_update_overall_state(xsan_volume_manager_t *vm, xsan_volume_t *vol) {
    if (!vm || !vol) return;

    pthread_mutex_lock(&vm->lock);
    xsan_storage_state_t old_vol_state = vol->state;
    uint32_t online_replicas = 0;
    for (uint32_t i = 0; i < vol->actual_replica_count; ++i) {
        if (vol->replica_nodes[i].state == XSAN_STORAGE_STATE_ONLINE) {
            online_replicas++;
        }
    }

    uint32_t required_online_for_ok = vol->FTT + 1;
    if (required_online_for_ok > vol->actual_replica_count && vol->actual_replica_count > 0) {
        required_online_for_ok = vol->actual_replica_count;
    }

    if (online_replicas >= required_online_for_ok) {
        vol->state = XSAN_STORAGE_STATE_ONLINE;
    } else if (online_replicas > 0) {
        vol->state = XSAN_STORAGE_STATE_DEGRADED;
    } else {
        vol->state = XSAN_STORAGE_STATE_OFFLINE;
    }

    bool state_changed = (vol->state != old_vol_state);
    pthread_mutex_unlock(&vm->lock);

    if (state_changed) {
        XSAN_LOG_INFO("Volume '%s' (ID: %s) overall state changed from %d to %d (Online Replicas: %u/%u, FTT: %u)",
                      vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]),
                      old_vol_state, vol->state, online_replicas, vol->actual_replica_count, vol->FTT);
        xsan_volume_manager_save_volume_meta(vm, vol);
    }
}

static xsan_error_t xsan_volume_manager_save_volume_meta(xsan_volume_manager_t *vm, xsan_volume_t *vol) {
    if (!vm || !vm->md_store || !vol) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    char *json_string = NULL;
    xsan_error_t err = _xsan_volume_to_json_string(vol, &json_string);
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("Failed to serialize volume '%s' (ID: %s) to JSON: %s",
                       vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]), xsan_error_string(err));
        return err;
    }
    char key_buf[XSAN_MAX_NAME_LEN + SPDK_UUID_STRING_LEN];
    snprintf(key_buf, sizeof(key_buf), "%s%s", XSAN_VOLUME_META_PREFIX, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]));
    err = xsan_metadata_store_put(vm->md_store, key_buf, strlen(key_buf), json_string, strlen(json_string));
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("Failed to save volume '%s' (ID: %s) metadata to RocksDB: %s",
                       vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]), xsan_error_string(err));
    } else {
        XSAN_LOG_DEBUG("Successfully saved volume '%s' (ID: %s) metadata.",
                       vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]));
    }
    XSAN_FREE(json_string);
    return err;
}

static xsan_error_t xsan_volume_manager_delete_volume_meta(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id) {
    if (!vm || !vm->md_store || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    char key_buf[XSAN_MAX_NAME_LEN + SPDK_UUID_STRING_LEN];
    snprintf(key_buf, sizeof(key_buf), "%s%s", XSAN_VOLUME_META_PREFIX, spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));
    xsan_error_t err = xsan_metadata_store_delete(vm->md_store, key_buf, strlen(key_buf));
    if (err != XSAN_OK && err != XSAN_ERROR_NOT_FOUND) {
        XSAN_LOG_ERROR("Failed to delete volume (ID: %s) metadata from RocksDB: %s",
                       spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), xsan_error_string(err));
    } else {
        XSAN_LOG_DEBUG("Successfully deleted/confirmed deletion of volume (ID: %s) metadata.",
                       spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));
        if (err == XSAN_ERROR_NOT_FOUND) err = XSAN_OK;
    }
    return err;
}

xsan_error_t xsan_volume_manager_load_metadata(xsan_volume_manager_t *vm) {
    if (!vm || !vm->initialized || !vm->md_store) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    XSAN_LOG_INFO("Loading volume metadata from RocksDB store: %s", vm->metadata_db_path);
    pthread_mutex_lock(&vm->lock);
    xsan_metadata_iterator_t *iter = xsan_metadata_iterator_create(vm->md_store);
    if (!iter) {
        XSAN_LOG_ERROR("Failed to create metadata iterator for volume loading.");
        pthread_mutex_unlock(&vm->lock);
        return XSAN_ERROR_STORAGE_GENERIC;
    }
    xsan_metadata_iterator_seek(iter, XSAN_VOLUME_META_PREFIX, strlen(XSAN_VOLUME_META_PREFIX));
    while (xsan_metadata_iterator_is_valid(iter)) {
        size_t key_len;
        const char *key = xsan_metadata_iterator_key(iter, &key_len);
        if (!key || strncmp(key, XSAN_VOLUME_META_PREFIX, strlen(XSAN_VOLUME_META_PREFIX)) != 0) {
            break;
        }
        size_t value_len;
        const char *value_str = xsan_metadata_iterator_value(iter, &value_len);
        if (value_str) {
            xsan_volume_t *vol = NULL;
            xsan_error_t deser_err = _xsan_json_string_to_volume(value_str, vm, &vol);
            if (deser_err == XSAN_OK && vol) {
                if (xsan_list_append(vm->managed_volumes, vol) != NULL) {
                    XSAN_LOG_DEBUG("Loaded volume '%s' (ID: %s) from metadata.",
                                   vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]));
                     _xsan_volume_update_overall_state(vm, vol);
                } else {
                    XSAN_LOG_ERROR("Failed to append loaded volume '%s' to list.", vol->name);
                    _xsan_internal_volume_destroy_cb(vol);
                }
            } else {
                XSAN_LOG_ERROR("Failed to deserialize volume from metadata key '%.*s': %s",
                               (int)key_len, key, xsan_error_string(deser_err));
            }
        }
        xsan_metadata_iterator_next(iter);
    }
    xsan_metadata_iterator_destroy(iter);
    pthread_mutex_unlock(&vm->lock);
    XSAN_LOG_INFO("Volume metadata loading complete.");
    return XSAN_OK;
}

xsan_error_t xsan_volume_create(xsan_volume_manager_t *vm, const char *name, uint64_t size_bytes, xsan_group_id_t group_id, uint32_t logical_block_size_bytes, bool thin, uint32_t ftt, xsan_volume_id_t *vol_id_out ) {
    if (!vm || !vm->initialized || !name || size_bytes == 0 || spdk_uuid_is_null((struct spdk_uuid*)&group_id.data[0]) ||
        (logical_block_size_bytes != 512 && logical_block_size_bytes != 4096) || ftt >= XSAN_MAX_REPLICAS ) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    pthread_mutex_lock(&vm->lock);
    xsan_error_t err = XSAN_OK;
    xsan_list_node_t *node_iter;
    XSAN_LIST_FOREACH(vm->managed_volumes, node_iter) {
        xsan_volume_t *v = (xsan_volume_t*)xsan_list_node_get_value(node_iter);
        if(strncmp(v->name, name, XSAN_MAX_NAME_LEN) == 0) {
            err = XSAN_ERROR_ALREADY_EXISTS;
            goto cleanup_unlock;
        }
    }
    xsan_disk_group_t *dg = xsan_disk_manager_find_disk_group_by_id(vm->disk_manager, group_id);
    if (!dg) {
        err = XSAN_ERROR_NOT_FOUND;
        goto cleanup_unlock;
    }
    if (dg->state != XSAN_STORAGE_STATE_ONLINE) {
        err = XSAN_ERROR_RESOURCE_UNAVAILABLE;
        goto cleanup_unlock;
    }
    if (!thin && size_bytes > dg->usable_capacity_bytes) {
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
    if (size_bytes % logical_block_size_bytes != 0) {
        new_volume->num_blocks++;
        new_volume->size_bytes = new_volume->num_blocks * logical_block_size_bytes;
        XSAN_LOG_WARN("Volume '%s' size adjusted to %lu bytes to be block aligned (block size %u).",
                      name, new_volume->size_bytes, logical_block_size_bytes);
    }
    memcpy(&new_volume->source_group_id, &group_id, sizeof(xsan_group_id_t));
    new_volume->thin_provisioned = thin;
    new_volume->allocated_bytes = thin ? 0 : new_volume->size_bytes;
    new_volume->FTT = ftt;
    new_volume->actual_replica_count = ftt + 1;
    if (new_volume->actual_replica_count > XSAN_MAX_REPLICAS) {
        new_volume->actual_replica_count = XSAN_MAX_REPLICAS;
        XSAN_LOG_WARN("Requested replica count for vol '%s' (%u) exceeds max (%d), capped.", name, ftt+1, XSAN_MAX_REPLICAS);
    }
    xsan_node_id_t local_node_id;
    char local_ip[INET6_ADDRSTRLEN];
    uint16_t local_port;
    xsan_get_local_node_info(&local_node_id, local_ip, sizeof(local_ip), &local_port);
    memcpy(&new_volume->replica_nodes[0].node_id, &local_node_id, sizeof(xsan_node_id_t));
    xsan_strcpy_safe(new_volume->replica_nodes[0].node_ip_addr, local_ip, INET6_ADDRSTRLEN);
    new_volume->replica_nodes[0].node_comm_port = local_port;
    new_volume->replica_nodes[0].state = XSAN_STORAGE_STATE_ONLINE;
    new_volume->replica_nodes[0].last_successful_contact_time_us = _get_current_time_us();
    for (uint32_t i = 1; i < new_volume->actual_replica_count; ++i) {
        memset(&new_volume->replica_nodes[i].node_id, 0, sizeof(xsan_node_id_t));
        strcpy(new_volume->replica_nodes[i].node_ip_addr, "0.0.0.0");
        new_volume->replica_nodes[i].node_comm_port = 0;
        new_volume->replica_nodes[i].state = XSAN_STORAGE_STATE_INITIALIZING;
        new_volume->replica_nodes[i].last_successful_contact_time_us = 0;
    }
    new_volume->state = XSAN_STORAGE_STATE_INITIALIZING;
    if (xsan_list_append(vm->managed_volumes, new_volume) == NULL) {
        err = XSAN_ERROR_OUT_OF_MEMORY;
        XSAN_FREE(new_volume);
        goto cleanup_unlock;
    }
    _xsan_volume_update_overall_state(vm, new_volume);
    if (new_volume->state == XSAN_STORAGE_STATE_INITIALIZING && err == XSAN_OK) {
         err = xsan_volume_manager_save_volume_meta(vm, new_volume);
    }
    if (err != XSAN_OK) {
        XSAN_LOG_ERROR("Failed to save metadata for new volume '%s'. It remains in memory but not persisted.", name);
    }
    if (vol_id_out) {
        memcpy(vol_id_out, &new_volume->id, sizeof(xsan_volume_id_t));
    }
    if(err == XSAN_OK) {
        XSAN_LOG_INFO("Volume '%s' (ID: %s) created successfully. Size: %lu bytes, FTT: %u.",
                  new_volume->name, spdk_uuid_get_string((struct spdk_uuid*)&new_volume->id.data[0]),
                  new_volume->size_bytes, new_volume->FTT);
    }
cleanup_unlock:
    pthread_mutex_unlock(&vm->lock);
    return err;
}

xsan_error_t xsan_volume_delete(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id){
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    pthread_mutex_lock(&vm->lock);
    xsan_error_t err = XSAN_ERROR_NOT_FOUND;
    xsan_list_node_t *node = xsan_list_get_head(vm->managed_volumes);
    xsan_volume_t *vol_to_delete = NULL;
    while (node != NULL) {
        xsan_volume_t *current_vol = (xsan_volume_t *)xsan_list_node_get_value(node);
        if (spdk_uuid_compare((struct spdk_uuid*)&current_vol->id.data[0], (struct spdk_uuid*)&volume_id.data[0]) == 0) {
            vol_to_delete = current_vol;
            break;
        }
        node = xsan_list_node_next(node);
    }
    if (vol_to_delete) {
        xsan_list_remove_node(vm->managed_volumes, node);
        vol_to_delete = NULL;
        err = xsan_volume_manager_delete_volume_meta(vm, volume_id);
        if (err != XSAN_OK) {
            XSAN_LOG_ERROR("Failed to delete metadata for volume ID %s. In-memory entry removed.",
                           spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));
        } else {
            XSAN_LOG_INFO("Volume (ID: %s) deleted successfully.", spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));
        }
        err = XSAN_OK;
    }
    pthread_mutex_unlock(&vm->lock);
    return err;
}

xsan_volume_t *xsan_volume_get_by_id(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id){
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

xsan_volume_t *xsan_volume_get_by_name(xsan_volume_manager_t *vm, const char *name){
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

xsan_error_t xsan_volume_list_all(xsan_volume_manager_t *vm, xsan_volume_t ***volumes_array_out, int *count_out){
    if (!vm || !vm->initialized || !volumes_array_out || !count_out) {
        if (count_out) *count_out = 0;
        if (volumes_array_out) *volumes_array_out = NULL;
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

void xsan_volume_manager_free_volume_pointer_list(xsan_volume_t **v_ptr_arr){ if(v_ptr_arr)XSAN_FREE(v_ptr_arr);}

xsan_error_t xsan_volume_map_lba_to_physical(xsan_volume_manager_t *vm,
                                             xsan_volume_id_t volume_id,
                                             uint64_t logical_block_idx,
                                             xsan_disk_id_t *out_disk_id,
                                             uint64_t *out_physical_block_idx,
                                             uint32_t *out_physical_block_size) {
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0]) ||
        !out_disk_id || !out_physical_block_idx || !out_physical_block_size) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    xsan_volume_t *vol = xsan_volume_get_by_id(vm, volume_id);
    if (!vol) {
        return XSAN_ERROR_NOT_FOUND;
    }
    if (logical_block_idx >= vol->num_blocks) {
        return XSAN_ERROR_OUT_OF_BOUNDS;
    }
    xsan_disk_group_t *dg = xsan_disk_manager_find_disk_group_by_id(vm->disk_manager, vol->source_group_id);
    if (!dg) {
        XSAN_LOG_ERROR("Volume %s (ID: %s) source disk group (ID: %s) not found.",
                       vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]),
                       spdk_uuid_get_string((struct spdk_uuid*)&vol->source_group_id.data[0]));
        return XSAN_ERROR_STORAGE_GENERIC;
    }
    if (dg->disk_count == 0 || dg->state != XSAN_STORAGE_STATE_ONLINE) {
        XSAN_LOG_ERROR("Volume %s (ID: %s) source disk group '%s' has no disks or is not online (State: %d).",
                       vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]), dg->name, dg->state);
        return XSAN_ERROR_RESOURCE_UNAVAILABLE;
    }
    xsan_disk_id_t target_disk_id = dg->disk_ids[0];
    xsan_disk_t *target_disk = xsan_disk_manager_find_disk_by_id(vm->disk_manager, target_disk_id);
    if (!target_disk) {
        XSAN_LOG_ERROR("Volume %s: First disk (ID: %s) of group '%s' not found.",
                       vol->name, spdk_uuid_get_string((struct spdk_uuid*)&target_disk_id.data[0]), dg->name);
        return XSAN_ERROR_STORAGE_GENERIC;
    }
    if (target_disk->state != XSAN_STORAGE_STATE_ONLINE) {
         XSAN_LOG_ERROR("Volume %s: Target disk '%s' (ID: %s) in group '%s' is not online (State: %d).",
                       vol->name, target_disk->bdev_name, spdk_uuid_get_string((struct spdk_uuid*)&target_disk_id.data[0]),
                       dg->name, target_disk->state);
        return XSAN_ERROR_RESOURCE_UNAVAILABLE;
    }
    if (target_disk->block_size_bytes == 0) {
        XSAN_LOG_ERROR("Volume %s: Target disk '%s' (ID: %s) has zero block size.", vol->name, target_disk->bdev_name, spdk_uuid_get_string((struct spdk_uuid*)&target_disk_id.data[0]));
        return XSAN_ERROR_STORAGE_GENERIC;
    }
    if (vol->num_blocks > target_disk->num_blocks) {
         XSAN_LOG_WARN("Volume %s (blocks: %lu) is larger than its target disk '%s' (blocks: %lu). Mapping might fail for higher LBAs.",
                       vol->name, vol->num_blocks, target_disk->bdev_name, target_disk->num_blocks);
    }
    memcpy(out_disk_id, &target_disk->id, sizeof(xsan_disk_id_t));
    *out_physical_block_idx = logical_block_idx;
    *out_physical_block_size = target_disk->block_size_bytes;
    return XSAN_OK;
}

// This callback is set as io_req->user_cb and is called by the xsan_io layer
// (specifically, by the SPDK IO completion handler which then calls io_req->user_cb)
// after the SPDK bdev I/O completes.
static void _xsan_physical_io_complete_cb(void *cb_arg_from_io_layer, xsan_error_t status) {
    // cb_arg_from_io_layer is actually the xsan_io_request_t* itself, as setup by xsan_io_submit_request_to_bdev
    xsan_io_request_t *io_req = (xsan_io_request_t *)cb_arg_from_io_layer;
    if (!io_req || !io_req->user_cb_arg) { // io_req->user_cb_arg should be our xsan_vm_physical_io_ctx_t*
        XSAN_LOG_ERROR("Physical I/O complete with NULL io_req or user_cb_arg (phys_io_ctx)!");
        if (io_req) {
            // This is tricky: if user_cb_arg is missing, we don't know the upper callback.
            // xsan_io_request_free should ideally be called. This indicates a programming error.
        }
        return;
    }

    xsan_vm_physical_io_ctx_t *phys_io_ctx = (xsan_vm_physical_io_ctx_t *)io_req->user_cb_arg;

    if (phys_io_ctx->is_read_op && status == XSAN_OK) {
        // In xsan_io_submit_request_to_bdev, if dma_buffer_is_internal is true,
        // data is read into io_req->dma_buffer. If dma_buffer_is_internal is false,
        // data is read directly into io_req->user_buffer (which is original_user_buffer).
        // So, copy is only needed if dma_buffer_is_internal was true.
        if (io_req->dma_buffer && phys_io_ctx->original_user_buffer && io_req->dma_buffer_is_internal) {
            memcpy(phys_io_ctx->original_user_buffer, io_req->dma_buffer, phys_io_ctx->length_bytes);
            XSAN_LOG_DEBUG("Read data copied from DMA to user buffer for vol %s, len %lu",
                spdk_uuid_get_string((struct spdk_uuid*)&phys_io_ctx->volume_id_for_log.data[0]), phys_io_ctx->length_bytes);
        } else if (io_req->dma_buffer_is_internal && (!io_req->dma_buffer || !phys_io_ctx->original_user_buffer)) {
             XSAN_LOG_ERROR("Read success for vol %s, but data copy failed due to missing buffers/info (dma_is_internal=%d, dma_buf=%p, user_buf=%p).",
                spdk_uuid_get_string((struct spdk_uuid*)&phys_io_ctx->volume_id_for_log.data[0]),
                io_req->dma_buffer_is_internal, io_req->dma_buffer, phys_io_ctx->original_user_buffer);
             status = XSAN_ERROR_INTERNAL;
        }
        // If !io_req->dma_buffer_is_internal, data was read directly into phys_io_ctx->original_user_buffer, so no copy needed.
    }

    if (phys_io_ctx->actual_upper_cb) {
        phys_io_ctx->actual_upper_cb(phys_io_ctx->actual_upper_cb_arg, status);
    }
    // xsan_io_request_free is called by _xsan_io_spdk_completion_cb after this user_cb returns.
    // So, we only need to free the phys_io_ctx wrapper.
    XSAN_FREE(phys_io_ctx);
}

static xsan_error_t _xsan_volume_submit_single_io_attempt(
    xsan_volume_manager_t *vm,
    xsan_volume_id_t volume_id,
    uint64_t logical_byte_offset,
    uint64_t length_bytes,
    void *original_user_buffer,
    bool is_read_op,
    xsan_user_io_completion_cb_t upper_completion_cb,
    void *upper_completion_cb_arg) {

    if (!vm || !vm->initialized) return XSAN_ERROR_INVALID_PARAM;
    xsan_volume_t *vol = xsan_volume_get_by_id(vm, volume_id);
    if (!vol) return XSAN_ERROR_NOT_FOUND;
    if (vol->block_size_bytes == 0 ||
        (logical_byte_offset % vol->block_size_bytes != 0) ||
        (length_bytes % vol->block_size_bytes != 0) ||
        (length_bytes == 0) ||
        (logical_byte_offset + length_bytes > vol->size_bytes)) {
        return XSAN_ERROR_INVALID_PARAM_ALIGNMENT;
    }
    uint64_t logical_block_idx = logical_byte_offset / vol->block_size_bytes;
    xsan_disk_id_t physical_disk_id;
    uint64_t physical_start_block_idx;
    uint32_t physical_block_size_bytes;
    xsan_error_t map_err = xsan_volume_map_lba_to_physical(vm, volume_id, logical_block_idx,
                                                         &physical_disk_id, &physical_start_block_idx,
                                                         &physical_block_size_bytes);
    if (map_err != XSAN_OK) {
        XSAN_LOG_ERROR("Vol %s: Failed to map LBA_idx %lu: %s",
                       spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), logical_block_idx, xsan_error_string(map_err));
        return map_err;
    }
    if (physical_block_size_bytes == 0) {
        XSAN_LOG_ERROR("Vol %s: Mapped physical disk has zero block size.", spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));
        return XSAN_ERROR_STORAGE_GENERIC;
    }
    if (length_bytes % physical_block_size_bytes != 0) {
        XSAN_LOG_ERROR("Vol %s: I/O length %lu is not a multiple of physical block size %u for mapped LBA %lu.",
                        spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), length_bytes, physical_block_size_bytes, logical_block_idx);
        return XSAN_ERROR_INVALID_PARAM_ALIGNMENT;
    }
    xsan_disk_t *physical_disk = xsan_disk_manager_find_disk_by_id(vm->disk_manager, physical_disk_id);
    if (!physical_disk) {
        XSAN_LOG_ERROR("Vol %s: Physical disk (ID: %s) for LBA map not found.",
                       spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), spdk_uuid_get_string((struct spdk_uuid*)&physical_disk_id.data[0]));
        return XSAN_ERROR_STORAGE_GENERIC;
    }
    if (!physical_disk->bdev_descriptor) {
        XSAN_LOG_ERROR("Vol %s: Physical disk '%s' (ID: %s) for LBA map has no bdev descriptor.",
                       spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), physical_disk->bdev_name, spdk_uuid_get_string((struct spdk_uuid*)&physical_disk_id.data[0]));
        return XSAN_ERROR_RESOURCE_UNAVAILABLE;
    }

    xsan_vm_physical_io_ctx_t *phys_io_ctx = XSAN_MALLOC(sizeof(xsan_vm_physical_io_ctx_t));
    if(!phys_io_ctx) return XSAN_ERROR_OUT_OF_MEMORY;

    phys_io_ctx->actual_upper_cb = upper_completion_cb;
    phys_io_ctx->actual_upper_cb_arg = upper_completion_cb_arg;
    phys_io_ctx->original_user_buffer = original_user_buffer;
    phys_io_ctx->is_read_op = is_read_op;
    phys_io_ctx->length_bytes = length_bytes;
    memcpy(&phys_io_ctx->volume_id_for_log, &volume_id, sizeof(xsan_volume_id_t));

    xsan_io_request_t *io_req = xsan_io_request_create(volume_id,
                                                      original_user_buffer,
                                                      physical_start_block_idx * physical_block_size_bytes,
                                                      length_bytes,
                                                      physical_block_size_bytes,
                                                      is_read_op,
                                                      _xsan_physical_io_complete_cb,
                                                      phys_io_ctx);
    if (!io_req) {
        XSAN_FREE(phys_io_ctx);
        return XSAN_ERROR_OUT_OF_MEMORY;
    }
    phys_io_ctx->io_req = io_req;

    memcpy(&io_req->target_disk_id, &physical_disk->id, sizeof(xsan_disk_id_t));
    xsan_strcpy_safe(io_req->target_bdev_name, physical_disk->bdev_name, XSAN_MAX_NAME_LEN);
    io_req->bdev_desc = physical_disk->bdev_descriptor;
    io_req->io_channel = NULL;
    io_req->own_spdk_resources = false;

    xsan_error_t submit_err = xsan_io_submit_request_to_bdev(io_req);

    if (submit_err != XSAN_OK) {
        XSAN_LOG_ERROR("Vol %s: Failed to submit %s to bdev '%s' via xsan_io: %s",
                       spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]),
                       is_read_op ? "read" : "write",
                       physical_disk->bdev_name, xsan_error_string(submit_err));
        // xsan_io_submit_request_to_bdev should free io_req on sync failure path (as per its current code)
        // so we only need to free phys_io_ctx
        XSAN_FREE(phys_io_ctx);
        return submit_err;
    }
    return XSAN_OK;
}


static void _xsan_local_replica_write_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_replicated_io_ctx_t *rep_ctx = cb_arg; if(!rep_ctx)return;
    xsan_volume_t *vol = xsan_volume_get_by_id(g_xsan_volume_manager_instance, rep_ctx->volume_id);
    if(vol && vol->actual_replica_count > 0){
        pthread_mutex_lock(&g_xsan_volume_manager_instance->lock);
        vol->replica_nodes[0].state = (status == XSAN_OK) ? XSAN_STORAGE_STATE_ONLINE : XSAN_STORAGE_STATE_FAILED;
        if(status == XSAN_OK) vol->replica_nodes[0].last_successful_contact_time_us = _get_current_time_us();
        pthread_mutex_unlock(&g_xsan_volume_manager_instance->lock);
    }
    if(status==XSAN_OK)__sync_fetch_and_add(&rep_ctx->successful_writes,1); else {__sync_fetch_and_add(&rep_ctx->failed_writes,1); if(rep_ctx->final_status==XSAN_OK)rep_ctx->final_status=status;}
    rep_ctx->local_io_req = NULL; _xsan_check_replicated_write_completion(rep_ctx);
}
static void _xsan_remote_replica_connect_then_send_cb(struct spdk_sock *sock, int status, void *cb_arg) {
    xsan_per_replica_op_ctx_t *p_ctx = cb_arg; if(!p_ctx || !p_ctx->parent_rep_ctx || !p_ctx->request_msg_to_send){ return;}
    xsan_replicated_io_ctx_t* rep_ctx = p_ctx->parent_rep_ctx;
    xsan_volume_t *vol = xsan_volume_get_by_id(g_xsan_volume_manager_instance, rep_ctx->volume_id);
    uint32_t replica_idx = XSAN_MAX_REPLICAS;
    if(vol){
        pthread_mutex_lock(&g_xsan_volume_manager_instance->lock);
        for(uint32_t i=1; i<vol->actual_replica_count; ++i) if(memcmp(&vol->replica_nodes[i].node_id, &p_ctx->replica_location_info.node_id, sizeof(xsan_node_id_t))==0) {replica_idx=i; break;}
        pthread_mutex_unlock(&g_xsan_volume_manager_instance->lock);
    }

    if(status==0 && sock){ p_ctx->connected_sock = sock; xsan_error_t s_err = xsan_node_comm_send_msg(sock, p_ctx->request_msg_to_send, _xsan_remote_replica_request_send_actual_cb, p_ctx); if(s_err!=XSAN_OK){ _xsan_remote_replica_request_send_actual_cb(s_err, p_ctx);}}
    else {
        if(vol && replica_idx < XSAN_MAX_REPLICAS) {
            pthread_mutex_lock(&g_xsan_volume_manager_instance->lock);
            vol->replica_nodes[replica_idx].state = XSAN_STORAGE_STATE_OFFLINE;
            pthread_mutex_unlock(&g_xsan_volume_manager_instance->lock);
        }
        __sync_fetch_and_add(&rep_ctx->failed_writes,1); if(rep_ctx->final_status==XSAN_OK)rep_ctx->final_status=xsan_error_from_errno(-status);
        xsan_protocol_message_destroy(p_ctx->request_msg_to_send); XSAN_FREE(p_ctx);
        _xsan_check_replicated_write_completion(rep_ctx);
    }
}
static void _xsan_remote_replica_request_send_actual_cb(int comm_status, void *cb_arg) {
    xsan_per_replica_op_ctx_t *p_ctx = cb_arg; if(!p_ctx || !p_ctx->parent_rep_ctx){if(p_ctx)XSAN_FREE(p_ctx);return;}
    xsan_replicated_io_ctx_t* rep_ctx = p_ctx->parent_rep_ctx;
    xsan_volume_t *vol = xsan_volume_get_by_id(g_xsan_volume_manager_instance, rep_ctx->volume_id);
    uint32_t replica_idx = XSAN_MAX_REPLICAS;
    if(vol){
        pthread_mutex_lock(&g_xsan_volume_manager_instance->lock);
        for(uint32_t i=1; i<vol->actual_replica_count; ++i) if(memcmp(&vol->replica_nodes[i].node_id, &p_ctx->replica_location_info.node_id, sizeof(xsan_node_id_t))==0) {replica_idx=i; break;}
        pthread_mutex_unlock(&g_xsan_volume_manager_instance->lock);
    }

    if(comm_status!=0){
        if(vol && replica_idx < XSAN_MAX_REPLICAS) {
            pthread_mutex_lock(&g_xsan_volume_manager_instance->lock);
            vol->replica_nodes[replica_idx].state = XSAN_STORAGE_STATE_OFFLINE;
            pthread_mutex_unlock(&g_xsan_volume_manager_instance->lock);
        }
        __sync_fetch_and_add(&rep_ctx->failed_writes,1); if(rep_ctx->final_status==XSAN_OK)rep_ctx->final_status=xsan_error_from_errno(-comm_status); _xsan_check_replicated_write_completion(rep_ctx);
    }
    if(p_ctx->request_msg_to_send) xsan_protocol_message_destroy(p_ctx->request_msg_to_send);
    XSAN_FREE(p_ctx);
}

void xsan_volume_manager_process_replica_write_response(xsan_volume_manager_t *vm, uint64_t tid, xsan_node_id_t resp_node_id, xsan_error_t repl_op_status) {
    if (!vm || !vm->initialized) return;
    pthread_mutex_lock(&vm->pending_ios_lock); xsan_replicated_io_ctx_t *rep_ctx = xsan_hashtable_get(vm->pending_replicated_ios, &tid); pthread_mutex_unlock(&vm->pending_ios_lock);
    if (rep_ctx) {
        xsan_volume_t *vol = xsan_volume_get_by_id(vm, rep_ctx->volume_id);
        if(vol){
            pthread_mutex_lock(&vm->lock);
            for(uint32_t i=1; i<vol->actual_replica_count; ++i){
                if(spdk_uuid_compare((struct spdk_uuid*)&vol->replica_nodes[i].node_id.data[0], (struct spdk_uuid*)&resp_node_id.data[0])==0){
                    vol->replica_nodes[i].state = (repl_op_status==XSAN_OK) ? XSAN_STORAGE_STATE_ONLINE : XSAN_STORAGE_STATE_DEGRADED;
                    if(repl_op_status==XSAN_OK) vol->replica_nodes[i].last_successful_contact_time_us = _get_current_time_us();
                    break;
                }
            }
            pthread_mutex_unlock(&vm->lock);
        }
        if (repl_op_status == XSAN_OK) __sync_fetch_and_add(&rep_ctx->successful_writes, 1);
        else { __sync_fetch_and_add(&rep_ctx->failed_writes, 1); if (rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = repl_op_status; }
        _xsan_check_replicated_write_completion(rep_ctx);
    } else { XSAN_LOG_WARN("No pending rep IO ctx for TID %lu from node %s.", tid, spdk_uuid_get_string((struct spdk_uuid*)&resp_node_id.data[0]));}
}

xsan_error_t xsan_volume_read_async(xsan_volume_manager_t *vm, xsan_volume_id_t vol_id, uint64_t log_byte_off, uint64_t len_bytes, void *u_buf, xsan_user_io_completion_cb_t u_cb, void *u_cb_arg) {
    if (!vm || !vm->initialized || !u_buf || len_bytes==0 || !u_cb) return XSAN_ERROR_INVALID_PARAM;
    xsan_volume_t *vol = xsan_volume_get_by_id(vm, vol_id); if(!vol) return XSAN_ERROR_NOT_FOUND;
    if (vol->block_size_bytes==0 || (log_byte_off % vol->block_size_bytes !=0) || (len_bytes % vol->block_size_bytes !=0) || (log_byte_off+len_bytes > vol->size_bytes)) return XSAN_ERROR_INVALID_PARAM_ALIGNMENT;
    static uint64_t s_rtid_ctr = 6000; uint64_t tid = __sync_fetch_and_add(&s_rtid_ctr,1);
    xsan_replica_read_coordinator_ctx_t *coord = xsan_replica_read_coordinator_ctx_create(vol,u_buf,log_byte_off,len_bytes,u_cb,u_cb_arg,tid);
    if(!coord) return XSAN_ERROR_OUT_OF_MEMORY;
    _xsan_try_read_from_next_replica(coord); return XSAN_OK;
}
static void _xsan_try_read_from_next_replica(xsan_replica_read_coordinator_ctx_t *coord_ctx) {
    if(!coord_ctx || !coord_ctx->vol){ if(coord_ctx){if(coord_ctx->original_user_cb)coord_ctx->original_user_cb(coord_ctx->original_user_cb_arg, XSAN_ERROR_INVALID_PARAM); xsan_replica_read_coordinator_ctx_free(coord_ctx);} return;}
    if(coord_ctx->current_replica_idx_to_try >= (int)coord_ctx->vol->actual_replica_count || coord_ctx->current_replica_idx_to_try >= XSAN_MAX_REPLICAS) {
        if(coord_ctx->original_user_cb)coord_ctx->original_user_cb(coord_ctx->original_user_cb_arg, coord_ctx->last_attempt_status);
        pthread_mutex_lock(&g_xsan_volume_manager_instance->pending_ios_lock); xsan_hashtable_remove(g_xsan_volume_manager_instance->pending_replica_reads, &coord_ctx->transaction_id); pthread_mutex_unlock(&g_xsan_volume_manager_instance->pending_ios_lock);
        xsan_replica_read_coordinator_ctx_free(coord_ctx); return;
    }
    int cur_idx = coord_ctx->current_replica_idx_to_try;
    xsan_replica_location_t *loc = NULL;
    pthread_mutex_lock(&g_xsan_volume_manager_instance->lock); // Lock to safely access vol->replica_nodes
    if (cur_idx < (int)coord_ctx->vol->actual_replica_count) { // Double check after lock
        loc = &coord_ctx->vol->replica_nodes[cur_idx];
    }
    pthread_mutex_unlock(&g_xsan_volume_manager_instance->lock);

    if (!loc) { // Should not happen if current_replica_idx_to_try is managed correctly
        coord_ctx->last_attempt_status = XSAN_ERROR_INTERNAL;
        coord_ctx->current_replica_idx_to_try++;
         _xsan_try_read_from_next_replica(coord_ctx);
        return;
    }

    bool is_local = (cur_idx==0);

    XSAN_LOG_DEBUG("Read attempt for vol %s, TID %lu, replica_idx %d (local: %d, IP: %s)",
                   spdk_uuid_get_string((struct spdk_uuid*)&coord_ctx->vol->id.data[0]), coord_ctx->transaction_id, cur_idx, is_local, loc->node_ip_addr);

    if(is_local){
        coord_ctx->last_attempt_status = _xsan_volume_submit_single_io_attempt(
                                            g_xsan_volume_manager_instance,
                                            coord_ctx->vol->id,
                                            coord_ctx->logical_byte_offset,
                                            coord_ctx->length_bytes,
                                            coord_ctx->user_buffer,
                                            true,
                                            _xsan_replica_read_attempt_complete_cb,
                                            coord_ctx);
        if(coord_ctx->last_attempt_status!=XSAN_OK){
            coord_ctx->current_replica_idx_to_try++;
            _xsan_try_read_from_next_replica(coord_ctx);
        }
    } else {
        if(!coord_ctx->internal_dma_buffer && coord_ctx->length_bytes > 0){
            size_t align = 4096;
            xsan_disk_group_t *dg = xsan_disk_manager_find_disk_group_by_id(g_xsan_volume_manager_instance->disk_manager, coord_ctx->vol->source_group_id);
            if (dg && dg->disk_count > 0) {
                xsan_disk_t *disk0 = xsan_disk_manager_find_disk_by_id(g_xsan_volume_manager_instance->disk_manager, dg->disk_ids[0]);
                if (disk0 && disk0->bdev_name[0] != '\0') align = xsan_bdev_get_buf_align(disk0->bdev_name);
            }
            coord_ctx->internal_dma_buffer=xsan_bdev_dma_malloc(coord_ctx->length_bytes, align);
            if(!coord_ctx->internal_dma_buffer){
                coord_ctx->last_attempt_status=XSAN_ERROR_OUT_OF_MEMORY;
                coord_ctx->current_replica_idx_to_try++;
                _xsan_try_read_from_next_replica(coord_ctx);
                return;
            }
            coord_ctx->internal_dma_buffer_allocated=true;
            coord_ctx->internal_dma_buffer_size=coord_ctx->length_bytes;
        }
        xsan_per_replica_op_ctx_t *rop_ctx = XSAN_MALLOC(sizeof(*rop_ctx));
        if(!rop_ctx){ coord_ctx->last_attempt_status=XSAN_ERROR_OUT_OF_MEMORY; coord_ctx->current_replica_idx_to_try++; _xsan_try_read_from_next_replica(coord_ctx); return;}
        memset(rop_ctx,0,sizeof(*rop_ctx));
        rop_ctx->parent_rep_ctx=(void*)coord_ctx;
        memcpy(&rop_ctx->replica_location_info,loc,sizeof(xsan_replica_location_t));
        xsan_replica_read_req_payload_t req_pl;
        memcpy(&req_pl.volume_id,&coord_ctx->vol->id,sizeof(req_pl.volume_id));
        req_pl.block_lba_on_volume=coord_ctx->logical_byte_offset/coord_ctx->vol->block_size_bytes;
        req_pl.num_blocks=coord_ctx->length_bytes/coord_ctx->vol->block_size_bytes;
        rop_ctx->request_msg_to_send = xsan_protocol_message_create(XSAN_MSG_TYPE_REPLICA_READ_BLOCK_REQ, coord_ctx->transaction_id, &req_pl, sizeof(req_pl));
        if(!rop_ctx->request_msg_to_send){
            XSAN_FREE(rop_ctx);
            coord_ctx->last_attempt_status=XSAN_ERROR_OUT_OF_MEMORY;
            coord_ctx->current_replica_idx_to_try++;
            _xsan_try_read_from_next_replica(coord_ctx);
            return;
        }
        coord_ctx->current_remote_op_ctx = rop_ctx;
        pthread_mutex_lock(&g_xsan_volume_manager_instance->pending_ios_lock);
        xsan_hashtable_put(g_xsan_volume_manager_instance->pending_replica_reads, &coord_ctx->transaction_id, coord_ctx);
        pthread_mutex_unlock(&g_xsan_volume_manager_instance->pending_ios_lock);
        struct spdk_sock *sock = xsan_node_comm_get_active_connection(loc->node_ip_addr,loc->node_comm_port);
        if(sock) _xsan_remote_replica_read_connect_then_send_cb(sock,0,rop_ctx);
        else xsan_node_comm_connect(loc->node_ip_addr,loc->node_comm_port,_xsan_remote_replica_read_connect_then_send_cb,rop_ctx);
    }
}
static void _xsan_replica_read_attempt_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_replica_read_coordinator_ctx_t*ctx=cb_arg; if(!ctx)return;
    XSAN_LOG_DEBUG("Replica read attempt for vol %s, TID %lu, replica_idx %d completed with status %d",
                   spdk_uuid_get_string((struct spdk_uuid*)&ctx->vol->id.data[0]), ctx->transaction_id, ctx->current_replica_idx_to_try, status);
    if(status==XSAN_OK){
        bool was_remote_attempt = (ctx->current_replica_idx_to_try > 0);
        if (was_remote_attempt && ctx->internal_dma_buffer_allocated && ctx->internal_dma_buffer && ctx->user_buffer) {
             memcpy(ctx->user_buffer, ctx->internal_dma_buffer, ctx->length_bytes);
        }
        ctx->original_user_cb(ctx->original_user_cb_arg,XSAN_OK);
        pthread_mutex_lock(&g_xsan_volume_manager_instance->pending_ios_lock);xsan_hashtable_remove(g_xsan_volume_manager_instance->pending_replica_reads,&ctx->transaction_id);pthread_mutex_unlock(&g_xsan_volume_manager_instance->pending_ios_lock);
        xsan_replica_read_coordinator_ctx_free(ctx);
    } else {
        ctx->last_attempt_status=status;
        ctx->current_replica_idx_to_try++;
        if(ctx->current_remote_op_ctx){
            if(ctx->current_remote_op_ctx->request_msg_to_send)xsan_protocol_message_destroy(ctx->current_remote_op_ctx->request_msg_to_send);
            XSAN_FREE(ctx->current_remote_op_ctx);
            ctx->current_remote_op_ctx=NULL;
        }
        _xsan_try_read_from_next_replica(ctx);
    }
}
static void _xsan_remote_replica_read_req_send_complete_cb(int comm_status, void *cb_arg) {
    xsan_per_replica_op_ctx_t*p_ctx=cb_arg;if(!p_ctx||!p_ctx->parent_rep_ctx){if(p_ctx)XSAN_FREE(p_ctx);return;}
    xsan_replica_read_coordinator_ctx_t*coord_ctx=(xsan_replica_read_coordinator_ctx_t*)p_ctx->parent_rep_ctx;
    if(comm_status!=0){
        _xsan_replica_read_attempt_complete_cb(coord_ctx,xsan_error_from_errno(-comm_status));
    }
    if(p_ctx->request_msg_to_send)xsan_protocol_message_destroy(p_ctx->request_msg_to_send);
    XSAN_FREE(p_ctx);
    coord_ctx->current_remote_op_ctx=NULL;
}
static void _xsan_remote_replica_read_connect_then_send_cb(struct spdk_sock *sock, int status, void *cb_arg) {
    xsan_per_replica_op_ctx_t*p_ctx=cb_arg;if(!p_ctx||!p_ctx->parent_rep_ctx||!p_ctx->request_msg_to_send){if(p_ctx&&p_ctx->request_msg_to_send)xsan_protocol_message_destroy(p_ctx->request_msg_to_send);if(p_ctx)XSAN_FREE(p_ctx);return;}
    xsan_replica_read_coordinator_ctx_t*coord_ctx=(xsan_replica_read_coordinator_ctx_t*)p_ctx->parent_rep_ctx;
    if(status==0&&sock){
        p_ctx->connected_sock=sock;
        xsan_error_t s_err=xsan_node_comm_send_msg(sock,p_ctx->request_msg_to_send,_xsan_remote_replica_read_req_send_complete_cb,p_ctx);
        if(s_err!=XSAN_OK) _xsan_remote_replica_read_req_send_complete_cb(s_err,p_ctx);
    } else {
        _xsan_replica_read_attempt_complete_cb(coord_ctx,xsan_error_from_errno(-status));
    }
}
void xsan_volume_manager_process_replica_read_response(xsan_volume_manager_t *vm, uint64_t tid, xsan_node_id_t r_nid, xsan_error_t r_op_status, const unsigned char *data, uint32_t data_len) {
    if(!vm||!vm->initialized)return;
    pthread_mutex_lock(&vm->pending_ios_lock); xsan_replica_read_coordinator_ctx_t*ctx=xsan_hashtable_get(vm->pending_replica_reads,&tid); pthread_mutex_unlock(&vm->pending_ios_lock);
    if(ctx){
        if(r_op_status==XSAN_OK){
            if(data && data_len == ctx->length_bytes){
                if(!ctx->internal_dma_buffer){
                    XSAN_LOG_ERROR("TID %lu: internal_dma_buffer is NULL in process_replica_read_response for vol %s. Allocating.", tid, spdk_uuid_get_string((struct spdk_uuid*)&ctx->vol->id.data[0]));
                    size_t align = 4096;
                    xsan_disk_group_t *dg = xsan_disk_manager_find_disk_group_by_id(g_xsan_volume_manager_instance->disk_manager, ctx->vol->source_group_id);
                    if (dg && dg->disk_count > 0) {
                        xsan_disk_t *disk0 = xsan_disk_manager_find_disk_by_id(g_xsan_volume_manager_instance->disk_manager, dg->disk_ids[0]);
                         if (disk0 && disk0->bdev_name[0] != '\0') align = xsan_bdev_get_buf_align(disk0->bdev_name);
                    }
                    ctx->internal_dma_buffer=xsan_bdev_dma_malloc(ctx->length_bytes,align);
                    if(ctx->internal_dma_buffer){ctx->internal_dma_buffer_allocated=true;ctx->internal_dma_buffer_size=ctx->length_bytes;}
                    else{r_op_status=XSAN_ERROR_OUT_OF_MEMORY;}
                }
                if(r_op_status==XSAN_OK && ctx->internal_dma_buffer) memcpy(ctx->internal_dma_buffer,data,data_len);
            } else {
                XSAN_LOG_ERROR("TID %lu: Replica read response for vol %s has data len %u, expected %lu, or data is NULL.", tid, spdk_uuid_get_string((struct spdk_uuid*)&ctx->vol->id.data[0]), data_len, ctx->length_bytes);
                r_op_status=XSAN_ERROR_PROTOCOL_GENERIC;
            }
        }
        // Do not remove from hashtable here, _xsan_replica_read_attempt_complete_cb will do it on final success/failure.
        _xsan_replica_read_attempt_complete_cb(ctx,r_op_status);
    } else {
        XSAN_LOG_WARN("No pending replica read ctx for TID %lu from node %s.", tid, spdk_uuid_get_string((struct spdk_uuid*)&r_nid.data[0]));
    }
}

xsan_error_t xsan_volume_write_async(xsan_volume_manager_t *vm,
                                     xsan_volume_id_t volume_id,
                                     uint64_t logical_byte_offset,
                                     uint64_t length_bytes,
                                     const void *user_buf,
                                     xsan_user_io_completion_cb_t user_cb,
                                     void *user_cb_arg) {
    if (!vm || !vm->initialized || !user_buf || length_bytes == 0 || !user_cb ||
        spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    xsan_volume_t *vol = xsan_volume_get_by_id(vm, volume_id);
    if (!vol) {
        XSAN_LOG_ERROR("Volume ID %s not found for write.", spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));
        return XSAN_ERROR_NOT_FOUND;
    }

    pthread_mutex_lock(&vm->lock); // Lock to read vol->state and vol->replica_nodes safely
    xsan_storage_state_t current_vol_state = vol->state;
    uint32_t current_actual_replica_count = vol->actual_replica_count;
    xsan_replica_location_t replica_locations_copy[XSAN_MAX_REPLICAS];
    if (current_actual_replica_count > 0 && current_actual_replica_count <= XSAN_MAX_REPLICAS) {
         memcpy(replica_locations_copy, vol->replica_nodes, sizeof(xsan_replica_location_t) * current_actual_replica_count);
    }
    uint32_t vol_block_size = vol->block_size_bytes;
    uint64_t vol_size_bytes = vol->size_bytes;
    char vol_name_copy[XSAN_MAX_NAME_LEN];
    xsan_strcpy_safe(vol_name_copy, vol->name, XSAN_MAX_NAME_LEN);
    pthread_mutex_unlock(&vm->lock);


    if (current_vol_state == XSAN_STORAGE_STATE_OFFLINE || current_vol_state == XSAN_STORAGE_STATE_FAILED) {
        XSAN_LOG_ERROR("Cannot write to volume '%s' (ID: %s), state is %d.",
                       vol_name_copy, spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), current_vol_state);
        return XSAN_ERROR_RESOURCE_UNAVAILABLE;
    }
    if (vol_block_size == 0 ||
        (logical_byte_offset % vol_block_size != 0) ||
        (length_bytes % vol_block_size != 0) ||
        (logical_byte_offset + length_bytes > vol_size_bytes)) {
        XSAN_LOG_ERROR("Write params invalid for vol %s: offset %lu, len %lu, vol_size %lu, blk_size %u",
                        vol_name_copy, logical_byte_offset, length_bytes, vol_size_bytes, vol_block_size);
        return XSAN_ERROR_INVALID_PARAM_ALIGNMENT;
    }

    if (current_actual_replica_count == 0) {
        XSAN_LOG_ERROR("Volume '%s' (ID: %s) has no replicas configured for write.",
                       vol_name_copy, spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));
        return XSAN_ERROR_REPLICATION_UNAVAILABLE;
    }

    static uint64_t s_wtid_ctr = 1000;
    uint64_t transaction_id = __sync_fetch_and_add(&s_wtid_ctr, 1);

    // Pass the original vol pointer to create, it will be used to read FTT, etc.
    // The create function should not store it long-term if vol can be freed.
    // Here, vol is from get_by_id, so it's a pointer to internal list, assumed valid.
    xsan_replicated_io_ctx_t *rep_ctx = xsan_replicated_io_ctx_create(
        user_cb, user_cb_arg, vol, user_buf, logical_byte_offset, length_bytes, transaction_id);

    if (!rep_ctx) {
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    pthread_mutex_lock(&vm->pending_ios_lock);
    if (xsan_hashtable_put(vm->pending_replicated_ios, &rep_ctx->transaction_id, rep_ctx) != XSAN_OK) {
        pthread_mutex_unlock(&vm->pending_ios_lock);
        XSAN_LOG_ERROR("Failed to add TID %lu to pending replicated IOs hashtable.", transaction_id);
        xsan_replicated_io_ctx_free(rep_ctx);
        return XSAN_ERROR_OUT_OF_MEMORY;
    }
    pthread_mutex_unlock(&vm->pending_ios_lock);

    XSAN_LOG_DEBUG("Starting replicated write for vol %s, TID %lu, offset %lu, len %lu, replicas %u",
                   spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), transaction_id,
                   logical_byte_offset, length_bytes, current_actual_replica_count);

    xsan_error_t submit_status;
    bool at_least_one_submission_attempted = false;

    for (uint32_t i = 0; i < current_actual_replica_count; ++i) {
        xsan_replica_location_t *current_replica_loc = &replica_locations_copy[i];
        bool should_attempt_write_to_replica = false;

        if (current_replica_loc->state == XSAN_STORAGE_STATE_ONLINE || current_replica_loc->state == XSAN_STORAGE_STATE_DEGRADED) {
            should_attempt_write_to_replica = true;
        } else {
             XSAN_LOG_WARN("Replica %u (NodeID: %s) for vol %s (TID %lu) is not in a writable state (state %d). Skipping write attempt to this replica.",
                          i, spdk_uuid_get_string((struct spdk_uuid*)&current_replica_loc->node_id.data[0]),
                          spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), transaction_id, current_replica_loc->state);
        }

        if (!should_attempt_write_to_replica) {
            // Simulate an immediate failure for this replica path to ensure rep_ctx is correctly managed.
            // This is a simplification. A more robust system might have different handling.
            if (i == 0) { // Local
                 _xsan_local_replica_write_complete_cb(rep_ctx, XSAN_ERROR_RESOURCE_UNAVAILABLE);
            } else { // Remote
                // Create a dummy context just to call the failure path of send_actual_cb
                xsan_per_replica_op_ctx_t *dummy_remote_ctx = XSAN_MALLOC(sizeof(xsan_per_replica_op_ctx_t));
                if (dummy_remote_ctx) {
                    memset(dummy_remote_ctx, 0, sizeof(xsan_per_replica_op_ctx_t));
                    dummy_remote_ctx->parent_rep_ctx = rep_ctx;
                    memcpy(&dummy_remote_ctx->replica_location_info, current_replica_loc, sizeof(xsan_replica_location_t));
                    _xsan_remote_replica_request_send_actual_cb(XSAN_ERROR_RESOURCE_UNAVAILABLE, dummy_remote_ctx);
                    // _xsan_remote_replica_request_send_actual_cb will free dummy_remote_ctx.
                } else { // Failed to alloc dummy, just count as failed write for rep_ctx
                    __sync_fetch_and_add(&rep_ctx->failed_writes, 1);
                     if (rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = XSAN_ERROR_RESOURCE_UNAVAILABLE;
                     _xsan_check_replicated_write_completion(rep_ctx); // Check if this completes the whole op
                }
            }
            continue;
        }
        at_least_one_submission_attempted = true;

        if (i == 0) {
            submit_status = _xsan_volume_submit_single_io_attempt(
                vm, volume_id, logical_byte_offset, length_bytes,
                (void*)user_buf,
                false,
                _xsan_local_replica_write_complete_cb,
                rep_ctx);

            if (submit_status != XSAN_OK) {
                XSAN_LOG_ERROR("Failed to submit local write for vol %s, TID %lu: %s",
                               spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), transaction_id, xsan_error_string(submit_status));
                _xsan_local_replica_write_complete_cb(rep_ctx, submit_status);
            }
        } else {
            xsan_per_replica_op_ctx_t *remote_op_ctx = XSAN_MALLOC(sizeof(xsan_per_replica_op_ctx_t));
            if (!remote_op_ctx) {
                 XSAN_LOG_ERROR("Failed to allocate per_replica_op_ctx for vol %s, TID %lu, replica %u",
                               spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), transaction_id, i);
                __sync_fetch_and_add(&rep_ctx->failed_writes, 1);
                if (rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = XSAN_ERROR_OUT_OF_MEMORY;
                _xsan_check_replicated_write_completion(rep_ctx);
                continue;
            }
            memset(remote_op_ctx, 0, sizeof(xsan_per_replica_op_ctx_t));
            remote_op_ctx->parent_rep_ctx = rep_ctx;
            memcpy(&remote_op_ctx->replica_location_info, current_replica_loc, sizeof(xsan_replica_location_t));

            xsan_replica_write_req_payload_t write_req_pl;
            memcpy(&write_req_pl.volume_id, &volume_id, sizeof(xsan_volume_id_t));
            write_req_pl.block_lba_on_volume = logical_byte_offset / vol_block_size;
            write_req_pl.num_blocks = length_bytes / vol_block_size;

            remote_op_ctx->request_msg_to_send = xsan_protocol_message_create_with_data(
                XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ,
                transaction_id,
                &write_req_pl, sizeof(write_req_pl),
                user_buf, length_bytes);

            if (!remote_op_ctx->request_msg_to_send) {
                XSAN_LOG_ERROR("Failed to create replica write message for vol %s, TID %lu, replica %u",
                               spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), transaction_id, i);
                XSAN_FREE(remote_op_ctx);
                __sync_fetch_and_add(&rep_ctx->failed_writes, 1);
                if (rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = XSAN_ERROR_OUT_OF_MEMORY;
                 _xsan_check_replicated_write_completion(rep_ctx);
                continue;
            }

            struct spdk_sock *sock = xsan_node_comm_get_active_connection(
                current_replica_loc->node_ip_addr, current_replica_loc->node_comm_port);

            if (sock) {
                _xsan_remote_replica_connect_then_send_cb(sock, 0, remote_op_ctx);
            } else {
                xsan_node_comm_connect(current_replica_loc->node_ip_addr,
                                       current_replica_loc->node_comm_port,
                                       _xsan_remote_replica_connect_then_send_cb,
                                       remote_op_ctx);
            }
        }
    }

    if (!at_least_one_submission_attempted && current_actual_replica_count > 0) {
        XSAN_LOG_ERROR("Vol %s, TID %lu: No replicas were in a state to attempt writes.",
            spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]), transaction_id);
        rep_ctx->final_status = XSAN_ERROR_REPLICATION_UNAVAILABLE;
        for(uint32_t k=0; k < rep_ctx->total_replicas_targeted; ++k) {
            __sync_fetch_and_add(&rep_ctx->failed_writes, 1);
        }
        _xsan_check_replicated_write_completion(rep_ctx);
        return XSAN_ERROR_REPLICATION_UNAVAILABLE;
    } else if (current_actual_replica_count == 0) { // Should have been caught earlier
         xsan_replicated_io_ctx_free(rep_ctx);
         return XSAN_ERROR_REPLICATION_UNAVAILABLE;
    }


    return XSAN_OK;
}
