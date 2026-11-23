# 构建

## C 库 / demo

```bash
make
make test
```

Termux：`pkg install clang zlib`。

## Rust

```bash
cd native
cargo build --release
```

Rust 1.82+。

## Vulkan 层

```bash
cmake -S vulkan-layer -B vulkan-layer/build
cmake --build vulkan-layer/build
```

需要 Vulkan headers / loader；构建产物不提交。

## Android

Android 注入/管理端代码不在公开仓。公开的 Vulkan 层可以移植到自己的壳里。
