# mco-auth-bypass

Universal client-side auth + NPS shim for **Motor City Online** (2001, EA).
Drop in, redirect the game to your own server, get past the login screen.

v2.0.0 — extracted from [Dadud/OpenMCO](https://github.com/Dadud/OpenMCO)
and extended with a Winsock detour that works with **any** MCO server project.

## What you get

| File | Purpose |
| --- | --- |
| `authlogin.c` | The shim. ~600 lines, builds with mingw-w64. |
| `build.sh` | Cross-compile to `authlogin.dll` from Linux/macOS. |
| `build_authlogin.bat` | Build on Windows. |
| `install.sh` | Copy built DLL into a game's install dir. |
| `tests/test_shim.c` + `tests/build_test.sh` | Unit tests for the pure data layer. |
| `INTEGRATION.md` | **Read this.** Three scenarios (offline, redirect to your server, embed your own ticket). |
| `LICENSE` | MIT, with the same EA legal notice as OpenMCO. |

## What it does

Two things, both on by default:

1. **Authlib bypass.** Replaces EA's `authlogin.dll`. `GetTicketSync` returns
   a self-describing `MCO1` ticket for any credentials. The game skips EA's
   HTTPS auth and proceeds to NPS as if everything were normal.
2. **NPS redirect.** Detours `ws2_32!WSAConnect` and rewrites the destination
   IP to `NPS_REDIRECT_HOST` (default `127.0.0.1`). All NPS/MCOTS traffic
   lands on a server the coder controls. Port stays the same (8226, 7003,
   43300, etc.).

Optional, opt-in via `-DENABLE_INPROC_FALLBACK`:

3. **In-proc NPS responder.** The DLL binds `127.0.0.1:8226` and answers
   `NPS_USER_LOGIN` (0x501) with a hardcoded `NPS_USER_VALID` (0x601) so
   the game reaches character select with no external server. Does **not**
   speak MCOTS or lobby — those are out of scope for a 25 KB shim.

## What it doesn't do (by design, see INTEGRATION.md)

- No RSA / DES / RC4 / DCL compression. Those belong in your **server**.
- No `NPS_OK_TO_LOGIN` (0x230) handshake. Your **lobby server** does that.
- No MCOTS. Your **shard** does that.

The shim's job is to make the client side portable. The server side is
your project.

## Build

### Windows

```cmd
build_authlogin.bat
```

Needs `gcc` in PATH (MinGW). For the fallback build, edit the `.bat`
to add `-DENABLE_INPROC_FALLBACK`.

### Linux / macOS

```bash
sudo apt install gcc-mingw-w64   # Debian/Ubuntu
brew install mingw-w64           # macOS
./build.sh
```

Output: `authlogin.dll` in the current directory.

### With the in-proc fallback enabled

```bash
i686-w64-mingw32-gcc -shared -o authlogin.dll authlogin.c -static \
    -Wl,--subsystem,windows -lws2_32 -DENABLE_INPROC_FALLBACK
```

## Test

The pure data layer is unit-tested:

```bash
cd tests
./build_test.sh   # cross-compiles and runs the test (needs wine on Linux)
# or
gcc -o test_shim test_shim.c && ./test_shim   # native build
```

Expected: `19 passed, 0 failed`.

The detour and the in-proc fallback are not unit-tested. They need a
live game process to exercise; verify them on a Windows box with the
actual game.

## Install

```bash
./install.sh /path/to/MCO/update
```

The DLL ends up in the game's `update` directory alongside
`MCity_d.exe`. The game loads it before the system `ws2_32`.

## Use it with your server

**Read [INTEGRATION.md](INTEGRATION.md).** Three scenarios, copy-paste
Python ticket validator, full ticket format spec, honest list of what
the shim does and doesn't do.

Quick start: edit `NPS_REDIRECT_HOST` in `authlogin.c`, build, install,
start your server on that host's port 8226.

```c
#define NPS_REDIRECT_HOST "192.168.1.42"   /* your server */
```

## Use in another project

Three common ways to consume this repo from a parent project:

### 1. Git submodule (versioned pin, easy updates)

```bash
git submodule add https://github.com/Dadud/mco-auth-bypass.git vendor/mco-auth-bypass
git -C vendor/mco-auth-bypass checkout v2.0.0
```

In your build:

```bash
cd vendor/mco-auth-bypass && ./build.sh && cp authlogin.dll ../../dist/
```

Update later: `git submodule update --remote vendor/mco-auth-bypass`.

### 2. CMake `FetchContent`

```cmake
include(FetchContent)
FetchContent_Declare(
  mco-auth-bypass
  GIT_REPOSITORY https://github.com/Dadud/mco-auth-bypass.git
  GIT_TAG        v2.0.0
)
FetchContent_MakeAvailable(mco-auth-bypass)
add_custom_target(authlogin.dll
  COMMAND ${CMAKE_COMMAND} -E env CC=i686-w64-mingw32-gcc
          ${mco-auth-bypass_SOURCE_DIR}/build.sh
  WORKING_DIRECTORY ${mco-auth-bypass_SOURCE_DIR}
  BYPRODUCTS ${mco-auth-bypass_SOURCE_DIR}/authlogin.dll
)
```

### 3. Plain `curl` (no git linkage)

```bash
curl -L https://github.com/Dadud/mco-auth-bypass/archive/refs/heads/main.tar.gz \
  | tar xz --strip-components=1 -C vendor/mco-auth-bypass
(cd vendor/mco-auth-bypass && ./build.sh)
cp vendor/mco-auth-bypass/authlogin.dll dist/
```

## License

MIT — see [LICENSE](LICENSE). Inherited legal notice regarding reverse
engineering applies. Not affiliated with EA.

## Origin

Extracted from [Dadud/OpenMCO](https://github.com/Dadud/OpenMCO)
(`tools/auth/authlogin.c`, commit `c58da14`).
