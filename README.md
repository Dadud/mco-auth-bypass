# mco-auth-bypass

A reference implementation for replacing EA's `authlogin.dll` in
Motor City Online (2001, EA). **This is a gist, not a library.**

The point of this repo is to show developers what the authlib
interface looks like and where to plug in their own server. It
implements only the bit the game calls for a login ticket. The
rest of the MCO protocol is documented in
[Dadud/Motor-City-Online-RE](https://github.com/Dadud/Motor-City-Online-RE)
and is the responsibility of your server project.

## What this is

A 100-line C file that exports the same 9 functions EA's
`authlogin.dll` exports. `GetTicketSync` returns a hardcoded
ticket. The game trusts it and moves on to NPS as if EA's auth
server had issued a real one.

That's the whole trick. The game never validates the ticket
locally — it just stores it and sends it to the login server
(port 8226) on the next connection. So your server's ticket
validator is the only thing that has to parse the string.

## What this is NOT

- Not a server. No socket, no Winsock, no HTTP.
- Not a complete drop-in. The DLL only handles the authlib
  interface. NPS (login server 8226, lobby 7003), MCOTS (43300),
  DES-CBC, RSA, RC4, DCL compression — all of that belongs in
  your server project.
- Not production-ready. It's a reference. Read it, learn the
  shape, write your own.

## Files

| File | Purpose |
| --- | --- |
| `authlogin.c` | The 100-line reference. Read this. |
| `build.sh` | Cross-compile from Linux/macOS (needs mingw-w64). |
| `build_authlogin.bat` | Build on Windows (needs MinGW-w64). |
| `install.sh` | Copy built DLL into a game's install dir. |
| `tests/test_shim.c` | Tiny smoke test (ticket length, export names). |
| `LICENSE` | MIT, with the same EA legal notice as OpenMCO. |

## Build

Linux / macOS:
```bash
sudo apt install gcc-mingw-w64   # Debian/Ubuntu
brew install mingw-w64           # macOS
./build.sh
```

Windows (any MinGW-w64 distribution: WinLibs, MSYS2, choco):
```cmd
build_authlogin.bat
```

## Install

```bash
./install.sh /path/to/MCO/update
```

The DLL ends up in the game's `update` directory alongside
`MCity_d.exe`. The game loads it before any system DLL of the
same name.

## What to do next (the part this repo doesn't do)

After the game accepts your ticket, it tries to talk to:

- **Login server on port 8226** — speaks NPS. Speaks
  `NPS_USER_LOGIN` (0x501), returns `NPS_USER_VALID` (0x601).
  Uses RSA + DES-CBC for key exchange. Spec: NETWORK_PROTOCOL.md §4-7.
- **Lobby server on port 7003** — sends `NPS_OK_TO_LOGIN` (0x230)
  handshake, then `NPS_LOGIN` (0x100). Spec: NETWORK_PROTOCOL.md §6.1.
- **MCOTS on port 43300** — Diffie-Hellman key exchange, then RC4
  stream cipher, then DCL compression. Spec: NETWORK_PROTOCOL.md §7.3.

The redirect mechanism (registry `Auth_NPS_AAI_Hostname`, hosts
file, EXE patch, whatever) is also your problem — the DLL has no
opinion on that.

## Resources

- Protocol spec: [Dadud/Motor-City-Online-RE/docs/research/NETWORK_PROTOCOL.md](https://github.com/Dadud/Motor-City-Online-RE/blob/main/docs/research/NETWORK_PROTOCOL.md)
- OpenMCO server (Python reference): [Dadud/OpenMCO](https://github.com/Dadud/OpenMCO)
- mcos server (TypeScript reference): [drazisil-codecov/mcos](https://github.com/drazisil-codecov/mcos)
- Original v1.0.0 DLL source: this file is derived from
  `Dadud/OpenMCO/tools/auth/authlogin.c` (commit `c58da14`).

## License

MIT — see [LICENSE](LICENSE). Inherited legal notice regarding
reverse engineering applies. Not affiliated with EA.
