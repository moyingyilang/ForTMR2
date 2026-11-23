// ============================================================================
// EFML4A 落盘侦察 / 统计落盘 实现
//
// 全部文件操作用 POSIX 直接 syscall（open/read/write/rename），不用 <fstream>：
//   * 不需要缓冲（每行本来就小，而且我们希望写完立刻能被读到）
//   * 少一层 libc++ 依赖，交叉编译时更省心
// ============================================================================

#include "efml4a/recon.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "efml4a/log.h"
#include "efml4a/mode.h"

namespace efml4a {
namespace recon {

namespace {

// 需求上限：2000 行 draw 数据，之后写一行 {"recon":"capped"} 就彻底停手。
// 上限存在的意义：侦察文件不能无限增长（游戏一晚上能画几百万个 draw）。
constexpr uint64_t kMaxLines = 2000;

// Android 包名长度上限（PackageManager 的硬限制）。
constexpr size_t kMaxPackageNameBytes = 255;

constexpr const char* kReconFileName = "efml4a-recon.jsonl";
constexpr const char* kStatsFileName = "efml4a-stats.json";

std::mutex g_mutex;
bool g_resolved = false;         // 输出目录是否已成功解析
bool g_output_disabled = false;  // 永久禁用（cmdline/目录都不可用）
std::string g_dir;
std::string g_recon_path;
std::string g_stats_path;
int g_recon_fd = -1;
uint64_t g_lines = 0;
bool g_capped = false;

bool WriteAll(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

// 复制一份 cmdline 内容。/proc/self/cmdline 是一次性可读的伪文件，
// 但一次 read() 未必能读全，所以循环读。
bool ReadFileAll(const char* path, char* buf, size_t cap, size_t* out_len) {
    if (buf == nullptr || cap == 0) {
        return false;
    }
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    size_t total = 0;
    while (total + 1 < cap) {
        const ssize_t n = ::read(fd, buf + total, cap - 1 - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        total += static_cast<size_t>(n);
    }
    ::close(fd);
    buf[total] = '\0';
    if (out_len != nullptr) {
        *out_len = total;
    }
    return total > 0;
}

// 目录必须存在、是目录、可进入，并且**真的能创建文件**。
// 为什么不能只信 access()：FUSE/sdcardfs（外置存储）上 access(W_OK) 可能返回 0，
// 而真正的 open() 仍然被 scoped storage 拒掉 —— 只信 access() 会把「首次写文件
// 失败」升级成「永久禁用文件输出」，那是最坏的结果。所以这里真建一个探针文件
// 再删掉；只在解析阶段做一次（≤5 个候选），代价可忽略，也不会留下垃圾文件。
bool DirWritable(const std::string& dir) {
    if (dir.empty()) {
        return false;
    }
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    if (::access(dir.c_str(), W_OK | X_OK) != 0) {
        return false;
    }
    const std::string probe = dir + "/.efml4a-write-probe";
    const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    ::unlink(probe.c_str());
    return true;
}

std::string StripTrailingSlashes(const std::string& in) {
    std::string s = in;
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

// 候选输出目录，按优先级排列。
std::vector<std::string> Candidates() {
    std::vector<std::string> out;

    char cmdline[4096] = {};
    size_t cmdline_len = 0;
    if (ReadFileAll("/proc/self/cmdline", cmdline, sizeof(cmdline), &cmdline_len)) {
        const std::string pkg = PackageFromCmdline(cmdline, cmdline_len);
        if (!pkg.empty()) {
            out.push_back("/data/data/" + pkg + "/files");
            // 兜底 1：不是所有 app 都创建过 files/ 子目录（它由 getFilesDir() 懒创建）。
            // 应用对自己的 /data/data/<pkg> 有写权限，所以这是比外置存储可靠得多的退路。
            out.push_back("/data/data/" + pkg);
            // 兜底 2/3：app 自己的**外置私有目录**（== getExternalFilesDir()）。
            // 加这两条是为了兼容那些习惯从 /sdcard/Android/data/<pkg>/files 取产物的
            // 工具（本仓库的 scripts/game-test-cycle.sh 就是），以及数据目录被加固的 ROM。
            // /sdcard 与 /storage/emulated/0 通常指向同一个卷，列两个只为兼容挂载点差异。
            // ⚠️ 未核实：Android 10+ 的 scoped storage 下，app 直接 open() 自己这个目录
            //   通常是允许的（它就是自己的私有外置目录），但个别 ROM 会拦；因此它排在
            //   /data/data 之后，并且真正能不能写由 DirWritable() 的探针决定。
            out.push_back("/sdcard/Android/data/" + pkg + "/files");
            out.push_back("/storage/emulated/0/Android/data/" + pkg + "/files");
        }
    }

    // 兜底 2：老式外置存储。
    // ⚠️ 未核实（真机行为）：Android 10+ 的 scoped storage 下 $EXTERNAL_STORAGE
    //   通常指向 /sdcard (= /storage/emulated/0)，app 对它的**根目录**一般没有写权限，
    //   所以这一条很可能失败。保留它只是为了覆盖 cmdline 也读不到的极端情况。
    const char* ext = std::getenv("EXTERNAL_STORAGE");
    if (ext != nullptr && *ext != '\0') {
        const std::string dir = StripTrailingSlashes(ext);
        if (!dir.empty()) {
            out.push_back(dir);
        }
    }

    return out;
}

// 已持锁。解析并缓存输出目录；失败则永久禁用。
bool ResolveLocked() {
    if (g_resolved) {
        return true;
    }
    if (g_output_disabled) {
        return false;
    }

    const std::vector<std::string> candidates = Candidates();
    for (const std::string& dir : candidates) {
        if (!DirWritable(dir)) {
            continue;
        }
        g_dir = dir;
        g_recon_path = dir + "/" + kReconFileName;
        g_stats_path = dir + "/" + kStatsFileName;
        g_resolved = true;
        // 需求：首次写文件之前把最终选定的目录与完整路径打一次 logcat。
        // 真机上排查「为什么没有文件」时，这一行是唯一的线索。
        EFML4A_LOGI("file output dir = %s (recon=%s, stats=%s)", g_dir.c_str(),
                    g_recon_path.c_str(), g_stats_path.c_str());
        return true;
    }

    g_output_disabled = true;
    EFML4A_LOGE(
        "no writable output directory; file output permanently disabled "
        "(candidates tried=%zu, first=/data/data/<pkg>/files, EXTERNAL_STORAGE=%s)",
        candidates.size(), std::getenv("EXTERNAL_STORAGE") != nullptr ? std::getenv("EXTERNAL_STORAGE")
                                                                     : "<unset>");
    return false;
}

// 已持锁。
bool EnsureReconFdLocked() {
    if (g_recon_fd >= 0) {
        return true;
    }
    if (!ResolveLocked()) {
        return false;
    }
    // O_APPEND：多次启动/多个进程写同一个文件时不会互相覆盖，天然是 JSONL 追加语义。
    const int fd = ::open(g_recon_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        g_output_disabled = true;
        EFML4A_LOGE("cannot open %s: %s; file output permanently disabled", g_recon_path.c_str(),
                    std::strerror(errno));
        return false;
    }
    g_recon_fd = fd;
    return true;
}

}  // namespace

std::string PackageFromCmdline(const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return std::string();
    }

    // 只取第一段。
    size_t n = 0;
    while (n < len && data[n] != '\0') {
        ++n;
    }
    if (n == 0) {
        return std::string();  // cmdline 以 NUL 开头：什么也没有
    }
    if (n > kMaxPackageNameBytes) {
        return std::string();  // 超长：不是合法包名，拒绝（见头文件契约）
    }

    std::string name(data, n);

    // Android 私有进程的 cmdline 是 "com.foo:remote"，数据目录仍然是
    // /data/data/com.foo —— 所以只取 ':' 之前的部分。
    const size_t colon = name.find(':');
    if (colon != std::string::npos) {
        name.resize(colon);
    }
    if (name.empty()) {
        return std::string();
    }

    // 白名单校验：只允许 Android 包名允许的字符。'/' 会在这里被拒掉 ——
    // 这正是 Termux/桌面下把可执行文件路径当成包名时的保护。
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) {
            return std::string();
        }
    }

    return name;
}

bool Enabled() {
    if (mode::Current() == mode::kRecon) {
        return true;
    }
    // 兼容旧开关：debug.efml4a.recon=1（在 mode 属性存在之前就是这么用的）。
    return mode::LegacyReconRequested();
}

bool AppendLine(const std::string& json_line) {
    if (json_line.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_output_disabled) {
        return false;
    }

    if (g_lines >= kMaxLines) {
        if (!g_capped) {
            g_capped = true;
            // 封顶标记只写一次。之后所有 AppendLine 都直接返回 false，
            // 不再碰文件、不再打日志（需求：不要每帧重试、不要刷日志）。
            static const char kCappedLine[] = "{\"recon\":\"capped\"}\n";
            if (EnsureReconFdLocked()) {
                WriteAll(g_recon_fd, kCappedLine, sizeof(kCappedLine) - 1);
            }
            EFML4A_LOG_ONCE(EFML4A_LOGW, "recon capped at %llu lines, no more draw lines will be written",
                            static_cast<unsigned long long>(kMaxLines));
        }
        return false;
    }

    if (!EnsureReconFdLocked()) {
        return false;
    }
    // JSONL 的换行由本函数负责补：调用方只需要给「一行 JSON」。
    // 这样即使某个调用点忘了结尾换行，文件也仍然是合法的 JSONL。
    const bool need_newline = json_line.back() != '\n';
    if (!WriteAll(g_recon_fd, json_line.data(), json_line.size())) {
        return false;
    }
    if (need_newline && !WriteAll(g_recon_fd, "\n", 1)) {
        return false;
    }
    ++g_lines;
    return true;
}

bool WriteStats(const char* json) {
    if (json == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_output_disabled) {
        return false;
    }
    if (!ResolveLocked()) {
        return false;
    }

    // 先写临时文件再 rename：读取方（人或脚本）永远不会读到写了一半的 JSON。
    // rename 在同一文件系统内是原子的，这里 tmp 与目标同目录，必然同文件系统。
    const std::string tmp_path = g_stats_path + ".tmp";
    const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        // 统计落盘失败不永久禁用输出（可能只是瞬时失败），但也不要每 600 次提交刷屏。
        EFML4A_LOG_ONCE(EFML4A_LOGW, "cannot open %s: %s", tmp_path.c_str(), std::strerror(errno));
        return false;
    }

    const size_t len = std::strlen(json);
    const bool written = WriteAll(fd, json, len) && WriteAll(fd, "\n", 1);
    // ⚠️ 未核实：这里不 fsync。我们认为「进程还活着时能读到文件」这个目标不需要
    //   持久化保证；如果真机上发现读到空文件，再补 fsync。
    const bool closed_ok = (::close(fd) == 0);
    if (!written || !closed_ok) {
        ::unlink(tmp_path.c_str());
        EFML4A_LOG_ONCE(EFML4A_LOGW, "cannot write %s: %s", tmp_path.c_str(), std::strerror(errno));
        return false;
    }
    if (::rename(tmp_path.c_str(), g_stats_path.c_str()) != 0) {
        EFML4A_LOG_ONCE(EFML4A_LOGW, "cannot rename %s -> %s: %s", tmp_path.c_str(),
                        g_stats_path.c_str(), std::strerror(errno));
        ::unlink(tmp_path.c_str());
        return false;
    }
    return true;
}

uint64_t LineCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_lines;
}

std::string OutputDir() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_resolved && !g_output_disabled) {
        ResolveLocked();
    }
    return g_dir;
}

}  // namespace recon
}  // namespace efml4a
