// ============================================================================
// EFML4A 运行模式实现
//
// 线程模型：
//   所有配置都缓存成 std::atomic，热路径（vkCmdDraw* / vkCmdBindVertexBuffers）
//   只做原子读。Refresh() 可以由任意线程、任意频率调用：它只是把「环境变量/属性」
//   重新读一遍再写进原子变量，并发的重复 Refresh 最多导致多打一行 INFO，
//   不会产生坏状态（幂等写）。
// ============================================================================

#include "efml4a/mode.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "efml4a/log.h"

namespace efml4a {
namespace mode {

namespace {

std::atomic<int> g_mode{kOff};
std::atomic<uint32_t> g_skip_draws{1};
std::atomic<uint32_t> g_donor_max{64};
std::atomic<bool> g_refreshed{false};

std::atomic<bool> g_legacy_recon_read{false};
std::atomic<bool> g_legacy_recon{false};

// 计数上限：纯粹是防呆。skip=1000000 已经等于「全丢」，donor_max=1000000
// 也早已超过任何安全范围，再大没有意义。
constexpr uint32_t kCountLimit = 1000000;

constexpr uint32_t kDefaultSkipDraws = 1;
constexpr uint32_t kDefaultDonorMax = 64;

char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// 就地去掉首尾空白。返回推进后的起点（可能不是原指针）。
char* TrimInPlace(char* s) {
    if (s == nullptr) {
        return nullptr;
    }
    while (*s != '\0' && IsSpace(*s)) {
        ++s;
    }
    size_t n = std::strlen(s);
    while (n > 0 && IsSpace(s[n - 1])) {
        s[--n] = '\0';
    }
    return s;
}

// 不依赖 <strings.h>/locale 的大小写无关比较（只处理 ASCII，属性值都是 ASCII）。
bool EqualsIgnoreCase(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (LowerAscii(*a) != LowerAscii(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// 「环境变量优先，系统属性兜底」。返回 false = 两个来源都没有值。
bool ReadParam(const char* env_name, const char* prop_name, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    const char* env = std::getenv(env_name);
    if (env != nullptr && *env != '\0') {
        std::snprintf(out, out_size, "%s", env);
        return true;
    }
    return log::SystemProperty(prop_name, out, out_size);
}

}  // namespace

const char* Name(Mode m) {
    switch (m) {
        case kOff:
            return "off";
        case kRecon:
            return "recon";
        case kSkip:
            return "skip";
        case kDonor:
            return "donor";
        default:
            return "off";
    }
}

Mode ParseMode(const char* value) {
    if (value == nullptr) {
        return kOff;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", value);
    const char* trimmed = TrimInPlace(buf);

    if (EqualsIgnoreCase(trimmed, "recon")) {
        return kRecon;
    }
    if (EqualsIgnoreCase(trimmed, "skip")) {
        return kSkip;
    }
    if (EqualsIgnoreCase(trimmed, "donor")) {
        return kDonor;
    }
    // "off" / "" / 任何拼错的值都回落到 off：宁可什么都不做，也不要因为
    // 属性写错而改变渲染行为。
    return kOff;
}

uint32_t ParseCount(const char* value, uint32_t fallback) {
    if (value == nullptr) {
        return fallback;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", value);
    const char* trimmed = TrimInPlace(buf);
    if (*trimmed == '\0') {
        return fallback;
    }
    // 负数显式拒绝：strtoul 会把 "-1" 变成巨大的无符号数（不是我们想要的）。
    if (*trimmed == '-') {
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(trimmed, &end, 10);
    if (end == trimmed || (end != nullptr && *end != '\0')) {
        return fallback;  // 空 / 含非数字尾巴
    }
    if (errno == ERANGE || parsed > kCountLimit) {
        return kCountLimit;  // 一律夹到上限，而不是回落：用户显然想要「很大」
    }
    return static_cast<uint32_t>(parsed);
}

void Refresh() {
    char buf[128] = {};

    Mode parsed_mode = kOff;
    if (ReadParam("EFML4A_MODE", "debug.efml4a.mode", buf, sizeof(buf))) {
        parsed_mode = ParseMode(buf);
    }

    uint32_t skip = kDefaultSkipDraws;
    if (ReadParam("EFML4A_SKIP", "debug.efml4a.skip", buf, sizeof(buf))) {
        skip = ParseCount(buf, kDefaultSkipDraws);
    }

    uint32_t donor_max = kDefaultDonorMax;
    if (ReadParam("EFML4A_DONOR_MAX", "debug.efml4a.donor_max", buf, sizeof(buf))) {
        // 注意：donor_max=0 是合法值，表示「一次都不替换」（等于临时关掉 donor，
        // 但保留 buffer 记录），所以这里不做非零兜底。
        donor_max = ParseCount(buf, kDefaultDonorMax);
    }

    const int previous = g_mode.load(std::memory_order_relaxed);
    const bool changed = previous != static_cast<int>(parsed_mode);

    g_skip_draws.store(skip, std::memory_order_relaxed);
    g_donor_max.store(donor_max, std::memory_order_relaxed);
    g_mode.store(static_cast<int>(parsed_mode), std::memory_order_relaxed);
    g_refreshed.store(true, std::memory_order_release);

    // 真机上没有调试器，logcat 是唯一能确认「模式到底生效没有」的地方，
    // 所以这里在「首次读到非 off」以及「之后每次模式变化」都打一行 INFO。
    if (changed && parsed_mode != kOff) {
        EFML4A_LOGI("runtime mode = %s (skip_draws=%u, donor_max=%u) [EFML4A_MODE / "
                    "debug.efml4a.mode]",
                    Name(parsed_mode), skip, donor_max);
    } else if (changed) {
        EFML4A_LOGI("runtime mode = off (pure pass-through, counters only)");
    }
}

Mode Current() {
    if (!g_refreshed.load(std::memory_order_acquire)) {
        Refresh();
    }
    return static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
}

uint32_t SkipDraws() {
    if (!g_refreshed.load(std::memory_order_acquire)) {
        Refresh();
    }
    return g_skip_draws.load(std::memory_order_relaxed);
}

uint32_t DonorMax() {
    if (!g_refreshed.load(std::memory_order_acquire)) {
        Refresh();
    }
    return g_donor_max.load(std::memory_order_relaxed);
}

bool LegacyReconRequested() {
    // 只在第一次调用时读属性（幂等）。
    if (!g_legacy_recon_read.exchange(true, std::memory_order_acq_rel)) {
        char buf[128] = {};
        bool on = false;
        if (log::SystemProperty("debug.efml4a.recon", buf, sizeof(buf))) {
            on = std::strcmp(buf, "1") == 0;
        }
        g_legacy_recon.store(on, std::memory_order_release);
    }
    return g_legacy_recon.load(std::memory_order_acquire);
}

}  // namespace mode
}  // namespace efml4a
