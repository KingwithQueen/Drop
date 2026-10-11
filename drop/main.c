/* 客户端原生入口及受构建开关控制的诊断命令。 */
#include "client.h"
#include "ui_data.h"
#include <shellapi.h>
#include <sodium.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* 分派版本、目录及诊断命令；测试入口受构建开关控制，诊断运行使用明确指定的数据目录。 */
static int diagnostic_main(int argc, WCHAR **argv) {
    if (!drop_init()) return 1;
    if (argc == 2 && !wcscmp(argv[1], L"--version")) {
        puts(DROP_VERSION);
        return 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"data-directory")) {
        WCHAR path[32768];
        char utf8[131072];
        if (!drop_program_data_directory(L"Drop", path, 32768, 0) || !drop_to_utf8(path, utf8, sizeof(utf8))) return 1;
        puts(utf8);
        return 0;
    }
#if DROP_DIAGNOSTICS
    if (argc == 3 && !wcscmp(argv[1], L"--self-test")) {
        sqlite3 *database = NULL;
        if (!drop_common_test(argv[2]) || !drop_client_test(argv[2]) || sqlite3_open(":memory:", &database) != SQLITE_OK) return 1;
        if (sqlite3_exec(database, "CREATE TABLE local_probe(id TEXT PRIMARY KEY);", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_close(database);
            return 1;
        }
        sqlite3_close(database);
        puts("PASS client: native SQLite loaded");
        return 0;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--test-space-sync")) {
        drop_client client;
        sqlite3_stmt *statement = NULL;
        sqlite3_int64 before_count = 0;
        char before_cursor[32], limit[96];
        int ok = 0;
        if (!drop_client_open(&client, argv[2])) return 1;
        if (sqlite3_prepare_v2(client.database, "SELECT (SELECT count(*) FROM messages),(SELECT value FROM meta WHERE key='cursor'),(SELECT page_count FROM pragma_page_count)", -1,
                &statement, NULL) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW)
            goto space_done;
        before_count = sqlite3_column_int64(statement, 0);
        strcpy_s(before_cursor, sizeof(before_cursor), (const char *)sqlite3_column_text(statement, 1));
        sprintf_s(limit, sizeof(limit), "PRAGMA max_page_count=%lld", sqlite3_column_int64(statement, 2));
        sqlite3_finalize(statement);
        statement = NULL;
        if (sqlite3_exec(client.database, limit, NULL, NULL, NULL) != SQLITE_OK || drop_client_sync(&client) != DROP_E_SPACE) goto space_done;
        if (sqlite3_prepare_v2(client.database, "SELECT (SELECT count(*) FROM messages),(SELECT value FROM meta WHERE key='cursor')", -1, &statement, NULL) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW)
            ok = sqlite3_column_int64(statement, 0) == before_count && !strcmp(before_cursor, (const char *)sqlite3_column_text(statement, 1));
    space_done:
        sqlite3_finalize(statement);
        sqlite3_exec(client.database, "PRAGMA max_page_count=4294967294", NULL, NULL, NULL);
        drop_client_close(&client);
        puts(ok ? "PASS sync: space error retains whole page and cursor" : "FAIL sync space rollback");
        return ok ? 0 : 1;
    }
#endif
    if (!DROP_DIAGNOSTICS && argc > 1 && !wcsncmp(argv[1], L"--test-", 7)) return 2;
    if ((argc == 5 || argc == 6) && (!wcscmp(argv[1], L"client-login") || !wcscmp(argv[1], L"--test-login"))) {
        char login[65], password[4096];
        int result;
        if (!drop_to_utf8(argv[4], login, sizeof(login))) return 2;
#if DROP_DIAGNOSTICS
        if (!wcscmp(argv[1], L"--test-login")) {
            if (!strcmp(login, "test-user"))
                strcpy_s(password, sizeof(password), "test-only-password-123");
            else if (!strcmp(login, "test-other"))
                strcpy_s(password, sizeof(password), "test-only-other-password-123");
            else
                return 2;
        } else
#endif
            if (!drop_password_read(L"Account password: ", password, sizeof(password)))
            return 1;
        result = drop_client_login(argv[3], argv[2], login, password, argc == 6 ? argv[5] : NULL);
        sodium_memzero(password, sizeof(password));
        puts(result ? "Login: protected session saved" : "Login: failed");
        return result ? 0 : 1;
    }
    if ((argc == 3 && (!wcscmp(argv[1], L"client-sync") || !wcscmp(argv[1], L"client-send") || !wcscmp(argv[1], L"client-watch") || !wcscmp(argv[1], L"client-retry-reviewed"))) ||
        ((argc == 4 || argc == 5) && !wcscmp(argv[1], L"client-queue")) || (argc == 4 && !wcscmp(argv[1], L"client-read")) ||
        (argc >= 6 && argc <= 5 + DROP_MAX_ATTACHMENTS && !wcscmp(argv[1], L"client-attach"))) {
        drop_client client;
        drop_error error = DROP_OK;
        if (!drop_client_open(&client, argv[2])) {
            puts("Client: state unavailable or already running");
            return 1;
        }
        if (!wcscmp(argv[1], L"client-sync"))
            error = drop_client_sync(&client);
        else if (!wcscmp(argv[1], L"client-retry-reviewed"))
            error = drop_client_retry_review(&client);
        else if (!wcscmp(argv[1], L"client-send")) {
            ULONGLONG deadline = GetTickCount64() + 3600000;
            do {
                error = drop_client_flush(&client);
            } while (error == DROP_E_PENDING && GetTickCount64() < deadline && drop_network_wait(1000));
            if (error == DROP_OK) error = drop_client_sync(&client);
        } else if (!wcscmp(argv[1], L"client-watch"))
            error = drop_client_watch(&client) ? DROP_OK : DROP_E_NETWORK;
        else if (!wcscmp(argv[1], L"client-read")) {
            char id[DROP_ID_CAP];
            error = drop_to_utf8(argv[3], id, sizeof(id)) && drop_client_read_queue(&client, id) ? drop_client_receipts(&client, 0) : DROP_E_ARGUMENT;
        } else if (!wcscmp(argv[1], L"client-attach")) {
            unsigned char *body = NULL;
            DWORD size = 0;
            char recipient[DROP_ID_CAP], request[DROP_ID_CAP];
            const char *to = NULL;
            if (wcscmp(argv[4], L"self")) {
                if (!drop_to_utf8(argv[4], recipient, sizeof(recipient)) || !drop_id_valid(recipient))
                    error = DROP_E_ARGUMENT;
                else
                    to = recipient;
            }
            if (error == DROP_OK) {
                if (!drop_file_read(argv[3], &body, &size, DROP_MAX_BODY) || !drop_utf8_valid((char *)body, size))
                    error = DROP_E_ARGUMENT;
                else
                    error = drop_client_queue_files(&client, (char *)body, to, (const WCHAR *const *)(argv + 5), argc - 5, request);
            }
            free(body);
        } else {
            unsigned char *body = NULL;
            DWORD size = 0;
            char recipient[DROP_ID_CAP], request[DROP_ID_CAP];
            if (argc == 5 && (!drop_to_utf8(argv[4], recipient, sizeof(recipient)) || !drop_id_valid(recipient)))
                error = DROP_E_ARGUMENT;
            else if (!drop_file_read(argv[3], &body, &size, DROP_MAX_BODY) || !drop_utf8_valid((char *)body, size))
                error = DROP_E_ARGUMENT;
            else
                error = drop_client_queue(&client, (char *)body, argc == 5 ? recipient : NULL, request);
            free(body);
        }
        {
            uint64_t received = client.downloaded_bytes;
            drop_client_close(&client);
            printf("Client: %s\nDownloaded bytes: %llu\n", drop_error_name(error), (unsigned long long)received);
        }
        return error == DROP_OK ? 0 : 1;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--probe")) return drop_network_probe(argv[2]) ? 0 : 1;
    if (argc == 3 && !wcscmp(argv[1], L"--check-config")) {
        drop_config config;
        if (!drop_config_load(argv[2], &config)) {
            puts("Configuration: invalid_argument");
            return 1;
        }
        printf("Configuration: server origin valid; heartbeat=%u timeout=%u reconcile=%u compression=%s level=%d\n", config.heartbeat_seconds, config.connection_timeout_seconds,
            config.reconcile_seconds, config.use_zstd ? "zstd" : "none", config.zstd_level);
        return 0;
    }
    puts("Drop " DROP_VERSION " (development diagnostic entry)\n"
         "  drop --version\n  drop --self-test TEST_DIRECTORY\n  drop --probe https://HOST:PORT\n  drop --check-config FILE\n"
         "  drop client-login https://HOST:PORT DATA_DIRECTORY LOGIN [CA_PEM]\n"
         "  drop client-queue DATA_DIRECTORY UTF8_BODY_FILE [RECIPIENT_ID]\n"
         "  drop client-attach DATA_DIRECTORY UTF8_BODY_FILE self|RECIPIENT_ID FILE...\n"
         "  drop client-send DATA_DIRECTORY\n  drop client-sync DATA_DIRECTORY\n  drop client-watch DATA_DIRECTORY\n"
         "  drop --ui-root DATA_DIRECTORY [--background]\n  drop --background");
    return argc == 1 ? 0 : 2;
}

/* 解析原生启动参数，选择托盘界面或命令入口，并在使用共享工具前初始化加密库。 */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    WCHAR **arguments;
    int count, result;
    (void)previous;
    (void)command;
    (void)show;
    if (!drop_init()) return 1;
    arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 1;
    if (count == 1 || (count == 2 && !wcscmp(arguments[1], L"--background")))
        result = drop_ui_run(instance, NULL, count == 2);
    else if ((count == 3 || count == 4) && !wcscmp(arguments[1], L"--ui-root") && (count == 3 || !wcscmp(arguments[3], L"--background")))
        result = drop_ui_run(instance, arguments[2], count == 4);
    else {
        if (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) != FILE_TYPE_CHAR) (void)AttachConsole(ATTACH_PARENT_PROCESS);
        result = diagnostic_main(count, arguments);
    }
    LocalFree(arguments);
    return result;
}
