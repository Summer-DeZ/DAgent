#include "storage/decision_store.hpp"


#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include "base/json.hpp"
#include "storage/storage.hpp"
#include "storage/sqlite.hpp"

#include "agent/decision_types.hpp"
#include "lib/sqlite/sqlite3.h"

namespace dagent::storage {
namespace fs = std::filesystem;
namespace {

using nlohmann::json;
using namespace dagent::agent;

using detail::Statement;
using detail::fail;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---- 名称与枚举的互换 ----

const char* pass_name(AdaptiveThreshold::Pass pass) {
    return pass == AdaptiveThreshold::Pass::at_most ? "at_most" : "at_least";
}

const char* disposition_name(ReviewDisposition disposition) {
    switch (disposition) {
    case ReviewDisposition::approve: return "approve";
    case ReviewDisposition::defer: return "defer";
    case ReviewDisposition::unavailable: return "unavailable";
    case ReviewDisposition::cancelled: return "cancelled";
    }
    return "unavailable";
}

const char* decision_error_name(DecisionError::Kind kind) {
    switch (kind) {
    case DecisionError::Kind::unavailable: return "unavailable";
    case DecisionError::Kind::timeout: return "timeout";
    case DecisionError::Kind::invalid_response: return "invalid_response";
    case DecisionError::Kind::cancelled: return "cancelled";
    }
    return "unavailable";
}

ReviewScene scene_from(std::string_view name) {
    if (name == "command_network") return ReviewScene::command_network;
    if (name == "script_basic") return ReviewScene::script_basic;
    if (name == "script_mixed_basic") return ReviewScene::script_mixed_basic;
    if (name == "script_diff") return ReviewScene::script_diff;
    if (name == "outside_read") return ReviewScene::outside_read;
    if (name == "mcp_action") return ReviewScene::mcp_action;
    if (name == "fetch_url") return ReviewScene::fetch_url;
    if (name == "web_content") return ReviewScene::web_content;
    fail(StorageError::Kind::corrupt, "unknown decision scene: " + std::string(name));
}

Dimension dimension_from(std::string_view name) {
    if (name == "relevance") return Dimension::relevance;
    if (name == "needs_authorization") return Dimension::needs_authorization;
    if (name == "evidence_sufficient") return Dimension::evidence_sufficient;
    if (name == "manual_only") return Dimension::manual_only;
    if (name == "authorization") return Dimension::authorization;
    if (name == "direction") return Dimension::direction;
    if (name == "net_change") return Dimension::net_change;
    if (name == "privacy") return Dimension::privacy;
    if (name == "injection") return Dimension::injection;
    fail(StorageError::Kind::corrupt, "unknown decision dimension: " + std::string(name));
}

Direction direction_from(std::string_view name) {
    if (name == "fetch") return Direction::fetch;
    if (name == "modify") return Direction::modify;
    if (name == "send") return Direction::send;
    if (name == "unknown") return Direction::unknown;
    fail(StorageError::Kind::corrupt, "unknown decision direction: " + std::string(name));
}

double threshold_real(const Statement& statement, int column) {
    const int type = statement.column_type(column);
    if (type != SQLITE_INTEGER && type != SQLITE_FLOAT)
        fail(StorageError::Kind::corrupt, "decision threshold is not numeric");
    return statement.real(column);
}

// ---- 决策库连接与初始化 ----

class Database {
public:
    explicit Database(const fs::path& file) : file_(file) {
        if (file_.empty()) fail(StorageError::Kind::io, "decision database path is empty");
        if (!file_.parent_path().empty()) {
            std::error_code ec;
            fs::create_directories(file_.parent_path(), ec);
            if (ec) fail(StorageError::Kind::io, "cannot create decision database directory: " + ec.message());
        }
        open();
        try {
            configure();
            initialize();
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }
    ~Database() { if (db_ != nullptr) sqlite3_close(db_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    sqlite3* get() const { return db_; }
    void exec(std::string_view sql) { detail::exec(db_, sql); }

private:
    void open() {
        const int rc = sqlite3_open_v2(
            file_.c_str(), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            const std::string text = db_ == nullptr ? "cannot open SQLite database" : sqlite3_errmsg(db_);
            if (db_ != nullptr) sqlite3_close(db_);
            db_ = nullptr;
            fail(StorageError::Kind::io, text);
        }
        // 审阅数据含用户原话与命令原文：仅本人可读；不支持 chmod 时忽略。
        ::chmod(file_.c_str(), 0600);
        sqlite3_busy_timeout(db_, 5000);
    }
    void configure() {
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA busy_timeout=5000;");
    }
    void initialize() {
        exec(R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS reviews (
  id TEXT PRIMARY KEY,
  approval_request_id TEXT,
  session_id TEXT,
  call_id TEXT,
  execution_id TEXT,
  root_turn_id TEXT,
  scene TEXT,
  version_key TEXT,
  state TEXT,
  reason TEXT,
  failed TEXT,
  payload TEXT,
  created_ms INTEGER
);
CREATE INDEX IF NOT EXISTS reviews_by_approval ON reviews(approval_request_id);
CREATE TABLE IF NOT EXISTS feedback (
  approval_request_id TEXT PRIMARY KEY,
  review_ids TEXT,
  final_authority TEXT,
  decision TEXT,
  created_ms INTEGER
);
CREATE TABLE IF NOT EXISTS thresholds (
  version_key TEXT,
  scene TEXT,
  dimension TEXT,
  direction TEXT,
  pass TEXT,
  strict REAL,
  relaxed REAL,
  current REAL,
  revision INTEGER,
  PRIMARY KEY(version_key, scene, dimension, direction)
);
CREATE TABLE IF NOT EXISTS fixed_thresholds (
  version_key TEXT,
  name TEXT,
  value REAL,
  PRIMARY KEY(version_key, name)
);
COMMIT;
)SQL");
    }
    fs::path file_;
    sqlite3* db_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE;"); }
    ~Transaction() {
        if (!committed_) {
            try { db_.exec("ROLLBACK;"); } catch (...) {}
        }
    }
    void commit() { db_.exec("COMMIT;"); committed_ = true; }

private:
    Database& db_;
    bool committed_ = false;
};

// ---- 记录序列化 ----

json identity_json(const ApprovalIdentity& identity) {
    return json{{"request_id", identity.request_id},
                {"parent_session_id", identity.parent_session_id},
                {"child_session_id", identity.child_session_id},
                {"origin_call_id", identity.origin_call_id},
                {"call_id", identity.call_id},
                {"execution_id", identity.execution_id},
                {"parent_revision", identity.parent_revision},
                {"child_revision", identity.child_revision}};
}

json version_json(const ReviewVersion& version) {
    return json{{"provider_kind", version.provider_kind},
                {"adapter_version", version.adapter_version},
                {"endpoint", version.endpoint},
                {"model", version.model},
                {"model_revision", version.model_revision},
                {"question_version", version.question_version},
                {"threshold_profile", version.threshold_profile},
                {"key", version.key()}};
}

json threshold_key_json(const ThresholdKey& key) {
    return json{{"scene", std::string(to_string(key.scene))},
                {"dimension", std::string(to_string(key.dimension))},
                {"direction", key.direction ? std::string(to_string(*key.direction)) : std::string{}}};
}

json adaptive_threshold_json(const AdaptiveThreshold& threshold) {
    return json{{"pass", pass_name(threshold.pass)},
                {"strict_value", threshold.strict_value},
                {"relaxed_value", threshold.relaxed_value},
                {"current_value", threshold.current_value}};
}

json thresholds_json(const ThresholdProfile& profile) {
    json adaptive = json::array();
    for (const auto& [key, threshold] : profile.adaptive) {
        adaptive.push_back(json{{"key", threshold_key_json(key)},
                                {"threshold", adaptive_threshold_json(threshold)}});
    }
    json risk = json::object();
    for (const auto& [scene, gate] : profile.risk) {
        risk[std::string(to_string(scene))] = {
            {"low", gate.low ? json(*gate.low) : json(nullptr)},
            {"high", gate.high ? json(*gate.high) : json(nullptr)},
            {"unknown_policy", !gate.unknown_policy ? json(nullptr)
                : json(*gate.unknown_policy == RiskThresholds::UnknownPolicy::authorize ? "authorize" : "defer")}};
    }
    return json{{"revision", profile.revision},
                {"adaptive", std::move(adaptive)},
                {"risk", std::move(risk)},
                {"net_change_low", profile.net_change_low ? json(*profile.net_change_low) : json(nullptr)},
                {"net_change_high", profile.net_change_high ? json(*profile.net_change_high) : json(nullptr)},
                {"injection_limit", profile.injection_limit ? json(*profile.injection_limit) : json(nullptr)}};
}

json score_json(const DecisionScore& score) {
    if (const auto* value = std::get_if<double>(&score))
        return json{{"kind", "noul"}, {"noul", *value}};
    const auto& choice = std::get<DirectionScore>(score);
    json probabilities = json::object();
    for (const auto& [direction, probability] : choice.probabilities)
        probabilities[std::string(to_string(direction))] = probability;
    return json{{"kind", "choice"},
                {"selected", std::string(to_string(choice.selected))},
                {"probabilities", std::move(probabilities)}};
}

json reply_json(const DecisionReply& reply) {
    json scores = json::object();
    for (const auto& [dimension, score] : reply.scores)
        scores[std::string(to_string(dimension))] = score_json(score);
    return json{{"model", reply.model},
                {"scores", std::move(scores)},
                {"usage", json{{"prompt", reply.usage.prompt},
                               {"completion", reply.usage.completion},
                               {"cached", reply.usage.cached}}}};
}

json error_json(const DecisionError& error) {
    return json{{"kind", decision_error_name(error.kind)}, {"detail", error.detail}};
}

json question_json(const DecisionQuestion& question) {
    json criteria = json::array();
    for (const auto& [direction, description] : question.criteria)
        criteria.push_back(json{{"direction", std::string(to_string(direction))},
                                {"description", description}});
    return json{{"dimension", std::string(to_string(question.dimension))},
                {"instructions", question.instructions},
                {"criteria", std::move(criteria)}};
}

json request_json(const DecisionRequest& request) {
    json questions = json::array();
    for (const DecisionQuestion& question : request.questions)
        questions.push_back(question_json(question));
    return json{{"state", request.state}, {"questions", std::move(questions)}};
}

json attempt_json(const DecisionAttempt& attempt) {
    return json{{"scene", std::string(to_string(attempt.scene))},
                {"target_id", attempt.target_id},
                {"request", request_json(attempt.request)},
                {"reply", attempt.reply ? reply_json(*attempt.reply) : json(nullptr)},
                {"error", attempt.error ? error_json(*attempt.error) : json(nullptr)},
                {"elapsed_ms", attempt.elapsed.count()}};
}

json failed_check_json(const FailedCheck& check) {
    return json{{"scene", std::string(to_string(check.scene))},
                {"dimension", std::string(to_string(check.dimension))},
                {"direction", check.direction ? std::string(to_string(*check.direction)) : std::string{}},
                {"score", check.score},
                {"adjustable", check.adjustable}};
}

json failed_checks_json(const std::vector<FailedCheck>& checks) {
    json array = json::array();
    for (const FailedCheck& check : checks) array.push_back(failed_check_json(check));
    return array;
}

json record_json(const DecisionReviewRecord& record) {
    json attempts = json::array();
    for (const DecisionAttempt& attempt : record.attempts) attempts.push_back(attempt_json(attempt));
    json result{{"disposition", disposition_name(record.result.disposition)},
                {"record_id", record.result.record_id},
                {"reason_code", record.result.reason_code},
                {"reason", record.result.reason},
                {"failed_checks", failed_checks_json(record.result.failed_checks)}};
    return json{{"id", record.id},
                {"identity", identity_json(record.identity)},
                {"root_turn_id", record.root_turn_id},
                {"version", version_json(record.version)},
                {"thresholds", thresholds_json(record.thresholds)},
                {"attempts", std::move(attempts)},
                {"result", std::move(result)}};
}

// ---- 存储实现 ----

class SqliteDecisionStore final : public DecisionReviewStore {
public:
    explicit SqliteDecisionStore(const DecisionStoreOptions& options) : db_(options.database) {
        if (options.initial_profile) seed(*options.initial_profile);
    }

    std::optional<ThresholdProfile> thresholds(const ReviewVersion& version) override {
        const std::lock_guard lock(mutex_);
        const std::string key = version.key();
        ThresholdProfile profile;
        profile.version = version;
        bool found = false;
        {
            Statement query(db_.get(),
                            "SELECT scene,dimension,direction,pass,strict,relaxed,current,revision "
                            "FROM thresholds WHERE version_key=?");
            query.text_or_empty(1, key);
            while (query.row()) {
                found = true;
                ThresholdKey threshold_key;
                threshold_key.scene = scene_from(query.string(0));
                threshold_key.dimension = dimension_from(query.string(1));
                const std::string direction = query.string(2);
                if (!direction.empty()) threshold_key.direction = direction_from(direction);
                AdaptiveThreshold threshold;
                const auto pass = query.string(3);
                if (pass != "at_most" && pass != "at_least")
                    fail(StorageError::Kind::corrupt, "invalid decision threshold pass");
                threshold.pass = pass == "at_most" ? AdaptiveThreshold::Pass::at_most
                                                              : AdaptiveThreshold::Pass::at_least;
                threshold.strict_value = threshold_real(query, 4);
                threshold.relaxed_value = threshold_real(query, 5);
                threshold.current_value = threshold_real(query, 6);
                profile.adaptive.emplace(threshold_key, threshold);
                if (query.integer(7) < 0) fail(StorageError::Kind::corrupt, "negative threshold revision");
                const auto revision = static_cast<std::uint64_t>(query.integer(7));
                if (revision > profile.revision) profile.revision = revision;
            }
        }
        {
            Statement query(db_.get(), "SELECT name,value FROM fixed_thresholds WHERE version_key=?");
            query.text_or_empty(1, key);
            while (query.row()) {
                found = true;
                const std::string name = query.string(0);
                const double value = threshold_real(query, 1);
                if (name == "net_change_low") profile.net_change_low = value;
                else if (name == "net_change_high") profile.net_change_high = value;
                else if (name == "injection_limit") profile.injection_limit = value;
                else {
                    const auto dot = name.find('.');
                    if (dot == std::string::npos)
                        fail(StorageError::Kind::corrupt, "unknown fixed decision threshold: " + name);
                    const auto scene_name = std::string_view(name).substr(0, dot);
                    const auto scene = scene_from(scene_name);
                    const auto field = std::string_view(name).substr(dot + 1);
                    if (field == "risk_low") profile.risk[scene].low = value;
                    else if (field == "risk_high") profile.risk[scene].high = value;
                    else if (field == "unknown_authorize" && (value == 0.0 || value == 1.0))
                        profile.risk[scene].unknown_policy = value == 1.0
                            ? RiskThresholds::UnknownPolicy::authorize : RiskThresholds::UnknownPolicy::defer;
                    else fail(StorageError::Kind::corrupt, "invalid fixed decision threshold: " + name);
                }
            }
        }
        if (!found) return std::nullopt;
        validate_decision_profile(profile);
        return profile;
    }

    void append(const DecisionReviewRecord& record) override {
        const std::lock_guard lock(mutex_);
        const std::string& session = record.identity.child_session_id.empty()
                                         ? record.identity.parent_session_id
                                         : record.identity.child_session_id;
        const std::string scene = record.attempts.empty() ? std::string{}
                                                          : std::string(to_string(record.attempts.front().scene));
        const std::string state = record.attempts.empty() ? std::string{}
                                                          : record.attempts.front().request.state;
        Statement insert(db_.get(),
                         "INSERT INTO reviews(id,approval_request_id,session_id,call_id,execution_id,"
                         "root_turn_id,scene,version_key,state,reason,failed,payload,created_ms) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
        insert.text_or_empty(1, record.id);
        insert.text_or_empty(2, record.identity.request_id);
        insert.text_or_empty(3, session);
        insert.text_or_empty(4, record.identity.call_id);
        insert.text_or_empty(5, record.identity.execution_id);
        insert.text_or_empty(6, record.root_turn_id);
        insert.text_or_empty(7, scene);
        insert.text_or_empty(8, record.version.key());
        insert.text_or_empty(9, state);
        insert.text_or_empty(10, record.result.reason);
        insert.text_or_empty(11, failed_checks_json(record.result.failed_checks).dump());
        insert.text_or_empty(12, record_json(record).dump());
        insert.integer(13, now_ms());
        insert.done();
    }

    void finish(const ReviewFeedback& feedback) override {
        const std::lock_guard lock(mutex_);
        Transaction tx(db_);
        {
            Statement insert(db_.get(),
                             "INSERT OR IGNORE INTO feedback(approval_request_id,review_ids,"
                             "final_authority,decision,created_ms) VALUES(?,?,?,?,?)");
            json review_ids = json::array();
            for (const std::string& id : feedback.review_ids) review_ids.push_back(id);
            insert.text_or_empty(1, feedback.approval_request_id);
            insert.text_or_empty(2, review_ids.dump());
            insert.text_or_empty(3, to_string(feedback.final_authority));
            insert.text_or_empty(4, feedback.approved ? "approved" : "denied");
            insert.integer(5, now_ms());
            insert.done();
        }
        // 同一 approval_request_id 已处理过：重启后不重放动态阈值更新。
        if (sqlite3_changes(db_.get()) == 0) {
            tx.commit();
            return;
        }
        if (feedback.user_decided && feedback.final_authority == ApprovalAuthority::user)
            adjust_thresholds(feedback);
        tx.commit();
    }

private:
    void seed(const ThresholdProfile& profile) {
        validate_decision_profile(profile);
        Transaction tx(db_);
        // 先校验已有快照，不能用新文件掩盖数据库中的非法值。
        const auto existing = thresholds(profile.version);
        const auto conflict = [] {
            fail(StorageError::Kind::corrupt,
                 "decision profile differs from stored calibration; choose a new threshold_profile name");
        };
        for (const auto& [key, value] : profile.adaptive) {
            if (existing) {
                const auto* old = existing->find(key.scene, key.dimension, key.direction);
                if (old) {
                    if (old->pass != value.pass || old->strict_value != value.strict_value ||
                        old->relaxed_value != value.relaxed_value) conflict();
                    continue; // current 和 revision 属于人工反馈，重启不覆盖。
                }
            }
            Statement insert(db_.get(),
                "INSERT INTO thresholds(version_key,scene,dimension,direction,pass,strict,relaxed,current,revision) "
                "VALUES(?,?,?,?,?,?,?,?,0)");
            insert.text_or_empty(1, profile.version.key());
            insert.text_or_empty(2, to_string(key.scene));
            insert.text_or_empty(3, to_string(key.dimension));
            insert.text_or_empty(4, key.direction ? to_string(*key.direction) : std::string_view{});
            insert.text_or_empty(5, pass_name(value.pass));
            insert.real(6, value.strict_value);
            insert.real(7, value.relaxed_value);
            insert.real(8, value.current_value);
            insert.done();
        }
        const auto fixed = [&](std::string_view name, double value) {
            Statement query(db_.get(), "SELECT value FROM fixed_thresholds WHERE version_key=? AND name=?");
            query.text_or_empty(1, profile.version.key());
            query.text_or_empty(2, name);
            if (query.row()) {
                if (threshold_real(query, 0) != value) conflict();
                return;
            }
            Statement insert(db_.get(), "INSERT INTO fixed_thresholds(version_key,name,value) VALUES(?,?,?)");
            insert.text_or_empty(1, profile.version.key());
            insert.text_or_empty(2, name);
            insert.real(3, value);
            insert.done();
        };
        if (profile.injection_limit) fixed("injection_limit", *profile.injection_limit);
        if (profile.net_change_low) fixed("net_change_low", *profile.net_change_low);
        if (profile.net_change_high) fixed("net_change_high", *profile.net_change_high);
        for (const auto& [scene, risk] : profile.risk) {
            const auto prefix = std::string(to_string(scene)) + ".";
            if (risk.low) fixed(prefix + "risk_low", *risk.low);
            if (risk.high) fixed(prefix + "risk_high", *risk.high);
            if (risk.unknown_policy) fixed(prefix + "unknown_authorize",
                *risk.unknown_policy == RiskThresholds::UnknownPolicy::authorize ? 1.0 : 0.0);
        }
        // 补齐的固定边界与已有的另一端也必须相容。
        thresholds(profile.version);
        tx.commit();
    }

    /// @brief 一次可能触发更新的失败检查，连同它所属记录的版本键。
    struct MergedCheck {
        std::string version_key;
        FailedCheck check;
    };

    void adjust_thresholds(const ReviewFeedback& feedback) {
        std::vector<MergedCheck> checks;
        for (const std::string& id : feedback.review_ids) {
            Statement query(db_.get(), "SELECT version_key,failed,payload FROM reviews WHERE id=?");
            query.text_or_empty(1, id);
            if (!query.row()) return; // 不完整的审阅链不能归因到唯一维度。
            {
                const std::string version_key = query.string(0);
                const json payload = json::parse(query.string(2));
                const auto& result = payload.at("result");
                if (result.at("disposition") != "approve" &&
                    (result.at("disposition") != "defer" || result.at("reason_code") != "failed_checks"))
                    return;
                const std::string failed = query.string(1);
                if (failed.empty()) continue;
                json array;
                try {
                    array = json::parse(failed);
                } catch (const json::exception& error) {
                    fail(StorageError::Kind::io,
                         std::format("corrupt failed checks for review {}: {}", id, error.what()));
                }
                for (const auto& item : array) {
                    MergedCheck merged;
                    merged.version_key = version_key;
                    merged.check.scene = scene_from(item.value("scene", std::string{}));
                    merged.check.dimension = dimension_from(item.value("dimension", std::string{}));
                    const std::string direction = item.value("direction", std::string{});
                    if (!direction.empty()) merged.check.direction = direction_from(direction);
                    merged.check.score = item.value("score", 0.0);
                    merged.check.adjustable = item.value("adjustable", false);
                    checks.push_back(std::move(merged));
                }
            }
        }

        // 只有明确唯一不达标的可调维度才更新；多维度不达标或不可调项一律跳过。
        if (checks.size() != 1 || !checks.front().check.adjustable) return;
        const MergedCheck* unique = &checks.front();

        double strict = 0.0;
        double relaxed = 0.0;
        double current = 0.0;
        std::int64_t revision = 0;
        {
            Statement query(db_.get(),
                            "SELECT strict,relaxed,current,revision,pass FROM thresholds "
                            "WHERE version_key=? AND scene=? AND dimension=? AND direction=?");
            query.text_or_empty(1, unique->version_key);
            query.text_or_empty(2, to_string(unique->check.scene));
            query.text_or_empty(3, to_string(unique->check.dimension));
            query.text_or_empty(4, unique->check.direction ? to_string(*unique->check.direction) : std::string_view{});
            if (!query.row()) return; // 该维度未标定：跳过，不新建行
            strict = threshold_real(query, 0);
            relaxed = threshold_real(query, 1);
            current = threshold_real(query, 2);
            revision = query.integer(3);
            const auto pass = query.string(4);
            if (pass != "at_most" && pass != "at_least")
                fail(StorageError::Kind::corrupt, "invalid decision threshold pass");
            if (revision < 0) fail(StorageError::Kind::corrupt, "negative threshold revision");
            ThresholdProfile snapshot;
            snapshot.adaptive.emplace(
                ThresholdKey{unique->check.scene, unique->check.dimension, unique->check.direction},
                AdaptiveThreshold{pass == "at_most" ? AdaptiveThreshold::Pass::at_most
                                                     : AdaptiveThreshold::Pass::at_least,
                                  strict, relaxed, current});
            validate_decision_profile(snapshot);
        }

        const double low = std::min(strict, relaxed);
        const double high = std::max(strict, relaxed);
        // 分数必须落在两端之间才归因到该维度；区间外说明不是阈值造成的转人工。
        if (unique->check.score < low || unique->check.score > high) return;

        const double step = std::abs(strict - relaxed) / 10.0;
        if (step == 0.0) return; // 固定候选没有可调范围，不制造无变化的 revision。
        const double toward_relaxed = relaxed >= strict ? 1.0 : -1.0;
        const double delta = feedback.approved ? toward_relaxed * step : -toward_relaxed * step * 3.0;
        const double next = std::clamp(current + delta, low, high);

        Statement update(db_.get(),
                         "UPDATE thresholds SET current=?, revision=? "
                         "WHERE version_key=? AND scene=? AND dimension=? AND direction=?");
        update.real(1, next);
        update.integer(2, revision + 1);
        update.text_or_empty(3, unique->version_key);
        update.text_or_empty(4, to_string(unique->check.scene));
        update.text_or_empty(5, to_string(unique->check.dimension));
        update.text_or_empty(6, unique->check.direction ? to_string(*unique->check.direction) : std::string_view{});
        update.done();
    }

    Database db_;
    std::mutex mutex_; ///< 同一 store 由主、子 Agent 线程共用：所有调用串行。
};

} // namespace

void validate_decision_profile(const agent::ThresholdProfile& profile) {
    const auto probability = [](double value) {
        if (!std::isfinite(value) || value < 0.0 || value > 1.0)
            fail(StorageError::Kind::corrupt, "decision threshold must be finite and within [0,1]");
    };
    for (const auto& [key, threshold] : profile.adaptive) {
        probability(threshold.strict_value);
        probability(threshold.relaxed_value);
        probability(threshold.current_value);
        const auto expected = key.dimension == Dimension::privacy || key.dimension == Dimension::manual_only ||
                              key.dimension == Dimension::injection
            ? AdaptiveThreshold::Pass::at_most : AdaptiveThreshold::Pass::at_least;
        if (threshold.pass != expected)
            fail(StorageError::Kind::corrupt, "decision threshold pass disagrees with its dimension");
        const bool ordered = threshold.pass == AdaptiveThreshold::Pass::at_most
            ? threshold.strict_value <= threshold.current_value && threshold.current_value <= threshold.relaxed_value
            : threshold.relaxed_value <= threshold.current_value && threshold.current_value <= threshold.strict_value;
        if (!ordered) fail(StorageError::Kind::corrupt, "decision threshold current is outside its ordered bounds");
    }
    const auto bounds = [&](const std::optional<double>& low, const std::optional<double>& high) {
        if (low) probability(*low);
        if (high) probability(*high);
        if (low && high && *low > *high)
            fail(StorageError::Kind::corrupt, "decision risk low exceeds high");
    };
    for (const auto& [scene, risk] : profile.risk) bounds(risk.low, risk.high);
    bounds(profile.net_change_low, profile.net_change_high);
    if (profile.injection_limit) probability(*profile.injection_limit);
}

std::shared_ptr<agent::DecisionReviewStore> open_decision_store(const DecisionStoreOptions& options) {
    if (options.database.empty())
        throw StorageError(StorageError::Kind::io, "decision database path is empty");
    return std::make_shared<SqliteDecisionStore>(options);
}

} // namespace dagent::storage
