# 语言政策 / Language Policy

本项目的最终实现语言必须是 C 系语言。

| 允许 | 用途 |
|---|---|
| C（C11/C17） | 核心工具链（VFS 解析、压缩、加密、重建） |
| C++（C++17/20） | Vulkan 层骨架、纹理处理 |
| C# / .NET | 桌面工具（可选，目前主仓没有） |

禁止 Python、JavaScript/TypeScript（作为实现语言），以及任何非 C 系语言的实现代码。

注：构建脚本可使用 Shell（POSIX/bash）。文档可使用 Markdown。

现状（2026-11-22 核对）：

```
C 源文件:   tools/vfs 75 个，tools/ 全部 129 个
C 工具:     make all 编 23 个（当前清单以 `make list` 为准）；CLI 集成 90 个带 main 的源文件
C++:        vulkan-layer 骨架、astcenc（ARM 源码编译）
C#:         4 个参考文件（tools/vfs/reference-*.cs，只读参考，不参与构建）
Python:     0
```

> `native/` 里的 Rust 骨架是早期规划，已冻结，不再扩展。后续实现不要新增 Rust；
> 需要复用旧代码时先看 [docs/H1-repo-map.md](docs/H1-repo-map.md)。

`.gitignore` 已排除 `*.py` / `*.pyc` / `__pycache__/`。
