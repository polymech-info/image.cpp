#pragma once

// Empty when this static lib is linked into the host and into siblings.
// A shared pm-media build then keeps one copy of the symbols per module
// that links pm-image-lib, without dllimport on the header.
#if defined(PM_IMAGE_STATIC)
#  define PM_IMAGE_API
#elif defined(PM_IMAGE_BUILD_SHARED)
#  if defined(_WIN32)
#    define PM_IMAGE_API __declspec(dllexport)
#  else
#    define PM_IMAGE_API __attribute__((visibility("default")))
#  endif
#elif defined(PM_IMAGE_SHARED)
#  if defined(_WIN32)
#    define PM_IMAGE_API __declspec(dllimport)
#  else
#    define PM_IMAGE_API __attribute__((visibility("default")))
#  endif
#elif defined(POLYMECH_API)
#  define PM_IMAGE_API POLYMECH_API
#else
#  include "polymech_export.h"
#  define PM_IMAGE_API POLYMECH_API
#endif
