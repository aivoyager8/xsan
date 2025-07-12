#ifndef XSAN_CLUSTER_H
#define XSAN_CLUSTER_H

#include "xsan_types.h"

/* Cluster management functions */

/**
 * Initialize the cluster subsystem
 * @param config_path Path to cluster configuration file
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_init(const char *config_path);

/**
 * Shutdown the cluster subsystem
 */
void xsan_cluster_shutdown(void);

/**
 * Join a new node to the cluster
 * @param node_info Node information structure
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_join_node(const xsan_node_t *node_info);

/**
 * Remove a node from the cluster
 * @param node_id UUID of the node to remove
 * @param force Force removal even if data migration is incomplete
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_remove_node(xsan_uuid_t node_id, bool force);

/**
 * Get cluster information
 * @param cluster_info Pointer to store cluster information
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_get_info(xsan_cluster_t *cluster_info);

/**
 * Get node information by ID
 * @param node_id UUID of the node
 * @param node_info Pointer to store node information
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_get_node(xsan_uuid_t node_id, xsan_node_t *node_info);

/**
 * Update node state
 * @param node_id UUID of the node
 * @param new_state New state for the node
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_update_node_state(xsan_uuid_t node_id, xsan_node_state_t new_state);

/**
 * Perform cluster health check
 * @return XSAN_SUCCESS if cluster is healthy, error code otherwise
 */
xsan_error_t xsan_cluster_health_check(void);

/**
 * Start heartbeat monitoring
 * @param interval_seconds Heartbeat interval in seconds
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_start_heartbeat(uint32_t interval_seconds);

/**
 * Stop heartbeat monitoring
 */
void xsan_cluster_stop_heartbeat(void);

/**
 * Register for cluster events
 * @param callback Callback function for node state changes
 * @param user_data User data to pass to callback
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_register_events(xsan_node_event_cb_t callback, void *user_data);

/**
 * Elect a new master node (for consensus)
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_elect_master(void);

/**
 * Check if current node is the master
 * @return true if current node is master, false otherwise
 */
bool xsan_cluster_is_master(void);

/**
 * Get master node ID
 * @param master_id Pointer to store master node UUID
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_cluster_get_master(xsan_uuid_t *master_id);

/**
 * @brief Retrieves the local node's essential information.
 * This information is typically loaded from configuration at startup.
 *
 * @param node_id_out Pointer to store the local node's XSAN UUID.
 * @param ip_buf Buffer to store the local node's IP address string.
 * @param ip_buf_len Size of the ip_buf.
 * @param port_out Pointer to store the local node's communication port.
 * @return XSAN_OK on success, XSAN_ERROR_NOT_INITIALIZED if config not loaded,
 *         or other error codes for invalid parameters/parsing issues.
 */
xsan_error_t xsan_get_local_node_info(xsan_node_id_t *node_id_out,
                                      char *ip_buf, size_t ip_buf_len,
                                      uint16_t *port_out);

#endif /* XSAN_CLUSTER_H */
