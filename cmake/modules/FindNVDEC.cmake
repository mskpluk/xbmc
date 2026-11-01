#.rst:
# FindNVDEC
# ---------
# Finds the ffnvcodec headers used to load CUDA/NVDEC from the NVIDIA driver at
# runtime (no CUDA toolkit or link-time dependency).
#
# This will define the following target:
#
#   ${APP_NAME_LC}::NVDEC - interface target with HAVE_NVDEC

if(NOT TARGET ${APP_NAME_LC}::NVDEC)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_FFNVCODEC ffnvcodec QUIET)
  endif()

  find_path(FFNVCODEC_INCLUDE_DIR NAMES ffnvcodec/dynlink_loader.h
                                  HINTS ${PC_FFNVCODEC_INCLUDEDIR})

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(NVDEC REQUIRED_VARS FFNVCODEC_INCLUDE_DIR)

  if(NVDEC_FOUND)
    add_library(${APP_NAME_LC}::NVDEC INTERFACE IMPORTED GLOBAL)
    set_target_properties(${APP_NAME_LC}::NVDEC PROPERTIES
                          INTERFACE_INCLUDE_DIRECTORIES "${FFNVCODEC_INCLUDE_DIR}"
                          INTERFACE_COMPILE_DEFINITIONS HAVE_NVDEC)
    # The define is also needed by the windowing code that registers the decoder.
    add_compile_definitions(HAVE_NVDEC)
    include_directories(SYSTEM ${FFNVCODEC_INCLUDE_DIR})
  endif()

  mark_as_advanced(FFNVCODEC_INCLUDE_DIR)
endif()
