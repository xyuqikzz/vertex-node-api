#include <napi.h>
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../shared/process_utils.h"

namespace {

struct FileOptions {
  bool overwrite;
  std::wstring password;
};

struct CommandResult {
  DWORD exitCode;
  std::string stdoutText;
  std::string stderrText;
};

struct ArchiveEntry {
  std::string path;
  std::string name;
  bool isDirectory;
  int64_t size;
  int64_t packedSize;
  std::string modifiedAt;
  std::string createdAt;
  std::string accessedAt;
  bool hasEncrypted;
  bool encrypted;
  std::string attributes;
  std::string method;
  std::string comment;
  std::string crc;
  std::string hostOs;
  std::string version;
  int64_t volumeIndex;
  int64_t offset;
};

struct ArchiveSummary {
  int64_t entryCount;
  int64_t fileCount;
  int64_t directoryCount;
  int64_t totalSize;
  int64_t totalPackedSize;
};

struct ParsedArchive {
  std::string archiveType;
  int64_t physicalSize;
  std::vector<ArchiveEntry> entries;
  ArchiveSummary summary;
};

struct ArchiveTreeNode {
  std::string name;
  std::string path;
  bool isDirectory;
  bool implicit;
  int64_t size;
  int64_t packedSize;
  std::string modifiedAt;
  std::string createdAt;
  std::string accessedAt;
  bool hasEncrypted;
  bool encrypted;
  std::string attributes;
  std::string method;
  std::string comment;
  std::string crc;
  std::string hostOs;
  std::string version;
  int64_t volumeIndex;
  int64_t offset;
  std::vector<std::unique_ptr<ArchiveTreeNode> > children;
};

struct StagingPhaseMetrics {
  double ensureTargetRootMs;
  double linkLoopMs;
  double unlinkLoopMs;
  double cleanupEmptyDirsMs;
};

struct StagingOperationMetrics {
  double lstatMs;
  double readdirMs;
  double ensureDirMs;
  double unlinkMs;
  double linkMs;
  double copyFileMs;
};

struct StagingMetrics {
  double totalMs;
  StagingPhaseMetrics phases;
  StagingOperationMetrics operations;
};

struct DeploySummary {
  int32_t directories;
  int32_t linkedFiles;
  int32_t copiedFiles;
  int32_t overwrittenFiles;
};

struct UndeploySummary {
  int32_t directories;
  int32_t removedFiles;
};

struct RemoveEmptyDirsSummary {
  int32_t directories;
  int32_t removedDirectories;
};

struct ModUninstallPrepareResult {
  std::vector<std::wstring> filePaths;
  std::vector<std::wstring> dirPaths;
  std::vector<std::wstring> occupiedFiles;
};

struct ModUninstallExecuteResult {
  std::vector<std::wstring> removedFiles;
  std::vector<std::wstring> missingFiles;
  std::vector<std::wstring> removedDirectories;
};

struct DriveScanSummary {
  bool found;
  std::wstring path;
  int32_t scannedDrives;
  int32_t scannedDirectories;
};

struct DetectMarkers {
  std::vector<std::string> anyFiles;
  std::vector<std::string> allFiles;
};

struct DetectRootFromRule {
  std::string type;
  int32_t up;
};

struct DetectCandidate {
  int32_t index;
  std::string name;
  std::string type;
  int32_t confidence;
  DetectMarkers markers;
  std::vector<std::string> exeGlobs;
  DetectRootFromRule rootFrom;
};

struct DetectLocalGameEntry {
  int32_t appId;
  int32_t gameId;
  std::wstring clientExePath;
};

struct DetectGameRootInput {
  std::vector<DetectCandidate> candidates;
  std::vector<DetectLocalGameEntry> localGameList;
  std::string strategy;
  std::string domainName;
  int32_t nexusGameId;
};

struct DetectMatchRecord {
  bool matched;
  std::wstring rootPath;
  std::wstring exePath;
  int32_t appId;
  int32_t gameId;
  std::string source;
  std::string candidateName;
  std::string candidateType;
  int32_t confidence;
};

struct DetectGameRootOutput {
  bool success;
  std::string reason;
  std::string strategy;
  std::string domainName;
  int32_t nexusGameId;
  DetectMatchRecord record;
};

struct PackageRootCandidate {
  std::wstring rootPath;
  std::string packageRoot;
  int32_t depth;
};

struct ExecutableScanResult {
  int32_t type;
  std::wstring path;
  std::vector<std::wstring> paths;
};

class AccumulateTimer {
 public:
  explicit AccumulateTimer(double* target)
      : target_(target), startTime_(std::chrono::steady_clock::now()) {}

  ~AccumulateTimer() {
    if (target_ == nullptr) {
      return;
    }

    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime_);
    *target_ += elapsed.count();
  }

 private:
  double* target_;
  std::chrono::steady_clock::time_point startTime_;
};

StagingMetrics CreateEmptyStagingMetrics() {
  return StagingMetrics{
      0.0,
      {0.0, 0.0, 0.0, 0.0},
      {0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
  };
}

DeploySummary CreateEmptyDeploySummary() {
  return DeploySummary{0, 0, 0, 0};
}

UndeploySummary CreateEmptyUndeploySummary() {
  return UndeploySummary{0, 0};
}

RemoveEmptyDirsSummary CreateEmptyRemoveEmptyDirsSummary() {
  return RemoveEmptyDirsSummary{0, 0};
}

ModUninstallPrepareResult CreateEmptyModUninstallPrepareResult() {
  ModUninstallPrepareResult result;
  result.filePaths.clear();
  result.dirPaths.clear();
  result.occupiedFiles.clear();
  return result;
}

ModUninstallExecuteResult CreateEmptyModUninstallExecuteResult() {
  ModUninstallExecuteResult result;
  result.removedFiles.clear();
  result.missingFiles.clear();
  result.removedDirectories.clear();
  return result;
}

DriveScanSummary CreateEmptyDriveScanSummary() {
  return DriveScanSummary{false, L"", 0, 0};
}

DetectMatchRecord CreateEmptyDetectMatchRecord() {
  return DetectMatchRecord{false, L"", L"", 0, 0, "", "", "", 0};
}

DetectGameRootOutput CreateEmptyDetectGameRootOutput() {
  DetectGameRootOutput output;
  output.success = false;
  output.reason = "";
  output.strategy = "highest_confidence";
  output.domainName = "";
  output.nexusGameId = 0;
  output.record = CreateEmptyDetectMatchRecord();
  return output;
}

ExecutableScanResult CreateEmptyExecutableScanResult() {
  ExecutableScanResult result;
  result.type = 0;
  result.path = L"";
  result.paths.clear();
  return result;
}

std::string TrimAsciiWhitespace(const std::string& input) {
  size_t startIndex = 0;
  while (startIndex < input.size() &&
         std::isspace(static_cast<unsigned char>(input[startIndex])) != 0) {
    startIndex += 1;
  }

  size_t endIndex = input.size();
  while (endIndex > startIndex &&
         std::isspace(static_cast<unsigned char>(input[endIndex - 1])) != 0) {
    endIndex -= 1;
  }

  return input.substr(startIndex, endIndex - startIndex);
}

std::vector<std::string> SplitString(const std::string& input, char delimiter) {
  std::vector<std::string> parts;
  size_t startIndex = 0;

  while (startIndex <= input.size()) {
    size_t delimiterIndex = input.find(delimiter, startIndex);
    if (delimiterIndex == std::string::npos) {
      parts.push_back(input.substr(startIndex));
      break;
    }

    parts.push_back(input.substr(startIndex, delimiterIndex - startIndex));
    startIndex = delimiterIndex + 1;
  }

  return parts;
}

std::string ReplaceBackslashes(const std::string& input) {
  std::string output = input;
  std::replace(output.begin(), output.end(), '\\', '/');
  return output;
}

bool ParseKeyValueLine(const std::string& line, std::string* key, std::string* value) {
  const size_t delimiterIndex = line.find(" = ");
  if (delimiterIndex == std::string::npos) {
    return false;
  }

  *key = TrimAsciiWhitespace(line.substr(0, delimiterIndex));
  *value = TrimAsciiWhitespace(line.substr(delimiterIndex + 3));
  return true;
}

int64_t ParseInt64(const std::string& value) {
  const std::string trimmedValue = TrimAsciiWhitespace(value);
  if (trimmedValue.empty()) {
    return -1;
  }

  try {
    return std::stoll(trimmedValue);
  } catch (...) {
    return -1;
  }
}

bool ParseBooleanFlag(const std::string& value, bool* output) {
  if (value == "+") {
    *output = true;
    return true;
  }

  if (value == "-") {
    *output = false;
    return true;
  }

  return false;
}

std::wstring EnsureAbsolutePath(const std::wstring& input) {
  DWORD requiredLength = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
  if (requiredLength == 0) {
    return input;
  }

  std::vector<wchar_t> buffer(requiredLength, L'\0');
  DWORD actualLength =
      GetFullPathNameW(input.c_str(), requiredLength, &buffer[0], nullptr);
  if (actualLength == 0) {
    return input;
  }

  return std::wstring(&buffer[0], actualLength);
}

bool IsExistingFile(const std::wstring& filePath) {
  DWORD attributes = GetFileAttributesW(filePath.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring GetDirectoryName(const std::wstring& filePath) {
  size_t delimiterIndex = filePath.find_last_of(L"\\/");
  if (delimiterIndex == std::wstring::npos) {
    return L"";
  }

  return filePath.substr(0, delimiterIndex);
}

std::wstring JoinWindowsPath(const std::wstring& leftPath, const std::wstring& rightPath) {
  if (leftPath.empty()) {
    return rightPath;
  }

  if (leftPath[leftPath.size() - 1] == L'\\' || leftPath[leftPath.size() - 1] == L'/') {
    return leftPath + rightPath;
  }

  return leftPath + L"\\" + rightPath;
}

std::wstring GetCurrentModuleDirectory() {
  HMODULE moduleHandle = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&GetCurrentModuleDirectory),
          &moduleHandle)) {
    return L"";
  }

  std::vector<wchar_t> buffer(1024, L'\0');
  while (true) {
    DWORD actualLength = GetModuleFileNameW(
        moduleHandle, &buffer[0], static_cast<DWORD>(buffer.size()));
    if (actualLength == 0) {
      return L"";
    }

    if (actualLength < buffer.size() - 1) {
      return GetDirectoryName(std::wstring(&buffer[0], actualLength));
    }

    buffer.resize(buffer.size() * 2, L'\0');
  }
}

std::wstring Resolve7ZipExecutablePath() {
  return JoinWindowsPath(GetCurrentModuleDirectory(), L"7z.exe");
}

std::string ToLowerAscii(const std::string& input) {
  std::string output = input;
  std::transform(output.begin(), output.end(), output.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return output;
}

std::wstring ToLowerWide(const std::wstring& input) {
  std::wstring output = input;
  std::transform(output.begin(), output.end(), output.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return output;
}

int32_t ToSafeInt32(double value, int32_t defaultValue) {
  const double minValue = static_cast<double>((std::numeric_limits<int32_t>::min)());
  const double maxValue = static_cast<double>((std::numeric_limits<int32_t>::max)());
  if (value < minValue || value > maxValue) {
    return defaultValue;
  }
  return static_cast<int32_t>(value);
}

std::string NormalizeFilename(const std::string& input) {
  std::string output = TrimAsciiWhitespace(input);
  if (output.size() >= 2) {
    const char firstChar = output.front();
    const char lastChar = output.back();
    const bool wrappedByDoubleQuotes = firstChar == '"' && lastChar == '"';
    const bool wrappedBySingleQuotes = firstChar == '\'' && lastChar == '\'';
    if (wrappedByDoubleQuotes || wrappedBySingleQuotes) {
      output = output.substr(1, output.size() - 2);
    }
  }
  return output;
}

std::wstring NormalizeRelativeFilePathWide(const std::string& input) {
  std::wstring output = vertex::shared::Utf8ToWide(NormalizeFilename(input));
  std::replace(output.begin(), output.end(), L'/', L'\\');
  std::replace(output.begin(), output.end(), L'\"', L' ');
  return output;
}

std::vector<std::string> DedupeTextList(const std::vector<std::string>& input) {
  std::vector<std::string> output;
  std::set<std::string> seen;
  for (size_t index = 0; index < input.size(); ++index) {
    const std::string text = TrimAsciiWhitespace(input[index]);
    if (text.empty()) {
      continue;
    }
    if (seen.insert(text).second) {
      output.push_back(text);
    }
  }
  return output;
}

std::string ExtractFileNameFromGlob(const std::string& globText) {
  const std::string normalized = ReplaceBackslashes(TrimAsciiWhitespace(globText));
  if (normalized.empty()) {
    return "";
  }

  const size_t delimiterIndex = normalized.find_last_of('/');
  const std::string fileName =
      delimiterIndex == std::string::npos ? normalized : normalized.substr(delimiterIndex + 1);
  if (fileName.empty() ||
      fileName.find('*') != std::string::npos ||
      fileName.find('?') != std::string::npos) {
    return "";
  }

  return NormalizeFilename(fileName);
}

std::wstring GetBaseName(const std::wstring& filePath) {
  const size_t delimiterIndex = filePath.find_last_of(L"\\/");
  if (delimiterIndex == std::wstring::npos) {
    return filePath;
  }

  return filePath.substr(delimiterIndex + 1);
}

std::string NormalizeRelativeRulePathAscii(const std::string& input) {
  std::string output = ReplaceBackslashes(TrimAsciiWhitespace(input));

  while (output.find("./") == 0) {
    output.erase(0, 2);
  }

  while (!output.empty() && output[0] == '/') {
    output.erase(0, 1);
  }
  while (!output.empty() && output[output.size() - 1] == '/') {
    output.erase(output.size() - 1, 1);
  }

  return output;
}

bool IsPreparedRuleDirectoryName(const std::wstring& entryName) {
  return ToLowerWide(entryName) == L"__vertex_nexus_rule__";
}

bool IsDirectoryEmpty(const std::wstring& directoryPath, bool* empty, std::string* errorMessage);
bool DeleteMappedTargetPath(const std::wstring& targetPath, DWORD targetAttributes, DWORD* errorCode);

int32_t CountPathDepth(const std::wstring& input) {
  int32_t depth = 0;
  bool inSegment = false;

  for (size_t index = 0; index < input.size(); ++index) {
    const wchar_t ch = input[index];
    if (ch == L'\\' || ch == L'/') {
      if (inSegment) {
        depth += 1;
        inSegment = false;
      }
      continue;
    }

    inSegment = true;
  }

  if (inSegment) {
    depth += 1;
  }

  return depth;
}

void SortPathsDeepFirst(std::vector<std::wstring>* pathList) {
  if (pathList == nullptr) {
    return;
  }

  std::sort(pathList->begin(), pathList->end(), [](const std::wstring& left, const std::wstring& right) {
    const int32_t leftDepth = CountPathDepth(left);
    const int32_t rightDepth = CountPathDepth(right);
    if (leftDepth != rightDepth) {
      return leftDepth > rightDepth;
    }
    return left < right;
  });
}

int32_t ParseNodeType(const Napi::Value& value, int32_t defaultValue) {
  if (value.IsNumber()) {
    return ToSafeInt32(value.As<Napi::Number>().DoubleValue(), defaultValue);
  }

  if (value.IsString()) {
    const std::string text = TrimAsciiWhitespace(value.As<Napi::String>().Utf8Value());
    if (text.empty()) {
      return defaultValue;
    }

    try {
      return ToSafeInt32(std::stod(text), defaultValue);
    } catch (...) {
      return defaultValue;
    }
  }

  return defaultValue;
}

void CollectParentDirectoriesForUninstall(
    const std::wstring& filePath,
    std::set<std::wstring>* directorySet) {
  if (directorySet == nullptr) {
    return;
  }

  std::wstring currentDirectory = GetDirectoryName(filePath);
  while (!currentDirectory.empty()) {
    const bool inserted = directorySet->insert(currentDirectory).second;
    const std::wstring nextDirectory = GetDirectoryName(currentDirectory);
    if (!inserted || nextDirectory == currentDirectory) {
      break;
    }
    currentDirectory = nextDirectory;
  }
}

void CollectUninstallTargetsFromValue(
    const Napi::Value& value,
    std::vector<std::wstring>* filePaths,
    std::set<std::wstring>* fileSet,
    std::set<std::wstring>* directorySet) {
  if (value.IsArray()) {
    const Napi::Array arrayValue = value.As<Napi::Array>();
    const uint32_t length = arrayValue.Length();
    for (uint32_t index = 0; index < length; ++index) {
      CollectUninstallTargetsFromValue(arrayValue.Get(index), filePaths, fileSet, directorySet);
    }
    return;
  }

  if (!value.IsObject() || value.IsNull()) {
    return;
  }

  const Napi::Object object = value.As<Napi::Object>();
  if (ParseNodeType(object.Get("type"), -1) == 1) {
    const Napi::Value pathValue = object.Get("path");
    if (!pathValue.IsString()) {
      return;
    }

    const std::wstring filePath =
        EnsureAbsolutePath(vertex::shared::Utf8ToWide(pathValue.As<Napi::String>().Utf8Value()));
    if (filePath.empty()) {
      return;
    }

    if (fileSet != nullptr && fileSet->insert(filePath).second) {
      if (filePaths != nullptr) {
        filePaths->push_back(filePath);
      }
      CollectParentDirectoriesForUninstall(filePath, directorySet);
    }
    return;
  }

  const Napi::Value subValue = object.Get("sub");
  if (subValue.IsArray()) {
    CollectUninstallTargetsFromValue(subValue, filePaths, fileSet, directorySet);
  }
}

bool IsFileOccupiedForUninstall(const std::wstring& filePath, DWORD* errorCode) {
  const DWORD attributes = GetFileAttributesW(filePath.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    if (errorCode != nullptr) {
      *errorCode = GetLastError();
    }
    return false;
  }

  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    if (errorCode != nullptr) {
      *errorCode = ERROR_DIRECTORY;
    }
    return false;
  }

  HANDLE fileHandle = CreateFileW(
      filePath.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (fileHandle != INVALID_HANDLE_VALUE) {
    CloseHandle(fileHandle);
    if (errorCode != nullptr) {
      *errorCode = ERROR_SUCCESS;
    }
    return false;
  }

  const DWORD lastError = GetLastError();
  if (errorCode != nullptr) {
    *errorCode = lastError;
  }

  return lastError == ERROR_SHARING_VIOLATION ||
         lastError == ERROR_LOCK_VIOLATION ||
         lastError == ERROR_ACCESS_DENIED ||
         lastError == ERROR_USER_MAPPED_FILE;
}

std::vector<std::wstring> ParsePathArrayArgument(const Napi::Value& value, const char* fieldName) {
  if (!value.IsArray()) {
    throw Napi::TypeError::New(value.Env(), std::string(fieldName) + " 必须是字符串数组");
  }

  std::vector<std::wstring> output;
  std::set<std::wstring> seen;
  const Napi::Array arrayValue = value.As<Napi::Array>();
  const uint32_t length = arrayValue.Length();

  for (uint32_t index = 0; index < length; ++index) {
    const Napi::Value item = arrayValue.Get(index);
    if (!item.IsString()) {
      throw Napi::TypeError::New(
          value.Env(),
          std::string(fieldName) + " 的第 " + std::to_string(index + 1) + " 项必须是字符串");
    }

    const std::wstring normalizedPath =
        EnsureAbsolutePath(vertex::shared::Utf8ToWide(item.As<Napi::String>().Utf8Value()));
    if (normalizedPath.empty()) {
      continue;
    }

    if (seen.insert(normalizedPath).second) {
      output.push_back(normalizedPath);
    }
  }

  return output;
}

ModUninstallPrepareResult RunPrepareModUninstall(const Napi::Value& value) {
  ModUninstallPrepareResult result = CreateEmptyModUninstallPrepareResult();
  std::set<std::wstring> fileSet;
  std::set<std::wstring> directorySet;

  CollectUninstallTargetsFromValue(value, &result.filePaths, &fileSet, &directorySet);
  result.dirPaths.assign(directorySet.begin(), directorySet.end());
  SortPathsDeepFirst(&result.dirPaths);

  for (size_t index = 0; index < result.filePaths.size(); ++index) {
    DWORD errorCode = ERROR_SUCCESS;
    if (IsFileOccupiedForUninstall(result.filePaths[index], &errorCode)) {
      result.occupiedFiles.push_back(result.filePaths[index]);
    }
  }

  return result;
}

ModUninstallExecuteResult RunExecuteModUninstall(
    const std::vector<std::wstring>& filePaths,
    const std::vector<std::wstring>& dirPaths) {
  ModUninstallExecuteResult result = CreateEmptyModUninstallExecuteResult();

  for (size_t index = 0; index < filePaths.size(); ++index) {
    const std::wstring& filePath = filePaths[index];
    const DWORD attributes = GetFileAttributesW(filePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD errorCode = GetLastError();
      if (errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND) {
        result.missingFiles.push_back(filePath);
        continue;
      }

      throw std::runtime_error(
          "文件检查失败 [" + vertex::shared::WideToUtf8(filePath) + "]: " +
          vertex::shared::GetSystemErrorMessage(errorCode));
    }

    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }

    DWORD occupiedError = ERROR_SUCCESS;
    if (IsFileOccupiedForUninstall(filePath, &occupiedError)) {
      throw std::runtime_error("文件在删除过程中被占用: " + vertex::shared::WideToUtf8(filePath));
    }

    DWORD deleteError = ERROR_SUCCESS;
    if (!DeleteMappedTargetPath(filePath, attributes, &deleteError)) {
      if (deleteError == ERROR_FILE_NOT_FOUND || deleteError == ERROR_PATH_NOT_FOUND) {
        result.missingFiles.push_back(filePath);
        continue;
      }

      throw std::runtime_error(
          "删除失败 [" + vertex::shared::WideToUtf8(filePath) + "]: " +
          vertex::shared::GetSystemErrorMessage(deleteError));
    }

    result.removedFiles.push_back(filePath);
  }

  std::vector<std::wstring> sortedDirectories = dirPaths;
  SortPathsDeepFirst(&sortedDirectories);

  for (size_t index = 0; index < sortedDirectories.size(); ++index) {
    const std::wstring& directoryPath = sortedDirectories[index];
    const DWORD attributes = GetFileAttributesW(directoryPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD errorCode = GetLastError();
      if (errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND) {
        continue;
      }

      throw std::runtime_error(
          "文件夹检查失败 [" + vertex::shared::WideToUtf8(directoryPath) + "]: " +
          vertex::shared::GetSystemErrorMessage(errorCode));
    }

    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      continue;
    }

    bool empty = false;
    std::string directoryError;
    if (!IsDirectoryEmpty(directoryPath, &empty, &directoryError)) {
      throw std::runtime_error(
          "文件夹检查失败 [" + vertex::shared::WideToUtf8(directoryPath) + "]: " +
          directoryError);
    }

    if (!empty) {
      continue;
    }

    if (RemoveDirectoryW(directoryPath.c_str()) == 0) {
      const DWORD errorCode = GetLastError();
      throw std::runtime_error(
          "文件夹检查失败 [" + vertex::shared::WideToUtf8(directoryPath) + "]: " +
          vertex::shared::GetSystemErrorMessage(errorCode));
    }

    result.removedDirectories.push_back(directoryPath);
  }

  return result;
}

Napi::Array BuildWideStringArray(Napi::Env env, const std::vector<std::wstring>& values) {
  Napi::Array arrayValue = Napi::Array::New(env, static_cast<uint32_t>(values.size()));
  for (uint32_t index = 0; index < values.size(); ++index) {
    arrayValue.Set(index, Napi::String::New(env, vertex::shared::WideToUtf8(values[index])));
  }
  return arrayValue;
}

std::string BytesToLowerHex(const BYTE* bytes, DWORD length) {
  static const char kHexChars[] = "0123456789abcdef";
  std::string output;
  output.reserve(static_cast<size_t>(length) * 2);

  for (DWORD index = 0; index < length; ++index) {
    const BYTE value = bytes[index];
    output.push_back(kHexChars[(value >> 4) & 0x0F]);
    output.push_back(kHexChars[value & 0x0F]);
  }

  return output;
}

std::wstring CreateTempFilePath(const wchar_t* prefix) {
  std::vector<wchar_t> tempDirectory(MAX_PATH + 1, L'\0');
  DWORD tempDirectoryLength =
      GetTempPathW(static_cast<DWORD>(tempDirectory.size()), &tempDirectory[0]);
  if (tempDirectoryLength == 0 || tempDirectoryLength >= tempDirectory.size()) {
    return L"";
  }

  std::vector<wchar_t> tempFilePath(MAX_PATH + 1, L'\0');
  if (GetTempFileNameW(&tempDirectory[0], prefix, 0, &tempFilePath[0]) == 0) {
    return L"";
  }

  return std::wstring(&tempFilePath[0]);
}

std::string ReadWholeFileUtf8(const std::wstring& filePath) {
  HANDLE fileHandle = CreateFileW(
      filePath.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (fileHandle == INVALID_HANDLE_VALUE) {
    return "";
  }

  LARGE_INTEGER fileSize;
  fileSize.QuadPart = 0;
  if (!GetFileSizeEx(fileHandle, &fileSize) || fileSize.QuadPart <= 0) {
    CloseHandle(fileHandle);
    return "";
  }

  std::string output(static_cast<size_t>(fileSize.QuadPart), '\0');
  DWORD bytesRead = 0;
  BOOL readOk = ReadFile(
      fileHandle,
      &output[0],
      static_cast<DWORD>(output.size()),
      &bytesRead,
      nullptr);
  CloseHandle(fileHandle);

  if (!readOk) {
    return "";
  }

  output.resize(bytesRead);
  return output;
}

CommandResult RunCommand(const std::wstring& executablePath, const std::vector<std::wstring>& args) {
  const std::wstring stdoutPath = CreateTempFilePath(L"vfo");
  const std::wstring stderrPath = CreateTempFilePath(L"vfe");
  if (stdoutPath.empty() || stderrPath.empty()) {
    throw std::runtime_error("无法创建临时输出文件");
  }

  SECURITY_ATTRIBUTES securityAttributes;
  securityAttributes.nLength = sizeof(securityAttributes);
  securityAttributes.lpSecurityDescriptor = nullptr;
  securityAttributes.bInheritHandle = TRUE;

  HANDLE stdoutHandle = CreateFileW(
      stdoutPath.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      &securityAttributes,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_TEMPORARY,
      nullptr);
  HANDLE stderrHandle = CreateFileW(
      stderrPath.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      &securityAttributes,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_TEMPORARY,
      nullptr);
  HANDLE stdinHandle = CreateFileW(
      L"NUL",
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &securityAttributes,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);

  if (stdoutHandle == INVALID_HANDLE_VALUE ||
      stderrHandle == INVALID_HANDLE_VALUE ||
      stdinHandle == INVALID_HANDLE_VALUE) {
    if (stdoutHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(stdoutHandle);
    }
    if (stderrHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(stderrHandle);
    }
    if (stdinHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(stdinHandle);
    }
    DeleteFileW(stdoutPath.c_str());
    DeleteFileW(stderrPath.c_str());
    throw std::runtime_error("无法创建进程输出句柄");
  }

  std::wstring commandLine = vertex::shared::QuoteCommandArg(executablePath);
  for (size_t index = 0; index < args.size(); ++index) {
    commandLine += L" ";
    commandLine += vertex::shared::QuoteCommandArg(args[index]);
  }

  std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
  commandLineBuffer.push_back(L'\0');

  STARTUPINFOW startupInfo;
  ZeroMemory(&startupInfo, sizeof(startupInfo));
  startupInfo.cb = sizeof(startupInfo);
  startupInfo.dwFlags = STARTF_USESTDHANDLES;
  startupInfo.hStdInput = stdinHandle;
  startupInfo.hStdOutput = stdoutHandle;
  startupInfo.hStdError = stderrHandle;

  PROCESS_INFORMATION processInformation;
  ZeroMemory(&processInformation, sizeof(processInformation));
  const std::wstring workingDirectory = GetDirectoryName(executablePath);
  BOOL created = CreateProcessW(
      executablePath.c_str(),
      &commandLineBuffer[0],
      nullptr,
      nullptr,
      TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
      nullptr,
      workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
      &startupInfo,
      &processInformation);

  CloseHandle(stdinHandle);
  CloseHandle(stdoutHandle);
  CloseHandle(stderrHandle);

  if (!created) {
    DeleteFileW(stdoutPath.c_str());
    DeleteFileW(stderrPath.c_str());
    throw std::runtime_error(
        "CreateProcessW failed: " +
        vertex::shared::GetSystemErrorMessage(GetLastError()));
  }

  WaitForSingleObject(processInformation.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(processInformation.hProcess, &exitCode);
  CloseHandle(processInformation.hThread);
  CloseHandle(processInformation.hProcess);

  CommandResult result;
  result.exitCode = exitCode;
  result.stdoutText = ReadWholeFileUtf8(stdoutPath);
  result.stderrText = ReadWholeFileUtf8(stderrPath);

  DeleteFileW(stdoutPath.c_str());
  DeleteFileW(stderrPath.c_str());
  return result;
}

std::string GenerateFileMD5Internal(const std::wstring& filePath) {
  if (!IsExistingFile(filePath)) {
    throw std::runtime_error("filePath 不存在: " + vertex::shared::WideToUtf8(filePath));
  }

  HANDLE fileHandle = CreateFileW(
      filePath.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (fileHandle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error(
        "打开文件失败: " + vertex::shared::GetSystemErrorMessage(GetLastError()));
  }

  HCRYPTPROV providerHandle = 0;
  HCRYPTHASH hashHandle = 0;
  if (!CryptAcquireContextW(
          &providerHandle,
          nullptr,
          nullptr,
          PROV_RSA_FULL,
          CRYPT_VERIFYCONTEXT)) {
    CloseHandle(fileHandle);
    throw std::runtime_error(
        "初始化 MD5 上下文失败: " +
        vertex::shared::GetSystemErrorMessage(GetLastError()));
  }

  if (!CryptCreateHash(providerHandle, CALG_MD5, 0, 0, &hashHandle)) {
    CryptReleaseContext(providerHandle, 0);
    CloseHandle(fileHandle);
    throw std::runtime_error(
        "创建 MD5 哈希对象失败: " +
        vertex::shared::GetSystemErrorMessage(GetLastError()));
  }

  const DWORD chunkSize = 10 * 1024 * 1024;
  std::vector<BYTE> buffer(chunkSize);
  DWORD bytesRead = 0;

  while (ReadFile(fileHandle, &buffer[0], chunkSize, &bytesRead, nullptr) && bytesRead > 0) {
    if (!CryptHashData(hashHandle, &buffer[0], bytesRead, 0)) {
      CryptDestroyHash(hashHandle);
      CryptReleaseContext(providerHandle, 0);
      CloseHandle(fileHandle);
      throw std::runtime_error(
          "写入 MD5 数据失败: " +
          vertex::shared::GetSystemErrorMessage(GetLastError()));
    }
  }

  const DWORD lastError = GetLastError();
  if (bytesRead > 0 || lastError != ERROR_HANDLE_EOF) {
    if (lastError != ERROR_SUCCESS && lastError != ERROR_HANDLE_EOF) {
      CryptDestroyHash(hashHandle);
      CryptReleaseContext(providerHandle, 0);
      CloseHandle(fileHandle);
      throw std::runtime_error(
          "读取文件失败: " + vertex::shared::GetSystemErrorMessage(lastError));
    }
  }

  BYTE hashValue[16];
  DWORD hashLength = sizeof(hashValue);
  if (!CryptGetHashParam(hashHandle, HP_HASHVAL, hashValue, &hashLength, 0)) {
    CryptDestroyHash(hashHandle);
    CryptReleaseContext(providerHandle, 0);
    CloseHandle(fileHandle);
    throw std::runtime_error(
        "读取 MD5 结果失败: " +
        vertex::shared::GetSystemErrorMessage(GetLastError()));
  }

  CryptDestroyHash(hashHandle);
  CryptReleaseContext(providerHandle, 0);
  CloseHandle(fileHandle);
  return BytesToLowerHex(hashValue, hashLength);
}

std::string NormalizeArchiveEntryPath(const std::string& rawPath) {
  std::string normalizedPath = ReplaceBackslashes(rawPath);
  if (normalizedPath.empty()) {
    throw std::runtime_error("压缩包中存在空路径条目");
  }

  if (normalizedPath.find('\0') != std::string::npos) {
    throw std::runtime_error("压缩包条目路径包含非法空字符");
  }

  if (normalizedPath.size() >= 2 &&
      std::isalpha(static_cast<unsigned char>(normalizedPath[0])) != 0 &&
      normalizedPath[1] == ':') {
    throw std::runtime_error("压缩包条目路径不能包含盘符: " + rawPath);
  }

  if (!normalizedPath.empty() && normalizedPath[0] == '/') {
    throw std::runtime_error("压缩包条目路径不能是绝对路径: " + rawPath);
  }

  std::vector<std::string> pathSegments = SplitString(normalizedPath, '/');
  std::vector<std::string> safeSegments;
  safeSegments.reserve(pathSegments.size());

  for (size_t index = 0; index < pathSegments.size(); ++index) {
    const std::string& segment = pathSegments[index];
    if (segment.empty()) {
      continue;
    }

    if (segment == "." || segment == "..") {
      throw std::runtime_error("压缩包条目路径存在目录穿越风险: " + rawPath);
    }

    safeSegments.push_back(segment);
  }

  if (safeSegments.empty()) {
    throw std::runtime_error("压缩包条目路径无效: " + rawPath);
  }

  std::string output = safeSegments[0];
  for (size_t index = 1; index < safeSegments.size(); ++index) {
    output += "/";
    output += safeSegments[index];
  }
  return output;
}

ArchiveEntry BuildArchiveEntry(const std::map<std::string, std::string>& rawEntry) {
  ArchiveEntry entry;
  entry.path = NormalizeArchiveEntryPath(rawEntry.find("Path")->second);
  size_t delimiterIndex = entry.path.find_last_of('/');
  entry.name = delimiterIndex == std::string::npos
                   ? entry.path
                   : entry.path.substr(delimiterIndex + 1);
  entry.isDirectory =
      rawEntry.find("Folder") != rawEntry.end() && rawEntry.find("Folder")->second == "+";
  entry.size = rawEntry.find("Size") != rawEntry.end()
                   ? ParseInt64(rawEntry.find("Size")->second)
                   : -1;
  entry.packedSize = rawEntry.find("Packed Size") != rawEntry.end()
                         ? ParseInt64(rawEntry.find("Packed Size")->second)
                         : -1;
  entry.modifiedAt = rawEntry.find("Modified") != rawEntry.end()
                         ? rawEntry.find("Modified")->second
                         : "";
  entry.createdAt = rawEntry.find("Created") != rawEntry.end()
                        ? rawEntry.find("Created")->second
                        : "";
  entry.accessedAt = rawEntry.find("Accessed") != rawEntry.end()
                         ? rawEntry.find("Accessed")->second
                         : "";
  entry.hasEncrypted = false;
  entry.encrypted = false;
  if (rawEntry.find("Encrypted") != rawEntry.end()) {
    entry.hasEncrypted =
        ParseBooleanFlag(rawEntry.find("Encrypted")->second, &entry.encrypted);
  }
  entry.attributes = rawEntry.find("Attributes") != rawEntry.end()
                         ? rawEntry.find("Attributes")->second
                         : "";
  entry.method = rawEntry.find("Method") != rawEntry.end()
                     ? rawEntry.find("Method")->second
                     : "";
  entry.comment = rawEntry.find("Comment") != rawEntry.end()
                      ? rawEntry.find("Comment")->second
                      : "";
  entry.crc = rawEntry.find("CRC") != rawEntry.end()
                  ? rawEntry.find("CRC")->second
                  : "";
  entry.hostOs = rawEntry.find("Host OS") != rawEntry.end()
                     ? rawEntry.find("Host OS")->second
                     : "";
  entry.version = rawEntry.find("Version") != rawEntry.end()
                      ? rawEntry.find("Version")->second
                      : "";
  entry.volumeIndex = rawEntry.find("Volume Index") != rawEntry.end()
                          ? ParseInt64(rawEntry.find("Volume Index")->second)
                          : -1;
  entry.offset = rawEntry.find("Offset") != rawEntry.end()
                     ? ParseInt64(rawEntry.find("Offset")->second)
                     : -1;
  return entry;
}

ParsedArchive ParseArchiveListing(const std::string& stdoutText) {
  ParsedArchive parsedArchive;
  parsedArchive.physicalSize = -1;
  parsedArchive.summary.entryCount = 0;
  parsedArchive.summary.fileCount = 0;
  parsedArchive.summary.directoryCount = 0;
  parsedArchive.summary.totalSize = 0;
  parsedArchive.summary.totalPackedSize = 0;

  std::map<std::string, std::string> archiveInfo;
  std::vector<std::map<std::string, std::string> > entryBlocks;
  std::map<std::string, std::string> currentEntry;
  bool hasCurrentEntry = false;
  bool inEntrySection = false;

  std::string normalizedText = stdoutText;
  normalizedText.erase(
      std::remove(normalizedText.begin(), normalizedText.end(), '\r'),
      normalizedText.end());
  std::vector<std::string> lines = SplitString(normalizedText, '\n');

  for (size_t index = 0; index < lines.size(); ++index) {
    const std::string& line = lines[index];

    if (line == "----------") {
      if (hasCurrentEntry && !currentEntry.empty()) {
        entryBlocks.push_back(currentEntry);
      }
      currentEntry.clear();
      hasCurrentEntry = false;
      inEntrySection = true;
      continue;
    }

    if (TrimAsciiWhitespace(line).empty()) {
      if (inEntrySection && hasCurrentEntry && !currentEntry.empty()) {
        entryBlocks.push_back(currentEntry);
        currentEntry.clear();
        hasCurrentEntry = false;
      }
      continue;
    }

    std::string key;
    std::string value;
    if (!ParseKeyValueLine(line, &key, &value)) {
      continue;
    }

    if (!inEntrySection) {
      archiveInfo[key] = value;
      continue;
    }

    hasCurrentEntry = true;
    currentEntry[key] = value;
  }

  if (hasCurrentEntry && !currentEntry.empty()) {
    entryBlocks.push_back(currentEntry);
  }

  if (archiveInfo.find("Type") != archiveInfo.end()) {
    parsedArchive.archiveType = archiveInfo["Type"];
  }
  if (archiveInfo.find("Physical Size") != archiveInfo.end()) {
    parsedArchive.physicalSize = ParseInt64(archiveInfo["Physical Size"]);
  }

  parsedArchive.entries.reserve(entryBlocks.size());
  for (size_t index = 0; index < entryBlocks.size(); ++index) {
    if (entryBlocks[index].find("Path") == entryBlocks[index].end()) {
      continue;
    }

    ArchiveEntry entry = BuildArchiveEntry(entryBlocks[index]);
    parsedArchive.entries.push_back(entry);
    parsedArchive.summary.entryCount += 1;

    if (entry.isDirectory) {
      parsedArchive.summary.directoryCount += 1;
    } else {
      parsedArchive.summary.fileCount += 1;
      if (entry.size > 0) {
        parsedArchive.summary.totalSize += entry.size;
      }
      if (entry.packedSize > 0) {
        parsedArchive.summary.totalPackedSize += entry.packedSize;
      }
    }
  }

  return parsedArchive;
}

ArchiveTreeNode* FindChildNode(ArchiveTreeNode* parentNode, const std::string& childName) {
  for (size_t index = 0; index < parentNode->children.size(); ++index) {
    if (parentNode->children[index]->name == childName) {
      return parentNode->children[index].get();
    }
  }

  return nullptr;
}

void ApplyEntryToNode(ArchiveTreeNode* node, const ArchiveEntry& entry) {
  node->isDirectory = entry.isDirectory;
  node->implicit = false;
  node->size = entry.size;
  node->packedSize = entry.packedSize;
  node->modifiedAt = entry.modifiedAt;
  node->createdAt = entry.createdAt;
  node->accessedAt = entry.accessedAt;
  node->hasEncrypted = entry.hasEncrypted;
  node->encrypted = entry.encrypted;
  node->attributes = entry.attributes;
  node->method = entry.method;
  node->comment = entry.comment;
  node->crc = entry.crc;
  node->hostOs = entry.hostOs;
  node->version = entry.version;
  node->volumeIndex = entry.volumeIndex;
  node->offset = entry.offset;
}

void SortTreeChildren(ArchiveTreeNode* node) {
  std::sort(
      node->children.begin(),
      node->children.end(),
      [](const std::unique_ptr<ArchiveTreeNode>& leftNode,
         const std::unique_ptr<ArchiveTreeNode>& rightNode) {
        if (leftNode->isDirectory != rightNode->isDirectory) {
          return leftNode->isDirectory;
        }

        return leftNode->name < rightNode->name;
      });

  for (size_t index = 0; index < node->children.size(); ++index) {
    SortTreeChildren(node->children[index].get());
  }
}

ArchiveTreeNode BuildArchiveTree(const std::vector<ArchiveEntry>& entries) {
  ArchiveTreeNode rootNode;
  rootNode.name = "";
  rootNode.path = "";
  rootNode.isDirectory = true;
  rootNode.implicit = false;
  rootNode.size = -1;
  rootNode.packedSize = -1;
  rootNode.hasEncrypted = false;
  rootNode.encrypted = false;
  rootNode.volumeIndex = -1;
  rootNode.offset = -1;

  for (size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
    const ArchiveEntry& entry = entries[entryIndex];
    std::vector<std::string> pathSegments = SplitString(entry.path, '/');
    ArchiveTreeNode* currentNode = &rootNode;
    std::string currentPath;

    for (size_t index = 0; index < pathSegments.size(); ++index) {
      const std::string& segment = pathSegments[index];
      if (segment.empty()) {
        continue;
      }

      if (!currentPath.empty()) {
        currentPath += "/";
      }
      currentPath += segment;

      const bool isLeafNode = index == pathSegments.size() - 1;
      ArchiveTreeNode* nextNode = FindChildNode(currentNode, segment);
      if (nextNode == nullptr) {
        std::unique_ptr<ArchiveTreeNode> childNode(new ArchiveTreeNode());
        childNode->name = segment;
        childNode->path = currentPath;
        childNode->isDirectory = !isLeafNode || entry.isDirectory;
        childNode->implicit = !isLeafNode;
        childNode->size = -1;
        childNode->packedSize = -1;
        childNode->hasEncrypted = false;
        childNode->encrypted = false;
        childNode->volumeIndex = -1;
        childNode->offset = -1;
        currentNode->children.push_back(std::move(childNode));
        nextNode = currentNode->children.back().get();
      }

      if (isLeafNode) {
        ApplyEntryToNode(nextNode, entry);
      }

      currentNode = nextNode;
    }
  }

  SortTreeChildren(&rootNode);
  return rootNode;
}

void SetStringOrNull(
    Napi::Env env, Napi::Object object, const char* key, const std::string& value) {
  if (value.empty()) {
    object.Set(key, env.Null());
    return;
  }

  object.Set(key, Napi::String::New(env, value));
}

void SetInt64OrNull(
    Napi::Env env, Napi::Object object, const char* key, int64_t value) {
  if (value < 0) {
    object.Set(key, env.Null());
    return;
  }

  object.Set(key, Napi::Number::New(env, static_cast<double>(value)));
}

Napi::Object BuildArchiveEntryObject(Napi::Env env, const ArchiveEntry& entry) {
  Napi::Object object = Napi::Object::New(env);
  object.Set("path", Napi::String::New(env, entry.path));
  object.Set("name", Napi::String::New(env, entry.name));
  object.Set("type", Napi::String::New(env, entry.isDirectory ? "directory" : "file"));
  SetInt64OrNull(env, object, "size", entry.size);
  SetInt64OrNull(env, object, "packedSize", entry.packedSize);
  SetStringOrNull(env, object, "modifiedAt", entry.modifiedAt);
  SetStringOrNull(env, object, "createdAt", entry.createdAt);
  SetStringOrNull(env, object, "accessedAt", entry.accessedAt);
  if (entry.hasEncrypted) {
    object.Set("encrypted", Napi::Boolean::New(env, entry.encrypted));
  } else {
    object.Set("encrypted", env.Null());
  }
  SetStringOrNull(env, object, "attributes", entry.attributes);
  SetStringOrNull(env, object, "method", entry.method);
  SetStringOrNull(env, object, "comment", entry.comment);
  SetStringOrNull(env, object, "crc", entry.crc);
  SetStringOrNull(env, object, "hostOs", entry.hostOs);
  SetStringOrNull(env, object, "version", entry.version);
  SetInt64OrNull(env, object, "volumeIndex", entry.volumeIndex);
  SetInt64OrNull(env, object, "offset", entry.offset);
  return object;
}

Napi::Object BuildArchiveTreeNodeObject(Napi::Env env, const ArchiveTreeNode& node) {
  Napi::Object object = Napi::Object::New(env);
  object.Set("name", Napi::String::New(env, node.name));
  object.Set("path", Napi::String::New(env, node.path));
  object.Set("type", Napi::String::New(env, node.isDirectory ? "directory" : "file"));
  SetInt64OrNull(env, object, "size", node.size);
  SetInt64OrNull(env, object, "packedSize", node.packedSize);
  SetStringOrNull(env, object, "modifiedAt", node.modifiedAt);
  SetStringOrNull(env, object, "createdAt", node.createdAt);
  SetStringOrNull(env, object, "accessedAt", node.accessedAt);
  if (node.hasEncrypted) {
    object.Set("encrypted", Napi::Boolean::New(env, node.encrypted));
  } else {
    object.Set("encrypted", env.Null());
  }
  SetStringOrNull(env, object, "attributes", node.attributes);
  SetStringOrNull(env, object, "method", node.method);
  SetStringOrNull(env, object, "comment", node.comment);
  SetStringOrNull(env, object, "crc", node.crc);
  SetStringOrNull(env, object, "hostOs", node.hostOs);
  SetStringOrNull(env, object, "version", node.version);
  SetInt64OrNull(env, object, "volumeIndex", node.volumeIndex);
  SetInt64OrNull(env, object, "offset", node.offset);
  object.Set("implicit", Napi::Boolean::New(env, node.implicit));

  if (node.isDirectory) {
    Napi::Array children =
        Napi::Array::New(env, static_cast<uint32_t>(node.children.size()));
    for (size_t index = 0; index < node.children.size(); ++index) {
      children[index] = BuildArchiveTreeNodeObject(env, *node.children[index]);
    }
    object.Set("children", children);
  }

  return object;
}

Napi::Object BuildTotalsObject(Napi::Env env, const ArchiveSummary& summary) {
  Napi::Object object = Napi::Object::New(env);
  object.Set("entryCount", Napi::Number::New(env, static_cast<double>(summary.entryCount)));
  object.Set("fileCount", Napi::Number::New(env, static_cast<double>(summary.fileCount)));
  object.Set(
      "directoryCount",
      Napi::Number::New(env, static_cast<double>(summary.directoryCount)));
  object.Set("totalSize", Napi::Number::New(env, static_cast<double>(summary.totalSize)));
  object.Set(
      "totalPackedSize",
      Napi::Number::New(env, static_cast<double>(summary.totalPackedSize)));
  return object;
}

Napi::Value InspectArchive(const Napi::CallbackInfo& info);
Napi::Value ParseArchiveStructure(const Napi::CallbackInfo& info);
Napi::Value ExtractArchive(const Napi::CallbackInfo& info);
Napi::Value InspectArchiveAsync(const Napi::CallbackInfo& info);
Napi::Value ExtractArchiveAsync(const Napi::CallbackInfo& info);
Napi::Value GenerateFileMD5(const Napi::CallbackInfo& info);
Napi::Value VerifyFileMD5(const Napi::CallbackInfo& info);
bool EnsureDirectoryExists(const std::wstring& directoryPath, std::string* errorMessage);
Napi::Value PrepareModUninstall(const Napi::CallbackInfo& info);
Napi::Value ExecuteModUninstall(const Napi::CallbackInfo& info);

FileOptions ParseOptions(const Napi::Value& value, bool allowOverwrite) {
  FileOptions options;
  options.overwrite = false;

  if (value.IsUndefined() || value.IsNull()) {
    return options;
  }

  if (!value.IsObject() || value.IsArray()) {
    throw Napi::TypeError::New(value.Env(), "options 必须是对象");
  }

  Napi::Object object = value.As<Napi::Object>();
  if (object.Has("password")) {
    Napi::Value passwordValue = object.Get("password");
    if (!passwordValue.IsString()) {
      throw Napi::TypeError::New(value.Env(), "options.password 必须是字符串");
    }
    options.password =
        vertex::shared::Utf8ToWide(passwordValue.As<Napi::String>().Utf8Value());
  }

  if (allowOverwrite && object.Has("overwrite")) {
    Napi::Value overwriteValue = object.Get("overwrite");
    if (!overwriteValue.IsBoolean()) {
      throw Napi::TypeError::New(value.Env(), "options.overwrite 必须是布尔值");
    }
    options.overwrite = overwriteValue.As<Napi::Boolean>().Value();
  }

  return options;
}

ParsedArchive InspectArchiveInternal(const std::wstring& archivePath, const FileOptions& options) {
  const std::wstring executablePath = Resolve7ZipExecutablePath();
  if (!IsExistingFile(executablePath)) {
    throw std::runtime_error(
        "未找到 7-Zip 运行时，请先执行 npm run build 生成 dist\\file\\7z.exe");
  }

  if (!IsExistingFile(archivePath)) {
    throw std::runtime_error("archivePath 不存在: " + vertex::shared::WideToUtf8(archivePath));
  }

  std::vector<std::wstring> args;
  args.push_back(L"l");
  args.push_back(L"-slt");
  args.push_back(L"-sccUTF-8");
  if (!options.password.empty()) {
    args.push_back(L"-p" + options.password);
  }
  args.push_back(archivePath);

  CommandResult result = RunCommand(executablePath, args);
  if (result.exitCode != 0) {
    const std::string details =
        !TrimAsciiWhitespace(result.stderrText).empty()
            ? TrimAsciiWhitespace(result.stderrText)
            : TrimAsciiWhitespace(result.stdoutText);
    throw std::runtime_error(
        "7-Zip 列目录失败，退出码 " + std::to_string(result.exitCode) +
        (details.empty() ? "" : "，详情：" + details));
  }

  return ParseArchiveListing(result.stdoutText);
}

ParsedArchive ExtractArchiveInternal(
    const std::wstring& archivePath,
    const std::wstring& outputDirectory,
    const FileOptions& options) {
  const ParsedArchive parsedArchive = InspectArchiveInternal(archivePath, options);

  std::string directoryError;
  if (!EnsureDirectoryExists(outputDirectory, &directoryError)) {
    throw std::runtime_error(
        "创建输出目录失败: " + vertex::shared::WideToUtf8(outputDirectory) +
        (directoryError.empty() ? "" : "，详情：" + directoryError));
  }

  std::vector<std::wstring> args;
  args.push_back(L"x");
  if (!options.password.empty()) {
    args.push_back(L"-p" + options.password);
  }
  args.push_back(archivePath);
  args.push_back(L"-o" + outputDirectory);
  args.push_back(options.overwrite ? L"-aoa" : L"-aos");
  args.push_back(L"-y");
  args.push_back(L"-bb0");
  args.push_back(L"-bso0");
  args.push_back(L"-bsp0");
  args.push_back(L"-sccUTF-8");

  CommandResult result = RunCommand(Resolve7ZipExecutablePath(), args);
  if (result.exitCode != 0) {
    const std::string details =
        !TrimAsciiWhitespace(result.stderrText).empty()
            ? TrimAsciiWhitespace(result.stderrText)
            : TrimAsciiWhitespace(result.stdoutText);
    throw std::runtime_error(
        "7-Zip 解压失败，退出码 " + std::to_string(result.exitCode) +
        (details.empty() ? "" : "，详情：" + details));
  }

  return parsedArchive;
}

Napi::Object BuildInspectResultObject(
    Napi::Env env,
    const std::wstring& archivePath,
    const ParsedArchive& parsedArchive) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("archivePath", Napi::String::New(env, vertex::shared::WideToUtf8(archivePath)));
  SetStringOrNull(env, result, "archiveType", parsedArchive.archiveType);
  SetInt64OrNull(env, result, "physicalSize", parsedArchive.physicalSize);

  Napi::Array entries =
      Napi::Array::New(env, static_cast<uint32_t>(parsedArchive.entries.size()));
  for (size_t index = 0; index < parsedArchive.entries.size(); ++index) {
    entries[index] = BuildArchiveEntryObject(env, parsedArchive.entries[index]);
  }
  result.Set("entries", entries);

  ArchiveTreeNode treeRoot = BuildArchiveTree(parsedArchive.entries);
  Napi::Array tree =
      Napi::Array::New(env, static_cast<uint32_t>(treeRoot.children.size()));
  for (size_t index = 0; index < treeRoot.children.size(); ++index) {
    tree[index] = BuildArchiveTreeNodeObject(env, *treeRoot.children[index]);
  }
  result.Set("tree", tree);
  result.Set("totals", BuildTotalsObject(env, parsedArchive.summary));

  Napi::Object engine = Napi::Object::New(env);
  engine.Set(
      "command",
      Napi::String::New(env, vertex::shared::WideToUtf8(Resolve7ZipExecutablePath())));
  result.Set("engine", engine);
  return result;
}

std::wstring ParseRequiredPathArgument(
    const Napi::CallbackInfo& info, size_t index, const char* fieldName) {
  Napi::Env env = info.Env();
  if (info.Length() <= index || !info[index].IsString()) {
    throw Napi::TypeError::New(env, std::string(fieldName) + " 必须是字符串路径");
  }

  return EnsureAbsolutePath(
      vertex::shared::Utf8ToWide(info[index].As<Napi::String>().Utf8Value()));
}

std::string GetOptionalStringProperty(
    const Napi::Object& object,
    const char* propertyName,
    const std::string& defaultValue = "") {
  if (!object.Has(propertyName)) {
    return defaultValue;
  }

  const Napi::Value value = object.Get(propertyName);
  if (value.IsString()) {
    return TrimAsciiWhitespace(value.As<Napi::String>().Utf8Value());
  }
  if (value.IsNumber()) {
    return TrimAsciiWhitespace(value.ToString().Utf8Value());
  }
  return defaultValue;
}

int32_t GetOptionalInt32Property(
    const Napi::Object& object,
    const char* propertyName,
    int32_t defaultValue = 0) {
  if (!object.Has(propertyName)) {
    return defaultValue;
  }

  const Napi::Value value = object.Get(propertyName);
  if (value.IsNumber()) {
    return ToSafeInt32(value.As<Napi::Number>().DoubleValue(), defaultValue);
  }
  if (value.IsString()) {
    const std::string text = TrimAsciiWhitespace(value.As<Napi::String>().Utf8Value());
    if (text.empty()) {
      return defaultValue;
    }
    try {
      return ToSafeInt32(std::stod(text), defaultValue);
    } catch (...) {
      return defaultValue;
    }
  }
  return defaultValue;
}

Napi::Object GetOptionalObjectProperty(const Napi::Object& object, const char* propertyName) {
  if (!object.Has(propertyName)) {
    return Napi::Object();
  }

  const Napi::Value value = object.Get(propertyName);
  return value.IsObject() && !value.IsArray() ? value.As<Napi::Object>() : Napi::Object();
}

std::vector<std::string> ParseStringArrayValue(const Napi::Value& value) {
  std::vector<std::string> result;
  if (!value.IsArray()) {
    return result;
  }

  const Napi::Array array = value.As<Napi::Array>();
  const uint32_t length = array.Length();
  result.reserve(length);
  for (uint32_t index = 0; index < length; ++index) {
    const Napi::Value item = array.Get(index);
    if (!item.IsString() && !item.IsNumber()) {
      continue;
    }
    const std::string text = item.IsString()
                                 ? TrimAsciiWhitespace(item.As<Napi::String>().Utf8Value())
                                 : TrimAsciiWhitespace(item.ToString().Utf8Value());
    if (!text.empty()) {
      result.push_back(text);
    }
  }

  return DedupeTextList(result);
}

DetectMarkers ParseDetectMarkers(const Napi::Value& value) {
  DetectMarkers markers;
  if (!value.IsObject() || value.IsArray()) {
    return markers;
  }

  const Napi::Object object = value.As<Napi::Object>();
  if (object.Has("any_files")) {
    markers.anyFiles = ParseStringArrayValue(object.Get("any_files"));
  }
  if (object.Has("all_files")) {
    markers.allFiles = ParseStringArrayValue(object.Get("all_files"));
  }
  return markers;
}

DetectRootFromRule ParseDetectRootFromRule(const Napi::Value& value) {
  DetectRootFromRule rule;
  rule.type = "exe_dir";
  rule.up = 0;

  if (!value.IsObject() || value.IsArray()) {
    return rule;
  }

  const Napi::Object object = value.As<Napi::Object>();
  rule.type = ToLowerAscii(GetOptionalStringProperty(object, "type", "exe_dir"));
  rule.up = GetOptionalInt32Property(object, "up", 0);
  if (rule.up < 0) {
    rule.up = 0;
  }
  return rule;
}

DetectCandidate ParseDetectCandidate(const Napi::Object& object, int32_t fallbackIndex) {
  DetectCandidate candidate;
  candidate.index = GetOptionalInt32Property(object, "index", fallbackIndex);
  candidate.name = GetOptionalStringProperty(
      object, "name", "candidate_" + std::to_string(fallbackIndex + 1));
  candidate.type = ToLowerAscii(GetOptionalStringProperty(object, "type", "exe_search"));
  candidate.confidence = GetOptionalInt32Property(object, "confidence", 0);
  candidate.markers = ParseDetectMarkers(GetOptionalObjectProperty(object, "markers"));
  if (object.Has("exeGlobs")) {
    candidate.exeGlobs = ParseStringArrayValue(object.Get("exeGlobs"));
  } else if (object.Has("exe_globs")) {
    candidate.exeGlobs = ParseStringArrayValue(object.Get("exe_globs"));
  }

  if (object.Has("rootFrom")) {
    candidate.rootFrom = ParseDetectRootFromRule(object.Get("rootFrom"));
  } else if (object.Has("root_from")) {
    candidate.rootFrom = ParseDetectRootFromRule(object.Get("root_from"));
  } else {
    candidate.rootFrom = ParseDetectRootFromRule(Napi::Object());
  }

  return candidate;
}

std::vector<DetectCandidate> ParseDetectCandidateList(const Napi::Value& value) {
  std::vector<DetectCandidate> result;
  if (!value.IsArray()) {
    return result;
  }

  const Napi::Array array = value.As<Napi::Array>();
  const uint32_t length = array.Length();
  result.reserve(length);
  for (uint32_t index = 0; index < length; ++index) {
    const Napi::Value item = array.Get(index);
    if (!item.IsObject() || item.IsArray()) {
      continue;
    }

    const DetectCandidate candidate = ParseDetectCandidate(item.As<Napi::Object>(), static_cast<int32_t>(index));
    if (!candidate.type.empty()) {
      result.push_back(candidate);
    }
  }
  return result;
}

DetectLocalGameEntry ParseDetectLocalGameEntry(const Napi::Object& object) {
  DetectLocalGameEntry entry;
  entry.appId = GetOptionalInt32Property(object, "appId", GetOptionalInt32Property(object, "app_id", 0));
  entry.gameId = GetOptionalInt32Property(object, "gameId", GetOptionalInt32Property(object, "game_id", 0));

  const std::string exePathText = GetOptionalStringProperty(
      object, "clientExePath", GetOptionalStringProperty(object, "client_exe_path", ""));
  entry.clientExePath =
      exePathText.empty() ? L"" : EnsureAbsolutePath(vertex::shared::Utf8ToWide(exePathText));
  return entry;
}

std::vector<DetectLocalGameEntry> ParseDetectLocalGameList(const Napi::Value& value) {
  std::vector<DetectLocalGameEntry> result;
  if (!value.IsArray()) {
    return result;
  }

  const Napi::Array array = value.As<Napi::Array>();
  const uint32_t length = array.Length();
  result.reserve(length);
  for (uint32_t index = 0; index < length; ++index) {
    const Napi::Value item = array.Get(index);
    if (!item.IsObject() || item.IsArray()) {
      continue;
    }

    result.push_back(ParseDetectLocalGameEntry(item.As<Napi::Object>()));
  }
  return result;
}

DetectGameRootInput ParseDetectGameRootInput(const Napi::Object& object) {
  DetectGameRootInput input;
  input.strategy = GetOptionalStringProperty(object, "strategy", "highest_confidence");
  input.domainName = GetOptionalStringProperty(object, "domainName", "");
  input.nexusGameId = GetOptionalInt32Property(object, "nexusGameId", 0);

  if (object.Has("candidates")) {
    input.candidates = ParseDetectCandidateList(object.Get("candidates"));
  }
  if (object.Has("localGameList")) {
    input.localGameList = ParseDetectLocalGameList(object.Get("localGameList"));
  }

  return input;
}

bool EnsureDirectoryExists(const std::wstring& directoryPath, std::string* errorMessage) {
  int result = SHCreateDirectoryExW(nullptr, directoryPath.c_str(), nullptr);
  if (result == ERROR_SUCCESS ||
      result == ERROR_ALREADY_EXISTS ||
      result == ERROR_FILE_EXISTS) {
    return true;
  }

  if (errorMessage != nullptr) {
    *errorMessage = vertex::shared::GetSystemErrorMessage(static_cast<DWORD>(result));
  }
  return false;
}

bool IsDotEntryName(const wchar_t* entryName) {
  return wcscmp(entryName, L".") == 0 || wcscmp(entryName, L"..") == 0;
}

std::vector<std::wstring> GetLogicalDriveRoots() {
  DWORD requiredLength = GetLogicalDriveStringsW(0, nullptr);
  if (requiredLength == 0) {
    return std::vector<std::wstring>();
  }

  std::vector<wchar_t> buffer(requiredLength + 1, L'\0');
  DWORD actualLength = GetLogicalDriveStringsW(requiredLength, &buffer[0]);
  if (actualLength == 0) {
    return std::vector<std::wstring>();
  }

  std::vector<std::wstring> drives;
  const wchar_t* cursor = &buffer[0];
  while (*cursor != L'\0') {
    drives.push_back(std::wstring(cursor));
    cursor += wcslen(cursor) + 1;
  }
  return drives;
}

bool ShouldSkipSearchDirectory(const std::wstring& directoryPath) {
  const std::wstring normalizedPath = ToLowerWide(directoryPath);
  static const wchar_t* kExcludedDirectoryMarkers[] = {
      L"\\windows\\",
      L"\\program files\\",
      L"\\program files (x86)\\",
      L"\\system volume information\\",
      L"\\$recycle.bin\\",
      L"\\.pnpm-store\\",
      L"\\node_modules\\",
      L"\\.git\\",
      L"\\.vscode\\",
      L"\\.idea\\",
      L"\\cache\\",
      L"\\temp\\",
      L"\\logs\\",
  };

  for (size_t index = 0; index < sizeof(kExcludedDirectoryMarkers) / sizeof(kExcludedDirectoryMarkers[0]);
       ++index) {
    if (normalizedPath.find(kExcludedDirectoryMarkers[index]) != std::wstring::npos) {
      return true;
    }
  }

  return false;
}

bool SearchDriveForFilename(
    const std::wstring& driveRoot,
    const std::wstring& targetFileNameLower,
    DriveScanSummary* summary,
    std::wstring* foundPath) {
  if (summary == nullptr || foundPath == nullptr) {
    return false;
  }

  std::vector<std::wstring> queue;
  queue.push_back(EnsureAbsolutePath(driveRoot));
  size_t cursor = 0;
  std::set<std::wstring> visited;

  while (cursor < queue.size()) {
    const std::wstring currentDirectory = queue[cursor++];
    const std::wstring visitKey = ToLowerWide(currentDirectory);
    if (visited.find(visitKey) != visited.end()) {
      continue;
    }
    visited.insert(visitKey);

    if (ShouldSkipSearchDirectory(currentDirectory)) {
      continue;
    }

    const DWORD attributes = GetFileAttributesW(currentDirectory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      continue;
    }

    summary->scannedDirectories += 1;

    const std::wstring searchPattern = JoinWindowsPath(currentDirectory, L"*");
    WIN32_FIND_DATAW findData;
    HANDLE findHandle = FindFirstFileW(searchPattern.c_str(), &findData);
    if (findHandle == INVALID_HANDLE_VALUE) {
      continue;
    }

    do {
      if (IsDotEntryName(findData.cFileName)) {
        continue;
      }

      const std::wstring entryName(findData.cFileName);
      const std::wstring fullPath = JoinWindowsPath(currentDirectory, entryName);
      const bool isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      const bool isReparsePoint = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

      if (!isDirectory) {
        if (ToLowerWide(entryName) == targetFileNameLower) {
          *foundPath = fullPath;
          FindClose(findHandle);
          return true;
        }
        continue;
      }

      if (isReparsePoint || ShouldSkipSearchDirectory(fullPath)) {
        continue;
      }

      queue.push_back(fullPath);
    } while (FindNextFileW(findHandle, &findData) != 0);

    FindClose(findHandle);
  }

  return false;
}

DriveScanSummary ScanDriveByFilenameInternal(const std::wstring& targetFileName) {
  const std::wstring normalizedTarget = ToLowerWide(targetFileName);
  if (normalizedTarget.empty()) {
    throw std::runtime_error("targetFileName 不能为空");
  }

  std::vector<std::wstring> driveRoots = GetLogicalDriveRoots();
  std::stable_sort(driveRoots.begin(), driveRoots.end(), [](const std::wstring& left, const std::wstring& right) {
    const std::wstring leftLower = ToLowerWide(left);
    const std::wstring rightLower = ToLowerWide(right);
    const bool leftIsCDrive = leftLower.rfind(L"c:\\", 0) == 0;
    const bool rightIsCDrive = rightLower.rfind(L"c:\\", 0) == 0;

    if (leftIsCDrive != rightIsCDrive) {
      return !leftIsCDrive;
    }

    return leftLower < rightLower;
  });

  DriveScanSummary summary = CreateEmptyDriveScanSummary();
  summary.scannedDrives = static_cast<int32_t>(driveRoots.size());

  for (size_t index = 0; index < driveRoots.size(); ++index) {
    const std::wstring& driveRoot = driveRoots[index];
    std::wstring foundPath;
    if (SearchDriveForFilename(driveRoot, normalizedTarget, &summary, &foundPath)) {
      summary.found = true;
      summary.path = foundPath;
      return summary;
    }
  }

  return summary;
}

std::vector<std::string> CollectMarkerFileHints(const DetectMarkers& markers) {
  std::vector<std::string> merged = markers.anyFiles;
  merged.insert(merged.end(), markers.allFiles.begin(), markers.allFiles.end());
  for (size_t index = 0; index < merged.size(); ++index) {
    merged[index] = NormalizeFilename(merged[index]);
  }
  return DedupeTextList(merged);
}

std::vector<std::string> CollectCandidateExeHints(const DetectCandidate& candidate) {
  std::vector<std::string> output;
  const std::vector<std::string> markerHints = CollectMarkerFileHints(candidate.markers);
  for (size_t index = 0; index < markerHints.size(); ++index) {
    const std::string lowerName = ToLowerAscii(markerHints[index]);
    if (lowerName.size() >= 4 && lowerName.substr(lowerName.size() - 4) == ".exe") {
      output.push_back(markerHints[index]);
    }
  }

  for (size_t index = 0; index < candidate.exeGlobs.size(); ++index) {
    const std::string fileName = ExtractFileNameFromGlob(candidate.exeGlobs[index]);
    if (!fileName.empty()) {
      output.push_back(fileName);
    }
  }

  return DedupeTextList(output);
}

bool IsExistingDirectory(const std::wstring& directoryPath) {
  const DWORD attributes = GetFileAttributesW(directoryPath.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ResolveRootFromExePath(
    const std::wstring& exePath,
    const DetectRootFromRule& rootFrom) {
  if (exePath.empty()) {
    return L"";
  }

  std::wstring current = exePath;
  const std::string rootFromType =
      rootFrom.type.empty() ? "exe_dir" : ToLowerAscii(rootFrom.type);

  if (rootFromType == "exe_dir") {
    current = GetDirectoryName(exePath);
  }

  for (int32_t index = 0; index < rootFrom.up; ++index) {
    const std::wstring parentPath = GetDirectoryName(current);
    if (parentPath.empty() || parentPath == current) {
      break;
    }
    current = parentPath;
  }

  return current;
}

bool CheckMarkerFiles(const std::wstring& rootPath, const DetectMarkers& markers) {
  if (rootPath.empty()) {
    return false;
  }

  std::vector<std::string> anyFiles = DedupeTextList(markers.anyFiles);
  std::vector<std::string> allFiles = DedupeTextList(markers.allFiles);
  if (anyFiles.empty() && allFiles.empty()) {
    return true;
  }

  for (size_t index = 0; index < allFiles.size(); ++index) {
    const std::wstring fullPath =
        JoinWindowsPath(rootPath, NormalizeRelativeFilePathWide(allFiles[index]));
    if (GetFileAttributesW(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return false;
    }
  }

  if (!anyFiles.empty()) {
    for (size_t index = 0; index < anyFiles.size(); ++index) {
      const std::wstring fullPath =
          JoinWindowsPath(rootPath, NormalizeRelativeFilePathWide(anyFiles[index]));
      if (GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
      }
    }
    return false;
  }

  return true;
}

DetectMatchRecord TryMatchByLocalGameList(
    const DetectCandidate& candidate,
    const std::vector<DetectLocalGameEntry>& localGameList) {
  DetectMatchRecord record = CreateEmptyDetectMatchRecord();
  if (localGameList.empty()) {
    return record;
  }

  std::vector<std::string> hintExeList = CollectCandidateExeHints(candidate);
  std::set<std::wstring> hintExeNameSet;
  for (size_t index = 0; index < hintExeList.size(); ++index) {
    hintExeNameSet.insert(ToLowerWide(vertex::shared::Utf8ToWide(hintExeList[index])));
  }
  const bool hasExeHints = !hintExeNameSet.empty();

  for (size_t index = 0; index < localGameList.size(); ++index) {
    const DetectLocalGameEntry& game = localGameList[index];
    if (game.clientExePath.empty() || !IsExistingFile(game.clientExePath)) {
      continue;
    }

    const std::wstring exeName = ToLowerWide(GetBaseName(game.clientExePath));
    if (hasExeHints && hintExeNameSet.find(exeName) == hintExeNameSet.end()) {
      continue;
    }

    const std::wstring rootPath = ResolveRootFromExePath(game.clientExePath, candidate.rootFrom);
    if (rootPath.empty() || !IsExistingDirectory(rootPath)) {
      continue;
    }

    if (!CheckMarkerFiles(rootPath, candidate.markers)) {
      continue;
    }

    record.matched = true;
    record.rootPath = rootPath;
    record.exePath = game.clientExePath;
    record.appId = game.appId;
    record.gameId = game.gameId;
    record.source = "local_game_list";
    return record;
  }

  return record;
}

DetectMatchRecord TryMatchByDriveScan(const DetectCandidate& candidate) {
  DetectMatchRecord record = CreateEmptyDetectMatchRecord();
  const std::vector<std::string> hintExeList = CollectCandidateExeHints(candidate);

  for (size_t index = 0; index < hintExeList.size(); ++index) {
    const std::string exeName = hintExeList[index];
    if (ToLowerAscii(exeName).size() < 4 ||
        ToLowerAscii(exeName).substr(ToLowerAscii(exeName).size() - 4) != ".exe") {
      continue;
    }

    const DriveScanSummary summary =
        ScanDriveByFilenameInternal(vertex::shared::Utf8ToWide(exeName));
    if (!summary.found || summary.path.empty()) {
      continue;
    }

    const std::wstring rootPath = ResolveRootFromExePath(summary.path, candidate.rootFrom);
    if (rootPath.empty() || !IsExistingDirectory(rootPath)) {
      continue;
    }

    if (!CheckMarkerFiles(rootPath, candidate.markers)) {
      continue;
    }

    record.matched = true;
    record.rootPath = rootPath;
    record.exePath = summary.path;
    record.appId = 0;
    record.gameId = 0;
    record.source = "drive_scan";
    return record;
  }

  return record;
}

DetectGameRootOutput DetectGameRootByRuleInternal(const DetectGameRootInput& input) {
  DetectGameRootOutput output = CreateEmptyDetectGameRootOutput();
  output.strategy = input.strategy.empty() ? "highest_confidence" : input.strategy;
  output.domainName = input.domainName;
  output.nexusGameId = input.nexusGameId;

  if (input.candidates.empty()) {
    output.reason = "no_candidates";
    return output;
  }

  std::vector<DetectMatchRecord> matchRecords;
  for (size_t index = 0; index < input.candidates.size(); ++index) {
    const DetectCandidate& candidate = input.candidates[index];
    DetectMatchRecord record = TryMatchByLocalGameList(candidate, input.localGameList);
    if (!record.matched) {
      record = TryMatchByDriveScan(candidate);
    }
    if (!record.matched) {
      continue;
    }

    record.candidateName = candidate.name;
    record.candidateType = candidate.type;
    record.confidence = candidate.confidence;
    matchRecords.push_back(record);

    if (output.strategy == "highest_confidence") {
      output.success = true;
      output.record = record;
      return output;
    }
  }

  if (matchRecords.empty()) {
    output.reason = "not_found";
    return output;
  }

  const DetectMatchRecord* bestRecord = &matchRecords[0];
  for (size_t index = 1; index < matchRecords.size(); ++index) {
    if (matchRecords[index].confidence > bestRecord->confidence) {
      bestRecord = &matchRecords[index];
    }
  }

  output.success = true;
  output.record = *bestRecord;
  return output;
}

bool TryListDirectoryEntries(
    const std::wstring& directoryPath,
    std::vector<std::pair<std::wstring, bool> >* entries) {
  if (entries == nullptr) {
    return false;
  }

  entries->clear();
  const std::wstring searchPattern = JoinWindowsPath(directoryPath, L"*");
  WIN32_FIND_DATAW findData;
  HANDLE findHandle = FindFirstFileW(searchPattern.c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) {
    return false;
  }

  do {
    if (IsDotEntryName(findData.cFileName)) {
      continue;
    }

    const std::wstring entryName(findData.cFileName);
    const bool isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entries->push_back(std::make_pair(entryName, isDirectory));
  } while (FindNextFileW(findHandle, &findData) != 0);

  FindClose(findHandle);
  return true;
}

std::vector<PackageRootCandidate> ListPackageRootCandidatesInternal(
    const std::wstring& stagingPath,
    int32_t maxDepth) {
  std::vector<PackageRootCandidate> result;
  const std::wstring safeStagingPath = EnsureAbsolutePath(stagingPath);
  if (!IsExistingDirectory(safeStagingPath)) {
    return result;
  }

  std::vector<PackageRootCandidate> queue;
  std::set<std::wstring> visited;
  queue.push_back(PackageRootCandidate{safeStagingPath, "", 0});
  size_t cursor = 0;

  while (cursor < queue.size()) {
    const PackageRootCandidate current = queue[cursor++];
    const std::wstring visitKey = ToLowerWide(current.rootPath);
    if (visited.find(visitKey) != visited.end()) {
      continue;
    }
    visited.insert(visitKey);
    result.push_back(current);

    if (maxDepth >= 0 && current.depth >= maxDepth) {
      continue;
    }

    std::vector<std::pair<std::wstring, bool> > entries;
    if (!TryListDirectoryEntries(current.rootPath, &entries)) {
      continue;
    }

    for (size_t index = 0; index < entries.size(); ++index) {
      const std::wstring& entryName = entries[index].first;
      const bool isDirectory = entries[index].second;
      if (!isDirectory || IsPreparedRuleDirectoryName(entryName)) {
        continue;
      }

      const std::wstring childRootPath = JoinWindowsPath(current.rootPath, entryName);
      const std::string childPackageRoot = NormalizeRelativeRulePathAscii(
          current.packageRoot.empty()
              ? vertex::shared::WideToUtf8(entryName)
              : current.packageRoot + "/" + vertex::shared::WideToUtf8(entryName));
      queue.push_back(
          PackageRootCandidate{childRootPath, childPackageRoot, current.depth + 1});
    }
  }

  return result;
}

ExecutableScanResult ScanExecutablesInDirectoryInternal(const std::wstring& dirPath) {
  ExecutableScanResult result = CreateEmptyExecutableScanResult();
  const std::wstring safeDirPath = EnsureAbsolutePath(dirPath);
  if (!IsExistingDirectory(safeDirPath)) {
    return result;
  }

  std::vector<std::wstring> directoryQueue;
  std::set<std::wstring> visited;
  directoryQueue.push_back(safeDirPath);
  size_t cursor = 0;

  while (cursor < directoryQueue.size()) {
    const std::wstring currentDirectory = directoryQueue[cursor++];
    const std::wstring visitKey = ToLowerWide(currentDirectory);
    if (visited.find(visitKey) != visited.end()) {
      continue;
    }
    visited.insert(visitKey);

    std::vector<std::pair<std::wstring, bool> > entries;
    if (!TryListDirectoryEntries(currentDirectory, &entries)) {
      continue;
    }

    for (size_t index = 0; index < entries.size(); ++index) {
      const std::wstring& entryName = entries[index].first;
      const bool isDirectory = entries[index].second;
      const std::wstring fullPath = JoinWindowsPath(currentDirectory, entryName);

      if (isDirectory) {
        if (IsPreparedRuleDirectoryName(entryName)) {
          continue;
        }
        directoryQueue.push_back(fullPath);
        continue;
      }

      const std::wstring lowerName = ToLowerWide(entryName);
      if (lowerName.size() >= 4 && lowerName.substr(lowerName.size() - 4) == L".exe") {
        result.paths.push_back(fullPath);
      }
    }
  }

  if (result.paths.empty()) {
    result.type = 0;
    return result;
  }

  if (result.paths.size() == 1) {
    result.type = 1;
    result.path = result.paths[0];
    result.paths.clear();
    return result;
  }

  result.type = 2;
  return result;
}

DWORD GetFileAttributesMeasured(const std::wstring& targetPath, StagingMetrics* metrics) {
  AccumulateTimer timer(metrics == nullptr ? nullptr : &metrics->operations.lstatMs);
  return GetFileAttributesW(targetPath.c_str());
}

bool EnsureDirectoryExistsMeasured(
    const std::wstring& directoryPath,
    StagingMetrics* metrics,
    std::string* errorMessage) {
  AccumulateTimer timer(metrics == nullptr ? nullptr : &metrics->operations.ensureDirMs);
  return EnsureDirectoryExists(directoryPath, errorMessage);
}

bool ListDirectoryEntryNames(
    const std::wstring& directoryPath,
    std::vector<std::wstring>* entryNames,
    std::string* errorMessage) {
  const std::wstring searchPath = JoinWindowsPath(directoryPath, L"*");
  WIN32_FIND_DATAW findData;
  HANDLE findHandle = FindFirstFileW(searchPath.c_str(), &findData);

  if (findHandle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return true;
    }

    if (errorMessage != nullptr) {
      *errorMessage = vertex::shared::GetSystemErrorMessage(error);
    }
    return false;
  }

  do {
    if (!IsDotEntryName(findData.cFileName)) {
      entryNames->push_back(findData.cFileName);
    }
  } while (FindNextFileW(findHandle, &findData) != 0);

  FindClose(findHandle);
  return true;
}

bool ListDirectoryEntryNamesMeasured(
    const std::wstring& directoryPath,
    std::vector<std::wstring>* entryNames,
    StagingMetrics* metrics,
    std::string* errorMessage) {
  AccumulateTimer timer(metrics == nullptr ? nullptr : &metrics->operations.readdirMs);
  return ListDirectoryEntryNames(directoryPath, entryNames, errorMessage);
}

bool IsDirectoryEmpty(const std::wstring& directoryPath, bool* empty, std::string* errorMessage) {
  const std::wstring searchPath = JoinWindowsPath(directoryPath, L"*");
  WIN32_FIND_DATAW findData;
  HANDLE findHandle = FindFirstFileW(searchPath.c_str(), &findData);

  if (findHandle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      *empty = true;
      return true;
    }

    if (errorMessage != nullptr) {
      *errorMessage = vertex::shared::GetSystemErrorMessage(error);
    }
    return false;
  }

  *empty = true;
  do {
    if (!IsDotEntryName(findData.cFileName)) {
      *empty = false;
      break;
    }
  } while (FindNextFileW(findHandle, &findData) != 0);

  FindClose(findHandle);
  return true;
}

bool IsDirectoryEmptyMeasured(
    const std::wstring& directoryPath,
    bool* empty,
    StagingMetrics* metrics,
    std::string* errorMessage) {
  AccumulateTimer timer(metrics == nullptr ? nullptr : &metrics->operations.readdirMs);
  return IsDirectoryEmpty(directoryPath, empty, errorMessage);
}

bool DeleteMappedTargetPath(const std::wstring& targetPath, DWORD targetAttributes, DWORD* errorCode) {
  DWORD normalizedAttributes = targetAttributes;

  if ((normalizedAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
    const DWORD writableAttributes = normalizedAttributes & ~FILE_ATTRIBUTE_READONLY;
    if (SetFileAttributesW(targetPath.c_str(), writableAttributes) != 0) {
      normalizedAttributes = writableAttributes;
    }
  }

  if ((normalizedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
      (normalizedAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    const BOOL removed = RemoveDirectoryW(targetPath.c_str());
    if (!removed && errorCode != nullptr) {
      *errorCode = GetLastError();
    }
    return removed != 0;
  }

  if ((normalizedAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    if (errorCode != nullptr) {
      *errorCode = ERROR_DIRECTORY;
    }
    return false;
  }

  const BOOL deleted = DeleteFileW(targetPath.c_str());
  if (!deleted && errorCode != nullptr) {
    *errorCode = GetLastError();
  }
  return deleted != 0;
}

bool DeleteMappedTargetPathMeasured(
    const std::wstring& targetPath,
    DWORD targetAttributes,
    StagingMetrics* metrics,
    DWORD* errorCode) {
  AccumulateTimer timer(metrics == nullptr ? nullptr : &metrics->operations.unlinkMs);
  return DeleteMappedTargetPath(targetPath, targetAttributes, errorCode);
}

bool CreateHardLinkMeasured(
    const std::wstring& linkPath,
    const std::wstring& existingPath,
    StagingMetrics* metrics,
    DWORD* errorCode) {
  AccumulateTimer timer(metrics == nullptr ? nullptr : &metrics->operations.linkMs);
  const BOOL linked = CreateHardLinkW(linkPath.c_str(), existingPath.c_str(), nullptr);
  if (!linked && errorCode != nullptr) {
    *errorCode = GetLastError();
  }
  return linked != 0;
}

bool CopyFileMeasured(
    const std::wstring& sourcePath,
    const std::wstring& targetPath,
    StagingMetrics* metrics,
    DWORD* errorCode) {
  AccumulateTimer timer(metrics == nullptr ? nullptr : &metrics->operations.copyFileMs);
  const BOOL copied = CopyFileW(sourcePath.c_str(), targetPath.c_str(), FALSE);
  if (!copied && errorCode != nullptr) {
    *errorCode = GetLastError();
  }
  return copied != 0;
}

bool RemoveDirectoryMeasured(const std::wstring& directoryPath, StagingMetrics* metrics, DWORD* errorCode) {
  AccumulateTimer timer(metrics == nullptr ? nullptr : &metrics->operations.unlinkMs);
  const BOOL removed = RemoveDirectoryW(directoryPath.c_str());
  if (!removed && errorCode != nullptr) {
    *errorCode = GetLastError();
  }
  return removed != 0;
}

bool RunDeployRecursive(
    const std::wstring& srcRoot,
    const std::wstring& destRoot,
    DeploySummary* summary,
    StagingMetrics* metrics,
    std::string* errorMessage) {
  std::vector<std::pair<std::wstring, std::wstring> > stack;
  stack.push_back(std::make_pair(srcRoot, destRoot));

  while (!stack.empty()) {
    const std::pair<std::wstring, std::wstring> current = stack.back();
    stack.pop_back();

    const std::wstring& src = current.first;
    const std::wstring& dest = current.second;

    const DWORD srcAttr = GetFileAttributesMeasured(src, metrics);
    if (srcAttr == INVALID_FILE_ATTRIBUTES) {
      continue;
    }

    if ((srcAttr & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (srcAttr & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
      summary->directories += 1;

      std::string directoryError;
      if (!EnsureDirectoryExistsMeasured(dest, metrics, &directoryError)) {
        if (errorMessage != nullptr) {
          *errorMessage =
              "创建部署目录失败: " + vertex::shared::WideToUtf8(dest) +
              (directoryError.empty() ? "" : "，详情：" + directoryError);
        }
        return false;
      }

      std::vector<std::wstring> entryNames;
      std::string readdirError;
      if (!ListDirectoryEntryNamesMeasured(src, &entryNames, metrics, &readdirError)) {
        if (errorMessage != nullptr) {
          *errorMessage =
              "读取暂存目录失败: " + vertex::shared::WideToUtf8(src) +
              (readdirError.empty() ? "" : "，详情：" + readdirError);
        }
        return false;
      }

      for (size_t index = 0; index < entryNames.size(); ++index) {
        const std::wstring& entryName = entryNames[index];
        stack.push_back(std::make_pair(
            JoinWindowsPath(src, entryName),
            JoinWindowsPath(dest, entryName)));
      }
      continue;
    }

    const std::wstring destDir = GetDirectoryName(dest);
    if (!destDir.empty()) {
      std::string directoryError;
      if (!EnsureDirectoryExistsMeasured(destDir, metrics, &directoryError)) {
        if (errorMessage != nullptr) {
          *errorMessage =
              "创建目标目录失败: " + vertex::shared::WideToUtf8(destDir) +
              (directoryError.empty() ? "" : "，详情：" + directoryError);
        }
        return false;
      }
    }

    const DWORD destAttr = GetFileAttributesMeasured(dest, metrics);
    if (destAttr != INVALID_FILE_ATTRIBUTES) {
      DWORD deleteError = ERROR_SUCCESS;
      if (!DeleteMappedTargetPathMeasured(dest, destAttr, metrics, &deleteError)) {
        if (deleteError != ERROR_DIRECTORY) {
          if (errorMessage != nullptr) {
            *errorMessage =
                "覆盖目标文件失败。原因：" +
                vertex::shared::GetSystemErrorMessage(deleteError) +
                " (文件: " + vertex::shared::WideToUtf8(dest) + ")";
          }
          return false;
        }
      } else {
        summary->overwrittenFiles += 1;
      }
    }

    DWORD linkError = ERROR_SUCCESS;
    if (CreateHardLinkMeasured(dest, src, metrics, &linkError)) {
      summary->linkedFiles += 1;
      continue;
    }

    if (linkError == ERROR_NOT_SAME_DEVICE ||
        linkError == ERROR_ACCESS_DENIED ||
        linkError == ERROR_ALREADY_EXISTS) {
      DWORD copyError = ERROR_SUCCESS;
      if (CopyFileMeasured(src, dest, metrics, &copyError)) {
        summary->copiedFiles += 1;
        continue;
      }

      if (errorMessage != nullptr) {
        *errorMessage =
            "部署失败。原因：" + vertex::shared::GetSystemErrorMessage(copyError) +
            " (复制文件时: " + vertex::shared::WideToUtf8(src) + ")";
      }
      return false;
    }

    if (errorMessage != nullptr) {
      *errorMessage =
          "创建硬链接失败。原因：" + vertex::shared::GetSystemErrorMessage(linkError) +
          " (文件: " + vertex::shared::WideToUtf8(src) + ")";
    }
    return false;
  }

  return true;
}

bool RunUndeployRecursive(
    const std::wstring& srcRoot,
    const std::wstring& destRoot,
    UndeploySummary* summary,
    StagingMetrics* metrics) {
  std::vector<std::pair<std::wstring, std::wstring> > stack;
  stack.push_back(std::make_pair(srcRoot, destRoot));

  while (!stack.empty()) {
    const std::pair<std::wstring, std::wstring> current = stack.back();
    stack.pop_back();

    const std::wstring& src = current.first;
    const std::wstring& dest = current.second;

    const DWORD srcAttributes = GetFileAttributesMeasured(src, metrics);
    if (srcAttributes == INVALID_FILE_ATTRIBUTES) {
      continue;
    }

    if ((srcAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (srcAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
      summary->directories += 1;

      std::vector<std::wstring> entryNames;
      if (!ListDirectoryEntryNamesMeasured(src, &entryNames, metrics, nullptr)) {
        continue;
      }

      for (size_t index = 0; index < entryNames.size(); ++index) {
        const std::wstring& entryName = entryNames[index];
        stack.push_back(std::make_pair(
            JoinWindowsPath(src, entryName),
            JoinWindowsPath(dest, entryName)));
      }
      continue;
    }

    const DWORD destAttributes = GetFileAttributesMeasured(dest, metrics);
    if (destAttributes == INVALID_FILE_ATTRIBUTES) {
      continue;
    }

    DWORD deleteError = ERROR_SUCCESS;
    if (DeleteMappedTargetPathMeasured(dest, destAttributes, metrics, &deleteError)) {
      summary->removedFiles += 1;
    }
  }

  return true;
}

void RemoveEmptyDirectoriesRecursive(
    const std::wstring& directoryPath,
    RemoveEmptyDirsSummary* summary,
    StagingMetrics* metrics) {
  const DWORD attributes = GetFileAttributesMeasured(directoryPath, metrics);
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return;
  }

  summary->directories += 1;

  std::vector<std::wstring> entryNames;
  if (!ListDirectoryEntryNamesMeasured(directoryPath, &entryNames, metrics, nullptr)) {
    return;
  }

  for (size_t index = 0; index < entryNames.size(); ++index) {
    RemoveEmptyDirectoriesRecursive(
        JoinWindowsPath(directoryPath, entryNames[index]),
        summary,
        metrics);
  }

  bool empty = false;
  if (!IsDirectoryEmptyMeasured(directoryPath, &empty, metrics, nullptr) || !empty) {
    return;
  }

  if (RemoveDirectoryMeasured(directoryPath, metrics, nullptr)) {
    summary->removedDirectories += 1;
  }
}

RemoveEmptyDirsSummary RunRemoveEmptyDirectories(
    const std::wstring& directoryPath,
    StagingMetrics* metrics) {
  RemoveEmptyDirsSummary summary = CreateEmptyRemoveEmptyDirsSummary();
  RemoveEmptyDirectoriesRecursive(directoryPath, &summary, metrics);
  return summary;
}

Napi::Object BuildStagingMetricsObject(Napi::Env env, const StagingMetrics& metrics) {
  Napi::Object operations = Napi::Object::New(env);
  operations.Set("lstatMs", Napi::Number::New(env, metrics.operations.lstatMs));
  operations.Set("readdirMs", Napi::Number::New(env, metrics.operations.readdirMs));
  operations.Set("ensureDirMs", Napi::Number::New(env, metrics.operations.ensureDirMs));
  operations.Set("unlinkMs", Napi::Number::New(env, metrics.operations.unlinkMs));
  operations.Set("linkMs", Napi::Number::New(env, metrics.operations.linkMs));
  operations.Set("copyFileMs", Napi::Number::New(env, metrics.operations.copyFileMs));

  Napi::Object phases = Napi::Object::New(env);
  phases.Set("ensureTargetRootMs", Napi::Number::New(env, metrics.phases.ensureTargetRootMs));
  phases.Set("linkLoopMs", Napi::Number::New(env, metrics.phases.linkLoopMs));
  phases.Set("unlinkLoopMs", Napi::Number::New(env, metrics.phases.unlinkLoopMs));
  phases.Set("cleanupEmptyDirsMs", Napi::Number::New(env, metrics.phases.cleanupEmptyDirsMs));

  Napi::Object result = Napi::Object::New(env);
  result.Set("totalMs", Napi::Number::New(env, metrics.totalMs));
  result.Set("phases", phases);
  result.Set("operations", operations);
  return result;
}

Napi::Object BuildDeployResultObject(
    Napi::Env env,
    const DeploySummary& summary,
    const StagingMetrics* metrics) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("directories", Napi::Number::New(env, summary.directories));
  result.Set("linkedFiles", Napi::Number::New(env, summary.linkedFiles));
  result.Set("copiedFiles", Napi::Number::New(env, summary.copiedFiles));
  result.Set("overwrittenFiles", Napi::Number::New(env, summary.overwrittenFiles));
  if (metrics != nullptr) {
    result.Set("metrics", BuildStagingMetricsObject(env, *metrics));
  }
  return result;
}

Napi::Object BuildUndeployResultObject(
    Napi::Env env,
    const UndeploySummary& summary,
    const RemoveEmptyDirsSummary& cleanupSummary,
    const StagingMetrics* metrics,
    bool performed,
    const std::string& skippedReason) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("performed", Napi::Boolean::New(env, performed));
  SetStringOrNull(env, result, "skippedReason", skippedReason);
  result.Set("directories", Napi::Number::New(env, summary.directories));
  result.Set("removedFiles", Napi::Number::New(env, summary.removedFiles));
  result.Set("removedDirectories", Napi::Number::New(env, cleanupSummary.removedDirectories));
  result.Set("cleanupDirectories", Napi::Number::New(env, cleanupSummary.directories));
  if (metrics != nullptr) {
    result.Set("metrics", BuildStagingMetricsObject(env, *metrics));
  }
  return result;
}

Napi::Object BuildRemoveEmptyDirsResultObject(
    Napi::Env env,
    const RemoveEmptyDirsSummary& summary,
    const StagingMetrics* metrics) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("directories", Napi::Number::New(env, summary.directories));
  result.Set("removedDirectories", Napi::Number::New(env, summary.removedDirectories));
  if (metrics != nullptr) {
    result.Set("metrics", BuildStagingMetricsObject(env, *metrics));
  }
  return result;
}

Napi::Object BuildModUninstallPrepareResultObject(
    Napi::Env env,
    const ModUninstallPrepareResult& resultValue) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("filePaths", BuildWideStringArray(env, resultValue.filePaths));
  result.Set("dirPaths", BuildWideStringArray(env, resultValue.dirPaths));
  result.Set("occupiedFiles", BuildWideStringArray(env, resultValue.occupiedFiles));
  return result;
}

Napi::Object BuildModUninstallExecuteResultObject(
    Napi::Env env,
    const ModUninstallExecuteResult& resultValue) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("removedFiles", BuildWideStringArray(env, resultValue.removedFiles));
  result.Set("missingFiles", BuildWideStringArray(env, resultValue.missingFiles));
  result.Set("removedDirectories", BuildWideStringArray(env, resultValue.removedDirectories));
  return result;
}

Napi::Object BuildDriveScanResultObject(Napi::Env env, const DriveScanSummary& summary) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("found", Napi::Boolean::New(env, summary.found));
  result.Set("path", Napi::String::New(env, vertex::shared::WideToUtf8(summary.path)));
  result.Set("scannedDrives", Napi::Number::New(env, summary.scannedDrives));
  result.Set("scannedDirectories", Napi::Number::New(env, summary.scannedDirectories));
  return result;
}

Napi::Object BuildDetectGameRootResultObject(
    Napi::Env env,
    const DetectGameRootOutput& output) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("success", Napi::Boolean::New(env, output.success));
  SetStringOrNull(env, result, "reason", output.reason);
  result.Set("strategy", Napi::String::New(env, output.strategy));
  result.Set("domainName", Napi::String::New(env, output.domainName));
  result.Set("nexusGameId", Napi::Number::New(env, output.nexusGameId));

  if (!output.success) {
    return result;
  }

  result.Set("rootPath", Napi::String::New(env, vertex::shared::WideToUtf8(output.record.rootPath)));
  result.Set("exePath", Napi::String::New(env, vertex::shared::WideToUtf8(output.record.exePath)));
  result.Set("appId", Napi::Number::New(env, output.record.appId));
  result.Set("gameId", Napi::Number::New(env, output.record.gameId));
  result.Set("source", Napi::String::New(env, output.record.source));
  result.Set("candidateName", Napi::String::New(env, output.record.candidateName));
  result.Set("candidateType", Napi::String::New(env, output.record.candidateType));
  result.Set("confidence", Napi::Number::New(env, output.record.confidence));
  return result;
}

Napi::Object BuildExecutableScanResultObject(
    Napi::Env env,
    const ExecutableScanResult& resultValue) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("type", Napi::Number::New(env, resultValue.type));
  result.Set("path", Napi::String::New(env, vertex::shared::WideToUtf8(resultValue.path)));

  Napi::Array paths = Napi::Array::New(env, static_cast<uint32_t>(resultValue.paths.size()));
  for (size_t index = 0; index < resultValue.paths.size(); ++index) {
    paths[static_cast<uint32_t>(index)] =
        Napi::String::New(env, vertex::shared::WideToUtf8(resultValue.paths[index]));
  }
  result.Set("paths", paths);
  return result;
}

Napi::Array BuildPackageRootCandidatesArray(
    Napi::Env env,
    const std::vector<PackageRootCandidate>& candidates) {
  Napi::Array result = Napi::Array::New(env, static_cast<uint32_t>(candidates.size()));
  for (size_t index = 0; index < candidates.size(); ++index) {
    const PackageRootCandidate& candidate = candidates[index];
    Napi::Object item = Napi::Object::New(env);
    item.Set("rootPath", Napi::String::New(env, vertex::shared::WideToUtf8(candidate.rootPath)));
    item.Set("packageRoot", Napi::String::New(env, candidate.packageRoot));
    item.Set("depth", Napi::Number::New(env, candidate.depth));
    result[static_cast<uint32_t>(index)] = item;
  }
  return result;
}

class DeployWorker : public Napi::AsyncWorker {
 public:
  DeployWorker(Napi::Function& callback, std::wstring srcRoot, std::wstring destRoot)
      : Napi::AsyncWorker(callback), srcRoot_(std::move(srcRoot)), destRoot_(std::move(destRoot)),
        summary_(CreateEmptyDeploySummary()) {}

  ~DeployWorker() override = default;

  void Execute() override {
    std::string errorMessage;
    if (!RunDeployRecursive(srcRoot_, destRoot_, &summary_, nullptr, &errorMessage)) {
      SetError(errorMessage);
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object result = BuildDeployResultObject(Env(), summary_, nullptr);
    Callback().Call({Env().Null(), result});
  }

 private:
  std::wstring srcRoot_;
  std::wstring destRoot_;
  DeploySummary summary_;
};

class UndeployWorker : public Napi::AsyncWorker {
 public:
  UndeployWorker(Napi::Function& callback, std::wstring srcRoot, std::wstring destRoot)
      : Napi::AsyncWorker(callback), srcRoot_(std::move(srcRoot)), destRoot_(std::move(destRoot)),
        summary_(CreateEmptyUndeploySummary()) {}

  ~UndeployWorker() override = default;

  void Execute() override {
    RunUndeployRecursive(srcRoot_, destRoot_, &summary_, nullptr);
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object result =
        BuildUndeployResultObject(Env(), summary_, CreateEmptyRemoveEmptyDirsSummary(), nullptr, true, "");
    Callback().Call({Env().Null(), result});
  }

 private:
  std::wstring srcRoot_;
  std::wstring destRoot_;
  UndeploySummary summary_;
};

class RemoveEmptyDirsWorker : public Napi::AsyncWorker {
 public:
  RemoveEmptyDirsWorker(Napi::Function& callback, std::wstring directoryPath)
      : Napi::AsyncWorker(callback), directoryPath_(std::move(directoryPath)),
        summary_(CreateEmptyRemoveEmptyDirsSummary()) {}

  ~RemoveEmptyDirsWorker() override = default;

  void Execute() override {
    summary_ = RunRemoveEmptyDirectories(directoryPath_, nullptr);
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object result = BuildRemoveEmptyDirsResultObject(Env(), summary_, nullptr);
    Callback().Call({Env().Null(), result});
  }

 private:
  std::wstring directoryPath_;
  RemoveEmptyDirsSummary summary_;
};

class DeployStagingWorker : public Napi::AsyncWorker {
 public:
  DeployStagingWorker(Napi::Function& callback, std::wstring srcRoot, std::wstring destRoot)
      : Napi::AsyncWorker(callback), srcRoot_(std::move(srcRoot)), destRoot_(std::move(destRoot)),
        summary_(CreateEmptyDeploySummary()), metrics_(CreateEmptyStagingMetrics()) {}

  ~DeployStagingWorker() override = default;

  void Execute() override {
    const std::chrono::steady_clock::time_point totalStart = std::chrono::steady_clock::now();

    const DWORD srcAttributes = GetFileAttributesMeasured(srcRoot_, &metrics_);
    if (srcAttributes == INVALID_FILE_ATTRIBUTES ||
        (srcAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      SetError("Staging directory does not exist: " + vertex::shared::WideToUtf8(srcRoot_));
      return;
    }

    {
      AccumulateTimer timer(&metrics_.phases.ensureTargetRootMs);
      std::string directoryError;
      if (!EnsureDirectoryExistsMeasured(destRoot_, &metrics_, &directoryError)) {
        SetError(
            "创建目标目录失败: " + vertex::shared::WideToUtf8(destRoot_) +
            (directoryError.empty() ? "" : "，详情：" + directoryError));
        return;
      }
    }

    {
      AccumulateTimer timer(&metrics_.phases.linkLoopMs);
      std::string errorMessage;
      if (!RunDeployRecursive(srcRoot_, destRoot_, &summary_, &metrics_, &errorMessage)) {
        SetError(errorMessage);
        return;
      }
    }

    metrics_.totalMs = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - totalStart)
                           .count();
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object result = BuildDeployResultObject(Env(), summary_, &metrics_);
    Callback().Call({Env().Null(), result});
  }

 private:
  std::wstring srcRoot_;
  std::wstring destRoot_;
  DeploySummary summary_;
  StagingMetrics metrics_;
};

class UndeployStagingWorker : public Napi::AsyncWorker {
 public:
  UndeployStagingWorker(Napi::Function& callback, std::wstring srcRoot, std::wstring destRoot)
      : Napi::AsyncWorker(callback), srcRoot_(std::move(srcRoot)), destRoot_(std::move(destRoot)),
        summary_(CreateEmptyUndeploySummary()),
        cleanupSummary_(CreateEmptyRemoveEmptyDirsSummary()),
        metrics_(CreateEmptyStagingMetrics()) {}

  ~UndeployStagingWorker() override = default;

  void Execute() override {
    const std::chrono::steady_clock::time_point totalStart = std::chrono::steady_clock::now();

    const DWORD srcAttributes = GetFileAttributesMeasured(srcRoot_, &metrics_);
    if (srcAttributes == INVALID_FILE_ATTRIBUTES) {
      skippedReason_ = "staging_missing";
      metrics_.totalMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - totalStart)
                             .count();
      return;
    }

    const DWORD destAttributes = GetFileAttributesMeasured(destRoot_, &metrics_);
    if (destAttributes == INVALID_FILE_ATTRIBUTES) {
      skippedReason_ = "target_missing";
      metrics_.totalMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - totalStart)
                             .count();
      return;
    }

    {
      AccumulateTimer timer(&metrics_.phases.unlinkLoopMs);
      RunUndeployRecursive(srcRoot_, destRoot_, &summary_, &metrics_);
    }

    {
      AccumulateTimer timer(&metrics_.phases.cleanupEmptyDirsMs);
      cleanupSummary_ = RunRemoveEmptyDirectories(destRoot_, &metrics_);
    }

    metrics_.totalMs = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - totalStart)
                           .count();
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    const bool performed = skippedReason_.empty();
    Napi::Object result = BuildUndeployResultObject(
        Env(), summary_, cleanupSummary_, &metrics_, performed, skippedReason_);
    Callback().Call({Env().Null(), result});
  }

 private:
  std::wstring srcRoot_;
  std::wstring destRoot_;
  UndeploySummary summary_;
  RemoveEmptyDirsSummary cleanupSummary_;
  StagingMetrics metrics_;
  std::string skippedReason_;
};

class ScanDriveByFilenameWorker : public Napi::AsyncWorker {
 public:
  ScanDriveByFilenameWorker(Napi::Function& callback, std::wstring targetFileName)
      : Napi::AsyncWorker(callback),
        targetFileName_(std::move(targetFileName)),
        summary_(CreateEmptyDriveScanSummary()) {}

  ~ScanDriveByFilenameWorker() override = default;

  void Execute() override {
    summary_ = ScanDriveByFilenameInternal(targetFileName_);
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object result = BuildDriveScanResultObject(Env(), summary_);
    Callback().Call({Env().Null(), result});
  }

 private:
  std::wstring targetFileName_;
  DriveScanSummary summary_;
};

class DetectGameRootByRuleWorker : public Napi::AsyncWorker {
 public:
  DetectGameRootByRuleWorker(Napi::Function& callback, DetectGameRootInput input)
      : Napi::AsyncWorker(callback),
        input_(std::move(input)),
        output_(CreateEmptyDetectGameRootOutput()) {}

  ~DetectGameRootByRuleWorker() override = default;

  void Execute() override {
    output_ = DetectGameRootByRuleInternal(input_);
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object result = BuildDetectGameRootResultObject(Env(), output_);
    Callback().Call({Env().Null(), result});
  }

 private:
  DetectGameRootInput input_;
  DetectGameRootOutput output_;
};

class ScanExecutablesInDirectoryWorker : public Napi::AsyncWorker {
 public:
  ScanExecutablesInDirectoryWorker(Napi::Function& callback, std::wstring dirPath)
      : Napi::AsyncWorker(callback),
        dirPath_(std::move(dirPath)),
        result_(CreateEmptyExecutableScanResult()) {}

  ~ScanExecutablesInDirectoryWorker() override = default;

  void Execute() override {
    result_ = ScanExecutablesInDirectoryInternal(dirPath_);
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object result = BuildExecutableScanResultObject(Env(), result_);
    Callback().Call({Env().Null(), result});
  }

 private:
  std::wstring dirPath_;
  ExecutableScanResult result_;
};

class ListPackageRootCandidatesWorker : public Napi::AsyncWorker {
 public:
  ListPackageRootCandidatesWorker(
      Napi::Function& callback,
      std::wstring stagingPath,
      int32_t maxDepth)
      : Napi::AsyncWorker(callback),
        stagingPath_(std::move(stagingPath)),
        maxDepth_(maxDepth) {}

  ~ListPackageRootCandidatesWorker() override = default;

  void Execute() override {
    candidates_ = ListPackageRootCandidatesInternal(stagingPath_, maxDepth_);
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Array result = BuildPackageRootCandidatesArray(Env(), candidates_);
    Callback().Call({Env().Null(), result});
  }

 private:
  std::wstring stagingPath_;
  int32_t maxDepth_;
  std::vector<PackageRootCandidate> candidates_;
};

class InspectArchiveWorker : public Napi::AsyncWorker {
 public:
  InspectArchiveWorker(Napi::Function& callback, std::wstring archivePath, FileOptions options)
      : Napi::AsyncWorker(callback),
        archivePath_(std::move(archivePath)),
        options_(std::move(options)) {}

  ~InspectArchiveWorker() override = default;

  void Execute() override {
    parsedArchive_ = InspectArchiveInternal(archivePath_, options_);
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object result = BuildInspectResultObject(Env(), archivePath_, parsedArchive_);
    Callback().Call({Env().Null(), result});
  }

 private:
  std::wstring archivePath_;
  FileOptions options_;
  ParsedArchive parsedArchive_;
};

class ExtractArchiveWorker : public Napi::AsyncWorker {
 public:
  ExtractArchiveWorker(
      Napi::Function& callback,
      std::wstring archivePath,
      std::wstring outputDirectory,
      FileOptions options)
      : Napi::AsyncWorker(callback),
        archivePath_(std::move(archivePath)),
        outputDirectory_(std::move(outputDirectory)),
        options_(std::move(options)) {}

  ~ExtractArchiveWorker() override = default;

  void Execute() override {
    parsedArchive_ = ExtractArchiveInternal(archivePath_, outputDirectory_, options_);
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object response = Napi::Object::New(Env());
    response.Set("archivePath", Napi::String::New(Env(), vertex::shared::WideToUtf8(archivePath_)));
    SetStringOrNull(Env(), response, "archiveType", parsedArchive_.archiveType);
    response.Set("outputDir", Napi::String::New(Env(), vertex::shared::WideToUtf8(outputDirectory_)));
    response.Set("overwrite", Napi::Boolean::New(Env(), options_.overwrite));
    response.Set("totals", BuildTotalsObject(Env(), parsedArchive_.summary));
    Callback().Call({Env().Null(), response});
  }

 private:
  std::wstring archivePath_;
  std::wstring outputDirectory_;
  FileOptions options_;
  ParsedArchive parsedArchive_;
};

Napi::Value DeployMod(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsFunction()) {
    throw Napi::TypeError::New(env, "参数错误：需要 srcRoot (string), destRoot (string), callback (function)");
  }

  std::wstring srcRoot = EnsureAbsolutePath(vertex::shared::Utf8ToWide(info[0].As<Napi::String>().Utf8Value()));
  std::wstring destRoot = EnsureAbsolutePath(vertex::shared::Utf8ToWide(info[1].As<Napi::String>().Utf8Value()));
  Napi::Function callback = info[2].As<Napi::Function>();

  DeployWorker* worker = new DeployWorker(callback, srcRoot, destRoot);
  worker->Queue();

  return env.Undefined();
}

Napi::Value UndeployMod(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsFunction()) {
    throw Napi::TypeError::New(env, "参数错误：需要 srcRoot (string), destRoot (string), callback (function)");
  }

  std::wstring srcRoot =
      EnsureAbsolutePath(vertex::shared::Utf8ToWide(info[0].As<Napi::String>().Utf8Value()));
  std::wstring destRoot =
      EnsureAbsolutePath(vertex::shared::Utf8ToWide(info[1].As<Napi::String>().Utf8Value()));
  Napi::Function callback = info[2].As<Napi::Function>();

  UndeployWorker* worker = new UndeployWorker(callback, srcRoot, destRoot);
  worker->Queue();

  return env.Undefined();
}

Napi::Value RemoveEmptyDirs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsFunction()) {
    throw Napi::TypeError::New(env, "参数错误：需要 directoryPath (string), callback (function)");
  }

  std::wstring directoryPath =
      EnsureAbsolutePath(vertex::shared::Utf8ToWide(info[0].As<Napi::String>().Utf8Value()));
  Napi::Function callback = info[1].As<Napi::Function>();

  RemoveEmptyDirsWorker* worker = new RemoveEmptyDirsWorker(callback, directoryPath);
  worker->Queue();

  return env.Undefined();
}

Napi::Value DeployStaging(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsFunction()) {
    throw Napi::TypeError::New(
        env, "参数错误：需要 stagingRoot (string), targetRoot (string), callback (function)");
  }

  std::wstring srcRoot =
      EnsureAbsolutePath(vertex::shared::Utf8ToWide(info[0].As<Napi::String>().Utf8Value()));
  std::wstring destRoot =
      EnsureAbsolutePath(vertex::shared::Utf8ToWide(info[1].As<Napi::String>().Utf8Value()));
  Napi::Function callback = info[2].As<Napi::Function>();

  DeployStagingWorker* worker = new DeployStagingWorker(callback, srcRoot, destRoot);
  worker->Queue();

  return env.Undefined();
}

Napi::Value UndeployStaging(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsFunction()) {
    throw Napi::TypeError::New(
        env, "参数错误：需要 stagingRoot (string), targetRoot (string), callback (function)");
  }

  std::wstring srcRoot =
      EnsureAbsolutePath(vertex::shared::Utf8ToWide(info[0].As<Napi::String>().Utf8Value()));
  std::wstring destRoot =
      EnsureAbsolutePath(vertex::shared::Utf8ToWide(info[1].As<Napi::String>().Utf8Value()));
  Napi::Function callback = info[2].As<Napi::Function>();

  UndeployStagingWorker* worker = new UndeployStagingWorker(callback, srcRoot, destRoot);
  worker->Queue();

  return env.Undefined();
}

Napi::Value ScanDriveByFilename(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsFunction()) {
    throw Napi::TypeError::New(
        env, "参数错误：需要 targetFileName (string), callback (function)");
  }

  const std::wstring targetFileName =
      vertex::shared::Utf8ToWide(info[0].As<Napi::String>().Utf8Value());
  Napi::Function callback = info[1].As<Napi::Function>();

  ScanDriveByFilenameWorker* worker = new ScanDriveByFilenameWorker(callback, targetFileName);
  worker->Queue();

  return env.Undefined();
}

Napi::Value DetectGameRootByRule(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsObject() || !info[1].IsFunction()) {
    throw Napi::TypeError::New(
        env, "参数错误：需要 payload (object), callback (function)");
  }

  DetectGameRootInput input = ParseDetectGameRootInput(info[0].As<Napi::Object>());
  Napi::Function callback = info[1].As<Napi::Function>();

  DetectGameRootByRuleWorker* worker =
      new DetectGameRootByRuleWorker(callback, std::move(input));
  worker->Queue();

  return env.Undefined();
}

Napi::Value ScanExecutablesInDirectory(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsFunction()) {
    throw Napi::TypeError::New(
        env, "参数错误：需要 dirPath (string), callback (function)");
  }

  const std::wstring dirPath = ParseRequiredPathArgument(info, 0, "dirPath");
  Napi::Function callback = info[1].As<Napi::Function>();

  ScanExecutablesInDirectoryWorker* worker =
      new ScanExecutablesInDirectoryWorker(callback, dirPath);
  worker->Queue();

  return env.Undefined();
}

Napi::Value ListPackageRootCandidates(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsString() || !info[2].IsFunction()) {
    throw Napi::TypeError::New(
        env, "参数错误：需要 stagingPath (string), maxDepth (number), callback (function)");
  }

  const std::wstring stagingPath = ParseRequiredPathArgument(info, 0, "stagingPath");
  int32_t maxDepth = -1;
  if (info[1].IsNumber()) {
    maxDepth = ToSafeInt32(info[1].As<Napi::Number>().DoubleValue(), -1);
  }
  Napi::Function callback = info[2].As<Napi::Function>();

  ListPackageRootCandidatesWorker* worker =
      new ListPackageRootCandidatesWorker(callback, stagingPath, maxDepth);
  worker->Queue();

  return env.Undefined();
}

Napi::Value InspectArchiveAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsFunction()) {
    throw Napi::TypeError::New(
        env, "参数错误：需要 archivePath (string), callback (function)");
  }

  const std::wstring archivePath = ParseRequiredPathArgument(info, 0, "archivePath");
  const FileOptions options =
      info.Length() >= 3 ? ParseOptions(info[2], false) : FileOptions{false, L""};
  Napi::Function callback = info[1].As<Napi::Function>();

  InspectArchiveWorker* worker = new InspectArchiveWorker(callback, archivePath, options);
  worker->Queue();
  return env.Undefined();
}

Napi::Value ExtractArchiveAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsFunction()) {
    throw Napi::TypeError::New(
        env, "参数错误：需要 archivePath (string), outputDir (string), callback (function)");
  }

  const std::wstring archivePath = ParseRequiredPathArgument(info, 0, "archivePath");
  const std::wstring outputDirectory = ParseRequiredPathArgument(info, 1, "outputDir");
  const FileOptions options =
      info.Length() >= 4 ? ParseOptions(info[3], true) : FileOptions{false, L""};
  Napi::Function callback = info[2].As<Napi::Function>();

  ExtractArchiveWorker* worker =
      new ExtractArchiveWorker(callback, archivePath, outputDirectory, options);
  worker->Queue();
  return env.Undefined();
}

Napi::Value PrepareModUninstall(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1) {
    throw Napi::TypeError::New(env, "参数错误：需要 uninstallTree (array|object)");
  }

  try {
    const ModUninstallPrepareResult result = RunPrepareModUninstall(info[0]);
    return BuildModUninstallPrepareResultObject(env, result);
  } catch (const Napi::Error&) {
    throw;
  } catch (const std::exception& error) {
    throw Napi::Error::New(env, error.what());
  }
}

Napi::Value ExecuteModUninstall(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsArray()) {
    throw Napi::TypeError::New(
        env, "参数错误：需要 filePaths (array), dirPaths (array)");
  }

  try {
    const std::vector<std::wstring> filePaths =
        ParsePathArrayArgument(info[0], "filePaths");
    const std::vector<std::wstring> dirPaths =
        ParsePathArrayArgument(info[1], "dirPaths");
    const ModUninstallExecuteResult result =
        RunExecuteModUninstall(filePaths, dirPaths);
    return BuildModUninstallExecuteResultObject(env, result);
  } catch (const Napi::Error&) {
    throw;
  } catch (const std::exception& error) {
    throw Napi::Error::New(env, error.what());
  }
}


Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("inspectArchive", Napi::Function::New(env, InspectArchive));
  exports.Set("inspectArchiveAsync", Napi::Function::New(env, InspectArchiveAsync));
  exports.Set("parseArchiveStructure", Napi::Function::New(env, ParseArchiveStructure));
  exports.Set("extractArchive", Napi::Function::New(env, ExtractArchive));
  exports.Set("extractArchiveAsync", Napi::Function::New(env, ExtractArchiveAsync));
  exports.Set("generateFileMD5", Napi::Function::New(env, GenerateFileMD5));
  exports.Set("verifyFileMD5", Napi::Function::New(env, VerifyFileMD5));
  exports.Set("deployMod", Napi::Function::New(env, DeployMod));
  exports.Set("undeployMod", Napi::Function::New(env, UndeployMod));
  exports.Set("removeEmptyDirs", Napi::Function::New(env, RemoveEmptyDirs));
  exports.Set("deployStaging", Napi::Function::New(env, DeployStaging));
  exports.Set("undeployStaging", Napi::Function::New(env, UndeployStaging));
  exports.Set("prepareModUninstall", Napi::Function::New(env, PrepareModUninstall));
  exports.Set("executeModUninstall", Napi::Function::New(env, ExecuteModUninstall));
  exports.Set("scanDriveByFilename", Napi::Function::New(env, ScanDriveByFilename));
  exports.Set("detectGameRootByRule", Napi::Function::New(env, DetectGameRootByRule));
  exports.Set(
      "scanExecutablesInDirectory",
      Napi::Function::New(env, ScanExecutablesInDirectory));
  exports.Set(
      "listPackageRootCandidates",
      Napi::Function::New(env, ListPackageRootCandidates));
  return exports;
}

Napi::Value InspectArchive(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  try {
    const std::wstring archivePath = ParseRequiredPathArgument(info, 0, "archivePath");
    const FileOptions options =
        info.Length() >= 2 ? ParseOptions(info[1], false) : FileOptions{false, L""};
    const ParsedArchive parsedArchive = InspectArchiveInternal(archivePath, options);
    return BuildInspectResultObject(env, archivePath, parsedArchive);
  } catch (const Napi::Error&) {
    throw;
  } catch (const std::exception& error) {
    throw Napi::Error::New(env, error.what());
  }
}

Napi::Value ParseArchiveStructure(const Napi::CallbackInfo& info) {
  return InspectArchive(info);
}

Napi::Value ExtractArchive(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  try {
    const std::wstring archivePath = ParseRequiredPathArgument(info, 0, "archivePath");
    const std::wstring outputDirectory = ParseRequiredPathArgument(info, 1, "outputDir");
    const FileOptions options =
        info.Length() >= 3 ? ParseOptions(info[2], true) : FileOptions{false, L""};
    const ParsedArchive parsedArchive =
        ExtractArchiveInternal(archivePath, outputDirectory, options);

    Napi::Object response = Napi::Object::New(env);
    response.Set("archivePath", Napi::String::New(env, vertex::shared::WideToUtf8(archivePath)));
    SetStringOrNull(env, response, "archiveType", parsedArchive.archiveType);
    response.Set(
        "outputDir",
        Napi::String::New(env, vertex::shared::WideToUtf8(outputDirectory)));
    response.Set("overwrite", Napi::Boolean::New(env, options.overwrite));
    response.Set("totals", BuildTotalsObject(env, parsedArchive.summary));
    return response;
  } catch (const Napi::Error&) {
    throw;
  } catch (const std::exception& error) {
    throw Napi::Error::New(env, error.what());
  }
}

Napi::Value GenerateFileMD5(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  try {
    const std::wstring filePath = ParseRequiredPathArgument(info, 0, "filePath");
    const std::string digest = GenerateFileMD5Internal(filePath);
    return Napi::String::New(env, digest);
  } catch (const Napi::Error&) {
    throw;
  } catch (const std::exception& error) {
    throw Napi::Error::New(env, error.what());
  }
}

Napi::Value VerifyFileMD5(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  try {
    const std::wstring filePath = ParseRequiredPathArgument(info, 0, "filePath");
    if (info.Length() <= 1 || !info[1].IsString()) {
      throw Napi::TypeError::New(env, "expectedMD5 必须是字符串");
    }

    const std::string expectedMD5 =
        ToLowerAscii(TrimAsciiWhitespace(info[1].As<Napi::String>().Utf8Value()));
    const std::string actualMD5 = GenerateFileMD5Internal(filePath);
    return Napi::Boolean::New(env, actualMD5 == expectedMD5);
  } catch (const Napi::Error&) {
    throw;
  } catch (const std::exception& error) {
    throw Napi::Error::New(env, error.what());
  }
}

}  // namespace

NODE_API_MODULE(file_api, Init)
