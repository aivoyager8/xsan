#include "xsan_node_comm.h"
#include "xsan_protocol.h"
#include "xsan_memory.h"
#include "xsan_error.h"
#include "xsan_log.h"
#include "xsan_string_utils.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/net.h"
#include "spdk/sock.h"
#include "spdk/thread.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

// --- Module Globals and Internal Structures ---

#define XSAN_COMM_MAX_PEER_ADDR_LEN 64
#define XSAN_COMM_DEFAULT_LISTEN_BACKLOG 128
#define XSAN_COMM_SOCK_POLL_INTERVAL_US (1000) // Increased for less aggressive polling if not expecting high freq events
#define XSAN_COMM_INITIAL_RECV_BUF_SIZE (XSAN_MESSAGE_HEADER_SIZE + 4096) // Initial buffer for header + some payload

// Context for an active connection (either server-accepted or client-initiated and connected)
typedef struct xsan_connection_ctx {
    struct spdk_sock *sock;
    char peer_addr_str[XSAN_COMM_MAX_PEER_ADDR_LEN];

    unsigned char *recv_buf;
    size_t recv_buf_capacity;
    size_t recv_buf_len; // Current amount of data in recv_buf
    xsan_message_header_t partial_recv_header;
    bool header_fully_received;

    // Generic fallback message handler for this connection (usually the global one)
    xsan_node_message_handler_cb_t app_msg_handler_cb;
    void *app_msg_handler_cb_arg;

    // For an ongoing send operation
    xsan_node_send_cb_t current_send_cb;
    void* current_send_cb_arg;

    struct xsan_connection_ctx *next;
    struct xsan_connection_ctx *prev;
} xsan_connection_ctx_t;

// Context for an outstanding client connect operation
typedef struct xsan_pending_connect_op {
    xsan_node_connect_cb_t user_cb;
    void *user_cb_arg;
    struct spdk_sock *sock_in_progress;
    char target_addr_str_for_log[XSAN_COMM_MAX_PEER_ADDR_LEN];
} xsan_pending_connect_op_t;


static struct {
    struct spdk_sock_group *sock_group_on_reactor;
    struct spdk_poller *sock_group_poller;
    struct spdk_sock *listener_sock;

    char module_listen_ip[INET6_ADDRSTRLEN];
    uint16_t module_listen_port;

    xsan_node_message_handler_cb_t global_app_msg_handler_cb; // Generic fallback if no specific handler
    void *global_app_msg_handler_cb_arg;

    xsan_connection_ctx_t *active_connections_head;
    pthread_mutex_t active_connections_lock; // Protects active_connections_head list

    bool module_initialized;

    xsan_specific_message_handler_cb_t specific_handlers[XSAN_MSG_TYPE_MAX];
    void *specific_handler_args[XSAN_MSG_TYPE_MAX];
} g_node_comm_ctx;


static int _xsan_comm_poller_fn(void *arg);
static void _xsan_comm_sock_event_callback(void *cb_arg, struct spdk_sock_group *group, struct spdk_sock *sock);
static void _process_received_data_for_connection(xsan_connection_ctx_t *conn_ctx);
static void _add_connection_to_active_list(xsan_connection_ctx_t *conn_ctx);
static void _remove_connection_from_active_list(xsan_connection_ctx_t *conn_ctx);
static void _cleanup_and_free_connection_ctx(xsan_connection_ctx_t *conn_ctx, bool close_spdk_sock_if_needed);
static xsan_connection_ctx_t* _create_connection_ctx(struct spdk_sock *sock,
                                                     xsan_node_message_handler_cb_t handler_cb, // Generic handler
                                                     void *handler_cb_arg);
static void _format_peer_addr(struct spdk_sock *sock, char *buf, size_t len);

#ifndef XSAN_ERROR_SPDK_ENV
#define XSAN_ERROR_SPDK_ENV XSAN_ERROR_SYSTEM
#endif
#ifndef XSAN_ERROR_INVALID_STATE
#define XSAN_ERROR_INVALID_STATE XSAN_ERROR_GENERIC
#endif


xsan_error_t xsan_node_comm_init(const char *listen_ip, uint16_t listen_port,
                                 xsan_node_message_handler_cb_t msg_handler_cb, // Generic fallback handler
                                 void *handler_cb_arg) {
    if (g_node_comm_ctx.module_initialized) { XSAN_LOG_WARN("XSAN Comm module already init."); return XSAN_OK; }
    if (spdk_get_thread() == NULL) { XSAN_LOG_ERROR("xsan_node_comm_init must be from SPDK thread."); return XSAN_ERROR_THREAD_CONTEXT; }

    XSAN_LOG_INFO("Initializing XSAN Node Comm module...");
    memset(&g_node_comm_ctx, 0, sizeof(g_node_comm_ctx));
    if (pthread_mutex_init(&g_node_comm_ctx.active_connections_lock, NULL) != 0) { XSAN_LOG_FATAL("Mutex init failed."); return XSAN_ERROR_SYSTEM; }

    uint32_t current_reactor = spdk_env_get_current_core();
    g_node_comm_ctx.sock_group_on_reactor = spdk_net_framework_get_sock_group(current_reactor);
    if (!g_node_comm_ctx.sock_group_on_reactor) { XSAN_LOG_FATAL("Failed to get SPDK sock group for reactor %u.", current_reactor); pthread_mutex_destroy(&g_node_comm_ctx.active_connections_lock); return XSAN_ERROR_SPDK_ENV; }

    g_node_comm_ctx.global_app_msg_handler_cb = msg_handler_cb;
    g_node_comm_ctx.global_app_msg_handler_cb_arg = handler_cb_arg;

    if (listen_ip && listen_port > 0) {
        struct spdk_sock_opts opts; memset(&opts, 0, sizeof(opts));
        opts.opts_size = sizeof(struct spdk_sock_opts);
        g_node_comm_ctx.listener_sock = spdk_sock_listen_ext(listen_ip, (int)listen_port, NULL, &opts);
        if (!g_node_comm_ctx.listener_sock) { XSAN_LOG_ERROR("Failed SPDK listen on %s:%u. Errno: %d (%s)", listen_ip, listen_port, errno, strerror(errno)); pthread_mutex_destroy(&g_node_comm_ctx.active_connections_lock); return XSAN_ERROR_NETWORK; }
        xsan_strcpy_safe(g_node_comm_ctx.module_listen_ip, listen_ip, INET6_ADDRSTRLEN);
        g_node_comm_ctx.module_listen_port = listen_port;
        XSAN_LOG_INFO("SPDK Listening socket created on %s:%u", listen_ip, listen_port);
    }

    g_node_comm_ctx.sock_group_poller = SPDK_POLLER_REGISTER(_xsan_comm_poller_fn, NULL, XSAN_COMM_SOCK_POLL_INTERVAL_US);
    if (!g_node_comm_ctx.sock_group_poller) {
        XSAN_LOG_FATAL("Failed to register SPDK sock group poller.");
        if (g_node_comm_ctx.listener_sock) { struct spdk_sock *tmp = g_node_comm_ctx.listener_sock; spdk_sock_close(&tmp); g_node_comm_ctx.listener_sock = NULL; }
        pthread_mutex_destroy(&g_node_comm_ctx.active_connections_lock);
        return XSAN_ERROR_SPDK_ENV;
    }
    g_node_comm_ctx.module_initialized = true;
    XSAN_LOG_INFO("XSAN Node Comm module initialized.");
    return XSAN_OK;
}

xsan_error_t xsan_node_comm_register_message_handler(xsan_message_type_t type,
                                                     xsan_specific_message_handler_cb_t specific_handler,
                                                     void *specific_cb_arg) {
    if (!g_node_comm_ctx.module_initialized) {
        XSAN_LOG_ERROR("Cannot register handler, XSAN Comm module not initialized.");
        return XSAN_ERROR_NOT_INITIALIZED;
    }
    if (type <= XSAN_MSG_TYPE_UNDEFINED || type >= XSAN_MSG_TYPE_MAX) {
        XSAN_LOG_ERROR("Invalid message type %u for handler registration.", type);
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (!specific_handler) {
        XSAN_LOG_WARN("Unregistering handler for message type %u (NULL handler provided).", type);
    }
    g_node_comm_ctx.specific_handlers[type] = specific_handler;
    g_node_comm_ctx.specific_handler_args[type] = specific_cb_arg;
    XSAN_LOG_INFO("Message handler %s for type %u.",
                  specific_handler ? "registered" : "unregistered", type);
    return XSAN_OK;
}

void xsan_node_comm_fini(void) {
    if (!g_node_comm_ctx.module_initialized) return;
    XSAN_LOG_INFO("Finalizing XSAN Node Comm module...");
    if (g_node_comm_ctx.sock_group_poller) { spdk_poller_unregister(&g_node_comm_ctx.sock_group_poller); g_node_comm_ctx.sock_group_poller = NULL; }
    pthread_mutex_lock(&g_node_comm_ctx.active_connections_lock);
    xsan_connection_ctx_t *conn_ctx = g_node_comm_ctx.active_connections_head;
    xsan_connection_ctx_t *next_conn_ctx;
    while (conn_ctx) {
        next_conn_ctx = conn_ctx->next;
        _cleanup_and_free_connection_ctx(conn_ctx, true);
        conn_ctx = next_conn_ctx;
    }
    g_node_comm_ctx.active_connections_head = NULL;
    pthread_mutex_unlock(&g_node_comm_ctx.active_connections_lock);
    if (g_node_comm_ctx.listener_sock) { struct spdk_sock *tmp = g_node_comm_ctx.listener_sock; spdk_sock_close(&tmp); g_node_comm_ctx.listener_sock = NULL; }
    g_node_comm_ctx.sock_group_on_reactor = NULL;
    pthread_mutex_destroy(&g_node_comm_ctx.active_connections_lock);
    g_node_comm_ctx.module_initialized = false;
    XSAN_LOG_INFO("XSAN Node Comm module finalized.");
}

static int _xsan_comm_poller_fn(void *arg) {
    (void)arg;
    if (g_node_comm_ctx.sock_group_on_reactor) {
        int events = spdk_sock_group_poll(g_node_comm_ctx.sock_group_on_reactor);
        return events > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
    }
    return SPDK_POLLER_IDLE;
}

static void _format_peer_addr(struct spdk_sock *sock, char *buf, size_t len) {
    if (!sock || !buf || len == 0) { if (buf && len > 0) buf[0] = '\0'; return; }
    char host[INET6_ADDRSTRLEN]; uint16_t port;
    if (spdk_sock_get_peer_addr(sock, host, sizeof(host), &port) == 0) {
        if (strchr(host, ':')) xsan_snprintf_safe(buf, len, "[%s]:%u", host, port);
        else xsan_snprintf_safe(buf, len, "%s:%u", host, port);
    } else xsan_snprintf_safe(buf, len, "unknown_peer");
}

static xsan_connection_ctx_t* _create_connection_ctx(struct spdk_sock *sock,
                                                     xsan_node_message_handler_cb_t handler_cb,
                                                     void *handler_cb_arg) {
    if (!sock) return NULL;
    xsan_connection_ctx_t *conn_ctx = (xsan_connection_ctx_t *)XSAN_MALLOC(sizeof(xsan_connection_ctx_t));
    if (!conn_ctx) { XSAN_LOG_ERROR("Failed to MALLOC conn_ctx."); return NULL; }
    memset(conn_ctx, 0, sizeof(xsan_connection_ctx_t));
    conn_ctx->sock = sock;
    _format_peer_addr(sock, conn_ctx->peer_addr_str, XSAN_COMM_MAX_PEER_ADDR_LEN);
    conn_ctx->recv_buf_capacity = XSAN_COMM_INITIAL_RECV_BUF_SIZE;
    conn_ctx->recv_buf = (unsigned char *)XSAN_MALLOC(conn_ctx->recv_buf_capacity);
    if (!conn_ctx->recv_buf) { XSAN_LOG_ERROR("Failed to MALLOC recv_buf for %s.", conn_ctx->peer_addr_str); XSAN_FREE(conn_ctx); return NULL; }
    conn_ctx->app_msg_handler_cb = handler_cb;
    conn_ctx->app_msg_handler_cb_arg = handler_cb_arg;
    // DO NOT call spdk_sock_set_cb_arg here. It's done by the caller (_xsan_comm_sock_event_callback for server,
    // or _xsan_client_sock_event_callback after successful connect for client)
    XSAN_LOG_INFO("Created connection context for peer: %s (sock %p)", conn_ctx->peer_addr_str, sock);
    return conn_ctx;
}

static void _add_connection_to_active_list(xsan_connection_ctx_t *conn_ctx) {
    if (!conn_ctx) return;
    pthread_mutex_lock(&g_node_comm_ctx.active_connections_lock);
    conn_ctx->prev = NULL; conn_ctx->next = g_node_comm_ctx.active_connections_head;
    if (g_node_comm_ctx.active_connections_head) g_node_comm_ctx.active_connections_head->prev = conn_ctx;
    g_node_comm_ctx.active_connections_head = conn_ctx;
    pthread_mutex_unlock(&g_node_comm_ctx.active_connections_lock);
    XSAN_LOG_DEBUG("Added connection %s to active list.", conn_ctx->peer_addr_str);
}

static void _remove_connection_from_active_list(xsan_connection_ctx_t *conn_ctx_to_remove) {
    if (!conn_ctx_to_remove) return;
    pthread_mutex_lock(&g_node_comm_ctx.active_connections_lock);
    if (conn_ctx_to_remove->prev) conn_ctx_to_remove->prev->next = conn_ctx_to_remove->next;
    else g_node_comm_ctx.active_connections_head = conn_ctx_to_remove->next;
    if (conn_ctx_to_remove->next) conn_ctx_to_remove->next->prev = conn_ctx_to_remove->prev;
    pthread_mutex_unlock(&g_node_comm_ctx.active_connections_lock);
    XSAN_LOG_DEBUG("Removed connection %s from active list.", conn_ctx_to_remove->peer_addr_str);
}

static void _cleanup_and_free_connection_ctx(xsan_connection_ctx_t *conn_ctx, bool close_spdk_sock_if_needed) {
    if (!conn_ctx) return;
    XSAN_LOG_INFO("Cleaning up conn_ctx for peer %s (sock %p).", conn_ctx->peer_addr_str, conn_ctx->sock);
    if (close_spdk_sock_if_needed && conn_ctx->sock) {
        struct spdk_sock *tmp_sock_ptr = conn_ctx->sock;
        XSAN_LOG_DEBUG("Calling spdk_sock_close for %s.", conn_ctx->peer_addr_str);
        spdk_sock_close(&tmp_sock_ptr); // tmp_sock_ptr will be NULLed by spdk_sock_close
        conn_ctx->sock = NULL;
    }
    if (conn_ctx->recv_buf) { XSAN_FREE(conn_ctx->recv_buf); conn_ctx->recv_buf = NULL; }
    XSAN_FREE(conn_ctx);
}

// Generic socket event callback for established connections (server-side accepted, or client-side after connect)
static void _xsan_comm_sock_event_callback(void *cb_arg, struct spdk_sock_group *group, struct spdk_sock *sock) {
    (void)group;
    xsan_connection_ctx_t *conn_ctx = (xsan_connection_ctx_t *)cb_arg; // This is now always xsan_connection_ctx_t

    if (sock == g_node_comm_ctx.listener_sock) {
        XSAN_LOG_INFO("Incoming connection on listener socket.");
        struct spdk_sock *new_data_sock = spdk_sock_accept(g_node_comm_ctx.listener_sock);
        if (new_data_sock) {
            xsan_connection_ctx_t *new_conn_ctx = _create_connection_ctx(new_data_sock,
                                                                       g_node_comm_ctx.global_app_msg_handler_cb,
                                                                       g_node_comm_ctx.global_app_msg_handler_cb_arg);
            if (new_conn_ctx) {
                _add_connection_to_active_list(new_conn_ctx);
            } else {
                XSAN_LOG_ERROR("Failed to create conn_ctx for accepted socket. Closing.");
                spdk_sock_close(&new_data_sock);
            }
        } else { XSAN_LOG_ERROR("spdk_sock_accept failed. Errno: %d (%s)", errno, strerror(errno)); }
        return;
    }

    if (!conn_ctx) {
        XSAN_LOG_ERROR("Socket event for non-listener sock %p with NULL conn_ctx! This is a bug.", sock);
        return;
    }

    XSAN_LOG_DEBUG("Socket event on %s (sock %p)", conn_ctx->peer_addr_str, sock);
    // 事件处理逻辑改为在 poller中处理 socket 状态
}

static void _process_received_data_for_connection(xsan_connection_ctx_t *conn_ctx) {
    if (!conn_ctx || !conn_ctx->sock) return;
    XSAN_LOG_DEBUG("Processing received data for %s (sock %p)", conn_ctx->peer_addr_str, conn_ctx->sock);

    ssize_t nbytes;
    do {
        if (conn_ctx->recv_buf_len == conn_ctx->recv_buf_capacity) {
            size_t new_capacity = conn_ctx->recv_buf_capacity > 0 ? conn_ctx->recv_buf_capacity * 2 : XSAN_COMM_INITIAL_RECV_BUF_SIZE;
            if (new_capacity > (XSAN_PROTOCOL_MAX_PAYLOAD_SIZE + XSAN_MESSAGE_HEADER_SIZE + 4096)) {
                 XSAN_LOG_ERROR("Receive buffer for %s reached excessive size %zu. Closing.", conn_ctx->peer_addr_str, new_capacity);
                 goto close_conn_error_proc_recv;
            }
            unsigned char *new_buf = (unsigned char *)XSAN_REALLOC(conn_ctx->recv_buf, new_capacity);
            if (!new_buf) { XSAN_LOG_ERROR("Failed to realloc recv_buf for %s. Closing.", conn_ctx->peer_addr_str); goto close_conn_error_proc_recv; }
            conn_ctx->recv_buf = new_buf; conn_ctx->recv_buf_capacity = new_capacity;
        }

        nbytes = spdk_sock_recv(conn_ctx->sock,
                                conn_ctx->recv_buf + conn_ctx->recv_buf_len,
                                conn_ctx->recv_buf_capacity - conn_ctx->recv_buf_len);

        if (nbytes > 0) {
            XSAN_LOG_TRACE("Read %zd bytes from %s", nbytes, conn_ctx->peer_addr_str);
            conn_ctx->recv_buf_len += nbytes;

            while(conn_ctx->recv_buf_len > 0) {
                if (!conn_ctx->header_fully_received) {
                    if (conn_ctx->recv_buf_len >= XSAN_MESSAGE_HEADER_SIZE) {
                        xsan_error_t err_hdr = xsan_protocol_header_deserialize(conn_ctx->recv_buf, &conn_ctx->partial_recv_header);
                        if (err_hdr != XSAN_OK) { XSAN_LOG_ERROR("Header deserialize failed from %s: %s. Closing.", conn_ctx->peer_addr_str, xsan_error_string(err_hdr)); goto close_conn_error_proc_recv;}
                        if (conn_ctx->partial_recv_header.magic != XSAN_PROTOCOL_MAGIC) { XSAN_LOG_ERROR("Bad magic 0x%x from %s. Closing.", conn_ctx->partial_recv_header.magic, conn_ctx->peer_addr_str); goto close_conn_error_proc_recv;}
                        if (conn_ctx->partial_recv_header.payload_length > XSAN_PROTOCOL_MAX_PAYLOAD_SIZE) { XSAN_LOG_ERROR("Payload %u too large from %s. Closing.", conn_ctx->partial_recv_header.payload_length, conn_ctx->peer_addr_str); goto close_conn_error_proc_recv;}

                        conn_ctx->header_fully_received = true;
                        memmove(conn_ctx->recv_buf, conn_ctx->recv_buf + XSAN_MESSAGE_HEADER_SIZE, conn_ctx->recv_buf_len - XSAN_MESSAGE_HEADER_SIZE);
                        conn_ctx->recv_buf_len -= XSAN_MESSAGE_HEADER_SIZE;
                        XSAN_LOG_TRACE("Header from %s. Type: %u, PayloadLen: %u", conn_ctx->peer_addr_str, conn_ctx->partial_recv_header.type, conn_ctx->partial_recv_header.payload_length);
                    } else break;
                }

                if (conn_ctx->header_fully_received) {
                    if (conn_ctx->recv_buf_len >= conn_ctx->partial_recv_header.payload_length) {
                        xsan_message_t *full_msg = (xsan_message_t*)XSAN_MALLOC(sizeof(xsan_message_t));
                        if (!full_msg) { XSAN_LOG_ERROR("OOM for xsan_message_t from %s. Closing.", conn_ctx->peer_addr_str); goto close_conn_error_proc_recv; }

                        memcpy(&full_msg->header, &conn_ctx->partial_recv_header, sizeof(xsan_message_header_t));
                        if (full_msg->header.payload_length > 0) {
                            full_msg->payload = (unsigned char *)XSAN_MALLOC(full_msg->header.payload_length);
                            if (!full_msg->payload) { XSAN_LOG_ERROR("OOM for payload from %s. Closing.", conn_ctx->peer_addr_str); XSAN_FREE(full_msg); goto close_conn_error_proc_recv; }
                            memcpy(full_msg->payload, conn_ctx->recv_buf, full_msg->header.payload_length);
                        } else full_msg->payload = NULL;

                        XSAN_LOG_DEBUG("Full msg (Type: %u, TID: %lu) from %s. Dispatching...",
                                       full_msg->header.type, full_msg->header.transaction_id, conn_ctx->peer_addr_str);

                        xsan_message_type_t msg_type = (xsan_message_type_t)full_msg->header.type;
                        if (msg_type > XSAN_MSG_TYPE_UNDEFINED && msg_type < XSAN_MSG_TYPE_MAX &&
                            g_node_comm_ctx.specific_handlers[msg_type] != NULL) {
                            g_node_comm_ctx.specific_handlers[msg_type](conn_ctx, full_msg, g_node_comm_ctx.specific_handler_args[msg_type]);
                        } else if (conn_ctx->app_msg_handler_cb) {
                            XSAN_LOG_WARN("No specific handler for msg type %u from %s. Using generic handler.", msg_type, conn_ctx->peer_addr_str);
                            conn_ctx->app_msg_handler_cb(conn_ctx->sock, conn_ctx->peer_addr_str, full_msg, conn_ctx->app_msg_handler_cb_arg);
                        } else {
                            XSAN_LOG_ERROR("No specific or generic handler for msg type %u from %s. Discarding.", msg_type, conn_ctx->peer_addr_str);
                            xsan_protocol_message_destroy(full_msg);
                        }

                        memmove(conn_ctx->recv_buf, conn_ctx->recv_buf + full_msg->header.payload_length, conn_ctx->recv_buf_len - full_msg->header.payload_length);
                        conn_ctx->recv_buf_len -= full_msg->header.payload_length;
                        conn_ctx->header_fully_received = false;
                    } else break;
                }
            }
        } else if (nbytes == 0) {
            XSAN_LOG_INFO("Connection %s closed by peer (recv returned 0).", conn_ctx->peer_addr_str);
            _remove_connection_from_active_list(conn_ctx);
            _cleanup_and_free_connection_ctx(conn_ctx, false);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { /* No more data now */ }
            else { XSAN_LOG_ERROR("spdk_sock_recv error on %s: %d (%s). Closing.", conn_ctx->peer_addr_str, errno, strerror(errno)); goto close_conn_error_proc_recv; }
            return;
        }
    } while (nbytes > 0 && conn_ctx->sock);
    return;

close_conn_error_proc_recv:
    _remove_connection_from_active_list(conn_ctx);
    _cleanup_and_free_connection_ctx(conn_ctx, true);
}

xsan_error_t xsan_node_comm_connect(const char *target_ip, uint16_t target_port,
                                    xsan_node_connect_cb_t connect_cb, void *cb_arg) {
    if (!g_node_comm_ctx.module_initialized) {
        XSAN_LOG_ERROR("Node comm module not initialized for connect.");
        return XSAN_ERROR_INVALID_STATE;
    }
    if (!target_ip || target_port == 0 || !connect_cb) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("xsan_node_comm_connect must be called from an SPDK thread.");
        return XSAN_ERROR_THREAD_CONTEXT;
    }

    XSAN_LOG_INFO("Attempting to connect to %s:%u", target_ip, target_port);

    xsan_pending_connect_op_t *pending_op =
        (xsan_pending_connect_op_t *)XSAN_MALLOC(sizeof(xsan_pending_connect_op_t));
    if (!pending_op) {
        XSAN_LOG_ERROR("Failed to allocate context for pending connect operation.");
        return XSAN_ERROR_OUT_OF_MEMORY;
    }
    pending_op->user_cb = connect_cb;
    pending_op->user_cb_arg = cb_arg;
    xsan_snprintf_safe(pending_op->target_addr_str_for_log, XSAN_COMM_MAX_PEER_ADDR_LEN, "%s:%u", target_ip, target_port);

    struct spdk_sock_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.opts_size = sizeof(struct spdk_sock_opts); // 必须设置
    // 其他参数可按需设置，如 opts.priority/opts.zcopy 等
    pending_op->sock_in_progress = spdk_sock_connect_ext(target_ip, (int)target_port, NULL, &opts);
    if (!pending_op->sock_in_progress) {
        int err_no = errno;
        XSAN_LOG_ERROR("spdk_sock_connect call failed immediately for %s. Errno: %d (%s)",
                       pending_op->target_addr_str_for_log, err_no, strerror(err_no));
        XSAN_FREE(pending_op);
        connect_cb(NULL, -err_no, cb_arg);
        return xsan_error_from_errno(err_no);
    }

    XSAN_LOG_DEBUG("SPDK connect initiated for %s, client sock %p, cb_arg (pending_op) %p",
                   pending_op->target_addr_str_for_log, pending_op->sock_in_progress, pending_op);
    return XSAN_OK;
}

xsan_error_t xsan_node_comm_send_msg(struct spdk_sock *sock, xsan_message_t *msg,
                                     xsan_node_send_cb_t send_cb, void *cb_arg) {
    if (!g_node_comm_ctx.module_initialized) return XSAN_ERROR_INVALID_STATE;
    if (!sock || !msg) return XSAN_ERROR_INVALID_PARAM;
    if (spdk_get_thread() == NULL) { XSAN_LOG_ERROR("send_msg must be from SPDK thread."); return XSAN_ERROR_THREAD_CONTEXT; }

    xsan_connection_ctx_t *conn_ctx = (xsan_connection_ctx_t *)spdk_sock_get_cb_arg(sock);
    if (!conn_ctx || conn_ctx->sock != sock) {
        XSAN_LOG_ERROR("No valid XSAN connection context found for sock %p during send_msg.", sock);
        return XSAN_ERROR_INVALID_PARAM;
    }

    if (conn_ctx->current_send_cb) {
        XSAN_LOG_WARN("Send already in progress for connection %s. Request rejected.", conn_ctx->peer_addr_str);
        return XSAN_ERROR_RESOURCE_BUSY;
    }

    unsigned char header_buf[XSAN_MESSAGE_HEADER_SIZE];
    if (xsan_protocol_header_serialize(&msg->header, header_buf) != XSAN_OK) {
        XSAN_LOG_ERROR("Failed to serialize msg header for sending to %s.", conn_ctx->peer_addr_str);
        return XSAN_ERROR_PROTOCOL_GENERIC;
    }

    struct iovec iov[2];
    int iovcnt = 1;
    iov[0].iov_base = header_buf;
    iov[0].iov_len = XSAN_MESSAGE_HEADER_SIZE;

    if (msg->header.payload_length > 0 && msg->payload) {
        iov[1].iov_base = msg->payload;
        iov[1].iov_len = msg->header.payload_length;
        iovcnt = 2;
    }

    size_t total_to_send = iov[0].iov_len + (iovcnt > 1 ? iov[1].iov_len : 0);
    XSAN_LOG_DEBUG("Attempting to send msg type %u (total %zu bytes) to %s (sock %p)",
                   msg->header.type, total_to_send, conn_ctx->peer_addr_str, sock);

    ssize_t bytes_written = spdk_sock_writev(sock, iov, iovcnt);

    if (bytes_written < 0) {
        int err_no = errno;
        XSAN_LOG_ERROR("spdk_sock_writev failed for %s: %d (%s)", conn_ctx->peer_addr_str, err_no, strerror(err_no));
        if (send_cb) send_cb(-err_no, cb_arg);
        return xsan_error_from_errno(err_no);
    }

    if ((size_t)bytes_written < total_to_send) {
        XSAN_LOG_WARN("Partial write to %s: %zd sent, %zu total. Full async send logic with queueing needed for this case.",
                      conn_ctx->peer_addr_str, bytes_written, total_to_send);
        conn_ctx->current_send_cb = send_cb;
        conn_ctx->current_send_cb_arg = cb_arg;
        return XSAN_OK;
    }

    XSAN_LOG_DEBUG("Successfully wrote all %zd bytes to %s.", bytes_written, conn_ctx->peer_addr_str);
    if (send_cb) {
        send_cb(0, cb_arg);
    }
    return XSAN_OK;
}

void xsan_node_comm_disconnect(struct spdk_sock **sock_ptr) {
    if (!sock_ptr || !*sock_ptr) return;
    XSAN_LOG_INFO("Disconnecting socket %p", *sock_ptr);

    pthread_mutex_lock(&g_node_comm_ctx.active_connections_lock);
    xsan_connection_ctx_t *conn_ctx = NULL;
    xsan_connection_ctx_t *iter = g_node_comm_ctx.active_connections_head;
    while(iter) {
        if (iter->sock == *sock_ptr) {
            conn_ctx = iter;
            break;
        }
        iter = iter->next;
    }
    if(conn_ctx) {
        _remove_connection_from_active_list(conn_ctx);
    }
    pthread_mutex_unlock(&g_node_comm_ctx.active_connections_lock);

    if(conn_ctx) {
        _cleanup_and_free_connection_ctx(conn_ctx, true);
    } else {
        XSAN_LOG_WARN("Disconnecting socket %p that had no XSAN connection context. Closing directly.", *sock_ptr);
        spdk_sock_close(sock_ptr);
    }
    if (sock_ptr) *sock_ptr = NULL;
}
