#ifndef XSAN_VOLUME_MANAGER_H
#define XSAN_VOLUME_MANAGER_H

#include "xsan_storage.h" // For xsan_volume_t, xsan_group_id_t, xsan_volume_id_t, xsan_disk_id_t
#include "xsan_error.h"   // For xsan_error_t
#include "xsan_disk_manager.h" // For xsan_disk_manager_t (as a dependency)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque type for the XSAN Volume Manager.
 * The actual structure definition is internal to volume_manager.c.
 */
typedef struct xsan_volume_manager xsan_volume_manager_t;

/**
 * @brief Initializes the XSAN Volume Manager.
 * This function should be called once during application startup, after the
 * Disk Manager has been initialized. It requires a pointer to the initialized
 * Disk Manager to interact with disk groups.
 *
 * @param disk_manager An initialized XSAN Disk Manager instance. Must not be NULL.
 * @param vm_instance_out Pointer to a `xsan_volume_manager_t*`. On successful initialization,
 *                        this will be set to point to the newly created Volume Manager instance.
 * @return XSAN_OK on success, or an XSAN error code on failure (e.g., XSAN_ERROR_OUT_OF_MEMORY).
 */
xsan_error_t xsan_volume_manager_init(xsan_disk_manager_t *disk_manager, xsan_volume_manager_t **vm_instance_out);

/**
 * @brief Finalizes and cleans up the XSAN Volume Manager.
 * Frees all resources associated with the volume manager, including its list of
 * managed volumes.
 *
 * @param vm_ptr Pointer to the volume manager instance pointer. The instance will be freed,
 *               and *vm_ptr will be set to NULL.
 */
void xsan_volume_manager_fini(xsan_volume_manager_t **vm_ptr);

/**
 * @brief Creates a new logical volume.
 *
 * @param vm The volume manager instance. Must not be NULL.
 * @param name The desired name for the new volume. Must be unique. Must not be NULL.
 * @param size_bytes The total provisioned size of the volume in bytes. Must be > 0.
 * @param group_id The ID of the disk group where this volume will be primarily stored.
 * @param logical_block_size_bytes The logical block size for this volume (e.g., 512 or 4096). Must be > 0.
 * @param thin_provisioned If true, the volume will be thin-provisioned.
 * @param new_volume_id_out Optional output parameter to store the UUID of the newly created volume. Can be NULL.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if parameters are invalid.
 *         XSAN_ERROR_ALREADY_EXISTS if a volume with the same name already exists.
 *         XSAN_ERROR_NOT_FOUND if the specified disk group ID does not exist or is not suitable.
 *         XSAN_ERROR_INSUFFICIENT_SPACE if the disk group does not have enough capacity (for thick, or initial for thin).
 *         XSAN_ERROR_OUT_OF_MEMORY if memory allocation fails.
 */
xsan_error_t xsan_volume_create(xsan_volume_manager_t *vm,
                                const char *name,
                                uint64_t size_bytes,
                                xsan_group_id_t group_id,
                                uint32_t logical_block_size_bytes,
                                bool thin_provisioned,
                                xsan_volume_id_t *new_volume_id_out);

/**
 * @brief Deletes an existing logical volume by its ID.
 *
 * @param vm The volume manager instance. Must not be NULL.
 * @param volume_id The UUID of the volume to delete.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if vm is NULL.
 *         XSAN_ERROR_NOT_FOUND if no volume with the given ID exists.
 *         XSAN_ERROR_RESOURCE_BUSY if the volume cannot be deleted (e.g., in use).
 */
xsan_error_t xsan_volume_delete(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id);

/**
 * @brief Retrieves a managed volume by its XSAN Volume ID (UUID).
 *
 * @param vm The volume manager instance. Must not be NULL.
 * @param volume_id The ID of the volume to find.
 * @return A pointer to the `xsan_volume_t` if found, otherwise NULL.
 *         The returned pointer is to an internally managed structure and should not be freed by the caller.
 */
xsan_volume_t *xsan_volume_get_by_id(xsan_volume_manager_t *vm, xsan_volume_id_t volume_id);

/**
 * @brief Retrieves a managed volume by its name.
 *
 * @param vm The volume manager instance. Must not be NULL.
 * @param name The name of the volume to find. Must not be NULL.
 * @return A pointer to the `xsan_volume_t` if found, otherwise NULL.
 *         The returned pointer is to an internally managed structure and should not be freed by the caller.
 */
xsan_volume_t *xsan_volume_get_by_name(xsan_volume_manager_t *vm, const char *name);

/**
 * @brief Gets a snapshot list of all currently managed logical volumes.
 * The returned list is an array of pointers to `xsan_volume_t` structures.
 * These structures are internally managed. The caller is responsible for freeing
 * the array of pointers itself using `xsan_volume_manager_free_volume_pointer_list`.
 *
 * @param vm The volume manager instance. Must not be NULL.
 * @param volumes_array_out Pointer to `xsan_volume_t**` where the array of volume pointers will be stored.
 * @param count_out Pointer to an int where the number of volumes in the array will be stored.
 * @return XSAN_OK on success, or an error code.
 */
xsan_error_t xsan_volume_list_all(xsan_volume_manager_t *vm, xsan_volume_t ***volumes_array_out, int *count_out);

/**
 * @brief Frees the array of volume pointers returned by `xsan_volume_list_all`.
 * This function only frees the array itself, not the `xsan_volume_t` elements.
 *
 * @param volume_ptr_array The array of volume pointers to free.
 */
void xsan_volume_manager_free_volume_pointer_list(xsan_volume_t **volume_ptr_array);

/**
 * @brief Maps a logical block address (LBA) within a volume to a physical disk and its LBA.
 * This is a crucial function for the I/O path. The initial implementation will be simple,
 * assuming a volume maps linearly to the first disk in its assigned disk group.
 *
 * @param vm The volume manager instance. Must not be NULL.
 * @param volume_id The ID of the volume.
 * @param logical_block_idx The logical block index within the volume.
 * @param out_disk_id Pointer to store the XSAN ID of the target physical disk. Must not be NULL.
 * @param out_physical_block_idx Pointer to store the physical block index on the target disk. Must not be NULL.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if parameters are invalid.
 *         XSAN_ERROR_NOT_FOUND if the volume or its underlying disk/group is not found.
 *         XSAN_ERROR_OUT_OF_BOUNDS if logical_block_idx is outside the volume's range.
 */
xsan_error_t xsan_volume_map_lba_to_physical(xsan_volume_manager_t *vm,
                                             xsan_volume_id_t volume_id,
                                             uint64_t logical_block_idx,
                                             xsan_disk_id_t *out_disk_id,
                                             uint64_t *out_physical_block_idx,
                                             uint32_t *out_physical_block_size); // Also return physical block size

/**
 * @brief Reads data asynchronously from a logical volume.
 *
 * @param vm The volume manager instance.
 * @param volume_id The ID of the volume to read from.
 * @param logical_byte_offset The starting byte offset within the logical volume.
 * @param length_bytes The number of bytes to read.
 * @param user_buf The buffer where the read data will be stored.
 *                 This buffer might be used directly if DMA-safe and aligned, or data
 *                 might be copied into it from an internal DMA buffer.
 * @param user_cb The user's completion callback function.
 * @param user_cb_arg Argument for the user's callback.
 * @return XSAN_OK if the read operation was successfully submitted.
 *         An xsan_error_t code on failure (e.g., volume not found, invalid params, submission error).
 *         The actual I/O result is delivered via the user_cb.
 */
xsan_error_t xsan_volume_read_async(xsan_volume_manager_t *vm,
                                    xsan_volume_id_t volume_id,
                                    uint64_t logical_byte_offset,
                                    uint64_t length_bytes,
                                    void *user_buf,
                                    xsan_user_io_completion_cb_t user_cb,
                                    void *user_cb_arg);

/**
 * @brief Writes data asynchronously to a logical volume.
 *
 * @param vm The volume manager instance.
 * @param volume_id The ID of the volume to write to.
 * @param logical_byte_offset The starting byte offset within the logical volume.
 * @param length_bytes The number of bytes to write.
 * @param user_buf The buffer containing the data to write.
 *                 This buffer might be used directly or data copied from it to an internal DMA buffer.
 * @param user_cb The user's completion callback function.
 * @param user_cb_arg Argument for the user's callback.
 * @return XSAN_OK if the write operation was successfully submitted.
 *         An xsan_error_t code on failure.
 *         The actual I/O result is delivered via the user_cb.
 */
xsan_error_t xsan_volume_write_async(xsan_volume_manager_t *vm,
                                     xsan_volume_id_t volume_id,
                                     uint64_t logical_byte_offset,
                                     uint64_t length_bytes,
                                     const void *user_buf, // const for write
                                     xsan_user_io_completion_cb_t user_cb,
                                     void *user_cb_arg);


// Potentially add functions for resizing volumes, snapshots, etc. in the future.

#ifdef __cplusplus
}
#endif

#endif // XSAN_VOLUME_MANAGER_H
