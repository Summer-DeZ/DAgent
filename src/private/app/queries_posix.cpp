#include "app/queries.hpp"

#include <utility>
#include <array>
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>

namespace dagent::app {

void QueryGatewayImpl::watch_workspace(std::function<void()> changed) {
    watcher_ = {};
    if (!changed) return;
    watcher_ = std::jthread([this, changed = std::move(changed)](std::stop_token stop) {
        const int fd = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK), wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (fd < 0 || wake < 0) {
            if (fd >= 0) ::close(fd);
            if (wake >= 0) ::close(wake);
            return;
        }
        {
            std::stop_callback cancel(stop, [wake] { const uint64_t one = 1; (void)::write(wake, &one, sizeof(one)); });
            std::map<int, std::filesystem::path> watched;
            const auto add = [&](const std::filesystem::path& path) {
                if (watched.size() >= 4096) return;
                const auto wd = ::inotify_add_watch(fd, path.c_str(), IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);
                if (wd >= 0) watched[wd] = path;
            };
            const auto scan = [&] {
                add(project_root_);
                std::error_code error;
                for (std::filesystem::recursive_directory_iterator it(project_root_, std::filesystem::directory_options::skip_permission_denied, error), end;
                     it != end && !stop.stop_requested() && watched.size() < 4096; it.increment(error)) {
                    const auto path = it->path();
                    if (!should_watch_directory(path)) { it.disable_recursion_pending(); continue; }
                    if (it->is_directory(error) && !it->is_symlink(error)) add(path);
                }
            };
            scan();
            std::array<pollfd, 2> descriptors{{{fd, POLLIN, 0}, {wake, POLLIN, 0}}};
            alignas(inotify_event) std::array<char, 16384> buffer;
            while (!stop.stop_requested() && ::poll(descriptors.data(), descriptors.size(), -1) > 0) {
                if (descriptors[1].revents) break;
                bool dirty = false, rescan = false;
                for (ssize_t size; (size = ::read(fd, buffer.data(), buffer.size())) > 0;) {
                    for (ssize_t offset = 0; offset < size;) {
                        const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                        dirty = true;
                        rescan = rescan || (event->mask & IN_Q_OVERFLOW) || ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)));
                        if (event->mask & IN_IGNORED) watched.erase(event->wd);
                        offset += sizeof(inotify_event) + event->len;
                    }
                }
                if (rescan) scan();
                if (dirty) changed();
            }
        }
        ::close(fd); ::close(wake);
    });
}

} // namespace dagent::app
