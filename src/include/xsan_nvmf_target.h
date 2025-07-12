#ifndef XSAN_NVMF_TARGET_H
#define XSAN_NVMF_TARGET_H

#include "xsan_error.h"
#include "xsan_types.h" // For xsan_volume_id_t (if needed in future public APIs)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the XSAN NVMe-oF Target subsystem.
 * This function should be called once from an SPDK reactor thread after
 * SPDK has fully started and core XSAN modules (like volume manager for bdev access)
 * are ready.
 *
 * It will create default NVMe-oF transports (e.g., TCP) and a default target.
 *
 * @param target_nqn The NQN for the default XSAN NVMe-oF Target Subsystem.
 *                   If NULL, a default NQN might be generated or an error returned.
 * @param listen_addr IP address for the NVMe-oF target to listen on (e.g., "0.0.0.0").
 * @param listen_port Port number for the NVMe-oF target (e.g., 4420 for TCP).
 * @return XSAN_OK on success, or an xsan_error_t code on failure.
 */
xsan_error_t xsan_nvmf_target_init(const char *target_nqn,
                                   const char *listen_addr,
                                   const char *listen_port_str); // Port as string for spdk_nvmf_transport_id

/**
 * @brief Finalizes and cleans up the XSAN NVMe-oF Target subsystem.
 * Stops all subsystems, listeners, and destroys transports and the target.
 * Must be called from an SPDK reactor thread during application shutdown.
 */
void xsan_nvmf_target_fini(void);

/**
 * @brief Makes an XSAN volume (represented by its backing SPDK bdev) available
 *        as a namespace under the default NVMe-oF subsystem.
 *
 * @param bdev_name The name of the SPDK bdev that backs the XSAN volume.
 * @param nsid The desired Namespace ID (NSID) for this namespace. If 0, SPDK might assign one.
 *             It's generally better to assign a specific, unique NSID > 0.
 * @param volume_uuid_str Optional: String representation of the XSAN volume UUID for the namespace. Can be NULL.
 * @return XSAN_OK on success, or an xsan_error_t code on failure.
 *         XSAN_ERROR_NOT_FOUND if the bdev_name does not correspond to a known SPDK bdev.
 *         XSAN_ERROR_ALREADY_EXISTS if a namespace with the given NSID already exists (if nsid > 0).
 */
xsan_error_t xsan_nvmf_target_add_namespace(const char *bdev_name, uint32_t nsid, const char *volume_uuid_str);

/**
 * @brief Removes a namespace (previously added XSAN volume) from the default NVMe-oF subsystem.
 *
 * @param nsid The Namespace ID (NSID) of the namespace to remove.
 * @return XSAN_OK on success, or XSAN_ERROR_NOT_FOUND if no namespace with the given NSID exists.
 */
xsan_error_t xsan_nvmf_target_remove_namespace(uint32_t nsid);

// TODO: Add functions to create/delete/manage multiple NVMf subsystems if needed,
//       rather than just a single default one.

#ifdef __cplusplus
}
#endif

#endif // XSAN_NVMF_TARGET_H
