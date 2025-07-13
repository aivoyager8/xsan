#include "xsan_hashtable.h"
#include "xsan_memory.h" // For XSAN_MALLOC, XSAN_CALLOC, XSAN_FREE
#include "../../include/xsan_error.h"
#include <string.h>      // For memset

// Default initial capacity if 0 is provided by the user
#define XSAN_HASHTABLE_DEFAULT_CAPACITY 16
// Note: Load factor and resizing are not implemented in this basic version for simplicity.
// In a production system, resizing would be crucial for maintaining performance.

// Internal structure for a hash table entry (node in the linked list for collisions)
typedef struct xsan_hashtable_entry {
    void *key;
    void *value;
    struct xsan_hashtable_entry *next; // Pointer to the next entry in case of collision (chaining)
} xsan_hashtable_entry_t;

// The main hash table structure
struct xsan_hashtable {
    xsan_hashtable_entry_t **buckets; // Array of pointers to entries (the buckets)
    size_t capacity;                  // Number of buckets in the array
    size_t size;                      // Current number of key-value pairs stored
    xsan_hash_func_t hash_func;             // User-provided hash function
    xsan_key_compare_func_t compare_func;   // User-provided key comparison function
    xsan_key_destroy_func_t key_destroy_func;   // Optional user-provided key destructor
    xsan_value_destroy_func_t value_destroy_func; // Optional user-provided value destructor
};

xsan_hashtable_t *xsan_hashtable_create(size_t initial_capacity,
                                        xsan_hash_func_t hash_func,
                                        xsan_key_compare_func_t compare_func,
                                        xsan_key_destroy_func_t key_destroy_func,
                                        xsan_value_destroy_func_t value_destroy_func) {
    if (hash_func == NULL || compare_func == NULL) {
        return NULL; // Essential functions are missing
    }

    xsan_hashtable_t *ht = (xsan_hashtable_t *)XSAN_MALLOC(sizeof(xsan_hashtable_t));
    if (!ht) {
        return NULL; // Out of memory
    }

    ht->capacity = (initial_capacity > 0) ? initial_capacity : XSAN_HASHTABLE_DEFAULT_CAPACITY;
    ht->size = 0;
    ht->hash_func = hash_func;
    ht->compare_func = compare_func;
    ht->key_destroy_func = key_destroy_func;
    ht->value_destroy_func = value_destroy_func;

    // Allocate memory for the buckets and initialize to NULL
    ht->buckets = (xsan_hashtable_entry_t **)XSAN_CALLOC(ht->capacity, sizeof(xsan_hashtable_entry_t *));
    if (!ht->buckets) {
        XSAN_FREE(ht);
        return NULL; // Out of memory for buckets
    }

    return ht;
}

void xsan_hashtable_destroy(xsan_hashtable_t *ht) {
    if (!ht) {
        return;
    }
    xsan_hashtable_clear(ht); // Clear all entries, calling destructors if provided
    XSAN_FREE(ht->buckets);   // Free the bucket array itself
    XSAN_FREE(ht);            // Free the hash table structure
}

xsan_error_t xsan_hashtable_put(xsan_hashtable_t *ht, void *key, void *value) {
    if (!ht || !key) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    uint32_t hash = ht->hash_func(key);
    size_t index = hash % ht->capacity;

    xsan_hashtable_entry_t *current_entry = ht->buckets[index];
    while (current_entry != NULL) {
        if (ht->compare_func(current_entry->key, key) == 0) {
            // Key already exists, update the value
            if (ht->value_destroy_func && current_entry->value != value) {
                ht->value_destroy_func(current_entry->value); // Destroy old value
            }
            current_entry->value = value;
            // If the key pointer itself is different but content is same,
            // and a key_destroy_func exists, one might want to destroy the old key.
            // This basic version assumes the key object itself is not replaced if it compares equal.
            return XSAN_OK;
        }
        current_entry = current_entry->next;
    }

    // Key does not exist, create a new entry and add it to the front of the chain
    xsan_hashtable_entry_t *new_entry = (xsan_hashtable_entry_t *)XSAN_MALLOC(sizeof(xsan_hashtable_entry_t));
    if (!new_entry) {
        return XSAN_ERROR_NO_MEMORY;
    }
    new_entry->key = key;
    new_entry->value = value;
    new_entry->next = ht->buckets[index]; // New entry points to the old head of the chain
    ht->buckets[index] = new_entry;       // New entry becomes the new head
    ht->size++;

    // Note: Resizing logic would go here if implemented
    // e.g., if ((float)ht->size / ht->capacity > XSAN_HASHTABLE_MAX_LOAD_FACTOR) { xsan_hashtable_resize(ht, ...); }

    return XSAN_OK;
}

void *xsan_hashtable_get(xsan_hashtable_t *ht, const void *key) {
    if (!ht || !key) {
        return NULL;
    }

    uint32_t hash = ht->hash_func(key);
    size_t index = hash % ht->capacity;

    xsan_hashtable_entry_t *entry = ht->buckets[index];
    while (entry != NULL) {
        if (ht->compare_func(entry->key, key) == 0) {
            return entry->value; // Key found
        }
        entry = entry->next;
    }
    return NULL; // Key not found
}

xsan_error_t xsan_hashtable_remove(xsan_hashtable_t *ht, const void *key)
{
    if (!ht || !key) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    uint32_t hash = ht->hash_func(key);
    size_t index = hash % ht->capacity;
    xsan_hashtable_entry_t *current_entry = ht->buckets[index];
    xsan_hashtable_entry_t *prev_entry = NULL;
    while (current_entry != NULL) {
        if (ht->compare_func(current_entry->key, key) == 0) {
            // Key found, remove it from the chain
            if (prev_entry) {
                prev_entry->next = current_entry->next;
            } else {
                ht->buckets[index] = current_entry->next;
            }
            if (ht->key_destroy_func) {
                ht->key_destroy_func(current_entry->key);
            }
            if (ht->value_destroy_func) {
                ht->value_destroy_func(current_entry->value);
            }
            XSAN_FREE(current_entry);
            ht->size--;
            return XSAN_OK;
        }
        prev_entry = current_entry;
        current_entry = current_entry->next;
    }
    return XSAN_ERROR_NOT_FOUND;
}

size_t xsan_hashtable_size(xsan_hashtable_t *ht)
{
    if (!ht) {
        return 0;
    }
    return ht->size;
}

void xsan_hashtable_clear(xsan_hashtable_t *ht)
{
    if (!ht) {
        return;
    }
    for (size_t i = 0; i < ht->capacity; ++i) {
        xsan_hashtable_entry_t *entry = ht->buckets[i];
        while (entry) {
            xsan_hashtable_entry_t *next = entry->next;
            if (ht->key_destroy_func) {
                ht->key_destroy_func(entry->key);
            }
            if (ht->value_destroy_func) {
                ht->value_destroy_func(entry->value);
            }
            XSAN_FREE(entry);
            entry = next;
        }
        ht->buckets[i] = NULL;
    }
    ht->size = 0;
}

void xsan_hashtable_iter_init(xsan_hashtable_t *ht, xsan_hashtable_iter_t *iter) {
    if (!ht || !iter) {
        if (iter) iter->ht = NULL;
        return;
    }
    iter->ht = ht;
    iter->current_bucket_index = 0;
    iter->current_entry = NULL;
}

// 哈希表迭代器遍历下一个元素
bool xsan_hashtable_iter_next(xsan_hashtable_iter_t *iter, void **key, void **value) {
    if (!iter || !iter->ht) {
        return false;
    }
    if (iter->current_entry) {
        iter->current_entry = iter->current_entry->next;
    }
    while (iter->current_entry == NULL) {
        if (iter->current_bucket_index >= iter->ht->capacity) {
            return false;
        }
        iter->current_entry = iter->ht->buckets[iter->current_bucket_index];
        iter->current_bucket_index++;
    }
    if (key) {
        *key = iter->current_entry->key;
    }
    if (value) {
        *value = iter->current_entry->value;
    }
    return true;
}
