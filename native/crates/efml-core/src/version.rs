//! 版本协商：mod 声明的 loader 版本要求 vs 当前 loader 版本。
//!
//! `mod.toml` 的 `min_loader_version` 与 `target.game_version` 都采用「宽松写法」：
//! 既可以写完整的 semver 要求（`>=0.1.0, <0.3.0`），也可以只写一个裸版本号
//! （`0.1.0`，等价于 `>=0.1.0`）。这样 mod 作者不必记 semver 语法细节。

use semver::{Version, VersionReq};

use crate::{Error, Result};

/// 当前 loader 版本，取自 Cargo 包版本，是版本协商的权威来源。
pub const LOADER_VERSION: &str = env!("CARGO_PKG_VERSION");

/// 当前支持的 mod 包格式版本。
pub const SUPPORTED_FORMAT_VERSION: u32 = 1;

/// 把宽松写法规范化为标准 semver 要求字符串。
///
/// 规则：
/// - 以比较/通配运算符开头（`>`、`<`、`=`、`^`、`~`、`*`），或含有 `,` / 空格
///   → 视为已经写成了 semver 要求，原样返回（仅去首尾空白）。
/// - 其它情况（裸版本号）→ 前缀 `>=`。
pub fn normalize_requirement(raw: &str) -> String {
    let trimmed = raw.trim();
    let starts_with_operator = trimmed
        .chars()
        .next()
        .is_some_and(|c| matches!(c, '>' | '<' | '=' | '^' | '~' | '*'));
    if starts_with_operator || trimmed.contains(',') || trimmed.contains(' ') {
        trimmed.to_string()
    } else {
        format!(">={trimmed}")
    }
}

/// 解析宽松写法为 semver 要求。
pub fn parse_requirement(raw: &str) -> Result<VersionReq> {
    let normalized = normalize_requirement(raw);
    Ok(VersionReq::parse(&normalized)?)
}

/// 检查当前 loader 版本是否满足 mod 的要求。
///
/// `min_loader_version` 为 `None` 时视为无要求（兼容早期 mod）。
pub fn ensure_loader_compatible(min_loader_version: Option<&str>) -> Result<()> {
    let Some(raw) = min_loader_version else {
        return Ok(());
    };
    let req = parse_requirement(raw)?;
    let loader = loader_version()?;
    if req.matches(&loader) {
        Ok(())
    } else {
        Err(Error::package(format!(
            "该 mod 要求 loader 版本满足 `{req}`，但当前 loader 版本为 {loader}"
        )))
    }
}

/// 当前 loader 版本的解析结果。
pub fn loader_version() -> Result<Version> {
    Ok(Version::parse(LOADER_VERSION)?)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bare_version_becomes_greater_or_equal() {
        assert_eq!(normalize_requirement("0.1.0"), ">=0.1.0");
        assert_eq!(normalize_requirement("  0.1.0  "), ">=0.1.0");
    }

    #[test]
    fn explicit_requirement_is_preserved() {
        assert_eq!(normalize_requirement(">=0.1.0, <0.3.0"), ">=0.1.0, <0.3.0");
        assert_eq!(normalize_requirement("^0.1"), "^0.1");
        assert_eq!(normalize_requirement("*"), "*");
    }

    #[test]
    fn none_requirement_is_always_compatible() {
        assert!(ensure_loader_compatible(None).is_ok());
    }

    #[test]
    fn current_loader_satisfies_its_own_bare_version() {
        let v = loader_version().expect("loader 版本必须可解析");
        let bare = format!("{v}");
        assert!(ensure_loader_compatible(Some(&bare)).is_ok());
    }

    #[test]
    fn future_requirement_is_rejected() {
        let err = ensure_loader_compatible(Some(">=99.0.0")).unwrap_err();
        let msg = err.to_string();
        assert!(msg.contains("99.0.0"), "错误信息应包含要求版本: {msg}");
        assert!(msg.contains("loader"), "错误信息应说明 loader 版本: {msg}");
    }

    #[test]
    fn malformed_requirement_is_an_error() {
        assert!(ensure_loader_compatible(Some(">=not-a-version")).is_err());
    }
}
