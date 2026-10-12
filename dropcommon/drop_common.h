#ifndef DROP_COMMON_H
#define DROP_COMMON_H
#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stddef.h>
#include <cjson/cJSON.h>
#include "version.h"

#define DROP_PROTOCOL_VERSION 1
#define DROP_SCHEMA_VERSION 12
#define DROP_MAX_BODY (1024u * 1024u)
#define DROP_MAX_JSON (2u * 1024u * 1024u)
#define DROP_MAX_ATTACHMENTS 32
#define DROP_ID_CAP 33
#define DROP_TOKEN_CAP 65

/* 客户端和服务器共用的错误分类；其对外文本由 drop_error_name 提供，枚举本身不直接作为协议数值。 */
typedef enum drop_error {
    DROP_OK = 0,
    DROP_E_ARGUMENT,
    DROP_E_LIMIT,
    DROP_E_IO,
    DROP_E_CRYPTO,
    DROP_E_DATABASE,
    DROP_E_AUTH,
    DROP_E_CONFLICT,
    DROP_E_PROTOCOL,
    DROP_E_NETWORK,
    DROP_E_CERTIFICATE,
    DROP_E_UNSUPPORTED,
    DROP_E_SPACE,
    DROP_E_PENDING
} drop_error;
/* 已解析的服务器根地址；secure 明确区分 TLS 与 PAKE 模式，不能据网络失败自动切换。 */
typedef struct drop_url {
    WCHAR host[256];
    INTERNET_PORT port;
    int secure;
} drop_url;
/* 公开运行配置及同步参数，不保存密码、令牌或运行密钥。 */
typedef struct drop_config {
    WCHAR server_url[2049];
    WCHAR ca_file[32768];
    unsigned heartbeat_seconds, connection_timeout_seconds, reconcile_seconds;
    int use_zstd, zstd_level;
} drop_config;
/* 正文压缩结果，data 由结构拥有，size 为保存长度，original_size 为受限的原始 UTF-8 字节数。 */
typedef struct drop_text {
    unsigned char *data;
    size_t size;
    size_t original_size;
    int compressed;
} drop_text;

/* 返回协议使用的稳定错误名称；未知枚举值统一表示为 unknown_error，返回字符串无需释放。 */
const char *drop_error_name(drop_error error);
/* 将当前 Windows 文件错误转换为项目错误，单独识别磁盘空间不足。 */
drop_error drop_io_error(void);
/* 初始化 libsodium；进程使用随机数和加密接口前必须调用，失败返回 0。 */
int drop_init(void);
/* 验证 3-64 字节的 ASCII 登录名及允许字符，不将显示名当作登录身份。 */
int drop_account_login_valid(const char *login);
/* 严格解析非空十进制无符号整数，拒绝非法字符及 64 位溢出；成功后才写入 value。 */
int drop_u64_parse(const char *text, uint64_t *value);
/* 检查固定长度的小写十六进制 ID，避免未经校验的标识进入协议和文件路径。 */
int drop_id_valid(const char *text);
/* 使用安全随机数生成 128 位 ID，并写成带终止符的小写十六进制字符串。 */
void drop_id_new(char output[DROP_ID_CAP]);
/* 使用安全随机数生成 256 位凭据；调用者负责保护输出并在不再使用时清零。 */
void drop_token_new(char output[DROP_TOKEN_CAP]);
/* 验证指定长度的 UTF-8 字节序列；同时拒绝内容中的 NUL，避免长度和字符串解释不一致。 */
int drop_utf8_valid(const char *text, size_t size);
/* 在 Windows API 边界将 UTF-8 转为 UTF-16；容量按 WCHAR 个数计算，非法输入或容量不足返回 0。 */
int drop_to_wide(const char *text, WCHAR *output, size_t capacity);
/* 将 Windows UTF-16 字符串转为 UTF-8；容量按字节计算，转换失败返回 0。 */
int drop_to_utf8(const WCHAR *text, char *output, size_t capacity);
/* 解析 HTTP 或 HTTPS 服务器根地址，拒绝用户信息、查询和业务路径；保留端口及明确的加密模式。 */
int drop_url_parse(const WCHAR *text, drop_url *url);
/* 读取受限大小的 UTF-8 INI，设置默认值并检查重复键、未知项及参数范围；失败清空输出，不接受明文秘密字段。 */
int drop_config_load(const WCHAR *path, drop_config *config);
/* 在大小和结构限制内解析完整 JSON 对象，拒绝嵌入 NUL、尾随内容及重复键；调用者用 cJSON_Delete 释放结果。 */
cJSON *drop_json_parse(const char *data, size_t size);
/* 读取大小写精确匹配且长度受限的字符串字段；返回 JSON 内部指针，其有效期随 object 结束。 */
const char *drop_json_string(const cJSON *object, const char *key, size_t maximum);
/* 校验正文并按配置尝试 zstd，仅在压缩后变小时保存压缩形式；成功结果由 drop_text_free 释放。 */
int drop_text_pack(const char *text, int use_zstd, int level, drop_text *output);
/* 按记录的原始长度严格解压并验证 UTF-8，拒绝损坏或额外压缩帧；成功返回的字符串由调用者 free。 */
int drop_text_unpack(const drop_text *input, char **output);
/* 释放正文缓冲并清空结构，允许传入空指针。 */
void drop_text_free(drop_text *text);
/* 在给定容量内连接 Windows 目录和文件名；检查长度并保留绝对目录的既有含义。 */
int drop_path_join(const WCHAR *root, const WCHAR *name, WCHAR *path, size_t capacity);
/* 基于当前 EXE 确定 Data 下的应用目录，可按需创建父目录；不向 AppData 或系统盘回退。 */
int drop_program_data_directory(const WCHAR *application, WCHAR *path, size_t capacity, int create_parent);
/* 创建或收紧用户数据目录的受保护权限，只允许当前用户、管理员和 SYSTEM 访问。 */
int drop_private_directory(const WCHAR *path);
/* 按是否允许替换创建文件，完整写入后刷新；失败返回 0，调用者决定是否保留或清理目标。 */
int drop_file_write(const WCHAR *path, const void *data, DWORD size, int replace);
/* 在长度上限内完整读取文件并追加 NUL；成功缓冲由调用者 free，输出 size 不含终止符。 */
int drop_file_read(const WCHAR *path, unsigned char **data, DWORD *size, DWORD limit);
/* 使用当前 Windows 用户的 DPAPI 创建秘密文件，拒绝覆盖已有文件。 */
int drop_secret_save(const WCHAR *path, const void *data, DWORD size);
/* 使用用户 DPAPI 更新秘密文件；调用者负责在业务允许时选择替换。 */
int drop_secret_replace(const WCHAR *path, const void *data, DWORD size);
/* 读取并解开受保护秘密，复制为调用者拥有的缓冲；使用后必须清零并 free。 */
int drop_secret_load(const WCHAR *path, unsigned char **data, DWORD *size);
/* 使用机器 DPAPI 创建服务秘密文件，要求目标目录已具备受限权限。 */
int drop_machine_secret_save(const WCHAR *path, const void *data, DWORD size);
/* 使用机器 DPAPI 替换服务秘密文件，供已授权的服务身份转换或秘密更新使用。 */
int drop_machine_secret_replace(const WCHAR *path, const void *data, DWORD size);
/* 将服务目录设置为 SYSTEM 和管理员可访问并调整所有者；权限设置失败返回 0。 */
int drop_machine_directory(const WCHAR *path);
/* 收紧服务数据目录及已有子项的权限，供转换机器身份前调用；不穿越重解析点。 */
int drop_machine_tree(const WCHAR *path);
/* 只从真实控制台遮蔽读取密码，拒绝重定向输入；限制长度并恢复控制台模式，失败清空输出。 */
int drop_password_read(const WCHAR *prompt, char *password, size_t capacity);
/* 在调用者提供的隔离目录验证整数、UTF-8、配置、JSON、压缩、DPAPI 和对象认证边界。 */
int drop_common_test(const WCHAR *directory);
#endif
