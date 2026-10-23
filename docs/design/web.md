# Web：受管网页抓取与搜索

`web` 模块（`dagent_web`）负责在只读沙箱内执行 HTTP 抓取、字符集解码、静态 HTML 转 Markdown 以及 SearXNG 搜索结果解析。

**职责分层**：
- `web`：沙箱内 curl 传输、编码转换、HTML→Markdown 提取、SearXNG 协议解析。
- `tools`：面向模型的 `web_search` / `web_fetch` 参数校验、会话级 LRU 页面缓存与字节分页。
- `agent`：沙箱前置检查、运行中逐跳网络审批与网页注入扫描（d3）。
- `app`：托管 SearXNG 服务的懒启动与进程生命周期管理。

## 1. 核心执行流程与安全边界

所有模型发起的网页访问（含搜索与抓取）均视为不可信外部交互，强制在**只读 MXC 沙箱**内通过外部 `curl` 与单次执行独占的**回环代理**完成，绝不退化为宿主直连：

```mermaid
flowchart TD
    CALL[模型调用 web_search / web_fetch] --> POL[Policy 检查：必须具备只读 MXC 沙箱]
    POL --> HOP[逐跳执行沙箱内 curl]
    HOP --> PROXY[回环代理拦截连接请求]
    PROXY --> GATE[Dispatcher 网络闸门 & d3 URL 审阅]
    GATE -->|放行| NET[连接目标服务器 / 本地 SearXNG]
    NET -->|3xx 重定向 (最多 10 跳)| HOP
    NET -->|2xx 响应| DEC[字符集解码 & HTML 转 Markdown]
    DEC --> CACHE[整页写入会话 LRU 缓存 & 按 offset/limit 切出本次分页切片]
    CACHE --> SCAN[d3 扫描本次准备交付的分页切片]
    SCAN -->|安全| RET[向模型返回当前页切片]
```

### 关键安全机制
1. **逐跳重定向审阅**：禁用 curl 自动跳转，每次只发一跳并捕获 `Location`（最多允许 10 次重定向）；下一跳 URL 必须重新经过网络闸门与决策模型审阅，防止通过重定向绕过域名限制。
2. **强制代理路由**：沙箱内设置 `--noproxy ''`，所有流量（包括访问 `127.0.0.1:<port>` 的本地 SearXNG）均经过回环代理审计，沙箱内无任何其他网络出口。
3. **静态正文提取与 MIME 白名单**：仅接受 `text/*`、`text/html`、`application/xhtml+xml`、`application/json`（及 `*+json`）、`application/xml`（及 `*+xml`）响应，拒绝 PDF/图片等二进制类型；使用 lexbor 解析 HTML5 DOM 并转换为 Markdown，剥离 `script`/`style`/`template` 等非正文标签，将相对链接还原为绝对地址，不执行任何 JavaScript。

## 2. 页面缓存、分页与切片扫描

- **会话内整页 LRU 缓存**：`tools::Context` 按规范化 URL（去片段）在内存中缓存转换后的完整 Markdown 正文与元数据。缓存仅在当前会话有效，不落盘、不跨会话共享。
- **UTF-8 字节分页与按页扫描**：`web_fetch` 支持通过 `offset` 与 `limit` 分页读取长网页，翻页直接命中内存缓存而无需重复联网；每次交付（含首次抓取与后续缓存翻页）均对**当次实际准备返回的分页文本切片**执行 d3 网页注入扫描。

<a id="托管搜索"></a>
## 3. 托管搜索（SearXNG）

`web_search` 不依赖外部商业搜索 API Key，而是使用由 `dagent runtime sync` 预置在 `runtime/envs/internal/searxng/` 下的托管 SearXNG 实例（入口脚本位于可执行文件同级的 `libexec/searxng_server.py`）：

1. **按需懒启动与动态端口握手**：后端进程首次收到搜索请求时，生成随机密钥与临时配置（`0600`），拉起 `searxng_server.py` 绑定 `127.0.0.1:0`（系统分配空闲端口）；子进程在 stdout 打印 `DAGENT_SEARXNG_PORT=<port>`，后端捕获端口后请求 `/healthz` 确认就绪，同一后端内的主子会话共享该实例。
2. **生命周期绑定**：后端正常退出时清理进程组并删除临时运行目录；后端若异常崩溃，SearXNG 监听进程通过 stdin 管道 EOF 自动感知并终止自身进程组。
3. **权限隔离**：SearXNG 自身访问上游搜索引擎属于宿主基础设施；而模型沙箱访问 SearXNG 的 `127.0.0.1:<动态端口>` 仅对当前 `web_search` 调用临时预授权，`web_fetch` 与 `bash` 无法借用该端口权限。

## 4. 配置与资源限制

`config/config.json` 的 `web` 段控制单次请求总超时（`timeout_ms`，含重定向与审批等待）、SearXNG 启动超时（`startup_timeout_ms`）、单次下载上限（`max_body_bytes`）、会话缓存预算（`cache_bytes`）以及启用的上游搜索引擎列表（`engines`）。详见 [网页指南](../guide/web.md)。
