// ============================================================================
// EFML4A 拦截点实现
//
// 当前阶段目标（骨架）：证明「能编译、能装载、能拦截并转发调用」。
// 每个拦截点都做三件事：
//   1) 计数（std::atomic<uint64_t>）
//   2) 打日志（受 EFML4A_LOG_LEVEL 控制）
//   3) 从 VkInstance / VkDevice 查 dispatch table，把调用原样转发给下一层
// 真正的「换模型」（替换顶点/索引缓冲、替换贴图）尚未实现，
// 见本文件下半部分的 BUFFER REPLACEMENT 设计草案。
// ============================================================================

#include "efml4a/hook.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "efml4a/log.h"
#include "efml4a/mode.h"
#include "efml4a/recon.h"

namespace efml4a {

// ---------------------------------------------------------------------------
// 统计计数
// ---------------------------------------------------------------------------
namespace {

std::atomic<uint64_t> g_stats[kStatCount];

const char* const kStatNames[kStatCount] = {
    "vkCreateInstance",
    "vkEnumerateInstanceLayerProperties",
    "vkEnumerateInstanceExtensionProperties",
    "vkCreateDevice",
    "vkCreateGraphicsPipelines",
    "vkCreateShaderModule",
    "vkCmdBindPipeline",
    "vkCmdBindVertexBuffers",
    "vkCmdBindIndexBuffer",
    "vkCmdDrawIndexed",
    "vkCmdDraw",
    "vkCmdPushConstants",
    "vkCmdBindDescriptorSets",
    "vkQueueSubmit",
};

}  // namespace

const char* StatName(StatId id) {
    if (id < 0 || id >= kStatCount) {
        return "invalid";
    }
    return kStatNames[id];
}

void Bump(StatId id) {
    if (id < 0 || id >= kStatCount) {
        return;
    }
    g_stats[id].fetch_add(1, std::memory_order_relaxed);
}

uint64_t GetStat(StatId id) {
    if (id < 0 || id >= kStatCount) {
        return 0;
    }
    return g_stats[id].load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// loader 在 pNext 链里塞给我们的信息
// ---------------------------------------------------------------------------
// 已核实（Vulkan-Headers v1.4.321.0 include/vulkan/vk_layer.h）：
//
//   typedef struct {
//       VkStructureType sType;   // VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
//       const void *pNext;
//       VkLayerFunction function;
//       union {
//           VkLayerInstanceLink *pLayerInfo;                       // VK_LAYER_LINK_INFO (=0)
//           PFN_vkSetInstanceLoaderData pfnSetInstanceLoaderData;   // VK_LOADER_DATA_CALLBACK (=1)
//           struct {
//               PFN_vkLayerCreateDevice pfnLayerCreateDevice;       // VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK (=2)
//               PFN_vkLayerDestroyDevice pfnLayerDestroyDevice;
//           } layerDevice;
//           VkLoaderFeatureFlags loaderFeatures;                    // VK_LOADER_FEATURES (=3)
//       } u;
//   } VkLayerInstanceCreateInfo;
//
//   typedef struct {
//       VkStructureType sType;   // VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
//       const void *pNext;
//       VkLayerFunction function;
//       union {
//           VkLayerDeviceLink *pLayerInfo;
//           PFN_vkSetDeviceLoaderData pfnSetDeviceLoaderData;
//       } u;
//   } VkLayerDeviceCreateInfo;
//
//   typedef struct VkLayerInstanceLink_ {
//       struct VkLayerInstanceLink_ *pNext;
//       PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
//       PFN_GetPhysicalDeviceProcAddr pfnNextGetPhysicalDeviceProcAddr;
//   } VkLayerInstanceLink;
//
//   typedef struct VkLayerDeviceLink_ {
//       struct VkLayerDeviceLink_ *pNext;
//       PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
//       PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
//   } VkLayerDeviceLink;
//
// ⚠️ 我实际核对时还发现任务书里的一个笔误：枚举成员叫
//    VK_LOADER_DATA_CALLBACK，**不是** VK_LOADER_DATA。

namespace {

struct InstanceLinkInfo {
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    PFN_GetPhysicalDeviceProcAddr next_gpdpa = nullptr;
    PFN_vkSetInstanceLoaderData set_instance_loader_data = nullptr;
    PFN_vkLayerCreateDevice layer_create_device = nullptr;
    PFN_vkLayerDestroyDevice layer_destroy_device = nullptr;
    bool has_device_create_callback = false;
};

// 遍历 vkCreateInstance 的 pNext 链，取出 loader 给的信息并**把链向前推进一格**
// （推进是必须的：否则下一层会看到自己，形成死循环）。
//
// 安全性说明：把任意 pNext 结构体强转成 VkLayerInstanceCreateInfo 是 Vulkan 层的
// 标准做法 —— 所有 Vulkan 结构体的前两个字段都是 sType/pNext，因此读取它们是安全的；
// 只有 sType 匹配时我们才会去读 union。
bool ExtractInstanceLinkInfo(const VkInstanceCreateInfo* pCreateInfo, InstanceLinkInfo* out) {
    if (pCreateInfo == nullptr || out == nullptr) {
        return false;
    }

    // const_cast：loader 明确要求层修改（推进）这条链。
    auto* header = reinterpret_cast<VkLayerInstanceCreateInfo*>(
        const_cast<void*>(pCreateInfo->pNext));

    while (header != nullptr) {
        if (header->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            switch (header->function) {
                case VK_LAYER_LINK_INFO:
                    if (header->u.pLayerInfo != nullptr) {
                        out->next_gipa = header->u.pLayerInfo->pfnNextGetInstanceProcAddr;
                        out->next_gpdpa = header->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
                        // 推进链表，交给下一层
                        header->u.pLayerInfo = header->u.pLayerInfo->pNext;
                    }
                    break;

                case VK_LOADER_DATA_CALLBACK:
                    // 这个回调用来把我们自己创建的 Vulkan 对象「登记」进 loader，
                    // 否则 loader 不认识这些对象，后续调用会失败。
                    out->set_instance_loader_data = header->u.pfnSetInstanceLoaderData;
                    break;

                case VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK:
                    // Android / 新版 loader 用这条路径让层创建设备。
                    out->layer_create_device = header->u.layerDevice.pfnLayerCreateDevice;
                    out->layer_destroy_device = header->u.layerDevice.pfnLayerDestroyDevice;
                    out->has_device_create_callback = true;
                    break;

                case VK_LOADER_FEATURES:
                default:
                    break;
            }
        }
        header = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(header->pNext));
    }

    return out->next_gipa != nullptr;
}

struct DeviceLinkInfo {
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
    PFN_vkSetDeviceLoaderData set_device_loader_data = nullptr;
};

bool ExtractDeviceLinkInfo(const VkDeviceCreateInfo* pCreateInfo, DeviceLinkInfo* out) {
    if (pCreateInfo == nullptr || out == nullptr) {
        return false;
    }

    auto* header =
        reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(pCreateInfo->pNext));

    while (header != nullptr) {
        if (header->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            switch (header->function) {
                case VK_LAYER_LINK_INFO:
                    if (header->u.pLayerInfo != nullptr) {
                        out->next_gipa = header->u.pLayerInfo->pfnNextGetInstanceProcAddr;
                        out->next_gdpa = header->u.pLayerInfo->pfnNextGetDeviceProcAddr;
                        header->u.pLayerInfo = header->u.pLayerInfo->pNext;  // 推进链
                    }
                    break;

                case VK_LOADER_DATA_CALLBACK:
                    out->set_device_loader_data = header->u.pfnSetDeviceLoaderData;
                    break;

                default:
                    break;
            }
        }
        header = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(header->pNext));
    }

    return out->next_gdpa != nullptr;
}

// 取设备 dispatch table 的样板宏。失败时打一次日志并直接 return（retval 可留空）。
#define EFML4A_DEVICE_TABLE_OR_RETURN(device, retval)                                        \
    ::efml4a::DeviceData efml4a_dev_;                                                        \
    if (!::efml4a::GetDeviceData((device), &efml4a_dev_)) {                                  \
        EFML4A_LOG_ONCE(EFML4A_LOGE, "device %p not registered, dropping call",              \
                        reinterpret_cast<const void*>(device));                              \
        return retval;                                                                       \
    }

// 命令缓冲版本的样板宏：vkCmd* 只拿得到 VkCommandBuffer，需要先反查 VkDevice。
// 用法：EFML4A_CMDBUF_TABLE_OR_RETURN(commandBuffer, );   <- 第二个参数留空即 return;
#define EFML4A_CMDBUF_TABLE_OR_RETURN(commandBuffer, retval)                                 \
    ::efml4a::DeviceData efml4a_dev_;                                                        \
    do {                                                                                     \
        const VkDevice efml4a_cb_device_ = ::efml4a::DeviceForCommandBuffer(commandBuffer);  \
        if (efml4a_cb_device_ == VK_NULL_HANDLE ||                                           \
            !::efml4a::GetDeviceData(efml4a_cb_device_, &efml4a_dev_)) {                     \
            EFML4A_LOG_ONCE(EFML4A_LOGE,                                                     \
                            "command buffer %p has no known device (did "                     \
                            "vkAllocateCommandBuffers go through this layer?)",               \
                            reinterpret_cast<const void*>(commandBuffer));                    \
            return retval;                                                                   \
        }                                                                                    \
    } while (0)

// 简单的 SPIR-V 指纹（djb2），用于将来把某个 draw 关联回它是哪个 shader。
uint64_t HashSpirv(const uint32_t* code, size_t word_count) {
    uint64_t hash = 5381;
    if (code == nullptr) {
        return hash;
    }
    for (size_t i = 0; i < word_count; ++i) {
        hash = ((hash << 5) + hash) ^ static_cast<uint64_t>(code[i]);
    }
    return hash;
}

// ---------------------------------------------------------------------------
// 句柄 -> 整数
// ---------------------------------------------------------------------------
// VkPipeline / VkBuffer 这类非 dispatchable handle 在 64 位平台上是**指针**类型，
// 在 32 位平台上是 uint64_t。用 if constexpr 同时兼容两种定义，免得 32 位构建编不过。
template <typename T>
uint64_t HandleBits(T handle) {
    if constexpr (std::is_pointer<T>::value) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    } else {
        return static_cast<uint64_t>(handle);
    }
}

// ---------------------------------------------------------------------------
// recon：按 command buffer 分别记录「绘制时的绑定状态」
// ---------------------------------------------------------------------------
// 为什么按 VkCommandBuffer 分表：Vulkan 允许从多个线程并发向**不同**命令缓冲录制，
// 用一张全局状态表会把并发录制串成错误的组合。按句柄分表是唯一正确的做法。
//
// ⚠️ 未拦截 vkResetCommandBuffer / vkBeginCommandBuffer / vkResetCommandPool
//   （它们不在本层的 hook 白名单里，见 layer.cpp）。后果：应用复用命令缓冲时
//   这份状态会残留到下一次录制。实际影响很小 —— Vulkan 规定 vkBeginCommandBuffer
//   之后绑定状态是未定义的，所以任何合法绘制之前都必须重新 vkCmdBindPipeline，
//   于是 pipeline / ib / vb0 在真正被用到之前都会被覆盖。
//   真正必须清理的 vkFreeCommandBuffers 路径已处理，避免无界增长。
struct DrawState {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = static_cast<VkPipelineBindPoint>(-1);  // -1 = 还没绑过
    VkBuffer ib_buffer = VK_NULL_HANDLE;
    VkDeviceSize ib_offset = 0;
    VkIndexType ib_type = static_cast<VkIndexType>(-1);
    VkBuffer vb0_buffer = VK_NULL_HANDLE;
    VkDeviceSize vb0_offset = 0;
};

std::mutex g_recon_mutex;
std::unordered_map<VkCommandBuffer, DrawState> g_recon_states;
uint64_t g_recon_seq = 0;

void ForgetReconState(VkCommandBuffer cb) {
    std::lock_guard<std::mutex> lock(g_recon_mutex);
    g_recon_states.erase(cb);
}

void ForgetAllReconState() {
    std::lock_guard<std::mutex> lock(g_recon_mutex);
    g_recon_states.clear();
}

// 写一行 JSONL。字段名与 README「落盘侦察」一节逐字对应，不要改。
void EmitReconDraw(VkCommandBuffer cb, const char* call, uint32_t indexCount, uint32_t firstIndex,
                   int32_t vertexOffset, uint32_t instanceCount, uint32_t firstInstance) {
    DrawState st;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(g_recon_mutex);
        st = g_recon_states[cb];  // 没有记录时用默认值（表示「本次绘制前没绑过」）
        seq = g_recon_seq++;
    }

    char line[768];
    const int n = std::snprintf(
        line, sizeof(line),
        "{\"seq\":%llu,\"call\":\"%s\",\"indexCount\":%u,\"firstIndex\":%u,\"vertexOffset\":%d,"
        "\"instanceCount\":%u,\"firstInstance\":%u,\"pipeline\":\"0x%llx\",\"bindPoint\":%d,"
        "\"ib_buffer\":\"0x%llx\",\"ib_offset\":%llu,\"ib_type\":%d,"
        "\"vb0_buffer\":\"0x%llx\",\"vb0_offset\":%llu}\n",
        static_cast<unsigned long long>(seq), call, indexCount, firstIndex,
        static_cast<int>(vertexOffset), instanceCount, firstInstance,
        static_cast<unsigned long long>(HandleBits(st.pipeline)), static_cast<int>(st.bind_point),
        static_cast<unsigned long long>(HandleBits(st.ib_buffer)),
        static_cast<unsigned long long>(st.ib_offset), static_cast<int>(st.ib_type),
        static_cast<unsigned long long>(HandleBits(st.vb0_buffer)),
        static_cast<unsigned long long>(st.vb0_offset));
    if (n <= 0) {
        return;
    }
    const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n)
                                                            : sizeof(line) - 1;
    recon::AppendLine(std::string(line, len));
}

// ---------------------------------------------------------------------------
// skip：每个 submit 窗口内已出现的 draw 序号
// ---------------------------------------------------------------------------
// 为什么按 submit 窗口重置，而不是全进程只数一次：
//   debug.efml4a.skip 的用途是「肉眼确认层能控制渲染」。如果 K=1 表示
//   「整个进程只丢第一个 draw」，那在一帧几千个 draw 里根本看不出来。
//   按窗口（≈一帧）重置后，K=1 的效果是「每帧丢掉第 1 个 draw」，
//   视觉上会稳定地少一个部件 —— 这才是能立刻看到的证据。
//   ⚠️ 这是我的工程判断，不是任务书原文（原文只说「跳过前 K 次绘制」）。
std::atomic<uint32_t> g_window_draw_index{0};
std::atomic<uint64_t> g_skipped_draws{0};
std::atomic<uint64_t> g_donor_swaps{0};

// ---------------------------------------------------------------------------
// donor：VkBuffer 注册表（size / usage）
// ---------------------------------------------------------------------------
// 为什么要无条件登记（不按 mode 门控）：真机上模式常常是游戏跑起来之后才
// setprop 的。如果只在 donor 模式下登记，切过去时表里是空的，永远找不到 donor。
// vkCreateBuffer 不是逐帧调用（多数游戏在加载期创建缓冲），这个开销可以忽略。
struct BufferInfo {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    VkDevice device = VK_NULL_HANDLE;
};

std::mutex g_buffer_mutex;
std::unordered_map<VkBuffer, BufferInfo> g_buffer_map;
// 「最近见过的顶点缓冲」，最新在前。donor 从这里挑。
std::deque<VkBuffer> g_recent_vertex_buffers;
constexpr size_t kRecentVertexBufferCap = 64;
uint32_t g_donor_rotation = 0;  // 轮转游标：避免每次都换同一个 donor

void RememberBuffer(VkBuffer buffer, VkDevice device, const VkBufferCreateInfo* info) {
    if (buffer == VK_NULL_HANDLE || info == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_buffer_mutex);
    BufferInfo bi;
    bi.size = info->size;
    bi.usage = info->usage;
    bi.device = device;
    g_buffer_map[buffer] = bi;

    if ((info->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0 && info->size > 0) {
        for (auto it = g_recent_vertex_buffers.begin(); it != g_recent_vertex_buffers.end(); ++it) {
            if (*it == buffer) {  // 句柄可能被复用，先去重
                g_recent_vertex_buffers.erase(it);
                break;
            }
        }
        g_recent_vertex_buffers.push_front(buffer);
        while (g_recent_vertex_buffers.size() > kRecentVertexBufferCap) {
            g_recent_vertex_buffers.pop_back();
        }
    }
}

void ForgetBuffer(VkBuffer buffer) {
    std::lock_guard<std::mutex> lock(g_buffer_mutex);
    g_buffer_map.erase(buffer);
    for (auto it = g_recent_vertex_buffers.begin(); it != g_recent_vertex_buffers.end();) {
        if (*it == buffer) {
            it = g_recent_vertex_buffers.erase(it);
        } else {
            ++it;
        }
    }
}

// 设备销毁时清掉它的所有 buffer（句柄之后全部失效，留着只会误判）。
void ForgetBuffersForDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_buffer_mutex);
    for (auto it = g_buffer_map.begin(); it != g_buffer_map.end();) {
        if (it->second.device == device) {
            it = g_buffer_map.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_recent_vertex_buffers.begin(); it != g_recent_vertex_buffers.end();) {
        if (g_buffer_map.find(*it) == g_buffer_map.end()) {
            it = g_recent_vertex_buffers.erase(it);
        } else {
            ++it;
        }
    }
}

bool LookupBuffer(VkBuffer buffer, VkDeviceSize* size, bool* is_vertex_buffer) {
    std::lock_guard<std::mutex> lock(g_buffer_mutex);
    const auto it = g_buffer_map.find(buffer);
    if (it == g_buffer_map.end()) {
        return false;
    }
    if (size != nullptr) {
        *size = it->second.size;
    }
    if (is_vertex_buffer != nullptr) {
        *is_vertex_buffer = (it->second.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0;
    }
    return true;
}

// 选一个 donor。安全条件（缺一不可）：
//   1) donor != 目标 buffer（否则等于没换）
//   2) donor.size >= 目标 size —— 这是**唯一**保证「顶点读取范围不会越过缓冲末尾」
//      的条件：原绑定合法 => offset + 读取长度 <= 目标 size <= donor.size。
//   3) donor 自己必须是 VERTEX_BUFFER_BIT 缓冲 —— 把 uniform/index 缓冲当顶点
//      数据绑上去，GPU 读到的是垃圾，运气不好直接 device lost。
// 选择策略：「最近见过」的优先，但在候选集合里轮转，避免整屏都换成同一个缓冲
// （那样画出来的东西完全无法判断是哪个模型被换了）。
bool PickDonor(VkBuffer target, VkDeviceSize needed_size, VkBuffer* out) {
    std::lock_guard<std::mutex> lock(g_buffer_mutex);
    std::vector<VkBuffer> candidates;
    for (VkBuffer candidate : g_recent_vertex_buffers) {
        if (candidate == target) {
            continue;
        }
        const auto it = g_buffer_map.find(candidate);
        if (it == g_buffer_map.end()) {
            continue;
        }
        if (it->second.size < needed_size) {
            continue;
        }
        if ((it->second.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) == 0) {
            continue;
        }
        candidates.push_back(candidate);
    }
    if (candidates.empty()) {
        return false;
    }
    *out = candidates[g_donor_rotation % candidates.size()];
    ++g_donor_rotation;
    return true;
}

}  // namespace

// 供 EFML4A_GetStatsJson 用的诊断计数（内部链接不可见，所以放在 efml4a 命名空间里）。
uint64_t SkippedDrawCount() { return g_skipped_draws.load(std::memory_order_relaxed); }
uint64_t DonorSwapCount() { return g_donor_swaps.load(std::memory_order_relaxed); }

}  // namespace efml4a

// ============================================================================
// Android 「自省」入口实现（自报家门，不转发）
// ============================================================================
namespace efml4a {
namespace android_introspect {

VkResult EnumerateInstanceLayerProperties(uint32_t* pCount, VkLayerProperties* pProperties) {
    if (pCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    EFML4A_LOGD("android introspection: vkEnumerateInstanceLayerProperties(count=%u)",
                *pCount);

    if (pProperties == nullptr) {
        *pCount = 1;
        return VK_SUCCESS;
    }
    if (*pCount == 0) {
        return VK_INCOMPLETE;
    }

    std::memset(&pProperties[0], 0, sizeof(pProperties[0]));
    std::snprintf(pProperties[0].layerName, sizeof(pProperties[0].layerName), "%s",
                  EFML4A_LAYER_NAME);
    pProperties[0].specVersion = EFML4A_LAYER_SPEC_VERSION;
    pProperties[0].implementationVersion = EFML4A_LAYER_IMPLEMENTATION_VERSION;
    std::snprintf(pProperties[0].description, sizeof(pProperties[0].description), "%s",
                  EFML4A_LAYER_DESCRIPTION);

    *pCount = 1;
    return VK_SUCCESS;
}

VkResult EnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pCount,
                                              VkExtensionProperties* pProperties) {
    if (pCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Android loader 会用「本层名字」来问本层提供哪些实例扩展
    // （layers_extensions.cpp: enumerate_instance_extensions(props.layerName, ...)）。
    if (pLayerName == nullptr || std::strcmp(pLayerName, EFML4A_LAYER_NAME) != 0) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    // 本骨架不自带扩展。将来若要暴露 VK_EXT_debug_utils / VK_EXT_debug_marker
    // 之外的私有控制接口（例如从 Rust 侧下发 mod 指令），在这里返回。
    if (pProperties == nullptr) {
        *pCount = 0;
        return VK_SUCCESS;
    }
    *pCount = 0;
    return VK_SUCCESS;
}

}  // namespace android_introspect
}  // namespace efml4a

// ============================================================================
// 实例级拦截点
// ============================================================================
namespace efml4a {
namespace hooks {

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                             const VkAllocationCallbacks* pAllocator,
                                             VkInstance* pInstance) {
    Bump(kStatCreateInstance);

    const char* app_name = "<null>";
    if (pCreateInfo != nullptr && pCreateInfo->pApplicationInfo != nullptr &&
        pCreateInfo->pApplicationInfo->pApplicationName != nullptr) {
        app_name = pCreateInfo->pApplicationInfo->pApplicationName;
    }
    EFML4A_LOGI("vkCreateInstance intercepted (app=\"%s\")", app_name);

    if (pCreateInfo == nullptr || pInstance == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    InstanceLinkInfo link;
    if (!ExtractInstanceLinkInfo(pCreateInfo, &link) || link.next_gipa == nullptr) {
        // 走到这里说明本层不是被 loader 通过 VK_LAYER_LINK_INFO 挂上的，
        // 或者已经有人在链上把我们的位置弄坏了。
        EFML4A_LOGE("vkCreateInstance: no VK_LAYER_LINK_INFO found in pNext chain");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // 记下来，供 vkEnumerateInstanceLayerProperties 等「无 instance」命令尽力转发。
    SetPreInstanceGetInstanceProcAddr(link.next_gipa);

    auto next_create = reinterpret_cast<PFN_vkCreateInstance>(
        link.next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (next_create == nullptr) {
        EFML4A_LOGE("vkCreateInstance: next layer did not provide vkCreateInstance");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkResult result = next_create(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        EFML4A_LOGE("vkCreateInstance: down-chain call failed (VkResult=%d)",
                    static_cast<int>(result));
        return result;
    }

    InstanceData data;
    data.dispatch.GetPhysicalDeviceProcAddr = link.next_gpdpa;
    data.set_instance_loader_data = link.set_instance_loader_data;
    data.layer_create_device = link.layer_create_device;
    data.layer_destroy_device = link.layer_destroy_device;
    data.has_device_create_callback = link.has_device_create_callback;

    // 关键一步：instance 已经真实存在，现在才能用它取到**实例级**命令。
    FillInstanceDispatchTable(*pInstance, link.next_gipa, &data.dispatch);
    RegisterInstance(*pInstance, data);

    EFML4A_LOGI("instance %p registered (gipa=%p, gpdpa=%p)", reinterpret_cast<void*>(*pInstance),
                reinterpret_cast<const void*>(link.next_gipa),
                reinterpret_cast<const void*>(link.next_gpdpa));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance,
                                          const VkAllocationCallbacks* pAllocator) {
    InstanceData data;
    const bool known = GetInstanceData(instance, &data);
    UnregisterInstance(instance);
    ForgetPhysicalDevices(instance);

    if (known && data.dispatch.DestroyInstance != nullptr) {
        data.dispatch.DestroyInstance(instance, pAllocator);
    } else {
        EFML4A_LOG_ONCE(EFML4A_LOGW, "vkDestroyInstance for unknown instance %p",
                        reinterpret_cast<void*>(instance));
    }
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                                               VkLayerProperties* pProperties) {
    Bump(kStatEnumerateInstanceLayerProperties);

    // 见上面对「无 instance 全局命令」的说明：优先转发，没有下一层指针时自报家门。
    PFN_vkGetInstanceProcAddr gipa = GetPreInstanceGetInstanceProcAddr();
    if (gipa != nullptr) {
        auto next = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
            gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
        if (next != nullptr) {
            return next(pPropertyCount, pProperties);
        }
    }

    EFML4A_LOG_ONCE(EFML4A_LOGW,
                    "vkEnumerateInstanceLayerProperties: no down-chain GIPA yet, "
                    "falling back to self-report");
    return android_introspect::EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(
    const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    Bump(kStatEnumerateInstanceExtensionProperties);

    // 应用显式询问本层提供了哪些扩展 -> 自报家门，不转发。
    if (pLayerName != nullptr) {
        return android_introspect::EnumerateInstanceExtensionProperties(pLayerName, pPropertyCount,
                                                                       pProperties);
    }

    PFN_vkGetInstanceProcAddr gipa = GetPreInstanceGetInstanceProcAddr();
    if (gipa != nullptr) {
        auto next = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
        if (next != nullptr) {
            return next(pLayerName, pPropertyCount, pProperties);
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance,
                                                       uint32_t* pPhysicalDeviceCount,
                                                       VkPhysicalDevice* pPhysicalDevices) {
    InstanceData data;
    if (!GetInstanceData(instance, &data) || data.dispatch.EnumeratePhysicalDevices == nullptr) {
        EFML4A_LOG_ONCE(EFML4A_LOGE, "vkEnumeratePhysicalDevices: instance %p not registered",
                        reinterpret_cast<void*>(instance));
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkResult result =
        data.dispatch.EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
        // 登记 physicalDevice -> instance，供 vkCreateDevice 反查。
        if (pPhysicalDevices != nullptr && pPhysicalDeviceCount != nullptr) {
            for (uint32_t i = 0; i < *pPhysicalDeviceCount; ++i) {
                RememberPhysicalDevice(pPhysicalDevices[i], instance);
            }
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice,
                                           const VkDeviceCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator,
                                           VkDevice* pDevice) {
    Bump(kStatCreateDevice);
    EFML4A_LOGI("vkCreateDevice intercepted (physicalDevice=%p)",
                reinterpret_cast<void*>(physicalDevice));

    if (pCreateInfo == nullptr || pDevice == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    DeviceLinkInfo link;
    if (!ExtractDeviceLinkInfo(pCreateInfo, &link) || link.next_gdpa == nullptr) {
        EFML4A_LOGE("vkCreateDevice: no VK_LAYER_LINK_INFO found in pNext chain");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkInstance instance = InstanceForPhysicalDevice(physicalDevice);

    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetDeviceProcAddr effective_gdpa = link.next_gdpa;

    // 路径 A：loader 通过 VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK 要求由层创建设备。
    // 这条路径下 loader 会替我们正确处理 pNext 链、并把真正的 nextGDPA 回填。
    if (instance != VK_NULL_HANDLE) {
        InstanceData inst_data;
        if (GetInstanceData(instance, &inst_data) && inst_data.has_device_create_callback &&
            inst_data.layer_create_device != nullptr) {
            PFN_vkGetDeviceProcAddr returned_gdpa = nullptr;
            result = inst_data.layer_create_device(
                instance, physicalDevice, pCreateInfo, pAllocator, pDevice,
                inst_data.dispatch.GetInstanceProcAddr, &returned_gdpa);
            if (returned_gdpa != nullptr) {
                effective_gdpa = returned_gdpa;
            }
            if (result != VK_SUCCESS) {
                EFML4A_LOGE("vkCreateDevice: pfnLayerCreateDevice failed (VkResult=%d)",
                            static_cast<int>(result));
                return result;
            }
            // 注意：这条路径下 pCreateInfo 的链已经由 loader 处理过了。
            DeviceData data;
            data.instance = instance;
            data.physical_device = physicalDevice;
            FillDeviceDispatchTable(*pDevice, effective_gdpa, &data.dispatch);
            RegisterDevice(*pDevice, data);
            return VK_SUCCESS;
        }
    }

    // 路径 B（常规）：自己用下一层的 GDPA 转发。
    auto next_create = reinterpret_cast<PFN_vkCreateDevice>(
        link.next_gipa != nullptr ? link.next_gipa(VK_NULL_HANDLE, "vkCreateDevice") : nullptr);
    if (next_create == nullptr) {
        EFML4A_LOGE("vkCreateDevice: next layer did not provide vkCreateDevice");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    result = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        EFML4A_LOGE("vkCreateDevice: down-chain call failed (VkResult=%d)",
                    static_cast<int>(result));
        return result;
    }

    DeviceData data;
    data.instance = instance;
    data.physical_device = physicalDevice;
    FillDeviceDispatchTable(*pDevice, link.next_gdpa, &data.dispatch);
    RegisterDevice(*pDevice, data);

    EFML4A_LOGI("device %p registered (instance=%p)", reinterpret_cast<void*>(*pDevice),
                reinterpret_cast<void*>(instance));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device,
                                        const VkAllocationCallbacks* pAllocator) {
    DeviceData data;
    const bool known = GetDeviceData(device, &data);
    UnregisterDevice(device);
    ForgetQueues(device);
    ForgetAllCommandBuffers(device);
    // 设备没了，所有 buffer / 命令缓冲句柄都失效，留着只会污染 donor 选择。
    ForgetBuffersForDevice(device);
    ForgetAllReconState();

    // 需求：vkDestroyDevice 时把最终统计覆盖写一次（游戏退出/重建 device 时能拿到最后一份计数）。
    recon::WriteStats(EFML4A_GetStatsJson());

    if (known && data.dispatch.DestroyDevice != nullptr) {
        data.dispatch.DestroyDevice(device, pAllocator);
    } else {
        EFML4A_LOG_ONCE(EFML4A_LOGW, "vkDestroyDevice for unknown device %p",
                        reinterpret_cast<void*>(device));
    }
}

}  // namespace hooks
}  // namespace efml4a

// ============================================================================
// 设备级拦截点
// ============================================================================
namespace efml4a {
namespace hooks {

VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device,
                                                 const VkShaderModuleCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkShaderModule* pShaderModule) {
    Bump(kStatCreateShaderModule);
    EFML4A_DEVICE_TABLE_OR_RETURN(device, VK_ERROR_INITIALIZATION_FAILED)

    // SPIR-V 指纹：将来要靠它判断「这个 shader 是不是我们要换掉的模型 shader」。
    // 3DMigoto 在 D3D11 里靠 shader hash 定位 draw call，这里对应地把
    // vkCreateShaderModule 得到的 SPIR-V 做指纹。
    if (pCreateInfo != nullptr && pCreateInfo->pCode != nullptr) {
        const uint64_t hash =
            HashSpirv(pCreateInfo->pCode, pCreateInfo->codeSize / sizeof(uint32_t));
        EFML4A_LOGD("vkCreateShaderModule: codeSize=%zu hash=0x%016llx",
                    static_cast<size_t>(pCreateInfo->codeSize),
                    static_cast<unsigned long long>(hash));
        // TODO(efml4a): 把 hash -> VkShaderModule 存进 shader registry，
        //               供 pipeline 创建/绘制时匹配 mod 目标。
    }

    return efml4a_dev_.dispatch.CreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) {
    Bump(kStatCreateGraphicsPipelines);
    EFML4A_DEVICE_TABLE_OR_RETURN(device, VK_ERROR_INITIALIZATION_FAILED)

    EFML4A_LOGD("vkCreateGraphicsPipelines: count=%u", createInfoCount);
    // TODO(efml4a): 在这里读 pCreateInfos[i].pStages 里的 VkShaderModule，
    //               与 shader registry 做匹配，记录「哪些 pipeline 是 mod 目标」。
    //               换模型的关键是拿到 pipeline 的顶点输入布局
    //               （pVertexInputState）与绑定的 VkBuffer，从而在绘制时替换数据。

    return efml4a_dev_.dispatch.CreateGraphicsPipelines(device, pipelineCache, createInfoCount,
                                                       pCreateInfos, pAllocator, pPipelines);
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer commandBuffer,
                                          VkPipelineBindPoint pipelineBindPoint,
                                          VkPipeline pipeline) {
    Bump(kStatCmdBindPipeline);
    EFML4A_CMDBUF_TABLE_OR_RETURN(commandBuffer, );

    // 侦察：只读记录，不碰任何参数。
    if (recon::Enabled()) {
        std::lock_guard<std::mutex> lock(g_recon_mutex);
        DrawState& st = g_recon_states[commandBuffer];
        st.pipeline = pipeline;
        st.bind_point = pipelineBindPoint;
    }

    // TODO(efml4a): 将来换模型时，这里也要把「目标 pipeline」换成我们自己的 pipeline。
    efml4a_dev_.dispatch.CmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                                               uint32_t firstBinding, uint32_t bindingCount,
                                               const VkBuffer* pBuffers,
                                               const VkDeviceSize* pOffsets) {
    Bump(kStatCmdBindVertexBuffers);
    EFML4A_CMDBUF_TABLE_OR_RETURN(commandBuffer, );

    // 侦察：只读记录 binding 0。
    // 只认 firstBinding==0：语义上「binding 0」就是 firstBinding==0 且 bindingCount>0
    // 时数组里的第 0 项；一次绑多个时取第一个。
    if (recon::Enabled() && firstBinding == 0 && bindingCount > 0 && pBuffers != nullptr) {
        std::lock_guard<std::mutex> lock(g_recon_mutex);
        DrawState& st = g_recon_states[commandBuffer];
        st.vb0_buffer = pBuffers[0];
        st.vb0_offset = (pOffsets != nullptr) ? pOffsets[0] : 0;
    }

    // ---- donor 模式：把 binding 0 换成另一个「足够大」的顶点缓冲 --------------
    const VkBuffer* buffers = pBuffers;
    VkBuffer stack_buffers[16];
    std::vector<VkBuffer> heap_buffers;
    if (mode::Current() == mode::kDonor && firstBinding == 0 && bindingCount > 0 &&
        pBuffers != nullptr && pBuffers[0] != VK_NULL_HANDLE) {
        VkDeviceSize target_size = 0;
        bool target_is_vertex = false;
        VkBuffer donor = VK_NULL_HANDLE;
        if (LookupBuffer(pBuffers[0], &target_size, &target_is_vertex) && target_is_vertex &&
            target_size > 0 && g_donor_swaps.load(std::memory_order_relaxed) < mode::DonorMax() &&
            PickDonor(pBuffers[0], target_size, &donor)) {
            // 绝不修改调用方的数组：pBuffers 可能指向应用自己的栈/常量内存，
            // 往里写属于未定义行为（而且会连带污染同一帧后续的绘制）。
            if (bindingCount <= 16) {
                std::memcpy(stack_buffers, pBuffers, sizeof(VkBuffer) * bindingCount);
                stack_buffers[0] = donor;
                buffers = stack_buffers;
            } else {
                heap_buffers.assign(pBuffers, pBuffers + bindingCount);
                heap_buffers[0] = donor;
                buffers = heap_buffers.data();
            }
            // ⚠️ 未核实：这里的上限判断与 fetch_add 不是原子的，多线程并发录制时
            //   实际替换次数可能略微超过 donor_max（超出量 = 并发线程数）。
            //   上限本身只是「别把整帧搞崩」的保险，不要求严格精确。
            g_donor_swaps.fetch_add(1, std::memory_order_relaxed);
            EFML4A_LOG_ONCE(EFML4A_LOGI,
                            "donor mode: binding 0 swapped (target=%p -> donor=%p); "
                            "up to donor_max=%u swaps",
                            reinterpret_cast<void*>(pBuffers[0]), reinterpret_cast<void*>(donor),
                            mode::DonorMax());
        }
    }

    efml4a_dev_.dispatch.CmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, buffers,
                                             pOffsets);
}

VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                             VkDeviceSize offset, VkIndexType indexType) {
    Bump(kStatCmdBindIndexBuffer);
    EFML4A_CMDBUF_TABLE_OR_RETURN(commandBuffer, );

    // 侦察：只读记录。
    if (recon::Enabled()) {
        std::lock_guard<std::mutex> lock(g_recon_mutex);
        DrawState& st = g_recon_states[commandBuffer];
        st.ib_buffer = buffer;
        st.ib_offset = offset;
        st.ib_type = indexType;
    }

    // TODO(efml4a): 同样需要替换。索引缓冲决定三角形数量，换模型时通常
    //               索引与顶点一起换。
    efml4a_dev_.dispatch.CmdBindIndexBuffer(commandBuffer, buffer, offset, indexType);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
                                         uint32_t instanceCount, uint32_t firstIndex,
                                         int32_t vertexOffset, uint32_t firstInstance) {
    Bump(kStatCmdDrawIndexed);
    EFML4A_CMDBUF_TABLE_OR_RETURN(commandBuffer, );

    // TODO(efml4a): 这里是「识别目标 draw call」的最佳位置。
    //               3DMigoto 用 (shader hash, indexCount, firstIndex) 作为签名来
    //               匹配要替换的绘制；换成 Vulkan 后还需要加上当前绑定的 pipeline。
    EFML4A_LOG_ONCE(EFML4A_LOGD, "first vkCmdDrawIndexed seen (indexCount=%u)", indexCount);

    if (recon::Enabled()) {
        EmitReconDraw(commandBuffer, "drawIndexed", indexCount, firstIndex, vertexOffset,
                      instanceCount, firstInstance);
    }

    // ---- skip 模式：直接丢弃这一笔绘制 ---------------------------------------
    // 不转发 = 这个 draw 不会进入命令缓冲。这是**零崩溃风险**的行为改变：
    // 我们只是少下发一条无害的命令，不碰任何资源/同步，GPU 侧不会有非法访问。
    if (mode::Current() == mode::kSkip) {
        const uint32_t k = mode::SkipDraws();
        const uint32_t index_in_window = g_window_draw_index.fetch_add(1, std::memory_order_relaxed);
        if (k != 0 && index_in_window < k) {
            g_skipped_draws.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    efml4a_dev_.dispatch.CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex,
                                       vertexOffset, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
                                  uint32_t instanceCount, uint32_t firstVertex,
                                  uint32_t firstInstance) {
    Bump(kStatCmdDraw);
    EFML4A_CMDBUF_TABLE_OR_RETURN(commandBuffer, );

    if (recon::Enabled()) {
        // vkCmdDraw 没有索引概念：按需求把 vertexCount/firstVertex 填进
        // indexCount/firstIndex 两个字段，vertexOffset 记 0。
        EmitReconDraw(commandBuffer, "draw", vertexCount, firstVertex, 0, instanceCount,
                      firstInstance);
    }

    if (mode::Current() == mode::kSkip) {
        const uint32_t k = mode::SkipDraws();
        const uint32_t index_in_window = g_window_draw_index.fetch_add(1, std::memory_order_relaxed);
        if (k != 0 && index_in_window < k) {
            g_skipped_draws.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // 非索引绘制。EFML4A 的目标游戏通常用索引绘制，但保留计数与转发。
    efml4a_dev_.dispatch.CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex,
                                firstInstance);
}

VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer commandBuffer,
                                           VkPipelineLayout layout, VkShaderStageFlags stageFlags,
                                           uint32_t offset, uint32_t size, const void* pValues) {
    Bump(kStatCmdPushConstants);
    EFML4A_CMDBUF_TABLE_OR_RETURN(commandBuffer, );

    // TODO(efml4a): push constant 里常常藏着 bone matrix / 变换矩阵偏移，
    //               识别目标角色时会用到。
    efml4a_dev_.dispatch.CmdPushConstants(commandBuffer, layout, stageFlags, offset, size,
                                        pValues);
}

VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                                                VkPipelineBindPoint pipelineBindPoint,
                                                VkPipelineLayout layout, uint32_t firstSet,
                                                uint32_t descriptorSetCount,
                                                const VkDescriptorSet* pDescriptorSets,
                                                uint32_t dynamicOffsetCount,
                                                const uint32_t* pDynamicOffsets) {
    Bump(kStatCmdBindDescriptorSets);
    EFML4A_CMDBUF_TABLE_OR_RETURN(commandBuffer, );

    // TODO(efml4a): 换贴图要走这里（替换 VkImageView/VkSampler 对应的 descriptor）。
    //               与换模型（顶点/索引缓冲）是两条独立的替换路径。
    efml4a_dev_.dispatch.CmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, firstSet,
                                              descriptorSetCount, pDescriptorSets,
                                              dynamicOffsetCount, pDynamicOffsets);
}


VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers) {
    EFML4A_DEVICE_TABLE_OR_RETURN(device, VK_ERROR_INITIALIZATION_FAILED)

    const VkResult result =
        efml4a_dev_.dispatch.AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
    if (result == VK_SUCCESS && pAllocateInfo != nullptr) {
        // 登记 commandBuffer -> device，供后面所有 Cmd* 拦截点反查 dispatch table。
        RememberCommandBuffers(device, pAllocateInfo->commandBufferCount, pCommandBuffers);
        EFML4A_LOGD("allocated %u command buffer(s) for device %p", pAllocateInfo->commandBufferCount,
                    reinterpret_cast<void*>(device));
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                             uint32_t commandBufferCount,
                                             const VkCommandBuffer* pCommandBuffers) {
    EFML4A_DEVICE_TABLE_OR_RETURN(device, )

    efml4a_dev_.dispatch.FreeCommandBuffers(device, commandPool, commandBufferCount,
                                          pCommandBuffers);
    ForgetCommandBuffers(device, commandBufferCount, pCommandBuffers);

    // recon 状态表按命令缓冲句柄索引，必须跟着释放一起清，否则会无界增长
    // （有些引擎每帧新建/销毁命令缓冲）。
    // 这里**不**判断 recon::Enabled()：模式可能在运行中被关掉，
    // 而那时表里已经积累的条目仍然需要被清掉。
    if (pCommandBuffers != nullptr) {
        for (uint32_t i = 0; i < commandBufferCount; ++i) {
            ForgetReconState(pCommandBuffers[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// vkCreateBuffer / vkDestroyBuffer —— donor 模式的「弹药库」
// ---------------------------------------------------------------------------
// 为什么现在才加：donor 需要知道每个 VkBuffer 的 size（安全条件）与 usage
// （只允许用顶点缓冲当 donor）。这两个 hook **不计入 14 项统计**，
// 以保持 EFML4A_GetStatsJson() 的 counters 集合不变（README 已固化该集合）。
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device,
                                           const VkBufferCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator,
                                           VkBuffer* pBuffer) {
    EFML4A_DEVICE_TABLE_OR_RETURN(device, VK_ERROR_INITIALIZATION_FAILED)

    const VkResult result =
        efml4a_dev_.dispatch.CreateBuffer(device, pCreateInfo, pAllocator, pBuffer);

    // 无条件登记（不按 mode 门控）：真机上模式常常是游戏跑起来之后才 setprop 的，
    // 只在 donor 模式下登记会导致切过去时表是空的、永远找不到 donor。
    if (result == VK_SUCCESS && pBuffer != nullptr && *pBuffer != VK_NULL_HANDLE) {
        RememberBuffer(*pBuffer, device, pCreateInfo);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer,
                                        const VkAllocationCallbacks* pAllocator) {
    EFML4A_DEVICE_TABLE_OR_RETURN(device, )

    efml4a_dev_.dispatch.DestroyBuffer(device, buffer, pAllocator);
    // 清理注册表，避免句柄复用后把旧 size/usage 当成新的（那会导致越界替换）。
    ForgetBuffer(buffer);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                         uint32_t queueIndex, VkQueue* pQueue) {
    EFML4A_DEVICE_TABLE_OR_RETURN(device, )

    efml4a_dev_.dispatch.GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);

    // 登记 queue -> device，供 vkQueueSubmit 反查 dispatch table。
    if (pQueue != nullptr && *pQueue != VK_NULL_HANDLE) {
        RememberQueue(*pQueue, device);
        EFML4A_LOGD("queue %p -> device %p", reinterpret_cast<void*>(*pQueue),
                    reinterpret_cast<void*>(device));
    }
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t submitCount,
                                          const VkSubmitInfo* pSubmits, VkFence fence) {
    Bump(kStatQueueSubmit);

    // 每次提交刷新一次运行模式。
    // 为什么放在这里而不是逐 draw 读：setprop 改 debug.efml4a.mode 后，效果会在
    // 1 帧内生效（不用重启游戏，这在真机排障时非常关键），而 draw 热路径上
    // 只需要一次原子读，不需要每次读属性。
    mode::Refresh();

    // 重置 skip 的窗口计数（窗口 = 相邻两次 vkQueueSubmit 之间录制的 draw ≈ 一帧）。
    g_window_draw_index.store(0, std::memory_order_relaxed);

    // 每 600 次提交覆盖写一次统计文件（游戏不退出也能读到计数）。
    const uint64_t submit_count = GetStat(kStatQueueSubmit);
    if (submit_count != 0 && (submit_count % 600) == 0) {
        recon::WriteStats(EFML4A_GetStatsJson());
    }

    // dispatch table 是按 VkDevice 索引的，而这里只有 VkQueue，
    // 所以要先通过 vkGetDeviceQueue 建立的映射反查。
    const VkDevice device = DeviceForQueue(queue);
    DeviceData data;
    if (device == VK_NULL_HANDLE || !GetDeviceData(device, &data) ||
        data.dispatch.QueueSubmit == nullptr) {
        EFML4A_LOG_ONCE(EFML4A_LOGE,
                        "vkQueueSubmit: queue %p has no known device (did "
                        "vkGetDeviceQueue go through this layer?)",
                        reinterpret_cast<void*>(queue));
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    EFML4A_LOGD("vkQueueSubmit: submitCount=%u", submitCount);
    return data.dispatch.QueueSubmit(queue, submitCount, pSubmits, fence);
}

}  // namespace hooks
}  // namespace efml4a

// ============================================================================
// EFML4A_GetStatsJson —— 计数以 JSON 字符串导出
// ============================================================================
EFML4A_LAYER_EXPORT const char* EFML4A_GetStatsJson() {
    // thread_local 静态缓冲区：返回的指针在下次同一线程调用前有效，调用方不要 free。
    static thread_local std::string buffer;

    buffer.clear();
    buffer.reserve(1024);
    buffer += "{\"layer\":\"";
    buffer += EFML4A_LAYER_NAME;
    // 需求：每 600 次 vkQueueSubmit 落盘统计时，把当前 mode 也写进去。
    buffer += "\",\"mode\":\"";
    buffer += efml4a::mode::Name(efml4a::mode::Current());
    buffer += "\",\"counters\":{";

    char tmp[96];
    for (int i = 0; i < efml4a::kStatCount; ++i) {
        if (i != 0) {
            buffer += ',';
        }
        std::snprintf(tmp, sizeof(tmp), "\"%s\":%llu", efml4a::StatName(static_cast<efml4a::StatId>(i)),
                      static_cast<unsigned long long>(efml4a::GetStat(static_cast<efml4a::StatId>(i))));
        buffer += tmp;
    }

    buffer += '}';

    // 诊断字段：真机上不一定能及时看 logcat，所以把「模式到底起作用没有」的证据
    // 也写进同一份文件里。注意 counters 集合本身**没有**改动。
    char extra[192];
    std::snprintf(extra, sizeof(extra),
                  ",\"skip_draws\":%llu,\"donor_swaps\":%llu,\"recon_lines\":%llu}",
                  static_cast<unsigned long long>(efml4a::SkippedDrawCount()),
                  static_cast<unsigned long long>(efml4a::DonorSwapCount()),
                  static_cast<unsigned long long>(efml4a::recon::LineCount()));
    buffer += extra;
    return buffer.c_str();
}

// ============================================================================
// TODO(efml4a): 真正的「换模型」设计草案（尚未实现）
// ============================================================================
// 3DMigoto 在 D3D11 上换模型的原理是：拦 ID3D11Device::CreateBuffer 拿到
// 顶点/索引缓冲的影子副本，再在 DrawIndexed 时用替换后的缓冲重新绑定。
// 对应到 Vulkan，缺少的环节如下 —— 请按顺序补齐：
//
// (1) 影子对象注册表（shadow registry）
//     需要新增拦截 vkCreateBuffer / vkAllocateMemory / vkBindBufferMemory /
//     vkGetBufferMemoryRequirements / vkMapMemory / vkUnmapMemory，
//     维护两张表：
//         std::unordered_map<VkBuffer, BufferInfo>
//         std::unordered_map<VkDeviceMemory, MemoryInfo>
//     其中 BufferInfo = { VkDeviceSize size, VkBufferUsageFlags usage,
//                         VkDeviceMemory memory, VkDeviceSize memory_offset,
//                         void* mapped, VkMemoryPropertyFlags mem_flags };
//     ⚠️ 这几个入口**目前不在** dispatch table 里（见 dispatch.h「按需扩展」），
//        需要在 dispatch.h + dispatch.cpp 各加一行，再在 hooks.cpp 实现。
//     注意：只有 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 的内存才能直接读改；
//     device-local 的缓冲需要 staged copy（建 staging buffer + 记命令缓冲拷贝），
//     这是与 D3D11 最大的不同点：D3D11 的 UpdateSubresource 是隐式的，
//     Vulkan 需要你自己管同步（barrier / semaphore / fence）。
//
// (2) 绘制状态机
//     现在各个 Cmd* hook 是「无状态」的（只计数+转发）。要识别目标 draw，
//     必须按 VkCommandBuffer 记录一份状态：
//         std::unordered_map<VkCommandBuffer, CmdState>
//     CmdState = { VkPipeline pipeline;
//                  std::array<VkBuffer, kMaxVertexBindings> vertex_buffers;
//                  std::array<VkDeviceSize, kMaxVertexBindings> vertex_offsets;
//                  VkBuffer index_buffer; VkDeviceSize index_offset;
//                  VkIndexType index_type;
//                  VkPipelineLayout layout;      // 用于关联 push constants
//                  std::vector<uint8_t> last_push_constants; };
//     ⚠️ 必须同时拦截 vkBeginCommandBuffer / vkEndCommandBuffer / vkResetCommandBuffer /
//        vkFreeCommandBuffers 来清理这张表，否则会随命令缓冲复用而错乱。
//
// (3) draw 签名匹配
//     3DMigoto 用 (shader hash, indexCount, firstIndex) 定位 draw。Vulkan 下建议：
//         signature = ( pipeline_spirv_hash, indexCount, firstIndex, vertexOffset,
//                       instanceCount, vertex_input_binding_count )
//     其中 pipeline_spirv_hash 来自 (1)(2) 的追踪：vkCreateShaderModule 记 hash，
//     vkCreateGraphicsPipelines 把 pipeline -> shader hash 关联起来。
//     EFML4A 的 mod 描述文件（对应 3DMigoto 的 .ini）就按这个签名匹配，
//     匹配成功后用 (1) 的替换缓冲 + 修改后的 indexCount/firstIndex 重新下发
//     vkCmdBindVertexBuffers / vkCmdBindIndexBuffer / vkCmdDrawIndexed。
//
// (4) 描述符/贴图替换（换贴图，独立于换模型）
//     拦 vkCreateImageView / vkCreateSampler / vkUpdateDescriptorSets，
//     在 vkCmdBindDescriptorSets 里把目标 VkDescriptorSet 换成我们自己的。
//
// (5) 线程与设备丢失
//     上面所有表都要处理多线程（vkCmd* 可从多个线程并发录制到不同 command buffer，
//     但同一 command buffer 不并发）+ VK_ERROR_DEVICE_LOST 后的清理。
// ============================================================================
