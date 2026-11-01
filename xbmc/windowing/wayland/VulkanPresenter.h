/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <chrono>
#include <vector>

#include <vulkan/vulkan.h>

#include "system_gl.h"

struct wl_display;
struct wl_surface;

namespace KODI::WINDOWING::WAYLAND
{

/*!
 * \brief Presents GL rendering through a Vulkan swapchain.
 *
 * GL renders into an FBO backed by a 10-bit image exported from Vulkan. On
 * present, the image is blitted into a swapchain image on the window surface.
 * This gives a 10-bit window where EGL only offers 8-bit configs.
 *
 * All methods are called on the render thread with the GL context current.
 */
class CVulkanPresenter
{
public:
  CVulkanPresenter() = default;
  ~CVulkanPresenter();
  CVulkanPresenter(const CVulkanPresenter&) = delete;
  CVulkanPresenter& operator=(const CVulkanPresenter&) = delete;

  bool Create(wl_display* display, wl_surface* surface, int width, int height);
  void Destroy();

  bool Resize(int width, int height);
  //! Hands the rendered frame to the compositor; GL may render the next frame afterwards.
  bool Present();

  GLuint GetFramebuffer() const { return m_fbo; }
  int GetBitDepth() const { return 10; }
  //! True after an unrecoverable error; frames can no longer be presented.
  bool HasFailed() const { return m_failed; }

private:
  struct GLFunctions;

  bool LoadGLFunctions();
  bool CreateInstance();
  bool CreateSurface();
  bool RecreateSurface();
  bool SelectDevice();
  bool CreateDevice();
  bool CreateSwapchain();
  void DestroySwapchain();
  bool CreateTarget();
  void DestroyTarget();
  bool CreateSemaphores();
  //! Waits for the GL frame without presenting it, keeping the semaphores paired.
  bool DropFrame();
  //! Gives up when frames could not be presented for too long.
  void CheckRecovery(bool presented);

  GLFunctions* m_gl{nullptr};

  int m_width{0};
  int m_height{0};

  VkInstance m_instance{VK_NULL_HANDLE};
  VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
  VkDevice m_device{VK_NULL_HANDLE};
  uint32_t m_queueFamily{0};
  VkQueue m_queue{VK_NULL_HANDLE};
  VkSurfaceKHR m_surface{VK_NULL_HANDLE};
  wl_display* m_display{nullptr};
  wl_surface* m_wlSurface{nullptr};

  VkSurfaceFormatKHR m_surfaceFormat{};
  VkPresentModeKHR m_presentMode{VK_PRESENT_MODE_FIFO_KHR};
  VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};
  std::vector<VkImage> m_swapchainImages;
  std::vector<VkSemaphore> m_renderDone;
  bool m_swapchainDirty{false};
  //! Set after an unrecoverable error; frames are no longer presented.
  bool m_failed{false};
  //! The surface went away, e.g. on an HDMI hotplug; it is created again.
  bool m_surfaceLost{false};
  std::chrono::steady_clock::time_point m_notPresentingSince{};

  // Render target shared with GL
  VkImage m_image{VK_NULL_HANDLE};
  VkDeviceMemory m_memory{VK_NULL_HANDLE};
  GLuint m_glMemory{0};
  GLuint m_texture{0};
  GLuint m_depth{0};
  GLuint m_fbo{0};

  // GL -> Vulkan: frame rendered; Vulkan -> GL: target free again
  VkSemaphore m_glReady{VK_NULL_HANDLE};
  VkSemaphore m_vkDone{VK_NULL_HANDLE};
  GLuint m_glReadySem{0};
  GLuint m_vkDoneSem{0};

  VkSemaphore m_acquired{VK_NULL_HANDLE};
  VkCommandPool m_commandPool{VK_NULL_HANDLE};
  VkCommandBuffer m_commandBuffer{VK_NULL_HANDLE};
  VkFence m_fence{VK_NULL_HANDLE};
};

} // namespace KODI::WINDOWING::WAYLAND
