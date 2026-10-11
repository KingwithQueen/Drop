/* WinHTTP 证书校验与异步等待适配；指定私有 CA 时只信任应用配置的根。 */
#include "trust.h"
#include <stdlib.h>
#include <string.h>

/* 将异步完成转为事件，并在发送 HTTP 凭据前使用独占 CA 引擎验证服务器；验证失败立即关闭请求。 */
static void CALLBACK verify_before_send(HINTERNET request, DWORD_PTR context, DWORD status, void *info, DWORD length) {
    drop_trust *trust = (drop_trust *)context;
    PCCERT_CONTEXT cert = NULL;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    DWORD size = sizeof(cert);
    CERT_CHAIN_PARA parameters = {0};
    CERT_CHAIN_POLICY_PARA policy = {0};
    CERT_CHAIN_POLICY_STATUS result = {0};
    HTTPSPolicyCallbackData ssl = {0};
    char *usage = szOID_PKIX_KP_SERVER_AUTH;
    int valid = 0;
    if (!trust) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        if (trust->closing) SetEvent(trust->closing);
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE || status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE ||
        status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE || status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        trust->completion_status = status;
        if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && info && length == sizeof(DWORD)) trust->available = *(DWORD *)info;
        if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) trust->available = length;
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && info && length == sizeof(WINHTTP_ASYNC_RESULT)) trust->operation_error = ((WINHTTP_ASYNC_RESULT *)info)->dwError;
        if (trust->completed) SetEvent(trust->completed);
        return;
    }
    if (status != WINHTTP_CALLBACK_STATUS_SENDING_REQUEST || !trust->engine) return;
    parameters.cbSize = sizeof(parameters);
    parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
    parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
    policy.cbSize = sizeof(policy);
    result.cbSize = sizeof(result);
    ssl.cbStruct = sizeof(ssl);
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = trust->host;
    policy.pvExtraPolicyPara = &ssl;
    if (WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &size) &&
        CertGetCertificateChain(trust->engine, cert, NULL, cert->hCertStore, &parameters, CERT_CHAIN_DISABLE_AIA | CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL, NULL, &chain) &&
        !chain->TrustStatus.dwErrorStatus && CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &result) && !result.dwError)
        valid = 1;
    if (chain) CertFreeCertificateChain(chain);
    if (cert) CertFreeCertificateContext(cert);
    trust->checked = 1;
    /* WinHTTP 在发送请求头和正文前通知 SENDING_REQUEST；在此拒绝证书，使令牌或密码尚未离开进程。 */
    if (!valid) {
        InterlockedExchange(&trust->rejected, 1);
        if (!InterlockedExchange(&trust->closed, 1)) WinHttpCloseHandle(request);
    }
}
/* 创建完成和关闭事件并安装 WinHTTP 回调；trust 必须存活到 drop_trust_close 等到关闭回调之后。 */
int drop_trust_async(drop_trust *trust, HINTERNET request) {
    DWORD_PTR context = (DWORD_PTR)trust;
    trust->completed = CreateEventW(NULL, TRUE, FALSE, NULL);
    trust->closing = CreateEventW(NULL, TRUE, FALSE, NULL);
    return trust->completed && trust->closing && WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) &&
        WinHttpSetStatusCallback(request, verify_before_send,
            WINHTTP_CALLBACK_FLAG_SEND_REQUEST | WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_DATA_AVAILABLE |
                WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES,
            0) != WINHTTP_INVALID_STATUS_CALLBACK;
}
/* 重置下一次异步操作的完成状态；同一 request 上的操作必须顺序进行。 */
void drop_trust_begin(drop_trust *trust) {
    ResetEvent(trust->completed);
    trust->completion_status = 0;
    trust->operation_error = 0;
    trust->available = 0;
}
/* 等待预期回调、取消或超时，检查证书和操作错误；失败设置 Windows 错误供上层诊断。 */
int drop_trust_wait(drop_trust *trust, HINTERNET request, DWORD expected, HANDLE cancel) {
    HANDLE events[3] = {trust->completed, trust->closing, cancel};
    DWORD result = WaitForMultipleObjects(cancel ? 3 : 2, events, FALSE, 30000);
    if (result == WAIT_OBJECT_0 && trust->completion_status == expected && !trust->operation_error && !trust->rejected) return 1;
    if (result == WAIT_OBJECT_0 + 2 || result == WAIT_TIMEOUT) {
        if (!InterlockedExchange(&trust->closed, 1)) WinHttpCloseHandle(request);
    }
    SetLastError(trust->rejected     ? ERROR_WINHTTP_SECURE_FAILURE
            : trust->operation_error ? trust->operation_error
            : result == WAIT_TIMEOUT ? ERROR_WINHTTP_TIMEOUT
                                     : ERROR_OPERATION_ABORTED);
    return 0;
}
/* 仅关闭请求一次并等待 HANDLE_CLOSING，保证回调不再访问 trust 和请求缓冲。 */
void drop_trust_close(drop_trust *trust, HINTERNET request) {
    if (request && !InterlockedExchange(&trust->closed, 1)) WinHttpCloseHandle(request);
    if (request && trust->closing) WaitForSingleObject(trust->closing, INFINITE);
}
/* 无 CA 文件时使用系统信任；指定 CA 时建立独占根引擎，仍验证主机名、日期及用途，失败释放资源。 */
int drop_trust_prepare(drop_trust *trust, HINTERNET request, const WCHAR *host, const WCHAR *ca_file) {
    unsigned char *pem = NULL, *der = NULL;
    DWORD size = 0, der_size = 0, flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
    CERT_CHAIN_ENGINE_CONFIG config = {0};
    DWORD_PTR context = (DWORD_PTR)trust;
    int ok = 0;
    memset(trust, 0, sizeof(*trust));
    if (!ca_file || !*ca_file) return 1; /* 沿用 WinHTTP 系统信任，不忽略任何证书错误。 */
    if (wcscpy_s(trust->host, 256, host) || !drop_file_read(ca_file, &pem, &size, 65536) ||
        !CryptStringToBinaryA((char *)pem, size, CRYPT_STRING_BASE64HEADER, NULL, &der_size, NULL, NULL))
        goto done;
    der = malloc(der_size);
    if (!der || !CryptStringToBinaryA((char *)pem, size, CRYPT_STRING_BASE64HEADER, der, &der_size, NULL, NULL)) goto done;
    trust->roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    if (!trust->roots || !CertAddEncodedCertificateToStore(trust->roots, X509_ASN_ENCODING, der, der_size, CERT_STORE_ADD_NEW, NULL)) goto done;
    config.cbSize = sizeof(config);
    config.hExclusiveRoot = trust->roots;
    if (!CertCreateCertificateChainEngine(&config, &trust->engine) || !WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(request, verify_before_send, WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0) == WINHTTP_INVALID_STATUS_CALLBACK ||
        !WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags)))
        goto done;
    /* 只把未知 CA 的判断交给应用独占根引擎；WinHTTP 仍检查主机名、有效期和用途，应用策略也再次核验。 */
    ok = 1;
done:
    free(pem);
    free(der);
    if (!ok) drop_trust_free(trust);
    return ok;
}
/* 在请求回调结束后释放事件、证书链引擎和根存储，并清空结构。 */
void drop_trust_free(drop_trust *trust) {
    if (trust->completed) CloseHandle(trust->completed);
    if (trust->closing) CloseHandle(trust->closing);
    if (trust->engine) CertFreeCertificateChainEngine(trust->engine);
    if (trust->roots) CertCloseStore(trust->roots, 0);
    memset(trust, 0, sizeof(*trust));
}
