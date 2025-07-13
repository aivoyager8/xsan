#ifndef XSAN_SPDK_MANAGER_H
#define XSAN_SPDK_MANAGER_H

#include "../../include/xsan_error.h"
#include "xsan_types.h" // For xsan_error_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque structure for SPDK application options.
 * This is used to pass options to the SPDK initialization.
 */
struct spdk_app_opts; // Forward declaration from SPDK

/**
 * @brief Initializes the SPDK application environment options.
 * This function should be called once at the start of the XSAN application
 * before calling xsan_spdk_manager_start_app.
 *
 * @param app_name The name of the application (e.g., "xsan_node"). Can be NULL for default.
 * @param spdk_conf_file Optional path to an SPDK configuration file (e.g., for bdevs, RPC). Can be NULL.
 * @param reactor_mask CPU core mask for SPDK reactors (e.g., "0x1", "0x3"). Can be NULL for default.
 * @param enable_rpc If true, starts the SPDK JSON-RPC server.
 * @param rpc_addr The listen address for the RPC server (e.g., "/var/tmp/spdk.sock" or "127.0.0.1:5260").
 *                 Only used if enable_rpc is true.
 * @return XSAN_OK on success, or an XSAN error code on failure.
 *         Note: This only prepares options. SPDK isn't fully started until xsan_spdk_manager_start_app.
 */
xsan_error_t xsan_spdk_manager_opts_init(const char *app_name,
                                         const char *spdk_conf_file,
                                         const char *reactor_mask,
                                         bool enable_rpc,
                                         const char *rpc_addr);

/**
 * @brief Callback function type that will be invoked once SPDK has started
 *        and reactors are running. This is where the main application logic
 *        that depends on SPDK should be initiated.
 *
 * @param arg User-provided argument passed to xsan_spdk_manager_start_app.
 * @param rc Return code from spdk_app_start. 0 on success.
 */
typedef void (*xsan_spdk_app_start_fn_t)(void *arg, int rc);


/**
 * @brief Starts the SPDK application framework.
 * This function initializes all SPDK subsystems based on options set via
 * xsan_spdk_manager_opts_init (or defaults if not called), starts the reactors,
 * and then calls the provided `start_fn` on an SPDK reactor thread.
 *
 * This function will block the calling thread until `spdk_app_stop()` is called
 * from within an SPDK context (e.g., from the `start_fn`, a reactor message, or a signal handler
 * that queues a stop message to a reactor).
 *
 * @param start_fn The application's main function to be called once SPDK is ready.
 *                 This function will be executed on an SPDK reactor thread.
 * @param fn_arg Argument to be passed to the `start_fn`.
 * @return XSAN_OK if SPDK started and exited cleanly (after spdk_app_stop).
 *         Returns an XSAN error code if spdk_app_start fails to initialize.
 */
xsan_error_t xsan_spdk_manager_start_app(xsan_spdk_app_start_fn_t start_fn, void *fn_arg);


/**
 * @brief Initiates the shutdown of the SPDK application.
 * This function should be called to stop the SPDK application framework.
 * It can be called from any thread (e.g., signal handler) or from an SPDK reactor.
 * It will cause `spdk_app_start` (wrapped by `xsan_spdk_manager_start_app`) to return.
 * Actual cleanup of SPDK resources happens after `spdk_app_start` returns.
 */
void xsan_spdk_manager_request_app_stop(void);


/**
 * @brief Cleans up SPDK resources after spdk_app_start has returned.
 * This should be called after xsan_spdk_manager_start_app completes.
 * It calls spdk_app_fini().
 */
void xsan_spdk_manager_app_fini(void);


#ifdef __cplusplus
}
#endif

#endif // XSAN_SPDK_MANAGER_H
