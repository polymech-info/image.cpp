# Windows libvips SDK. Included by the product root and by a standalone
# image.cpp configure. CMAKE_CURRENT_SOURCE_DIR stays the caller's source dir.
if(WIN32)
  if(NOT PM_DIST_ROOT_DIR)
    set(PM_DIST_ROOT_DIR "${CMAKE_SOURCE_DIR}/dist")
  endif()
  file(GLOB _MEDIA_VIPS_SDK
    "${PM_DIST_ROOT_DIR}/vendor/vips-dev-*"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vips-dev-*")
  if(NOT _MEDIA_VIPS_SDK)
    set(_VIPS_VERSION "8.18.7")
    set(_VIPS_ZIP_NAME "vips-dev-x64-all-${_VIPS_VERSION}.zip")
    set(_VIPS_URL "https://github.com/libvips/build-win64-mxe/releases/download/v${_VIPS_VERSION}/${_VIPS_ZIP_NAME}")
    set(_VIPS_ZIP "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_VIPS_ZIP_NAME}")
    message(STATUS "libvips SDK not found in third_party/ — downloading ${_VIPS_URL}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    file(DOWNLOAD "${_VIPS_URL}" "${_VIPS_ZIP}"
         SHOW_PROGRESS
         STATUS _VIPS_DL_STATUS
         TLS_VERIFY ON)
    list(GET _VIPS_DL_STATUS 0 _VIPS_DL_CODE)
    if(NOT _VIPS_DL_CODE EQUAL 0)
      message(FATAL_ERROR "Failed to download libvips SDK: ${_VIPS_DL_STATUS}\n"
              "Run scripts/fetch-vips-windows.ps1 manually or set VIPS_ROOT.")
    endif()
    message(STATUS "Extracting ${_VIPS_ZIP_NAME} ...")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xf "${_VIPS_ZIP}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
      RESULT_VARIABLE _VIPS_EXTRACT_RESULT
    )
    if(NOT _VIPS_EXTRACT_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to extract ${_VIPS_ZIP_NAME}.")
    endif()
    file(GLOB _MEDIA_VIPS_SDK
      "${PM_DIST_ROOT_DIR}/vendor/vips-dev-*"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vips-dev-*")
  endif()
  if(_MEDIA_VIPS_SDK)
    list(GET _MEDIA_VIPS_SDK 0 _MEDIA_VIPS_SDK_FIRST)
    if(MSVC AND EXISTS "${_MEDIA_VIPS_SDK_FIRST}/lib/libvips.def" AND NOT EXISTS "${_MEDIA_VIPS_SDK_FIRST}/lib/libvips.lib")
      file(GLOB _MEDIA_VIPS_DEF_FILES "${_MEDIA_VIPS_SDK_FIRST}/lib/*.def")
      foreach(_MEDIA_VIPS_DEF IN LISTS _MEDIA_VIPS_DEF_FILES)
        get_filename_component(_MEDIA_VIPS_IMPORT_STEM "${_MEDIA_VIPS_DEF}" NAME_WE)
        set(_MEDIA_VIPS_IMPORT_LIB "${_MEDIA_VIPS_SDK_FIRST}/lib/${_MEDIA_VIPS_IMPORT_STEM}.lib")
        if(NOT EXISTS "${_MEDIA_VIPS_IMPORT_LIB}")
          execute_process(
            COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${_MEDIA_VIPS_DEF}" "/out:${_MEDIA_VIPS_IMPORT_LIB}"
            RESULT_VARIABLE _MEDIA_VIPS_LIB_RESULT
            OUTPUT_QUIET
            ERROR_VARIABLE _MEDIA_VIPS_LIB_ERROR
          )
          if(NOT _MEDIA_VIPS_LIB_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to generate ${_MEDIA_VIPS_IMPORT_LIB} from ${_MEDIA_VIPS_DEF}: ${_MEDIA_VIPS_LIB_ERROR}")
          endif()
        endif()
      endforeach()
    endif()
    list(APPEND CMAKE_PREFIX_PATH ${_MEDIA_VIPS_SDK})
  endif()
endif()
