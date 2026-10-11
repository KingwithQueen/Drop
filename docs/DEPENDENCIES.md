# 固定依赖与构建

日期：2026-10-10。固定 vcpkg baseline：`e456309491fd875487bf02b8a0dca76c1b1b00cc`，工具版本 `2026-09-26-51bf87ca6e9bf3e622d84ff323bd202ab1ca0c0b`。清单为根目录 `vcpkg.json`。各 port 在此提交固定源码标签、SHA-512 和补丁；不在每次构建获取最新版。

| 依赖 | 固定版本 | 许可证 | 用途与 C 接口 |
| --- | --- | --- | --- |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | MIT | 公共 JSON；拒绝超限、重复键、无效 UTF-8、尾随数据及 NUL。 |
| [CivetWeb](https://github.com/civetweb/civetweb) | 1.16 port 2 | MIT | 服务器 C API；SSL、WebSocket 编译接入，所有 HTTP 路径由应用接管。未接入 C++ wrapper。 |
| [OpenSSL](https://github.com/openssl/openssl) | 3.6.5 port 1 | Apache-2.0 | CivetWeb TLS 及 SQLCipher 加密后端；测试证书工具。 |
| [SQLCipher Community](https://github.com/sqlcipher/sqlcipher/tree/v4.6.1) | 4.6.1 port 3 | BSD-3-Clause，底层 SQLite 公有领域 | 服务器专用；`include/sqlcipher/sqlite3.h` 与 `sqlcipher.lib`。 |
| [SQLite](https://sqlite.org/) | 3.53.4 port 2 | Public domain | 客户端专用；不与 SQLCipher 在同一程序链接，以免同名符号混用。 |
| [libsodium](https://github.com/jedisct1/libsodium) | 1.0.22 port 1 | ISC | 随机 ID、SHA-256、Argon2id、XChaCha20-Poly1305。固定 RELEASE 标签及哈希。 |
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 | BSD-3-Clause（采用此许可）或 GPL-2.0 | 正文存储压缩；不对附件重复压缩。 |
| [csrp SRP-6a](https://github.com/cocagne/csrp/tree/rfc5054_compat) | b1a6ebc3331672694149469de6f33ce3c69e1fe9 | MIT | vendor/csrp 中原版源码；OpenSSL C 包装、固定 4096 位/SHA-256、密码认证。源码 SHA-256 及包装边界见 vendor/csrp/PROVENANCE.md。 |
| WinHTTP / DPAPI | Windows SDK | Microsoft Windows SDK 条款 | 系统 C API；客户端 TLS 验证与用户范围秘密保护。 |

传递依赖包括 zlib 1.3.2 port 2、Tcl 9.0.3 port 2（SQLCipher amalgamation 生成）、libtommath、minizip、pkgconf 和 vcpkg 构建辅助脚本；完整版本及许可保留于 `third_party/installed-x64/vcpkg/status`、`third_party/installed-Win32/vcpkg/status` 和各 `share/<包>/copyright`。发布候选只打包运行所需 DLL 和许可，构建工具不进入产品包。

初次执行 `scripts/Bootstrap.ps1`，然后 `scripts/Build.ps1 -Dependencies -Platform x64 -Configuration Debug`。Release 共用同一 triplet 的两套依赖；Win32 使用 `x86-windows` 独立构建。所有业务 `.c` 文件明确 `/TC /std:c17 /utf-8`，保留 v145 及主项目 GUID。

依赖源码、工具、安装目录与二进制缓存均在项目 `third_party/` 或 `build/` 下。系统 vcpkg 不执行 integrate install，不改变机器全局项目设置。部分第三方工具由 C++ 构建；Drop 业务代码和接口为 C。CivetWeb overlay 关闭 C++ wrapper 和未使用的 WebSocket zlib 压缩，保留 C WebSocket 接口。

项目局部修正在 `third_party/overlays/`，配置由 vcpkg-configuration.json 接入，原标签及下载哈希保留。Tcl 修正移除 Windows 不需要的 SQLite 扩展；CivetWeb 修正 Windows TLS 关闭为有界完整关闭，最多等待对端 1 秒，防止短连接响应丢失。解释及副本许可见 overlays/README.md。验收含 100 次不重试的 Connection: close 请求，未仅靠连接复用绕过故障。

架构构建与运行结果见 VERIFICATION.md。SQLCipher port 的版本比上游新版本滞后，当前固定用于开发验证；发布前须复核上游安全公告、升级兼容性及加密恢复，不以固定版本代表永久免维护。
