# d3 权限审批接口设计草案

日期：2026-10-17。状态：接口草案，尚未实现。

已结合当前源码完成一次兼容性审查；具体缺口、处理约定及真实运行验收项见第 15 节。此结论不代表实现或运行验收已经完成。

依据：[d3 权限审批计划](d3-approval-plan.md)。本文将已确定的行为落到当前 C++ 模块和接口；代码块仅表示建议声明，省略 include、已有成员与私有实现。功能实现后再共同校准，不在本草案指定正式阈值。

本文中的 d3 是第一版实际使用的决策模型；配置、核心接口和审阅记录使用通用 Decision 命名，模型协议由适配器负责。业务场景与触发时机不随决策模型更换而改变。

## 1. 必须保持的调用边界

| 场景 | 调用时机 | 提供的证据 |
| --- | --- | --- |
| 普通 bash 联网 | 运行中实际请求网络目标，需要代批时 | 用户原话、完整操作事实、命令、host:port |
| 明确目标脚本的基础审查 | 运行中联网触发审阅后，每个目标分别审查 | 完整命令背景、当前目标路径、修改状态；未修改文件不读内容 |
| 目标脚本 diff 复审 | auto-edit 下运行中联网，且目标在本会话新建或修改过；已有 host 放行也不能跳过 | 当前目标的 diff；未跟踪文件按新增、非 Git 仓库用全文 |
| 工作区外普通文件读取 | read 工具或可确定路径的简单只读命令实际读取前 | 路径与用户任务；不先读取文件正文给 d3 |
| MCP 动作 | 调用外部工具前 | 工具名与调用参数；不扫描调用结果 |
| web_fetch URL | 初始请求及每次重定向发送前 | 当前完整 URL，包括查询参数 |
| web_fetch 结果 | 该次结果交给模型之前，包括缓存分页 | 按计划切片抽样后的 fetch 结果 |

准备阶段只做确定性分析和元数据采集，不调用 d3、不生成脚本 diff。运行中复审只检查当前目标文件，不遍历 import、source、require、子脚本或其他依赖。`npm run` / `make` 不展开对应文件内容，联网时按普通命令审阅。

一条命令里的多个明确脚本分别做基础审查；需要 diff 的目标分别提交，一次请求不混入其他目标的文件内容。300 行上限按单文件计。所有必要审查通过后才产生 d3 批准。

## 2. 模块与依赖

沿用核心端口与外围适配的现有结构，不让 Policy 访问 HTTP、Git 或数据库。

| 文件落点 | 新增或调整的职责 |
| --- | --- |
| `app/model_config.hpp` / `.cpp` | 从现有 app/config 中抽取模型配置加载、校验、选择与保存；统一维护生成模型和决策模型目录 |
| `agent/decision_types.hpp` | 审阅场景、请求、概率、版本、阈值、审阅结果与记录值类型 |
| `agent/port_decision.hpp` | `DecisionModel` 与 `DecisionReviewStore` 两个外部端口 |
| `agent/decision_review.hpp` / `.cpp` | state 构造、固定问句、打分判定、扫描抽样、后续校准归因 |
| `llm/decision_provider.hpp` / `.cpp` | 决策协议配置、适配器目录与 make_decision_model 工厂 |
| `llm/decision_systemone.hpp` / `.cpp` | 第一版 SystemOneDecisionModel，实现 DecisionModel，调用 `/v1/systemone`，复用 `net::HttpClient` |
| `storage/decision_store.hpp` / `.cpp` | 实现 `DecisionReviewStore`，保存审阅数据与动态阈值 |
| `agent/intent.hpp`、`exec/shell.hpp`、`tools/bash.cpp` | 确定目标脚本与简单只读命令的读取路径 |
| `agent/action.hpp`、`workspace/diff.hpp` / `.cpp` | 运行中取得单个目标的修改状态与文件证据 |
| `agent/permission.hpp` / `.cpp`、`agent/grant.hpp` | 网络规则事实、d3 来源、文件级授权与授权消费 |
| `agent/dispatch.cpp`、`runtime/subagent.cpp` | d3 → 父复批 → 人工的编排，网络及网页回调 |
| `agent/run.hpp`、`agent/session.hpp`、`agent/port_delegation.hpp` | 根轮次共享状态、真实用户消息、委派上下文 |
| `tools/web.cpp`、`web/http.cpp`、`web/web.hpp` | 逐跳请求与结果交付前扫描 |
| `agent/committer`、`agent/record_codec`、`protocol/dto`、`backend/convert`、`ui/*` | 轻量审阅记录、前后端显示及 unrestricted 确认 |
| `app/config`、`app/configuration`、`app/session_assembly` | 配置、实例所有权与可选装配 |

表中 `.hpp` 均位于 `src/public/`，`.cpp` 均位于 `src/private/`。新增编译单元沿用 `src/CMakeLists.txt`。本文只新增 Markdown，不创建这些源码文件。

## 3. 配置与装配

### 3.1 模型配置模块

将现有 `app/config.cpp` 中的 `load_models()`、模型条目校验和 `add_model()` 等职责抽取到 `app/model_config`；继续使用 `home/config/models.json`，不新增一套配置目录。

模块负责加载/保存、名称解析、凭据解析与公开描述；不发送推理请求、不管理审批阈值、不决定是否批准。生成模型继续使用已有 `llm::ProviderConfig` 和 `ModelSession`，决策模型使用独立的 `DecisionProviderConfig` 和 `DecisionModel`。两者共用配置入口，保留各自的字段和能力契约。

`models.json` 保留原有 `default` 与 `models`，增量添加 `decision` 与 `decision_models`。以下为需要合入现有文件的决策配置片段：

```json
{
  "decision": "d3-local",
  "decision_models": {
    "d3-local": {
      "kind": "systemone",
      "endpoint": "http://116.56.129.99:10006/v1/systemone",
      "model": "d3",
      "model_revision": "deployment-1"
    }
  }
}
```

行为配置仍放 `config.json`：

```json
{
  "decision_review": {
    "timeout_ms": 10000,
    "threshold_profile": "default"
  }
}
```

- `decision` 是决策模型配置名，缺省或 null 表示禁用决策审阅，保持无 d3 的原行为；配置了不存在的名称或不支持的 kind 则明确报配置错误。
- `kind` 标识接口协议，`model` 标识模型，配置名标识用户维护的连接条目。`systemone` 与 `d3` 不是同一个概念。
- `endpoint` 在 System One 适配器中是完整地址，不再拼接 `/v1`；其他适配器需明确各自的地址语义。
- 当前适配器使用 model 核对响应和记录，不假定 `/v1/systemone` 支持通过请求字段切换模型；请求仍为已实测的 `state + questions`。该部署换权重需要服务端部署或切换 endpoint，不能只改 model 字符串。
- `model_revision` 由部署者在权重或量化变化时更新。`d3` 别名不能区分权重版本。
- `timeout_ms` 第一版固定为 10000，与网络审批、工具执行的剩余时间取最短值。`threshold_profile` 指向 Home/data 的阈值配置，正式与临时配置分开。
- 问句版本由程序维护，不放入连接配置。当前 System One 服务无需鉴权；后续适配器需要凭据时复用现有模型配置的凭据解析和公开投影边界，不把密钥传入核心。
- 旧 models.json 不含新字段仍能加载；现有生成模型列表和 `/model` 切换行为保持原角色，不自动列入决策模型。

建议接口增量：

```cpp
// llm/decision_provider.hpp
struct DecisionProviderConfig {
    std::string name;
    std::string kind;
    std::string endpoint;
    std::string model;
    std::string model_revision;
};

// app/model_config.hpp：扩展现有 ModelCatalog，不另建重复目录对象。
struct ModelCatalog {
    std::map<std::string, llm::ProviderConfig> models;
    std::string selected;
    std::vector<std::string> selection_log; // 保留现有 --model 解析日志
    std::map<std::string, llm::DecisionProviderConfig> decision_models;
    std::optional<std::string> selected_decision;
};

ModelCatalog load_models(const std::filesystem::path& root,
                        const std::vector<std::string>& overrides);
void select_decision_model(const std::filesystem::path& root,
                           std::optional<std::string_view> name);
```

配置保存沿用现有 `run/models.lock`、原子替换与 0600 文件权限；修改某一角色时保留另一角色及其他条目。`select_decision_model()` 只校验并保存选择，不负责中途替换正在工作的 Session。主配置装配保存模型目录与 `DecisionReviewOptions`，不再增加以 D3 命名的单独连接配置。

### 3.2 所有权

```cpp
// agent/port_decision.hpp
struct DecisionServices {
    std::shared_ptr<DecisionModel> model;
    std::shared_ptr<DecisionReviewStore> store;
    ReviewVersion version;
};
```

- `SessionAssembly` 持有并向主、子会话共享 `DecisionServices`；`Session` 构造函数增加可选的 `std::shared_ptr<DecisionServices>`，并提供只读访问器。
- 未配置时指针为空，不建客户端、不发请求、不把“不存在 d3”记成扫描故障。
- 端口和存储寿命覆盖后端会话；一轮开关属于 Run；某次执行的 URL 与网络批准缓存属于 Dispatcher 的执行槽位。
- 不把 d3 混入用于生成主模型回复的 `ModelSession`，不复用父模型的消息历史或 token 预算。d3 用量独立记录。

### 3.3 决策适配器与切换边界

```cpp
// llm/decision_provider.hpp
struct DecisionCapabilities {
    bool boolean_probability;
    bool choice_probabilities;
};

struct DecisionProviderInfo {
    std::string_view kind;
    std::string_view adapter_version;
    DecisionCapabilities capabilities;
};

std::span<const DecisionProviderInfo> decision_providers() noexcept;
std::shared_ptr<agent::DecisionModel> make_decision_model(
    const DecisionProviderConfig&, std::chrono::milliseconds timeout);
```

第一版只注册 `systemone` 并实现 `SystemOneDecisionModel`。适配器负责 HTTP 地址、请求字段、响应解码、概率语义与错误转换；核心只调用 `DecisionModel::evaluate()`。注册表是普通静态目录和工厂，不引入动态插件加载。

| 切换情况 | 要改什么 | 不需要改什么 |
| --- | --- | --- |
| 同协议换服务地址/部署 | models.json 的选择或连接配置，重新核对模型版本和校准配置 | Dispatcher、Policy、文件复审流程 |
| 同协议换权重 | 服务端部署或 endpoint、model/model_revision；重新校准 | 已兼容的协议适配器 |
| 换成另一种决策 API | 增加协议适配器、注册 kind，将结果映射为统一概率接口；校准 | 审批路由与既定复审时机 |
| 换成只输出 allow/deny 或文字理由的模型 | 不能直接接入当前概率阈值契约；需要另行设计其判定方式 | 不能把分类硬转成 0/1 概率来复用当前阈值 |

只有能提供布尔概率和各选项概率的适配器才满足当前完整审批方案；普通模型输出的“自报置信度”不自动等价于这些概率。适配器必须解释分数来源，未知能力不推测、不补分数。

第一版通过保存配置并重启后端切换决策模型，不增加运行中热切换 UI。一次根轮次和所有子 Agent 固定使用同一 `DecisionServices` 快照。以后若增加交互式切换，也只在根轮次结束后应用，避免一个审批流程混用两个模型或两套阈值。

阈值与反馈按 endpoint、协议 kind、adapter_version、model、model_revision、question_version、threshold_profile 隔离。任一身份变化不继承旧动态阈值；新模型尚无有效标定配置时进入后续审批，有配置则从严格端开始。模型配置名仅供选择和展示，不用它替代实际模型身份。

## 4. 统一决策请求与第一版协议适配

### 4.1 中立值类型

```cpp
namespace dagent::agent {

enum class Dimension { relevance, direction, net_change, privacy, injection };
enum class Direction { fetch, modify, send, unknown };
enum class ReviewScene {
    command_network, script_basic, script_diff,
    outside_read, mcp_action, fetch_url, web_content
};

struct ReviewVersion {
    std::string model_config_name;
    std::string provider_kind;
    std::string adapter_version;
    std::string endpoint;
    std::string model;
    std::string model_revision;
    std::string question_version;
    std::string threshold_profile;
};

struct DecisionQuestion {
    Dimension dimension;
    std::string instructions;
    // 空表示 noul；direction 固定使用四个类别及计划中的选项说明。
    std::vector<std::pair<Direction, std::string>> criteria;
};

struct DirectionScore {
    Direction selected;
    std::map<Direction, double> probabilities;
};
using DecisionScore = std::variant<double, DirectionScore>;

struct DecisionRequest {
    std::string state;
    std::vector<DecisionQuestion> questions;
};

struct DecisionReply {
    std::string model;
    std::map<Dimension, DecisionScore> scores;
    Usage usage;
};

struct DecisionError {
    enum class Kind { unavailable, timeout, invalid_response, cancelled };
    Kind kind;
    std::string detail;
};

using ReviewDeadline = std::chrono::steady_clock::time_point;

class DecisionModel {
public:
    virtual ~DecisionModel() = default;
    virtual std::expected<DecisionReply, DecisionError> evaluate(
        const DecisionRequest&, ReviewDeadline, std::stop_token) = 0;
};

} // namespace dagent::agent
```

`asked` 第一版不请求、不参与判断。固定问句表位于 `agent/decision_review.cpp`；`llm/decision_systemone.cpp` 只编码协议，不自行生成或改写问句。

### 4.2 System One 协议映射与超时

| 核心数据 | HTTP JSON |
| --- | --- |
| `DecisionRequest::state` | `state` 字符串 |
| `DecisionQuestion::dimension` | `questions` 中的键名 |
| criteria 为空 | `type: "noul"`，返回 `answers.<key>.noul` |
| criteria 非空 | `type: "choice"`，发送 `criteria` 对象 |
| `DirectionScore` | `answers.direction.choice` 与 `probabilities` |
| `DecisionReply::usage` | `usage.input_tokens` / `output_tokens` |

以上映射只属于 System One 适配器；`noul` 是该协议的 wire 类型，核心布尔结果为 double。其他适配器可使用不同报文，但必须返回相同含义的 DecisionReply。

不使用 choice 响应中的 `confidence` 代替类别概率。只校验消费所需字段、分数范围与必需类别；缺项不补零。服务错误与响应不合法统一返回 `DecisionError`，不静默重试。

当前服务只有一个处理槽，后端共享客户端串行请求。排队等待必须可取消且计入 10 秒；取得执行槽后，将剩余时间传给 HTTP 客户端。不能用不可取消的长时间 mutex 等待后再重新计时。主、子线程调用同一端口是安全的，实际 `HttpClient` 始终只有一个使用者。

## 5. 用户上下文与整轮共享开关

```cpp
// agent/decision_types.hpp
struct ReviewContext {
    std::vector<std::string> user_messages; // 主最多 3 条；委派辅助最多 2 条
    std::string delegated_task;            // 子会话的主要依据，主会话为空
    std::filesystem::path workspace_root;
    std::chrono::system_clock::time_point session_started;
};

enum class AutoReviewBlock { none, injection, scan_failed };

// agent/run.hpp；内部用短锁保护首次关闭原因。
class RunReviewGate {
public:
    bool enabled() const;
    AutoReviewBlock reason() const;
    bool disable(AutoReviewBlock); // 只允许从 none 转为关闭，返回是否首次关闭
};
```

成员接入：

| 对象 | 增量成员 | 生命周期 |
| --- | --- | --- |
| `Session` | 最近 3 条真实用户消息，供构造 `ReviewContext` | 独立于压缩后的 Conversation；由真实用户提交更新 |
| `RecoveryResult` | 最近真实用户消息 | 从原始用户记录重建，不从压缩摘要猜测 |
| `Run` | `shared_ptr<RunReviewGate> review_gate` | 主会话每次新的用户请求创建一个；所有子会话共用 |
| `DelegationContext` | `ReviewContext review_context`、共享 `review_gate` | 委派时取最近 2 条用户原话，主要任务设为本次委派文本 |

嵌套委派继续传递根用户原话；不能将父 Agent 委派文本当作新的用户原话。恢复会话时 `session_started` 使用原会话创建时间。

当前主、子会话共用 `TurnRunner::run()`，都会调用 `commit_user()`。新增 `UserInputOrigin { user, delegation }`，由 runtime 明确传入 TurnRunner/Committer 并写入 user 记录；子任务不更新真实用户消息缓存。旧 user 记录按会话 parent_id 判定来源：主会话为 user，子会话为 delegation，不能把来源不明的旧委派文本提升为用户授权。委派上下文另记入子会话初始化记录，恢复时保留辅助用户原话与主要任务；缺失时保持缺失，不使用摘要补造。

state 构造集中在 `agent/decision_review`：用户消息每条保留前 300 字、后 200 字；命令与 MCP 参数各最多 1000 字，均按 Unicode 码点处理并注明截断。路径、当前目标、完整 URL 和脚本证据单独成段，不能被命令截断规则截掉。完整命令始终保留在操作对象中，截断只作用于提交给 d3 的副本。

`disable()` 在扫描判定完成后立即调用，再返回工具结果；不能等工具结果按序写入历史才关闭，否则并行子 Agent 可能继续代批。关闭后 d3 代批与父复批都跳过；此前正在等待的自动审阅结果在消费前重新检查该开关。只读事实和已经完成的动作不因此回滚。

## 6. 目标识别与单文件证据接口

### 6.1 准备阶段：只记录目标事实

```cpp
// agent/intent.hpp
struct ScriptTarget {
    std::filesystem::path path; // 规范化绝对路径
    std::size_t command_index; // 对应解析出的简单命令，保持命令原顺序
};

// 增加到 CommandIntent：
std::vector<ScriptTarget> script_targets;
bool has_unresolved_execution = false;
```

`exec::Analysis` 提供相应的外围分析结果，由 `tools/bash.cpp` 映射成中立值。简单只读命令读路径写入现有 `PreparedIntent::paths`；不要再用 `impacts` 的说明文字反解析路径。

`has_unresolved_execution` 用于解释器动态参数、任务运行器或不能静态确定目标的执行片段。该标记不能被解释成“未修改的用户代码”。混合命令如 `python a.py && curl ...` 除逐个目标审查外，还需按普通命令规则判断其余联网动作，不能因存在一个未修改脚本而省略整个命令的 direction 判断。

静态识别只解析命令与路径，不读取脚本正文，不读取 npm/make 配置，不调用外部程序探测间接执行文件。确定性 mtime 事实可在准备时记录，但运行中复审以当时取得的状态为准。

### 6.2 运行中：按单个目标取得证据

```cpp
// agent/decision_types.hpp
struct ScriptEvidence {
    enum class Kind { unchanged, diff, full_text, too_large, unavailable };
    Kind kind;
    std::filesystem::path target;
    std::string text;          // 仅此目标的 diff / 全文；unchanged 时为空
    std::size_t content_lines;
    std::string reason;        // too_large / unavailable 的固定说明
};

// agent/action.hpp，PreparedTool 增加默认返回 unavailable 的虚接口：
virtual std::expected<ScriptEvidence, DecisionError> script_evidence(
    const ScriptTarget&,
    std::chrono::system_clock::time_point session_started,
    ReviewDeadline,
    std::stop_token) const;
```

`BashCall` 实现此接口，其他工具不实现。Dispatcher 只在运行中网络请求进入复审流程时调用；接口绝不能从 `prepare()` 或通用执行前审批中调用。

执行顺序：

1. 读取当前目标的 mtime。未晚于会话开始时间则返回 `unchanged`，不读正文。
2. 已修改且在 Git 仓库中：仅取该路径相对 HEAD 的差异；未跟踪文件整体按新增处理。不遍历仓库改动。
3. 已修改且不在 Git 仓库中：取得该文件全文。
4. 对单个目标计算行数。超过 300 返回 `too_large`，不截短后继续代批；无法取证返回 `unavailable`。

Git 取证不使用外部 diff/textconv。证据读取遵守既有读取边界；不能借复审扩大工具的文件访问范围。文件 I/O 与 Git 调用在 tools/workspace 层完成，Policy 仍是纯规则判断。

多个目标由 Dispatcher 顺序调用该接口、分别审阅。工作线程等待网络决定期间，接口只读取不可变的 BashCall 参数与文件系统，不修改正在执行的对象状态。

## 7. 审阅器与结果

```cpp
struct ReviewOperation {
    ApprovalIdentity identity;
    std::string root_turn_id;
    std::string tool;
    std::string command;
    std::string arguments;
    std::optional<NetworkTarget> network;
    std::optional<std::filesystem::path> path;
    std::string url;
};

enum class ReviewDisposition { approve, defer, unavailable, cancelled };

struct FailedCheck {
    ReviewScene scene;
    Dimension dimension;
    std::optional<Direction> direction;
    std::string target_id;
    double score;
    bool adjustable;
};

struct ReviewResult {
    ReviewDisposition disposition;
    std::string record_id;
    std::string reason_code;
    std::string reason;
    std::vector<FailedCheck> failed_checks; // 只在后端使用
};

class DecisionReviewer {
public:
    DecisionReviewer(DecisionServices&, const ReviewContext&);

    ReviewResult review_action(
        ReviewScene, const ReviewOperation&, ReviewDeadline, std::stop_token);

    ReviewResult review_script(
        const ReviewOperation&, const ScriptTarget&, const ScriptEvidence&,
        ReviewDeadline, std::stop_token);

    ScanResult scan_web(
        const ReviewOperation&, std::string_view result_text,
        ReviewDeadline, std::stop_token);
};
```

场景与合法输入由调用点固定：`outside_read` 必须带 path，`fetch_url` 必须带完整 URL，MCP 使用工具名和原始参数。`review_action()` 不接受 `script_diff` 或 `web_content`；后二者分别使用专门入口。

审阅器负责评分、阈值比较及独立审阅记录，**不返回 ExecutionGrant，不弹人工窗口，不调用父 Agent**。关闭整轮开关与最终路由仍由 Dispatcher 负责。

`review_script()` 的规则：

- `unchanged`：只问 relevance。
- `diff/full_text`：对当前目标问 relevance 与 net_change；明确不涉网时不问 direction，明确涉网时再问 direction，灰区转后续审批。
- `too_large/unavailable`：直接 defer，不向 d3 提交截断或伪造的证据。
- fetch/send 先满足相应类别概率阈值，再满足对应 relevance 阈值；modify/unknown 不代批。类别并列最高时按不确定处理。

逻辑上一次审批可能包含多个目标和两阶段请求。各 `ReviewResult` 有自己的记录 ID，Dispatcher 用 `review_ids` 关联到同一个 Approval；不把多文件 state 合并成一条请求。当前操作所有必要检查通过才批准；失败归因按整次 Approval 汇总，供动态阈值使用。

## 8. Policy 与审批编排

### 8.1 规则事实与最终批准分开

```cpp
// agent/permission.hpp
struct NetworkRules {
    enum class Match { denied, allowed, unmatched };
    Match match;
    std::uint64_t revision;
    std::string reason;
    // 命中既有规则时保留来源、身份及撤权 token，供执行登记。
    std::optional<ExecutionGrant> existing_grant;
};

// 取代 Dispatcher 中直接把 check_network().allow 当最终决定的使用方式：
NetworkRules inspect_network(const NetworkTarget&) const;
```

`allowed` 仅表示命中原有放行规则，不能绕过修改脚本复审或完整 URL 审阅。配置拒绝、会话拒绝仍优先。已有 `consume_approval()`、`consume_network_approval()` 保留，继续负责 revision、作用域及最终消费。

其他增量：

- `ApprovalAuthority` 增加 `decision_model`，实际模型（第一版为 d3）记入审阅记录；不为每个新模型增加一个枚举值。`GrantSource` 仍表达 mode/once/session/unrestricted，两者不混用。
- `Approval` 增加 `vector<string> review_ids`；切换到父复批或人工时保留这些 ID，最终 authority 改为真正做决定的一方。
- `Approval::Request::Kind` 增加 `web_request`，目标为完整 URL；URL 允许一次，不提供 URL 自动会话规则。
- d3 的网络/MCP 批准只允许 `allow`；工作区外普通文件可使用 `allow_session`，但消费后仅保存文件级读取规则。
- Policy 新增独立的精确 `read_files_` 规则，不借用当前 `read_dirs_` 的包含关系匹配。路径读取与 bash 的沙箱授权都使用同一精确文件规则。
- d3 不审 host_access、敏感文件、受保护写入等未纳入计划的审批，也不能覆盖硬拒绝。

审批消费还需明确 scope，不能只从 ToolKind 推导。给 `Approval` 增加 `ApprovalScope { operation, exact_file_read, network_target, web_request }`；原审批默认 operation，简单只读命令的外部读取与 read 工具均使用 exact_file_read。该分支只将批准的普通文件追加到 readable，bash 保持 read_only profile；不能落入现有 `grant_for(exec)` 的 workspace_write 分支，或 `remember_locked(exec)` 的整条命令会话规则。单次请求含多个文件时逐个判断，所有文件都满足才消费，任一敏感/控制面路径沿原规则处理。目录读取、grep/glob 的目录范围继续由原有审批处理，不用文件级 d3 批准替代目录授权。

`valid_approval()` 对 decision_model 单独校验允许的 scope 和 Answer；不能沿用当前“非 parent_model 一律进入普通批准分支”的条件。文件规则保存来源与审阅 ID，`session_grants()` / `revoke()` 增加独立的文件规则 ID，并由执行登记参与撤销处理。

### 8.2 运行中网络顺序

```text
工具运行中发起 host:port 请求
  → 检查停止状态及当前网络拒绝规则；拒绝立即结束本次判定
  → 无 d3 配置：走现有网络授权流程
  → 有配置且为 workspace：取得明确目标脚本的当前修改状态
      → 存在已修改目标：逐个基础审查，修改目标分别审 diff
          → 全部通过：保留本次审阅结果
          → 未通过/故障：进入后续审批，不使用已有 host 放行绕过
          → 整轮自动审批关闭：直接人工
      → 没有强制复审：已有网络放行规则可直接生效
      → 新目标：复用刚才全部通过的结果；否则执行必要的普通命令/脚本审阅
  → 未获 d3 批准：主会话人工；子会话父复批后人工
  → 消费前再次检查 revision、停止状态与根轮次开关
  → Policy 消费授权，登记来源，沙箱执行结果
```

取证失败不能解释为“未修改”；需要强制复审时 d3 失败、超时或关闭，不能退回已有 host 放行。取消与撤权直接取消旧请求，不转人工续批。

执行级缓存键沿用规范化 host:port，并保存授权来源、revision 和相关审阅 ID；只在当前执行内有效。查询缓存前先检查停止、拒绝和整轮状态。根轮次关闭后，尚未消费的 d3/父复批结果不能形成新授权；已经消费到当前执行的批准不回滚，不额外终止已经获批的执行。新的审批请求转人工，不能把旧缓存带到新执行或用于跳过新的强制复审。人工和配置已有规则按计划继续生效，但不免除强制脚本复审的人工替代。

`inspect_network()` 之外保留 `check_network()` 作为已有规则的纯查询兼容入口，供 `ExecutionRegistry::reconcile()` 使用；其中不触发审阅。配置、会话规则不能作为待复审脚本的 SRT 初始 allowedDomains 提前下发，否则会绕过宿主回调。沙箱只接收该次执行已经完成判定的授权。

### 8.3 父复批与 headless

- `ParentApprovalOptions::parent_when_unrestricted` 改为能表达 workspace 与 unrestricted 的选项，例如 `ParentReviewMode { disabled, unrestricted_only, workspace_and_unrestricted }`；旧配置值映射到旧语义，新默认使用后者。
- `AuthorityView::can_review()`、父审阅器内部有效性检查与 runtime 子路由使用同一模式判断；planning/read_only 上限保持原约束。
- `runtime/subagent.cpp` 将父拒绝、超时、模型失败转到真正的用户审批入口；父停止、请求取消或权限失效直接结束。嵌套子会话也只经过自己的直接父复批，再抵达根用户入口，不能沿祖先链重复复批。
- 在 `RunServices`、`ControlActionExecutor::Services` 与 `DelegationContext` 显式传递 `const Approver* user_approver`，它始终指向根 runtime 的人工交互出口。既有 approver 是组合路由，不能把它当作嵌套委派的人工出口。引用寿命覆盖子执行，headless 为空。
- 父复批转人工时保留请求/执行身份及 review_ids，但清除本次尚未批准的父审阅 authority_stop。父授权取消回调只在父批准实际被消费后安装；不能因一次失败的父复批，让后续人工批准依赖那个失败尝试。此前已消费父授权的其他动作仍保留原有撤权关系。
- 根轮次关闭时不进入父模型请求。父模型批准返回后、消费前也检查共享开关。
- Dispatcher 先尝试符合条件的 d3 审阅，再判断人工出口是否存在。headless 无人工出口时返回审批不可用，不阻塞等待。
- d3 defer、模型故障、父复批中间拒绝均不作为最终用户拒绝写入 host 会话拒绝规则。只有最终明确拒绝按现有策略记录。
- 放宽 workspace 父复批入口不等于允许它授予主机完全访问：host_access 仍只允许具备 unrestricted 且无只读/规划上限的父复批，否则转人工；未列入本计划的特殊资源审批保持原约束。
- `DerivedPermission::may_ask == false` 的子会话保留禁止申请额外权限的语义。该限制显式传递到决策编排，不能因为“先 d3、后检查人工出口”而给它新增权限；这与 headless 没有人工出口是两个不同条件。

## 9. 网页逐跳审阅与结果扫描

### 9.1 执行期回调

保持 `PreparedTool::execute()` 的签名，将执行期能力加到现有 `ExecutionGrant`；回调不持久化、不出现在前后端 DTO 中。

```cpp
// agent/grant.hpp；NetworkAction / NetworkDecider 沿用现有类型。
struct WebRequestIntent {
    std::string url;
    NetworkTarget target;
    std::size_t redirect_index; // 初始为 0
};

struct WebRequestAccess {
    NetworkAction action;
    std::string reason;
    // 绑定当前完整 URL 审阅结果的网络回调，仅供这一跳使用。
    NetworkDecider network_decider;
};

struct WebScanInput {
    std::string url;
    std::string result_text; // 本次准备交付的 fetch 结果
};

struct ScanResult {
    enum class Kind { clean, injection, unavailable, cancelled, bypass };
    Kind kind;
    std::string record_id;
    std::string message;
};

using WebRequestDecider = std::function<WebRequestAccess(
    const WebRequestIntent&, std::stop_token)>;
using WebResultScanner = std::function<ScanResult(
    const WebScanInput&, std::stop_token)>;

// ExecutionGrant 的新增成员：
WebRequestDecider web_request_decider;
WebResultScanner web_result_scanner;
```

`WebRequestAccess::network_decider` 捕获本跳的审批结果与身份；实际连接时仍检查拒绝规则和权限有效性，但不对相同操作再调用一次 d3。回调只接受当前 URL 对应的目标，跳转的新 URL 必须重新走 `web_request_decider`。

在 `web` 层使用不依赖 agent 类型的回调适配：

```cpp
// web/web.hpp；由 tools/web.cpp 包装 agent 回调。
using BeforeRequest = std::function<void(
    std::string_view normalized_url,
    std::size_t redirect_index,
    exec::SrtRequest& request,
    std::stop_token)>;

Response get(exec::SrtRequest, std::string_view url, const Options&,
             exec::Options, const BeforeRequest&, std::stop_token = {});
```

`BeforeRequest` 在发送前审阅 URL，并替换该跳请求的 network_gate；拒绝/取消按现有 web/exec 错误转换返回，不能继续发送。`web_search` 使用原有基础设施网络规则，不挂 URL d3 审阅和结果扫描。

### 9.2 重定向

`web::get()` 去掉自动 `--location`，每次运行一跳并取得响应头；解析 Location、规范化下一跳后再调用 `BeforeRequest`。维持 HTTP(S) 限制、原有 10 次跳转上限、整体截止时间和下载大小限制。相同 host 的不同 URL 同样审阅，不用 host 缓存代替 URL 决定。

未配置 d3、ask 或 unrestricted 时，回调走相应现有权限行为，不调用 d3。web_fetch 的实际网络传输始终保留现有只读 SRT 边界。

### 9.3 结果扫描

```cpp
// agent/decision_review.hpp
struct ScanSlice {
    std::size_t begin; // 原 fetch 结果中的 UTF-8 字节偏移
    std::size_t end;
};
struct ScanSample {
    std::string text;
    std::vector<ScanSlice> slices;
};
ScanSample sample_fetch_result(std::string_view result);
```

严格执行计划的规则：结果不超过 16 KB 时全检；超过时头尾各 4 KB，另在前后指定区间各随机取完整落在区间内的 4 KB 窗口。每次重新随机，按原顺序用 `…[省略]…` 拼成一次请求。实现统一按 KB=1024 字节处理，切片边界向内调整到有效 UTF-8；正文抽样额度不因插入结构性省略标记而改变。审阅记录保存实际 slice 范围和发送的 state，便于后续校准复现。

工具完成格式化、输出预算处理后，将本次准备返回的 fetch 结果交给 `web_result_scanner`；缓存命中和分页同样执行。扫描前不得通过 ToolOutput 提前发送正文。

| ScanResult | 工具结果与根轮次处理 |
| --- | --- |
| clean | 原结果交付模型 |
| bypass | 仅在当前模式不扫描或从未配置 d3 时使用，沿用原结果 |
| injection | Dispatcher 关闭整轮及子 Agent 自动审批；工具只返回固定提示 |
| unavailable | 同样关闭整轮自动审批；工具只返回扫描失败提示 |
| cancelled | 按工具取消处理，不伪装成注入或用户拒绝 |

发现注入或扫描失败时同时替换 `ToolResult::model_text` 和 `WebView::output`，避免正文通过另一展示/记录字段泄漏给模型。缓存可保留下载内容，但不保存可复用的“已扫描通过”状态。

已关闭整轮代批不等于关闭后续结果扫描：后续 fetch 仍需扫描才能交付正文，正常扫描不能重新打开自动审批。已配置但缺少有效 injection 阈值时按扫描不可用处理，不把未经判断的正文标成 clean。

## 10. 线程、回调与取消

沿用现有 `ExecutionMailbox`：工具工作线程提交网络、URL、扫描请求，所属 Dispatcher 线程执行审批编排、提交会话记录，再返回结果。不能在工作线程直接写 Conversation 或调用 SessionCommitter 的普通提交路径。

- 每个回调拥有自己的请求数据、promise 和停止 token；执行槽位及 PreparedTool 在回调结束前保持存活。
- 网络、URL 和扫描回调安装在串行工具执行路径。现有并行只读工具组继续不携带新增审批能力；需要运行中审批/扫描的工具不加入该组。
- 回调等待期间不持有 Policy、共享轮次或存储事务锁。d3 排队与 HTTP 等待均可被执行取消唤醒。
- 根轮次关闭只用短锁更新共享状态；授权消费前取最新快照，关闭后返回的自动批准不生效。若实现需要与消费严格串行化，用同一短临界区检查开关和消费，锁内不调用网络或写记录。
- 父 Dispatcher 等待 task 时继续处理现有父审阅 mailbox；子审阅不能直接重入父 Session 的模型请求。
- 所有回调必须走到唯一终态；超时、异常或取消均回送结果并释放等待，不增加后台无限重试任务。

## 11. 阈值、审阅存储与人工反馈

### 11.1 阈值类型

```cpp
struct ThresholdKey {
    ReviewScene scene;
    Dimension dimension;
    std::optional<Direction> direction; // fetch / send 分开
};

struct AdaptiveThreshold {
    enum class Pass { at_least, at_most };
    Pass pass;
    double strict_value;
    double relaxed_value;
    double current_value;
};

struct ThresholdProfile {
    ReviewVersion version;
    std::uint64_t revision;
    bool calibrated;
    std::map<ThresholdKey, AdaptiveThreshold> adaptive;
    // 只供相应场景使用，不参与动态调整。
    std::optional<double> net_change_low;
    std::optional<double> net_change_high;
    std::optional<double> injection_limit;
};
```

实际实现为 map 键补齐比较操作即可。`relevance` 与 fetch/send 类别概率使用 at_least，privacy 使用 at_most；net_change 两界限与 injection 固定。严格端和宽松端不按数值大小命名。

每次逻辑审阅先读取阈值快照，各阶段使用同一快照，记录其 revision。无适用阈值时不猜默认数值；开发期必须显式选用临时 profile，不能自动变成正式配置。

### 11.2 存储端口

```cpp
struct DecisionAttempt {
    ReviewScene scene;
    std::string target_id;
    DecisionRequest request;             // 实际发送的 state 与问句
    std::optional<DecisionReply> reply;
    std::optional<DecisionError> error;
    std::chrono::milliseconds elapsed;
};

struct DecisionReviewRecord {
    std::string id;
    ApprovalIdentity identity;
    std::string root_turn_id;
    ReviewVersion version;
    ThresholdProfile thresholds;
    std::vector<DecisionAttempt> attempts; // 同一目标的基础/涉网两阶段
    ReviewResult result;
};

struct ReviewFeedback {
    std::string approval_request_id;
    std::vector<std::string> review_ids;
    ApprovalAuthority final_authority;
    Decision final_decision;
};

class DecisionReviewStore {
public:
    virtual ~DecisionReviewStore() = default;
    virtual std::optional<ThresholdProfile> thresholds(const ReviewVersion&) = 0;
    virtual void append(const DecisionReviewRecord&) = 0;
    virtual void finish(const ReviewFeedback&) = 0;
};
```

实现使用 Home/data 下独立 SQLite 数据库，例如 `data/decision.sqlite`，与主会话数据库分开。错误沿用存储层异常风格，由审阅入口统一转换；无法保存审阅与必要关联时不继续生成无法追溯的 d3 批准，退回后续审批并给出一次说明。

建议三类数据：

| 数据 | 主要内容 |
| --- | --- |
| reviews | 审阅 ID、会话/调用/执行/目标关联、实际 state、问句与模型版本、分数、阈值快照、结果、固定理由、耗时 |
| feedback | 最终审批请求 ID、关联的全部审阅 ID、最终来源和人工决定；审批 ID 唯一，避免重复更新 |
| thresholds | 版本与 profile 键、场景/维度/方向、严格端/宽松端/当前值及 revision |

`finish()` 在一个事务内记录最终反馈并更新符合条件的阈值：

1. 只处理用户最终明确批准或拒绝；父模型、d3、取消、失效、无人工出口不调整。
2. 汇总本次 Approval 关联的所有审阅，只在恰好一个可调整检查不达标、且分数处在端点之间时更新。多个脚本分别失败也算多个检查，不能各自调整后变成一次用户决定更新多处。
3. 批准向宽松端走一步，拒绝向严格端走三步，步长为两端距离的 1/10，并限制在端点内。
4. 固定阈值、modify/unknown 类别、不完整审阅或证据超长等不参与动态反馈。
5. 人工决定不能应用到不同模型/问句/profile 的新版本。多个后端共用 Home 时仍以事务读取当前值并更新，避免旧快照覆盖新值。

`SessionCommitter::commit_permission()` 后由 Dispatcher 调用 `finish()`；父复批的中间结果不调用最终反馈入口。

SessionCommitter 当前会吞掉记录写入异常并设置 broken；因此上述“之后”不代表主会话记录一定已落盘。DecisionReviewStore 的反馈事务以唯一 approval_request_id 去重，不依赖另一数据库的事务成功，也不承诺两个数据库原子提交。批准已经实际消费后，反馈保存失败只记录一次故障、跳过该次阈值更新，不能反过来把已执行的批准记为拒绝或重新执行操作。重启后不得从不完整记录自动重放人工反馈。

## 12. 前后端记录与显示

新增轻量事件，表示已经完成的一次自动审阅，不把 d3 的 defer 冒充一次最终拒绝：

```cpp
struct AutoReviewFinished {
    std::string call_id;
    std::vector<std::string> review_ids;
    std::string result; // approved / needs_approval / unavailable / cancelled
    std::string reason;
};
```

事件经 `SessionCommitter` 统一提交，加入 `Event`、DTO、后端转换、历史编解码与 UI 投影；界面标签固定为 `auto-review`。此事件和会话 permission 中的 d3 摘要只保留来源、结果、理由和审阅 ID，不复制 state、diff 或分数。现有执行身份与关联字段继续保留。

决策模型直接批准时，permission 的 authority 为 `decision_model`，实际模型身份由关联审阅记录解析。最终由父或用户批准时，authority 应为 `parent_model` 或 `user`，同时保留前序审阅 ID；不能因经过 d3 就把最终审批来源写成 decision_model。工具调用自身的既有记录格式保持原用途。

## 13. unrestricted 首次开启确认

确认必须发生在实际升权之前。现有 `Runtime::cycle_permission()` 会立即切换，需要拆出“请求切换”和“确认后应用”，避免先升权再由 UI 补弹窗。

```cpp
// runtime 层中立结果；对接现有 RPC。
struct PermissionChangeResult {
    agent::PermissionMode effective_mode;
    bool confirmation_required;
};

std::expected<PermissionChangeResult, RuntimeError> request_permission(
    agent::PermissionMode target, bool confirmed = false);
```

- `session.cycle_permission` 计算目标后调用此接口；首次目标为 unrestricted 时设置 confirmation_required，当前权限不变。RPC 继续返回完整 SessionSnapshot，新增可选 pending_permission_confirmation 字段；不把现有返回值替换为两字段小对象。
- 新增显式目标的 `session.set_permission`，前端确认后传 `target=unrestricted, confirmed=true`，避免再次 cycle 导致目标变化。请求沿用现有会话身份校验。
- 确认文案固定使用计划 §3.1；默认焦点取消。Shift+Tab 路径取消后显式切到 ask。
- CLI/配置以 unrestricted 启动 TUI 时，后端先以 workspace 建立可用会话，将 unrestricted 作为待确认启动目标；取消后留在 auto-edit，不能在弹窗前执行用户请求。
- 用户确认状态按会话保存，可用轻量 `unrestricted_ack` 记录恢复；切模型保留，新会话重置。这只是警告确认记录，不恢复旧执行授权。
- headless 在真正启用前向 stderr 输出同样警告，不等待确认，不向 stdout 写警告；不把它记录成用户已确认，之后首次 TUI 进入仍需确认。
- 已确认会话再次进入不弹窗，保留 error 色状态提示。此功能与 d3 是否配置无关。

pending_permission_confirmation 记录目标与入口（startup/cycle），以保证两种取消回退不同；不保存正文提示，由 UI 使用固定文案。确认响应必须匹配当前 session_generation，切换/新建会话时清除旧待确认状态。待确认期间 `input.submit` 可以排队，但 runtime 不得启动执行；确认或取消完成后再按生效模式执行。前后端协议常量共同升级，保持当前 hello 的严格版本匹配，防止旧 UI 忽略新字段后静默按另一模式运行。

## 14. 接入顺序与完成边界

| 计划阶段 | 本草案的接口交付 |
| --- | --- |
| P1 | app/model_config 配置模块、决策模型目录与选择、DecisionModel、System One 适配器及可取消请求 |
| P2 | ReviewContext、DecisionReviewRecord、ReviewStore、轻量事件 |
| P3 | NetworkRules、decision_model authority、Dispatcher 路由、父复批/headless |
| P4 | 简单命令读取路径、read_files_ 与精确文件授权 |
| P5 | ScriptTarget、script_evidence、逐目标基础审查与运行中 diff 复审 |
| P6 | WebRequestDecider、BeforeRequest、WebResultScanner、RunReviewGate 联动 |
| P7 | PermissionChangeResult、确认前不升权、TUI/headless 两条入口 |
| P8 | 阈值快照、唯一人工反馈、跨会话事务更新 |
| C1 | 功能实现后，使用真实服务共同校准正式阈值 |

最低真实运行验收沿用主计划，并特别覆盖：准备/执行前不发脚本 d3 请求；修改脚本访问已允许 host 仍复审；多个目标逐个审且不读取依赖；URL 每跳审核；扫描失败不返回正文且子 Agent 直接人工；无 d3 保持原行为；headless 可代批；取消/撤权后的旧结果不生效。

本次交付仅为接口文档，不运行构建、不新增测试代码或模拟服务。后续实现按项目约束使用构建与真实功能运行验收，临时材料仅放 `temp/`。

## 15. 与现有代码的兼容性审查

审查日期：2026-10-17。范围为草案与当前源码的接口、数据和行为衔接；本节列出的风险指直接按先前草案实现可能发生的问题，不表示未实现的新功能已经通过运行验证。

### 15.1 必须随功能一起修改的地方

| 优先级 | 现有代码依据 | 草案落地时的兼容风险 | 已确定的处理 |
| --- | --- | --- | --- |
| 高 | `agent/permission.cpp`：known_readonly 提前放行；末尾 workspace 对 exec 直接放行；grant_for/remember_locked 按 ToolKind 分流 | 外部读取可能跳过审批，或批准后仍读不到文件；也可能升级成 workspace_write 或整条命令会话许可 | 外部路径判断先于 known_readonly/auto-edit 快捷放行；增加 exact_file_read scope，批准精确文件并保持只读沙箱 |
| 高 | `agent/permission.cpp`：valid_approval、to_string 与记录 summary 将来源分成 parent_model/其他 | 新 decision_model 来源可能被误记为 user，或绕过范围约束 | 所有来源分支改为明确枚举处理；新增来源独立校验，不用非父模型分支兜底 |
| 高 | `runtime/subagent.cpp`：父结果直接返回，提前安装 parent_review_stop_；`agent/control.cpp` 传递组合 approver | 父拒绝后无法转人工；转人工后仍被失败的父审阅 token 撤销；嵌套时可能再次走祖先模型 | 独立根 user_approver，区分普通拒绝与取消；只对已消费的父授权建立撤权依赖 |
| 高 | `agent/record_codec.cpp`：permission 接受 schema 2/3/4；parent_review 只接受 4；未知 record type 报 corrupt | 新字段/新来源/确认记录若直接落库，恢复和历史读取会失败 | 新 schema 与类型有明确解码分支；保留旧版分支，新读旧，不依赖未知类型静默跳过 |
| 高 | `agent/turn_runner.cpp`：主子共用 commit_user；`agent/recovery.cpp` 用同样 UserRecord 恢复 | 委派任务被误算为真实用户原话，压缩或恢复后审阅依据变化 | 显式 UserInputOrigin；真实用户缓存独立于 Conversation；初始化记录保存委派上下文 |
| 高 | `agent/execution_registry.cpp` 仍调用 check_network；`exec/srt.cpp` 将 network_targets 放入 allowedDomains | 改网络接口后撤销失效，或预放行目标绕过脚本复审 | 保留纯规则查询供 reconcile；规则放行与已完成审查的执行授权分开 |
| 高 | `backend/backend.cpp`：cycle 返回快照且 hello 严格匹配版本；`ui/shell.cpp` 直接解码快照 | 返回小对象导致 UI 使用缺省模式；启动弹窗前队列可能开始执行 | 增量快照字段、完整 DTO 往返、待确认执行闸门、共同升级协议版本 |
| 中 | `app/configuration.cpp` 使用 ModelCatalog::selection_log；模型覆盖由 @model= 顺序处理 | 抽取配置时遗漏字段或改变 --model 语义，生成模型切换退化 | 草案已补 selection_log；保留 --model 的选配置/覆盖模型 ID 双语义；决策选择使用独立字段 |
| 中 | `app/config.cpp` 存在 known_key、模型加载与配置投影；bootstrap/session_assembly 装配具体对象 | 只新增 JSON 示例会产生 unknown-key 警告或丢掉决策目录；切换生成模型时误重建/切换决策模型 | 一并更新主配置解析、已知键、Config、bootstrap、SessionAssembly；DecisionServices 按后端快照持有 |
| 中 | `ui/projection.cpp` 忽略未知实时事件；history/backend convert 使用明确 Kind；RunOutput 只识别现有事件 | 新 auto-review 实时不显示、恢复后消失，或只有 jsonl 有结果 | 接通 Event 序列化、HistoryItem、前端投影和 headless 输出；避免与现有 permission Notice 重复显示 |
| 中 | `cmake/install_home.cmake.in` 从 example 安装且不覆盖已有文件；home/config/config.json 显式旧父审批模式 | 修改默认值不改变旧主机配置；安装后也未必具备决策配置 | 随附示例默认 decision=null；旧文件不覆盖；新增父审批模式用新字符串，升级说明列出显式启用项 |
| 中 | `SessionCommitter` 写入失败设 broken；新审阅库与主库分离 | 误把两库写入当作原子操作，造成重复反馈或已批准操作被误报拒绝 | 独立 feedback 去重事务，允许故障时少一次校准数据；不重新执行、不补造人工决定 |

### 15.2 历史记录与版本约定

| 数据 | 新写入约定 | 旧数据读取约定 |
| --- | --- | --- |
| permission / parent_review | schema 5，增加 review_ids 与显式审批 scope；decision_model 分支写必要身份与轻量摘要 | 保留 permission 2/3/4 与 parent_review 4 的原解码；旧记录 review_ids 为空 |
| tool_started | schema 4，表达新增来源及必要的审阅关联；不序列化任何回调 | 保留 1/2/3 的解码，不从记录恢复权限 |
| auto_review | 新类型，schema 1，记录轻量结果，不插入模型 Conversation | 老会话没有此类型，不产生空白占位或重新调用模型 |
| unrestricted_ack | 新类型，schema 1，记录该会话用户确认事实 | 缺失表示未确认；不能根据旧 mode 或历史批准反推已看过警告 |
| delegation_context | 新类型，schema 1，包含主要任务与最多两条辅助用户原话 | 旧子会话缺失时不从委派 user 记录猜用户授权 |
| user.origin | 现有 user 记录增加 user/delegation 来源字段；不改变消息 ordinal | 缺失按会话身份判定；SessionRecovery 显式接收会话元信息 |

所有新类型进入 `record_codec::DecodedRecord`，同时处理 `SessionRecovery`、`HistoryCursor` 和 `HistoryItem` 投影。轻量审阅记录保持独立于 user/assistant/tool 的消息编号与压缩切点；不要因为新增审计记录破坏恢复时的 ordinal 校验。unrestricted_ack 与 delegation_context 参与各自元信息恢复，auto_review 仅回放展示。

新版保证读取项目目前支持的旧记录格式；旧可执行文件不保证读取新 schema。前后端必须同版本升级，不增加双协议服务或替旧程序伪造旧来源。独立 decision.sqlite 暂时缺失时，历史页仍可用会话摘要展示，不能因缺少审阅详情阻断整个会话恢复。

新增来源时同步检查 `grant` JSON、`permission_summary()`、拒绝文案、usage_owner 和 UI 标识。决策模型用量留在独立审阅记录，不加到主生成模型 token 预算；父模型审阅继续沿现有父侧记账，不能重复计费。

### 15.3 模型配置与安装约定

- `app/model_config` 抽取现有实现，而不是复制另一份 Node/密钥/写锁逻辑；只把必要的配置解析原语提为 app 内共用帮助函数，不建立通用配置框架。
- `load_models()` 保留 default、非空生成模型目录、@model= 覆盖顺序和 selection_log。decision_models 缺省可为空；decision 为 null 时不尝试模型连接。sessions、models、runtime 等查询/维护命令不创建决策 HTTP 连接或执行决策推理。
- `Configuration::models()`、provider_kinds、add_model、resolve 和子 Agent def.model 仍指生成模型。决策配置不能混入生成模型 UI 或被 --model 误选；第一版决策选择按第 3 节通过配置和重启生效。
- 配置写入保留整份 models.json 的非目标条目及原始凭据表达式；不能把已经解析的其他模型密钥重新回写成明文。现有 add_model 已保留文档其他字段，抽取时保留此行为。
- 新父审批配置字符串确定为 `parent_when_workspace_or_unrestricted`；旧 `parent_when_unrestricted` 与 `user` 继续保持原语义。随附默认可改新值，已安装用户配置不自动覆盖；主机要启用新的父复批范围时更新该项。
- `HomePaths` 增加 decision_database，统一生成 `data/decision.sqlite`。配置及数据库错误通过现有后端错误出口呈现，不在核心拼 Home 路径。
- CMake 将新源码分别放回 dagent_app_config、dagent_llm、dagent_agent、dagent_storage 等原目标，保持 agent 不链接具体 llm/storage。避免为了配置模块增加循环依赖或新的测试目标。

### 15.4 运行路径与权限边界

1. **read-only/plan 与 headless 分开判断。** headless 只是无人机交互出口，不等于禁止自动审批；may_ask=false 是子权限限制，决策模型也不能突破。workspace 下原本允许的普通读取和网页研究可接入决策流程，硬拒绝仍先返回。
2. **审批阶段与最终权限分开。** 父复批超时、预算耗尽和普通拒绝可转人工；执行 stop、会话替换或权限 revision 失效不可续批。每个阶段使用自己的 deadline，并服从整个工具/网络请求的剩余时间，不因转人工而重置总超时。
3. **会话规则要可撤销。** read_files_ 区分 `read-file-*` 与既有目录规则 ID；列表、撤销、授权消费和运行中 reconcile 一并更新。精确文件的来源、request_id 和 review_ids 不能在复用时丢失。
4. **MCP 原有显式授权继续有效。** 决策模型只生成本次调用许可；已有用户会话许可按原作用域匹配，不能被新增决策代码扩大。规划/只读下原来禁止的 external 调用不能由决策模型改成允许。
5. **根轮次关闭不是全局撤销。** 关闭影响之后尚未获批的自动审批；已消费到执行中的授权按原生命周期结束。不能承诺仅靠 Dispatcher 缓存就能中断 SRT 内已建立或已授权的连接；本计划不增加这种中断功能。
6. **SRT 回调不得提前绕过。** 已允许 host 的修改脚本联网仍需到达 Dispatcher。第一版只保证每次执行首次相应授权时复审，沿用同次执行 host:port 批准的作用域；不新增脚本运行中版本追踪。
7. **网页接口保留旧调用入口。** `web::get` 保留不带 BeforeRequest 的 overload，委托到新实现；web_search 仍使用 SearXNG 基础设施授权，不承担 URL 决策或正文扫描。缓存分页只扫描结果，不凭缓存 URL 制造新的网络审批。
8. **逐跳下载不重置预算。** 每跳发送前、审批后都重新计算整体剩余时间；累计跳转和下载预算，不让 10 跳变成 10 份工具超时。3xx Location 必须在最终非 2xx 错误转换之前处理；最终 url/status/content_type、压缩解码、UTF-8 offset 和截断语义沿用当前工具契约。
9. **unrestricted 启动覆盖所有入口。** 新会话、恢复、continue、显式模式切换与配置启动均走同一确认状态。当前 exit_plan 仅切到 workspace/ask，不需新增 unrestricted 分支。headless 警告直接走前端 stderr，不能借普通 Notice 同时写入 jsonl stdout。

### 15.5 合并前的真实兼容验收

以下是实现完成后的真实运行要求，本次不执行、不创建测试代码：

- 使用旧 models.json 和旧会话启动，完成 --model、模型添加、切换生成模型、恢复和历史查看；加入决策配置后这些路径仍有效。
- 新版恢复旧 schema 的主、子会话；新会话产生 auto_review、人工接管与 unrestricted_ack 后，重启能恢复展示与正确上下文，不恢复旧执行许可。
- 实际读取工作区外两个普通文件：首次批准范围精确，另一个文件仍需独立批准；bash 保持只读，文件规则可撤销，目录/敏感文件保留原审批。
- 真实子任务完成父拒绝→人工批准、父超时→人工、执行取消不续批，以及根轮次关闭后跳过父审阅；核对每次最终用户决定只更新一次阈值。
- 修改目标脚本真实访问预允许 host，确认仍发生复审；撤销会话网络规则后依赖它的执行按现有规则终止。该项需真实 SRT 运行，源码审查不能证明其底层缓存行为。
- TUI 的模式循环与启动确认、headless 的 text/json/jsonl 输出分别运行，确认警告、审批来源和 auto-review 展示正确。
- 真实网页完成重定向、缓存分页、扫描失败与取消；真实 MCP 调用保持原许可与只读限制。所有临时材料放 temp/。
