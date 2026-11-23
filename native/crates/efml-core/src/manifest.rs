//! `mod.toml` 清单模型与字段校验。
//!
//! 字段规范见 `docs/10-mod-format.md`。设计要点：
//! - **未知字段一律忽略**（不使用 `deny_unknown_fields`），保证旧 loader 能读新 mod。
//! - 所有被引用的数据文件路径必须是**相对包根的**、且不含 `..`，避免 mod 包越界读写。
//! - IR 既可以内联在本文件，也可以通过 `entry` 指向外部 TOML。

use std::collections::BTreeSet;
use std::path::Path;

use serde::{Deserialize, Serialize};

use crate::ir::ModIr;
use crate::{Error, Result};

/// `mod.toml` 的文件名。
pub const MANIFEST_FILE: &str = "mod.toml";

/// mod id 的最大长度。
pub const ID_MAX_LEN: usize = 64;

/// 目标游戏标识（Arknights: Endfield）。
pub const GAME_ENDFIELD: &str = "arknights-endfield";

/// mod 的执行后端。
///
/// **不要**用 `rename_all = "kebab-case"`：它会把 `Il2CppHook` 变成 `il2-cpp-hook`，
/// 与 [`Backend::as_str`] 和文档中的 `il2cpp-hook` 不一致。
/// 这里对每个变体显式写出 TOML 名，并由
/// `backend_serde_names_match_as_str` 测试锁定「serde 名 == as_str()」这一不变量。
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub enum Backend {
    /// Vulkan 隐式层：最接近 PC 上 3DMigoto 的路线。
    #[serde(rename = "vulkan-layer")]
    VulkanLayer,
    /// Unity IL2CPP 运行时 hook：在 Mesh 层面替换。
    #[serde(rename = "il2cpp-hook")]
    Il2CppHook,
    /// AssetBundle 磁盘替换：不 hook 运行时。
    #[serde(rename = "asset-bundle")]
    AssetBundle,
}

impl Backend {
    /// 全部后端，顺序即默认优先级。
    pub const ALL: [Backend; 3] = [
        Backend::VulkanLayer,
        Backend::Il2CppHook,
        Backend::AssetBundle,
    ];

    /// kebab-case 名称，与 TOML 中的写法一致。
    pub fn as_str(self) -> &'static str {
        match self {
            Self::VulkanLayer => "vulkan-layer",
            Self::Il2CppHook => "il2cpp-hook",
            Self::AssetBundle => "asset-bundle",
        }
    }
}

impl std::fmt::Display for Backend {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// 目标游戏与版本范围。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ModTarget {
    /// 目标游戏标识，例如 `arknights-endfield`。
    pub game: String,
    /// 可选的游戏版本要求（宽松写法）。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub game_version: Option<String>,
    /// 可选的目标 ABI，例如 `arm64-v8a`。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub abi: Option<String>,
}

/// `mod.toml` 的根结构。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ModManifest {
    /// 包格式版本，当前只支持 [`crate::version::SUPPORTED_FORMAT_VERSION`]。
    pub format_version: u32,
    /// mod 唯一标识，小写字母/数字/`-`/`_`。
    pub id: String,
    /// 人类可读名称。
    pub name: String,
    /// mod 版本（semver）。
    pub version: String,
    /// 作者列表。
    #[serde(default)]
    pub authors: Vec<String>,
    /// 可选的许可证标识（建议使用 SPDX 标识符）。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub license: Option<String>,
    /// 可选描述。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub description: Option<String>,
    /// 目标游戏与版本范围。
    pub target: ModTarget,
    /// 可选的最低 loader 版本要求（宽松写法）。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub min_loader_version: Option<String>,
    /// 声明支持的执行后端，不能为空。
    #[serde(default)]
    pub backends: Vec<Backend>,
    /// 可选的外部 IR 文件路径（相对包根）。省略时 IR 内联在本文件。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub entry: Option<String>,
    /// 内联 IR。
    ///
    /// 用 `flatten` 展开到 TOML 的根层，因此 `mod.toml` 里写的是 `[[mesh.replace]]`
    /// 而不是 `[[ir.mesh.replace]]`（见 `docs/10-mod-format.md` 的示例）。
    ///
    /// 这里有一个**必须记住的坑**：展平意味着任意未知的根层键都会被当作 IR 的字段去解析。
    /// 由于 [`ModIr`] 未启用 `deny_unknown_fields`，真正的未知键仍会被静默忽略——
    /// 这正是 `efml validate` 的告警要兜底的地方（见 `ModManifest::warnings`）。
    #[serde(default, flatten)]
    pub ir: ModIr,
}

impl ModManifest {
    /// 从 TOML 字符串解析并校验（不解析外部 `entry`）。
    pub fn from_toml_str(content: &str) -> Result<Self> {
        let manifest = Self::parse(content)?;
        manifest.validate()?;
        Ok(manifest)
    }

    /// 只解析，不校验。
    fn parse(content: &str) -> Result<Self> {
        Ok(toml::from_str(content)?)
    }

    /// 从 mod 包根目录加载 `mod.toml`，并在声明了 `entry` 时合并外部 IR。
    pub fn load(root: &Path) -> Result<Self> {
        let path = root.join(MANIFEST_FILE);
        let content = std::fs::read_to_string(&path)
            .map_err(|e| Error::package(format!("无法读取 {}: {e}", path.display())))?;
        let mut manifest = Self::parse(&content)?;

        if let Some(entry) = manifest.entry.clone() {
            validate_relative_path("entry", &entry)?;
            let entry_path = root.join(&entry);
            let entry_content = std::fs::read_to_string(&entry_path).map_err(|e| {
                Error::package(format!(
                    "无法读取 entry 指向的 IR 文件 {}: {e}",
                    entry_path.display()
                ))
            })?;
            manifest.ir = toml::from_str(&entry_content)?;
        }

        manifest.validate()?;
        Ok(manifest)
    }

    /// 序列化为 TOML 字符串。
    pub fn to_toml_string(&self) -> Result<String> {
        Ok(toml::to_string_pretty(self)?)
    }

    /// 校验清单的全部字段与内联 IR。
    pub fn validate(&self) -> Result<()> {
        if self.format_version != crate::version::SUPPORTED_FORMAT_VERSION {
            return Err(Error::manifest(format!(
                "不支持的包格式版本 {}，当前只支持 {}",
                self.format_version,
                crate::version::SUPPORTED_FORMAT_VERSION
            )));
        }
        validate_id(&self.id)?;
        if self.name.trim().is_empty() {
            return Err(Error::manifest("name 不能为空"));
        }
        semver::Version::parse(&self.version).map_err(|e| {
            Error::manifest(format!("version `{}` 不是合法 semver: {e}", self.version))
        })?;
        if self.target.game.trim().is_empty() {
            return Err(Error::manifest("target.game 不能为空"));
        }
        if let Some(v) = &self.target.game_version {
            crate::version::parse_requirement(v)
                .map_err(|e| Error::manifest(format!("target.game_version `{v}` 非法: {e}")))?;
        }
        if let Some(v) = &self.min_loader_version {
            crate::version::parse_requirement(v)
                .map_err(|e| Error::manifest(format!("min_loader_version `{v}` 非法: {e}")))?;
        }
        if let Some(entry) = &self.entry {
            validate_relative_path("entry", entry)?;
        }
        if self.backends.is_empty() {
            return Err(Error::manifest(
                "backends 不能为空：至少声明一个执行后端（vulkan-layer / il2cpp-hook / asset-bundle）",
            ));
        }
        let mut seen = BTreeSet::new();
        for b in &self.backends {
            if !seen.insert(*b) {
                return Err(Error::manifest(format!("backends 中重复声明了 `{b}`")));
            }
        }
        self.ir.validate()?;
        Ok(())
    }

    /// 若 `backends` 未声明、可由 IR 的识别策略推断，则给出建议后端顺序。
    pub fn suggested_backends(&self) -> Vec<Backend> {
        let mut out: Vec<Backend> = Vec::new();
        for id in self
            .ir
            .mesh
            .replace
            .iter()
            .map(|m| &m.identify)
            .chain(self.ir.skeleton.map.iter().map(|s| &s.identify))
            .chain(self.ir.blendshape.iter().map(|b| &b.identify))
            .chain(self.ir.material.overrides.iter().map(|m| &m.identify))
            .chain(self.ir.texture.replace.iter().map(|t| &t.identify))
        {
            let b = id.preferred_backend();
            if !out.contains(&b) {
                out.push(b);
            }
        }
        out
    }

    /// 该 mod 引用的全部数据文件（含外部 IR 文件）。
    pub fn referenced_files(&self) -> Vec<String> {
        let mut files = self.ir.referenced_files();
        if let Some(entry) = &self.entry {
            files.push(entry.clone());
        }
        files.sort();
        files.dedup();
        files
    }

    /// 非致命告警。
    pub fn warnings(&self) -> Vec<String> {
        let mut out = self.ir.warnings();
        let suggested = self.suggested_backends();
        for b in &suggested {
            if !self.backends.contains(b) {
                out.push(format!(
                    "IR 中的识别策略暗示需要后端 `{b}`，但 backends 未声明它"
                ));
            }
        }
        if self.license.is_none() {
            out.push("未声明 license 字段，建议填写 SPDX 标识符（例如 AGPL-3.0-or-later）".into());
        }
        out
    }

    /// 面向用户的摘要行。
    pub fn summary_line(&self) -> String {
        let backends = self
            .backends
            .iter()
            .map(|b| b.as_str())
            .collect::<Vec<_>>()
            .join(", ");
        format!(
            "{} v{} [{}] 后端: {}",
            self.name, self.version, self.id, backends
        )
    }
}

/// 校验 mod id 的合法性。
pub fn validate_id(id: &str) -> Result<()> {
    if id.is_empty() || id.len() > ID_MAX_LEN {
        return Err(Error::manifest(format!(
            "mod id 长度必须在 1..={ID_MAX_LEN} 之间，实际为 {}",
            id.len()
        )));
    }
    if !id
        .chars()
        .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-' || c == '_')
    {
        return Err(Error::manifest(format!(
            "mod id `{id}` 含非法字符：只允许小写字母、数字、`-`、`_`"
        )));
    }
    if id.starts_with('-') || id.starts_with('_') {
        return Err(Error::manifest(format!(
            "mod id `{id}` 不能以 `-` 或 `_` 开头"
        )));
    }
    Ok(())
}

/// 校验一个 mod 包内的相对路径：必须非空、非绝对、且不含 `.` / `..` 分量。
pub fn validate_relative_path(field: &str, path: &str) -> Result<()> {
    let normalized = path.replace('\\', "/");
    if normalized.trim().is_empty() {
        return Err(Error::manifest(format!("{field}: 路径不能为空")));
    }
    if normalized.starts_with('/') {
        return Err(Error::manifest(format!(
            "{field}: 路径必须是相对 mod 包根目录的相对路径，不能是绝对路径: {path:?}"
        )));
    }
    if normalized
        .split('/')
        .any(|c| c == ".." || c == "." || c.is_empty())
    {
        return Err(Error::manifest(format!(
            "{field}: 路径不得包含 `.`、`..` 或空分量: {path:?}"
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const MINIMAL: &str = r#"
format_version = 1
id = "angel-body"
name = "Angel Body"
version = "1.0.0"
authors = ["someone"]
license = "AGPL-3.0-or-later"
backends = ["vulkan-layer"]

[target]
game = "arknights-endfield"
abi = "arm64-v8a"

[[mesh.replace]]
identify = { kind = "pipeline-hash", hash = "a1b2c3d4" }
vertex_count = 3
source = "mesh/body.pos.bin"

[[mesh.replace.submeshes]]
index_count = 3
index_format = "u16"
source = "mesh/body.idx.bin"
"#;

    fn minimal() -> ModManifest {
        ModManifest::from_toml_str(MINIMAL).expect("最小清单应能解析")
    }

    #[test]
    fn minimal_manifest_parses() {
        let m = minimal();
        assert_eq!(m.id, "angel-body");
        assert_eq!(m.backends, vec![Backend::VulkanLayer]);
        assert_eq!(m.ir.mesh.replace.len(), 1);
        assert_eq!(m.ir.mesh.replace[0].vertex_count, 3);
        assert_eq!(m.referenced_files().len(), 2);
        assert!(m.warnings().is_empty(), "不应有告警: {:?}", m.warnings());
    }

    #[test]
    fn manifest_roundtrips_through_toml() {
        let m = minimal();
        let text = m.to_toml_string().expect("应能序列化");
        let again = ModManifest::from_toml_str(&text).expect("应能往返");
        assert_eq!(again, m);
    }

    #[test]
    fn unknown_fields_are_ignored() {
        // 未知字段必须被忽略，保证旧 loader 能读新 mod。
        let src = format!("{MINIMAL}\nfuture_field = \"whatever\"\n");
        assert!(ModManifest::from_toml_str(&src).is_ok());
    }

    #[test]
    fn empty_backends_is_rejected() {
        let src = MINIMAL.replace("backends = [\"vulkan-layer\"]", "backends = []");
        let err = ModManifest::from_toml_str(&src).unwrap_err();
        assert!(err.to_string().contains("backends"), "{err}");
    }

    #[test]
    fn duplicate_backends_is_rejected() {
        let src = MINIMAL.replace(
            "backends = [\"vulkan-layer\"]",
            "backends = [\"vulkan-layer\", \"vulkan-layer\"]",
        );
        let err = ModManifest::from_toml_str(&src).unwrap_err();
        assert!(err.to_string().contains("重复"), "{err}");
    }

    #[test]
    fn wrong_format_version_is_rejected() {
        let src = MINIMAL.replace("format_version = 1", "format_version = 99");
        let err = ModManifest::from_toml_str(&src).unwrap_err();
        assert!(err.to_string().contains("格式版本"), "{err}");
    }

    #[test]
    fn empty_ir_is_rejected() {
        let src = r#"
format_version = 1
id = "empty"
name = "Empty"
version = "1.0.0"
backends = ["vulkan-layer"]

[target]
game = "arknights-endfield"
"#;
        let err = ModManifest::from_toml_str(src).unwrap_err();
        assert!(err.to_string().contains("IR 为空"), "{err}");
    }

    #[test]
    fn path_traversal_in_source_is_rejected() {
        let src = MINIMAL.replace("mesh/body.pos.bin", "../../etc/passwd");
        let err = ModManifest::from_toml_str(&src).unwrap_err();
        assert!(
            err.to_string().contains("相对路径") || err.to_string().contains(".."),
            "{err}"
        );
    }

    #[test]
    fn absolute_source_path_is_rejected() {
        let src = MINIMAL.replace("mesh/body.pos.bin", "/etc/passwd");
        let err = ModManifest::from_toml_str(&src).unwrap_err();
        assert!(err.to_string().contains("绝对路径"), "{err}");
    }

    #[test]
    fn invalid_id_characters_are_rejected() {
        assert!(validate_id("Angel-Body").is_err(), "大写应被拒绝");
        assert!(validate_id("angel body").is_err(), "空格应被拒绝");
        assert!(validate_id("-angel").is_err(), "前导 - 应被拒绝");
        assert!(validate_id("_angel").is_err(), "前导 _ 应被拒绝");
        assert!(validate_id("").is_err());
        assert!(validate_id(&"a".repeat(ID_MAX_LEN + 1)).is_err());
        assert!(validate_id("angel-body_2").is_ok());
    }

    #[test]
    fn attribute_offset_beyond_stride_is_rejected() {
        let src = MINIMAL.replace(
            "vertex_count = 3",
            "vertex_count = 3\nvertex_layout = { stride = 12, attributes = [\n  { semantic = \"POSITION\", format = \"R32G32B32_SFLOAT\", offset = 16 }\n] }",
        );
        let err = ModManifest::from_toml_str(&src).unwrap_err();
        assert!(err.to_string().contains("超出 stride"), "{err}");
    }

    #[test]
    fn unknown_vertex_format_produces_warning_not_error() {
        let src = MINIMAL.replace(
            "vertex_count = 3",
            "vertex_count = 3\nvertex_layout = { stride = 12, attributes = [\n  { semantic = \"POSITION\", format = \"SOME_FUTURE_FORMAT\", offset = 0 }\n] }",
        );
        let m = ModManifest::from_toml_str(&src).expect("未知格式不应导致失败");
        let warnings = m.warnings();
        assert!(
            warnings.iter().any(|w| w.contains("未知顶点格式")),
            "应产生未知格式告警: {warnings:?}"
        );
    }

    #[test]
    fn backend_serde_names_match_as_str() {
        // 锁定不变量：TOML 里的名字必须与 as_str()（也就是文档里写的名字）一致。
        // 曾经因为 rename_all = "kebab-case" 把 Il2CppHook 变成 `il2-cpp-hook` 而踩坑。
        #[derive(Serialize, Deserialize)]
        struct Wrapper {
            backends: Vec<Backend>,
        }

        let wrapper = Wrapper {
            backends: Backend::ALL.to_vec(),
        };
        let text = toml::to_string(&wrapper).expect("应能序列化");
        for backend in Backend::ALL {
            assert!(
                text.contains(backend.as_str()),
                "`{}` 未出现在序列化结果中: {text}",
                backend.as_str()
            );
        }
        let back: Wrapper = toml::from_str(&text).expect("应能反序列化");
        assert_eq!(back.backends, Backend::ALL.to_vec());
    }

    #[test]
    fn suggested_backends_derived_from_identify() {
        let src = MINIMAL.replace(
            "identify = { kind = \"pipeline-hash\", hash = \"a1b2c3d4\" }",
            "identify = { kind = \"unity-path\", path = \"Assets/Angel\" }",
        );
        let m = ModManifest::from_toml_str(&src).expect("应能解析");
        assert_eq!(m.suggested_backends(), vec![Backend::Il2CppHook]);
        // backends 只声明了 vulkan-layer，与 IR 暗示不一致 → 应有告警。
        assert!(m.warnings().iter().any(|w| w.contains("il2cpp-hook")));
    }
}
