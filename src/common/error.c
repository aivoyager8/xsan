/**
 * XSAN 错误处理实现
 * 
 * 提供统一的错误处理和错误码转换功能
 */

#include "xsan_error.h"
#include <stdio.h>
#include <stdlib.h>

/**
 * 错误码到字符串的映射表
 */
static const char *error_messages[] = {
    [XSAN_OK] = "Success",
    
    /* 通用错误 */
    [XSAN_ERROR_GENERIC] = "Generic error",
    [XSAN_ERROR_INVALID_PARAM] = "Invalid parameter",
    [XSAN_ERROR_NULL_POINTER] = "Null pointer",
    [XSAN_ERROR_OUT_OF_MEMORY] = "Out of memory",
    [XSAN_ERROR_BUFFER_TOO_SMALL] = "Buffer too small",
    [XSAN_ERROR_NOT_IMPLEMENTED] = "Not implemented",
    [XSAN_ERROR_PERMISSION_DENIED] = "Permission denied",
    [XSAN_ERROR_RESOURCE_BUSY] = "Resource busy",
    [XSAN_ERROR_TIMEOUT] = "Timeout",
    [XSAN_ERROR_INTERRUPTED] = "Interrupted",
    
    /* 系统错误 */
    [XSAN_ERROR_SYSTEM] = "System error",
    [XSAN_ERROR_IO] = "I/O error",
    [XSAN_ERROR_FILE_NOT_FOUND] = "File not found",
    [XSAN_ERROR_FILE_EXISTS] = "File exists",
    [XSAN_ERROR_DISK_FULL] = "Disk full",
    [XSAN_ERROR_NETWORK] = "Network error",
    [XSAN_ERROR_CONNECTION_LOST] = "Connection lost",
    [XSAN_ERROR_PROTOCOL] = "Protocol error",
    
    /* 集群错误 */
    [XSAN_ERROR_CLUSTER] = "Cluster error",
    [XSAN_ERROR_NODE_NOT_FOUND] = "Node not found",
    [XSAN_ERROR_NODE_UNREACHABLE] = "Node unreachable",
    [XSAN_ERROR_CLUSTER_NOT_READY] = "Cluster not ready",
    [XSAN_ERROR_SPLIT_BRAIN] = "Split brain detected",
    [XSAN_ERROR_QUORUM_LOST] = "Quorum lost",
    [XSAN_ERROR_LEADER_ELECTION] = "Leader election failed",
    [XSAN_ERROR_MEMBERSHIP_CHANGE] = "Membership change failed",
    
    /* 存储错误 */
    [XSAN_ERROR_STORAGE] = "Storage error",
    [XSAN_ERROR_VOLUME_NOT_FOUND] = "Volume not found",
    [XSAN_ERROR_VOLUME_EXISTS] = "Volume exists",
    [XSAN_ERROR_VOLUME_BUSY] = "Volume busy",
    [XSAN_ERROR_BLOCK_NOT_FOUND] = "Block not found",
    [XSAN_ERROR_BLOCK_CORRUPTED] = "Block corrupted",
    [XSAN_ERROR_CHECKSUM_MISMATCH] = "Checksum mismatch",
    [XSAN_ERROR_DEVICE_FAILED] = "Device failed",
    [XSAN_ERROR_DEVICE_NOT_FOUND] = "Device not found",
    [XSAN_ERROR_INSUFFICIENT_SPACE] = "Insufficient space",
    
    /* 复制错误 */
    [XSAN_ERROR_REPLICATION] = "Replication error",
    [XSAN_ERROR_REPLICA_NOT_FOUND] = "Replica not found",
    [XSAN_ERROR_REPLICA_OUTDATED] = "Replica outdated",
    [XSAN_ERROR_SYNC_FAILED] = "Synchronization failed",
    [XSAN_ERROR_CONSISTENCY_CHECK] = "Consistency check failed",
    [XSAN_ERROR_RECOVERY_FAILED] = "Recovery failed",
    
    /* 策略错误 */
    [XSAN_ERROR_POLICY] = "Policy error",
    [XSAN_ERROR_POLICY_NOT_FOUND] = "Policy not found",
    [XSAN_ERROR_POLICY_VIOLATION] = "Policy violation",
    [XSAN_ERROR_QOS_LIMIT_EXCEEDED] = "QoS limit exceeded",
    
    /* 虚拟化错误 */
    [XSAN_ERROR_VIRTUALIZATION] = "Virtualization error",
    [XSAN_ERROR_LIBVIRT_FAILED] = "libvirt operation failed",
    [XSAN_ERROR_VM_NOT_FOUND] = "Virtual machine not found",
    [XSAN_ERROR_STORAGE_POOL_FAILED] = "Storage pool operation failed",
    
    /* 配置错误 */
    [XSAN_ERROR_CONFIG] = "Configuration error",
    [XSAN_ERROR_CONFIG_INVALID] = "Invalid configuration",
    [XSAN_ERROR_CONFIG_MISSING] = "Missing configuration",
    [XSAN_ERROR_CONFIG_PARSE] = "Configuration parse error",
};

/**
 * 获取错误码对应的字符串描述
 */
const char *xsan_error_string(xsan_error_t error)
{
    if (error >= 0 && error < XSAN_ERROR_MAX && 
        error < sizeof(error_messages) / sizeof(error_messages[0]) &&
        error_messages[error] != NULL) {
        return error_messages[error];
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
            return XSAN_ERROR_FILE_EXISTS;
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
        case ECONNREFUSED:
        case ECONNABORTED:
            return XSAN_ERROR_CONNECTION_LOST;
        case ENETDOWN:
        case ENETUNREACH:
        case EHOSTDOWN:
        case EHOSTUNREACH:
            return XSAN_ERROR_NETWORK;
        default:
            return XSAN_ERROR_SYSTEM;
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
void xsan_error_print(xsan_error_t error, const char *context)
{
    const char *error_str = xsan_error_string(error);
    
    if (context && *context) {
        fprintf(stderr, "XSAN Error: %s - %s (code: %d)\n", 
                context, error_str, error);
    } else {
        fprintf(stderr, "XSAN Error: %s (code: %d)\n", 
                error_str, error);
    }
}

/**
 * 将错误码转换为 HTTP 状态码 (用于 Web API)
 */
int xsan_error_to_http_status(xsan_error_t error)
{
    switch (error) {
        case XSAN_OK:
            return 200;  /* OK */
        case XSAN_ERROR_INVALID_PARAM:
            return 400;  /* Bad Request */
        case XSAN_ERROR_PERMISSION_DENIED:
            return 403;  /* Forbidden */
        case XSAN_ERROR_FILE_NOT_FOUND:
        case XSAN_ERROR_VOLUME_NOT_FOUND:
        case XSAN_ERROR_NODE_NOT_FOUND:
            return 404;  /* Not Found */
        case XSAN_ERROR_FILE_EXISTS:
        case XSAN_ERROR_VOLUME_EXISTS:
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
            return 500;  /* Internal Server Error */
        case XSAN_ERROR_NOT_IMPLEMENTED:
            return 501;  /* Not Implemented */
        case XSAN_ERROR_CLUSTER_NOT_READY:
        case XSAN_ERROR_NODE_UNREACHABLE:
            return 503;  /* Service Unavailable */
        default:
            return 500;  /* Internal Server Error */
    }
}

/**
 * 获取错误类别的字符串描述
 */
const char *xsan_error_category_string(xsan_error_t error)
{
    if (error == XSAN_OK) {
        return "Success";
    } else if (error < XSAN_ERROR_SYSTEM) {
        return "Generic";
    } else if (error < XSAN_ERROR_CLUSTER) {
        return "System";
    } else if (error < XSAN_ERROR_STORAGE) {
        return "Cluster";
    } else if (error < XSAN_ERROR_REPLICATION) {
        return "Storage";
    } else if (error < XSAN_ERROR_POLICY) {
        return "Replication";
    } else if (error < XSAN_ERROR_VIRTUALIZATION) {
        return "Policy";
    } else if (error < XSAN_ERROR_CONFIG) {
        return "Virtualization";
    } else if (error < XSAN_ERROR_MAX) {
        return "Configuration";
    } else {
        return "Unknown";
    }
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
        case XSAN_ERROR_NETWORK:
        case XSAN_ERROR_NODE_UNREACHABLE:
        case XSAN_ERROR_SYNC_FAILED:
        case XSAN_ERROR_CLUSTER_NOT_READY:
            return true;
        default:
            return false;
    }
}

/**
 * 检查错误是否是致命的
 */
bool xsan_error_is_fatal(xsan_error_t error)
{
    switch (error) {
        case XSAN_ERROR_OUT_OF_MEMORY:
        case XSAN_ERROR_BLOCK_CORRUPTED:
        case XSAN_ERROR_CHECKSUM_MISMATCH:
        case XSAN_ERROR_DEVICE_FAILED:
        case XSAN_ERROR_SPLIT_BRAIN:
        case XSAN_ERROR_QUORUM_LOST:
        case XSAN_ERROR_RECOVERY_FAILED:
            return true;
        default:
            return false;
    }
}
