/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PLHelper.h"
#include "ServiceBroker.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"
#include "windowing/linux/WinSystemEGL.h"

#include <libplacebo/opengl.h>

#include <EGL/egl.h>

bool PL::PLInstance::InitGpu()
{
  // On GBM (RPi5), eglGetCurrentDisplay/Context() can return EGL_NO_*
  // because the platform display is obtained via eglGetPlatformDisplay()
  // and with DRMPRIME direct-to-plane the GLES context may never be made
  // current on this thread.  Pull display and context from the windowing
  // system directly, falling back to the EGL thread-local getters.
  EGLDisplay eglDpy = EGL_NO_DISPLAY;
  EGLContext eglCtx = EGL_NO_CONTEXT;
  auto* winEGL =
      dynamic_cast<KODI::WINDOWING::LINUX::CWinSystemEGL*>(CServiceBroker::GetWinSystem());
  if (winEGL)
  {
    eglDpy = winEGL->GetEGLDisplay();
    eglCtx = winEGL->GetEGLContext();
  }
  if (eglDpy == EGL_NO_DISPLAY)
    eglDpy = eglGetCurrentDisplay();
  if (eglCtx == EGL_NO_CONTEXT)
    eglCtx = eglGetCurrentContext();

  // If the context is not current on this thread, make it current now.
  // libplacebo calls eglGetCurrentContext() internally even when egl_context
  // is provided in params, so the context must be bound to the calling thread.
  // This is safe: on GBM the context is surfaceless and not held by any
  // thread between render frames, so eglMakeCurrent will succeed.
  const bool needMakeCurrent = (eglCtx != EGL_NO_CONTEXT && eglGetCurrentContext() != eglCtx);
  if (needMakeCurrent)
  {
    if (!eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, eglCtx))
      CLog::Log(LOGWARNING, "PLInstance::InitGpu - eglMakeCurrent failed: 0x{:x}", eglGetError());
  }

  pl_opengl_params gl_params = pl_opengl_default_params;
  gl_params.egl_display = eglDpy;
  gl_params.egl_context = eglCtx;
  // Explicitly provide eglGetProcAddress so libplacebo uses EGL's function
  // loader rather than its own internal logic (which may fail to load core
  // GLES functions on some platforms, e.g. Broadcom V3D on RPi5).
  gl_params.get_proc_addr = reinterpret_cast<pl_voidfunc_t (*)(const char*)>(eglGetProcAddress);
  pl_opengl plGl = pl_opengl_create(m_plLog, &gl_params);
  if (!plGl)
  {
    CLog::Log(LOGERROR, "PLInstance::InitGpu - failed to create libplacebo OpenGL context");
    return false;
  }
  m_plGpu = plGl->gpu;
  m_nativeGpuHandle = const_cast<pl_opengl_t*>(plGl);
  return true;
}

void PL::PLInstance::DestroyGpu()
{
  if (m_nativeGpuHandle)
  {
    auto plGl = static_cast<pl_opengl>(m_nativeGpuHandle);
    pl_opengl_destroy(&plGl);
    m_nativeGpuHandle = nullptr;
  }
  m_plGpu = nullptr;
}
