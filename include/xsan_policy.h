#ifndef XSAN_POLICY_H
#define XSAN_POLICY_H

#include "xsan_types.h"

/* Storage policy management functions */

/**
 * Initialize the policy subsystem
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_init(void);

/**
 * Shutdown the policy subsystem
 */
void xsan_policy_shutdown(void);

/**
 * Create a new storage policy
 * @param name Name of the policy
 * @param policy_type Type of RAID policy (RAID-1, RAID-5, RAID-6)
 * @param replica_count Number of replicas (for RAID-1)
 * @param stripe_width Stripe width (for RAID-5/6)
 * @param preferred_tier Preferred storage tier
 * @param policy_id Pointer to store the created policy UUID
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_create(const char *name, xsan_policy_type_t policy_type,
                               uint32_t replica_count, uint32_t stripe_width,
                               xsan_tier_t preferred_tier, xsan_uuid_t *policy_id);

/**
 * Delete a storage policy
 * @param policy_id UUID of the policy to delete
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_delete(xsan_uuid_t policy_id);

/**
 * Get policy information
 * @param policy_id UUID of the policy
 * @param policy_info Pointer to store policy information
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_get_info(xsan_uuid_t policy_id, xsan_storage_policy_t *policy_info);

/**
 * Update policy configuration
 * @param policy_id UUID of the policy to update
 * @param policy_info Updated policy information
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_update(xsan_uuid_t policy_id, const xsan_storage_policy_t *policy_info);

/**
 * List all available policies
 * @param policies Array to store policy information
 * @param max_policies Maximum number of policies to return
 * @param policy_count Pointer to store actual number of policies returned
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_list(xsan_storage_policy_t *policies, uint32_t max_policies, uint32_t *policy_count);

/**
 * Get default storage policy
 * @param policy_id Pointer to store default policy UUID
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_get_default(xsan_uuid_t *policy_id);

/**
 * Set default storage policy
 * @param policy_id UUID of the policy to set as default
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_set_default(xsan_uuid_t policy_id);

/**
 * Evaluate policy compliance for a virtual disk
 * @param vdisk_id UUID of the virtual disk
 * @param policy_id UUID of the policy to check against
 * @param is_compliant Pointer to store compliance result
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_check_compliance(xsan_uuid_t vdisk_id, xsan_uuid_t policy_id, bool *is_compliant);

/**
 * Apply policy to existing virtual disk (migration)
 * @param vdisk_id UUID of the virtual disk
 * @param new_policy_id UUID of the new policy to apply
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_apply_to_vdisk(xsan_uuid_t vdisk_id, xsan_uuid_t new_policy_id);

/**
 * Calculate storage requirements for a policy
 * @param policy_id UUID of the policy
 * @param logical_size Logical size of data in bytes
 * @param physical_size Pointer to store calculated physical size needed
 * @param node_count Pointer to store minimum number of nodes required
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_calculate_requirements(xsan_uuid_t policy_id, uint64_t logical_size,
                                               uint64_t *physical_size, uint32_t *node_count);

/**
 * Get optimal placement for data based on policy
 * @param policy_id UUID of the policy
 * @param size_bytes Size of data to place
 * @param primary_node Pointer to store primary node UUID
 * @param replica_nodes Array to store replica node UUIDs
 * @param replica_count Pointer to store number of replicas
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_get_placement(xsan_uuid_t policy_id, uint64_t size_bytes,
                                      xsan_uuid_t *primary_node, xsan_uuid_t *replica_nodes,
                                      uint32_t *replica_count);

/**
 * Set QoS limits for a policy
 * @param policy_id UUID of the policy
 * @param iops_limit Maximum IOPS (0 for unlimited)
 * @param bandwidth_limit_mbps Maximum bandwidth in MB/s (0 for unlimited)
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_set_qos(xsan_uuid_t policy_id, uint32_t iops_limit, uint32_t bandwidth_limit_mbps);

/**
 * Enable/disable features for a policy
 * @param policy_id UUID of the policy
 * @param enable_encryption Whether to enable encryption
 * @param enable_compression Whether to enable compression
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_policy_set_features(xsan_uuid_t policy_id, bool enable_encryption, bool enable_compression);

#endif /* XSAN_POLICY_H */
