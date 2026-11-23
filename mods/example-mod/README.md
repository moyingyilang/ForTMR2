# example-mod · 格式参考实现

这个包的作用是把 `docs/ARCHITECTURE.md` 定义的
**每一个 IR 段落都实际用一遍**，作为格式的活文档与校验器的回归样本。

## ⚠️ 数据说明

`mesh/` 与 `texture/` 下的内容是**合成占位数据**（全零 / 递增值），
**不包含任何来自游戏的资源**。它们只用来让「文件存在性」与「sha256 校验」两条校验链路有真实对象。
这些数据不能用于游戏，也不代表真实的模型或贴图。

## 覆盖的格式特性

| 特性 | 在本包中的体现 |
| --- | --- |
| 全部四种识别策略 | `pipeline-hash` / `vertex-signature` / `unity-path` / `asset-path` |
| 多后端声明 | `backends = ["vulkan-layer", "il2cpp-hook", "asset-bundle"]` |
| 完整顶点布局 | `stride = 40` + 5 个属性，含 `location`（Vulkan 用） |
| 子网格 | 1 个 `triangle-list` 子网格 |
| 骨骼映射 | 2 根骨骼 + 外部 `bindposes` 文件 |
| 形态键 | `smile`，`default_weight = 0.0` |
| 材质与贴图绑定 | `_MainTex` → `texture/body_diffuse.bin` |
| 贴图替换 | 声明 `format = "RGBA8_UNORM"` |
| 热重载 | `hot_reload = true` |
| 完整性校验 | 包内 `checksums.txt` 覆盖全部被引用文件 |

## 校验

```bash
cd native
cargo run -p efml-cli -- validate ../mods/example-mod
cargo run -p efml-cli -- info ../mods/example-mod
cargo run -p efml-cli -- checksum ../mods/example-mod        # 只打印
cargo run -p efml-cli -- checksum ../mods/example-mod --write # 重新生成 checksums.txt
```

## 已知的设计缺口（本示例刻意暴露）

1. **无依赖声明**。真实 EFMI mod 会强制依赖第三方框架（实测样本依赖 `RabbitFx v26`，
   见 [docs/archive/13](../../docs/archive/13-real-mod-analysis.md) §13.7），当前 manifest 无法表达。
2. **贴图缺色彩空间**。EFMI 用 `BC7-sRGB` / `BC7-Linear` 区分，
   本格式只有 `format`（§13.9 C-6）。
3. **无变体维度**。实测样本内含 G0/G1/G2 三套网格（疑似画质档位），当前 IR 无法表达多档变体
   （§13.10 N-4）。
4. **无部件开关语义**。EFMI mod 普遍提供按部件分组的用户开关
   （实测样本有 4 个 `$part_*` 变量与 4 个热键），当前 IR 只能整体开关。
