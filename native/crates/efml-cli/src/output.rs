//! 输出格式化：人类可读表格 与 JSON 两种模式。
//!
//! JSON 模式是刻意保留的一等公民：后续的 Android 管理器 App 消费
//! 同一份 JSON，避免把展示逻辑复制三遍。

use std::path::Path;

use anyhow::Result;
use efml_core::{ModPackage, ValidationReport};

/// 输出单个 mod 包的清单信息（可选带校验报告）。
pub fn emit_package(pkg: &ModPackage, report: Option<&ValidationReport>, json: bool) -> Result<()> {
    let m = pkg.manifest();
    if json {
        let value = serde_json::json!({
            "root": pkg.root().display().to_string(),
            "manifest": m,
            "referenced_files": m.referenced_files(),
            "suggested_backends": m.suggested_backends().iter().map(|b| b.as_str()).collect::<Vec<_>>(),
            "warnings": m.warnings(),
            "has_checksums": pkg.checksums().is_some(),
            "report": report.map(|r| serde_json::json!({
                "warnings": r.warnings,
                "checked_files": r.checked_files,
                "clean": r.is_clean(),
            })),
        });
        println!("{}", serde_json::to_string_pretty(&value)?);
        return Ok(());
    }

    println!("{}", m.summary_line());
    println!("  目录      : {}", pkg.root().display());
    if let Some(d) = &m.description {
        println!("  描述      : {d}");
    }
    if !m.authors.is_empty() {
        println!("  作者      : {}", m.authors.join(", "));
    }
    println!(
        "  许可      : {}",
        m.license.as_deref().unwrap_or("(未声明)")
    );
    println!("  目标游戏  : {}", m.target.game);
    if let Some(v) = &m.target.game_version {
        println!("  游戏版本  : {v}");
    }
    if let Some(a) = &m.target.abi {
        println!("  ABI       : {a}");
    }
    println!(
        "  loader 要求: {}",
        m.min_loader_version.as_deref().unwrap_or("(无要求)")
    );
    println!("  包格式版本: {}", m.format_version);
    println!(
        "  校验和    : {}",
        if pkg.checksums().is_some() {
            format!("已提供 {} 条", pkg.checksums().map_or(0, |c| c.len()))
        } else {
            "缺失".to_string()
        }
    );

    let suggested = m.suggested_backends();
    if !suggested.is_empty() {
        let list = suggested
            .iter()
            .map(|b| b.as_str())
            .collect::<Vec<_>>()
            .join(", ");
        println!("  IR 暗示后端: {list}");
    }

    println!("\nIR 概览:");
    println!(
        "  mesh.replace    {} 项\n  skeleton.map    {} 项\n  blendshape      {} 项\n  material.override {} 项\n  texture.replace {} 项",
        m.ir.mesh.replace.len(),
        m.ir.skeleton.map.len(),
        m.ir.blendshape.len(),
        m.ir.material.overrides.len(),
        m.ir.texture.replace.len()
    );

    let files = m.referenced_files();
    println!("\n被引用文件 ({}):", files.len());
    for rel in &files {
        let present = pkg.root().join(rel).is_file();
        println!("  [{}] {rel}", if present { "✓" } else { "✗" });
    }

    let warnings = report.map_or_else(|| m.warnings(), |r| r.warnings.clone());
    if warnings.is_empty() {
        println!("\n校验: 通过，无告警");
    } else {
        println!("\n校验: 通过，{} 条告警", warnings.len());
        for w in &warnings {
            println!("  ! {w}");
        }
    }
    Ok(())
}

/// 输出一个 Mods 目录下所有包的结果。
pub fn emit_directory(
    dir: &Path,
    reports: &[(&ModPackage, efml_core::Result<ValidationReport>)],
    failed: &[(std::path::PathBuf, String)],
    json: bool,
) -> Result<()> {
    if json {
        let ok: Vec<serde_json::Value> = reports
            .iter()
            .map(|(pkg, report)| {
                serde_json::json!({
                    "root": pkg.root().display().to_string(),
                    "id": pkg.manifest().id,
                    "name": pkg.manifest().name,
                    "version": pkg.manifest().version,
                    "backends": pkg.manifest().backends.iter().map(|b| b.as_str()).collect::<Vec<_>>(),
                    "summary": pkg.manifest().summary_line(),
                    "warnings": match report {
                        Ok(r) => r.warnings.clone(),
                        Err(e) => vec![e.to_string()],
                    },
                    "valid": report.is_ok(),
                })
            })
            .collect();
        let bad: Vec<serde_json::Value> = failed
            .iter()
            .map(|(path, err)| {
                serde_json::json!({
                    "root": path.display().to_string(),
                    "error": err,
                })
            })
            .collect();
        let value = serde_json::json!({
            "mods_dir": dir.display().to_string(),
            "count": ok.len(),
            "failed_count": bad.len(),
            "packages": ok,
            "failed": bad,
        });
        println!("{}", serde_json::to_string_pretty(&value)?);
        return Ok(());
    }

    println!("Mods 目录: {}", dir.display());
    println!(
        "共 {} 个可加载的 mod，{} 个加载失败\n",
        reports.len(),
        failed.len()
    );

    if !reports.is_empty() {
        println!("{:<24} {:<10} {:<28} 名称", "ID", "版本", "后端");
        println!("{}", "-".repeat(88));
        for (pkg, report) in reports {
            let m = pkg.manifest();
            let backends = m
                .backends
                .iter()
                .map(|b| b.as_str())
                .collect::<Vec<_>>()
                .join(",");
            let flag = match report {
                Ok(r) if r.is_clean() => "✓",
                Ok(_) => "!",
                Err(_) => "✗",
            };
            println!(
                "{:<24} {:<10} {:<28} {} {}",
                m.id, m.version, backends, flag, m.name
            );
        }
    }

    if !failed.is_empty() {
        println!("\n加载失败的目录:");
        for (path, err) in failed {
            println!("  ✗ {} — {}", path.display(), err);
        }
    }
    Ok(())
}

/// 输出四条技术路线的评估矩阵。
pub fn emit_routes(json: bool) {
    if json {
        let routes: Vec<serde_json::Value> = ROUTES
            .iter()
            .map(|r| {
                serde_json::json!({
                    "route": r.name,
                    "pc_analog": r.pc_analog,
                    "prerequisite": r.prerequisite,
                    "injection_point": r.injection_point,
                    "granularity": r.granularity,
                    "feasibility": r.feasibility,
                    "effort": r.effort,
                    "stability": r.stability,
                    "detection_risk": r.detection_risk,
                    "version_robustness": r.version_robustness,
                })
            })
            .collect();
        let value = serde_json::json!({ "routes": routes });
        match serde_json::to_string_pretty(&value) {
            Ok(s) => println!("{s}"),
            Err(e) => eprintln!("JSON 序列化失败: {e}"),
        }
        return;
    }

    println!("EFML4A 技术路线评估矩阵");
    println!("（详细论证见 docs/04 ~ docs/07，本表是速查版）\n");
    println!(
        "{:<22} {:<14} {:<12} {:<12} {:<10} 版本鲁棒性",
        "路线", "PC 类比", "可行性", "工程量", "检测风险"
    );
    println!("{}", "-".repeat(92));
    for r in ROUTES {
        println!(
            "{:<22} {:<14} {:<12} {:<12} {:<10} {:<12}",
            r.name, r.pc_analog, r.feasibility, r.effort, r.detection_risk, r.version_robustness
        );
    }
    println!("\n前置条件:");
    for r in ROUTES {
        println!("  · {} → {}", r.name, r.prerequisite);
    }
    println!("\n拦截粒度:");
    for r in ROUTES {
        println!("  · {} → {}", r.name, r.granularity);
    }
}

use efml_core::routes::ROUTES;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn route_table_covers_four_routes() {
        assert_eq!(ROUTES.len(), 4);
        assert!(ROUTES.iter().any(|r| r.name.contains("Vulkan")));
        assert!(ROUTES.iter().any(|r| r.name.contains("IL2CPP")));
        assert!(ROUTES.iter().any(|r| r.name.contains("AssetBundle")));
        assert!(ROUTES.iter().any(|r| r.name.contains("frida")));
    }

    #[test]
    fn emit_routes_json_is_disabled_safe() {
        // 只要求不 panic；输出走 stdout，不在此断言内容。
        emit_routes(true);
        emit_routes(false);
    }
}
