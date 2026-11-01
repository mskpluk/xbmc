/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemWaylandEGLContextGL.h"

#include "OptionalsReg.h"
#include "cores/RetroPlayer/process/RPProcessInfo.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererDMAOpenGL.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererFBO.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGL.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGL.h"
#include "rendering/gl/GuiCompositeShaderGL.h"
#include "rendering/gl/ScreenshotSurfaceGL.h"
#include "utils/BufferObjectFactory.h"
#include "utils/DMAHeapBufferObject.h"
#include "utils/GLUtils.h"
#include "utils/UDMABufferObject.h"
#include "utils/log.h"
#include "windowing/WindowSystemFactory.h"

#include <cstdlib>
#include <stdexcept>

#include <EGL/eglext.h>

extern "C"
{
#include <libavutil/pixfmt.h>
}

#if defined(HAVE_VULKAN)
#include "VulkanPresenter.h"
#endif

#if defined(HAVE_NVDEC)
#include "cores/VideoPlayer/DVDCodecs/Video/NVDEC.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererNVDECGL.h"
#endif

using namespace KODI::WINDOWING::WAYLAND;

void CWinSystemWaylandEGLContextGL::Register()
{
  CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "wayland");
}

std::unique_ptr<CWinSystemBase> CWinSystemWaylandEGLContextGL::CreateWinSystem()
{
  return std::make_unique<CWinSystemWaylandEGLContextGL>();
}

bool CWinSystemWaylandEGLContextGL::InitWindowSystem()
{
  if (!CWinSystemWaylandEGLContext::InitWindowSystemEGL(EGL_OPENGL_BIT, EGL_OPENGL_API))
  {
    return false;
  }

  CLinuxRendererGL::Register();
  RETRO::CRPProcessInfo::RegisterRendererFactory(new RETRO::CRendererFactoryDMAOpenGL);
  RETRO::CRPProcessInfo::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGL);
  RETRO::CRPProcessInfo::RegisterRendererFactory(new RETRO::CRendererFactoryFBO);

  bool general, deepColor;
  m_vaapiProxy.reset(WAYLAND::VaapiProxyCreate());
  WAYLAND::VaapiProxyConfig(m_vaapiProxy.get(), GetConnection()->GetDisplay(),
                            m_eglContext.GetEGLDisplay());
  WAYLAND::VAAPIRegisterRenderGL(m_vaapiProxy.get(), general, deepColor);
  if (general)
  {
    WAYLAND::VAAPIRegister(m_vaapiProxy.get(), deepColor);
  }

#if defined(HAVE_NVDEC)
  NVDEC::CDecoder::Register();
  CRendererNVDECGL::Register();
#endif

  CBufferObjectFactory::ClearBufferObjects();
#if defined(HAVE_LINUX_MEMFD) && defined(HAVE_LINUX_UDMABUF)
  CUDMABufferObject::Register();
#endif
#if defined(HAVE_LINUX_DMA_HEAP)
  CDMAHeapBufferObject::Register();
#endif

  CScreenshotSurfaceGL::Register();

  return true;
}

bool CWinSystemWaylandEGLContextGL::CreateContext()
{
  const EGLint glMajor = 3;
  const EGLint glMinor = 2;

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_MAJOR_VERSION_KHR, glMajor},
                      {EGL_CONTEXT_MINOR_VERSION_KHR, glMinor},
                      {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR}});

  if (!m_eglContext.CreateContext(contextAttribs))
  {
    CEGLAttributesVec fallbackContextAttribs;
    fallbackContextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, 2}});

    if (!m_eglContext.CreateContext(fallbackContextAttribs))
    {
      CLog::Log(LOGERROR, "EGL context creation failed");
      return false;
    }
    else
    {
      CLog::Log(LOGWARNING, "Your OpenGL drivers do not support OpenGL {}.{} core profile. Kodi will run in compatibility mode, but performance may suffer.", glMajor, glMinor);
    }
  }

  return true;
}

bool CWinSystemWaylandEGLContextGL::CreatePresenter()
{
#if defined(HAVE_VULKAN)
  // Escape hatch to compare against the plain 8-bit EGL window.
  if (std::getenv("KODI_WAYLAND_EGL_WINDOW"))
    return false;

  if (!m_eglContext.BindContextSurfaceless())
    return false;

  m_presenter.reset(new CVulkanPresenter);
  if (!m_presenter->Create(reinterpret_cast<wl_display*>(GetConnection()->GetDisplay().c_ptr()),
                           reinterpret_cast<wl_surface*>(GetMainSurface().c_ptr()),
                           GetBufferSize().Width(), GetBufferSize().Height()))
  {
    CLog::Log(LOGINFO, "Vulkan presentation unavailable, using an 8-bit EGL window");
    m_presenter.reset();
    return false;
  }

  BindDefaultFramebuffer();
  return true;
#else
  return false;
#endif
}

void CWinSystemWaylandEGLContextGL::BindDefaultFramebuffer()
{
#if defined(HAVE_VULKAN)
  KODI::UTILS::GL::SetDefaultFramebuffer(m_presenter ? m_presenter->GetFramebuffer() : 0);
#endif
  glBindFramebuffer(GL_FRAMEBUFFER, KODI::UTILS::GL::GetDefaultFramebuffer());
}

bool CWinSystemWaylandEGLContextGL::DestroyWindow()
{
  if (m_presenter)
  {
    m_presenter.reset();
    BindDefaultFramebuffer();
  }

  return CWinSystemWaylandEGLContext::DestroyWindow();
}

void CWinSystemWaylandEGLContextGL::SetDirtyRegions(const CDirtyRegionList& dirtyRegions)
{
  if (!m_presenter)
    CWinSystemWaylandEGLContext::SetDirtyRegions(dirtyRegions);
}

int CWinSystemWaylandEGLContextGL::GetBufferAge()
{
  // The presenter keeps no history of buffers: always redraw everything.
  return m_presenter ? 0 : CWinSystemWaylandEGLContext::GetBufferAge();
}

int CWinSystemWaylandEGLContextGL::GetOutputBitDepth() const
{
#if defined(HAVE_VULKAN)
  if (m_presenter)
    return m_presenter->GetBitDepth();
#endif
  return CWinSystemWaylandEGLContext::GetOutputBitDepth();
}

void CWinSystemWaylandEGLContextGL::SetContextSize(CSizeInt size)
{
#if defined(HAVE_VULKAN)
  if (m_presenter)
  {
    if (!m_presenter->Resize(size.Width(), size.Height()))
      CLog::Log(LOGERROR, "Failed to resize Vulkan presentation to {}x{}", size.Width(),
                size.Height());
    BindDefaultFramebuffer();
  }
  else
#endif
  {
    CWinSystemWaylandEGLContext::SetContextSize(size);
  }

  // Propagate changed dimensions to render system if necessary
  if (CRenderSystemGL::m_width != size.Width() || CRenderSystemGL::m_height != size.Height())
  {
    CLog::LogF(LOGDEBUG, "Resetting render system to {}x{}", size.Width(), size.Height());
    CRenderSystemGL::ResetRenderSystem(size.Width(), size.Height());
  }
}

void CWinSystemWaylandEGLContextGL::SetVSyncImpl(bool enable)
{
  // Unsupported
}

void CWinSystemWaylandEGLContextGL::PresentRenderImpl(bool rendered)
{
#if defined(HAVE_VULKAN)
  if (m_presenter)
  {
    PrepareFramePresentation();
    if (rendered)
    {
      m_presenter->Present();
      // Like a failing eglSwapBuffers, there is no way to show frames anymore.
      if (m_presenter->HasFailed())
        throw std::runtime_error("Vulkan presentation failed");
    }
    else
    {
      // For presentation feedback: Get notification of the next vblank even
      // when contents did not change
      GetMainSurface().commit();
      GetConnection()->GetDisplay().flush();
    }
    FinishFramePresentation();
    return;
  }
#endif

  PresentFrame(rendered);
}

bool CWinSystemWaylandEGLContextGL::SetGuiCompositing(int colorTransfer)
{
  // The composite shader only encodes PQ.
  m_guiCompositing = (colorTransfer == AVCOL_TRC_SMPTE2084);

  if (m_guiCompositing)
  {
    if (!m_compositeShader)
    {
      std::string defines;
      if (UseLimitedColor())
        defines += "#define KODI_LIMITED_RANGE 1\n";
      m_compositeShader = std::make_unique<CGuiCompositeShaderGL>(defines);
      if (!m_compositeShader->CompileAndLink())
      {
        CLog::Log(LOGERROR,
                  "CWinSystemWaylandEGLContextGL: failed to compile GUI composite shader");
        m_compositeShader.reset();
        m_guiCompositing = false;
      }
    }
  }
  else
  {
    m_guiFbo.Cleanup();
    m_guiFboWidth = 0;
    m_guiFboHeight = 0;
    m_compositeShader.reset();
  }

  return m_guiCompositing;
}

bool CWinSystemWaylandEGLContextGL::BeginGuiComposite(bool guiWillRender)
{
  if (!m_guiCompositing)
    return false;

  m_guiWillRender = guiWillRender;

  const int width = CRenderSystemGL::m_width;
  const int height = CRenderSystemGL::m_height;

  if (!m_guiFbo.IsValid() || m_guiFboWidth != width || m_guiFboHeight != height)
  {
    m_guiFbo.Cleanup();

    if (!m_guiFbo.Initialize())
    {
      CLog::Log(LOGERROR, "CWinSystemWaylandEGLContextGL: failed to initialize GUI FBO");
      return false;
    }

    if (!m_guiFbo.CreateAndBindToTexture(GL_TEXTURE_2D, width, height, GL_RGBA))
    {
      CLog::Log(LOGERROR, "CWinSystemWaylandEGLContextGL: failed to create GUI FBO texture {}x{}",
                width, height);
      m_guiFbo.Cleanup();
      return false;
    }

    if (GetEnabledFrontToBackRendering() && !m_guiFbo.AttachDepthBuffer(width, height))
    {
      CLog::Log(LOGERROR,
                "CWinSystemWaylandEGLContextGL: failed to attach depth buffer to GUI FBO {}x{}",
                width, height);
      m_guiFbo.Cleanup();
      return false;
    }

    m_guiFboWidth = width;
    m_guiFboHeight = height;
    m_guiFboClean = false; // fresh FBO is undefined, force a clear
    CLog::Log(LOGDEBUG, "CWinSystemWaylandEGLContextGL: created GUI FBO {}x{}", width, height);
  }

  // Nothing draws into the FBO when the GUI render is skipped; its content is kept.
  if (!guiWillRender)
    return true;

  if (!m_guiFbo.BeginRender())
    return false;

  // Clear only when the FBO holds stale content; idle frames are already clean.
  if (!m_guiFboClean)
  {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_guiFboClean = true;
  }

  return true;
}

void CWinSystemWaylandEGLContextGL::EndGuiComposite()
{
  if (m_guiWillRender)
    m_guiFbo.EndRender();

  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

// CompositeGui is the last GL operation in the frame (called just before EndRender).
// GL state (blend mode, active texture, vertex arrays) is not restored afterward;
// the next frame's rendering sets its own state.
void CWinSystemWaylandEGLContextGL::CompositeGui()
{
  if (!m_guiFbo.IsValid() || !m_guiFbo.IsBound() || !m_compositeShader)
    return;

  // Only update m_guiFboClean when GUI render fired this frame; otherwise the
  // FBO is in the same state as the previous frame and the flag stays as-is.
  if (m_guiWillRender)
  {
    const bool guiEmpty = (GetGUIElementCount() == 0);
    m_guiFboClean = guiEmpty;
    if (guiEmpty)
      return;
  }
  else if (m_guiFboClean)
  {
    return;
  }

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_guiFbo.Texture());

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // set up orthographic projection (screen coords, Y-down)
  const float w = static_cast<float>(m_guiFboWidth);
  const float h = static_cast<float>(m_guiFboHeight);

  GLfloat proj[16] = {2.0f / w, 0, 0, 0, 0, -2.0f / h, 0, 0, 0, 0, -1, 0, -1.0f, 1.0f, 0, 1};

  m_compositeShader->SetProjection(proj);
  m_compositeShader->SetSdrPeak(203.0f / 10000.0f);
  m_compositeShader->Enable();

  const GLint posLoc = m_compositeShader->GetPosLoc();
  const GLint texLoc = m_compositeShader->GetTexLoc();

  GLfloat vert[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
  GLfloat tex[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  GLubyte idx[4] = {0, 1, 3, 2};

  GLuint vbo[3];
  glGenBuffers(3, vbo);

  glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vert), vert, GL_STREAM_DRAW);
  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glEnableVertexAttribArray(posLoc);

  glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(tex), tex, GL_STREAM_DRAW);
  glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glEnableVertexAttribArray(texLoc);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo[2]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STREAM_DRAW);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, 0);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(3, vbo);

  m_compositeShader->Disable();
}

void CWinSystemWaylandEGLContextGL::delete_CVulkanPresenter::operator()(CVulkanPresenter* p) const
{
#if defined(HAVE_VULKAN)
  delete p;
#endif
}

void CWinSystemWaylandEGLContextGL::delete_CVaapiProxy::operator()(CVaapiProxy *p) const
{
  WAYLAND::VaapiProxyDelete(p);
}
