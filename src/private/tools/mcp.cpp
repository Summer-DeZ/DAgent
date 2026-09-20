#include <chrono>
#include <format>
#include <utility>

#include "base/log.hpp"
#include "base/text.hpp"
#include "tools/detail.hpp"

namespace dagent::tools {
using detail::error_result;

namespace {

/// 把 MCP 的内容块转成给模型的占位文本：文本块拼接，图片/音频占位，resource 给 uri。
std::string blocks_to_text(const std::vector<nlohmann::json>& content) {
    std::string text;
    const auto append = [&](std::string piece) {
        if (!text.empty()) text += '\n';
        text += std::move(piece);
    };
    for (const nlohmann::json& block : content) {
        if (!block.is_object()) continue;
        const std::string type = block.value("type", "");
        if (type == "text") {
            append(base::to_valid_utf8(block.value("text", "")));
        } else if (type == "image" || type == "audio") {
            std::size_t bytes = 0;
            if (const auto data = block.find("data"); data != block.end() && data->is_string()) {
                try {
                    bytes = base::base64_decode(data->get<std::string>()).size();
                } catch (const std::invalid_argument&) {
                    bytes = data->get<std::string>().size();
                }
            }
            append(std::format("[{} {}, {} bytes, not displayed]", type == "image" ? "image" : "audio",
                               block.value("mimeType", "application/octet-stream"), bytes));
        } else if (type == "resource") {
            const nlohmann::json& resource = block.contains("resource") ? block.at("resource") : block;
            std::string piece = std::format("Resource {}", resource.value("uri", "(no URI)"));
            if (const auto it = resource.find("text"); it != resource.end() && it->is_string())
                piece += ": " + base::to_valid_utf8(it->get<std::string>());
            else if (const auto blob = resource.find("blob"); blob != resource.end() && blob->is_string())
                piece += std::format(" (binary content: {} bytes, not displayed)", blob->get<std::string>().size());
            append(std::move(piece));
        } else if (type == "resource_link") {
            append(std::format("Resource link {} ({})", block.value("name", ""), block.value("uri", "")));
        } else {
            append(std::format("[unknown content block {}]", type.empty() ? "(no type)" : type));
        }
    }
    return text;
}

class McpCall final : public Call {
public:
    McpCall(mcp::Client& client, const mcp::Tool& tool, nlohmann::json args,
            std::chrono::milliseconds timeout, std::size_t max_result_bytes)
        : client_(client), tool_(tool), args_(std::move(args)), timeout_(timeout),
          max_result_bytes_(max_result_bytes) {
        intent_.kind = Intent::Kind::external;
        intent_.summary = std::format("Call MCP tool {}", tool_.qualified_name);
    }

private:
    Result do_run(const Grant&, const std::function<void(std::string_view)>&, std::stop_token stop) override {
        mcp::CallResult outcome;
        try {
            outcome = client_.call(tool_.qualified_name, args_, timeout_, stop);
        } catch (const mcp::McpError& e) {
            if (e.kind() == mcp::McpError::Kind::cancelled) {
                Result result;
                result.text = "interrupted by the user";
                result.interrupted = true;
                McpView view;
                view.server = tool_.server;
                view.tool = tool_.qualified_name;
                view.disconnected = false;
                result.display = std::move(view);
                return result;
            }
            base::logger("tools")->warn("MCP tool {} failed: {}", tool_.qualified_name, e.what());
            McpView view;
            view.server = tool_.server;
            view.tool = tool_.qualified_name;
            view.disconnected = e.kind() == mcp::McpError::Kind::disconnected;
            return error_result(std::format("MCP tool call failed ({}): {}", tool_.qualified_name, e.what()),
                                std::move(view));
        }

        McpView view;
        view.server = tool_.server;
        view.tool = tool_.qualified_name;
        view.content = outcome.content;
        view.structured = outcome.structured;
        view.disconnected = false;

        std::string text = blocks_to_text(outcome.content);
        if (text.empty() && !outcome.structured.is_null())
            text = outcome.structured.dump(); // 只给结构化结果的 server
        if (text.empty()) text = "(no result)";
        text = base::truncate_middle(base::to_valid_utf8(std::move(text)), max_result_bytes_).text;

        Result result;
        result.text = std::move(text);
        result.is_error = outcome.is_error;
        result.display = std::move(view);
        return result;
    }

    mcp::Client& client_;
    mcp::Tool tool_;
    nlohmann::json args_;
    std::chrono::milliseconds timeout_;
    std::size_t max_result_bytes_ = 0;
};

class McpTool final : public Tool {
public:
    McpTool(mcp::Client& client, const mcp::Tool& tool)
        : client_(client),
          spec_([ & ] {
              Spec spec;
              spec.name = tool.qualified_name;
              spec.description = tool.description.empty()
                                     ? std::format("MCP server {} provides tool {}", tool.server, tool.name)
                                     : tool.description;
              spec.parameters = tool.input_schema.is_object() ? tool.input_schema
                                                              : nlohmann::json::object();
              return spec;
          }()),
          tool_(tool) {}

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                         Context& ctx) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(error_result(args.error()));
        return std::make_unique<McpCall>(client_, tool_, std::move(*args), ctx.options().mcp_call_timeout,
                                         ctx.options().max_result_bytes);
    }

private:
    mcp::Client& client_;
    Spec spec_;
    mcp::Tool tool_;
};

} // namespace

std::unique_ptr<Tool> detail::make_mcp_tool(mcp::Client& client, const mcp::Tool& tool) {
    return std::make_unique<McpTool>(client, tool);
}

} // namespace dagent::tools
