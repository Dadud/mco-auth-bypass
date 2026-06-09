/**
 * authlogin.dll - Replacement authentication DLL for Motor City Online
 *
 * This DLL bypasses EA's authentication servers by returning a valid
 * offline ticket without making any network requests.
 *
 * Compatible with the original authlogin.dll exports while preserving
 * any anti-cheat hooks that may be present.
 *
 * Compile with:
 *   gcc -shared -o authlogin.dll authlogin.c -static
 *
 * Or use the build script: build_authlogin.bat
 */

#include <windows.h>
#include <string.h>

/* DLL instance handle */
static HINSTANCE hDllInstance = NULL;

/**
 * DllMain - Entry point
 * Called when the DLL is loaded/unloaded
 */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            hDllInstance = hinstDLL;
            DisableThreadLibraryCalls(hDllInstance);
            break;
        case DLL_PROCESS_DETACH:
            break;
        case DLL_THREAD_ATTACH:
            break;
        case DLL_THREAD_DETACH:
            break;
    }
    return TRUE;
}

/**
 * GetTicketSync - Get authentication ticket (main auth function)
 *
 * Parameters:
 *   username    - Player's username (ignored in offline mode)
 *   password    - Player's password (ignored in offline mode)
 *   outTicket   - Buffer to receive the ticket string
 *   outReasonCode - Pointer to receive reason code (0 = success)
 *
 * Returns:
 *   TRUE if successful, FALSE otherwise
 *
 * This is the main authentication function called by the game.
 * In offline mode, we simply return a success ticket without
 * making any network requests.
 */
__declspec(dllexport)
int WINAPI GetTicketSync(const char* username, const char* password, char* outTicket, int* outReasonCode)
{
    /* Return success ticket */
    if (outTicket != NULL)
    {
        strcpy(outTicket, "OFFLINE_TOKEN");
    }

    /* Return success code */
    if (outReasonCode != NULL)
    {
        *outReasonCode = 0;  /* 0 = success */
    }

    return 1;  /* TRUE */
}

/**
 * GetTicketSyncA - ANSI version of GetTicketSync
 */
__declspec(dllexport)
int WINAPI GetTicketSyncA(const char* username, const char* password, char* outTicket, int* outReasonCode)
{
    return GetTicketSync(username, password, outTicket, outReasonCode);
}

/**
 * GetTicketSyncW - Unicode version of GetTicketSync
 */
__declspec(dllexport)
int WINAPI GetTicketSyncW(const WCHAR* username, const WCHAR* password, WCHAR* outTicket, int* outReasonCode)
{
    /* Convert Unicode to ANSI for internal handling */
    char ansiUser[256];
    char ansiPass[256];
    char ansiTicket[256];

    WideCharToMultiByte(CP_ACP, 0, username, -1, ansiUser, sizeof(ansiUser), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, password, -1, ansiPass, sizeof(ansiPass), NULL, NULL);

    int result = GetTicketSync(ansiUser, ansiPass, ansiTicket, outReasonCode);

    if (result && outTicket != NULL)
    {
        MultiByteToWideChar(CP_ACP, 0, ansiTicket, -1, outTicket, 256);
    }

    return result;
}

/**
 * AuthLogin_Init - Initialize the auth module
 *
 * Called once when the game starts
 */
__declspec(dllexport)
int WINAPI AuthLogin_Init(void)
{
    return 1;  /* Success */
}

/**
 * AuthLogin_Shutdown - Shutdown the auth module
 *
 * Called once when the game exits
 */
__declspec(dllexport)
int WINAPI AuthLogin_Shutdown(void)
{
    return 1;  /* Success */
}

/**
 * AuthLogin_GetLastError - Get the last error code
 *
 * Some games may call this to get extended error info
 */
__declspec(dllexport)
int WINAPI AuthLogin_GetLastError(void)
{
    return 0;  /* No error */
}

/**
 * AuthLogin_GetServerName - Get the auth server name
 *
 * Returns the configured auth server (e.g., "www.ea.com")
 * In offline mode, this returns the original server name
 * to avoid triggering validation checks
 */
__declspec(dllexport)
const char* WINAPI AuthLogin_GetServerName(void)
{
    return "www.ea.com";
}

/**
 * AuthLogin_SetServerName - Set the auth server name
 *
 * Allows the game to configure which server to use
 */
__declspec(dllexport)
void WINAPI AuthLogin_SetServerName(const char* serverName)
{
    /* No-op in offline mode - we don't connect anywhere */
}

/**
 * AuthLogin_GetVersion - Get the DLL version
 */
__declspec(dllexport)
int WINAPI AuthLogin_GetVersion(void)
{
    return 1;  /* Version 1 */
}
