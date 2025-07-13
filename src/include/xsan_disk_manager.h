#ifndef XSAN_DISK_MANAGER_H
#define XSAN_DISK_MANAGER_H

#include "xsan_storage.h" // For xsan_disk_t, xsan_disk_group_t, xsan_disk_id_t, etc.
#include "../../include/xsan_error.h"
// We will use xsan_list_t from "xsan_list.h" for internal management.
// #include "xsan_list.h" // Not directly exposed in this header's API for manager instance

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque type for the XSAN Disk Manager.
 * The actual structure definition is internal to disk_manager.c.
 */
typedef struct xsan_disk_manager xsan_disk_manager_t;

/**
 * @brief Initializes the XSAN Disk Manager.
 * This function should be called once during application startup, typically after
 * the SPDK environment is ready and from an SPDK thread.
 * It will discover available SPDK bdevs and populate the initial list of XSAN disks.
 *
 * @param dm_instance_out Pointer to a `xsan_disk_manager_t*`. On successful initialization,
 *                        this will be set to point to the newly created and initialized
 *                        disk manager instance. The caller does not own this pointer directly
 *                        in terms of freeing it; use `xsan_disk_manager_fini`.
 * @return XSAN_OK on success, or an XSAN error code on failure (e.g., XSAN_ERROR_OUT_OF_MEMORY).
 */
xsan_error_t xsan_disk_manager_init(xsan_disk_manager_t **dm_instance_out);

/**
 * @brief Finalizes and cleans up the XSAN Disk Manager.
 * Frees all resources associated with the disk manager, including its lists of
 * disks and disk groups.
 * This should be called once during application shutdown, preferably from an SPDK thread
 * if it needs to interact with SPDK for cleanup (though currently, cleanup is mostly memory).
 *
 * @param dm_ptr Pointer to the disk manager instance pointer. The instance will be freed,
 *               and *dm_ptr will be set to NULL.
 */
void xsan_disk_manager_fini(xsan_disk_manager_t **dm_ptr);

/**
 * @brief Scans for SPDK bdevs and registers/updates them in the disk manager.
 * This function is typically called by `xsan_disk_manager_init` and can also be
 * called to refresh the list of disks (e.g., after a hotplug event, though
 * full hotplug handling is more complex).
 * MUST be called from an SPDK thread.
 *
 * @param dm The disk manager instance. Must not be NULL.
 * @return XSAN_OK on success, or an error code if scanning or registration fails.
 */
xsan_error_t xsan_disk_manager_scan_and_register_bdevs(xsan_disk_manager_t *dm);


// --- Disk Query Operations ---

/**
 * @brief Gets a snapshot list of all currently managed disks.
 * The returned list is an array of pointers to `xsan_disk_t` structures.
 * These structures are internally managed by the disk manager.
 * The caller is responsible for freeing the array of pointers itself using
 * `xsan_disk_manager_free_disk_pointer_list` but NOT the `xsan_disk_t` structures.
 *
 * @param dm The disk manager instance. Must not be NULL.
 * @param disks_array_out Pointer to `xsan_disk_t**` where the array of disk pointers will be stored.
 * @param count_out Pointer to an int where the number of disks in the array will be stored.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM if dm or out params are NULL,
 *         XSAN_ERROR_OUT_OF_MEMORY if allocating the pointer array fails.
 */
xsan_error_t xsan_disk_manager_get_all_disks(xsan_disk_manager_t *dm, xsan_disk_t ***disks_array_out, int *count_out);

/**
 * @brief Frees the array of disk pointers returned by `xsan_disk_manager_get_all_disks`.
 * This function only frees the array itself, not the `xsan_disk_t` elements pointed to,
 * as those are managed by the disk manager.
 *
 * @param disk_ptr_array The array of disk pointers to free.
 */
void xsan_disk_manager_free_disk_pointer_list(xsan_disk_t **disk_ptr_array);

/**
 * @brief Finds a managed disk by its XSAN Disk ID (which is an xsan_uuid_t).
 *
 * @param dm The disk manager instance. Must not be NULL.
 * @param disk_id The ID of the disk to find.
 * @return A pointer to the `xsan_disk_t` if found, otherwise NULL.
 *         The returned pointer is to an internally managed structure and should not be freed by the caller.
 */
xsan_disk_t *xsan_disk_manager_find_disk_by_id(xsan_disk_manager_t *dm, xsan_disk_id_t disk_id);

/**
 * @brief Finds a managed disk by its underlying SPDK bdev name.
 *
 * @param dm The disk manager instance. Must not be NULL.
 * @param bdev_name The SPDK bdev name. Must not be NULL.
 * @return A pointer to the `xsan_disk_t` if found, otherwise NULL.
 *         The returned pointer is to an internally managed structure and should not be freed by the caller.
 */
xsan_disk_t *xsan_disk_manager_find_disk_by_bdev_name(xsan_disk_manager_t *dm, const char *bdev_name);


// --- Disk Group Management Operations ---

/**
 * @brief Creates a new disk group using specified disks (by their bdev names).
 *
 * @param dm The disk manager instance. Must not be NULL.
 * @param group_name The desired name for the new disk group. Must be unique. Must not be NULL.
 * @param group_type The type of the disk group (e.g., XSAN_DISK_GROUP_TYPE_PASSSTHROUGH).
 * @param bdev_names_list An array of const char* pointers, each pointing to an SPDK bdev name.
 * @param num_bdevs The number of bdev names in the `bdev_names_list` array. Must be > 0.
 * @param group_id_out Optional output parameter to store the UUID of the newly created disk group. Can be NULL.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if parameters are invalid (NULLs, empty list, etc.).
 *         XSAN_ERROR_ALREADY_EXISTS if a disk group with the same name already exists.
 *         XSAN_ERROR_NOT_FOUND if one or more specified bdevs (disks) are not found or are already in use.
 *         XSAN_ERROR_OUT_OF_MEMORY if memory allocation fails.
 */
xsan_error_t xsan_disk_manager_disk_group_create(xsan_disk_manager_t *dm,
                                                 const char *group_name,
                                                 xsan_disk_group_type_t group_type,
                                                 const char *bdev_names_list[],
                                                 int num_bdevs,
                                                 xsan_group_id_t *group_id_out);

/**
 * @brief Deletes an existing disk group by its ID.
 * Disks that were part of this group become available (unassigned).
 *
 * @param dm The disk manager instance. Must not be NULL.
 * @param group_id The UUID of the disk group to delete.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if dm is NULL.
 *         XSAN_ERROR_NOT_FOUND if no disk group with the given ID exists.
 *         XSAN_ERROR_RESOURCE_BUSY if the disk group cannot be deleted (e.g., has active volumes).
 */
xsan_error_t xsan_disk_manager_disk_group_delete(xsan_disk_manager_t *dm, xsan_group_id_t group_id);

/**
 * @brief Gets a snapshot list of all currently managed disk groups.
 * Similar to `xsan_disk_manager_get_all_disks`, the caller frees the array of pointers
 * using `xsan_disk_manager_free_group_pointer_list`.
 *
 * @param dm The disk manager instance. Must not be NULL.
 * @param groups_array_out Pointer to `xsan_disk_group_t**`.
 * @param count_out Pointer to store the number of groups.
 * @return XSAN_OK on success.
 */
xsan_error_t xsan_disk_manager_get_all_disk_groups(xsan_disk_manager_t *dm, xsan_disk_group_t ***groups_array_out, int *count_out);

/**
 * @brief Frees the array of disk group pointers returned by `xsan_disk_manager_get_all_disk_groups`.
 *
 * @param group_ptr_array The array of disk group pointers to free.
 */
void xsan_disk_manager_free_group_pointer_list(xsan_disk_group_t **group_ptr_array);

/**
 * @brief Finds a disk group by its XSAN Group ID (UUID).
 *
 * @param dm The disk manager instance. Must not be NULL.
 * @param group_id The ID of the disk group.
 * @return Pointer to `xsan_disk_group_t` if found, NULL otherwise. Pointer is to internal data.
 */
xsan_disk_group_t *xsan_disk_manager_find_disk_group_by_id(xsan_disk_manager_t *dm, xsan_group_id_t group_id);

/**
 * @brief Finds a disk group by its name.
 *
 * @param dm The disk manager instance. Must not be NULL.
 * @param name The name of the disk group. Must not be NULL.
 * @return Pointer to `xsan_disk_group_t` if found, NULL otherwise. Pointer is to internal data.
 */
xsan_disk_group_t *xsan_disk_manager_find_disk_group_by_name(xsan_disk_manager_t *dm, const char *name);


// --- Disk Group Space Allocation Operations ---

/**
 * @brief Allocates a set of physical extents from a disk group for a volume.
 * This is a simplified initial implementation assuming mostly contiguous allocation
 * or simple JBOD-like spanning.
 *
 * @param dm The disk manager instance.
 * @param group_id ID of the disk group to allocate from.
 * @param total_blocks_needed Total number of logical blocks the volume needs from this group
 *                            (in terms of volume's logical block size).
 * @param volume_logical_block_size The logical block size of the volume requesting space.
 * @param extents_out Pointer to an array of xsan_volume_extent_mapping_t.
 *                    On success, this will be allocated by the function and filled with
 *                    the details of the allocated physical extents. The caller is responsible
 *                    for freeing this array using XSAN_FREE().
 * @param num_extents_out Pointer to store the number of extents in the allocated array.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if inputs are invalid.
 *         XSAN_ERROR_NOT_FOUND if the disk group is not found.
 *         XSAN_ERROR_INSUFFICIENT_SPACE if the group doesn't have enough free space.
 *         XSAN_ERROR_OUT_OF_MEMORY on memory allocation failure for extents_out.
 */
xsan_error_t xsan_disk_group_allocate_extents(xsan_disk_manager_t *dm,
                                              xsan_group_id_t group_id,
                                              uint64_t total_blocks_needed,
                                              uint32_t volume_logical_block_size,
                                              xsan_volume_extent_mapping_t **extents_out,
                                              uint32_t *num_extents_out);

/**
 * @brief Frees a set of physical extents previously allocated to a volume from a disk group.
 * This is a simplified initial implementation. True space reclamation for future reuse
 * might be limited if not using a proper free space map (bitmap/freelist).
 *
 * @param dm The disk manager instance.
 * @param group_id ID of the disk group from which space was allocated.
 * @param extents Pointer to an array of xsan_volume_extent_mapping_t describing the extents to free.
 * @param num_extents The number of extents in the array.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if inputs are invalid.
 *         XSAN_ERROR_NOT_FOUND if the disk group or specified physical disks within extents are not found.
 */
xsan_error_t xsan_disk_group_free_extents(xsan_disk_manager_t *dm,
                                          xsan_group_id_t group_id,
                                          const xsan_volume_extent_mapping_t *extents,
                                          uint32_t num_extents);


#ifdef __cplusplus
}
#endif

#endif // XSAN_DISK_MANAGER_H
