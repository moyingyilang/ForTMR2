# ForTMR Vulkan 隐式层骨架（`vulkan-layer/`）

ForTMR（原 EFML4A，for tomorrow）= **A**rknights: **E**ndfield **M**od **L**oader **for** **A**ndroid（技术符号/层文件名仍保留 `EFML4A`，见 [docs/H1](../docs/H1-repo-map.md)）。

PC 上 3DMigoto 通过 **DXGI 代理 DLL** 把 D3D11 调用劫持进游戏进程；Android 没有 DXGI，
最接近的对应物就是 **Vulkan 隐式层（implicit layer）**：一个由系统 Vulkan loader 加载进
目标 app 进程的原生库，可以看到并修改每一个 `vk*` 调用。

本目录是一个**可编译的骨架**，目标是证明「能编译 / 能装载 / 能协商 / 能拦截 / 能转发」。
真正的「换模型」（替换顶点/索引缓冲、替换贴图）**尚未实现**，设计草案见
[`src/hooks.cpp`](src/hooks.cpp) 末尾的 `TODO(efml4a)` 区块。

> **当前状态（2026-11-22 审计）**：本层可在本机 host 构建并通过 46 项冒烟测试
> （见 §5），但「Vulkan 隐式层」这条注入路线已在真机上**被证伪**——反篡改看门狗会
> 把 hook 拔掉。历史结论见 [../docs/F1-history.md](../docs/F1-history.md) 与
> [../docs/archive/04-route-vulkan-layer.md](../docs/archive/04-route-vulkan-layer.md)。
> 目录保留作技术证据与可复用的 Linux/Android 层实现参考，**不要当主线方案投入**。

---

## 1. 目录结构

```
vulkan-layer/
├── CMakeLists.txt                    # C++17 + FetchContent(Vulkan-Headers) + CTest
├── README.md                         # 本文件
├── include/efml4a/
│   ├── layer.h                       # 层入口声明与版本宏
│   ├── dispatch.h                    # instance / device dispatch table
│   ├── log.h                         # 日志宏（logcat / stderr 双后端）
│   ├── hook.h                        # 拦截点声明 + 注册表 + 统计计数
│   ├── mode.h                        # 运行模式（off/recon/skip/donor）解析与热更新
│   └── recon.h                       # 落盘侦察（JSONL）与 stats 落盘接口
├── src/
│   ├── layer.cpp                     # vkNegotiateLoaderLayerInterfaceVersion + GIPA/GDPA
│   │                                 #   + Android 自省入口
│   ├── dispatch.cpp                  # dispatch table 填充 + 实例/设备/队列/命令缓冲注册表
│   ├── hooks.cpp                     # 14 个拦截点（转发 + 计数 + 日志）+ 设计 TODO
│   ├── log.cpp                       # 日志后端
│   ├── mode.cpp                      # debug.efml4a.mode 属性/环境变量解析、按帧刷新
│   └── recon.cpp                     # 输出目录选择、JSONL 追加、stats tmp+rename
├── layer/
│   ├── VkLayer_EFML4A.json.in        # layer manifest 模板（@EFML4A_LAYER_LIBRARY_PATH@）
│   ├── efml4a_layer.map              # 导出符号白名单（linker version script）
│   └── install_layer.sh              # 安装到 Android 设备（需 root，脚本内有显式提示）
├── scripts/                          # 交叉编译 / 真机试验脚本
│   ├── build-android-termux.sh       # Termux clang + NDK sysroot 交叉编译
│   ├── game-test-cycle.sh            # 隐式层版的游戏测试循环（路线已证伪，保留记录）
│   ├── install-implicit-layer.sh     # 安装隐式层到 /data/local/debug/vulkan
│   └── run-ondevice-probe.sh         # 自建 Vulkan 探针验证拦截链路
└── tests/
    ├── smoke_host.cpp                # 宿主侧冒烟测试（dlopen + 假 loader 端到端）
    └── test_pure.cpp                 # 纯函数/落盘逻辑单测（recon / mode）
```

产物：`build/libVkLayer_EFML4A.so`、`build/VkLayer_EFML4A.json`、`build/efml4a_smoke_host`、
`build/efml4a_test_pure`。

---

## 2. 构建

### 2.1 依赖：Khronos 官方 Vulkan-Headers（FetchContent）

`CMakeLists.txt` 用 `FetchContent` 拉取官方头文件。**本机没有 Vulkan SDK，也不需要**
（`Vulkan-Headers` 是纯头文件 INTERFACE 库）。

```cmake
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY "https://github.com/KhronosGroup/Vulkan-Headers.git"
    GIT_TAG        "vulkan-sdk-1.4.321.0"
    GIT_SHALLOW    FALSE)
FetchContent_MakeAvailable(VulkanHeaders)
target_link_libraries(EFML4A_VkLayer PRIVATE Vulkan::Headers)
```

#### ✅ 已核实的 tag 与 target 名字

| 项 | 任务书建议 | 核实结果 |
|---|---|---|
| tag | `v1.4.321` | ❌ **不存在**。官方 tag 形如 `vulkan-sdk-<major>.<minor>.<patch>.<build>`；`v1.4.321` 在 `git ls-remote --tags` 里搜不到。 |
| tag（实际使用） | — | ✅ `vulkan-sdk-1.4.321.0`（GitHub API `/repos/KhronosGroup/Vulkan-Headers/tags` 确认存在，同批还有 `vulkan-sdk-1.4.328.0/328.1/335.0/341.0/350.0/350.1/357.0/363.0`） |
| CMake target | `Vulkan::Headers` | ✅ 存在。`Vulkan-Headers/CMakeLists.txt:48-49`：`add_library(Vulkan-Headers INTERFACE)` + `add_library(Vulkan::Headers ALIAS Vulkan-Headers)` |
| `Vulkan-Headers` target | 任务书提到 | ✅ 也真实存在（就是 `Vulkan::Headers` 别名指向的实体 target） |
| `cmake_minimum_required` | — | Vulkan-Headers 自身要求 `3.22.1`，所以本工程设为 `3.22`。 |

#### 受限网络 / 离线构建

本机 `github.com:443` 被墙（`git clone` 会超时），但 `codeload.github.com` 与
`api.github.com` 可达。因此 `CMakeLists.txt` 提供三条路径，按推荐顺序：

```bash
# 方式 A（推荐，本次实际验证通过）：让 FetchContent 直接下载官方 tarball。
# github.com 的 git 端口被墙时，codeload.github.com 通常仍可达。
cmake -S . -B build -G Ninja \
  -DEFML4A_VULKAN_HEADERS_URL=https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/vulkan-sdk-1.4.321.0

# 方式 B：手工下载并解包后，指向源码目录（FetchContent 完全跳过下载步骤）
cmake -S . -B build -G Ninja \
  -DEFML4A_VULKAN_HEADERS_SOURCE_DIR=$PWD/Vulkan-Headers-vulkan-sdk-1.4.321.0

# 方式 C：换成可用的 git 镜像
cmake -S . -B build -G Ninja \
  -DEFML4A_VULKAN_HEADERS_REPO=https://<mirror>/Vulkan-Headers.git
```

* 方式 A 走 FetchContent 的 `URL` 下载路径（本机实测成功，配置阶段耗时约 134 s，
  解包到 `build/_deps/vulkanheaders-src`）。
* 方式 B 的实现是设置 FetchContent 原生支持的 `FETCHCONTENT_SOURCE_DIR_VULKANHEADERS`。
* 方式 A/B 与默认的 `GIT_REPOSITORY` 路径互斥，由 CMake 内部 `if/else` 选择。

> ⚠️ 一个小坑：`set(... CACHE STRING "doc")` 的文档字符串必须是**一个**字符串字面量；
> 写成多个相邻字面量会被当成列表并用 `;` 连接，最终 URL 会变成垃圾值
> （报错 `At least one entry of URL is a path`）。代码里已加注释。

### 2.2 host 构建（用于本地验证）

```bash
cd vulkan-layer
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

### 2.3 Android NDK 交叉编译

```bash
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-android
```

> **2026-11-22 审计更正**：本机已装 `~/android-sdk/ndk/29.0.14206865`，
> 且 `build-android/libVkLayer_EFML4A.so` 是 **NDK r29 交叉编译产物**
> （`file` 报 ARM aarch64 / Android 29），说明这条命令在本机执行过。
> 本次审计没有重新跑一遍交叉编译，真机加载验证仍需 NDK/SDK 环境与设备。

#### 判别 host / Android 的条件（有个 Termux 坑）

`CMakeLists.txt` 用 **`CMAKE_CROSSCOMPILING`** 而不是 `if(ANDROID)` 来区分两种构建：

| 场景 | `CMAKE_SYSTEM_NAME` | `ANDROID` | `CMAKE_CROSSCOMPILING` | 结论 |
|---|---|---|---|---|
| Termux 本地构建 | `Android` | `1` | `FALSE` | 构建并运行冒烟测试 |
| NDK 交叉编译 | `Android` | `1` | `TRUE` | 跳过冒烟测试（跑不了） |

原因：**Termux 的 `uname` 把 host 报告成 `Android`**，于是 CMake 认为
`CMAKE_HOST_SYSTEM_NAME=Android`，连带 `ANDROID=1`。如果按 `if(ANDROID)` 分支，
Termux 本地构建会被误判成交叉编译、冒烟测试被跳过。而「是否交叉编译」才是正确判据。
（本机实测：`CMAKE_HOST_SYSTEM_NAME=[Android] ANDROID=[1] CMAKE_CROSSCOMPILING=FALSE`。）

#### 静态链接 libc++ —— 不是优化，是必需

AOSP `frameworks/native/vulkan/libvulkan/layers_extensions.cpp` 的
`LayerLibrary::Open()` 里有一条原文注释：

> Libraries in the system layer library dir can't be loaded into the application namespace.
> That causes compatibility problems, since any symbol dependencies will be resolved by system
> libraries. They **can't safely use libc++_shared**, for example.

也就是说层被 `dlopen` 进**应用的 linker namespace** 时，`libc++_shared.so` 很可能不在其
搜索路径里，层会因为缺符号而加载失败。所以 `CMakeLists.txt` 默认打开
`EFML4A_STATIC_LIBCXX=ON`（`-static-libstdc++`）。本机实测 `DT_NEEDED` 变化：

```
未开启: liblog.so libdl.so libc.so libc++_shared.so libm.so
开启后: liblog.so libdl.so libc.so libm.so          ← libc++_shared.so 消失
```

#### 导出符号白名单（version script）

`layer/efml4a_layer.map` 用 linker version script 把 ABI 面**收敛成恰好 10 个符号**，
并配 `-Wl,--exclude-libs,ALL` 作为第二道保险。

为什么必须这么做：静态链进来的 libc++ 符号默认是 `default visibility`，
`-fvisibility=hidden` **管不到静态库**。实测在没有白名单时，
`readelf --dyn-syms` 会列出约 400 个 `std::__ndk1::*` 符号 —— 相当于把一整份 libc++
注入宿主进程；如果目标 app 自己也用 `libc++_shared`，就可能发生符号插入
（interposition）冲突。加上白名单后导出表恰好是：

```
EFML4A_GetDeviceProcAddr            EFML4A_GetInstanceProcAddr
EFML4A_GetPhysicalDeviceProcAddr    EFML4A_GetStatsJson
vkEnumerateDeviceLayerProperties    vkEnumerateInstanceExtensionProperties
vkEnumerateInstanceLayerProperties  vkGetDeviceProcAddr
vkGetInstanceProcAddr               vkNegotiateLoaderLayerInterfaceVersion
```

### 2.4 编译选项

C++17、`-Wall -Wextra`、默认 `RelWithDebInfo`、`-fvisibility=hidden` +
`EFML4A_LAYER_EXPORT` 显式导出、`-Wl,--no-undefined` 缺符号立刻报错。
**本机 host 构建为 0 warning / 0 error。**

---

## 3. ⚠️ 核实结论汇总（任务书里哪些假设不成立）

这一节是本骨架最重要的产出之一：任务书里有若干 API 名字假设**是错的**，全部已用实际
头文件/源码核对。

| # | 任务书的说法 | 核实结果 | 证据 |
|---|---|---|---|
| 1 | tag `v1.4.321` 存在 | ❌ 不存在，实际是 `vulkan-sdk-1.4.321.0` | `git ls-remote --tags` / GitHub tags API |
| 2 | `Vulkan::Headers` target 存在 | ✅ 存在 | `Vulkan-Headers/CMakeLists.txt:49` |
| 3 | `vulkan/vk_layer.h` 由 Vulkan-Headers 提供 | ✅ 提供 | `include/vulkan/vk_layer.h`（189 行） |
| 4 | `vk_layer.h` 里有 `VkLayerInstanceDispatchTable` / `VkLayerDeviceDispatchTable` | ❌ **完全没有**。`grep -rn` 整个 Vulkan-Headers 仓库 0 命中 | — |
| 5 | 设备表叫 `VkLayerDeviceDispatchTable` | ❌ 真名是 **`VkLayerDispatchTable`**，且首字段是 `uint64_t magic` | `Vulkan-Loader/loader/generated/vk_layer_dispatch_table.h:316` |
| 6 | 实例表叫 `VkLayerInstanceDispatchTable` | ✅ 名字对，但它在 **Vulkan-Loader 的生成文件**里：`loader/generated/vk_layer_dispatch_table.h:38`，不随 Vulkan-Headers 分发 | 同上 |
| 7 | `VK_LOADER_DATA` | ❌ 枚举成员真名是 **`VK_LOADER_DATA_CALLBACK`**（值 1） | `vk_layer.h` `VkLayerFunction_` |
| 8 | `VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK` | ✅ 正确（值 2）；同枚举还有 `VK_LAYER_LINK_INFO=0`、`VK_LOADER_FEATURES=3` | 同上 |
| 9 | `CURRENT_LOADER_LAYER_INTERFACE_VERSION` == 2 | ✅ 正确 | `vk_layer.h:24` |
| 10 | `LAYER_NEGOTIATE_INTERFACE_STRUCT` | ✅ 正确（值 1） | `vk_layer.h:33-36` |
| 11 | `VkNegotiateLayerInterface` 字段 | ✅ `sType/pNext/loaderLayerInterfaceVersion/pfnGetInstanceProcAddr/pfnGetDeviceProcAddr/pfnGetPhysicalDeviceProcAddr` 全部正确 | `vk_layer.h:39-46` |
| 12 | manifest `layer.functions.vkNegotiateLoaderLayerInterfaceVersion` | ✅ 真实字段（JSON ≥ 1.1.0 才解析） | `Vulkan-Loader/loader/loader.c:2646-2649` |
| 13 | manifest `enable_environment` | ✅ 真实，**可选**，只在 implicit layer 上解析 | `loader.c:2754`，`docs/LoaderLayerInterface.md` |
| 14 | manifest `disable_environment` | ✅ 真实，且对 implicit layer 是 **REQUIRED**（缺了会 skip 整个 layer） | `loader.c:2606-2615` |
| 15 | manifest `layer.type` = `GLOBAL` | ✅ 合法（`DEVICE` 已废弃；`INSTANCE`/`GLOBAL` 都映射到 instance-layer flag） | `loader.c:2469-2480` |
| 16 | manifest `file_format_version` | ✅ 支持 1.0.0 / 1.0.1 / 1.1.0 / 1.1.1 / 1.1.2 / 1.2.0；本工程用 `1.2.0` | `docs/LoaderLayerInterface.md` 字段表 |
| 17 | Android 读 layer manifest | ❌ **完全不读**。Android loader 直接扫描 `libVkLayer*.so` 并 `dlsym` 自省入口 | `layers_extensions.cpp` 全文无 `.json` |

### 因此 `dispatch.h` 是手写的

因为官方类型不在 Vulkan-Headers 里，本骨架在
[`include/efml4a/dispatch.h`](include/efml4a/dispatch.h) 自定义了两个**最小子集**表：

```cpp
struct InstanceDispatchTable { PFN_GetPhysicalDeviceProcAddr GetPhysicalDeviceProcAddr; ... };
struct DeviceDispatchTable   { uint64_t magic = 0; PFN_vkGetDeviceProcAddr GetDeviceProcAddr; ... };
```

字段名刻意与 loader 生成表**逐字一致**，将来若要换成官方生成表，改动只是替换类型名。
填充用显式赋值宏（任务书要求的 GETPROCADDR 套路），依赖调用点同名局部变量：

```cpp
#define EFML4A_LOAD_INSTANCE_PROC(field) \
    do { (dst)->field = reinterpret_cast<decltype((dst)->field)>( \
             (next_gipa)((instance), "vk" #field)); } while (0)
```

新增拦截点只需在 `dispatch.h` 加字段 + 在 `dispatch.cpp` 加一行，注释里标了「按需扩展」。

---

## 4. 层入口与设计

### 4.1 导出的符号（已用 `readelf --dyn-syms` 核对）

```
EFML4A_GetDeviceProcAddr            EFML4A_GetInstanceProcAddr
EFML4A_GetPhysicalDeviceProcAddr    EFML4A_GetStatsJson
vkEnumerateDeviceLayerProperties    vkEnumerateInstanceExtensionProperties
vkEnumerateInstanceLayerProperties  vkGetDeviceProcAddr
vkGetInstanceProcAddr               vkNegotiateLoaderLayerInterfaceVersion
```

**为什么要导出「无前缀」的 `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`**：
桌面 loader 走协商（`vkNegotiateLoaderLayerInterfaceVersion`），而
**Android loader 从不调用协商函数**，它用 `dlsym` 按名字找。已核实
AOSP `layers_extensions.cpp` 的 `LayerLibrary::GetGPA()`：

```cpp
std::string layer_name{layer.properties.layerName};
if (void* gpa = GetTrampoline(layer_name.append(gpa_name).c_str())) return gpa;
return GetTrampoline((std::string{"vk"}.append(gpa_name)).c_str());
```

先试 `<层名>GetInstanceProcAddr`，再试 `vkGetInstanceProcAddr`；并且
`LayerChain::LoadLayer()` 找不到 `vkGetInstanceProcAddr` 会直接返回
`VK_ERROR_LAYER_NOT_PRESENT` —— 在 Android 上不导出它 = 层根本挂不上。

### 4.2 版本协商

`vkNegotiateLoaderLayerInterfaceVersion` 完整实现了协议要求：

- `pVersionStruct == nullptr` → `VK_ERROR_INITIALIZATION_FAILED`
- `sType != LAYER_NEGOTIATE_INTERFACE_STRUCT` → 拒绝
- `loaderLayerInterfaceVersion < MIN_SUPPORTED(1)` → 拒绝
- `loaderLayerInterfaceVersion > CURRENT(2)` → **回落到 2**（协议要求的行为）
- 设置 `pfnGetInstanceProcAddr` / `pfnGetDeviceProcAddr`；
  `pfnGetPhysicalDeviceProcAddr` 只在接口版本 ≥ 2 时设置

### 4.3 instance / device 注册表

Vulkan 的 dispatch 是「按对象」的：loader 通过 `vkCreateInstance` / `vkCreateDevice`
的 `pNext` 链把「下一层的 GIPA/GDPA」交给层，层必须保存下来并在每次拦截时用它转发。
所以有：

| 表 | 键 | 用途 |
|---|---|---|
| `g_instance_map` | `VkInstance` | instance dispatch table + loader 回调 |
| `g_device_map` | `VkDevice` | device dispatch table |
| `g_physdev_map` | `VkPhysicalDevice` | `vkCreateDevice` 里反查 `VkInstance` |
| `g_queue_map` | `VkQueue` | `vkQueueSubmit` 只给 queue，需要反查 device |
| `g_cmdbuf_map` | `VkCommandBuffer` | 所有 `vkCmd*` 只给 command buffer，需要反查 device |

全部是 `std::unordered_map` + `std::mutex`，查询返回**值拷贝**避免数据竞争。
后三张表是「簿记」拦截点（`vkEnumeratePhysicalDevices` / `vkGetDeviceQueue` /
`vkAllocateCommandBuffers` / `vkFreeCommandBuffers`），**不单独计数**，
以保持 `EFML4A_GetStatsJson()` 的 counters 集合与任务书要求的 14 项一致。

`vkCreateInstance` 的 `pNext` 链处理覆盖三种 `VkLayerFunction`：
`VK_LAYER_LINK_INFO`（取 GIPA/GDPA，并**把链向前推进一格**）、
`VK_LOADER_DATA_CALLBACK`（保存 `pfnSetInstanceLoaderData`）、
`VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK`（保存 `pfnLayerCreateDevice`，
`vkCreateDevice` 时优先走这条 loader 指定路径，失败再走常规 GDPA 转发）。

### 4.4 拦截点（转发 + 计数 + 日志）

14 个必需拦截点：`vkCreateInstance`、`vkEnumerateInstanceLayerProperties`、
`vkEnumerateInstanceExtensionProperties`、`vkCreateDevice`、`vkCreateGraphicsPipelines`、
`vkCreateShaderModule`、`vkCmdBindPipeline`、`vkCmdBindVertexBuffers`、
`vkCmdBindIndexBuffer`、`vkCmdDrawIndexed`、`vkCmdDraw`、`vkCmdPushConstants`、
`vkCmdBindDescriptorSets`、`vkQueueSubmit`。
另有 2 个生命周期拦截点 `vkDestroyInstance` / `vkDestroyDevice`（只清理注册表，不计数）。

计数用 `std::atomic<uint64_t>`，经 `EFML4A_GetStatsJson()` 导出成 JSON：

```json
{"layer":"VK_LAYER_EFML4A_model_replacer",
 "counters":{"vkCreateInstance":1,"vkEnumerateInstanceLayerProperties":0, ... }}
```

返回的是 `thread_local` 静态缓冲区，调用方不要 `free`（方便将来和本仓库的 Rust 工具联动）。

### 4.5 一个语义细节：自省 vs 转发

`vkEnumerateInstanceLayerProperties` / `vkEnumerateInstanceExtensionProperties` 有两份实现，
**这是有意的**：

| 实现 | 位置 | 语义 |
|---|---|---|
| 导出的 C 符号 | `src/layer.cpp` → `android_introspect::*` | **自报家门**：报告本层自身属性。给 Android loader 直接 `dlsym` 用。 |
| `efml4a::hooks::*` | `src/hooks.cpp` | **计数 + 沿链转发**。给桌面 loader 的调用链用。 |

如果只有一份「自报家门」的实现，桌面 loader 上应用调用枚举时就会只看到本层、看不到其它层；
如果只有一份「转发」的实现，Android loader 就无法通过自省发现本层。所以必须分开。

`vkEnumerateDeviceLayerProperties` 也必须导出：AOSP 用「实例属性与设备属性 `memcmp` 相等」
来判定 `layer.is_global`，而只有 `is_global` 的层才会被挂进**设备**调用链
（否则 `vkCmdDrawIndexed` 之类的设备命令根本拦不到）。

---

## 5. 构建与冒烟测试的**真实**结果

### 5.1 构建

```
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DEFML4A_VULKAN_HEADERS_SOURCE_DIR=$HOME/.cache/efml4a/Vulkan-Headers-1.4.321.0
-- EFML4A: using local Vulkan-Headers at .../Vulkan-Headers-1.4.321.0
-- Performing Test EFML4A_HAVE_ANDROID_LOG - Success
-- EFML4A: android log backend enabled (-llog links -> logcat)
-- EFML4A: local build (system=Android, host=Android)
-- EFML4A: layer library_path = .../vulkan-layer/build/libVkLayer_EFML4A.so

$ cmake --build build
[1/1] Linking CXX shared library libVkLayer_EFML4A.so      # 0 warning / 0 error

$ file build/libVkLayer_EFML4A.so
ELF 64-bit LSB shared object, ARM aarch64, dynamically linked, for Android 24,
built by NDK r30, with debug_info, not stripped
```

产物：测试程序约 143 KB，层库约 2.0 MB（静态 libc++ 的代价，换来了对 `libc++_shared.so` 的零依赖）。

### 5.2 CTest

```
$ ctest --test-dir build --output-on-failure
    Start 1: efml4a_smoke_host
1/2 Test #1: efml4a_smoke_host ................   Passed    0.01 sec
    Start 2: efml4a_test_pure
2/2 Test #2: efml4a_test_pure .................   Passed    0.02 sec
100% tests passed out of 2
```

> **2026-11-22 审计实测**：本机 `ctest` 现在是 **2 个测试、2/2 通过**。
> 第 2 个 `efml4a_test_pure` 是新加的纯函数/落盘逻辑单测，覆盖
> `recon::PackageFromCmdline`、`mode::ParseMode/ParseCount/Refresh`，
> 以及 JSONL 2000 行封顶、stats 的 tmp+rename 覆盖写。

冒烟测试 `efml4a_smoke_host` 共 **46 项断言，0 失败**，输出 `SMOKE TEST PASSED`。它做了 7 组验证：

1. `dlopen` 层 `.so`
2. `dlsym` 必需导出符号（协商入口、GIPA/GDPA、`EFML4A_GetStatsJson`）
3. 版本协商：`VK_SUCCESS`、GIPA/GDPA/GPD-PA 非空、v99 回落 v2、v0 拒绝、错误 sType 拒绝
4. **拦截 vs 转发**：`gipa(NULL,"vkCreateInstance"|"vkCmdDrawIndexed"|...)` 返回本层 hook
   而不是下层哨兵函数；未拦截的名字落到哨兵（证明真的转发了）
5. **端到端模拟 loader**：手工搭出 `VK_LAYER_LINK_INFO` 的 `pNext` 链 + 假 GIPA + 假
   `vkCreateInstance`，真正调用本层 `vkCreateInstance`，验证：
   返回成功、instance 被回填、下层 GIPA 被调用、链被推进（`pLayerInfo` 变 `nullptr`）、
   注册后已拦截/未拦截命令都正确转发、下层返回的函数指针被原样透传
6. Android 自省入口：自省返回 1 个层、层名/版本正确、扩展枚举语义正确、
   `vkEnumerateDeviceLayerProperties` 已导出
7. `EFML4A_GetStatsJson()`：JSON 合法、`vkCreateInstance` 计数 == 1

```
[7] EFML4A_GetStatsJson()
  stats: {"layer":"VK_LAYER_EFML4A_model_replacer","counters":{"vkCreateInstance":1,
          "vkEnumerateInstanceLayerProperties":0,"vkEnumerateInstanceExtensionProperties":0,
          "vkCreateDevice":0,"vkCreateGraphicsPipelines":0,"vkCreateShaderModule":0,
          "vkCmdBindPipeline":0,"vkCmdBindVertexBuffers":0,"vkCmdBindIndexBuffer":0,
          "vkCmdDrawIndexed":0,"vkCmdDraw":0,"vkCmdPushConstants":0,
          "vkCmdBindDescriptorSets":0,"vkQueueSubmit":0}}
== 46 checks, 0 failures ==
SMOKE TEST PASSED
```

> 一个小坑记录：CTest 的 `FAIL_REGULAR_EXPRESSION` 最初写成 `"FAILED"`，
> 结果匹配到了合法输出里的 `VK_ERROR_INITIALIZATION_FAILED`，导致测试永远失败。
> 现在写成 `"FAILED:"`。

---

## 6. 目标设备（本项目实际要注入的机器）

| 项 | 值 |
|---|---|
| 机型 | Xiaomi `24122RKC7C` |
| SoC | `SM8750`（Snapdragon 8 Elite，平台代号 `sun`） |
| GPU | **Adreno**（`ro.hardware.vulkan=adreno`，驱动 `/vendor/lib64/hw/vulkan.adreno.so`） |
| ABI | `arm64-v8a` |
| 系统 | **Android 17（API 37）** |
| 构建类型 | `ro.build.type=user`，`ro.build.tags=release-keys` |

**这份信息对注入方案的影响（关键结论）**：

因为 `ro.build.type=user`（**不是** userdebug/eng）：

1. 编译期宏 `ANDROID_DEBUGGABLE` 为假 —— 而
   `GraphicsEnv::isDebuggable() = (prctl(PR_GET_DUMPABLE) > 0) || ANDROID_DEBUGGABLE`，
   所以只剩下「进程 dumpable」这一半。**「userdebug 系统自带 root，所以随便注入」这条
   准入路径在本机上不成立。**
2. `ro.debuggable=0`，`/data/local/debug/vulkan` 不会被无条件扫描。
3. 因此只能靠**已 root 的 `user` 构建**去影响目标 app 的 debuggable 判定，
   或者走「目标 app 自己声明 opt-in / 层随 app 的 APK 一起分发」这两条路（见第 7 节）。

> ⚠️ 未核实：本骨架的 Android 侧结论是在 AOSP 分支 **`a16`（Android 16）** 源码上核对的，
> 而目标机是 **Android 17 / API 37**。Android 17 的 `libvulkan` / `GraphicsEnvironment`
> 若调整了层发现或准入逻辑，结论可能需要更新。

---

## 7. Android 侧 manifest 与启用方式（重点）

### 7.1 桌面 manifest 字段说明（本工程 `layer/VkLayer_EFML4A.json.in`）

字段名全部已核实（`Vulkan-Loader/loader/loader.c` + `docs/LoaderLayerInterface.md`）：

```json
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_EFML4A_model_replacer",
    "type": "GLOBAL",
    "library_path": "@EFML4A_LAYER_LIBRARY_PATH@",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "EFML4A model replacer (implicit layer skeleton ...)",
    "functions": { "vkNegotiateLoaderLayerInterfaceVersion":
                   "vkNegotiateLoaderLayerInterfaceVersion" },
    "enable_environment":  { "EFML4A_ENABLE": "1" },
    "disable_environment": { "EFML4A_DISABLE": "1" }
  }
}
```

| 字段 | 必需性 | 说明 |
|---|---|---|
| `file_format_version` | 必需 | 支持 1.0.0 / 1.0.1 / 1.1.0 / 1.1.1 / 1.1.2 / 1.2.0 |
| `layer.name` | 必需 | 层唯一标识；必须与 `EFML4A_LAYER_NAME` 一致 |
| `layer.type` | 必需 | `GLOBAL`（`DEVICE` 已废弃；`INSTANCE`/`GLOBAL` 等价映射到 instance layer） |
| `layer.library_path` | 非 meta-layer 必需 | 由 CMake `configure_file()` 替换；本地构建写绝对路径，Android 构建只写文件名 |
| `layer.api_version` | 必需 | 非零 variant 会被拒绝；本工程 `1.3.0` 与 `EFML4A_LAYER_SPEC_VERSION` 一致 |
| `layer.implementation_version` | 必需 | 整数；本工程 `1` |
| `layer.description` | 必需 | 长度上限 `VK_MAX_DESCRIPTION_SIZE`(256) |
| `layer.functions.vkNegotiateLoaderLayerInterfaceVersion` | 可选（但本层用） | 允许用别名替代默认符号名 |
| `layer.enable_environment` | **可选** | 设置后：只有该环境变量存在且值匹配时才加载 → 实现「隐式但需显式开启」 |
| `layer.disable_environment` | **implicit layer 必需** | 该变量被设为任意非空值即禁用本层（逃生舱） |

**默认带 `enable_environment`**：这样本层是「implicit 但只在显式设置
`EFML4A_ENABLE=1` 时才生效」，不会静默劫持机器上每个 Vulkan 应用。
若想做成常驻层，删掉 `enable_environment` 段即可（`disable_environment` 必须保留）。

生成结果已用 `python3 -c json.load` 验证为合法 JSON。

### 7.2 ⚠️ Android **不读**这个 manifest

已核实（AOSP `frameworks/native/vulkan/libvulkan/layers_extensions.cpp`）：

* 全文**没有任何 `.json` 解析代码**。Android 的层发现是**目录扫描**：
  ```cpp
  void DiscoverLayersInPathList(const std::string& pathstr) {
      std::vector<std::string> paths = android::base::Split(pathstr, ":");
      for (const auto& path : paths)
          ForEachFileInPath(path, [&](const std::string& filename) {
              if (android::base::StartsWith(filename, "libVkLayer") &&
                  android::base::EndsWith(filename, ".so")) {
                  ... AddLayerLibrary(path, filename);
              }
          });
  }
  ```
  → **文件名必须以 `libVkLayer` 开头、`.so` 结尾**，否则被静默忽略。
  （本工程产物 `libVkLayer_EFML4A.so` 满足。）
* 层属性从 `.so` **自省**得到（`LayerLibrary::EnumerateLayers`，用 `dlsym`）：
  `vkEnumerateInstanceLayerProperties`（必需）、`vkEnumerateInstanceExtensionProperties`（必需）、
  `vkEnumerateDeviceLayerProperties` / `vkEnumerateDeviceExtensionProperties`（可选）。
* 挂链用 `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`（见 4.1）。
* **`vkNegotiateLoaderLayerInterfaceVersion` 在 Android 上从不被调用。**

**所以 Android 侧的正确写法是**：

1. 把层编成 `libVkLayer*.so`（名字是硬性要求）；
2. 导出 `vkEnumerateInstanceLayerProperties` / `vkEnumerateInstanceExtensionProperties` /
   `vkEnumerateDeviceLayerProperties` / `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`
   （本骨架已全部导出，并有冒烟测试覆盖）；
3. **manifest `.json` 可以一起 push 过去，但纯属无害文件，Android 不会读它**；
4. 用「目录 + 属性 / Settings」告诉 loader 去哪找、加载哪个层（见 7.3）。

### 7.3 三条启用通道（已核实）

#### (a) `/data/local/debug/vulkan` —— 需要目标 app 自己 dumpable

```cpp
const char kSystemLayerLibraryDir[] = "/data/local/debug/vulkan";

void DiscoverLayers() {
    if (android::GraphicsEnv::getInstance().isDebuggable())
        DiscoverLayersInPathList(kSystemLayerLibraryDir);
    if (!android::GraphicsEnv::getInstance().getLayerPaths().empty())
        DiscoverLayersInPathList(android::GraphicsEnv::getInstance().getLayerPaths());
}
```

`GraphicsEnv::isDebuggable()`：

```cpp
bool GraphicsEnv::isDebuggable() {
    // This flag determines if the application is marked debuggable
    bool appDebuggable = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) > 0;
    // This flag is set only in `debuggable` builds of the platform
#if defined(ANDROID_DEBUGGABLE)
    bool platformDebuggable = true;
#else
    bool platformDebuggable = false;
#endif
    return appDebuggable || platformDebuggable;
}
```

**结论**：`/data/local/debug/vulkan` 只对「app 可调试」或「系统是 debuggable 构建」的进程生效。
本机 `ro.build.type=user` ⇒ `ANDROID_DEBUGGABLE` 为假 ⇒ 只剩 app 侧那一半。

#### (b) `debug.vulkan.layers` 属性 —— 需要 root 才能设置，且只是**回退**通道

已核实真正读它的地方是 `frameworks/native/vulkan/libvulkan/api.cpp`（**不是**老文档说的
`layers_extensions.cpp`）：

```cpp
void AddImplicitLayers() {
    if (!is_instance_) return;
    GetLayersFromSettings();                 // 来自 Settings.Global.GPU_DEBUG_LAYERS
    // If no layers specified via Settings, check legacy properties
    if (implicit_layers_.count <= 0) {
        ParseDebugVulkanLayers();            // debug.vulkan.layers
        ParseDebugVulkanLayer();             // debug.vulkan.layer.<priority>
        ... sort by priorities ...
    }
}

void ParseDebugVulkanLayers() {
    // debug.vulkan.layers specifies colon-separated layer names
    char prop[PROPERTY_VALUE_MAX];
    if (!property_get("debug.vulkan.layers", prop, "")) return;
    int prio = -PROPERTY_VALUE_MAX;          // 负优先级：排在 Settings 之后
    ...
}
```

* 值格式：**冒号分隔的「层名字」**（`VK_LAYER_X:VK_LAYER_Y`），**不是路径**。
* 它是 **legacy 回退通道**：只有当 Settings 没有给出任何层时才生效。
* 另有 `debug.vulkan.layer.<priority>`（priority 0..9 固定区间后连续扫描），同样只接受层名。
* **需要特权**：普通 app 无法设置任何系统属性。只有 `adb shell`（CTS 测试就是这么做的）
  或 root 才能 `setprop`。

> ✅ 已核实：只有列表里的**层名字**被属性指定还不够 —— 该层必须已经出现在
> `DiscoverLayers()` 扫到的目录里（即 `getLayerPaths()` 或 `/data/local/debug/vulkan`）。
> 这两条路的准入门槛见 (a) 与 (d)。

#### (c) `Settings.Global` 通道 + manifest opt-in（Android 10+ 的「donor app」机制）

Java 侧逻辑在 `android.os.GraphicsEnvironment`（AOSP `frameworks/base`）：

```java
private static final String METADATA_INJECT_LAYERS_ENABLE =
        "com.android.graphics.injectLayers.enable";

// Check whether application is has set the manifest metadata for layer injection.
private boolean canInjectLayers(ApplicationInfo ai) {
    return (ai.metaData != null && ai.metaData.getBoolean(METADATA_INJECT_LAYERS_ENABLE)
            && setInjectLayersPrSetDumpable());
}

private boolean debugLayerEnabled(Bundle coreSettings, String packageName, ApplicationInfo ai) {
    // Only enable additional debug functionality if the following conditions are met:
    // 1. App is debuggable or device is rooted or layer injection metadata flag is true
    // 2. ENABLE_GPU_DEBUG_LAYERS is true
    // 3. Package name is equal to GPU_DEBUG_APP
    if (!isDebuggable() && !canInjectLayers(ai)) return false;
    final int enable = coreSettings.getInt(Settings.Global.ENABLE_GPU_DEBUG_LAYERS, 0);
    if (enable == 0) return false;
    final String gpuDebugApp = coreSettings.getString(Settings.Global.GPU_DEBUG_APP, "");
    if (packageName == null || gpuDebugApp.isEmpty() || packageName.isEmpty()
            || !gpuDebugApp.equals(packageName)) return false;
    return true;
}

private void setupGpuLayers(Context context, Bundle coreSettings, PackageManager pm,
                            String packageName, ApplicationInfo ai) {
    final boolean enabled = debugLayerEnabled(coreSettings, packageName, ai);
    String layerPaths = "";
    if (enabled) {
        layerPaths = mLibraryPermittedPaths;
        final String layers = coreSettings.getString(Settings.Global.GPU_DEBUG_LAYERS);
        if (layers != null && !layers.isEmpty()) setDebugLayers(layers);   // -> native
        ...
    }
    // Include the app's lib directory in all cases
    layerPaths += mLibrarySearchPaths;
    setLayerPaths(mClassLoader, layerPaths);
}
```

以及 `GraphicsEnv.cpp` 里那个关键的一行：

```cpp
bool GraphicsEnv::setInjectLayersPrSetDumpable() {
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1) return false;
    return true;
}
```

**「donor app / 自愿 opt-in」的准确含义**：

* **要接收层**（target app）需要：
  * 自己可调试（`android:debuggable="true"`），**或**
  * 在 `AndroidManifest.xml` 里声明
    ```xml
    <meta-data android:name="com.android.graphics.injectLayers.enable"
               android:value="true"/>
    ```
    这个声明会让 `setInjectLayersPrSetDumpable()` 把本进程设成 dumpable，
    于是 `isDebuggable()` 变真、`/data/local/debug/vulkan` 与 layer 搜索路径开始生效。
* **提供层**（donor app）通过 `Settings.Global.GPU_DEBUG_LAYER_APP` 指定，
  其 `nativeLibraryDir` 会被加进层的搜索路径。
* 三个 Settings 都必须配好（用 `adb shell settings put global ...`，然后
  `am refresh-settings-cache`）：
  ```bash
  adb shell settings put global enable_gpu_debug_layers 1
  adb shell settings put global gpu_debug_app   <目标 app 包名>
  adb shell settings put global gpu_debug_layers VK_LAYER_EFML4A_model_replacer
  adb shell settings put global gpu_debug_layer_app <donor app 包名>   # 可选
  adb shell am refresh-settings-cache
  ```

> 📌 **对任务书表述的两点更正/补充**（都已核实）：
> 1. 「Android 11+ 应用可自愿 manifest opt-in」—— Vulkan 侧的 manifest opt-in 用的是
>    `com.android.graphics.injectLayers.enable`，而**不是** GLES 那套
>    `com.android.gles.debug.layer`；而且它与 (c) 的 `gpu_debug_*` Settings 是**同一套**
>    机制（CTS `hostsidetests/gputools` 同时覆盖 Vulkan 与 GLES，文件名
>    `CtsRootlessGpuDebugHostTest.java` 版权年为 2019 ⇒ 属于 Android 10 / API 29 时代）。
>    > ⚠️ 未核实：该 meta-data 具体在哪个 API level 首次引入，我没能从一手源码确定版本号。
> 2. 「donor app 机制」的作用是**把层从别的 app 的 native lib 目录借过来**，
>    它并**不**让层获得「跳过 debuggable 判定」的能力 —— target app 侧仍要满足
>    `isDebuggable() || canInjectLayers()`。CTS 的负面用例
>    `testInjectLayerLoadVulkan`（= `testLayerNotLoadedVulkan(INJECT_APP)`）明确断言
>    「不能把层 push 进 injectLayers app 的私有目录」。

#### (d) 应用私有 native lib 目录**永远**在搜索路径里

`setupGpuLayers()` 的最后一行是 `layerPaths += mLibrarySearchPaths;`，注释写着
"Include the app's lib directory in all cases"。这意味着：
**只要层 `.so` 随目标 app 的 APK 一起分发（作为 jniLib，名字 `libVkLayer*.so`），
再用 `debug.vulkan.layers`（或 `debug.vulkan.layer.<n>`）指定层名，即使 app 不可调试也能加载。**
CTS 的 `testSystemPropertyEnableVulkan` 就是拿**非**可调试的 RELEASE app +
「packaged with the RELEASE app」的 layerC 验证这条路径的。

### 7.4 针对本机（`user` + `release-keys` + 已 root）的实操结论

| 目标 app 状态 | 可行方案 |
|---|---|
| `android:debuggable="true"` | `install_layer.sh` 把层放进 `/data/local/debug/vulkan` + `setprop debug.vulkan.layers` 即可 |
| 不可调试，但能改它的 APK | 把 `libVkLayer_EFML4A.so` 作为 jniLib 打进去 + `setprop debug.vulkan.layers`。**但这会改变 APK 签名，无法覆盖安装** |
| 不可调试的**已安装**游戏 | 只能靠 root 侧手段（例如 Magisk/Zygisk 模块在目标 app 的 native lib 目录里放层、或改 app 的 dumpable 状态），AOSP 的 `gpu_debug_*` 通道对它无效，除非该 app 自己声明了 injectLayers |

> ⚠️ 未核实：在已 root 的 `user` 构建上，除了上面这些，是否还有厂商私有通道
> （小米/MIUI 特有；本机 `ro.build.tags=release-keys`）可以影响任意 app 的层注入。
> 这一点我没有一手资料。

---

### 7.5 运行时开关（系统属性）与落盘侦察

> **为什么必须用系统属性而不是环境变量**：安卓的 app 进程由 zygote fork/
> `exec` 出来，**不继承 shell 里的 `export`**（`setprop` / `am` / `ActivityManager`
> 都只传 `env` 以外的信息）。所以用 `export EFML4A_LOG_LEVEL=debug` 启游戏，
> 层在游戏进程里 `getenv()` 一定拿到 `NULL` —— 这就是「层加载成功但 logcat 里
> 什么都没有」的原因。属性区（`__system_property_get`）是所有进程共享、**可读**
> 的，因此属性是唯一可行的运行时通道；写属性需要 root。

#### 7.5.1 三个属性

| 属性 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `debug.efml4a.log_level` | `silent`/`off`/`error`/`warn`/`info`/`debug` 或数字 `0`~`4` | `info` | 层日志级别（digital 0=off,1=error,2=warn,3=info,4=debug） |
| `debug.efml4a.mode` | `off`/`recon`/`skip`/`donor`（大小写不敏感，前后空白忽略） | `off` | 运行模式 |
| `debug.efml4a.recon` | `1` | 未设置 | 兼容旧开关：等价于 `mode=recon` |
| `debug.efml4a.skip` | 数字 | `1` | `skip` 模式每个 submit 窗口跳过的绘制数 |
| `debug.efml4a.donor_max` | 数字 | `64` | `donor` 模式最多替换多少次 |

设置方式（root）：

```bash
adb shell su -c 'setprop debug.efml4a.log_level debug'
adb shell su -c 'setprop debug.efml4a.mode recon'
adb shell su -c 'getprop debug.efml4a.mode'      # 回读确认
adb logcat -s EFML4A:V                            # tag 固定 EFML4A
```

* **环境变量优先级更高**（`EFML4A_LOG_LEVEL` / `EFML4A_MODE` / `EFML4A_SKIP` /
  `EFML4A_DONOR_MAX`）。桌面/冒烟测试用环境变量注入，真机 app 进程只有属性；
  两者不冲突。
* **模式是随帧热更新的**：`vkQueueSubmit` 里会重新读一次属性，所以
  `setprop debug.efml4a.mode skip` 大约**一帧内**生效，不用重启游戏。
  每次模式发生变化（含首次读到非 `off`）都会打一行 INFO，含模式名与参数：
  `runtime mode = skip (skip_draws=1, donor_max=64) [EFML4A_MODE / debug.efml4a.mode]`。
* 属性写错（拼写错误/乱值）一律回落 `off`，**绝不因为属性写错就改变渲染行为**。

#### 7.5.2 输出文件的位置与格式

输出目录按下面的顺序解析（`src/recon.cpp` 的 `Candidates()`）：

1. `/data/data/<包名>/files/` —— 首选；包名从 `/proc/self/cmdline` 第一段解析
   （`:process` 后缀会被去掉；含 `/` 或非法字符则拒绝，见 `PackageFromCmdline()`）
2. `/data/data/<包名>/` —— 兜底（个别 app 还没有 `files/` 子目录）
3. `/sdcard/Android/data/<包名>/files` 与 `/storage/emulated/0/Android/data/<包名>/files`
   —— app 自己的**外置私有目录**（`getExternalFilesDir()`）。加这两条是为了兼容
   习惯从 `/sdcard/Android/data/<pkg>/files` 取产物的工具（本仓库的
   `scripts/game-test-cycle.sh` 就是）
4. `$EXTERNAL_STORAGE` —— 最末兜底；⚠️ 现代 Android 的 scoped storage 下这个值
   通常指向 `/sdcard`，app 对它的根目录**一般没有写权限**，所以这一条大概率失败

**只会写到第一个可写的目录**（不是同时写多处）。判定「可写」用的是
**真建一个探针文件再删掉**，而不是只信 `access()`：FUSE/sdcardfs 上
`access(W_OK)` 可能返回成功而 `open()` 仍被拒，只信 `access()` 会把
「首次写文件失败」升级成「永久禁用文件输出」。
所以：**想知道文件到底在哪，看那一行 `file output dir = ...` 的 INFO 日志。**

首次写文件前会打一行 INFO 说明最终选定的目录与完整路径：

```
[I] file output dir = /data/data/com.hypergryph.endfield/files (recon=..., stats=...)
```

两个文件（`0600`，属于游戏自己的 uid）：

* `efml4a-recon.jsonl` —— 侦察模式**追加**写，一行一个 JSON 对象（JSONL）。
  最多 **2000 行**，到顶后写一行 `{"recon":"capped"}` 并**永久停止**（不重试、不刷日志）。
  读取：`adb shell su -c 'cat /data/data/<pkg>/files/efml4a-recon.jsonl'`
* `efml4a-stats.json` —— **每 600 次 `vkQueueSubmit`** 覆盖写一次，`vkDestroyDevice`
  时再写一次。先写 `.tmp` 再 `rename`，所以读的人不会看到写了一半的 JSON。
  格式（counters 集合与 §4.1 一致，未改动）：

  ```json
  {"layer":"VK_LAYER_EFML4A_model_replacer","mode":"skip","counters":{"vkCreateInstance":1,...},
   "skip_draws":123,"donor_swaps":0,"recon_lines":0}
  ```

  即：`mode` 是当前模式，`skip_draws` / `donor_swaps` / `recon_lines` 是三个诊断计数
  —— 游戏不退出也能确认「模式到底起作用没有」。

侦察行字段（每行字段名固定，`pipeline`/buffer 是十六进制字符串）：

```json
{"seq":0,"call":"drawIndexed","indexCount":36,"firstIndex":0,"vertexOffset":0,
 "instanceCount":1,"firstInstance":0,"pipeline":"0x7f1234000","bindPoint":0,
 "ib_buffer":"0x7f1235000","ib_offset":0,"ib_type":0,"vb0_buffer":"0x7f1236000","vb0_offset":0}
```

* `vkCmdDraw` 没有索引概念：按约定把 `vertexCount`/`firstVertex` 填进
  `indexCount`/`firstIndex`，`vertexOffset` 记 `0`，`call` 为 `"draw"`。
* 从没绑过的字段是「哨兵值」：`bindPoint`/`ib_type` 为 `-1`，buffer 为 `0x0`。
* 状态是**按 `VkCommandBuffer` 分别记录**的（多线程并发录制不同命令缓冲时不会串）。
  ⚠️ 未拦截 `vkResetCommandBuffer` / `vkBeginCommandBuffer`（见 §9 的未验证清单），
  命令缓冲复用时旧状态可能残留到下一次录制；`vkFreeCommandBuffers` 已经清理，
  所以不会无界增长。

#### 7.5.3 四种模式

| 模式 | 行为 | 风险 |
|---|---|---|
| `off`（默认） | 只计数 + 原样转发 | 无 |
| `recon` | 逐 draw 写 JSONL，**只读**，不改任何 Vulkan 参数 | 无 |
| `skip` | 在 `vkCmdDraw*` 里直接 `return`（不转发给下层），丢弃每个 submit 窗口内的前 K 次绘制 | **零崩溃风险**（只是少下发一条命令），视觉上会稳定少一个部件 |
| `donor` | 把 `vkCmdBindVertexBuffers` 里 `firstBinding==0` 的 buffer 换成另一个「足够大」的顶点缓冲 | **有崩溃/花屏风险，见下** |

`skip` 的语义说明（工程判断，非任务书原文）：K 的计数**每个 submit 窗口重置一次**
（窗口 = 相邻两次 `vkQueueSubmit` 之间录制的 draw ≈ 一帧）。若改成「全进程只丢前 K 个
draw」，`K=1` 在一帧几千个 draw 里根本看不出来；按窗口重置后 `skip=1` 的效果是
「每帧丢掉第 1 个 draw」，视觉上稳定可辨，这才是能立刻确认「层在控制渲染」的证据。

`donor` 的安全条件（三条缺一不可，全部在 `src/hooks.cpp` 的 `PickDonor()` 里）：

1. **donor 的 `size >= 目标 buffer 的 size`**。这是唯一能保证顶点读取范围不越过缓冲
   末尾的条件（原绑定合法 ⇒ `offset + 读取长度 <= 目标 size <= donor.size`）。
2. donor 自己必须是 `VERTEX_BUFFER_BIT` 缓冲 —— 把 uniform/index 缓冲当顶点数据绑上
   去，GPU 读到垃圾，运气不好直接 device lost。
3. donor 必须与目标不是同一个；在候选集合里**轮转**（最近见过的优先），避免整屏都换成
   同一个缓冲导致完全无法判断。

另外：`debug.efml4a.donor_max`（默认 64）到顶后自动停止替换，全部原样转发。
`vkCmdBindVertexBuffers2`（Vulkan 1.3 动态状态）**未拦截** —— 如果游戏只用它绑定顶点缓冲，
`donor` 不会生效（`skip` 不受影响）。⚠️ 真机上 `donor` 崩溃风险仍然存在：
**顶点步长/格式与 donor 的数据布局不匹配**时，GPU 可能在缓冲内读到垃圾（这本身不崩），
但如果驱动做了越界预取或 app 未开启 robustBufferAccess，就可能 device lost。

#### 7.5.4 恢复默认

```bash
adb shell su -c 'setprop debug.efml4a.mode off'
adb shell su -c 'setprop debug.efml4a.log_level info'
adb shell su -c 'setprop debug.efml4a.skip 0'        # 只清参数：mode=off 时它们本来也不生效
```

---

## 8. 安装脚本

```bash
cd vulkan-layer
./layer/install_layer.sh --package com.example.game --abi arm64-v8a --yes
./layer/install_layer.sh --package com.example.game --layer-app com.example.donor
./layer/install_layer.sh --uninstall
```

脚本行为：

* **开头就会检测 `su` 是否可用，不可用直接退出**（并在注释与运行时都显式提示需要 root）；
* 打印设备的 `ro.build.type` / `ro.debuggable` / ABI，`ro.debuggable != 1` 时给出明确警告；
* `adb push` → `su -c cp` 到 `/data/local/debug/vulkan`，`chmod 644`；
* 两种启用方式 `--mode property|settings|both`（默认 both）；
* 结束时打印「怎么验证 / 怎么排查」清单（`adb logcat -s EFML4A:V` 等）；
* `--uninstall` 清理文件、`debug.vulkan.*` 属性与 `gpu_debug_*` Settings。

层自身的日志（`EFML4A_LOGI` 等）后端：链接到 `liblog` 时走 logcat（tag `EFML4A`），
否则退化为 stderr。日志级别默认 `info`，可用环境变量 `EFML4A_LOG_LEVEL` —— **但在真机
app 进程里环境变量不存在**，必须用系统属性 `debug.efml4a.log_level`，见 §7.5。


---

## 9. 已验证 / 未验证清单

**已在本机实际验证**：

* ✅ host 构建（logcat 后端 + 静态 libc++）：0 warning / 0 error
* ✅ 产物 `libVkLayer_EFML4A.so`（Android aarch64 ELF）与 `VkLayer_EFML4A.json`（合法 JSON）
* ✅ `readelf --dyn-syms` 确认 10 个必需符号全部导出
* ✅ `readelf -d` 确认 `libc++_shared.so` 依赖已消除（`DT_NEEDED` 只剩
  `liblog.so libdl.so libc.so libm.so`）
* ✅ 用 `-DEFML4A_VULKAN_HEADERS_URL=<codeload tarball>` 从零跑通了 FetchContent
  的 **真实下载** 路径（不只是 `SOURCE_DIR` 跳过下载的捷径）
* ✅ version script 生效：`readelf --dyn-syms` 导出 FUNC 符号恰好 10 个，
  无 `std::__ndk1::*` 泄漏
* ✅ CTest 冒烟测试 46 项断言全过（含端到端假 loader 的拦截/转发/链推进验证）
* ✅ Vulkan-Headers tag `vulkan-sdk-1.4.321.0` 与 target `Vulkan::Headers` 真实存在
* ✅ `vk_layer.h` 所有结构体/枚举/宏名字逐个核对
* ✅ manifest 字段名逐个核对（含 `disable_environment` 对 implicit layer 必需）
* ✅ Android 侧结论来自一手 AOSP 源码（`layers_extensions.cpp`、`api.cpp`、
  `GraphicsEnv.cpp`、`GraphicsEnvironment.java`、CTS `hostsidetests/gputools`）

**未验证**：

* ⚠️ Android NDK 交叉编译已留下产物（`build-android/`，NDK r29），但本次审计未重跑；
  `-static-libstdc++` / `android.toolchain.cmake` / ABI 分支未逐项复核
* ⚠️ 没有真机（Adreno）运行验证；没有实际被游戏 app 加载过
* ⚠️ Android 侧源码核对基于 AOSP 分支 **`a16`（Android 16）**，目标机是 **Android 17 / API 37**
* ⚠️ `com.android.graphics.injectLayers.enable` 首次引入的 API level
* ⚠️ 已 root 的 `user` 构建上是否存在厂商私有的层注入通道
* ⚠️ `debug.vulkan.layers` 在 Android 17 上是否仍然存在（读的是 a16 分支源码）

---

## 10. 下一步（真正实现换模型）

见 [`src/hooks.cpp`](src/hooks.cpp) 末尾的 `TODO(efml4a)` 区块，按依赖顺序：

1. **影子对象注册表**：拦截 `vkCreateBuffer` / `vkAllocateMemory` / `vkBindBufferMemory` /
   `vkGetBufferMemoryRequirements` / `vkMapMemory` / `vkUnmapMemory`，建
   `VkBuffer → {size, usage, memory, offset, mapped}` 表。
   注意 device-local 内存需要 staging copy + barrier，这是与 D3D11 最大的不同。
2. **按 command buffer 的绘制状态机**：现有 `Cmd*` hook 是无状态的，要记录
   `VkCommandBuffer → {pipeline, vertex buffers/offsets, index buffer, push constants}`，
   并补 `vkBeginCommandBuffer` / `vkEndCommandBuffer` / `vkResetCommandBuffer` /
   `vkFreeCommandBuffers` 来清理。
3. **draw 签名匹配**：把 3DMigoto 的 `(shader hash, indexCount, firstIndex)` 扩展成
   `(pipeline spir-v hash, indexCount, firstIndex, vertexOffset, instanceCount, bindingCount)`；
   现有 `HashSpirv()`（djb2）已经在 `vkCreateShaderModule` 里算好并打日志。
4. **贴图/描述符替换**（独立路径）：`vkCreateImageView` / `vkCreateSampler` /
   `vkUpdateDescriptorSets` + 在 `vkCmdBindDescriptorSets` 里换 descriptor set。
