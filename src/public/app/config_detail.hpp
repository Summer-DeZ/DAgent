/// @file config_detail.hpp
/// @brief app 配置实现共享的内部原语；不作为装配接口。
#pragma once

#include <cstdint>
#include <format>
#include <memory>
#include <string_view>
#include <utility>

#include "app/config.hpp"

namespace dagent::app::detail {

using json = nlohmann::json;

std::shared_ptr<spdlog::logger> log_app();
[[noreturn]] void fail(ConfigError::Kind kind, const std::string& what);
std::filesystem::path absolute_under(const std::filesystem::path& base, const std::filesystem::path& path);

class Node {
public:
    Node() = default;
    Node(const json& value, std::string pointer) : value_(&value), pointer_(std::move(pointer)) {}

    bool exists() const { return value_ != nullptr; }
    bool has() const { return value_ != nullptr && !value_->is_null(); }
    Node child(std::string_view key) const {
        if (!exists()) return {};
        if (!value_->is_object()) wrong("an object");
        const auto it = value_->find(key);
        return it == value_->end() ? Node{} : Node(*it, pointer_ + "/" + std::string(key));
    }
    Node required(std::string_view key) const {
        const Node value = child(key);
        if (!value.has()) fail(ConfigError::Kind::invalid, pointer_ + "/" + std::string(key) + " is required");
        return value;
    }
    const json& raw() const { return *value_; }
    const std::string& pointer() const { return pointer_; }
    std::string str(std::string fallback = {}) const {
        if (!has()) return fallback;
        if (!value_->is_string()) wrong("a string");
        return value_->get<std::string>();
    }
    bool flag(bool fallback = false) const {
        if (!has()) return fallback;
        if (!value_->is_boolean()) wrong("a boolean");
        return value_->get<bool>();
    }
    int integer(int fallback = 0) const {
        if (!has()) return fallback;
        if (!value_->is_number_integer() && !value_->is_number_unsigned()) wrong("an integer");
        try { return value_->get<int>(); }
        catch (const json::exception&) { wrong("a 32-bit integer"); }
    }
    std::size_t usize(std::size_t fallback = 0) const {
        if (!has()) return fallback;
        if (value_->is_number_unsigned()) return value_->get<std::size_t>();
        if (value_->is_number_integer()) {
            const auto n = value_->get<std::int64_t>();
            if (n >= 0) return static_cast<std::size_t>(n);
        }
        wrong("a non-negative integer");
    }
    double real(double fallback = 0) const {
        if (!has()) return fallback;
        if (!value_->is_number()) wrong("a number");
        return value_->get<double>();
    }
    std::vector<std::string> strings() const {
        if (!has()) return {};
        if (!value_->is_array()) wrong("an array of strings");
        std::vector<std::string> out;
        for (const auto& item : *value_) {
            if (!item.is_string()) wrong("an array of strings");
            out.push_back(item.get<std::string>());
        }
        return out;
    }

private:
    [[noreturn]] void wrong(std::string_view expected) const {
        fail(ConfigError::Kind::type, std::format("{} must be {}", pointer_, expected));
    }
    const json* value_ = nullptr;
    std::string pointer_;
};

std::size_t size_option(const Node& node, std::string_view key);
json parse_file(const std::filesystem::path& file);
void require_private_file(const std::filesystem::path& file);
std::string env_value(std::string_view name, const std::string& where);
std::string expand_env(const std::string& text, const std::string& where);

/// 模型与外观保存共用原有 models.lock，包围重读、校验和原子写。
/// flock 随进程退出释放，不留下需要清理的锁状态。
class ModelWriteLock {
public:
    explicit ModelWriteLock(const std::filesystem::path& root);
    ~ModelWriteLock();
    ModelWriteLock(const ModelWriteLock&) = delete;
    ModelWriteLock& operator=(const ModelWriteLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Parse with ordered_json, then patch only changed values into the original text.
// Unowned objects, comments, inline arrays and whitespace remain byte-for-byte intact.
struct ConfigDocument {
    using Ordered = nlohmann::ordered_json;
    std::string source, indent = "  ", newline = "\n";
    Ordered original, value;
    explicit ConfigDocument(const std::filesystem::path& file);
    void trivia(std::size_t& p) const;
    std::size_t token_end(std::size_t p) const;
    std::string encoded(const Ordered& item, const std::string& padding, bool multiline) const;
    std::string patch(const Ordered& before, const Ordered& after, std::size_t begin, std::size_t end) const;
    std::string dump() const;
};

} // namespace dagent::app::detail
