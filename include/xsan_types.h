
#ifndef XSAN_TYPES_H
#define XSAN_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

// 前置声明
struct xsan_address;
typedef struct xsan_address xsan_address_t;

struct xsan_protocol_header;
typedef struct xsan_protocol_header xsan_protocol_header_t;

typedef struct xsan_replica_location xsan_replica_location_t;
typedef enum xsan_storage_state xsan_storage_state_t;
typedef struct xsan_volume xsan_volume_t;

typedef struct {
    uint8_t data[16];
} xsan_uuid_t;

typedef xsan_uuid_t xsan_node_id_t;
typedef xsan_uuid_t xsan_disk_id_t;
typedef xsan_uuid_t xsan_group_id_t;
typedef xsan_uuid_t xsan_volume_id_t;

#define XSAN_MAX_SEED_NODES 16
#define XSAN_MAX_NODES 8
#define XSAN_MAX_NAME_LEN 256
#define XSAN_MAX_PATH_LEN 4096

typedef void (*xsan_user_io_completion_cb_t)(void *ctx, int status);

#endif /* XSAN_TYPES_H */
