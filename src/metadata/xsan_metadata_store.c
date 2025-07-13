#include "xsan_metadata_store.h"
#include "xsan_memory.h" // For XSAN_MALLOC, XSAN_FREE, xsan_strdup
#include "../../include/xsan_error.h"
#include "xsan_log.h"

#include "rocksdb/c.h"   // RocksDB C API
#include <string.h>      // For memcpy, strlen

xsan_metadata_store_t *xsan_metadata_store_open(const char *db_path, bool create_if_missing) {
    if (!db_path) {
        XSAN_LOG_ERROR("Database path is NULL for metadata store open.");
        return NULL;
    }

    xsan_metadata_store_t *store = (xsan_metadata_store_t *)XSAN_MALLOC(sizeof(xsan_metadata_store_t));
    if (!store) {
        XSAN_LOG_ERROR("Failed to allocate memory for xsan_metadata_store_t.");
        return NULL;
    }
    memset(store, 0, sizeof(xsan_metadata_store_t));

    store->db_options = rocksdb_options_create();
    if (!store->db_options) {
        XSAN_LOG_ERROR("Failed to create RocksDB options.");
        XSAN_FREE(store);
        return NULL;
    }
    rocksdb_options_set_create_if_missing(store->db_options, create_if_missing ? 1 : 0);
    // rocksdb_options_increase_parallelism(store->db_options, spdk_env_get_core_count()); // Example advanced option
    // rocksdb_options_optimize_level_style_compaction(store->db_options, 0); // Example

    store->write_options = rocksdb_writeoptions_create();
    if (!store->write_options) {
        XSAN_LOG_ERROR("Failed to create RocksDB write options.");
        rocksdb_options_destroy(store->db_options);
        XSAN_FREE(store);
        return NULL;
    }
    // rocksdb_writeoptions_disable_WAL(store->write_options, 1); // Example: if WAL not desired for some use cases

    store->read_options = rocksdb_readoptions_create();
    if (!store->read_options) {
        XSAN_LOG_ERROR("Failed to create RocksDB read options.");
        rocksdb_writeoptions_destroy(store->write_options);
        rocksdb_options_destroy(store->db_options);
        XSAN_FREE(store);
        return NULL;
    }
    // rocksdb_readoptions_set_fill_cache(store->read_options, 0); // Example: Don't fill block cache for this read

    char *err_ptr = NULL;
    store->db_handle = rocksdb_open(store->db_options, db_path, &err_ptr);

    if (err_ptr) {
        XSAN_LOG_ERROR("Failed to open RocksDB database at '%s': %s", db_path, err_ptr);
        rocksdb_free(err_ptr); // Important to free error string from RocksDB
        rocksdb_readoptions_destroy(store->read_options);
        rocksdb_writeoptions_destroy(store->write_options);
        rocksdb_options_destroy(store->db_options);
        XSAN_FREE(store);
        return NULL;
    }
    if (!store->db_handle) { // Should be redundant if err_ptr is checked, but good practice
        XSAN_LOG_ERROR("RocksDB open returned NULL handle without error string (path: %s).", db_path);
        rocksdb_readoptions_destroy(store->read_options);
        rocksdb_writeoptions_destroy(store->write_options);
        rocksdb_options_destroy(store->db_options);
        XSAN_FREE(store);
        return NULL;
    }

    store->db_path_copy = xsan_strdup(db_path); // Store a copy of the path for reference
    XSAN_LOG_INFO("RocksDB metadata store opened successfully at '%s'.", db_path);
    return store;
}

void xsan_metadata_store_close(xsan_metadata_store_t *store) {
    if (!store) {
        return;
    }
    XSAN_LOG_INFO("Closing RocksDB metadata store at '%s'.", store->db_path_copy ? store->db_path_copy : "unknown_path");
    if (store->db_handle) {
        rocksdb_close(store->db_handle);
    }
    if (store->read_options) {
        rocksdb_readoptions_destroy(store->read_options);
    }
    if (store->write_options) {
        rocksdb_writeoptions_destroy(store->write_options);
    }
    if (store->db_options) {
        rocksdb_options_destroy(store->db_options);
    }
    if (store->db_path_copy) {
        XSAN_FREE(store->db_path_copy);
    }
    XSAN_FREE(store);
}

xsan_error_t xsan_metadata_store_put(xsan_metadata_store_t *store,
                                     const char *key, size_t key_len,
                                     const char *value, size_t value_len) {
    if (!store || !store->db_handle || !key || key_len == 0 || !value) { // value_len can be 0 for empty value
        return XSAN_ERROR_INVALID_PARAM;
    }
    char *err_ptr = NULL;
    rocksdb_put(store->db_handle, store->write_options, key, key_len, value, value_len, &err_ptr);
    if (err_ptr) {
        XSAN_LOG_ERROR("RocksDB put failed for key '%.*s': %s", (int)key_len, key, err_ptr);
        rocksdb_free(err_ptr);
        return XSAN_ERROR_IO; // Or a more specific metadata store error
    }
    return XSAN_OK;
}

xsan_error_t xsan_metadata_store_get(xsan_metadata_store_t *store,
                                     const char *key, size_t key_len,
                                     char **value_out, size_t *value_len_out) {
    if (!store || !store->db_handle || !key || key_len == 0 || !value_out || !value_len_out) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    char *err_ptr = NULL;
    *value_out = rocksdb_get(store->db_handle, store->read_options, key, key_len, value_len_out, &err_ptr);

    if (err_ptr) {
        XSAN_LOG_ERROR("RocksDB get failed for key '%.*s': %s", (int)key_len, key, err_ptr);
        rocksdb_free(err_ptr);
        if (*value_out) { // Should not happen if err_ptr is set, but defensive
            rocksdb_free(*value_out);
            *value_out = NULL;
        }
        *value_len_out = 0;
        return XSAN_ERROR_IO;
    }

    if (*value_out == NULL) { // Key not found
        *value_len_out = 0;
        return XSAN_ERROR_NOT_FOUND;
    }

    // RocksDB's value from rocksdb_get needs to be freed by rocksdb_free.
    // Our API contract says caller frees with XSAN_FREE. So we must copy.
    char *copied_value = (char *)XSAN_MALLOC(*value_len_out + 1); // +1 for null terminator if string
    if (!copied_value) {
        rocksdb_free(*value_out); // Free the rocksdb allocated buffer
        *value_out = NULL;
        *value_len_out = 0;
        return XSAN_ERROR_NO_MEMORY;
    }
    memcpy(copied_value, *value_out, *value_len_out);
    copied_value[*value_len_out] = '\0'; // Ensure null termination if it's a string

    rocksdb_free(*value_out); // Free original RocksDB buffer
    *value_out = copied_value; // Return the copy

    return XSAN_OK;
}

xsan_error_t xsan_metadata_store_delete(xsan_metadata_store_t *store,
                                        const char *key, size_t key_len) {
    if (!store || !store->db_handle || !key || key_len == 0) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    char *err_ptr = NULL;
    rocksdb_delete(store->db_handle, store->write_options, key, key_len, &err_ptr);
    if (err_ptr) {
        XSAN_LOG_ERROR("RocksDB delete failed for key '%.*s': %s", (int)key_len, key, err_ptr);
        rocksdb_free(err_ptr);
        return XSAN_ERROR_IO;
    }
    return XSAN_OK;
}

// --- Iterator Functions ---

xsan_metadata_iterator_t *xsan_metadata_iterator_create(xsan_metadata_store_t *store) {
    if (!store || !store->db_handle) {
        return NULL;
    }
    xsan_metadata_iterator_t *iter = (xsan_metadata_iterator_t *)XSAN_MALLOC(sizeof(xsan_metadata_iterator_t));
    if (!iter) {
        XSAN_LOG_ERROR("Failed to allocate memory for xsan_metadata_iterator_t.");
        return NULL;
    }
    // Use the store's default read options for the iterator
    iter->rocksdb_iter_handle = rocksdb_create_iterator(store->db_handle, store->read_options);
    if (!iter->rocksdb_iter_handle) {
        XSAN_LOG_ERROR("Failed to create RocksDB iterator.");
        XSAN_FREE(iter);
        return NULL;
    }
    return iter;
}

void xsan_metadata_iterator_destroy(xsan_metadata_iterator_t *iter) {
    if (!iter) {
        return;
    }
    if (iter->rocksdb_iter_handle) {
        rocksdb_iter_destroy(iter->rocksdb_iter_handle);
    }
    XSAN_FREE(iter);
}

void xsan_metadata_iterator_seek_to_first(xsan_metadata_iterator_t *iter) {
    if (iter && iter->rocksdb_iter_handle) {
        rocksdb_iter_seek_to_first(iter->rocksdb_iter_handle);
    }
}

void xsan_metadata_iterator_seek(xsan_metadata_iterator_t *iter, const char *seek_key, size_t seek_key_len) {
    if (iter && iter->rocksdb_iter_handle && seek_key) {
        rocksdb_iter_seek(iter->rocksdb_iter_handle, seek_key, seek_key_len);
    }
}

void xsan_metadata_iterator_next(xsan_metadata_iterator_t *iter) {
    if (iter && iter->rocksdb_iter_handle && rocksdb_iter_valid(iter->rocksdb_iter_handle)) {
        rocksdb_iter_next(iter->rocksdb_iter_handle);
    }
}

bool xsan_metadata_iterator_is_valid(xsan_metadata_iterator_t *iter) {
    if (iter && iter->rocksdb_iter_handle) {
        return rocksdb_iter_valid(iter->rocksdb_iter_handle);
    }
    return false;
}

const char *xsan_metadata_iterator_key(xsan_metadata_iterator_t *iter, size_t *key_len_out) {
    if (iter && iter->rocksdb_iter_handle && rocksdb_iter_valid(iter->rocksdb_iter_handle)) {
        return rocksdb_iter_key(iter->rocksdb_iter_handle, key_len_out);
    }
    if (key_len_out) *key_len_out = 0;
    return NULL;
}

const char *xsan_metadata_iterator_value(xsan_metadata_iterator_t *iter, size_t *value_len_out) {
    if (iter && iter->rocksdb_iter_handle && rocksdb_iter_valid(iter->rocksdb_iter_handle)) {
        return rocksdb_iter_value(iter->rocksdb_iter_handle, value_len_out);
    }
    if (value_len_out) *value_len_out = 0;
    return NULL;
}
