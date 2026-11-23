//! mod 包（目录）的加载与校验。
//!
//! 一个 mod 包就是一个目录，根目录下必须有 `mod.toml`，可选 `checksums.txt`。
//! 其余文件由 IR 引用，路径一律相对于包根。

use std::path::{Path, PathBuf};

use crate::checksum::{self, ChecksumMap};
use crate::manifest::{ModManifest, MANIFEST_FILE};
use crate::{Error, Result};

/// 校验结果：非致命告警 + 实际检查过的文件数。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ValidationReport {
    /// 非致命告警。为空表示完全干净。
    pub warnings: Vec<String>,
    /// 实际检查存在的被引用文件数。
    pub checked_files: usize,
}

impl ValidationReport {
    /// 是否没有任何告警。
    pub fn is_clean(&self) -> bool {
        self.warnings.is_empty()
    }
}

/// 加载失败的条目：(目录路径, 错误信息)。
///
/// 单独起别名是为了避免 `clippy::type_complexity`，同时让
/// [`ModPackage::load_all`] 的签名自解释。
pub type LoadFailure = (PathBuf, String);

/// [`ModPackage::load_all`] 的返回：成功加载的包 + 加载失败的条目。
pub type LoadAllResult = (Vec<ModPackage>, Vec<LoadFailure>);

/// 一个已加载的 mod 包。
#[derive(Debug, Clone)]
pub struct ModPackage {
    root: PathBuf,
    manifest: ModManifest,
    checksums: Option<ChecksumMap>,
}

impl ModPackage {
    /// 打开一个 mod 包目录：解析 `mod.toml`、校验清单、按需读取 `checksums.txt`。
    pub fn open(root: impl AsRef<Path>) -> Result<Self> {
        let root = root.as_ref().to_path_buf();
        if !root.is_dir() {
            return Err(Error::package(format!(
                "mod 包根目录不存在或不是目录: {}",
                root.display()
            )));
        }
        let manifest = ModManifest::load(&root)?;
        let checksums = checksum::read_checksums(&root)?;
        Ok(Self {
            root,
            manifest,
            checksums,
        })
    }

    /// 包根目录。
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// 清单。
    pub fn manifest(&self) -> &ModManifest {
        &self.manifest
    }

    /// `checksums.txt` 的内容（若存在）。
    pub fn checksums(&self) -> Option<&ChecksumMap> {
        self.checksums.as_ref()
    }

    /// 该 mod 是否声明与当前 loader 版本兼容。
    pub fn ensure_loader_compatible(&self) -> Result<()> {
        crate::version::ensure_loader_compatible(self.manifest.min_loader_version.as_deref())
    }

    /// 结构化校验：检查所有被引用文件是否存在、校验和覆盖是否完整。
    ///
    /// 返回 [`ValidationReport`] 表示校验通过（可能带告警）；返回 `Err` 表示存在硬性错误。
    pub fn validate(&self) -> Result<ValidationReport> {
        let mut warnings = self.manifest.warnings();
        let mut checked_files = 0usize;
        let referenced = self.manifest.referenced_files();

        for rel in &referenced {
            let path = self.root.join(rel);
            if path.is_file() {
                checked_files += 1;
            } else {
                return Err(Error::package(format!(
                    "IR 引用的文件不存在: {rel}（期望路径 {}）",
                    path.display()
                )));
            }
        }

        match &self.checksums {
            None => warnings.push(
                "缺少 checksums.txt：建议用 `efml pack` 或 `sha256sum` 生成，以便加载器校验完整性"
                    .into(),
            ),
            Some(map) => {
                for rel in &referenced {
                    if !map.contains_key(rel) {
                        warnings.push(format!("checksums.txt 未覆盖被引用文件: {rel}"));
                    }
                }
                for rel in map.keys() {
                    if !self.root.join(rel).is_file() {
                        warnings.push(format!("checksums.txt 列出的文件不存在: {rel}"));
                    }
                }
            }
        }

        if !self.manifest.ir.fully_hot_reloadable() {
            warnings.push("部分条目未声明 hot_reload：改动这些条目将需要重启游戏".into());
        }

        Ok(ValidationReport {
            warnings,
            checked_files,
        })
    }

    /// 按 `checksums.txt` 逐文件校验 sha256，返回校验通过的文件数。
    ///
    /// 包内没有 `checksums.txt` 时返回错误——调用方应先通过
    /// [`ModPackage::checksums`] 判断是否存在。
    pub fn verify_checksums(&self) -> Result<usize> {
        let Some(map) = &self.checksums else {
            return Err(Error::package(format!(
                "{} 中没有 checksums.txt，无法校验完整性",
                self.root.display()
            )));
        };
        let mut verified = 0usize;
        for (rel, expected) in map {
            let path = self.root.join(rel);
            if !path.is_file() {
                return Err(Error::package(format!(
                    "checksums.txt 列出的文件不存在: {rel}"
                )));
            }
            checksum::verify_file(&path, expected)?;
            verified += 1;
        }
        Ok(verified)
    }

    /// 扫描一个目录，返回其下所有含 `mod.toml` 的直接子目录（按字典序）。
    ///
    /// 这是加载器的「Mods 目录」语义，与 EFMI 把 mod 文件夹丢进 `Mods/` 的用法对应。
    pub fn scan_dir(dir: impl AsRef<Path>) -> Result<Vec<PathBuf>> {
        let dir = dir.as_ref();
        if !dir.is_dir() {
            return Err(Error::package(format!(
                "Mods 目录不存在或不是目录: {}",
                dir.display()
            )));
        }
        let mut found = Vec::new();
        for entry in std::fs::read_dir(dir)? {
            let entry = entry?;
            let path = entry.path();
            if path.is_dir() && path.join(MANIFEST_FILE).is_file() {
                found.push(path);
            }
        }
        found.sort();
        Ok(found)
    }

    /// 扫描并加载一个 Mods 目录下的全部 mod 包。
    ///
    /// 返回 `(成功加载的包, 加载失败的 (路径, 错误信息))`，使调用方可以展示
    /// 「哪一个 mod 坏了」而不是整体失败。
    pub fn load_all(dir: impl AsRef<Path>) -> Result<LoadAllResult> {
        let dirs = Self::scan_dir(dir)?;
        let mut ok = Vec::new();
        let mut failed = Vec::new();
        for path in dirs {
            match Self::open(&path) {
                Ok(pkg) => ok.push(pkg),
                Err(e) => failed.push((path, e.to_string())),
            }
        }
        Ok((ok, failed))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    /// 在系统临时目录里造一个 mod 包，避免给仓库引入测试夹具目录。
    fn make_package(name: &str, manifest: &str, files: &[(&str, &[u8])]) -> PathBuf {
        let root =
            std::env::temp_dir().join(format!("efml4a-test-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("应能创建临时目录");
        fs::write(root.join(MANIFEST_FILE), manifest).expect("应能写 mod.toml");
        for (rel, data) in files {
            let path = root.join(rel);
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent).expect("应能创建子目录");
            }
            fs::write(path, data).expect("应能写数据文件");
        }
        root
    }

    const MANIFEST: &str = r#"
format_version = 1
id = "demo"
name = "Demo"
version = "0.1.0"
license = "AGPL-3.0-or-later"
backends = ["vulkan-layer"]

[target]
game = "arknights-endfield"

[[mesh.replace]]
identify = { kind = "pipeline-hash", hash = "abcd" }
vertex_count = 3
hot_reload = true
source = "mesh/a.bin"
"#;

    #[test]
    fn open_and_validate_a_complete_package() {
        let root = make_package("ok", MANIFEST, &[("mesh/a.bin", b"hello")]);
        let pkg = ModPackage::open(&root).expect("应能打开");
        assert_eq!(pkg.manifest().id, "demo");
        let report = pkg.validate().expect("应能通过校验");
        assert_eq!(report.checked_files, 1);
        // 没有 checksums.txt → 应有一条告警。
        assert!(report.warnings.iter().any(|w| w.contains("checksums.txt")));
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn missing_referenced_file_is_an_error() {
        let root = make_package("missing", MANIFEST, &[]);
        let pkg = ModPackage::open(&root).expect("清单本身合法");
        let err = pkg.validate().unwrap_err();
        assert!(err.to_string().contains("mesh/a.bin"), "{err}");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn checksum_verification_roundtrip() {
        let data = b"hello";
        let digest = crate::checksum::sha256_bytes(data);
        let checksums = format!("# demo\n{digest}  mesh/a.bin\n");
        let root = make_package("ck", MANIFEST, &[("mesh/a.bin", data)]);
        fs::write(root.join("checksums.txt"), checksums).expect("应能写校验和");
        let pkg = ModPackage::open(&root).expect("应能打开");
        assert_eq!(pkg.verify_checksums().expect("应校验通过"), 1);
        let report = pkg.validate().expect("应能通过校验");
        assert!(
            report
                .warnings
                .iter()
                .all(|w| !w.contains("checksums.txt 未覆盖")),
            "校验和已覆盖该文件，不应告警: {:?}",
            report.warnings
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn tampered_file_fails_checksum_verification() {
        let data = b"hello";
        let digest = crate::checksum::sha256_bytes(data);
        let checksums = format!("{digest}  mesh/a.bin\n");
        let root = make_package("tamper", MANIFEST, &[("mesh/a.bin", b"tampered")]);
        fs::write(root.join("checksums.txt"), checksums).expect("应能写校验和");
        let pkg = ModPackage::open(&root).expect("应能打开");
        let err = pkg.verify_checksums().unwrap_err();
        assert!(err.to_string().contains("校验和不匹配"), "{err}");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn missing_manifest_is_an_error() {
        let root =
            std::env::temp_dir().join(format!("efml4a-test-nomanifest-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("应能创建目录");
        let err = ModPackage::open(&root).unwrap_err();
        assert!(err.to_string().contains("mod.toml"), "{err}");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn load_all_collects_broken_packages_without_failing() {
        let base = std::env::temp_dir().join(format!("efml4a-test-mods-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        fs::create_dir_all(&base).expect("应能创建 Mods 目录");
        // 一个好包
        let good = base.join("good");
        fs::create_dir_all(good.join("mesh")).expect("应能创建");
        fs::write(good.join(MANIFEST_FILE), MANIFEST).expect("应能写清单");
        fs::write(good.join("mesh/a.bin"), b"x").expect("应能写数据");
        // 一个坏包：mod.toml 内容非法
        let bad = base.join("bad");
        fs::create_dir_all(&bad).expect("应能创建");
        fs::write(bad.join(MANIFEST_FILE), "format_version = 1\n").expect("应能写清单");

        let (ok, failed) = ModPackage::load_all(&base).expect("应能扫描");
        assert_eq!(ok.len(), 1);
        assert_eq!(failed.len(), 1);
        assert!(failed[0].0.ends_with("bad"));
        let _ = fs::remove_dir_all(&base);
    }
}
