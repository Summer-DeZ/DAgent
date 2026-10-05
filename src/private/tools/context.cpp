#include "tools/tools.hpp"

#include <map>
#include <list>
#include <algorithm>
#include <mutex>

#include "agent/permission.hpp"

namespace dagent::tools {
namespace fs = std::filesystem;

struct Context::Impl {
    fs::path root;
    fs::path control_root;
    Options options;
    workspace::FileOptions files;
    workspace::SearchOptions search;
    exec::Options process;

    // FileTracker：resolve 之后的路径 → 模型读到/写到时的 Stamp。只读调用可能被核心并行执行，加锁。
    std::mutex tracker_mutex;
    std::list<std::pair<std::string, std::shared_ptr<const web::Page>>> pages;
    std::size_t page_bytes = 0;
    std::map<std::string, workspace::Stamp> tracked;
};

Context::Context(fs::path root, fs::path control_root, Options opt, workspace::FileOptions files,
                 workspace::SearchOptions search, exec::Options process)
    : impl_(std::make_unique<Impl>()) {
    impl_->root = std::move(root);
    impl_->control_root = std::move(control_root);
    impl_->options = std::move(opt);
    impl_->files = std::move(files);
    impl_->search = std::move(search);
    impl_->process = std::move(process);
}

Context::~Context() = default;

const fs::path& Context::root() const { return impl_->root; }
const fs::path& Context::control_root() const { return impl_->control_root; }

bool Context::content_restricted(const fs::path& path, bool inside_workspace) const {
    return agent::classify_resource(path, inside_workspace, impl_->control_root) != agent::ResourceClass::normal;
}
namespace {
bool within(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}
}
bool Context::can_read(const Grant& grant, const fs::path& path) const {
    if (!std::ranges::any_of(grant.readable, [&](const auto& root) { return within(path, root); })) return false;
    if (!grant.protect_sensitive_names) return true;
    const auto category = agent::classify_resource(path, within(path, root()), control_root());
    if (category == agent::ResourceClass::normal || category == agent::ResourceClass::outside) return true;
    return std::ranges::find(grant.read_exceptions, path) != grant.read_exceptions.end();
}
void Context::require_access(const Grant& grant, const workspace::Resolved& target, agent::Access access) const {
    const bool allowed = access == agent::Access::read ? can_read(grant, target.path)
        : std::ranges::find(grant.writable, target.path) != grant.writable.end();
    if (!allowed) throw workspace::WorkspaceError(workspace::WorkspaceError::Kind::io,
                                                  "file access is outside the approved scope");
}
const Options& Context::options() const { return impl_->options; }
const workspace::FileOptions& Context::files() const { return impl_->files; }
const workspace::SearchOptions& Context::search() const { return impl_->search; }
const exec::Options& Context::process() const { return impl_->process; }

std::shared_ptr<const web::Page> Context::cached_page(std::string_view url) {
    const std::lock_guard lock(impl_->tracker_mutex);
    const auto it = std::ranges::find_if(impl_->pages, [&](const auto& page) { return page.first == url; });
    if (it == impl_->pages.end()) return {};
    impl_->pages.splice(impl_->pages.begin(), impl_->pages, it);
    return impl_->pages.front().second;
}

void Context::cache_page(std::string url, std::shared_ptr<const web::Page> page) {
    const std::lock_guard lock(impl_->tracker_mutex);
    const auto size = [](const auto& entry) {
        const auto& p = *entry.second;
        return entry.first.size() + p.url.size() + p.title.size() + p.content_type.size() + p.text.size();
    };
    const auto existing = std::ranges::find_if(impl_->pages, [&](const auto& p) { return p.first == url; });
    if (existing != impl_->pages.end()) { impl_->page_bytes -= size(*existing); impl_->pages.erase(existing); }
    impl_->pages.emplace_front(std::move(url), std::move(page));
    impl_->page_bytes += size(impl_->pages.front());
    while (impl_->page_bytes > impl_->options.web.cache_bytes && !impl_->pages.empty()) {
        impl_->page_bytes -= size(impl_->pages.back()); impl_->pages.pop_back();
    }
}

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
