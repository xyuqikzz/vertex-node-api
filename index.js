const fs = require("fs");
const path = require("path");

function loadAddon(domain, targetName) {
  const packagedPath = path.join(__dirname, "dist", domain, `${targetName}.node`);
  const buildPath = path.join(__dirname, "build", "Release", `${targetName}.node`);
  const candidates = [packagedPath, buildPath];
  const loadErrors = [];

  for (const candidate of candidates) {
    if (!fs.existsSync(candidate)) {
      continue;
    }

    try {
      return require(candidate);
    } catch (error) {
      loadErrors.push(`${candidate} -> ${error.message}`);
    }
  }

  const errorMessage =
    loadErrors.length > 0
      ? `无法加载原生模块 ${targetName}。错误详情：${loadErrors.join("；")}`
      : `无法加载原生模块 ${targetName}。未找到候选文件：${candidates.join("、")}`;
  throw new Error(errorMessage);
}

const fileDomain = loadAddon("file", "file_api");
const processDomain = loadAddon("process", "process_api");
const systemDomain = loadAddon("system", "system_api");

function emitProgressEvent(event, channel, payload) {
  if (!event || !channel) {
    return;
  }

  try {
    const sender = event.sender || event;
    if (sender && typeof sender.send === "function") {
      sender.send(channel, payload);
      return;
    }

    if (typeof event.reply === "function") {
      event.reply(channel, payload);
    }
  } catch {
    // 忽略事件发送失败，避免影响主流程。
  }
}

function pickWindowsPathFromText(text = "") {
  const matchList = String(text || "").match(/[A-Za-z]:\\[^\r\n"'<>|?*]+/g);
  if (!matchList || !matchList.length) {
    return "";
  }
  return String(matchList[matchList.length - 1] || "").trim();
}

function formatExtractErrorMessage(error) {
  const message = String(error?.message || "");
  const stderr = String(error?.stderr || "");
  const fullText = `${message}\n${stderr}`;

  if (/Cannot delete output file/i.test(fullText)) {
    const lockedPath = pickWindowsPathFromText(fullText);
    if (lockedPath) {
      return `解压失败：无法覆盖文件 ${lockedPath}，该文件可能被占用。请先关闭游戏和相关进程后重试。`;
    }
    return "解压失败：目标文件正在被占用，无法覆盖。请先关闭游戏和相关进程后重试。";
  }

  if (/Access is denied|EACCES|EPERM/i.test(fullText)) {
    const deniedPath = pickWindowsPathFromText(fullText);
    if (deniedPath) {
      return `解压失败：没有权限写入 ${deniedPath}，请以管理员权限运行或检查目录权限。`;
    }
    return "解压失败：没有权限写入目标目录，请以管理员权限运行或检查目录权限。";
  }

  if (/wrong password|password is incorrect|can not open encrypted archive/i.test(fullText)) {
    return "压缩包已加密，需要正确密码";
  }

  return `解压失败: ${message || "未知错误"}`;
}

function normalizeLegacyPath(targetPath = "") {
  return String(targetPath || "").replace(/[\\/]+/g, "\\").replace(/\\$/, "");
}

function createLegacyNode(node, parentPath) {
  const currentPath = normalizeLegacyPath(path.join(parentPath, String(node?.name || "")));
  const isDirectory = String(node?.type || "") === "directory";
  const childList = Array.isArray(node?.children)
    ? node.children.map((child) => createLegacyNode(child, currentPath))
    : [];

  return {
    path: currentPath,
    type: isDirectory ? 0 : 1,
    name: String(node?.name || ""),
    sub: isDirectory ? childList : undefined,
  };
}

function buildLegacyStructure(tree = [], rootPath = "") {
  const normalizedRootPath = normalizeLegacyPath(path.resolve(String(rootPath || "")));
  const rootNode = {
    path: normalizedRootPath,
    type: 0,
    name: path.basename(normalizedRootPath),
    sub: Array.isArray(tree) ? tree.map((node) => createLegacyNode(node, normalizedRootPath)) : [],
  };
  return [rootNode];
}

function resolveExecutableRun(structure = []) {
  const exePaths = [];

  function walk(nodes) {
    nodes.forEach((item) => {
      if (Number(item?.type) === 1 && String(item?.name || "").toLowerCase().endsWith(".exe")) {
        exePaths.push(String(item.path || ""));
      }
      if (Array.isArray(item?.sub) && item.sub.length > 0) {
        walk(item.sub);
      }
    });
  }

  walk(Array.isArray(structure) ? structure : []);

  if (exePaths.length === 0) {
    return { type: 0, path: "", paths: [] };
  }

  if (exePaths.length === 1) {
    return { type: 1, path: exePaths[0], paths: [] };
  }

  return { type: 2, path: "", paths: exePaths };
}

function isEventLike(value) {
  return !!(
    value &&
    typeof value === "object" &&
    ((value.sender && typeof value.sender.send === "function") || typeof value.reply === "function")
  );
}

function resolveFileOptionsAndEvent(thirdArg, fourthArg) {
  if (isEventLike(thirdArg)) {
    return {
      options: {},
      event: thirdArg,
    };
  }

  return {
    options: thirdArg && typeof thirdArg === "object" && !Array.isArray(thirdArg) ? thirdArg : {},
    event: isEventLike(fourthArg) ? fourthArg : null,
  };
}

function isArchiveEncrypted(inspectResult) {
  return Array.isArray(inspectResult?.entries)
    ? inspectResult.entries.some((entry) => entry?.encrypted === true)
    : false;
}

function buildEncryptedArchiveMessage(archiveType = "") {
  const normalizedType = String(archiveType || "").trim().toLowerCase();
  if (normalizedType === "zip") {
    return "ZIP文件已加密，需要密码";
  }
  if (normalizedType === "rar" || normalizedType === "rar5") {
    return "RAR文件已加密，需要密码";
  }
  if (normalizedType === "7z") {
    return "7Z文件已加密，需要密码";
  }
  return "压缩包已加密，需要密码";
}

function normalizeFileOptions(options = {}) {
  const target = options && typeof options === "object" && !Array.isArray(options) ? options : {};
  const password = String(target.password || "").trim();
  return {
    overwrite: target.overwrite === true,
    password,
  };
}

function parseAndExtractFile(filePath, extractPath, thirdArg, fourthArg) {
  const safeFilePath = path.resolve(String(filePath || ""));
  const safeExtractPath = path.resolve(String(extractPath || ""));
  const { options: rawOptions, event } = resolveFileOptionsAndEvent(thirdArg, fourthArg);
  const options = normalizeFileOptions(rawOptions);

  if (!fs.existsSync(safeFilePath)) {
    throw new Error(`源文件不存在: ${safeFilePath}`);
  }

  fs.mkdirSync(safeExtractPath, { recursive: true });

  let inspectResult = null;
  try {
    inspectResult = options.password
      ? fileDomain.inspectArchive(safeFilePath, { password: options.password })
      : fileDomain.inspectArchive(safeFilePath);
  } catch (error) {
    return {
      type: 0,
      msg: formatExtractErrorMessage(error),
      data: {
        structure: [],
        isEncrypted: false,
        backup: [],
      },
    };
  }

  const structure = buildLegacyStructure(inspectResult?.tree || [], safeExtractPath);
  const encrypted = isArchiveEncrypted(inspectResult);

  if (encrypted && !options.password) {
    return {
      type: 0,
      msg: buildEncryptedArchiveMessage(inspectResult?.archiveType),
      data: {
        structure,
        isEncrypted: true,
        backup: [],
      },
    };
  }

  try {
    const nativeOptions = {
      overwrite: true,
    };
    if (options.password) {
      nativeOptions.password = options.password;
    }

    fileDomain.extractArchive(safeFilePath, safeExtractPath, nativeOptions);
    emitProgressEvent(event, "install-task-extract-progress", { percent: 100 });

    return {
      type: 1,
      msg: "解压成功",
      data: {
        structure,
        isEncrypted: encrypted,
        backup: [],
      },
    };
  } catch (error) {
    return {
      type: 0,
      msg: formatExtractErrorMessage(error),
      data: {
        structure,
        isEncrypted: encrypted,
        backup: [],
      },
    };
  }
}

function extractAndGetStructure(filePath, extractPath, usageType = 1, thirdArg, fourthArg) {
  const result = parseAndExtractFile(filePath, extractPath, thirdArg, fourthArg);

  if (Number(result?.type) === 1 && (Number(usageType) === 2 || Number(usageType) === 4)) {
    result.data.run = resolveExecutableRun(result?.data?.structure || []);
  }

  return result;
}

function generateFileMD5(filePath, onProgress) {
  const safeFilePath = path.resolve(String(filePath || ""));
  const digest = String(fileDomain.generateFileMD5(safeFilePath) || "").toLowerCase();

  if (typeof onProgress === "function") {
    try {
      const stats = fs.statSync(safeFilePath);
      const totalSize = Number(stats?.size || 0);
      onProgress(100, totalSize, totalSize);
    } catch {
      onProgress(100, 0, 0);
    }
  }

  return digest;
}

function verifyFileMD5(filePath, expectedMD5, onProgress) {
  const safeFilePath = path.resolve(String(filePath || ""));
  const normalizedExpectedMD5 = String(expectedMD5 || "").trim().toLowerCase();
  const matched = fileDomain.verifyFileMD5(safeFilePath, normalizedExpectedMD5);

  if (typeof onProgress === "function") {
    try {
      const stats = fs.statSync(safeFilePath);
      const totalSize = Number(stats?.size || 0);
      onProgress(100, totalSize, totalSize);
    } catch {
      onProgress(100, 0, 0);
    }
  }

  return matched;
}

module.exports = {
  file: fileDomain,
  process: processDomain,
  system: systemDomain,
  inspectArchive: fileDomain.inspectArchive,
  parseArchiveStructure: fileDomain.parseArchiveStructure,
  extractArchive: fileDomain.extractArchive,
  parseAndExtractFile,
  extractAndGetStructure,
  generateFileMD5,
  verifyFileMD5,
  launchExe: processDomain.launchExe,
  launchTarget: processDomain.launchTarget,
  getHardwareModel: systemDomain.getHardwareModel,
};
