// ============================================================================
// EFML4A 层日志实现
// ============================================================================

#include "efml4a/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <android/log.h>
// ⚠️ 未核实：非 bionic 的 Android 工具链（例如某些 musl 交叉工具链）可能没有
//   <sys/system_properties.h>。这里再套一层 __has_include 兜底：没有头文件时
//   退化成「只认环境变量」，而不是编译失败。已核实存在的两处：
//     * Termux host:  $PREFIX/include/sys/system_properties.h
//     * NDK r29:      sysroot/usr/include/sys/system_properties.h
#if __has_include(<sys/system_properties.h>)
#include <sys/system_properties.h>
#define EFML4A_HAVE_SYSTEM_PROPERTIES 1
#endif
#endif

namespace efml4a {
namespace log {

namespace {

std::atomic<int> g_level{kLevelInfo};
std::atomic<bool> g_initialized{false};

// __system_property_get 不接受缓冲区长度参数；bionic 的属性值上限是
// PROP_VALUE_MAX(92) 字节，缓冲区小于它就有越界风险，所以直接拒绝调用。
constexpr size_t kPropertyBufferMin = 92;

const char* LevelTag(Level level) {
    switch (level) {
        case kLevelError:
            return "E";
        case kLevelWarn:
            return "W";
        case kLevelInfo:
            return "I";
        case kLevelDebug:
            return "D";
        default:
            return "?";
    }
}

const char* LevelName(Level level) {
    switch (level) {
        case kLevelSilent:
            return "silent";
        case kLevelError:
            return "error";
        case kLevelWarn:
            return "warn";
        case kLevelInfo:
            return "info";
        case kLevelDebug:
            return "debug";
        default:
            return "unknown";
    }
}

// 把字符串解析成 Level。返回 false 表示「不认识这个值」，调用方应保持原值。
// 为什么同时接受字符串和数字：字符串是仓库既有语义（不能改），数字是任务书里
// 约定的 0..4（真机上 `setprop debug.efml4a.log_level 4` 比敲单词顺手）。
// off 与 silent 都映射到 kLevelSilent(=-1)：既有实现里它就是「关闭」。
bool ParseLevel(const char* raw, Level* out) {
    if (raw == nullptr || out == nullptr || *raw == '\0') {
        return false;
    }
    if (std::strcmp(raw, "silent") == 0 || std::strcmp(raw, "off") == 0 ||
        std::strcmp(raw, "none") == 0 || std::strcmp(raw, "0") == 0) {
        *out = kLevelSilent;
    } else if (std::strcmp(raw, "error") == 0 || std::strcmp(raw, "1") == 0) {
        *out = kLevelError;
    } else if (std::strcmp(raw, "warn") == 0 || std::strcmp(raw, "warning") == 0 ||
               std::strcmp(raw, "2") == 0) {
        *out = kLevelWarn;
    } else if (std::strcmp(raw, "info") == 0 || std::strcmp(raw, "3") == 0) {
        *out = kLevelInfo;
    } else if (std::strcmp(raw, "debug") == 0 || std::strcmp(raw, "verbose") == 0 ||
               std::strcmp(raw, "4") == 0) {
        *out = kLevelDebug;
    } else {
        return false;
    }
    return true;
}

}  // namespace

void SetLevel(Level level) { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }

Level GetLevel() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

bool SystemProperty(const char* name, char* value, size_t value_size) {
    if (name == nullptr || value == nullptr) {
        return false;
    }
    if (value_size == 0) {
        return false;
    }
    value[0] = '\0';

#if defined(EFML4A_HAVE_SYSTEM_PROPERTIES)
    if (value_size < kPropertyBufferMin) {
        // 宁可读不到，也不要越界写。
        return false;
    }
    const int len = __system_property_get(name, value);
    if (len <= 0) {
        // 属性不存在（返回 0）；某些实现也会留下空串，统一按「无值」处理。
        value[0] = '\0';
        return false;
    }
    value[value_size - 1] = '\0';  // 双保险：即使实现写满也不越界
    return true;
#else
    // 桌面（非 Android）没有属性服务：恒「无值」。
    (void)value_size;
    return false;
#endif
}

void InitFromEnv() {
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true)) {
        return;  // 已经初始化过：幂等，后续调用只是一次原子读
    }

    Level parsed = kLevelInfo;
    const char* source = "default";

    // 1) 环境变量优先 —— 保持桌面/测试里既有行为完全不变。
    const char* raw = std::getenv("EFML4A_LOG_LEVEL");
    if (ParseLevel(raw, &parsed)) {
        source = "env EFML4A_LOG_LEVEL";
        SetLevel(parsed);
    } else {
        // 2) 环境变量没给出可识别值时读系统属性。注意这里**不是**「环境变量存在就
        //    完全屏蔽属性」：真机上环境变量恒为空，桌面上写错环境变量时回落到
        //    属性更符合直觉（少一个静默失效的坑）。
        char value[kPropertyBufferMin + 64] = {};
        if (SystemProperty("debug.efml4a.log_level", value, sizeof(value)) &&
            ParseLevel(value, &parsed)) {
            source = "setprop debug.efml4a.log_level";
            SetLevel(parsed);
        }
        // 两个来源都没有可用值 -> 保持默认 info（与改动前一致）。
    }

    // 这一行本身也受日志级别控制：真机上 logcat 里看到它就说明属性读到了。
    // 递归安全性：此时 g_initialized 已置位，嵌套的 Write()->InitFromEnv() 会立刻返回。
    EFML4A_LOGI("log level = %s (from %s)", LevelName(GetLevel()), source);
}

void Write(Level level, const char* fmt, ...) {
    InitFromEnv();

    const int configured = g_level.load(std::memory_order_relaxed);
    if (configured == kLevelSilent || static_cast<int>(level) > configured) {
        return;
    }

    // 先把消息格式化到栈缓冲区，再由各平台的后端输出。
    char message[1024];
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    message[sizeof(message) - 1] = '\0';

#if defined(__ANDROID__)
    // Android 后端：打进 logcat，tag 固定为 EFML4A。
    int prio = ANDROID_LOG_INFO;
    switch (level) {
        case kLevelError:
            prio = ANDROID_LOG_ERROR;
            break;
        case kLevelWarn:
            prio = ANDROID_LOG_WARN;
            break;
        case kLevelInfo:
            prio = ANDROID_LOG_INFO;
            break;
        case kLevelDebug:
            prio = ANDROID_LOG_DEBUG;
            break;
        default:
            break;
    }
    __android_log_print(prio, "EFML4A", "[%s] %s", LevelTag(level), message);
#else
    // host 后端：stderr。
    std::fprintf(stderr, "EFML4A [%s] %s\n", LevelTag(level), message);
    std::fflush(stderr);
#endif
}

}  // namespace log
}  // namespace efml4a
