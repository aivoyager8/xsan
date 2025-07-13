#ifndef XSAN_STORAGE_H
#define XSAN_STORAGE_H

#include "xsan_types.h"

typedef enum xsan_storage_state {
    XSAN_STORAGE_STATE_UNKNOWN = 0,
    XSAN_STORAGE_STATE_INITIALIZING,
    XSAN_STORAGE_STATE_ONLINE,
    XSAN_STORAGE_STATE_OFFLINE,
    XSAN_STORAGE_STATE_DEGRADED,
    XSAN_STORAGE_STATE_FAILED,
    XSAN_STORAGE_STATE_MAINTENANCE
} xsan_storage_state_t;

typedef struct xsan_replica_location {
    char ip[46];      // IPv6 address max length
    uint16_t port;    // Port number
    bool is_ipv6;     // Is IPv6 address
    xsan_storage_state_t state;
    // 可扩展字段
} xsan_replica_location_t;

typedef struct xsan_volume {
    xsan_volume_id_t id;
    char name[XSAN_MAX_NAME_LEN];
    uint64_t size_bytes;
    uint32_t actual_replica_count;
    xsan_replica_location_t replicas[XSAN_MAX_REPLICAS];
    xsan_storage_state_t state;
    uint32_t FTT; // 容错级别
    // 可扩展元数据字段
} xsan_volume_t;

/* Storage engine functions */

/**
 * Initialize the storage subsystem
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_init(void);

/**
 * Shutdown the storage subsystem
 */
void xsan_storage_shutdown(void);

/**
 * Add a storage device to the cluster
 * @param node_id UUID of the node owning the device
 * @param device_path Path to the storage device
 * @param is_cache_device Whether this is a cache device (SSD) or capacity device
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_add_device(xsan_uuid_t node_id, const char *device_path, bool is_cache_device);

/**
 * Remove a storage device from the cluster
 * @param device_id UUID of the device to remove
 * @param force Force removal even if data migration is incomplete
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_remove_device(xsan_uuid_t device_id, bool force);

/**
 * Create a virtual disk
 * @param vm_id UUID of the VM that owns this disk
 * @param name Name of the virtual disk
 * @param size_bytes Size of the disk in bytes
 * @param policy_id Storage policy to use
 * @param thin_provision Whether to use thin provisioning
 * @param vdisk_id Pointer to store the created disk UUID
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_create_vdisk(xsan_uuid_t vm_id, const char *name, 
                                       uint64_t size_bytes, xsan_uuid_t policy_id,
                                       bool thin_provision, xsan_uuid_t *vdisk_id);

/**
 * Delete a virtual disk
 * @param vdisk_id UUID of the virtual disk to delete
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_delete_vdisk(xsan_uuid_t vdisk_id);

/**
 * Read data from a virtual disk
 * @param vdisk_id UUID of the virtual disk
 * @param offset Offset in bytes from the beginning of the disk
 * @param buffer Buffer to store the read data
 * @param size Number of bytes to read
 * @param bytes_read Pointer to store actual bytes read
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_read(xsan_uuid_t vdisk_id, uint64_t offset, 
                              void *buffer, size_t size, size_t *bytes_read);

/**
 * Write data to a virtual disk
 * @param vdisk_id UUID of the virtual disk
 * @param offset Offset in bytes from the beginning of the disk
 * @param buffer Buffer containing data to write
 * @param size Number of bytes to write
 * @param bytes_written Pointer to store actual bytes written
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_write(xsan_uuid_t vdisk_id, uint64_t offset,
                               const void *buffer, size_t size, size_t *bytes_written);

/**
 * Flush pending writes for a virtual disk
 * @param vdisk_id UUID of the virtual disk
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_flush(xsan_uuid_t vdisk_id);

/**
 * Get virtual disk information
 * @param vdisk_id UUID of the virtual disk
 * @param vdisk_info Pointer to store disk information
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_get_vdisk_info(xsan_uuid_t vdisk_id, xsan_vdisk_t *vdisk_info);

/**
 * Resize a virtual disk
 * @param vdisk_id UUID of the virtual disk
 * @param new_size_bytes New size in bytes
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_resize_vdisk(xsan_uuid_t vdisk_id, uint64_t new_size_bytes);

/**
 * Get storage statistics for a node
 * @param node_id UUID of the node (or null for all nodes)
 * @param total_capacity Pointer to store total capacity
 * @param free_capacity Pointer to store free capacity
 * @param total_iops Pointer to store total IOPS capability
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_get_stats(xsan_uuid_t *node_id, uint64_t *total_capacity,
                                   uint64_t *free_capacity, uint32_t *total_iops);

/**
 * Perform storage health check
 * @return XSAN_SUCCESS if all storage devices are healthy, error code otherwise
 */
xsan_error_t xsan_storage_health_check(void);

/**
 * Start background maintenance tasks (garbage collection, defragmentation)
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_start_maintenance(void);

/**
 * Stop background maintenance tasks
 */
void xsan_storage_stop_maintenance(void);

/**
 * Register for storage events
 * @param callback Callback function for storage events
 * @param user_data User data to pass to callback
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_storage_register_events(xsan_storage_event_cb_t callback, void *user_data);

#endif /* XSAN_STORAGE_H */
