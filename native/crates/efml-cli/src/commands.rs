//! 各子命令的实现。
//!
//! 约定：返回 `Ok(())` 表示命令成功执行；**校验发现的问题通过告警表达，不改变退出码**
//! ——只有无法完成操作（目录不存在、清单语法错误等）才返回 `Err`。这样 CI 里可以
//! 用 `efml validate` 作为门禁，而作者本地能反复看到告警。

use std::path::Path;

use anyhow::{bail, Context, Result};
use efml_core::manifest::MANIFEST_FILE;
use efml_core::{checksum, ModPackage};

use crate::output;

/// `efml validate`：校验单个 mod 包或整个 Mods 目录。
pub fn validate(path: &Path, json: bool) -> Result<()> {
    if !path.exists() {
        bail!("路径不存在: {}", path.display());
    }

    if path.join(MANIFEST_FILE).is_file() {
        let pkg = ModPackage::open(path)
            .with_context(|| format!("打开 mod 包失败: {}", path.display()))?;
        pkg.ensure_loader_compatible()
            .with_context(|| format!("mod `{}` 与当前 loader 不兼容", pkg.manifest().id))?;
        let report = pkg.validate().context("mod 包结构校验失败")?;
        output::emit_package(&pkg, Some(&report), json)?;
        return Ok(());
    }

    if !path.is_dir() {
        bail!("{} 既不是 mod 包也不是目录", path.display());
    }

    let (packages, failed) = ModPackage::load_all(path)?;
    let mut reports = Vec::new();
    for pkg in &packages {
        let report = pkg.validate();
        reports.push((pkg, report));
    }
    output::emit_directory(path, &reports, &failed, json)?;
    Ok(())
}

/// `efml info`：打印单个 mod 包的详细信息。
pub fn info(path: &Path, json: bool) -> Result<()> {
    let pkg =
        ModPackage::open(path).with_context(|| format!("打开 mod 包失败: {}", path.display()))?;
    output::emit_package(&pkg, None, json)?;
    Ok(())
}

/// `efml list`：列出 Mods 目录下的全部 mod 包。
pub fn list(dir: &Path, json: bool) -> Result<()> {
    let (packages, failed) = ModPackage::load_all(dir)
        .with_context(|| format!("扫描 Mods 目录失败: {}", dir.display()))?;
    let reports: Vec<(&ModPackage, efml_core::Result<efml_core::ValidationReport>)> =
        packages.iter().map(|p| (p, p.validate())).collect();
    output::emit_directory(dir, &reports, &failed, json)?;
    Ok(())
}

/// `efml checksum`：计算被引用文件的 sha256；`--write` 时写入 `checksums.txt`。
pub fn checksum(path: &Path, write: bool, json: bool) -> Result<()> {
    let pkg =
        ModPackage::open(path).with_context(|| format!("打开 mod 包失败: {}", path.display()))?;
    let files = pkg.manifest().referenced_files();
    if files.is_empty() {
        bail!("mod `{}` 没有引用任何数据文件", pkg.manifest().id);
    }

    let mut entries: Vec<(String, String)> = Vec::with_capacity(files.len());
    for rel in &files {
        let abs = pkg.root().join(rel);
        if !abs.is_file() {
            bail!("被引用文件不存在: {rel}");
        }
        let digest = checksum::sha256_file(&abs)
            .with_context(|| format!("计算 sha256 失败: {}", abs.display()))?;
        entries.push((rel.clone(), digest));
    }
    entries.sort();

    if write {
        let mut body = String::from("# 由 `efml checksum --write` 生成\n");
        for (rel, digest) in &entries {
            body.push_str(&format!("{digest}  {rel}\n"));
        }
        let target = pkg.root().join("checksums.txt");
        std::fs::write(&target, body).with_context(|| format!("写入失败: {}", target.display()))?;
        if json {
            println!(
                "{}",
                serde_json::json!({
                    "written": target.display().to_string(),
                    "files": entries.len(),
                })
            );
        } else {
            println!(
                "已写入 {} 个文件的校验和 → {}",
                entries.len(),
                target.display()
            );
        }
    } else if json {
        let map: serde_json::Map<String, serde_json::Value> = entries
            .iter()
            .map(|(k, v)| (k.clone(), serde_json::Value::String(v.clone())))
            .collect();
        println!("{}", serde_json::Value::Object(map));
    } else {
        for (rel, digest) in &entries {
            println!("{digest}  {rel}");
        }
    }
    Ok(())
}

/// `efml routes`：打印四条技术路线的评估矩阵。
pub fn routes(json: bool) -> Result<()> {
    output::emit_routes(json);
    Ok(())
}
