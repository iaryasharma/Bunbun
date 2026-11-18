// Bunbun: Per-display values and the mirror islands on other displays.
// Split out of island_core.cpp; included inside its anonymous namespace at the
// original position, so declaration order is unchanged. Not a standalone file.

// ── Bunbun: per-display values ("2=pill;3=notch") ───────────────────────────
// The entry for 1-based display `index` in a "n=value;n=value" list, "" when none.
std::wstring PerDisplayValue(const std::wstring& list, int index) {
    size_t start = 0;
    while (start < list.size()) {
        size_t end = list.find_first_of(L";,", start);
        if (end == std::wstring::npos) end = list.size();
        const std::wstring item = list.substr(start, end - start);
        const size_t eq = item.find(L'=');
        if (eq != std::wstring::npos && _wtoi(item.substr(0, eq).c_str()) == index) {
            std::wstring v = item.substr(eq + 1);
            v.erase(0, v.find_first_not_of(L" \t"));
            v.erase(v.find_last_not_of(L" \t") + 1);
            return v;
        }
        start = end + 1;
    }
    return {};
}
// Display `index`'s shape: 0 pill, 1 Windows 11, 2 notch; -1 = the island's own.
int DisplayShape(const Settings& s, int index) {
    const std::wstring v = PerDisplayValue(s.displayShapes, index);
    if (EqualsNoCase(v, L"notch")) return 2;
    if (EqualsNoCase(v, L"w11")) return 1;
    if (EqualsNoCase(v, L"pill") || EqualsNoCase(v, L"default")) return 0;
    return -1;
}
// Display `index`'s size, relative to Size scale (1 = the same).
float DisplaySize(const Settings& s, int index) {
    const float v = static_cast<float>(_wtof(PerDisplayValue(s.displaySizes, index).c_str()));
    return v >= 0.5f && v <= 2.5f ? v : 1.0f;
}
// 1-based number of a display (0 when it isn't connected).
int DisplayIndexOf(HMONITOR monitor) {
    MonitorEnumData mons;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&mons));
    for (size_t i = 0; i < mons.monitors.size(); ++i)
        if (mons.monitors[i] == monitor) return static_cast<int>(i) + 1;
    return 0;
}

// ── Bunbun: mirror islands on other displays ────────────────────────────────
// With Appearance.Displays = all, every display other than the island's own gets
// a mirror: a click-through layered window that shows the same pixels. Each
// present copies just the pill (plus room for its shadow) out of the main
// stage's DIB with UpdateLayeredWindowIndirect's source offset, so mirrors cost
// one extra composite each, never a second render. A mirror can sit at a
// different anchor (top on one screen, bottom on another). Render thread only.
// Bunbun: Appearance.DisplayLink. Linked: a mirror works like the island itself:
// the pointer on it opens the island (and so every mirror), and its mouse input
// is handed to the island at the matching point. Separate: the working island
// follows the pointer's display (GetAnchorMonitor); every other display gets its
// own island at rest, drawn live by a second renderer in that display's shape and
// size (RenderRestIsland), so only the one you use opens. The kept copy below is
// only a fallback for when that renderer isn't there.
// Bunbun: draws the island at rest for another display (shape 0 pill, 1 Windows 11,
// 2 notch; `scale` its size scale). Defined after the Renderer; render thread only.
bool RenderRestIsland(int shape, float scale, POINT origin, HDC* dc, SIZE* size, RECT* pill, bool tucked = false);
namespace Mirrors {

struct Mirror {
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;
    int index = 0;                // 1-based display number
    Position position = Position::TopCenter;
    bool visible = false;
    RECT shown{};                 // where its pixels are on screen (pill plus margin)
    POINT toMain{};               // add to a point on this mirror to get the same point on the island
    bool tracking = false;        // TrackMouseEvent is armed for its WM_MOUSELEAVE
    int shape = -1;               // Separate: this display's shape (0 pill, 1 Windows 11, 2 notch; -1 the island's)
    float size = 1.0f;            // Separate: this display's size, relative to Size scale
    POINT origin{};               // where its bitmap was last put on screen
    HDC dc = nullptr;             // its own bitmap, when it's drawn in its own shape or size
    HBITMAP bmp = nullptr, old = nullptr;
    SIZE dcSize{};
};
std::vector<Mirror> g_list;
double g_lastSync = -100.0;
bool g_classRegistered = false;
HMONITOR g_lastAnchor = nullptr;
std::atomic<bool> g_forwarding{false};  // the island is handling a mirror's mouse message
std::atomic<bool> g_pointerOn{false};   // the pointer is on a mirror's pill (Linked)
std::mutex g_placeMutex;                // guards the `shown` / `toMain` copy below, for the pipe thread
struct Place { HMONITOR monitor; int index; RECT shown; POINT toMain; };
std::vector<Place> g_places;

// The island at rest, for Separate mode: the last pixels shown while it wasn't open.
HDC g_restDc = nullptr;
HBITMAP g_restBmp = nullptr, g_restOld = nullptr;
SIZE g_restSize{};
RECT g_restPill{};  // the pill within that copy
SIZE g_lastPillSize{};   // to tell when the island has settled
double g_pillSizeSince = 0;

// A mirror's mouse messages, handed to the island as if it had been pointed at
// directly (Linked mode).
LRESULT CALLBACK MirrorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Mirror* m = nullptr;
    for (auto& x : g_list)
        if (x.hwnd == hwnd) m = &x;
    if (!m || !g_hwnd) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONUP: {
            if (msg == WM_MOUSEMOVE && !m->tracking) {
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
                m->tracking = TrackMouseEvent(&tme) != FALSE;
            }
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &pt);
            pt.x += m->toMain.x;
            pt.y += m->toMain.y;
            ScreenToClient(g_hwnd, &pt);
            g_forwarding = true;
            const LRESULT r = SendMessageW(g_hwnd, msg, wParam, MAKELPARAM(pt.x, pt.y));
            g_forwarding = false;
            return r;
        }
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {  // screen coordinates
            const POINT pt{GET_X_LPARAM(lParam) + m->toMain.x, GET_Y_LPARAM(lParam) + m->toMain.y};
            g_forwarding = true;
            const LRESULT r = SendMessageW(g_hwnd, msg, wParam, MAKELPARAM(pt.x, pt.y));
            g_forwarding = false;
            return r;
        }
        case WM_MOUSELEAVE:
            m->tracking = false;
            SendMessageW(g_hwnd, WM_MOUSELEAVE, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Linked: a pointer on a display with a copy of the island, moved to the matching spot
// on the island itself (for the eyes; the copy shows the island's pixels).
void PointerToIsland(POINT* p) {
    if (!g_settings.displaysAll || g_settings.displaysSeparate) return;
    const HMONITOR there = MonitorFromPoint(*p, MONITOR_DEFAULTTONULL);
    for (const auto& m : g_list)
        if (m.visible && m.monitor == there) {
            p->x += m.toMain.x;
            p->y += m.toMain.y;
            return;
        }
}

// Separate: the display whose island the pointer is on (with a little slack), else null.
HMONITOR RestIslandUnder(POINT cursor) {
    const HMONITOR fs = g_fsOtherMonitor.load(std::memory_order_relaxed);
    for (const auto& m : g_list) {
        if (m.monitor == fs) continue;  // Bunbun: that display is in full screen; its island stays folded
        RECT r = m.shown;
        InflateRect(&r, 10, 10);
        if (m.visible && PtInRect(&r, cursor)) return m.monitor;
    }
    return nullptr;
}

// Bunbun: a display with its own island other than `avoid` (Separate: where the
// working island goes when a full-screen app covers its display).
HMONITOR OtherDisplay(HMONITOR avoid) {
    for (const auto& m : g_list)
        if (m.monitor != avoid) return m.monitor;
    return nullptr;
}

// Is the pointer on a mirror's island (Linked mode)? Its rect, in screen pixels.
bool PointerOn(POINT cursor, const Settings& s) {
    if (!s.displaysAll || s.displaysSeparate) return false;
    for (const auto& m : g_list)
        if (m.visible && PtInRect(&m.shown, cursor)) return true;
    return false;
}

// "2=bottom-center;3=top-left" -> position for 1-based display `index`.
Position PositionFor(int index, const Settings& s) {
    Position pos = s.mirrorPositionSame ? s.position : s.mirrorPosition;
    const std::wstring& list = s.displayPositions;
    size_t start = 0;
    while (start < list.size()) {
        size_t end = list.find_first_of(L";,", start);
        if (end == std::wstring::npos) end = list.size();
        const std::wstring item = list.substr(start, end - start);
        const size_t eq = item.find(L'=');
        if (eq != std::wstring::npos && _wtoi(item.substr(0, eq).c_str()) == index) {
            Position p;
            if (ParsePositionName(item.substr(eq + 1), &p)) pos = p;
        }
        start = end + 1;
    }
    return pos;
}

void FreeBitmap(Mirror& m) {
    if (!m.dc) return;
    SelectObject(m.dc, m.old);
    DeleteObject(m.bmp);
    DeleteDC(m.dc);
    m.dc = nullptr;
    m.bmp = m.old = nullptr;
    m.dcSize = SIZE{};
}

void DestroyAll() {
    for (auto& m : g_list) {
        if (m.hwnd) DestroyWindow(m.hwnd);
        FreeBitmap(m);
    }
    g_list.clear();
}

// Matches the mirror list to the connected displays (cheap; run every ~2 s).
void Sync(const Settings& s, double now) {
    // Every ~2 s, and at once when the island moves to another display (Separate).
    const HMONITOR anchor = GetAnchorMonitor();
    if (now - g_lastSync < 2.0 && anchor == g_lastAnchor) return;
    g_lastAnchor = anchor;
    g_lastSync = now;
    if (!s.displaysAll) {
        DestroyAll();
        return;
    }
    if (!g_classRegistered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = MirrorWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"BunbunMirror";
        g_classRegistered = RegisterClassW(&wc) != 0;
    }
    MonitorEnumData data;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));
    const HMONITOR own = GetAnchorMonitor();

    std::vector<Mirror> next;
    for (size_t i = 0; i < data.monitors.size(); ++i) {
        if (data.monitors[i] == own) continue;
        Mirror m;
        for (auto& old : g_list)
            if (old.monitor == data.monitors[i]) { m = old; old.hwnd = nullptr; old.dc = nullptr; }
        m.monitor = data.monitors[i];
        m.index = static_cast<int>(i) + 1;
        // Separate mode: the configured main display keeps Appearance.Position.
        const int mainIndex = s.targetMonitor > 0 ? s.targetMonitor : 0;
        const bool isMain = s.targetMonitor == 0 ? MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY) == m.monitor
                                                 : mainIndex == m.index;
        m.position = s.displaysSeparate && isMain ? s.basePosition : PositionFor(m.index, s);
        // Separate: each display's own shape and size (Linked mirrors are exact copies).
        // Its own entry when there is one (the main display's too); else the island's look.
        m.shape = s.displaysSeparate ? DisplayShape(s, m.index) : -1;
        if (s.displaysSeparate && m.shape < 0) m.shape = s.baseNotch ? 2 : s.baseW11 ? 1 : 0;  // no entry: the island's set shape
        m.size = s.displaysSeparate ? DisplaySize(s, m.index) : 1.0f;
        if (IsBottomPosition(m.position) && m.shape == 2) m.shape = 0;  // a notch only hangs from the top
        if (!m.hwnd) {
            // Not click-through: a Linked mirror takes the pointer (fully clear pixels still pass it on).
            m.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                     L"BunbunMirror", L"Bunbun", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
            m.visible = false;
        }
        next.push_back(m);
    }
    DestroyAll();  // windows of displays that went away (moved ones were nulled above)
    g_list = std::move(next);
}

// The island's outline in a given shape: pill (0), Windows 11 (1) or notch (2, with
// its ears when `ears`), for a mirror drawn in its display's own look.
ComPtr<ID2D1Geometry> ShapeGeometry(ID2D1Factory* f, D2D1_RECT_F r, int shape, float scale, bool ears) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    if (shape != 2) {
        const float radius = shape == 1 ? 8.0f * scale : std::min(h * 0.5f, 44.0f * scale);
        ComPtr<ID2D1RoundedRectangleGeometry> g;
        f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &g);
        return g;
    }
    const float hu = h / scale;
    const float rb = (hu <= 44.0f ? hu * 0.5f : std::min(30.0f, 22.0f + (hu - 44.0f) * 0.1f)) * scale;  // NotchBottomRadius
    const float e = ears ? std::max(0.0f, std::min({16.0f * scale, h * 0.42f, w / 8.0f})) : 0.0f;
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> k;
    if (FAILED(f->CreatePathGeometry(&g)) || FAILED(g->Open(&k))) return nullptr;
    k->BeginFigure(D2D1::Point2F(r.left - e, r.top), D2D1_FIGURE_BEGIN_FILLED);
    k->AddLine(D2D1::Point2F(r.right + e, r.top));
    if (e > 0) k->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.right, r.top + e), D2D1::SizeF(e, e), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    k->AddLine(D2D1::Point2F(r.right, r.bottom - rb));
    k->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.right - rb, r.bottom), D2D1::SizeF(rb, rb), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    k->AddLine(D2D1::Point2F(r.left + rb, r.bottom));
    k->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.left, r.bottom - rb), D2D1::SizeF(rb, rb), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    k->AddLine(D2D1::Point2F(r.left, r.top + e));
    if (e > 0) k->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.left - e, r.top), D2D1::SizeF(e, e), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    k->EndFigure(D2D1_FIGURE_END_CLOSED);
    k->Close();
    return g;
}

// Draws the island at rest into mirror `m`'s own bitmap, in that display's shape and
// size: its surface in the island's own colour, then the island's content scaled and
// clipped into the new outline. `pill` is the island in `src`. Returns the bitmap's
// size and where the pill sits in it.
bool DrawOwnLook(Mirror& m, HDC src, RECT pill, int shape, float k, float scale, SIZE* outSize, POINT* pillAt) {
    static ComPtr<ID2D1Factory> factory;
    static ComPtr<ID2D1DCRenderTarget> rt;
    if (!factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()))) return false;
    if (!rt) {
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(factory->CreateDCRenderTarget(&props, &rt))) return false;
    }
    const int pw = pill.right - pill.left, ph = pill.bottom - pill.top;
    if (pw <= 2 || ph <= 2) return false;
    // The island's pixels, top-down, for a Direct2D bitmap.
    static HDC tmpDc = nullptr;
    static HBITMAP tmpBmp = nullptr;
    static void* tmpBits = nullptr;
    static SIZE tmpSize{};
    if (!tmpDc || tmpSize.cx != pw || tmpSize.cy != ph) {
        if (tmpDc) { DeleteObject(tmpBmp); DeleteDC(tmpDc); }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), pw, -ph, 1, 32, BI_RGB};
        tmpDc = CreateCompatibleDC(src);
        tmpBmp = CreateDIBSection(src, &bi, DIB_RGB_COLORS, &tmpBits, nullptr, 0);
        SelectObject(tmpDc, tmpBmp);
        tmpSize = SIZE{pw, ph};
    }
    BitBlt(tmpDc, 0, 0, pw, ph, src, pill.left, pill.top, SRCCOPY);
    GdiFlush();
    // The surface colour: a pixel just inside the island's left edge, half way down.
    const BYTE* px = static_cast<const BYTE*>(tmpBits) + (ph / 2) * pw * 4 + std::min(4, pw - 1) * 4;
    const float a = px[3] / 255.0f;
    const D2D1_COLOR_F fill = a > 0.01f ? D2D1::ColorF(px[2] / 255.0f / a, px[1] / 255.0f / a, px[0] / 255.0f / a, a)
                                        : D2D1::ColorF(0, 0, 0, 1);

    const float W = pw * k, H = ph * k;
    const int margin = static_cast<int>(std::ceil(20.0f * scale));
    const SIZE size{static_cast<LONG>(std::ceil(W)) + 2 * margin, static_cast<LONG>(std::ceil(H)) + 2 * margin};
    if (!m.dc || m.dcSize.cx != size.cx || m.dcSize.cy != size.cy) {
        FreeBitmap(m);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB};
        void* bits = nullptr;
        m.dc = CreateCompatibleDC(src);
        m.bmp = CreateDIBSection(src, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        m.old = static_cast<HBITMAP>(SelectObject(m.dc, m.bmp));
        m.dcSize = size;
    }
    const RECT bind{0, 0, size.cx, size.cy};
    if (FAILED(rt->BindDC(m.dc, &bind))) return false;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    const D2D1_RECT_F body = D2D1::RectF(static_cast<float>(margin), static_cast<float>(margin), margin + W, margin + H);
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(fill, &brush);
    if (ComPtr<ID2D1Geometry> outline = ShapeGeometry(factory.Get(), body, shape, scale, true); outline && brush)
        rt->FillGeometry(outline.Get(), brush.Get());
    ComPtr<ID2D1Bitmap> content;
    rt->CreateBitmap(D2D1::SizeU(pw, ph), tmpBits, pw * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &content);
    if (content) {
        ComPtr<ID2D1Geometry> clip = ShapeGeometry(factory.Get(), body, shape, scale, false);
        ComPtr<ID2D1Layer> layer;
        rt->CreateLayer(&layer);
        if (clip && layer) rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        rt->DrawBitmap(content.Get(), body, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (clip && layer) rt->PopLayer();
    }
    if (FAILED(rt->EndDraw())) return false;
    *outSize = size;
    *pillAt = POINT{margin, margin};
    return true;
}

// Keeps a copy of the island at rest (Separate mode shows it on the other displays
// while the working island is open).
void KeepRest(HDC src, RECT srcRect, RECT pill) {
    const SIZE size{srcRect.right - srcRect.left, srcRect.bottom - srcRect.top};
    if (size.cx <= 0 || size.cy <= 0) return;
    if (!g_restDc || size.cx != g_restSize.cx || size.cy != g_restSize.cy) {
        if (g_restDc) {
            SelectObject(g_restDc, g_restOld);
            DeleteObject(g_restBmp);
            DeleteDC(g_restDc);
        }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB};
        void* bits = nullptr;
        g_restDc = CreateCompatibleDC(src);
        g_restBmp = CreateDIBSection(src, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        g_restOld = static_cast<HBITMAP>(SelectObject(g_restDc, g_restBmp));
        g_restSize = size;
    }
    BitBlt(g_restDc, 0, 0, size.cx, size.cy, src, srcRect.left, srcRect.top, SRCCOPY);
    g_restPill = RECT{pill.left - srcRect.left, pill.top - srcRect.top, pill.right - srcRect.left, pill.bottom - srcRect.top};
}

// Where display `index` (1-based) shows the island, for the settings app's preview of
// that display: false when it's the island's own display (or has no mirror).
bool PlaceOf(int index, RECT* shown, HMONITOR* monitor) {
    std::lock_guard lock(g_placeMutex);
    for (const auto& p : g_places)
        if (p.index == index) {
            *shown = p.shown;
            *monitor = p.monitor;
            return true;
        }
    return false;
}

// `pill` is the pill's rect in the stage's client px; `stage` the DIB's size.
void Present(HDC src, const Settings& s, RECT pill, SIZE stage, BYTE alpha, double now, bool open) {
    Sync(s, now);
    const bool show = pill.right - pill.left > 2 && pill.bottom - pill.top > 2;
    const int margin = static_cast<int>(std::ceil(26.0f * s.sizeScale));
    RECT srcRect = {std::max(0L, pill.left - margin), std::max(0L, pill.top - margin),
                    std::min(stage.cx, pill.right + margin), std::min(stage.cy, pill.bottom + margin)};
    SIZE size = {srcRect.right - srcRect.left, srcRect.bottom - srcRect.top};
    const bool flushTop = s.notchStyle || s.borderMergedMode;
    // Separate: the others show the island only while it is settled at rest. While it
    // opens, closes or reacts to the pointer they keep the last settled copy: frames
    // from the start of an opening have their content faded out (a plain black shape).
    const bool separate = s.displaysAll && s.displaysSeparate;
    const SIZE pillSize{pill.right - pill.left, pill.bottom - pill.top};
    if (std::abs(pillSize.cx - g_lastPillSize.cx) > 1 || std::abs(pillSize.cy - g_lastPillSize.cy) > 1) {
        g_lastPillSize = pillSize;
        g_pillSizeSince = now;
    }
    const bool settled = !open && show && now - g_pillSizeSince > 0.35;
    if (separate && settled) KeepRest(src, srcRect, pill);
    if (separate && !settled && g_restDc) {
        open = true;  // not at rest: the others show the kept copy
        src = g_restDc;
        srcRect = RECT{0, 0, g_restSize.cx, g_restSize.cy};
        pill = g_restPill;
        size = g_restSize;
    }
    POINT mainOrigin{0, 0};
    if (g_hwnd) ClientToScreen(g_hwnd, &mainOrigin);
    std::vector<Place> places;

    for (auto& m : g_list) {
        if (!show || size.cx <= 0 || size.cy <= 0) {
            if (m.visible) { ShowWindow(m.hwnd, SW_HIDE); m.visible = false; }
            continue;
        }
        // Bunbun: a full-screen app on this display: its island hides, or folds to a line.
        const int fsHere = separate && m.monitor == g_fsOtherMonitor.load(std::memory_order_relaxed)
                               ? g_fsOtherAction.load(std::memory_order_relaxed) : 0;
        if (fsHere == 2) {
            if (m.visible) { ShowWindow(m.hwnd, SW_HIDE); m.visible = false; }
            continue;
        }
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(m.monitor, &mi);
        const RECT& work = mi.rcWork;
        // Separate: this display's own shape and size, when they differ from the island's.
        const int mainShape = s.notchStyle ? 2 : s.w11Style ? 1 : 0;
        // This display's size against the working island's, which already has its own
        // display's scale and size in it.
        const float mirrorScale = s.baseSizeScale * m.size * MonitorScale(m.monitor, s.autoDpiScale, s.matchScreenSize);
        const float k = mirrorScale / std::max(0.01f, s.sizeScale);
        const bool ownLook = separate && ((m.shape >= 0 && m.shape != mainShape) || std::fabs(k - 1.0f) > 0.01f);
        HDC from = src;
        RECT fromRect = srcRect;
        RECT fromPill = pill;
        SIZE fromSize = size;
        // Separate: its own island, drawn live at rest in this display's look.
        HDC liveDc = nullptr;
        SIZE liveSize{};
        RECT livePill{};
        const bool live = separate && RenderRestIsland(m.shape >= 0 ? m.shape : mainShape, mirrorScale, m.origin,
                                                       &liveDc, &liveSize, &livePill, fsHere == 1);
        if (live) {
            from = liveDc;
            fromRect = RECT{0, 0, liveSize.cx, liveSize.cy};
            fromPill = livePill;
            fromSize = liveSize;
        } else if (ownLook) {
            POINT at{};
            if (DrawOwnLook(m, src, pill, m.shape >= 0 ? m.shape : mainShape, k, mirrorScale, &fromSize, &at)) {
                from = m.dc;
                fromRect = RECT{0, 0, fromSize.cx, fromSize.cy};
                fromPill = RECT{at.x, at.y, at.x + static_cast<LONG>(std::lround((pill.right - pill.left) * k)),
                                at.y + static_cast<LONG>(std::lround((pill.bottom - pill.top) * k))};
            }
        }
        const bool flushHere = ((live || ownLook) && m.shape >= 0) ? (m.shape == 2 || s.borderMergedMode) : flushTop;
        const int pillW = fromPill.right - fromPill.left, pillH = fromPill.bottom - fromPill.top;

        // Pill position on that display, mirroring ComputeStageLayout's anchors.
        int pillX = work.left + (work.right - work.left - pillW) / 2;
        if (m.position == Position::TopLeft || m.position == Position::BottomLeft) pillX = work.left + 16;
        if (m.position == Position::TopRight || m.position == Position::BottomRight) pillX = work.right - 16 - pillW;
        const int pillY = IsBottomPosition(m.position) ? work.bottom - 40 - static_cast<int>(kRenderPadY) - pillH
                          : flushHere ? work.top
                                      : work.top + 8 + static_cast<int>(kRenderPadY);

        POINT dst = {pillX - (fromPill.left - fromRect.left), pillY - (fromPill.top - fromRect.top)};
        POINT srcPt = {fromRect.left, fromRect.top};
        m.shown = RECT{pillX, pillY, pillX + pillW, pillY + pillH};
        if (!(separate && open)) m.toMain = POINT{mainOrigin.x + pill.left - pillX, mainOrigin.y + pill.top - pillY};
        m.origin = dst;  // Bunbun: where its pixels start, for its own island's eyes next frame
        places.push_back({m.monitor, m.index, RECT{dst.x, dst.y, dst.x + fromSize.cx, dst.y + fromSize.cy}, m.toMain});
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof info;
        info.pptDst = &dst;
        info.psize = &fromSize;
        info.hdcSrc = from;
        info.pptSrc = &srcPt;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        if (UpdateLayeredWindowIndirect(m.hwnd, &info) && !m.visible) {
            SetWindowPos(m.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            m.visible = true;
        }
    }
    std::lock_guard lock(g_placeMutex);
    g_places = std::move(places);
}

// Bunbun: the island is hidden (hotkey or menu, idle, full screen): its copies on the other displays go too.
// Present() isn't called while the render thread is parked, so nothing else would hide them.
void HideAll() {
    for (auto& m : g_list)
        if (m.visible) { ShowWindow(m.hwnd, SW_HIDE); m.visible = false; }
}

}  // namespace Mirrors

// z-order / owner / backdrop-region upkeep only. Position and size are NOT touched
// here: UpdateLayeredWindow sets them together with the new pixels, so the window
// never moves or resizes in a separate step from its content.
