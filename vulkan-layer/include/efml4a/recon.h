#pragma once

// ============================================================================
// EFML4A 落盘侦察（recon）/ 统计落盘
//
// 为什么需要落盘：真机上游戏进程的 stdout/stderr 拿不到，logcat 又会刷屏且
// 会被 buffer 覆盖。把「每个 draw 一行 JSON」直接写进游戏自己的数据目录，
// 是最可靠的取证方式（用 root 或 run-as 或 adb pull 都能读）。
//
// 输出目录的确定顺序（见 src/recon.cpp 的 ResolveLocked()）：
//   1) /data/data/<包名>/files   <- 首选。包名从 /proc/self/cmdline 解析
//   2) /data/data/<包名>         <- 兜底 1：个别 app 还没创建 files/ 子目录
//   3) $EXTERNAL_STORAGE         <- 兜底 2：老式外置存储（现代 Android 上常常不可写，见 .cpp 注释）
// 全都不可用时：打一次 LOGE，然后**永久**关掉文件输出（不重试、不刷日志）。
//
// 本模块**不包含**任何 Vulkan 头文件：这样它的纯函数可以被独立单测链接。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>

namespace efml4a {
namespace recon {

// 是否启用侦察：mode==recon，或者兼容旧开关 debug.efml4a.recon=="1"。
bool Enabled();

// ---------------------------------------------------------------------------
// 纯函数（可单测，不做任何系统调用）
// ---------------------------------------------------------------------------
// 从 /proc/self/cmdline 的原始字节里取出包名。
// 契约：
//   * 只取第一段（cmdline 是 NUL 分隔的 argv；app 进程第一段就是包名）
//   * data==nullptr / len==0 / 首字节就是 NUL        -> ""
//   * 第一段含 ':'（Android 私有进程 "pkg:svc"）     -> 取 ':' 之前的部分
//   * 第一段含 '/'（其实是可执行文件路径，例如 Termux 下跑测试） -> ""
//   * 出现 [A-Za-z0-9._-] 之外的字符（含非 ASCII）   -> ""
//   * 第一段长度 > 255（Android 包名上限）           -> ""
// 之所以要拒绝而不是「尽力而为」：返回值会被拼进文件路径，让它无条件可信
// 比拼出一个奇怪的目录更安全。
std::string PackageFromCmdline(const char* data, size_t len);

// ---------------------------------------------------------------------------
// 文件输出
// ---------------------------------------------------------------------------
// 追加一行 JSONL。**不做 Enabled() 判断**（由调用方决定要不要采），这样单元
// 测试可以直接验证文件落盘与封顶逻辑。
// 返回 false 表示没有写入（已封顶 / 目录不可用 / 输出已永久禁用）。
bool AppendLine(const std::string& json_line);

// 覆盖写统计 JSON（先写 <file>.tmp 再 rename，读取方永远不会看到半个 JSON）。
bool WriteStats(const char* json);

// 已经写入的侦察数据行数（不含那行 {"recon":"capped"}）。
uint64_t LineCount();

// 当前选定的输出目录；还没解析或解析失败时返回空串（供排障/测试用）。
std::string OutputDir();

}  // namespace recon
}  // namespace efml4a
