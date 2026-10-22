#include "ui/overlay/model_picker.hpp"
#include "base/platform.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <utility>
#include "ui/style/appearance.hpp"
#include "ui/browser.hpp"
#include "ui/style/display.hpp"
#include "ui/style/strings.hpp"

namespace dagent::ui {
namespace {
using json = nlohmann::json;
using namespace std::chrono_literals;
std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string expiry(const json& status) {
    const auto raw = static_cast<std::time_t>(status.value("expires_at", std::int64_t{0}));
    if (!raw) return std::string(text().model_unknown);
    std::tm local{};
    base::local_time(raw, local);
    return format_text(text().welcome_date, text().welcome_months.substr(local.tm_mon * 3, 3), local.tm_mday);
}
std::string age(std::int64_t seconds) {
    if (seconds < 10) return std::string(text().provider_age_now);
    if (seconds < 60) return format_text(text().provider_age_seconds, seconds);
    if (seconds < 3600) return format_text(text().provider_age_minutes, seconds / 60);
    if (seconds < 86400) return format_text(text().provider_age_hours, seconds / 3600);
    return format_text(text().provider_age_days, seconds / 86400);
}
std::string context_size(std::size_t tokens) {
    if (tokens < 1'000'000) return std::format("{:.0f}k", tokens / 1000.0);
    auto out = std::format("{:.2f}", tokens / 1'000'000.0);
    while (out.back() == '0') out.pop_back();
    if (out.back() == '.') out.pop_back();
    return out + "M";
}
std::string output_limit(std::size_t tokens) {
    auto out = std::to_string(tokens);
    for (auto pos = out.size(); pos > 3; pos -= 3) out.insert(pos - 3, ",");
    return out;
}
std::string price(const json& model, bool detail = false) {
    const auto cost = model.value("cost", json::object());
    if (!cost.contains("input") || !cost["input"].is_number() ||
        !cost.contains("output") || !cost["output"].is_number()) return {};
    const double input = cost["input"].get<double>(), output = cost["output"].get<double>();
    if (input == 0 && output == 0) return std::string(text().model_free);
    auto money = [](double value) {
        auto out = std::format("{:.6f}", value);
        while (out.size() > out.find('.') + 3 && out.back() == '0') out.pop_back();
        return out;
    };
    auto out = format_text(text().model_price_pair, money(input), money(output));
    return detail ? format_text(text().model_price_detail, out) : out;
}
// Only show host/path; userinfo, query and fragment never belong in the details pane.
std::string endpoint(std::string url, bool host_only) {
    if (const auto scheme = url.find("://"); scheme != std::string::npos) url.erase(0, scheme + 3);
    if (const auto end = url.find_first_of("?#"); end != std::string::npos) url.resize(end);
    const auto slash = url.find('/');
    if (const auto user = url.substr(0, slash).rfind('@'); user != std::string::npos) url.erase(0, user + 1);
    if (host_only) if (const auto path = url.find('/'); path != std::string::npos) url.resize(path);
    return url;
}
}

class ModelPicker::LoginView final : public tui::Widget {
public:
    explicit LoginView(ModelPicker& owner) : owner_(owner) {}
    tui::Size measure(tui::Size available) const override {
        return {std::max(1, std::min(88, available.cols - 4)), std::max(1, std::min(14, available.rows - 2))};
    }
    void render(tui::Surface& surface) override {
        auto& o = owner_;
        const int w = surface.cols(), h = surface.rows();
        const auto& t = o.theme_;
        surface.fill({0, 0, w, h}, U' ', t.background_panel);
        if (w < 4 || h < 3) return;
        auto tone = [&](tui::Style style) { style.bg = t.background_panel.bg; return style; };
        draw_frame(surface, {0, 0, w, h}, tone(t.border_active));
        surface.text(2, 0, fit_columns(" " + std::string(text().login_title) + " ", w - 4), tone(t.text));
        auto body = surface.view({2, 1, w - 4, h - 2});
        int y = body.rows() >= 9 ? 1 : 0;
        auto line = [&](std::string_view value, tui::Style style) {
            body.text(0, y++, fit_columns(value, body.cols()), tone(style));
        };
        if (o.url_.empty()) {
            line(o.error_.empty() ? text().login_starting : std::string_view(o.error_), o.error_.empty() ? t.text_muted : t.error);
        } else {
            line(o.browser_opened_ ? text().login_opened : text().login_open_link, t.text);
            line(o.url_, t.primary);
            if (body.rows() >= 9) ++y;
            body.text(0, y, text().login_code, tone(t.text_muted));
            const int x = display_width(text().login_code) + 2;
            draw_chip(body, x, y++, fit_columns(o.code_, body.cols() - x - 2), t.primary, t, o.outline_, t.background_panel.bg);
            if (body.rows() >= 9) ++y;
            if (o.error_.empty()) {
                const auto remaining = std::max(std::int64_t{0}, o.expires_at_ - now_seconds());
                line(std::string(icons().spinner[o.frame_]) + " " +
                     format_text(text().login_waiting, remaining / 60, remaining % 60), t.accent);
            } else line(o.error_, t.error);
            if (body.rows() >= 9) ++y;
            line(text().login_subscription, t.text_muted);
        }
        const auto hint = fit_columns(" " + std::string(o.error_.empty() ? text().login_footer : text().login_retry_footer) + " ", w - 4);
        draw_hint(surface, (w - display_width(hint)) / 2, h - 1, hint, tone(t.text_muted), t.text_faint);
    }
private:
    ModelPicker& owner_;
};

ModelPicker::ModelPicker(tui::Runtime& rt, Actions actions) : rt_(rt), actions_(std::move(actions)), panel_(rt) {}
ModelPicker::~ModelPicker() { close(); }
bool ModelPicker::active() const noexcept { return page_ != Page::closed; }
void ModelPicker::set_theme(const tui::ThemeTokens& theme, bool outline) {
    theme_ = theme; outline_ = outline; panel_.set_theme(theme);
    if (login_view_) login_view_->invalidate();
}
void ModelPicker::cancel_login(const std::string& id) {
    if (!id.empty()) actions_.request("provider.login.cancel", {{"provider", "opencode"}, {"login_id", id}}, {}, {});
}
void ModelPicker::dismiss() {
    ++epoch_;
    panel_.close(true);
    rt_.cancel(timer_); timer_ = 0;
    if (overlay_) rt_.close_overlay(overlay_);
    overlay_ = 0; login_view_ = nullptr;
    pending_ = false;
}
void ModelPicker::close() {
    cancel_login(std::exchange(login_id_, {}));
    dismiss(); page_ = Page::closed;
}
void ModelPicker::open(const std::vector<protocol::PublicModel>& models, std::string current) {
    close(); models_ = models; current_ = std::move(current);
    status_ = nullptr; status_error_.clear();
    show_picker(); load_status();
}
void ModelPicker::update_models(const std::vector<protocol::PublicModel>& models, std::string current) {
    models_ = models; current_ = std::move(current);
    if (page_ == Page::picker) panel_.update_rows(picker_rows());
    if (page_ == Page::catalog && !pending_ && error_.empty() && !catalog_.is_null()) catalog_rows();
}
bool ModelPicker::signed_in() const {
    return status_.is_object() && status_.value("state", "") == "signed_in" &&
        status_.value("expires_at", std::int64_t{0}) > now_seconds();
}
std::string ModelPicker::status_text() const {
    if (!status_error_.empty()) return status_error_;
    if (status_.is_null()) return std::string(text().model_loading);
    if (signed_in()) return format_text(text().model_signed_in, status_.value("workspace", ""));
    return std::string(status_.value("state", "") == "signed_out" ? text().model_not_signed_in : text().model_expired);
}
void ModelPicker::load_status() {
    const auto epoch = epoch_;
    actions_.request("provider.status", {{"provider", "opencode"}}, [this, epoch](const json& value) {
        if (epoch != epoch_) return;
        status_ = value; status_error_.clear(); panel_.update_rows(picker_rows());
    }, [this, epoch](const protocol::RpcError& error) {
        if (epoch != epoch_) return;
        status_error_ = error.message; panel_.update_rows(picker_rows());
    });
}
std::vector<Panel::Row> ModelPicker::picker_rows() {
    std::vector<Panel::Row> rows;
    for (const auto& model : models_) {
        const bool login = model.auth == "login:opencode";
        const bool needs = login && !status_.is_null() && !signed_in();
        const auto tag = needs ? text().model_needs_sign_in : model.name == current_ ? text().panel_current :
            login ? (status_.is_null() ? text().model_loading : text().model_login) :
            model.auth == "key" ? text().model_key : text().model_local;
        rows.push_back({model.name, {}, std::string(tag), true,
            [this, name = model.name] { close(); actions_.select(name); }, std::string(text().model_configured), {},
            Panel::Tone::normal, needs ? Panel::Tone::warning : Panel::Tone::muted});
    }
    std::string tag;
    auto tone = Panel::Tone::muted;
    if (!status_error_.empty()) { tag = status_error_; tone = Panel::Tone::error; }
    else if (status_.is_null()) tag = text().model_loading;
    else if (signed_in()) tag = catalog_.is_null() ? std::string(icons().chevron) :
        format_text(text().model_provider_count, catalog_["models"].size(), icons().chevron);
    else if (status_.value("state", "") != "signed_out") { tag = text().model_expired; tone = Panel::Tone::warning; }
    else tag = text().model_sign_in;
    rows.push_back({std::string(text().model_provider), {}, tag, true, [this] { provider(); },
        std::string(text().model_providers), {}, Panel::Tone::normal, tone});
    rows.push_back({std::string(text().model_add_custom), {}, {}, true, [this] { custom(); }});
    return rows;
}
void ModelPicker::show_picker() {
    cancel_login(std::exchange(login_id_, {}));
    dismiss(); page_ = Page::picker; error_.clear();
    int initial = 0;
    for (int i = 0; i < static_cast<int>(models_.size()); ++i) if (models_[i].name == current_) initial = i;
    panel_.open(std::string(text().panel_model), picker_rows(), format_text(text().panel_model_footer, icons().right), true, {},
        [this](bool committed) { if (!committed) close(); }, initial, [this] { custom(); });
    Panel::Presentation presentation;
    presentation.width = 100; presentation.height = 24;
    presentation.close_on_activate = false;
    presentation.details = [this](tui::Surface& surface, int index) { details(surface, index); };
    presentation.open_right = [this](int index) { if (index == static_cast<int>(models_.size())) provider(); };
    panel_.present(std::move(presentation));
}
void ModelPicker::provider() {
    if (status_.is_null()) {
        if (!status_error_.empty()) { status_error_.clear(); panel_.update_rows(picker_rows()); load_status(); }
        return;
    }
    if (signed_in()) show_catalog(); else start_login();
}
void ModelPicker::custom() { close(); actions_.custom(); }

void ModelPicker::details(tui::Surface& surface, int index) {
    const auto& t = theme_;
    int y = 0;
    auto line = [&](std::string_view value, tui::Style style) {
        style.bg = t.background_panel.bg;
        surface.text(0, y++, fit_columns(value, surface.cols()), style);
    };
    auto pair = [&](std::string_view key, std::string_view value) {
        auto muted = t.text_muted; muted.bg = t.background_panel.bg;
        auto normal = t.text; normal.bg = t.background_panel.bg;
        surface.text(0, y, key, muted);
        surface.text(11, y++, fit_columns(value, surface.cols() - 11), normal);
    };
    auto bold = t.text; bold.attrs = bold.attrs | tui::Attr::bold;
    if (index < 0) return;
    if (index > static_cast<int>(models_.size())) { line(text().model_custom_help, t.text_muted); return; }
    if (index == static_cast<int>(models_.size())) {
        line(text().model_provider, bold); ++y;
        line(status_text(), !status_error_.empty() ? t.error : signed_in() ? t.success : t.warning);
        if (signed_in()) {
            pair(text().model_workspace, status_.value("workspace", ""));
            pair(text().model_expires, expiry(status_));
        }
        ++y; line("https://opencode.ai/go", t.primary); ++y;
        line(signed_in() ? text().model_provider_open : text().model_provider_authorize, t.text_muted);
        return;
    }
    const auto& model = models_[index];
    const bool login = model.auth == "login:opencode";
    line(model.name, bold);
    line(login ? std::string(text().model_via) : endpoint(model.base_url, true), t.text_muted); ++y;
    pair(text().model_kind, model.kind);
    pair(text().model_endpoint, endpoint(model.base_url, false));
    pair(text().model_context, context_size(model.context_window));
    pair(text().model_output, output_limit(model.max_tokens));
    if (model.kind == "openai-chat" || model.kind == "ollama")
        pair(text().model_reasoning, model.send_reasoning_content ? text().model_on : text().model_off);
    pair(text().model_auth, login ? text().model_login_auth : model.auth == "key" ? text().model_key : text().model_none);
    if (!catalog_.is_null()) for (const auto& item : catalog_["models"]) {
        if (item["id"] != model.name) continue;
        const auto cost = price(item, true);
        if (!cost.empty()) pair(text().model_price, cost);
        break;
    }
    if (login) {
        ++y; line(text().model_sign_in_section, t.text_muted);
        if (status_.is_null()) line(status_text(), status_error_.empty() ? t.text_muted : t.error);
        else if (signed_in()) {
            line(std::string(icons().dot) + " " + status_text(), t.success);
            line(format_text(text().model_expiry, expiry(status_)), t.text_muted);
        } else {
            line(std::string(icons().circle) + " " + std::string(text().model_not_signed_in), t.warning);
            line(text().model_sign_in_help, t.text_muted);
        }
    }
    if (y < surface.rows()) { y = surface.rows() - 1; line(text().model_saved, t.text_muted); }
}

void ModelPicker::start_login() {
    cancel_login(std::exchange(login_id_, {}));
    dismiss(); page_ = Page::login; error_.clear(); url_.clear(); code_.clear();
    expires_at_ = 0; browser_opened_ = false;
    auto view = std::make_unique<LoginView>(*this); login_view_ = view.get();
    overlay_ = rt_.open_overlay(std::move(view), tui::Placement::center, {}, this);
    const auto epoch = epoch_;
    actions_.request("provider.login.start", {{"provider", "opencode"}}, [this, epoch](const json& value) {
        const auto id = value.at("login_id").get<std::string>();
        // Escape may arrive while start is still in flight; cancel its result as well.
        if (epoch != epoch_) { cancel_login(id); return; }
        login_id_ = id; url_ = value.at("verification_url").get<std::string>();
        code_ = value.at("user_code").get<std::string>(); expires_at_ = value.at("expires_at").get<std::int64_t>();
        browser_opened_ = open_browser(url_); login_view_->invalidate();
        timer_ = rt_.every(100ms, [this] {
            frame_ = (frame_ + 1) % icons().spinner.size();
            if (now_seconds() >= expires_at_) {
                cancel_login(std::exchange(login_id_, {}));
                login_error(std::string(text().login_expired)); return false;
            }
            login_view_->invalidate(); return true;
        });
        actions_.request("provider.login.wait", {{"provider", "opencode"}, {"login_id", id}}, [this, epoch](const json& result) {
            if (epoch != epoch_) return;
            login_id_.clear(); status_ = result; status_error_.clear();
            actions_.toast(ToastStack::Severity::success, std::string(text().login_success));
            show_catalog();
        }, [this, epoch](const protocol::RpcError& error) {
            if (epoch != epoch_ || !error_.empty()) return;
            login_id_.clear(); login_error(error.message);
        });
    }, [this, epoch](const protocol::RpcError& error) {
        if (epoch == epoch_) login_error(error.message);
    });
}
void ModelPicker::login_error(std::string error) {
    error_ = std::move(error); rt_.cancel(timer_); timer_ = 0;
    if (login_view_) login_view_->invalidate();
}
bool ModelPicker::on_event(const tui::Event& event) {
    if (page_ != Page::login) return false;
    if (event.key == tui::Key::escape || (tui::any(event.mods & tui::Mods::ctrl) && event.text == "c")) {
        show_picker(); load_status(); return true;
    }
    if (!error_.empty() && event.key == tui::Key::enter) { start_login(); return true; }
    if (event.kind == tui::Event::Kind::text && event.mods == tui::Mods::none && !url_.empty()) {
        if (event.text == "c") {
            const bool copied = rt_.set_clipboard(url_);
            actions_.toast(copied ? ToastStack::Severity::success : ToastStack::Severity::warning,
                std::string(copied ? text().login_copied : text().login_copy_failed));
        } else if (event.text == "o") { browser_opened_ = open_browser(url_); login_view_->invalidate(); }
    }
    return true;
}

void ModelPicker::show_catalog(bool refresh) {
    dismiss(); page_ = Page::catalog; pending_ = true; error_.clear();
    panel_.open(std::string(text().model_provider), {{std::string(text().panel_loading), {}, {}, false, {}}},
        std::string(text().provider_list_footer), true, {},
        [this](bool committed) { if (!committed) { show_picker(); load_status(); } });
    Panel::Presentation presentation;
    presentation.width = 100; presentation.height = 24; presentation.close_on_activate = false;
    presentation.status = format_text(text().provider_list_status, status_.value("workspace", ""), expiry(status_));
    presentation.refresh = [this] { if (!pending_) show_catalog(true); };
    panel_.present(std::move(presentation));
    const auto epoch = epoch_;
    actions_.request("provider.models.list", {{"provider", "opencode"}, {"refresh", refresh}}, [this, epoch](const json& value) {
        if (epoch != epoch_) return;
        pending_ = false; catalog_ = value; catalog_received_ = now_seconds(); catalog_rows();
    }, [this, epoch](const protocol::RpcError& error) {
        if (epoch != epoch_) return;
        pending_ = false; error_ = error.message;
        panel_.update_rows({{error_, {}, {}, false, {}, {}, {}, Panel::Tone::error}}, true);
    });
}
void ModelPicker::catalog_rows() {
    std::vector<Panel::Row> rows;
    int context_width = 0;
    for (const auto& item : catalog_["models"])
        context_width = std::max(context_width, display_width(context_size(item.at("context_window").get<std::size_t>())));
    for (const auto& item : catalog_["models"]) {
        const auto id = item.at("id").get<std::string>();
        const bool available = item.at("available").get<bool>();
        const bool added = item.value("added", false) || std::ranges::any_of(models_, [&](const auto& m) { return m.name == id; });
        const auto cost = price(item);
        const auto context = context_size(item.at("context_window").get<std::size_t>());
        rows.push_back({item.at("model").get<std::string>(),
            std::string(context_width - display_width(context), ' ') + context + "  " +
                (cost.empty() ? std::string(text().model_unknown) : cost),
            !available ? std::string(text().provider_unavailable) : added ? std::string(text().provider_added) : "",
            available, [this, id] { add_provider_model(id); }, {}, {}, Panel::Tone::normal, Panel::Tone::muted,
            item.at("name").get<std::string>()});
    }
    if (rows.empty()) rows.push_back({std::string(text().provider_empty), {}, {}, false, {}});
    panel_.update_rows(std::move(rows));
    Panel::Presentation presentation;
    presentation.width = 100; presentation.height = 24; presentation.close_on_activate = false;
    presentation.status = format_text(text().provider_list_status, status_.value("workspace", ""), expiry(status_));
    presentation.footer = format_text(text().provider_list_source,
        age(catalog_.value("cache_age_seconds", std::int64_t{0}) + now_seconds() - catalog_received_),
        catalog_.value("stale", false) ? text().provider_stale : std::string_view{});
    presentation.align_columns = true;
    presentation.refresh = [this] { if (!pending_) show_catalog(true); };
    panel_.present(std::move(presentation));
}
void ModelPicker::add_provider_model(const std::string& id) {
    if (pending_) return;
    pending_ = true;
    auto params = actions_.target(); params["provider"] = "opencode"; params["model_id"] = id;
    const auto epoch = epoch_;
    panel_.update_rows({{std::string(text().act_adding_model), {}, {}, false, {}}}, true);
    actions_.request("provider.model.add", std::move(params), [this, epoch, id](const json& value) {
        // A completed command still updates the session if the user already went back.
        actions_.changed();
        const bool selected = value.value("selected", false);
        if (selected) {
            actions_.toast(ToastStack::Severity::success, format_text(text().provider_switch, id));
            if (epoch == epoch_) close();
        } else {
            const auto it = value.find("selection_error");
            const auto message = it != value.end() && it->is_string() ? it->get<std::string>() : std::string(text().provider_switch_failed);
            if (epoch == epoch_) {
                pending_ = false; error_ = message;
                panel_.update_rows({{message, {}, {}, false, {}, {}, {}, Panel::Tone::error}}, true);
            } else actions_.toast(ToastStack::Severity::error, message);
        }
    }, [this, epoch](const protocol::RpcError& error) {
        if (epoch != epoch_) return;
        pending_ = false; error_ = error.message;
        panel_.update_rows({{error_, {}, {}, false, {}, {}, {}, Panel::Tone::error}}, true);
    });
}
std::vector<KeyHint> ModelPicker::hints() const {
    const auto hint = [](std::string_view key, std::string_view value) { return KeyHint{std::string(key), std::string(value)}; };
    if (page_ == Page::picker) return {hint("enter", text().hint_model_switch), hint(icons().right, text().hint_provider_open),
        hint("a", text().hint_custom_add), hint("esc", text().hint_close)};
    if (page_ == Page::catalog) return {hint("enter", text().hint_add_switch), hint("r", text().hint_refresh), hint("esc", text().hint_back)};
    if (!error_.empty()) return {hint("enter", text().hint_retry), hint("esc", text().hint_back)};
    return {hint("c", text().hint_link_copy), hint("o", text().hint_link_open), hint("esc", text().hint_cancel)};
}
}
