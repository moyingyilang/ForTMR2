#!/data/data/com.termux/files/usr/bin/bash
# EFML4A 游戏内测试循环：安装层 → 启动游戏 → 取样 → **无条件清理**。
#
# 设计原则：暴露窗口必须短且**必然关闭**。脚本用 trap 保证无论中途失败、
# Ctrl-C 还是超时，都会删除游戏 lib 目录里的层 .so 并清空系统属性。
#
# 用法：
#   game-test-cycle.sh <包名> [模式] [等待秒数]
#     <包名>   例如 com.hypergryph.endfield
#     [模式]   off | recon | skip | donor      （默认 recon）
#     [等待]   启动后观察秒数                    （默认 25）
#
# 依赖：vulkan-layer/build-android/libVkLayer_EFML4A.so
set -uo pipefail

PKG="${1:?用法: game-test-cycle.sh <包名> [模式] [等待秒数]}"
MODE="${2:-recon}"
WAIT="${3:-25}"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${EFML4A_OUT:-$ROOT/vulkan-layer/build-android}"
SO="$OUT/libVkLayer_EFML4A.so"
LAYER_NAME="VK_LAYER_EFML4A_model_replacer"
GAMEDIR="/sdcard/Android/data/$PKG/files"

[ -f "$SO" ] || { echo "找不到 $SO，先跑 scripts/build-android-termux.sh" >&2; exit 1; }

LIBDIR="$(su -c "dumpsys package $PKG" 2>/dev/null | grep -m1 nativeLibraryDir | sed 's/.*=//' | tr -d '\r')"
[ -z "$LIBDIR" ] && LIBDIR="$(su -c "ls -d /data/app/*/$PKG-*/lib/arm64" 2>/dev/null | head -1)"
[ -n "$LIBDIR" ] || { echo "找不到 $PKG 的 native lib 目录" >&2; exit 1; }

cleanup() {
    echo
    echo "[cycle] === 清理（无条件执行）==="
    su -c "am force-stop $PKG" >/dev/null 2>&1
    sleep 2
    su -c "rm -f '$LIBDIR/libVkLayer_EFML4A.so'"
    su -c 'setprop debug.vulkan.layers ""'
    su -c 'setprop debug.efml4a.mode ""'
    su -c 'setprop debug.efml4a.recon ""'
    su -c 'setprop debug.efml4a.skip ""'
    su -c 'setprop debug.efml4a.donor_max ""'
    echo "[cycle] 层 .so 已删除，系统属性已清空"
    su -c "ls '$LIBDIR/libVkLayer_EFML4A.so'" 2>&1 | grep -q No.such || echo "[cycle] 警告：删除可能未成功，请手动检查 $LIBDIR"
}
trap cleanup EXIT INT TERM

echo "[cycle] 目标    : $PKG"
echo "[cycle] lib 目录: $LIBDIR"
echo "[cycle] 模式    : $MODE   观察 $WAIT 秒"

echo "[cycle] 安装层 .so"
su -c "cp '$SO' '$LIBDIR/' && chmod 644 '$LIBDIR/libVkLayer_EFML4A.so' && chcon u:object_r:apk_data_file:s0 '$LIBDIR/libVkLayer_EFML4A.so'"

echo "[cycle] 设置属性"
su -c "setprop debug.vulkan.layers $LAYER_NAME"
[ "$MODE" != "off" ] && su -c "setprop debug.efml4a.mode $MODE"

echo "[cycle] 清空 logcat/dmesg"
su -c 'logcat -c -b all' >/dev/null 2>&1
su -c 'dmesg -c' >/dev/null 2>&1

echo "[cycle] 启动游戏"
su -c "am force-stop $PKG" >/dev/null 2>&1
sleep 2
su -c "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1

for i in $(seq 1 "$((WAIT / 5))"); do
    sleep 5
    PID="$(su -c "pidof $PKG" 2>/dev/null | awk '{print $1}')"
    if [ -z "$PID" ]; then echo "[cycle][$((i*5))s] 进程不在"; continue; fi
    L="$(su -c "cat /proc/$PID/maps" 2>/dev/null | grep -c libVkLayer_EFML4A)"
    echo "[cycle][$((i*5))s] pid=$PID 层模块=$L"
    if [ "$L" -gt 0 ] && [ "$i" -ge 2 ]; then break; fi
done

echo
echo "[cycle] === 层日志（logcat 中的 EFML4A 行）==="
su -c 'logcat -d -b all' 2>/dev/null | grep -iE 'EFML4A' | tail -30

echo
echo "[cycle] === 落盘产物 ==="
for f in efml4a-recon.jsonl efml4a-stats.json; do
    if [ -f "$GAMEDIR/$f" ]; then
        echo "  --- $f ($(wc -c < "$GAMEDIR/$f") 字节) ---"
        head -5 "$GAMEDIR/$f"
        echo "  ...（共 $(wc -l < "$GAMEDIR/$f") 行）"
        cp "$GAMEDIR/$f" "$OUT/" 2>/dev/null
    else
        echo "  $f 不存在（层未开启该模式，或写入失败）"
    fi
done
