// Bunbun — the Studio layout on Linux.
//
// The same frame as Windows (src/island/modules/studio_draw.inc): a header with
// the page name, a tiny mascot, the date and time, the page, and a tab bar with
// one icon per page. The page list and order (Bunbun.PageOrder), names, colours,
// Home cards and the agent replies come from src/island/studio_pages.h, and the
// icons from studio_icons.h (drawn through the Direct2D-on-Cairo stand-ins), so
// both systems agree. The layout and text are drawn here with Cairo and Pango.
//
// Pages: Home, Music and Lyrics (while something plays), Agent, Calendar, Weather, System,
// Quick toggles, To-do, Clipboard, Note, Focus and All apps; their data comes
// from sources.cpp. Also here: the plan and several-question cards, and typing on
// the island (a reply to a finished agent, what to change in a plan, an answer
// of your own).
#include "island_linux.h"

#include <gtk-layer-shell.h>
#include <pango/pangocairo.h>

#include <dirent.h>
#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "../../agents/hook_installer.h"
#include "../../common/ag_quota.h"
#include "../../common/agent_limits.h"
#include "../../island/studio_icons.h"
#include "platform.h"
#include "settings.h"
#include "calendar_linux.h"
#include "features_linux.h"
#include "shelf_linux.h"
#include "look_linux.h"
#include "lyrics_linux.h"
#include "sources.h"
#include "water_linux.h"
#include "work_linux.h"
#include "timers_linux.h"
#include "screentime_linux.h"

namespace bb::lisland {
namespace {

namespace st = bb::studio;
using st::Page;
using bb::AgentPhase;

constexpr double kPi = 3.14159265358979;

// ── Settings ────────────────────────────────────────────────────────────────

enum AgentKind { AgentClaude, AgentAntigravity, AgentCursor, AgentCopilot, AgentCodex, AgentOther, AgentKindCount };

struct StudioSettings {
    bool studio = true;
    bool agentTab = true;
    bool usageLimits = true;
    bool showAgent[AgentKindCount] = {true, true, true, true, true, true};
    int mainAgent = -1;  // -1 = auto
    int homeTile[3] = {-1, -1, -1};
    std::wstring homeLinks, pageOrder;
    int accentMode = 0;  // 0 auto (album art), 1 system, 2 custom
    uint32_t customAccent = 0x4cc9f0;
    // Pages (the same switches as Windows; File shelf, Mirror and Lyrics aren't on Linux yet)
    bool weather = true, system = true, toggles = true, todo = true, clipboard = true, note = true, focus = true, apps = true;
    bool antigravityQuota = true;  // Bunbun.AntigravityQuota
};
StudioSettings g_s;

// ── Antigravity quota ───────────────────────────────────────────────────────
// What Antigravity caches about its model quota (common/ag_quota.h), read from its
// state database once a minute on a worker thread. SQLite is loaded at run time
// (libsqlite3.so.0, on practically every desktop), so it's no dependency of ours.
struct AgSnapshot {
    bool valid = false;
    float remaining = 1.0f;
    long long resetAt = 0;
    std::vector<bb::agquota::Pool> pools;  // every pool of models, the most used first
};
std::mutex g_agMutex;
AgSnapshot g_ag;            // guarded by g_agMutex
std::wstring g_cursorPlan;  // Cursor's plan ("free", "pro"): all Cursor keeps locally; guarded by g_agMutex

// The first of these state databases that exists ("" for none).
std::string StateDb(std::initializer_list<const char*> dirs) {
    for (const char* dir : dirs) {
        const std::string p = bb::plat::HomeDir() + dir + "/User/globalStorage/state.vscdb";
        if (bb::plat::FileExists(p)) return p;
    }
    return {};
}

// One value from a VS Code-style state database, read-only ("" when missing).
std::string ReadStateValue(const std::string& path, const char* key) {
    if (path.empty()) return {};
    static void* lib = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return {};
    using OpenFn = int (*)(const char*, void**, int, const char*);
    using PrepareFn = int (*)(void*, const char*, int, void**, const char**);
    using BindFn = int (*)(void*, int, const char*, int, void (*)(void*));
    using StepFn = int (*)(void*);
    using TextFn = const unsigned char* (*)(void*, int);
    using BytesFn = int (*)(void*, int);
    using FinalizeFn = int (*)(void*);
    using CloseFn = int (*)(void*);
    auto open = reinterpret_cast<OpenFn>(dlsym(lib, "sqlite3_open_v2"));
    auto prepare = reinterpret_cast<PrepareFn>(dlsym(lib, "sqlite3_prepare_v2"));
    auto bind = reinterpret_cast<BindFn>(dlsym(lib, "sqlite3_bind_text"));
    auto step = reinterpret_cast<StepFn>(dlsym(lib, "sqlite3_step"));
    auto text = reinterpret_cast<TextFn>(dlsym(lib, "sqlite3_column_text"));
    auto bytes = reinterpret_cast<BytesFn>(dlsym(lib, "sqlite3_column_bytes"));
    auto finalize = reinterpret_cast<FinalizeFn>(dlsym(lib, "sqlite3_finalize"));
    auto close = reinterpret_cast<CloseFn>(dlsym(lib, "sqlite3_close"));
    if (!open || !prepare || !bind || !step || !text || !bytes || !finalize || !close) return {};
    void* db = nullptr;
    constexpr int kReadOnly = 0x00000001;
    if (open(path.c_str(), &db, kReadOnly, nullptr) != 0 || !db) {
        if (db) close(db);
        return {};
    }
    std::string value;
    void* stmt = nullptr;
    if (prepare(db, "SELECT value FROM ItemTable WHERE key=?", -1, &stmt, nullptr) == 0 && stmt) {
        if (bind(stmt, 1, key, -1, nullptr) == 0 && step(stmt) == 100 /* SQLITE_ROW */)
            if (const unsigned char* t = text(stmt, 0)) value.assign(reinterpret_cast<const char*>(t), static_cast<size_t>(bytes(stmt, 0)));
        finalize(stmt);
    }
    close(db);
    return value;
}

bool ReadAgQuota(AgSnapshot* out) {
    const std::string value = ReadStateValue(StateDb({"/.config/Antigravity IDE", "/.config/Antigravity"}),
                                             "antigravityUnifiedStateSync.userStatus");
    bb::agquota::Quota q;
    if (value.empty() || !bb::agquota::Decode(value, &q)) return false;
    *out = {true, q.remaining, q.resetAt, q.pools};
    return true;
}

std::wstring ReadCursorPlan() {
    std::string plan = ReadStateValue(StateDb({"/.config/Cursor"}), "cursorAuth/stripeMembershipType");
    if (plan.size() >= 2 && plan.front() == '"' && plan.back() == '"') plan = plan.substr(1, plan.size() - 2);
    return bb::plat::Wide(plan);
}

AgSnapshot AgQuota() {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            for (;;) {
                AgSnapshot snap;
                const bool ok = ReadAgQuota(&snap);
                const std::wstring plan = ReadCursorPlan();
                {
                    std::lock_guard lock(g_agMutex);
                    g_cursorPlan = plan;
                    if (ok) g_ag = snap;
                    else g_ag.valid = false;
                }
                if (ok) g_idle_add([](gpointer) -> gboolean { Redraw(); return G_SOURCE_REMOVE; }, nullptr);
                sleep(60);
            }
        }).detach();
    });
    if (!g_s.antigravityQuota || !g_s.showAgent[AgentAntigravity]) return {};
    std::lock_guard lock(g_agMutex);
    return g_ag;
}

std::wstring CursorPlan() {
    AgQuota();  // starts the reader
    std::lock_guard lock(g_agMutex);
    return g_cursorPlan;
}

// The most used pool's share used now, percent (a pool past its reset counts as refilled).
float AgUsedNow(const AgSnapshot& s) {
    const long long now = static_cast<long long>(time(nullptr));
    float used = 0;
    for (const auto& p : s.pools) used = std::max(used, (1.0f - bb::agquota::RemainingNow(p.remaining, p.resetAt, now)) * 100.0f);
    if (s.pools.empty()) used = (1.0f - bb::agquota::RemainingNow(s.remaining, s.resetAt, now)) * 100.0f;
    return used;
}

AgentKind AgentKindFor(const std::wstring& name) {
    std::wstring n = name;
    for (auto& c : n) c = towlower(c);
    if (n.find(L"claude") != std::wstring::npos || n.empty()) return AgentClaude;
    if (n.find(L"antigravity") != std::wstring::npos || n.find(L"gemini") != std::wstring::npos) return AgentAntigravity;
    if (n.find(L"cursor") != std::wstring::npos) return AgentCursor;
    if (n.find(L"vs code") != std::wstring::npos || n.find(L"copilot") != std::wstring::npos) return AgentCopilot;
    if (n.find(L"codex") != std::wstring::npos) return AgentCodex;
    return AgentOther;
}

const wchar_t* AgentKindName(int kind) {
    static const wchar_t* kNames[] = {L"Claude Code", L"Antigravity", L"Cursor", L"VS Code", L"Codex", L"Agents"};
    return kNames[std::clamp(kind, 0, AgentKindCount - 1)];
}

// Connected through Bunbun's installer (checked every 30 s) or seen sending
// events since Bunbun started: either makes an agent "in use".
unsigned g_agentsConnected = 0, g_agentsSeen = 0;
double g_agentsCheckedAt = -100;
int g_lastAgentKind = -1;

unsigned AgentsInUse() {
    if (g_agent.phase != AgentPhase::None && !g_agent.agent.empty()) {
        g_lastAgentKind = AgentKindFor(g_agent.agent);
        g_agentsSeen |= 1u << g_lastAgentKind;
    }
    if (Now() - g_agentsCheckedAt > 30) {
        g_agentsCheckedAt = Now();
        unsigned bits = 0;
        if (bb::hooks::ClaudeHooksInstalled()) bits |= 1u << AgentClaude;
        if (bb::hooks::AntigravityHooksInstalled()) bits |= 1u << AgentAntigravity;
        if (bb::hooks::CursorHooksInstalled()) bits |= 1u << AgentCursor;
        if (bb::hooks::CopilotHooksInstalled()) bits |= 1u << AgentCopilot;
        if (bb::hooks::CodexHooksInstalled()) bits |= 1u << AgentCodex;
        g_agentsConnected = bits;
    }
    return g_agentsConnected | g_agentsSeen;
}

// True when an agent you switched on is connected or has been active: only then
// do the Agent card, the Agent page and the usage ring appear.
bool AgentsShown() {
    if (!g_agentsOn) return false;
    const unsigned inUse = AgentsInUse();
    for (int k = 0; k < AgentKindCount; ++k)
        if (g_s.showAgent[k] && (inUse & (1u << k))) return true;
    return false;
}

// The agent the Home card and the header ring follow: the chosen main agent,
// else the one used last, else the first one switched on and in use.
int MainAgentKind() {
    if (g_s.mainAgent >= 0 && g_s.showAgent[g_s.mainAgent]) return g_s.mainAgent;
    if (g_lastAgentKind >= 0 && g_s.showAgent[g_lastAgentKind]) return g_lastAgentKind;
    const unsigned inUse = AgentsInUse();
    for (int k = 0; k < AgentKindCount; ++k)
        if (g_s.showAgent[k] && (inUse & (1u << k))) return k;
    return AgentClaude;
}

// The pages switched on; PageOrder orders them.
st::PageSwitches Switches() {
    st::PageSwitches p;
    p.studio = true;
    p.calendar = true;
    p.weather = g_s.weather;
    p.system = g_s.system;
    p.toggles = g_s.toggles;
    p.todo = g_s.todo;
    p.clipboard = g_s.clipboard;
    p.note = g_s.note;
    p.focus = g_s.focus;
    p.apps = g_s.apps;
    p.lyrics = llyrics::PageOn() && g_track.active && g_mediaOn;
    p.tray = bb::lshelf::ShelfOn();
    p.mirror = bb::lshelf::MirrorOn();
    p.water = false;  // Water is a mode of the Timers page (Bunbun.WaterPage = Water in Timers)
    p.screentime = bb::lscreen::On();
    p.work = bb::lwork::On();  // Work mode's page
    p.agent = g_s.agentTab && AgentsShown();
    p.order = g_s.pageOrder;
    return p;
}

// ── Colours and text ────────────────────────────────────────────────────────

struct Col {
    double r = 1, g = 1, b = 1, a = 1;
};
Col C(uint32_t rgb, double a = 1) { return {((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0, a}; }
Col W(double a) { return {1, 1, 1, a}; }
Col WithA(Col c, double a) { c.a = a; return c; }
Col Lerp(Col x, Col y, double t) { return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t}; }
D2D1_COLOR_F D2(Col c) { return D2D1::ColorF(static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b), static_cast<float>(c.a)); }
void Source(cairo_t* cr, Col c) {
    bb::llook::Ink(&c.r, &c.g, &c.b, &c.a);  // "white" is the theme's text colour
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

double g_alpha = 1.0;  // the frame's fade while the island opens
Col F(Col c) { c.a *= g_alpha; return c; }

// The same type ramp as Windows (studio_draw.inc's SFont).
enum Font { FTitle, FLabel, FBody, FBig, FHero, FMono, FSmall, FSong, FAsk, FInput };
struct FontSpec { double size; PangoWeight weight; bool mono; };
constexpr FontSpec kFonts[] = {
    {13, PANGO_WEIGHT_BOLD, false},  {11, PANGO_WEIGHT_SEMIBOLD, false}, {12, PANGO_WEIGHT_MEDIUM, false},
    {26, PANGO_WEIGHT_BOLD, false},  {40, PANGO_WEIGHT_BOLD, false},     {11, PANGO_WEIGHT_NORMAL, true},
    {10, PANGO_WEIGHT_MEDIUM, false}, {15, PANGO_WEIGHT_BOLD, false},    {13, PANGO_WEIGHT_MEDIUM, false},
    {14, PANGO_WEIGHT_NORMAL, false},
};

PangoLayout* Layout(cairo_t* cr, const std::string& text, Font f) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string(kFonts[f].mono ? "Monospace" : bb::llook::Font());
    pango_font_description_set_absolute_size(font, bb::llook::TextSize(kFonts[f].size) * PANGO_SCALE);
    pango_font_description_set_weight(font, kFonts[f].weight);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_text(layout, text.c_str(), -1);
    return layout;
}

double Measure(cairo_t* cr, const std::string& text, Font f) {
    if (text.empty()) return 0;
    PangoLayout* layout = Layout(cr, text, f);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    return w;
}

// One line in the box, vertically centred, ellipsized. align: 0 left, 1 centre, 2 right.
void SText(cairo_t* cr, const std::string& text, double x, double y, double w, double h, Font f, Col c, int align = 0) {
    if (text.empty() || w <= 0) return;
    PangoLayout* layout = Layout(cr, text, f);
    pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, align == 1 ? PANGO_ALIGN_CENTER : align == 2 ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    Source(cr, c);
    cairo_move_to(cr, x, y + (h - ph) / 2);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}
void SText(cairo_t* cr, const std::wstring& text, double x, double y, double w, double h, Font f, Col c, int align = 0) {
    SText(cr, bb::plat::Utf8(text), x, y, w, h, f, c, align);
}

// Wrapped text from (x, y); returns its height. Draws only when `draw`.
double WrapText(cairo_t* cr, const std::string& text, double x, double y, double w, Font f, Col c, bool draw = true) {
    if (text.empty()) return 0;
    PangoLayout* layout = Layout(cr, text, f);
    pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    if (draw) {
        Source(cr, c);
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);
    }
    g_object_unref(layout);
    return ph;
}

void Fill(cairo_t* cr, double x, double y, double w, double h, double r, Col c) {
    RoundRect(cr, x, y, w, h, r);
    Source(cr, c);
    cairo_fill(cr);
}

void Dot(cairo_t* cr, double cx, double cy, double r, Col c) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * kPi);
    Source(cr, c);
    cairo_fill(cr);
}

// A vertical glow inside the current clip: clear at the top, `c` at the bottom.
void Glow(cairo_t* cr, double x, double y, double w, double h, double r, Col c, double strength, double from = 0.18) {
    cairo_pattern_t* g = cairo_pattern_create_linear(0, y + h * from, 0, y + h);
    cairo_pattern_add_color_stop_rgba(g, 0.0, c.r, c.g, c.b, 0);
    cairo_pattern_add_color_stop_rgba(g, 0.55, c.r, c.g, c.b, 0.22 * strength);
    cairo_pattern_add_color_stop_rgba(g, 1.0, c.r, c.g, c.b, 0.62 * strength);
    RoundRect(cr, x, y, w, h, r);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

std::string Clock(double seconds) {
    const int s = std::max(0, static_cast<int>(seconds));
    char b[32];
    if (s >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

const char* StageWord(int stage) {
    switch (stage) {
        case 0: return "Reading";
        case 1: return "Editing";
        case 2: return "Testing";
        case 3: return "Done";
        default: return "Thinking";
    }
}

std::string U(const std::wstring& w) { return bb::plat::Utf8(w); }
const std::string kDot = " \xC2\xB7 ";

// ── Icons (shared with Windows, through the Direct2D stand-ins) ─────────────

ID2D1Factory* g_factory = nullptr;
ID2D1StrokeStyle* g_round = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;

st::Pen PenFor(cairo_t* cr) {
    bbcompat::SetCairo(g_rt, cr);
    if (!g_factory) {
        g_rt->GetFactory(&g_factory);
        g_factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                 D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                     nullptr, 0, &g_round);
        g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g_brush);
    }
    // Immediate mode: one brush, recoloured before each use.
    return {g_rt, g_factory, g_round, [](const D2D1_COLOR_F& c) { g_brush->SetColor(c); return g_brush; }};
}

D2D1_POINT_2F Pt(double x, double y) { return D2D1::Point2F(static_cast<float>(x), static_cast<float>(y)); }

void Icon(cairo_t* cr, Page page, double cx, double cy, double size, Col c) {
    cairo_save(cr);
    st::Icon(PenFor(cr), page, Pt(cx, cy), static_cast<float>(size), D2(c));
    cairo_restore(cr);
}

void Ring(cairo_t* cr, double cx, double cy, double r, double width, double fraction, Col c) {
    cairo_save(cr);
    st::Ring(PenFor(cr), Pt(cx, cy), static_cast<float>(r), static_cast<float>(width), static_cast<float>(fraction), D2(c),
             static_cast<float>(g_alpha));
    cairo_restore(cr);
}

void Gear(cairo_t* cr, double cx, double cy, double size, Col c) {
    cairo_save(cr);
    st::Gear(PenFor(cr), Pt(cx, cy), static_cast<float>(size), D2(c));
    cairo_restore(cr);
}

void Tick(cairo_t* cr, double cx, double cy, double scale, Col c) {
    cairo_save(cr);
    st::Tick(PenFor(cr), Pt(cx, cy), static_cast<float>(scale), D2(c));
    cairo_restore(cr);
}

// ── Accent ──────────────────────────────────────────────────────────────────
// The island's accent: the album's colour (Themes.AccentColorMode = auto), or
// the custom one. Linux has no system accent to read, so "system" uses the default.

const GdkPixbuf* g_accentFor = nullptr;
Col g_albumAccent = C(0xff375f);
bool g_albumAccentValid = false;

// A vivid colour from the cover: the average of its saturated pixels, lifted.
void ComputeAlbumAccent() {
    g_accentFor = g_art;
    g_albumAccentValid = false;
    if (!g_art || gdk_pixbuf_get_bits_per_sample(g_art) != 8) return;
    const int w = gdk_pixbuf_get_width(g_art), h = gdk_pixbuf_get_height(g_art);
    const int n = gdk_pixbuf_get_n_channels(g_art), stride = gdk_pixbuf_get_rowstride(g_art);
    const guchar* px = gdk_pixbuf_read_pixels(g_art);
    double sr = 0, sg = 0, sb = 0, sw = 0;
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            const guchar* p = px + y * stride + x * n;
            const double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
            const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
            const double sat = mx > 0 ? (mx - mn) / mx : 0;
            const double weight = sat * sat * (0.2 + mx);  // saturated and not too dark
            sr += r * weight; sg += g * weight; sb += b * weight; sw += weight;
        }
    if (sw < 1e-3) return;
    Col c{sr / sw, sg / sw, sb / sw, 1};
    // Lift to a bright, saturated tone so it glows on the dark island.
    const double mx = std::max({c.r, c.g, c.b, 1e-3}), mn = std::min({c.r, c.g, c.b});
    const double k = 1.0 / mx;
    c = {c.r * k, c.g * k, c.b * k, 1};
    const double s = (mx - mn) / mx;
    if (s < 0.45) {  // push the grey out
        const double boost = 0.45 / std::max(s, 0.05);
        const double avg = (c.r + c.g + c.b) / 3;
        c = {std::clamp(avg + (c.r - avg) * boost, 0.0, 1.0), std::clamp(avg + (c.g - avg) * boost, 0.0, 1.0),
             std::clamp(avg + (c.b - avg) * boost, 0.0, 1.0), 1};
    }
    g_albumAccent = c;
    g_albumAccentValid = true;
}

Col Accent() {
    if (g_s.accentMode == 2) return C(g_s.customAccent);
    if (g_s.accentMode == 0) {
        if (g_art != g_accentFor) ComputeAlbumAccent();
        if (g_albumAccentValid && g_track.active) return g_albumAccent;
    }
    return C(0xff375f);
}

Col PageColour(Page page) {
    if (page == Page::Agent) return C(st::AgentBrand(g_agent.agent));
    return Accent();
}

// ── Frame state ─────────────────────────────────────────────────────────────

int g_tabIdx = 0;
std::vector<double> g_tabW;  // eased tab widths, one per tab
bool g_offscreen = false;    // drawing a page picture: no easing, no hover
bool g_wantAgentPage = false;
bool g_wantShelfPage = false;  // files were just dropped on the island
double g_scrollAcc = 0, g_scrollAt = 0;
std::vector<std::pair<std::wstring, std::wstring>> g_links;

// Plan card scroll, question answers.
double g_planScroll = 0, g_planScrollMax = 0;
uint64_t g_planScrollFor = 0;
st::Answers g_answers;

// ── Typing on the island ────────────────────────────────────────────────────

enum ComposeKind { ComposeNone, ComposePlan, ComposeReply, ComposeOther };
struct Compose {
    ComposeKind kind = ComposeNone;
    uint64_t id = 0;    // the request (plan, question) or the reply id
    int question = -1;  // ComposeOther: which question
    std::string label;  // "What should change in the plan?"
    std::string text;   // UTF-8
    double since = 0;
};
Compose g_compose;
GtkIMContext* g_im = nullptr;
bool g_seatGrabbed = false;

void OnImCommit(GtkIMContext*, const gchar* str, gpointer) {
    g_compose.text += str;
    Redraw();
}

// Keyboard to the island while typing, and back to the windows below after.
void KeyboardFocus(bool on) {
    GtkWindow* win = GTK_WINDOW(g_window);
    if (g_layerShell) {
        gtk_layer_set_keyboard_mode(win, on ? GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE : GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    } else {
        // An X11 dock doesn't get focus by clicking it: grab the keyboard while typing.
        gtk_window_set_accept_focus(win, on);
        GdkWindow* gw = gtk_widget_get_window(g_window);
        GdkSeat* seat = gdk_display_get_default_seat(gtk_widget_get_display(g_window));
        if (on && gw && seat) {
            gdk_window_focus(gw, GDK_CURRENT_TIME);
            g_seatGrabbed = gdk_seat_grab(seat, gw, GDK_SEAT_CAPABILITY_KEYBOARD, FALSE, nullptr, nullptr, nullptr, nullptr) ==
                            GDK_GRAB_SUCCESS;
        } else if (!on && g_seatGrabbed && seat) {
            gdk_seat_ungrab(seat);
            g_seatGrabbed = false;
        }
    }
    if (!g_im) {
        g_im = gtk_im_multicontext_new();
        g_signal_connect(g_im, "commit", G_CALLBACK(OnImCommit), nullptr);
    }
    gtk_im_context_set_client_window(g_im, gtk_widget_get_window(g_window));
    if (on) gtk_im_context_focus_in(g_im);
    else gtk_im_context_focus_out(g_im);
}

void StartCompose(ComposeKind kind, uint64_t id, int question, const std::string& label) {
    g_compose = Compose{kind, id, question, label, "", Now()};
    KeyboardFocus(true);
}

void EndCompose() {
    if (g_compose.kind == ComposeNone) return;
    g_compose = Compose{};
    KeyboardFocus(false);
}

void SendCompose() {
    std::string t = g_compose.text;
    while (!t.empty() && isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    while (!t.empty() && isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    if (t.empty()) return;
    const std::wstring text = bb::plat::Wide(t);
    const Compose c = g_compose;
    if (c.kind == ComposePlan) {
        bb::agents::DecideRaw(c.id, st::PlanChangeReply(text));
    } else if (c.kind == ComposeReply) {
        bb::agents::SendReply(c.id, text);
    } else if (c.kind == ComposeOther && g_answers.requestId == c.id && c.question >= 0 &&
               c.question < static_cast<int>(g_answers.other.size())) {
        g_answers.other[c.question] = text;
        g_answers.picks[c.question].clear();
    }
    EndCompose();
}

// The compose target went away (answered in the terminal, a new request).
void CheckCompose() {
    const Compose& c = g_compose;
    if (c.kind == ComposeNone) return;
    const bool alive = (c.kind == ComposeReply && g_agent.replyId == c.id) ||
                       ((c.kind == ComposePlan || c.kind == ComposeOther) && g_agent.requestId == c.id &&
                        (g_agent.phase == AgentPhase::Approval || g_agent.phase == AgentPhase::Question));
    if (!alive) EndCompose();
}

void Button(cairo_t* cr, const Rect& r, const std::string& label, bool primary, Action action, int index = -1, bool enabled = true) {
    const bool hovered = enabled && NextHitHovered();
    const Col fill = primary ? W(enabled ? (hovered ? 1.0 : 0.92) : 0.25) : W(hovered ? 0.18 : 0.11);
    Fill(cr, r.x, r.y, r.w, r.h, std::min(10.0, r.h / 2), fill);
    SText(cr, label, r.x + 4, r.y, r.w - 8, r.h, FLabel, primary ? C(0x141416) : (enabled ? W(1) : C(0x8e8e93)), 1);
    if (enabled) g_hits.push_back({r, action, index});
}

// The text field with Cancel and Send, in `bar` (about 30 high).
void PaintCompose(cairo_t* cr, const Rect& bar) {
    const double bw = 64, gap = 8;
    const Rect field{bar.x, bar.y, bar.w - 2 * (bw + gap), bar.h};
    Fill(cr, field.x, field.y, field.w, field.h, 10, W(0.10));
    RoundRect(cr, field.x + 0.5, field.y + 0.5, field.w - 1, field.h - 1, 9.5);
    Source(cr, WithA(Accent(), 0.8));
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    cairo_save(cr);
    cairo_rectangle(cr, field.x + 8, field.y, field.w - 16, field.h);
    cairo_clip(cr);
    // Long text scrolls left so the caret stays in view.
    const double tw = Measure(cr, g_compose.text, FInput);
    const double room = field.w - 22;
    const double x0 = field.x + 10 - std::max(0.0, tw - room);
    if (g_compose.text.empty()) {
        SText(cr, g_compose.label, field.x + 10, field.y, field.w - 20, field.h, FInput, W(0.4));
    } else {
        PangoLayout* layout = Layout(cr, g_compose.text, FInput);
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        Source(cr, W(1));
        cairo_move_to(cr, x0, field.y + (field.h - ph) / 2);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
    if (std::fmod(Now() - g_compose.since, 1.0) < 0.6) {
        const double cx = g_compose.text.empty() ? field.x + 10 : x0 + tw + 1;
        cairo_rectangle(cr, cx, field.y + 7, 1.5, field.h - 14);
        Source(cr, W(0.9));
        cairo_fill(cr);
    }
    cairo_restore(cr);
    Button(cr, {bar.x + bar.w - 2 * bw - gap, bar.y, bw, bar.h}, "Cancel", false, Action::ComposeCancel);
    Button(cr, {bar.x + bar.w - bw, bar.y, bw, bar.h}, "Send", true, Action::ComposeSend);
}

// ── Header and tab bar ──────────────────────────────────────────────────────

int g_context = 1;
std::vector<int> g_tabs;
Page g_page = Page::Home;

void UpdateTabs() {
    g_context = g_track.active && g_mediaOn ? 0 : 1;
    g_tabs = st::TabIds(g_context, Switches());
    if (g_wantAgentPage || g_wantShelfPage) {
        const Page want = g_wantShelfPage ? Page::Tray : Page::Agent;
        for (size_t i = 0; i < g_tabs.size(); ++i)
            if (st::PageFor(g_context, g_tabs[i]) == want) g_tabIdx = static_cast<int>(i);
        g_wantAgentPage = g_wantShelfPage = false;
    }
    g_tabIdx = std::clamp(g_tabIdx, 0, std::max(0, static_cast<int>(g_tabs.size()) - 1));
    g_page = g_tabs.empty() ? Page::Home : st::PageFor(g_context, g_tabs[static_cast<size_t>(g_tabIdx)]);
}

int TabIndexOf(Page page) {
    for (size_t i = 0; i < g_tabs.size(); ++i)
        if (st::PageFor(g_context, g_tabs[i]) == page) return static_cast<int>(i);
    return -1;
}

// Whose usage the header ring shows: Claude Code's 5-hour window, from its
// statusLine, while the reading is younger than the window it describes.
bool PickUsage(float* used, const char** tag = nullptr) {
    const bool claude = g_s.usageLimits && g_s.showAgent[AgentClaude] && g_agent.usage5h >= 0 &&
                        Now() - g_agent.usageAt < 5 * 3600.0;
    const AgSnapshot ag = AgQuota();
    const int main = MainAgentKind();
    const bool auto_ = g_s.mainAgent < 0;
    if (claude && (main == AgentClaude || (auto_ && !(main == AgentAntigravity && ag.valid)))) {
        *used = bb::limits::UsedNow(g_agent.usage5h, g_agent.usage5hResets, static_cast<long long>(time(nullptr)));
        if (tag) *tag = "5h";
        return true;
    }
    if (ag.valid && (main == AgentAntigravity || auto_)) {
        *used = AgUsedNow(ag);
        if (tag) *tag = "AG";
        return true;
    }
    return false;
}

// Mascot and page name on the left (Weather adds its unit switch and Change
// City); on the right, close, gear, eyedropper, battery, agent usage, then the
// date and time, which give way (date first) when the left side needs the room.
// The same layout as Windows (studio_draw.inc's EndStudioFrame).
// Work mode (work_linux.cpp): each state's colour, the card's and the page's lines.
Col WorkColour(bb::work::Phase p) {
    namespace wk = bb::work;
    switch (p) {
        case wk::Phase::Working: return Accent();
        case wk::Phase::Overtime: return C(wk::KindColour(wk::Kind::Overtime));
        case wk::Phase::Away: return C(wk::KindColour(wk::Kind::Away));
        case wk::Phase::Asleep: return C(wk::KindColour(wk::Kind::Asleep));
        case wk::Phase::Lunch: return C(wk::KindColour(wk::Kind::Lunch));
        case wk::Phase::Break: return C(wk::KindColour(wk::Kind::Break));
        case wk::Phase::Done: return C(0x30d158);
        default: return C(0x8e8e93);
    }
}
void TileWork(cairo_t* cr, const Rect& r);
// A page's name in the tab bar; Work is Study in Study mode.
std::wstring TabName(Page p) { return p == Page::Work && bb::lwork::Now().study ? std::wstring(L"Study") : std::wstring(st::TabLabel(p)); }
void PaintWork(cairo_t* cr, const Rect& pg);

void PaintHeader(cairo_t* cr, const Rect& isl) {
    const double hy = isl.y + 17;
    const Col grey = C(0x8e8e93);

    // ── Left: mascot, then the page name ──
    double fx = isl.x + 18;
    if (g_mascotOn && g_alpha > 0.3) {
        DrawMascot(cr, fx + 9, hy, 18);
        fx += 26;
    }
    std::wstring first, second;
    st::PageName(g_page, g_agent.agent, &first, &second);
    bool subtitled = true;
    if (g_page == Page::Hardware) { first = L"System"; second = L"Power \x00B7 Network \x00B7 Audio"; }
    else if (g_page == Page::Weather) {
        first = L"Weather";
        const std::string city = lsrc::CurrentWeather().city;
        second = city.empty() ? L"" : bb::plat::Wide(city);
    } else if (g_page == Page::ScreenTime) { first = L"Screen Time"; second.clear(); }
    else if (g_page == Page::Work && bb::lwork::Now().study) { first = L"Study Time"; second.clear(); }
    else subtitled = false;
    const double w1 = Measure(cr, U(first), FTitle);
    SText(cr, first, fx, hy - 10, w1 + 2, 20, FTitle, F(W(1)));
    fx += w1;
    if (!second.empty()) {
        if (subtitled) {
            const double barX = fx + 8;
            cairo_rectangle(cr, barX - 0.6, hy - 6.5, 1.2, 13);
            Source(cr, F(C(0x3a3a3c)));
            cairo_fill(cr);
            const double w2 = Measure(cr, U(second), FBody);
            SText(cr, second, barX + 9, hy - 9, w2 + 2, 18, FBody, F(grey));
            fx = barX + 9 + w2;
        } else {
            const double w2 = Measure(cr, U(second), FTitle);
            SText(cr, second, fx, hy - 10, w2 + 2, 20, FTitle, F(PageColour(g_page)));
            fx += w2;
        }
    }
    // Weather's own controls follow its name: a °C / °F capsule and Change City.
    if (g_page == Page::Weather) {
        const bool f = lsrc::CurrentWeather().fahrenheit;
        const Rect tog{fx + 12, hy - 11, 56, 22};
        Fill(cr, tog.x, tog.y, tog.w, tog.h, 11, F(C(0x242426)));
        const Rect cHalf{tog.x + 2, tog.y, 25, tog.h}, fHalf{tog.x + 29, tog.y, tog.w - 31, tog.h};
        const Rect on = f ? fHalf : cHalf;
        Fill(cr, on.x, on.y + 2, on.w, on.h - 4, 9, F(C(0x3a3a3c)));
        SText(cr, "\xC2\xB0" "C", cHalf.x, cHalf.y, cHalf.w, cHalf.h, FSmall, F(f ? grey : W(1)), 1);
        SText(cr, "\xC2\xB0" "F", fHalf.x, fHalf.y, fHalf.w, fHalf.h, FSmall, F(f ? W(1) : grey), 1);
        g_hits.push_back({tog, Action::WeatherUnit});
        const Rect city{tog.x + tog.w + 6, hy - 11, 90, 22};
        const bool hover = NextHitHovered();
        Fill(cr, city.x, city.y, city.w, city.h, 11, F(hover ? C(0x3a3a3c) : C(0x242426)));
        SText(cr, "Change City", city.x, city.y, city.w, city.h, FSmall, F(W(1)), 1);
        g_hits.push_back({city, Action::WeatherCity});
        fx = city.x + city.w;
    }
    const double leftEnd = fx + 14;

    // ── Right: close, gear, eyedropper, battery, usage, date and time ──
    const double closeX = isl.x + isl.w - 22;
    {
        const bool hover = NextHitHovered();
        if (hover) Dot(cr, closeX, hy, 11, F(C(0x3a3a3c)));
        Source(cr, F(hover ? W(1) : grey));
        cairo_set_line_width(cr, 1.4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, closeX - 4.5, hy - 4.5); cairo_line_to(cr, closeX + 4.5, hy + 4.5);
        cairo_move_to(cr, closeX + 4.5, hy - 4.5); cairo_line_to(cr, closeX - 4.5, hy + 4.5);
        cairo_stroke(cr);
        g_hits.push_back({{closeX - 12, hy - 12, 24, 24}, Action::Close});
    }
    const double gearX = closeX - 24;
    {
        const bool hover = NextHitHovered();
        if (hover) Dot(cr, gearX, hy, 12, F(C(0x3a3a3c)));
        Gear(cr, gearX, hy, hover ? 15.4 : 14, F(hover ? W(1) : grey));
        g_hits.push_back({{gearX - 12, hy - 12, 24, 24}, Action::Settings});
    }
    const double dropX = gearX - 24;
    {
        const bool hover = NextHitHovered();
        if (hover) Dot(cr, dropX, hy, 12, F(C(0x3a3a3c)));
        cairo_save(cr);
        st::Eyedropper(PenFor(cr), Pt(dropX, hy), hover ? 15.4f : 14.0f, D2(F(hover ? W(1) : grey)));
        cairo_restore(cr);
        g_hits.push_back({{dropX - 12, hy - 12, 24, 24}, Action::ColorPick});
    }
    double rightEdge = dropX - 16;
    const lsrc::Battery& bat = lsrc::BatteryState();
    if (bat.has) {  // the battery, like the status bar: 100% and a little cell
        const std::string pct = std::to_string(bat.percent) + "%";
        const double bw = Measure(cr, pct, FSmall);
        const double bx = rightEdge - 18;
        RoundRect(cr, bx, hy - 4.5, 14, 9, 2);
        Source(cr, F(W(1)));
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);
        Fill(cr, bx + 14.5, hy - 2, 1.5, 4, 0.8, F(W(1)));
        Fill(cr, bx + 1, hy - 3.5, 12 * std::clamp(bat.percent / 100.0, 0.0, 1.0), 7, 1, F(bat.percent <= 20 ? C(0xff453a) : C(0x30d158)));
        SText(cr, pct, rightEdge - 22 - bw, hy - 8, bw + 2, 16, FSmall, F(W(1)), 2);
        rightEdge -= 26 + bw;
    }
    float used = 0;
    const char* tag = "5h";
    if (AgentsShown() && PickUsage(&used, &tag)) {
        char pct[16];
        snprintf(pct, sizeof pct, "%s %d%%", tag, static_cast<int>(used + 0.5f));
        const double pw = Measure(cr, pct, FLabel);
        const double rx = rightEdge - pw - 14;
        Ring(cr, rx, hy, 6, 2.4, used / 100.0, C(st::UsageColour(used)));
        SText(cr, pct, rx + 10, hy - 8, pw + 2, 16, FLabel, F(grey));
        g_hits.push_back({{rx - 9, hy - 10, pw + 21, 20}, Action::AgentPage});
        rightEdge = rx - 9;
    }
    // Work mode without agents on show: the day's progress in the limits' place.
    if (!AgentsShown() && bb::lwork::On()) {
        const bb::lwork::Snapshot w = bb::lwork::Now();
        if (w.goal > 0) {
            const double u = std::min(1.0, static_cast<double>(w.today.Worked()) / w.goal);
            char pct[24];
            snprintf(pct, sizeof pct, w.study ? "Study %d%%" : "Work %d%%", static_cast<int>(u * 100 + 0.5));
            const double pw = Measure(cr, pct, FLabel);
            const double rx = rightEdge - pw - 14;
            Ring(cr, rx, hy, 6, 2.4, u, WorkColour(w.phase));
            SText(cr, pct, rx + 10, hy - 8, pw + 2, 16, FLabel, F(grey));
            g_hits.push_back({{rx - 9, hy - 10, pw + 21, 20}, Action::WorkPage});
            rightEdge = rx - 9;
        }
    }
    const std::string date = bb::lfeat::FormatDate(time(nullptr));  // Modules.DateFormat
    const std::string clock = ClockText();
    const double timeW = Measure(cr, clock, FTitle), dateW = Measure(cr, date, FBody);
    const double timeRight = rightEdge - 12, timeLeft = timeRight - timeW;
    if (timeLeft >= leftEnd) {
        SText(cr, clock, timeLeft, hy - 10, timeW + 2, 20, FTitle, F(W(1)));
        const double dateLeft = timeLeft - 6 - dateW;
        if (dateLeft >= leftEnd) SText(cr, date, dateLeft, hy - 9, dateW + 2, 18, FBody, F(grey));
    }
}

void PaintTabBar(cairo_t* cr, const Rect& isl) {
    const double fy = isl.y + isl.h - 19, cx = isl.x + isl.w / 2;
    const int n = static_cast<int>(g_tabs.size());
    if (static_cast<int>(g_tabW.size()) != n) g_tabW.assign(static_cast<size_t>(n), 30.0);
    double total = 0;
    for (int i = 0; i < n; ++i) {
        const Page p = st::PageFor(g_context, g_tabs[static_cast<size_t>(i)]);
        const double want = i == g_tabIdx ? 30 + 10 + Measure(cr, U(TabName(p)), FLabel) : 30;
        double& cur = g_tabW[static_cast<size_t>(i)];
        cur += (want - cur) * 0.25;
        if (std::fabs(want - cur) < 0.3 || g_offscreen) cur = want;
        total += cur + (i ? 2 : 0);
    }
    double tx = cx - total / 2;
    for (int i = 0; i < n; ++i) {
        const Page p = st::PageFor(g_context, g_tabs[static_cast<size_t>(i)]);
        const double w = g_tabW[static_cast<size_t>(i)];
        const bool hovered = NextHitHovered();
        if (i == g_tabIdx) {
            const bool white = p == Page::Home || p == Page::Hardware;
            const Col tint = PageColour(p);
            Fill(cr, tx, fy - 13, w, 26, 13, F(white ? C(0x2c2c2e) : WithA(tint, 0.22)));
            const Col ink = F(white ? W(1) : tint);
            Icon(cr, p, tx + 15, fy, 15, ink);
            if (w > 44) SText(cr, TabName(p), tx + 27, fy - 9, w - 31, 18, FLabel, ink);
        } else {
            if (hovered) Dot(cr, tx + w / 2, fy, 13, F(C(0x3a3a3c)));
            Icon(cr, p, tx + w / 2, fy - (hovered ? 1.5 : 0), hovered ? 16.8 : 15, F(hovered ? W(1) : C(0x8e8e93)));
        }
        g_hits.push_back({{tx, fy - 15, w, 30}, Action::Tab, i});
        tx += w + 2;
    }
}

void TileWeather(cairo_t* cr, const Rect& r);  // with the pages, below
void TileFocus(cairo_t* cr, const Rect& r);
std::vector<const bb::todos::Item*> TodayTodos();
std::string HourMinute(long long unix);

// ── Home ────────────────────────────────────────────────────────────────────

void Tile(cairo_t* cr, const Rect& r, Col glow, bool live) {
    Fill(cr, r.x, r.y, r.w, r.h, 22, W(0.055));
    const double a = live ? 0.82 + 0.30 * (0.5 + 0.5 * std::sin(Now() * 2 * kPi / 2.6)) : 0.55;
    Glow(cr, r.x, r.y, r.w, r.h, 22, glow, a);
    RoundRect(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1, 21.5);
    Source(cr, W(0.10));
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

// A round white button with a glyph (0 play, 1 pause) in `ink`.
void RoundButton(cairo_t* cr, double cx, double cy, double r, int glyph, Col ink, bool hover) {
    if (hover) { r *= 1.08; Dot(cr, cx, cy, r + 4, W(0.18)); }
    Dot(cr, cx, cy, r, W(hover ? 1.0 : 0.92));
    const double g = r * 0.42;
    Source(cr, ink);
    if (glyph == 0) {
        cairo_move_to(cr, cx - g * 0.55, cy - g * 0.75);
        cairo_line_to(cr, cx + g * 0.8, cy);
        cairo_line_to(cr, cx - g * 0.55, cy + g * 0.75);
        cairo_close_path(cr);
        cairo_fill(cr);
    } else {
        Fill(cr, cx - g * 0.6, cy - g * 0.7, g * 0.45, g * 1.4, 1.2, ink);
        Fill(cr, cx + g * 0.15, cy - g * 0.7, g * 0.45, g * 1.4, 1.2, ink);
    }
}

void TileTexts(cairo_t* cr, const Rect& r, const std::string& label, Col labelCol, const std::string& big, const std::string& line,
               double rightPad = 10) {
    SText(cr, label, r.x + 14, r.y + 14, r.w - 66, 18, FLabel, labelCol);
    SText(cr, big, r.x + 14, r.y + r.h - 58, r.w - 14 - rightPad, 30, big.size() > 6 ? FSong : FBig, W(1));
    SText(cr, line, r.x + 14, r.y + r.h - 26, r.w - 26, 16, FBody, W(0.8));
}

void TileAgent(cairo_t* cr, const Rect& r) {
    const AgentView& a = g_agent;
    const bool busy = a.phase == AgentPhase::Working || a.phase == AgentPhase::Thinking;
    const bool waiting = a.phase == AgentPhase::Approval || a.phase == AgentPhase::Question;
    const std::wstring brandName = a.agent.empty() ? std::wstring(AgentKindName(MainAgentKind())) : a.agent;
    const Col col = a.phase == AgentPhase::None ? C(st::AgentBrand(brandName)) : C(st::AgentStatus(a.phase));
    const Col brand = C(st::AgentBrand(a.agent));
    Tile(cr, r, busy ? brand : col, busy || waiting);
    const std::string name = U(a.phase != AgentPhase::None && !a.agent.empty() ? a.agent : AgentKindName(MainAgentKind()));
    const double rx = r.x + r.w - 30, ry = r.y + 30;
    std::string big, line;
    if (busy) {
        Ring(cr, rx, ry, 15, 4.5, (std::max(a.stage, 0) + 0.5) / 4.0, brand);
        Icon(cr, Page::Agent, rx, ry, 13, brand);
        big = Clock(a.turnStart > 0 ? Now() - a.turnStart : Now() - a.phaseSince);
        line = StageWord(a.phase == AgentPhase::Thinking && a.stage < 0 ? -1 : a.stage);
        if (!a.project.empty()) line += kDot + U(a.project);
    } else if (waiting) {
        Ring(cr, rx, ry, 15, 4.5, 1, col);
        Icon(cr, Page::Agent, rx, ry, 13, col);
        big = a.phase == AgentPhase::Question ? "Your turn" : "May I?";
        line = U(a.tool.empty() ? a.detail : a.tool);
    } else if (a.phase == AgentPhase::Finished) {
        Ring(cr, rx, ry, 15, 4.5, 1, col);
        big = "Done";
        line = U(a.prompt.empty() ? a.step : a.prompt);
    } else if (a.phase == AgentPhase::Error) {
        Ring(cr, rx, ry, 15, 4.5, 1, col);
        big = "Stopped";
        line = U(a.step);
    } else {
        float used = 0;
        if (PickUsage(&used)) {
            Ring(cr, rx, ry, 15, 4.5, used / 100.0, C(st::UsageColour(used)));
            SText(cr, std::to_string(static_cast<int>(used + 0.5f)), rx - 12, ry - 8, 24, 16, FSmall, W(1), 1);
        } else {
            Ring(cr, rx, ry, 15, 4.5, 0, W(1));
            Icon(cr, Page::Agent, rx, ry, 13, W(0.5));
        }
        big = std::to_string(a.jobsToday);
        line = a.jobsToday == 1 ? "task today" : "tasks today";
    }
    TileTexts(cr, r, name, W(0.65), big, line);
    g_hits.push_back({r, Action::AgentPage});
}

void TileMusic(cairo_t* cr, const Rect& r) {
    const bool has = g_track.active && g_mediaOn && !g_track.title.empty();
    Tile(cr, r, Accent(), g_track.playing);
    SText(cr, "Music", r.x + 14, r.y + 14, r.w - 74, 18, FLabel, W(0.65));
    if (has) {
        if (g_art) Art(cr, r.x + r.w - 54, r.y + 12, 42, 1.0);
        else Icon(cr, Page::Music, r.x + r.w - 30, r.y + 30, 18, W(0.5));
        SText(cr, g_track.title, r.x + 14, r.y + r.h - 62, r.w - 60, 20, FSong, W(1));
        SText(cr, g_track.artist.empty() ? g_track.player : g_track.artist, r.x + 14, r.y + r.h - 40, r.w - 60, 16, FBody, W(0.6));
        const double bx = r.x + r.w - 27, by = r.y + r.h - 27;
        RoundButton(cr, bx, by, 15, g_track.playing ? 1 : 0, C(0x000000), NextHitHovered());
        g_hits.push_back({{bx - 17, by - 17, 34, 34}, Action::PlayPause});
    } else {
        Icon(cr, Page::Music, r.x + r.w - 30, r.y + 30, 18, W(0.5));
        TileTexts(cr, r, "", W(0), "Quiet", "Nothing playing");
    }
    g_hits.push_back({r, Action::OpenMusic});
}

void TileCalendar(cairo_t* cr, const Rect& r) {
    const Col red = C(0xff453a);
    Tile(cr, r, red, false);
    const time_t now = time(nullptr);
    tm lt{};
    localtime_r(&now, &lt);
    char day[32], big[32];
    strftime(day, sizeof day, "%A", &lt);
    strftime(big, sizeof big, "%-d %b", &lt);
    Icon(cr, Page::Calendar, r.x + r.w - 30, r.y + 30, 20, red);
    // The next thing on today's list, like the next meeting on Windows.
    std::string line = "Nothing planned today";
    if (const auto today = TodayTodos(); !today.empty())
        line = (today[0]->due > 0 ? HourMinute(static_cast<long long>(today[0]->due)) + " " : "") + U(today[0]->text);
    TileTexts(cr, r, day, red, big, line);
    if (const int i = TabIndexOf(Page::Calendar); i >= 0) g_hits.push_back({r, Action::Tab, i});
}

void TileBattery(cairo_t* cr, const Rect& r) {
    const lsrc::Battery& b = lsrc::BatteryState();
    const Col col = !b.has ? C(0x8e8e93) : b.charging ? C(0x30d158) : b.percent <= 20 ? C(0xff453a) : C(0x30d158);
    Tile(cr, r, col, b.has && b.charging && b.percent < 100);
    Ring(cr, r.x + r.w - 30, r.y + 30, 15, 4.5, b.has ? b.percent / 100.0 : 0, col);
    TileTexts(cr, r, "Battery", W(0.65), b.has ? std::to_string(b.percent) + "%" : "Plugged in",
              !b.has ? "No battery" : b.charging ? "Charging" : "On battery");
}

void TileSystem(cairo_t* cr, const Rect& r) {
    const lsrc::Load& load = lsrc::SystemLoad();
    Tile(cr, r, C(0x30d158), false);  // Home cards keep their own colours
    Ring(cr, r.x + r.w - 30, r.y + 30, 15, 4.5, load.mem / 100.0, C(0x64d2ff));
    TileTexts(cr, r, "System", W(0.65), std::to_string(load.cpu) + "%", "CPU" + kDot + "RAM " + std::to_string(load.mem) + "%");
    if (const int i = TabIndexOf(Page::Hardware); i >= 0) g_hits.push_back({r, Action::Tab, i});
}

void PaintHome(cairo_t* cr, const Rect& pg) {
    const double gap = 10, tileW = (pg.w - 2 * gap) / 3, tileH = std::min(tileW, pg.h - 34), top = pg.y + 2;
    const bool agents = AgentsShown();
    for (int slot = 0; slot < 3; ++slot) {
        const Rect r{pg.x + slot * (tileW + gap), top, tileW, tileH};
        switch (st::HomeTileFor(g_s.homeTile[slot], slot, agents, bb::lwork::On())) {
            case st::HomeTile::Work: TileWork(cr, r); break;
            case st::HomeTile::Agent: TileAgent(cr, r); break;
            case st::HomeTile::Music: TileMusic(cr, r); break;
            case st::HomeTile::Weather: TileWeather(cr, r); break;
            case st::HomeTile::Calendar: TileCalendar(cr, r); break;
            case st::HomeTile::Battery: TileBattery(cr, r); break;
            case st::HomeTile::Focus: TileFocus(cr, r); break;
            case st::HomeTile::System: TileSystem(cr, r); break;
        }
    }
    // Your links, as chips under the tiles; else one quiet line about the battery.
    const double cy = top + tileH + 6;
    if (!g_links.empty()) {
        std::vector<double> widths;
        double total = 0;
        for (const auto& [name, target] : g_links) {
            widths.push_back(Measure(cr, U(name), FLabel) + 40);
            total += widths.back() + 8;
        }
        double x = pg.x + (pg.w - (total - 8)) / 2;
        for (size_t i = 0; i < g_links.size(); ++i) {
            const Rect chip{x, cy, widths[i], 24};
            Fill(cr, chip.x, chip.y, chip.w, chip.h, 12, W(NextHitHovered() ? 0.18 : 0.09));
            // A coloured dot with the first letter, its hue from the address.
            unsigned hash = 0;
            for (wchar_t ch : g_links[i].second) hash = hash * 31 + static_cast<unsigned>(ch);
            const double hue = (hash % 360) / 360.0;
            auto channel = [&](double n) {
                const double k = std::fmod(n + hue * 12, 12.0);
                return 0.52 - 0.62 * 0.48 * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
            };
            Dot(cr, chip.x + 14, cy + 12, 8, {channel(0), channel(8), channel(4), 1});
            const std::wstring& name = g_links[i].first;
            SText(cr, std::wstring(1, static_cast<wchar_t>(towupper(name.empty() ? L'?' : name[0]))), chip.x + 6, cy, 16, 24, FSmall, W(1), 1);
            SText(cr, name, chip.x + 28, cy, chip.w - 36, 24, FLabel, W(1));
            g_hits.push_back({chip, Action::OpenLink, static_cast<int>(i)});
            x += widths[i] + 8;
        }
        return;
    }
    // One quiet line under the tiles: the weather and the battery.
    std::string info;
    const lsrc::Weather& wx = lsrc::CurrentWeather();
    if (g_s.weather && wx.has) {
        char b[96];
        snprintf(b, sizeof b, "%.0f\xC2\xB0  %s", wx.temp, lsrc::WeatherWords(wx.code));
        info = b;
        if (!wx.city.empty()) info += kDot + wx.city;
    }
    const lsrc::Battery& bat = lsrc::BatteryState();
    if (bat.has) {
        if (!info.empty()) info += "   ";
        info += std::string(bat.charging ? "\xE2\x9A\xA1 " : "Battery ") + std::to_string(bat.percent) + "%";
    }
    SText(cr, info, pg.x + 4, cy, pg.w - 8, 22, FBody, W(0.55), 1);
}

// ── Music ───────────────────────────────────────────────────────────────────

void Glyph(cairo_t* cr, Action a, double cx, double cy, double s, Col c) {
    Source(cr, c);
    const double dir = a == Action::Previous ? -1 : 1;
    const auto tri = [&](double x0) {
        cairo_move_to(cr, x0 - dir * s * 0.35, cy - s * 0.5);
        cairo_line_to(cr, x0 + dir * s * 0.45, cy);
        cairo_line_to(cr, x0 - dir * s * 0.35, cy + s * 0.5);
        cairo_close_path(cr);
    };
    tri(cx - dir * s * 0.3);
    tri(cx + dir * s * 0.45);
    cairo_fill(cr);
    cairo_rectangle(cr, dir > 0 ? cx + s * 0.85 : cx - s * 0.95, cy - s * 0.5, s * 0.12, s);
    cairo_fill(cr);
}

void PaintMusic(cairo_t* cr, const Rect& pg) {
    const double art = pg.h - 30;
    const double ax = pg.x + 4, ay = pg.y + 6;
    if (g_art) {
        Art(cr, ax, ay, art, 1.0);
    } else {
        Fill(cr, ax, ay, art, art, art * 0.22, W(0.08));
        DrawMascot(cr, ax + art / 2, ay + art / 2, art * 0.8);
    }
    const double tx = ax + art + 18, tw = pg.x + pg.w - tx;
    // The player's name. With more than one running it says so, and a click shows the next one
    // (the right-click menu has the list and "Automatic").
    std::string sourceLabel = g_track.player;
    const size_t running = bb::mpris::Sources().size();
    if (running > 1) sourceLabel += "  \xC2\xB7  " + std::to_string(running) + " players";
    if (!bb::mpris::Picked().empty()) sourceLabel += "  \xC2\xB7  picked";
    SText(cr, sourceLabel, tx, ay, tw, 16, FLabel, Accent());
    if (running > 1) g_hits.push_back({{tx, ay - 2, std::min(tw, 260.0), 20}, Action::MediaSource});
    SText(cr, g_track.title.empty() ? "Nothing playing" : g_track.title, tx, ay + 22, tw, 24, FSong, W(1));
    SText(cr, g_track.artist, tx, ay + 46, tw, 18, FBody, W(0.65));
    if (!g_track.album.empty()) SText(cr, g_track.album, tx, ay + 64, tw, 16, FSmall, W(0.45));
    if (g_track.length > 0) {
        // Where the song is, in Appearance.ProgressStyle, with the times beside it.
        const double pos = std::clamp(bb::mpris::Position(), 0.0, g_track.length);
        const Col acc = Accent();
        const double rgb[3] = {acc.r, acc.g, acc.b};
        const double barW = tw - 90;
        SText(cr, Clock(pos), tx, ay + 76, 40, 16, FSmall, W(0.45));
        bb::lfeat::Progress(cr, tx + 42, ay + 84, barW, pos / g_track.length, rgb, g_track.playing, Now());
        g_hits.push_back({{tx + 42, ay + 74, barW, 20}, Action::Seek});  // click to jump there
        SText(cr, Clock(g_track.length), tx + 48 + barW, ay + 76, 40, 16, FSmall, W(0.45));
    }
    // Controls: previous, play / pause, next.
    const double cy = ay + art - 22;
    auto hoverDot = [&](double x, double r) { if (NextHitHovered()) Dot(cr, x, cy, r, W(0.14)); };
    hoverDot(tx + 16, 18);
    Glyph(cr, Action::Previous, tx + 16, cy, 11, W(0.92));
    g_hits.push_back({{tx - 2, cy - 18, 36, 36}, Action::Previous});
    RoundButton(cr, tx + 64, cy, 20, g_track.playing ? 1 : 0, C(0x000000), NextHitHovered());
    g_hits.push_back({{tx + 42, cy - 22, 44, 44}, Action::PlayPause});
    hoverDot(tx + 112, 18);
    Glyph(cr, Action::Next, tx + 112, cy, 11, W(0.92));
    g_hits.push_back({{tx + 94, cy - 18, 36, 36}, Action::Next});
    // The music bars in Appearance.SpectrumStyle (not driven by the audio yet).
    const Col acc = Accent();
    const double rgb[3] = {acc.r, acc.g, acc.b};
    bb::lfeat::Spectrum(cr, pg.x + pg.w - 50, cy, 44, 28, rgb, 0.9, g_track.playing, Now());
}

// ── Agent ───────────────────────────────────────────────────────────────────

std::string g_claudeModel;
double g_claudeModelAt = -100;

std::string ClaudeModelLabel() {
    if (Now() - g_claudeModelAt > 10) {
        g_claudeModelAt = Now();
        g_claudeModel = bb::hooks::ClaudeModel();
    }
    std::string m = g_claudeModel;
    for (auto& c : m) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (m.empty() || m == "default") return "Default";
    if (m.find("opus") != std::string::npos) return "Opus";
    if (m.find("sonnet") != std::string::npos) return "Sonnet";
    if (m.find("haiku") != std::string::npos) return "Haiku";
    return g_claudeModel;
}

void CycleClaudeModel() {
    static const char* kOrder[] = {"", "opus", "sonnet", "haiku"};
    const std::string now = ClaudeModelLabel();
    int i = now == "Opus" ? 1 : now == "Sonnet" ? 2 : now == "Haiku" ? 3 : 0;
    std::wstring report;
    if (bb::hooks::SetClaudeModel(kOrder[(i + 1) % 4], &report)) {
        g_claudeModel = kOrder[(i + 1) % 4];
        g_claudeModelAt = Now();
    }
}

std::string HourMinute(long long unix) {
    const time_t t = static_cast<time_t>(unix);
    tm lt{};
    localtime_r(&t, &lt);
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d", lt.tm_hour, lt.tm_min);
    return b;
}

std::string ResetsIn(long long unix) {
    const long long mins = (unix - static_cast<long long>(time(nullptr))) / 60;
    if (mins <= 0) return "";
    return kDot + "resets in " + (mins >= 60 ? std::to_string(mins / 60) + "h" : std::to_string(mins) + "m");
}

// ── Pages: calendar, weather, system, toggles, to-do, clipboard, note, focus, apps ──

const Col kRed = C(0xff453a), kSky = C(0x64d2ff), kGreen = C(0x30d158), kPurple = C(0xbf5af2), kGrey = C(0x8e8e93);

std::string Temp(double t) {
    char b[16];
    snprintf(b, sizeof b, "%.0f\xC2\xB0", t);
    return b;
}

// A pill button: white when primary.
void Pill(cairo_t* cr, const Rect& b, const std::string& label, bool primary, Action action, int index = -1) {
    const bool hover = NextHitHovered();
    Fill(cr, b.x, b.y, b.w, b.h, b.h / 2, primary ? W(hover ? 1.0 : 0.92) : W(hover ? 0.18 : 0.09));
    SText(cr, label, b.x, b.y, b.w, b.h, FLabel, primary ? C(0x141416) : W(1), 1);
    g_hits.push_back({b, action, index});
}

// Today's open to-dos (timed ones first, in order), for the agenda and Home.
std::vector<const bb::todos::Item*> TodayTodos() {
    const time_t now = time(nullptr);
    tm end{};
    localtime_r(&now, &end);
    end.tm_mday += 1;
    end.tm_hour = end.tm_min = end.tm_sec = 0;
    end.tm_isdst = -1;
    const double dayEnd = static_cast<double>(mktime(&end));
    std::vector<const bb::todos::Item*> out;
    for (const auto& t : lsrc::Todos())
        if (!t.done && (t.due <= 0 || t.due < dayEnd)) out.push_back(&t);
    std::stable_sort(out.begin(), out.end(), [](const auto* a, const auto* b) {
        return (a->due > 0 ? a->due : 1e18) < (b->due > 0 ? b->due : 1e18);
    });
    return out;
}

void PaintCalendar(cairo_t* cr, const Rect& pg) {
    const time_t now = time(nullptr);
    tm lt{};
    localtime_r(&now, &lt);
    // The month: a grid of days, today in red, a dot on days with a to-do.
    const double gridW = 238, cellW = gridW / 7;
    char month[32];
    strftime(month, sizeof month, "%B %Y", &lt);
    SText(cr, month, pg.x, pg.y, gridW, 18, FLabel, Accent());
    // Monday or Sunday first (Modules.FirstDayOfWeek).
    const bool monday = bb::lfeat::Get().mondayFirst;
    static const char* kDays[] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int d = 0; d < 7; ++d)
        SText(cr, kDays[(d + (monday ? 1 : 0)) % 7], pg.x + d * cellW, pg.y + 20, cellW, 14, FSmall, C(0x636366), 1);
    tm first = lt;
    first.tm_mday = 1;
    first.tm_isdst = -1;
    mktime(&first);
    const int lead = monday ? (first.tm_wday + 6) % 7 : first.tm_wday;
    tm next = first;
    next.tm_mon += 1;
    next.tm_isdst = -1;
    mktime(&next);
    tm lastDay = next;
    lastDay.tm_mday -= 1;
    mktime(&lastDay);
    const int days = lastDay.tm_mday;
    bool marked[32] = {};
    for (const auto& t : lsrc::Todos()) {
        if (t.done || t.due <= 0) continue;
        const time_t due = static_cast<time_t>(t.due);
        tm dl{};
        localtime_r(&due, &dl);
        if (dl.tm_year == lt.tm_year && dl.tm_mon == lt.tm_mon) marked[dl.tm_mday] = true;
    }
    const double rowH = std::min(22.0, (pg.h - 38) / ((lead + days + 6) / 7));
    for (int day = 1; day <= days; ++day) {
        const int cell = lead + day - 1;
        const double cx = pg.x + (cell % 7 + 0.5) * cellW, cy = pg.y + 38 + (cell / 7 + 0.5) * rowH;
        const bool today = day == lt.tm_mday;
        if (today) Dot(cr, cx, cy, rowH / 2 - 1, Accent());
        SText(cr, std::to_string(day), cx - cellW / 2, cy - rowH / 2, cellW, rowH, today ? FLabel : FSmall,
              today ? W(1) : ((monday ? cell % 7 >= 5 : cell % 7 == 0 || cell % 7 == 6) ? C(0x8e8e93) : C(0xd1d1d6)), 1);
        if (marked[day] && !today) Dot(cr, cx, cy + rowH / 2 - 2.5, 1.6, Accent());
    }
    // The agenda: today's open to-dos with their times.
    const Rect ag{pg.x + gridW + 16, pg.y, pg.w - gridW - 16, pg.h - 4};
    Fill(cr, ag.x, ag.y, ag.w, ag.h, 12, W(0.05));
    SText(cr, "Today", ag.x + 10, ag.y + 6, ag.w - 20, 16, FLabel, kGrey);
    double y = ag.y + 28;
    const double epoch = static_cast<double>(now);
    bool any = false;
    // Today's meetings from your calendar links first (Modules.CalendarIcsUrl).
    for (const bb::ics::Meeting& m : bb::lcal::Today()) {
        if (y + 30 > ag.y + ag.h - 4) break;
        if (m.endsAt <= epoch) continue;  // already over
        const bool live = m.startsAt <= epoch;
        Fill(cr, ag.x + 10, y + 3, 3, 24, 1.5, live ? C(0x22e05a) : C(0x0a84ff));
        SText(cr, (live ? std::string("Now") + kDot : std::string()) + HourMinute(static_cast<long long>(m.startsAt)), ag.x + 19, y,
              ag.w - 27, 13, FSmall, kGrey);
        SText(cr, U(m.title), ag.x + 19, y + 13, ag.w - 27, 16, FBody, W(1));
        y += 34;
        any = true;
    }
    for (const auto* t : TodayTodos()) {
        if (y + 30 > ag.y + ag.h - 4) break;
        const bool overdue = t->due > 0 && t->due < epoch;
        Fill(cr, ag.x + 10, y + 3, 3, 24, 1.5, overdue ? C(0xff9f0a) : Accent());
        SText(cr, t->due > 0 ? std::string(overdue ? "Overdue" + kDot : "") + HourMinute(static_cast<long long>(t->due)) : "To-do",
              ag.x + 19, y, ag.w - 27, 13, FSmall, kGrey);
        SText(cr, U(t->text), ag.x + 19, y + 13, ag.w - 27, 16, FBody, W(1));
        y += 34;
        any = true;
    }
    if (!any) {
        SText(cr, "Nothing planned", ag.x + 10, y + 4, ag.w - 20, 18, FBody, C(0xaeaeb2));
        SText(cr, "Add to-dos in Settings", ag.x + 10, y + 22, ag.w - 20, 16, FSmall, C(0x636366));
        g_hits.push_back({{ag.x, ag.y, ag.w, ag.h}, Action::TodoOpen});
    }
}

void PaintWeather(cairo_t* cr, const Rect& pg) {
    const lsrc::Weather& w = lsrc::CurrentWeather();
    if (!w.has) {
        Icon(cr, Page::Weather, pg.x + pg.w / 2, pg.y + 56, 34, Accent());
        SText(cr, w.loading ? "Getting the weather\xE2\x80\xA6" : w.error, pg.x, pg.y + 86, pg.w, 22, FBody, W(0.8), 1);
        if (!w.loading) SText(cr, "Settings \xE2\x86\x92 Features \xE2\x86\x92 Page options sets your city.", pg.x, pg.y + 108, pg.w, 18,
                              FSmall, C(0x636366), 1);
        return;
    }
    const double leftW = pg.w * 0.42;
    SText(cr, w.city.empty() ? std::string("Weather") : w.city, pg.x, pg.y, leftW, 18, FLabel, Accent());
    SText(cr, Temp(w.temp), pg.x, pg.y + 22, leftW, 56, FHero, W(1));
    SText(cr, lsrc::WeatherWords(w.code), pg.x, pg.y + 82, leftW, 20, FSong, W(0.9));
    SText(cr, "H " + Temp(w.high) + "   L " + Temp(w.low), pg.x, pg.y + 106, leftW, 18, FBody, kGrey);
    // The next hours, one column each.
    const Rect hr{pg.x + leftW, pg.y + 4, pg.w - leftW, pg.h - 30};
    Fill(cr, hr.x, hr.y, hr.w, hr.h, 14, W(0.05));
    const size_t n = std::max<size_t>(1, w.hours.size());
    const double colW = hr.w / n;
    for (size_t i = 0; i < w.hours.size(); ++i) {
        const auto& h = w.hours[i];
        const double x = hr.x + i * colW;
        char hour[8];
        snprintf(hour, sizeof hour, "%02d", h.hour);
        SText(cr, hour, x, hr.y + 12, colW, 16, FSmall, kGrey, 1);
        Icon(cr, Page::Weather, x + colW / 2, hr.y + 52, 22, h.code <= 2 ? C(0xffd60a) : kSky);
        SText(cr, Temp(h.temp), x, hr.y + 78, colW, 20, FTitle, W(1), 1);
        SText(cr, lsrc::WeatherWords(h.code), x + 2, hr.y + 100, colW - 4, 16, FSmall, kGrey, 1);
    }
}

void Meter(cairo_t* cr, double cx, double cy, double fraction, Col c, const std::string& big, const std::string& label) {
    Ring(cr, cx, cy, 34, 7, fraction, c);
    SText(cr, big, cx - 34, cy - 12, 68, 24, FSong, W(1), 1);
    SText(cr, label, cx - 50, cy + 42, 100, 16, FSmall, kGrey, 1);
}

std::string Rate(double kbs) {
    char b[32];
    if (kbs >= 1024) snprintf(b, sizeof b, "%.1f MB/s", kbs / 1024);
    else snprintf(b, sizeof b, "%.0f KB/s", kbs);
    return b;
}

void PaintSystem(cairo_t* cr, const Rect& pg) {
    const lsrc::Load& l = lsrc::SystemLoad();
    const double cy = pg.y + 52;
    char mem[48];
    snprintf(mem, sizeof mem, "RAM %.1f of %.0f GB", l.memUsedGB, l.memTotalGB);
    Meter(cr, pg.x + pg.w * 0.17, cy, l.cpu / 100.0, Accent(), std::to_string(l.cpu) + "%", "CPU");
    Meter(cr, pg.x + pg.w * 0.50, cy, l.mem / 100.0, Accent(), std::to_string(l.mem) + "%", mem);
    Meter(cr, pg.x + pg.w * 0.83, cy, l.disk / 100.0, C(0xff9f0a), std::to_string(l.disk) + "%", "Disk");
    const double by = pg.y + pg.h - 34;
    Fill(cr, pg.x, by, pg.w, 30, 12, W(0.05));
    const long up = l.uptime;
    char uptime[48];
    if (up >= 86400) snprintf(uptime, sizeof uptime, "Up %ld d %ld h", up / 86400, up / 3600 % 24);
    else snprintf(uptime, sizeof uptime, "Up %ld h %ld min", up / 3600, up / 60 % 60);
    SText(cr, "\xE2\x86\x93 " + Rate(l.rxKBs) + "    \xE2\x86\x91 " + Rate(l.txKBs), pg.x + 12, by, pg.w / 2, 30, FBody, W(0.85));
    SText(cr, uptime, pg.x + pg.w / 2, by, pg.w / 2 - 12, 30, FBody, kGrey, 2);
}

void PaintToggles(cairo_t* cr, const Rect& pg) {
    const lsrc::Radios& r = lsrc::RadioState();
    struct T { const char* name; bool known, on; std::string line; Col c; };
    const T tiles[3] = {
        {"Wi-Fi", r.wifiKnown, r.wifi, !r.wifiKnown ? "Needs NetworkManager" : !r.wifi ? "Off" : r.network.empty() ? "Not connected" : r.network, kSky},
        {"Bluetooth", r.btKnown, r.bt, !r.btKnown ? "No Bluetooth adapter" : r.bt ? "On" : "Off", C(0x0a84ff)},
        {"Do not disturb", r.dndKnown, r.dnd, !r.dndKnown ? "GNOME only for now" : r.dnd ? "Banners hidden" : "Off", kPurple},
    };
    const double gap = 10, tw = (pg.w - 2 * gap) / 3, th = pg.h - 40;
    for (int i = 0; i < 3; ++i) {
        const T& t = tiles[i];
        const Rect b{pg.x + i * (tw + gap), pg.y + 2, tw, th};
        const bool hover = t.known && NextHitHovered();
        Fill(cr, b.x, b.y, b.w, b.h, 20, t.on ? WithA(t.c, hover ? 0.42 : 0.32) : W(hover ? 0.10 : 0.055));
        // A switch in the corner.
        const double sx = b.x + b.w - 50, sy = b.y + 16;
        Fill(cr, sx, sy, 36, 20, 10, t.on ? t.c : W(0.18));
        Dot(cr, t.on ? sx + 26 : sx + 10, sy + 10, 8, W(t.known ? 1 : 0.4));
        SText(cr, t.name, b.x + 16, b.y + b.h - 58, b.w - 24, 22, FSong, t.known ? W(1) : W(0.45));
        SText(cr, t.line, b.x + 16, b.y + b.h - 34, b.w - 24, 18, FBody, W(t.known ? 0.75 : 0.4));
        if (t.known) g_hits.push_back({b, Action::Radio, i});
    }
    const lsrc::Battery& bat = lsrc::BatteryState();
    if (bat.has)
        SText(cr, std::string(bat.charging ? "\xE2\x9A\xA1 Charging" : "Battery") + kDot + std::to_string(bat.percent) + "%", pg.x, pg.y + pg.h - 30,
              pg.w, 24, FBody, W(0.6), 1);
}

void PaintTodo(cairo_t* cr, const Rect& pg) {
    const auto& items = lsrc::Todos();
    int open = 0;
    for (const auto& t : items) open += t.done ? 0 : 1;
    SText(cr, open == 0 ? std::string("All done") : std::to_string(open) + (open == 1 ? " to-do" : " to-dos"), pg.x, pg.y, pg.w - 80, 20,
          FTitle, W(1));
    Pill(cr, {pg.x + pg.w - 70, pg.y, 70, 22}, "Add\xE2\x80\xA6", false, Action::TodoOpen);
    if (items.empty()) {
        SText(cr, "Add to-dos in Settings, with a time to be reminded.", pg.x, pg.y + 60, pg.w, 20, FBody, C(0x636366), 1);
        return;
    }
    // Open ones first (timed in order), then the done ones.
    std::vector<const bb::todos::Item*> list;
    for (const auto& t : items) list.push_back(&t);
    std::stable_sort(list.begin(), list.end(), [](const auto* a, const auto* b) {
        if (a->done != b->done) return !a->done;
        return (a->due > 0 ? a->due : 1e18) < (b->due > 0 ? b->due : 1e18);
    });
    const double epoch = static_cast<double>(time(nullptr));
    double y = pg.y + 30;
    for (const auto* t : list) {
        if (y + 24 > pg.y + pg.h + 2) break;
        const Rect row{pg.x, y, pg.w, 24};
        Fill(cr, row.x, row.y, row.w, row.h, 8, W(NextHitHovered() ? 0.12 : 0.05));
        const double cx = row.x + 14, cy = row.y + 12;
        if (t->done) {
            Dot(cr, cx, cy, 7, Accent());
            Tick(cr, cx, cy, 0.9, C(0x000000));
        } else {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, 7, 0, 2 * kPi);
            Source(cr, W(0.5));
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        }
        std::string when;
        if (t->due > 0) {
            const bool today = t->due - epoch < 20 * 3600 && t->due > epoch - 20 * 3600;
            when = today ? HourMinute(static_cast<long long>(t->due)) : std::string();
            if (!today) {
                const time_t d = static_cast<time_t>(t->due);
                tm dl{};
                localtime_r(&d, &dl);
                char b[32];
                strftime(b, sizeof b, "%a %-d %b", &dl);
                when = b;
            }
            if (!t->repeat.empty()) when = U(bb::todos::DescribeRepeat(t->repeat)) + kDot + when;
        }
        const double ww = when.empty() ? 0 : Measure(cr, when, FSmall) + 12;
        SText(cr, U(t->text), row.x + 28, row.y, row.w - 36 - ww, row.h, FBody, t->done ? C(0x636366) : W(1));
        if (!when.empty())
            SText(cr, when, row.x + row.w - ww - 6, row.y, ww, row.h, FSmall,
                  !t->done && t->due < epoch ? C(0xff9f0a) : kGrey, 2);
        g_hits.push_back({row, Action::TodoToggle, t->id});
        y += 28;
    }
}

void PaintClipboard(cairo_t* cr, const Rect& r) {
    const auto& items = lsrc::ClipHistory();
    SText(cr, "Recent copies", r.x, r.y, r.w - 60, 20, FLabel, kGrey);
    if (items.empty()) {
        SText(cr, "Copy some text and it shows up here.", r.x, r.y + 40, r.w, 30, FBody, C(0x636366), 1);
        return;
    }
    const Rect clear{r.x + r.w - 52, r.y + 1, 52, 18};
    Fill(cr, clear.x, clear.y, clear.w, clear.h, 9, {1.0, 0.27, 0.23, NextHitHovered() ? 0.3 : 0.18});
    SText(cr, "Clear", clear.x, clear.y, clear.w, clear.h, FLabel, C(0xff6961), 1);
    g_hits.push_back({clear, Action::ClipClear});
    double y = r.y + 26;
    for (size_t i = 0; i < items.size() && y + 24 <= r.y + r.h + 2; ++i) {
        std::string text = items[i];
        for (auto& c : text) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        const Rect row{r.x, y, r.w, 22};
        const bool hover = NextHitHovered();
        Fill(cr, row.x, row.y, row.w, row.h, 8, W(hover ? 0.13 : 0.05));
        Icon(cr, Page::Clipboard, row.x + 14, row.y + 11, 13, Accent());
        SText(cr, text, row.x + 28, row.y, row.w - 92, row.h, FBody, W(1));
        if (hover) SText(cr, "Copy", row.x + row.w - 60, row.y, 50, row.h, FLabel, Accent(), 2);
        g_hits.push_back({row, Action::ClipCopy, static_cast<int>(i)});
        y += 24;
    }
}

void PaintNote(cairo_t* cr, const Rect& r) {
    const auto& lines = lsrc::NoteLines();
    const Rect edit{r.x + r.w - 52, r.y + 1, 52, 18};
    Fill(cr, edit.x, edit.y, edit.w, edit.h, 9, WithA(Accent(), NextHitHovered() ? 0.3 : 0.18));
    SText(cr, "Edit", edit.x, edit.y, edit.w, edit.h, FLabel, Accent(), 1);
    g_hits.push_back({edit, Action::NoteEdit});
    if (lines.empty()) {
        SText(cr, "No note yet", r.x, r.y, r.w - 60, 20, FSong, W(1));
        SText(cr, "Click Edit to write one. Lines that start with [ ] get a checkbox.", r.x, r.y + 40, r.w, 20, FBody, C(0x636366));
        return;
    }
    double y = r.y;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& line = lines[i];
        if (i == 0) {
            SText(cr, line, r.x, y, r.w - 60, 20, FSong, W(1));
            y += 26;
            continue;
        }
        const double h = line.empty() ? 8 : 20;
        if (y + h > r.y + r.h + 2) break;
        bool checked = false;
        if (lsrc::NoteIsCheckbox(line, &checked)) {
            const double cx = r.x + 8, cy = y + 10;
            const bool hover = NextHitHovered();
            if (checked) {
                Dot(cr, cx, cy, 7, Accent());
                Tick(cr, cx, cy, 1.0, C(0x000000));
            } else {
                cairo_new_sub_path(cr);
                cairo_arc(cr, cx, cy, 7, 0, 2 * kPi);
                Source(cr, W(hover ? 0.9 : 0.5));
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
            }
            SText(cr, line.substr(line.find(L']') + 1), r.x + 22, y, r.w - 22, 20, FBody, checked ? C(0x636366) : W(1));
            g_hits.push_back({{r.x, y, r.w, 20}, Action::NoteToggle, static_cast<int>(i)});
        } else if (line.rfind(L"- ", 0) == 0) {
            Dot(cr, r.x + 8, y + 10, 2.5, Accent());
            SText(cr, line.substr(2), r.x + 22, y, r.w - 22, 20, FBody, W(1));
        } else {
            SText(cr, line, r.x, y, r.w, 20, FBody, C(0xaeaeb2));
        }
        y += h;
    }
}

int FocusPickerMinutes() {
    const lsrc::Focus& f = lsrc::FocusState();
    return f.pickH * 60 + f.pickM;
}

void PaintFocus(cairo_t* cr, const Rect& r) {
    const lsrc::Focus& f = lsrc::FocusState();
    const Col accent = Accent();
    if (f.active) {
        const double left = f.remaining;
        const double frac = f.total > 0 ? left / f.total : 0;
        SText(cr, Clock(left), r.x, r.y + 6, r.w * 0.62, 64, FHero, W(1));
        SText(cr, f.running ? "Focus" : "Focus" + kDot + "paused", r.x + 2, r.y + 72, r.w * 0.62, 18, FBody, accent);
        Fill(cr, r.x, r.y + 104, r.w, 4, 2, W(0.12));
        Fill(cr, r.x, r.y + 104, r.w * std::clamp(frac, 0.0, 1.0), 4, 2, accent);
        const double bx = r.x + r.w - 120;
        Pill(cr, {bx, r.y + 8, 120, 30}, f.running ? "Pause" : "Resume", true, Action::FocusPause);
        Pill(cr, {bx, r.y + 44, 120, 26}, "+5 min", false, Action::FocusAdd, 5);
        Pill(cr, {bx, r.y + 76, 120, 24}, "Stop", false, Action::FocusStop);
        SText(cr, "Bun lets you know when time is up.", r.x, r.y + 118, r.w, 18, FSmall, kGrey);
        return;
    }
    // Idle: the picker on the left (click a number above or below to change it),
    // start and presets on the right.
    const double colW = 64, rowH = 30, top = r.y + 14;
    Fill(cr, r.x, top + rowH, colW * 2 + 40, rowH, 9, W(0.07));
    for (int col = 0; col < 2; ++col) {
        const double x = r.x + col * (colW + 20);
        const int value = col == 0 ? f.pickH : f.pickM;
        const int step = col == 0 ? 1 : 5, mod = col == 0 ? 6 : 60;
        for (int k = -1; k <= 1; ++k) {
            const int v = ((value + k * step) % mod + mod) % mod;
            char b[8];
            snprintf(b, sizeof b, col == 0 ? "%d" : "%02d", v);
            const bool hover = k != 0 && NextHitHovered();
            SText(cr, b, x, top + (k + 1) * rowH, colW, rowH, k == 0 ? FSong : FBody, k == 0 ? W(1) : hover ? W(0.8) : C(0x48484a), 1);
            if (k != 0) g_hits.push_back({{x, top + (k + 1) * rowH, colW, rowH}, Action::FocusPick, col * 2 + (k > 0 ? 1 : 0)});
        }
        SText(cr, col == 0 ? "h" : "min", x + colW - 6, top + rowH, 26, rowH, FSmall, kGrey);
    }
    const int total = FocusPickerMinutes();
    const time_t doneAt = time(nullptr) + total * 60;
    tm dl{};
    localtime_r(&doneAt, &dl);
    char done[32];
    snprintf(done, sizeof done, "Done at %02d:%02d", dl.tm_hour, dl.tm_min);
    SText(cr, done, r.x, top + rowH * 3 + 6, colW * 2 + 60, 18, FSmall, kGrey);
    const double rx = r.x + colW * 2 + 80;
    SText(cr, Clock(total * 60.0), rx, r.y + 8, r.x + r.w - rx, 48, FBig, W(1));
    Pill(cr, {rx, r.y + 62, 110, 32}, "Start", true, Action::FocusStart, total);
    double cx = rx;
    for (int m : {5, 15, 25, 50}) {
        const std::string label = std::to_string(m) + " min";
        const double cw = Measure(cr, label, FLabel) + 22;
        const bool on = total == m;
        Fill(cr, cx, r.y + 104, cw, 24, 12, on ? WithA(accent, 0.35) : W(NextHitHovered() ? 0.16 : 0.09));
        SText(cr, label, cx, r.y + 104, cw, 24, FLabel, on ? W(1) : C(0xe8e8ee), 1);
        g_hits.push_back({{cx, r.y + 104, cw, 24}, Action::FocusStart, -m});  // negative: pick, don't start
        cx += cw + 6;
    }
}

// Every page of the island, plus Settings, in one grid.
void PaintApps(cairo_t* cr, const Rect& r) {
    struct App { Page page; int tab; const char* name; };
    std::vector<App> apps;
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        const Page p = st::PageFor(g_context, g_tabs[i]);
        if (p != Page::Apps) apps.push_back({p, static_cast<int>(i), nullptr});
    }
    apps.push_back({Page::Home, -2, "Colour"});  // the colour picker
    apps.push_back({Page::Home, -1, "Settings"});
    const int cols = 6;
    const double cellW = r.w / cols, cellH = 84;
    const bool waiting = g_agent.phase == AgentPhase::Approval || g_agent.phase == AgentPhase::Question;
    for (size_t i = 0; i < apps.size() && i < 12; ++i) {
        const double x = r.x + (i % cols) * cellW, y = r.y + 4 + (i / cols) * cellH;
        const App& a = apps[i];
        const bool hover = NextHitHovered();
        const double ix = x + cellW / 2 - 22;
        Fill(cr, ix, y, 44, 44, 10, hover ? C(0x3a3a3c) : C(0x1c1c1e));
        if (a.tab == -2) {
            cairo_save(cr);
            st::Eyedropper(PenFor(cr), Pt(ix + 22, y + 22), 20, D2(C(0xe8e8ee)));
            cairo_restore(cr);
        } else if (a.tab < 0) Gear(cr, ix + 22, y + 22, 20, C(0xe8e8ee));
        else Icon(cr, a.page, ix + 22, y + 22, 20, C(0xe8e8ee));
        SText(cr, a.name ? std::string(a.name) : U(TabName(a.page)), x, y + 48, cellW, 16, FSmall, W(1), 1);
        if (a.page == Page::Agent && waiting) {
            Dot(cr, ix + 42, y + 2, 9, C(0xff4245));
            SText(cr, "1", ix + 33, y - 7, 18, 18, FSmall, W(1), 1);
        }
        g_hits.push_back({{x + 4, y, cellW - 8, 66}, a.tab == -2 ? Action::ColorPick : a.tab < 0 ? Action::Settings : Action::OpenPage, a.tab});
    }
}

// ── File shelf ──────────────────────────────────────────────────────────────
// What you dropped on the island: click a file to open it, drag it out to use it
// somewhere else, x to take it off.

void PaintShelf(cairo_t* cr, const Rect& r) {
    const auto& items = bb::lshelf::Items();
    if (items.empty()) {
        Icon(cr, Page::Tray, r.x + r.w / 2, r.y + 54, 30, Accent());
        SText(cr, "Drop files on the island to keep them here", r.x, r.y + 86, r.w, 20, FBody, W(0.75), 1);
        SText(cr, "Click one to open it, or drag it out again", r.x, r.y + 108, r.w, 16, FSmall, kGrey, 1);
        return;
    }
    SText(cr, std::to_string(items.size()) + (items.size() == 1 ? " file" : " files"), r.x, r.y, 120, 18, FLabel, kGrey);
    Pill(cr, {r.x + r.w - 64, r.y - 2, 64, 22}, "Clear", false, Action::ShelfClear);
    const int cols = 2;
    const double cellW = (r.w - 10) / cols, cellH = 34, top = r.y + 26;
    const int rows = std::max(1, static_cast<int>((r.h - 26) / (cellH + 6)));
    for (size_t i = 0; i < items.size() && static_cast<int>(i) < rows * cols; ++i) {
        const double x = r.x + (i % cols) * (cellW + 10), y = top + (i / cols) * (cellH + 6);
        const bool hover = NextHitHovered();
        Fill(cr, x, y, cellW, cellH, 9, hover ? C(0x2c2c2e) : C(0x1c1c1e));
        // A folder tab or a page with a folded corner.
        cairo_save(cr);
        Source(cr, items[i].folder ? C(0x0a84ff) : C(0xaeaeb2));
        if (items[i].folder) {
            cairo_rectangle(cr, x + 10, y + 11, 16, 12);
            cairo_rectangle(cr, x + 10, y + 9, 7, 3);
            cairo_fill(cr);
        } else {
            cairo_move_to(cr, x + 12, y + 8);
            cairo_line_to(cr, x + 20, y + 8);
            cairo_line_to(cr, x + 25, y + 13);
            cairo_line_to(cr, x + 25, y + 26);
            cairo_line_to(cr, x + 12, y + 26);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
        cairo_restore(cr);
        SText(cr, items[i].name, x + 34, y, cellW - 34 - 26, cellH, FBody, W(1));
        g_hits.push_back({{x, y, cellW - 26, cellH}, Action::ShelfOpen, static_cast<int>(i)});
        const bool xHover = NextHitHovered();
        SText(cr, "\xC3\x97", x + cellW - 24, y, 20, cellH, FTitle, xHover ? W(1) : W(0.45), 1);
        g_hits.push_back({{x + cellW - 26, y, 26, cellH}, Action::ShelfRemove, static_cast<int>(i)});
    }
}

// ── Mirror ──────────────────────────────────────────────────────────────────
// A camera preview for a quick look before a call. The camera only turns on when
// you ask, and off again when you leave the page.

// The Water page, as on Windows (studio_draw.inc's DrawStudioWaterPage): a glass
// filling up to the goal, today with a drop per glass and the buttons, the week.
void PaintWater(cairo_t* cr, const Rect& r) {
    const bb::lwater::Snapshot w = bb::lwater::Now();
    const Col blue = Accent(), grey = C(0x8e8e93);  // the accent, and a deeper shade for the water
    const Col deep = {blue.r * 0.72, blue.g * 0.72, blue.b * 0.72};
    auto amount = [](int ml) {
        char b[24];
        if (ml >= 1000) snprintf(b, sizeof b, ml % 100 ? "%.2f L" : "%.1f L", ml / 1000.0);
        else snprintf(b, sizeof b, "%d ml", ml);
        return std::string(b);
    };
    const double frac = w.goalMl > 0 ? std::clamp(static_cast<double>(w.takenMl) / w.goalMl, 0.0, 1.0) : 0.0;

    // The glass, its water moving gently.
    const double gW = std::min(92.0, r.h * 0.5), gH = r.h - 12, gx = r.x + 4, gy = r.y + 6, gr = 18;
    Fill(cr, gx, gy, gW, gH, gr, W(0.05));
    if (frac > 0) {
        cairo_save(cr);
        RoundRect(cr, gx, gy, gW, gH, gr);
        cairo_clip(cr);
        const double level = gy + gH - gH * std::max(frac, 0.04);
        auto wave = [&](double amp, double speed, double phase, Col c) {
            cairo_move_to(cr, gx, gy + gH);
            for (double x = gx; x <= gx + gW + 0.5; x += 3)
                cairo_line_to(cr, x, level + std::sin(Now() * speed + phase + (x - gx) * 0.11) * amp);
            cairo_line_to(cr, gx + gW, gy + gH);
            cairo_close_path(cr);
            Source(cr, c);
            cairo_fill(cr);
        };
        wave(3.0, 1.6, 1.7, WithA(blue, 0.35));
        wave(2.4, 2.3, 0.0, WithA(deep, 0.85));
        cairo_restore(cr);
    }
    RoundRect(cr, gx + 0.75, gy + 0.75, gW - 1.5, gH - 1.5, gr);
    Source(cr, W(0.16));
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    SText(cr, std::to_string(static_cast<int>(frac * 100 + 0.5)) + "%", gx, gy + 10, gW, 24, FSong, W(1), 1);

    // Today.
    const double x0 = gx + gW + 22, colR = std::min(r.x + r.w, x0 + std::max(210.0, (r.x + r.w - x0) * 0.52));
    SText(cr, "Today", x0, r.y + 4, colR - x0, 16, FSmall, grey);
    const std::string big = amount(w.takenMl);
    SText(cr, big, x0, r.y + 18, colR - x0, 34, FBig, W(1));
    SText(cr, "of " + amount(w.goalMl), x0 + Measure(cr, big, FBig) + 8, r.y + 30, colR - x0, 20, FBody, grey);
    const int glasses = std::clamp(w.glassMl > 0 ? (w.goalMl + w.glassMl - 1) / w.glassMl : 8, 1, 12);
    for (int i = 0; i < glasses; ++i) Icon(cr, Page::Water, x0 + 8 + i * 18, r.y + 66, 14, i < w.glasses ? blue : C(0x48484a));
    const double by = r.y + 84;
    Button(cr, {x0, by, 112, 30}, "+ " + std::to_string(w.glassMl) + " ml", true, Action::WaterAdd);
    double bx = x0 + 120;
    if (w.glasses > 0) {
        Button(cr, {bx, by, 60, 30}, "Undo", false, Action::WaterUndo);
        bx += 68;
    }
    if (w.bottleMl > 0) Button(cr, {bx, by, std::min(86.0, colR - bx), 30}, "Refilled", false, Action::WaterRefill);
    std::string status;
    Col statusCol = grey;
    if (w.takenMl >= w.goalMl) {
        status = "Goal reached. Nicely done!";
        statusCol = blue;
    } else if (!w.reminders) {
        status = "Reminders off \xC2\xB7 Settings \xE2\x86\x92 Productivity \xE2\x86\x92 Water";
    } else if (w.nextMinute < 0) {
        status = "No more reminders today";
    } else {
        char b[48];
        snprintf(b, sizeof b, "Next reminder at %02d:%02d", w.nextMinute / 60, w.nextMinute % 60);
        status = b;
    }
    SText(cr, status, x0, by + 38, colR - x0, 18, FSmall, statusCol);
    if (w.bottleMl > 0)
        SText(cr, std::to_string(w.bottleLeftMl) + " ml left in your bottle", x0, by + 56, colR - x0, 18, FSmall, grey);

    // The week against the goal.
    const double cx0 = colR + 14, cw = r.x + r.w - cx0, cy0 = r.y + 4, ch = r.h - 8;
    if (cw < 120 || w.week.empty()) return;
    Fill(cr, cx0, cy0, cw, ch, 14, W(0.05));
    long long sum = 0;
    for (const auto& d : w.week) sum += d.second;
    SText(cr, "This week", cx0 + 12, cy0 + 8, cw - 24, 16, FSmall, grey);
    SText(cr, "avg " + amount(static_cast<int>(sum / static_cast<long long>(w.week.size()))), cx0 + 12, cy0 + 8, cw - 24, 16, FSmall,
          W(1), 2);
    const double top = cy0 + 34, bottom = cy0 + ch - 24, slot = (cw - 24) / 7.0;
    for (double x = cx0 + 12; x < cx0 + cw - 12; x += 7) Fill(cr, x, top - 1, std::min(4.0, cx0 + cw - 12 - x), 2, 1, W(0.18));
    const int n = static_cast<int>(w.week.size());
    static const char* kDays = "SMTWTFS";
    for (int i = 0; i < n; ++i) {
        const int date = w.week[static_cast<size_t>(i)].first, ml = w.week[static_cast<size_t>(i)].second;
        const double cx = cx0 + 12 + slot * (7 - n + i) + slot * 0.5, bw = std::min(14.0, slot * 0.5);
        const double h = (bottom - top) * std::clamp(w.goalMl > 0 ? static_cast<double>(ml) / w.goalMl : 0.0, 0.0, 1.0);
        const bool today = i == n - 1;
        Fill(cr, cx - bw / 2, top, bw, bottom - top, bw / 2, W(0.06));
        if (h > 0.5)
            Fill(cr, cx - bw / 2, bottom - std::max(h, bw), bw, std::max(h, bw), bw / 2,
                 today ? blue : WithA(blue, ml >= w.goalMl ? 0.75 : 0.4));
        tm t{};
        t.tm_year = date / 10000 - 1900;
        t.tm_mon = date / 100 % 100 - 1;
        t.tm_mday = date % 100;
        t.tm_hour = 12;
        mktime(&t);
        const char day[2] = {kDays[std::clamp(t.tm_wday, 0, 6)], 0};
        SText(cr, day, cx - 10, bottom + 4, 20, 16, FSmall, today ? W(1) : grey, 1);
    }
}

void PaintMirror(cairo_t* cr, const Rect& r) {
    cairo_surface_t* frame = bb::lshelf::CameraFrame();
    if (!bb::lshelf::CameraRunning() || !frame) {
        const std::string error = bb::lshelf::CameraError();
        Icon(cr, Page::Mirror, r.x + r.w / 2, r.y + 44, 30, Accent());
        SText(cr, error.empty() ? (bb::lshelf::CameraRunning() ? "Starting the camera\xE2\x80\xA6" : "Check how you look before a call")
                                : error,
              r.x, r.y + 74, r.w, 20, FBody, W(0.75), 1);
        if (!bb::lshelf::CameraRunning())
            Pill(cr, {r.x + r.w / 2 - 70, r.y + 104, 140, 30}, "Turn on the camera", true, Action::MirrorToggle);
        return;
    }
    // The picture fills the page, mirrored like a real mirror, with round corners.
    const double fw = cairo_image_surface_get_width(frame), fh = cairo_image_surface_get_height(frame);
    const double k = std::max(r.w / fw, r.h / fh);
    cairo_save(cr);
    RoundRect(cr, r.x, r.y, r.w, r.h, 14);
    cairo_clip(cr);
    cairo_translate(cr, r.x + r.w / 2, r.y + r.h / 2);
    cairo_scale(cr, -k, k);
    cairo_set_source_surface(cr, frame, -fw / 2, -fh / 2);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    Pill(cr, {r.x + r.w - 84, r.y + r.h - 34, 74, 26}, "Turn off", false, Action::MirrorToggle);
}

struct { double x = 0, y = 0; } g_timerRing;  // the countdown dial's centre, for clicks on it
#include "pages_timers_linux.inc"
#include "pages_system_linux.inc"
#include "pages_weather_linux.inc"
#include "pages_screentime_linux.inc"
#include "pages_clipboard_linux.inc"

// ── More Home cards ─────────────────────────────────────────────────────────

void TileWeather(cairo_t* cr, const Rect& r) {
    const lsrc::Weather& w = lsrc::CurrentWeather();
    Tile(cr, r, kSky, false);  // Home cards keep their own colours; only Music follows the album
    Icon(cr, Page::Weather, r.x + r.w - 30, r.y + 30, 20, kSky);
    if (w.has) TileTexts(cr, r, w.city.empty() ? "Weather" : w.city, W(0.65), Temp(w.temp), lsrc::WeatherWords(w.code));
    else TileTexts(cr, r, "Weather", W(0.65), "--\xC2\xB0", w.loading ? "Weather is loading" : "Set your city");
    if (const int i = TabIndexOf(Page::Weather); i >= 0) g_hits.push_back({r, Action::Tab, i});
}

void TileFocus(cairo_t* cr, const Rect& r) {
    const lsrc::Focus& f = lsrc::FocusState();
    Tile(cr, r, kPurple, f.active && f.running);
    const double rx = r.x + r.w - 30, ry = r.y + 30, bx = r.x + r.w - 27, by = r.y + r.h - 27;
    if (f.active) {
        Ring(cr, rx, ry, 15, 4.5, f.total > 0 ? f.remaining / f.total : 0, kPurple);
        const int mins = static_cast<int>(std::ceil(f.remaining / 60));
        TileTexts(cr, r, "Focus", W(0.65), Clock(f.remaining),
                  f.running ? std::to_string(mins) + (mins == 1 ? " min left" : " mins left") : "Paused", 46);
        const bool hover = NextHitHovered();
        Dot(cr, bx, by, hover ? 16 : 15, W(hover ? 1 : 0.92));
        Fill(cr, bx - 6, by - 6, 12, 12, 2, kPurple);
        g_hits.push_back({{bx - 17, by - 17, 34, 34}, Action::FocusStop});
    } else {
        Ring(cr, rx, ry, 15, 4.5, 0, kPurple);
        TileTexts(cr, r, "Focus", W(0.65), "25:00", "Ready to start", 46);
        RoundButton(cr, bx, by, 15, 0, kPurple, NextHitHovered());
        g_hits.push_back({{bx - 17, by - 17, 34, 34}, Action::FocusStart, 25});
    }
    if (const int i = TabIndexOf(Page::Focus); i >= 0) g_hits.push_back({r, Action::Tab, i});
}

// ── Work mode: the Home card and the Work page ──────────────────────────────
std::string WorkStatusLine(const bb::lwork::Snapshot& w) {
    namespace wk = bb::work;
    const std::string mid = " \xC2\xB7 ";
    if (w.study) {  // "Maths · until 11:00", "Physics at 14:00", "Free time"
        if ((w.phase == wk::Phase::Working || w.phase == wk::Phase::Overtime) && !w.subject.empty())
            return w.blockEnd >= 0 ? U(w.subject) + mid + "until " + U(wk::FormatClock(w.blockEnd)) : U(w.subject);
        if (w.phase == wk::Phase::Off) return w.nextBlock >= 0 ? U(w.nextSubject) + " at " + U(wk::FormatClock(w.nextBlock)) : "Free time";
        // Studying outside a class (by hand, or a Pomodoro): the work day's end doesn't apply.
        // The title already says Studying: the line says what's next.
        if (w.phase == wk::Phase::Working || w.phase == wk::Phase::Overtime)
            return w.nextBlock >= 0 ? "Next: " + U(w.nextSubject) + " at " + U(wk::FormatClock(w.nextBlock)) : "Free study";
    }
    switch (w.phase) {
        case wk::Phase::Working: return "Working" + mid + "until " + U(wk::FormatClock(w.schedule.end));
        case wk::Phase::Overtime: return "Overtime";
        case wk::Phase::Lunch: return "Lunch" + mid + "back at " + U(wk::FormatClock(w.schedule.lunchStart + w.schedule.lunchMinutes));
        case wk::Phase::Break: {
            const long long left = std::max(0LL, w.breakUntil - static_cast<long long>(time(nullptr)));
            return "Break" + mid + std::to_string((left + 59) / 60) + " min left";
        }
        case wk::Phase::Away: return "Away";
        case wk::Phase::Asleep: return "Asleep";
        case wk::Phase::Paused: return "Paused";
        case wk::Phase::Done: return "Done for today";
        default: return w.schedule.autoStart ? "Starts at " + U(wk::FormatClock(w.schedule.start)) : "Not started";
    }
}

void TileWork(cairo_t* cr, const Rect& r) {
    namespace wk = bb::work;
    const bb::lwork::Snapshot w = bb::lwork::Now();
    // Home cards keep their own colours (the Work page follows the accent).
    const Col col = w.phase == wk::Phase::Working ? C(wk::KindColour(wk::Kind::Work)) : WorkColour(w.phase);
    Tile(cr, r, col, w.phase == wk::Phase::Working || w.phase == wk::Phase::Overtime);
    Ring(cr, r.x + r.w - 30, r.y + 30, 15, 4.5, w.goal > 0 ? std::min(1.0, static_cast<double>(w.today.Worked()) / w.goal) : 0, col);
    std::string line = w.phase == wk::Phase::Working && !w.study ? "Until " + U(wk::FormatClock(w.schedule.end)) : WorkStatusLine(w);
    if (w.study && w.phase == wk::Phase::Working && !w.subject.empty() && w.blockEnd >= 0)
        line = U(w.subject) + " \xC2\xB7 " + U(wk::FormatClock(w.blockEnd));  // short on the card
    TileTexts(cr, r, w.study ? "Study" : "Work", W(0.65), U(wk::Duration(w.today.Worked())), line, 46);
    const double bx = r.x + r.w - 27, by = r.y + r.h - 27;
    const bool going = w.phase == wk::Phase::Working || w.phase == wk::Phase::Overtime || w.phase == wk::Phase::Away ||
                       w.phase == wk::Phase::Asleep;
    const Action act = going ? Action::WorkPause : (w.phase == wk::Phase::Paused || w.phase == wk::Phase::Break) ? Action::WorkResume
                                                                                                             : Action::WorkStart;
    RoundButton(cr, bx, by, 15, going ? 1 : 0, col, NextHitHovered());
    g_hits.push_back({{bx - 17, by - 17, 34, 34}, act});
    if (const int i = TabIndexOf(Page::Work); i >= 0) g_hits.push_back({r, Action::Tab, i});
}

void PaintWork(cairo_t* cr, const Rect& pg) {
    namespace wk = bb::work;
    const bb::lwork::Snapshot w = bb::lwork::Now();
    const Col col = WorkColour(w.phase), grey = C(0x8e8e93), card = C(0x1c1c1e);
    const double leftW = std::min(300.0, std::floor(pg.w * 0.4));
    // Left: the state, the time, the buttons.
    const Rect lc{pg.x, pg.y, leftW, pg.h};
    Fill(cr, lc.x, lc.y, lc.w, lc.h, 14, card);
    Dot(cr, lc.x + 20, lc.y + 22, 4.5, col);
    SText(cr, U(w.study ? wk::StudyLabel(w.phase) : wk::PhaseLabel(w.phase)), lc.x + 32, lc.y + 12, lc.w - 44, 20, FLabel, W(1));
    if (w.study && w.streak > 0)
        SText(cr, "\xF0\x9F\x94\xA5 " + std::to_string(w.streak) + (w.streak == 1 ? " day" : " days"), lc.x + 120, lc.y + 12, lc.w - 134, 20,
              FLabel, C(0xff9f0a), 2);
    {   // past an hour ("1h 12m") the hero size no longer fits; a step down then (as on Windows)
        const std::string worked = U(wk::Duration(w.today.Worked()));
        SText(cr, worked, lc.x + 16, lc.y + 38, lc.w - 86, 52, Measure(cr, worked, FHero) > lc.w - 86 ? FBig : FHero, W(1));
    }
    const double frac = w.goal > 0 ? std::min(1.0, static_cast<double>(w.today.Worked()) / w.goal) : 0;
    Ring(cr, lc.x + lc.w - 40, lc.y + 62, 22, 5, frac, col);
    SText(cr, std::to_string(static_cast<int>(frac * 100 + 0.5)) + "%", lc.x + lc.w - 62, lc.y + 54, 44, 16, FSmall, grey, 1);
    SText(cr, w.goal > 0 ? "of " + U(wk::Duration(w.goal)) + " today" : std::string(w.study ? "studied today" : "worked today"), lc.x + 16, lc.y + 92, lc.w - 28, 18, FBody, grey);
    SText(cr, WorkStatusLine(w), lc.x + 16, lc.y + 112, lc.w - 28, 18, FBody, col);
    std::vector<std::pair<std::string, Action>> btns;
    switch (w.phase) {
        case wk::Phase::Working: case wk::Phase::Overtime: case wk::Phase::Away: case wk::Phase::Asleep:
            btns = {{"Pause", Action::WorkPause}, {"Break", Action::WorkBreak}, {"Done", Action::WorkDone}}; break;
        case wk::Phase::Paused: case wk::Phase::Break: btns = {{"Resume", Action::WorkResume}, {"Done", Action::WorkDone}}; break;
        case wk::Phase::Lunch: btns = {{"Done for today", Action::WorkDone}}; break;
        case wk::Phase::Done: btns = {{"Start again", Action::WorkStart}}; break;
        default: btns = {{w.study ? "Start studying" : "Start work", Action::WorkStart}}; break;
    }
    double bx = lc.x + 14;
    const double by = lc.y + lc.h - 44;
    for (size_t i = 0; i < btns.size(); ++i) {
        const double bw = Measure(cr, btns[i].first, FLabel) + 28;
        if (bx + bw > lc.x + lc.w - 12) break;
        Button(cr, {bx, by, bw, 30}, btns[i].first, i == 0, btns[i].second);
        bx += bw + 8;
    }
    // Right: today's timeline and totals.
    const Rect rc{lc.x + lc.w + 10, pg.y, pg.w - lc.w - 10, pg.h};
    Fill(cr, rc.x, rc.y, rc.w, rc.h, 14, card);
    SText(cr, "Today", rc.x + 16, rc.y + 12, 120, 18, FLabel, W(1));
    const double repW = Measure(cr, "Report", FLabel) + 28;
    Button(cr, {rc.x + rc.w - 14 - repW, rc.y + 8, repW, 26}, "Report", false, Action::WorkReport);
    int from = w.schedule.start * 60, to = (w.schedule.end <= w.schedule.start ? w.schedule.end + 1440 : w.schedule.end) * 60;
    if (w.study) { from = w.today.first >= 0 ? w.today.first : 8 * 3600; to = std::max(from + 3600, w.today.last); }
    if (w.today.first >= 0) from = std::min(from, w.today.first);
    if (w.today.last >= 0) to = std::max(to, w.today.last);
    if (to <= from) to = from + 3600;
    const double bx0 = rc.x + 16, bw = rc.w - 32, byTop = rc.y + 46;
    Fill(cr, bx0, byTop, bw, 20, 6, W(0.06));
    auto xAt = [&](int s) { return bx0 + bw * std::clamp(static_cast<double>(s - from) / (to - from), 0.0, 1.0); };
    for (const wk::Segment& g : w.segments) {
        const double x0 = xAt(g.start), x1 = std::max(xAt(g.end), x0 + 1.5);
        Fill(cr, x0, byTop, x1 - x0, 20, 3, g.kind == wk::Kind::Work ? Accent() : C(wk::KindColour(g.kind)));
    }
    const time_t now = time(nullptr);
    tm lt{};
    localtime_r(&now, &lt);
    int nowSec = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    if (nowSec < from && to > 86400) nowSec += 86400;
    if (nowSec >= from && nowSec <= to) Fill(cr, xAt(nowSec) - 1, byTop - 4, 2, 28, 1, W(1));
    SText(cr, U(wk::FormatClock(from / 60)), bx0, byTop + 24, 60, 14, FSmall, grey);
    SText(cr, U(wk::FormatClock(to / 60)), bx0 + bw - 60, byTop + 24, 60, 14, FSmall, grey, 2);
    if (w.study) {  // today's subjects as bars, then away and breaks
        const double gy = byTop + 48, wAll = rc.w - 32;
        int most = 1;
        for (const auto& s : w.subjects) most = std::max(most, s.second);
        if (w.subjects.empty()) SText(cr, "Subjects show here as you study.", bx0, gy, wAll, 18, FBody, grey);
        static const uint32_t kSubject[] = {0x0a84ff, 0xbf5af2, 0x30d158, 0xff9f0a, 0x64d2ff};
        for (size_t i = 0; i < w.subjects.size() && i < 4; ++i) {
            const double y = gy + i * 22;
            const std::string dur = U(wk::Duration(w.subjects[i].second));
            const double dw = Measure(cr, dur, FLabel) + 6;
            SText(cr, U(w.subjects[i].first), bx0, y, 110, 18, FBody, W(1));
            const double b0 = bx0 + 116, b1 = rc.x + rc.w - 16 - dw;
            Fill(cr, b0, y + 5, b1 - b0, 8, 4, W(0.06));
            Fill(cr, b0, y + 5, std::max(8.0, (b1 - b0) * w.subjects[i].second / most), 8, 4, C(kSubject[i % 5]));
            SText(cr, dur, b1 + 6, y, dw, 18, FLabel, W(1), 2);
        }
        SText(cr, "Away " + U(wk::Duration(w.today.Of(wk::Kind::Away) + w.today.Of(wk::Kind::Asleep))) + " \xC2\xB7 Breaks " +
                      std::to_string(w.today.breaks), bx0, rc.y + rc.h - 26, wAll, 16, FSmall, grey);
        return;
    }
    const wk::Kind kinds[] = {wk::Kind::Work, wk::Kind::Away, wk::Kind::Lunch, wk::Kind::Overtime, wk::Kind::Asleep, wk::Kind::Break};
    const double colW = (rc.w - 32) / 2, gy = byTop + 50;
    for (int i = 0; i < 6; ++i) {
        const wk::Kind k = kinds[i];
        const double x = bx0 + (i / 3) * colW, y = gy + (i % 3) * 26;
        Fill(cr, x, y + 4, 10, 10, 3, k == wk::Kind::Work ? Accent() : C(wk::KindColour(k)));
        const std::string dur = U(wk::Duration(w.today.Of(k)));
        const double dw = Measure(cr, dur, FLabel) + 6;
        std::string label = U(wk::KindLabel(k));
        if (k == wk::Kind::Break) label += " (" + std::to_string(w.today.breaks) + ")";
        if (Measure(cr, label, FBody) > colW - 26 - dw) label = k == wk::Kind::Work ? "Work" : k == wk::Kind::Overtime ? "Over" : k == wk::Kind::Break ? "Breaks" : label;
        SText(cr, label, x + 18, y, colW - 22 - dw, 18, FBody, grey);
        SText(cr, dur, x + colW - 4 - dw, y, dw, 18, FLabel, W(1), 2);
    }
}

// ── Lyrics: the small-island pill and the Lyrics page ───────────────────────
// The same behaviour as Windows (island_core.cpp DrawCollapsedLyrics): the pill
// is sized to the song's longest line (280-380), a line too long for it gets
// smaller (down to 80 %) and only then an ellipsis, so its start is never cut
// off, and each new line rolls up from below in 0.42 s (ease-out quart).

constexpr double kRollSec = 0.42;
constexpr double kPillTextSize = 13.0;

// One line of lyric text, fitted to `w`: shrunk to 80 % at most, then ellipsized.
// Returns the width drawn. Centred when it fits.
double FittedLine(cairo_t* cr, const std::string& text, double x, double y, double w, double h, double size, Col c) {
    if (text.empty() || w <= 4) return 0;
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string(bb::llook::Font());
    pango_font_description_set_weight(font, PANGO_WEIGHT_SEMIBOLD);
    pango_font_description_set_absolute_size(font, bb::llook::TextSize(size) * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), -1);
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    if (pw > w) {
        pango_font_description_set_absolute_size(font, size * std::max(0.8, w / pw) * PANGO_SCALE);
        pango_layout_set_font_description(layout, font);
        pango_layout_get_pixel_size(layout, &pw, &ph);
        if (pw > w) {
            pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
            pango_layout_get_pixel_size(layout, &pw, &ph);
        }
    }
    Source(cr, c);
    cairo_move_to(cr, x + std::max(0.0, (w - pw) / 2), y + (h - ph) / 2);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
    return pw;
}

double WidthAt(cairo_t* cr, const std::string& text, double size) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string(bb::llook::Font());
    pango_font_description_set_weight(font, PANGO_WEIGHT_SEMIBOLD);
    pango_font_description_set_absolute_size(font, bb::llook::TextSize(size) * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), -1);
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    pango_font_description_free(font);
    g_object_unref(layout);
    return pw;
}

double Ease(double t) {
    const double inv = 1.0 - std::clamp(t, 0.0, 1.0);
    return 1.0 - inv * inv * inv * inv;
}

const std::string kNote = "\xE2\x99\xAA";

// What a lyric slot shows now: the line, the note in the intro and long breaks,
// or the title while there are no usable lyrics. `key` changes when it should roll.
struct Shown {
    std::string text;
    int key = -2;
    bool title = false;
};

Shown ShowNow(int* idxOut = nullptr, int64_t* posOut = nullptr) {
    Shown s;
    int64_t posMs = 0;
    int idx = -1;
    if (!llyrics::Usable(g_track)) {
        s.text = g_track.title.empty() ? g_track.player : g_track.title;
        s.key = -3;
        s.title = true;
    } else {
        idx = llyrics::LineAt(bb::mpris::Position(), g_track.playing, &posMs);
        const auto& lines = llyrics::Current().lines;
        const bool gap = llyrics::GapNoteMix(idx, posMs) > 0.0f;
        s.key = gap ? -1000 - idx : idx;
        s.text = !gap && idx >= 0 && !lines[idx].text.empty() ? U(lines[idx].text) : kNote;
    }
    if (idxOut) *idxOut = idx;
    if (posOut) *posOut = posMs;
    return s;
}

// ── The pill ──

Shown g_pillCur, g_pillPrev;
double g_pillAnimAt = -10;
std::string g_pillTrack;           // the song g_pillCur belongs to
std::string g_pillWidthTrack;      // the song the fitted width was measured for
double g_pillWidthNeed = 0;        // the longest lyric line of that song, in px

}  // namespace

double LyricPillWidth(cairo_t* cr, double chrome, double maxWidth) {
    if (!llyrics::PillOn() || !llyrics::Usable(g_track)) {
        g_pillWidthTrack.clear();
        return 0;
    }
    const std::string song = g_track.title + "\n" + g_track.artist;
    if (song != g_pillWidthTrack) {  // measured once per song
        g_pillWidthTrack = song;
        double widest = 0;
        for (const auto& l : llyrics::Current().lines)
            if (!l.text.empty()) widest = std::max(widest, WidthAt(cr, U(l.text), kPillTextSize));
        g_pillWidthNeed = widest + chrome + 6;
    }
    // The limit comes from the screen, not a fixed number, so a long line isn't cut.
    return std::clamp(g_pillWidthNeed, 280.0, std::max(280.0, maxWidth));
}

double PillTextWidth(cairo_t* cr, const std::string& text) {
    // The same font and size PaintLyricPill draws the title in (the title shows there when no
    // lyric is usable), so the pill is sized for the text it will show.
    return WidthAt(cr, text, kPillTextSize);
}

void PaintLyricPill(cairo_t* cr, const Rect& area) {
    const Shown now = ShowNow();
    const std::string song = g_track.title + "\n" + g_track.artist;
    if (song != g_pillTrack || g_pillCur.key == -2) {  // a new song or the first draw: no roll
        g_pillTrack = song;
        g_pillCur = now;
        g_pillPrev = Shown{};
        g_pillAnimAt = -10;
    } else if (now.key != g_pillCur.key || (now.title && now.text != g_pillCur.text)) {
        g_pillPrev = g_pillCur;
        g_pillCur = now;
        g_pillAnimAt = Now();
    }
    const double t = std::clamp((Now() - g_pillAnimAt) / kRollSec, 0.0, 1.0), e = Ease(t);
    const double slide = std::max(13.0, area.h * 0.8);
    cairo_save(cr);
    cairo_rectangle(cr, area.x, area.y, area.w, area.h);
    cairo_clip(cr);
    // The outgoing line rises and fades a bit faster than the new one arrives.
    if (t < 1.0 && !g_pillPrev.text.empty())
        FittedLine(cr, g_pillPrev.text, area.x, area.y - e * slide, area.w, area.h, kPillTextSize,
                   W((1.0 - std::clamp(t * 1.6, 0.0, 1.0)) * 0.9));
    FittedLine(cr, g_pillCur.text, area.x, area.y + (1.0 - e) * slide, area.w, area.h, kPillTextSize,
               W(e * (g_pillCur.title ? 0.90 : 0.97)));
    cairo_restore(cr);
}

namespace {

// ── The page ──

int g_pageIdx = -2;
double g_pageAnimAt = -10;
std::string g_pageTrack;

void PaintLyrics(cairo_t* cr, const Rect& pg) {
    const llyrics::State& st = llyrics::Current();
    const bool mine = st.title == g_track.title && st.artist == g_track.artist;
    const double cx = pg.x + pg.w / 2;
    if (!mine || st.fetching || (!st.hasData && !st.notFound)) {
        Icon(cr, Page::Lyrics, cx, pg.y + 60, 30, Accent());
        SText(cr, "Finding the lyrics\xE2\x80\xA6", pg.x, pg.y + 92, pg.w, 22, FBody, W(0.75), 1);
        return;
    }
    if (st.notFound) {
        Icon(cr, Page::Lyrics, cx, pg.y + 60, 30, W(0.4));
        SText(cr, st.instrumental ? "Instrumental \xE2\x80\x94 enjoy the music" : "No lyrics for this song", pg.x, pg.y + 92, pg.w, 22,
              FBody, W(0.75), 1);
        return;
    }
    const double rowH = 30;
    // Modules.LyricsVisualizer: a small live spectrum in the corner while it plays.
    if (bb::lfeat::LyricsVisualizer() && g_track.playing) {
        const Col acc = Accent();
        const double rgb[3] = {acc.r, acc.g, acc.b};
        bb::lfeat::Spectrum(cr, pg.x + pg.w - 34, pg.y + 8, 30, 14, rgb, 0.85, true, Now());
    }
    if (!st.synced) {  // plain lyrics: the opening lines, as they are
        SText(cr, "Not synced", pg.x, pg.y, pg.w, 16, FSmall, kGrey, 1);
        double y = pg.y + 22;
        for (size_t i = 0; i < st.lines.size() && y + 24 <= pg.y + pg.h; ++i, y += 24)
            FittedLine(cr, U(st.lines[i].text), pg.x, y, pg.w, 24, 14, W(0.85));
        return;
    }
    int idx = -1;
    int64_t posMs = 0;
    const Shown now = ShowNow(&idx, &posMs);
    const std::string song = g_track.title + "\n" + g_track.artist;
    if (song != g_pageTrack) {
        g_pageTrack = song;
        g_pageIdx = idx;
        g_pageAnimAt = -10;
    } else if (idx != g_pageIdx) {
        g_pageIdx = idx;
        g_pageAnimAt = Now();
    }
    // The current line in the middle, the ones around it fading out; the list
    // rolls up by one row as each line starts.
    const double e = Ease((Now() - g_pageAnimAt) / kRollSec);
    const double mid = pg.y + pg.h / 2 - rowH / 2 + (1.0 - e) * rowH;
    const Col accent = Accent();
    cairo_save(cr);
    cairo_rectangle(cr, pg.x, pg.y, pg.w, pg.h);
    cairo_clip(cr);
    for (int k = -3; k <= 4; ++k) {
        const int i = idx + k;
        const double y = mid + k * rowH;
        if (y + rowH < pg.y || y > pg.y + pg.h) continue;
        std::string text;
        if (i < 0) {
            if (k != 0) continue;
            text = kNote;  // before the first line
        } else if (i < static_cast<int>(st.lines.size())) {
            text = k == 0 ? now.text : U(st.lines[i].text);
            if (text.empty()) text = kNote;
        } else {
            continue;
        }
        // The lines around the current one are a faint ghost, or readable with
        // Modules.LyricsNeighborLinesVisible.
        const double near = bb::lfeat::Get().neighbours ? 0.72 : 0.42;
        const double fade = k == 0 ? 1.0 : std::max(0.12, near - 0.1 * (std::abs(k) - 1));
        const Col lit = Lerp(W(0.85), accent, 0.25);
        const auto& feat = bb::lfeat::Get();
        if (k == 0 && feat.karaoke && i >= 0 && text != kNote) {
            // Karaoke: the line lights up left to right as it's sung; with the letter
            // glow, the letters at the edge of the light glow in the accent colour.
            const int64_t start = st.lines[i].timeMs;
            int64_t end = start + 4000;
            if (i + 1 < static_cast<int>(st.lines.size()) && st.lines[i + 1].timeMs > start) end = st.lines[i + 1].timeMs;
            const double frac = std::clamp(double(posMs - start) / double(std::max<int64_t>(1, end - start)), 0.0, 1.0);
            const double pw = FittedLine(cr, text, pg.x, y, pg.w, rowH, 19, W(0.38));
            const double left = pg.x + std::max(0.0, (pg.w - pw) / 2), edge = left + pw * frac;
            cairo_save(cr);
            cairo_rectangle(cr, left - 2, y, edge - left + 2, rowH);
            cairo_clip(cr);
            FittedLine(cr, text, pg.x, y, pg.w, rowH, 19, lit);
            cairo_restore(cr);
            if (feat.karaokeGlow && frac > 0 && frac < 1) {
                cairo_save(cr);
                cairo_rectangle(cr, edge - 10, y, 12, rowH);
                cairo_clip(cr);
                FittedLine(cr, text, pg.x, y, pg.w, rowH, 19, WithA(accent, 0.95));
                cairo_restore(cr);
            }
            continue;
        }
        FittedLine(cr, text, pg.x, y, pg.w, rowH, k == 0 ? 19 : 15, k == 0 ? lit : W(fade));
    }
    cairo_restore(cr);
}

void PaintAgentPage(cairo_t* cr, const Rect& r) {
    const AgentView& a = g_agent;
    const Col status = C(st::AgentStatus(a.phase)), brand = C(st::AgentBrand(a.agent));
    const bool busy = a.phase == AgentPhase::Working || a.phase == AgentPhase::Thinking;
    const double now = Now();

    // Top row: lamp, agent and project; the status (and Claude's model) on the right.
    Dot(cr, r.x + 5, r.y + 10, 4, WithA(status, busy && std::fmod(now, 1.2) < 0.6 ? 0.45 : 1.0));
    std::string who = a.agent.empty() ? "No agent running" : U(a.agent);
    if (!a.project.empty()) who += kDot + U(a.project);
    SText(cr, who, r.x + 16, r.y, r.w * 0.6 - 16, 20, FTitle, W(1));
    std::string stat;
    switch (a.phase) {
        case AgentPhase::Thinking:
        case AgentPhase::Working: stat = "Working" + kDot + Clock(a.turnStart > 0 ? now - a.turnStart : now - a.phaseSince); break;
        case AgentPhase::Approval: stat = "Needs your OK"; break;
        case AgentPhase::Question: stat = "Has a question"; break;
        case AgentPhase::Finished: stat = "Done"; break;
        case AgentPhase::Error: stat = "Stopped"; break;
        default: stat = "Idle"; break;
    }
    SText(cr, stat, r.x + r.w * 0.6, r.y, r.w * 0.4, 20, FBody, C(0x8e8e93), 2);
    if (AgentKindFor(a.agent) == AgentClaude && g_s.showAgent[AgentClaude]) {
        const std::string model = "Model: " + ClaudeModelLabel();
        const double mw = Measure(cr, model, FLabel) + 20, sw = Measure(cr, stat, FBody);
        const Rect chip{r.x + r.w - sw - 12 - mw, r.y, mw, 20};
        Fill(cr, chip.x, chip.y, chip.w, chip.h, 10, W(NextHitHovered() ? 0.16 : 0.09));
        SText(cr, model, chip.x, chip.y, chip.w, chip.h, FLabel, C(0xe8e8ee), 1);
        g_hits.push_back({chip, Action::ModelCycle});
    }

    // Left: a big number or word, and a line under it.
    const double leftW = r.w * 0.40;
    std::string hero, sub;
    if (busy) {
        hero = Clock(a.turnStart > 0 ? now - a.turnStart : now - a.phaseSince);
        sub = std::string(StageWord(a.stage)) + kDot + "step " + std::to_string(std::clamp(a.stage + 1, 1, 4)) + " of 4";
    } else if (a.phase == AgentPhase::Approval) { hero = "May I?"; sub = U(a.tool); }
    else if (a.phase == AgentPhase::Question) { hero = "Your turn"; sub = "Answer on the island"; }
    else if (a.phase == AgentPhase::Finished) { hero = "Done"; sub = a.turnSeconds > 0 ? "took " + Clock(a.turnSeconds) : ""; }
    else if (a.phase == AgentPhase::Error) { hero = "Stopped"; sub = U(a.step); }
    else { hero = std::to_string(a.jobsToday); sub = a.jobsToday == 1 ? "task done today" : "tasks done today"; }
    SText(cr, hero, r.x, r.y + 30, leftW, 52, hero.size() > 7 ? FBig : FHero, busy ? brand : W(1));
    SText(cr, sub, r.x, r.y + 84, leftW, 18, FBody, C(0x8e8e93));

    // Right: a card with the command, the diff, the prompt, or recent tasks.
    const Rect card{r.x + leftW + 12, r.y + 28, r.w - leftW - 12, r.h - 28 - 40};
    Fill(cr, card.x, card.y, card.w, card.h, 12, W(0.07));
    const double lineH = 17;
    double y = card.y + 8;
    auto row = [&](const std::string& text, Font f, Col c, Col bg = {0, 0, 0, 0}) {
        if (y + lineH > card.y + card.h - 4) return;
        if (bg.a > 0) Fill(cr, card.x + 6, y, card.w - 12, lineH, 4, bg);
        SText(cr, text, card.x + 12, y, card.w - 22, lineH, f, c);
        y += lineH + 2;
    };
    if (!a.command.empty() && (a.stage == 2 || busy) && (a.stage == 2 || a.diffNew.empty())) {
        row("$ " + U(a.command), FMono, C(0x32d74b));
        for (const auto& l : a.output) row(U(l), FMono, a.commandFailed ? C(0xff6961) : C(0xaeaeb2));
        if (a.output.empty() && busy) row("running\xE2\x80\xA6", FMono, C(0x636366));
    } else if (!a.diffOld.empty() || !a.diffNew.empty()) {
        if (!a.diffOld.empty()) row("\xE2\x88\x92 " + U(a.diffOld), FMono, C(0xff8a80), {1.0, 0.27, 0.23, 0.13});
        if (!a.diffNew.empty()) row("+ " + U(a.diffNew), FMono, C(0x7ee787), {0.2, 0.84, 0.29, 0.12});
        if (a.added >= 0) row("+" + std::to_string(a.added) + "  \xE2\x88\x92" + std::to_string(a.removed) + " lines", FSmall, C(0x8e8e93));
    } else if (busy || a.phase == AgentPhase::Approval) {
        if (!a.prompt.empty()) row("\xE2\x80\x9C" + U(a.prompt) + "\xE2\x80\x9D", FBody, W(1));
        row(a.phase == AgentPhase::Approval ? U(a.detail) : (a.step.empty() ? "Thinking\xE2\x80\xA6" : U(a.step)), FBody, C(0xaeaeb2));
    } else {
        row("Recently", FLabel, C(0x8e8e93));
        if (a.history.empty()) row("Finished tasks show up here.", FBody, C(0x636366));
        for (size_t i = 0; i < a.history.size() && i < 4; ++i) {
            const auto& h = a.history[i];
            if (y + lineH > card.y + card.h - 4) break;
            const std::string right = HourMinute(h.finishedAt) + kDot + Clock(h.seconds);
            const double rw = Measure(cr, right, FSmall);
            SText(cr, std::string(h.failed ? "\xE2\x9C\x95 " : "") + U(h.project.empty() ? h.agent : h.project) + kDot + U(h.prompt),
                  card.x + 12, y, card.w - rw - 28, lineH, FBody, W(1));
            SText(cr, right, card.x + card.w - rw - 12, y, rw + 2, lineH, FSmall, C(0x8e8e93), 2);
            y += lineH + 2;
        }
    }

    // Typing a reply takes the bottom row.
    const double by = r.y + r.h - 30;
    if (g_compose.kind == ComposeReply) {
        PaintCompose(cr, {r.x, by - 6, r.w, 30});
        return;
    }
    // A finished task still listening for your next message: Reply.
    if (a.replyId && !busy) Button(cr, {card.x + card.w - 92, card.y + card.h - 34, 84, 26}, "Reply\xE2\x80\xA6", true, Action::Reply);

    // Bottom: Read / Edit / Test / Done while working, else the usage limits.
    if (busy || a.phase == AgentPhase::Finished) {
        static const char* kSteps[] = {"Read", "Edit", "Test", "Done"};
        const double segW = (r.w - 3 * 8) / 4;
        for (int i = 0; i < 4; ++i) {
            const double x = r.x + i * (segW + 8);
            const bool done = a.stage > i || a.phase == AgentPhase::Finished;
            const bool current = !done && (a.stage == i || (a.stage < 0 && i == 0));
            Fill(cr, x, by, segW, 4, 2, W(done ? 0.95 : 0.16));
            if (current) {
                const double t = 0.5 + 0.5 * std::sin(now * 2 * kPi / 1.3);
                const double sw = segW * 0.45;
                Fill(cr, x + (segW - sw) * t, by, sw, 4, 2, brand);
            }
            SText(cr, kSteps[i], x, by + 7, segW, 15, FSmall, done || current ? W(1) : C(0x636366));
        }
    } else {
        // The limits of the agent on this page (or the main agent between tasks), from
        // what it keeps on this PC: Claude Code's windows and context, Antigravity's
        // model pools, Cursor's plan. The same as Windows (common/agent_limits.h).
        const long long unixNow = static_cast<long long>(time(nullptr));
        const int kind = a.agent.empty() ? MainAgentKind() : static_cast<int>(AgentKindFor(a.agent));
        bb::limits::Row row;
        if (kind == AgentAntigravity && g_s.antigravityQuota) {
            const AgSnapshot ag = AgQuota();
            row = bb::limits::Antigravity(ag.valid ? ag.pools : std::vector<bb::agquota::Pool>{}, unixNow);
        } else if (kind == AgentCursor) {
            row = bb::limits::Cursor(CursorPlan());
        } else if (kind == AgentClaude && g_s.usageLimits) {
            row = bb::limits::Claude(a.usage5h, a.usage5hResets, a.usage7d, a.usage7dResets, a.contextPct, unixNow);
        }
        if (!row.bars.empty()) {
            const double ly = r.y + r.h - 38;  // a second line under each bar for its reset time
            const int n = static_cast<int>(row.bars.size());
            const double segW = (r.w - (n - 1) * 14) / n;
            for (int i = 0; i < n; ++i) {
                const bb::limits::Bar& b = row.bars[static_cast<size_t>(i)];
                const double x = r.x + i * (segW + 14);
                const float p = b.used;
                Fill(cr, x, ly, segW, 4, 2, W(0.16));
                if (p > 0) Fill(cr, x, ly, std::max(4.0, segW * std::clamp(p / 100.0, 0.0, 1.0)), 4, 2, C(st::UsageColour(p)));
                std::string label = U(b.label);
                if (p >= 0) label += kDot + std::to_string(static_cast<int>(p + 0.5f)) + "%";
                SText(cr, label, x, ly + 7, segW, 14, FSmall, p >= 0 ? C(0xaeaeb2) : C(0x636366));
                const std::wstring reset = bb::limits::ResetsIn(b.resetAt, unixNow);
                if (!reset.empty()) SText(cr, reset, x, ly + 21, segW, 13, FSmall, C(0x636366));
            }
        } else if (!row.note.empty()) {
            SText(cr, row.note, r.x, by, r.w, 20, FSmall, C(0x8e8e93));
        }
    }
}

// ── Page pictures (Settings → Island pages) ────────────────────────────────
// The same files as Windows: <state>/previews/<page key>.png, taken only when
// asked ("Take pictures of my island"). Linux draws each page offscreen, so
// taking them never changes what the island shows.

const char* PageKey(Page p) {
    static const char* kKeys[] = {"home", "music", "calendar", "weather", "system", "toggles", "tray", "todo",
                                  "lyrics", "agent", "clipboard", "note", "focus", "apps", "mirror", "water", "screentime", "work"};
    return kKeys[std::clamp(static_cast<int>(p), 0, static_cast<int>(std::size(kKeys)) - 1)];
}

cairo_status_t AppendPng(void* closure, const unsigned char* data, unsigned int length) {
    static_cast<std::string*>(closure)->append(reinterpret_cast<const char*>(data), length);
    return CAIRO_STATUS_SUCCESS;
}

void PaintStudioFrame(cairo_t* cr, const Rect& isl, double mix);  // below

bool SavePicture(int tabIndex) {
    constexpr double kScale = 2;  // sharp in the settings window on HiDPI screens
    const double w = st::kIslandW, h = st::kIslandH;
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(w * kScale), static_cast<int>(h * kScale));
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, kScale, kScale);
    RoundRect(cr, 0, 0, w, h, 32);
    cairo_set_source_rgba(cr, 0.035, 0.035, 0.045, 0.98);
    cairo_fill_preserve(cr);
    cairo_clip(cr);
    // Draw as if this tab were open, without touching the live island.
    const std::vector<Hit> hits = g_hits;
    const int hover = g_hoverHit, tab = g_tabIdx;
    const bool want = g_wantAgentPage;
    g_hoverHit = -1;
    g_wantAgentPage = false;
    g_tabIdx = tabIndex;
    g_offscreen = true;
    UpdateTabs();
    const Page page = g_page;
    PaintStudioFrame(cr, {0, 0, w, h}, 1.0);
    g_offscreen = false;
    g_tabIdx = tab;
    g_wantAgentPage = want;
    g_hoverHit = hover;
    g_hits = hits;
    cairo_destroy(cr);
    std::string png;
    const bool ok = cairo_surface_write_to_png_stream(surface, AppendPng, &png) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(surface);
    const std::string dir = bb::plat::StateDir() + "/previews";
    bb::plat::MakeDirs(dir);
    return ok && bb::plat::WriteFileAtomic(dir + "/" + PageKey(page) + ".png", png);
}

}  // namespace

// ── Entry points (island_linux.h) ───────────────────────────────────────────

int StudioCapturePages() {
    UpdateTabs();
    int saved = 0;
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i) saved += SavePicture(i) ? 1 : 0;
    UpdateTabs();
    return saved;
}

void StudioLoadSettings() {
    namespace s = bb::settings;
    bb::lwater::LoadSettings();
    g_s.studio = st::EqualsNoCase(s::Get(L"Bunbun.Layout"), L"studio");
    g_s.agentTab = s::GetBool(L"Bunbun.AgentTab");
    g_s.usageLimits = s::GetBool(L"Bunbun.UsageLimits");
    static const wchar_t* kShow[AgentKindCount] = {L"Bunbun.ShowClaude", L"Bunbun.ShowAntigravity", L"Bunbun.ShowCursor",
                                                   L"Bunbun.ShowCopilot", L"Bunbun.ShowCodex", L"Bunbun.ShowOtherAgents"};
    static const wchar_t* kMain[AgentKindCount] = {L"claude", L"antigravity", L"cursor", L"copilot", L"codex", L"other"};
    for (int i = 0; i < AgentKindCount; ++i) g_s.showAgent[i] = s::GetBool(kShow[i]);
    const std::wstring main = s::Get(L"Bunbun.MainAgent");
    g_s.mainAgent = -1;
    for (int i = 0; i < AgentKindCount; ++i)
        if (st::EqualsNoCase(main, kMain[i])) g_s.mainAgent = i;
    static const wchar_t* kTiles[3] = {L"Bunbun.HomeTile1", L"Bunbun.HomeTile2", L"Bunbun.HomeTile3"};
    for (int i = 0; i < 3; ++i) g_s.homeTile[i] = st::HomeTileSetting(s::Get(kTiles[i]));
    g_s.homeLinks = s::Get(L"Bunbun.HomeLinks");
    g_links = st::ParseHomeLinks(g_s.homeLinks);
    g_s.pageOrder = s::Get(L"Bunbun.PageOrder");
    g_s.weather = s::GetBool(L"Modules.Weather");
    g_s.system = s::GetBool(L"Modules.HardwareMonitorModule");
    g_s.toggles = s::GetBool(L"Modules.ConnectivityModule");
    g_s.todo = s::GetBool(L"Modules.TodoModule");
    g_s.clipboard = s::GetBool(L"Bunbun.ClipboardHistory");
    g_s.note = s::GetBool(L"Bunbun.QuickNote");
    g_s.focus = s::GetBool(L"Bunbun.FocusPage");
    g_s.apps = s::GetBool(L"Bunbun.AppsPage");
    g_s.antigravityQuota = s::GetBool(L"Bunbun.AntigravityQuota");
    bb::lshelf::LoadSettings();
    lsrc::LoadSettings();
    const std::wstring accent = s::Get(L"Themes.AccentColorMode");
    g_s.accentMode = st::EqualsNoCase(accent, L"custom") ? 2 : st::EqualsNoCase(accent, L"system") ? 1 : 0;
    std::wstring hex = s::Get(L"Themes.CustomAccentHex");
    if (!hex.empty() && hex[0] == L'#') hex.erase(0, 1);
    if (hex.size() == 6) g_s.customAccent = static_cast<uint32_t>(wcstoul(hex.c_str(), nullptr, 16));
}

bool StudioLayout() { return g_s.studio; }

void StudioAccentRgb(double rgb[3]) {
    const Col c = Accent();
    rgb[0] = c.r;
    rgb[1] = c.g;
    rgb[2] = c.b;
}

int StudioContext() { return g_track.active && g_mediaOn ? 0 : 1; }

void StudioShowAgentPage() { g_wantAgentPage = true; }
void StudioShowShelf() { g_wantShelfPage = true; }
bool StudioOnMirror() { return g_page == Page::Mirror; }

void PaintStudio(cairo_t* cr, const Rect& isl, double mix) {
    CheckCompose();
    UpdateTabs();
    PaintStudioFrame(cr, isl, mix);  // page pictures are taken only when asked (StudioCapturePages)
}

namespace {
void PaintStudioFrame(cairo_t* cr, const Rect& isl, double mix) {
    g_alpha = mix * mix;
    // Music glows from the bottom in the album's colour; the Agent page in the
    // agent's status colour while it works.
    Col tint;
    double strength = 0;
    if (g_page == Page::Music || g_page == Page::Lyrics) {
        tint = Accent();
        strength = g_track.playing ? 0.55 : 0.30;
    } else if (g_page == Page::Agent && g_agent.phase != AgentPhase::None) {
        tint = C(st::AgentStatus(g_agent.phase));
        strength = 0.35;
    }
    if (strength > 0) Glow(cr, isl.x, isl.y, isl.w, isl.h, 0, tint, strength * mix, 0.2);

    const Rect pg{isl.x + st::kSide * mix, isl.y + st::kHeaderH * mix, isl.w - 2 * st::kSide * mix,
                  isl.h - (st::kHeaderH + st::kFooterH) * mix};
    cairo_save(cr);
    cairo_push_group(cr);
    switch (g_page) {
        case Page::Music: PaintMusic(cr, pg); break;
        case Page::Lyrics: PaintLyrics(cr, pg); break;
        case Page::Agent: PaintAgentPage(cr, pg); break;
        case Page::Calendar: PaintCalendar(cr, pg); break;
        case Page::Weather: PaintWeatherCards(cr, pg); break;
        case Page::Hardware: PaintSystemCards(cr, pg); break;
        case Page::Connectivity: PaintToggles(cr, pg); break;
        case Page::Todo: PaintTodo(cr, pg); break;
        case Page::Clipboard: PaintClipboardPanels(cr, pg); break;
        case Page::Note: PaintNote(cr, pg); break;
        case Page::Focus: PaintTimers(cr, pg); break;
        case Page::Apps: PaintApps(cr, pg); break;
        case Page::Tray: PaintShelf(cr, pg); break;
        case Page::Mirror: PaintMirror(cr, pg); break;
        case Page::Water: PaintWater(cr, pg); break;
        case Page::ScreenTime: PaintScreenTime(cr, pg); break;
        case Page::Work: PaintWork(cr, pg); break;
        default: PaintHome(cr, pg); break;
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, g_alpha);
    cairo_restore(cr);
    PaintHeader(cr, isl);
    PaintTabBar(cr, isl);
    g_alpha = 1;
}
}  // namespace

bool StudioScrollTimer(double delta, bool fine) {
    if (g_page != Page::Focus) return false;
    if (g_hoverHit < 0 || g_hoverHit >= static_cast<int>(g_hits.size()) || g_hits[g_hoverHit].action != Action::TimerRing) return false;
    static double acc = 0;
    acc += delta;
    if (std::fabs(acc) < 1.0) return true;  // a touchpad's small steps add up
    const int notches = acc < 0 ? 1 : -1;  // up adds time
    acc = 0;
    if (g_hits[g_hoverHit].index == 1) {  // the Focus dial: 5 minutes a notch
        bb::ltimers::PomodoroLength(bb::timers::StepFocusMinutes(bb::ltimers::Config().focusMinutes, notches));
        return true;
    }
    bb::ltimers::CountdownStep(notches, fine);
    return true;
}

void StudioScroll(double delta) {
    if (RichCardShown()) {  // the plan scrolls
        g_planScroll = std::clamp(g_planScroll + delta * 36, 0.0, g_planScrollMax);
        return;
    }
    // A touchpad sends many small steps: one page per swipe, not one per event.
    g_scrollAcc += delta;
    if (std::fabs(g_scrollAcc) < 1.0 || Now() - g_scrollAt < 0.28) return;
    const int step = g_scrollAcc > 0 ? 1 : -1;
    g_scrollAcc = 0;
    g_scrollAt = Now();
    g_tabIdx = std::clamp(g_tabIdx + step, 0, std::max(0, static_cast<int>(g_tabs.size()) - 1));
}

// ── Plan and question cards ─────────────────────────────────────────────────

bool RichCardShown() {
    const AgentView& a = g_agent;
    return (a.phase == AgentPhase::Approval && !a.plan.empty()) || st::NeedsQuestionCard(a);
}

// The question's wrapped height on the card (the text column beside the mascot).
std::string QuestionText(const AgentView::Question& q) {
    return q.text.empty() ? std::string("Has a question for you") : U(q.text);
}
double QuestionTextH(cairo_t* cr, const AgentView::Question& q) {
    return WrapText(cr, QuestionText(q), 0, 0, 440 - 32 - (g_mascotOn ? 44 : 0), FAsk, W(1), false);
}

void RichCardSize(cairo_t* cr, double* w, double* h) {
    const AgentView& a = g_agent;
    if (a.phase == AgentPhase::Approval && !a.plan.empty()) {
        *w = 480;
        *h = 320;
        return;
    }
    g_answers.Sync(a);
    const size_t idx = std::min<size_t>(static_cast<size_t>(std::max(0, g_answers.index)), a.questions.size() - 1);
    const auto& q = a.questions[idx];
    *w = 440;
    *h = st::QuestionCardHeight(QuestionTextH(cr, q), !q.header.empty() || q.multi, st::QuestionRows(q)) +
         (g_compose.kind == ComposeOther ? 10 : 0);
}

void PaintRichCard(cairo_t* cr, const Rect& rect) {
    CheckCompose();
    const AgentView& a = g_agent;
    const bool plan = a.phase == AgentPhase::Approval && !a.plan.empty();
    if (!plan) g_planScrollMax = 0;
    const double L = rect.x + 16, R = rect.x + rect.w - 16, T = rect.y + 12;
    double tx = L;
    if (g_mascotOn) {
        DrawMascot(cr, L + 17, T + 17, 34);
        tx = L + 44;
    }
    SText(cr, U(a.project.empty() ? a.agent : a.agent + L" \x00B7 " + a.project), tx, T, R - 60 - tx, 16, FSmall, C(0x8e8e93));
    const double bh = 30, by = rect.y + rect.h - 12 - bh;

    if (plan) {
        SText(cr, U(a.agent.empty() ? std::wstring(L"Claude") : a.agent) + " has a plan", tx, T + 15, R - tx, 21, FSong, W(1));
        // The plan, scrollable with the wheel.
        if (g_planScrollFor != a.requestId) { g_planScrollFor = a.requestId; g_planScroll = 0; }
        const Rect area{L, T + 46, R - L, by - 10 - (T + 46)};
        Fill(cr, area.x, area.y, area.w, area.h, 12, W(0.06));
        const std::string text = U(st::CleanPlanText(a.plan));
        const double innerW = area.w - 24;
        const double h = WrapText(cr, text, 0, 0, innerW, FAsk, W(1), false);
        const double viewH = area.h - 16;
        g_planScrollMax = std::max(0.0, h - viewH);
        g_planScroll = std::clamp(g_planScroll, 0.0, g_planScrollMax);
        cairo_save(cr);
        cairo_rectangle(cr, area.x, area.y + 6, area.w, area.h - 12);
        cairo_clip(cr);
        WrapText(cr, text, area.x + 12, area.y + 8 - g_planScroll, innerW, FAsk, C(0xe8e8ee));
        cairo_restore(cr);
        if (g_planScrollMax > 0) {
            const double trackH = area.h - 16, thumbH = std::max(18.0, trackH * viewH / h);
            const double ty = area.y + 8 + (trackH - thumbH) * (g_planScroll / g_planScrollMax);
            Fill(cr, area.x + area.w - 6, ty, 3, thumbH, 1.5, W(0.3));
        }
        if (g_compose.kind == ComposePlan) {
            PaintCompose(cr, {L, by, R - L, bh});
            return;
        }
        Button(cr, {L, by, 150, bh}, "Answer in terminal", false, Action::Terminal);
        Button(cr, {R - 228, by, 110, bh}, "Change it\xE2\x80\xA6", false, Action::PlanChange);
        Button(cr, {R - 110, by, 110, bh}, "Approve", true, Action::PlanApprove);
        return;
    }

    // Several questions: one at a time, then send them all together.
    g_answers.Sync(a);
    const int count = static_cast<int>(a.questions.size());
    const int idx = std::clamp(g_answers.index, 0, count - 1);
    const auto& q = a.questions[static_cast<size_t>(idx)];
    if (count > 1) SText(cr, std::to_string(idx + 1) + " of " + std::to_string(count), R - 60, T, 60, 16, FSmall, C(0x8e8e93), 2);
    // The question wraps in full (RichCardSize measured the card to fit), then the
    // hint line, then one row per option.
    const double qH = st::QuestionTextShown(QuestionTextH(cr, q));
    cairo_save(cr);
    cairo_rectangle(cr, tx, rect.y + 27, R - tx, qH + 1);
    cairo_clip(cr);
    WrapText(cr, QuestionText(q), tx, rect.y + 27, R - tx, FAsk, W(1));
    cairo_restore(cr);
    const bool subLine = !q.header.empty() || q.multi;
    if (subLine)
        SText(cr, q.multi ? (q.header.empty() ? std::string("Pick any") : U(q.header) + kDot + "pick any") : U(q.header), tx,
              rect.y + 31 + qH, R - tx, 14, FSmall, C(0x8e8e93));
    double y = rect.y + st::QuestionOptionsTop(qH, subLine);
    const auto& picks = g_answers.picks[static_cast<size_t>(idx)];
    const std::wstring& typed = g_answers.other[static_cast<size_t>(idx)];
    const Col accent = Accent();
    for (size_t i = 0; i < q.options.size() && i < static_cast<size_t>(st::kQuestionRowsMax); ++i) {
        const bool on = std::find(picks.begin(), picks.end(), static_cast<int>(i)) != picks.end();
        const Rect row{L, y, R - L, 26};
        Fill(cr, row.x, row.y, row.w, row.h, 9, W(on ? 0.16 : NextHitHovered() ? 0.10 : 0.05));
        const double cx = row.x + 16, cy = row.y + 13;
        const Col mark = on ? accent : W(0.45);
        if (q.multi) {
            RoundRect(cr, cx - 6, cy - 6, 12, 12, 3);
            Source(cr, mark);
            if (on) cairo_fill(cr);
            else { cairo_set_line_width(cr, 1.4); cairo_stroke(cr); }
        } else if (on) {
            Dot(cr, cx, cy, 6, mark);
        } else {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, 6, 0, 2 * kPi);
            Source(cr, mark);
            cairo_set_line_width(cr, 1.4);
            cairo_stroke(cr);
        }
        if (on) Tick(cr, cx, cy, 0.9, W(1));
        SText(cr, q.options[i], row.x + 30, row.y, row.w - 40, row.h, FBody, W(1));
        g_hits.push_back({row, Action::QuestionToggle, static_cast<int>(i)});
        y += 30;
    }
    if (g_compose.kind == ComposeOther) {
        PaintCompose(cr, {L, by, R - L, bh});
        return;
    }
    if (!typed.empty())
        SText(cr, "Your answer: \xE2\x80\x9C" + U(typed) + "\xE2\x80\x9D", L, by - 18, R - L, 16, FSmall, accent);
    double bx = L;
    if (idx > 0) {
        Button(cr, {bx, by, 70, bh}, "Back", false, Action::QuestionBack);
        bx += 78;
    }
    Button(cr, {bx, by, 86, bh}, "Other\xE2\x80\xA6", false, Action::QuestionOther);
    Button(cr, {R - 210, by, 106, bh}, "In terminal", false, Action::Terminal);
    Button(cr, {R - 96, by, 96, bh}, idx + 1 < count ? "Next" : "Send", true, Action::QuestionNext, -1, g_answers.Answered(idx));
}

// ── Actions ─────────────────────────────────────────────────────────────────

bool StudioDo(const Hit& h) {
    const AgentView& a = g_agent;
    const std::wstring who = a.project.empty() ? a.agent : a.agent + L" \x00B7 " + a.project;
    switch (h.action) {
        case Action::ShelfOpen: bb::lshelf::Open(h.index); return true;
        case Action::ShelfRemove: bb::lshelf::Remove(h.index); return true;
        case Action::ShelfClear: bb::lshelf::Clear(); return true;
        case Action::MirrorToggle:
            if (bb::lshelf::CameraRunning()) bb::lshelf::CameraStop();
            else bb::lshelf::CameraStart([] { Redraw(); });
            return true;
        case Action::Tab:
            g_tabIdx = h.index;
            return true;
        case Action::OpenLink:
            if (h.index >= 0 && h.index < static_cast<int>(g_links.size())) {
                std::string target = U(g_links[static_cast<size_t>(h.index)].second);
                if (target.find("://") == std::string::npos && target[0] != '/' && target[0] != '~') target = "https://" + target;
                if (target[0] == '~') target = bb::plat::HomeDir() + target.substr(1);
                bb::plat::OpenWithDefault(target);
            }
            return true;
        case Action::MediaSource:
            bb::mpris::CycleSource();
            return true;
        case Action::OpenMusic:
            // Bunbun.MusicApp: the app that's playing, or the one you picked. With
            // nothing playing on "auto", the Music page instead.
            if (g_track.active || bb::lfeat::Get().musicApp != L"auto") {
                bb::lfeat::OpenMusicApp();
                return true;
            }
            if (const int i = TabIndexOf(Page::Music); i >= 0) g_tabIdx = i;
            return true;
        case Action::AgentPage:
            if (const int i = TabIndexOf(Page::Agent); i >= 0) g_tabIdx = i;
            return true;
        case Action::ModelCycle:
            CycleClaudeModel();
            return true;
        case Action::PlanApprove:
            bb::agents::Decide(a.requestId, bb::AgentDecision::Allow);
            return true;
        case Action::PlanChange:
            StartCompose(ComposePlan, a.requestId, -1, "What should change in the plan?");
            return true;
        case Action::WaterAdd: bb::lwater::Drink(); return true;
        case Action::WorkStart: bb::lwork::Start(); return true;  // Work mode
        case Action::WorkPause: bb::lwork::Pause(); return true;
        case Action::WorkResume: bb::lwork::Resume(); return true;
        case Action::WorkBreak: bb::lwork::TakeBreak(); return true;
        case Action::WorkDone: bb::lwork::Done(); return true;
        case Action::WorkReport: OpenSettings("work-report"); return true;
        case Action::WorkPage: if (const int i = TabIndexOf(Page::Work); i >= 0) g_tabIdx = i; return true;
        case Action::WaterUndo: bb::lwater::Undo(); return true;
        case Action::WaterRefill: bb::lwater::Refill(); return true;
        case Action::QuestionToggle:
            g_answers.Toggle(a, h.index);
            // One single-choice question: the pick is the answer.
            if (st::AnswersOnPick(a) && g_answers.Answered(0))
                if (const std::string reply = st::QuestionsReply(a, g_answers); !reply.empty()) bb::agents::DecideRaw(a.requestId, reply);
            return true;
        case Action::QuestionNext:
            g_answers.Sync(a);
            if (!g_answers.Answered(g_answers.index)) return true;
            if (g_answers.index + 1 < static_cast<int>(a.questions.size())) {
                ++g_answers.index;
            } else if (const std::string reply = st::QuestionsReply(a, g_answers); !reply.empty()) {
                bb::agents::DecideRaw(a.requestId, reply);
            }
            return true;
        case Action::QuestionBack:
            if (g_answers.index > 0) --g_answers.index;
            return true;
        case Action::QuestionOther:
            g_answers.Sync(a);
            StartCompose(ComposeOther, a.requestId, g_answers.index, "Your answer");
            return true;
        case Action::Reply:
            if (a.replyId) StartCompose(ComposeReply, a.replyId, -1, "Reply to " + U(a.replyTo.empty() ? who : a.replyTo));
            return true;
        case Action::ComposeSend:
            SendCompose();
            return true;
        case Action::ComposeCancel:
            EndCompose();
            return true;
        case Action::ClipCopy: lsrc::CopyClip(h.index); return true;
        case Action::ClipClear: lsrc::ClearClips(); return true;
        case Action::NoteToggle: lsrc::NoteToggle(h.index); return true;
        case Action::NoteEdit: OpenSettings("notes"); return true;  // Settings' Notes page (it was the text editor)
        case Action::TodoToggle: lsrc::TodoToggle(h.index); return true;
        case Action::TodoOpen: OpenSettings("todos"); return true;
        case Action::FocusStart:
            if (h.index < 0) {  // a preset: set the picker
                lsrc::FocusState().pickH = -h.index / 60;
                lsrc::FocusState().pickM = -h.index % 60;
            } else {
                lsrc::FocusStart(h.index);
            }
            return true;
        case Action::FocusPause: lsrc::FocusPause(); return true;
        case Action::FocusAdd: lsrc::FocusAdd(h.index); return true;
        case Action::FocusStop: lsrc::FocusStop(); return true;
        case Action::FocusPick: {
            lsrc::Focus& f = lsrc::FocusState();
            const int dir = h.index % 2 ? 1 : -1;
            if (h.index / 2 == 0) f.pickH = (f.pickH + dir + 6) % 6;
            else f.pickM = (f.pickM + dir * 5 + 60) % 60;
            if (f.pickH == 0 && f.pickM == 0) f.pickM = 5;
            return true;
        }
        case Action::Close: CloseIsland(); return true;
        case Action::ClipFilter: g_clipFilter = h.index; g_clipSelected = 0; return true;
        case Action::ClipSelect: g_clipSelected = h.index; return true;
        case Action::ClipDelete: lsrc::RemoveClip(h.index); return true;
        case Action::ScreenTimeDay: bb::lscreen::ShowDay(h.index); return true;
        case Action::ScreenTimeLimit: {  // none, 2 h, 4 h ... 12 h, none; none is the default, written as unset
            static const int kSteps[] = {0, 2, 4, 6, 8, 10, 12};
            int next = 0;
            for (size_t k = 0; k + 1 < std::size(kSteps); ++k)
                if (bb::lscreen::LimitHours() == kSteps[k]) next = kSteps[k + 1];
            const std::wstring v = std::to_wstring(next);
            bb::settings::Set(L"Bunbun.ScreenTimeLimit", next ? &v : nullptr);
            return true;
        }
        case Action::VolumeMute: bb::lfeat::ToggleMute(); return true;
        case Action::VolumeSet:  // where on the slider it was clicked
            if (h.r.w > 0) bb::lfeat::SetVolume(static_cast<int>(std::clamp((g_mouseX - h.r.x) / h.r.w, 0.0, 1.0) * 100 + 0.5));
            return true;
        case Action::TimerMode: bb::ltimers::SetMode(static_cast<bb::timers::Mode>(std::clamp(h.index, 0, bb::timers::kModeCount - 1))); return true;
        case Action::PomoToggle: bb::ltimers::PomodoroToggle(); return true;
        case Action::PomoSkip: bb::ltimers::PomodoroSkip(); return true;
        case Action::PomoReset: bb::ltimers::PomodoroReset(); return true;
        case Action::PomoAuto: bb::ltimers::PomodoroAuto(); return true;
        case Action::PomoLength: bb::ltimers::PomodoroLength(h.index); return true;
        case Action::CountToggle: bb::ltimers::CountdownToggle(); return true;
        case Action::CountAdd: bb::ltimers::CountdownAdd(h.index); return true;
        case Action::CountReset: bb::ltimers::CountdownReset(); return true;
        case Action::CountPreset: bb::ltimers::CountdownSet(h.index); return true;
        case Action::CountSave: bb::ltimers::SavePreset(); return true;
        case Action::TimerRing: {  // a click on the dial: straight up is 0, clockwise, one turn an hour
            const double angle = std::atan2(g_mouseX - g_timerRing.x, g_timerRing.y - g_mouseY);
            if (h.index == 1) bb::ltimers::PomodoroLength(bb::timers::FocusMinutesFromAngle(angle));  // the Focus dial
            else bb::ltimers::CountdownSet(bb::timers::SecondsFromAngle(angle));
            return true;
        }
        case Action::SwToggle: bb::ltimers::StopwatchToggle(); return true;
        case Action::SwLap: bb::ltimers::StopwatchLap(); return true;
        case Action::SwReset: bb::ltimers::StopwatchReset(); return true;
        case Action::SwCopy: lsrc::CopyText(bb::ltimers::LapsText()); return true;
        case Action::WeatherUnit: {  // Modules.WeatherFahrenheit; the default (off) is written as unset
            static const std::wstring on = L"true";
            bb::settings::Set(L"Modules.WeatherFahrenheit", lsrc::CurrentWeather().fahrenheit ? nullptr : &on);
            return true;
        }
        case Action::WeatherCity: OpenSettings("tools"); return true;  // Page options: the weather city
        case Action::RadioToggle: {
            const lsrc::Radios& r = lsrc::RadioState();
            if (h.index == 0 && r.wifiKnown) lsrc::SetWifi(!r.wifi);
            else if (h.index == 1 && r.btKnown) lsrc::SetBluetooth(!r.bt);
            return true;
        }
        case Action::RadioSettings: {  // pick a network or a device in the desktop's own settings
            const bool bt = h.index == 1;
            if (!bb::plat::Spawn({"gnome-control-center", bt ? "bluetooth" : "wifi"}) &&
                !bb::plat::Spawn({"systemsettings", bt ? "kcm_bluetooth" : "kcm_networkmanagement"}))
                bb::plat::Spawn({bt ? "blueman-manager" : "nm-connection-editor"});
            return true;
        }
        case Action::Radio: {
            const lsrc::Radios& r = lsrc::RadioState();
            if (h.index == 0) lsrc::SetWifi(!r.wifi);
            else if (h.index == 1) lsrc::SetBluetooth(!r.bt);
            else lsrc::SetDnd(!r.dnd);
            return true;
        }
        case Action::OpenPage:
            g_tabIdx = h.index;
            return true;
        default:
            return false;
    }
}

// ── Typing ──────────────────────────────────────────────────────────────────

bool Composing() { return g_compose.kind != ComposeNone; }

bool ComposeKey(GdkEventKey* e) {
    if (!Composing()) return false;
    g_compose.since = Now();  // the caret stays lit while typing
    if (g_im && gtk_im_context_filter_keypress(g_im, e)) return true;
    const bool ctrl = (e->state & GDK_CONTROL_MASK) != 0;
    switch (e->keyval) {
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter: SendCompose(); return true;
        case GDK_KEY_Escape: EndCompose(); return true;
        case GDK_KEY_BackSpace: {
            std::string& t = g_compose.text;
            if (ctrl) { t.clear(); return true; }
            while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();  // UTF-8 continuation bytes
            if (!t.empty()) t.pop_back();
            return true;
        }
        default: break;
    }
    if (ctrl && (e->keyval == GDK_KEY_v || e->keyval == GDK_KEY_V)) {
        if (gchar* clip = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD))) {
            std::string text = clip;
            g_free(clip);
            std::replace(text.begin(), text.end(), '\n', ' ');
            g_compose.text += text;
        }
        return true;
    }
    // No input method running: take the key's own character.
    const guint32 cp = gdk_keyval_to_unicode(e->keyval);
    if (cp >= 0x20 && !ctrl) {
        char buf[8] = {};
        g_unichar_to_utf8(cp, buf);
        g_compose.text += buf;
    }
    return true;
}

void ComposeFocusOut() {
    if (g_im) gtk_im_context_focus_out(g_im);
}

}  // namespace bb::lisland
