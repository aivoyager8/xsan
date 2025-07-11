#include "xsan_volume_manager.h"
#include "xsan_disk_manager.h"
#include "xsan_storage.h"
#include "xsan_memory.h"
#include "xsan_list.h"
#include "xsan_string_utils.h"
#include "xsan_log.h"
#include "xsan_error.h"
#include "xsan_io.h"
#include "xsan_replication.h"
#include "xsan_node_comm.h"
#include "xsan_protocol.h"
#include "xsan_metadata_store.h"
#include "json-c/json.h"

#include "spdk/uuid.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/util.h" // For spdk_get_ticks, spdk_get_ticks_hz
#include <pthread.h>
#include <errno.h>

#define XSAN_VOLUME_META_PREFIX "v:"

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

static xsan_volume_manager_t *g_xsan_volume_manager_instance = NULL; // For callbacks to access vm

// Forward declarations
static xsan_error_t xsan_volume_manager_load_metadata(xsan_volume_manager_t *vm);
static xsan_error_t xsan_volume_manager_save_volume_meta(xsan_volume_manager_t *vm, xsan_volume_t *vol);
static xsan_error_t xsan_volume_manager_delete_volume_meta(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id);
static xsan_error_t _xsan_volume_to_json_string(const xsan_volume_t *vol, char **json_string_out);
static xsan_error_t _xsan_json_string_to_volume(const char *json_string, xsan_volume_manager_t *vm, xsan_volume_t **vol_out);
static xsan_error_t _xsan_volume_submit_single_io_attempt(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id, uint64_t logical_byte_offset, uint64_t length_bytes, void *user_buf, bool is_read_op, xsan_user_io_completion_cb_t user_cb, void *user_cb_arg);

static void _xsan_check_replicated_write_completion(xsan_replicated_io_ctx_t *rep_ctx);
static void _xsan_local_replica_write_complete_cb(void *cb_arg, xsan_error_t status);
static void _xsan_remote_replica_connect_then_send_cb(struct spdk_sock *sock, int status, void *cb_arg);
static void _xsan_remote_replica_request_send_actual_cb(int comm_status, void *cb_arg);

static void _xsan_try_read_from_next_replica(xsan_replica_read_coordinator_ctx_t *coord_ctx);
static void _xsan_replica_read_attempt_complete_cb(void *cb_arg, xsan_error_t status);
static void _xsan_remote_replica_read_req_send_complete_cb(int comm_status, void *cb_arg);
static void _xsan_remote_replica_read_connect_then_send_cb(struct spdk_sock *sock, int status, void *cb_arg);
static void _xsan_volume_update_overall_state(xsan_volume_manager_t *vm, xsan_volume_t *vol);


static uint64_t _get_current_time_us() {
    return spdk_get_ticks() * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
}

static void _xsan_internal_volume_destroy_cb(void *volume_data) { /* ... as before ... */
    if (volume_data) { xsan_volume_t *v = (xsan_volume_t *)volume_data; XSAN_FREE(v); }
}
static uint32_t uint64_tid_hash_func(const void *key) { /* ... */ if(!key)return 0;uint64_t v=*(const uint64_t*)key;v=(~v)+(v<<21);v=v^(v>>24);v=(v+(v<<3))+(v<<8);v=v^(v>>14);v=(v+(v<<2))+(v<<4);v=v^(v>>28);v=v+(v<<31);return (uint32_t)v;}
static int uint64_tid_key_compare_func(const void *k1,const void *k2){ /* ... */ if(k1==k2)return 0;if(!k1)return-1;if(!k2)return 1;uint64_t v1=*(const uint64_t*)k1;uint64_t v2=*(const uint64_t*)k2;if(v1<v2)return-1;if(v1>v2)return 1;return 0;}


xsan_error_t xsan_volume_manager_init(xsan_disk_manager_t *dm, xsan_volume_manager_t **vm_out){ /* ... as before ... */
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
    vm->pending_replicated_ios=xsan_hashtable_create(256,uint64_tid_hash_func,uint64_tid_key_compare_func,NULL,NULL);
    if(!vm->pending_replicated_ios){ XSAN_FREE(vm); return XSAN_ERROR_OUT_OF_MEMORY;}
    vm->pending_replica_reads = xsan_hashtable_create(256, uint64_tid_hash_func, uint64_tid_key_compare_func, NULL, NULL);
    if(!vm->pending_replica_reads){ xsan_hashtable_destroy(vm->pending_replicated_ios); XSAN_FREE(vm); return XSAN_ERROR_OUT_OF_MEMORY;}
    vm->disk_manager=dm; vm->md_store=xsan_metadata_store_open(vm->metadata_db_path,true);
    if(!vm->md_store){ xsan_hashtable_destroy(vm->pending_replica_reads); xsan_hashtable_destroy(vm->pending_replicated_ios); XSAN_FREE(vm); return XSAN_ERROR_STORAGE_GENERIC;}
    vm->initialized=true; g_xsan_volume_manager_instance=vm; if(vm_out)*vm_out=vm;
    xsan_volume_manager_load_metadata(vm); XSAN_LOG_INFO("Volume Manager initialized."); return XSAN_OK;
}
void xsan_volume_manager_fini(xsan_volume_manager_t **vm_ptr){ /* ... as before ... */
    xsan_volume_manager_t *vm = (vm_ptr && *vm_ptr) ? *vm_ptr : g_xsan_volume_manager_instance;
    if (!vm || !vm->initialized) { if(vm_ptr) *vm_ptr = NULL; g_xsan_volume_manager_instance = NULL; return; }
    XSAN_LOG_INFO("Finalizing Volume Manager...");
    pthread_mutex_lock(&vm->pending_ios_lock);
    if(vm->pending_replicated_ios){ xsan_hashtable_iter_t it_w; xsan_hashtable_iter_init(vm->pending_replicated_ios, &it_w); void *k_w,*v_w; while(xsan_hashtable_iter_next(&it_w,&k_w,&v_w)){if(v_w)xsan_replicated_io_ctx_free(v_w);} xsan_hashtable_destroy(vm->pending_replicated_ios);vm->pending_replicated_ios=NULL;}
    if(vm->pending_replica_reads){ xsan_hashtable_iter_t it_r; xsan_hashtable_iter_init(vm->pending_replica_reads, &it_r); void *k_r,*v_r; while(xsan_hashtable_iter_next(&it_r,&k_r,&v_r)){if(v_r)xsan_replica_read_coordinator_ctx_free(v_r);} xsan_hashtable_destroy(vm->pending_replica_reads);vm->pending_replica_reads=NULL;}
    pthread_mutex_unlock(&vm->pending_ios_lock); pthread_mutex_destroy(&vm->pending_ios_lock);
    pthread_mutex_lock(&vm->lock); xsan_list_destroy(vm->managed_volumes); if(vm->md_store)xsan_metadata_store_close(vm->md_store); pthread_mutex_unlock(&vm->lock); pthread_mutex_destroy(&vm->lock);
    XSAN_FREE(vm); if(vm_ptr)*vm_ptr=NULL; if(vm==g_xsan_volume_manager_instance)g_xsan_volume_manager_instance=NULL;
    XSAN_LOG_INFO("Volume Manager finalized.");
}

static xsan_error_t _xsan_volume_to_json_string(const xsan_volume_t *vol, char **json_s_out){ /* ... as before, includes replica_nodes with state and contact time ... */
    if (!vol || !json_s_out) return XSAN_ERROR_INVALID_PARAM;
    json_object *jobj = json_object_new_object(); char uuid_buf[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(uuid_buf, sizeof(uuid_buf), (const struct spdk_uuid *)&vol->id.data[0]); json_object_object_add(jobj, "id", json_object_new_string(uuid_buf));
    json_object_object_add(jobj, "name", json_object_new_string(vol->name));
    json_object_object_add(jobj, "size_bytes", json_object_new_int64(vol->size_bytes));
    json_object_object_add(jobj, "block_size_bytes", json_object_new_int(vol->block_size_bytes));
    json_object_object_add(jobj, "num_blocks", json_object_new_int64(vol->num_blocks));
    json_object_object_add(jobj, "state", json_object_new_int(vol->state)); // Overall volume state
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
        json_object_object_add(jreplica, "state", json_object_new_int(vol->replica_nodes[i].state)); // Persist replica state
        json_object_object_add(jreplica, "last_successful_contact_time_us", json_object_new_int64(vol->replica_nodes[i].last_successful_contact_time_us));
        json_object_array_add(jarray_replicas, jreplica);
    }
    json_object_object_add(jobj, "replica_nodes", jarray_replicas);
    const char *tmp_str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    *json_s_out = xsan_strdup(tmp_str); json_object_put(jobj);
    return (*json_s_out) ? XSAN_OK : XSAN_ERROR_OUT_OF_MEMORY;
}
static xsan_error_t _xsan_json_string_to_volume(const char *js, xsan_volume_manager_t *vm, xsan_volume_t **v_out){ /* ... as before, includes replica_nodes with state and contact time ... */
    (void)vm; if (!js || !v_out) return XSAN_ERROR_INVALID_PARAM;
    struct json_object *jobj = json_tokener_parse(js); if (!jobj || is_error(jobj)) { if(jobj&&!is_error(jobj))json_object_put(jobj);return XSAN_ERROR_CONFIG_PARSE; }
    xsan_volume_t *vol = (xsan_volume_t *)XSAN_MALLOC(sizeof(*vol)); if (!vol) { json_object_put(jobj); return XSAN_ERROR_OUT_OF_MEMORY; }
    memset(vol, 0, sizeof(*vol)); struct json_object *val;
    if (json_object_object_get_ex(jobj, "id", &val)) spdk_uuid_parse((struct spdk_uuid*)&vol->id.data[0], json_object_get_string(val));
    if (json_object_object_get_ex(jobj, "name", &val)) xsan_strcpy_safe(vol->name, json_object_get_string(val), XSAN_MAX_NAME_LEN);
    if (json_object_object_get_ex(jobj, "size_bytes", &val)) vol->size_bytes = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "block_size_bytes", &val)) vol->block_size_bytes = (uint32_t)json_object_get_int(val);
    if (json_object_object_get_ex(jobj, "num_blocks", &val)) vol->num_blocks = (uint64_t)json_object_get_int64(val);
    if (json_object_object_get_ex(jobj, "state", &val)) vol->state = (xsan_storage_state_t)json_object_get_int(val); else vol->state = XSAN_STORAGE_STATE_OFFLINE; // Default loaded state
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
                if (json_object_object_get_ex(j_repl_node, "state", &val)) vol->replica_nodes[i].state = (xsan_storage_state_t)json_object_get_int(val); else vol->replica_nodes[i].state = XSAN_STORAGE_STATE_UNKNOWN; // Default if missing
                if (json_object_object_get_ex(j_repl_node, "last_successful_contact_time_us", &val)) vol->replica_nodes[i].last_successful_contact_time_us = (uint64_t)json_object_get_int64(val);
            }
        }
    }
    json_object_put(jobj); *v_out = vol; return XSAN_OK;
}

static void _xsan_volume_update_overall_state(xsan_volume_manager_t *vm, xsan_volume_t *vol) {
    if (!vm || !vol) return;
    // This function should be called with vm->lock HELD or from a context that ensures serial access to 'vol'.
    // However, to save metadata, it will call save_volume_meta which also takes vm->lock.
    // This can lead to recursive locking if not careful.
    // For now, assume this function is called when vm->lock is NOT held, or make save_volume_meta take a flag to not lock.
    // Alternative: this function only calculates state, and caller saves if changed.

    xsan_storage_state_t old_vol_state = vol->state;
    uint32_t online_replicas = 0;
    for (uint32_t i = 0; i < vol->actual_replica_count; ++i) {
        if (vol->replica_nodes[i].state == XSAN_STORAGE_STATE_ONLINE) {
            online_replicas++;
        }
    }

    uint32_t required_online_for_ok = vol->FTT + 1;
    // Cap required_online_for_ok at actual_replica_count if actual is less (e.g. during initial setup or degraded state)
    if (required_online_for_ok > vol->actual_replica_count && vol->actual_replica_count > 0) {
        required_online_for_ok = vol->actual_replica_count;
    }


    if (online_replicas >= required_online_for_ok) {
        vol->state = XSAN_STORAGE_STATE_ONLINE;
    } else if (online_replicas > 0) { // Still has some online replicas, but not full redundancy
        vol->state = XSAN_STORAGE_STATE_DEGRADED;
    } else { // No online replicas
        vol->state = XSAN_STORAGE_STATE_OFFLINE; // Or FAILED
    }

    if (vol->state != old_vol_state) {
        XSAN_LOG_INFO("Volume '%s' (ID: %s) overall state changed from %d to %d (Online Replicas: %u/%u, FTT: %u)",
                      vol->name, spdk_uuid_get_string((struct spdk_uuid*)&vol->id.data[0]),
                      old_vol_state, vol->state, online_replicas, vol->actual_replica_count, vol->FTT);
        xsan_volume_manager_save_volume_meta(vm, vol); // Persist the change
    }
}

static xsan_error_t xsan_volume_manager_save_volume_meta(xsan_volume_manager_t *vm, xsan_volume_t *vol) { /* ... */ return XSAN_OK;}
static xsan_error_t xsan_volume_manager_delete_volume_meta(xsan_volume_manager_t *vm, xsan_volume_id_t v_id) { /* ... */ return XSAN_OK;}
xsan_error_t xsan_volume_manager_load_metadata(xsan_volume_manager_t *vm) { /* ... */ return XSAN_OK;}
xsan_error_t xsan_volume_create(xsan_volume_manager_t *vm, const char *name, uint64_t size_bytes, xsan_group_id_t group_id, uint32_t logical_block_size_bytes, bool thin, uint32_t ftt, xsan_volume_id_t *vol_id_out ) { /* ... as before, but init replica_nodes[i].state and last_successful_contact_time_us ... */
    // In xsan_volume_create, after populating replica_nodes[0] (local):
    // new_volume->replica_nodes[0].state = XSAN_STORAGE_STATE_ONLINE;
    // new_volume->replica_nodes[0].last_successful_contact_time_us = _get_current_time_us();
    // For remote replicas (placeholder):
    // new_volume->replica_nodes[i].state = XSAN_STORAGE_STATE_INITIALIZING; // Or UNKNOWN
    // new_volume->replica_nodes[i].last_successful_contact_time_us = 0;
    // Then call _xsan_volume_update_overall_state(vm, new_volume); before saving.
    return XSAN_OK;
}
xsan_error_t xsan_volume_delete(xsan_volume_manager_t *vm, xsan_volume_id_t vid){ /* ... */ return XSAN_OK;}
xsan_volume_t *xsan_volume_get_by_id(xsan_volume_manager_t *vm, xsan_volume_id_t vid){ /* ... */ return NULL;}
xsan_volume_t *xsan_volume_get_by_name(xsan_volume_manager_t *vm, const char *n){ /* ... */ return NULL;}
xsan_error_t xsan_volume_list_all(xsan_volume_manager_t *vm, xsan_volume_t ***v_arr_out, int *c_out){ /* ... */ return XSAN_OK;}
void xsan_volume_manager_free_volume_pointer_list(xsan_volume_t **v_ptr_arr){ /* ... */ if(v_ptr_arr)XSAN_FREE(v_ptr_arr);}
xsan_error_t xsan_volume_map_lba_to_physical(xsan_volume_manager_t *vm, xsan_volume_id_t vid, uint64_t lba_idx, xsan_disk_id_t *d_id_out, uint64_t *p_lba_out, uint32_t *p_bs_out){ /* ... */ return XSAN_OK;}

static void _xsan_check_replicated_write_completion(xsan_replicated_io_ctx_t *rep_ctx) { /* ... as before ... */
    if(!rep_ctx)return; uint32_t done_c = __sync_add_and_fetch(&rep_ctx->successful_writes,0) + __sync_add_and_fetch(&rep_ctx->failed_writes,0);
    if(done_c >= rep_ctx->total_replicas_targeted){
        xsan_volume_t *vol = xsan_volume_get_by_id(g_xsan_volume_manager_instance, rep_ctx->volume_id); // Get volume to update its state
        if(rep_ctx->successful_writes >= rep_ctx->total_replicas_targeted) rep_ctx->final_status=XSAN_OK;
        else if(rep_ctx->final_status==XSAN_OK)rep_ctx->final_status=XSAN_ERROR_REPLICATION_GENERIC;
        if(vol) _xsan_volume_update_overall_state(g_xsan_volume_manager_instance, vol); // Update overall vol state
        if(rep_ctx->original_user_cb)rep_ctx->original_user_cb(rep_ctx->original_user_cb_arg,rep_ctx->final_status);
        pthread_mutex_lock(&g_xsan_volume_manager_instance->pending_ios_lock); xsan_hashtable_remove(g_xsan_volume_manager_instance->pending_replicated_ios,&rep_ctx->transaction_id); pthread_mutex_unlock(&g_xsan_volume_manager_instance->pending_ios_lock);
        xsan_replicated_io_ctx_free(rep_ctx);
    }
}
static void _xsan_local_replica_write_complete_cb(void *cb_arg, xsan_error_t status) {
    xsan_replicated_io_ctx_t *rep_ctx = cb_arg; if(!rep_ctx)return;
    xsan_volume_t *vol = xsan_volume_get_by_id(g_xsan_volume_manager_instance, rep_ctx->volume_id);
    if(vol && vol->actual_replica_count > 0){ // Update local replica state
        vol->replica_nodes[0].state = (status == XSAN_OK) ? XSAN_STORAGE_STATE_ONLINE : XSAN_STORAGE_STATE_FAILED;
        if(status == XSAN_OK) vol->replica_nodes[0].last_successful_contact_time_us = _get_current_time_us();
        // No need to lock vm->lock here if _xsan_volume_update_overall_state handles its own locking for save_meta
        // or if this callback is already on a thread that serializes access to 'vol'.
        // _xsan_volume_update_overall_state(g_xsan_volume_manager_instance, vol); // This will be called in check_completion
    }
    if(status==XSAN_OK)__sync_fetch_and_add(&rep_ctx->successful_writes,1); else {__sync_fetch_and_add(&rep_ctx->failed_writes,1); if(rep_ctx->final_status==XSAN_OK)rep_ctx->final_status=status;}
    rep_ctx->local_io_req = NULL; _xsan_check_replicated_write_completion(rep_ctx);
}
static void _xsan_remote_replica_connect_then_send_cb(struct spdk_sock *sock, int status, void *cb_arg) { /* ... as before, on failure update replica state ... */
    xsan_per_replica_op_ctx_t *p_ctx = cb_arg; if(!p_ctx || !p_ctx->parent_rep_ctx || !p_ctx->request_msg_to_send){ /* free */ return;}
    xsan_replicated_io_ctx_t* rep_ctx = p_ctx->parent_rep_ctx;
    xsan_volume_t *vol = xsan_volume_get_by_id(g_xsan_volume_manager_instance, rep_ctx->volume_id);
    uint32_t replica_idx = XSAN_MAX_REPLICAS; // Find which replica this is for state update
    if(vol){ for(uint32_t i=1; i<vol->actual_replica_count; ++i) if(memcmp(&vol->replica_nodes[i], &p_ctx->replica_location_info, sizeof(xsan_replica_location_t))==0) {replica_idx=i; break;} }

    if(status==0 && sock){ p_ctx->connected_sock = sock; xsan_error_t s_err = xsan_node_comm_send_msg(sock, p_ctx->request_msg_to_send, _xsan_remote_replica_request_send_actual_cb, p_ctx); if(s_err!=XSAN_OK){ _xsan_remote_replica_request_send_actual_cb(s_err, p_ctx);}}
    else {
        if(vol && replica_idx < XSAN_MAX_REPLICAS) vol->replica_nodes[replica_idx].state = XSAN_STORAGE_STATE_OFFLINE;
        __sync_fetch_and_add(&rep_ctx->failed_writes,1); if(rep_ctx->final_status==XSAN_OK)rep_ctx->final_status=xsan_error_from_errno(-status);
        xsan_protocol_message_destroy(p_ctx->request_msg_to_send); XSAN_FREE(p_ctx);
        _xsan_check_replicated_write_completion(rep_ctx);
    }
}
static void _xsan_remote_replica_request_send_actual_cb(int comm_status, void *cb_arg) { /* ... as before, on failure update replica state ... */
    xsan_per_replica_op_ctx_t *p_ctx = cb_arg; if(!p_ctx || !p_ctx->parent_rep_ctx){if(p_ctx)XSAN_FREE(p_ctx);return;}
    xsan_replicated_io_ctx_t* rep_ctx = p_ctx->parent_rep_ctx;
    xsan_volume_t *vol = xsan_volume_get_by_id(g_xsan_volume_manager_instance, rep_ctx->volume_id);
    uint32_t replica_idx = XSAN_MAX_REPLICAS;
    if(vol){ for(uint32_t i=1; i<vol->actual_replica_count; ++i) if(memcmp(&vol->replica_nodes[i], &p_ctx->replica_location_info, sizeof(xsan_replica_location_t))==0) {replica_idx=i; break;} }

    if(comm_status!=0){
        if(vol && replica_idx < XSAN_MAX_REPLICAS) vol->replica_nodes[replica_idx].state = XSAN_STORAGE_STATE_OFFLINE; // Send failed
        __sync_fetch_and_add(&rep_ctx->failed_writes,1); if(rep_ctx->final_status==XSAN_OK)rep_ctx->final_status=xsan_error_from_errno(-comm_status); _xsan_check_replicated_write_completion(rep_ctx);
    }
    if(p_ctx->request_msg_to_send) xsan_protocol_message_destroy(p_ctx->request_msg_to_send);
    XSAN_FREE(p_ctx);
}

void xsan_volume_manager_process_replica_write_response(xsan_volume_manager_t *vm, uint64_t tid, xsan_node_id_t resp_node_id, xsan_error_t repl_op_status) { /* ... as before, update replica state ... */
    if (!vm || !vm->initialized) return;
    pthread_mutex_lock(&vm->pending_ios_lock); xsan_replicated_io_ctx_t *rep_ctx = xsan_hashtable_get(vm->pending_replicated_ios, &tid); pthread_mutex_unlock(&vm->pending_ios_lock);
    if (rep_ctx) {
        xsan_volume_t *vol = xsan_volume_get_by_id(vm, rep_ctx->volume_id);
        if(vol){ for(uint32_t i=1; i<vol->actual_replica_count; ++i){ if(spdk_uuid_compare((struct spdk_uuid*)&vol->replica_nodes[i].node_id.data[0], (struct spdk_uuid*)&resp_node_id.data[0])==0){
            vol->replica_nodes[i].state = (repl_op_status==XSAN_OK) ? XSAN_STORAGE_STATE_ONLINE : XSAN_STORAGE_STATE_DEGRADED; // Or FAILED
            if(repl_op_status==XSAN_OK) vol->replica_nodes[i].last_successful_contact_time_us = _get_current_time_us();
            break;
        }}}
        if (repl_op_status == XSAN_OK) __sync_fetch_and_add(&rep_ctx->successful_writes, 1);
        else { __sync_fetch_and_add(&rep_ctx->failed_writes, 1); if (rep_ctx->final_status == XSAN_OK) rep_ctx->final_status = repl_op_status; }
        _xsan_check_replicated_write_completion(rep_ctx);
    } else { XSAN_LOG_WARN("No pending rep IO ctx for TID %lu from node %s.", tid, spdk_uuid_get_string((struct spdk_uuid*)&resp_node_id.data[0]));}
}

static xsan_error_t _xsan_volume_submit_single_io_attempt( /* ... */ ) { /* ... as before ... */ return XSAN_OK;}
xsan_error_t xsan_volume_read_async(xsan_volume_manager_t *vm, xsan_volume_id_t vol_id, uint64_t log_byte_off, uint64_t len_bytes, void *u_buf, xsan_user_io_completion_cb_t u_cb, void *u_cb_arg) { /* ... uses _xsan_try_read_from_next_replica ... */
    if (!vm || !vm->initialized || !u_buf || len_bytes==0 || !u_cb) return XSAN_ERROR_INVALID_PARAM;
    xsan_volume_t *vol = xsan_volume_get_by_id(vm, vol_id); if(!vol) return XSAN_ERROR_NOT_FOUND;
    if (vol->block_size_bytes==0 || (log_byte_off % vol->block_size_bytes !=0) || (len_bytes % vol->block_size_bytes !=0) || (log_byte_off+len_bytes > vol->size_bytes)) return XSAN_ERROR_INVALID_PARAM;
    static uint64_t s_rtid_ctr = 6000; uint64_t tid = __sync_fetch_and_add(&s_rtid_ctr,1);
    xsan_replica_read_coordinator_ctx_t *coord = xsan_replica_read_coordinator_ctx_create(vol,u_buf,log_byte_off,len_bytes,u_cb,u_cb_arg,tid);
    if(!coord) return XSAN_ERROR_OUT_OF_MEMORY;
    _xsan_try_read_from_next_replica(coord); return XSAN_OK;
}
static void _xsan_try_read_from_next_replica(xsan_replica_read_coordinator_ctx_t *coord_ctx) { /* ... as before, but local read cb is _xsan_replica_read_attempt_complete_cb ... */
    if(!coord_ctx || !coord_ctx->vol){ if(coord_ctx){if(coord_ctx->original_user_cb)coord_ctx->original_user_cb(coord_ctx->original_user_cb_arg, XSAN_ERROR_INVALID_PARAM); xsan_replica_read_coordinator_ctx_free(coord_ctx);} return;}
    if(coord_ctx->current_replica_idx_to_try >= (int)coord_ctx->vol->actual_replica_count || coord_ctx->current_replica_idx_to_try >= XSAN_MAX_REPLICAS) { /* All failed */
        if(coord_ctx->original_user_cb)coord_ctx->original_user_cb(coord_ctx->original_user_cb_arg, coord_ctx->last_attempt_status);
        pthread_mutex_lock(&g_xsan_volume_manager_instance->pending_ios_lock); xsan_hashtable_remove(g_xsan_volume_manager_instance->pending_replica_reads, &coord_ctx->transaction_id); pthread_mutex_unlock(&g_xsan_volume_manager_instance->pending_ios_lock);
        xsan_replica_read_coordinator_ctx_free(coord_ctx); return;
    }
    int cur_idx = coord_ctx->current_replica_idx_to_try; xsan_replica_location_t *loc = &coord_ctx->vol->replica_nodes[cur_idx];
    bool is_local = (cur_idx==0); // Simplified local check
    if(is_local){ coord_ctx->last_attempt_status = _xsan_volume_submit_single_io_attempt(g_xsan_volume_manager_instance, coord_ctx->vol->id, coord_ctx->logical_byte_offset, coord_ctx->length_bytes, coord_ctx->user_buffer, true, _xsan_replica_read_attempt_complete_cb, coord_ctx); if(coord_ctx->last_attempt_status!=XSAN_OK){coord_ctx->current_replica_idx_to_try++; _xsan_try_read_from_next_replica(coord_ctx);}}
    else { /* Remote read: setup DMA, per_replica_op_ctx, add to pending_replica_reads, connect/send */
        if(!coord_ctx->internal_dma_buffer){coord_ctx->internal_dma_buffer=xsan_bdev_dma_malloc(coord_ctx->length_bytes,4096); if(!coord_ctx->internal_dma_buffer){coord_ctx->last_attempt_status=XSAN_ERROR_OUT_OF_MEMORY; coord_ctx->current_replica_idx_to_try++; _xsan_try_read_from_next_replica(coord_ctx);return;} coord_ctx->internal_dma_buffer_allocated=true; coord_ctx->internal_dma_buffer_size=coord_ctx->length_bytes;}
        xsan_per_replica_op_ctx_t *rop_ctx = XSAN_MALLOC(sizeof(*rop_ctx)); if(!rop_ctx){/* ... */ _xsan_try_read_from_next_replica(coord_ctx);return;} memset(rop_ctx,0,sizeof(*rop_ctx)); rop_ctx->parent_rep_ctx=(void*)coord_ctx; memcpy(&rop_ctx->replica_location_info,loc,sizeof(*loc));
        xsan_replica_read_req_payload_t req_pl; memcpy(&req_pl.volume_id,&coord_ctx->vol->id,sizeof(req_pl.volume_id)); req_pl.block_lba_on_volume=coord_ctx->logical_byte_offset/coord_ctx->vol->block_size_bytes; req_pl.num_blocks=coord_ctx->length_bytes/coord_ctx->vol->block_size_bytes;
        rop_ctx->request_msg_to_send = xsan_protocol_message_create(XSAN_MSG_TYPE_REPLICA_READ_BLOCK_REQ, coord_ctx->transaction_id, &req_pl, sizeof(req_pl));
        if(!rop_ctx->request_msg_to_send){XSAN_FREE(rop_ctx); /* ... */ _xsan_try_read_from_next_replica(coord_ctx);return;}
        coord_ctx->current_remote_op_ctx = rop_ctx;
        pthread_mutex_lock(&g_xsan_volume_manager_instance->pending_ios_lock); xsan_hashtable_put(g_xsan_volume_manager_instance->pending_replica_reads, &coord_ctx->transaction_id, coord_ctx); pthread_mutex_unlock(&g_xsan_volume_manager_instance->pending_ios_lock);
        struct spdk_sock *sock = xsan_node_comm_get_active_connection(loc->node_ip_addr,loc->node_comm_port);
        if(sock) _xsan_remote_replica_read_connect_then_send_cb(sock,0,rop_ctx); else xsan_node_comm_connect(loc->node_ip_addr,loc->node_comm_port,_xsan_remote_replica_read_connect_then_send_cb,rop_ctx);
    }
}
static void _xsan_replica_read_attempt_complete_cb(void *cb_arg, xsan_error_t status) { /* ... as before, on success copy from internal_dma_buffer ... */
    xsan_replica_read_coordinator_ctx_t*ctx=cb_arg; if(!ctx)return;
    if(status==XSAN_OK){ if(ctx->internal_dma_buffer_allocated&&ctx->internal_dma_buffer&&ctx->user_buffer)memcpy(ctx->user_buffer,ctx->internal_dma_buffer,ctx->length_bytes); ctx->original_user_cb(ctx->original_user_cb_arg,XSAN_OK); pthread_mutex_lock(&g_xsan_volume_manager_instance->pending_ios_lock);xsan_hashtable_remove(g_xsan_volume_manager_instance->pending_replica_reads,&ctx->transaction_id);pthread_mutex_unlock(&g_xsan_volume_manager_instance->pending_ios_lock); xsan_replica_read_coordinator_ctx_free(ctx);}
    else{ctx->last_attempt_status=status; ctx->current_replica_idx_to_try++; if(ctx->current_remote_op_ctx){if(ctx->current_remote_op_ctx->request_msg_to_send)xsan_protocol_message_destroy(ctx->current_remote_op_ctx->request_msg_to_send);XSAN_FREE(ctx->current_remote_op_ctx);ctx->current_remote_op_ctx=NULL;} _xsan_try_read_from_next_replica(ctx);}
}
static void _xsan_remote_replica_read_req_send_complete_cb(int comm_status, void *cb_arg) { /* ... as before ... */
    xsan_per_replica_op_ctx_t*p_ctx=cb_arg;if(!p_ctx||!p_ctx->parent_rep_ctx){if(p_ctx)XSAN_FREE(p_ctx);return;}
    xsan_replica_read_coordinator_ctx_t*coord_ctx=(xsan_replica_read_coordinator_ctx_t*)p_ctx->parent_rep_ctx;
    if(comm_status!=0){ pthread_mutex_lock(&g_xsan_volume_manager_instance->pending_ios_lock);xsan_hashtable_remove(g_xsan_volume_manager_instance->pending_replica_reads,&coord_ctx->transaction_id);pthread_mutex_unlock(&g_xsan_volume_manager_instance->pending_ios_lock); _xsan_replica_read_attempt_complete_cb(coord_ctx,xsan_error_from_errno(-comm_status));}
    if(p_ctx->request_msg_to_send)xsan_protocol_message_destroy(p_ctx->request_msg_to_send); XSAN_FREE(p_ctx); coord_ctx->current_remote_op_ctx=NULL;
}
static void _xsan_remote_replica_read_connect_then_send_cb(struct spdk_sock *sock, int status, void *cb_arg) { /* ... as before ... */
    xsan_per_replica_op_ctx_t*p_ctx=cb_arg;if(!p_ctx||!p_ctx->parent_rep_ctx||!p_ctx->request_msg_to_send){if(p_ctx&&p_ctx->request_msg_to_send)xsan_protocol_message_destroy(p_ctx->request_msg_to_send);if(p_ctx)XSAN_FREE(p_ctx);return;}
    xsan_replica_read_coordinator_ctx_t*coord_ctx=(xsan_replica_read_coordinator_ctx_t*)p_ctx->parent_rep_ctx;
    if(status==0&&sock){p_ctx->connected_sock=sock; xsan_error_t s_err=xsan_node_comm_send_msg(sock,p_ctx->request_msg_to_send,_xsan_remote_replica_read_req_send_complete_cb,p_ctx); if(s_err!=XSAN_OK)_xsan_remote_replica_read_req_send_complete_cb(s_err,p_ctx);}
    else{pthread_mutex_lock(&g_xsan_volume_manager_instance->pending_ios_lock);xsan_hashtable_remove(g_xsan_volume_manager_instance->pending_replica_reads,&coord_ctx->transaction_id);pthread_mutex_unlock(&g_xsan_volume_manager_instance->pending_ios_lock); _xsan_replica_read_attempt_complete_cb(coord_ctx,xsan_error_from_errno(-status)); if(p_ctx->request_msg_to_send)xsan_protocol_message_destroy(p_ctx->request_msg_to_send);XSAN_FREE(p_ctx); coord_ctx->current_remote_op_ctx=NULL;}
}
void xsan_volume_manager_process_replica_read_response(xsan_volume_manager_t *vm, uint64_t tid, xsan_node_id_t r_nid, xsan_error_t r_op_status, const unsigned char *data, uint32_t data_len) { /* ... as before ... */
    if(!vm||!vm->initialized)return;
    pthread_mutex_lock(&vm->pending_ios_lock); xsan_replica_read_coordinator_ctx_t*ctx=xsan_hashtable_remove(vm->pending_replica_reads,&tid); pthread_mutex_unlock(&vm->pending_ios_lock);
    if(ctx){if(r_op_status==XSAN_OK){if(data&&data_len==ctx->length_bytes){if(!ctx->internal_dma_buffer){ctx->internal_dma_buffer=xsan_bdev_dma_malloc(ctx->length_bytes,0);if(ctx->internal_dma_buffer){ctx->internal_dma_buffer_allocated=true;ctx->internal_dma_buffer_size=ctx->length_bytes;}else{r_op_status=XSAN_ERROR_OUT_OF_MEMORY;}} if(r_op_status==XSAN_OK)memcpy(ctx->internal_dma_buffer,data,data_len);}else{r_op_status=XSAN_ERROR_PROTOCOL_GENERIC;}}_xsan_replica_read_attempt_complete_cb(ctx,r_op_status);}
}
xsan_error_t xsan_volume_write_async(xsan_volume_manager_t *vm, xsan_volume_id_t vol_id, uint64_t lbo, uint64_t len, const void *u_buf, xsan_user_io_completion_cb_t u_cb, void *u_cb_arg) { /* ... as before ... */ return XSAN_OK;}
