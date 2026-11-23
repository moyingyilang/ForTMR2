//! 四条 Android 注入路线的评估矩阵（研究结论的结构化沉淀）。
//!
//! 这张表是 `docs/07-route-matrix.md` 的**可执行版本**：CLI（`efml routes`）与
//! 混合架构 GUI（`GET /api/routes`）都从这里取数据，避免同一份结论在三个地方各写一遍
//! 然后慢慢漂移。
//!
//! 评分口径：
//! - **可行性**：在当前目标设备（已 root 的 Android 17 / Snapdragon 8 Elite / Adreno）
//!   上能否做到。
//! - **工程量**：从零到「能替换一个模型」所需的工作量。
//! - **稳定性**：跨游戏小版本更新与跨设备的表现。
//! - **检测风险**：被游戏完整性校验发现的难度（低 = 更难被发现）。
//! - **版本鲁棒性**：对游戏版本变化的容忍度。

/// 一条注入路线的评估数据。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RouteInfo {
    /// 路线名称。
    pub name: &'static str,
    /// PC 上最接近的对应物。
    pub pc_analog: &'static str,
    /// 前置条件。
    pub prerequisite: &'static str,
    /// 注入点。
    pub injection_point: &'static str,
    /// 拦截粒度。
    pub granularity: &'static str,
    /// 可行性评级。
    pub feasibility: &'static str,
    /// 工程量评级。
    pub effort: &'static str,
    /// 稳定性评级。
    pub stability: &'static str,
    /// 检测风险评级（低 = 更难被发现）。
    pub detection_risk: &'static str,
    /// 对游戏版本变化的鲁棒性。
    pub version_robustness: &'static str,
    /// 一句话结论。
    pub verdict: &'static str,
}

/// 四条路线的评估结论。
pub const ROUTES: &[RouteInfo] = &[
    RouteInfo {
        name: "Vulkan 隐式层",
        pc_analog: "3DMigoto / EFMI",
        prerequisite: "root + 目标 app 被判定为可调试",
        injection_point: "Vulkan loader 加载 layer",
        granularity: "逐 vk* 调用 / 逐 draw",
        feasibility: "高",
        effort: "高",
        stability: "中",
        detection_risk: "中低",
        version_robustness: "高",
        verdict: "与 EFMI 类比最强：同在图形 API 边界替换顶点/索引缓冲，游戏逻辑完全不知情。",
    },
    RouteInfo {
        name: "IL2CPP hook",
        pc_analog: "无（Unity 逻辑层）",
        prerequisite: "root + LSPosed/Zygisk 或 frida",
        injection_point: "libil2cpp.so 方法入口",
        granularity: "Unity 对象 / Mesh",
        feasibility: "高",
        effort: "中高",
        stability: "中",
        detection_risk: "高",
        version_robustness: "低",
        verdict: "实现最直观、能改到材质与 shader 选择，但每个游戏版本都要重新适配，且易被完整性校验发现。",
    },
    RouteInfo {
        name: "AssetBundle 替换",
        pc_analog: "无（EFMI 刻意不做）",
        prerequisite: "root，或重打包 APK",
        injection_point: "磁盘资源 / open 重定向",
        granularity: "整包资源",
        feasibility: "中",
        effort: "中",
        stability: "高",
        detection_risk: "高",
        version_robustness: "低",
        verdict: "门槛最低且支持热替换整包，但无法做骨骼合并/shape key 这类需要 GPU 侧数据参与的注入，且触及资源版权红线。",
    },
    RouteInfo {
        name: "frida / ptrace 辅助",
        pc_analog: "无",
        prerequisite: "root（可调试或 ptrace 授权）",
        injection_point: "进程附加",
        granularity: "内存 / 函数级",
        feasibility: "高",
        effort: "低",
        stability: "低",
        detection_risk: "高",
        version_robustness: "低",
        verdict: "适合做研究与逆向阶段的探针（dump、抓参数），不适合作为最终加载器形态。",
    },
];

/// 按名称精确查找一条路线。
pub fn by_name(name: &str) -> Option<&'static RouteInfo> {
    ROUTES.iter().find(|r| r.name == name)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn covers_the_four_evaluated_routes() {
        assert_eq!(ROUTES.len(), 4);
        assert!(by_name("Vulkan 隐式层").is_some());
        assert!(by_name("IL2CPP hook").is_some());
        assert!(by_name("AssetBundle 替换").is_some());
        assert!(by_name("frida / ptrace 辅助").is_some());
        assert!(by_name("不存在的路线").is_none());
    }

    #[test]
    fn vulkan_layer_is_the_strongest_pc_analog() {
        let r = by_name("Vulkan 隐式层").expect("应存在");
        assert!(r.pc_analog.contains("3DMigoto"));
        assert_eq!(r.version_robustness, "高");
    }

    #[test]
    fn every_route_documents_prerequisites_and_verdict() {
        for r in ROUTES {
            assert!(!r.prerequisite.is_empty(), "{} 缺少前置条件", r.name);
            assert!(!r.verdict.is_empty(), "{} 缺少结论", r.name);
            assert!(!r.injection_point.is_empty(), "{} 缺少注入点", r.name);
        }
    }
}
