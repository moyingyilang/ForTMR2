#!/usr/bin/env bash
# ============================================================================
# EFML4A Vulkan 隐式层 —— Android 安装 / 启用脚本
# ============================================================================
#
# ⚠️⚠️  需要 root  ⚠️⚠️
#   本脚本会通过 `su -c` 往 /data/local/debug/vulkan 写文件、并 setprop
#   debug.vulkan.layers。**没有 root 就什么都做不了**（脚本会在开始时检测并退出）。
#
# 另外请务必理解「能被注入」的前提（已核实，见 README「Android 侧 manifest 与
# 启用方式」一节）：
#   1) /data/local/debug/vulkan 这条路径只有当**目标 app 自己**满足
#      GraphicsEnv::isDebuggable() 时才会被扫描：
#          isDebuggable() == (prctl(PR_GET_DUMPABLE) > 0) || ANDROID_DEBUGGABLE
#      对第三方 app 来说，等价于「app 的 manifest 里有 android:debuggable="true"」
#      或者在 manifest 里声明了 com.android.graphics.injectLayers.enable（见下）。
#      ro.build.type=user 的设备上 ANDROID_DEBUGGABLE 为假，所以「userdebug 自带
#      root」这条捷径**不存在**。
#   2) 目标 app 是**不可调试**的话，只有两条路：
#        a) 它自己的 APK 里就打包了 libVkLayer*.so（应用私有 native lib 目录
#           永远在层的搜索路径里），再用 debug.vulkan.layers / 
#           debug.vulkan.layer.<n> 指定层名；
#        b) 用「donor app」机制：目标 app 在 manifest 里声明
#           <meta-data android:name="com.android.graphics.injectLayers.enable"
#                      android:value="true"/>
#           并把 Settings.Global 的 enable_gpu_debug_layers / gpu_debug_app /
#           gpu_debug_layers / gpu_debug_layer_app 配好。
#
# 用法：
#   ./install_layer.sh --package com.example.game [选项]
#
# 选项：
#   --build-dir DIR    层 .so / .json 所在目录（默认 ../build）
#   --abi ABI          arm64-v8a | armeabi-v7a | x86_64（默认 arm64-v8a）
#   --serial SERIAL    adb -s 指定设备
#   --package PKG      目标 app 包名（gpu_debug_app）
#   --layer-app PKG    提供 layer .so 的外部 app 包名（gpu_debug_layer_app，可选）
#   --mode MODE        property | settings | both（默认 both）
#   --uninstall        卸载 / 关闭
#   --yes              跳过交互确认
#   -h | --help        帮助
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
ABI="arm64-v8a"
SERIAL=""
PACKAGE=""
LAYER_APP=""
MODE="both"
UNINSTALL=0
ASSUME_YES=0

LAYER_NAME="VK_LAYER_EFML4A_model_replacer"
LAYER_SO="libVkLayer_EFML4A.so"
LAYER_JSON="VkLayer_EFML4A.json"
DEVICE_DIR="/data/local/debug/vulkan"
TMP_DIR="/data/local/tmp"

usage() { sed -n '2,60p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --abi)       ABI="$2"; shift 2 ;;
        --serial)    SERIAL="$2"; shift 2 ;;
        --package)   PACKAGE="$2"; shift 2 ;;
        --layer-app) LAYER_APP="$2"; shift 2 ;;
        --mode)      MODE="$2"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        --yes|-y)    ASSUME_YES=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done

case "$ABI" in
    arm64-v8a|armeabi-v7a|x86|x86_64) ;;
    *) echo "错误：不支持的 ABI '$ABI'" >&2; exit 2 ;;
esac

if [[ ! -f "${BUILD_DIR}/${LAYER_SO}" ]]; then
    echo "错误：找不到 ${BUILD_DIR}/${LAYER_SO}" >&2
    echo "请先构建：cmake -S . -B build -G Ninja && cmake --build build" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# adb 封装
# ---------------------------------------------------------------------------
adb_cmd() {
    if [[ -n "$SERIAL" ]]; then adb -s "$SERIAL" "$@"; else adb "$@"; fi
}
# 在设备上以 root 身份执行一条命令（整条命令作为一个字符串交给 su -c）
su_sh() {
    local cmd="$1"
    if [[ -n "$SERIAL" ]]; then adb -s "$SERIAL" shell su -c "$cmd"; else adb shell su -c "$cmd"; fi
}

echo "=============================================================="
echo " EFML4A Vulkan 隐式层安装 / 启用"
echo "=============================================================="
echo " 层名字        : ${LAYER_NAME}"
echo " 层库          : ${BUILD_DIR}/${LAYER_SO}"
echo " 目标 ABI      : ${ABI}"
echo " 目标目录      : ${DEVICE_DIR}"
echo " 目标 app      : ${PACKAGE:-<未指定，需要 --package 才能用 settings 方式启用>}"
echo " 外部 layer app: ${LAYER_APP:-<未指定>}"
echo " 启用方式      : ${MODE}"
echo

if ! command -v adb >/dev/null 2>&1; then
    echo "错误：找不到 adb（Android 上没有 adb？请从 PC 上运行本脚本）。" >&2
    exit 2
fi

adb_cmd get-state >/dev/null 2>&1 || { echo "错误：adb 连不上设备/模拟器。" >&2; exit 2; }

# ---------------------------------------------------------------------------
# 设备状态检查（只做提示，不阻断）
# ---------------------------------------------------------------------------
BUILD_TYPE="$(adb_cmd shell getprop ro.build.type 2>/dev/null | tr -d '\r')"
DEBUGGABLE="$(adb_cmd shell getprop ro.debuggable 2>/dev/null | tr -d '\r')"
echo "[设备] ro.build.type   = ${BUILD_TYPE:-?}"
echo "[设备] ro.debuggable   = ${DEBUGGABLE:-?}"
echo "[设备] ro.product.cpu.abi = $(adb_cmd shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')"
if [[ "$DEBUGGABLE" != "1" ]]; then
    echo
    echo "⚠️  该设备 ro.debuggable != 1（不是 userdebug/eng 构建）。这意味着："
    echo "    - /data/local/debug/vulkan 只会对**可调试的目标 app**（或声明了"
    echo "      com.android.graphics.injectLayers.enable 的 app）生效；"
    echo "    - 对普通不可调试的 app，单靠 setprop / settings **不会**被注入。"
fi
echo

# ---------------------------------------------------------------------------
# root 检测
# ---------------------------------------------------------------------------
echo "[1/4] 检测 root ..."
if ! su_sh "id" >/dev/null 2>&1; then
    echo
    echo "❌ 检测不到 root（su 不可用）。本脚本必须 root。"
    echo "   如果你只是想手动操作，请自行以 root 身份执行 README 里的命令。"
    exit 1
fi
echo "     ok: 设备上 su 可用（$(su_sh 'id -u' | tr -d '\r')）"
echo

# ---------------------------------------------------------------------------
# 卸载
# ---------------------------------------------------------------------------
if [[ "$UNINSTALL" == "1" ]]; then
    echo "[2/4] 卸载 / 关闭 ..."
    su_sh "rm -f ${DEVICE_DIR}/${LAYER_SO} ${DEVICE_DIR}/${LAYER_JSON}" || true
    su_sh "setprop debug.vulkan.layers ''" || true
    for p in 0 1 2 3 4 5 6 7 8 9; do
        su_sh "setprop debug.vulkan.layer.${p} ''" || true
    done
    adb_cmd shell settings delete global enable_gpu_debug_layers >/dev/null 2>&1 || true
    adb_cmd shell settings delete global gpu_debug_app >/dev/null 2>&1 || true
    adb_cmd shell settings delete global gpu_debug_layers >/dev/null 2>&1 || true
    adb_cmd shell settings delete global gpu_debug_layer_app >/dev/null 2>&1 || true
    adb_cmd shell am refresh-settings-cache >/dev/null 2>&1 || true
    echo "     已清理 layer 文件、debug.vulkan.* 属性与 gpu_debug_* settings。"
    echo "     注意：需要重启目标 app 才彻底生效。"
    exit 0
fi

if [[ "$ASSUME_YES" != "1" ]]; then
    echo "⚠️  即将以 root 身份写入 ${DEVICE_DIR} 并修改系统属性/Settings。"
    read -r -p "    确认继续？[y/N] " reply
    [[ "$reply" =~ ^[Yy]$ ]] || { echo "已取消。"; exit 0; }
    echo
fi

# ---------------------------------------------------------------------------
# 推送
# ---------------------------------------------------------------------------
echo "[2/4] 推送到设备 ..."
adb_cmd push "${BUILD_DIR}/${LAYER_SO}" "${TMP_DIR}/${LAYER_SO}"
if [[ -f "${BUILD_DIR}/${LAYER_JSON}" ]]; then
    adb_cmd push "${BUILD_DIR}/${LAYER_JSON}" "${TMP_DIR}/${LAYER_JSON}"
else
    echo "     提示：没有找到 ${LAYER_JSON}（Android 侧本来也不读 manifest，可忽略）"
fi
echo

# ---------------------------------------------------------------------------
# 安装到系统层目录
# ---------------------------------------------------------------------------
echo "[3/4] 安装到 ${DEVICE_DIR} ..."
# ⚠️ 目录必须是 /data/local/debug/vulkan，且文件名必须以 libVkLayer 开头、以 .so 结尾，
#    否则 AOSP layers_extensions.cpp 的 DiscoverLayersInPathList() 会直接忽略它：
#        if (StartsWith(filename, "libVkLayer") && EndsWith(filename, ".so"))
su_sh "mkdir -p ${DEVICE_DIR}"
su_sh "cp ${TMP_DIR}/${LAYER_SO} ${DEVICE_DIR}/${LAYER_SO}"
if [[ -f "${BUILD_DIR}/${LAYER_JSON}" ]]; then
    su_sh "cp ${TMP_DIR}/${LAYER_JSON} ${DEVICE_DIR}/${LAYER_JSON}"
fi
# 644 + 目录 755：层是被 dlopen 的，不需要可执行位。
su_sh "chmod 644 ${DEVICE_DIR}/${LAYER_SO}"
su_sh "chmod 755 ${DEVICE_DIR}"
adb_cmd shell rm -f "${TMP_DIR}/${LAYER_SO}" "${TMP_DIR}/${LAYER_JSON}" >/dev/null 2>&1 || true
echo "     已安装："
su_sh "ls -laZ ${DEVICE_DIR}" || su_sh "ls -la ${DEVICE_DIR}"
echo

# ---------------------------------------------------------------------------
# 启用
# ---------------------------------------------------------------------------
echo "[4/4] 启用层 ..."

if [[ "$MODE" == "property" || "$MODE" == "both" ]]; then
    # debug.vulkan.layers 的值是**冒号分隔的层名字**（不是路径！）。
    # 已核实：frameworks/native/vulkan/libvulkan/api.cpp 的 ParseDebugVulkanLayers()
    #         property_get("debug.vulkan.layers", prop, "") 然后按 ':' 切分。
    # 它是「legacy properties」回退路径：只在 Settings 没有提供层时才生效。
    su_sh "setprop debug.vulkan.layers ${LAYER_NAME}"
    echo "     setprop debug.vulkan.layers = $(adb_cmd shell getprop debug.vulkan.layers | tr -d '\r')"
    # 另一条等价的、更细粒度的属性通道（优先级 0..9 固定区间后连续扫描）：
    # su_sh "setprop debug.vulkan.layer.0 ${LAYER_NAME}"
fi

if [[ "$MODE" == "settings" || "$MODE" == "both" ]]; then
    if [[ -z "$PACKAGE" ]]; then
        echo "     跳过 settings 方式：没有 --package（gpu_debug_app 必填）"
    else
        adb_cmd shell settings put global enable_gpu_debug_layers 1
        adb_cmd shell settings put global gpu_debug_app "$PACKAGE"
        adb_cmd shell settings put global gpu_debug_layers "$LAYER_NAME"
        if [[ -n "$LAYER_APP" ]]; then
            adb_cmd shell settings put global gpu_debug_layer_app "$LAYER_APP"
        fi
        adb_cmd shell am refresh-settings-cache >/dev/null 2>&1 || true
        echo "     settings: enable_gpu_debug_layers=1 gpu_debug_app=${PACKAGE}"
        echo "               gpu_debug_layers=${LAYER_NAME}" \
             "${LAYER_APP:+gpu_debug_layer_app=${LAYER_APP}}"
        echo "     注意：这条通道要求目标 app 可调试，或者声明了"
        echo "           com.android.graphics.injectLayers.enable（见 README）。"
    fi
fi
echo

cat <<'TIP'
--------------------------------------------------------------
接下来怎么验证：

  # 1) 看层有没有被 loader 加载（本层日志 tag 固定为 EFML4A）
  adb logcat -s EFML4A:V

  # 2) 强制停掉目标 app 再启动（层只在 app 启动时注入）
  adb shell am force-stop <包名>
  adb shell monkey -p <包名> -c android.intent.category.LAUNCHER 1

  # 3) 如果什么都没打印，按顺序排查：
  #    - 目标 app 是否 debuggable / 是否声明了 injectLayers metadata？
  #    - ro.debuggable 是否为 1？（user 构建为 0）
  #    - setprop 之后是否重启了 app？属性只在 CreateInstance 时读。
  #    - 文件名是否严格是 libVkLayer*.so？
  #    - 用 `adb logcat | grep -i vulkan` 看 loader 自己的工作日志。

关闭 / 卸载：
  ./install_layer.sh --uninstall
--------------------------------------------------------------
TIP
