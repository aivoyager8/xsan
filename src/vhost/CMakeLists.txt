add_library(xsan_vhost STATIC
    xsan_vhost.c
)

# Public include directories needed by code that uses this library's headers
target_include_directories(xsan_vhost PUBLIC
    ${CMAKE_SOURCE_DIR}/include      # For xsan_types.h, xsan_storage.h etc.
    ${CMAKE_SOURCE_DIR}/src/include  # For xsan_vhost.h, xsan_volume_manager.h etc.
)

# Dependencies for xsan_vhost library itself
target_link_libraries(xsan_vhost PUBLIC # Use PUBLIC for transitive include directory propagation
    xsan_volume_manager # Uses xsan_volume_get_by_id, xsan_volume_read/write_async
    xsan_io             # Indirectly via volume_manager, or directly if vhost needs to create xsan_io_request_t
    xsan_common         # For error types, etc.
    xsan_utils          # For logging, memory allocation
)

# SPDK includes are handled globally by the root CMakeLists.txt when XSAN_ENABLE_SPDK is ON.
# SPDK libraries (like spdk_vhost, spdk_scsi) will be linked by the final executable (xsan_node),
# not directly by this static library, but the include directories must be available.
# If this library *statically* linked SPDK components that weren't header-only, then PRIVATE linking here
# would be appropriate for those specific SPDK components. But for general SPDK usage,
# the final executable handles the broad SPDK library linking.
