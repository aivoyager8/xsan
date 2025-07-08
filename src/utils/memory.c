/**
 * XSAN 内存管理模块实现
 * 
 * 提供内存分配、释放、统计和调试功能
 */

#include "xsan_memory.h"
#include "xsan_error.h"
#include "xsan_log.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

/* 调试模式下的内存块头部信息 */
typedef struct memory_block_header {
    size_t size;                    /* 块大小 */
    uint32_t magic;                 /* 魔术字 */
    struct memory_block_header *next;
    struct memory_block_header *prev;
    const char *file;               /* 分配时的文件名 */
    int line;                       /* 分配时的行号 */
} memory_block_header_t;

/* 内存池块 */
typedef struct memory_pool_block {
    void *data;                     /* 数据指针 */
    bool in_use;                    /* 是否正在使用 */
    struct memory_pool_block *next;
} memory_pool_block_t;

/* 内存池结构 */
struct xsan_memory_pool {
    xsan_memory_pool_config_t config;
    memory_pool_block_t *free_blocks;
    memory_pool_block_t *all_blocks;
    size_t current_blocks;
    pthread_mutex_t mutex;
};

/* 全局内存管理状态 */
static struct {
    bool initialized;
    bool debug_enabled;
    xsan_memory_stats_t stats;
    memory_block_header_t *allocated_blocks;
    pthread_mutex_t mutex;
    void (*oom_callback)(size_t size);
} g_memory_mgr = {
    .initialized = false,
    .debug_enabled = false,
    .stats = {0},
    .allocated_blocks = NULL,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .oom_callback = NULL
};

/* 魔术字，用于检测内存越界 */
#define XSAN_MEMORY_MAGIC           0x58534146  /* "XSAF" */
#define XSAN_MEMORY_FREED_MAGIC     0x46524545  /* "FREE" */

/**
 * 初始化内存管理系统
 */
xsan_error_t xsan_memory_init(bool enable_debug)
{
    pthread_mutex_lock(&g_memory_mgr.mutex);
    
    if (g_memory_mgr.initialized) {
        pthread_mutex_unlock(&g_memory_mgr.mutex);
        return XSAN_OK;
    }
    
    g_memory_mgr.debug_enabled = enable_debug;
    g_memory_mgr.allocated_blocks = NULL;
    memset(&g_memory_mgr.stats, 0, sizeof(g_memory_mgr.stats));
    g_memory_mgr.initialized = true;
    
    pthread_mutex_unlock(&g_memory_mgr.mutex);
    
    XSAN_LOG_INFO("Memory manager initialized (debug: %s)", 
                  enable_debug ? "enabled" : "disabled");
    
    return XSAN_OK;
}

/**
 * 清理内存管理系统
 */
void xsan_memory_cleanup(void)
{
    pthread_mutex_lock(&g_memory_mgr.mutex);
    
    if (!g_memory_mgr.initialized) {
        pthread_mutex_unlock(&g_memory_mgr.mutex);
        return;
    }
    
    /* 检查内存泄漏 */
    if (g_memory_mgr.debug_enabled && g_memory_mgr.allocated_blocks) {
        XSAN_LOG_WARN("Memory leaks detected during cleanup");
        xsan_memory_check_leaks();
    }
    
    g_memory_mgr.initialized = false;
    pthread_mutex_unlock(&g_memory_mgr.mutex);
    
    XSAN_LOG_INFO("Memory manager cleaned up");
}

/**
 * 分配内存
 */
void *xsan_malloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    
    void *ptr = NULL;
    
    pthread_mutex_lock(&g_memory_mgr.mutex);
    
    if (!g_memory_mgr.initialized) {
        pthread_mutex_unlock(&g_memory_mgr.mutex);
        return malloc(size);
    }
    
    if (g_memory_mgr.debug_enabled) {
        /* 调试模式：添加头部信息 */
        memory_block_header_t *header = malloc(sizeof(memory_block_header_t) + size);
        if (!header) {
            if (g_memory_mgr.oom_callback) {
                g_memory_mgr.oom_callback(size);
            }
            pthread_mutex_unlock(&g_memory_mgr.mutex);
            return NULL;
        }
        
        header->size = size;
        header->magic = XSAN_MEMORY_MAGIC;
        header->file = __FILE__;
        header->line = __LINE__;
        
        /* 添加到分配列表 */
        header->next = g_memory_mgr.allocated_blocks;
        header->prev = NULL;
        if (g_memory_mgr.allocated_blocks) {
            g_memory_mgr.allocated_blocks->prev = header;
        }
        g_memory_mgr.allocated_blocks = header;
        
        ptr = (char*)header + sizeof(memory_block_header_t);
        
        /* 更新统计信息 */
        g_memory_mgr.stats.total_allocated += size;
        g_memory_mgr.stats.current_allocated += size;
        if (g_memory_mgr.stats.current_allocated > g_memory_mgr.stats.peak_allocated) {
            g_memory_mgr.stats.peak_allocated = g_memory_mgr.stats.current_allocated;
        }
        g_memory_mgr.stats.allocation_count++;
    } else {
        /* 非调试模式：直接分配 */
        ptr = malloc(size);
        if (!ptr) {
            if (g_memory_mgr.oom_callback) {
                g_memory_mgr.oom_callback(size);
            }
            pthread_mutex_unlock(&g_memory_mgr.mutex);
            return NULL;
        }
        
        /* 更新统计信息 */
        g_memory_mgr.stats.total_allocated += size;
        g_memory_mgr.stats.current_allocated += size;
        if (g_memory_mgr.stats.current_allocated > g_memory_mgr.stats.peak_allocated) {
            g_memory_mgr.stats.peak_allocated = g_memory_mgr.stats.current_allocated;
        }
        g_memory_mgr.stats.allocation_count++;
    }
    
    pthread_mutex_unlock(&g_memory_mgr.mutex);
    return ptr;
}

/**
 * 分配并清零内存
 */
void *xsan_calloc(size_t nmemb, size_t size)
{
    size_t total_size = nmemb * size;
    
    /* 检查溢出 */
    if (nmemb != 0 && total_size / nmemb != size) {
        return NULL;
    }
    
    void *ptr = xsan_malloc(total_size);
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    
    return ptr;
}

/**
 * 重新分配内存
 */
void *xsan_realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return xsan_malloc(size);
    }
    
    if (size == 0) {
        xsan_free(ptr);
        return NULL;
    }
    
    pthread_mutex_lock(&g_memory_mgr.mutex);
    
    if (!g_memory_mgr.initialized) {
        pthread_mutex_unlock(&g_memory_mgr.mutex);
        return realloc(ptr, size);
    }
    
    void *new_ptr = NULL;
    
    if (g_memory_mgr.debug_enabled) {
        /* 调试模式：查找原始块 */
        memory_block_header_t *header = (memory_block_header_t*)((char*)ptr - sizeof(memory_block_header_t));
        
        if (header->magic != XSAN_MEMORY_MAGIC) {
            XSAN_LOG_ERROR("Invalid memory block in realloc");
            pthread_mutex_unlock(&g_memory_mgr.mutex);
            return NULL;
        }
        
        size_t old_size = header->size;
        new_ptr = xsan_malloc(size);
        if (new_ptr) {
            memcpy(new_ptr, ptr, old_size < size ? old_size : size);
            xsan_free(ptr);
        }
    } else {
        /* 非调试模式：直接重新分配 */
        new_ptr = realloc(ptr, size);
        if (new_ptr) {
            /* 更新统计信息（简化处理） */
            g_memory_mgr.stats.allocation_count++;
        }
    }
    
    pthread_mutex_unlock(&g_memory_mgr.mutex);
    return new_ptr;
}

/**
 * 释放内存
 */
void xsan_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    
    pthread_mutex_lock(&g_memory_mgr.mutex);
    
    if (!g_memory_mgr.initialized) {
        pthread_mutex_unlock(&g_memory_mgr.mutex);
        free(ptr);
        return;
    }
    
    if (g_memory_mgr.debug_enabled) {
        /* 调试模式：检查头部信息 */
        memory_block_header_t *header = (memory_block_header_t*)((char*)ptr - sizeof(memory_block_header_t));
        
        if (header->magic != XSAN_MEMORY_MAGIC) {
            XSAN_LOG_ERROR("Invalid memory block in free (magic: 0x%08x)", header->magic);
            pthread_mutex_unlock(&g_memory_mgr.mutex);
            return;
        }
        
        /* 从分配列表中移除 */
        if (header->prev) {
            header->prev->next = header->next;
        } else {
            g_memory_mgr.allocated_blocks = header->next;
        }
        if (header->next) {
            header->next->prev = header->prev;
        }
        
        /* 更新统计信息 */
        g_memory_mgr.stats.total_freed += header->size;
        g_memory_mgr.stats.current_allocated -= header->size;
        g_memory_mgr.stats.free_count++;
        
        /* 标记为已释放 */
        header->magic = XSAN_MEMORY_FREED_MAGIC;
        
        free(header);
    } else {
        /* 非调试模式：直接释放 */
        free(ptr);
        g_memory_mgr.stats.free_count++;
    }
    
    pthread_mutex_unlock(&g_memory_mgr.mutex);
}

/**
 * 安全释放内存
 */
void xsan_safe_free(void **ptr)
{
    if (ptr && *ptr) {
        xsan_free(*ptr);
        *ptr = NULL;
    }
}

/**
 * 复制字符串并分配内存
 */
char *xsan_strdup(const char *str)
{
    if (!str) {
        return NULL;
    }
    
    size_t len = strlen(str) + 1;
    char *copy = xsan_malloc(len);
    if (copy) {
        memcpy(copy, str, len);
    }
    
    return copy;
}

/**
 * 复制指定长度的字符串并分配内存
 */
char *xsan_strndup(const char *str, size_t n)
{
    if (!str) {
        return NULL;
    }
    
    size_t len = strnlen(str, n);
    char *copy = xsan_malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    
    return copy;
}

/**
 * 创建内存池
 */
xsan_memory_pool_t *xsan_memory_pool_create(const xsan_memory_pool_config_t *config)
{
    if (!config || config->block_size == 0) {
        return NULL;
    }
    
    xsan_memory_pool_t *pool = xsan_malloc(sizeof(xsan_memory_pool_t));
    if (!pool) {
        return NULL;
    }
    
    pool->config = *config;
    pool->free_blocks = NULL;
    pool->all_blocks = NULL;
    pool->current_blocks = 0;
    
    if (config->thread_safe) {
        if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
            xsan_free(pool);
            return NULL;
        }
    }
    
    /* 预分配初始块 */
    for (size_t i = 0; i < config->initial_blocks; i++) {
        memory_pool_block_t *block = xsan_malloc(sizeof(memory_pool_block_t));
        if (!block) {
            break;
        }
        
        block->data = xsan_malloc(config->block_size);
        if (!block->data) {
            xsan_free(block);
            break;
        }
        
        block->in_use = false;
        block->next = pool->free_blocks;
        pool->free_blocks = block;
        
        /* 添加到全部块列表 */
        block->next = pool->all_blocks;
        pool->all_blocks = block;
        
        pool->current_blocks++;
    }
    
    return pool;
}

/**
 * 销毁内存池
 */
void xsan_memory_pool_destroy(xsan_memory_pool_t *pool)
{
    if (!pool) {
        return;
    }
    
    if (pool->config.thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }
    
    /* 释放所有块 */
    memory_pool_block_t *block = pool->all_blocks;
    while (block) {
        memory_pool_block_t *next = block->next;
        xsan_free(block->data);
        xsan_free(block);
        block = next;
    }
    
    if (pool->config.thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
        pthread_mutex_destroy(&pool->mutex);
    }
    
    xsan_free(pool);
}

/**
 * 从内存池分配内存
 */
void *xsan_memory_pool_alloc(xsan_memory_pool_t *pool)
{
    if (!pool) {
        return NULL;
    }
    
    if (pool->config.thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }
    
    void *ptr = NULL;
    
    if (pool->free_blocks) {
        /* 从空闲块中分配 */
        memory_pool_block_t *block = pool->free_blocks;
        pool->free_blocks = block->next;
        block->in_use = true;
        ptr = block->data;
        
        pthread_mutex_lock(&g_memory_mgr.mutex);
        g_memory_mgr.stats.pool_hits++;
        pthread_mutex_unlock(&g_memory_mgr.mutex);
    } else if (pool->current_blocks < pool->config.max_blocks) {
        /* 创建新块 */
        memory_pool_block_t *block = xsan_malloc(sizeof(memory_pool_block_t));
        if (block) {
            block->data = xsan_malloc(pool->config.block_size);
            if (block->data) {
                block->in_use = true;
                block->next = pool->all_blocks;
                pool->all_blocks = block;
                pool->current_blocks++;
                ptr = block->data;
                
                pthread_mutex_lock(&g_memory_mgr.mutex);
                g_memory_mgr.stats.pool_hits++;
                pthread_mutex_unlock(&g_memory_mgr.mutex);
            } else {
                xsan_free(block);
            }
        }
    }
    
    if (!ptr) {
        pthread_mutex_lock(&g_memory_mgr.mutex);
        g_memory_mgr.stats.pool_misses++;
        pthread_mutex_unlock(&g_memory_mgr.mutex);
    }
    
    if (pool->config.thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }
    
    return ptr;
}

/**
 * 归还内存到内存池
 */
void xsan_memory_pool_free(xsan_memory_pool_t *pool, void *ptr)
{
    if (!pool || !ptr) {
        return;
    }
    
    if (pool->config.thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }
    
    /* 查找对应的块 */
    memory_pool_block_t *block = pool->all_blocks;
    while (block) {
        if (block->data == ptr) {
            if (block->in_use) {
                block->in_use = false;
                block->next = pool->free_blocks;
                pool->free_blocks = block;
            }
            break;
        }
        block = block->next;
    }
    
    if (pool->config.thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }
}

/**
 * 获取内存统计信息
 */
xsan_error_t xsan_memory_get_stats(xsan_memory_stats_t *stats)
{
    if (!stats) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&g_memory_mgr.mutex);
    *stats = g_memory_mgr.stats;
    pthread_mutex_unlock(&g_memory_mgr.mutex);
    
    return XSAN_OK;
}

/**
 * 打印内存统计信息
 */
void xsan_memory_print_stats(void)
{
    xsan_memory_stats_t stats;
    
    if (xsan_memory_get_stats(&stats) != XSAN_OK) {
        return;
    }
    
    XSAN_LOG_INFO("Memory Statistics:");
    XSAN_LOG_INFO("  Total Allocated: %lu bytes", stats.total_allocated);
    XSAN_LOG_INFO("  Total Freed: %lu bytes", stats.total_freed);
    XSAN_LOG_INFO("  Current Allocated: %lu bytes", stats.current_allocated);
    XSAN_LOG_INFO("  Peak Allocated: %lu bytes", stats.peak_allocated);
    XSAN_LOG_INFO("  Allocation Count: %lu", stats.allocation_count);
    XSAN_LOG_INFO("  Free Count: %lu", stats.free_count);
    XSAN_LOG_INFO("  Pool Hits: %lu", stats.pool_hits);
    XSAN_LOG_INFO("  Pool Misses: %lu", stats.pool_misses);
}

/**
 * 检查内存泄漏
 */
bool xsan_memory_check_leaks(void)
{
    if (!g_memory_mgr.debug_enabled) {
        return false;
    }
    
    pthread_mutex_lock(&g_memory_mgr.mutex);
    
    bool has_leaks = false;
    memory_block_header_t *header = g_memory_mgr.allocated_blocks;
    
    while (header) {
        if (header->magic == XSAN_MEMORY_MAGIC) {
            XSAN_LOG_ERROR("Memory leak detected: %zu bytes at %s:%d",
                          header->size, header->file, header->line);
            has_leaks = true;
        }
        header = header->next;
    }
    
    pthread_mutex_unlock(&g_memory_mgr.mutex);
    
    return has_leaks;
}

/**
 * 设置内存分配失败回调
 */
void xsan_memory_set_oom_callback(void (*callback)(size_t size))
{
    pthread_mutex_lock(&g_memory_mgr.mutex);
    g_memory_mgr.oom_callback = callback;
    pthread_mutex_unlock(&g_memory_mgr.mutex);
}
