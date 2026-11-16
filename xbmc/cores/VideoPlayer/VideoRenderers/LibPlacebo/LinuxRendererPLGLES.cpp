/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "LinuxRendererPLGLES.h"

#include "PLHelper.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/log.h"
#include "windowing/linux/WinSystemEGL.h"

#if defined(HAVE_LIBVA)
#include "cores/VideoPlayer/DVDCodecs/Video/VAAPI.h"
#endif

#include <libplacebo/utils/libav.h>

#include <EGL/egl.h>

// ---------------------------------------------------------------------------
// Static factory
// ---------------------------------------------------------------------------

CBaseRenderer* CLinuxRendererPLGLES::Create(CVideoBuffer* buffer)
{
  // buffer is null during PreInit (before the codec is open). Accept it —
  // the buffer format will be validated later in Configure().
  if (buffer)
  {
    bool isDRMPRIME = (dynamic_cast<CVideoBufferDRMPRIME*>(buffer) != nullptr);
#if defined(HAVE_LIBVA)
    bool isVAAPI = (dynamic_cast<VAAPI::CVaapiRenderPicture*>(buffer) != nullptr);
#else
    constexpr bool isVAAPI = false;
#endif
    pl_bit_encoding bits{};
    pl_plane_data pdata[4]{};
    bool isSW = (pl_plane_data_from_pixfmt(pdata, &bits, buffer->GetFormat()) > 0);

    if (!isDRMPRIME && !isVAAPI && !isSW)
    {
      CLog::Log(
          LOGDEBUG,
          "CLinuxRendererPLGLES::Create - buffer format {} not supported by libplacebo, skipping",
          static_cast<int>(buffer->GetFormat()));
      return nullptr;
    }
  }

  auto* inst = PL::PLInstance::Get().get();
  if (!inst->Init())
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLGLES::Create - PLInstance::Init() failed");
    return nullptr;
  }

  return new CLinuxRendererPLGLES();
}

bool CLinuxRendererPLGLES::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("libplacebo", CLinuxRendererPLGLES::Create);
  return true;
}

// ---------------------------------------------------------------------------
// Platform-specific hooks
// ---------------------------------------------------------------------------

EGLDisplay CLinuxRendererPLGLES::GetDRMPRIMEEGLDisplay() const
{
  // On GLES/EGL platforms, retrieve the display via the windowing system rather
  // than eglGetCurrentDisplay() to ensure the correct display is used on
  // multi-display setups (e.g. GBM/RPi where eglGetCurrentDisplay may differ).
  auto* winEGL =
      dynamic_cast<KODI::WINDOWING::LINUX::CWinSystemEGL*>(CServiceBroker::GetWinSystem());
  return winEGL ? winEGL->GetEGLDisplay() : EGL_NO_DISPLAY;
}
