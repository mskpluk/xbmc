/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#define VK_USE_PLATFORM_WAYLAND_KHR
#include "VulkanPresenter.h"

#include "utils/log.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <unistd.h>

#include <EGL/egl.h>

using namespace KODI::WINDOWING::WAYLAND;

namespace
{
// Format of the shared render target, GL_RGB10_A2 on the GL side
constexpr VkFormat TARGET_FORMAT = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
constexpr uint64_t ACQUIRE_TIMEOUT_NS = 1'000'000'000;
// How long the swapchain may stay unusable (output hotplug, mode change)
// before presentation is given up.
constexpr auto RECOVERY_TIMEOUT = std::chrono::seconds(10);

bool CheckVK(VkResult result, const char* call)
{
  if (result == VK_SUCCESS)
    return true;
  CLog::Log(LOGERROR, "CVulkanPresenter: {} failed: {}", call, static_cast<int>(result));
  return false;
}

bool Is10Bit(VkFormat format)
{
  return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
         format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

//! Drops errors left by unrelated GL code, so the next check sees only our own.
void ClearGLErrors()
{
  while (glGetError() != GL_NO_ERROR)
  {
  }
}

bool CheckGL(const char* what)
{
  const GLenum error = glGetError();
  if (error == GL_NO_ERROR)
    return true;
  CLog::Log(LOGERROR, "CVulkanPresenter: GL error {:#x} in {}", error, what);
  ClearGLErrors();
  return false;
}

template<typename T>
T GetGLProc(const char* name)
{
  return reinterpret_cast<T>(eglGetProcAddress(name));
}
} // namespace

struct CVulkanPresenter::GLFunctions
{
  PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjects;
  PFNGLDELETEMEMORYOBJECTSEXTPROC DeleteMemoryObjects;
  PFNGLMEMORYOBJECTPARAMETERIVEXTPROC MemoryObjectParameteriv;
  PFNGLIMPORTMEMORYFDEXTPROC ImportMemoryFd;
  PFNGLTEXSTORAGEMEM2DEXTPROC TexStorageMem2D;
  PFNGLGENSEMAPHORESEXTPROC GenSemaphores;
  PFNGLDELETESEMAPHORESEXTPROC DeleteSemaphores;
  PFNGLIMPORTSEMAPHOREFDEXTPROC ImportSemaphoreFd;
  PFNGLSIGNALSEMAPHOREEXTPROC SignalSemaphore;
  PFNGLWAITSEMAPHOREEXTPROC WaitSemaphore;
  PFNGLGETUNSIGNEDBYTEI_VEXTPROC GetUnsignedBytei_v;
};

CVulkanPresenter::~CVulkanPresenter()
{
  Destroy();
}

bool CVulkanPresenter::Create(wl_display* display, wl_surface* surface, int width, int height)
{
  m_width = width;
  m_height = height;
  m_display = display;
  m_wlSurface = surface;

  if (!LoadGLFunctions() || !CreateInstance() || !CreateSurface())
    return false;

  if (!SelectDevice() || !CreateDevice() || !CreateSemaphores() || !CreateTarget() ||
      !CreateSwapchain())
    return false;

  CLog::Log(LOGINFO, "CVulkanPresenter: presenting {}x{} through Vulkan (format {}, mode {})",
            m_width, m_height, static_cast<int>(m_surfaceFormat.format),
            static_cast<int>(m_presentMode));
  return true;
}

void CVulkanPresenter::Destroy()
{
  if (m_device)
  {
    glFinish();
    vkDeviceWaitIdle(m_device);
  }

  DestroySwapchain();
  DestroyTarget();

  if (m_gl)
  {
    if (m_glReadySem)
      m_gl->DeleteSemaphores(1, &m_glReadySem);
    if (m_vkDoneSem)
      m_gl->DeleteSemaphores(1, &m_vkDoneSem);
  }
  m_glReadySem = 0;
  m_vkDoneSem = 0;

  if (m_device)
  {
    vkDestroySemaphore(m_device, m_glReady, nullptr);
    vkDestroySemaphore(m_device, m_vkDone, nullptr);
    vkDestroySemaphore(m_device, m_acquired, nullptr);
    vkDestroyFence(m_device, m_fence, nullptr);
    vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    vkDestroyDevice(m_device, nullptr);
  }
  m_glReady = VK_NULL_HANDLE;
  m_vkDone = VK_NULL_HANDLE;
  m_acquired = VK_NULL_HANDLE;
  m_fence = VK_NULL_HANDLE;
  m_commandPool = VK_NULL_HANDLE;
  m_commandBuffer = VK_NULL_HANDLE;
  m_device = VK_NULL_HANDLE;
  m_queue = VK_NULL_HANDLE;

  if (m_instance)
  {
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    vkDestroyInstance(m_instance, nullptr);
  }
  m_surface = VK_NULL_HANDLE;
  m_instance = VK_NULL_HANDLE;
  m_physicalDevice = VK_NULL_HANDLE;

  delete m_gl;
  m_gl = nullptr;
}

bool CVulkanPresenter::LoadGLFunctions()
{
  auto* gl = new GLFunctions;
  gl->CreateMemoryObjects = GetGLProc<PFNGLCREATEMEMORYOBJECTSEXTPROC>("glCreateMemoryObjectsEXT");
  gl->DeleteMemoryObjects = GetGLProc<PFNGLDELETEMEMORYOBJECTSEXTPROC>("glDeleteMemoryObjectsEXT");
  gl->MemoryObjectParameteriv =
      GetGLProc<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>("glMemoryObjectParameterivEXT");
  gl->ImportMemoryFd = GetGLProc<PFNGLIMPORTMEMORYFDEXTPROC>("glImportMemoryFdEXT");
  gl->TexStorageMem2D = GetGLProc<PFNGLTEXSTORAGEMEM2DEXTPROC>("glTexStorageMem2DEXT");
  gl->GenSemaphores = GetGLProc<PFNGLGENSEMAPHORESEXTPROC>("glGenSemaphoresEXT");
  gl->DeleteSemaphores = GetGLProc<PFNGLDELETESEMAPHORESEXTPROC>("glDeleteSemaphoresEXT");
  gl->ImportSemaphoreFd = GetGLProc<PFNGLIMPORTSEMAPHOREFDEXTPROC>("glImportSemaphoreFdEXT");
  gl->SignalSemaphore = GetGLProc<PFNGLSIGNALSEMAPHOREEXTPROC>("glSignalSemaphoreEXT");
  gl->WaitSemaphore = GetGLProc<PFNGLWAITSEMAPHOREEXTPROC>("glWaitSemaphoreEXT");
  gl->GetUnsignedBytei_v = GetGLProc<PFNGLGETUNSIGNEDBYTEI_VEXTPROC>("glGetUnsignedBytei_vEXT");
  m_gl = gl;

  bool hasMemoryFd = false;
  bool hasSemaphoreFd = false;
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; ++i)
  {
    const auto* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
    hasMemoryFd |= std::strcmp(ext, "GL_EXT_memory_object_fd") == 0;
    hasSemaphoreFd |= std::strcmp(ext, "GL_EXT_semaphore_fd") == 0;
  }

  if (!hasMemoryFd || !hasSemaphoreFd || !gl->CreateMemoryObjects || !gl->ImportMemoryFd ||
      !gl->TexStorageMem2D || !gl->ImportSemaphoreFd || !gl->SignalSemaphore ||
      !gl->WaitSemaphore || !gl->GetUnsignedBytei_v)
  {
    CLog::Log(LOGINFO, "CVulkanPresenter: GL lacks memory object or semaphore interop");
    return false;
  }
  return true;
}

bool CVulkanPresenter::CreateInstance()
{
  std::vector<const char*> extensions{VK_KHR_SURFACE_EXTENSION_NAME,
                                      VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};

  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> available(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
  for (const auto& ext : available)
  {
    if (std::strcmp(ext.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0)
      extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
  }

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "Kodi";
  appInfo.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &appInfo;
  info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  info.ppEnabledExtensionNames = extensions.data();
  return CheckVK(vkCreateInstance(&info, nullptr, &m_instance), "vkCreateInstance");
}

bool CVulkanPresenter::CreateSurface()
{
  VkWaylandSurfaceCreateInfoKHR surfaceInfo{};
  surfaceInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
  surfaceInfo.display = m_display;
  surfaceInfo.surface = m_wlSurface;
  return CheckVK(vkCreateWaylandSurfaceKHR(m_instance, &surfaceInfo, nullptr, &m_surface),
                 "vkCreateWaylandSurfaceKHR");
}

bool CVulkanPresenter::RecreateSurface()
{
  DestroySwapchain();
  vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
  m_surface = VK_NULL_HANDLE;
  if (!CreateSurface())
    return false;

  VkBool32 present = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(m_physicalDevice, m_queueFamily, m_surface, &present);
  return present == VK_TRUE;
}

bool CVulkanPresenter::SelectDevice()
{
  // GL and Vulkan must run on the same GPU to share memory.
  std::array<GLubyte, GL_UUID_SIZE_EXT> glUuid{};
  m_gl->GetUnsignedBytei_v(GL_DEVICE_UUID_EXT, 0, glUuid.data());

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

  for (auto device : devices)
  {
    VkPhysicalDeviceIDProperties idProps{};
    idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &idProps;
    vkGetPhysicalDeviceProperties2(device, &props);
    if (std::memcmp(idProps.deviceUUID, glUuid.data(), VK_UUID_SIZE) != 0)
      continue;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount; ++i)
    {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
      if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
      {
        m_physicalDevice = device;
        m_queueFamily = i;
        CLog::Log(LOGDEBUG, "CVulkanPresenter: using {}", props.properties.deviceName);
        return true;
      }
    }
  }

  CLog::Log(LOGINFO, "CVulkanPresenter: no Vulkan device matches the GL device");
  return false;
}

bool CVulkanPresenter::CreateDevice()
{
  const std::array<const char*, 3> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                              VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                              VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
  const float priority = 1.0f;

  VkDeviceQueueCreateInfo queueInfo{};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = m_queueFamily;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;

  VkDeviceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &queueInfo;
  info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  info.ppEnabledExtensionNames = extensions.data();
  if (!CheckVK(vkCreateDevice(m_physicalDevice, &info, nullptr, &m_device), "vkCreateDevice"))
    return false;

  vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = m_queueFamily;
  if (!CheckVK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool),
               "vkCreateCommandPool"))
    return false;

  VkCommandBufferAllocateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  bufferInfo.commandPool = m_commandPool;
  bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  bufferInfo.commandBufferCount = 1;
  if (!CheckVK(vkAllocateCommandBuffers(m_device, &bufferInfo, &m_commandBuffer),
               "vkAllocateCommandBuffers"))
    return false;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  return CheckVK(vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence), "vkCreateFence");
}

bool CVulkanPresenter::CreateSemaphores()
{
  auto getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
      vkGetDeviceProcAddr(m_device, "vkGetSemaphoreFdKHR"));
  if (!getSemaphoreFd)
    return false;

  ClearGLErrors();

  VkSemaphoreCreateInfo plainInfo{};
  plainInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  if (!CheckVK(vkCreateSemaphore(m_device, &plainInfo, nullptr, &m_acquired), "vkCreateSemaphore"))
    return false;

  VkExportSemaphoreCreateInfo exportInfo{};
  exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
  exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkSemaphoreCreateInfo sharedInfo{};
  sharedInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  sharedInfo.pNext = &exportInfo;

  const std::array<std::pair<VkSemaphore*, GLuint*>, 2> semaphores{
      {{&m_glReady, &m_glReadySem}, {&m_vkDone, &m_vkDoneSem}}};
  for (const auto& [vkSem, glSem] : semaphores)
  {
    if (!CheckVK(vkCreateSemaphore(m_device, &sharedInfo, nullptr, vkSem), "vkCreateSemaphore"))
      return false;

    VkSemaphoreGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    fdInfo.semaphore = *vkSem;
    fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (!CheckVK(getSemaphoreFd(m_device, &fdInfo, &fd), "vkGetSemaphoreFdKHR"))
      return false;

    // GL takes ownership of the fd.
    m_gl->GenSemaphores(1, glSem);
    m_gl->ImportSemaphoreFd(*glSem, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
  }

  return CheckGL("semaphore import");
}

bool CVulkanPresenter::CreateTarget()
{
  ClearGLErrors();

  VkFormatProperties formatProps;
  vkGetPhysicalDeviceFormatProperties(m_physicalDevice, TARGET_FORMAT, &formatProps);
  if (!(formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
  {
    CLog::Log(LOGINFO, "CVulkanPresenter: 10-bit render target cannot be blitted");
    return false;
  }

  VkExternalMemoryImageCreateInfo externalInfo{};
  externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.pNext = &externalInfo;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = TARGET_FORMAT;
  imageInfo.extent = {static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height), 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!CheckVK(vkCreateImage(m_device, &imageInfo, nullptr, &m_image), "vkCreateImage"))
    return false;

  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(m_device, m_image, &requirements);

  VkPhysicalDeviceMemoryProperties memoryProps;
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProps);
  uint32_t memoryType = UINT32_MAX;
  for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i)
  {
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (memoryProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
    {
      memoryType = i;
      break;
    }
  }
  if (memoryType == UINT32_MAX)
  {
    CLog::Log(LOGERROR, "CVulkanPresenter: no device local memory for the render target");
    return false;
  }

  VkMemoryDedicatedAllocateInfo dedicatedInfo{};
  dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicatedInfo.image = m_image;
  VkExportMemoryAllocateInfo exportInfo{};
  exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  exportInfo.pNext = &dedicatedInfo;
  exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.pNext = &exportInfo;
  allocInfo.allocationSize = requirements.size;
  allocInfo.memoryTypeIndex = memoryType;
  if (!CheckVK(vkAllocateMemory(m_device, &allocInfo, nullptr, &m_memory), "vkAllocateMemory") ||
      !CheckVK(vkBindImageMemory(m_device, m_image, m_memory, 0), "vkBindImageMemory"))
    return false;

  auto getMemoryFd =
      reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(m_device, "vkGetMemoryFdKHR"));
  VkMemoryGetFdInfoKHR fdInfo{};
  fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  fdInfo.memory = m_memory;
  fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  if (!getMemoryFd || !CheckVK(getMemoryFd(m_device, &fdInfo, &fd), "vkGetMemoryFdKHR"))
  {
    CLog::Log(LOGERROR, "CVulkanPresenter: cannot export the render target");
    return false;
  }

  // GL takes ownership of the fd.
  const GLint dedicated = GL_TRUE;
  m_gl->CreateMemoryObjects(1, &m_glMemory);
  m_gl->MemoryObjectParameteriv(m_glMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
  m_gl->ImportMemoryFd(m_glMemory, requirements.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);

  glGenTextures(1, &m_texture);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  m_gl->TexStorageMem2D(GL_TEXTURE_2D, 1, GL_RGB10_A2, m_width, m_height, m_glMemory, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);

  // The GUI uses a depth buffer for front-to-back rendering.
  glGenRenderbuffers(1, &m_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_width, m_height);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    CLog::Log(LOGERROR, "CVulkanPresenter: render target incomplete: {:#x}", status);
    return false;
  }

  // New memory holds stale content: clear all of it, whatever scissor and masks
  // Kodi has set for the old size.
  GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLboolean colorMask[4];
  GLboolean depthMask;
  glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
  glDepthMask(depthMask);
  if (scissor)
    glEnable(GL_SCISSOR_TEST);

  return CheckGL("render target setup");
}

void CVulkanPresenter::DestroyTarget()
{
  if (m_fbo)
    glDeleteFramebuffers(1, &m_fbo);
  if (m_depth)
    glDeleteRenderbuffers(1, &m_depth);
  if (m_texture)
    glDeleteTextures(1, &m_texture);
  if (m_glMemory)
    m_gl->DeleteMemoryObjects(1, &m_glMemory);
  m_fbo = 0;
  m_depth = 0;
  m_texture = 0;
  m_glMemory = 0;

  if (m_device)
  {
    vkDestroyImage(m_device, m_image, nullptr);
    vkFreeMemory(m_device, m_memory, nullptr);
  }
  m_image = VK_NULL_HANDLE;
  m_memory = VK_NULL_HANDLE;
}

bool CVulkanPresenter::CreateSwapchain()
{
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &count, formats.data());

  // Kodi tags the surface itself through the color management protocol, so the
  // driver must not interpret the content: prefer pass-through, else plain sRGB.
  const VkSurfaceFormatKHR* chosen = nullptr;
  for (const auto& format : formats)
  {
    if (!Is10Bit(format.format))
      continue;
    if (format.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT)
    {
      chosen = &format;
      break;
    }
    if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && !chosen)
      chosen = &format;
  }
  if (!chosen)
  {
    CLog::Log(LOGINFO, "CVulkanPresenter: no 10-bit swapchain format");
    return false;
  }
  m_surfaceFormat = *chosen;

  VkFormatProperties formatProps;
  vkGetPhysicalDeviceFormatProperties(m_physicalDevice, m_surfaceFormat.format, &formatProps);
  if (!(formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
  {
    CLog::Log(LOGINFO, "CVulkanPresenter: swapchain format cannot be blitted to");
    return false;
  }

  // Kodi paces frames with Wayland frame callbacks, so presenting must not block.
  vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &count, modes.data());
  m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
  for (const auto mode : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
  {
    if (std::find(modes.begin(), modes.end(), mode) != modes.end())
    {
      m_presentMode = mode;
      break;
    }
  }

  VkSurfaceCapabilitiesKHR caps;
  if (!CheckVK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
    return false;

  uint32_t imageCount = std::max(caps.minImageCount + 1, 3u);
  if (caps.maxImageCount > 0)
    imageCount = std::min(imageCount, caps.maxImageCount);

  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & alpha))
    alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha &
                                                     -caps.supportedCompositeAlpha);

  VkSwapchainKHR oldSwapchain = m_swapchain;

  VkSwapchainCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = m_surface;
  info.minImageCount = imageCount;
  info.imageFormat = m_surfaceFormat.format;
  info.imageColorSpace = m_surfaceFormat.colorSpace;
  info.imageExtent = {static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height)};
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  info.compositeAlpha = alpha;
  info.presentMode = m_presentMode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = oldSwapchain;
  const bool ok =
      CheckVK(vkCreateSwapchainKHR(m_device, &info, nullptr, &m_swapchain), "vkCreateSwapchainKHR");

  if (oldSwapchain)
    vkDestroySwapchainKHR(m_device, oldSwapchain, nullptr);
  if (!ok)
  {
    m_swapchain = VK_NULL_HANDLE;
    return false;
  }

  vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
  m_swapchainImages.resize(count);
  vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, m_swapchainImages.data());

  for (auto semaphore : m_renderDone)
    vkDestroySemaphore(m_device, semaphore, nullptr);
  m_renderDone.assign(count, VK_NULL_HANDLE);
  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  for (auto& semaphore : m_renderDone)
  {
    if (!CheckVK(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &semaphore),
                 "vkCreateSemaphore"))
      return false;
  }

  m_swapchainDirty = false;
  return true;
}

void CVulkanPresenter::DestroySwapchain()
{
  if (!m_device)
    return;

  for (auto semaphore : m_renderDone)
    vkDestroySemaphore(m_device, semaphore, nullptr);
  m_renderDone.clear();
  m_swapchainImages.clear();

  vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
  m_swapchain = VK_NULL_HANDLE;
}

bool CVulkanPresenter::Resize(int width, int height)
{
  if (width == m_width && height == m_height)
    return true;

  glFinish();
  vkDeviceWaitIdle(m_device);

  m_width = width;
  m_height = height;

  DestroyTarget();
  DestroySwapchain();
  if (!CreateTarget())
  {
    CLog::Log(LOGERROR, "CVulkanPresenter: cannot render at {}x{}, presentation stopped", width,
              height);
    m_failed = true;
    return false;
  }

  // The output may be changing; the swapchain is retried on present.
  if (!CreateSwapchain())
  {
    m_swapchainDirty = true;
    return false;
  }

  return true;
}

bool CVulkanPresenter::DropFrame()
{
  const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &m_glReady;
  submit.pWaitDstStageMask = &waitStage;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &m_vkDone;
  vkResetFences(m_device, 1, &m_fence);
  return CheckVK(vkQueueSubmit(m_queue, 1, &submit, m_fence), "vkQueueSubmit");
}

bool CVulkanPresenter::Present()
{
  if (m_failed)
    return false;

  // Hand the finished frame to Vulkan.
  GLenum layout = GL_LAYOUT_TRANSFER_SRC_EXT;
  m_gl->SignalSemaphore(m_glReadySem, 0, nullptr, 1, &m_texture, &layout);
  glFlush();

  // The command buffer and the acquire semaphore are reused every frame.
  vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);

  bool presented = false;
  bool submitted = false;
  if (m_surfaceLost || m_swapchainDirty)
  {
    vkDeviceWaitIdle(m_device);
    if (m_surfaceLost && RecreateSurface())
      m_surfaceLost = false;
    if (!m_surfaceLost && !CreateSwapchain())
      m_swapchainDirty = true;
  }

  uint32_t index = 0;
  VkResult result = VK_ERROR_OUT_OF_DATE_KHR;
  if (m_swapchain)
    result = vkAcquireNextImageKHR(m_device, m_swapchain, ACQUIRE_TIMEOUT_NS, m_acquired,
                                   VK_NULL_HANDLE, &index);

  if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
  {
    m_swapchainDirty |= (result == VK_SUBOPTIMAL_KHR);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(m_commandBuffer, 0);
    vkBeginCommandBuffer(m_commandBuffer, &begin);

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImage swapImage = m_swapchainImages[index];

    // Take the target over from GL and prepare the swapchain image.
    std::array<VkImageMemoryBarrier, 2> before{};
    before[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    before[0].dstQueueFamilyIndex = m_queueFamily;
    before[0].image = m_image;
    before[0].subresourceRange = range;
    before[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before[1].image = swapImage;
    before[1].subresourceRange = range;
    vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(before.size()), before.data());

    // GL rows start at the bottom, Vulkan rows at the top.
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, m_height, 0};
    blit.srcOffsets[1] = {m_width, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {m_width, m_height, 1};
    vkCmdBlitImage(m_commandBuffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

    // Present the swapchain image and give the target back to GL.
    std::array<VkImageMemoryBarrier, 2> after{};
    after[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    after[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    after[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after[0].image = swapImage;
    after[0].subresourceRange = range;
    after[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    after[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    after[1].srcQueueFamilyIndex = m_queueFamily;
    after[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    after[1].image = m_image;
    after[1].subresourceRange = range;
    vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(after.size()), after.data());
    vkEndCommandBuffer(m_commandBuffer);

    const std::array<VkSemaphore, 2> waits{m_acquired, m_glReady};
    const std::array<VkPipelineStageFlags, 2> waitStages{VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                         VK_PIPELINE_STAGE_TRANSFER_BIT};
    const std::array<VkSemaphore, 2> signals{m_renderDone[index], m_vkDone};

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    submit.pWaitSemaphores = waits.data();
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_commandBuffer;
    submit.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    submit.pSignalSemaphores = signals.data();
    vkResetFences(m_device, 1, &m_fence);
    submitted = CheckVK(vkQueueSubmit(m_queue, 1, &submit, m_fence), "vkQueueSubmit");
    if (submitted)
    {
      VkPresentInfoKHR present{};
      present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
      present.waitSemaphoreCount = 1;
      present.pWaitSemaphores = &m_renderDone[index];
      present.swapchainCount = 1;
      present.pSwapchains = &m_swapchain;
      present.pImageIndices = &index;
      result = vkQueuePresentKHR(m_queue, &present);
      if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        m_swapchainDirty = true;
      else if (result == VK_ERROR_SURFACE_LOST_KHR)
        m_surfaceLost = true;
      else if (!CheckVK(result, "vkQueuePresentKHR") && result == VK_ERROR_DEVICE_LOST)
        m_failed = true;
      presented = (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR);
    }
  }
  else
  {
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
      m_swapchainDirty = true;
    else if (result == VK_ERROR_SURFACE_LOST_KHR)
      m_surfaceLost = true;
    else if (result != VK_TIMEOUT && !CheckVK(result, "vkAcquireNextImageKHR") &&
             result == VK_ERROR_DEVICE_LOST)
      m_failed = true;
    submitted = DropFrame();
  }

  if (!submitted)
  {
    // Without a submission nothing signals the semaphore GL would wait on.
    CLog::Log(LOGERROR, "CVulkanPresenter: Vulkan submission failed, presentation stopped");
    m_failed = true;
    return false;
  }

  // GL may render again once the blit has read the target.
  layout = GL_LAYOUT_COLOR_ATTACHMENT_EXT;
  m_gl->WaitSemaphore(m_vkDoneSem, 0, nullptr, 1, &m_texture, &layout);

  CheckRecovery(presented);
  return presented;
}

void CVulkanPresenter::CheckRecovery(bool presented)
{
  const auto now = std::chrono::steady_clock::now();
  if (presented || m_notPresentingSince == std::chrono::steady_clock::time_point{})
  {
    m_notPresentingSince = presented ? std::chrono::steady_clock::time_point{} : now;
    return;
  }

  if (now - m_notPresentingSince > RECOVERY_TIMEOUT)
  {
    CLog::Log(LOGERROR, "CVulkanPresenter: no frame presented for {} s, presentation stopped",
              std::chrono::duration_cast<std::chrono::seconds>(RECOVERY_TIMEOUT).count());
    m_failed = true;
  }
}
