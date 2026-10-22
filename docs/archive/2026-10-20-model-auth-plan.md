# 模型登录与请求头计划

设计日期：2026-10-20。状态：已完成（TUI 改版第 3 期）。交互预览：[TUI 预览](https://claude.ai/artifact/Lrbc8enKxAV8Nm1xw5Qf12)。

## 1. 目标

1. 在 TUI 的 `/model` 中选择 OpenCode Go：浏览器批准登录后直接选择 Go 的模型，不手工复制密钥。
2. 选中的模型写入 `models.json` 保存；TUI 只负责让选择更方便。
3. 登录凭据自动刷新，多个 DAgent 进程同时使用时不会因刷新令牌重复使用而导致登录被吊销。
4. 模型请求头作为通用能力，规则明确、可校验、不泄露。

## 2. 背景

- OpenCode Go 推理接口为 `https://opencode.ai/inference/go/openai/v1`（部分模型走 `…/go/anthropic/v1`），拒绝不带 `x-opencode-session` 的请求，要求客户端使用自己的 User-Agent。
- OpenCode Console 公开 OAuth 设备授权（RFC 8628）：`POST https://opencode.ai/console/auth/device/code`（`client_id`、`supports_org_scope=true`），轮询与刷新使用 `…/auth/device/token`。
- 实测（2026-10-20）：`client_id=dagent` 可以登录，令牌可直接调用 Go 推理；access token 有效期 30 天；刷新后 refresh token 轮换、工作区不变，旧 access token 立即失效；旧 refresh token 重复使用会吊销整个登录。
- 当前分支 `feat/model-headers` 已有 `models.json` 的 `headers` 与 `{session_id}` 的初版实现，本计划在其上重做。

## 3. 使用方式

### 3.1 TUI

- `/model` 面板在已配置模型之后增加「Providers」分区，列出登录提供方。OpenCode Go 一行显示登录状态：未登录（`sign in`）、已登录（工作区、到期时间）、已失效（`sign in` 重新登录）。
- 未登录时选中：前端用 `xdg-open`/`open` 打开授权页面（以 argv 启动，失败则只显示），弹出登录框显示授权链接、用户码、剩余时间与 Go 订阅链接；`c` 复制链接，`o` 重新打开，`Esc` 取消。批准后自动进入模型列表。
- 已登录时选中：显示 Go 模型列表，每行为模型名、上下文长度、输入/输出价格（免费模型标 `free`），已添加的标 `added`；可搜索，`r` 刷新清单。
- 选中模型：写入 `models.json` 的 `opencode-go/<模型名>` 条目并切换到该模型；已存在则直接切换。
- 使用登录的模型在登录失效时报错并提示到 `/model` 重新登录，面板中这些模型标 `needs sign-in`。

### 3.2 命令行

- `dagent login opencode`：打印授权链接与用户码，尝试打开浏览器，等待批准，成功后显示工作区与到期时间。
- `dagent login`：列出提供方的本地登录状态（未登录、已登录及到期时间、已过期、需重新登录），不联网、不刷新。
- `dagent logout opencode`：删除本地凭据。
- `--list-models` 的 KEY 列改为 AUTH 列（`key`、`login:opencode`、无）。

### 3.3 models.json

```json
"opencode-go/deepseek-v4.1-flash": {
  "kind": "openai-chat",
  "base_url": "https://opencode.ai/inference/go/openai/v1",
  "model": "deepseek-v4.1-flash",
  "login": "opencode",
  "context_window": 1000000,
  "max_tokens": 32768,
  "send_reasoning_content": true
}
```

- `login` 与 `api_key` 互斥，按字段是否存在判断；同时出现（含 `null` 或空字符串）为配置错误。
- 配置加载只校验引用：提供方存在、协议与地址在该提供方允许范围内；不读取凭据。未登录不影响其它模型，也不影响登录命令。
- 使用登录的模型自动带提供方默认请求头（OpenCode：`x-opencode-session: {session_id}`），条目的 `headers` 可追加或覆盖非保留头。

## 4. 登录提供方

- 内置提供方表，第一版只有 `opencode`：设备码与令牌端点、`client_id`（`dagent`）、附加参数、批准页面基础地址、**允许接收令牌的 HTTPS origin 与路径**、支持的协议 kind、默认请求头、模型清单来源。新增提供方即新增一项，只支持 RFC 8628 设备授权。
- 带登录令牌的请求只发往允许的 origin：检查最终编码后的 URL，禁止 userinfo，不用字符串前缀判断域名；强制校验 TLS 证书，不继承可关闭校验的配置；不跟随重定向。
- 设备授权流程：缺少 `interval` 时为 5 秒；`slow_down` 后续间隔每次加 5 秒；拒绝、过期及其它 OAuth 错误终止；批准链接按提供方 origin 解析并校验；等待、轮询可取消。

## 5. 模型清单

- 来源为 models.dev 公开数据中的 `opencode-go` 提供方：模型名、上下文与输出上限、价格、接口类型。缓存到 `home/cache/`；超过 24 小时打开列表时刷新，`r` 强制刷新，无网络时使用缓存。
- 接口地址不在允许范围内的模型不显示。Anthropic 接口的模型在实测确认令牌可用后开放，确认前在列表中标为不可用。
- 写入的条目：`kind` 与 `base_url` 按接口类型；`context_window` 取清单上下文；`max_tokens` 取清单输出上限与 32768 的较小值；`send_reasoning_content` 按清单的推理标记设置并实测确认。

## 6. 凭据存储

- 凭据：`home/data/credentials/opencode.json`，目录 0700、文件 0600，字段 `access_token`、`refresh_token`、`expires_at`（绝对时间）、`org_id`、`client_id`、`state`（`ready` 或 `refresh_pending`）、`updated`。
- 锁：`home/run/credentials/opencode.lock`，固定存在、不随登出删除；登录、刷新、登出使用同一把锁。
- 写入：临时文件从创建起 0600，写完 `fsync`、rename、`fsync` 目录；拒绝符号链接、属主不符和非普通文件。
- 每次请求读取凭据文件，不做进程内缓存。
- 凭据位于控制根 `data/`，受控制路径规则保护；该规则允许人工批准读取，`unrestricted` 模式也会放行，因此不承诺绝对不可读。

## 7. 刷新与 401 恢复

- 距到期 5 分钟内，或推理返回 HTTP 401 时刷新。
- 刷新在锁内进行：重读文件，重新判断状态与到期时间；若 access token 已与本次所用不同（其它进程已刷新），直接使用新值。否则先写入 `refresh_pending`，再请求刷新，成功后原子写入新令牌并恢复 `ready`。
- 读到 `refresh_pending`：表示上次刷新结果不确定，提示重新登录，**不自动重发旧 refresh token**。刷新返回 `invalid_grant` 等错误同样提示重新登录。
- 401 恢复只对使用登录的模型启用，每次 `complete()` 最多一次，额度不随网络重试重置；在调用 `classify_http()` 前按状态码识别。403 不刷新；408、429、5xx 与流中断仍走现有重试。重发保持正文与会话 id，并与 `ModelAttemptHooks`、预算计数一致。
- 锁等待、轮询、刷新都可取消并有自己的超时；认证请求有响应体上限。刷新只串行化刷新本身，不锁住推理请求。

## 8. 请求头

- 来源与顺序：协议内置头 → 登录提供方默认头 → 条目 `headers`；同名按 ASCII 大小写不敏感合并，后者覆盖前者。同一配置对象中仅大小写不同的重复键为配置错误。不支持多值头和删除头。
- 保留头：传输控制头（`Host`、`Content-Length`、`Transfer-Encoding`、`Connection`、`TE`、`Trailer`、`Upgrade`、`Expect`、代理认证头）全局禁止配置；`Content-Type`、`Accept` 由协议拥有；`anthropic-version` 在 Anthropic 协议下保留；由 DAgent 管理的认证头（`api_key` 或 `login` 生成的 `Authorization`、`x-api-key`）不得覆盖。没有托管认证的协议（如 Ollama）可以通过 `headers` 自行配置认证头。
- 取值：`${ENV}` 在加载配置时展开；`{session_id}` 每次请求替换；其它花括号原样保留。展开与替换后都校验：头名为非空合法 token；值不含 CR、LF、NUL 和其它控制字符。使用 `{session_id}` 却没有会话身份时报错，不发送空值。
- 会话 id：主回合、压缩使用所属会话；父审阅使用执行审阅的父会话；子 Agent 使用自己的会话；d3 审阅使用被审阅动作所属的会话（`DecisionRequest` 从 `ApprovalIdentity` 取得）。
- `User-Agent` 为 `DAgent/<构建版本>`，只在 HTTP 层设置一处。

## 9. 脱敏

- 每次请求的敏感值（实际使用的令牌、静态 key、展开后的请求头值）统一脱敏，在截断、事件外发与写日志之前处理，覆盖 `classify_http()` 与流内错误（`Finish::raw`）两条路径。
- OAuth 错误只显示状态码、受控错误码与固定说明，不转发响应正文；JSON 解析异常不透传原文。
- 请求头的值与令牌不出现在 UI、日志、`workspace.info`、会话记录中；公开模型信息只给出认证来源。

## 10. d3 决策模型

- `decision_models` 支持通用 `headers`，与生成模型共用解析与合并函数，会话 id 见第 8 节。
- 暂不支持 `login`：配置了即报错。

## 11. 实现位置

| 部分 | 位置 |
| --- | --- |
| 提供方表、设备授权、凭据读写、锁、刷新、允许的令牌地址 | `src/public/llm/auth.hpp`、`src/private/llm/auth.cpp` |
| 每次尝试取得认证快照、一次 401 恢复 | `src/private/llm/model.cpp` |
| 请求头合并、校验、脱敏 | `provider_detail.hpp`、`provider.cpp`、各 `provider_*.cpp` |
| 配置：`login`、`headers` 解析与校验，`decision_models` 的 `headers` | `src/private/app/config.cpp` |
| 凭据与锁路径 | `HomePaths`（`app/home.hpp`） |
| 登录、登出、状态、模型清单的后端操作 | 新增 `src/private/app/login.cpp`，后端协议增加「开始登录 → 返回链接与用户码 → 等待结果」及清单查询 |
| 命令行 | `cli.hpp`、`cli.cpp`、`main.cpp`（展示链接、打开浏览器、取消） |
| TUI | `shell.cpp` 的 `/model` 面板、新增登录框、Go 模型列表 |

## 12. 不做

多账号、可配置的授权端点、通用认证插件、d3 的 `login`、TUI 之外的 `/login` 命令。

## 13. 验收

只做构建检查与真实功能运行，检测放在 `temp/`：

1. TUI 中从 `/model` 选择 OpenCode Go，浏览器批准后选择 DeepSeek V4.1 Flash 与 MiniMax M3.1 Flash Free，`models.json` 写入正确，对话正常。
2. `dagent login opencode`、`dagent login`、`dagent logout opencode`、`--list-models` 的输出。
3. 并发：把凭据的到期时间改到 5 分钟内后同时启动多个 `dagent run`，只刷新一次，其余进程读取新令牌，登录不被吊销；持旧 access token 的进程经一次 401 恢复后成功。
4. `refresh_pending` 残留时提示重新登录且不重发 refresh token；登出与刷新并发；取消登录与等待。
5. 配置错误：`login` 与 `api_key` 同时出现、未知提供方、不允许的 `base_url`、保留头、含换行的头值、大小写重复的头名。
6. 错误输出与日志中不出现令牌和请求头的值。
