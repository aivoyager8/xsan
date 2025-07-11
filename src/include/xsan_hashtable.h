#ifndef XSAN_HASHTABLE_H
#define XSAN_HASHTABLE_H

#include "xsan_types.h" // For xsan_error_t and potentially common types
#include <stddef.h>     // For size_t
#include <stdint.h>     // For uint32_t
#include <stdbool.h>    // For bool

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct xsan_hashtable xsan_hashtable_t;

// Key hashing function pointer type
typedef uint32_t (*xsan_hash_func_t)(const void *key);

// Key comparison function pointer type
// Returns 0 if keys are equal, non-zero otherwise
typedef int (*xsan_key_compare_func_t)(const void *key1, const void *key2);

// Value destructor function pointer type (optional, for freeing complex values)
typedef void (*xsan_value_destroy_func_t)(void *value);
// Key destructor function pointer type (optional, for freeing complex keys)
typedef void (*xsan_key_destroy_func_t)(void *key);

/**
 * Creates a new hash table.
 *
 * @param initial_capacity The initial number of buckets in the hash table.
 *                         If 0, a default capacity will be used.
 * @param hash_func Pointer to the hash function for keys. Must not be NULL.
 * @param compare_func Pointer to the key comparison function. Must not be NULL.
 * @param key_destroy_func Optional function to destroy keys when removed or table is destroyed. Can be NULL.
 * @param value_destroy_func Optional function to destroy values when removed or table is destroyed. Can be NULL.
 * @return A pointer to the new hash table, or NULL on error (e.g., OOM, invalid params).
 */
xsan_hashtable_t *xsan_hashtable_create(size_t initial_capacity,
                                        xsan_hash_func_t hash_func,
                                        xsan_key_compare_func_t compare_func,
                                        xsan_key_destroy_func_t key_destroy_func,
                                        xsan_value_destroy_func_t value_destroy_func);

/**
 * Destroys a hash table.
 * Frees all memory associated with the hash table, including all entries.
 * If key_destroy_func or value_destroy_func were provided at creation,
 * they will be called for each key or value respectively.
 *
 * @param ht The hash table to destroy. If NULL, the function does nothing.
 */
void xsan_hashtable_destroy(xsan_hashtable_t *ht);

/**
 * Inserts or updates a key-value pair in the hash table.
 * If the key already exists in the table, its associated value is updated.
 * If a value_destroy_func was provided and an old value is being replaced,
 * the old value will be destroyed.
 * The table does not make copies of the key or value data; it stores the pointers.
 *
 * @param ht The hash table. Must not be NULL.
 * @param key The key. Must not be NULL.
 * @param value The value associated with the key.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM if ht or key is NULL,
 *         or XSAN_ERROR_OUT_OF_MEMORY if a new entry cannot be allocated.
 */
xsan_error_t xsan_hashtable_put(xsan_hashtable_t *ht, void *key, void *value);

/**
 * Retrieves a value from the hash table by its key.
 *
 * @param ht The hash table. Must not be NULL.
 * @param key The key to look up. Must not be NULL.
 * @return The value associated with the key, or NULL if the key is not found or if ht/key is NULL.
 */
void *xsan_hashtable_get(xsan_hashtable_t *ht, const void *key);

/**
 * Removes a key-value pair from the hash table.
 * If key_destroy_func or value_destroy_func were provided at creation,
 * they will be called for the removed key and value respectively.
 *
 * @param ht The hash table. Must not be NULL.
 * @param key The key to remove. Must not be NULL.
 * @return XSAN_OK if the key was found and removed,
 *         XSAN_ERROR_NOT_FOUND if the key was not found,
 *         or XSAN_ERROR_INVALID_PARAM if ht or key is NULL.
 */
xsan_error_t xsan_hashtable_remove(xsan_hashtable_t *ht, const void *key);

/**
 * Returns the number of key-value pairs currently in the hash table.
 *
 * @param ht The hash table.
 * @return The number of items, or 0 if ht is NULL.
 */
size_t xsan_hashtable_size(xsan_hashtable_t *ht);

/**
 * Clears all key-value pairs from the hash table.
 * If key_destroy_func or value_destroy_func were provided at creation,
 * they will be called for all keys and values respectively.
 * The table capacity remains the same.
 *
 * @param ht The hash table. If NULL, the function does nothing.
 */
void xsan_hashtable_clear(xsan_hashtable_t *ht);

// Iterator structure
typedef struct xsan_hashtable_iter {
    xsan_hashtable_t *ht;
    size_t current_bucket_index;
    struct xsan_hashtable_entry *current_entry; // Internal structure, avoid direct use
} xsan_hashtable_iter_t;

/**
 * Initializes a hash table iterator.
 * The iterator should be initialized before its first use with xsan_hashtable_iter_next.
 *
 * @param ht The hash table to iterate over. Must not be NULL.
 * @param iter Pointer to the iterator structure to initialize. Must not be NULL.
 */
void xsan_hashtable_iter_init(xsan_hashtable_t *ht, xsan_hashtable_iter_t *iter);

/**
 * Advances the iterator to the next key-value pair in the hash table.
 * The order of iteration is not guaranteed.
 *
 * @param iter The iterator, previously initialized with xsan_hashtable_iter_init. Must not be NULL.
 * @param key Pointer to a void pointer where the key of the next entry will be stored. Can be NULL if key is not needed.
 * @param value Pointer to a void pointer where the value of the next entry will be stored. Can be NULL if value is not needed.
 * @return true if a next element was found and its key/value (if requested) are stored in the output parameters,
 *         false if there are no more elements or if iter is NULL.
 */
bool xsan_hashtable_iter_next(xsan_hashtable_iter_t *iter, void **key, void **value);

#ifdef __cplusplus
}
#endif

#endif // XSAN_HASHTABLE_H
