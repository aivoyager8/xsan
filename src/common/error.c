/**
 * XSAN 错误处理实现
 * 
 * 提供统一的错误处理和错误码转换功能
 */

#include "xsan_error.h" // Contains xsan_error_t enum
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For strerror
#include <errno.h>  // For errno constants

// Calculate the size of the error messages array.
// XSAN_OK is 0. Errors are negative.
// XSAN_ERROR_LAST_CODE_MARKER is the most negative defined code (-225).
// The largest absolute error code value is abs(XSAN_ERROR_DECRYPTION_FAILED) = 224.
// Array size needs to accommodate index 0 (for XSAN_OK) and indices 1 through 224 for negative codes.
#define MAX_ABS_ERROR_CODE_VAL 224 // abs(XSAN_ERROR_DECRYPTION_FAILED)
#define ERROR_MESSAGES_ARRAY_SIZE (1 + MAX_ABS_ERROR_CODE_VAL)

/**
 * 错误码到字符串的映射表.
 * Index 0 is for XSAN_OK.
 * For a negative error_code, its message is at index abs(error_code).
 */
static const char *error_messages[ERROR_MESSAGES_ARRAY_SIZE];

/**
 * Helper function to initialize the error_messages array.
 * This is not standard C to run this automatically, so it must be called,
 * or the array must be statically initialized if possible.
 * For simplicity with a large array and specific indices, we'll use a function
 * and call it, or rely on a different lookup if direct static init is too complex.
 *
 * A better way for static initialization with sparse/negative enums in C if direct
 * array indexing is hard, is to use a switch case in xsan_error_string or a series of if-else.
 * However, to maintain the array lookup for potential performance/simplicity:
 */
static void initialize_error_messages() {
    // Initialize all to "Unknown error" or NULL first if not using designated initializers fully.
    // For this example, assuming direct assignment. If compiler supports it for all:
    error_messages[0] = "Success"; // For XSAN_OK

    // Generic Errors
    error_messages[abs(XSAN_ERROR_GENERIC)] = "Generic error";
    error_messages[abs(XSAN_ERROR_INVALID_PARAM)] = "Invalid parameter";
    error_messages[abs(XSAN_ERROR_OUT_OF_MEMORY)] = "Out of memory";
    error_messages[abs(XSAN_ERROR_IO)] = "I/O error";
    error_messages[abs(XSAN_ERROR_NETWORK)] = "Network error";
    error_messages[abs(XSAN_ERROR_NOT_FOUND)] = "Not found";
    error_messages[abs(XSAN_ERROR_TIMEOUT)] = "Operation timed out";
    error_messages[abs(XSAN_ERROR_NOT_IMPLEMENTED)] = "Feature or function not implemented";
    error_messages[abs(XSAN_ERROR_SYSTEM)] = "System error";
    error_messages[abs(XSAN_ERROR_PERMISSION_DENIED)] = "Permission denied";
    error_messages[abs(XSAN_ERROR_RESOURCE_BUSY)] = "Resource busy";
    error_messages[abs(XSAN_ERROR_INTERRUPTED)] = "Operation interrupted";
    error_messages[abs(XSAN_ERROR_ALREADY_EXISTS)] = "Item or resource already exists";

    // File System & Disk Errors
    error_messages[abs(XSAN_ERROR_FILE_NOT_FOUND)] = "File not found";
    error_messages[abs(XSAN_ERROR_FILE_EXISTS)] = "File exists";
    error_messages[abs(XSAN_ERROR_DISK_FULL)] = "Disk full";
    error_messages[abs(XSAN_ERROR_INSUFFICIENT_SPACE)] = "Insufficient space";
    error_messages[abs(XSAN_ERROR_CHECKSUM_MISMATCH)] = "Checksum mismatch";
    error_messages[abs(XSAN_ERROR_INVALID_OFFSET)] = "Invalid offset";
    error_messages[abs(XSAN_ERROR_INVALID_SIZE)] = "Invalid size";

    // Network Specific Errors
    error_messages[abs(XSAN_ERROR_CONNECTION_LOST)] = "Connection lost";
    error_messages[abs(XSAN_ERROR_ADDRESS_IN_USE)] = "Address in use";
    error_messages[abs(XSAN_ERROR_CONNECTION_REFUSED)] = "Connection refused";
    error_messages[abs(XSAN_ERROR_HOST_UNREACHABLE)] = "Host unreachable";
    error_messages[abs(XSAN_ERROR_NETWORK_DOWN)] = "Network down";

    // Cluster Errors
    error_messages[abs(XSAN_ERROR_CLUSTER_GENERIC)] = "Cluster error";
    error_messages[abs(XSAN_ERROR_NODE_NOT_FOUND)] = "Node not found";
    error_messages[abs(XSAN_ERROR_NODE_EXISTS)] = "Node exists";
    error_messages[abs(XSAN_ERROR_NODE_UNREACHABLE)] = "Node unreachable";
    error_messages[abs(XSAN_ERROR_SPLIT_BRAIN)] = "Cluster split brain";
    error_messages[abs(XSAN_ERROR_QUORUM_LOST)] = "Cluster quorum lost";
    error_messages[abs(XSAN_ERROR_CLUSTER_NOT_READY)] = "Cluster not ready";
    error_messages[abs(XSAN_ERROR_INVALID_NODE_STATE)] = "Invalid node state";

    // Storage & Device Errors
    error_messages[abs(XSAN_ERROR_STORAGE_GENERIC)] = "Storage error";
    error_messages[abs(XSAN_ERROR_DEVICE_FAILED)] = "Device failed";
    error_messages[abs(XSAN_ERROR_DEVICE_NOT_FOUND)] = "Device not found";
    error_messages[abs(XSAN_ERROR_VOLUME_NOT_FOUND)] = "Volume not found";
    error_messages[abs(XSAN_ERROR_VOLUME_EXISTS)] = "Volume exists";
    error_messages[abs(XSAN_ERROR_VOLUME_BUSY)] = "Volume busy";
    error_messages[abs(XSAN_ERROR_BLOCK_NOT_FOUND)] = "Block not found";
    error_messages[abs(XSAN_ERROR_BLOCK_CORRUPTED)] = "Block corrupted";

    // Replication Errors
    error_messages[abs(XSAN_ERROR_REPLICATION_GENERIC)] = "Replication error";
    error_messages[abs(XSAN_ERROR_REPLICA_NOT_FOUND)] = "Replica not found";
    error_messages[abs(XSAN_ERROR_REPLICA_OUTDATED)] = "Replica outdated";
    error_messages[abs(XSAN_ERROR_SYNC_FAILED)] = "Synchronization failed";
    error_messages[abs(XSAN_ERROR_CONSISTENCY_CHECK_FAILED)] = "Consistency check failed";
    error_messages[abs(XSAN_ERROR_RECOVERY_FAILED)] = "Recovery failed";
    error_messages[abs(XSAN_ERROR_NOT_ENOUGH_REPLICAS)] = "Not enough replicas";

    // Policy Errors
    error_messages[abs(XSAN_ERROR_POLICY_GENERIC)] = "Policy error";
    error_messages[abs(XSAN_ERROR_POLICY_NOT_FOUND)] = "Policy not found";
    error_messages[abs(XSAN_ERROR_POLICY_VIOLATION)] = "Policy violation";
    error_messages[abs(XSAN_ERROR_QOS_LIMIT_EXCEEDED)] = "QoS limit exceeded";

    // Virtualization Errors
    error_messages[abs(XSAN_ERROR_VIRTUALIZATION_GENERIC)] = "Virtualization error";
    error_messages[abs(XSAN_ERROR_LIBVIRT_FAILED)] = "libvirt operation failed";
    error_messages[abs(XSAN_ERROR_VM_NOT_FOUND)] = "Virtual machine not found";
    error_messages[abs(XSAN_ERROR_STORAGE_POOL_FAILED)] = "Storage pool operation failed";

    // Configuration Errors
    error_messages[abs(XSAN_ERROR_CONFIG_GENERIC)] = "Configuration error";
    error_messages[abs(XSAN_ERROR_CONFIG_INVALID)] = "Invalid configuration";
    error_messages[abs(XSAN_ERROR_CONFIG_MISSING)] = "Missing configuration";
    error_messages[abs(XSAN_ERROR_CONFIG_PARSE)] = "Configuration parse error";

    // Metadata Errors
    error_messages[abs(XSAN_ERROR_METADATA_GENERIC)] = "Metadata error";
    error_messages[abs(XSAN_ERROR_METADATA_READ_FAILED)] = "Metadata read failed";
    error_messages[abs(XSAN_ERROR_METADATA_WRITE_FAILED)] = "Metadata write failed";
    error_messages[abs(XSAN_ERROR_METADATA_CORRUPTED)] = "Metadata corrupted";

    // Task/Job Errors
    error_messages[abs(XSAN_ERROR_TASK_GENERIC)] = "Task/Job error";
    error_messages[abs(XSAN_ERROR_TASK_NOT_FOUND)] = "Task/Job not found";
    error_messages[abs(XSAN_ERROR_TASK_FAILED)] = "Task/Job failed";
    error_messages[abs(XSAN_ERROR_TASK_CANCELLED)] = "Task/Job cancelled";

    // Security Errors
    error_messages[abs(XSAN_ERROR_SECURITY_GENERIC)] = "Security error";
    error_messages[abs(XSAN_ERROR_AUTHENTICATION_FAILED)] = "Authentication failed";
    error_messages[abs(XSAN_ERROR_AUTHORIZATION_FAILED)] = "Authorization failed";
    error_messages[abs(XSAN_ERROR_ENCRYPTION_FAILED)] = "Encryption failed";
    error_messages[abs(XSAN_ERROR_DECRYPTION_FAILED)] = "Decryption failed";
}

// Flag to ensure initialization happens once
static bool error_messages_initialized = false;

/**
 * 获取错误码对应的字符串描述
 */
const char *xsan_error_string(xsan_error_t error)
{
    if (!error_messages_initialized) {
        initialize_error_messages();
        error_messages_initialized = true;
    }

    if (error == XSAN_OK) {
        return error_messages[0];
    }

    int index = -error; // Make the error code positive for array indexing

    if (index > 0 && index < ERROR_MESSAGES_ARRAY_SIZE && error_messages[index] != NULL) {
        return error_messages[index];
    }
    
    return "Unknown error";
}

/**
 * 将系统错误码转换为 XSAN 错误码
 */
xsan_error_t xsan_error_from_errno(int sys_errno)
{
    switch (sys_errno) {
        case 0:
            return XSAN_OK;
        case EINVAL:
            return XSAN_ERROR_INVALID_PARAM;
        case ENOMEM:
            return XSAN_ERROR_OUT_OF_MEMORY;
        case ENOENT:
            return XSAN_ERROR_FILE_NOT_FOUND;
        case EEXIST:
            return XSAN_ERROR_FILE_EXISTS; // Or XSAN_ERROR_ALREADY_EXISTS if more general
        case EACCES:
        case EPERM:
            return XSAN_ERROR_PERMISSION_DENIED;
        case EBUSY:
            return XSAN_ERROR_RESOURCE_BUSY;
        case ETIMEDOUT:
            return XSAN_ERROR_TIMEOUT;
        case EINTR:
            return XSAN_ERROR_INTERRUPTED;
        case EIO:
            return XSAN_ERROR_IO;
        case ENOSPC:
            return XSAN_ERROR_DISK_FULL;
        case ECONNRESET:
             return XSAN_ERROR_CONNECTION_LOST;
        case ECONNREFUSED:
            return XSAN_ERROR_CONNECTION_REFUSED;
        case ECONNABORTED:
            return XSAN_ERROR_CONNECTION_LOST; // Or a more specific "Connection aborted"
        case EADDRINUSE:
            return XSAN_ERROR_ADDRESS_IN_USE;
        case ENETDOWN:
            return XSAN_ERROR_NETWORK_DOWN;
        case ENETUNREACH:
        case EHOSTUNREACH:
            return XSAN_ERROR_HOST_UNREACHABLE;
        // Add more errno mappings as needed
        default:
            return XSAN_ERROR_SYSTEM; // Generic system error for unmapped errnos
    }
}

/**
 * 获取当前系统错误并转换为 XSAN 错误码
 */
xsan_error_t xsan_error_last(void)
{
    return xsan_error_from_errno(errno);
}

/**
 * 打印错误消息和上下文信息
 */
void xsan_error_print(xsan_error_t error, const char *file, int line, const char *func, const char *context_msg)
{
    const char *error_str = xsan_error_string(error);
    
    if (context_msg && *context_msg) {
        fprintf(stderr, "XSAN Error: %s:%d [%s]: %s - %s (code: %d)\n",
                file, line, func, context_msg, error_str, error);
    } else {
        fprintf(stderr, "XSAN Error: %s:%d [%s]: %s (code: %d)\n",
                file, line, func, error_str, error);
    }
}

/**
 * 将错误码转换为 HTTP 状态码 (用于 Web API)
 */
int xsan_error_to_http_status(xsan_error_t error)
{
    // Ensure messages are initialized if this can be called early
    if (!error_messages_initialized) {
        initialize_error_messages();
        error_messages_initialized = true;
    }

    switch (error) {
        case XSAN_OK:
            return 200;  /* OK */
        case XSAN_ERROR_INVALID_PARAM:
        case XSAN_ERROR_CONFIG_INVALID:
        case XSAN_ERROR_CONFIG_PARSE:
            return 400;  /* Bad Request */
        case XSAN_ERROR_PERMISSION_DENIED:
        case XSAN_ERROR_AUTHORIZATION_FAILED:
            return 403;  /* Forbidden */
        case XSAN_ERROR_AUTHENTICATION_FAILED:
            return 401; /* Unauthorized */
        case XSAN_ERROR_NOT_FOUND:
        case XSAN_ERROR_FILE_NOT_FOUND:
        case XSAN_ERROR_VOLUME_NOT_FOUND:
        case XSAN_ERROR_NODE_NOT_FOUND:
        case XSAN_ERROR_REPLICA_NOT_FOUND:
        case XSAN_ERROR_POLICY_NOT_FOUND:
        case XSAN_ERROR_VM_NOT_FOUND:
        case XSAN_ERROR_TASK_NOT_FOUND:
        case XSAN_ERROR_DEVICE_NOT_FOUND:
            return 404;  /* Not Found */
        case XSAN_ERROR_ALREADY_EXISTS:
        case XSAN_ERROR_FILE_EXISTS:
        case XSAN_ERROR_VOLUME_EXISTS:
        case XSAN_ERROR_NODE_EXISTS:
            return 409;  /* Conflict */
        case XSAN_ERROR_TIMEOUT:
            return 408;  /* Request Timeout */
        case XSAN_ERROR_RESOURCE_BUSY:
        case XSAN_ERROR_VOLUME_BUSY:
            return 423;  /* Locked */
        case XSAN_ERROR_INSUFFICIENT_SPACE:
        case XSAN_ERROR_DISK_FULL:
            return 507;  /* Insufficient Storage */
        case XSAN_ERROR_OUT_OF_MEMORY:
        case XSAN_ERROR_GENERIC: // Catch-all for server-side issues
        case XSAN_ERROR_IO:
        case XSAN_ERROR_SYSTEM:
        case XSAN_ERROR_BLOCK_CORRUPTED:
        case XSAN_ERROR_METADATA_CORRUPTED:
            return 500;  /* Internal Server Error */
        case XSAN_ERROR_NOT_IMPLEMENTED:
            return 501;  /* Not Implemented */
        case XSAN_ERROR_CLUSTER_NOT_READY:
        case XSAN_ERROR_NODE_UNREACHABLE:
        case XSAN_ERROR_QUORUM_LOST:
        case XSAN_ERROR_NETWORK: // Generic network issues could also be 503
            return 503;  /* Service Unavailable */
        default:
            // For unmapped specific errors, default to 500
            return 500;
    }
}

/**
 * 获取错误类别的字符串描述
 * This function might need adjustment based on the final error code ranges.
 */
const char *xsan_error_category_string(xsan_error_t error)
{
    // Ensure messages are initialized
    if (!error_messages_initialized) {
        initialize_error_messages();
        error_messages_initialized = true;
    }

    if (error == XSAN_OK) return "Success";
    if (error >= XSAN_ERROR_ALREADY_EXISTS && error <= XSAN_ERROR_GENERIC) return "Generic"; // -1 to -13
    if (error >= XSAN_ERROR_INVALID_SIZE && error <= XSAN_ERROR_FILE_NOT_FOUND) return "File System & Disk"; // -20 to -26
    if (error >= XSAN_ERROR_NETWORK_DOWN && error <= XSAN_ERROR_CONNECTION_LOST) return "Network"; // -40 to -44
    if (error >= XSAN_ERROR_INVALID_NODE_STATE && error <= XSAN_ERROR_CLUSTER_GENERIC) return "Cluster"; // -60 to -67
    if (error >= XSAN_ERROR_BLOCK_CORRUPTED && error <= XSAN_ERROR_STORAGE_GENERIC) return "Storage & Device"; // -80 to -87
    if (error >= XSAN_ERROR_NOT_ENOUGH_REPLICAS && error <= XSAN_ERROR_REPLICATION_GENERIC) return "Replication"; // -100 to -106
    if (error >= XSAN_ERROR_QOS_LIMIT_EXCEEDED && error <= XSAN_ERROR_POLICY_GENERIC) return "Policy"; // -120 to -123
    if (error >= XSAN_ERROR_STORAGE_POOL_FAILED && error <= XSAN_ERROR_VIRTUALIZATION_GENERIC) return "Virtualization"; // -140 to -143
    if (error >= XSAN_ERROR_CONFIG_PARSE && error <= XSAN_ERROR_CONFIG_GENERIC) return "Configuration"; // -160 to -163
    if (error >= XSAN_ERROR_METADATA_CORRUPTED && error <= XSAN_ERROR_METADATA_GENERIC) return "Metadata"; // -180 to -183
    if (error >= XSAN_ERROR_TASK_CANCELLED && error <= XSAN_ERROR_TASK_GENERIC) return "Task/Job"; // -200 to -203
    if (error >= XSAN_ERROR_DECRYPTION_FAILED && error <= XSAN_ERROR_SECURITY_GENERIC) return "Security"; // -220 to -224

    return "Unknown Category";
}

/**
 * 检查错误是否是可恢复的
 */
bool xsan_error_is_recoverable(xsan_error_t error)
{
    switch (error) {
        case XSAN_ERROR_TIMEOUT:
        case XSAN_ERROR_INTERRUPTED:
        case XSAN_ERROR_RESOURCE_BUSY:
        case XSAN_ERROR_VOLUME_BUSY:
        case XSAN_ERROR_CONNECTION_LOST:
        case XSAN_ERROR_CONNECTION_REFUSED: // Might be recoverable with retry
        case XSAN_ERROR_HOST_UNREACHABLE:   // Might be temporary
        case XSAN_ERROR_NETWORK_DOWN:       // Might be temporary
        case XSAN_ERROR_NODE_UNREACHABLE:
        case XSAN_ERROR_SYNC_FAILED:        // May be retried
        case XSAN_ERROR_CLUSTER_NOT_READY:  // State might change
            return true;
        default:
            return false;
    }
}

/**
 * 检查错误是否是致命的
 * (Fatal errors usually mean the application or current operation cannot proceed meaningfully)
 */
bool xsan_error_is_fatal(xsan_error_t error)
{
    switch (error) {
        case XSAN_ERROR_OUT_OF_MEMORY:
        case XSAN_ERROR_BLOCK_CORRUPTED:
        case XSAN_ERROR_METADATA_CORRUPTED:
        case XSAN_ERROR_DEVICE_FAILED:      // Often fatal for operations relying on this device
        case XSAN_ERROR_SPLIT_BRAIN:
        case XSAN_ERROR_QUORUM_LOST:
        case XSAN_ERROR_RECOVERY_FAILED:    // If recovery itself fails, it's often fatal for data
        case XSAN_ERROR_DISK_FULL:          // Can be fatal if no space can be freed
        case XSAN_ERROR_SYSTEM:             // Unspecified system errors are often problematic
            return true;
        default:
            return false;
    }
}
