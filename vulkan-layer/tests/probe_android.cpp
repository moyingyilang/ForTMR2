// EFML4A 设备端 Vulkan 探针（真机验证用）
//
// 分级验证 Vulkan 隐式层的拦截链路，每级结束都把层统计写入
// ./efml4a-layer-stats.json（即使随后驱动崩溃也不丢证据，这是踩过坑之后的教训）：
//
//   stage 1  instance + physical device + logical device + buffer + memory + command pool
//   stage 2  + vkCmdBindVertexBuffers / vkCmdBindIndexBuffer → 提交 → 读统计
//   stage 3  + vkCmdDrawIndexed
//
// ⚠️ stage 3 会在**没有绑定 pipeline / render pass** 的情况下调用 vkCmdDrawIndexed，
//    实测会把 Adreno 驱动直接搞崩（段错误）。这不是层的缺陷——不带层同样崩，
//    而是「无 pipeline 直接 draw」属于非法用法。要在真机上验证 draw 级拦截，
//    必须先构造合法的 graphics pipeline（需要 SPIR-V），属于 M2 的工作。
//
// 构建与运行见 vulkan-layer/scripts/run-ondevice-probe.sh。
// EFML4A 探针 3：分级验证拦截，每级结束都把层统计落盘（崩溃也不丢证据）。
//   用法： ./efml_probe3 <stage>      stage: 1=device  2=+bind  3=+draw  4=+pipeline
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using StatsFn = const char *(*)();
static int g_stage = 1;

static void dump_stats(const char *tag) {
    void *h = dlopen("libVkLayer_EFML4A.so", RTLD_NOW | RTLD_NOLOAD);
    const char *json = "(层未映射)";
    if (h) {
        auto fn = reinterpret_cast<StatsFn>(dlsym(h, "EFML4A_GetStatsJson"));
        if (fn) json = fn();
    }
    std::printf("[stats:%s] %s\n", tag, json);
    if (FILE *f = std::fopen("efml4a-layer-stats.json", "w")) {
        std::fprintf(f, "%s\n%s\n", tag, json); std::fclose(f);
    }
}
#define CHECK(expr, what) do { VkResult _r = (expr); \
    std::printf("  %-44s -> %d%s\n", what, (int)_r, _r == VK_SUCCESS ? "  ✓" : "  ✗"); } while (0)

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_stage = (argc > 1) ? std::atoi(argv[1]) : 1;
    std::printf("===== stage %d =====\n", g_stage);

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "EFML4A probe3"; ai.apiVersion = VK_API_VERSION_1_1;
    const char *layers[1] = {"VK_LAYER_EFML4A_model_replacer"};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;

    uint32_t lc = 0; vkEnumerateInstanceLayerProperties(&lc, nullptr);
    std::vector<VkLayerProperties> lp(lc);
    if (lc) vkEnumerateInstanceLayerProperties(&lc, lp.data());
    bool found = false;
    for (auto &l : lp) if (!std::strcmp(l.layerName, "VK_LAYER_EFML4A_model_replacer")) found = true;
    std::printf("[probe3] 层可见: %s\n", found ? "是" : "否");
    if (found) { ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = layers; }

    VkInstance inst = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&ci, nullptr, &inst), "vkCreateInstance");
    if (!inst) return 1;

    uint32_t pc = 0; vkEnumeratePhysicalDevices(inst, &pc, nullptr);
    std::vector<VkPhysicalDevice> pds(pc);
    if (pc) vkEnumeratePhysicalDevices(inst, &pc, pds.data());
    uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(pds[0], &qc, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qc);
    vkGetPhysicalDeviceQueueFamilyProperties(pds[0], &qc, qfs.data());
    uint32_t qfi = 0; for (uint32_t i = 0; i < qc; ++i) if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(pds[0], &dci, nullptr, &dev), "vkCreateDevice");
    if (!dev) return 1;
    VkQueue q = VK_NULL_HANDLE; vkGetDeviceQueue(dev, qfi, 0, &q);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 4096; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(dev, &bci, nullptr, &buf), "vkCreateBuffer");
    VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(pds[0], &mp);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mt = i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem), "vkAllocateMemory");
    CHECK(vkBindBufferMemory(dev, buf, mem, 0), "vkBindBufferMemory");

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = qfi;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &pool), "vkCreateCommandPool");

    // ── 阶段 1 结束：设备级链路
    if (g_stage == 1) { dump_stats("stage1-设备级"); return 0; }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cb, &cbbi), "vkBeginCommandBuffer");

    // ── 阶段 2：只做顶点/索引缓冲绑定（换模型的核心机制），不绘制
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &buf, &off);
    vkCmdBindIndexBuffer(cb, buf, 0, VK_INDEX_TYPE_UINT16);
    std::printf("  %-44s -> 已调用\n", "vkCmdBindVertexBuffers + vkCmdBindIndexBuffer");
    CHECK(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(dev, &fci, nullptr, &fence), "vkCreateFence");
    VkSubmitInfo sii{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sii.commandBufferCount = 1; sii.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(q, 1, &sii, fence), "vkQueueSubmit");
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull), "vkWaitForFences");
    if (g_stage == 2) { dump_stats("stage2-绑定级"); return 0; }

    // ── 阶段 3：加入 draw（已知会让 Adreno 驱动崩溃，故放在最后单独测）
    VkCommandBuffer cb2 = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb2), "vkAllocateCommandBuffers(2)");
    CHECK(vkBeginCommandBuffer(cb2, &cbbi), "vkBeginCommandBuffer(2)");
    vkCmdBindVertexBuffers(cb2, 0, 1, &buf, &off);
    vkCmdBindIndexBuffer(cb2, buf, 0, VK_INDEX_TYPE_UINT16);
    std::printf("  即将调用 vkCmdDrawIndexed（无 pipeline，可能崩驱动）...\n");
    vkCmdDrawIndexed(cb2, 3, 1, 0, 0, 0);
    std::printf("  vkCmdDrawIndexed 返回了（驱动未崩）\n");
    CHECK(vkEndCommandBuffer(cb2), "vkEndCommandBuffer(2)");
    dump_stats("stage3-绘制级");
    return 0;
}
