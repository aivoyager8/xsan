add_library(xsan_io STATIC
    xsan_io.c
)

target_include_directories(xsan_io PUBLIC
    ${CMAKE_SOURCE_DIR}/include      # For xsan_types.h, xsan_storage.h etc.
    ${CMAKE_SOURCE_DIR}/src/include  # For xsan_io.h, xsan_bdev.h etc.
)

# Dependencies for xsan_io library itself
target_link_libraries(xsan_io PUBLIC # Or INTERFACE if only headers are needed by consumers
    xsan_bdev       # Uses xsan_bdev_dma_malloc/free
    xsan_common     # For error types
    xsan_utils      # For logging, memory allocation
)

# SPDK includes are handled globally
# SPDK libraries linked by final executable
