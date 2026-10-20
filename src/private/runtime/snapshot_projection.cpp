#include "runtime/snapshot_projection.hpp"

#include "base/overloaded.hpp"
#include "runtime/factory.hpp"

namespace dagent::runtime::detail {
namespace {

void project_usage(RuntimeSnapshot& snapshot, std::size_t used, std::size_t limit) {
    snapshot.used_tokens = used;
    snapshot.token_limit = limit;
}

void project_mode(RuntimeSnapshot& snapshot, agent::PermissionMode mode, bool planning) {
    snapshot.permission_mode = mode;
    snapshot.planning = planning;
}

} // namespace

RuntimeSnapshot snapshot_from_session(SessionInstance& instance) {
    agent::Session& session = instance.session();
    RuntimeSnapshot next;
    next.session_id = session.meta().id;
    next.model = session.config().provider;
    next.trigger_percent = session.config().options.context.compaction_trigger_percent;
    next.window_tokens = session.config().options.context.window_tokens;
    next.cwd = session.config().cwd;
    next.project_root = session.config().project_root;
    const agent::SessionSnapshot values = session.snapshot(); // 执行线程上的即时值
    project_mode(next, values.permission_mode, values.planning);
    next.read_only = values.read_only;
    project_usage(next, values.used_tokens, values.token_limit);
    next.work_plan = values.plan;
    next.grants = values.grants;
    next.recording_broken = session.committer().broken();
    next.recording_error = session.committer().error();
    return next;
}

void apply_snapshot_event(RuntimeSnapshot& snapshot, const Event& event) {
    // EventPayload 是 variant<agent::Event, ControlEvent>：核心事件要先取出内层 variant 再分派，
    // 否则快照字段不会随实时事件更新（前端按快照显示标签，必须保持一致）。
    const auto* core = std::get_if<agent::Event>(&event.payload);
    if (core == nullptr) return;
    std::visit(base::Overloaded{
                   [&](const agent::ContextUpdate& update) {
                       project_usage(snapshot, update.used, update.limit);
                   },
                   [&](const agent::ModeChanged& mode) {
                       project_mode(snapshot,
                           mode.mode == "ask" ? agent::PermissionMode::ask
                           : mode.mode == "unrestricted" ? agent::PermissionMode::unrestricted
                                                         : agent::PermissionMode::workspace,
                           mode.planning);
                       if (mode.planning) snapshot.read_only = true;
                   },
                   [&](const agent::ModelChanged& model) { snapshot.model.model = model.model; },
                   [&](const agent::ToolFinished& finished) {
                       if (const auto* plan = std::get_if<agent::TodoView>(&finished.result.display))
                           snapshot.work_plan = *plan;
                   },
                   [&](const auto&) {},
               },
               *core);
}

} // namespace dagent::runtime::detail
