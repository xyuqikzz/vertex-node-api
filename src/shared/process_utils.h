#pragma once

#include <napi.h>
#include <windows.h>

#include <string>
#include <vector>

namespace vertex::shared {

struct WindowSearchContext {
  DWORD pid;
  HWND hwnd;
};

struct ProcessCloseEvent {
  DWORD pid;
  DWORD exitCode;
};

std::wstring Utf8ToWide(const std::string& input);
std::string WideToUtf8(const std::wstring& input);
std::string GetSystemErrorMessage(DWORD errorCode);
std::wstring QuoteCommandArg(const std::wstring& arg);
BOOL CALLBACK EnumWindowsFindMainProc(HWND hwnd, LPARAM lParam);
HWND WaitForMainWindow(DWORD pid, DWORD timeoutMs);
bool BringWindowToTopmostFront(HWND hwnd);
void EmitProcessEvent(
    Napi::Env env,
    const Napi::Function& callback,
    const std::string& eventName,
    bool started,
    DWORD pid,
    bool windowActivated,
    bool watchSupported,
    DWORD exitCode,
    const std::string& message);
void ParseArgsArray(const Napi::Array& arrayValue, std::vector<std::wstring>* args);

}  // namespace vertex::shared
