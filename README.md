# mco-auth-bypass

Replacement `authlogin.dll` for **Motor City Online** (2001, EA). Returns a
valid offline ticket for any credentials, so the game launches without
talking to EA's auth servers.

Extracted from [Dadud/OpenMCO](https://github.com/Dadud/OpenMCO), packaged
as a standalone drop-in for any other MCO-related project.

## What you get

| File | Purpose |
| --- | --- |
| `authlogin.c` | The DLL source. ~140 lines of C, single file, no deps. |
| `build.sh` | Cross-compile to `authlogin.dll` from Linux/macOS (needs `mingw-w64`). |
| `build_authlogin.bat` | Build on Windows (needs MinGW or MSVC). |
| `install.sh` | Copy a built DLL into a game's install tree. |
| `LICENSE` | MIT, with the same EA legal notice as OpenMCO. |

The DLL exports the same symbols EA's original `authlogin.dll` does
(`GetTicketSync[A|W]`, `AuthLogin_Init`, `AuthLogin_Shutdown`,
`AuthLogin_GetLastError`, `AuthLogin_GetServerName`,
`AuthLogin_SetServerName`, `AuthLogin_GetVersion`). All of them short-circuit
to success. The only network-aware function (`GetServerName`) still returns
`"www.ea.com"` so the game doesn't notice the server is gone.

## Build

### Windows

```cmd
build_authlogin.bat
```

Needs `gcc` in PATH (MinGW) or substitute `cl /LD authlogin.c /Fe:authlogin.dll` for MSVC.

### Linux / macOS

```bash
sudo apt install gcc-mingw-w64   # Debian/Ubuntu
brew install mingw-w64           # macOS
./build.sh
```

Output: `authlogin.dll` in the current directory.

## Install

Drop the built DLL into the game's `update` directory (the folder that
contains `MCity_d.exe` / `MCity.exe`).

### From this repo

```bash
./build.sh
./install.sh /path/to/MCO/update
```

### Manually

```bash
cp authlogin.dll /path/to/MCO/update/
```

The game's loader will pick up `authlogin.dll` from its own directory
before the system one. No registration, no config, no registry edits.

## Use in another project

Three common ways to consume this repo from a parent project:

### 1. Git submodule (versioned pin, easy updates)

```bash
git submodule add https://github.com/Dadud/mco-auth-bypass.git vendor/mco-auth-bypass
git -C vendor/mco-auth-bypass checkout v1.0.0
```

In your build:

```bash
cd vendor/mco-auth-bypass && ./build.sh && cp authlogin.dll ../../dist/
```

Update later: `git submodule update --remote vendor/mco-auth-bypass`.

### 2. CMake `FetchContent` (no submodule metadata, fully reproducible)

```cmake
include(FetchContent)
FetchContent_Declare(
  mco-auth-bypass
  GIT_REPOSITORY https://github.com/Dadud/mco-auth-bypass.git
  GIT_TAG        v1.0.0
)
FetchContent_MakeAvailable(mco-auth-bypass)
add_custom_target(authlogin.dll
  COMMAND ${CMAKE_COMMAND} -E env CC=i686-w64-mingw32-gcc
          ${mco-auth-bypass_SOURCE_DIR}/build.sh
  WORKING_DIRECTORY ${mco-auth-bypass_SOURCE_DIR}
  BYPRODUCTS ${mco-auth-bypass_SOURCE_DIR}/authlogin.dll
)
```

### 3. Plain `curl` (no git linkage, smallest footprint)

```bash
curl -L https://github.com/Dadud/mco-auth-bypass/archive/refs/heads/main.tar.gz \
  | tar xz --strip-components=1 -C vendor/mco-auth-bypass
(cd vendor/mco-auth-bypass && ./build.sh)
cp vendor/mco-auth-bypass/authlogin.dll dist/
```

Use this if you don't want a permanent connection to this repo in
`.gitmodules` or `CMakeLists.txt`.

## License

MIT — see [LICENSE](LICENSE). Inherited legal notice regarding reverse
engineering applies. Not affiliated with EA.

## Origin

Extracted from [Dadud/OpenMCO](https://github.com/Dadud/OpenMCO)
(`tools/auth/authlogin.c`, commit `c58da14`).
