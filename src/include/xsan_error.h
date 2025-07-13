#ifndef XSAN_ERROR_H
#define XSAN_ERROR_H

// Deprecated: src/include/xsan_error.h
#error "Do not use src/include/xsan_error.h. Use include/xsan_error.h instead."
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

#endif // XSAN_ERROR_H
