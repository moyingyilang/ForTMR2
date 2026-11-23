#pragma once

// ============================================================================
// EFML4A — Vulkan 隐式层（implicit layer）骨架
// 层入口声明与版本宏
//
// 背景：EFML4A = "Arknights: Endfield Mod Loader for Android"。
// PC 上 3DMigoto 通过 DXGI 代理 DLL 把 D3D11 调用劫持下来；Android 没有 DXGI，
// 最接近的对应物就是 Vulkan 隐式层：由系统 loader 加载进目标 app 进程的原生库，
// 可以看到并修改每一个 vk* 调用。
// ============================================================================

#include <vulkan/vulkan.h>

// vk_layer.h 由 KhronosGroup/Vulkan-Headers 提供（已核实：include/vulkan/vk_layer.h）。
// 它提供 VkNegotiateLayerInterface、CURRENT_LOADER_LAYER_INTERFACE_VERSION、
// VkLayerInstanceCreateInfo / VkLayerDeviceCreateInfo、VkLayerInstanceLink /
// VkLayerDeviceLink。它 **不** 提供 dispatch table —— 见 dispatch.h 顶部说明。
#include <vulkan/vk_layer.h>

// ---------------------------------------------------------------------------
// 符号导出
// ---------------------------------------------------------------------------
// extern "C" 保证 C 符号名不被 mangle —— Android loader 与桌面 loader 都是按
// 字符串名字用 dlsym / GetProcAddr 查找入口的。
// visibility("default") 保证在 -fvisibility=hidden 下依然导出。
#define EFML4A_LAYER_EXPORT extern "C" __attribute__((visibility("default")))

// ---------------------------------------------------------------------------
// 层身份与版本
// ---------------------------------------------------------------------------
// 这个名字必须与 layer/VkLayer_EFML4A.json.in 里的 layer.name 完全一致。
#define EFML4A_LAYER_NAME "VK_LAYER_EFML4A_model_replacer"

#define EFML4A_LAYER_DESCRIPTION \
    "EFML4A model replacer (implicit layer skeleton for Arknights: Endfield mod loading)"

// VkLayerProperties.specVersion —— 本层声明支持的 Vulkan 版本。
// 刻意选 1.3 而不是 1.4：层只是转发，声明一个较保守、覆盖面更广的版本更安全。
#define EFML4A_LAYER_SPEC_VERSION VK_API_VERSION_1_3

// VkLayerProperties.implementationVersion；与 manifest 的 implementation_version 对应。
#define EFML4A_LAYER_IMPLEMENTATION_VERSION 1

// ---------------------------------------------------------------------------
// Android 专用「自省」(introspection) 入口
// ---------------------------------------------------------------------------
// 重要核实结论（详见 README「Android 侧 manifest 与启用方式」）：
// Android 的 loader **完全不读 .json manifest**，它在 /data/local/debug/vulkan 等
// 目录里直接扫描文件名形如 libVkLayer*.so 的共享库，然后用 dlsym 从这个 .so 里
// 取下面这些「自省」入口来获知层信息：
//     vkEnumerateInstanceLayerProperties        （必需）
//     vkEnumerateInstanceExtensionProperties    （必需）
//     vkEnumerateDeviceLayerProperties          （可选）
//     vkEnumerateDeviceExtensionProperties      （可选）
// 以及用于真正挂链的 vkGetInstanceProcAddr / vkGetDeviceProcAddr。
// 参考实现：AOSP CTS hostsidetests/gputools/layers/jni/nullLayer.cpp。
//
// 因此下面这两个函数走的是「自报家门」语义（报告本层自身的属性），
// 而不是沿调用链向下转发 —— 后者是 hooks.cpp 里同名 hook 的职责。
namespace efml4a {
namespace android_introspect {

VkResult EnumerateInstanceLayerProperties(uint32_t* pCount, VkLayerProperties* pProperties);
VkResult EnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pCount,
                                              VkExtensionProperties* pProperties);

}  // namespace android_introspect
}  // namespace efml4a

// ---------------------------------------------------------------------------
// 层入口（layer.cpp 实现）
// ---------------------------------------------------------------------------

// loader 与本层协商接口版本的「内核入口」。必须导出。
EFML4A_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct);

// 协商后交给 loader 的三个函数指针。刻意使用 EFML4A_ 前缀（任务要求），
// 同时 layer.cpp 另外导出了无前缀的 vkGetInstanceProcAddr / vkGetDeviceProcAddr——
// 因为 **Android loader 不调用协商函数**，它只按名字 dlsym
// vkGetInstanceProcAddr / vkGetDeviceProcAddr。两者指向同一份实现。
EFML4A_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
EFML4A_GetInstanceProcAddr(VkInstance instance, const char* pName);

EFML4A_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
EFML4A_GetDeviceProcAddr(VkDevice device, const char* pName);

EFML4A_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
EFML4A_GetPhysicalDeviceProcAddr(VkInstance instance, const char* pName);

// ---------------------------------------------------------------------------
// 「无 instance」全局命令的转发指针
// ---------------------------------------------------------------------------
// vkEnumerateInstanceLayerProperties / vkEnumerateInstanceExtensionProperties 是
// 在 vkCreateInstance 之前就可能被调用的全局命令，而此时 loader 还没有通过
// vkCreateInstance 的 pNext 链把「下一层的 GIPA」交给我们。
// 这里保存最近一次 vkCreateInstance 捕获到的下一层 GIPA，供上述前置命令尽力转发。
// TODO: 更正确的做法是使用 manifest 的 "pre_instance_functions" 段（桌面 loader 支持）。
namespace efml4a {

void SetPreInstanceGetInstanceProcAddr(PFN_vkGetInstanceProcAddr gipa);
PFN_vkGetInstanceProcAddr GetPreInstanceGetInstanceProcAddr();

// 桌面 loader 是否真的调用过 vkNegotiateLoaderLayerInterfaceVersion。
// 用于区分「桌面 loader 链路」与「Android loader 直接 dlsym 自省」两条路径。
bool DesktopLoaderNegotiated();

}  // namespace efml4a

// 由 vkNegotiateLoaderLayerInterfaceVersion 在协商成功时置位。
namespace efml4a {
void MarkDesktopLoaderNegotiated();
}  // namespace efml4a
