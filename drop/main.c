#include "network.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int wmain(int argc, WCHAR **argv) {
    if (!drop_init()) return 1;
    if (argc == 2 && !wcscmp(argv[1], L"--version")) { puts(DROP_VERSION); return 0; }
    if (argc == 3 && !wcscmp(argv[1], L"--self-test")) {
        sqlite3 *database = NULL;
        if (!drop_common_test(argv[2]) || sqlite3_open(":memory:", &database) != SQLITE_OK) return 1;
        if (sqlite3_exec(database, "CREATE TABLE local_probe(id TEXT PRIMARY KEY);", NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(database); return 1; }
        sqlite3_close(database); puts("PASS client: native SQLite loaded"); return 0;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--probe")) return drop_network_probe(argv[2]) ? 0 : 1;
    puts("Drop " DROP_VERSION " (development diagnostic entry)\n"
        "  drop --version\n  drop --self-test TEST_DIRECTORY\n  drop --probe https://HOST:PORT\n"
        "Tray UI and synchronization are scheduled after the server foundation.");
    return argc == 1 ? 0 : 2;
}
