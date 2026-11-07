/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VideoLayerBridge.h"
#include "drm/DRMObject.h"
#include "drm/DRMUtils.h"
#include "threads/CriticalSection.h"
#include "threads/SystemClock.h"
#include "utils/DisplayInfo.h"
#include "windowing/WinSystem.h"

#include "platform/linux/input/LibInputHandler.h"

#include <atomic>
#include <utility>
#include <vector>

#include <gbm.h>

class IDispResource;
struct udev;
struct udev_monitor;

namespace KODI
{
namespace UTILS
{
class CDisplayInfo;
}
namespace WINDOWING
{
namespace GBM
{

class CWinSystemGbm : public CWinSystemBase
{
public:
  CWinSystemGbm();
  ~CWinSystemGbm() override;

  const std::string GetName() override;

  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  void FlipPage(bool rendered, bool videoLayer, bool async);

  bool CanDoWindowed() override { return false; }
  bool MessagePump() override;
  void UpdateResolutions() override;

  bool UseLimitedColor() override;

  bool Hide() override;
  bool Show(bool raise = true) override;
  void Register(IDispResource* resource) override;
  void Unregister(IDispResource* resource) override;

  bool SetVideoOutput(const VideoPicture* videoPicture) override;

  void SetColorimetry(const VideoPicture* videoPicture) override;
  KODI::UTILS::Colorimetry GetColorimetry() const override { return m_colorimetry; }
  KODI::UTILS::Eotf GetEotf() const override { return m_eotf; }
  bool SetHDR(const VideoPicture* videoPicture) override;
  bool IsHDRDisplay() override;
  CHDRCapabilities GetDisplayHDRCapabilities() const override;

  // holding the returned reference defers video plane teardown until it is released
  std::shared_ptr<CVideoLayerBridge> GetVideoLayerBridge() const { return m_videoLayerBridge; }
  void RegisterVideoLayerBridge(std::shared_ptr<CVideoLayerBridge> bridge)
  {
    m_videoLayerBridge = std::move(bridge);
  };

  CGBMUtils::CGBMDevice& GetGBMDevice() const { return m_GBM->GetDevice(); }
  std::shared_ptr<CDRMUtils> GetDrm() const { return m_DRM; }

  std::vector<std::string> GetConnectedOutputs() override;

protected:
  void OnLostDevice();

  std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock* clock) override;

  std::shared_ptr<CDRMUtils> m_DRM;
  std::unique_ptr<CGBMUtils> m_GBM;
  std::shared_ptr<CVideoLayerBridge> m_videoLayerBridge;

  CCriticalSection m_resourceSection;
  std::vector<IDispResource*> m_resources;

  bool m_dispReset = false;
  XbmcThreads::EndTime<> m_dispResetTimer;
  std::unique_ptr<CLibInputHandler> m_libinput;

private:
  void StartHotplugMonitor();
  void StopHotplugMonitor();
  static void OnUdevEvent(int id, int fd, short revents, void* data);
  void OnHotplug();
  void SetDisplayInfo(const std::vector<uint8_t>& edid);
  //! also read by other threads, e.g. for infolabels
  std::shared_ptr<const UTILS::CDisplayInfo> GetDisplayInfo() const;

  udev* m_udev{nullptr};
  udev_monitor* m_udevMonitor{nullptr};
  int m_udevMonitorId{0};
  std::atomic<bool> m_hotplug{false};
  bool m_hotplugPending{false};
  XbmcThreads::EndTime<> m_hotplugTimer;

  CDRMPropertyBlob m_hdrBlob;
  KODI::UTILS::Eotf m_eotf = KODI::UTILS::Eotf::TRADITIONAL_SDR;
  KODI::UTILS::Colorimetry m_colorimetry = KODI::UTILS::Colorimetry::DEFAULT;

  mutable CCriticalSection m_infoSection;
  std::shared_ptr<const UTILS::CDisplayInfo> m_info;
  std::vector<uint8_t> m_edid;
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
