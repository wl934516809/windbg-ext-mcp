/**
 * @file output_callbacks.cpp
 * @brief Implementation of OutputCallbacks class for capturing WinDbg output.
 */
#include "pch.h"
#include "utils/output_callbacks.h"
#include <algorithm>

// Convert a system ANSI (CP_ACP) string to UTF-8.
// WinDbg output callbacks deliver PCSTR in the process ANSI codepage, which on
// Chinese Windows is GBK/CP936.  nlohmann/json requires all string values to be
// valid UTF-8, so we must convert before storing the text.
static std::string AnsiToUtf8(const char* ansiStr) {
    if (!ansiStr || *ansiStr == '\0') return {};

    // Step 1: ANSI → UTF-16
    int wLen = MultiByteToWideChar(CP_ACP, 0, ansiStr, -1, nullptr, 0);
    if (wLen <= 0) return ansiStr; // conversion failed – return as-is (best effort)

    std::wstring wide(wLen - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansiStr, -1, &wide[0], wLen);

    // Step 2: UTF-16 → UTF-8
    int u8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8Len <= 0) return ansiStr;

    std::string utf8(u8Len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], u8Len, nullptr, nullptr);
    return utf8;
}

OutputCallbacks::OutputCallbacks(bool echoToWindbg)
    : m_refCount(1)
    , m_echoToWindbg(echoToWindbg) {
}

OutputCallbacks::~OutputCallbacks() = default;

// IUnknown methods
STDMETHODIMP OutputCallbacks::QueryInterface(
    __in REFIID InterfaceId,
    __out PVOID* Interface
) {
    if (!Interface) {
        return E_POINTER;
    }

    *Interface = nullptr;

    if (IsEqualIID(InterfaceId, __uuidof(IUnknown)) ||
        IsEqualIID(InterfaceId, __uuidof(IDebugOutputCallbacks))) {
        *Interface = this;
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) OutputCallbacks::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) OutputCallbacks::Release() {
    LONG retVal = InterlockedDecrement(&m_refCount);
    if (retVal == 0) {
        delete this;
    }
    return retVal;
}

// IDebugOutputCallbacks methods
STDMETHODIMP OutputCallbacks::Output(
    __in ULONG Mask,
    __in PCSTR Text
) {
    // Append the output text to our buffer
    if (!Text) {
        return S_OK;
    }

    // Echo to WinDbg window if enabled, with thread_local reentry guard.
    // dprintf triggers Output() again, so the guard breaks the cycle after one hop.
    thread_local bool s_inOutputEcho = false;
    if (m_echoToWindbg && !s_inOutputEcho && *Text != '\0') {
        s_inOutputEcho = true;
        dprintf("%s", Text);
        s_inOutputEcho = false;
    }

    const std::string textStr = AnsiToUtf8(Text);

    // Check for various types of messages
    if (textStr.find("WARNING: .cache forcedecodeuser is not enabled") != std::string::npos) {
        // This is a common warning, not a fatal error - log but continue
        m_output += "Note: " + textStr + "\n";
    }
    else if (textStr.find("is not extension gallery command") != std::string::npos) {
        // Extract the command name
        const size_t pos = textStr.find(" is not extension gallery command");
        if (pos != std::string::npos) {
            const std::string cmdName = textStr.substr(0, pos);
            if (!m_extensionError) {
                // Provide a more helpful error message
                if (cmdName == "modinfo") {
                    m_output += "Note: The !modinfo command is not available. Using alternative lmv command instead.\n";
                } else {
                    m_output += "Error: Command '" + cmdName + "' is not available. Make sure the required extension is loaded.\n";
                }
                m_extensionError = true;
            }
        }
        else {
            m_output += textStr;
        }
    }
    else if (textStr.find("No export") != std::string::npos && textStr.find("found") != std::string::npos) {
        // Handle "No export" errors
        const size_t pos = textStr.find(" found");
        if (pos != std::string::npos) {
            const std::string cmdName = textStr.substr(9, pos - 9);  // Extract name after "No export "
            if (!m_exportError) {
                m_output += "Note: Command '" + cmdName + "' is not available in the current debugging context.\n";
                m_exportError = true;
            }
        }
        else {
            m_output += textStr;
        }
    }
    else {
        m_output += textStr;
    }
    
    return S_OK;
}

std::string OutputCallbacks::GetOutput() const {
    // If the output exceeds a reasonable size, truncate it and add a note
    if (m_output.size() > MAX_OUTPUT_SIZE) {
        std::string truncated = m_output.substr(0, MAX_OUTPUT_SIZE);
        truncated += "\n[Output truncated. Result too large (exceeded " + 
                     std::to_string(MAX_OUTPUT_SIZE) + " bytes)]";
        return truncated;
    }
    return m_output;
}

void OutputCallbacks::Clear() {
    m_output.clear();
    m_extensionError = false;
    m_exportError = false;
} 