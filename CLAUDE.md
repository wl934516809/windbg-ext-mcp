# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

WinDbg-ext-MCP bridges MCP-compatible AI coding assistants to the Windows Debugger (WinDbg). Two components:

- **C++ WinDbg extension DLL** (`extension/`) loads inside WinDbg, hosts a named pipe server at `\\.\pipe\windbgmcp`
- **Python MCP server** (`mcp_server/`) speaks FastMCP over stdio, relays validated commands to the extension over the named pipe

Author: NadavLorber, v0.1.0. Kernel-first design; user-mode debugging also supported.

## Development Commands

```bash
poetry install                    # Install Python dependencies
poetry run mcp                    # Run the MCP server (stdio)
poetry run selftest               # Hermetic self-test (no WinDbg required)
poetry run pytest                 # Run test suite with coverage
```

To build the C++ extension DLL (requires Visual Studio 2022 and Windows SDK Debugging Tools):
```powershell
msbuild extension\windbgmcpExt.sln /p:Configuration=Release /p:Platform=x64
```

## Architecture

```
MCP Client (stdio) <-> Python MCP Server (FastMCP) <-> WinDbg Extension DLL (named pipe) <-> WinDbg/Target
```

**Python MCP Server** (`mcp_server/`) — the "smart middleware":
- `server.py` — entry point; `WinDbgMCPServer` class bootstraps FastMCP and registers tools
- `config.py` — all timeouts, retry config, command categories, DebuggingMode enum. Uses module-level constants (not env vars) unless `load_environment_config()` is called; env vars `DEBUG=true` / `VERBOSE=true` control log level
- `core/communication.py` — `NamedPipeProtocol` and `CommunicationManager`; connects to `\\.\pipe\windbgmcp` via pywin32
- `core/validation.py` — blocks dangerous commands (`q`, `.kill`, `.detach`, `.restart`, `.dump`); allows safe prefixes and all extension commands (`!`). `validate_command()` returns `(bool, str|None)`
- `core/execution/` — `UnifiedCommandExecutor` with pluggable strategies (`DirectStrategy`, `ResilientStrategy`, `OptimizedStrategy`, `AsyncStrategy`)
- `core/context.py` — saves/restores debug context across process/thread switches
- `core/async_ops/` — async task submission with priority queues and batch execution
- `tools/` — registers 15 MCP tools organized in 5 categories (session, execution, analysis, performance, support). `register_all_tools(mcp)` wires everything to FastMCP

**C++ WinDbg Extension** (`extension/`) — lightweight DLL loaded via `.load`:
- `src/dllmain.cpp` — DLL entry; `src/ipc/mcp_server.cpp` — named pipe server with thread pool
- `src/command/` — command handler registry; `basic_command_handlers` (version, type/memory display), `enhanced_command_handlers` (command execution with timeout optimization, 28 KB), `diagnostic_command_handlers` (health checks)
- Constants in `src/utils/constants.h`: pipe name, categorized timeouts (10s–180s), 1MB max output
- Depends on Windows SDK Debugging Tools (`dbgeng.lib`, `DbgEng.h`, `WDBGEXTS.H`) and bundled `nlohmann/json`

**Install client config** — `install_client_config.py` auto-detects MCP clients (Claude Desktop, Cursor, VS Code Cline/Roo, Windsurf) and writes their config files.

## Key Design Patterns

- **Timeouts** are resolved per-command based on categories (quick=10s, normal=30s, memory=90s, analysis=120s, bulk=180s, symbols=300s, streaming=900s, extended=1200s). A mode multiplier is then applied: Local=1x, VM network=2x, VM serial=1.5x, Remote=2.5x
- **Retry** uses exponential backoff with 3 attempts, 1s base delay, 30s cap
- Dangerous commands are blocked at the Python layer before reaching WinDbg; safe prefixes and `!`-extension commands are always allowed
- **Self-test** (`selftest.py`) stubs the pipe transport and validates message roundtrips, executor output, and tool registration — runs with no WinDbg required