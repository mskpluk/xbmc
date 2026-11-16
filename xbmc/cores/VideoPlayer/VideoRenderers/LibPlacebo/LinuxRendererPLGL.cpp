/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "LinuxRendererPLGL.h"

#include "PLHelper.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/log.h"

#if defined(HAVE_LIBVA)
#include "cores/VideoPlayer/DVDCodecs/Video/VAAPI.h"
#endif

#include <libplacebo/utils/libav.h>

#include <EGL/egl.h>

// ---------------------------------------------------------------------------
// Static factory
// ---------------------------------------------------------------------------

CBaseRenderer* CLinuxRendererPLGL::Create(CVideoBuffer* buffer)
{
  // buffer is null during PreInit (before the codec is open). Accept it —
  // the buffer format will be validated later in Configure().
  if (buffer)
  {
    pl_bit_encoding bits{};
    pl_plane_data pdata[4]{};
    bool isSW = (pl_plane_data_from_pixfmt(pdata, &bits, buffer->GetFormat()) > 0);
    bool isDRMPRIME = (dynamic_cast<CVideoBufferDRMPRIME*>(buffer) != nullptr);
#if defined(HAVE_NVDEC)
    isSW = isSW || dynamic_cast<NVDEC::CVideoBufferNVDEC*>(buffer) != nullptr;
#endif
#if defined(HAVE_LIBVA)
    bool isVAAPI = (dynamic_cast<VAAPI::CVaapiRenderPicture*>(buffer) != nullptr);
#else
    constexpr bool isVAAPI = false;
#endif

    if (!isSW && !isVAAPI && !isDRMPRIME)
    {
      CLog::Log(LOGDEBUG,
                "CLinuxRendererPLGL::Create - unsupported buffer type (format {}), skipping",
                static_cast<int>(buffer->GetFormat()));
      return nullptr;
    }
  }

  auto* inst = PL::PLInstance::Get().get();
  if (!inst->Init())
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLGL::Create - PLInstance::Init() failed");
    return nullptr;
  }

  return new CLinuxRendererPLGL();
}

bool CLinuxRendererPLGL::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("libplacebo", CLinuxRendererPLGL::Create);
  return true;
}

// ---------------------------------------------------------------------------
// Platform-specific hooks
// ---------------------------------------------------------------------------

EGLDisplay CLinuxRendererPLGL::GetDRMPRIMEEGLDisplay() const
{
  // Desktop GL: the current EGL display is always valid for DRMPRIME init.
  return eglGetCurrentDisplay();
}
