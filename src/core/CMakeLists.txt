add_library(xsan_core STATIC
    spdk_manager.c
)

target_include_directories(xsan_core PUBLIC
    # Project's top-level include directory (for xsan.h, xsan_types.h)
    ${CMAKE_SOURCE_DIR}/include
    # Project's src-level include directory (for module headers like xsan_spdk_manager.h)
    ${CMAKE_SOURCE_DIR}/src/include
)

# Dependencies on other XSAN libraries (if any in the future)
# target_link_libraries(xsan_core PUBLIC xsan_common xsan_utils)

# SPDK includes are handled globally if XSAN_ENABLE_SPDK is ON in the root CMakeLists.txt
# No need to add SPDK specific link libraries here for a STATIC library,
# that will be handled by the final executable.
