#ifndef XSAN_EVENT_LOOP_H
#define XSAN_EVENT_LOOP_H

#include "xsan_types.h" // For xsan_error_t
#include <stdint.h>     // For uint32_t
#include <stdbool.h>    // For bool

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct xsan_event_loop xsan_event_loop_t;

/**
 * @brief Event types that can be monitored.
 * These correspond to epoll events and can be bitwise OR'd.
 */
typedef enum {
    XSAN_EVENT_TYPE_NONE = 0,      ///< No events
    XSAN_EVENT_TYPE_READ = 1,      ///< Data available for reading (EPOLLIN)
    XSAN_EVENT_TYPE_WRITE = 2,     ///< Ready for writing (EPOLLOUT)
    XSAN_EVENT_TYPE_ERROR = 4,     ///< Error condition occurred (EPOLLERR)
    XSAN_EVENT_TYPE_HANGUP = 8,    ///< Hang-up detected (EPOLLHUP)
    // XSAN_EVENT_TYPE_EDGE_TRIGGERED = 16 ///< Edge-triggered behavior (EPOLLET) - advanced
} xsan_event_type_t;

/**
 * @brief Callback function for when an event occurs on a file descriptor.
 *
 * @param loop The event loop instance.
 * @param fd The file descriptor that triggered the event.
 * @param events A bitmask of xsan_event_type_t that occurred.
 * @param user_data The user_data pointer provided when the fd was added/modified.
 */
typedef void (*xsan_event_callback_t)(xsan_event_loop_t *loop, int fd, uint32_t events, void *user_data);

/**
 * @brief Creates a new event loop.
 *
 * Internally, this uses epoll on Linux.
 *
 * @param max_events_per_poll The maximum number of events to retrieve in a single call to xsan_event_loop_poll.
 *                            This influences the size of an internal buffer. Must be > 0.
 * @return A pointer to the new event loop, or NULL on error (e.g., epoll_create1 failed, out of memory).
 */
xsan_event_loop_t *xsan_event_loop_create(int max_events_per_poll);

/**
 * @brief Destroys an event loop.
 *
 * This closes the internal epoll file descriptor. It does NOT close any file descriptors
 * that were registered with the loop; those must be managed externally.
 *
 * @param loop The event loop to destroy. If NULL, the function does nothing.
 */
void xsan_event_loop_destroy(xsan_event_loop_t *loop);

/**
 * @brief Adds a file descriptor to the event loop for monitoring.
 *
 * @param loop The event loop. Must not be NULL.
 * @param fd The file descriptor to monitor. Must be >= 0.
 * @param events A bitmask of xsan_event_type_t to monitor (e.g., XSAN_EVENT_TYPE_READ | XSAN_EVENT_TYPE_WRITE).
 * @param callback The callback function to invoke when an event occurs on this fd. Must not be NULL.
 * @param user_data A pointer to user-specific data to be passed to the callback. Can be NULL.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if loop, callback is NULL, or fd is invalid.
 *         XSAN_ERROR_OUT_OF_MEMORY if internal data structures cannot be allocated.
 *         Other XSAN error codes corresponding to epoll_ctl failures.
 */
xsan_error_t xsan_event_loop_add_fd(xsan_event_loop_t *loop, int fd, uint32_t events,
                                    xsan_event_callback_t callback, void *user_data);

/**
 * @brief Modifies the events being monitored for an already registered file descriptor.
 *
 * The callback and user_data for the fd can also be updated.
 *
 * @param loop The event loop. Must not be NULL.
 * @param fd The file descriptor to modify. Must be >= 0 and previously added.
 * @param events A new bitmask of xsan_event_type_t to monitor.
 * @param callback The new callback function. Must not be NULL.
 * @param user_data The new user_data pointer. Can be NULL.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if loop, callback is NULL, or fd is invalid.
 *         Other XSAN error codes corresponding to epoll_ctl failures.
 */
xsan_error_t xsan_event_loop_modify_fd(xsan_event_loop_t *loop, int fd, uint32_t events,
                                       xsan_event_callback_t callback, void *user_data);

/**
 * @brief Removes a file descriptor from event loop monitoring.
 *
 * @param loop The event loop. Must not be NULL.
 * @param fd The file descriptor to remove. Must be >= 0.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_INVALID_PARAM if loop is NULL or fd is invalid.
 *         XSAN_ERROR_NOT_FOUND if the fd was not found in epoll (e.g., already removed or never added).
 *         Other XSAN error codes corresponding to epoll_ctl failures.
 */
xsan_error_t xsan_event_loop_remove_fd(xsan_event_loop_t *loop, int fd);

/**
 * @brief Waits for and dispatches events.
 *
 * This function will block for up to 'timeout_ms' milliseconds waiting for events.
 * When events occur, the respective registered callback functions are invoked.
 *
 * @param loop The event loop. Must not be NULL.
 * @param timeout_ms The maximum time to block in milliseconds.
 *                   A value of -1 means block indefinitely.
 *                   A value of 0 means return immediately, even if no events are present (poll).
 * @return The number of file descriptors for which events were processed.
 *         Returns 0 if the timeout expired and no events occurred.
 *         Returns -1 on error (e.g., epoll_wait failed). If -1, check errno.
 *         Note: This return is from epoll_wait; XSAN error codes are not directly returned here.
 */
int xsan_event_loop_poll(xsan_event_loop_t *loop, int timeout_ms);

// Consider adding a xsan_event_loop_run(loop) that internally calls poll in a loop
// and a xsan_event_loop_stop(loop) to break that loop if needed for a typical run model.
// For now, poll provides the core blocking/waiting mechanism.

#ifdef __cplusplus
}
#endif

#endif // XSAN_EVENT_LOOP_H
