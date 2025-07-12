#include "xsan_cluster.h"
#include "xsan_config.h" // For xsan_node_config_t
#include "xsan_log.h"
#include "xsan_string_utils.h" // For xsan_strcpy_safe
#include "xsan_error.h"
#include "spdk/uuid.h"   // For spdk_uuid_parse

// Assumes g_local_node_config is defined and populated in xsan_node.c (or another main module)
// This is a common way to access app-wide config, but consider passing config via init functions
// for better modularity in the long run.
extern xsan_node_config_t g_local_node_config;
extern xsan_config_t *g_xsan_config; // To check if config system itself is initialized

xsan_error_t xsan_get_local_node_info(xsan_node_id_t *node_id_out,
                                      char *ip_buf, size_t ip_buf_len,
                                      uint16_t *port_out) {
    if (!node_id_out || !ip_buf || ip_buf_len == 0 || !port_out) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    // Check if the global configuration has been loaded.
    // A simple check could be if g_xsan_config is NULL or g_local_node_config.node_id is empty.
    if (!g_xsan_config || strlen(g_local_node_config.node_id) == 0) {
        XSAN_LOG_ERROR("Local node configuration not loaded or node_id is empty. Call config load first.");
        return XSAN_ERROR_NOT_INITIALIZED;
    }

    // Parse the node_id string from config into UUID format
    if (spdk_uuid_parse((struct spdk_uuid *)node_id_out->data, g_local_node_config.node_id) != 0) {
        XSAN_LOG_ERROR("Failed to parse configured node_id '%s' as UUID.", g_local_node_config.node_id);
        // Ensure node_id_out is zeroed on error to avoid returning garbage
        memset(node_id_out->data, 0, sizeof(node_id_out->data));
        return XSAN_ERROR_CONFIG_PARSE;
    }

    // Copy IP address and port
    xsan_strcpy_safe(ip_buf, g_local_node_config.bind_address, ip_buf_len);
    *port_out = g_local_node_config.port;

    // Basic validation of copied/parsed values
    if (strlen(ip_buf) == 0) { // Check if IP is empty after copy
        XSAN_LOG_ERROR("Loaded local node IP address is empty.");
        return XSAN_ERROR_CONFIG_INVALID;
    }
    if (*port_out == 0) { // Check if port is invalid (0 is typically not a valid service port)
        XSAN_LOG_WARN("Loaded local node port is 0. This might be unintentional.");
        // Depending on requirements, this could be an error: return XSAN_ERROR_CONFIG_INVALID;
    }

    char uuid_str[SPDK_UUID_STRING_LEN];
    spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), (struct spdk_uuid*)node_id_out->data);
    XSAN_LOG_DEBUG("Retrieved local node info: ID=%s, IP=%s, Port=%u",
                   uuid_str, ip_buf, *port_out);

    return XSAN_OK;
}

// Stub implementations for other functions declared in xsan_cluster.h
// These would be filled out as cluster management features are developed.

xsan_error_t xsan_cluster_init(const char *config_path) {
    XSAN_LOG_INFO("xsan_cluster_init called (config_path: %s) - STUB", config_path ? config_path : "None");
    // TODO:
    // 1. Load cluster-specific parts of the configuration (e.g., seed nodes)
    //    using g_xsan_config and xsan_config_load_cluster_config(&g_cluster_config).
    // 2. Initialize internal data structures for managing cluster nodes.
    // 3. Potentially start discovery mechanisms or try to connect to seed nodes.
    return XSAN_OK;
}

void xsan_cluster_shutdown(void) {
    XSAN_LOG_INFO("xsan_cluster_shutdown called - STUB");
    // TODO: Clean up cluster resources, stop any cluster-related services (heartbeat, discovery).
}

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
    (void)cluster_info;
    XSAN_LOG_INFO("xsan_cluster_get_info called - STUB");
    return XSAN_ERROR_NOT_IMPLEMENTED;
}

xsan_error_t xsan_cluster_get_node(xsan_uuid_t node_id, xsan_node_t *node_info) {
    (void)node_id;
    (void)node_info;
    XSAN_LOG_INFO("xsan_cluster_get_node called - STUB");
    return XSAN_ERROR_NOT_IMPLEMENTED;
}

xsan_error_t xsan_cluster_update_node_state(xsan_uuid_t node_id, xsan_node_state_t new_state) {
    (void)node_id;
    (void)new_state;
    XSAN_LOG_INFO("xsan_cluster_update_node_state called - STUB");
    return XSAN_ERROR_NOT_IMPLEMENTED;
}

xsan_error_t xsan_cluster_health_check(void) {
    XSAN_LOG_INFO("xsan_cluster_health_check called - STUB");
    return XSAN_OK; // Assume healthy for now
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
    return true; // Placeholder
}

xsan_error_t xsan_cluster_get_master(xsan_uuid_t *master_id) {
    (void)master_id;
    XSAN_LOG_INFO("xsan_cluster_get_master called - STUB");
    // For placeholder, could copy local node ID if is_master is true
    if (master_id) {
         // xsan_get_local_node_info(master_id, NULL, 0, NULL); // Simplified, only gets ID
    }
    return XSAN_ERROR_NOT_IMPLEMENTED;
}
