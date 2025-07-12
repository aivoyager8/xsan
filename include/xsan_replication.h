#ifndef XSAN_REPLICATION_H
#define XSAN_REPLICATION_H

#include "xsan_types.h"
#include "xsan_storage.h" // 补充完整类型定义，消除 incomplete typedef 错误

/* Data replication and consistency functions */

/**
 * Initialize the replication subsystem
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_init(void);

/**
 * Shutdown the replication subsystem
 */
void xsan_replication_shutdown(void);

/**
 * Replicate a data block to specified nodes
 * @param block_metadata Block metadata containing replication info
 * @param data Pointer to the data to replicate
 * @param data_size Size of data in bytes
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_replicate_block(const xsan_block_metadata_t *block_metadata,
                                              const void *data, size_t data_size);

/**
 * Read data with consistency check
 * @param block_metadata Block metadata
 * @param data Buffer to store the data
 * @param data_size Size of buffer
 * @param bytes_read Pointer to store actual bytes read
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_read_consistent(const xsan_block_metadata_t *block_metadata,
                                             void *data, size_t data_size, size_t *bytes_read);

/**
 * Perform repair operation for corrupted block
 * @param block_id UUID of the block to repair
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_repair_block(xsan_uuid_t block_id);

/**
 * Check data integrity for a block
 * @param block_metadata Block metadata
 * @param data Data to check
 * @param data_size Size of data
 * @param is_valid Pointer to store integrity check result
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_check_integrity(const xsan_block_metadata_t *block_metadata,
                                              const void *data, size_t data_size, bool *is_valid);

/**
 * Start background scrubbing process
 * @param priority Scrubbing priority (0=low, 1=normal, 2=high)
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_start_scrub(uint32_t priority);

/**
 * Stop background scrubbing process
 */
void xsan_replication_stop_scrub(void);

/**
 * Migrate data from a failed node to healthy nodes
 * @param failed_node_id UUID of the failed node
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_migrate_data(xsan_uuid_t failed_node_id);

/**
 * Rebalance data across cluster nodes
 * @param force_rebalance Force rebalancing even if cluster is balanced
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_rebalance(bool force_rebalance);

/**
 * Create snapshot of a virtual disk
 * @param vdisk_id UUID of the virtual disk
 * @param snapshot_name Name for the snapshot
 * @param snapshot_id Pointer to store snapshot UUID
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_create_snapshot(xsan_uuid_t vdisk_id, const char *snapshot_name,
                                              xsan_uuid_t *snapshot_id);

/**
 * Delete a snapshot
 * @param snapshot_id UUID of the snapshot to delete
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_delete_snapshot(xsan_uuid_t snapshot_id);

/**
 * Restore virtual disk from snapshot
 * @param vdisk_id UUID of the virtual disk
 * @param snapshot_id UUID of the snapshot to restore from
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_restore_snapshot(xsan_uuid_t vdisk_id, xsan_uuid_t snapshot_id);

/**
 * Clone a virtual disk
 * @param source_vdisk_id UUID of the source virtual disk
 * @param clone_name Name for the cloned disk
 * @param clone_vdisk_id Pointer to store cloned disk UUID
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_clone_vdisk(xsan_uuid_t source_vdisk_id, const char *clone_name,
                                          xsan_uuid_t *clone_vdisk_id);

/**
 * Get replication status for a virtual disk
 * @param vdisk_id UUID of the virtual disk
 * @param replica_count Pointer to store current replica count
 * @param healthy_replicas Pointer to store number of healthy replicas
 * @param is_synchronized Pointer to store synchronization status
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_get_status(xsan_uuid_t vdisk_id, uint32_t *replica_count,
                                        uint32_t *healthy_replicas, bool *is_synchronized);

/**
 * Force synchronization of replicas
 * @param vdisk_id UUID of the virtual disk
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_force_sync(xsan_uuid_t vdisk_id);

/**
 * Set replication mode (synchronous/asynchronous)
 * @param vdisk_id UUID of the virtual disk
 * @param synchronous true for synchronous replication, false for asynchronous
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_replication_set_mode(xsan_uuid_t vdisk_id, bool synchronous);

#endif /* XSAN_REPLICATION_H */
