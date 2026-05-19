# WinDbg 命令同步显示到调试窗口

## 需求背景

C++ Extension 通过 MCP 执行 WinDbg 命令时，为每个命令创建独立的 `IDebugClient` + 自定义 `OutputCallbacks`，所有输出被捕获到内存缓冲区返回给 MCP 客户端，**不显示在 WinDbg 调试窗口**。用户在 WinDbg 窗口中无法看到 AI 执行的命令及其输出，缺少可观测性。

## 原方案（改动前）

```
MCP 命令 -> IDebugClient::SetOutputCallbacks(自定义回调)
       -> IDebugControl::Execute(DEBUG_OUTCTL_THIS_CLIENT, ...)
       -> OutputCallbacks::Output() 捕获到 m_output 字符串
       -> 通过 Named Pipe 返回 JSON 给 Python MCP Server
       -> 返回给 MCP Client

WinDbg 调试窗口：无任何输出
```

**关键限制**：`DEBUG_OUTCTL_THIS_CLIENT` 将输出路由到当前 client 的回调，而自定义 `OutputCallbacks` 仅将文本存入内存缓冲区 `m_output`，不会写回 WinDbg 的调试输出窗口。

## 现方案（改动后）

**原理**：`dprintf()` 使用 WinDbg Extension Framework 的 `ExtensionApis.lpOutputRoutine`，这是一个与 `IDebugClient::SetOutputCallbacks` 完全独立的输出通道。在自定义回调中调用 `dprintf` 不会产生递归。

```
MCP 命令 -> dprintf("[MCP] <command>\n")    ← 新增：命令前缀打印到窗口
       -> IDebugClient::SetOutputCallbacks(自定义回调)
       -> IDebugControl::Execute(DEBUG_OUTCTL_THIS_CLIENT, ...)
       -> OutputCallbacks::Output() 中：
            dprintf("%s", Text)              ← 新增：输出回显到窗口
            m_output += AnsiToUtf8(Text)     ← 原有：捕获到缓冲区
       -> 通过 Named Pipe 返回 JSON 给 Python MCP Server
       -> 返回给 MCP Client

WinDbg 调试窗口：显示 "[MCP] <command>" + 全部命令输出
```

### 改动文件（3 个，共 +16 行）

| 文件 | 改动 |
|---|---|
| `extension/src/utils/output_callbacks.h` | 构造函数增加 `bool echoToWindbg = false` 参数；新增 `m_echoToWindbg` 成员变量 |
| `extension/src/utils/output_callbacks.cpp` | 构造函数接收并存储 `m_echoToWindbg`；`Output()` 中在 null 检查后、ANSI→UTF-8 转换前，通过 `dprintf("%s", Text)` 回显到 WinDbg 窗口 |
| `extension/src/command/command_utilities.cpp` | 创建 `OutputCallbacks` 时传入 `true` 启用回显；`Execute()` 前添加 `dprintf("[MCP] %s\n", command.c_str())` 显示命令前缀 |

### 安全性分析

| 关注点 | 结论 |
|---|---|
| **递归风险** | 无。`dprintf` 使用 `ExtensionApis.lpOutputRoutine`，与 `IDebugClient::SetOutputCallbacks` 是两条完全独立的输出路径 |
| **格式字符串注入** | 已规避。使用 `dprintf("%s", Text)` 将 Text 作为参数传递，而非格式字符串，输出中的 `%` 不会被误解析 |
| **编码安全** | 正确。`dprintf` 传入原始 ANSI `Text`（非 UTF-8 `textStr`），与 Windows CP_ACP 编码一致 |
| **MCP 兼容性** | 无影响。内存缓冲区捕获逻辑不变，JSON 响应内容不变 |

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

- 向后兼容：`echoToWindbg` 默认为 `false`，不影响其他调用点
- MCP 响应格式无变化，AI 客户端行为不变
- 仅在 `CommandExecutor::ExecuteWithTimeout()` 中启用（`true`），其他场景保持原行为