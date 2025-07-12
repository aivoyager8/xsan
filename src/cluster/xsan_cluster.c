#include "xsan_cluster.h"
#include "xsan_config.h" // For xsan_node_config_t, xsan_cluster_config_t
#include "xsan_log.h"
#include "xsan_string_utils.h" // For xsan_strcpy_safe
#include "xsan_error.h"
#include "xsan_memory.h"   // For XSAN_MALLOC, XSAN_FREE
#include "xsan_types.h"    // For xsan_node_t
#include "spdk/uuid.h"   // For spdk_uuid_parse, spdk_uuid_is_null, spdk_uuid_compare, spdk_uuid_get_string

#include <string.h>      // For memcpy, strlen
#include <pthread.h>     // For pthread_mutex_t

// Assumes g_local_node_config and g_cluster_config are defined and populated
// in xsan_node.c (or another main module an linked appropriately)
extern xsan_node_config_t g_local_node_config;
extern xsan_cluster_config_t g_cluster_config;
extern xsan_config_t *g_xsan_config;

// Internal storage for known cluster nodes, populated from seed_nodes initially
#define XSAN_INTERNAL_MAX_KNOWN_NODES XSAN_MAX_SEED_NODES
static xsan_node_t g_known_cluster_nodes[XSAN_INTERNAL_MAX_KNOWN_NODES];
static size_t g_known_node_count = 0;
static bool g_cluster_initialized = false;
static pthread_mutex_t g_known_nodes_lock = PTHREAD_MUTEX_INITIALIZER;


xsan_error_t xsan_get_local_node_info(xsan_node_id_t *node_id_out,
                                      char *ip_buf, size_t ip_buf_len,
                                      uint16_t *port_out) {
    if (!node_id_out || !ip_buf || ip_buf_len == 0 || !port_out) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    if (!g_xsan_config || strlen(g_local_node_config.node_id) == 0) {
        XSAN_LOG_ERROR("Local node configuration not loaded or node_id is empty. Call config load first.");
        return XSAN_ERROR_NOT_INITIALIZED;
    }

    if (spdk_uuid_parse((struct spdk_uuid *)node_id_out->data, g_local_node_config.node_id) != 0) {
        XSAN_LOG_ERROR("Failed to parse configured node_id '%s' as UUID.", g_local_node_config.node_id);
        memset(node_id_out->data, 0, sizeof(node_id_out->data));
        return XSAN_ERROR_CONFIG_PARSE;
    }

    xsan_strcpy_safe(ip_buf, g_local_node_config.bind_address, ip_buf_len);
    *port_out = g_local_node_config.port;

    if (strlen(ip_buf) == 0) {
        XSAN_LOG_ERROR("Loaded local node IP address is empty.");
        return XSAN_ERROR_CONFIG_INVALID;
    }
    if (*port_out == 0 && strlen(ip_buf) > 0) { // Only warn if IP is set but port is 0
        XSAN_LOG_WARN("Loaded local node port is 0 for IP %s. This might be unintentional.", ip_buf);
    }

    char uuid_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), (struct spdk_uuid*)node_id_out->data);
    XSAN_LOG_DEBUG("Retrieved local node info: ID=%s, IP=%s, Port=%u",
                   uuid_str, ip_buf, *port_out);

    return XSAN_OK;
}

xsan_error_t xsan_cluster_init(const char *config_path) {
    (void)config_path; // Assuming g_cluster_config is already populated from g_xsan_config

    if (g_cluster_initialized) {
        XSAN_LOG_WARN("XSAN Cluster module already initialized.");
        return XSAN_OK;
    }
    if (!g_xsan_config || strlen(g_cluster_config.cluster_name) == 0) {
         XSAN_LOG_ERROR("Global config (g_xsan_config) not loaded or cluster_config not populated (cluster_name is empty). Cannot initialize cluster module.");
         return XSAN_ERROR_NOT_INITIALIZED;
    }

    XSAN_LOG_INFO("Initializing XSAN Cluster module with cluster name: %s", g_cluster_config.cluster_name);

    pthread_mutex_lock(&g_known_nodes_lock);
    g_known_node_count = 0;
    for (size_t i = 0; i < g_cluster_config.seed_node_count && i < XSAN_INTERNAL_MAX_KNOWN_NODES; ++i) {
        if (spdk_uuid_is_null((struct spdk_uuid*)&g_cluster_config.seed_nodes[i].id.data[0])) {
            XSAN_LOG_WARN("Seed node at index %zu has a NULL UUID. Skipping.", i);
            continue;
        }
        // Assuming storage_addr is the primary communication address for replicas
        if (strlen(g_cluster_config.seed_nodes[i].storage_addr.ip) == 0 || g_cluster_config.seed_nodes[i].storage_addr.port == 0) {
             XSAN_LOG_WARN("Seed node at index %zu (ID: %s) has invalid storage IP or port. Skipping.", i, spdk_uuid_get_string((struct spdk_uuid*)&g_cluster_config.seed_nodes[i].id.data[0]));
            continue;
        }

        memcpy(&g_known_cluster_nodes[g_known_node_count], &g_cluster_config.seed_nodes[i], sizeof(xsan_node_t));

        char uuid_str[SPDK_UUID_STRING_LEN];
        spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), (struct spdk_uuid*)&g_known_cluster_nodes[g_known_node_count].id.data[0]);
        XSAN_LOG_INFO("Added known node from seed config: ID=%s, Hostname/IP=%s, StoragePort=%u",
                       uuid_str,
                       g_known_cluster_nodes[g_known_node_count].storage_addr.ip,
                       g_known_cluster_nodes[g_known_node_count].storage_addr.port);
        g_known_node_count++;
    }
    pthread_mutex_unlock(&g_known_nodes_lock);

    g_cluster_initialized = true;
    XSAN_LOG_INFO("XSAN Cluster module initialized with %zu known seed nodes.", g_known_node_count);
    return XSAN_OK;
}

void xsan_cluster_shutdown(void) {
    if (!g_cluster_initialized) {
        XSAN_LOG_DEBUG("XSAN Cluster module shutdown called but not initialized or already shut down.");
        return;
    }
    XSAN_LOG_INFO("Shutting down XSAN Cluster module...");
    pthread_mutex_lock(&g_known_nodes_lock);
    g_known_node_count = 0;
    pthread_mutex_unlock(&g_known_nodes_lock);
    g_cluster_initialized = false;
    XSAN_LOG_INFO("XSAN Cluster module shut down.");
}

xsan_error_t xsan_cluster_get_all_known_nodes(xsan_node_t **nodes_array_out, size_t *count_out) {
    if (!nodes_array_out || !count_out) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (!g_cluster_initialized) {
        XSAN_LOG_WARN("xsan_cluster_get_all_known_nodes called before cluster init or after shutdown.");
        *nodes_array_out = NULL;
        *count_out = 0;
        return XSAN_ERROR_NOT_INITIALIZED;
    }

    pthread_mutex_lock(&g_known_nodes_lock);
    if (g_known_node_count == 0) {
        *nodes_array_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&g_known_nodes_lock);
        return XSAN_OK;
    }

    *nodes_array_out = (xsan_node_t *)XSAN_MALLOC(sizeof(xsan_node_t) * g_known_node_count);
    if (!*nodes_array_out) {
        *count_out = 0;
        pthread_mutex_unlock(&g_known_nodes_lock);
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    memcpy(*nodes_array_out, g_known_cluster_nodes, sizeof(xsan_node_t) * g_known_node_count);
    *count_out = g_known_node_count;
    pthread_mutex_unlock(&g_known_nodes_lock);

    return XSAN_OK;
}

void xsan_cluster_free_known_nodes_array(xsan_node_t *nodes_array) {
    if (nodes_array) {
        XSAN_FREE(nodes_array);
    }
}

xsan_error_t xsan_cluster_get_node_by_id(xsan_node_id_t node_id, xsan_node_t *node_info_out) {
    if (!node_info_out || spdk_uuid_is_null((struct spdk_uuid*)&node_id.data[0])) {
        return XSAN_ERROR_INVALID_PARAM;
    }
     if (!g_cluster_initialized) {
        return XSAN_ERROR_NOT_INITIALIZED;
    }
    pthread_mutex_lock(&g_known_nodes_lock);
    for (size_t i = 0; i < g_known_node_count; ++i) {
        if (spdk_uuid_compare((struct spdk_uuid*)&g_known_cluster_nodes[i].id.data[0], (struct spdk_uuid*)&node_id.data[0]) == 0) {
            memcpy(node_info_out, &g_known_cluster_nodes[i], sizeof(xsan_node_t));
            pthread_mutex_unlock(&g_known_nodes_lock);
            return XSAN_OK;
        }
    }
    pthread_mutex_unlock(&g_known_nodes_lock);
    return XSAN_ERROR_NOT_FOUND;
}

// Stub implementations for other functions declared in xsan_cluster.h
xsan_error_t xsan_cluster_join_node(const xsan_node_t *node_info) {
    (void)node_info;
    XSAN_LOG_INFO("xsan_cluster_join_node called - STUB");
    return XSAN_ERROR_NOT_IMPLEMENTED;
}

xsan_error_t xsan_cluster_remove_node(xsan_uuid_t node_id, bool force) {
    (void)node_id;
    (void)force;
    XSAN_LOG_INFO("xsan_cluster_remove_node called - STUB");
    return XSAN_ERROR_NOT_IMPLEMENTED;
}

xsan_error_t xsan_cluster_get_info(xsan_cluster_t *cluster_info) {
    (void)cluster_info; // This parameter is xsan_cluster_t from xsan_types.h, not xsan_cluster_config_t
    XSAN_LOG_INFO("xsan_cluster_get_info called - STUB");
    // TODO: Populate cluster_info with runtime cluster state if needed.
    // For now, it might copy data from g_cluster_config or g_known_cluster_nodes.
    return XSAN_ERROR_NOT_IMPLEMENTED;
}

// Note: xsan_cluster_get_node is similar to xsan_cluster_get_node_by_id,
// but xsan_types.h defines xsan_node_t which is more detailed than just config.
// The current xsan_cluster_get_node_by_id returns the configured/seed node info.
xsan_error_t xsan_cluster_get_node(xsan_uuid_t node_id, xsan_node_t *node_info) {
    return xsan_cluster_get_node_by_id(node_id, node_info); // Delegate for now
}

xsan_error_t xsan_cluster_update_node_state(xsan_uuid_t node_id, xsan_node_state_t new_state) {
    (void)node_id;
    (void)new_state;
    XSAN_LOG_INFO("xsan_cluster_update_node_state called - STUB");
    // TODO: Update the state of a node in g_known_cluster_nodes (if found)
    // and potentially trigger events or re-evaluate cluster health.
    return XSAN_ERROR_NOT_IMPLEMENTED;
}

xsan_error_t xsan_cluster_health_check(void) {
    XSAN_LOG_INFO("xsan_cluster_health_check called - STUB");
    return XSAN_OK;
}

xsan_error_t xsan_cluster_start_heartbeat(uint32_t interval_seconds) {
    (void)interval_seconds;
    XSAN_LOG_INFO("xsan_cluster_start_heartbeat called - STUB");
    return XSAN_OK;
}

void xsan_cluster_stop_heartbeat(void) {
    XSAN_LOG_INFO("xsan_cluster_stop_heartbeat called - STUB");
}

xsan_error_t xsan_cluster_register_events(xsan_node_event_cb_t callback, void *user_data) {
    (void)callback;
    (void)user_data;
    XSAN_LOG_INFO("xsan_cluster_register_events called - STUB");
    return XSAN_OK;
}

xsan_error_t xsan_cluster_elect_master(void) {
    XSAN_LOG_INFO("xsan_cluster_elect_master called - STUB");
    return XSAN_ERROR_NOT_IMPLEMENTED;
}

bool xsan_cluster_is_master(void) {
    XSAN_LOG_INFO("xsan_cluster_is_master called - STUB - returning true for now");
    return true;
}

xsan_error_t xsan_cluster_get_master(xsan_uuid_t *master_id) {
    if (!master_id) return XSAN_ERROR_INVALID_PARAM;
    XSAN_LOG_INFO("xsan_cluster_get_master called - STUB");
    // For placeholder, could copy local node ID if is_master is true
    char ip_buf[INET6_ADDRSTRLEN];
    uint16_t port;
    return xsan_get_local_node_info(master_id, ip_buf, sizeof(ip_buf), &port); // Return local as master for now
}
