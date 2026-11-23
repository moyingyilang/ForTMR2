//! EFML4A 统一错误类型。
//!
//! 设计原则：错误信息面向 mod 作者，必须包含「哪个字段 / 哪个文件 / 期望什么」。

/// EFML4A 统一错误类型。
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// 文件或目录 I/O 失败。
    #[error("I/O 失败: {0}")]
    Io(#[from] std::io::Error),

    /// `mod.toml` 解析失败。
    #[error("mod.toml 解析失败: {0}")]
    TomlDe(#[from] toml::de::Error),

    /// 序列化失败。
    #[error("序列化失败: {0}")]
    TomlSer(#[from] toml::ser::Error),

    /// JSON 处理失败。
    #[error("JSON 处理失败: {0}")]
    Json(#[from] serde_json::Error),

    /// 版本号不符合 semver。
    #[error("版本号非法: {0}")]
    Semver(#[from] semver::Error),

    /// manifest 字段不合法。
    #[error("manifest 校验失败: {0}")]
    Manifest(String),

    /// mod 包整体校验失败（缺文件、校验和不匹配等）。
    #[error("mod 包校验失败: {0}")]
    Package(String),
}

/// EFML4A 统一结果类型。
pub type Result<T, E = Error> = std::result::Result<T, E>;

impl Error {
    /// 构造 [`Error::Manifest`]。
    pub fn manifest(msg: impl Into<String>) -> Self {
        Self::Manifest(msg.into())
    }

    /// 构造 [`Error::Package`]。
    pub fn package(msg: impl Into<String>) -> Self {
        Self::Package(msg.into())
    }
}
