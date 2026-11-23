# lib —— 资源源抽象（LGPL-3.0）

公开部分只有：

- `efml4a_source` 接口
- `dir` 后端（普通目录）
- `mem` 后端（内存块）

VFS 后端、密钥和 `.blc/.chk` 解析不在本仓库；这是 ForTMR2 的公开边界。

```c
efml4a_source *s = efml4a_source_dir_new("mods");
if (s->open(s) == 0) {
    for (long i = 0; i < s->count(s); i++) { /* stat / read */ }
}
efml4a_source_free(s);
```
