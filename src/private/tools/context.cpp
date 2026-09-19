#include "tools/tools.hpp"

#include <map>
#include <mutex>

namespace dagent::tools {
namespace fs = std::filesystem;

struct Context::Impl {
    fs::path root;
    Options options;
    workspace::FileOptions files;
    workspace::SearchOptions search;
    exec::Options process;

    // FileTracker：resolve 之后的路径 → 模型读到/写到时的 Stamp。只读调用可能被核心并行执行，加锁。
    std::mutex tracker_mutex;
    std::map<std::string, workspace::Stamp> tracked;
};

Context::Context(fs::path root, Options opt, workspace::FileOptions files, workspace::SearchOptions search,
                 exec::Options process)
    : impl_(std::make_unique<Impl>()) {
    impl_->root = std::move(root);
    impl_->options = std::move(opt);
    impl_->files = std::move(files);
    impl_->search = std::move(search);
    impl_->process = std::move(process);
}

Context::~Context() = default;

const fs::path& Context::root() const { return impl_->root; }
const Options& Context::options() const { return impl_->options; }
const workspace::FileOptions& Context::files() const { return impl_->files; }
const workspace::SearchOptions& Context::search() const { return impl_->search; }
const exec::Options& Context::process() const { return impl_->process; }

std::optional<workspace::Stamp> Context::tracked_stamp(const workspace::Resolved& resolved) const {
    const std::lock_guard lock(impl_->tracker_mutex);
    const auto it = impl_->tracked.find(resolved.path.string());
    if (it == impl_->tracked.end()) return std::nullopt;
    return it->second;
}

void Context::track(const workspace::Resolved& resolved, workspace::Stamp stamp) {
    const std::lock_guard lock(impl_->tracker_mutex);
    impl_->tracked.insert_or_assign(resolved.path.string(), stamp);
}

} // namespace dagent::tools
