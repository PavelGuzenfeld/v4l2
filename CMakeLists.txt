cmake_minimum_required(VERSION 3.20)
project(v4l2)

# Require C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# default to release build for the library if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type" FORCE)
endif()
message(STATUS "Library will be built using CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

# Find dependencies
find_package(cmake-library REQUIRED)
find_package(fmt REQUIRED)
find_package(exception-rt REQUIRED)

# Runner library
add_library(${PROJECT_NAME} STATIC src/v4l2.cpp)

# Ensure PIC is enabled for this target.
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set_release_options(${PROJECT_NAME})
endif()

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(${PROJECT_NAME} PRIVATE fmt exception-rt::exception-rt)

# Install public headers so that the INSTALL_INTERFACE path exists.
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION include
)

# Export the target
install(
    TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    DESTINATION lib
    INCLUDES DESTINATION include
)

# Install the export file
install(
    EXPORT ${PROJECT_NAME}Targets
    FILE ${PROJECT_NAME}Targets.cmake
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION lib/cmake/${PROJECT_NAME}
)

# Configure package files
include(CMakePackageConfigHelpers)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${PROJECT_NAME}Config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  INSTALL_DESTINATION lib/cmake/${PROJECT_NAME}
  NO_SET_AND_CHECK_MACRO
  NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

# Install the package configuration files
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  DESTINATION lib/cmake/${PROJECT_NAME}
)


# Add tests
add_executable(${PROJECT_NAME}-test test/v4l2_test.cpp)
target_link_libraries(${PROJECT_NAME}-test PRIVATE ${PROJECT_NAME} fmt exception-rt::exception-rt z)

set_warnings_and_errors(${PROJECT_NAME}-test)
enable_sanitizers(${PROJECT_NAME}-test)

# install the executable
install(
    TARGETS ${PROJECT_NAME}-test
    RUNTIME DESTINATION bin
)

# Add tests
add_executable(${PROJECT_NAME}-performance_test test/v4l2_performance_test.cpp)
target_link_libraries(${PROJECT_NAME}-performance_test PRIVATE ${PROJECT_NAME} fmt exception-rt::exception-rt z)

set_warnings_and_errors(${PROJECT_NAME}-performance_test)
set_release_options(${PROJECT_NAME}-performance_test)

# install the executable
install(
    TARGETS ${PROJECT_NAME}-performance_test
    RUNTIME DESTINATION bin
)

# Add demo
add_executable(${PROJECT_NAME}-demo src/v4l2_demo.cpp)
target_link_libraries(${PROJECT_NAME}-demo PRIVATE ${PROJECT_NAME} fmt)

set_warnings_and_errors(${PROJECT_NAME}-demo)
set_release_options(${PROJECT_NAME}-demo)

# install the executable
install(
    TARGETS ${PROJECT_NAME}-demo
    RUNTIME DESTINATION bin
)

# Handle "Manually-specified variables were not used by the project" warning.
unset(CATKIN_INSTALL_INTO_PREFIX_ROOT)
unset(CATKIN_SYMLINK_INSTALL)


#
# ─── PART I: FIND GSTREAMER ──────────────────────────────────────────────────────
#
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
  gstreamer-1.0>=1.2
  gstreamer-base-1.0>=1.2
  gstreamer-video-1.0>=1.2
)

#
# ─── PART II: BUILD THE PLUGIN ───────────────────────────────────────────────────
#
add_library(v4l2-src MODULE src/v4l2-src.cpp)

set_warnings_and_errors(v4l2-src)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_release_options(v4l2-src)
endif()

target_include_directories(v4l2-src
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE SYSTEM
    ${GST_INCLUDE_DIRS}
)

# Link against GStreamer and your v4l2 static lib:
target_link_libraries(v4l2-src
  PRIVATE
    exception-rt::exception-rt
    ${GST_LIBRARIES}
    ${GSTVIDEO_LIBRARIES}   # from pkg_check_modules(gstreamer-video-1.0)
    ${PROJECT_NAME}         # v4l2
)

# On some platforms you may need to suppress the “.so” -> “.so.0” versioning
set_target_properties(v4l2-src PROPERTIES
  PREFIX ""               # so it becomes libgstrocxv4l2src.so
)

#
# ─── PART III: INSTALL INTO GSTREAMER’S PLUGIN FOLDER ────────────────────────────
#
# Use GNUInstallDirs for portable libdir (lib or lib64)
include(GNUInstallDirs)  # gives you ${CMAKE_INSTALL_LIBDIR}
# 🧠 for colcon: strip our package name from the install prefix
get_filename_component(COLCON_INSTALL_BASE "${CMAKE_INSTALL_PREFIX}" DIRECTORY)

set(GST_PLUGIN_INSTALL_DIR "${COLCON_INSTALL_BASE}/lib/gstreamer-1.0"
    CACHE PATH "GStreamer plugin install directory" FORCE)

install(
    TARGETS v4l2-src
    LIBRARY DESTINATION ${GST_PLUGIN_INSTALL_DIR}
)
