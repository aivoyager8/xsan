// 协议实现
#include "xsan_protocol.h"
#include "xsan_memory.h" // For XSAN_MALLOC, XSAN_FREE, XSAN_CALLOC
#include "../../include/xsan_error.h" // 统一错误码头文件
// For error codes and XSAN_ERROR_PROTOCOL_MAGIC_MISMATCH etc.
#include <string.h>      // For memcpy, memset
#include <arpa/inet.h>   // For htons, htonl, ntohs, ntohl

// htonll and ntohll are not standard, so implement them manually if needed.
// Most modern systems might have them (e.g. from glibc if _BSD_SOURCE or _GNU_SOURCE is defined)
// For portability, a manual implementation is safer.
#ifndef htonll
// Define htonll if not available (e.g., on some systems or with strict POSIX)
// This checks host endianness.
#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t local_htonll(uint64_t val) {
    return ((uint64_t)htonl((uint32_t)(val & 0xFFFFFFFFULL)) << 32) | (uint64_t)htonl((uint32_t)(val >> 32));
}
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t local_htonll(uint64_t val) {
    return val;
}
#else
#error "Could not determine byte order for htonll/ntohll"
#endif
#define htonll local_htonll
#define ntohll local_htonll // ntohll is the same as htonll for swapping bytes
#endif


void xsan_protocol_header_init(xsan_message_header_t *header,
                               xsan_message_type_t type,
                               uint32_t payload_length,
                               uint64_t transaction_id) {
    if (!header) return;

    header->magic = XSAN_PROTOCOL_MAGIC;
    header->version = XSAN_PROTOCOL_VERSION;
    header->type = (uint16_t)type;
    header->payload_length = payload_length;
    header->transaction_id = transaction_id;
    header->checksum = 0; // Checksum should be calculated and set separately
}

xsan_error_t xsan_protocol_header_serialize(const xsan_message_header_t *header, unsigned char *buffer) {
    if (!header || !buffer) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    uint32_t magic_n = htonl(header->magic);
    uint16_t type_n = htons(header->type);
    uint16_t version_n = htons(header->version);
    uint32_t payload_length_n = htonl(header->payload_length);
    uint64_t transaction_id_n = htonll(header->transaction_id);
    uint32_t checksum_n = htonl(header->checksum);

    unsigned char *ptr = buffer;
    memcpy(ptr, &magic_n, sizeof(magic_n));
    ptr += sizeof(magic_n);
    memcpy(ptr, &type_n, sizeof(type_n));
    ptr += sizeof(type_n);
    memcpy(ptr, &version_n, sizeof(version_n));
    ptr += sizeof(version_n);
    memcpy(ptr, &payload_length_n, sizeof(payload_length_n));
    ptr += sizeof(payload_length_n);
    memcpy(ptr, &transaction_id_n, sizeof(transaction_id_n));
    ptr += sizeof(transaction_id_n);
    memcpy(ptr, &checksum_n, sizeof(checksum_n));

    return XSAN_OK;
}

xsan_error_t xsan_protocol_header_deserialize(const unsigned char *buffer, xsan_message_header_t *header) {
    if (!buffer || !header) {
        return XSAN_ERROR_INVALID_PARAM;
    }

    const unsigned char *ptr = buffer;
    uint32_t magic_n;
    uint16_t type_n;
    uint16_t version_n;
    uint32_t payload_length_n;
    uint64_t transaction_id_n;
    uint32_t checksum_n;

    memcpy(&magic_n, ptr, sizeof(magic_n));
    ptr += sizeof(magic_n);
    header->magic = ntohl(magic_n);

    if (header->magic != XSAN_PROTOCOL_MAGIC) {
        return XSAN_ERROR_INVALID_PARAM; // 协议魔数不匹配
    }

    memcpy(&type_n, ptr, sizeof(type_n));
    ptr += sizeof(type_n);
    header->type = ntohs(type_n);

    memcpy(&version_n, ptr, sizeof(version_n));
    ptr += sizeof(version_n);
    header->version = ntohs(version_n);

    if (header->version > XSAN_PROTOCOL_VERSION) {
        // Allow processing of same or older versions if designed for backward compatibility
        // For now, strict check: only current version is supported.
        // return XSAN_ERROR_PROTOCOL_VERSION_UNSUPPORTED;
    }

    memcpy(&payload_length_n, ptr, sizeof(payload_length_n));
    ptr += sizeof(payload_length_n);
    header->payload_length = ntohl(payload_length_n);

    if (header->payload_length > XSAN_PROTOCOL_MAX_PAYLOAD_SIZE) {
        return XSAN_ERROR_INVALID_PARAM; // 协议负载过大
    }

    memcpy(&transaction_id_n, ptr, sizeof(transaction_id_n));
    ptr += sizeof(transaction_id_n);
    header->transaction_id = ntohll(transaction_id_n);

    memcpy(&checksum_n, ptr, sizeof(checksum_n));
    header->checksum = ntohl(checksum_n);

    return XSAN_OK;
}

// Placeholder checksum: simple XOR sum. Replace with robust algorithm (e.g. CRC32).
uint32_t xsan_protocol_calculate_checksum(const unsigned char *data, size_t length) {
    if (!data || length == 0) {
        return 0;
    }
    uint32_t checksum = 0;
    // A common simple checksum is Fletcher-16 or Fletcher-32, or just sum of bytes.
    // For this placeholder, a simple XOR sum of all bytes.
    // This is NOT cryptographically secure or very robust against errors.
    for (size_t i = 0; i < length; ++i) {
        // A slightly better approach than pure XOR for illustration:
        checksum = (checksum + data[i]) & 0xFFFFFFFF; // Accumulate sum
    }
     // Could also incorporate length or a more complex mixing
    // For instance, a simple Internet Checksum like algorithm part:
    // uint32_t sum = 0;
    // const uint16_t *p = (const uint16_t *)data;
    // size_t nleft = length;
    // while (nleft > 1) {
    //     sum += *p++;
    //     nleft -= 2;
    // }
    // if (nleft == 1) {
    //     sum += *(const uint8_t *)p;
    // }
    // sum = (sum >> 16) + (sum & 0xffff);
    // sum += (sum >> 16);
    // checksum = ~sum & 0xffff; // For 16-bit checksum
    // For a 32-bit placeholder, the simple sum is okay for now.
    return checksum;
}

bool xsan_protocol_verify_checksum(const xsan_message_header_t *header, const unsigned char *payload) {
    if (!header) {
        return false; // Invalid parameter
    }
    // If checksum field in header is 0, assume checksum is not used/validated
    if (header->checksum == 0) {
        return true;
    }

    // To verify, we calculate checksum over the header (with its checksum field temporarily zeroed)
    // and the payload, then compare with the header's checksum field.
    xsan_message_header_t temp_header_for_calc = *header;
    temp_header_for_calc.checksum = 0; // Zero out checksum for calculation

    unsigned char serialized_header_for_calc[XSAN_MESSAGE_HEADER_SIZE];
    if (xsan_protocol_header_serialize(&temp_header_for_calc, serialized_header_for_calc) != XSAN_OK) {
        return false; // Should not happen if header is valid
    }

    // The checksum should cover all header fields (magic to transaction_id) and the payload.
    // The serialized_header_for_calc already has checksum field as 0.
    // We calculate checksum on the part of serialized_header_for_calc *before* the checksum field.
    uint32_t calculated_checksum;

    // Simpler: calculate checksum over (header_fields_before_checksum + payload)
    // This requires a temporary buffer to combine these parts if checksum func takes one buffer.
    // Or, update checksum iteratively.
    // For this placeholder, let's assume checksum is over (all header fields with checksum=0) + payload.
    // This means the checksum field in the header should be the last field.

    // Create a temporary buffer for the data to be checksummed
    size_t data_to_checksum_len = XSAN_MESSAGE_HEADER_SIZE + header->payload_length;
    unsigned char *checksum_data_buf = (unsigned char *)XSAN_MALLOC(data_to_checksum_len);
    if (!checksum_data_buf) {
        return false; // Out of memory
    }

    // Copy serialized header (with checksum field as 0) into the buffer
    memcpy(checksum_data_buf, serialized_header_for_calc, XSAN_MESSAGE_HEADER_SIZE);

    // Copy payload (if any) after the header data
    if (payload && header->payload_length > 0) {
        memcpy(checksum_data_buf + XSAN_MESSAGE_HEADER_SIZE, payload, header->payload_length);
    }

    calculated_checksum = xsan_protocol_calculate_checksum(checksum_data_buf, data_to_checksum_len);
    XSAN_FREE(checksum_data_buf);

    return calculated_checksum == header->checksum;
}


xsan_message_t *xsan_protocol_message_create(xsan_message_type_t type,
                                             uint64_t transaction_id,
                                             const void *payload_data,
                                             uint32_t payload_length) {
    if (payload_length > XSAN_PROTOCOL_MAX_PAYLOAD_SIZE) {
        return NULL; // Payload too large as per protocol definition
    }

    xsan_message_t *msg = (xsan_message_t *)XSAN_MALLOC(sizeof(xsan_message_t));
    if (!msg) {
        return NULL; // Out of memory for message struct
    }
    // Initialize header first (sets checksum to 0 initially)
    xsan_protocol_header_init(&msg->header, type, payload_length, transaction_id);

    if (payload_length > 0) {
        msg->payload = (unsigned char *)XSAN_MALLOC(payload_length);
        if (!msg->payload) {
            XSAN_FREE(msg);
            return NULL; // Out of memory for payload
        }
        if (payload_data) {
            memcpy(msg->payload, payload_data, payload_length);
        } else {
            // If payload_data is NULL but length > 0, it implies an empty (zeroed out) buffer.
            memset(msg->payload, 0, payload_length);
        }
    } else {
        msg->payload = NULL;
    }

    // Now calculate and set the checksum for the initialized header and payload
    // The header already has its checksum field as 0 due to xsan_protocol_header_init.
    unsigned char serialized_header_for_calc[XSAN_MESSAGE_HEADER_SIZE];
    if (xsan_protocol_header_serialize(&msg->header, serialized_header_for_calc) != XSAN_OK) {
        // This serialization is of the header with checksum=0 for calculation purposes.
        // If it fails, something is wrong, clean up.
        if(msg->payload) XSAN_FREE(msg->payload);
        XSAN_FREE(msg);
        return NULL;
    }

    size_t data_to_checksum_len = XSAN_MESSAGE_HEADER_SIZE + msg->header.payload_length;
    unsigned char *checksum_data_buf = (unsigned char *)XSAN_MALLOC(data_to_checksum_len);
    if (!checksum_data_buf) {
        if(msg->payload) XSAN_FREE(msg->payload);
        XSAN_FREE(msg);
        return NULL; // OOM for checksum calculation buffer
    }

    memcpy(checksum_data_buf, serialized_header_for_calc, XSAN_MESSAGE_HEADER_SIZE);
    if (msg->payload && msg->header.payload_length > 0) {
        memcpy(checksum_data_buf + XSAN_MESSAGE_HEADER_SIZE, msg->payload, msg->header.payload_length);
    }

    msg->header.checksum = xsan_protocol_calculate_checksum(checksum_data_buf, data_to_checksum_len);
    XSAN_FREE(checksum_data_buf);

    return msg;
}

void xsan_protocol_message_destroy(xsan_message_t *msg) {
    if (!msg) {
        return;
    }
    if (msg->payload) {
        XSAN_FREE(msg->payload);
    }
    XSAN_FREE(msg);
}

xsan_message_t *xsan_protocol_message_create_with_data(
    xsan_message_type_t type,
    uint64_t transaction_id,
    const void *structured_payload,
    uint32_t structured_payload_len,
    const void *additional_data,
    uint32_t additional_data_len) {

    uint64_t total_payload_length_64 = (uint64_t)structured_payload_len + additional_data_len;
    if (total_payload_length_64 > XSAN_PROTOCOL_MAX_PAYLOAD_SIZE) {
        // Log error: total payload too large
        return NULL;
    }
    uint32_t total_payload_length = (uint32_t)total_payload_length_64;

    xsan_message_t *msg = (xsan_message_t *)XSAN_MALLOC(sizeof(xsan_message_t));
    if (!msg) {
        return NULL;
    }

    xsan_protocol_header_init(&msg->header, type, total_payload_length, transaction_id);

    if (total_payload_length > 0) {
        msg->payload = (unsigned char *)XSAN_MALLOC(total_payload_length);
        if (!msg->payload) {
            XSAN_FREE(msg);
            return NULL;
        }

        unsigned char *ptr = msg->payload;
        if (structured_payload && structured_payload_len > 0) {
            memcpy(ptr, structured_payload, structured_payload_len);
            ptr += structured_payload_len;
        }
        if (additional_data && additional_data_len > 0) {
            memcpy(ptr, additional_data, additional_data_len);
        }
    } else {
        msg->payload = NULL;
    }

    // Calculate checksum
    unsigned char serialized_header_for_calc[XSAN_MESSAGE_HEADER_SIZE];
    if (xsan_protocol_header_serialize(&msg->header, serialized_header_for_calc) != XSAN_OK) {
        if(msg->payload) XSAN_FREE(msg->payload);
        XSAN_FREE(msg);
        return NULL;
    }

    size_t data_to_checksum_len = XSAN_MESSAGE_HEADER_SIZE + msg->header.payload_length;
    unsigned char *checksum_data_buf = (unsigned char *)XSAN_MALLOC(data_to_checksum_len);
    if (!checksum_data_buf) {
        if(msg->payload) XSAN_FREE(msg->payload);
        XSAN_FREE(msg);
        return NULL;
    }

    memcpy(checksum_data_buf, serialized_header_for_calc, XSAN_MESSAGE_HEADER_SIZE);
    if (msg->payload && msg->header.payload_length > 0) {
        memcpy(checksum_data_buf + XSAN_MESSAGE_HEADER_SIZE, msg->payload, msg->header.payload_length);
    }

    msg->header.checksum = xsan_protocol_calculate_checksum(checksum_data_buf, data_to_checksum_len);
    XSAN_FREE(checksum_data_buf);

    return msg;
}
