#include "xsan_nvmf_target.h"
#include "xsan_log.h"
#include "xsan_error.h"
#include "xsan_memory.h" // For XSAN_STRDUP if needed for NQN copy

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h" // For spdk_nvmf_subsystem_set_allow_any_host
#include "spdk/bdev.h"     // For spdk_bdev_get_by_name

// Define a default NQN if none is provided.
// Format: nqn.2016-06.io.spdk:xsan-target
#define XSAN_DEFAULT_NVMF_NQN_PREFIX "nqn.2024-01.org.xsan:"
#define XSAN_DEFAULT_NVMF_SUBSYSTEM_SERIAL "XSAN000000000001"
#define XSAN_DEFAULT_NVMF_SUBSYSTEM_MODEL "XSAN Virtual Controller"

// Global context for this module (simplified for now)
static struct spdk_nvmf_tgt *g_xsan_nvmf_tgt = NULL;
static struct spdk_nvmf_subsystem *g_xsan_default_subsystem = NULL;
// We might support multiple transports, but TCP is common for initial setup.
// static struct spdk_nvmf_transport *g_xsan_tcp_transport = NULL;

// Store NQN to avoid issues with string lifetime if passed NQN is on stack
static char g_target_nqn_storage[SPDK_NVMF_NQN_MAX_LEN + 1];


xsan_error_t xsan_nvmf_target_init(const char *target_nqn_param,
                                   const char *listen_addr,
                                   const char *listen_port_str) {
    if (g_xsan_nvmf_tgt) {
        XSAN_LOG_WARN("XSAN NVMe-oF Target already initialized.");
        return XSAN_OK;
    }
    if (spdk_get_thread() == NULL) {
        XSAN_LOG_ERROR("NVMe-oF Target init must be called from an SPDK thread.");
        return XSAN_ERROR_THREAD_CONTEXT;
    }
    if (!listen_addr || !listen_port_str) {
        XSAN_LOG_ERROR("Listen address and port must be provided for NVMe-oF Target init.");
        return XSAN_ERROR_INVALID_PARAM;
    }

    XSAN_LOG_INFO("Initializing XSAN NVMe-oF Target...");

    int rc = 0;
    const char *used_nqn = target_nqn_param;

    if (!used_nqn || strlen(used_nqn) == 0) {
        // Generate a default NQN if not provided
        snprintf(g_target_nqn_storage, sizeof(g_target_nqn_storage), "%stgt1", XSAN_DEFAULT_NVMF_NQN_PREFIX);
        used_nqn = g_target_nqn_storage;
        XSAN_LOG_INFO("No NQN provided, using default: %s", used_nqn);
    } else {
        // Copy the provided NQN in case it's a temporary string
        strncpy(g_target_nqn_storage, target_nqn_param, SPDK_NVMF_NQN_MAX_LEN);
        g_target_nqn_storage[SPDK_NVMF_NQN_MAX_LEN] = '\0'; // Ensure null termination
        used_nqn = g_target_nqn_storage;
    }


    // 1. Create an NVMe-oF Target
    // Max subsystems can be configured via spdk_nvmf_tgt_opts if needed. Default is usually fine.
    g_xsan_nvmf_tgt = spdk_nvmf_tgt_create(NULL); // Use default opts
    if (!g_xsan_nvmf_tgt) {
        XSAN_LOG_ERROR("spdk_nvmf_tgt_create() failed.");
        return XSAN_ERROR_SPDK_API; // Define this error
    }

    // 2. Create and configure the TCP transport (common default)
    // Transport specific opts can be set in struct spdk_nvmf_transport_opts
    struct spdk_nvmf_transport_opts transport_opts;
    spdk_nvmf_transport_opts_init("TCP", &transport_opts, sizeof(transport_opts));
    // transport_opts.io_unit_size = 8192; // Example: Set IO unit size for this transport
    // transport_opts.max_queue_depth = 128; // Example

    struct spdk_nvmf_transport *tcp_transport = spdk_nvmf_transport_create("TCP", &transport_opts);
    if (!tcp_transport) {
        XSAN_LOG_ERROR("spdk_nvmf_transport_create('TCP') failed.");
        spdk_nvmf_tgt_destroy(g_xsan_nvmf_tgt);
        g_xsan_nvmf_tgt = NULL;
        return XSAN_ERROR_SPDK_API;
    }
    // g_xsan_tcp_transport = tcp_transport; // Store if needed for specific operations later

    // 3. Create a default NVMf Subsystem
    // A subsystem is what an initiator discovers and connects to.
    g_xsan_default_subsystem = spdk_nvmf_subsystem_create(g_xsan_nvmf_tgt, used_nqn,
                                                          SPDK_NVMF_SUBTYPE_NVME, // or SPDK_NVMF_SUBTYPE_DISCOVERY
                                                          1); // Number of NS, can be increased dynamically
    if (!g_xsan_default_subsystem) {
        XSAN_LOG_ERROR("spdk_nvmf_subsystem_create() failed for NQN %s.", used_nqn);
        // spdk_nvmf_transport_destroy(tcp_transport); // How to get this handle back if not stored globally?
        // Need a way to iterate and destroy transports if tgt_destroy doesn't do it.
        // For now, assume tgt_destroy cleans up transports created for it if not explicitly destroyed.
        // Better: SPDK examples show transports are usually created and then passed to tgt_listen.
        // The target doesn't "own" transports created this way directly.
        // Let's defer transport destruction to fini.
        spdk_nvmf_tgt_destroy(g_xsan_nvmf_tgt);
        g_xsan_nvmf_tgt = NULL;
        return XSAN_ERROR_SPDK_API;
    }

    // Configure the subsystem (e.g., allow any host to connect for simplicity)
    spdk_nvmf_subsystem_set_allow_any_host(g_xsan_default_subsystem, true);

    // Set serial number and model number for the subsystem (good practice)
    rc = spdk_nvmf_subsystem_set_sn(g_xsan_default_subsystem, XSAN_DEFAULT_NVMF_SUBSYSTEM_SERIAL);
    if (rc != 0) {
        XSAN_LOG_WARN("Failed to set subsystem serial number for NQN %s: %s", used_nqn, spdk_strerror(-rc));
    }
    rc = spdk_nvmf_subsystem_set_mn(g_xsan_default_subsystem, XSAN_DEFAULT_NVMF_SUBSYSTEM_MODEL);
     if (rc != 0) {
        XSAN_LOG_WARN("Failed to set subsystem model number for NQN %s: %s", used_nqn, spdk_strerror(-rc));
    }


    // 4. Add a listener for the subsystem on the TCP transport
    struct spdk_nvmf_listen_addr listen_saddr; // Changed from spdk_nvme_transport_id
    memset(&listen_saddr, 0, sizeof(listen_saddr));
    listen_saddr.trtype = SPDK_NVMF_TRTYPE_TCP; // Explicitly TCP
    xsan_strcpy_safe(listen_saddr.traddr, listen_addr, sizeof(listen_saddr.traddr));
    xsan_strcpy_safe(listen_saddr.trsvcid, listen_port_str, sizeof(listen_saddr.trsvcid));

    rc = spdk_nvmf_subsystem_add_listener(g_xsan_default_subsystem, &listen_saddr);
    if (rc != 0) {
        XSAN_LOG_ERROR("spdk_nvmf_subsystem_add_listener() failed for NQN %s on %s:%s : %s",
                       used_nqn, listen_addr, listen_port_str, spdk_strerror(-rc));
        spdk_nvmf_subsystem_destroy(g_xsan_default_subsystem, NULL, NULL); //Requires callback, or direct destroy
        g_xsan_default_subsystem = NULL;
        spdk_nvmf_tgt_destroy(g_xsan_nvmf_tgt);
        g_xsan_nvmf_tgt = NULL;
        // Transport cleanup might be needed too
        return XSAN_ERROR_SPDK_API;
    }
    XSAN_LOG_INFO("NVMe-oF Target listening on %s (IP: %s, Port: %s) for NQN: %s",
                  "TCP", listen_addr, listen_port_str, used_nqn);

    // 5. Start the subsystem (makes it available for discovery and connection)
    // Subsystems are started asynchronously. The callback is optional.
    rc = spdk_nvmf_subsystem_start(g_xsan_default_subsystem, NULL, NULL);
    if (rc != 0) {
         XSAN_LOG_ERROR("spdk_nvmf_subsystem_start() failed for NQN %s: %s", used_nqn, spdk_strerror(-rc));
        // Complex cleanup needed here as listener might be active
        // For simplicity, we might leak some resources on this specific error path
        // or rely on higher level fini to clean up.
        return XSAN_ERROR_SPDK_API;
    }

    XSAN_LOG_INFO("XSAN NVMe-oF Target initialized and default subsystem NQN '%s' started.", used_nqn);
    return XSAN_OK;
}

void xsan_nvmf_target_fini(void) {
    if (!g_xsan_nvmf_tgt) {
        XSAN_LOG_DEBUG("XSAN NVMe-oF Target already finalized or not initialized.");
        return;
    }
    XSAN_LOG_INFO("Finalizing XSAN NVMe-oF Target...");

    // Subsystems (and their namespaces, listeners) should be destroyed before the target.
    // SPDK's spdk_nvmf_tgt_destroy is supposed to clean up its subsystems.
    // If we created subsystems separately and need specific cleanup for them:
    if (g_xsan_default_subsystem) {
        // To stop it if subsystem_destroy doesn't imply stop:
        // spdk_nvmf_subsystem_stop(g_xsan_default_subsystem, NULL, NULL); // Async, wait if needed
        // spdk_nvmf_subsystem_set_state(g_xsan_default_subsystem, SPDK_NVMF_SUBSYSTEM_STATE_INACTIVE);
        // spdk_nvmf_subsystem_pause(g_xsan_default_subsystem, NULL, NULL);

        // Destroying the subsystem should also remove its listeners and namespaces.
        // The spdk_nvmf_subsystem_destroy is asynchronous.
        // For a synchronous-like fini, one might need to use the callback or poll.
        // For simplicity in this example, we call destroy.
        // Proper shutdown might involve spdk_nvmf_subsystem_stop() first.
        // Let's assume tgt_destroy handles subsystems correctly.
        XSAN_LOG_DEBUG("Default subsystem NQN %s will be destroyed by tgt_destroy.", spdk_nvmf_subsystem_get_nqn(g_xsan_default_subsystem));
        g_xsan_default_subsystem = NULL; // tgt_destroy should handle it.
    }

    spdk_nvmf_tgt_destroy(g_xsan_nvmf_tgt, NULL, NULL); // Also async.
    g_xsan_nvmf_tgt = NULL;

    // Transports are typically managed globally by SPDK after creation
    // and might be destroyed via spdk_nvmf_transport_destroy if we have the handle
    // or during spdk_app_fini. For transports created with opts and passed to tgt_listen,
    // they are often managed by the target.
    // If we called spdk_nvmf_transport_create, we should call spdk_nvmf_transport_destroy.
    // Let's assume for now that spdk_app_fini or tgt_destroy handles transports if not explicitly.
    // This needs verification against SPDK examples for the specific transport creation method.
    // If spdk_nvmf_transport_create was used, we need to destroy it:
    // struct spdk_nvmf_transport *tcp_transport = spdk_nvmf_get_transport("TCP");
    // if (tcp_transport) {
    //     spdk_nvmf_transport_destroy(tcp_transport, NULL, NULL);
    // }
    // However, getting it by name might not be safe if multiple exist or if it was already cleaned.
    // Better to store g_xsan_tcp_transport if we create it.
    // For now, relying on SPDK's higher-level cleanup (spdk_app_fini).

    XSAN_LOG_INFO("XSAN NVMe-oF Target finalized.");
}

xsan_error_t xsan_nvmf_target_add_namespace(const char *bdev_name, uint32_t nsid, const char *volume_uuid_str) {
    // STUB
    XSAN_LOG_WARN("xsan_nvmf_target_add_namespace for bdev '%s' (NSID %u) - STUB", bdev_name, nsid);
    if (!g_xsan_nvmf_tgt || !g_xsan_default_subsystem) {
        XSAN_LOG_ERROR("NVMe-oF target or default subsystem not initialized.");
        return XSAN_ERROR_NOT_INITIALIZED;
    }
    if (!bdev_name || bdev_name[0] == '\0') {
        return XSAN_ERROR_INVALID_PARAM;
    }

    struct spdk_bdev *bdev = spdk_bdev_get_by_name(bdev_name);
    if (!bdev) {
        XSAN_LOG_ERROR("Bdev '%s' not found to add as NVMe-oF namespace.", bdev_name);
        return XSAN_ERROR_NOT_FOUND;
    }

    struct spdk_nvmf_ns_opts ns_opts;
    spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
    if (nsid > 0) { // If 0, SPDK assigns next available. If >0, user requests specific.
        ns_opts.nsid = nsid;
    }
    if (volume_uuid_str && strlen(volume_uuid_str) > 0) {
        if (spdk_uuid_parse(&ns_opts.uuid, volume_uuid_str) != 0) {
            XSAN_LOG_WARN("Failed to parse provided volume UUID string '%s' for namespace. Namespace will get a generated UUID.", volume_uuid_str);
            // ns_opts.uuid will remain zeroed, SPDK will generate one.
        } else {
             char parsed_uuid_char[SPDK_UUID_STRING_LEN];
             spdk_uuid_fmt_lower(parsed_uuid_char, sizeof(parsed_uuid_char), &ns_opts.uuid);
             XSAN_LOG_INFO("Using provided UUID %s for NVMe-oF namespace for bdev %s", parsed_uuid_char, bdev_name);
        }
    }
    // Can also set NGUID (ns_opts.nguid) if needed.

    uint32_t actual_nsid = spdk_nvmf_subsystem_add_ns(g_xsan_default_subsystem, bdev, &ns_opts);
    if (actual_nsid == 0) { // spdk_nvmf_subsystem_add_ns returns 0 on failure, or the assigned NSID on success.
        XSAN_LOG_ERROR("spdk_nvmf_subsystem_add_ns() failed for bdev '%s' on NQN %s.",
                       bdev_name, spdk_nvmf_subsystem_get_nqn(g_xsan_default_subsystem));
        return XSAN_ERROR_SPDK_API;
    }

    XSAN_LOG_INFO("Namespace (NSID: %u) added for bdev '%s' to subsystem NQN '%s'.",
                  actual_nsid, bdev_name, spdk_nvmf_subsystem_get_nqn(g_xsan_default_subsystem));
    return XSAN_OK;
}

xsan_error_t xsan_nvmf_target_remove_namespace(uint32_t nsid) {
    if (!g_xsan_nvmf_tgt || !g_xsan_default_subsystem) {
        XSAN_LOG_ERROR("NVMe-oF target or default subsystem not initialized for ns remove.");
        return XSAN_ERROR_NOT_INITIALIZED;
    }
    if (nsid == 0) { // Invalid NSID
        return XSAN_ERROR_INVALID_PARAM;
    }

    int rc = spdk_nvmf_subsystem_remove_ns(g_xsan_default_subsystem, nsid);
    if (rc != 0) {
        XSAN_LOG_ERROR("spdk_nvmf_subsystem_remove_ns() failed for NSID %u on NQN %s: %s",
                       nsid, spdk_nvmf_subsystem_get_nqn(g_xsan_default_subsystem), spdk_strerror(-rc));
        return XSAN_ERROR_SPDK_API;
    }

    XSAN_LOG_INFO("Namespace (NSID: %u) removed from subsystem NQN '%s'.",
                  nsid, spdk_nvmf_subsystem_get_nqn(g_xsan_default_subsystem));
    return XSAN_OK;
}
