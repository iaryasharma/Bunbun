// Bunbun — what island_linux.cpp and studio_linux.cpp share: the island's state
// on the GTK main thread, its drawing helpers, and the Studio entry points.
// Internal to the Linux island (the shared code's side is island_api.h).
#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

#include "../../agents/agent_view.h"
#include "compat/d2d1.h"
#include "mpris.h"

namespace bb::lisland {

// ── Layout (logical pixels; GTK scales for HiDPI) ───────────────────────────

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool Contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

enum class Action {
    None, PlayPause, Next, Previous, Seek, Allow, Deny, Terminal, Answer, Settings,
    // Studio (studio_linux.cpp)
    Tab, OpenLink, OpenMusic, AgentPage, ModelCycle,
    PlanApprove, PlanChange, QuestionToggle, QuestionNext, QuestionBack, QuestionOther, Reply,
    ComposeSend, ComposeCancel,
    // Pages: clipboard, note, to-dos, focus timer, quick toggles, all apps
    ClipCopy, ClipClear, NoteToggle, NoteEdit, TodoToggle, TodoOpen,
    FocusStart, FocusPause, FocusAdd, FocusStop, FocusPick, Radio, OpenPage,
    // The meeting pill (calendar_linux.cpp)
    MeetingJoin, MeetingDismiss,
    // File shelf and Mirror pages
    ShelfOpen, ShelfRemove, ShelfClear, MirrorToggle,
    // The colour picker: start it, copy a row of the colour card
    ColorPick, ColorCopy,
    // The Water page (water_linux.cpp)
    WaterAdd, WaterUndo, WaterRefill,
    // The Studio header's close button; Weather's unit and city; the System page's radios
    Close, WeatherUnit, WeatherCity, RadioToggle, RadioSettings,
    // The Timers page (timers_linux.cpp)
    TimerMode, PomoToggle, PomoSkip, PomoReset, PomoAuto, PomoLength, CountToggle, CountAdd, CountReset,
    CountPreset, CountSave, TimerRing, SwToggle, SwLap, SwReset, SwCopy,
    VolumeMute, VolumeSet,  // the System page's volume card
    MediaSource,            // the music page's player name: show the next running player
    WorkStart, WorkPause, WorkResume, WorkBreak, WorkDone, WorkReport, WorkPage,  // Work mode (work_linux.cpp)
    ScreenTimeLimit,        // the Screen Time page's limit: steps through 2 to 12 hours
    ScreenTimeDay,          // a bar of the week: that day's time and apps (index = yyyymmdd)
    ClipFilter, ClipSelect, ClipDelete,  // the Clipboard page's two panels
};
struct Hit {
    Rect r;
    Action action;
    int index = -1;
};

// ── State (all on the GTK main thread) ──────────────────────────────────────

extern GtkWidget* g_window;
extern bool g_layerShell;
extern double g_scale;
extern bool g_mascotOn, g_agentsOn, g_agentPill, g_mediaOn, g_clock24, g_showSeconds;
extern bool g_hover;
extern double g_mouseX, g_mouseY;
extern std::vector<Hit> g_hits;
extern int g_hoverHit;
extern bb::mpris::Track g_track;
extern GdkPixbuf* g_art;
extern AgentView g_agent;
extern ID2D1RenderTarget* g_rt;

double Now();
void Redraw();
void OpenSettings(const char* page = nullptr);  // page: "notes", "todos"…

// ── Drawing helpers ─────────────────────────────────────────────────────────

void RoundRect(cairo_t* cr, double x, double y, double w, double h, double r);
// Text in a box, one line, ellipsized; returns its width.
double Text(cairo_t* cr, const std::string& text, double x, double y, double maxW, double size, bool bold,
            double r = 1, double g = 1, double b = 1, double a = 1, bool mono = false, int lines = 1);
double TextWidth(cairo_t* cr, const std::string& text, double size, bool bold);
void DrawMascot(cairo_t* cr, double cx, double cy, double size);
void Art(cairo_t* cr, double x, double y, double size, double alpha);
std::string ClockText();
std::string AgentLine(const AgentView& a);
// True when the pointer is on the hit about to be pushed (g_hits.size()).
inline bool NextHitHovered() { return static_cast<int>(g_hits.size()) == g_hoverHit; }

// ── Studio (studio_linux.cpp) ───────────────────────────────────────────────

void StudioLoadSettings();
bool StudioLayout();                      // Bunbun.Layout = studio
// 0 = the media view (something is playing), 1 = the idle view.
int StudioContext();
// Opens the Agent page the next time the frame opens (hovering an agent pill).
void StudioShowAgentPage();
// Opens the File shelf next time (files were just dropped on the island).
void StudioShowShelf();
bool StudioOnMirror();  // the Mirror page is the one showing
// The frame: header, the current page, tab bar. `mix` fades it in (0..1).
void PaintStudio(cairo_t* cr, const Rect& island, double mix);
void StudioAccentRgb(double rgb[3]);
void CloseIsland();
// The wheel over the Timers dial sets its time; true when it did (else the wheel turns pages).
bool StudioScrollTimer(double delta, bool fine);  // the header's close button: fold, and stay folded until the pointer leaves  // the island's accent (album art or custom), for the edge light
// The wheel or a touchpad over the open frame: the next or previous page.
void StudioScroll(double delta);
// Plan and several-question cards (drawn instead of the plain approval card).
bool RichCardShown();
void RichCardSize(cairo_t* cr, double* w, double* h);
void PaintRichCard(cairo_t* cr, const Rect& p);
// Studio's own actions (tabs, links, cards, compose, pages). False if not one of them.
bool StudioDo(const Hit& h);
// Lyrics on the small island (Bunbun.PillLyrics): the pill's width for this song
// (0 = no usable lyrics), given the room the pill needs besides the text and the widest it
// may grow (maxWidth); and the line itself, drawn into `area`.
double LyricPillWidth(cairo_t* cr, double chrome, double maxWidth);
// The width of `text` as the lyric pill draws it (its font and size).
double PillTextWidth(cairo_t* cr, const std::string& text);
void PaintLyricPill(cairo_t* cr, const Rect& area);
// A short message on the island (a reminder, the focus timer), for `seconds`.
// level 0..1 draws a bar (volume, brightness); < 0 none.
void ShowToast(const std::string& title, const std::string& text, double seconds, double level = -1);
// Saves a picture of every Studio page for Settings → Island pages; returns how many.
int StudioCapturePages();

// Typing on the island: a reply, a plan change, an answer of your own.
bool Composing();
bool ComposeKey(GdkEventKey* e);  // true when the key was used
void ComposeFocusOut();

}  // namespace bb::lisland
