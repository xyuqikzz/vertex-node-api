const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { spawnSync } = require("child_process");
const { path7z } = require("7zip-bin-full");

const addon = require("./index");

function createSampleZipArchive() {
  const baseDir = path.join(os.tmpdir(), "vertex-napi-file-test");
  const inputDir = path.join(baseDir, "input");
  const nestedDir = path.join(inputDir, "nested");
  const archivePath = path.join(baseDir, "sample.zip");

  fs.rmSync(baseDir, { recursive: true, force: true });
  fs.mkdirSync(nestedDir, { recursive: true });
  fs.writeFileSync(path.join(inputDir, "root.txt"), "root\n");
  fs.writeFileSync(path.join(nestedDir, "child.txt"), "child\n");

  const archiveResult = spawnSync(
    path7z,
    ["a", "-tzip", archivePath, path.join(inputDir, "*")],
    {
      shell: true,
      encoding: "utf8",
      windowsHide: true,
    }
  );

  if (archiveResult.status !== 0) {
    throw new Error(
      `测试压缩包创建失败：${archiveResult.stderr || archiveResult.stdout}`
    );
  }

  return {
    archivePath,
    extractDir: path.join(baseDir, "output"),
  };
}

function callAddonAsync(method, ...args) {
  return new Promise((resolve, reject) => {
    method(...args, (error, result) => {
      if (error) {
        reject(error);
        return;
      }
      resolve(result);
    });
  });
}

function createStagingCase() {
  const baseDir = path.join(os.tmpdir(), "vertex-napi-staging-test");
  const stagingDir = path.join(baseDir, "staging");
  const targetDir = path.join(baseDir, "target");
  const nestedDir = path.join(stagingDir, "nested");

  fs.rmSync(baseDir, { recursive: true, force: true });
  fs.mkdirSync(nestedDir, { recursive: true });
  fs.mkdirSync(targetDir, { recursive: true });
  fs.writeFileSync(path.join(stagingDir, "root.txt"), "root-file\n");
  fs.writeFileSync(path.join(nestedDir, "child.txt"), "child-file\n");

  return {
    baseDir,
    stagingDir,
    targetDir,
  };
}

async function main() {
  console.log("可用功能域 =", Object.keys(addon).filter((key) => ["file", "process", "system"].includes(key)));
  console.log("process.launchExe 类型 =", typeof addon.process.launchExe);
  console.log("process.launchTarget 类型 =", typeof addon.process.launchTarget);
  console.log("system.getHardwareModel 类型 =", typeof addon.system.getHardwareModel);
  console.log("file.inspectArchive 类型 =", typeof addon.file.inspectArchive);
  console.log("file.extractArchive 类型 =", typeof addon.file.extractArchive);
  console.log("addon.parseAndExtractFile 类型 =", typeof addon.parseAndExtractFile);
  console.log("addon.generateFileMD5 类型 =", typeof addon.generateFileMD5);

  // assert.strictEqual(typeof addon.file.inspectArchive, "function");
  assert.strictEqual(typeof addon.system.getHardwareModel, "function");
  assert.strictEqual(typeof addon.system.getSystemDetails, "function");
  
  const hwModel = addon.system.getHardwareModel();
  console.log("旧版硬件设备信息 =", hwModel);

  const sysDetails = addon.system.getSystemDetails();
  console.log("----------------------------------------");
  console.log("详细系统信息 =", JSON.stringify(sysDetails, null, 2));
  console.log("----------------------------------------");
  
  // assert.ok(hwModel.cpu);
  // assert.ok(hwModel.systemModel);
  // assert.ok(hwModel.memory);
  // assert.strictEqual(typeof addon.file.extractArchive, "function");
  // assert.strictEqual(typeof addon.parseAndExtractFile, "function");
  // assert.strictEqual(typeof addon.generateFileMD5, "function");

  // const { archivePath, extractDir } = createSampleZipArchive();
  // const inspectResult = await addon.file.inspectArchive(archivePath);

  // assert.strictEqual(inspectResult.archiveType, "zip");
  // assert.strictEqual(inspectResult.totals.fileCount, 2);
  // assert.strictEqual(inspectResult.totals.directoryCount, 1);
  // assert.strictEqual(inspectResult.totals.totalSize, 11);
  // assert.ok(inspectResult.entries.some((entry) => entry.path === "root.txt"));
  // assert.ok(inspectResult.entries.some((entry) => entry.path === "nested/child.txt"));
  // assert.ok(inspectResult.tree.some((node) => node.path === "nested"));
  // assert.strictEqual(addon.generateFileMD5(path.join(path.dirname(archivePath), "input", "root.txt")).length, 32);

  // const extractResult = await addon.file.extractArchive(archivePath, extractDir);
  // assert.strictEqual(extractResult.outputDir, path.resolve(extractDir));
  // assert.strictEqual(
  //   fs.readFileSync(path.join(extractDir, "root.txt"), "utf8"),
  //   "root\n"
  // );
  // assert.strictEqual(
  //   fs.readFileSync(path.join(extractDir, "nested", "child.txt"), "utf8"),
  //   "child\n"
  // );

  // const legacyResult = addon.parseAndExtractFile(archivePath, path.join(path.dirname(archivePath), "legacy-output"));
  // assert.strictEqual(legacyResult.type, 1);
  // assert.ok(Array.isArray(legacyResult.data?.structure));
  // assert.strictEqual(legacyResult.data?.isEncrypted, false);

  // const fileMd5 = addon.generateFileMD5(path.join(path.dirname(archivePath), "input", "root.txt"));
  // assert.strictEqual(addon.verifyFileMD5(path.join(path.dirname(archivePath), "input", "root.txt"), fileMd5), true);

  // console.log("压缩包解析结果 =", {
  //   archiveType: inspectResult.archiveType,
  //   totals: inspectResult.totals,
  // });
  // console.log("压缩包解压结果 =", extractResult);
  // console.log("兼容解压结果 =", legacyResult);
  // console.log("文件 MD5 =", fileMd5);

  assert.strictEqual(typeof addon.file.deployMod, "function");
  assert.strictEqual(typeof addon.file.undeployMod, "function");
  assert.strictEqual(typeof addon.file.removeEmptyDirs, "function");
  assert.strictEqual(typeof addon.file.deployStaging, "function");
  assert.strictEqual(typeof addon.file.undeployStaging, "function");
  assert.strictEqual(typeof addon.file.prepareModUninstall, "function");
  assert.strictEqual(typeof addon.file.executeModUninstall, "function");

  const stagingCase = createStagingCase();
  const deployResult = await callAddonAsync(
    addon.file.deployMod,
    stagingCase.stagingDir,
    stagingCase.targetDir
  );
  assert.strictEqual(deployResult.linkedFiles + deployResult.copiedFiles, 2);
  assert.strictEqual(fs.existsSync(path.join(stagingCase.targetDir, "root.txt")), true);
  assert.strictEqual(
    fs.existsSync(path.join(stagingCase.targetDir, "nested", "child.txt")),
    true
  );

  const undeployResult = await callAddonAsync(
    addon.file.undeployMod,
    stagingCase.stagingDir,
    stagingCase.targetDir
  );
  assert.strictEqual(undeployResult.removedFiles, 2);

  const cleanupResult = await callAddonAsync(addon.file.removeEmptyDirs, stagingCase.targetDir);
  assert.strictEqual(cleanupResult.removedDirectories >= 1, true);
  assert.strictEqual(fs.existsSync(stagingCase.targetDir), false);

  console.log("暂存部署测试 =", {
    deployResult,
    undeployResult,
    cleanupResult,
  });

  const stagingCaseForHighLevel = createStagingCase();
  const stagingDeployResult = await callAddonAsync(
    addon.file.deployStaging,
    stagingCaseForHighLevel.stagingDir,
    stagingCaseForHighLevel.targetDir
  );
  assert.strictEqual(stagingDeployResult.linkedFiles + stagingDeployResult.copiedFiles, 2);
  assert.strictEqual(typeof stagingDeployResult.metrics?.totalMs, "number");

  const stagingUndeployResult = await callAddonAsync(
    addon.file.undeployStaging,
    stagingCaseForHighLevel.stagingDir,
    stagingCaseForHighLevel.targetDir
  );
  assert.strictEqual(stagingUndeployResult.performed, true);
  assert.strictEqual(stagingUndeployResult.removedFiles, 2);
  assert.strictEqual(typeof stagingUndeployResult.metrics?.totalMs, "number");
  assert.strictEqual(fs.existsSync(stagingCaseForHighLevel.targetDir), false);

  const missingUndeployResult = await callAddonAsync(
    addon.file.undeployStaging,
    path.join(stagingCaseForHighLevel.baseDir, "missing-staging"),
    stagingCaseForHighLevel.targetDir
  );
  assert.strictEqual(missingUndeployResult.performed, false);
  assert.strictEqual(missingUndeployResult.skippedReason, "staging_missing");

  console.log("暂存高层接口测试 =", {
    stagingDeployResult,
    stagingUndeployResult,
    missingUndeployResult,
  });

  const nativeUninstallCase = createStagingCase();
  fs.writeFileSync(path.join(nativeUninstallCase.targetDir, "root.txt"), "root-file\n");
  fs.mkdirSync(path.join(nativeUninstallCase.targetDir, "nested"), { recursive: true });
  fs.writeFileSync(path.join(nativeUninstallCase.targetDir, "nested", "child.txt"), "child-file\n");

  const uninstallTree = [
    {
      type: 0,
      sub: [
        {
          type: 1,
          path: path.join(nativeUninstallCase.targetDir, "root.txt"),
        },
        {
          type: 0,
          sub: [
            {
              type: 1,
              path: path.join(nativeUninstallCase.targetDir, "nested", "child.txt"),
            },
          ],
        },
      ],
    },
  ];

  const prepareUninstallResult = addon.file.prepareModUninstall(uninstallTree);
  assert.strictEqual(prepareUninstallResult.filePaths.length, 2);
  assert.strictEqual(prepareUninstallResult.occupiedFiles.length, 0);

  const executeUninstallResult = addon.file.executeModUninstall(
    prepareUninstallResult.filePaths,
    prepareUninstallResult.dirPaths
  );
  assert.strictEqual(executeUninstallResult.removedFiles.length, 2);
  assert.strictEqual(fs.existsSync(path.join(nativeUninstallCase.targetDir, "root.txt")), false);
  assert.strictEqual(
    fs.existsSync(path.join(nativeUninstallCase.targetDir, "nested", "child.txt")),
    false
  );
  assert.strictEqual(fs.existsSync(nativeUninstallCase.targetDir), false);

  console.log("原生卸载计划测试 =", {
    prepareUninstallResult,
    executeUninstallResult,
  });
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
