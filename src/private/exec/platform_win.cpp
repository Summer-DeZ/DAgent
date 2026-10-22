#include "exec/platform.hpp"
#include "exec/detail.hpp"

#include "base/path.hpp"
#include "base/platform.hpp"

#include <boost/process/v2/stdio.hpp>
#include <boost/process/v2/windows/default_launcher.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <system_error>
#include <utility>
#include <windows.h>

namespace dagent::exec {
namespace {

namespace fs = std::filesystem;

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

[[noreturn]] void fail(const char* operation, DWORD error = ::GetLastError()) {
    throw ExecError(ExecError::Kind::spawn_failed,
                    std::string(operation) + ": " + std::system_category().message(static_cast<int>(error)));
}

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (!size) fail("MultiByteToWideChar");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (!::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                               result.data(), size)) fail("MultiByteToWideChar");
    return result;
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) fail("WideCharToMultiByte");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                               result.data(), size, nullptr, nullptr)) fail("WideCharToMultiByte");
    return result;
}

int compare(std::wstring_view left, std::wstring_view right) {
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                   static_cast<int>(right.size()), TRUE);
}

bool native_extension(const fs::path& path) {
    const auto ext = path.extension().native();
    return compare(ext, L".exe") == CSTR_EQUAL || compare(ext, L".com") == CSTR_EQUAL;
}

// Windows 的系统目录 API 返回所需长度；不要从待执行命令的 PATH 推导这些默认值。
std::wstring system_directory(UINT (WINAPI *query)(LPWSTR, UINT)) {
    std::wstring buffer(MAX_PATH + 1, L'\0');
    for (;;) {
        const UINT size = query(buffer.data(), static_cast<UINT>(buffer.size()));
        if (!size) fail("system directory");
        if (size < buffer.size()) { buffer.resize(size); return buffer; }
        buffer.resize(static_cast<std::size_t>(size) + 1);
    }
}

} // namespace

fs::path system_git_program() { return fs::path(L"C:\\Program Files\\Git\\cmd\\git.exe"); }

fs::path shell_path_to_native(std::string_view shell_path) {
    // MSYS 形式 /c/dir 或 /C/...：单个字母盘符后跟 / 或串尾。
    if (shell_path.size() >= 2 && shell_path.front() == '/' &&
        ((shell_path[1] >= 'a' && shell_path[1] <= 'z') || (shell_path[1] >= 'A' && shell_path[1] <= 'Z')) &&
        (shell_path.size() == 2 || shell_path[2] == '/')) {
        std::string native;
        native += static_cast<char>(std::toupper(static_cast<unsigned char>(shell_path[1])));
        native += ':';
        native += shell_path.substr(2);
        return fs::path(native);
    }
    return fs::path(shell_path);
}

std::string default_search_path() {
    const auto git_root = system_git_program().parent_path().parent_path();
    return base::path_to_utf8(git_root / L"cmd") + ";" + base::path_to_utf8(git_root / L"bin") + ";" +
           utf8(system_directory(::GetSystemDirectoryW)) + ";" + utf8(system_directory(::GetWindowsDirectoryW));
}

std::string project_search_path() {
    const auto path = base::environment_variable("PATH");
    return path && !path->empty() ? *path : default_search_path();
}

namespace detail {
namespace asio = boost::asio;
namespace bp = boost::process::v2;

struct InheritedResources {
    std::vector<HANDLE> handles;
};

std::vector<std::string> inherited_environment() {
    struct FreeEnvironment { void operator()(wchar_t* block) const { ::FreeEnvironmentStringsW(block); } };
    std::unique_ptr<wchar_t, FreeEnvironment> block(::GetEnvironmentStringsW());
    if (!block) fail("GetEnvironmentStringsW");
    std::vector<std::string> result;
    for (const wchar_t* entry = block.get(); *entry;) {
        const std::wstring_view value(entry);
        result.push_back(utf8(value));
        entry += value.size() + 1;
    }
    return result;
}

bool environment_names_equal(std::string_view left, std::string_view right) {
    if (left.empty() || right.empty()) return left.empty() && right.empty();
    return compare(wide(left), wide(right)) == CSTR_EQUAL;
}

namespace {

// 按 UTF-16 字符匹配 *、?、[]、[!...]、范围和反斜杠转义；大小写使用 Windows ordinal 规则。
// 星号仅记住最后一个回溯点，不递归展开模式。
bool glob(std::wstring_view name, std::wstring_view pattern) {
    std::size_t n = 0, p = 0, star = std::wstring_view::npos, retry = 0;
    auto equal = [](wchar_t a, wchar_t b) { return compare({&a, 1}, {&b, 1}) == CSTR_EQUAL; };
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == L'*') { star = ++p; retry = n; continue; }
        auto next = p;
        bool matched = false;
        if (next < pattern.size()) {
            const wchar_t token = pattern[next++];
            if (token == L'?') matched = true;
            else if (token == L'\\' && next < pattern.size()) matched = equal(name[n], pattern[next++]);
            else if (token == L'[') {
                const bool negate = next < pattern.size() && (pattern[next] == L'!' || pattern[next] == L'^');
                if (negate) ++next;
                bool any = false;
                const auto first = next;
                while (next < pattern.size() && (pattern[next] != L']' || next == first)) {
                    wchar_t low = pattern[next++];
                    if (low == L'\\' && next < pattern.size()) low = pattern[next++];
                    if (next + 1 < pattern.size() && pattern[next] == L'-' && pattern[next + 1] != L']') {
                        ++next;
                        wchar_t high = pattern[next++];
                        if (high == L'\\' && next < pattern.size()) high = pattern[next++];
                        any |= compare({&low, 1}, name.substr(n, 1)) != CSTR_GREATER_THAN &&
                               compare(name.substr(n, 1), {&high, 1}) != CSTR_GREATER_THAN;
                    } else any |= equal(name[n], low);
                }
                if (next < pattern.size()) { ++next; matched = negate ? !any : any; }
                else { next = p + 1; matched = equal(name[n], L'['); }
            } else matched = equal(name[n], token);
        }
        if (matched) { ++n; p = next; }
        else if (star != std::wstring_view::npos) { n = ++retry; p = star; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}

std::vector<std::wstring> executable_extensions(const std::vector<std::string>& environment) {
    auto text = find_env(environment, "PATHEXT");
    if (text.empty()) text = ".EXE;.COM";
    std::vector<std::wstring> result;
    for (;;) {
        const auto separator = text.find(';');
        auto extension = wide(text.substr(0, separator));
        if (compare(extension, L".exe") == CSTR_EQUAL || compare(extension, L".com") == CSTR_EQUAL)
            result.push_back(std::move(extension));
        if (separator == std::string_view::npos) return result;
        text.remove_prefix(separator + 1);
    }
}

fs::path executable_candidate(const fs::path& path, const std::vector<std::wstring>& extensions) {
    if (path.has_extension()) return regular_executable(path) ? path : fs::path{};
    for (const auto& extension : extensions) {
        auto candidate = path;
        candidate += extension;
        if (regular_executable(candidate)) return candidate;
    }
    return {};
}

std::wstring environment_block(const std::vector<std::string>& environment) {
    std::vector<std::wstring> entries;
    entries.reserve(environment.size());
    for (const auto& entry : environment) entries.push_back(wide(entry));
    std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return compare(std::wstring_view(left).substr(0, left.find(L'=')),
                       std::wstring_view(right).substr(0, right.find(L'='))) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& entry : entries) { block += entry; block.push_back(L'\0'); }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

struct LaunchSetup {
    std::wstring& environment;
    const Command& command;
    Handle& thread;
    std::vector<std::pair<HANDLE, DWORD>> flags;

    ~LaunchSetup() {
        for (const auto& [handle, original] : flags)
            ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, original & HANDLE_FLAG_INHERIT);
    }

    boost::system::error_code on_setup(bp::windows::default_launcher& launcher, const fs::path&, std::wstring&) {
        launcher.creation_flags |= CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT;
        launcher.environment = environment.data();
        launcher.current_directory = command.cwd;
        if (command.inherited_resources) {
            for (HANDLE handle : command.inherited_resources->handles) {
                DWORD original = 0;
                if (!::GetHandleInformation(handle, &original))
                    return {static_cast<int>(::GetLastError()), boost::system::system_category()};
                flags.emplace_back(handle, original);
                if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                    return {static_cast<int>(::GetLastError()), boost::system::system_category()};
                launcher.inherited_handles.push_back(handle);
            }
        }
        auto& handles = launcher.inherited_handles;
        std::sort(handles.begin(), handles.end(), std::less<HANDLE>{});
        handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
        return {};
    }

    // Boost 1.92 的 invoke_on_success 要求此钩子返回 void；只有 on_setup 返回 error_code。
    void on_success(bp::windows::default_launcher& launcher, const fs::path&, std::wstring&) {
        // launcher 原本在此钩子之后关闭线程句柄；移交给本次启动，加入 Job 后才能恢复。
        thread.value = std::exchange(launcher.process_information.hThread, INVALID_HANDLE_VALUE);
    }
};

void launch(const Command& command, const fs::path& program, const std::vector<std::string>& environment,
            asio::io_context& context, bp::process& process, asio::writable_pipe& input,
            asio::readable_pipe& output, asio::readable_pipe& error, ProcessGroup& group,
            bool interactive, bool merge) {
    auto block = environment_block(environment);
    std::vector<std::wstring> arguments;
    for (auto i = command.argv.begin() + 1; i != command.argv.end(); ++i) arguments.push_back(wide(*i));
    try {
        // Boost 的 pipe binding 创建 overlapped 管道；只有父端注册到 Asio，子端留给 stdio。
        auto in = interactive ? bp::detail::process_input_binding(input) : bp::detail::process_input_binding(nullptr);
        auto out = bp::detail::process_output_binding(output);
        auto err = merge ? bp::detail::process_error_binding(out.h.get()) : bp::detail::process_error_binding(error);
        bp::process_stdio stdio{std::move(in), std::move(out), std::move(err)};
        Handle thread;
        LaunchSetup setup{block, command, thread, {}};
        bp::windows::default_launcher launcher;
        process = launcher(context.get_executor(), program, arguments, stdio, setup);
        try {
            group.capture(process);
            if (::ResumeThread(thread.value) == static_cast<DWORD>(-1)) fail("ResumeThread");
        } catch (...) {
            // 不允许任何未进入 Job 的进程继续执行，也不留下挂起进程。
            ::TerminateProcess(process.native_handle(), 1);
            ::WaitForSingleObject(process.native_handle(), INFINITE);
            throw;
        }
    } catch (const boost::system::system_error& exception) {
        throw ExecError(ExecError::Kind::spawn_failed, exception.what());
    }
}

[[noreturn]] void unsupported_sandbox() {
    throw ExecError(ExecError::Kind::sandbox, "sandbox is unsupported on Windows");
}

} // namespace

bool environment_name_matches(std::string_view name, const std::string& pattern) {
    return glob(wide(name), wide(pattern));
}

bool regular_executable(const fs::path& path) {
    return native_extension(path) && base::can_execute(path);
}

fs::path resolve_program(std::string_view name, const fs::path& cwd, const std::vector<std::string>& environment) {
    if (name.empty()) return {};
    const fs::path path = base::path_from_utf8(name);
    // C:foo 依赖进程的每驱动器工作目录；不从宿主隐式引入这种解析状态。
    if (path.has_root_name() && !path.has_root_directory()) return {};
    const fs::path base = fs::absolute(cwd.empty() ? fs::current_path() : cwd);
    const auto extensions = executable_extensions(environment);
    if (path.has_parent_path() || path.has_root_path())
        return executable_candidate(path.is_absolute() ? path : base / path, extensions);
    auto search = find_env(environment, "PATH");
    if (search.empty()) return {};
    for (;;) {
        const auto separator = search.find(';');
        auto directory = search.substr(0, separator);
        if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"')
            directory = directory.substr(1, directory.size() - 2);
        const auto dir = base::path_from_utf8(directory);
        if (!dir.has_root_name() || dir.has_root_directory()) {
            const auto candidate = executable_candidate((dir.is_absolute() ? dir : base / dir) / path, extensions);
            if (!candidate.empty()) return candidate;
        }
        if (separator == std::string_view::npos) return {};
        search.remove_prefix(separator + 1);
    }
}

void ignore_sigpipe() {}

struct ProcessGroup::Impl {
    Handle job{::CreateJobObjectW(nullptr, nullptr)};
    DWORD pid = 0;
    Impl() {
        if (!job.value) fail("CreateJobObjectW");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            fail("SetInformationJobObject");
    }
};

ProcessGroup::ProcessGroup() : impl_(std::make_unique<Impl>()) {}
ProcessGroup::~ProcessGroup() = default;

void ProcessGroup::capture(const bp::process& process) {
    // 保持公共接口的 const process；Boost 的 native_handle() 非 const。
    Handle handle(::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, process.id()));
    if (!handle.value) fail("OpenProcess");
    if (!::AssignProcessToJobObject(impl_->job.value, handle.value)) fail("AssignProcessToJobObject");
    impl_->pid = process.id();
}

bool ProcessGroup::valid() const noexcept { return impl_->pid != 0; }
void ProcessGroup::terminate() noexcept { if (valid()) ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, impl_->pid); }
void ProcessGroup::force_kill() noexcept { if (valid()) ::TerminateJobObject(impl_->job.value, 1); }

ExitStatus process_exit_status(const bp::process& process) noexcept {
    // Result 的既有 int 接口保留 DWORD 的 32 位值，不做 POSIX wait status 解码。
    return {static_cast<int>(process.native_exit_code()), std::nullopt};
}

void spawn_command(const Command& command, const fs::path& program, const std::vector<std::string>& environment,
                   asio::io_context& context, bp::process& process, asio::writable_pipe& input,
                   asio::readable_pipe& output, asio::readable_pipe& error, ProcessGroup& group, bool& spawned) {
    launch(command, program, environment, context, process, input, output, error, group,
           command.stdin_data.has_value(), command.merge_stderr);
    spawned = true;
}

void spawn_child(const Command& command, const fs::path& program, const std::vector<std::string>& environment,
                 asio::io_context& context, bp::process& process, asio::writable_pipe& input,
                 asio::readable_pipe& output, asio::readable_pipe& error, ProcessGroup& group) {
    launch(command, program, environment, context, process, input, output, error, group, true, false);
}

struct SrtControl::Impl {};
SrtControl::SrtControl() { unsupported_sandbox(); }
SrtControl::~SrtControl() = default;
void SrtControl::inherit_bridge_endpoint(Command&) const { unsupported_sandbox(); }
std::string SrtControl::bridge_argument() const { unsupported_sandbox(); }
std::optional<std::string> SrtControl::send_initial(std::string_view) { unsupported_sandbox(); }
void SrtControl::send_decision(std::string_view) { unsupported_sandbox(); }
void SrtControl::send_start(std::string_view) { unsupported_sandbox(); }
std::size_t SrtControl::read_chunk(std::span<char>) { unsupported_sandbox(); }
void SrtControl::shutdown_host() noexcept {}
void SrtControl::close_bridge() noexcept {}
void SrtControl::close() noexcept {}
std::vector<fs::path> default_srt_readable_paths() { unsupported_sandbox(); }

} // namespace detail
} // namespace dagent::exec

namespace dagent::exec::detail {
namespace {

std::string system_message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD size = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (size == 0) return "error " + std::to_string(error);
    std::wstring text(buffer, size);
    ::LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return utf8(text);
}

// CommandLineToArgvW 的引用规则：引号前的反斜杠加倍再转义引号，结尾的反斜杠加倍后闭合。
std::string quote_argument(std::string_view argument) {
    std::string quoted = "\"";
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        quoted.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        quoted += c;
        backslashes = 0;
    }
    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
}

} // namespace

struct SharedLibrary::Impl {
    HMODULE module = nullptr;
    ~Impl() {
        if (module) ::FreeLibrary(module);
    }
};

SharedLibrary::SharedLibrary() : impl_(std::make_unique<Impl>()) {}
SharedLibrary::SharedLibrary(SharedLibrary&&) noexcept = default;
SharedLibrary& SharedLibrary::operator=(SharedLibrary&&) noexcept = default;
SharedLibrary::~SharedLibrary() = default;

SharedLibrary SharedLibrary::open(const fs::path& file) {
    SharedLibrary library;
    library.impl_->module = ::LoadLibraryW(file.c_str());
    if (!library.impl_->module) {
        const DWORD error = ::GetLastError();
        throw ExecError(ExecError::Kind::sandbox, "load " + base::path_to_utf8(file) + ": " + system_message(error));
    }
    return library;
}

void* SharedLibrary::symbol(const char* name) const noexcept {
    return reinterpret_cast<void*>(::GetProcAddress(impl_->module, name));
}

std::string sandbox_command_line(const fs::path& shell, std::string_view script) {
    return quote_argument(base::path_to_utf8(shell)) + " --noprofile --norc -c " + quote_argument(script);
}

bool loopback_peer_is_current_user(unsigned short, unsigned short) { return true; }

} // namespace dagent::exec::detail
