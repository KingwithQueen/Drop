#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    puts("Drop server " DROP_VERSION " (development)\n"
        "  dropserver init --data DIRECTORY\n"
        "  dropserver account-create --data DIRECTORY --login NAME [--display NAME]\n"
        "  dropserver serve --data DIRECTORY --certificate PEM [--listen 127.0.0.1:8443]\n"
        "  dropserver --self-test TEST_DIRECTORY\n"
        "Passwords are entered only at the local masked console. No public admin endpoint.");
}
static const WCHAR *argument(int argc, WCHAR **argv, const WCHAR *key) {
    int i; for (i = 2; i + 1 < argc; i += 2) if (!wcscmp(argv[i], key)) return argv[i+1]; return NULL;
}
int wmain(int argc, WCHAR **argv) {
    drop_server server; const WCHAR *directory, *parameter; char login[256], display[512], password[4096], certificate[131072], listen[256];
    int result = 0;
    if (!drop_init()) return 1;
    if (argc == 2 && !wcscmp(argv[1], L"--version")) { puts(DROP_VERSION); return 0; }
    if (argc == 3 && !wcscmp(argv[1], L"--self-test")) return drop_database_test(argv[2]) ? 0 : 1;
    if (argc < 4 || argc % 2 != 0 || !(directory = argument(argc, argv, L"--data"))) { usage(); return 2; }
    if (wcscmp(argv[1], L"init") && wcscmp(argv[1], L"account-create") && wcscmp(argv[1], L"serve")) { usage(); return 2; }
    if (!drop_server_open(&server, directory, !wcscmp(argv[1], L"init"))) {
        fputs("Cannot open encrypted server state: check directory, key identity, and whether another process owns it.\n", stderr); return 1;
    }
    if (!wcscmp(argv[1], L"init")) { puts("Encrypted server initialized. Runtime keys protected for this Windows user."); result = 1; }
    else if (!wcscmp(argv[1], L"account-create")) {
        parameter = argument(argc, argv, L"--login");
        if (parameter && drop_to_utf8(parameter, login, sizeof(login))) {
            parameter = argument(argc, argv, L"--display");
            if (!parameter) strcpy_s(display, sizeof(display), login);
            else if (!drop_to_utf8(parameter, display, sizeof(display))) goto cleanup;
            if (drop_password_read(L"Account password (12+ UTF-8 bytes): ", password, sizeof(password))) {
                result = drop_account_create(&server, login, display, password); sodium_memzero(password, sizeof(password));
                if (result) puts("Account created.");
            }
        }
    } else {
        parameter = argument(argc, argv, L"--certificate");
        if (!parameter || !drop_to_utf8(parameter, certificate, sizeof(certificate))) goto cleanup;
        parameter = argument(argc, argv, L"--listen");
        if (!parameter) strcpy_s(listen, sizeof(listen), "127.0.0.1:8443");
        else if (!drop_to_utf8(parameter, listen, sizeof(listen))) goto cleanup;
        result = drop_server_run(&server, listen, certificate);
    }
cleanup:
    drop_server_close(&server); if (!result) fputs("Operation failed.\n", stderr); return result ? 0 : 1;
}
