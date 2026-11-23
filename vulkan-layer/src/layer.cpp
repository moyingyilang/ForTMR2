// ============================================================================
// EFML4A 层入口
//   * vkNegotiateLoaderLayerInterfaceVersion  —— 桌面 / Khronos loader 的内核入口
//   * EFML4A_GetInstanceProcAddr / EFML4A_GetDeviceProcAddr /
//     EFML4A_GetPhysicalDeviceProcAddr        —— 协商后交给 loader 的函数指针
//   * vkGetInstanceProcAddr / vkGetDeviceProcAddr（无前缀，额外导出）
//                                            —— Android loader 用 dlsym 按名字查找
//   * vkEnumerateInstanceLayerProperties / vkEnumerateInstanceExtensionProperties
//                                            —— Android 的「自省」入口（自报家门）
//
// 为什么无前缀的 vkGetInstanceProcAddr 必须导出：
//   已核实 AOSP frameworks/native/vulkan/libvulkan/layers_extensions.cpp 的
//   LayerLibrary::GetGPA() 实现为：
//       std::string layer_name{layer.properties.layerName};
//       if (void* gpa = GetTrampoline(layer_name.append(gpa_name).c_str())) return gpa;
//       return GetTrampoline((std::string{"vk"}.append(gpa_name)).c_str());
//   即先试 "<层名>GetInstanceProcAddr"，再试 "vkGetInstanceProcAddr"。
//   并且 LayerChain::LoadLayer() 在找不到 vkGetInstanceProcAddr 时会直接
//   返回 VK_ERROR_LAYER_NOT_PRESENT —— 所以 Android 上不导出它 = 层根本挂不上。
//   Android **从不**调用 vkNegotiateLoaderLayerInterfaceVersion。
// ============================================================================

#include "efml4a/layer.h"

#include <atomic>
#include <cstring>

#include "efml4a/dispatch.h"
#include "efml4a/hook.h"
#include "efml4a/log.h"

namespace efml4a {

namespace {

std::atomic<PFN_vkGetInstanceProcAddr> g_pre_instance_gipa{nullptr};
std::atomic<bool> g_desktop_negotiated{false};

}  // namespace

void SetPreInstanceGetInstanceProcAddr(PFN_vkGetInstanceProcAddr gipa) {
    g_pre_instance_gipa.store(gipa, std::memory_order_release);
}

PFN_vkGetInstanceProcAddr GetPreInstanceGetInstanceProcAddr() {
    return g_pre_instance_gipa.load(std::memory_order_acquire);
}

bool DesktopLoaderNegotiated() { return g_desktop_negotiated.load(std::memory_order_acquire); }

}  // namespace efml4a

// ============================================================================
// GETPROCADDR 宏
// ============================================================================
// 这是 Vulkan 层里的标准套路：把 "vkXxx" 字符串映射到本层的实现函数。
// 未命中的名字原样转发给下一层。
#define EFML4A_GETPROCADDR(func)                                                        \
    if (std::strcmp(pName, "vk" #func) == 0) {                                          \
        return reinterpret_cast<PFN_vkVoidFunction>(&::efml4a::hooks::func);            \
    }

EFML4A_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
EFML4A_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (pName == nullptr) {
        return nullptr;
    }

    // ---- 1) 本层拦截的入口优先 -------------------------------------------
    // 全局命令（无 instance 也可用）
    EFML4A_GETPROCADDR(CreateInstance)
    EFML4A_GETPROCADDR(DestroyInstance)
    EFML4A_GETPROCADDR(EnumerateInstanceLayerProperties)
    EFML4A_GETPROCADDR(EnumerateInstanceExtensionProperties)
    // 实例级
    EFML4A_GETPROCADDR(CreateDevice)
    EFML4A_GETPROCADDR(EnumeratePhysicalDevices)
    // 设备级（通过 GIPA 也能取到，官方 sample 层同样这么做）
    EFML4A_GETPROCADDR(DestroyDevice)
    EFML4A_GETPROCADDR(GetDeviceQueue)
    EFML4A_GETPROCADDR(AllocateCommandBuffers)
    EFML4A_GETPROCADDR(FreeCommandBuffers)
    EFML4A_GETPROCADDR(CreateShaderModule)
    EFML4A_GETPROCADDR(CreateGraphicsPipelines)
    // donor 模式需要 buffer 的 size/usage -> 必须拦 vkCreateBuffer / vkDestroyBuffer
    EFML4A_GETPROCADDR(CreateBuffer)
    EFML4A_GETPROCADDR(DestroyBuffer)
    EFML4A_GETPROCADDR(CmdBindPipeline)
    EFML4A_GETPROCADDR(CmdBindVertexBuffers)
    EFML4A_GETPROCADDR(CmdBindIndexBuffer)
    EFML4A_GETPROCADDR(CmdDrawIndexed)
    EFML4A_GETPROCADDR(CmdDraw)
    EFML4A_GETPROCADDR(CmdPushConstants)
    EFML4A_GETPROCADDR(CmdBindDescriptorSets)
    EFML4A_GETPROCADDR(QueueSubmit)
    // ---- 按需扩展：新增拦截点时在此加一行 EFML4A_GETPROCADDR(Xxx)

    // ---- 2) 其余原样转发给下一层 -----------------------------------------
    efml4a::InstanceData data;
    if (efml4a::GetInstanceData(instance, &data) && data.dispatch.GetInstanceProcAddr != nullptr) {
        return data.dispatch.GetInstanceProcAddr(instance, pName);
    }

    // 还没有 instance（例如 loader 在 vkCreateInstance 之前问某个命令）：
    // 用最近捕获到的下一层 GIPA 尽力转发。
    PFN_vkGetInstanceProcAddr pre = efml4a::GetPreInstanceGetInstanceProcAddr();
    if (pre != nullptr) {
        return pre(instance, pName);
    }

    return nullptr;
}

EFML4A_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
EFML4A_GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (pName == nullptr) {
        return nullptr;
    }

    // 设备级拦截点。注意：层**必须**在这里返回自己的实现，否则设备命令会绕过本层。
    EFML4A_GETPROCADDR(DestroyDevice)
    EFML4A_GETPROCADDR(GetDeviceQueue)
    EFML4A_GETPROCADDR(AllocateCommandBuffers)
    EFML4A_GETPROCADDR(FreeCommandBuffers)
    EFML4A_GETPROCADDR(CreateShaderModule)
    EFML4A_GETPROCADDR(CreateGraphicsPipelines)
    // donor 模式需要 buffer 的 size/usage -> 必须拦 vkCreateBuffer / vkDestroyBuffer
    EFML4A_GETPROCADDR(CreateBuffer)
    EFML4A_GETPROCADDR(DestroyBuffer)
    EFML4A_GETPROCADDR(CmdBindPipeline)
    EFML4A_GETPROCADDR(CmdBindVertexBuffers)
    EFML4A_GETPROCADDR(CmdBindIndexBuffer)
    EFML4A_GETPROCADDR(CmdDrawIndexed)
    EFML4A_GETPROCADDR(CmdDraw)
    EFML4A_GETPROCADDR(CmdPushConstants)
    EFML4A_GETPROCADDR(CmdBindDescriptorSets)
    EFML4A_GETPROCADDR(QueueSubmit)
    // ---- 按需扩展

    efml4a::DeviceData data;
    if (efml4a::GetDeviceData(device, &data) && data.dispatch.GetDeviceProcAddr != nullptr) {
        return data.dispatch.GetDeviceProcAddr(device, pName);
    }
    return nullptr;
}

// loader 需要它来枚举物理设备扩展（vkEnumerateDeviceExtensionProperties 之类）。
EFML4A_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
EFML4A_GetPhysicalDeviceProcAddr(VkInstance instance, const char* pName) {
    if (pName == nullptr) {
        return nullptr;
    }

    efml4a::InstanceData data;
    if (efml4a::GetInstanceData(instance, &data)) {
        if (data.dispatch.GetPhysicalDeviceProcAddr != nullptr) {
            return data.dispatch.GetPhysicalDeviceProcAddr(instance, pName);
        }
        // 回退：有些 loader 不单独提供 GetPhysicalDeviceProcAddr，用 GIPA 即可。
        if (data.dispatch.GetInstanceProcAddr != nullptr) {
            return data.dispatch.GetInstanceProcAddr(instance, pName);
        }
    }
    return nullptr;
}

#undef EFML4A_GETPROCADDR

// ============================================================================
// vkNegotiateLoaderLayerInterfaceVersion —— 层的「内核入口」
// ============================================================================
// 已核实的结构（KhronosGroup/Vulkan-Headers vulkan-sdk-1.4.321.0,
// include/vulkan/vk_layer.h）：
//   typedef struct VkNegotiateLayerInterface {
//       VkNegotiateLayerStructType sType;      // LAYER_NEGOTIATE_INTERFACE_STRUCT == 1
//       void *pNext;
//       uint32_t loaderLayerInterfaceVersion;  // 桌面 loader 传 2
//       PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
//       PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;
//       PFN_GetPhysicalDeviceProcAddr pfnGetPhysicalDeviceProcAddr;
//   } VkNegotiateLayerInterface;
//   #define CURRENT_LOADER_LAYER_INTERFACE_VERSION 2
//   #define MIN_SUPPORTED_LOADER_LAYER_INTERFACE_VERSION 1
EFML4A_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (pVersionStruct == nullptr) {
        EFML4A_LOGE("vkNegotiateLoaderLayerInterfaceVersion: pVersionStruct is NULL");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // sType 校验
    if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        EFML4A_LOGE("vkNegotiateLoaderLayerInterfaceVersion: bad sType %d",
                    static_cast<int>(pVersionStruct->sType));
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // loader 要求的版本低于我们能支持的最低版本 -> 拒绝
    if (pVersionStruct->loaderLayerInterfaceVersion <
        MIN_SUPPORTED_LOADER_LAYER_INTERFACE_VERSION) {
        EFML4A_LOGE("vkNegotiateLoaderLayerInterfaceVersion: loader wants v%u, min supported is %d",
                    pVersionStruct->loaderLayerInterfaceVersion,
                    MIN_SUPPORTED_LOADER_LAYER_INTERFACE_VERSION);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // loader 要求的版本高于我们实现的 -> 回落到我们支持的版本（这是协议要求的行为）
    if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION) {
        pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    }

    pVersionStruct->pfnGetInstanceProcAddr = EFML4A_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = EFML4A_GetDeviceProcAddr;

    // GetPhysicalDeviceProcAddr 是接口版本 2 才引入的（vk_layer.h 注释：
    // "Version 2 - Add support for vk_layerGetPhysicalDeviceProcAddr"）。
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = EFML4A_GetPhysicalDeviceProcAddr;
    } else {
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }

    efml4a::log::InitFromEnv();
    efml4a::MarkDesktopLoaderNegotiated();
    return VK_SUCCESS;
}

// ----------------------------------------------------------------------------
// 协商成功标记（实现放在文件末尾，复用上面匿名命名空间里的 atomic）
// ----------------------------------------------------------------------------
namespace efml4a {

void MarkDesktopLoaderNegotiated() {
    g_desktop_negotiated.store(true, std::memory_order_release);
    EFML4A_LOGI("desktop loader negotiated interface version %d",
                CURRENT_LOADER_LAYER_INTERFACE_VERSION);
}

}  // namespace efml4a

// ============================================================================
// Android 「自省」入口之二：vkEnumerateDeviceLayerProperties
// ============================================================================
// 为什么必须导出它（已核实 AOSP layers_extensions.cpp 的 EnumerateLayers）：
//   Android loader 用「实例层属性与设备层属性逐字节 memcmp 相等」来判定
//   layer.is_global，而 is_global 决定这个层会不会被挂进**设备**调用链：
//
//       for (size_t j = 0; j < num_device_layers; j++) {
//           const auto& dev_props = properties[num_instance_layers + j];
//           if (memcmp(&props, &dev_props, sizeof(props)) == 0) {
//               layer.is_global = true; break;
//           }
//       }
//
//   EFML4A 要拦 vkCmdDrawIndexed / vkQueueSubmit 这类设备命令，所以必须报告
//   与实例属性**完全一致**的一份属性。这里直接复用同一个实现即可。
// 参考：AOSP CTS hostsidetests/gputools/layers/jni/nullLayer.cpp 也是这么做的。
EFML4A_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount,
                                 VkLayerProperties* pProperties) {
    // 设备层（VkLayerProperties 的 device 维度）在 Vulkan 1.0 之后已被废弃，
    // 这个入口存在的唯一目的就是让 Android loader 把本层标记成 global。
    (void)physicalDevice;
    return efml4a::android_introspect::EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

// 无前缀的 GIPA/GDPA（Android loader 用 dlsym 按名字查找）。
// 与协商路径返回的是同一份实现，见本文件顶部说明。
EFML4A_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return EFML4A_GetInstanceProcAddr(instance, pName);
}

EFML4A_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return EFML4A_GetDeviceProcAddr(device, pName);
}

// Android 的「自省」入口之三：实例层枚举 / 实例扩展枚举的 dlsym 目标。
// 注意语义区别（详见 layer.h 顶部说明）：
//   * 这两个导出函数是给 Android loader **直接 dlsym** 用的 -> 自报家门；
//   * hooks.cpp 里的同名 hook 是给桌面 loader 的调用链用的 -> 计数 + 转发。
EFML4A_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    return efml4a::android_introspect::EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

EFML4A_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount,
                                      VkExtensionProperties* pProperties) {
    return efml4a::android_introspect::EnumerateInstanceExtensionProperties(
        pLayerName, pPropertyCount, pProperties);
}
