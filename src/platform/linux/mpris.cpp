// Bunbun — MPRIS players (see mpris.h).
#include "mpris.h"

#include <gio/gio.h>

#include <map>
#include <memory>

#include "../../common/media_pick.h"

namespace bb::mpris {
namespace {

constexpr char kPrefix[] = "org.mpris.MediaPlayer2.";
constexpr char kPath[] = "/org/mpris/MediaPlayer2";

struct Player {
    std::string bus;            // org.mpris.MediaPlayer2.spotify (or .instance123)
    GDBusProxy* player = nullptr;  // org.mpris.MediaPlayer2.Player
    std::string identity;
    gint64 lastPlaying = 0;     // monotonic µs of the last Playing we saw
    double position = 0;        // seconds, as of positionAt
    gint64 positionAt = 0;      // monotonic µs; 0 = ask again
    bool asking = false;
};

GDBusConnection* g_bus = nullptr;
guint g_ownerSub = 0;
guint g_seekSub = 0;
std::map<std::string, std::unique_ptr<Player>> g_players;
std::function<void()> g_onChange;
std::string g_picked;  // the player chosen by hand ("" = automatic), forgotten when it stops

std::string StringProp(GDBusProxy* proxy, const char* name) {
    GVariant* v = g_dbus_proxy_get_cached_property(proxy, name);
    std::string out;
    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) out = g_variant_get_string(v, nullptr);
    if (v) g_variant_unref(v);
    return out;
}

bool IsPlaying(const Player& p) { return p.player && StringProp(p.player, "PlaybackStatus") == "Playing"; }

void Changed() {
    for (auto& [bus, p] : g_players)
        if (IsPlaying(*p)) p->lastPlaying = g_get_monotonic_time();
    if (g_onChange) g_onChange();
}

void OnProps(GDBusProxy* proxy, GVariant*, GStrv, gpointer) {
    // A new track, a pause or a resume: the position needs asking again.
    for (auto& [bus, p] : g_players)
        if (p->player == proxy) p->positionAt = 0;
    Changed();
}

// Seeked(x position) comes from the player's unique bus name.
void OnSeeked(GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer) {
    gint64 us = 0;
    g_variant_get(params, "(x)", &us);
    for (auto& [bus, p] : g_players) {
        gchar* owner = g_dbus_proxy_get_name_owner(p->player);
        if (owner && sender && g_strcmp0(owner, sender) == 0) {
            p->position = us / 1e6;
            p->positionAt = g_get_monotonic_time();
        }
        g_free(owner);
    }
}

// The app's own name (Identity), read once from the root interface.
void ReadIdentity(Player* p) {
    GVariant* r = g_dbus_connection_call_sync(g_bus, p->bus.c_str(), kPath, "org.freedesktop.DBus.Properties", "Get",
                                              g_variant_new("(ss)", "org.mpris.MediaPlayer2", "Identity"),
                                              G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
    if (!r) return;
    GVariant* inner = nullptr;
    g_variant_get(r, "(v)", &inner);
    if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING)) p->identity = g_variant_get_string(inner, nullptr);
    if (inner) g_variant_unref(inner);
    g_variant_unref(r);
}

void Add(const std::string& bus) {
    if (g_players.count(bus)) return;
    auto p = std::make_unique<Player>();
    p->bus = bus;
    p->player = g_dbus_proxy_new_sync(g_bus, G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES, nullptr, bus.c_str(), kPath,
                                      "org.mpris.MediaPlayer2.Player", nullptr, nullptr);
    if (!p->player) return;
    g_signal_connect(p->player, "g-properties-changed", G_CALLBACK(OnProps), nullptr);
    ReadIdentity(p.get());
    if (p->identity.empty()) p->identity = bus.substr(sizeof kPrefix - 1);
    if (IsPlaying(*p)) p->lastPlaying = g_get_monotonic_time();
    g_players[bus] = std::move(p);
}

void Remove(const std::string& bus) {
    auto it = g_players.find(bus);
    if (it == g_players.end()) return;
    g_object_unref(it->second->player);
    g_players.erase(it);
}

void OnOwnerChanged(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer) {
    const gchar *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
    g_variant_get(params, "(&s&s&s)", &name, &oldOwner, &newOwner);
    if (!g_str_has_prefix(name, kPrefix)) return;
    if (newOwner && *newOwner) Add(name);
    else Remove(name);
    Changed();
}

mediapick::State StateOf(const Player& p) {
    const std::string status = StringProp(p.player, "PlaybackStatus");
    if (status == "Playing") return mediapick::State::Playing;
    if (status == "Paused") return mediapick::State::Paused;
    return mediapick::State::Stopped;  // "Stopped", or no status yet
}

// The player on show. Bunbun: a pick made by hand stays until that player stops, and with no
// pick the playing one wins, then the most recent (common/media_pick.h). It used to be
// "whichever started last", so a video in the browser took over from the music and
// there was no way back.
Player* Best() {
    std::vector<mediapick::Candidate> candidates;
    std::vector<Player*> players;
    for (auto& [bus, p] : g_players) {
        if (!p->player) continue;
        candidates.push_back({bus, StateOf(*p), p->lastPlaying});
        players.push_back(p.get());
    }
    bool pickGone = false;
    const int i = mediapick::Choose(candidates, g_picked, &pickGone);
    if (pickGone) g_picked.clear();
    return i < 0 ? nullptr : players[static_cast<size_t>(i)];
}

void Call(const char* method) {
    if (Player* p = Best())
        g_dbus_proxy_call(p->player, method, nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
}

}  // namespace

void Start(std::function<void()> onChange) {
    g_onChange = std::move(onChange);
    g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!g_bus) return;  // no session bus: no media, the island still runs
    // arg0namespace takes the namespace without its trailing dot.
    g_ownerSub = g_dbus_connection_signal_subscribe(g_bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                                                    "/org/freedesktop/DBus", "org.mpris.MediaPlayer2",
                                                    G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE, OnOwnerChanged, nullptr, nullptr);
    GVariant* r = g_dbus_connection_call_sync(g_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                              "ListNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 1000,
                                              nullptr, nullptr);
    g_seekSub = g_dbus_connection_signal_subscribe(g_bus, nullptr, "org.mpris.MediaPlayer2.Player", "Seeked", kPath, nullptr,
                                                   G_DBUS_SIGNAL_FLAGS_NONE, OnSeeked, nullptr, nullptr);
    if (r) {
        GVariantIter* it = nullptr;
        const gchar* name = nullptr;
        g_variant_get(r, "(as)", &it);
        while (g_variant_iter_loop(it, "&s", &name))
            if (g_str_has_prefix(name, kPrefix)) Add(name);
        g_variant_iter_free(it);
        g_variant_unref(r);
    }
    Changed();
}

void Stop() {
    while (!g_players.empty()) Remove(g_players.begin()->first);
    if (g_bus && g_ownerSub) g_dbus_connection_signal_unsubscribe(g_bus, g_ownerSub);
    if (g_bus && g_seekSub) g_dbus_connection_signal_unsubscribe(g_bus, g_seekSub);
    g_ownerSub = g_seekSub = 0;
    if (g_bus) g_object_unref(g_bus);
    g_bus = nullptr;
}

Track Current() {
    Track t;
    Player* p = Best();
    if (!p) return t;
    t.active = true;
    t.playing = IsPlaying(*p);
    t.player = p->identity;
    t.playerId = p->bus;
    if (GVariant* md = g_dbus_proxy_get_cached_property(p->player, "Metadata")) {
        GVariantDict dict;
        g_variant_dict_init(&dict, md);
        const gchar* s = nullptr;
        if (g_variant_dict_lookup(&dict, "xesam:title", "&s", &s)) t.title = s;
        if (g_variant_dict_lookup(&dict, "xesam:album", "&s", &s)) t.album = s;
        if (g_variant_dict_lookup(&dict, "mpris:artUrl", "&s", &s)) t.artUrl = s;
        if (GVariant* artists = g_variant_dict_lookup_value(&dict, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY)) {
            gsize n = 0;
            const gchar** list = g_variant_get_strv(artists, &n);
            for (gsize i = 0; i < n; ++i) t.artist += (i ? ", " : "") + std::string(list[i]);
            g_free(list);
            g_variant_unref(artists);
        }
        if (GVariant* len = g_variant_dict_lookup_value(&dict, "mpris:length", nullptr)) {
            if (g_variant_is_of_type(len, G_VARIANT_TYPE_INT64)) t.length = g_variant_get_int64(len) / 1e6;
            else if (g_variant_is_of_type(len, G_VARIANT_TYPE_UINT64)) t.length = g_variant_get_uint64(len) / 1e6;
            g_variant_unref(len);
        }
        g_variant_dict_clear(&dict);
        g_variant_unref(md);
    }
    if (t.title.empty() && !t.playing) t.active = false;  // a paused player with nothing loaded
    return t;
}

std::vector<Source> Sources() {
    const Player* shown = Best();
    std::vector<Source> out;
    std::vector<std::string> names;
    for (auto& [bus, p] : g_players) {
        if (!p->player || StateOf(*p) == mediapick::State::Stopped) continue;  // not running anything
        Source s;
        s.id = bus;
        s.playing = IsPlaying(*p);
        s.shown = p.get() == shown;
        out.push_back(s);
        names.push_back(p->identity);
    }
    const std::vector<std::string> unique = mediapick::UniqueNames(names);
    for (size_t i = 0; i < out.size(); ++i) out[i].name = unique[i];
    return out;
}

void Pick(const std::string& id) {
    if (!id.empty() && !g_players.count(id)) return;
    g_picked = id;
    Changed();  // the island redraws with the new player
}

std::string Picked() {
    Best();  // forgets a pick whose player has stopped
    return g_picked;
}

void CycleSource() {
    std::vector<std::string> ids;
    std::string shown;
    for (const Source& s : Sources()) {
        ids.push_back(s.id);
        if (s.shown) shown = s.id;
    }
    const std::string next = mediapick::NextPick(ids, shown);
    if (!next.empty()) Pick(next);
}

double Position() {
    Player* p = Best();
    if (!p) return 0;
    const gint64 now = g_get_monotonic_time();
    if ((p->positionAt == 0 || now - p->positionAt > 2000000) && !p->asking) {
        p->asking = true;
        g_dbus_connection_call(g_bus, p->bus.c_str(), kPath, "org.freedesktop.DBus.Properties", "Get",
                               g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Position"), G_VARIANT_TYPE("(v)"),
                               G_DBUS_CALL_FLAGS_NONE, 1000, nullptr,
                               [](GObject* src, GAsyncResult* res, gpointer data) {
                                   std::string* bus = static_cast<std::string*>(data);
                                   GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
                                   auto it = g_players.find(*bus);
                                   if (it != g_players.end()) {
                                       Player& pl = *it->second;
                                       pl.asking = false;
                                       pl.positionAt = g_get_monotonic_time();
                                       if (r) {
                                           GVariant* v = nullptr;
                                           g_variant_get(r, "(v)", &v);
                                           if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) pl.position = g_variant_get_int64(v) / 1e6;
                                           if (v) g_variant_unref(v);
                                       }
                                   }
                                   if (r) g_variant_unref(r);
                                   delete bus;
                               },
                               new std::string(p->bus));
    }
    if (p->positionAt == 0) return p->position;
    return p->position + (IsPlaying(*p) ? (now - p->positionAt) / 1e6 : 0.0);
}

// Brings the player's window to the front (org.mpris.MediaPlayer2.Raise, on the
// player's root object; players that can't raise ignore it).
void Raise() {
    Player* p = Best();
    if (!p || !g_bus) return;
    g_dbus_connection_call(g_bus, p->bus.c_str(), "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2", "Raise",
                           nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
}

// Jumps to `seconds` with Seek (an offset from where the song is), which every
// player that can seek takes; SetPosition would also need the track's id.
void SeekTo(double seconds) {
    Player* p = Best();
    if (!p || !p->player) return;
    const double from = Position();
    const gint64 offset = static_cast<gint64>((std::max(0.0, seconds) - from) * 1e6);
    g_dbus_proxy_call(p->player, "Seek", g_variant_new("(x)", offset), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
    // Shown at once; the player's Seeked signal (or the next Position) corrects it.
    p->position = std::max(0.0, seconds);
    p->positionAt = g_get_monotonic_time();
}

void PlayPause() { Call("PlayPause"); }
void Next() { Call("Next"); }
void Previous() { Call("Previous"); }

}  // namespace bb::mpris
