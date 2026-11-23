//! EFML4A 工具链入口（TUI + 命令行）。
//!
//! 本项目偏好用 **Rust 或 Kotlin** 编写工具；凡是不值得做原生窗口的逻辑，一律做成
//! TUI（`efml tui`）而不是一堆 shell 脚本，这样逻辑可测试、可跨设备复用。
//! 需要图形界面时使用 [`efml_gui`](../efml_gui/index.html)（混合架构：Rust 核心 + 本地 Web UI）。

mod commands;
mod output;
mod tui;

use std::path::PathBuf;

use clap::{Parser, Subcommand};

/// Arknights: Endfield Mod Loader for Android —— 研究用工具链。
#[derive(Debug, Parser)]
#[command(name = "efml", version, about, long_about = None)]
struct Cli {
    /// 以 JSON 输出，便于脚本与 GUI 消费。
    #[arg(long, global = true)]
    json: bool,

    /// 子命令。
    #[command(subcommand)]
    command: Command,
}

/// 可用的子命令。
#[derive(Debug, Subcommand)]
enum Command {
    /// 校验一个 mod 包目录，或一个包含多个 mod 的 Mods 目录。
    Validate {
        /// mod 包目录或 Mods 目录。
        path: PathBuf,
    },
    /// 打印某个 mod 包的清单信息与被引用文件。
    Info {
        /// mod 包目录。
        path: PathBuf,
    },
    /// 列出 Mods 目录下的全部 mod 包。
    List {
        /// Mods 目录。
        dir: PathBuf,
    },
    /// 生成或校验 checksums.txt。
    Checksum {
        /// mod 包目录。
        path: PathBuf,
        /// 计算并写入（覆盖）checksums.txt。
        #[arg(long)]
        write: bool,
    },
    /// 启动交互式 TUI 浏览与校验 Mods 目录。
    Tui {
        /// Mods 目录，默认为 ./Mods。
        dir: Option<PathBuf>,
    },
    /// 打印四条技术路线的评估矩阵（研究速查）。
    Routes,
}

fn main() -> anyhow::Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Command::Validate { path } => commands::validate(&path, cli.json),
        Command::Info { path } => commands::info(&path, cli.json),
        Command::List { dir } => commands::list(&dir, cli.json),
        Command::Checksum { path, write } => commands::checksum(&path, write, cli.json),
        Command::Tui { dir } => tui::run(dir.unwrap_or_else(|| PathBuf::from("Mods"))),
        Command::Routes => commands::routes(cli.json),
    }
}
