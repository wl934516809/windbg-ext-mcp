# WinDbg 命令同步显示到调试窗口

## 需求背景

C++ Extension 通过 MCP 执行 WinDbg 命令时，为每个命令创建独立的 `IDebugClient` + 自定义 `OutputCallbacks`，所有输出被捕获到内存缓冲区返回给 MCP 客户端，**不显示在 WinDbg 调试窗口**。用户在 WinDbg 窗口中无法看到 AI 执行的命令及其输出，缺少可观测性。

## 最终方案：`DEBUG_OUTCTL_ALL_CLIENTS`

**原理**：`DEBUG_OUTCTL_ALL_CLIENTS` 指示引擎将命令输出多播到**所有** `IDebugClient` 实例，包括 WinDbg 自身的 UI 客户端。这样命令执行结果自然地显示在 WinDbg 调试窗口中，无需任何额外回显代码。

```
MCP 命令 -> Control::ControlledOutput(DEBUG_OUTCTL_ALL_CLIENTS, "[MCP] <command>\n")
             → 命令前缀广播到所有客户端（含 WinDbg UI）
       -> Control::Execute(DEBUG_OUTCTL_ALL_CLIENTS, command, ...)
             → 引擎将输出多播到所有客户端
             → WinDbg UI 客户端接收并显示在窗口
             → 自定义 OutputCallbacks 捕获到 m_output 缓冲区
       -> 通过 Named Pipe 返回 JSON 给 Python MCP Server
       -> 返回给 MCP Client

WinDbg 调试窗口：显示 "[MCP] <command>" + 全部命令输出
```

**关键区别**：
| | `DEBUG_OUTCTL_THIS_CLIENT` | `DEBUG_OUTCTL_ALL_CLIENTS` |
|---|---|---|
| 输出目标 | 仅当前 client 的回调 | **所有**已注册的 client |
| WinDbg 窗口可见 | 否 | **是** |
| 自定义回调捕获 | 是 | 是 |

## 废弃方案及失败原因

### 方案 1：dprintf 回显（导致 WinDbg 崩溃）

在 `OutputCallbacks::Output()` 中调用 `dprintf("%s", Text)` 回显到窗口。

**失败原因**：`dprintf` 使用 `ExtensionApis.lpOutputRoutine`，该函数与 `IDebugClient::SetOutputCallbacks` 共享引擎输出路由，调用 `dprintf` 会再次触发 `Output()` 回调，形成无限递归，导致 WinDbg 崩溃（栈溢出）。

### 方案 2：thread_local 重入守卫（输出重复）

添加 `thread_local` 标志位阻止递归调用。

**失败原因**：虽然不会崩溃，但出现两个问题：
1. `dprintf("[MCP] %s\n", command.c_str())` 的输出也被 `Output()` 回调捕获到 `m_output` 缓冲区
2. 命令名出现两次：`[MCP] [MCP] !analyze -v!analyze -v`

### 方案 3：DEBUG_OUTCTL_ALL_CLIENTS（最终采用）

根本不需要回显代码。引擎自身通过多播机制将输出送到所有客户端（含 WinDbg UI），一个宏开关解决问题。

## 改动文件（1 个，+3 -3 行）

| 文件 | 改动 |
|---|---|
| `extension/src/command/command_utilities.cpp` | `new OutputCallbacks(true)` → `new OutputCallbacks()`；移除 `dprintf("[MCP] %s\n", command.c_str())`；`ControlledOutput` + `Execute` 使用 `DEBUG_OUTCTL_ALL_CLIENTS` |

`extension/src/utils/output_callbacks.h` 和 `output_callbacks.cpp` **无需改动**，保持纯捕获模式。

### 核心代码

```cpp
// command_utilities.cpp — ExecuteWithTimeout()

// 创建回调（无参数，纯捕获）
callbacks = new OutputCallbacks();

// 命令前缀广播到所有客户端
control->ControlledOutput(DEBUG_OUTCTL_ALL_CLIENTS, DEBUG_OUTPUT_NORMAL,
    "[MCP] %s\n", command.c_str());

// 执行命令 — 输出多播到 ALL clients（含 WinDbg UI）
hr = control->Execute(DEBUG_OUTCTL_ALL_CLIENTS, command.c_str(), DEBUG_EXECUTE_DEFAULT);
```

### 安全性分析

| 关注点 | 结论 |
|---|---|
| **递归风险** | 无。引擎原生多播机制，不存在代码层面的回环 |
| **格式字符串注入** | 已规避。`ControlledOutput` 使用格式参数 |
| **MCP 兼容性** | 无影响。`OutputCallbacks::Output()` 仍然正常捕获到 `m_output`，JSON 响应内容不变 |
| **额外依赖** | 无。`DEBUG_OUTCTL_ALL_CLIENTS` 是 DbgEng API 标准常量 |

### 运行时效果

WinDbg 调试窗口中将显示：
```
[MCP] lm
start             end                 module name
fffff800`00000000 fffff800`00a5c000   nt         (pdb symbols)
...
[MCP] k
 # Child-SP          RetAddr           Call Site
00 fffff800`12345678 fffff800`00123456 nt!DbgBreakPointWithStatus
...
```

### 兼容性

- `OutputCallbacks` 类无改动，保持向后兼容
- MCP 响应格式无变化，AI 客户端行为不变
- 仅 `CommandExecutor::ExecuteWithTimeout()` 中调整执行宏和命令前缀方式

## 多 WinDbg 实例分析

### Named Pipe 层面：单实例限制

两个 WinDbg 进程加载同一个扩展 DLL，都执行 `!mcpstart`，第二个会失败：

```
WinDbg #1: !mcpstart → CreateNamedPipe(\\.\pipe\windbgmcp) → 成功，成为 pipe server
WinDbg #2: !mcpstart → CreateNamedPipe(\\.\pipe\windbgmcp) → 失败（管道名已被占用）
```

`PIPE_UNLIMITED_INSTANCES` 仅允许**同一进程内**创建多个同名 pipe 实例供多客户端连接，不能跨进程共享同一个 pipe 名称。

### Python MCP Server 层面：单连接

Python 端连接的是 `\\.\pipe\windbgmcp` 这个固定名称，只能连到第一个启动 pipe server 的 WinDbg。

### 回显功能本身：无额外影响

`DEBUG_OUTCTL_ALL_CLIENTS` 是引擎层面的多播机制，无论几个 WinDbg，各自的输出只会显示在各自的调试窗口，互不干扰。

### 结论

| 场景 | 结果 |
|---|---|
| 开 1 个 WinDbg，`!mcpstart` | 正常工作 |
| 开 2 个 WinDbg，都 `!mcpstart` | 第二个 `CreateNamedPipe` 失败，pipe server 起不来 |
| 开 2 个 WinDbg，仅一个 `!mcpstart` | 正常，另一个不受影响 |

多 WinDbg 实例支持需要架构层面的改动（如给每个 WinDbg 分配不同的 pipe 名称、Python 端维护多连接池），不在此次回显功能范围内。