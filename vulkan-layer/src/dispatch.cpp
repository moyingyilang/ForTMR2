// ============================================================================
// EFML4A dispatch table 填充 + 实例/设备注册表
//
// 为什么要注册表：
//   Vulkan 的 dispatch 是「按对象」的。loader 通过 vkCreateInstance /
//   vkCreateDevice 的 pNext 链把「下一层的 GetInstanceProcAddr /
//   GetDeviceProcAddr」交给层；层必须把它们原样保存下来，之后每一次拦截到的
//   调用都要用对应 VkInstance / VkDevice 的那一份指针转发下去。
//   因此需要 VkInstance -> 表 和 VkDevice -> 表 两个映射。
//
// 线程安全：
//   用 std::mutex 保护 std::unordered_map（Vulkan 允许从多个线程并发创建对象）。
//   查询接口返回**值拷贝**，避免调用方持锁或读到被并发注销的数据。
// ============================================================================

#include "efml4a/dispatch.h"

#include <mutex>
#include <unordered_map>

#include "efml4a/hook.h"
#include "efml4a/log.h"

namespace efml4a {

namespace {

std::mutex g_instance_mutex;
std::unordered_map<VkInstance, InstanceData> g_instance_map;

std::mutex g_device_mutex;
std::unordered_map<VkDevice, DeviceData> g_device_map;

}  // namespace

// ---------------------------------------------------------------------------
// 注册表
// ---------------------------------------------------------------------------

bool RegisterInstance(VkInstance instance, const InstanceData& data) {
    if (instance == VK_NULL_HANDLE) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    g_instance_map[instance] = data;
    return true;
}

bool GetInstanceData(VkInstance instance, InstanceData* out) {
    if (instance == VK_NULL_HANDLE || out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    const auto it = g_instance_map.find(instance);
    if (it == g_instance_map.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

bool UnregisterInstance(VkInstance instance) {
    if (instance == VK_NULL_HANDLE) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    return g_instance_map.erase(instance) > 0;
}

bool RegisterDevice(VkDevice device, const DeviceData& data) {
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_device_mutex);
    g_device_map[device] = data;
    return true;
}

bool GetDeviceData(VkDevice device, DeviceData* out) {
    if (device == VK_NULL_HANDLE || out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_device_mutex);
    const auto it = g_device_map.find(device);
    if (it == g_device_map.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

bool UnregisterDevice(VkDevice device) {
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_device_mutex);
    return g_device_map.erase(device) > 0;
}

void ResetRegistry() {
    {
        std::lock_guard<std::mutex> lock(g_instance_mutex);
        g_instance_map.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        g_device_map.clear();
    }
}

// ---------------------------------------------------------------------------
// dispatch table 填充
// ---------------------------------------------------------------------------
//
// 下面是显式赋值宏。它们刻意依赖调用点的局部变量名，这是 Vulkan 层里通行的
// 「样板宏」写法（任务书要求的 GETPROCADDR 套路），好处是新增字段只要一行：
//
//     EFML4A_LOAD_INSTANCE_PROC(CreateDevice);
//
// 依赖的局部变量（由 FillXxx 函数提供）：
//     dst        —— 目标表指针
//     next_gipa  —— 下一层的 PFN_vkGetInstanceProcAddr
//     instance   —— 当前 VkInstance
//     next_gdpa / device —— 设备版对应物
//
// 注意：vkGetInstanceProcAddr(instance, ...) 在 instance 为 VK_NULL_HANDLE 时
// 只能取到全局命令；实例级命令必须传入真实 instance。这正是我们要在
// vkCreateInstance 成功之后立刻填表的原因。
#define EFML4A_LOAD_INSTANCE_PROC(field)                                                  \
    do {                                                                                  \
        (dst)->field = reinterpret_cast<decltype((dst)->field)>(                          \
            (next_gipa)((instance), "vk" #field));                                        \
    } while (0)

#define EFML4A_LOAD_DEVICE_PROC(field)                                                    \
    do {                                                                                  \
        (dst)->field = reinterpret_cast<decltype((dst)->field)>(                          \
            (next_gdpa)((device), "vk" #field));                                          \
    } while (0)

void FillInstanceDispatchTable(VkInstance instance, PFN_vkGetInstanceProcAddr next_gipa,
                               InstanceDispatchTable* dst) {
    if (dst == nullptr || next_gipa == nullptr) {
        return;
    }

    // 物理设备扩展枚举入口不能通过 vkGetInstanceProcAddr 拿到，loader 是通过
    // VK_LAYER_LINK_INFO 的 pfnNextGetPhysicalDeviceProcAddr 单独给的，
    // 因此这里必须先保留调用方已经填好的值。
    const PFN_GetPhysicalDeviceProcAddr keep_gpdpa = dst->GetPhysicalDeviceProcAddr;

    EFML4A_LOAD_INSTANCE_PROC(GetInstanceProcAddr);
    EFML4A_LOAD_INSTANCE_PROC(DestroyInstance);
    EFML4A_LOAD_INSTANCE_PROC(EnumerateInstanceLayerProperties);
    EFML4A_LOAD_INSTANCE_PROC(EnumerateInstanceExtensionProperties);
    EFML4A_LOAD_INSTANCE_PROC(EnumerateDeviceExtensionProperties);
    EFML4A_LOAD_INSTANCE_PROC(EnumeratePhysicalDevices);
    // 注：hook 的登记在 hooks::EnumeratePhysicalDevices 里完成
    EFML4A_LOAD_INSTANCE_PROC(GetPhysicalDeviceProperties);
    EFML4A_LOAD_INSTANCE_PROC(GetPhysicalDeviceFeatures);
    EFML4A_LOAD_INSTANCE_PROC(GetPhysicalDeviceQueueFamilyProperties);
    EFML4A_LOAD_INSTANCE_PROC(GetPhysicalDeviceMemoryProperties);
    EFML4A_LOAD_INSTANCE_PROC(CreateDevice);

    // ---- 按需扩展：新增实例级字段时在这里加一行 EFML4A_LOAD_INSTANCE_PROC(Xxx);

    dst->GetPhysicalDeviceProcAddr = keep_gpdpa;
}

void FillDeviceDispatchTable(VkDevice device, PFN_vkGetDeviceProcAddr next_gdpa,
                             DeviceDispatchTable* dst) {
    if (dst == nullptr || next_gdpa == nullptr) {
        return;
    }

    EFML4A_LOAD_DEVICE_PROC(GetDeviceProcAddr);
    EFML4A_LOAD_DEVICE_PROC(DestroyDevice);
    EFML4A_LOAD_DEVICE_PROC(GetDeviceQueue);
    EFML4A_LOAD_DEVICE_PROC(AllocateCommandBuffers);
    EFML4A_LOAD_DEVICE_PROC(FreeCommandBuffers);
    EFML4A_LOAD_DEVICE_PROC(QueueSubmit);
    EFML4A_LOAD_DEVICE_PROC(QueueWaitIdle);
    EFML4A_LOAD_DEVICE_PROC(DeviceWaitIdle);
    EFML4A_LOAD_DEVICE_PROC(CreateShaderModule);
    EFML4A_LOAD_DEVICE_PROC(CreateGraphicsPipelines);
    EFML4A_LOAD_DEVICE_PROC(CreatePipelineLayout);
    EFML4A_LOAD_DEVICE_PROC(CreateBuffer);
    EFML4A_LOAD_DEVICE_PROC(DestroyBuffer);
    EFML4A_LOAD_DEVICE_PROC(AllocateMemory);
    EFML4A_LOAD_DEVICE_PROC(BindBufferMemory);
    EFML4A_LOAD_DEVICE_PROC(CmdBindPipeline);
    EFML4A_LOAD_DEVICE_PROC(CmdBindDescriptorSets);
    EFML4A_LOAD_DEVICE_PROC(CmdBindVertexBuffers);
    EFML4A_LOAD_DEVICE_PROC(CmdBindIndexBuffer);
    EFML4A_LOAD_DEVICE_PROC(CmdDraw);
    EFML4A_LOAD_DEVICE_PROC(CmdDrawIndexed);
    EFML4A_LOAD_DEVICE_PROC(CmdPushConstants);

    // ---- 按需扩展：新增设备级字段时在这里加一行 EFML4A_LOAD_DEVICE_PROC(Xxx);
}

#undef EFML4A_LOAD_INSTANCE_PROC
#undef EFML4A_LOAD_DEVICE_PROC

}  // namespace efml4a

// ---------------------------------------------------------------------------
// VkPhysicalDevice -> VkInstance 反查表
// ---------------------------------------------------------------------------
// 设备级 hook 拿不到 VkInstance，而有时需要按 instance 查状态；这张表补上这一环。
// 数据来源：hooks::EnumeratePhysicalDevices 枚举成功后逐项登记。
namespace efml4a {

namespace {

std::mutex g_physdev_mutex;
std::unordered_map<VkPhysicalDevice, VkInstance> g_physdev_map;

}  // namespace

void RememberPhysicalDevice(VkPhysicalDevice physicalDevice, VkInstance instance) {
    if (physicalDevice == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_physdev_mutex);
    g_physdev_map[physicalDevice] = instance;
}

VkInstance InstanceForPhysicalDevice(VkPhysicalDevice physicalDevice) {
    if (physicalDevice == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    std::lock_guard<std::mutex> lock(g_physdev_mutex);
    const auto it = g_physdev_map.find(physicalDevice);
    return (it == g_physdev_map.end()) ? VK_NULL_HANDLE : it->second;
}

void ForgetPhysicalDevices(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_physdev_mutex);
    for (auto it = g_physdev_map.begin(); it != g_physdev_map.end();) {
        it = (it->second == instance) ? g_physdev_map.erase(it) : std::next(it);
    }
}

}  // namespace efml4a

#include <iterator>

// ---------------------------------------------------------------------------
// VkQueue -> VkDevice 反查表
// ---------------------------------------------------------------------------
// 数据来源：hooks::GetDeviceQueue（vkGetDeviceQueue 成功后被调用）。
namespace efml4a {

namespace {

std::mutex g_queue_mutex;
std::unordered_map<VkQueue, VkDevice> g_queue_map;

}  // namespace

void RememberQueue(VkQueue queue, VkDevice device) {
    if (queue == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue_map[queue] = device;
}

VkDevice DeviceForQueue(VkQueue queue) {
    if (queue == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    const auto it = g_queue_map.find(queue);
    return (it == g_queue_map.end()) ? VK_NULL_HANDLE : it->second;
}

void ForgetQueues(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    for (auto it = g_queue_map.begin(); it != g_queue_map.end();) {
        it = (it->second == device) ? g_queue_map.erase(it) : std::next(it);
    }
}

}  // namespace efml4a

// ---------------------------------------------------------------------------
// VkCommandBuffer -> VkDevice 反查表
// ---------------------------------------------------------------------------
// 数据来源：hooks::AllocateCommandBuffers（vkAllocateCommandBuffers 成功后被调用）。
// vkCmd* 拦截点靠它把 VkCommandBuffer 解析回 VkDevice，进而拿到 dispatch table。
namespace efml4a {

namespace {

std::mutex g_cmdbuf_mutex;
std::unordered_map<VkCommandBuffer, VkDevice> g_cmdbuf_map;

}  // namespace

void RememberCommandBuffers(VkDevice device, uint32_t count,
                            const VkCommandBuffer* commandBuffers) {
    if (count == 0 || commandBuffers == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_cmdbuf_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        if (commandBuffers[i] != VK_NULL_HANDLE) {
            g_cmdbuf_map[commandBuffers[i]] = device;
        }
    }
}

void ForgetCommandBuffers(VkDevice device, uint32_t count,
                          const VkCommandBuffer* commandBuffers) {
    (void)device;
    if (count == 0 || commandBuffers == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_cmdbuf_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        g_cmdbuf_map.erase(commandBuffers[i]);
    }
}

VkDevice DeviceForCommandBuffer(VkCommandBuffer commandBuffer) {
    if (commandBuffer == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    std::lock_guard<std::mutex> lock(g_cmdbuf_mutex);
    const auto it = g_cmdbuf_map.find(commandBuffer);
    return (it == g_cmdbuf_map.end()) ? VK_NULL_HANDLE : it->second;
}

void ForgetAllCommandBuffers(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_cmdbuf_mutex);
    for (auto it = g_cmdbuf_map.begin(); it != g_cmdbuf_map.end();) {
        it = (it->second == device) ? g_cmdbuf_map.erase(it) : std::next(it);
    }
}

}  // namespace efml4a
