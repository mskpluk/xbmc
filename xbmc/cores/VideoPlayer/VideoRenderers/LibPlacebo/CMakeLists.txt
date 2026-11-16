if (NOT TARGET ${APP_NAME_LC}::Libplacebo)
    return()
endif ()

set(SOURCES libav_wrapper.c
        PLHelper.cpp)

set(HEADERS
        PLHelper.h)

if (NOT (WIN32 OR WINDOWS_STORE))
    if (TARGET ${APP_NAME_LC}::OpenGl OR TARGET ${APP_NAME_LC}::OpenGLES)
        list(APPEND SOURCES LinuxPLInstanceGL.cpp)
        list(APPEND HEADERS LinuxRendererPLBase.h)
        if (TARGET ${APP_NAME_LC}::OpenGl)
            list(APPEND SOURCES LinuxRendererPLGL.cpp)
            list(APPEND HEADERS LinuxRendererPLGL.h)
        endif ()
        if (TARGET ${APP_NAME_LC}::OpenGLES)
            list(APPEND SOURCES LinuxRendererPLGLES.cpp)
            list(APPEND HEADERS LinuxRendererPLGLES.h)
        endif ()
    endif ()
endif ()

core_add_library(videorenderers-libplacebo)

if (TARGET ${APP_NAME_LC}::VAAPI)
    target_link_libraries(videorenderers-libplacebo PRIVATE ${APP_NAME_LC}::VAAPI)
endif ()
