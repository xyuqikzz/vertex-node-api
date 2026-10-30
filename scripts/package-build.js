const fs = require("fs");
const path = require("path");
const { path7z } = require("7zip-bin-full");

const rootDir = path.resolve(__dirname, "..");
const buildDir = path.join(rootDir, "build", "Release");
const distDir = path.join(rootDir, "dist");

const moduleTargets = [
  { domain: "file", targetName: "file_api" },
  { domain: "process", targetName: "process_api" },
  { domain: "system", targetName: "system_api" },
];

function ensureFileExists(filePath) {
  if (!fs.existsSync(filePath)) {
    throw new Error(`未找到构建产物：${filePath}`);
  }
}

function packageTarget({ domain, targetName }) {
  const sourcePath = path.join(buildDir, `${targetName}.node`);
  const targetDir = path.join(distDir, domain);
  const targetPath = path.join(targetDir, `${targetName}.node`);

  ensureFileExists(sourcePath);
  fs.mkdirSync(targetDir, { recursive: true });
  fs.copyFileSync(sourcePath, targetPath);
  console.log(`已整理产物：${targetPath}`);
}

function packageFileRuntime() {
  const runtimeDir = path.dirname(path7z);
  const targetDir = path.join(distDir, "file");
  ["7z.exe", "7z.dll"].forEach((fileName) => {
    const sourcePath = path.join(runtimeDir, fileName);
    const targetPath = path.join(targetDir, fileName);
    ensureFileExists(sourcePath);
    fs.mkdirSync(targetDir, { recursive: true });
    fs.copyFileSync(sourcePath, targetPath);
    console.log(`已整理运行时：${targetPath}`);
  });
}

fs.rmSync(distDir, { recursive: true, force: true });
moduleTargets.forEach(packageTarget);
packageFileRuntime();

// 随原生产物保留项目和直接集成组件的许可原文。
const licenseDir = path.join(distDir, "licenses");
fs.mkdirSync(licenseDir, { recursive: true });
fs.copyFileSync(path.join(rootDir, "LICENSE"), path.join(licenseDir, "Vertex-GPL-3.0.txt"));
for (const name of fs.readdirSync(path.join(rootDir, "licenses"))) {
  fs.copyFileSync(path.join(rootDir, "licenses", name), path.join(licenseDir, name));
}
