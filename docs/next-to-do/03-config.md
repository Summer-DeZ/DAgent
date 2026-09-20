# 03 多模型配置

> 里程碑 P3 · `src/private/app/config.cpp`、`src/public/app/config.hpp`、`cli.cpp`、`main.cpp`

一段 `gateway` 变成一张可命名的表，用户写几套、启动时选一套。

---

## 1. 配置形状

```jsonc
{
  "models": {
    "local":    { "kind": "openai-chat",
                  "base_url": "http://127.0.0.1:10009/v1",
                  "model": "Qwen3.8-Flash-Next",
                  "context_window": 32768,
                  "extra_body": { "chat_template_kwargs": { "enable_thinking": true } } },
    "deepseek": { "kind": "openai-chat",
                  "base_url": "https://api.deepseek.com/v1",
                  "model": "deepseek-chat",
                  "api_key_env": "DEEPSEEK_API_KEY" },
    "glm":      { "kind": "openai-chat",
                  "base_url": "https://open.bigmodel.cn/api/paas/v4",
                  "model": "glm-4.6",
                  "api_key_env": "ZHIPU_API_KEY" },
    "sonnet":   { "kind": "anthropic",
                  "model": "claude-sonnet-5",
                  "max_tokens": 8192,
                  "context_window": 200000,
                  "api_key_env": "ANTHROPIC_API_KEY" }
  },
  "model": "local"
}
```

每个条目的键与 `ProviderConfig` 一一对应，外加一个 `api_key_env`（沿用现有密钥优先级：
环境变量 → `.env`）。`base_url` 省略时用该 kind 的默认端点；`model` 必填。

## 2. `gateway` 段直接删掉

不做兼容层。现在唯一的使用者是仓库里的 [config/dagent.json](../../config/dagent.json)，
用户级配置目录（`~/.config/dagent/`）还不存在，没有需要照顾的存量。留一层「gateway 等价于某个条目」
的映射，只会让配置加载多一条分支、文档多一种说法。

迁移动作只有两步：

1. `app::Config` 里 `Gateway gateway;` 换成 `std::map<std::string, ProviderConfig> models;` 与
   `std::string model;`，`make_setup` 从选中的条目填 `Setup::provider`；
2. 把 `config/dagent.json` 的 `gateway` 段改写成 `models.local` 加 `"model": "local"`。

配置里出现 `gateway` 时报错而不是忽略：`unknown config key gateway.*` 已有的 warn 不够醒目，
这里要明确提示「`gateway` 已被 `models` 取代」，免得改到一半的配置静默跑在默认值上。

分层合并：用户级与项目级配置里的 `models` **按名字合并**（同名条目逐键覆盖），不是整表替换——
否则项目配置加一个模型就会把用户的全冲掉。`model`（选哪个）按普通标量覆盖。

## 3. 校验

启动时一次性查完，错误信息必须指明是哪个模型的哪个键：

| 情况 | 行为 |
| --- | --- |
| `kind` 不在 `providers()` 里 | 失败：`models.sonnet.kind: unknown provider "claude"` |
| `model` 为空 | 失败 |
| `needs_api_key` 且密钥取不到 | 失败，提示看哪个环境变量；**不打印密钥** |
| `needs_max_tokens` 且 `max_tokens` 为 0 | 失败（Anthropic 必填） |
| `base_url` 为空且该 kind 无默认端点 | 失败 |
| `model` 指向不存在的条目 | 失败，列出可用名字 |
| `models` 为空 | 失败：至少要有一个模型 |
| 出现 `gateway` 段 | 失败，提示已被 `models` 取代 |
| `context_window` 为 0 | 正常，回落到 `context.window_tokens`；不做探测（[01 §2.3](01-internal.md)） |

`known_key()` 要把 `models.*` 当作已知前缀（和现在的 `gateway.extra_body` 一样），
否则每个条目都会 warn 一次未知键。

## 4. 命令行

| 参数 | 现在 | 改成 |
| --- | --- | --- |
| `-m/--model <值>` | `--set gateway.model=<值>` 的简写 | 先按 `models` 的名字查；查不到就当模型 id，覆盖当前条目的 `model` |
| `--set models.x.y=z` | — | 照常可用（`models.*` 进已知键前缀） |
| `--list-models` | — | 打印名字、kind、model、base_url（密钥只显示有/无），然后退出 |

`-m` 的两义性是有意的：`dagent -m sonnet` 选配置，`dagent -m deepseek-reasoner` 换模型 id。
查表优先，找不到再当 id，并在日志里说明走了哪条路。

## 5. 完成标准

1. 三套模型的配置，`--model` 分别启动，状态栏/侧栏显示的模型名正确，密钥取对。
2. 项目级配置只写一个 `models.local.model`，不会冲掉用户级的其它条目。
3. §3 的七种错误各造一次，报错都指到具体键，且不泄露密钥。
4. 写着 `gateway` 段的旧配置启动时明确报错，不静默回落。
5. `--list-models` 的输出可以直接贴进 issue（无密钥）。
