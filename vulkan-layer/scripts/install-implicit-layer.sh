#!/data/data/com.termux/files/usr/bin/bash
# 把 EFML4A 隐式层安装到设备上，并（可选）对某个目标进程启用。
#
# ⚠️ 需要 root。本脚本只做三件事，且每一步都可逆：
#   1. 把层 .so 与 manifest 复制到 /data/local/debug/vulkan/
#   2. 用 chcon 把它们改标为 u:object_r:apk_data_file:s0
#      —— **这一步是关键**。该目录默认标签是 system_data_file，
#      而应用进程的 SELinux 域（untrusted_app）**无权读取**，
#      loader 会找不到 manifest，表现为 vkCreateInstance 返回
#      VK_ERROR_LAYER_NOT_PRESENT(-6)。apk_data_file 是全局可读类型。
#   3. 设置 debug.vulkan.layers 属性（非持久，重启即失效）
#
# 卸载：见文件末尾的说明。
set -euo pipefail

LAYER_NAME="VK_LAYER_EFML4A_model_replacer"
DIR=/data/local/debug/vulkan
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${EFML4A_OUT:-$ROOT/vulkan-layer/build-android}"
SO="$OUT/libVkLayer_EFML4A.so"

[ -f "$SO" ] || { echo "找不到 $SO，请先跑 build-android-termux.sh" >&2; exit 1; }

TMP="$(mktemp -d)"
cat > "$TMP/VkLayer_EFML4A.json" <<JSON
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "$LAYER_NAME",
    "type": "GLOBAL",
    "library_path": "$DIR/libVkLayer_EFML4A.so",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "EFML4A model replacer (implicit layer, research build)",
    "functions": { "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion" }
  }
}
JSON

echo "[install] 复制到 $DIR"
su -c "mkdir -p $DIR && cp '$SO' '$TMP/VkLayer_EFML4A.json' $DIR/ && chmod 644 $DIR/*"
echo "[install] chcon -> apk_data_file（否则 app 域读不到）"
su -c "chcon -R u:object_r:apk_data_file:s0 $DIR"
su -c "ls -lZ $DIR"

if [ "${1:-}" = "--enable" ]; then
    echo "[install] setprop debug.vulkan.layers=$LAYER_NAME"
    su -c "setprop debug.vulkan.layers $LAYER_NAME"
    echo "[install] 已启用。用 --disable 关闭。"
fi
rm -rf "$TMP"

cat <<'NOTE'

启用 / 关闭 / 卸载：
  启用：  su -c 'setprop debug.vulkan.layers VK_LAYER_EFML4A_model_replacer'
  关闭：  su -c 'setprop debug.vulkan.layers ""'      # 重启也会自动失效
  卸载：  su -c 'rm -rf /data/local/debug/vulkan'      # 或只删本层的两个文件
NOTE
