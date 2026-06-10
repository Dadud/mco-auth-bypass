/**
 * authlogin.c - Universal client-side auth + NPS shim for Motor City Online
 *
 * Two modes:
 *
 *  1. Auth bypass (always on)
 *     Replaces EA's authlogin.dll. GetTicketSync returns a configurable,
 *     self-describing ticket. The game skips EA's HTTPS auth and proceeds
 *     to NPS as if everything were normal.
 *
 *  2. NPS redirect (always on)
 *     Detours the game's calls to WSAConnect() and rewrites the destination
 *     IP to NPS_REDIRECT_HOST (default 127.0.0.1) so all NPS traffic lands
 *     on a server the coder controls. Works with mcos, OpenMCO, AZMCO, or
 *     any custom server listening on 127.0.0.1:8226.
 *
 *  3. In-proc fallback NPS responder (opt-in via -DENABLE_INPROC_FALLBACK)
 *     If compiled with the flag, the DLL also starts a tiny in-process NPS
 *     responder on 127.0.0.1:8226 that answers NPS_USER_LOGIN (0x501) with
 *     a hardcoded NPS_USER_VALID (0x601) containing one persona. Lets the
 *     game get to the character select screen with NO external server.
 *     Does NOT speak MCOTS or lobby - those are out of scope for v2.x.
 *
 * Build (cross-compile from Linux/macOS, needs mingw-w64):
 *     i686-w64-mingw32-gcc -shared -o authlogin.dll authlogin.c -static \
 *         -Wl,--subsystem,windows -lws2_32
 *
 * Build with in-proc fallback (off by default):
 *     i686-w64-mingw32-gcc -shared -o authlogin.dll authlogin.c -static \
 *         -Wl,--subsystem,windows -lws2_32 -DENABLE_INPROC_FALLBACK
 *
 * Protocol references:
 *   - NETWORK_PROTOCOL.md (Dadud/Motor-City-Online-RE, sections 4, 6, 7)
 *   - OpenMCO nps_payloads.py GameProfile / ProfileList byte layouts
 *
 * License: MIT (see LICENSE).
 */

/* Order matters: winsock2.h must come before windows.h (mingw warning). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>      /* WinHTTP for the modernized login POST */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

/* =====================================================================
 * USER CONFIGURATION - edit this block to customize the shim
 * =====================================================================
 */

/* Where NPS connections get redirected. Default: loopback so the coder's
 * local server picks them up. Change to your server's IP if it's not
 * on the same machine. Also used as the host portion of the default
 * LOGIN_URL. */
#define NPS_REDIRECT_HOST "127.0.0.1"

/* Port for the modernized login endpoint. The full default URL is
 * https://<NPS_REDIRECT_HOST>:8443/auth/login. Override LOGIN_URL below
 * to change the path or scheme. */
#define LOGIN_PORT 8443
#define LOGIN_PATH "/auth/login"

/* Set LOGIN_URL to a full URL to override the derived default. If left
 * as the empty string, the DLL uses https://<NPS_REDIRECT_HOST>:8443/auth/login
 * (or http://... if LOGIN_ALLOW_INSECURE is defined). */
#define LOGIN_URL ""

/* HTTP request timeout in milliseconds. */
#define LOGIN_TIMEOUT_MS 5000

/* If LOGIN_ALLOW_INSECURE is defined, the DLL will accept plain HTTP
 * for the login endpoint (http:// instead of https://). DO NOT enable
 * in production. Off by default. */
/* #define LOGIN_ALLOW_INSECURE */

/* If LOGIN_FALLBACK_OFFLINE is defined and the login server is
 * unreachable, the DLL falls back to the MCO1 offline ticket (v2.0.0
 * behavior) so the game still launches. Off by default - in production
 * you want a hard failure so silent outages don't get masked. */
/* #define LOGIN_FALLBACK_OFFLINE */

/* Ticket format. The DLL returns a self-describing ticket so any server
 * can validate it without prior coordination. Format:
 *
 *   "MCO1" + uint32_be(issued_unix) + uint32_be(expires_unix)
 *   + ":" + user + "@" + realm
 *
 * Total: 4 + 4 + 4 + 2 + len(user) + 1 + len(realm) bytes.
 * Max 117 bytes. Edit user/realm/expiry to taste.
 */
#define TICKET_MAGIC "MCO1"
#define TICKET_USER  "offline"
#define TICKET_REALM "localhost"
#define TICKET_EXPIRY_SECONDS 86400  /* 24h from first call */

/* In-proc fallback persona (only used if ENABLE_INPROC_FALLBACK).
 * This is the one persona the game will see in its character select. */
#define FALLBACK_PERSONA_NAME  "Driver"
#define FALLBACK_CUSTOMER_ID   1001
#define FALLBACK_PROFILE_ID    1

/* =====================================================================
 * END USER CONFIGURATION
 * =====================================================================
 */

static HINSTANCE g_hDllInstance = NULL;

/* ------------------------------------------------------------------
 * TICKET BUILDER
 * ------------------------------------------------------------------
 * Builds a self-describing ticket. The format is:
 *
 *   "MCO1" + uint32_be(issued) + uint32_be(expires) + ":" + user + "@" + realm
 *
 * Any server can parse this in 10 lines: read the 4-byte magic, BE-u32
 * for issued and expires, then split on ":" + "@". No shared secret,
 * no key, no coordination. Server decides whether the ticket is
 * trustworthy (e.g., "was it issued by MY DLL?" = "is the magic MCO1
 * and the realm mine?"). For local/offline mode, accept any MCO1 ticket.
 */
static int build_ticket(char* out, int out_size)
{
    if (out == NULL || out_size < 32) {
        return 0;
    }
    /* Initialize once per process. Cached for performance and to keep
     * issued/expiry stable across multiple GetTicketSync calls. */
    static char  cached[160];
    static int   cached_len = 0;
    static DWORD last_init_tick = 0;
    DWORD now_tick = GetTickCount();
    if (cached_len == 0 || (now_tick - last_init_tick) > 60000) {
        time_t now = time(NULL);
        uint32_t issued  = (uint32_t)now;
        uint32_t expires = (uint32_t)(now + TICKET_EXPIRY_SECONDS);
        int n = 0;
        /* Magic */
        memcpy(cached + n, TICKET_MAGIC, 4); n += 4;
        /* issued (BE) */
        cached[n++] = (issued  >> 24) & 0xFF;
        cached[n++] = (issued  >> 16) & 0xFF;
        cached[n++] = (issued  >>  8) & 0xFF;
        cached[n++] =  issued        & 0xFF;
        /* expires (BE) */
        cached[n++] = (expires >> 24) & 0xFF;
        cached[n++] = (expires >> 16) & 0xFF;
        cached[n++] = (expires >>  8) & 0xFF;
        cached[n++] =  expires       & 0xFF;
        /* separator + user + @ + realm */
        cached[n++] = ':';
        int u = 0;
        while (TICKET_USER[u] && n < (int)sizeof(cached) - 64) cached[n++] = TICKET_USER[u++];
        cached[n++] = '@';
        int r = 0;
        while (TICKET_REALM[r] && n < (int)sizeof(cached) - 2) cached[n++] = TICKET_REALM[r++];
        cached[n] = '\0';
        cached_len = n;
        last_init_tick = now_tick;
    }
    if (cached_len >= out_size) return 0;
    memcpy(out, cached, cached_len);
    out[cached_len] = '\0';
    return 1;
}

/* ------------------------------------------------------------------
 * MODERNIZED LOGIN - real HTTPS POST to a login endpoint
 * ------------------------------------------------------------------
 *
 * Replaces the v2.0.0 'always return MCO1 ticket' behavior. GetTicketSync
 * now POSTs the supplied username + password to a configurable endpoint
 * and returns the ticket the server issues.
 *
 * Wire format (request):
 *   POST <LOGIN_URL> HTTP/1.1
 *   Host: <NPS_REDIRECT_HOST>
 *   Content-Type: application/json
 *
 *   {"username":"...","password":"..."}
 *
 * Wire format (response on 200):
 *   {"ticket":"<server-issued>","customer_id":<u32>,"persona_id":<u32>}
 *   Only the "ticket" field is required. customer_id/persona_id are
 *   optional; if present, the DLL logs them at INFO level for debugging.
 *
 * Wire format (response on 401 or any non-200):
 *   {"error":"<reason>"}   (body ignored; reason code returned to game)
 *
 * On transport failure (connection refused, timeout, DNS):
 *   - If LOGIN_FALLBACK_OFFLINE is defined: return the MCO1 ticket (v2.0.0 behavior)
 *   - Otherwise: return failure with reason=-2 (network error)
 *
 * Build flags:
 *   -DLOGIN_ALLOW_INSECURE   Use http:// instead of https:// (dev only)
 *   -DLOGIN_FALLBACK_OFFLINE Return offline ticket on transport error
 */

/* Build a minimal JSON body: {"username":"...","password":"..."}
 * Performs basic JSON string escaping (backslash and double-quote).
 * Returns the number of bytes written, or 0 on overflow. */
static int build_login_json_body(char* out, int out_size,
                                 const char* username, const char* password)
{
    int n = 0;
    int w = 0;
    /* {"username":" */
    w = snprintf(out + n, out_size - n, "{\"username\":\"");
    if (w < 0 || w >= out_size - n) return 0;
    n += w;
    /* username with escaping */
    for (const char* p = username ? username : ""; *p && n < out_size - 16; p++) {
        if (*p == '\\' || *p == '"') out[n++] = '\\';
        out[n++] = *p;
    }
    /* ","password":" */
    w = snprintf(out + n, out_size - n, "\",\"password\":\"");
    if (w < 0 || w >= out_size - n) return 0;
    n += w;
    /* password with escaping */
    for (const char* p = password ? password : ""; *p && n < out_size - 16; p++) {
        if (*p == '\\' || *p == '"') out[n++] = '\\';
        out[n++] = *p;
    }
    /* "} */
    if (n + 2 >= out_size) return 0;
    out[n++] = '"';
    out[n++] = '}';
    out[n] = '\0';
    return n;
}

/* Naive JSON field extractor: looks for "key":"value" in a flat JSON
 * object and copies the value (up to out_size-1 bytes) into out.
 * Returns 1 on success, 0 if the key is not found. This is intentionally
 * simple - we trust the server to send well-formed JSON, and we only
 * need to extract a single string field ("ticket"). */
static int json_get_string_field(const char* json, int json_len,
                                 const char* key, char* out, int out_size)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    if (n < 0 || n >= (int)sizeof(needle)) return 0;
    const char* p = json;
    const char* end = json + json_len;
    while (p < end) {
        if (p + n > end) return 0;
        if (memcmp(p, needle, n) == 0) {
            const char* v = p + n;
            int i = 0;
            while (v < end && *v != '"' && i < out_size - 1) {
                /* Handle backslash-escapes simply. */
                if (*v == '\\' && v + 1 < end) v++;
                out[i++] = *v++;
            }
            out[i] = '\0';
            return 1;
        }
        p++;
    }
    return 0;
}

/* WinHTTP is per-thread, so we initialize WinHTTP on first use and
 * open a session. The session is process-global. Caller must be the
 * same thread for session + connect + request lifecycle. */
static HINTERNET g_http_session = NULL;
static CRITICAL_SECTION g_http_lock;
static BOOL g_http_lock_init = FALSE;

static int login_via_http(const char* username, const char* password,
                          char* out_ticket, int out_ticket_size,
                          int* out_reason)
{
    if (out_ticket == NULL || out_ticket_size < 32) {
        if (out_reason) *out_reason = -1;
        return 0;
    }
    out_ticket[0] = '\0';
    if (out_reason) *out_reason = 0;

    if (!g_http_lock_init) {
        InitializeCriticalSection(&g_http_lock);
        g_http_lock_init = TRUE;
    }
    EnterCriticalSection(&g_http_lock);

    if (g_http_session == NULL) {
        g_http_session = WinHttpOpen(L"mco-auth-bypass/2.1",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (g_http_session == NULL) {
        LeaveCriticalSection(&g_http_lock);
#ifdef LOGIN_FALLBACK_OFFLINE
        build_ticket(out_ticket, out_ticket_size);
        return 1;
#else
        if (out_reason) *out_reason = -2;
        return 0;
#endif
    }

    /* Resolve LOGIN_URL: explicit override, or derived from NPS_REDIRECT_HOST. */
    wchar_t wurl[512];
    const char* scheme;
#ifdef LOGIN_ALLOW_INSECURE
    scheme = "http://";
#else
    scheme = "https://";
#endif
    if (LOGIN_URL[0] != '\0') {
        /* Use the override URL. Convert to wide. */
        int wn = MultiByteToWideChar(CP_ACP, 0, LOGIN_URL, -1, wurl, 512);
        if (wn == 0) {
            LeaveCriticalSection(&g_http_lock);
            if (out_reason) *out_reason = -2;
            return 0;
        }
    } else {
        /* Derived default: scheme://NPS_REDIRECT_HOST:LOGIN_PORT/LOGIN_PATH */
        char url[512];
        snprintf(url, sizeof(url), "%s%s:%d%s", scheme, NPS_REDIRECT_HOST, LOGIN_PORT, LOGIN_PATH);
        int wn = MultiByteToWideChar(CP_ACP, 0, url, -1, wurl, 512);
        if (wn == 0) {
            LeaveCriticalSection(&g_http_lock);
            if (out_reason) *out_reason = -2;
            return 0;
        }
    }

    /* WinHTTP needs the URL split into components. */
    URL_COMPONENTS urlc;
    memset(&urlc, 0, sizeof(urlc));
    urlc.dwStructSize = sizeof(urlc);
    wchar_t host[128] = {0};
    wchar_t path[256] = {0};
    urlc.lpszHostName = host;
    urlc.dwHostNameLength = 128;
    urlc.lpszUrlPath = path;
    urlc.dwUrlPathLength = 256;
    if (!WinHttpCrackUrl(wurl, 0, 0, &urlc)) {
        LeaveCriticalSection(&g_http_lock);
        if (out_reason) *out_reason = -2;
        return 0;
    }
    DWORD port = urlc.nPort;

    HINTERNET connect = WinHttpConnect(g_http_session, urlc.lpszHostName,
                                        port, 0);
    if (connect == NULL) {
        LeaveCriticalSection(&g_http_lock);
        if (out_reason) *out_reason = -2;
        return 0;
    }
    DWORD flags = (urlc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", urlc.lpszUrlPath,
                                           NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request == NULL) {
        WinHttpCloseHandle(connect);
        LeaveCriticalSection(&g_http_lock);
        if (out_reason) *out_reason = -2;
        return 0;
    }
    /* Set timeouts. */
    WinHttpSetTimeouts(request, LOGIN_TIMEOUT_MS, LOGIN_TIMEOUT_MS,
                       LOGIN_TIMEOUT_MS, LOGIN_TIMEOUT_MS);

    /* Build and send the body. */
    char body[1024];
    int body_len = build_login_json_body(body, sizeof(body), username, password);
    if (body_len <= 0) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        LeaveCriticalSection(&g_http_lock);
        if (out_reason) *out_reason = -1;
        return 0;
    }

    LPCWSTR headers = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(request, headers, (DWORD)-1L,
                                 (LPVOID)body, (DWORD)body_len, (DWORD)body_len, 0);
    if (!ok) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        LeaveCriticalSection(&g_http_lock);
#ifdef LOGIN_FALLBACK_OFFLINE
        build_ticket(out_ticket, out_ticket_size);
        if (out_reason) *out_reason = -2;
        return 1;
#else
        if (out_reason) *out_reason = -2;
        return 0;
#endif
    }
    if (!WinHttpReceiveResponse(request, NULL)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        LeaveCriticalSection(&g_http_lock);
#ifdef LOGIN_FALLBACK_OFFLINE
        build_ticket(out_ticket, out_ticket_size);
        if (out_reason) *out_reason = -2;
        return 1;
#else
        if (out_reason) *out_reason = -2;
        return 0;
#endif
    }

    /* Read status code. */
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        LeaveCriticalSection(&g_http_lock);
        if (out_reason) *out_reason = (int)(status == 401 ? -3 : -4);
        return 0;
    }

    /* Read response body. */
    char resp[4096];
    int resp_len = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) break;
        if (avail == 0) break;
        if (resp_len + (int)avail > (int)sizeof(resp)) avail = sizeof(resp) - resp_len;
        DWORD read = 0;
        if (!WinHttpReadData(request, resp + resp_len, avail, &read)) break;
        resp_len += read;
        if (resp_len >= (int)sizeof(resp) - 1) break;
    }
    resp[resp_len] = '\0';
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    LeaveCriticalSection(&g_http_lock);

    if (!json_get_string_field(resp, resp_len, "ticket", out_ticket, out_ticket_size)) {
        /* 200 OK but no ticket field - server bug */
        if (out_reason) *out_reason = -5;
        return 0;
    }
    if (out_reason) *out_reason = 0;
    return 1;
}

/* ------------------------------------------------------------------
 * EXPORTS - EA authlib interface
 * ------------------------------------------------------------------
 */

__declspec(dllexport)
int WINAPI GetTicketSync(const char* username, const char* password,
                         char* outTicket, int* outReasonCode)
{
    if (outTicket == NULL) {
        if (outReasonCode != NULL) *outReasonCode = -1;
        return 0;
    }
    /* Try the modernized login endpoint first. */
    if (login_via_http(username, password, outTicket, 256, outReasonCode)) {
        return 1;
    }
    /* login_via_http returned failure. If the failure was a network
     * error AND the caller compiled with LOGIN_FALLBACK_OFFLINE, fall
     * back to the v2.0.0 MCO1 ticket. Note: login_via_http handles
     * the fallback itself when the flag is set, so this branch is
     * only reached for actual auth failures (401, malformed, etc.). */
#ifdef LOGIN_FALLBACK_OFFLINE
    if (outReasonCode && (*outReasonCode == -3 || *outReasonCode == -5)) {
        /* Auth failure (not network). Still try offline. */
        if (build_ticket(outTicket, 256)) {
            *outReasonCode = 0;
            return 1;
        }
    }
#endif
    return 0;
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
    char ansiTicket[256] = {0};
    int result = GetTicketSync(NULL, NULL, ansiTicket, outReasonCode);
    if (result && outTicket != NULL) {
        MultiByteToWideChar(CP_ACP, 0, ansiTicket, -1, outTicket, 256);
    }
    return result;
}

__declspec(dllexport) int WINAPI AuthLogin_Init(void)              { return 1; }
__declspec(dllexport) int WINAPI AuthLogin_Shutdown(void)          { return 1; }
__declspec(dllexport) int WINAPI AuthLogin_GetLastError(void)      { return 0; }
__declspec(dllexport)
const char* WINAPI AuthLogin_GetServerName(void)
{
    /* Match the real EA server name so the game doesn't notice the
     * auth layer is gone. The NPS layer (port 8226 etc.) is independent. */
    return "www.ea.com";
}
__declspec(dllexport)
void WINAPI AuthLogin_SetServerName(const char* serverName) { /* no-op */ }
__declspec(dllexport) int WINAPI AuthLogin_GetVersion(void)        { return 2; }

/* ------------------------------------------------------------------
 * NPS REDIRECT - WSAConnect detour
 * ------------------------------------------------------------------
 * Intercepts the game's connect() calls and rewrites the destination
 * IP to NPS_REDIRECT_HOST. The port stays the same (8226 for login,
 * 7003 for lobby, 43300 for MCOTS, etc.), so the coder's server just
 * needs to be listening on those ports on 127.0.0.1.
 *
 * This is a 12-byte inline hook: we save the first 12 bytes of the
 * real WSAConnect and replace them with a JMP to our detour function.
 * After 5 bytes of JMP opcode + 4 bytes of relative address = 9 bytes
 * are used, then a 3-byte NOP sled for alignment. Total: 12 bytes.
 */

static FARPROC  g_real_WSAConnect = NULL;
static BYTE     g_real_WSAConnect_bytes[12];
static BOOL     g_hook_installed  = FALSE;
static CRITICAL_SECTION g_hook_lock;
static BOOL     g_hook_lock_init = FALSE;

/* The detour function runs in the game's process. It must be safe
 * to call from any thread. */
static int WSAAPI Detour_WSAConnect(
    SOCKET s,
    const struct sockaddr* name,
    int namelen,
    LPWSABUF lpCallerData,
    LPWSABUF lpCalleeData,
    LPQOS lpSQOS,
    LPQOS lpGQOS)
{
    /* Call the real WSAConnect with a rewritten sockaddr. We only
     * touch IPv4 (sockaddr_in). IPv6 / other families pass through. */
    struct sockaddr_in copy;
    struct sockaddr_in* target = NULL;
    if (name != NULL && name->sa_family == AF_INET && namelen >= (int)sizeof(struct sockaddr_in)) {
        memcpy(&copy, name, sizeof(copy));
        copy.sin_addr.s_addr = inet_addr(NPS_REDIRECT_HOST);
        target = &copy;
    }
    typedef int (WSAAPI *WSAConnect_fn)(SOCKET, const struct sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
    WSAConnect_fn real = (WSAConnect_fn)g_real_WSAConnect;
    if (target != NULL) {
        return real(s, (const struct sockaddr*)target, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
    }
    return real(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
}

/* Install the 12-byte inline hook on the real ws2_32!WSAConnect. */
static void install_wsa_connect_hook(void)
{
    if (g_hook_installed) return;
    if (!g_hook_lock_init) {
        InitializeCriticalSection(&g_hook_lock);
        g_hook_lock_init = TRUE;
    }
    EnterCriticalSection(&g_hook_lock);
    if (g_hook_installed) { LeaveCriticalSection(&g_hook_lock); return; }

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (ws2 == NULL) ws2 = LoadLibraryA("ws2_32.dll");
    if (ws2 == NULL) { LeaveCriticalSection(&g_hook_lock); return; }

    void* fn = (void*)GetProcAddress(ws2, "WSAConnect");
    if (fn == NULL) { LeaveCriticalSection(&g_hook_lock); return; }
    g_real_WSAConnect = (FARPROC)fn;
    memcpy(g_real_WSAConnect_bytes, fn, 12);

    /* Build the trampoline in a freshly allocated executable page. */
    /* 5 bytes JMP rel32 (E9) + 4 bytes relative = 9 bytes. We write
     * the 12 bytes at the function entry. Original bytes are preserved
     * in g_real_WSAConnect_bytes for the trampoline; the trampoline
     * itself is allocated as +12 from the original entry so a jump
     * from our hook lands on the original code after the 12-byte
     * patch. */
    BYTE tramp[32];
    /* mov rax, <8-byte original after the 12-byte patch> ; jmp rax */
    /* For simplicity we patch only 5 bytes (E9 + rel32) and a 7-byte
     * NOP sled. The trampoline is a fresh allocation that contains
     * the original 12 bytes followed by a JMP back to entry+12. */
    BYTE* orig = (BYTE*)fn;

    /* Allocate executable memory for the trampoline. */
    BYTE* tramp_alloc = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (tramp_alloc == NULL) { LeaveCriticalSection(&g_hook_lock); return; }
    memcpy(tramp_alloc, orig, 12);
    /* JMP back to orig+12: E9 <rel32> where rel32 = (orig+12) - (tramp_alloc+12) */
    DWORD rel = (DWORD)((BYTE*)orig + 12 - (tramp_alloc + 12 + 5));
    tramp_alloc[12] = 0xE9;
    tramp_alloc[13] = (BYTE)(rel        & 0xFF);
    tramp_alloc[14] = (BYTE)((rel >>  8) & 0xFF);
    tramp_alloc[15] = (BYTE)((rel >> 16) & 0xFF);
    tramp_alloc[16] = (BYTE)((rel >> 24) & 0xFF);

    /* Patch the function entry: E9 <rel32 to Detour_WSAConnect> + 7 NOPs */
    DWORD rel_detour = (DWORD)((BYTE*)Detour_WSAConnect - (orig + 5));
    BYTE patch[12];
    patch[0] = 0xE9;
    patch[1] = (BYTE)(rel_detour        & 0xFF);
    patch[2] = (BYTE)((rel_detour >>  8) & 0xFF);
    patch[3] = (BYTE)((rel_detour >> 16) & 0xFF);
    patch[4] = (BYTE)((rel_detour >> 24) & 0xFF);
    patch[5] = 0x90;  /* NOP */
    patch[6] = 0x90;
    patch[7] = 0x90;
    patch[8] = 0x90;
    patch[9] = 0x90;
    patch[10] = 0x90;
    patch[11] = 0x90;

    DWORD old_prot;
    VirtualProtect(fn, 12, PAGE_EXECUTE_READWRITE, &old_prot);
    memcpy(fn, patch, 12);
    VirtualProtect(fn, 12, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), fn, 12);

    g_hook_installed = TRUE;
    LeaveCriticalSection(&g_hook_lock);
}

/* ------------------------------------------------------------------
 * IN-PROCESS FALLBACK NPS RESPONDER (opt-in, see -DENABLE_INPROC_FALLBACK)
 * ------------------------------------------------------------------
 *
 * Speaks just enough NPS to get the game past the login screen:
 *   - Listens on 127.0.0.1:8226
 *   - Answers NPS_USER_LOGIN (0x501) with NPS_USER_VALID (0x601)
 *     containing one hardcoded persona
 *   - Closes the connection after sending
 *
 * NOT IMPLEMENTED in v2.x (by design, see INTEGRATION.md):
 *   - NPS_CRYPTO_PUB_KEY (0x1001) - no RSA
 *   - NPS_CRYPTO_DES_CBC (0x1101) - no DES
 *   - NPS_OK_TO_LOGIN (0x230) handshake on 7003 - no lobby
 *   - MCOTS on 43300 - no transaction layer
 *
 * This is a "game-launches-and-shows-character-select" responder, not
 * a server. Use with a real server for actual gameplay.
 */
#ifdef ENABLE_INPROC_FALLBACK

/* NPS packet header (v0): msgid(2 BE) + length(2 BE). Length is total
 * packet size including the 4-byte header. */
static void nps_header(unsigned char* buf, unsigned short msgid, unsigned short length)
{
    buf[0] = (msgid   >> 8) & 0xFF;
    buf[1] =  msgid         & 0xFF;
    buf[2] = (length  >> 8) & 0xFF;
    buf[3] =  length        & 0xFF;
}

/* NPS length-prefixed string (Pascal-style, big-endian length). */
static int nps_write_len_string(unsigned char* buf, int off, const char* s)
{
    int len = (int)strlen(s);
    if (len > 255) len = 255;
    buf[off++] = (unsigned char)((len >> 8) & 0xFF);
    buf[off++] = (unsigned char)( len       & 0xFF);
    memcpy(buf + off, s, len);
    return off + len;
}

/* NPS length-prefixed blob (Pascal-style, big-endian length). */
static int nps_write_len_blob(unsigned char* buf, int off, const unsigned char* data, int len)
{
    if (len > 65535) len = 65535;
    buf[off++] = (unsigned char)((len >> 8) & 0xFF);
    buf[off++] = (unsigned char)( len       & 0xFF);
    memcpy(buf + off, data, len);
    return off + len;
}

/* Write a uint32 big-endian. */
static int nps_write_u32(unsigned char* buf, int off, uint32_t v)
{
    buf[off++] = (unsigned char)((v >> 24) & 0xFF);
    buf[off++] = (unsigned char)((v >> 16) & 0xFF);
    buf[off++] = (unsigned char)((v >>  8) & 0xFF);
    buf[off++] = (unsigned char)( v        & 0xFF);
    return off;
}

/* Write a uint16 big-endian. */
static int nps_write_u16(unsigned char* buf, int off, uint16_t v)
{
    buf[off++] = (unsigned char)((v >> 8) & 0xFF);
    buf[off++] = (unsigned char)( v       & 0xFF);
    return off;
}

/* Build a GameProfile (per OpenMCO's GameProfile.to_bytes() layout). */
static int build_game_profile(unsigned char* buf, int maxlen,
                              uint32_t customer_id, uint32_t profile_id,
                              const char* profile_name, uint32_t shard_id)
{
    int off = 0;
    /* customer_id(4) */
    off = nps_write_u32(buf, off, customer_id);
    /* profile_name: lenstr */
    off = nps_write_len_string(buf, off, profile_name);
    /* server_id(4) - hardcoded magic 3341 from OpenMCO defaults */
    off = nps_write_u32(buf, off, 3341);
    /* create_stamp(4) - now */
    off = nps_write_u32(buf, off, (uint32_t)time(NULL));
    /* last_login_stamp(4) - now */
    off = nps_write_u32(buf, off, (uint32_t)time(NULL));
    /* number_games(4) - 0 */
    off = nps_write_u32(buf, off, 0);
    /* profile_id(4) */
    off = nps_write_u32(buf, off, profile_id);
    /* is_online(2) - 0 (false) */
    off = nps_write_u16(buf, off, 0);
    /* game_purchase_stamp(4) - 0 */
    off = nps_write_u32(buf, off, 0);
    /* game_serial_number: lenstr - empty */
    off = nps_write_len_string(buf, off, "");
    /* time_online(4) - 0 */
    off = nps_write_u32(buf, off, 0);
    /* time_in_game(4) - 0 */
    off = nps_write_u32(buf, off, 0);
    /* game_blob: lenstr(0) - empty */
    off = nps_write_len_blob(buf, off, (const unsigned char*)"", 0);
    /* personal_blob: lenstr(0) - empty */
    off = nps_write_len_blob(buf, off, (const unsigned char*)"", 0);
    /* picture_blob(1) - 0 */
    buf[off++] = 0;
    /* dnd(2) - 0 (false) */
    off = nps_write_u16(buf, off, 0);
    /* game_start_stamp(4) - 0 */
    off = nps_write_u32(buf, off, 0);
    /* current_key: lenstr - empty */
    off = nps_write_len_string(buf, off, "");
    /* profile_level(2) - 0 */
    off = nps_write_u16(buf, off, 0);
    /* shard_id(4) */
    off = nps_write_u32(buf, off, shard_id);
    (void)maxlen;
    return off;
}

/* Build NPS_USER_VALID (0x601) response with one persona. */
static int build_nps_user_valid(unsigned char* buf, int maxlen)
{
    int off = 0;
    /* header placeholder - filled in last */
    off += 4;
    /* customer_id(4) */
    off = nps_write_u32(buf, off, FALLBACK_CUSTOMER_ID);
    /* persona_count(1) - 1 */
    buf[off++] = 1;
    /* persona[0] - GameProfile bytes */
    int profile_off = off;
    off += build_game_profile(buf + off, maxlen - off,
                              FALLBACK_CUSTOMER_ID,
                              FALLBACK_PROFILE_ID,
                              FALLBACK_PERSONA_NAME,
                              0);  /* shard_id 0 */
    (void)profile_off;
    /* total length = off */
    nps_header(buf, 0x0601, (unsigned short)off);
    return off;
}

/* NPS responder worker thread. */
static DWORD WINAPI nps_thread(LPVOID arg)
{
    (void)arg;
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
        return 1;
    }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { WSACleanup(); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(NPS_REDIRECT_HOST);
    addr.sin_port = htons(8226);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(srv); WSACleanup(); return 1;
    }
    if (listen(srv, 8) == SOCKET_ERROR) {
        closesocket(srv); WSACleanup(); return 1;
    }
    for (;;) {
        SOCKET client = accept(srv, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        /* Read up to 4 KB, parse, respond. */
        unsigned char inbuf[4096];
        int n = recv(client, inbuf, sizeof(inbuf), 0);
        if (n >= 4) {
            unsigned short msgid = ((unsigned short)inbuf[0] << 8) | inbuf[1];
            if (msgid == 0x0501) {  /* NPS_USER_LOGIN */
                unsigned char outbuf[4096];
                int olen = build_nps_user_valid(outbuf, sizeof(outbuf));
                if (olen > 0) send(client, (const char*)outbuf, olen, 0);
            }
            /* Other message IDs: ignored. Client will see no response
             * and time out. That's the right behavior for a minimal
             * responder - the game will still get past the character
             * select if all it needs is NPS_USER_VALID. */
        }
        closesocket(client);
    }
    /* Unreachable, but tidy. */
    closesocket(srv);
    WSACleanup();
    return 0;
}

static BOOL g_nps_thread_started = FALSE;
static void start_nps_responder(void)
{
    if (g_nps_thread_started) return;
    HANDLE h = CreateThread(NULL, 0, nps_thread, NULL, 0, NULL);
    if (h != NULL) {
        CloseHandle(h);
        g_nps_thread_started = TRUE;
    }
}

#endif /* ENABLE_INPROC_FALLBACK */

/* ------------------------------------------------------------------
 * DLL ENTRY POINT
 * ------------------------------------------------------------------
 * On load (DLL_PROCESS_ATTACH) we install the WSAConnect detour. The
 * detour is a no-op if no NPS connection ever happens, so it's safe
 * to install always. Under -DENABLE_INPROC_FALLBACK, we also kick off
 * the in-proc NPS responder thread.
 */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            g_hDllInstance = hinstDLL;
            DisableThreadLibraryCalls(hinstDLL);
            install_wsa_connect_hook();
#ifdef ENABLE_INPROC_FALLBACK
            start_nps_responder();
#endif
            break;
        case DLL_PROCESS_DETACH:
            break;
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
    }
    return TRUE;
}
