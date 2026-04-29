# 部署与使用指南

## 环境要求

| 组件 | 要求 | 说明 |
|------|------|------|
| Windows | 10/11 x64 | 仅支持 Windows |
| WinDbg | x64 版本 | Windows SDK "Debugging Tools for Windows" |
| Python | 3.10 ~ 3.x | 已验证 3.13/3.14 |
| pywin32 | 310 | named pipe 通信依赖 |
| fastmcp | 2.5.1 | MCP 框架 |

---

## 一、安装 Python 依赖

DLL 已预编译（`extension/build/x64/Release/windbgmcpExt.dll`），无需重新构建。

### 方式 A：使用 pip（推荐，无需 Poetry）

```powershell
pip install fastmcp==2.5.1 pywin32==310
```

### 方式 B：使用 Poetry

```powershell
# 安装 Poetry（如未安装）
(Invoke-WebRequest -Uri https://install.python-poetry.org -UseBasicParsing).Content | python -

# 在项目根目录安装依赖
cd E:\mcp\windbg-ext-mcp
poetry install
```

---

## 二、加载 WinDbg 扩展

在 WinDbg 命令栏执行：

```text
.load E:\mcp\windbg-ext-mcp\extension\build\x64\Release\windbgmcpExt.dll
```

加载成功后启动 pipe 服务端：

```text
!mcpstart
```

验证状态：

```text
!mcpstatus
```

> **注意**：WinDbg 必须是 x64 版本，与 DLL 架构一致。加载失败时检查路径和架构是否匹配。

### 扩展命令一览

| 命令 | 说明 |
|------|------|
| `!mcpstart` | 启动 named pipe 服务端（`\\.\pipe\windbgmcp`） |
| `!mcpstop` | 停止服务端 |
| `!mcpstatus` | 查看运行状态 |
| `!hello` | 连通性测试 |
| `!objecttypes` | 列出内核对象类型 |
| `!help` | 扩展帮助 |

---

## 三、MCP 服务器（无需手动启动）

**MCP 客户端会根据配置文件自动启动 Python 服务器进程，无需手动操作。**

客户端（Claude Desktop、Cursor 等）读取配置中的 `command` 字段，在需要时自动 spawn 服务器进程，通过 stdio 与其通信，会话结束后自动终止。

```
需要手动：  WinDbg → .load DLL → !mcpstart
自动完成：  MCP 客户端 → 启动 Python 服务器 → 连接 pipe → 调用工具
```

### 手动启动（仅用于调试/验证）

如需独立验证服务器是否正常，可手动启动：

**方式 A（pip 安装）**：

```powershell
cd E:\mcp\windbg-ext-mcp
python -m mcp_server.server
```

**方式 B（Poetry 安装）**：

```powershell
cd E:\mcp\windbg-ext-mcp
poetry run mcp
```

服务器通过 **stdio** 与 MCP 客户端通信，通过 `\\.\pipe\windbgmcp` 与 WinDbg 扩展通信。

### 验证服务器是否正常（无需 WinDbg）

```powershell
# pip 方式
python -m mcp_server.selftest

# Poetry 方式
poetry run selftest
```

输出 `Selftest OK` 即正常。

### 可选参数

```powershell
python -m mcp_server.server --list-tools   # 列出所有工具
python -m mcp_server.server --version      # 查看版本
```

---

## 四、配置 MCP 客户端

### 自动配置（推荐）

```powershell
# 预览将写入的配置（不实际修改）
python install_client_config.py --install --dry-run

# 正式写入（自动检测已安装的客户端）
python install_client_config.py --install

# 撤销配置
python install_client_config.py --uninstall
```

支持的客户端：Cursor、Claude Desktop、VS Code (Cline/Roo Code)、Windsurf

### 手动配置 Claude Desktop

编辑 `%APPDATA%\Claude\claude_desktop_config.json`：

```json
{
	"mcpServers": {
		"windbg": {
			"command": "C:\\Python314\\python.exe",
			"args": [
				"E:\\mcp\\windbg-ext-mcp\\mcp_server\\server.py"
			],
			"env": {
				"DEBUG": "false",
				"PYTHONUTF8": "1",
				"PYTHONIOENCODING": "utf-8",
				"FASTMCP_SHOW_SERVER_BANNER": "false",
				"FASTMCP_NO_BANNER": "1"
			},
			"description": "WinDbg MCP Server (FastMCP, stdio)",
			"disabled": false,
			"timeout": 30000,
			"autoApprove": [
				"debug_session",
				"connection_manager",
				"session_manager",
				"run_command",
				"run_sequence",
				"breakpoint_and_continue",
				"analyze_process",
				"analyze_thread",
				"analyze_memory",
				"analyze_kernel",
				"performance_manager",
				"async_manager",
				"troubleshoot",
				"get_help",
				"test_windbg_communication",
				"network_debugging_troubleshoot"
			],
			"alwaysAllow": [
				"debug_session",
				"connection_manager",
				"session_manager",
				"run_command",
				"run_sequence",
				"breakpoint_and_continue",
				"analyze_process",
				"analyze_thread",
				"analyze_memory",
				"analyze_kernel",
				"performance_manager",
				"async_manager",
				"troubleshoot",
				"get_help",
				"test_windbg_communication",
				"network_debugging_troubleshoot"
			]
		}
	}
}
```

---

## 五、调试模式与超时

通过环境变量控制行为：

```powershell
# 开启详细日志
$env:DEBUG = "true"
python -m mcp_server.server

# 关闭后恢复
Remove-Item Env:DEBUG
```

| 环境变量 | 值 | 效果 |
|----------|----|------|
| `DEBUG` | `true` | DEBUG 级别日志 |
| `VERBOSE` | `true` | INFO 级别日志 |

### 超时自动分级

服务器根据命令类型自动选择超时，无需手动配置：

| 场景 | 超时 |
|------|------|
| 快速命令（`version`、`r`） | 10s |
| 普通命令（`lm`、`k`） | 30s |
| 内存操作（`dd`/`dq`） | 90s |
| 分析命令（`!analyze`） | 120s |
| 批量操作（`!handle`、`!vm`） | 180s |
| 符号加载（`.reload`） | 300s |
| 进程枚举（`!process 0 0`） | 480s |
| 强制符号加载（`.reload /f`） | 1200s |

VM/远程调试时超时自动加倍（VM 网络 ×2，远程 ×2.5）。

---

## 六、完整启动流程

```
1. 打开 WinDbg (x64) 并附加目标
2. .load E:\mcp\windbg-ext-mcp\extension\build\x64\Release\windbgmcpExt.dll
3. !mcpstart
4. 打开 MCP 客户端（Cursor / Claude Desktop 等）   ← Python 服务器由客户端自动启动
5. 向客户端发送自然语言调试指令
```

步骤 1~3 需要手动操作，步骤 4 开始由客户端全程托管。

---

## 七、常见问题

**DLL 加载失败**
- 确认 WinDbg 是 x64 版本（`File → Open Executable` 标题栏可见）
- 确认路径没有中文或特殊字符

**Pipe 连接错误（`\\.\pipe\windbgmcp` 不存在）**
- 确认已执行 `!mcpstart`
- 确认 WinDbg 扩展已正确加载（`!mcpstatus` 输出 running）

**命令超时**
- VM 调试：超时已自动 ×2，若仍超时可设 `DEBUG=true` 查看实际耗时
- 符号未加载会导致分析命令极慢，先执行 `.symfix` + `.reload`

**Python 找不到模块**
- 确认在项目根目录（`E:\mcp\windbg-ext-mcp`）执行命令
- 确认 `pip install fastmcp==2.5.1 pywin32==310` 已成功
