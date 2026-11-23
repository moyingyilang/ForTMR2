# 语言政策：CRC（C / Rust / C++）

**C 写主链路，Rust 写叶子库，C++ 写宿主/复杂模块；禁止 Python / JavaScript 实现。**

| 语言 | 位置 | 规则 |
|---|---|---|
| C（C11/C17） | VFS、loader、CLI、低层解析、ABI、构建 glue | 默认选择；主链路不引入运行时/GC/异常 |
| Rust | 密码、哈希、压缩、JSON/serde、复杂但独立的解析器 | 只做叶子；C ABI；不影响主链路控制流 |
| C++（C++17/20） | Vulkan 层、渲染/纹理、复杂宿主模块 | 边界用 `extern "C"`；不把 STL 类型跨 ABI |
| C# / .NET | 只读参考代码 | 不参与构建，不新增实现 |
| Python / JS / TS | 禁止 | 仅文档/构建脚本可用 shell |

## CRC 的边界

**C 是主干**：VFS、热重载、注入、CLI dispatch、资源生命周期、游戏结构体。
这些路径要求可预测的延迟、无 panic、无隐藏分配。

**Rust 是叶子**：只在“标准库/成熟 crate 明显更划算”时使用，例如
`chacha20` / `sha2` / `crc32c` / `zstd` / `serde`。
Rust 不接管主循环，不跨 FFI 传播 panic，不在 hook 热路径里做分配。

**C++ 是宿主**：Vulkan 层、渲染、复杂对象管理。C++ 只在边界内使用；
跨模块一律 C ABI。

## FFI 硬规则

1. 公共 ABI 只认 C 头文件（`extern "C"` / `#[no_mangle] extern "C"`）。
2. Rust 导出：opaque 指针 + 显式 `create/destroy`；不导出 Rust 结构体布局。
3. `panic = "abort"`；禁止 unwinding 穿过 FFI。
4. 所有输入长度/边界在 FFI 入口检查；不信任调用方。
5. C++ 的 `std::string` / `std::vector` / 异常不得跨 ABI。
6. 叶子库静态链接优先；动态库只用于明确的可替换后端。
7. 新 Rust 依赖要过 `cargo audit`，不引入运行时/异步框架。

## 现状（2026-11-23）

- C：`tools/vfs` 与 `tools/` 主工具链，CLI 集成 90 个底层工具
- C++：`vulkan-layer`、astcenc
- Rust：`native/` 作为叶子 CLI/core，不接管主链路
- C#：仅 `tools/vfs/reference-*.cs` 只读参考
- Python：0

新增实现默认 C；只有满足“叶子 + C ABI + 有明确库收益”才用 Rust；
复杂宿主/渲染才用 C++。
