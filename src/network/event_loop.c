#include "xsan_event_loop.h"
#include "xsan_memory.h" // For XSAN_MALLOC, XSAN_FREE, XSAN_CALLOC
#include "xsan_error.h"  // For error codes
#include <sys/epoll.h>   // For epoll functions
#include <unistd.h>      // For close()
#include <stdlib.h>      // For NULL
#include <string.h>      // For memset
#include <errno.h>       // For errno

// Internal structure to store callback and user_data per file descriptor.
// This simple implementation uses a fixed-size array to map fd to its data.
// A more scalable solution for large numbers of FDs or sparse FDs would use a hash table
// or a dynamically resizing array.
#define XSAN_EVENT_LOOP_DEFAULT_MAP_SIZE 1024 // Max FD value + 1 this map can handle directly

typedef struct {
    xsan_event_callback_t callback;
    void *user_data;
} xsan_fd_event_data_t;

struct xsan_event_loop {
    int epoll_fd;                   // File descriptor for the epoll instance
    int max_events_per_poll;        // Max events to fetch from epoll_wait at once
    struct epoll_event *epoll_events_buf; // Buffer for epoll_wait to store triggered events

    xsan_fd_event_data_t *fd_data_map; // Array to map fd to callback/user_data
    int fd_data_map_capacity;       // Current capacity of fd_data_map
};

// Helper to map xsan_event_type_t to epoll event flags
static uint32_t xsan_to_epoll_events(uint32_t xsan_events) {
    uint32_t ep_events = 0;
    if (xsan_events & XSAN_EVENT_TYPE_READ) ep_events |= EPOLLIN;
    if (xsan_events & XSAN_EVENT_TYPE_WRITE) ep_events |= EPOLLOUT;
    if (xsan_events & XSAN_EVENT_TYPE_ERROR) ep_events |= EPOLLERR; // Usually implicitly monitored
    if (xsan_events & XSAN_EVENT_TYPE_HANGUP) ep_events |= EPOLLHUP; // Usually implicitly monitored
    // Example for edge-triggered, if supported:
    // if (xsan_events & XSAN_EVENT_TYPE_EDGE_TRIGGERED) ep_events |= EPOLLET;
    return ep_events;
}

// Helper to map epoll event flags back to xsan_event_type_t
static uint32_t epoll_to_xsan_events(uint32_t ep_events) {
    uint32_t xsan_events = XSAN_EVENT_TYPE_NONE;
    if (ep_events & EPOLLIN) xsan_events |= XSAN_EVENT_TYPE_READ;
    if (ep_events & EPOLLOUT) xsan_events |= XSAN_EVENT_TYPE_WRITE;
    if (ep_events & EPOLLERR) xsan_events |= XSAN_EVENT_TYPE_ERROR;
    if (ep_events & EPOLLHUP) xsan_events |= XSAN_EVENT_TYPE_HANGUP;
    // Note: EPOLLRDHUP could also be mapped if needed.
    return xsan_events;
}

// Helper to ensure fd_data_map is large enough for a given fd.
// This is a simple resizing strategy. In a real system, more sophisticated growth might be used.
static xsan_error_t ensure_fd_map_capacity(xsan_event_loop_t *loop, int fd) {
    if (fd < loop->fd_data_map_capacity) {
        return XSAN_OK;
    }

    int new_capacity = loop->fd_data_map_capacity;
    if (new_capacity == 0) new_capacity = XSAN_EVENT_LOOP_DEFAULT_MAP_SIZE;
    while (fd >= new_capacity) {
        new_capacity *= 2;
    }

    // Check for excessively large FD values that might indicate an error or extreme case
    if (new_capacity > 1024 * 1024) { // Arbitrary sanity limit (e.g. ~1 million FDs)
        return XSAN_ERROR_OUT_OF_MEMORY; // Or a more specific "FD too large"
    }

    xsan_fd_event_data_t *new_map = (xsan_fd_event_data_t *)XSAN_CALLOC(new_capacity, sizeof(xsan_fd_event_data_t));
    if (!new_map) {
        return XSAN_ERROR_OUT_OF_MEMORY;
    }

    if (loop->fd_data_map) {
        memcpy(new_map, loop->fd_data_map, loop->fd_data_map_capacity * sizeof(xsan_fd_event_data_t));
        XSAN_FREE(loop->fd_data_map);
    }

    loop->fd_data_map = new_map;
    loop->fd_data_map_capacity = new_capacity;
    return XSAN_OK;
}


xsan_event_loop_t *xsan_event_loop_create(int max_events_per_poll) {
    if (max_events_per_poll <= 0) {
        return NULL;
    }

    xsan_event_loop_t *loop = (xsan_event_loop_t *)XSAN_MALLOC(sizeof(xsan_event_loop_t));
    if (!loop) {
        return NULL;
    }
    memset(loop, 0, sizeof(xsan_event_loop_t)); // Initialize all fields

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC); // Use CLOEXEC for safety
    if (loop->epoll_fd == -1) {
        XSAN_FREE(loop);
        return NULL;
    }

    loop->epoll_events_buf = (struct epoll_event *)XSAN_CALLOC(max_events_per_poll, sizeof(struct epoll_event));
    if (!loop->epoll_events_buf) {
        close(loop->epoll_fd);
        XSAN_FREE(loop);
        return NULL;
    }
    loop->max_events_per_poll = max_events_per_poll;

    // Initial fd_data_map (can be NULL initially and grown by ensure_fd_map_capacity)
    loop->fd_data_map = NULL;
    loop->fd_data_map_capacity = 0;
    // Pre-allocate a default size or let it grow on first add_fd
    if (ensure_fd_map_capacity(loop, XSAN_EVENT_LOOP_DEFAULT_MAP_SIZE -1) != XSAN_OK) {
         XSAN_FREE(loop->epoll_events_buf);
         close(loop->epoll_fd);
         XSAN_FREE(loop);
         return NULL;
    }

    return loop;
}

void xsan_event_loop_destroy(xsan_event_loop_t *loop) {
    if (!loop) {
        return;
    }
    if (loop->epoll_fd != -1) {
        close(loop->epoll_fd);
    }
    XSAN_FREE(loop->epoll_events_buf);
    XSAN_FREE(loop->fd_data_map);
    XSAN_FREE(loop);
}

xsan_error_t xsan_event_loop_add_fd(xsan_event_loop_t *loop, int fd, uint32_t events,
                                    xsan_event_callback_t callback, void *user_data) {
    if (!loop || fd < 0 || !callback) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    xsan_error_t err = ensure_fd_map_capacity(loop, fd);
    if (err != XSAN_OK) {
        return err;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev)); // Important for some epoll_ctl versions/setups
    ev.events = xsan_to_epoll_events(events);
    ev.data.fd = fd; // Store fd; user_data and callback are in our map

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        return xsan_error_from_errno(errno);
    }

    loop->fd_data_map[fd].callback = callback;
    loop->fd_data_map[fd].user_data = user_data;
    return XSAN_OK;
}

xsan_error_t xsan_event_loop_modify_fd(xsan_event_loop_t *loop, int fd, uint32_t events,
                                       xsan_event_callback_t callback, void *user_data) {
    if (!loop || fd < 0 || !callback) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    // ensure_fd_map_capacity should ideally not be needed if fd was already added,
    // but as a safety for cases where fd might be > initial map size and MOD is used.
    xsan_error_t err = ensure_fd_map_capacity(loop, fd);
    if (err != XSAN_OK) {
        // This implies fd was likely never added or is excessively large.
        // If it was added, map should already cover it.
        return err;
    }
    // If fd_data_map[fd].callback is NULL, it implies fd was not properly added before.
    // epoll_ctl MOD on a non-added FD will fail.
    if (loop->fd_data_map[fd].callback == NULL && fd < loop->fd_data_map_capacity) {
         // This suggests an attempt to modify an FD that wasn't added via our wrapper,
         // or was removed. For robustness, one might call ADD instead, or return error.
         // For now, let epoll_ctl handle it (it should fail with ENOENT).
    }


    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = xsan_to_epoll_events(events);
    ev.data.fd = fd;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        return xsan_error_from_errno(errno);
    }

    loop->fd_data_map[fd].callback = callback;
    loop->fd_data_map[fd].user_data = user_data;
    return XSAN_OK;
}

xsan_error_t xsan_event_loop_remove_fd(xsan_event_loop_t *loop, int fd) {
    if (!loop || fd < 0) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    // For EPOLL_CTL_DEL, the event struct passed to epoll_ctl can be NULL (on Linux >= 2.6.9)
    // or a pointer to a struct epoll_event, but its content is ignored.
    // We don't strictly need to pass ev.data.fd for DEL.
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        return xsan_error_from_errno(errno); // e.g., ENOENT if fd not in epoll set
    }

    // Clear our map entry if fd is within current map bounds
    if (fd < loop->fd_data_map_capacity) {
        loop->fd_data_map[fd].callback = NULL;
        loop->fd_data_map[fd].user_data = NULL;
    }
    return XSAN_OK;
}

int xsan_event_loop_poll(xsan_event_loop_t *loop, int timeout_ms) {
    if (!loop) {
        errno = EINVAL; // Set errno for caller if they check it after -1 return
        return -1;
    }

    int num_events = epoll_wait(loop->epoll_fd, loop->epoll_events_buf, loop->max_events_per_poll, timeout_ms);

    if (num_events < 0) {
        // EINTR is a common case where poll was interrupted by a signal, not a fatal error.
        // The caller might choose to retry. We return 0 to indicate no FDs processed due to this.
        if (errno == EINTR) {
            return 0;
        }
        return -1; // Other errors are reported as -1, errno is set by epoll_wait.
    }

    for (int i = 0; i < num_events; ++i) {
        int event_fd = loop->epoll_events_buf[i].data.fd;
        uint32_t ep_events = loop->epoll_events_buf[i].events;
        uint32_t xsan_events = epoll_to_xsan_events(ep_events);

        if (event_fd >= 0 && event_fd < loop->fd_data_map_capacity && loop->fd_data_map[event_fd].callback) {
            loop->fd_data_map[event_fd].callback(loop, event_fd, xsan_events, loop->fd_data_map[event_fd].user_data);
        }
        // If no callback is found (e.g., fd was removed concurrently or map issue),
        // the event for that fd is effectively ignored by this loop.
    }
    return num_events;
}
