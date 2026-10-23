# Toolchain：托管工具链与独立依赖环境

`Toolchain` 机制（实现于 `dagent_app_config`）负责根据 `<home>/config/runtime.json` 声明的版本与哈希，显式下载、校验并装配工具二进制与语言依赖环境，为命令执行、代码搜索、MCP 服务及托管网页搜索提供**可复现、与宿主环境隔离的不可变执行快照**。

## 1. 目录结构与快照机制

```text
home/runtime/
├── bash/<version-generation>/        基础工具包（按版本与配置摘要隔离）
├── git/<version-generation>/
├── uv/<version-generation>/
├── python/<version-generation>/      由托管 uv 安装的独立 Python
├── node/<version-generation>/
├── rg/<version-generation>/
├── mxc/<version-generation>/         MXC 沙箱原生动态库（libmxc_ffi.so / mxc_ffi.dll）
├── envs/                             消费者独立依赖环境
│   ├── skills/<name>/<generation>/
│   ├── mcp/<name>/<generation>/
│   └── internal/searxng/<generation>/
├── homes/<profile>/                  受管执行的环境根目录
└── installed.json                    当前已就绪的全局清单快照
```

### 核心同步流程（`dagent runtime sync`）
1. **显式离线准备与代理继承**：常规启动从不隐式下载或安装包。用户执行 `dagent runtime sync` 时，获取 `run/runtime.lock` 独占锁，自动继承宿主代理环境变量（`HTTP(S)_PROXY` / `ALL_PROXY` / `NO_PROXY`），按序准备缺失的工具包与依赖环境。
2. **强校验、8 大基础工具断言与代际隔离**：所有归档下载强制使用 HTTPS 并校验 SHA-256；每个包和依赖环境的目录名包含其配置与锁文件内容的哈希（generation）。基础包同步完成后，强制校验 8 个核心可执行文件（`bash`, `git`, `rg`, `uv`, `python`, `python3`, `node`, `npm`）全部就绪并写入 `.ready.json`，最后才原子更新顶层 `installed.json`。
3. **运行期不可变**：已发布的旧代际目录不在同步时原地修改或删除，确保正在运行的后端进程所持有的路径快照始终有效。

## 2. 四类工具包来源（`packages`）

| 类型（`kind`） | 准备方式与用途 |
| --- | --- |
| `archive` | 从配置的平台 URL 下载压缩包，校验 SHA-256 后解压（如 `uv`、`node`、`rg`、`mxc` 原生库） |
| `python` | 使用已就绪的托管 `uv` 安装指定版本的独立 Python，不依赖系统 Python |
| `files` | 在显式同步时将本机指定二进制及辅助文件导入到包目录（如 Linux 下的 `bash`、`git`） |
| `system` | 直接引用宿主固定安装目录（如 Windows 下引用 `C:/Program Files/Git`），仅校验程序存在并计算摘要 |

配置支持通过 `platforms` 字段按目标系统架构（如 `Linux-x86_64`、`Linux-aarch64`、`Windows-x86_64`）覆盖包定义、原生库路径，或设为 `null` 在特定平台禁用某环境（如 Windows 下禁用 `internal/searxng`）。

## 3. 消费者独立依赖环境（`environments`）

为避免不同技能或 MCP 插件之间的 Python/Node 依赖冲突，支持按消费者声明独立环境：
- `skills/<skill-name>`：供特定技能的脚本使用（激活技能时自动提示模型传入 `environment: "skills/<name>"`）。
- `mcp/<server-name>`：供特定 stdio MCP 服务使用。
- `internal/searxng`：仅供后端内部托管搜索服务使用，不向模型暴露。

**确定性构建**：
- **Python 环境**：使用托管 Python 创建虚拟环境，通过 `uv pip sync --require-hashes --only-binary=:all:` 从锁文件安装预编译 wheel，禁止隐式调用系统编译器构建源码。
- **Node 环境**：复制 `package.json` 与 `package-lock.json` 后执行 `npm ci`（默认禁用安装脚本）。

## 4. 运行时环境隔离（`managed` vs `project`）

工具或 MCP 启动子进程时可选择两类环境策略：
- **受管环境（`managed` / `<consumer-env>`，默认）**：从空环境变量起步，注入消费者依赖目录、托管工具 `bin` 目录及基础系统路径；将 `HOME`/`XDG`/`UV`/`NPM` 缓存指向受管目录并默认开启离线标志，彻底隔绝宿主 shell 配置与语言环境污染。
- **项目环境（`project`）**：供主动需要使用宿主开发工具链的场景，保留过滤敏感变量后的宿主 `PATH` 与环境（沙箱内执行时仍会清洗危险加载器变量并将临时目录重定向到私有 HOME）。

<a id="托管搜索基础设施"></a>
## 5. 托管搜索基础设施

`internal/searxng` 环境使用固定 commit 的 SearXNG 源码归档（校验 SHA-256）与带哈希的 `config/searxng/requirements.lock` 预置在 `runtime/envs/` 下（仅 Linux 启用）。`runtime sync` 在同步阶段完成源码解压与 Python 依赖安装；运行期由 `app::Searxng` 在首次搜索时直接拉起，无需 Docker 或系统级服务（详见 [web](web.md#托管搜索)）。
