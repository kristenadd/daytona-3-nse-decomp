#pragma once
// The Vulkan entry points the renderer uses (src/gpu/vulkan.cpp), loaded at
// run time: libvulkan is found through SDL when it has loaded it for the
// window, else opened directly, so the binary has no link-time dependency on
// it (the Steam Runtime and SteamOS both provide one).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace dcusa::gpu::vk {

#define DCUSA_VK_GLOBAL(X)                  \
    X(vkCreateInstance)                     \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceVersion)

#define DCUSA_VK_INSTANCE(X)                        \
    X(vkDestroyInstance)                            \
    X(vkEnumeratePhysicalDevices)                   \
    X(vkGetPhysicalDeviceProperties)                \
    X(vkGetPhysicalDeviceProperties2)               \
    X(vkGetPhysicalDeviceFeatures)                  \
    X(vkGetPhysicalDeviceFeatures2)                 \
    X(vkGetPhysicalDeviceQueueFamilyProperties)     \
    X(vkGetPhysicalDeviceMemoryProperties)          \
    X(vkGetPhysicalDeviceFormatProperties)          \
    X(vkEnumerateDeviceExtensionProperties)         \
    X(vkCreateDevice)                               \
    X(vkGetDeviceProcAddr)                          \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)         \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)    \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)         \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)    \
    X(vkDestroySurfaceKHR)

#define DCUSA_VK_DEVICE(X)                 \
    X(vkGetDeviceQueue)                    \
    X(vkDeviceWaitIdle)                    \
    X(vkCreateCommandPool)                 \
    X(vkAllocateCommandBuffers)            \
    X(vkResetCommandBuffer)                \
    X(vkBeginCommandBuffer)                \
    X(vkEndCommandBuffer)                  \
    X(vkQueueSubmit)                       \
    X(vkCreateFence)                       \
    X(vkWaitForFences)                     \
    X(vkResetFences)                       \
    X(vkGetFenceStatus)                    \
    X(vkCreateSemaphore)                   \
    X(vkDestroySemaphore)                  \
    X(vkAllocateMemory)                    \
    X(vkFreeMemory)                        \
    X(vkMapMemory)                         \
    X(vkCreateBuffer)                      \
    X(vkDestroyBuffer)                     \
    X(vkGetBufferMemoryRequirements)       \
    X(vkBindBufferMemory)                  \
    X(vkCreateImage)                       \
    X(vkDestroyImage)                      \
    X(vkGetImageMemoryRequirements)        \
    X(vkBindImageMemory)                   \
    X(vkCreateImageView)                   \
    X(vkDestroyImageView)                  \
    X(vkCreateSampler)                     \
    X(vkDestroySampler)                    \
    X(vkCreateShaderModule)                \
    X(vkCreateDescriptorSetLayout)         \
    X(vkCreatePipelineLayout)              \
    X(vkCreateGraphicsPipelines)           \
    X(vkCreateDescriptorPool)              \
    X(vkResetDescriptorPool)               \
    X(vkAllocateDescriptorSets)            \
    X(vkUpdateDescriptorSets)              \
    X(vkCreateQueryPool)                   \
    X(vkGetQueryPoolResults)               \
    X(vkCmdResetQueryPool)                 \
    X(vkCmdWriteTimestamp)                 \
    X(vkCmdPipelineBarrier)                \
    X(vkCmdCopyBufferToImage)              \
    X(vkCmdCopyImageToBuffer)              \
    X(vkCmdBlitImage)                      \
    X(vkCmdClearColorImage)                \
    X(vkCmdClearDepthStencilImage)         \
    X(vkCmdBindPipeline)                   \
    X(vkCmdBindDescriptorSets)             \
    X(vkCmdBindVertexBuffers)              \
    X(vkCmdBindIndexBuffer)                \
    X(vkCmdSetViewport)                    \
    X(vkCmdSetScissor)                     \
    X(vkCmdSetBlendConstants)              \
    X(vkCmdDraw)                           \
    X(vkCmdDrawIndexed)                    \
    X(vkCreateSwapchainKHR)                \
    X(vkDestroySwapchainKHR)               \
    X(vkGetSwapchainImagesKHR)             \
    X(vkAcquireNextImageKHR)               \
    X(vkQueuePresentKHR)

// Core in Vulkan 1.3, else from VK_KHR_dynamic_rendering /
// VK_EXT_extended_dynamic_state: loaded by either name.
#define DCUSA_VK_DEVICE_ALIASED(X)                                     \
    X(vkCmdBeginRendering, "vkCmdBeginRenderingKHR")                   \
    X(vkCmdEndRendering, "vkCmdEndRenderingKHR")                       \
    X(vkCmdSetCullMode, "vkCmdSetCullModeEXT")                         \
    X(vkCmdSetFrontFace, "vkCmdSetFrontFaceEXT")                       \
    X(vkCmdSetDepthTestEnable, "vkCmdSetDepthTestEnableEXT")           \
    X(vkCmdSetDepthWriteEnable, "vkCmdSetDepthWriteEnableEXT")         \
    X(vkCmdSetDepthCompareOp, "vkCmdSetDepthCompareOpEXT")

#define DCUSA_VK_DECLARE(name) inline PFN_##name name = nullptr;
#define DCUSA_VK_DECLARE2(name, alias) inline PFN_##name name = nullptr;
inline PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
DCUSA_VK_GLOBAL(DCUSA_VK_DECLARE)
DCUSA_VK_INSTANCE(DCUSA_VK_DECLARE)
DCUSA_VK_DEVICE(DCUSA_VK_DECLARE)
DCUSA_VK_DEVICE_ALIASED(DCUSA_VK_DECLARE2)
#undef DCUSA_VK_DECLARE
#undef DCUSA_VK_DECLARE2

}  // namespace dcusa::gpu::vk
