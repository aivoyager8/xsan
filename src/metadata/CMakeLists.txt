add_library(xsan_metadata STATIC
    xsan_metadata_store.c
)

# Public include directories needed by code that uses this library's headers
target_include_directories(xsan_metadata PUBLIC
    ${CMAKE_SOURCE_DIR}/include      # For xsan_types.h
    ${CMAKE_SOURCE_DIR}/src/include  # For xsan_metadata_store.h
)

# Dependencies for xsan_metadata library itself
target_link_libraries(xsan_metadata PUBLIC # Use PUBLIC if its headers expose types from these, INTERFACE otherwise
    xsan_common     # For xsan_error_t
    xsan_utils      # For XSAN_MALLOC, XSAN_LOG_*
)

# Conditionally link RocksDB if SPDK (and thus advanced metadata) is enabled
if(XSAN_ENABLE_SPDK)
    # Ensure that XSAN_ROCKSDB_LIBRARIES and XSAN_ROCKSDB_INCLUDE_DIRS are available
    # from the root CMakeLists.txt (they are set as CACHE INTERNAL).
    # The include directories for RocksDB should already be globally applied if found.

    # Link against RocksDB library.
    # The XSAN_ROCKSDB_LIBRARIES variable should contain the necessary linker flags or library names.
    target_link_libraries(xsan_metadata PRIVATE ${XSAN_ROCKSDB_LIBRARIES})

    # The RocksDB C API is often a wrapper around a C++ core,
    # so linking against the C++ standard library might be necessary.
    # This can sometimes be handled by RocksDB's own pkg-config file if it lists 'stdc++'
    # in Libs.private. If not, we add it explicitly.
    # Check if "stdc++" is already in XSAN_ROCKSDB_LIBRARIES or handled by it.
    # Forcing it here might be needed on some systems.
    # A common way to do this platform-independently if needed:
    # target_link_libraries(xsan_metadata PRIVATE Threads::stdc++) # If using modern CMake for stdc++
    # Or simply:
    target_link_libraries(xsan_metadata PRIVATE stdc++)

    # Note: json-c is NOT a direct dependency of xsan_metadata_store.c itself.
    # json-c will be used by disk_manager.c and volume_manager.c (in xsan_storage lib)
    # for serializing the objects *before* calling xsan_metadata_store_put.
endif()
