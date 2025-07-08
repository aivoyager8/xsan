#ifndef XSAN_ERROR_H
#define XSAN_ERROR_H

#include <errno.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * XSAN 错误码定义
 */
typedef enum {
    XSAN_OK = 0,                    /* 成功 */
    
    /* 通用错误 */
    XSAN_ERROR_GENERIC = 1,         /* 通用错误 */
    XSAN_ERROR_INVALID_PARAM,       /* 无效参数 */
    XSAN_ERROR_NULL_POINTER,        /* 空指针 */
    XSAN_ERROR_OUT_OF_MEMORY,       /* 内存不足 */
    XSAN_ERROR_BUFFER_TOO_SMALL,    /* 缓冲区太小 */
    XSAN_ERROR_NOT_IMPLEMENTED,     /* 功能未实现 */
    XSAN_ERROR_PERMISSION_DENIED,   /* 权限拒绝 */
    XSAN_ERROR_RESOURCE_BUSY,       /* 资源繁忙 */
    XSAN_ERROR_TIMEOUT,             /* 超时 */
    XSAN_ERROR_INTERRUPTED,         /* 被中断 */
    
    /* 系统错误 */
    XSAN_ERROR_SYSTEM = 100,        /* 系统错误 */
    XSAN_ERROR_IO,                  /* I/O 错误 */
    XSAN_ERROR_FILE_NOT_FOUND,      /* 文件未找到 */
    XSAN_ERROR_FILE_EXISTS,         /* 文件已存在 */
    XSAN_ERROR_DISK_FULL,           /* 磁盘满 */
    XSAN_ERROR_NETWORK,             /* 网络错误 */
    XSAN_ERROR_CONNECTION_LOST,     /* 连接丢失 */
    XSAN_ERROR_PROTOCOL,            /* 协议错误 */
    
    /* 集群错误 */
    XSAN_ERROR_CLUSTER = 200,       /* 集群错误 */
    XSAN_ERROR_NODE_NOT_FOUND,      /* 节点未找到 */
    XSAN_ERROR_NODE_UNREACHABLE,    /* 节点不可达 */
    XSAN_ERROR_CLUSTER_NOT_READY,   /* 集群未就绪 */
    XSAN_ERROR_SPLIT_BRAIN,         /* 脑裂 */
    XSAN_ERROR_QUORUM_LOST,         /* 丢失法定人数 */
    XSAN_ERROR_LEADER_ELECTION,     /* 领导者选举错误 */
    XSAN_ERROR_MEMBERSHIP_CHANGE,   /* 成员变更错误 */
    
    /* 存储错误 */
    XSAN_ERROR_STORAGE = 300,       /* 存储错误 */
    XSAN_ERROR_VOLUME_NOT_FOUND,    /* 卷未找到 */
    XSAN_ERROR_VOLUME_EXISTS,       /* 卷已存在 */
    XSAN_ERROR_VOLUME_BUSY,         /* 卷繁忙 */
    XSAN_ERROR_BLOCK_NOT_FOUND,     /* 块未找到 */
    XSAN_ERROR_BLOCK_CORRUPTED,     /* 块损坏 */
    XSAN_ERROR_CHECKSUM_MISMATCH,   /* 校验和不匹配 */
    XSAN_ERROR_DEVICE_FAILED,       /* 设备故障 */
    XSAN_ERROR_DEVICE_NOT_FOUND,    /* 设备未找到 */
    XSAN_ERROR_INSUFFICIENT_SPACE,  /* 空间不足 */
    
    /* 复制错误 */
    XSAN_ERROR_REPLICATION = 400,   /* 复制错误 */
    XSAN_ERROR_REPLICA_NOT_FOUND,   /* 副本未找到 */
    XSAN_ERROR_REPLICA_OUTDATED,    /* 副本过期 */
    XSAN_ERROR_SYNC_FAILED,         /* 同步失败 */
    XSAN_ERROR_CONSISTENCY_CHECK,   /* 一致性检查失败 */
    XSAN_ERROR_RECOVERY_FAILED,     /* 恢复失败 */
    
    /* 策略错误 */
    XSAN_ERROR_POLICY = 500,        /* 策略错误 */
    XSAN_ERROR_POLICY_NOT_FOUND,    /* 策略未找到 */
    XSAN_ERROR_POLICY_VIOLATION,    /* 策略违反 */
    XSAN_ERROR_QOS_LIMIT_EXCEEDED,  /* QoS 限制超过 */
    
    /* 虚拟化错误 */
    XSAN_ERROR_VIRTUALIZATION = 600, /* 虚拟化错误 */
    XSAN_ERROR_LIBVIRT_FAILED,      /* libvirt 操作失败 */
    XSAN_ERROR_VM_NOT_FOUND,        /* 虚拟机未找到 */
    XSAN_ERROR_STORAGE_POOL_FAILED, /* 存储池操作失败 */
    
    /* 配置错误 */
    XSAN_ERROR_CONFIG = 700,        /* 配置错误 */
    XSAN_ERROR_CONFIG_INVALID,      /* 配置无效 */
    XSAN_ERROR_CONFIG_MISSING,      /* 配置缺失 */
    XSAN_ERROR_CONFIG_PARSE,        /* 配置解析错误 */
    
    /* 特殊错误码 */
    XSAN_ERROR_MAX = 999            /* 最大错误码 */
} xsan_error_t;

/**
 * Convert xsan_error_t to human readable string
 * @param error Error code
 * @return Error message string
 */
const char *xsan_error_string(xsan_error_t error);

/**
 * Convert system errno to xsan_error_t
 * @param sys_errno System error number
 * @return XSAN error code
 */
xsan_error_t xsan_error_from_errno(int sys_errno);

/**
 * Print error message with context
 * @param error Error code
 * @param context Context string
 */
void xsan_error_print(xsan_error_t error, const char *context);

/**
 * Get the last system error as XSAN error code
 * @return XSAN error code
 */
xsan_error_t xsan_error_last(void);

/**
 * Check if error code indicates success
 * @param error Error code
 * @return true if success, false otherwise
 */
static inline bool xsan_error_is_ok(xsan_error_t error) {
    return error == XSAN_OK;
}

/**
 * Check if error code indicates failure
 * @param error Error code
 * @return true if failure, false otherwise
 */
static inline bool xsan_error_is_error(xsan_error_t error) {
    return error != XSAN_OK;
}

/**
 * Check if error code is a system error
 * @param error Error code
 * @return true if system error, false otherwise
 */
static inline bool xsan_error_is_system(xsan_error_t error) {
    return error >= XSAN_ERROR_SYSTEM && error < XSAN_ERROR_CLUSTER;
}

/**
 * Check if error code is a cluster error
 * @param error Error code
 * @return true if cluster error, false otherwise
 */
static inline bool xsan_error_is_cluster(xsan_error_t error) {
    return error >= XSAN_ERROR_CLUSTER && error < XSAN_ERROR_STORAGE;
}

/**
 * Check if error code is a storage error
 * @param error Error code
 * @return true if storage error, false otherwise
 */
static inline bool xsan_error_is_storage(xsan_error_t error) {
    return error >= XSAN_ERROR_STORAGE && error < XSAN_ERROR_REPLICATION;
}

/**
 * Error handling macros
 */
#define XSAN_CHECK(expr) \
    do { \
        xsan_error_t __err = (expr); \
        if (xsan_error_is_error(__err)) { \
            return __err; \
        } \
    } while (0)

#define XSAN_CHECK_GOTO(expr, label) \
    do { \
        xsan_error_t __err = (expr); \
        if (xsan_error_is_error(__err)) { \
            error = __err; \
            goto label; \
        } \
    } while (0)

#define XSAN_CHECK_NULL(ptr) \
    do { \
        if ((ptr) == NULL) { \
            return XSAN_ERROR_NULL_POINTER; \
        } \
    } while (0)

#define XSAN_CHECK_PARAM(cond) \
    do { \
        if (!(cond)) { \
            return XSAN_ERROR_INVALID_PARAM; \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* XSAN_ERROR_H */
