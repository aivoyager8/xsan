#include "xsan_volume_manager.h"
#include "xsan_disk_manager.h"
#include "xsan_storage.h"
#include "xsan_memory.h"
#include "xsan_list.h"
#include "xsan_string_utils.h"
#include "xsan_log.h"
#include "xsan_error.h"
#include "xsan_io.h"
#include "xsan_replication.h"  // For xsan_replicated_io_ctx_t
#include "xsan_node_comm.h"    // For xsan_node_comm_send_msg
#include "xsan_protocol.h"     // For XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ
#include "xsan_metadata_store.h"
#include "json-c/json.h"

#include "spdk/uuid.h"
#include "spdk/thread.h"
#include <pthread.h>
#include <errno.h> // For ECONNREFUSED

// --- Defines for RocksDB Keys ---
#define XSAN_VOLUME_META_PREFIX "v:"

struct xsan_volume_manager {
    xsan_list_t *managed_volumes;
    xsan_disk_manager_t *disk_manager;
    pthread_mutex_t lock; // Protects managed_volumes list primarily
    bool initialized;
    xsan_metadata_store_t *md_store;
    char metadata_db_path[XSAN_MAX_PATH_LEN];
    // For Step 5: Managing Pending Replicated I/Os
    xsan_hashtable_t *pending_replicated_ios; // Maps transaction_id (uint64_t*) to xsan_replicated_io_ctx_t*
    pthread_mutex_t pending_ios_lock;         // Separate lock for this map
};

static xsan_volume_manager_t *g_xsan_volume_manager_instance = NULL;

// Forward declarations
static xsan_error_t xsan_volume_manager_load_metadata(xsan_volume_manager_t *vm);
static xsan_error_t xsan_volume_manager_save_volume_meta(xsan_volume_manager_t *vm, xsan_volume_t *vol);
static xsan_error_t xsan_volume_manager_delete_volume_meta(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id);
static xsan_error_t _xsan_volume_to_json_string(const xsan_volume_t *vol, char **json_string_out);
static xsan_error_t _xsan_json_string_to_volume(const char *json_string, xsan_volume_manager_t *vm, xsan_volume_t **vol_out);
static xsan_error_t _xsan_volume_submit_async_io(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id, uint64_t logical_byte_offset, uint64_t length_bytes, void *user_buf, bool is_read_op, xsan_user_io_completion_cb_t user_cb, void *user_cb_arg);
static void _xsan_check_replicated_write_completion(xsan_replicated_io_ctx_t *rep_ctx);
static void _xsan_local_replica_write_complete_cb(void *cb_arg, xsan_error_t status);
static void _xsan_remote_replica_request_send_complete_cb(int comm_status, void *cb_arg);

// --- Hash functions for uint64_t keys for pending_replicated_ios map ---
static uint32_t uint64_tid_hash_func(const void *key) {
    if (!key) return 0;
    uint64_t val = *(const uint64_t *)key;
    val = (~val) + (val << 21); val = val ^ (val >> 24);
    val = (val + (val << 3)) + (val << 8); val = val ^ (val >> 14);
    val = (val + (val << 2)) + (val << 4); val = val ^ (val >> 28);
    val = val + (val << 31);
    return (uint32_t)val;
}
static int uint64_tid_key_compare_func(const void *key1, const void *key2) {
    if (key1 == key2) return 0; if (!key1) return -1; if (!key2) return 1;
    uint64_t val1 = *(const uint64_t *)key1; uint64_t val2 = *(const uint64_t *)key2;
    if (val1 < val2) return -1; if (val1 > val2) return 1; return 0;
}
// Key for hashtable will be rep_ctx->transaction_id. We need to store a pointer to it.
// The key_destroy_func for the hashtable should be NULL as the transaction_id is part of rep_ctx.
// The value_destroy_func for the hashtable should also be NULL as rep_ctx is freed by its own lifecycle.

static void _xsan_internal_volume_destroy_cb(void *volume_data) { /* ... as before ... */
    if (volume_data) { xsan_volume_t *v = (xsan_volume_t *)volume_data; XSAN_FREE(v); }
}

xsan_error_t xsan_volume_manager_init(xsan_disk_manager_t *disk_manager, xsan_volume_manager_t **vm_instance_out) {
    const char *db_path_suffix = "xsan_meta_db/volume_manager";
    char actual_db_path[XSAN_MAX_PATH_LEN];
    snprintf(actual_db_path, sizeof(actual_db_path), "./%s", db_path_suffix);

    if (g_xsan_volume_manager_instance) { /* ... return OK ... */ if(vm_instance_out) *vm_instance_out = g_xsan_volume_manager_instance; return XSAN_OK; }
    if (!disk_manager) { /* ... return error ... */ if(vm_instance_out) *vm_instance_out = NULL; return XSAN_ERROR_INVALID_PARAM; }

    XSAN_LOG_INFO("Initializing Volume Manager (DB: %s)...", actual_db_path);
    xsan_volume_manager_t *vm = (xsan_volume_manager_t *)XSAN_MALLOC(sizeof(xsan_volume_manager_t));
    if (!vm) { /* ... return error ... */ return XSAN_ERROR_OUT_OF_MEMORY; }
    memset(vm, 0, sizeof(xsan_volume_manager_t));
    xsan_strcpy_safe(vm->metadata_db_path, actual_db_path, XSAN_MAX_PATH_LEN);

    vm->managed_volumes = xsan_list_create(_xsan_internal_volume_destroy_cb);
    if (!vm->managed_volumes) { /* ... cleanup, error ... */ XSAN_FREE(vm); return XSAN_ERROR_OUT_OF_MEMORY; }
    if (pthread_mutex_init(&vm->lock, NULL) != 0) { /* ... cleanup, error ... */ xsan_list_destroy(vm->managed_volumes); XSAN_FREE(vm); return XSAN_ERROR_SYSTEM; }
    if (pthread_mutex_init(&vm->pending_ios_lock, NULL) != 0) { /* ... cleanup ... */ pthread_mutex_destroy(&vm->lock); xsan_list_destroy(vm->managed_volumes); XSAN_FREE(vm); return XSAN_ERROR_SYSTEM; }

    vm->pending_replicated_ios = xsan_hashtable_create(256, uint64_tid_hash_func, uint64_tid_key_compare_func, NULL, NULL);
    if (!vm->pending_replicated_ios) { /* ... cleanup ... */ pthread_mutex_destroy(&vm->pending_ios_lock); pthread_mutex_destroy(&vm->lock); xsan_list_destroy(vm->managed_volumes); XSAN_FREE(vm); return XSAN_ERROR_OUT_OF_MEMORY;}

    vm->disk_manager = disk_manager;
    vm->md_store = xsan_metadata_store_open(vm->metadata_db_path, true);
    if (!vm->md_store) { /* ... cleanup ... */ xsan_hashtable_destroy(vm->pending_replicated_ios); pthread_mutex_destroy(&vm->pending_ios_lock); pthread_mutex_destroy(&vm->lock); xsan_list_destroy(vm->managed_volumes); XSAN_FREE(vm); return XSAN_ERROR_STORAGE_GENERIC;}

    vm->initialized = true; g_xsan_volume_manager_instance = vm; if (vm_instance_out) *vm_instance_out = vm;
    xsan_volume_manager_load_metadata(vm); XSAN_LOG_INFO("Volume Manager initialized."); return XSAN_OK;
}

void xsan_volume_manager_fini(xsan_volume_manager_t **vm_ptr) {
    xsan_volume_manager_t *vm = (vm_ptr && *vm_ptr) ? *vm_ptr : g_xsan_volume_manager_instance;
    if (!vm || !vm->initialized) { if(vm_ptr) *vm_ptr = NULL; g_xsan_volume_manager_instance = NULL; return; }
    XSAN_LOG_INFO("Finalizing Volume Manager...");

    pthread_mutex_lock(&vm->pending_ios_lock);
    if (vm->pending_replicated_ios) {
        if (xsan_hashtable_size(vm->pending_replicated_ios) > 0) {
             XSAN_LOG_WARN("Volume Manager fini: %zu replicated IOs still pending. These will be orphaned.", xsan_hashtable_size(vm->pending_replicated_ios));
             // Iterate and free contexts if hashtable doesn't own them (it doesn't via NULL value_destroy_func)
            xsan_hashtable_iter_t iter; xsan_hashtable_iter_init(vm->pending_replicated_ios, &iter);
            void *key_ptr, *val_ptr;
            while(xsan_hashtable_iter_next(&iter, &key_ptr, &val_ptr)) {
                if(val_ptr) xsan_replicated_io_ctx_free((xsan_replicated_io_ctx_t*)val_ptr);
            }
        }
        xsan_hashtable_destroy(vm->pending_replicated_ios); vm->pending_replicated_ios = NULL;
    }
    pthread_mutex_unlock(&vm->pending_ios_lock);
    pthread_mutex_destroy(&vm->pending_ios_lock);

    pthread_mutex_lock(&vm->lock);
    xsan_list_destroy(vm->managed_volumes); vm->managed_volumes = NULL;
    if (vm->md_store) xsan_metadata_store_close(vm->md_store); vm->md_store = NULL;
    vm->initialized = false; pthread_mutex_unlock(&vm->lock); pthread_mutex_destroy(&vm->lock);
    XSAN_FREE(vm); if (vm_ptr) *vm_ptr = NULL; if (vm == g_xsan_volume_manager_instance) g_xsan_volume_manager_instance = NULL;
    XSAN_LOG_INFO("Volume Manager finalized.");
}

static xsan_error_t _xsan_volume_to_json_string(const xsan_volume_t *vol, char **json_string_out) { /* ... as before ... */
    if (!vol || !json_string_out) return XSAN_ERROR_INVALID_PARAM;
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
        json_object_array_add(jarray_replicas, jreplica);
    }
    json_object_object_add(jobj, "replica_nodes", jarray_replicas);
    const char *tmp_str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    *json_string_out = xsan_strdup(tmp_str); json_object_put(jobj);
    return (*json_string_out) ? XSAN_OK : XSAN_ERROR_OUT_OF_MEMORY;
}
static xsan_error_t _xsan_json_string_to_volume(const char *json_string, xsan_volume_manager_t *vm, xsan_volume_t **vol_out) { /* ... as before ... */
    (void)vm; if (!json_string || !vol_out) return XSAN_ERROR_INVALID_PARAM;
    struct json_object *jobj = json_tokener_parse(json_string); if (!jobj || is_error(jobj)) { if(jobj&&!is_error(jobj))json_object_put(jobj);return XSAN_ERROR_CONFIG_PARSE; }
    xsan_volume_t *vol = (xsan_volume_t *)XSAN_MALLOC(sizeof(xsan_volume_t)); if (!vol) { json_object_put(jobj); return XSAN_ERROR_OUT_OF_MEMORY; }
    memset(vol, 0, sizeof(xsan_volume_t)); struct json_object *val;
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
        if(vol->actual_replica_count != (uint32_t)arr_len) { vol->actual_replica_count = arr_len; }
        for (uint32_t i = 0; i < vol->actual_replica_count; ++i) {
            struct json_object *j_repl_node = json_object_array_get_idx(j_repl_nodes, i);
            if (j_repl_node && json_object_is_type(j_repl_node, json_type_object)) {
                if (json_object_object_get_ex(j_repl_node, "node_id", &val)) spdk_uuid_parse((struct spdk_uuid*)&vol->replica_nodes[i].node_id.data[0], json_object_get_string(val));
                if (json_object_object_get_ex(j_repl_node, "node_ip_addr", &val)) xsan_strcpy_safe(vol->replica_nodes[i].node_ip_addr, json_object_get_string(val), INET6_ADDRSTRLEN);
                if (json_object_object_get_ex(j_repl_node, "node_comm_port", &val)) vol->replica_nodes[i].node_comm_port = (uint16_t)json_object_get_int(val);
            }
        }
    }
    json_object_put(jobj); *vol_out = vol; return XSAN_OK;
}
static xsan_error_t xsan_volume_manager_save_volume_meta(xsan_volume_manager_t *vm, xsan_volume_t *vol) { /* ... as before ... */
    if (!vm || !vol || !vm->md_store) return XSAN_ERROR_INVALID_PARAM;
    char k[256]; char id_s[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(id_s,sizeof(id_s),(struct spdk_uuid*)&vol->id.data[0]);
    snprintf(k,sizeof(k),"%s%s",XSAN_VOLUME_META_PREFIX,id_s); char *v_s=NULL;
    xsan_error_t e=_xsan_volume_to_json_string(vol,&v_s); if(e!=XSAN_OK)return e;
    e=xsan_metadata_store_put(vm->md_store,k,strlen(k),v_s,strlen(v_s)); XSAN_FREE(v_s); return e;
}
static xsan_error_t xsan_volume_manager_delete_volume_meta(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id) { /* ... as before ... */
    if (!vm || !vm->md_store) return XSAN_ERROR_INVALID_PARAM;
    char k[256]; char id_s[SPDK_UUID_STRING_LEN]; spdk_uuid_fmt_lower(id_s,sizeof(id_s),(struct spdk_uuid*)&volume_id.data[0]);
    snprintf(k,sizeof(k),"%s%s",XSAN_VOLUME_META_PREFIX,id_s);
    return xsan_metadata_store_delete(vm->md_store,k,strlen(k));
}
xsan_error_t xsan_volume_manager_load_metadata(xsan_volume_manager_t *vm) { /* ... as before ... */
    if (!vm || !vm->initialized || !vm->md_store) return XSAN_ERROR_INVALID_PARAM;
    XSAN_LOG_INFO("Loading volume metadata from: %s", vm->metadata_db_path); pthread_mutex_lock(&vm->lock);
    xsan_metadata_iterator_t *it = xsan_metadata_iterator_create(vm->md_store); if(!it){pthread_mutex_unlock(&vm->lock); return XSAN_ERROR_STORAGE_GENERIC;}
    xsan_metadata_iterator_seek(it, XSAN_VOLUME_META_PREFIX, strlen(XSAN_VOLUME_META_PREFIX));
    while(xsan_metadata_iterator_is_valid(it)){
        size_t kl,vl; const char *k=xsan_metadata_iterator_key(it,&kl); if(!k || strncmp(k,XSAN_VOLUME_META_PREFIX,strlen(XSAN_VOLUME_META_PREFIX))!=0)break;
        const char *v_s=xsan_metadata_iterator_value(it,&vl); xsan_volume_t *vol=NULL;
        if(_xsan_json_string_to_volume(v_s,vm,&vol)==XSAN_OK && vol){
            bool ex=false; xsan_list_node_t *ln; XSAN_LIST_FOREACH(vm->managed_volumes,ln){if(spdk_uuid_compare((struct spdk_uuid*)&((xsan_volume_t*)xsan_list_node_get_value(ln))->id.data[0],(struct spdk_uuid*)&vol->id.data[0])==0){ex=true;break;}}
            if(!ex) xsan_list_append(vm->managed_volumes,vol); else {XSAN_LOG_WARN("Vol ID %s from meta already in mem.",spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0])); XSAN_FREE(vol);}
        } else {XSAN_LOG_ERROR("Failed deserialize vol from: %.*s",(int)vl,v_s);}
        xsan_metadata_iterator_next(it);
    } xsan_metadata_iterator_destroy(it); pthread_mutex_unlock(&vm->lock); XSAN_LOG_INFO("Volume metadata loading done."); return XSAN_OK;
}

xsan_error_t xsan_volume_create( /* ... FTT added ... */ ) { /* ... as before, with FTT and replica_nodes population ... */
    // (Full implementation from previous step, including FTT handling)
    if (!vm || !vm->initialized || !name || size_bytes == 0 || logical_block_size_bytes == 0 ||
        (logical_block_size_bytes & (logical_block_size_bytes - 1)) != 0 ||
        spdk_uuid_is_null((struct spdk_uuid*)&group_id.data[0]) ||
        (ftt + 1) > XSAN_MAX_REPLICAS ) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if ((size_bytes % logical_block_size_bytes) != 0) return XSAN_ERROR_INVALID_PARAM;
    pthread_mutex_lock(&vm->lock); xsan_error_t err = XSAN_OK;
    xsan_list_node_t *iter_node; XSAN_LIST_FOREACH(vm->managed_volumes, iter_node) { if (strncmp(((xsan_volume_t*)xsan_list_node_get_value(iter_node))->name, name, XSAN_MAX_NAME_LEN) == 0) { pthread_mutex_unlock(&vm->lock); return XSAN_ERROR_ALREADY_EXISTS; } }
    xsan_disk_group_t *group = xsan_disk_manager_find_disk_group_by_id(vm->disk_manager, group_id);
    if (!group || group->state != XSAN_STORAGE_STATE_ONLINE) { err = !group ? XSAN_ERROR_NOT_FOUND : XSAN_ERROR_RESOURCE_BUSY; goto create_vol_unlock_exit; }
    if (!thin_provisioned && size_bytes > group->usable_capacity_bytes) { err = XSAN_ERROR_INSUFFICIENT_SPACE; goto create_vol_unlock_exit; }
    xsan_volume_t *new_volume = (xsan_volume_t *)XSAN_MALLOC(sizeof(xsan_volume_t));
    if (!new_volume) { err = XSAN_ERROR_OUT_OF_MEMORY; goto create_vol_unlock_exit; }
    memset(new_volume, 0, sizeof(xsan_volume_t));
    spdk_uuid_generate((struct spdk_uuid *)&new_volume->id.data[0]);
    xsan_strcpy_safe(new_volume->name, name, XSAN_MAX_NAME_LEN);
    new_volume->size_bytes = size_bytes; new_volume->block_size_bytes = logical_block_size_bytes;
    new_volume->num_blocks = size_bytes / logical_block_size_bytes;
    new_volume->state = XSAN_STORAGE_STATE_ONLINE;
    memcpy(&new_volume->source_group_id, &group_id, sizeof(xsan_group_id_t));
    new_volume->thin_provisioned = thin_provisioned;
    new_volume->allocated_bytes = thin_provisioned ? 0 : size_bytes;
    new_volume->FTT = ftt; new_volume->actual_replica_count = 0;
    uint32_t target_total_replicas = ftt + 1; if (target_total_replicas > XSAN_MAX_REPLICAS) target_total_replicas = XSAN_MAX_REPLICAS;
    if (target_total_replicas > 0) {
        xsan_node_id_t self_id_placeholder; spdk_uuid_generate((struct spdk_uuid*)&self_id_placeholder.data[0]);
        memcpy(&new_volume->replica_nodes[0].node_id, &self_id_placeholder, sizeof(xsan_node_id_t));
        xsan_strcpy_safe(new_volume->replica_nodes[0].node_ip_addr, "127.0.0.1", INET6_ADDRSTRLEN);
        new_volume->replica_nodes[0].node_comm_port = 7777;
        new_volume->actual_replica_count = 1;
        for (uint32_t i = 1; i < target_total_replicas; ++i) { XSAN_LOG_WARN("Vol '%s': Remote replica %u assignment placeholder.", new_volume->name, i); }
        if (new_volume->actual_replica_count < target_total_replicas && ftt > 0) { XSAN_LOG_WARN("Vol '%s': Configured %u replicas, FTT %u needs %u.", new_volume->name, new_volume->actual_replica_count, ftt, target_total_replicas); }
    }
    if (xsan_list_append(vm->managed_volumes, new_volume) == NULL) { XSAN_FREE(new_volume); err = XSAN_ERROR_OUT_OF_MEMORY; goto create_vol_unlock_exit; }
    if (new_volume_id_out) memcpy(new_volume_id_out, &new_volume->id, sizeof(xsan_volume_id_t));
    err = xsan_volume_manager_save_volume_meta(vm, new_volume); if(err!=XSAN_OK) XSAN_LOG_ERROR("Failed save meta for new vol '%s'.",new_volume->name);
create_vol_unlock_exit: pthread_mutex_unlock(&vm->lock); return err;
}

xsan_error_t xsan_volume_delete(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id) { /* ... as before ... */
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) return XSAN_ERROR_INVALID_PARAM;
    pthread_mutex_lock(&vm->lock); xsan_error_t err = XSAN_ERROR_NOT_FOUND; xsan_list_node_t *ln = xsan_list_get_head(vm->managed_volumes); xsan_volume_t *vol_del = NULL;
    while(ln) { xsan_volume_t *v=(xsan_volume_t*)xsan_list_node_get_value(ln); if(spdk_uuid_compare((struct spdk_uuid*)&v->id.data[0],(struct spdk_uuid*)&volume_id.data[0])==0){vol_del=v;break;} ln=xsan_list_node_next(ln);}
    if(vol_del){ xsan_list_remove_node(vm->managed_volumes, ln); err=xsan_volume_manager_delete_volume_meta(vm,volume_id); if(err!=XSAN_OK){XSAN_LOG_ERROR("Vol ID %s deleted from mem, but meta delete failed.",spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));} else {XSAN_LOG_INFO("Vol ID %s and meta deleted.",spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));} err=XSAN_OK;}
    else {XSAN_LOG_WARN("Vol ID %s not found for deletion.",spdk_uuid_get_string((struct spdk_uuid*)&volume_id.data[0]));}
    pthread_mutex_unlock(&vm->lock); return err;
}
xsan_volume_t *xsan_volume_get_by_id(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id) { /* ... as before ... */
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0])) return NULL;
    pthread_mutex_lock(&vm->lock); xsan_list_node_t *n; xsan_volume_t *found=NULL; XSAN_LIST_FOREACH(vm->managed_volumes,n){xsan_volume_t *v=(xsan_volume_t*)xsan_list_node_get_value(n); if(spdk_uuid_compare((struct spdk_uuid*)&v->id.data[0],(struct spdk_uuid*)&volume_id.data[0])==0){found=v;break;}} pthread_mutex_unlock(&vm->lock); return found;
}
xsan_volume_t *xsan_volume_get_by_name(xsan_volume_manager_t *vm, const char *name) { /* ... as before ... */
    if (!vm || !vm->initialized || !name) return NULL;
    pthread_mutex_lock(&vm->lock); xsan_list_node_t *n; xsan_volume_t *found=NULL; XSAN_LIST_FOREACH(vm->managed_volumes,n){xsan_volume_t *v=(xsan_volume_t*)xsan_list_node_get_value(n); if(strncmp(v->name,name,XSAN_MAX_NAME_LEN)==0){found=v;break;}} pthread_mutex_unlock(&vm->lock); return found;
}
xsan_error_t xsan_volume_list_all(xsan_volume_manager_t *vm, xsan_volume_t ***volumes_array_out, int *count_out) { /* ... as before ... */
    if (!vm || !vm->initialized || !volumes_array_out || !count_out) { if(count_out)*count_out=0; if(volumes_array_out)*volumes_array_out=NULL; return XSAN_ERROR_INVALID_PARAM; }
    pthread_mutex_lock(&vm->lock); size_t num=xsan_list_size(vm->managed_volumes); if(num==0){*volumes_array_out=NULL;*count_out=0;pthread_mutex_unlock(&vm->lock);return XSAN_OK;}
    *volumes_array_out=(xsan_volume_t**)XSAN_MALLOC(sizeof(xsan_volume_t*)*num); if(!*volumes_array_out){*count_out=0;pthread_mutex_unlock(&vm->lock);return XSAN_ERROR_OUT_OF_MEMORY;}
    int i=0; xsan_list_node_t *n; XSAN_LIST_FOREACH(vm->managed_volumes,n){if(i<(int)num){(*volumes_array_out)[i++]=(xsan_volume_t*)xsan_list_node_get_value(n);}else break;}
    *count_out=i; pthread_mutex_unlock(&vm->lock); return XSAN_OK;
}
void xsan_volume_manager_free_volume_pointer_list(xsan_volume_t **volume_ptr_array) { /* ... as before ... */ if(volume_ptr_array)XSAN_FREE(volume_ptr_array); }
xsan_error_t xsan_volume_map_lba_to_physical(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id, uint64_t logical_block_idx, xsan_disk_id_t *out_disk_id, uint64_t *out_physical_block_idx, uint32_t *out_physical_block_size) { /* ... as before ... */
    if (!vm || !vm->initialized || !out_disk_id || !out_physical_block_idx || !out_physical_block_size) return XSAN_ERROR_INVALID_PARAM;
    xsan_volume_t *vol = xsan_volume_get_by_id(vm, volume_id); if (!vol) return XSAN_ERROR_NOT_FOUND;
    if (logical_block_idx >= vol->num_blocks) return XSAN_ERROR_OUT_OF_BOUNDS;
    xsan_disk_group_t *g = xsan_disk_manager_find_disk_group_by_id(vm->disk_manager, vol->source_group_id); if (!g || g->disk_count == 0) return XSAN_ERROR_NOT_FOUND;
    xsan_disk_t *d = xsan_disk_manager_find_disk_by_id(vm->disk_manager, g->disk_ids[0]); if (!d || d->block_size_bytes == 0) return XSAN_ERROR_NOT_FOUND;
    *out_physical_block_size = d->block_size_bytes; *out_physical_block_idx = (logical_block_idx * vol->block_size_bytes) / d->block_size_bytes;
    uint64_t phys_blks_needed = (vol->block_size_bytes + d->block_size_bytes - 1) / d->block_size_bytes;
    if ((*out_physical_block_idx + phys_blks_needed -1) >= d->num_blocks) return XSAN_ERROR_OUT_OF_BOUNDS;
    memcpy(out_disk_id, &d->id, sizeof(xsan_disk_id_t)); return XSAN_OK;
}

// --- Internal Callbacks and Logic for Replicated Write ---
static void _xsan_check_replicated_write_completion(xsan_replicated_io_ctx_t *rep_ctx) {
    if (!rep_ctx) return;
    uint32_t completed_count = __sync_add_and_fetch(&rep_ctx->successful_writes, 0) +
                               __sync_add_and_fetch(&rep_ctx->failed_writes, 0);
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&rep_ctx->volume_id.data[0]);

    XSAN_LOG_DEBUG("CheckRepWrite Vol %s (TID %lu): Target %u, Done %u (S:%u, F:%u)",
                   vol_id_str, rep_ctx->transaction_id, rep_ctx->total_replicas_targeted,
                   completed_count, rep_ctx->successful_writes, rep_ctx->failed_writes);

    if (completed_count >= rep_ctx->total_replicas_targeted) {
        if (rep_ctx->successful_writes >= rep_ctx->total_replicas_targeted) { // Strict: all must succeed
            rep_ctx->final_status = XSAN_OK;
        } else {
            if (rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = XSAN_ERROR_REPLICATION_GENERIC;
        }
        if (rep_ctx->final_status == XSAN_OK) {
             XSAN_LOG_INFO("Replicated write for Vol %s (TID %lu) SUCCEEDED.", vol_id_str, rep_ctx->transaction_id);
        } else {
            XSAN_LOG_ERROR("Replicated write for Vol %s (TID %lu) FAILED. Final Status: %d (%s)",
                           vol_id_str, rep_ctx->transaction_id, rep_ctx->final_status, xsan_error_string(rep_ctx->final_status));
        }
        if (rep_ctx->original_user_cb) {
            rep_ctx->original_user_cb(rep_ctx->original_user_cb_arg, rep_ctx->final_status);
        }
        // TODO: Remove rep_ctx from g_xsan_volume_manager_instance->pending_replicated_ios map.
        xsan_replicated_io_ctx_free(rep_ctx);
    }
}

static void _xsan_local_replica_write_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_replicated_io_ctx_t *rep_ctx = (xsan_replicated_io_ctx_t *)cb_arg;
    if (!rep_ctx) { XSAN_LOG_ERROR("NULL rep_ctx in local replica write completion."); return; }
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&rep_ctx->volume_id.data[0]);
    XSAN_LOG_DEBUG("Local replica write for Vol %s (TID %lu) completed: status %d", vol_id_str, rep_ctx->transaction_id, status);
    if (status == XSAN_OK) __sync_fetch_and_add(&rep_ctx->successful_writes, 1);
    else { __sync_fetch_and_add(&rep_ctx->failed_writes, 1); if (rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = status; }
    rep_ctx->local_io_req = NULL;
    _xsan_check_replicated_write_completion(rep_ctx);
}

static void _xsan_remote_replica_request_send_complete_cb(int comm_status, void *cb_arg) {
    xsan_replicated_io_ctx_t *rep_ctx = (xsan_replicated_io_ctx_t *)cb_arg;
    if (!rep_ctx) { XSAN_LOG_ERROR("NULL rep_ctx in remote replica send completion."); return; }
    char vol_id_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(vol_id_str, sizeof(vol_id_str), (struct spdk_uuid*)&rep_ctx->volume_id.data[0]);
    if (comm_status != 0) {
        XSAN_LOG_ERROR("Send REPLICA_WRITE_BLOCK_REQ for vol %s (TID %lu) FAILED, comm_status: %d (%s)",
                       vol_id_str, rep_ctx->transaction_id, comm_status, strerror(-comm_status));
        __sync_fetch_and_add(&rep_ctx->failed_writes, 1);
        if (rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = xsan_error_from_errno(-comm_status);
        _xsan_check_replicated_write_completion(rep_ctx);
    } else {
        XSAN_LOG_DEBUG("REPLICA_WRITE_BLOCK_REQ for vol %s (TID %lu) sent. Awaiting remote write RESP.",
                       vol_id_str, rep_ctx->transaction_id);
    }
}

void xsan_volume_manager_process_replica_write_response(
    xsan_volume_manager_t *vm, uint64_t transaction_id,
    xsan_node_id_t responding_node_id, xsan_error_t replica_op_status) {
    if (!vm || !vm->initialized) return;
    XSAN_LOG_DEBUG("Processing REPLICA_WRITE_BLOCK_RESP for TID %lu from node %s, status %d",
                   transaction_id, spdk_uuid_get_string((struct spdk_uuid*)&responding_node_id.data[0]), replica_op_status);

    pthread_mutex_lock(&vm->pending_ios_lock);
    xsan_replicated_io_ctx_t *rep_ctx = (xsan_replicated_io_ctx_t *)xsan_hashtable_get(vm->pending_replicated_ios, &transaction_id);
    if (rep_ctx) {
        // We don't remove it yet, _xsan_check_replicated_write_completion will remove after all ops done.
        if (replica_op_status == XSAN_OK) {
            __sync_fetch_and_add(&rep_ctx->successful_writes, 1);
        } else {
            __sync_fetch_and_add(&rep_ctx->failed_writes, 1);
            if (rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = replica_op_status;
        }
        _xsan_check_replicated_write_completion(rep_ctx);
    } else {
        XSAN_LOG_WARN("No pending replicated IO context found for TID %lu from node %s (or already completed/timed out).",
                      transaction_id, spdk_uuid_get_string((struct spdk_uuid*)&responding_node_id.data[0]));
    }
    pthread_mutex_unlock(&vm->pending_ios_lock);
}

xsan_error_t xsan_volume_read_async(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id, uint64_t logical_byte_offset, uint64_t length_bytes, void *user_buf, xsan_user_io_completion_cb_t user_cb, void *user_cb_arg) {
    return _xsan_volume_submit_async_io(vm, volume_id, logical_byte_offset, length_bytes, user_buf, true, user_cb, user_cb_arg);
}

xsan_error_t xsan_volume_write_async(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id, uint64_t logical_byte_offset, uint64_t length_bytes, const void *user_buf, xsan_user_io_completion_cb_t user_cb, void *user_cb_arg) {
    if (!vm || !vm->initialized || spdk_uuid_is_null((struct spdk_uuid*)&volume_id.data[0]) || !user_buf || length_bytes == 0 || !user_cb) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    xsan_volume_t *vol = xsan_volume_get_by_id(vm, volume_id);
    if (!vol) return XSAN_ERROR_NOT_FOUND;
    if (vol->block_size_bytes == 0 || (logical_byte_offset % vol->block_size_bytes != 0) || (length_bytes % vol->block_size_bytes != 0)) return XSAN_ERROR_INVALID_PARAM;
    uint64_t logical_start_block_idx = logical_byte_offset / vol->block_size_bytes;
    uint64_t num_logical_blocks_in_io = length_bytes / vol->block_size_bytes;
    if ((logical_start_block_idx + num_logical_blocks_in_io) > vol->num_blocks) return XSAN_ERROR_OUT_OF_BOUNDS;

    if (vol->FTT == 0 || vol->actual_replica_count <= 1) {
        return _xsan_volume_submit_async_io(vm, volume_id, logical_byte_offset, length_bytes, (void*)user_buf, false, user_cb, user_cb_arg);
    }

    static uint64_t s_tid_counter = 1000;
    uint64_t tid = __sync_fetch_and_add(&s_tid_counter, 1);
    xsan_replicated_io_ctx_t *rep_ctx = xsan_replicated_io_ctx_create(user_cb, user_cb_arg, vol, user_buf, logical_byte_offset, length_bytes, tid);
    if (!rep_ctx) return XSAN_ERROR_OUT_OF_MEMORY;

    pthread_mutex_lock(&vm->pending_ios_lock);
    // Store using pointer to transaction_id within rep_ctx. Hashtable must not free this key.
    if(xsan_hashtable_put(vm->pending_replicated_ios, &rep_ctx->transaction_id, rep_ctx) != XSAN_OK) {
        XSAN_LOG_ERROR("Failed to add TID %lu to pending replicated IO map.", tid);
        pthread_mutex_unlock(&vm->pending_ios_lock);
        xsan_replicated_io_ctx_free(rep_ctx);
        return XSAN_ERROR_OUT_OF_MEMORY; // Or other internal error
    }
    pthread_mutex_unlock(&vm->pending_ios_lock);
    XSAN_LOG_DEBUG("Added rep_ctx TID %lu to pending list for vol %s", tid, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]));


    xsan_disk_id_t phys_disk_id; uint64_t phys_lba; uint32_t phys_bs;
    xsan_error_t map_err = xsan_volume_map_lba_to_physical(vm, volume_id, logical_start_block_idx, &phys_disk_id, &phys_lba, &phys_bs);
    if (map_err != XSAN_OK) { /* remove from pending, free rep_ctx, return */ goto write_async_error_cleanup; }
    xsan_disk_t *local_disk = xsan_disk_manager_find_disk_by_id(vm->disk_manager, phys_disk_id);
    if (!local_disk || !local_disk->bdev_descriptor) { /* remove from pending, free rep_ctx, return */ map_err = XSAN_ERROR_NOT_FOUND; goto write_async_error_cleanup; }

    rep_ctx->local_io_req = xsan_io_request_create(volume_id, (void*)user_buf, phys_lba * phys_bs, length_bytes, phys_bs, false, _xsan_local_replica_write_complete_cb, rep_ctx);
    if (!rep_ctx->local_io_req) { /* remove from pending, free rep_ctx, return */ map_err = XSAN_ERROR_OUT_OF_MEMORY; goto write_async_error_cleanup; }
    memcpy(&rep_ctx->local_io_req->target_disk_id, &local_disk->id, sizeof(xsan_disk_id_t));
    xsan_strcpy_safe(rep_ctx->local_io_req->target_bdev_name, local_disk->bdev_name, XSAN_MAX_NAME_LEN);
    rep_ctx->local_io_req->bdev_desc = local_disk->bdev_descriptor;

    xsan_error_t local_submit_err = xsan_io_submit_request_to_bdev(rep_ctx->local_io_req);
    if (local_submit_err != XSAN_OK) {
        // Callbacks are expected to be called by xsan_io_submit_request_to_bdev even on submit failure,
        // which will lead to _xsan_check_replicated_write_completion and cleanup of rep_ctx from map.
        // So, we don't need to remove from map or free rep_ctx here directly.
        return local_submit_err;
    }

    for (uint32_t i = 1; i < rep_ctx->total_replicas_targeted && i < vol->actual_replica_count; ++i) {
        xsan_replica_location_t *remote_loc = &vol->replica_nodes[i];
        if (spdk_uuid_is_null((struct spdk_uuid*)&remote_loc->node_id.data[0])) {
            __sync_fetch_and_add(&rep_ctx->failed_writes, 1); if(rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = XSAN_ERROR_REPLICATION_GENERIC;
            continue;
        }
        uint32_t data_len = length_bytes; uint32_t struct_payload_len = sizeof(xsan_replica_write_req_payload_t);
        uint32_t total_msg_payload_len = struct_payload_len + data_len;
        unsigned char *full_msg_payload = (unsigned char*)XSAN_MALLOC(total_msg_payload_len);
        if(!full_msg_payload) { __sync_fetch_and_add(&rep_ctx->failed_writes, 1); if(rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = XSAN_ERROR_OUT_OF_MEMORY; continue; }
        xsan_replica_write_req_payload_t *req_pl = (xsan_replica_write_req_payload_t*)full_msg_payload;
        memcpy(&req_pl->volume_id, &vol->id, sizeof(xsan_volume_id_t));
        req_pl->block_lba_on_volume = logical_start_block_idx; req_pl->num_blocks = num_logical_blocks_in_io;
        memcpy(full_msg_payload + struct_payload_len, user_buf, data_len);
        xsan_message_t *rep_msg = xsan_protocol_message_create(XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ, tid, full_msg_payload, total_msg_payload_len);
        XSAN_FREE(full_msg_payload);
        if(!rep_msg) { __sync_fetch_and_add(&rep_ctx->failed_writes, 1); if(rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = XSAN_ERROR_OUT_OF_MEMORY; continue; }

        XSAN_LOG_WARN("Replica Write to %s:%u for vol %s (TID %lu) - Actual send not implemented. Simulating send failure.", remote_loc->node_ip_addr, remote_loc->node_comm_port, vol->name, tid);
        _xsan_remote_replica_request_send_complete_cb(-ECONNREFUSED, rep_ctx);
        xsan_protocol_message_destroy(rep_msg);
    }
    _xsan_check_replicated_write_completion(rep_ctx);
    return XSAN_OK;

write_async_error_cleanup:
    // This path is taken if setup before local IO submission fails.
    pthread_mutex_lock(&vm->pending_ios_lock);
    xsan_hashtable_remove(vm->pending_replicated_ios, &rep_ctx->transaction_id); // Key is &rep_ctx->transaction_id
    pthread_mutex_unlock(&vm->pending_ios_lock);
    xsan_replicated_io_ctx_free(rep_ctx);
    return map_err; // Or the specific error that occurred
}
