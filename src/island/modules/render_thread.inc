// Bunbun: The render thread: frame loop and window messages.
// Split out of island_core.cpp; included inside its anonymous namespace at the
// original position, so declaration order is unchanged. Not a standalone file.

DWORD WINAPI RenderThreadProc(void*) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClass, L"Dynamic Island for Windows", WS_POPUP, 0, 0, 520, 140,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        Wh_Log(L"Failed to create Dynamic Island overlay window.");
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
        return 0;
    }

    g_hwnd = hwnd;
    if (g_shellHookMessage == 0) g_shellHookMessage = RegisterWindowMessageW(L"SHELLHOOK");
    if (g_taskbarCreatedMessage == 0) g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    using ChangeWindowMessageFilterEx_t = BOOL(WINAPI*)(HWND, UINT, DWORD, PVOID);
    static auto pChangeWindowMessageFilterEx = reinterpret_cast<ChangeWindowMessageFilterEx_t>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx"));
    if (pChangeWindowMessageFilterEx) {
        if (g_shellHookMessage) pChangeWindowMessageFilterEx(hwnd, g_shellHookMessage, 1 /*MSGFLT_ALLOW*/, nullptr);
        if (g_taskbarCreatedMessage) pChangeWindowMessageFilterEx(hwnd, g_taskbarCreatedMessage, 1 /*MSGFLT_ALLOW*/, nullptr);
    } else {
        using ChangeWindowMessageFilter_t = BOOL(WINAPI*)(UINT, DWORD);
        static auto pChangeWindowMessageFilter = reinterpret_cast<ChangeWindowMessageFilter_t>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilter"));
        if (pChangeWindowMessageFilter) {
            if (g_shellHookMessage) pChangeWindowMessageFilter(g_shellHookMessage, 1 /*MSGFLT_ADD*/);
            if (g_taskbarCreatedMessage) pChangeWindowMessageFilter(g_taskbarCreatedMessage, 1 /*MSGFLT_ADD*/);
        }
    }
    EnableBlurBehind(hwnd);
    ApplyBackdropMaterial(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ApplyHideShowHotkey();

    if (g_settings.autoHideIdleSeconds == 0) {
        g_manuallyHidden = false;
        Wh_SetIntValue(L"ManuallyHidden", 0);
    } else {
        g_manuallyHidden = Wh_GetIntValue(L"ManuallyHidden", 0) != 0;
    }
    if (g_manuallyHidden.load()) {
        ShowWindow(hwnd, SW_HIDE);
    }

    Renderer renderer;
    {
        std::lock_guard frameLock(g_frameMutex);
        g_snapshotRenderer = &renderer;
    }
    // Unpublished on every exit path, before `renderer` goes away.
    struct SnapshotRendererGuard {
        ~SnapshotRendererGuard() {
            std::lock_guard frameLock(g_frameMutex);
            g_snapshotRenderer = nullptr;
        }
    } snapshotRendererGuard;
    // Bunbun: the other displays' islands at rest (Separate mode), drawn off screen.
    Renderer restRenderer;
    if (restRenderer.Initialize(nullptr)) g_restRenderer = &restRenderer;
    struct RestRendererGuard {
        ~RestRendererGuard() { g_restRenderer = nullptr; g_restState = nullptr; }
    } restRendererGuard;
    if (!renderer.Initialize(hwnd)) {
        DestroyWindow(hwnd);
        g_hwnd = nullptr;
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
        return 0;
    }

    // Restore the File Tray from disk. Done here because the shell icons need the
    // COM apartment this thread already initialised.
    LoadFileTray();

    // A 1ms timer resolution costs power, so it is requested only while the island
    // is actually animating. It used to be requested once here and released at
    // shutdown, so a parked or idle island held it for the mod's whole lifetime.
    //
    // Since Windows 10 2004 this affects only the calling process's timers rather
    // than the system clock globally, but the power cost is the reason to scope it
    // either way.
    //
    // The flag is only set when timeBeginPeriod actually succeeded, so the
    // begin/end pairs stay balanced -- these calls are reference counted per
    // process, and an unmatched timeEndPeriod would decrement someone else's
    // request.
    bool highResTimer = false;

    // How long the resolution is held after animation stops. Long enough to ride
    // out the gaps between 60Hz content frames and brief pauses between spring
    // animations without flapping, short enough that a settled island gives it back
    // promptly. A plain local rather than a static, so an unload/reload cycle cannot
    // carry a stale timestamp across.
    constexpr double kHighResTimerHoldSec = 0.5;
    double lastAnimatingAt = -1.0;

    auto setHighResTimer = [&](bool want) {
        if (want == highResTimer) {
            return;
        }
        if (want) {
            if (timeBeginPeriod(1) == TIMERR_NOERROR) {
                highResTimer = true;
            }
        } else {
            timeEndPeriod(1);
            highResTimer = false;
        }
    };

    SpringValue widthSpring;
    SpringValue heightSpring;
    SpringValue nudgeSpring;
    SpringValue volumeBarSpring;
    widthSpring.Reset((g_settings.autoHideIdleSeconds == -1 ? 0.0f : 120.0f) * g_settings.sizeScale);
    heightSpring.Reset((g_settings.autoHideIdleSeconds == -1 ? 0.0f : 36.0f) * g_settings.sizeScale);
    nudgeSpring.Reset(0.0f);

    IslandKind previousPrimary = IslandKind::Idle;
    auto previousFrame = std::chrono::steady_clock::now();
    auto nextFrameTarget = previousFrame;
    double nextBatteryPoll = 0.0;
    double nextProgressPoll = 0.0;
    double nextSystemPoll = 0.0;
    double nextBrightnessPoll = 0.0;
    double nextPrivacyPoll = 0.0;
    bool wasManuallyHidden = false;
    bool wasAutoHiddenParked = false;
    bool parkedForFullscreen = false;

    static double lastInteractionTime = NowSeconds();
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        const double iterStart = QpcMs();  // Bunbun: BUNBUN_LOG loop-cost diagnostics
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_APP_NEW_EVENT) {
                nudgeSpring.value = -6.0f;
                nudgeSpring.velocity = 0.0f;
                nudgeSpring.target = 0.0f;
                lastInteractionTime = NowSeconds();
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        BindVolumeEndpoint();

        bool justUnhidden = false;

        if (g_manuallyHidden.load()) {
            // Manual hide always takes precedence over — and invalidates —
            // any auto-park bookkeeping, since the window's shown/hidden
            // state is now fully owned by the manual toggle. Without this,
            // toggling manual-hide off after having been auto-parked would
            // leave g_autoHiddenParked stale-true and the window stuck
            // hidden.
            g_autoHiddenParked = false;
            wasAutoHiddenParked = false;

            if (!wasManuallyHidden) {
                // Just hid: drop to zero-CPU parking immediately, no
                // lingering render/animation work this frame.
                ShowWindow(hwnd, SW_HIDE);
                DwmBackdrop::HideAll();
                Mirrors::HideAll();  // Bunbun: the hotkey hid only this display's island
                g_audioCaptureNeeded.store(false, std::memory_order_relaxed);
                wasManuallyHidden = true;
            }
            previousFrame = std::chrono::steady_clock::now();
            // Fully parked: no polling, no timer wakeups — only the stop
            // event or a posted/queued message (hotkey, settings change,
            // clipboard update, etc.) wakes this thread while hidden. Nothing
            // is being paced, so give the system timer resolution back.
            setHighResTimer(false);
            MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
            nextFrameTarget = std::chrono::steady_clock::now();
            continue;
        }

        // Sits ahead of the parked branch below on purpose: that branch can block
        // for up to 1.5s, and the mouse thread needs to hear about a transition
        // before that, not after.
        {
            static bool prevParkedForHook = false;
            const bool parkedNow = g_autoHiddenParked.load(std::memory_order_relaxed);
            if (parkedNow != prevParkedForHook) {
                prevParkedForHook = parkedNow;
                NotifyMouseThreadParkedChanged();
            }
        }

        if (g_autoHiddenParked.load()) {
            if (g_settings.autoHideIdleSeconds == 0 && !parkedForFullscreen) {
                g_autoHiddenParked = false;
            } else {
                previousFrame = std::chrono::steady_clock::now();
                // Auto-parked, so nothing is animating; same reasoning as above.
                setHighResTimer(false);
                if (parkedForFullscreen) {
                    MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, 1500, QS_ALLINPUT);
                    const bool stillFullscreen =  // Bunbun: still a full screen that hides
                        FullscreenWatched(g_settings) && FullscreenAction(hwnd, g_settings) == 2;
                    g_isFullscreen.store(stillFullscreen, std::memory_order_relaxed);
                    if (!stillFullscreen) {
                        g_fullscreenOverrideVisible = false;
                        g_autoHiddenParked = false;
                    } else if (g_fullscreenOverrideVisible.load()) {
                        g_autoHiddenParked = false;
                    }
                } else {
                    MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
                }
                nextFrameTarget = std::chrono::steady_clock::now();
                continue;
            }
        }

        if (wasManuallyHidden) {
            // Just un-hid. Show now; springs get snapped straight to their
            // freshly-computed targets further down this same iteration so
            // there's no stale pop-in animation from wherever they were
            // left off before hiding.
            wasManuallyHidden = false;
            justUnhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            lastInteractionTime = NowSeconds();
            g_layoutDirty = true;
            previousFrame = std::chrono::steady_clock::now();
            nextFrameTarget = previousFrame;
        }

        if (wasAutoHiddenParked && !g_autoHiddenParked.load()) {
            // Something woke us (mouse near the island, a transient alert,
            // the hotkey, fullscreen ending, etc.) — reveal and let this
            // frame's normal logic decide whether to actually stay visible
            // or immediately re-collapse and re-park.
            wasAutoHiddenParked = false;
            justUnhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            lastInteractionTime = NowSeconds();
            g_layoutDirty = true;
            previousFrame = std::chrono::steady_clock::now();
            nextFrameTarget = previousFrame;
        }

        const double now = NowSeconds();
        // True while a tab transition is genuinely being painted. The registry /
        // device polls below run on this thread and can take several ms, which
        // shows up as a hitch mid-animation, so they wait for the spring to settle.
        const bool tabBusy = g_tabAnimating.load(std::memory_order_relaxed) &&
                             (now - g_tabLastPaintAt.load(std::memory_order_relaxed)) < 0.25;
        // The island itself is resizing (spring state from the previous frame).
        const bool springBusy =
            std::fabs(widthSpring.value - widthSpring.target) > 0.5f ||
            std::fabs(heightSpring.value - heightSpring.target) > 0.5f ||
            std::fabs(widthSpring.velocity) > 0.5f ||
            std::fabs(heightSpring.velocity) > 0.5f;
        const bool pollBusy = tabBusy || springBusy;
        if (now >= nextBatteryPoll) {
            UpdateBatterySnapshot();
            nextBatteryPoll = now + 15.0;
        }
        if (now >= nextProgressPoll) {
            UpdateProgressSnapshot();
            nextProgressPoll = now + 0.25;
        }
        // Bunbun: privacy dots are sampled on their own thread (privacy_module.inc).
        BrainIdleCheck(g_settings);  // Bunbun: free the local AI's memory when idle

        bool timerJustCompleted = false;
        int finishedKind = 0;              // Bunbun: the Timers page
        std::wstring finishedLabel;
        SharedState snapshot;
        {
            std::lock_guard lock(g_stateMutex);
            snapshot = g_state;
            if (g_state.timer.active && g_state.timer.running && now >= g_state.timer.endsAt) {
                g_state.timer.active = false;
                g_state.timer.running = false;
                g_state.timer.justFinished = true;
                g_state.timer.finishedExpiresAt = now + 6.0;
                timerJustCompleted = true;
                finishedKind = g_state.timer.kind;  // Bunbun
                finishedLabel = g_state.timer.label;
            }
            if (g_state.timer.justFinished && now >= g_state.timer.finishedExpiresAt) {
                g_state.timer.justFinished = false;
            }
            snapshot.timer = g_state.timer;
            if (g_state.clipboard.active && now >= g_state.clipboard.expiresAt) {
                g_state.clipboard.active = false;
                snapshot.clipboard.active = false;
            }
            if (g_state.notification.active && now >= g_state.notification.expiresAt) {
                g_state.notification.active = false;
                snapshot.notification.active = false;
            }
            if (g_state.volume.active && now >= g_state.volume.expiresAt) {
                g_state.volume.active = false;
                snapshot.volume.active = false;
            }
            if (g_state.brightness.active && now >= g_state.brightness.expiresAt) {
                g_state.brightness.active = false;
                snapshot.brightness.active = false;
            }
            if (g_state.capsLock.active && now >= g_state.capsLock.expiresAt) {
                g_state.capsLock.active = false;
                snapshot.capsLock.active = false;
            }
            if (g_state.battery.active && now >= g_state.battery.expiresAt) {
                g_state.battery.active = false;
                snapshot.battery.active = false;
            }
            if (g_state.device.active && now >= g_state.device.expiresAt) {
                g_state.device.active = false;
                snapshot.device.active = false;
            }
        }
            if (timerJustCompleted) {
                TriggerNudge();
        }
        UpdateFocus(timerJustCompleted, snapshot.timer.active);  // Bunbun: ends Focus mode with its summary
        if (timerJustCompleted) Timers::OnTimerFinished(finishedKind, finishedLabel, g_settings, now);  // Bunbun
        Timers::Tick(g_settings, now);  // Bunbun: the next Pomodoro phase starts by itself

        // Quick Lookup: hand keyboard focus back once the panel has timed out or the
        // module was switched off in settings.
        if (g_lookupUi.open &&
            (!g_settings.quickLookup || !snapshot.lookup.active || now >= snapshot.lookup.expiresAt)) {
            CloseLookupPanel(hwnd);
        }

        const std::vector<IslandKind> kinds = ChooseActivities(snapshot, g_settings, now);
        g_lyricsTabAvailable.store(
            g_settings.lyrics && g_settings.media && snapshot.media.available,
            std::memory_order_relaxed);
        // Bunbun: one scroll ring. The wheel steps through every live activity (an
        // agent session, a timer, a progress ring...) and then every dashboard tab of
        // the host view (music if something plays, else the clock): media, calendar,
        // weather, hardware, files, lyrics -- and wraps. Alerts and approval cards
        // are not in the ring: they always show at once. A short while after the
        // cursor leaves, the ring snaps back to its first, most important entry.
        auto inRing = [&snapshot](IslandKind k) {
            // Bunbun: a running download and a meeting about to start too, so the wheel
            // (and opening the island) can reach the pages past them.
            return k == IslandKind::Media || k == IslandKind::Idle || k == IslandKind::Timer ||
                   k == IslandKind::Progress || k == IslandKind::Download || k == IslandKind::Meeting ||
                   (k == IslandKind::Agent && !AgentLayout::IsCard(snapshot.agent.phase));
        };
        std::vector<IslandKind> ringActivities;
        IslandKind ringHost = IslandKind::Idle;
        for (IslandKind k : kinds) {
            if (k == IslandKind::Media) ringHost = IslandKind::Media;
            else if (k != IslandKind::Idle && inRing(k)) ringActivities.push_back(k);
        }
        // Bunbun: exactly the pages the host view draws (DashboardTabIds), so the
        // wheel reaches the last one (All apps) and never stops on a phantom tab.
        const int ringTabs = std::max(1, static_cast<int>(
            DashboardTabIds(ringHost == IslandKind::Media ? 0 : 1, g_settings).size()));
        // A media session that isn't the main view (paused, say) gets the last stop,
        // so the player is always reachable: past the last tile, or one notch up from
        // the first. At rest the island still shows the clock.
        const bool mediaStop = ringHost == IslandKind::Idle && g_settings.media && snapshot.media.available;
        const int ringSize = static_cast<int>(ringActivities.size()) + ringTabs + (mediaStop ? 1 : 0);
        g_activeActivityCount.store(ringSize, std::memory_order_relaxed);
        // Bunbun: a timer, agent or download joining or leaving the ring shifts every
        // page's stop. Keep the page on screen (Reset on a running timer jumped to the
        // next page). Only while a page is shown, not the activity itself.
        {
            const int count = static_cast<int>(ringActivities.size());
            const int was = g_ringActivityCount.load(std::memory_order_relaxed);
            const int idx = g_activityIndex.load(std::memory_order_relaxed);
            if (count != was && idx >= was && idx > 0)
                g_activityIndex.store(std::max(count, idx + (count - was)), std::memory_order_relaxed);
        }
        g_ringActivityCount.store(static_cast<int>(ringActivities.size()), std::memory_order_relaxed);  // Bunbun

        static double s_lastRingHover = 0.0;
        // Bunbun: not while pinned open (no pointer there to keep it alive).
        if (now - s_lastRingHover > 2.5 && !g_pinnedExpanded.load()) g_activityIndex.store(0, std::memory_order_relaxed);
        int actIdx = ((g_activityIndex.load(std::memory_order_relaxed) % ringSize) + ringSize) % ringSize;

        IslandKind shownKind = kinds[0];
        const bool ringStepped = g_ringStepped.exchange(false, std::memory_order_relaxed);  // Bunbun
        // Bunbun: a page capture steers the tab itself; the ring must not pull it back.
        if (inRing(kinds[0]) && !g_capturingPages.load(std::memory_order_relaxed)) {
            if (actIdx < static_cast<int>(ringActivities.size())) {
                shownKind = ringActivities[static_cast<size_t>(actIdx)];
            } else if (mediaStop && actIdx == ringSize - 1) {
                shownKind = IslandKind::Media;  // Bunbun: the player's own stop
                g_idleTab = MediaTabIndex(g_settings);
            } else {
                shownKind = ringHost;
                // Ring positions past the activities are the host's dashboard tabs.
                // Only steer the tab while the user is scrolling the ring, so the
                // tab they picked earlier stays put otherwise.
                const int tab = actIdx - static_cast<int>(ringActivities.size());
                // Bunbun: also when the wheel wraps back to the first stop. Leaving
                // that out kept the last tab (To-do) on screen for one dead notch,
                // then the next notch went on to Calendar.
                if ((ringStepped || g_activityIndex.load(std::memory_order_relaxed) != 0) && g_idleTab.load() != tab) {
                    g_idleTab = tab;
                }
            }
        }

        // Bunbun: while the settings app takes pictures of the pages, show the pages
        // (not an agent pill or another activity on top of them).
        if (g_capturingPages.load(std::memory_order_relaxed)) shownKind = ringHost;
        Activity primary = ActivityForKind(shownKind, g_settings, snapshot);
        std::optional<Activity> secondary;
        // Secondary is only shown if an agent approval/card requires immediate stand-alone prompt,
        // or if not scrolling between disparate active modules.
        // Bunbun: never. With coding agents off, a second activity sat beside the first
        // (Do Not Disturb beside a notification, the timer beside the mascot) and widened
        // the notch; the owner found it cluttered. One at a time: the others wait their turn
        // (alerts expire, live activities are on the scroll ring), as with agents on.
        (void)kinds;

        const bool pinned = g_pinnedExpanded.load();

        if (primary.kind != previousPrimary) {
            if (primary.kind != IslandKind::Idle) {
                nudgeSpring.value = -6.0f;
                nudgeSpring.velocity = 0.0f;
                nudgeSpring.target = 0.0f;
            }
            lastInteractionTime = now;
        }
        previousPrimary = primary.kind;

        // Bunbun: islands on all displays, working separately: the island moves to the
        // display whose island the pointer reaches, while it is at rest, and sits where
        // that display's island is set to.
        if (g_settings.displaysAll && g_settings.displaysSeparate) {
            POINT pointer{};
            GetCursorPos(&pointer);
            const bool resting = !pinned && !g_clickExpanded.load() &&
                                 heightSpring.value < 64.0f * g_settings.sizeScale;
            if (resting)
                if (const HMONITOR there = Mirrors::RestIslandUnder(pointer)) g_separateAnchor.store(there, std::memory_order_relaxed);
            const HMONITOR anchor = GetAnchorMonitor();
            MonitorEnumData mons;
            EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&mons));
            int index = 0;
            for (size_t i = 0; i < mons.monitors.size(); ++i)
                if (mons.monitors[i] == anchor) index = static_cast<int>(i) + 1;
            const bool isMain = g_settings.targetMonitor > 0 ? index == g_settings.targetMonitor
                                                            : anchor == MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
            const Position want = isMain || index == 0 ? g_settings.basePosition : Mirrors::PositionFor(index, g_settings);
            // ...and that display's shape (Appearance.DisplayShapes), else the island's own.
            const int shape = index == 0 ? -1 : DisplayShape(g_settings, index);
            bool notch = shape < 0 ? g_settings.baseNotch : shape == 2, w11 = shape < 0 ? g_settings.baseW11 : shape == 1;
            if (IsBottomPosition(want)) notch = false;
            if (want != g_settings.position || notch != g_settings.notchStyle || w11 != g_settings.w11Style) {
                std::lock_guard lock(g_settingsMutex);
                g_settings.position = want;
                g_settings.notchStyle = notch;
                g_settings.w11Style = w11;
                g_layoutDirty = true;
            }
        } else if (g_settings.position != g_settings.basePosition || g_settings.notchStyle != g_settings.baseNotch ||
                   g_settings.w11Style != g_settings.baseW11) {
            std::lock_guard lock(g_settingsMutex);
            g_settings.position = g_settings.basePosition;
            g_settings.notchStyle = g_settings.baseNotch;
            g_settings.w11Style = g_settings.baseW11;
            g_layoutDirty = true;
        }

        POINT cursor = {};
        GetCursorPos(&cursor);

        // Pill rectangle in screen space, from the same layout Render() uses. The window
        // is a big fixed stage now, so its own rect says nothing about where the pill is.
        const bool flushTopNow = g_settings.notchStyle || g_settings.borderMergedMode;
        const StageLayout hoverStage =
            ComputeStageLayout(g_settings, widthSpring.value, heightSpring.value);
        const float hoverTop = hoverStage.pillTop +
            (flushTopNow ? std::max(0.0f, nudgeSpring.value) : nudgeSpring.value);
        RECT pillRect = {
            hoverStage.origin.x + static_cast<int>(std::lround(hoverStage.pillLeft)),
            hoverStage.origin.y + static_cast<int>(std::lround(hoverTop)),
            hoverStage.origin.x + static_cast<int>(std::lround(hoverStage.pillLeft + widthSpring.value)),
            hoverStage.origin.y + static_cast<int>(std::lround(hoverTop + heightSpring.value))
        };

        bool hover = false;
        const auto noteRingHover = [&](bool h) { if (h) s_lastRingHover = now; };
        if (widthSpring.value > 1.0f && heightSpring.value > 1.0f) {
            hover = PtInRect(&pillRect, cursor) != FALSE ||
                    Mirrors::PointerOn(cursor, g_settings);  // Bunbun: Linked islands on other displays
        } else if (g_settings.unhideOnHover) {
            RECT dockRect = GetIslandDockRect();
            hover = PtInRect(&dockRect, cursor) != FALSE;
        }
        if (g_hoverSuppressed.load(std::memory_order_relaxed)) {
            if (!hover) {
                g_hoverSuppressed.store(false, std::memory_order_relaxed);
            } else {
                hover = false;
            }
        }

        // A file drag hovering over the island counts as hover, so it expands onto
        // the File Tray even with "Expand on hover" turned off.
        if (g_trayDragOver.load(std::memory_order_relaxed)) {
            hover = true;
        }
        noteRingHover(hover);

        bool needsRender = false;

        if (!hover && g_clickExpanded.load()) {
            g_clickExpanded = false;
            needsRender = true;
        }
        if (!hover && g_hoveredMediaButton.load() != -1) {
            g_hoveredMediaButton = -1;
            needsRender = true;
        }
        // Active playback used to count as a continuous event, so the island
        // stayed visible for as long as anything was playing and kept resetting the
        // auto-hide timer. Only the 5s window after a *title* change is transient
        // now, so the pill behaves like the clipboard and battery alerts -- it surfaces,
        // then hides again while playback continues in the background.
        // Bunbun: videos (the Streaming pill) never auto-expand.
        const bool recentTrackChange = g_settings.mediaAutoExpand &&
                                       !snapshot.media.video &&
                                       !MediaExpandBlocked(snapshot.media) &&
                                       primary.kind == IslandKind::Media &&
                                       snapshot.media.playing &&
                                       !snapshot.media.title.empty() &&
                                       (now - snapshot.media.titleChangedAt < 5.0);

        bool isTransientAlert = (primary.kind == IslandKind::Clipboard ||
                                 primary.kind == IslandKind::Notification ||
                                 primary.kind == IslandKind::Volume ||
                                 primary.kind == IslandKind::Brightness ||
                                 primary.kind == IslandKind::BatteryLow ||
                                 primary.kind == IslandKind::CapsLock ||
                                 primary.kind == IslandKind::Device ||
                                 primary.kind == IslandKind::Bluetooth ||
                                 primary.kind == IslandKind::DoNotDisturb ||
                                 primary.kind == IslandKind::Lookup ||
                                 recentTrackChange);
        // Bunbun: a request or a finish/error flash surfaces even when hidden;
        // a session that is merely working respects hiding and fullscreen.
        if (primary.kind == IslandKind::Agent && snapshot.agent.phase != bb::AgentPhase::Working &&
            snapshot.agent.phase != bb::AgentPhase::Thinking) {
            isTransientAlert = true;
        }

        const bool unhideGraceActive = (now < g_hotkeyUnhideUntil.load());
        const bool hoverUnhides = g_settings.unhideOnHover && hover;

        // Bunbun: idle means the user is away: no keyboard or mouse input anywhere, nothing playing and no
        // screen being shared or recorded. It used to count only touches on the island itself.
        if (g_settings.autoHideIdleSeconds > 0) {
            LASTINPUTINFO lii{sizeof(lii)};
            const bool inputRecent = GetLastInputInfo(&lii) && (GetTickCount() - lii.dwTime) / 1000.0 < g_settings.autoHideIdleSeconds;
            if (inputRecent || snapshot.system.screenActive || snapshot.media.playing) lastInteractionTime = now;
        }

        bool currentlyHidden = false;
        if (!unhideGraceActive) {
            if (g_settings.autoHideIdleSeconds == -1 && !isTransientAlert && !pinned) {
                currentlyHidden = true;
            } else if (g_settings.autoHideIdleSeconds > 0) {
                currentlyHidden = (now - lastInteractionTime > g_settings.autoHideIdleSeconds);
            }
        }

        bool isHoverExpanded = g_settings.expandOnHover ? hover : (hover && g_clickExpanded.load());
        const bool gameMetricsPresent = primary.kind == IslandKind::Idle && GameOverlayShown(g_settings.gameOverlay);
        if (gameMetricsPresent) {
            isHoverExpanded = false;
        } else if (g_settings.gameOverlay || g_gameOverlayPinned.load()) {
            // Bunbun: a wheel peek is open. It stays open while the cursor is on the
            // island and closes 1.5 s after it leaves.
            const double wall = NowSeconds();
            if (hover) {
                g_gameOverlayPeekUntil.store(std::max(g_gameOverlayPeekUntil.load(), wall + 1.5), std::memory_order_relaxed);
                isHoverExpanded = true;
            }
        }

        // Bunbun: while a timer or stopwatch runs its pill comes first, and opening the
        // island only made that pill taller: no way to the pages. In Studio it opens on the
        // Timers page instead (the ring moves on to it; the pill is back once you leave).
        // Bunbun: the same for a progress bar or a meeting about to start (they open on the
        // page you were on), with music beside the pill too, and for a running download
        // when pinned (hovering it is left alone: a click on it opens the file).
        const bool passivePill = primary.kind == IslandKind::Timer || primary.kind == IslandKind::Progress ||
                                 primary.kind == IslandKind::Meeting || (primary.kind == IslandKind::Download && pinned);
        if (g_settings.studioLayout && passivePill && (isHoverExpanded || pinned)) {
            // In the page list of the view it opens in (music adds pages to it).
            const std::vector<int> ids = DashboardTabIds(ringHost == IslandKind::Media ? 0 : 1, g_settings);
            int tab = std::clamp(g_idleTab.load(), 0, std::max(0, static_cast<int>(ids.size()) - 1));
            if (primary.kind == IslandKind::Timer) {
                const auto it = std::find(ids.begin(), ids.end(), kTabFocus);
                if (it != ids.end()) tab = static_cast<int>(it - ids.begin());
            }
            if (!ids.empty()) {
                g_idleTab = tab;
                g_activityIndex.store(static_cast<int>(ringActivities.size()) + tab, std::memory_order_relaxed);
                needsRender = true;
            }
        }
        {
            // Bunbun: a blip when the dashboard opens or folds (Bunbun.SoundOpenClose).
            static bool s_wasOpen = false;
            const bool open = (isHoverExpanded || pinned) &&
                              (primary.kind == IslandKind::Idle || primary.kind == IslandKind::Media);
            if (open != s_wasOpen) PlayChime(open ? Chime::Open : Chime::Close);
            s_wasOpen = open;
            g_dashboardOpen.store(open && !currentlyHidden, std::memory_order_relaxed);
        }
        if (currentlyHidden && !g_settings.unhideOnHover) {
            isHoverExpanded = false;
        } else if (isHoverExpanded || hoverUnhides || pinned || isTransientAlert || unhideGraceActive) {
            lastInteractionTime = now;
            g_lastIslandInteractionTime.store(now, std::memory_order_relaxed);
        }

        bool isHidden = false;
        if (!unhideGraceActive) {
            if (g_settings.autoHideIdleSeconds == -1 && !isTransientAlert && !isHoverExpanded && !hoverUnhides && !pinned) {
                isHidden = true;
            } else if (g_settings.autoHideIdleSeconds > 0) {
                if (hoverUnhides) {
                    isHidden = false;
                } else {
                    isHidden = (now - lastInteractionTime > g_settings.autoHideIdleSeconds);
                }
            }
        }

        static bool isFullscreen = false;
        static bool s_fullscreenTuckNow = false;  // Bunbun: this full screen tucks (else hides)
        static bool s_fullscreenTucked = false;   // Bunbun: we folded it for this full screen
        static bool s_tuckPeek = false;           // Bunbun: shown from the line in full screen; folds back
        static double s_peekLeftAt = 0.0;         // Bunbun: when the pointer left the peeking island
        static double lastFullscreenCheck = -1.0;  // -1.0 guarantees the very first iteration checks
        if (now - lastFullscreenCheck > 0.5) {
            // Bunbun: per app (the Show / Tuck / Hide lists) on top of In full screen.
            HWND fsWin = nullptr;
            const bool separateAll = g_settings.displaysAll && g_settings.displaysSeparate;
            int fsAction = FullscreenWatched(g_settings) ? FullscreenAction(hwnd, g_settings, &fsWin, GetAnchorMonitor(), separateAll) : 0;
            // Bunbun: Separate displays. Only the full-screen app's display folds. If that is
            // the working island's display, the island moves to another one; that display's
            // own island then does the tucking or hiding (Mirrors::Present).
            HMONITOR fsOther = nullptr;
            if (fsAction != 0 && g_settings.displaysAll && g_settings.displaysSeparate) {
                const HMONITOR fsMon = MonitorFromWindow(fsWin, MONITOR_DEFAULTTONEAREST);
                HMONITOR anchor = GetAnchorMonitor();
                if (fsMon == anchor && !pinned && !isHoverExpanded) {
                    if (const HMONITOR other = Mirrors::OtherDisplay(fsMon)) {
                        g_separateAnchor.store(other, std::memory_order_relaxed);
                        anchor = other;
                    }
                }
                if (fsMon != anchor) {
                    fsOther = fsMon;
                    g_fsOtherAction.store(fsAction, std::memory_order_relaxed);
                    fsAction = 0;  // the working island isn't on that display
                }
            }
            g_fsOtherMonitor.store(fsOther, std::memory_order_relaxed);
            const bool newFullscreen = fsAction != 0;
            s_fullscreenTuckNow = fsAction == 1;
            if (isFullscreen && !newFullscreen) {
                // Fullscreen ended — re-arm so the next fullscreen session
                // hides again even if the hotkey was used to reveal the
                // island this time.
                g_fullscreenOverrideVisible = false;
            }
            // Bunbun: with "Tuck instead of hiding", full screen folds the island to a
            // line, and leaving full screen unfolds it again (only if we folded it).
            if (s_fullscreenTuckNow && newFullscreen && !isFullscreen && !g_tucked.load()) {
                SetTucked(true);
                s_fullscreenTucked = true;
            } else if (!newFullscreen && isFullscreen && s_fullscreenTucked) {
                if (g_tucked.load()) SetTucked(false);
                s_fullscreenTucked = false;
                s_tuckPeek = false;  // Bunbun: full screen is over: the island stays out
            }
            isFullscreen = newFullscreen;
            g_isFullscreen.store(isFullscreen, std::memory_order_relaxed);
            if (isFullscreen) g_fullscreenSeenAt.store(now, std::memory_order_relaxed);  // Bunbun: for the DND pill filter
            lastFullscreenCheck = now;
        }

        // Reclaim the top of the z-order if another always-on-top window has
        // been raised over the island. Cheap and rate-limited, and skipped
        // while the island is hidden or suppressed so it cannot un-hide itself.
        static double lastTopmostCheck = 0.0;
        if (!isFullscreen && !g_manuallyHidden.load() && !g_autoHiddenParked.load() &&
            now - lastTopmostCheck > 1.0) {
            EnsureTopmost(hwnd, &pillRect);
            lastTopmostCheck = now;
        }

        const bool micIndicatorActive = snapshot.system.micActive && g_settings.privacyDots && g_settings.privacyDotsMic;
        const bool camIndicatorActive = snapshot.system.cameraActive && g_settings.privacyDots && g_settings.privacyDotsCam;
        const bool privacyActive = micIndicatorActive || camIndicatorActive;

        // Whether any surface that actually draws CPU / RAM / disk / GPU / network
        // figures is on screen: the in-game overlay, or the idle dashboard's
        // Hardware Monitor tab while expanded (hovered or pinned) and scrolled into
        // view.
        //
        // Hoisted out of the poll block below because two decisions need it: whether
        // to sample the expensive GPU and network counters at all, and whether a
        // change in those numbers is worth repainting for.
        const int metricTabIdx = NormalizedTabIndex(g_settings);
        const bool onHardwareMonitorTab = (metricTabIdx == HardwareMonitorTabIndex(g_settings));
        const bool hwMonitorVisible = (primary.kind == IslandKind::Idle || primary.kind == IslandKind::Media) &&
            !isFullscreen && !gameMetricsPresent && (pinned || isHoverExpanded) && onHardwareMonitorTab;
        const bool gameOverlayVisible = gameMetricsPresent && !isFullscreen;
        const bool systemMetricsVisible = hwMonitorVisible || gameOverlayVisible;

        // Bunbun: keep the connectivity sampler running while its tab is on screen,
        // and wake it the moment the tab appears so the status is current.
        if ((primary.kind == IslandKind::Idle || primary.kind == IslandKind::Media) && !isFullscreen &&
            !gameMetricsPresent && (pinned || isHoverExpanded) &&
            (g_settings.studioLayout || metricTabIdx == ConnectivityTabIndex(g_settings))) {  // Bunbun: Studio shows battery and radios on every page (header, System)
            const double wallNow = NowSeconds();
            if (wallNow - g_connectivityWantedAt.load(std::memory_order_relaxed) > 2.5 && g_connectivityWake) {
                SetEvent(g_connectivityWake);
            }
            g_connectivityWantedAt.store(wallNow, std::memory_order_relaxed);
        }

        if (g_settings.brightness && !pollBusy && now >= nextBrightnessPoll) {
            UpdateBrightnessSnapshot();
            nextBrightnessPoll = now + 0.25;
        }

        if (!pollBusy && now >= nextSystemPoll) {
            const bool needGpuStats = gameOverlayVisible || hwMonitorVisible;
            const bool needNetStats = hwMonitorVisible;  // net is only ever drawn in the HW dashboard

            UpdateSystemSnapshot(needGpuStats, needNetStats);
            if (!g_endpointVolume) {
                g_volumeEndpointStale = true;  // no audio device yet, or binding failed
            }
            screentime::Tick(now);
            nextSystemPoll = now + (g_powerSaving && !systemMetricsVisible ? 2.0 : 1.0);  // Bunbun: battery saver
        }

        if (primary.kind == IslandKind::Idle) {
            // Bunbun: in a full screen that tucks, the peeking island opens on hover too
            // (only Hide keeps it closed); it never could, so the pill just sat there.
            if (!(isFullscreen && !s_fullscreenTuckNow) && (pinned || isHoverExpanded)) {
                primary.width = MediaLayout::kExpandedWidth * g_settings.sizeScale;
                primary.height = MediaLayout::kExpandedHeight * g_settings.sizeScale;
                if (g_settings.studioLayout) {  // Bunbun: header + page + tab bar
                    primary.width = kStudioIslandW * g_settings.sizeScale;
                    primary.height = kStudioIslandH * g_settings.sizeScale;
                }
            } else if (primary.width > 0.0f) {
                // Collapsed: size the strip to the text it will actually render.
                // ActivityForKind's 96/170px was wrong both ways -- dead air around
                // a short "9:41", and clipping on "10:41:32 PM" at larger Text size.
                // The > 0 guard preserves ActivityForKind's fully-hidden case.
                primary.width =
                    renderer.MeasureIdleStrip(snapshot, g_settings, now).totalWidth *
                    g_settings.sizeScale;
            }
        }
        if (!isFullscreen && primary.kind == IslandKind::Idle &&
            GameOverlayShown(g_settings.gameOverlay)) {
            // Width follows the metrics actually enabled (#25) so switching some
            // off shrinks the strip instead of leaving empty space, and compact
            // mode narrows it enough to sit neatly in the taskbar area. Both the
            // size and the painting come from GameOverlayLayout, so the strip
            // cannot end up sized for a card width DrawGameOverlay no longer uses.
            const int metricCount = (g_settings.gameOverlayShowCpu ? 1 : 0) +
                                    (g_settings.gameOverlayShowRam ? 1 : 0) +
                                    (g_settings.gameOverlayShowGpu ? 1 : 0) +
                                    (g_settings.gameOverlayShowDisk ? 1 : 0);

            const bool compact = g_settings.gameOverlayCompact;
            primary.width = GameOverlayLayout::Width(compact, g_settings.gameOverlayShowFps,
                                                     metricCount,
                                                     g_settings.bunbunMascot && g_settings.bunbunMascotOnIsland) *  // Bunbun
                            g_settings.sizeScale;
            primary.height = GameOverlayLayout::For(compact).height * g_settings.sizeScale;
        }
        if (primary.kind == IslandKind::Media) {
            if (!(isFullscreen && !s_fullscreenTuckNow) && (isHoverExpanded || pinned || recentTrackChange)) {  // Bunbun: as above
                primary.width = MediaLayout::kExpandedWidth * g_settings.sizeScale;
                primary.height = MediaLayout::kExpandedHeight * g_settings.sizeScale;
                if (g_settings.studioLayout) {  // Bunbun: header + page + tab bar
                    primary.width = kStudioIslandW * g_settings.sizeScale;
                    primary.height = kStudioIslandH * g_settings.sizeScale;
                }
                // (Immersive lyrics: the island keeps its normal 380px width.)
            } else {
                // Optional clock section on the left of the collapsed pill.
                const float clockSection = g_settings.mediaPillClock
                    ? renderer.MeasureMediaPill(snapshot, g_settings).sectionWidth
                    : 0.0f;
                if (CollapsedLyricsActive(snapshot, g_settings)) {
                    // Collapsed but carrying lyrics: wider pill, same height.
                    // Bunbun: title-only is narrower; lyrics fit the song's longest line.
                    const float fitted = g_collapsedLyricsWidth.load(std::memory_order_relaxed);
                    const float lyricsW = fitted > 0.0f ? fitted : kCollapsedLyricsWidth;
                    primary.width = ((g_settings.collapsedLyrics && !StreamingPill(snapshot, g_settings) ? lyricsW : kCollapsedTitleWidth) +
                                     clockSection) * g_settings.sizeScale;
                } else if (g_settings.mediaPillClock) {
                    primary.width = (MediaPillLayout::kBaseWidth + clockSection) * g_settings.sizeScale;
                }
            }
        }

        if (primary.kind == IslandKind::Agent && AgentLayout::IsCard(snapshot.agent.phase)) {
            secondary.reset();  // the approval card stands alone; its buttons own the clicks
            // Bunbun: a question card is as tall as its wrapped question needs.
            if (NeedsQuestionCard(snapshot.agent)) {
                const Settings questionSettings = GetSettingsCopy();
                primary.height = renderer.MeasureQuestionCard(snapshot, questionSettings) * questionSettings.sizeScale;
            }
        }

        if (primary.kind == IslandKind::Lookup) {
            secondary.reset();  // the panel stands alone (see HandleLookupClick)
            if (primary.width > 1.0f) {
                // The Lookup card is sized to its (wrapped, line-capped) content.
                const Settings lookupSettings = GetSettingsCopy();
                primary.height = renderer.MeasureLookupCard(snapshot, lookupSettings) *
                                 lookupSettings.sizeScale;
                if (lookupSettings.studioLayout) {  // Bunbun: Ask Bun as a chat
                    primary.width = kStudioChatW * lookupSettings.sizeScale;
                    primary.height = kStudioChatH * lookupSettings.sizeScale;
                }
            }
        }

        bool hideCollapse = false;  // Bunbun: this frame the island is folding away (idle hide, full screen)
        const bool fullscreenSuppressed =
            isFullscreen && !g_fullscreenOverrideVisible.load(std::memory_order_relaxed) &&
            !s_fullscreenTuckNow;  // Bunbun: tucked instead (see the full screen check)
        if ((isHidden || fullscreenSuppressed) && !privacyActive && !pinned && !isHoverExpanded && !isTransientAlert) {
            primary.width = 0.0f;
            primary.height = 0.0f;
            secondary.reset();
            hideCollapse = true;
        }

        // Bunbun: tucked away. The island shrinks to a thin line; pointing at it for
        // a moment (after the pointer has left it once) brings the island back, and so
        // does an agent that needs an answer.
        if (g_tucked.load(std::memory_order_relaxed)) {
            const bool needsYou = snapshot.agent.phase == bb::AgentPhase::Approval ||
                                  snapshot.agent.phase == bb::AgentPhase::Question;
            const LONG zoneHalfW = static_cast<LONG>(80 * g_settings.sizeScale);
            const LONG zoneH = static_cast<LONG>(12 * g_settings.sizeScale);
            const LONG lineCx = (pillRect.left + pillRect.right) / 2;
            // Screen space, like pillRect and cursor. Reaches above the line so the
            // very top edge of the screen counts.
            const RECT zone{lineCx - zoneHalfW, pillRect.top - zoneH, lineCx + zoneHalfW, pillRect.top + zoneH};
            const bool onLine = PtInRect(&zone, cursor) != FALSE;
            if (!onLine) {
                g_tuckPointerLeft = true;
                g_tuckHoverSince = 0.0;
            } else if (g_tuckPointerLeft && g_tuckHoverSince <= 0.0) {
                g_tuckHoverSince = now;
            }
            s_tuckPeek = false;  // Bunbun: tucked; only the reveal below starts a new peek
            // Bunbun: a notification that came in after tucking peeks out too, for as long
            // as it shows; not in full screen, where tucking keeps the game or film clear.
            const bool notifying = !isFullscreen && snapshot.notification.active &&
                                   snapshot.notification.expiresAt > now &&
                                   (snapshot.notification.shownAt > 0.0 ? snapshot.notification.shownAt : snapshot.notification.expiresAt - 4.0) > g_tuckedAt;
            if (needsYou || notifying ||
                (g_tuckHoverSince > 0.0 && now - g_tuckHoverSince > g_settings.tuckRevealMs / 1000.0)) {
                SetTucked(false);
                // Bunbun: brought out from the line, it's only a peek: the island folds
                // back to the line once the pointer leaves it (see below). Tuck away from
                // the menu or the hotkey, and full screen, alike. The menu's "Bring the
                // island back" and the hotkey bring it back for good.
                s_tuckPeek = true;
                s_peekLeftAt = 0.0;
            } else {
                primary.width = 60.0f * g_settings.sizeScale;
                primary.height = 6.0f * g_settings.sizeScale;
                secondary.reset();
                if (g_tuckHoverSince > 0.0 || now - g_tuckedAt < 0.3) needsRender = true;
            }
        } else if (s_tuckPeek) {
            // Bunbun: a peek. 0.4 s after the pointer leaves the island (and no agent is
            // waiting for an answer, nor a notification showing), it folds back to the line.
            const bool waiting = snapshot.agent.phase == bb::AgentPhase::Approval ||
                                 snapshot.agent.phase == bb::AgentPhase::Question ||
                                 (snapshot.notification.active && snapshot.notification.expiresAt > now);
            if (hover || waiting || g_lookupUi.open) {
                s_peekLeftAt = 0.0;
            } else if (s_peekLeftAt <= 0.0) {
                s_peekLeftAt = now;
                needsRender = true;
            } else if (now - s_peekLeftAt > 0.4) {
                SetTucked(true);
                s_tuckPeek = false;
                needsRender = true;
            } else {
                needsRender = true;  // keep ticking until it folds
            }
        }

        const bool mediaWaveformVisible =
            g_settings.media && snapshot.media.playing &&
            ((primary.kind == IslandKind::Media && primary.width > 1.0f && primary.height > 1.0f) ||
             (secondary && secondary->kind == IslandKind::Media && secondary->width > 1.0f && secondary->height > 1.0f));
        g_audioCaptureNeeded.store(mediaWaveformVisible, std::memory_order_relaxed);

        // Bunbun: Bun sits in the collapsed island (beside the clock, or the
        // music pill, headphones on). Notch islands also get a little extra width.
        const bool collapsedLook = primary.width > 1.0f && primary.height > 1.0f &&
                                   primary.height <= 64.0f * g_settings.sizeScale;
        if (collapsedLook && g_settings.notchStyle && !g_tucked.load(std::memory_order_relaxed)) {
            primary.width += kNotchExtraContent * g_settings.sizeScale;
        }
        // Bunbun: Bun sits in the collapsed idle island (beside the clock).
        // On the music pill, the mascot alternates with the cover art in a cycle.
        g_bunSlotWidth = (collapsedLook && !secondary && g_settings.bunbunMascot && g_settings.bunbunMascotOnIsland &&
                          !g_tucked.load(std::memory_order_relaxed) &&
                          primary.kind == IslandKind::Idle &&
                          // Bunbun: not over the game overlay's chips (compact is short enough to
                          // count as collapsed, and Bun was drawn on top of the RAM chip).
                          !(GameOverlayShown(g_settings.gameOverlay) && !isFullscreen))
                             ? kBunSlotContent * g_settings.sizeScale
                             : 0.0f;
        if (g_bunSlotWidth > 0.0f) {
            // Bunbun: a skin pack whose art reaches past its square (music notes,
            // sparks) gets a wider slot, so the clock doesn't overlap it.
            float reachL = 0.5f, reachR = 0.5f;
            bb::mascot::HorizontalReach(&reachL, &reachR);
            g_bunSlotWidth *= Clamp(std::max(reachL, reachR) * 2.0f, 1.0f, 1.6f);
        }
        if (g_bunSlotWidth > 0.0f) {
            float energy = 0.0f;
            for (float b : snapshot.bands) energy += b;
            bb::mascot::SetMusicEnergy(Clamp(energy / kSpectrumBands * 2.2f, 0.0f, 1.0f));
        }

        // Bunbun: Separate displays. What the island looks like at rest, for the other
        // displays' islands (RenderRestIsland): kept while it rests, not while it opens.
        if (collapsedLook && !secondary && !hover && !pinned && !isHoverExpanded &&
            (primary.kind == IslandKind::Idle || primary.kind == IslandKind::Media) && !g_tucked.load(std::memory_order_relaxed)) {
            const float k = std::max(0.01f, g_settings.sizeScale);
            g_restLook.valid = true;
            g_restLook.kind = static_cast<int>(primary.kind);
            g_restLook.width = primary.width / k - (g_settings.notchStyle ? kNotchExtraContent : 0.0f);
            g_restLook.height = primary.height / k;
            g_restLook.bunSlot = g_bunSlotWidth / k;
        }

        float targetWidth = primary.width + g_bunSlotWidth;
        float targetHeight = primary.height;
        if (secondary) {
            targetWidth = primary.width + secondary->width + 12.0f * g_settings.sizeScale;
            targetHeight = std::max(primary.height, secondary->height);
        }
        // Bunbun: hiding keeps the width and only folds the height, so the island retracts up into
        // the top of the screen (and drops down from it when it comes back) instead of shrinking
        // toward the middle of its own length.
        static float s_shownWidth = 120.0f;
        if (hideCollapse) targetWidth = std::max(s_shownWidth, 1.0f);
        else if (targetWidth > 1.0f) s_shownWidth = targetWidth;

        widthSpring.target = targetWidth;
        heightSpring.target = targetHeight;

        if (justUnhidden) {  // the grace period needs no reset: the springs just grow the height from 0
            widthSpring.Reset(targetWidth);
            heightSpring.Reset(targetHeight);
            heightSpring.value = 0.0f;  // Bunbun: grow down from the top of the screen
            nudgeSpring.Reset(0.0f);
        }

        const auto currentFrame = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(currentFrame - previousFrame).count();
        previousFrame = currentFrame;
        dt = Clamp(dt, 0.001f, 0.050f);

        float styleStiffnessMult = 1.0f;
        float styleDampingMult = 1.0f;
        if (g_settings.animationStyle == AnimationStyle::Smooth) {
            styleStiffnessMult = 1.0f;
            styleDampingMult = 1.35f; // Critically damped, no bounciness
        } else if (g_settings.animationStyle == AnimationStyle::Bouncy) {
            styleStiffnessMult = 1.1f;
            styleDampingMult = 0.70f; // Underdamped, lively elasticity
        } else if (g_settings.animationStyle == AnimationStyle::Snappy) {
            styleStiffnessMult = 1.5f;
            styleDampingMult = 1.25f; // High stiffness and quick settle
        }

        const float speed = g_settings.animationSpeed;
        float widthStiffness = 280.0f * styleStiffnessMult;
        float widthDamping = 24.0f * styleDampingMult;
        if (targetWidth > widthSpring.value) {
            widthStiffness = 380.0f * styleStiffnessMult;
            widthDamping = 26.0f * styleDampingMult;
        } else if (targetWidth < widthSpring.value) {
            widthStiffness = 200.0f * styleStiffnessMult;
            widthDamping = 28.0f * styleDampingMult;
        }

        float heightStiffness = 280.0f * styleStiffnessMult;
        float heightDamping = 24.0f * styleDampingMult;
        if (targetHeight > heightSpring.value) {
            heightStiffness = 380.0f * styleStiffnessMult;
            heightDamping = 26.0f * styleDampingMult;
        } else if (targetHeight < heightSpring.value) {
            heightStiffness = 200.0f * styleStiffnessMult;
            heightDamping = 28.0f * styleDampingMult;
        }

        // Bunbun: tucking to the line and back: a stiffer, critically damped spring
        // (about 0.3 s, no overshoot). The usual ones took 0.65-0.9 s, most of it a
        // slow crawl over the last few pixels, which read as lag.
        if (now - g_tuckedAt < 0.6) {
            widthStiffness = heightStiffness = 700.0f * styleStiffnessMult;
            widthDamping = heightDamping = 2.0f * std::sqrt(widthStiffness);  // critical, whatever the style
        }

        widthSpring.Step(dt * speed, widthStiffness, widthDamping);
        if (widthSpring.value < 0.0f) {
            widthSpring.value = 0.0f;
            widthSpring.velocity = 0.0f;
        }

        heightSpring.Step(dt * speed, heightStiffness, heightDamping);
        if (heightSpring.value < 0.0f) {
            heightSpring.value = 0.0f;
            heightSpring.velocity = 0.0f;
        }

        nudgeSpring.Step(dt * speed, 280.0f * styleStiffnessMult, 24.0f * styleDampingMult);

        // Tracks the live level while the flyout is closed, so opening it doesn't sweep from a stale value.
        if (snapshot.volume.active) {
            volumeBarSpring.target = static_cast<float>(snapshot.volume.percent);
        } else {
            volumeBarSpring.Reset(static_cast<float>(snapshot.system.volumePercent));
        }
        // Fixed, overdamped constants: the animation style must not make the bar overshoot.
        volumeBarSpring.Step(dt * speed, 520.0f, 46.0f);
        snapshot.volume.displayPercent = volumeBarSpring.value;

        {
            std::lock_guard lock(g_stateMutex);
            g_state.system.renderFps = ClampInt(static_cast<int>(1.0f / std::max(dt, 0.001f) + 0.5f), 0, 1000);
        }

        const bool draggingOrHover = hover || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && PtInRect(&pillRect, cursor));

        // Ctrl+hover see-through: holding Ctrl while hovering makes the island
        // transparent and click-through in ANY state, so clicks pass through
        // to windows underneath. Releasing Ctrl or moving away restores it.
        const bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool ctrlHoverCT = hover && ctrlHeld && !g_lookupUi.open;

        SetClickThrough(hwnd, (primary.kind == IslandKind::Idle && !draggingOrHover && !pinned) || ctrlHoverCT);

        // Check if animating structurally.
        // Bunbun: only motion that can show. The window lands on whole pixels, so
        // anything under a quarter pixel is invisible; redrawing for it ran the
        // island at the monitor's full refresh rate (about 128 frames a second
        // under an agent pill) with nothing visibly moving.
        constexpr float kVisible = 0.25f;
        if (std::abs(widthSpring.velocity) > kVisible || std::abs(widthSpring.target - widthSpring.value) > kVisible ||
            std::abs(heightSpring.velocity) > kVisible || std::abs(heightSpring.target - heightSpring.value) > kVisible ||
            std::abs(nudgeSpring.velocity) > kVisible || std::abs(nudgeSpring.target - nudgeSpring.value) > kVisible ||
            std::abs(volumeBarSpring.velocity) > kVisible || std::abs(volumeBarSpring.target - volumeBarSpring.value) > kVisible) {
            needsRender = true;
        }

        // Active Monitor Tracking & dynamic DPI scaling
        {
            HMONITOR currentAnchor = nullptr;
            GetAnchorWorkRect(&currentAnchor);
            // Bunbun: the monitor's scale (Match screen size or Auto DPI), so a new
            // monitor, DPI or resolution re-sizes the island.
            bool autoDpi = true, matchScreen = false;
            float displaySize = 1.0f;  // Bunbun: Separate displays: this display's own size
            {
                std::lock_guard lock(g_settingsMutex);
                autoDpi = g_settings.autoDpiScale;
                matchScreen = g_settings.matchScreenSize;
                if (g_settings.displaysAll && g_settings.displaysSeparate) {
                    const int index = DisplayIndexOf(currentAnchor);
                    const bool isMain = g_settings.targetMonitor > 0 ? index == g_settings.targetMonitor
                                                                    : currentAnchor == MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
                    (void)isMain;
                    displaySize = DisplaySize(g_settings, index);  // the main display's own entry too
                }
            }
            const float currentScale = MonitorScale(currentAnchor, autoDpi, matchScreen) * displaySize;
            static HMONITOR s_lastAnchorMon = nullptr;
            static float s_lastScale = 0.0f;
            // Bunbun: also when a settings reload put the size back without this display's factor.
            float applied = 0.0f;
            {
                std::lock_guard lock(g_settingsMutex);
                applied = g_settings.sizeScale / std::max(0.01f, g_settings.baseSizeScale);
            }
            if (currentAnchor != s_lastAnchorMon || currentScale != s_lastScale || std::fabs(applied - currentScale) > 0.001f) {
                s_lastAnchorMon = currentAnchor;
                s_lastScale = currentScale;
                std::lock_guard lock(g_settingsMutex);
                g_settings.sizeScale = g_settings.baseSizeScale * currentScale;
                g_layoutDirty = true;
            }
        }

        // Quick Lookup panel: caret blink, and keeps the published hit-test geometry fresh.
        if (g_lookupUi.open && primary.kind == IslandKind::Lookup) {
            static double s_lastLookupPaint = 0.0;
            if (now - s_lastLookupPaint >= 0.2) {
                s_lastLookupPaint = now;
                needsRender = true;
            }
        }

        // A tab transition keeps painting until its spring settles. It used to
        // request that through g_layoutDirty, which also made Render() run
        // SetWindowPos / backdrop-region upkeep on every frame of the animation.
        if (g_tabAnimating.load(std::memory_order_relaxed)) {
            if (tabBusy) {
                needsRender = true;
            } else {
                // The tab view stopped drawing mid-motion (island collapsed or an
                // alert took over); don't keep the loop spinning for it.
                g_tabAnimating.store(false, std::memory_order_relaxed);
            }
        }

        // Check if layout was explicitly invalidated
        if (g_layoutDirty.load()) {
            needsRender = true;
        }

        // Hover or pinned state changes visual elements slightly
        static bool prevHover = false;
        static bool prevPinned = false;
        if (hover != prevHover || pinned != prevPinned) {
            needsRender = true;
            prevHover = hover;
            prevPinned = pinned;
        }

        // Activities that animate continuously: the waveform bars, the marquee
        // scroll and the battery pulse. These still only want ~60Hz -- they look no
        // different above it, and they should not piggyback on whatever high Target
        // FPS the user picked for structural resize animation.
        //
        // The rate limit lives in the pacer at the bottom of the loop, not here.
        // This used to gate needsRender behind a 1/60s (16.667ms) timer and then
        // fall through to the flat 16ms idle wait, which cannot satisfy it: 16ms is
        // shorter than the gate, so the next pass failed the check and waited a
        // second time. The result was a paint roughly every 32ms -- about 31fps
        // instead of 60, which is what made playing media look choppy.
        // Media only counts as continuous while something on it is actually moving.
        //
        // ChooseActivities selects the Media pill for any *available* SMTC session,
        // playing or not, so keying off the kind alone meant a paused Spotify or a
        // browser tab with a paused video -- which can sit there for hours, since
        // browsers keep the session alive -- held the island at a 60fps render loop
        // and 1ms timer resolution indefinitely. Nothing on it moves in that state:
        // the collapsed pill draws the album art plus a row of flat bars, taking the
        // !playing branch that skips DrawWaveform entirely.
        //
        // Paused media now falls back to the ordinary change detection above (title
        // and art generation, springs, hover), so the timer is released 0.5s after
        // the last paint.
        //
        // The marquees are expanded-only, hence the hover/pinned terms. No need to
        // include recentTrackChange: it already requires snapshot.media.playing, so
        // it cannot be true while paused.
        // Checks the secondary pill too. The island can show two pills side by side,
        // so a playing media pill sitting next to a running timer or a progress ring
        // is the secondary one -- and looking only at primary left its waveform
        // frozen.
        const bool mediaShown = primary.kind == IslandKind::Media ||
                                (secondary && secondary->kind == IslandKind::Media);
        const bool mediaAnimating =
            mediaShown && (snapshot.media.playing || isHoverExpanded || pinned);

        // Transient alert pills. All expire within a few seconds, so treating them as
        // continuous cannot run away.
        //
        // Device, Bluetooth and Do Not Disturb are here only when the countdown bar
        // is switched on, since that bar is the one thing on them that moves.
        // Bunbun: the reminder pill too (it expires within a minute): its bell rings and
        // the water drop bobs.
        const auto isTransientPill = [](IslandKind kind) {
            return kind == IslandKind::BatteryLow || kind == IslandKind::Clipboard ||
                   kind == IslandKind::Notification || kind == IslandKind::Reminder;
        };
        const auto hasCountdownBar = [](IslandKind kind) {
            return kind == IslandKind::Device || kind == IslandKind::Bluetooth ||
                   kind == IslandKind::DoNotDisturb;
        };
        const bool countdownBarsOn = g_settings.statusCountdownProgress;

        // Bunbun: Bun only needs full frame rate for the hop and the alert twitch.
        // A working session's bob, blink and dots read fine at ~12 fps, painted
        // once per bucket like the seconds clock below, so a long agent run costs
        // a fraction of continuous animation.
        // Bunbun: the mascot lives in the Ask Bun box, so it animates whenever the panel is open.
        const bool bunThinking = primary.kind == IslandKind::Lookup &&
                                 ((snapshot.lookup.status == LookupStatus::Loading && snapshot.lookup.source == L"Bun") ||
                                  (g_settings.bunbunAssistant && g_settings.bunbunMascot));
        const bool agentOnScreen = primary.kind == IslandKind::Agent ||
                                   (secondary && secondary->kind == IslandKind::Agent) || bunThinking ||
                                   (g_bunSlotWidth > 0.0f && !isHidden);
        const bool agentFast = agentOnScreen &&
                               bb::mascot::NeedsFastFrames(AgentMood(snapshot.agent.phase), now,
                                                           snapshot.agent.phaseSince);
        if (agentOnScreen && !agentFast) {
            static long long s_agentFrame = -1;
            // The agent pill gets 12 fps; Bun idling beside the clock only breathes and blinks, so 8.
            const bool agentAnim = primary.kind == IslandKind::Agent || (secondary && secondary->kind == IslandKind::Agent) || bunThinking;
            // Bunbun: half that on battery; the motion still reads, at half the wakeups.
            const double bucketFps = (agentAnim ? 12.0 : 8.0) * (g_powerSaving ? 0.5 : 1.0);
            const long long frame = static_cast<long long>(now * bucketFps);
            if (frame != s_agentFrame) {
                s_agentFrame = frame;
                needsRender = true;
            }
        }

        // Bunbun: the gaming mascot in game mode moves at 12 fps (6 on battery): enough for
        // the buttons and the lean, and the game keeps the GPU. (A redraw every frame ran
        // the island at the full refresh rate.)
        if (!isFullscreen && primary.kind == IslandKind::Idle && GameOverlayShown(g_settings.gameOverlay) &&
            g_settings.bunbunMascot && g_settings.bunbunMascotOnIsland && !isHidden) {
            static long long s_gameFrame = -1;
            const long long frame = static_cast<long long>(now * (g_powerSaving ? 6.0 : 12.0));
            if (frame != s_gameFrame) {
                s_gameFrame = frame;
                needsRender = true;
            }
        }

        const bool continuousAnimation =
            mediaAnimating || isTransientPill(primary.kind) ||
            (secondary && isTransientPill(secondary->kind)) ||
            (countdownBarsOn && (hasCountdownBar(primary.kind) ||
                                 (secondary && hasCountdownBar(secondary->kind)))) ||
            agentFast ||
            (!isHidden && !g_tucked.load(std::memory_order_relaxed) && EdgeLight::Lit(g_settings, now));  // Bunbun
        if (continuousAnimation) {
            needsRender = true;
        }

        // Privacy dots
        if (snapshot.system.micActive || snapshot.system.cameraActive) {
            needsRender = true;
        }

        // Idle dashboard clock changes once a minute
        static SYSTEMTIME prevTime = {};
        const bool clockOnScreen =
            primary.kind == IslandKind::Idle ||
            (primary.kind == IslandKind::Media && g_settings.mediaPillClock);
        if (clockOnScreen && !isHidden) {
            SYSTEMTIME local = {};
            GetLocalTime(&local);
            if (local.wMinute != prevTime.wMinute) {
                needsRender = true;
                prevTime = local;
            }
        }

        // Text that ticks once a second: the focus timer countdown and, when Show
        // seconds is on, the clock.
        //
        // Both used to ride on a side effect. The system poll refreshed CPU load
        // every second and the change detection compared it unconditionally, so the
        // whole island repainted about once a second whether anything visible had
        // changed or not. Gating those metric comparisons on visibility removed that,
        // which left DrawTimer recomputing its m:ss from NowSeconds() on a surface
        // nothing marked dirty -- a 25 minute session sat at 25:00 until some
        // unrelated event forced a paint -- and left the seconds clock updating once
        // a minute despite its own setting promising every second.
        //
        // Keyed to the displayed value rather than to elapsed time, so each visible
        // change paints exactly once. Deliberately not folded into
        // continuousAnimation: these need one frame per second, not the 1ms timer
        // resolution that continuous animation asks for.
        int shownSecond = -1;
        const bool timerShown = primary.kind == IslandKind::Timer ||
                               (secondary && secondary->kind == IslandKind::Timer);
        if (timerShown && snapshot.timer.running) {
            shownSecond = static_cast<int>(std::ceil(snapshot.timer.endsAt - now));
        } else if (timerShown && snapshot.stopwatch.running) {  // Bunbun: the stopwatch counts up
            shownSecond = static_cast<int>(now - snapshot.stopwatch.startedAt);
        } else if (clockOnScreen && g_settings.showSeconds && !isHidden) {
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            shownSecond = st.wSecond;
        }
        static int s_prevShownSecond = -1;
        if (shownSecond != s_prevShownSecond) {
            s_prevShownSecond = shownSecond;
            needsRender = true;
        }

        // Compare data snapshot to detect changes
        static uint64_t prevArtGen = 0;
        static uint64_t prevSrcIconGen = 0;
        static uint64_t prevNotifIconGen = 0;
        static uint64_t prevClipIconGen = 0;
        static int prevCpu = -1;
        static int prevRam = -1;
        static int prevDisk = -1;
        static int prevVol = -1;
        static int prevBright = -1;
        static bool prevMuted = false;
        static int prevBat = -1;
        static bool prevCharging = false;
        static int prevProg = -1;
        static std::wstring prevMediaTitle;
        static bool prevPlaying = false;

        // CPU / RAM / disk are compared only while a surface that draws them is on
        // screen. UpdateSystemSnapshot refreshes them every second and CPU load
        // essentially always differs between samples, so comparing them
        // unconditionally repainted the whole island once a second for numbers that
        // appear nowhere on the collapsed pill.
        const bool systemMetricsChanged =
            systemMetricsVisible && (snapshot.system.cpuPercent != prevCpu ||
                                     snapshot.system.memoryPercent != prevRam ||
                                     snapshot.system.diskFreePercent != prevDisk);

        if (snapshot.media.artGeneration != prevArtGen ||
            snapshot.media.sourceIconGeneration != prevSrcIconGen ||
            snapshot.media.title != prevMediaTitle ||
            // Play/pause has to be in here now that the metric tick no longer
            // repaints every second as a side effect. Pausing while collapsed swaps
            // the live waveform for the static bars, and without this the frozen
            // last playing frame stayed up until some unrelated repaint came along.
            snapshot.media.playing != prevPlaying ||
            snapshot.notification.icon->generation != prevNotifIconGen ||
            snapshot.clipboard.appIcon->generation != prevClipIconGen ||
            systemMetricsChanged ||
            snapshot.system.volumePercent != prevVol ||
            snapshot.system.volumeMuted != prevMuted ||
            snapshot.brightness.percent != prevBright ||
            snapshot.battery.percent != prevBat ||
            snapshot.battery.charging != prevCharging ||
            snapshot.progress.percent != prevProg) {
            needsRender = true;
            prevArtGen = snapshot.media.artGeneration;
            prevSrcIconGen = snapshot.media.sourceIconGeneration;
            prevMediaTitle = snapshot.media.title;
            prevPlaying = snapshot.media.playing;
            prevNotifIconGen = snapshot.notification.icon->generation;
            prevClipIconGen = snapshot.clipboard.appIcon->generation;
            prevCpu = snapshot.system.cpuPercent;
            prevRam = snapshot.system.memoryPercent;
            prevDisk = snapshot.system.diskFreePercent;
            prevVol = snapshot.system.volumePercent;
            prevMuted = snapshot.system.volumeMuted;
            prevBright = snapshot.brightness.percent;
            prevBat = snapshot.battery.percent;
            prevCharging = snapshot.battery.charging;
            prevProg = snapshot.progress.percent;
        }

        // Track whether Ctrl+hover click-through state changed so we re-render
        static bool prevCtrlHoverCT = false;
        if (ctrlHoverCT != prevCtrlHoverCT) {
            needsRender = true;
            prevCtrlHoverCT = ctrlHoverCT;
        }

        if (needsRender) {
            // When Ctrl+hover click-through is active, reduce pill opacity so the
            // island becomes visually see-through to match the pass-through behavior.
            Settings renderSettings = GetSettingsCopy();
            if (ctrlHoverCT) {
                renderSettings.pillOpacity = Clamp(renderSettings.pillOpacity * 0.35f, 0.15f, 0.45f);
            } else if (renderSettings.themePreset == ThemePreset::Graphite && g_pillOpacityOverride.load() < 0) {
                const bool isExpanded = isHoverExpanded || pinned || isTransientAlert || (widthSpring.value > 260.0f);
                renderSettings.pillOpacity = isExpanded ? 0.98f : 0.88f;
            }
            // No usable backdrop (old Windows / effects off): keep the solid background.
            if (renderSettings.backdropMaterial != BackdropMaterial::None &&
                !DwmBackdrop::Available()) {
                renderSettings.backdropMaterial = BackdropMaterial::None;
            }
            std::lock_guard frameLock(g_frameMutex);  // Bunbun: see IslandSnapshotPng
            const double renderStart = QpcMs();
            g_restState = &snapshot;  // Bunbun: for the other displays' islands (Separate)
            renderer.Render(snapshot, renderSettings, primary, secondary,
                            widthSpring.value, heightSpring.value, nudgeSpring.value,
                            hover, pinned, now);
            g_renderMsTotal += QpcMs() - renderStart;
            ++g_framesDrawn;  // Bunbun: for the BUNBUN_LOG frame diagnostics below
        }

        // --- Zero-CPU parking for auto-hidden states (idle timeout / fullscreen) ---
        // Only parks once the collapse animation has actually settled at 0,
        // so the shrink still animates before we cut over to OS-hidden.
        const bool wantsAutoHiddenPark =
            !g_manuallyHidden.load() && !pinned && !isHoverExpanded && !isTransientAlert &&
            !privacyActive && (isHidden || fullscreenSuppressed) &&
            heightSpring.value < 0.5f && std::fabs(heightSpring.velocity) < 0.5f;

        if (wantsAutoHiddenPark) {
            parkedForFullscreen = fullscreenSuppressed;
            wasAutoHiddenParked = true;
            g_autoHiddenParked = true;
            ShowWindow(hwnd, SW_HIDE);
            DwmBackdrop::HideAll();
            Mirrors::HideAll();
            g_audioCaptureNeeded.store(false, std::memory_order_relaxed);
            setHighResTimer(false);
            continue;
        }

        int targetFps = g_settings.targetFps;
        if (targetFps <= 0) {
            static int s_cachedRate = 60;
            static double s_rateCheckedAt = -1e9;
            if (now - s_rateCheckedAt > 2.0) {
                s_cachedRate = GetMonitorRefreshRate(hwnd);
                s_rateCheckedAt = now;
            }
            targetFps = s_cachedRate;
        }
        targetFps = ClampInt(targetFps, 30, 1000);
        // Bunbun: on battery, springs don't need a 120-240 Hz panel's full rate.
        UpdatePowerSaving(g_settings.powerSaver, now);
        if (g_powerSaving) targetFps = std::min(targetFps, 60);
        double targetFrameMs = 1000.0 / static_cast<double>(targetFps);

        // Structural animation -- the springs resizing or nudging the island -- is
        // what benefits from a high Target FPS. Continuous content does not, so once
        // the springs have settled the interval is relaxed to 60Hz and the precise
        // pacer below hits it accurately.
        //
        // std::max, not a plain assignment: a user who deliberately set Target FPS
        // to 40 should keep 40 rather than being pushed up to 60.
        const bool springsAnimating =
            std::fabs(widthSpring.value - widthSpring.target) > 0.5f ||
            std::fabs(heightSpring.value - heightSpring.target) > 0.5f ||
            std::fabs(widthSpring.velocity) > 0.5f ||
            std::fabs(heightSpring.velocity) > 0.5f ||
            std::fabs(nudgeSpring.value) > 0.5f ||
            std::fabs(nudgeSpring.velocity) > 0.5f ||
            std::fabs(volumeBarSpring.value - volumeBarSpring.target) > 0.5f ||
            std::fabs(volumeBarSpring.velocity) > 0.5f ||
            g_tabAnimating.load(std::memory_order_relaxed);

        // Bunbun: ambient content (waveform, marquees, alert pulses) runs at
        // Bunbun.AmbientFps, 30 by default. Every frame here is a full CPU
        // composite pushed through UpdateLayeredWindow, so halving the rate
        // roughly halves the cost of having music playing; spring motion above
        // keeps the full refresh rate, so transitions stay fluid.
        if (continuousAnimation && !springsAnimating) {
            const int ambientFps = g_powerSaving ? std::min(g_settings.bunbunAmbientFps, 15) : g_settings.bunbunAmbientFps;
            const double continuousFrameMs = 1000.0 / static_cast<double>(ambientFps);
            targetFrameMs = std::max(targetFrameMs, continuousFrameMs);
        }

        // Keyed to actual animation, not to repainting.
        //
        // A single repaint does not need 1ms pacing -- it needs one frame, which the
        // pacer delivers fine at the default resolution. Only things that draw a
        // *sequence* of frames care: continuous content and spring motion.
        //
        // Keying it to needsRender was wrong twice over. Per frame it thrashed,
        // because needsRender alternates when 60Hz content runs under a higher
        // Target FPS. And with the hold added, any one-off repaint still took the
        // resolution for the full hold -- including the metric tick, which fired
        // every second, so a plain idle island requested and released it once a
        // second and held it roughly half the time with nothing moving at all.
        //
        // continuousAnimation and springsAnimating are exactly the two cases that
        // want precise frame spacing, so they drive it directly.
        if (continuousAnimation || springsAnimating) {
            lastAnimatingAt = now;
        }
        // Bunbun: a 1 ms timer resolution keeps the CPU out of deep idle states. On
        // battery only spring motion gets it; ambient content paces fine without.
        // Bunbun: with BUNBUN_LOG=1, one line a second saying how many frames were
        // drawn and why, so an island that never settles is easy to spot.
        {
            static const bool s_logFrames = GetEnvironmentVariableW(L"BUNBUN_LOG", nullptr, 0) > 0;
            static double s_logAt = 0.0;
            static long long s_lastFrames = 0;
            if (s_logFrames && now - s_logAt >= 1.0) {
                static double s_lastRenderMs = 0.0, s_lastUlwMs = 0.0;
                const long long n = std::max(1LL, g_framesDrawn - s_lastFrames);
                static double s_lastBusy = 0.0;
                Wh_Log(L"  per frame: render %.2f ms (of which UpdateLayeredWindow %.2f ms); loop busy %.1f ms/s",
                       (g_renderMsTotal - s_lastRenderMs) / n, (g_ulwMsTotal - s_lastUlwMs) / n, g_loopBusyMs - s_lastBusy);
                s_lastBusy = g_loopBusyMs;
                s_lastRenderMs = g_renderMsTotal;
                s_lastUlwMs = g_ulwMsTotal;
                Wh_Log(L"frames %lld/s render=%d continuous=%d springs=%d (w %.2f->%.2f v%.2f, h %.2f->%.2f v%.2f, nudge %.2f, tab %d) kind=%d saver=%d",
                       g_framesDrawn - s_lastFrames, needsRender ? 1 : 0, continuousAnimation ? 1 : 0, springsAnimating ? 1 : 0,
                       widthSpring.value, widthSpring.target, widthSpring.velocity, heightSpring.value, heightSpring.target,
                       heightSpring.velocity, nudgeSpring.value, g_tabAnimating.load() ? 1 : 0, static_cast<int>(primary.kind),
                       g_powerSaving ? 1 : 0);
                s_logAt = now;
                s_lastFrames = g_framesDrawn;
            }
        }
        if (g_powerSaving) {
            setHighResTimer(springsAnimating);
        } else {
            setHighResTimer(lastAnimatingAt >= 0.0 && now - lastAnimatingAt < kHighResTimerHoldSec);
        }

        g_loopBusyMs += QpcMs() - iterStart;  // Bunbun: diagnostics
        if (!needsRender) {
            // Nothing changed on screen, so poll at ~60Hz rather than the target
            // frame rate. Accuracy does not matter here -- this is a poll interval,
            // not frame pacing -- so it runs at whatever resolution is in effect.
            //
            // Bunbun: each poll copies SharedState and recomputes the layout, which
            // at 60Hz was the bulk of an idle island's CPU. Poll fast only while
            // the cursor is close enough to hover soon; otherwise 20Hz. Any posted
            // event (a nudge, an agent update, mouse input on the island) still
            // wakes the wait at once.
            RECT nearRect = pillRect;
            if (widthSpring.value <= 1.0f || heightSpring.value <= 1.0f) nearRect = GetIslandDockRect();
            InflateRect(&nearRect, 160, 120);
            const bool cursorNear = hover || PtInRect(&nearRect, cursor) != FALSE;
            // Bunbun: on battery the far-cursor wait is 250 ms (4 wakeups/s instead of 20).
            const DWORD idleWait = cursorNear ? (g_powerSaving ? 33u : 16u) : (g_powerSaving ? 250u : 50u);
            MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, idleWait, QS_ALLINPUT);
            nextFrameTarget = std::chrono::steady_clock::now();
        } else if (springsAnimating && g_settings.targetFps <= 0 && SUCCEEDED(DwmFlush())) {
            // Auto FPS while the island is resizing: block until DWM's next
            // composition so every frame lands exactly on a vsync. A timer drifts
            // against the display's refresh and produces judder even at high fps.
            nextFrameTarget = std::chrono::steady_clock::now();
        } else {
            // When animating, achieve ultra-smooth target refresh rate (e.g. 144Hz, 240Hz, 360Hz+).
            nextFrameTarget += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(targetFrameMs));

            auto nowTime = std::chrono::steady_clock::now();
            if (nowTime < nextFrameTarget) {
                double remainingMs = std::chrono::duration<double, std::milli>(nextFrameTarget - nowTime).count();
                if (remainingMs >= 1.5) {
                    // Sleep for the bulk of the remaining time using OS event wait (zero CPU usage)
                    WaitForSingleObject(g_stopEvent, static_cast<DWORD>(remainingMs - 0.5));
                }
                // Spinning only pays off for fast spring motion on high refresh rates.
                // For 60Hz content a frame landing under 1ms late is invisible, so sleep only.
                if (springsAnimating && targetFrameMs < 10.0) {
                    while (std::chrono::steady_clock::now() < nextFrameTarget &&
                           WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
                        std::this_thread::yield();
                    }
                }
            } else {
                // If we fell behind, reset target to avoid speed-up catch-up loop
                nextFrameTarget = nowTime;
            }
        }
    }

    // Balances whichever state the loop exited in; a no-op if already released.
    setHighResTimer(false);

    UnbindVolumeEndpoint();
    DwmBackdrop::DestroyAll();
    renderer.Shutdown();
    DestroyWindow(hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(kWindowClass, wc.hInstance);

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return 0;
}



bool StartThreads() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_settingsChangedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_mediaRefreshEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_settingsChangedEvent) {
        return false;
    }

    g_running = true;
    g_renderThread = CreateThread(nullptr, 0, RenderThreadProc, nullptr, 0, nullptr);
    if (!g_renderThread) {
        return false;
    }

    g_mediaThread = CreateThread(nullptr, 0, MediaThreadProc, nullptr, 0, nullptr);
    g_audioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
    g_weatherThread = CreateThread(nullptr, 0, WeatherThreadProc, nullptr, 0, nullptr);
    g_lyricsThread = CreateThread(nullptr, 0, LyricsThreadProc, nullptr, 0, nullptr);
    g_keyboardThread = CreateThread(nullptr, 0, KeyboardThreadProc, nullptr, 0, &g_keyboardThreadId);
    g_mouseThread = CreateThread(nullptr, 0, MouseThreadProc, nullptr, 0, &g_mouseThreadId);
#if DYNAMIC_ISLAND_HAS_USER_NOTIFICATION_LISTENER
    g_notificationThread = CreateThread(nullptr, 0, NotificationThreadProc, nullptr, 0, nullptr);
#endif
    g_bluetoothThread = CreateThread(nullptr, 0, BluetoothThreadProc, nullptr, 0, nullptr);
    // Bunbun: Wi-Fi / Bluetooth status sampler for the Connectivity tab.
    g_connectivityWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_connectivityThread = CreateThread(nullptr, 0, ConnectivityThreadProc, nullptr, 0, nullptr);
    g_calendarThread = CreateThread(nullptr, 0, CalendarThreadProc, nullptr, 0, nullptr);    // Bunbun
    g_downloadsThread = CreateThread(nullptr, 0, DownloadsThreadProc, nullptr, 0, nullptr);  // Bunbun
    g_privacyThread = CreateThread(nullptr, 0, PrivacyThreadProc, nullptr, 0, nullptr);      // Bunbun
    std::thread([] { bb::assistant::WarmStartMenuIndex(); }).detach();                        // Bunbun: instant "open …"
    g_todoWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);                                // Bunbun
    g_todoThread = CreateThread(nullptr, 0, TodoThreadProc, nullptr, 0, nullptr);
    g_waterWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);                               // Bunbun
    g_waterThread = CreateThread(nullptr, 0, WaterThreadProc, nullptr, 0, nullptr);           // Bunbun
    g_workWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);                                // Bunbun: Work mode
    g_workThread = CreateThread(nullptr, 0, WorkThreadProc, nullptr, 0, nullptr);
    SubscribeDndNotification();

    return true;
}

