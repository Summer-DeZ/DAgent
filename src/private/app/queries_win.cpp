#include "app/queries.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <windows.h>

namespace dagent::app {
namespace {

namespace fs = std::filesystem;

constexpr DWORD change_filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;

// 每个受监视目录一个 overlapped 请求；完成键就是本结构的地址。
struct Watch {
    fs::path path;
    HANDLE directory = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    bool pending = false;
    std::array<DWORD, (64 << 10) / sizeof(DWORD)> buffer{}; // ReadDirectoryChangesW 要求 DWORD 对齐

    ~Watch() {
        if (directory != INVALID_HANDLE_VALUE) ::CloseHandle(directory);
    }

    bool arm() {
        overlapped = {};
        pending = ::ReadDirectoryChangesW(directory, buffer.data(), static_cast<DWORD>(sizeof(buffer)), FALSE,
                                          change_filter, nullptr, &overlapped, nullptr) != 0;
        return pending;
    }
};

bool is_reparse_point(HANDLE directory) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    return !::GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &info, sizeof(info)) ||
           (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

} // namespace

void QueryGatewayImpl::watch_workspace(std::function<void()> changed) {
    watcher_ = {};
    if (!changed) return;
    watcher_ = std::jthread([this, changed = std::move(changed)](std::stop_token stop) {
        const HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (!port) return;
        {
            // 取消用完成键 0 唤醒等待；与 POSIX 的 eventfd 对应。
            std::stop_callback cancel(stop, [port] { ::PostQueuedCompletionStatus(port, 0, 0, nullptr); });
            std::map<Watch*, std::unique_ptr<Watch>> watched;
            std::set<fs::path> paths;
            const auto add = [&](const fs::path& path) {
                if (watched.size() >= 4096 || paths.contains(path)) return;
                auto watch = std::make_unique<Watch>();
                watch->path = path;
                watch->directory = ::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                 OPEN_EXISTING,
                                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT,
                                                 nullptr);
                if (watch->directory == INVALID_HANDLE_VALUE || is_reparse_point(watch->directory)) return;
                if (!::CreateIoCompletionPort(watch->directory, port, reinterpret_cast<ULONG_PTR>(watch.get()), 0)) return;
                if (!watch->arm()) return;
                paths.insert(path);
                watched.emplace(watch.get(), std::move(watch));
            };
            const auto scan = [&] {
                add(project_root_);
                std::error_code error;
                for (fs::recursive_directory_iterator it(project_root_, fs::directory_options::skip_permission_denied, error), end;
                     it != end && !stop.stop_requested() && watched.size() < 4096; it.increment(error)) {
                    const auto path = it->path();
                    if (!should_watch_directory(path)) { it.disable_recursion_pending(); continue; }
                    if (it->is_directory(error) && !it->is_symlink(error)) add(path);
                }
            };
            scan();
            while (!stop.stop_requested()) {
                DWORD bytes = 0;
                ULONG_PTR key = 0;
                OVERLAPPED* overlapped = nullptr;
                const BOOL ok = ::GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, INFINITE);
                if (!ok && overlapped == nullptr) break; // 完成端口本身出错
                if (key == 0) break;                      // 取消唤醒
                auto* watch = reinterpret_cast<Watch*>(key);
                const auto found = watched.find(watch);
                if (found == watched.end()) continue;
                watch->pending = false;
                bool dirty = true, rescan = false;
                if (!ok) {
                    // 目录被删除/重命名或缓冲区溢出：移除该监视，溢出时重新扫描。
                    rescan = ::GetLastError() == ERROR_NOTIFY_ENUM_DIR;
                    paths.erase(watch->path);
                    watched.erase(found);
                } else if (bytes == 0) {
                    rescan = true;
                    if (!watch->arm()) { paths.erase(watch->path); watched.erase(found); }
                } else {
                    for (std::size_t offset = 0;;) {
                        const auto* event = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(reinterpret_cast<const std::byte*>(watch->buffer.data()) + offset);
                        if (event->Action == FILE_ACTION_ADDED || event->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                            const fs::path entry = watch->path / std::wstring(event->FileName, event->FileNameLength / sizeof(wchar_t));
                            const DWORD attributes = ::GetFileAttributesW(entry.c_str());
                            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) rescan = true;
                        }
                        if (event->NextEntryOffset == 0) break;
                        offset += event->NextEntryOffset;
                    }
                    if (!watch->arm()) { paths.erase(watch->path); watched.erase(found); }
                }
                if (rescan) scan();
                if (dirty) changed();
            }
            // 取消全部在途请求，收齐完成通知后才释放缓冲区与句柄。
            std::size_t pending = 0;
            for (auto& [key, watch] : watched)
                if (watch->pending && ::CancelIoEx(watch->directory, &watch->overlapped)) ++pending;
                else watch->pending = false;
            while (pending > 0) {
                DWORD bytes = 0;
                ULONG_PTR key = 0;
                OVERLAPPED* overlapped = nullptr;
                ::GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, INFINITE);
                if (key != 0 && overlapped != nullptr) --pending;
            }
        }
        ::CloseHandle(port);
    });
}

} // namespace dagent::app
