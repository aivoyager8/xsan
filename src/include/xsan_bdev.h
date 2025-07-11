#ifndef XSAN_BDEV_H
#define XSAN_BDEV_H

#include "xsan_types.h" // For xsan_uuid_t, xsan_error_t, XSAN_MAX_NAME_LEN
#include <stdint.h>     // For uint64_t, uint32_t
#include <stddef.h>     // For size_t
#include <stdbool.h>    // For bool

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration for the SPDK bdev structure.
// This allows us to potentially store a pointer to it without fully including spdk/bdev.h here,
// though implementation in .c file will need the full SPDK headers.
struct spdk_bdev;

/**
 * @brief Structure to hold information about an SPDK block device (bdev).
 * This mirrors some key fields from spdk_bdev and xsan_disk_t.
 */
typedef struct {
    char name[XSAN_MAX_NAME_LEN];          ///< Name of the bdev (e.g., "Nvme0n1", "Malloc0")
    xsan_uuid_t uuid;                      ///< SPDK bdev UUID (if available)
    uint64_t num_blocks;                   ///< Total number of blocks
    uint32_t block_size;                   ///< Size of a single block in bytes
    uint64_t capacity_bytes;               ///< Total capacity in bytes (num_blocks * block_size)
    char product_name[XSAN_MAX_NAME_LEN];  ///< Product name (e.g., "INTEL SSDPED1K750GA", "Malloc Bdev")

    // Additional useful information from spdk_bdev
    bool is_rotational;                    ///< True if the bdev is a rotational device (HDD)
    uint32_t optimal_io_boundary;          ///< Optimal I/O boundary in blocks, if reported
    bool has_write_cache;                  ///< True if the device has a volatile write cache enabled
    // void *internal_spdk_bdev_ptr;      // Consider if exposing the raw spdk_bdev pointer is safe/needed here.
                                           // If added, its lifetime must be carefully managed.
} xsan_bdev_info_t;

/**
 * @brief Initializes the XSAN bdev management subsystem.
 * This function should be called once from an SPDK reactor thread after SPDK has fully started
 * (e.g., from the main application callback passed to xsan_spdk_manager_start_app).
 * It can perform initial setup, like registering for bdev hotplug events if needed.
 *
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_bdev_subsystem_init(void);

/**
 * @brief Shuts down the XSAN bdev management subsystem.
 * This should be called from an SPDK reactor thread during application shutdown,
 * before spdk_app_fini() or xsan_spdk_manager_app_fini().
 * It unregisters event handlers and cleans up resources.
 */
void xsan_bdev_subsystem_fini(void);

/**
 * @brief Retrieves a list of all currently available SPDK block devices.
 *
 * The caller is responsible for freeing the allocated list of `xsan_bdev_info_t` structures
 * and the array itself using `xsan_bdev_list_free`.
 * This function MUST be called from an SPDK reactor thread.
 *
 * @param bdev_list_out Pointer to a `xsan_bdev_info_t*`. On success, this will point to
 *                      an allocated array of `xsan_bdev_info_t` structures.
 * @param count_out Pointer to an int where the number of bdevs found will be stored.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_OUT_OF_MEMORY if memory allocation fails.
 *         XSAN_ERROR_INVALID_PARAM if out pointers are NULL.
 *         Other XSAN_ERROR codes if an underlying SPDK operation fails.
 */
xsan_error_t xsan_bdev_list_get_all(xsan_bdev_info_t **bdev_list_out, int *count_out);

/**
 * @brief Frees a list of bdev information previously obtained from `xsan_bdev_list_get_all`.
 * This function frees each `xsan_bdev_info_t` structure in the array (if they were individually allocated,
 * which they are in the typical implementation) and then frees the array itself.
 *
 * @param bdev_list The array of `xsan_bdev_info_t` structures to free.
 * @param count The number of elements in the array (as returned by `xsan_bdev_list_get_all`).
 */
void xsan_bdev_list_free(xsan_bdev_info_t *bdev_list, int count);

/**
 * @brief Gets detailed information for a specific bdev by its name.
 *
 * The caller is responsible for freeing the returned `xsan_bdev_info_t` structure
 * using `xsan_bdev_info_free()`.
 * This function MUST be called from an SPDK reactor thread.
 *
 * @param bdev_name The name of the bdev to find. Must not be NULL.
 * @return Pointer to an allocated `xsan_bdev_info_t` structure if found,
 *         or NULL if the bdev is not found, if `bdev_name` is NULL, or on memory allocation error.
 */
xsan_bdev_info_t *xsan_bdev_get_info_by_name(const char *bdev_name);

/**
 * @brief Frees an `xsan_bdev_info_t` structure allocated by `xsan_bdev_get_info_by_name`.
 *
 * @param bdev_info Pointer to the `xsan_bdev_info_t` structure to free.
 *                  If NULL, the function does nothing.
 */
void xsan_bdev_info_free(xsan_bdev_info_t *bdev_info);

/**
 * @brief Reads blocks of data from a specified bdev.
 * This function provides a simplified synchronous-like interface by internally handling
 * the asynchronous SPDK I/O and polling for its completion.
 * It MUST be called from an SPDK reactor thread.
 *
 * @param bdev_name The name of the bdev to read from. Must not be NULL.
 * @param offset_blocks The starting block offset to read from.
 * @param num_blocks The number of blocks to read. Must be > 0.
 * @param user_buf The application buffer where read data will be copied. Must not be NULL.
 * @param user_buf_len The length of the `user_buf` in bytes. Must be at least num_blocks * (bdev block_size).
 * @param use_internal_dma_alloc If true, this function allocates/frees an internal DMA buffer, and copies data to `user_buf`.
 *                               If false, `user_buf` itself is assumed to be DMA-safe and directly used.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if parameters are invalid (e.g. NULL pointers, zero num_blocks, insufficient buffer).
 *         XSAN_ERROR_NOT_FOUND if bdev is not found.
 *         XSAN_ERROR_OUT_OF_MEMORY for DMA memory allocation failures.
 *         XSAN_ERROR_IO on SPDK I/O error during read.
 *         XSAN_ERROR_THREAD_CONTEXT if not called from an SPDK thread.
 */
xsan_error_t xsan_bdev_read_sync(const char *bdev_name,
                                 uint64_t offset_blocks,
                                 uint32_t num_blocks,
                                 void *user_buf,
                                 size_t user_buf_len,
                                 bool use_internal_dma_alloc);

/**
 * @brief Writes blocks of data to a specified bdev.
 * Similar to xsan_bdev_read_sync, this provides a simplified synchronous-like interface.
 * It MUST be called from an SPDK reactor thread.
 *
 * @param bdev_name The name of the bdev to write to. Must not be NULL.
 * @param offset_blocks The starting block offset to write to.
 * @param num_blocks The number of blocks to write. Must be > 0.
 * @param user_buf The application buffer containing the data to write. Must not be NULL.
 * @param user_buf_len The length of the `user_buf` in bytes. Must be at least num_blocks * (bdev block_size).
 * @param use_internal_dma_alloc If true, this function allocates an internal DMA buffer, copies data from `user_buf`,
 *                               performs the write, and then frees the DMA buffer.
 *                               If false, `user_buf` itself is assumed to be DMA-safe and directly used.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if parameters are invalid.
 *         XSAN_ERROR_NOT_FOUND if bdev is not found.
 *         XSAN_ERROR_OUT_OF_MEMORY for DMA memory allocation failures.
 *         XSAN_ERROR_IO on SPDK I/O error during write.
 *         XSAN_ERROR_THREAD_CONTEXT if not called from an SPDK thread.
 */
xsan_error_t xsan_bdev_write_sync(const char *bdev_name,
                                  uint64_t offset_blocks,
                                  uint32_t num_blocks,
                                  const void *user_buf,
                                  size_t user_buf_len,
                                  bool use_internal_dma_alloc);

/**
 * @brief Allocates a DMA-safe memory buffer suitable for SPDK I/O.
 * The buffer will be aligned to `align` bytes. Memory is zeroed.
 *
 * @param size The size of the buffer to allocate in bytes. Must be > 0.
 * @param align The required alignment of the buffer (e.g., spdk_bdev_get_buf_align(bdev)).
 *              If 0, a default SPDK alignment (typically page size) will be used by spdk_dma_malloc.
 * @return Pointer to the allocated DMA-safe buffer, or NULL on failure (OOM or invalid size).
 */
void *xsan_bdev_dma_malloc(size_t size, size_t align);

/**
 * @brief Frees a DMA-safe memory buffer previously allocated by `xsan_bdev_dma_malloc`.
 *
 * @param buf Pointer to the buffer to free. If NULL, the function does nothing.
 */
void xsan_bdev_dma_free(void *buf);

/**
 * @brief Gets the required buffer alignment for a given bdev.
 * This is useful for allocating DMA buffers that will be used directly with SPDK I/O calls.
 * MUST be called from an SPDK thread.
 *
 * @param bdev_name The name of the bdev. Must not be NULL.
 * @return The required buffer alignment in bytes. Returns 0 if bdev is not found,
 *         if `bdev_name` is NULL, or if not called from an SPDK thread.
 */
size_t xsan_bdev_get_buf_align(const char *bdev_name);


#ifdef __cplusplus
}
#endif

#endif // XSAN_BDEV_H
