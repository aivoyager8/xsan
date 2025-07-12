#ifndef XSAN_STORAGE_H
#define XSAN_STORAGE_H

#include "xsan_types.h" // For xsan_uuid_t, XSAN_MAX_NAME_LEN, XSAN_MAX_PATH_LEN (if needed)
#include "xsan_error.h" // For xsan_error_t
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Type Definitions for IDs ---
typedef xsan_uuid_t xsan_node_id_t;       // Unique identifier for a node in the cluster
typedef xsan_uuid_t xsan_disk_id_t;       // Using UUID for unique disk identification
typedef xsan_uuid_t xsan_group_id_t;      // Using UUID for unique disk group identification
typedef xsan_uuid_t xsan_volume_id_t;     // Using UUID for unique logical volume identification

// --- Constants ---
#define XSAN_MAX_REPLICAS 3               // Maximum number of replicas for a volume (e.g., FTT=2 -> 3 replicas)

// --- Structures for Replication ---
/**
 * @brief Describes the location and connection information for a volume replica.
 */
typedef struct {
    xsan_node_id_t node_id;                     ///< ID of the node hosting this replica
    char node_ip_addr[INET6_ADDRSTRLEN];        ///< IP address of the replica node for communication
    uint16_t node_comm_port;                    ///< Communication port on the replica node
    xsan_storage_state_t state;                 ///< Current state of this specific replica (e.g., ONLINE, OFFLINE, DEGRADED)
    uint64_t last_successful_contact_time_us;   ///< Timestamp (microseconds) of last known successful interaction
    // char target_on_replica[XSAN_MAX_NAME_LEN]; ///< Optional: Specific storage target name on replica node
} xsan_replica_location_t;


// --- Enumerations for Storage Entities ---

/**
 * @brief Defines the physical type of a storage disk.
 * This is typically inferred from SPDK bdev properties.
 */
typedef enum {
    XSAN_STORAGE_DISK_TYPE_UNKNOWN = 0,
    XSAN_STORAGE_DISK_TYPE_NVME_SSD,
    XSAN_STORAGE_DISK_TYPE_SATA_SSD,
    XSAN_STORAGE_DISK_TYPE_SAS_SSD,
    XSAN_STORAGE_DISK_TYPE_HDD_SATA,
    XSAN_STORAGE_DISK_TYPE_HDD_SAS,
    XSAN_STORAGE_DISK_TYPE_OTHER_SSD, // Generic SSD if more specific type unknown
    XSAN_STORAGE_DISK_TYPE_OTHER_HDD  // Generic HDD if more specific type unknown
} xsan_storage_disk_type_t;

/**
 * @brief Defines the operational state of a disk or disk group.
 */
typedef enum {
    XSAN_STORAGE_STATE_UNKNOWN = 0,      ///< State is not determined
    XSAN_STORAGE_STATE_INITIALIZING,   ///< Being initialized or discovered
    XSAN_STORAGE_STATE_ONLINE,         ///< Healthy and operational
    XSAN_STORAGE_STATE_OFFLINE,        ///< Intentionally taken offline or not available
    XSAN_STORAGE_STATE_DEGRADED,       ///< Operational but with reduced performance/redundancy
    XSAN_STORAGE_STATE_FAILED,         ///< Not operational due to failure
    XSAN_STORAGE_STATE_MISSING,        ///< Expected disk is not found
    XSAN_STORAGE_STATE_REBUILDING,     ///< Data is being rebuilt (e.g., for a RAID array)
    XSAN_STORAGE_STATE_MAINTENANCE     ///< Undergoing maintenance
} xsan_storage_state_t;

/**
 * @brief Defines the type or layout of a disk group.
 */
typedef enum {
    XSAN_DISK_GROUP_TYPE_UNDEFINED = 0,
    XSAN_DISK_GROUP_TYPE_PASSSTHROUGH, ///< Group of one or more bdevs, exposed individually or as a pool base
    XSAN_DISK_GROUP_TYPE_JBOD,         ///< Just a Bunch Of Disks, concatenated space (simple linear LVM-like)
    // Future types:
    // XSAN_DISK_GROUP_TYPE_RAID0,       ///< Striping
    // XSAN_DISK_GROUP_TYPE_RAID1,       ///< Mirroring
    // XSAN_DISK_GROUP_TYPE_RAID5,
    // XSAN_DISK_GROUP_TYPE_CACHE_TIER,  ///< Group with cache and capacity tiers
} xsan_disk_group_type_t;


// --- Core Storage Structures ---

/**
 * @brief Represents a single physical storage disk (backed by an SPDK bdev) within XSAN.
 */
typedef struct xsan_disk {
    xsan_disk_id_t id;                          ///< Unique XSAN identifier for this disk
    char bdev_name[XSAN_MAX_NAME_LEN];          ///< Corresponding SPDK bdev name (e.g., "Nvme0n1", "Malloc0")
    // Link to the group this disk is part of. A zeroed UUID means unassigned.
    xsan_group_id_t assigned_to_group_id;
    xsan_uuid_t bdev_uuid;                     ///< UUID of the SPDK bdev, if available

    xsan_storage_disk_type_t type;             ///< Physical type of the disk (SSD, HDD, NVMe)
    xsan_storage_state_t state;                ///< Current operational state of the disk in XSAN

    uint64_t capacity_bytes;                   ///< Total capacity of the disk in bytes
    uint32_t block_size_bytes;                 ///< Native block size of the disk in bytes
    uint64_t num_blocks;                       ///< Total number of blocks (capacity_bytes / block_size_bytes)

    char product_name[XSAN_MAX_NAME_LEN];      ///< Product name/model of the disk
    // char serial_number[XSAN_MAX_NAME_LEN];  // Consider adding if reliably obtainable from bdev
    // char firmware_revision[XSAN_MAX_NAME_LEN];

    bool is_rotational;                        ///< True if it's an HDD
    uint32_t optimal_io_boundary_blocks;       ///< Optimal I/O boundary in blocks
    bool has_write_cache;                      ///< True if write cache is enabled

    struct spdk_bdev_desc *bdev_descriptor;    ///< SPDK bdev descriptor, opened by disk_manager
                                               ///< NULL if not opened or could not be opened.

    // Linkage for disk manager's internal list (example, actual list mgmt may differ)
    // These are not needed if xsan_list stores void* to xsan_disk_t instances.
    // struct xsan_disk *next;
    // struct xsan_disk *prev;

    // Runtime information (optional, could be managed elsewhere)
    // uint64_t used_bytes;
    // uint64_t free_bytes;
    // xsan_group_id_t assigned_group_id; // ID of the disk group this disk belongs to, if any

} xsan_disk_t;

#define XSAN_MAX_DISKS_PER_GROUP 32 // Example, can be adjusted

/**
 * @brief Represents a group of xsan_disk_t instances, forming a logical storage pool or tier.
 */
typedef struct xsan_disk_group {
    xsan_group_id_t id;                        ///< Unique identifier for this disk group
    char name[XSAN_MAX_NAME_LEN];              ///< User-defined name for the disk group

    xsan_disk_group_type_t type;               ///< Type of the disk group (e.g., JBOD, RAID)
    xsan_storage_state_t state;                ///< Overall state of the disk group

    xsan_disk_id_t disk_ids[XSAN_MAX_DISKS_PER_GROUP]; ///< Array of disk IDs belonging to this group
    // Alternatively, or additionally, pointers to xsan_disk_t:
    // xsan_disk_t* disks[XSAN_MAX_DISKS_PER_GROUP];
    uint32_t disk_count;                       ///< Number of disks currently in this group

    uint64_t total_capacity_bytes;             ///< Total raw capacity of the group
    uint64_t usable_capacity_bytes;            ///< Usable capacity (after RAID overhead, formatting, etc.)
    // uint64_t used_capacity_bytes;

    // Depending on type, other params like stripe_size, chunk_size etc. might be needed.
    // For PASSSTHROUGH/JBOD, these might not be relevant initially.
    uint64_t allocated_bytes_in_group;      ///< Total bytes currently allocated to volumes from this group
    // For simple contiguous allocation within the group's logical space:
    uint64_t next_alloc_logical_block_in_group; ///< Next available logical block offset within the group's aggregated space
                                                ///< (expressed in terms of the smallest physical block size of member disks, or a common size)
    uint32_t group_logical_block_size;      ///< The block size used for group's logical space tracking (e.g., smallest physical block size)


    // Linkage for disk manager's internal list
    struct xsan_disk_group *next;
    struct xsan_disk_group *prev;

} xsan_disk_group_t;


/**
 * @brief Represents a logical volume presented to the user/VM.
 * Built on top of one or more disk groups.
 */
typedef struct xsan_volume {
    xsan_volume_id_t id;                       ///< Unique identifier for this logical volume
    char name[XSAN_MAX_NAME_LEN];              ///< User-defined name for the volume

    uint64_t size_bytes;                       ///< Total provisioned size of the volume in bytes
    uint32_t block_size_bytes;                 ///< Logical block size exposed by this volume (e.g., 512, 4096)
    uint64_t num_blocks;                       ///< Total number of logical blocks (size_bytes / block_size_bytes)

    xsan_storage_state_t state;                ///< Current state of the volume (e.g., ONLINE, CREATING, DELETED)

    xsan_group_id_t source_group_id;           ///< ID of the disk group providing storage for this volume

    bool thin_provisioned;                     ///< True if this is a thin-provisioned volume
    uint64_t allocated_bytes;                  ///< For thin-provisioned volumes, actual bytes allocated from the group
                                               ///< For thick-provisioned, this would equal size_bytes after creation.

    // xsan_policy_id_t policy_id;             // Future: Storage policy applied (replication, QoS, etc.)
    // time_t creation_time;
    // xsan_snapshot_id_t parent_snapshot_id;  // Future: For snapshots/clones

    // xsan_policy_id_t policy_id;             // Future: Storage policy applied (replication, QoS, etc.)
    // time_t creation_time;
    // xsan_snapshot_id_t parent_snapshot_id;  // Future: For snapshots/clones

    // Replication specific fields
    uint32_t FTT;                               ///< Failures To Tolerate (e.g., 0, 1, 2)
                                                ///< Number of data replicas = FTT + 1.
    uint32_t actual_replica_count;              ///< Actual number of replicas configured/active for this volume.
                                                ///< This might be less than FTT+1 if not enough resources.
    xsan_replica_location_t replica_nodes[XSAN_MAX_REPLICAS]; ///< Information about nodes holding replicas.
                                                              ///< replica_nodes[0] is often the primary/local.

    // next/prev pointers are not part of the data structure if xsan_list stores void*
} xsan_volume_t;


// --- Volume Allocation and Mapping Metadata ---

/**
 * @brief Describes a single physical extent on a disk that is part of a volume's allocation.
 */
typedef struct {
    xsan_disk_id_t disk_id;         ///< XSAN ID of the physical disk where this extent resides.
    uint64_t start_block_on_disk;   ///< Starting LBA of this extent on the physical disk.
    uint64_t num_blocks_on_disk;    ///< Number of blocks in this extent on this disk.
    uint64_t volume_start_lba;      ///< The logical LBA within the volume that this extent corresponds to.
                                    ///< (e.g., if volume is 1000 blocks, and this extent covers blocks 200-299 of the volume,
                                    ///< then volume_start_lba would be 200, and num_blocks_on_disk would map to 100 volume blocks).
                                    ///< If num_blocks_on_disk maps to fewer volume blocks due to block size differences,
                                    ///< this LBA is still in terms of volume's logical blocks.
} xsan_volume_extent_mapping_t;

// Define a reasonable maximum number of extents a single volume might be broken into.
// For simple linear or striped (across few disks) volumes, this can be small.
// For highly fragmented or very large JBOD-spanned volumes, it might be larger.
#define XSAN_MAX_EXTENTS_PER_VOLUME 8

/**
 * @brief Metadata describing how a volume is allocated across physical disk extents.
 * This structure would typically be serialized (e.g., to JSON) and stored in a K/V store
 * with a key like "volmap:<volume_uuid>".
 */
typedef struct {
    xsan_volume_id_t volume_id;     ///< The ID of the volume this allocation map belongs to.
    xsan_group_id_t disk_group_id;  ///< The ID of the disk group from which space was allocated.
    uint32_t num_extents;           ///< Number of actual extents used by this volume.
    xsan_volume_extent_mapping_t extents[XSAN_MAX_EXTENTS_PER_VOLUME]; ///< Array of extent mappings.
    uint64_t total_volume_blocks_logical; ///< Total logical blocks in the volume (for consistency check).
    uint32_t volume_logical_block_size;   ///< Logical block size of the volume (for consistency).
    // bool is_thin_provisioned;    // Already in xsan_volume_t, but could be here for map-specific logic
    // uint64_t last_updated_time;  // Optional: for tracking changes
} xsan_volume_allocation_meta_t;


#ifdef __cplusplus
}
#endif

#endif // XSAN_STORAGE_H
