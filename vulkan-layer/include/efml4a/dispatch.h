#pragma once

// ============================================================================
// EFML4A instance / device dispatch table
// ============================================================================

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

// ============================================================================
// ⚠️ 重要核实结论：VkLayerInstanceDispatchTable / VkLayerDeviceDispatchTable
//    并不在 vulkan/vk_layer.h 里 —— 任务书对该头文件的假设不成立。
// ============================================================================
// 实测（KhronosGroup/Vulkan-Headers，tag vulkan-sdk-1.4.321.0）：
//
//   1) include/vulkan/vk_layer.h 全文只有 189 行，**没有**任何 dispatch table。
//      它只提供：VkNegotiateLayerInterface、CURRENT_LOADER_LAYER_INTERFACE_VERSION(=2)、
//      VkLayerFunction 枚举、VkLayerInstanceCreateInfo / VkLayerDeviceCreateInfo、
//      VkLayerInstanceLink / VkLayerDeviceLink。grep 整个仓库都搜不到
//      "VkLayerInstanceDispatchTable"。
//
//   2) 真正的 dispatch table 定义在 **Vulkan-Loader** 仓库的生成文件里：
//        Vulkan-Loader/loader/generated/vk_layer_dispatch_table.h
//      由 scripts/loader_extension_generator.py 生成，**不随 Vulkan-Headers 分发**。
//      该文件里：
//        - 实例表确实叫 VkLayerInstanceDispatchTable（typedef struct ..._ {...}）
//          字段名是去掉 vk 前缀的命令名（CreateInstance / GetInstanceProcAddr / ...），
//          外加一个手工添加的 GetPhysicalDeviceProcAddr。
//        - 设备表叫 VkLayerDispatchTable —— **不是** VkLayerDeviceDispatchTable！
//          且第一个字段是 `uint64_t magic; // DEVICE_DISP_TABLE_MAGIC_NUMBER`。
//
// 结论：层不能直接 include 官方的生成文件（它不在 Vulkan-Headers 里）。
// 本骨架按下述方式处理 —— 自己写显式赋值宏，并只填充真正用到的函数指针：
//   * 自定义两个「最小子集」表，字段名刻意与 loader 生成表逐字保持一致，
//     这样日后如果想把官方生成表接进来，改动只是替换类型名。
//   * 每个表尾部都有 "按需扩展" 注释，说明新增拦截点时该往哪加。
// ============================================================================

namespace efml4a {

// ---------------------------------------------------------------------------
// Instance dispatch table（VkLayerInstanceDispatchTable 的最小子集）
// ---------------------------------------------------------------------------
struct InstanceDispatchTable {
    // loader 生成表里这个字段是手工添加的，用来支持物理设备扩展枚举。
    PFN_GetPhysicalDeviceProcAddr GetPhysicalDeviceProcAddr = nullptr;

    // ---- Core Vulkan 1.0 commands（本骨架用到的部分）
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;

    // ---- 按需扩展 ----------------------------------------------------------
    // 新增实例级拦截点时，在这里加 PFN_vkXxx Xxx = nullptr; 并在
    // src/dispatch.cpp 的 FillInstanceDispatchTable() 里加一行
    // EFML4A_LOAD_INSTANCE_PROC(dst.Xxx, next_gipa, instance);
    // 例如要换模型，至少要加：
    //   PFN_vkGetPhysicalDeviceFormatProperties      GetPhysicalDeviceFormatProperties;
    //   PFN_vkGetPhysicalDeviceImageFormatProperties GetPhysicalDeviceImageFormatProperties;
    //   PFN_vkCreateBuffer / PFN_vkAllocateMemory / PFN_vkBindBufferMemory（见 hooks.cpp TODO）
};

// ---------------------------------------------------------------------------
// Device dispatch table（VkLayerDispatchTable 的最小子集）
// ---------------------------------------------------------------------------
struct DeviceDispatchTable {
    // loader 生成表在设备表首部有 magic 字段（DEVICE_DISP_TABLE_MAGIC_NUMBER）。
    // 层自己持有的表不需要被 loader 校验，但保留字段并置 0，以免日后切换到官方
    // 生成表时出现偏移差异带来的误读。
    uint64_t magic = 0;

    // ---- Core Vulkan 1.0 commands（本骨架用到的部分）
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    // donor 模式要用它清理 VkBuffer 注册表（见 hooks.cpp）。
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;

    // ---- 按需扩展 ----------------------------------------------------------
    // 换模型真正需要但本骨架尚未拦截的入口（见 hooks.cpp 顶部的设计草案）：
    //   PFN_vkGetBufferMemoryRequirements  GetBufferMemoryRequirements;
    //   PFN_vkMapMemory / PFN_vkUnmapMemory
    //   PFN_vkCreateComputePipelines / PFN_vkCmdDispatch / PFN_vkCmdDispatchIndirect
    //   PFN_vkCmdDrawIndirect / PFN_vkCmdDrawIndexedIndirect
    //   PFN_vkCmdBindVertexBuffers2 / PFN_vkCmdBindIndexBuffer2（Vulkan 1.3）
    //   PFN_vkQueueSubmit2
};

}  // namespace efml4a

// ---------------------------------------------------------------------------
// dispatch table 填充（dispatch.cpp 实现）
// ---------------------------------------------------------------------------
namespace efml4a {

// 这两个函数内部的赋值宏要求调用点存在同名局部变量：
//   FillInstanceDispatchTable: `instance`(VkInstance), `next_gipa`, `dst`
//   FillDeviceDispatchTable  : `device`(VkDevice),     `next_gdpa`, `dst`
void FillInstanceDispatchTable(VkInstance instance, PFN_vkGetInstanceProcAddr next_gipa,
                               InstanceDispatchTable* dst);

void FillDeviceDispatchTable(VkDevice device, PFN_vkGetDeviceProcAddr next_gdpa,
                             DeviceDispatchTable* dst);

}  // namespace efml4a
