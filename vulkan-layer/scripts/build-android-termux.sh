#!/data/data/com.termux/files/usr/bin/bash
# 在 Android 设备上用 Termux 原生工具链交叉编译 EFML4A Vulkan 隐式层（arm64）。
#
# 为什么不用 NDK 自带的 CMake 工具链：
#   NDK 的 toolchains/llvm/prebuilt/linux-x86_64/ 里全是 **x86-64 二进制**，
#   在 arm64 的 Termux 上无法执行（Exec format error）。但 NDK 的 **sysroot 只是数据**
#   （头文件 + 静态库），所以正解是：
#       Termux 自带的 clang（本身就是 aarch64-unknown-linux-android 目标）
#     + NDK 的 sysroot（提供 bionic 头文件与 libc++ 静态库）
#
# 两个必须踩过的坑（否则链接一定失败）：
#   1. **静态库必须放在目标文件之后**。`libc++_static.a` 放前面会导致
#      `undefined symbol: operator new / operator delete`。
#   2. `libc++abi.a` 内部会用 `-l:libunwind.a` 请求 unwind 库，必须用 `-L`
#      把 clang 的 resource lib 目录（lib/clang/<ver>/lib/linux/aarch64）加进搜索路径。
#
# 产物只依赖 liblog/libdl/libm/libc，**不依赖 libc++_shared.so**，
# 因此可以干净地加载进任意 Android 应用进程。
set -euo pipefail

ANDROID_HOME="${ANDROID_HOME:-$HOME/android-sdk}"
NDK="${EFML4A_NDK:-$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)}"
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "找不到 NDK，请设置 EFML4A_NDK 或 ANDROID_HOME" >&2; exit 1; }

PRE="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="$PRE/sysroot"
LIBDIR="$SYSROOT/usr/lib/aarch64-linux-android"
CLANG_VER="$(ls "$PRE/lib/clang" | sort -V | tail -1)"
CLANGLIB="$PRE/lib/clang/$CLANG_VER/lib/linux/aarch64"
API="${EFML4A_API:-29}"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/vulkan-layer"
OUT="${EFML4A_OUT:-$ROOT/vulkan-layer/build-android}"

# Vulkan 头文件：优先用本地缓存/NDK sysroot，避免联网。
HDRS="${EFML4A_VULKAN_HEADERS:-}"
for cand in "$HOME/.cache/efml4a/Vulkan-Headers-1.4.321.0" \
            "$ROOT/vulkan-layer/build/_deps/vulkanheaders-src"; do
    [ -n "$HDRS" ] && break
    [ -f "$cand/include/vulkan/vk_layer.h" ] && HDRS="$cand"
done
INCS="-I$SRC/include"
[ -n "$HDRS" ] && INCS="$INCS -I$HDRS/include" || INCS="$INCS -I$SYSROOT/usr/include"

FLAGS="--target=aarch64-linux-android$API --sysroot=$SYSROOT -fPIC -O2 -std=c++17 -Wall -Wextra -fvisibility=hidden"

mkdir -p "$OUT"
echo "[build] NDK      = $NDK"
echo "[build] toolchain= $(clang --version | head -1)"
echo "[build] headers  = ${HDRS:-NDK sysroot}"

OBJS=()
for f in layer dispatch hooks log mode recon; do
    echo "[build] 编译 $f.cpp"
    clang++ $FLAGS $INCS -c "$SRC/src/$f.cpp" -o "$OUT/$f.o"
    OBJS+=("$OUT/$f.o")
done

echo "[build] 链接 libVkLayer_EFML4A.so"
clang++ --target="aarch64-linux-android$API" --sysroot="$SYSROOT" -shared \
    -Wl,--no-undefined -Wl,-soname,libVkLayer_EFML4A.so \
    "${OBJS[@]}" \
    -L"$CLANGLIB" -nostdlib++ \
    "$LIBDIR/libc++_static.a" "$LIBDIR/libc++abi.a" \
    -llog -o "$OUT/libVkLayer_EFML4A.so"

echo "[build] 完成：$OUT/libVkLayer_EFML4A.so"
readelf -d "$OUT/libVkLayer_EFML4A.so" | grep NEEDED || true
