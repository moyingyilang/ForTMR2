//! 与执行后端无关的中间表示（IR）。
//!
//! IR 是本项目的核心设计：EFMI 的 mod 是「ini + HLSL + Buffer/Texture 目录」的松散
//! 组合，只有 3DMigoto 自己能解释。EFML4A 把 mod 要做的事情描述成**声明式的、
//! 可静态校验的**结构，再由各后端（Vulkan 层 / IL2CPP hook / AssetBundle）分别编译
//! 成具体动作。这样一个 mod 包可以跨后端复用。
//!
//! TOML 中的组织方式（与 `docs/10-mod-format.md` 一致）：
//!
//! ```toml
//! [[mesh.replace]]
//! [[skeleton.map]]
//! [[blendshape]]
//! [[material.override]]
//! [[texture.replace]]
//! ```

use serde::{Deserialize, Serialize};

use crate::identify::Identify;
use crate::{Error, Result};

/// 已知的顶点属性格式名（不区分大小写）。
///
/// 该列表**仅用于产生告警**，不会导致校验失败：不同 GPU 厂商与引擎版本可能引入
/// 新的格式名，硬性拒绝会误伤合法 mod。
pub const KNOWN_VERTEX_FORMATS: &[&str] = &[
    "R32G32B32A32_SFLOAT",
    "R32G32B32_SFLOAT",
    "R32G32_SFLOAT",
    "R32_SFLOAT",
    "R16G16B16A16_SFLOAT",
    "R16G16B16A16_UNORM",
    "R16G16B16A16_SINT",
    "R16G16B16A16_SNORM",
    "R16G16_SFLOAT",
    "R16G16_UNORM",
    "R16G16_SINT",
    "R16G16_SNORM",
    "R8G8B8A8_UNORM",
    "R8G8B8A8_SNORM",
    "R8G8B8A8_SINT",
    "R8G8B8A8_UINT",
    "R8G8B8A8_UNORM_SRGB",
    "R32_SINT",
    "R32_UINT",
    "R16_SFLOAT",
    "R16_UNORM",
    "R8_UNORM",
    "R10G10B10A2_UNORM",
    "R11G11B10_SFLOAT",
];

/// 顶点属性格式。用字符串包装而非封闭枚举，避免后端支持新格式时被迫破坏格式版本。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct VertexFormat(pub String);

impl VertexFormat {
    /// 构造一个顶点格式。
    pub fn new(name: impl Into<String>) -> Self {
        Self(name.into())
    }

    /// 格式名原文。
    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// 是否属于已知格式列表（大小写不敏感）。未知格式只会产生告警。
    pub fn is_known(&self) -> bool {
        let upper = self.0.to_ascii_uppercase();
        KNOWN_VERTEX_FORMATS.contains(&upper.as_str())
    }
}

impl std::fmt::Display for VertexFormat {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

/// 单个顶点属性。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct VertexAttribute {
    /// 语义名，例如 `POSITION`、`NORMAL`、`TEXCOORD0`、`BLENDWEIGHTS`、`BLENDINDICES`。
    pub semantic: String,
    /// 数据格式。
    pub format: VertexFormat,
    /// 相对于顶点起始位置的字节偏移。
    pub offset: u32,
    /// 可选的显式 shader location（Vulkan 后端使用）。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub location: Option<u32>,
}

/// 顶点布局：步长与属性表。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct VertexLayout {
    /// 顶点步长（字节），对应 Vulkan 的 `VkVertexInputBindingDescription::stride`。
    pub stride: u32,
    /// 属性列表。
    pub attributes: Vec<VertexAttribute>,
}

impl VertexLayout {
    /// 校验布局的内部一致性：必须非空，且每个属性必须落在 stride 之内。
    pub fn validate(&self, ctx: &str) -> Result<()> {
        if self.stride == 0 {
            return Err(Error::manifest(format!(
                "{ctx}: vertex_layout.stride 不能为 0"
            )));
        }
        if self.attributes.is_empty() {
            return Err(Error::manifest(format!(
                "{ctx}: vertex_layout.attributes 不能为空"
            )));
        }
        for attr in &self.attributes {
            if attr.offset >= self.stride {
                return Err(Error::manifest(format!(
                    "{ctx}: 属性 `{}` 的 offset={} 超出 stride={}",
                    attr.semantic, attr.offset, self.stride
                )));
            }
        }
        Ok(())
    }

    /// 收集未知格式的告警（不构成错误）。
    pub fn format_warnings(&self, ctx: &str) -> Vec<String> {
        self.attributes
            .iter()
            .filter(|a| !a.format.is_known())
            .map(|a| {
                format!(
                    "{ctx}: 属性 `{}` 使用了未知顶点格式 `{}`，将原样透传给后端",
                    a.semantic, a.format
                )
            })
            .collect()
    }
}

/// 索引缓冲的索引宽度。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum IndexFormat {
    /// 16 位无符号索引。
    U16,
    /// 32 位无符号索引。
    U32,
}

impl IndexFormat {
    /// 单个索引的字节宽度。
    pub fn byte_width(self) -> u32 {
        match self {
            Self::U16 => 2,
            Self::U32 => 4,
        }
    }
}

/// 一个子网格（submesh）。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SubMesh {
    /// 索引数量。
    pub index_count: u32,
    /// 索引宽度。
    pub index_format: IndexFormat,
    /// 索引数据文件，相对 mod 包根目录。
    pub source: String,
    /// 可选拓扑，默认 `triangle-list`。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub topology: Option<String>,
}

/// 网格替换条目，对应 `[[mesh.replace]]`。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MeshReplace {
    /// 识别策略。
    pub identify: Identify,
    /// 可选的目标顶点布局。省略表示沿用游戏原有的布局（仅替换数据）。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub vertex_layout: Option<VertexLayout>,
    /// 顶点数量。
    pub vertex_count: u32,
    /// 子网格列表。
    #[serde(default)]
    pub submeshes: Vec<SubMesh>,
    /// 顶点数据文件，相对 mod 包根目录。
    pub source: String,
    /// 是否支持热重载。不支持则需要重启游戏。
    #[serde(default)]
    pub hot_reload: bool,
}

impl MeshReplace {
    /// 校验单个网格替换条目。
    pub fn validate(&self, ctx: &str) -> Result<()> {
        crate::manifest::validate_relative_path(&format!("{ctx}.source"), &self.source)?;
        if self.vertex_count == 0 {
            return Err(Error::manifest(format!("{ctx}: vertex_count 不能为 0")));
        }
        if let Some(layout) = &self.vertex_layout {
            layout.validate(ctx)?;
        }
        for (i, sub) in self.submeshes.iter().enumerate() {
            crate::manifest::validate_relative_path(
                &format!("{ctx}.submeshes[{i}].source"),
                &sub.source,
            )?;
            if sub.index_count == 0 {
                return Err(Error::manifest(format!(
                    "{ctx}.submeshes[{i}]: index_count 不能为 0"
                )));
            }
        }
        Ok(())
    }

    /// 该条目引用的数据文件。
    pub fn referenced_files(&self) -> Vec<String> {
        let mut files = vec![self.source.clone()];
        files.extend(self.submeshes.iter().map(|s| s.source.clone()));
        files
    }

    /// 顶点数据期望的字节数（仅在声明了 `vertex_layout` 时可计算）。
    pub fn expected_vertex_bytes(&self) -> Option<u64> {
        self.vertex_layout
            .as_ref()
            .map(|l| u64::from(l.stride) * u64::from(self.vertex_count))
    }
}

/// 骨骼重映射表的一项。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BoneMap {
    /// 目标骨骼槽位。
    pub slot: u32,
    /// 源骨骼名（用于文档与调试）。
    pub name: String,
}

/// 骨骼映射条目，对应 `[[skeleton.map]]`。
///
/// 与 EFMI 的 `MergedSkeleton` 对应：把多个角色/部件的骨骼合并到统一骨骼列表，
/// 使 mod 作者可以只针对统一列表工作。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SkeletonMapping {
    /// 识别策略。
    pub identify: Identify,
    /// 合并后骨骼数量。
    pub bone_count: u32,
    /// 槽位重映射表。
    #[serde(default)]
    pub map: Vec<BoneMap>,
    /// 可选的 bindpose 数据文件，相对 mod 包根目录。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bindposes: Option<String>,
}

impl SkeletonMapping {
    /// 校验骨骼映射条目。
    pub fn validate(&self, ctx: &str) -> Result<()> {
        if self.bone_count == 0 {
            return Err(Error::manifest(format!("{ctx}: bone_count 不能为 0")));
        }
        if let Some(p) = &self.bindposes {
            crate::manifest::validate_relative_path(&format!("{ctx}.bindposes"), p)?;
        }
        let mut slots = std::collections::BTreeSet::new();
        for (i, bone) in self.map.iter().enumerate() {
            if bone.slot >= self.bone_count {
                return Err(Error::manifest(format!(
                    "{ctx}.map[{i}]: slot={} 超出 bone_count={}",
                    bone.slot, self.bone_count
                )));
            }
            if !slots.insert(bone.slot) {
                return Err(Error::manifest(format!(
                    "{ctx}.map[{i}]: slot={} 重复声明",
                    bone.slot
                )));
            }
        }
        Ok(())
    }

    /// 该条目引用的数据文件。
    pub fn referenced_files(&self) -> Vec<String> {
        self.bindposes.iter().cloned().collect()
    }
}

/// 形态键（表情）条目，对应 `[[blendshape]]`。
///
/// 与 EFMI 的 `ShapeKeys` 对应。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BlendShape {
    /// 识别策略。
    pub identify: Identify,
    /// 形态键名称。
    pub name: String,
    /// 形变帧数量。
    pub frame_count: u32,
    /// 数据文件，相对 mod 包根目录。
    pub source: String,
    /// 可选的默认权重（0.0..=1.0）。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub default_weight: Option<f32>,
}

impl BlendShape {
    /// 校验形态键条目。
    pub fn validate(&self, ctx: &str) -> Result<()> {
        crate::manifest::validate_relative_path(&format!("{ctx}.source"), &self.source)?;
        if self.name.trim().is_empty() {
            return Err(Error::manifest(format!("{ctx}: name 不能为空")));
        }
        if self.frame_count == 0 {
            return Err(Error::manifest(format!("{ctx}: frame_count 不能为 0")));
        }
        if let Some(w) = self.default_weight {
            if !(0.0..=1.0).contains(&w) {
                return Err(Error::manifest(format!(
                    "{ctx}: default_weight={w} 必须落在 0.0..=1.0"
                )));
            }
        }
        Ok(())
    }

    /// 该条目引用的数据文件。
    pub fn referenced_files(&self) -> Vec<String> {
        vec![self.source.clone()]
    }
}

/// 贴图绑定。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TextureBinding {
    /// 目标贴图槽，例如 `_MainTex` 或 `diffuse`。
    pub slot: String,
    /// 贴图文件，相对 mod 包根目录。
    pub source: String,
}

/// 材质覆盖条目，对应 `[[material.override]]`。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MaterialOverride {
    /// 识别策略。
    pub identify: Identify,
    /// 可选的目标 shader 名。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub shader: Option<String>,
    /// 贴图绑定列表。
    #[serde(default)]
    pub textures: Vec<TextureBinding>,
}

impl MaterialOverride {
    /// 校验材质覆盖条目。
    pub fn validate(&self, ctx: &str) -> Result<()> {
        for (i, t) in self.textures.iter().enumerate() {
            crate::manifest::validate_relative_path(
                &format!("{ctx}.textures[{i}].source"),
                &t.source,
            )?;
            if t.slot.trim().is_empty() {
                return Err(Error::manifest(format!(
                    "{ctx}.textures[{i}]: slot 不能为空"
                )));
            }
        }
        Ok(())
    }

    /// 该条目引用的数据文件。
    pub fn referenced_files(&self) -> Vec<String> {
        self.textures.iter().map(|t| t.source.clone()).collect()
    }
}

/// 贴图替换条目，对应 `[[texture.replace]]`。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TextureReplace {
    /// 识别策略。
    pub identify: Identify,
    /// 贴图文件，相对 mod 包根目录。
    pub source: String,
    /// 可选的像素格式提示，例如 `RGBA8_UNORM`。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub format: Option<String>,
}

impl TextureReplace {
    /// 校验贴图替换条目。
    pub fn validate(&self, ctx: &str) -> Result<()> {
        crate::manifest::validate_relative_path(&format!("{ctx}.source"), &self.source)
    }

    /// 该条目引用的数据文件。
    pub fn referenced_files(&self) -> Vec<String> {
        vec![self.source.clone()]
    }
}

/// 网格段落，承载 `[[mesh.replace]]`。
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct MeshSection {
    /// 网格替换条目列表。
    #[serde(default)]
    pub replace: Vec<MeshReplace>,
}

impl MeshSection {
    /// 段落是否为空（用于序列化时省略）。
    pub fn is_empty(&self) -> bool {
        self.replace.is_empty()
    }
}

/// 骨骼段落，承载 `[[skeleton.map]]`。
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct SkeletonSection {
    /// 骨骼映射条目列表。
    #[serde(default)]
    pub map: Vec<SkeletonMapping>,
}

impl SkeletonSection {
    /// 段落是否为空。
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

/// 材质段落，承载 `[[material.override]]`。
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct MaterialSection {
    /// 材质覆盖条目列表。
    #[serde(default, rename = "override")]
    pub overrides: Vec<MaterialOverride>,
}

impl MaterialSection {
    /// 段落是否为空。
    pub fn is_empty(&self) -> bool {
        self.overrides.is_empty()
    }
}

/// 贴图段落，承载 `[[texture.replace]]`。
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct TextureSection {
    /// 贴图替换条目列表。
    #[serde(default)]
    pub replace: Vec<TextureReplace>,
}

impl TextureSection {
    /// 段落是否为空。
    pub fn is_empty(&self) -> bool {
        self.replace.is_empty()
    }
}

/// EFML4A 的完整 IR。
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct ModIr {
    /// 网格替换。
    #[serde(default, skip_serializing_if = "MeshSection::is_empty")]
    pub mesh: MeshSection,
    /// 骨骼映射。
    #[serde(default, skip_serializing_if = "SkeletonSection::is_empty")]
    pub skeleton: SkeletonSection,
    /// 形态键。
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub blendshape: Vec<BlendShape>,
    /// 材质覆盖。
    #[serde(default, skip_serializing_if = "MaterialSection::is_empty")]
    pub material: MaterialSection,
    /// 贴图替换。
    #[serde(default, skip_serializing_if = "TextureSection::is_empty")]
    pub texture: TextureSection,
}

impl ModIr {
    /// IR 是否完全没有内容。
    pub fn is_empty(&self) -> bool {
        self.mesh.is_empty()
            && self.skeleton.is_empty()
            && self.blendshape.is_empty()
            && self.material.is_empty()
            && self.texture.is_empty()
    }

    /// 校验整个 IR。
    pub fn validate(&self) -> Result<()> {
        if self.is_empty() {
            return Err(Error::manifest(
                "IR 为空：mod 至少需要声明一项替换（mesh / skeleton / blendshape / material / texture）",
            ));
        }
        for (i, m) in self.mesh.replace.iter().enumerate() {
            m.validate(&format!("mesh.replace[{i}]"))?;
        }
        for (i, s) in self.skeleton.map.iter().enumerate() {
            s.validate(&format!("skeleton.map[{i}]"))?;
        }
        for (i, b) in self.blendshape.iter().enumerate() {
            b.validate(&format!("blendshape[{i}]"))?;
        }
        for (i, m) in self.material.overrides.iter().enumerate() {
            m.validate(&format!("material.override[{i}]"))?;
        }
        for (i, t) in self.texture.replace.iter().enumerate() {
            t.validate(&format!("texture.replace[{i}]"))?;
        }
        Ok(())
    }

    /// IR 中所有被引用的数据文件（去重后按字典序）。
    pub fn referenced_files(&self) -> Vec<String> {
        let mut files: Vec<String> = Vec::new();
        for m in &self.mesh.replace {
            files.extend(m.referenced_files());
        }
        for s in &self.skeleton.map {
            files.extend(s.referenced_files());
        }
        for b in &self.blendshape {
            files.extend(b.referenced_files());
        }
        for m in &self.material.overrides {
            files.extend(m.referenced_files());
        }
        for t in &self.texture.replace {
            files.extend(t.referenced_files());
        }
        files.sort();
        files.dedup();
        files
    }

    /// 非致命告警（未知顶点格式等）。
    pub fn warnings(&self) -> Vec<String> {
        let mut out = Vec::new();
        for (i, m) in self.mesh.replace.iter().enumerate() {
            if let Some(layout) = &m.vertex_layout {
                out.extend(layout.format_warnings(&format!("mesh.replace[{i}]")));
            }
            if m.vertex_layout.is_some() && m.submeshes.is_empty() {
                out.push(format!(
                    "mesh.replace[{i}]: 声明了 vertex_layout 但没有 submeshes，索引将沿用游戏原有数据"
                ));
            }
        }
        out
    }

    /// 全部条目是否都支持热重载。
    pub fn fully_hot_reloadable(&self) -> bool {
        self.mesh.replace.iter().all(|m| m.hot_reload) && self.skeleton.map.iter().all(|_| true)
    }
}
