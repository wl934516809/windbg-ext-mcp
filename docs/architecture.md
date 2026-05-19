# WinDbg-ext-MCP 架构设计文档

## 1. 项目概述

WinDbg-ext-MCP 是一个桥梁系统，将 MCP 兼容的 AI 编程助手（Claude Desktop、Cursor、VS Code + Cline/Roo Code、Windsurf）连接到 Windows 调试器（WinDbg），使 AI 能够通过自然语言进行内核与用户态调试。

**作者**: NadavLorber  **版本**: v0.1.0  **定位**: kernel-first，兼容 user-mode

---

## 2. 总体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                        MCP Client 层                                  │
│  Claude Desktop | Cursor | VS Code Cline/Roo | Windsurf              │
│                        │  stdio (JSON-RPC / MCP)                     │
└────────────────────────┼─────────────────────────────────────────────┘
                         │
┌────────────────────────┼─────────────────────────────────────────────┐
│                  Python MCP Server 层                                 │
│                                                                       │
│  ┌──────────┐  ┌──────────────┐  ┌───────────────────────────────┐  │
│  │ FastMCP   │  │ 15 MCP Tools │  │ Core Services                 │  │
│  │ (stdio)   │  │ (session/    │  │ • Validation (安全检查)        │  │
│  │           │  │  execution/   │  │ • Execution (统一执行引擎)    │  │
│  │           │  │  analysis/    │  │ • Context (调试上下文栈)      │  │
│  │           │  │  performance/ │  │ • Cache (统一缓存系统)        │  │
│  │           │  │  support)     │  │ • Error Handler (增强错误)    │  │
│  └──────────┘  └──────────────┘  │ • Session Recovery (会话恢复) │  │
│                                   │ • Async Ops (异步任务管理)    │  │
│                                   │ • Performance Optimizer       │  │
│                                   └───────────────────────────────┘  │
│                         │ Named Pipe (\\.\pipe\windbgmcp)             │
└─────────────────────────┼────────────────────────────────────────────┘
                          │
┌─────────────────────────┼────────────────────────────────────────────┐
│                 C++ WinDbg Extension 层                               │
│                                                                       │
│  ┌─────────────────────┐  ┌────────────────────────────────────┐     │
│  │ Named Pipe Server    │  │ Command Handlers                   │     │
│  │ • Multi-client       │  │ • Basic (version, modules, type)   │     │
│  │ • Thread pool        │  │ • Enhanced (执行+超时优化)        │     │
│  │ • Message routing    │  │ • Diagnostic (健康检查, 性能)     │     │
│  │ • Per-client queues  │  │ • CommandRegistry (中央注册)      │     │
│  └─────────────────────┘  └────────────────────────────────────┘     │
│                         │ WinDbg Engine API                           │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │ IDebugControl | IDebugSymbols | IDebugDataSpaces | ...        │    │
│  └──────────────────────────────────────────────────────────────┘    │
└───────────────────────────────────────────────────────────────────────┘
                          │
┌─────────────────────────┼────────────────────────────────────────────┐
│                    Target (调试目标)                                   │
│  Local Kernel | Remote Kernel | User Process | Dump File             │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.1 通信链路

三层之间通过两种 IPC 机制连接：

| 层间边界 | 协议 | 传输 | 消息格式 |
|----------|------|------|----------|
| MCP Client → Python Server | MCP (JSON-RPC 2.0) | stdin/stdout | JSON-RPC |
| Python Server → C++ Extension | 自定义协议 | Named Pipe (`\\.\pipe\windbgmcp`) | JSON |

---

## 3. Python MCP Server 层详解

### 3.1 启动流程

```
main()                                      # server.py:76
  ├── _configure_logging()                  # 加载环境变量 (DEBUG, VERBOSE)
  ├── WinDbgMCPServer.__init__()
  │     ├── FastMCP()                       # 创建 MCP 协议服务器
  │     └── ServerInitializer()             # 连接测试 + 诊断
  └── server.start()
        ├── initializer.initialize()         # 测试命名管道连通性
        ├── register_all_tools(mcp)         # 注册 15 个 MCP 工具
        │     ├── register_session_tools()  # 3 工具
        │     ├── register_execution_tools()# 3 工具
        │     ├── register_analysis_tools() # 4 工具
        │     ├── register_performance_tools()# 2 工具
        │     └── register_support_tools()  # 3 工具 (实际 4)
        └── mcp.run()                       # 阻塞，监听 stdio
```

### 3.2 核心模块架构

#### 3.2.1 统一执行引擎 (`core/execution/`)

这是 Python 层最核心的子系统，采用**策略模式**实现灵活的命令执行：

```
                  ┌──────────────────────────┐
                  │  UnifiedCommandExecutor   │  ← 单例，唯一入口
                  │                          │
                  │  execute(command,         │
                  │   resilient=True,         │
                  │   optimize=True,          │
                  │   async_mode=False)       │
                  └──────────┬───────────────┘
                             │
                  ┌──────────▼───────────┐
                  │  TimeoutResolver      │  ← 命令分类 → 超时值映射
                  │  (10s ~ 1200s)       │
                  └──────────┬───────────┘
                             │
                  ┌──────────▼───────────┐
                  │  Strategy Factory     │
                  │  create_strategy(r,   │
                  │    o, a)              │
                  └──────────┬───────────┘
                             │
         ┌───────────────────┼───────────────────────┐
         │                   │                       │
  ┌──────▼──────┐  ┌────────▼───────┐  ┌────────────▼──────┐  ┌──────▼──────┐
  │DirectStrategy│  │ResilientStrategy│ │OptimizedStrategy  │  │AsyncStrategy│
  │              │  │                 │  │                   │  │             │
  │send_command()│  │execute_with_    │  │send_command()     │  │send_command()│
  │直接执行       │  │retry() 重试执行  │  │+ 缓存 + 压缩      │  │标记异步模式   │
  └──────────────┘  └─────────────────┘  └───────────────────┘  └─────────────┘
```

**执行结果** (`result.py:ExecutionResult`) 包含完整的元数据：
- 成功/失败状态、执行时间、重试次数
- 缓存命中/压缩信息、超时类别
- 新旧格式兼容输出 (`to_legacy_format()`)

**Decision**: 策略工厂通过 `(resilient, optimize, async_mode)` 三元组选择策略，优先级为 `async > optimize > resilient > direct`。默认为 `OptimizedStrategy`。

#### 3.2.2 超时系统 (`config.py` + `execution/timeout_resolver.py`)

采用**两层超时决策**机制：

**第一层：命令类别 → 基础超时**

| 类别 | 超时(ms) | 典型命令 |
|------|----------|----------|
| quick | 10,000 | `version`, `help`, `r` |
| normal | 30,000 | `lm`, `k`, `dv`, `dt` |
| execution | 60,000 | `g`, `p`, `t`, `bp` |
| memory | 90,000 | `dd`, `dq`, `dp`, `da` |
| analysis | 120,000 | `!analyze`, `!thread`, `!process` |
| bulk | 180,000 | `lm`, `!dlls`, `!handle`, `!address` |
| large_analysis | 300,000 | `!analyze -v`, `!thread -1` |
| symbols | 300,000 | `.reload`, `.sympath`, `.symfix` |
| process_list | 480,000 | `!process 0 0`, `!process 0 7` |
| streaming | 900,000 | `!for_each_process`, `!for_each_thread` |
| extended | 1,200,000 | `.reload /f` |

**第二层：调试模式 → 超时乘数**

| 模式 | 乘数 | 说明 |
|------|------|------|
| LOCAL | 1.0x | 本机调试 |
| VM_SERIAL | 1.5x | VM 串口调试 |
| VM_NETWORK | 2.0x | VM 网络调试（默认调试方式） |
| REMOTE | 2.5x | 远程网络调试 |

**Decision**: 两层乘积决定最终超时。VM network 调试（本项目的典型场景）自动获得 2x 容错，减少误报超时。

#### 3.2.3 命令安全验证 (`core/validation.py`)

采用**白名单 + 黑名单**双重策略：

```
                    命令输入
                       │
              ┌────────▼────────┐
              │ 空命令/超长检查   │ → 拒绝: "Empty command"
              │ (>4096 chars)    │
              └────────┬────────┘
                       │
              ┌────────▼────────┐
              │ 提取 base cmd   │
              │ (第一个单词)      │
              └────────┬────────┘
                       │
         ┌─────────────▼──────────────┐
         │ 黑名单检查 (DANGEROUS_CMDS) │ → 拒绝: q, qq, qd, .kill, .detach,
         │                            │   .restart, .dump, .load/.unload,
         │                            │   .connect, .server, .logopen
         └─────────────┬──────────────┘
                       │
         ┌─────────────▼──────────────┐
         │ 白名单前缀 (ALWAYS_SAFE)    │ → 允许: lm, x, dt, dd, k, r, u,
         │                            │   !process, !thread, !object, etc.
         └─────────────┬──────────────┘
                       │
         ┌─────────────▼──────────────┐
         │ 特殊命令许可               │ → 允许: bp/ba/bc (断点),
         │                            │   g/p/t/gu (执行控制),
         │                            │   .thread/.process (上下文切换)
         └─────────────┬──────────────┘
                       │
         ┌─────────────▼──────────────┐
         │ 兜底策略 (default allow)    │ → 允许: 所有 ! 扩展命令、. 元命令、
         │                            │   其他未识别命令（日志记录）
         └────────────────────────────┘
```

**Decision**: 安全策略偏宽松——只阻止真正危险的操作，允许 AI 助手进行断点设置、执行控制、上下文切换等交互式调试工作流。

#### 3.2.4 命名管道通信 (`core/communication.py`)

三层封装结构：

```
┌───────────────────────────────────────────────────────┐
│ CommunicationManager (单例)                            │
│ • 连接池管理 (ConnectionPool)                          │
│ • 健康监控 (ConnectionHealth)                          │
│ • 自动重连 + 诊断                                     │
└──────────────┬────────────────────────────────────────┘
               │
┌──────────────▼────────────────────────────────────────┐
│ MessageProtocol                                        │
│ • create_command_message(cmd, timeout) → dict          │
│ • serialize_message(msg) → bytes                      │
│ • parse_response(bytes) → dict                        │
│ 消息格式: {id, command, timeout_ms, timestamp}         │
└──────────────┬────────────────────────────────────────┘
               │
┌──────────────▼────────────────────────────────────────┐
│ NamedPipeProtocol                                      │
│ • connect_to_pipe(name, timeout) → handle              │
│ • write_to_pipe(handle, data)                          │
│ • read_from_pipe(handle) → bytes                       │
│ 基于 pywin32 (win32pipe/win32file/win32event)         │
└───────────────────────────────────────────────────────┘
```

**Decision**: 连接池设计支持多线程并发访问命名管道。使用 `win32event` 实现异步等待，缓冲区 8KB。

#### 3.2.5 调试上下文管理 (`core/context.py`)

采用**栈式上下文管理**，支持嵌套的进程/线程切换：

```
ContextManager (单例)
  ├── _context_stack: List[DebugContext]   ← 上下文栈
  └── _current_context: DebugContext       ← 当前上下文

DebugContext:
  ├── process_address: Optional[str]       ← ".process" 提取
  └── thread_address: Optional[str]        ← ".thread" 提取

API:
  push_context(comm_func) → DebugContext   ← 保存当前上下文并入栈
  pop_context(comm_func)  → bool           ← 恢复栈顶上下文
  switch_to_process(addr, comm_func)       ← 切换到指定进程
  switch_to_thread(addr, comm_func)        ← 切换到指定线程
  restore_context(ctx, comm_func)          ← 恢复到指定上下文

装饰器:
  @with_saved_context(comm_func)           ← 自动保存/恢复上下文
```

**Decision**: 在 `run_sequence` 和 `breakpoint_and_continue` 中自动保存上下文，防止序列操作破坏调试环境。WinDbg 的 `.process /r /p` 和 `.thread` 命令用于上下文的持久化和恢复。

#### 3.2.6 统一缓存系统 (`core/unified_cache.py`)

四种缓存上下文，不同生命周期：

| 上下文 | 生命周期 | 用途 |
|--------|----------|------|
| STARTUP | 启动后清除 | 连接测试、版本信息 |
| COMMAND | TTL 1小时 | 命令结果缓存 |
| SESSION | TTL 5分钟 | 会话快照 |
| PERFORMANCE | TTL 可配置 | 性能优化缓存 |

缓存特性：
- 基于 `OrderedDict` 的 LRU 淘汰（max 100 条目）
- 支持优先级标记 (LOW/NORMAL/HIGH/CRITICAL)
- 可选 gzip 压缩存储
- 线程安全（threading.Lock）
- 自动过期清理（5分钟间隔）

#### 3.2.7 会话恢复 (`core/session_recovery.py`)

```
SessionState 枚举:
  ACTIVE → INTERRUPTED → RECOVERING → ACTIVE  (正常恢复)
  ACTIVE → LOST → MANUAL_INTERVENTION         (需人工干预)

RecoveryStrategy:
  RECONNECT_ONLY      ← 仅重新连接管道
  RESTORE_CONTEXT     ← 连接 + 恢复调试上下文
  FULL_RECOVERY       ← 连接 + 上下文 + 断点 + 符号
  MANUAL_INTERVENTION ← 自动恢复失败，需手动处理

SessionSnapshot:
  timestamp, session_id, debugging_mode
  target_info, current_process, current_thread
  loaded_modules, symbol_path, breakpoints
```

**Decision**: 会话快照每5分钟自动保存到 `session_snapshots/` 目录。恢复时首先尝试 `RECONNECT_ONLY`，失败后逐级提升恢复策略，最多重试3次。

#### 3.2.8 异步任务管理 (`core/async_ops/`)

```
┌──────────────────────────────────┐
│ AsyncOperationManager            │
│                                  │
│ • ThreadPoolExecutor(max=5)      │
│ • Priority Queue (4级)           │
│ • Task Lifecycle (PENDING →     │
│   RUNNING → COMPLETED/FAILED    │
│   /CANCELLED)                    │
│ • Task ID 追踪 + 结果查询        │
│ • 统计 + 监控                    │
└──────────────────────────────────┘

TaskPriority: LOW(1) → NORMAL(2) → HIGH(3) → CRITICAL(4)

BatchCommandExecutor:
  • 并行执行多条独立命令
  • stop_on_error 控制故障传播
```

#### 3.2.9 性能优化子系统 (`core/performance/`)

```
PerformanceOptimizer (协调器)
  ├── CommandOptimizer    ← 命令级缓存 + 优化
  ├── DataCompressor      ← 大输出 gzip 压缩
  ├── StreamingHandler    ← 大输出流式传输
  └── PerformanceMetrics  ← 实时指标收集

优化级别:
  NONE → BASIC → AGGRESSIVE → MAXIMUM

BYPASS 规则（以下命令跳过优化直接执行）:
  .reload /f, .restart, g/p/t, bp/bc/bd/be,
  .attach, .detach, .symfix, .sympath,
  .process /i, ~, .context, 内存写命令(ed/ew/eb/eq)
```

#### 3.2.10 增强错误处理 (`core/error_handler.py`)

```
ErrorCategory 分类:
  CONNECTION  ← 连接失败 → 建议检查 WinDbg 和 !mcpstart
  VALIDATION  ← 命令校验失败 → 列出安全替代命令
  PARAMETER   ← 参数缺失/错误 → 显示 get_parameter_help()
  CONTEXT     ← 上下文错误 → 建议切换进程/线程
  TIMEOUT     ← 超时 → 建议调整 timeout 或检查网络
  PERMISSION  ← 权限错误 → 建议提升权限
  SYMBOL      ← 符号问题 → 建议 .reload /f
  MEMORY      ← 内存访问错误 → 检查地址有效性
  WORKFLOW    ← 调试流程错误 → 引导正确调试步骤

DebugContext 上下文感知:
  KERNEL_MODE ← 内核调试特有建议
  USER_MODE   ← 用户态特有建议
  BREAKPOINT_HIT / EXCEPTION_OCCURRED / ...

响应结构:
  {error, category, suggestions[], examples[], next_steps[],
   related_tools[], debug_context}
```

---

## 4. C++ WinDbg Extension 层详解

### 4.1 启动流程

```
WinDbg 命令:
  .load windbgmcpExt.dll               ← DebugExtensionInitialize()
  !mcpstart                            ← MCPServer::Start()
                                        └── 创建 PipeServerThread
                                            └── 循环 CreatePipeInstance()
                                                └── 每个客户端: 创建 ClientConnection
                                                    └── 启动 thread: HandleClient()

工具命令:
  !mcpstop                             ← MCPServer::Stop()
  !mcpstatus                           ← 查询运行状态
```

### 4.2 Named Pipe Server 设计

```
MCPServer (single instance)
│
├── m_serverThread: PipeServerThread()
│   └── loop:
│       ├── CreateNamedPipeA()          ← 创建管道实例
│       │   (PIPE_ACCESS_DUPLEX,       ← 双向通信
│       │    PIPE_TYPE_MESSAGE,        ← 消息模式（非字节流）
│       │    PIPE_READMODE_MESSAGE)    ← 消息读取模式
│       ├── ConnectNamedPipe()          ← 等待客户端连接
│       ├── CreateThread() → HandleClient()  ← 每个客户端独立线程
│       └── CleanupDisconnectedClients()
│
├── m_clients: vector<shared_ptr<ClientConnection>>
│   └── 每个 ClientConnection:
│       ├── hPipe: HANDLE              ← 管道句柄
│       ├── thread: std::thread        ← 处理线程
│       ├── active: atomic<bool>       ← 连接状态
│       ├── outgoingMessages: queue<json>  ← 发送队列
│       ├── queueMutex + queueCondition    ← 线程同步
│
└── m_handlers: map<string, MessageHandler>
    └── 命令 → 处理函数 注册表

ClientConnection 生命周期:
  HandleClient() {
    while (active) {
      ReadFile(hPipe, buffer, BUFFER_SIZE)  ← 阻塞读取 JSON 消息
        → json::parse(buffer)
        → response = ProcessMessage(msg)     ← 路由到注册的 handler
        → 序列化 response → WriteFile(hPipe)  ← 返回结果
    }
    DisconnectNamedPipe() + CloseHandle()
  }
```

**Decision**: 使用 `PIPE_TYPE_MESSAGE` 模式确保每次 `ReadFile` 读取完整的一条 JSON 消息，无需手动处理消息边界。

### 4.3 Command Handlers 架构

```
CommandRegistry::RegisterAllHandlers(server)
│
├── RegisterBasicHandlers()
│   ├── "check_connection" → CheckConnectionHandler
│   ├── "version" → VersionHandler
│   ├── "get_metadata" → GetMetadataHandler
│   ├── "list_modules" → ListModulesHandler
│   ├── "display_type" → DisplayTypeHandler
│   └── "display_memory" → DisplayMemoryHandler
│
├── RegisterEnhancedHandlers()
│   ├── "execute_command" → ExecuteCommandHandler
│   ├── "execute_command_enhanced" → ExecuteCommandEnhancedHandler
│   ├── "execute_command_streaming" → ExecuteCommandStreamingHandler
│   └── "for_each_module" → ForEachModuleHandler
│
├── RegisterDiagnosticHandlers()
│   ├── "health_check" → HealthCheckHandler
│   └── "performance_metrics" → PerformanceMetricsHandler
│
└── RegisterSpecializedHandlers()
    ├── "handle_process" → HandleProcessCommand
    ├── "handle_dlls" → HandleDllsCommand
    └── "handle_address" → HandleAddressCommand
```

**核心执行路径** (`ExecuteCommandEnhancedHandler`)：

```
ExecuteCommandEnhancedHandler(message)
  ├── 提取 command + id + timeout
  ├── CategorizeCommand(cmd)           ← 命令分类 (Quick/Normal/Slow/Analysis/Bulk)
  ├── GetTimeoutForCategory(cat)       ← 获取超时值
  ├── ExecuteWithTimeout(cmd, timeout) ← 超时执行
  │   └── ExecuteWinDbgCommand(cmd, timeout)
  │       └── IDebugControl::Execute()        ← WinDbg 引擎 API
  │           └── IDebugOutputCallbacks 捕获输出
  ├── ClassifyError(output, hr)        ← 错误分类 (CommandSyntax/Timeout/...)
  └── CreateEnhancedErrorResponse() 或 CreateSuccessResponseWithMetadata()
```

### 4.4 错误分类与超时管理 (C++ 层)

C++ 侧同样实现了超时分类和错误分类，与 Python 侧形成双层防护：

```
TimeoutCategory (C++):
  Quick(10s), Normal(30s), Slow(60s), Bulk(120s), Analysis(180s)

ErrorCategory (C++):
  CommandSyntax, PermissionDenied, ResourceExhaustion,
  ConnectionLost, Timeout, ExecutionContext, InternalError, Unknown

CommandUtilities 提供的共享能力:
  • ExecuteWinDbgCommand()     ← 调用 IDebugControl::Execute()
  • ExecuteWithTimeout()       ← 带超时的安全执行
  • CategorizeCommand()        ← 命令分类
  • ClassifyError()            ← 错误分析 + 建议生成
  • 响应格式化 (Success / Error / Enhanced / Detailed)
  • 性能指标跟踪 (g_lastExecutionTime, g_lastCommandTime)
  • 会话 ID 管理
```

---

## 5. MCP 工具设计（15 个工具）

### 5.1 工具分类与职责

```
Session Management (3 tools)
├── debug_session          ← 会话状态、连接信息、版本
├── connection_manager     ← 连接测试、状态检查
└── session_manager        ← 会话状态管理

Command Execution (3 tools)
├── run_command            ← 核心：执行单个 WinDbg 命令
├── run_sequence           ← 批量执行命令序列
└── breakpoint_and_continue ← 断点设置 + 执行控制

Analysis (4 tools)
├── analyze_process        ← process list/switch/info/PEB
├── analyze_thread         ← thread list/switch/info/stack/TEB
├── analyze_memory         ← display/type/search/PTE/regions
└── analyze_kernel         ← object/IDT/handles/interrupts/modules

Performance (2 tools)
├── performance_manager    ← 优化级别/缓存/流式/基准测试
└── async_manager          ← 提交/状态/结果/并行/取消

Support (3 tools → 实际注册 4)
├── troubleshoot           ← symbols/exception/analyze/connection
├── get_help               ← 工具帮助、参数说明
├── test_windbg_communication ← 管道通信测试
└── network_debugging_troubleshoot ← 网络调试专项诊断
```

### 5.2 run_command 工具执行流程

```
run_command(action="", command="k", validate=True, resilient=True, optimize=True)
│
├── 1. validate_tool_parameters()         ← 参数校验
├── 2. validate_command("k")             ← 安全检查 (黑名单+白名单)
├── 3. is_safe_for_automation("k")       ← 自动化安全检查
├── 4. execute_unified(command, resilient, optimize)
│   └── UnifiedCommandExecutor.execute()
│       ├── TimeoutResolver → 30s (normal, VM_NETWORK=2x → 60s)
│       ├── Strategy = OptimizedStrategy
│       │   └── send_command("k", 60000)
│       │       └── NamedPipeProtocol → WriteFile → ReadFile
│       │           └── C++ Extension → IDebugControl::Execute("k")
│       └── ExecutionResult {success, result, metadata...}
├── 5. 成功 → to_legacy_format()
│   失败 → enhance_error() + 元数据
```

---

## 6. 关键设计决策记录

### 6.1 为什么选择 Named Pipe 而非 HTTP/WebSocket？

- WinDbg Extension DLL 运行在 WinDbg 进程内，无法方便地启动 HTTP 服务器
- Named Pipe 是 Windows 内核对象，零网络栈开销，延迟最低
- 支持双向通信和消息模式（`PIPE_TYPE_MESSAGE`），天然适合 JSON 消息
- 无需处理端口冲突、防火墙等问题

### 6.2 为什么是双层超时（Python + C++）？

- **Python 层**：基于命令类别的粗粒度超时 + 调试模式乘数 → 提前终止，避免长时间阻塞 MCP 客户端
- **C++ 层**：基于 `WinDbg Engine API` 的精确超时控制 → 防止 WinDbg 内部挂起
- 双层机制相互独立，任一层超时都能正确终止执行

### 6.3 为什么策略工厂按 async > optimize > resilient > direct 优先级选择？

- `AsyncStrategy` 是独立模式，优先检查
- `OptimizedStrategy` 包含缓存/压缩，是默认生产模式
- `ResilientStrategy` 添加重试，但无缓存优化
- `DirectStrategy` 是最简实现，用于自测和回退场景

### 6.4 为什么上下文管理用栈而非简单切换？

- 允许嵌套操作：AI 可能深层探索多线程调用栈
- 自动恢复：`run_sequence` 末尾自动恢复到操作前的上下文
- 装饰器模式：`@with_saved_context` 无侵入式保护

### 6.5 为什么使用单例模式（CommunicationManager / ContextManager / Executor / Cache）？

- 命名管道是稀缺资源（WinDbg Extension 只托管一个 pipe server）
- 调试上下文全局唯一
- 缓存跨工具共享
- 但单例不暴露构造函数，通过 `get_xxx()` 工厂函数获取，保留了可测试性

---

## 7. 数据流示例：一次典型的 "查看调用栈" 请求

```
User: "Show me the call stack"
  │
  ▼
MCP Client → JSON-RPC: {method: "tools/call", params: {name: "run_command",
                         arguments: {command: "k"}}}
  │
  ▼
FastMCP → run_command(command="k")
  │
  ▼
validate_command("k") → True (以 "k" 开头的命令在白名单中)
  │
  ▼
execute_unified("k", resilient=True, optimize=True)
  │
  ▼
TimeoutResolver: "k" ∈ NORMAL_COMMANDS → 30s × VM_NETWORK(2.0) = 60s
  │
  ▼
OptimizedStrategy.execute()
  ├── 检查缓存 → 未命中
  └── send_command("k", 60000)
      │
      ▼
NamedPipeProtocol:
  ├── MessageProtocol.create_command_message("k", 60000)
  │   → {id: 42, command: "k", timeout_ms: 60000, timestamp: "..."}
  ├── serialize_message → json bytes
  ├── WriteFile(pipe_handle, data)
  ├── ReadFile(pipe_handle, buffer, 8192)
  └── MessageProtocol.parse_response(bytes)
      → {status: "success", output: "# ChildEBP RetAddr  ...", ...}
      │
      ▼
C++ Extension (pipe server:
  ├── ReadFile(hPipe, buffer)
  ├── json::parse(buffer)
  ├── ExecuteCommandHandler(msg)
  │   ├── Extract: command="k", timeout=60000
  │   ├── CategorizeCommand("k") → TimeoutCategory::Quick
  │   ├── GetTimeoutForCategory(Quick) → 10000 (C++ 侧更保守)
  │   ├── ExecuteWithTimeout("k", 10000)
  │   │   └── IDebugControl::Execute(OUTPUT_NORMAL, "k")
  │   │       └── OutputCallbacks::Output() 收集文本
  │   └── json response → WriteFile(hPipe)
  │
  ▼
OptimizedStrategy:
  ExecutionResult(success=True, result="ChildEBP RetAddr  \n...",
                  execution_time=0.35, timeout_category="normal",
                  timeout_ms=60000, execution_mode="optimized")
  │
  ▼
run_command → to_legacy_format()
  {success: True, result: "ChildEBP RetAddr  \n...",
   execution_method: "optimized", performance_info: {response_time: 0.35, ...}}
  │
  ▼
FastMCP → MCP Client → Displayed to User
```

---

## 8. 部署架构

```
                     Host Machine (Windows)
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  ┌─────────────────────────────────────┐                    │
│  │ MCP Client (VS Code / Cursor / ...) │                    │
│  │   └── spawns: python -m mcp_server  │                    │
│  └────────────────┬────────────────────┘                    │
│                   │ stdio                                   │
│  ┌────────────────▼────────────────────┐                    │
│  │ Python MCP Server                    │                    │
│  │ (FastMCP, 独立进程)                   │                    │
│  └────────────────┬────────────────────┘                    │
│                   │ \\.\pipe\windbgmcp                      │
│  ┌────────────────▼────────────────────┐                    │
│  │ WinDbg (x64)                         │                    │
│  │  └── windbgmcpExt.dll (loaded)       │                    │
│  │      └── !mcpstart                   │                    │
│  └────────────────┬────────────────────┘                    │
│                   │ Debugging Engine                        │
│  ┌────────────────▼────────────────────┐                    │
│  │ Target: Local Kernel / VM / Dump     │                    │
│  └─────────────────────────────────────┘                    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**依赖要求**：
- Python ≥ 3.10 + pywin32==310 + fastmcp==2.5.1
- Visual Studio 2022 Build Tools (仅编译时)
- Windows SDK Debugging Tools (dbgeng.lib, DbgEng.h)
- WinDbg x64 (Windows SDK 自带)

---

## 9. 测试架构

```
Selftest (poetry run selftest)
├── _roundtrip_message()        ← stub 管道，验证 MessageProtocol 序列化
├── _stubbed_executor()         ← stub send_command，验证 executor 链路
└── _tools_import()             ← 验证所有工具注册无异常

Pytest Suite (poetry run pytest)
├── test_unified_execution.py   ← 执行系统单元测试 (13KB, 最完整)
├── test_command_validation.py  ← 安全验证测试
├── test_message_protocol.py    ← 消息协议测试
├── test_timeout_resolver.py    ← 超时解析测试
├── test_tools_registry.py      ← 工具注册测试
└── test_install_client_config_dryrun.py ← 安装脚本测试

覆盖配置: source=mcp_server, HTML报告输出到 htmlcov/
```

Selftest 的关键设计：通过 **monkey-patch** `send_command` 函数，不依赖实际的 WinDbg/命名管道即可验证整个 Python 层的核心链路。

---

## 10. 扩展点与未来方向

| 扩展点 | 位置 | 说明 |
|--------|------|------|
| 新增 MCP 工具 | `mcp_server/tools/` | 创建 `xxx_tools.py` → `register_xxx_tools()` → 在 `__init__.py` 注册 |
| 新增 C++ 命令处理 | `extension/src/command/` | 添加 handler → `CommandRegistry::RegisterAllHandlers()` |
| 新增执行策略 | `mcp_server/core/execution/strategies.py` | 继承 `ExecutionStrategy` → 更新 `create_strategy()` |
| 新增超时类别 | `mcp_server/config.py` | 在 `TimeoutConfig` dataclass 添加新字段 |
| 新增调试模式 | `mcp_server/config.py` | 在 `DebuggingMode` enum 添加 + `TIMEOUT_MULTIPLIERS` |
| 新增安全白名单 | `mcp_server/core/validation.py` | 在 `ALWAYS_SAFE_PREFIXES` 添加 |
| 新增缓存上下文 | `mcp_server/core/unified_cache.py` | 在 `CacheContext` enum 添加 + 配置 TTL |