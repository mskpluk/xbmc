/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// LinuxRendererGLES.h must precede LinuxRendererPLBase.h so that renderer-specific
// types (CPictureBuffer, RenderMethod, EShaderFormat, etc.) are already declared
// when the template body is parsed.
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"

#include "LinuxRendererPLBase.h"

/**
 * @brief Linux libplacebo renderer using an OpenGL ES GPU context.
 *
 * Subclasses CLinuxRendererGLES via the CLinuxRendererPLBase<> CRTP mixin and intercepts
 * the render hook to pass frames to libplacebo instead of Kodi's YUV shader pipeline.
 *
 * VAAPI hardware-decoded frames are imported zero-copy via vaExportSurfaceHandle
 * (DRM PRIME 2 → per-plane DMA-buf fds → EGLImage → GL_TEXTURE_EXTERNAL_OES →
 * pl_opengl_wrap). DRMPRIME frames use CDRMPRIMETexture (DMA-buf → EGLImage →
 * GL_TEXTURE_EXTERNAL_OES). Software-decoded frames fall back to pl_upload_plane().
 */
class CLinuxRendererPLGLES : public CLinuxRendererPLBase<CLinuxRendererGLES>
{
public:
  static CBaseRenderer* Create(CVideoBuffer* buffer);
  static bool Register();

protected:
  GLenum GetVaapiTexTarget() const override { return GL_TEXTURE_EXTERNAL_OES; }
  EGLDisplay GetDRMPRIMEEGLDisplay() const override;
};
