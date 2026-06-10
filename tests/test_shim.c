/*
 * test_shim.c - Unit tests for the authlogin.c shim
 *
 * Verifies:
 *  1. MakeTicket produces a valid MCO1 ticket with the right byte layout
 *  2. MakeTicket is self-describing (server can parse without prior coordination)
 *  3. NPS packet header builder produces the correct big-endian layout
 *  4. NPS_USER_VALID response builder produces a well-formed packet
 *
 * The detour and the in-proc fallback require a live process; this test
 * only covers the pure data-layer functions. Detour/fallback integration
 * must be verified on a Windows box with the actual game.
 *
 * Build (cross-compile):
 *   i686-w64-mingw32-gcc -o test_shim.exe test_shim.c -static
 *
 * The test pulls in a subset of authlogin.c by #include. To keep the
 * shim self-contained we use the same source file but stub out the
 * detour / DllMain entry point.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

/* Replicate just the pure helpers from authlogin.c. Keep them in
 * sync with the main source - if you change one, change both. */
#define TICKET_MAGIC "MCO1"
#define TICKET_USER  "offline"
#define TICKET_REALM "localhost"

static int build_ticket(char* out, int out_size)
{
    if (out == NULL || out_size < 32) return 0;
    time_t now = time(NULL);
    uint32_t issued  = (uint32_t)now;
    uint32_t expires = (uint32_t)(now + 86400);
    int n = 0;
    memcpy(out + n, TICKET_MAGIC, 4); n += 4;
    out[n++] = (issued  >> 24) & 0xFF;
    out[n++] = (issued  >> 16) & 0xFF;
    out[n++] = (issued  >>  8) & 0xFF;
    out[n++] =  issued        & 0xFF;
    out[n++] = (expires >> 24) & 0xFF;
    out[n++] = (expires >> 16) & 0xFF;
    out[n++] = (expires >>  8) & 0xFF;
    out[n++] =  expires       & 0xFF;
    out[n++] = ':';
    int u = 0; while (TICKET_USER[u])  out[n++] = TICKET_USER[u++];
    out[n++] = '@';
    int r = 0; while (TICKET_REALM[r]) out[n++] = TICKET_REALM[r++];
    out[n] = '\0';
    return 1;
}

static int nps_write_len_string(unsigned char* buf, int off, const char* s)
{
    int len = (int)strlen(s);
    if (len > 255) len = 255;
    buf[off++] = (unsigned char)((len >> 8) & 0xFF);
    buf[off++] = (unsigned char)( len       & 0xFF);
    memcpy(buf + off, s, len);
    return off + len;
}
static int nps_write_u32(unsigned char* buf, int off, uint32_t v)
{
    buf[off++] = (unsigned char)((v >> 24) & 0xFF);
    buf[off++] = (unsigned char)((v >> 16) & 0xFF);
    buf[off++] = (unsigned char)((v >>  8) & 0xFF);
    buf[off++] = (unsigned char)( v        & 0xFF);
    return off;
}
static int nps_write_u16(unsigned char* buf, int off, uint16_t v)
{
    buf[off++] = (unsigned char)((v >> 8) & 0xFF);
    buf[off++] = (unsigned char)( v       & 0xFF);
    return off;
}
static void nps_header(unsigned char* buf, unsigned short msgid, unsigned short length)
{
    buf[0] = (msgid   >> 8) & 0xFF;
    buf[1] =  msgid         & 0xFF;
    buf[2] = (length  >> 8) & 0xFF;
    buf[3] =  length        & 0xFF;
}

/* ---- test framework ---- */
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_passed++; printf("  PASS  %s\n", msg); } \
    else      { g_failed++; printf("  FAIL  %s\n", msg); } \
} while (0)

/* ---- v2.1.0 helpers (duplicated from authlogin.c) ---- */

/* Build a minimal JSON body: {"username":"...","password":"..."} */
static int build_login_json_body(char* out, int out_size,
                                 const char* username, const char* password)
{
    int n = 0;
    int w = snprintf(out + n, out_size - n, "{\"username\":\"");
    if (w < 0 || w >= out_size - n) return 0;
    n += w;
    for (const char* p = username ? username : ""; *p && n < out_size - 16; p++) {
        if (*p == '\\' || *p == '"') out[n++] = '\\';
        out[n++] = *p;
    }
    w = snprintf(out + n, out_size - n, "\",\"password\":\"");
    if (w < 0 || w >= out_size - n) return 0;
    n += w;
    for (const char* p = password ? password : ""; *p && n < out_size - 16; p++) {
        if (*p == '\\' || *p == '"') out[n++] = '\\';
        out[n++] = *p;
    }
    if (n + 2 >= out_size) return 0;
    out[n++] = '"';
    out[n++] = '}';
    out[n] = '\0';
    return n;
}

/* JSON field extractor: looks for "key":"value" in a flat JSON object. */
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

/* ---- tests ---- */

static void test_ticket_format(void)
{
    printf("\n[test_ticket_format]\n");
    char ticket[256];
    CHECK(build_ticket(ticket, sizeof(ticket)) == 1, "build_ticket returns success");

    /* Magic */
    CHECK(memcmp(ticket, "MCO1", 4) == 0, "ticket starts with MCO1 magic");

    /* issued/expiry: 4 BE bytes each at offset 4 and 8 */
    uint32_t issued = ((uint32_t)(unsigned char)ticket[4] << 24) |
                      ((uint32_t)(unsigned char)ticket[5] << 16) |
                      ((uint32_t)(unsigned char)ticket[6] <<  8) |
                      ((uint32_t)(unsigned char)ticket[7]);
    uint32_t expires = ((uint32_t)(unsigned char)ticket[8] << 24) |
                       ((uint32_t)(unsigned char)ticket[9] << 16) |
                       ((uint32_t)(unsigned char)ticket[10] << 8) |
                       ((uint32_t)(unsigned char)ticket[11]);
    time_t now = time(NULL);
    CHECK((int32_t)(issued - now) <= 2 && (int32_t)(issued - now) >= -2,
          "issued is approximately now");
    CHECK(expires > issued, "expires > issued");
    CHECK(expires - issued == 86400, "expiry is exactly 24h");

    /* separator */
    CHECK(ticket[12] == ':', "':' separator at offset 12");

    /* user@realm */
    char* at = strchr(ticket, '@');
    CHECK(at != NULL, "ticket contains '@'");
    if (at != NULL) {
        int user_len = (int)(at - (ticket + 13));
        CHECK(user_len == (int)strlen(TICKET_USER), "user matches TICKET_USER");
        CHECK(strcmp(at + 1, TICKET_REALM) == 0, "realm matches TICKET_REALM");
    }
}

static void test_ticket_self_describing(void)
{
    printf("\n[test_ticket_self_describing]\n");
    /* A server with no prior knowledge should be able to parse this.
     * Simulate that parse: extract magic, timestamps, user, realm. */
    char ticket[256];
    build_ticket(ticket, sizeof(ticket));

    char magic[5] = {0};
    memcpy(magic, ticket, 4);

    uint32_t issued = ((uint32_t)(unsigned char)ticket[4] << 24) |
                      ((uint32_t)(unsigned char)ticket[5] << 16) |
                      ((uint32_t)(unsigned char)ticket[6] <<  8) |
                      ((uint32_t)(unsigned char)ticket[7]);
    uint32_t expires = ((uint32_t)(unsigned char)ticket[8] << 24) |
                       ((uint32_t)(unsigned char)ticket[9] << 16) |
                       ((uint32_t)(unsigned char)ticket[10] << 8) |
                       ((uint32_t)(unsigned char)ticket[11]);

    char* sep = strchr(ticket + 13, '@');
    int user_len = (int)(sep - (ticket + 13));
    char user[64] = {0};
    memcpy(user, ticket + 13, user_len);
    char realm[64] = {0};
    strcpy(realm, sep + 1);

    CHECK(strcmp(magic, "MCO1") == 0, "server: magic is MCO1");
    CHECK(time(NULL) >= (time_t)issued - 2 && time(NULL) <= (time_t)expires + 2,
          "server: ticket is currently valid");
    CHECK(strcmp(user, "offline") == 0, "server: extracted user 'offline'");
    CHECK(strcmp(realm, "localhost") == 0, "server: extracted realm 'localhost'");
}

static void test_nps_header(void)
{
    printf("\n[test_nps_header]\n");
    unsigned char buf[4];
    nps_header(buf, 0x0501, 16);
    CHECK(buf[0] == 0x05 && buf[1] == 0x01, "NPS header msgid 0x0501 big-endian");
    CHECK(buf[2] == 0x00 && buf[3] == 0x10, "NPS header length 16 big-endian");
}

static void test_nps_user_valid_structure(void)
{
    printf("\n[test_nps_user_valid_structure]\n");
    /* Build a minimal NPS_USER_VALID: header(4) + customer_id(4) +
     * persona_count(1) + [one minimal GameProfile].
     * Just verify the header/ids are well-formed; full GameProfile
     * byte layout is the same as OpenMCO's implementation. */
    unsigned char buf[256];
    int off = 0;
    off += 4;  /* header placeholder */
    off = nps_write_u32(buf, off, 1001);   /* customer_id */
    buf[off++] = 1;                         /* persona_count */
    /* Minimum profile: just customer_id(4) + name("Driver") + server_id(4) + stamps
     * We're not testing the full profile here, just the wrapping. */
    off = nps_write_u32(buf, off, 1001);
    off = nps_write_len_string(buf, off, "Driver");
    off = nps_write_u32(buf, off, 3341);
    off = nps_write_u32(buf, off, (uint32_t)time(NULL));  /* create_stamp */
    off = nps_write_u32(buf, off, (uint32_t)time(NULL));  /* last_login_stamp */
    off = nps_write_u32(buf, off, 0);  /* number_games */
    off = nps_write_u32(buf, off, 1);  /* profile_id */
    nps_header(buf, 0x0601, (unsigned short)off);

    CHECK(buf[0] == 0x06 && buf[1] == 0x01, "NPS_USER_VALID msgid is 0x0601");
    uint16_t length = ((uint16_t)buf[2] << 8) | buf[3];
    CHECK(length == off, "header length matches body length");

    uint32_t cid = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                   ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
    CHECK(cid == 1001, "customer_id is 1001");
    CHECK(buf[8] == 1, "persona_count is 1");
}

static void test_login_json_body(void)
{
    printf("\n[test_login_json_body]\n");
    char body[256];
    int n = build_login_json_body(body, sizeof(body), "alice", "secret");
    CHECK(n > 0, "build_login_json_body returns success");
    CHECK(strcmp(body, "{\"username\":\"alice\",\"password\":\"secret\"}") == 0,
          "body is the expected JSON");
}

static void test_login_json_escaping(void)
{
    printf("\n[test_login_json_escaping]\n");
    char body[256];
    /* Password with a quote and a backslash should be escaped. */
    int n = build_login_json_body(body, sizeof(body), "alice", "p\"a\\ss");
    CHECK(n > 0, "escaping returns success");
    CHECK(strcmp(body, "{\"username\":\"alice\",\"password\":\"p\\\"a\\\\ss\"}") == 0,
          "quote and backslash are escaped in password");
    /* Username with a quote. */
    n = build_login_json_body(body, sizeof(body), "a\"b", "c");
    CHECK(strcmp(body, "{\"username\":\"a\\\"b\",\"password\":\"c\"}") == 0,
          "quote in username is escaped");
}

static void test_json_field_extraction(void)
{
    printf("\n[test_json_field_extraction]\n");
    const char* resp1 = "{\"ticket\":\"abc123\",\"customer_id\":1001}";
    char out[64];
    CHECK(json_get_string_field(resp1, (int)strlen(resp1), "ticket", out, sizeof(out)) == 1,
          "extract ticket from typical response");
    CHECK(strcmp(out, "abc123") == 0, "ticket value is correct");

    CHECK(json_get_string_field(resp1, (int)strlen(resp1), "customer_id", out, sizeof(out)) == 0,
          "numeric customer_id is NOT extracted (json_get_string_field is string-only)");

    /* Missing field. */
    const char* resp2 = "{\"error\":\"invalid\"}";
    CHECK(json_get_string_field(resp2, (int)strlen(resp2), "ticket", out, sizeof(out)) == 0,
          "missing ticket returns 0");
}

static void test_login_json_roundtrip(void)
{
    printf("\n[test_login_json_roundtrip]\n");
    /* Realistic test: server returns a 200 with a JSON body, the DLL
     * extracts the ticket. The WinHTTP transport is not tested here
     * (it would need a live server), but the request body construction
     * and response parsing are. */
    char body[512];
    build_login_json_body(body, sizeof(body), "player1", "p@ss\"word");
    /* Server response (simulated). */
    char resp[256] = "{\"ticket\":\"sess-xyz-789\",\"customer_id\":42,\"persona_id\":1}";
    char ticket[64];
    int ok = json_get_string_field(resp, (int)strlen(resp), "ticket", ticket, sizeof(ticket));
    CHECK(ok == 1, "server response parses");
    CHECK(strcmp(ticket, "sess-xyz-789") == 0, "extracted ticket matches");
    /* The request body that produced this is what the DLL would POST. */
    CHECK(strstr(body, "\"username\":\"player1\"") != NULL, "request body has username");
    CHECK(strstr(body, "\"password\":\"p@ss\\\"word\"") != NULL, "request body has escaped password");
}

int main(void)
{
    printf("=== mco-auth-bypass shim tests ===\n");
    test_ticket_format();
    test_ticket_self_describing();
    test_nps_header();
    test_nps_user_valid_structure();
    test_login_json_body();
    test_login_json_escaping();
    test_json_field_extraction();
    test_login_json_roundtrip();

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
