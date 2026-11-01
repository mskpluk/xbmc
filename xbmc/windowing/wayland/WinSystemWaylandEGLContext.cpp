/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemWaylandEGLContext.h"

#include "Connection.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"

#include <drm_fourcc.h>

#include <EGL/eglext.h>

using namespace KODI::WINDOWING::WAYLAND;
using namespace KODI::WINDOWING::LINUX;

CWinSystemWaylandEGLContext::CWinSystemWaylandEGLContext()
  : CWinSystemEGL{EGL_PLATFORM_WAYLAND_EXT, "EGL_EXT_platform_wayland"}
{}

bool CWinSystemWaylandEGLContext::InitWindowSystemEGL(EGLint renderableType, EGLint apiType)
{
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  CDVDFactoryCodec::ClearHWAccels();
  if (!CWinSystemWaylandImpl::InitWindowSystem())
  {
    return false;
  }

  if (!m_eglContext.CreatePlatformDisplay(GetConnection()->GetDisplay(), GetConnection()->GetDisplay()))
  {
    return false;
  }

  if (!m_eglContext.InitializeDisplay(apiType))
  {
    return false;
  }

  if (!Choose10BitConfig(renderableType) && !m_eglContext.ChooseConfig(renderableType))
  {
    return false;
  }

  return true;
}

bool CWinSystemWaylandEGLContext::Choose10BitConfig(EGLint renderableType)
{
#ifdef TARGET_WEBOS
  // webOS shows video under the GUI, keep the default config with alpha.
  return false;
#else
  // Mesa reports the DRM format as native visual ID. Ask for the opaque
  // formats: 10-bit configs with alpha carry a config caveat.
  for (const EGLint format : {DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010})
  {
    if (m_eglContext.ChooseConfig(renderableType, format, false, 0))
    {
      CLog::Log(LOGINFO, "Using a 10-bit EGL config");
      return true;
    }
  }
  return false;
#endif
}

bool CWinSystemWaylandEGLContext::CreateNewWindow(const std::string& name,
                                                  bool fullScreen,
                                                  RESOLUTION_INFO& res)
{
  if (!CWinSystemWaylandImpl::CreateNewWindow(name, fullScreen, res))
  {
    return false;
  }

  if (!CreateContext())
  {
    return false;
  }

  if (CreatePresenter())
  {
    return true;
  }

  return CreateWindowSurface();
}

bool CWinSystemWaylandEGLContext::CreateWindowSurface()
{
  m_nativeWindow = wayland::egl_window_t{GetMainSurface(), GetBufferSize().Width(), GetBufferSize().Height()};

  // CWinSystemWayland::CreateNewWindow sets internal m_bufferSize
  // to the resolution that should be used for the initial surface size
  // - the compositor might want something other than the resolution given
  if (!m_eglContext.CreatePlatformSurface(
          m_nativeWindow.c_ptr(), reinterpret_cast<khronos_uintptr_t>(m_nativeWindow.c_ptr())))
  {
    return false;
  }

  if (!m_eglContext.BindContext())
  {
    return false;
  }

  // Never enable the vsync of the EGL implementation, we handle that ourselves
  // in WinSystemWayland
  m_eglContext.SetVSync(false);

  return true;
}

bool CWinSystemWaylandEGLContext::DestroyWindow()
{
  m_eglContext.DestroySurface();
  m_nativeWindow = {};

  return CWinSystemWaylandImpl::DestroyWindow();
}

bool CWinSystemWaylandEGLContext::DestroyWindowSystem()
{
  m_eglContext.Destroy();

  return CWinSystemWaylandImpl::DestroyWindowSystem();
}

bool CWinSystemWaylandEGLContext::BindTextureUploadContext()
{
  return m_eglContext.BindTextureUploadContext();
}

bool CWinSystemWaylandEGLContext::UnbindTextureUploadContext()
{
  return m_eglContext.UnbindTextureUploadContext();
}

bool CWinSystemWaylandEGLContext::HasContext()
{
  return m_eglContext.HasContext();
}

CSizeInt CWinSystemWaylandEGLContext::GetNativeWindowAttachedSize()
{
  int width, height;
  m_nativeWindow.get_attached_size(width, height);
  return {width, height};
}

void CWinSystemWaylandEGLContext::SetContextSize(CSizeInt size)
{
  // Change EGL surface size if necessary
  if (GetNativeWindowAttachedSize() != size)
  {
    CLog::LogF(LOGDEBUG, "Updating egl_window size to {}x{}", size.Width(), size.Height());
    m_nativeWindow.resize(size.Width(), size.Height(), 0, 0);
  }
}

void CWinSystemWaylandEGLContext::PresentFrame(bool rendered)
{
  PrepareFramePresentation();

  if (rendered)
  {
    if (!m_eglContext.TrySwapBuffers())
    {
      // For now we just hard fail if this fails
      // Theoretically, EGL_CONTEXT_LOST could be handled, but it needs to be checked
      // whether egl implementations actually use it (mesa does not)
      CEGLUtils::Log(LOGERROR, "eglSwapBuffers failed");
      throw std::runtime_error("eglSwapBuffers failed");
    }
    // eglSwapBuffers() (hopefully) calls commit on the surface and flushes
    // ... well mesa does anyway
  }
  else
  {
    // For presentation feedback: Get notification of the next vblank even
    // when contents did not change
    GetMainSurface().commit();
    // Make sure it reaches the compositor
    GetConnection()->GetDisplay().flush();
  }

  FinishFramePresentation();
}
