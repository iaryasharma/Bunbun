# Building Bunbun

Bunbun builds on Windows 10/11 (x64) with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) from
Git Bash. No Visual Studio needed. For the Linux build, see [Linux](#linux) at the end.

## Quick setup

```bash
./scripts/setup.sh
```

Downloads the pinned llvm-mingw, C++/WinRT and WebView2 SDK into `.toolchain/`, generates the
WinRT headers, and installs the settings UI's packages. Re-running it keeps what's already there.
You need Git Bash, Node.js 18+ and Python 3. The details below are only for doing it by hand.

## Requirements

| What | Where it goes | Notes |
|---|---|---|
| llvm-mingw (UCRT, x86_64) | `.toolchain/llvm-mingw-*-ucrt-x86_64/` | Or set `LLVM_MINGW` |
| C++/WinRT headers | `.toolchain/winrt-include/` | Or set `WINRT_INCLUDE` |
| WebView2 SDK | `.toolchain/webview2/` (NuGet `Microsoft.Web.WebView2`, extracted) | Or set `WEBVIEW2` |
| Node.js 18+ | on `PATH` | For the settings UI |
| Python 3 | on `PATH` | Code generators and tests |

Generate the C++/WinRT headers from this PC's Windows metadata with Microsoft's `cppwinrt.exe`
(NuGet `Microsoft.Windows.CppWinRT`):

```bash
cppwinrt.exe -in local -out .toolchain/winrt-include
```

Install the settings UI's packages once:

```bash
cd settings-ui && npm ci
```

`.toolchain/` and `node_modules/` are gitignored.

## Build

```bash
./build.sh
```

Outputs, all in `build/`:

- `bunbun.exe`: the island and tray app
- `bunbun-hook.exe`: the relay that coding agents run on each hook event
- `bunbun-settings.exe` and `WebView2Loader.dll`: the settings window
- `settings-ui/`: the settings UI, built with Vite
- `bunbun-brain.exe` (optional): the local AI helper, only after `./scripts/setup.sh --local-ai`
  fetched llama.cpp. The first build compiles llama.cpp once into `.toolchain/llama-build`. See
  [Local AI](local-ai.md); model files are never part of the build or the repository.

`./build.sh debug` builds without optimisation and with symbols; debug with
`.toolchain/llvm-mingw-*/bin/lldb.exe`. The version comes from the `VERSION` file.

## Test

```bash
./tests/run.sh
```

Runs the JSON, hook-installer and Ask Bun parser tests. With the island running, these exercise the
real hook and island:

```bash
python tests/test_dialects.py
```

```bash
python tests/simulate_agent.py demo
```

## Package a release

```bash
./scripts/package.sh
```

Builds, then writes `dist/Bunbun-<version>-win-x64.zip` and `dist/SHA256SUMS.txt`. Pass `--no-build`
to package what's already in `build/` (for example while the island is running). The zip is
portable: the exes, `WebView2Loader.dll`, `settings-ui/`, the licence and notices, and a
`START HERE.txt`. Everything is linked statically, so it runs on a clean Windows 10/11 x64.

**The installer.** With [Inno Setup](https://jrsoftware.org/isinfo.php) in the toolchain folder, the
same files also become `dist/Bunbun-<version>-setup.exe`:

```bash
./scripts/setup.sh --installer   # a portable Inno Setup in .toolchain/innosetup, once
./scripts/package.sh
```

The installer is `installer/bunbun.iss`. It installs per user without admin rights, adds Start menu
shortcuts and an uninstaller, offers "start with Windows", installs the WebView2 Runtime when it's
missing, and can download the recommended Local AI model (checked against its SHA-256) and run
`bunbun.exe --enable-local-ai`. Uninstalling quits Bunbun and disconnects the coding agents whose hooks call
this copy (`--disconnect <agent> --quiet --if-ours`), so none is left calling a deleted hook and
hooks pointing at another copy (a portable or development build) stay.

Releases are automated: CI builds and packages every push (the zip is attached to the run), and
pushing a tag that matches `VERSION` publishes a GitHub Release with one download, the installer,
plus its checksum and the CHANGELOG section as notes (the portable zip stays a run artifact). Release builds include the local AI helper
(`setup.sh --local-ai --installer`):

```bash
git tag v0.1.0
```

```bash
git push origin v0.1.0
```

## Working while the island runs

A running `bunbun.exe` can't be overwritten, so plain `./build.sh` fails while the island is up.
Use the dev script instead:

```bash
./scripts/dev.sh
```

It moves the running binaries aside, builds, and swaps in the new island with about a second of
downtime. If the build fails, the old binaries go back and the island keeps running. The hook is
replaced in place with a rename, so connected agents never see it missing.

| Mode | What it does |
|---|---|
| `./scripts/dev.sh` | Full rebuild and swap |
| `./scripts/dev.sh debug` | Same, debug build |
| `./scripts/dev.sh ui` | Rebuild only the settings UI and reopen its window |
| `./scripts/dev.sh stop` | Quit everything |

`bunbun.exe --quit` and `bunbun.exe --replace` also work by hand.

## Settings schema

Every setting is declared once in `src/island/settings_schema.yaml`. After changing it, regenerate
the island's defaults and the settings UI's schema:

```bash
sh scripts/gen_defaults.sh
```

```bash
python scripts/gen_schema.py
```

## Settings UI in a browser

```bash
cd settings-ui && npm run dev
```

A mock bridge answers instead of the real host. To load the dev server inside the real settings
window, start `build/bunbun-settings.exe` with `BUNBUN_UI_DEV=http://localhost:5173`. Add
`BUNBUN_DEVTOOLS=1` for DevTools.

## Diagnostics

| Variable | Effect |
|---|---|
| `BUNBUN_LOG=1` | Writes `%LOCALAPPDATA%\Bunbun\bunbun.log`, including one line a second with frames drawn, why, and time per frame |
| `BUNBUN_HOOK_DEBUG=1` | The hook explains on stderr why an event didn't arrive |

## Linux

The Linux build uses CMake and GTK. It's separate from `./build.sh`, which stays the Windows build.
What the Linux version does and how it's put together is in [Bunbun on Linux](linux.md).

**Packages** (Ubuntu 22.04 or newer, Debian 12 or newer):

```bash
sudo apt install build-essential cmake ninja-build pkg-config dpkg-dev libgtk-3-dev libsoup-3.0-dev libwebkit2gtk-4.1-dev libx11-dev desktop-file-utils meson libwayland-dev wayland-protocols
```

**gtk-layer-shell 0.10 or newer** is needed: the island's right-click menu crashes with 0.7, and
Ubuntu 22.04 ships 0.7. Build it once into `/usr/local` and point pkg-config at it:

```bash
./scripts/build-gtk-layer-shell.sh
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
```

CMake warns if it finds an older copy, and `scripts/package-linux.sh` refuses to package one.
Check which copy is found with `pkg-config --modversion gtk-layer-shell-0`.

On Fedora: `gcc-c++ cmake ninja-build gtk3-devel gtk-layer-shell-devel libsoup3-devel webkit2gtk4.1-devel`. On Arch:
`base-devel cmake ninja gtk3 gtk-layer-shell libsoup3 webkit2gtk-4.1`. Node.js 18+ builds the settings UI.

**Build:**

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build-linux
```

Outputs, in `build-linux/`: `bunbun` (the island), `bunbun-hook`, `bunbun-settings` and
`settings-ui/`. Run `build-linux/bunbun`. Options: `-DBUNBUN_BUILD_UI=OFF` skips the npm build (copy
a built `settings-ui/` there yourself); `-DBUNBUN_BUILD_SETTINGS=OFF` skips the settings window, so
WebKitGTK isn't needed.

**Test** with the island running:

```bash
python3 tests/test_dialects.py
```

```bash
python3 tests/simulate_agent.py demo
```

Both use `build-linux/bunbun-hook` on Linux, or the path in `BUNBUN_HOOK`.

**Package:**

```bash
./scripts/package-linux.sh
```

Writes `dist/Bunbun-<version>-amd64.deb`, `dist/Bunbun-<version>-x86_64.AppImage` and
`dist/SHA256SUMS-linux.txt`. It downloads `appimagetool` into `.toolchain/` the first time.
`--no-build` packages what's already built; `--appdir` stops at `dist/Bunbun.AppDir` (run its
`AppRun` to try it) without fetching `appimagetool`. Build packages on the oldest system you want to
support; CI uses Ubuntu 22.04, and every release includes both Linux packages.

**Diagnostics:** `BUNBUN_LOG=1` writes `~/.local/state/Bunbun/bunbun.log`, `BUNBUN_HOOK_DEBUG=1`
works as on Windows, `BUNBUN_UI_DEV` and `BUNBUN_DEVTOOLS=1` work for `bunbun-settings` too, and
`BUNBUN_X11_OVERRIDE=1` uses an override-redirect window on X11 instead of a dock.
