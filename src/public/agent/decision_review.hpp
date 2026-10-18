/// @file decision_review.hpp
/// @brief DecisionReviewer：state 构造、固定问句、打分判定、网页抽样与固定理由。
///
/// 审阅器只产出 ReviewResult / ScanResult：不返回 ExecutionGrant、不弹人工窗口、不调用父 Agent。
/// 整轮关闭与最终路由由 Dispatcher 负责；审阅记录经 DecisionReviewStore 独立保存。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "agent/decision_types.hpp"
#include "agent/port_decision.hpp"

namespace dagent::agent {

/// @brief 一次审阅的只读入口；寿命覆盖一次调度（同一批次共用）。
class DecisionReviewer {
public:
    DecisionReviewer(const DecisionServices& services, const ReviewContext& context,
                     std::string root_turn_id);

    /// @brief 普通动作审阅：普通命令联网 / 工作区外读取 / MCP / web_fetch URL。
    ReviewResult review_action(ReviewScene, const ReviewOperation&, ReviewDeadline, std::stop_token);
    /// @brief 明确目标脚本复审：风险优先，未修改不读正文，修改过的仅审本目标 diff/全文。
    ReviewResult review_script(const ReviewOperation&, const ScriptTarget&, const ScriptEvidence&,
                               ReviewDeadline, std::stop_token);
    /// @brief fetch 结果注入扫描；正文交付前调用，缓存分页同样执行。
    ScanResult scan_web(std::string_view url, std::string_view result_text,
                        const ApprovalIdentity&, ReviewDeadline, std::stop_token);

private:
    const DecisionServices& services_;
    const ReviewContext& context_;
    std::string root_turn_id_;
};

/// @brief 按 2.3 的固定规则抽样：不超过 16 KB 全检，否则头尾各 4 KB 加前后半区各一个随机窗口。
ScanSample sample_fetch_result(std::string_view result);

/// @brief 当前问句与选项说明的版本号；改动措辞即为新版本。
std::string_view decision_question_version();

} // namespace dagent::agent
