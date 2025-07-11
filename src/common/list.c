#include "xsan_list.h"
#include "xsan_memory.h" // For XSAN_MALLOC, XSAN_FREE
#include "xsan_error.h"  // For error codes

// Internal structure for a list node
struct xsan_list_node {
    void *value;
    xsan_list_node_t *prev;
    xsan_list_node_t *next;
};

// The main list structure
struct xsan_list {
    xsan_list_node_t *head;
    xsan_list_node_t *tail;
    size_t size;
    xsan_list_value_destroy_func_t value_destroy_func;
};

xsan_list_t *xsan_list_create(xsan_list_value_destroy_func_t value_destroy_func) {
    xsan_list_t *list = (xsan_list_t *)XSAN_MALLOC(sizeof(xsan_list_t));
    if (!list) {
        return NULL; // Allocation failed
    }
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
    list->value_destroy_func = value_destroy_func;
    return list;
}

void xsan_list_destroy(xsan_list_t *list) {
    if (!list) {
        return;
    }
    xsan_list_clear(list); // Clear all nodes and call destructors
    XSAN_FREE(list);       // Free the list structure itself
}

// Helper function to create a new node
static xsan_list_node_t *create_new_node(void *value) {
    xsan_list_node_t *node = (xsan_list_node_t *)XSAN_MALLOC(sizeof(xsan_list_node_t));
    if (!node) {
        return NULL; // Allocation failed
    }
    node->value = value;
    node->prev = NULL;
    node->next = NULL;
    return node;
}

xsan_list_node_t *xsan_list_append(xsan_list_t *list, void *value) {
    if (!list) {
        return NULL;
    }
    xsan_list_node_t *node = create_new_node(value);
    if (!node) {
        return NULL; // Node creation failed
    }
    if (list->tail == NULL) { // List is currently empty
        list->head = node;
        list->tail = node;
    } else {
        list->tail->next = node;
        node->prev = list->tail;
        list->tail = node;
    }
    list->size++;
    return node;
}

xsan_list_node_t *xsan_list_prepend(xsan_list_t *list, void *value) {
    if (!list) {
        return NULL;
    }
    xsan_list_node_t *node = create_new_node(value);
    if (!node) {
        return NULL; // Node creation failed
    }
    if (list->head == NULL) { // List is currently empty
        list->head = node;
        list->tail = node;
    } else {
        list->head->prev = node;
        node->next = list->head;
        list->head = node;
    }
    list->size++;
    return node;
}

xsan_list_node_t *xsan_list_insert_after(xsan_list_t *list, xsan_list_node_t *prev_node, void *value) {
    if (!list || !prev_node) {
        return NULL; // Invalid parameters
    }
    // If prev_node is the tail, it's equivalent to append
    if (prev_node == list->tail) {
        return xsan_list_append(list, value);
    }

    xsan_list_node_t *new_node = create_new_node(value);
    if (!new_node) {
        return NULL; // Node creation failed
    }

    new_node->next = prev_node->next;
    new_node->prev = prev_node;
    if (prev_node->next != NULL) { // Should not be NULL if not tail
         prev_node->next->prev = new_node;
    }
    prev_node->next = new_node;
    list->size++;
    return new_node;
}

xsan_list_node_t *xsan_list_insert_before(xsan_list_t *list, xsan_list_node_t *next_node, void *value) {
    if (!list || !next_node) {
        return NULL; // Invalid parameters
    }
    // If next_node is the head, it's equivalent to prepend
    if (next_node == list->head) {
        return xsan_list_prepend(list, value);
    }

    xsan_list_node_t *new_node = create_new_node(value);
    if (!new_node) {
        return NULL; // Node creation failed
    }

    new_node->prev = next_node->prev;
    new_node->next = next_node;
    if (next_node->prev != NULL) { // Should not be NULL if not head
        next_node->prev->next = new_node;
    }
    next_node->prev = new_node;
    list->size++;
    return new_node;
}

xsan_error_t xsan_list_remove_node(xsan_list_t *list, xsan_list_node_t *node) {
    if (!list || !node) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    // Ensure the node is actually part of this list by checking its linkage
    // This is a basic check. A more robust check would involve iterating, but is costly.
    // If node->prev is NULL, it must be list->head. If node->next is NULL, it must be list->tail.
    // If both are NULL, list size must be 1.
    bool is_head = (node == list->head);
    bool is_tail = (node == list->tail);

    if (node->prev) {
        node->prev->next = node->next;
    } else if (!is_head) {
        // Node has no prev, but it's not the head -> inconsistent state or not from this list
        // This case should ideally not happen if node is from the list and not head.
        // For simplicity, we'll proceed assuming node is valid if passed.
        // return XSAN_ERROR_NOT_FOUND; // Or a more specific "node not in list"
    }

    if (node->next) {
        node->next->prev = node->prev;
    } else if (!is_tail) {
        // Node has no next, but it's not the tail
        // return XSAN_ERROR_NOT_FOUND;
    }

    if (is_head) {
        list->head = node->next;
    }
    if (is_tail) {
        list->tail = node->prev;
    }

    if (list->value_destroy_func && node->value) {
        list->value_destroy_func(node->value);
    }
    XSAN_FREE(node);
    list->size--;

    // If list becomes empty, ensure head and tail are NULL
    if (list->size == 0) {
        list->head = NULL;
        list->tail = NULL;
    }
    return XSAN_OK;
}

void *xsan_list_pop_front(xsan_list_t *list) {
    if (!list || !list->head) {
        return NULL;
    }
    xsan_list_node_t *head_node = list->head;
    void *value = head_node->value;

    list->head = head_node->next;
    if (list->head) {
        list->head->prev = NULL;
    } else { // List became empty
        list->tail = NULL;
    }
    XSAN_FREE(head_node);
    list->size--;
    return value;
}

void *xsan_list_pop_back(xsan_list_t *list) {
    if (!list || !list->tail) {
        return NULL;
    }
    xsan_list_node_t *tail_node = list->tail;
    void *value = tail_node->value;

    list->tail = tail_node->prev;
    if (list->tail) {
        list->tail->next = NULL;
    } else { // List became empty
        list->head = NULL;
    }
    XSAN_FREE(tail_node);
    list->size--;
    return value;
}

xsan_list_node_t *xsan_list_get_head(xsan_list_t *list) {
    return list ? list->head : NULL;
}

xsan_list_node_t *xsan_list_get_tail(xsan_list_t *list) {
    return list ? list->tail : NULL;
}

xsan_list_node_t *xsan_list_node_next(xsan_list_node_t *node) {
    return node ? node->next : NULL;
}

xsan_list_node_t *xsan_list_node_prev(xsan_list_node_t *node) {
    return node ? node->prev : NULL;
}

void *xsan_list_node_get_value(xsan_list_node_t *node) {
    return node ? node->value : NULL;
}

size_t xsan_list_size(xsan_list_t *list) {
    return list ? list->size : 0;
}

bool xsan_list_is_empty(xsan_list_t *list) {
    return (list == NULL || list->size == 0);
}

void xsan_list_clear(xsan_list_t *list) {
    if (!list) {
        return;
    }
    xsan_list_node_t *current_node = list->head;
    while (current_node != NULL) {
        xsan_list_node_t *next_node = current_node->next;
        if (list->value_destroy_func && current_node->value) {
            list->value_destroy_func(current_node->value);
        }
        XSAN_FREE(current_node);
        current_node = next_node;
    }
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
}
