// Bunbun — Studio layout: shared state for the framed island and its extra pages.
//
// #included inside island_core.cpp's anonymous namespace (after the focus timer
// and media transport helpers). The drawing lives in studio_draw.inc, inside the
// Renderer class. What lives here:
//   - the dashboard's tab list, built in one place for the idle and media views;
//   - the Agent, Clipboard and Note pages' data (clipboard history, note.txt);
//   - tuck-away (the island folded to a thin line at the top of the screen);
//   - the mascot's poke reactions (cross after 3 quick pokes, dizzy after 6);
//   - the clickable spots the frame and pages publish, and what a click does.

// ── Tabs ─────────────────────────────────────────────────────────────────────

// The open Studio island: a 184-unit page between a 34-unit header and a 38-unit
// tab bar, 520 wide (content units; times sizeScale on screen). The page model is
// shared with the Linux island (src/island/studio_pages.h).
constexpr float kStudioIslandW = bb::studio::kIslandW;
constexpr float kStudioIslandH = bb::studio::kIslandH;
// Ask Bun's chat panel in the Studio layout.
constexpr float kStudioChatW = 520.0f;
constexpr float kStudioChatH = 300.0f;

// Extra pages share their ids in both views; the built-in ids differ per view.
using bb::studio::kTabAgent;
using bb::studio::kTabClipboard;
using bb::studio::kTabNote;
using bb::studio::kTabFocus;
using bb::studio::kTabApps;
using bb::studio::kTabMirror;
using bb::studio::kTabWater;
using bb::studio::kTabHome;
using bb::studio::kTabScreenTime;
using bb::studio::kTabWork;

// ── Which agents the island shows ────────────────────────────────────────────
// Agents in the order of Settings::showAgent (Bunbun.Show*).
enum AgentKind { AgentClaude, AgentAntigravity, AgentCursor, AgentCopilot, AgentCodex, AgentOther, AgentKindCount };

AgentKind AgentKindFor(const std::wstring& displayName) {
    std::wstring n = displayName;
    for (auto& c : n) c = towlower(c);
    if (n.find(L"claude") != std::wstring::npos || n.empty()) return AgentClaude;
    if (n.find(L"antigravity") != std::wstring::npos || n.find(L"gemini") != std::wstring::npos) return AgentAntigravity;
    if (n.find(L"cursor") != std::wstring::npos) return AgentCursor;
    if (n.find(L"vs code") != std::wstring::npos || n.find(L"copilot") != std::wstring::npos) return AgentCopilot;
    if (n.find(L"codex") != std::wstring::npos) return AgentCodex;
    return AgentOther;
}

int AgentKindOf(const std::wstring& displayName) { return AgentKindFor(displayName); }

const wchar_t* AgentKindName(int kind) {
    static const wchar_t* kNames[] = {L"Claude Code", L"Antigravity", L"Cursor", L"VS Code", L"Codex", L"Agents"};
    return kNames[std::clamp(kind, 0, AgentKindCount - 1)];
}

// Bit per AgentKind: connected through Bunbun's installer (refreshed every 30 s),
// and seen sending events since Bunbun started. Either makes an agent "in use".
std::atomic<unsigned> g_agentsConnected{0};
std::atomic<unsigned> g_agentsSeen{0};
std::atomic<int> g_lastAgentKind{-1};
std::atomic<double> g_agentsCheckedAt{-100.0};

void NoteAgentActivity(const bb::AgentView& view) {
    if (view.phase == bb::AgentPhase::None || view.agent.empty()) return;
    const int kind = AgentKindFor(view.agent);
    g_agentsSeen.fetch_or(1u << kind);
    g_lastAgentKind.store(kind);
}

unsigned AgentsInUse() {
    const double now = NowSeconds();
    if (now - g_agentsCheckedAt.load() > 30.0) {
        g_agentsCheckedAt.store(now);
        unsigned bits = 0;
        if (bb::hooks::ClaudeHooksInstalled()) bits |= 1u << AgentClaude;
        if (bb::hooks::AntigravityHooksInstalled()) bits |= 1u << AgentAntigravity;
        if (bb::hooks::CursorHooksInstalled()) bits |= 1u << AgentCursor;
        if (bb::hooks::CopilotHooksInstalled()) bits |= 1u << AgentCopilot;
        if (bb::hooks::CodexHooksInstalled()) bits |= 1u << AgentCodex;
        g_agentsConnected.store(bits);
    }
    return g_agentsConnected.load() | g_agentsSeen.load();
}

// True when an agent the user switched on is connected or has been active: only
// then do the Agent tile, the Agent page and the usage ring appear.
bool AgentsShown(const Settings& s) {
    if (!s.bunbunAgents) return false;
    const unsigned inUse = AgentsInUse();
    for (int k = 0; k < AgentKindCount; ++k)
        if (s.showAgent[k] && (inUse & (1u << k))) return true;
    return false;
}

// The agent the Home tile and the header ring follow: the chosen main agent, else
// the one used last, else the first one switched on and in use.
int MainAgentKind(const Settings& s) {
    if (s.mainAgent >= 0 && s.mainAgent < AgentKindCount && s.showAgent[s.mainAgent]) return s.mainAgent;
    const int last = g_lastAgentKind.load();
    if (last >= 0 && s.showAgent[last]) return last;
    const unsigned inUse = AgentsInUse();
    for (int k = 0; k < AgentKindCount; ++k)
        if (s.showAgent[k] && (inUse & (1u << k))) return k;
    return AgentClaude;
}

int StudioExtraTabCount(const Settings& s) {
    return (s.agentTab && AgentsShown(s) ? 1 : 0) + (s.clipboardHistory ? 1 : 0) + (s.quickNote ? 1 : 0) + (s.focusPage ? 1 : 0) + (s.appsPage ? 1 : 0) + (s.mirrorPage ? 1 : 0) +
           (s.screenTime ? 1 : 0);  // Bunbun: Water is a Timers mode, never a tab of its own
}

// The dashboard's tabs, in scroll order. context 0 = the media view (first tab is
// the player), 1 = the idle view (first tab is the clock, or Home in Studio).
// DrawMedia and DrawIdleDashboard both draw from this list, so the frame's tab
// bar, the scroll ring and the click handlers can't disagree about the order.
// The order itself (Bunbun.PageOrder, Home first in Studio) is bb::studio::TabIds.
bb::studio::PageSwitches PageSwitchesFor(const Settings& s) {
    bb::studio::PageSwitches p;
    p.studio = s.studioLayout;
    p.lyricsPinnedFirst = s.lyricsPinnedFirst;
    p.weather = s.weather;
    if (s.studioLayout) {
        p.system = s.hardwareMonitorModule || s.connectivityModule;
        p.toggles = false;
    } else {
        p.system = s.hardwareMonitorModule;
        p.toggles = s.connectivityModule;
    }
    p.tray = s.fileTrayModule;
    p.todo = s.todoModule;
    p.lyrics = s.lyrics && g_lyricsTabAvailable.load(std::memory_order_relaxed);
    p.agent = s.agentTab && AgentsShown(s);
    p.clipboard = s.clipboardHistory;
    p.note = s.quickNote;
    p.focus = s.focusPage;
    p.mirror = s.mirrorPage;
    p.water = false;  // Bunbun: Water lives inside the Timers page (its Water mode), not as a page
    p.apps = s.appsPage;
    p.screentime = s.screenTime;
    p.work = bb::work::WorkOn(static_cast<bb::work::UseCase>(s.useCase));  // Bunbun: Work mode's page
    p.order = s.pageOrder;
    return p;
}

std::vector<int> DashboardTabIds(int context, const Settings& s) {
    return bb::studio::TabIds(context, PageSwitchesFor(s));
}

StudioPage StudioPageFor(int context, int id) { return bb::studio::PageFor(context, id); }

// Which view the dashboard was last drawn in (render thread writes, the window
// proc reads; both run on the render thread, the atomic just keeps it honest).
std::atomic<int> g_dashboardContext{1};

int DashboardPageIndex(StudioPage page, const Settings& s) {
    return DashboardPageIndexIn(g_dashboardContext.load(std::memory_order_relaxed), page, s);
}

int DashboardPageIndexIn(int context, StudioPage page, const Settings& s) {
    const std::vector<int> ids = DashboardTabIds(context, s);
    for (size_t i = 0; i < ids.size(); ++i)
        if (StudioPageFor(context, ids[i]) == page) return static_cast<int>(i);
    return -1;
}

int DashboardTabCount(const Settings& s) {
    return static_cast<int>(DashboardTabIds(g_dashboardContext.load(std::memory_order_relaxed), s).size());
}

int DashboardTabIndexOf(int id) {
    const Settings s = GetSettingsCopy();
    const std::vector<int> ids = DashboardTabIds(g_dashboardContext.load(std::memory_order_relaxed), s);
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == id) return static_cast<int>(i);
    return -1;
}

// The page currently on screen in the dashboard, or -1.
int CurrentDashboardTabId() {
    const Settings s = GetSettingsCopy();
    const std::vector<int> ids = DashboardTabIds(g_dashboardContext.load(std::memory_order_relaxed), s);
    if (ids.empty()) return -1;
    const int i = std::min(NormalizedTabIndex(s), static_cast<int>(ids.size()) - 1);
    return ids[static_cast<size_t>(i)];
}

// ── Rich Clipboard history (Vayli style) ────────────────────────────────────

enum class ClipKind { Text, Image, Link };

struct ClipItem {
    ClipKind kind = ClipKind::Text;
    std::wstring title;
    std::wstring text;
    BitmapPixels image;
    int width = 0;
    int height = 0;
    size_t byteSize = 0;
    std::wstring appName;
    PixelsRef appIcon;
    double copiedAt = 0.0;
    bool pinned = false;
};

constexpr size_t kRichClipMax = 40;
std::deque<ClipItem> g_richClipItems;
std::deque<std::wstring> g_clipHistory;    // for legacy callers
std::atomic<bool> g_clipSelfSet{false};    // our own copy
int g_clipFilter = 0;                      // 0 = All, 1 = Text, 2 = Images, 3 = Links
int g_clipSelected = 0;                    // selected index in filtered list

void RememberClipboardText(const std::wstring& raw, const std::wstring& appName, PixelsRef appIcon) {
    if (g_clipSelfSet.exchange(false)) return;
    std::wstring text = raw;
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    if (text.empty()) return;
    if (text.size() > 4000) text.resize(4000);

    ClipItem item;
    item.text = text;
    item.byteSize = text.size() * sizeof(wchar_t);
    item.appName = appName;
    item.appIcon = appIcon;
    item.copiedAt = NowSeconds();

    if (text.rfind(L"http://", 0) == 0 || text.rfind(L"https://", 0) == 0 || text.rfind(L"www.", 0) == 0) {
        item.kind = ClipKind::Link;
        item.title = text.substr(0, std::min<size_t>(text.size(), 36));
    } else {
        item.kind = ClipKind::Text;
        size_t nl = text.find_first_of(L"\r\n");
        item.title = text.substr(0, std::min<size_t>(nl == std::wstring::npos ? text.size() : nl, 36));
    }

    std::lock_guard lock(g_stateMutex);
    g_richClipItems.erase(std::remove_if(g_richClipItems.begin(), g_richClipItems.end(),
        [&](const ClipItem& c) { return c.kind == item.kind && c.text == item.text; }), g_richClipItems.end());
    g_richClipItems.push_front(std::move(item));
    while (g_richClipItems.size() > kRichClipMax) g_richClipItems.pop_back();

    g_clipHistory.erase(std::remove(g_clipHistory.begin(), g_clipHistory.end(), text), g_clipHistory.end());
    g_clipHistory.push_front(text);
    while (g_clipHistory.size() > 8) g_clipHistory.pop_back();
    g_layoutDirty = true;
}

void RememberClipboardImage(BitmapPixels&& image, const std::wstring& appName, PixelsRef appIcon) {
    if (g_clipSelfSet.exchange(false)) return;
    if (image.bgra.empty() || !image.width || !image.height) return;

    ClipItem item;
    item.kind = ClipKind::Image;
    item.title = L"Image";
    item.width = static_cast<int>(image.width);
    item.height = static_cast<int>(image.height);
    item.byteSize = image.bgra.size();
    item.image = std::move(image);
    item.appName = appName;
    item.appIcon = appIcon;
    item.copiedAt = NowSeconds();

    std::lock_guard lock(g_stateMutex);
    g_richClipItems.push_front(std::move(item));
    while (g_richClipItems.size() > kRichClipMax) g_richClipItems.pop_back();
    g_layoutDirty = true;
}

std::vector<std::wstring> ClipHistorySnapshot() {
    std::lock_guard lock(g_stateMutex);
    return {g_clipHistory.begin(), g_clipHistory.end()};
}

std::vector<size_t> FilteredClipIndices() {
    std::lock_guard lock(g_stateMutex);
    std::vector<size_t> res;
    for (size_t i = 0; i < g_richClipItems.size(); ++i) {
        const auto& it = g_richClipItems[i];
        if (g_clipFilter == 1 && it.kind != ClipKind::Text) continue;
        if (g_clipFilter == 2 && it.kind != ClipKind::Image) continue;
        if (g_clipFilter == 3 && it.kind != ClipKind::Link) continue;
        res.push_back(i);
    }
    return res;
}

bool StudioCopyText(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            g_clipSelfSet = true;  // don't flash the clipboard pill for our own copy
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            if (!ok) { GlobalFree(mem); g_clipSelfSet = false; }
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    if (ok) {
        std::lock_guard lock(g_stateMutex);
        g_clipHistory.erase(std::remove(g_clipHistory.begin(), g_clipHistory.end(), text), g_clipHistory.end());
        g_clipHistory.push_front(text);
    }
    return ok;
}

bool StudioCopyImagePixels(HWND hwnd, const BitmapPixels& pixels) {
    if (pixels.bgra.empty() || !pixels.width || !pixels.height) return false;
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    const size_t headerSize = sizeof(BITMAPINFOHEADER);
    const size_t imageSize = static_cast<size_t>(pixels.width) * pixels.height * 4;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, headerSize + imageSize);
    bool ok = false;
    if (hMem) {
        uint8_t* p = static_cast<uint8_t*>(GlobalLock(hMem));
        if (p) {
            BITMAPINFOHEADER* bih = reinterpret_cast<BITMAPINFOHEADER*>(p);
            bih->biSize = sizeof(BITMAPINFOHEADER);
            bih->biWidth = static_cast<LONG>(pixels.width);
            bih->biHeight = static_cast<LONG>(pixels.height);
            bih->biPlanes = 1;
            bih->biBitCount = 32;
            bih->biCompression = BI_RGB;
            bih->biSizeImage = static_cast<DWORD>(imageSize);
            uint8_t* dst = p + headerSize;
            const size_t stride = pixels.width * 4;
            for (size_t y = 0; y < pixels.height; ++y) {
                const uint8_t* srcRow = pixels.bgra.data() + (pixels.height - 1 - y) * stride;
                memcpy(dst + y * stride, srcRow, stride);
            }
            GlobalUnlock(hMem);
            g_clipSelfSet = true;
            ok = SetClipboardData(CF_DIB, hMem) != nullptr;
            if (!ok) { GlobalFree(hMem); g_clipSelfSet = false; }
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
    return ok;
}

void StudioCopyRichClip(HWND hwnd, int filteredIdx) {
    const std::vector<size_t> indices = FilteredClipIndices();
    if (filteredIdx < 0 || filteredIdx >= static_cast<int>(indices.size())) return;
    std::lock_guard lock(g_stateMutex);
    const size_t realIdx = indices[static_cast<size_t>(filteredIdx)];
    if (realIdx >= g_richClipItems.size()) return;
    const ClipItem& item = g_richClipItems[realIdx];
    if (item.kind == ClipKind::Image) {
        StudioCopyImagePixels(hwnd, item.image);
    } else {
        StudioCopyText(hwnd, item.text);
    }
}

void StudioPinRichClip(int filteredIdx) {
    const std::vector<size_t> indices = FilteredClipIndices();
    if (filteredIdx < 0 || filteredIdx >= static_cast<int>(indices.size())) return;
    std::lock_guard lock(g_stateMutex);
    const size_t realIdx = indices[static_cast<size_t>(filteredIdx)];
    if (realIdx >= g_richClipItems.size()) return;
    g_richClipItems[realIdx].pinned = !g_richClipItems[realIdx].pinned;
    g_layoutDirty = true;
}

void StudioDeleteRichClip(int filteredIdx) {
    const std::vector<size_t> indices = FilteredClipIndices();
    if (filteredIdx < 0 || filteredIdx >= static_cast<int>(indices.size())) return;
    std::lock_guard lock(g_stateMutex);
    const size_t realIdx = indices[static_cast<size_t>(filteredIdx)];
    if (realIdx >= g_richClipItems.size()) return;
    g_richClipItems.erase(g_richClipItems.begin() + realIdx);
    if (g_clipSelected >= static_cast<int>(indices.size()) - 1) {
        g_clipSelected = std::max(0, static_cast<int>(indices.size()) - 2);
    }
    g_layoutDirty = true;
}

// Bunbun: the Clipboard page's Clear. Empties the history but keeps pinned items.
void StudioClearRichClips() {
    std::lock_guard lock(g_stateMutex);
    std::erase_if(g_richClipItems, [](const ClipItem& c) { return !c.pinned; });
    g_clipSelected = 0;
    g_layoutDirty = true;
}

void StudioSaveRichClip(int filteredIdx) {
    const std::vector<size_t> indices = FilteredClipIndices();
    if (filteredIdx < 0 || filteredIdx >= static_cast<int>(indices.size())) return;
    std::lock_guard lock(g_stateMutex);
    const size_t realIdx = indices[static_cast<size_t>(filteredIdx)];
    if (realIdx >= g_richClipItems.size()) return;
    const ClipItem& item = g_richClipItems[realIdx];

    wchar_t picPath[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", picPath, MAX_PATH);
    std::wstring out = std::wstring(picPath) + L"\\Pictures\\Bunbun_clip_" + std::to_wstring(static_cast<uint64_t>(item.copiedAt));
    if (item.kind == ClipKind::Image) {
        out += L".bmp";
        FILE* f = _wfopen(out.c_str(), L"wb");
        if (f) {
            BITMAPFILEHEADER bfh = {};
            bfh.bfType = 0x4D42;
            bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            bfh.bfSize = static_cast<DWORD>(bfh.bfOffBits + item.image.bgra.size());
            BITMAPINFOHEADER bih = {};
            bih.biSize = sizeof(BITMAPINFOHEADER);
            bih.biWidth = static_cast<LONG>(item.image.width);
            bih.biHeight = static_cast<LONG>(item.image.height);
            bih.biPlanes = 1;
            bih.biBitCount = 32;
            bih.biCompression = BI_RGB;
            fwrite(&bfh, 1, sizeof(bfh), f);
            fwrite(&bih, 1, sizeof(bih), f);
            const size_t stride = item.image.width * 4;
            for (size_t y = 0; y < item.image.height; ++y) {
                const uint8_t* srcRow = item.image.bgra.data() + (item.image.height - 1 - y) * stride;
                fwrite(srcRow, 1, stride, f);
            }
            fclose(f);
        }
    } else {
        out += L".txt";
        FILE* f = _wfopen(out.c_str(), L"w, ccs=UTF-8");
        if (f) {
            fputws(item.text.c_str(), f);
            fclose(f);
        }
    }
}

// ── Quick note ───────────────────────────────────────────────────────────────

namespace QuickNote {

std::wstring FilePath() {
    std::wstring dir = bb::SettingsPath();
    dir.erase(dir.find_last_of(L"\\/"));
    return dir + L"\\note.txt";
}

struct Cache {
    std::vector<std::wstring> lines;
    FILETIME written{};
    double checkedAt = -10.0;
    bool exists = false;
};
Cache g_note;  // render thread only

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Lines of note.txt, re-read when the file changes (checked at most once a second).
const std::vector<std::wstring>& Lines(double now) {
    if (now - g_note.checkedAt < 1.0) return g_note.lines;
    g_note.checkedAt = now;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(FilePath().c_str(), GetFileExInfoStandard, &attr)) {
        g_note.exists = false;
        g_note.lines.clear();
        return g_note.lines;
    }
    if (g_note.exists && CompareFileTime(&attr.ftLastWriteTime, &g_note.written) == 0) return g_note.lines;
    g_note.exists = true;
    g_note.written = attr.ftLastWriteTime;
    g_note.lines.clear();
    HANDLE f = CreateFileW(FilePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return g_note.lines;
    std::string data(std::min<DWORD>(attr.nFileSizeLow, 64 * 1024), '\0');
    DWORD n = 0;
    ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &n, nullptr);
    CloseHandle(f);
    data.resize(n);
    if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
    std::wstring text = Widen(data);
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        g_note.lines.push_back(std::move(line));
        start = end + 1;
    }
    while (!g_note.lines.empty() && g_note.lines.back().empty()) g_note.lines.pop_back();
    return g_note.lines;
}

bool IsCheckbox(const std::wstring& line, bool* checked) {
    size_t i = 0;
    while (i < line.size() && line[i] == L' ') ++i;
    if (line.size() < i + 3 || line[i] != L'[' || line[i + 2] != L']') return false;
    *checked = line[i + 1] == L'x' || line[i + 1] == L'X';
    return line[i + 1] == L' ' || *checked;
}

void Save(const std::vector<std::wstring>& lines) {
    std::wstring text;
    for (const auto& l : lines) text += l + L"\r\n";
    const std::string bytes = Narrow(text);
    const std::wstring tmp = FilePath() + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    const bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) && n == bytes.size();
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), FilePath().c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
    g_note.checkedAt = -10.0;  // re-read on the next frame
}

void Toggle(int lineIndex) {
    std::vector<std::wstring> lines = Lines(NowSeconds());
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return;
    std::wstring& line = lines[static_cast<size_t>(lineIndex)];
    bool checked = false;
    if (!IsCheckbox(line, &checked)) return;
    const size_t at = line.find(L'[') + 1;
    line[at] = checked ? L' ' : L'x';
    Save(lines);
}

void OpenInEditor() {
    const std::wstring path = FilePath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Save({L"Quick note", L"", L"[ ] Something to do", L"- A thought"});
    }
    ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

}  // namespace QuickNote

// ── Antigravity quota ───────────────────────────────────────────────────────
// Antigravity caches the user's status from Google in its VS Code-style state
// database (%APPDATA%\Antigravity IDE\User\globalStorage\state.vscdb, key
// antigravityUnifiedStateSync.userStatus): base64 of a small wrapper holding
// base64 of a protobuf. Each model config sits at field 33 -> 1, with its name as
// a string and its quota at field 15: 1 = fraction remaining (float), 2 = reset
// time (a Timestamp, 1 = Unix seconds). Read once a minute on a worker thread,
// through Windows' own winsqlite3.dll, read-only. No network, no credentials.
namespace AgQuota {

struct Snapshot {
    bool valid = false;
    float remaining = 1.0f;     // lowest fraction left across models
    std::wstring model;         // the model with that lowest fraction
    long long resetAt = 0;      // its reset time, Unix seconds
    double readAt = 0.0;        // NowSeconds() of the last successful read
    std::vector<bb::agquota::Pool> pools;  // Bunbun: every pool of models, the most used first
};
Snapshot g_quota;               // guarded by g_stateMutex
std::wstring g_cursorPlan;      // Bunbun: Cursor's plan ("free", "pro"), from its own state; guarded by g_stateMutex
std::atomic<bool> g_started{false};

// The decoding itself lives in common/ag_quota.h, shared with Linux.

// Bunbun: one value from a VS Code-style state database (state.vscdb), read-only.
// "" when the file, the key or winsqlite3 isn't there.
std::string ReadStateValue(const std::wstring& path, const char* key) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    // winsqlite3.dll ships with Windows 10 and 11; loaded on demand so the island
    // has no SQLite dependency of its own.
    static HMODULE dll = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return {};
    using OpenFn = int(__stdcall*)(const char*, void**, int, const char*);
    using PrepareFn = int(__stdcall*)(void*, const char*, int, void**, const char**);
    using BindFn = int(__stdcall*)(void*, int, const char*, int, void*);
    using StepFn = int(__stdcall*)(void*);
    using TextFn = const unsigned char*(__stdcall*)(void*, int);
    using BytesFn = int(__stdcall*)(void*, int);
    using FinalizeFn = int(__stdcall*)(void*);
    using CloseFn = int(__stdcall*)(void*);
    auto open = reinterpret_cast<OpenFn>(GetProcAddress(dll, "sqlite3_open_v2"));
    auto prepare = reinterpret_cast<PrepareFn>(GetProcAddress(dll, "sqlite3_prepare_v2"));
    auto bind = reinterpret_cast<BindFn>(GetProcAddress(dll, "sqlite3_bind_text"));
    auto step = reinterpret_cast<StepFn>(GetProcAddress(dll, "sqlite3_step"));
    auto text = reinterpret_cast<TextFn>(GetProcAddress(dll, "sqlite3_column_text"));
    auto bytes = reinterpret_cast<BytesFn>(GetProcAddress(dll, "sqlite3_column_bytes"));
    auto finalize = reinterpret_cast<FinalizeFn>(GetProcAddress(dll, "sqlite3_finalize"));
    auto close = reinterpret_cast<CloseFn>(GetProcAddress(dll, "sqlite3_close"));
    if (!open || !prepare || !bind || !step || !text || !bytes || !finalize || !close) return {};

    std::string utf8Path(WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8Path.data(), static_cast<int>(utf8Path.size()), nullptr, nullptr);
    void* db = nullptr;
    constexpr int kReadOnly = 0x00000001;
    if (open(utf8Path.c_str(), &db, kReadOnly, nullptr) != 0 || !db) {
        if (db) close(db);
        return {};
    }
    std::string value;
    void* stmt = nullptr;
    if (prepare(db, "SELECT value FROM ItemTable WHERE key=?", -1, &stmt, nullptr) == 0 && stmt) {
        if (bind(stmt, 1, key, -1, nullptr) == 0 && step(stmt) == 100 /* SQLITE_ROW */) {
            const unsigned char* tx = text(stmt, 0);
            if (tx) value.assign(reinterpret_cast<const char*>(tx), static_cast<size_t>(bytes(stmt, 0)));
        }
        finalize(stmt);
    }
    close(db);
    return value;
}

std::wstring AppDataPath(const wchar_t* rest) {
    wchar_t appData[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return {};
    return std::wstring(appData) + rest;
}

// Reads the cached status. False when Antigravity isn't installed or the layout changed.
bool Read(Snapshot* out) {
    const std::string value = ReadStateValue(AppDataPath(L"\\Antigravity IDE\\User\\globalStorage\\state.vscdb"),
                                             "antigravityUnifiedStateSync.userStatus");
    if (value.empty()) return false;

    bb::agquota::Quota q;
    if (!bb::agquota::Decode(value, &q)) return false;
    Snapshot snap;
    snap.valid = true;
    snap.remaining = q.remaining;
    snap.model = std::wstring(q.model.begin(), q.model.end());
    snap.resetAt = q.resetAt;
    snap.pools = q.pools;
    snap.readAt = NowSeconds();
    *out = snap;
    return true;
}

// Starts the once-a-minute reader the first time the quota is wanted.
void EnsureStarted() {
    if (g_started.exchange(true)) return;
    std::thread([] {
        for (;;) {
            Snapshot snap;
            const bool ok = Read(&snap);
            // Bunbun: Cursor keeps only its plan locally (no usage numbers).
            std::string plan = ReadStateValue(AppDataPath(L"\\Cursor\\User\\globalStorage\\state.vscdb"),
                                              "cursorAuth/stripeMembershipType");
            if (plan.size() >= 2 && plan.front() == '"' && plan.back() == '"') plan = plan.substr(1, plan.size() - 2);
            {
                std::lock_guard lock(g_stateMutex);
                g_cursorPlan = std::wstring(plan.begin(), plan.end());
                if (ok) g_quota = snap;
                else if (NowSeconds() - g_quota.readAt > 600.0) g_quota.valid = false;
            }
            if (ok) g_layoutDirty = true;
            Sleep(60 * 1000);
        }
    }).detach();
}

Snapshot Get() {
    EnsureStarted();
    std::lock_guard lock(g_stateMutex);
    return g_quota;
}

// Bunbun: the most used pool's share used now, percent (a pool past its reset counts as refilled).
float UsedNow(const Snapshot& s) {
    const long long now = static_cast<long long>(time(nullptr));
    float used = 0.0f;
    for (const auto& p : s.pools) used = std::max(used, (1.0f - bb::agquota::RemainingNow(p.remaining, p.resetAt, now)) * 100.0f);
    if (s.pools.empty()) used = (1.0f - bb::agquota::RemainingNow(s.remaining, s.resetAt, now)) * 100.0f;
    return used;
}

std::wstring CursorPlan() {
    EnsureStarted();
    std::lock_guard lock(g_stateMutex);
    return g_cursorPlan;
}

}  // namespace AgQuota

// ── Answering agents from the island ─────────────────────────────────────────
// Plan feedback, replies to a finished task and free-text answers are typed in the
// Ask Bun box, aimed at the agent instead of at Bun (g_compose). Questions with
// several parts or multi-select step through a card (g_questions). Render thread.
struct ComposeTarget {
    int kind = 0;           // 0 none, 1 plan feedback, 2 reply, 3 free-text answer
    uint64_t id = 0;        // request id (1, 3) or reply id (2)
    int question = -1;      // kind 3: which question
    std::wstring label;     // shown in the box: "Reply to Claude Code · Bunbun"
};
ComposeTarget g_compose;

using QuestionState = bb::studio::Answers;  // studio_pages.h
QuestionState g_questions;

float g_planScroll = 0.0f;      // plan card: scrolled distance (wheel), render thread
float g_planScrollMax = 0.0f;   // set while drawing; 0 = nothing to scroll
uint64_t g_planScrollFor = 0;   // the request the scroll belongs to

// The approval card's diff: it grows to kDiffRowsShown lines, and the wheel scrolls the rest.
constexpr int kDiffRowsShown = 12;
constexpr float kDiffRowH = 16.0f;
float g_diffScroll = 0.0f;      // render thread, like the plan's
float g_diffScrollMax = 0.0f;
uint64_t g_diffScrollFor = 0;

void SyncQuestions(const bb::AgentView& a) { g_questions.Sync(a); }
bool NeedsQuestionCard(const bb::AgentView& a) { return bb::studio::NeedsQuestionCard(a); }
bool QuestionAnswered(int i) { return g_questions.Answered(i); }

void ClearCompose() { g_compose = ComposeTarget{}; }
bool ComposeActive() { return g_compose.kind != 0; }

void StartCompose(int kind, uint64_t id, int question, const std::wstring& label) {
    HWND hwnd = g_hwnd;
    if (!hwnd) return;
    StartQuickLookup(hwnd, true);  // the box, however it was (not) open before
    g_compose = ComposeTarget{kind, id, question, label};  // after: opening a box clears any target
    {
        std::lock_guard lock(g_stateMutex);
        if (g_state.lookup.active) g_state.lookup.expiresAt = NowSeconds() + QuickLookup::kComposeIdleSeconds;
    }
    g_layoutDirty = true;
}

std::string QuestionsReply(const bb::AgentView& a);

// SubmitLookup hands the typed text here first; true when it went to an agent.
bool SubmitCompose(const std::wstring& raw) {
    if (g_compose.kind == 0) return false;
    std::wstring text = raw;
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    if (text.empty()) return true;
    const ComposeTarget target = g_compose;
    if (target.kind == 1) {
        bb::agents::DecideRaw(target.id, bb::studio::PlanChangeReply(text));
    } else if (target.kind == 2) {
        bb::agents::SendReply(target.id, text);
    } else if (target.kind == 3 && target.question >= 0 &&
               target.question < static_cast<int>(g_questions.other.size()) && g_questions.requestId == target.id) {
        g_questions.other[target.question] = text;
        g_questions.picks[target.question].clear();
        // One single-choice question: the typed answer is the answer, like a pick (no Send to find).
        bb::AgentView a;
        {
            std::lock_guard lock(g_stateMutex);
            a = g_state.agent;
        }
        if (a.requestId == target.id && bb::studio::AnswersOnPick(a) && QuestionAnswered(0)) {
            const std::string reply = QuestionsReply(a);
            if (!reply.empty()) bb::agents::DecideRaw(a.requestId, reply);
        }
    }
    ClearCompose();
    CloseLookupPanel(g_hwnd);
    return true;
}

std::string QuestionsReply(const bb::AgentView& a) { return bb::studio::QuestionsReply(a, g_questions); }

// Agent card buttons that aren't plain Allow / Deny (AgentButton::action).
enum AgentAction { ActDecide = 0, ActPlanChange = 1, ActReply = 2, ActToggle = 3, ActNext = 4, ActOther = 5, ActBack = 6 };

void HandleAgentAction(int action, int arg) {
    bb::AgentView a;
    {
        std::lock_guard lock(g_stateMutex);
        a = g_state.agent;
    }
    const std::wstring who = a.project.empty() ? a.agent : a.agent + L" \x00B7 " + a.project;
    switch (action) {
        case ActPlanChange:
            StartCompose(1, a.requestId, -1, L"What should change in the plan?");
            break;
        case ActReply:
            if (a.replyId) StartCompose(2, a.replyId, -1, L"Reply to " + (a.replyTo.empty() ? who : a.replyTo));
            break;
        case ActToggle: {
            SyncQuestions(a);
            g_questions.Toggle(a, arg);
            // One single-choice question: the pick is the answer.
            if (bb::studio::AnswersOnPick(a) && QuestionAnswered(0)) {
                const std::string reply = QuestionsReply(a);
                if (!reply.empty()) bb::agents::DecideRaw(a.requestId, reply);
            }
            break;
        }
        case ActNext: {
            SyncQuestions(a);
            if (!QuestionAnswered(g_questions.index)) break;
            if (g_questions.index + 1 < static_cast<int>(a.questions.size())) {
                ++g_questions.index;
            } else {
                const std::string reply = QuestionsReply(a);
                if (!reply.empty()) bb::agents::DecideRaw(a.requestId, reply);
            }
            break;
        }
        case ActBack:
            if (g_questions.index > 0) --g_questions.index;
            break;
        case ActOther:
            SyncQuestions(a);
            StartCompose(3, a.requestId, g_questions.index, L"Your answer");
            break;
        default:
            break;
    }
    g_layoutDirty = true;
}

// ── The right-click menu's look ─────────────────────────────────────────────
// Dark menus like the island, with a small icon per item. Dark mode for classic
// Win32 menus is uxtheme's SetPreferredAppMode (ordinal 135) + FlushMenuThemes
// (136), the same switch Windows' own apps use; it covers the tray menu too, as it
// lives in this process. Icons are glyphs from Segoe Fluent Icons (Segoe MDL2
// Assets on Windows 10), drawn into premultiplied 32-bit bitmaps.
constexpr wchar_t kGlyphEyedropper = 0xEF3C;
constexpr wchar_t kGlyphTuck = 0xE70E;       // chevron up
constexpr wchar_t kGlyphPin = 0xE718;
constexpr wchar_t kGlyphDismiss = 0xE711;    // cancel
constexpr wchar_t kGlyphHide = 0xED1A;
constexpr wchar_t kGlyphTimer = 0xE916;      // stopwatch
constexpr wchar_t kGlyphMusic = 0xEC4F;
constexpr wchar_t kGlyphTray = 0xE8B7;       // folder
constexpr wchar_t kGlyphLyrics = 0xE720;     // microphone
constexpr wchar_t kGlyphAppearance = 0xE790; // colour
constexpr wchar_t kGlyphSettings = 0xE713;

void UseDarkMenus() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetModeFn = int(WINAPI*)(int);
    using FlushFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(2);  // ForceDark
    if (flush) flush();
}

HBITMAP MenuGlyphBitmap(wchar_t glyph) {
    static std::map<wchar_t, HBITMAP> cache;  // kept for the process's life, like the menu itself
    if (auto it = cache.find(glyph); it != cache.end()) return it->second;
    const int size = MulDiv(16, static_cast<int>(GetDpiForSystem()), 96);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { DeleteDC(dc); return nullptr; }
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT r{0, 0, size, size};
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    HFONT font = CreateFontW(-MulDiv(15, static_cast<int>(GetDpiForSystem()), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH, L"Segoe Fluent Icons");
    LOGFONTW lf{};
    GetObjectW(font, sizeof(lf), &lf);
    HGDIOBJ oldFont = SelectObject(dc, font);
    WORD index = 0;
    if (GetGlyphIndicesW(dc, &glyph, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || index == 0xFFFF) {
        // Windows 10: the same code points live in Segoe MDL2 Assets.
        SelectObject(dc, oldFont);
        DeleteObject(font);
        font = CreateFontW(-MulDiv(15, static_cast<int>(GetDpiForSystem()), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH, L"Segoe MDL2 Assets");
        oldFont = SelectObject(dc, font);
    }
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, old);
    DeleteDC(dc);
    // White-on-black coverage becomes alpha; the ink is a soft white, premultiplied.
    auto* px = static_cast<uint8_t*>(bits);
    for (int i = 0; i < size * size; ++i) {
        const uint8_t a = px[i * 4 + 1];
        px[i * 4 + 0] = static_cast<uint8_t>(232 * a / 255);
        px[i * 4 + 1] = static_cast<uint8_t>(232 * a / 255);
        px[i * 4 + 2] = static_cast<uint8_t>(232 * a / 255);
        px[i * 4 + 3] = a;
    }
    cache[glyph] = bmp;
    return bmp;
}

void SetMenuGlyph(HMENU menu, int position, wchar_t glyph) {
    HBITMAP bmp = MenuGlyphBitmap(glyph);
    if (!bmp || position < 0) return;
    MENUITEMINFOW mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_BITMAP;
    mi.hbmpItem = bmp;
    SetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &mi);
}

// Writes one key of settings.ini. "-" removes it, so a default is never frozen in.
void WriteBunbunSetting(const wchar_t* key, const wchar_t* value) {
    WritePrivateProfileStringW(L"Bunbun", key, wcscmp(value, L"-") == 0 ? nullptr : value, bb::SettingsPath());
}

// The release page of a newer Bunbun, set by IslandAnnounceUpdate (guarded by g_stateMutex).
std::wstring g_updatePageUrl;

// ── Tuck away ────────────────────────────────────────────────────────────────

std::atomic<bool> g_tucked{false};
double g_tuckedAt = 0.0;         // render thread: when it folded, for the line's grow-in
bool g_tuckPointerLeft = false;  // render thread: the pointer left the line once since tucking
double g_tuckHoverSince = 0.0;   // render thread: pointer on the line since (0 = not on it)

void SetTucked(bool tucked) {
    if (g_tucked.exchange(tucked) == tucked) return;
    g_tuckedAt = NowSeconds();
    g_tuckPointerLeft = false;
    g_tuckHoverSince = 0.0;
    g_layoutDirty = true;
}

// ── Poke reactions ───────────────────────────────────────────────────────────

std::atomic<double> g_mascotCrossUntil{0.0};
std::atomic<double> g_mascotDizzyUntil{0.0};
std::deque<double> g_pokeTimes;  // window proc only

// Called for every poke on the mascot. 3 within 1.6 s: cross; 6: dizzy.
void RegisterMascotPoke(double now) {
    if (!g_settings.pokeReactions) return;
    g_pokeTimes.push_back(now);
    while (!g_pokeTimes.empty() && now - g_pokeTimes.front() > 1.6) g_pokeTimes.pop_front();
    if (g_pokeTimes.size() >= 6) {
        g_mascotDizzyUntil.store(now + 2.6, std::memory_order_relaxed);
        g_mascotCrossUntil.store(0.0, std::memory_order_relaxed);
        g_pokeTimes.clear();
    } else if (g_pokeTimes.size() >= 3) {
        g_mascotCrossUntil.store(now + 1.4, std::memory_order_relaxed);
    }
}

// ── Tray drop animation ──────────────────────────────────────────────────────
// Set by TrayDropTarget::Drop; the Studio frame plays "caught it" for 1.6 s.
std::atomic<double> g_trayDropAt{-100.0};
std::wstring g_trayDropName;   // first file's name (render thread reads after the drop)
int g_trayDropCount = 0;

void NoteTrayDrop(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    const std::wstring& first = paths.front();
    const size_t slash = first.find_last_of(L"/\\");
    g_trayDropName = slash == std::wstring::npos ? first : first.substr(slash + 1);
    g_trayDropCount = static_cast<int>(paths.size());
    g_trayDropAt.store(NowSeconds());
}

// ── Home tiles ───────────────────────────────────────────────────────────────
// What each of Home's three slots shows: Bunbun.HomeTile1-3 (auto = agent, or the
// weather when no agent is shown; music; focus). Edit on Home cycles a slot.
using bb::studio::HomeTile;  // studio_pages.h
bool g_homeEdit = false;  // render thread

using bb::studio::kHomeTileIds;

bool AgentsShown(const Settings& s);  // above

HomeTile HomeTileKind(const Settings& s, int slot) {
    return bb::studio::HomeTileFor(s.homeTile[std::clamp(slot, 0, 2)], slot, AgentsShown(s),
                                   bb::work::WorkOn(static_cast<bb::work::UseCase>(s.useCase)));
}

void CycleHomeTile(int slot) {
    const Settings s = GetSettingsCopy();
    const int next = (static_cast<int>(HomeTileKind(s, slot)) + 1) % 7;
    wchar_t key[16];
    swprintf_s(key, L"HomeTile%d", slot + 1);
    WriteBunbunSetting(key, kHomeTileIds[next]);
}

// ── Your links on Home ───────────────────────────────────────────────────────
// "Name|address; Name|address" -> up to four (name, address). A bare address is
// named after its host or file name.
std::vector<std::pair<std::wstring, std::wstring>> ParseHomeLinks(const std::wstring& text) {
    return bb::studio::ParseHomeLinks(text);
}

// ── Focus page ───────────────────────────────────────────────────────────────
// The picker's hours (0-5) and minutes (0-59), remembered in state.ini.
int g_focusPickH = -1, g_focusPickM = -1;  // render thread; -1 = not loaded yet

void LoadFocusPicker() {
    if (g_focusPickH >= 0) return;
    const int total = std::clamp(Wh_GetIntValue(L"FocusPickMinutes", 25), 1, 359);
    g_focusPickH = total / 60;
    g_focusPickM = total % 60;
}

int FocusPickerMinutes() {
    LoadFocusPicker();
    return std::max(1, g_focusPickH * 60 + g_focusPickM);
}

void SaveFocusPicker() { Wh_SetIntValue(L"FocusPickMinutes", FocusPickerMinutes()); }

void StepFocusPicker(int column, int step) {
    LoadFocusPicker();
    if (column == 0) g_focusPickH = (g_focusPickH + step + 6) % 6;
    else g_focusPickM = (g_focusPickM + step + 60) % 60;
    if (g_focusPickH == 0 && g_focusPickM == 0) g_focusPickM = step > 0 ? 1 : 59;
    SaveFocusPicker();
    g_layoutDirty = true;
}

void SetFocusPicker(int minutes) {
    g_focusPickH = minutes / 60;
    g_focusPickM = minutes % 60;
    SaveFocusPicker();
    g_layoutDirty = true;
}

void AddFocusMinutes(int minutes) {
    {
        std::lock_guard lock(g_stateMutex);
        if (!g_state.timer.active) return;
        g_state.timer.totalSeconds += minutes * 60;
        if (g_state.timer.running) g_state.timer.endsAt += minutes * 60;
        else g_state.timer.remainingAtPause += minutes * 60;
    }
    g_layoutDirty = true;
}

// Focus guard: while a focus timer runs (not a break), a gentle note on the island
// when a distracting app or site comes to the front. At most once a minute per match.
void FocusGuardLoop() {
    std::wstring lastMatch;
    double lastAt = -1000.0;
    for (;;) {
        Sleep(2000);
        const Settings s = GetSettingsCopy();
        if (!s.focusGuard) continue;
        bool running = false;
        double left = 0.0;
        {
            std::lock_guard lock(g_stateMutex);
            // Bunbun: focus only; a plain countdown ("Timer") isn't a focus session.
            running = g_state.timer.active && g_state.timer.running && !g_state.timer.isBreak && g_state.timer.label != L"Timer";
            left = g_state.timer.endsAt - NowSeconds();
        }
        if (!running) continue;
        HWND fg = GetForegroundWindow();
        if (!fg) continue;
        wchar_t title[256] = {};
        GetWindowTextW(fg, title, ARRAYSIZE(title));
        std::wstring hay = title;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        std::wstring exe;
        if (ProcessImageNameForPid(pid, &exe)) hay += L" " + exe;
        for (auto& c : hay) c = towlower(c);
        std::wstring list = s.focusGuardApps;
        for (auto& c : list) c = towlower(c);
        std::wstring match;
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find(L',', start);
            if (end == std::wstring::npos) end = list.size();
            std::wstring word = list.substr(start, end - start);
            while (!word.empty() && iswspace(word.front())) word.erase(word.begin());
            while (!word.empty() && iswspace(word.back())) word.pop_back();
            if (word.size() >= 2 && hay.find(word) != std::wstring::npos) { match = word; break; }
            start = end + 1;
        }
        const double now = NowSeconds();
        if (match.empty() || (match == lastMatch && now - lastAt < 60.0)) continue;
        lastMatch = match;
        lastAt = now;
        const int mins = std::max(1, static_cast<int>((left + 59.0) / 60.0));
        {
            std::lock_guard lock(g_stateMutex);
            g_state.notification.active = true;
            g_state.notification.app = L"Focus";
            g_state.notification.title = L"Still focusing \x00B7 " + std::to_wstring(mins) + (mins == 1 ? L" min left" : L" min left");
            std::wstring name = match;
            if (!name.empty()) name[0] = towupper(name[0]);
            g_state.notification.body = name + L" can wait until the timer ends.";
            g_state.notification.icon = {};
            g_state.notification.expiresAt = now + 4.0;
        }
        g_layoutDirty = true;
        if (HWND hwnd = g_hwnd) PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
    }
}

void EnsureFocusGuard() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread(FocusGuardLoop).detach();
}

// ── Clickable spots ──────────────────────────────────────────────────────────

enum class StudioAction {
    Tab, Volume, PlayPause, OpenMusic, FocusStart, FocusStop, AgentPage, AgentFocus,
    ClipCopy, ClipClear, NoteToggle, NoteEdit, Settings, Close,
    ChatChip, ChatNew, ChatPanel, ChatClose, ColorPicker, ModelCycle, AgentReply,
    FocusWheel, FocusPreset, FocusAdd5, FocusPause, FocusPage, OpenTab, OpenLink, MirrorStart, MirrorStop, HomeEdit, HomeTileCycle,
    Brightness,  // Bunbun: the System page's brightness slider
    SoundMic, SoundTest, SoundOpen, SoundDevice, SoundToggle,  // Bunbun: the Mirror page's sound check (sound_check_module.inc)
    WaterAdd, WaterUndo, WaterRefill,  // Bunbun: the Water page
    ReminderDone, ReminderSnooze, ReminderExpand,  // Bunbun: the reminder pill (todos_draw.inc)
    CalPrev, CalNext, CalToday, CalView, CalOpenWeek,  // Bunbun: the Calendar page's month / week views
    // Bunbun: the Timers page (timers_module.inc)
    TimerMode, PomoToggle, PomoSkip, PomoReset, PomoAuto, PomoLength, CountToggle, CountAdd, CountReset, CountPreset, CountSave,
    TimerRing, SwToggle, SwLap, SwReset, SwCopy,
    WeatherUnitToggle, WeatherCitySearch,  // Bunbun: Studio Weather page
    RadioToggle, RadioSettings,            // Bunbun: Wi-Fi / Bluetooth on the System page
    ClipFilter, ClipSelect, ClipSearch, ClipPin, ClipDelete, ClipSave,  // Bunbun: Clipboard page
    ScreenTimeRefresh, ScreenTimeLimitEdit, ScreenTimeDay,  // Bunbun: Screen Time page
    WorkStart, WorkPause, WorkResume, WorkBreak, WorkDone, WorkReport, WorkPage,  // Bunbun: Work mode
};

// Bunbun: the Calendar page's view (render thread): 0 = month, 1 = week, and how far
// from today's month / week it has been moved with ‹ ›.
int g_calView = 0;
int g_calMonthOffset = 0;
int g_calWeekOffset = 0;

// Rects are in the published content space (see PublishContentGeometry), so
// MediaContentFromClient maps a click into the same coordinates. Render thread only.
struct StudioHit {
    D2D1_RECT_F rect{};
    StudioAction action = StudioAction::Tab;
    int arg = 0;
};
std::vector<StudioHit> g_studioHits;
uint64_t g_studioHitsFrame = 0;  // GetTickCount64() of the frame that published them

// The pointer as a Studio hit-test point. Unlike MediaContentFromClient it has no
// "painted in the last 0.5 s" rule (a still island doesn't repaint, which made idle
// clicks miss) and doesn't depend on the last published content rect.
MediaContentPoint StudioPointFromClient(int clientX, int clientY) {
    // Studio spots are stored in client coordinates (see Renderer::SHit), so the
    // pointer needs no mapping. Kept as a function so every caller reads the same.
    MediaContentPoint out;
    out.valid = true;
    out.x = static_cast<float>(clientX);
    out.y = static_cast<float>(clientY);
    return out;
}

// Claude Code's model (settings.json "model"), read at most every 10 s.
std::string g_claudeModel;
double g_claudeModelAt = -100.0;

std::wstring ClaudeModelLabel() {
    if (NowSeconds() - g_claudeModelAt > 10.0) {
        g_claudeModelAt = NowSeconds();
        g_claudeModel = bb::hooks::ClaudeModel();
    }
    std::string m = g_claudeModel;
    for (auto& c : m) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (m.empty() || m == "default") return L"Default";
    if (m.find("opus") != std::string::npos) return L"Opus";
    if (m.find("sonnet") != std::string::npos) return L"Sonnet";
    if (m.find("haiku") != std::string::npos) return L"Haiku";
    return std::wstring(g_claudeModel.begin(), g_claudeModel.end());
}

void HandleAgentAction(int action, int arg);  // below

// Default -> Opus -> Sonnet -> Haiku -> Default. New Claude Code sessions use it.
void CycleClaudeModel() {
    static const char* kOrder[] = {"", "opus", "sonnet", "haiku"};
    const std::wstring now = ClaudeModelLabel();
    int i = 0;
    if (now == L"Opus") i = 1; else if (now == L"Sonnet") i = 2; else if (now == L"Haiku") i = 3;
    const std::string next = kOrder[(i + 1) % 4];
    std::wstring report;
    if (bb::hooks::SetClaudeModel(next, &report)) {
        g_claudeModel = next;
        g_claudeModelAt = NowSeconds();
    }
}

// `page` opens it on that page (bunbun-settings --page notes), also when it's already open.
void OpenSettingsApp(const wchar_t* page = nullptr) {
    wchar_t appPath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, appPath, ARRAYSIZE(appPath));
    if (len == 0 || len >= ARRAYSIZE(appPath)) return;
    if (wchar_t* slash = wcsrchr(appPath, L'\\')) {
        *(slash + 1) = L'\0';
        const std::wstring exe = std::wstring(appPath) + L"bunbun-settings.exe";
        const std::wstring args = page ? std::wstring(L"--page ") + page : std::wstring();
        ShellExecuteW(nullptr, L"open", exe.c_str(), page ? args.c_str() : nullptr, appPath, SW_SHOWNORMAL);
    }
}

bool ShellOpen(const std::wstring& target) {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

bool ProtocolRegistered(const wchar_t* scheme) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, scheme, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

// The Agent tile and page: bring the agent's window forward (terminal, IDE or the
// Claude app). With no session to find, open the Claude app itself.
void OpenAgentApp() {
    bb::AgentView agent;
    {
        std::lock_guard lock(g_stateMutex);
        agent = g_state.agent;
    }
    if (agent.agent.empty()) agent.agent = L"Claude Code";
    if (FocusAgentApp(agent)) return;
    if (ProtocolRegistered(L"claude")) ShellOpen(L"claude://");
    else ShellOpen(L"https://claude.ai/");
}

// Bunbun.MusicApp: auto | spotify | youtube-music | apple-music.
// The Music tile focuses whatever is playing when it is that app (or on auto);
// otherwise it opens the chosen app.
void OpenMusicApp() {
    const std::wstring choice = ToLowerCopy(GetStringSettingCopy(L"Bunbun.MusicApp"));
    std::wstring source, sourceName;
    bool available = false;
    {
        std::lock_guard lock(g_stateMutex);
        available = g_state.media.available;
        source = ToLowerCopy(g_state.media.sourceAppUserModelId);
        sourceName = ToLowerCopy(g_state.media.sourceName + L" " + g_state.media.title);
    }
    auto playing = [&](const wchar_t* needle) {
        return available && (source.find(needle) != std::wstring::npos || sourceName.find(needle) != std::wstring::npos);
    };
    // Nothing playing on Auto: open a music app rather than asking Windows to open
    // an empty "playing app" (which it answered with Settings).
    auto openDefaultMusic = [] {
        if (ProtocolRegistered(L"spotify")) ShellOpen(L"spotify:");
        else ShellOpen(L"https://music.youtube.com/");
    };
    if (choice.empty() || choice == L"auto") {
        if (available) OpenRelevantApp(); else openDefaultMusic();
        return;
    }
    if (choice == L"spotify") {
        if (playing(L"spotify")) { OpenRelevantApp(); return; }
        if (!ProtocolRegistered(L"spotify") || !ShellOpen(L"spotify:")) ShellOpen(L"https://open.spotify.com/");
    } else if (choice == L"youtube-music") {
        if (playing(L"youtube music") || playing(L"music.youtube")) { OpenRelevantApp(); return; }
        ShellOpen(L"https://music.youtube.com/");
    } else if (choice == L"apple-music") {
        if (playing(L"applemusic") || playing(L"apple music")) { OpenRelevantApp(); return; }
        if (!ShellOpen(L"shell:AppsFolder\\AppleInc.AppleMusicWin_nzyj5cx40ttqa!App")) ShellOpen(L"https://music.apple.com/");
    } else if (available) {
        OpenRelevantApp();
    } else {
        openDefaultMusic();
    }
}

float g_studioVolumeLevelPending = -1.0f;  // last level sent while dragging, to skip repeats
// g_studioVolumeDrag and g_suppressVolumeNotificationUntil are declared in island_core.cpp
D2D1_RECT_F g_studioVolumeRect{};          // that slider's rect, in content space

void SetStudioVolume(const MediaContentPoint& pt) {
    if (!g_endpointVolume) {
        BindVolumeEndpoint();
    }
    const float w = g_studioVolumeRect.right - g_studioVolumeRect.left;
    if (!pt.valid || w <= 1.0f || !g_endpointVolume) return;
    const float level = Clamp((pt.x - g_studioVolumeRect.left) / w, 0.0f, 1.0f);
    if (std::fabs(level - g_studioVolumeLevelPending) < 0.005f) return;
    g_studioVolumeLevelPending = level;
    g_suppressVolumeNotificationUntil.store(NowSeconds() + 1.5, std::memory_order_relaxed);
    g_endpointVolume->SetMasterVolumeLevelScalar(level, nullptr);
    g_endpointVolume->SetMute(FALSE, nullptr);
    {
        std::lock_guard lock(g_stateMutex);
        g_state.system.volumePercent = static_cast<int>(std::round(level * 100.0f));
        g_state.system.volumeMuted = false;
        g_state.muted = false;
        g_state.volume.active = false;
    }
    g_layoutDirty = true;
}

// ── Ask Bun as a chat (Studio) ──
// The lookup panel keeps one answer at a time; the chat view remembers the last
// few questions and answers on the render thread, in memory only.
struct ChatTurn {
    std::wstring question;
    LookupSnapshot answer;
    uint64_t seq = 0;  // g_lookupRequestSeq when it was asked
};
std::vector<ChatTurn> g_chat;
std::vector<std::wstring> g_chatChips;  // suggestion chips on screen, by hit arg
float g_chatScroll = 0.0f;              // scrolled distance in transcript (wheel)
float g_chatScrollMax = 0.0f;           // max scroll distance; 0 = fits in view
uint64_t g_chatLastSeq = 0;             // request seq to detect new questions and auto-scroll

int StudioHitAt(const MediaContentPoint& pt) {
    if (!pt.valid) return -1;
    for (int i = static_cast<int>(g_studioHits.size()) - 1; i >= 0; --i) {
        const D2D1_RECT_F& r = g_studioHits[static_cast<size_t>(i)].rect;
        if (pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom) return i;
    }
    return -1;
}

// Left button down: a press on the volume slider starts a drag (the window
// captures the mouse until the button comes up).
// Bunbun: the System page's brightness slider. The built-in panel only (SetPanelBrightness);
// never below 5 %, so a stray drag can't leave the screen black.
bool g_studioBrightDrag = false;
float g_studioBrightPending = -1.0f;  // last level sent while dragging, to skip repeats
D2D1_RECT_F g_studioBrightRect{};

void SetStudioBrightness(const MediaContentPoint& pt) {
    const float w = g_studioBrightRect.right - g_studioBrightRect.left;
    if (!pt.valid || w <= 1.0f) return;
    const float level = Clamp((pt.x - g_studioBrightRect.left) / w, 0.05f, 1.0f);
    if (std::fabs(level - g_studioBrightPending) < 0.01f) return;
    g_studioBrightPending = level;
    g_suppressBrightnessUntil.store(NowSeconds() + 1.5, std::memory_order_relaxed);
    SetPanelBrightness(static_cast<int>(std::round(level * 100.0f)));
    g_layoutDirty = true;
}

bool HandleStudioButtonDown(HWND hwnd, int x, int y) {
    if (g_studioHits.empty()) return false;
    const MediaContentPoint pt = StudioPointFromClient(x, y);
    const int i = StudioHitAt(pt);
    if (i < 0) return false;
    const StudioHit& hit = g_studioHits[static_cast<size_t>(i)];
    if (hit.action == StudioAction::TimerRing && hit.arg == 1) {  // Bunbun: set the Focus length
        Timers::g_ringDrag = true;
        Timers::g_ringFocus = true;
        Timers::g_ringCenter = D2D1::Point2F((hit.rect.left + hit.rect.right) * 0.5f, (hit.rect.top + hit.rect.bottom) * 0.5f);
        SetCapture(hwnd);
        Timers::SetCountdownFromPoint(pt);
        return true;
    }
    if (hit.action == StudioAction::TimerRing && hit.arg == 0 && !Timers::CountdownRunning()) {  // Bunbun: set the countdown
        Timers::g_ringDrag = true;
        Timers::g_ringCenter = D2D1::Point2F((hit.rect.left + hit.rect.right) * 0.5f, (hit.rect.top + hit.rect.bottom) * 0.5f);
        SetCapture(hwnd);
        Timers::SetCountdownFromPoint(pt);
        return true;
    }
    if (hit.action == StudioAction::Brightness) {  // Bunbun: start dragging the brightness slider
        g_studioBrightDrag = true;
        g_studioBrightRect = hit.rect;
        g_studioBrightPending = -1.0f;
        SetCapture(hwnd);
        SetStudioBrightness(pt);
        return true;
    }
    if (hit.action != StudioAction::Volume || hit.arg != 0) return false;
    g_suppressVolumeNotificationUntil.store(NowSeconds() + 1.5, std::memory_order_relaxed);
    g_studioVolumeDrag = true;
    g_studioVolumeRect = hit.rect;
    g_studioVolumeLevelPending = -1.0f;
    SetCapture(hwnd);
    SetStudioVolume(pt);
    return true;
}

// Returns true when the click belonged to the Studio frame or one of its pages.
// Opens a dashboard page from a click. The scroll ring remembers its own stop while
// the pointer is over the island and steers the page back to it, which undid tab
// and All apps clicks; so the ring is moved to the clicked page too.
void ShowDashboardTab(int tab) {
    if (tab < 0) return;
    g_idleTab = tab;
    g_activityIndex.store(g_ringActivityCount.load(std::memory_order_relaxed) + tab, std::memory_order_relaxed);
    g_layoutDirty = true;
}

bool HandleStudioClick(HWND hwnd, int x, int y) {
    if (Timers::g_ringDrag) {  // Bunbun: the end of a countdown ring drag
        Timers::g_ringDrag = false;
        ReleaseCapture();
        Timers::SetCountdownFromPoint(StudioPointFromClient(x, y));
        if (Timers::g_ringFocus) {  // Bunbun: the Focus length, saved once
            Timers::g_ringFocus = false;
            if (Timers::g_focusDrag > 0) Timers::SaveFocusMinutes(Timers::g_focusDrag);
        }
        return true;
    }
    if (g_studioBrightDrag) {  // Bunbun: the end of a brightness drag
        g_studioBrightDrag = false;
        ReleaseCapture();
        SetStudioBrightness(StudioPointFromClient(x, y));
        g_studioBrightPending = -1.0f;
        return true;
    }
    if (g_studioVolumeDrag) {
        g_suppressVolumeNotificationUntil.store(NowSeconds() + 1.5, std::memory_order_relaxed);
        g_studioVolumeDrag = false;
        ReleaseCapture();
        SetStudioVolume(StudioPointFromClient(x, y));
        g_studioVolumeLevelPending = -1.0f;
        return true;
    }
    if (g_studioHits.empty()) return false;
    const MediaContentPoint pt = StudioPointFromClient(x, y);
    const int i = StudioHitAt(pt);
    if (i < 0) return false;
    const StudioHit hit = g_studioHits[static_cast<size_t>(i)];
    switch (hit.action) {
        case StudioAction::Tab:
            ShowDashboardTab(hit.arg);
            break;
        case StudioAction::Volume:
            g_suppressVolumeNotificationUntil.store(NowSeconds() + 1.5, std::memory_order_relaxed);
            if (hit.arg == -1) {  // the speaker icon: mute / unmute
                ToggleEndpointMute();
            } else {
                g_studioVolumeRect = hit.rect;
                g_studioVolumeLevelPending = -1.0f;
                SetStudioVolume(pt);
            }
            break;
        case StudioAction::Brightness:  // Bunbun: a tap on the brightness slider (drags start in HandleStudioButtonDown)
            g_studioBrightRect = hit.rect;
            g_studioBrightPending = -1.0f;
            SetStudioBrightness(pt);
            break;
        case StudioAction::PlayPause:
            SendMediaTransportCommand(1);
            break;
        case StudioAction::OpenMusic:
            OpenMusicApp();
            break;
        case StudioAction::FocusStart:
            StartFocusTimer(hit.arg > 0 ? hit.arg : FocusPickerMinutes(), false);
            break;
        case StudioAction::FocusStop:
            StopFocusTimer();
            break;
        case StudioAction::AgentPage:
            ShowDashboardTab(DashboardTabIndexOf(kTabAgent));
            break;
        case StudioAction::AgentFocus:
            OpenAgentApp();
            break;
        case StudioAction::ClipCopy: {
            const std::vector<std::wstring> items = ClipHistorySnapshot();
            if (hit.arg >= 0 && hit.arg < static_cast<int>(items.size()))
                StudioCopyText(hwnd, items[static_cast<size_t>(hit.arg)]);
            break;
        }
        case StudioAction::ClipClear: {
            {
                std::lock_guard lock(g_stateMutex);
                g_clipHistory.clear();
            }
            StudioClearRichClips();  // Bunbun: the Clipboard page's list too (pinned stay)
            break;
        }
        case StudioAction::NoteToggle:
            QuickNote::Toggle(hit.arg);
            break;
        case StudioAction::NoteEdit:  // Bunbun: Settings' Notes page (it was Notepad)
            OpenSettingsApp(L"notes");
            break;
        case StudioAction::Settings:
            OpenSettingsApp();
            break;
        case StudioAction::Close:
            g_pinnedExpanded.store(false);
            g_clickExpanded.store(false);
            g_hoverSuppressed.store(true);
            Wh_SetIntValue(L"PinnedExpanded", 0);
            g_layoutDirty = true;
            break;
        case StudioAction::FocusWheel: {
            // Clicking the upper half of a column steps down, the lower half up.
            const float mid = (hit.rect.top + hit.rect.bottom) * 0.5f;
            StepFocusPicker(hit.arg, pt.y < mid ? -1 : 1);
            break;
        }
        case StudioAction::FocusPreset:
            SetFocusPicker(hit.arg);
            break;
        case StudioAction::FocusAdd5:
            AddFocusMinutes(5);
            break;
        case StudioAction::FocusPause:
            ToggleTimerPause();
            break;
        case StudioAction::FocusPage:
            ShowDashboardTab(DashboardTabIndexOf(kTabFocus));
            break;
        case StudioAction::WorkPage:  // Bunbun: Work mode
            ShowDashboardTab(DashboardTabIndexOf(kTabWork));
            break;
        case StudioAction::WorkStart: WorkCommand(WorkButton::Start); break;
        case StudioAction::WorkPause: WorkCommand(WorkButton::Pause); break;
        case StudioAction::WorkResume: WorkCommand(WorkButton::Resume); break;
        case StudioAction::WorkBreak: WorkCommand(WorkButton::Break); break;
        case StudioAction::WorkDone: WorkCommand(WorkButton::Done); break;
        case StudioAction::WorkReport: OpenSettingsApp(L"work-report"); break;
        case StudioAction::HomeEdit:
            g_homeEdit = !g_homeEdit;
            break;
        case StudioAction::HomeTileCycle:
            CycleHomeTile(hit.arg);
            break;
        case StudioAction::MirrorStart:
            Mirror::Start();
            break;
        case StudioAction::MirrorStop:
            Mirror::Stop();
            break;
        case StudioAction::SoundMic:
            if (SoundCheck::g_running.load()) SoundCheck::StopMic();
            else SoundCheck::StartMic();
            break;
        case StudioAction::SoundTest:
            SoundCheck::TestSpeaker();
            break;
        case StudioAction::SoundOpen:
            SoundCheck::OpenSoundSettings();
            break;
        case StudioAction::SoundDevice:  // arg: 0 / 1 = microphone previous / next, 2 / 3 = speaker
            SoundCheck::CycleDevice(hit.arg < 2, (hit.arg & 1) ? 1 : -1);
            break;
        case StudioAction::SoundToggle:
            SoundCheck::g_open = !SoundCheck::g_open.load();
            break;
        case StudioAction::TimerMode:  // Bunbun: the Timers page
            Timers::SetMode(static_cast<bb::timers::Mode>(std::clamp(hit.arg, 0, bb::timers::kModeCount - 1)));
            break;
        case StudioAction::PomoToggle:
            Timers::PomodoroToggle(g_settings);
            break;
        case StudioAction::PomoSkip:
            Timers::PomodoroSkip(g_settings);
            break;
        case StudioAction::PomoReset:
            Timers::PomodoroReset();
            break;
        case StudioAction::PomoLength:  // Bunbun: a focus-length chip; 25 is the default, so it's written as unset
            Timers::g_focusDrag = -1;
            Timers::SaveFocusMinutes(hit.arg);
            break;
        case StudioAction::PomoAuto:  // the default is on: off is written, on goes back to the default
            WriteBunbunSetting(L"PomodoroAutoStart", g_settings.pomoAutoStart ? L"false" : L"-");
            break;
        case StudioAction::CountToggle:
            Timers::CountdownToggle();
            break;
        case StudioAction::CountAdd:
            Timers::CountdownAdd(hit.arg);
            break;
        case StudioAction::CountReset:
            Timers::CountdownReset();
            break;
        case StudioAction::CountPreset:
            Timers::CountdownSet(hit.arg);
            break;
        case StudioAction::CountSave:
            Timers::SavePreset(g_settings);
            break;
        case StudioAction::TimerRing:
            break;  // handled on button down (a drag)
        case StudioAction::SwToggle:
            Timers::StopwatchToggle();
            break;
        case StudioAction::SwLap:
            Timers::StopwatchLap();
            break;
        case StudioAction::SwReset:
            Timers::StopwatchReset();
            break;
        case StudioAction::SwCopy:
            StudioCopyText(hwnd, bb::timers::LapsText(Timers::g_laps));
            break;
        case StudioAction::WaterAdd:  // Bunbun: the Water page
            WaterDrink();
            break;
        case StudioAction::WaterUndo:
            WaterUndo();
            break;
        case StudioAction::WaterRefill:
            WaterRefill();
            break;
        case StudioAction::CalPrev:  // Bunbun: the Calendar page
        case StudioAction::CalNext: {
            const int step = hit.action == StudioAction::CalNext ? 1 : -1;
            if (g_calView == 0) g_calMonthOffset = std::clamp(g_calMonthOffset + step, -24, 24);
            else g_calWeekOffset = std::clamp(g_calWeekOffset + step, -104, 104);
            break;
        }
        case StudioAction::CalToday:
            g_calMonthOffset = g_calWeekOffset = 0;
            break;
        case StudioAction::CalView:
            // Bunbun: the week view opens on a week of the month on screen (its first week,
            // or this week when that's the current month), and back again.
            if (hit.arg == 1 && g_calView == 0) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                int y = st.wYear, m = st.wMonth + g_calMonthOffset;
                while (m < 1) { m += 12; --y; }
                while (m > 12) { m -= 12; ++y; }
                const int64_t todayDays = bb::ics::DaysFromCivil(st.wYear, st.wMonth, st.wDay);
                const int64_t firstDays = bb::ics::DaysFromCivil(y, m, 1);
                const int64_t delta = g_calMonthOffset == 0 ? 0 : firstDays - todayDays;
                g_calWeekOffset = static_cast<int>(delta >= 0 ? (delta + 6) / 7 : -((-delta) / 7));
            } else if (hit.arg == 0 && g_calView == 1) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                int y = 0, m = 0, d = 0;
                bb::ics::CivilFromDays(bb::ics::DaysFromCivil(st.wYear, st.wMonth, st.wDay) + 7LL * g_calWeekOffset, &y, &m, &d);
                g_calMonthOffset = (y - st.wYear) * 12 + (m - st.wMonth);
            }
            g_calView = hit.arg;
            break;
        case StudioAction::CalOpenWeek:  // a day in the month: its week
            g_calView = 1;
            g_calWeekOffset = hit.arg;
            break;
        case StudioAction::ReminderDone:  // Bunbun: the reminder pill
            ReminderPillAction(1);
            break;
        case StudioAction::ReminderSnooze:
            ReminderPillAction(2);
            break;
        case StudioAction::ReminderExpand:
            ReminderPillAction(3);
            break;
        case StudioAction::OpenTab:
            ShowDashboardTab(DashboardTabIndexOf(hit.arg));
            break;
        case StudioAction::OpenLink: {
            const auto links = ParseHomeLinks(GetSettingsCopy().homeLinks);
            if (hit.arg >= 0 && hit.arg < static_cast<int>(links.size()))
                ShellExecuteW(nullptr, L"open", links[static_cast<size_t>(hit.arg)].second.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case StudioAction::ModelCycle:
            CycleClaudeModel();
            break;
        case StudioAction::AgentReply:
            HandleAgentAction(2 /*ActReply*/, 0);
            break;
        case StudioAction::ColorPicker:
            ColorPicker::Start();
            break;
        case StudioAction::ChatChip:
            TouchLookupPanel();
            if (hit.arg >= 0 && hit.arg < static_cast<int>(g_chatChips.size()))
                SubmitLookup(g_chatChips[static_cast<size_t>(hit.arg)]);
            break;
        case StudioAction::ChatNew:
            TouchLookupPanel();
            g_chat.clear();
            g_chatScroll = 0.0f;
            g_chatScrollMax = 0.0f;
            g_chatLastSeq = 0;
            break;
        case StudioAction::ChatPanel:
            TouchLookupPanel();  // a click inside the chat keeps it open and focused
            break;
        case StudioAction::ChatClose:  // Bunbun: the chat had no way to be closed with the mouse
            CloseLookupPanel(g_hwnd);
            break;
        case StudioAction::RadioToggle: {  // Bunbun: arg 0 Wi-Fi, 1 Bluetooth
            int radio = -1;
            {
                std::lock_guard lock(g_stateMutex);
                radio = hit.arg == 1 ? g_state.connectivity.bluetoothRadio : g_state.connectivity.wifiRadio;
            }
            if (radio >= 0) SetRadioAsync(hit.arg == 1, radio != 1);  // opens Windows settings if it can't
            break;
        }
        case StudioAction::RadioSettings:  // Bunbun: 0 Wi-Fi, 1 Bluetooth, 2 Ethernet: pick a network or device
            ShellExecuteW(nullptr, L"open", hit.arg == 1 ? L"ms-settings:bluetooth" : hit.arg == 2 ? L"ms-settings:network-ethernet" : L"ms-settings:network-wifi",
                          nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case StudioAction::WeatherUnitToggle: {
            bool nextF = false;
            {
                std::lock_guard lock(g_settingsMutex);
                g_settings.weatherFahrenheit = !g_settings.weatherFahrenheit;
                nextF = g_settings.weatherFahrenheit;
            }
            // Bunbun: Modules.WeatherFahrenheit, where the setting lives (it was written to
            // [Bunbun], so the reload this triggers read the old unit back). Off = unset, the default.
            WritePrivateProfileStringW(L"Modules", L"WeatherFahrenheit", nextF ? L"true" : nullptr, bb::SettingsPath());
            WriteBunbunSetting(L"WeatherFahrenheit", L"-");  // the stray copy older builds left
            Wh_SetIntValue(L"WeatherFahrenheit", nextF ? 1 : 0);
            if (g_settingsChangedEvent) SetEvent(g_settingsChangedEvent);
            break;
        }
        case StudioAction::WeatherCitySearch:
            StartQuickLookup(hwnd, true);
            g_lookupUi.text = L"weather in ";
            g_lookupUi.caret = static_cast<int>(g_lookupUi.text.size());
            break;
        case StudioAction::ClipFilter:
            g_clipFilter = hit.arg;
            g_clipSelected = 0;
            break;
        case StudioAction::ClipSelect:
            g_clipSelected = hit.arg;
            break;
        case StudioAction::ClipSearch:
            StartQuickLookup(hwnd, true);
            g_lookupUi.text = L"clip ";
            g_lookupUi.caret = static_cast<int>(g_lookupUi.text.size());
            break;
        case StudioAction::ClipPin:
            StudioPinRichClip(hit.arg);
            break;
        case StudioAction::ClipDelete:
            StudioDeleteRichClip(hit.arg);
            break;
        case StudioAction::ClipSave:
            StudioSaveRichClip(hit.arg);
            break;
        case StudioAction::ScreenTimeRefresh:
            break;  // redraws below
        case StudioAction::ScreenTimeDay:  // a bar of the week: that day's time and apps
            screentime::ShowDay(hit.arg);
            break;
        case StudioAction::ScreenTimeLimitEdit: {
            // Bunbun: a click on the limit steps it: none, 2 h, 4 h ... 12 h, none.
            static const int kSteps[] = {0, 2, 4, 6, 8, 10, 12};
            int next = 0;
            for (size_t k = 0; k + 1 < std::size(kSteps); ++k)
                if (g_settings.screenTimeLimit == kSteps[k]) next = kSteps[k + 1];
            WriteBunbunSetting(L"ScreenTimeLimit", next ? std::to_wstring(next).c_str() : L"-");
            break;
        }
    }
    g_layoutDirty = true;
    return true;
}

// The wheel over the Focus page's picker turns that column. True when it did.
bool HandleStudioWheel(HWND hwnd, LPARAM lParam, int step) {
    if (g_studioHits.empty()) return false;
    POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};  // WM_MOUSEWHEEL is in screen coordinates
    ScreenToClient(hwnd, &p);
    const int i = StudioHitAt(StudioPointFromClient(p.x, p.y));
    if (i >= 0 && g_studioHits[static_cast<size_t>(i)].action == StudioAction::TimerRing && g_studioHits[static_cast<size_t>(i)].arg == 1) {
        // Bunbun: the Focus dial, 5 minutes a notch
        const int from = Timers::g_focusDrag > 0 ? Timers::g_focusDrag : Timers::Config(g_settings).focusMinutes;
        Timers::g_focusDrag = bb::timers::StepFocusMinutes(from, -step);
        Timers::SaveFocusMinutes(Timers::g_focusDrag);
        g_layoutDirty = true;
        return true;
    }
    if (i >= 0 && g_studioHits[static_cast<size_t>(i)].action == StudioAction::TimerRing && !Timers::CountdownRunning()) {
        const bool fine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;  // Bunbun: Shift steps seconds
        Timers::CountdownSet(bb::timers::StepCountdown(Timers::g_countSet, -step, fine));
        return true;
    }
    if (i < 0 || g_studioHits[static_cast<size_t>(i)].action != StudioAction::FocusWheel) return false;
    StepFocusPicker(g_studioHits[static_cast<size_t>(i)].arg, step);
    return true;
}

// Which spot the pointer is over, for hover feedback (-1 = none), and the
// volume drag. Render thread.
// Kept as the button itself (action + arg), not its index: the hit list is rebuilt
// every frame, and when the number of spots drawn before the header changed, an
// index pointed at the wrong button (the gear and eyedropper never lit up).
struct StudioHoverSpot { bool on = false; StudioAction action{}; int arg = 0; };
StudioHoverSpot g_studioHover;
void HandleStudioMouseMove(int x, int y) {
    const MediaContentPoint pt = StudioPointFromClient(x, y);
    if (g_studioBrightDrag) {  // Bunbun: dragging the brightness slider
        SetStudioBrightness(pt);
        return;
    }
    if (g_studioVolumeDrag) {
        g_suppressVolumeNotificationUntil.store(NowSeconds() + 1.5, std::memory_order_relaxed);
        SetStudioVolume(pt);
        return;
    }
    if (Timers::g_ringDrag) {  // Bunbun: dragging the countdown ring
        Timers::SetCountdownFromPoint(pt);
        return;
    }
    const int i = g_studioHits.empty() ? -1 : StudioHitAt(pt);
    StudioHoverSpot next;
    if (i >= 0) next = {true, g_studioHits[static_cast<size_t>(i)].action, g_studioHits[static_cast<size_t>(i)].arg};
    if (next.on != g_studioHover.on || next.action != g_studioHover.action || next.arg != g_studioHover.arg) {
        g_studioHover = next;
        g_layoutDirty = true;
    }
}

void ClearStudioHover() {
    if (!g_studioHover.on) return;
    g_studioHover = {};
    g_layoutDirty = true;
}

