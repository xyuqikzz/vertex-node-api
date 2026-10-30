# Vertex Node API

Vertex 的 Windows x64 原生扩展，使用 C++、Node-API 和 `node-addon-api`，提供文件、进程及系统信息能力。

- 源码：[xyuqikzz/vertex-node-api](https://github.com/xyuqikzz/vertex-node-api)
- 客户端：[xyuqikzz/vertex-mods](https://github.com/xyuqikzz/vertex-mods)
- 自有代码许可证：**GPL-3.0-only**，见 [LICENSE](./LICENSE)。Copyright (c) 2026 YuQiKzz。
- 第三方依赖保留各自许可，见 [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)。

## 源码范围与客户端兼容性

本次公开的是维护者指定工作区的源码，原始基线为 `5565d6e77c252c86e76bd319426375e5e78f6757`。包含 C++ 实现、Node.js 封装、构建配置和锁定依赖；不提交本机依赖、编译缓存或预编译产物。

**当前源码不能直接替换客户端已附带的原生模块。** 2026-10-30 核对发现，客户端 `file_api_v3.node` 还导出本源码尚未实现的 `generateFileSHA256`、`generateFileSHA256Async`、`buildManifest`、`copyPath`、`deployFile`、`removePath`、`protectModSource`、`restoreModSource`、`validateModSourceReadable` 和 `repairModSourceAccess`；客户端 `process_api.node` 另有 `launchElevated`。

本仓库的构建目标为 `file_api`、`process_api`、`system_api`，不会生成 `file_api_v3` 等版本别名。不要通过改文件名绕过客户端版本检查。源码公开、编译成功、接口兼容和逐字节复现是不同结论；当前未证明与客户端现有二进制对应。

## 构建

需要 Windows x64、Node.js、Python 3，以及包含“使用 C++ 的桌面开发”和 Windows SDK 的 Visual Studio 2022 Build Tools。依赖版本以 `package-lock.json` 为准；不要用全局安装的 node-gyp 替代项目版本。

```powershell
git clone https://github.com/xyuqikzz/vertex-node-api.git
cd vertex-node-api
npm ci --ignore-scripts
npm run build
```

`--ignore-scripts` 避免安装阶段隐式构建，随后由显式构建命令完成编译与产物整理。首次构建可能下载 Node.js 头文件；交叉架构或 Electron 加载兼容性需要单独验证。

| 目录 | 内容 |
| --- | --- |
| `src/domains/file/addon.cpp` | 压缩包检查、解压、MD5、部署及卸载、游戏目录识别 |
| `src/domains/process/addon.cpp` | 普通程序与 Shell 目标启动、窗口与进程事件 |
| `src/domains/system/addon.cpp` | 硬件型号、系统信息、温度查询 |
| `src/shared/` | Windows 进程、字符串等公共实现 |
| `index.js` | 功能域加载与历史接口兼容封装 |
| `binding.gyp` | 三个原生目标的编译配置 |
| `scripts/package-build.js` | 构建后整理模块、7-Zip 运行时与许可文件 |

构建会重建本仓库的 `build/` 和 `dist/`，不会复制文件到客户端或游戏目录。输出为：

```text
dist/
├─ file/
│  ├─ file_api.node
│  ├─ 7z.exe
│  └─ 7z.dll
├─ process/process_api.node
├─ system/system_api.node
└─ licenses/
```

`dist/licenses/` 随产物包含本项目及直接集成组件的许可文本。7-Zip 通过子进程调用；其源码和许可不属于 Vertex 自有代码。

## 调用与验证

2026-10-30 发布前核对：锁定依赖安装、JavaScript 语法与许可原文检查通过。本机为 Node.js 26.7.0、Python 3.13.2、Visual Studio 2026；锁定的 node-gyp 11.5.0 未识别 VS 2026，构建在工具链发现阶段失败，未完成编译及运行验收。上述构建步骤要求 VS 2022，不应把本次结果描述为构建通过。

```javascript
const addon = require('./index')

// 只加载接口，不启动进程或操作用户文件。
console.log(Object.keys(addon.file))
console.log(Object.keys(addon.process))
console.log(Object.keys(addon.system))
```

`index.js` 优先加载 `dist/<domain>/`，找不到时回退到 `build/Release/`。文件域的同步和异步接口应按实际导出使用；进程域提供 `launchExe`、`launchTarget`，系统域提供 `getHardwareModel`、`getSystemDetails`、`getTemperatureSnapshot`。

已有的 `npm test` 是本机诊断示例，会读取并输出硬件与温度信息，**不是完整回归测试**。不要公开其中的设备信息，也不要把它通过当作所有文件、部署和进程路径均已验证。新增临时验证放入忽略目录 `.scratch/`，不在 `scripts/` 堆积测试脚本。

与客户端集成前，应核对导出函数、运行平台、Node-API 加载能力和文件摘要，在隔离目录验证受影响操作。当前版本存在上述接口差异，因此应继续使用客户端已附带的原生文件。客户端集成说明见 [NATIVE_BUILD.md](https://github.com/xyuqikzz/vertex-mods/blob/main/NATIVE_BUILD.md)。

## English

Windows x64 Node-API native modules for Vertex. Build with `npm ci --ignore-scripts` followed by `npm run build`, using Python 3 and Visual Studio 2022 C++ Build Tools. First-party source is licensed under GPL-3.0-only; third-party components retain their own licenses.

This published source snapshot does not yet implement all exports found in the client's bundled native binaries, including SHA-256 operations, file-protection helpers and elevated process launch. It is not a verified replacement or reproducible source baseline for those binaries. Keep the client's bundled files until source and binary compatibility has been established.
