add_library(xsan_network STATIC
    socket.c          # Original socket wrappers (might be deprecated/refactored if using spdk_sock fully)
    event_loop.c      # Original event loop (might be deprecated/refactored)
    protocol.c        # Message serialization/deserialization
    xsan_node_comm.c  # New SPDK sock based communication
)

target_include_directories(xsan_network PUBLIC
    # Project's top-level include directory (for xsan.h, xsan_types.h)
    ${CMAKE_SOURCE_DIR}/include
    # Project's src-level include directory (for module headers like xsan_socket.h)
    ${CMAKE_SOURCE_DIR}/src/include
)

# Dependencies on other XSAN libraries (e.g., common for error codes, utils for logging/memory)
# For static libraries, these are more about ensuring correct include paths are propagated
# if these INTERFACE properties are used. The final linking is done at the executable.
# target_link_libraries(xsan_network INTERFACE xsan_common xsan_utils)

# SPDK includes are handled globally if XSAN_ENABLE_SPDK is ON in the root CMakeLists.txt.
# No need to add SPDK specific link libraries here for a STATIC library.
# That will be handled by the final executable that uses SPDK functionalities.
