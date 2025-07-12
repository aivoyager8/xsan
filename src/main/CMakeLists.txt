# Define the main executable
add_executable(xsan_node
    xsan_node.c
    xsan_cli.c  # Assuming this is part of the main executable
)

# Add XSAN include directories
target_include_directories(xsan_node PUBLIC
    ${CMAKE_SOURCE_DIR}/include     # For xsan.h, xsan_types.h
    ${CMAKE_SOURCE_DIR}/src/include # For all module headers (xsan_spdk_manager.h, xsan_bdev.h, etc.)
)

# Link against our XSAN static libraries
target_link_libraries(xsan_node PRIVATE
    xsan_core       # For SPDK management
    xsan_bdev       # For BDEV operations
    xsan_network    # For socket, event_loop, protocol, node_comm
    xsan_storage    # For disk_manager, volume_manager
    xsan_vhost      # For vhost integration
    xsan_common     # For error, version, common data structures
    xsan_utils      # For config, log, memory, string_utils
    Threads::Threads # From find_package(Threads REQUIRED) in root
)

# Conditionally link SPDK libraries and apply SPDK specific settings
if(XSAN_ENABLE_SPDK)
    message(STATUS "Configuring xsan_node target with SPDK support.")
    # XSAN_SPDK_INCLUDE_DIRS are already added globally in the root CMakeLists.txt via include_directories(SYSTEM ...)
    # XSAN_SPDK_CFLAGS are already added globally in the root CMakeLists.txt via add_compile_options(...)

    # Add SPDK library directories for linking (e.g., -L/path/to/spdk/lib)
    # These are from XSAN_SPDK_LIBRARY_DIRS set in the root CMakeLists.txt
    target_link_directories(xsan_node PRIVATE ${XSAN_SPDK_LIBRARY_DIRS})

    # Link against the SPDK libraries (e.g., -lspdk_event -lspdk_bdev ...)
    # These are from XSAN_SPDK_LIBRARIES set in the root CMakeLists.txt
    target_link_libraries(xsan_node PRIVATE ${XSAN_SPDK_LIBRARIES})

    # SPDK (via DPDK) often requires additional system libraries.
    # Many of these should be pulled in by SPDK's .pc files if they are comprehensive.
    # Common ones include: numa, dl, m, rt (already added pthread via Threads::Threads)
    # If not automatically included by SPDK's .pc files, add them here.
    # Check SPDK documentation for full list of dependencies for applications.
    # For example, DPDK might need: -lnuma -ldl -lm -lpthread (-lrt for older systems)
    # We can try adding some common ones if pkg-config doesn't cover them all.
    # However, a well-formed pkg-config setup for SPDK should handle this.
    # Let's add numa explicitly as it's a common one for DPDK.
    # 直接使用主 CMakeLists.txt 检测到的 NUMA 库
    if(XSAN_NUMA_LIBRARIES)
        target_link_libraries(xsan_node PRIVATE ${XSAN_NUMA_LIBRARIES})
        message(STATUS "Linking with NUMA library: ${XSAN_NUMA_LIBRARIES}")
    else()
        message(WARNING "NUMA library not found. SPDK performance might be affected or build might fail if SPDK requires it directly.")
    endif()

    # Other typical dependencies that SPDK's pkg-config might or might not list as transitive.
    # Most modern pkg-config setups for SPDK should handle these.
    # target_link_libraries(xsan_node PRIVATE dl m)


    # If SPDK requires specific C standards or features not enabled globally:
    # target_compile_features(xsan_node PRIVATE c_std_11) # Example if needed

    # If SPDK static libraries require --whole-archive, it's complex with pkg-config.
    # SPDK's pkg-config files for static linking *should* ideally include these flags
    # in LIBS_PRIVATE or similar. If not, manual adjustment is needed.
    # Example:
    # target_link_libraries(xsan_node PRIVATE -Wl,--whole-archive spdk_some_static_lib -Wl,--no-whole-archive)
    # This is highly dependent on the SPDK build and installation.
    # For now, assume pkg-config handles it.

    message(STATUS "xsan_node target configured to link with SPDK.")
else()
    message(STATUS "xsan_node target configured WITHOUT SPDK support.")
endif()

# Installation (optional example)
# install(TARGETS xsan_node DESTINATION bin)
# install(FILES ${CMAKE_SOURCE_DIR}/config/xsan_default.conf DESTINATION etc/xsan)
