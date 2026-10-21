# Android 开发环境准备

核实日期：2026-10-20。此文记录现有桌面代码整理之后的 Android 工具准备；Android 客户端尚未实现，也未生成 APK。

## 本机 Visual Studio Installer

当前安装的是 Visual Studio Community 2026（18.10.3）。在 Installer 的“修改”中保留“使用 C++ 的桌面开发”，确认包含：

- MSVC v145 的 x64/x86 生成工具，用于维护现有 Windows C17 客户端和服务器。
- Windows SDK，用于现有 Windows 资源和系统接口。
- 适用于 Windows 的 C++ CMake 工具，用于编辑和配置 CMake 工程。
- C++ Clang 工具（可选），用于本机静态检查和 clang-format；Android 交叉编译使用 NDK 自带的 Clang。

这些组件的名称含有 C++，不要求项目使用 C++；现有 Windows 项目明确配置为 C17，Unix 项目也只启用 C 语言。

微软从 VS 2026（18.0）开始已停止支持面向 Android/iOS 的“使用 C++ 的移动开发”工作负载，并计划移除；其中附带的 Android NDK 仍受支持。因此不把旧版 VS Android 工程模板作为本项目后续构建的前提，也不需要为 C 项目安装 .NET MAUI。

官方说明：[跨平台移动开发支持状态](https://learn.microsoft.com/en-us/cpp/cross-platform/visual-cpp-for-cross-platform-mobile-development?view=msvc-170)、[VS 构建工具组件清单](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=visualstudio)。

## Android 工具

通过 Android Studio 的 SDK Manager 或 Google 官方命令行工具安装：

| 组件 | 用途 |
| --- | --- |
| Android SDK Platform | 提供确定目标 API 所需的 Android 平台文件；具体 API 在 Android 工程建立时统一锁定。 |
| Android SDK Build-Tools | 资源处理、APK 对齐及签名等打包工具。 |
| Android SDK Platform-Tools | adb 安装、日志和真机连接。 |
| NDK (Side by side) | Android 的 C17 交叉编译器、头文件和原生系统库；工程明确固定 NDK 版本。 |
| CMake 与 Ninja | 配置和构建 C 原生库；与 NDK 版本一并记录。 |
| JDK / Android Studio 自带 JBR | 运行 Android 构建与签名工具，不表示应用业务需要 Java 源码。 |
| Android Emulator 与系统镜像（可选） | 无真机时运行验收；初期可以直接使用已开启 USB 调试的 Android 真机。 |
| 设备厂商的 USB 驱动（按需） | Windows 上通过 USB 连接部分品牌设备。 |

SDK、NDK、构建缓存可放在例如 `E:\Android\Sdk` 的非系统盘目录；本项目继续位于 `E:\work\Drop`。不要把本机 SDK 路径、签名私钥、运行数据或工具缓存加入 Git。

官方说明：[安装 NDK 和 CMake](https://developer.android.com/studio/projects/install-ndk)、[SDK 工具](https://developer.android.com/tools)、[原生 Activity 的 C 入口示例](https://developer.android.com/ndk/samples/sample_na)。

## 后续实现边界

以 Android NDK、NativeActivity 和既有 C17 协议/存储核心为起点，项目自有程序代码保持 C。系统框架需要时由 C 调用 JNI；工程配置、资源 XML 和打包脚本属于构建材料。

移动端的输入法、文件选择与导出、通知、后台生命周期、系统秘密存储、触摸和屏幕布局需要单独设计验证。桌面平台的 Secret Service、Keychain、Win32 和 GTK 适配不能直接作为 Android 实现。安卓版主界面在实现前按项目约定提交设计图确认。
