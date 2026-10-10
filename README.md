# Drop

C17 Windows 客户端与中央服务器，保留 Visual Studio 2026 的 `drop.slnx` 和主项目 `drop`。已确认需求及阶段顺序见 [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)。

阶段零至七的本机实现与发布候选验收已完成，提供 x64 和 Win32 包；现场验证边界以 [开发状态](docs/DEVELOPMENT_STATUS.md) 及 [验收记录](docs/VERIFICATION.md) 为准。[构建与运行](docs/DEPLOYMENT.md)、[固定依赖](docs/DEPENDENCIES.md)、[协议](docs/PROTOCOL.md)、[存储格式](docs/STORAGE_FORMAT.md) 随实现同步维护。

本地加密备份、独立恢复包和服务器服务已接入；Google Drive 原生后端完成模拟验收，真实授权按用户要求留待后续。见 [备份恢复](docs/BACKUP_RESTORE.md) 与 [Google Drive](docs/GOOGLE_DRIVE.md)。发布候选包由 scripts/Package.ps1 生成，生产构建排除测试入口；实际验收限制以状态文档为准。
