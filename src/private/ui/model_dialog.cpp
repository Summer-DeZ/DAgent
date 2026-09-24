#include "ui/model_dialog.hpp"

#include <algorithm>

#include <algorithm>
#include <charconv>
#include <format>
#include <memory>
#include <string_view>
#include <utility>

#include "ui/display.hpp"

namespace dagent::ui {
namespace {

constexpr int k_fields = 7;

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<std::size_t> positive_number(std::string_view text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) return std::nullopt;
    return value;
}

} // namespace

class ModelDialog::View final : public tui::Container {
public:
    tui::Text *prompt = nullptr, *error = nullptr;
    tui::InputBox* input = nullptr;
    std::unique_ptr<tui::InputBoxHandler> edit;
    const tui::ThemeTokens* current_theme = &tui::dark_theme();
    std::string title;

    View() {
        add({tui::Sizing::fixed, 1}, std::make_unique<tui::Text>());
        auto prompt_widget = std::make_unique<tui::Text>(); prompt = prompt_widget.get();
        add({tui::Sizing::content, 0, 1, 2}, std::move(prompt_widget));
        auto input_widget = std::make_unique<tui::InputBox>(); input = input_widget.get();
        add({tui::Sizing::fixed, 3}, std::move(input_widget));
        auto error_widget = std::make_unique<tui::Text>(); error = error_widget.get();
        add({tui::Sizing::fixed, 1}, std::move(error_widget));
        add({tui::Sizing::fixed, 1}, std::make_unique<tui::Text>());
        edit = std::make_unique<tui::InputBoxHandler>(*input);
    }

    tui::Size measure(tui::Size available) const override {
        return {std::min(76, std::max(1, available.cols - 8)), 9};
    }

    void layout(tui::Rect area) override {
        tui::Container::layout(area);
        for (tui::Widget* child : {static_cast<tui::Widget*>(prompt),
                                   static_cast<tui::Widget*>(input),
                                   static_cast<tui::Widget*>(error)}) {
            const auto rect = child->rect();
            child->layout({2, rect.y, std::max(0, area.w - 4), rect.h});
        }
    }

    void render(tui::Surface& surface) override {
        surface.fill({0, 0, surface.cols(), surface.rows()}, U' ', current_theme->background_panel);
        tui::Container::render(surface);
        const int width = surface.cols(), height = surface.rows();
        if (width < 4 || height < 3) return;
        surface.fill({1, 0, width - 2, 1}, U'─', current_theme->border_active);
        surface.fill({1, height - 1, width - 2, 1}, U'─', current_theme->border_active);
        surface.fill({0, 1, 1, height - 2}, U'│', current_theme->border_active);
        surface.fill({width - 1, 1, 1, height - 2}, U'│', current_theme->border_active);
        surface.put(0, 0, "╭", current_theme->border_active);
        surface.put(width - 1, 0, "╮", current_theme->border_active);
        surface.put(0, height - 1, "╰", current_theme->border_active);
        surface.put(width - 1, height - 1, "╯", current_theme->border_active);
        surface.text(2, 0, fit_columns(" " + title + " ", width - 4), current_theme->text);
        const std::string footer = " enter next/save · shift+tab back · esc cancel ";
        surface.text(std::max(2, (width - display_width(footer)) / 2), height - 1,
                     fit_columns(footer, width - 4), current_theme->text_muted);
    }

    void theme(const tui::ThemeTokens& value) {
        current_theme = &value;
        prompt->set_theme(value); prompt->set_style(value.text);
        error->set_theme(value); error->set_style(value.error);
        input->set_theme(value);
    }
};

ModelDialog::ModelDialog(tui::Runtime& rt) : rt_(rt) {}
ModelDialog::~ModelDialog() { close(); }

void ModelDialog::open(std::size_t default_context_window,
                       const std::vector<ProviderKind>& kinds, Submit submit) {
    close();
    kinds_ = kinds;
    values_ = {"openai-chat", "", "", "", "", "8192",
               std::to_string(default_context_window == 0 ? 131072 : default_context_window)};
    step_ = 0;
    submit_ = std::move(submit);
    auto view = std::make_unique<View>(); view_ = view.get(); view_->theme(theme_);
    overlay_ = rt_.open_overlay(std::move(view), tui::Placement::center, {}, this, view_->input);
    refresh();
}

void ModelDialog::close() {
    if (!overlay_) return;
    rt_.close_overlay(overlay_);
    overlay_ = 0; view_ = nullptr; values_.clear(); submit_ = {};
}

void ModelDialog::set_theme(const tui::ThemeTokens& theme) {
    theme_ = theme;
    if (view_) { view_->theme(theme_); view_->invalidate_tree(); }
}

void ModelDialog::fail(std::string message) {
    if (!view_) return;
    view_->error->set_text("  " + std::move(message));
}

const ProviderKind* ModelDialog::find_kind(std::string_view kind) const {
    const auto it = std::ranges::find_if(kinds_, [&](const ProviderKind& info) {
        return info.kind == kind;
    });
    return it == kinds_.end() ? nullptr : &*it;
}

bool ModelDialog::accept_field() {
    if (!view_) return false;
    const std::string value = trim(view_->input->text());
    switch (step_) {
    case 0: {
        const ProviderKind* provider = find_kind(value);
        if (!provider) { fail("kind must be openai-chat, anthropic or ollama"); return false; }
        values_[0] = value;
        values_[2] = provider->default_base_url;
        break;
    }
    case 1:
        if (value.empty()) { fail("configuration name is required"); return false; }
        values_[1] = value;
        break;
    case 2:
        if (value.empty()) { fail("base URL is required"); return false; }
        values_[2] = value;
        break;
    case 3:
        if (value.empty()) { fail("model ID is required"); return false; }
        values_[3] = value;
        break;
    case 4: {
        const ProviderKind* provider = find_kind(values_[0]);
        if (provider && provider->needs_credential && value.empty()) {
            fail("this provider requires an API key or env:VARIABLE"); return false;
        }
        values_[4] = value;
        break;
    }
    case 5:
        if (!positive_number(value)) { fail("max tokens must be a positive integer"); return false; }
        values_[5] = value;
        break;
    case 6:
        if (!positive_number(value)) { fail("context window must be a positive integer"); return false; }
        values_[6] = value;
        break;
    default:
        return false;
    }
    return true;
}

void ModelDialog::move(int delta) {
    if (!view_) return;
    if (delta > 0 && !accept_field()) return;
    step_ = std::clamp(step_ + delta, 0, k_fields - 1);
    refresh();
}

void ModelDialog::refresh() {
    if (!view_) return;
    static constexpr std::string_view prompts[k_fields] = {
        "Provider kind: openai-chat, anthropic or ollama",
        "Configuration name shown in /model",
        "Base URL (without /chat/completions or /messages)",
        "Provider model ID",
        "API key, env:VARIABLE, or empty when the endpoint needs no key",
        "Maximum output tokens",
        "Total context window tokens",
    };
    view_->title = std::format("Add model {}/{}", step_ + 1, k_fields);
    view_->prompt->set_text("  " + std::string(prompts[step_]));
    view_->error->set_text({});
    view_->input->set_text(values_[static_cast<std::size_t>(step_)]);
    view_->input->line_end();
    view_->invalidate_tree();
}

bool ModelDialog::on_event(const tui::Event& event) {
    if (!view_) return false;
    if (event.key == tui::Key::escape ||
        (tui::any(event.mods & tui::Mods::ctrl) && event.text == "c")) {
        close(); return true;
    }
    if (event.key == tui::Key::tab && tui::any(event.mods & tui::Mods::shift)) {
        move(-1); return true;
    }
    if (event.key == tui::Key::tab || event.key == tui::Key::enter) {
        if (step_ + 1 < k_fields) { move(1); return true; }
        if (!accept_field()) return true;
        ModelInput model;
        model.kind = values_[0]; model.name = values_[1]; model.base_url = values_[2];
        model.model = values_[3]; model.credential = values_[4];
        model.max_tokens = *positive_number(values_[5]);
        model.context_window = *positive_number(values_[6]);
        auto submit = std::move(submit_);
        rt_.close_overlay(overlay_); overlay_ = 0; view_ = nullptr; values_.clear();
        if (submit) submit(std::move(model));
        return true;
    }
    view_->error->set_text({});
    view_->edit->on_event(event);
    return true;
}

} // namespace dagent::ui
