# Bug Fix: Invalid UTF-8 Crash on Chinese Windows (GBK → UTF-8)

## 问题描述

在中文 Windows 系统上运行 WinDbg MCP Extension 时，控制台持续输出：

```
MCPServer: Waiting for client connection on \\.\pipe\windbgmcp
MCPServer: Error processing message: [json.exception.type_error.316] invalid UTF-8 byte at index 366: 0xDA
MCPServer: Error processing message: [json.exception.type_error.316] invalid UTF-8 byte at index 366: 0xDA
```

所有包含中文字符的 WinDbg 命令响应被静默丢弃，客户端收不到任何数据。

## 根本原因

**调用链：**

```
WinDbg 调试目标（含中文符号/路径）
  → IDebugOutputCallbacks::Output(PCSTR Text)
      ↓ Text 编码 = 系统 ANSI 代码页（中文 Windows = GBK/CP936）
  → OutputCallbacks::m_output.append(Text)   ← 原始 GBK 字节进入 std::string
  → CommandUtilities::CreateSuccessResponse(output)
  → json["output"] = output                  ← GBK 字节进入 JSON 字符串值
  → json::dump()                             ← nlohmann/json 3.12 校验 UTF-8，抛出
      json.exception.type_error.316: invalid UTF-8 byte at index N: 0xDA
  → catch(std::exception& e) in HandleClient
  → dprintf("Error processing message: %s\n", e.what())  ← 响应丢失
```

**关键事实：**
- WinDbg `IDebugOutputCallbacks` 回调以 `PCSTR`（窄字符）传递文本，编码由**进程 ANSI 代码页**决定
- 中文 Windows 默认代码页为 **CP936（GBK）**，其双字节字符包含 0x80–0xFE 范围的字节（如 0xDA）
- nlohmann/json 3.12 的 `dump()` 在遇到非 UTF-8 字节时抛出 `type_error.316`
- 异常在 `HandleClient` 的 catch 块中被捕获但响应已丢失

## 修复方案

### 主修复：`extension/src/utils/output_callbacks.cpp`

在 `OutputCallbacks::Output()` 中，于文本追加到缓冲区之前，通过 Win32 API 将 ANSI 字节转换为 UTF-8：

```cpp
static std::string AnsiToUtf8(const char* ansiStr) {
    if (!ansiStr || *ansiStr == '\0') return {};

    // Step 1: ANSI (CP_ACP) → UTF-16
    int wLen = MultiByteToWideChar(CP_ACP, 0, ansiStr, -1, nullptr, 0);
    if (wLen <= 0) return ansiStr;
    std::wstring wide(wLen - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansiStr, -1, &wide[0], wLen);

    // Step 2: UTF-16 → UTF-8
    int u8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8Len <= 0) return ansiStr;
    std::string utf8(u8Len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], u8Len, nullptr, nullptr);
    return utf8;
}

// 在 Output() 中：
const std::string textStr = AnsiToUtf8(Text);  // 原为 std::string textStr(Text)
```

**转换路径：** GBK → UTF-16 → UTF-8，使用系统代码页（`CP_ACP`）确保在任何语言的 Windows 上都正确工作。

### 安全兜底：`extension/src/ipc/mcp_server.cpp`

所有 `json::dump()` 调用改为使用 `error_handler_t::replace`，将任何漏网的非 UTF-8 字节替换为 U+FFFD（`?`）而非抛出异常：

```cpp
// 原：message.dump()
message.dump(-1, ' ', false, json::error_handler_t::replace)
```

涉及三处：发送响应、发送错误响应、发送推送消息。

## 受影响文件

| 文件 | 变更 |
|------|------|
| `extension/src/utils/output_callbacks.cpp` | 新增 `AnsiToUtf8()`，`Output()` 调用该函数 |
| `extension/src/ipc/mcp_server.cpp` | 3 处 `dump()` 改为 `replace` 错误处理模式 |

## 验证方法

1. 在中文 Windows 系统上加载扩展
2. 执行包含中文路径或符号的命令，例如 `lm`（模块列表）或 `!process 0 0`
3. 确认不再出现 `type_error.316` 错误
4. 确认客户端能正常收到包含中文内容的 JSON 响应

## 提交信息

```
Fix invalid UTF-8 crash on Chinese Windows (GBK output from WinDbg)

WinDbg delivers IDebugOutputCallbacks::Output() text in the process ANSI
codepage (GBK/CP936 on Chinese Windows).  nlohmann/json 3.12 rejects
non-UTF-8 bytes in string values, causing json::dump() to throw
json.exception.type_error.316 and silently drop every response that
contained CJK characters.

Primary fix: add AnsiToUtf8() in OutputCallbacks::Output() that converts
PCSTR via MultiByteToWideChar(CP_ACP) → WideCharToMultiByte(CP_UTF8)
before appending to the output buffer.

Safety net: all json::dump() call sites in mcp_server.cpp now pass
error_handler_t::replace so any residual invalid bytes are substituted
with U+FFFD instead of throwing.
```
