cmake_minimum_required(VERSION 3.20)

set(_pm_image_standalone FALSE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_pm_image_standalone TRUE)
  project(pm-image
    VERSION 0.1.0
    DESCRIPTION "PolyMech still-image analysis"
    LANGUAGES CXX)
endif()
include(GNUInstallDirs)

# Quality, fingerprint, and the shared thumbnail decode. Resize, transform,
# and metadata stay on the host: they read settings and call providers.
if(_pm_image_standalone)
  add_library(pm-image-lib SHARED
    src/images/image_quality.cpp
    src/images/image_fingerprint.cpp
    src/images/analyze.cpp)
  set_target_properties(pm-image-lib PROPERTIES
    OUTPUT_NAME pm-image
    EXPORT_NAME image)
else()
  add_library(pm-image-lib STATIC
    src/images/image_quality.cpp
    src/images/image_fingerprint.cpp
    src/images/analyze.cpp)
endif()
add_library(pm::image ALIAS pm-image-lib)

target_include_directories(pm-image-lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(pm-image-lib PUBLIC cxx_std_20)

if(_pm_image_standalone)
  target_compile_definitions(pm-image-lib
    PRIVATE PM_IMAGE_BUILD_SHARED=1
    INTERFACE PM_IMAGE_SHARED=1)
else()
  target_compile_definitions(pm-image-lib PUBLIC PM_IMAGE_STATIC=1)
endif()

if(MSVC)
  target_compile_definitions(pm-image-lib PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX)
  target_compile_options(pm-image-lib PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-image-lib PRIVATE -Wall -Wextra)
endif()

if(NOT _pm_image_standalone)
  if(TARGET pm-commons-lib)
    target_link_libraries(pm-image-lib PUBLIC pm-commons-lib)
  endif()
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(pm-image-lib PUBLIC nlohmann_json::nlohmann_json)
  endif()
  if(COMMAND pm_link_vips)
    pm_link_vips(pm-image-lib)
  endif()
  if(COMMAND pm_vips_includes)
    pm_vips_includes("${CMAKE_CURRENT_SOURCE_DIR}/src/images/image_quality.cpp")
  endif()
  # libvips calls glib. The host links these next to pm-media; this lib is
  # also pulled into the exes, which do not inherit that private link.
  if(WIN32)
    file(GLOB _pm_image_vips_sdk
      "${CMAKE_CURRENT_SOURCE_DIR}/../../dist/vendor/vips-dev-*"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/vips-dev-*")
    if(_pm_image_vips_sdk)
      list(GET _pm_image_vips_sdk 0 _pm_image_vips_sdk)
      if(EXISTS "${_pm_image_vips_sdk}/lib/libgobject-2.0.lib")
        target_link_libraries(pm-image-lib PUBLIC
          "${_pm_image_vips_sdk}/lib/libgobject-2.0.lib"
          "${_pm_image_vips_sdk}/lib/libglib-2.0.lib")
      endif()
    endif()
  endif()
else()
  include(FetchContent)
  set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
  set(JSON_Install ON CACHE BOOL "" FORCE)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
  target_include_directories(pm-image-lib PUBLIC
    $<BUILD_INTERFACE:${nlohmann_json_SOURCE_DIR}/include>)

  if(NOT TARGET pm::commons)
    if(TARGET pm-commons-lib)
      add_library(pm::commons ALIAS pm-commons-lib)
    else()
      find_package(pm-commons CONFIG REQUIRED)
    endif()
  endif()
  target_link_libraries(pm-image-lib PRIVATE pm::commons)

  if(NOT COMMAND pm_link_vips)
    set(_pm_image_root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    file(GLOB _pm_image_vips_sdk
      "${_pm_image_root}/dist/vendor/vips-dev-*"
      "${_pm_image_root}/third_party/vips-dev-*")
    if(_pm_image_vips_sdk)
      list(APPEND CMAKE_PREFIX_PATH ${_pm_image_vips_sdk})
    endif()
    if(NOT EXISTS "${_pm_image_root}/cmake/FindVips.cmake")
      message(FATAL_ERROR "pm-image needs FindVips.cmake or an existing Vips::vips target")
    endif()
    include("${_pm_image_root}/cmake/FindVips.cmake")
  endif()
  if(NOT TARGET Vips::vips)
    message(FATAL_ERROR "pm-image did not find libvips. Set VIPS_ROOT or install the vips development package.")
  endif()
  pm_link_vips(pm-image-lib)
  pm_vips_includes("${CMAKE_CURRENT_SOURCE_DIR}/src/images/image_quality.cpp")
  # pm_vips_includes uses /external:I. A standalone cl.exe drops those flags
  # unless /external:W is also set, so the quality TU also gets a normal -I.
  set(_pm_vips_real Vips::vips)
  get_target_property(_pm_vips_aliased Vips::vips ALIASED_TARGET)
  if(_pm_vips_aliased)
    set(_pm_vips_real "${_pm_vips_aliased}")
  endif()
  get_target_property(_pm_vips_inc "${_pm_vips_real}" INTERFACE_INCLUDE_DIRECTORIES)
  if(_pm_vips_inc)
    target_include_directories(pm-image-lib PRIVATE ${_pm_vips_inc})
  endif()
  if(WIN32)
    file(GLOB _pm_image_vips_sdk
      "${CMAKE_CURRENT_SOURCE_DIR}/../../dist/vendor/vips-dev-*"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/vips-dev-*")
    if(_pm_image_vips_sdk)
      list(GET _pm_image_vips_sdk 0 _pm_image_vips_sdk)
      if(EXISTS "${_pm_image_vips_sdk}/lib/libgobject-2.0.lib")
        target_link_libraries(pm-image-lib PRIVATE
          "${_pm_image_vips_sdk}/lib/libgobject-2.0.lib"
          "${_pm_image_vips_sdk}/lib/libglib-2.0.lib")
      endif()
    endif()
  endif()

  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  install(TARGETS pm-image-lib
    EXPORT pm-image-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(FILES
    src/images/export.hpp
    src/images/image_quality.hpp
    src/images/image_fingerprint.hpp
    src/images/analyze.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/images)
  install(EXPORT pm-image-targets
    FILE pm-image-targets.cmake
    NAMESPACE pm::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pm-image)
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pm-image-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pm-image-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pm-image)
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/pm-image-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/pm-image-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/pm-image-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pm-image)
  set(PM_IMAGE_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(PM_IMAGE_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
  set(PM_IMAGE_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pm-image.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pm-image.pc"
    @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pm-image.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()
