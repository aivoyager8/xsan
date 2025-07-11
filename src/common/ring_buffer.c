#include "xsan_ring_buffer.h"
#include "xsan_memory.h" // For XSAN_MALLOC, XSAN_CALLOC, XSAN_FREE
#include "xsan_error.h"  // For error codes

// The main ring buffer structure
struct xsan_ring_buffer {
    void **buffer;          // The underlying array to store element pointers
    size_t capacity;        // Maximum number of elements the buffer can hold
    size_t count;           // Current number of elements in the buffer
    size_t head;            // Index of the first (oldest) element for popping
    size_t tail;            // Index where the next element will be inserted (pushed)
    xsan_ring_buffer_value_destroy_func_t value_destroy_func; // Optional destructor for values
    // For a thread-safe version, a pthread_mutex_t would be added here.
};

xsan_ring_buffer_t *xsan_ring_buffer_create(size_t capacity,
                                            xsan_ring_buffer_value_destroy_func_t value_destroy_func) {
    if (capacity == 0) {
        return NULL; // Capacity must be greater than 0
    }

    xsan_ring_buffer_t *rb = (xsan_ring_buffer_t *)XSAN_MALLOC(sizeof(xsan_ring_buffer_t));
    if (!rb) {
        return NULL; // Failed to allocate memory for the ring buffer structure
    }

    rb->buffer = (void **)XSAN_CALLOC(capacity, sizeof(void *));
    if (!rb->buffer) {
        XSAN_FREE(rb);
        return NULL; // Failed to allocate memory for the buffer itself
    }

    rb->capacity = capacity;
    rb->count = 0;
    rb->head = 0;
    rb->tail = 0;
    rb->value_destroy_func = value_destroy_func;

    return rb;
}

void xsan_ring_buffer_destroy(xsan_ring_buffer_t *rb) {
    if (!rb) {
        return;
    }
    // Clear will call destructors for remaining elements
    xsan_ring_buffer_clear(rb);
    XSAN_FREE(rb->buffer); // Free the underlying buffer array
    XSAN_FREE(rb);         // Free the ring buffer structure
}

xsan_error_t xsan_ring_buffer_push(xsan_ring_buffer_t *rb, void *value) {
    if (!rb) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (rb->count == rb->capacity) {
        return XSAN_ERROR_INSUFFICIENT_SPACE; // Buffer is full
    }

    rb->buffer[rb->tail] = value;
    rb->tail = (rb->tail + 1) % rb->capacity; // Wrap around if necessary
    rb->count++;

    return XSAN_OK;
}

xsan_error_t xsan_ring_buffer_pop(xsan_ring_buffer_t *rb, void **value_ptr) {
    if (!rb || !value_ptr) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (rb->count == 0) {
        return XSAN_ERROR_NOT_FOUND; // Buffer is empty
    }

    *value_ptr = rb->buffer[rb->head];
    // Optional: rb->buffer[rb->head] = NULL; // Defensively clear the slot
    rb->head = (rb->head + 1) % rb->capacity; // Wrap around if necessary
    rb->count--;

    return XSAN_OK;
}

xsan_error_t xsan_ring_buffer_peek(xsan_ring_buffer_t *rb, void **value_ptr) {
    if (!rb || !value_ptr) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (rb->count == 0) {
        return XSAN_ERROR_NOT_FOUND; // Buffer is empty
    }

    *value_ptr = rb->buffer[rb->head];
    return XSAN_OK;
}

size_t xsan_ring_buffer_size(xsan_ring_buffer_t *rb) {
    return rb ? rb->count : 0;
}

size_t xsan_ring_buffer_capacity(xsan_ring_buffer_t *rb) {
    return rb ? rb->capacity : 0;
}

bool xsan_ring_buffer_is_empty(xsan_ring_buffer_t *rb) {
    return (rb == NULL || rb->count == 0);
}

bool xsan_ring_buffer_is_full(xsan_ring_buffer_t *rb) {
    return (rb != NULL && rb->count == rb->capacity);
}

void xsan_ring_buffer_clear(xsan_ring_buffer_t *rb) {
    if (!rb) {
        return;
    }

    if (rb->value_destroy_func) {
        // Iterate through the elements currently in the buffer and destroy them
        size_t current_head = rb->head;
        for (size_t i = 0; i < rb->count; ++i) {
            void* value_to_destroy = rb->buffer[current_head];
            if (value_to_destroy) { // Check if the pointer is not NULL
                 rb->value_destroy_func(value_to_destroy);
            }
            // rb->buffer[current_head] = NULL; // Optionally clear pointer after destruction
            current_head = (current_head + 1) % rb->capacity;
        }
    }
    // Reset the buffer state
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    // Note: The actual pointers in rb->buffer are not zeroed out here unless done
    // during the destruction loop above. This is generally fine as they are
    // considered "garbage" slots until overwritten by new pushes.
}
