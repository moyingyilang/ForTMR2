// ============================================================================
// EFML4A 纯函数 / 落盘逻辑 单元测试（仅宿主机构建；不需要 Vulkan 设备）
//
// 覆盖：
//   [1] recon::PackageFromCmdline —— 从 /proc/self/cmdline 原始字节解析包名
//       （正常单段 / 含额外参数 / 空 / 只有 NUL / 超长 / 边界 / 非法字符 / 私有进程名）
//   [2] mode::ParseMode  —— off/recon/skip/donor，大小写、空白、非法值回落 off
//   [3] mode::ParseCount —— skip / donor_max 的数值解析
//   [4] mode::Refresh    —— 环境变量 -> 缓存 -> Current()/SkipDraws()/DonorMax()
//   [5] 落盘侦察与统计文件 —— 用 EXTERNAL_STORAGE 兜底路径验证 JSONL 追加、
//       2000 行封顶 + {"recon":"capped"}、以及 stats 的 tmp+rename 覆盖写
//
// 为什么直接编译源码而不是 dlopen 层 .so：
//   层用 -fvisibility=hidden + version script 只导出 ABI 入口，这些内部函数
//   在 .so 里是 local 符号，dlsym 拿不到。
//
// 用法：efml4a_test_pure
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "efml4a/mode.h"
#include "efml4a/recon.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        std::printf("  ok   : %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  FAILED: %s\n", what.c_str());
    }
}

std::string Show(const std::string& s) {
    if (s.empty()) {
        return "<empty>";
    }
    return "\"" + s + "\"";
}

void CheckStr(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got == want) {
        std::printf("  ok   : %s -> %s\n", what.c_str(), Show(got).c_str());
    } else {
        ++g_failures;
        std::printf("  FAILED: %s -> got %s, want %s\n", what.c_str(), Show(got).c_str(),
                    Show(want).c_str());
    }
}

void CheckMode(efml4a::mode::Mode got, efml4a::mode::Mode want, const std::string& what) {
    ++g_checks;
    if (got == want) {
        std::printf("  ok   : %s -> %s\n", what.c_str(), efml4a::mode::Name(got));
    } else {
        ++g_failures;
        std::printf("  FAILED: %s -> got %s, want %s\n", what.c_str(), efml4a::mode::Name(got),
                    efml4a::mode::Name(want));
    }
}

void CheckU32(uint32_t got, uint32_t want, const std::string& what) {
    ++g_checks;
    if (got == want) {
        std::printf("  ok   : %s -> %u\n", what.c_str(), got);
    } else {
        ++g_failures;
        std::printf("  FAILED: %s -> got %u, want %u\n", what.c_str(), got, want);
    }
}

std::string ReadWholeFile(const std::string& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (ok != nullptr) {
            *ok = false;
        }
        return std::string();
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (ok != nullptr) {
        *ok = true;
    }
    return ss.str();
}

size_t CountLines(const std::string& text) {
    size_t n = 0;
    for (char c : text) {
        if (c == '\n') {
            ++n;
        }
    }
    return n;
}

// 最后一行（去掉结尾换行之后）。
std::string LastLine(const std::string& text) {
    size_t end = text.find_last_not_of("\n");
    if (end == std::string::npos) {
        return std::string();
    }
    const size_t nl = text.find_last_of('\n', end);
    const size_t begin = (nl == std::string::npos) ? 0 : nl + 1;
    return text.substr(begin, end - begin + 1);
}

// PackageFromCmdline 的便利包装：从 std::string 里取（保留内部 NUL）。
std::string Pkg(const std::string& raw) {
    return efml4a::recon::PackageFromCmdline(raw.data(), raw.size());
}

}  // namespace

int main() {
    std::printf("== EFML4A pure-function / recon file-output tests ==\n");

    // -----------------------------------------------------------------------
    std::printf("\n[1] recon::PackageFromCmdline\n");
    {
        CheckStr(Pkg(std::string("com.hypergryph.endfield\0", 25)), "com.hypergryph.endfield",
                 "正常单段");

        CheckStr(Pkg(std::string("com.hypergryph.endfield\0--some-arg\0-x\0", 41)),
                 "com.hypergryph.endfield", "含额外参数只取第一段");

        CheckStr(Pkg(std::string("com.foo.bar")), "com.foo.bar", "没有结尾 NUL");

        CheckStr(Pkg(std::string()), "", "空字符串");
        CheckStr(efml4a::recon::PackageFromCmdline(nullptr, 0), "", "nullptr + len=0");
        CheckStr(efml4a::recon::PackageFromCmdline(nullptr, 16), "", "nullptr + len>0");

        CheckStr(Pkg(std::string("\0", 1)), "", "只有一个 NUL");
        CheckStr(Pkg(std::string("\0\0\0", 3)), "", "只有三个 NUL");

        {
            const std::string too_long(300, 'a');
            CheckStr(Pkg(too_long), "", "超长 300 字节 -> 拒绝");
            const std::string boundary(255, 'a');
            CheckStr(Pkg(boundary), boundary, "恰好 255 字节 -> 接受");
            const std::string over_boundary(256, 'a');
            CheckStr(Pkg(over_boundary), "", "256 字节 -> 拒绝");
        }

        CheckStr(Pkg(std::string("/data/data/com.termux/files/usr/bin/x\0", 38)), "",
                 "含 '/'（可执行文件路径）-> 拒绝");
        CheckStr(Pkg(std::string("com.foo bar\0", 12)), "", "含空格 -> 拒绝");
        CheckStr(Pkg(std::string("com.foo\nbar\0", 12)), "", "含换行 -> 拒绝");
        CheckStr(Pkg(std::string("com.foo;rm -rf /\0", 16)), "", "含 ';' -> 拒绝");

        CheckStr(Pkg(std::string("com.hypergryph.endfield:remote\0", 32)),
                 "com.hypergryph.endfield", "私有进程名取 ':' 之前");
        CheckStr(Pkg(std::string(":onlyservice\0", 13)), "", "只有 ':' -> 拒绝");

        CheckStr(Pkg(std::string("com.Foo-Bar_baz9.qux\0", 21)), "com.Foo-Bar_baz9.qux",
                 "字母/数字/点/下划线/连字符均可");
    }

    // -----------------------------------------------------------------------
    std::printf("\n[2] mode::ParseMode\n");
    {
        CheckMode(efml4a::mode::ParseMode("off"), efml4a::mode::kOff, "\"off\"");
        CheckMode(efml4a::mode::ParseMode("recon"), efml4a::mode::kRecon, "\"recon\"");
        CheckMode(efml4a::mode::ParseMode("skip"), efml4a::mode::kSkip, "\"skip\"");
        CheckMode(efml4a::mode::ParseMode("donor"), efml4a::mode::kDonor, "\"donor\"");

        CheckMode(efml4a::mode::ParseMode("OFF"), efml4a::mode::kOff, "\"OFF\"（大写）");
        CheckMode(efml4a::mode::ParseMode("Recon"), efml4a::mode::kRecon, "\"Recon\"（混合大小写）");
        CheckMode(efml4a::mode::ParseMode("SKIP"), efml4a::mode::kSkip, "\"SKIP\"");
        CheckMode(efml4a::mode::ParseMode("DoNoR"), efml4a::mode::kDonor, "\"DoNoR\"");

        CheckMode(efml4a::mode::ParseMode("  skip  "), efml4a::mode::kSkip, "带首尾空白");
        CheckMode(efml4a::mode::ParseMode("\toff\n"), efml4a::mode::kOff, "带制表/换行");

        CheckMode(efml4a::mode::ParseMode(""), efml4a::mode::kOff, "空串 -> off");
        CheckMode(efml4a::mode::ParseMode(nullptr), efml4a::mode::kOff, "nullptr -> off");
        CheckMode(efml4a::mode::ParseMode("garbage"), efml4a::mode::kOff, "非法值 -> off");
        CheckMode(efml4a::mode::ParseMode("s k i p"), efml4a::mode::kOff, "中间有空格 -> off");
        CheckMode(efml4a::mode::ParseMode("don"), efml4a::mode::kOff, "前缀不完整 -> off");
        CheckMode(efml4a::mode::ParseMode("recon2"), efml4a::mode::kOff, "多余后缀 -> off");
        CheckMode(efml4a::mode::ParseMode("1"), efml4a::mode::kOff, "数字 -> off（只认单词）");
    }

    // -----------------------------------------------------------------------
    std::printf("\n[3] mode::ParseCount\n");
    {
        CheckU32(efml4a::mode::ParseCount("0", 42), 0, "\"0\" -> 0");
        CheckU32(efml4a::mode::ParseCount("1", 42), 1, "\"1\" -> 1");
        CheckU32(efml4a::mode::ParseCount("64", 42), 64, "\"64\" -> 64");
        CheckU32(efml4a::mode::ParseCount("  7 ", 42), 7, "带空白 -> 7");
        CheckU32(efml4a::mode::ParseCount("", 42), 42, "空串 -> fallback");
        CheckU32(efml4a::mode::ParseCount(nullptr, 42), 42, "nullptr -> fallback");
        CheckU32(efml4a::mode::ParseCount("abc", 42), 42, "非数字 -> fallback");
        CheckU32(efml4a::mode::ParseCount("12abc", 42), 42, "数字后有垃圾 -> fallback");
        CheckU32(efml4a::mode::ParseCount("-5", 42), 42, "负数 -> fallback");
        CheckU32(efml4a::mode::ParseCount("-", 42), 42, "只有负号 -> fallback");
        // 溢出/超大值：一律夹到上限 1000000（而不是回落），因为用户显然想要「很大」。
        CheckU32(efml4a::mode::ParseCount("2000000", 42), 1000000, "2000000 -> 夹到上限");
        CheckU32(efml4a::mode::ParseCount("999999999999", 42), 1000000,
                 "溢出（ERANGE）-> 夹到上限");
    }

    // -----------------------------------------------------------------------
    std::printf("\n[4] mode::Refresh（环境变量 -> 缓存）\n");
    {
        // 环境变量优先于系统属性；真机 app 进程拿不到环境变量，只能用属性。
        setenv("EFML4A_MODE", "donor", 1);
        setenv("EFML4A_DONOR_MAX", "7", 1);
        setenv("EFML4A_SKIP", "3", 1);
        efml4a::mode::Refresh();
        CheckMode(efml4a::mode::Current(), efml4a::mode::kDonor, "EFML4A_MODE=donor");
        CheckU32(efml4a::mode::DonorMax(), 7, "EFML4A_DONOR_MAX=7");
        CheckU32(efml4a::mode::SkipDraws(), 3, "EFML4A_SKIP=3");

        setenv("EFML4A_MODE", "SKIP", 1);
        efml4a::mode::Refresh();
        CheckMode(efml4a::mode::Current(), efml4a::mode::kSkip, "EFML4A_MODE=SKIP（大写）");

        setenv("EFML4A_MODE", "definitely-not-a-mode", 1);
        efml4a::mode::Refresh();
        CheckMode(efml4a::mode::Current(), efml4a::mode::kOff, "非法 EFML4A_MODE -> off");

        setenv("EFML4A_MODE", "", 1);
        setenv("EFML4A_SKIP", "", 1);
        unsetenv("EFML4A_DONOR_MAX");
        efml4a::mode::Refresh();
        CheckMode(efml4a::mode::Current(), efml4a::mode::kOff, "空 EFML4A_MODE -> off");
        CheckU32(efml4a::mode::SkipDraws(), 1, "空 EFML4A_SKIP -> 默认 1");
        CheckU32(efml4a::mode::DonorMax(), 64, "未设置的 EFML4A_DONOR_MAX -> 默认 64");
    }

    // -----------------------------------------------------------------------
    std::printf("\n[5] 落盘侦察 / 统计文件（走 EXTERNAL_STORAGE 兜底路径）\n");
    std::string dir;
    {
        // 造一个临时目录。真机上第三个候选就是 $EXTERNAL_STORAGE，
        // 这里把它指向临时目录，就能在没有 /data/data/<pkg> 的桌面上验证整条写盘路径。
        const char* tmpdir = std::getenv("TMPDIR");
        std::string base = (tmpdir != nullptr && *tmpdir != '\0') ? tmpdir : ".";
        std::string tmpl = base + "/efml4a-pure-XXXXXX";
        std::string buf = tmpl;
        char* made = mkdtemp(&buf[0]);
        if (made == nullptr) {
            base = ".";
            tmpl = base + "/efml4a-pure-XXXXXX";
            buf = tmpl;
            made = mkdtemp(&buf[0]);
        }
        dir = (made != nullptr) ? std::string(made) : std::string();
        Check(!dir.empty(), "创建临时输出目录");
        if (dir.empty()) {
            std::printf("FAILED\n");
            return 1;
        }
        std::printf("  temp output dir: %s\n", dir.c_str());
        setenv("EXTERNAL_STORAGE", dir.c_str(), 1);

        const std::string recon_path = dir + "/efml4a-recon.jsonl";
        const std::string stats_path = dir + "/efml4a-stats.json";

        // ---- 追加三行 ----
        const std::string l1 = "{\"seq\":0,\"call\":\"drawIndexed\"}";
        const std::string l2 = "{\"seq\":1,\"call\":\"draw\"}";
        const std::string l3 = "{\"seq\":2,\"call\":\"drawIndexed\"}";
        Check(efml4a::recon::AppendLine(l1), "AppendLine #1");
        Check(efml4a::recon::AppendLine(l2), "AppendLine #2");
        Check(efml4a::recon::AppendLine(l3), "AppendLine #3");
        CheckU32(static_cast<uint32_t>(efml4a::recon::LineCount()), 3, "LineCount() == 3");

        Check(efml4a::recon::OutputDir() == dir,
              "OutputDir() 就是 EXTERNAL_STORAGE 指向的目录（而不是 /data/data/...）");

        bool ok = false;
        std::string text = ReadWholeFile(recon_path, &ok);
        Check(ok, "侦察文件已创建: " + recon_path);
        Check(text == l1 + "\n" + l2 + "\n" + l3 + "\n", "文件内容 == 三次追加（每行一个 JSON + \\n）");
        CheckU32(static_cast<uint32_t>(CountLines(text)), 3, "文件里 3 行");

        // ---- 一直写到封顶 ----
        std::string filler = "{\"seq\":9999,\"call\":\"draw\"}";
        for (int i = 0; i < 2200 && efml4a::recon::LineCount() < 2000; ++i) {
            efml4a::recon::AppendLine(filler);
        }
        CheckU32(static_cast<uint32_t>(efml4a::recon::LineCount()), 2000,
                 "封顶后 LineCount() == 2000（总量上限）");
        Check(!efml4a::recon::AppendLine(filler), "封顶后再追加返回 false（停止写入）");
        Check(!efml4a::recon::AppendLine(filler), "再次追加仍然返回 false（capped 行只写一次）");

        text = ReadWholeFile(recon_path, &ok);
        Check(ok, "重新读取侦察文件");
        CheckU32(static_cast<uint32_t>(CountLines(text)), 2001,
                 "文件总行数 == 2001（2000 数据行 + 1 行 capped）");
        CheckStr(LastLine(text), "{\"recon\":\"capped\"}", "最后一行是 {\"recon\":\"capped\"}");

        // ---- 统计文件：覆盖写 + tmp/rename ----
        Check(efml4a::recon::WriteStats("{\"layer\":\"x\",\"mode\":\"skip\"}"), "WriteStats #1");
        text = ReadWholeFile(stats_path, &ok);
        Check(ok, "统计文件已创建: " + stats_path);
        CheckStr(text, "{\"layer\":\"x\",\"mode\":\"skip\"}\n", "统计文件内容");

        Check(efml4a::recon::WriteStats("{\"mode\":\"off\"}"), "WriteStats #2（覆盖写）");
        text = ReadWholeFile(stats_path, &ok);
        CheckStr(text, "{\"mode\":\"off\"}\n", "第二次覆盖写后的内容");

        bool tmp_ok = false;
        ReadWholeFile(stats_path + ".tmp", &tmp_ok);
        Check(!tmp_ok, "临时文件 .tmp 已被 rename 掉（不残留）");
    }

    // -----------------------------------------------------------------------
    std::printf("\n== %d checks, %d failures ==\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PURE TESTS PASSED\n");
    return 0;
}
