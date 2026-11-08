cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pm-image
    VERSION 0.1.0
    DESCRIPTION "PolyMech still-image analysis"
    LANGUAGES CXX)
endif()

# Quality, fingerprint, and the shared thumbnail decode. Resize, transform,
# and metadata stay on the host: they read settings and call providers.
add_library(pm-image-lib STATIC
  src/images/image_quality.cpp
  src/images/image_fingerprint.cpp
  src/images/analyze.cpp
)

target_include_directories(pm-image-lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(pm-image-lib PUBLIC cxx_std_20)
target_compile_definitions(pm-image-lib PUBLIC PM_IMAGE_STATIC=1)

if(MSVC)
  target_compile_definitions(pm-image-lib PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX)
  target_compile_options(pm-image-lib PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-image-lib PRIVATE -Wall -Wextra)
endif()

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
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
  set(_pm_image_commons "${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/cmake/host-layout.cmake")
  if(EXISTS "${_pm_image_commons}")
    include("${_pm_image_commons}")
    pm_commons_add_sibling("${CMAKE_CURRENT_BINARY_DIR}/pm-commons-lib")
    target_link_libraries(pm-image-lib PUBLIC pm-commons-lib)
  endif()
endif()
