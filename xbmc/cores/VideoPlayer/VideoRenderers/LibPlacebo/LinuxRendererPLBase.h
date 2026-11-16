/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// NOTE: This header must be included AFTER the concrete base renderer header
// (LinuxRendererGL.h or LinuxRendererGLES.h) so that renderer-specific types
// such as CPictureBuffer, RenderMethod, and EShaderFormat are already declared.

#include "PLHelper.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/DRMPRIMEEGL.h"
#include "cores/VideoPlayer/VideoRenderers/VideoShaders/ShaderFormats.h"
#include "utils/HDRCapabilities.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <drm_fourcc.h>
#include <libplacebo/opengl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

#include "system_gl.h"

#if defined(HAVE_NVDEC)
#include "cores/VideoPlayer/DVDCodecs/Video/NVDEC.h"
#endif

#if defined(HAVE_LIBVA)
#include "cores/VideoPlayer/DVDCodecs/Video/VAAPI.h"

#include <va/va_drmcommon.h>
#endif // HAVE_LIBVA

extern "C"
{
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <vector>
#include <bit>
#include <cstring>

#include <libplacebo/shaders/icc.h>
#include <libplacebo/utils/libav.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

// GL_R16 / GL_RG16 / GL_RGBA16 are desktop GL core.  GLES exposes them via
// GL_EXT_texture_norm16 under the _EXT suffix but with identical enum values.
// Define the un-suffixed names as fallbacks so the same code compiles on both.
#ifndef GL_R16
#define GL_R16 0x822A
#endif
#ifndef GL_RG16
#define GL_RG16 0x822C
#endif
#ifndef GL_RGB16
#define GL_RGB16 0x8054
#endif
#ifndef GL_RGBA16
#define GL_RGBA16 0x805B
#endif
#ifndef GL_UNSIGNED_NORMALIZED
#define GL_UNSIGNED_NORMALIZED 0x8C17
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE 0x8211
#endif

/**
 * @brief CRTP mixin providing the shared libplacebo renderer implementation.
 *
 * TBase is either CLinuxRendererGL or CLinuxRendererGLES.
 * Concrete classes must:
 *   - Provide static Create()/Register() factory methods
 *   - Implement GetVaapiTexTarget() — returns GL_TEXTURE_2D (desktop) or
 *     GL_TEXTURE_EXTERNAL_OES (GLES)
 *   - Implement GetDRMPRIMEEGLDisplay() — returns the platform EGLDisplay to
 *     pass to CDRMPRIMETexture::Init()
 */
template<typename TBase>
class CLinuxRendererPLBase : public TBase
{
public:
  CLinuxRendererPLBase();
  ~CLinuxRendererPLBase() override;

  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation) override;
  bool ConfigChanged(const VideoPicture& picture) override;
  [[nodiscard]] bool Supports(ERENDERFEATURE feature) const override;
  [[nodiscard]] bool Supports(ESCALINGMETHOD method) const override;
  CRenderInfo GetRenderInfo() override;
  void AddVideoPicture(const VideoPicture& picture, int index) override;
  bool Flush(bool saveBuffers) override;

protected:
  bool CreateTexture(int index) override;
  void DeleteTexture(int index) override;
  bool UploadTexture(int index) override;
  bool LoadShadersHook() override;
  bool RenderHook(int idx) override;
  void UpdateVideoFilter() override;
  EShaderFormat GetShaderFormat() override;

  // Platform-specific hooks implemented by concrete subclasses.
  [[nodiscard]] virtual GLenum GetVaapiTexTarget() const = 0;
  [[nodiscard]] virtual EGLDisplay GetDRMPRIMEEGLDisplay() const = 0;

  // libplacebo per-pass render info callback.  Logs shader description and
  // GPU execution time for each pass when LOGVIDEO debug logging is enabled.
  static void PlRenderInfoCallback(void* priv, const struct pl_render_info* info);

private:
  // Maximum number of VAAPI layers / DRM planes we handle.
  static constexpr uint32_t kMaxPlanes = 3;

  // Fraction of a vsync period allowed for decoder jitter before pl_queue_update
  // falls back to PL_QUEUE_MORE. Tuned for V4L2/bcm2835-codec on RPi5.
  static constexpr float kQueueTimeoutVsyncFraction = 0.4f;

  // Persistent per-slot GL resources for the software decode upload path.
  // Kept separate from PLBuffer so they survive across per-frame ReleasePLBuffer
  // calls (which only destroy the lightweight pl_tex wrappers). Freed in
  // DeleteTexture and the destructor.
  struct SWBuffer
  {
    GLuint pbo[4]{0, 0, 0, 0}; ///< Pixel Buffer Objects for async CPU→GPU DMA
    GLsizeiptr pboSize[4]{0, 0, 0, 0}; ///< Allocated PBO size (bytes) for resize detection
    GLuint tex[4]{0, 0, 0, 0}; ///< Target GL_TEXTURE_2D textures
    int texW[4]{0, 0, 0, 0}; ///< Cached dimensions for resize detection
    int texH[4]{0, 0, 0, 0};
    GLenum texIformat[4]{0, 0, 0, 0}; ///< Cached iformat for format-change detection
  };

  struct PLBuffer
  {
    pl_plane planes[4]{};
    pl_tex tex[4]{};
    int num_planes{0};
    pl_color_space colorSpace{};
    pl_color_repr colorRepr{};
    pl_dovi_metadata doviMetadata{}; ///< Owned copy; colorRepr.dovi points here when valid
    unsigned int iFlags{0}; ///< DVP_FLAG_* interlace flags
    bool loaded{false};

    // Pending EGL sync object imported from a DMA-buf sync-file fence.
    // EGL_NO_SYNC_KHR means eglWaitSyncKHR has already been called or no sync is needed.
    // Used by both the VAAPI and DRMPRIME paths.
    EGLSyncKHR eglSyncFence{EGL_NO_SYNC_KHR};

#if defined(HAVE_LIBVA)
    GLuint vaapiGLTex[3]{};
    EGLImageKHR vaapiEGLImage[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    int vaapiExportedFd[4]{-1, -1, -1, -1};
    int vaapiNumFds{0};
#endif

    // Set when the per-plane DV import path was used for this frame.
    // ReleasePLBuffer uses this to skip CDRMPRIMETexture::Unmap (not used)
    // and to null the pl_tex slots without destroying them (cache owns them).
    bool usedPerPlaneImport{false};
  };

  std::array<PLBuffer, NUM_BUFFERS> m_plBuffers{};
  std::array<SWBuffer, NUM_BUFFERS> m_swBuffers{};
  std::array<CDRMPRIMETexture, NUM_BUFFERS> m_drmTextures{};

  // Cached pl_tex wrapper for each DRMPRIME slot.
  // pl_opengl_wrap + pl_tex_destroy per frame costs a glGetError() stall (via
  // gl_check_err inside libplacebo) plus mutex round-trips and heap allocation.
  // The GL texture ID and dimensions are stable across frames for a given slot,
  // so the wrapper can be reused. Destroyed in DeleteTexture and the destructor.
  struct DRMTexCache
  {
    pl_tex tex{nullptr};
    GLuint glTex{0};
    int width{0};
    int height{0};
  };
  std::array<DRMTexCache, NUM_BUFFERS> m_drmTexCache{};

  // Persistent per-slot GL texture / pl_tex / EGLImage cache.
  // Used by both the DRMPRIME per-plane (DV) and VAAPI upload paths.
  // GL textures and pl_tex wrappers are created once (dimensions are stable for
  // the entire video) and reused across frames.  Only the EGLImages change per
  // frame — glEGLImageTargetTexture2DOES rebinds the existing GL texture to the
  // new EGLImage.  glEGLImageTargetTexStorageEXT cannot be used here because it
  // creates immutable-format textures that cannot be rebound to a new EGLImage.
  // This avoids glGenTextures/glDeleteTextures/glTexParameteri/pl_opengl_wrap/
  // pl_tex_destroy overhead per frame (the last two cause glGetError stalls on
  // tile-based GPUs).
  struct PlaneCache
  {
    static constexpr int MAX_PLANES = kMaxPlanes;
    pl_tex plTex[MAX_PLANES]{};
    GLuint glTex[MAX_PLANES]{};
    EGLImageKHR eglImage[MAX_PLANES]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    int width[MAX_PLANES]{};
    int height[MAX_PLANES]{};
    int numPlanes{0};
    bool initialized{false};
  };
  std::array<PlaneCache, NUM_BUFFERS> m_drmPlaneCache{};

  bool m_isDRMPRIME{false};

#if defined(HAVE_NVDEC)
  // NVDEC frames stay in GPU memory and are copied device-to-device into the
  // software path's GL textures, registered with CUDA (CUDA-GL interop).
  struct NVDECBuffer
  {
    CUgraphicsResource resource[2]{};
    GLuint registeredTex[2]{};
  };
  std::array<NVDECBuffer, NUM_BUFFERS> m_nvdecBuffers{};
  CUcontext m_nvdecContext{nullptr};
  AVBufferRef* m_nvdecDevice{nullptr}; ///< keeps the CUDA context alive
  bool m_isNVDEC{false};
#endif
  // True when eglQueryDmaBufModifiersEXT is available: the EGL stack inserts
  // implicit DMA-buf fences at eglCreateImageKHR time, making explicit CPU or
  // GPU-side sync redundant for both VAAPI and DRMPRIME paths.
  bool m_hasEGLModifiers{false};

#if defined(HAVE_LIBVA)
  std::array<PlaneCache, NUM_BUFFERS> m_vaapiCache{};
#endif

  // EGL context used globally for Sync objects and Image KHR
  EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};

#if defined(HAVE_LIBVA)
  bool m_isVAAPI{false};
#endif

  // EGL interop for per-plane DMA-buf import (VAAPI + DRMPRIME DV path).
  // Probed in Configure() when either VAAPI or DRMPRIME is detected.
  PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR{nullptr};
  PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR{nullptr};
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES{nullptr};

  AVPixelFormat m_format{AV_PIX_FMT_NONE};
  pl_color_space m_colorSpace{};
  pl_chroma_location m_chromaLocation{PL_CHROMA_UNKNOWN};
  std::unique_ptr<PL::RenderConfig> m_plConfig;
  // Video settings the libplacebo options were last built from. The player applies
  // them after the renderer is configured, so changes are picked up when rendering.
  CVideoSettings m_appliedSettings;
  pl_color_map_params m_autoColorMap{};
  bool m_appliedSettingsValid{false};
  void SyncOptions();
  CHDRCapabilities m_displayHDRCaps;

  struct QueuedFrameState
  {
    int bufferIndex;
    CLinuxRendererPLBase* renderer;
  };
  // Pool of QueuedFrameState objects indexed by buffer slot.  Avoids per-frame
  // heap allocation — each slot is reused after libplacebo calls unmap/discard.
  std::array<QueuedFrameState, NUM_BUFFERS> m_queueFramePool{};

  pl_queue m_plQueue{nullptr};
  double m_queuePtsOffset{0.0};
  bool m_queuePtsOffsetSet{false};
  // Last pl_queue request, to keep requests monotonic and to step through the
  // fields of a frame that Kodi renders on several vsyncs.
  double m_queueLastRequest{-1.0};
  int m_queueLastIdx{-1};
  double m_queueLastPts{0.0};
  int m_queueRepeat{0};

  //! Kodi timestamps (DVD_TIME_BASE) to pl_queue seconds
  double QueueTime(double pts) const { return (pts - m_queuePtsOffset) / DVD_TIME_BASE; }
  void PushToQueue(int index, double pts, unsigned int flags);
  void RequeueHeldFrames();

  // Cached GL framebuffer → pl_tex wrapper.
  // pl_opengl_wrap/pl_tex_destroy for a framebuffer flushes GPU command queues on
  // some drivers and forces a full libplacebo internal state reset.  Wrapping once
  // and reusing the same pl_tex across frames avoids that overhead entirely.
  // Invalidated when the bound FBO id, viewport width, or viewport height changes.
  pl_tex m_cachedFboTex{nullptr};
  unsigned int m_cachedFboId{UINT_MAX};
  int m_cachedFboW{0};
  int m_cachedFboH{0};

  // Cached Kodi VAO handle.
  // glGetIntegerv(GL_VERTEX_ARRAY_BINDING) serializes the GPU command stream on
  // tile-based GPUs (Mali / Adreno / PowerVR / Apple GPU) because the driver must
  // finish all in-flight work before reading back a GPU-side integer.  Kodi's VAO
  // is stable for the lifetime of the renderer, so we query it once and cache it.
  GLint m_kodiVAO{0};
  bool m_kodiVaoCached{false};

  // Cached GL save/restore state.
  // Viewport, scissor enable, and scissor box are CPU-side shadow state in every
  // known GL driver (not a GPU round-trip), but we still save 3 function calls per
  // frame by caching them.  Invalidated whenever the FBO dimensions change.
  bool m_glStateCached{false};
  GLint m_cachedGLViewport[4]{};
  GLboolean m_cachedScissorEnabled{GL_FALSE};
  GLint m_cachedGLScissor[4]{};

  // When the GL context was created with EGL_CONTEXT_OPENGL_NO_ERROR_KHR, all
  // error generation is disabled and glGetError() always returns GL_NO_ERROR.
  // In that case we can skip the pre-render error drain entirely.
  bool m_glNoError{false};

  // Per-slot upload cache: tracks the last successfully uploaded videoBuffer
  // pointer for each buffer slot.  When the same slot is re-rendered with the
  // same source buffer (24fps content at 60Hz display), UploadDRMPRIME skips
  // the Unmap/Map/EGLImage cycle entirely — the existing EGLImage and pl_tex
  // are still valid.  Cleared on ReleaseBuffer and Flush.
  std::array<CVideoBuffer*, NUM_BUFFERS> m_lastUploadedBuffer{};

  // Cached per-session values that don't change between Configure() calls.
  // Avoids virtual dispatch through CServiceBroker on every frame.
  double m_cachedFrameDuration{0.0};
  float m_cachedVsyncDuration{0.0f};
  float m_cachedSdrPeakLuminance{0.0f};
  bool m_cachedUseLimitedColor{false};
  bool m_cachedLogVideo{false};
  pl_color_space m_frameOutColor{};
  pl_color_repr m_frameOutRepr{};

  // Pre-built input frame template.  Session-stable fields (rotation, chroma
  // location, field) are set once in Configure(); only planes, color, repr,
  // and crop are patched per frame.  Saves a pl_frame zero-init + the
  // pl_frame_set_chroma_location call per render.
  pl_frame m_frameInTemplate{};

  // Render-time diagnostic: measures wall-clock time per frame (including GPU
  // finish) and logs avg/peak every 2 seconds.  Temporary — remove after
  // identifying the periodic spike source.

  // EGL_ANDROID_native_fence_sync: GPU-side DMA-buf fence wait.
  //
  // Used for both VAAPI and DRMPRIME paths.  When this extension and the
  // DMA_BUF_IOCTL_EXPORT_SYNC_FILE ioctl (kernel ≥ 5.2) are available, we
  // export the DMA-buf read fence as a sync-file fd, import it as an EGL
  // sync object, and call eglWaitSyncKHR — a GPU command-stream wait that
  // never stalls the CPU.
  PFNEGLCREATESYNCKHRPROC m_eglCreateSyncKHR{nullptr};
  PFNEGLDESTROYSYNCKHRPROC m_eglDestroySyncKHR{nullptr};
  PFNEGLWAITSYNCKHRPROC m_eglWaitSyncKHR{nullptr};
  bool m_hasEGLSyncFence{false};

  // Bitfield tracking which m_plBuffers slots have a pending EGL sync fence.
  // Avoids scanning all NUM_BUFFERS slots in RenderHook every frame.
  static_assert(NUM_BUFFERS <= 32, "pendingSyncSlots bitfield requires NUM_BUFFERS <= 32");
  uint32_t m_pendingSyncSlots{0};

  bool UploadVAAPI(int index, PLBuffer& plbuf);
  bool UploadDRMPRIME(int index, PLBuffer& plbuf);
  bool UploadDRMPRIMEPlanes(int index, PLBuffer& plbuf);
  bool UploadSoftware(int index, PLBuffer& plbuf);
#if defined(HAVE_NVDEC)
  bool UploadNVDEC(int index, PLBuffer& plbuf);
  void ReleaseNVDEC(int index);
#endif

  // Destroy all resources in a PlaneCache and reset it.
  void DestroyPlaneCache(PlaneCache& cache);

  // Per-plane descriptor for the shared EGLImage import helper.
  struct PlaneImportDesc
  {
    uint32_t drmFormat;
    int width;
    int height;
    int fd;
    int offset;
    int pitch;
    uint64_t modifier;
  };

  // Shared import loop for VAAPI and DRMPRIME per-plane paths.
  // On first frame (cache not initialized): creates GL textures, sets defaults,
  // creates EGLImages, binds them, and wraps with pl_opengl_wrap.
  // On subsequent frames: destroys old EGLImages, creates new ones, rebinds.
  // Returns false on failure; on error during first frame, cleans up the cache.
  bool ImportPlanesToCache(PlaneCache& cache,
                           const PlaneImportDesc* planes,
                           int numPlanes,
                           GLenum texTarget,
                           PLBuffer& plbuf,
                           const char* callerName);

  // Chroma subsampling divisors for a pixel format.
  // chroma width  = (fullW + widthDiv - 1) / widthDiv
  // chroma height = (fullH + heightDiv - 1) / heightDiv
  // Defaults to {1, 1, false}: callers must check `known` before use, but
  // if the guard is missed the 1:1 divisors produce full-resolution chroma
  // (a visual artifact) instead of a divide-by-zero crash.
  struct ChromaDiv
  {
    int widthDiv{1};
    int heightDiv{1};
    bool known{false};
  };

  // Look up chroma subsampling from a combined (multi-plane) DRM format.
  static ChromaDiv ChromaDivFromDrmFormat(uint32_t drmFormat);

#if defined(HAVE_LIBVA)
  // Look up chroma subsampling from a VA fourcc.
  static ChromaDiv ChromaDivFromVaFourcc(uint32_t vaFourcc);
#endif

  // Per-plane format and subsampling info for a multi-plane DRM format.
  // GL internal formats are derived via DrmFormatToGLIformat() to avoid
  // duplicating the DRM → GL mapping.
  struct DRMPlaneSplit
  {
    uint32_t drmFormat[kMaxPlanes]{};
    ChromaDiv chroma;
  };

  // Split a combined DRM multi-plane format into per-plane formats with subsampling.
  // Returns an invalid split (valid == false) for single-plane or unknown formats.
  static DRMPlaneSplit SplitDrmFormat(uint32_t layerFormat, int numPlanes);

  static bool MapCallback(pl_gpu gpu,
                          pl_tex* tex,
                          const struct pl_source_frame* src,
                          struct pl_frame* out);
  static void UnmapCallback(pl_gpu gpu, struct pl_frame* frame, const struct pl_source_frame* src);
  static void DiscardCallback(const struct pl_source_frame* src);

  void ReleasePLBuffer(int index);
  void ReleaseSWBuffer(int index);

  // Populate plbuf.colorSpace/colorRepr from the buffer's source metadata.
  // When isYCbCr is true, derives the YCbCr matrix from the stream and guesses
  // BT.601/709/2020 if unknown. When false, forces PL_COLOR_SYSTEM_RGB (OES path).
  template<typename TBuffer>
  void SetSourceColorSpace(PLBuffer& plbuf, const TBuffer& buf, bool isYCbCr);

  // Export a DMA-buf read fence as an EGL sync object for GPU-side wait.
  // No-op when EGL sync fences or modifiers are unavailable.
  void ExportDmaBufSyncFence(int dmaBufFd, int slotIndex, PLBuffer& plbuf);

  // Build EGL_LINUX_DMA_BUF_EXT attrib list for a single DMA-buf plane.
  // Caller must provide an array of at least kDmaBufEGLAttribsSize elements.
  // Returns pointer past the last written element (the EGL_NONE terminator).
  static constexpr int kDmaBufEGLAttribsSize = 6 * 2 + 2 * 2 + 1; // 6 mandatory + 2 modifier + terminator
  EGLint* BuildDmaBufEGLAttribs(EGLint* a,
                                uint32_t drmFormat,
                                int width,
                                int height,
                                int fd,
                                int offset,
                                int pitch,
                                uint64_t modifier) const;

  // Map a single-plane DRM fourcc to its GL internal format.
  // Returns 0 for unrecognized formats.
  static GLenum DrmFormatToGLIformat(uint32_t drmFormat);

  // Set common texture parameters for DMA-buf imported textures.
  static void SetTextureDefaults(GLenum target);

  // Set up YCbCr pl_plane component mapping for numPlanes planes.
  // When planarChroma is true (e.g. YUV420 3-plane), chroma planes have 1
  // component each; when false (NV12/P010 semi-planar), plane 1 has 2 (CbCr).
  static void SetYCbCrPlaneMapping(PLBuffer& plbuf, int numPlanes, bool planarChroma);

  // Map pl_plane_data component layout → GL iformat/format/type/bytesPerPixel.
  static bool PlaneDataToGLFormats(
      const pl_plane_data& pd, GLenum& iformat, GLenum& format, GLenum& type, int& bytesPerPixel);
};

// =============================================================================
// Template implementations
// =============================================================================

template<typename TBase>
CLinuxRendererPLBase<TBase>::CLinuxRendererPLBase()
{
  m_plConfig = std::make_unique<PL::RenderConfig>();
}

template<typename TBase>
CLinuxRendererPLBase<TBase>::~CLinuxRendererPLBase()
{
  for (int i = 0; i < NUM_BUFFERS; ++i)
  {
    ReleasePLBuffer(i);
#if defined(HAVE_NVDEC)
    ReleaseNVDEC(i);
#endif
    ReleaseSWBuffer(i);
  }
#if defined(HAVE_NVDEC)
  av_buffer_unref(&m_nvdecDevice);
#endif
  const pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  for (auto& cache : m_drmTexCache)
  {
    if (cache.tex)
      pl_tex_destroy(gpu, &cache.tex);
    cache = {};
  }
  for (auto& pc : m_drmPlaneCache)
    DestroyPlaneCache(pc);
#if defined(HAVE_LIBVA)
  for (auto& vc : m_vaapiCache)
    DestroyPlaneCache(vc);
#endif
  if (m_plQueue)
  {
    pl_queue_destroy(&m_plQueue);
    m_plQueue = nullptr;
  }
  if (m_cachedFboTex)
  {
    pl_tex_destroy(gpu, &m_cachedFboTex);
    m_cachedFboTex = nullptr;
  }
  m_plConfig.reset();
  PL::PLInstance::Get()->Reset();
}

// ---------------------------------------------------------------------------
// DestroyPlaneCache — release all resources in a PlaneCache and reset it
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::DestroyPlaneCache(PlaneCache& cache)
{
  if (!cache.initialized)
    return;
  const pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  for (int n = 0; n < cache.numPlanes; ++n)
  {
    if (cache.plTex[n])
      pl_tex_destroy(gpu, &cache.plTex[n]);
    if (cache.eglImage[n] != EGL_NO_IMAGE_KHR)
      m_eglDestroyImageKHR(m_eglDisplay, cache.eglImage[n]);
  }
  if (cache.numPlanes > 0)
    glDeleteTextures(cache.numPlanes, cache.glTex);
  cache = {};
}

// ---------------------------------------------------------------------------
// Configure / Flush / AddVideoPicture
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::Configure(const VideoPicture& picture,
                                            float fps,
                                            unsigned int orientation)
{
  if (!TBase::Configure(picture, fps, orientation))
    return false;

  m_format = picture.videoBuffer->GetFormat();

  m_colorSpace = pl_color_space{};
  m_colorSpace.primaries = pl_primaries_from_av(picture.color_primaries);
  m_colorSpace.transfer = pl_transfer_from_av(picture.color_transfer);
  m_chromaLocation = pl_chroma_from_av(picture.chroma_position);

  m_eglDisplay = eglGetCurrentDisplay();
  if (m_eglDisplay == EGL_NO_DISPLAY)
    m_eglDisplay = GetDRMPRIMEEGLDisplay();

  // Probe EGL DMA-buf modifier support unconditionally: applies to both VAAPI
  // and DRMPRIME.  When present, eglCreateImageKHR implicitly attaches the
  // DMA-buf reservation fence to the EGLImage so the GPU waits automatically —
  // no explicit CPU stall or explicit sync is needed.
  m_hasEGLModifiers = (eglGetProcAddress("eglQueryDmaBufModifiersEXT") != nullptr);

#if defined(HAVE_LIBVA)
  m_isVAAPI = (dynamic_cast<VAAPI::CVaapiRenderPicture*>(picture.videoBuffer) != nullptr);
#endif

  m_isDRMPRIME = (dynamic_cast<CVideoBufferDRMPRIME*>(picture.videoBuffer) != nullptr);

#if defined(HAVE_NVDEC)
  // The planes are NV12 or P010/P016 in GPU memory, handled like software frames.
  m_isNVDEC = dynamic_cast<NVDEC::CVideoBufferNVDEC*>(picture.videoBuffer) != nullptr;
#endif

  // Probe EGL interop functions shared by VAAPI and DRMPRIME per-plane (DV) paths.
  if (m_isDRMPRIME
#if defined(HAVE_LIBVA)
      || m_isVAAPI
#endif
  )
  {
    m_eglCreateImageKHR =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    m_eglDestroyImageKHR =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    m_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  }

  if (m_isDRMPRIME)
  {
    const EGLDisplay eglDpy = GetDRMPRIMEEGLDisplay();
    for (auto& dt : m_drmTextures)
      dt.Init(eglDpy);
  }

  if (!m_plQueue)
  {
    m_plQueue = pl_queue_create(PL::PLInstance::Get()->m_plGpu);
    if (!m_plQueue)
      CLog::Log(LOGERROR, "CLinuxRendererPLBase::Configure - pl_queue_create failed");
  }
  m_queuePtsOffsetSet = false;
  m_queueLastRequest = -1.0;
  m_queueLastIdx = -1;

  // Cache display HDR capabilities for use in RenderHook (avoids per-frame allocation).
  if (this->m_passthroughHDR)
    m_displayHDRCaps = CServiceBroker::GetWinSystem()->GetDisplayHDRCapabilities();
  else
    m_displayHDRCaps = {};

  // Cache per-session windowing values used every frame in RenderHook.
  // These only change on resolution/display switch (which triggers reconfigure).
  m_cachedFrameDuration = (fps > 0.0f) ? 1.0 / static_cast<double>(fps) : 0.0;
  m_cachedVsyncDuration = 1.0f / CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  m_cachedSdrPeakLuminance = CServiceBroker::GetWinSystem()->GetGuiSdrPeakLuminance();
  m_cachedUseLimitedColor = CServiceBroker::GetWinSystem()->UseLimitedColor();
  m_cachedLogVideo = CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO);

  // Pre-build output frame color/repr template. Most fields are session-stable;
  // only HDR transfer (PQ vs HLG) may vary per-frame in passthrough mode.
  m_frameOutColor = {};
  m_frameOutRepr = {};
  if (this->m_passthroughHDR)
  {
    m_frameOutColor.primaries = PL_COLOR_PRIM_BT_2020;
    // Transfer is set per-frame in RenderHook (PQ vs HLG depends on source).
    if (m_displayHDRCaps.GetDisplayMaxLuminance() > 0.0f)
    {
      m_frameOutColor.hdr.max_luma = m_displayHDRCaps.GetDisplayMaxLuminance();
      if (m_displayHDRCaps.GetDisplayMinLuminance() > 0.0f)
        m_frameOutColor.hdr.min_luma = m_displayHDRCaps.GetDisplayMinLuminance();
    }
  }
  else
  {
    m_frameOutColor.primaries = PL_COLOR_PRIM_BT_709;
    m_frameOutColor.transfer = PL_COLOR_TRC_BT_1886;
    if (m_cachedSdrPeakLuminance > 0.0f)
      m_frameOutColor.hdr.max_luma = m_cachedSdrPeakLuminance;
  }
  m_frameOutRepr.sys = PL_COLOR_SYSTEM_RGB;
  m_frameOutRepr.levels =
      m_cachedUseLimitedColor ? PL_COLOR_LEVELS_LIMITED : PL_COLOR_LEVELS_FULL;

  // Invalidate CMS state: source primaries may have changed and we are about
  // to render a new video source.
  m_plConfig->ResetCmsState();

  // Flush libplacebo's renderer caches (peak detection, frame mix state) so
  // the new source starts clean.  Required when switching content.
  if (const auto inst = PL::PLInstance::Get())
    pl_renderer_flush_cache(inst->GetRenderer());

  // Invalidate per-session render caches: the FBO or window size may have changed
  // and Kodi's VAO may have been recreated since the last Configure call.
  if (m_cachedFboTex)
  {
    pl_tex_destroy(PL::PLInstance::Get()->m_plGpu, &m_cachedFboTex);
    m_cachedFboTex = nullptr;
  }
  m_cachedFboId = UINT_MAX;
  m_cachedFboW = 0;
  m_cachedFboH = 0;
  m_kodiVaoCached = false;
  m_glStateCached = false;

  // Detect GL_KHR_no_error: if the context was created with no-error mode,
  // glGetError() is a no-op and we can skip the pre-render drain entirely.
  // GL_CONTEXT_FLAGS is a desktop GL query; GLES does not expose it.
#if defined(HAS_GL)
  {
    GLint ctxFlags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &ctxFlags);
    m_glNoError = (ctxFlags & GL_CONTEXT_FLAG_NO_ERROR_BIT_KHR) != 0;
  }
#else
  // GLES: GL_CONTEXT_FLAGS is not available. Query the EGL context attribute instead.
  // Mesa on RPi5 (and other platforms) sets EGL_CONTEXT_OPENGL_NO_ERROR_KHR when the
  // context is created with KHR_no_error, allowing us to skip the glGetError() drain.
  {
    EGLint noError = EGL_FALSE;
    if (m_eglDisplay != EGL_NO_DISPLAY)
      eglQueryContext(m_eglDisplay, eglGetCurrentContext(), EGL_CONTEXT_OPENGL_NO_ERROR_KHR,
                      &noError);
    m_glNoError = (noError == EGL_TRUE);
  }
#endif

  // Probe EGL_ANDROID_native_fence_sync + KHR_wait_sync
  // Used by both VAAPI (replaces vaSyncSurface() CPU stall) and DRMPRIME
  // (adds explicit GPU-side sync for V4L2/VC4 decode fences on RPi5 etc.).
  // Only meaningful when DMA_BUF_IOCTL_EXPORT_SYNC_FILE is also available
  // at compile time (kernel ≥ 5.2 headers).
  {
    auto* createSync = eglGetProcAddress("eglCreateSyncKHR");
    auto* destroySync = eglGetProcAddress("eglDestroySyncKHR");
    auto* waitSync = eglGetProcAddress("eglWaitSyncKHR");
    if (createSync && destroySync && waitSync)
    {
      m_eglCreateSyncKHR = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(createSync);
      m_eglDestroySyncKHR = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(destroySync);
      m_eglWaitSyncKHR = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(waitSync);
      m_hasEGLSyncFence = true;
    }
    else
    {
      m_hasEGLSyncFence = false;
    }
  }

  return true;
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::ConfigChanged(const VideoPicture& picture)
{
  return picture.videoBuffer->GetFormat() != m_format;
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::Flush(bool saveBuffers)
{
  if (m_plQueue)
  {
    pl_queue_reset(m_plQueue);
    m_queuePtsOffsetSet = false;
    m_queueLastRequest = -1.0;
    m_queueLastIdx = -1;
  }
  // The FBO may be recreated after a seek/stop; drop the cached wrapper so
  // RenderHook re-wraps with the current FBO on the next frame.
  if (m_cachedFboTex)
  {
    pl_tex_destroy(PL::PLInstance::Get()->m_plGpu, &m_cachedFboTex);
    m_cachedFboTex = nullptr;
  }
  m_cachedFboId = UINT_MAX;
  m_cachedFboW = 0;
  m_cachedFboH = 0;
  // Force re-import on next render after a seek/stop.
  m_lastUploadedBuffer.fill(nullptr);
  // VAO is stable across seeks; no need to invalidate m_kodiVaoCached.
  // Viewport/scissor state is window-level; invalidate conservatively.
  m_glStateCached = false;
  return TBase::Flush(saveBuffers);
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::AddVideoPicture(const VideoPicture& picture, int index)
{
  TBase::AddVideoPicture(picture, index);
  m_plBuffers[index].loaded = false;

  if (!m_plQueue || this->m_fps <= 0.0f)
    return;

  if (!m_queuePtsOffsetSet)
  {
    m_queuePtsOffset = picture.pts;
    m_queuePtsOffsetSet = true;
  }

  PushToQueue(index, picture.pts, picture.iFlags);
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::PushToQueue(int index, double pts, unsigned int flags)
{
  auto* qf = &m_queueFramePool[index];
  qf->bufferIndex = index;
  qf->renderer = this;

  // pl_queue works in seconds, like the frame and vsync durations.
  pl_source_frame src{};
  src.pts = QueueTime(pts);
  src.duration = m_cachedFrameDuration;
  if (flags & DVP_FLAG_INTERLACED)
    src.first_field = (flags & DVP_FLAG_TOP_FIELD_FIRST) ? PL_FIELD_TOP : PL_FIELD_BOTTOM;
  src.frame_data = qf;
  src.map = &CLinuxRendererPLBase<TBase>::MapCallback;
  src.unmap = &CLinuxRendererPLBase<TBase>::UnmapCallback;
  src.discard = &CLinuxRendererPLBase<TBase>::DiscardCallback;

  pl_queue_push(m_plQueue, &src);
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::RequeueHeldFrames()
{
  // pl_queue only moves forward. After a seek or in pause Kodi can render an older
  // frame, so start over with the frames the renderer still holds, oldest first.
  pl_queue_reset(m_plQueue);
  std::vector<int> held;
  for (int i = 0; i < NUM_BUFFERS; ++i)
  {
    if (this->m_buffers[i].videoBuffer)
      held.push_back(i);
  }
  std::sort(held.begin(), held.end(),
            [this](int a, int b) { return this->m_buffers[a].pts < this->m_buffers[b].pts; });
  for (int i : held)
    PushToQueue(i, this->m_buffers[i].pts, this->m_buffers[i].iFlags);
}

// ---------------------------------------------------------------------------
// pl_queue callbacks
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::MapCallback(pl_gpu /*gpu*/,
                                              pl_tex* /*tex*/,
                                              const struct pl_source_frame* src,
                                              struct pl_frame* out)
{
  auto* qf = static_cast<QueuedFrameState*>(src->frame_data);
  CLinuxRendererPLBase<TBase>* r = qf->renderer;
  const int idx = qf->bufferIndex;

  if (!r->UploadTexture(idx))
    return false;

  const PLBuffer& plbuf = r->m_plBuffers[idx];
  if (!plbuf.loaded)
    return false;

  out->num_planes = plbuf.num_planes;
  for (int n = 0; n < plbuf.num_planes; ++n)
    out->planes[n] = plbuf.planes[n];
  out->color = plbuf.colorSpace;
  out->repr = plbuf.colorRepr;
  pl_frame_set_chroma_location(out, r->m_chromaLocation);
  out->rotation = PL::RotationFromOrientation(r->m_renderOrientation);

  // Set source crop so zoom/stretch/pixel ratio affect the source region.
  CRect srcRect, dstRect, viewRect;
  r->GetVideoRect(srcRect, dstRect, viewRect);
  out->crop = {srcRect.x1, srcRect.y1, srcRect.x2, srcRect.y2};

  // pl_queue sets out->field after map() returns based on first_field splitting.
  out->field = PL_FIELD_NONE;

  // Leave planes[n].flipped as set by UploadTexture (true for EGLImage DMA-buf
  // imports where DMA-buf row 0 = top of video but GL texcoord t=0 = bottom).
  // libplacebo's pass_align_planes inverts the sampling rect for flipped planes.

  return true;
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::UnmapCallback(pl_gpu /*gpu*/,
                                                struct pl_frame* /*frame*/,
                                                const struct pl_source_frame* src)
{
  // Textures remain in m_plBuffers and are freed by DeleteTexture / ReleasePLBuffer.
  // QueuedFrameState lives in m_queueFramePool — no delete needed.
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::DiscardCallback(const struct pl_source_frame* /*src*/)
{
  // QueuedFrameState lives in m_queueFramePool — no delete needed.
}

// ---------------------------------------------------------------------------
// Feature / scaling support
// ---------------------------------------------------------------------------

template<typename TBase>
CRenderInfo CLinuxRendererPLBase<TBase>::GetRenderInfo()
{
  CRenderInfo info = TBase::GetRenderInfo();
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_NONE);
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_AUTO);
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_LIBPLACEBO_BOB);
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_LIBPLACEBO_YADIF);
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_LIBPLACEBO_BWDIF);
  return info;
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::Supports(ERENDERFEATURE feature) const
{
  switch (feature)
  {
    case RENDERFEATURE_ZOOM:
    case RENDERFEATURE_VERTICAL_SHIFT:
    case RENDERFEATURE_PIXEL_RATIO:
    case RENDERFEATURE_STRETCH:
    case RENDERFEATURE_ROTATION:
    case RENDERFEATURE_TONEMAP:
    case RENDERFEATURE_TONEMAP_LIBPLACEBO:
    case RENDERFEATURE_BRIGHTNESS:
    case RENDERFEATURE_CONTRAST:
      return true;
    default:
      return false;
  }
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::Supports(ESCALINGMETHOD method) const
{
  switch (method)
  {
    case VS_SCALINGMETHOD_AUTO:
    case VS_SCALINGMETHOD_NEAREST:
    case VS_SCALINGMETHOD_LINEAR:
    case VS_SCALINGMETHOD_LANCZOS2:
    case VS_SCALINGMETHOD_LANCZOS3:
    case VS_SCALINGMETHOD_LANCZOS3_FAST:
    case VS_SCALINGMETHOD_SPLINE36:
    case VS_SCALINGMETHOD_SPLINE36_FAST:
    case VS_SCALINGMETHOD_CUBIC_MITCHELL:
    case VS_SCALINGMETHOD_CUBIC_CATMULL:
    case VS_SCALINGMETHOD_CUBIC_B_SPLINE:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// UpdateVideoFilter — apply Kodi GUI settings to libplacebo render params
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::UpdateVideoFilter()
{
  TBase::UpdateVideoFilter();
  m_plConfig->UpdateVideoFilter(this->m_scalingMethod, this->m_videoSettings);
  m_appliedSettings = this->m_videoSettings;
  m_appliedSettingsValid = true;
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::SyncOptions()
{
  const CVideoSettings& vs = this->m_videoSettings;
  if (m_appliedSettingsValid && vs.m_InterlaceMethod == m_appliedSettings.m_InterlaceMethod &&
      vs.m_ToneMapMethod == m_appliedSettings.m_ToneMapMethod &&
      vs.m_ToneMapParam == m_appliedSettings.m_ToneMapParam &&
      vs.m_Brightness == m_appliedSettings.m_Brightness &&
      vs.m_Contrast == m_appliedSettings.m_Contrast)
    return;

  m_plConfig->UpdateVideoFilter(this->m_scalingMethod, vs);
  m_appliedSettings = vs;
  m_appliedSettingsValid = true;

}

// ---------------------------------------------------------------------------
// PlRenderInfoCallback — per-pass GPU timing for diagnostics
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::PlRenderInfoCallback(void* /*priv*/,
                                                       const struct pl_render_info* info)
{
  // No CanLogComponent guard needed: this callback is only set in RenderHook
  // when m_cachedLogVideo is true, so libplacebo won't invoke it otherwise.
  const char* stage = (info->stage == PL_RENDER_STAGE_FRAME) ? "frame" : "blend";
  const char* desc = info->pass->shader ? info->pass->shader->description : "?";

  if (info->pass->num_samples > 0)
  {
    CLog::Log(LOGDEBUG, LOGVIDEO,
              "libplacebo pass [{}/{}]: {:6.3f} ms (avg {:6.3f} ms, peak {:6.3f} ms) — {}",
              stage, info->index, static_cast<double>(info->pass->last) / 1e6,
              static_cast<double>(info->pass->average) / 1e6,
              static_cast<double>(info->pass->peak) / 1e6, desc);
  }
  else
  {
    CLog::Log(LOGDEBUG, LOGVIDEO, "libplacebo pass [{}/{}]: (no timing) — {}", stage,
              info->index, desc);
  }
}

template<typename TBase>
EShaderFormat CLinuxRendererPLBase<TBase>::GetShaderFormat()
{
  return SHADER_NONE;
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::LoadShadersHook()
{
  // Prevent TBase from loading its own YUV shaders; we render via RenderHook.
  // RENDER_GLSL is a file-scope enum value defined in both LinuxRendererGL.h and
  // LinuxRendererGLES.h, so it is visible here as a non-dependent name.
  this->m_renderMethod = RENDER_GLSL;
  return true;
}

// ---------------------------------------------------------------------------
// Texture lifecycle
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::CreateTexture(int index)
{
  auto& buf = this->m_buffers[index];
  auto& im = buf.image;
  memset(&im, 0, sizeof(im));
  im.height = this->m_sourceHeight;
  im.width = this->m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;

  // Generate a real dummy texture so ValidateRenderer passes safely.
  GLuint dummyTex = 0;
  glGenTextures(1, &dummyTex);
  buf.fields[0][0].id = dummyTex; // 0 == FIELD_FULL
  return true;
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::DeleteTexture(int index)
{
  ReleasePLBuffer(index);
#if defined(HAVE_NVDEC)
  ReleaseNVDEC(index);
#endif
  ReleaseSWBuffer(index);
  if (m_drmTexCache[index].tex)
  {
    pl_tex_destroy(PL::PLInstance::Get()->m_plGpu, &m_drmTexCache[index].tex);
    m_drmTexCache[index] = {};
  }
  DestroyPlaneCache(m_drmPlaneCache[index]);
#if defined(HAVE_LIBVA)
  DestroyPlaneCache(m_vaapiCache[index]);
#endif
  GLuint dummyTex = this->m_buffers[index].fields[0][0].id; // 0 == FIELD_FULL
  if (dummyTex > 0)
    glDeleteTextures(1, &dummyTex);
  this->m_buffers[index].fields[0][0].id = 0;
}

// ---------------------------------------------------------------------------
// UploadTexture — VAAPI / DRMPRIME / software paths
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadTexture(int index)
{
  auto& buf = this->m_buffers[index];
  if (!buf.videoBuffer)
    return false;

  PLBuffer& plbuf = m_plBuffers[index];
  if (plbuf.loaded) [[likely]]
    return true;
  ReleasePLBuffer(index);

#if defined(HAVE_LIBVA)
  if (m_isVAAPI)
    return UploadVAAPI(index, plbuf);
#endif

  if (m_isDRMPRIME)
    return UploadDRMPRIME(index, plbuf);

#if defined(HAVE_NVDEC)
  if (m_isNVDEC)
    return UploadNVDEC(index, plbuf);
#endif

  return UploadSoftware(index, plbuf);
}

// ---------------------------------------------------------------------------
// ImportPlanesToCache — shared EGLImage import for VAAPI and DRMPRIME paths
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::ImportPlanesToCache(PlaneCache& cache,
                                                      const PlaneImportDesc* planes,
                                                      int numPlanes,
                                                      GLenum texTarget,
                                                      PLBuffer& plbuf,
                                                      const char* callerName)
{
  static_assert(PlaneCache::MAX_PLANES == kMaxPlanes,
                "PlaneCache::MAX_PLANES must match kMaxPlanes — callers stack-allocate "
                "PlaneImportDesc[kMaxPlanes] and loop up to numPlanes");

  const pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  const bool firstFrame = !cache.initialized;
  bool success = true;

  if (firstFrame)
    glGenTextures(numPlanes, cache.glTex);

  for (int i = 0; i < numPlanes && success; ++i)
  {
    // Destroy the previous frame's EGLImage (no-op on first frame)
    if (cache.eglImage[i] != EGL_NO_IMAGE_KHR)
    {
      m_eglDestroyImageKHR(m_eglDisplay, cache.eglImage[i]);
      cache.eglImage[i] = EGL_NO_IMAGE_KHR;
    }

    EGLint attribs[kDmaBufEGLAttribsSize];
    BuildDmaBufEGLAttribs(attribs, planes[i].drmFormat, planes[i].width, planes[i].height,
                          planes[i].fd, planes[i].offset, planes[i].pitch, planes[i].modifier);

    cache.eglImage[i] =
        m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (!cache.eglImage[i])
    {
      CLog::Log(LOGERROR,
                "CLinuxRendererPLBase::{} - eglCreateImageKHR failed for plane {} "
                "(EGL error 0x{:x})",
                callerName, i, static_cast<unsigned>(eglGetError()));
      success = false;
      break;
    }

    glBindTexture(texTarget, cache.glTex[i]);

    if (firstFrame)
      SetTextureDefaults(texTarget);

    // Always use glEGLImageTargetTexture2DOES (mutable path) — cannot use
    // TexStorage because immutable-format textures cannot be rebound to a
    // different EGLImage on subsequent frames.
    m_glEGLImageTargetTexture2DOES(texTarget, cache.eglImage[i]);

    if (firstFrame)
    {
      GLenum glIformat = DrmFormatToGLIformat(planes[i].drmFormat);
#ifdef DRM_FORMAT_P030
      // P030: packed 10-bit in 32-bit container, per-plane R16/RG16
      if (planes[i].drmFormat == DRM_FORMAT_P030)
        glIformat = (i == 0) ? GL_R16 : GL_RG16;
#endif
      if (glIformat == 0)
      {
        CLog::Log(LOGERROR,
                  "CLinuxRendererPLBase::{} - unsupported DRM fourcc 0x{:x} for plane {}",
                  callerName, planes[i].drmFormat, i);
        success = false;
        break;
      }

      pl_opengl_wrap_params wp{};
      wp.texture = cache.glTex[i];
      wp.target = texTarget;
      wp.iformat = static_cast<int>(glIformat);
      wp.width = planes[i].width;
      wp.height = planes[i].height;

      cache.plTex[i] = pl_opengl_wrap(gpu, &wp);
      if (!cache.plTex[i])
      {
        CLog::Log(LOGERROR, "CLinuxRendererPLBase::{} - pl_opengl_wrap failed for plane {}",
                  callerName, i);
        success = false;
      }
      cache.width[i] = planes[i].width;
      cache.height[i] = planes[i].height;
    }
  }

  if (!success)
  {
    if (firstFrame)
    {
      for (int n = 0; n < numPlanes; ++n)
      {
        if (cache.plTex[n])
          pl_tex_destroy(gpu, &cache.plTex[n]);
        if (cache.eglImage[n] != EGL_NO_IMAGE_KHR)
          m_eglDestroyImageKHR(m_eglDisplay, cache.eglImage[n]);
        cache.eglImage[n] = EGL_NO_IMAGE_KHR;
      }
      glDeleteTextures(numPlanes, cache.glTex);
      cache = {};
    }
    return false;
  }

  if (firstFrame)
  {
    cache.numPlanes = numPlanes;
    cache.initialized = true;
  }

  // Copy cached pl_tex pointers into the per-frame PLBuffer
  for (int i = 0; i < cache.numPlanes; ++i)
    plbuf.tex[i] = cache.plTex[i];

  return true;
}

// ---------------------------------------------------------------------------
// UploadVAAPI — VAAPI: export surface as DRM PRIME 2 → EGLImage → GL tex → pl_opengl_wrap
// ---------------------------------------------------------------------------

#if defined(HAVE_LIBVA)
template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadVAAPI(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];

  auto* vaaPic = dynamic_cast<VAAPI::CVaapiRenderPicture*>(buf.videoBuffer);
  if (!vaaPic)
    return false;

  if (!m_eglCreateImageKHR || !m_eglDestroyImageKHR || !m_glEGLImageTargetTexture2DOES)
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadVAAPI - EGL interop not available");
    return false;
  }

  VASurfaceID surface = vaaPic->procPic.videoSurface;
  if (surface == VA_INVALID_ID && vaaPic->avFrame)
    surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(vaaPic->avFrame->data[3]));
  if (surface == VA_INVALID_ID)
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadVAAPI - no valid VAAPI surface");
    return false;
  }

  const VADisplay vadsp = vaaPic->vadsp;
  // Synchronize the VAAPI surface before we import its DMA-bufs.
  //
  // Priority order (best → worst):
  //  1. Implicit fencing (m_hasEGLModifiers): the kernel/EGL stack inserts a
  //     DMA-buf fence automatically when eglCreateImageKHR is called.  No
  //     explicit sync needed at all.
  //  2. EGL_ANDROID_native_fence_sync (m_hasEGLSyncFence): export a sync-file fd
  //     from the DMA-buf via DMA_BUF_IOCTL_EXPORT_SYNC_FILE, then import it as an
  //     EGL Sync Object.  eglWaitSyncKHR (called in RenderHook) inserts the
  //     wait into the GPU command stream — the CPU thread returns immediately.
  //  3. vaSyncSurface(): CPU-blocking stall.  Used only when neither of the
  //     above is available.
  if (!m_hasEGLModifiers)
  {
    bool syncHandled = false;
#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
    if (m_hasEGLSyncFence)
    {
      // vaExportSurfaceHandle (called below) gives us the DMA-buf fds.
      // We need to export the fence AFTER vaExportSurfaceHandle so the
      // descriptor is populated.  Defer to after the export call by setting
      // a flag; we'll do the ioctl there.
      syncHandled = true; // fence will be created after vaExportSurfaceHandle
    }
#endif
    if (!syncHandled)
      vaSyncSurface(vadsp, surface);
  }

  VADRMPRIMESurfaceDescriptor desc{};
  const VAStatus vaStatus =
      vaExportSurfaceHandle(vadsp, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
  if (vaStatus != VA_STATUS_SUCCESS)
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadVAAPI - vaExportSurfaceHandle failed: {}",
              vaErrorStr(vaStatus));
    return false;
  }

  plbuf.vaapiNumFds = static_cast<int>(
      std::min(desc.num_objects, static_cast<uint32_t>(std::size(plbuf.vaapiExportedFd))));
  for (int obj = 0; obj < plbuf.vaapiNumFds; ++obj)
    plbuf.vaapiExportedFd[obj] = desc.objects[obj].fd;

  // GPU-side fence: export DMA-buf read fence as EGL sync object.
  // Falls back to CPU sync (vaSyncSurface) if the ioctl is unavailable.
  if (plbuf.vaapiNumFds > 0)
  {
    ExportDmaBufSyncFence(plbuf.vaapiExportedFd[0], index, plbuf);
    if (!m_hasEGLModifiers && plbuf.eglSyncFence == EGL_NO_SYNC_KHR)
      vaSyncSurface(vadsp, surface);
  }

  const GLenum vaapiTexTarget = GetVaapiTexTarget();
  const uint32_t numLayers = std::min(desc.num_layers, kMaxPlanes);

  const ChromaDiv chroma = ChromaDivFromVaFourcc(desc.fourcc);
  if (!chroma.known)
  {
    CLog::Log(LOGERROR,
              "CLinuxRendererPLBase::UploadVAAPI - unknown VA fourcc 0x{:x}, "
              "cannot determine chroma subsampling",
              desc.fourcc);
    return false;
  }

  // Build per-plane descriptors for the shared import helper.
  PlaneImportDesc importDescs[kMaxPlanes]{};
  for (uint32_t i = 0; i < numLayers; ++i)
  {
    const auto& layer = desc.layers[i];
    const auto& object = desc.objects[layer.object_index[0]];
    importDescs[i].drmFormat = layer.drm_format;
    importDescs[i].width =
        (i == 0) ? static_cast<int>(desc.width)
                 : (static_cast<int>(desc.width) + chroma.widthDiv - 1) / chroma.widthDiv;
    importDescs[i].height =
        (i == 0) ? static_cast<int>(desc.height)
                 : (static_cast<int>(desc.height) + chroma.heightDiv - 1) / chroma.heightDiv;
    importDescs[i].fd = object.fd;
    importDescs[i].offset = static_cast<int>(layer.offset[0]);
    importDescs[i].pitch = static_cast<int>(layer.pitch[0]);
    importDescs[i].modifier = object.drm_format_modifier;
  }

  PlaneCache& cache = m_vaapiCache[index];
  if (!ImportPlanesToCache(cache, importDescs, static_cast<int>(numLayers), vaapiTexTarget, plbuf,
                           "UploadVAAPI"))
  {
    ReleasePLBuffer(index);
    return false;
  }

  // Sync EGLImage pointers into plbuf for ReleasePLBuffer's cleanup path.
  for (uint32_t i = 0; i < numLayers; ++i)
    plbuf.vaapiEGLImage[i] = cache.eglImage[i];
  // Copy GL tex IDs for ReleasePLBuffer compatibility
  for (int i = 0; i < cache.numPlanes; ++i)
    plbuf.vaapiGLTex[i] = cache.glTex[i];

  plbuf.num_planes = static_cast<int>(numLayers);
  SetYCbCrPlaneMapping(plbuf, plbuf.num_planes, /*planarChroma=*/false);

  SetSourceColorSpace(plbuf, buf, /*isYCbCr=*/true);

  switch (desc.fourcc)
  {
    case VA_FOURCC_P010:
      plbuf.colorRepr.bits = {16, 10, 6};
      break;
    case VA_FOURCC_P016:
      plbuf.colorRepr.bits = {16, 16, 0};
      break;
    default:
      break;
  }

  PL::ApplyHdrMetadata(plbuf.colorSpace, plbuf.colorRepr, plbuf.doviMetadata, buf);
  PL::ValidateInputColorSpace(plbuf.colorSpace, plbuf.colorRepr);

  plbuf.iFlags = buf.iFlags;
  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}
#endif // HAVE_LIBVA

// ---------------------------------------------------------------------------
// UploadDRMPRIME — DMA-buf → single combined OES texture → pl_tex
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadDRMPRIME(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];

  auto* drmBuf = dynamic_cast<CVideoBufferDRMPRIME*>(buf.videoBuffer);
  if (!drmBuf)
    return false;

  // Skip re-import when displaying the same source frame again (e.g. 24fps
  // content re-rendered at 60Hz display rate).  The EGLImage and pl_tex from
  // the previous upload are still valid — only the render output changes.
  if (buf.videoBuffer == m_lastUploadedBuffer[index] && plbuf.loaded)
    return true;

  // DV content: per-plane import for full RPU reshaping in libplacebo.
  // Requires EGL interop and that the DRM format can be split into single-
  // component planes. Falls through to the fast single-OES path on failure.
  if (buf.plColorRepr.dovi != nullptr && m_eglCreateImageKHR)
  {
    if (UploadDRMPRIMEPlanes(index, plbuf))
    {
      m_lastUploadedBuffer[index] = buf.videoBuffer;
      return true;
    }
    CLog::Log(LOGWARNING, "CLinuxRendererPLBase::UploadDRMPRIME - per-plane DV import failed, "
                          "falling back to single-OES (DV reshaping will be unavailable)");
  }

  m_drmTextures[index].Reset(); // defensive — no-op if not mapped
  if (!m_drmTextures[index].Import(drmBuf))
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadDRMPRIME - CDRMPRIMETexture::Import failed");
    return false;
  }

  const pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  const GLuint glTex = m_drmTextures[index].GetTexture();
  const CSizeInt sz = m_drmTextures[index].GetTextureSize();

  // Reuse cached pl_tex wrapper when the underlying GL texture and dimensions
  // haven't changed. pl_opengl_wrap + pl_tex_destroy cost a glGetError() stall
  // inside libplacebo (gl_check_err), plus mutex round-trips and a heap alloc/free.
  // The GL texture ID is stable for a given DRMPRIME slot; only the EGL image
  // behind it changes per frame, which pl_opengl_wrap doesn't interact with.
  auto& cache = m_drmTexCache[index];
  if (cache.tex && cache.glTex == glTex && cache.width == sz.Width() && cache.height == sz.Height())
  {
    plbuf.tex[0] = cache.tex;
  }
  else
  {
    if (cache.tex)
      pl_tex_destroy(gpu, &cache.tex);

    pl_opengl_wrap_params wp{};
    wp.texture = glTex;
    wp.target = GL_TEXTURE_EXTERNAL_OES;
    // GL_TEXTURE_EXTERNAL_OES is opaque to GL — the EGL/DRM layer controls
    // the actual pixel format internally. libplacebo cannot map format-specific
    // ifomats (GL_RGB10_A2, GL_RGBA16F) for external OES targets. Pass GL_RGBA8
    // for all bit depths; actual precision is carried via color metadata.
    wp.iformat = static_cast<int>(GL_RGBA8);
    wp.width = sz.Width();
    wp.height = sz.Height();

    plbuf.tex[0] = pl_opengl_wrap(gpu, &wp);
    if (!plbuf.tex[0])
    {
      CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadDRMPRIME - pl_opengl_wrap failed");
      m_drmTextures[index].Reset();
      return false;
    }

    cache.tex = plbuf.tex[0];
    cache.glTex = glTex;
    cache.width = sz.Width();
    cache.height = sz.Height();
  }

  plbuf.planes[0] = {};
  plbuf.planes[0].texture = plbuf.tex[0];
  plbuf.planes[0].components = 3;
  plbuf.planes[0].component_mapping[0] = PL_CHANNEL_R;
  plbuf.planes[0].component_mapping[1] = PL_CHANNEL_G;
  plbuf.planes[0].component_mapping[2] = PL_CHANNEL_B;
  plbuf.planes[0].component_mapping[3] = PL_CHANNEL_NONE;
  plbuf.planes[0].flipped = true; // DMA-buf row 0 = top of video, GL texcoord t=0 = bottom
  plbuf.num_planes = 1;

  // OES: GPU driver already applied YCbCr→RGB when sampling.
  SetSourceColorSpace(plbuf, buf, /*isYCbCr=*/false);

  PL::ApplyHdrMetadata(plbuf.colorSpace, plbuf.colorRepr, plbuf.doviMetadata, buf);

  // DV reshaping curves operate on YCbCr input and cannot be applied on
  // post-conversion RGB data. Keep DV-informed HDR metadata (luminance from
  // RPU) for tone mapping, but clear the reshaping pointer.
  plbuf.colorRepr.dovi = nullptr;

  PL::ValidateInputColorSpace(plbuf.colorSpace, plbuf.colorRepr);

  plbuf.iFlags = buf.iFlags;

  // GPU-side DMA-buf fence for V4L2/DRMPRIME decode path.
  {
    const AVDRMFrameDescriptor* desc = drmBuf->GetDescriptor();
    if (desc && desc->nb_objects > 0)
      ExportDmaBufSyncFence(desc->objects[0].fd, index, plbuf);
  }

  m_lastUploadedBuffer[index] = buf.videoBuffer;
  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}

// ---------------------------------------------------------------------------
// SetSourceColorSpace — populate PLBuffer color metadata from source stream
// ---------------------------------------------------------------------------

template<typename TBase>
template<typename TBuffer>
void CLinuxRendererPLBase<TBase>::SetSourceColorSpace(PLBuffer& plbuf,
                                                      const TBuffer& buf,
                                                      bool isYCbCr)
{
  plbuf.colorSpace = {};
  plbuf.colorRepr = {};
  plbuf.colorSpace.primaries = pl_primaries_from_av(buf.m_srcPrimaries);
  plbuf.colorSpace.transfer = pl_transfer_from_av(buf.m_srcColTransfer);
  if (isYCbCr)
  {
    plbuf.colorRepr.sys = pl_system_from_av(buf.m_srcColSpace);
    if (plbuf.colorRepr.sys == PL_COLOR_SYSTEM_UNKNOWN)
      plbuf.colorRepr.sys = pl_color_system_guess_ycbcr(this->m_sourceWidth, this->m_sourceHeight);
  }
  else
  {
    plbuf.colorRepr.sys = PL_COLOR_SYSTEM_RGB;
  }
  plbuf.colorRepr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
}

// ---------------------------------------------------------------------------
// ExportDmaBufSyncFence — export DMA-buf read fence as EGL sync object
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::ExportDmaBufSyncFence(int dmaBufFd,
                                                        int slotIndex,
                                                        PLBuffer& plbuf)
{
#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
  if (!m_hasEGLSyncFence || m_hasEGLModifiers)
    return;

  dma_buf_export_sync_file syncExport{};
  syncExport.flags = DMA_BUF_SYNC_READ;
  syncExport.fd = -1;
  if (ioctl(dmaBufFd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &syncExport) == 0 && syncExport.fd >= 0)
  {
    const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, syncExport.fd, EGL_NONE};
    plbuf.eglSyncFence = m_eglCreateSyncKHR(m_eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (plbuf.eglSyncFence == EGL_NO_SYNC_KHR)
      close(syncExport.fd);
    else
      m_pendingSyncSlots |= (1u << slotIndex);
  }
#endif
}

// ---------------------------------------------------------------------------
// BuildDmaBufEGLAttribs — build EGL attrib list for a single DMA-buf plane
// ---------------------------------------------------------------------------

template<typename TBase>
EGLint* CLinuxRendererPLBase<TBase>::BuildDmaBufEGLAttribs(EGLint* a,
                                                           uint32_t drmFormat,
                                                           int width,
                                                           int height,
                                                           int fd,
                                                           int offset,
                                                           int pitch,
                                                           uint64_t modifier) const
{
  *a++ = EGL_LINUX_DRM_FOURCC_EXT;
  *a++ = static_cast<EGLint>(drmFormat);
  *a++ = EGL_WIDTH;
  *a++ = static_cast<EGLint>(width);
  *a++ = EGL_HEIGHT;
  *a++ = static_cast<EGLint>(height);
  *a++ = EGL_DMA_BUF_PLANE0_FD_EXT;
  *a++ = fd;
  *a++ = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
  *a++ = static_cast<EGLint>(offset);
  *a++ = EGL_DMA_BUF_PLANE0_PITCH_EXT;
  *a++ = static_cast<EGLint>(pitch);
  if (m_hasEGLModifiers && modifier != DRM_FORMAT_MOD_INVALID)
  {
    *a++ = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    *a++ = static_cast<EGLint>(modifier & 0xFFFFFFFFu);
    *a++ = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    *a++ = static_cast<EGLint>(modifier >> 32);
  }
  *a = EGL_NONE;
  return a;
}

// ---------------------------------------------------------------------------
// SetTextureDefaults — common texture parameters for DMA-buf imported textures
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::SetTextureDefaults(GLenum target)
{
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// ---------------------------------------------------------------------------
// DrmFormatToGLIformat — map single-plane DRM fourcc to GL internal format
// ---------------------------------------------------------------------------

template<typename TBase>
GLenum CLinuxRendererPLBase<TBase>::DrmFormatToGLIformat(uint32_t drmFormat)
{
  switch (drmFormat)
  {
    case DRM_FORMAT_R8:
      return GL_R8;
    case DRM_FORMAT_GR88:
      return GL_RG8;
    case DRM_FORMAT_R16:
      return GL_R16;
    case DRM_FORMAT_GR1616:
      return GL_RG16;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
      return GL_RGBA8;
    default:
      return 0;
  }
}

// ---------------------------------------------------------------------------
// SetYCbCrPlaneMapping — set up Y/CbCr/Cr pl_plane component mapping
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::SetYCbCrPlaneMapping(PLBuffer& plbuf,
                                                       int numPlanes,
                                                       bool planarChroma)
{
  plbuf.planes[0] = {};
  plbuf.planes[0].texture = plbuf.tex[0];
  plbuf.planes[0].components = 1;
  plbuf.planes[0].component_mapping[0] = PL_CHANNEL_Y;
  plbuf.planes[0].component_mapping[1] = PL_CHANNEL_NONE;
  plbuf.planes[0].component_mapping[2] = PL_CHANNEL_NONE;
  plbuf.planes[0].component_mapping[3] = PL_CHANNEL_NONE;
  plbuf.planes[0].flipped = true;

  if (numPlanes >= 2)
  {
    plbuf.planes[1] = {};
    plbuf.planes[1].texture = plbuf.tex[1];
    if (planarChroma)
    {
      plbuf.planes[1].components = 1;
      plbuf.planes[1].component_mapping[0] = PL_CHANNEL_CB;
    }
    else
    {
      plbuf.planes[1].components = 2;
      plbuf.planes[1].component_mapping[0] = PL_CHANNEL_CB;
      plbuf.planes[1].component_mapping[1] = PL_CHANNEL_CR;
    }
    plbuf.planes[1].component_mapping[2] = PL_CHANNEL_NONE;
    plbuf.planes[1].component_mapping[3] = PL_CHANNEL_NONE;
    plbuf.planes[1].flipped = true;
  }

  if (numPlanes >= 3)
  {
    plbuf.planes[2] = {};
    plbuf.planes[2].texture = plbuf.tex[2];
    plbuf.planes[2].components = 1;
    plbuf.planes[2].component_mapping[0] = PL_CHANNEL_CR;
    plbuf.planes[2].component_mapping[1] = PL_CHANNEL_NONE;
    plbuf.planes[2].component_mapping[2] = PL_CHANNEL_NONE;
    plbuf.planes[2].component_mapping[3] = PL_CHANNEL_NONE;
    plbuf.planes[2].flipped = true;
  }
}

// ---------------------------------------------------------------------------
// ChromaDivFromDrmFormat — chroma subsampling for combined DRM pixel formats
// ---------------------------------------------------------------------------

template<typename TBase>
typename CLinuxRendererPLBase<TBase>::ChromaDiv
CLinuxRendererPLBase<TBase>::ChromaDivFromDrmFormat(uint32_t drmFormat)
{
  switch (drmFormat)
  {
    // 4:2:0
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_P010:
#ifdef DRM_FORMAT_P030
    case DRM_FORMAT_P030:
#endif
    case DRM_FORMAT_YUV420:
      return {2, 2, true};
    // 4:2:2
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_P210:
    case DRM_FORMAT_YUV422:
      return {2, 1, true};
    // 4:4:4
    case DRM_FORMAT_YUV444:
      return {1, 1, true};
    default:
      return {};
  }
}

// ---------------------------------------------------------------------------
// ChromaDivFromVaFourcc — chroma subsampling for VA-API surface formats
// ---------------------------------------------------------------------------

#if defined(HAVE_LIBVA)
template<typename TBase>
typename CLinuxRendererPLBase<TBase>::ChromaDiv
CLinuxRendererPLBase<TBase>::ChromaDivFromVaFourcc(uint32_t vaFourcc)
{
  switch (vaFourcc)
  {
    // 4:2:0
    case VA_FOURCC_NV12:
    case VA_FOURCC_P010:
    case VA_FOURCC_P016:
    case VA_FOURCC_YV12:
    case VA_FOURCC_I420:
    case VA_FOURCC_IMC3:
      return {2, 2, true};
    // 4:2:2
    case VA_FOURCC_YUY2:
    case VA_FOURCC_UYVY:
    case VA_FOURCC_Y210:
    case VA_FOURCC_Y212:
      return {2, 1, true};
    // 4:4:4
    case VA_FOURCC_Y410:
    case VA_FOURCC_Y412:
    case VA_FOURCC_AYUV:
    case VA_FOURCC_XYUV:
      return {1, 1, true};
    default:
      return {};
  }
}
#endif // HAVE_LIBVA

// ---------------------------------------------------------------------------
// SplitDrmFormat — map combined DRM multi-plane formats to per-plane formats
// ---------------------------------------------------------------------------

template<typename TBase>
typename CLinuxRendererPLBase<TBase>::DRMPlaneSplit
CLinuxRendererPLBase<TBase>::SplitDrmFormat(uint32_t layerFormat, int numPlanes)
{
  DRMPlaneSplit s;
  if (numPlanes < 2)
    return s;

  s.chroma = ChromaDivFromDrmFormat(layerFormat);
  if (!s.chroma.known)
    return s;

  switch (layerFormat)
  {
    // Semi-planar 8-bit (R8 luma + GR88 chroma)
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV16:
      s.drmFormat[0] = DRM_FORMAT_R8;
      s.drmFormat[1] = DRM_FORMAT_GR88;
      break;
    // Semi-planar 10/16-bit (R16 luma + GR1616 chroma)
    case DRM_FORMAT_P010:
    case DRM_FORMAT_P210:
#ifdef DRM_FORMAT_P030
    case DRM_FORMAT_P030:
#endif
      s.drmFormat[0] = DRM_FORMAT_R16;
      s.drmFormat[1] = DRM_FORMAT_GR1616;
      break;
    // Planar 8-bit (three R8 planes)
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YUV444:
      s.drmFormat[0] = s.drmFormat[1] = s.drmFormat[2] = DRM_FORMAT_R8;
      break;
    default:
      // chroma was known but no plane split defined — reset to unknown
      s.chroma = {};
      return s;
  }
  return s;
}

// ---------------------------------------------------------------------------
// UploadDRMPRIMEPlanes — per-plane DMA-buf import for Dolby Vision content
// ---------------------------------------------------------------------------
//
// Imports each YCbCr plane of the DMA-buf as a separate EGLImage → GL texture
// → pl_tex, giving libplacebo raw plane access so DV RPU reshaping curves can
// be applied in the shader.  Mirrors the VAAPI upload logic but reads from
// AVDRMFrameDescriptor instead of VADRMPRIMESurfaceDescriptor.

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadDRMPRIMEPlanes(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];
  auto* drmBuf = dynamic_cast<CVideoBufferDRMPRIME*>(buf.videoBuffer);
  if (!drmBuf)
    return false;

  if (!drmBuf->AcquireDescriptor())
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadDRMPRIMEPlanes - AcquireDescriptor failed");
    return false;
  }

  const AVDRMFrameDescriptor* desc = drmBuf->GetDescriptor();
  if (!desc || desc->nb_layers == 0)
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadDRMPRIMEPlanes - no layers in descriptor");
    drmBuf->ReleaseDescriptor();
    return false;
  }

  const AVDRMLayerDescriptor* layer = &desc->layers[0];
  const int numPlanes = layer->nb_planes;
  const int fullW = static_cast<int>(drmBuf->GetWidth());
  const int fullH = static_cast<int>(drmBuf->GetHeight());

  const DRMPlaneSplit split = SplitDrmFormat(layer->format, numPlanes);
  if (!split.chroma.known)
  {
    CLog::Log(LOGERROR,
              "CLinuxRendererPLBase::UploadDRMPRIMEPlanes - unsupported DRM format 0x{:x} "
              "with {} planes, falling back to OES path",
              layer->format, numPlanes);
    drmBuf->ReleaseDescriptor();
    return false;
  }

  const int chromaW = (fullW + split.chroma.widthDiv - 1) / split.chroma.widthDiv;
  const int chromaH = (fullH + split.chroma.heightDiv - 1) / split.chroma.heightDiv;

  // Build per-plane descriptors for the shared import helper.
  PlaneImportDesc importDescs[kMaxPlanes]{};
  for (int i = 0; i < numPlanes; ++i)
  {
    const auto& obj = desc->objects[layer->planes[i].object_index];
    importDescs[i].drmFormat = split.drmFormat[i];
    importDescs[i].width = (i == 0) ? fullW : chromaW;
    importDescs[i].height = (i == 0) ? fullH : chromaH;
    importDescs[i].fd = obj.fd;
    importDescs[i].offset = static_cast<int>(layer->planes[i].offset);
    importDescs[i].pitch = static_cast<int>(layer->planes[i].pitch);
    importDescs[i].modifier = obj.format_modifier;
  }

  // Per-plane texture target: GL_TEXTURE_2D on desktop GL, GL_TEXTURE_EXTERNAL_OES
  // on GLES.  Some GLES drivers (V3D, Mali) require OES for any DMA-buf EGLImage,
  // even single-component formats like R8/GR88.
  const GLenum texTarget = GetVaapiTexTarget();
  PlaneCache& cache = m_drmPlaneCache[index];

  if (!ImportPlanesToCache(cache, importDescs, numPlanes, texTarget, plbuf,
                           "UploadDRMPRIMEPlanes"))
  {
    drmBuf->ReleaseDescriptor();
    return false;
  }

  plbuf.usedPerPlaneImport = true;
  plbuf.num_planes = cache.numPlanes;

  // Planar formats (YUV420/422/444) have 3 planes with 1 component each;
  // semi-planar (NV12, P010, etc.) have 2 planes with interleaved CbCr.
  SetYCbCrPlaneMapping(plbuf, cache.numPlanes, /*planarChroma=*/cache.numPlanes >= 3);

  // YCbCr color space so DV reshaping curves can apply (unlike OES/RGB path).
  SetSourceColorSpace(plbuf, buf, /*isYCbCr=*/true);

  // Bit encoding for 10-bit formats (P010: 10 bits in 16-bit container, 6-bit shift)
  if (layer->format == DRM_FORMAT_P010 || layer->format == DRM_FORMAT_P210)
    plbuf.colorRepr.bits = {16, 10, 6};

  PL::ApplyHdrMetadata(plbuf.colorSpace, plbuf.colorRepr, plbuf.doviMetadata, buf);
  PL::ValidateInputColorSpace(plbuf.colorSpace, plbuf.colorRepr);

  plbuf.iFlags = buf.iFlags;

  if (desc->nb_objects > 0)
    ExportDmaBufSyncFence(desc->objects[0].fd, index, plbuf);

  drmBuf->ReleaseDescriptor();
  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}

// ---------------------------------------------------------------------------
// ReleaseSWBuffer — frees the persistent PBOs and GL textures for one slot
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::ReleaseSWBuffer(int index)
{
  SWBuffer& sw = m_swBuffers[index];
  // GL spec: zero names in the array are silently ignored by glDelete{Buffers,Textures}.
  glDeleteBuffers(std::size(sw.pbo), sw.pbo);
  glDeleteTextures(std::size(sw.tex), sw.tex);
  std::memset(&sw, 0, sizeof(sw));
}

// ---------------------------------------------------------------------------
// PlaneDataToGLFormats — map pl_plane_data layout to GL format/type/iformat
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::PlaneDataToGLFormats(
    const pl_plane_data& pd, GLenum& iformat, GLenum& format, GLenum& type, int& bytesPerPixel)
{
  int numComp = 0;
  for (const int c : pd.component_size)
    if (c > 0)
      ++numComp;
  if (numComp == 0)
    return false;

  bytesPerPixel = static_cast<int>(pd.pixel_stride);
  const bool wide = (bytesPerPixel > numComp); // >1 byte per component: 16-bit container

  switch (numComp)
  {
    case 1:
      iformat = wide ? GL_R16 : GL_R8;
      format = GL_RED;
      type = wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
      return true;
    case 2:
      iformat = wide ? GL_RG16 : GL_RG8;
      format = GL_RG;
      type = wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
      return true;
    case 3:
      iformat = wide ? GL_RGB16 : GL_RGB8;
      format = GL_RGB;
      type = wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
      return true;
    case 4:
      iformat = wide ? GL_RGBA16 : GL_RGBA8;
      format = GL_RGBA;
      type = wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// UploadSoftware — async CPU-to-GPU upload via PBO + pl_opengl_wrap
//
// Replaces synchronous pl_upload_plane with a PBO pipeline:
//   1. Orphan PBO each frame  ->  no CPU/GPU sync stall
//   2. memcpy frame data into the mapped PBO (CPU-side write)
//   3. glTexSubImage2D with PBO bound  ->  driver schedules async DMA
//   4. pl_opengl_wrap wraps the persistent GL texture for libplacebo
//
// The GL textures (m_swBuffers) survive ReleasePLBuffer; only the lightweight
// pl_opengl_wrap descriptor (plbuf.tex[n]) is recreated each frame.
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadSoftware(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];
  SWBuffer& sw = m_swBuffers[index];

  uint8_t* src[3]{};
  int srcStrides[3]{};
  buf.videoBuffer->GetPlanes(src);
  buf.videoBuffer->GetStrides(srcStrides);

  pl_bit_encoding bits{};
  pl_plane_data pdata[4]{};
  const AVPixelFormat fmt = m_format;
  plbuf.num_planes = pl_plane_data_from_pixfmt(pdata, &bits, fmt);
  if (plbuf.num_planes <= 0)
  {
    CLog::Log(LOGERROR,
              "CLinuxRendererPLBase::UploadSoftware - unsupported pixel format {} (buf reports {})",
              static_cast<int>(fmt), static_cast<int>(buf.videoBuffer->GetFormat()));
    return false;
  }

  // Derive chroma plane dimensions. Do NOT use AV_CEIL_RSHIFT(a, b) with a
  // runtime b -- the macro is implementation-defined for signed integers when b
  // is not a compile-time constant. Use the explicit ceiling-shift formula.
  const AVPixFmtDescriptor* fmtDesc = av_pix_fmt_desc_get(fmt);
  const int chromaShiftW = fmtDesc ? static_cast<int>(fmtDesc->log2_chroma_w) : 1;
  const int chromaShiftH = fmtDesc ? static_cast<int>(fmtDesc->log2_chroma_h) : 1;

  const pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;

  for (int n = 0; n < plbuf.num_planes; ++n)
  {
    const int planeW = (n > 0) ? (this->m_sourceWidth + (1 << chromaShiftW) - 1) >> chromaShiftW
                               : this->m_sourceWidth;
    const int planeH = (n > 0) ? (this->m_sourceHeight + (1 << chromaShiftH) - 1) >> chromaShiftH
                               : this->m_sourceHeight;

    GLenum iformat, glFormat, glType;
    int bytesPerPixel;
    if (!PlaneDataToGLFormats(pdata[n], iformat, glFormat, glType, bytesPerPixel))
    {
      CLog::Log(LOGERROR,
                "CLinuxRendererPLBase::UploadSoftware - no GL format mapping for plane {}", n);
      return false;
    }

    // Reallocate the GL texture only when dimensions or format change.
    if (sw.tex[n] == 0 || sw.texW[n] != planeW || sw.texH[n] != planeH ||
        sw.texIformat[n] != iformat)
    {
      // The underlying GL texture is being reallocated; the old pl_tex wrapper
      // would hold a stale texture ID, so invalidate it now.
      if (plbuf.tex[n])
      {
        pl_tex_destroy(gpu, &plbuf.tex[n]);
        plbuf.tex[n] = nullptr;
      }

      if (sw.tex[n] == 0)
        glGenTextures(1, &sw.tex[n]);
      glBindTexture(GL_TEXTURE_2D, sw.tex[n]);
      SetTextureDefaults(GL_TEXTURE_2D);
      glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(iformat), planeW, planeH, 0, glFormat,
                   glType, nullptr);
      glBindTexture(GL_TEXTURE_2D, 0);

      sw.texW[n] = planeW;
      sw.texH[n] = planeH;
      sw.texIformat[n] = iformat;
    }

    if (sw.pbo[n] == 0)
      glGenBuffers(1, &sw.pbo[n]);

    const GLsizeiptr pboSize = static_cast<GLsizeiptr>(srcStrides[n]) * planeH;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sw.pbo[n]);

    // (Re)allocate the PBO storage when size changes. glBufferData with nullptr
    // orphans any previous allocation without a CPU stall.
    if (sw.pboSize[n] != pboSize)
    {
      glBufferData(GL_PIXEL_UNPACK_BUFFER, pboSize, nullptr, GL_STREAM_DRAW);
      sw.pboSize[n] = pboSize;
    }

    // GL_MAP_INVALIDATE_BUFFER_BIT orphans the current content, avoiding pipeline
    // stalls if the GPU is still reading from the previous upload. This replaces
    // the old glBufferData(nullptr) + glMapBuffer pattern and is GLES 3.0 compatible.
    void* pboPtr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, pboSize,
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (!pboPtr)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      CLog::Log(LOGERROR,
                "CLinuxRendererPLBase::UploadSoftware - glMapBufferRange failed for plane {}", n);
      return false;
    }

    std::memcpy(pboPtr, src[n], static_cast<size_t>(srcStrides[n]) * planeH);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

    // With a PBO bound, glTexSubImage2D reads from the PBO's GPU address space
    // (nullptr = offset 0 into the PBO) rather than CPU memory. The driver can
    // schedule this DMA transfer asynchronously.
    const GLint rowLengthPixels = srcStrides[n] / bytesPerPixel;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLengthPixels);
    glBindTexture(GL_TEXTURE_2D, sw.tex[n]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, planeW, planeH, glFormat, glType, nullptr);
    // Don't unbind here — next iteration rebinds immediately. One unbind after loop.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // Wrap the persistent GL texture for libplacebo. pl_opengl_wrap allocates
    // only a small descriptor struct -- no GL calls. pl_tex_destroy (called in
    // ReleasePLBuffer) releases the wrapper without deleting the GL texture
    // because libplacebo honours the "external owner" contract for wrapped textures.
    if (!plbuf.tex[n])
    {
      pl_opengl_wrap_params wp{};
      wp.texture = sw.tex[n];
      wp.target = GL_TEXTURE_2D;
      wp.iformat = static_cast<int>(iformat);
      wp.width = planeW;
      wp.height = planeH;
      plbuf.tex[n] = pl_opengl_wrap(gpu, &wp);
      if (!plbuf.tex[n])
      {
        CLog::Log(LOGERROR,
                  "CLinuxRendererPLBase::UploadSoftware - pl_opengl_wrap failed for plane {}", n);
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
      }
    }

    // Build the pl_plane descriptor using pl_plane_data's component mapping.
    plbuf.planes[n] = {};
    plbuf.planes[n].texture = plbuf.tex[n];
    for (int c = 0; c < 4; ++c)
    {
      if (pdata[n].component_size[c] > 0)
        plbuf.planes[n].component_mapping[plbuf.planes[n].components++] =
            static_cast<pl_channel>(pdata[n].component_map[c]);
    }
    // OpenGL's coordinate system is bottom-up; top-down memory uploads are inverted.
    plbuf.planes[n].flipped = true;
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  SetSourceColorSpace(plbuf, buf, /*isYCbCr=*/true);
  plbuf.colorRepr.bits = bits;

  PL::ApplyHdrMetadata(plbuf.colorSpace, plbuf.colorRepr, plbuf.doviMetadata, buf);
  PL::ValidateInputColorSpace(plbuf.colorSpace, plbuf.colorRepr);

  plbuf.iFlags = buf.iFlags;
  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}

#if defined(HAVE_NVDEC)
// ---------------------------------------------------------------------------
// UploadNVDEC — CUDA-GL interop into the software path's GL textures
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::ReleaseNVDEC(int index)
{
  NVDECBuffer& nb = m_nvdecBuffers[index];
  if (!nb.resource[0] && !nb.resource[1])
    return;

  CudaFunctions* cu = NVDEC::GetCudaFunctions();
  const bool pushed = m_nvdecContext && cu->cuCtxPushCurrent(m_nvdecContext) == CUDA_SUCCESS;
  for (auto& resource : nb.resource)
  {
    if (resource && pushed)
      cu->cuGraphicsUnregisterResource(resource);
    resource = nullptr;
  }
  if (pushed)
  {
    CUcontext dummy;
    cu->cuCtxPopCurrent(&dummy);
  }
  nb = {};
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadNVDEC(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];
  auto* nvdec = dynamic_cast<NVDEC::CVideoBufferNVDEC*>(buf.videoBuffer);
  const AVFrame* frame = nvdec ? nvdec->GetFrame() : nullptr;
  if (!frame || !nvdec->GetCudaContext())
    return false;

  CudaFunctions* cu = NVDEC::GetCudaFunctions();

  // Resources registered in another CUDA context are released with that context.
  if (m_nvdecContext != nvdec->GetCudaContext())
  {
    for (int i = 0; i < NUM_BUFFERS; ++i)
      ReleaseNVDEC(i);
    av_buffer_unref(&m_nvdecDevice);
    m_nvdecContext = nvdec->GetCudaContext();
    m_nvdecDevice = av_buffer_ref(nvdec->GetDeviceRef());
  }

  pl_bit_encoding bits{};
  pl_plane_data pdata[4]{};
  plbuf.num_planes = pl_plane_data_from_pixfmt(pdata, &bits, nvdec->GetSwFormat());
  if (plbuf.num_planes != 2)
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadNVDEC - unsupported format {}",
              static_cast<int>(nvdec->GetSwFormat()));
    return false;
  }

  SWBuffer& sw = m_swBuffers[index];
  NVDECBuffer& nb = m_nvdecBuffers[index];
  const pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  int planeW[2]{};
  int planeH[2]{};
  int bytesPerPixel[2]{};
  GLenum iformat[2]{};

  for (int n = 0; n < 2; ++n)
  {
    planeW[n] = n ? (frame->width + 1) / 2 : frame->width;
    planeH[n] = n ? (frame->height + 1) / 2 : frame->height;

    GLenum glFormat, glType;
    if (!PlaneDataToGLFormats(pdata[n], iformat[n], glFormat, glType, bytesPerPixel[n]))
      return false;

    if (sw.tex[n] == 0 || sw.texW[n] != planeW[n] || sw.texH[n] != planeH[n] ||
        sw.texIformat[n] != iformat[n])
    {
      if (plbuf.tex[n])
        pl_tex_destroy(gpu, &plbuf.tex[n]);
      ReleaseNVDEC(index);

      if (sw.tex[n] == 0)
        glGenTextures(1, &sw.tex[n]);
      glBindTexture(GL_TEXTURE_2D, sw.tex[n]);
      SetTextureDefaults(GL_TEXTURE_2D);
      glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(iformat[n]), planeW[n], planeH[n], 0,
                   glFormat, glType, nullptr);
      glBindTexture(GL_TEXTURE_2D, 0);

      sw.texW[n] = planeW[n];
      sw.texH[n] = planeH[n];
      sw.texIformat[n] = iformat[n];
    }
  }

  if (cu->cuCtxPushCurrent(m_nvdecContext) != CUDA_SUCCESS)
    return false;

  bool ok = true;
  for (int n = 0; n < 2 && ok; ++n)
  {
    if (nb.resource[n] && nb.registeredTex[n] == sw.tex[n])
      continue;
    ok = cu->cuGraphicsGLRegisterImage(&nb.resource[n], sw.tex[n], GL_TEXTURE_2D,
                                       CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD) == CUDA_SUCCESS;
    nb.registeredTex[n] = ok ? sw.tex[n] : 0;
  }

  const bool mapped =
      ok && cu->cuGraphicsMapResources(2, nb.resource, nullptr) == CUDA_SUCCESS;
  ok = mapped;
  for (int n = 0; n < 2 && ok; ++n)
  {
    CUarray array = nullptr;
    ok = cu->cuGraphicsSubResourceGetMappedArray(&array, nb.resource[n], 0, 0) == CUDA_SUCCESS;
    if (!ok)
      break;

    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = reinterpret_cast<CUdeviceptr>(frame->data[n]);
    copy.srcPitch = static_cast<size_t>(frame->linesize[n]);
    copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.dstArray = array;
    copy.WidthInBytes = static_cast<size_t>(planeW[n]) * bytesPerPixel[n];
    copy.Height = static_cast<size_t>(planeH[n]);
    ok = cu->cuMemcpy2D(&copy) == CUDA_SUCCESS;
  }
  // GL must not sample the textures while they are still mapped.
  if (mapped)
    ok = cu->cuGraphicsUnmapResources(2, nb.resource, nullptr) == CUDA_SUCCESS && ok;

  CUcontext dummy;
  cu->cuCtxPopCurrent(&dummy);

  if (!ok)
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadNVDEC - CUDA-GL copy failed");
    return false;
  }

  for (int n = 0; n < 2; ++n)
  {
    if (!plbuf.tex[n])
    {
      pl_opengl_wrap_params wp{};
      wp.texture = sw.tex[n];
      wp.target = GL_TEXTURE_2D;
      wp.iformat = static_cast<int>(iformat[n]);
      wp.width = planeW[n];
      wp.height = planeH[n];
      plbuf.tex[n] = pl_opengl_wrap(gpu, &wp);
      if (!plbuf.tex[n])
        return false;
    }

    plbuf.planes[n] = {};
    plbuf.planes[n].texture = plbuf.tex[n];
    for (int c = 0; c < 4; ++c)
    {
      if (pdata[n].component_size[c] > 0)
        plbuf.planes[n].component_mapping[plbuf.planes[n].components++] =
            static_cast<pl_channel>(pdata[n].component_map[c]);
    }
    // Rows are copied top-down like the software upload.
    plbuf.planes[n].flipped = true;
  }

  SetSourceColorSpace(plbuf, buf, /*isYCbCr=*/true);
  plbuf.colorRepr.bits = bits;

  PL::ApplyHdrMetadata(plbuf.colorSpace, plbuf.colorRepr, plbuf.doviMetadata, buf);
  PL::ValidateInputColorSpace(plbuf.colorSpace, plbuf.colorRepr);

  plbuf.iFlags = buf.iFlags;
  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}
#endif

// ---------------------------------------------------------------------------
// Main render hook
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::RenderHook(int idx)
{
  PLBuffer& plbuf = m_plBuffers[idx];
  if (!plbuf.loaded) [[unlikely]]
    return false;

  SyncOptions();

  const auto plInst = PL::PLInstance::Get();
  const pl_gpu gpu = plInst->GetGpu();
  const pl_renderer renderer = plInst->GetRenderer();

  pl_frame frameIn{};
  frameIn.num_planes = plbuf.num_planes;
  for (int n = 0; n < plbuf.num_planes; ++n)
    frameIn.planes[n] = plbuf.planes[n];
  frameIn.color = plbuf.colorSpace;
  frameIn.repr = plbuf.colorRepr;
  pl_frame_set_chroma_location(&frameIn, m_chromaLocation);
  frameIn.rotation = PL::RotationFromOrientation(this->m_renderOrientation);
  frameIn.field = PL_FIELD_NONE;

  CRect src, dst, view;
  this->GetVideoRect(src, dst, view);

  // Source crop: tells libplacebo which region of the video texture to display.
  // Without this, zoom, pixel ratio, and stretch features have no effect on the
  // source side — the entire texture is always rendered into the destination rect.
  frameIn.crop = {src.x1, src.y1, src.x2, src.y2};
  const int viewW = static_cast<int>(view.Width());
  const int viewH = static_cast<int>(view.Height());
  if (viewW <= 0 || viewH <= 0)
    return false;

  // Re-wrap the framebuffer only when it changes.  pl_opengl_wrap/pl_tex_destroy
  // on every frame is expensive: some drivers flush pending GPU work at this point
  // and libplacebo discards cached per-target state, forcing a full re-initialization
  // on the next pl_render_image[_mix] call.
  //
  // The GL FBO query (glGetIntegerv GL_FRAMEBUFFER_BINDING) is deferred into the
  // re-wrap block: on tile-based GPUs (V3D) it serializes the command stream.
  // On the fast path the cached FBO ID is reused — the FBO only changes on
  // Flush/Configure (which clear m_cachedFboTex) or window resize (caught by
  // the view width/height check).
  if (!m_cachedFboTex || viewW != m_cachedFboW || viewH != m_cachedFboH)
  {
    if (m_cachedFboTex)
      pl_tex_destroy(gpu, &m_cachedFboTex);

    GLint currentFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &currentFbo);
    const auto fboId = static_cast<unsigned int>(currentFbo);

    // Query the actual internal format of the framebuffer color attachment so
    // libplacebo knows the real output precision. On HDR-capable compositors
    // (e.g. KDE Wayland with HDR) the surface may be GL_RGB10_A2 or wider;
    // lying to libplacebo with GL_RGBA8 would cause it to dither/clamp
    // unnecessarily before the compositor receives the frame.
    GLenum attachment = (fboId == 0) ? GL_BACK : GL_COLOR_ATTACHMENT0;
    GLint redBits = 8;
    GLint alphaBits = 8;
    GLint colorType = GL_UNSIGNED_NORMALIZED;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &redBits);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alphaBits);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &colorType);
    GLenum fboIformat;
    if (colorType == GL_FLOAT)
      fboIformat = GL_RGBA16F;
    else if (redBits > 8 && alphaBits <= 2)
      fboIformat = GL_RGB10_A2;
    else if (redBits > 8)
      fboIformat = GL_RGBA16;
    else
      fboIformat = GL_RGBA8;

    pl_opengl_wrap_params wrapParams{};
    wrapParams.framebuffer = fboId;
    wrapParams.width = viewW;
    wrapParams.height = viewH;
    wrapParams.iformat = static_cast<int>(fboIformat);

    m_cachedFboTex = pl_opengl_wrap(gpu, &wrapParams);
    if (!m_cachedFboTex)
    {
      CLog::Log(LOGERROR, "CLinuxRendererPLBase::RenderHook - failed to wrap GL framebuffer");
      m_cachedFboId = UINT_MAX;
      return false;
    }
    m_cachedFboId = fboId;
    m_cachedFboW = viewW;
    m_cachedFboH = viewH;
    // FBO change implies the window/surface changed; invalidate cached GL state.
    m_glStateCached = false;
  }
  pl_tex outTex = m_cachedFboTex;

  pl_frame frameOut{};
  frameOut.num_planes = 1;
  frameOut.planes[0].texture = outTex;
  frameOut.planes[0].components = 3;
  frameOut.planes[0].component_mapping[0] = PL_CHANNEL_R;
  frameOut.planes[0].component_mapping[1] = PL_CHANNEL_G;
  frameOut.planes[0].component_mapping[2] = PL_CHANNEL_B;
  frameOut.planes[0].component_mapping[3] = PL_CHANNEL_NONE;
  frameOut.crop = {dst.x1, dst.y1, dst.x2, dst.y2};

  // Use pre-built output color/repr from Configure(). Only HDR transfer is per-frame.
  frameOut.color = m_frameOutColor;
  frameOut.repr = m_frameOutRepr;
  if (this->m_passthroughHDR)
  {
    const auto& buf = this->m_buffers[idx];
    frameOut.color.transfer =
        (buf.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67) ? PL_COLOR_TRC_HLG : PL_COLOR_TRC_PQ;
  }
  // Jointly infer source and destination color spaces. This is the canonical
  // libplacebo approach (matches mpv's vo_gpu_next): it infers src first, then
  // dst using src as reference, and coordinates SDR contrast (min_luma) between
  // them. For HLG→HDR, it also tunes the HLG source peak to the display peak.
  pl_color_space_infer_map(&frameIn.color, &frameOut.color);

  // CMS (ICC profile / 3D LUT) is SDR colour management.  Do not apply it when
  // the output frame is in HDR passthrough mode (PQ/HLG transfer) — libplacebo
  // would apply SDR colour management on top of an HDR signal, producing wrong
  // colours (e.g. blown-out cyan sky when an SDR ICC profile is active).
  if (!this->m_passthroughHDR)
    m_plConfig->ApplyCMS(frameOut, this->m_srcPrimaries);

  pl_render_params params = m_plConfig->GetOptions()->params;

  // Automatic curve, as an HDR10+ display does: the curve the frame carries, spline otherwise.
  if (this->m_videoSettings.m_ToneMapMethod == VS_TONEMAPMETHOD_PL_AUTO)
  {
    m_autoColorMap =
        params.color_map_params ? *params.color_map_params : pl_color_map_default_params;
    m_autoColorMap.tone_mapping_function = plbuf.colorSpace.hdr.ootf.num_anchors > 0
                                               ? &pl_tone_map_st2094_40
                                               : &pl_tone_map_spline;
    params.color_map_params = &m_autoColorMap;
  }
  params.border = PL_CLEAR_SKIP;

  // When frame mixing is active, each source frame is typically shown once
  // (the mixer interpolates between adjacent frames).  Caching the rendered
  // output wastes VRAM because it will never be reused.
  // When frame mixing is OFF, source fps < display fps means the same frame
  // is rendered multiple times (e.g. 24fps on 60Hz = ~2.5×).  Enabling the
  // cache lets libplacebo detect the same source pl_tex and skip the full
  // render pipeline on repeated displays — a cheap blit instead of a full
  // 4K→1080p downscale + tone map + colour management chain.
  params.skip_caching_single_frame = (params.frame_mixer != nullptr);

  // Keep frame mixing cache across renders when interpolation is active.
  // Disable interpolation entirely for still images (no adjacent frames to mix).
  if (params.frame_mixer)
    params.preserve_mixing_cache = true;

  // Allow peak detection to lag one frame — avoids a GPU pipeline stall
  // waiting for the compute shader result on the same frame it was measured.
  if (params.peak_detect_params)
  {
    static pl_peak_detect_params peakParams = pl_peak_detect_default_params;
    peakParams.allow_delayed = true;
    params.peak_detect_params = &peakParams;
  }

  // Dolby Vision RPU provides explicit per-frame luminance bounds in its metadata.
  // Libplacebo's peak detection pass is a redundant GPU sampling operation when DoVi
  // data is present — disable it to save a full GPU pass on every DV frame.
  if (plbuf.colorRepr.dovi != nullptr)
    params.peak_detect_params = nullptr;

  // Anti-aliasing only matters when downscaling; skip the extra shader work
  // when the source fits within the destination rect.
  if ((src.x2 - src.x1) <= (dst.x2 - dst.x1) && (src.y2 - src.y1) <= (dst.y2 - dst.y1))
    params.skip_anti_aliasing = true;

  // Per-pass GPU timing — only set when LOGVIDEO debug logging is enabled.
  // On tile-based GPUs (V3D, Mali), a non-null info_callback enables timer
  // queries (glBeginQuery/glEndQuery per pass) that force partial tile flushes.
  if (m_cachedLogVideo)
  {
    params.info_callback = PlRenderInfoCallback;
    params.info_priv = nullptr;
  }

  // Drain any pre-existing GL errors so libplacebo's gl_check_err doesn't abort
  // a pass early.  Skipped when the context was created with GL_KHR_no_error
  // (EGL_CONTEXT_OPENGL_NO_ERROR_KHR) since no errors can ever be generated.
  if (!m_glNoError)
  {
    while (glGetError() != GL_NO_ERROR)
      ;
  }

  // Save GL state that libplacebo modifies but may not restore on error paths.
  // Viewport, scissor enable, and scissor box are CPU-side shadow state in every
  // known GL/GLES driver (no GPU round-trip), but caching them still removes
  // 3 function calls from the hot path.  The cache is invalidated whenever the
  // FBO dimensions change (window resize) or on Configure/Flush.
  if (!m_glStateCached)
  {
    glGetIntegerv(GL_VIEWPORT, m_cachedGLViewport);
    m_cachedScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, m_cachedGLScissor);
    m_glStateCached = true;
  }
  const GLint* savedViewport = m_cachedGLViewport;
  const GLboolean scissorWasEnabled = m_cachedScissorEnabled;
  const GLint* savedScissor = m_cachedGLScissor;
  // libplacebo binds its own VAO per-pass and resets to VAO 0 at exit.
  // In GL/GLES core profile VAO 0 is invalid; restore Kodi's VAO.
  // Cache the result: glGetIntegerv(GL_VERTEX_ARRAY_BINDING) serializes the GPU
  // command stream on tile-based GPUs (Mali/Adreno/PowerVR) because the driver
  // must flush in-flight work before reading the register.  Kodi's VAO is created
  // once at startup and never changes, so we query it only on the first render.
  if (!m_kodiVaoCached)
  {
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_kodiVAO);
    m_kodiVaoCached = true;
  }
  const GLint savedVAO = m_kodiVAO;

  // GPU-side DMA-buf fence wait (EGL_ANDROID_native_fence_sync).
  // UploadTexture() may have imported a DMA-buf sync-file fence for VAAPI or
  // DRMPRIME frames.  Drain only slots with pending fences (tracked via bitfield)
  // instead of scanning all NUM_BUFFERS slots.  eglWaitSyncKHR inserts the wait
  // into the GPU command stream; the CPU thread returns immediately.
  if (m_pendingSyncSlots)
  {
    uint32_t bits = m_pendingSyncSlots;
    while (bits)
    {
      const int i = std::countr_zero(bits);
      m_eglWaitSyncKHR(m_eglDisplay, m_plBuffers[i].eglSyncFence, 0);
      m_eglDestroySyncKHR(m_eglDisplay, m_plBuffers[i].eglSyncFence);
      m_plBuffers[i].eglSyncFence = EGL_NO_SYNC_KHR;
      bits &= bits - 1;
    }
    m_pendingSyncSlots = 0;
  }

  // Queue-based render path — always used (matches mpv's vo_gpu_next).
  // pl_render_image_mix with radius=0 behaves like pl_render_image but
  // benefits from the queue's frame signature tracking for cache reuse.
  bool rendered = false;
  if (m_plQueue && m_queuePtsOffsetSet)
  {
    const auto& buf = this->m_buffers[idx];
    const float vsyncDuration = m_cachedVsyncDuration;

    // Kodi renders the current frame on every vsync until the next one is due.
    // For interlaced frames the repeats show the second field.
    if (idx != m_queueLastIdx || buf.pts != m_queueLastPts)
    {
      m_queueLastIdx = idx;
      m_queueLastPts = buf.pts;
      m_queueRepeat = 0;
    }
    else
      ++m_queueRepeat;

    double requestPts = QueueTime(buf.pts);
    if (m_queueRepeat > 0 && (buf.iFlags & DVP_FLAG_INTERLACED))
      requestPts += m_cachedFrameDuration / 2;

    if (requestPts < m_queueLastRequest)
    {
      CLog::Log(LOGDEBUG, "CLinuxRendererPLBase::RenderHook - requeue, {:.3f} < {:.3f}", requestPts,
                m_queueLastRequest);
      RequeueHeldFrames();
    }
    m_queueLastRequest = requestPts;

    pl_queue_params qparams{};
    qparams.pts = requestPts;
    qparams.radius = pl_frame_mix_radius(&params);
    qparams.vsync_duration = vsyncDuration;
    // Allow up to 40% of a vsync period for the decoder to deliver the frame.
    // V4L2/bcm2835-codec on RPi5 has measurable decode jitter; a zero timeout causes
    // pl_queue_update to return PL_QUEUE_MORE on any slip, falling back to the direct
    // pl_render_image path and losing frame-mixing / motion-compensation.
    qparams.timeout = static_cast<uint64_t>(vsyncDuration * kQueueTimeoutVsyncFraction * 1e9f);

    pl_frame_mix mix{};
    const auto status = pl_queue_update(m_plQueue, &mix, &qparams);
    if (status == PL_QUEUE_OK || status == PL_QUEUE_MORE) [[likely]]
    {
      if (!pl_render_image_mix(renderer, &mix, &frameOut, &params))
        CLog::Log(LOGWARNING, "CLinuxRendererPLBase::RenderHook - pl_render_image_mix failed");
      rendered = true;
    }
    else if (status == PL_QUEUE_ERR) [[unlikely]]
    {
      CLog::Log(LOGWARNING, "CLinuxRendererPLBase::RenderHook - pl_queue_update error");
    }
  }
  if (!rendered)
  {
    if (!pl_render_image(renderer, &frameIn, &frameOut, &params))
      CLog::Log(LOGWARNING, "CLinuxRendererPLBase::RenderHook - pl_render_image failed");
  }

  // Restore draw framebuffer: libplacebo's gl_tex_blit resets it to 0.
  // GL_READ_FRAMEBUFFER is left at 0 (default) — Kodi's overlay/GUI code
  // uses GL_FRAMEBUFFER which only binds DRAW on GLES 3.0+.
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_cachedFboId);

  glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
  if (scissorWasEnabled)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  glScissor(savedScissor[0], savedScissor[1], savedScissor[2], savedScissor[3]);

  glBindVertexArray(savedVAO);

  // Reset active texture unit (libplacebo may leave it at a non-zero index).
  glActiveTexture(GL_TEXTURE0);

  // Reset shader program: Kodi's CGLShader may skip glUseProgram if it thinks
  // its program is still bound; if libplacebo's program is active instead the
  // GUI renders with the wrong shader.
  glUseProgram(0);

  // outTex aliases m_cachedFboTex — do NOT destroy it here.  It lives across
  // frames and is released in Flush(), Configure(), and the destructor.

  // Always return true: we own this slot; the base must not fall through to
  // its own render path which would crash on uninitialized GL planes.
  return true;
}

// ---------------------------------------------------------------------------
// ReleasePLBuffer
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::ReleasePLBuffer(int index)
{
  PLBuffer& plbuf = m_plBuffers[index];

#if defined(HAVE_LIBVA)
  if (m_isVAAPI)
  {
    // Drop any pending EGL sync object. eglDestroySyncKHR deletes the object
    // immediately; if a wait is pending, the driver keeps the underlying fence
    // alive until the wait completes.
    if (plbuf.eglSyncFence != EGL_NO_SYNC_KHR && m_eglDestroySyncKHR)
    {
      m_eglDestroySyncKHR(m_eglDisplay, plbuf.eglSyncFence);
      plbuf.eglSyncFence = EGL_NO_SYNC_KHR;
      m_pendingSyncSlots &= ~(1u << index);
    }

    // VAAPI cache owns GL textures and pl_tex wrappers — only release EGLImages
    // and close exported DMA-buf fds. Null per-frame slots so the render pipeline
    // won't reference stale data.
    PlaneCache& vc = m_vaapiCache[index];
    for (int n = 0; n < static_cast<int>(std::size(plbuf.vaapiEGLImage)); ++n)
    {
      if (plbuf.vaapiEGLImage[n] != EGL_NO_IMAGE_KHR)
      {
        m_eglDestroyImageKHR(m_eglDisplay, plbuf.vaapiEGLImage[n]);
        plbuf.vaapiEGLImage[n] = EGL_NO_IMAGE_KHR;
        if (vc.initialized && n < vc.numPlanes)
          vc.eglImage[n] = EGL_NO_IMAGE_KHR;
      }
      plbuf.tex[n] = nullptr; // cache owns it
      plbuf.vaapiGLTex[n] = 0; // cache owns it
      plbuf.planes[n] = {};
    }
    for (int f = 0; f < plbuf.vaapiNumFds; ++f)
    {
      if (plbuf.vaapiExportedFd[f] >= 0)
      {
        close(plbuf.vaapiExportedFd[f]);
        plbuf.vaapiExportedFd[f] = -1;
      }
    }
    plbuf.vaapiNumFds = 0;
    plbuf.num_planes = 0;
    plbuf.loaded = false;
    return;
  }
#endif

  // Drop any pending EGL sync object (DRMPRIME path).
  if (plbuf.eglSyncFence != EGL_NO_SYNC_KHR && m_eglDestroySyncKHR)
  {
    m_eglDestroySyncKHR(m_eglDisplay, plbuf.eglSyncFence);
    plbuf.eglSyncFence = EGL_NO_SYNC_KHR;
    m_pendingSyncSlots &= ~(1u << index);
  }

  // Per-plane DRMPRIME (DV path): the cache owns GL textures and pl_tex wrappers.
  // Just null the per-frame slots so the render pipeline won't reference stale data.
  if (plbuf.usedPerPlaneImport)
  {
    for (int n = 0; n < plbuf.num_planes; ++n)
    {
      plbuf.tex[n] = nullptr; // cache owns it
      plbuf.planes[n] = {};
    }
    plbuf.usedPerPlaneImport = false;
    plbuf.colorSpace = {};
    plbuf.colorRepr = {};
    plbuf.num_planes = 0;
    plbuf.loaded = false;
    return;
  }

  m_drmTextures[index].Reset();
  m_lastUploadedBuffer[index] = nullptr;

  // For DRMPRIME, the pl_tex wrapper is owned by m_drmTexCache and reused across
  // frames (the underlying GL texture ID is stable). Don't destroy it here — just
  // null the slot so the render pipeline won't reference a stale frame.
  // For software-decode textures (no cache), destroy normally.
  const pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  for (int n = 0; n < plbuf.num_planes; ++n)
  {
    if (plbuf.tex[n])
    {
      if (m_isDRMPRIME && m_drmTexCache[index].tex == plbuf.tex[n])
        plbuf.tex[n] = nullptr; // cache owns it
      else
        pl_tex_destroy(gpu, &plbuf.tex[n]);
    }
    plbuf.planes[n] = {};
  }
  plbuf.colorSpace = {};
  plbuf.colorRepr = {};
  plbuf.num_planes = 0;
  plbuf.loaded = false;
}
