/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/ColorManager.h"
#include "cores/VideoPlayer/VideoRenderers/RenderInfo.h"

#include <libplacebo/cache.h>
#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/options.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/icc.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/utils/upload.h>
extern "C"
{
#include <libavutil/dovi_meta.h>
#include <libavutil/pixfmt.h>
}

#include <memory>
#include <string>
#include <vector>

#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>

#define MAX_FRAME_PASSES 256
#define MAX_BLEND_PASSES 8
#define MAX_BLEND_FRAMES 8
namespace PL
{

class PLInstance
{
public:
  static std::shared_ptr<PLInstance> Get();
  PLInstance();

  virtual ~PLInstance();
  bool Init();
  void Reset();

  pl_renderer GetRenderer() { return m_plRenderer; }
  pl_gpu GetGpu() { return m_plGpu; }

  pl_log m_plLog;
  pl_gpu m_plGpu;
  pl_renderer m_plRenderer;
  pl_cache m_plCache;

  // Backend-specific GPU handle, set by InitGpu(). Backends that need to
  // expose their native handle (e.g. pl_opengl for GL texture wrapping)
  // can store it here as an opaque pointer and cast in the backend renderer.
  void* m_nativeGpuHandle{nullptr};

private:
  // Backend-specific GPU creation/destruction, implemented per-backend
  // (PLInstanceGL.cpp for OpenGL/EGL, future PLInstanceVK.cpp for Vulkan, etc.)
  bool InitGpu();
  void DestroyGpu();

  void LoadCache();
  void SaveCache();

  bool m_isInitialized{false};
  uint64_t m_cacheSignature{0};
};

constexpr pl_rotation RotationFromOrientation(unsigned int deg)
{
  switch (deg)
  {
    case 90:
      return PL_ROTATION_270;
    case 180:
      return PL_ROTATION_180;
    case 270:
      return PL_ROTATION_90;
    default:
      return PL_ROTATION_0;
  }
}

const char* KodiScalingToPlacebo(ESCALINGMETHOD method);

// Merge HDR metadata into colorSpace from the decoded buffer.
//
// Priority 1: Dolby Vision — decoder provides complete, authoritative metadata
// via pl_map_avdovi_metadata. Use it directly; no fallbacks needed.
//
// Priority 2: HDR10+ — per-frame dynamic tone mapping from decoder. scene_max
// changes every frame, so assign directly (not merge) to avoid stale values.
//
// Priority 3: Static HDR10/HLG — decoder's pl_map_hdr_metadata result (mastering
// display + content light level). Merge into colorSpace.
//
// Priority 4 (fallback): When pl_map_hdr_metadata got no frame side data,
// manually extract from displayMetadata/lightMetadata (stream hints via
// DVDVideoCodecFFmpeg's m_hints fallback path).
template<typename TBuffer>
void ApplyHdrMetadata(pl_color_space& colorSpace,
                      pl_color_repr& colorRepr,
                      pl_dovi_metadata& doviMetadata,
                      const TBuffer& b)
{
  // Priority 1: Dolby Vision — early return with authoritative metadata
  if (b.plColorRepr.dovi != nullptr)
  {
    colorSpace = b.plColorSpace;
    colorRepr = b.plColorRepr;
    doviMetadata = b.plDoviMetadata;
    colorRepr.dovi = &doviMetadata;
    return;
  }

  // Priority 2: HDR10+ — per-frame dynamic metadata from decoder.
  // scene_max[0] > 0 means pl_map_hdr_metadata extracted HDR10+ dynamic data.
  // Assign directly: scene values change every frame, merge would keep stale data.
  if (b.plColorSpace.hdr.scene_max[0] > 0)
  {
    colorSpace.hdr = b.plColorSpace.hdr;
    return;
  }

  // Priority 3: Static HDR10/HLG from decoder's pl_map_hdr_metadata
  if (!pl_hdr_metadata_equal(&b.plColorSpace.hdr, &pl_hdr_metadata_empty))
  {
    pl_hdr_metadata_merge(&colorSpace.hdr, &b.plColorSpace.hdr);
    return;
  }

  // Priority 4: Fallback — pl_map_hdr_metadata got no frame side data. Try stream
  // hints from DVDVideoCodecFFmpeg's m_hints path. No transfer restriction:
  // bcm2835-codec on RPi5 sends AVCOL_TRC_UNSPECIFIED even for HDR content.
  if (b.hasDisplayMetadata)
  {
    const auto& m = b.displayMetadata;
    auto& hdr = colorSpace.hdr;
    hdr.prim.red = {static_cast<float>(av_q2d(m.display_primaries[0][0])),
                    static_cast<float>(av_q2d(m.display_primaries[0][1]))};
    hdr.prim.green = {static_cast<float>(av_q2d(m.display_primaries[1][0])),
                      static_cast<float>(av_q2d(m.display_primaries[1][1]))};
    hdr.prim.blue = {static_cast<float>(av_q2d(m.display_primaries[2][0])),
                     static_cast<float>(av_q2d(m.display_primaries[2][1]))};
    hdr.prim.white = {static_cast<float>(av_q2d(m.white_point[0])),
                      static_cast<float>(av_q2d(m.white_point[1]))};
    if (m.has_luminance)
    {
      hdr.max_luma = static_cast<float>(av_q2d(m.max_luminance));
      hdr.min_luma = static_cast<float>(av_q2d(m.min_luminance));
    }
  }
  if (b.hasLightMetadata)
  {
    colorSpace.hdr.max_cll = b.lightMetadata.MaxCLL;
    colorSpace.hdr.max_fall = b.lightMetadata.MaxFALL;
  }
}

// Ensures primaries, transfer, color system, and luminance are valid for
// libplacebo. Guards against out-of-range enum values that cause UB via
// __builtin_unreachable() on ARM64 (manifests as NaN in GLSL constants).
//
// Uses HDR metadata presence to make informed defaults for UNKNOWN fields,
// then delegates to pl_color_space_infer() — libplacebo's own inference that
// computes signal peak from transfer function and fills remaining gaps. This
// matches mpv's approach (mp_image_params_guess_csp → pl_color_space_infer).
inline void ValidateInputColorSpace(pl_color_space& colorSpace, pl_color_repr& colorRepr)
{
  const bool hasAnyHdrMeta = !pl_hdr_metadata_equal(&colorSpace.hdr, &pl_hdr_metadata_empty);

  // Clamp out-of-range enums to UNKNOWN before inference. These originate from
  // hw decoders returning garbage values and would hit pl_unreachable() → UB.
  if (colorSpace.primaries < PL_COLOR_PRIM_UNKNOWN || colorSpace.primaries >= PL_COLOR_PRIM_COUNT)
    colorSpace.primaries = PL_COLOR_PRIM_UNKNOWN;
  if (colorSpace.transfer < PL_COLOR_TRC_UNKNOWN || colorSpace.transfer >= PL_COLOR_TRC_COUNT)
    colorSpace.transfer = PL_COLOR_TRC_UNKNOWN;

  // When HDR metadata is present but primaries/transfer are UNKNOWN (e.g.
  // RPi5 bcm2835-codec sends AVCOL_TRC_UNSPECIFIED for HDR content), set
  // informed defaults before pl_color_space_infer's generic BT.709/BT.1886.
  if (hasAnyHdrMeta)
  {
    if (!colorSpace.primaries)
      colorSpace.primaries = PL_COLOR_PRIM_BT_2020;
    if (!colorSpace.transfer)
      colorSpace.transfer = PL_COLOR_TRC_PQ;
  }

  // HDR YCbCr content must use a BT.2020 matrix
  if (pl_color_transfer_is_hdr(colorSpace.transfer) &&
      pl_color_system_is_ycbcr_like(colorRepr.sys) && colorRepr.sys != PL_COLOR_SYSTEM_BT_2020_NC &&
      colorRepr.sys != PL_COLOR_SYSTEM_BT_2020_C && colorRepr.sys != PL_COLOR_SYSTEM_BT_2100_PQ &&
      colorRepr.sys != PL_COLOR_SYSTEM_BT_2100_HLG)
  {
    colorRepr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
  }

  // Let libplacebo fill remaining gaps: computes signal peak (max_luma) from
  // transfer function, infers min_luma, defaults hdr.prim from primaries.
  // This is the canonical way to ensure a complete pl_color_space — avoids
  // the NaN crash from max_luma=0 reaching pl_color_transfer_nominal_peak.
  pl_color_space_infer(&colorSpace);

  // Strip HDR metadata from SDR content (matches mpv behavior)
  if (!pl_color_space_is_hdr(&colorSpace))
    colorSpace.hdr = pl_hdr_metadata_empty;
}

class RenderConfig
{
public:
  RenderConfig();
  ~RenderConfig();

  // Applies Kodi GUI video settings (brightness, quality, algorithms)
  void UpdateVideoFilter(ESCALINGMETHOD scalingMethod, const CVideoSettings& videoSettings);

  // Reloads CMS LUT / ICC profile based on settings
  void ApplyCMS(pl_frame& frameOut, AVColorPrimaries srcPrimaries);

  // Flushes state when a new video is played
  void ResetCmsState();

  pl_options GetOptions() const { return m_plOpts; }

private:
  void UpdateCmsLut(AVColorPrimaries srcPrimaries);
  void UpdateIccProfile();

  pl_options m_plOpts{nullptr};

  // Cached CMS settings — refreshed in UpdateVideoFilter(), not per-frame.
  bool m_cachedCmsEnabled{false};
  int m_cachedCmsMode{0};

  std::unique_ptr<CColorManager> m_plCmsManager;
  pl_icc_object m_iccObject{nullptr};
  std::vector<uint8_t> m_iccData;
  std::string m_iccPath;
  uint64_t m_iccSignature{0};

  std::vector<float> m_cmsLutData;
  pl_custom_lut m_cmsLut{};
  int m_plCmsToken{-1};
  bool m_cmsLutValid{false};
};

} // namespace PL
