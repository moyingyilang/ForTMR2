#pragma once

// ============================================================================
// EFML4A 运行模式（off / recon / skip / donor）
//
// 为什么要一个「模式」而不是一堆独立的开关：
//   真机上唯一能改层行为的通道是系统属性（app 进程看不到环境变量，见 log.h）。
//   一个属性 `debug.efml4a.mode` 就能把层切到不同的实验行为，排障时不用记
//   一堆 setprop 组合，也不会出现「两个开关互相矛盾」的状态。
//
// 取值（未知值一律回落到 off，绝不因为写错属性就改变渲染行为）：
//   off    只计数 + 转发（默认，零行为改变）
//   recon  只读侦察：逐 draw 写 JSONL，不改任何 Vulkan 参数
//   skip   在 vkCmdDraw* 里直接 return（不转发），丢弃每个 submit 窗口内的前 K 次绘制
//   donor  把 vkCmdBindVertexBuffers 里 firstBinding==0 的缓冲换成另一个「足够大」的缓冲
//
// 读取顺序：环境变量 EFML4A_MODE 优先，其次系统属性 debug.efml4a.mode。
//   * 真机 app 进程里环境变量不存在 -> 只能走属性，这正是本模块存在的原因；
//   * 桌面/冒烟测试里用环境变量注入模式，方便在没有属性服务的环境验证代码路径。
// 同名的两个数值参数：
//   debug.efml4a.skip      （环境变量 EFML4A_SKIP）     默认 1
//   debug.efml4a.donor_max （环境变量 EFML4A_DONOR_MAX）默认 64
// ============================================================================

#include <cstdint>

namespace efml4a {
namespace mode {

enum Mode {
    kOff = 0,  // 只计数 + 转发
    kRecon,    // 只读侦察
    kSkip,     // 丢弃绘制
    kDonor,    // 替换 binding 0 的顶点缓冲
};

// 稳定名字，同时用于 logcat 与 stats JSON 的 "mode" 字段（不要改）。
const char* Name(Mode m);

// 纯函数：字符串 -> 模式。空串 / nullptr / 非法值 -> kOff。
// 大小写不敏感，允许首尾空白（setprop 里手滑敲空格时不至于静默失效）。
Mode ParseMode(const char* value);

// 纯函数：字符串 -> 计数。空串 / nullptr / 非法 / 负数 / 溢出 -> fallback。
// 上限 1000000（防呆：真机上把 skip 敲成 999999999 也不至于把整帧清空）。
uint32_t ParseCount(const char* value, uint32_t fallback);

// 重新读属性/环境变量并更新缓存。可以反复调用：
//   hooks.cpp 在**每次 vkQueueSubmit** 调它，这样 setprop 改模式可以在 1 帧内生效，
//   而不用重启游戏；代价是每帧几次属性读（不落在 draw 热路径上）。
// 值发生变化（含首次读到非 off）时打一行 INFO，方便在 logcat 里确认。
void Refresh();

// 热路径用：读缓存（首次调用会触发一次 Refresh）。无锁，只有原子读。
Mode Current();

uint32_t SkipDraws();  // K
uint32_t DonorMax();   // donor 替换次数上限

// 兼容旧开关：debug.efml4a.recon == "1"。同样只读一次。
bool LegacyReconRequested();

}  // namespace mode
}  // namespace efml4a
