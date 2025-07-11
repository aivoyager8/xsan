#include "xsan_socket.h"
#include "xsan_error.h"
#include "xsan_string_utils.h" // For xsan_snprintf_safe
#include <errno.h>
#include <string.h> // For memset, strchr, strrchr, strlen
#include <stdio.h>  // For snprintf

// Helper to fill sockaddr_storage from ip_address and port
// Returns AF_INET, AF_INET6, or 0 on error
static int fill_sockaddr(const char *ip_address, uint16_t port, struct sockaddr_storage *ss, socklen_t *addr_len) {
    memset(ss, 0, sizeof(struct sockaddr_storage));

    // Try IPv6 first if address contains ':'
    if (ip_address && strchr(ip_address, ':')) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)ss;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        if (inet_pton(AF_INET6, ip_address, &addr6->sin6_addr) == 1) {
            *addr_len = sizeof(struct sockaddr_in6);
            return AF_INET6;
        }
        // If inet_pton for IPv6 fails, and it wasn't "::" or similar, it might be an error or IPv4
        // If ip_address is NULL, it's treated as any address (0.0.0.0 or ::)
    }

    // Try IPv4 or handle NULL ip_address for IPv4 any
    struct sockaddr_in *addr4 = (struct sockaddr_in *)ss;
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
    if (ip_address) {
        if (inet_pton(AF_INET, ip_address, &addr4->sin_addr) == 1) {
            *addr_len = sizeof(struct sockaddr_in);
            return AF_INET;
        }
    } else { // Default to INADDR_ANY if ip_address is NULL
        addr4->sin_addr.s_addr = htonl(INADDR_ANY);
        *addr_len = sizeof(struct sockaddr_in);
        return AF_INET;
    }

    // If ip_address was "::" and inet_pton for IPv4 failed, it must be IPv6 any
    if (ip_address && strcmp(ip_address, "::") == 0) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)ss;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        addr6->sin6_addr = in6addr_any;
        *addr_len = sizeof(struct sockaddr_in6);
        return AF_INET6;
    }


    return 0; // Invalid IP address format
}


int xsan_socket_create(xsan_socket_type_t type, xsan_socket_family_t family) {
    int sock_type_os = (type == XSAN_SOCKET_TYPE_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    int family_os = (family == XSAN_SOCKET_FAMILY_IPV4) ? AF_INET : AF_INET6;

    int sockfd = socket(family_os, sock_type_os, 0);
    if (sockfd < 0) {
        return XSAN_INVALID_SOCKET;
    }
    return sockfd;
}

xsan_error_t xsan_socket_close(int sockfd) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (close(sockfd) == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_shutdown(int sockfd, int how) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (shutdown(sockfd, how) == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_bind(int sockfd, const char *ip_address, uint16_t port) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    struct sockaddr_storage ss;
    socklen_t addr_len;

    // Determine socket family to correctly interpret NULL ip_address
    struct sockaddr current_addr;
    socklen_t len = sizeof(current_addr);
    if (getsockname(sockfd, &current_addr, &len) == -1) {
        return xsan_error_from_errno(errno);
    }

    const char* bind_ip = ip_address;
    if (!ip_address) { // If NULL, use OS default for "any" based on socket family
        bind_ip = (current_addr.sa_family == AF_INET6) ? "::" : "0.0.0.0";
    }


    if (fill_sockaddr(bind_ip, port, &ss, &addr_len) == 0) {
         // If ip_address was non-NULL and fill_sockaddr failed, it's an invalid IP format.
        if (ip_address) return XSAN_ERROR_INVALID_PARAM;
        // If ip_address was NULL, and we failed to determine default bind_ip (should not happen with current logic)
        // then try to force based on socket family.
        if (current_addr.sa_family == AF_INET6) {
            struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&ss;
            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = htons(port);
            addr6->sin6_addr = in6addr_any;
            addr_len = sizeof(struct sockaddr_in6);
        } else { // Default to IPv4
            struct sockaddr_in *addr4 = (struct sockaddr_in *)&ss;
            addr4->sin_family = AF_INET;
            addr4->sin_port = htons(port);
            addr4->sin_addr.s_addr = htonl(INADDR_ANY);
            addr_len = sizeof(struct sockaddr_in);
        }
    }


    if (bind(sockfd, (struct sockaddr *)&ss, addr_len) == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_listen(int sockfd, int backlog) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    if (listen(sockfd, backlog) == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

int xsan_socket_accept(int sockfd, xsan_address_t *client_addr) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_INVALID_SOCKET;
    }
    struct sockaddr_storage ss;
    socklen_t addr_len = sizeof(ss);
    int client_fd = accept(sockfd, (struct sockaddr *)&ss, &addr_len);

    if (client_fd < 0) {
        // errno is set by accept() (e.g., EAGAIN, EWOULDBLOCK for non-blocking)
        return XSAN_INVALID_SOCKET;
    }

    if (client_addr) {
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *s_in = (struct sockaddr_in *)&ss;
            inet_ntop(AF_INET, &s_in->sin_addr, client_addr->ip, INET_ADDRSTRLEN);
            client_addr->port = ntohs(s_in->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *s_in6 = (struct sockaddr_in6 *)&ss;
            inet_ntop(AF_INET6, &s_in6->sin6_addr, client_addr->ip, INET6_ADDRSTRLEN);
            client_addr->port = ntohs(s_in6->sin6_port);
        } else {
            client_addr->ip[0] = '\0';
            client_addr->port = 0;
        }
    }
    return client_fd;
}

xsan_error_t xsan_socket_connect(int sockfd, const char *remote_ip, uint16_t port) {
    if (sockfd == XSAN_INVALID_SOCKET || !remote_ip) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    struct sockaddr_storage ss;
    socklen_t addr_len;
    if (fill_sockaddr(remote_ip, port, &ss, &addr_len) == 0) {
        return XSAN_ERROR_INVALID_PARAM; // Invalid remote_ip format
    }

    if (connect(sockfd, (struct sockaddr *)&ss, addr_len) == -1) {
        if (errno == EINPROGRESS) {
            return XSAN_OK; // Non-blocking connect in progress
        }
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_send(int sockfd, const void *buffer, size_t length, ssize_t *bytes_sent) {
    if (sockfd == XSAN_INVALID_SOCKET || !buffer || !bytes_sent) {
        if(bytes_sent) *bytes_sent = -1;
        return XSAN_ERROR_INVALID_PARAM;
    }
    *bytes_sent = send(sockfd, buffer, length, 0); // Flags typically 0 for basic send
    if (*bytes_sent == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_receive(int sockfd, void *buffer, size_t length, ssize_t *bytes_received) {
    if (sockfd == XSAN_INVALID_SOCKET || !buffer || !bytes_received) {
        if(bytes_received) *bytes_received = -1;
        return XSAN_ERROR_INVALID_PARAM;
    }
    *bytes_received = recv(sockfd, buffer, length, 0); // Flags typically 0
    if (*bytes_received == -1) {
        return xsan_error_from_errno(errno);
    }
    if (*bytes_received == 0) { // For TCP, 0 means connection closed by peer
        // Need to check socket type if we want to differentiate UDP behavior (0-byte datagram is possible)
        struct sockaddr sa;
        socklen_t salen = sizeof(sa);
        int type;
        socklen_t typelen = sizeof(type);
        if (getsockopt(sockfd, SOL_SOCKET, SO_TYPE, &type, &typelen) == 0 && type == SOCK_STREAM) {
             return XSAN_ERROR_CONNECTION_LOST;
        }
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_sendto(int sockfd, const void *buffer, size_t length,
                                const char *dest_ip, uint16_t dest_port, ssize_t *bytes_sent) {
    if (sockfd == XSAN_INVALID_SOCKET || !buffer || !dest_ip || !bytes_sent) {
        if(bytes_sent) *bytes_sent = -1;
        return XSAN_ERROR_INVALID_PARAM;
    }
    struct sockaddr_storage ss;
    socklen_t addr_len;
    if (fill_sockaddr(dest_ip, dest_port, &ss, &addr_len) == 0) {
        *bytes_sent = -1;
        return XSAN_ERROR_INVALID_PARAM; // Invalid dest_ip format
    }

    *bytes_sent = sendto(sockfd, buffer, length, 0, (struct sockaddr *)&ss, addr_len);
    if (*bytes_sent == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_recvfrom(int sockfd, void *buffer, size_t length,
                                  xsan_address_t *sender_addr, ssize_t *bytes_received) {
    if (sockfd == XSAN_INVALID_SOCKET || !buffer || !bytes_received) {
        if(bytes_received) *bytes_received = -1;
        return XSAN_ERROR_INVALID_PARAM;
    }
    struct sockaddr_storage ss;
    socklen_t addr_len = sizeof(ss);

    *bytes_received = recvfrom(sockfd, buffer, length, 0, (struct sockaddr *)&ss, &addr_len);
    if (*bytes_received == -1) {
        return xsan_error_from_errno(errno);
    }

    if (sender_addr) {
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *s_in = (struct sockaddr_in *)&ss;
            inet_ntop(AF_INET, &s_in->sin_addr, sender_addr->ip, INET_ADDRSTRLEN);
            sender_addr->port = ntohs(s_in->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *s_in6 = (struct sockaddr_in6 *)&ss;
            inet_ntop(AF_INET6, &s_in6->sin6_addr, sender_addr->ip, INET6_ADDRSTRLEN);
            sender_addr->port = ntohs(s_in6->sin6_port);
        } else {
            sender_addr->ip[0] = '\0';
            sender_addr->port = 0;
        }
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_set_nonblocking(int sockfd) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        return xsan_error_from_errno(errno);
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_set_blocking(int sockfd) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        return xsan_error_from_errno(errno);
    }
    if (fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_set_reuseaddr(int sockfd, bool enable) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    int optval = enable ? 1 : 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_set_tcp_nodelay(int sockfd, bool enable) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    int optval = enable ? 1 : 0;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) == -1) {
        return xsan_error_from_errno(errno);
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_set_keepalive(int sockfd, bool enable,
                                       int idle_time_sec, int interval_sec, int count_probes) {
    if (sockfd == XSAN_INVALID_SOCKET) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    int optval = enable ? 1 : 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) == -1) {
        return xsan_error_from_errno(errno);
    }
    if (enable) {
        #ifdef TCP_KEEPIDLE
        if (idle_time_sec > 0) {
            if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_time_sec, sizeof(idle_time_sec)) == -1) {
                 return xsan_error_from_errno(errno);
            }
        }
        #endif
        #ifdef TCP_KEEPINTVL
        if (interval_sec > 0) {
            if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_sec, sizeof(interval_sec)) == -1) {
                 return xsan_error_from_errno(errno);
            }
        }
        #endif
        #ifdef TCP_KEEPCNT
        if (count_probes > 0) {
            if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &count_probes, sizeof(count_probes)) == -1) {
                 return xsan_error_from_errno(errno);
            }
        }
        #endif
    }
    return XSAN_OK;
}


xsan_error_t xsan_socket_get_local_address(int sockfd, xsan_address_t *local_addr) {
    if (sockfd == XSAN_INVALID_SOCKET || !local_addr) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    struct sockaddr_storage ss;
    socklen_t addr_len = sizeof(ss);
    if (getsockname(sockfd, (struct sockaddr *)&ss, &addr_len) == -1) {
        return xsan_error_from_errno(errno);
    }
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *s_in = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &s_in->sin_addr, local_addr->ip, INET_ADDRSTRLEN);
        local_addr->port = ntohs(s_in->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *s_in6 = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &s_in6->sin6_addr, local_addr->ip, INET6_ADDRSTRLEN);
        local_addr->port = ntohs(s_in6->sin6_port);
    } else {
        return XSAN_ERROR_NETWORK; // Unknown family
    }
    return XSAN_OK;
}

xsan_error_t xsan_socket_get_peer_address(int sockfd, xsan_address_t *peer_addr) {
    if (sockfd == XSAN_INVALID_SOCKET || !peer_addr) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    struct sockaddr_storage ss;
    socklen_t addr_len = sizeof(ss);
    if (getpeername(sockfd, (struct sockaddr *)&ss, &addr_len) == -1) {
        return xsan_error_from_errno(errno);
    }
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *s_in = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &s_in->sin_addr, peer_addr->ip, INET_ADDRSTRLEN);
        peer_addr->port = ntohs(s_in->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *s_in6 = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &s_in6->sin6_addr, peer_addr->ip, INET6_ADDRSTRLEN);
        peer_addr->port = ntohs(s_in6->sin6_port);
    } else {
        return XSAN_ERROR_NETWORK; // Unknown family
    }
    return XSAN_OK;
}

const char* xsan_socket_address_to_string(const xsan_address_t *addr, char *buffer, size_t buffer_size) {
    if (!addr || !buffer || buffer_size == 0) {
        return NULL;
    }
    // Check if IPv6 based on presence of ':'
    if (strchr(addr->ip, ':')) { // Likely IPv6
        xsan_snprintf_safe(buffer, buffer_size, "[%s]:%u", addr->ip, addr->port);
    } else { // Likely IPv4
        xsan_snprintf_safe(buffer, buffer_size, "%s:%u", addr->ip, addr->port);
    }
    // Ensure null termination if xsan_snprintf_safe doesn't guarantee it on overflow
    // (assuming xsan_snprintf_safe does, like standard snprintf)
    return buffer;
}

xsan_error_t xsan_socket_string_to_address(const char *address_string, xsan_address_t *addr) {
    if (!address_string || !addr) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    char ip_buffer[INET6_ADDRSTRLEN]; // Max length for IPv6
    const char *port_str = NULL;

    if (address_string[0] == '[') { // Likely IPv6 with port, e.g., "[::1]:1234"
        const char *closing_bracket = strchr(address_string, ']');
        if (!closing_bracket || *(closing_bracket + 1) != ':') {
            return XSAN_ERROR_INVALID_PARAM; // Malformed IPv6 address string
        }
        size_t ip_len = closing_bracket - (address_string + 1);
        if (ip_len >= INET6_ADDRSTRLEN) {
            return XSAN_ERROR_INVALID_PARAM; // IP part too long
        }
        memcpy(ip_buffer, address_string + 1, ip_len);
        ip_buffer[ip_len] = '\0';
        port_str = closing_bracket + 2;
    } else { // IPv4 or IPv6 without brackets (less common for address:port)
        port_str = strrchr(address_string, ':');
        if (port_str) { // IPv4 or IPv6 with port
            size_t ip_len = port_str - address_string;
            if (ip_len >= INET6_ADDRSTRLEN) { // Use INET6_ADDRSTRLEN as a general buffer limit
                return XSAN_ERROR_INVALID_PARAM;
            }
            memcpy(ip_buffer, address_string, ip_len);
            ip_buffer[ip_len] = '\0';
            port_str++; // Skip the ':'
        } else { // No port found, assume it's just an IP
            if (strlen(address_string) >= INET6_ADDRSTRLEN) {
                 return XSAN_ERROR_INVALID_PARAM;
            }
            strcpy(ip_buffer, address_string);
            // No port_str, port will remain 0 or be set by caller if this is not an error
        }
    }

    // Validate IP and copy to addr->ip
    struct sockaddr_storage ss_test; // Used for validation via inet_pton
    if (inet_pton(AF_INET, ip_buffer, &((struct sockaddr_in *)&ss_test)->sin_addr) == 1) {
        // Valid IPv4
        xsan_strcpy_safe(addr->ip, ip_buffer, INET_ADDRSTRLEN);
    } else if (inet_pton(AF_INET6, ip_buffer, &((struct sockaddr_in6 *)&ss_test)->sin6_addr) == 1) {
        // Valid IPv6
        xsan_strcpy_safe(addr->ip, ip_buffer, INET6_ADDRSTRLEN);
    } else {
        return XSAN_ERROR_INVALID_PARAM; // Invalid IP format
    }

    if (port_str && *port_str) {
        char *endptr;
        long port_val = strtol(port_str, &endptr, 10);
        if (*endptr != '\0' || port_val <= 0 || port_val > 65535) {
            return XSAN_ERROR_INVALID_PARAM; // Invalid port
        }
        addr->port = (uint16_t)port_val;
    } else {
        // If port_str was NULL or empty, it means no port was specified in the string.
        // Depending on requirements, this could be an error or default to 0.
        // For now, let's assume it's an error if a port was expected.
        // If just parsing an IP is allowed, then addr->port might be left as is or set to 0.
        // For this function, we'll assume port is usually part of the string.
        // If no port string, maybe it's an error unless a "default port" behavior is intended.
        return XSAN_ERROR_INVALID_PARAM; // No port found or invalid format
    }

    return XSAN_OK;
}
