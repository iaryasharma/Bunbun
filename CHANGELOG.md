# Changelog

All notable changes to Bunbun. The format follows [Keep a Changelog](https://keepachangelog.com/),
and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
- **Linux: right-clicking the island no longer quits Bunbun.** The AppImage bundled gtk-layer-shell
  0.7.0, which crashes when the island's right-click menu opens on newer GTK (seen on Fedora 42 with
  Hyprland). The Linux build now links gtk-layer-shell 0.10.0, and both the `.deb` and the AppImage
  carry it, so the system's copy (Ubuntu 22.04 has 0.7, which crashes) no longer matters.
- **Linux: the settings window closes right after it opens on some Wayland compositors** (Hyprland).
  WebKitGTK's DMABUF renderer failed with "Protocol error"; Bunbun now turns it off on Wayland. Set
  `WEBKIT_DISABLE_DMABUF_RENDERER` yourself to override.

## [0.3.10]

### Added
- **A gaming mascot in game mode.** With game mode on, the mascot sits on the left of the stats,
  controller in hand: buttons being mashed, eyes on the game, leaning into the turns. Every
  character has it (Dazai puts his book down, Bolt looks up from his visor), and skin packs can
  add `gaming.png` (else `working.png` stands in). It animates at 12 fps so the game keeps the
  GPU; turning the mascot off removes the slot. The Mascot studio previews it. Windows (game mode
  isn't on Linux yet; the mood is there for skin packs).
- **Ask Bun knows water and your work day.** `drank water`, `drank 500 ml`, `2 glasses`, `undo water`
  and `how much water` log and report water (the mascot drinks along); `start work`, `pause work`,
  `back to work`, `take a break`, `done for today` and `how long have I worked` (or studied) drive
  Work and Study mode. The local AI knows them too ("I just finished a big bottle" logs 750 ml) and
  can see today's water and work time. Windows.
- **Work mode, and a choice of what Bunbun is for.** Settings asks once: **AI agents** (as
  before), **Office work** (the agent pages, cards and limits are hidden) or **Both**. Settings →
  Work sets your working days and hours (night shifts too), lunch, breaks and a daily goal. The
  island then:
  - starts and stops with your hours, or when you press Start
  - pauses for lunch, reminds you to take a break and notes when the day is done
  - counts time away and asleep apart from work
  - shows a Work card on Home, your progress in the header and a Work page with today's timeline
    and Pause, Break and Done buttons

  **Generate work report** previews today, yesterday or the last 7 days and saves it as an
  image or a PDF. The log stays on your PC and is part of backups. Windows and Linux.
- **Study mode**, a fourth answer to what Bunbun is for. Enter your timetable in Settings → Work &
  study (days, times and subject for each class). The island:
  - follows it: "Maths · until 11:00", then free time with "Physics at 14:00"
  - notes when each class begins
  - counts a Pomodoro session as study, inside the timetable or not
  - keeps time per subject, with a daily study goal and a streak of days that met it
  - shows a Study card, "Study n%" in the header and a Study page with today's subjects as bars

  The study report adds hours per subject. Windows and Linux.
- **A clearer Settings app for your mode.**
  - The sidebar entry is named after your mode (Work day, Study, or Mode for AI agents), with its
    own icon and colour, next to Overview.
  - The page opens with a Today card: time so far, a goal ring, the streak, a mini timeline and the
    report button.
  - The mode cards say what each one adds.
  - Working hours show a preview of the day with lunch, and the timetable a week view.
  - Overview shows your mode, and drops the agents switch when your mode has no agents.
- **A richer work and study report.** A header with your name and dates, a donut of the day's
  split, focus, longest-stretch and break chips, a day-by-day chart against your goal (week), a
  timeline with hour marks, when in the day you worked, time per subject (study) and a short list
  of insights.
- **Accent colour: Dynamic, Solid or System**, in Appearance and in Settings → General. Dynamic
  takes it from the album art of what's playing; Solid is one fixed colour from a palette of 14
  or your own; System is the device accent. The settings app now wears exactly the island's
  colours: its theme presets and the accent the island shows right now. Accent following the album art is Windows only for now.
- **Every page follows the accent** except the agents', which keep each agent's colour: Weather,
  Calendar, System, Water, Timers and the Work timeline. The Home cards keep their own colours, with
  only Music following the album. Colours that mean something stay as they are: a low battery is
  red, and lunch, away and breaks keep their own colours on the timeline. Windows and Linux.
- **Set the Focus length on the ring**, as on Timer: drag the knob (Windows) or click the ring
  (Linux), or scroll over it. One turn is two hours, in 5-minute steps.
- The first-run "What will you use Bunbun for?" choice is a 2 × 2 grid of the mode cards, and
  Overview's Your mode is a proper card with the mode's icon and colour.
- **Away and asleep in every state.** The mascot shows you're away (or asleep) over music, a
  working agent or Work mode; only an agent waiting for your answer still shows.
- **Backup & restore** (Settings → General). Create backup saves your settings and Bunbun's data in
  one .zip: water, Screen Time, notes, to-dos, timers and your mascot packs (not the AI model).
  Restore loads one back, keeps a copy of what you had first and restarts the island. A backup
  made on Windows restores on Linux and the other way round.

### Fixed
  `bunbun-settings --backup <file>` and `--restore <file>` do the same from a script.
- **Backups left out your own Ask Bun prompt** (`ask/system-prompt.md`): the folder was looked
  for in the wrong place. It's included now. Windows.
- **Two activities squeezed side by side** (with coding agents off): Do Not Disturb beside a
  notification, the timer beside the mascot, in a widened notch. The island shows one at a time
  now, as it already did with agents on. Windows.
- **Ask Bun left focus nowhere after a quick command** (a timer): the box closed and your window
  didn't get the keyboard back. It does now. Windows.
- **Compact game mode drew the mascot over the RAM chip.** Windows.
- **Settings, Island pages left out the Work page**, so it couldn't be moved in the page order.
  It's listed now (as Study in Study mode), on with your mode.
- **The mascot's celebration never played for the battery-full cheer**, so Celebrate and Hop height
  seemed to do nothing there. It hops now. Windows.
- **The Work page cut its time off after an hour** ("1h 1…"); a longer time steps down a size.
  Windows and Linux.
- **On a light theme the album's accent could be too pale to read** when the cover was loaded
  under a dark theme. It's matched to the current theme every time. Windows.
- **Light themes (Porcelain, a light Custom or solid fill) in Studio:** the open island kept its
  white text and black cards, so most of it couldn't be read. Its neutral colours, cards and weather
  icons now follow the theme. Windows.
- **°C / °F set in Settings** showed °F beside Celsius numbers until the next weather refresh;
  the weather now reloads in the new unit at once. Windows.
- **Mascot Do Not Disturb did nothing during work hours**: Work mode's laptop pose covered it.
  DND you turn on (or Windows' DND, when followed) now wins. Windows and Linux.
- **Quick toggles and Hardware monitor switches had no effect in Studio**: the System page still
  showed the Network and Performance cards. An off card now goes, and its neighbour takes the row.
  Windows and Linux.
- **The island couldn't be opened during a download, a progress bar or a meeting alert**, nor
  while a timer ran with music beside it: those pills took over and hovering only grew the
  pill. It now opens on your pages (on Timers for a timer), and the wheel reaches the pages
  past them. Clicking a download pill still opens the file. Windows.
- **The island closed whenever a setting was saved.** A pinned island folded up when you
  pressed °C / °F, Auto-start or a Focus length on it, or changed anything in Settings. Windows.
- **Study mode said "Working · until 18:00"** while studying outside a class. It now shows the
  next class, or Free study. Windows and Linux.
- **Separate displays: full screen folds only its own display.** A full-screen app tucked (or
  hid) the island on every display. Now only the island on that display tucks or hides; if it
  was the one you were using, the island carries on from another display. Windows and Linux.
- **Settings: blocks no longer touch.** Every block on a page keeps the same gap from the one above
  (the Mascot studio's character grid sat against the box above it).
- **The °C / °F switch on the Weather page** went back to °C at once: it saved the choice under
  [Bunbun] while the setting is read from [Modules]. Windows.

## [0.3.9]

### Fixed
- **Buttons inside the open island stopped working** with islands on all displays in Separate mode
  (the Timers page's Focus / Timer / Stopwatch / Water, and other Studio buttons). Windows.
- **Opening the island while a timer runs** goes to the Timers page. Before, it only made the
  timer's pill taller, with no way to the pages while a timer or stopwatch ran.
- **Reset on a running timer** no longer jumps the open island to the next page.
- **Focus Guard** ("Still focusing · … can wait") only comes with focus sessions, not a plain
  countdown; Ask Bun's "timer 5 min" pill now says Timer instead of Focus Timer.
- **An agent's command approval** no longer shows the diff of the session's previous edit.
- **The music pill's mascot shows what your agent is doing** too (with the agent pill off).
- **All Apps in Classic** fits every page: smaller tiles and short names in narrow columns.
- **Cover art keeps its shape.** A video's wide thumbnail isn't squashed into a square any more:
  on Home's Music tile it widens to the left, on the Music page it's a wide card.
- **The cards on System, Weather and Screen Time** have the same rounded corners top and bottom
  (the page was clipped with the island's own, much bigger, corner radius; it also cut the first
  letter of the Agent page's bottom line). No bottom glow behind those cards any more.
- **All Apps lists every page** (it stopped at twelve; Mirror and Screen Time were missing).
- **Screen Time's figures** no longer run into each other ("2h 12m2h 12m"); a long one steps down a size.
- **The To-do page's hint** wraps instead of running off the right edge; Clipboard shows no
  "Today" over an empty list.
- **Bun drinks along on the music pill too** (it only did on the clock pill and the open island).
- **Reply to an agent no longer looks like Ask Bun.** The box says who you're messaging and shows
  the task you're answering; Ask Bun's ideas, chat history and New chat stay out of it. Windows.

## [0.3.8]

### Added
- **Timers page** (it replaces the Focus page). One page, four modes around a big ring:
  **Focus** runs Pomodoro sessions ("Focus 1 of 4") with short and long breaks, Skip, Reset, a count
  of today's sessions and, if you like, the next phase starting by itself; **Timer** is a countdown
  you set by dragging the ring or scrolling over it (hold Shift for seconds), with +1 / +5 min and
  your own presets (+ Save adds the time on the ring); **Stopwatch** keeps laps with the fastest in
  green and the slowest in red, and copies them; **Water** is the Water page. A running timer or
  stopwatch shows on the small island. Lengths and presets are in Settings → Productivity → Timers.
  With the Timers page off, Water stays a page of its own. Focus also has chips for the session
  length (15, 25, 50 or 90 min). Windows.
- **Screen Time page.** How long you've used the PC today, the last seven days as bars, and a list of
  apps with each app's real name and icon (Store apps included), the time and share of each. It
  counts the app in front while you're at the PC (not when it's locked or you've been away two
  minutes, unless the app is full screen). Set a daily limit in Settings → System & tools → Page
  options (or click the number on the page) to see what's left of it. Kept on your PC only, in
  `screentime.tsv` beside `settings.ini`. Windows.
- **System and Weather pages, redone.** System puts battery, network, CPU, memory and GPU, volume and
  Bluetooth on one page (in Studio it holds the Wi-Fi & Bluetooth toggles too). Weather shows now, the
  next hours as a curve, five days, humidity, wind and UV, with a °C / °F switch and Change City in
  its header. Windows.

- **Edge light.** A light runs along the island's outline for a few seconds, then fades: **Twin
  comets** (the default) set off from both top corners and meet at the bottom, **Comet** is a single
  light, **Breathe** makes the whole outline glow; each style has a live preview in Settings →
  Appearance → Edge light. Choose any mix of what plays it: hover, opening, closing, a track change,
  notifications, agents, system pop-ups, or always (a little more power). Its colour is **Default**
  (white), **Dynamic** (the album art's, or your accent) or **Custom**, from a palette or any hex.
  Windows.
- **Wi-Fi and Bluetooth switches on the System page.** In Studio, the Network card has a Wi-Fi and a
  Bluetooth chip: a click turns the radio on or off (a radio Windows can't use shows dimmed and opens
  its settings). Click the network name, or the connected Bluetooth device, to pick another in
  Windows settings. Windows.
- **Bun drinks along.** Logging a glass of water (Drank, + glass) shows the mascot sipping from a
  glass for a moment (Drank on a reminder puts the island back to its small pill for it, instead of
  opening it under the pointer), and the glass that reaches the day's goal gets a celebration. Every built-in
  character; a skin pack can add `drinking.png` (else its snack picture). Windows and Linux.
- **Your own text on the small island.** Settings → Mascot → Beside the face can show your text (a
  name, a word, an emoji) in the time's place, with or without the weather.
- **Clear on the Clipboard page.** Empties the history; pinned items stay.

### Changed
- **The face sits in the middle when a status ring shows.** The ring is on its right and the time
  (or weather, or your text) on its left. Auto's water ring now shows whenever you track water (Water
  in Timers), not only with water reminders on. Windows and Linux.
- **The mascot shows what your agents are doing** (working, thinking, waiting for you, done) even
  with the agent pill turned off. Windows and Linux.
- **Separate displays: every display has its own live island.** The island on each other display is
  drawn live at rest in that display's shape and size (clock, mascot, ring and all), instead of a
  picture of the main one. The island moves to another display only when you point at that display's
  island, never while it's open. The mascot's eyes follow the pointer on every display.
- **Islands on every display, working.** With the island on all displays, Settings → Position &
  motion → **How they work** chooses **Linked** (the default: point at the island on any screen and
  they all open together; clicks, scrolling and hover work on each one) or **Separate** (the island
  you point at opens on its own, at that display's position, while the others stay at rest). Each
  display keeps its own position, and the live preview can show any display's island. Before, the
  islands on other screens were pictures only. In Separate mode each display can also have its own
  **shape** (pill, notch, Windows 11) and **size** (80 % to 150 %): the island at rest there is drawn
  that way, and the island takes that look when it opens there. Windows and Linux.
- **Agent limits, per agent.** The Agent page shows the limits of the agent on it: Claude Code's
  5-hour and weekly windows (each with when it resets) and its context; Antigravity's quota pool by
  pool (the Gemini Flash models, its Claude and GPT-OSS models) with their resets; Cursor's plan,
  with a note that Cursor doesn't keep its usage on your PC. A limit or pool past its reset time now
  counts as refilled, so the header ring no longer shows an old number (Antigravity's cache only
  changes while it runs). Windows and Linux.
- **Linux matches Windows.** The Studio header (mascot and page name on the left, date, time,
  battery and close on the right), the round notch ears and the edge light with all its triggers
  and colours, the Timers page (Focus, Timer, Stopwatch, Water), the new System, Weather and
  Clipboard pages, and the **Screen Time** page (X11; Wayland doesn't say which app is in front)
  now work on Linux too. Water is a Timers mode there as well, and Island pages no longer
  photographs the Linux island by itself either.
- **Position & motion.** The settings app's Position & displays and Motion & behaviour pages are one
  page now: the screen and where the island sits, then its motion style, speed and when it shows up.
- **Island pages no longer photographs your island by itself.** It used to flip through the
  island's pages whenever you opened it or changed something, and the island saved a picture each
  time you looked at a page. Now it shows example pictures that come with the app (taken fresh for
  this release). **Take pictures of my island** captures yours when you ask, and you can switch
  between the examples and your own. Windows.
- **Water lives in Timers.** On Windows, Water is a mode of the Timers page and no longer a page of
  its own, even with Timers off; its switch is Productivity › Timers › Water in Timers. Island pages
  no longer lists Water, nor Wi-Fi & Bluetooth in Studio (those toggles are part of System).
- **A calmer settings app.** Each page in the sidebar has its icon on a small coloured tile,
  greys are neutral instead of purple-tinted, switches and selected items use your flat accent colour
  (no gradient), rows have more room, and long descriptions stop at two lines (hover a row for the
  whole text). Windows and Linux.
- **Notch ears.** The notch's curves into the top of the screen are larger and perfectly round
  (16 px), so it flows out of the screen edge. Windows.
- **Studio header.** Every page now has the mascot and the page's name at the top left; the date and
  time sit on the right beside the agent usage, battery and buttons (they make way when a page needs
  the room, as Weather does).

### Fixed
- **Separate displays:** opening the island on one display no longer turns the others into a plain
  black shape, and moving the pointer between displays no longer makes the islands jump and flicker.
- **Settings rows no longer run off the right edge** in a narrow window: wide controls (the per-display
  shape and size, for one) drop under their label.
- **The Clipboard page's pin** is a proper pin icon (filled when pinned), and the delete button a bin.
- Settings → Island pages showed each page's picture one page late while music was around.
- **The display map** in Position & motion fits your screens inside its box, centred, instead of
  running past the right edge; each display's number sits beside its name rather than under the
  position grid; and while the island follows the mouse, displays are no longer all marked "Mirror".
- **The Weather page made up values it didn't have** ("Clear Sky", "Feels like 25", sunrise at 6:39,
  wind 5, UV "2 Low", and invented hours and days); it now leaves out what the service didn't send.
  The System page's performance waves started from invented samples and GPU showed "--%" without a
  reading: the waves now start flat at the real value, and GPU appears only when it's reported.
  Windows.
- **The System page showed made-up values:** "USB Audio Device" when Windows gave no output device
  name, "Ethernet · 1 Gbps" whatever the link, and moving speed lines before any data. It now shows
  only what Windows reports. Windows.
- **The Clipboard page showed made-up items** (sample images and text) until you copied something; it
  now shows only what you copied, and "Nothing copied yet" before that. Windows.
- **Classic layout:** the Screen Time page's By app list no longer gets squeezed until icons overlap
  the times, and the Timers page's ring no longer sits under its mode bar, with all its buttons and
  length chips fitting. Windows.

## [0.3.7]

### Added
- **Status ring** (Settings → Appearance, beside "Beside the face"): a thin ring next to the clock
  on the small island that fills up as you go: the focus timer while one runs, today's water, or the
  to-dos ticked off. Auto (the default) shows focus, else water when its reminders are on. With
  "just the face", the ring sits beside the face. Windows and Linux.
- **Calendar month and week views.** The Calendar page names the month it shows, with ‹ › to move
  between months (click the title to come back to today), and dots under busy days: one per meeting
  from your calendar link and per to-do due. **Week** (on the Studio page) lists each day's meetings
  and to-dos with their times; a day in the month opens its week. Windows.
- **Notes in Settings.** A Notes page edits the island's quick note right in Bunbun (it used to open
  Notepad); it saves as you type and the island shows the change at once. The Note page's Edit opens
  it. Windows and Linux.
- **Settings, sorted.** The sidebar groups everything: the island (pages, appearance, position,
  motion and behaviour), its features with a page per area (Music & lyrics, Notifications & alerts,
  Productivity, System & tools, plus All features), Bun and the agents, and your own lists (To-dos,
  Notes). Each feature page shows every option with its switch, nothing folded away.
- **Live while sharing from Discord.** Besides the browser's sharing bar and OBS, an app that keeps
  the GPU's video encoder busy now counts as sharing (not while it has the camera on, which is a
  video call, and not NVIDIA or AMD replay recorders). On GPUs whose one video engine also plays
  video (some AMD graphics), it can't tell the two apart and doesn't guess. Windows.
- **Water.** A Water page: a glass filling up to your daily goal, a drop per glass, + glass and
  undo, when the next reminder is due, and the week against the goal. Water reminders (Settings →
  Features → Water, off by default) are paced across your active hours: one comes when you've fallen
  a glass behind, at least 20 minutes after the last, with **Drank** and **Later** (15 minutes) on
  the island; with a bottle size set, it asks you to refill when it's probably empty. Not while
  you're away from the keyboard. Goal, glass and bottle sizes and the hours are in Settings; the
  page can be switched off in Island pages. Kept in `water.txt` on your PC. Windows and Linux (on
  Linux the reminder is a toast; click it to log the glass).

### Changed
- **Tuck away is quicker and peeks.** Folding to the line and coming back take about 0.3 s (they
  took 0.65 to 0.9 s, most of it a slow crawl at the end). Pointing at the tucked line opens the
  island as a peek, and it tucks back 0.4 s after the pointer leaves, whether you tucked it by hand
  or full screen did; *Bring the island back* or Ctrl+Alt+H brings it back for good. Windows.
- **Notifications, Samsung-style.** The island grows in its own shape: the app's icon first, then
  the title, then a second line with the app and the message, and steps back the same way when the
  notification goes. The title and the message are on lines of their own (they were joined into one
  scrolling line), and Store apps' logos fill their circle. A notification also peeks out of the
  tucked island, except in full screen. Windows.
- **Follow Windows' Do Not Disturb** (Features → Notifications, first): on, the island stays quiet
  during Do Not Disturb; off, your apps' notifications still show on the island. It's the old
  *Notifications respect Do Not Disturb*, renamed so it says what it does. Windows.

### Fixed
- **A tucked island in full screen couldn't open.** In a full-screen app that tucks the island, it
  peeked out of the line but never opened on hover or click. Windows.
- Linux: the To-dos page in Settings was always empty (a placeholder answered before the real list).
- **Settings: the bottom of long pages was cut off.** The page area grew past the window instead of
  scrolling, so the last options sat under the taskbar out of reach (the sidebar too). Both scroll
  now. The window also opens sized for the screen under the pointer and stays above the taskbar when
  moved to a screen with a different scale. Windows and Linux (the page fix).
- **Reminder buttons.** Done, Snooze (and the water reminder's Drank and Later) could do nothing: the
  pill recorded its buttons in the wrong place, only took clicks while it was being redrawn, and a
  coding agent's card it had replaced could take the click instead (an old Reply button). Windows.
- **Lyrics staying in sync.** The line no longer flicks back and forth near its start when a player
  (Spotify, browsers) nudges its position back a little; pausing no longer drops back a line; after
  a seek on the island the line doesn't jump back to where the song was; the Lyrics tab no longer
  shows the previous song's lyrics against the new song while the new ones load; and lyrics stay in
  time in players that only report their position on a seek. Lyrics files' own `[offset:]` is
  applied, and `[mm:ss:xx]` times keep their fraction. Windows (the `[offset:]` fix on Linux too).

## [0.3.6]

### Changed
- **Approval cards show the whole edit.** The card grows with the change (up to 12 lines, in a
  monospace font, red for removed and green for added lines) and the mouse wheel scrolls the rest. A
  new file's content shows too. Windows and Linux.
- **Questions from your agent show in full.** The question wraps over as many lines as it needs, and
  every option has its own row. With one single-choice question, a click on an option answers it.
  Windows and Linux.
- **Linux: the hotkeys work on Wayland.** On GNOME (Ubuntu's default) Bunbun adds its hide and tuck
  shortcuts to GNOME's custom shortcuts; on other Wayland desktops, bind `bunbun --toggle` and
  `bunbun --tuck` (new) to keys.
- **Island size by screen.** New *Match screen size* (Appearance → Size & text): the island grows
  with bigger screens, so it takes the same share of each one (1080p 1x, 1440p about 1.3x, 4K 2x),
  re-sizing when it moves to another monitor, and never below your display scaling. *Size scale* is
  now a slider from 0.6x to 2.5x (it was seven steps); the cards, text and icons grow with it. Linux
  goes up to 2.5x too. Windows and Linux.
- **Sleep after (minutes)** (Mascot studio → Moods, next to AFK after): how long without keyboard
  or mouse before the mascot falls asleep, 10 minutes by default (it was a fixed 20); 0 keeps it
  awake. On Windows the nap after not touching the island (a fixed 8 minutes) follows it too.
  Windows and Linux.
- **Linux: seek in the song.** Click the progress bar on the Music page, or on the expanded music
  card (which now has one).

### Fixed
- Windows: in a full screen that tucks the island, bringing it back from the line and moving away
  left the island open; it now folds back to the line.
- Linux: the island could crash on a settings save, on Connect Claude Code or when hiding it from the
  tray (every second tray menu refresh).
- Linux AppImage: the island didn't start on Ubuntu 24.04 and other newer systems (a bundled
  library needed one they don't have), and the settings window opened from the island closed
  straight away (it got the island's bundled libraries).
- Linux: a right-click could pick the menu's first item, Settings, as the button came up; the menu
  now opens on release.

### Documentation
- Bunbun on Linux lists its requirements (64-bit, glibc 2.35, GTK 3.22, WebKitGTK 4.1 for the
  settings window) and the distributions it's tested on: Ubuntu 22.04 to 26.10, Linux Mint 22,
  Debian 12 and 13, Kali, Fedora 41 and 42, Arch, openSUSE Tumbleweed and Leap 15.6.

## [0.3.5]

### Fixed
- Linux: the notch hung below a top panel instead of touching the top of the screen. It now
  touches the edge on KDE, Sway, Hyprland and X11 desktops. On GNOME, whose top bar covers app
  windows, it sits directly under the bar, and moves to the edge when the bar is hidden.

## [0.3.4]

A Linux release: 148 of the 177 settings now work on Linux (up from 56), with the tray icon and the
colour picker, and the island is a notch by default everywhere.

### Added
- **In full screen** (Settings → Animations → When the island appears) now has three choices, on
  Windows and Linux: **Tuck** (the new default: a thin line you point at to bring the island back),
  **Show** (the island stays, hover and all) and **Hide**. It replaces *Hide on full screen* and *Tuck
  instead of hiding*; if you'd turned hiding off, you get Show. On Linux it needs X11.
- **New moods for every mascot.** **Do Not Disturb** (mascot DND or Focus): headphones on, eyes
  closed and a crescent-moon badge, instead of the snack. **Live**: while an app streams, shares or
  records your screen, the mascot is at its laptop with a pulsing REC badge (uses the screen privacy
  dot's detection; switch: *Live while the screen is shared*). **AFK**: after 3 minutes without
  keyboard or mouse (*AFK after*, 0 turns it off), the mascot munches its favourite snack, then naps
  after 20. The movie mood is now called **Streaming Shows**. Skin packs can add `dnd` and `live`
  frames (falling back to `sleepy` and `working`), and Mascot studio's importer lists them. All four
  work on Linux too; AFK reads the idle time from GNOME or KDE.
- **Full screen per app:** *Always show / tuck / hide over these apps* (Settings → Animations) override
  *In full screen* for the apps you name, say Show over the browser and Hide in games.
- **Tucked line delay:** *Point at the tucked line for* sets how long you point before the island
  comes back (450 ms by default), so a mouse passing over the top in a game doesn't open it.
- **Moods in one place:** a Moods section in Mascot studio with Do not disturb, *Follow the system's
  Do Not Disturb* (new: the mascot goes DND with Windows' or your desktop's DND), AFK, Live,
  Streaming Shows, battery moods, and *Always music* / *Always video* lists for when Bunbun guesses
  a player wrong.
- **Live without the dots:** the Live mood no longer needs the privacy dots switched on; the blue dot
  and its alert still follow their own switches.

### Changed
- **The Notch is the default shape**, on Windows and Linux: the island hangs from the top edge of the
  screen. Pick Pill or Windows 11 in Appearance → Shape. If you chose a shape before, it stays.
- **The blue privacy dot means your screen is shared or recorded**, on Windows and Linux alike, and
  the island says which: "OBS Studio is streaming your screen", "… is recording your screen", "… is
  sharing your screen". It now also sees Firefox sharing, Xbox Game Bar recording, Camtasia and
  ShareX on Windows; on Linux, PipeWire screen sharing (browsers, Discord, Zoom, GNOME's recorder),
  browsers sharing on X11, OBS, ffmpeg, wf-recorder, gpu-screen-recorder and SimpleScreenRecorder.

### Added
- **Linux: shape and colours.** The Linux island now follows Appearance like Windows does: the Notch
  (flush with the top of the screen, with the same curved ears), Pill and Windows 11 shapes, every
  theme preset including the light Porcelain, Custom colours (background, primary and secondary
  text, border), Fill (Transparent or Opaque), the border, the drop shadow, depth shading, accent
  bloom, tint, the font and the text size. Before, it always drew a dark pill.
- **Linux: where it sits.** Position (top or bottom, left, centre or right), the monitor (a number,
  the primary one, or the one your pointer is on), Offset X and Y, a separate offset while open,
  Border-merged mode, and Always on top.
- **Linux: hiding.** Auto-hide after a while, hide in full screen or tuck into a line instead, unhide
  on hover, and the hide (Ctrl+Alt+D) and tuck (Ctrl+Alt+H) hotkeys on X11; Tuck away in the
  right-click menu everywhere.
- **Linux: motion.** The motion styles (Smooth, Default, Bouncy, Snappy) and speeds, the frame rate,
  the battery saver, the ambient frame rate, the cover spin and the expanded player's transitions.
- **Linux: the music pill.** Song title on or off, the clock in the pill, auto-expand on a new song
  (never for videos) with its blocklist, swipe sideways to skip, the Music tile opening your music
  app, the progress line styles and the spectrum styles (bars, LED, plasma, orb).
- **Linux: alerts on the island.** Low battery and charging (with time to full), the mascot's battery
  moods, Caps Lock and Num Lock, brightness, volume (through `pactl`), Bluetooth devices connecting
  with their battery and running low, Do not disturb, a preview of what you copy, the countdown bar,
  and the focus timer in the right-click menu. Reminders play a sound, kept quiet in full screen or
  while focusing.
- **Linux: the face and the date.** The idle eyes face with its colours and agent colours, what sits
  beside the face, the mascot's do-not-disturb snack, a custom date format, and the first day of the
  week. Focus guard nudges you when a distracting app comes up during a focus timer (X11).
- **Linux: lyrics.** Karaoke lighting and the letter glow, readable neighbouring lines, and a
  separate offset for Bluetooth headphones.
- **Linux: the File shelf and Mirror pages.** Drop files on the island to park them on the shelf,
  click one to open it or drag it out again; they're kept across restarts. The Mirror page shows your
  camera (read through the kernel's video interface, no extra libraries), only after you turn it on.
- **Linux: calendar links.** Your calendars' private iCal links show today's meetings on the Calendar
  page, and the island shows the next one shortly before it starts, with Join for Meet, Zoom, Teams
  and Webex links. Windows and Linux now read calendars with the same code.
- **Linux: the island on every display**, as a view-only copy on each other monitor, placed like the
  main one or per display.
- **Linux: music bars from the real audio**, through `parec` (PulseAudio or PipeWire), and the small
  visualizer on the Lyrics page. Without `parec` the bars still animate.
- **Linux: Antigravity's quota** in the header ring and on the Agent page, read from Antigravity's
  own database with the system's SQLite (loaded only when needed).
- **Linux: the tray icon.** Your mascot in the panel with the same menu as on Windows: the version,
  hide or show the island, Settings, Pick a colour, Connect Claude Code, Start when you log in, Quit.
  Through AppIndicator (KDE, Ubuntu's GNOME, XFCE, Cinnamon, MATE, Budgie) or the classic X11 tray.
- **Linux: the colour picker**, from the eyedropper in the Studio header, Colour in All apps, the
  menus, or `bunbun --pick-colour` on a key of your choice. On X11 the screen freezes under a loupe as
  on Windows; on Wayland it uses the desktop's own picker. The hex is copied, and the island shows
  HEX, RGB and HSL to copy.

- **Linux: sounds.** The chimes for an agent that finishes, asks or hits an error, and the open /
  close blips, as on Windows (General → Sounds). They play through `paplay`, `pw-play` or `aplay`;
  the `.deb` recommends one.
- **Linux: privacy dots.** A green dot while an app uses the camera and an orange one while
  something records from the microphone, on the right of the small island.

### Fixed
- **Settings on Linux** marked some working settings **Not on Linux yet** and left others unmarked.
  The list is now checked against the Linux code by the tests, so it stays right. Settings that will
  stay Windows-only (Windows' blur, DPI scaling, the update check, the game overlay, downloads and
  progress alerts, notifications, poke reactions) are hidden on Linux instead.

## [0.3.3]

Everything that was briefly out as 0.3.1 and 0.3.2, plus a solid-colour island.

### Added
- **One-click updates.** Choose **Update** in Settings → General, the tray menu or the island's
  update notice: Bunbun and its settings close, a small window with Bun downloads the new version,
  checks it against the release's checksums, installs it silently and starts it. Your settings,
  skin packs and connected agents carry over. Copies that weren't installed with setup still open
  the release page.
- **Linux.** Bunbun now runs on Linux, packaged as a `.deb` (Debian, Ubuntu and derivatives) and an
  AppImage (most other distributions), both attached to every release. It runs on GNOME and KDE, on
  Wayland (layer-shell; GNOME through XWayland) and X11. This first Linux version has the island
  with the clock, music from any MPRIS player (cover art, play / pause, next, previous), the
  mascot with every character and mood, coding agents with Allow / Deny and question answers, the
  right-click menu, and the settings app (Codex connects from there too). Ask Bun, the tray icon and
  the system pop-ups aren't on Linux yet. See [Bunbun on Linux](docs/linux.md). To build it:
  `cmake -S . -B build-linux`, and `./scripts/package-linux.sh` for the packages;
  `tests/test_dialects.py` and `tests/simulate_agent.py` run against the Linux hook too.
- **Studio on Linux.** The island opens into the Studio frame on Linux too, with the same header,
  tab bar and page order, and the same pages: Home with its cards and your links, Music, Calendar,
  Weather, System, Quick toggles (Wi-Fi, Bluetooth, Do not disturb), To-do with reminders,
  Agent, Clipboard, Note, Focus and All apps. The plan and question cards, and Reply… / Change it… /
  Other… typed on the island, work as on Windows. File shelf and Mirror come later.
- **Lyrics on Linux**: the Lyrics page and lyrics on the small island, from LRCLIB and NetEase like
  on Windows, cached per song. The Linux build now needs libsoup 3 (`libsoup-3.0-dev` to build; the
  `.deb` depends on it and the AppImage carries it).
  See [Studio on Linux](docs/linux.md#studio-on-linux).
- **Settings on Linux** say what's there: Island pages shows real pictures of the Linux island's pages,
  and features, pages and options Linux doesn't have yet are marked **Not on Linux yet**. The To-dos
  screen works on Linux too. Music & media, Expand on hover and Pill while an agent works now work
  on Linux too.
- **Streaming.** While a video plays (YouTube, Netflix, an anime site, VLC…), the small island shows
  your mascot watching and munching popcorn with "Streaming", instead of the title and cover; the
  bars still move in the album colour. Music apps (Spotify, YouTube Music, Apple Music and others)
  keep the song and the headphones. Every character has the new Streaming mood, and skin packs can
  add `streaming` frames (falling back to `snack`). Turn it off with **Streaming mode for videos**
  in Features → Music. On Linux too, from the MPRIS player (browsers, VLC, mpv, Celluloid…).
- **Fill: Transparent or Opaque.** A new Fill choice in Appearance. Transparent is the look you had
  (the theme's colour, with the transparency and blur settings). Opaque paints the island one steady
  solid colour, with nothing showing through and no album-art tint: pick Black, Obsidian, Graphite,
  Navy, Forest, Plum, Espresso or White, or type any hex. Works on every shape, including the notch,
  and on Linux.

### Changed
- **Videos don't auto-expand.** With **Auto-expand on track change** on, a new song still opens the
  island for a moment, but a video (the Streaming pill) stays small.

### Fixed
- Sliding text on the island (song titles, the agent's current step) started at a random point, so
  it often appeared mid-word. Every line now starts at its first word and rests there a moment
  before it scrolls.
- A finished agent's pill showed its whole reply sliding under the Reply button. It now shows the
  first sentence, without markdown, ending in "…" when it doesn't fit.
- Updating while a coding agent was running could fail, because the agent kept `bunbun-hook.exe`
  in use. The installer now moves a running copy aside and Bunbun clears it up on the next start.
- A link on Home given as a bare file path (`C:\Notes\todo.txt`) was named after its drive (`C:`)
  instead of its file.

## [0.3.0]

### Added
- **Island pages** in Settings: a drawing of every page of the island, a switch for each, and drag
  (or the arrows) to put them in any order. Home always comes first. The same screen picks what each
  of the three Home cards shows and edits the links under them. **Show real pages** has the island
  open each page for a moment and take its picture, so the screen shows your actual island instead
  of sketches (saved in `%LOCALAPPDATA%\Bunbun\previews`). The pictures now update by themselves:
  when the screen opens, a few seconds after you change something there, and whenever you look at a
  page on the island. Pages the island hasn't shown yet use example pictures.
- **Pill while an agent works** (Settings → Assistant & agents): turn off the small "Runs · …" pill
  for a quieter island. Approvals, questions and plans still appear.
- **Tuck instead of hiding** in full screen (Settings → Animations → When the island appears): the
  island folds into a thin line while a video or game is full screen, and unfolds when you leave.
- **Lyrics on the small island** is now a setting (Features → Lyrics) as well as a right-click option.
  The pill now sizes itself to the song's longest line, a long line gets slightly smaller and then
  an ellipsis instead of sliding off the left edge, and each new line rolls up more smoothly.
- In the Studio layout, Home stays on the island while music plays (it comes first, then the
  player), so the three cards no longer disappear when a song starts.

### Changed
- **Studio is now the default layout.** Classic is still in Settings → Island pages and in the
  right-click menu.
- The Studio island follows your accent colour (the album art in Auto mode) for page titles, tabs and
  highlights. Each coding agent keeps its own colour (Claude orange, Antigravity blue, VS Code blue,
  Cursor grey, Codex green), and the Home cards keep theirs; only the Music card follows the album.
- Features is now a grid of switches grouped by what they do (Music, Lyrics, System pop-ups,
  Notifications, Reminders & meetings, Page options…); click one to see its options. Settings for
  whole pages moved to Island pages, and the sidebar is grouped into Island, Bun & agents, and App.

### Fixed
- The music bars sat as a row of dots: the refined bars read frequency bands that were only worked
  out for the Orb and Plasma styles. They move with the music again.
- Scrolling couldn't reach the last page (All apps) in the Studio layout while music played.
- In the Studio layout, clicks and hover could land on the wrong button (pausing music on Home
  started the focus timer; All apps lit up and opened the wrong page). The island only repaints the
  part that changed, and button positions were measured in that part instead of the whole window.
  A clicked tab was also pulled back by the scroll ring while the pointer stayed on the island.
- Hovering the gear and the eyedropper in the Studio header did nothing when other buttons had
  appeared or gone since the last frame.
- Home no longer has an Edit button; pick the cards in Settings → Island pages.
- Changes in the settings app could stop saving altogether: the Home card settings were missing
  from the list the app accepts, and one rejected setting made every later change fail too. All
  settings are accepted now, a bad one no longer blocks the rest, and a banner says when changes
  haven't reached the island.
- In the Studio layout, the open tab's name disappeared when there were many tabs; the other icons
  now shrink instead. Clicking the Music tile with nothing playing opened Windows Settings; it now
  opens your music app (Spotify if installed, else YouTube Music).
- Turning on the Mirror camera made the "is using your camera" alert cover the page, which turned
  the camera off again. Bunbun no longer alerts about its own camera use (the dot still lights).
- In the Studio layout, hovering and clicking the tab bar, the gear, the colour picker and other
  buttons could do nothing while an agent was active: their positions were measured against the
  wrong part of the island. They are now matched against where they are actually drawn.
- Buttons on the Studio island now fade their highlight in and out, and icons lift a little under
  the pointer.
- Clicking the island to open the playing app could start a second copy when its window was
  minimised or hidden in the tray, or when the player was a Store app or Firefox. Bunbun now finds
  the app's window by its app ID too, and shows a window hidden in the tray again.
- Scrolling the island past the last dashboard tab (To-do) did nothing for one notch, then jumped
  to Calendar. The wheel now wraps straight back to the first tab.
- Weather stopped showing whenever wttr.in was down, and each refresh waited minutes for it.
  Weather now comes from Open-Meteo, in a second or two; with no city set, the location comes from
  Windows' location service instead of your IP address. Ask Bun answers for any city you name.
- Skin-pack celebrations play as drawn, without a hop on top, and quick frames no longer blur
  into each other (the cross-fade takes at most a third of a frame).
- Synced lyrics with three-digit timestamps (`[01:02.345]`) ran up to a second late, and lines with
  several timestamps (repeated choruses) appeared only once. Both are read correctly now.
- The agent pill missed updates that changed only the step's details (a command's output, for one).
- Lyrics for YouTube videos titled "Artist - Song (Official Video)" are found much more often: the
  artist is taken from the title when the uploader is a channel ("ArtistVEVO", "Artist - Topic"),
  and only whole tags like (Official Video), [HD] or (Lyrics) are dropped, so "(Mixtape)" stays.
- A song without lyrics no longer asks the lyrics sites again on every replay; the answer is
  remembered for three days. Network failures are still retried, never remembered.

### Added
- **NetEase lyrics fallback.** When LRCLIB has no synced lyrics, Bunbun also looks on NetEase Cloud
  Music (strong on Chinese, Japanese and Korean songs) and only accepts the same title, artist and
  length. It sends the song title and artist to music.163.com; turn it off with **Also search
  NetEase**. Credit lines ("作词 : …") at the top are dropped.
- Instrumental tracks say **Instrumental** instead of "No lyrics found".
- During a long instrumental break the last sung line fades to ♪ after a few seconds, in the
  collapsed pill, the Lyrics tab and the music-page lyrics panel; the same after the final line.
- Lyrics are fetched as soon as a new song starts, so opening the Lyrics tab shows them at once.
- **Studio layout** (Settings → Bunbun → Island layout). The open island gets a header (page name,
  date and time, a settings gear) and a tab bar with one icon per page and a volume slider. The
  first page has big Agent, Music and Focus tiles, and the music page glows in the album's colour.
  Classic stays the default and is unchanged.
- **Ask Bun as a chat** in the Studio layout: your questions and Bun's answers stay on screen as a
  short conversation, with suggestion chips, a New chat button, and your mascot in the input box.
- **Agent page**: a live timer, Read / Edit / Test / Done progress, the last command and its
  output, tasks done today, recent tasks, and your Claude Code usage (5-hour and weekly limits,
  context; turn them off in Settings → Assistant & agents → Show Claude Code usage limits).
  Connecting Claude Code now also adds a statusLine that sends those limits to Bunbun; an
  existing statusLine of your own is left alone.
- **Answer agents from the island**: Claude Code's plans show as "Claude has a plan" with the plan
  text, **Approve** and **Change it…** (tell Claude what to change). Questions with several parts,
  multi-select or your own words step through a card. When a task finishes, **Reply** sends Claude
  your next message (connect Claude Code again to add the reply hook). The Agent page has a model
  chip (Default / Opus / Sonnet / Haiku).
- **Codex** can be connected in one click (progress on the island; Codex asks for permission itself).
- **Focus page**: an hours and minutes picker (scroll or click the numbers), 5 / 15 / 25 / 50 minute
  chips, "Done at", +5 min, pause and stop. Optional **focus guard** nudges you when a distracting
  app or site comes to the front.
- **Home**: pick what each tile shows (agent, music, focus, weather, calendar, battery, system) in
  Settings or with **Edit** on Home, and add up to four **links** under the tiles. An **All apps**
  page lists every page plus the colour picker and Settings, with badges.
- **Mirror**: an optional camera preview page; the camera only runs while you have it on.
- **Sounds**: soft chimes when an agent finishes, needs you or stops with an error, with a volume
  setting, and optional open and close blips.
- **Bluetooth battery warning** for connected devices at 20% or below, once a day.
- Dropping files on the open island shows a "Let go, I'll catch it!" zone, then a card as the file
  lands in your tray.
- **Choose which agents the island shows**: an On island switch beside each agent's Connect button
  (plus Codex and other tools), and a Main agent for the Home tile and the usage ring. A switched-off
  agent is ignored completely and asks in its own window. With no agent on, there is no agent pill,
  tab or ring, and the Home tile shows the weather instead.
- **Antigravity quota**: how much of Antigravity's model quota you've used and when it resets, read
  from the status Antigravity keeps on your PC (no sign-in, nothing sent anywhere). It shows on the
  Agent page and stands in for Claude's ring when Claude Code has no numbers. Toggle it in Settings →
  Assistant & agents → Show Antigravity quota.
- **Clipboard page** with your last 8 copied texts (memory only), click to copy again, and a
  **Note page** for one quick note with checkboxes (`note.txt` next to `settings.ini`).
- **Tuck away**: Ctrl+Alt+H, or Tuck away in the right-click menu, folds the island to a thin
  status-coloured line. Point at it to bring it back; it returns by itself when an agent needs you.
- **Idle face**: show two glowing eyes instead of the mascot. They follow the pointer, blink (and
  now and then wink), pulse orange while an agent works, turn green when it's done and red on an
  error, and change shape with the mood. Pick their colour in **Eye colour**, and choose what sits
  beside the face in **Beside the face**: time and weather, either one, or nothing at all.
  **Eyes show agent status** (off by default) makes them pulse orange while an agent works.
  All of these are on the settings app's Mascot page (Idle face), and the layout on Appearance.
- The calendar page uses the Studio layout's extra width for an **Agenda**: your next meeting and
  today's open to-dos with their times.
- The music bars are fewer and fatter, follow the frequency bands instead of raw samples, rise
  fast and fall slowly, and glow softly in the album's colour.
- **Music app** setting (Spotify, YouTube Music, Apple Music) for what the Music tile opens.
- Swipe sideways on the music pill, or scroll sideways on a touchpad, for the next or previous
  song. Poke the mascot three times and it gets cross, six times and it gets dizzy; it says hello
  when Bunbun starts.
- **A cleaner right-click menu**: dark like the island, an icon on each item, quick actions first,
  and Lyrics and Appearance (layout, idle face, shape, transparency, theme) in their own submenus.
- **Colour picker**: the eyedropper in the Studio header, or Pick a colour on screen in the
  right-click menu, freezes the screen under a magnifier. Click a pixel to copy its hex code, then
  copy it as RGB or HSL from the card that appears. Arrow keys nudge one pixel (Shift: ten), Esc
  closes.
- New versions are announced on the island too; click the notice to open the download page.
- The notch now flows out of the screen edge with small curved corners, and its bottom corners
  open up as the island grows.
- **A real celebration** when a task finishes: every mascot gets yellow sparks and a confetti burst
  that flutters down around the head, on top of the hop.

## [0.2.0]

### Fixed
- Asking Bun for the weather in another city answered with your own city's weather.
- The island no longer redraws at the monitor's full refresh rate while nothing moves. A spring
  that never quite settled kept agent pills at ~128 frames a second; idle CPU under an agent pill
  drops from ~30% of a core to ~2%, GPU from ~3% to ~0.3%.
- Scrolling no longer hits a dead stop past the last tile, and the music player is always reachable
  in the scroll ring, even while paused.
- With the game overlay on, the mouse wheel opens the dashboard so it can be scrolled.
- Mascots on agent pills are as big as the one beside the clock, and skin-pack images are trimmed
  to their visible area so every mood fills the same space.
- Skin packs no longer look blurry or jagged at island size: their art is scaled down with
  high-quality filtering. Packs made from a character sheet show up in the Mascot studio's list.
- Skin-pack frames are no longer tilted or bobbed on top of their own drawn motion, which made
  them wobble.
- A skin pack's art that reaches past the head (question marks, sparks, a raised hand) is no
  longer cut off by the island's edge or covered by text: the agent pill, approval card, Ask Bun
  and the slot beside the clock leave room for it.

### Added
- **An installer.** `Bunbun-<version>-setup.exe` installs for your account (no admin prompt), adds
  Start menu shortcuts and an uninstaller, can start Bunbun at sign-in, installs the WebView2
  Runtime if it's missing, and can download a Local AI model and turn Local AI on. Uninstalling
  disconnects the coding agents connected to that copy first. Releases now offer just this one
  file (with the local AI helper) instead of a zip.
- `bunbun.exe --enable-local-ai` turns Local AI on in `settings.ini`; `--disconnect <agent> --if-ours`
  only disconnects an agent whose hooks call that copy of Bunbun.
- **Your own character from a character sheet.** Mascot studio → Custom skins → Pick a character
  sheet: Bunbun cuts out every frame, you say which row is which mood, and the frames play as
  animations. The resting idle frame's eyes follow your cursor, and a row of look directions
  (left, up, centre, down, right) makes the character look toward it. Timing per mood in
  `pack.ini`.
- **Animated skin packs**: `idle_1.png`, `idle_2.png`… play in turn, cross-fading between frames
  and between moods.
- **Live eyes for skin packs (face rig).** A pack can include its face without eyes and brows
  (`pack.ini` `[Rig]`); Bunbun draws them live and sharp: smooth gaze in every direction toward
  the cursor, idle glances, lids that follow the gaze, natural and double blinks, brows that lift,
  and closed "^ ^" eyes when happy.
- **Local AI for Ask Bun (optional, offline).** A small language model on your own PC answers
  what Bun's commands don't cover, and acts like an agent: several steps for one request ("get me
  ready for a meeting"), and lookups it reads before answering ("is it warmer in Delhi or
  Mumbai?"). Runs in a separate helper (`bunbun-brain.exe`, llama.cpp) that starts on demand and
  frees its memory when idle. No account or keys; you download a model yourself. See the Local AI
  guide. Settings → Assistant & agents → Local AI shows its status.
- **Weather anywhere**: `weather in Mumbai`, `mumbai weather`, `will it rain in Pune tomorrow`.
- **Three new moods for every mascot**: Listening while you type to Bun, Talking while the local AI
  answers, Confused (dizzy: spiral eyes and a ring of stars) when Bun didn't understand. Skin packs can add `listening.png`, `talking.png`
  and `confused.png`.
- **The mascot lives in the Ask Bun box**, in place of the magnifier, and reacts to what's happening.
- **Venom Spidey**: Spidey's colour swatches give him a dark Venom suit with grey webbing.
- **Repeating reminders**: `remind me every day at 9 to stand up`, `every weekday`, `every Monday
  at 10`, `every Mon and Wed`, `every morning`. Done moves one on to its next time; Snooze rings
  again in 10 minutes without changing the schedule.
- **To-dos page** in Settings: add, edit, tick off, reschedule, set repeats, delete, clear
  finished. Changes reach the island at once.
- **Quiet reminders**: during a full-screen app, a presentation, a calendar meeting or Focus mode,
  a reminder shows as a small silent pill (tap it for Snooze and Done).
- **Focus mode**: Ask Bun `focus` (25 min) or `focus 50`. Runs the timer, puts the mascot on
  do-not-disturb, keeps notification pop-ups off the island (they stay in Windows' notification
  centre) and ends with a summary: minutes, to-dos ticked off, notifications that waited.
- **Ask Bun extras**: `undo` (or Ctrl+Z in the empty box) reverts the last command (volume, mute,
  dnd, timer, focus, to-dos, Wi-Fi, Bluetooth, brightness); Up / Down step through earlier
  commands; `wifi on/off`, `bluetooth on/off`, `brightness 60`, `brighter`, `dim the screen`; and
  apps open by fuzzy name from a remembered Start-menu list (`vsc` → Visual Studio Code).
- **To-dos and reminders.** Ask Bun `add buy milk`, `remind me at 3pm to call Sam`,
  `remind me tomorrow at 9 to send the report`, `my list`, `done buy milk`, `clear done`. A To-do
  tab lists them (click to tick off); a reminder pill rings when one is due with **Snooze** (10 min)
  and **Done**, and plays Windows' Calendar Reminder sound (off with "Reminder sound"). Kept on
  your PC in `todos.tsv` next to `settings.ini`.
- **Privacy dots that catch every app.** The microphone dot follows live recording sessions and
  the camera dot follows Windows' live camera monitor, so Discord, OBS and games show up (Windows'
  own usage records had stopped updating for desktop apps). Screen recording and streaming come from
  OBS's log (recording, live, replay buffer) and the "… is sharing your screen" bar of Chromium
  browsers. One dot per device in use, a short pill names the app when it starts, and pills make
  room for the dots instead of being covered by them. Apps like Discord keep the microphone open
  while you're muted, so the mic dot stays on, as Windows' own icon does.
- **Connectivity cards you can use.** Click Wi-Fi or Bluetooth to turn it on or off; hold it to
  see nearby Wi-Fi networks or paired Bluetooth devices and connect or disconnect right there
  (saved networks and Bluetooth audio devices; others open Windows' own picker). Right-click any
  card for its Windows settings. Do Not Disturb still opens Settings: Windows has no public switch.
- **Battery details and moods** on laptops: time until full while charging, a note when the
  battery is held at a limit, battery health on hover, a charging update every 15 minutes, and the
  mascot holds a plug while charging, gets drowsy below 15% and cheers when it's full.
- **Next meeting** from your calendar's private iCal link (Google Calendar, Outlook.com): a
  countdown pill before it starts with a **Join** button for Meet, Zoom, Teams and Webex, and an
  "Up next" panel on the calendar tab (in place of the date block while a meeting is coming up).
  With the Battery card, the Connectivity tab switches to a 2 × 2 grid so every card stays readable. Repeating meetings and time zones are handled.
- **Download progress**: browser downloads in your Downloads folder show as a live pill with name,
  size and speed; click the finished one to show the file.
- **Screen-recording dot**: blue while an app captures the screen through Windows' capture API.
- **Battery on the Connectivity tab** (laptops and tablets only): charging, fully charged on power,
  plugged in but not charging (battery held at a limit), or the percentage with time left and
  battery saver while unplugged. Click it for Windows' Power & battery settings.
- **Ask Bun gets out of the way.** A command that ran (open an app, dnd, volume, media, timer,
  lock, snip, web search) flashes its confirmation for a moment, then the box closes, focus goes
  back to your app and the island returns home. Answers to questions still stay up to read.
  The box empties after each query, ready for the next one. Bun also understands looser phrasing: `turn on dnd`, `disable do not disturb`,
  `can you open up notepad?`, `launch the spotify app`.
- **Mascots at work.** While an agent works, every mascot types at a little laptop with its eyes
  reading across the screen; Bolt runs his terminal on his visor instead. Bolt's do-not-disturb
  snack is now a charging battery.
- **Update check.** Once a day (and on demand from the tray or Settings → General → Updates) Bunbun
  asks GitHub for the latest release and shows a notification when a newer one is out. Read-only:
  it opens the release page and never downloads anything. Off with "Check automatically".
- **Battery saver** (Animations): fewer wakeups, lower ambient frame rates and no 1 ms timer for
  ambient content. Auto turns it on while running on battery or Windows battery saver.
- The hardware monitor says **Upload** and **Download** instead of Net up / Net down.
- `BUNBUN_LOG=1` also logs frames per second, why they were drawn and how long each took.

## [0.1.0]

First public version.

### Island
- One island for media, agents, timers, alerts and an idle dashboard; pill, notch or Windows 11
  shapes; top or bottom; one display or all, with a position per display.
- Media with album art, controls, synced lyrics, and the song title on the collapsed pill.
- Dashboard tabs: calendar, weather, hardware monitor (CPU, RAM, GPU, disk, network speed),
  connectivity (Wi-Fi or Ethernet, Bluetooth, Do Not Disturb), file shelf.
- Privacy dots when the microphone or camera is in use.

### Coding agents
- Claude Code, Antigravity, Cursor and GitHub Copilot: live session ticker and one-click Allow / Deny,
  with one-click connect and disconnect (backups made).

### Mascots
- Six original characters (Bun, Miso, Biscuit, Ember, Bao, Bolt) with moods for idle, working,
  thinking, needs you, celebrate, error, sleepy, music and do not disturb; per-character colours;
  custom skin packs.

### Ask Bun
- A local Windows assistant on Ctrl+Alt+B: open apps and settings, media and volume, timers,
  searches, quick answers, mascot do-not-disturb.

### Releases
- A portable zip (`Bunbun-<version>-win-x64.zip`) with SHA256 checksums; `scripts/setup.sh`
  fetches the pinned toolchains; CI builds every push and tags publish releases.

### Settings app
- WebView2 settings window with a live preview of the real island on your wallpaper, themes,
  animations with real spring previews, and search across every setting.
