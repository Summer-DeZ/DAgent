#!/usr/bin/env node
// DAgent 的 SRT 运行桥。每个 Bash 执行实例（或每个长期 MCP 进程）一个进程：
// SRT 的模块级状态不跨进程共享，清理与占位点跟踪也只作用于本实例。
//
// 模式：
//   --mode=probe  真实做一次最小隔离启动，把结果写成一行 JSON 到 stdout（sandbox status 用）。
//   --mode=run    从控制 socket 读 init 帧，启动受限命令；网络目标经控制帧询问宿主。
//   --mode=stdio  长生命周期 MCP server：init 从 --init-file 读，stdin/stdout 连接 server，
//                 stderr 上以 {"dagent_bridge":true,...} 帧报告 ready/error/exited。
//                 网络只按配置的 strict allowlist，不存在运行中审批。
//
// run 模式的控制通道是 C++ 传入的一个双向 socketpair fd；命令的 stdin/stdout/stderr 保持独立，
// 本进程自身的诊断只写 stderr，绝不写 stdout（probe 模式除外）。

import net from 'node:net';
import os from 'node:os';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const PROTOCOL_VERSION = 1;

function parse_args(argv) {
  const args = {};
  for (const item of argv.slice(2)) {
    const match = /^--([a-z0-9-]+)(?:=(.*))?$/.exec(item);
    if (match) args[match[1]] = match[2] ?? 'true';
  }
  return args;
}

function fail(message) {
  throw new Error(message);
}

function shell_quote(value) {
  return `'${String(value).replaceAll("'", `'\\''`)}'`;
}

async function load_srt(entry) {
  if (!entry) fail('missing --srt entry');
  const url = pathToFileURL(entry).href;
  return await import(url);
}

function build_config(args, extra = {}) {
  const config = {
    network: { allowedDomains: [], deniedDomains: [], strictAllowlist: true },
    filesystem: { denyRead: [], allowRead: [], allowWrite: [], denyWrite: [] },
    ...extra,
  };
  if (args.bwrap) config.bwrapPath = args.bwrap;
  if (args.socat) config.socatPath = args.socat;
  if (args.rg) config.ripgrep = { command: args.rg };
  return config;
}

function result_versions(SandboxManager, srt_entry) {
  const out = { node: process.version };
  try {
    const package_json = path.join(path.dirname(srt_entry), '..', 'package.json');
    out.srt = JSON.parse(fs.readFileSync(package_json, 'utf8')).version;
  } catch {
    out.srt = 'unknown';
  }
  out.sandboxing_enabled = SandboxManager.isSandboxingEnabled();
  return out;
}

// ---------------------------------------------------------------------------
// probe：最小真实隔离启动。成功标准是命令在 SRT 下启动并正常退出、且可写目录真实可写。
// ---------------------------------------------------------------------------
async function probe(args) {
  const srt_entry = args.srt;
  let stage = 'load';
  let SandboxManager;
  const started = Date.now();
  try {
    ({ SandboxManager } = await load_srt(srt_entry));
    const workdir = args.workdir;
    if (!workdir || !fs.existsSync(workdir)) fail('probe workdir missing');
    stage = 'initialize';
    await SandboxManager.initialize(build_config(args, {
      filesystem: { denyRead: [], allowRead: [], allowWrite: [workdir], denyWrite: [] },
    }));
    stage = 'wrap';
    const marker = path.join(workdir, 'srt-probe.txt');
    fs.rmSync(marker, { force: true });
    const command = `cd ${shell_quote(args.cwd ?? workdir)} && pwd && printf ok > ${shell_quote(marker)} && cat ${shell_quote(marker)}`;
    const wrapped = await SandboxManager.wrapWithSandbox(command, args.shell);
    stage = 'spawn';
    const child = spawn(args.shell, ['-c', wrapped], { stdio: ['ignore', 'pipe', 'pipe'], detached: true });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (data) => (stdout += data));
    child.stderr.on('data', (data) => (stderr += data));
    const finished = new Promise((resolve) => {
      child.on('error', (error) => resolve({ error }));
      child.on('close', (code, signal) => resolve({ code, signal }));
    });
    const outcome = await finished;
    stage = 'command';
    const wrote = outcome.code === 0 && fs.existsSync(marker) && fs.readFileSync(marker, 'utf8') === 'ok';
    fs.rmSync(marker, { force: true });
    if (outcome.error) fail(`spawn failed: ${outcome.error.message}`);
    if (!wrote) {
      stage = /bwrap:|namespace|uid map|uid_map|loopback/i.test(stderr) ? 'sandbox_start' : 'command';
      fail(stderr.trim() || `command exit ${outcome.code ?? outcome.signal}`);
    }
    return {
      ok: true,
      stage: 'completed',
      elapsed_ms: Date.now() - started,
      ...result_versions(SandboxManager, srt_entry),
    };
  } catch (error) {
    return { ok: false, stage, error: String(error?.message ?? error).slice(0, 2000) };
  } finally {
    try {
      await SandboxManager?.cleanupAfterCommand();
      await SandboxManager?.reset();
    } catch {
      /* 清理失败由桥外层的进程生命周期兜底 */
    }
  }
}

// ---------------------------------------------------------------------------
// run：完整执行模式（Bash）。init 帧到达前不启动任何命令。
// ---------------------------------------------------------------------------
function make_control(fd) {
  const socket = new net.Socket({ fd, readable: true, writable: true });
  let buffer = '';
  const handlers = new Set();
  socket.on('data', (chunk) => {
    buffer += chunk.toString('utf8');
    for (;;) {
      const index = buffer.indexOf('\n');
      if (index < 0) break;
      const line = buffer.slice(0, index);
      buffer = buffer.slice(index + 1);
      if (!line.trim()) continue;
      let message;
      try {
        message = JSON.parse(line);
      } catch {
        continue;
      }
      for (const handler of handlers) handler(message);
    }
  });
  return {
    send(message) {
      socket.write(JSON.stringify(message) + '\n');
    },
    on_message(handler) {
      handlers.add(handler);
    },
    on_close(handler) {
      socket.on('close', handler);
      socket.on('error', handler);
    },
    // end(callback) 确认已写出的帧刷完，再 unref，避免控制 socket 把事件循环留到宿主侧关闭为止。
    close() {
      return new Promise((resolve) => {
        socket.end(() => {
          socket.unref();
          socket.destroy();
          resolve();
        });
      });
    },
    socket,
  };
}

async function run(args) {
  const fd = Number(args['control-fd'] ?? -1);
  if (!Number.isInteger(fd) || fd < 0) fail('missing --control-fd');
  const control = make_control(fd);

  const init = await new Promise((resolve, reject) => {
    let settled = false;
    control.on_message((message) => {
      if (message.type === 'init' && !settled) {
        settled = true;
        resolve(message);
      } else if (message.type === 'cancel' && !settled) {
        settled = true;
        reject(new Error('cancelled before start'));
      }
    });
    control.on_close(() => {
      if (!settled) {
        settled = true;
        reject(new Error('control channel closed before init'));
      }
    });
  });

  const { SandboxManager } = await load_srt(args.srt);
  const config = init.config ?? build_config(args);
  if (args.bwrap && !config.bwrapPath) config.bwrapPath = args.bwrap;
  if (args.socat && !config.socatPath) config.socatPath = args.socat;
  if (args.rg && !config.ripgrep) config.ripgrep = { command: args.rg };

  const timeout_ms = Number(init.network_approval_timeout_ms ?? 120000);
  const max_requests = Number(init.max_network_requests ?? 32);
  let requests = 0;
  let next_request_id = 1;
  const pending = new Map();
  let cancelled = false;

  control.on_message((message) => {
    if (message.type === 'network_decision') {
      const entry = pending.get(message.request_id);
      if (!entry) return;
      pending.delete(message.request_id);
      clearTimeout(entry.timer);
      entry.resolve(message.allow === true);
      // 用户明确拒绝：取消本次调用，而不是只让该连接失败。
      if (message.allow !== true && message.cancel === true) {
        cancelled = true;
        terminate();
      }
    } else if (message.type === 'cancel') {
      cancelled = true;
      for (const [id, entry] of pending) {
        pending.delete(id);
        clearTimeout(entry.timer);
        entry.resolve(false);
      }
    }
  });

  const ask = ({ host, port }) =>
    new Promise((resolve) => {
      if (cancelled || requests >= max_requests) return resolve(false);
      const request_id = next_request_id++;
      requests += 1;
      const timer = setTimeout(() => {
        pending.delete(request_id);
        control.send({ type: 'network_expired', request_id });
        resolve(false);
      }, timeout_ms);
      pending.set(request_id, { resolve, timer });
      control.send({ type: 'network_request', request_id, host, port });
    });

  await SandboxManager.initialize(config, ask);
  const start = new Promise((resolve, reject) => {
    control.on_message((message) => { if (message.type === 'start') resolve(); });
    control.on_close(() => reject(new Error('control closed during handshake')));
  });
  control.send({ type: 'ready', protocol: PROTOCOL_VERSION, environment: path.resolve(args.srt),
                 ...result_versions(SandboxManager, args.srt) });
  await start;

  const workspace = init.workspace;
  const command = init.command ?? '';
  const shell = init.shell ?? args.shell ?? 'bash';
  const full_command = workspace ? `cd ${shell_quote(workspace)} && ${command}` : command;

  let child;
  try {
    // denyRead 下的代理 socket 路径必须按执行重开：SRT 不会把内部 bridge socket 自动加入 allowRead。
    const sockets = [SandboxManager.getLinuxHttpSocketPath?.(), SandboxManager.getLinuxSocksSocketPath?.()].filter(Boolean);
    const custom = sockets.length
      ? { filesystem: { allowRead: [...(config.filesystem?.allowRead ?? []), ...sockets] } }
      : undefined;
    const wrapped = await SandboxManager.wrapWithSandbox(full_command, shell, custom);
    child = spawn(shell, ['-c', wrapped], { stdio: ['inherit', 'inherit', 'inherit'], detached: true });
  } catch (error) {
    control.send({ type: 'bridge_error', stage: 'wrap', error: String(error?.message ?? error) });
    await SandboxManager.reset().catch(() => {});
    await control.close();
    process.exitCode = 3;
    return;
  }

  let termination_timer;
  const kill_tree = (signal) => {
    try {
      process.kill(-child.pid, signal);
    } catch {
      /* 已经退出 */
    }
  };
  const terminate = () => {
    kill_tree('SIGTERM');
    termination_timer = setTimeout(() => kill_tree('SIGKILL'), init.kill_grace_ms);
    termination_timer.unref();
  };
  // bridge 被宿主终止时，命令树必须一起结束（bwrap 另有 --die-with-parent 兜底）。
  process.on('SIGTERM', () => kill_tree('SIGKILL'));
  process.on('SIGINT', () => kill_tree('SIGKILL'));
  const finished = new Promise((resolve) => {
    child.on('error', (error) => resolve({ error }));
    child.on('close', (code, signal) => resolve({ code, signal }));
  });
  control.on_close(() => {
    cancelled = true;
    kill_tree('SIGKILL');
  });
  const outcome = await finished;
  clearTimeout(termination_timer);
  for (const [id, entry] of pending) {
    pending.delete(id);
    clearTimeout(entry.timer);
    entry.resolve(false);
  }
  kill_tree('SIGTERM');
  control.send({
    type: 'exited',
    exit_code: outcome.code ?? null,
    // 上报信号编号（宿主按整数处理）；名称到编号的映射只在 bridge 内完成。
    signal: outcome.signal ? (os.constants.signals[outcome.signal] ?? null) : null,
    cancelled,
    error: outcome.error ? String(outcome.error.message) : null,
  });
  try {
    SandboxManager.cleanupAfterCommand();
  } catch {
    /* 清理失败由 reset/进程生命周期兜底 */
  }
  await SandboxManager.reset().catch(() => {});
  await control.close();
}

// ---------------------------------------------------------------------------
// stdio：长生命周期 MCP server。init 从文件读；stdin/stdout 归 server，状态帧走 stderr。
// ---------------------------------------------------------------------------
async function stdio(args) {
  const init_path = args['init-file'];
  if (!init_path) fail('missing --init-file');
  let init;
  try {
    init = JSON.parse(fs.readFileSync(init_path, 'utf8'));
  } catch (error) {
    fail(`cannot read init file: ${error?.message ?? error}`);
  }
  const report = (message) => {
    try {
      process.stderr.write(JSON.stringify({ dagent_bridge: true, ...message }) + '\n');
    } catch {
      /* 宿主已经关闭 */
    }
  };

  let SandboxManager;
  try {
    ({ SandboxManager } = await load_srt(args.srt));
  } catch (error) {
    report({ type: 'error', stage: 'load', error: String(error?.message ?? error) });
    process.exitCode = 3;
    return;
  }
  const config = init.config ?? build_config(args);
  if (args.bwrap && !config.bwrapPath) config.bwrapPath = args.bwrap;
  if (args.socat && !config.socatPath) config.socatPath = args.socat;
  if (args.rg && !config.ripgrep) config.ripgrep = { command: args.rg };

  try {
    await SandboxManager.initialize(config, () => Promise.resolve(false));
  } catch (error) {
    report({ type: 'error', stage: 'initialize', error: String(error?.message ?? error) });
    await SandboxManager.reset().catch(() => {});
    process.exitCode = 3;
    return;
  }
  report({
    type: 'ready',
    protocol: PROTOCOL_VERSION,
    environment: path.resolve(args.srt),
    ...result_versions(SandboxManager, args.srt),
  });

  const shell = init.shell ?? args.shell ?? 'bash';
  let child;
  try {
    const sockets = [SandboxManager.getLinuxHttpSocketPath?.(), SandboxManager.getLinuxSocksSocketPath?.()].filter(Boolean);
    const custom = sockets.length
      ? { filesystem: { allowRead: [...(config.filesystem?.allowRead ?? []), ...sockets] } }
      : undefined;
    const wrapped = await SandboxManager.wrapWithSandbox(init.command ?? '', shell, custom);
    child = spawn(shell, ['-c', wrapped], { stdio: ['pipe', 'pipe', 'pipe'], detached: true });
  } catch (error) {
    report({ type: 'error', stage: 'wrap', error: String(error?.message ?? error) });
    await SandboxManager.reset().catch(() => {});
    process.exitCode = 3;
    return;
  }

  const kill_tree = (signal) => {
    try {
      process.kill(-child.pid, signal);
    } catch {
      /* 已经退出 */
    }
  };
  process.on('SIGTERM', () => kill_tree('SIGKILL'));
  process.on('SIGINT', () => kill_tree('SIGKILL'));
  process.stdin.on('error', () => {});
  process.stdin.on('end', () => {
    try {
      child.stdin.end();
    } catch {
      /* server 已经退出 */
    }
  });
  process.stdin.pipe(child.stdin);
  child.stdout.pipe(process.stdout);
  child.stderr.pipe(process.stderr);
  const finished = new Promise((resolve) => {
    child.on('error', (error) => resolve({ error }));
    child.on('close', (code, signal) => resolve({ code, signal }));
  });
  const outcome = await finished;
  kill_tree('SIGTERM');
  report({
    type: 'exited',
    exit_code: outcome.code ?? null,
    signal: outcome.signal ? (os.constants.signals[outcome.signal] ?? null) : null,
    error: outcome.error ? String(outcome.error.message) : null,
  });
  try {
    SandboxManager.cleanupAfterCommand();
  } catch {
    /* 清理失败由 reset/进程生命周期兜底 */
  }
  await SandboxManager.reset().catch(() => {});
}

async function main() {
  const args = parse_args(process.argv);
  const mode = args.mode ?? 'probe';
  if (mode === 'probe') {
    const result = await probe(args);
    process.stdout.write(JSON.stringify(result) + '\n');
    if (!result.ok) process.exitCode = 1;
  } else if (mode === 'run') {
    await run(args);
  } else if (mode === 'stdio') {
    await stdio(args);
  } else {
    fail(`unknown mode: ${mode}`);
  }
}

main().catch((error) => {
  process.stderr.write(`srt_bridge: ${String(error?.stack ?? error)}\n`);
  process.exitCode = 2;
});
