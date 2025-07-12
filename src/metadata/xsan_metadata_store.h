// xsan_metadata_store.h
// 空头文件，后续补充接口声明
#pragma once
#include <stdbool.h>
#include <stddef.h>

// 元数据存储结构体
typedef struct xsan_metadata_store_t {
    void *db_handle;
    void *db_options;
    void *write_options;
    void *read_options;
    char *db_path_copy;
} xsan_metadata_store_t;

// 元数据迭代器结构体
typedef struct xsan_metadata_iterator_t {
    void *rocksdb_iter_handle;
    xsan_metadata_store_t *store;
} xsan_metadata_iterator_t;

// 接口声明（可根据实现补充）
xsan_metadata_store_t *xsan_metadata_store_open(const char *db_path, bool create_if_missing);
void xsan_metadata_store_close(xsan_metadata_store_t *store);
// ... 其他接口 ...
