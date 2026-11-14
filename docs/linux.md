# Bunbun on Linux

Bunbun runs on Linux as a `.deb` (Debian, Ubuntu and their derivatives) and an AppImage (most other
distributions). The Linux version is newer and smaller than the Windows one: it has the island
with the Studio layout, the clock, music, the mascot, coding agents (Allow / Deny, plans, questions
and replies typed on the island), and the settings app. The rest of this page says what works where,
what isn't there yet, and how the port is built.

## Requirements

| | Needs |
|---|---|
| **CPU** | 64-bit x86 (`x86_64` / `amd64`). There's no ARM build yet. |
| **C library** | glibc 2.35 or newer, and the C++ runtime from GCC 12 or newer (`libstdc++` with `GLIBCXX_3.4.30`). Every distribution from 2022 on has both. |
| **Toolkit** | GTK 3.22 or newer. Every GNOME, KDE, XFCE, Cinnamon and MATE desktop has it. |
| **Settings window** | WebKitGTK 4.1 (2.40 or newer). The `.deb` installs it. With the AppImage, install it yourself if the settings window doesn't open (the commands are below); without it the island still runs and opens `settings.ini` in a text editor instead. |
| **Desktop** | Any desktop on X11 or Wayland. See [What works where](#what-works-where) for the differences. |

**Tested on** (each one: the island starts and stays up, `bunbun --toggle` reaches it, the settings
window opens, and agent hooks run):

| Distribution | Version | glibc | Package |
|---|---|---|---|
| Ubuntu | 22.04 LTS, 24.04 LTS, 26.10 | 2.35, 2.39, 2.44 | `.deb` and AppImage |
| Linux Mint | 22 | 2.39 | `.deb` and AppImage |
| Debian | 12 (bookworm), 13 (trixie) | 2.36, 2.41 | `.deb` and AppImage |
| Kali Linux | Rolling | 2.43 | `.deb` and AppImage |
| Fedora | 41, 42 | 2.40, 2.41 | AppImage |
| Arch Linux | Rolling | 2.44 | AppImage |
| openSUSE | Tumbleweed, Leap 15.6 | 2.44, 2.38 | AppImage |

Distributions built on these work the same way: Pop!_OS, Zorin OS, elementary OS and Linux Mint 21
(Ubuntu 22.04 or newer), MX Linux (Debian 12 or newer), Manjaro and EndeavourOS (Arch).

**Not supported:** systems older than glibc 2.35. That includes Ubuntu 20.04, Debian 11, openSUSE
Leap 15.5, and RHEL, AlmaLinux, Rocky Linux and CentOS Stream 9 (glibc 2.34): Bunbun won't start
there.

## Install

**Debian, Ubuntu, Linux Mint, Pop!_OS, Kali** (Ubuntu 22.04 or Debian 12 and newer):

```bash
sudo apt install ./Bunbun-<version>-amd64.deb
```

Then start **Bunbun** from your app menu, or run `bunbun`. Remove it with `sudo apt remove bunbun`.

**Fedora, Arch, openSUSE and any other distribution** with glibc 2.35 or newer:

```bash
chmod +x Bunbun-<version>-x86_64.AppImage
```

```bash
./Bunbun-<version>-x86_64.AppImage
```

`./Bunbun-<version>-x86_64.AppImage settings` opens the settings window directly. If the AppImage
doesn't start because your system has no FUSE, add `--appimage-extract-and-run`.

The AppImage brings its own libraries for the island. The settings window uses your system's
WebKitGTK; most GNOME desktops already have it. If the settings window doesn't open, install it:

```bash
sudo dnf install webkit2gtk4.1
```

```bash
sudo pacman -S webkit2gtk-4.1
```

```bash
sudo zypper install libwebkit2gtk-4_1-0
```

(Fedora, Arch and Manjaro, openSUSE, in that order.)

**Start at login:** Settings → General → Start when you log in. It writes
`~/.config/autostart/bunbun.desktop`, which GNOME, KDE, XFCE, Cinnamon and MATE all read.

**Connect your coding agent:** right-click the island → **Connect Claude Code**, or Settings →
Assistant & agents for Claude Code, Codex, Antigravity, Cursor and Copilot. It works as on Windows; see
[Coding agents](agents.md).

## What works where

| Desktop | Session | How the island is shown | Status |
|---|---|---|---|
| KDE Plasma 5.27 / 6 | Wayland | layer-shell surface | Supported |
| Sway, Hyprland, river, Wayfire, labwc | Wayland | layer-shell surface | Supported (tested on Sway) |
| GNOME | Wayland | through XWayland (see below) | Supported, with caveats |
| GNOME, KDE, XFCE, Cinnamon, MATE | X11 | always-on-top dock window | Supported |
| i3 and other X11 window managers | X11 | dock window; override-redirect with `BUNBUN_X11_OVERRIDE=1` | Works; no shadow without a compositor |

**GNOME on Wayland** has no layer-shell protocol, so a Wayland window can't place itself at the top
of the screen or stay above others. Bunbun notices this and restarts itself through XWayland, where
it can. This needs XWayland, which GNOME ships by default. On a fractional scale (125 %, 150 %) the
island may look a little soft, because GNOME scales XWayland windows. A GNOME Shell extension that
offers layer-shell would let Bunbun draw natively; there isn't a maintained one yet.

## Studio on Linux

The island opens into the Studio frame when the pointer rests on it, as on Windows: the page name,
a tiny mascot, the date and time, the settings gear, the Claude Code usage ring, and a tab bar with
one icon per page. Scroll or click the tab bar to change pages. The pages, and where Linux gets
their data:

| Page | On Linux |
|---|---|
| **Home** | Three cards picked in Settings → Island pages (Agent, Music, Focus, Weather, Calendar, Battery, System) and **Your links** under them |
| **Music** | While something plays (MPRIS): cover, title, artist, album, controls, a glow in the album's colour; click the progress bar to jump to that point. With several players running (say Spotify and a video in the browser) the player's name shows how many, and a click on it shows the next one; right-click → **Media source** lists them and has **Automatic**. A player you pick stays on show until it stops, then the island chooses again (a playing player first, then the most recent) |
| **Lyrics** | Synced lyrics for the song that's playing, the current line in the middle (Features → Lyrics) |
| **Calendar** | The month, today, and today's to-dos as an agenda. Calendar links (`.ics`) aren't read on Linux yet |
| **Weather** | Open-Meteo, like Windows: now (feels like, humidity, wind, UV, sunrise and sunset), the next hours as a curve, and five days. °C / °F and Change City in the header. Your city from Settings, else roughly where you are from your IP address. Fetched with GIO, or `curl` where GIO can't do https |
| **System** | Battery (health and time left), network with Wi-Fi and Bluetooth switches, CPU, memory, disk and GPU (AMD and Intel report it) with small waves, and volume with mute and a slider (`pactl`), from `/proc`, `/sys`, NetworkManager and BlueZ |
| **Quick toggles** | Wi-Fi (NetworkManager), Bluetooth (BlueZ) and Do not disturb (GNOME's notification banners). Its own page in Classic; in Studio the switches are on the System page |
| **To-do** | The same `todos.tsv` as the settings app; click to tick one off. Reminders show on the island when they're due |
| **Agent** | What the agent is doing, its command, diff or recent tasks, the steps or your Claude Code limits, the model chip |
| **Clipboard** | The last 40 texts and links you copied, in two panels: filter and pick on the left, the whole copy with Copy and Delete on the right. No images yet. On Wayland it may miss copies made while another app has the keyboard |
| **Note** | `note.txt` next to `settings.ini`; click a `[ ]` line to tick it, **Edit** opens it in your text editor |
| **Timers** | Like Windows: **Focus** (Pomodoro with short and long breaks, auto-start, length chips), **Timer** (click or scroll the dial, Shift for seconds, your own presets), **Stopwatch** with laps, and **Water** as a mode. The island tells you when a phase or a countdown ends |
| **Screen Time** | How long each app has been in front today and over the week, with each app's own name and icon (from its `.desktop` entry and your icon theme), and an optional daily limit. **X11 only**: Wayland doesn't let an app see which window is in front, so on Wayland the page says so. Kept in `screentime.tsv` in the state folder |
| **Water** | A mode of the Timers page: today's water against your goal, + glass, undo and the week. Reminders (Features → Water) come as a toast: click it to log the glass |
| **All apps** | Every page and Settings in one grid |

**Page order** (Settings → Island pages) orders these the same way as on Windows; the pages Linux
doesn't have yet (File shelf, Mirror) are left out.

**Lyrics** come from the same places as on Windows: LRCLIB first, then NetEase (Features → Lyrics →
NetEase fallback), matched to the song by title, artist and length. Each song is looked up once and
kept in `~/.cache/Bunbun/lyrics` (Features → Lyrics sets the cache's size). **Lyrics on the small
island** (Features → Lyrics, or right-click the island) shows the current line in the music pill:
the pill grows to fit the song's longest line, a long line gets a little smaller and then an
ellipsis, and each new line rolls up from below. The timing follows the player's position over MPRIS;
**Lyrics sync offset** (Features → Lyrics) moves it earlier or later. Lyrics are fetched with libsoup 3, which the
`.deb` installs and the AppImage carries. Island pages shows real pictures of your island's pages: the island
draws them in the background when the screen opens, and whenever you leave a page open for a moment.
**Expand on hover** off makes a click open and close the island instead. A plan to approve and several questions at once get their own cards,
with Approve / Change it… and one question at a time. **Reply…** on a finished agent, **Change it…**
on a plan and **Other…** on a question let you type on the island: the keyboard goes to the island
while you type (Enter sends, Esc cancels) and back to your windows after.

**Look and sounds** follow the same settings as on Windows: the shape (the Notch, flush with the top
edge and the default; Pill; or Windows 11), the theme presets and Custom colours (background, primary
and secondary text, border), Fill (Transparent, or Opaque in one solid colour), the border, the drop
shadow, depth shading, accent bloom, tint, font and text size, and the island's size (Size scale,
0.6x to 2.5x, and Match screen size, which grows it on bigger screens; GTK's own desktop scaling
applies on top, so there's no Auto DPI scaling here). The chimes (agent done, a question, an
error, the open / close blips, reminders) play through `paplay`, or `pw-play` or `aplay` when that's
what the system has; the `.deb` recommends one. **Privacy dots** show on the small island while an app
uses the camera (green: one of your processes has a `/dev/video*` device open) or the microphone
(orange: an ALSA capture stream is running, which is also true under PipeWire and PulseAudio).

**Where it sits and when it hides** follow Appearance and Behavior too: top or bottom, left, centre
or right, on the monitor you pick (or the one your pointer is on), Offset X / Y and a separate offset
while open, Border-merged mode, Always on top; auto-hide after a while, hide or tuck in full screen,
and unhide on hover. Full-screen detection works on X11 (including GNOME, which runs Bunbun through
XWayland); on a Wayland compositor with layer-shell the island sits under full-screen windows by
itself, and Tuck away is in the right-click menu.

**The hide (Ctrl+Alt+D) and tuck (Ctrl+Alt+H) hotkeys** work everywhere, set up in one of three ways:

- **X11 desktops:** Bunbun grabs the keys itself.
- **GNOME on Wayland** (Ubuntu's default): apps can't grab keys there, so Bunbun adds two GNOME
  custom shortcuts for you, "Bunbun: hide or show the island" and "Bunbun: tuck the island away"
  (Settings → Keyboard → View and Customize Shortcuts → Custom Shortcuts). They follow the keys you
  pick in Bunbun's settings, and go away when you turn the hotkeys off.
- **KDE, Sway, Hyprland and other Wayland desktops:** bind a key to `bunbun --toggle` (hide or show)
  and `bunbun --tuck` in your desktop's keyboard settings. With the AppImage, use its path:
  `/path/to/Bunbun-<version>-x86_64.AppImage --toggle`.

**Alerts** appear on the island as on Windows: low battery and charging (with time to full),
Caps Lock and Num Lock, brightness (from `/sys/class/backlight`), volume (`pactl subscribe`),
Bluetooth devices connecting and running low (BlueZ), Do not disturb, and a preview of what you copy.
The motion styles and speeds, frame rate and battery saver, the idle eyes face, the music-pill options
(title, clock, auto-expand, swipe to skip), the progress and spectrum styles, karaoke lyrics, the date
format and the first day of the week all work as on Windows.

**Pages and extras** that came later: the **File shelf** (drop files on the island; click to open,
drag out again; kept in `$XDG_STATE_HOME/Bunbun/shelf.txt`), the **Mirror** camera page (V4L2,
`/dev/video*`, only while you've turned it on), **calendar links** (today's meetings on the Calendar
page and a meeting pill with Join before each one), **island copies on other displays**
(Appearance → Displays), **music bars from the real audio** (`parec` on the default output's
monitor; the bars animate without it), and **Antigravity's quota** (its `state.vscdb`, read with the
system's `libsqlite3.so.0`, loaded at run time). The **blue privacy dot** lights while the screen is
shared or recorded: PipeWire screencasts, browsers sharing on X11, OBS outputs, and recorders such as
ffmpeg, wf-recorder, gpu-screen-recorder and SimpleScreenRecorder; the island names the app and says
whether it's streaming, recording or sharing.

**The tray icon** is your mascot in the panel with the same menu as on Windows (the version, hide /
show the island, Settings, Pick a colour, Connect Claude Code, Start when you log in, Quit…). It uses
AppIndicator (KDE, Ubuntu's GNOME, XFCE, Cinnamon, MATE, Budgie), loaded at run time from
`libayatana-appindicator3` or `libappindicator3` (the `.deb` recommends it), and the classic X11 tray
where that's all the panel has. **The colour picker** (the eyedropper in the Studio header, Colour in
All apps, the right-click and tray menus, or `bunbun --pick-colour` bound to a key in your desktop's
keyboard settings) works as on Windows on X11: the screen freezes under a loupe, a click picks, arrow
keys nudge by a pixel, Esc cancels; the hex is copied and the island shows HEX, RGB and HSL, a click
copying each. On Wayland, where apps can't see the screen, it uses the desktop's own picker through
the screenshot portal (`xdg-desktop-portal`).

**The notch and the top bar.** The notch hangs from the very top edge of the screen, over any top
panel, on KDE, Sway, Hyprland and other desktops. GNOME draws its top bar over every app window, so
there the notch sits directly under the bar instead; hide the top bar (for example with the *Hide Top
Bar* or *Just Perfection* extension) and it moves up to the edge by itself.

**Without a compositor** (some X11 window managers, or XFCE with compositing off) transparency isn't
possible, so the window is cut to the pill's shape instead: the island still looks like a pill, only
without its soft shadow.

## What isn't on Linux yet

| Feature | Why | Plan |
|---|---|---|
| Ask Bun (Ctrl+Alt+B) | Its commands open Windows apps and Settings pages | Port the commands to Linux equivalents (`xdg-open`, desktop files, `pactl`) |
| Discord sharing on X11 | Discord reads the screen itself there and leaves no sign | Detected on Wayland, through PipeWire |
| A tray icon on plain GNOME | GNOME shows tray icons only with the AppIndicator extension (Ubuntu has it on) | Right-click the island for the same menu |
| Live island preview in Settings, character-sheet import, Local AI, the island's languages | Windows-only for now | Later |

Settings for features that aren't on Linux yet still show in the settings app, marked **Not on Linux
yet** (in Island pages, Features and the other screens), so it's clear which switches do something.
Settings that stay Windows-only are hidden on Linux: Windows' own blur and DPI scaling, the update
check (your package manager updates Bunbun), the game overlay, the downloads and progress alerts,
notifications and poke reactions.

## Files

| What | Where |
|---|---|
| Settings | `~/.config/Bunbun/settings.ini` (same keys as on Windows; plain UTF-8) |
| Skin packs | `~/.config/Bunbun/mascots/<name>/` |
| Log (with `BUNBUN_LOG=1`) | `~/.local/state/Bunbun/bunbun.log` |
| Agent socket, instance lock | `$XDG_RUNTIME_DIR/bunbun/` (only your user can open it) |
| Hook used by agents (AppImage) | `~/.local/share/Bunbun/bin/bunbun-hook` |
| Programs (`.deb`) | `/usr/lib/bunbun/`, with `/usr/bin/bunbun` |

`bunbun --quit` closes a running island; `bunbun --replace` restarts it; `bunbun --settings` opens
the settings window.

## How the port is built

### What is shared

These parts are the same code on both systems:

- **The mascot** (`src/mascot/`): every character and mood, and skin packs. It's written against
  Direct2D; on Linux it compiles unchanged against a small Direct2D implementation on Cairo (see
  [Compatibility layer](#compatibility-layer)), so a new mood or character arrives on both systems at once.
- **The Studio page model and icons** (`src/island/studio_pages.h`, `studio_icons.h`): which pages
  there are and in what order, their names and colours, the Home cards and links, how plan and
  question replies are built, and every tab icon (drawn through the same Direct2D stand-ins as the
  mascot). Unit-tested by `tests/test_studio.cpp` on both systems. The frame itself is drawn twice:
  Direct2D in `studio_draw.inc`, Cairo in `studio_linux.cpp`.
- **The agent bridge** (`src/agents/agent_bridge.cpp`): sessions, phases, the approval queue and
  question answers. Only its transport differs: a named pipe on Windows, a Unix socket on Linux.
- **The hook** (`src/hook/`): every agent dialect and reply format; again only the transport differs.
- **The hook installers** (`src/agents/hook_installer.cpp`): Claude Code (with its statusLine and
  reply hook), Codex, Antigravity, Cursor and Copilot configs are merged the same way.
- **Settings**: the schema (`settings_schema.yaml`), the generated defaults and `settings.ini`'s rules
  (keys written commented out, so defaults stay live).
- **The settings UI** (`settings-ui/`): the same React build. It talks to `window.chrome.webview`;
  the Linux host provides that object on WebKitGTK, so the UI has no Linux branch.
- **JSON** (`src/common/json.h`).

`src/assistant/` (the Ask Bun parser) and `src/common/todos.h` are portable too, but their callers
are Windows-only for now.

### What each system does its own way

| Need | Windows | Linux |
|---|---|---|
| The overlay window | Layered Win32 window, `UpdateLayeredWindow` | GTK 3 window: a wlr-layer-shell surface (gtk-layer-shell) on Wayland, an always-on-top dock or override-redirect window on X11; input region = the pill |
| Drawing | Direct2D + DirectWrite | Cairo + Pango (the mascot through the Direct2D stand-ins) |
| Settings window | WebView2 | WebKitGTK 4.1, UI served from a private `bunbun-ui://` scheme |
| Now playing | Media session (GSMTC) | MPRIS on the session D-Bus |
| Agent channel | Named pipe `\\.\pipe\bunbun-<SID>`, owner-only DACL | Unix socket `$XDG_RUNTIME_DIR/bunbun/agent.sock`, folder mode 0700, both ends check `SO_PEERCRED` |
| Single instance | Named mutex | `flock` on `$XDG_RUNTIME_DIR/bunbun/island.lock` |
| Settings file watching | `ReadDirectoryChangesW` | `GFileMonitor` |
| Autostart | `HKCU\…\Run` | `~/.config/autostart/bunbun.desktop` |
| Tray | Shell notify icon | Not yet (StatusNotifierItem planned); right-click menu on the island |
| Volume / audio level | Core Audio | Planned: PipeWire or PulseAudio |
| Wi-Fi / Bluetooth | WLAN API, WinRT radios | Planned: NetworkManager, BlueZ (D-Bus) |
| Battery | `GetSystemPowerStatus` | Planned: UPower (D-Bus) |
| Notifications | WinRT notification listener | Planned: `org.freedesktop.Notifications` |
| Brightness | WMI | Planned: logind `SetBrightness` or `brightnessctl`; not every display allows it |
| Location for weather | Windows location API | Planned: GeoClue, or the city from settings |
| Screen text lookup | Windows OCR | Dropped on Linux |

### Code layout

```
src/
  island/, settings/, main.cpp     Windows island, settings host and tray (unchanged)
  mascot/, agents/, hook/,         shared; Windows-only lines inside are behind #ifdef _WIN32
  common/, assistant/
  platform/linux/
    island_linux.cpp               the island: window, layout, drawing, input
    studio_linux.cpp               the Studio frame, its pages, plan / question cards, typing
    mpris.cpp                      now playing over D-Bus
    settings.cpp                   settings.ini and the generated defaults
    settings_host_linux.cpp        bunbun-settings on WebKitGTK
    platform.cpp                   XDG folders, UTF-8, starting programs
    image_codec.cpp                skin-pack images through GdkPixbuf
    mascot_png.cpp                 mascot frames as PNG (previews, the app icon)
    compat/                        Win32 and Direct2D stand-ins for shared code
packaging/linux/                   desktop entry, AppStream metadata
CMakeLists.txt                     the Linux build
scripts/package-linux.sh           .deb and AppImage
```

The Windows code stays where it is and builds exactly as before with `./build.sh`; nothing in
`src/platform/linux/` is part of it. As more of the Windows island is shared, those pieces move out
of `island_core.cpp` into portable modules, with a `src/platform/win/` counterpart for the parts that
stay Windows-specific.

### Compatibility layer

`src/platform/linux/compat/` holds a `windows.h`, `d2d1.h` and friends that only the Linux build sees,
and only for shared files that still include them:

- **Direct2D on Cairo** (`d2d1.h`, `d2d_cairo.cpp`): render targets, brushes (solid, radial gradient,
  bitmap), path geometries with Béziers and arcs, layers with geometric and opacity masks, axis-aligned
  clips, bitmaps. Exactly what the mascot uses; anything else is a compile error rather than a silent
  difference. `bbcompat::CreateCairoTarget()` and `bbcompat::SetCairo()` connect it to a `cairo_t`.
- **Win32 basics** (`windows.h`, `shlobj.h`, `win32_compat.cpp`): files, environment, known folders
  mapped to XDG folders, INI reading, UTF-8 conversion. Paths with backslashes are converted to
  slashes.

New Linux code doesn't use these: it talks to POSIX, GLib and GTK directly. They exist so the
mascot and the hook installers stay one copy.

### Packaging

`scripts/package-linux.sh` builds with CMake (`-DCMAKE_INSTALL_PREFIX=/usr`), installs into a staging
tree, and makes both packages from it:

- the **`.deb`** takes its dependencies from the binaries (`dpkg-shlibdeps`), so on newer releases
  the 64-bit-time package names (`libgtk-3-0t64`) satisfy them. It also ships gtk-layer-shell 0.10 in
  `/usr/lib/bunbun/lib`, which the island loads before the system's copy, so it doesn't depend on
  `libgtk-layer-shell0`;
- the **AppImage** carries the programs, the settings UI, gtk-layer-shell 0.10 and libsoup 3 (for the
  island only: what the island opens, the settings window included, gets the system's libraries),
  and uses the system's
  GTK 3 and WebKitGTK. Without WebKitGTK the island opens `settings.ini` in a text editor instead of
  the settings window. Because the AppImage's folder changes on every run, agent hooks point at a
  copy of `bunbun-hook` kept in `~/.local/share/Bunbun/bin`; the hook needs only the C library.

Both are built on Ubuntu 22.04 in CI, so they run on glibc 2.35 and newer: Ubuntu 22.04+, Debian 12+,
Fedora 36+, current Arch and openSUSE Tumbleweed. Building is covered in [Building](building.md#linux).

### Testing

The island has been run end to end on X11 (Openbox, with and without a compositor) and on Wayland
(Sway): clock, music with cover art, the hover card and its controls, agent pills, and an approval
answered with a real click. The Studio frame, its pages, the plan and question cards and typing a
reply have been checked the same way, on X11 and on Sway (`tests/simulate_agent.py plan` and
`questions` drive the cards). `tests/test_dialects.py` and `tests/simulate_agent.py` run against the
Linux hook (they find `build-linux/bunbun-hook`, or set `BUNBUN_HOOK`). Real GNOME and KDE sessions
haven't been tested yet.
