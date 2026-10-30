#include <napi.h>
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#include <string>
#include <vector>
#include <memory>
#include <iostream>

#pragma comment(lib, "wbemuuid.lib")

namespace {

// WMI Scoped Object Helpers
struct VariantCloser {
  void operator()(VARIANT* v) const { VariantClear(v); }
};
using ScopedVariant = std::unique_ptr<VARIANT, VariantCloser>;

struct COMInitializer {
  HRESULT hr;
  COMInitializer() {
    hr = CoInitializeEx(0, COINIT_MULTITHREADED);
  }
  ~COMInitializer() {
    if (SUCCEEDED(hr)) {
      CoUninitialize();
    }
  }
  bool IsInitialized() const {
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
  }
};

std::wstring GetSystemRegistryString(HKEY hKey, const std::wstring& subKey, const std::wstring& valueName) {
  DWORD dataSize = 0;
  LSTATUS status = RegGetValueW(
      hKey, subKey.c_str(), valueName.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &dataSize);
  if (status != ERROR_SUCCESS || dataSize == 0) {
    return L"";
  }

  std::vector<wchar_t> buffer(dataSize / sizeof(wchar_t));
  status = RegGetValueW(
      hKey, subKey.c_str(), valueName.c_str(), RRF_RT_REG_SZ, nullptr, buffer.data(), &dataSize);
  if (status != ERROR_SUCCESS) {
    return L"";
  }

  size_t len = buffer.size();
  while (len > 0 && buffer[len - 1] == L'\0') {
    len--;
  }
  return std::wstring(buffer.data(), len);
}

// ============================================
// WMI Helper Function
// ============================================
std::vector<std::vector<std::pair<std::wstring, std::wstring>>> RunWMIQuery(
    const std::wstring& wqlQuery, const std::vector<std::wstring>& properties) {

  std::vector<std::vector<std::pair<std::wstring, std::wstring>>> results;

  COMInitializer com;
  if (!com.IsInitialized()) {
    return results; // CoInitializeEx failed
  }

  HRESULT hres;

  // Set general COM security levels
  hres = CoInitializeSecurity(
      NULL,
      -1,
      NULL,
      NULL,
      RPC_C_AUTHN_LEVEL_DEFAULT,
      RPC_C_IMP_LEVEL_IMPERSONATE,
      NULL,
      EOAC_NONE,
      NULL);
  
  // It's acceptable if security is already initialized
  if (FAILED(hres) && hres != RPC_E_TOO_LATE) {
    return results;
  }

  // Obtain the initial locator to WMI
  IWbemLocator* pLoc = NULL;
  hres = CoCreateInstance(
      CLSID_WbemLocator,
      0,
      CLSCTX_INPROC_SERVER,
      IID_IWbemLocator, (LPVOID*)&pLoc);

  if (FAILED(hres)) {
    return results;
  }

  // Connect to WMI through the IWbemLocator::ConnectServer method
  IWbemServices* pSvc = NULL;
  hres = pLoc->ConnectServer(
      _bstr_t(L"ROOT\\CIMV2"), // Object path of WMI namespace
      NULL,                    // User name. NULL = current user
      NULL,                    // User password. NULL = current
      0,                       // Locale. NULL indicates current
      NULL,                    // Security flags.
      0,                       // Authority (for example, Kerberos)
      0,                       // Context object
      &pSvc                    // pointer to IWbemServices proxy
  );

  if (FAILED(hres)) {
    pLoc->Release();
    return results;
  }

  // Set security levels on the proxy
  hres = CoSetProxyBlanket(
      pSvc,                        // Indicates the proxy to set
      RPC_C_AUTHN_WINNT,           // RPC_C_AUTHN_xxx
      RPC_C_AUTHZ_NONE,            // RPC_C_AUTHZ_xxx
      NULL,                        // Server principal name
      RPC_C_AUTHN_LEVEL_CALL,      // RPC_C_AUTHN_LEVEL_xxx
      RPC_C_IMP_LEVEL_IMPERSONATE, // RPC_C_IMP_LEVEL_xxx
      NULL,                        // client identity
      EOAC_NONE                    // proxy capabilities
  );

  if (FAILED(hres)) {
    pSvc->Release();
    pLoc->Release();
    return results;
  }

  // Use the IWbemServices pointer to make requests of WMI
  IEnumWbemClassObject* pEnumerator = NULL;
  hres = pSvc->ExecQuery(
      bstr_t("WQL"),
      bstr_t(wqlQuery.c_str()),
      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
      NULL,
      &pEnumerator);

  if (FAILED(hres)) {
    pSvc->Release();
    pLoc->Release();
    return results;
  }

  // Get the data from the query
  IWbemClassObject* pclsObj = NULL;
  ULONG uReturn = 0;

  while (pEnumerator) {
    HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);

    if (0 == uReturn) {
      break;
    }

    std::vector<std::pair<std::wstring, std::wstring>> currentObjResults;
    for (const auto& prop : properties) {
      VARIANT vtProp;
      VariantInit(&vtProp);
      hr = pclsObj->Get(prop.c_str(), 0, &vtProp, 0, 0);
      
      std::wstring valStr = L"";
      if (SUCCEEDED(hr)) {
        if (vtProp.vt == VT_BSTR && vtProp.bstrVal != nullptr) {
          valStr = vtProp.bstrVal;
        } else if (vtProp.vt == VT_I4) {
          valStr = std::to_wstring(vtProp.lVal);
        } else if (vtProp.vt == VT_UI4) {
          valStr = std::to_wstring(vtProp.ulVal);
        } else if (vtProp.vt == VT_I8) {
          valStr = std::to_wstring(vtProp.llVal);
        } else if (vtProp.vt == VT_UI8) {
          valStr = std::to_wstring(vtProp.ullVal);
        } else if (vtProp.vt == VT_R4) {
          valStr = std::to_wstring(vtProp.fltVal);
        } else if (vtProp.vt == VT_R8) {
          valStr = std::to_wstring(vtProp.dblVal);
        } else if (vtProp.vt == VT_BOOL) {
          valStr = vtProp.boolVal ? L"1" : L"0";
        }
      }
      currentObjResults.push_back({prop, valStr});
      VariantClear(&vtProp);
    }

    results.push_back(currentObjResults);
    pclsObj->Release();
  }

  pEnumerator->Release();
  pSvc->Release();
  pLoc->Release();

  return results;
}

std::wstring GetWMIValueOrDefault(const std::vector<std::pair<std::wstring, std::wstring>>& obj, const std::wstring& key) {
  for (const auto& pair : obj) {
    if (pair.first == key) return pair.second;
  }
  return L"";
}

// ============================================
// NAPI Exports
// ============================================

Napi::Value GetSystemDetails(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object result = Napi::Object::New(env);

  // 1. OS Info
  Napi::Object osDetails = Napi::Object::New(env);
  auto osQueryResult = RunWMIQuery(L"SELECT Caption, Version, OSArchitecture FROM Win32_OperatingSystem", {L"Caption", L"Version", L"OSArchitecture"});
  if (!osQueryResult.empty()) {
    osDetails.Set("caption", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(osQueryResult[0], L"Caption").c_str())));
    osDetails.Set("version", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(osQueryResult[0], L"Version").c_str())));
    osDetails.Set("architecture", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(osQueryResult[0], L"OSArchitecture").c_str())));
  }
  result.Set("os", osDetails);

  // 2. CPU Info
  Napi::Object cpuDetails = Napi::Object::New(env);
  auto cpuQueryResult = RunWMIQuery(
      L"SELECT Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed, SocketDesignation, VirtualizationFirmwareEnabled FROM Win32_Processor", 
      {L"Name", L"NumberOfCores", L"NumberOfLogicalProcessors", L"MaxClockSpeed", L"SocketDesignation", L"VirtualizationFirmwareEnabled"}
  );
  if (!cpuQueryResult.empty()) {
    cpuDetails.Set("name", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(cpuQueryResult[0], L"Name").c_str())));
    cpuDetails.Set("cores", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(cpuQueryResult[0], L"NumberOfCores").c_str())));
    cpuDetails.Set("threads", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(cpuQueryResult[0], L"NumberOfLogicalProcessors").c_str())));
    cpuDetails.Set("maxClockSpeed", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(cpuQueryResult[0], L"MaxClockSpeed").c_str())));
    cpuDetails.Set("socket", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(cpuQueryResult[0], L"SocketDesignation").c_str())));
    cpuDetails.Set("virtualization", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(cpuQueryResult[0], L"VirtualizationFirmwareEnabled").c_str())));
  }
  result.Set("cpu", cpuDetails);

  // 3. GPU Info
  Napi::Array gpusList = Napi::Array::New(env);
  auto gpuQueryResult = RunWMIQuery(L"SELECT Name, AdapterRAM FROM Win32_VideoController", {L"Name", L"AdapterRAM"});
  for (size_t i = 0; i < gpuQueryResult.size(); ++i) {
    Napi::Object gpuObj = Napi::Object::New(env);
    gpuObj.Set("name", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(gpuQueryResult[i], L"Name").c_str())));
    gpuObj.Set("ramBytes", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(gpuQueryResult[i], L"AdapterRAM").c_str())));
    gpusList.Set(i, gpuObj);
  }
  result.Set("gpus", gpusList);

  // 4. Board Info
  Napi::Object boardDetails = Napi::Object::New(env);
  auto boardQueryResult = RunWMIQuery(L"SELECT Manufacturer, Product FROM Win32_BaseBoard", {L"Manufacturer", L"Product"});
  if (!boardQueryResult.empty()) {
    boardDetails.Set("manufacturer", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(boardQueryResult[0], L"Manufacturer").c_str())));
    boardDetails.Set("product", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(boardQueryResult[0], L"Product").c_str())));
  }
  result.Set("board", boardDetails);

  // 5. Memory Info
  Napi::Array memoriesList = Napi::Array::New(env);
  auto memQueryResult = RunWMIQuery(
      L"SELECT Manufacturer, Capacity, Speed, SMBIOSMemoryType, DeviceLocator FROM Win32_PhysicalMemory", 
      {L"Manufacturer", L"Capacity", L"Speed", L"SMBIOSMemoryType", L"DeviceLocator"}
  );
  for (size_t i = 0; i < memQueryResult.size(); ++i) {
    Napi::Object memObj = Napi::Object::New(env);
    memObj.Set("manufacturer", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(memQueryResult[i], L"Manufacturer").c_str())));
    memObj.Set("capacityBytes", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(memQueryResult[i], L"Capacity").c_str())));
    memObj.Set("speed", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(memQueryResult[i], L"Speed").c_str())));
    memObj.Set("smbiosType", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(memQueryResult[i], L"SMBIOSMemoryType").c_str())));
    memObj.Set("locator", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(memQueryResult[i], L"DeviceLocator").c_str())));
    memoriesList.Set(i, memObj);
  }
  result.Set("memories", memoriesList);

  // 6. Disk Info
  Napi::Array disksList = Napi::Array::New(env);
  auto diskQueryResult = RunWMIQuery(L"SELECT Model, Size, MediaType FROM Win32_DiskDrive", {L"Model", L"Size", L"MediaType"});
  for (size_t i = 0; i < diskQueryResult.size(); ++i) {
    Napi::Object diskObj = Napi::Object::New(env);
    diskObj.Set("model", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(diskQueryResult[i], L"Model").c_str())));
    diskObj.Set("sizeBytes", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(diskQueryResult[i], L"Size").c_str())));
    diskObj.Set("mediaType", Napi::String::New(env, reinterpret_cast<const char16_t*>(GetWMIValueOrDefault(diskQueryResult[i], L"MediaType").c_str())));
    disksList.Set(i, diskObj);
  }
  result.Set("disks", disksList);


  return result;
}

// Keep the old one for backwards compatibility
Napi::Value GetHardwareModel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  std::wstring cpuModel = GetSystemRegistryString(
      HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
  std::wstring systemModel = GetSystemRegistryString(
      HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName");

  MEMORYSTATUSEX statex;
  statex.dwLength = sizeof(statex);
  GlobalMemoryStatusEx(&statex);
  double memoryGB = static_cast<double>(statex.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
  char memStr[64];
  snprintf(memStr, sizeof(memStr), "%.2f GB", memoryGB);

  Napi::Object result = Napi::Object::New(env);
  result.Set("cpu", Napi::String::New(env, reinterpret_cast<const char16_t*>(cpuModel.c_str())));
  result.Set("systemModel", Napi::String::New(env, reinterpret_cast<const char16_t*>(systemModel.c_str())));
  result.Set("memory", Napi::String::New(env, memStr));

  return result;
}


Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("getHardwareModel", Napi::Function::New(env, GetHardwareModel));
  exports.Set("getSystemDetails", Napi::Function::New(env, GetSystemDetails));
  return exports;
}

}  // namespace

NODE_API_MODULE(system_api, Init)
