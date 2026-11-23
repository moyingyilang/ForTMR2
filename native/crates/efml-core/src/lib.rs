//! # EFML4A 核心库
//!
//! **EFML4A**（Arknights: Endfield Mod Loader for Android）是关于「把 PC 上
//! 3DMigoto 风格的换模型 mod 加载器移植到 Android」的研究项目。本 crate 是整个
//! 项目的中枢：它定义**与执行后端无关**的 mod 描述模型，使同一份 mod 包可以被
//! 不同后端分别执行。
//!
//! ## 为什么要 IR
//!
//! 对标的 [EFMI](https://github.com/SpectrumQT/EFMI-Package) 在 PC 上把 mod 表达为
//! 「ini + HLSL + Buffer/Texture 目录」，只有 3DMigoto 自己能解释这套东西。Android
//! 上不存在 DXGI，那套注入前提消失，必须重新选型（Vulkan 隐式层 / IL2CPP hook /
//! AssetBundle 替换，见 `docs/07-route-matrix.md`）。如果 mod 格式与后端实现绑死，
//! 换后端就等于让所有 mod 重写一遍。因此本项目先定义 IR，再由后端编译成具体动作。
//!
//! ## 模块
//!
//! - [`manifest`]：`mod.toml` 清单、字段校验、后端与目标声明
//! - [`ir`]：网格/骨骼/形态键/材质/贴图的声明式描述
//! - [`identify`]：draw call 与资源的识别策略（各后端的公共词汇）
//! - [`package`]：mod 包目录的加载、结构校验与完整性校验
//! - [`routes`]：四条 Android 注入路线的评估矩阵（研究结论的结构化版本）
//! - [`checksum`]：sha256 工具与 `checksums.txt` 解析
//! - [`version`]：loader 版本协商与包格式版本
//! - [`error`]：统一错误类型
//!
//! ## 示例
//!
//! ```
//! use efml_core::ModManifest;
//!
//! let src = r#"
//! format_version = 1
//! id = "demo"
//! name = "Demo"
//! version = "0.1.0"
//! backends = ["vulkan-layer"]
//!
//! [target]
//! game = "arknights-endfield"
//!
//! [[mesh.replace]]
//! identify = { kind = "pipeline-hash", hash = "abcd" }
//! vertex_count = 3
//! source = "mesh/a.bin"
//! "#;
//!
//! let manifest = ModManifest::from_toml_str(src).expect("清单应合法");
//! assert_eq!(manifest.id, "demo");
//! assert_eq!(manifest.referenced_files(), vec!["mesh/a.bin".to_string()]);
//! ```
//!
//! ## 许可
//!
//! AGPL-3.0-or-later。详见仓库根 `LICENSE`。

#![warn(missing_docs)]
#![warn(rustdoc::broken_intra_doc_links)]

pub mod checksum;
pub mod error;
pub mod identify;
pub mod ir;
pub mod manifest;
pub mod package;
pub mod routes;
pub mod version;

pub use error::{Error, Result};
pub use identify::Identify;
pub use ir::ModIr;
pub use manifest::{Backend, ModManifest, ModTarget};
pub use package::{ModPackage, ValidationReport};
