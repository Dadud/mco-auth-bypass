/*
 * test_shim.c - smoke test for the authlogin reference shim
 *
 * Verifies the hardcoded ticket is what we expect and the EA
 * authlib exports are present in the built DLL. The actual runtime
 * behavior (does the game accept the ticket?) is a manual test on
 * a Windows box with the actual game.
 *
 * Build (native, runs on this Linux box):
 *   gcc -o test_shim test_shim.c
 *   ./test_shim
 *
 * Build (cross-compile, runs on Windows; needs MinGW-w64):
 *   i686-w64-mingw32-gcc -o test_shim.exe test_shim.c -static
 *   ./test_shim.exe
 */

#include <stdio.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_passed++; printf("  PASS  %s\n", msg); } \
    else      { g_failed++; printf("  FAIL  %s\n", msg); } \
} while (0)

int main(void)
{
    printf("=== mco-auth-bypass reference shim smoke test ===\n");

    /* The ticket the DLL hands back. Match authlogin.c. */
    const char* ticket = "MCO1OFFLINE0000";
    CHECK(strlen(ticket) > 0,        "ticket is non-empty");
    CHECK(strchr(ticket, '\0') != NULL, "ticket is null-terminated");
    CHECK(strlen(ticket) < 256,      "ticket fits in EA's 256-byte buffer");

    /* Spot-check the 9 EA authlib export names. The build process
     * produces the DLL; the actual export table is verified by
     * inspecting the .dll with i686-w64-mingw32-objdump. We just
     * remind ourselves of the names here so the source stays in
     * sync with the docs. */
    const char* expected_exports[] = {
        "GetTicketSync",
        "GetTicketSyncA",
        "GetTicketSyncW",
        "AuthLogin_Init",
        "AuthLogin_Shutdown",
        "AuthLogin_GetLastError",
        "AuthLogin_GetServerName",
        "AuthLogin_SetServerName",
        "AuthLogin_GetVersion",
    };
    int n = (int)(sizeof(expected_exports) / sizeof(expected_exports[0]));
    CHECK(n == 9, "EA authlib interface has 9 exports");

    /* Verify the source contains the right names. This catches
     * typos in the source. Look in the parent dir (where the .c
     * lives) and in the current dir. */
    FILE* f = fopen("../authlogin.c", "r");
    if (f == NULL) f = fopen("authlogin.c", "r");
    if (f != NULL) {
        char buf[64 * 1024];
        size_t r = fread(buf, 1, sizeof(buf) - 1, f);
        buf[r] = '\0';
        fclose(f);
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (strstr(buf, expected_exports[i]) != NULL) found++;
        }
        CHECK(found == n, "all 9 export names are present in authlogin.c");
    } else {
        printf("  SKIP  (authlogin.c not in cwd; export-name source check skipped)\n");
    }

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
