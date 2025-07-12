#include "xsan_types.h" // 统一类型定义
#ifndef XSAN_PROTOCOL_H
#define XSAN_PROTOCOL_H

#include "xsan_storage.h" // For xsan_volume_id_t, xsan_storage_state_t, xsan_volume_t
#include "xsan_types.h" // For xsan_error_t
#include <stdint.h>     // For uint32_t, uint64_t
#include <stddef.h>     // For size_t
#include <stdbool.h>    // For bool

#ifdef __cplusplus
extern "C" {
#endif

// Magic number to identify XSAN messages
#define XSAN_PROTOCOL_MAGIC ((uint32_t)0x5853414E) // "XSAN" (ASCII: X S A N)

// Current protocol version
#define XSAN_PROTOCOL_VERSION ((uint16_t)1)

// Maximum payload size for a single message (e.g., 16MB)
// This is a protocol-level limit, not necessarily a TCP segment limit.
#define XSAN_PROTOCOL_MAX_PAYLOAD_SIZE (16 * 1024 * 1024)

/**
 * @brief Defines the types of messages in the XSAN protocol.
 */
typedef enum {
    XSAN_MSG_TYPE_UNDEFINED = 0,    ///< Should not be used

    // Control Plane Messages
    XSAN_MSG_TYPE_HEARTBEAT = 1,          ///< Node heartbeat signal
    XSAN_MSG_TYPE_HEARTBEAT_ACK = 2,      ///< Acknowledgement for heartbeat

    XSAN_MSG_TYPE_NODE_REGISTER_REQ = 10, ///< Request for a node to register with cluster
    XSAN_MSG_TYPE_NODE_REGISTER_RESP = 11,///< Response to node registration

    XSAN_MSG_TYPE_GET_CLUSTER_STATUS_REQ = 20, ///< Request cluster status
    XSAN_MSG_TYPE_GET_CLUSTER_STATUS_RESP = 21,///< Response with cluster status

    // Data Plane Messages (Examples)
    XSAN_MSG_TYPE_READ_BLOCK_REQ = 100,   ///< Request to read a data block
    XSAN_MSG_TYPE_READ_BLOCK_RESP = 101,  ///< Response containing block data or error
    XSAN_MSG_TYPE_WRITE_BLOCK_REQ = 102,  ///< Request to write a data block
    XSAN_MSG_TYPE_WRITE_BLOCK_RESP = 103, ///< Response indicating write success/failure

    // Metadata Operations (Examples)
    XSAN_MSG_TYPE_CREATE_VOLUME_REQ = 200,
    XSAN_MSG_TYPE_CREATE_VOLUME_RESP = 201,
    XSAN_MSG_TYPE_DELETE_VOLUME_REQ = 202,
    XSAN_MSG_TYPE_DELETE_VOLUME_RESP = 203,

    // General Error Response (if a specific response type isn't used for errors)
    XSAN_MSG_TYPE_ERROR_RESP = 500,

    // Data Replication Messages
    XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ = 600,  ///< Request to write a block to a replica
    XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP = 601, ///< Response to a replica write request
    XSAN_MSG_TYPE_REPLICA_READ_BLOCK_REQ = 602,  ///< Request to read a block from a replica
    XSAN_MSG_TYPE_REPLICA_READ_BLOCK_RESP = 603, ///< Response to a replica read request
    // XSAN_MSG_TYPE_REPLICA_SYNC_REQ = 610,      // Future: Request to sync a range of blocks
    // XSAN_MSG_TYPE_REPLICA_SYNC_RESP = 611,     // Future: Response to sync request

    // Add more message types as the protocol evolves
    XSAN_MSG_TYPE_MAX // Sentinel, keep last
} xsan_message_type_t;


// --- Payload Structures for Replication Messages ---

/**
 * @brief Payload for XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_REQ.
 * The actual block data follows this structured payload within the xsan_message_t's payload field.
 */
typedef struct {
    xsan_volume_id_t volume_id;         ///< ID of the volume this block belongs to (on the target replica node)
    // Instead of disk_id, the replica node should know where to place this based on its local volume layout.
    // Or, if the primary dictates exact placement on replica:
    // xsan_disk_id_t target_disk_id_on_replica;
    // char target_bdev_name_on_replica[XSAN_MAX_NAME_LEN];

    uint64_t block_lba_on_volume;       ///< Logical block address within the VOLUME on the replica node
                                        ///< The replica node will map this to its physical storage.
    uint32_t num_blocks;                ///< Number of blocks being written (data length = num_blocks * block_size)
    // uint32_t block_size;             // This should be known from the volume metadata or consistent.
                                        // If it can vary, it needs to be here. Assume consistent for now.
    // uint32_t data_checksum;          // Optional: checksum of the data blocks that follow
} __attribute__((packed)) xsan_replica_write_req_payload_t;

/**
 * @brief Payload for XSAN_MSG_TYPE_REPLICA_WRITE_BLOCK_RESP.
 */
typedef struct {
    xsan_error_t status;                ///< XSAN_OK on success, or an error code.
    uint64_t block_lba_on_volume;       ///< The starting LBA that this response pertains to
    uint32_t num_blocks_processed;      ///< Number of blocks successfully/attempted to process
} __attribute__((packed)) xsan_replica_write_resp_payload_t;

// Size of the structured part of the replica write request payload.
// The actual data blocks will follow this.
#define XSAN_REPLICA_WRITE_REQ_PAYLOAD_SIZE sizeof(xsan_replica_write_req_payload_t)


/**
 * @brief Payload for XSAN_MSG_TYPE_REPLICA_READ_BLOCK_REQ.
 */
typedef struct {
    xsan_volume_id_t volume_id;         ///< ID of the volume this block belongs to
    uint64_t block_lba_on_volume;       ///< Logical block address within the VOLUME to read
    uint32_t num_blocks;                ///< Number of blocks to read
    // uint32_t block_size;             // Assumed to be known by replica from volume metadata
} __attribute__((packed)) xsan_replica_read_req_payload_t;

/**
 * @brief Payload for XSAN_MSG_TYPE_REPLICA_READ_BLOCK_RESP.
 * The actual block data follows this structured payload if status is XSAN_OK.
 */
typedef struct {
    xsan_error_t status;                ///< XSAN_OK on success, or an error code.
    xsan_volume_id_t volume_id;         ///< ID of the volume this block belongs to (for verification)
    uint64_t block_lba_on_volume;       ///< The starting LBA that this response pertains to
    uint32_t num_blocks;                ///< Number of blocks read (if successful) or 0 on error
    // uint32_t data_checksum;          // Optional: checksum of the data blocks that follow
} __attribute__((packed)) xsan_replica_read_resp_payload_t;

#define XSAN_REPLICA_READ_REQ_PAYLOAD_SIZE sizeof(xsan_replica_read_req_payload_t)
#define XSAN_REPLICA_READ_RESP_PAYLOAD_SIZE sizeof(xsan_replica_read_resp_payload_t)

/**
 * @brief Message header structure for all XSAN protocol messages.
 *
 * All multi-byte fields are expected to be in Network Byte Order (Big Endian)
 * when serialized for transmission. The serialization/deserialization functions
 * are responsible for these conversions.
 */
typedef struct {
    uint32_t magic;           ///< Magic number (XSAN_PROTOCOL_MAGIC)
    uint16_t type;            ///< Message type (xsan_message_type_t)
    uint16_t version;         ///< Protocol version (XSAN_PROTOCOL_VERSION)
    uint32_t payload_length;  ///< Length of the data payload following this header, in bytes.
    uint64_t transaction_id;  ///< Unique ID for matching requests and responses.
    uint32_t checksum;        ///< Checksum of (header fields excluding checksum + payload). 0 if not used.
} __attribute__((packed)) xsan_message_header_t;

/// Size of the protocol header in bytes.
#define XSAN_MESSAGE_HEADER_SIZE (sizeof(xsan_message_header_t))

/**
 * @brief Represents a full XSAN message, including header and payload.
 * The payload buffer is managed externally or by helper create/destroy functions.
 */
struct xsan_message {
    xsan_message_header_t header; ///< The message header.
    unsigned char *payload;       ///< Pointer to the payload data. NULL if payload_length is 0.
};

/**
 * @brief Initializes a message header with common values.
 * Sets magic, version, type, payload_length, and transaction_id.
 * Checksum is typically calculated and set separately after payload is known.
 *
 * @param header Pointer to the xsan_message_header_t to initialize. Must not be NULL.
 * @param type The message type (from xsan_message_type_t).
 * @param payload_length The length of the payload that will follow this header.
 * @param transaction_id The transaction ID for this message.
 */
void xsan_protocol_header_init(xsan_message_header_t *header,
                               xsan_message_type_t type,
                               uint32_t payload_length,
                               uint64_t transaction_id);

/**
 * @brief Serializes a message header into a byte buffer for network transmission.
 * Converts multi-byte fields to network byte order (Big Endian).
 *
 * @param header Pointer to the xsan_message_header_t to serialize. Must not be NULL.
 * @param buffer The destination byte buffer. Must be at least XSAN_MESSAGE_HEADER_SIZE bytes. Must not be NULL.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM if buffer or header is NULL.
 */
xsan_error_t xsan_protocol_header_serialize(const xsan_message_header_t *header, unsigned char *buffer);

/**
 * @brief Deserializes a message header from a byte buffer.
 * Converts multi-byte fields from network byte order to host byte order.
 * Validates the magic number.
 *
 * @param buffer The source byte buffer. Must contain at least XSAN_MESSAGE_HEADER_SIZE bytes. Must not be NULL.
 * @param header Pointer to the xsan_message_header_t structure to fill. Must not be NULL.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if buffer or header is NULL.
 *         XSAN_ERROR_PROTOCOL_ERROR if magic number mismatch, or other parsing issue. (Requires XSAN_ERROR_PROTOCOL_ERROR to be defined)
 */
xsan_error_t xsan_protocol_header_deserialize(const unsigned char *buffer, xsan_message_header_t *header);

/**
 * @brief Calculates a simple checksum (e.g., CRC32 or Fletcher-16/32) for a data buffer.
 * This function needs to be implemented with a chosen checksum algorithm.
 * For now, a placeholder.
 *
 * @param data The data buffer.
 * @param length The length of the data in bytes.
 * @return The calculated checksum. A value of 0 might indicate an error or an actual checksum of 0.
 */
uint32_t xsan_protocol_calculate_checksum(const unsigned char *data, size_t length);

/**
 * @brief Verifies the checksum of a received message (header + payload).
 * The header's checksum field should contain the expected checksum.
 *
 * @param header Pointer to the received message header.
 * @param payload Pointer to the received payload data. Can be NULL if payload_length is 0.
 * @return true if the checksum is valid (or if header.checksum is 0, implying checksum not used), false otherwise.
 */
bool xsan_protocol_verify_checksum(const xsan_message_header_t *header, const unsigned char *payload);


/**
 * @brief Creates a complete xsan_message_t, including allocating and copying payload data.
 * Initializes the header and sets its checksum field *after* copying the payload.
 *
 * @param type The message type.
 * @param transaction_id The transaction ID.
 * @param payload Pointer to the payload data to be copied. Can be NULL if payload_length is 0.
 * @param payload_length The length of the payload data.
 * @return A pointer to the newly created xsan_message_t, or NULL on failure (e.g., out of memory).
 *         The returned message (and its internal payload) must be freed using xsan_protocol_message_destroy.
 */
xsan_message_t *xsan_protocol_message_create(xsan_message_type_t type,
                                             uint64_t transaction_id,
                                             const void *payload,
                                             uint32_t payload_length);

/**
 * @brief Destroys an xsan_message_t, freeing its payload and the message structure itself.
 *
 * @param msg The message to destroy. If NULL, the function does nothing.
 */
void xsan_protocol_message_destroy(xsan_message_t *msg);

/**
 * @brief Creates a complete xsan_message_t, including a structured payload
 *        and an optional additional raw data block.
 * Initializes the header and sets its checksum field. The header's payload_length
 * will be the sum of structured_payload_len and additional_data_len.
 *
 * @param type The message type.
 * @param transaction_id The transaction ID.
 * @param structured_payload Pointer to the structured payload data (e.g., a request struct). Can be NULL if structured_payload_len is 0.
 * @param structured_payload_len Length of the structured payload data.
 * @param additional_data Pointer to the additional raw data block (e.g., disk block data). Can be NULL if additional_data_len is 0.
 * @param additional_data_len Length of the additional raw data block.
 * @return A pointer to the newly created xsan_message_t, or NULL on failure.
 *         The returned message must be freed using xsan_protocol_message_destroy.
 */
xsan_message_t *xsan_protocol_message_create_with_data(
    xsan_message_type_t type,
    uint64_t transaction_id,
    const void *structured_payload,
    uint32_t structured_payload_len,
    const void *additional_data,
    uint32_t additional_data_len);

#ifdef __cplusplus
}
#endif

#endif // XSAN_PROTOCOL_H
