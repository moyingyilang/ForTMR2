//! draw call 与资源的识别策略。
//!
//! EFMI 在 PC 上依赖 3DMigoto 的「哈希狩猎」来定位目标 draw call：把顶点/索引缓冲
//! 的内容哈希与 ini 中登记的签名比对。Android 上不存在 DXGI，3DMigoto 的整个注入
//! 前提消失，但「识别出要替换的那一次绘制」这件事依然存在，只是可用特征不同。
//!
//! 本模块用统一枚举表达各后端可用的识别手段，使同一份 mod 描述可以在
//! Vulkan 层后端与 IL2CPP 后端之间复用。详见 `docs/04-route-vulkan-layer.md`
//! 与 `docs/05-route-il2cpp-hook.md`。

use serde::{Deserialize, Serialize};

use crate::manifest::Backend;

/// 识别策略。序列化时用 `kind` 字段区分变体。
///
/// TOML 写法示例：
///
/// ```toml
/// [[mesh.replace]]
/// identify = { kind = "pipeline-hash", hash = "a1b2c3d4" }
/// ```
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum Identify {
    /// 流水线指纹：SPIR-V 模块哈希与关键流水线状态的摘要。
    ///
    /// 这是 Vulkan 层后端的主要手段，语义上最接近 PC 上 3DMigoto 对
    /// shader/pipeline 的哈希识别。
    PipelineHash {
        /// 十六进制流水线指纹。
        hash: String,
        /// 可选的期望顶点数，用于降低误命中概率。
        #[serde(default, skip_serializing_if = "Option::is_none")]
        vertex_count: Option<u32>,
    },
    /// 顶点签名：顶点布局（stride 与属性组合）的特征。
    ///
    /// 比流水线指纹更宽松，适合跨版本复用，但误命中率更高，
    /// 因此规范中允许把多种识别策略按优先级串成回退链。
    VertexSignature {
        /// 顶点步长（字节）。
        stride: u32,
        /// 属性语义列表，例如 `["POSITION", "NORMAL", "TEXCOORD0"]`。
        attributes: Vec<String>,
    },
    /// Unity 对象路径（IL2CPP 后端）。
    ///
    /// 例如 `"Assets/Characters/Angel/Model/Angel_Body"`。
    UnityPath {
        /// Unity 资源或层级路径。
        path: String,
    },
    /// 资源包路径（AssetBundle 后端）。
    AssetPath {
        /// AssetBundle 内或包内的资源路径。
        path: String,
    },
}

impl Identify {
    /// 返回变体名（kebab-case），用于日志与错误信息。
    pub fn kind(&self) -> &'static str {
        match self {
            Self::PipelineHash { .. } => "pipeline-hash",
            Self::VertexSignature { .. } => "vertex-signature",
            Self::UnityPath { .. } => "unity-path",
            Self::AssetPath { .. } => "asset-path",
        }
    }

    /// 该识别策略最自然对应的执行后端。
    ///
    /// 这只是一个「提示」，用于在 mod 未显式声明 `backends` 时推断默认值；
    /// 不构成硬性约束（例如 IL2CPP 后端同样可以通过顶点签名做二次确认）。
    pub fn preferred_backend(&self) -> Backend {
        match self {
            Self::PipelineHash { .. } | Self::VertexSignature { .. } => Backend::VulkanLayer,
            Self::UnityPath { .. } => Backend::Il2CppHook,
            Self::AssetPath { .. } => Backend::AssetBundle,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pipeline_hash_roundtrips_through_toml() {
        #[derive(Debug, Serialize, Deserialize)]
        struct Wrapper {
            identify: Identify,
        }
        let src = r#"
            [identify]
            kind = "pipeline-hash"
            hash = "a1b2c3d4"
            vertex_count = 4096
        "#;
        let parsed: Wrapper = toml::from_str(src).expect("应能解析");
        assert_eq!(
            parsed.identify,
            Identify::PipelineHash {
                hash: "a1b2c3d4".to_string(),
                vertex_count: Some(4096),
            }
        );
        let text = toml::to_string(&parsed).expect("应能序列化");
        let again: Wrapper = toml::from_str(&text).expect("应能往返");
        assert_eq!(again.identify, parsed.identify);
    }

    #[test]
    fn vertex_signature_omits_absent_optional_field() {
        let id = Identify::VertexSignature {
            stride: 40,
            attributes: vec!["POSITION".into(), "NORMAL".into()],
        };
        assert_eq!(id.kind(), "vertex-signature");
        assert_eq!(id.preferred_backend(), Backend::VulkanLayer);
    }

    #[test]
    fn unity_path_prefers_il2cpp_backend() {
        let id = Identify::UnityPath {
            path: "Assets/Characters/Angel/Model".into(),
        };
        assert_eq!(id.preferred_backend(), Backend::Il2CppHook);
    }

    #[test]
    fn asset_path_prefers_assetbundle_backend() {
        let id = Identify::AssetPath {
            path: "assets/characters/angel".into(),
        };
        assert_eq!(id.preferred_backend(), Backend::AssetBundle);
    }
}
