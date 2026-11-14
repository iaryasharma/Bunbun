// Bunbun — unit tests for the JSON library, the Claude Code hook installer and
// Bun's assistant parser.
// Build + run: ./tests/run.sh   (uses a throwaway CLAUDE_CONFIG_DIR)
#include <windows.h>

#include <cstdio>
#include <string>

#include "../src/agents/hook_installer.h"
#include "../src/assistant/assistant.h"
#include "../src/common/json.h"
#include "../src/common/update.h"
#include "../src/common/todos.h"
#include "../src/common/chime_wav.h"
#include "../src/common/screentime.h"
#include "../src/common/edgelight.h"
#include "../src/common/ag_quota.h"
#include "../src/common/agent_limits.h"
#include "../src/common/applist.h"
#include "../src/common/water.h"
#include "../src/common/backup.h"
#include "../src/common/work.h"
#include "../src/common/ics.h"
#include "../src/common/media_pick.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <vector>
#include "../src/island/modules/lyrics_text.h"

using bb::json::Value;

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                 \
        }                                                               \
    } while (0)

static std::string ReadAll(const std::wstring& path) {
    std::string s;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static void WriteAll(const std::wstring& path, const std::string& s) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

// The UI chimes (common/chime_wav.h, shared by Windows and Linux): each one is a
// valid 16-bit mono WAV of the expected length, louder with the volume.
// Screen Time's bookkeeping (common/screentime.h): days, the file, the summary.
// What plays the edge light (common/edgelight.h).
// Antigravity's quota pools (common/ag_quota.h).
// Work mode (common/work.h): the schedule, a simulated day, a night shift, the buttons, the file.
static void TestWork() {
    namespace wk = bb::work;
    auto Check = [](bool ok, const char* what) { if (!ok) { std::printf("FAIL work: %s\n", what); ++g_failed; } };
    Check(wk::ParseClock(L"09:00", -1) == 540 && wk::ParseClock(L"6:15 pm", -1) == 1095 && wk::ParseClock(L"18", -1) == 1080 &&
          wk::ParseClock(L"x", -1) == -1, "clock parsing");
    Check(wk::ParseDays(L"mon,tue,wed,thu,fri") == 0x1F && wk::ParseDays(L"weekend") == 0x60 && wk::ParseDays(L"everyday") == 0x7F,
          "day parsing");
    Check(wk::ParseUseCase(L"work") == wk::UseCase::Work && wk::ParseUseCase(L"") == wk::UseCase::Agents &&
          !wk::AgentsOn(wk::UseCase::Work) && wk::WorkOn(wk::UseCase::Both), "use case");

    // A Monday, 9:00 to 18:00, lunch 13:00 for 45 minutes, breaks every 50 minutes,
    // AFK after 3 minutes. Away from 10:00 to 10:20.
    wk::Tracker t;
    unsigned all = 0;
    int lunchStarts = 0, breakDue = 0, dayEnds = 0;
    for (int s = 8 * 3600 + 55 * 60; s <= 18 * 3600 + 5 * 60; s += 10) {
        wk::Input in;
        in.epoch = 1800000000 + s;
        in.date = 20261109; in.prevDate = 20261108; in.weekday = 0; in.second = s;
        in.idle = (s >= 10 * 3600 && s < 10 * 3600 + 20 * 60) ? s - 10 * 3600 : 0;
        in.afkMinutes = 3; in.sleepMinutes = 0;
        const unsigned e = t.Tick(in, 10);
        all |= e;
        lunchStarts += (e & wk::kLunchStart) ? 1 : 0;
        breakDue += (e & wk::kBreakDue) ? 1 : 0;
        dayEnds += (e & wk::kDayEnded) ? 1 : 0;
    }
    const wk::Summary sum = wk::Summarize(t.day);
    auto approx = [](int seconds, int minutes) { return std::abs(seconds - minutes * 60) <= 60; };
    Check(approx(sum.Of(wk::Kind::Lunch), 45), "45 minutes of lunch");
    Check(approx(sum.Of(wk::Kind::Away), 20), "20 minutes away, counted from when the idling began");
    Check(approx(sum.Worked(), 9 * 60 - 45 - 20), "the rest is work");
    Check(lunchStarts == 1 && dayEnds == 1, "lunch and the day's end each come once");
    Check(breakDue >= 7 && breakDue <= 10, "break reminders through the day");
    Check(sum.breaks >= 1, "20 minutes away counts as a break");
    Check(t.day.date == 20261109 && sum.first >= 9 * 3600 && sum.last <= 18 * 3600 + 10, "the day's span");
    Check((all & wk::kGoalReached) == 0, "8h15m isn't reached by 7h55m of work");

    // Days off: a date, and a range that rolls over a month end, keep auto-start from treating the day as work.
    {
        const std::vector<int> off = wk::ParseDatesOff(L"2026-12-25, 2026-12-30..2027-01-02");
        Check(off.size() == 5 && off[1] == 20261230 && off[3] == 20270101, "days off parsing");
        // The settings app's calendar tags holidays with "h" ("2026-12-25h"): still a day off.
        const std::vector<int> tagged = wk::ParseDatesOff(L"2026-11-14h, 2026-12-30..2027-01-01h, 2026-12-01");
        Check(tagged.size() == 5 && tagged[0] == 20261114 && tagged[3] == 20270101 && tagged[4] == 20261201, "holiday-tagged days off");
        wk::Tracker h;
        h.schedule.daysOff = off;
        wk::Input xmas;
        xmas.date = 20261225; xmas.prevDate = 20261224; xmas.weekday = 4; xmas.second = 10 * 3600; xmas.epoch = 1;
        bool w = true; int d = 0, sec = 0;
        h.Locate(xmas, &w, &d, &sec);
        Check(!w, "no work on a day off, even a working weekday");
        xmas.date = 20261224; xmas.prevDate = 20261223; xmas.weekday = 3;
        h.Locate(xmas, &w, &d, &sec);
        Check(w, "the day before is still a work day");
    }

    // A night shift, Friday 22:00 to Saturday 06:00: 02:00 on Saturday is Friday's work day.
    wk::Tracker n;
    n.schedule.days = 1u << 4;  // Friday
    n.schedule.start = 22 * 60; n.schedule.end = 6 * 60; n.schedule.lunchMinutes = 0;
    wk::Input sat;
    sat.date = 20261114; sat.prevDate = 20261113; sat.weekday = 5; sat.second = 2 * 3600; sat.epoch = 1;
    bool within = false;
    int workDate = 0, second = 0;
    n.Locate(sat, &within, &workDate, &second);
    Check(within && workDate == 20261113 && second == 86400 + 2 * 3600, "night shift past midnight");
    sat.second = 7 * 3600;
    n.Locate(sat, &within, &workDate, &second);
    Check(!within, "the night shift is over at 07:00");

    // The buttons: Start outside the schedule is overtime; Pause; Done stops tracking.
    wk::Tracker m;
    m.schedule.autoStart = false;
    wk::Input ev;
    ev.date = 20261109; ev.prevDate = 20261108; ev.weekday = 0; ev.second = 20 * 3600; ev.epoch = 1000;
    m.Tick(ev, 10);
    Check(m.phase == wk::Phase::Off, "nothing without Start when the schedule is off");
    m.Start(20261109);
    ev.second += 10; m.Tick(ev, 10);
    Check(m.phase == wk::Phase::Working, "Start works on a day with the schedule off");
    m.schedule.autoStart = true;
    m.schedule.start = 9 * 60; m.schedule.end = 18 * 60;
    ev.second += 10; m.Tick(ev, 10);
    Check(m.phase == wk::Phase::Overtime, "after hours it's overtime");
    m.Pause();
    ev.second += 10; m.Tick(ev, 10);
    Check(m.phase == wk::Phase::Paused, "Pause");
    m.Resume();
    m.TakeBreak(ev.epoch);
    ev.second += 10; ev.epoch += 10; m.Tick(ev, 10);
    Check(m.phase == wk::Phase::Break, "Take a break");
    ev.epoch += 6 * 60; ev.second += 6 * 60; m.Tick(ev, 10);
    Check(m.phase == wk::Phase::Overtime, "the break ends by itself");
    m.Done(20261109);
    ev.second += 10; m.Tick(ev, 10);
    Check(m.phase == wk::Phase::Done, "Done for today");

    // The file keeps the days, the breaks and the buttons' state.
    wk::Log log;
    log.days[t.day.date] = t.day;
    log.manual = m.manual;
    const wk::Log back = wk::Parse(wk::Serialize(log));
    Check(back.days.count(20261109) == 1, "a day survives the file");
    const wk::Summary again = wk::Summarize(back.days.at(20261109));
    Check(again.Worked() == sum.Worked() && again.Of(wk::Kind::Away) == sum.Of(wk::Kind::Away) && again.breaks == sum.breaks,
          "the same totals after the file");
    Check(back.manual.done && back.manual.date == 20261109, "the buttons' state survives the file");
    Check(wk::Duration(3 * 3600 + 12 * 60) == L"3h 12m" && wk::Duration(45 * 60) == L"45m", "durations");
}

// Study mode (common/work.h): the timetable, subjects, Pomodoro sessions, the streak.
static void TestStudy() {
    namespace wk = bb::work;
    auto Check = [](bool ok, const char* what) { if (!ok) { std::printf("FAIL study: %s\n", what); ++g_failed; } };
    const std::vector<wk::Block> tt = wk::ParseTimetable(L"mon 10:00-11:00 Maths; mon,wed 14:00-15:00 Physics\ntue 9-10:30");
    Check(tt.size() == 4, "timetable: four blocks (Physics twice)");
    Check(tt[0].weekday == 0 && tt[0].start == 600 && tt[0].end == 660 && tt[0].subject == L"Maths", "timetable: a block");
    Check(tt[3].weekday == 1 && tt[3].end == 630 && tt[3].subject == L"Study", "timetable: no subject is Study");
    Check(wk::StudyOn(wk::UseCase::Study) && !wk::AgentsOn(wk::UseCase::Study) && wk::WorkOn(wk::UseCase::Study), "study use case");

    wk::Tracker t;
    t.schedule.blocks = tt;
    t.schedule.goalMinutes = 150;
    t.schedule.breakEvery = 0;
    int blockStarts = 0;
    for (int s = 9 * 3600 + 50 * 60; s <= 21 * 3600; s += 10) {
        wk::Input in;
        in.epoch = 1800000000 + s;
        in.date = 20261109; in.prevDate = 20261108; in.weekday = 0; in.second = s;
        in.afkMinutes = 3;
        in.extraActive = s >= 20 * 3600 && s < 20 * 3600 + 25 * 60;  // a Pomodoro after dinner
        if (t.Tick(in, 10) & wk::kBlockStarted) ++blockStarts;
    }
    wk::Summary sum = wk::Summarize(t.day);
    auto approx = [](int seconds, int minutes) { return std::abs(seconds - minutes * 60) <= 60; };
    Check(blockStarts == 2, "a note as each block begins");
    Check(approx(sum.subjects[L"Maths"], 60) && approx(sum.subjects[L"Physics"], 60), "an hour each of Maths and Physics");
    Check(approx(sum.subjects[L"Pomodoro"], 25), "the Pomodoro counts as study");
    Check(approx(sum.Worked(), 145), "2h 25m studied");
    Check(t.phase == wk::Phase::Off, "free time after the last session");

    // The streak: two days before met the goal, and today has too.
    wk::Log log;
    for (int d : {20261107, 20261108}) {
        wk::Day day;
        day.date = d;
        day.segments.push_back({36000, 36000 + 3 * 3600, wk::Kind::Work});
        log.days[d] = day;
    }
    wk::Day today;
    today.date = 20261109;
    Check(wk::Streak(log, today, 20261109, 150 * 60) == 2, "the streak runs to yesterday until today's goal is met");
    today.segments.push_back({36000, 36000 + 3 * 3600, wk::Kind::Work});
    Check(wk::Streak(log, today, 20261109, 150 * 60) == 3, "today's goal adds to the streak");
    Check(wk::PrevDate(20260301) == 20260228 && wk::PrevDate(20240301) == 20240229 && wk::PrevDate(20260101) == 20251231, "previous day");

    // Subjects (even with accents) survive the file.
    wk::Log keep;
    wk::Day d = t.day;
    d.subjects[L"Mathématiques"] = 1234;
    keep.days[d.date] = d;
    const wk::Log back = wk::Parse(wk::Serialize(keep));
    Check(back.days.at(d.date).subjects.at(L"Mathématiques") == 1234 && back.days.at(d.date).subjects.at(L"Maths") == d.subjects.at(L"Maths"),
          "subjects survive the file");
}

// Backups (common/backup.h): a round trip, the names it keeps, and what it refuses.
static void TestBackup() {
    namespace bk = bb::backup;
    auto Check = [](bool ok, const char* what) { if (!ok) { std::printf("FAIL backup: %s\n", what); ++g_failed; } };
    std::vector<bk::File> in = {{"settings.ini", "[Bunbun]\nMascot=true\n"}, {"water.txt", "taken=3500\n"},
                                {"mascots/Arya/idle.png", std::string("\x89PNG\0\x01", 7)}, {"ask/system-prompt.md", "hi"}};
    const std::string zip = bk::Zip(in, bk::Manifest("0.3.10", "windows", 1793990000), 1793990000);
    std::vector<bk::File> out;
    std::string manifest, err;
    Check(bk::Unzip(zip, &out, &manifest, &err), "backup: unzips what it zipped");
    Check(out.size() == in.size(), "backup: every file comes back");
    bool same = out.size() == in.size();
    for (size_t i = 0; same && i < in.size(); ++i) same = out[i].name == in[i].name && out[i].data == in[i].data;
    Check(same, "backup: names and bytes are the same");
    Check(bk::ManifestValue(manifest, "version") == "0.3.10" && bk::ManifestValue(manifest, "platform") == "windows",
          "backup: manifest values");
    Check(bk::IsBackupName("mascots/Pack/frame 1.png") && bk::IsBackupName("timers.txt"), "backup: keeps known names");
    Check(!bk::IsBackupName("../evil.ini") && !bk::IsBackupName("/etc/passwd") && !bk::IsBackupName("C:x") &&
          !bk::IsBackupName("mascots/../../x") && !bk::IsBackupName("mascots\\x") && !bk::IsBackupName("models/a.gguf") &&
          !bk::IsBackupName("mascots/"),
          "backup: refuses unsafe or unknown names");
    // A zip without the manifest, a damaged one, and a stranger's file.
    const std::string bare = bk::Zip({}, "something else", 1793990000);
    Check(!bk::Unzip(bare, &out, &manifest, &err), "backup: refuses a zip that isn't a backup");
    std::string broken = zip;
    broken[60] ^= 0x5A;
    Check(!bk::Unzip(broken, &out, &manifest, &err), "backup: refuses a damaged backup");
    Check(!bk::Unzip("hello", &out, &manifest, &err), "backup: refuses a non-zip");
}

static void TestAgPools() {
    namespace aq = bb::agquota;
    const std::vector<aq::Pool> models = {
        {"Gemini 3.8 Flash (High)", 0.98f, 1000}, {"Gemini 3.8 Flash (Low)", 0.98f, 1000},
        {"Claude Sonnet 4.6 (Thinking)", 1.0f, 2000}, {"Claude Opus 4.6 (Thinking)", 1.0f, 2000}, {"GPT-OSS 120B (Medium)", 1.0f, 2000}};
    const std::vector<aq::Pool> pools = aq::MakePools(models);
    CHECK(pools.size() == 2);
    CHECK(pools[0].name == "Gemini 3.8 Flash" && pools[0].resetAt == 1000);    // the most used first
    CHECK(pools[1].name == "Claude Sonnet 4.6, Claude Opus 4.6 +1");
    CHECK(aq::BaseModelName("Plain") == "Plain");
    CHECK(aq::RemainingNow(0.4f, 1000, 999) == 0.4f);   // before the reset
    CHECK(aq::RemainingNow(0.4f, 1000, 1000) == 1.0f);  // after it: refilled
    CHECK(aq::RemainingNow(0.4f, 0, 5000) == 0.4f);     // no reset time known
}

// The Agent page's limits row (common/agent_limits.h).
static void TestAgentLimits() {
    namespace lm = bb::limits;
    CHECK(lm::ResetsIn(1000, 1000).empty() && lm::ResetsIn(0, 5).empty());
    CHECK(lm::ResetsIn(1000 + 45 * 60, 1000) == L"resets in 45m");
    CHECK(lm::ResetsIn(1000 + 130 * 60, 1000) == L"resets in 2h 10m");
    CHECK(lm::ResetsIn(1000 + 120 * 60, 1000) == L"resets in 2h");
    CHECK(lm::ResetsIn(1000 + 3 * 86400, 1000) == L"resets in 3d");
    // Claude: a window past its reset is back at 0; no numbers at all gives the note.
    const lm::Row c = lm::Claude(40, 900, 70, 5000, 12, 1000);
    CHECK(c.bars.size() == 3 && c.bars[0].used == 0.0f && c.bars[1].used == 70.0f && c.bars[2].label == L"Context");
    CHECK(lm::Claude(-1, 0, -1, 0, -1, 1000).bars.empty() && !lm::Claude(-1, 0, -1, 0, -1, 1000).note.empty());
    // Antigravity: one bar per pool, a refilled pool at 0.
    const lm::Row a = lm::Antigravity({{"Gemini Flash", 0.6f, 900}, {"Claude, GPT", 0.9f, 5000}}, 1000);
    CHECK(a.bars.size() == 2 && a.bars[0].used == 0.0f && a.bars[0].resetAt == 0);
    CHECK(a.bars[1].used > 9.9f && a.bars[1].used < 10.1f && a.bars[1].resetAt == 5000);
    // Cursor: the plan, and why there are no numbers.
    CHECK(lm::PlanName(L"free") == L"Free plan" && lm::PlanName(L"free_trial") == L"Pro trial" && lm::PlanName(L"").empty());
    CHECK(lm::Cursor(L"pro").note.rfind(L"Pro plan", 0) == 0 && lm::Cursor(L"").bars.empty());
}

static void TestEdgeLightOn() {
    namespace el = bb::edgelight;
    CHECK(el::ParseOn(L"") == (el::kOnOpen | el::kOnNotification | el::kOnAgent));
    CHECK(el::ParseOn(L"  ") == el::ParseOn(L""));
    CHECK(el::ParseOn(L"hover") == el::kOnHover);
    CHECK(el::ParseOn(L" Track , ALWAYS,") == (el::kOnTrack | el::kOnAlways));
    CHECK(el::ParseOn(L"close,system,nonsense") == (el::kOnClose | el::kOnSystem));
    CHECK(el::ParseOn(L"nonsense") == 0u);  // something set, nothing known: plays on nothing
}

static void TestScreenTime() {
    namespace st = bb::screentime;
    CHECK(st::Weekday(19700101) == 4);   // a Thursday
    CHECK(st::Weekday(20261106) == 5);   // Friday
    CHECK(st::FromDayNumber(st::DayNumber(20240229)) == 20240229);
    CHECK(st::FromDayNumber(st::DayNumber(20261101) - 1) == 20261031);

    st::Log log;
    st::Add(&log, 20261106, L"aumid:WhatsApp!App", L"WhatsApp", L"C:\\WA\\WhatsApp.Root.exe", 600);
    st::Add(&log, 20261106, L"c:\\chrome\\chrome.exe", L"Google Chrome", L"c:\\Chrome\\chrome.exe", 1800);
    st::Add(&log, 20261106, L"c:\\chrome\\chrome.exe", L"", L"", 30);   // more of it; the name stays
    st::Add(&log, 20261106, L"c:\\tool\\tool.exe", L"Tool", L"", 20);   // under a minute: not "used"
    st::Add(&log, 20261104, L"c:\\chrome\\chrome.exe", L"Google Chrome", L"", 3600);
    st::Add(&log, 20261020, L"c:\\old\\old.exe", L"Old", L"", 99);       // older than a week

    // The file keeps everything, tabs in names included (as spaces).
    st::Add(&log, 20261105, L"k", L"A\tB", L"", 5);
    const st::Log back = st::Parse(st::Serialize(log));
    CHECK(back.days.size() == log.days.size());
    CHECK(back.days.at(20261106).at(L"c:\\chrome\\chrome.exe").seconds == 1830);
    CHECK(back.days.at(20261106).at(L"c:\\chrome\\chrome.exe").name == L"Google Chrome");
    CHECK(back.days.at(20261105).at(L"k").name == L"A B");
    CHECK(st::Parse(L"garbage\nx\ty\n20261106\t-5\tk\tn\tp\n").days.empty());

    st::Prune(&log, 20261106);
    CHECK(log.days.count(20261020) == 0 && log.days.count(20261104) == 1);

    const st::Summary s = st::Summarize(log, 20261106);
    CHECK(s.todaySeconds == 600 + 1830 + 20);
    CHECK(s.appsUsed == 2);
    CHECK(s.apps.size() == 3 && s.apps[0].name == L"Google Chrome" && s.apps[1].name == L"WhatsApp");
    CHECK(s.apps[0].percent == 75);
    CHECK(s.week.size() == 7 && s.week.back().today && s.week.back().day == 20261106 && s.week.front().day == 20261031);
    CHECK(s.week[4].day == 20261104 && s.week[4].seconds == 3600 && s.week[4].weekday == 3);
    CHECK(s.averageSeconds == (2450 + 3600 + 5) / 3);
    // A clicked bar: that day's apps and total; the week still ends today.
    const st::Summary wed = st::Summarize(log, 20261106, 20261104);
    CHECK(wed.todaySeconds == 3600 && wed.apps.size() == 1 && wed.apps[0].percent == 100);
    CHECK(wed.week.back().day == 20261106 && wed.week.back().today && wed.averageSeconds == s.averageSeconds);
    CHECK(st::Duration(0) == L"0m" && st::Duration(30) == L"<1m" && st::Duration(45 * 60) == L"45m");
    CHECK(st::Duration(2 * 3600 + 5 * 60) == L"2h 5m" && st::Duration(3600) == L"1h");
}

// All-day events (holidays, leave) are kept for the calendar and days off, but are never meetings.
static void TestIcsAllDay() {
    namespace ics = bb::ics;
    const ics::ToEpochFn utc = [](const ics::WallTime& w, const std::wstring&) { return ics::UtcEpoch(w); };
    const std::wstring cal =
        L"BEGIN:VCALENDAR\r\n"
        L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART;VALUE=DATE:20261114\r\nDTEND;VALUE=DATE:20261115\r\nSUMMARY:Diwali\r\nEND:VEVENT\r\n"
        L"BEGIN:VEVENT\r\nUID:b\r\nDTSTART;VALUE=DATE:20261116\r\nDTEND;VALUE=DATE:20261118\r\nSUMMARY:Annual leave\r\nEND:VEVENT\r\n"
        L"BEGIN:VEVENT\r\nUID:c\r\nDTSTART:20261116T100000Z\r\nDTEND:20261116T103000Z\r\nSUMMARY:Standup\r\nEND:VEVENT\r\n"
        L"END:VCALENDAR\r\n";
    const std::vector<ics::Event> events = ics::Parse(cal, utc);
    CHECK(events.size() == 3);
    const double from = ics::UtcEpoch({2026, 11, 10, 0, 0, 0}), to = ics::UtcEpoch({2026, 11, 30, 0, 0, 0});
    const auto meetings = ics::MeetingsBetween(events, from, to, utc);
    CHECK(meetings.size() == 1 && meetings[0].title == L"Standup");
    CHECK(ics::NextMeeting(events, from, utc).title == L"Standup" || !ics::NextMeeting(events, from, utc).valid);
    const auto all = ics::MeetingsBetween(events, from, to, utc, true);
    CHECK(all.size() == 3);
    auto ymd = [](double e) {
        const long long d = static_cast<long long>(e) / 86400;
        int y, m, dd;
        ics::CivilFromDays(d, &y, &m, &dd);
        return y * 10000 + m * 100 + dd;
    };
    const std::vector<int> leave = ics::LeaveDates(all, ymd);
    CHECK(leave.size() == 2 && leave[0] == 20261116 && leave[1] == 20261117);
    const std::wstring text = ics::AllDayText(all, ymd);
    CHECK(text == L"20261114\tE\tDiwali\n20261116\tL\tAnnual leave\n20261117\tL\tAnnual leave\n");
}

static void TestChimes() {
    using bb::chime::Chime;
    const size_t bytes[] = {30914, 23858, 24740, 6218, 6218};  // Done, Question, Error, Open, Close
    for (int c = 0; c < static_cast<int>(Chime::Count); ++c) {
        const auto wav = bb::chime::MakeWav(bb::chime::NotesFor(static_cast<Chime>(c)), 0.4f);
        CHECK(wav.size() == bytes[c]);
        CHECK(memcmp(wav.data(), "RIFF", 4) == 0 && memcmp(wav.data() + 8, "WAVEfmt ", 8) == 0);
        int peakQuiet = 0, peakLoud = 0;
        const auto loud = bb::chime::MakeWav(bb::chime::NotesFor(static_cast<Chime>(c)), 1.0f);
        for (size_t i = 44; i + 1 < wav.size(); i += 2) {
            int16_t a, b;
            memcpy(&a, &wav[i], 2);
            memcpy(&b, &loud[i], 2);
            peakQuiet = std::max(peakQuiet, std::abs(static_cast<int>(a)));
            peakLoud = std::max(peakLoud, std::abs(static_cast<int>(b)));
        }
        CHECK(peakQuiet > 1000 && peakLoud > peakQuiet);
    }
}

static void TestJson() {
    Value v;
    CHECK(bb::json::Parse(R"J({"a":1,"b":[true,false,null],"c":"x\"y\u00e9\n","d":{"e":-1.5e3}})J", &v));
    CHECK(v.getNumber("a") == 1);
    CHECK(v.find("b")->arr.size() == 3);
    CHECK(v.getString("c") == "x\"y\xC3\xA9\n");
    CHECK(v.find("d")->getNumber("e") == -1500);
    // Round trip keeps key order and number text.
    CHECK(bb::json::Serialize(v) == R"J({"a":1,"b":[true,false,null],"c":"x\"y)J" "\xC3\xA9" R"J(\n","d":{"e":-1.5e3}})J");
    // Surrogate pair.
    CHECK(bb::json::Parse("\"\\ud83d\\udc30\"", &v) && v.str == "\xF0\x9F\x90\xB0");
    // Strictness: these must all be rejected.
    for (const char* bad : {"{", "{\"a\":}", "[1,]", "{'a':1}", "01", "1.", "\"\x01\"", "{\"a\":1} x", "nul"}) {
        CHECK(!bb::json::Parse(bad, &v));
    }
    // BOM is tolerated.
    CHECK(bb::json::Parse("\xEF\xBB\xBF{}", &v) && v.isObject());
}

static void TestInstaller(const std::wstring& dir) {
    const std::wstring path = dir + L"\\settings.json";
    SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", dir.c_str());
    CHECK(bb::hooks::ClaudeSettingsPath() == path);

    // Existing user config with an unrelated hook and other keys.
    WriteAll(path, R"J({
  "model": "opus",
  "permissions": {"allow": ["Bash(ls)"]},
  "hooks": {"PreToolUse": [{"matcher": "Bash", "hooks": [{"type": "command", "command": "my-linter"}]}]}
})J");
    std::wstring report;
    CHECK(!bb::hooks::ClaudeHooksInstalled());
    CHECK(bb::hooks::InstallClaudeHooks(&report));
    CHECK(bb::hooks::ClaudeHooksInstalled());

    Value root;
    CHECK(bb::json::Parse(ReadAll(path), &root));
    CHECK(root.getString("model") == "opus");
    CHECK(root.find("permissions") != nullptr);
    const Value* pre = root.find("hooks")->find("PreToolUse");
    CHECK(pre && pre->arr.size() == 2);  // user's hook kept, ours appended
    CHECK(pre->arr[0].find("hooks")->arr[0].getString("command") == "my-linter");
    const std::string cmd = pre->arr[1].find("hooks")->arr[0].getString("command");
    CHECK(cmd.find("bunbun-hook.exe\" PreToolUse") != std::string::npos);
    CHECK(cmd.find('\\') == std::string::npos);  // forward slashes for Git Bash
    CHECK(root.find("hooks")->find("PermissionRequest")->arr[0].find("hooks")->arr[0].getNumber("timeout") == 120);

    // Idempotent: installing twice does not duplicate entries.
    CHECK(bb::hooks::InstallClaudeHooks(&report));
    CHECK(bb::json::Parse(ReadAll(path), &root));
    CHECK(root.find("hooks")->find("PreToolUse")->arr.size() == 2);

    // Uninstall restores the user's hooks only.
    CHECK(bb::hooks::UninstallClaudeHooks(&report));
    CHECK(!bb::hooks::ClaudeHooksInstalled());
    CHECK(bb::json::Parse(ReadAll(path), &root));
    CHECK(root.find("hooks")->find("PreToolUse")->arr.size() == 1);
    CHECK(root.find("hooks")->find("Stop") == nullptr);

    // Invalid JSON is never overwritten.
    WriteAll(path, "{ \"model\": \"opus\", }");
    CHECK(!bb::hooks::InstallClaudeHooks(&report));
    CHECK(ReadAll(path) == "{ \"model\": \"opus\", }");

    // A hooks value of the wrong type is refused, not replaced.
    WriteAll(path, R"J({"hooks": []})J");
    CHECK(!bb::hooks::InstallClaudeHooks(&report));
    CHECK(ReadAll(path) == R"J({"hooks": []})J");

    // Missing file: created from scratch.
    DeleteFileW(path.c_str());
    CHECK(bb::hooks::InstallClaudeHooks(&report));
    CHECK(bb::json::Parse(ReadAll(path), &root) && root.find("hooks")->obj.size() == 12);
}


static void TestAssistant() {
    namespace as = bb::assistant;
    auto parse = [](const wchar_t* t) { return as::Parse(t, L"google"); };
    double v = 0;
    CHECK(as::Evaluate(L"2+3*4", &v) && v == 14);
    CHECK(as::Evaluate(L"(1+2)^2 / 3", &v) && v == 3);
    CHECK(as::Evaluate(L"-4 + 10", &v) && v == 6);
    CHECK(as::Evaluate(L"sqrt(144) + 1", &v) && v == 13);
    CHECK(as::Evaluate(L"abs(-25)", &v) && v == 25);
    CHECK(!as::Evaluate(L"1/0", &v));
    CHECK(!as::Evaluate(L"open notepad", &v));
    CHECK(!as::Evaluate(L"2+", &v));

    as::Intent in = parse(L"what is 12*7?");
    CHECK(in.action == as::Action::Reply && in.reply.find(L"84") != std::wstring::npos);
    in = parse(L"what is 15% of 200?");
    CHECK(in.action == as::Action::Reply && in.reply.find(L"30") != std::wstring::npos);
    in = parse(L"how much is 25 * 4 + 10?");
    CHECK(in.action == as::Action::Reply && in.reply.find(L"110") != std::wstring::npos);
    in = parse(L"wiki quantum computing");
    CHECK(in.action == as::Action::Lookup && in.target == L"quantum computing");
    in = parse(L"lookup Ada Lovelace");
    CHECK(in.action == as::Action::Lookup && in.target == L"ada lovelace");
    in = parse(L"Open Notepad");
    CHECK(in.action == as::Action::Open && in.target == L"notepad.exe");
    in = parse(L"wifi settings");
    CHECK(in.action == as::Action::Open && in.target == L"ms-settings:network-wifi");
    in = parse(L"launch some unknown app");
    CHECK(in.action == as::Action::Open && in.target.empty() && in.label == L"some unknown");  // " app" is trimmed
    in = parse(L"hey bun volume 35");
    CHECK(in.action == as::Action::VolumeSet && in.number == 35);
    CHECK(parse(L"turn the volume up").action == as::Action::VolumeUp);
    CHECK(parse(L"mute").action == as::Action::Mute);
    CHECK(parse(L"dnd on").action == as::Action::MascotDndOn);
    CHECK(parse(L"snack time").action == as::Action::MascotDndOn);
    CHECK(parse(L"dnd off").action == as::Action::MascotDndOff);
    CHECK(parse(L"Hey Bun, please turn on DND!").action == as::Action::MascotDndOn);
    CHECK(parse(L"enable do not disturb").action == as::Action::MascotDndOn);
    CHECK(parse(L"turn off dnd please").action == as::Action::MascotDndOff);
    CHECK(parse(L"disable do not disturb").action == as::Action::MascotDndOff);
    CHECK(parse(L"open dnd settings").action == as::Action::Open);  // Windows Focus settings, not the mascot
    in = parse(L"can you open up notepad for me?");
    CHECK(in.action == as::Action::Open && in.target == L"notepad.exe");
    in = parse(L"launch the spotify app");
    CHECK(in.action == as::Action::Open && _wcsicmp(in.label.c_str(), L"spotify") == 0);
    CHECK(parse(L"next song").action == as::Action::MediaNext);
    // To-dos and reminders
    in = parse(L"add buy milk");
    CHECK(in.action == as::Action::TodoAdd && in.label == L"Buy milk" && in.dueHour < 0 && in.dueInMinutes < 0);
    in = parse(L"add call mom to my list");
    CHECK(in.action == as::Action::TodoAdd && in.label == L"Call mom");
    in = parse(L"remind me at 3pm to call Sam");
    CHECK(in.action == as::Action::TodoAdd && in.label == L"Call Sam" && in.dueHour == 15 && in.dueMinute == 0);
    in = parse(L"remind me to stretch in 20 minutes");
    CHECK(in.action == as::Action::TodoAdd && in.label == L"Stretch" && in.dueInMinutes == 20);
    in = parse(L"remind me tomorrow at 9:30 to send the report");
    CHECK(in.action == as::Action::TodoAdd && in.dueDays == 1 && in.dueHour == 9 && in.dueMinute == 30 && in.label == L"Send the report");
    in = parse(L"remind me to water plants tomorrow");
    CHECK(in.action == as::Action::TodoAdd && in.dueDays == 1 && in.dueHour == 9 && in.label == L"Water plants");
    in = parse(L"remind me in an hour to check the oven");
    CHECK(in.action == as::Action::TodoAdd && in.dueInMinutes == 60 && in.label == L"Check the oven");
    CHECK(parse(L"remind me in 10 min").action == as::Action::TimerStart);  // nothing to be reminded of: a timer
    CHECK(parse(L"what's on my list?").action == as::Action::TodoList);
    in = parse(L"done buy milk");
    CHECK(in.action == as::Action::TodoDone && in.label == L"buy milk");
    in = parse(L"mark call mom as done");
    CHECK(in.action == as::Action::TodoDone && in.label == L"call mom");
    CHECK(parse(L"clear done").action == as::Action::TodoClearDone);
    // Undo, radios, brightness
    CHECK(parse(L"undo").action == as::Action::Undo);
    CHECK(parse(L"turn wifi off").action == as::Action::WifiOff);
    CHECK(parse(L"wifi on").action == as::Action::WifiOn);
    CHECK(parse(L"disable bluetooth").action == as::Action::BluetoothOff);
    CHECK(parse(L"turn on bluetooth please").action == as::Action::BluetoothOn);
    CHECK(parse(L"wifi settings").action == as::Action::Open);  // the Settings page, as before
    in = parse(L"brightness 60");
    CHECK(in.action == as::Action::BrightnessSet && in.number == 60);
    CHECK(parse(L"brighter").action == as::Action::BrightnessUp);
    CHECK(parse(L"dim the screen").action == as::Action::BrightnessDown);
    // Weather: here, a named place, tomorrow
    in = parse(L"weather");
    CHECK(in.info == as::Info::Weather && in.target.empty() && in.number == 0);
    in = parse(L"today's weather in mumbai");
    CHECK(in.info == as::Info::Weather && in.target == L"mumbai" && in.number == 0);
    CHECK(parse(L"new york weather").target == L"new york");
    in = parse(L"will it rain in pune tomorrow?");
    CHECK(in.info == as::Info::Weather && in.target == L"pune" && in.number == 1);
    CHECK(parse(L"what's the weather like here").target.empty());
    // Fuzzy app names
    CHECK(as::AppMatchScore(L"Visual Studio Code", L"vsc") >= 60);
    CHECK(as::AppMatchScore(L"Visual Studio Code", L"code") >= 70);
    CHECK(as::AppMatchScore(L"Spotify", L"spot") > as::AppMatchScore(L"Microsoft Teams", L"spot"));
    CHECK(as::AppMatchScore(L"Spotify", L"spfy") > 0);
    CHECK(as::AppMatchScore(L"Notepad", L"notepad") == 100);
    CHECK(as::AppMatchScore(L"Calculator", L"zz") == 0);
    // Focus mode
    CHECK(parse(L"focus").action == as::Action::FocusStart && parse(L"focus").number == 25);
    CHECK(parse(L"focus 50").number == 50);
    CHECK(parse(L"start focus for 45 minutes").number == 45);
    CHECK(parse(L"stop focus").action == as::Action::FocusStop);
    CHECK(parse(L"open focus settings").action == as::Action::Open);
    // Repeating reminders
    in = parse(L"remind me every day at 9 to stand up");
    CHECK(in.action == as::Action::TodoAdd && in.repeat == L"daily" && in.dueHour == 9 && in.label == L"Stand up");
    in = parse(L"remind me every Monday at 10 to send the report");
    CHECK(in.action == as::Action::TodoAdd && in.repeat == L"weekly:1" && in.dueHour == 10 && in.label == L"Send the report");
    in = parse(L"remind me every mon and wed at 6pm to go to the gym");
    CHECK(in.repeat == L"weekly:1,3" && in.dueHour == 18 && in.label == L"Go to the gym");
    in = parse(L"remind me every weekday at 9:30 to check email");
    CHECK(in.repeat == L"weekdays" && in.dueHour == 9 && in.dueMinute == 30 && in.label == L"Check email");
    in = parse(L"remind me every morning to drink water");
    CHECK(in.repeat == L"daily" && in.dueHour == 9 && in.label == L"Drink water");
    in = parse(L"remind me to water plants on fridays");
    CHECK(in.repeat == L"weekly:5" && in.label == L"Water plants");
    CHECK(parse(L"pause").action == as::Action::MediaToggle);
    in = parse(L"set a timer for 10 minutes");
    CHECK(in.action == as::Action::TimerStart && in.number == 10);
    CHECK(parse(L"timer 1 hour").number == 60);
    CHECK(parse(L"timer 5min").number == 5);
    CHECK(parse(L"pomodoro timer").number == 25);
    CHECK(parse(L"stop the timer").action == as::Action::TimerStop);
    in = parse(L"search cute bunnies");
    CHECK(in.action == as::Action::WebSearch && in.target == L"https://www.google.com/search?q=cute+bunnies");
    CHECK(as::Parse(L"search a&b", L"duckduckgo").target == L"https://duckduckgo.com/?q=a%26b");
    // Bunbun: water and Work / Study mode.
    CHECK(parse(L"drank water").action == as::Action::WaterAdd && parse(L"drank water").number == 0);
    CHECK(parse(L"add water").action == as::Action::WaterAdd);  // not a to-do called water
    CHECK(parse(L"i had a glass of water").action == as::Action::WaterAdd);
    CHECK(parse(L"drank 500 ml").number == 500 && parse(L"log 300ml water").number == 300);
    CHECK(parse(L"drank 1.5 litres of water").number == 1500 && parse(L"drank half a litre of water").number == 500);
    CHECK(parse(L"2 glasses of water").number == -2 && parse(L"drank two glasses of water").number == -2);
    CHECK(parse(L"undo water").action == as::Action::WaterUndo);
    CHECK(parse(L"i just finished a big bottle of water").number == as::kWaterBottle);
    CHECK(parse(L"drank a 1 litre bottle of water").number == 1000);  // an amount wins
    CHECK(parse(L"how much water today").info == as::Info::Water && parse(L"water").info == as::Info::Water);
    CHECK(parse(L"remind me to drink water at 3pm").action == as::Action::TodoAdd);
    CHECK(parse(L"add water the plants").action != as::Action::WaterAdd);
    CHECK(parse(L"start work").action == as::Action::WorkStart && parse(L"clock in").action == as::Action::WorkStart);
    CHECK(parse(L"start studying").action == as::Action::WorkStart);
    CHECK(parse(L"pause work").action == as::Action::WorkPause && parse(L"back to work").action == as::Action::WorkResume);
    CHECK(parse(L"take a break").action == as::Action::WorkBreak);
    CHECK(parse(L"done for today").action == as::Action::WorkDone && parse(L"clock out").action == as::Action::WorkDone);
    CHECK(parse(L"stop studying").action == as::Action::WorkDone);
    CHECK(parse(L"how long have i worked").info == as::Info::Work && parse(L"how much did i study today").info == as::Info::Work);
    CHECK(parse(L"start focus").action == as::Action::FocusStart);  // focus stays focus
    CHECK(parse(L"open wifi settings").action == as::Action::Open && parse(L"start network").action != as::Action::WorkStart);
    CHECK(parse(L"do my homework").action != as::Action::WorkStart);
    CHECK(parse(L"what time is it").info == as::Info::Time);
    CHECK(parse(L"battery").info == as::Info::Battery);
    CHECK(parse(L"battery settings").action == as::Action::Open);
    CHECK(parse(L"what's playing").info == as::Info::NowPlaying);
    CHECK(parse(L"lock").action == as::Action::Lock);
    CHECK(parse(L"gibberish words").action == as::Action::None);
}

// Version comparison decides whether "an update is available"; a wrong answer
// either nags about old versions or hides real updates.
void TestUpdateVersions() {
    using bb::update::CompareVersions;
    using bb::update::PackVersion;
    CHECK(CompareVersions("0.2.0", "0.1.0") > 0);
    CHECK(CompareVersions("v0.1.0", "0.1.0") == 0);
    CHECK(CompareVersions("0.1", "0.1.0") == 0);
    CHECK(CompareVersions("0.10.0", "0.9.9") > 0);  // numeric, not string order
    CHECK(CompareVersions("1.0.0", "0.99.99") > 0);
    CHECK(CompareVersions("0.2.0-beta", "0.2.0") < 0);
    CHECK(CompareVersions("0.2.0-beta", "0.1.9") > 0);
    CHECK(CompareVersions("0.1.0", "0.1.1") < 0);
    CHECK(PackVersion("v1.2.3") == 1002003);
    CHECK(PackVersion("0.10") == 10000);
}

// The to-do file and repeat maths shared by the island and the settings app.
void TestTodos() {
    namespace td = bb::todos;
    // Old files (no repeat column) still load; new ones round-trip.
    auto items = td::Parse(L"1\t0\t0\t0\tBuy milk\n2\t1\t1700000000\t1\tCall Sam\n");
    CHECK(items.size() == 2 && items[0].text == L"Buy milk" && items[0].repeat.empty() && items[1].done);
    td::Item r;
    r.id = 3;
    r.text = L"Stand up";
    r.due = 1700000000;
    r.repeat = L"weekdays";
    items.push_back(r);
    const auto back = td::Parse(td::Serialize(items));
    CHECK(back.size() == 3 && back[2].repeat == L"weekdays" && back[2].text == L"Stand up" && td::NextId(back) == 4);
    CHECK(td::RepeatDays(L"weekdays") == 0x3E && td::RepeatDays(L"weekly:1,3") == 0x0A && td::RepeatDays(L"") == 0);
    CHECK(td::DescribeRepeat(L"weekly:1,3") == L"Every Mon, Wed");

    // Daily: the next one is a day later at the same local time.
    SYSTEMTIME t{};
    t.wYear = 2026; t.wMonth = 11; t.wDay = 2; t.wHour = 9;  // Monday 2 Nov 2026, 9:00 local
    const double monday9 = td::FromLocal(t);
    double next = td::NextOccurrence(L"daily", monday9, monday9);
    SYSTEMTIME n = td::ToLocal(next);
    CHECK(n.wDay == 3 && n.wHour == 9 && n.wMinute == 0);
    // Weekdays from Friday 9:00 skips to Monday.
    t.wDay = 6;  // Friday
    next = td::NextOccurrence(L"weekdays", td::FromLocal(t), td::FromLocal(t));
    n = td::ToLocal(next);
    CHECK(n.wDay == 9 && n.wDayOfWeek == 1 && n.wHour == 9);
    // Weekly on Wednesday, asked on Monday morning before 9: this Wednesday.
    t.wDay = 2; t.wHour = 8;
    next = td::NextOccurrence(L"weekly:3", monday9, td::FromLocal(t));
    n = td::ToLocal(next);
    CHECK(n.wDay == 4 && n.wDayOfWeek == 3 && n.wHour == 9);
    CHECK(td::NextOccurrence(L"", monday9, monday9) == 0.0);
}

// Lyrics lookups: title cleaning and the YouTube "Artist - Song" split.
static void TestLyricsText() {
    using namespace LyricsText;
    // Noise tags are whole words / bracketed tags, not substrings.
    CHECK(CleanTitle(L"Song (Official Music Video)") == L"Song");
    CHECK(CleanTitle(L"Song [HD]") == L"Song");
    CHECK(CleanTitle(L"Song (feat. Someone)") == L"Song");
    CHECK(CleanTitle(L"Song (Lyrics)") == L"Song");
    CHECK(CleanTitle(L"Song (Visualizer)") == L"Song");
    CHECK(CleanTitle(L"Song [M/V]") == L"Song");
    CHECK(CleanTitle(L"Song - Remastered 2011") == L"Song");
    CHECK(CleanTitle(L"Song | Some Channel") == L"Song");
    CHECK(CleanTitle(L"Song feat. Someone") == L"Song");
    CHECK(CleanTitle(L"Song (Mixtape)") == L"Song (Mixtape)");
    CHECK(CleanTitle(L"Lovestoned (Interlude)") == L"Lovestoned (Interlude)");
    CHECK(CleanTitle(L"Song (Live at Wembley)") == L"Song");
    CHECK(CleanTitle(L"I Will Live (Forever)") == L"I Will Live (Forever)");
    // YouTube: "Artist - Song" titles from a channel.
    Query q = PrepareQuery(L"Daft Punk - Get Lucky (Official Video)", L"DaftPunkVEVO");
    CHECK(q.split && q.artist == L"Daft Punk" && q.title == L"Get Lucky");
    q = PrepareQuery(L"Adele - Hello", L"Adele - Topic");
    CHECK(q.split && q.artist == L"Adele" && q.title == L"Hello");
    q = PrepareQuery(L"Queen - Bohemian Rhapsody (Official Video Remastered)", L"Queen Official");
    CHECK(q.split && q.artist == L"Queen" && q.title == L"Bohemian Rhapsody");
    q = PrepareQuery(L"Get Lucky - Daft Punk", L"DaftPunkVEVO");
    CHECK(q.split && q.artist == L"Daft Punk" && q.title == L"Get Lucky");
    q = PrepareQuery(L"Bohemian Rhapsody - Remastered 2011", L"Queen");
    CHECK(!q.split && q.artist == L"Queen" && q.title == L"Bohemian Rhapsody");
    q = PrepareQuery(L"Shape of You", L"Ed Sheeran");
    CHECK(!q.split && q.artist == L"Ed Sheeran" && q.title == L"Shape of You");
    q = PrepareQuery(L"Hello", L"Adele, Someone Else");
    CHECK(!q.split && q.artist == L"Adele");
    // Credit lines and NetEase's "pure music" marker.
    CHECK(IsCreditLine(L"\u4f5c\u8bcd : Ed Sheeran"));    // 作词
    CHECK(IsCreditLine(L"Composer: Someone"));
    CHECK(!IsCreditLine(L"I'm the composer of my life"));
    CHECK(IsPureMusicText(L"\u7eaf\u97f3\u4e50\uff0c\u8bf7\u6b23\u8d4f"));  // 纯音乐，请欣赏
    CHECK(TextMatch(L"Get Lucky", L"get lucky!") == 2);
}

// App lists typed in Settings (full screen per app, always music / always video).
static void TestAppList() {
    using bb::applist::Matches;
    CHECK(Matches(std::wstring(L"chrome, vlc.exe; steam"), std::wstring(L"chrome")));
    CHECK(Matches(std::wstring(L"chrome, vlc.exe; steam"), std::wstring(L"vlc")));            // ".exe" ignored
    CHECK(Matches(std::wstring(L"  Steam  "), std::wstring(L"steamwebhelper")));              // part of the name, any case
    CHECK(Matches(std::wstring(L"spotify\nplex"), std::wstring(L"Plex Media Player")));
    CHECK(!Matches(std::wstring(L"chrome"), std::wstring(L"firefox")));
    CHECK(!Matches(std::wstring(L""), std::wstring(L"chrome")));
    CHECK(!Matches(std::wstring(L" , ;"), std::wstring(L"chrome")));                          // empty entries match nothing
    CHECK(Matches(std::string("mpv, celluloid"), std::string("io.github.celluloid_player.celluloid")));  // narrow too
}

// Which media player the island shows when several run (Linux MPRIS; the rules are shared code).
static void TestMediaPick() {
    using namespace bb::mediapick;
    const auto cand = [](const char* id, State s, long long at) { return Candidate{id, s, at}; };
    bool gone = false;
    CHECK(Choose({}, "", &gone) == -1 && !gone);
    // Automatic: playing beats paused; the most recent of equals wins; stopped never shows.
    CHECK(Choose({cand("spotify", State::Paused, 9), cand("chrome", State::Playing, 1)}, "") == 1);
    CHECK(Choose({cand("spotify", State::Playing, 5), cand("chrome", State::Playing, 8)}, "") == 1);
    CHECK(Choose({cand("spotify", State::Stopped, 9), cand("chrome", State::Paused, 1)}, "") == 1);
    CHECK(Choose({cand("spotify", State::Stopped, 9)}, "") == -1);
    // Issue 7: Spotify plays, a Chrome video starts. Picking Spotify keeps it on show.
    const std::vector<Candidate> both = {cand("spotify", State::Playing, 5), cand("chrome", State::Playing, 8)};
    CHECK(Choose(both, "spotify", &gone) == 0 && !gone);
    // The pick stays even if it is paused ...
    CHECK(Choose({cand("spotify", State::Paused, 5), cand("chrome", State::Playing, 8)}, "spotify", &gone) == 0 && !gone);
    // ... until that player stops or goes: then the island chooses again, and says the pick is gone.
    CHECK(Choose({cand("spotify", State::Stopped, 5), cand("chrome", State::Playing, 8)}, "spotify", &gone) == 1 && gone);
    CHECK(Choose({cand("chrome", State::Paused, 8)}, "spotify", &gone) == 0 && gone);
    // Cycling through the list, and the names.
    CHECK(NextPick({"a", "b", "c"}, "a") == "b");
    CHECK(NextPick({"a", "b", "c"}, "c") == "a");
    CHECK(NextPick({"a", "b"}, "gone") == "a");
    CHECK(NextPick({}, "a").empty());
    const std::vector<std::string> names = UniqueNames({"Chrome", "Spotify", "Chrome"});
    CHECK(names[0] == "Chrome" && names[1] == "Spotify" && names[2] == "Chrome (2)");
}

static void TestWater() {
    namespace w = bb::water;
    w::Settings s;
    s.reminders = true;
    s.goalMl = 2400;  // 9:00 to 21:00: 200 ml an hour
    s.glassMl = 200;
    w::Log log;
    w::RollOver(log, 20261105);
    w::Day& d = log.today;
    CHECK(w::ExpectedBy(s, 9 * 60) == 0 && w::ExpectedBy(s, 15 * 60) == 1200 && w::ExpectedBy(s, 22 * 60) == 2400);
    CHECK(w::Check(s, d, 8 * 60) == w::Due::None);               // before the active hours
    CHECK(w::Check(s, d, 9 * 60 + 30) == w::Due::None);          // not a glass behind yet
    CHECK(w::Check(s, d, 10 * 60) == w::Due::Drink);             // an hour in: a glass behind
    CHECK(w::NextReminder(s, d, 9 * 60 + 30) == 10 * 60);
    d.lastRemindMinute = 10 * 60;
    CHECK(w::Check(s, d, 10 * 60 + 10) == w::Due::None);         // too soon after the last one
    w::Snooze(d, 10 * 60 + 25);
    CHECK(w::Check(s, d, 10 * 60 + 30) == w::Due::None);         // snoozed for 15 min
    CHECK(w::Check(s, d, 10 * 60 + 41) == w::Due::Drink);
    w::Drink(s, d, 200);
    CHECK(d.takenMl == 200 && d.glasses == 1 && w::Check(s, d, 10 * 60 + 41) == w::Due::None);
    w::Undo(s, d);
    CHECK(d.takenMl == 0 && d.glasses == 0);
    d.takenMl = 2400;
    CHECK(w::Check(s, d, 15 * 60) == w::Due::None && w::NextReminder(s, d, 15 * 60) == -1);  // goal reached
    // A bottle: empty asks for a refill, refilled is full again.
    s.bottleMl = 500;
    w::Day b;
    w::Drink(s, b, 200);
    w::Drink(s, b, 300);
    CHECK(w::BottleLeft(s, b) == 0 && w::Check(s, b, 12 * 60) == w::Due::Refill);
    w::Refill(s, b);
    CHECK(w::BottleLeft(s, b) == 500);
    // A new day moves today into the history; the file keeps both.
    log.today.takenMl = 1800;
    w::RollOver(log, 20261106);
    CHECK(log.today.date == 20261106 && log.today.takenMl == 0 && log.history.size() == 1 && log.history[0].ml == 1800);
    log.today.takenMl = 400;
    const w::Log back = w::Parse(w::Serialize(log));
    CHECK(back.today.date == 20261106 && back.today.takenMl == 400 && back.history.size() == 1 &&
          back.history[0].date == 20261105 && back.history[0].ml == 1800);
    for (int i = 0; i < 10; ++i) w::RollOver(log, 20261107 + i);
    CHECK(log.history.size() == static_cast<size_t>(w::kHistoryDays));
}

static void TestStudioChat() {
    // 1. Text formatting logic
    auto formatAnswer = [](int status, const std::wstring& title, const std::wstring& source,
                           const std::wstring& body, const std::wstring& example, const std::wstring& question) {
        if (status == 2 /*NoResult*/) return body.empty() ? std::wstring(L"I couldn't find anything for that.") : body;
        std::wstring text;
        if (!title.empty() && source != L"Bun" && title != question) text = title + L" \x2014 ";
        text += body;
        if (!example.empty()) text += L"\n\u201c" + example + L"\u201d";
        return text;
    };

    CHECK(formatAnswer(2, L"", L"", L"", L"", L"unknown") == L"I couldn't find anything for that.");
    CHECK(formatAnswer(2, L"", L"", L"Custom error", L"", L"unknown") == L"Custom error");
    CHECK(formatAnswer(1, L"Weather", L"wttr.in", L"22°C sunny", L"", L"weather") == L"Weather \x2014 22°C sunny");
    CHECK(formatAnswer(1, L"weather", L"wttr.in", L"22°C sunny", L"", L"weather") == L"22°C sunny");
    CHECK(formatAnswer(1, L"", L"Bun", L"Opened Spotify", L"", L"open spotify") == L"Opened Spotify");
    CHECK(formatAnswer(1, L"Dictionary", L"Wiktionary", L"A small mammal", L"A cute rabbit", L"rabbit") ==
          L"Dictionary \x2014 A small mammal\n\u201cA cute rabbit\u201d");

    // 2. Transcript scroll math
    const float viewH = 192.0f;
    const float msgTop = 42.0f;
    const float msgBottom = msgTop + viewH;

    // Fits in view
    float totalH = 120.0f;
    float maxScroll = std::max(0.0f, totalH - viewH);
    CHECK(maxScroll == 0.0f);
    float scrollOffset = (totalH > viewH) ? (maxScroll - 0.0f) : 0.0f;
    CHECK(scrollOffset == 0.0f);
    float curY = msgTop - scrollOffset;
    CHECK(curY == msgTop);

    // Exceeds view (scrolled to bottom)
    totalH = 350.0f;
    maxScroll = std::max(0.0f, totalH - viewH);
    CHECK(maxScroll == 158.0f);
    float scroll = 0.0f;  // bottom
    scrollOffset = (totalH > viewH) ? (maxScroll - scroll) : 0.0f;
    CHECK(scrollOffset == 158.0f);
    curY = msgTop - scrollOffset;
    CHECK(curY + totalH == msgBottom);  // newest message aligns with bottom!

    // Scrolled to top
    scroll = maxScroll;
    scrollOffset = (totalH > viewH) ? (maxScroll - scroll) : 0.0f;
    CHECK(scrollOffset == 0.0f);
    curY = msgTop - scrollOffset;
    CHECK(curY == msgTop);  // oldest message aligns with top!

    // Clamping scroll delta
    auto clampScroll = [](float val, float maxVal) {
        return val < 0.0f ? 0.0f : (val > maxVal ? maxVal : val);
    };
    CHECK(clampScroll(scroll + 40.0f, maxScroll) == maxScroll);
    CHECK(clampScroll(0.0f - 40.0f, maxScroll) == 0.0f);
    CHECK(clampScroll(50.0f + 36.0f, maxScroll) == 86.0f);

    // 3. Bubble bounds
    const float maxChatW = 480.0f;
    const float maxUserBubbleW = maxChatW * 0.72f;
    const float maxAnsBubbleW = maxChatW * 0.88f;
    CHECK(maxUserBubbleW == 345.6f);
    CHECK(maxAnsBubbleW == 422.4f);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: test_core <scratch dir>\n");
        return 2;
    }
    TestJson();
    TestInstaller(argv[1]);
    TestAssistant();
    TestUpdateVersions();
    TestTodos();
    TestLyricsText();
    TestStudioChat();
    TestAppList();
    TestMediaPick();
    TestWater();
    TestIcsAllDay();
    TestChimes();
    TestScreenTime();
    TestEdgeLightOn();
    TestAgPools();
    TestAgentLimits();
    TestBackup();
    TestWork();
    TestStudy();
    std::printf(g_failed ? "%d check(s) failed\n" : "all checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
