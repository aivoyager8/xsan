#ifndef XSAN_TYPES_H
#define XSAN_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* UUID support (optional) */
#ifdef HAVE_UUID
#include <uuid/uuid.h>
typedef struct {
    uuid_t uuid;
} xsan_uuid_t;
#else
/* Fallback UUID implementation */
typedef struct {
    uint8_t data[16];
} xsan_uuid_t;
#endif

/* Maximum limits */
#define XSAN_MAX_NODES 64
#define XSAN_MAX_DISKS_PER_NODE 32
#define XSAN_MAX_VMS_PER_NODE 256
#define XSAN_MAX_PATH_LEN 4096
#define XSAN_MAX_NAME_LEN 256
#define XSAN_BLOCK_SIZE 4096
#define XSAN_DEFAULT_REPLICAS 2

/* Error codes - use the detailed ones from xsan_error.h */
typedef enum {
    XSAN_OK = 0,
    XSAN_ERROR_GENERIC = -1,
    XSAN_ERROR_INVALID_PARAM = -2,
    XSAN_ERROR_OUT_OF_MEMORY = -3,
    XSAN_ERROR_IO = -4,
    XSAN_ERROR_NETWORK = -5,
    XSAN_ERROR_NOT_FOUND = -6,
    XSAN_ERROR_TIMEOUT = -7,
    XSAN_ERROR_CLUSTER = -8,
    XSAN_ERROR_STORAGE = -9,
    XSAN_ERROR_REPLICATION = -10
} xsan_error_t;

/* Node states */
typedef enum {
    XSAN_NODE_STATE_UNKNOWN = 0,
    XSAN_NODE_STATE_INITIALIZING,
    XSAN_NODE_STATE_ACTIVE,
    XSAN_NODE_STATE_MAINTENANCE,
    XSAN_NODE_STATE_FAILED,
    XSAN_NODE_STATE_DECOMMISSIONED
} xsan_node_state_t;

/* Storage device types */
typedef enum {
    XSAN_DEVICE_TYPE_UNKNOWN = 0,
    XSAN_DEVICE_TYPE_HDD,
    XSAN_DEVICE_TYPE_SSD,
    XSAN_DEVICE_TYPE_NVME
} xsan_device_type_t;

/* Storage policy types */
typedef enum {
    XSAN_POLICY_RAID_1 = 1,  /* Mirror */
    XSAN_POLICY_RAID_5 = 5,  /* Striped with parity */
    XSAN_POLICY_RAID_6 = 6   /* Striped with double parity */
} xsan_policy_type_t;

/* Performance tiers */
typedef enum {
    XSAN_TIER_UNKNOWN = 0,
    XSAN_TIER_PERFORMANCE,   /* All SSD/NVMe */
    XSAN_TIER_BALANCED,      /* SSD cache + HDD capacity */
    XSAN_TIER_CAPACITY       /* All HDD */
} xsan_tier_t;

/* Network address */
typedef struct {
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
} xsan_address_t;

/* Storage device information */
typedef struct {
    xsan_uuid_t id;
    char path[XSAN_MAX_PATH_LEN];
    char serial[XSAN_MAX_NAME_LEN];
    xsan_device_type_t type;
    uint64_t size_bytes;
    uint64_t free_bytes;
    uint32_t iops_capability;
    bool is_cache_device;
    bool is_healthy;
    pthread_mutex_t lock;
} xsan_device_t;

/* Cluster node information */
typedef struct {
    xsan_uuid_t id;
    char hostname[XSAN_MAX_NAME_LEN];
    xsan_address_t mgmt_addr;
    xsan_address_t storage_addr;
    xsan_node_state_t state;
    uint32_t device_count;
    xsan_device_t devices[XSAN_MAX_DISKS_PER_NODE];
    uint64_t total_capacity;
    uint64_t free_capacity;
    uint32_t cpu_cores;
    uint64_t memory_gb;
    time_t last_heartbeat;
    pthread_mutex_t lock;
} xsan_node_t;

/* Storage policy definition */
typedef struct {
    xsan_uuid_t id;
    char name[XSAN_MAX_NAME_LEN];
    xsan_policy_type_t policy_type;
    uint32_t replica_count;
    uint32_t stripe_width;
    xsan_tier_t preferred_tier;
    uint32_t iops_limit;
    uint32_t bandwidth_limit_mbps;
    bool allow_mixed_tiers;
    bool encryption_enabled;
    bool compression_enabled;
    pthread_mutex_t lock;
} xsan_storage_policy_t;

/* Virtual disk information */
typedef struct {
    xsan_uuid_t id;
    xsan_uuid_t vm_id;
    char name[XSAN_MAX_NAME_LEN];
    uint64_t size_bytes;
    xsan_uuid_t policy_id;
    uint32_t block_count;
    void *block_map;  /* Block allocation metadata */
    bool is_thin_provisioned;
    pthread_rwlock_t lock;
} xsan_vdisk_t;

/* Block metadata */
typedef struct {
    xsan_uuid_t block_id;
    uint64_t offset;
    uint32_t size;
    xsan_uuid_t primary_node;
    xsan_uuid_t replica_nodes[XSAN_MAX_NODES];
    uint32_t replica_count;
    uint64_t checksum;
    time_t last_modified;
} xsan_block_metadata_t;

/* Cluster configuration */
typedef struct {
    xsan_uuid_t cluster_id;
    char cluster_name[XSAN_MAX_NAME_LEN];
    uint32_t node_count;
    xsan_node_t nodes[XSAN_MAX_NODES];
    xsan_uuid_t master_node;
    uint32_t policy_count;
    xsan_storage_policy_t *policies;
    pthread_rwlock_t lock;
} xsan_cluster_t;

/* Function callback types */
typedef xsan_error_t (*xsan_node_event_cb_t)(xsan_uuid_t node_id, xsan_node_state_t old_state, xsan_node_state_t new_state, void *user_data);
typedef xsan_error_t (*xsan_storage_event_cb_t)(xsan_uuid_t device_id, bool device_failed, void *user_data);

#endif /* XSAN_TYPES_H */
