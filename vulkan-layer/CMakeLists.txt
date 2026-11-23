# ============================================================================
# EFML4A Vulkan 隐式层骨架
#
# 构建（host，用于本地验证）：
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# 构建（Android NDK 交叉编译）：
#   cmake -S . -B build-android -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo
# ============================================================================
cmake_minimum_required(VERSION 3.22)

project(EFML4A_VulkanLayer
    VERSION 0.1.0
    DESCRIPTION "EFML4A model replacer - Vulkan implicit layer skeleton"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 编译选项
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# 默认隐藏所有符号，只导出显式标注 EFML4A_LAYER_EXPORT 的入口。
# 这对层来说很重要：避免本层符号污染宿主进程、或与驱动/其它层撞名。
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# 依赖：Khronos 官方 Vulkan-Headers（FetchContent）
# ---------------------------------------------------------------------------
# ⚠️ tag 名字核实：
#   任务书建议的 `v1.4.321` **不存在**。GIT_TAG 必须是 "vulkan-sdk-<major>.<minor>.<patch>.<build>"
#   形式。已用 `git ls-remote` / GitHub API 核实真实存在的 tag，例如：
#       vulkan-sdk-1.4.363.0, vulkan-sdk-1.4.357.0, vulkan-sdk-1.4.350.1,
#       vulkan-sdk-1.4.350.0, vulkan-sdk-1.4.341.0, vulkan-sdk-1.4.335.0,
#       vulkan-sdk-1.4.328.1, vulkan-sdk-1.4.328.0, vulkan-sdk-1.4.321.0, ...
#   这里选 vulkan-sdk-1.4.321.0（稳定、与 Android 侧镜像一致）。
#
# ⚠️ target 名字核实：Vulkan-Headers 的 CMakeLists.txt 里有
#       add_library(Vulkan-Headers INTERFACE)
#       add_library(Vulkan::Headers ALIAS Vulkan-Headers)
#   所以 `Vulkan::Headers` 确实存在，可以直接 target_link_libraries。
#
# 受限网络 / 离线构建：
#   github.com:443 在部分网络环境会被墙（本机就是如此）。此时有两种绕过方式：
#     1) 指向镜像：  -DEFML4A_VULKAN_HEADERS_REPO=https://<mirror>/Vulkan-Headers.git
#     2) 手工下载官方 tarball 后指定源码目录（FetchContent 会跳过下载步骤）：
#          curl -L -o vh.tar.gz \
#            https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/vulkan-sdk-1.4.321.0
#          tar xzf vh.tar.gz
#          cmake -S . -B build -DEFML4A_VULKAN_HEADERS_SOURCE_DIR=$PWD/Vulkan-Headers-vulkan-sdk-1.4.321.0
# ---------------------------------------------------------------------------
include(FetchContent)

set(EFML4A_VULKAN_HEADERS_REPO
    "https://github.com/KhronosGroup/Vulkan-Headers.git"
    CACHE STRING "Git repository for KhronosGroup/Vulkan-Headers")
set(EFML4A_VULKAN_HEADERS_TAG
    "vulkan-sdk-1.4.321.0"
    CACHE STRING "Git tag for KhronosGroup/Vulkan-Headers (must exist!)")
set(EFML4A_VULKAN_HEADERS_SOURCE_DIR
    ""
    CACHE PATH "Already-downloaded Vulkan-Headers source tree (skips the download)")

if(EFML4A_VULKAN_HEADERS_SOURCE_DIR)
    # FetchContent 原生支持的覆盖变量：设置后完全跳过 download/update 步骤。
    set(FETCHCONTENT_SOURCE_DIR_VULKANHEADERS
        "${EFML4A_VULKAN_HEADERS_SOURCE_DIR}"
        CACHE PATH "Vulkan-Headers source dir override" FORCE)
    message(STATUS "EFML4A: using local Vulkan-Headers at ${EFML4A_VULKAN_HEADERS_SOURCE_DIR}")
else()
    message(STATUS "EFML4A: fetching Vulkan-Headers ${EFML4A_VULKAN_HEADERS_TAG} from ${EFML4A_VULKAN_HEADERS_REPO}")
endif()

# 只想要头文件，不要 Vulkan-Headers 自己的 tests / install 规则。
set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)

# 可选：Vulkan-Headers 的 tarball URL。当 git 访问 github.com 被墙、但
# codeload.github.com 可达时用这条路径（会真正走 FetchContent 的下载流程）。
# ⚠️ CACHE STRING 的文档字符串必须是**一个**字符串字面量：写成多个相邻字面量
#    会被当成列表并用 ';' 连接，最终 URL 变成垃圾值。
set(EFML4A_VULKAN_HEADERS_URL
    ""
    CACHE STRING "Optional tarball URL for Vulkan-Headers (use when git access to github.com is blocked)")

if(EFML4A_VULKAN_HEADERS_URL)
    # 走 URL 而不是 GIT_REPOSITORY。本机实测 github.com:443 被墙、
    # codeload.github.com 可达，因此这条路径能真正跑通 FetchContent 的下载。
    message(STATUS "EFML4A: fetching Vulkan-Headers from URL ${EFML4A_VULKAN_HEADERS_URL}")
    FetchContent_Declare(VulkanHeaders
        URL "${EFML4A_VULKAN_HEADERS_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
else()
    FetchContent_Declare(VulkanHeaders
        GIT_REPOSITORY "${EFML4A_VULKAN_HEADERS_REPO}"
        GIT_TAG        "${EFML4A_VULKAN_HEADERS_TAG}"
        GIT_SHALLOW    FALSE
        GIT_PROGRESS   TRUE)
endif()

FetchContent_MakeAvailable(VulkanHeaders)

if(NOT TARGET Vulkan::Headers)
    message(FATAL_ERROR
        "Vulkan::Headers target not found after FetchContent_MakeAvailable(VulkanHeaders). "
        "Check EFML4A_VULKAN_HEADERS_SOURCE_DIR / _REPO / _TAG.")
endif()

# ---------------------------------------------------------------------------
# 层本体
# ---------------------------------------------------------------------------
add_library(EFML4A_VkLayer SHARED
    src/layer.cpp
    src/dispatch.cpp
    src/hooks.cpp
    src/log.cpp
    # src/mode.cpp  —— 运行时模式（off/recon/skip/donor）解析与缓存
    # src/recon.cpp —— 落盘侦察 / 统计落盘（输出目录解析、JSONL 追加）
    src/mode.cpp
    src/recon.cpp)

target_include_directories(EFML4A_VkLayer
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# 只需要头文件；层**不能**链接 libvulkan —— 所有 Vulkan 入口都是运行时
# 通过 loader 交给我们的函数指针拿到的。
target_link_libraries(EFML4A_VkLayer PRIVATE Vulkan::Headers)

# 产物名：libVkLayer_EFML4A.so
# ⚠️ "libVkLayer" 前缀在 Android 上是**硬性要求**：AOSP
#    layers_extensions.cpp 的 DiscoverLayersInPathList() 只认
#      StartsWith(filename, "libVkLayer") && EndsWith(filename, ".so")
#    不满足的文件名会被直接忽略。
set_target_properties(EFML4A_VkLayer PROPERTIES
    OUTPUT_NAME "VkLayer_EFML4A"
    PREFIX "lib"
    CXX_VISIBILITY_PRESET hidden)

# 缺符号要立刻报错，而不是留到运行时 dlopen 才炸。
target_link_options(EFML4A_VkLayer PRIVATE "$<$<PLATFORM_ID:Linux,Android>:-Wl,--no-undefined>")

# ---------------------------------------------------------------------------
# 静态链接 C++ 运行库
# ---------------------------------------------------------------------------
# ⚠️ 这不是「可选优化」，而是隐式层能否被注入的**硬性条件**之一。
# AOSP frameworks/native/vulkan/libvulkan/layers_extensions.cpp 的
# LayerLibrary::Open() 里有一条明确注释：
#
#     // Libraries in the system layer library dir can't be loaded into
#     // the application namespace. That causes compatibility problems, since
#     // any symbol dependencies will be resolved by system libraries. They
#     // can't safely use libc++_shared, for example. Which is one reason
#     // (among several) we only allow them in non-user builds.
#
# 也就是说：层被 dlopen 进**应用的 linker namespace** 时，libc++_shared.so
# 很可能不在该 namespace 的搜索路径里，层会因为找不到符号而加载失败。
# 静态链接 libc++ 可以彻底消除这个依赖。
# 实测（本机 Termux host 构建）：加该选项后 DT_NEEDED 从
#     liblog.so libdl.so libc.so libc++_shared.so libm.so
# 变成
#     liblog.so libdl.so libc.so libm.so
option(EFML4A_STATIC_LIBCXX "Statically link the C++ runtime into the layer" ON)
if(EFML4A_STATIC_LIBCXX)
    target_link_options(EFML4A_VkLayer PRIVATE -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# 导出符号白名单（version script）
# ---------------------------------------------------------------------------
# 静态链进来的 libc++ 符号默认是 default visibility，`-fvisibility=hidden` 管不到，
# 于是层会导出几百个 std::__ndk1::* 符号 —— 相当于把 libc++ 注入宿主进程，
# 若目标 app 自己也用 libc++_shared 就可能发生符号插入冲突。
# 这里用白名单只导出层的 ABI 入口；`--exclude-libs,ALL` 作为第二道保险，
# 把来自静态库的符号一并隐藏。
# ⚠️ 实测：加白名单前 `readelf --dyn-syms` 有 ~400 个 std::__ndk1::* 符号；
#    加之后只剩下面 10 个 + 编译器生成的少量符号。
target_link_options(EFML4A_VkLayer PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer/efml4a_layer.map"
    "-Wl,--exclude-libs,ALL")

find_package(Threads REQUIRED)
target_link_libraries(EFML4A_VkLayer PRIVATE Threads::Threads)

# log 后端：Android/bionic 下用 __android_log_print（需要 liblog）。
# ⚠️ 这里不能用 find_library(NAMES log)：本机（Termux）上 liblog 不在常规链接
#    搜索路径里，find_library 找不到它，但编译器驱动加 -llog 却能链接成功
#    （clang 为 Android target 内置了系统库搜索路径）。所以用「试链接」来探测。
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES log)
check_cxx_source_compiles("
#include <android/log.h>
int main() { __android_log_print(4, \"EFML4A\", \"%s\", \"probe\"); return 0; }
" EFML4A_HAVE_ANDROID_LOG)
unset(CMAKE_REQUIRED_LIBRARIES)

if(EFML4A_HAVE_ANDROID_LOG)
    target_link_libraries(EFML4A_VkLayer PRIVATE log)
    message(STATUS "EFML4A: android log backend enabled (-llog links -> logcat)")
else()
    message(STATUS "EFML4A: android log backend unavailable, log.cpp falls back to stderr")
endif()

# ---------------------------------------------------------------------------
# host / Android NDK 交叉编译 差异
# ---------------------------------------------------------------------------
# ⚠️ 为什么判别条件是 CMAKE_CROSSCOMPILING 而不是 if(ANDROID)：
#   在 Termux 下 CMake 会把 **host 自身** 也识别成 Android —— Termux 的 uname
#   报告 "Android"，于是 CMAKE_HOST_SYSTEM_NAME=Android、CMAKE_SYSTEM_NAME=Android、
#   ANDROID=1。但此时 CMAKE_CROSSCOMPILING 为 FALSE，产出的二进制可以在本机直接运行
#   （本机就是 Android/bionic），所以 host 冒烟测试仍然应该构建并运行。
#   而真正的 NDK 交叉编译（-DCMAKE_TOOLCHAIN_FILE=android.toolchain.cmake）会令
#   CMAKE_CROSSCOMPILING=TRUE。
#   结论：「是否交叉编译」才是正确的判别条件。
if(CMAKE_CROSSCOMPILING)
    message(STATUS "EFML4A: cross-compiling for ${CMAKE_SYSTEM_NAME} "
                   "ABI=${ANDROID_ABI} platform=${ANDROID_PLATFORM}")

    # Android loader 按文件名在标准目录里找层，因此 manifest 里只写文件名即可。
    set(EFML4A_LAYER_LIBRARY_PATH "libVkLayer_EFML4A.so")
    set(EFML4A_LAYER_MANIFEST_DIR "etc/vulkan/implicit_layer.d")

else()
    message(STATUS "EFML4A: local build (system=${CMAKE_SYSTEM_NAME}, "
                   "host=${CMAKE_HOST_SYSTEM_NAME})")

    # 本地构建时 manifest 里写绝对路径，方便 loader 直接找到刚构建出来的 .so。
    set(EFML4A_LAYER_LIBRARY_PATH "${CMAKE_CURRENT_BINARY_DIR}/libVkLayer_EFML4A.so")
    set(EFML4A_LAYER_MANIFEST_DIR "share/vulkan/implicit_layer.d")

    target_link_libraries(EFML4A_VkLayer PRIVATE ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# layer manifest（@EFML4A_LAYER_LIBRARY_PATH@ 由 configure_file 替换）
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/layer/VkLayer_EFML4A.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_EFML4A.json"
    @ONLY)

message(STATUS "EFML4A: layer manifest -> ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_EFML4A.json")
message(STATUS "EFML4A: layer library_path = ${EFML4A_LAYER_LIBRARY_PATH}")

# ---------------------------------------------------------------------------
# 安装规则
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS EFML4A_VkLayer
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_EFML4A.json"
    DESTINATION "${EFML4A_LAYER_MANIFEST_DIR}")

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
# host 冒烟测试需要能 dlopen 目标文件并运行，交叉编译时无意义 -> 关掉。
if(CMAKE_CROSSCOMPILING)
    set(EFML4A_BUILD_TESTS_DEFAULT OFF)
else()
    set(EFML4A_BUILD_TESTS_DEFAULT ON)
endif()
option(EFML4A_BUILD_TESTS "Build the host smoke test" ${EFML4A_BUILD_TESTS_DEFAULT})

if(EFML4A_BUILD_TESTS)
    enable_testing()

    add_executable(efml4a_smoke_host tests/smoke_host.cpp)
    target_link_libraries(efml4a_smoke_host PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    add_dependencies(efml4a_smoke_host EFML4A_VkLayer)

    # 把层 .so 的绝对路径作为 argv[1] 传给测试（用 $<TARGET_FILE:...> 保证正确）。
    add_test(NAME efml4a_smoke_host
        COMMAND efml4a_smoke_host "$<TARGET_FILE:EFML4A_VkLayer>")

    set_tests_properties(efml4a_smoke_host PROPERTIES
        # 注意：这里必须写 "FAILED:" 而不是 "FAILED" —— 否则会匹配到
        # VK_ERROR_INITIALIZATION_FAILED 这类合法输出，导致测试永远失败。
        FAIL_REGULAR_EXPRESSION "FAILED:"
        PASS_REGULAR_EXPRESSION "SMOKE TEST PASSED"
        TIMEOUT 60)

    # -----------------------------------------------------------------------
    # 纯函数单测：包名解析（recon）+ 模式解析（mode）+ JSONL/统计落盘
    # -----------------------------------------------------------------------
    # 为什么直接编译源文件而不是 dlopen 层 .so：
    #   层用 -fvisibility=hidden + version script 只导出 ABI 入口，
    #   PackageFromCmdline / ParseMode 这类内部函数在 .so 里是 local 符号，
    #   dlsym 取不到。直接编进测试可执行文件是最简单可靠的办法（也不需要 Vulkan）。
    #
    # 仅宿主机构建时启用：这里沿用与上面 smoke test 完全相同的判别方式
    # （EFML4A_BUILD_TESTS 的默认值来自 CMAKE_CROSSCOMPILING），
    # 交叉编译时默认不构建、不影响 Android 产物。
    add_executable(efml4a_test_pure
        tests/test_pure.cpp
        src/mode.cpp
        src/recon.cpp
        src/log.cpp)

    target_include_directories(efml4a_test_pure
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

    # recon.cpp / mode.cpp 用 std::mutex；log.cpp 在 Android 后端下需要 liblog。
    target_link_libraries(efml4a_test_pure PRIVATE Threads::Threads)
    if(EFML4A_HAVE_ANDROID_LOG)
        target_link_libraries(efml4a_test_pure PRIVATE log)
    endif()

    add_test(NAME efml4a_test_pure COMMAND efml4a_test_pure)

    set_tests_properties(efml4a_test_pure PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAILED:"
        PASS_REGULAR_EXPRESSION "PURE TESTS PASSED"
        TIMEOUT 60)
endif()
