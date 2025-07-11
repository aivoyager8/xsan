#ifndef XSAN_ERROR_H
#define XSAN_ERROR_H

/**
 * XSAN Error Codes
 *
 * This file defines all error codes used within the XSAN system.
 * Convention:
 *  - XSAN_OK (0) for success.
 *  - Negative values for errors.
 */
typedef enum {
    XSAN_OK = 0,

    /* Generic Errors (Derived from original xsan_types.h and error.c needs) */
    XSAN_ERROR_GENERIC = -1,            // General unspecified error
    XSAN_ERROR_INVALID_PARAM = -2,      // Invalid function parameter
    XSAN_ERROR_OUT_OF_MEMORY = -3,    // Memory allocation failure
    XSAN_ERROR_IO = -4,                 // General I/O error
    XSAN_ERROR_NETWORK = -5,            // General network error (use more specific ones below if possible)
    XSAN_ERROR_NOT_FOUND = -6,          // Generic item not found
    XSAN_ERROR_TIMEOUT = -7,            // Operation timed out
    XSAN_ERROR_NOT_IMPLEMENTED = -8,    // Feature or function not implemented
    XSAN_ERROR_SYSTEM = -9,             // Catch-all for unmapped system/errno
    XSAN_ERROR_PERMISSION_DENIED = -10, // General permission issue
    XSAN_ERROR_RESOURCE_BUSY = -11,     // Resource is currently busy/locked
    XSAN_ERROR_INTERRUPTED = -12,       // Operation was interrupted
    XSAN_ERROR_ALREADY_EXISTS = -13,    // Item or resource already exists

    /* File System & Disk Errors */
    XSAN_ERROR_FILE_NOT_FOUND = -20,    // Specific to files
    XSAN_ERROR_FILE_EXISTS = -21,       // Specific to files
    XSAN_ERROR_DISK_FULL = -22,         // Disk is full
    XSAN_ERROR_INSUFFICIENT_SPACE = -23,// Generic insufficient space (can be disk, memory, etc.)
    XSAN_ERROR_CHECKSUM_MISMATCH = -24, // Data integrity check failed
    XSAN_ERROR_INVALID_OFFSET = -25,    // Invalid offset for I/O
    XSAN_ERROR_INVALID_SIZE = -26,      // Invalid size for I/O or allocation

    /* Network Specific Errors */
    XSAN_ERROR_CONNECTION_LOST = -40,   // Connection was unexpectedly lost
    XSAN_ERROR_ADDRESS_IN_USE = -41,    // Network address (IP/port) already in use
    XSAN_ERROR_CONNECTION_REFUSED = -42,// Connection was actively refused
    XSAN_ERROR_HOST_UNREACHABLE = -43,  // Host cannot be reached
    XSAN_ERROR_NETWORK_DOWN = -44,      // Network interface/subsystem is down

    /* Cluster Errors */
    XSAN_ERROR_CLUSTER_GENERIC = -60,
    XSAN_ERROR_NODE_NOT_FOUND = -61,
    XSAN_ERROR_NODE_EXISTS = -62,
    XSAN_ERROR_NODE_UNREACHABLE = -63,
    XSAN_ERROR_SPLIT_BRAIN = -64,       // Cluster partition leading to multiple masters
    XSAN_ERROR_QUORUM_LOST = -65,       // Not enough nodes for quorum
    XSAN_ERROR_CLUSTER_NOT_READY = -66, // Cluster is not in a state to perform operation
    XSAN_ERROR_INVALID_NODE_STATE = -67,// Node is in an invalid state for the operation

    /* Storage & Device Errors */
    XSAN_ERROR_STORAGE_GENERIC = -80,
    XSAN_ERROR_DEVICE_FAILED = -81,     // Physical or virtual device failure
    XSAN_ERROR_DEVICE_NOT_FOUND = -82,
    XSAN_ERROR_VOLUME_NOT_FOUND = -83,
    XSAN_ERROR_VOLUME_EXISTS = -84,
    XSAN_ERROR_VOLUME_BUSY = -85,
    XSAN_ERROR_BLOCK_NOT_FOUND = -86,
    XSAN_ERROR_BLOCK_CORRUPTED = -87,   // Data block is corrupted

    /* Replication Errors */
    XSAN_ERROR_REPLICATION_GENERIC = -100,
    XSAN_ERROR_REPLICA_NOT_FOUND = -101,
    XSAN_ERROR_REPLICA_OUTDATED = -102,
    XSAN_ERROR_SYNC_FAILED = -103,          // Replication sync operation failed
    XSAN_ERROR_CONSISTENCY_CHECK_FAILED = -104,
    XSAN_ERROR_RECOVERY_FAILED = -105,      // Data recovery from replica failed
    XSAN_ERROR_NOT_ENOUGH_REPLICAS = -106, // Not enough healthy replicas available

    /* Policy Errors */
    XSAN_ERROR_POLICY_GENERIC = -120,
    XSAN_ERROR_POLICY_NOT_FOUND = -121,
    XSAN_ERROR_POLICY_VIOLATION = -122,
    XSAN_ERROR_QOS_LIMIT_EXCEEDED = -123,

    /* Virtualization Errors */
    XSAN_ERROR_VIRTUALIZATION_GENERIC = -140,
    XSAN_ERROR_LIBVIRT_FAILED = -141,       // Error from libvirt library
    XSAN_ERROR_VM_NOT_FOUND = -142,
    XSAN_ERROR_STORAGE_POOL_FAILED = -143,

    /* Configuration Errors */
    XSAN_ERROR_CONFIG_GENERIC = -160,
    XSAN_ERROR_CONFIG_INVALID = -161,     // Configuration value is invalid
    XSAN_ERROR_CONFIG_MISSING = -162,     // Required configuration item is missing
    XSAN_ERROR_CONFIG_PARSE = -163,       // Failed to parse configuration file/string

    /* Metadata Errors */
    XSAN_ERROR_METADATA_GENERIC = -180,
    XSAN_ERROR_METADATA_READ_FAILED = -181,
    XSAN_ERROR_METADATA_WRITE_FAILED = -182,
    XSAN_ERROR_METADATA_CORRUPTED = -183,

    /* Task/Job Errors */
    XSAN_ERROR_TASK_GENERIC = -200,
    XSAN_ERROR_TASK_NOT_FOUND = -201,
    XSAN_ERROR_TASK_FAILED = -202,
    XSAN_ERROR_TASK_CANCELLED = -203,

    /* Security Errors */
    XSAN_ERROR_SECURITY_GENERIC = -220,
    XSAN_ERROR_AUTHENTICATION_FAILED = -221,
    XSAN_ERROR_AUTHORIZATION_FAILED = -222, // Could be same as PERMISSION_DENIED but for specific auth context
    XSAN_ERROR_ENCRYPTION_FAILED = -223,
    XSAN_ERROR_DECRYPTION_FAILED = -224,

    /* Protocol Errors */
    XSAN_ERROR_PROTOCOL_GENERIC = -240,   // Generic protocol error
    XSAN_ERROR_PROTOCOL_MAGIC_MISMATCH = -241, // Incorrect magic number
    XSAN_ERROR_PROTOCOL_VERSION_UNSUPPORTED = -242, // Unsupported protocol version
    XSAN_ERROR_PROTOCOL_CHECKSUM_INVALID = -243, // Checksum validation failed
    XSAN_ERROR_PROTOCOL_PAYLOAD_TOO_LARGE = -244, // Payload exceeds max allowed size
    XSAN_ERROR_PROTOCOL_MESSAGE_INCOMPLETE = -245, // Message appears truncated

    /* Threading & Context Errors */
    XSAN_ERROR_THREAD_CONTEXT = -260,   // Operation called from an incorrect thread context (e.g., non-SPDK thread for SPDK API)


    // XSAN_ERROR_MAX is a conceptual marker for the end of defined errors.
    // The actual count and range will be managed by the error string array in error.c
    // For array sizing, use a counter or ensure XSAN_ERROR_MAX is the last *positive* enum if switching scheme.
    // Given negative codes, the 'max' is actually the 'min' (most negative).
    // Let's define a specific last error for array sizing purposes if needed, or calculate it.
    XSAN_ERROR_LAST_CODE_MARKER = -261 // Represents the boundary for known error codes
} xsan_error_t;

#endif /* XSAN_ERROR_H */
