# Enable testing for the project
enable_testing()

# Ensure CUnit is found. This should ideally be in the root CMakeLists.txt
# find_package(CUnit REQUIRED)
# If not found globally, you might need to specify its include dirs and libs manually or use pkg-config.
# For now, assume CUnit headers are findable and 'cunit' is the link library name.

# --- Test for Infrastructure (example, based on existing test_infrastructure.c) ---
add_executable(xsan_test_infrastructure test_infrastructure.c)

target_link_libraries(xsan_test_infrastructure PRIVATE
    xsan_common  # Assuming test_infrastructure uses common types/error codes
    xsan_utils   # Assuming test_infrastructure uses logging or memory utils
    # Add any other specific xsan libraries test_infrastructure.c needs
    cunit        # Link against CUnit
    Threads::Threads # For pthreads if needed
    ${XSAN_SPDK_LIBRARIES}
)

target_include_directories(xsan_test_infrastructure PRIVATE
    ${CMAKE_SOURCE_DIR}/include      # For xsan_types.h
    ${CMAKE_SOURCE_DIR}/src/include  # For internal headers like xsan_config.h if used by utils
)

add_test(NAME XsanInfrastructureTest COMMAND xsan_test_infrastructure)


# --- Test for Cluster module (xsan_get_local_node_info) ---
add_executable(xsan_test_cluster test_cluster.c)

target_link_libraries(xsan_test_cluster PRIVATE
    xsan_cluster  # The library we are testing
    xsan_utils    # For xsan_config.c (linked by xsan_utils) and xsan_log.h, xsan_string_utils.h
    xsan_common   # For xsan_error_t
    cunit         # CUnit library
    Threads::Threads
    # Link SPDK libraries needed by xsan_cluster.c (e.g., for spdk_uuid_parse)
    # XSAN_SPDK_LIBRARIES should be available from the root CMakeLists.txt
    # It typically includes spdk_util which has uuid functions.
    ${XSAN_SPDK_LIBRARIES}
)

target_include_directories(xsan_test_cluster PRIVATE
    ${CMAKE_SOURCE_DIR}/include       # For xsan_types.h, xsan_cluster.h
    ${CMAKE_SOURCE_DIR}/src/include   # For xsan_config.h (used by xsan_cluster.c indirectly)
    # SPDK include directories are globally added in the root CMakeLists.txt
)

add_test(NAME XsanClusterGetLocalNodeInfoTest COMMAND xsan_test_cluster)

# Add more tests here in the future in a similar manner.
# Example:
# add_executable(xsan_test_storage test_storage.c)
# target_link_libraries(xsan_test_storage PRIVATE xsan_storage xsan_disk_manager xsan_volume_manager cunit Threads::Threads ${XSAN_SPDK_LIBRARIES})
# target_include_directories(xsan_test_storage PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/include)
# add_test(NAME XsanStorageTests COMMAND xsan_test_storage)
