#pragma once

#include <cstddef>

// ============================================================================
// EFML4A 层日志宏
//
// 目标平台差异：
//   * Android/bionic -> __android_log_print(LOG_TAG, ...)  （需要链接 liblog）
//   * host/Linux     -> fputs 到 stderr
// 日志级别有两个来源，**环境变量优先**：
//   1) 环境变量 EFML4A_LOG_LEVEL
//   2) 系统属性 debug.efml4a.log_level（仅 __ANDROID__ 下编译）
// 为什么需要系统属性：Android 的 app 进程是由 zygote fork 出来的，**看不到**
//   shell 里 export 的任何环境变量，所以 EFML4A_LOG_LEVEL 在真机游戏进程里
//   永远是空的。真机上唯一可用的通道是 `setprop debug.efml4a.log_level <v>`
//   （需要 root），层在 app 进程内用 __system_property_get 读回来。
//
// 取值语义沿用本仓库既有实现（字符串，默认 info）：
//   silent | error | warn | info | debug
// 另外兼容任务书里的数字写法（属性用数字更顺手）：
//   0=silent 1=error 2=warn 3=info 4=debug
// 注意：0 在任务书里是 "off"，本仓库既有实现里对应的名字是 "silent"，
//       两者等价，见 src/log.cpp 的 ParseLevel()。
// ============================================================================

namespace efml4a {
namespace log {

enum Level {
    kLevelSilent = -1,
    kLevelError = 0,
    kLevelWarn = 1,
    kLevelInfo = 2,
    kLevelDebug = 3,
};

// 读取 EFML4A_LOG_LEVEL / debug.efml4a.log_level；只生效一次（幂等），线程安全。
// 名字保留 InitFromEnv 以维持既有调用点不变（见 layer.cpp / log.cpp）。
void InitFromEnv();

// 读一个 Android 系统属性（bionic __system_property_get）。
// 仅在 __ANDROID__ 且有 <sys/system_properties.h> 时真正生效，其它平台恒返回 false
// —— 这样桌面构建不需要任何桩实现。
// value_size 必须 >= 96 字节：bionic 的属性值上限是 PROP_VALUE_MAX(92)，
// __system_property_get 不接受长度参数，缓冲区太小会越界。
// 返回 false 表示「没有这个属性」或平台不支持；value 一定被写成合法 C 字符串。
bool SystemProperty(const char* name, char* value, size_t value_size);

void SetLevel(Level level);
Level GetLevel();

// printf 风格。内部会加 "EFML4A: " 前缀。
void Write(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// 把某条日志限制为「整个进程只打印一次」（用于高频 hook 里避免刷屏）。
// 用法：EFML4A_LOG_ONCE(EFML4A_LOGI, "first draw seen");
#define EFML4A_LOG_ONCE(level, ...)                       \
    do {                                                  \
        static bool efml4a_logged_ = false;               \
        if (!efml4a_logged_) {                            \
            efml4a_logged_ = true;                        \
            level(__VA_ARGS__);                           \
        }                                                 \
    } while (0)

}  // namespace log
}  // namespace efml4a

// 注意：__VA_ARGS__ 前不加 ##，强制要求至少一个格式串参数（-Wall -Wextra 下更安全）。
#define EFML4A_LOGE(...) ::efml4a::log::Write(::efml4a::log::kLevelError, __VA_ARGS__)
#define EFML4A_LOGW(...) ::efml4a::log::Write(::efml4a::log::kLevelWarn, __VA_ARGS__)
#define EFML4A_LOGI(...) ::efml4a::log::Write(::efml4a::log::kLevelInfo, __VA_ARGS__)
#define EFML4A_LOGD(...) ::efml4a::log::Write(::efml4a::log::kLevelDebug, __VA_ARGS__)
