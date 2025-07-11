#ifndef XSAN_LIST_H
#define XSAN_LIST_H

#include "xsan_types.h" // For xsan_error_t
#include <stddef.h>     // For size_t
#include <stdbool.h>    // For bool

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct xsan_list xsan_list_t;
typedef struct xsan_list_node xsan_list_node_t;

// Optional value destructor function pointer type
typedef void (*xsan_list_value_destroy_func_t)(void *value);

/**
 * Creates a new doubly linked list.
 *
 * @param value_destroy_func Optional function to destroy values when nodes are removed
 *                           or the list is destroyed. Can be NULL if values are managed
 *                           externally or are simple types.
 * @return A pointer to the new list, or NULL on error (e.g., XSAN_ERROR_OUT_OF_MEMORY).
 */
xsan_list_t *xsan_list_create(xsan_list_value_destroy_func_t value_destroy_func);

/**
 * Destroys a list.
 * Frees all memory associated with the list, including all nodes.
 * If a value_destroy_func was provided at creation, it will be called for each value.
 *
 * @param list The list to destroy. If NULL, the function does nothing.
 */
void xsan_list_destroy(xsan_list_t *list);

/**
 * Appends a new value to the end of the list.
 *
 * @param list The list. Must not be NULL.
 * @param value The value to append. The list stores this pointer directly.
 * @return A pointer to the newly created node, or NULL on failure (e.g., XSAN_ERROR_OUT_OF_MEMORY).
 */
xsan_list_node_t *xsan_list_append(xsan_list_t *list, void *value);

/**
 * Prepends a new value to the beginning of the list.
 *
 * @param list The list. Must not be NULL.
 * @param value The value to prepend. The list stores this pointer directly.
 * @return A pointer to the newly created node, or NULL on failure.
 */
xsan_list_node_t *xsan_list_prepend(xsan_list_t *list, void *value);

/**
 * Inserts a new value after a specified node.
 *
 * @param list The list. Must not be NULL.
 * @param node The node after which to insert the new value. Must not be NULL and must belong to the list.
 * @param value The value to insert.
 * @return A pointer to the newly created node, or NULL on failure.
 */
xsan_list_node_t *xsan_list_insert_after(xsan_list_t *list, xsan_list_node_t *node, void *value);

/**
 * Inserts a new value before a specified node.
 *
 * @param list The list. Must not be NULL.
 * @param node The node before which to insert the new value. Must not be NULL and must belong to the list.
 * @param value The value to insert.
 * @return A pointer to the newly created node, or NULL on failure.
 */
xsan_list_node_t *xsan_list_insert_before(xsan_list_t *list, xsan_list_node_t *node, void *value);

/**
 * Removes a node from the list.
 * If a value_destroy_func was provided, it's called on the node's value.
 * The node itself is freed.
 *
 * @param list The list. Must not be NULL.
 * @param node The node to remove. Must not be NULL and must belong to the list.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM if list or node is NULL.
 */
xsan_error_t xsan_list_remove_node(xsan_list_t *list, xsan_list_node_t *node);

/**
 * Removes and returns the value from the head of the list.
 * The node is freed. The value is NOT destroyed by this function; caller takes ownership.
 *
 * @param list The list. Must not be NULL.
 * @return The value from the head, or NULL if the list is empty.
 */
void *xsan_list_pop_front(xsan_list_t *list);

/**
 * Removes and returns the value from the tail of the list.
 * The node is freed. The value is NOT destroyed by this function; caller takes ownership.
 *
 * @param list The list. Must not be NULL.
 * @return The value from the tail, or NULL if the list is empty.
 */
void *xsan_list_pop_back(xsan_list_t *list);


/**
 * Gets the head node of the list.
 *
 * @param list The list.
 * @return A pointer to the head node, or NULL if the list is empty or list is NULL.
 */
xsan_list_node_t *xsan_list_get_head(xsan_list_t *list);

/**
 * Gets the tail node of the list.
 *
 * @param list The list.
 * @return A pointer to the tail node, or NULL if the list is empty or list is NULL.
 */
xsan_list_node_t *xsan_list_get_tail(xsan_list_t *list);

/**
 * Gets the next node in the list.
 *
 * @param node The current node.
 * @return A pointer to the next node, or NULL if the current node is the tail or node is NULL.
 */
xsan_list_node_t *xsan_list_node_next(xsan_list_node_t *node);

/**
 * Gets the previous node in the list.
 *
 * @param node The current node.
 * @return A pointer to the previous node, or NULL if the current node is the head or node is NULL.
 */
xsan_list_node_t *xsan_list_node_prev(xsan_list_node_t *node);

/**
 * Gets the value stored in a list node.
 *
 * @param node The list node.
 * @return The value stored in the node, or NULL if node is NULL.
 */
void *xsan_list_node_get_value(xsan_list_node_t *node);

/**
 * Returns the number of elements in the list.
 *
 * @param list The list.
 * @return The number of elements, or 0 if list is NULL.
 */
size_t xsan_list_size(xsan_list_t *list);

/**
 * Checks if the list is empty.
 *
 * @param list The list.
 * @return true if the list is empty or NULL, false otherwise.
 */
bool xsan_list_is_empty(xsan_list_t *list);

/**
 * Clears all nodes from the list.
 * If a value_destroy_func was provided, it's called for each value.
 *
 * @param list The list. If NULL, the function does nothing.
 */
void xsan_list_clear(xsan_list_t *list);

// Convenience macro for iterating through the list (forward)
#define XSAN_LIST_FOREACH(list, current_node) \
    for (xsan_list_node_t *current_node = xsan_list_get_head(list); \
         current_node != NULL; \
         current_node = xsan_list_node_next(current_node))

// Convenience macro for iterating through the list (backward)
#define XSAN_LIST_FOREACH_REVERSE(list, current_node) \
    for (xsan_list_node_t *current_node = xsan_list_get_tail(list); \
         current_node != NULL; \
         current_node = xsan_list_node_prev(current_node))

#ifdef __cplusplus
}
#endif

#endif // XSAN_LIST_H
