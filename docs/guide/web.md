# 网页搜索与抓取

`web_search` 搜索网页并返回标题、URL 和摘要，`web_fetch` 读取指定 URL。
这两个名字是模型工具，通过向 DAgent 提交任务使用；没有独立的 `dagent web_search` 子命令。

## 准备与使用

先完成 [构建与安装](build.md)，确认宿主提供 curl 7.75+、bubblewrap 和 socat，然后执行：

```bash
dagent runtime sync
dagent sandbox status
dagent --permissions ask "用 web_search 查找 Python 官方 urllib 文档，再用 web_fetch 阅读相关页面并给出来源链接"
```

`runtime sync` 准备固定版本的 SearXNG 和独立 Python 依赖环境。
首次 `web_search` 会启动本后端专用的搜索进程，端口自动分配；退出后端时结束进程。
`web_fetch` 直接通过只读 SRT 内的 curl 请求网页，不需要启动 SearXNG。

ask/workspace 中，新网页目标需要网络审批。允许本会话访问后，web 和受限 bash 可复用同一 host:port 授权；
ask 下 bash 命令本身仍可能需要审批。重定向的每个目标分别判定，`/permissions` 可撤销允许或拒绝规则。

规划模式也可以联网调研：

```bash
dagent --plan "搜索并阅读官方文档，规划 HTTP 客户端的迁移，不修改文件"
```

`run` 没有人工审批入口。搜索自身的托管端点自动获准；抓取网页时，应预先配置需要的目标，例如：

```bash
dagent run --permissions ask \
  --set 'sandbox.allowed_targets=["docs.python.org:443"]' \
  "用 web_fetch 阅读 https://docs.python.org/3/library/urllib.parse.html 并总结 URL 解析的注意事项"
```

重定向到其它目标仍可能需要审批。unrestricted 下 web 连接自动放行新目标，但配置拒绝、会话拒绝及只读/规划上限仍生效；
web 始终要求只读 SRT。SearXNG 的上游引擎连接属于宿主基础设施，不逐引擎走工具审批。详见 [权限与沙箱](permissions.md#网页搜索与抓取)。

## 内容与分页

| 工具 | 参数 | 返回内容 |
| --- | --- | --- |
| `web_search` | `query`；`limit` 默认 8，范围 1–20 | 标题、URL、摘要及引擎失败说明 |
| `web_fetch` | `url`；`offset` 默认 0；`limit` 默认 16000，范围 4–1000000 | 最终 URL、标题、HTTP 状态、正文及分页信息 |

抓取只接受不含用户名/密码的 HTTP(S) URL。HTML 转为带绝对链接的 Markdown；文本和 JSON 等文本类型转换为 UTF-8。
GB2312/GBK 页面可以解码；不执行 JavaScript，不解析 PDF 或图片。

`offset` 和 `limit` 是 UTF-8 **字节数**。需要下一页时，让模型传入结果中的精确 `next_offset`；
不要按汉字数量计算偏移。`cached=true` 表示使用当前会话的缓存，没有再次联网。
`has_more=true` 表示缓存中还有下一页；下载截断提示则表示只保留了原页面前缀，继续翻页不能恢复未下载的部分。
实际返回长度还受 `tools.max_result_bytes` 约束。

缓存只存于内存，按预算淘汰；新建、恢复会话或切换模型重新装配 Context 后，需要重新获取页面。
`offset=0` 在缓存命中时也会复用已有页面，目前没有强制刷新参数。搜索摘要和网页正文均作为不可信资料，引用内容时应保留来源 URL。

## 引擎和资源配置

Home `config/config.json` 中的可选 `web` 段默认如下：

```json
{
  "web": {
    "timeout_ms": 120000,
    "startup_timeout_ms": 30000,
    "max_body_bytes": 2097152,
    "cache_bytes": 16777216,
    "engines": ["yahoo", "brave", "duckduckgo"]
  }
}
```

修改引擎后重新启动后端；一次性覆写可用 `--set 'web.engines=["yahoo"]'`。
引擎名必须对应托管 SearXNG 中的定义，列表不能为空；不配置固定端口，也不填外部 SearXNG 地址。
不同网络出口可能遇到验证码、限流或封锁，有可用结果时会返回结果并列出失败引擎。

`timeout_ms` 限制一次 SRT 请求，包括等待审批的时间；懒启动搜索服务另受 `startup_timeout_ms` 约束。
正文下载上限必须大于 0、不超过 64 MiB，缓存预算至少等于下载上限。缓存按转换后的正文及元数据计费，
Markdown 展开后可能大于下载体积。调整这些行为配置不需要重新安装依赖；修改 runtime 声明或锁文件后才需要 sync。

## 常见情况

| 提示或现象 | 含义与处理 |
| --- | --- |
| `SearXNG is not prepared` | 检查 runtime 中的 `internal/searxng` 声明及锁文件，执行 `runtime sync` |
| `SearXNG did not become ready` | 启动或健康检查失败；核对入口资源、引擎名称和错误文本 |
| `required read-only SRT capabilities are unavailable` | 用 `sandbox status` 排查；两个 web 工具都不会回退宿主执行 |
| `runtime network approval is unavailable in this run` | 当前目标没有授权且没有可用审批器；使用交互入口或明确配置所需目标 |
| `network target is on the configured deny list` | 命中 `sandbox.denied_targets`，unrestricted 也不能覆盖 web 的这项拒绝 |
| `the user denied this network target earlier in the session` | 已有会话拒绝；需要改变决定时由用户在 `/permissions` 撤销 |
| `Search engine failures` / CAPTCHA | 上游引擎失败；查看是否仍有结果，必要时调整引擎配置 |
| `page is no longer cached` | 用原 URL、`offset=0` 重新获取第一页，再使用新结果的 `next_offset` |
| `download truncated` | 下载已达到上限；按需提高 `web.max_body_bytes` 并在新的 Context 中重新抓取 |
| `search response exceeded web.max_body_bytes` | 搜索 JSON 不完整，整次搜索返回错误；提高下载上限后重试 |
| `unsupported content type` | 目标不是支持的文本类型；JS 页面也不会自动启动浏览器渲染 |

当前实现契约见 [web 设计](../design/web.md)，已有真实运行覆盖及未覆盖范围见 [验收记录](../archive/2026-10-05-web-tools-plan.md#10-实施与真实验收记录2026-10-05)。
