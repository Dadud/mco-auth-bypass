/**
 * authlogin.c - Motor City Online authlib reference implementation
 *
 * This file is a *reference* for replacing EA's authlogin.dll. It is
 * NOT a complete drop-in: it implements only the authlib interface
 * (the bit the game calls for a ticket). The rest of the NPS /
 * MCOTS / server flow is your project.
 *
 * What this file does:
 *   - Exports the same symbols EA's authlogin.dll exposes.
 *   - GetTicketSync returns a hardcoded ticket. The game trusts it
 *     and moves on to NPS as if EA's auth server had issued a real
 *     one.
 *
 * What this file deliberately does NOT do:
 *   - No network code. No socket. No server.
 *   - No NPS / MCOTS / DES / RSA / RC4 / DCL.
 *   - No ticket validation. The DLL is trusted by the game; what
 *     happens after it hands over the ticket is the server's job.
 *
 * The point: a developer can read this 100-line file in 5 minutes
 * and see exactly what the authlib interface looks like, what the
 * game expects back, and where to plug in their own server's
 * ticket logic. The rest of the MCO protocol (login server on
 * port 8226, NPS handshake, MCOTS) is documented in
 * Dadud/Motor-City-Online-RE/docs/research/NETWORK_PROTOCOL.md
 * and is the responsibility of your server project.
 *
 * Build (cross-compile from Linux/macOS):
 *   i686-w64-mingw32-gcc -shared -o authlogin.dll authlogin.c \
 *       -static -Wl,--subsystem,windows
 *
 * On Windows with MinGW-w64 (WinLibs, MSYS2, etc.):
 *   gcc -shared -o authlogin.dll authlogin.c \
 *       -static -Wl,--subsystem,windows
 *
 * Install (drop the DLL into the game's update directory):
 *   ./install.sh /path/to/MCO/update
 *
 * License: MIT (see LICENSE).
 */

#include <windows.h>
#include <string.h>

/* ------------------------------------------------------------------
 * TICKET
 * ------------------------------------------------------------------
 * The ticket string the DLL hands back to the game. Anything works
 * for offline play; for a real server, this is where you'd POST
 * credentials somewhere and write whatever the server issues.
 *
 * The game doesn't validate the ticket format - it just stores it
 * and sends it to the login server (port 8226) on the next
 * connection. So your server's ticket validator is the only thing
 * that has to parse this string.
 */
static const char* OFFLINE_TICKET = "MCO1OFFLINE0000";

/* ------------------------------------------------------------------
 * EA authlib interface
 * ------------------------------------------------------------------
 * These are the 9 functions EA's authlogin.dll exports. The game
 * loads them by name; if any are missing or have the wrong
 * signature, the game won't start. Keep this list in sync with
 * NETWORK_PROTOCOL.md and OpenMCO's `tools/auth/authlogin.c` (the
 * original source this file is derived from).
 */

__declspec(dllexport)
int WINAPI GetTicketSync(const char* username, const char* password,
                         char* outTicket, int* outReasonCode)
{
    /* The game calls this when it wants a login ticket. We don't
     * care about the credentials; we hand back a hardcoded string.
     * For a real auth flow, replace this body with a network call
     * to your server's login endpoint. */
    if (outTicket != NULL) {
        strncpy(outTicket, OFFLINE_TICKET, strlen(OFFLINE_TICKET) + 1);
    }
    if (outReasonCode != NULL) *outReasonCode = 0;  /* 0 = success */
    return 1;  /* TRUE */
}

__declspec(dllexport)
int WINAPI GetTicketSyncA(const char* username, const char* password,
                          char* outTicket, int* outReasonCode)
{
    return GetTicketSync(username, password, outTicket, outReasonCode);
}

__declspec(dllexport)
int WINAPI GetTicketSyncW(const WCHAR* username, const WCHAR* password,
                          WCHAR* outTicket, int* outReasonCode)
{
    if (outTicket != NULL) {
        /* Trivial ANSI->Wide copy of the hardcoded ticket. */
        char ansi[64];
        strncpy(ansi, OFFLINE_TICKET, sizeof(ansi));
        MultiByteToWideChar(CP_ACP, 0, ansi, -1, outTicket, 64);
    }
    if (outReasonCode != NULL) *outReasonCode = 0;
    return 1;
}

__declspec(dllexport) int WINAPI AuthLogin_Init(void)         { return 1; }
__declspec(dllexport) int WINAPI AuthLogin_Shutdown(void)     { return 1; }
__declspec(dllexport) int WINAPI AuthLogin_GetLastError(void) { return 0; }

__declspec(dllexport)
const char* WINAPI AuthLogin_GetServerName(void)
{
    /* Return EA's server name so the game doesn't notice the auth
     * layer is gone. Your server is reached on the NPS ports
     * (8226 login, 7003 lobby, 43300 MCOTS) via whatever redirect
     * mechanism your server project uses (registry edit, hosts
     * file, etc.) - this DLL has no opinion on that. */
    return "www.ea.com";
}

__declspec(dllexport)
void WINAPI AuthLogin_SetServerName(const char* serverName) { /* no-op */ }

__declspec(dllexport) int WINAPI AuthLogin_GetVersion(void)   { return 1; }

/* ------------------------------------------------------------------
 * DLL entry point
 * ------------------------------------------------------------------
 * Nothing to initialize. The game will call GetTicketSync the first
 * time it needs a ticket; we just answer.
 */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            break;
    }
    return TRUE;
}
