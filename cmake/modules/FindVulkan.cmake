#.rst:
# FindVulkan
# ----------
# Finds the Vulkan loader and headers.
#
# This will define the following target:
#
#   ${APP_NAME_LC}::Vulkan - The Vulkan library

if(NOT TARGET ${APP_NAME_LC}::Vulkan)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_VULKAN vulkan QUIET)
  endif()

  find_path(VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h
                               HINTS ${PC_VULKAN_INCLUDEDIR})
  find_library(VULKAN_LIBRARY NAMES vulkan
                              HINTS ${PC_VULKAN_LIBDIR})

  set(VULKAN_VERSION ${PC_VULKAN_VERSION})

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(Vulkan
                                    REQUIRED_VARS VULKAN_LIBRARY VULKAN_INCLUDE_DIR
                                    VERSION_VAR VULKAN_VERSION)

  if(VULKAN_FOUND)
    add_library(${APP_NAME_LC}::Vulkan UNKNOWN IMPORTED GLOBAL)
    set_target_properties(${APP_NAME_LC}::Vulkan PROPERTIES
                                                 IMPORTED_LOCATION "${VULKAN_LIBRARY}"
                                                 INTERFACE_INCLUDE_DIRECTORIES "${VULKAN_INCLUDE_DIR}"
                                                 INTERFACE_COMPILE_DEFINITIONS HAVE_VULKAN)
    # The define is also needed by the windowing code that uses the presenter.
    add_compile_definitions(HAVE_VULKAN)
  endif()

  mark_as_advanced(VULKAN_INCLUDE_DIR VULKAN_LIBRARY)
endif()
