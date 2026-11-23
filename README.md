# ForTMR2

ForTMR 的公开分支。**不含 VFS 解密链**；VFS 后端、`.blc/.chk`、密钥和资源提取留在私有仓。

## 这是什么

一个面向《明日方舟：终末地》的 mod/加载器**公开基础设施**：

- `lib/`：与后端无关的资源源抽象（目录 / 内存）
- `native/`：Rust 的 mod 包格式、校验、路由与 CLI
- `vulkan-layer/`：Vulkan 层加载器（公开源码，不含 VFS）
- `mods/`：合成数据示例包，演示格式和完整性校验
- 文档：架构、构建、公开边界

VFS 是**可替换的外部后端**，不在本仓库；没有它时 lib 的目录/内存后端和格式工具仍然可用。

## 构建

```bash
make            # lib + demo
make test
```

可选：

```bash
cd native && cargo build --release
cmake -S vulkan-layer -B vulkan-layer/build && cmake --build vulkan-layer/build
```

## 公开边界

**不包含**：

- `.blc` / `.chk` 解密、ChaCha 密钥、VFSAES
- `tools/vfs` 的 VFS 工具链
- 游戏原始资源、解包产物、抓帧数据

**包含**：

- mod 格式、资源抽象、加载器框架、渲染层、示例
- 与 BEN 的格式桥接：[Converter4BENeXt](https://github.com/moyingyilang/Converter4BENeXt)

## 许可

GPL-3.0；`lib/` 为 LGPL-3.0。见 `LICENSE` 与 `lib/LICENSE`。
