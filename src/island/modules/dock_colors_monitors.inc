// Bunbun: Source dock, colour maths, fullscreen checks, monitors and accent.
// Split out of island_core.cpp; included inside its anonymous namespace at the
// original position, so declaration order is unchanged. Not a standalone file.

// ── Source dock: slot layout + hit testing ──────────────────────────────────
struct DockSlots {
    int count = 0;                      // slots drawn
    int sourceIndex[3] = {-1, -1, -1};  // index into mediaSources; -1 = the "+N" slot
    int overflow = 0;                   // N in "+N" (0 = none)
};

// Drawing and hit testing call this with the same inputs, so they always agree.
DockSlots ComputeDockSlots(int sourceCount, int activeIndex) {
    DockSlots d;
    if (sourceCount < 2) return d;
    if (sourceCount <= MediaLayout::kDockMaxSlots) {
        d.count = sourceCount;
        for (int i = 0; i < sourceCount; ++i) d.sourceIndex[i] = i;
    } else {
        d.count = 3;  // first, the active one, and "+N" for the rest
        d.sourceIndex[0] = 0;
        d.sourceIndex[1] = activeIndex >= 2 ? activeIndex : 1;
        d.sourceIndex[2] = -1;
        d.overflow = sourceCount - 2;
    }
    return d;
}

// Slot under a content-space point, or -1. Boundaries sit in the middle of the gaps,
// so the targets tile the pill (28px+ each) with no dead space.
int MediaSourceDockHitTest(const MediaContentPoint& rawPt, int slotCount) {
    const MediaContentPoint pt = MediaPlayerZonePoint(rawPt);
    if (slotCount < 2 || !pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return -1;
    }
    const float halfH = std::max(MediaLayout::kDockHitMin, MediaLayout::kDockHeight) * 0.5f;
    if (std::fabs(pt.y - MediaLayout::kControlsY) > halfH) return -1;

    const float left = MediaLayout::kDockLeft;
    const float right = left + MediaLayout::DockWidth(slotCount);
    if (pt.x < left || pt.x > right) return -1;

    const float stride = MediaLayout::kDockSlot + MediaLayout::kDockGap;
    const float origin = left + MediaLayout::kDockPad - MediaLayout::kDockGap * 0.5f;
    const int index = static_cast<int>(std::floor((pt.x - origin) / stride));
    return ClampInt(index, 0, slotCount - 1);
}

struct DockHit {
    int slot = -1;
    bool overflow = false;  // the "+N" slot
    bool isActive = false;
    std::wstring aumid;
};

DockHit ResolveSourceDockHit(const MediaContentPoint& pt) {
    DockHit hit;
    std::lock_guard lock(g_stateMutex);
    const int count = static_cast<int>(g_state.mediaSources.size());
    const int active = ActiveMediaSourceIndex(g_state);
    const DockSlots slots = ComputeDockSlots(count, active);
    const int slot = MediaSourceDockHitTest(pt, slots.count);
    if (slot < 0) return hit;

    hit.slot = slot;
    const int idx = slots.sourceIndex[slot];
    if (idx < 0) {
        hit.overflow = true;
        return hit;
    }
    hit.aumid = g_state.mediaSources[idx].aumid;
    hit.isActive = (idx == active);
    return hit;
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) {
            return false;
        }
    }

    return true;
}

std::wstring GetStringSettingCopy(PCWSTR name) {
    PCWSTR value = Wh_GetStringSetting(name);
    std::wstring result = value ? value : L"";
    Wh_FreeStringSetting(value);
    return result;
}

std::wstring GetStringSettingWithFallback(PCWSTR primary, PCWSTR fallback, PCWSTR fallback2 = nullptr) {
    std::wstring result = GetStringSettingCopy(primary);
    if (!result.empty()) {
        return result;
    }
    result = GetStringSettingCopy(fallback);
    if (!result.empty()) {
        return result;
    }
    if (fallback2) {
        return GetStringSettingCopy(fallback2);
    }
    return L"";
}



D2D1_COLOR_F ColorFromHex(std::wstring text, D2D1_COLOR_F fallback) {
    // Trim surrounding whitespace: a stray space used to make the whole value
    // fail to parse and silently fall back to the default color.
    const size_t firstChar = text.find_first_not_of(L" \t\r\n");
    if (firstChar == std::wstring::npos) {
        return fallback;
    }
    text = text.substr(firstChar, text.find_last_not_of(L" \t\r\n") - firstChar + 1);

    if (!text.empty() && text[0] == L'#') {
        text.erase(text.begin());
    }

    // Accepted forms: RGB, RGBA, RRGGBB, RRGGBBAA. The alpha-bearing forms are
    // how a translucent island background is specified independently of the
    // global pill transparency slider.
    const size_t digits = text.size();
    if (digits != 3 && digits != 4 && digits != 6 && digits != 8) {
        return fallback;
    }
    if (text.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) {
        return fallback;
    }

    auto nibble = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        return c - L'A' + 10;
    };

    int r = 0, g = 0, b = 0, a = 255;
    if (digits == 3 || digits == 4) {
        // Shorthand, each digit doubled so 'f' means 0xff.
        r = nibble(text[0]) * 17;
        g = nibble(text[1]) * 17;
        b = nibble(text[2]) * 17;
        if (digits == 4) {
            a = nibble(text[3]) * 17;
        }
    } else {
        r = nibble(text[0]) * 16 + nibble(text[1]);
        g = nibble(text[2]) * 16 + nibble(text[3]);
        b = nibble(text[4]) * 16 + nibble(text[5]);
        if (digits == 8) {
            a = nibble(text[6]) * 16 + nibble(text[7]);
        }
    }

    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static float HueToRgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

D2D1_COLOR_F HslToRgb(float h, float s, float l, float a = 1.0f) {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    s = Clamp(s, 0.0f, 1.0f);
    l = Clamp(l, 0.0f, 1.0f);

    if (s <= 1e-5f) {
        return D2D1::ColorF(l, l, l, a);
    }

    const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
    const float p = 2.0f * l - q;
    const float hNorm = h / 360.0f;

    const float r = Clamp(HueToRgb(p, q, hNorm + 1.0f / 3.0f), 0.0f, 1.0f);
    const float g = Clamp(HueToRgb(p, q, hNorm), 0.0f, 1.0f);
    const float b = Clamp(HueToRgb(p, q, hNorm - 1.0f / 3.0f), 0.0f, 1.0f);

    return D2D1::ColorF(r, g, b, a);
}

void RgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    r = Clamp(r, 0.0f, 1.0f);
    g = Clamp(g, 0.0f, 1.0f);
    b = Clamp(b, 0.0f, 1.0f);

    const float maxVal = std::max({r, g, b});
    const float minVal = std::min({r, g, b});
    const float delta = maxVal - minVal;

    l = (maxVal + minVal) * 0.5f;

    if (delta <= 1e-5f) {
        h = 0.0f;
        s = 0.0f;
        return;
    }

    s = (l > 0.5f) ? (delta / (2.0f - maxVal - minVal)) : (delta / (maxVal + minVal));

    if (maxVal == r) {
        h = ((g - b) / delta) + (g < b ? 6.0f : 0.0f);
    } else if (maxVal == g) {
        h = ((b - r) / delta) + 2.0f;
    } else {
        h = ((r - g) / delta) + 4.0f;
    }
    h *= 60.0f;
    if (h < 0.0f) h += 360.0f;
    if (h >= 360.0f) h -= 360.0f;
}

double RelativeLuminance(D2D1_COLOR_F c) {
    auto toLinear = [](float channel) -> double {
        const double v = Clamp(channel, 0.0f, 1.0f);
        return (v <= 0.04045) ? (v / 12.92) : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * toLinear(c.r) + 0.7152 * toLinear(c.g) + 0.0722 * toLinear(c.b);
}

// Nudges an accent until it clears 3:1 against the surface it will sit on.
//
// This used to only ever *lighten*, which silently assumed a dark island. On a
// light background lightening reduces contrast, so the loop would run all the
// way to l = 0.95 and hand back a near-white accent that vanished into the
// surface. The step direction is now chosen from the background's luminance,
// which is what lets a light theme (Porcelain) keep a readable accent.
D2D1_COLOR_F EnsureContrastAgainstBackground(D2D1_COLOR_F candidate, D2D1_COLOR_F bgColor) {
    float h = 0.0f, s = 0.0f, l = 0.0f;
    RgbToHsl(candidate.r, candidate.g, candidate.b, h, s, l);

    if (s > 0.01f) {
        s = std::max(s, 0.35f);
    }

    const double bgLum = RelativeLuminance(bgColor);
    const bool onDark = bgLum < 0.45;

    // Clamp toward the half that can actually move away from the background.
    l = onDark ? std::min(l, 0.85f) : std::max(l, 0.15f);

    const float step = onDark ? 0.02f : -0.02f;
    const float limit = onDark ? 0.95f : 0.06f;

    candidate = HslToRgb(h, s, l, candidate.a);
    auto contrastOf = [&](const D2D1_COLOR_F& c) {
        const double lum = RelativeLuminance(c);
        return (std::max(bgLum, lum) + 0.05) / (std::min(bgLum, lum) + 0.05);
    };

    double contrast = contrastOf(candidate);
    // Bounded by `limit` in both directions, so this terminates either way.
    while (contrast < 3.0 && (onDark ? (l < limit) : (l > limit))) {
        l += step;
        candidate = HslToRgb(h, s, l, candidate.a);
        contrast = contrastOf(candidate);
    }

    return candidate;
}

// Returns true if the currently active foreground window is in full screen mode
bool IsForegroundFullscreen(HWND targetHwnd);
bool IsFullscreenWindow(HWND fg, HWND targetHwnd);

// Bunbun: the program behind a window (the foreground one by default), lower case without ".exe".
std::wstring ForegroundAppName(HWND window = nullptr) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window ? window : GetForegroundWindow(), &pid);
    HANDLE proc = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!proc) return {};
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        name = path;
        name = name.substr(name.find_last_of(L'\\') + 1);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
    }
    CloseHandle(proc);
    return bb::applist::Lower(name);
}

// Bunbun: does anything need full screen watched at all (In full screen, or a per-app list)?
bool FullscreenWatched(const Settings& s) {
    return s.autoHideFullscreen || !s.fullscreenTuckApps.empty() || !s.fullscreenHideApps.empty();
}

// Bunbun: the full-screen window to act on: the foreground one, else (with several displays) one that
// is still full screen on a display you looked away from. Focusing another display used to unfold
// the island over the game or film still playing on the first. `onlyMonitor` limits that search to one
// display (null: any), for when the island lives on a single display.
HWND FullscreenWindow(HWND islandHwnd, HMONITOR onlyMonitor, bool anyDisplay) {
    HWND fg = GetForegroundWindow();
    if (IsFullscreenWindow(fg, islandHwnd)) return fg;
    if (GetSystemMetrics(SM_CMONITORS) < 2) return nullptr;
    // Only a display you looked away from counts: on the display you are using, a full-screen window
    // behind your current one (alt-tabbed away) is no reason to keep the island folded.
    struct Ctx { HWND island, found; HMONITOR only, fgMon; bool any; } ctx{
        islandHwnd, nullptr, onlyMonitor, fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) : nullptr, anyDisplay};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(w) || IsIconic(w)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(w, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
        const HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        if (mon == c->fgMon || (!c->any && mon != c->only)) return TRUE;
        if (!IsFullscreenWindow(w, c->island)) return TRUE;
        c->found = w;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// Bunbun: what the island does for the app in full screen right now: 0 = nothing
// (not full screen, or Show), 1 = tuck, 2 = hide. The per-app lists win over
// In full screen.
int FullscreenAction(HWND islandHwnd, const Settings& s, HWND* fsWindow = nullptr, HMONITOR onlyMonitor = nullptr, bool anyDisplay = false) {
    const HWND fsWin = FullscreenWindow(islandHwnd, onlyMonitor, anyDisplay);
    if (fsWindow) *fsWindow = fsWin;
    if (!fsWin) return 0;
    const std::wstring app = ForegroundAppName(fsWin);
    if (bb::applist::Matches(s.fullscreenShowApps, app)) return 0;
    if (bb::applist::Matches(s.fullscreenHideApps, app)) return 2;
    if (bb::applist::Matches(s.fullscreenTuckApps, app)) return 1;
    return !s.autoHideFullscreen ? 0 : s.fullscreenTuck ? 1 : 2;
}

bool IsForegroundFullscreen(HWND targetHwnd) {
    return IsFullscreenWindow(GetForegroundWindow(), targetHwnd);
}

bool IsFullscreenWindow(HWND fg, HWND targetHwnd) {
    if (!fg || fg == targetHwnd || fg == GetDesktopWindow() || fg == GetShellWindow()) {
        return false;
    }

    if (!IsWindowVisible(fg)) return false;

    wchar_t className[256] = {};
    GetClassNameW(fg, className, 256);
    if (wcscmp(className, L"WorkerW") == 0 || wcscmp(className, L"Progman") == 0 ||
        wcscmp(className, L"Shell_TrayWnd") == 0) {
        return false;
    }

    RECT clientRect = {};
    if (!GetClientRect(fg, &clientRect)) return false;
    POINT pt = {0, 0};
    ClientToScreen(fg, &pt);
    clientRect.left += pt.x;
    clientRect.right += pt.x;
    clientRect.top += pt.y;
    clientRect.bottom += pt.y;

    HMONITOR hMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMon, &mi)) {
        if (clientRect.left <= mi.rcMonitor.left &&
            clientRect.top <= mi.rcMonitor.top &&
            clientRect.right >= mi.rcMonitor.right &&
            clientRect.bottom >= mi.rcMonitor.bottom) {
            return true;
        }
    }

    return false;
}

struct MonitorEnumData {
    std::vector<HMONITOR> monitors;
};

inline BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) {
    auto* data = reinterpret_cast<MonitorEnumData*>(dwData);
    data->monitors.push_back(hMonitor);
    return TRUE;
}

inline HMONITOR GetAnchorMonitorFor(int targetMonitor) {
    HMONITOR selectedMonitor = nullptr;

    if (targetMonitor == -1) {
        POINT pt = {0, 0};
        GetCursorPos(&pt);
        selectedMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    } else if (targetMonitor > 0) {
        MonitorEnumData data;
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));

        int index = targetMonitor - 1;
        if (index >= 0 && index < static_cast<int>(data.monitors.size())) {
            selectedMonitor = data.monitors[index];
        }
    }

    if (!selectedMonitor) {
        POINT pt = {0, 0};
        selectedMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    return selectedMonitor;
}

// Bunbun: Separate displays. The display the working island is on. It moves only
// when the pointer reaches another display's island while this one is at rest (the
// render loop sets it), not whenever the pointer crosses screens: that moved the
// island, rebuilt the others and re-sized it on every pass between displays.
std::atomic<HMONITOR> g_separateAnchor{nullptr};

// Bunbun: IslandTestState holds media / privacy state until these times (NowSeconds()).
std::atomic<double> g_testMediaUntil{0.0};
std::atomic<double> g_testPrivacyUntil{0.0};

// Bunbun: Separate displays and full screen. Each display's island is its own, so a
// full-screen app folds only the island on its display: the working island moves to
// another display, and that display's own island tucks (1) or hides (2), by the same
// Show / Tuck / Hide rules. Render thread writes; Mirrors reads on the same thread.
std::atomic<HMONITOR> g_fsOtherMonitor{nullptr};
std::atomic<int> g_fsOtherAction{0};

inline HMONITOR GetAnchorMonitor() {
    if (g_settings.displaysAll && g_settings.displaysSeparate) {
        HMONITOR m = g_separateAnchor.load(std::memory_order_relaxed);
        MONITORINFO mi{sizeof mi};
        if (!m || !GetMonitorInfoW(m, &mi)) {  // first use, or that display went away
            m = GetAnchorMonitorFor(-1);
            g_separateAnchor.store(m, std::memory_order_relaxed);
        }
        return m;
    }
    return GetAnchorMonitorFor(g_settings.targetMonitor);
}

// Returns the DPI scale factor for a given monitor (1.0 = 96 DPI = 100%)
inline float GetMonitorDpiScale(HMONITOR monitor) {
    if (!monitor) {
        POINT pt = {0, 0};
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto pGetDpiForMonitor = reinterpret_cast<GetDpiForMonitor_t>(
        GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (pGetDpiForMonitor && monitor) {
        pGetDpiForMonitor(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
    }
    return static_cast<float>(dpiX) / 96.0f;
}

// Bunbun: the island's scale for a monitor, before Size scale. Auto DPI scaling follows
// the monitor's DPI. Match screen size gives the same share of every screen (1080
// pixels high = 1x; the process is per-monitor DPI aware, so these are real pixels);
// with Auto DPI on too, the larger of the two, so a 1080p screen at 125% isn't shrunk.
inline float MonitorScale(HMONITOR monitor, bool autoDpi, bool matchScreen) {
    if (!monitor) monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const float dpi = autoDpi ? GetMonitorDpiScale(monitor) : 1.0f;
    if (matchScreen) {
        MONITORINFO mi{sizeof(mi)};
        if (monitor && GetMonitorInfoW(monitor, &mi)) {
            const float share = std::clamp(static_cast<float>(mi.rcMonitor.bottom - mi.rcMonitor.top) / 1080.0f, 0.75f, 3.0f);
            return autoDpi ? std::max(dpi, share) : share;
        }
    }
    return dpi;
}


int GetMonitorRefreshRate(HWND hwnd) {
    HMONITOR monitor = nullptr;
    if (hwnd) {
        monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    } else {
        POINT pt = {0, 0};
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            if (dm.dmDisplayFrequency > 1) {
                return static_cast<int>(dm.dmDisplayFrequency);
            }
        }
    }
    HDC hdc = GetDC(nullptr);
    int rate = GetDeviceCaps(hdc, VREFRESH);
    ReleaseDC(nullptr, hdc);
    return (rate > 1) ? rate : 60;
}

D2D1_COLOR_F GetSystemAccentColor() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    using DwmGetColorizationColor_t = HRESULT(WINAPI*)(DWORD*, BOOL*);
    auto proc = reinterpret_cast<DwmGetColorizationColor_t>(
        GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmGetColorizationColor"));

    if (proc && SUCCEEDED(proc(&color, &opaque))) {
        return D2D1::ColorF(
            ((color >> 16) & 0xff) / 255.0f,
            ((color >> 8) & 0xff) / 255.0f,
            (color & 0xff) / 255.0f,
            1.0f);
    }

    return D2D1::ColorF(0x4cc9f0);
}

void ApplyHideShowHotkey();                 // forward declaration; defined after LoadSettings()
void ApplyBackdropMaterial(HWND);           // forward declaration; defined after LoadSettings()
void NotifyKeyboardThreadSettingChanged();  // forward declaration; defined with the keyboard hook

void ApplyLookupHotkey();  // forward declaration; defined with Quick Lookup

// Bunbun: "top-center" ... "bottom-right" -> Position.
bool ParsePositionName(const std::wstring& name, Position* out) {
    static const std::pair<const wchar_t*, Position> kNames[] = {
        {L"top-center", Position::TopCenter},       {L"top-left", Position::TopLeft},
        {L"top-right", Position::TopRight},         {L"bottom-center", Position::BottomCenter},
        {L"bottom-left", Position::BottomLeft},     {L"bottom-right", Position::BottomRight},
    };
    for (const auto& [n, p] : kNames)
        if (EqualsNoCase(name, n)) { *out = p; return true; }
    return false;
}

