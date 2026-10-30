#include <napi.h>
#include <windows.h>
#include <shellapi.h>

#include <thread>
#include <vector>

#include "../../shared/process_utils.h"

namespace {

struct LaunchRequest {
  std::wstring target;
  std::vector<std::wstring> args;
  Napi::Function callback;
};

struct ProcessStartEvent {
  DWORD pid;
  bool windowActivated;
  bool watchSupported;
  std::string message;
};

bool IsProtocolTarget(const std::wstring& target) {
  const size_t delimiterIndex = target.find(L"://");
  if (delimiterIndex == std::wstring::npos || delimiterIndex == 0) {
    return false;
  }

  for (size_t index = 0; index < delimiterIndex; ++index) {
    const wchar_t ch = target[index];
    const bool isAlpha = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
    const bool isDigit = ch >= L'0' && ch <= L'9';
    const bool isAllowedSymbol = ch == L'+' || ch == L'-' || ch == L'.';
    if (!isAlpha && !isDigit && !isAllowedSymbol) {
      return false;
    }
  }

  return true;
}

std::wstring BuildShellParameterLine(const std::vector<std::wstring>& args) {
  std::wstring parameters;
  for (size_t index = 0; index < args.size(); ++index) {
    if (index > 0) {
      parameters += L" ";
    }
    parameters += vertex::shared::QuoteCommandArg(args[index]);
  }
  return parameters;
}

LaunchRequest ParseLaunchRequest(
    const Napi::CallbackInfo& info, const std::string& methodName) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsString()) {
    Napi::TypeError::New(
        env,
        methodName +
            " requires: " + methodName +
            "(target, callback) or " + methodName + "(target, argsArray, callback)")
        .ThrowAsJavaScriptException();
    return {L"", {}, Napi::Function()};
  }

  LaunchRequest request{
      vertex::shared::Utf8ToWide(info[0].As<Napi::String>().Utf8Value()),
      {},
      Napi::Function()};

  if (info[1].IsFunction()) {
    request.callback = info[1].As<Napi::Function>();
    return request;
  }

  if (info[1].IsArray() && info.Length() >= 3 && info[2].IsFunction()) {
    vertex::shared::ParseArgsArray(info[1].As<Napi::Array>(), &request.args);
    request.callback = info[2].As<Napi::Function>();
    return request;
  }

  Napi::TypeError::New(
      env, methodName + " requires callback function as second or third argument")
      .ThrowAsJavaScriptException();
  return {L"", {}, Napi::Function()};
}

void DispatchStartedEventAsync(
    Napi::Env env,
    const Napi::Function& callback,
    DWORD pid,
    bool windowActivated,
    bool watchSupported,
    const std::string& message) {
  Napi::ThreadSafeFunction tsfn = Napi::ThreadSafeFunction::New(
      env, callback, "process-start-dispatcher", 0, 1);

  std::thread startThread(
      [pid, windowActivated, watchSupported, message, tsfn = std::move(tsfn)]() mutable {
        auto* eventData = new ProcessStartEvent{pid, windowActivated, watchSupported, message};
        napi_status status = tsfn.BlockingCall(
            eventData,
            [](Napi::Env env, Napi::Function jsCallback, ProcessStartEvent* data) {
              Napi::Object payload = Napi::Object::New(env);
              payload.Set("event", Napi::String::New(env, "started"));
              payload.Set("started", Napi::Boolean::New(env, true));
              payload.Set("pid", Napi::Number::New(env, static_cast<double>(data->pid)));
              payload.Set("windowActivated", Napi::Boolean::New(env, data->windowActivated));
              payload.Set("watchSupported", Napi::Boolean::New(env, data->watchSupported));
              payload.Set("exitCode", Napi::Number::New(env, static_cast<double>(static_cast<DWORD>(-1))));
              payload.Set("message", Napi::String::New(env, data->message));
              jsCallback.Call({payload});
              delete data;
            });

        if (status != napi_ok) {
          delete eventData;
        }

        tsfn.Release();
      });
  startThread.detach();
}

void StartWindowActivationWorker(DWORD pid) {
  std::thread activationThread([pid]() {
    HWND mainWindow = vertex::shared::WaitForMainWindow(pid, 5000);
    vertex::shared::BringWindowToTopmostFront(mainWindow);
  });
  activationThread.detach();
}

void StartProcessWatcher(
    Napi::Env env, const Napi::Function& callback, DWORD pid, HANDLE processHandle) {
  Napi::ThreadSafeFunction tsfn = Napi::ThreadSafeFunction::New(
      env, callback, "process-close-watcher", 0, 1);

  std::thread watcherThread([pid, processHandle, tsfn = std::move(tsfn)]() mutable {
    DWORD waitResult = WaitForSingleObject(processHandle, INFINITE);
    DWORD exitCode = static_cast<DWORD>(-1);

    if (waitResult == WAIT_OBJECT_0) {
      DWORD tempExitCode = 0;
      if (GetExitCodeProcess(processHandle, &tempExitCode)) {
        exitCode = tempExitCode;
      }
    }

    CloseHandle(processHandle);

    auto* eventData = new vertex::shared::ProcessCloseEvent{pid, exitCode};
    napi_status status = tsfn.BlockingCall(
        eventData,
        [](Napi::Env env, Napi::Function jsCallback, vertex::shared::ProcessCloseEvent* data) {
          Napi::Object payload = Napi::Object::New(env);
          payload.Set("event", Napi::String::New(env, "closed"));
          payload.Set("started", Napi::Boolean::New(env, true));
          payload.Set("pid", Napi::Number::New(env, static_cast<double>(data->pid)));
          payload.Set("windowActivated", Napi::Boolean::New(env, false));
          payload.Set("watchSupported", Napi::Boolean::New(env, true));
          payload.Set("exitCode", Napi::Number::New(env, static_cast<double>(data->exitCode)));
          payload.Set("message", Napi::String::New(env, "Process has exited"));
          jsCallback.Call({payload});
          delete data;
        });

    if (status != napi_ok) {
      delete eventData;
    }

    tsfn.Release();
  });
  watcherThread.detach();
}

Napi::Object BuildStartFailedResult(
    Napi::Env env,
    const Napi::Function& callback,
    const std::string& errorMessage) {
  vertex::shared::EmitProcessEvent(
      env,
      callback,
      "start_failed",
      false,
      0,
      false,
      false,
      static_cast<DWORD>(-1),
      errorMessage);

  Napi::Object result = Napi::Object::New(env);
  result.Set("ok", Napi::Boolean::New(env, false));
  result.Set("pid", env.Null());
  result.Set("watchSupported", Napi::Boolean::New(env, false));
  result.Set("error", Napi::String::New(env, errorMessage));
  return result;
}

Napi::Object LaunchViaCreateProcess(
    Napi::Env env, const LaunchRequest& request, const Napi::Function& callback) {
  std::wstring commandLine = vertex::shared::QuoteCommandArg(request.target);
  for (const auto& arg : request.args) {
    commandLine += L" ";
    commandLine += vertex::shared::QuoteCommandArg(arg);
  }

  std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
  commandLineBuffer.push_back(L'\0');

  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};

  BOOL created = CreateProcessW(
      request.target.c_str(),
      commandLineBuffer.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_UNICODE_ENVIRONMENT,
      nullptr,
      nullptr,
      &startupInfo,
      &processInfo);

  if (!created) {
    DWORD errorCode = GetLastError();
    std::string message = "CreateProcessW failed: " + vertex::shared::GetSystemErrorMessage(errorCode);
    return BuildStartFailedResult(env, callback, message);
  }

  DWORD pid = processInfo.dwProcessId;
  DispatchStartedEventAsync(
      env, callback, pid, false, true, "Process started");
  StartWindowActivationWorker(pid);

  CloseHandle(processInfo.hThread);
  StartProcessWatcher(env, callback, pid, processInfo.hProcess);

  Napi::Object result = Napi::Object::New(env);
  result.Set("ok", Napi::Boolean::New(env, true));
  result.Set("pid", Napi::Number::New(env, static_cast<double>(pid)));
  result.Set("watchSupported", Napi::Boolean::New(env, true));
  return result;
}

Napi::Object LaunchViaShellExecute(
    Napi::Env env, const LaunchRequest& request, const Napi::Function& callback) {
  std::wstring parameters = BuildShellParameterLine(request.args);

  SHELLEXECUTEINFOW shellExecuteInfo{};
  shellExecuteInfo.cbSize = sizeof(shellExecuteInfo);
  shellExecuteInfo.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
  shellExecuteInfo.lpVerb = L"open";
  shellExecuteInfo.lpFile = request.target.c_str();
  shellExecuteInfo.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
  shellExecuteInfo.nShow = SW_SHOWNORMAL;

  BOOL executed = ShellExecuteExW(&shellExecuteInfo);
  if (!executed) {
    DWORD errorCode = GetLastError();
    std::string message = "ShellExecuteExW failed: " + vertex::shared::GetSystemErrorMessage(errorCode);
    return BuildStartFailedResult(env, callback, message);
  }

  HANDLE processHandle = shellExecuteInfo.hProcess;
  DWORD pid = 0;
  bool watchSupported = processHandle != nullptr && processHandle != INVALID_HANDLE_VALUE;
  if (watchSupported) {
    pid = GetProcessId(processHandle);
    if (pid == 0) {
      watchSupported = false;
      CloseHandle(processHandle);
      processHandle = nullptr;
    }
  }

  bool windowActivated = false;
  std::string startMessage;
  if (watchSupported) {
    startMessage = "Target started by shell";
    DispatchStartedEventAsync(
        env, callback, pid, false, true, startMessage);
    StartWindowActivationWorker(pid);
  } else {
    startMessage = "Target started by shell, but process handle is unavailable";
    DispatchStartedEventAsync(
        env, callback, pid, false, false, startMessage);
  }

  if (watchSupported) {
    StartProcessWatcher(env, callback, pid, processHandle);
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("ok", Napi::Boolean::New(env, true));
  if (watchSupported) {
    result.Set("pid", Napi::Number::New(env, static_cast<double>(pid)));
  } else {
    result.Set("pid", env.Null());
  }
  result.Set("watchSupported", Napi::Boolean::New(env, watchSupported));
  return result;
}

Napi::Value LaunchSpecifiedTarget(
    const Napi::CallbackInfo& info, const std::string& methodName) {
  Napi::Env env = info.Env();
  LaunchRequest request = ParseLaunchRequest(info, methodName);
  if (env.IsExceptionPending()) {
    return env.Null();
  }

  if (request.target.empty()) {
    return BuildStartFailedResult(env, request.callback, "Target path is empty");
  }

  if (IsProtocolTarget(request.target)) {
    return LaunchViaShellExecute(env, request, request.callback);
  }

  return LaunchViaCreateProcess(env, request, request.callback);
}

Napi::Value LaunchExe(const Napi::CallbackInfo& info) {
  return LaunchSpecifiedTarget(info, "launchExe");
}

Napi::Value LaunchTarget(const Napi::CallbackInfo& info) {
  return LaunchSpecifiedTarget(info, "launchTarget");
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("launchExe", Napi::Function::New(env, LaunchExe));
  exports.Set("launchTarget", Napi::Function::New(env, LaunchTarget));
  return exports;
}

}  // namespace

NODE_API_MODULE(process_api, Init)
