/* 服务器前台及本机管理命令入口；新密码二次确认，数据路径在开始操作前确定。 */
#include "server.h"
#include "cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 输出服务器管理和运行命令说明，密码通过控制台读取，不要求在参数中携带。 */
static void usage(void) {
    puts("Drop server " DROP_VERSION " (development)\n"
         "  dropserver data-directory [--data DIRECTORY]\n"
         "  dropserver init [--data DIRECTORY]\n"
         "  dropserver account-create --data DIRECTORY --login NAME [--display NAME]\n"
         "  dropserver serve [--data DIRECTORY] [--certificate PEM] [--listen HOST:PORT]\n"
         "    [--config FILE] [--backup-directory DIRECTORY] [--backup-seconds SECONDS]\n"
         "  dropserver account-enable|account-disable|account-reset --data DIRECTORY --login NAME\n"
         "  dropserver account-delete --data DIRECTORY --login NAME\n"
         "  dropserver account-display --data DIRECTORY --login NAME --display DISPLAY_NAME\n"
         "  dropserver account-list --data DIRECTORY [--after LOGIN_NAME]\n"
         "  dropserver device-revoke --data DIRECTORY --login NAME --device ID\n"
         "  dropserver quota-set --data DIRECTORY --login NAME --bytes COUNT\n"
         "  dropserver storage-status|cleanup|runtime-machine --data DIRECTORY\n"
         "  dropserver recovery-export|backup --data DIRECTORY --target PATH\n"
         "  dropserver backup-trigger --data DIRECTORY\n"
         "  dropserver restore --data NEW_DIRECTORY --backup DIRECTORY --package FILE\n"
         "  dropserver service-install --name NAME [--data DIRECTORY] [--certificate PEM] [--listen HOST:PORT]\n"
         "  dropserver service-start|service-stop|service-status|service-uninstall --name NAME\n"
         "  dropserver drive-authorize --data DIRECTORY --oauth-client DESKTOP_JSON\n"
         "  dropserver drive-upload --data DIRECTORY --backup COMPLETE_DIRECTORY\n"
         "  dropserver drive-list|drive-status --data DIRECTORY\n"
         "  dropserver drive-download --data DIRECTORY --backup-id ID --target DIRECTORY\n"
         "  dropserver drive-delete --data DIRECTORY --backup-id ID\n"
         "Server data defaults to Data\\DropServer beside dropserver.exe.\n"
         "Without --certificate: HTTP + PAKE (port 8080). With --certificate: HTTPS (port 8443).\n"
         "Specify --data DIRECTORY to use another directory for any data command.\n"
         "Account, quota, status, cleanup, backup and existing Drive data commands support live administration.\n"
         "Passwords are entered only at the local masked console. No public admin endpoint.");
}
/* 将显式目录或 EXE 旁默认目录解析为确定路径，供前台和服务安装共用。 */
int drop_server_resolve_directory(const WCHAR *directory, WCHAR *path, size_t capacity) {
    DWORD length;
    if (!path || !capacity || capacity > 32768) return 0;
    if (directory) {
        if (!*directory || wcspbrk(directory, L"\"\r\n")) return 0;
        length = GetFullPathNameW(directory, (DWORD)capacity, path, NULL);
        return length && length < capacity;
    }
    return drop_program_data_directory(L"DropServer", path, capacity, 0);
}
/* 按服务器命令的参数布局查找名称对应的值，缺失返回空指针。 */
static const WCHAR *argument(int argc, WCHAR **argv, const WCHAR *key) {
    int i;
    for (i = 2; i + 1 < argc; i += 2)
        if (!wcscmp(argv[i], key)) return argv[i + 1];
    return NULL;
}
/* 遮蔽读取新密码并要求二次完全一致；不一致或取消清零两个缓冲，不保存秘密。 */
static int read_new_password(const WCHAR *prompt, size_t minimum, char *password, size_t capacity) {
    char confirmation[4096] = {0};
    size_t length;
    int result = 0;
    if (!password || !capacity || capacity > sizeof(confirmation)) return 0;
    sodium_memzero(password, capacity);
    if (!drop_password_read(prompt, password, capacity)) {
        fputs("Cannot read password: enter it in an interactive local console; redirected input is not supported.\n", stderr);
        goto done;
    }
    length = strlen(password);
    if (length < minimum || length > 1024 || !drop_utf8_valid(password, length)) {
        fprintf(stderr, "Invalid password: use %zu-1024 UTF-8 bytes.\n", minimum);
        goto done;
    }
    if (!drop_password_read(L"Confirm password: ", confirmation, sizeof(confirmation))) {
        fputs("Cannot read password confirmation. No changes were saved.\n", stderr);
        goto done;
    }
    if (strlen(confirmation) != length || sodium_memcmp(password, confirmation, length)) {
        fputs("Passwords do not match. No changes were saved.\n", stderr);
        goto done;
    }
    result = 1;
done:
    sodium_memzero(confirmation, sizeof(confirmation));
    if (!result) sodium_memzero(password, capacity);
    return result;
}
/* 将受支持的管理命令转换为有界请求；路径在调用者进程解析，密码仅在遮蔽控制台输入，在线失败不擅自降级写库。 */
static int managed_command(int argc, WCHAR **argv, const WCHAR *directory) {
    const WCHAR *operations[] = {L"account-create", L"account-enable", L"account-disable", L"account-reset", L"account-delete", L"account-display", L"account-list",
        L"device-revoke", L"quota-set", L"storage-status", L"cleanup", L"backup", L"recovery-export", L"drive-status", L"drive-list", L"drive-upload", L"drive-download",
        L"drive-delete"};
    cJSON *request = NULL, *response = NULL, *rows, *row;
    char *text = NULL, *buffer = NULL, password[4096] = {0}, operation[33];
    WCHAR *absolute = NULL;
    drop_server *server = NULL;
    drop_error error = DROP_E_ARGUMENT;
    size_t index;
    int i, j, mode, result = 2;
    for (index = 0; index < sizeof(operations) / sizeof(*operations); ++index)
        if (!wcscmp(argv[1], operations[index])) break;
    if (index == sizeof(operations) / sizeof(*operations)) return -1;
    request = cJSON_CreateObject();
    buffer = malloc(131072);
    absolute = malloc(32768 * sizeof(WCHAR));
    if (!request || !buffer || !absolute || !drop_to_utf8(argv[1], operation, sizeof(operation)) || !cJSON_AddStringToObject(request, "operation", operation)) goto done;
    for (i = 2; i + 1 < argc; i += 2) {
        const WCHAR *key = argv[i], *value = argv[i + 1];
        char name[33];
        int account = !wcsncmp(argv[1], L"account-", 8), path = !wcscmp(key, L"--target") || !wcscmp(key, L"--backup");
        int allowed = !wcscmp(key, L"--data") ||
            (((account && wcscmp(argv[1], L"account-list")) || !wcscmp(argv[1], L"quota-set") || !wcscmp(argv[1], L"device-revoke")) && !wcscmp(key, L"--login"));
        allowed |= (!wcscmp(argv[1], L"account-create") || !wcscmp(argv[1], L"account-display")) && !wcscmp(key, L"--display");
        allowed |= !wcscmp(argv[1], L"account-list") && !wcscmp(key, L"--after");
        allowed |= !wcscmp(argv[1], L"quota-set") && !wcscmp(key, L"--bytes");
        allowed |= !wcscmp(argv[1], L"device-revoke") && !wcscmp(key, L"--device");
        allowed |= (!wcscmp(argv[1], L"backup") || !wcscmp(argv[1], L"recovery-export") || !wcscmp(argv[1], L"drive-download")) && !wcscmp(key, L"--target");
        allowed |= !wcscmp(argv[1], L"drive-upload") && !wcscmp(key, L"--backup");
        allowed |= (!wcscmp(argv[1], L"drive-download") || !wcscmp(argv[1], L"drive-delete")) && !wcscmp(key, L"--backup-id");
        if (!allowed) goto done;
        for (j = 2; j < i; j += 2)
            if (!wcscmp(argv[j], key)) goto done;
        if (!wcscmp(key, L"--data")) continue;
        if (path) {
            DWORD length = GetFullPathNameW(value, 32768, absolute, NULL);
            if (!length || length >= 32768) goto done;
            value = absolute;
        }
        if (!drop_to_utf8(key + 2, name, sizeof(name)) || !drop_to_utf8(value, buffer, 131072) || !cJSON_AddStringToObject(request, name, buffer)) goto done;
    }
    if ((!strncmp(operation, "account-", 8) && strcmp(operation, "account-list")) || !strcmp(operation, "device-revoke") || !strcmp(operation, "quota-set")) {
        if (!drop_account_login_valid(drop_json_string(request, "login", 64))) {
            fputs("Invalid account name: use 3-64 ASCII letters (A-Z, a-z), digits, or _ . -; spaces and Chinese characters are not allowed.\n", stderr);
            result = 1;
            goto done;
        }
    }
    if ((!strcmp(operation, "account-display") || cJSON_HasObjectItem(request, "display")) &&
        (!drop_json_string(request, "display", 128) || !*drop_json_string(request, "display", 128))) {
        fputs("Invalid display name: use 1-128 UTF-8 bytes.\n", stderr);
        result = 1;
        goto done;
    }
    if (!strcmp(operation, "account-create") || !strcmp(operation, "account-reset") || !strcmp(operation, "recovery-export")) {
        int recovery = !strcmp(operation, "recovery-export");
        if (!read_new_password(
                recovery ? L"Independent recovery password (16+ UTF-8 bytes): " : L"Account password (12+ UTF-8 bytes): ", recovery ? 16 : 12, password, sizeof(password)) ||
            !cJSON_AddStringToObject(request, "password", password)) {
            result = 1;
            goto done;
        }
        sodium_memzero(password, sizeof(password));
    }
    mode = drop_admin_local(directory, request, &response);
    if (mode == -1) {
        server = calloc(1, sizeof(*server));
        if (!server || !drop_server_open(server, directory, 0)) {
            fputs("Cannot open server data. Check the directory and local permission.\n", stderr);
            result = 1;
            goto done;
        }
        error = drop_admin_execute(server, request, &response);
        drop_server_close(server);
        free(server);
        server = NULL;
    } else if (mode == 1) {
        const char *name = drop_json_string(response, "error", 64);
        for (j = DROP_OK; j <= DROP_E_PENDING; ++j)
            if (name && !strcmp(name, drop_error_name((drop_error)j))) break;
        error = j <= DROP_E_PENDING ? (drop_error)j : DROP_E_PROTOCOL;
    } else {
        fputs("Live management failed: check local permission and the running server. Check account-list before retrying an uncertain operation.\n", stderr);
        result = 1;
        goto done;
    }
    result = error == DROP_OK ? 0 : 1;
    if (error != DROP_OK) {
        if (!strncmp(operation, "drive-", 6))
            printf("Drive: %s\n", drop_error_name(error));
        else if (error == DROP_E_CONFLICT)
            fputs("Account already exists: use the existing account or reset its password.\n", stderr);
        else
            fprintf(stderr, "Management failed: %s. Check parameters, account and server data.\n", drop_error_name(error));
        goto done;
    }
    rows = cJSON_GetObjectItemCaseSensitive(response, "rows");
    if (!strcmp(operation, "storage-status") && (row = cJSON_GetArrayItem(rows, 0)))
        printf("Text records: %s\nText zstd: %s\n", cJSON_GetArrayItem(row, 0)->valuestring, cJSON_GetArrayItem(row, 1)->valuestring);
    else if (!strcmp(operation, "drive-status")) {
        cJSON_ArrayForEach(row, rows)
            printf("backup=%s state=%s error=%s files=%s/%s completed_at=%s\n", cJSON_GetArrayItem(row, 0)->valuestring, cJSON_GetArrayItem(row, 1)->valuestring,
                cJSON_GetArrayItem(row, 2)->valuestring, cJSON_GetArrayItem(row, 3)->valuestring, cJSON_GetArrayItem(row, 4)->valuestring, cJSON_GetArrayItem(row, 5)->valuestring);
    } else if (!strcmp(operation, "drive-list")) {
        cJSON *files = cJSON_GetObjectItemCaseSensitive(response, "files");
        cJSON_ArrayForEach(row, files) {
            const char *id = drop_json_string(cJSON_GetObjectItemCaseSensitive(row, "appProperties"), "dropBackup", 32);
            if (drop_id_valid(id)) printf("backup=%s\n", id);
        }
        puts("Drive: ok");
    } else if (!strcmp(operation, "account-list")) {
        text = cJSON_PrintUnformatted(response);
        if (!text) {
            result = 1;
            goto done;
        }
        puts(text);
    } else if (!strcmp(operation, "backup"))
        printf("Encrypted backup complete: %s\n", drop_json_string(response, "backup_id", 32));
    else if (!strncmp(operation, "drive-", 6))
        puts("Drive: ok");
    else if (!strcmp(operation, "account-create"))
        puts("Account created.");
    else if (!strcmp(operation, "recovery-export"))
        puts("Protected portable recovery package created. Keep the password separately.");
    else
        puts(drop_json_string(response, "message", 256));
done:
    if (result == 2) fputs("Invalid management parameters or password confirmation. No operation was submitted.\n", stderr);
    sodium_memzero(password, sizeof(password));
    if (text) cJSON_free(text);
    free(buffer);
    free(absolute);
    free(server);
    drop_admin_json_free(request);
    drop_admin_json_free(response);
    return result;
}
/* 校验管理参数并分派初始化、账号、备份、恢复和运行命令，统一清理密码及服务器资源。 */
int drop_server_command(int argc, WCHAR **argv) {
    drop_server server;
    const WCHAR *directory, *parameter;
    char password[4096], certificate[131072], listen[256];
    WCHAR resolved_directory[32768];
    int result = 0, i, j;
    if (!drop_init()) return 1;
    if (argc == 2 && !wcscmp(argv[1], L"--version")) {
        puts(DROP_VERSION);
        return 0;
    }
#if DROP_DIAGNOSTICS
    if (argc == 3 && !wcscmp(argv[1], L"--test-backup")) return drop_backup_test(argv[2]) ? 0 : 1;
    if (argc == 3 && !wcscmp(argv[1], L"--self-test"))
        return drop_database_test(argv[2]) && drop_migration_test(argv[2]) && drop_maintenance_test(argv[2]) && drop_backup_test(argv[2]) ? 0 : 1;
#endif
    if (!DROP_DIAGNOSTICS && argc > 1 && !wcsncmp(argv[1], L"--", 2)) return 2;
    if (argc < 2 || argc % 2 != 0) {
        usage();
        return 2;
    }
    if (!drop_server_resolve_directory(argument(argc, argv, L"--data"), resolved_directory, 32768)) {
        fputs("Cannot resolve server data directory. Specify a valid --data DIRECTORY.\n", stderr);
        return 2;
    }
    directory = resolved_directory;
    if (!wcscmp(argv[1], L"data-directory")) {
        char *text;
        if (argc != 2 && (argc != 4 || wcscmp(argv[2], L"--data"))) return 2;
        text = malloc(131072);
        if (!text) return 1;
        result = drop_to_utf8(directory, text, 131072);
        if (result) puts(text);
        free(text);
        return result ? 0 : 1;
    }
    if (!wcscmp(argv[1], L"backup-trigger")) {
        if (argc != 2 && (argc != 4 || wcscmp(argv[2], L"--data"))) return 2;
        result = drop_backup_trigger(directory);
        puts(result ? "Backup requested; completion is reported separately by the running server."
                    : "Backup request failed; check server backup configuration and local permission.");
        return result ? 0 : 1;
    }
    result = managed_command(argc, argv, directory);
    if (result != -1) return result;
    if (!wcsncmp(argv[1], L"drive-", 6)
#if DROP_DIAGNOSTICS
        || !wcscmp(argv[1], L"--test-drive-authorize")
#endif
    ) {
        if (wcscmp(argv[1], L"drive-authorize") && wcscmp(argv[1], L"drive-upload") && wcscmp(argv[1], L"drive-download") && wcscmp(argv[1], L"drive-delete") &&
            wcscmp(argv[1], L"drive-list") && wcscmp(argv[1], L"drive-status")
#if DROP_DIAGNOSTICS
            && wcscmp(argv[1], L"--test-drive-authorize")
#endif
        )
            return 2;
        for (i = 2; i + 1 < argc; i += 2) {
            int allowed = !wcscmp(argv[i], L"--data") || (!wcscmp(argv[1], L"drive-authorize") && !wcscmp(argv[i], L"--oauth-client")) ||
                (!wcscmp(argv[1], L"drive-upload") && !wcscmp(argv[i], L"--backup")) ||
                ((!wcscmp(argv[1], L"drive-download") || !wcscmp(argv[1], L"drive-delete")) && !wcscmp(argv[i], L"--backup-id")) ||
                (!wcscmp(argv[1], L"drive-download") && !wcscmp(argv[i], L"--target"));
#if DROP_DIAGNOSTICS
            allowed |= !wcscmp(argv[1], L"--test-drive-authorize") && (!wcscmp(argv[i], L"--origin") || !wcscmp(argv[i], L"--ca"));
#endif
            if (!allowed) return 2;
            for (j = 2; j < i; j += 2)
                if (!wcscmp(argv[i], argv[j])) return 2;
        }
        if (!drop_server_open(&server, directory, 0)) return 1;
        result = drop_cloud_command(&server, argc, argv);
        drop_server_close(&server);
        return result ? 0 : 1;
    }
    if (wcscmp(argv[1], L"init") && wcscmp(argv[1], L"serve") && wcscmp(argv[1], L"service-run") && wcscmp(argv[1], L"runtime-machine") && wcscmp(argv[1], L"restore") &&
        wcscmp(argv[1], L"--test-recovery-export") && wcscmp(argv[1], L"--test-restore")) {
        usage();
        return 2;
    }
    for (i = 2; i + 1 < argc; i += 2) {
        int allowed = !wcscmp(argv[i], L"--data") || (!wcscmp(argv[1], L"--test-recovery-export") && !wcscmp(argv[i], L"--target")) ||
            ((!wcscmp(argv[1], L"restore") || !wcscmp(argv[1], L"--test-restore")) && (!wcscmp(argv[i], L"--backup") || !wcscmp(argv[i], L"--package"))) ||
            ((!wcscmp(argv[1], L"serve") || !wcscmp(argv[1], L"service-run")) &&
                (!wcscmp(argv[i], L"--listen") || !wcscmp(argv[i], L"--certificate") || !wcscmp(argv[i], L"--config") || !wcscmp(argv[i], L"--backup-directory") ||
                    !wcscmp(argv[i], L"--backup-seconds") || (!wcscmp(argv[1], L"service-run") && !wcscmp(argv[i], L"--name"))));
        if (!allowed) {
            usage();
            return 2;
        }
        for (j = 2; j < i; j += 2)
            if (!wcscmp(argv[i], argv[j])) {
                usage();
                return 2;
            }
    }
    if (!wcscmp(argv[1], L"restore") || !wcscmp(argv[1], L"--test-restore")) {
        const WCHAR *backup = argument(argc, argv, L"--backup"), *package = argument(argc, argv, L"--package");
#if DROP_DIAGNOSTICS
        if (!wcscmp(argv[1], L"--test-restore"))
            strcpy_s(password, sizeof(password), "independent-recovery-fixture-password-123");
        else
#endif
            if (!drop_password_read(L"Recovery password: ", password, sizeof(password)))
            return 1;
        result = backup && package && drop_backup_restore(backup, package, directory, password);
        sodium_memzero(password, sizeof(password));
        puts(result ? "Restore complete; server identity retained, sync generation changed." : "Restore failed; no server started.");
        return result ? 0 : 1;
    }
    if (!drop_server_open(&server, directory, !wcscmp(argv[1], L"init"))) {
        fputs("Cannot open encrypted server state: check directory, key identity, and whether another process owns it.\n", stderr);
        return 1;
    }
    if (!wcscmp(argv[1], L"init")) {
        puts("Encrypted server initialized. Runtime keys protected for this Windows user.");
        result = 1;
    } else if (!wcscmp(argv[1], L"runtime-machine")) {
        result = drop_runtime_machine(&server);
        if (result) puts("Runtime keys migrated to machine DPAPI; directory restricted to SYSTEM and administrators.");
    } else if (!wcscmp(argv[1], L"--test-recovery-export")) {
#if DROP_DIAGNOSTICS
        if (!wcscmp(argv[1], L"--test-recovery-export"))
            strcpy_s(password, sizeof(password), "independent-recovery-fixture-password-123");
        else
#endif
            if (!read_new_password(L"Independent recovery password (16+ UTF-8 bytes): ", 16, password, sizeof(password)))
            goto cleanup;
        parameter = argument(argc, argv, L"--target");
        result = parameter && drop_recovery_export(&server, parameter, password);
        sodium_memzero(password, sizeof(password));
        if (result) puts("Protected portable recovery package created. Keep the password separately.");
    } else {
        parameter = argument(argc, argv, L"--backup-directory");
        if (parameter) {
            DWORD n = GetFullPathNameW(parameter, 32768, server.backup_directory, NULL);
            if (!n || n >= 32768) goto cleanup;
        }
        parameter = argument(argc, argv, L"--backup-seconds");
        if (parameter) {
            char number[32];
            uint64_t seconds;
            if (!*server.backup_directory || !drop_to_utf8(parameter, number, sizeof(number)) || !drop_u64_parse(number, &seconds) || seconds > 604800 || (seconds && seconds < 60))
                goto cleanup;
            server.backup_seconds = (unsigned)seconds;
        }
        parameter = argument(argc, argv, L"--config");
        if (parameter) {
            drop_config *config = malloc(sizeof(*config));
            if (!config || !drop_config_load(parameter, config)) {
                free(config);
                goto cleanup;
            }
            server.use_zstd = config->use_zstd;
            server.zstd_level = config->zstd_level;
            server.connection_timeout_seconds = config->connection_timeout_seconds;
            free(config);
        }
        parameter = argument(argc, argv, L"--certificate");
        certificate[0] = 0;
        if (parameter && (!*parameter || !drop_to_utf8(parameter, certificate, sizeof(certificate)))) goto cleanup;
        parameter = argument(argc, argv, L"--listen");
        if (!parameter)
            strcpy_s(listen, sizeof(listen), *certificate ? "127.0.0.1:8443" : "127.0.0.1:8080");
        else if (!drop_to_utf8(parameter, listen, sizeof(listen)))
            goto cleanup;
        result = drop_server_run(&server, listen, *certificate ? certificate : NULL);
    }
cleanup:
    drop_server_close(&server);
    if (!result) fputs("Operation failed.\n", stderr);
    return result ? 0 : 1;
}
/* 初始化公共加密库，优先处理 Windows 服务命令，再进入普通服务器命令分派。 */
int wmain(int argc, WCHAR **argv) {
    if (!drop_init()) return 1;
    if (argc > 1 && !wcsncmp(argv[1], L"service-", 8)) return drop_service_command(argc, argv);
    return drop_server_command(argc, argv);
}
