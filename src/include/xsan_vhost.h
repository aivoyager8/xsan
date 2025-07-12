#include "xsan_storage.h" // For xsan_volume_t, xsan_volume_id_t
#ifndef XSAN_VHOST_H
#define XSAN_VHOST_H

#include "xsan_types.h"   // For xsan_error_t
#include "xsan_storage.h" // For xsan_volume_id_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the XSAN vhost subsystem, primarily by registering the
 * XSAN virtual bdev (vbdev) module with SPDK.
 * This function should be called once during SPDK application startup,
 * from an SPDK thread, after other necessary XSAN managers (like volume_manager)
 * are initialized.
 *
 * @param vm Pointer to the initialized xsan_volume_manager instance, which will be
 *           needed by the vbdev module to look up XSAN volumes.
 * @return XSAN_OK on success, or an xsan_error_t code on failure.
 */
xsan_error_t xsan_vhost_subsystem_init(struct xsan_volume_manager *vm);

/**
 * @brief Unregisters the XSAN vbdev module and cleans up any global resources
 * used by the XSAN vhost subsystem.
 * Should be called during SPDK application shutdown.
 */
void xsan_vhost_subsystem_fini(void);

/**
 * @brief Creates and registers an XSAN virtual bdev (vbdev) that exposes an
 * existing XSAN logical volume.
 * Once created, this vbdev can be used as a backend for an SPDK vhost LUN
 * (typically configured via SPDK JSON config or RPC).
 *
 * This function MUST be called from an SPDK thread.
 *
 * @param volume_id The ID of the XSAN logical volume to expose.
 * @param vbdev_name The desired name for the new virtual bdev (e.g., "xsan_vol_MyVMDisk1").
 *                   This name will be used by SPDK to identify this vbdev.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_NOT_FOUND if the xsan_volume_id_t does not correspond to an existing volume.
 *         XSAN_ERROR_ALREADY_EXISTS if a vbdev with the given name already exists.
 *         XSAN_ERROR_OUT_OF_MEMORY if memory allocation fails.
 *         Other xsan_error_t codes for underlying SPDK errors.
 */
xsan_error_t xsan_vhost_expose_volume_as_vbdev(xsan_volume_id_t volume_id, const char *vbdev_name);

/**
 * @brief Destroys and unregisters an XSAN virtual bdev previously created by
 * xsan_vhost_expose_volume_as_vbdev.
 * This effectively stops exposing the XSAN volume as an SPDK bdev.
 *
 * This function MUST be called from an SPDK thread.
 *
 * @param vbdev_name The name of the XSAN virtual bdev to destroy.
 * @return XSAN_OK on success.
 *         XSAN_ERROR_NOT_FOUND if no vbdev with the given name exists.
 *         Other xsan_error_t codes for underlying SPDK errors.
 */
xsan_error_t xsan_vhost_unexpose_volume_vbdev(const char *vbdev_name);


/**
 * @brief (Optional Helper for SPDK JSON Config or RPC)
 * Generates a suggested bdev name for an XSAN volume.
 * e.g., "xsan_vol_" + volume_name or volume_uuid.
 *
 * @param volume_id The ID of the XSAN volume.
 * @param buffer Buffer to store the generated name.
 * @param buffer_len Length of the buffer.
 * @return XSAN_OK on success, XSAN_ERROR_INVALID_PARAM or XSAN_ERROR_NOT_FOUND if volume not found.
 */
// xsan_error_t xsan_vhost_get_suggested_vbdev_name(xsan_volume_id_t volume_id, char *buffer, size_t buffer_len);


// Note: The actual creation of vhost-user SCSI controllers and attaching LUNs (which point to these
// XSAN vbdevs) is typically handled by SPDK's configuration mechanisms (JSON file at startup or JSON-RPC).
// This module focuses on providing the XSAN vbdevs that SPDK's vhost layer can then use.
// If direct programmatic control over vhost controller/LUN creation is needed from XSAN,
// further functions would be added here to wrap SPDK RPC calls or equivalent C APIs if available
// for that purpose.

#ifdef __cplusplus
}
#endif

#endif // XSAN_VHOST_H
