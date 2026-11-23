//! mod 包的 sha256 校验和工具。
//!
//! mod 包会携带 `checksums.txt`，内容形如 `<sha256>  <相对路径>`（两空格分隔，
//! 与 `sha256sum` 输出兼容，便于作者用系统工具生成）。

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufRead, BufReader, Read};
use std::path::Path;

use sha2::{Digest, Sha256};

use crate::{Error, Result};

/// 单个 mod 包的校验和清单：相对路径 → 小写十六进制 sha256。
pub type ChecksumMap = BTreeMap<String, String>;

/// 计算字节切片的 sha256，返回小写十六进制字符串。
pub fn sha256_bytes(data: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(data);
    to_hex(&hasher.finalize())
}

/// 以流式方式计算文件 sha256，避免把大 mesh 文件整体读进内存。
pub fn sha256_file(path: &Path) -> Result<String> {
    let mut reader = BufReader::new(File::open(path)?);
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(to_hex(&hasher.finalize()))
}

/// 解析 `checksums.txt` 的内容。
///
/// 容忍空行与注释行（以 `#` 开头）。校验和大小写不敏感，统一归一化为小写。
/// 路径中的 `..` 与绝对路径会被拒绝，避免 mod 包越出自身根目录。
pub fn parse_checksums(content: &str) -> Result<ChecksumMap> {
    let mut map = ChecksumMap::new();
    for (lineno, raw_line) in content.lines().enumerate() {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut parts = line.split_whitespace();
        let (Some(digest), Some(path)) = (parts.next(), parts.next()) else {
            return Err(Error::package(format!(
                "checksums.txt 第 {} 行格式非法，应为 `<sha256>  <相对路径>`: {raw_line:?}",
                lineno + 1
            )));
        };
        if digest.len() != 64 || !digest.chars().all(|c| c.is_ascii_hexdigit()) {
            return Err(Error::package(format!(
                "checksums.txt 第 {} 行的 sha256 非法: {digest:?}",
                lineno + 1
            )));
        }
        let normalized_path = path.replace('\\', "/");
        if normalized_path.starts_with('/') || normalized_path.split('/').any(|c| c == "..") {
            return Err(Error::package(format!(
                "checksums.txt 第 {} 行的路径越出 mod 包根目录: {path:?}",
                lineno + 1
            )));
        }
        map.insert(normalized_path, digest.to_ascii_lowercase());
    }
    Ok(map)
}

/// 读取并解析 mod 包根目录下的 `checksums.txt`；文件不存在时返回 `None`。
pub fn read_checksums(root: &Path) -> Result<Option<ChecksumMap>> {
    let path = root.join("checksums.txt");
    if !path.is_file() {
        return Ok(None);
    }
    let content = std::fs::read_to_string(&path)?;
    Ok(Some(parse_checksums(&content)?))
}

/// 校验一个文件是否与期望的 sha256 一致。
pub fn verify_file(path: &Path, expected: &str) -> Result<()> {
    let actual = sha256_file(path)?;
    if actual.eq_ignore_ascii_case(expected) {
        Ok(())
    } else {
        Err(Error::package(format!(
            "{} 校验和不匹配：期望 {expected}，实际 {actual}",
            path.display()
        )))
    }
}

/// 从任意 `BufRead` 逐行解析校验和，供流式场景使用。
pub fn parse_checksums_reader<R: BufRead>(reader: R) -> Result<ChecksumMap> {
    let mut buf = String::new();
    for line in reader.lines() {
        buf.push_str(&line?);
        buf.push('\n');
    }
    parse_checksums(&buf)
}

/// 把摘要字节格式化为小写十六进制字符串。
fn to_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_matches_known_vectors() {
        assert_eq!(
            sha256_bytes(b""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            sha256_bytes(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn parse_checksums_accepts_sha256sum_format() {
        let content = "# 由 efml-cli pack 生成\n\
             e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  mesh/body.pos.bin\n\
             \n\
             BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  mesh/body.idx.bin\n";
        let map = parse_checksums(content).expect("应能解析");
        assert_eq!(map.len(), 2);
        assert_eq!(
            map.get("mesh/body.pos.bin").map(String::as_str),
            Some("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        );
        assert_eq!(
            map.get("mesh/body.idx.bin").map(String::as_str),
            Some("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        );
    }

    #[test]
    fn parse_checksums_rejects_bad_digest() {
        assert!(parse_checksums("deadbeef  a.bin\n").is_err());
        assert!(parse_checksums("nothex  a.bin\n").is_err());
    }

    #[test]
    fn parse_checksums_rejects_path_traversal() {
        let digest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        assert!(parse_checksums(&format!("{digest}  ../../etc/passwd\n")).is_err());
        assert!(parse_checksums(&format!("{digest}  /etc/passwd\n")).is_err());
    }

    #[test]
    fn parse_checksums_rejects_short_line() {
        let digest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        assert!(parse_checksums(&format!("{digest}\n")).is_err());
    }
}
