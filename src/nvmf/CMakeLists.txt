add_library(xsan_nvmf_target STATIC
    xsan_nvmf_target.c
)

# Public include directories needed by code that uses this library's headers
target_include_directories(xsan_nvmf_target PUBLIC
    ${CMAKE_SOURCE_DIR}/include      # For xsan_types.h, xsan_error.h
    ${CMAKE_SOURCE_DIR}/src/include  # For xsan_nvmf_target.h
)

# Dependencies for xsan_nvmf_target library itself
target_link_libraries(xsan_nvmf_target PUBLIC
    xsan_common     # For xsan_error_t
    xsan_utils      # For XSAN_LOG_*, XSAN_STRDUP (if used)
    # SPDK libraries are linked at the executable level.
    # However, this module directly calls SPDK NVMe-oF and bdev APIs.
    # The necessary SPDK headers (nvmf.h, bdev.h, thread.h, env.h) should be globally available.
    # Linking against specific SPDK components might be needed if not handled by XSAN_SPDK_LIBRARIES.
    # For example: spdk_nvmf, spdk_bdev, spdk_thread, spdk_env_dpdk
    # We rely on XSAN_SPDK_LIBRARIES from the root CMakeLists.txt to cover these.
)

message(STATUS "XSAN NVMe-oF Target module (xsan_nvmf_target.c) configured.")
