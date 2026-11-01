/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "WinSystemWaylandEGLContext.h"
#include "cores/VideoPlayer/VideoRenderers/FrameBufferObject.h"
#include "rendering/gl/RenderSystemGL.h"

#include <memory>

class CGuiCompositeShaderGL;

namespace KODI
{
namespace WINDOWING
{
namespace WAYLAND
{

class CVaapiProxy;

class CWinSystemWaylandEGLContextGL : public CWinSystemWaylandEGLContext, public CRenderSystemGL
{
public:
  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemWaylandEGLContext
  CRenderSystemBase *GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;

  // GUI compositing for HDR (FBO + sRGB->PQ shader)
  bool SetGuiCompositing(int colorTransfer) override;
  bool BeginGuiComposite(bool guiWillRender) override;
  void EndGuiComposite() override;
  void CompositeGui() override;
  bool IsHdrComposite() const override { return m_guiCompositing; }

protected:
  bool CreateContext() override;
  void SetContextSize(CSizeInt size) override;
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override;
  struct delete_CVaapiProxy
  {
    void operator()(CVaapiProxy *p) const;
  };
  std::unique_ptr<CVaapiProxy, delete_CVaapiProxy> m_vaapiProxy;

private:
  bool m_guiCompositing{false};
  CFrameBufferObject m_guiFbo;
  int m_guiFboWidth{0};
  int m_guiFboHeight{0};
  // True when the GUI FBO is empty (no draws this frame); CompositeGui skips composite when true.
  bool m_guiFboClean{false};
  // Whether the GUI render pass will run this frame; set by BeginGuiComposite.
  bool m_guiWillRender{true};
  std::unique_ptr<CGuiCompositeShaderGL> m_compositeShader;
};

}
}
}
