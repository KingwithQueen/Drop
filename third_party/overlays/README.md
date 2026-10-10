# 固定构建修正

`tcl/` 复制自 vcpkg baseline `e456309491fd875487bf02b8a0dca76c1b1b00cc` 的 9.0.3 port 2，保留原下载 SHA-512 及补丁，仅在 Windows 禁用不使用的 bundled SQLite Tcl 扩展。上游 port 删除了 bundled sqlite3.c，Windows 扩展仍试图包含它，触发 C1083；顶层循环还会继续，不能据退出码忽略错误。

Drop 仅用 Tclsh 生成 SQLCipher amalgamation，不链接 Tcl 或使用此扩展。本修正不改变客户端 SQLite 或服务器 SQLCipher 的功能和加密选项。解释器及 SQLCipher 构建需实际通过，结果记录在 docs/VERIFICATION.md。副本许可见 LICENSE.vcpkg.txt。

`civetweb/` 复制自相同 baseline 的 1.16 port 2，保留下载哈希及原补丁，添加 bounded-tls-shutdown.patch。Windows 非阻塞 TLS 单次关闭后立即 TCP shutdown 会丢失最后响应；在固定 1 秒 socket 超时内执行完整 SSL_shutdown 后再关闭 TCP，避免无限等待。HTTPS 100 次 Connection: close 回归实际通过。关闭未使用的 C++ wrapper 和 zlib WebSocket 压缩，业务通过 C 接口调用。

关闭逻辑参考 [OpenSSL 3.6 SSL_shutdown](https://docs.openssl.org/3.6/man3/SSL_shutdown/)，修复效果以本项目真实集成回归为依据。构建与测试不修改系统信任或全局 vcpkg 设置。
