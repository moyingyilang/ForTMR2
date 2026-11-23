# Mods

示例包只有合成占位数据，不含游戏资源。

- `example-mod/`：`mod.toml` + IR 数据，展示顶点声明、子网格、骨骼、blendshape、
  材质/贴图引用与 `checksums.txt`。

校验：

```bash
cd native && cargo run -- release --help
```

格式规范在 `docs/ARCHITECTURE.md`；完整规范作为私有研究资料保留。
