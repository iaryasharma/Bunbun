// Bunbun — Studio layout drawing. #included inside the Renderer class.
//
// The Studio frame wraps the dashboard (idle and media views) once the island is
// open: a header with the page name, a tiny mascot, the date and time, and a
// bottom tab bar with one icon per page and a volume slider. The page itself is
// drawn by its usual function into the rect between the two, so every existing
// page and its hit tests keep working. Also here: the Home tiles, the Agent,
// Clipboard and Note pages (shared by both layouts) and the tuck-away line.
// State and click handling live in studio_module.inc.

    // ── Metrics (content units; DrawPill applies sizeScale) ──
    static constexpr float kStudioIslandHeader = 34.0f;
    static constexpr float kStudioFooter = 38.0f;
    static constexpr float kStudioSide = 24.0f;
    static constexpr float kStudioPageH = 184.0f;  // the dashboard pages' own height

    // ── Fonts ──
    enum StudioFontId { SFTitle, SFLabel, SFBody, SFBig, SFHero, SFMono, SFSmall, SFSong, SFAnswer, SFAsk, SFInput, SFCount };
    ComPtr<IDWriteTextFormat> studioFonts_[SFCount];
    std::wstring studioFontFamily_ = L"\x01";  // never a real family, so the first call builds them

    IDWriteTextFormat* SFont(StudioFontId id) {
        if (studioFontFamily_ != lastFontFamily_) {
            studioFontFamily_ = lastFontFamily_;
            const wchar_t* family = lastFontFamily_.empty() ? L"Segoe UI Variable Display" : lastFontFamily_.c_str();
            struct Spec { const wchar_t* family; DWRITE_FONT_WEIGHT weight; float size; };
            const Spec specs[SFCount] = {
                {family, DWRITE_FONT_WEIGHT_BOLD, 13.0f},       // SFTitle
                {family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f},  // SFLabel
                {family, DWRITE_FONT_WEIGHT_MEDIUM, 12.0f},     // SFBody
                {family, DWRITE_FONT_WEIGHT_BOLD, 26.0f},       // SFBig
                {family, DWRITE_FONT_WEIGHT_BOLD, 40.0f},       // SFHero
                {L"Cascadia Mono", DWRITE_FONT_WEIGHT_NORMAL, 11.0f},  // SFMono
                {family, DWRITE_FONT_WEIGHT_MEDIUM, 10.0f},     // SFSmall
                {family, DWRITE_FONT_WEIGHT_BOLD, 15.0f},       // SFSong
                {family, DWRITE_FONT_WEIGHT_NORMAL, 16.0f},     // SFAnswer (wraps)
                {family, DWRITE_FONT_WEIGHT_MEDIUM, 13.0f},     // SFAsk (wraps)
                {family, DWRITE_FONT_WEIGHT_NORMAL, 14.0f},     // SFInput
            };
            for (int i = 0; i < SFCount; ++i) {
                studioFonts_[i] = nullptr;
                HRESULT hr = dwriteFactory_->CreateTextFormat(specs[i].family, nullptr, specs[i].weight,
                                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                              specs[i].size, L"", &studioFonts_[i]);
                if (FAILED(hr) || !studioFonts_[i]) {
                    dwriteFactory_->CreateTextFormat(i == SFMono ? L"Consolas" : L"Segoe UI", nullptr, specs[i].weight,
                                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                     specs[i].size, L"", &studioFonts_[i]);
                }
                if (!studioFonts_[i]) continue;
                const bool wraps = i == SFAnswer || i == SFAsk;
                studioFonts_[i]->SetWordWrapping(wraps ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
                studioFonts_[i]->SetParagraphAlignment(wraps ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                if (wraps) continue;  // no ellipsis: wrapped text is measured and shown whole
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                ComPtr<IDWriteInlineObject> ellipsis;
                dwriteFactory_->CreateEllipsisTrimmingSign(studioFonts_[i].Get(), &ellipsis);
                studioFonts_[i]->SetTrimming(&trim, ellipsis.Get());
            }
        }
        return studioFonts_[id].Get();
    }

    // align: 0 left, 1 centre, 2 right. Text is vertically centred in r.
    void SText(const std::wstring& text, D2D1_RECT_F r, StudioFontId font, D2D1_COLOR_F color, int align = 0) {
        IDWriteTextFormat* f = SFont(font);
        if (!f || text.empty() || r.right <= r.left) return;
        f->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER
                            : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        ID2D1SolidColorBrush* b = AcquireBrush(color);
        if (!b) return;
        target_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), f, r, b,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    float SMeasure(const std::wstring& text, StudioFontId font) {
        IDWriteTextFormat* f = SFont(font);
        if (!f || text.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), f, 2000.0f,
                                                    100.0f, &layout)) || !layout)
            return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    // ── Small drawing helpers ──
    float studioAlpha_ = 1.0f;          // the frame's fade while the island opens
    D2D1_POINT_2F studioOrigin_{};      // published content rect's top-left, for hit rects

    static D2D1_COLOR_F SCol(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }

    // Bunbun: Studio on a light island (Porcelain, a light Custom or solid fill). The
    // pages are drawn for a dark island: white text, grey labels, near-black cards. On
    // a light one every neutral colour is turned around against the theme (white text
    // becomes the theme's text, black cards light ones, greys in between); the accent
    // and status colours, and the theme's own colours, stay as they are. AcquireBrush
    // applies it while a Studio frame is drawn.
    bool studioInk_ = false;
    D2D1_COLOR_F inkText_{}, inkCard_{}, inkKeep1_{}, inkKeep2_{}, inkKeep3_{};
    static bool SameCol(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
        // Near-exact: Porcelain's text (#14161A) is only 4/255 from the cards' #18181A.
        return std::fabs(a.r - b.r) < 0.004f && std::fabs(a.g - b.g) < 0.004f && std::fabs(a.b - b.b) < 0.004f;
    }
    D2D1_COLOR_F StudioInk(D2D1_COLOR_F c) const {
        const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
        if (mx - mn > 0.08f) return c;  // a colour, not a neutral
        if (SameCol(c, inkKeep1_) || SameCol(c, inkKeep2_) || SameCol(c, inkKeep3_)) return c;
        const float lum = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        return D2D1::ColorF(SLerp(inkCard_.r, inkText_.r, lum), SLerp(inkCard_.g, inkText_.g, lum),
                            SLerp(inkCard_.b, inkText_.b, lum), c.a);
    }
    D2D1_COLOR_F SFade(D2D1_COLOR_F c) const { c.a *= studioAlpha_; return c; }

    // Records a clickable spot in window (client) coordinates, through the transform
    // in force right now: the island's size scale, a page sliding in, a nudge. The
    // pointer is matched against these directly, so nothing else drawn later in the
    // frame (an agent pill publishing its own content rect) can shift them.
    void SHit(D2D1_RECT_F r, StudioAction action, int arg = 0) {
        D2D1_MATRIX_3X2_F m;
        target_->GetTransform(&m);
        // The live transform maps into the region bound this frame, not the window;
        // bindOffset_ turns that back into window (client) coordinates.
        auto map = [&](float x, float y) {
            return D2D1::Point2F(x * m._11 + y * m._21 + m._31 + bindOffset_.x,
                                 x * m._12 + y * m._22 + m._32 + bindOffset_.y);
        };
        const D2D1_POINT_2F a = map(r.left, r.top), b = map(r.right, r.bottom);
        g_studioHits.push_back({D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)),
                                action, arg});
        g_studioHitsFrame = GetTickCount64();
    }

    // Hover as an eased 0..1 per button (about 80 ms each way), so highlights fade
    // in and out instead of switching. Keeps frames coming while one is moving.
    struct HoverAnim { float value = 0.0f; double at = 0.0; };
    std::map<std::pair<int, int>, HoverAnim> hoverAnim_;

    float SHover(StudioAction action, int arg = INT_MIN) {
        HoverAnim& h = hoverAnim_[{static_cast<int>(action), arg}];
        const double now = NowSeconds();
        const float dt = h.at > 0.0 ? static_cast<float>(std::min(0.1, now - h.at)) : 0.1f;
        h.at = now;
        const float target = SHovered(action, arg) ? 1.0f : 0.0f;
        h.value += (target - h.value) * (1.0f - std::exp(-dt / 0.08f));
        if (std::fabs(target - h.value) < 0.01f) h.value = target;
        else g_layoutDirty = true;
        return h.value;
    }

    static float SLerp(float a, float b, float t) { return a + (b - a) * t; }
    static D2D1_COLOR_F SLerpCol(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
        return D2D1::ColorF(SLerp(a.r, b.r, t), SLerp(a.g, b.g, t), SLerp(a.b, b.b, t), SLerp(a.a, b.a, t));
    }

    bool SHovered(StudioAction action, int arg = INT_MIN) const {
        return g_studioHover.on && g_studioHover.action == action && (arg == INT_MIN || g_studioHover.arg == arg);
    }

    void SFillRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
        if (ID2D1SolidColorBrush* b = AcquireBrush(c))
            target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
    }

    void SLine(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float w) {
        if (ID2D1SolidColorBrush* br = AcquireBrush(c)) target_->DrawLine(a, b, br, w, studioStroke_.Get());
    }

    ComPtr<ID2D1StrokeStyle> studioStroke_;
    void EnsureRoundStroke() {
        if (studioStroke_) return;
        D2D1_STROKE_STYLE_PROPERTIES p = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        d2dFactory_->CreateStrokeStyle(p, nullptr, 0, &studioStroke_);
    }

    // The icons and rings are shared with the Linux island (src/island/studio_icons.h).
    bb::studio::Pen SPen() {
        EnsureRoundStroke();
        return {target_.Get(), d2dFactory_.Get(), studioStroke_.Get(),
                [this](const D2D1_COLOR_F& c) { return AcquireBrush(c); }};
    }

    // Open or closed polyline, in a 24-unit icon grid placed at `c` with `scale`.
    void SPoly(std::initializer_list<D2D1_POINT_2F> pts, D2D1_POINT_2F c, float scale, D2D1_COLOR_F col,
               float width, bool closed = false, bool fill = false) {
        bb::studio::Poly(SPen(), pts, c, scale, col, width, closed, fill);
    }

    // An arc around (cx, cy) from angle a0 to a1 (radians, 0 = 3 o'clock, clockwise).
    void SArc(D2D1_POINT_2F c, float r, float a0, float a1, D2D1_COLOR_F col, float width) {
        bb::studio::Arc(SPen(), c, r, a0, a1, col, width);
    }

    // Progress ring from 12 o'clock: track plus a coloured arc.
    void SRing(D2D1_POINT_2F c, float r, float width, float fraction, D2D1_COLOR_F col) {
        bb::studio::Ring(SPen(), c, r, width, fraction, col, studioAlpha_);
    }

    void SIcon(StudioPage page, D2D1_POINT_2F c, float size, D2D1_COLOR_F col) { bb::studio::Icon(SPen(), page, c, size, col); }
    void SGear(D2D1_POINT_2F c, float size, D2D1_COLOR_F col) { bb::studio::Gear(SPen(), c, size, col); }
    void SEyedropper(D2D1_POINT_2F c, float size, D2D1_COLOR_F col) { bb::studio::Eyedropper(SPen(), c, size, col); }
    void SSpeaker(D2D1_POINT_2F c, float size, int level, bool muted, D2D1_COLOR_F col) {
        bb::studio::Speaker(SPen(), c, size, level, muted, col);
    }

    // ── Page names and colours ──
    static void StudioName(StudioPage page, const bb::AgentView& agent, std::wstring* first, std::wstring* second) {
        bb::studio::PageName(page, agent.agent, first, second);
    }

    static const wchar_t* StudioTabLabel(StudioPage page) { return bb::studio::TabLabel(page); }

    // Each coding agent keeps its own brand colour; everything else on the island
    // follows the dynamic accent (SAccent).
    static D2D1_COLOR_F AgentBrand(const std::wstring& agent) { return SCol(bb::studio::AgentBrand(agent)); }

    // The island's live accent: album art in Auto mode, else the system or custom
    // accent (Themes.AccentColorMode), already eased between changes.
    D2D1_COLOR_F SAccent() const {
        D2D1_COLOR_F c = currentAccent_;
        if (c.a <= 0.0f) c = SCol(0xff375f);
        c.a = 1.0f;
        return c;
    }

    static D2D1_COLOR_F AgentStatusColor(bb::AgentPhase p) { return SCol(bb::studio::AgentStatus(p)); }

    D2D1_COLOR_F StudioPageColor(StudioPage page, const SharedState& state) const {
        if (page == StudioPage::Agent) return AgentBrand(state.agent.agent);
        return SAccent();
    }

    static std::wstring Clock(double seconds) {
        const int s = std::max(0, static_cast<int>(seconds));
        wchar_t b[32];
        if (s >= 3600) swprintf_s(b, L"%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
        else swprintf_s(b, L"%d:%02d", s / 60, s % 60);
        return b;
    }

    static const wchar_t* StageWord(int stage) {
        switch (stage) {
            case 0: return L"Reading";
            case 1: return L"Editing";
            case 2: return L"Testing";
            case 3: return L"Done";
            default: return L"Thinking";
        }
    }

    // Whose usage the header ring and the Agent tile show: the main agent's, when it
    // has numbers (Claude Code's statusLine, Antigravity's cached quota) and is
    // switched on. On Auto, whichever of the two has numbers. False = no ring.
    bool PickUsage(const SharedState& state, const Settings& settings, double now, float* usedPct, const wchar_t** tag) {
        const int main = MainAgentKind(settings);
        const long long unixNow = static_cast<long long>(time(nullptr));
        const bool claude = settings.usageLimits && settings.showAgent[AgentClaude] && state.agent.usage5h >= 0.0f &&
                            now - state.agent.usageAt < 5 * 3600.0;
        const AgQuota::Snapshot ag = settings.antigravityQuota && settings.showAgent[AgentAntigravity]
                                         ? AgQuota::Get() : AgQuota::Snapshot{};
        const bool auto_ = settings.mainAgent < 0;
        if (claude && (main == AgentClaude || (auto_ && !(main == AgentAntigravity && ag.valid)))) {
            // Bunbun: past its reset time the 5-hour window is empty again.
            *usedPct = bb::limits::UsedNow(state.agent.usage5h, state.agent.usage5hResets, unixNow);
            *tag = L"5h";
            return true;
        }
        if (ag.valid && (main == AgentAntigravity || auto_)) {
            *usedPct = AgQuota::UsedNow(ag);  // the most used pool, refilled pools as empty
            *tag = L"AG";
            return true;
        }
        return false;
    }

    // ── Frame ──
    struct StudioFrame {
        bool on = false;
        float mix = 0.0f;        // 0 collapsed .. 1 fully open
        D2D1_RECT_F island{};    // the whole island (content units)
        D2D1_RECT_F page{};      // where the page is drawn
        int context = 1;
        std::vector<int> tabs;
        int tabIdx = 0;
        StudioPage page_ = StudioPage::Home;
    };
    StudioFrame studio_;
    std::vector<float> studioTabW_;  // eased tab widths, one per tab

    // Called by DrawPill before the page is drawn. Returns the rect the page gets:
    // the island itself while collapsed, the space between header and tab bar when open.
    D2D1_RECT_F BeginStudioFrame(const SharedState& state, const Settings& settings, IslandKind kind,
                                 D2D1_RECT_F island, double now) {
        studio_ = StudioFrame{};
        studioInk_ = false;
        if (!settings.studioLayout || (kind != IslandKind::Media && kind != IslandKind::Idle) ||
            GameOverlayShown(settings.gameOverlay)) {
            return island;
        }
        const float h = island.bottom - island.top;
        const float mix = SmoothStep01(Clamp((h - 120.0f) / (kStudioIslandH - 120.0f), 0.0f, 1.0f));
        if (mix <= 0.001f) return island;

        studio_.on = true;
        studio_.mix = mix;
        {
            const D2D1_COLOR_F bg = settings.pillBgColor;
            studioInk_ = 0.2126f * bg.r + 0.7152f * bg.g + 0.0722f * bg.b > 0.55f;
            inkText_ = settings.textPrimaryColor;
            inkCard_ = D2D1::ColorF(bg.r * 0.93f, bg.g * 0.93f, bg.b * 0.93f, 1.0f);
            inkKeep1_ = settings.textPrimaryColor;
            inkKeep2_ = settings.textSecondaryColor;
            inkKeep3_ = bg;
            if (0.2126f * inkText_.r + 0.7152f * inkText_.g + 0.0722f * inkText_.b > 0.5f)
                inkText_ = D2D1::ColorF(0.08f, 0.09f, 0.10f, 1.0f);  // a light Custom fill with white text set
        }
        studio_.island = island;
        studio_.context = kind == IslandKind::Media ? 0 : 1;
        studio_.tabs = DashboardTabIds(studio_.context, settings);
        studio_.tabIdx = studio_.tabs.empty() ? 0
            : std::min(NormalizedTabIndex(settings), static_cast<int>(studio_.tabs.size()) - 1);
        studio_.page_ = studio_.tabs.empty() ? StudioPage::Home
            : StudioPageFor(studio_.context, studio_.tabs[static_cast<size_t>(studio_.tabIdx)]);
        studio_.page = D2D1::RectF(island.left + kStudioSide * mix, island.top + kStudioIslandHeader * mix,
                                   island.right - kStudioSide * mix, island.bottom - kStudioFooter * mix);

        // Music pages glow from the bottom in the album's colour; Home and the
        // Agent page in the agent's status colour while it works.
        D2D1_COLOR_F tint{};
        float strength = 0.0f;
        if (studio_.page_ == StudioPage::Music || studio_.page_ == StudioPage::Lyrics) {
            tint = StudioPageColor(studio_.page_, state);
            strength = state.media.playing ? 0.55f : 0.30f;
        } else if (studio_.page_ == StudioPage::Agent) {
            if (state.agent.phase == bb::AgentPhase::Error) {
                tint = SCol(0xff453a);
            } else if (state.agent.phase == bb::AgentPhase::Finished) {
                tint = SCol(0x30d158);
            } else {
                tint = AgentBrand(state.agent.agent);
            }
            strength = (state.agent.phase != bb::AgentPhase::None) ? 0.40f : 0.25f;
        } else if (studio_.page_ == StudioPage::Clipboard) {
            // Bunbun: not System, Weather or Screen Time: their see-through cards sit on the
            // glow, which washed out each card's bottom corners against its crisp top.
            tint = SAccent();
            strength = 0.28f;
        }
        if (strength > 0.0f) {
            const float radius = ContentIslandRadius(h);
            ComPtr<ID2D1Geometry> mask = CreateIslandMaskGeometry(island, radius, settings.notchStyle);
            D2D1_GRADIENT_STOP stops[3] = {
                {0.0f, WithAlpha(tint, 0.0f)},
                {0.55f, WithAlpha(tint, 0.10f * strength * mix)},
                {1.0f, WithAlpha(tint, 0.62f * strength * mix)},
            };
            ComPtr<ID2D1GradientStopCollection> coll;
            ComPtr<ID2D1LinearGradientBrush> grad;
            if (mask && SUCCEEDED(target_->CreateGradientStopCollection(stops, 3, &coll)) &&
                SUCCEEDED(target_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, island.top + h * 0.2f),
                                                        D2D1::Point2F(0, island.bottom)),
                    coll.Get(), &grad))) {
                target_->FillGeometry(mask.Get(), grad.Get());
            }
        }
        (void)now;
        return studio_.page;
    }

    // Called by DrawPill after the page: header and tab bar over the frame.
    void EndStudioFrame(const SharedState& state, const Settings& settings, double now) {
        if (!studio_.on) return;
        EnsureRoundStroke();
        studioAlpha_ = studio_.mix * studio_.mix;
        studioOrigin_ = publishedOrigin_;
        const D2D1_RECT_F isl = studio_.island;
        const float cx = (isl.left + isl.right) * 0.5f;

        // Header ─ mascot and page name on the left; date, time and controls on the right.
        const float hy = isl.top + 17.0f;
        std::wstring first, second;
        StudioName(studio_.page_, state.agent, &first, &second);
        if (studio_.page_ == StudioPage::Work && state.work.study) { first = L"Study "; second = L"Time"; }  // Bunbun
        const D2D1_COLOR_F pageCol = StudioPageColor(studio_.page_, state);

        // Bunbun: header, left to right: mascot, page name (and Weather's controls);
        // right to left: close, gear, eyedropper, battery, agent usage, then the
        // date and time, which give way (date first) when the left side needs room.

        // ── Header Left: mascot, then the page name ──
        float fx = isl.left + 18.0f;
        if (settings.bunbunMascot && studioAlpha_ > 0.3f) {
            auto mood = state.agent.phase != bb::AgentPhase::None ? AgentMood(state.agent.phase)
                        : state.media.playing ? MediaMascotMood(state.media, settings) : bb::mascot::Mood::Idle;
            double moodSince = state.agent.phaseSince;
            if (state.agent.phase == bb::AgentPhase::None) WorkMood(state.work, &mood);  // Bunbun: Work mode
            AwayMood(settings, state, &mood);  // Bunbun: away or asleep over the rest
            if (WaterMood(now, &mood, &moodSince)) g_layoutDirty = true;  // Bunbun: drinking a logged glass, the goal's cheer
            if (settings.idleEyes) {
                DrawIdleEyes(state, D2D1::Point2F(fx + 9.0f, hy), 14.0f, mood, now, state.agent.phaseSince);
            } else {
                D2D1_POINT_2F mc{};
                float msize = 0.0f;
                bb::mascot::FitToBox(D2D1::RectF(fx, hy - 9.0f, fx + 18.0f, hy + 9.0f), &mc, &msize);
                bb::mascot::Draw(target_.Get(), mc, msize, mood, now, moodSince, MascotLook(mc));
            }
            fx += 26.0f;
        }

        // System, Weather and Screen Time read "Name | detail"; every other page
        // keeps its two-tone name ("Now" white, "Playing" in the page colour).
        bool subtitled = true;
        if (studio_.page_ == StudioPage::Hardware) {
            first = L"System";
            second = L"Power \x00B7 Network \x00B7 Audio";
        } else if (studio_.page_ == StudioPage::Weather) {
            first = L"Weather";
            second = state.weather.city.empty() ? L"Jaipur" : state.weather.city;
        } else if (studio_.page_ == StudioPage::ScreenTime) {
            first = L"Screen Time";
            second.clear();
        } else {
            subtitled = false;
        }
        const float w1 = SMeasure(first, SFTitle);
        SText(first, D2D1::RectF(fx, hy - 10, fx + w1 + 2, hy + 10), SFTitle, SFade(SCol(0xffffff)));
        fx += w1;
        if (!second.empty()) {
            if (subtitled) {
                const float barX = fx + 8.0f;
                SLine(D2D1::Point2F(barX, hy - 6.5f), D2D1::Point2F(barX, hy + 6.5f), SFade(SCol(0x3a3a3c)), 1.2f);
                const float w2 = SMeasure(second, SFBody);
                SText(second, D2D1::RectF(barX + 9.0f, hy - 9, barX + 11.0f + w2, hy + 9), SFBody, SFade(SCol(0x8e8e93)));
                fx = barX + 9.0f + w2;
            } else {
                const float w2 = SMeasure(second, SFTitle);
                SText(second, D2D1::RectF(fx, hy - 10, fx + w2 + 2, hy + 10), SFTitle, SFade(pageCol));
                fx += w2;
            }
        }

        // Weather's own controls follow its name: a °C / °F capsule and Change City.
        if (studio_.page_ == StudioPage::Weather) {
            const D2D1_RECT_F togRect = D2D1::RectF(fx + 12.0f, hy - 11.0f, fx + 68.0f, hy + 11.0f);
            SFillRound(togRect, 11.0f, SCol(0x242426));
            const bool f = settings.weatherFahrenheit;
            const D2D1_RECT_F cHalf = D2D1::RectF(togRect.left + 2.0f, togRect.top, togRect.left + 27.0f, togRect.bottom);
            const D2D1_RECT_F fHalf = D2D1::RectF(togRect.left + 29.0f, togRect.top, togRect.right - 2.0f, togRect.bottom);
            const D2D1_RECT_F on = f ? fHalf : cHalf;
            SFillRound(D2D1::RectF(on.left, on.top + 2.0f, on.right, on.bottom - 2.0f), 9.0f, SCol(0x3a3a3c));
            SText(L"\x00B0" L"C", cHalf, SFSmall, SCol(f ? 0x8e8e93 : 0xffffff), 1);
            SText(L"\x00B0" L"F", fHalf, SFSmall, SCol(f ? 0xffffff : 0x8e8e93), 1);
            SHit(togRect, StudioAction::WeatherUnitToggle);

            const D2D1_RECT_F cityBtn = D2D1::RectF(togRect.right + 6.0f, hy - 11.0f, togRect.right + 96.0f, hy + 11.0f);
            const float cHover = SHover(StudioAction::WeatherCitySearch);
            SFillRound(cityBtn, 11.0f, SLerpCol(SCol(0x242426), SCol(0x3a3a3c), cHover));
            SText(L"\x2316 Change City", cityBtn, SFSmall, SCol(0xffffff), 1);
            SHit(cityBtn, StudioAction::WeatherCitySearch);
            fx = cityBtn.right;
        }
        const float leftEnd = fx + 14.0f;   // the right group stops here

        // ── Header Right Controls: [date time]  [AG Quota]  [100% 🔋]  [✎]  [⚙]  [✕] ──
        // 1. Close button (✕)
        const D2D1_POINT_2F closePt = D2D1::Point2F(isl.right - 22.0f, hy);
        const float closeHover = SHover(StudioAction::Close);
        if (closeHover > 0.01f)
            SFillRound(D2D1::RectF(closePt.x - 11, closePt.y - 11, closePt.x + 11, closePt.y + 11), 11, SFade(WithAlpha(SCol(0x3a3a3c), closeHover)));
        const D2D1_COLOR_F closeCol = SFade(SLerpCol(SCol(0x8e8e93), SCol(0xffffff), closeHover));
        SLine(D2D1::Point2F(closePt.x - 4.5f, closePt.y - 4.5f), D2D1::Point2F(closePt.x + 4.5f, closePt.y + 4.5f), closeCol, 1.4f);
        SLine(D2D1::Point2F(closePt.x + 4.5f, closePt.y - 4.5f), D2D1::Point2F(closePt.x - 4.5f, closePt.y + 4.5f), closeCol, 1.4f);
        SHit(D2D1::RectF(closePt.x - 12, closePt.y - 12, closePt.x + 12, closePt.y + 12), StudioAction::Close);

        // 2. Settings gear (⚙)
        const D2D1_POINT_2F gear = D2D1::Point2F(closePt.x - 24.0f, hy);
        const float gearHover = SHover(StudioAction::Settings);
        if (gearHover > 0.01f)
            SFillRound(D2D1::RectF(gear.x - 12, gear.y - 12, gear.x + 12, gear.y + 12), 12, SFade(WithAlpha(SCol(0x3a3a3c), gearHover)));
        SGear(gear, 14.0f * (1.0f + 0.1f * gearHover), SFade(SLerpCol(SCol(0x8e8e93), SCol(0xffffff), gearHover)));
        SHit(D2D1::RectF(gear.x - 12, gear.y - 12, gear.x + 12, gear.y + 12), StudioAction::Settings);

        // 3. Eyedropper (✎)
        const D2D1_POINT_2F drop = D2D1::Point2F(gear.x - 24.0f, hy);
        const float dropHover = SHover(StudioAction::ColorPicker);
        if (dropHover > 0.01f)
            SFillRound(D2D1::RectF(drop.x - 12, drop.y - 12, drop.x + 12, drop.y + 12), 12, SFade(WithAlpha(SCol(0x3a3a3c), dropHover)));
        SEyedropper(drop, 14.0f * (1.0f + 0.1f * dropHover), SFade(SLerpCol(SCol(0x8e8e93), SCol(0xffffff), dropHover)));
        SHit(D2D1::RectF(drop.x - 12, drop.y - 12, drop.x + 12, drop.y + 12), StudioAction::ColorPicker);

        // 4. Battery indicator in header (like Vayli: 100% 🔋)
        const int hBatt = state.connectivity.hasBattery ? state.connectivity.batteryPercent : 100;
        const std::wstring hBattStr = std::to_wstring(hBatt) + L"%";
        const float hBattW = SMeasure(hBattStr, SFSmall);
        const float bRight = drop.x - 16.0f;
        const D2D1_RECT_F hBp = D2D1::RectF(bRight - 18.0f, hy - 4.5f, bRight - 4.0f, hy + 4.5f);
        if (ID2D1SolidColorBrush* br = AcquireBrush(SFade(SCol(0xffffff)))) {
            target_->DrawRoundedRectangle(D2D1::RoundedRect(hBp, 2.0f, 2.0f), br, 1.2f);
            const D2D1_RECT_F tip = D2D1::RectF(hBp.right + 0.5f, hy - 2.0f, hBp.right + 2.0f, hy + 2.0f);
            target_->FillRoundedRectangle(D2D1::RoundedRect(tip, 0.8f, 0.8f), br);
            const float hFill = (hBp.right - hBp.left - 2.0f) * std::clamp(hBatt / 100.0f, 0.0f, 1.0f);
            if (ID2D1SolidColorBrush* gbr = AcquireBrush(SFade(SCol(0x30d158)))) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(hBp.left + 1.0f, hBp.top + 1.0f, hBp.left + 1.0f + hFill, hBp.bottom - 1.0f), 1.0f, 1.0f), gbr);
            }
        }
        SText(hBattStr, D2D1::RectF(bRight - 22.0f - hBattW, hy - 8.0f, bRight - 20.0f, hy + 8.0f), SFSmall, SFade(SCol(0xffffff)), 2);
        float rightEdge = bRight - 26.0f - hBattW;   // where the next item to the left ends

        // 5. Claude Code's 5-hour limit / Antigravity Quota (positioned left of battery)
        float used = 0.0f;
        const wchar_t* tag = L"";
        if (AgentsShown(settings) && PickUsage(state, settings, now, &used, &tag)) {
            const float u = used / 100.0f;
            const D2D1_COLOR_F uc = u >= 0.85f ? SCol(0xff3b3b) : u >= 0.6f ? SCol(0xff9a1f) : SCol(0x22e05a);
            wchar_t pct[16];
            swprintf_s(pct, L"%s %d%%", tag, static_cast<int>(used + 0.5f));
            const float pw = SMeasure(pct, SFLabel);
            const D2D1_POINT_2F rc = D2D1::Point2F(rightEdge - pw - 14.0f, hy);
            SRing(rc, 6.0f, 2.4f, u, uc);
            SText(pct, D2D1::RectF(rc.x + 10, hy - 8, rc.x + 12 + pw, hy + 8), SFLabel, SFade(SCol(0x8e8e93)));
            SHit(D2D1::RectF(rc.x - 9, hy - 10, rc.x + 12 + pw, hy + 10), StudioAction::AgentPage);
            rightEdge = rc.x - 9.0f;
        }

        // 5b. Bunbun: Work mode without agents on show: the day's progress in that place.
        if (!AgentsShown(settings) && state.work.on && state.work.goal > 0) {
            const float u = std::min(1.0f, static_cast<float>(state.work.Worked()) / state.work.goal);
            const D2D1_COLOR_F uc = WorkPhaseColour(static_cast<bb::work::Phase>(state.work.phase));
            wchar_t pct[24];
            swprintf_s(pct, state.work.study ? L"Study %d%%" : L"Work %d%%", static_cast<int>(u * 100.0f + 0.5f));
            const float pw = SMeasure(pct, SFLabel);
            const D2D1_POINT_2F rc = D2D1::Point2F(rightEdge - pw - 14.0f, hy);
            SRing(rc, 6.0f, 2.4f, u, uc);
            SText(pct, D2D1::RectF(rc.x + 10, hy - 8, rc.x + 12 + pw, hy + 8), SFLabel, SFade(SCol(0x8e8e93)));
            SHit(D2D1::RectF(rc.x - 9, hy - 10, rc.x + 12 + pw, hy + 10), StudioAction::WorkPage);
            rightEdge = rc.x - 9.0f;
        }

        // 6. Date and time, right-aligned against the rest.
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);
            static const wchar_t* kDays[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
            static const wchar_t* kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                               L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
            wchar_t date[32];
            swprintf_s(date, L"%s %d %s", kDays[t.wDayOfWeek % 7], t.wDay, kMonths[(t.wMonth + 11) % 12]);
            const std::wstring timeStr = FormatIslandTime(t, settings.clockFollowSystem, settings.use24HourClock, false);
            const float timeW = SMeasure(timeStr, SFTitle), dateW = SMeasure(date, SFBody);
            const float timeRight = rightEdge - 12.0f;
            const float timeLeft = timeRight - timeW;
            if (timeLeft >= leftEnd) {
                SText(timeStr, D2D1::RectF(timeLeft, hy - 10, timeRight + 2, hy + 10), SFTitle, SFade(SCol(0xffffff)));
                const float dateLeft = timeLeft - 6.0f - dateW;
                if (dateLeft >= leftEnd)
                    SText(date, D2D1::RectF(dateLeft, hy - 9, timeLeft - 4.0f, hy + 9), SFBody, SFade(SCol(0x8e8e93)));
            }
        }

        // Tab bar ─ one icon per page; the open one widens into a tinted capsule.
        const float fy = isl.bottom - 19.0f;
        const int n = static_cast<int>(studio_.tabs.size());
        if (static_cast<int>(studioTabW_.size()) != n) studioTabW_.assign(static_cast<size_t>(n), 30.0f);
        std::vector<std::wstring> labels(static_cast<size_t>(n));
        std::vector<StudioPage> pages(static_cast<size_t>(n));
        float total = 0.0f;
        bool easing = false;
        const float maxBar = (isl.right - isl.left) - 2 * 64.0f;
        for (int i = 0; i < n; ++i) {
            pages[i] = StudioPageFor(studio_.context, studio_.tabs[static_cast<size_t>(i)]);
            labels[i] = StudioTabLabel(pages[i]);
            if (pages[i] == StudioPage::Work && state.work.study) labels[i] = L"Study";  // Bunbun
            const float want = i == studio_.tabIdx ? 30.0f + 10.0f + SMeasure(labels[i], SFLabel) : 30.0f;
            float& cur = studioTabW_[static_cast<size_t>(i)];
            cur += (want - cur) * 0.25f;
            if (std::fabs(want - cur) < 0.3f) cur = want; else easing = true;
            total += cur + (i ? 2.0f : 0.0f);
        }
        if (easing) g_layoutDirty = true;
        // Too many tabs for the bar: the inactive ones shrink (down to 20), the open
        // one keeps its full width so its name always shows.
        float inactiveScale = 1.0f;
        if (total > maxBar && n > 1) {
            float activeW = studioTabW_[static_cast<size_t>(std::clamp(studio_.tabIdx, 0, n - 1))];
            float others = total - activeW;
            inactiveScale = std::max(20.0f / 30.0f, (maxBar - activeW) / std::max(1.0f, others));
            total = activeW + others * inactiveScale;
        }
        const bool squeeze = false;
        float tx = cx - total * 0.5f;
        for (int i = 0; i < n; ++i) {
            float w = studioTabW_[static_cast<size_t>(i)];
            if (i != studio_.tabIdx) w *= inactiveScale;
            const bool active = i == studio_.tabIdx;
            const D2D1_COLOR_F tint = StudioPageColor(pages[i], state);
            const float hoverAmt = SHover(StudioAction::Tab, i);
            if (active) {
                const bool white = pages[i] == StudioPage::Home;
                SFillRound(D2D1::RectF(tx, fy - 13, tx + w, fy + 13), 13, SFade(white ? SCol(0x2c2c2e) : WithAlpha(tint, 0.22f)));
                const D2D1_COLOR_F ink = SFade(white ? SCol(0xffffff) : tint);
                SIcon(pages[i], D2D1::Point2F(tx + 15, fy), 15.0f, ink);
                if (w > 44.0f && !squeeze) SText(labels[i], D2D1::RectF(tx + 27, fy - 9, tx + w - 4, fy + 9), SFLabel, ink);
            } else {
                if (hoverAmt > 0.01f) {
                    if (ID2D1SolidColorBrush* b = AcquireBrush(SFade(WithAlpha(SCol(0x3a3a3c), hoverAmt))))
                        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx + w * 0.5f, fy), 13 * hoverAmt + 6 * (1 - hoverAmt), 13), b);
                }
                SIcon(pages[i], D2D1::Point2F(tx + w * 0.5f, fy - 1.5f * hoverAmt), 15.0f * (1.0f + 0.12f * hoverAmt),
                      SFade(SLerpCol(SCol(0x8e8e93), SCol(0xffffff), hoverAmt)));
            }
            SHit(D2D1::RectF(tx, fy - 15, tx + w, fy + 15), StudioAction::Tab, i);
            tx += w + 2.0f;
        }

        DrawTrayDropOverlay(now);
        studioAlpha_ = 1.0f;
        // Bunbun: the light-island remap stays on until the next frame begins: live pages
        // (Weather, System) repaint their cards in partial passes after this.
    }

    // A file dragged over the open island: a drop zone with a marching dashed
    // outline. Just after a drop: a card that fills up and ticks "In your tray".
    void DrawTrayDropOverlay(double now) {
        const D2D1_RECT_F pg = studio_.page;
        const D2D1_COLOR_F yellow = SAccent();
        if (g_trayDragOver.load(std::memory_order_relaxed)) {
            SFillRound(pg, 22, SFade(D2D1::ColorF(0.04f, 0.04f, 0.05f, 0.92f)));
            const D2D1_RECT_F zone = D2D1::RectF(pg.left + 8, pg.top + 6, pg.right - 8, pg.bottom - 6);
            const float dashes[] = {5.0f, 4.0f};
            ComPtr<ID2D1StrokeStyle> dashed;
            D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                D2D1_DASH_STYLE_CUSTOM, -static_cast<float>(std::fmod(now * 18.0, 9.0)));  // marching
            d2dFactory_->CreateStrokeStyle(props, dashes, 2, &dashed);
            if (ID2D1SolidColorBrush* b = AcquireBrush(SFade(yellow)))
                target_->DrawRoundedRectangle(D2D1::RoundedRect(zone, 20, 20), b, 2.0f, dashed.Get());
            const float cx = (pg.left + pg.right) * 0.5f, cy = (pg.top + pg.bottom) * 0.5f;
            SIcon(StudioPage::Tray, D2D1::Point2F(cx, cy - 22), 30.0f, SFade(yellow));
            SText(L"Let go, I'll catch it!", D2D1::RectF(pg.left, cy + 2, pg.right, cy + 28), SFSong, SFade(SCol(0xffffff)), 1);
            g_layoutDirty = true;  // keep the outline marching
            return;
        }
        const double since = now - g_trayDropAt.load();
        if (since < 0.0 || since > 1.6) return;
        g_layoutDirty = true;
        // Pop in, fill the bar, tick, fade out.
        const float in = SmoothStep01(Clamp(static_cast<float>(since / 0.25), 0.0f, 1.0f));
        const float out = 1.0f - SmoothStep01(Clamp(static_cast<float>((since - 1.3) / 0.3), 0.0f, 1.0f));
        const float a = in * out;
        const float cx = (pg.left + pg.right) * 0.5f, cy = (pg.top + pg.bottom) * 0.5f;
        const float w = 300.0f * (0.9f + 0.1f * in), h = 92.0f;
        const D2D1_RECT_F card = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
        SFillRound(card, 20, D2D1::ColorF(0.11f, 0.11f, 0.12f, 0.97f * a));
        SIcon(StudioPage::Tray, D2D1::Point2F(card.left + 30, cy - 10), 24.0f, WithAlpha(yellow, a));
        std::wstring name = g_trayDropName;
        if (g_trayDropCount > 1) name += L" + " + std::to_wstring(g_trayDropCount - 1) + L" more";
        SText(name, D2D1::RectF(card.left + 56, card.top + 14, card.right - 16, card.top + 34), SFTitle, D2D1::ColorF(1, 1, 1, a));
        const float fill = SmoothStep01(Clamp(static_cast<float>((since - 0.3) / 0.65), 0.0f, 1.0f));
        const D2D1_RECT_F bar = D2D1::RectF(card.left + 56, card.top + 44, card.right - 20, card.top + 50);
        SFillRound(bar, 3, D2D1::ColorF(1, 1, 1, 0.12f * a));
        SFillRound(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * fill, bar.bottom), 3, WithAlpha(yellow, a));
        if (since > 0.95) {
            const float tick = SmoothStep01(Clamp(static_cast<float>((since - 0.95) / 0.2), 0.0f, 1.0f));
            if (ID2D1SolidColorBrush* b = AcquireBrush(WithAlpha(SCol(0x30d158), a * tick)))
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(card.left + 66, card.top + 70), 8, 8), b);
            auto P = [](float x, float y) { return D2D1::Point2F(x, y); };
            SPoly({P(8.5f, 12), P(11, 14.5f), P(15.5f, 9.5f)}, D2D1::Point2F(card.left + 66, card.top + 70), 0.9f,
                  D2D1::ColorF(0, 0, 0, a * tick), 1.8f);
            SText(L"In your tray", D2D1::RectF(card.left + 80, card.top + 60, card.right - 16, card.top + 80), SFBody,
                  WithAlpha(SCol(0x30d158), a * tick));
        }
    }
    D2D1_POINT_2F publishedOrigin_{};

    // ── Home tiles ──
    void DrawStudioTile(D2D1_RECT_F r, D2D1_COLOR_F glow, bool live, double now) {
        const float w = r.right - r.left, h = r.bottom - r.top;
        SFillRound(r, 22, D2D1::ColorF(1, 1, 1, 0.055f));
        float a = live ? 0.82f + 0.30f * (0.5f + 0.5f * std::sin(static_cast<float>(now) * 6.2831853f / 2.6f)) : 0.55f;
        D2D1_GRADIENT_STOP stops[3] = {
            {0.0f, WithAlpha(glow, 0.0f)},
            {0.55f, WithAlpha(glow, 0.22f * a)},
            {1.0f, WithAlpha(glow, 0.62f * a)},
        };
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> grad;
        if (SUCCEEDED(target_->CreateGradientStopCollection(stops, 3, &coll)) &&
            SUCCEEDED(target_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.top + h * 0.18f), D2D1::Point2F(0, r.bottom)),
                coll.Get(), &grad))) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(r, 22, 22), grad.Get());
        }
        if (ID2D1SolidColorBrush* rim = AcquireBrush(D2D1::ColorF(1, 1, 1, 0.10f)))
            target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 21.5f, 21.5f), rim, 1.0f);
        (void)w;
    }

    // A round white button with a glyph (0 play, 1 pause, 2 stop) in `ink`.
    void SRoundButton(D2D1_POINT_2F c, float r, int glyph, D2D1_COLOR_F ink, float hover) {
        r *= 1.0f + 0.08f * hover;  // lifts a little under the pointer
        if (hover > 0.01f) {
            if (ID2D1SolidColorBrush* g = AcquireBrush(D2D1::ColorF(1, 1, 1, 0.18f * hover)))
                target_->FillEllipse(D2D1::Ellipse(c, r + 4.0f, r + 4.0f), g);
        }
        if (ID2D1SolidColorBrush* b = AcquireBrush(D2D1::ColorF(1, 1, 1, SLerp(0.92f, 1.0f, hover))))
            target_->FillEllipse(D2D1::Ellipse(c, r, r), b);
        const float g = r * 0.42f;
        auto P = [](float x, float y) { return D2D1::Point2F(x, y); };
        if (glyph == 0) {
            SPoly({P(12 - g * 0.55f, 12 - g * 0.75f), P(12 + g * 0.8f, 12), P(12 - g * 0.55f, 12 + g * 0.75f)}, c, 1.0f, ink, 0, true, true);
        } else if (glyph == 1) {
            SFillRound(D2D1::RectF(c.x - g * 0.6f, c.y - g * 0.7f, c.x - g * 0.15f, c.y + g * 0.7f), 1.2f, ink);
            SFillRound(D2D1::RectF(c.x + g * 0.15f, c.y - g * 0.7f, c.x + g * 0.6f, c.y + g * 0.7f), 1.2f, ink);
        } else {
            SFillRound(D2D1::RectF(c.x - g * 0.5f, c.y - g * 0.5f, c.x + g * 0.5f, c.y + g * 0.5f), 2.0f, ink);
        }
    }

    void DrawStudioHome(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        EnsureRoundStroke();
        studioOrigin_ = publishedOrigin_;
        const float W = rect.right - rect.left;
        const float gap = 10.0f;
        const float tileW = (W - 2 * gap) / 3.0f;
        const float tileH = std::min(tileW, (rect.bottom - rect.top) - 34.0f);
        const float top = rect.top + 2.0f;
        const D2D1_COLOR_F white = SCol(0xffffff);
        const bb::AgentView& a = state.agent;

        // First tile: the main agent, or the weather when no agent is shown.
        auto tileWeather = [&](const D2D1_RECT_F r) {
            const D2D1_COLOR_F sky = SCol(0x64d2ff);  // Home cards keep their own colours; only Music follows the album
            if (const int tab = DashboardPageIndex(StudioPage::Weather, settings); tab >= 0) SHit(r, StudioAction::Tab, tab);
            DrawStudioTile(r, sky, false, now);
            SText(state.weather.city.empty() ? std::wstring(L"Weather") : state.weather.city,
                  D2D1::RectF(r.left + 14, r.top + 14, r.right - 52, r.top + 32), SFLabel, D2D1::ColorF(1, 1, 1, 0.65f));
            SIcon(StudioPage::Weather, D2D1::Point2F(r.right - 30, r.top + 30), 20, sky);
            if (state.weather.hasData) {
                std::wstring icon, desc = state.weather.weatherDesc;
                GetWeatherIconAndText(state.weather.weatherCode, icon, desc);
                wchar_t temp[16];
                swprintf_s(temp, L"%.0f\x00B0", state.weather.temperature);
                SText(temp, D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 10, r.bottom - 28), SFBig, white);
                SText(desc, D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 12, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
            } else {
                SText(L"--\x00B0", D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 10, r.bottom - 28), SFBig, white);
                SText(L"Weather is loading", D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 12, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
            }
        };
        auto tileAgent = [&](const D2D1_RECT_F r) {
            const bool busy = a.phase == bb::AgentPhase::Working || a.phase == bb::AgentPhase::Thinking;
            const bool waiting = a.phase == bb::AgentPhase::Approval || a.phase == bb::AgentPhase::Question;
            // Idle, the tile wears the main agent's colour (Claude orange, Antigravity blue...).
            const std::wstring brandName = a.agent.empty() ? std::wstring(AgentKindName(MainAgentKind(settings))) : a.agent;
            const D2D1_COLOR_F col = a.phase == bb::AgentPhase::None ? AgentBrand(brandName) : AgentStatusColor(a.phase);
            SHit(r, StudioAction::AgentFocus);  // opens the agent's window, or the Claude app
            DrawStudioTile(r, busy ? AgentBrand(a.agent) : col, busy || waiting, now);
            const std::wstring tileName = a.phase != bb::AgentPhase::None && !a.agent.empty()
                                              ? a.agent : std::wstring(AgentKindName(MainAgentKind(settings)));
            SText(tileName, D2D1::RectF(r.left + 14, r.top + 14, r.right - 52, r.top + 32), SFLabel, D2D1::ColorF(1, 1, 1, 0.65f));
            const D2D1_POINT_2F rc = D2D1::Point2F(r.right - 30, r.top + 30);
            std::wstring big, line;
            if (busy) {
                SRing(rc, 15, 4.5f, (std::max(a.stage, 0) + 0.5f) / 4.0f, AgentBrand(a.agent));
                SIcon(StudioPage::Agent, rc, 13, AgentBrand(a.agent));
                big = Clock(a.turnStart > 0 ? now - a.turnStart : now - a.phaseSince);
                line = StageWord(a.phase == bb::AgentPhase::Thinking && a.stage < 0 ? -1 : a.stage);
                if (!a.project.empty()) line += L" \x00B7 " + a.project;
            } else if (waiting) {
                SRing(rc, 15, 4.5f, 1.0f, col);
                SIcon(StudioPage::Agent, rc, 13, col);
                big = a.phase == bb::AgentPhase::Question ? L"Your turn" : L"May I?";
                line = a.tool.empty() ? a.detail : a.tool;
            } else if (a.phase == bb::AgentPhase::Finished) {
                SRing(rc, 15, 4.5f, 1.0f, col);
                big = L"Done";
                line = a.prompt.empty() ? a.step : a.prompt;
            } else if (a.phase == bb::AgentPhase::Error) {
                SRing(rc, 15, 4.5f, 1.0f, col);
                big = L"Stopped";
                line = a.step;
            } else {
                float used = 0.0f;
                const wchar_t* tag = L"";
                if (PickUsage(state, settings, now, &used, &tag)) {
                    const float u = used / 100.0f;
                    SRing(rc, 15, 4.5f, u, u >= 0.85f ? SCol(0xff3b3b) : u >= 0.6f ? SCol(0xff9a1f) : SCol(0x22e05a));
                    wchar_t pct[8];
                    swprintf_s(pct, L"%d", static_cast<int>(used + 0.5f));
                    SText(pct, D2D1::RectF(rc.x - 12, rc.y - 8, rc.x + 12, rc.y + 8), SFSmall, white, 1);
                } else {
                    SRing(rc, 15, 4.5f, 0.0f, white);
                    SIcon(StudioPage::Agent, rc, 13, D2D1::ColorF(1, 1, 1, 0.5f));
                }
                big = std::to_wstring(a.jobsToday);
                line = a.jobsToday == 1 ? L"task today" : L"tasks today";
            }
            SText(big, D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 10, r.bottom - 28), big.size() > 6 ? SFSong : SFBig, white);
            SText(line, D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 12, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
        };
        auto tileMusic = [&](const D2D1_RECT_F r) {
            const bool has = state.media.available && !state.media.title.empty();
            const D2D1_COLOR_F col = SAccent();
            SHit(r, StudioAction::OpenMusic);
            DrawStudioTile(r, col, state.media.playing, now);
            const D2D1_RECT_F artAt = ArtShape(state.media, D2D1::RectF(r.right - 54, r.top + 12, r.right - 12, r.top + 54), true);
            SText(L"Music", D2D1::RectF(r.left + 14, r.top + 14, std::max(r.left + 60, artAt.left - 6), r.top + 32), SFLabel, D2D1::ColorF(1, 1, 1, 0.65f));
            if (has) {
                DrawAlbumArt(state.media, D2D1::RectF(r.right - 54, r.top + 12, r.right - 12, r.top + 54), now, 10.0f, false,
                             true);  // Bunbun: a wide cover widens to the left
                SText(state.media.title, D2D1::RectF(r.left + 14, r.bottom - 62, r.right - 46, r.bottom - 42), SFSong, white);
                SText(state.media.artist, D2D1::RectF(r.left + 14, r.bottom - 40, r.right - 46, r.bottom - 24), SFBody, D2D1::ColorF(1, 1, 1, 0.6f));
                const D2D1_POINT_2F bc = D2D1::Point2F(r.right - 27, r.bottom - 27);
                SRoundButton(bc, 15, state.media.playing ? 1 : 0, SCol(0x000000), SHover(StudioAction::PlayPause));
                SHit(D2D1::RectF(bc.x - 17, bc.y - 17, bc.x + 17, bc.y + 17), StudioAction::PlayPause);
            } else {
                SIcon(StudioPage::Music, D2D1::Point2F(r.right - 30, r.top + 30), 18, D2D1::ColorF(1, 1, 1, 0.5f));
                SText(L"Quiet", D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 10, r.bottom - 28), SFBig, white);
                SText(L"Nothing playing", D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 12, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
            }
        };
        auto tileFocus = [&](const D2D1_RECT_F r) {
            const D2D1_COLOR_F purple = SCol(0xbf5af2);  // Bunbun: Home cards keep their own colours
            const TimerSnapshot& tm = state.timer;
            if (settings.focusPage) SHit(r, StudioAction::FocusPage);
            DrawStudioTile(r, purple, tm.active && tm.running, now);
            SText(tm.active && tm.isBreak ? L"Break" : L"Focus", D2D1::RectF(r.left + 14, r.top + 14, r.right - 52, r.top + 32), SFLabel, D2D1::ColorF(1, 1, 1, 0.65f));
            const D2D1_POINT_2F rc = D2D1::Point2F(r.right - 30, r.top + 30);
            const D2D1_POINT_2F bc = D2D1::Point2F(r.right - 27, r.bottom - 27);
            if (tm.active) {
                const double left = tm.running ? std::max(0.0, tm.endsAt - now) : tm.remainingAtPause;
                SRing(rc, 15, 4.5f, tm.totalSeconds > 0 ? static_cast<float>(left / tm.totalSeconds) : 0.0f, purple);
                SText(Clock(left), D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 46, r.bottom - 28), SFBig, white);
                const int mins = static_cast<int>(std::ceil(left / 60.0));
                SText(tm.running ? std::to_wstring(mins) + (mins == 1 ? L" min left" : L" mins left") : std::wstring(L"Paused"),
                      D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 46, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
                SRoundButton(bc, 15, 2, purple, SHover(StudioAction::FocusStop));
                SHit(D2D1::RectF(bc.x - 17, bc.y - 17, bc.x + 17, bc.y + 17), StudioAction::FocusStop);
            } else {
                SRing(rc, 15, 4.5f, 0.0f, purple);
                SText(L"25:00", D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 46, r.bottom - 28), SFBig, white);
                SText(L"Ready to start", D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 46, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
                SRoundButton(bc, 15, 0, purple, SHover(StudioAction::FocusStart));
                SHit(D2D1::RectF(bc.x - 17, bc.y - 17, bc.x + 17, bc.y + 17), StudioAction::FocusStart, 25);
            }
        };
        // Bunbun: Work mode's card: today's work against the goal, the state, start / pause.
        auto tileWork = [&](const D2D1_RECT_F r) {
            const WorkSnapshot& w = state.work;
            const auto phase = static_cast<bb::work::Phase>(w.phase);
            // Bunbun: Home cards keep their own colours (the Work page follows the accent).
            const D2D1_COLOR_F col = phase == bb::work::Phase::Working ? SCol(bb::work::KindColour(bb::work::Kind::Work))
                                                                         : WorkPhaseColour(phase);
            SHit(r, StudioAction::WorkPage);
            DrawStudioTile(r, col, phase == bb::work::Phase::Working || phase == bb::work::Phase::Overtime, now);
            SText(w.study ? L"Study" : L"Work", D2D1::RectF(r.left + 14, r.top + 14, r.right - 52, r.top + 32), SFLabel, D2D1::ColorF(1, 1, 1, 0.65f));
            const D2D1_POINT_2F rc = D2D1::Point2F(r.right - 30, r.top + 30);
            SRing(rc, 15, 4.5f, w.goal > 0 ? std::min(1.0f, static_cast<float>(w.Worked()) / w.goal) : 0.0f, col);
            SText(bb::work::Duration(w.Worked()), D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 46, r.bottom - 28), SFBig, white);
            // The card's line is short: "Until 18:00" while working, else the state.
            std::wstring line = phase == bb::work::Phase::Working && !w.study ? L"Until " + bb::work::FormatClock(w.end) : WorkStatusLine(w, now);
            if (w.study && phase == bb::work::Phase::Working && !w.subject.empty() && w.blockEnd >= 0)
                line = w.subject + L" \x00B7 " + bb::work::FormatClock(w.blockEnd);  // short on the card
            SText(line, D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 46, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
            const D2D1_POINT_2F bc = D2D1::Point2F(r.right - 27, r.bottom - 27);
            const bool going = phase == bb::work::Phase::Working || phase == bb::work::Phase::Overtime ||
                               phase == bb::work::Phase::Away || phase == bb::work::Phase::Asleep;
            const StudioAction act = going ? StudioAction::WorkPause
                                   : phase == bb::work::Phase::Paused || phase == bb::work::Phase::Break ? StudioAction::WorkResume
                                                                                                         : StudioAction::WorkStart;
            SRoundButton(bc, 15, going ? 1 : 0, col, SHover(act));
            SHit(D2D1::RectF(bc.x - 17, bc.y - 17, bc.x + 17, bc.y + 17), act);
        };
        auto tileCalendar = [&](const D2D1_RECT_F r) {
            const D2D1_COLOR_F red = SCol(0xff453a);  // Bunbun: Home cards keep their own colours
            if (const int tab = DashboardPageIndex(StudioPage::Calendar, settings); tab >= 0) SHit(r, StudioAction::Tab, tab);
            DrawStudioTile(r, red, state.meeting.valid, now);
            SYSTEMTIME st{};
            GetLocalTime(&st);
            static const wchar_t* kDays[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
            static const wchar_t* kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
            SText(kDays[st.wDayOfWeek % 7], D2D1::RectF(r.left + 14, r.top + 14, r.right - 52, r.top + 32), SFLabel, red);
            SIcon(StudioPage::Calendar, D2D1::Point2F(r.right - 30, r.top + 30), 20, red);
            SText(std::to_wstring(st.wDay) + L" " + kMonths[(st.wMonth + 11) % 12],
                  D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 10, r.bottom - 28), SFBig, white);
            std::wstring line = L"No meetings coming up";
            if (state.meeting.valid) {
                const time_t t = static_cast<time_t>(state.meeting.startsAt);
                tm lt{};
                localtime_s(&lt, &t);
                wchar_t b[16];
                swprintf_s(b, L"%02d:%02d ", lt.tm_hour, lt.tm_min);
                line = b + (state.meeting.title.empty() ? std::wstring(L"Meeting") : state.meeting.title);
            }
            SText(line, D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 12, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
        };
        auto tileBattery = [&](const D2D1_RECT_F r) {
            const int pct = state.battery.percent;
            const D2D1_COLOR_F col = !state.battery.hasBattery ? SCol(0x8e8e93)
                                     : state.battery.charging ? SCol(0x30d158) : pct <= 20 ? SCol(0xff453a) : SCol(0x30d158);
            DrawStudioTile(r, col, state.battery.reallyCharging, now);
            SText(L"Battery", D2D1::RectF(r.left + 14, r.top + 14, r.right - 52, r.top + 32), SFLabel, D2D1::ColorF(1, 1, 1, 0.65f));
            SRing(D2D1::Point2F(r.right - 30, r.top + 30), 15, 4.5f, state.battery.hasBattery ? pct / 100.0f : 0.0f, col);
            SText(state.battery.hasBattery ? std::to_wstring(pct) + L"%" : std::wstring(L"Plugged in"),
                  D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 10, r.bottom - 28), state.battery.hasBattery ? SFBig : SFSong, white);
            std::wstring line = !state.battery.hasBattery ? L"No battery" : state.battery.charging ? L"Charging" : L"On battery";
            if (state.battery.hasBattery && state.battery.charging && state.battery.minutesToFull > 0)
                {
                const int m = state.battery.minutesToFull;
                line = L"Full in " + (m >= 60 ? std::to_wstring(m / 60) + L" h " + std::to_wstring(m % 60) + L" min" : std::to_wstring(m) + L" min");
            }
            SText(line, D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 12, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
        };
        auto tileSystem = [&](const D2D1_RECT_F r) {
            const D2D1_COLOR_F green = SCol(0x30d158);  // Bunbun: Home cards keep their own colours
            if (const int tab = DashboardPageIndex(StudioPage::Hardware, settings); tab >= 0) SHit(r, StudioAction::Tab, tab);
            DrawStudioTile(r, green, false, now);
            SText(L"System", D2D1::RectF(r.left + 14, r.top + 14, r.right - 52, r.top + 32), SFLabel, D2D1::ColorF(1, 1, 1, 0.65f));
            SRing(D2D1::Point2F(r.right - 30, r.top + 30), 15, 4.5f, state.system.memoryPercent / 100.0f, SCol(0x64d2ff));
            SText(std::to_wstring(state.system.cpuPercent) + L"%", D2D1::RectF(r.left + 14, r.bottom - 58, r.right - 10, r.bottom - 28), SFBig, white);
            SText(L"CPU \x00B7 RAM " + std::to_wstring(state.system.memoryPercent) + L"%",
                  D2D1::RectF(r.left + 14, r.bottom - 26, r.right - 12, r.bottom - 10), SFBody, D2D1::ColorF(1, 1, 1, 0.8f));
        };

        // Three slots; each shows the tile picked in Settings (Bunbun.HomeTile1-3)
        // or with Edit on Home. Auto = agent (weather when no agent is shown), music, focus.
        for (int slot = 0; slot < 3; ++slot) {
            const float x = rect.left + slot * (tileW + gap);
            const D2D1_RECT_F r = D2D1::RectF(x, top, x + tileW, top + tileH);
            switch (HomeTileKind(settings, slot)) {
                case HomeTile::Agent: tileAgent(r); break;
                case HomeTile::Music: tileMusic(r); break;
                case HomeTile::Focus: tileFocus(r); break;
                case HomeTile::Weather: tileWeather(r); break;
                case HomeTile::Calendar: tileCalendar(r); break;
                case HomeTile::Battery: tileBattery(r); break;
                case HomeTile::System: tileSystem(r); break;
                case HomeTile::Work: tileWork(r); break;
            }
        }
        // The cards are chosen in Settings → Island pages (the island has no Edit mode).

        // Your links, as chips under the tiles (they take the info line's place).
        if (const auto links = ParseHomeLinks(settings.homeLinks); !links.empty()) {
            float total = 0.0f;
            std::vector<float> widths;
            for (const auto& [name, target] : links) {
                widths.push_back(SMeasure(name, SFLabel) + 40.0f);
                total += widths.back() + 8.0f;
            }
            float cx = (rect.left + rect.right - 60.0f - (total - 8.0f)) * 0.5f;
            const float cy = top + tileH + 6;
            for (size_t i = 0; i < links.size(); ++i) {
                const D2D1_RECT_F chip = D2D1::RectF(cx, cy, cx + widths[i], cy + 24);
                const float hover = SHover(StudioAction::OpenLink, static_cast<int>(i));
                SFillRound(chip, 12, D2D1::ColorF(1, 1, 1, SLerp(0.09f, 0.18f, hover)));
                // A coloured dot with the first letter, its hue from the address.
                unsigned hash = 0;
                for (wchar_t ch : links[i].second) hash = hash * 31 + ch;
                const float hue = static_cast<float>(hash % 360) / 360.0f;
                auto channel = [&](float n) {
                    const float k = std::fmod(n + hue * 12.0f, 12.0f);
                    return 0.52f - 0.62f * 0.48f * std::max(-1.0f, std::min({k - 3.0f, 9.0f - k, 1.0f}));
                };
                const D2D1_COLOR_F dot = D2D1::ColorF(channel(0), channel(8), channel(4));
                if (ID2D1SolidColorBrush* b = AcquireBrush(dot))
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(chip.left + 14, cy + 12), 8, 8), b);
                SText(std::wstring(1, static_cast<wchar_t>(towupper(links[i].first.empty() ? L'?' : links[i].first[0]))),
                      D2D1::RectF(chip.left + 6, cy, chip.left + 22, cy + 24), SFSmall, white, 1);
                SText(links[i].first, D2D1::RectF(chip.left + 28, cy, chip.right - 8, cy + 24), SFLabel, white);
                SHit(chip, StudioAction::OpenLink, static_cast<int>(i));
                cx += widths[i] + 8.0f;
            }
            return;
        }

        // One quiet line under the tiles: weather and battery.
        std::wstring info;
        if (state.weather.hasData) {
            std::wstring icon = L"", text = state.weather.weatherDesc;
            GetWeatherIconAndText(state.weather.weatherCode, icon, text);
            wchar_t b[96];
            swprintf_s(b, L"%s %.0f\x00B0  %s", icon.c_str(), state.weather.temperature, text.c_str());
            info = b;
            if (!state.weather.city.empty()) info += L" \x00B7 " + state.weather.city;
        }
        if (state.battery.hasBattery) {
            if (!info.empty()) info += L"   ";
            info += (state.battery.charging ? L"\x26A1 " : L"\U0001F50B ") + std::to_wstring(state.battery.percent) + L"%";
        }
        SText(info, D2D1::RectF(rect.left + 4, top + tileH + 6, rect.right - 60, top + tileH + 28), SFBody, D2D1::ColorF(1, 1, 1, 0.55f), 1);
    }

    // ── Extra pages (both layouts) ──
    bool DrawStudioExtraPage(int tabId, const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        if (tabId != kTabAgent && tabId != kTabClipboard && tabId != kTabNote && tabId != kTabFocus && tabId != kTabApps &&
            tabId != kTabMirror && tabId != kTabHome && tabId != kTabWater && tabId != kTabScreenTime && tabId != kTabWork)
            return false;
        if (tabId == kTabHome) {
            DrawStudioHome(state, rect, settings, now);
            return true;
        }
        EnsureRoundStroke();
        studioOrigin_ = publishedOrigin_;
        // Classic pages have no frame, so keep clear of the pagination dots on the right.
        D2D1_RECT_F r = rect;
        if (!settings.studioLayout) r = D2D1::RectF(rect.left + 18, rect.top + 14, rect.right - 24, rect.bottom - 12);
        if (tabId == kTabAgent) DrawStudioAgentPage(state, r, now);
        else if (tabId == kTabClipboard) DrawStudioClipboardPage(r);
        else if (tabId == kTabFocus) DrawStudioTimersPage(state, r, now);  // Bunbun: the Timers page
        else if (tabId == kTabApps) DrawStudioAppsPage(state, r);
        else if (tabId == kTabMirror) DrawStudioMirrorPage(r);
        else if (tabId == kTabWater) DrawStudioWaterPage(state, r, now);
        else if (tabId == kTabScreenTime) DrawStudioScreenTimePage(state, r, settings, now);
        else if (tabId == kTabWork) DrawStudioWorkPage(state, r, now);  // Bunbun: Work mode
        else DrawStudioNotePage(r, now);
        return true;
    }

    // ── Water (water_module.inc) ──
    // A glass filling up to the day's goal (its surface moves gently), then today:
    // the amount, a drop per glass, + glass / undo / refilled and the next reminder;
    // then the week against the goal.
    void DrawStudioWaterPage(const SharedState& state, D2D1_RECT_F r, double now) {
        const WaterSnapshot& w = state.water;
        // Bunbun: the accent, and a deeper shade of it for the water's body.
        const D2D1_COLOR_F blue = SAccent(), white = SCol(0xffffff), grey = SCol(0x8e8e93);
        const D2D1_COLOR_F deep = D2D1::ColorF(blue.r * 0.72f, blue.g * 0.72f, blue.b * 0.72f, 1.0f);
        const float H = r.bottom - r.top;
        auto amount = [](int ml) {
            wchar_t b[24];
            if (ml >= 1000) swprintf_s(b, ml % 100 ? L"%.2f L" : L"%.1f L", ml / 1000.0);
            else swprintf_s(b, L"%d ml", ml);
            return std::wstring(b);
        };
        const float frac = w.goalMl > 0 ? Clamp(static_cast<float>(w.takenMl) / w.goalMl, 0.0f, 1.0f) : 0.0f;

        // ── The glass ──
        const float gW = std::min(92.0f, H * 0.5f), gH = H - 12.0f;
        const D2D1_RECT_F glass = D2D1::RectF(r.left + 4, r.top + 6, r.left + 4 + gW, r.top + 6 + gH);
        const float gr = 18.0f;
        SFillRound(glass, gr, D2D1::ColorF(1, 1, 1, 0.05f));
        ComPtr<ID2D1RoundedRectangleGeometry> shape;
        ComPtr<ID2D1Layer> layer;
        if (frac > 0.0f && SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(glass, gr, gr), &shape)) &&
            SUCCEEDED(target_->CreateLayer(&layer))) {
            target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), layer.Get());
            // Two waves: a pale one behind, the water in front, moving at different speeds.
            const float level = glass.bottom - (glass.bottom - glass.top) * std::max(frac, 0.04f);
            auto wave = [&](float amp, float speed, float phase, D2D1_COLOR_F col) {
                ComPtr<ID2D1PathGeometry> path;
                ComPtr<ID2D1GeometrySink> sink;
                if (FAILED(d2dFactory_->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
                sink->BeginFigure(D2D1::Point2F(glass.left, glass.bottom), D2D1_FIGURE_BEGIN_FILLED);
                for (float x = glass.left; x <= glass.right + 0.5f; x += 3.0f) {
                    const float t = static_cast<float>(now) * speed + phase + (x - glass.left) * 0.11f;
                    sink->AddLine(D2D1::Point2F(x, level + std::sin(t) * amp));
                }
                sink->AddLine(D2D1::Point2F(glass.right, glass.bottom));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                if (ID2D1SolidColorBrush* b = AcquireBrush(col)) target_->FillGeometry(path.Get(), b);
            };
            wave(3.0f, 1.6f, 1.7f, D2D1::ColorF(blue.r, blue.g, blue.b, 0.35f));
            wave(2.4f, 2.3f, 0.0f, D2D1::ColorF(deep.r, deep.g, deep.b, 0.85f));
            target_->PopLayer();
        }
        if (ID2D1SolidColorBrush* rim = AcquireBrush(D2D1::ColorF(1, 1, 1, 0.16f)))
            target_->DrawRoundedRectangle(D2D1::RoundedRect(glass, gr, gr), rim, 1.5f);
        wchar_t pct[8];
        swprintf_s(pct, L"%d%%", static_cast<int>(frac * 100.0f + 0.5f));
        SText(pct, D2D1::RectF(glass.left, glass.top + 10, glass.right, glass.top + 34), SFSong, white, 1);

        // ── Today ──
        // Today takes about half of what is right of the glass (at least 210), the week the rest.
        const float x0 = glass.right + 22, colR = std::min(r.right, x0 + std::max(210.0f, (r.right - x0) * 0.52f));
        SText(L"Today", D2D1::RectF(x0, r.top + 4, colR, r.top + 20), SFSmall, grey);
        const std::wstring big = amount(w.takenMl);
        SText(big, D2D1::RectF(x0, r.top + 18, colR, r.top + 52), SFBig, white);
        const float bigW = SMeasureBox(big, SFBig, 400).width;
        SText(L"of " + amount(w.goalMl), D2D1::RectF(x0 + bigW + 8, r.top + 30, colR, r.top + 50), SFBody, grey);

        // A drop per glass of the goal: filled ones are drunk.
        const int glasses = std::clamp(w.glassMl > 0 ? (w.goalMl + w.glassMl - 1) / w.glassMl : 8, 1, 12);
        for (int i = 0; i < glasses; ++i) {
            const D2D1_POINT_2F c = D2D1::Point2F(x0 + 8 + i * 18.0f, r.top + 66);
            SIcon(StudioPage::Water, c, 14.0f, i < w.glasses ? blue : SCol(0x48484a));
        }

        auto pill = [&](D2D1_RECT_F b, const std::wstring& label, bool primary, StudioAction action) {
            const float hover = SHover(action);
            SFillRound(b, (b.bottom - b.top) * 0.5f,
                       primary ? D2D1::ColorF(blue.r, blue.g, blue.b, SLerp(0.9f, 1.0f, hover))
                               : D2D1::ColorF(1, 1, 1, SLerp(0.09f, 0.18f, hover)));
            // Bunbun: dark text on a light accent, white on a dark one.
            const bool lightFill = 0.2126f * blue.r + 0.7152f * blue.g + 0.0722f * blue.b > 0.55f;
            SText(label, b, SFLabel, primary ? (lightFill ? SCol(0x101014) : white) : white, 1);
            SHit(b, action);
        };
        const float by = r.top + 84;
        pill(D2D1::RectF(x0, by, x0 + 112, by + 30), L"+ " + std::to_wstring(w.glassMl) + L" ml", true, StudioAction::WaterAdd);
        float bx = x0 + 120;
        if (w.glasses > 0) {
            pill(D2D1::RectF(bx, by, bx + 60, by + 30), L"Undo", false, StudioAction::WaterUndo);
            bx += 68;
        }
        if (w.bottleMl > 0) pill(D2D1::RectF(bx, by, std::min(colR, bx + 86), by + 30), L"Refilled", false, StudioAction::WaterRefill);

        std::wstring status;
        D2D1_COLOR_F statusCol = grey;
        if (w.takenMl >= w.goalMl) {
            status = L"Goal reached. Nicely done!";
            statusCol = blue;
        } else if (!w.reminders) {
            status = L"Reminders off \x00B7 Settings \x2192 Productivity \x2192 Water";
        } else if (w.nextMinute < 0) {
            status = L"No more reminders today";
        } else {
            wchar_t b[48];
            swprintf_s(b, L"Next reminder at %02d:%02d", w.nextMinute / 60, w.nextMinute % 60);
            status = b;
        }
        SText(status, D2D1::RectF(x0, by + 38, colR, by + 56), SFSmall, statusCol);
        if (w.bottleMl > 0)
            SText(std::to_wstring(w.bottleLeftMl) + L" ml left in your bottle", D2D1::RectF(x0, by + 56, colR, by + 74), SFSmall, grey);

        // ── The week ──
        const D2D1_RECT_F card = D2D1::RectF(colR + 14, r.top + 4, r.right, r.bottom - 4);
        if (card.right - card.left < 120 || w.week.empty()) return;
        SFillRound(card, 14, D2D1::ColorF(1, 1, 1, 0.05f));
        long long sum = 0;
        for (const auto& d : w.week) sum += d.second;
        const int avg = static_cast<int>(sum / static_cast<long long>(w.week.size()));
        SText(L"This week", D2D1::RectF(card.left + 12, card.top + 8, card.right - 12, card.top + 24), SFSmall, grey);
        SText(L"avg " + amount(avg), D2D1::RectF(card.left + 12, card.top + 8, card.right - 12, card.top + 24), SFSmall, white, 2);
        const float top = card.top + 34, bottom = card.bottom - 24;
        const int n = static_cast<int>(w.week.size());
        const float slot = (card.right - card.left - 24) / 7.0f;
        // The goal line, dashed.
        for (float x = card.left + 12; x < card.right - 12; x += 7)
            SFillRound(D2D1::RectF(x, top - 1, std::min(x + 4, card.right - 12), top + 1), 1, D2D1::ColorF(1, 1, 1, 0.18f));
        static const wchar_t* kDays = L"SMTWTFS";
        for (int i = 0; i < n; ++i) {
            const int date = w.week[static_cast<size_t>(i)].first, ml = w.week[static_cast<size_t>(i)].second;
            const float cx = card.left + 12 + slot * (7 - n + i) + slot * 0.5f, bw = std::min(14.0f, slot * 0.5f);
            const float h = (bottom - top) * Clamp(w.goalMl > 0 ? static_cast<float>(ml) / w.goalMl : 0.0f, 0.0f, 1.0f);
            const bool today = i == n - 1;
            SFillRound(D2D1::RectF(cx - bw * 0.5f, top, cx + bw * 0.5f, bottom), bw * 0.5f, D2D1::ColorF(1, 1, 1, 0.06f));
            if (h > 0.5f)
                SFillRound(D2D1::RectF(cx - bw * 0.5f, bottom - std::max(h, bw), cx + bw * 0.5f, bottom), bw * 0.5f,
                           today ? blue : D2D1::ColorF(blue.r, blue.g, blue.b, ml >= w.goalMl ? 0.75f : 0.4f));
            tm t{};
            t.tm_year = date / 10000 - 1900;
            t.tm_mon = date / 100 % 100 - 1;
            t.tm_mday = date % 100;
            t.tm_hour = 12;
            mktime(&t);
            const wchar_t day[2] = {kDays[std::clamp(t.tm_wday, 0, 6)], 0};
            SText(day, D2D1::RectF(cx - 10, bottom + 4, cx + 10, bottom + 20), SFSmall, today ? white : grey, 1);
        }
    }

    void DrawStudioAgentPage(const SharedState& state, D2D1_RECT_F r, double now) {
        const bb::AgentView& a = state.agent;
        const float W = r.right - r.left;
        const D2D1_COLOR_F status = AgentStatusColor(a.phase);
        const D2D1_COLOR_F brand = AgentBrand(a.agent);
        const bool busy = a.phase == bb::AgentPhase::Working || a.phase == bb::AgentPhase::Thinking;
        SHit(r, StudioAction::AgentFocus);

        // Top row: lamp + agent · project, status on the right.
        const bool blink = busy && std::fmod(now, 1.2) < 0.6;
        if (ID2D1SolidColorBrush* b = AcquireBrush(WithAlpha(status, blink ? 0.45f : 1.0f)))
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 5, r.top + 10), 4, 4), b);
        std::wstring who = a.agent.empty() ? L"No agent running" : a.agent;
        if (!a.project.empty()) who += L" \x00B7 " + a.project;
        SText(who, D2D1::RectF(r.left + 16, r.top, r.left + W * 0.6f, r.top + 20), SFTitle, SCol(0xffffff));
        std::wstring stat;
        switch (a.phase) {
            case bb::AgentPhase::Thinking:
            case bb::AgentPhase::Working: stat = L"Working \x00B7 " + Clock(a.turnStart > 0 ? now - a.turnStart : now - a.phaseSince); break;
            case bb::AgentPhase::Approval: stat = L"Needs your OK"; break;
            case bb::AgentPhase::Question: stat = L"Has a question"; break;
            case bb::AgentPhase::Finished: stat = L"Done"; break;
            case bb::AgentPhase::Error: stat = L"Stopped"; break;
            default: stat = L"Idle"; break;
        }
        SText(stat, D2D1::RectF(r.left + W * 0.6f, r.top, r.right, r.top + 20), SFBody, SCol(0x8e8e93), 2);
        // Claude Code's model, as a chip that cycles Default / Opus / Sonnet / Haiku.
        if (AgentKindFor(a.agent) == AgentClaude && g_settings.showAgent[AgentClaude]) {
            const std::wstring model = L"Model: " + ClaudeModelLabel();
            const float mw = SMeasure(model, SFLabel) + 20;
            const float sw = SMeasure(stat, SFBody);
            const D2D1_RECT_F chip = D2D1::RectF(r.right - sw - 12 - mw, r.top, r.right - sw - 12, r.top + 20);
            SFillRound(chip, 10, D2D1::ColorF(1, 1, 1, SLerp(0.09f, 0.16f, SHover(StudioAction::ModelCycle))));
            SText(model, chip, SFLabel, SCol(0xe8e8ee), 1);
            SHit(chip, StudioAction::ModelCycle);
        }

        // Left: hero number or word, and a sub-line.
        const float leftW = W * 0.40f;
        std::wstring hero, sub;
        if (busy) {
            hero = Clock(a.turnStart > 0 ? now - a.turnStart : now - a.phaseSince);
            const int step = std::clamp(a.stage + 1, 1, 4);
            sub = std::wstring(StageWord(a.stage)) + L" \x00B7 step " + std::to_wstring(step) + L" of 4";
        } else if (a.phase == bb::AgentPhase::Approval) { hero = L"May I?"; sub = a.tool; }
        else if (a.phase == bb::AgentPhase::Question) { hero = L"Your turn"; sub = L"Answer on the island"; }
        else if (a.phase == bb::AgentPhase::Finished) { hero = L"Done"; sub = a.turnSeconds > 0 ? L"took " + Clock(a.turnSeconds) : L""; }
        else if (a.phase == bb::AgentPhase::Error) { hero = L"Stopped"; sub = a.step; }
        else { hero = std::to_wstring(a.jobsToday); sub = a.jobsToday == 1 ? L"task done today" : L"tasks done today"; }
        SText(hero, D2D1::RectF(r.left, r.top + 30, r.left + leftW, r.top + 82), hero.size() > 7 ? SFBig : SFHero, busy ? brand : SCol(0xffffff));
        SText(sub, D2D1::RectF(r.left, r.top + 84, r.left + leftW, r.top + 102), SFBody, SCol(0x8e8e93));

        // Right: a card with the command, the diff, the prompt, or recent tasks.
        const D2D1_RECT_F card = D2D1::RectF(r.left + leftW + 12, r.top + 28, r.right, r.bottom - 40);
        SFillRound(card, 12, D2D1::ColorF(1, 1, 1, 0.07f));
        const float lineH = 17.0f;
        float y = card.top + 8;
        auto row = [&](const std::wstring& text, StudioFontId f, D2D1_COLOR_F c, D2D1_COLOR_F bg = D2D1::ColorF(0, 0, 0, 0)) {
            if (y + lineH > card.bottom - 4) return;
            if (bg.a > 0) SFillRound(D2D1::RectF(card.left + 6, y, card.right - 6, y + lineH), 4, bg);
            SText(text, D2D1::RectF(card.left + 12, y, card.right - 10, y + lineH), f, c);
            y += lineH + 2;
        };
        if (!a.command.empty() && (a.stage == 2 || busy) && (a.stage == 2 || a.diffNew.empty())) {
            row(L"$ " + a.command, SFMono, SCol(0x32d74b));
            for (const auto& l : a.output) row(l, SFMono, a.commandFailed ? SCol(0xff6961) : SCol(0xaeaeb2));
            if (a.output.empty() && busy) row(L"running\x2026", SFMono, SCol(0x636366));
        } else if (!a.diffOld.empty() || !a.diffNew.empty()) {
            if (!a.diffOld.empty()) row(L"\x2212 " + bb::FirstDiffLine(a.diffOld), SFMono, SCol(0xff8a80), D2D1::ColorF(1.0f, 0.27f, 0.23f, 0.13f));
            if (!a.diffNew.empty()) row(L"+ " + bb::FirstDiffLine(a.diffNew), SFMono, SCol(0x7ee787), D2D1::ColorF(0.2f, 0.84f, 0.29f, 0.12f));
            if (a.added >= 0) row(L"+" + std::to_wstring(a.added) + L"  \x2212" + std::to_wstring(a.removed) + L" lines", SFSmall, SCol(0x8e8e93));
        } else if (busy || a.phase == bb::AgentPhase::Approval) {
            if (!a.prompt.empty()) row(L"\x201C" + a.prompt + L"\x201D", SFBody, SCol(0xffffff));
            row(a.phase == bb::AgentPhase::Approval ? a.detail : (a.step.empty() ? L"Thinking\x2026" : a.step), SFBody, SCol(0xaeaeb2));
        } else {
            row(L"Recently", SFLabel, SCol(0x8e8e93));
            if (a.history.empty()) row(L"Finished tasks show up here.", SFBody, SCol(0x636366));
            for (size_t i = 0; i < a.history.size() && i < 4; ++i) {
                const auto& h = a.history[i];
                const time_t when = static_cast<time_t>(h.finishedAt);
                tm lt{};
                localtime_s(&lt, &when);
                wchar_t right[32];
                swprintf_s(right, L"%02d:%02d \x00B7 %s", lt.tm_hour, lt.tm_min, Clock(h.seconds).c_str());
                const float rw = SMeasure(right, SFSmall);
                if (y + lineH > card.bottom - 4) break;
                SText((h.failed ? L"\x2715 " : L"") + (h.project.empty() ? h.agent : h.project) + L" \x00B7 " + h.prompt,
                      D2D1::RectF(card.left + 12, y, card.right - rw - 16, y + lineH), SFBody, SCol(0xffffff));
                SText(right, D2D1::RectF(card.right - rw - 12, y, card.right - 10, y + lineH), SFSmall, SCol(0x8e8e93), 2);
                y += lineH + 2;
            }
        }

        // A finished task still listening for your next message: Reply.
        if (a.replyId && !busy) {
            const std::wstring label = L"Reply\x2026";
            const D2D1_RECT_F rb = D2D1::RectF(card.right - 92, card.bottom - 34, card.right - 8, card.bottom - 8);
            SFillRound(rb, 13, D2D1::ColorF(1, 1, 1, SLerp(0.92f, 1.0f, SHover(StudioAction::AgentReply))));
            SText(label, rb, SFLabel, SCol(0x141416), 1);
            SHit(rb, StudioAction::AgentReply);
        }

        // Bottom: Read / Edit / Test / Done while working, else the usage limits.
        const float by = r.bottom - 30;
        if (busy || a.phase == bb::AgentPhase::Finished) {
            static const wchar_t* kSteps[] = {L"Read", L"Edit", L"Test", L"Done"};
            const float segW = (W - 3 * 8.0f) / 4.0f;
            for (int i = 0; i < 4; ++i) {
                const float x = r.left + i * (segW + 8.0f);
                const bool done = a.stage > i || a.phase == bb::AgentPhase::Finished;
                const bool current = !done && (a.stage == i || (a.stage < 0 && i == 0));
                SFillRound(D2D1::RectF(x, by, x + segW, by + 4), 2, D2D1::ColorF(1, 1, 1, done ? 0.95f : 0.16f));
                if (current) {
                    const float t = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 6.2831853f / 1.3f);
                    const float sw = segW * 0.45f, sx = x + (segW - sw) * t;
                    SFillRound(D2D1::RectF(sx, by, sx + sw, by + 4), 2, brand);
                }
                SText(kSteps[i], D2D1::RectF(x + (i == 0 ? 6.0f : 0.0f), by + 7, x + segW, by + 22), SFSmall, done || current ? SCol(0xffffff) : SCol(0x636366));
            }
        } else {
            // Bunbun: the limits of the agent on this page (or the main agent between
            // tasks), from what it keeps on this PC: Claude Code's 5-hour and weekly
            // windows and context, Antigravity's model pools, Cursor's plan.
            const long long unixNow = static_cast<long long>(time(nullptr));
            const int kind = a.agent.empty() ? MainAgentKind(g_settings) : static_cast<int>(AgentKindFor(a.agent));
            bb::limits::Row row;
            if (kind == AgentAntigravity && g_settings.antigravityQuota) {
                const AgQuota::Snapshot ag = AgQuota::Get();
                row = bb::limits::Antigravity(ag.valid ? ag.pools : std::vector<bb::agquota::Pool>{}, unixNow);
            } else if (kind == AgentCursor) {
                row = bb::limits::Cursor(AgQuota::CursorPlan());
            } else if (kind == AgentClaude && g_settings.usageLimits) {
                row = bb::limits::Claude(a.usage5h, a.usage5hResets, a.usage7d, a.usage7dResets, a.contextPct, unixNow);
            }
            if (!row.bars.empty()) {
                const float by = r.bottom - 38;  // a second line under each bar for its reset time
                const int n = static_cast<int>(row.bars.size());
                const float segW = (W - (n - 1) * 14.0f) / static_cast<float>(n);
                for (int i = 0; i < n; ++i) {
                    const bb::limits::Bar& b = row.bars[static_cast<size_t>(i)];
                    const float x = r.left + i * (segW + 14.0f);
                    const float p = b.used;
                    const D2D1_COLOR_F c = p >= 85 ? SCol(0xff3b3b) : p >= 60 ? SCol(0xff9a1f) : SCol(0x22e05a);
                    SFillRound(D2D1::RectF(x, by, x + segW, by + 4), 2, D2D1::ColorF(1, 1, 1, 0.16f));
                    if (p > 0) SFillRound(D2D1::RectF(x, by, x + std::max(4.0f, segW * Clamp(p / 100.0f, 0.0f, 1.0f)), by + 4), 2, c);
                    // "Gemini 3.8 Flash · 2%", then "resets in 2h 10m" under it.
                    std::wstring label = b.label;
                    if (p >= 0) label += L" \x00B7 " + std::to_wstring(static_cast<int>(p + 0.5f)) + L"%";
                    const float tx = x + (i == 0 ? 6.0f : 0.0f);  // clear of the page's rounded corner
                    SText(label, D2D1::RectF(tx, by + 7, x + segW, by + 21), SFSmall, p >= 0 ? SCol(0xaeaeb2) : SCol(0x636366));
                    const std::wstring reset = bb::limits::ResetsIn(b.resetAt, unixNow);
                    if (!reset.empty()) SText(reset, D2D1::RectF(tx, by + 21, x + segW, by + 34), SFSmall, SCol(0x636366));
                }
            } else if (!row.note.empty()) {
                // Bunbun: inset; the page's rounded corner clipped the first letter.
                SText(row.note, D2D1::RectF(r.left + 6, by, r.right, by + 20), SFSmall, SCol(0x8e8e93));
            }
        }
    }

    // ── Bunbun: Studio Clipboard Page Redo (Vayli 2-panel layout) ──
    void DrawStudioClipboardPage(D2D1_RECT_F r) {
        EnsureRoundStroke();

        const D2D1_COLOR_F white = SCol(0xffffff), grey = SCol(0x8e8e93), cardBg = SCol(0x131315);
        const D2D1_COLOR_F accent = SAccent();

        const float leftW = 212.0f;
        const float rightX = r.left + leftW;

        // ── Left Panel: Filter Tabs ──
        const float tabY = r.top + 2.0f;
        int allCnt = 0, textCnt = 0, imgCnt = 0, linkCnt = 0;
        {
            std::lock_guard lock(g_stateMutex);
            allCnt = static_cast<int>(g_richClipItems.size());
            for (const auto& it : g_richClipItems) {
                if (it.kind == ClipKind::Text) ++textCnt;
                else if (it.kind == ClipKind::Image) ++imgCnt;
                else if (it.kind == ClipKind::Link) ++linkCnt;
            }
        }

        struct TabInfo { int id; std::wstring name; int count; };
        const TabInfo tabs[4] = {
            {0, L"All", allCnt},
            {1, L"Text", textCnt},
            {2, L"Images", imgCnt},
            {3, L"Links", linkCnt}
        };

        float curX = r.left + 4.0f;
        for (int i = 0; i < 4; ++i) {
            const bool active = (g_clipFilter == tabs[i].id);
            const std::wstring label = tabs[i].name + L" " + std::to_wstring(tabs[i].count);
            const float tw = SMeasure(label, SFLabel) + 2.0f;
            const D2D1_RECT_F tRect = D2D1::RectF(curX, tabY, curX + tw, tabY + 16.0f);

            SText(label, tRect, SFLabel, active ? white : grey);
            if (active) {
                SFillRound(D2D1::RectF(curX, tabY + 16.0f, curX + tw, tabY + 18.0f), 1.0f, white);
            }
            SHit(D2D1::RectF(curX - 2.0f, tabY - 2.0f, curX + tw + 2.0f, tabY + 20.0f), StudioAction::ClipFilter, tabs[i].id);
            curX += tw + 10.0f;
        }

        // Search icon on far right of left panel
        const D2D1_RECT_F searchRect = D2D1::RectF(rightX - 20.0f, tabY, rightX - 4.0f, tabY + 16.0f);
        const D2D1_POINT_2F sc = D2D1::Point2F(searchRect.left + 7.0f, searchRect.top + 7.0f);
        if (ID2D1SolidColorBrush* sb = AcquireBrush(grey)) {
            target_->DrawEllipse(D2D1::Ellipse(sc, 4.0f, 4.0f), sb, 1.3f);
            target_->DrawLine(D2D1::Point2F(sc.x + 2.8f, sc.y + 2.8f), D2D1::Point2F(sc.x + 6.5f, sc.y + 6.5f), sb, 1.4f);
        }
        SHit(searchRect, StudioAction::ClipSearch, 0);

        // Section header: "Today"
        if (allCnt > 0)  // Bunbun: no "Today" over an empty list
            SText(L"Today", D2D1::RectF(r.left + 6.0f, r.top + 24.0f, r.left + 80.0f, r.top + 38.0f), SFSmall, grey);
        // Bunbun: Clear. Empties the history; pinned items stay.
        if (allCnt > 0) {
            const float cw = SMeasure(L"Clear", SFSmall) + 14.0f;
            const D2D1_RECT_F clearRect = D2D1::RectF(rightX - 8.0f - cw, r.top + 23.0f, rightX - 8.0f, r.top + 39.0f);
            const bool hot = SHovered(StudioAction::ClipClear);
            SFillRound(clearRect, 8.0f, SCol(0xFFFFFF, hot ? 0.14f : 0.07f));
            SText(L"Clear", clearRect, SFSmall, hot ? white : grey, 1);
            SHit(clearRect, StudioAction::ClipClear, 0);
        }

        // ── Left Panel: List of Items ──
        const std::vector<size_t> indices = FilteredClipIndices();
        const float listTop = r.top + 40.0f;
        const float rowH = 34.0f;

        for (size_t i = 0; i < indices.size() && (listTop + i * (rowH + 2.0f) + rowH <= r.bottom); ++i) {
            const size_t realIdx = indices[i];
            const float y = listTop + i * (rowH + 2.0f);
            const D2D1_RECT_F row = D2D1::RectF(r.left + 4.0f, y, rightX - 6.0f, y + rowH);
            const bool isSel = (static_cast<int>(i) == g_clipSelected);

            if (isSel) {
                SFillRound(row, 8.0f, SCol(0x28282a));
            } else if (SHover(StudioAction::ClipSelect, static_cast<int>(i)) > 0.05f) {
                SFillRound(row, 8.0f, SCol(0x1a1a1c));
            }

            ClipItem item;
            {
                std::lock_guard lock(g_stateMutex);
                if (realIdx < g_richClipItems.size()) item = g_richClipItems[realIdx];
            }

            // Thumbnail (28x28)
            const D2D1_RECT_F thumb = D2D1::RectF(row.left + 4.0f, row.top + 3.0f, row.left + 32.0f, row.bottom - 3.0f);
            SFillRound(thumb, 5.0f, SCol(0x222224));

            if (item.kind == ClipKind::Image && !item.image.bgra.empty()) {
                DrawCoverFitBitmapPixels(item.image, thumb, 5.0f, clipThumbBmp_[i % 8], clipThumbGen_[i % 8], 1.0f);
            } else {
                const D2D1_POINT_2F tc = D2D1::Point2F((thumb.left + thumb.right) * 0.5f, (thumb.top + thumb.bottom) * 0.5f);
                if (ID2D1SolidColorBrush* tb = AcquireBrush(grey)) {
                    if (item.kind == ClipKind::Link) {
                        target_->DrawLine(D2D1::Point2F(tc.x - 4, tc.y), D2D1::Point2F(tc.x + 4, tc.y), tb, 1.4f);
                        target_->DrawEllipse(D2D1::Ellipse(tc, 5.0f, 5.0f), tb, 1.2f);
                    } else {
                        target_->DrawRectangle(D2D1::RectF(tc.x - 4.5f, tc.y - 6.0f, tc.x + 4.5f, tc.y + 6.0f), tb, 1.2f);
                        target_->DrawLine(D2D1::Point2F(tc.x - 2.5f, tc.y - 2.0f), D2D1::Point2F(tc.x + 2.5f, tc.y - 2.0f), tb, 1.0f);
                        target_->DrawLine(D2D1::Point2F(tc.x - 2.5f, tc.y + 1.5f), D2D1::Point2F(tc.x + 2.5f, tc.y + 1.5f), tb, 1.0f);
                    }
                }
            }

            // Mini app icon badge on thumbnail corner (Image 1 style)
            if (item.appIcon) {
                const D2D1_RECT_F badgeRect = D2D1::RectF(thumb.right - 8.0f, thumb.bottom - 8.0f, thumb.right + 4.0f, thumb.bottom + 4.0f);
                DrawCoverFitBitmapPixels(*item.appIcon, badgeRect, 2.0f, clipThumbBmp_[7], clipThumbGen_[7], 0.95f);
            }

            // Title
            SText(item.title, D2D1::RectF(row.left + 38.0f, row.top + 2.0f, row.right - 4.0f, row.top + 16.0f), SFLabel, white);

            // Subtitle: "932 x 455 · Just now"
            std::wstring sub;
            if (item.kind == ClipKind::Image) {
                sub = std::to_wstring(item.width) + L" × " + std::to_wstring(item.height);
            } else {
                sub = std::to_wstring(item.text.size()) + L" chars";
            }
            const double dt = NowSeconds() - item.copiedAt;
            std::wstring timeStr = L"Just now";
            if (dt >= 86400.0) timeStr = std::to_wstring(static_cast<int>(dt / 86400.0)) + L"d ago";
            else if (dt >= 3600.0) timeStr = std::to_wstring(static_cast<int>(dt / 3600.0)) + L"h ago";
            else if (dt >= 60.0) timeStr = std::to_wstring(static_cast<int>(dt / 60.0)) + L"m ago";
            sub += L" · " + timeStr;

            SText(sub, D2D1::RectF(row.left + 38.0f, row.top + 17.0f, row.right - 4.0f, row.bottom - 1.0f), SFSmall, grey);

            SHit(row, StudioAction::ClipSelect, static_cast<int>(i));
        }

        // ── Vertical Divider ──
        SLine(D2D1::Point2F(rightX, r.top + 4.0f), D2D1::Point2F(rightX, r.bottom - 4.0f), SCol(0x28282a), 1.0f);

        // ── Right Panel: Detail & Actions ──
        const float prL = rightX + 12.0f;
        const float prR = r.right - 4.0f;

        if (indices.empty() || g_clipSelected < 0 || g_clipSelected >= static_cast<int>(indices.size())) {
            // Bunbun: only what you really copied shows here (no sample items).
            if (allCnt == 0) {
                SText(L"Nothing copied yet", D2D1::RectF(prL, r.top + 56.0f, prR, r.top + 76.0f), SFBody, white, 1);
                SText(L"Text, links and images you copy show up here.", D2D1::RectF(prL, r.top + 78.0f, prR, r.top + 96.0f), SFSmall, grey, 1);
            } else {
                SText(L"No items match filter", D2D1::RectF(prL, r.top + 60.0f, prR, r.top + 90.0f), SFBody, grey, 1);
            }
            return;
        }

        const size_t selRealIdx = indices[static_cast<size_t>(g_clipSelected)];
        ClipItem cur;
        {
            std::lock_guard lock(g_stateMutex);
            if (selRealIdx < g_richClipItems.size()) cur = g_richClipItems[selRealIdx];
        }

        // Header: Kind · Relative time
        const double dt = NowSeconds() - cur.copiedAt;
        std::wstring relTime = L"Just now";
        if (dt >= 86400.0) relTime = std::to_wstring(static_cast<int>(dt / 86400.0)) + L"d ago";
        else if (dt >= 3600.0) relTime = std::to_wstring(static_cast<int>(dt / 3600.0)) + L"h ago";
        else if (dt >= 60.0) relTime = std::to_wstring(static_cast<int>(dt / 60.0)) + L"m ago";

        std::wstring headStr = std::wstring(cur.kind == ClipKind::Image ? L"Image" : cur.kind == ClipKind::Link ? L"Link" : L"Text") + L" · " + relTime;
        SText(headStr, D2D1::RectF(prL, r.top + 2.0f, prR - 50.0f, r.top + 18.0f), SFSmall, grey);

        // Pin Button
        const D2D1_RECT_F pinRect = D2D1::RectF(prR - 44.0f, r.top, prR - 24.0f, r.top + 18.0f);
        const D2D1_POINT_2F pc = D2D1::Point2F((pinRect.left + pinRect.right) * 0.5f, (pinRect.top + pinRect.bottom) * 0.5f);
        if (SHovered(StudioAction::ClipPin) || cur.pinned) SFillRound(D2D1::RectF(pc.x - 10.0f, pc.y - 10.0f, pc.x + 10.0f, pc.y + 10.0f), 6.0f, SCol(0xFFFFFF, 0.08f));
        bb::studio::Pushpin(SPen(), pc, 15.0f, cur.pinned ? accent : grey, cur.pinned);  // Bunbun: a real pin
        SHit(pinRect, StudioAction::ClipPin, g_clipSelected);

        // Trash Button
        const D2D1_RECT_F trashRect = D2D1::RectF(prR - 20.0f, r.top, prR, r.top + 18.0f);
        const D2D1_POINT_2F tc = D2D1::Point2F((trashRect.left + trashRect.right) * 0.5f, (trashRect.top + trashRect.bottom) * 0.5f);
        if (SHovered(StudioAction::ClipDelete)) SFillRound(D2D1::RectF(tc.x - 10.0f, tc.y - 10.0f, tc.x + 10.0f, tc.y + 10.0f), 6.0f, SCol(0xFFFFFF, 0.08f));
        bb::studio::Bin(SPen(), tc, 15.0f, SHovered(StudioAction::ClipDelete) ? SCol(0xFF6961) : grey);
        SHit(trashRect, StudioAction::ClipDelete, g_clipSelected);

        // Preview Box
        const D2D1_RECT_F pBox = D2D1::RectF(prL, r.top + 22.0f, prR, r.bottom - 36.0f);
        SFillRound(pBox, 10.0f, cardBg);

        if (cur.kind == ClipKind::Image && !cur.image.bgra.empty()) {
            const float boxW = pBox.right - pBox.left - 8.0f;
            const float boxH = pBox.bottom - pBox.top - 8.0f;
            if (boxW > 1.0f && boxH > 1.0f && cur.image.width > 0 && cur.image.height > 0) {
                const float scale = std::min(boxW / cur.image.width, boxH / cur.image.height);
                const float iw = cur.image.width * scale;
                const float ih = cur.image.height * scale;
                const float ix = (pBox.left + pBox.right) * 0.5f - iw * 0.5f;
                const float iy = (pBox.top + pBox.bottom) * 0.5f - ih * 0.5f;
                DrawRoundedBitmapPixels(cur.image, D2D1::RectF(ix, iy, ix + iw, iy + ih), 6.0f, clipPreviewBmp_, clipPreviewGen_, 1.0f);
            }
        } else {
            const D2D1_RECT_F txtR = D2D1::RectF(pBox.left + 12.0f, pBox.top + 10.0f, pBox.right - 12.0f, pBox.bottom - 10.0f);
            SText(cur.text, txtR, SFBody, white);
        }

        // Footer Metadata: "937 x 446 · 216.6 KB"
        std::wstring footStr;
        if (cur.kind == ClipKind::Image) {
            wchar_t sz[32];
            swprintf(sz, 32, L"%.1f KB", cur.byteSize / 1024.0);
            footStr = std::to_wstring(cur.width) + L" × " + std::to_wstring(cur.height) + L" · " + sz;
        } else {
            footStr = std::to_wstring(cur.text.size()) + L" chars";
        }
        SText(footStr, D2D1::RectF(prL, r.bottom - 28.0f, prR - 85.0f, r.bottom - 4.0f), SFSmall, grey);

        // Action Buttons
        // White Pill Copy Button
        const D2D1_RECT_F btnCopy = D2D1::RectF(prR - 64.0f, r.bottom - 30.0f, prR, r.bottom - 6.0f);
        SFillRound(btnCopy, 12.0f, white);
        const D2D1_POINT_2F ic = D2D1::Point2F(btnCopy.left + 14.0f, (btnCopy.top + btnCopy.bottom) * 0.5f);
        if (ID2D1SolidColorBrush* cb = AcquireBrush(SCol(0x000000))) {
            target_->DrawRectangle(D2D1::RectF(ic.x - 4.0f, ic.y - 4.5f, ic.x + 4.0f, ic.y + 4.5f), cb, 1.2f);
            target_->DrawLine(D2D1::Point2F(ic.x - 2.0f, ic.y - 5.5f), D2D1::Point2F(ic.x + 2.0f, ic.y - 5.5f), cb, 1.2f);
        }
        SText(L"Copy", D2D1::RectF(btnCopy.left + 20.0f, btnCopy.top, btnCopy.right - 4.0f, btnCopy.bottom), SFLabel, SCol(0x000000), 1);
        SHit(btnCopy, StudioAction::ClipCopy, g_clipSelected);

        // Save Button (Floppy disk / download icon)
        const D2D1_RECT_F btnSave = D2D1::RectF(prR - 92.0f, r.bottom - 28.0f, prR - 70.0f, r.bottom - 6.0f);
        const D2D1_POINT_2F discC = D2D1::Point2F((btnSave.left + btnSave.right) * 0.5f, (btnSave.top + btnSave.bottom) * 0.5f);
        if (ID2D1SolidColorBrush* db = AcquireBrush(white)) {
            target_->DrawRectangle(D2D1::RectF(discC.x - 5.0f, discC.y - 5.0f, discC.x + 5.0f, discC.y + 5.0f), db, 1.2f);
            target_->DrawLine(D2D1::Point2F(discC.x - 2.5f, discC.y - 5.0f), D2D1::Point2F(discC.x - 2.5f, discC.y - 1.5f), db, 1.2f);
            target_->DrawLine(D2D1::Point2F(discC.x + 2.5f, discC.y - 5.0f), D2D1::Point2F(discC.x + 2.5f, discC.y - 1.5f), db, 1.2f);
        }
        SHit(btnSave, StudioAction::ClipSave, g_clipSelected);
    }

    // ── Screen Time page (screentime_module.inc) ──
    // Today (time in front, what's left of your daily limit or your daily
    // average, apps used), the last seven days as bars, and today's apps with
    // their own icons and a bar for their share.
    std::map<std::wstring, std::pair<ComPtr<ID2D1Bitmap>, uint64_t>> screenTimeIcons_;  // key -> bitmap, its pixels' generation

    void DrawStudioScreenTimePage(const SharedState& state, D2D1_RECT_F r, const Settings& settings, double now) {
        (void)state;
        (void)now;
        namespace bst = bb::screentime;
        EnsureRoundStroke();
        studioOrigin_ = publishedOrigin_;
        const D2D1_COLOR_F white = SCol(0xffffff), grey = SCol(0x8e8e93), cardBg = SCol(0x18181a), accent = SAccent();
        const screentime::View view = screentime::GetView();
        const bst::Summary& sum = view.summary;

        // Bunbun: the columns share the page's real width (Classic's page is narrower
        // than Studio's), so the By app list always keeps room for its names and times.
        const float gap = 6.0f;
        const float leftW = std::min(304.0f, std::floor((r.right - r.left) * 0.6f));
        auto barsGlyph = [&](D2D1_POINT_2F c) {  // the little chart beside a card's title
            if (ID2D1SolidColorBrush* b = AcquireBrush(grey)) {
                target_->DrawLine(D2D1::Point2F(c.x - 3.0f, c.y + 3.0f), D2D1::Point2F(c.x - 3.0f, c.y - 1.0f), b, 1.2f);
                target_->DrawLine(D2D1::Point2F(c.x, c.y + 4.0f), D2D1::Point2F(c.x, c.y - 3.5f), b, 1.2f);
                target_->DrawLine(D2D1::Point2F(c.x + 3.0f, c.y + 3.0f), D2D1::Point2F(c.x + 3.0f, c.y), b, 1.2f);
            }
        };

        // ── Today ──
        const D2D1_RECT_F r1 = D2D1::RectF(r.left, r.top, r.left + leftW, r.top + 76.0f);
        SFillRound(r1, 12.0f, cardBg);
        {
            barsGlyph(D2D1::Point2F(r1.left + 16.0f, r1.top + 14.0f));
            // Bunbun: a clicked bar shows that day ("Mon 9 Nov"); a click on it again goes back to today.
            std::wstring dayTitle = L"Today";
            if (view.shownDay) {
                static const wchar_t* kWd[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
                static const wchar_t* kMo[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
                dayTitle = std::wstring(kWd[bst::Weekday(view.shownDay) % 7]) + L" " + std::to_wstring(view.shownDay % 100) + L" " +
                           kMo[std::clamp(view.shownDay / 100 % 100 - 1, 0, 11)];
            }
            SText(dayTitle, D2D1::RectF(r1.left + 24.0f, r1.top + 8.0f, r1.left + 140.0f, r1.top + 22.0f), SFSmall, view.shownDay ? accent : grey);
            const float cw = (leftW - 24.0f) / 3.0f;  // Bunbun: three even columns from x + 16
            const bool tight = cw < 84.0f;             // Classic's narrower page: shorter labels
            // Bunbun: a figure that doesn't fit its column steps down a size ("2h 12m" ran into the next one).
            auto figure = [&](const std::wstring& s, float l, float rgt) {
                const StudioFontId f = SMeasure(s, SFBig) <= rgt - l - 6.0f ? SFBig : SFSong;
                SText(s, D2D1::RectF(l, r1.top + (f == SFBig ? 24.0f : 30.0f), rgt, r1.top + 54.0f), f, white);
            };
            figure(bst::Duration(sum.todaySeconds), r1.left + 16.0f, r1.left + 16.0f + cw);
            SText(tight ? (view.shownDay ? L"That day" : L"Today") : L"Screen time", D2D1::RectF(r1.left + 16.0f, r1.top + 54.0f, r1.left + 16.0f + cw, r1.top + 68.0f), SFSmall, grey);

            // The middle: what's left of your daily limit (Bunbun.ScreenTimeLimit), with a
            // ring; without a limit, your daily average this week. A click changes the limit.
            const float mx = r1.left + 16.0f + cw;
            const long long limit = static_cast<long long>(settings.screenTimeLimit) * 3600;
            std::wstring big, small;
            if (limit > 0 && !view.shownDay) {
                const D2D1_POINT_2F rc = D2D1::Point2F(mx + 6.0f, r1.top + 40.0f);
                const float frac = Clamp(static_cast<float>(sum.todaySeconds) / static_cast<float>(limit), 0.0f, 1.0f);
                if (ID2D1SolidColorBrush* b = AcquireBrush(D2D1::ColorF(1, 1, 1, 0.18f))) target_->DrawEllipse(D2D1::Ellipse(rc, 11.0f, 11.0f), b, 2.2f);
                if (frac > 0.0f)
                    bb::studio::Arc(SPen(), rc, 11.0f, -1.5707963f, -1.5707963f + 6.2831853f * frac,
                                    sum.todaySeconds >= limit ? SCol(0xff453a) : white, 2.2f);
                const bool over = sum.todaySeconds >= limit;
                big = bst::Duration(over ? sum.todaySeconds - limit : limit - sum.todaySeconds);
                small = tight ? std::wstring(over ? L"over " : L"left of ") + std::to_wstring(settings.screenTimeLimit) + L"h"
                              : (over ? L"over your " : L"left of your ") + std::to_wstring(settings.screenTimeLimit) + L"h limit";
            } else {
                big = bst::Duration(sum.averageSeconds);
                small = tight ? L"Average" : L"Daily average";
            }
            const float tx = limit > 0 && !view.shownDay ? mx + 24.0f : mx;
            const float c3 = r1.left + 16.0f + cw * 2.0f;
            figure(big, tx, c3);
            SText(small, D2D1::RectF(tx, r1.top + 54.0f, c3, r1.top + 68.0f), SFSmall, grey);
            SHit(D2D1::RectF(mx - 4.0f, r1.top + 22.0f, c3, r1.top + 70.0f), StudioAction::ScreenTimeLimitEdit);

            figure(std::to_wstring(sum.appsUsed), c3, r1.right - 8.0f);
            SText(tight ? L"Apps" : L"Apps used", D2D1::RectF(c3, r1.top + 54.0f, r1.right - 8.0f, r1.top + 68.0f), SFSmall, grey);
        }

        // ── The last seven days ──
        const D2D1_RECT_F r2 = D2D1::RectF(r.left, r.top + 82.0f, r.left + leftW, r.bottom);
        SFillRound(r2, 12.0f, cardBg);
        {
            const D2D1_POINT_2F cc = D2D1::Point2F(r2.left + 16.0f, r2.top + 14.0f);
            if (ID2D1SolidColorBrush* b = AcquireBrush(grey)) {
                target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cc.x - 4.5f, cc.y - 4.5f, cc.x + 4.5f, cc.y + 4.5f), 1.5f, 1.5f), b, 1.2f);
                target_->DrawLine(D2D1::Point2F(cc.x - 4.5f, cc.y - 1.5f), D2D1::Point2F(cc.x + 4.5f, cc.y - 1.5f), b, 1.0f);
            }
            SText(L"Last 7 days", D2D1::RectF(r2.left + 25.0f, r2.top + 8.0f, r2.left + 140.0f, r2.top + 22.0f), SFSmall, grey);
            if (sum.averageSeconds > 0)
                SText(L"avg " + bst::Duration(sum.averageSeconds), D2D1::RectF(r2.right - 110.0f, r2.top + 8.0f, r2.right - 14.0f, r2.top + 22.0f),
                      SFSmall, grey, 2);
            static const wchar_t* kDays[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
            long long most = 1;
            for (const bst::DayTotal& d : sum.week) most = std::max(most, d.seconds);
            const float dw = (leftW - 20.0f) / 7.0f, baseline = r2.bottom - 22.0f, tallest = baseline - (r2.top + 46.0f);
            for (size_t i = 0; i < sum.week.size() && i < 7; ++i) {
                const bst::DayTotal& d = sum.week[i];
                const float dx = r2.left + 10.0f + i * dw, cx = dx + dw * 0.5f;
                const bool shown = view.shownDay ? d.day == view.shownDay : d.today;  // Bunbun: the day the cards show
                const float hov = SHover(StudioAction::ScreenTimeDay, d.day);
                if (hov > 0.01f) SFillRound(D2D1::RectF(dx + 2.0f, r2.top + 28.0f, dx + dw - 2.0f, r2.bottom - 2.0f), 8.0f, D2D1::ColorF(1, 1, 1, 0.05f * hov));
                SHit(D2D1::RectF(dx, r2.top + 28.0f, dx + dw, r2.bottom), StudioAction::ScreenTimeDay, d.day);
                SText(kDays[d.weekday % 7], D2D1::RectF(dx, r2.bottom - 18.0f, dx + dw, r2.bottom - 4.0f), SFSmall, shown ? white : grey, 1);
                if (d.seconds >= 60) {
                    // Bars scale to the busiest day; today's is white, the others grey.
                    const float h = std::max(6.0f, tallest * static_cast<float>(d.seconds) / static_cast<float>(most));
                    SFillRound(D2D1::RectF(cx - 9.0f, baseline - h, cx + 9.0f, baseline), 4.5f, shown ? white : SCol(0x8e8e93, 0.55f));
                    const long long m = d.seconds / 60;
                    SText(m >= 60 ? std::to_wstring(m / 60) + L"h" : std::to_wstring(m) + L"m",
                          D2D1::RectF(dx - 2.0f, baseline - h - 16.0f, dx + dw + 2.0f, baseline - h - 2.0f), SFSmall, shown ? white : grey, 1);
                } else {
                    SFillRound(D2D1::RectF(cx - 8.0f, baseline - 1.5f, cx + 8.0f, baseline + 1.5f), 1.0f, SCol(0x38383a));
                }
            }
        }

        // ── By app ──
        const D2D1_RECT_F r3 = D2D1::RectF(r.left + leftW + gap, r.top, r.right, r.bottom);
        SFillRound(r3, 12.0f, cardBg);
        barsGlyph(D2D1::Point2F(r3.left + 16.0f, r3.top + 14.0f));
        SText(L"By app", D2D1::RectF(r3.left + 24.0f, r3.top + 8.0f, r3.left + 90.0f, r3.top + 22.0f), SFSmall, grey);
        if (sum.apps.empty()) {
            SText(view.shownDay ? L"Nothing that day" : settings.screenTime ? L"Counting from now" : L"Screen Time is off",
                  D2D1::RectF(r3.left + 14.0f, r3.top + 60.0f, r3.right - 14.0f, r3.top + 80.0f), SFBody, white, 1);
            SText(L"Apps show here as you use them", D2D1::RectF(r3.left + 14.0f, r3.top + 82.0f, r3.right - 14.0f, r3.top + 98.0f),
                  SFSmall, grey, 1);
            return;
        }
        const float rowH = 28.0f, listY = r3.top + 28.0f;
        for (size_t i = 0; i < sum.apps.size() && i < 5; ++i) {
            const bst::AppTime& app = sum.apps[i];
            const float y = listY + i * (rowH + 2.0f);
            if (y + rowH > r3.bottom - 2.0f) break;  // only as many rows as the card holds
            const D2D1_RECT_F icBox = D2D1::RectF(r3.left + 12.0f, y + 2.0f, r3.left + 32.0f, y + 22.0f);
            // The app's own icon; while it's being looked up, its initial on a tile.
            const PixelsRef& icon = i < view.icons.size() ? view.icons[i] : PixelsRef();
            if (icon && !icon->bgra.empty()) {
                auto& slot = screenTimeIcons_[app.key];
                DrawBitmapPixels(*icon, icBox, slot.first, slot.second);
            } else {
                SFillRound(icBox, 5.0f, SCol(0x3a3a3c));
                SText(app.name.substr(0, 1), icBox, SFLabel, white, 1);
            }
            SText(app.name, D2D1::RectF(r3.left + 38.0f, y, r3.right - 50.0f, y + 14.0f), SFLabel, white);
            SText(bst::Duration(app.seconds), D2D1::RectF(r3.right - 54.0f, y, r3.right - 10.0f, y + 14.0f), SFLabel, white, 2);
            const float barL = r3.left + 38.0f, barR = r3.right - 12.0f, barY = y + 16.5f;
            SFillRound(D2D1::RectF(barL, barY, barR, barY + 2.5f), 1.2f, SCol(0x28282a));
            const float fW = (barR - barL) * Clamp(app.percent / 100.0f, 0.0f, 1.0f);
            if (fW > 1.0f) SFillRound(D2D1::RectF(barL, barY, barL + fW, barY + 2.5f), 1.2f, i == 0 ? accent : white);
        }
        if (screenTimeIcons_.size() > 64) screenTimeIcons_.clear();  // apps come and go; keep the cache small
    }

    void DrawStudioNotePage(D2D1_RECT_F r, double now) {
        const std::vector<std::wstring>& lines = QuickNote::Lines(now);
        const D2D1_RECT_F edit = D2D1::RectF(r.right - 52, r.top + 1, r.right, r.top + 19);
        SFillRound(edit, 9, WithAlpha(SAccent(), SLerp(0.18f, 0.3f, SHover(StudioAction::NoteEdit))));
        SText(L"Edit", edit, SFLabel, SAccent(), 1);
        SHit(edit, StudioAction::NoteEdit);
        if (lines.empty()) {
            SText(L"No note yet", D2D1::RectF(r.left, r.top, r.right - 60, r.top + 20), SFSong, SCol(0xffffff));
            SText(L"Click Edit to write one. Lines that start with [ ] get a checkbox.",
                  D2D1::RectF(r.left, r.top + 40, r.right, r.top + 60), SFBody, SCol(0x636366));
            return;
        }
        float y = r.top;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::wstring& line = lines[i];
            if (i == 0) {
                SText(line, D2D1::RectF(r.left, y, r.right - 60, y + 20), SFSong, SCol(0xffffff));
                y += 26;
                continue;
            }
            const float h = line.empty() ? 8.0f : 20.0f;
            if (y + h > r.bottom + 2) break;
            bool checked = false;
            if (QuickNote::IsCheckbox(line, &checked)) {
                const D2D1_POINT_2F c = D2D1::Point2F(r.left + 8, y + 10);
                const bool hover = SHovered(StudioAction::NoteToggle, static_cast<int>(i));
                if (ID2D1SolidColorBrush* b = AcquireBrush(checked ? SAccent() : D2D1::ColorF(1, 1, 1, hover ? 0.9f : 0.5f))) {
                    if (checked) target_->FillEllipse(D2D1::Ellipse(c, 7, 7), b);
                    else target_->DrawEllipse(D2D1::Ellipse(c, 7, 7), b, 1.5f);
                }
                if (checked) {
                    auto P = [](float x, float yy) { return D2D1::Point2F(x, yy); };
                    SPoly({P(8.5f, 12), P(11, 14.5f), P(15.5f, 9.5f)}, c, 1.0f, SCol(0x000000), 1.8f);
                }
                const std::wstring text = line.substr(line.find(L']') + 1);
                SText(text, D2D1::RectF(r.left + 22, y, r.right, y + 20), SFBody,
                      checked ? SCol(0x636366) : SCol(0xffffff));
                SHit(D2D1::RectF(r.left, y, r.right, y + 20), StudioAction::NoteToggle, static_cast<int>(i));
            } else if (line.rfind(L"- ", 0) == 0) {
                if (ID2D1SolidColorBrush* b = AcquireBrush(SAccent()))
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 8, y + 10), 2.5f, 2.5f), b);
                SText(line.substr(2), D2D1::RectF(r.left + 22, y, r.right, y + 20), SFBody, SCol(0xffffff));
            } else {
                SText(line, D2D1::RectF(r.left, y, r.right, y + 20), SFBody, SCol(0xaeaeb2));
            }
            y += h;
        }
    }

    // ── Mirror page ──
    ComPtr<ID2D1Bitmap> mirrorBmp_;
    ComPtr<ID2D1Bitmap> clipThumbBmp_[8];
    uint64_t clipThumbGen_[8] = {};
    ComPtr<ID2D1Bitmap> clipPreviewBmp_;
    uint64_t clipPreviewGen_ = 0;
    uint64_t mirrorBmpId_ = 0;

    void DrawStudioMirrorPage(D2D1_RECT_F r) {
        EnsureRoundStroke();
        const D2D1_COLOR_F sky = SAccent();
        if (!Mirror::g_running.load()) {
            mirrorBmp_.Reset();
            std::wstring err;
            {
                std::lock_guard lock(Mirror::g_mutex);
                err = Mirror::g_error;
            }
            const float cx = (r.left + r.right) * 0.5f, cy = r.top + 60.0f;  // the sound panel takes the bottom 78 px
            SIcon(StudioPage::Mirror, D2D1::Point2F(cx, cy - 42), 30.0f, sky);
            const D2D1_RECT_F b = D2D1::RectF(cx - 74, cy - 10, cx + 74, cy + 22);
            SFillRound(b, 16, D2D1::ColorF(1, 1, 1, SLerp(0.92f, 1.0f, SHover(StudioAction::MirrorStart))));
            SText(L"Turn on camera", b, SFLabel, SCol(0x141416), 1);
            SHit(b, StudioAction::MirrorStart);
            SText(err.empty() ? std::wstring(L"Only while this page is open. Nothing is saved.") : err,
                  D2D1::RectF(r.left, cy + 28, r.right, cy + 44), SFSmall, err.empty() ? SCol(0x8e8e93) : SCol(0xff6961), 1);
            DrawSoundPanel(r, false);
            return;
        }
        Mirror::KeepAlive();
        {
            std::lock_guard lock(Mirror::g_mutex);
            if (Mirror::g_frameId != mirrorBmpId_ && !Mirror::g_frame.empty()) {
                mirrorBmpId_ = Mirror::g_frameId;
                const D2D1_SIZE_U size = D2D1::SizeU(Mirror::g_width, Mirror::g_height);
                const D2D1_BITMAP_PROPERTIES props =
                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                if (!mirrorBmp_ || mirrorBmp_->GetPixelSize().width != size.width || mirrorBmp_->GetPixelSize().height != size.height) {
                    mirrorBmp_.Reset();
                    target_->CreateBitmap(size, Mirror::g_frame.data(), size.width * 4, props, &mirrorBmp_);
                } else {
                    mirrorBmp_->CopyFromMemory(nullptr, Mirror::g_frame.data(), size.width * 4);
                }
            }
        }
        const D2D1_RECT_F view = D2D1::RectF(r.left, r.top, r.right, r.bottom);
        SFillRound(view, 16, SCol(0x000000));
        if (mirrorBmp_) {
            // Cover-fit, flipped like a mirror, clipped to a rounded frame.
            const D2D1_SIZE_F src = mirrorBmp_->GetSize();
            const float vw = view.right - view.left, vh = view.bottom - view.top;
            const float scale = std::max(vw / src.width, vh / src.height);
            const float dw = src.width * scale, dh = src.height * scale;
            const D2D1_RECT_F dst = D2D1::RectF((view.left + view.right - dw) * 0.5f, (view.top + view.bottom - dh) * 0.5f,
                                                (view.left + view.right + dw) * 0.5f, (view.top + view.bottom + dh) * 0.5f);
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            d2dFactory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(view, 16, 16), &clip);
            ID2D1Layer* layer = AcquireLayer();
            if (clip && layer) target_->PushLayer(D2D1::LayerParameters(view, clip.Get()), layer);
            D2D1_MATRIX_3X2_F old;
            target_->GetTransform(&old);
            const float mid = (view.left + view.right) * 0.5f;
            target_->SetTransform(D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(mid, 0)) * old);
            target_->DrawBitmap(mirrorBmp_.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            target_->SetTransform(old);
            if (clip && layer) target_->PopLayer();
        } else {
            SText(L"Starting the camera\x2026", view, SFBody, SCol(0x8e8e93), 1);
        }
        const D2D1_RECT_F off = D2D1::RectF(view.right - 96, view.top + 8, view.right - 8, view.top + 34);
        SFillRound(off, 13, D2D1::ColorF(0, 0, 0, SLerp(0.55f, 0.75f, SHover(StudioAction::MirrorStop))));
        SText(L"Turn off", off, SFLabel, SCol(0xffffff), 1);
        SHit(off, StudioAction::MirrorStop);
        DrawSoundPanel(r, true);
    }

    // ── All apps page ──
    // Every page of the island, plus the colour picker and Settings, in one grid.
    void DrawStudioAppsPage(const SharedState& state, D2D1_RECT_F r) {
        EnsureRoundStroke();
        struct App { StudioPage page; int tab; StudioAction action; const wchar_t* name; };
        std::vector<App> apps;
        const Settings& s = g_settings;
        const int ctx = g_dashboardContext.load();
        for (int id : DashboardTabIds(ctx, s)) {
            const StudioPage p = StudioPageFor(ctx, id);
            if (p == StudioPage::Apps) continue;
            apps.push_back({p, id, StudioAction::OpenTab, p == StudioPage::Work && state.work.study ? L"Study" : StudioTabLabel(p)});
        }
        apps.push_back({StudioPage::Home, -1, StudioAction::ColorPicker, L"Colour"});
        apps.push_back({StudioPage::Home, -2, StudioAction::Settings, L"Settings"});

        const int cols = apps.size() > 12 ? 8 : 6;  // Bunbun: room for every page (it stopped at 12)
        const float cellW = (r.right - r.left) / cols;
        const float cellH = 84.0f;
        const bool waiting = state.agent.phase == bb::AgentPhase::Approval || state.agent.phase == bb::AgentPhase::Question;
        size_t trayCount = state.fileTrayItems.size();
        for (size_t i = 0; i < apps.size() && i < static_cast<size_t>(cols) * 2; ++i) {
            const float x = r.left + (i % cols) * cellW;
            const float y = r.top + 4 + (i / cols) * cellH;
            // Bunbun: smaller tiles when the columns are narrow (Classic's page with every page listed).
            const float half = std::min(22.0f, cellW * 0.5f - 9.0f);
            const D2D1_RECT_F icon = D2D1::RectF(x + cellW * 0.5f - half, y + 22 - half, x + cellW * 0.5f + half, y + 22 + half);
            const App& app = apps[i];
            const int arg = app.action == StudioAction::OpenTab ? app.tab : 0;
            const float hover = SHover(app.action, app.action == StudioAction::OpenTab ? arg : INT_MIN);
            SFillRound(icon, 10, SLerpCol(SCol(0x1c1c1e), SCol(0x3a3a3c), hover));
            const D2D1_POINT_2F c = D2D1::Point2F((icon.left + icon.right) * 0.5f, (icon.top + icon.bottom) * 0.5f);
            const D2D1_COLOR_F ink = SCol(0xe8e8ee);
            if (app.tab == -1) SEyedropper(c, std::min(20.0f, half), ink);
            else if (app.tab == -2) SGear(c, std::min(20.0f, half), ink);
            else SIcon(app.page, c, std::min(20.0f, half), ink);
            std::wstring label = app.name;
            if (SMeasure(label, SFSmall) > cellW - 4) {  // Bunbun: short names in narrow columns
                if (app.page == StudioPage::Clipboard) label = L"Clips";
                else if (app.page == StudioPage::ScreenTime) label = L"Screen";
            }
            SText(label, D2D1::RectF(x, y + 48, x + cellW, y + 64), SFSmall, SCol(0xffffff), 1);
            // Badges: an agent waiting for you, files in the tray.
            int badge = 0;
            if (app.page == StudioPage::Agent && waiting) badge = 1;
            if (app.page == StudioPage::Tray && trayCount > 0) badge = static_cast<int>(trayCount);
            if (badge > 0) {
                const D2D1_POINT_2F bc = D2D1::Point2F(icon.right - 2, icon.top + 2);
                if (ID2D1SolidColorBrush* b = AcquireBrush(SCol(0xff4245))) target_->FillEllipse(D2D1::Ellipse(bc, 9, 9), b);
                SText(badge > 9 ? std::wstring(L"9+") : std::to_wstring(badge), D2D1::RectF(bc.x - 9, bc.y - 9, bc.x + 9, bc.y + 9),
                      SFSmall, SCol(0xffffff), 1);
            }
            SHit(D2D1::RectF(x + 4, y, x + cellW - 4, y + 66), app.action, arg);
        }
    }

    // ── Timers page (timers_module.inc) ──
    // One page for every timer: a mode bar on the top right, a big ring on the
    // left, the mode's controls to its right. Water draws its own page below the bar.
    // ── Work page (work_module.inc) ──
    // Left: the state, today's work against the goal and the buttons. Right: the
    // day's timeline (work, away, asleep, lunch, breaks, overtime) and its totals,
    // with Report for the full report in Settings.
    // Bunbun: worked time wears the accent; the other kinds keep their own colours
    // so a timeline still reads at a glance.
    D2D1_COLOR_F WorkKindColour(bb::work::Kind k) const {
        return k == bb::work::Kind::Work ? SAccent() : SCol(bb::work::KindColour(k));
    }

    D2D1_COLOR_F WorkPhaseColour(bb::work::Phase p) const {
        namespace wk = bb::work;
        switch (p) {
            case wk::Phase::Working: return SAccent();
            case wk::Phase::Overtime: return SCol(wk::KindColour(wk::Kind::Overtime));
            case wk::Phase::Away: return SCol(wk::KindColour(wk::Kind::Away));
            case wk::Phase::Asleep: return SCol(wk::KindColour(wk::Kind::Asleep));
            case wk::Phase::Lunch: return SCol(wk::KindColour(wk::Kind::Lunch));
            case wk::Phase::Break: return SCol(wk::KindColour(wk::Kind::Break));
            case wk::Phase::Done: return SCol(0x30d158);
            default: return SCol(0x8e8e93);
        }
    }

    // "Working · until 18:00", "Lunch · back at 13:45", "Break · 3 min left"…
    // Study: "Maths · until 11:00", "Free time · Physics at 14:00".
    static std::wstring WorkStatusLine(const WorkSnapshot& w, double now) {
        namespace wk = bb::work;
        (void)now;
        const auto phase = static_cast<wk::Phase>(w.phase);
        if (w.study) {
            if ((phase == wk::Phase::Working || phase == wk::Phase::Overtime) && !w.subject.empty())
                return w.blockEnd >= 0 ? w.subject + L" \x00B7 until " + wk::FormatClock(w.blockEnd) : w.subject;
            if (phase == wk::Phase::Off)
                return w.nextBlock >= 0 ? w.nextSubject + L" at " + wk::FormatClock(w.nextBlock) : std::wstring(L"Free time");
            // Bunbun: studying outside a class (by hand, or a Pomodoro): the work day's end doesn't apply.
            // The title already says Studying: the line says what's next.
            if (phase == wk::Phase::Working || phase == wk::Phase::Overtime)
                return w.nextBlock >= 0 ? L"Next: " + w.nextSubject + L" at " + wk::FormatClock(w.nextBlock) : std::wstring(L"Free study");
        }
        switch (phase) {
            case wk::Phase::Working: return L"Working \x00B7 until " + wk::FormatClock(w.end);
            case wk::Phase::Overtime: return L"Overtime";
            case wk::Phase::Lunch: return L"Lunch \x00B7 back at " + wk::FormatClock(w.lunchStart + w.lunchMinutes);
            case wk::Phase::Break: {
                const long long left = std::max(0LL, w.breakUntil - static_cast<long long>(time(nullptr)));
                return L"Break \x00B7 " + std::to_wstring((left + 59) / 60) + L" min left";
            }
            case wk::Phase::Away: return L"Away";
            case wk::Phase::Asleep: return L"Asleep";
            case wk::Phase::Paused: return L"Paused";
            case wk::Phase::Done: return L"Done for today";
            default: return w.autoStart ? L"Starts at " + wk::FormatClock(w.start) : std::wstring(L"Not started");
        }
    }

    void DrawStudioWorkPage(const SharedState& state, D2D1_RECT_F r, double now) {
        namespace wk = bb::work;
        EnsureRoundStroke();
        const WorkSnapshot& w = state.work;
        const auto phase = static_cast<wk::Phase>(w.phase);
        const D2D1_COLOR_F white = SCol(0xffffff), grey = SCol(0x8e8e93), cardBg = SCol(0x1c1c1e);
        const D2D1_COLOR_F col = WorkPhaseColour(phase);
        auto pill = [&](D2D1_RECT_F b, const std::wstring& label, bool primary, StudioAction action) {
            const float hover = SHover(action);
            SFillRound(b, (b.bottom - b.top) * 0.5f,
                       primary ? D2D1::ColorF(1, 1, 1, SLerp(0.92f, 1.0f, hover)) : D2D1::ColorF(1, 1, 1, SLerp(0.09f, 0.18f, hover)));
            SText(label, b, SFLabel, primary ? SCol(0x141416) : white, 1);
            SHit(b, action);
        };
        auto pillW = [&](const std::wstring& label) { return SMeasure(label, SFLabel) + 28.0f; };

        // ── Left: the state, the time, the buttons ──
        const float W = r.right - r.left;
        const float leftW = std::min(300.0f, std::floor(W * 0.4f));
        const D2D1_RECT_F lc = D2D1::RectF(r.left, r.top, r.left + leftW, r.bottom);
        SFillRound(lc, 14, cardBg);
        if (ID2D1SolidColorBrush* b = AcquireBrush(col)) target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(lc.left + 20, lc.top + 22), 4.5f, 4.5f), b);
        SText(w.study ? wk::StudyLabel(phase) : wk::PhaseLabel(phase), D2D1::RectF(lc.left + 32, lc.top + 12, lc.right - 12, lc.top + 32), SFLabel, white);
        if (w.study && w.streak > 0)  // Study: the streak of days that met the goal
            SText(L"\U0001F525 " + std::to_wstring(w.streak) + (w.streak == 1 ? L" day" : L" days"),
                  D2D1::RectF(lc.left + 120, lc.top + 12, lc.right - 14, lc.top + 32), SFLabel, SCol(0xff9f0a), 2);
        {   // Bunbun: past an hour ("1h 12m") the hero size no longer fits; a step down then.
            const std::wstring worked = wk::Duration(w.Worked());
            const D2D1_RECT_F at = D2D1::RectF(lc.left + 16, lc.top + 38, lc.right - 70, lc.top + 92);
            SText(worked, at, SMeasure(worked, SFHero) > at.right - at.left ? SFBig : SFHero, white);
        }
        const float frac = w.goal > 0 ? std::min(1.0f, static_cast<float>(w.Worked()) / w.goal) : 0.0f;
        SRing(D2D1::Point2F(lc.right - 40, lc.top + 62), 22, 5.0f, frac, col);
        SText(std::to_wstring(static_cast<int>(frac * 100.0f + 0.5f)) + L"%", D2D1::RectF(lc.right - 62, lc.top + 52, lc.right - 18, lc.top + 72),
              SFSmall, grey, 1);
        SText(w.goal > 0 ? L"of " + wk::Duration(w.goal) + L" today" : std::wstring(w.study ? L"studied today" : L"worked today"),
              D2D1::RectF(lc.left + 16, lc.top + 92, lc.right - 12, lc.top + 110), SFBody, grey);
        SText(WorkStatusLine(w, now), D2D1::RectF(lc.left + 16, lc.top + 112, lc.right - 12, lc.top + 130), SFBody, col);

        // Buttons for this state, on one row (the second goes when there's no room).
        std::vector<std::pair<std::wstring, StudioAction>> btns;
        switch (phase) {
            case wk::Phase::Working: case wk::Phase::Overtime: case wk::Phase::Away: case wk::Phase::Asleep:
                btns = {{L"Pause", StudioAction::WorkPause}, {L"Break", StudioAction::WorkBreak}, {L"Done", StudioAction::WorkDone}};
                break;
            case wk::Phase::Paused: case wk::Phase::Break:
                btns = {{L"Resume", StudioAction::WorkResume}, {L"Done", StudioAction::WorkDone}};
                break;
            case wk::Phase::Lunch:
                btns = {{L"Done for today", StudioAction::WorkDone}};
                break;
            case wk::Phase::Done:
                btns = {{L"Start again", StudioAction::WorkStart}};
                break;
            default:
                btns = {{w.study ? L"Start studying" : L"Start work", StudioAction::WorkStart}};
                break;
        }
        float bx = lc.left + 14;
        const float by = lc.bottom - 44;
        for (size_t i = 0; i < btns.size(); ++i) {
            const float bw = pillW(btns[i].first);
            if (bx + bw > lc.right - 12) break;
            pill(D2D1::RectF(bx, by, bx + bw, by + 30), btns[i].first, i == 0, btns[i].second);
            bx += bw + 8;
        }

        // ── Right: today's timeline and totals ──
        const D2D1_RECT_F rc = D2D1::RectF(lc.right + 10, r.top, r.right, r.bottom);
        SFillRound(rc, 14, cardBg);
        SText(L"Today", D2D1::RectF(rc.left + 16, rc.top + 12, rc.left + 120, rc.top + 30), SFLabel, white);
        const float repW = pillW(L"Report");
        pill(D2D1::RectF(rc.right - 14 - repW, rc.top + 8, rc.right - 14, rc.top + 34), L"Report", false, StudioAction::WorkReport);

        // The span: the scheduled hours, widened to whatever was recorded.
        int from = w.start * 60, to = (w.end <= w.start ? w.end + 1440 : w.end) * 60;
        if (w.study) { from = w.first >= 0 ? w.first : 8 * 3600; to = std::max(from + 3600, w.last); }
        if (w.first >= 0) from = std::min(from, w.first);
        if (w.last >= 0) to = std::max(to, w.last);
        if (to <= from) to = from + 3600;
        const D2D1_RECT_F bar = D2D1::RectF(rc.left + 16, rc.top + 46, rc.right - 16, rc.top + 66);
        SFillRound(bar, 6, D2D1::ColorF(1, 1, 1, 0.06f));
        const float bw = bar.right - bar.left;
        auto xAt = [&](int s) { return bar.left + bw * Clamp(static_cast<float>(s - from) / static_cast<float>(to - from), 0.0f, 1.0f); };
        for (const wk::Segment& g : w.segments) {
            const float x0 = xAt(g.start), x1 = std::max(xAt(g.end), x0 + 1.5f);
            SFillRound(D2D1::RectF(x0, bar.top, x1, bar.bottom), 3, WorkKindColour(g.kind));
        }
        // Now, as a thin line.
        SYSTEMTIME st{};
        GetLocalTime(&st);
        int nowSec = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
        if (w.workDate && nowSec < from && to > 86400) nowSec += 86400;
        if (nowSec >= from && nowSec <= to) {
            const float nx = xAt(nowSec);
            SFillRound(D2D1::RectF(nx - 1, bar.top - 4, nx + 1, bar.bottom + 4), 1, white);
        }
        SText(wk::FormatClock(from / 60), D2D1::RectF(bar.left, bar.bottom + 4, bar.left + 60, bar.bottom + 18), SFSmall, grey);
        SText(wk::FormatClock(to / 60), D2D1::RectF(bar.right - 60, bar.bottom + 4, bar.right, bar.bottom + 18), SFSmall, grey, 2);

        // Study: today's subjects, most studied first, as bars; then away and breaks.
        if (w.study) {
            const float gx = rc.left + 16, gy = bar.bottom + 28;
            const float wAll = rc.right - rc.left - 32;
            int most = 1;
            for (const auto& s : w.subjects) most = std::max(most, s.second);
            if (w.subjects.empty())
                SText(L"Subjects show here as you study.", D2D1::RectF(gx, gy, gx + wAll, gy + 18), SFBody, grey);
            static const uint32_t kSubject[] = {0x0a84ff, 0xbf5af2, 0x30d158, 0xff9f0a, 0x64d2ff};
            for (size_t i = 0; i < w.subjects.size() && i < 4; ++i) {
                const float y = gy + i * 22.0f;
                const std::wstring dur = wk::Duration(w.subjects[i].second);
                const float durW = SMeasure(dur, SFLabel) + 6.0f;
                SText(w.subjects[i].first, D2D1::RectF(gx, y - 2, gx + 110, y + 18), SFBody, white);
                const float bx0 = gx + 116, bx1 = rc.right - 16 - durW;
                SFillRound(D2D1::RectF(bx0, y + 4, bx1, y + 12), 4, D2D1::ColorF(1, 1, 1, 0.06f));
                SFillRound(D2D1::RectF(bx0, y + 4, bx0 + std::max(8.0f, (bx1 - bx0) * w.subjects[i].second / most), y + 12), 4, SCol(kSubject[i % 5]));
                SText(dur, D2D1::RectF(bx1 + 6, y - 2, rc.right - 16, y + 18), SFLabel, white, 2);
            }
            const std::wstring tail = L"Away " + wk::Duration(w.seconds[static_cast<int>(wk::Kind::Away)] + w.seconds[static_cast<int>(wk::Kind::Asleep)]) +
                                      L" \x00B7 Breaks " + std::to_wstring(w.breaks);
            SText(tail, D2D1::RectF(gx, rc.bottom - 26, rc.right - 16, rc.bottom - 8), SFSmall, grey);
            return;
        }
        // Totals: two columns of three.
        const wk::Kind kinds[] = {wk::Kind::Work, wk::Kind::Away, wk::Kind::Lunch, wk::Kind::Overtime, wk::Kind::Asleep, wk::Kind::Break};
        const float gx = rc.left + 16, gy = bar.bottom + 30;
        const float colW = (rc.right - rc.left - 32) / 2.0f;
        for (int i = 0; i < 6; ++i) {
            const wk::Kind k = kinds[i];
            const float x = gx + (i / 3) * colW, y = gy + (i % 3) * 26.0f;
            if (ID2D1SolidColorBrush* b = AcquireBrush(WorkKindColour(k)))
                target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y + 4, x + 10, y + 14), 3, 3), b);
            std::wstring label = wk::KindLabel(k);
            if (k == wk::Kind::Break) label += L" (" + std::to_wstring(w.breaks) + L")";
            // Bunbun: the label gets what the time doesn't need; a short form when it's narrow.
            const std::wstring dur = wk::Duration(w.seconds[static_cast<int>(k)]);
            const float durW = SMeasure(dur, SFLabel) + 6.0f;
            if (SMeasure(label, SFBody) > colW - 26.0f - durW) {
                if (k == wk::Kind::Work) label = L"Work";
                else if (k == wk::Kind::Overtime) label = L"Over";
                else if (k == wk::Kind::Break) label = L"Breaks";
            }
            SText(label, D2D1::RectF(x + 18, y - 2, x + colW - 4 - durW, y + 20), SFBody, grey);
            SText(dur, D2D1::RectF(x + colW - 4 - durW, y - 2, x + colW - 4, y + 20), SFLabel, white, 2);
        }
    }

    void DrawStudioTimersPage(const SharedState& state, D2D1_RECT_F r, double now) {
        namespace bt = bb::timers;
        EnsureRoundStroke();
        if (g_settings.focusGuard) EnsureFocusGuard();
        Timers::Load();
        const D2D1_COLOR_F accent = SAccent(), white = SCol(0xffffff), grey = SCol(0x8e8e93);
        const D2D1_COLOR_F green = SCol(0x30d158), red = SCol(0xff453a);
        const bool water = g_settings.waterPage;
        bt::Mode mode = Timers::g_mode;
        if (mode == bt::Mode::Water && !water) mode = bt::Mode::Focus;

        auto pill = [&](D2D1_RECT_F b, const std::wstring& label, bool primary, StudioAction action, int arg = 0) {
            const float hover = SHover(action, arg);
            SFillRound(b, (b.bottom - b.top) * 0.5f,
                       primary ? D2D1::ColorF(1, 1, 1, SLerp(0.92f, 1.0f, hover)) : D2D1::ColorF(1, 1, 1, SLerp(0.09f, 0.18f, hover)));
            SText(label, b, SFLabel, primary ? SCol(0x141416) : white, 1);
            SHit(b, action, arg);
        };
        // A row of pills from x, each as wide as its label; returns where the row ends.
        auto pillW = [&](const std::wstring& label) { return SMeasure(label, SFLabel) + 28.0f; };

        // ── The mode bar ──
        static const wchar_t* kNames[] = {L"Focus", L"Timer", L"Stopwatch", L"Water"};
        const int modes = water ? 4 : 3;
        float segW = 0.0f;
        for (int i = 0; i < modes; ++i) segW = std::max(segW, SMeasure(kNames[i], SFLabel) + 22.0f);
        const D2D1_RECT_F bar = D2D1::RectF(r.right - segW * modes - 4, r.top, r.right, r.top + 26);
        SFillRound(bar, 13, D2D1::ColorF(1, 1, 1, 0.07f));
        for (int i = 0; i < modes; ++i) {
            const D2D1_RECT_F seg = D2D1::RectF(bar.left + 2 + segW * i, bar.top + 2, bar.left + 2 + segW * (i + 1), bar.bottom - 2);
            const bool on = i == static_cast<int>(mode);
            const float hover = SHover(StudioAction::TimerMode, i);
            if (on) SFillRound(seg, 11, D2D1::ColorF(1, 1, 1, 0.92f));
            else if (hover > 0.01f) SFillRound(seg, 11, D2D1::ColorF(1, 1, 1, 0.08f * hover));
            SText(kNames[i], seg, SFLabel, on ? SCol(0x141416) : SLerpCol(SCol(0xaeaeb2), white, hover), 1);
            SHit(seg, StudioAction::TimerMode, i);
        }

        if (mode == bt::Mode::Water) {
            DrawStudioWaterPage(state, D2D1::RectF(r.left, r.top + 30, r.right, r.bottom), now);
            return;
        }

        // ── The ring ──
        const float H = r.bottom - r.top;
        float R = std::min(64.0f, (H - 14.0f) * 0.5f);
        const float ringW = 7.0f;
        D2D1_POINT_2F c = D2D1::Point2F(r.left + R + 6, r.top + H * 0.5f);
        // Bunbun: on a narrow page (Classic) the mode bar reaches over the ring; then the
        // ring sits below the bar, smaller, so the two never overlap.
        if (bar.left < c.x + R + 8.0f) {
            const float avail = r.bottom - (bar.bottom + 6.0f);
            R = std::max(30.0f, std::min(R, avail * 0.5f - 2.0f));
            c = D2D1::Point2F(r.left + R + 6, bar.bottom + 6.0f + avail * 0.5f);
        }
        const float x0 = c.x + R + 22;             // the controls' column
        const float y0 = r.top + 38;               // below the mode bar
        const TimerSnapshot& t = state.timer;
        const double left = t.running ? std::max(0.0, t.endsAt - now) : t.remainingAtPause;
        auto up = [](double s) { return std::ceil(s - 1e-6); };  // 4:59.2 left reads 5:00
        auto ringText = [&](const std::wstring& big, const std::wstring& small, D2D1_COLOR_F smallCol) {
            SText(big, D2D1::RectF(c.x - R, c.y - 22, c.x + R, c.y + 10), big.size() > 7 ? SFSong : SFBig, white, 1);
            SText(small, D2D1::RectF(c.x - R + 8, c.y + 12, c.x + R - 8, c.y + 28), SFSmall, smallCol, 1);
        };
        auto endsAt = [&](double seconds) {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            const int m = static_cast<int>((st.wHour * 3600 + st.wMinute * 60 + st.wSecond + seconds) / 60) % (24 * 60);
            wchar_t b[32];
            swprintf_s(b, L"Ends at %02d:%02d", m / 60, m % 60);
            return std::wstring(b);
        };

        if (mode == bt::Mode::Focus) {
            // ── Pomodoro ──
            // Bunbun: the ring on the left like the other modes; on the right the
            // phase, what's next, the buttons, and the old Focus page's length chips.
            // Every row is measured against the page's right edge so nothing clips.
            const bt::PomodoroConfig cfg = Timers::Config(g_settings);
            const bt::Pomodoro& p = Timers::g_pomo;
            const bool mine = t.active && t.kind == kTimerKindPomodoro;
            const int phaseSec = bt::PhaseMinutes(p.phase, cfg) * 60;
            const bool brk = p.phase != bt::Phase::Focus;
            const D2D1_COLOR_F col = brk ? green : accent;
            const float frac = mine && t.totalSeconds > 0 ? Clamp(1.0f - static_cast<float>(left / t.totalSeconds), 0.0f, 1.0f) : 0.0f;
            // Bunbun: before a focus session starts, the ring is a dial for its length, as on Timer.
            if (Timers::g_focusDrag > 0 && !Timers::g_ringFocus && Timers::g_focusDrag == cfg.focusMinutes)
                Timers::g_focusDrag = -1;  // the saved length has come back through the settings
            const bool dial = !mine && !brk && Timers::g_autoStartAt <= 0.0;
            if (dial) {
                const int setMin = Timers::g_focusDrag > 0 ? Timers::g_focusDrag : cfg.focusMinutes;
                const float df = static_cast<float>(bt::FocusDialFraction(setMin));
                const float hover = std::max(SHover(StudioAction::TimerRing, 1), Timers::g_ringFocus ? 1.0f : 0.0f);
                SRing(c, R, ringW, df, accent);
                const float a = df * 6.2831853f;
                const D2D1_POINT_2F k = D2D1::Point2F(c.x + R * std::sin(a), c.y - R * std::cos(a));
                if (ID2D1SolidColorBrush* b = AcquireBrush(white))
                    target_->FillEllipse(D2D1::Ellipse(k, SLerp(6.5f, 8.0f, hover), SLerp(6.5f, 8.0f, hover)), b);
                ringText(Clock(setMin * 60), L"Drag or scroll", grey);
                SHit(D2D1::RectF(c.x - R - 10, c.y - R - 10, c.x + R + 10, c.y + R + 10), StudioAction::TimerRing, 1);
            } else {
            SRing(c, R, ringW, frac, col);
            ringText(Clock(mine ? up(left) : phaseSec),
                     mine ? (t.running ? (brk ? L"Break" : L"Focus") : L"Paused")
                          : (Timers::g_autoStartAt > 0.0 ? L"Starting\x2026" : L"Ready"),
                     mine && t.running ? col : grey);
            }
            const float xr = r.right - 4.0f;   // keep clear of the page's edge

            // Row 1: the phase, then a dot per focus session of the cycle.
            const std::wstring phase = bt::PhaseLabel(p, cfg);
            const float phaseW = SMeasure(phase, SFSong);
            SText(phase, D2D1::RectF(x0, y0, xr, y0 + 20), SFSong, white);
            for (int i = 0; i < cfg.cycles && i < 12; ++i) {
                const D2D1_POINT_2F d = D2D1::Point2F(x0 + phaseW + 14 + i * 13.0f, y0 + 10);
                if (d.x + 5 > xr) break;
                const bool done = i < p.cycle - 1 || (i == p.cycle - 1 && brk);
                const bool current = i == p.cycle - 1 && !brk;
                if (ID2D1SolidColorBrush* b = AcquireBrush(done ? accent : (current ? WithAlpha(accent, 0.45f) : D2D1::ColorF(1, 1, 1, 0.14f))))
                    target_->FillEllipse(D2D1::Ellipse(d, 4.0f, 4.0f), b);
            }

            // Row 2: what comes next; the auto-start switch on the right.
            const float ny = y0 + 24;
            const std::wstring autoLabel = L"Auto-start";
            const float autoW = 26 + 7 + SMeasure(autoLabel, SFSmall) + 8;
            const float ax = xr - autoW;
            const bt::Phase next = bt::NextPhase(p, cfg);
            SText(L"Then " + std::wstring(bt::PhaseName(next)) + L" \x00B7 " + std::to_wstring(bt::PhaseMinutes(next, cfg)) + L" min",
                  D2D1::RectF(x0, ny, ax - 8, ny + 18), SFSmall, grey);
            const bool autoOn = cfg.autoStart;
            const D2D1_RECT_F sw = D2D1::RectF(ax, ny + 2, ax + 26, ny + 16);
            const float hover = SHover(StudioAction::PomoAuto);
            SFillRound(sw, 7, autoOn ? accent : D2D1::ColorF(1, 1, 1, SLerp(0.16f, 0.24f, hover)));
            if (ID2D1SolidColorBrush* b = AcquireBrush(white))
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(autoOn ? sw.right - 7 : sw.left + 7, ny + 9), 5, 5), b);
            SText(autoLabel, D2D1::RectF(sw.right + 7, ny - 1, xr + 4, ny + 19), SFSmall, SLerpCol(SCol(0xc7c7cc), white, hover));
            SHit(D2D1::RectF(ax, ny - 1, xr, ny + 19), StudioAction::PomoAuto);

            // Row 3: start / pause, skip, reset; today's count if there's room.
            const float by = y0 + 52;
            float bx = x0;
            // Bunbun: a narrow column (Classic) gets smaller buttons, so all three fit.
            const bool narrow = xr - x0 < 250.0f;
            const float goW = narrow ? 72.0f : 86.0f, skipW = narrow ? 50.0f : 58.0f, resetW = narrow ? 56.0f : 62.0f, sp = narrow ? 5.0f : 6.0f;
            const std::wstring go = mine ? (t.running ? L"Pause" : L"Resume") : L"Start";
            pill(D2D1::RectF(bx, by, bx + goW, by + 30), go, true, StudioAction::PomoToggle);
            bx += goW + sp;
            if (bx + skipW <= xr) { pill(D2D1::RectF(bx, by, bx + skipW, by + 30), L"Skip", false, StudioAction::PomoSkip); bx += skipW + sp; }
            if (bx + resetW <= xr) { pill(D2D1::RectF(bx, by, bx + resetW, by + 30), L"Reset", false, StudioAction::PomoReset); bx += resetW + sp; }
            const int n = p.sessionsToday;
            const std::wstring today = std::to_wstring(n) + L" today";
            if (bx + SMeasure(today, SFSmall) + 6 <= xr)
                SText(today, D2D1::RectF(bx, by + 6, xr, by + 24), SFSmall, grey, 2);

            // Row 4: the focus length, as on the old Focus page.
            const float cy = by + 40;
            if (cy + 24 <= r.bottom) {
                float chipX = x0;
                for (int m : {15, 25, 50, 90}) {
                    const std::wstring label = std::to_wstring(m) + (narrow ? L"m" : L" min");
                    const float cw = SMeasure(label, SFLabel) + (narrow ? 18.0f : 22.0f);
                    if (chipX + cw > xr) break;
                    const bool on = (Timers::g_focusDrag > 0 ? Timers::g_focusDrag : cfg.focusMinutes) == m;
                    const D2D1_RECT_F chip = D2D1::RectF(chipX, cy, chipX + cw, cy + 24);
                    SFillRound(chip, 12, on ? WithAlpha(accent, 0.35f) : D2D1::ColorF(1, 1, 1, SLerp(0.09f, 0.16f, SHover(StudioAction::PomoLength, m))));
                    SText(label, chip, SFLabel, on ? white : SCol(0xe8e8ee), 1);
                    SHit(chip, StudioAction::PomoLength, m);
                    chipX += cw + 6;
                }
            }
            return;
        }

        if (mode == bt::Mode::Countdown) {
            // ── Countdown ──
            const bool mine = t.active && t.kind == kTimerKindTimer;
            const int set = Timers::g_countSet;
            if (mine) {
                const float frac = t.totalSeconds > 0 ? Clamp(static_cast<float>(left / t.totalSeconds), 0.0f, 1.0f) : 0.0f;
                SRing(c, R, ringW, frac, accent);
                ringText(Clock(up(left)), t.running ? endsAt(left) : L"Paused", t.running ? accent : grey);
            } else {
                // The dial: one turn an hour, a knob to drag; the wheel works over it too.
                const float frac = static_cast<float>(bt::DialFraction(set));
                const float hover = SHover(StudioAction::TimerRing);
                SRing(c, R, ringW, frac, accent);
                const float a = frac * 6.2831853f;
                const D2D1_POINT_2F k = D2D1::Point2F(c.x + R * std::sin(a), c.y - R * std::cos(a));
                if (ID2D1SolidColorBrush* b = AcquireBrush(white))
                    target_->FillEllipse(D2D1::Ellipse(k, SLerp(6.5f, 8.0f, std::max(hover, Timers::g_ringDrag ? 1.0f : 0.0f)),
                                                       SLerp(6.5f, 8.0f, std::max(hover, Timers::g_ringDrag ? 1.0f : 0.0f))), b);
                ringText(Clock(set), L"Drag or scroll", grey);
                SHit(D2D1::RectF(c.x - R - 10, c.y - R - 10, c.x + R + 10, c.y + R + 10), StudioAction::TimerRing);
            }

            SText(L"Timer", D2D1::RectF(x0, y0, r.right, y0 + 20), SFSong, white);
            SText(mine ? (t.running ? L"Running on the island" : L"Paused")
                       : L"Scroll for minutes \x00B7 Shift for seconds",
                  D2D1::RectF(x0, y0 + 20, r.right, y0 + 36), SFSmall, grey);

            // Presets, then + Save when the ring shows a time that isn't one yet.
            const std::vector<int> presets = bt::ParsePresets(g_settings.timerPresets);
            float cx = x0;
            const float cy = y0 + 44;
            for (int s : presets) {
                const std::wstring label = bt::PresetLabel(s);
                const float cw = SMeasure(label, SFLabel) + 20;
                if (cx + cw > r.right) break;
                const bool on = !mine && s == set;
                const D2D1_RECT_F chip = D2D1::RectF(cx, cy, cx + cw, cy + 24);
                SFillRound(chip, 12, on ? WithAlpha(accent, 0.4f) : D2D1::ColorF(1, 1, 1, SLerp(0.08f, 0.16f, SHover(StudioAction::CountPreset, s))));
                SText(label, chip, SFLabel, on ? white : SCol(0xe8e8ee), 1);
                SHit(chip, StudioAction::CountPreset, s);
                cx += cw + 6;
            }
            const bool known = std::find(presets.begin(), presets.end(), set) != presets.end();
            if (!mine && !known && presets.size() < bt::kMaxPresets) {
                const std::wstring label = L"+ Save";
                const float cw = SMeasure(label, SFLabel) + 20;
                if (cx + cw <= r.right) {
                    const D2D1_RECT_F chip = D2D1::RectF(cx, cy, cx + cw, cy + 24);
                    const float hover = SHover(StudioAction::CountSave);
                    if (ID2D1SolidColorBrush* b = AcquireBrush(D2D1::ColorF(1, 1, 1, SLerp(0.22f, 0.4f, hover))))
                        target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(chip.left + 0.5f, chip.top + 0.5f, chip.right - 0.5f, chip.bottom - 0.5f), 12, 12), b, 1.0f);
                    SText(label, chip, SFLabel, white, 1);
                    SHit(chip, StudioAction::CountSave);
                }
            }

            const float by = y0 + 78;
            float bx = x0;
            const std::wstring go = mine ? (t.running ? L"Pause" : L"Resume") : L"Start";
            pill(D2D1::RectF(bx, by, bx + 86, by + 30), go, true, StudioAction::CountToggle);
            bx += 92;
            for (int m : {1, 5}) {
                const std::wstring label = L"+" + std::to_wstring(m) + L" min";
                const float w = pillW(label);
                if (bx + w > r.right) break;
                pill(D2D1::RectF(bx, by, bx + w, by + 30), label, false, StudioAction::CountAdd, m);
                bx += w + 6;
            }
            if (mine && bx + 62 <= r.right) pill(D2D1::RectF(bx, by, bx + 62, by + 30), L"Reset", false, StudioAction::CountReset);
            return;
        }

        // ── Stopwatch ──
        const StopwatchSnapshot& sw = state.stopwatch;
        const double elapsed = Timers::StopwatchElapsed(sw, now);
        SRing(c, R, ringW, static_cast<float>(std::fmod(elapsed, 60.0) / 60.0), accent);
        ringText(bt::StopwatchText(elapsed), sw.running ? L"Running" : (sw.active ? L"Paused" : L"Ready"), sw.running ? accent : grey);
        if (sw.running) g_layoutDirty = true;  // hundredths move every frame

        float bx = x0;
        const float by = y0;
        pill(D2D1::RectF(bx, by, bx + 86, by + 30), sw.running ? L"Pause" : (sw.active ? L"Resume" : L"Start"), true, StudioAction::SwToggle);
        bx += 92;
        if (sw.running) {
            pill(D2D1::RectF(bx, by, bx + 58, by + 30), L"Lap", false, StudioAction::SwLap);
            bx += 64;
        } else if (sw.active) {
            pill(D2D1::RectF(bx, by, bx + 62, by + 30), L"Reset", false, StudioAction::SwReset);
            bx += 68;
        }
        const std::vector<bt::Lap>& laps = Timers::g_laps;
        if (!laps.empty() && bx + 62 <= r.right) pill(D2D1::RectF(bx, by, bx + 62, by + 30), L"Copy", false, StudioAction::SwCopy);

        // The laps, newest first; the fastest in green, the slowest in red.
        const float ly = by + 40;
        if (laps.empty()) {
            SText(sw.running ? L"Lap marks a split" : L"Laps show here", D2D1::RectF(x0, ly, r.right, ly + 18), SFSmall, grey);
            return;
        }
        const bt::LapMarks marks = bt::MarkLaps(laps);
        const int rows = std::max(1, static_cast<int>((r.bottom - ly) / 17.0f));
        const int n = static_cast<int>(laps.size());
        for (int k = 0; k < rows && k < n; ++k) {
            const int i = n - 1 - k;
            const float y = ly + k * 17.0f;
            const D2D1_COLOR_F col = i == marks.fastest ? green : (i == marks.slowest ? red : SCol(0xe8e8ee));
            SText(L"Lap " + std::to_wstring(i + 1), D2D1::RectF(x0, y, x0 + 60, y + 17), SFBody, col);
            SText(bt::StopwatchText(laps[static_cast<size_t>(i)].split), D2D1::RectF(x0 + 60, y, r.right - 90, y + 17), SFBody, col, 2);
            SText(bt::StopwatchText(laps[static_cast<size_t>(i)].total), D2D1::RectF(r.right - 90, y, r.right, y + 17), SFSmall, grey, 2);
        }
    }

    // ── Bunbun: Rolling Wave History & Spline Renderer ──
    struct StudioWaveHistory {
        static constexpr size_t kMax = 28;
        float buf[kMax] = {};
        size_t count = 0;
        double lastPush = 0.0;

        void Push(float val, double now, double interval = 0.4) {
            if (count == 0) {
                buf[0] = val;
                count = 1;
                lastPush = now;
                return;
            }
            if (now - lastPush < interval) {
                buf[count - 1] = val;
                return;
            }
            lastPush = now;
            if (count < kMax) {
                buf[count++] = val;
            } else {
                for (size_t i = 1; i < kMax; ++i) buf[i - 1] = buf[i];
                buf[kMax - 1] = val;
            }
        }
    };

    StudioWaveHistory cpuWave_, gpuWave_, ramWave_, diskWave_, netDownWave_, netUpWave_;

    void DrawSmoothWave(D2D1_RECT_F r, const float* vals, size_t n, float minVal, float maxVal,
                        D2D1_COLOR_F strokeCol, float strokeW, bool filled = false,
                        D2D1_COLOR_F fillColTop = D2D1::ColorF(1, 1, 1, 0.22f),
                        D2D1_COLOR_F fillColBottom = D2D1::ColorF(1, 1, 1, 0.0f)) {
        if (n < 2 || r.right <= r.left || r.bottom <= r.top) return;
        const float range = std::max(0.001f, maxVal - minVal);
        std::vector<D2D1_POINT_2F> pts(n);
        for (size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float x = r.left + t * (r.right - r.left);
            const float norm = std::clamp((vals[i] - minVal) / range, 0.0f, 1.0f);
            const float y = r.bottom - norm * (r.bottom - r.top);
            pts[i] = D2D1::Point2F(x, y);
        }

        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(d2dFactory_->CreatePathGeometry(&path)) || !path) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(&sink)) || !sink) return;

        sink->BeginFigure(pts[0], filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 0; i + 1 < n; ++i) {
            const D2D1_POINT_2F p0 = (i > 0) ? pts[i - 1] : pts[i];
            const D2D1_POINT_2F p1 = pts[i];
            const D2D1_POINT_2F p2 = pts[i + 1];
            const D2D1_POINT_2F p3 = (i + 2 < n) ? pts[i + 2] : p2;
            const D2D1_POINT_2F c1 = D2D1::Point2F(p1.x + (p2.x - p0.x) / 6.0f, p1.y + (p2.y - p0.y) / 6.0f);
            const D2D1_POINT_2F c2 = D2D1::Point2F(p2.x - (p3.x - p1.x) / 6.0f, p2.y - (p3.y - p1.y) / 6.0f);
            sink->AddBezier(D2D1::BezierSegment(c1, c2, p2));
        }

        if (filled) {
            sink->AddLine(D2D1::Point2F(r.right, r.bottom));
            sink->AddLine(D2D1::Point2F(r.left, r.bottom));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();

            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, fillColTop},
                {1.0f, fillColBottom}
            };
            ComPtr<ID2D1GradientStopCollection> stopColl;
            ComPtr<ID2D1LinearGradientBrush> gradBrush;
            if (SUCCEEDED(target_->CreateGradientStopCollection(stops, 2, &stopColl)) &&
                SUCCEEDED(target_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.bottom)),
                    stopColl.Get(), &gradBrush))) {
                target_->FillGeometry(path.Get(), gradBrush.Get());
            }

            // Stroke on top
            ComPtr<ID2D1PathGeometry> linePath;
            if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&linePath)) && linePath) {
                ComPtr<ID2D1GeometrySink> lineSink;
                if (SUCCEEDED(linePath->Open(&lineSink)) && lineSink) {
                    lineSink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
                    for (size_t i = 0; i + 1 < n; ++i) {
                        const D2D1_POINT_2F p0 = (i > 0) ? pts[i - 1] : pts[i];
                        const D2D1_POINT_2F p1 = pts[i];
                        const D2D1_POINT_2F p2 = pts[i + 1];
                        const D2D1_POINT_2F p3 = (i + 2 < n) ? pts[i + 2] : p2;
                        const D2D1_POINT_2F c1 = D2D1::Point2F(p1.x + (p2.x - p0.x) / 6.0f, p1.y + (p2.y - p0.y) / 6.0f);
                        const D2D1_POINT_2F c2 = D2D1::Point2F(p2.x - (p3.x - p1.x) / 6.0f, p2.y - (p3.y - p1.y) / 6.0f);
                        lineSink->AddBezier(D2D1::BezierSegment(c1, c2, p2));
                    }
                    lineSink->EndFigure(D2D1_FIGURE_END_OPEN);
                    lineSink->Close();
                    if (ID2D1SolidColorBrush* sb = AcquireBrush(strokeCol))
                        target_->DrawGeometry(linePath.Get(), sb, strokeW);
                }
            }
        } else {
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            if (ID2D1SolidColorBrush* sb = AcquireBrush(strokeCol))
                target_->DrawGeometry(path.Get(), sb, strokeW);
        }
    }

    // ── Bunbun: Studio System Page Redo (Vayli 2x2 layout) ──
    int brightValue_ = -1;     // built-in panel brightness 0..100, -1 = none (a desktop monitor)
    double brightAt_ = -10.0;  // when it was last read
    void DrawStudioSystemPage(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        EnsureRoundStroke();
        studioOrigin_ = publishedOrigin_;
        const float w = rect.right - rect.left;
        const float h = rect.bottom - rect.top;
        const float gap = 10.0f;
        const float colW = (w - gap) * 0.5f;
        const float rowH = (h - gap) * 0.5f;

        // Bunbun: Quick toggles (Network) and Hardware monitor (Performance) are switches
        // in Studio too: an off card goes and its neighbour takes the whole row.
        const bool netCard = settings.connectivityModule, perfCard = settings.hardwareMonitorModule;
        const D2D1_RECT_F r1 = D2D1::RectF(rect.left, rect.top, netCard ? rect.left + colW : rect.right, rect.top + rowH);
        const D2D1_RECT_F r2 = D2D1::RectF(rect.left + colW + gap, rect.top, rect.right, rect.top + rowH);
        const D2D1_RECT_F r3 = D2D1::RectF(rect.left, rect.top + rowH + gap, rect.left + colW, rect.bottom);
        const D2D1_RECT_F r4 = D2D1::RectF(perfCard ? rect.left + colW + gap : rect.left, rect.top + rowH + gap, rect.right, rect.bottom);

        const D2D1_COLOR_F cardBg = SCol(0x18181a);
        const D2D1_COLOR_F white = SCol(0xffffff);
        const D2D1_COLOR_F grey = SCol(0x8e8e93);
        const D2D1_COLOR_F labelCol = SCol(0xa0a0a0);
        const D2D1_COLOR_F green = SCol(0x30d158);
        const D2D1_COLOR_F accent = SAccent();

        // ── Card 1: Battery ──
        SFillRound(r1, 12.0f, cardBg);
        {
            // Battery icon
            D2D1_RECT_F bp = D2D1::RectF(r1.left + 16.0f, r1.top + 10.0f, r1.left + 29.0f, r1.top + 19.0f);
            if (ID2D1SolidColorBrush* br = AcquireBrush(labelCol)) {
                target_->DrawRoundedRectangle(D2D1::RoundedRect(bp, 2.0f, 2.0f), br, 1.2f);
                D2D1_RECT_F tip = D2D1::RectF(bp.right + 0.5f, bp.top + 2.5f, bp.right + 2.0f, bp.bottom - 2.5f);
                target_->FillRoundedRectangle(D2D1::RoundedRect(tip, 0.8f, 0.8f), br);
            }
            SText(L"Battery", D2D1::RectF(r1.left + 34.0f, r1.top + 8.0f, r1.left + 120.0f, r1.top + 22.0f), SFLabel, labelCol);

            // Health
            if (state.connectivity.hasBattery) {
                const int health = state.connectivity.designMwh > 0
                    ? std::clamp(state.connectivity.fullMwh * 100 / state.connectivity.designMwh, 0, 100)
                    : 100;
                SText(L"Health " + std::to_wstring(health) + L"%", D2D1::RectF(r1.right - 90.0f, r1.top + 8.0f, r1.right - 16.0f, r1.top + 22.0f), SFSmall, grey, 2);
            } else {
                SText(L"AC Power", D2D1::RectF(r1.right - 90.0f, r1.top + 8.0f, r1.right - 16.0f, r1.top + 22.0f), SFSmall, grey, 2);
            }

            // Big percent
            const int batt = state.connectivity.hasBattery ? state.connectivity.batteryPercent : 100;
            const std::wstring bStr = std::to_wstring(batt) + L"%";
            SText(bStr, D2D1::RectF(r1.left + 16.0f, r1.top + 26.0f, r1.left + 90.0f, r1.top + 56.0f), SFBig, white);

            // Subtitle
            const float bW = SMeasure(bStr, SFBig);
            std::wstring sub;
            if (!state.connectivity.hasBattery) sub = L"Plugged in";
            else if (batt == 100 && state.connectivity.onPower) sub = L"Fully charged";
            else if (state.connectivity.charging) sub = L"Charging";
            else if (state.connectivity.batterySecondsLeft != BATTERY_LIFE_UNKNOWN) {
                const int hSec = static_cast<int>(state.connectivity.batterySecondsLeft);
                sub = std::to_wstring(hSec / 3600) + L"h " + std::to_wstring((hSec % 3600) / 60) + L"m remaining";
            } else sub = L"On battery";
            SText(sub, D2D1::RectF(r1.left + 16.0f + bW + 8.0f, r1.top + 34.0f, r1.right - 16.0f, r1.top + 52.0f), SFLabel, grey);

            // Progress Bar
            const float barL = r1.left + 16.0f, barR = r1.right - 16.0f;
            SFillRound(D2D1::RectF(barL, r1.top + 64.0f, barR, r1.top + 71.0f), 3.5f, SCol(0x28282a));
            const float f = std::clamp(batt / 100.0f, 0.0f, 1.0f);
            const D2D1_COLOR_F barCol = state.connectivity.charging ? green : accent;
            SFillRound(D2D1::RectF(barL, r1.top + 64.0f, barL + (barR - barL) * f, r1.top + 71.0f), 3.5f, barCol);
        }

        // ── Card 2: Network ──
        if (netCard) {
        SFillRound(r2, 12.0f, cardBg);
            // Wi-Fi glyph
            const D2D1_POINT_2F wc = D2D1::Point2F(r2.left + 22.0f, r2.top + 17.0f);
            SArc(wc, 2.5f, -2.4f, -0.74f, accent, 1.2f);
            SArc(wc, 5.5f, -2.4f, -0.74f, accent, 1.2f);
            SArc(wc, 8.5f, -2.4f, -0.74f, accent, 1.2f);
            SText(L"Network", D2D1::RectF(r2.left + 34.0f, r2.top + 8.0f, r2.left + 120.0f, r2.top + 22.0f), SFLabel, labelCol);

            // Bunbun: Wi-Fi and Bluetooth switches (the Wi-Fi & Bluetooth page is part of
            // System in Studio). A click turns the radio on or off; with no usable
            // radio (none fitted, or its driver failed) the chip is dimmed and opens
            // the Windows settings page instead.
            {
                float chipR = r2.right - 12.0f;
                auto chip = [&](const wchar_t* label, int radio, int arg) {
                    const bool on = radio == 1, missing = radio < 0;
                    const float w = SMeasure(label, SFSmall) + 20.0f;
                    const D2D1_RECT_F c = D2D1::RectF(chipR - w, r2.top + 6.0f, chipR, r2.top + 24.0f);
                    const float hover = SHover(StudioAction::RadioToggle, arg);
                    SFillRound(c, 9.0f, on ? WithAlpha(accent, SLerp(0.85f, 1.0f, hover)) : SLerpCol(SCol(0x2c2c2e), SCol(0x3a3a3c), hover));
                    if (ID2D1SolidColorBrush* b = AcquireBrush(on ? white : grey))
                        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.left + 8.0f, (c.top + c.bottom) * 0.5f), 2.5f, 2.5f), b);
                    SText(label, D2D1::RectF(c.left + 13.0f, c.top, c.right - 4.0f, c.bottom), SFSmall,
                          on ? white : missing ? SCol(0x5a5a5e) : grey, 1);
                    if (missing) SHit(c, StudioAction::RadioSettings, arg);
                    else SHit(c, StudioAction::RadioToggle, arg);
                    chipR = c.left - 5.0f;
                };
                chip(L"Bluetooth", state.connectivity.bluetoothRadio, 1);
                chip(L"Wi-Fi", state.connectivity.wifiRadio, 0);
            }

            // Connection Name (a click opens Windows' Wi-Fi or Ethernet settings, to pick a network)
            std::wstring netName = state.connectivity.wifiName;
            if (netName.empty()) netName = state.connectivity.ethernetConnected ? L"Ethernet"
                                         : state.connectivity.wifiRadio == 0 ? L"Wi-Fi is off" : L"Not Connected";
            SText(netName, D2D1::RectF(r2.left + 16.0f, r2.top + 23.0f, r2.right - 16.0f, r2.top + 45.0f), SFTitle, white);
            SHit(D2D1::RectF(r2.left + 12.0f, r2.top + 23.0f, std::min(r2.right - 12.0f, r2.left + 20.0f + SMeasure(netName, SFTitle)), r2.top + 45.0f),
                 StudioAction::RadioSettings, state.connectivity.ethernetConnected && !state.connectivity.wifiConnected ? 2 : 0);

            // Sub-label
            std::wstring netSub;
            if (state.connectivity.wifiConnected) {
                netSub = state.connectivity.wifiSignal > 0
                    ? (L"Wi-Fi \x00B7 " + std::to_wstring(state.connectivity.wifiSignal) + L"% signal")
                    : L"Wi-Fi \x00B7 Connected";
            } else if (state.connectivity.ethernetConnected) {
                netSub = L"Ethernet \x00B7 Connected";  // Bunbun: no made-up link speed
            } else {
                netSub = L"Disconnected";
            }
            SText(netSub, D2D1::RectF(r2.left + 16.0f, r2.top + 45.0f, r2.right - 16.0f, r2.top + 59.0f), SFSmall, grey);

            // Down speed & sparkline
            wchar_t downBuf[32];
            FormatBitRate(downBuf, state.system.netDownMbps, true);
            const std::wstring downText = L"\x2193 " + std::wstring(downBuf);
            const float dW = SMeasure(downText, SFSmall);
            SText(downText, D2D1::RectF(r2.left + 16.0f, r2.top + 63.0f, r2.left + 16.0f + dW + 4.0f, r2.top + 77.0f), SFSmall, white);
            netDownWave_.Push(state.system.netDownMbps, now);
            const D2D1_RECT_F rDown = D2D1::RectF(r2.left + 16.0f + dW + 6.0f, r2.top + 64.0f, r2.left + 16.0f + dW + 48.0f, r2.top + 76.0f);
            float maxDown = 1.0f;
            for (size_t k = 0; k < netDownWave_.count; ++k) maxDown = std::max(maxDown, netDownWave_.buf[k]);
            if (netDownWave_.count < 4) {
                float sInit[6] = {};  // Bunbun: flat until there are real samples
                DrawSmoothWave(rDown, sInit, 6, 0.0f, 1.0f, accent, 1.2f);
            } else {
                DrawSmoothWave(rDown, netDownWave_.buf, netDownWave_.count, 0.0f, maxDown, accent, 1.2f);
            }

            // Up speed & sparkline
            wchar_t upBuf[32];
            FormatBitRate(upBuf, state.system.netUpMbps, true);
            const std::wstring upText = L"\x2191 " + std::wstring(upBuf);
            const float uW = SMeasure(upText, SFSmall);
            const float upStart = r2.left + colW * 0.54f;
            SText(upText, D2D1::RectF(upStart, r2.top + 63.0f, upStart + uW + 4.0f, r2.top + 77.0f), SFSmall, white);
            netUpWave_.Push(state.system.netUpMbps, now);
            const D2D1_RECT_F rUp = D2D1::RectF(upStart + uW + 6.0f, r2.top + 64.0f, r2.right - 14.0f, r2.top + 76.0f);
            float maxUp = 1.0f;
            for (size_t k = 0; k < netUpWave_.count; ++k) maxUp = std::max(maxUp, netUpWave_.buf[k]);
            if (netUpWave_.count < 4) {
                float sInit[6] = {};  // Bunbun: flat until there are real samples
                DrawSmoothWave(rUp, sInit, 6, 0.0f, 1.0f, accent, 1.2f);
            } else {
                DrawSmoothWave(rUp, netUpWave_.buf, netUpWave_.count, 0.0f, maxUp, accent, 1.2f);
            }
        }

        // ── Card 3: Performance (CPU, GPU, RAM, Disk) ──
        // Bunbun: real samples only. Each wave starts flat at the current value (no
        // made-up history), and GPU has a column only when Windows reports its load.
        if (perfCard) {
        SFillRound(r3, 12.0f, cardBg);
            SIcon(StudioPage::Hardware, D2D1::Point2F(r3.left + 22.0f, r3.top + 14.0f), 13.0f, accent);
            SText(L"Performance", D2D1::RectF(r3.left + 34.0f, r3.top + 8.0f, r3.left + 140.0f, r3.top + 22.0f), SFLabel, labelCol);

            const int gpu = state.system.gpuPercent;
            const int diskUsed = 100 - state.system.diskFreePercent;
            cpuWave_.Push(static_cast<float>(state.system.cpuPercent), now);
            ramWave_.Push(static_cast<float>(state.system.memoryPercent), now);
            diskWave_.Push(static_cast<float>(diskUsed), now);
            if (gpu >= 0) gpuWave_.Push(static_cast<float>(gpu), now);
            struct PerfCol { const wchar_t* label; int value; StudioWaveHistory* wave; bool filled; };
            PerfCol cols[4] = {{L"CPU", state.system.cpuPercent, &cpuWave_, false}};
            int n = 1;
            if (gpu >= 0) cols[n++] = {L"GPU", gpu, &gpuWave_, false};
            cols[n++] = {L"RAM", state.system.memoryPercent, &ramWave_, true};
            cols[n++] = {L"Disk", diskUsed, &diskWave_, false};
            const float pColW = (r3.right - r3.left - 32.0f) / static_cast<float>(n);
            for (int k = 0; k < n; ++k) {
                const float x = r3.left + 16.0f + pColW * k;
                SText(std::to_wstring(cols[k].value) + L"%", D2D1::RectF(x, r3.top + 25.0f, x + pColW, r3.top + 45.0f), SFTitle, white);
                SText(cols[k].label, D2D1::RectF(x, r3.top + 45.0f, x + pColW, r3.top + 57.0f), SFSmall, grey);
                const D2D1_RECT_F wr = D2D1::RectF(x, r3.top + (cols[k].filled ? 60.0f : 64.0f), x + pColW - 4.0f, r3.top + 76.0f);
                const StudioWaveHistory& w = *cols[k].wave;
                float flat[2] = {w.count ? w.buf[0] : 0.0f, w.count ? w.buf[0] : 0.0f};
                const float* vals = w.count >= 2 ? w.buf : flat;
                const size_t cnt = w.count >= 2 ? w.count : 2;
                if (cols[k].filled)
                    DrawSmoothWave(wr, vals, cnt, 0.0f, 100.0f, accent, 1.3f, true, WithAlpha(accent, 0.25f), WithAlpha(accent, 0.0f));
                else
                    DrawSmoothWave(wr, vals, cnt, 0.0f, 100.0f, accent, 1.3f);
            }
        }

        // ── Card 4: Volume & Bluetooth ──
        SFillRound(r4, 12.0f, cardBg);
        {
            const int vol = (g_studioVolumeDrag && g_studioVolumeLevelPending >= 0.0f)
                ? static_cast<int>(std::round(g_studioVolumeLevelPending * 100.0f))
                : state.system.volumePercent;

            // Bunbun: a built-in panel adds a brightness slider; the reading is refreshed once a second
            // (an external monitor has none, and the card keeps its big volume number).
            if (!g_studioBrightDrag && now - brightAt_ > 1.0) {
                brightValue_ = CurrentPanelBrightness();
                brightAt_ = now;
            }
            const bool hasBright = brightValue_ >= 0;
            const int bright = (g_studioBrightDrag && g_studioBrightPending >= 0.0f)
                ? static_cast<int>(std::round(g_studioBrightPending * 100.0f))
                : brightValue_;

            SSpeaker(D2D1::Point2F(r4.left + 22.0f, r4.top + 14.0f), 13.0f, vol, state.system.volumeMuted, accent);
            SText(hasBright ? L"Volume & Brightness" : L"Volume",
                  D2D1::RectF(r4.left + 34.0f, r4.top + 8.0f, r4.left + (hasBright ? 150.0f : 90.0f), r4.top + 22.0f), SFLabel, labelCol);

            // Right header: Connected Bluetooth device name & its battery!
            if (!state.connectivity.connectedBtName.empty()) {
                std::wstring btStr = L"\U0001F3A7 " + state.connectivity.connectedBtName; // 🎧
                if (state.connectivity.connectedBtBattery >= 0) {
                    btStr += L" \x00B7 \U0001F50B " + std::to_wstring(state.connectivity.connectedBtBattery) + L"%"; // 🔋 %
                }
                SText(btStr, D2D1::RectF(r4.left + (hasBright ? 150.0f : 50.0f), r4.top + 8.0f, r4.right - 14.0f, r4.top + 22.0f), SFSmall, green, 2);
                const float bw = std::min(r4.right - r4.left - 64.0f, SMeasure(btStr, SFSmall) + 8.0f);
                SHit(D2D1::RectF(r4.right - 14.0f - bw, r4.top + 6.0f, r4.right - 10.0f, r4.top + 24.0f), StudioAction::RadioSettings, 1);
            } else if (!state.system.audioDeviceName.empty()) {  // Bunbun: only the real device name, never a made-up one
                SText(state.system.audioDeviceName, D2D1::RectF(r4.left + (hasBright ? 150.0f : 50.0f), r4.top + 8.0f, r4.right - 14.0f, r4.top + 22.0f), SFSmall, grey, 2);
            }

            if (hasBright) {
                // Two slim rows: [speaker / mute] ---o--- 80%   and   [sun] ---o--- 60%.
                const float trackL = r4.left + 46.0f, trackR = r4.right - 52.0f;
                auto sliderRow = [&](float y, int percent) {
                    const float cy = y + 11.0f;
                    SFillRound(D2D1::RectF(trackL, cy - 2.0f, trackR, cy + 2.0f), 2.0f, SCol(0x28282a));
                    const float kx = trackL + (trackR - trackL) * std::clamp(percent / 100.0f, 0.0f, 1.0f);
                    SFillRound(D2D1::RectF(trackL, cy - 2.0f, kx, cy + 2.0f), 2.0f, accent);
                    if (ID2D1SolidColorBrush* kb = AcquireBrush(white)) target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), 5.5f, 5.5f), kb);
                    SText(std::to_wstring(percent) + L"%", D2D1::RectF(trackR + 8.0f, y, r4.right - 12.0f, y + 22.0f), SFLabel, white, 2);
                    return D2D1::RectF(trackL, y - 2.0f, trackR, y + 24.0f);
                };
                const float yVol = r4.top + 30.0f, yBri = r4.top + 58.0f;
                const D2D1_RECT_F muteBtn = D2D1::RectF(r4.left + 14.0f, yVol, r4.left + 38.0f, yVol + 22.0f);
                SFillRound(muteBtn, 11.0f, SCol(state.system.volumeMuted ? 0x501b1b : 0x242426));
                SSpeaker(D2D1::Point2F(r4.left + 26.0f, yVol + 11.0f), 11.0f, vol, state.system.volumeMuted, white);
                SHit(muteBtn, StudioAction::Volume, -1);
                SHit(sliderRow(yVol, vol), StudioAction::Volume, 0);
                // Sun: a disc with eight rays.
                const D2D1_POINT_2F sun = D2D1::Point2F(r4.left + 26.0f, yBri + 11.0f);
                if (ID2D1SolidColorBrush* sb = AcquireBrush(accent)) target_->FillEllipse(D2D1::Ellipse(sun, 3.4f, 3.4f), sb);
                for (int k = 0; k < 8; ++k) {
                    const float a = static_cast<float>(k) * 0.7853982f;
                    SLine(D2D1::Point2F(sun.x + std::cos(a) * 5.8f, sun.y + std::sin(a) * 5.8f),
                          D2D1::Point2F(sun.x + std::cos(a) * 8.0f, sun.y + std::sin(a) * 8.0f), accent, 1.2f);
                }
                SHit(sliderRow(yBri, bright), StudioAction::Brightness, 0);
            } else {
            // Big Volume %
            SText(std::to_wstring(vol) + L"%", D2D1::RectF(r4.left + 16.0f, r4.top + 26.0f, r4.left + 100.0f, r4.top + 56.0f), SFBig, white);

            // Circular Mute Button
            const D2D1_RECT_F muteRect = D2D1::RectF(r4.right - 44.0f, r4.top + 26.0f, r4.right - 16.0f, r4.top + 54.0f);
            SFillRound(muteRect, 14.0f, SCol(state.system.volumeMuted ? 0x501b1b : 0x242426));
            SSpeaker(D2D1::Point2F(r4.right - 30.0f, r4.top + 40.0f), 13.0f, vol, state.system.volumeMuted, white);
            SHit(muteRect, StudioAction::Volume, -1);

            // Volume Slider
            const float slL = r4.left + 16.0f, slR = r4.right - 16.0f;
            SFillRound(D2D1::RectF(slL, r4.top + 66.0f, slR, r4.top + 70.0f), 2.0f, SCol(0x28282a));
            const float kx = slL + (slR - slL) * std::clamp(vol / 100.0f, 0.0f, 1.0f);
            SFillRound(D2D1::RectF(slL, r4.top + 66.0f, kx, r4.top + 70.0f), 2.0f, accent);
            if (ID2D1SolidColorBrush* kb = AcquireBrush(white)) {
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, r4.top + 68.0f), 5.5f, 5.5f), kb);
            }
            SHit(D2D1::RectF(slL, r4.top + 58.0f, slR, r4.top + 78.0f), StudioAction::Volume, 0);
            }
        }
    }

    // ── Bunbun: Studio Weather Page Redo (Vayli 3-card layout) ──
    void DrawStudioWeatherPage(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        EnsureRoundStroke();
        studioOrigin_ = publishedOrigin_;
        const float w = rect.right - rect.left;
        const float h = rect.bottom - rect.top;
        const float gap = 6.0f;
        const float w1 = 138.0f;
        const float w3 = 140.0f;
        const float w2 = w - w1 - w3 - gap * 2.0f; // 182px

        const D2D1_RECT_F rc1 = D2D1::RectF(rect.left, rect.top, rect.left + w1, rect.bottom);
        const D2D1_RECT_F rc2 = D2D1::RectF(rect.left + w1 + gap, rect.top, rect.left + w1 + gap + w2, rect.bottom);
        const D2D1_RECT_F rc3 = D2D1::RectF(rc2.right + gap, rect.top, rect.right, rect.bottom);

        const D2D1_COLOR_F cardBg = SCol(0x18181a);
        const D2D1_COLOR_F white = SCol(0xffffff);
        const D2D1_COLOR_F grey = SCol(0x8e8e93);

        const D2D1_COLOR_F accent = SAccent();

        // ── Card 1: Current Weather ──
        SFillRound(rc1, 12.0f, cardBg);
        {
            // Big weather icon (dynamic accent)
            ComPtr<ID2D1SolidColorBrush> icStrong, icSoft;
            target_->CreateSolidColorBrush(accent, &icStrong);
            target_->CreateSolidColorBrush(WithAlpha(accent, 0.45f), &icSoft);
            if (icStrong && icSoft) {
                DrawWeatherIcon(D2D1::Point2F(rc1.left + 36.0f, rc1.top + 34.0f), 38.0f,
                                WeatherVisualFromCode(state.weather.weatherCode), icStrong.Get(), icSoft.Get());
            }

            // Big temperature
            wchar_t tBuf[16];
            swprintf(tBuf, 16, L"%.0f\x00B0", state.weather.temperature);
            SText(tBuf, D2D1::RectF(rc1.left + 14.0f, rc1.top + 54.0f, rc1.right - 14.0f, rc1.top + 88.0f), SFHero, white);

            // Condition text
            // Bunbun: only what the service sent; nothing made up when a value is missing.
            const std::wstring desc = state.weather.weatherDesc;
            SText(desc, D2D1::RectF(rc1.left + 14.0f, rc1.top + 90.0f, rc1.right - 14.0f, rc1.top + 106.0f), SFBody, white);

            // H / L / Feels like
            wchar_t hlBuf[64];
            if (state.weather.feelsLike.empty())
                swprintf(hlBuf, 64, L"H %.0f\x00B0 L %.0f\x00B0", state.weather.todayHigh, state.weather.todayLow);
            else
                swprintf(hlBuf, 64, L"H %.0f\x00B0 L %.0f\x00B0 \x00B7 Feels like %s\x00B0", state.weather.todayHigh, state.weather.todayLow,
                         state.weather.feelsLike.c_str());
            SText(hlBuf, D2D1::RectF(rc1.left + 10.0f, rc1.top + 108.0f, rc1.right - 8.0f, rc1.top + 124.0f), SFSmall, grey);

            // Divider
            SLine(D2D1::Point2F(rc1.left + 14.0f, rc1.top + 132.0f), D2D1::Point2F(rc1.right - 14.0f, rc1.top + 132.0f), SCol(0x28282a), 1.0f);

            // Sunrise / Sunset
            wchar_t sunBuf[64] = L"";
            if (!state.weather.sunrise.empty() && !state.weather.sunset.empty())
                swprintf(sunBuf, 64, L"Sunrise %s \x00B7 Sunset %s", state.weather.sunrise.c_str(), state.weather.sunset.c_str());
            SText(sunBuf, D2D1::RectF(rc1.left + 10.0f, rc1.top + 140.0f, rc1.right - 8.0f, rc1.top + 154.0f), SFSmall, grey);

            // Updated time
            const int mins = static_cast<int>(std::max(0.0, now - state.weather.lastUpdated) / 60.0);
            const std::wstring updStr = mins <= 1 ? L"Updated just now" : (L"Updated " + std::to_wstring(mins) + L"m ago");
            SText(updStr, D2D1::RectF(rc1.left + 14.0f, rc1.top + 156.0f, rc1.right - 14.0f, rc1.top + 170.0f), SFSmall, SCol(0x636366));
        }

        // ── Card 2: Hourly Forecast & Stats ──
        SFillRound(rc2, 12.0f, cardBg);
        {
            SText(L"Hourly", D2D1::RectF(rc2.left + 14.0f, rc2.top + 8.0f, rc2.left + 80.0f, rc2.top + 22.0f), SFSmall, grey);

            const size_t hours = std::min<size_t>(6, state.weather.hourly.size());
            const float hStep = (w2 - 24.0f) / static_cast<float>(std::max<size_t>(1, hours - (hours > 1 ? 1 : 0)));
            std::vector<D2D1_POINT_2F> curvePts(hours);

            float tMin = 1e6f, tMax = -1e6f;
            for (size_t i = 0; i < hours; ++i) {
                const float t = state.weather.hourly[i].temp;
                tMin = std::min(tMin, t);
                tMax = std::max(tMax, t);
            }
            if (tMax <= tMin) tMax = tMin + 2.0f;

            ComPtr<ID2D1SolidColorBrush> icStrong, icSoft;
            target_->CreateSolidColorBrush(studioInk_ ? StudioInk(white) : white, &icStrong);  // Bunbun: light islands
            target_->CreateSolidColorBrush(WithAlpha(studioInk_ ? StudioInk(white) : white, 0.45f), &icSoft);

            for (size_t i = 0; i < hours; ++i) {
                const float cx = rc2.left + 12.0f + static_cast<float>(i) * hStep;
                const std::wstring lbl = state.weather.hourly[i].timeLabel;
                SText(lbl, D2D1::RectF(cx - 16.0f, rc2.top + 22.0f, cx + 16.0f, rc2.top + 34.0f), SFSmall, grey, 1);

                if (icStrong && icSoft) {
                    const int code = state.weather.hourly[i].weatherCode;
                    DrawWeatherIcon(D2D1::Point2F(cx, rc2.top + 43.0f), 12.0f, WeatherVisualFromCode(code), icStrong.Get(), icSoft.Get());
                }

                const float tVal = state.weather.hourly[i].temp;
                wchar_t tStr[16];
                swprintf(tStr, 16, L"%.0f\x00B0", tVal);
                SText(tStr, D2D1::RectF(cx - 16.0f, rc2.top + 52.0f, cx + 16.0f, rc2.top + 64.0f), SFSmall, white, 1);

                const float py = (rc2.top + 104.0f) - ((tVal - tMin) / (tMax - tMin)) * 26.0f;
                curvePts[i] = D2D1::Point2F(cx, py);
            }

            // Smooth Catmull-Rom Bézier spline with gradient fill
            ComPtr<ID2D1PathGeometry> curvePath;
            if (hours >= 2 && SUCCEEDED(d2dFactory_->CreatePathGeometry(&curvePath)) && curvePath) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(curvePath->Open(&sink)) && sink) {
                    sink->BeginFigure(curvePts[0], D2D1_FIGURE_BEGIN_FILLED);
                    for (size_t i = 0; i + 1 < hours; ++i) {
                        const D2D1_POINT_2F p0 = (i > 0) ? curvePts[i - 1] : curvePts[i];
                        const D2D1_POINT_2F p1 = curvePts[i];
                        const D2D1_POINT_2F p2 = curvePts[i + 1];
                        const D2D1_POINT_2F p3 = (i + 2 < hours) ? curvePts[i + 2] : p2;
                        const D2D1_POINT_2F c1 = D2D1::Point2F(p1.x + (p2.x - p0.x) / 6.0f, p1.y + (p2.y - p0.y) / 6.0f);
                        const D2D1_POINT_2F c2 = D2D1::Point2F(p2.x - (p3.x - p1.x) / 6.0f, p2.y - (p3.y - p1.y) / 6.0f);
                        sink->AddBezier(D2D1::BezierSegment(c1, c2, p2));
                    }
                    sink->AddLine(D2D1::Point2F(curvePts[hours - 1].x, rc2.top + 116.0f));
                    sink->AddLine(D2D1::Point2F(curvePts[0].x, rc2.top + 116.0f));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();

                    D2D1_GRADIENT_STOP stops[2] = {
                        {0.0f, WithAlpha(accent, 0.25f)},
                        {1.0f, WithAlpha(accent, 0.0f)}
                    };
                    ComPtr<ID2D1GradientStopCollection> stopColl;
                    ComPtr<ID2D1LinearGradientBrush> gradBrush;
                    if (SUCCEEDED(target_->CreateGradientStopCollection(stops, 2, &stopColl)) &&
                        SUCCEEDED(target_->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, rc2.top + 76.0f), D2D1::Point2F(0, rc2.top + 116.0f)),
                            stopColl.Get(), &gradBrush))) {
                        target_->FillGeometry(curvePath.Get(), gradBrush.Get());
                    }
                }
            }

            // Stroke line on top
            ComPtr<ID2D1PathGeometry> linePath;
            if (hours >= 2 && SUCCEEDED(d2dFactory_->CreatePathGeometry(&linePath)) && linePath) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(linePath->Open(&sink)) && sink) {
                    sink->BeginFigure(curvePts[0], D2D1_FIGURE_BEGIN_HOLLOW);
                    for (size_t i = 0; i + 1 < hours; ++i) {
                        const D2D1_POINT_2F p0 = (i > 0) ? curvePts[i - 1] : curvePts[i];
                        const D2D1_POINT_2F p1 = curvePts[i];
                        const D2D1_POINT_2F p2 = curvePts[i + 1];
                        const D2D1_POINT_2F p3 = (i + 2 < hours) ? curvePts[i + 2] : p2;
                        const D2D1_POINT_2F c1 = D2D1::Point2F(p1.x + (p2.x - p0.x) / 6.0f, p1.y + (p2.y - p0.y) / 6.0f);
                        const D2D1_POINT_2F c2 = D2D1::Point2F(p2.x - (p3.x - p1.x) / 6.0f, p2.y - (p3.y - p1.y) / 6.0f);
                        sink->AddBezier(D2D1::BezierSegment(c1, c2, p2));
                    }
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    if (ID2D1SolidColorBrush* sb = AcquireBrush(accent))
                        target_->DrawGeometry(linePath.Get(), sb, 1.4f);
                }
            }

            // Circular white dots at the 6 points
            if (ID2D1SolidColorBrush* dotBrush = AcquireBrush(white)) {
                for (size_t i = 0; i < hours; ++i) {
                    target_->FillEllipse(D2D1::Ellipse(curvePts[i], 2.5f, 2.5f), dotBrush);
                }
            }

            // Bottom 3 Stats (Humidity, Wind, UV Index)
            const float sColW = (w2 - 16.0f) / 3.0f;
            const float sy = rc2.top + 136.0f;

            // Stat 1: Humidity
            SText(L"\U0001F4A7 Humidity", D2D1::RectF(rc2.left + 8.0f, sy, rc2.left + 8.0f + sColW + 6.0f, sy + 14.0f), SFSmall, grey);
            SText(state.weather.humidity.empty() ? std::wstring(L"\x2014") : state.weather.humidity + L"%", D2D1::RectF(rc2.left + 8.0f, sy + 15.0f, rc2.left + 8.0f + sColW, sy + 32.0f), SFLabel, white);

            // Stat 2: Wind
            SText(L"\U0001F4A8 Wind", D2D1::RectF(rc2.left + 8.0f + sColW, sy, rc2.left + 8.0f + sColW * 2, sy + 14.0f), SFSmall, grey);
            const std::wstring windVal = state.weather.windSpeed.empty() ? std::wstring(L"\x2014")
                                        : state.weather.windSpeed + (settings.weatherFahrenheit ? L" mph" : L" km/h");
            SText(windVal, D2D1::RectF(rc2.left + 8.0f + sColW, sy + 15.0f, rc2.left + 8.0f + sColW * 2, sy + 32.0f), SFLabel, white);

            // Stat 3: UV Index
            SText(L"\x2600 UV index", D2D1::RectF(rc2.left + 8.0f + sColW * 2, sy, rc2.right - 4.0f, sy + 14.0f), SFSmall, grey);
            const std::wstring uvVal = state.weather.uvDesc.empty() ? std::wstring(L"\x2014") : state.weather.uvDesc;
            const StudioFontId uvFont = (uvVal.size() > 6) ? SFSmall : SFLabel;
            SText(uvVal, D2D1::RectF(rc2.left + 8.0f + sColW * 2, sy + (uvFont == SFSmall ? 16.0f : 15.0f), rc2.right - 4.0f, sy + 32.0f), uvFont, white);
        }

        // ── Card 3: 5-Day Forecast ──
        SFillRound(rc3, 12.0f, cardBg);
        {
            SText(L"5-Day", D2D1::RectF(rc3.left + 14.0f, rc3.top + 8.0f, rc3.left + 80.0f, rc3.top + 22.0f), SFSmall, grey);

            const size_t days = std::min<size_t>(5, state.weather.daily.size());
            float allMin = 1e6f, allMax = -1e6f;
            for (size_t i = 0; i < days; ++i) {
                allMin = std::min(allMin, state.weather.daily[i].minTemp);
                allMax = std::max(allMax, state.weather.daily[i].maxTemp);
            }
            if (allMax <= allMin) allMax = allMin + 4.0f;

            ComPtr<ID2D1SolidColorBrush> icStrong, icSoft;
            target_->CreateSolidColorBrush(studioInk_ ? StudioInk(white) : white, &icStrong);  // Bunbun: light islands
            target_->CreateSolidColorBrush(WithAlpha(studioInk_ ? StudioInk(white) : white, 0.45f), &icSoft);

            for (size_t i = 0; i < days; ++i) {
                const float rowY = rc3.top + 28.0f + static_cast<float>(i) * 28.0f;

                const std::wstring& dName = state.weather.daily[i].dayName;
                const float dLo = state.weather.daily[i].minTemp;
                const float dHi = state.weather.daily[i].maxTemp;
                const int dCode = state.weather.daily[i].weatherCode;

                // Day Name
                SText(dName, D2D1::RectF(rc3.left + 12.0f, rowY, rc3.left + 46.0f, rowY + 24.0f), SFLabel, white);

                // Weather glyph
                if (icStrong && icSoft) {
                    DrawWeatherIcon(D2D1::Point2F(rc3.left + 54.0f, rowY + 12.0f), 12.0f, WeatherVisualFromCode(dCode), icStrong.Get(), icSoft.Get());
                }

                // Min temp
                wchar_t loBuf[16];
                swprintf(loBuf, 16, L"%.0f\x00B0", dLo);
                SText(loBuf, D2D1::RectF(rc3.left + 64.0f, rowY, rc3.left + 86.0f, rowY + 24.0f), SFSmall, grey, 2);

                // Horizontal range bar
                const float barL = rc3.left + 92.0f, barR = rc3.right - 34.0f;
                const float barW = barR - barL;
                SFillRound(D2D1::RectF(barL, rowY + 10.0f, barR, rowY + 14.0f), 2.0f, SCol(0x28282a));

                const float f0 = std::clamp((dLo - allMin) / (allMax - allMin), 0.0f, 1.0f);
                const float f1 = std::clamp((dHi - allMin) / (allMax - allMin), 0.0f, 1.0f);
                const float rx0 = barL + barW * f0;
                const float rx1 = barL + barW * f1;
                SFillRound(D2D1::RectF(rx0, rowY + 10.0f, rx1, rowY + 14.0f), 2.0f, white);

                // Today row has current temperature indicator dot
                if (i == 0) {
                    const float curF = std::clamp((state.weather.temperature - allMin) / (allMax - allMin), 0.0f, 1.0f);
                    const float curX = barL + barW * curF;
                    if (ID2D1SolidColorBrush* db = AcquireBrush(accent)) {
                        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(curX, rowY + 12.0f), 3.0f, 3.0f), db);
                    }
                }

                // Max temp
                wchar_t hiBuf[16];
                swprintf(hiBuf, 16, L"%.0f\x00B0", dHi);
                SText(hiBuf, D2D1::RectF(rc3.right - 30.0f, rowY, rc3.right - 10.0f, rowY + 24.0f), SFLabel, white, 2);
            }
        }
    }

    // ── Agent cards: a plan to approve, several questions ──
    // Drawn instead of the plain approval card (DrawAgent) when the agent sent a
    // plan (ExitPlanMode) or questions the quick buttons can't answer. Buttons go
    // into g_agentButtons like the plain card's, with an action for the new ones.
    static std::wstring CleanPlanText(const std::wstring& plan) { return bb::studio::CleanPlanText(plan); }

    bool DrawAgentRichCard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        const bb::AgentView& a = state.agent;
        const bool plan = a.phase == bb::AgentPhase::Approval && !a.plan.empty();
        const bool questions = NeedsQuestionCard(a);
        if (!plan) g_planScrollMax = 0.0f;
        if (!plan && !questions) return false;
        EnsureRoundStroke();

        const float L = rect.left + 16.0f, R = rect.right - 16.0f, T = rect.top + 12.0f;
        const D2D1_COLOR_F white = SCol(0xffffff), grey = SCol(0x8e8e93);
        float tx = L;
        if (settings.bunbunMascot) {
            D2D1_POINT_2F fc{};
            float fs = 0.0f;
            bb::mascot::FitToBox(D2D1::RectF(L, T, L + 34, T + 34), &fc, &fs);
            bb::mascot::Draw(target_.Get(), fc, fs, plan ? bb::mascot::Mood::Thinking : bb::mascot::Mood::Alert, now,
                             a.phaseSince, MascotLook(fc));
            tx = L + 44;
        }
        const std::wstring who = a.project.empty() ? a.agent : a.agent + L" \x00B7 " + a.project;
        SText(who, D2D1::RectF(tx, T, R - 60, T + 16), SFSmall, grey);

        auto button = [&](D2D1_RECT_F r, const std::wstring& label, bool primary, int action, int arg,
                          bb::AgentDecision decision, bool enabled) {
            const int index = static_cast<int>(g_agentButtons.size());
            const bool hovered = index == g_agentHoveredButton && enabled;
            const D2D1_COLOR_F fill = primary ? D2D1::ColorF(1, 1, 1, enabled ? (hovered ? 1.0f : 0.92f) : 0.25f)
                                              : D2D1::ColorF(1, 1, 1, hovered ? 0.18f : 0.11f);
            SFillRound(r, 10, fill);
            SText(label, r, SFLabel, primary ? SCol(0x141416) : (enabled ? white : grey), 1);
            if (!enabled) return;
            AgentButton b;
            b.rect = D2D1::RectF(r.left - rect.left, r.top - rect.top, r.right - rect.left, r.bottom - rect.top);
            b.decision = decision;
            b.action = action;
            b.arg = arg;
            g_agentButtons.push_back(b);
        };
        const float bh = 30.0f;
        const float by = rect.bottom - 12.0f - bh;

        if (plan) {
            SText((a.agent.empty() ? std::wstring(L"Claude") : a.agent) + L" has a plan",
                  D2D1::RectF(tx, T + 15, R, T + 36), SFSong, white);
            // The plan, scrollable with the wheel.
            if (g_planScrollFor != a.requestId) { g_planScrollFor = a.requestId; g_planScroll = 0.0f; }
            const D2D1_RECT_F area = D2D1::RectF(L, T + 46, R, by - 10);
            SFillRound(area, 12, D2D1::ColorF(1, 1, 1, 0.06f));
            const std::wstring text = CleanPlanText(a.plan);
            const float innerW = area.right - area.left - 24;
            const float h = SWrapHeight(text, SFAsk, innerW);
            const float viewH = area.bottom - area.top - 16;
            g_planScrollMax = std::max(0.0f, h - viewH);
            g_planScroll = Clamp(g_planScroll, 0.0f, g_planScrollMax);
            target_->PushAxisAlignedClip(D2D1::RectF(area.left, area.top + 6, area.right, area.bottom - 6),
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (IDWriteTextFormat* f = SFont(SFAsk)) {
                f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                if (ID2D1SolidColorBrush* b = AcquireBrush(SCol(0xe8e8ee)))
                    target_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), f,
                                       D2D1::RectF(area.left + 12, area.top + 8 - g_planScroll, area.right - 12,
                                                   area.top + 8 - g_planScroll + h + 4), b);
            }
            target_->PopAxisAlignedClip();
            if (g_planScrollMax > 0.0f) {
                // A thin scroll bar on the right edge.
                const float trackH = area.bottom - area.top - 16;
                const float thumbH = std::max(18.0f, trackH * viewH / h);
                const float ty = area.top + 8 + (trackH - thumbH) * (g_planScroll / g_planScrollMax);
                SFillRound(D2D1::RectF(area.right - 6, ty, area.right - 3, ty + thumbH), 1.5f, D2D1::ColorF(1, 1, 1, 0.3f));
            }
            button(D2D1::RectF(L, by, L + 150, by + bh), L"Answer in terminal", false, 0, 0, bb::AgentDecision::Terminal, true);
            button(D2D1::RectF(R - 228, by, R - 118, by + bh), L"Change it\x2026", false, 1 /*ActPlanChange*/, 0,
                   bb::AgentDecision::Terminal, true);
            button(D2D1::RectF(R - 110, by, R, by + bh), L"Approve", true, 0, 0, bb::AgentDecision::Allow, true);
            return true;
        }

        // Several questions: one at a time, then send them all together.
        SyncQuestions(a);
        const int count = static_cast<int>(a.questions.size());
        const int idx = std::clamp(g_questions.index, 0, count - 1);
        const auto& q = a.questions[static_cast<size_t>(idx)];
        if (count > 1) {
            wchar_t step[24];
            swprintf_s(step, L"%d of %d", idx + 1, count);
            SText(step, D2D1::RectF(R - 60, T, R, T + 16), SFSmall, grey, 2);
        }
        // Bunbun: the question wraps in full (the card is measured to fit it, see
        // MeasureQuestionCard), then the hint line, then one row per option.
        const std::wstring qText = q.text.empty() ? std::wstring(L"Has a question for you") : q.text;
        const float qH = static_cast<float>(bb::studio::QuestionTextShown(SWrapHeight(qText, SFAsk, R - tx)));
        SText(qText, D2D1::RectF(tx, rect.top + 27, R, rect.top + 27 + qH + 1), SFAsk, white);
        const bool subLine = !q.header.empty() || q.multi;
        if (subLine)
            SText(q.multi ? (q.header.empty() ? std::wstring(L"Pick any") : q.header + L" \x00B7 pick any") : q.header,
                  D2D1::RectF(tx, rect.top + 31 + qH, R, rect.top + 45 + qH), SFSmall, grey);

        float y = rect.top + static_cast<float>(bb::studio::QuestionOptionsTop(qH, subLine));
        const auto& picks = g_questions.picks[static_cast<size_t>(idx)];
        const std::wstring& typed = g_questions.other[static_cast<size_t>(idx)];
        for (size_t i = 0; i < q.options.size() && i < static_cast<size_t>(bb::studio::kQuestionRowsMax); ++i) {
            const bool on = std::find(picks.begin(), picks.end(), static_cast<int>(i)) != picks.end();
            const D2D1_RECT_F row = D2D1::RectF(L, y, R, y + 26);
            const int index = static_cast<int>(g_agentButtons.size());
            const bool hovered = index == g_agentHoveredButton;
            SFillRound(row, 9, D2D1::ColorF(1, 1, 1, on ? 0.16f : (hovered ? 0.10f : 0.05f)));
            const D2D1_POINT_2F c = D2D1::Point2F(row.left + 16, (row.top + row.bottom) * 0.5f);
            if (ID2D1SolidColorBrush* b = AcquireBrush(on ? SAccent() : D2D1::ColorF(1, 1, 1, 0.45f))) {
                if (q.multi) {
                    const D2D1_RECT_F box = D2D1::RectF(c.x - 6, c.y - 6, c.x + 6, c.y + 6);
                    if (on) target_->FillRoundedRectangle(D2D1::RoundedRect(box, 3, 3), b);
                    else target_->DrawRoundedRectangle(D2D1::RoundedRect(box, 3, 3), b, 1.4f);
                } else if (on) {
                    target_->FillEllipse(D2D1::Ellipse(c, 6, 6), b);
                } else {
                    target_->DrawEllipse(D2D1::Ellipse(c, 6, 6), b, 1.4f);
                }
            }
            if (on) {
                auto P = [](float x, float yy) { return D2D1::Point2F(x, yy); };
                SPoly({P(8.5f, 12), P(11, 14.5f), P(15.5f, 9.5f)}, c, 0.9f, SCol(0xffffff), 1.8f);
            }
            SText(q.options[i], D2D1::RectF(row.left + 30, row.top, row.right - 10, row.bottom), SFBody, white);
            AgentButton b;
            b.rect = D2D1::RectF(row.left - rect.left, row.top - rect.top, row.right - rect.left, row.bottom - rect.top);
            b.action = 3;  // ActToggle
            b.arg = static_cast<int>(i);
            g_agentButtons.push_back(b);
            y += 30;
        }
        if (!typed.empty())
            SText(L"Your answer: \x201C" + typed + L"\x201D", D2D1::RectF(L, by - 18, R, by - 2), SFSmall, SAccent());

        float bx = L;
        if (idx > 0) {
            button(D2D1::RectF(bx, by, bx + 70, by + bh), L"Back", false, 6 /*ActBack*/, 0, bb::AgentDecision::Terminal, true);
            bx += 78;
        }
        button(D2D1::RectF(bx, by, bx + 86, by + bh), L"Other\x2026", false, 5 /*ActOther*/, 0, bb::AgentDecision::Terminal, true);
        button(D2D1::RectF(R - 210, by, R - 104, by + bh), L"In terminal", false, 0, 0, bb::AgentDecision::Terminal, true);
        button(D2D1::RectF(R - 96, by, R, by + bh), idx + 1 < count ? L"Next" : L"Send", true, 4 /*ActNext*/, 0,
               bb::AgentDecision::Terminal, QuestionAnswered(idx));
        return true;
    }

    // ── Calendar agenda (Studio width) ──
    // The calendar page's right-hand column when the page is wide enough: the next
    // meeting from the calendar link, then today's open to-dos with their times.
    void DrawCalendarAgenda(const SharedState& state, D2D1_RECT_F r, double now) {
        (void)now;
        EnsureRoundStroke();
        SFillRound(r, 12, D2D1::ColorF(1, 1, 1, 0.05f));
        float y = r.top + 8;
        SText(L"Agenda", D2D1::RectF(r.left + 10, y, r.right - 8, y + 16), SFLabel, SCol(0x8e8e93));
        y += 20;

        const double epoch = EpochNow();
        auto clockOf = [](double unix) {
            const time_t t = static_cast<time_t>(unix);
            tm lt{};
            localtime_s(&lt, &t);
            wchar_t b[16];
            swprintf_s(b, L"%02d:%02d", lt.tm_hour, lt.tm_min);
            return std::wstring(b);
        };
        auto row = [&](const std::wstring& when, const std::wstring& what, D2D1_COLOR_F dot) {
            if (y + 30 > r.bottom - 4) return false;
            if (ID2D1SolidColorBrush* b = AcquireBrush(dot))
                target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 10, y + 3, r.left + 13, y + 27), 1.5f, 1.5f), b);
            SText(when, D2D1::RectF(r.left + 19, y, r.right - 8, y + 13), SFSmall, SCol(0x8e8e93));
            SText(what, D2D1::RectF(r.left + 19, y + 13, r.right - 8, y + 29), SFBody, SCol(0xffffff));
            y += 34;
            return true;
        };

        bool any = false;
        if (state.meeting.valid && state.meeting.endsAt > epoch) {
            const bool live = state.meeting.startsAt <= epoch;
            row(live ? L"Now" : clockOf(state.meeting.startsAt), state.meeting.title.empty() ? L"Meeting" : state.meeting.title,
                SAccent());
            any = true;
        }
        // Today's open to-dos: timed ones in order, then the rest.
        SYSTEMTIME st{};
        GetLocalTime(&st);
        tm endTm{};
        endTm.tm_year = st.wYear - 1900; endTm.tm_mon = st.wMonth - 1; endTm.tm_mday = st.wDay + 1; endTm.tm_isdst = -1;
        const double dayEnd = static_cast<double>(mktime(&endTm));
        std::vector<const TodoItem*> todos;
        for (const TodoItem& t : state.todos)
            if (!t.done && (t.due <= 0.0 || t.due < dayEnd)) todos.push_back(&t);
        std::stable_sort(todos.begin(), todos.end(), [](const TodoItem* a, const TodoItem* b) {
            return (a->due > 0 ? a->due : 1e18) < (b->due > 0 ? b->due : 1e18);
        });
        for (const TodoItem* t : todos) {
            const bool overdue = t->due > 0 && t->due < epoch;
            if (!row(t->due > 0 ? (overdue ? L"Overdue \x00B7 " : L"") + clockOf(t->due) : L"To-do", t->text,
                     overdue ? SCol(0xff9f0a) : SAccent()))
                break;
            any = true;
        }
        if (!any) {
            SText(L"Nothing planned", D2D1::RectF(r.left + 10, y + 4, r.right - 8, y + 22), SFBody, SCol(0xaeaeb2));
            SText(L"Ask Bun: \x201Cremind me at 3pm\x2026\x201D", D2D1::RectF(r.left + 10, y + 22, r.right - 8, y + 38),
                  SFSmall, SCol(0x636366));
        }
    }

    // ── Ask Bun as a chat (Studio) ──

  public:
    // Bunbun: the question card's height for the question on screen: its wrapped text
    // decides where the options start (bb::studio::QuestionCardHeight). The render loop sizes the card with it.
    float MeasureQuestionCard(const SharedState& state, const Settings& settings) {
        const bb::AgentView& a = state.agent;
        if (a.questions.empty()) return 128.0f;
        SyncQuestions(a);
        const size_t idx = std::min<size_t>(static_cast<size_t>(std::max(0, g_questions.index)), a.questions.size() - 1);
        const auto& q = a.questions[idx];
        const float textW = 440.0f - 32.0f - (settings.bunbunMascot ? 44.0f : 0.0f);
        const float textH = SWrapHeight(q.text.empty() ? std::wstring(L"Has a question for you") : q.text, SFAsk, textW);
        return static_cast<float>(bb::studio::QuestionCardHeight(textH, !q.header.empty() || q.multi, bb::studio::QuestionRows(q)));
    }
  private:

    float SWrapHeight(const std::wstring& text, StudioFontId font, float width) {
        IDWriteTextFormat* f = SFont(font);
        if (!f || text.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), f, width, 4000.0f,
                                                    &layout)) || !layout)
            return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.height;
    }

    D2D1_SIZE_F SMeasureBox(const std::wstring& text, StudioFontId font, float maxWidth) {
        IDWriteTextFormat* f = SFont(font);
        if (!f || text.empty()) return {0.0f, 0.0f};
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), f, maxWidth, 4000.0f,
                                                    &layout)) || !layout)
            return {0.0f, 0.0f};
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return {std::ceil(m.widthIncludingTrailingWhitespace), std::ceil(m.height)};
    }

    // The answer as one block of text: the title when it adds something, the body,
    // and the example on its own line.
    static std::wstring ChatAnswerText(const ChatTurn& t) {
        const LookupSnapshot& a = t.answer;
        if (a.status == LookupStatus::NoResult) return a.body.empty() ? L"I couldn't find anything for that." : a.body;
        std::wstring text;
        if (!a.title.empty() && a.source != L"Bun" && !EqualsNoCase(a.title, t.question)) text = a.title + L" \x2014 ";
        text += a.body;
        if (!a.example.empty()) text += L"\n\x201C" + a.example + L"\x201D";
        return text;
    }

    void DrawStudioChat(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        const LookupSnapshot& lk = state.lookup;
        const float h = rect.bottom - rect.top;
        const float w = rect.right - rect.left;
        if (h < 60.0f || w < 200.0f) return;
        PublishContentGeometry(rect);
        studioOrigin_ = publishedOrigin_;
        EnsureRoundStroke();
        const float alpha = SmoothStep01((h - kStudioChatH * 0.55f) / (kStudioChatH * 0.45f));
        if (alpha <= 0.01f) return;

        // Remember each question and keep its answer current while it arrives.
        const uint64_t seq = g_lookupRequestSeq.load();
        if (lk.status != LookupStatus::Search && !lk.query.empty()) {
            if (g_chat.empty() || g_chat.back().seq != seq) {
                g_chat.push_back({lk.query, lk, seq});
                if (g_chat.size() > 12) g_chat.erase(g_chat.begin());
            } else {
                g_chat.back().answer = lk;
            }
        }
        if (g_chatLastSeq != seq) {
            g_chatLastSeq = seq;
            g_chatScroll = 0.0f;  // New question or answer: jump to latest
        }

        ComPtr<ID2D1Geometry> mask = CreateIslandMaskGeometry(rect, ContentIslandRadius(h), settings.notchStyle);
        ID2D1Layer* layer = AcquireLayer();
        if (mask && layer) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), alpha, nullptr, D2D1_LAYER_OPTIONS_NONE),
                               layer);
        }
        SHit(rect, StudioAction::ChatPanel);

        // Top: a pill with the mascot and who's answering, and New chat.
        const float topY = rect.top + 8.0f;
        const bool aiBusy = lk.talking || BrainBusyForCurrentCard();
        const std::wstring who = g_compose.kind ? g_compose.label
                                 : settings.bunbunAssistant ? L"Bun \x00B7 on this PC" : L"Quick Lookup";
        const float whoW = SMeasure(who, SFLabel);
        const D2D1_RECT_F pill = D2D1::RectF(rect.left + 16, topY, rect.left + 16 + 26 + whoW + 14, topY + 26);
        SFillRound(pill, 13, D2D1::ColorF(1, 1, 1, 0.08f));
        if (ID2D1SolidColorBrush* b = AcquireBrush(SCol(0x5ee07a))) {
            const D2D1_POINT_2F dot = D2D1::Point2F(pill.left + 14, (pill.top + pill.bottom) * 0.5f);
            if (ID2D1SolidColorBrush* g = AcquireBrush(WithAlpha(SCol(0x5ee07a), 0.3f))) target_->FillEllipse(D2D1::Ellipse(dot, 6, 6), g);
            target_->FillEllipse(D2D1::Ellipse(dot, 3.5f, 3.5f), b);
        }
        SText(who, D2D1::RectF(pill.left + 26, pill.top, pill.right - 8, pill.bottom), SFLabel, SCol(0xe8e8ef));
        // Bunbun: a close button, so the chat can be dismissed with the mouse (Esc still works).
        const D2D1_RECT_F cb = D2D1::RectF(rect.right - 14 - 22, topY + 2, rect.right - 14, topY + 24);
        const float closeHover = SHover(StudioAction::ChatClose);
        SFillRound(cb, 11, D2D1::ColorF(1, 1, 1, SLerp(0.08f, 0.14f, closeHover)));
        {
            const D2D1_COLOR_F xc = SLerpCol(SCol(0xc9c9d2), SCol(0xffffff), closeHover);
            const D2D1_POINT_2F c = D2D1::Point2F((cb.left + cb.right) * 0.5f, (cb.top + cb.bottom) * 0.5f);
            SLine(D2D1::Point2F(c.x - 4.0f, c.y - 4.0f), D2D1::Point2F(c.x + 4.0f, c.y + 4.0f), xc, 1.4f);
            SLine(D2D1::Point2F(c.x + 4.0f, c.y - 4.0f), D2D1::Point2F(c.x - 4.0f, c.y + 4.0f), xc, 1.4f);
        }
        SHit(cb, StudioAction::ChatClose);
        if (!g_chat.empty() && !g_compose.kind) {  // Bunbun: not while replying to an agent
            const float nw = SMeasure(L"New chat", SFLabel) + 22;
            const D2D1_RECT_F nb = D2D1::RectF(cb.left - 8 - nw, topY + 2, cb.left - 8, topY + 24);
            SFillRound(nb, 11, D2D1::ColorF(1, 1, 1, SLerp(0.08f, 0.14f, SHover(StudioAction::ChatNew))));
            SText(L"New chat", nb, SFLabel, SCol(0xc9c9d2), 1);
            SHit(nb, StudioAction::ChatNew);
        }

        // Input pill at the bottom, with the caret and a send button.
        const LookupUiState& ui = g_lookupUi;
        const D2D1_RECT_F input = D2D1::RectF(rect.left + 12, rect.bottom - 56, rect.right - 12, rect.bottom - 10);
        SFillRound(input, 23, D2D1::ColorF(1, 1, 1, 0.09f));
        if (ID2D1SolidColorBrush* rim = AcquireBrush(D2D1::ColorF(1, 1, 1, ui.open ? 0.22f : 0.07f)))
            target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(input.left + 0.5f, input.top + 0.5f, input.right - 0.5f, input.bottom - 0.5f), 22.5f, 22.5f), rim, 1.0f);
        // The mascot sits at the left of the box: listening while you type,
        // thinking while Bun works, talking while the local AI answers.
        float textL = input.left + 18;
        if (settings.bunbunMascot) {
            DrawAskBunMascot(D2D1::RectF(input.left + 8, input.top + 3, input.left + 46, input.bottom - 3), lk, 0.0f, now);
            textL = input.left + 52;
        }
        const float textR = input.right - 50;
        if (ui.text.empty()) {
            SText(g_compose.kind ? L"Type your message and press Enter" : aiBusy ? L"Bun is answering\x2026" : L"Ask Bun anything, or tell it what to do",
                  D2D1::RectF(textL, input.top, textR, input.bottom), SFInput, SCol(0x8a8a96));
        } else {
            if (ui.selectAll) {
                const float sw = std::min(SMeasure(ui.text, SFInput), textR - textL);
                SFillRound(D2D1::RectF(textL - 2, input.top + 12, textL + sw + 2, input.bottom - 12), 4, D2D1::ColorF(0.3f, 0.55f, 1.0f, 0.45f));
            }
            SText(ui.text, D2D1::RectF(textL, input.top, textR, input.bottom), SFInput, SCol(0xffffff));
        }
        const double since = now - ui.caretResetAt;
        if (ui.open && std::fmod(since, LookupLayout::kCaretPeriod) < LookupLayout::kCaretOn) {
            const float cx = std::min(textL + SMeasure(ui.text.substr(0, std::min(ui.caret, ui.text.size())), SFInput), textR);
            SFillRound(D2D1::RectF(cx, input.top + 14, cx + 1.6f, input.bottom - 14), 0.8f, SCol(0xffffff));
        }
        const D2D1_POINT_2F send = D2D1::Point2F(input.right - 25, (input.top + input.bottom) * 0.5f);
        if (ID2D1SolidColorBrush* b = AcquireBrush(D2D1::ColorF(1, 1, 1, ui.text.empty() ? 0.25f : 0.95f)))
            target_->FillEllipse(D2D1::Ellipse(send, 16, 16), b);
        auto P = [](float x, float y) { return D2D1::Point2F(x, y); };
        SPoly({P(12, 17), P(12, 7)}, send, 1.0f, SCol(0x141416), 2.2f);
        SPoly({P(7.5f, 11), P(12, 6.5f), P(16.5f, 11)}, send, 1.0f, SCol(0x141416), 2.2f);

        // Messages, newest at the bottom; older ones scroll off the top.
        const float msgTop = rect.top + 42, msgBottom = input.top - 10;
        const float viewH = msgBottom - msgTop;
        const float msgL = rect.left + 20, msgR = rect.right - 18;
        const float maxChatW = msgR - msgL;
        g_chatChips.clear();
        if (g_compose.kind) {
            // Bunbun: replying to an agent (Reply, Change the plan, Other answer). Not Ask
            // Bun's chat: what you're answering, and where the message goes.
            const bb::AgentView& ag = state.agent;
            const std::wstring title = g_compose.kind == 1 ? L"What should change in the plan?"
                                     : g_compose.kind == 3 ? L"Your own answer"
                                     : L"Message " + (ag.replyTo.empty() ? (ag.agent.empty() ? std::wstring(L"the agent") : ag.agent) : ag.replyTo);
            SText(title, D2D1::RectF(msgL, msgTop + 6, msgR, msgTop + 36), SFSong, SCol(0xffffff));
            float yy = msgTop + 40;
            if (!ag.prompt.empty() && g_compose.kind == 2) {
                SText(L"Last task", D2D1::RectF(msgL, yy, msgR, yy + 18), SFLabel, SCol(0x8a8a96));
                yy += 20;
                const D2D1_RECT_F quote = D2D1::RectF(msgL - 4, yy, msgR, std::min(yy + 44, msgBottom - 26));
                SFillRound(quote, 10, D2D1::ColorF(1, 1, 1, 0.07f));
                SText(L"\x201C" + ag.prompt + L"\x201D", D2D1::RectF(quote.left + 10, quote.top + 4, quote.right - 10, quote.bottom - 4),
                      SFBody, SCol(0xd8d8e0));
                yy = quote.bottom + 8;
            }
            SText(g_compose.kind == 2 ? L"It goes straight to that session, which carries on with it. Enter sends, Esc cancels."
                                      : L"Enter sends it to the agent, Esc cancels.",
                  D2D1::RectF(msgL, std::min(yy, msgBottom - 20), msgR, msgBottom), SFBody, SCol(0x8a8a96));
        } else if (g_chat.empty()) {
            SText(L"Hi! What can I do for you?", D2D1::RectF(msgL, msgTop + 6, msgR, msgTop + 36), SFSong, SCol(0xffffff));
            SText(L"Open apps, set timers, control music, ask about the weather, look up a word\x2026",
                  D2D1::RectF(msgL, msgTop + 36, msgR, msgTop + 56), SFBody, SCol(0x8a8a96));
            // Chips: your recent questions first, then a few ideas.
            for (const auto& r : state.lookupRecent) {
                if (g_chatChips.size() >= 2) break;
                g_chatChips.push_back(r.query);
            }
            for (const wchar_t* idea : {L"Weather tomorrow", L"Timer 25 min", L"Open Spotify", L"Battery"}) {
                if (g_chatChips.size() >= 4) break;
                if (std::find(g_chatChips.begin(), g_chatChips.end(), idea) == g_chatChips.end()) g_chatChips.push_back(idea);
            }
            float cx = msgL - 4;
            const float cy = msgBottom - 30;
            for (size_t i = 0; i < g_chatChips.size(); ++i) {
                const float cw = SMeasure(g_chatChips[i], SFLabel) + 28;
                if (cx + cw > msgR) break;
                const D2D1_RECT_F chip = D2D1::RectF(cx, cy, cx + cw, cy + 28);
                SFillRound(chip, 14, D2D1::ColorF(1, 1, 1, SLerp(0.10f, 0.16f, SHover(StudioAction::ChatChip, static_cast<int>(i)))));
                SText(g_chatChips[i], chip, SFLabel, SCol(0xffffff), 1);
                SHit(chip, StudioAction::ChatChip, static_cast<int>(i));
                cx += cw + 8;
            }
        } else {
            struct MeasuredTurn {
                D2D1_SIZE_F userTextSize{};
                float userBubbleW = 0.0f;
                float userBubbleH = 0.0f;

                bool waiting = false;
                bool tagged = false;
                std::wstring ansText;
                D2D1_SIZE_F ansTextSize{};
                float ansBubbleW = 0.0f;
                float ansBubbleH = 0.0f;

                float totalTurnH = 0.0f;
            };

            std::vector<MeasuredTurn> measured;
            measured.reserve(g_chat.size());
            constexpr float kQAGap = 6.0f;
            constexpr float kTurnGap = 12.0f;
            float totalH = 0.0f;

            for (size_t k = 0; k < g_chat.size(); ++k) {
                const ChatTurn& t = g_chat[k];
                MeasuredTurn mt;

                // User question bubble (right side)
                const float maxUserBubbleW = maxChatW * 0.72f;
                const float maxUserTextW = std::max(60.0f, maxUserBubbleW - 24.0f);
                mt.userTextSize = SMeasureBox(t.question, SFAsk, maxUserTextW);
                mt.userBubbleW = std::min(maxUserBubbleW, mt.userTextSize.width + 24.0f);
                mt.userBubbleH = std::max(28.0f, mt.userTextSize.height + 14.0f);

                // Bun answer bubble (left side)
                const bool latest = (k + 1 == g_chat.size());
                mt.waiting = (t.answer.status == LookupStatus::Loading) ||
                             (latest && aiBusy && t.answer.body.empty());

                if (mt.waiting) {
                    mt.ansBubbleW = 54.0f;
                    mt.ansBubbleH = 30.0f;
                } else {
                    mt.ansText = ChatAnswerText(t);
                    mt.tagged = !t.answer.source.empty() && t.answer.source != L"Bun";
                    const float maxAnsBubbleW = maxChatW * 0.88f;
                    const float maxAnsTextW = std::max(60.0f, maxAnsBubbleW - 24.0f);
                    mt.ansTextSize = SMeasureBox(mt.ansText, SFAnswer, maxAnsTextW);
                    const float tagExtra = mt.tagged ? 16.0f : 0.0f;
                    const float tagW = mt.tagged ? SMeasure(t.answer.source, SFSmall) + 24.0f : 0.0f;
                    mt.ansBubbleW = std::min(maxAnsBubbleW, std::max(mt.ansTextSize.width + 24.0f, tagW));
                    mt.ansBubbleH = std::max(32.0f, mt.ansTextSize.height + 16.0f + tagExtra);
                }

                mt.totalTurnH = mt.userBubbleH + kQAGap + mt.ansBubbleH;
                totalH += mt.totalTurnH;
                if (k + 1 < g_chat.size()) totalH += kTurnGap;
                measured.push_back(mt);
            }

            g_chatScrollMax = std::max(0.0f, totalH - viewH);
            g_chatScroll = Clamp(g_chatScroll, 0.0f, g_chatScrollMax);
            const float scrollOffset = (totalH > viewH) ? (g_chatScrollMax - g_chatScroll) : 0.0f;
            float curY = msgTop - scrollOffset;

            target_->PushAxisAlignedClip(D2D1::RectF(rect.left, msgTop, rect.right, msgBottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            for (size_t k = 0; k < g_chat.size(); ++k) {
                const MeasuredTurn& m = measured[k];
                const ChatTurn& t = g_chat[k];

                // 1) User bubble (right side)
                const float userTop = curY;
                const float userBottom = curY + m.userBubbleH;
                if (userBottom >= msgTop - 4.0f && userTop <= msgBottom + 4.0f) {
                    const float userR = msgR;
                    const float userL = userR - m.userBubbleW;
                    const D2D1_RECT_F ub = D2D1::RectF(userL, userTop, userR, userBottom);
                    SFillRound(ub, 14.0f, D2D1::ColorF(0.18f, 0.19f, 0.23f, 0.95f));
                    if (ID2D1SolidColorBrush* rim = AcquireBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f))) {
                        target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ub.left + 0.5f, ub.top + 0.5f, ub.right - 0.5f, ub.bottom - 0.5f), 13.5f, 13.5f), rim, 1.0f);
                    }
                    if (IDWriteTextFormat* f = SFont(SFAsk)) {
                        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        if (ID2D1SolidColorBrush* b = AcquireBrush(D2D1::ColorF(0.96f, 0.96f, 0.98f, 1.0f))) {
                            target_->DrawTextW(t.question.c_str(), static_cast<UINT32>(t.question.size()), f,
                                               D2D1::RectF(ub.left + 12.0f, ub.top + 7.0f, ub.right - 12.0f, ub.bottom - 7.0f),
                                               b, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                        }
                    }
                }
                curY += m.userBubbleH + kQAGap;

                // 2) Bun answer bubble (left side)
                const float ansTop = curY;
                const float ansBottom = curY + m.ansBubbleH;
                if (ansBottom >= msgTop - 4.0f && ansTop <= msgBottom + 4.0f) {
                    const float ansL = msgL;
                    const float ansR = ansL + m.ansBubbleW;
                    const D2D1_RECT_F ab = D2D1::RectF(ansL, ansTop, ansR, ansBottom);
                    if (m.waiting) {
                        SFillRound(ab, 14.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
                        if (ID2D1SolidColorBrush* rim = AcquireBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f))) {
                            target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ab.left + 0.5f, ab.top + 0.5f, ab.right - 0.5f, ab.bottom - 0.5f), 13.5f, 13.5f), rim, 1.0f);
                        }
                        const float dotCenterY = (ab.top + ab.bottom) * 0.5f;
                        for (int d = 0; d < 3; ++d) {
                            const float ph = static_cast<float>(std::fmod(now * 1.0 + d * 0.15, 1.0));
                            const float lift = std::sin(ph * 3.14159f) * 3.5f;
                            const D2D1_COLOR_F dotCol = WithAlpha(SCol(0x5ee07a), 0.35f + 0.65f * std::sin(ph * 3.14159f));
                            if (ID2D1SolidColorBrush* b = AcquireBrush(dotCol)) {
                                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ab.left + 16.0f + d * 11.0f, dotCenterY - lift), 3.0f, 3.0f), b);
                            }
                        }
                        g_layoutDirty = true;  // keep the dots animating
                    } else {
                        const bool isNoResult = (t.answer.status == LookupStatus::NoResult);
                        const D2D1_COLOR_F bgCol = isNoResult ? D2D1::ColorF(0.35f, 0.12f, 0.10f, 0.40f)
                                                              : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f);
                        const D2D1_COLOR_F rimCol = isNoResult ? D2D1::ColorF(1.0f, 0.4f, 0.35f, 0.25f)
                                                               : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f);
                        SFillRound(ab, 14.0f, bgCol);
                        if (ID2D1SolidColorBrush* rim = AcquireBrush(rimCol)) {
                            target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ab.left + 0.5f, ab.top + 0.5f, ab.right - 0.5f, ab.bottom - 0.5f), 13.5f, 13.5f), rim, 1.0f);
                        }

                        float textTop = ab.top + 8.0f;
                        if (m.tagged) {
                            SText(t.answer.source, D2D1::RectF(ab.left + 12.0f, textTop, ab.right - 12.0f, textTop + 14.0f), SFSmall, SCol(0x9a9aa8));
                            textTop += 15.0f;
                        }

                        IDWriteTextFormat* f = SFont(SFAnswer);
                        if (f) {
                            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                            const D2D1_COLOR_F tc = isNoResult ? SCol(0xffb4a8) : SCol(0xffffff);
                            if (ID2D1SolidColorBrush* b = AcquireBrush(tc)) {
                                target_->DrawTextW(m.ansText.c_str(), static_cast<UINT32>(m.ansText.size()), f,
                                                   D2D1::RectF(ab.left + 12.0f, textTop, ab.right - 12.0f, ab.bottom - 7.0f),
                                                   b, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                            }
                        }
                    }
                }
                curY += m.ansBubbleH + kTurnGap;
            }

            target_->PopAxisAlignedClip();

            // Subtle scrollbar thumb on the right
            if (g_chatScrollMax > 0.0f) {
                const float trackH = viewH;
                const float thumbH = std::max(22.0f, trackH * (viewH / (viewH + g_chatScrollMax)));
                const float thumbY = msgTop + (trackH - thumbH) * (1.0f - (g_chatScroll / g_chatScrollMax));
                const D2D1_RECT_F thumbRect = D2D1::RectF(rect.right - 7.0f, thumbY, rect.right - 4.0f, thumbY + thumbH);
                SFillRound(thumbRect, 1.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f));
            }
        }

        if (mask && layer) target_->PopLayer();
    }

    // ── Idle eyes (Bunbun.IdleFace = eyes) ──
    // The island as a face: two glowing eyes, 17 units apart at a 32-unit bar,
    // drawn at 0.62 of their design size (a 5 x 8 ellipse). Mint by default (or
    // Bunbun.EyeColor); orange and pulsing while an agent works, steady orange
    // when it needs you, green when done, red on an error, and a cross red when
    // poked too often. They follow the pointer by up to 2.2 x 0.8 units and blink
    // for 140 ms every 2.5-6 s, now and then a wink.
    static constexpr uint32_t kEyeColors[6] = {0xe3ffd4, 0xffffff, 0x8fd3ff, 0xffb3d1, 0xd4b3ff, 0xfff2a8};

    void DrawIdleEyes(const SharedState& state, D2D1_POINT_2F c, float height, bb::mascot::Mood mood, double now,
                      double moodSince) {
        EnsureRoundStroke();
        const float u = Clamp(height / 32.0f, 0.4f, 3.0f);  // one design unit at this size
        const float k = 0.62f * u;                         // eye design units

        // Colour and shape follow the agents first, then the mascot's own mood.
        D2D1_COLOR_F col = SCol(kEyeColors[ClampInt(g_settings.eyeColor, 0, 5)]);
        enum class Shape { Normal, Focus, Curious, Surprised, Happy, Closed, Dizzy, Sad } shape = Shape::Normal;
        switch (g_settings.eyeStatusColors ? state.agent.phase : bb::AgentPhase::None) {
            case bb::AgentPhase::Thinking:
            case bb::AgentPhase::Working: {
                const float pulse = 0.3f + 0.7f * (0.5f + 0.5f * std::cos(static_cast<float>(now) / 0.2f));
                col = WithAlpha(SCol(0xff9a1f), pulse);
                shape = Shape::Focus;
                break;
            }
            case bb::AgentPhase::Question: col = SCol(0xff9a1f); shape = Shape::Curious; break;
            case bb::AgentPhase::Approval: col = SCol(0xff9a1f); shape = Shape::Surprised; break;
            case bb::AgentPhase::Finished: col = SCol(0x22e05a); shape = Shape::Happy; break;
            case bb::AgentPhase::Error: col = SCol(0xff3b3b); break;
            default: break;
        }
        switch (mood) {  // the mascot's own reactions win while they last
            case bb::mascot::Mood::Confused: shape = Shape::Dizzy; break;
            case bb::mascot::Mood::Sleepy: shape = Shape::Closed; break;
            case bb::mascot::Mood::Happy: shape = Shape::Happy; break;
            case bb::mascot::Mood::Sad: shape = Shape::Sad; break;
            case bb::mascot::Mood::Alert:
                if (state.agent.phase == bb::AgentPhase::None) { col = SCol(0xff6b6b); shape = Shape::Surprised; }
                break;
            default: break;
        }

        // Pointer follow, straight from the cursor (no easing, like the original).
        float dx = 0.0f, dy = 0.0f;
        if (bb::mascot::GetStyle().eyesFollow) {
            POINT p{};
            if (GetCursorPos(&p) && ScreenToClient(hwnd_, &p)) {
                dx = Clamp((p.x - c.x) / (320.0f * u), -1.0f, 1.0f) * 2.2f * u;
                dy = Clamp((p.y - c.y) / (200.0f * u), -1.0f, 1.0f) * 0.8f * u;
            }
        }

        // Blink: 140 ms, the first at 2.5 s, then every 2.5-6 s; one in 40 is a wink.
        bool blink = false, wink = false;
        if (shape == Shape::Normal || shape == Shape::Focus) {
            static double s_next = -1.0, s_until = -1.0;
            static bool s_wink = false;
            if (s_next < 0.0) s_next = now + 2.5;
            if (now >= s_next) {
                s_until = now + 0.14;
                const double r = std::fmod(std::sin(now * 12.9898) * 43758.5453, 1.0);
                s_next = now + 2.5 + std::fabs(r) * 3.5;
                s_wink = std::fabs(std::fmod(r * 97.0, 1.0)) < 0.025;
            }
            if (now < s_until) { blink = !s_wink; wink = s_wink; g_layoutDirty = true; }
        }

        const float sep = 8.5f * u;
        const float bob = mood == bb::mascot::Mood::Music
                              ? std::sin(static_cast<float>(now) * 6.2831853f / 0.5f) * 1.0f * u : 0.0f;
        const float line = 3.0f * k;

        for (int side = -1; side <= 1; side += 2) {
            const D2D1_POINT_2F e = D2D1::Point2F(c.x + side * sep + dx, c.y + dy + bob);
            // A faint rim of light around a solid eye (TwoCode's small shadow), not a halo.
            if (ID2D1SolidColorBrush* b = AcquireBrush(WithAlpha(col, col.a * 0.16f)))
                target_->FillEllipse(D2D1::Ellipse(e, 5.0f * k + 1.2f * u, 8.0f * k + 1.2f * u), b);
            ID2D1SolidColorBrush* ink = AcquireBrush(col);
            if (!ink) return;
            Shape s = shape;
            if (blink || (wink && side > 0)) s = Shape::Closed;
            switch (s) {
                case Shape::Normal:
                    target_->FillEllipse(D2D1::Ellipse(e, 5.0f * k, 8.0f * k), ink);
                    break;
                case Shape::Focus:
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(e.x, e.y + 1.0f * k), 5.5f * k, 4.0f * k), ink);
                    break;
                case Shape::Curious:
                    target_->FillEllipse(D2D1::Ellipse(e, 6.0f * k, 9.5f * k), ink);
                    break;
                case Shape::Surprised:
                    target_->FillEllipse(D2D1::Ellipse(e, 7.0f * k, 7.0f * k), ink);
                    break;
                case Shape::Happy:  // ^: the top of a circle around (0, 3), 1.15 pi to 1.85 pi
                    SArc(D2D1::Point2F(e.x, e.y + 3.0f * k), 5.0f * k, 3.6128f, 5.8119f, col, line);
                    break;
                case Shape::Closed:  // u: the bottom of a circle around (0, -2); a wink is a flat line
                    if (wink && side > 0 && !blink) SLine(D2D1::Point2F(e.x - 5 * k, e.y + 2 * k), D2D1::Point2F(e.x + 5 * k, e.y + 2 * k), col, line);
                    else SArc(D2D1::Point2F(e.x, e.y - 2.0f * k), 5.0f * k, 0.4712f, 2.6704f, col, line);
                    break;
                case Shape::Sad: {
                    D2D1_MATRIX_3X2_F old;
                    target_->GetTransform(&old);
                    const D2D1_POINT_2F se = D2D1::Point2F(e.x, e.y + 2.0f * k);
                    target_->SetTransform(D2D1::Matrix3x2F::Rotation(side * -25.8f, se) * old);  // 0.45 rad
                    target_->FillEllipse(D2D1::Ellipse(se, 4.5f * k, 6.5f * k), ink);
                    target_->SetTransform(old);
                    break;
                }
                case Shape::Dizzy: {
                    // A two-turn spiral (radius 0.55 per radian) spinning a turn every 0.88 s.
                    const float turn = static_cast<float>(now - moodSince) / 0.14f * side;
                    ComPtr<ID2D1PathGeometry> spiral;
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&spiral)) && SUCCEEDED(spiral->Open(&sink))) {
                        sink->BeginFigure(e, D2D1_FIGURE_BEGIN_HOLLOW);
                        for (float a = 0.3f; a < 12.566f; a += 0.3f) {
                            const float r = a * 0.55f * k;
                            sink->AddLine(D2D1::Point2F(e.x + std::cos(a + turn) * r, e.y + std::sin(a + turn) * r));
                        }
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        target_->DrawGeometry(spiral.Get(), ink, 2.2f * k, studioStroke_.Get());
                    }
                    g_layoutDirty = true;
                    break;
                }
            }
        }
        if (g_settings.eyeStatusColors &&
            (state.agent.phase == bb::AgentPhase::Working || state.agent.phase == bb::AgentPhase::Thinking))
            g_layoutDirty = true;  // the pulse needs frames

        // Sleeping: little z's drifting up.
        if (shape == Shape::Closed && mood == bb::mascot::Mood::Sleepy) {
            const float t = static_cast<float>(std::fmod(now, 2.4) / 2.4);
            SText(L"z", D2D1::RectF(c.x + sep + 4 * u, c.y - 10 * u - t * 6 * u, c.x + sep + 14 * u, c.y - 2 * u - t * 6 * u),
                  SFSmall, WithAlpha(SCol(0xc6e994), 1.0f - t));
        }
    }

    // ── Tuck-away line ──
    void DrawTuckLine(const SharedState& state, D2D1_RECT_F rect, double now) {
        D2D1_COLOR_F c = D2D1::ColorF(1, 1, 1, 0.55f);
        const bb::AgentPhase p = state.agent.phase;
        if (p == bb::AgentPhase::Working || p == bb::AgentPhase::Thinking) c = SCol(0xff9a1f);
        else if (p == bb::AgentPhase::Finished) c = SCol(0x22e05a);
        else if (p == bb::AgentPhase::Error) c = SCol(0xff3b3b);
        else if (state.media.playing) c = D2D1::ColorF(1, 1, 1, 0.85f);
        const float grow = SmoothStep01(Clamp(static_cast<float>(now - g_tuckedAt) / 0.22f, 0.0f, 1.0f));
        const float cx = (rect.left + rect.right) * 0.5f;
        const float w = 56.0f * g_settings.sizeScale * (0.4f + 0.6f * grow);
        const float top = rect.top;
        const float h = 4.0f * g_settings.sizeScale;
        SFillRound(D2D1::RectF(cx - w * 0.5f - 2, top - 1, cx + w * 0.5f + 2, top + h + 1), h, WithAlpha(c, c.a * 0.25f));
        SFillRound(D2D1::RectF(cx - w * 0.5f, top, cx + w * 0.5f, top + h), h * 0.5f, c);
    }
