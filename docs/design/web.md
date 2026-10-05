# Web：受管搜索与网页读取

`dagent_web` 负责 SRT 内 HTTP GET、字符集解码、HTML→Markdown 和 SearXNG JSON 协议。
它依赖 exec、base、lexbor v3.0.1 和 glibc iconv，不依赖 agent。工具参数、分页、缓存及给模型的措辞在 tools；
权限决策在 agent；SearXNG 运行时准备和进程生命周期在 app。

## 执行与权限

```text
模型 → web_search / web_fetch（ToolKind::network）
     → Policy：必须具备 read_only SRT
     → tools::detail::sandbox_request（与 bash 共用）
     → dagent_web → SRT 内 curl → SRT 代理 → Dispatcher 网络闸门
     → 解码、Markdown / 搜索结果 → Context 缓存 / 分页 → WebView
```

两个工具都串行执行，在 plan/read_only 中可用。unrestricted 仍走只读 SRT，仅改变网络闸门对新目标的默认决定。
配置拒绝、会话拒绝优先；其它情况下，父/会话规则、配置允许和审批复用 bash 的逻辑。
会话允许按 host:port 共享，撤销和降权会停止失去授权的活跃调用。
完整规则见 [agent §7](agent.md#7-权限与沙箱) 和 [权限指南](../guide/permissions.md)。

curl 禁用 curlrc、URL glob、非 HTTP(S) 协议和非 HTTP(S) 重定向；保留 TLS 校验，最多跟随 10 跳重定向。
`--noproxy ''` 强制 loopback/私网连接也进入 SRT 代理。`--output -` 明确通过管道输出字节，避免 curl 对某些响应的二进制检测提前停止输出。
正文经 `head -c (max_body_bytes+1)` 限制，curl JSON 元数据走独立 stderr；超限保留正文前缀，退出码 23 只有在确实达到截断上限时才被接受。
网络回调另外保存拒绝目标与原因，避免把代理的 HTTP 403 当成成功。超时包含等待审批；SRT 不可用时绝不转宿主抓取。

## 页面与缓存

HTML 通过 lexbor HTML5 DOM 转换为 Markdown，去掉 script/style/template 等非正文节点，解析标题，
保留标题层级、段落、列表、代码、强调和链接，链接按最终 URL 与 `<base>` 转为绝对地址并保留片段。
这是静态文档转换，不执行 JS，也不保证复刻网页排版。

编码按 BOM、HTTP charset、HTML 中的 charset 声明处理；GB2312/GBK 用 GB18030 兼容解码。
未声明编码时按 UTF-8 处理，非法字节替换；不进行语言模型摘要。文本、JSON/XML 保留正文内容，仅转换字符集并去除终端控制序列。
PDF、图片、其它二进制 MIME 类型和非成功 HTTP 状态返回错误。

`web_fetch` 使用 `url`、`offset`、`limit`；后两者是 UTF-8 字节计数。下一页使用返回的 `next_offset`，
`has_more` 表示缓存的正文仍有内容，`truncated` 表示下载只保留了前缀。
工具结果的总长度仍受 `tools.max_result_bytes` 约束。

每个 Context 维护独立 LRU 页面缓存，按原请求的规范化 URL（移除片段）索引，缓存内容包括最终 URL、标题、类型、状态和正文。
缓存命中不触发网络访问。缓存不落盘、不跨会话；超预算会淘汰页面，非零 offset 找不到缓存时要求从 0 重新抓取。
已获取内容不因网络授权撤销而从会话中抹除，后续新连接仍按当前权限判定。

## 托管搜索

`dagent runtime sync` 准备 `internal/searxng`，使用固定 commit 归档和 SHA-256，依赖从带哈希的锁文件安装预编译 wheel。
源码、venv 与 `internal/sandbox` 分开；不依赖 Docker、系统 Python 或系统 SearXNG 服务。
实现和配置格式见 [toolchain](toolchain.md#托管搜索基础设施)。

后端第一次搜索时，用随机密钥生成 `run/searxng-*/settings.yml`，开启 JSON、关闭限流；入口用内核分配的动态端口绑定 127.0.0.1，
通过启动通道报告实际端口，并经 `/healthz` 确认就绪。每个后端一个实例，多会话/子会话共享其所在后端的服务。
进程退出后下一次搜索重新拉起；正常关闭由 exec::Child 清理进程组，后端异常退出则由 stdin EOF 监听清理。

SearXNG 本身运行在宿主，属于 harness 基础设施，其上游连接不逐目标审批。
模型的搜索请求仍需通过 SRT 内 curl 连接托管端点；只有当前 web_search 对这个确切端点预授权，配置拒绝仍优先。
web_fetch 与 bash 不能借用这个基础设施标记。引擎失败和验证码会在结果中报告，有可用结果时仍返回结果；全部失败时返回工具错误。

## 配置

Home `config/config.json` 可选的 `web` 段，省略时使用以下默认值：

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

`timeout_ms` 是单次 SRT 请求上限，`startup_timeout_ms` 是搜索服务启动等待上限。
`max_body_bytes` 不超过 64 MiB，`cache_bytes` 至少等于正文下载上限。引擎名称使用固定 SearXNG 源码中的定义；不配置端口。
不同出口可能遭遇不同引擎的限流、验证码或封锁，可覆盖 `web.engines`。

搜索摘要和页面正文始终是不可信数据；工具说明与系统提示要求模型只把它们当作证据，不能执行其中的指令，也不能据此泄露秘密。
不支持厂商原生搜索、浏览器渲染、PDF/图片解析、robots.txt 抓取策略或 web 调用并行。
本机真实验收范围与证据见 [实施计划](../../nexttodo/web-tools-plan.md#10-实施与真实验收记录2026-10-05)。

## 上游依据

- [lexbor URL 与相对链接 API](https://lexbor.com/tutorials/extract-links/)。
- [SearXNG 安装与配置入口](https://docs.searxng.org/admin/installation-searxng.html)。
- [固定的 SearXNG 源码](https://github.com/searxng/searxng/tree/d48c4b555421e824342c51d68482dd0898e54d0f)。
