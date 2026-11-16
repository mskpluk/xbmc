#.rst:
# FindLibplacebo
# --------
# Finds the libplacebo library
#
# This will define the following target:
#
#   ${APP_NAME_LC}::Libplacebo   - The libplacebo library

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})

  include(cmake/scripts/common/ModuleHelpers.cmake)

  macro(buildmacroLibplacebo)

    find_package(Meson REQUIRED)
    find_package(Ninja REQUIRED)

    if(WIN32 OR WINDOWS_STORE)
      set(${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_SHARED_LIB 1)

      if(WINDOWS_STORE)
        set(${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_EXE_LINKER_FLAGS "/APPCONTAINER windowsapp.lib")
      endif()

      create_module_dev_env()
    endif()

    # generate meson cross file for build target
    generate_mesoncrossfile()

    if(EXISTS ${DEPENDS_PATH}/share/${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}-cross-file.meson)
      set(${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}_CROSS_FILE --cross-file=${DEPENDS_PATH}/share/${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}-cross-file.meson)
    elseif(EXISTS ${DEPENDS_PATH}/share/cross-file.meson)
      set(${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}_CROSS_FILE --cross-file=${DEPENDS_PATH}/share/cross-file.meson)
    endif()

    set(CONFIGURE_COMMAND ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_dev_env}
                          ${CMAKE_COMMAND} -E env --modify NINJA=set:${NINJA_EXECUTABLE}
                          ${MESON_EXECUTABLE} setup ./build
                          --buildtype=release
                          --default-library=static
                          --prefix=${DEPENDS_PATH}
                          --libdir=lib
                          -Dtests=false
                          -Ddemos=false
                          -Dvulkan=enabled
                          -Dopengl=enabled
                          -Dd3d11=disabled
                          ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}_CROSS_FILE})

    if(WIN32 OR WINDOWS_STORE)
      set(CONFIGURE_COMMAND ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_dev_env}
                            ${CMAKE_COMMAND} -E env --modify NINJA=set:${NINJA_EXECUTABLE}
                            ${MESON_EXECUTABLE} setup ./build
                            --buildtype=release
                            --default-library=shared
                            --prefix=${DEPENDS_PATH}
                            --libdir=lib
                            -Dtests=false
                            -Ddemos=false
                            -Dvulkan=disabled
                            -Dopengl=disabled
                            -Dd3d11=enabled
                            ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}_CROSS_FILE})
    endif()

    set(BUILD_COMMAND ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_dev_env}
                      ${NINJA_EXECUTABLE} -C ./build)
    set(INSTALL_COMMAND ${NINJA_EXECUTABLE} -C ./build install)
    set(BUILD_IN_SOURCE 1)

    BUILD_DEP_TARGET()

  endmacro()

  set(${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC libplacebo)

  SETUP_BUILD_VARS()

  SETUP_FIND_SPECS()

  SEARCH_EXISTING_PACKAGES()

  if((${${CMAKE_FIND_PACKAGE_NAME}_SEARCH_NAME}_VERSION VERSION_LESS ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_VER} AND ENABLE_INTERNAL_LIBPLACEBO) OR
     (((CORE_SYSTEM_NAME STREQUAL linux AND NOT "webos" IN_LIST CORE_PLATFORM_NAME_LC) OR CORE_SYSTEM_NAME STREQUAL freebsd) AND ENABLE_INTERNAL_LIBPLACEBO))
    message(STATUS "Building ${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}: \(version \"${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_VER}\"\)")

    cmake_language(EVAL CODE "
      buildmacro${CMAKE_FIND_PACKAGE_NAME}()
    ")

    if(TARGET libplacebo::libplacebo)
      if(EXISTS "${DEPENDS_PATH}/lib/cmake/libplacebo")
        file(REMOVE_RECURSE "${DEPENDS_PATH}/lib/cmake/libplacebo")
      endif()
    endif()
  endif()

  if(${${CMAKE_FIND_PACKAGE_NAME}_SEARCH_NAME}_FOUND)
    if(TARGET libplacebo::libplacebo AND NOT TARGET ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_BUILD_NAME})
      add_library(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} ALIAS libplacebo::libplacebo)
    elseif(TARGET PkgConfig::${${CMAKE_FIND_PACKAGE_NAME}_SEARCH_NAME} AND NOT TARGET ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_BUILD_NAME})
      add_library(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} ALIAS PkgConfig::${${CMAKE_FIND_PACKAGE_NAME}_SEARCH_NAME})
    else()
      SETUP_BUILD_TARGET()

      add_dependencies(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_BUILD_NAME})

      set_target_properties(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} PROPERTIES LIB_BUILD ON)
    endif()

    # Signal that libplacebo is available on this platform (non-Windows uses OpenGL backend)
    if(NOT (WIN32 OR WINDOWS_STORE))
      add_compile_definitions(HAS_LIBPLACEBO)
    endif()

    ADD_MULTICONFIG_BUILDMACRO()
  endif()
endif()
