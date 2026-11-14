// Bunbun — the island on Linux (GTK 3 + Cairo/Pango).
//
// One transparent window at the top of the screen holds the pill; its input
// region is the pill itself, so clicks around it reach the windows below.
//   - Wayland with layer-shell (KDE Plasma, Sway, Hyprland, …): a layer
//     surface anchored to the top edge (gtk-layer-shell).
//   - X11 (and GNOME on Wayland, which has no layer-shell, through XWayland):
//     an always-on-top dock window placed at the top of the work area.
// It shows the clock, what's playing (MPRIS), the mascot, and coding agents
// (the same agent bridge as on Windows, over a Unix socket), with Allow / Deny
// and question answers. With the Studio layout (the default) the island opens
// into the Studio frame on hover; that lives in studio_linux.cpp. See
// docs/linux.md for what isn't ported yet.
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <pango/pangocairo.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <cstring>
#include <ctime>
#include <mutex>
#include <cwchar>
#include <string>
#include <vector>

#include "../../agents/agent_diff.h"  // the approval card's diff lines, shared with Windows
#include "../../agents/agent_view.h"
#include "../../agents/hook_installer.h"
#include "compat/d2d1.h"
#include "../../mascot/mascot.h"
#include "island_api.h"
#include "../../common/applist.h"
#include "island_linux.h"
#include "../../island/studio_pages.h"
#include "mascot_png.h"
#include "mpris.h"
#include "platform.h"
#include "settings.h"
#include "calendar_linux.h"
#include "edgelight_linux.h"
#include "timers_linux.h"
#include "screentime_linux.h"
#include "../../common/edgelight.h"
#include "features_linux.h"
#include "picker_linux.h"
#include "shelf_linux.h"
#include "tray_linux.h"
#include "look_linux.h"
#include "lyrics_linux.h"
#include "sources.h"
#include "water_linux.h"
#include "work_linux.h"

namespace bb::lisland {

using bb::AgentPhase;
using bb::AgentView;
namespace mascot = bb::mascot;

// ── Layout (logical pixels; GTK scales for HiDPI) ───────────────────────────

// The window; the pill sits inside it. Room for the Studio frame (520 x 256) and
// the plan card (480 x 320), plus the nudge and the shadow.
constexpr double kCanvasW = 560, kCanvasH = 340;  // room beside the open island for the notch's ears
constexpr double kTopGap = 6;                       // between the screen edge and the pill
constexpr double kCollapsedH = 36;
constexpr double kOpenDelay = 0.15;                 // the pointer rests this long before Studio opens

// ── State (all on the GTK main thread, except the agent mailbox) ────────────

GtkWidget* g_window = nullptr;
bool g_layerShell = false;
double g_scale = 1.0;       // what everything is drawn at: g_baseScale, times the screen's share
double g_baseScale = 1.0;   // Appearance.SizeScale
bool g_matchScreen = false; // Appearance.MatchScreenSize
double g_offsetY = 0;       // Appearance.OffsetY
bool g_mascotOn = true;     // Bunbun.Mascot and MascotOnIsland
bool g_agentsOn = true;     // Bunbun.Agents
bool g_agentPill = true;    // Bunbun.AgentPill: the pill while an agent works (cards always show)
bool g_mediaOn = true;      // Modules.Media: what's playing
bool g_streamingMood = true;  // Bunbun.StreamingMood: a video shows the popcorn mascot and "Streaming"
bool g_liveMood = true;       // Bunbun.LiveMood: the Live mood while the screen is shared or recorded
std::wstring g_musicApps, g_videoApps;  // Bunbun.MusicApps / VideoApps: overrides for IsVideoTrack
bool g_expandOnHover = true;  // Behavior.ExpandOnHover; off: a click opens and closes the island
bool g_pinned = false;      // opened by a click (ExpandOnHover off)
bool g_hoverHold = false;   // closed with the header's button: no hover-open until the pointer leaves
std::string g_toastTitle, g_toastText;  // a reminder or the focus timer, until g_toastUntil
int g_toastWater = 0;  // the toast is a water reminder: 1 = drink, 2 = refill (a click logs it)
int g_statusRing = 1;  // Bunbun.StatusRing: 0 off, 1 auto, 2 water, 3 tasks, 4 focus
bool StatusRingNow(double* frac, double rgb[3]);  // see PaintClock
double g_toastUntil = 0;
double g_toastStart = 0, g_toastSeconds = 0;
double g_toastLevel = -1;   // 0..1 draws a level bar (volume, brightness); < 0 none
double g_vw = 0, g_vh = 0;  // the resize spring's velocities
double g_yOff = 0, g_vy = 0;  // Appearance.OffsetYExpanded: the pill's extra drop while open
double g_artChangedAt = -10;  // Animations.PillCoverSpin
double g_trackChangedAt = -10;  // Animations.ExpandedMediaTransitions
double g_expandUntil = 0;   // Modules.MediaAutoExpand: open until then after a new song
double g_swipeAt = -10;     // Bunbun.MusicSwipe: one skip per gesture
int g_shelfPress = -1;      // the shelf item pressed (opens on release, drags on a move)
double g_shelfPressX = 0, g_shelfPressY = 0;
int g_shelfDrag = -1;       // the shelf item being dragged out
std::string g_lastSong;
bool g_clock24 = true;
bool g_showSeconds = false;

double g_w = 120, g_h = kCollapsedH;  // the pill's current size (animated)
double g_lastFrame = 0;
bool g_hover = false;
double g_hoverSince = 0;
double g_mouseX = -1, g_mouseY = -1;
std::vector<Hit> g_hits;
int g_hoverHit = -1;
guint g_timer = 0;
double g_lastDrawAt = 0;  // the watchdog in main() remaps the window if frames stop

bb::mpris::Track g_track;
GdkPixbuf* g_art = nullptr;
std::string g_artUrl;
GCancellable* g_artCancel = nullptr;

std::mutex g_agentMutex;
AgentView g_agentInbox;
bool g_agentInboxNew = false;
bool g_agentInboxNudge = false;
AgentView g_agent;
double g_nudgeAt = -10;

mascot::Mood g_mood = mascot::Mood::Idle;
double g_pokeUntil = 0;  // clicking the mascot makes it hop
double g_moodSince = 0;
ID2D1RenderTarget* g_rt = nullptr;  // the mascot draws through the Direct2D stand-ins

double Now() { return g_get_monotonic_time() / 1e6; }

void ScheduleFrame(bool fast);
int g_fastMs = 16;  // the fast frame interval (bb::lfeat::FrameMs)

// Appearance.Displays = all: a copy of the island on every other monitor, painted
// from the main island's last frame. Each sits per Appearance.MirrorPosition, or its
// own entry in Appearance.DisplayPositions ("2=bottom-center;3=top-left").
// Appearance.DisplayLink, as on Windows: Linked mirrors take the pointer and hand
// it to the island (so pointing at any opens them all); Separate, the island
// follows the pointer's display and the mirrors show it at rest.
struct Mirror {
    GtkWidget* window = nullptr;
    int monitor = 0;     // 0-based
    int align = 1;       // 0 left, 1 centre, 2 right
    bool bottom = false;
    double px = 0, py = 0;           // where its pill is drawn, canvas units
    cairo_rectangle_int_t input{};   // its input region now (pixels)
    int shape = -1;                  // Separate: its display's shape (bb::llook::Shape), -1 the island's
    double size = 1.0;               // Separate: its display's size, relative to Size scale
};
// "2=pill;3=notch" lists by display number (Appearance.DisplayShapes / DisplaySizes).
std::wstring PerDisplay(const std::wstring& list, int index1) {
    const std::wstring key = std::to_wstring(index1) + L"=";
    const size_t at = list.find(key);
    if (at == std::wstring::npos || (at && list[at - 1] != L';' && list[at - 1] != L' ')) return {};
    const size_t end = list.find(L';', at);
    return list.substr(at + key.size(), end == std::wstring::npos ? std::wstring::npos : end - at - key.size());
}
int DisplayShapeOf(int index1) {  // -1: the island's own
    const std::wstring v = PerDisplay(bb::settings::Get(L"Appearance.DisplayShapes"), index1);
    return v == L"notch" ? static_cast<int>(bb::llook::Shape::Notch) : v == L"w11" ? static_cast<int>(bb::llook::Shape::W11)
         : v == L"pill" ? static_cast<int>(bb::llook::Shape::Pill) : -1;
}
double DisplaySizeOf(int index1) {
    const double v = wcstod(PerDisplay(bb::settings::Get(L"Appearance.DisplaySizes"), index1).c_str(), nullptr);
    return v >= 0.5 && v <= 2.5 ? v : 1.0;
}
double ScreenShare(GdkMonitor* mon);  // below
cairo_pattern_t* g_restFrame = nullptr;  // Separate: the island's last frame at rest
std::vector<Mirror> g_mirrors;
cairo_pattern_t* g_frame = nullptr;  // the main island's last frame (canvas units)
struct { double x = 0, y = 0, w = 0, h = 0; } g_framePill;  // where the pill was in it
decltype(g_framePill) g_restPill;  // Separate: where the pill was in g_restFrame
void RebuildMirrors();

// ── Settings ────────────────────────────────────────────────────────────────

void LoadSettings() {
    namespace s = bb::settings;
    s::Invalidate();
    g_baseScale = std::clamp(s::GetDouble(L"Appearance.SizeScale"), 0.6, 2.5);
    if (g_baseScale <= 0) g_baseScale = 1.0;
    g_matchScreen = s::GetBool(L"Appearance.MatchScreenSize");
    g_scale = g_baseScale;  // PlaceWindow adds the screen's share
    g_offsetY = s::GetInt(L"Appearance.OffsetY");
    {
        const std::wstring ring = s::Get(L"Bunbun.StatusRing");
        g_statusRing = ring == L"off" ? 0 : ring == L"water" ? 2 : ring == L"tasks" ? 3 : ring == L"focus" ? 4 : 1;
    }
    bb::llook::LoadSettings();  // shape, theme, colours, font, sounds, privacy dots
    bb::lfeat::LoadSettings();  // placement, hiding, motion, alerts, the date, eyes, lyrics
    bb::ledge::LoadSettings();  // the edge light
    bb::ltimers::LoadSettings();  // Pomodoro lengths, presets, Water in Timers
    bb::lscreen::LoadSettings();  // Screen Time and its limit
    bb::lcal::LoadSettings();   // calendar links and the meeting alert
    bb::llook::SetEdge(bb::lfeat::Get().bottom, bb::lfeat::Get().merged);
    g_mascotOn = s::GetBool(L"Bunbun.Mascot") && s::GetBool(L"Bunbun.MascotOnIsland");
    bb::lwork::LoadSettings();  // Bunbun.UseCase: office work hides every agent piece
    g_agentsOn = s::GetBool(L"Bunbun.Agents") && bb::lwork::AgentsOn();
    g_agentPill = s::GetBool(L"Bunbun.AgentPill");
    g_mediaOn = s::GetBool(L"Modules.Media");
    g_streamingMood = s::GetBool(L"Bunbun.StreamingMood");
    g_musicApps = s::Get(L"Bunbun.MusicApps");
    g_videoApps = s::Get(L"Bunbun.VideoApps");
    g_liveMood = s::GetBool(L"Bunbun.LiveMood");
    g_expandOnHover = s::GetBool(L"Behavior.ExpandOnHover");
    if (g_expandOnHover) g_pinned = false;
    g_showSeconds = s::GetBool(L"Modules.ShowSeconds");
    const std::wstring fmt = s::Get(L"Modules.ClockFormat");
    if (fmt == L"12h") g_clock24 = false;
    else if (fmt == L"24h") g_clock24 = true;
    else {
        // "system": the locale's own time format says whether it uses AM/PM.
        char buf[16] = {};
        tm t{};
        t.tm_hour = 15;
        strftime(buf, sizeof buf, "%X", &t);
        g_clock24 = std::string(buf).find("15") != std::string::npos;
    }
    mascot::SetStyle(mascot::StyleFromSettings(s::Get(L"Bunbun.Character"), s::Get(L"Bunbun.MascotColor"),
                                               s::GetInt(L"Bunbun.MascotMotion"), s::GetInt(L"Bunbun.MascotHop"),
                                               s::GetInt(L"Bunbun.MascotBlinkEvery") * 10,
                                               s::GetBool(L"Bunbun.MascotEyesFollow"), s::GetBool(L"Bunbun.MascotCelebrate")));
    StudioLoadSettings();
    bb::llyrics::LoadSettings();
    bb::llyrics::OnTrack(g_track);  // lyrics just switched on: look this song up
}

// ── Drawing helpers ─────────────────────────────────────────────────────────

void RoundRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    r = std::min({r, w / 2, h / 2});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

double Text(cairo_t* cr, const std::string& text, double x, double y, double maxW, double size, bool bold,
            double r, double g, double b, double a, bool mono, int lines) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string(mono ? "Monospace" : bb::llook::Font());
    pango_font_description_set_absolute_size(font, bb::llook::TextSize(size) * PANGO_SCALE);
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), -1);
    if (maxW > 0) {
        pango_layout_set_width(layout, static_cast<int>(maxW * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        if (lines > 1) {
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
            pango_layout_set_height(layout, -lines);
        }
    }
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    bb::llook::Ink(&r, &g, &b, &a);  // "white" is the theme's text colour
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
    return w;
}

double TextWidth(cairo_t* cr, const std::string& text, double size, bool bold) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string(bb::llook::Font());
    pango_font_description_set_absolute_size(font, bb::llook::TextSize(size) * PANGO_SCALE);
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), -1);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    pango_font_description_free(font);
    g_object_unref(layout);
    return w;
}

// Is the player showing a video rather than music? Same rule as Windows'
// IsVideoMedia: music apps are music; browsers report everything alike, so there a
// track with an album is music (YouTube Music, Spotify on the web) and one without
// is a video; known video players are video.
bool IsVideoTrack(const bb::mpris::Track& t) {
    std::string id = t.player;
    for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::wstring wid = bb::plat::Wide(id);
    if (bb::applist::Matches(g_videoApps, wid)) return true;   // your own lists first
    if (bb::applist::Matches(g_musicApps, wid)) return false;
    for (const char* m : {"spotify", "rhythmbox", "elisa", "lollypop", "amarok", "clementine", "strawberry", "audacious",
                          "deadbeef", "cmus", "quod libet", "youtube music", "youtube-music", "tidal", "deezer", "amberol",
                          "g4music", "tauon", "musikcube", "cider", "apple music"})
        if (id.find(m) != std::string::npos) return false;
    for (const char* b : {"firefox", "chrom", "brave", "edge", "vivaldi", "opera", "librewolf", "zen", "epiphany", "web"})
        if (id.find(b) != std::string::npos) return t.album.empty();
    for (const char* v : {"vlc", "mpv", "celluloid", "totem", "videos", "smplayer", "kodi", "stremio", "haruna", "dragon", "parole"})
        if (id.find(v) != std::string::npos) return true;
    return false;
}

// A video plays: the small island shows the mascot munching popcorn and "Streaming".
bool StreamingNow() { return g_streamingMood && g_mediaOn && g_track.active && IsVideoTrack(g_track); }

void DrawMascot(cairo_t* cr, double cx, double cy, double size) {
    if (!g_mascotOn) return;
    cairo_save(cr);
    bbcompat::SetCairo(g_rt, cr);
    // Eyes follow the pointer while it's over the island, else glance ahead.
    D2D1_POINT_2F look{0, 0};
    if (g_hover && g_mouseX >= 0) {
        look.x = static_cast<float>(std::clamp((g_mouseX - cx) / 80.0, -1.0, 1.0));
        look.y = static_cast<float>(std::clamp((g_mouseY - cy) / 40.0, -1.0, 1.0));
    }
    mascot::Draw(g_rt, D2D1::Point2F(static_cast<float>(cx), static_cast<float>(cy)), static_cast<float>(size), g_mood,
                 Now(), g_moodSince, look);
    cairo_restore(cr);
}

void Button(cairo_t* cr, const Rect& r, const std::string& label, bool primary, Action action, int index = -1) {
    const bool hovered = static_cast<int>(g_hits.size()) == g_hoverHit;
    RoundRect(cr, r.x, r.y, r.w, r.h, 9);
    if (primary) cairo_set_source_rgba(cr, 0.47, 0.78, 1.0, hovered ? 1.0 : 0.85);
    else cairo_set_source_rgba(cr, 1, 1, 1, hovered ? 0.22 : 0.13);
    cairo_fill(cr);
    const double tw = TextWidth(cr, label, 12, true);
    const double ink = primary ? 0.07 : 1.0;
    Text(cr, label, r.x + std::max(6.0, (r.w - tw) / 2), r.y + (r.h - 15) / 2, r.w - 12, 12, true, ink, ink, ink + (primary ? 0.01 : 0), 0.95);
    g_hits.push_back({r, action, index});
}

// Media control glyphs, drawn rather than taken from a font.
void Glyph(cairo_t* cr, Action a, double cx, double cy, double s, bool playing) {
    const auto& ink = bb::llook::Get().ink;
    cairo_set_source_rgba(cr, ink[0], ink[1], ink[2], 0.92);
    if (a == Action::PlayPause && playing) {
        cairo_rectangle(cr, cx - s * 0.45, cy - s * 0.5, s * 0.3, s);
        cairo_rectangle(cr, cx + s * 0.15, cy - s * 0.5, s * 0.3, s);
        cairo_fill(cr);
        return;
    }
    const double dir = a == Action::Previous ? -1 : 1;
    const auto tri = [&](double x0) {
        cairo_move_to(cr, x0 - dir * s * 0.35, cy - s * 0.5);
        cairo_line_to(cr, x0 + dir * s * 0.45, cy);
        cairo_line_to(cr, x0 - dir * s * 0.35, cy + s * 0.5);
        cairo_close_path(cr);
    };
    if (a == Action::PlayPause) tri(cx);
    else { tri(cx - dir * s * 0.3); tri(cx + dir * s * 0.45); }
    cairo_fill(cr);
}

void IconButton(cairo_t* cr, double cx, double cy, double size, Action a, bool playing) {
    const Rect r{cx - size / 2, cy - size / 2, size, size};
    if (static_cast<int>(g_hits.size()) == g_hoverHit) {
        cairo_arc(cr, cx, cy, size / 2, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
        cairo_fill(cr);
    }
    Glyph(cr, a, cx, cy, size * 0.36, playing);
    g_hits.push_back({r, a});
}

void Art(cairo_t* cr, double x, double y, double size, double alpha) {
    if (!g_art || alpha <= 0) return;
    cairo_save(cr);
    // Animations.PillCoverSpin: a new cover spins in on the small island.
    const double spin = Now() - g_artChangedAt;
    if (bb::lfeat::Get().coverSpin && size < 40 && spin < 0.6) {
        const double e = 1.0 - std::pow(1.0 - spin / 0.6, 3);
        cairo_translate(cr, x + size / 2, y + size / 2);
        cairo_rotate(cr, (1.0 - e) * -2 * M_PI);
        cairo_translate(cr, -(x + size / 2), -(y + size / 2));
    }
    RoundRect(cr, x, y, size, size, size * 0.22);
    cairo_clip(cr);
    const double k = size / std::max(gdk_pixbuf_get_width(g_art), gdk_pixbuf_get_height(g_art));
    cairo_translate(cr, x, y);
    cairo_scale(cr, k, k);
    gdk_cairo_set_source_pixbuf(cr, g_art, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

// Bars that bob while music plays, in Appearance.SpectrumStyle. Not driven by the
// audio yet (see docs/linux.md).
void Bars(cairo_t* cr, double x, double cy, double h) {
    static const double kBlue[3] = {0.47, 0.78, 1.0};
    bb::lfeat::Spectrum(cr, x, cy, 20, h, kBlue, 0.9, g_track.playing, Now());
}

std::string ClockText() {
    const time_t now = time(nullptr);
    tm lt{};
    localtime_r(&now, &lt);
    char buf[32];
    if (g_clock24) strftime(buf, sizeof buf, g_showSeconds ? "%H:%M:%S" : "%H:%M", &lt);
    else strftime(buf, sizeof buf, g_showSeconds ? "%l:%M:%S %p" : "%l:%M %p", &lt);
    std::string s = buf;
    while (!s.empty() && s[0] == ' ') s.erase(0, 1);
    return s;
}

std::string AgentLine(const AgentView& a) {
    std::string who = bb::plat::Utf8(a.agent.empty() ? L"Agent" : a.agent);
    if (!a.project.empty()) who += " \xC2\xB7 " + bb::plat::Utf8(a.project);
    return who;
}

// ── What to show ────────────────────────────────────────────────────────────

enum class Kind { Clock, Media, Agent, AgentCard, Studio, RichCard, Toast, Hidden, Tuck, Meeting, Color };

// The colour just picked (the colour picker): a card with HEX, RGB and HSL; a
// click on a row copies that form. It stays a while, or until you click elsewhere.
struct PickedColour { uint8_t r = 0, g = 0, b = 0; };
PickedColour g_color;
double g_colorUntil = 0;
int g_colorCopied = 0;       // the row copied last (the hex is copied on picking)
double g_colorCopiedAt = 0;

std::string ColorRow(int row) {
    char buf[64];
    const auto& c = g_color;
    if (row == 0) {
        snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
    } else if (row == 1) {
        snprintf(buf, sizeof buf, "rgb(%d, %d, %d)", c.r, c.g, c.b);
    } else {
        const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
        const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), l = (mx + mn) / 2, d = mx - mn;
        double h = 0, s = 0;
        if (d > 1e-9) {
            s = d / (1 - std::fabs(2 * l - 1));
            h = mx == r ? std::fmod((g - b) / d, 6.0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
            h *= 60;
            if (h < 0) h += 360;
        }
        snprintf(buf, sizeof buf, "hsl(%d, %d%%, %d%%)", static_cast<int>(std::lround(h)), static_cast<int>(std::lround(s * 100)),
                 static_cast<int>(std::lround(l * 100)));
    }
    return buf;
}

void CopyColorRow(int row) {
    bb::lsrc::CopyText(ColorRow(row));
    g_colorCopied = row;
    g_colorCopiedAt = Now();
    g_colorUntil = std::max(g_colorUntil, Now() + 6);
}

void PickColour() {
    bb::lpick::Start([](uint8_t r, uint8_t g, uint8_t b) {
        g_color = {r, g, b};
        g_colorUntil = Now() + 12;
        CopyColorRow(0);  // the hex is on the clipboard straight away
        Redraw();
    });
}

// A new song opened the island for a moment (Modules.MediaAutoExpand).
bool AutoExpanding() { return g_track.playing && Now() < g_expandUntil; }

// Separate displays: the island is folded only because a full-screen app shares its
// display. The other displays keep showing it at rest (OnMirrorDraw).
bool g_fsFolded = false;

// GDK index of the display the island is on now.
int IslandMonitorIndex() {
    if (!g_window) return -1;
    GdkDisplay* display = gtk_widget_get_display(g_window);
    GdkMonitor* mon = bb::lfeat::TargetMonitor(display);
    for (int i = 0; i < gdk_display_get_n_monitors(display); ++i)
        if (gdk_display_get_monitor(display, i) == mon) return i;
    return -1;
}

Kind CurrentKind() {
    g_fsFolded = false;
    Kind k = Kind::Clock;
    const bool toast = Now() < g_toastUntil && !Composing() && !g_hover;
    const auto& f = bb::lfeat::Get();
    const bool needsYou = g_agentsOn && (g_agent.phase == AgentPhase::Approval || g_agent.phase == AgentPhase::Question);
    if (!needsYou && Now() < g_colorUntil) return Kind::Color;
    if (!needsYou && !Composing()) {
        // Hidden with the hotkey; tucked into a line (by hand, or in full screen);
        // hidden in full screen; hidden after a while without anything happening.
        // Pointing at a tucked line for a moment brings the island back.
        bool fullscreen = bb::lfeat::FullscreenAppActive();
        // Separate displays: a full-screen app on another display folds that display's island, not this one.
        if (fullscreen && bb::lfeat::SeparateDisplays() && bb::lfeat::FullscreenMonitor() >= 0 &&
            bb::lfeat::FullscreenMonitor() != IslandMonitorIndex())
            fullscreen = false;
        const bool reveal = g_hover && Now() - g_hoverSince > f.tuckRevealMs / 1000.0;
        if (bb::lfeat::HiddenByHotkey()) return Kind::Hidden;
        if (bb::lfeat::Tucked() && reveal) bb::lfeat::SetTucked(false);
        if (bb::lfeat::Tucked()) return Kind::Tuck;
        if (fullscreen && bb::lfeat::FullscreenTuckNow() && !reveal) { g_fsFolded = bb::lfeat::SeparateDisplays(); return Kind::Tuck; }
        if (fullscreen && !bb::lfeat::FullscreenTuckNow() && !(f.unhideOnHover && g_hover)) {
            g_fsFolded = bb::lfeat::SeparateDisplays();
            return Kind::Hidden;
        }
        if (bb::lfeat::AutoHidden() && !toast && !(f.unhideOnHover && g_hover)) return Kind::Hidden;
    }
    if (g_agentsOn && g_agent.phase != AgentPhase::None) {
        if (g_agent.phase == AgentPhase::Approval || g_agent.phase == AgentPhase::Question)
            return RichCardShown() ? Kind::RichCard : Kind::AgentCard;
        if (g_agentPill) k = Kind::Agent;
    }
    if (toast) return Kind::Toast;
    // A meeting about to start (Modules.MeetingAlertMinutes), over the clock and music.
    if (k == Kind::Clock && bb::lcal::PillWanted(static_cast<double>(time(nullptr))) && !g_hover) return Kind::Meeting;
    if (k == Kind::Clock && g_track.active && g_mediaOn) k = Kind::Media;
    // Studio: the frame opens while the pointer rests on the island, and stays
    // open while you type a reply into it.
    const bool open = g_pinned || (g_expandOnHover && g_hover && !g_hoverHold && Now() - g_hoverSince >= kOpenDelay);
    if (StudioLayout() && (Composing() || open || (k == Kind::Media && AutoExpanding()))) return Kind::Studio;
    return k;
}

mascot::Mood MoodFor(Kind k) {
    // A glass of water logged: drinking it, then the goal's cheer (as on Windows).
    if (Now() - bb::lwater::DrankAt() < 2.6) return mascot::Mood::Drinking;
    if (Now() >= bb::lwater::GoalAt() && Now() - bb::lwater::GoalAt() < 3.0) return mascot::Mood::Happy;
    const bool agentShown = g_agentsOn && g_agent.phase != AgentPhase::None;
    const bool needsYou = g_agentsOn && (g_agent.phase == AgentPhase::Approval || g_agent.phase == AgentPhase::Question);
    // Away or asleep wins in every state (music, agents at work, Work mode); only an
    // agent waiting for your answer still shows (as on Windows).
    if (!needsYou) {
        const auto& ff = bb::lfeat::Get();
        const double idle = bb::lfeat::IdleSeconds();
        if (ff.sleepMinutes > 0 && idle >= ff.sleepMinutes * 60.0) return mascot::Mood::Sleepy;
        if (ff.afkMinutes > 0 && idle >= ff.afkMinutes * 60.0) return mascot::Mood::Snack;
    }
    // Work mode when no agent is on show: at the laptop while working, a snack at lunch.
    // Not over Do Not Disturb you chose (as on Windows).
    const bool dndChosen = bb::lfeat::Get().mascotDnd ||
                           (bb::lfeat::Get().dndFollowSystem && bb::lsrc::RadioState().dndKnown && bb::lsrc::RadioState().dnd);
    if (!agentShown && !dndChosen && bb::lwork::On()) {
        const bb::work::Phase wp = bb::lwork::Now().phase;
        if (wp == bb::work::Phase::Working || wp == bb::work::Phase::Overtime) return mascot::Mood::Working;
        if (wp == bb::work::Phase::Lunch) return mascot::Mood::Snack;
    }
    // An agent at work shows on the mascot even with the agent pill off.
    if (k == Kind::Agent || k == Kind::AgentCard || k == Kind::RichCard || agentShown) {
        switch (g_agent.phase) {
            case AgentPhase::Thinking: return mascot::Mood::Thinking;
            case AgentPhase::Working: return mascot::Mood::Working;
            case AgentPhase::Approval:
            case AgentPhase::Question: return mascot::Mood::Alert;
            case AgentPhase::Finished: return mascot::Mood::Happy;
            case AgentPhase::Error: return mascot::Mood::Sad;
            default: break;
        }
    }
    if (g_liveMood && bb::llook::ScreenInUse()) return mascot::Mood::Live;  // streaming / sharing / recording
    if ((k == Kind::Media || k == Kind::Studio) && g_track.playing)
        return StreamingNow() ? mascot::Mood::Streaming : mascot::Mood::Music;
    const auto& feat = bb::lfeat::Get();
    const auto& radios = bb::lsrc::RadioState();
    if (feat.mascotDnd || (feat.dndFollowSystem && radios.dndKnown && radios.dnd))
        return mascot::Mood::Dnd;  // headphones on, a DND badge (also the desktop's DND)
    if (feat.afkMinutes > 0 || feat.sleepMinutes > 0) {  // away from the keyboard: a snack, then asleep
        const double idle = bb::lfeat::IdleSeconds();
        if (feat.sleepMinutes > 0 && idle >= feat.sleepMinutes * 60.0) return mascot::Mood::Sleepy;
        if (feat.afkMinutes > 0 && idle >= feat.afkMinutes * 60.0) return mascot::Mood::Snack;
    }
    if (k == Kind::Toast || Now() < g_pokeUntil || bb::lfeat::BatteryCheer()) return mascot::Mood::Happy;
    if (bb::lfeat::BatteryDrowsy()) return mascot::Mood::Sleepy;
    return mascot::Mood::Idle;
}

// The approval card's diff: up to kDiffRowsShown lines, the wheel scrolls the rest (as on Windows).
constexpr int kDiffRowsShown = 12;
constexpr double kDiffRowH = 16;
double g_diffScroll = 0, g_diffScrollMax = 0;
uint64_t g_diffScrollFor = 0;

int DiffRowsShown(const AgentView& a) {
    return std::min<int>(static_cast<int>(bb::DiffRows(a).size()), kDiffRowsShown);
}

// How many lines the card's detail takes (at most 3, or 2 above a diff).
int CardLines(cairo_t* cr, const AgentView& a) {
    if (a.detail.empty()) return 0;
    const int most = a.diffOld.empty() && a.diffNew.empty() ? 3 : 2;
    const double width = TextWidth(cr, bb::plat::Utf8(a.detail), 12, false);
    return std::clamp(static_cast<int>(std::ceil(width / (460 - 32 - 30))), 1, most);  // a little slack for mono / wrapping
}

// Privacy dots on the right of the small island, as on Windows: outermost the
// screen (blue, shared or recorded), then the camera (green), then the microphone (orange).
int DotCount() { return (bb::llook::ScreenDotOn() ? 1 : 0) + (bb::llook::CamInUse() ? 1 : 0) + (bb::llook::MicInUse() ? 1 : 0); }
double DotsRoom() { const int n = DotCount(); return n ? 8.0 + n * 13.0 : 0.0; }

void PaintDots(cairo_t* cr, const Rect& p) {
    const auto& l = bb::llook::Get();
    double x = p.x + p.w - 16 - 4;
    const auto dot = [&](const double* c) {
        cairo_arc(cr, x, p.y + p.h / 2, 4, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, c[0], c[1], c[2], 1);
        cairo_fill(cr);
        x -= 13;
    };
    if (bb::llook::ScreenDotOn()) dot(l.screenColor);
    if (bb::llook::CamInUse()) dot(l.camColor);
    if (bb::llook::MicInUse()) dot(l.micColor);
}

// What the small idle island shows beside the face (Bunbun.IdleStripItems).
std::string StripText() {
    const int strip = bb::lfeat::Get().strip;
    std::string text;
    if (strip == 0 || strip == 1) text = ClockText();
    if (strip == 4 || strip == 5) text = bb::lfeat::Get().name;  // your own text
    const auto& w = bb::lsrc::CurrentWeather();
    if ((strip == 0 || strip == 2 || strip == 5) && w.has) {
        const std::string temp = std::to_string(static_cast<int>(std::lround(w.temp))) + "\xC2\xB0";
        text += text.empty() ? temp : " \xC2\xB7 " + temp;
    }
    if (strip == 2 && text.empty()) text = ClockText();  // no weather yet: the time meanwhile
    return text;
}

bool FaceShown() { return g_mascotOn || bb::lfeat::Get().eyes; }

// The widest the music pill may grow: 40 % of the monitor it's on, and never under 280 px.
// The limit follows the screen, so a long title or lyric line can't push the island off it.
double MusicPillMaxWidth() {
    GdkRectangle geo{0, 0, 1920, 1080};
    GdkDisplay* display = g_window ? gtk_widget_get_display(g_window) : gdk_display_get_default();
    if (GdkMonitor* mon = display ? bb::lfeat::TargetMonitor(display) : nullptr) gdk_monitor_get_geometry(mon, &geo);
    return std::max(280.0, 0.4 * geo.width);
}

// The pill's target size for what it shows.
void TargetSize(cairo_t* cr, Kind k, double* w, double* h) {
    const auto& f = bb::lfeat::Get();
    switch (k) {
        case Kind::Hidden:  // invisible; its size is where pointing brings it back
            *w = 120;
            *h = kCollapsedH;
            return;
        case Kind::Tuck:
            *w = 60;
            *h = 6;
            return;
        case Kind::Color:
            *w = 300;
            *h = 112;
            return;
        case Kind::Meeting:
            *w = std::clamp(56 + TextWidth(cr, bb::plat::Utf8(bb::lcal::Next().title), 13.5, true) + 30 +
                                (bb::lcal::Next().joinUrl.empty() ? 0 : 70),
                            240.0, 460.0);
            *h = 50;
            return;
        case Kind::Clock: {
            const std::string strip = StripText();
            double ringFrac = 0, rgb[3] = {};
            const double ring = StatusRingNow(&ringFrac, rgb) ? (strip.empty() ? 15 : 23) : 0;  // the ring, and its gap
            *w = (strip.empty() ? 16 : 30 + TextWidth(cr, strip, 14, true)) + ring + (FaceShown() ? 34 : 0) + DotsRoom();
            if (ring > 0 && FaceShown()) {  // the face in the middle: both sides as wide as the wider one
                const double side = std::max(strip.empty() ? 0.0 : TextWidth(cr, strip, 14, true) + 8, 15.0) + 14;
                *w = side * 2 + 34 + DotsRoom();
            }
            *h = kCollapsedH;
            return;
        }
        case Kind::Media: {
            if (g_pinned || (g_expandOnHover && g_hover) || AutoExpanding()) { *w = 380; *h = 96; return; }
            // Modules.MediaPillClock: the time on the left of the music pill.
            const double clock = f.pillClock ? TextWidth(cr, ClockText(), 13, true) + 18 : 0;
            if (StreamingNow()) {  // the mascot, "Streaming" and the bars
                *w = clock + 44 + TextWidth(cr, "Streaming", 12.5, true) + 44 + DotsRoom();
                *h = kCollapsedH;
                return;
            }
            const double maxPill = MusicPillMaxWidth();
            // Lyrics on the small island: as wide as the song's longest line needs, up to the limit.
            if (const double lw = LyricPillWidth(cr, 38 + 40, maxPill); lw > 0) { *w = clock + lw + DotsRoom(); *h = kCollapsedH; return; }
            // Modules.CollapsedTitle off: just the cover and the bars. Otherwise the title's own width
            // (what Text() draws: the title, or the player's name when there's no title), up to the limit.
            const std::string& shown = g_track.title.empty() ? g_track.player : g_track.title;
            const double room = std::max(0.0, maxPill - clock - 36 - 44 - DotsRoom());
            // With Lyrics on, the title is drawn by the lyric pill's painter, so measure it the same way.
            const double textW = bb::llyrics::PillOn() ? PillTextWidth(cr, shown) : TextWidth(cr, shown, 12.5, true);
            const double title = f.collapsedTitle ? std::min(textW, room) : 0;
            *w = clock + 36 + title + 44 + DotsRoom();
            *h = kCollapsedH;
            return;
        }
        case Kind::Agent:
            *w = g_agent.phase == AgentPhase::Finished || g_agent.phase == AgentPhase::Error ? 320 : 400;
            *h = 44;
            return;
        case Kind::AgentCard: {
            *w = 460;
            double body = 18.0 * CardLines(cr, g_agent);
            if (!g_agent.diffOld.empty() || !g_agent.diffNew.empty()) body += kDiffRowH * DiffRowsShown(g_agent) + 10;
            *h = 54 + body + 52;
            return;
        }
        case Kind::Studio:
            *w = bb::studio::kIslandW;
            *h = bb::studio::kIslandH;
            return;
        case Kind::RichCard:
            RichCardSize(cr, w, h);
            return;
        case Kind::Toast:
            *w = std::clamp(56 + std::max(TextWidth(cr, g_toastTitle, 11.5, true), TextWidth(cr, g_toastText, 13.5, true)) + 22 +
                                (g_toastLevel >= 0 ? 90 : 0),
                            200.0, 440.0);
            *h = 50;
            return;
    }
}

// ── Painting ────────────────────────────────────────────────────────────────

// The status ring beside the clock (Bunbun.StatusRing), as on Windows: the focus
// timer while one runs, today's water, or the to-dos ticked off. Auto: focus while
// a timer runs, else water when it's tracked (Water in Timers or reminders). False for no ring.
bool StatusRingNow(double* frac, double rgb[3]) {
    auto set = [&](uint32_t c) {
        rgb[0] = ((c >> 16) & 0xff) / 255.0;
        rgb[1] = ((c >> 8) & 0xff) / 255.0;
        rgb[2] = (c & 0xff) / 255.0;
    };
    if (g_statusRing == 0) return false;
    const auto& fs = bb::lsrc::FocusState();
    if ((g_statusRing == 1 || g_statusRing == 4) && fs.active && fs.total > 0) {
        const double left = fs.running ? std::max(0.0, fs.endsAt - Now()) : fs.remaining;
        *frac = std::clamp(left / fs.total, 0.0, 1.0);
        set(0xbf5af2);
        return true;
    }
    if (g_statusRing == 1 || g_statusRing == 2) {
        // Read at most once a second: the size pass and the paint both ask, every frame.
        static bb::lwater::Snapshot w;
        static double s_waterAt = -10;
        static bool s_tracked = false;  // Water in Timers (Bunbun.WaterPage)
        if (Now() - s_waterAt > 1.0) {
            w = bb::lwater::Now();
            s_tracked = bb::settings::GetBool(L"Bunbun.WaterPage");
            s_waterAt = Now();
        }
        if ((g_statusRing == 2 || w.reminders || s_tracked) && w.goalMl > 0) {
            *frac = std::clamp(static_cast<double>(w.takenMl) / w.goalMl, 0.0, 1.0);
            set(0x5ac8fa);
            return true;
        }
        return false;
    }
    if (g_statusRing == 3) {
        const auto& todos = bb::lsrc::Todos();
        if (todos.empty()) return false;
        int done = 0;
        for (const auto& t : todos) done += t.done ? 1 : 0;
        *frac = static_cast<double>(done) / static_cast<double>(todos.size());
        set(0x30d158);
        return true;
    }
    return false;
}

void PaintClock(cairo_t* cr, const Rect& p) {
    double frac = 0, rgb[3] = {};
    const bool ringOn = StatusRingNow(&frac, rgb);
    // With a status ring the face sits in the middle: the text on its left, the ring on its right.
    const bool centred = ringOn && FaceShown();
    const double faceX = centred ? p.x + (p.w - DotsRoom()) / 2 - 17 : p.x + 14;
    double x = faceX;
    if (bb::lfeat::Get().eyes) {  // Bunbun.IdleFace = eyes
        bb::lfeat::DrawEyes(cr, x + 13, p.y + p.h / 2, 26, g_mood, g_agent.phase, Now(), g_moodSince, g_hover ? g_mouseX : -1, g_mouseY);
        x += 34;
    } else if (g_mascotOn) {
        DrawMascot(cr, x + 13, p.y + p.h / 2, 30);
        x += 34;
    }
    const std::string strip = StripText();
    if (centred) {
        const double tw = strip.empty() ? 0 : TextWidth(cr, strip, 14, true);
        Text(cr, strip, p.x + std::max(4.0, (faceX - p.x - tw) / 2), p.y + (p.h - 18) / 2, faceX - p.x - 4, 14, true);
    } else {
        Text(cr, strip, x, p.y + (p.h - 18) / 2, p.w - (x - p.x) - 10 - DotsRoom(), 14, true);
    }
    if (ringOn) {
        const double cx = centred ? (faceX + 34 + p.x + p.w - DotsRoom()) / 2
                                  : x + (strip.empty() ? 0 : TextWidth(cr, strip, 14, true) + 8) + 7.5,
                     cy = p.y + p.h / 2, r = 6.3;
        cairo_set_line_width(cr, 2.4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
        cairo_stroke(cr);
        if (frac > 0.004) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, r, -M_PI / 2, -M_PI / 2 + 2 * M_PI * std::min(frac, 0.9999));
            cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
            cairo_stroke(cr);
        }
    }
}

void PaintColor(cairo_t* cr, const Rect& p) {
    const auto& c = g_color;
    // The swatch, then one row per form; the copied one says so for a moment.
    RoundRect(cr, p.x + 14, p.y + 14, 84, 84, 14);
    cairo_set_source_rgb(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    g_hits.push_back({{p.x + 14, p.y + 14, 84, 84}, Action::ColorPick});  // the swatch picks again
    static const char* kLabels[3] = {"HEX", "RGB", "HSL"};
    for (int row = 0; row < 3; ++row) {
        const Rect r{p.x + 110, p.y + 12 + row * 30, p.w - 124, 28};
        const bool hover = static_cast<int>(g_hits.size()) == g_hoverHit;
        if (hover) {
            RoundRect(cr, r.x, r.y, r.w, r.h, 8);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
            cairo_fill(cr);
        }
        const bool copied = row == g_colorCopied && Now() - g_colorCopiedAt < 1.6;
        Text(cr, kLabels[row], r.x + 8, r.y + 6, 34, 11, true, 1, 1, 1, 0.5);
        Text(cr, copied ? "Copied" : ColorRow(row), r.x + 44, r.y + 5, r.w - 52, 13, true, copied ? 0.2 : 1, copied ? 0.88 : 1,
             copied ? 0.4 : 1, 1, row == 0);
        g_hits.push_back({r, Action::ColorCopy, row});
    }
}

// A meeting about to start: its title, a countdown and Join (when it has a link).
void PaintMeeting(cairo_t* cr, const Rect& p) {
    const bb::ics::Meeting& m = bb::lcal::Next();
    DrawMascot(cr, p.x + 26, p.y + p.h / 2, 32);
    const double now = static_cast<double>(time(nullptr));
    const int mins = static_cast<int>(std::ceil((m.startsAt - now) / 60.0));
    const std::string when = mins > 1 ? "In " + std::to_string(mins) + " min" : mins == 1 ? "In 1 min" : "Now";
    const bool join = !m.joinUrl.empty();
    const double x = p.x + 50, w = p.w - 50 - 16 - (join ? 74 : 0);
    Text(cr, when, x, p.y + 7, w, 11.5, true, 1.0, 0.62, 0.04, 1.0);
    Text(cr, bb::plat::Utf8(m.title), x, p.y + 24, w, 13.5, true);
    if (join) Button(cr, {p.x + p.w - 16 - 66, p.y + 10, 66, 30}, "Join", true, Action::MeetingJoin);
    g_hits.push_back({{p.x, p.y, p.w - (join ? 90 : 0), p.h}, Action::MeetingDismiss});
}

// A tucked island: a thin line in the agents' colour (orange while one works,
// green when it's done, red on an error), white otherwise.
void PaintTuck(cairo_t* cr, const Rect& p) {
    double c[3] = {1, 1, 1}, a = 0.55;
    switch (g_agentsOn ? g_agent.phase : AgentPhase::None) {
        case AgentPhase::Working:
        case AgentPhase::Thinking: c[0] = 1; c[1] = 0.6; c[2] = 0.12; a = 0.95; break;
        case AgentPhase::Finished: c[0] = 0.13; c[1] = 0.88; c[2] = 0.35; a = 0.95; break;
        case AgentPhase::Error: c[0] = 1; c[1] = 0.23; c[2] = 0.23; a = 0.95; break;
        default: break;
    }
    RoundRect(cr, p.x + 4, p.y + 1.5, p.w - 8, 3, 1.5);
    cairo_set_source_rgba(cr, c[0], c[1], c[2], a);
    cairo_fill(cr);
}

void PaintMedia(cairo_t* cr, Rect p) {
    const bool expanded = p.h > 60;
    if (!expanded && bb::lfeat::Get().pillClock) {  // the time on the left, then a divider
        const std::string clock = ClockText();
        const double cw = TextWidth(cr, clock, 13, true);
        Text(cr, clock, p.x + 14, p.y + (p.h - 17) / 2, cw + 2, 13, true);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
        cairo_rectangle(cr, p.x + 14 + cw + 8, p.y + 10, 1, p.h - 20);
        cairo_fill(cr);
        p.x += cw + 18;
        p.w -= cw + 18;
    }
    if (!expanded && StreamingNow()) {
        // A video: no cover or title, just the mascot with its popcorn, "Streaming"
        // and the bars (still in the accent colour).
        DrawMascot(cr, p.x + 22, p.y + p.h / 2, 34);
        Text(cr, "Streaming", p.x + 44, p.y + (p.h - 16) / 2, p.w - 44 - 40 - DotsRoom(), 12.5, true);
        Bars(cr, p.x + p.w - 30 - DotsRoom(), p.y + p.h / 2, 16);
        return;
    }
    if (!expanded) {
        // The owner's rule: art and the music-vibing mascot take turns, 3.5 s each.
        const double phase = std::fmod(Now(), 7.0);
        double artAlpha = 0;
        if (g_art) artAlpha = g_mascotOn ? (phase < 3.5 ? std::clamp(std::min(phase, 3.5 - phase) / 0.35, 0.0, 1.0) : 0.0) : 1.0;
        if (artAlpha < 1.0) DrawMascot(cr, p.x + 21, p.y + p.h / 2, 28);  // under the art while it fades
        Art(cr, p.x + 9, p.y + 5, p.h - 10, artAlpha);
        const double room = DotsRoom();
        if (bb::llyrics::PillOn()) PaintLyricPill(cr, {p.x + 38, p.y, p.w - 38 - 40 - room, p.h});
        else if (bb::lfeat::Get().collapsedTitle)
            Text(cr, g_track.title.empty() ? g_track.player : g_track.title, p.x + 38, p.y + (p.h - 16) / 2, p.w - 38 - 40 - room,
                 12.5, true);
        Bars(cr, p.x + p.w - 30 - room, p.y + p.h / 2, 16);
        return;
    }
    const double art = p.h - 24;
    if (g_art) Art(cr, p.x + 12, p.y + 12, art, 1.0);
    else {
        RoundRect(cr, p.x + 12, p.y + 12, art, art, art * 0.22);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
        cairo_fill(cr);
        DrawMascot(cr, p.x + 12 + art / 2, p.y + 12 + art / 2, art * 0.9);
    }
    const double tx = p.x + 24 + art, tw = p.w - (tx - p.x) - 14;
    // Animations.ExpandedMediaTransitions: a new song's words fade and slide in.
    const double since = Now() - g_trackChangedAt;
    const double in = bb::lfeat::Get().mediaTransitions && since < 0.35 ? since / 0.35 : 1.0;
    const double slide = (1.0 - in) * 8;
    Text(cr, g_track.title, tx + slide, p.y + 13, tw, 14, true, 1, 1, 1, in);
    Text(cr, g_track.artist.empty() ? g_track.player : g_track.artist, tx + slide, p.y + 33, tw, 12, false, 1, 1, 1, 0.65 * in);
    const double cy = p.y + p.h - 24;
    IconButton(cr, tx + 16, cy, 30, Action::Previous, g_track.playing);
    IconButton(cr, tx + 56, cy, 34, Action::PlayPause, g_track.playing);
    IconButton(cr, tx + 96, cy, 30, Action::Next, g_track.playing);
    Bars(cr, p.x + p.w - 34, cy, 18);
    // Where the song is, beside the buttons; a click there jumps to that point.
    const double barX = tx + 124, barW = p.x + p.w - 56 - barX;
    if (g_track.length > 0 && barW > 40) {
        static const double kBlue[3] = {0.47, 0.78, 1.0};
        const double pos = std::clamp(bb::mpris::Position(), 0.0, g_track.length);
        bb::lfeat::Progress(cr, barX, cy, barW, pos / g_track.length, kBlue, g_track.playing, Now());
        g_hits.push_back({{barX, cy - 10, barW, 20}, Action::Seek});
    }
}

void PaintAgent(cairo_t* cr, const Rect& p) {
    DrawMascot(cr, p.x + 24, p.y + p.h / 2, 32);
    const double x = p.x + 46, w = p.w - 46 - 14;
    const AgentView& a = g_agent;
    std::string status;
    switch (a.phase) {
        case AgentPhase::Thinking: status = "Thinking\xE2\x80\xA6"; break;
        case AgentPhase::Finished: status = "Done"; break;
        case AgentPhase::Error: status = "Hit an error"; break;
        default: status = a.step.empty() ? "Working\xE2\x80\xA6" : bb::plat::Utf8(a.step);
    }
    Text(cr, AgentLine(a), x, p.y + 5, w, 11.5, true, 1, 1, 1, 0.7);
    Text(cr, status, x, p.y + 22, w - 70, 12.5, true);
    if (a.added >= 0 || a.removed >= 0) {
        const std::string add = "+" + std::to_string(std::max(0, a.added)), del = "\xE2\x88\x92" + std::to_string(std::max(0, a.removed));
        const double dw = TextWidth(cr, del, 11.5, true);
        Text(cr, del, p.x + p.w - 14 - dw, p.y + 24, 0, 11.5, true, 1.0, 0.45, 0.45);
        Text(cr, add, p.x + p.w - 20 - dw - TextWidth(cr, add, 11.5, true), p.y + 24, 0, 11.5, true, 0.45, 0.9, 0.55);
    }
    if (a.sessions > 1) Text(cr, std::to_string(a.sessions) + " agents", p.x + p.w - 74, p.y + 5, 60, 10.5, false, 1, 1, 1, 0.5);
}

void PaintCard(cairo_t* cr, const Rect& p) {
    const AgentView& a = g_agent;
    DrawMascot(cr, p.x + 28, p.y + 28, 36);
    const double x = p.x + 54, w = p.w - 54 - 16;
    Text(cr, AgentLine(a), x, p.y + 10, w, 11.5, true, 1, 1, 1, 0.65);
    const std::string head = a.phase == AgentPhase::Question ? "Has a question" : bb::plat::Utf8(a.tool.empty() ? L"Needs your OK" : a.tool);
    Text(cr, head, x, p.y + 27, w, 14, true);
    double y = p.y + 54;
    const int lines = CardLines(cr, a);
    if (!a.detail.empty()) {
        Text(cr, bb::plat::Utf8(a.detail), p.x + 16, y, p.w - 32, 12, false, 1, 1, 1, 0.85, a.phase == AgentPhase::Approval, lines);
        y += 18.0 * lines;
    }
    if (!a.diffOld.empty() || !a.diffNew.empty()) {
        // Every − and + line on a tinted row; longer than the card and the wheel scrolls it.
        const std::vector<bb::DiffRow> rows = bb::DiffRows(a);
        if (g_diffScrollFor != a.requestId) { g_diffScrollFor = a.requestId; g_diffScroll = 0; }
        const double vx = p.x + 14, vw = p.w - 28, vy = y + 2, vh = kDiffRowH * DiffRowsShown(a);
        const double total = kDiffRowH * rows.size();
        g_diffScrollMax = std::max(0.0, total - vh);
        g_diffScroll = std::clamp(g_diffScroll, 0.0, g_diffScrollMax);
        cairo_save(cr);
        cairo_rectangle(cr, vx, vy, vw, vh);
        cairo_clip(cr);
        double ry = vy - g_diffScroll;
        for (const bb::DiffRow& r : rows) {
            if (ry + kDiffRowH >= vy && ry <= vy + vh) {
                const bool del = r.sign < 0;
                const double rw = vw - (g_diffScrollMax > 0 ? 8 : 0);
                if (del) cairo_set_source_rgba(cr, 1.0, 0.27, 0.23, 0.13);
                else cairo_set_source_rgba(cr, 0.2, 0.84, 0.29, 0.12);
                cairo_rectangle(cr, vx, ry, rw, kDiffRowH);
                cairo_fill(cr);
                Text(cr, std::string(del ? "\xE2\x88\x92 " : "+ ") + bb::plat::Utf8(r.text), vx + 6, ry + 1, rw - 10, 11, false,
                     del ? 1.0 : 0.49, del ? 0.54 : 0.91, del ? 0.5 : 0.53, 0.95, true);
            }
            ry += kDiffRowH;
        }
        cairo_restore(cr);
        if (g_diffScrollMax > 0) {
            const double thumb = std::max(18.0, vh * vh / total);
            const double ty = vy + (vh - thumb) * (g_diffScroll / g_diffScrollMax);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.3);
            RoundRect(cr, vx + vw - 4, ty, 3, thumb, 1.5);
            cairo_fill(cr);
        }
        y += vh + 10;
    }
    const double bh = 32, gap = 8, by = p.y + p.h - bh - 12, left = p.x + 14, right = p.x + p.w - 14;
    if (a.phase == AgentPhase::Question && !a.options.empty()) {
        const int n = static_cast<int>(std::min<size_t>(a.options.size(), 4));
        const double bw = (right - left - gap * (n - 1)) / n;
        for (int i = 0; i < n; ++i)
            Button(cr, {left + i * (bw + gap), by, bw, bh}, bb::plat::Utf8(a.options[i]), i == 0, Action::Answer, i);
    } else if (a.phase == AgentPhase::Question) {
        // Several questions, multi-select or free text (AgentView::questions): there's
        // no card for those on Linux yet, and Allow would send no answers.
        Button(cr, {left, by, right - left, bh}, "Answer in terminal", true, Action::Terminal);
    } else {
        const double bw = (right - left - gap * 2) / 3;
        Button(cr, {left, by, bw, bh}, "Allow", true, Action::Allow);
        Button(cr, {left + bw + gap, by, bw, bh}, "Deny", false, Action::Deny);
        Button(cr, {left + 2 * (bw + gap), by, bw, bh}, "In terminal", false, Action::Terminal);
    }
    if (a.queued > 0) Text(cr, "+" + std::to_string(a.queued) + " waiting", p.x + p.w - 90, p.y + 10, 76, 10.5, false, 1, 1, 1, 0.5);
}

void PaintToast(cairo_t* cr, const Rect& p) {
    DrawMascot(cr, p.x + 26, p.y + p.h / 2, 32);
    const double barW = g_toastLevel >= 0 ? 90 : 0;
    const double x = p.x + 50, w = p.w - 50 - 16 - barW;
    Text(cr, g_toastTitle, x, p.y + 7, w, 11.5, true, 1, 1, 1, 0.65);
    Text(cr, g_toastText, x, p.y + 24, w, 13.5, true);
    if (g_toastLevel >= 0) {  // a level: volume, brightness, battery
        const double bx = p.x + p.w - 16 - barW + 8, bw = barW - 8, cy = p.y + p.h / 2;
        RoundRect(cr, bx, cy - 3, bw, 6, 3);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
        cairo_fill(cr);
        RoundRect(cr, bx, cy - 3, std::max(6.0, bw * std::clamp(g_toastLevel, 0.0, 1.0)), 6, 3);
        cairo_set_source_rgba(cr, 0.47, 0.78, 1.0, 0.95);
        cairo_fill(cr);
    }
    if (bb::lfeat::Get().countdownBar && g_toastSeconds > 0) {  // Modules.StatusCountdownProgress
        const double left = std::clamp(1.0 - (Now() - g_toastStart) / g_toastSeconds, 0.0, 1.0);
        cairo_rectangle(cr, p.x + 16, p.y + p.h - 3, (p.w - 32) * left, 2);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
        cairo_fill(cr);
    }
}

// Room for the expanded offset (Appearance.OffsetYExpanded) on top of the canvas.
double ExpandedShift() {
    const auto& f = bb::lfeat::Get();
    return f.separateExpandedY ? f.offsetYExpanded - static_cast<int>(g_offsetY) : 0;
}
double CanvasH() { return kCanvasH + std::fabs(ExpandedShift()); }

// Where the pill is in the window this frame: left, centre or right
// (Appearance.Position), against the top or the bottom edge, moved by the
// expanded offset while open.
Rect PillRect() {
    const auto& f = bb::lfeat::Get();
    const double x = f.align == 0 ? 0 : f.align == 2 ? kCanvasW - g_w : (kCanvasW - g_w) / 2;
    // The window sits at the smaller of the two offsets; g_yOff is how far the pill
    // is from it now (eased by the spring as the island opens and closes).
    const double gap = bb::llook::TopGap(kTopGap);
    if (f.bottom) return {x, CanvasH() - gap - g_h - g_yOff, g_w, g_h};
    return {x, gap + g_yOff, g_w, g_h};
}

// The pill as a region of pixel rows, its corners rounded with `radius`.
cairo_region_t* PillRegion(const Rect& p, double radius) {
    cairo_region_t* region = cairo_region_create();
    const int x0 = static_cast<int>(std::floor(p.x)), y0 = static_cast<int>(std::floor(p.y));
    const int w = static_cast<int>(std::ceil(p.w)), h = static_cast<int>(std::ceil(p.h));
    for (int y = 0; y < h; ++y) {
        // The notch and the merged island are square on the screen edge's side.
        const bool square = bb::llook::Get().shape == bb::llook::Shape::Notch || bb::lfeat::Get().merged;
        const bool bottom = bb::lfeat::Get().bottom;
        const double top = square && !bottom ? 0.0 : radius - (y + 0.5);
        const double low = square && bottom ? 0.0 : (y + 0.5) - (h - radius);
        const double dy = std::max({0.0, top, low});
        const int inset = dy > 0 ? static_cast<int>(std::ceil(radius - std::sqrt(std::max(0.0, radius * radius - dy * dy)))) : 0;
        cairo_rectangle_int_t row{x0 + inset, y0 + y, w - 2 * inset, 1};
        if (row.width > 0) cairo_region_union_rectangle(region, &row);
    }
    return region;
}

void UpdateInputRegion(const Rect& p, double radius) {
    if (!gtk_widget_get_realized(g_window)) return;
    cairo_rectangle_int_t r{static_cast<int>(std::floor(p.x)) - 2, static_cast<int>(std::floor(p.y)) - 2,
                            static_cast<int>(std::ceil(p.w)) + 4, static_cast<int>(std::ceil(p.h)) + 4};
    cairo_region_t* region = cairo_region_create_rectangle(&r);
    gtk_widget_input_shape_combine_region(g_window, region);
    cairo_region_destroy(region);
    // X11 without a compositor can't show transparency: the whole window would
    // be a black box. Cut the window itself to the pill's shape instead.
    if (!g_layerShell) {
        if (gdk_screen_is_composited(gtk_widget_get_screen(g_window))) {
            gtk_widget_shape_combine_region(g_window, nullptr);
        } else {
            cairo_region_t* shape = PillRegion(p, radius);
            gtk_widget_shape_combine_region(g_window, shape);
            cairo_region_destroy(shape);
        }
    }
}

gboolean OnDraw(GtkWidget*, cairo_t* cr, gpointer) {
    const double now = Now();
    g_lastDrawAt = now;
    const double dt = std::clamp(now - g_lastFrame, 0.0, 0.1);
    g_lastFrame = now;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    // With mirrors, draw into a group first so they can show the same frame.
    cairo_t* const windowCr = cr;
    const bool capture = !g_mirrors.empty();
    if (capture) cairo_push_group(cr);
    cairo_scale(cr, g_scale, g_scale);
    const auto finishFrame = [&](const Rect& pill) {
        if (!capture) return;
        cairo_pattern_t* frame = cairo_pop_group(windowCr);
        cairo_set_source(windowCr, frame);
        cairo_paint(windowCr);
        if (g_frame) cairo_pattern_destroy(g_frame);
        g_frame = frame;
        g_framePill = {pill.x, pill.y, pill.w, pill.h};
        if (bb::lfeat::SeparateDisplays() && pill.h <= 60 && pill.w > 1 && !g_fsFolded) {  // at rest: keep it for the mirrors
            if (g_restFrame) cairo_pattern_destroy(g_restFrame);
            g_restFrame = cairo_pattern_reference(frame);
            g_restPill = {pill.x, pill.y, pill.w, pill.h};
        }
        for (const Mirror& m : g_mirrors) gtk_widget_queue_draw(m.window);
    };

    const Kind kind = CurrentKind();
    const mascot::Mood mood = MoodFor(kind);
    if (mood != g_mood) {
        g_mood = mood;
        g_moodSince = now;
    }

    double tw = 0, th = 0;
    TargetSize(cr, kind, &tw, &th);
    // The resize spring, in the chosen motion style and speed (Animations.*).
    bb::lfeat::Spring(&g_w, &g_vw, tw, dt);
    bb::lfeat::Spring(&g_h, &g_vh, th, dt);
    g_w = std::max(g_w, 1.0);
    g_h = std::max(g_h, 1.0);
    const bool open = th > 60;
    const double shift = ExpandedShift();
    bb::lfeat::Spring(&g_yOff, &g_vy, open ? std::max(shift, 0.0) : std::max(-shift, 0.0), dt);
    const bool resizing = g_w != tw || g_h != th;
    if (g_hover || kind == Kind::AgentCard || kind == Kind::RichCard) bb::lfeat::NoteInteraction();
    if (bb::lshelf::CameraRunning() && !(kind == Kind::Studio && StudioOnMirror())) bb::lshelf::CameraStop();
    // The bars follow the real audio while music plays on screen.
    if (g_track.playing && (kind == Kind::Media || kind == Kind::Studio)) bb::lfeat::WantAudio(true);

    Rect p = PillRect();
    // The nudge: a small bounce when a new agent event arrives.
    const double since = now - g_nudgeAt;
    if (since < 0.45) {
        const double bump = std::sin(since / 0.45 * M_PI) * 4;
        p.x -= bump;
        p.w += bump * 2;
    }

    // Hidden: nothing drawn. Pointing where it was brings it back (unless the
    // hotkey hid it), so the input region stays.
    if (kind == Kind::Hidden) {
        g_hits.clear();
        const bool pointable = !bb::lfeat::HiddenByHotkey() && bb::lfeat::Get().unhideOnHover;
        Rect scaled{p.x * g_scale, p.y * g_scale, pointable ? p.w * g_scale : 1, pointable ? p.h * g_scale : 1};
        UpdateInputRegion(scaled, 0);
        finishFrame({0, 0, 0, 0});
        ScheduleFrame(false);
        return TRUE;
    }

    // The surface in the chosen shape and colours, with a soft shadow. The open
    // island gets rounder corners.
    const auto& look = bb::llook::Get();
    const double radius = bb::llook::Radius(p.h);
    // Themes.TintIntensity: a dark scrim deepens a see-through fill.
    if (look.opacity < 1.0 && look.tint > 0 && kind != Kind::Tuck) {
        bb::llook::SurfacePath(cr, p.x, p.y, p.w, p.h, radius);
        cairo_set_source_rgba(cr, 0, 0, 0, look.tint * 0.35 * (1.0 - look.opacity) * 3);
        cairo_fill(cr);
    }
    if (look.shadow && kind != Kind::Tuck) {
        for (int i = 3; i >= 1; --i) {
            bb::llook::SurfacePath(cr, p.x - i, p.y - i + 2, p.w + 2 * i, p.h + 2 * i, radius + i);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.07);
            cairo_fill(cr);
        }
    }
    bb::llook::SurfacePath(cr, p.x, p.y, p.w, p.h, radius);
    cairo_set_source_rgba(cr, look.fill[0], look.fill[1], look.fill[2], look.opacity);
    if (look.borderOn) {
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, look.border[0], look.border[1], look.border[2], 0.9);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    } else {
        cairo_fill(cr);
    }
    // Themes.MaterialDepth: soft shading downwards; Themes.AccentBloom: the accent
    // (the album's colour while music plays) washing in from the top.
    if (look.depth && kind != Kind::Tuck) {
        cairo_pattern_t* g = cairo_pattern_create_linear(0, p.y, 0, p.y + p.h);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 1, 1, 1, 0.035);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 0, 0, 0, 0.16);
        bb::llook::SurfacePath(cr, p.x, p.y, p.w, p.h, radius);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        if (look.bloom > 0) {
            static const double kAccent[3] = {0.47, 0.78, 1.0};
            const double a = (kind == Kind::Media || (kind == Kind::Studio && g_track.playing) ? 0.16 : 0.06) * look.bloom;
            cairo_pattern_t* b = cairo_pattern_create_radial(p.x + p.w / 2, p.y, 0, p.x + p.w / 2, p.y, std::max(p.w, p.h) * 0.6);
            cairo_pattern_add_color_stop_rgba(b, 0, kAccent[0], kAccent[1], kAccent[2], a);
            cairo_pattern_add_color_stop_rgba(b, 1, kAccent[0], kAccent[1], kAccent[2], 0);
            bb::llook::SurfacePath(cr, p.x, p.y, p.w, p.h, radius);
            cairo_set_source(cr, b);
            cairo_fill(cr);
            cairo_pattern_destroy(b);
        }
    }

    // A blip when the island opens into Studio or folds back (Bunbun.SoundOpenClose).
    static bool wasOpen = false;
    if ((kind == Kind::Studio) != wasOpen) {
        wasOpen = kind == Kind::Studio;
        bb::llook::PlayChime(wasOpen ? bb::chime::Chime::Open : bb::chime::Chime::Close);
    }

    g_hits.clear();
    cairo_save(cr);
    bb::llook::SurfacePath(cr, p.x, p.y, p.w, p.h, radius);
    cairo_clip(cr);
    // Content appears once the pill is close to its size, so it never spills.
    if (!resizing || (std::fabs(tw - g_w) < 24 && std::fabs(th - g_h) < 16)) {
        switch (kind) {
            case Kind::Clock: PaintClock(cr, p); break;
            case Kind::Media: PaintMedia(cr, p); break;
            case Kind::Agent: PaintAgent(cr, p); break;
            case Kind::AgentCard: PaintCard(cr, p); break;
            case Kind::Studio:
                PaintStudio(cr, p, std::clamp((g_h - 120.0) / (bb::studio::kIslandH - 120.0), 0.0, 1.0));
                break;
            case Kind::RichCard: PaintRichCard(cr, p); break;
            case Kind::Toast: PaintToast(cr, p); break;
            case Kind::Tuck: PaintTuck(cr, p); break;
            case Kind::Meeting: PaintMeeting(cr, p); break;
            case Kind::Color: PaintColor(cr, p); break;
            case Kind::Hidden: break;
        }
        if ((kind == Kind::Clock || kind == Kind::Media) && p.h <= 55 && DotCount()) PaintDots(cr, p);
    }
    cairo_restore(cr);

    // The edge light: what happened this frame, against the chosen triggers
    // (Bunbun.EdgeLightOn), then the light itself over the outline.
    {
        namespace el = bb::edgelight;
        static bool prevHover = false, prevOpen = false;
        static Kind prevKind = Kind::Clock;
        static std::string prevTitle;
        static double prevToastUntil = 0;
        unsigned events = 0;
        const bool openNow = kind == Kind::Studio;
        if (g_hover && !prevHover) events |= el::kOnHover;
        if (openNow && !prevOpen) events |= el::kOnOpen;
        if (!openNow && prevOpen) events |= el::kOnClose;
        const bool agentKind = kind == Kind::Agent || kind == Kind::AgentCard || kind == Kind::RichCard;
        const bool agentPrev = prevKind == Kind::Agent || prevKind == Kind::AgentCard || prevKind == Kind::RichCard;
        if (agentKind && !agentPrev) events |= el::kOnAgent;
        if (kind == Kind::Meeting && prevKind != Kind::Meeting) events |= el::kOnNotification;
        if (g_toastUntil > prevToastUntil + 0.5 && g_toastUntil > now) events |= g_toastLevel >= 0 ? el::kOnSystem : el::kOnNotification;
        if (g_track.active && !g_track.title.empty() && g_track.title != prevTitle && !prevTitle.empty()) events |= el::kOnTrack;
        prevHover = g_hover;
        prevOpen = openNow;
        prevKind = kind;
        prevToastUntil = g_toastUntil;
        if (g_track.active && !g_track.title.empty()) prevTitle = g_track.title;
        bb::ledge::Note(events, now);
        if (kind != Kind::Tuck && bb::ledge::Lit(now)) {
            double accent[3];
            StudioAccentRgb(accent);
            bb::ledge::Draw(cr, p.x, p.y, p.w, p.h, radius, accent, now);
        }
    }

    Rect scaled{p.x * g_scale, p.y * g_scale, p.w * g_scale, p.h * g_scale};
    UpdateInputRegion(scaled, radius * g_scale);
    finishFrame(p);

    // Fast frames while something moves; the mascot's idle motion is fine at ~8 fps.
    const bool fast = resizing || since < 0.45 || kind == Kind::Media || kind == Kind::AgentCard || kind == Kind::Studio ||
                      kind == Kind::RichCard || kind == Kind::Toast ||
                      mascot::NeedsFastFrames(g_mood, now, g_moodSince) || g_hover || bb::ledge::Lit(now) ||
                      (kind == Kind::Clock && bb::lfeat::Get().eyes);
    g_fastMs = bb::lfeat::FrameMs(resizing || g_hover || kind == Kind::Studio || kind == Kind::AgentCard || kind == Kind::RichCard);
    ScheduleFrame(fast);
    return TRUE;
}

gboolean OnTimer(gpointer) {
    g_timer = 0;
    if (g_window) gtk_widget_queue_draw(g_window);
    return G_SOURCE_REMOVE;
}

void ScheduleFrame(bool fast) {
    if (g_timer) return;
    // Fast: Animations.TargetFPS while you use the island, Bunbun.AmbientFps for
    // moving content otherwise, both lowered by the battery saver.
    g_timer = g_timeout_add(fast ? g_fastMs : 125, OnTimer, nullptr);
}

void Redraw() {
    if (g_timer) {
        g_source_remove(g_timer);
        g_timer = 0;
    }
    if (g_window) gtk_widget_queue_draw(g_window);
}

// ── Album art ───────────────────────────────────────────────────────────────

void SetArt(GdkPixbuf* pb) {
    if (g_art) g_object_unref(g_art);
    g_art = nullptr;
    g_artChangedAt = Now();
    if (pb) {
        const int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), side = std::min(w, h);
        // A centred square, at most 192 px: covers are often 1000+ px.
        GdkPixbuf* square = gdk_pixbuf_new_subpixbuf(pb, (w - side) / 2, (h - side) / 2, side, side);
        g_art = gdk_pixbuf_scale_simple(square, std::min(side, 192), std::min(side, 192), GDK_INTERP_BILINEAR);
        g_object_unref(square);
    }
    Redraw();
}

void OnArtLoaded(GObject* src, GAsyncResult* res, gpointer) {
    char* data = nullptr;
    gsize len = 0;
    GError* err = nullptr;
    if (!g_file_load_contents_finish(G_FILE(src), res, &data, &len, nullptr, &err)) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            Wh_Log(L"art: %ls", bb::plat::Wide(err ? err->message : "load failed").c_str());
        g_clear_error(&err);
        return;
    }
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GdkPixbuf* pb = nullptr;
    if (gdk_pixbuf_loader_write(loader, reinterpret_cast<guchar*>(data), len, &err) && gdk_pixbuf_loader_close(loader, &err))
        pb = gdk_pixbuf_loader_get_pixbuf(loader);
    else
        gdk_pixbuf_loader_close(loader, nullptr);
    if (err) Wh_Log(L"art: %ls", bb::plat::Wide(err->message).c_str());
    g_clear_error(&err);
    SetArt(pb);
    g_object_unref(loader);
    g_free(data);
}

void LoadArt(const std::string& url) {
    if (url == g_artUrl) return;
    g_artUrl = url;
    if (g_artCancel) {
        g_cancellable_cancel(g_artCancel);
        g_object_unref(g_artCancel);
        g_artCancel = nullptr;
    }
    SetArt(nullptr);
    if (url.empty()) return;
    // file:// always works; http(s):// needs GVfs, which GNOME and KDE desktops have.
    GFile* file = g_file_new_for_uri(url.c_str());
    g_artCancel = g_cancellable_new();
    g_file_load_contents_async(file, g_artCancel, OnArtLoaded, nullptr);
    g_object_unref(file);
}

void OnMediaChanged() {
    g_track = bb::mpris::Current();
    // A new song: the transitions, and Modules.MediaAutoExpand (never for a video,
    // nor for apps and sites on Modules.MediaExpandBlocklist).
    const std::string song = g_track.title + "\n" + g_track.artist;
    if (g_track.active && !g_track.title.empty() && song != g_lastSong) {
        const bool first = g_lastSong.empty();
        g_lastSong = song;
        g_trackChangedAt = Now();
        const auto& f = bb::lfeat::Get();
        if (!first && g_track.playing && f.autoExpand && !IsVideoTrack(g_track) && !bb::lfeat::ExpandBlocked(g_track.player, g_track.title))
            g_expandUntil = Now() + 5.0;
        bb::lfeat::NoteInteraction();
    }
    LoadArt(g_track.active ? g_track.artUrl : std::string());
    bb::llyrics::OnTrack(g_track);
    Redraw();
}

// ── Agents ──────────────────────────────────────────────────────────────────

gboolean ApplyAgentView(gpointer) {
    std::lock_guard lock(g_agentMutex);
    if (!g_agentInboxNew) return G_SOURCE_REMOVE;
    g_agentInboxNew = false;
    if (g_agentInbox.generation < g_agent.generation && g_agentInbox.generation != 0) return G_SOURCE_REMOVE;
    // A chime when an agent finishes, needs you, or stops with an error (as on Windows).
    const AgentView& next = g_agentInbox;
    using bb::chime::Chime;
    if (next.phase == AgentPhase::Finished && g_agent.phase != AgentPhase::Finished) bb::llook::PlayChime(Chime::Done);
    else if (next.phase == AgentPhase::Error && g_agent.phase != AgentPhase::Error) bb::llook::PlayChime(Chime::Error);
    else if ((next.phase == AgentPhase::Approval || next.phase == AgentPhase::Question) && next.requestId != 0 &&
             next.requestId != g_agent.requestId)
        bb::llook::PlayChime(Chime::Question);
    g_agent = g_agentInbox;
    bb::lfeat::NoteInteraction();
    if (g_agentInboxNudge) g_nudgeAt = Now();
    g_agentInboxNudge = false;
    Redraw();
    return G_SOURCE_REMOVE;
}

// ── Input ───────────────────────────────────────────────────────────────────

int HitAt(double x, double y) {
    x /= g_scale;
    y /= g_scale;
    for (size_t i = 0; i < g_hits.size(); ++i)
        if (g_hits[i].r.Contains(x, y)) return static_cast<int>(i);
    return -1;
}

void ShowMenu(GdkEvent* event);

void Do(const Hit& h) {
    switch (h.action) {
        case Action::PlayPause: bb::mpris::PlayPause(); break;
        case Action::Next: bb::mpris::Next(); break;
        case Action::Previous: bb::mpris::Previous(); break;
        case Action::Allow: bb::agents::Decide(g_agent.requestId, bb::AgentDecision::Allow); break;
        case Action::Deny: bb::agents::Decide(g_agent.requestId, bb::AgentDecision::Deny); break;
        case Action::Terminal: bb::agents::Decide(g_agent.requestId, bb::AgentDecision::Terminal); break;
        case Action::Answer: bb::agents::Decide(g_agent.requestId, bb::AgentDecision::Allow, h.index); break;
        case Action::Settings: OpenSettings(); break;
        case Action::MeetingJoin: bb::lcal::Join(); break;
        case Action::ColorPick: g_colorUntil = 0; PickColour(); break;
        case Action::ColorCopy: CopyColorRow(h.index); break;
        case Action::MeetingDismiss: bb::lcal::Dismiss(); break;
        case Action::None: break;
        default: StudioDo(h); break;
    }
}

gboolean OnButtonPress(GtkWidget*, GdkEventButton* e, gpointer) {
    if (e->type != GDK_BUTTON_PRESS) return TRUE;
    if (e->button != 1) return TRUE;  // the menu opens on release (OnButtonRelease)
    const int i = HitAt(e->x, e->y);
    const Kind kind = CurrentKind();
    if (i >= 0 && g_hits[i].action == Action::ShelfOpen) {  // decided on release or on a drag
        g_shelfPress = g_hits[i].index;
        g_shelfPressX = e->x;
        g_shelfPressY = e->y;
        return TRUE;
    }
    if (i >= 0 && g_hits[i].action == Action::Seek) {  // the progress bar: jump to where it was clicked
        const Rect& r = g_hits[i].r;
        if (g_track.length > 0 && r.w > 0)
            bb::mpris::SeekTo(std::clamp((e->x / g_scale - r.x) / r.w, 0.0, 1.0) * g_track.length);
        Redraw();
        return TRUE;
    }
    if (kind == Kind::Color && i < 0) g_colorUntil = 0;  // a click off its rows puts it away
    if (kind == Kind::Toast && i < 0) {  // a click puts it away; a water reminder's logs the glass
        if (g_toastWater == 1) bb::lwater::Drink();
        else if (g_toastWater == 2) bb::lwater::Refill();
        g_toastWater = 0;
        g_toastUntil = 0;
        g_hoverHold = true;  // back to the small island (the mascot drinks) rather than opening under the pointer
    }
    else if (i >= 0) Do(g_hits[i]);
    // Expand on hover off: a click on the island opens it, another closes it.
    else if (!g_expandOnHover && kind != Kind::AgentCard && kind != Kind::RichCard) g_pinned = !g_pinned;
    else if (kind == Kind::Clock && !bb::lfeat::Get().mascotDnd) g_pokeUntil = Now() + 1.2;  // a poke
    Redraw();
    return TRUE;
}

gboolean OnMotion(GtkWidget*, GdkEventMotion* e, gpointer) {
    g_mouseX = e->x / g_scale;
    g_mouseY = e->y / g_scale;
    // Dragging a file off the shelf hands it to wherever you drop it.
    if (g_shelfPress >= 0 && std::hypot(e->x - g_shelfPressX, e->y - g_shelfPressY) > 6) {
        g_shelfDrag = g_shelfPress;
        g_shelfPress = -1;
        GtkTargetList* targets = gtk_target_list_new(nullptr, 0);
        gtk_target_list_add_uri_targets(targets, 0);
        gtk_drag_begin_with_coordinates(g_window, targets, static_cast<GdkDragAction>(GDK_ACTION_COPY | GDK_ACTION_LINK), 1,
                                        reinterpret_cast<GdkEvent*>(e), static_cast<int>(e->x), static_cast<int>(e->y));
        gtk_target_list_unref(targets);
    }
    const int hit = HitAt(e->x, e->y);
    if (hit != g_hoverHit) {
        g_hoverHit = hit;
        Redraw();
    }
    return FALSE;
}

gboolean OnButtonRelease(GtkWidget*, GdkEventButton* e, gpointer) {
    // The menu opens on release: opened on press, the release that follows could land
    // on its first item (Settings) and pick it, as happens under XWayland.
    if (e->button == 3) {
        ShowMenu(reinterpret_cast<GdkEvent*>(e));
        return TRUE;
    }
    if (e->button == 1 && g_shelfPress >= 0) {
        bb::lshelf::Open(g_shelfPress);
        g_shelfPress = -1;
        Redraw();
    }
    return TRUE;
}

// Files dropped on the island go on the shelf, and the island opens on it.
void OnDragReceived(GtkWidget*, GdkDragContext* ctx, int, int, GtkSelectionData* data, guint, guint time, gpointer) {
    std::vector<std::string> uris;
    if (gchar** list = gtk_selection_data_get_uris(data)) {
        for (gchar** u = list; *u; ++u) uris.emplace_back(*u);
        g_strfreev(list);
    }
    gtk_drag_finish(ctx, !uris.empty(), FALSE, time);
    if (uris.empty() || !bb::lshelf::ShelfOn()) return;
    bb::lshelf::Add(uris);
    StudioShowShelf();
    ShowToast("File shelf", uris.size() == 1 ? "1 file added" : std::to_string(uris.size()) + " files added", 2);
}

void OnDragGet(GtkWidget*, GdkDragContext*, GtkSelectionData* data, guint, guint, gpointer) {
    const auto& items = bb::lshelf::Items();
    if (g_shelfDrag < 0 || g_shelfDrag >= static_cast<int>(items.size())) return;
    gchar* uris[] = {const_cast<gchar*>(items[static_cast<size_t>(g_shelfDrag)].uri.c_str()), nullptr};
    gtk_selection_data_set_uris(data, uris);
}

gboolean OnCrossing(GtkWidget*, GdkEventCrossing* e, gpointer) {
    // Moving onto a child area isn't leaving the island.
    if (e->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    const bool entering = e->type == GDK_ENTER_NOTIFY;
    if (entering && !g_hover) {
        g_hoverSince = Now();
        // Over a working agent, the frame opens on the Agent page.
        if (g_agentsOn && g_agent.phase != AgentPhase::None) StudioShowAgentPage();
        // Studio opens after kOpenDelay; make sure a frame comes then.
        g_timeout_add(static_cast<guint>(kOpenDelay * 1000) + 10, [](gpointer) -> gboolean { Redraw(); return G_SOURCE_REMOVE; }, nullptr);
    }
    g_hover = entering;
    if (!g_hover) g_hoverHold = false;
    if (!g_hover) {
        g_hoverHit = -1;
        g_mouseX = g_mouseY = -1;
    }
    Redraw();
    return FALSE;
}

gboolean OnScroll(GtkWidget*, GdkEventScroll* e, gpointer) {
    double delta = 0;
    switch (e->direction) {
        case GDK_SCROLL_UP:
        case GDK_SCROLL_LEFT: delta = -1; break;
        case GDK_SCROLL_DOWN:
        case GDK_SCROLL_RIGHT: delta = 1; break;
        case GDK_SCROLL_SMOOTH: delta = std::fabs(e->delta_x) > std::fabs(e->delta_y) ? e->delta_x : e->delta_y; break;
    }
    const Kind k = CurrentKind();
    const bool fine = (e->state & GDK_SHIFT_MASK) != 0;
    if ((k == Kind::Studio || k == Kind::RichCard) && !(k == Kind::Studio && StudioScrollTimer(delta, fine))) StudioScroll(delta);
    if (k == Kind::AgentCard && g_diffScrollMax > 0 && g_agent.requestId == g_diffScrollFor)
        g_diffScroll = std::clamp(g_diffScroll + delta * 3 * kDiffRowH, 0.0, g_diffScrollMax);
    // Bunbun.MusicSwipe: sideways on the small music pill skips a song.
    const bool sideways = e->direction == GDK_SCROLL_LEFT || e->direction == GDK_SCROLL_RIGHT ||
                          (e->direction == GDK_SCROLL_SMOOTH && std::fabs(e->delta_x) > std::fabs(e->delta_y) && std::fabs(e->delta_x) > 0.4);
    if (k == Kind::Media && g_h < 60 && sideways && bb::lfeat::Get().musicSwipe && Now() - g_swipeAt > 0.6) {
        g_swipeAt = Now();
        if (delta > 0) bb::mpris::Next();
        else bb::mpris::Previous();
    }
    Redraw();
    return TRUE;
}

gboolean OnKey(GtkWidget*, GdkEventKey* e, gpointer) {
    const bool used = ComposeKey(e);
    if (used) Redraw();
    return used;
}

// ── Menu (the tray menu's items; there's no tray on Linux yet) ──────────────

void CloseIsland() {
    g_pinned = false;
    g_hoverHold = true;
    Redraw();
}

void OpenSettings(const char* page) {
    // bunbun-settings needs WebKitGTK. If it can't start (exit 127: a library
    // is missing), open settings.ini in the default editor instead.
    const std::string exe = bb::plat::ExeDir() + "/bunbun-settings";
    bb::settings::EnsureFile();
    if (!bb::plat::FileExists(exe)) {
        bb::plat::OpenWithDefault(bb::settings::Path());
        return;
    }
    // bunbun-settings takes the page to open as a plain argument.
    gchar* argv[] = {const_cast<gchar*>(exe.c_str()), const_cast<gchar*>(page ? page : ""), nullptr};
    if (!page) argv[1] = nullptr;
    GPid pid = 0;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_DO_NOT_REAP_CHILD, nullptr, nullptr, &pid, nullptr)) {
        bb::plat::OpenWithDefault(bb::settings::Path());
        return;
    }
    g_child_watch_add(pid, [](GPid child, gint status, gpointer) {
        g_spawn_close_pid(child);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) bb::plat::OpenWithDefault(bb::settings::Path());
    }, nullptr);
}

// The menu, for the island's right-click and the tray icon (the tray adds the
// version, the hide / show switch and Start when you log in, as on Windows).
GtkWidget* BuildMenu(bool tray) {
    GtkWidget* menu = gtk_menu_new();
    const auto item = [&](const char* label, GCallback cb) {
        GtkWidget* it = gtk_menu_item_new_with_label(label);
        g_signal_connect(it, "activate", cb, nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    };
    if (tray) {
        GtkWidget* version = gtk_menu_item_new_with_label("Bunbun " BUNBUN_VERSION_STR);
        gtk_widget_set_sensitive(version, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), version);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        item(bb::lfeat::HiddenByHotkey() ? "Show the island" : "Hide the island",
             G_CALLBACK(+[](GtkMenuItem*, gpointer) { bb::lfeat::ToggleHidden(); bb::ltray::Refresh(); }));
    }
    item("Settings\xE2\x80\xA6", G_CALLBACK(+[](GtkMenuItem*, gpointer) { OpenSettings(); }));
    item("Pick a colour\xE2\x80\xA6", G_CALLBACK(+[](GtkMenuItem*, gpointer) {
        // After the menu has closed, so it isn't in the picture.
        g_timeout_add(200, [](gpointer) -> gboolean { PickColour(); return G_SOURCE_REMOVE; }, nullptr);
    }));
    if (bb::hooks::ClaudeHooksInstalled())
        item("Disconnect Claude Code", G_CALLBACK(+[](GtkMenuItem*, gpointer) {
            std::wstring report;
            bb::hooks::UninstallClaudeHooks(&report);
            Wh_Log(L"%ls", report.c_str());
            bb::ltray::Refresh();
        }));
    else
        item("Connect Claude Code", G_CALLBACK(+[](GtkMenuItem*, gpointer) {
            std::wstring report;
            bb::hooks::InstallClaudeHooks(&report);
            Wh_Log(L"%ls", report.c_str());
            bb::ltray::Refresh();
        }));
    // Lyrics on the small island, as on Windows' right-click menu.
    GtkWidget* lyrics = gtk_check_menu_item_new_with_label("Lyrics on the island");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(lyrics), bb::llyrics::PillOn());
    g_signal_connect(lyrics, "toggled", G_CALLBACK(+[](GtkCheckMenuItem* it, gpointer) {
        const std::wstring on = L"true";
        bb::settings::Set(L"Bunbun.PillLyrics", gtk_check_menu_item_get_active(it) ? &on : nullptr);
    }), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), lyrics);
    // Media source: every player that's playing or paused, and "Automatic". Only worth a menu
    // when there is something to choose (several players, or a pick to undo).
    {
        const std::vector<bb::mpris::Source> sources = bb::mpris::Sources();
        const std::string picked = bb::mpris::Picked();
        if (sources.size() > 1 || !picked.empty()) {
            GtkWidget* sub = gtk_menu_new();
            GSList* group = nullptr;
            const auto add = [&](const std::string& id, const std::string& label, bool active) {
                GtkWidget* it = gtk_radio_menu_item_new_with_label(group, label.c_str());
                group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), active);
                g_signal_connect_data(it, "activate",
                                      G_CALLBACK(+[](GtkMenuItem*, gpointer d) { bb::mpris::Pick(*static_cast<std::string*>(d)); }),
                                      new std::string(id), +[](gpointer d, GClosure*) { delete static_cast<std::string*>(d); },
                                      static_cast<GConnectFlags>(0));
                gtk_menu_shell_append(GTK_MENU_SHELL(sub), it);
            };
            add("", "Automatic", picked.empty());
            for (const bb::mpris::Source& s : sources)
                add(s.id, s.name + (s.playing ? "  (playing)" : "  (paused)"), s.id == picked);
            GtkWidget* parent = gtk_menu_item_new_with_label("Media source");
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(parent), sub);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), parent);
        }
    }
    if (bb::lfeat::Get().tuckHotkey)
        item("Tuck away (Ctrl+Alt+H)", G_CALLBACK(+[](GtkMenuItem*, gpointer) { bb::lfeat::SetTucked(true); }));
    if (bb::lfeat::Get().timer) {  // Modules.TimerModule: the focus timer from the menu
        if (bb::lsrc::FocusState().active)
            item("Stop the focus timer", G_CALLBACK(+[](GtkMenuItem*, gpointer) { bb::lsrc::FocusStop(); Redraw(); }));
        else
            item("Focus for 25 minutes", G_CALLBACK(+[](GtkMenuItem*, gpointer) { bb::lsrc::FocusStart(25); Redraw(); }));
    }
    item("Open data folder", G_CALLBACK(+[](GtkMenuItem*, gpointer) {
        bb::plat::MakeDirs(bb::plat::ConfigDir());
        bb::plat::OpenWithDefault(bb::plat::ConfigDir());
    }));
    if (tray) {
        GtkWidget* login = gtk_check_menu_item_new_with_label("Start when you log in");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(login), bb::plat::AutostartEnabled());
        g_signal_connect(login, "toggled", G_CALLBACK(+[](GtkCheckMenuItem* it, gpointer) {
            bb::plat::SetAutostart(gtk_check_menu_item_get_active(it));
        }), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), login);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    item("Quit Bunbun", G_CALLBACK(+[](GtkMenuItem*, gpointer) { gtk_main_quit(); }));
    return menu;
}

void ShowMenu(GdkEvent* event) {
    GtkWidget* menu = BuildMenu(false);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* m, gpointer) {
        g_idle_add([](gpointer w) -> gboolean { gtk_widget_destroy(GTK_WIDGET(w)); return G_SOURCE_REMOVE; }, m);
    }), nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
}

// ── Window placement ────────────────────────────────────────────────────────

// GNOME Shell draws its top bar over every app window (Bunbun runs there through
// XWayland), so an island pushed to the very top would mostly hide behind it.
bool GnomeShell() {
    const char* d = getenv("XDG_CURRENT_DESKTOP");
    return d && (strstr(d, "GNOME") || strstr(d, "gnome"));
}

// The top the island hangs from. A notch (or the merged island) touches the screen's
// real edge, above the work area's top margin (top panels), unless GNOME's top bar
// would cover it; then it hangs right under the bar. Other shapes keep clear of panels.
int IslandTop(const GdkRectangle& area, const GdkRectangle& geo) {
    const bool flush = bb::llook::TopGap(1.0) == 0.0;
    const bool gnomeBar = GnomeShell() && area.y > geo.y;
    return flush && !gnomeBar ? geo.y : area.y;
}

// Places the window on the chosen monitor (Appearance.TargetMonitor), at the
// chosen edge and side (Position), moved by OffsetX / OffsetY; above other windows
// or not (Behavior.AlwaysOnTop).
void PlaceWindow() {
    const auto& f = bb::lfeat::Get();
    GdkDisplay* display = gtk_widget_get_display(g_window);
    GdkMonitor* mon = bb::lfeat::TargetMonitor(display);
    GdkRectangle area{0, 0, 1920, 1080}, geo{0, 0, 1920, 1080};
    if (mon) {
        gdk_monitor_get_workarea(mon, &area);
        gdk_monitor_get_geometry(mon, &geo);
    }
    // Separate displays: this display's own shape and size (its place is set by FollowPointerDisplay).
    double displaySize = 1.0;
    if (bb::lfeat::SeparateDisplays()) {
        int index = 0;
        for (int i = 0; i < gdk_display_get_n_monitors(display); ++i)
            if (gdk_display_get_monitor(display, i) == mon) index = i;
        const int shape = DisplayShapeOf(index + 1);
        bb::llook::SetShape(shape >= 0 ? static_cast<bb::llook::Shape>(shape) : bb::llook::BaseShape());
        displaySize = DisplaySizeOf(index + 1);
    } else {
        bb::llook::SetShape(bb::llook::BaseShape());
    }
    // Appearance.MatchScreenSize: the same share of every screen, 1080 high being 1x.
    // GDK's sizes are already in the desktop's scaled units, so a 4K screen at 200%
    // counts as 1080 (GTK doubles everything there itself) and at 100% as 2160. Never
    // below 1x: that would undo the desktop's own scaling (as on Windows with Auto DPI).
    g_scale = g_baseScale * ScreenShare(mon) * displaySize;
    const int w = static_cast<int>(std::ceil(kCanvasW * g_scale)), h = static_cast<int>(std::ceil(CanvasH() * g_scale));
    gtk_widget_set_size_request(g_window, w, h);
    gtk_window_resize(GTK_WINDOW(g_window), w, h);
    const int side = 12;  // from the screen's side for left / right
    const int dy = static_cast<int>(std::min<double>(g_offsetY, g_offsetY + ExpandedShift()));
    int x = f.align == 0 ? area.x + side : f.align == 2 ? area.x + area.width - w - side : area.x + (area.width - w) / 2;
    x += f.offsetX;
    if (g_layerShell) {
        gtk_layer_set_monitor(GTK_WINDOW(g_window), mon);
        gtk_layer_set_layer(GTK_WINDOW(g_window), f.onTop ? GTK_LAYER_SHELL_LAYER_TOP : GTK_LAYER_SHELL_LAYER_BOTTOM);
        gtk_layer_set_anchor(GTK_WINDOW(g_window), GTK_LAYER_SHELL_EDGE_TOP, !f.bottom);
        gtk_layer_set_anchor(GTK_WINDOW(g_window), GTK_LAYER_SHELL_EDGE_BOTTOM, f.bottom);
        gtk_layer_set_anchor(GTK_WINDOW(g_window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(g_window), f.bottom ? GTK_LAYER_SHELL_EDGE_BOTTOM : GTK_LAYER_SHELL_EDGE_TOP, dy);
        gtk_layer_set_margin(GTK_WINDOW(g_window), GTK_LAYER_SHELL_EDGE_LEFT, x - geo.x);
        // A notch touches the screen's edge itself: -1 ignores panels' exclusive zones
        // (0 would hang it below a top panel, leaving a gap).
        gtk_layer_set_exclusive_zone(GTK_WINDOW(g_window), !f.bottom && bb::llook::TopGap(1.0) == 0.0 ? -1 : 0);
        return;
    }
    gtk_window_set_keep_above(GTK_WINDOW(g_window), f.onTop);
    if (!f.onTop) gtk_window_set_keep_below(GTK_WINDOW(g_window), FALSE);
    const int y = f.bottom ? area.y + area.height - h - dy : IslandTop(area, geo) + dy;
    gtk_window_move(GTK_WINDOW(g_window), x, y);
}

// ── Mirrors on other displays ───────────────────────────────────────────────

// Appearance.MatchScreenSize: the same share of every screen, 1080 high being 1x.
double ScreenShare(GdkMonitor* mon) {
    if (!g_matchScreen || !mon) return 1.0;
    GdkRectangle geo{};
    gdk_monitor_get_geometry(mon, &geo);
    return std::clamp(geo.height / 1080.0, 1.0, 3.0);
}

gboolean OnMirrorDraw(GtkWidget* w, cairo_t* cr, gpointer data) {
    Mirror& m = g_mirrors[static_cast<size_t>(GPOINTER_TO_INT(data))];
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    // Separate: a full-screen app on this display (and the island isn't here): this
    // display's island hides, or folds to a line.
    if (bb::lfeat::SeparateDisplays() && bb::lfeat::FullscreenMonitor() == m.monitor && !g_fsFolded) {
        if (!bb::lfeat::FullscreenTuckNow()) return TRUE;
        const double tw = 60, th = 6;
        const double px = m.align == 0 ? 0 : m.align == 2 ? kCanvasW - tw : (kCanvasW - tw) / 2;
        const double canvasH = gtk_widget_get_allocated_height(w) / g_scale;
        const double py = m.bottom ? canvasH - kTopGap - th : 0;
        cairo_scale(cr, g_scale, g_scale);
        PaintTuck(cr, Rect{px, py, tw, th});
        return TRUE;
    }
    // Separate: while the island is open on its display (or folded for a full-screen
    // app there), the others show it at rest.
    const bool rest = bb::lfeat::SeparateDisplays() && (g_framePill.h > 60 || g_fsFolded) && g_restFrame;
    cairo_pattern_t* frame = rest ? g_restFrame : g_frame;
    const auto& fp = rest ? g_restPill : g_framePill;
    if (!frame || fp.w <= 0) return TRUE;
    // The main frame's pill (and its shadow), moved to where this mirror's pill sits.
    const double margin = 12;
    // Separate: this display's own shape and size, when they differ from the island's.
    GdkDisplay* gdisplay = gtk_widget_get_display(w);
    const double k = m.size * ScreenShare(gdk_display_get_monitor(gdisplay, m.monitor)) * g_baseScale / std::max(0.01, g_scale);
    const bb::llook::Shape mainShape = bb::llook::Get().shape;
    const bool own = bb::lfeat::SeparateDisplays() && ((m.shape >= 0 && m.shape != static_cast<int>(mainShape)) || std::fabs(k - 1) > 0.01);
    const bb::llook::Shape shape = m.shape >= 0 ? static_cast<bb::llook::Shape>(m.shape) : mainShape;
    const double tw = fp.w * (own ? k : 1), th = fp.h * (own ? k : 1);
    const double px = m.align == 0 ? 0 : m.align == 2 ? kCanvasW - tw : (kCanvasW - tw) / 2;
    const double canvasH = gtk_widget_get_allocated_height(w) / g_scale;
    const bool flush = own ? (shape == bb::llook::Shape::Notch) : bb::llook::TopGap(1.0) == 0.0;
    const double py = m.bottom ? canvasH - (flush ? 0 : kTopGap) - th : (own ? (flush ? 0 : kTopGap) : fp.y);
    m.px = px;
    m.py = py;
    if (own) {  // its surface in the island's colour, then the island's content scaled into it
        const auto& look = bb::llook::Get();
        cairo_save(cr);
        cairo_scale(cr, g_scale, g_scale);
        bb::llook::SurfacePathAs(cr, px, py, tw, th, bb::llook::RadiusAs(th, shape), shape, true);
        cairo_set_source_rgba(cr, look.fill[0], look.fill[1], look.fill[2], look.opacity);
        cairo_fill(cr);
        bb::llook::SurfacePathAs(cr, px, py, tw, th, bb::llook::RadiusAs(th, shape), shape, false);
        cairo_clip(cr);
        cairo_translate(cr, px, py);
        cairo_scale(cr, k, k);
        cairo_translate(cr, -fp.x, -fp.y);
        cairo_scale(cr, 1.0 / g_scale, 1.0 / g_scale);
        cairo_set_source(cr, frame);
        cairo_paint(cr);
        cairo_restore(cr);
        return TRUE;
    }
    // Linked: the pill takes the pointer (handed to the island); the rest lets it through.
    if (!bb::lfeat::SeparateDisplays()) {
        const cairo_rectangle_int_t in{static_cast<int>(std::floor(px * g_scale)), static_cast<int>(std::floor(py * g_scale)),
                                       static_cast<int>(std::ceil(fp.w * g_scale)), static_cast<int>(std::ceil(fp.h * g_scale))};
        if (in.x != m.input.x || in.y != m.input.y || in.width != m.input.width || in.height != m.input.height) {
            m.input = in;
            cairo_region_t* region = cairo_region_create_rectangle(&in);
            gtk_widget_input_shape_combine_region(m.window, region);
            cairo_region_destroy(region);
        }
    }
    cairo_scale(cr, g_scale, g_scale);
    cairo_rectangle(cr, px - margin, py - margin, fp.w + 2 * margin, fp.h + 2 * margin);
    cairo_clip(cr);
    cairo_translate(cr, px - fp.x, py - fp.y);
    cairo_scale(cr, 1.0 / g_scale, 1.0 / g_scale);
    cairo_set_source(cr, frame);
    cairo_paint(cr);
    return TRUE;
}

// A Linked mirror's pointer events, handed to the island at the matching point.
Mirror* MirrorOf(GtkWidget* w) {
    for (Mirror& m : g_mirrors)
        if (m.window == w) return &m;
    return nullptr;
}
template <typename Ev>
Ev ToIsland(const Mirror& m, const Ev* e) {
    Ev copy = *e;
    copy.x = (e->x / g_scale - m.px + g_framePill.x) * g_scale;
    copy.y = (e->y / g_scale - m.py + g_framePill.y) * g_scale;
    return copy;
}
gboolean OnMirrorPress(GtkWidget* w, GdkEventButton* e, gpointer) {
    if (Mirror* m = MirrorOf(w)) { GdkEventButton c = ToIsland(*m, e); return OnButtonPress(g_window, &c, nullptr); }
    return FALSE;
}
gboolean OnMirrorRelease(GtkWidget* w, GdkEventButton* e, gpointer) {
    if (Mirror* m = MirrorOf(w)) { GdkEventButton c = ToIsland(*m, e); return OnButtonRelease(g_window, &c, nullptr); }
    return FALSE;
}
gboolean OnMirrorMotion(GtkWidget* w, GdkEventMotion* e, gpointer) {
    if (Mirror* m = MirrorOf(w)) { GdkEventMotion c = ToIsland(*m, e); return OnMotion(g_window, &c, nullptr); }
    return FALSE;
}
gboolean OnMirrorScroll(GtkWidget* w, GdkEventScroll* e, gpointer) {
    if (Mirror* m = MirrorOf(w)) { GdkEventScroll c = ToIsland(*m, e); return OnScroll(g_window, &c, nullptr); }
    return FALSE;
}
gboolean OnMirrorCrossing(GtkWidget*, GdkEventCrossing* e, gpointer) { return OnCrossing(g_window, e, nullptr); }

// The placement for display `index` (0-based): its own entry in
// Appearance.DisplayPositions, else MirrorPosition, else Appearance.Position.
void DisplayPlacement(int index, bool isMain, int* align, bool* bottom) {
    namespace s = bb::settings;
    const auto parse = [&](const std::wstring& pos) {
        if (pos.empty() || pos == L"same") return;
        *bottom = pos.rfind(L"bottom", 0) == 0;
        *align = pos.find(L"left") != std::wstring::npos ? 0 : pos.find(L"right") != std::wstring::npos ? 2 : 1;
    };
    const std::wstring base = s::Get(L"Appearance.Position");
    *bottom = base.rfind(L"bottom", 0) == 0;
    *align = base.find(L"left") != std::wstring::npos ? 0 : base.find(L"right") != std::wstring::npos ? 2 : 1;
    if (isMain) return;
    parse(s::Get(L"Appearance.MirrorPosition"));
    const std::wstring per = s::Get(L"Appearance.DisplayPositions"), key = std::to_wstring(index + 1) + L"=";
    if (const size_t at = per.find(key); at != std::wstring::npos && (at == 0 || per[at - 1] == L';' || per[at - 1] == L' ')) {
        const size_t end = per.find(L';', at);
        parse(per.substr(at + key.size(), end == std::wstring::npos ? std::wstring::npos : end - at - key.size()));
    }
}

void PlaceMirror(Mirror& m) {
    const auto& f = bb::lfeat::Get();
    const int w = static_cast<int>(std::ceil(kCanvasW * g_scale)), h = static_cast<int>(std::ceil(kCanvasH * g_scale));
    gtk_widget_set_size_request(m.window, w, h);
    GdkDisplay* display = gtk_widget_get_display(m.window);
    GdkMonitor* mon = gdk_display_get_monitor(display, m.monitor);
    if (!mon) return;
    GdkRectangle area{}, geo{};
    gdk_monitor_get_workarea(mon, &area);
    gdk_monitor_get_geometry(mon, &geo);
    const int side = 12;
    int x = m.align == 0 ? area.x + side : m.align == 2 ? area.x + area.width - w - side : area.x + (area.width - w) / 2;
    x += f.offsetX;
    const int dy = static_cast<int>(g_offsetY);
    if (g_layerShell) {
        gtk_layer_set_monitor(GTK_WINDOW(m.window), mon);
        gtk_layer_set_anchor(GTK_WINDOW(m.window), GTK_LAYER_SHELL_EDGE_TOP, !m.bottom);
        gtk_layer_set_anchor(GTK_WINDOW(m.window), GTK_LAYER_SHELL_EDGE_BOTTOM, m.bottom);
        gtk_layer_set_anchor(GTK_WINDOW(m.window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(m.window), m.bottom ? GTK_LAYER_SHELL_EDGE_BOTTOM : GTK_LAYER_SHELL_EDGE_TOP, dy);
        gtk_layer_set_margin(GTK_WINDOW(m.window), GTK_LAYER_SHELL_EDGE_LEFT, x - geo.x);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(m.window), !m.bottom && bb::llook::TopGap(1.0) == 0.0 ? -1 : 0);
        return;
    }
    gtk_window_move(GTK_WINDOW(m.window), x, m.bottom ? area.y + area.height - h - dy : IslandTop(area, geo) + dy);
}

// Rebuilds the mirror windows for the current monitors and settings.
void RebuildMirrors() {
    for (Mirror& m : g_mirrors) gtk_widget_destroy(m.window);
    g_mirrors.clear();
    namespace s = bb::settings;
    if (s::Get(L"Appearance.Displays") != L"all" || !g_window) return;
    GdkDisplay* display = gtk_widget_get_display(g_window);
    GdkMonitor* main = bb::lfeat::TargetMonitor(display);
    const std::wstring same = s::Get(L"Appearance.MirrorPosition"), per = s::Get(L"Appearance.DisplayPositions");
    const auto parse = [](const std::wstring& pos, Mirror* m) {
        if (pos.empty() || pos == L"same") return;
        m->bottom = pos.rfind(L"bottom", 0) == 0;
        m->align = pos.find(L"left") != std::wstring::npos ? 0 : pos.find(L"right") != std::wstring::npos ? 2 : 1;
    };
    for (int i = 0; i < gdk_display_get_n_monitors(display); ++i) {
        if (gdk_display_get_monitor(display, i) == main) continue;
        Mirror m;
        m.monitor = i;
        m.align = bb::lfeat::Get().align;
        m.bottom = bb::lfeat::Get().bottom;
        if (bb::lfeat::SeparateDisplays()) {  // its own entry, else the island's set shape
            m.shape = DisplayShapeOf(i + 1);
            if (m.shape < 0) m.shape = static_cast<int>(bb::llook::BaseShape());
            m.size = DisplaySizeOf(i + 1);
        }
        parse(same, &m);
        // "2=bottom-center;3=top-left": display numbers start at 1.
        const std::wstring key = std::to_wstring(i + 1) + L"=";
        if (const size_t at = per.find(key); at != std::wstring::npos && (at == 0 || per[at - 1] == L';' || per[at - 1] == L' '))
            parse(per.substr(at + key.size(), per.find(L';', at) == std::wstring::npos ? std::wstring::npos : per.find(L';', at) - at - key.size()), &m);
        m.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(m.window), "Bunbun mirror");
        gtk_widget_set_app_paintable(m.window, TRUE);
        if (GdkVisual* rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(m.window))) gtk_widget_set_visual(m.window, rgba);
        gtk_window_set_decorated(GTK_WINDOW(m.window), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(m.window), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(m.window), TRUE);
        gtk_window_set_accept_focus(GTK_WINDOW(m.window), FALSE);
        if (g_layerShell) {
            gtk_layer_init_for_window(GTK_WINDOW(m.window));
            gtk_layer_set_namespace(GTK_WINDOW(m.window), "bunbun-mirror");
            gtk_layer_set_layer(GTK_WINDOW(m.window), GTK_LAYER_SHELL_LAYER_TOP);
            gtk_layer_set_exclusive_zone(GTK_WINDOW(m.window), 0);
            gtk_layer_set_keyboard_mode(GTK_WINDOW(m.window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        } else {
            gtk_window_set_type_hint(GTK_WINDOW(m.window), GDK_WINDOW_TYPE_HINT_DOCK);
            gtk_window_set_keep_above(GTK_WINDOW(m.window), TRUE);
            gtk_window_stick(GTK_WINDOW(m.window));
        }
        g_mirrors.push_back(m);
        g_signal_connect(m.window, "draw", G_CALLBACK(OnMirrorDraw), GINT_TO_POINTER(static_cast<int>(g_mirrors.size() - 1)));
        PlaceMirror(g_mirrors.back());
        gtk_widget_show_all(m.window);
        PlaceMirror(g_mirrors.back());
        // Until it's drawn (and always in Separate mode), clicks go through to what's below.
        cairo_region_t* none = cairo_region_create();
        gtk_widget_input_shape_combine_region(m.window, none);
        cairo_region_destroy(none);
        if (!bb::lfeat::SeparateDisplays()) {  // Linked: the mirror's pointer goes to the island
            gtk_widget_add_events(m.window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
                                                GDK_SMOOTH_SCROLL_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
            g_signal_connect(m.window, "button-press-event", G_CALLBACK(OnMirrorPress), nullptr);
            g_signal_connect(m.window, "button-release-event", G_CALLBACK(OnMirrorRelease), nullptr);
            g_signal_connect(m.window, "motion-notify-event", G_CALLBACK(OnMirrorMotion), nullptr);
            g_signal_connect(m.window, "scroll-event", G_CALLBACK(OnMirrorScroll), nullptr);
            g_signal_connect(m.window, "enter-notify-event", G_CALLBACK(OnMirrorCrossing), nullptr);
            g_signal_connect(m.window, "leave-notify-event", G_CALLBACK(OnMirrorCrossing), nullptr);
        }
    }
    Redraw();
}

// Separate: the island follows the pointer to another display, taking that
// display's place; the mirrors are rebuilt around it.
void FollowPointerDisplay() {
    if (!bb::lfeat::SeparateDisplays() || !g_window) return;
    GdkDisplay* display = gtk_widget_get_display(g_window);
    GdkMonitor* now = bb::lfeat::TargetMonitor(display);
    static GdkMonitor* last = nullptr;
    if (now == last) return;
    last = now;
    int index = 0;
    for (int i = 0; i < gdk_display_get_n_monitors(display); ++i)
        if (gdk_display_get_monitor(display, i) == now) index = i;
    // The configured main display (Appearance.TargetMonitor, primary by default) keeps Appearance.Position.
    const std::wstring target = bb::settings::Get(L"Appearance.TargetMonitor");
    GdkMonitor* primary = gdk_display_get_primary_monitor(display);
    const int n = static_cast<int>(wcstol(target.c_str(), nullptr, 10));
    const bool isMain = n >= 1 ? n - 1 == index : (primary ? primary == now : index == 0);
    int align = 1;
    bool bottom = false;
    DisplayPlacement(index, isMain, &align, &bottom);
    bb::lfeat::SetPlacement(align, bottom);
    bb::llook::SetEdge(bottom, bb::lfeat::Get().merged);
    PlaceWindow();
    RebuildMirrors();
}

// X11: a window mapped while the window manager or compositor is still
// starting (an autostart race at login) can end up never getting exposed;
// mapping it again fixes that. Also used when compositing turns on or off.
void Remap() {
    if (g_layerShell || !g_window) return;
    gtk_widget_hide(g_window);
    gtk_widget_show(g_window);
    PlaceWindow();
    Redraw();
}

void OnSettingsFileChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev, gpointer) {
    if (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && ev != G_FILE_MONITOR_EVENT_CREATED &&
        ev != G_FILE_MONITOR_EVENT_RENAMED && ev != G_FILE_MONITOR_EVENT_MOVED_IN)
        return;
    LoadSettings();
    bb::lfeat::ApplySettingsChange();
    PlaceWindow();
    RebuildMirrors();
    bb::ltray::Refresh();  // your character may have changed, and the menu's switches
    Redraw();
}

// ── Single instance ─────────────────────────────────────────────────────────

std::string LockPath() { return bb::plat::RuntimeDir() + "/island.lock"; }

// Holds an flock for the life of the process; the file names our pid for --quit.
bool TakeInstanceLock() {
    const int fd = open(LockPath().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    if (ftruncate(fd, 0) == 0) {
        const std::string pid = std::to_string(getpid()) + "\n";
        if (write(fd, pid.data(), pid.size()) < 0) {}
    }
    return true;  // fd stays open on purpose
}

// Asks a running island to quit (SIGTERM) and waits up to 3 s for it.
bool QuitRunning() {
    std::string text;
    if (!bb::plat::ReadFile(LockPath(), &text)) return false;
    const pid_t pid = static_cast<pid_t>(atoi(text.c_str()));
    if (pid <= 1 || pid == getpid() || kill(pid, SIGTERM) != 0) return false;
    for (int i = 0; i < 60; ++i) {
        if (kill(pid, 0) != 0) return true;
        usleep(50 * 1000);
    }
    return false;
}

// Under the AppImage, our folder is a new temporary mount every run; agents'
// hooks must point at a copy of bunbun-hook that stays put.
void InstallStableHook() {
    if (!getenv("APPIMAGE")) return;
    const std::string dst = bb::plat::HomeDir() + "/.local/share/Bunbun/bin/bunbun-hook";
    std::string data;
    if (!bb::plat::ReadFile(bb::plat::ExeDir() + "/bunbun-hook", &data)) return;
    std::string old;
    if (bb::plat::ReadFile(dst, &old) && old == data) return;
    if (bb::plat::WriteFileAtomic(dst, data)) chmod(dst.c_str(), 0755);
}

void ShowToast(const std::string& title, const std::string& text, double seconds, double level) {
    g_toastWater = 0;
    g_toastTitle = title;
    g_toastText = text;
    g_toastLevel = level;
    g_toastStart = Now();
    g_toastSeconds = seconds;
    g_toastUntil = Now() + seconds;
    bb::lfeat::NoteInteraction();
    g_nudgeAt = Now();
    Redraw();
}

}  // namespace bb::lisland

using namespace bb::lisland;

// ── Entry points used by shared code ────────────────────────────────────────

double IslandNow() { return Now(); }

void IslandSetAgentView(const AgentView& view, bool nudge) {
    {
        std::lock_guard lock(g_agentMutex);
        g_agentInbox = view;
        g_agentInboxNew = true;
        g_agentInboxNudge = g_agentInboxNudge || nudge;
    }
    g_idle_add(ApplyAgentView, nullptr);
}

void IslandPickColour() {
    g_idle_add([](gpointer) -> gboolean { PickColour(); return G_SOURCE_REMOVE; }, nullptr);
}

void IslandToggleHidden() {
    g_idle_add([](gpointer) -> gboolean { bb::lfeat::ToggleHidden(); bb::ltray::Refresh(); Redraw(); return G_SOURCE_REMOVE; }, nullptr);
}

void IslandToggleTucked() {
    g_idle_add([](gpointer) -> gboolean { bb::lfeat::SetTucked(!bb::lfeat::Tucked()); Redraw(); return G_SOURCE_REMOVE; }, nullptr);
}

int IslandCapturePages() {
    struct Job {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        int count = 0;
    };
    auto job = std::make_shared<Job>();
    g_idle_add([](gpointer p) -> gboolean {
        auto* j = static_cast<std::shared_ptr<Job>*>(p);
        const int count = StudioCapturePages();
        {
            std::lock_guard lock((*j)->m);
            (*j)->count = count;
            (*j)->done = true;
        }
        (*j)->cv.notify_all();
        delete j;
        return G_SOURCE_REMOVE;
    }, new std::shared_ptr<Job>(job));
    std::unique_lock lock(job->m);
    return job->cv.wait_for(lock, std::chrono::seconds(5), [&] { return job->done; }) ? job->count : -1;
}

int main(int argc, char** argv) {
    bool replace = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quit") return QuitRunning() ? 0 : 1;
        if (a == "--replace") replace = true;
        // The desktop file's Settings action. Exec keys can't hold shell code, so
        // the island finds bunbun-settings next to itself.
        if (a == "--settings") {
            const std::string exe = bb::plat::ExeDir() + "/bunbun-settings";
            bb::settings::EnsureFile();
            execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
            return bb::plat::OpenWithDefault(bb::settings::Path()) ? 0 : 1;
        }
        // Asks the running island to start the colour picker, hide / show, or tuck:
        // bind these to keys in your desktop's keyboard settings (Wayland has no global
        // hotkeys for apps; on GNOME the island adds the hide and tuck ones itself).
        const char* command = a == "--pick-colour" || a == "--pick-color" ? "pick_colour"
                              : a == "--toggle"                          ? "toggle"
                              : a == "--tuck"                            ? "tuck"
                                                                         : nullptr;
        if (command) {
            const std::string sock = bb::plat::RuntimeDir() + "/agent.sock";
            const std::string line = std::string("{\"bunbun_command\":\"") + command + "\"}\n";
            const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, sock.c_str(), sizeof addr.sun_path - 1);
            const bool ok = fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 &&
                            write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
            if (fd >= 0) close(fd);
            if (!ok) fprintf(stderr, "Bunbun isn't running.\n");
            return ok ? 0 : 1;
        }
        if (a == "--version") {
            printf("Bunbun %s\n", BUNBUN_VERSION_STR);
            return 0;
        }
        if (a == "--help" || a == "-h") {
            printf("Usage: bunbun [--replace | --quit | --settings | --toggle | --tuck | --pick-colour | --version |"
                   " --render-icon FILE.png [SIZE]]\n");
            return 0;
        }
        // Packaging draws the app icon with the real mascot code (Bun, idle).
        if (a == "--render-icon" && i + 1 < argc) {
            const int px = i + 2 < argc ? std::clamp(atoi(argv[i + 2]), 16, 1024) : 256;
            const std::string png = bb::plat::RenderMascotPng(mascot::Style{}, mascot::Mood::Idle, px, 0.6, -10, {0, 0});
            return !png.empty() && bb::plat::WriteFileAtomic(argv[i + 1], png) ? 0 : 1;
        }
    }
    if (replace) QuitRunning();
    if (!TakeInstanceLock()) {
        fprintf(stderr, "Bunbun is already running (bunbun --replace restarts it).\n");
        return 0;
    }

    gtk_init(&argc, &argv);
    GdkDisplay* display = gdk_display_get_default();
#ifdef GDK_WINDOWING_WAYLAND
    // Wayland without layer-shell (GNOME): a normal Wayland window can't place
    // itself or stay on top, so run through XWayland instead.
    if (GDK_IS_WAYLAND_DISPLAY(display) && !gtk_layer_is_supported()) {
        if (!getenv("BUNBUN_NO_XWAYLAND_FALLBACK") && getenv("DISPLAY")) {
            setenv("GDK_BACKEND", "x11", 1);
            setenv("BUNBUN_NO_XWAYLAND_FALLBACK", "1", 1);
            unlink(LockPath().c_str());
            execv("/proc/self/exe", argv);
        }
        fprintf(stderr, "Bunbun: this Wayland compositor has no layer-shell and there's no XWayland; see docs/linux.md.\n");
    }
    g_layerShell = GDK_IS_WAYLAND_DISPLAY(display) && gtk_layer_is_supported();
#endif
    // The AppImage's bundled libraries (AppRun's LD_LIBRARY_PATH) are loaded by now.
    // Put the caller's path back, so what the island starts (the settings window and
    // its system WebKitGTK, links, music apps) doesn't load them too. Only after the
    // XWayland re-exec above, which still needs them.
    if (getenv("BUNBUN_APPIMAGE")) {
        const char* parent = getenv("BUNBUN_PARENT_LD_LIBRARY_PATH");
        if (parent && *parent) setenv("LD_LIBRARY_PATH", parent, 1);
        else unsetenv("LD_LIBRARY_PATH");
        unsetenv("BUNBUN_PARENT_LD_LIBRARY_PATH");
    }

    bb::settings::EnsureFile();
    LoadSettings();
    InstallStableHook();
    g_rt = bbcompat::CreateCairoTarget();

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "Bunbun");
    gtk_widget_set_app_paintable(g_window, TRUE);
    if (GdkVisual* rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(g_window)))
        gtk_widget_set_visual(g_window, rgba);
    gtk_window_set_decorated(GTK_WINDOW(g_window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(g_window), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(g_window), FALSE);
    if (g_layerShell) {
        gtk_layer_init_for_window(GTK_WINDOW(g_window));
        gtk_layer_set_namespace(GTK_WINDOW(g_window), "bunbun");
        gtk_layer_set_layer(GTK_WINDOW(g_window), GTK_LAYER_SHELL_LAYER_TOP);
        gtk_layer_set_anchor(GTK_WINDOW(g_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(g_window), 0);  // sit below top panels, reserve no space
        gtk_layer_set_keyboard_mode(GTK_WINDOW(g_window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    } else {
        // A dock stays above normal windows but under fullscreen ones, and
        // window managers place it where we ask. BUNBUN_X11_OVERRIDE=1 uses an
        // unmanaged (override-redirect) window instead, for window managers
        // that mishandle docks.
        gtk_window_set_type_hint(GTK_WINDOW(g_window), GDK_WINDOW_TYPE_HINT_DOCK);
        gtk_window_set_keep_above(GTK_WINDOW(g_window), TRUE);
        gtk_window_stick(GTK_WINDOW(g_window));
        if (getenv("BUNBUN_X11_OVERRIDE")) {
            gtk_widget_realize(g_window);
            gdk_window_set_override_redirect(gtk_widget_get_window(g_window), TRUE);
        }
    }
    gtk_widget_add_events(g_window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK |
                                        GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
                                        GDK_KEY_PRESS_MASK | GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(g_window, "draw", G_CALLBACK(OnDraw), nullptr);
    g_signal_connect(g_window, "button-press-event", G_CALLBACK(OnButtonPress), nullptr);
    g_signal_connect(g_window, "button-release-event", G_CALLBACK(OnButtonRelease), nullptr);
    // The File shelf: drop files on the island, drag them out again.
    {
        GtkTargetList* targets = gtk_target_list_new(nullptr, 0);
        gtk_target_list_add_uri_targets(targets, 0);
        gtk_drag_dest_set(g_window, GTK_DEST_DEFAULT_ALL, nullptr, 0, static_cast<GdkDragAction>(GDK_ACTION_COPY | GDK_ACTION_LINK));
        gtk_drag_dest_set_target_list(g_window, targets);
        gtk_target_list_unref(targets);
    }
    g_signal_connect(g_window, "drag-data-received", G_CALLBACK(OnDragReceived), nullptr);
    g_signal_connect(g_window, "drag-data-get", G_CALLBACK(OnDragGet), nullptr);
    g_signal_connect(g_window, "motion-notify-event", G_CALLBACK(OnMotion), nullptr);
    g_signal_connect(g_window, "enter-notify-event", G_CALLBACK(OnCrossing), nullptr);
    g_signal_connect(g_window, "leave-notify-event", G_CALLBACK(OnCrossing), nullptr);
    g_signal_connect(g_window, "scroll-event", G_CALLBACK(OnScroll), nullptr);
    g_signal_connect(g_window, "key-press-event", G_CALLBACK(OnKey), nullptr);
    g_signal_connect(g_window, "focus-out-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean {
        ComposeFocusOut();
        return FALSE;
    }), nullptr);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    PlaceWindow();
    gtk_widget_show_all(g_window);
    PlaceWindow();

    GFile* settingsFile = g_file_new_for_path(bb::settings::Path().c_str());
    GFileMonitor* monitor = g_file_monitor_file(settingsFile, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    if (monitor) g_signal_connect(monitor, "changed", G_CALLBACK(OnSettingsFileChanged), nullptr);

    for (int sig : {SIGTERM, SIGINT, SIGHUP})
        g_unix_signal_add(sig, [](gpointer) -> gboolean { gtk_main_quit(); return G_SOURCE_REMOVE; }, nullptr);

    g_lastDrawAt = Now();
    bb::llyrics::Start([] { Redraw(); });
    bb::mpris::Start(OnMediaChanged);
    bb::lsrc::Start([] { Redraw(); });
    bb::lcal::Start([] { Redraw(); });
    bb::ltray::Start([] { return BuildMenu(true); });
    bb::llook::StartPrivacy([] {
        // The screen dot came on: say who and how, as Windows does.
        static bool screenWas = false;
        const bool screen = bb::llook::ScreenDotOn();  // the toast goes with the dot, not with Live alone
        if (screen && !screenWas) ShowToast("Screen", bb::llook::ScreenText(), 3.5);
        screenWas = screen;
        Redraw();
    });
    bb::lfeat::Start([](const std::string& title, const std::string& text, double seconds, double level) {
        ShowToast(title, text, seconds, level);
    }, [] { Redraw(); });
    if (!bb::agents::Start()) fprintf(stderr, "Bunbun: the agent socket is in use; agents won't show.\n");
    // The clock needs a redraw when the minute (or second) turns over. Every
    // frame path also passes through here, so no draw for a few seconds means
    // the window isn't being exposed (see Remap).
    g_timeout_add_seconds(1, [](gpointer) -> gboolean {
        if (Now() - g_lastDrawAt > 3) {
            g_lastDrawAt = Now();
            Remap();
        }
        // Reminders that came due, and the end of a focus timer.
        for (const std::wstring& text : bb::lsrc::TakeDueReminders()) {
            ShowToast("Reminder", bb::plat::Utf8(text), 10);
            // Modules.ReminderSound, kept quiet in full screen or while focusing (Modules.QuietReminders).
            const auto& f = bb::lfeat::Get();
            const bool quiet = f.quietReminders && (bb::lfeat::FullscreenAppActive() || bb::lsrc::FocusState().running);
            if (f.reminderSound && !quiet) bb::llook::PlayChime(bb::chime::Chime::Question);
        }
        // A Pomodoro phase or a countdown ran out: say what's next (timers_linux.cpp).
        if (const int k = bb::lsrc::FocusTakeFinishedKind(); k >= 0)
            ShowToast(k == bb::ltimers::kKindPomodoro ? "Pomodoro" : k == bb::ltimers::kKindTimer ? "Timer" : "Focus",
                      bb::ltimers::OnFinished(k), 10);
        bb::ltimers::Tick();  // the next Pomodoro phase, when it starts by itself
        // Work mode (work_linux.cpp), every 5 s: the day's log and its notes.
        static int s_workTick = 0;
        if (++s_workTick >= 5) {
            s_workTick = 0;
            for (const auto& [title, text] : bb::lwork::Tick()) ShowToast(title, text, 6);
        }
        bb::lscreen::Tick();  // Screen Time: the app in front gets this second
        // The water reminder (Bunbun.WaterReminders), every 30 s.
        static int s_waterTick = 0;
        if (++s_waterTick >= 30) {
            s_waterTick = 0;
            const bb::lwater::Due due = bb::lwater::Check();
            if (due != bb::lwater::Due::None) {
                const bb::lwater::Snapshot w = bb::lwater::Now();
                const bool refill = due == bb::lwater::Due::Refill;
                ShowToast("Water \xC2\xB7 " + std::to_string(w.takenMl) + " of " + std::to_string(w.goalMl) + " ml",
                          refill ? "Time to refill your bottle \xC2\xB7 click when you have"
                                 : "Time to drink water \xC2\xB7 click to log a glass",
                          20);
                g_toastWater = refill ? 2 : 1;
                const auto& f = bb::lfeat::Get();
                const bool quiet = f.quietReminders && (bb::lfeat::FullscreenAppActive() || bb::lsrc::FocusState().running);
                if (f.reminderSound && !quiet) bb::llook::PlayChime(bb::chime::Chime::Question);
            }
        }
        gtk_widget_queue_draw(g_window);
        return G_SOURCE_CONTINUE;
    }, nullptr);
    g_signal_connect(gtk_widget_get_screen(g_window), "composited-changed", G_CALLBACK(+[](GdkScreen*, gpointer) { Remap(); }),
                     nullptr);
    // Monitors coming and going: re-place the island and its mirrors.
    RebuildMirrors();
    // Separate displays: the island goes to the display the pointer is on.
    g_timeout_add(250, [](gpointer) -> gboolean { FollowPointerDisplay(); return G_SOURCE_CONTINUE; }, nullptr);
    for (const char* sig : {"monitor-added", "monitor-removed"})
        g_signal_connect(display, sig, G_CALLBACK(+[](GdkDisplay*, GdkMonitor*, gpointer) {
            PlaceWindow();
            RebuildMirrors();
        }), nullptr);
    // A new resolution: Match screen size re-sizes the island for it.
    g_signal_connect(gtk_widget_get_screen(g_window), "size-changed", G_CALLBACK(+[](GdkScreen*, gpointer) {
        PlaceWindow();
        RebuildMirrors();
        Redraw();
    }), nullptr);

    gtk_main();

    bb::lscreen::Save();  // today's Screen Time
    bb::agents::Stop();
    bb::mpris::Stop();
    if (monitor) g_object_unref(monitor);
    g_object_unref(settingsFile);
    unlink(LockPath().c_str());
    return 0;
}
