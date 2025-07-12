add_library(xsan_replication STATIC
    xsan_replication.c # Added: contains context create/free functions
    xsan_replication_common.c
    # Add other .c files from src/replication/ here in the future
    # e.g., xsan_replication_manager.c, xsan_replica_placement.c
)

# Public include directories needed by code that uses this library's headers
target_include_directories(xsan_replication PUBLIC
    ${CMAKE_SOURCE_DIR}/include      # For xsan_types.h, xsan_storage.h etc.
    ${CMAKE_SOURCE_DIR}/src/include  # For xsan_replication.h, xsan_io.h etc.
)

# Dependencies for xsan_replication library itself.
target_link_libraries(xsan_replication PUBLIC
    xsan_common     # For xsan_error_t
    xsan_utils      # For XSAN_MALLOC, XSAN_LOG_*, xsan_strdup
    xsan_io         # For xsan_io_request_t in headers
    xsan_bdev       # For xsan_bdev_dma_free
    xsan_protocol   # For xsan_protocol_message_destroy
)

# SPDK includes are handled globally by the root CMakeLists.txt.
# SPDK libraries will be linked by the final executable.
