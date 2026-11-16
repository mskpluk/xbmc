/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// LinuxRendererGL.h must precede LinuxRendererPLBase.h so that renderer-specific
// types (CPictureBuffer, RenderMethod, EShaderFormat, etc.) are already declared
// when the template body is parsed.
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGL.h"

#include "LinuxRendererPLBase.h"

/**
 * @brief Linux libplacebo renderer using an OpenGL GPU context.
 *
 * Subclasses CLinuxRendererGL via the CLinuxRendererPLBase<> CRTP mixin and intercepts
 * the render hook to pass frames to libplacebo instead of Kodi's YUV shader pipeline.
 *
 * VAAPI hardware-decoded frames are imported zero-copy via vaExportSurfaceHandle
 * (DRM PRIME 2 → per-plane DMA-buf fds → EGLImage → GL_TEXTURE_2D → pl_opengl_wrap).
 * GL_TEXTURE_2D is used (via GL_OES_EGL_image) to avoid the samplerExternalOES
 * restriction in desktop GL 4.6 Core Profile GLSL shaders (Mesa Intel).
 * DRMPRIME frames use CDRMPRIMETexture (DMA-buf → EGLImage → GL_TEXTURE_EXTERNAL_OES).
 * Software-decoded frames are uploaded via pl_upload_plane().
 */
class CLinuxRendererPLGL : public CLinuxRendererPLBase<CLinuxRendererGL>
{
public:
  static CBaseRenderer* Create(CVideoBuffer* buffer);
  static bool Register();

protected:
  GLenum GetVaapiTexTarget() const override { return GL_TEXTURE_2D; }
  EGLDisplay GetDRMPRIMEEGLDisplay() const override;
};
