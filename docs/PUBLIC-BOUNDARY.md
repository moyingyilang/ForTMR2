# 公开边界

## 不公开

- VFS 索引/`.blc`/`.chk`、ChaCha 密钥、VFSAES、资源解包
- 游戏资源、解密资产、抓帧导出
- 运行时内存 dump、key dump、注入 payload

原因：这些内容涉及游戏资源保护，公开分发有 DMCA/ToS 风险。

## 公开

- 资源源抽象（dir/mem）
- mod 格式、校验、路由、CLI
- Vulkan 层源码
- 合成示例
- 与 BEN 的 BEM 转换器（独立仓）

VFS 作为**可选私有后端**，通过进程边界接入；公开构建不依赖它。
