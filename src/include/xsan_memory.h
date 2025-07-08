/**
 * XSAN 内存管理模块
 * 
 * 提供内存分配、释放、统计和调试功能
 */

#ifndef XSAN_MEMORY_H
#define XSAN_MEMORY_H

#include "xsan_types.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 内存池块大小 */
#define XSAN_MEMORY_POOL_BLOCK_SIZE_SMALL   64
#define XSAN_MEMORY_POOL_BLOCK_SIZE_MEDIUM  256
#define XSAN_MEMORY_POOL_BLOCK_SIZE_LARGE   1024

/* 内存池配置 */
typedef struct xsan_memory_pool_config {
    size_t block_size;              /* 块大小 */
    size_t initial_blocks;          /* 初始块数量 */
    size_t max_blocks;              /* 最大块数量 */
    bool thread_safe;               /* 是否线程安全 */
} xsan_memory_pool_config_t;

/* 内存池 */
typedef struct xsan_memory_pool xsan_memory_pool_t;

/* 内存统计信息 */
typedef struct xsan_memory_stats {
    uint64_t total_allocated;       /* 总分配内存 */
    uint64_t total_freed;           /* 总释放内存 */
    uint64_t current_allocated;     /* 当前分配内存 */
    uint64_t peak_allocated;        /* 峰值分配内存 */
    uint64_t allocation_count;      /* 分配次数 */
    uint64_t free_count;            /* 释放次数 */
    uint64_t pool_hits;             /* 内存池命中次数 */
    uint64_t pool_misses;           /* 内存池未命中次数 */
} xsan_memory_stats_t;

/**
 * 初始化内存管理系统
 * 
 * @param enable_debug 是否启用调试模式
 * @return 错误码
 */
xsan_error_t xsan_memory_init(bool enable_debug);

/**
 * 清理内存管理系统
 */
void xsan_memory_cleanup(void);

/**
 * 分配内存
 * 
 * @param size 要分配的内存大小
 * @return 内存指针，失败返回 NULL
 */
void *xsan_malloc(size_t size);

/**
 * 分配并清零内存
 * 
 * @param nmemb 元素数量
 * @param size 每个元素的大小
 * @return 内存指针，失败返回 NULL
 */
void *xsan_calloc(size_t nmemb, size_t size);

/**
 * 重新分配内存
 * 
 * @param ptr 原内存指针
 * @param size 新的大小
 * @return 新的内存指针，失败返回 NULL
 */
void *xsan_realloc(void *ptr, size_t size);

/**
 * 释放内存
 * 
 * @param ptr 要释放的内存指针
 */
void xsan_free(void *ptr);

/**
 * 安全释放内存（自动设置指针为 NULL）
 * 
 * @param ptr 指向内存指针的指针
 */
void xsan_safe_free(void **ptr);

/**
 * 复制字符串并分配内存
 * 
 * @param str 要复制的字符串
 * @return 新分配的字符串，失败返回 NULL
 */
char *xsan_strdup(const char *str);

/**
 * 复制指定长度的字符串并分配内存
 * 
 * @param str 要复制的字符串
 * @param n 最大复制长度
 * @return 新分配的字符串，失败返回 NULL
 */
char *xsan_strndup(const char *str, size_t n);

/**
 * 创建内存池
 * 
 * @param config 内存池配置
 * @return 内存池句柄，失败返回 NULL
 */
xsan_memory_pool_t *xsan_memory_pool_create(const xsan_memory_pool_config_t *config);

/**
 * 销毁内存池
 * 
 * @param pool 内存池句柄
 */
void xsan_memory_pool_destroy(xsan_memory_pool_t *pool);

/**
 * 从内存池分配内存
 * 
 * @param pool 内存池句柄
 * @return 内存指针，失败返回 NULL
 */
void *xsan_memory_pool_alloc(xsan_memory_pool_t *pool);

/**
 * 归还内存到内存池
 * 
 * @param pool 内存池句柄
 * @param ptr 要归还的内存指针
 */
void xsan_memory_pool_free(xsan_memory_pool_t *pool, void *ptr);

/**
 * 获取内存统计信息
 * 
 * @param stats 输出统计信息
 * @return 错误码
 */
xsan_error_t xsan_memory_get_stats(xsan_memory_stats_t *stats);

/**
 * 打印内存统计信息
 */
void xsan_memory_print_stats(void);

/**
 * 检查内存泄漏
 * 
 * @return 是否有内存泄漏
 */
bool xsan_memory_check_leaks(void);

/**
 * 设置内存分配失败回调
 * 
 * @param callback 回调函数
 */
void xsan_memory_set_oom_callback(void (*callback)(size_t size));

/* 便利宏 */
#define XSAN_MALLOC(size)           xsan_malloc(size)
#define XSAN_CALLOC(nmemb, size)    xsan_calloc(nmemb, size)
#define XSAN_REALLOC(ptr, size)     xsan_realloc(ptr, size)
#define XSAN_FREE(ptr)              xsan_free(ptr)
#define XSAN_SAFE_FREE(ptr)         xsan_safe_free((void**)(ptr))
#define XSAN_STRDUP(str)            xsan_strdup(str)
#define XSAN_STRNDUP(str, n)        xsan_strndup(str, n)

/* 类型安全的内存分配宏 */
#define XSAN_MALLOC_TYPE(type)      ((type*)xsan_malloc(sizeof(type)))
#define XSAN_CALLOC_TYPE(type)      ((type*)xsan_calloc(1, sizeof(type)))
#define XSAN_CALLOC_ARRAY(type, n)  ((type*)xsan_calloc(n, sizeof(type)))

#ifdef __cplusplus
}
#endif

#endif /* XSAN_MEMORY_H */
