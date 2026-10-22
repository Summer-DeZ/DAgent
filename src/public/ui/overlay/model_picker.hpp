#pragma once

#include "protocol/dto.hpp"
#include "protocol/rpc.hpp"
#include "ui/page/hint_bar.hpp"
#include "ui/overlay/panel.hpp"
#include "ui/overlay/toast.hpp"

namespace dagent::ui {

// All callbacks run on the render thread, through Shell's RPC lifetime gate.
class ModelPicker final : public tui::EventHandler {
public:
    using Done = std::function<void(const nlohmann::json&)>;
    using Failed = std::function<void(const protocol::RpcError&)>;
    using Request = std::function<void(std::string, nlohmann::json, Done, Failed)>;
    struct Actions {
        Request request;
        std::function<nlohmann::json()> target;
        std::function<void(const std::string&)> select;
        std::function<void()> custom, changed;
        std::function<void(ToastStack::Severity, std::string)> toast;
    };

    ModelPicker(tui::Runtime&, Actions);
    ~ModelPicker() override;
    void open(const std::vector<protocol::PublicModel>&, std::string current);
    void update_models(const std::vector<protocol::PublicModel>&, std::string current);
    void close();
    bool active() const noexcept;
    void set_theme(const tui::ThemeTokens&, bool outline);
    std::vector<KeyHint> hints() const;
    bool on_event(const tui::Event&) override;

private:
    enum class Page { closed, picker, login, catalog };
    class LoginView;
    void dismiss();
    void show_picker();
    void load_status();
    void provider();
    void custom();
    void start_login();
    void login_error(std::string);
    void show_catalog(bool refresh = false);
    void add_provider_model(const std::string&);
    std::vector<Panel::Row> picker_rows();
    void catalog_rows();
    void details(tui::Surface&, int);
    bool signed_in() const;
    std::string status_text() const;
    void cancel_login(const std::string&);

    tui::Runtime& rt_;
    Actions actions_;
    Panel panel_;
    Page page_ = Page::closed;
    std::uint64_t epoch_ = 0;
    std::vector<protocol::PublicModel> models_;
    std::string current_, status_error_, error_, login_id_, url_, code_;
    nlohmann::json status_, catalog_;
    std::int64_t expires_at_ = 0, catalog_received_ = 0;
    bool browser_opened_ = false, pending_ = false, outline_ = false;
    int frame_ = 0;
    tui::TimerId timer_ = 0;
    std::uint32_t overlay_ = 0;
    LoginView* login_view_ = nullptr;
    tui::ThemeTokens theme_ = tui::dark_theme();
};
}
