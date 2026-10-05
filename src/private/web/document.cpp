#include "web/web.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <iconv.h>
#include <memory>
#include <regex>
#include "base/text.hpp"
#include <lexbor/html/html.h>
#include <lexbor/url/url.h>

namespace dagent::web {
namespace {

const lxb_char_t* bytes(std::string_view s) { return reinterpret_cast<const lxb_char_t*>(s.data()); }
struct UrlParser {
    lxb_url_parser_t parser{};
    UrlParser() {
        if (lxb_url_parser_init(&parser, nullptr) != LXB_STATUS_OK) throw std::bad_alloc();
    }
    ~UrlParser() { lxb_url_parser_memory_destroy(&parser); lxb_url_parser_destroy(&parser, false); }
};
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string attr(lxb_dom_node_t* node, const char* name) {
    size_t length = 0;
    const auto* value = lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
        bytes(name), std::char_traits<char>::length(name), &length);
    return value ? std::string(reinterpret_cast<const char*>(value), length) : "";
}
std::string text(lxb_dom_node_t* node) {
    size_t length = 0;
    auto* value = lxb_dom_node_text_content(node, &length);
    std::string result = value ? std::string(reinterpret_cast<char*>(value), length) : "";
    if (value) lxb_dom_document_destroy_text(node->owner_document, value);
    return result;
}
std::string utf8(std::string body, const std::string& type, bool html, bool truncated) {
    std::string charset;
    const std::regex pattern(R"(charset\s*=\s*["']?\s*([A-Za-z0-9._-]+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(type, match, pattern)) charset = lower(match[1]);
    else if (html) {
        const auto prefix = body.substr(0, 4096);
        if (std::regex_search(prefix, match, pattern)) charset = lower(match[1]);
    }
    if (body.starts_with("\xef\xbb\xbf")) { body.erase(0, 3); charset = "utf-8"; }
    else if (body.starts_with("\xff\xfe")) { body.erase(0, 2); charset = "utf-16le"; }
    else if (body.starts_with("\xfe\xff")) { body.erase(0, 2); charset = "utf-16be"; }
    if (charset.empty() || charset == "utf-8" || charset == "utf8") return base::to_valid_utf8(body);
    if (charset == "gb2312" || charset == "gbk") charset = "gb18030";
    iconv_t converter = iconv_open("UTF-8", charset.c_str());
    if (converter == reinterpret_cast<iconv_t>(-1)) throw std::runtime_error("unsupported page charset: " + charset);
    std::string out(body.size() * 4 + 16, '\0');
    char* input = body.data(); char* output = out.data();
    size_t remaining = body.size(), available = out.size();
    while (remaining) {
        if (iconv(converter, &input, &remaining, &output, &available) != size_t(-1)) break;
        if (errno == EINVAL && truncated) break;
        if (errno != EILSEQ && errno != EINVAL) {
            iconv_close(converter); throw std::runtime_error("page charset conversion failed");
        }
        *output++ = '?'; --available; ++input; --remaining;
    }
    iconv_close(converter);
    out.resize(output - out.data());
    return out;
}

class Markdown {
public:
    std::string output, base;
    explicit Markdown(std::string url) : base(std::move(url)) {}
    void newline(int count = 2) {
        while (!output.empty() && output.back() == ' ') output.pop_back();
        int n = 0;
        for (auto it = output.rbegin(); it != output.rend() && *it == '\n'; ++it) ++n;
        while (n++ < count && !output.empty()) output += '\n';
    }
    void inline_text(std::string_view value) {
        for (unsigned char c : value) {
            if (c < 128 && std::isspace(c)) {
                if (!output.empty() && output.back() != ' ' && output.back() != '\n') output += ' ';
            } else {
                if (c == '*' || c == '_' || c == '[' || c == ']' || c == '`' || c == '\\') output += '\\';
                output += c;
            }
        }
    }
    void children(lxb_dom_node_t* node, int depth) {
        for (auto* child = node->first_child; child; child = child->next) render(child, depth + 1);
    }
    void render(lxb_dom_node_t* node, int depth = 0) {
        if (depth > 256) return;
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) { inline_text(text(node)); return; }
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) { children(node, depth); return; }
        const auto tag = node->local_name;
        if (tag == LXB_TAG_SCRIPT || tag == LXB_TAG_STYLE || tag == LXB_TAG_TEMPLATE ||
            tag == LXB_TAG_NOSCRIPT || tag == LXB_TAG_SVG || tag == LXB_TAG_HEAD) return;
        if (lxb_dom_element_has_attribute(lxb_dom_interface_element(node), bytes("hidden"), 6) ||
            attr(node, "aria-hidden") == "true") return;
        if (tag == LXB_TAG_PRE) {
            newline(); const auto code = text(node);
            std::string fence = "```";
            while (code.find(fence) != std::string::npos) fence += '`';
            output += fence + "\n" + code + "\n" + fence; newline(); return;
        }
        if (tag == LXB_TAG_CODE) { output += '`'; inline_text(text(node)); output += '`'; return; }
        if (tag == LXB_TAG_BR) { newline(1); return; }
        if (tag == LXB_TAG_HR) { newline(); output += "---"; newline(); return; }
        if (tag == LXB_TAG_IMG) { inline_text(attr(node, "alt")); return; }
        if (tag == LXB_TAG_A) {
            std::string url;
            try { url = normalize_url(attr(node, "href"), base); } catch (const std::exception&) {}
            if (!url.empty()) output += '[';
            children(node, depth);
            if (!url.empty()) output += "](<" + url + ">)";
            return;
        }
        const bool heading = tag >= LXB_TAG_H1 && tag <= LXB_TAG_H6;
        const bool block = heading || tag == LXB_TAG_P || tag == LXB_TAG_DIV || tag == LXB_TAG_SECTION ||
            tag == LXB_TAG_ARTICLE || tag == LXB_TAG_UL || tag == LXB_TAG_OL || tag == LXB_TAG_BLOCKQUOTE ||
            tag == LXB_TAG_TABLE || tag == LXB_TAG_HEADER || tag == LXB_TAG_FOOTER;
        if (block) newline();
        if (heading) output += std::string(tag - LXB_TAG_H1 + 1, '#') + ' ';
        if (tag == LXB_TAG_LI) { newline(1); output += "- "; }
        if (tag == LXB_TAG_TR) newline(1);
        if (tag == LXB_TAG_TD || tag == LXB_TAG_TH) output += " | ";
        if (tag == LXB_TAG_BLOCKQUOTE) output += "> ";
        const std::string emphasis = tag == LXB_TAG_STRONG || tag == LXB_TAG_B ? "**" :
            tag == LXB_TAG_EM || tag == LXB_TAG_I ? "*" : "";
        output += emphasis; children(node, depth); output += emphasis;
        if (block) newline();
    }
};

} // namespace

std::string normalize_url(std::string_view url, std::string_view base) {
    UrlParser parser;
    auto* base_url = base.empty() ? nullptr : lxb_url_parse(&parser.parser, nullptr, bytes(base), base.size());
    auto* parsed = lxb_url_parse(&parser.parser, base_url, bytes(url), url.size());
    if (!parsed || parsed->username.length || parsed->password.length)
        throw std::runtime_error("expected an HTTP(S) URL without credentials");
    std::string result;
    const auto status = lxb_url_serialize(parsed, [](const lxb_char_t* data, size_t length, void* context) -> lxb_status_t {
        static_cast<std::string*>(context)->append(reinterpret_cast<const char*>(data), length);
        return LXB_STATUS_OK;
    }, &result, false);
    if (status != LXB_STATUS_OK || (!result.starts_with("https://") && !result.starts_with("http://")))
        throw std::runtime_error("only HTTP and HTTPS URLs are supported");
    return result;
}

Page decode(Response response) {
    Page page{response.url, {}, response.content_type, {}, response.status, response.truncated};
    const auto type = lower(response.content_type.substr(0, response.content_type.find(';')));
    const bool html = type == "text/html" || type == "application/xhtml+xml";
    if (!html && !type.starts_with("text/") && type != "application/json" && !type.ends_with("+json") &&
        type != "application/xml" && !type.ends_with("+xml"))
        throw std::runtime_error("unsupported content type: " + response.content_type + "; only HTML, text and JSON are supported");
    auto content = utf8(std::move(response.body), response.content_type, html, response.truncated);
    if (!html) { page.text = std::move(content); return page; }
    auto document = std::unique_ptr<lxb_html_document_t, decltype(&lxb_html_document_destroy)>(
        lxb_html_document_create(), lxb_html_document_destroy);
    if (!document || lxb_html_document_parse(document.get(), bytes(content), content.size()) != LXB_STATUS_OK)
        throw std::runtime_error("HTML parser failed");
    Markdown markdown(response.url);
    // Head is not rendered; use title and the first valid base URL as metadata.
    auto* head = lxb_dom_interface_node(document->head);
    if (head) for (auto* node = head->first_child; node; node = node->next) {
        if (node->local_name == LXB_TAG_TITLE) page.title = text(node);
        if (node->local_name == LXB_TAG_BASE && markdown.base == response.url) {
            try { markdown.base = normalize_url(attr(node, "href"), response.url); }
            catch (const std::exception&) {}
        }
    }
    auto* body = lxb_dom_interface_node(lxb_html_document_body_element(document.get()));
    if (body) markdown.render(body);
    page.text = std::move(markdown.output);
    return page;
}

} // namespace dagent::web
