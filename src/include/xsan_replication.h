#ifndef XSAN_REPLICATION_H
#define XSAN_REPLICATION_H

#include "xsan_types.h"    // For xsan_error_t
#include "xsan_io.h"       // For xsan_io_request_t, xsan_user_io_completion_cb_t
#include "xsan_storage.h"  // For XSAN_MAX_REPLICAS, xsan_volume_id_t
#include <pthread.h>       // For pthread_mutex_t (if needed for future concurrent access)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Context for a replicated I/O operation (primarily write).
 * This structure tracks the original user I/O request and the status
 * of its constituent sub-operations (local write and remote replica writes).
 */
typedef struct xsan_replicated_io_ctx {
    // Original I/O request details (or a pointer to it if it's managed separately)
    // If original_user_io_req is a pointer, its lifetime needs careful management.
    // For simplicity, let's embed key info or assume this context takes over some aspects.
    xsan_volume_id_t volume_id;
    void *user_buffer;                  // Original user buffer (for data source on write)
    uint64_t logical_byte_offset;       // Original logical byte offset in the volume
    uint64_t length_bytes;              // Original length of the I/O in bytes
    // uint32_t logical_block_size;     // Volume's logical block size (needed for LBA calculations)

    xsan_user_io_completion_cb_t original_user_cb; ///< The user's callback to call when all replicas are done.
    void *original_user_cb_arg;         ///< Argument for the user's callback.

    uint32_t total_replicas_targeted;   ///< Total number of replicas this I/O targets (e.g., FTT + 1)
    uint32_t successful_writes;         ///< Count of successfully completed replica writes (local or remote)
    uint32_t failed_writes;             ///< Count of failed replica writes

    // For tracking individual replica write statuses and potentially retries (more advanced)
    // For now, just counts are used to determine overall success based on a policy (e.g., all must succeed).
    // xsan_error_t replica_op_status[XSAN_MAX_REPLICAS];
    // bool replica_op_completed[XSAN_MAX_REPLICAS];

    xsan_error_t final_status;          ///< Overall status of the replicated write operation.

    // If this context needs to be looked up (e.g., by transaction ID when a remote response arrives)
    uint64_t transaction_id;            ///< Transaction ID linking this replicated write.

    // Could add a pointer to the xsan_volume_t for easy access to replication config,
    // or pass it around as needed.
    // struct xsan_volume *volume;

    // Add a simple lock if different callbacks might update counts concurrently,
    // though typically SPDK callbacks for a single logical operation resolve on one thread or are serialized.
    // pthread_mutex_t lock;

    // Internal IO request for the local part of the replicated write
    xsan_io_request_t *local_io_req;

    // Add fields to track remote replica send operations if needed, e.g.,
    // struct xsan_pending_replica_send {
    //    xsan_node_id_t node_id;
    //    bool send_initiated;
    //    bool ack_received;
    //    xsan_error_t ack_status;
    // } remote_sends[XSAN_MAX_REPLICAS -1]; // If actual_replica_count > 1
    // Consider adding a per-replica sub-context array if complex state per replica is needed.
} xsan_replicated_io_ctx_t;


/**
 * @brief Context for a single operation targeted at one specific (usually remote) replica.
 * This is used by both replicated writes (for each remote replica) and by the read coordinator
 * (for the current remote read attempt).
 */
typedef struct xsan_per_replica_op_ctx {
    void *parent_rep_ctx;                      ///< Pointer to the parent context (e.g., xsan_replicated_io_ctx_t or xsan_replica_read_coordinator_ctx_t)
    xsan_replica_location_t replica_location_info; ///< Information about the target replica node
    struct xsan_message *request_msg_to_send;  ///< The protocol message to send to the replica
    struct spdk_sock *connected_sock;          ///< Socket, if connection is established and reused
    // Add any other state needed for this specific per-replica operation,
    // e.g., retry count for this specific replica, timeout timer.
    // uint32_t current_attempt_retries;
} xsan_per_replica_op_ctx_t;


/**
 * @brief Context for coordinating a read operation that might try multiple replicas.
 */
typedef struct xsan_replica_read_coordinator_ctx {
    struct xsan_volume *vol;            ///< Pointer to the volume being read (non-owning)
    void *user_buffer;                  ///< User's original buffer to store read data
    uint64_t logical_byte_offset;       ///< Original logical byte offset in the volume
    uint64_t length_bytes;              ///< Original length of the I/O in bytes
    // uint32_t volume_logical_block_size; ///< Volume's logical block size

    xsan_user_io_completion_cb_t original_user_cb; ///< User's final callback
    void *original_user_cb_arg;         ///< Argument for the user's callback

    int current_replica_idx_to_try;     ///< Index into vol->replica_nodes[] for current attempt
    // int total_replicas_in_volume;    // vol->actual_replica_count can be used

    xsan_error_t last_attempt_status;   ///< Status from the most recent read attempt

    uint64_t transaction_id;            ///< Transaction ID for remote read REQ/RESP matching

    // For reads from remote replicas, data is first read into this DMA buffer,
    // then copied to user_buffer in the final completion step.
    void *internal_dma_buffer;          ///< DMA buffer for receiving data from remote replica
    size_t internal_dma_buffer_size;    ///< Size of the allocated internal_dma_buffer
    bool internal_dma_buffer_allocated; ///< True if internal_dma_buffer was allocated by this context

    // Context for the current remote operation (connect and send REQ)
    // This reuses the per-replica op context structure, but only one is active at a time for reads.
    xsan_per_replica_op_ctx_t *current_remote_op_ctx;

    // Could also store a list of already tried replica indices to avoid retrying the same failed one immediately.
} xsan_replica_read_coordinator_ctx_t;


/**
 * @brief Allocates and initializes a replicated I/O context.
 *
 * @param original_user_cb User's original completion callback.
 * @param original_user_cb_arg Argument for the user's callback.
 * @param vol Pointer to the volume being written to (to get FTT, replica info).
 * @param user_buffer User's data buffer for the write.
 * @param offset Original byte offset of the write.
 * @param length Original length of the write in bytes.
 * @param transaction_id A unique transaction ID for this replicated operation.
 * @return Pointer to the allocated xsan_replicated_io_ctx_t, or NULL on failure.
 */
xsan_replicated_io_ctx_t *xsan_replicated_io_ctx_create(
    xsan_user_io_completion_cb_t original_user_cb,
    void *original_user_cb_arg,
    struct xsan_volume *vol, // Pass the volume to get FTT, replica_count, volume_id
    const void *user_buffer, // const because it's a write source
    uint64_t offset,
    uint64_t length,
    uint64_t transaction_id
);

/**
 * @brief Frees a replicated I/O context.
 * Make sure any associated resources (like internal io_reqs if not auto-freed) are handled.
 *
 * @param rep_io_ctx The context to free.
 */
void xsan_replicated_io_ctx_free(xsan_replicated_io_ctx_t *rep_io_ctx);


/**
 * @brief Allocates and initializes a replica read coordinator context.
 *
 * @param vol Pointer to the volume to read from.
 * @param user_buffer User's buffer for the read data.
 * @param offset_bytes Byte offset within the volume.
 * @param length_bytes Number of bytes to read.
 * @param original_user_cb User's final completion callback.
 * @param original_user_cb_arg Argument for the user's callback.
 * @param transaction_id Transaction ID for potential remote read requests.
 * @return Pointer to an allocated xsan_replica_read_coordinator_ctx_t, or NULL on failure.
 */
xsan_replica_read_coordinator_ctx_t *xsan_replica_read_coordinator_ctx_create(
    struct xsan_volume *vol,
    void *user_buffer,
    uint64_t offset_bytes,
    uint64_t length_bytes,
    xsan_user_io_completion_cb_t original_user_cb,
    void *original_user_cb_arg,
    uint64_t transaction_id
);

/**
 * @brief Frees a replica read coordinator context.
 * Also frees any internally allocated DMA buffer.
 *
 * @param read_coord_ctx The context to free.
 */
void xsan_replica_read_coordinator_ctx_free(xsan_replica_read_coordinator_ctx_t *read_coord_ctx);


#ifdef __cplusplus
}
#endif

#endif // XSAN_REPLICATION_H
