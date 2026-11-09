# Third-Party Notices & Acknowledgements

Bunbun incorporates ideas and components under open-source software licenses.

| Component | Description | License |
|---|---|---|
| [Dynamic Island for Windows](https://windhawk.net/mods/dynamic-island-for-windows) (Windhawk mod v1.4.0) | Island core: Direct2D renderer, media, lyrics, weather, hardware, battery, clipboard, notification, and file-tray modules | MIT |
| [Coucou](https://github.com/louis-cfm/coucou) | Coding agent hook protocol inspiration (re-implemented in C++) | MIT (code) |
| [C++/WinRT](https://github.com/microsoft/cppwinrt) | WinRT projection headers compiled into Bunbun | MIT |
| [WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2) | Settings window WebView2 runtime host (`WebView2Loader.dll`) | BSD-3-Clause |
| React, React DOM | Settings UI | MIT |
| Framer Motion | Settings UI animations | MIT |
| Lucide React | Settings UI icons | ISC |
| Vite & TypeScript | Settings UI build tooling | MIT / Apache-2.0 |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | Optional local AI helper (`bunbun-brain.exe`); built only when fetched with `scripts/setup.sh --local-ai`. Model files are not part of Bunbun: each carries its own licence | MIT |
| [GTK Layer Shell](https://github.com/wmww/gtk-layer-shell) v0.10.0 | Layer-shell support for the Linux island. Shipped unmodified as `libgtk-layer-shell.so.0` in the Linux packages (the source is the linked tag) | LGPL-3.0-or-later (most files MIT) |
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | Statically linked C++ toolchain & runtime (libc++, mingw-w64 CRT) | Apache-2.0 with LLVM Exception |

### Exclusions & Proprietary Assets
- **Coucou Assets**: Coucou's branding, the Mochi character, audio assets, and artworks are not used. Bunbun's mascots (Bun, Miso, Biscuit, Ember, Bao, Bolt) are original and generated procedurally in code (`src/mascot/mascot.cpp`).
- **Bloom**: Studied for design concepts only (GPL-3.0); no code from Bloom is included.
