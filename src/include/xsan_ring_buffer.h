#ifndef XSAN_RING_BUFFER_H
#define XSAN_RING_BUFFER_H

#include "xsan_types.h" // For xsan_error_t
#include <stddef.h>     // For size_t
#include <stdbool.h>    // For bool

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct xsan_ring_buffer xsan_ring_buffer_t;

// Optional value destructor function pointer type for elements when buffer is destroyed or cleared
typedef void (*xsan_ring_buffer_value_destroy_func_t)(void *value);

/**
 * Creates a new ring buffer.
 * The ring buffer will store void* pointers.
 *
 * @param capacity The maximum number of elements the ring buffer can hold. Must be greater than 0.
 * @param value_destroy_func Optional function to destroy values when the buffer is destroyed
 *                           or when elements are cleared. Can be NULL.
 * @return A pointer to the new ring buffer, or NULL on error (e.g., invalid capacity, OOM).
 */
xsan_ring_buffer_t *xsan_ring_buffer_create(size_t capacity,
                                            xsan_ring_buffer_value_destroy_func_t value_destroy_func);

/**
 * Destroys a ring buffer.
 * Frees all memory associated with the ring buffer. If a value_destroy_func was provided,
 * it will be called for any remaining elements in the buffer.
 *
 * @param rb The ring buffer to destroy. If NULL, the function does nothing.
 */
void xsan_ring_buffer_destroy(xsan_ring_buffer_t *rb);

/**
 * Pushes (enqueues) an element onto the tail of the ring buffer.
 * If the buffer is full, this implementation returns an error.
 *
 * @param rb The ring buffer. Must not be NULL.
 * @param value The value (pointer) to push.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM if rb is NULL,
 *         or XSAN_ERROR_INSUFFICIENT_SPACE if the buffer is full.
 */
xsan_error_t xsan_ring_buffer_push(xsan_ring_buffer_t *rb, void *value);

/**
 * Pops (dequeues) an element from the head of the ring buffer.
 *
 * @param rb The ring buffer. Must not be NULL.
 * @param value_ptr Pointer to a void pointer where the popped value will be stored. Must not be NULL.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM if rb or value_ptr is NULL,
 *         or XSAN_ERROR_NOT_FOUND if the buffer is empty.
 */
xsan_error_t xsan_ring_buffer_pop(xsan_ring_buffer_t *rb, void **value_ptr);

/**
 * Peeks at the front element (head) of the ring buffer without removing it.
 *
 * @param rb The ring buffer. Must not be NULL.
 * @param value_ptr Pointer to a void pointer where the peeked value will be stored. Must not be NULL.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM if rb or value_ptr is NULL,
 *         or XSAN_ERROR_NOT_FOUND if the buffer is empty.
 */
xsan_error_t xsan_ring_buffer_peek(xsan_ring_buffer_t *rb, void **value_ptr);

/**
 * Returns the number of elements currently in the ring buffer.
 *
 * @param rb The ring buffer.
 * @return The number of elements, or 0 if rb is NULL.
 */
size_t xsan_ring_buffer_size(xsan_ring_buffer_t *rb);

/**
 * Returns the maximum capacity of the ring buffer.
 *
 * @param rb The ring buffer.
 * @return The capacity, or 0 if rb is NULL.
 */
size_t xsan_ring_buffer_capacity(xsan_ring_buffer_t *rb);

/**
 * Checks if the ring buffer is empty.
 *
 * @param rb The ring buffer.
 * @return true if the buffer is empty or rb is NULL, false otherwise.
 */
bool xsan_ring_buffer_is_empty(xsan_ring_buffer_t *rb);

/**
 * Checks if the ring buffer is full.
 *
 * @param rb The ring buffer.
 * @return true if the buffer is full, false if it's not full or rb is NULL.
 */
bool xsan_ring_buffer_is_full(xsan_ring_buffer_t *rb);

/**
 * Clears all elements from the ring buffer.
 * If a value_destroy_func was provided, it's called for each element being cleared.
 * The buffer's capacity remains unchanged. Head and tail pointers are reset.
 *
 * @param rb The ring buffer. If NULL, the function does nothing.
 */
void xsan_ring_buffer_clear(xsan_ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif // XSAN_RING_BUFFER_H
