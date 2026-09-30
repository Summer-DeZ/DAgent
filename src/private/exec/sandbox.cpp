#include "exec/sandbox.hpp"

#include <system_error>

#include "exec/process.hpp"

namespace dagent::exec {
namespace {

namespace fs = std::filesystem;

std::filesystem::path normalized(const fs::path& path) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(path, ec);
    if (ec) result = fs::absolute(path, ec).lexically_normal();
    return result;
}

bool sensitive_name(std::string_view name) {
    return name == ".env" || name.starts_with(".env.") || name.ends_with(".pem") ||
           name.ends_with(".key") || name.starts_with("id_rsa") || name.starts_with("id_ed25519");
}

} // namespace

std::vector<fs::path> sensitive_paths(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(status)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        const std::string name = it->path().filename().string();
        const bool protected_dir = fs::is_directory(status) && (name == ".ssh" || name == ".gnupg");
        if (protected_dir || sensitive_name(name)) out.push_back(normalized(it->path()));
        if (protected_dir) it.disable_recursion_pending();
    }
    if (ec) throw ExecError{ExecError::Kind::sandbox,
                            "scan sensitive paths under " + root.string() + ": " + ec.message()};
    return out;
}

bool Support::read_only_ready() const { return backend != "none"; }

bool Support::workspace_ready() const { return backend != "none"; }

} // namespace dagent::exec
