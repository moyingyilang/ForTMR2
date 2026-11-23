#pragma once

// ============================================================================
// EFML4A 拦截点声明 + 实例/设备注册表 + 统计计数
//
// 设计要点：
//  * 每个被拦截的 vk* 入口都实现「转发 + 计数 + 日志」，真正的缓冲替换留 TODO。
//  * 用实例/设备级别的全局映射表（std::unordered_map + std::mutex）从
//    VkInstance / VkDevice 查到 dispatch table —— 这是层能把调用转发回
//    loader/驱动的唯一途径（见 dispatch.h 与 hooks.cpp 的说明）。
// ============================================================================

#include <cstdint>

#include "efml4a/dispatch.h"
#include "efml4a/layer.h"

namespace efml4a {

// ---------------------------------------------------------------------------
// 统计计数（std::atomic<uint64_t>）
// ---------------------------------------------------------------------------
enum StatId {
    kStatCreateInstance = 0,
    kStatEnumerateInstanceLayerProperties,
    kStatEnumerateInstanceExtensionProperties,
    kStatCreateDevice,
    kStatCreateGraphicsPipelines,
    kStatCreateShaderModule,
    kStatCmdBindPipeline,
    kStatCmdBindVertexBuffers,
    kStatCmdBindIndexBuffer,
    kStatCmdDrawIndexed,
    kStatCmdDraw,
    kStatCmdPushConstants,
    kStatCmdBindDescriptorSets,
    kStatQueueSubmit,
    kStatCount
};

// 与 StatId 一一对应的稳定名字（用于 JSON 输出，不要随意改动）。
const char* StatName(StatId id);

void Bump(StatId id);
uint64_t GetStat(StatId id);

// ---------------------------------------------------------------------------
// 实例 / 设备注册表
// ---------------------------------------------------------------------------
// 每个 VkInstance 除了 dispatch table 之外，还要记住 loader 在 pNext 链里塞给我们的
// 一些回调（它们只在 vkCreateInstance 那一刻有效，必须当场存下来）：
//   * VK_LAYER_LINK_INFO                       -> pfnNextGetInstanceProcAddr /
//                                                 pfnNextGetPhysicalDeviceProcAddr
//   * VK_LOADER_DATA_CALLBACK                  -> pfnSetInstanceLoaderData
//   * VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK   -> pfnLayerCreateDevice /
//                                                 pfnLayerDestroyDevice
//
// ⚠️ 名字核实：任务书把它们写作 VK_LOADER_DATA /
//    VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK。实测 vk_layer.h 里真实的枚举是：
//        typedef enum VkLayerFunction_ {
//            VK_LAYER_LINK_INFO = 0,
//            VK_LOADER_DATA_CALLBACK = 1,     <-- 不是 VK_LOADER_DATA
//            VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK = 2,
//            VK_LOADER_FEATURES = 3,
//        } VkLayerFunction;
struct InstanceData {
    InstanceDispatchTable dispatch{};

    // 下面这些是 loader 给的回调，仅作记录 / 转发用。
    PFN_vkSetInstanceLoaderData set_instance_loader_data = nullptr;
    PFN_vkLayerCreateDevice layer_create_device = nullptr;
    PFN_vkLayerDestroyDevice layer_destroy_device = nullptr;
    bool has_device_create_callback = false;
};

struct DeviceData {
    DeviceDispatchTable dispatch{};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
};

// 注册 / 查询 / 注销。查询返回的是**值拷贝**（内部持锁后再拷贝），
// 调用方拿到后不会与其它线程的注销操作产生数据竞争。
bool RegisterInstance(VkInstance instance, const InstanceData& data);
bool GetInstanceData(VkInstance instance, InstanceData* out);
bool UnregisterInstance(VkInstance instance);

bool RegisterDevice(VkDevice device, const DeviceData& data);
bool GetDeviceData(VkDevice device, DeviceData* out);
bool UnregisterDevice(VkDevice device);

// 清空（供冒烟测试 / 卸载时使用）。
void ResetRegistry();

// ---------------------------------------------------------------------------
// 拦截点（hooks.cpp 实现）
// ---------------------------------------------------------------------------
// 命名约定：efml4a::hooks::Xxx 对应 Vulkan 的 vkXxx。
// 它们在 GETPROCADDR 宏里被返回给 loader，因此签名必须与 PFN_vkXxx 完全一致。
namespace hooks {

// ---- 实例级 --------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                             const VkAllocationCallbacks* pAllocator,
                                             VkInstance* pInstance);

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                                               VkLayerProperties* pProperties);

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(
    const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties);

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice,
                                           const VkDeviceCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator,
                                           VkDevice* pDevice);

// ---- 设备级 --------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device,
                                                 const VkShaderModuleCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkShaderModule* pShaderModule);

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device,
                                                      VkPipelineCache pipelineCache,
                                                      uint32_t createInfoCount,
                                                      const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkPipeline* pPipelines);

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer commandBuffer,
                                          VkPipelineBindPoint pipelineBindPoint,
                                          VkPipeline pipeline);

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                                               uint32_t firstBinding, uint32_t bindingCount,
                                               const VkBuffer* pBuffers,
                                               const VkDeviceSize* pOffsets);

VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                             VkDeviceSize offset, VkIndexType indexType);

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
                                         uint32_t instanceCount, uint32_t firstIndex,
                                         int32_t vertexOffset, uint32_t firstInstance);

VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
                                  uint32_t instanceCount, uint32_t firstVertex,
                                  uint32_t firstInstance);

VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer commandBuffer,
                                           VkPipelineLayout layout, VkShaderStageFlags stageFlags,
                                           uint32_t offset, uint32_t size, const void* pValues);

VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                                                VkPipelineBindPoint pipelineBindPoint,
                                                VkPipelineLayout layout, uint32_t firstSet,
                                                uint32_t descriptorSetCount,
                                                const VkDescriptorSet* pDescriptorSets,
                                                uint32_t dynamicOffsetCount,
                                                const uint32_t* pDynamicOffsets);

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t submitCount,
                                          const VkSubmitInfo* pSubmits, VkFence fence);

}  // namespace hooks
}  // namespace efml4a

// ---------------------------------------------------------------------------
// 统计计数的 C 导出（供冒烟测试 / 与本仓库的 Rust 工具联动）
// ---------------------------------------------------------------------------
// 返回一个 NUL 结尾的 JSON 字符串，形如：
//   {"layer":"VK_LAYER_EFML4A_model_replacer","counters":{"vkCreateInstance":1,...}}
//
// 所有权：返回的是 thread_local 静态缓冲区，调用方**不要** free；
// 内容在下一次同一线程调用 EFML4A_GetStatsJson() 时失效。
EFML4A_LAYER_EXPORT const char* EFML4A_GetStatsJson();

// ---------------------------------------------------------------------------
// 额外的生命周期拦截点（不在任务书的 14 个之内，但为了让注册表不泄漏而必须实现）
// ---------------------------------------------------------------------------
// 它们只做「清理注册表 + 转发」，**不单独计数**，以保持 EFML4A_GetStatsJson() 的
// counters 集合与任务书要求的 14 项一致。
namespace efml4a {
namespace hooks {

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance,
                                          const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);

// vkCreateBuffer / vkDestroyBuffer：donor 模式需要知道每个 buffer 的 size/usage。
// 同样**不单独计数**（counters 集合保持不变）。
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device,
                                           const VkBufferCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator,
                                           VkBuffer* pBuffer);

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer,
                                        const VkAllocationCallbacks* pAllocator);

}  // namespace hooks
}  // namespace efml4a

// ---------------------------------------------------------------------------
// 额外的「簿记」拦截点：VkPhysicalDevice -> VkInstance 反查
// ---------------------------------------------------------------------------
// 设备级 hook 只能拿到 VkDevice / VkQueue / VkCommandBuffer，拿不到 VkInstance。
// 但 VkPhysicalDevice 与 VkInstance 的归属关系是可以枚举出来的：
//   在 vkEnumeratePhysicalDevices 里把 (physicalDevice -> instance) 记下来，
//   之后 vkCreateDevice(physicalDevice, ...) 就能反查出 instance。
// 同样**不单独计数**。
namespace efml4a {

void RememberPhysicalDevice(VkPhysicalDevice physicalDevice, VkInstance instance);
VkInstance InstanceForPhysicalDevice(VkPhysicalDevice physicalDevice);
void ForgetPhysicalDevices(VkInstance instance);

namespace hooks {

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance,
                                                       uint32_t* pPhysicalDeviceCount,
                                                       VkPhysicalDevice* pPhysicalDevices);

}  // namespace hooks
}  // namespace efml4a

// ---------------------------------------------------------------------------
// 额外的「簿记」拦截点：VkQueue -> VkDevice 反查
// ---------------------------------------------------------------------------
// vkQueueSubmit 只给 VkQueue，而 dispatch table 是按 VkDevice 索引的。
// 正确做法是拦截 vkGetDeviceQueue 建立映射（同样**不单独计数**）。
namespace efml4a {

void RememberQueue(VkQueue queue, VkDevice device);
VkDevice DeviceForQueue(VkQueue queue);
void ForgetQueues(VkDevice device);

namespace hooks {

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                         uint32_t queueIndex, VkQueue* pQueue);

}  // namespace hooks
}  // namespace efml4a

// ---------------------------------------------------------------------------
// 额外的「簿记」拦截点：VkCommandBuffer -> VkDevice 反查
// ---------------------------------------------------------------------------
// 所有 vkCmd* 拦截点拿到的都是 VkCommandBuffer，而 dispatch table 按 VkDevice 索引，
// 所以必须拦 vkAllocateCommandBuffers / vkFreeCommandBuffers 建立映射。
// 当前版本号（Vulkan 1.0 风格）下这是唯一正确的做法。
// 同样**不单独计数**。
namespace efml4a {

void RememberCommandBuffers(VkDevice device, uint32_t count, const VkCommandBuffer* commandBuffers);
void ForgetCommandBuffers(VkDevice device, uint32_t count, const VkCommandBuffer* commandBuffers);
VkDevice DeviceForCommandBuffer(VkCommandBuffer commandBuffer);
void ForgetAllCommandBuffers(VkDevice device);

namespace hooks {

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers);

VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                             uint32_t commandBufferCount,
                                             const VkCommandBuffer* pCommandBuffers);

}  // namespace hooks
}  // namespace efml4a
