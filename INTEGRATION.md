# INTEGRATION.md

Three ways to use the mco-auth-bypass shim with your MCO server project.

Pick the one that matches your goal. Read top to bottom or jump to your scenario.

---

## Scenario A — Offline only (no server)

You just want the game to launch. No external server. No internet. The
character select screen appears with one persona named `Driver`.

**Build the fallback variant:**

```bash
i686-w64-mingw32-gcc -shared -o authlogin.dll authlogin.c -static \
    -Wl,--subsystem,windows -lws2_32 -DENABLE_INPROC_FALLBACK
```

**Install:**

```bash
./install.sh /path/to/MCO/update
```

**What you get:**

- Game launches
- Authlib returns an MCO1 ticket (`MCO1<ts><exp>:offline@localhost`)
- DLL listens on `127.0.0.1:8226` and answers `NPS_USER_LOGIN` (0x501)
  with a hardcoded `NPS_USER_VALID` (0x601) containing one persona
- Character select appears

**What you don't get:**

- No real MCOTS (port 43300) — the game will hang trying to connect
  to it after you pick a persona. You can see the UI, but you can't
  enter a race.
- No lobby (port 7003) — the lobby server `NPS_OK_TO_LOGIN` (0x230)
  handshake is not implemented.

This scenario is for **"I just want to demo that the shim works"** or
**"I want to see the menus and confirm the DLL is loaded"**. For actual
gameplay, use scenario B or C.

---

## Scenario B — Redirect NPS to your own server (universal)

You're running (or writing) an MCO server elsewhere — mcos, OpenMCO,
AZMCO, or a custom one. You want the game to talk to **your** server
without ever touching EA.

**Build the default variant** (no fallback flag):

```bash
i686-w64-mingw32-gcc -shared -o authlogin.dll authlogin.c -static \
    -Wl,--subsystem,windows -lws2_32
```

**Configure the redirect target** by editing `authlogin.c`:

```c
#define NPS_REDIRECT_HOST "127.0.0.1"   /* change to your server IP */
```

Then re-build and re-install.

**Install:**

```bash
./install.sh /path/to/MCO/update
```

**Start your server on `NPS_REDIRECT_HOST` listening on the standard
ports:**

- `8226` — Login Server (must answer `NPS_USER_LOGIN` with `NPS_USER_VALID`)
- `7003` — Lobby Server (must send `NPS_OK_TO_LOGIN` 0x230 first, then handle `NPS_LOGIN` 0x100)
- `43300` — MCOTS (the game/server will negotiate RC4 session key after NPS login)

**What you get:**

- Authlib returns the MCO1 ticket
- Every `WSAConnect()` call from the game gets its `sin_addr` rewritten
  to `NPS_REDIRECT_HOST` (port unchanged), so all NPS/MCOTS traffic
  lands on your server
- Your server gets a known ticket format it can validate (see ticket
  format below)

**What you need to handle in your server:**

- Accept the MCO1 ticket. The format is:
  ```
  MCO1  + uint32_be(issued_unix)  + uint32_be(expires_unix)
       + ":"  + TICKET_USER  + "@"  + TICKET_REALM
  ```
  Any language, 10 lines. See `examples/validate_ticket.py` below.
- Speak the NPS protocol (login → user_valid → OK_TO_LOGIN → MCOTS).
  See `docs/research/NETWORK_PROTOCOL.md` in
  `Dadud/Motor-City-Online-RE` for the full spec.
- Optionally, validate the ticket's `realm` against your own — only
  accept `MCO1...@myrealm` if you want to lock the shim to your
  deployment.

This scenario is **universal**: any MCO server project works, no
coordination required.

---

## Scenario C — Embed your own session info in the ticket

You want the ticket to carry a real customer ID, persona ID, or
session data that your server can use without further negotiation.

**Edit the ticket macros** in `authlogin.c`:

```c
#define TICKET_MAGIC "MCO1"
#define TICKET_USER  "yourname"        /* change this */
#define TICKET_REALM "yourserver.com"  /* change this */
#define TICKET_EXPIRY_SECONDS 86400
```

For richer payloads (customer ID, persona ID, custom JSON), the
`build_ticket()` function is the only place you need to edit. Replace
the `memcpy + uint32 packing` block with whatever byte layout you
want, up to ~250 bytes (the buffer the game reads is 256 bytes, the
ticket format `MCO1...<rest>` is the only constraint).

**Coordinate with your server:** make sure your server's ticket
validator parses the new format.

This scenario is for coders who want to drop the bypass into an
existing MMO-style auth system that already expects structured
tickets.

---

## Ticket format spec (for server implementors)

```
Offset  Size  Field
0       4     Magic (must be "MCO1" / 0x4D 0x43 0x4F 0x31)
4       4     issued_unix  (uint32 big-endian, seconds since epoch)
8       4     expires_unix (uint32 big-endian, seconds since epoch)
12      1     ':' (literal 0x3A)
13      N     user (UTF-8, no terminator)
13+N    1     '@' (literal 0x40)
14+N    M     realm (UTF-8, no terminator)
```

Total: `4 + 4 + 4 + 1 + N + 1 + M` bytes, max 256.

`N` and `M` are bounded by your `#define TICKET_USER` /
`TICKET_REALM` strings. The current defaults give a 22-byte ticket
(`MCO1` + 4 + 4 + `:` + `offline` + `@` + `localhost` = 4+4+4+1+7+1+9 = 30
bytes, with the `'\0'` C-string terminator).

### Python ticket validator (10 lines)

```python
import struct, time

def parse_ticket(t: bytes) -> dict | None:
    if len(t) < 14 or t[:4] != b"MCO1" or t[12:13] != b":" or b"@" not in t[13:]:
        return None
    issued  = struct.unpack(">I", t[4:8])[0]
    expires = struct.unpack(">I", t[8:12])[0]
    if time.time() > expires:
        return None  # expired
    user, _, realm = t[13:].rpartition(b"@")
    return {"user": user.decode(), "realm": realm.decode(),
            "issued": issued, "expires": expires}

# Use it:
ticket = b"MCO1\x00\x00\x00\x00\x67\xee\x93\x00:offline@localhost"
print(parse_ticket(ticket))
# {'user': 'offline', 'realm': 'localhost', 'issued': ..., 'expires': ...}
```

This is the entire server-side half of the bypass. Drop it into a
test endpoint, accept any MCO1 ticket, and the rest of NPS is whatever
your server already does.

---

## How the Winsock detour works (so you can audit it)

`DllMain` runs on DLL load. It finds `ws2_32!WSAConnect` via
`GetProcAddress`, saves the first 12 bytes into a trampoline
allocation, and overwrites the function entry with a 5-byte JMP to
`Detour_WSAConnect` (plus 7 bytes of NOP sled for alignment).

`Detour_WSAConnect` checks the destination address family. If it's
`AF_INET` (IPv4), it copies the sockaddr, rewrites `sin_addr.s_addr`
to `inet_addr(NPS_REDIRECT_HOST)`, and calls the real `WSAConnect`
with the modified destination. The original port stays the same.

**This is invasive.** It rewrites the function entry of a system DLL.
It will trigger anti-virus software (Windows Defender might flag it,
sandboxed test environments definitely will). It's the right tool for
a private-shard deployment, not for distribution on a public
marketplace.

If your deployment needs to be quieter, alternatives:
- Patch the game EXE's import table instead of `ws2_32`'s (more
  targeted, no system-DLL hook)
- Use a custom DNS resolver / hosts file to redirect
  `Auth_NPS_AAI_Hostname` (no detour at all, but the coders' server
  has to bind the EA server's hostname)

---

## What this shim does NOT do (v2.x scope)

| Feature | Status |
|---|---|
| Authlib ticket (GetTicketSync) | ✅ |
| WSAConnect detour to loopback | ✅ |
| In-proc NPS_USER_LOGIN responder (opt-in) | ✅ |
| NPS_CRYPTO_PUB_KEY (RSA) | ❌ — your server has to do this |
| NPS_CRYPTO_DES_CBC | ❌ — your server has to do this |
| NPS_OK_TO_LOGIN handshake on port 7003 | ❌ — your lobby server has to do this |
| MCOTS on port 43300 | ❌ — your shard has to do this |
| Diffie-Hellman key exchange | ❌ — out of scope |
| RC4 stream cipher for MCOTS | ❌ — out of scope |
| PKWARE DCL compression | ❌ — out of scope |

A complete NPS/MCOTS implementation belongs in your **server**
project, not in a 25 KB client-side shim. The shim's job is to make
the client side portable so you can pair it with any server.

---

## Reference

- Protocol spec: `Dadud/Motor-City-Online-RE/docs/research/NETWORK_PROTOCOL.md`
- NPS message IDs (Appendix A of NETWORK_PROTOCOL.md)
- OpenMCO server reference (Python): `Dadud/OpenMCO/src/handlers/login.py`
- mcos server reference (TypeScript): `github.com/drazisil-codecov/mcos`
- EA authlib interface: `authlogin.c` lines 95-145 (the original 9
  exports from v1.0.0, unchanged)
