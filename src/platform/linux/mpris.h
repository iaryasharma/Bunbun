// Bunbun — now playing on Linux: MPRIS players on the session D-Bus
// (org.mpris.MediaPlayer2.*). The Linux counterpart of Windows' media session
// (GSMTC). Runs on the GTK main loop; no threads.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace bb::mpris {

struct Track {
    bool active = false;   // a player has something loaded
    bool playing = false;
    std::string title, artist, album;
    std::string artUrl;    // mpris:artUrl (file:// or http(s)://), may be empty
    std::string player;    // the app's name ("Spotify", "Firefox")
    std::string playerId;  // its bus name, what Pick() takes
    double length = 0;     // seconds, 0 when unknown
};

// A running player, for choosing between several.
struct Source {
    std::string id;     // bus name
    std::string name;   // "Spotify", "Chrome", "Chrome (2)" when the same app runs twice
    bool playing = false;
    bool shown = false;  // the one on the island now
};

// Calls onChange (on the main loop) whenever the current track or state changes.
void Start(std::function<void()> onChange);
void Stop();

// The player worth showing: the one picked by hand, else the one playing most
// recently, else a paused one (rules in common/media_pick.h).
Track Current();

// Every player that is playing or paused, in a steady order; `shown` marks the one
// on the island.
std::vector<Source> Sources();
// Shows this player until it stops (then the island chooses again). "" = automatic.
void Pick(const std::string& id);
// The player picked by hand, "" when the island is choosing.
std::string Picked();
// The next player in the list, for a click on the source label.
void CycleSource();

// Where that player is in the track, in seconds (for synced lyrics). Players
// don't announce their position, so it's asked for every couple of seconds and
// on a seek, and runs on with the clock in between.
double Position();

void PlayPause();
void SeekTo(double seconds);  // the progress bar's click
void Raise();  // the player's window to the front
void Next();
void Previous();

}  // namespace bb::mpris
