//! 交互式 TUI：浏览 Mods 目录、查看清单、运行校验。
//!
//! 之所以把这块做成 TUI 而不是 shell 脚本：项目约定「工具用 Rust/Kotlin 写，若确实
//! 只能写成 shell 则必须做成 TUI」。TUI 有真实的数据模型与错误处理，能在设备上直接
//! 跑，也能被复用进 GUI 后端。

use std::path::PathBuf;
use std::time::Duration;

use anyhow::{Context, Result};
use efml_core::ModPackage;
use ratatui::backend::CrosstermBackend;
use ratatui::crossterm::event::{self, Event, KeyCode, KeyEventKind};
use ratatui::crossterm::execute;
use ratatui::crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use ratatui::layout::{Constraint, Layout};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, List, ListItem, ListState, Paragraph, Wrap};
use ratatui::{Frame, Terminal};

/// 终端后端类型别名。
type Term = Terminal<CrosstermBackend<std::io::Stdout>>;

/// TUI 的应用状态。
struct App {
    /// 被浏览的 Mods 目录。
    mods_dir: PathBuf,
    /// 成功加载的 mod 包。
    packages: Vec<ModPackage>,
    /// 加载失败的目录与原因。
    failed: Vec<(PathBuf, String)>,
    /// 列表选择状态。
    state: ListState,
    /// 右侧上栏：清单详情。
    detail: String,
    /// 右侧下栏：校验输出。
    validation: String,
    /// 底部状态栏文本。
    status: String,
    /// 是否退出。
    should_quit: bool,
}

impl App {
    /// 创建应用并立即扫描一次目录。
    fn new(mods_dir: PathBuf) -> Self {
        let mut app = Self {
            mods_dir,
            packages: Vec::new(),
            failed: Vec::new(),
            state: ListState::default(),
            detail: String::new(),
            validation: String::new(),
            status: String::new(),
            should_quit: false,
        };
        app.reload();
        app
    }

    /// 重新扫描 Mods 目录。
    fn reload(&mut self) {
        match ModPackage::load_all(&self.mods_dir) {
            Ok((packages, failed)) => {
                self.status = format!("已加载 {} 个 mod，{} 个失败", packages.len(), failed.len());
                self.packages = packages;
                self.failed = failed;
            }
            Err(e) => {
                self.status = format!("扫描失败: {e}");
                self.packages.clear();
                self.failed.clear();
            }
        }
        if self.packages.is_empty() {
            self.state.select(None);
        } else {
            self.state.select(Some(0));
        }
        self.refresh_detail();
    }

    /// 当前选中的包。
    fn selected_package(&self) -> Option<&ModPackage> {
        self.state.selected().and_then(|i| self.packages.get(i))
    }

    /// 重建详情栏。
    fn refresh_detail(&mut self) {
        let detail = match self.selected_package() {
            Some(pkg) => describe(pkg),
            None => "（没有可显示的 mod）\n\n把 mod 目录放进 Mods/ 后按 r 重新扫描。".to_string(),
        };
        self.detail = detail;
        self.validation = String::from("按 Enter 校验当前 mod。");
    }

    /// 移动选择（环形）。
    fn move_selection(&mut self, delta: i64) {
        if self.packages.is_empty() {
            return;
        }
        let len = self.packages.len() as i64;
        let current = self.state.selected().unwrap_or(0) as i64;
        let next = (current + delta).rem_euclid(len);
        self.state.select(Some(next as usize));
        self.refresh_detail();
    }

    /// 对当前选中的包执行完整校验，结果写入校验栏。
    fn validate_selected(&mut self) {
        let Some(pkg) = self.selected_package() else {
            self.validation = String::from("没有选中的 mod。");
            return;
        };
        let mut out = String::new();

        match pkg.ensure_loader_compatible() {
            Ok(()) => out.push_str("loader 版本: 兼容\n"),
            Err(e) => out.push_str(&format!("loader 版本: 不兼容 —— {e}\n")),
        }

        match pkg.validate() {
            Ok(report) => {
                out.push_str(&format!("已检查文件: {} 个\n", report.checked_files));
                if report.is_clean() {
                    out.push_str("结构校验: 通过，无告警\n");
                } else {
                    out.push_str(&format!(
                        "结构校验: 通过，{} 条告警\n",
                        report.warnings.len()
                    ));
                    for w in &report.warnings {
                        out.push_str(&format!("  ! {w}\n"));
                    }
                }
            }
            Err(e) => out.push_str(&format!("结构校验: 失败 —— {e}\n")),
        }

        if pkg.checksums().is_some() {
            match pkg.verify_checksums() {
                Ok(n) => out.push_str(&format!("sha256 校验: {n} 个文件全部匹配\n")),
                Err(e) => out.push_str(&format!("sha256 校验: 失败 —— {e}\n")),
            }
        } else {
            out.push_str("sha256 校验: 包内没有 checksums.txt\n");
        }

        self.validation = out;
    }

    /// 处理按键。
    fn handle_key(&mut self, code: KeyCode) {
        match code {
            KeyCode::Char('q') | KeyCode::Esc => self.should_quit = true,
            KeyCode::Char('j') | KeyCode::Down => self.move_selection(1),
            KeyCode::Char('k') | KeyCode::Up => self.move_selection(-1),
            KeyCode::Char('r') => self.reload(),
            KeyCode::Enter => self.validate_selected(),
            _ => {}
        }
    }
}

/// 生成某个 mod 包的详情文本。
fn describe(pkg: &ModPackage) -> String {
    let m = pkg.manifest();
    let mut out = String::new();
    out.push_str(&format!("{}\n", m.summary_line()));
    out.push_str(&format!("目录      : {}\n", pkg.root().display()));
    if let Some(d) = &m.description {
        out.push_str(&format!("描述      : {d}\n"));
    }
    if !m.authors.is_empty() {
        out.push_str(&format!("作者      : {}\n", m.authors.join(", ")));
    }
    out.push_str(&format!(
        "许可      : {}\n",
        m.license.as_deref().unwrap_or("(未声明)")
    ));
    out.push_str(&format!("目标游戏  : {}\n", m.target.game));
    out.push_str(&format!(
        "loader 要求: {}\n",
        m.min_loader_version.as_deref().unwrap_or("(无要求)")
    ));
    out.push_str(&format!(
        "校验和    : {}\n",
        if pkg.checksums().is_some() {
            "已提供"
        } else {
            "缺失"
        }
    ));
    out.push_str(&format!(
        "\nIR: mesh={} skeleton={} blendshape={} material={} texture={}\n",
        m.ir.mesh.replace.len(),
        m.ir.skeleton.map.len(),
        m.ir.blendshape.len(),
        m.ir.material.overrides.len(),
        m.ir.texture.replace.len()
    ));
    let files = m.referenced_files();
    out.push_str(&format!("\n被引用文件 ({}):\n", files.len()));
    for rel in files {
        let present = pkg.root().join(&rel).is_file();
        out.push_str(&format!("  [{}] {rel}\n", if present { "✓" } else { "✗" }));
    }
    out
}

/// 初始化终端。
fn setup() -> Result<Term> {
    enable_raw_mode().context("启用 raw mode 失败（当前不是交互式终端？）")?;
    let mut stdout = std::io::stdout();
    execute!(stdout, EnterAlternateScreen).context("进入备用屏幕失败")?;
    let backend = CrosstermBackend::new(stdout);
    Terminal::new(backend).context("创建终端失败")
}

/// 恢复终端状态。
fn restore(terminal: &mut Term) -> Result<()> {
    disable_raw_mode().context("退出 raw mode 失败")?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen).context("离开备用屏幕失败")?;
    terminal.show_cursor().context("恢复光标失败")?;
    Ok(())
}

/// 启动 TUI。
pub fn run(mods_dir: PathBuf) -> Result<()> {
    let mut terminal = setup()?;
    let mut app = App::new(mods_dir);
    let loop_result = event_loop(&mut terminal, &mut app);
    let restore_result = restore(&mut terminal);
    loop_result.and(restore_result)
}

/// 主事件循环。
fn event_loop(terminal: &mut Term, app: &mut App) -> Result<()> {
    while !app.should_quit {
        terminal.draw(|f| draw(f, app))?;
        if event::poll(Duration::from_millis(250))? {
            if let Event::Key(key) = event::read()? {
                if key.kind == KeyEventKind::Press {
                    app.handle_key(key.code);
                }
            }
        }
    }
    Ok(())
}

/// 渲染一帧。
fn draw(f: &mut Frame, app: &mut App) {
    let rows = Layout::vertical([Constraint::Min(5), Constraint::Length(1)]).split(f.area());
    let (body, status_area) = (rows[0], rows[1]);

    let cols =
        Layout::horizontal([Constraint::Percentage(38), Constraint::Percentage(62)]).split(body);
    let (left, right) = (cols[0], cols[1]);
    let right_rows =
        Layout::vertical([Constraint::Percentage(55), Constraint::Percentage(45)]).split(right);

    let mut items: Vec<ListItem> = app
        .packages
        .iter()
        .map(|pkg| {
            let m = pkg.manifest();
            ListItem::new(Line::from(vec![
                Span::styled(
                    m.name.clone(),
                    Style::default().add_modifier(Modifier::BOLD),
                ),
                Span::raw("  "),
                Span::styled(m.version.clone(), Style::default().fg(Color::DarkGray)),
            ]))
        })
        .collect();
    for (path, err) in &app.failed {
        let name = path
            .file_name()
            .map(|s| s.to_string_lossy().to_string())
            .unwrap_or_else(|| path.display().to_string());
        items.push(ListItem::new(Line::from(Span::styled(
            format!("✗ {name} — {err}"),
            Style::default().fg(Color::Red),
        ))));
    }

    let list = List::new(items)
        .block(
            Block::default()
                .borders(Borders::ALL)
                .title(format!(" Mods · {} ", app.mods_dir.display())),
        )
        .highlight_style(
            Style::default()
                .fg(Color::Black)
                .bg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        )
        .highlight_symbol("▶ ");
    f.render_stateful_widget(list, left, &mut app.state);

    f.render_widget(
        Paragraph::new(app.detail.as_str())
            .block(Block::default().borders(Borders::ALL).title(" 清单 "))
            .wrap(Wrap { trim: false }),
        right_rows[0],
    );
    f.render_widget(
        Paragraph::new(app.validation.as_str())
            .block(Block::default().borders(Borders::ALL).title(" 校验 "))
            .wrap(Wrap { trim: false }),
        right_rows[1],
    );

    let status = Paragraph::new(Line::from(vec![
        Span::styled(" q ", Style::default().fg(Color::Yellow)),
        Span::raw("退出 "),
        Span::styled("j/k ", Style::default().fg(Color::Yellow)),
        Span::raw("移动 "),
        Span::styled("Enter ", Style::default().fg(Color::Yellow)),
        Span::raw("校验 "),
        Span::styled("r ", Style::default().fg(Color::Yellow)),
        Span::raw("重扫  "),
        Span::styled(app.status.clone(), Style::default().fg(Color::Green)),
    ]));
    f.render_widget(status, status_area);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    const MANIFEST: &str = r#"
format_version = 1
id = "tui-demo"
name = "TUI Demo"
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

    fn temp_package(tag: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!("efml4a-tui-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("mesh")).expect("应能创建目录");
        fs::write(root.join("mod.toml"), MANIFEST).expect("应能写清单");
        fs::write(root.join("mesh/a.bin"), b"data").expect("应能写数据");
        root
    }

    #[test]
    fn describe_contains_manifest_and_files() {
        let root = temp_package("describe");
        let pkg = ModPackage::open(&root).expect("应能打开");
        let text = describe(&pkg);
        assert!(text.contains("TUI Demo"), "{text}");
        assert!(text.contains("mesh/a.bin"), "{text}");
        assert!(text.contains("IR: mesh=1"), "{text}");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn app_reload_and_move_selection() {
        let base = std::env::temp_dir().join(format!("efml4a-tui-mods-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        fs::create_dir_all(base.join("one/mesh")).expect("应能创建");
        fs::write(base.join("one/mod.toml"), MANIFEST).expect("应能写");
        fs::write(base.join("one/mesh/a.bin"), b"x").expect("应能写");

        let mut app = App::new(base.clone());
        assert_eq!(app.packages.len(), 1);
        assert_eq!(app.state.selected(), Some(0));

        // 单个元素时环形移动应停在原地。
        app.move_selection(1);
        assert_eq!(app.state.selected(), Some(0));

        // 方向键与 q 键行为。
        app.handle_key(KeyCode::Char('j'));
        assert_eq!(app.state.selected(), Some(0));
        app.handle_key(KeyCode::Enter);
        assert!(app.validation.contains("结构校验"), "{}", app.validation);
        app.handle_key(KeyCode::Char('q'));
        assert!(app.should_quit);

        let _ = fs::remove_dir_all(&base);
    }
}
