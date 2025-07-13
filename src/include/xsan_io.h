#include "xsan_storage.h" // For xsan_volume_t, xsan_disk_t (potentially needed for context)

#include "xsan_storage.h" // For xsan_volume_t
#ifndef XSAN_IO_H
#define XSAN_IO_H

#include "xsan_types.h"
#include "../../include/xsan_error.h"
#include "xsan_storage.h" // For xsan_volume_t, xsan_disk_t (potentially needed for context)
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> // For size_t

// Forward declarations for SPDK types used in xsan_io_request_t if not included directly
struct spdk_bdev_desc;
struct spdk_io_channel;
struct spdk_bdev_io;

#ifdef __cplusplus
extern "C" {
#endif


// xsan_user_io_completion_cb_t 类型已在 xsan_types.h 定义，无需重复声明

/**
 * @brief Structure to encapsulate an XSAN I/O request.
 * This structure tracks all necessary information for an asynchronous I/O operation
 * through its lifecycle, from submission to completion.
 * An instance of this is typically allocated per I/O operation.
 */
typedef struct xsan_io_request {
    // --- User-provided parameters / context ---
    xsan_volume_id_t target_volume_id;  ///< ID of the target logical volume (if applicable)
    xsan_disk_id_t target_disk_id;      ///< ID of the target physical disk (resolved from volume or direct)
    char target_bdev_name[XSAN_MAX_NAME_LEN]; ///< Name of the target SPDK bdev

    bool is_read_op;                    ///< True for read, false for write

    void *user_buffer;                  ///< User's original data buffer (for final copy on read, or source on write)
    uint64_t user_buffer_offset_bytes;  ///< Offset within the user_buffer (if handling partial block copies)

    uint64_t offset_bytes;              ///< Byte offset for the I/O operation on the target (volume or disk)
    uint64_t length_bytes;              ///< Length of the I/O operation in bytes

    // For block-based operations (derived from offset_bytes/length_bytes and block_size)
    uint64_t offset_blocks;             ///< Starting block number
    uint32_t num_blocks;                ///< Number of blocks
    uint32_t block_size_bytes;          ///< Block size for this operation

    xsan_user_io_completion_cb_t user_cb; ///< User's callback function for completion notification
    void *user_cb_arg;                  ///< Argument for the user's callback

    // --- Internal state and resources ---
    void *dma_buffer;                   ///< Pointer to DMA-safe buffer used for SPDK I/O
    bool dma_buffer_is_internal;        ///< True if dma_buffer was allocated internally by xsan_io module
    size_t dma_buffer_size;             ///< Size of the allocated dma_buffer

    struct spdk_bdev_desc *bdev_desc;   ///< SPDK bdev descriptor (owned by the IO request or managed externally)
    struct spdk_io_channel *io_channel; ///< SPDK I/O channel (owned by the IO request or managed externally)
    bool own_spdk_resources;            ///< True if this request opened/got the bdev_desc and io_channel and should release them

    xsan_error_t status;                ///< Final status of the I/O operation

    // Could add retry counts, timestamps, etc. for more advanced features
    // int retries_left;

    // For linking into lists if the IO scheduler manages queues of requests
    struct xsan_io_request *next;
    struct xsan_io_request *prev;

    // Potentially a pointer back to the I/O scheduler or volume context if needed
    // void *parent_scheduler;

} xsan_io_request_t;


/**
 * @brief Allocates and initializes an xsan_io_request_t structure.
 *
 * @param target_volume_id ID of the volume (can be zeroed if targeting a disk directly).
 * @param user_buffer User's data buffer.
 * @param offset_bytes Byte offset for the I/O.
 * @param length_bytes Length of the I/O in bytes.
 * @param block_size_bytes The block size to be used for calculating num_blocks and offset_blocks.
 * @param is_read True for a read operation, false for write.
 * @param user_cb User completion callback.
 * @param user_cb_arg Argument for user callback.
 * @return Pointer to a new xsan_io_request_t, or NULL on allocation failure.
 *         The caller is responsible for populating target_disk_id/target_bdev_name
 *         and SPDK resources (desc, channel) or using functions that manage them.
 */
xsan_io_request_t *xsan_io_request_create(
    xsan_volume_id_t target_volume_id,
    void *user_buffer,
    uint64_t offset_bytes,
    uint64_t length_bytes,
    uint32_t block_size_bytes,
    bool is_read,
    xsan_user_io_completion_cb_t user_cb,
    void *user_cb_arg
);

/**
 * @brief Frees an xsan_io_request_t structure.
 * If dma_buffer_is_internal is true, it also frees the internal DMA buffer.
 * It does NOT automatically close/put bdev_desc or io_channel unless own_spdk_resources is true
 * and specific logic is added for that. Resource management for desc/channel is complex.
 *
 * @param io_req The I/O request to free.
 */
void xsan_io_request_free(xsan_io_request_t *io_req);

/**
 * @brief Submits an I/O request to the appropriate SPDK bdev.
 * This is the core function that translates an XSAN I/O request to an SPDK I/O.
 * It handles DMA buffer preparation (if needed and configured in io_req),
 * and calls the underlying SPDK async read/write block functions.
 * The bdev_desc and io_channel must be valid and set in io_req before calling this,
 * or this function needs to acquire them (which adds complexity).
 *
 * This function MUST be called from an SPDK thread.
 *
 * @param io_req The fully populated I/O request.
 * @return XSAN_OK if the I/O was successfully submitted to SPDK.
 *         XSAN_ERROR_INVALID_PARAM if io_req or its critical members are invalid.
 *         XSAN_ERROR_OUT_OF_MEMORY if SPDK cannot allocate an spdk_bdev_io.
 *         Other XSAN_ERROR codes for SPDK submission failures.
 */
xsan_error_t xsan_io_submit_request_to_bdev(xsan_io_request_t *io_req);


#ifdef __cplusplus
}
#endif

#endif // XSAN_IO_H
