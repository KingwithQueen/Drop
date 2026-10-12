# Drop Linux 和 macOS 图形版本开发交接

交接日期：2026年10月12日。Windows 工作目录：`E:\work\Drop`。当前产品版本：`1.0.2`。

Windows 1.0.2 已完成开发。下一对话开展 Linux 和 macOS 图形界面版本的移植；Linux 仅要求 Wayland。两台远端开发机的 SSH、公钥认证、指定开发目录及基础编译环境已验收通过。本对话只完成环境准备、连接配置、基础编译运行检查及交接文档，未修改产品实现，也未向远端同步项目源码。

交接复核时，当前工作区已出现新的 CMake、POSIX 平台文件及多处未提交的源码修改。这些改动未在本对话构建验证，应作为后续继续工作的现有内容保留，具体见“代码与构建起点”。下一对话先读取现场状态，再判断已有工作的完成程度。

## 新对话开始步骤

1. 阅读本交接、根目录 `AGENTS.md`、当前 `README.md`、`docs/DEVELOPMENT_STATUS.md` 和现有源码；先检查 Git 状态，保留手工修改。
2. 复用下方的专用 SSH 配置，分别确认两个 `owl` 账号可登录及开发目录可访问。
3. 检查远端已有文件和适用的 `AGENTS.md`，再安排源码同步、跨平台构建及实现。操作限于本项目和下方明确指定的远端开发目录。
4. 沿用已确认的紧凑布局与交互；改变主界面设计时，按根目录 `AGENTS.md` 提交可审阅的设计图并取得确认。已有布局授权和普通实现选择无需重复询问。

`DEVELOPMENT_PLAN.md` 保留早期计划，其中“尚无源码”等内容描述的是建立计划时的历史状态。当前实现状态以 `AGENTS.md`、`README.md`、状态文档及实际源码为准。

## SSH 登录

连接从当前 Windows 电脑发起，使用系统 OpenSSH 客户端。新对话继续使用这台电脑及 `E:\work\Drop` 时，可以直接复用已有文件。

| 目标 | SSH 别名 | 地址 | 用户 | 端口 | 开发目录 |
| --- | --- | --- | --- | --- | --- |
| Linux | `drop-linux` | `192.168.1.51` | `owl` | `22` | `/home/owl/开发/Drop` |
| macOS | `drop-mac` | `10.10.10.221` | `owl` | `22` | `/Users/owl/程序/Drop` |

两个目录均已验证存在、可写，规范化后的路径与用户指定路径一致。Mac 最终登录用户名为 `owl`，此前提到的 `Jsnowly` 已被用户更正。

本地连接文件如下。

| 文件 | 用途 |
| --- | --- |
| `E:\work\Drop\build\ssh\config` | 两台开发机的 SSH 配置 |
| `E:\work\Drop\build\ssh\drop-dev-ed25519` | 专用私钥，仅保存在本机 |
| `E:\work\Drop\build\ssh\drop-dev-ed25519.pub` | 专用公钥，已加入两台远端 `owl` 账号 |
| `E:\work\Drop\build\ssh\known_hosts` | 已核验的两台主机密钥 |
| `E:\work\Drop\build\ssh\targets.json` | 地址、用户、开发目录、指纹及 Mac 构建语言环境 |
| `E:\work\Drop\build\ssh\environment-verification-20261012.json` | 基础环境验收记录 |

私钥无交互口令，文件权限已限制为当前 Windows 用户、SYSTEM 和管理员；未读取或输出私钥正文。`build/` 被根目录 `.gitignore` 忽略。保留 `build\ssh`，不要把私钥加入 Git、源码同步包或发布包。交接文档只记录文件位置和公开指纹。

### 直接验证登录

在 Windows PowerShell 中执行：

```powershell
ssh.exe -F 'E:\work\Drop\build\ssh\config' drop-linux 'id -un'
ssh.exe -F 'E:\work\Drop\build\ssh\config' drop-mac 'id -un'
```

两条命令的预期输出均为 `owl`。务必带 `-F`；这些别名定义在项目专用配置中。

需要用户自己打开交互终端时，可执行：

```powershell
ssh.exe -F 'E:\work\Drop\build\ssh\config' -t drop-linux
ssh.exe -F 'E:\work\Drop\build\ssh\config' -t drop-mac
```

`-t` 为交互终端申请 TTY；自动构建及脚本操作沿用配置中的 `RequestTTY no`。`BatchMode yes` 和 `IdentitiesOnly yes` 让连接仅使用现有专用密钥，不等待密码输入。

### 主机指纹

用户在两台机器本地提供了以下 ED25519 指纹，已与实际 SSH 握手取得的主机密钥逐一核对一致：

| 目标 | SHA256 指纹 |
| --- | --- |
| Linux | `SHA256:uwDv6yWxd6GqQM1M+shchJeBOz75inwfn42syQUN1hY` |
| macOS | `SHA256:VgWIL8g734EUz9b1nNNkE8FUIYieqAxJJfrB1EtE+yU` |

专用客户端公钥指纹为 `SHA256:WcKL020ZXsO7WX/LfoAzlZB52p5mysQtJc2k0zSFEJk`，它与上面的服务器主机指纹用途不同。

配置启用了 `StrictHostKeyChecking yes`、`HostKeyAlgorithms ssh-ed25519`、`KexAlgorithms curve25519-sha256`，使用项目内的 `known_hosts`。如果以后出现主机指纹变化，先由用户在目标机器本地确认原因和新指纹，再更新记录。

### 当前会话的连接限制

本会话普通沙箱命令进行网络连接时曾报“以一种访问权限不允许的方式做了一个访问套接字的尝试”。随后通过 `exec_command` 的 `sandbox_permissions: "require_escalated"` 进入权限审核，已成功完成两台机器的登录和环境验收。新对话若出现相同错误，应使用该权限流程执行范围明确的 SSH 操作。

用户已授权连接这两台开发机，并明确指定上述开发目录。同范围的登录、检查和后续开发按既有授权继续；自动审核拒绝时应说明实际拒绝原因并按规则处理。

Mac 的 `sudo -n true` 曾返回 `sudo: a password is required`。普通 `owl` 登录与管理员认证是两件事；需要管理员权限时，由用户在本机终端或本地遮蔽输入界面完成认证。密码和私钥正文不经聊天传递，也不写入日志。用户控制注销、重启及其他会结束当前桌面会话的操作。

### Windows 主机密钥扫描工具兼容问题

当前 Windows OpenSSH 版本为 `9.5p2`。它的 `ssh-keyscan` 连接两台机器时曾报：

```text
choose_kex: unsupported KEX method sntrup761x25519-sha512@openssh.com
```

已使用 SSH 客户端、`curve25519-sha256` 和 ED25519 主机密钥完成握手，核对用户提供的指纹后才启用公钥账号登录。现有配置已经可以正常连接，后续直接复用。

## 中文目录和远程脚本

Mac 的非交互 SSH 会话最初使用 `LANG` 为空、`LC_CTYPE=C` 的环境。MacPorts 在中文工作目录中因此曾把“程序”解释为错误字符，报无法切换工作目录。显式指定 UTF-8 后已验证正常。

Mac 的每个远程构建脚本都设置以下环境，再进入目录。这些设置仅作用于该次脚本，不改机器的全局环境。

```sh
set -eu
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
export PATH=/opt/local/bin:/opt/local/sbin:/usr/bin:/bin:/usr/sbin:/sbin
cd '/Users/owl/程序/Drop'
```

MacPorts 工具位于 `/opt/local/bin`；非交互 SSH 会话不能依赖桌面终端的 PATH。Mac 使用 `python3.13`，MacPorts 的 GNU libtool 命令为 `glibtool`；系统同名 `libtool` 是 Apple 工具。

Linux 的脚本使用：

```sh
set -eu
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
cd '/home/owl/开发/Drop'
```

### 已验证的 UTF-8 脚本传输方式

Windows PowerShell 原生命令参数涉及多行和多重引号时，使用 `ProcessStartInfo.ArgumentList`，并以 UTF-8 通过标准输入传给远端 `/bin/sh -s`。以下示例已在本会话验证，可正确输出 Mac 中文目录、Xcode 和 CMake 版本。

```powershell
$dropRemote = @'
set -eu
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
export PATH=/opt/local/bin:/opt/local/sbin:/usr/bin:/bin:/usr/sbin:/sbin
cd '/Users/owl/程序/Drop'
pwd -P
xcodebuild -version
cmake --version
'@

$dropStart = [Diagnostics.ProcessStartInfo]::new()
$dropStart.FileName = (Get-Command ssh.exe).Source
$dropStart.UseShellExecute = $false
$dropStart.CreateNoWindow = $true
$dropStart.RedirectStandardInput = $true
$dropStart.RedirectStandardOutput = $true
$dropStart.RedirectStandardError = $true
$dropStart.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
$dropStart.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
$dropStart.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
foreach ($dropArgument in @('-F', 'E:\work\Drop\build\ssh\config', 'drop-mac', '/bin/sh -s')) {
    $dropStart.ArgumentList.Add($dropArgument)
}

$dropProcess = [Diagnostics.Process]::Start($dropStart)
$dropOut = $dropProcess.StandardOutput.ReadToEndAsync()
$dropErr = $dropProcess.StandardError.ReadToEndAsync()
$dropProcess.StandardInput.Write($dropRemote.Replace("`r`n", "`n") + "`n")
$dropProcess.StandardInput.Close()
if (!$dropProcess.WaitForExit(45000)) {
    $dropProcess.Kill()
    throw 'SSH verification timed out.'
}
$dropOut.GetAwaiter().GetResult()
$dropDiagnostic = $dropErr.GetAwaiter().GetResult()
if ($dropDiagnostic) { [Console]::Error.Write($dropDiagnostic) }
if ($dropProcess.ExitCode -ne 0) { throw ('SSH command failed: ' + $dropProcess.ExitCode) }
```

Linux 使用同样的传输方法，改为 `drop-linux`，并替换远端脚本中的工作目录及需要的工具命令。示例的 45 秒超时适用于环境检查；耗时构建应使用可持续跟踪输出的进程会话，按任务设置时限。

## 已验收的环境

以下为2026年10月12日实测结果。版本和连接状态可能随机器更新变化，新对话先做简短复核。

| 项目 | Linux | macOS |
| --- | --- | --- |
| 系统 | openSUSE Tumbleweed，快照 `20261007`，KDE 6 | Ventura `13.7.8`，构建 `22H730` |
| 架构 | `x86_64` | Intel `x86_64` |
| 图形开发方案 | GTK4，Wayland | 原生 AppKit，必要的少量 Objective-C 桥接 |
| C 编译器 | GCC `16.2.1`；Clang `23.1.2` | Apple Clang `15.0.0` |
| Xcode | 不适用 | `15.2`，构建 `15C500b` |
| CMake | `4.4.3` | `3.31.12` |
| Ninja | `1.13.2` | `1.13.2` |
| Python | `3.13.15`，命令 `python3` | `3.13.16`，命令 `python3.13` |
| pkg-config | `2.5.1` | `0.29.2` |
| GTK4 | `4.22.5` | 不作为当前 Mac 界面方案 |
| libsecret | `0.21.8.2` | 凭据保护需接入 macOS 系统接口 |
| Autoconf | `2.73` | `2.73` |
| Automake | `1.19` | `1.18.1` |
| GNU libtool | `2.6.2`，命令 `libtool` | `2.6.2`，命令 `glibtool` |

用户确认的目标是图形界面版本和 Linux 仅支持 Wayland；GTK4、AppKit及跨平台构建组织属于此前讨论的实施方案，具体接入根据工程需要决定并记录。

Linux 的 `owl` 账号有 `Remote=no`、`Type=wayland`、`Active=yes` 的桌面会话，`xdg-desktop-portal` 和 `xdg-desktop-portal-kde6` 已检查安装。

Mac 当前开发者目录为 `/Applications/Xcode.app/Contents/Developer`。SDK 路径为 `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`。Xcode 15.2、CMake、Ninja、pkg-config、Python和构建辅助工具已经可调用。

MacPorts 配置过程中曾出现 `Error: Port cmake not found`，当时软件源中没有 `PortIndex`。用户在 Mac 本地完成同步和安装后，上述构建工具已实测可用。无需据旧错误假设当前环境仍缺包。

## 验收范围

两台机器均完成了实际编译、链接及运行：

```text
Linux: PASS C17 + GTK4: 4.22.5
macOS: PASS C17 + AppKit
```

测试在用户指定开发目录内的临时子目录进行，结束后自动清理。Linux 调用了 GTK4 版本接口；Mac 加载了 AppKit 类。该结果验证编译器、头文件、链接及基础库加载，不代替实际窗口、托盘、通知、拖放、缩放及客户端同步的验收。

本对话没有编译或运行完整的 Linux/macOS Drop 产品；当前新增跨平台改动的编译、业务依赖及功能验收状态需在下一对话核对。上面的基础环境验收记录不代表这些新增改动已通过测试。Apple Silicon 的运行验证也未在本对话开展。

## 移植时保留的产品约定

所有者指定当前版本为 `1.0.2`，程序可见版本、文档和包名采用三段格式。Windows 内部固定资源末尾零只用于系统要求的四段元数据。版本来源为 `dropcommon/version.h`。

业务代码采用 C17、UTF-8和根目录 `.clang-format`；中文注释、四空格缩进、180列换行规则以 `AGENTS.md` 为准。第三方目录保留上游格式与许可证。

主界面沿用紧凑传统布局：真实账户列表、消息区域、正文、明确的发送对象、待发送附件预览和状态栏。自己的账号及设备明确标识，左侧已移除“全部消息”。附件归属各自消息，显示文件名、大小及保存状态；点击只选择，用户明确导出后才保存到所选位置。

消息删除权限按发送账号控制：同账号电脑 B 可以删除电脑 A 发出的消息，不能删除其他账号的消息。删除同步至服务器及双方账号的所有设备，离线操作持久排队。账户删除仍保留历史内容。

通信保留两个明确模式：HTTP/WS 配合 PAKE；HTTPS/WSS 使用 TLS，不叠加 PAKE，不自动降级。服务器和云备份存储保持加密。本次目标沿用已有业务协议和数据语义。

运行数据目录、手动覆盖、权限失败和升级保留策略以当前 README 与部署文档为准。Windows 的“程序旁 Data，禁止向系统盘回退”约定需要保留；跨平台数据布局在实现时明确记录。各设备使用自己的本地数据库，通过协议同步消息和附件。

## 代码与构建起点

现有 Windows 构建使用 `drop.slnx`、Visual Studio C 项目及 PowerShell 脚本。交接复核时，工作区已新增 Linux/macOS 构建入口和平台适配文件，并修改了若干共享源码；本对话没有编写或验证这些产品改动。

复核时 `git status --short` 显示下列源码修改。工作区正在变化，下一对话应重新检查状态；保留这些文件的现有内容，避免重新初始化或覆盖正在进行的移植。

```text
 M drop/client.c
 M drop/network.h
 M drop/transfers.c
 M drop/ui_data.c
 M dropcommon/common.c
 M dropcommon/drop_common.h
 M dropcommon/object.c
 M dropcommon/srp_vendor.c
?? CMakeLists.txt
?? checks/
?? platform/
?? scripts/Bootstrap-Unix.sh
?? scripts/Build-Unix.sh
```

新 `CMakeLists.txt` 使用 C17、pkg-config 和 Threads，定义 `dropcommon`、`dropclient` 及从 `platform/main.c` 生成的 `drop`。当前可见的平台文件包括 `encoding.c`、`posix.c`、`posix.h`、`storage.c`、`network_curl.c`、`secret_linux.c`、`secret_macos.c` 和 `main.c`。共享头文件已增加 `_WIN32` 条件和 POSIX 头文件分支，不能继续按“只有 Windows 实现”假设修改。

`scripts/Bootstrap-Unix.sh` 中有按固定源码及 SHA-512 获取 cJSON、libsodium、SQLite 并写入项目 `build/unix` 的步骤。`scripts/Build-Unix.sh` 中有配置、构建、CTest 和版本查询步骤。`checks/portable_check.c` 也已出现，并由当前 CMake 测试配置引用。已查看的 `platform/main.c` 是命令入口，当前适配代码及图形界面的实现程度应在新对话中按现场源码核实；用户确定的产品目标仍是图形界面版本。

| 当前文件 | 后续工作关联 |
| --- | --- |
| `dropcommon/drop_common.h`、`dropcommon/platform.c` | Win32 类型、文件和目录权限、DPAPI等平台边界 |
| `CMakeLists.txt`、`scripts/Bootstrap-Unix.sh`、`scripts/Build-Unix.sh` | 已出现的跨平台构建改动，先阅读并核实状态 |
| `platform/` | 已出现的 POSIX、编码、存储、libcurl和系统秘密保护适配 |
| `checks/portable_check.c` | 已出现的跨平台检查源码，本对话未运行 |
| `drop/network.c`、`drop/network_ws.c`、`drop/trust.c` | WinHTTP、WebSocket、取消及证书验证 |
| `drop/client.c`、`drop/transfers.c`、`drop/deletions.c` | 本机存储、消息同步、附件、删除队列 |
| `drop/ui_worker.c` | 心跳、重连、任务调度和界面交还，需整理为可复用同步调度 |
| `drop/ui*.c`、`drop/ui_data.c` | 现有界面交互及数据查询参考 |
| `drop/main.c` | 已有命令入口；`client-watch` 是诊断监听，不是完整后台客户端 |
| `docs/design/client-main-win32-1.0.1.png`、同名 SVG | 已确认的原始布局参考；结合1.0.2实际界面与最新约定使用 |

既有 Windows 依赖由根目录 `vcpkg.json` 固定，baseline 为 `e456309491fd875487bf02b8a0dca76c1b1b00cc`，包含 cJSON、CivetWeb、OpenSSL、SQLite、SQLCipher、libsodium 和 Zstandard。CSRP 源码在 `vendor/csrp`。当前 Unix 脚本固定获取其中部分库，当前 CMake 还通过 pkg-config 查询 OpenSSL、Zstandard、libcurl等依赖。后续按 `docs/DEPENDENCIES.md` 核对跨平台依赖来源、版本及许可；检查 `third_party/overlays` 中的 Windows 条件补丁是否适用于目标平台。

客户端使用 SQLite，服务器使用 SQLCipher；保持两者的链接边界。完善跨平台构建入口时保留现有 Windows 解决方案和构建流程，并用原有自测及必要的跨平台联调验证行为。

## 源码同步与后续验收

Windows `E:\work\Drop` 是本次交接的现有源码位置。下一对话先读取远端已有内容，再同步需要的源码和文档。源码同步排除本地 `build/ssh`、运行数据、私钥、Windows 编译产物和已编译的第三方安装目录；各平台在自己的开发目录构建。

实现后验证登录、两个传输模式、文本及附件发送接收、续传、断线恢复、跨设备删除、账号状态和历史保留。Linux 使用实际 Wayland 桌面，Mac 使用真实图形会话；SSH编译成功只证明构建，不证明图形界面行为。

后台网络、数据库和文件任务需保持可取消，界面事件及时响应。Linux 验证 KDE 托盘、通知、剪贴板、拖放及非整数缩放；Mac 验证菜单栏图标、通知、钥匙串、文件导出、缩放和休眠恢复。接入中央服务器时使用独立测试账号和数据目录，保护现有部署与历史内容。

## 新对话可使用的启动语

> 请读取 E:\work\Drop\docs\HANDOFF_LINUX_MACOS_20261012.md 和根目录 AGENTS.md，复核现有源码、未提交的跨平台改动及已配置的 SSH。在保留现有工作的基础上继续开发 Drop 的 Linux Wayland 和 macOS 图形界面版本，产品版本保持1.0.2。两台远端都以 owl 登录，Linux 开发目录为 /home/owl/开发/Drop，Mac 为 /Users/owl/程序/Drop。复用交接中的专用 SSH 配置，先检查远端已有文件，保留手工修改，再继续移植。沿用已确认的紧凑布局和消息、附件、删除权限约定。

相关本地文件：[工程规范](E:/work/Drop/AGENTS.md)、[当前说明](E:/work/Drop/README.md)、[依赖说明](E:/work/Drop/docs/DEPENDENCIES.md)、[开发状态](E:/work/Drop/docs/DEVELOPMENT_STATUS.md)、[验收记录](E:/work/Drop/build/ssh/environment-verification-20261012.json)、[SSH 配置](E:/work/Drop/build/ssh/config)、[远端目录配置](E:/work/Drop/build/ssh/targets.json)。

