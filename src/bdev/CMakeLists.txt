add_library(xsan_bdev STATIC
    xsan_bdev.c
)

target_include_directories(xsan_bdev PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/include
)

# If xsan_bdev.c uses functions from xsan_common (like error handling) or xsan_utils (like logging/memory)
# they should be linked. For a static library, this primarily affects include directories
# and usage if building these modules also as OBJECT libraries or INTERFACE libraries.
# For direct source compilation or linking static libs together, it's mostly about ensuring
# the final executable links everything.
# However, it's good practice to declare usage.
# Since xsan_bdev.c includes xsan_log.h, xsan_memory.h, xsan_error.h, it depends on them.
# These might be part of a general xsan_common_utils library or linked individually.

# Assuming xsan_utils and xsan_common will also be libraries:
# This might not be strictly necessary if they are header-only or if the final executable links all.
# For modularity:
# find_package(xsan_utils REQUIRED) # If they were exportable packages
# find_package(xsan_common REQUIRED)
# For now, let's assume these are available targets from other add_subdirectory calls.
# If xsan_utils and xsan_common are built as static libraries:
# target_link_libraries(xsan_bdev INTERFACE xsan_utils xsan_common)
# Using INTERFACE so that anything linking xsan_bdev also gets includes from utils/common.
# Or PUBLIC if xsan_bdev itself needs to link them (not for static libs usually).

# For simplicity, given our current structure where main links everything,
# we primarily need to ensure include paths are correct.
# The linking dependencies will be resolved in the final executable.
# However, if we were creating shared libraries, this would be more critical.
# No specific link libraries needed here if SPDK is handled by the main executable
# and other xsan parts are also linked by main exe.
# We do need to ensure that targets xsan_utils and xsan_common are defined before this.
# This is handled by add_subdirectory order in root CMakeLists.txt.
