#include "process_utils.h"

#include <chrono>
#include <thread>

namespace vertex::shared {

std::wstring Utf8ToWide(const std::string& input) {
  if (input.empty()) {
    return L"";
  }

  int requiredLength = MultiByteToWideChar(
      CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0);
  if (requiredLength <= 0) {
    return L"";
  }

  std::wstring output(static_cast<size_t>(requiredLength), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), &output[0], requiredLength);
  return output;
}

std::string WideToUtf8(const std::wstring& input) {
  if (input.empty()) {
    return "";
  }

  int requiredLength = WideCharToMultiByte(
      CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
  if (requiredLength <= 0) {
    return "";
  }

  std::string output(static_cast<size_t>(requiredLength), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), &output[0], requiredLength, nullptr, nullptr);
  return output;
}

std::string GetSystemErrorMessage(DWORD errorCode) {
  LPWSTR messageBuffer = nullptr;
  DWORD messageLength = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      errorCode,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&messageBuffer),
      0,
      nullptr);

  if (messageLength == 0 || messageBuffer == nullptr) {
    return "Unknown error";
  }

  std::wstring message(messageBuffer, messageLength);
  LocalFree(messageBuffer);

  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
    message.pop_back();
  }

  return WideToUtf8(message);
}

std::wstring QuoteCommandArg(const std::wstring& arg) {
  if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
    return arg;
  }

  std::wstring quoted = L"\"";
  for (wchar_t ch : arg) {
    if (ch == L'"') {
      quoted += L"\\\"";
    } else {
      quoted += ch;
    }
  }
  quoted += L"\"";
  return quoted;
}

BOOL CALLBACK EnumWindowsFindMainProc(HWND hwnd, LPARAM lParam) {
  auto* context = reinterpret_cast<WindowSearchContext*>(lParam);
  DWORD windowPid = 0;
  GetWindowThreadProcessId(hwnd, &windowPid);

  if (windowPid != context->pid) {
    return TRUE;
  }

  if (!IsWindowVisible(hwnd)) {
    return TRUE;
  }

  if (GetWindow(hwnd, GW_OWNER) != nullptr) {
    return TRUE;
  }

  context->hwnd = hwnd;
  return FALSE;
}

HWND WaitForMainWindow(DWORD pid, DWORD timeoutMs) {
  DWORD waitedMs = 0;
  constexpr DWORD stepMs = 100;

  while (waitedMs <= timeoutMs) {
    WindowSearchContext context{pid, nullptr};
    EnumWindows(EnumWindowsFindMainProc, reinterpret_cast<LPARAM>(&context));
    if (context.hwnd != nullptr) {
      return context.hwnd;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    waitedMs += stepMs;
  }

  return nullptr;
}

bool BringWindowToTopmostFront(HWND hwnd) {
  if (hwnd == nullptr) {
    return false;
  }

  ShowWindow(hwnd, SW_RESTORE);
  SetWindowPos(
      hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  BringWindowToTop(hwnd);
  return SetForegroundWindow(hwnd) == TRUE;
}

void EmitProcessEvent(
    Napi::Env env,
    const Napi::Function& callback,
    const std::string& eventName,
    bool started,
    DWORD pid,
    bool windowActivated,
    bool watchSupported,
    DWORD exitCode,
    const std::string& message) {
  Napi::Object payload = Napi::Object::New(env);
  payload.Set("event", Napi::String::New(env, eventName));
  payload.Set("started", Napi::Boolean::New(env, started));
  payload.Set("pid", Napi::Number::New(env, static_cast<double>(pid)));
  payload.Set("windowActivated", Napi::Boolean::New(env, windowActivated));
  payload.Set("watchSupported", Napi::Boolean::New(env, watchSupported));
  payload.Set("exitCode", Napi::Number::New(env, static_cast<double>(exitCode)));
  payload.Set("message", Napi::String::New(env, message));
  callback.Call({payload});
}

void ParseArgsArray(const Napi::Array& arrayValue, std::vector<std::wstring>* args) {
  uint32_t length = arrayValue.Length();
  args->reserve(length);
  for (uint32_t i = 0; i < length; ++i) {
    Napi::Value item = arrayValue[i];
    if (!item.IsString()) {
      throw Napi::Error::New(arrayValue.Env(), "argsArray must contain only string items");
    }
    args->push_back(Utf8ToWide(item.As<Napi::String>().Utf8Value()));
  }
}

}  // namespace vertex::shared
