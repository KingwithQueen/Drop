# 客户端证书校验

默认无 CA 文件时，WinHTTP 使用 Windows 默认链、主机名、有效期和用途检查，不忽略证书错误。私有 CA 模式由用户明确提供一个 PEM 证书文件；当前读取其中一个根/受控信任证书，证书文件路径保存为绝对路径。该设置只影响此客户端的连接，不写系统信任库。

私有 CA 模式只将 unknown-CA 的判定交给应用内独占根链引擎，其余 WinHTTP 检查保持启用。在 SENDING_REQUEST 回调中查询当前实际 TLS 证书，使用 exclusive-root CryptoAPI chain engine 和 SSL policy 再验证签名链、主机名、日期和 serverAuth 用途；拒绝时在 HTTP 头和正文发送前关闭请求。既不先发凭据再查证书，也不先无认证探测后信任另一条连接。每次独立会话重新验证，不跟随重定向。

该回调使用方式参考 [Microsoft WinHTTP 客户端实现](https://github.com/microsoft/cpprestsdk/blob/master/Release/src/http/client/http_client_winhttp.cpp)，CryptoAPI 语义参考 [CertGetCertificateChain](https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certgetcertificatechain)。没有配置证书吊销联网检查，不把默认链验证描述成完整在线吊销验证；多根 PEM 和证书自动更新尚未实现。

实际验收：原生客户端 HTTPS/WSS 正向连接、未配置的自签名拒绝、错误私有 CA 拒绝并且服务器没有收到该设备登录、过期证书拒绝；测试记录见 VERIFICATION.md。曾尝试临时 CurrentUser ROOT 测试，Windows 确认阻塞，已取消并核查没有新增测试证书。正式测试完全使用应用内 CA。
