#ifndef XSAN_VIRTUALIZATION_H
#define XSAN_VIRTUALIZATION_H

#include "xsan_types.h"
#include <libvirt/libvirt.h>

/* KVM/libvirt integration functions */

/**
 * Initialize the virtualization subsystem
 * @param hypervisor_uri URI for hypervisor connection (e.g., "qemu:///system")
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_init(const char *hypervisor_uri);

/**
 * Shutdown the virtualization subsystem
 */
void xsan_virt_shutdown(void);

/**
 * Connect to libvirt hypervisor
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_connect(void);

/**
 * Disconnect from libvirt hypervisor
 */
void xsan_virt_disconnect(void);

/**
 * Create a storage pool for XSAN virtual disks
 * @param pool_name Name of the storage pool
 * @param pool_path Path where pool metadata will be stored
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_create_pool(const char *pool_name, const char *pool_path);

/**
 * Delete a storage pool
 * @param pool_name Name of the storage pool to delete
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_delete_pool(const char *pool_name);

/**
 * Create a libvirt storage volume backed by XSAN virtual disk
 * @param pool_name Name of the storage pool
 * @param vol_name Name of the volume
 * @param vdisk_id UUID of the XSAN virtual disk
 * @param size_bytes Size of the volume in bytes
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_create_volume(const char *pool_name, const char *vol_name,
                                     xsan_uuid_t vdisk_id, uint64_t size_bytes);

/**
 * Delete a libvirt storage volume
 * @param pool_name Name of the storage pool
 * @param vol_name Name of the volume to delete
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_delete_volume(const char *pool_name, const char *vol_name);

/**
 * Attach XSAN virtual disk to a VM
 * @param vm_name Name of the virtual machine
 * @param vdisk_id UUID of the XSAN virtual disk
 * @param target_dev Target device name (e.g., "vda", "vdb")
 * @param readonly Whether to attach as read-only
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_attach_disk(const char *vm_name, xsan_uuid_t vdisk_id,
                                   const char *target_dev, bool readonly);

/**
 * Detach XSAN virtual disk from a VM
 * @param vm_name Name of the virtual machine
 * @param target_dev Target device name to detach
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_detach_disk(const char *vm_name, const char *target_dev);

/**
 * Get VM information
 * @param vm_name Name of the virtual machine
 * @param vm_id Pointer to store VM UUID
 * @param vm_state Pointer to store VM state
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_get_vm_info(const char *vm_name, xsan_uuid_t *vm_id, int *vm_state);

/**
 * List all VMs using XSAN storage
 * @param vm_names Array to store VM names
 * @param max_vms Maximum number of VMs to return
 * @param vm_count Pointer to store actual number of VMs returned
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_list_vms(char vm_names[][XSAN_MAX_NAME_LEN], uint32_t max_vms, uint32_t *vm_count);

/**
 * Create VM configuration with XSAN storage
 * @param vm_name Name of the virtual machine
 * @param memory_mb Memory size in MB
 * @param vcpus Number of virtual CPUs
 * @param boot_disk_id UUID of the boot disk
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_create_vm(const char *vm_name, uint32_t memory_mb, uint32_t vcpus,
                                 xsan_uuid_t boot_disk_id);

/**
 * Start a virtual machine
 * @param vm_name Name of the virtual machine
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_start_vm(const char *vm_name);

/**
 * Stop a virtual machine
 * @param vm_name Name of the virtual machine
 * @param force Force shutdown if graceful shutdown fails
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_stop_vm(const char *vm_name, bool force);

/**
 * Migrate VM to another node
 * @param vm_name Name of the virtual machine
 * @param dest_node_id UUID of the destination node
 * @param live_migration Whether to perform live migration
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_migrate_vm(const char *vm_name, xsan_uuid_t dest_node_id, bool live_migration);

/**
 * Create VM snapshot
 * @param vm_name Name of the virtual machine
 * @param snapshot_name Name for the snapshot
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_create_vm_snapshot(const char *vm_name, const char *snapshot_name);

/**
 * Restore VM from snapshot
 * @param vm_name Name of the virtual machine
 * @param snapshot_name Name of the snapshot to restore from
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_restore_vm_snapshot(const char *vm_name, const char *snapshot_name);

/**
 * Register event callbacks for VM events
 * @param callback Callback function for VM events
 * @param user_data User data to pass to callback
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_register_events(virConnectDomainEventGenericCallback callback, void *user_data);

/**
 * Get hypervisor capabilities
 * @param capabilities Pointer to store capabilities XML string (caller must free)
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_get_capabilities(char **capabilities);

/**
 * Get node (hypervisor host) information
 * @param hostname Pointer to store hostname
 * @param memory_kb Pointer to store total memory in KB
 * @param cpus Pointer to store number of CPUs
 * @param cpu_mhz Pointer to store CPU frequency in MHz
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_virt_get_node_info(char **hostname, uint64_t *memory_kb, uint32_t *cpus, uint32_t *cpu_mhz);

#endif /* XSAN_VIRTUALIZATION_H */
