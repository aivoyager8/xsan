#ifndef XSAN_NODE_COMM_H
#define XSAN_NODE_COMM_H

#include "xsan_types.h"    // For xsan_error_t
#include "xsan_protocol.h" // For xsan_message_t
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> // For size_t

// Forward declaration for SPDK socket types
struct spdk_sock;
struct spdk_sock_group; // May not be needed in public header if managed internally

#ifdef __cplusplus
extern "C" {
#endif

// --- Callback Type Definitions ---

/**
 * @brief Callback invoked when a new message is fully received from a peer.
 *
 * @param sock The SPDK socket on which the message was received. This can be used as a handle
 *             to identify the peer connection if needed for replies or further interaction.
 * @param peer_addr_str String representation of the peer's address (e.g., "ip:port").
 *                      The lifetime of this string is typically tied to the callback invocation.
 * @param msg Pointer to the fully assembled and deserialized xsan_message_t.
 *            The callee (this callback) is responsible for calling xsan_protocol_message_destroy(msg)
 *            when it's done processing the message.
 * @param handler_cb_arg User-provided context argument originally passed to xsan_node_comm_init.
 */
typedef void (*xsan_node_message_handler_cb_t)(struct spdk_sock *sock,
                                               const char *peer_addr_str,
                                               xsan_message_t *msg,
                                               void *handler_cb_arg);

/**
 * @brief Callback for a specific message type, used with the registration mechanism.
 * @param conn_ctx Context of the connection from which the message was received.
 * @param msg The fully assembled message. Callee is responsible for destroying it.
 * @param specific_cb_arg User-provided argument during handler registration.
 */
typedef void (*xsan_specific_message_handler_cb_t)(struct xsan_connection_ctx *conn_ctx, // Pass conn_ctx
                                                   xsan_message_t *msg,
                                                   void *specific_cb_arg);

/**
 * @brief Callback invoked when a connection attempt (initiated by xsan_node_comm_connect) completes.
 *
 * @param sock The SPDK socket for the connection. This is valid and usable if status is 0 (success).
 *             If status is non-zero, sock may be NULL or invalid and should not be used.
 * @param status 0 on successful connection, or a negative errno value on failure.
 * @param connect_cb_arg User-provided context argument originally passed to xsan_node_comm_connect.
 */
typedef void (*xsan_node_connect_cb_t)(struct spdk_sock *sock, int status, void *connect_cb_arg);

/**
 * @brief Callback invoked after a send operation (initiated by xsan_node_comm_send_msg) completes.
 * This indicates that the data has been passed to the socket's send buffer or an error occurred.
 *
 * @param status 0 on success (all data written to socket send buffer), or a negative errno on failure.
 * @param send_cb_arg User-provided context argument originally passed to xsan_node_comm_send_msg.
 */
typedef void (*xsan_node_send_cb_t)(int status, void *send_cb_arg);


// --- Module Initialization and Teardown ---

/**
 * @brief Initializes the XSAN Node Communication module.
 * This function must be called from an SPDK reactor thread after SPDK has been initialized
 * (e.g., after xsan_spdk_manager_start_app calls its main callback).
 * It sets up the necessary SPDK socket infrastructure, including creating a socket group
 * and a poller for processing socket events on the current SPDK reactor.
 * If listen_ip and listen_port are provided, it also creates a listening socket.
 *
 * @param listen_ip IP address (e.g., "0.0.0.0") to listen on for incoming connections.
 *                  If NULL, the node will not listen for incoming connections (client-only mode).
 * @param listen_port Port number to listen on. Ignored if listen_ip is NULL.
 * @param msg_handler_cb Callback function to process fully received messages from any connection.
 *                       Must not be NULL if listen_ip is provided or if connections are expected.
 * @param handler_cb_arg User context argument for the msg_handler_cb.
 * @return XSAN_OK on success, or an xsan_error_t code on failure.
 */
xsan_error_t xsan_node_comm_init(const char *listen_ip, uint16_t listen_port,
                                 xsan_node_message_handler_cb_t msg_handler_cb, // This can be a generic dispatcher
                                 void *handler_cb_arg);

/**
 * @brief Registers a handler for a specific XSAN message type.
 * If a handler is already registered for this type, it will be overwritten.
 *
 * @param type The message type to register a handler for.
 * @param specific_handler The callback function to handle this message type.
 * @param specific_cb_arg User-defined argument to be passed to the specific_handler.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM if type is invalid or handler is NULL,
 *         XSAN_ERROR_NOT_INITIALIZED if the comm module is not initialized.
 */
xsan_error_t xsan_node_comm_register_message_handler(xsan_message_type_t type,
                                                     xsan_specific_message_handler_cb_t specific_handler,
                                                     void *specific_cb_arg);

/**
 * @brief Finalizes and cleans up the XSAN Node Communication module.
 * Closes all active connections, the listening socket (if any), and frees associated SPDK resources.
 * Must be called from an SPDK reactor thread during application shutdown.
 */
void xsan_node_comm_fini(void);


// --- Client Operations ---

/**
 * @brief Asynchronously connects to a remote XSAN node.
 * The result of the connection attempt (success or failure) is reported via the `connect_cb`.
 * This function must be called from an SPDK reactor thread.
 *
 * @param target_ip IP address string of the remote node to connect to.
 * @param target_port Port number of the remote node.
 * @param connect_cb Callback function to be invoked upon connection completion or failure. Must not be NULL.
 * @param cb_arg User context argument for the `connect_cb`.
 * @return XSAN_OK if the connection attempt was successfully initiated.
 *         An xsan_error_t code on immediate failure (e.g., invalid parameters, SPDK socket creation error).
 */
xsan_error_t xsan_node_comm_connect(const char *target_ip, uint16_t target_port,
                                    xsan_node_connect_cb_t connect_cb, void *cb_arg);


// --- Data Transfer Operations ---

/**
 * @brief Asynchronously sends an xsan_message_t to a connected peer.
 * The message (header and payload) will be serialized and sent over the socket.
 * The `xsan_message_t` structure itself (passed as `msg`) is NOT freed by this function;
 * the caller retains ownership and should decide when to free it, typically after the `send_cb`
 * indicates completion or based on the `send_cb` status.
 * This function must be called from an SPDK reactor thread.
 *
 * @param sock The SPDK socket representing the connection to the peer. Must be a valid, connected socket.
 * @param msg Pointer to the `xsan_message_t` to send. The header should be correctly populated
 *            (e.g., with type, payload_length, transaction_id). The checksum will be calculated
 *            by this function before sending if not already set.
 * @param send_cb Callback to notify upon send completion or failure. Can be NULL if a "fire-and-forget"
 *                approach is acceptable (not generally recommended for reliable messaging).
 * @param cb_arg User context argument for the `send_cb`.
 * @return XSAN_OK if the message was successfully queued for sending by SPDK.
 *         An xsan_error_t code on immediate failure (e.g., invalid parameters, socket error, serialization error).
 */
xsan_error_t xsan_node_comm_send_msg(struct spdk_sock *sock, xsan_message_t *msg,
                                     xsan_node_send_cb_t send_cb, void *cb_arg);

/**
 * @brief Closes a specific connection represented by an SPDK socket.
 * This should be called when a connection is no longer needed or if an error occurs
 * that requires closing the connection.
 *
 * @param sock The SPDK socket to close. The socket will be set to NULL after closing.
 *             If sock or *sock is NULL, the function does nothing.
 */
void xsan_node_comm_disconnect(struct spdk_sock **sock_ptr);


#ifdef __cplusplus
}
#endif

#endif // XSAN_NODE_COMM_H
