#!/data/data/com.termux/files/usr/bin/bash
# 在设备上构建并运行 Vulkan 层探针，验证「加载 → 拦截 → 转发」链路。
#
# 前置：先跑 build-android-termux.sh 生成 libVkLayer_EFML4A.so
#      再跑 install-implicit-layer.sh 安装层（需要 root）
#
# 用法： run-ondevice-probe.sh [stage]     stage 默认 2
#   stage 1  设备级（instance/device/buffer/memory/pool）
#   stage 2  + 顶点/索引缓冲绑定 —— **换模型的核心机制**
#   stage 3  + vkCmdDrawIndexed（⚠️ 无 pipeline 会崩 Adreno 驱动，默认不跑）
set -euo pipefail

STAGE="${1:-2}"
ANDROID_HOME="${ANDROID_HOME:-$HOME/android-sdk}"
NDK="${EFML4A_NDK:-$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)}"
PRE="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="$PRE/sysroot"
LIBDIR="$SYSROOT/usr/lib/aarch64-linux-android"
CLANGLIB="$PRE/lib/clang/$(ls "$PRE/lib/clang" | sort -V | tail -1)/lib/linux/aarch64"
API="${EFML4A_API:-29}"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${EFML4A_OUT:-$ROOT/vulkan-layer/build-android}"
HDRS="${EFML4A_VULKAN_HEADERS:-$HOME/.cache/efml4a/Vulkan-Headers-1.4.321.0}"
mkdir -p "$OUT"

echo "[probe] 构建 efml_probe"
clang++ --target="aarch64-linux-android$API" --sysroot="$SYSROOT" -O2 -std=c++17 -pie -fPIE \
    -I"$HDRS/include" \
    -L"$CLANGLIB" -nostdlib++ "$LIBDIR/libc++_static.a" "$LIBDIR/libc++abi.a" \
    "$ROOT/vulkan-layer/tests/probe_android.cpp" -lvulkan -llog -o "$OUT/efml_probe"

echo "[probe] 临时启用层属性（运行结束后自动清空）"
su -c 'setprop debug.vulkan.layers VK_LAYER_EFML4A_model_replacer'
trap 'su -c "setprop debug.vulkan.layers \"\"" || true' EXIT

cd "$OUT"
set +e
./efml_probe "$STAGE"
rc=$?
set -e
echo "[probe] 退出码=$rc"
[ -f efml4a-layer-stats.json ] && { echo "[probe] 层统计已写入 $OUT/efml4a-layer-stats.json"; cat efml4a-layer-stats.json; }
