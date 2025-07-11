add_library(xsan_replication STATIC
    xsan_replication_common.c
    # Add other .c files from src/replication/ here in the future
    # e.g., xsan_replication_manager.c, xsan_replica_placement.c
)

# Public include directories needed by code that uses this library's headers
# (e.g., if xsan_storage includes xsan_replication.h)
target_include_directories(xsan_replication PUBLIC
    ${CMAKE_SOURCE_DIR}/include      # For xsan_types.h, xsan_storage.h etc.
    ${CMAKE_SOURCE_DIR}/src/include  # For xsan_replication.h, xsan_io.h etc.
)

# Dependencies for xsan_replication library itself.
# These are other XSAN static libraries that xsan_replication_common.c might use.
target_link_libraries(xsan_replication PUBLIC
    xsan_common     # For xsan_error_t, potentially data structures if not using xsan_list directly
    xsan_utils      # For XSAN_MALLOC, XSAN_LOG_*
    xsan_io         # xsan_replicated_io_ctx_t contains xsan_io_request_t*
    # xsan_list     # If xsan_list is a separate target and used directly here.
                    # Currently, xsan_replicated_io_ctx_t doesn't directly use xsan_list.
)

# SPDK includes are handled globally by the root CMakeLists.txt.
# SPDK libraries will be linked by the final executable.
