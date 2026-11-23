// ============================================================================
// EFML4A host 侧冒烟测试
//
// 目标（必须真实跑通）：证明本层「能编译 / 能装载 / 能协商 / 能拦截 / 能转发」。
//
//  1. dlopen() 打开编译出来的层 .so；
//  2. dlsym() 取出 vkNegotiateLoaderLayerInterfaceVersion；
//  3. 构造 VkNegotiateLayerInterface 调用它，断言：
//       - 返回 VK_SUCCESS
//       - pVersionStruct->pfnGetInstanceProcAddr != nullptr
//       - 版本回落、sType 校验、过低版本拒绝等协议行为正确
//  4. 用一个**自制的假 loader**（stub GIPA/GDPA + stub vkCreateInstance）
//     手工搭出 loader 会传的那条 pNext 链，真正调用本层的 vkCreateInstance，
//     从而验证：
//       - VK_LAYER_LINK_INFO 的解析与链表推进
//       - instance 注册与 dispatch table 填充
//       - 「拦截」（hook 被返回）与「转发」（未拦截的名字落到下一层哨兵函数）
//  5. 验证 Android 专有的自省入口（vkEnumerateInstanceLayerProperties 等）。
//  6. 验证 EFML4A_GetStatsJson() 确实记录了计数。
//
// 用法：efml4a_smoke_host <path-to-libVkLayer_EFML4A.so>
// ============================================================================

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        std::printf("  ok   : %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAILED: %s\n", what);
    }
}

// ---------------------------------------------------------------------------
// 假 loader
// ---------------------------------------------------------------------------
// 哨兵函数：只要调用转发到了下一层，我们就会拿回这个指针，从而可以区分
// 「本层拦截了」和「本层转发给了下一层」。
VKAPI_ATTR void VKAPI_CALL SentinelForwarded() {}

// 假的下层 dispatch table 里放的占位函数（本测试不会真的调用它们）。
VKAPI_ATTR VkResult VKAPI_CALL StubCreateInstance(const VkInstanceCreateInfo*,
                                                 const VkAllocationCallbacks*, VkInstance* pInstance) {
    // 造一个假的 VkInstance 句柄。VkInstance 是 non-dispatchable handle，
    // 在 64 位平台上就是指针类型，所以可以直接塞一个非空地址。
    *pInstance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(0xEF4A0000));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL StubDestroyInstance(VkInstance, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL StubEnumerateLayerProps(uint32_t* pCount,
                                                      VkLayerProperties* pProps) {
    if (pProps == nullptr) {
        *pCount = 0;
        return VK_SUCCESS;
    }
    *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL StubEnumerateExtProps(const char*, uint32_t* pCount,
                                                    VkExtensionProperties*) {
    *pCount = 0;
    return VK_SUCCESS;
}

int g_stub_gipa_calls = 0;

PFN_vkVoidFunction VKAPI_CALL StubGetPhysicalDeviceProcAddr(VkInstance, const char*);
PFN_vkVoidFunction VKAPI_CALL StubGetDeviceProcAddr(VkDevice, const char*);

// 假的 vkGetInstanceProcAddr：只认识少数几个名字，其余一律返回哨兵。
PFN_vkVoidFunction VKAPI_CALL StubGetInstanceProcAddr(VkInstance, const char* pName) {
    ++g_stub_gipa_calls;
    if (pName == nullptr) {
        return nullptr;
    }
    if (std::strcmp(pName, "vkCreateInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&StubCreateInstance);
    }
    if (std::strcmp(pName, "vkDestroyInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&StubDestroyInstance);
    }
    if (std::strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&StubEnumerateLayerProps);
    }
    if (std::strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&StubEnumerateExtProps);
    }
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&StubGetInstanceProcAddr);
    }
    if (std::strcmp(pName, "vkGetPhysicalDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&StubGetPhysicalDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&StubGetDeviceProcAddr);
    }
    return reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded);
}

PFN_vkVoidFunction VKAPI_CALL StubGetPhysicalDeviceProcAddr(VkInstance, const char*) {
    return reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded);
}

PFN_vkVoidFunction VKAPI_CALL StubGetDeviceProcAddr(VkDevice, const char*) {
    return reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded);
}

template <typename T>
T LoadSymbol(void* handle, const char* name) {
    return reinterpret_cast<T>(dlsym(handle, name));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-libVkLayer_EFML4A.so>\n", argv[0]);
        return 2;
    }
    const char* layer_path = argv[1];

    std::printf("== EFML4A host smoke test ==\n");
    std::printf("layer .so : %s\n\n", layer_path);

    // -----------------------------------------------------------------------
    std::printf("[1] dlopen\n");
    void* handle = dlopen(layer_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        std::printf("  FAILED: dlopen: %s\n", dlerror());
        return 1;
    }
    Check(true, "dlopen() succeeded");

    // -----------------------------------------------------------------------
    std::printf("\n[2] dlsym 必需导出符号\n");
    auto negotiate = LoadSymbol<PFN_vkNegotiateLoaderLayerInterfaceVersion>(
        handle, "vkNegotiateLoaderLayerInterfaceVersion");
    Check(negotiate != nullptr, "vkNegotiateLoaderLayerInterfaceVersion exported");

    auto gipa_export =
        LoadSymbol<PFN_vkGetInstanceProcAddr>(handle, "vkGetInstanceProcAddr");
    Check(gipa_export != nullptr, "vkGetInstanceProcAddr exported (Android loader 需要)");

    auto gdpa_export = LoadSymbol<PFN_vkGetDeviceProcAddr>(handle, "vkGetDeviceProcAddr");
    Check(gdpa_export != nullptr, "vkGetDeviceProcAddr exported (Android loader 需要)");

    auto efml4a_gipa =
        LoadSymbol<PFN_vkGetInstanceProcAddr>(handle, "EFML4A_GetInstanceProcAddr");
    Check(efml4a_gipa != nullptr, "EFML4A_GetInstanceProcAddr exported");
    auto efml4a_gpdpa =
        LoadSymbol<PFN_GetPhysicalDeviceProcAddr>(handle, "EFML4A_GetPhysicalDeviceProcAddr");
    Check(efml4a_gpdpa != nullptr, "EFML4A_GetPhysicalDeviceProcAddr exported");

    auto get_stats = LoadSymbol<const char* (*)()>(handle, "EFML4A_GetStatsJson");
    Check(get_stats != nullptr, "EFML4A_GetStatsJson exported");

    if (g_failures != 0) {
        std::printf("\nFAILED: 必需符号缺失，后续测试无法进行\n");
        dlclose(handle);
        return 1;
    }

    // -----------------------------------------------------------------------
    std::printf("\n[3] 版本协商（vkNegotiateLoaderLayerInterfaceVersion）\n");
    {
        VkNegotiateLayerInterface vi{};
        vi.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        vi.pNext = nullptr;
        vi.loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

        const VkResult r = negotiate(&vi);
        Check(r == VK_SUCCESS, "返回 VK_SUCCESS");
        Check(vi.loaderLayerInterfaceVersion == CURRENT_LOADER_LAYER_INTERFACE_VERSION,
              "loaderLayerInterfaceVersion == CURRENT_LOADER_LAYER_INTERFACE_VERSION (2)");
        Check(vi.pfnGetInstanceProcAddr != nullptr, "pfnGetInstanceProcAddr != nullptr");
        Check(vi.pfnGetDeviceProcAddr != nullptr, "pfnGetDeviceProcAddr != nullptr");
        Check(vi.pfnGetPhysicalDeviceProcAddr != nullptr,
              "pfnGetPhysicalDeviceProcAddr != nullptr (interface v2 必须提供)");
    }

    // loader 传来的版本比我们支持的高 -> 必须回落到 CURRENT
    {
        VkNegotiateLayerInterface vi{};
        vi.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        vi.loaderLayerInterfaceVersion = 99;
        const VkResult r = negotiate(&vi);
        Check(r == VK_SUCCESS && vi.loaderLayerInterfaceVersion == 2,
              "loader 要求 v99 时回落到 v2 且成功");
    }

    // 版本低于最低支持（1）-> 拒绝
    {
        VkNegotiateLayerInterface vi{};
        vi.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        vi.loaderLayerInterfaceVersion = 0;
        const VkResult r = negotiate(&vi);
        Check(r == VK_ERROR_INITIALIZATION_FAILED, "loader 要求 v0 时返回 VK_ERROR_INITIALIZATION_FAILED");
    }

    // sType 不对 -> 拒绝
    {
        VkNegotiateLayerInterface vi{};
        vi.sType = LAYER_NEGOTIATE_UNINTIALIZED;
        vi.loaderLayerInterfaceVersion = 2;
        const VkResult r = negotiate(&vi);
        Check(r == VK_ERROR_INITIALIZATION_FAILED, "sType 非法时返回 VK_ERROR_INITIALIZATION_FAILED");
    }

    // -----------------------------------------------------------------------
    // 从此刻开始用**协商拿到的**指针，模拟真实 loader 的行为。
    // -----------------------------------------------------------------------
    VkNegotiateLayerInterface vi{};
    vi.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    vi.loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    negotiate(&vi);
    PFN_vkGetInstanceProcAddr gipa = vi.pfnGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = vi.pfnGetDeviceProcAddr;

    std::printf("\n[4] 拦截 vs 转发（用假 loader 检查 GIPA 返回值）\n");
    {
        const auto hook_create =
            reinterpret_cast<PFN_vkVoidFunction>(gipa(nullptr, "vkCreateInstance"));
        Check(hook_create != nullptr, "gipa(NULL, \"vkCreateInstance\") != nullptr");
        Check(hook_create != reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded),
              "vkCreateInstance 被本层拦截（没有直接落到下一层）");

        const auto hook_draw =
            reinterpret_cast<PFN_vkVoidFunction>(gipa(nullptr, "vkCmdDrawIndexed"));
        Check(hook_draw != nullptr &&
                  hook_draw != reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded),
              "vkCmdDrawIndexed 被本层拦截");

        const auto hook_bind = reinterpret_cast<PFN_vkVoidFunction>(
            gipa(nullptr, "vkCmdBindVertexBuffers"));
        Check(hook_bind != nullptr &&
                  hook_bind != reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded),
              "vkCmdBindVertexBuffers 被本层拦截");

        const auto hook_submit =
            reinterpret_cast<PFN_vkVoidFunction>(gipa(nullptr, "vkQueueSubmit"));
        Check(hook_submit != nullptr &&
                  hook_submit != reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded),
              "vkQueueSubmit 被本层拦截");

        // 注意：在 vkCreateInstance 之前，loader 还没通过 pNext 链把「下一层的 GIPA」
        // 交给我们，所以此时无法转发，只能返回 nullptr。这是本层有意的行为
        // （见 layer.cpp 顶部对「无 instance 全局命令」的说明）。
        // 真正的转发能力在 [5] 里用已注册 instance + 哨兵函数验证。
        const auto not_yet_forwardable =
            reinterpret_cast<PFN_vkVoidFunction>(gipa(nullptr, "vkSomeUnhookedCommand"));
        Check(not_yet_forwardable == nullptr,
              "vkCreateInstance 之前未拦截的命令返回 nullptr（此时尚无下一层 GIPA）");

        const auto d_hook =
            reinterpret_cast<PFN_vkVoidFunction>(gdpa(nullptr, "vkCmdBindIndexBuffer"));
        Check(d_hook != nullptr && d_hook != reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded),
              "GDPA 也拦截 vkCmdBindIndexBuffer");
    }

    // -----------------------------------------------------------------------
    std::printf("\n[5] 模拟 loader 调用本层 vkCreateInstance（走 pNext 链）\n");
    {

        // 搭出 loader 会传的那条链。
        VkLayerInstanceLink link{};
        link.pNext = nullptr;
        link.pfnNextGetInstanceProcAddr = &StubGetInstanceProcAddr;
        link.pfnNextGetPhysicalDeviceProcAddr = &StubGetPhysicalDeviceProcAddr;

        VkLayerInstanceCreateInfo layer_info{};
        layer_info.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
        layer_info.pNext = nullptr;
        layer_info.function = VK_LAYER_LINK_INFO;
        layer_info.u.pLayerInfo = &link;

        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "efml4a-smoke-host";

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pNext = &layer_info;
        ci.pApplicationInfo = &app_info;

        auto layer_create_instance =
            reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

        VkInstance instance = VK_NULL_HANDLE;
        const VkResult r = layer_create_instance(&ci, nullptr, &instance);

        Check(r == VK_SUCCESS, "本层 vkCreateInstance 返回 VK_SUCCESS");
        Check(instance != VK_NULL_HANDLE, "假 loader 回填了 VkInstance");
        Check(g_stub_gipa_calls > 0, "本层确实调用了下一层的 GIPA");
        Check(layer_info.u.pLayerInfo == nullptr,
              "VK_LAYER_LINK_INFO 的 pLayerInfo 已被推进（pNext 为空 -> nullptr）");

        // instance 已经注册 -> 用真实 instance 查某个未拦截的名字应当转发成功。
        const auto forwarded_with_instance =
            reinterpret_cast<PFN_vkVoidFunction>(gipa(instance, "vkAnotherUnhookedCommand"));
        Check(forwarded_with_instance == reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded),
              "已注册 instance 上未拦截的命令可正常转发（拿到哨兵）");

        // 现在 g_pre_instance_gipa 已经记下来了，因此「无 instance」的全局命令
        // 也能转发到下一层了。
        const auto forwarded_global =
            reinterpret_cast<PFN_vkVoidFunction>(gipa(nullptr, "vkYetAnotherUnhooked"));
        Check(forwarded_global == reinterpret_cast<PFN_vkVoidFunction>(&SentinelForwarded),
              "vkCreateInstance 之后，全局命令也能转发到下一层（拿到哨兵）");

        // 下层 GIPA 返回的普通函数指针必须被原样透传，而不是被吞掉。
        const auto passthrough =
            reinterpret_cast<PFN_vkVoidFunction>(gipa(instance, "vkGetDeviceProcAddr"));
        Check(passthrough == reinterpret_cast<PFN_vkVoidFunction>(&StubGetDeviceProcAddr),
              "下层 GIPA 返回的函数指针被原样透传");

        // 清理，避免 valgrind/ASAN 层面留下悬垂（本测试不强求）。
        auto layer_destroy_instance =
            reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
        if (layer_destroy_instance != nullptr) {
            layer_destroy_instance(instance, nullptr);
        }
    }

    // -----------------------------------------------------------------------
    std::printf("\n[6] Android 自省入口（自报家门）\n");
    {
        auto enum_layers = LoadSymbol<PFN_vkEnumerateInstanceLayerProperties>(
            handle, "vkEnumerateInstanceLayerProperties");
        Check(enum_layers != nullptr, "vkEnumerateInstanceLayerProperties 已导出");

        if (enum_layers != nullptr) {
            uint32_t count = 0;
            VkResult r = enum_layers(&count, nullptr);
            Check(r == VK_SUCCESS && count == 1, "自省返回 1 个层");

            VkLayerProperties props{};
            count = 1;
            r = enum_layers(&count, &props);
            Check(r == VK_SUCCESS && count == 1, "填充层属性成功");
            Check(std::strcmp(props.layerName, "VK_LAYER_EFML4A_model_replacer") == 0,
                  "层名 == VK_LAYER_EFML4A_model_replacer");
            Check(props.specVersion == VK_API_VERSION_1_3, "specVersion == 1.3");
            Check(props.implementationVersion == 1, "implementationVersion == 1");
        }

        auto enum_exts = LoadSymbol<PFN_vkEnumerateInstanceExtensionProperties>(
            handle, "vkEnumerateInstanceExtensionProperties");
        Check(enum_exts != nullptr, "vkEnumerateInstanceExtensionProperties 已导出");

        if (enum_exts != nullptr) {
            uint32_t count = 0;
            VkResult r = enum_exts("VK_LAYER_EFML4A_model_replacer", &count, nullptr);
            Check(r == VK_SUCCESS && count == 0, "用本层名字查扩展 -> 成功且 0 个扩展");

            r = enum_exts("VK_LAYER_SOMEONE_ELSE", &count, nullptr);
            Check(r == VK_ERROR_LAYER_NOT_PRESENT, "用别的层名字查扩展 -> VK_ERROR_LAYER_NOT_PRESENT");
        }

        auto enum_dev_layers = LoadSymbol<PFN_vkEnumerateDeviceLayerProperties>(
            handle, "vkEnumerateDeviceLayerProperties");
        Check(enum_dev_layers != nullptr,
              "vkEnumerateDeviceLayerProperties 已导出（Android 需要它才会把层当成 global）");
    }

    // -----------------------------------------------------------------------
    std::printf("\n[7] EFML4A_GetStatsJson()\n");
    {
        const char* json = get_stats();
        Check(json != nullptr, "返回非空指针");
        if (json != nullptr) {
            std::printf("  stats: %s\n", json);
            Check(json[0] == '{', "是 JSON 对象");
            Check(std::strstr(json, "\"layer\":\"VK_LAYER_EFML4A_model_replacer\"") != nullptr,
                  "包含 layer 名");
            Check(std::strstr(json, "\"vkCreateInstance\":1") != nullptr,
                  "vkCreateInstance 计数 == 1");
            Check(std::strstr(json, "\"vkCmdDrawIndexed\":0") != nullptr,
                  "vkCmdDrawIndexed 计数存在（== 0）");
            Check(std::strstr(json, "\"vkQueueSubmit\":0") != nullptr, "vkQueueSubmit 计数存在");
        }
    }

    Check(dlclose(handle) == 0, "dlclose() succeeded");

    // -----------------------------------------------------------------------
    std::printf("\n== %d checks, %d failures ==\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("SMOKE TEST PASSED\n");
    return 0;
}
