#include <stdio.h>

// SPDK 日志集成包装函数，重定向到 XSAN 日志系统
static void _xsan_spdk_log_print_fn_wrapper(const char *file, int line, const char *func, const char *format, ...) {
    va_list args;
    va_start(args, format);
    // 可根据 XSAN_LOG_LEVEL 映射 SPDK 日志等级
    fprintf(stderr, "[SPDK][%s:%d][%s] ", file, line, func);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}
#include "xsan_spdk_manager.h"
#include "xsan_error.h"
#include "xsan_log.h" // For logging XSAN specific messages

#include "spdk/event.h"   // 包含 spdk_app_start, spdk_app_opts_init, spdk_app_stop, spdk_app_fini
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include <stdarg.h>      // For va_list in log print function
#include <string.h>      // For strlen in log print function

// Global structure to hold SPDK application options.
// SPDK's spdk_app_opts is effectively global for the spdk_app_start call.
static struct spdk_app_opts g_xsan_spdk_app_opts;
static bool g_xsan_spdk_opts_customized = false; // Flag to see if user called opts_init

// Wrapper context for the user's start function and its argument
typedef struct {
    xsan_spdk_app_start_fn_t user_main_fn;
    void *user_main_fn_arg;
} xsan_internal_app_start_ctx_t;

/**
 * @brief This is the actual "main" function passed to spdk_app_start.
 * It is responsible for calling the user-provided xsan_spdk_app_start_fn_t.
 * This function executes on an SPDK reactor thread.
 *
 * @param arg1 Custom context (xsan_internal_app_start_ctx_t*).
 */
static void
_xsan_internal_spdk_app_main(void *arg1)
{
    xsan_internal_app_start_ctx_t *ctx = (xsan_internal_app_start_ctx_t *)arg1;

    XSAN_LOG_INFO("SPDK application main wrapper started on SPDK reactor.");

    if (ctx && ctx->user_main_fn) {
        // The 'rc' parameter for user_main_fn is to indicate if spdk_app_start itself
        // had an issue before even calling this main function.
        // Since we are *in* this function, spdk_app_start has successfully launched reactors.
        // So, we pass 0 (success) for the SPDK part of the startup.
        // Any errors from the user's function are their own to handle or propagate.
        ctx->user_main_fn(ctx->user_main_fn_arg, 0 /* rc for spdk_app_start success */);
    } else {
        XSAN_LOG_ERROR("No user start function provided to SPDK app main wrapper.");
        // If no user function, we might want to stop the app if this isn't intended.
        // However, some apps might just initialize SPDK and let RPC handle things.
        // For now, it just means this main SPDK thread function completes.
        // If other reactors/services were started, they would keep running.
    }

    // If user_main_fn is non-blocking and returns, or if it's NULL,
    // this thread function will complete.
    // SPDK reactors will continue to run until spdk_app_stop() is explicitly called.
    // If user_main_fn is intended to be the main "blocking" work, it should
    // either loop itself or ensure spdk_app_stop() is called upon completion/error.
    XSAN_LOG_DEBUG("SPDK application main wrapper returning.");
}

xsan_error_t xsan_spdk_manager_opts_init(const char *app_name,
                                         const char *spdk_conf_file,
                                         const char *reactor_mask,
                                         bool enable_rpc,
                                         const char *rpc_addr) {
    // 参数校验优先
    if (!app_name || !spdk_conf_file) {
        XSAN_LOG_ERROR("SPDK manager opts init: missing app_name or conf_file");
        return XSAN_ERROR_INVALID_PARAM;
    }
    // 初始化 opts 结构体
    spdk_app_opts_init(&g_xsan_spdk_app_opts, sizeof(g_xsan_spdk_app_opts));
    g_xsan_spdk_opts_customized = true;
    g_xsan_spdk_app_opts.name = app_name;
    g_xsan_spdk_app_opts.json_config_file = spdk_conf_file;
    if (reactor_mask) {
        g_xsan_spdk_app_opts.reactor_mask = reactor_mask;
    }
    if (enable_rpc) {
        g_xsan_spdk_app_opts.rpc_addr = rpc_addr ? rpc_addr : SPDK_DEFAULT_RPC_ADDR;
    } else {
        g_xsan_spdk_app_opts.rpc_addr = NULL;
    }
    XSAN_LOG_INFO("SPDK manager opts initialized: %s, conf: %s", app_name, spdk_conf_file);
    return XSAN_OK;
}

xsan_error_t xsan_spdk_manager_start_app(xsan_spdk_app_start_fn_t start_fn, void *fn_arg) {
    if (!g_xsan_spdk_opts_customized) {
        XSAN_LOG_INFO("SPDK options not explicitly customized by xsan_spdk_manager_opts_init(), using defaults.");
        spdk_app_opts_init(&g_xsan_spdk_app_opts, sizeof(g_xsan_spdk_app_opts));
        g_xsan_spdk_app_opts.name = "xsan_default_app";
    }

    if (!start_fn) {
        XSAN_LOG_ERROR("User start function (start_fn) is NULL.");
        return XSAN_ERROR_INVALID_PARAM;
    }

    xsan_internal_app_start_ctx_t app_main_ctx;
    app_main_ctx.user_main_fn = start_fn;
    app_main_ctx.user_main_fn_arg = fn_arg;

    XSAN_LOG_INFO("Starting SPDK application framework...");
    int rc = spdk_app_start(&g_xsan_spdk_app_opts, _xsan_internal_spdk_app_main, &app_main_ctx);
    if (rc) {
        XSAN_LOG_ERROR("spdk_app_start() failed with return code: %d", rc);
        spdk_app_fini();
        return XSAN_ERROR_SPDK_START_FAILED;
    }
    XSAN_LOG_INFO("SPDK application framework has stopped (spdk_app_start returned). Calling spdk_app_fini().");
    spdk_app_fini();
    return XSAN_OK;
}

void xsan_spdk_manager_request_app_stop(void) {
    XSAN_LOG_INFO("Requesting SPDK application to stop...");
    // spdk_app_stop can be called from any thread. It signals the SPDK event loop
    // on the master core to begin the shutdown sequence.
    // The '0' argument is an exit status code for the application.
    spdk_app_stop(0);
}

void xsan_spdk_manager_app_fini(void) {
    XSAN_LOG_INFO("Finalizing SPDK application environment...");
    spdk_app_fini();
    g_xsan_spdk_opts_customized = false; // Reset flag
    XSAN_LOG_INFO("SPDK application environment finalized.");
}
