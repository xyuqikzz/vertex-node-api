# 第三方组件

Vertex 自有源码采用 [GPL-3.0-only](./LICENSE)。以下组件保留原始许可，不能将本项目许可证用于替换它们的声明。

| 组件 | 用途与版本 | 许可及源码 |
| --- | --- | --- |
| node-addon-api | Node-API C++ 封装，锁定 8.6.0 | MIT；[原文](./licenses/node-addon-api-LICENSE.md)；[上游](https://github.com/nodejs/node-addon-api/tree/v8.6.0) |
| 7zip-bin-full | 获取 7-Zip 运行文件，锁定 26.0.1 | npm 包装代码为 MIT；[原文](./licenses/7zip-bin-full-LICENSE.txt)；[上游](https://github.com/ollm/7zip-bin-full) |
| 7-Zip | 子进程归档引擎，26.00；`7z.exe` 和 `7z.dll` | LGPL-2.1-or-later、部分 BSD 代码及 unRAR 限制；[原文](./licenses/7-Zip-26.00-LICENSE.txt)；[26.00 源码](https://github.com/ip7z/7zip/releases/download/26.00/7z2600-src.tar.xz) |

构建用 node-gyp 及其依赖以 `package-lock.json` 和安装包内许可为准；`node_modules/` 不随仓库分发。`npm run build` 将许可目录和项目 LICENSE 一并复制到 `dist/licenses/`，分发构建产物时保留这些文件。
