#ifndef XSAN_SOCKET_H
#define XSAN_SOCKET_H

#include "xsan_types.h" // For xsan_error_t, xsan_address_t
#include <stddef.h>     // For size_t
#include <stdbool.h>    // For bool
#include <sys/socket.h> // For socklen_t, struct sockaddr_storage, SHUT_RDWR
#include <netinet/in.h> // For struct sockaddr_in, sockaddr_in6, IPPROTO_TCP
#include <netinet/tcp.h> // For TCP_NODELAY
#include <arpa/inet.h>  // For inet_ntop, inet_pton
#include <unistd.h>     // For close
#include <fcntl.h>      // For fcntl, O_NONBLOCK

#ifdef __cplusplus
extern "C" {
#endif

#define XSAN_INVALID_SOCKET (-1)

// Socket type, primarily for creation
typedef enum {
    XSAN_SOCKET_TYPE_TCP,
    XSAN_SOCKET_TYPE_UDP
} xsan_socket_type_t;

// Socket address family, used internally for address resolution
typedef enum {
    XSAN_SOCKET_FAMILY_IPV4,
    XSAN_SOCKET_FAMILY_IPV6
    // XSAN_SOCKET_FAMILY_UNSPEC is not used for direct socket creation here,
    // but could be used for higher-level address resolution functions if added.
} xsan_socket_family_t;


/**
 * Creates a new socket.
 *
 * @param type The type of socket (TCP or UDP).
 * @param family The address family (IPv4 or IPv6).
 * @return The socket file descriptor, or XSAN_INVALID_SOCKET on error.
 *         Use xsan_error_last() to get a more specific XSAN error code based on errno.
 */
int xsan_socket_create(xsan_socket_type_t type, xsan_socket_family_t family);

/**
 * Closes a socket.
 *
 * @param sockfd The socket file descriptor to close.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_close(int sockfd);

/**
 * Shuts down socket send and receive operations.
 *
 * @param sockfd The socket file descriptor.
 * @param how Specifies the type of shutdown. Use SHUT_RD, SHUT_WR, or SHUT_RDWR.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_shutdown(int sockfd, int how);

/**
 * Binds a socket to a specific address and port.
 *
 * @param sockfd The socket file descriptor.
 * @param ip_address The IP address string to bind to (e.g., "0.0.0.0", "192.168.1.100", "::").
 *                   If NULL, it implies binding to "0.0.0.0" for IPv4 or "::" for IPv6,
 *                   depending on the socket's family.
 * @param port The port number.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_bind(int sockfd, const char *ip_address, uint16_t port);

/**
 * Puts a socket into listening mode (for TCP server sockets).
 *
 * @param sockfd The socket file descriptor.
 * @param backlog The maximum length of the queue of pending connections.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_listen(int sockfd, int backlog);

/**
 * Accepts an incoming connection on a listening socket (for TCP server sockets).
 *
 * @param sockfd The listening socket file descriptor.
 * @param client_addr Optional pointer to an xsan_address_t structure to store the client's address. Can be NULL.
 * @return The file descriptor for the accepted client socket, or XSAN_INVALID_SOCKET on error.
 *         If the socket is non-blocking and no pending connections are present,
 *         errno will be set to EAGAIN or EWOULDBLOCK.
 */
int xsan_socket_accept(int sockfd, xsan_address_t *client_addr);

/**
 * Connects a socket to a remote address and port (for TCP client sockets).
 *
 * @param sockfd The socket file descriptor.
 * @param remote_ip The IP address string of the remote host.
 * @param port The port number of the remote host.
 * @return XSAN_OK on successful immediate connection.
 *         If the socket is non-blocking, XSAN_OK is returned if connect would block (errno == EINPROGRESS).
 *         Returns an XSAN error code on immediate failure.
 */
xsan_error_t xsan_socket_connect(int sockfd, const char *remote_ip, uint16_t port);

/**
 * Sends data over a connected socket (TCP or connected UDP).
 *
 * @param sockfd The socket file descriptor.
 * @param buffer The data buffer to send.
 * @param length The number of bytes to send from the buffer.
 * @param bytes_sent Pointer to store the number of bytes actually sent. Must not be NULL.
 * @return XSAN_OK on success (even if partial send for non-blocking), or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_send(int sockfd, const void *buffer, size_t length, ssize_t *bytes_sent);

/**
 * Receives data from a connected socket (TCP or connected UDP).
 *
 * @param sockfd The socket file descriptor.
 * @param buffer The buffer to store received data.
 * @param length The maximum number of bytes to receive into the buffer.
 * @param bytes_received Pointer to store the number of bytes actually received. Must not be NULL.
 *                       For TCP, 0 indicates peer closed connection gracefully.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_CONNECTION_LOST if peer disconnected (for TCP, when recv returns 0).
 *         An XSAN error code on other failures.
 */
xsan_error_t xsan_socket_receive(int sockfd, void *buffer, size_t length, ssize_t *bytes_received);

/**
 * Sends data to a specific destination using a UDP socket.
 *
 * @param sockfd The UDP socket file descriptor.
 * @param buffer The data buffer to send.
 * @param length The number of bytes to send.
 * @param dest_ip The destination IP address string.
 * @param dest_port The destination port number.
 * @param bytes_sent Pointer to store the number of bytes actually sent. Must not be NULL.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_sendto(int sockfd, const void *buffer, size_t length,
                                const char *dest_ip, uint16_t dest_port, ssize_t *bytes_sent);

/**
 * Receives data from a UDP socket, also capturing the sender's address.
 *
 * @param sockfd The UDP socket file descriptor.
 * @param buffer The buffer to store received data.
 * @param length The maximum number of bytes to receive.
 * @param sender_addr Pointer to an xsan_address_t structure to store the sender's address. Can be NULL.
 * @param bytes_received Pointer to store the number of bytes actually received. Must not be NULL.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_recvfrom(int sockfd, void *buffer, size_t length,
                                  xsan_address_t *sender_addr, ssize_t *bytes_received);

/**
 * Sets a socket to non-blocking mode.
 *
 * @param sockfd The socket file descriptor.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_set_nonblocking(int sockfd);

/**
 * Sets a socket to blocking mode.
 *
 * @param sockfd The socket file descriptor.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_set_blocking(int sockfd);


/**
 * Sets the SO_REUSEADDR socket option.
 * Allows reuse of local addresses, useful for server restart.
 *
 * @param sockfd The socket file descriptor.
 * @param enable true to enable, false to disable.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_set_reuseaddr(int sockfd, bool enable);

/**
 * Sets the TCP_NODELAY socket option (disables Nagle's algorithm).
 *
 * @param sockfd The socket file descriptor (must be a TCP socket).
 * @param enable true to enable TCP_NODELAY (disable Nagle), false to disable.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_set_tcp_nodelay(int sockfd, bool enable);

/**
 * Sets the SO_KEEPALIVE socket option.
 *
 * @param sockfd The socket file descriptor (typically TCP).
 * @param enable true to enable, false to disable.
 * @param idle_time_sec Time (seconds) of inactivity before sending keepalive probes. (OS specific default if 0 or less)
 * @param interval_sec Time (seconds) between keepalive probes. (OS specific default if 0 or less)
 * @param count_probes Number of probes before dropping connection. (OS specific default if 0 or less)
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_set_keepalive(int sockfd, bool enable,
                                       int idle_time_sec, int interval_sec, int count_probes);

/**
 * Gets the local address and port the socket is bound to.
 *
 * @param sockfd The socket file descriptor.
 * @param local_addr Pointer to an xsan_address_t structure to store the local address. Must not be NULL.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_get_local_address(int sockfd, xsan_address_t *local_addr);

/**
 * Gets the peer (remote) address and port of a connected socket.
 *
 * @param sockfd The socket file descriptor.
 * @param peer_addr Pointer to an xsan_address_t structure to store the peer's address. Must not be NULL.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 */
xsan_error_t xsan_socket_get_peer_address(int sockfd, xsan_address_t *peer_addr);

/**
 * Converts an xsan_address_t to a human-readable string (e.g., "192.168.1.100:8080").
 * The provided xsan_address_t must have a valid IP string.
 *
 * @param addr The xsan_address_t structure. Must not be NULL.
 * @param buffer The buffer to store the string. Must not be NULL.
 * @param buffer_size The size of the buffer.
 * @return Pointer to the buffer on success, NULL on failure (e.g., buffer too small).
 */
const char* xsan_socket_address_to_string(const xsan_address_t *addr, char *buffer, size_t buffer_size);

/**
 * Parses a string like "192.168.1.100:8080" or "[ipv6_addr]:port" into xsan_address_t.
 * This is a simplified parser; robust parsing of IPv6 with port can be complex.
 * It assumes the IP part and port part are separated by the last colon for IPv4,
 * or for IPv6 it expects a format like "[::1]:1234".
 *
 * @param address_string The input string. Must not be NULL.
 * @param addr Pointer to xsan_address_t to store the result. Must not be NULL.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM on parsing failure.
 */
xsan_error_t xsan_socket_string_to_address(const char *address_string, xsan_address_t *addr);

#ifdef __cplusplus
}
#endif

#endif // XSAN_SOCKET_H
