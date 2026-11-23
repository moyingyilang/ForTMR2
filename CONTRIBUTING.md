# 贡献指南

## 提交之前

这个项目是研究性质的，研究对象有敏感性。提交前确认三件事：

1. 你读过 [`docs/archive/08-threat-model.md`](docs/archive/08-threat-model.md) 的红线清单，改动没跨过去。
2. 你没有提交游戏原始资源、密钥、解密资产或真实设备标识。
3. 你的改动不引入「改数值 / 竞技作弊 / 绕过 DRM」类功能。

## 语言与工具

| 场景 | 约定 |
|---|---|
| 工具链、格式处理、加载器逻辑 | **C**（C11）；需要抽象时用 C++ |
| 桌面工具 | C / C++ / C#（.NET） |
| 构建与测试脚本 | Shell（POSIX / bash） |
| 文档 | 中文为主，英文摘要放 `README.en.md` |

**不许用 Python / JavaScript 作为实现语言**（构建脚本和 CI 临时命令也不建议用 Python，
仓库里现在没有 `.py` 文件，`.gitignore` 也拦着）。Rust 骨架（`native/`）是早期规划，已冻结，
别再往上加东西。

## 开发环境

这个项目是在 **Android / Termux** 上写的，下面的坑都是踩过的：

- **f2fs 上不能硬链接**。`/data/data/<pkg>/` 下 `ln` 会返回 `EACCES`，所以依赖「临时文件 + 硬链接」的原子写入在这里不可用。写文件用 heredoc（`cat > file <<'EOF'`），写完 `wc -l` 确认。
- **NDK 的 clang 是 x86-64 的**，在 aarch64 设备上跑不了。编 payload 用 Termux 自带的 clang，见 [`tools/zygisk/BUILD.md`](tools/zygisk/BUILD.md)。
- 本机没有 Android SDK，`android/` 编不了；CI 也只做静态检查，不产出 APK。
- 网络可能不稳，首次拉 Rust 依赖会慢。

## 构建与验证

```bash
make            # lib + tools/vfs + cli + 抽象层 demo
make test       # lib 回归 + CLI 自测 + demo
```

改了单个工具就单独编：

```bash
make -C tools/vfs
make -C lib
```

下面这些不在主线里，需要时再跑：

```bash
cd native && cargo test
cmake -S vulkan-layer -B vulkan-layer/build -G Ninja
cmake --build vulkan-layer/build && ctest --test-dir vulkan-layer/build --output-on-failure
```

**提交前至少把 `make test` 跑通。** CI 还会编 Rust 骨架、Vulkan 骨架和 Android 静态检查，
但 Android 那一路本来就编不出 APK，见 CI 里的说明。

## 代码与文档

- 改工具就改它对应的文档：`docs/B1-tools.md` 或 `docs/A*-G*` 里相关的篇；索引 `docs/00-INDEX.md` 顺手看一眼。
- 改技术结论时，代码、文档一起改，别只改一边。C2 里保留了旧结论，但顶部标了「已被 D3 更正」，这就是处理旧结论的模板。
- `docs/archive/` 是历史快照，**只增不改**；新结论写进编号文档，再回链归档。
- 每篇文档开头写「最后核对」日期；数字变了就改，别把过期数字留在正文里。
- 注释解释「为什么」，不复述代码做了什么。
- 新增外部结论要标核实状态（`✅ 已核实` / `⚠️ 未核实`），未核实的别当事实写。
- 不提交 `*.bak`、编译产物、游戏资源（`.gitignore` 已拦大部分）。

## 提交信息与许可

提交信息用 Conventional Commits 前缀，正文中文：

```
feat(vfs): blcsetdata 加 CRC 回填
fix(rtloader): 插件句柄按文件名缓存
docs(H1): 更新仓库地图
```

提交即表示同意以 **GPL-3.0-or-later** 授权你的贡献。

第三方代码先做许可证兼容分析：主仓 GPL-3.0，`lib/` 是 LGPL-3.0（原 EFML4A-repack 的许
可延续），EFMI 系是 GPL-3.0。拿不准就在 Issue 里问，别直接贴进来。
