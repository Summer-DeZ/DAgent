#include "ipc/channel.hpp"
#include "base/platform.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <system_error>
#include <vector>
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>

namespace dagent::ipc {
namespace {

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct LocalFreeDeleter { void operator()(void* memory) const { ::LocalFree(memory); } };

[[noreturn]] void fail(const char* operation, DWORD error = ::GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

// 管道 DACL 只授予当前用户，不使用会把其他用户带进来的默认 DACL。
struct PipeSecurity {
    std::vector<BYTE> user;
    std::unique_ptr<ACL, LocalFreeDeleter> acl;
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};

    PipeSecurity() {
        Handle token;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token.value)) fail("OpenProcessToken");
        DWORD size = 0;
        ::GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
        user.resize(size);
        if (!::GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) fail("GetTokenInformation");
        EXPLICIT_ACCESS_W access{};
        access.grfAccessPermissions = FILE_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
        PACL result = nullptr;
        const DWORD error = ::SetEntriesInAclW(1, &access, nullptr, &result);
        if (error != ERROR_SUCCESS) fail("SetEntriesInAclW", error);
        acl.reset(result);
        if (!::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !::SetSecurityDescriptorDacl(&descriptor, TRUE, acl.get(), FALSE) ||
            !::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
            fail("pipe security descriptor");
    }
};

std::wstring pipe_name() {
    std::array<UCHAR, 16> random{};
    const NTSTATUS status = ::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) throw std::system_error(std::make_error_code(std::errc::io_error),
        std::format("BCryptGenRandom failed: 0x{:08x}", static_cast<ULONG>(status)));
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = L"\\\\.\\pipe\\dagent-";
    for (UCHAR byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    return name;
}

struct AttributeList {
    std::vector<BYTE> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    explicit AttributeList(std::span<HANDLE> handles) {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage.resize(size);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!::InitializeProcThreadAttributeList(list, 1, 0, &size)) fail("InitializeProcThreadAttributeList");
        if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                          handles.size_bytes(), nullptr, nullptr)) {
            const DWORD error = ::GetLastError();
            ::DeleteProcThreadAttributeList(list);
            fail("UpdateProcThreadAttribute", error);
        }
        value = list;
    }
    ~AttributeList() { ::DeleteProcThreadAttributeList(value); }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
};

void inherit_standard_handle(Handle& target, DWORD stream, DWORD access) {
    const HANDLE source = ::GetStdHandle(stream);
    if (source && source != INVALID_HANDLE_VALUE) {
        if (!::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(), &target.value,
                                0, TRUE, DUPLICATE_SAME_ACCESS)) fail("DuplicateHandle stdio");
    } else {
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        target.value = ::CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (target.value == INVALID_HANDLE_VALUE) fail("CreateFileW NUL");
    }
}

} // namespace

struct detail::EndpointState {
    HANDLE pipe = INVALID_HANDLE_VALUE; // 参数解析不拥有此句柄；只有 Channel 接管后负责关闭。
    Handle read_event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle write_event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED read{};
    OVERLAPPED write{};
    std::mutex mutex;
    std::condition_variable idle;
    bool closing = false;
    bool reading = false;
    bool writing = false;

    EndpointState() {
        if (!read_event.value || !write_event.value) fail("CreateEventW");
        read.hEvent = read_event.value;
        write.hEvent = write_event.value;
    }

    struct Result { DWORD bytes; DWORD error; };

    Result transfer(bool sending, void* data, DWORD size) {
        std::unique_lock lock(mutex);
        if (closing || pipe == INVALID_HANDLE_VALUE) return {0, ERROR_OPERATION_ABORTED};
        auto& operation = sending ? write : read;
        auto& active = sending ? writing : reading;
        const HANDLE event = operation.hEvent;
        operation = {};
        operation.hEvent = event;
        ::ResetEvent(event);
        active = true;
        DWORD bytes = 0;
        const BOOL immediate = sending ? ::WriteFile(pipe, data, size, &bytes, &operation)
                                       : ::ReadFile(pipe, data, size, &bytes, &operation);
        DWORD error = immediate ? ERROR_SUCCESS : ::GetLastError();
        if (error == ERROR_IO_PENDING) {
            lock.unlock();
            // 即使 shutdown 已请求取消，也必须等待完成后才能让调用方回收 data。
            if (::GetOverlappedResult(pipe, &operation, &bytes, TRUE)) error = ERROR_SUCCESS;
            else error = ::GetLastError();
            lock.lock();
        }
        active = false;
        idle.notify_all();
        return {bytes, error};
    }

    void shutdown() {
        std::unique_lock lock(mutex);
        closing = true;
        if (pipe == INVALID_HANDLE_VALUE) return;
        ::CancelIoEx(pipe, nullptr);
        idle.wait(lock, [&] { return !reading && !writing; });
        // 命名管道没有 socket shutdown；关闭已排空的端点才能让另一端收到 EOF。
        if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(std::exchange(pipe, INVALID_HANDLE_VALUE));
    }
};

InheritedEndpoint::InheritedEndpoint() noexcept = default;
InheritedEndpoint::InheritedEndpoint(InheritedEndpoint&&) noexcept = default;
InheritedEndpoint& InheritedEndpoint::operator=(InheritedEndpoint&&) noexcept = default;
InheritedEndpoint::~InheritedEndpoint() = default;

InheritedEndpoint InheritedEndpoint::parse_argument(const char* text) {
    InheritedEndpoint endpoint;
    const std::string_view argument(text);
    std::uintptr_t value = 0;
    const auto [end, error] = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (error != std::errc{} || end != argument.data() + argument.size() ||
        value == 0 || value == std::numeric_limits<std::uintptr_t>::max()) return endpoint;
    endpoint.state_ = std::make_unique<detail::EndpointState>();
    endpoint.state_->pipe = reinterpret_cast<HANDLE>(value);
    return endpoint;
}

bool InheritedEndpoint::valid() const noexcept {
    return state_ && state_->pipe != INVALID_HANDLE_VALUE;
}

Channel::Channel() noexcept = default;
Channel::Channel(Channel&& other) noexcept
    : native_(std::move(other.native_)), buffer_(std::move(other.buffer_)), error_(std::move(other.error_)), eof_(other.eof_) {}
Channel& Channel::operator=(Channel&& other) noexcept {
    if (this == &other) return *this;
    close();
    native_ = std::move(other.native_);
    buffer_ = std::move(other.buffer_);
    error_ = std::move(other.error_);
    eof_ = other.eof_;
    return *this;
}
Channel::~Channel() { close(); }

Channel Channel::adopt(InheritedEndpoint endpoint) {
    Channel channel;
    channel.native_ = std::move(endpoint.state_);
    return channel;
}

std::pair<Channel, Channel> Channel::create_pair() {
    Channel parent, child;
    try {
        parent.native_ = std::make_unique<detail::EndpointState>();
        child.native_ = std::make_unique<detail::EndpointState>();
        PipeSecurity security;
        const auto name = pipe_name();
        parent.native_->pipe = ::CreateNamedPipeW(name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, 0, &security.attributes);
        if (parent.native_->pipe == INVALID_HANDLE_VALUE) fail("CreateNamedPipeW");
        child.native_->pipe = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (child.native_->pipe == INVALID_HANDLE_VALUE) fail("CreateFileW pipe");
        // 客户端已在父进程内打开；ConnectNamedPipe 通常返回 ERROR_PIPE_CONNECTED。
        OVERLAPPED connection{};
        connection.hEvent = parent.native_->read_event.value;
        if (!::ConnectNamedPipe(parent.native_->pipe, &connection)) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_IO_PENDING) {
                DWORD bytes = 0;
                if (!::GetOverlappedResult(parent.native_->pipe, &connection, &bytes, TRUE)) fail("ConnectNamedPipe");
            } else if (error != ERROR_PIPE_CONNECTED) fail("ConnectNamedPipe", error);
        }
    } catch (const std::system_error& error) {
        parent.close();
        child.close();
        parent.error_ = error.what();
    }
    return {std::move(parent), std::move(child)};
}

bool Channel::valid() const noexcept {
    if (!native_) return false;
    std::lock_guard lock(native_->mutex);
    return !native_->closing && native_->pipe != INVALID_HANDLE_VALUE;
}

void Channel::prevent_inheritance() {
    if (!native_) return;
    std::lock_guard lock(native_->mutex);
    if (native_->pipe != INVALID_HANDLE_VALUE && !::SetHandleInformation(native_->pipe, HANDLE_FLAG_INHERIT, 0))
        fail("SetHandleInformation IPC");
}

Channel::Transfer Channel::send_bytes(std::string_view bytes) {
    const auto result = native_->transfer(true, const_cast<char*>(bytes.data()),
        static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::numeric_limits<DWORD>::max())));
    if (result.error == ERROR_SUCCESS && result.bytes) return {Transfer::Kind::bytes, result.bytes, {}};
    if (result.error == ERROR_SUCCESS || result.error == ERROR_BROKEN_PIPE || result.error == ERROR_NO_DATA ||
        result.error == ERROR_PIPE_NOT_CONNECTED || result.error == ERROR_OPERATION_ABORTED)
        return {Transfer::Kind::closed, 0, {}};
    return {Transfer::Kind::failed, 0, std::system_category().message(static_cast<int>(result.error))};
}

Channel::Transfer Channel::receive_bytes(std::span<char> bytes) {
    const auto result = native_->transfer(false, bytes.data(),
        static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::numeric_limits<DWORD>::max())));
    if (result.error == ERROR_SUCCESS && result.bytes) return {Transfer::Kind::bytes, result.bytes, {}};
    if (result.error == ERROR_SUCCESS || result.error == ERROR_BROKEN_PIPE || result.error == ERROR_NO_DATA ||
        result.error == ERROR_PIPE_NOT_CONNECTED || result.error == ERROR_OPERATION_ABORTED)
        return {Transfer::Kind::closed, 0, {}};
    return {Transfer::Kind::failed, 0, std::system_category().message(static_cast<int>(result.error))};
}

void Channel::shutdown() { if (native_) native_->shutdown(); }
void Channel::close() { shutdown(); }

struct BackendProcess::State {
    Handle process;
    Handle job;
};

BackendProcess::BackendProcess() noexcept = default;
BackendProcess::BackendProcess(Channel connection, std::shared_ptr<const State> state)
    : channel(std::move(connection)), state_(std::move(state)) {}
BackendProcess::BackendProcess(BackendProcess&& other) noexcept
    : channel(std::move(other.channel)), state_(other.state_), reaped_(other.reaped_) {}
BackendProcess& BackendProcess::operator=(BackendProcess&& other) noexcept {
    channel = std::move(other.channel);
    state_ = other.state_;
    reaped_ = other.reaped_;
    return *this;
}
BackendProcess::~BackendProcess() { channel.close(); }

std::filesystem::path backend_binary_path() {
    std::error_code error;
    const auto self = base::executable_path(error);
    if (error) return {};
    const auto backend = self.parent_path() / L"dagent-backend.exe";
    if (!std::filesystem::exists(backend, error)) return {};
    return backend;
}

std::optional<BackendProcess> spawn_backend(std::string& error) {
    const auto binary = backend_binary_path();
    if (binary.empty()) {
        error = "dagent-backend was not found next to the frontend binary";
        return std::nullopt;
    }
    auto [parent, child] = Channel::create_pair();
    if (!parent.valid()) { error = parent.error(); return std::nullopt; }
    try {
        Handle input, output, errors;
        inherit_standard_handle(input, STD_INPUT_HANDLE, GENERIC_READ);
        inherit_standard_handle(output, STD_OUTPUT_HANDLE, GENERIC_WRITE);
        inherit_standard_handle(errors, STD_ERROR_HANDLE, GENERIC_WRITE);
        if (!::SetHandleInformation(child.native_->pipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            fail("SetHandleInformation child IPC");
        std::array<HANDLE, 4> handles{input.value, output.value, errors.value, child.native_->pipe};
        AttributeList attributes(handles);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value;
        startup.StartupInfo.hStdOutput = output.value;
        startup.StartupInfo.hStdError = errors.value;
        startup.lpAttributeList = attributes.value;
        auto state = std::make_shared<BackendProcess::State>();
        state->job.value = ::CreateJobObjectW(nullptr, nullptr);
        if (!state->job.value) fail("CreateJobObjectW backend");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(state->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            fail("SetInformationJobObject backend");
        std::wstring command = L"\"" + binary.native() + L"\" --ipc-fd " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(child.native_->pipe));
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP,
            nullptr, nullptr, &startup.StartupInfo, &process)) fail("CreateProcessW backend");
        state->process.value = process.hProcess;
        Handle thread(process.hThread);
        // 关闭父进程中的子端副本；这里不能 CancelIoEx 取消子进程将在同一管道端点上发起的 I/O。
        ::CloseHandle(std::exchange(child.native_->pipe, INVALID_HANDLE_VALUE));
        if (!::AssignProcessToJobObject(state->job.value, state->process.value)) {
            const DWORD code = ::GetLastError();
            ::TerminateProcess(state->process.value, 1);
            ::WaitForSingleObject(state->process.value, INFINITE);
            fail("AssignProcessToJobObject backend", code);
        }
        if (::ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
            const DWORD code = ::GetLastError();
            ::TerminateJobObject(state->job.value, 1);
            ::WaitForSingleObject(state->process.value, INFINITE);
            fail("ResumeThread backend", code);
        }
        return BackendProcess(std::move(parent), std::move(state));
    } catch (const std::system_error& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

bool wait_or_terminate(BackendProcess& process, std::chrono::milliseconds grace) {
    if (process.reaped_ || !process.state_) return true;
    process.channel.shutdown();
    const auto timeout = static_cast<DWORD>(std::clamp<std::int64_t>(grace.count(), 0, INFINITE - 1));
    DWORD result = ::WaitForSingleObject(process.state_->process.value, timeout);
    if (result == WAIT_OBJECT_0) { process.reaped_ = true; return true; }
    if (result != WAIT_TIMEOUT) return false;
    if (!::TerminateJobObject(process.state_->job.value, 1)) return false;
    result = ::WaitForSingleObject(process.state_->process.value, INFINITE);
    process.reaped_ = result == WAIT_OBJECT_0;
    return false; // 与 POSIX 强制终止分支一致：已回收，但未在宽限期正常结束。
}

} // namespace dagent::ipc
