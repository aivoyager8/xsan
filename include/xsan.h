#ifndef XSAN_H
#define XSAN_H

/**
 * XSAN - Distributed Storage System for KVM
 * 
 * A high-performance distributed storage system similar to VMware vSAN,
 * specifically designed for KVM virtualization environments.
 * 
 * Features:
 * - Distributed storage with automatic data distribution
 * - Multiple RAID levels (RAID-1, RAID-5, RAID-6)
 * - Storage policies for performance and availability
 * - Automatic data replication and consistency
 * - Seamless KVM/libvirt integration
 * - Storage pool management
 * - VM storage provisioning
 * - Live migration support
 * - Data scrubbing and repair
 * - Snapshots and cloning
 */

#include "xsan_types.h"
#include "xsan_common.h"
#include "xsan_cluster.h"
#include "xsan_storage.h"
#include "xsan_policy.h"
#include "xsan_replication.h"
#include "xsan_virtualization.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the entire XSAN system
 * @param config_file Path to configuration file
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_init(const char *config_file);

/**
 * Shutdown the entire XSAN system
 */
void xsan_shutdown(void);

/**
 * Get XSAN version information
 * @param major Pointer to store major version
 * @param minor Pointer to store minor version
 * @param patch Pointer to store patch version
 * @param build_string Pointer to store build string (caller must not free)
 */
void xsan_get_version(uint32_t *major, uint32_t *minor, uint32_t *patch, const char **build_string);

/**
 * Get overall system status
 * @param cluster_healthy Pointer to store cluster health status
 * @param storage_healthy Pointer to store storage health status
 * @param total_capacity Pointer to store total capacity in bytes
 * @param used_capacity Pointer to store used capacity in bytes
 * @param node_count Pointer to store number of active nodes
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_get_system_status(bool *cluster_healthy, bool *storage_healthy,
                                   uint64_t *total_capacity, uint64_t *used_capacity,
                                   uint32_t *node_count);

#ifdef __cplusplus
}
#endif

#endif /* XSAN_H */
