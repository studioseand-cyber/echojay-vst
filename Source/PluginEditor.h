#pragma once
#include <JuceHeader.h>
#include <array>
#include <set>
#include <map>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "ChainWetKnob.h"
#include "NativeClip.h"
#include "EchoJayAPI.h"
#include "EchoJayLookAndFeel.h"
#include "ParticleVisual.h"
#include "PluginChecklist.h"
#include "EchoJayWorkspace.h"
#include "CodecRender.h"
#include "DashboardTab.h"

// TEMP DEBUG (25 Jul 2026, review-overlay z-order diagnosis) — remove with
// the [zdbg] sites in PluginEditor.cpp.
extern bool gEjReviewModalDbg;
bool gEjZdbgPaint(const char* site, juce::Component& c);

class EchoJayEditor : public juce::AudioProcessorEditor,
                       private juce::Timer,
                       private juce::TextEditor::Listener,
                       public juce::FileDragAndDropTarget,
                       public juce::TooltipClient
{
public:
    EchoJayEditor(EchoJayProcessor&);
    ~EchoJayEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress& key) override;
    
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;

private:
    void timerCallback() override;
    void textEditorReturnKeyPressed(juce::TextEditor&) override;
    
    // displayLabel (optional): short label rendered instead of msg for
    // tap-generated turns (chips, alternative pills). msg always rides the
    // history/send unchanged.
    // turnTypeOverride: staged turnType for tap-generated turns whose ASK
    // choice carried an intent (Phase 3-pre; server trust-but-validates).
    void sendChatMessage(const juce::String& msg,
                         const juce::String& displayLabel = juce::String(),
                         const juce::String& turnTypeOverride = juce::String());
    // ONE injection-attach path for EVERY compose site (chat send, capture,
    // compare) — Phase 3-pre. Returns the validated chain feed (or the
    // full-list fallback) + [CURRENT CHAIN]. A third copy of this logic
    // would drift exactly like the two tH sums did; call sites must add no
    // injection logic of their own. alwaysAttach: capture/compare turns are
    // plugin-relevant BY DEFINITION (their typed text has no cue words);
    // chat stays cue-gated. hadChainFeedOut: true when the validated feed
    // rode (drives chat's turnType staging).
    // targetLinkUid (Phase R): compose against THAT Link's published rack —
    // exactly ONE [CURRENT CHAIN] per turn, always the target's; the local
    // rack never rides a targeted turn. Sidecar absent = rack unknown =
    // build-only (no block, never an error).
    // mainCaptureAttribution (capture turns in a channel chat): the
    // capture/meter data is the MAIN instance's signal — the statement
    // says so explicitly so the model never describes the mix bus as the
    // channel's audio (same failure family as STATE 3 falling to STATE 1,
    // through the capture door).
    // captureOwnAttribution: the capture on this turn IS this channel's own
    // audio (per-channel capture) -> the model may speak about it directly.
    // Mutually exclusive with mainCaptureAttribution (main-instance data in
    // a channel chat). At most one rides.
    juce::String standardChainInjections(const juce::String& typedMsg,
                                         bool alwaysAttach,
                                         bool* hadChainFeedOut = nullptr,
                                         const juce::String& targetLinkUid = juce::String(),
                                         bool mainCaptureAttribution = false,
                                         bool captureOwnAttribution = false,
                                         bool compareAttribution = false);

    // prevReview is non-null when this is not the first capture in the chat.
    void requestAIFeedback(const CaptureSnapshot& snap,
                           const juce::String& chatId,
                           const juce::String& reviewId,
                           const juce::String& passName,
                           int version,
                           const WsReview* prevReview,
                           const juce::String& scopeLinkUid = juce::String());
    void layoutChatMessages();
    
    void showLoginScreen();
    void showMainScreen();
    void attemptLogin();
    void handleLogout();
    bool shouldShowChannelPrompt() const;
    void updateChannelPromptVisibility();
    void selectChannelPromptType(ChannelType type);
    void dismissChannelPrompt();
    
    void showCompareView();
    void hideCompareView();
    void loadReferenceFile();
    void runAICompare();
    void paintCompareView(juce::Graphics& g, juce::Rectangle<int> area);
    // Spectrum panel for Compare tab: independent per-panel state (avoids
    // top/bottom interference). isTop picks the identity: A = cyan heat-map,
    // B = pink/coral comparison colour. Renders via paintSpectrumCurve.
    void paintCompareSpectrum(juce::Graphics& g, juce::Rectangle<int> area,
                              const MeterData& md, bool isLive, bool isTop);
    void showSettingsView();
    void hideSettingsView();
    void saveSettingsToServer();
    void paintSettingsView(juce::Graphics& g, juce::Rectangle<int> area);
    
    // === Section painters matching web app ===
    void paintLoudnessPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);
    void paintLevelsPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);
    void paintStereoPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);
    void paintSpectrumPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);
    void paintTonalBalancePanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);
    void paintCapturesPanel(juce::Graphics& g, juce::Rectangle<int> area);
    void paintWaveformPanel(juce::Graphics& g, juce::Rectangle<int> area);
    
    void drawPanel(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, juce::Colour titleCol);
    void drawHBar(juce::Graphics& g, int x, int y, int w, int h,
                  const juce::String& label, float valuedB, float minDb, float maxDb,
                  juce::Colour startCol, juce::Colour endCol, const juce::String& unit = "dB",
                  float displayValue = -200.0f);
    
    EchoJayProcessor& processorRef;
    
    enum class Screen { Login, Loading, Main };
    Screen currentScreen { Screen::Login };
    
    enum class View { Meters, Visual, Compare, Settings };
    View currentView { View::Meters };

    // V2 tab shell. Dashboard is FIRST, in the enum and in kTabNames below,
    // and the static_assert ties the two together so changing one without the
    // other stops the build instead of misrouting every click at runtime.
    enum class Tab { Dashboard, Visualisation, Meters, Chat, Compare, Link, Chain, Settings };
    // Seeded from the processor in the constructor so a Logic editor recreate
    // (every Link window switch) returns to the tab the user was on. A fresh
    // instance starts on Dashboard, which is what "the default on first launch
    // after update, then last tab remembered" means in a host that destroys
    // editors for fun.
    Tab currentTab { Tab::Dashboard };
    // Top header row height (source/genre/project/Capture/plugin count) and
    // the tab strip height. Bumped for larger, less-cramped typography;
    // topH (= kTopBarH + kTabBarH) is the content-area top everywhere, so the
    // whole layout reflows off these two constants.
    static constexpr int kTopBarH = 38;   // header row (was a hardcoded 32)
    static constexpr int kTabBarH = 29;   // tab strip (was 28; +1 for the ~10px label)

    // ---- tab strip geometry: ONE authority ------------------------------
    //
    // These used to be two hardcoded `constexpr int kTabCount = 7` eleven
    // thousand lines apart, one in paint() and one in mouseDown(), each
    // dividing the width itself. Three things had to agree and nothing made
    // them: the two counts, and the assumption that this label order matches
    // the Tab enum order (mouseDown does static_cast<Tab>(index)). Change one
    // and every click lands on the wrong tab, silently, which is the same
    // shape as the four overlap bugs this file has produced.
    //
    // Now: kTabNames is the only place the count is expressed, tabRects_ is
    // written by resized() alone, and paint() and mouseDown() both consume it
    // and measure nothing.
    // Eight tabs fit at the 900px full-mode floor with room to spare: 112px
    // each against the 81px the longest label (VISUALISATION) needs. Measured
    // 29 Jul 2026. Compact mode draws no strip at all, so there is no narrow
    // case to design for, and no icon-only or overflow mode is wanted.
    static constexpr const char* kTabNames[] = {
        "DASHBOARD", "VISUALISATION", "METERS", "CHAT", "COMPARE", "LINK", "CHAIN", "SETTINGS"
    };
    static constexpr int kTabCount = (int) (sizeof (kTabNames) / sizeof (kTabNames[0]));
    // Ties the label array to the enum. Adding a tab to one and not the other
    // stops the build instead of misrouting clicks at runtime.
    static_assert (kTabCount == (int) Tab::Settings + 1,
                   "kTabNames and the Tab enum must stay in step");

    /** Tab strip rects, in editor coordinates. WRITTEN ONLY BY resized(), via
        layoutTabStrip(). paint() and mouseDown() read these and compute
        nothing of their own. Authored unconditionally, including in the mini
        modes that do not draw the strip, so there is one authority that always
        evaluates rather than a branch leaving stale rects behind. */
    using TabRects = std::array<juce::Rectangle<int>, (size_t) kTabCount>;
    TabRects tabRects_ {};

    /** The arithmetic, as a pure function of the width. STATIC and free of
        editor state on purpose: tools/tabstrip_test calls it directly, so the
        test exercises the code that actually ships rather than a copy of it.
        A copy in a test is the same duplication bug in a different file. */
    static TabRects computeTabRects (int width);

    /** Which rect contains p, or -1. Also static and also pure, for the same
        reason. */
    static int tabIndexIn (const TabRects& rects, juce::Point<int> p);

    /** Lets tools/tabstrip_test reach the two pure helpers above without
        making them public, so the shipped API is unchanged by the existence
        of a test.

        UNCONDITIONAL, with no EJ_SELFTEST guard, deliberately: the ODR note in
        tools/workspace_roundtrip_test/build_and_run.sh is explicit that the
        test define must never appear in a header, because a header that
        compiles differently in two translation units fails as random runtime
        memory corruption rather than a link error. A friend declaration costs
        nothing at runtime and changes no layout, size or vtable. */
    friend struct EchoJayTabStripTestAccess;

    /** The sole geometry author for the strip. Called from resized() before
        any early return, so the rects are valid on every screen. */
    void layoutTabStrip (int width);

    /** Index of the tab under p, or -1 when p is not on the strip. A pure
        lookup against tabRects_: it does not know the width and therefore
        cannot disagree with what was painted. */
    int tabIndexAt (juce::Point<int> p) const;

    /** Unread dot on the DASHBOARD tab label. AUTHORED BY layoutTabStrip, the
        same authority that writes tabRects_, so paint() draws it without
        measuring a label or dividing a width. Empty when the strip has no
        room for it. */
    juce::Rectangle<int> dashUnreadDotRect_;

    // ======================================================================
    //  Session C: the Dashboard tab
    // ======================================================================
    //
    // ONE child component holds the whole surface (see DashboardTab.h), shown
    // by a single unconditional expression in resized(). The editor's job is
    // the three things a view must not do: fetch, cache, and navigate.
    //
    // TWO ENDPOINTS, TWO CADENCES, AND THEY MUST NOT BE CONFUSED:
    //   fetchDashboard   5 Redis reads + 4 Postgres queries. Tab open and the
    //                    payload's own 60s TTL. NOTHING ELSE. Never the poll.
    //   pollCommunity    1 Redis round trip, every 20s, on the PROCESSOR.
    // The payload carries community.unread, which makes polling it for the
    // badge tempting and would be roughly a hundred times the cost per tick.
    juce::Viewport             dashViewport_;
    echojay::DashboardView     dashView_;
    juce::int64                dashFetchedAtMs_ = 0;   // 0 = never fetched
    int                        dashTtlSeconds_  = 60;  // from payload.ttl
    bool                       dashLoading_     = false;
    juce::String               dashError_;
    /** A chat the user deep linked to before the workspace finished loading.
        Consumed once, in workspace.onLoaded. */
    juce::String               pendingDashChatId_;

    /** Tab open. Renders the disk cache immediately, then refreshes behind it
        only if the TTL has expired. Idempotent and cheap when warm. */
    void openDashboardTab();
    void fetchDashboardPayload();
    void applyDashboardJson (const juce::var& payload, juce::int64 fetchedAtMs, bool fromCache);
    /** Interprets an abstract DeepLink: switch tab, select the id. */
    void followDashLink (const echojay::DashLink& link);
    /** Uploaded project art. Procedural art is NEVER fetched: the payload
        sends the seed and ProjectArt draws it locally. */
    void fetchProjectArt (const juce::String& url);

    // THE single writer of the visible tab: strip selection (currentTab),
    // content visibility, sidebar/input visibility, and GL start/stop all
    // change here and nowhere else. force=true runs the full pass even when
    // the tab is unchanged — for paths that must re-assert component state
    // (login completion, compact/mini exits) rather than trust it.
    void switchToTab(Tab t, bool force = false);

    bool compactMode = false;        // chat-only mini window (rightmost header icon)
    Tab  prevTabBeforeCompact_ { Tab::Chat };  // restored on exit

    // Same explicit-layout discipline as the visual mini view: hide EVERY
    // child, then show only what chat-only mode owns (Capture + the chat
    // stack). See applyVisualOnlyVisibility for the rationale.
    void applyCompactVisibility();
    int fullModeWidth = 900;
    int fullModeHeight = 580;
    void toggleCompactMode();
    bool compareVisible = false;
    bool dragHovering = false;
    
    // Visual mode
    bool visualMode = true;          // true when left panel shows particle visual instead of meters
    bool visualOnlyMode = false;     // "screensaver" mode — visual fills whole window, no chat
    Tab  prevTabBeforeVisualOnly_ { Tab::Visualisation }; // restored on exit

    // MINI MODE IS AN EXPLICIT LAYOUT: hide EVERY child component, then show
    // only what the mini view owns (Capture + the visual holder). Painted
    // chrome (logo/badge, mini strip, expand icon) is gated in paint().
    // The old approach hid a hand-picked list, so full-view components
    // (project field, Upgrade button, tab panels) leaked in.
    void applyVisualOnlyVisibility();
    bool abBarShowing = false;       // tracks whether AB transport bar is visible (window resized)
    static constexpr int kAbBarH = 32;
    
    // Spectrum A/B overlay — holds the "other" spectrum when switching between ref and DAW
    std::array<float, 64> heldSpectrum{};
    std::array<float, 64> prevFrameSpectrum{};
    bool heldSpectrumValid = false;
    float heldSpectrumAlpha = 0.0f;
    float heldDbMax = 0.0f, heldDbMin = -66.0f; // frozen dB range at capture time
    bool wasPlayingRef = false;        // tracks ref state changes for spectrum hold
    
    // Spectrum peak hold — slowly falling peak line
    std::array<float, 64> spectrumPeakHold{};
    bool spectrumPeakHoldInit = false;

    // Paint-side frame-to-frame smoothing of the display bins so the curve
    // glides instead of jumping (engine smoothing is time-based per buffer;
    // this is per PAINT, so it also absorbs uneven repaint cadence).
    // 64-bin buffers serve the A/B REFERENCE curve; the live curve renders
    // from the visual FFT below.
    std::array<float, 64> spectrumSmoothed{};
    bool spectrumSmoothedInit = false;

    // ===== Shared spectrum-curve renderer =====
    // ONE render path (median → mean → tilt → region-aware clamped spline →
    // heat-map fill/line) used by the main SPECTRUM panel and both Compare
    // panels, so the look can never drift. Display-only throughout.
    struct SpectrumCurveState {                   // per-surface frame lerp
        std::array<float, MeterEngine::kVisBins> smoothed{};
        bool init = false;
    };
    struct SpectrumCurveStyle {
        bool cyanHeat = true;                     // false: pink/coral comparison identity
        SpectrumCurveState* lerpState = nullptr;  // nullptr = static data, no frame lerp
        std::array<float, MeterEngine::kVisBins>* peakHold = nullptr;  // optional
        bool* peakHoldInit = nullptr;
    };
    // binsIn are LINEAR-frequency dB magnitudes spaced visBinHz apart
    // (the visual-FFT shape). Stored 64-log-bin data enters through
    // expandLog64Spectrum below.
    void paintSpectrumCurve(juce::Graphics& g, int x, int y, int w, int barMaxH,
                            const std::array<float, MeterEngine::kVisBins>& binsIn,
                            double visBinHz, const SpectrumCurveStyle& style);
    static std::array<float, MeterEngine::kVisBins> expandLog64Spectrum(
        const std::array<float, 64>& logBins, double visBinHz);

    SpectrumCurveState spectrumCurveState_;                            // main panel
    SpectrumCurveState compareTopCurveState_, compareBotCurveState_;   // Compare live slots
    std::array<float, MeterEngine::kVisBins> visPeakHold{};            // main panel only
    bool visPeakHoldInit = false;

    // Shared visualiser texture (EchoJayVisualiserTexture.h) — decoded once.
    // SPECTRUM clips it under the curve; SPECTROGRAM samples its diagonal as
    // the colour palette so both views share one material.
    juce::Image visualiserTexture_;
    std::array<juce::Colour, 256> spectroPalette_{};
    bool spectroPaletteInit_ = false;
    void ensureVisualiserTexture();

    // SPECTRUM panel view mode — false = spectrum curve (default), true =
    // scrolling spectrogram waterfall. Toggled in the panel header, persisted
    // to ~/Documents/EchoJay/spectrogram_mode.txt (same pattern as chat scale).
    bool spectrogramMode_ = false;
    void loadSpectrogramMode();
    void saveSpectrogramMode() const;
    // Persistent history image (kSpecHistFrames x kSpectroRows) — one column
    // per new frame, Catmull-Rom interpolated across the 64 log display bins
    // so rows are smooth, then drawn scaled with high resampling quality.
    static constexpr int kSpectroRows = 256;
    juce::Image spectroImg;
    int spectroFrameCounter_ = 0;
    // Header toggle hit rects, cached during paint for mouseDown
    juce::Rectangle<int> spectrumToggleRect_, spectrogramToggleRect_;
    void paintSpectrogramContent(juce::Graphics& g, int x, int y, int w, int h);

    // TONAL BALANCE display smoothing (rel values per macro band)
    std::array<float, 6> tonalSmooth_ {};
    bool tonalSmoothInit_ = false;
    int visualOnlyWidth = 900;
    int visualOnlyHeight = 580;
    // Opaque holder around particleVisual. macOS positions JUCE's OpenGL overlay
    // based on the GL component's window position; if the immediate parent is
    // opaque with the bg colour, the brief blank frame during GL init shows
    // the bg instead of the desktop/host's white. Workaround for known JUCE
    // issue: https://forum.juce.com/t/white-flash-for-the-first-opengl-frame-only-in-logic-pro/28240
    struct ParticleVisualHolder : public juce::Component {
        ParticleVisualHolder() { setOpaque(true); }
        void paint(juce::Graphics& g) override {
            g.fillAll(juce::Colour(0xff080A12));  // C::bg
        }
        void resized() override {
            if (auto* c = getChildComponent(0)) c->setBounds(getLocalBounds());
        }
    };
    ParticleVisualHolder particleVisualHolder;
    
    std::unique_ptr<ParticleVisual> particleVisual;
    void toggleVisualMode();
    void toggleVisualOnlyMode();
    bool wasCapturing = false;       // for detecting capture start/stop transitions
    bool captureAnimTriggered = false;
    
    // Login
    juce::TextEditor emailInput;
    juce::TextEditor passwordInput;
    juce::TextButton loginBtn { "Log In" };
    juce::Label loginErrorLabel;
    juce::Label loginTitle;
    juce::TextButton signUpBtn { "Sign Up" };
    juce::Label signUpLabel;
    bool loginLoading = false;
    // Device pairing (Sign in with browser) — additive; the email/password
    // path above is untouched. pairingActive gates the login-screen swap.
    juce::TextButton browserLoginBtn { "Sign in with browser" };
    juce::Label browserLoginSub;
    juce::Label pairCodeLabel, pairInfoLabel;
    juce::TextButton pairCancelBtn { "Cancel" };
    bool pairingActive = false;
    void startBrowserPairing();
    void cancelBrowserPairing();
    
    // Top bar — left group
    juce::TextButton captureBtn { "Capture" };
    juce::TextButton resetBtn { "Reset" };
    juce::TextButton compareBtn { "Compare" };
    juce::TextButton settingsBtn { "Settings" };
    juce::ComboBox channelTypeBox;
    juce::ComboBox genreBox;
    juce::TextEditor projectInput;  // optional project name — drives pass versioning
    juce::Label statusLabel;
    juce::Label durationLabel;
    juce::Label detectedLabel;
    // Item 2: dim, non-interactive indicator of WHAT a capture will measure
    // (channel name in a channel chat, else the Full capture constant). Sits
    // beside the fixed-width Capture button; the ONLY capture-scope signal on
    // non-chat surfaces (Visualisation/Meters/Settings have no banner).
    juce::Label captureTargetLabel;
    juce::Label passLabel;
    
    // Top bar — right group (stacked)
    juce::Label userLabel;
    juce::Label usageLabel;
    juce::TextButton scanBtn { "Scan Plugins" };
    juce::TextButton logoutBtn { "Log Out" };
    
    // First-open channel prompt (page state; UI is the shared intake set below)
    static constexpr int kChannelPromptGroupCount = 8;
    static constexpr int kChannelPromptOptionCount = 35;
    bool channelPromptVisible = false;

    // Session-level genre prompt — shown once per DAW session across all instances
    // Genre-prompt dismissal lives in the PROCESSOR (serialised with state);
    // the old editor static re-prompted after project reload / process recycle.
    bool genrePromptVisible = false;
    static constexpr int kGenreGroupCount = 4;
    static constexpr int kGenreOptionCount = 36; // built-in genres (excludes Custom button)

    // ---- Centered intake overlay (one question at a time) ------------------
    // ONE shared component set serves all three pages (genre -> channel ->
    // project name): question label on top, free-text input about a third
    // down, popular quick-pick chips below, More / Continue / Skip actions.
    // configureIntakePage() swaps the content in place; every answer routes
    // through the SAME dismiss/select paths the old list UI used, so the
    // processor state and ChainHost session sharing stay the one source of
    // truth.
    juce::Label intakeTitleLabel;
    juce::Label intakeSubLabel;      // muted helper line under the question
    juce::TextEditor intakeInputBox;
    // Chips: every canonical option exactly once, equal treatment, under its
    // canonical group label; the channel page additionally leads with ONE
    // featured Mix Bus chip (colour-highlighted default choice). 40 covers
    // both pages (genre 36, channel 36 incl. the Mix Bus chip).
    static constexpr int kIntakeMaxChips = 40;
    static constexpr int kIntakeMaxGroups = 8;
    std::array<juce::TextButton, kIntakeMaxChips> intakeChipBtns;
    std::array<int, kIntakeMaxChips> intakeChipGroup_ {};  // -1 featured, else group idx
    std::array<juce::Label, kIntakeMaxGroups> intakeGroupLabels;
    juce::TextButton intakeMoreBtn { "More..." };
    juce::TextButton intakeContinueBtn { "Continue" };
    juce::TextButton intakeSkipBtn { "Skip" };
    int intakeConfiguredPage_ = -1;  // last page whose content was built
    int intakeChipCount_ = 0;        // chips in use on the current page
    int intakeFeaturedCount_ = 0;    // leading featured chips (channel: Mix Bus)
    // Reveal sequence, driven by the shared 20Hz editor timer: the question
    // types out first (2 chars per tick, ~25ms per char), then the chips
    // cascade in add-order (3 triggered per tick, ~17ms stagger), each
    // FADING from alpha 0 to 1 in quarters per tick (~150ms, overlapping
    // the stagger) and clickable the moment it starts appearing (setAlpha
    // never affects hit testing). Pure opacity: resized() is the ONE layout
    // authority for bounds/visibility and nothing animates position; the
    // timer only advances counters, flips visibility and steps alphas.
    juce::String intakeTitleFull_;
    int intakeTitleShown_ = 0;
    bool intakeContentRevealed_ = false;
    int intakeChipsShown_ = 0;       // cascade progress (chips triggered)
    int intakeCascadeTick_ = 0;      // ticks since the cascade started
    std::array<bool, kIntakeMaxChips> intakeChipFits_ {};
    std::array<bool, kIntakeMaxGroups> intakeGroupFits_ {};
    void configureIntakePage(int page);
    void submitIntakeInput();
    void showIntakeMoreMenu();

    // ---- ONE modal onboarding component ------------------------------------
    // Full editor bounds, paints its OWN scrim+card (no reliance on the
    // editor's paint chain), swallows all mouse events, re-fronted on every
    // show and tab switch. The three pages' components are PARENTED INSIDE
    // it — its local coordinates equal editor coordinates, so the page
    // layout code is unchanged. channel/genre/projectPromptVisible are
    // DERIVED page state written ONLY by updateOnboardingPrompts().
    struct OnboardingOverlay : juce::Component
    {
        // THE single source of truth for which page renders: 0 none,
        // 1 channel, 2 genre, 3 project. Written ONLY by
        // updateOnboardingPrompts(); paint() and component visibility both
        // derive from it, so they cannot disagree.
        int currentPage = 0;
        int lastPaintedPage = -1;   // paint-side change log throttle
        std::function<void(juce::Graphics&)> paintScrim;
        void paint(juce::Graphics& g) override { if (paintScrim) paintScrim(g); }
        void mouseDown(const juce::MouseEvent&) override {}   // swallow
    };
    OnboardingOverlay onboardingOverlay_;

    // Project-name prompt (shown ONCE when the instance has no own name AND
    // no shared session value exists) + session sharing. UI is the shared
    // intake set above; the free-text answer is read from intakeInputBox.
    bool projectPromptVisible = false;
    juce::String lastSeenSharedProject_;   // previous shared value (follow rule)
    juce::String lastSeenSharedGenre_;     // session genre follow (flag-keyed)
    bool shouldShowProjectPrompt() const;
    void updateProjectPromptVisibility();
    void dismissProjectPrompt(bool accepted);
    void publishProjectName();

    // While ANY onboarding prompt is up: child components paint ABOVE the
    // editor's paint() scrim, and the visualiser's GL surface composites
    // over everything regardless of component visibility — occlude them
    // (visual hidden AND stopped, sidebar hidden); restore on dismiss.
    void applyPromptOverlayOcclusion();

    // ONE show path for all three onboarding prompts (channel -> genre ->
    // project). Evaluates the chain in order, applies ALL component
    // visibility, chat/topbar treatment, and ONE occlusion pass computed
    // from the FINAL combined state — the old per-prompt update functions
    // ran occlusion mid-chain, so a genre-dismiss briefly RESUMED the GL
    // visual before the project prompt's update stopped it again, and the
    // resulting attach/detach race left the Orb compositing over the
    // project prompt. The per-prompt update functions are thin forwarders;
    // the only differences between prompts are text, input-vs-buttons, and
    // which flag they set.
    void updateOnboardingPrompts();

    // ONE source of truth for the content/sidebar column split, used by
    // BOTH paint() and resized(). They previously had divergent formulas
    // (35% 280-420 painted vs 32% 240-380 laid out), which put the chat
    // input row short of the painted column edge — the third input-row
    // alignment bug, and the last: there is now exactly one formula.
    struct ColumnLayout { int chatW = 0, mW = 0; };
    ColumnLayout computeColumns(int width) const;

    // CHAT tab empty-state centred layout (Claude/ChatGPT pattern): when the
    // ACTIVE chat has no messages, the input renders centred in the message
    // area with a greeting above it; one or more messages = the docked
    // layout. Flag + text rects are computed by resized() (ONE formula);
    // paint() only reads them. Compare/Chain sidebars and the compact
    // window keep the docked layout in both states (too narrow).
    bool chatCentredEmpty_ = false;
    juce::Rectangle<int> chatEmptyHeading_, chatEmptySub_;

    // FREE V2 premium lock (two-lane contract): when the monthly premium
    // pool is spent (no credits), Link/Chain/Compare show a lock strip —
    // visible with an upgrade affordance, never hidden tabs. The strip is
    // painted (banner pattern); its CTA is the ONE shared upgradeBtn,
    // positioned by the timer (chat-input gate takes priority when the
    // chat lane is ALSO spent). Chat stays usable on its own daily lane.
    bool premiumLocked() const
    {
        return api.isLoggedIn()
            && api.getUserInfo().usagePool.twoLane()
            && api.isPremiumExhausted();
    }
    juce::Rectangle<int> premiumLockRect_, premiumLockBtnRect_;

    // Model-tier banner: ONE shared implementation on every assistant
    // surface (assistantInputContext — chat tab, Compare/Chain/etc sidebars,
    // compact window) when tier is free, usagePool.model.fast is true and
    // tasteRemaining is 0. Docked/sidebar: above the input, pushing the
    // message list bound up; centred empty chat: below the input (above it
    // collides with the subtitle). Dismissal is GLOBAL for the session (one
    // per-process static flag — an editor rebuild stays dismissed);
    // reappears next session. Upsell treatment, not a warning.
    juce::Rectangle<int> chatBannerRect_, chatBannerCloseRect_;
    bool chatBannerVisible_ = false;
    static bool fastModelBannerDismissed_;
    bool shouldShowFastModelBanner() const
    {
        const auto& up = api.getUserInfo().usagePool;
        return !fastModelBannerDismissed_ && api.isLoggedIn()
            && api.getUserInfo().tierLevel == 0
            && up.present && up.modelFast && up.tasteRemaining <= 0;
    }

    // Host audio liveness (Compare transport honesty): sampled in the timer
    // from the processor's block counter. Compare playback renders in
    // processBlock, so a play press while the host has idled the channel
    // cannot sound — the play state must not pretend (hint shown instead).
    uint32_t cmpLastBlockCount_ = 0, cmpLastBlockAdvanceMs_ = 0;
    uint32_t cmpHintUntilMs_ = 0;
    int cmpSyncDiagTick_ = 0;   // 1Hz sync-follow diagnostic throttle
    bool hostAudioAlive() const
    { return juce::Time::getMillisecondCounter() - cmpLastBlockAdvanceMs_ < 500; }

    // ONE source of truth for "an assistant input row exists here", used by
    // the out-of-credits gate (upgrade button replaces the row). Every tab
    // with the sidebar counts — Chat, Compare, Meters, Visualisation, Link,
    // Chain (uncollapsed) — plus the chat-only compact window. A hand-listed
    // subset here is exactly how Meters/Vis/Link once kept accepting text
    // while Chat showed the upgrade prompt.
    bool assistantInputContext() const;
    // ONE predicate for "the AI sidebar is painted on this tab" — the single
    // gate for every assistant-DRAWN element (chain Build buttons, gain cards,
    // wave cards). Consulted by BOTH paint() (early-out + cleanup) and
    // mouseDown() (hit-test early-out), so an element can never be drawn where
    // it isn't clickable, or clickable where it isn't drawn. False on Settings,
    // visual-only, and any tab computeColumns gives no chat column.
    bool assistantSidebarVisible() const;


    // ---- Settings right column (ACCOUNT / visual / slim info card) ----
    // Rects computed in resized(), painted in paintSettingsView(). Two-column
    // mode: ACCOUNT on top, the ambient visual card (no chrome) filling the
    // middle, and one slim card below holding WHAT'S NEW + THIS MONTH
    // compressed side by side (settingsWhatsNewCard_/settingsMonthCard_ are
    // its left/right halves). Below ~1100px the cards stack under the form
    // as a row and the visual is omitted (no room to breathe).
    juce::Rectangle<int> settingsAccountCard_, settingsWhatsNewCard_, settingsMonthCard_;
    juce::Rectangle<int> settingsVisualCard_;    // ambient logo-O particle field
    juce::Rectangle<int> settingsInfoCard_;      // slim combined info card
    juce::Rectangle<int> settingsUpgradeRect_;   // Upgrade button slot in ACCOUNT
    // ONE vertical-stack layout for the ACCOUNT card, shared by resized()
    // (card height + upgrade button rect) and paintSettingsView() (element
    // Ys), so they can't disagree. All Ys are RELATIVE to the card top.
    // statusYRel/buttonYRel are -1 when that element doesn't apply to the tier.
    struct AccountLayout { int cardH, barsTop, oneBarH, barGap, statusYRel, buttonYRel; };
    AccountLayout accountLayout(int tierLevel, bool twoLane) const;

    // Ambient Settings visual: the logo's "O" as a breathing particle field.
    // Point set is derived ONCE from the embedded logo PNG's alpha channel
    // (never hand-drawn); particles drift around home positions, the whole
    // glyph breathes (~7s), audio gently modulates breath depth/brightness
    // and the six macroBands light angular regions (sub = bottom arc,
    // air = top). 15fps timer runs ONLY while the card is visible.
    struct SettingsOrbCard : juce::Component, private juce::Timer
    {
        // Master reactivity — ONE knob for how hard audio moves the field
        // (scales energy expansion, band turbulence, sparkle). Tune by feel.
        static constexpr float kReactivity = 1.0f;

        // px/py are per-particle PHASE ACCUMULATORS advanced each tick —
        // never sin(bigTime * f): at system-uptime magnitudes float only
        // resolves ~0.06s (visible jitter), and turbulence needs per-tick
        // speed changes without phase jumps anyway.
        struct Pt { float hx = 0, hy = 0;            // home, glyph-normalised
                    float fx = 0, fy = 0, px = 0, py = 0, amp = 0;  // drift
                    float size = 1, bright = 1, band = 2.5f; };
        std::vector<Pt> pts;
        // Transient sparkle pool — fixed size, no allocation per burst
        struct Spark { float hx = 0, hy = 0, size = 2.0f, life = 0.0f; };
        std::vector<Spark> sparks;                   // sized once in buildFromLogo
        juce::Random sparkRng { 0x51A2 };
        int sparkCooldown = 0;
        float breathPhase = 0.0f, loudSm = 0.0f;
        // Energy for the expansion layer: fast attack (~1 tick ≈ 50-70ms),
        // slow release (~400ms) — separate from loudSm's gentler curve
        float energySm = 0.0f;
        int   tickCount = 0;
        bool  hadSignal = false;
        std::array<float, 6> bandSm { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
        // returns true when signal present; fills loudness 0..1, transient
        // 0..1 (momentary rising over short-term), band rels 0..1
        std::function<bool(float&, float&, std::array<float, 6>&)> fetchAudio;
        SettingsOrbCard() { setOpaque(true); setInterceptsMouseClicks(false, false); }
        void buildFromLogo();                        // alpha-channel extraction, logs bounds
        void ensureTimerMatchesVisibility() { if (isVisible() && isShowing()) { if (!isTimerRunning()) startTimerHz(15); } else stopTimer(); }
        void visibilityChanged() override { ensureTimerMatchesVisibility(); }
        void parentHierarchyChanged() override { ensureTimerMatchesVisibility(); }
        void timerCallback() override;
        void paint(juce::Graphics& g) override;
    };
    SettingsOrbCard settingsOrbCard_;
    juce::var whatsNewEntries_;                  // fetched/cached release notes
    bool whatsNewFetched_ = false;
    // Local monthly usage counters (monthly_stats.json, month-keyed, shared
    // across instances; no server involvement)
    int statCaptures_ = 0, statChats_ = 0, statChains_ = 0;
    void loadMonthlyStats();
    void bumpMonthlyStat(const juce::String& key);
    void updateGenrePromptVisibility();
    void dismissGenrePrompt(const juce::String& selectedGenre);
    void paintGenrePromptOverlay(juce::Graphics& g, juce::Rectangle<int> bounds);
    bool shouldShowGenrePrompt() const;
    void rebuildGenreBox();
    
    // Custom genres added by the user
    juce::StringArray customGenreNames;
    void addCustomGenreToList(const juce::String& name);
    void loadCustomGenres();
    void saveCustomGenres();
    
    // Custom channel names added by the user
    juce::StringArray customChannelNames;
    void addCustomChannelToList(const juce::String& name);
    void rebuildChannelTypeBox();
    void loadCustomChannels();
    void saveCustomChannels();

    // Compare
    juce::TextButton loadRefBtn { "+ Add Mix" };
    juce::TextButton aiCompareBtn { "AI Compare" };
    // VESTIGIAL — NOT the source of truth. These boxes are never made visible
    // and never given bounds; they are still populated on rebuild only to keep
    // legacy code compiling. The Compare source of truth is compareTop_ /
    // compareBot_ (the slot buttons), read via getSlotMeterData(). Do NOT wire
    // AI Compare, audition, or any new logic to their getSelectedId(): from
    // v2.9.31 (2 Jul 2026) until it was repaired, runAICompare read exactly
    // this hidden selection and analysed the wrong audio. Read the slots.
    juce::ComboBox compareSlotABox;
    juce::ComboBox compareSlotBBox;
    juce::Label refStatusLabel;
    // Stage 1: meter-type selector (Waveform / Spectrum / Levels / Stereo Image / Loudness)
    std::array<juce::TextButton, 5> compareMeterBtns;
    int compareMeterId_ = 0; // 0=Waveform, 1=Spectrum, 2=Levels, 3=Stereo, 4=Loudness

    // Stage 2: per-slot source selection
    struct CompareSlotState {
        enum class Kind { Empty, Live, Snapshot, WsCapture, Reference, CodecFile };
        Kind kind = Kind::Empty;
        int  index = -1;          // Snapshot or Reference index
        juce::String wsReviewId;  // WsCapture: review ID from workspace
        juce::String label;       // display name shown in the slot button
        juce::String codecPath;   // CodecFile: rendered temp wav path
        std::vector<float> codecThumb;  // CodecFile: abs-peak thumbnail
    };
    CompareSlotState compareTop_, compareBot_;
    juce::TextButton compareTopSlotBtn_;
    juce::TextButton compareBotSlotBtn_;
    void openCompareSlotMenu(bool isTop);
    // Item 1: ordered review IDs backing the compare-slot menu's CHAT
    // CAPTURES section (project-scoped, deduped). The result handler indexes
    // this directly instead of re-deriving by iteration, so the label the
    // user picked and the review the slot binds can never drift.
    std::vector<juce::String> compareMenuReviewIds_;
    // Scope-labelled name for a capture review: "Link <name> vN" only when
    // the data is genuinely the channel's (channelDataScoped); otherwise
    // "Full capture vN - HH:MM" (a pre-fix channel record's numbers ARE the
    // full mix, so it labels as a full capture — the label follows the data).
    juce::String compareReviewLabel(const WsReview& rev) const;
    // Item 3: relative-then-absolute date string for the menu's
    // shortcutKeyDescription field ("Today 07:23" / "Yesterday 18:06" /
    // "27 Jul 07:23"). Empty when the date is unparseable.
    juce::String compareEntryDate(const juce::String& iso) const;
    void updateCompareSlotBtn(bool isTop);
    MeterData getSlotMeterData(const CompareSlotState& slot) const;
    // Item 6: a slot's DATA scope. channelDataScoped only for a WsCapture
    // review carrying the marker; everything else (snapshot, reference, live,
    // pre-fix review) is full-scope. The cross-scope guard keys off THIS,
    // never off linkUid presence.
    bool slotIsChannelScoped(const CompareSlotState& slot) const;
    // Item 4: the channel uid a slot's DATA belongs to ("" = full-capture /
    // live / non-channel). Used to decide same-scope vs cross-scope against
    // the active channel chat.
    juce::String slotChannelUid(const CompareSlotState& slot) const;
    // Step 1: the display label + duration for a slot, so the compare context
    // reads the SAME source the buttons and gate read (getSlotMeterData). Label
    // is the scope-labelled review name (compareReviewLabel) for a WsCapture,
    // else the snapshot/reference name or "Live signal". Duration is 0 when
    // unknown (Live), which the length-mismatch caveat treats as "skip".
    juce::String slotDisplayName(const CompareSlotState& slot) const;
    float slotDurationSeconds(const CompareSlotState& slot) const;
    // Step 2: cross-scope covers all three cases the send must ask about -
    // channel-vs-full, channel-vs-DIFFERENT-channel, and anything-vs-Live.
    // Keys off channelDataScoped (via slotChannelUid), never linkUid presence.
    bool crossScope(const CompareSlotState& a, const CompareSlotState& b) const;
    // Step 3: the cross-scope decision for a compare run. Unasked is the
    // initial press — it triggers the ASK when the slots are cross-scope.
    // Anyway / NumbersOnly are the two chip outcomes, both of which mean the
    // user already chose, so the ASK is skipped (no re-prompt loop).
    enum class CompareScope { Unasked, Anyway, NumbersOnly };
    // Step 1/3: the compare body, parameterised on the two slots and the
    // cross-scope decision. runAICompare() reads the visible slots and calls
    // this Unasked; a cross-scope press routes through presentCompareScopeAsk
    // first, whose chip re-enters here with Anyway or NumbersOnly.
    void runAICompareWith(const CompareSlotState& slotA,
                          const CompareSlotState& slotB,
                          CompareScope scope);
    // Step 3: client-side deterministic ASK shown when the two loaded slots
    // are cross-scope. Pushes a local ASK message (cmp_anyway / cmp_numbers
    // intents) onto the chat and persists it, so the choice survives an editor
    // recreate instead of living only on the editor.
    void presentCompareScopeAsk(const CompareSlotState& slotA,
                                const CompareSlotState& slotB);
    static MeterData meterDataFromWsReview(const WsReview& r);
    void paintCompareWaveform(juce::Graphics& g, juce::Rectangle<int> area,
                              const CompareSlotState& slot, bool isPlaying);

    // Stage 3: transport bar controls
    juce::TextButton comparePlayTopBtn_;   // per-header play (kept for direct play)
    juce::TextButton comparePlayBotBtn_;
    juce::TextButton compareSyncBtn_;      // SYNC toggle: capture follows DAW transport
    juce::TextButton cmpABtn_ { "A" };     // transport bar: select side A (top)
    juce::TextButton cmpBBtn_ { "B" };     // transport bar: select side B (bottom)
    juce::TextButton cmpPlayBtn_;          // transport bar: play/pause selected side
    void toggleComparePlay(bool isTop);    // play/pause a capture slot
    void toggleCompareAudible(bool isTop); // toggle which slot is audible
    void updateComparePlayBtns();
    void updateTransportBar();             // refresh A/B/play/sync appearance
    void startCompareStream(int slot);     // load WAV into compare stream (no auto-play)
    bool bothSlotsAreCaptures() const;     // true when neither slot is Live or Empty
    // Returns full path, "WEB" (web-only, no local file), or "" (no audio / empty)
    juce::String resolveSlotWavPath(const CompareSlotState& slot) const;

    // ---- Codec Player (COMPARE, phase 1) -----------------------------------
    // Offline render of the source through a real codec (AAC via AudioToolbox
    // on mac, Ogg via JUCE's bundled libvorbis), then codec mode: top=ORIGINAL,
    // bottom=codec render, lockstep via the existing two-captures machinery.
    // ONE modal panel component (onboarding pattern: paints its own scrim+card,
    // swallows mouse, ESC/X closes). Layout rects computed in paint() (one
    // formula), stored for hit testing.
    struct CodecPanel : juce::Component
    {
        EchoJayEditor* owner = nullptr;
        void paint(juce::Graphics& g) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;
        void mouseDown(const juce::MouseEvent&) override {}   // swallow
        bool keyPressed(const juce::KeyPress& k) override;
        std::vector<juce::Rectangle<int>> cardRects;
        juce::Rectangle<int> normRect, closeRect;
        int hoverIdx = -1;
    };
    CodecPanel codecPanel_;
    // Feature-launcher button: right-aligned against the panel edge (NOT part
    // of the centred transport cluster). Custom-painted: codec glyph
    // (waveform between brackets) + label, subtle cyan 1px outline that
    // brightens on hover and stays lit while codec mode is engaged. No solid
    // fill — it must read as a doorway, not a toggle, and must not compete
    // with Send/Upgrade.
    struct CodecLaunchBtn : juce::Button
    {
        CodecLaunchBtn() : juce::Button("CODECS") {}
        bool active = false;   // codec mode engaged (written by updateTransportBar)
        void paintButton(juce::Graphics& g, bool over, bool down) override;
    };
    CodecLaunchBtn codecsBtn_;
    bool codecNormalise_ = true;                 // panel toggle, default ON
    int  codecRendering_ = -1;                   // preset index while rendering
    juce::String codecStatus_;                   // error line on the card
    juce::String codecSrcPath_, codecSrcLabel_;  // resolved on panel open
    bool codecSrcIsTopSlot_ = false;
    bool codecModeActive_ = false;
    CompareSlotState codecSavedTop_, codecSavedBot_;  // restored on chip X
    juce::String codecChipLabel_;
    juce::Rectangle<int> codecChipX_;            // painted chip close zone
    void openCodecPanel();
    void closeCodecPanel();
    void resolveCodecSource();
    void startCodecRender(int presetIdx);
    void enterCodecMode(int presetIdx, bool normalised, const CodecRender::Result& res);
    void exitCodecMode();

    // Reference Presets
    juce::ComboBox presetBox;
    juce::TextButton savePresetBtn { "Save Preset" };
    juce::TextButton deletePresetBtn { "Delete" };
    void loadPresetList();
    void saveCurrentPreset(const juce::String& name);
    void loadPreset(const juce::String& name);
    void deletePreset(const juce::String& name);
    juce::File getPresetsFolder();
    juce::StringArray presetNames;
    
    // Loudness panel bounds for click-to-reset
    juce::Rectangle<int> loudnessPanelBounds;
    
    // Waveform click positions — stored during paint, overlays positioned in timer
    struct CompareWavePos { juce::Rectangle<int> bounds; juce::String wavPath; float duration; };

    // Compare static waveform seek areas — inner rect of each panel's waveform
    struct CmpWaveSeekArea { juce::Rectangle<int> inner; int slotIdx; };
    std::array<CmpWaveSeekArea, 2> cmpWaveSeekAreas_ = {};
    
    // Chat wave card positions for direct mouseDown hit testing (Windows overlay workaround)
    std::vector<CompareWavePos> chatWavePositions;
    
    // Transparent click catcher for compare waveform area — forwards file drags to parent
    struct DragForwardingComponent : public juce::Component, public juce::FileDragAndDropTarget
    {
        bool isInterestedInFileDrag(const juce::StringArray& files) override
        {
            if (auto* p = dynamic_cast<juce::FileDragAndDropTarget*>(getParentComponent()))
                return p->isInterestedInFileDrag(files);
            return false;
        }
        void filesDropped(const juce::StringArray& files, int x, int y) override
        {
            if (auto* p = dynamic_cast<juce::FileDragAndDropTarget*>(getParentComponent()))
                p->filesDropped(files, x + getX(), y + getY());
        }
        void fileDragEnter(const juce::StringArray& files, int x, int y) override
        {
            if (auto* p = dynamic_cast<juce::FileDragAndDropTarget*>(getParentComponent()))
                p->fileDragEnter(files, x + getX(), y + getY());
        }
        void fileDragExit(const juce::StringArray& files) override
        {
            if (auto* p = dynamic_cast<juce::FileDragAndDropTarget*>(getParentComponent()))
                p->fileDragExit(files);
        }
    };
    DragForwardingComponent compareClickCatcher;    static constexpr int kMaxRefRemoveBtns = 8;
    std::array<juce::TextButton, kMaxRefRemoveBtns> refRemoveBtns;
    int activeRefRemoveBtns = 0;
    int lastRefCount = 0; // track ref changes for auto-refresh
    
    // Settings
    juce::TextEditor settingsName;
    juce::TextEditor settingsMonitors;
    juce::TextEditor settingsHeadphones;
    juce::TextEditor settingsGenres;
    juce::TextEditor settingsPlugins;
    juce::ComboBox settingsExpLevel;
    // Chat language picker — controls what language the AI replies in.
    // Persisted locally via EchoJayAPI::setChatLanguage(); doesn't sync to
    // the SaaS (intentionally — it's a per-device preference, and adding
    // server sync would mean a new SaaS field and a new endpoint).
    juce::ComboBox settingsLanguage;
    std::array<juce::ToggleButton, 11> dawButtons;
    juce::TextButton saveSettingsBtn { "Save" };
    juce::Label settingsSavedLabel;
    juce::ComboBox uiScaleCombo;     // UI scale picker: 80–150%
    // Auto-dial mode (default off): opt-in restriction of chain suggestions
    // to plugins EchoJay can dial in automatically (server param maps).
    // State lives in EchoJayAPI (local settings file) and rides chat bodies;
    // the toggle applies immediately, independent of the Save button.
    juce::ToggleButton autoDialToggle { "Only suggest plugins EchoJay can auto-dial (fewer options, every suggestion one-click)" };
    float uiScale_ = 1.0f;          // current scale factor
    void applyUIScale(float scale);
    void saveUIScale() const;
    void loadUIScale();
    static juce::File getUIScaleFile();
    
    // Chat
    struct ChatMsg {
        juce::String role, content;
        juce::String reviewId;    // non-empty for capture messages; key into workspace.reviews
        bool hasWaveform = false;
        std::vector<WaveformRecorder::ThumbnailPoint> waveform;
        juce::String wavFilename;
        juce::String wavFilePath;
        juce::String audioUrl;    // remote URL (web captures)
        juce::String origin;      // "plugin" | "" (web) — controls playback UI
        float durationSeconds = 0;
        float lufs = -100;
        juce::String chainData;   // non-empty when AI returned a <<<ECHOJAY_CHAIN>>> block
        juce::String gainData;    // non-empty when AI returned a <<<ECHOJAY_GAIN>>> block
        juce::String askData;     // non-empty when AI returned an <<<ECHOJAY_ASK>>> block
        bool askAnswered = false; // chip tapped — chips render disabled/hidden
        juce::String editData;    // non-empty when AI returned <<<ECHOJAY_CHAIN_EDIT>>>
        bool editApplied = false; // Apply pressed (or edit aborted) — card retired
        juce::String editResult;  // outcome summary shown on the retired card
        juce::String editAltPrompt; // "Suggest an alternative" follow-up (load
                                    // failures only); cleared once tapped
        juce::String editAltLabel;  // short display label for the alt tap
        // Result-bubble "stop suggesting" chip (build failures): the failed
        // plugin names. NOT persisted — the exclusion itself is session-
        // scoped (chainFailSessionSeen_), and the chip is a just-failed-now
        // action, so a reloaded bubble keeps its alt chip but not this one.
        juce::StringArray excludeNames;
        bool excludeApplied = false;
        juce::String displayText;   // tap-generated user turns: what the
                                    // bubble SHOWS; content is what was SENT
                                    // (empty = show content, i.e. typed text)
        int  editBaseRevision = -1; // revision at receive; -1 = restored from a
                                    // previous session (revision guard skipped,
                                    // baseSlots check still applies).
                                    // WHICH counter depends on editTargetUid:
                                    // empty -> the LOCAL ChainHost revision;
                                    // set   -> that LINK's sidecar revision.
                                    // The two are separate racks' counters and
                                    // are NEVER compared to each other.
        juce::String editTargetUid;  // Phase R: set when this edit card targets a
                                     // Link's rack — Apply routes v:2 to that Link,
                                     // never the local sequencer
        juce::String editTargetName; // display label at propose time (user text)
        // Item 3/4: a channel capture has no host waveform/audio and no
        // per-channel audio routed yet — the card shows no player and an
        // HONEST reason, never "Audio not on this device" (which implies a
        // lost file).
        bool channelCaptureNoAudio = false;
        // Item 6: STRUCTURAL cross-scope guard. Set when a compare turn in a
        // CHANNEL chat carries full-capture data — the chain card renders
        // WITHOUT a Build button (a channel chain from full-mix numbers must
        // be unreachable, not merely discouraged by the injection).
        bool chainBuildSuppressed = false;
        // AI Compare figure card: both sources' figures + labels + cross flag,
        // built client-side at compose time (buildCompareFiguresJson). Rendered
        // as the figure card; persisted on the message so a reloaded chat
        // redraws it identically (the prose no longer restates the numbers).
        juce::String figuresData;
    };
    std::vector<ChatMsg> chatMessages;
    // THE shared display source: both text-layout passes (measure + paint)
    // MUST get their string from here so heights and pixels cannot disagree.
    static const juce::String& displayedText(const ChatMsg& m)
    { return m.displayText.isNotEmpty() ? m.displayText : m.content; }
    bool chatLoading = false;

    // ---- Staged replies (Phase 1d): working-state stage row -----------------
    // The shimmer status renders as the END-OF-LIST row (where "Analysing..."
    // lived) — deliberately NOT a message, so message heights never change as
    // stages appear/swap. That sidesteps the two-tH-sums bug class outright
    // (ASK chips, then the ops card, both from one pass missing a term):
    // stageRowH() is the ONE height source and BOTH the measure pass and the
    // paint pass consume it.
    // SHIMMER PARITY (keep in sync with app.html .stage-shimmer CSS): dim
    // text3 base, bright 0xff7FE3F2 band, 1.6s sweep cycle.
    juce::String stageStatusText_;      // real event labels while applying;
                                        // generic-safe line during model wait
    juce::Rectangle<int> stageRowRect_; // last painted row (ticker repaints it)
    int stageRowH() const { return (chatLoading || stageStatusText_.isNotEmpty()) ? 30 : 0; }
    void setStageStatus(const juce::String& s);
    void clearStageStatus();
    // Result stage: a local assistant bubble (persisted, block-less).
    // altPrompt (optional) attaches a Suggest-an-alternative pill to the
    // bubble (build failures) - same one-shot machinery as edit cards.
    void appendLocalResultBubble(const juce::String& text,
                                 const juce::String& altPrompt = juce::String(),
                                 const juce::String& altLabel  = juce::String(),
                                 const juce::StringArray& excludeNames = {});
    // ---- Apply-time honesty (26 Jul 2026) ----
    // Clean-load chain builds defer the result bubble until per-slot dial
    // state settles (async map fetches; 250ms polls, ~2s cap). Only a clean
    // FULL dial relays the model's "result" line; anything else composes
    // factual wording naming the hand-dial slots and controls. On timeout
    // the conservative wording is used, never the model line.
    void finishChainBubbleWhenDialSettled(const juce::String& chainJson, int attemptsLeft);
    // Non-clean-load paths keep their factual bubbles but still log
    // dial_miss events once dial state settles.
    void logDialMissesWhenSettled(int attemptsLeft);
    // events.jsonl dial_miss writer (EchoJayEventLog schema v1).
    void logDialMiss(const juce::String& plugin, const juce::String& fp,
                     const juce::String& reason, const juce::StringArray& manual);
    // Alt pill on PLAIN messages (result bubbles): height helper shared by
    // the measure and paint passes (edit cards carry their pill inside
    // editCardHeight; this returns 0 for them)
    int altPillH(const ChatMsg& msg) const
    {
        // Result-bubble chip ROW height (single source). One 32px row holding
        // the single alternatives chip (the exclude chip was removed); it takes
        // the full bubble width, and layoutResultChips shrinks its label to fit.
        return (msg.role == "assistant" && msg.editData.isEmpty()
                && !resultChipList(msg).empty()) ? 32 : 0;
    }
    struct StageTicker : juce::Timer
    {
        EchoJayEditor& ed;
        explicit StageTicker(EchoJayEditor& e) : ed(e) {}
        void timerCallback() override;
    };
    StageTicker stageTicker_ { *this };
    
    // Custom viewport that forwards clicks to parent for wave card hit testing
    struct ChatViewport : public juce::Viewport
    {
        std::function<bool(const juce::MouseEvent&)> onClickCheck;
        std::function<void()> onScroll;
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (onClickCheck && onClickCheck(e))
                return; // consumed by wave card
            juce::Viewport::mouseDown(e);
        }
        // Called by JUCE when the visible area changes (i.e. when the user
        // scrolls). We use this to ask the parent editor to repaint the
        // chat region — without it, the editor's manually-painted avatars
        // tear at the viewport boundary because half the avatar lives in
        // the area JUCE marks dirty during scroll and half doesn't.
        void visibleAreaChanged(const juce::Rectangle<int>&) override
        {
            if (onScroll) onScroll();
        }
    };
    ChatViewport chatScroll;
    juce::Component chatContent;
    juce::TextEditor chatInput;
    juce::TextButton chatSendBtn { "Send" };
    // Phase R compose-time target: which rack the NEXT turns talk about.
    // An edit must know its rack at compose (the injected [CURRENT CHAIN]
    // defines the slot numbers the model writes), so the picker lives on
    // the input row, not on the apply step. Session state, never persisted.
    juce::TextButton chatTargetBtn { "This channel" };
    bool rehydratedFromStore_ = false;  // one-shot onLoaded re-hydrate
    // Chat composer container (single rounded box: text area on top, a
    // control row inside the bottom — target pill left, model LABEL +
    // Send right). chatBoxH() is the ONE height source; resized() computes
    // only the container origin/width per branch and layoutChatBox() is the
    // SOLE author of the internals on EVERY surface (no per-surface or
    // per-state variants — that is what fixed the sidebar empty-state
    // divergence); paint consumes chatBoxRect_ ONLY. The model text is a
    // Label, never a selector: routing is tier+turnType, not user choice.
    static constexpr int kChatBoxTextH = 56;
    static constexpr int kChatBoxRowH  = 30;
    int  chatBoxH() const { return kChatBoxTextH + kChatBoxRowH; }
    void layoutChatBox(juce::Rectangle<int> box);
    bool targetPillEligible() const;   // links connected or target set
    // ---- Phase C1: channel chats (active chat drives the target) ----
    juce::String activeChatLinkUid() const;      // "" = main chat
    juce::String effectiveChannelUid() const;    // active chat's, else pending
    juce::String findChannelChatId(const juce::String& linkUid) const; // "" = none
    bool linkUidLive(const juce::String& uid) const;
    juce::String channelDisplayLabel(const juce::String& uid) const;
    juce::String findOrCreateChannelChatId(const juce::String& linkUid,
                                           const juce::String& linkNameNow);
    juce::Rectangle<int> chatBoxRect_;
    bool lastPillEligible_ = false;    // timer change-detect -> resized()
    juce::String lastLockedUid_;       // } active-chat target: a live<->offline
    bool lastLockedLive_ = false;      // } flip changes pill TEXT WIDTH -> relayout
    juce::String lastLockedLabel_;     // } rename detection (label feeds pill width)
    // Derived cache, never state of record: the CHANNEL BANNER text
    // ("Working on Link: Vocals" + offline suffix). Resolved by
    // refreshChannelBannerCache() (timer change-detector + synchronous
    // activation points) so PAINT reads a string and never scans the
    // registry per frame. Empty = main chat = no banner (its height
    // collapses via channelBannerH()).
    juce::String chanBannerText_;
    bool chanBannerLive_ = false;
    void refreshChannelBannerCache();
    // Banner geometry: ONE height source; resized() authors and stores
    // the rect; paint consumes it and measures nothing (two-tH-sums
    // discipline, same as the ASK shelf and the composer).
    static constexpr int kChannelBannerH = 24;
    // Link tab: ALWAYS present (a main chat there reads "No channel
    // selected" and the dropdown is how you pick one). Everywhere else:
    // channel chats only (cached text non-empty).
    int channelBannerH() const
    {
        return (chanBannerText_.isNotEmpty() || currentTab == Tab::Link)
                 ? kChannelBannerH : 0;
    }
    juce::Rectangle<int> channelBannerRect_;
    void showChatTargetMenu();
    juce::TextButton chatTextSizeBtn { "Aa" };
    juce::Label      chatDisclaimerLabel;   // AI-mistakes footer pinned under the input
    juce::Label      chatModelLabel;        // model name indicator by the input (server-fed)
    juce::TextButton upgradeBtn { "Upgrade to Pro" };

    // ---- CHAIN tab (stage 2: multi-plugin rack) ------------------------------
    struct ChainPluginListModel : juce::ListBoxModel
    {
        juce::Array<juce::PluginDescription> items;
        std::function<void(int)> onRowSelected;
        std::function<void(int)> onRowDoubleClicked;
        int getNumRows() override { return items.size(); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool sel) override
        {
            if (sel) g.fillAll(juce::Colour(0xff2a4d7a));
            if (row >= items.size()) return;
            bool isAU = items[row].pluginFormatName == "AudioUnit";
            juce::Colour tagCol = isAU ? juce::Colour(0xff3a7a3a) : juce::Colour(0xff2a4d7a);
            juce::String tag    = isAU ? "AU" : "VST3";
            int tagW = 36;
            g.setColour(tagCol.withAlpha(0.7f));
            g.fillRoundedRectangle((float)(w - tagW - 4), (float)(h/2 - 8), (float)tagW, 16.0f, 3.0f);
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
            g.drawText(tag, w - tagW - 4, h/2 - 8, tagW, 16, juce::Justification::centred);
            g.setColour(sel ? juce::Colours::white : juce::Colour(0xffcccccc));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText(items[row].name, 4, 0, w - tagW - 12, h, juce::Justification::centredLeft);
        }
        void selectedRowsChanged(int r) override { if (onRowSelected) onRowSelected(r); }
        void listBoxItemDoubleClicked(int r, const juce::MouseEvent&) override
        { if (onRowDoubleClicked) onRowDoubleClicked(r); }
    };
    std::unique_ptr<ChainPluginListModel> chainListModel;
    juce::ListBox    chainPluginList;
    juce::TextEditor chainSearchBox;
    juce::TextButton chainScanBtn  { "Refresh" };

    // ---- Saved chains (Session B.1 / B.2) --------------------------------
    // Explicit saves only, never auto-save: a chain you did not choose to
    // save is not one you want back, and auto-saving would fill the
    // dashboard with rows nobody asked for.
    juce::TextButton chainSaveBtn   { "Save" };
    juce::TextButton chainSaveAsBtn { "Save As" };
    juce::TextButton chainOpenBtn   { "Open" };
    // Save with no chain loaded behaves as Save As (prompts for a name).
    void saveChainToApi(bool forceNew);
    // The request itself. id empty = create, id set = overwrite that chain.
    void sendChainSave(const juce::String& id, const juce::String& name);
    void showSavedChainsMenu();
    void openSavedChain(const juce::String& id, const juce::String& name);
    // Transient one-liner in the chain header ("Saved \"X\""). Past tense is
    // legitimate because saving IS something the user did, but it says only
    // that a chain was saved and never implies anything about the sound.
    // ---- Chain sidebar: AI | Chains ---------------------------------------
    // A MODE on the existing sidebar, not new chrome. The mode itself lives
    // on the processor (see chainSidebarChainsMode) so a Logic editor
    // recreate cannot flip it; the CONVERSATION survives because it always
    // lived on the processor too, and switching modes only toggles
    // visibility. Nothing about the thread is torn down or rebuilt.
    //
    // GEOMETRY: every rect below is authored in resized() and consumed by
    // paint() and mouseDown. paint() measures nothing.
    juce::Rectangle<int> chainModeAiRect_, chainModeChainsRect_;
    juce::Rectangle<int> chainListStatusRect_;

    struct ChainRow {
        juce::String id, name, updatedAt;
        int  slotCount = 0;
        bool hasState  = false;
        bool favourite = false;
        /** D3.2: "plugin" | "web" | "import". How the row came to exist,
            never rewritten after the insert. GET /api/v2/chains has always
            returned it; Session B simply had nothing to do with it, because
            nothing could produce an import until sharing existed. */
        juce::String source;
        bool isImport() const noexcept { return source == "import"; }
    };
    std::vector<ChainRow>              chainRows_;
    // Parallel to chainRows_ AFTER grouping; index i of these is the same
    // chain as displayRows_[i]. Authored by resized(), never by paint().
    std::vector<ChainRow>              chainDisplayRows_;
    std::vector<juce::Rectangle<int>>  chainRowRects_;
    std::vector<juce::Rectangle<int>>  chainRowStarRects_;
    // -1 = not a row (a group heading occupies the slot instead).
    std::vector<int>                   chainRowIsHeading_;
    std::vector<juce::String>          chainHeadingText_;

    juce::int64  chainListFetchedAtMs_ = 0;   // 0 = never fetched
    bool         chainListFromCache_   = false;
    bool         chainListLoading_     = false;
    juce::String chainListError_;

    void setChainSidebarMode(bool chainsMode);
    void refreshChainList();
    void applyChainRows(const juce::var& chains, juce::int64 fetchedAtMs, bool fromCache);
    void toggleChainFavourite(int displayIdx);
    // Right click / ctrl-click on a row. Delete is NOT here: the API has no
    // delete verb yet (see the note to the user), and a menu item that
    // cannot work is worse than one that is missing.
    void showChainRowMenu(int displayIdx);
    void renameChainRow(int displayIdx);
    void sendChainRename(const juce::String& id, const juce::String& name);
    void deleteChainRow(int displayIdx);
    void sendChainDelete(const juce::String& id, const juce::String& name);
    /** D3.2. Creates an UNLISTED LINK share and puts the URL on the clipboard.
        Never a public share: that needs a claimed handle the plugin cannot
        send anyone to claim. Idempotent server side, so a second press copies
        the same link and costs nothing against the monthly cap. */
    void shareChainRow(int displayIdx);
    void writeChainRowsToCache();
    void paintChainSidebar(juce::Graphics& g, int chatX, int chatW, int topH, int bottomY);
    /** The grouped list: a heading occupies a slot in all three vectors, with
        a placeholder row that is never drawn. Same shape Session B stored
        directly in the members; extracted so it can be proved. */
    struct ChainGrouping
    {
        std::vector<ChainRow>     rows;
        std::vector<int>          isHeading;      // 1 = this slot is a heading
        std::vector<juce::String> headingText;
    };

    /** THE grouping rule, as a pure function of the rows. STATIC and free of
        editor state on purpose, exactly like computeTabRects: it is what
        tools/dashboard_test calls, so the test exercises the code that ships
        rather than a copy of the rule. Every chain appears EXACTLY ONCE and
        the precedence is documented at the definition. */
    static ChainGrouping groupChainRows(const std::vector<ChainRow>& rows);

    /** Reaches groupChainRows without making ChainRow or the rule public. */
    friend struct EchoJayChainGroupTestAccess;

    // Group order is FAVOURITES, SAVED, IMPORTED. Session B built the grouping
    // as a list of (heading, rows) precisely so Imported could drop in as one
    // more entry once D3 could produce one, which is what happened. A group
    // with no members renders no heading, so IMPORTED is invisible on an
    // account that has never been sent a chain.
    void rebuildChainDisplayRows();
    bool chainSidebarInChainsMode() const;

    /** D3.2: the pending-imports dot on the CHAINS segment. AUTHORED BY THE
        SAME right-anchor block in resized() that places the segment itself,
        so it is not a second control anchored to the same edge. That pairing
        is what put `Aa` on top of the CHAINS switch, which was the fourth
        overlap bug in this file. Empty when the switch is not on screen. */
    juce::Rectangle<int> chainImportDotRect_;

    juce::String chainSaveStatus_;
    juce::uint32 chainSaveStatusAt_ = 0;
    bool         chainSaveInFlight_ = false;
    void setChainSaveStatus(const juce::String& s);
    // Left edge of the Save button, stored by resized() and CONSUMED by
    // paint(). Height reservation rule: resized() is the sole geometry
    // author, paint() measures nothing. 0 = the buttons are hidden, so the
    // name has the whole strip.
    int chainSaveBtnRight_ = 0;
    juce::Label      chainStatusLabel;
    juce::TextButton chainLoadBtn  { "Add to Chain" };
    juce::Label      chainRecommendLabel;  // "recommendable: N resolved (M enabled, K unmatched)"
    juce::TextEditor chainDebugJsonBox;    // shows raw chain JSON after each build (temporary debug)
    // Restricts the list to plugins loadable in this wrapper format.
    juce::String chainFormatFilter_;

    // Holder for the currently-selected slot's editor
    // Pop-out window for hosted plugin editors at native size
    struct ChainEditorWindow : juce::DocumentWindow
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor;
        std::function<void()> onCloseRequest;   // owner brings the editor back inline

        ChainEditorWindow(const juce::String& name, juce::AudioProcessorEditor* e)
            : juce::DocumentWindow(name, juce::Colour(0xff0A0C18),
                                   juce::DocumentWindow::closeButton),
              editor(e)
        {
            setUsingNativeTitleBar(true);
            setContentNonOwned(editor.get(), true);
            setResizable(false, false);
            centreWithSize(editor->getWidth(), editor->getHeight());
            // Float above the EchoJay window — Logic keeps plugin windows at
            // a raised level, so a normal-level window can be fully hidden
            // behind EchoJay no matter the stacking order at creation.
            setAlwaysOnTop(true);
            setVisible(true);
        }
        void closeButtonPressed() override
        {
            if (onCloseRequest) onCloseRequest();
            else setVisible(false);
        }
    };

    // Meaw:Chain-style rack — one large inline plugin view on top, horizontal
    // chain strip along the bottom. ONE hosted editor open at a time; selecting
    // another block closes the current editor first (sequential, never
    // simultaneous). The hosted NSView is clipped via NativeClip masksToBounds
    // and laid out from the ACTUAL native frame, not JUCE-reported sizes.
    struct ChainListPanel : juce::Component, juce::Timer
    {
        static constexpr int kMaxSlots    = 15;
        static constexpr int kCardHeaderH = 30;  // B/X + name + pop-out row above the plugin box
        static constexpr int kCardMargin  = 18;  // outer margin around the card boxes
        static constexpr int kSettingsH   = 72;  // SUGGESTED SETTINGS box height
        static constexpr int kCardGap     = 10;  // vertical gap between card boxes
        static constexpr int kStripH   = 76;   // bottom chain strip incl. scrollbar
        static constexpr int kBlockW   = 118;
        static constexpr int kBlockH   = 64;   // room for the wet/dry knob row
        static constexpr int kBlockGap = 26;   // connector-line gap between blocks
        static constexpr int kAddW     = 40;   // "+" block
        static constexpr int kMasterW  = 62;   // fixed master MIX knob area, right of strip

        // Compact rounded block in the bottom strip — name + B/X/</> controls.
        // Clicking anywhere else on the block selects it (shows editor above).
        struct Block : juce::Component
        {
            juce::String name;
            int  slotIdx  = 0;
            bool bypassed = false;
            bool selected = false;
            bool popoutOnly = false;   // editor opens in a floating window

            juce::TextButton bypassBtn { "B" };
            juce::TextButton removeBtn { "X" };
            juce::TextButton prevBtn   { "<" };
            juce::TextButton nextBtn   { ">" };
            ChainWetKnob     wetKnob;   // per-slot wet/dry (hidden while bypassed)

            std::function<void()>      onSelect;
            std::function<void()>      onBypass;
            std::function<void()>      onRemove;
            std::function<void(int)>   onMove;
            std::function<void(float)> onWet;

            Block()
            {
                auto style = [](juce::TextButton& b, juce::Colour fg) {
                    b.setColour(juce::TextButton::buttonColourId, juce::Colour(0xcc0E1020));
                    b.setColour(juce::TextButton::textColourOffId, fg);
                };
                style(bypassBtn, juce::Colour(0xffa0a0b8));
                style(removeBtn, juce::Colour(0xffef4444));
                style(prevBtn,   juce::Colour(0xffa0a0b8));
                style(nextBtn,   juce::Colour(0xffa0a0b8));
                bypassBtn.setTooltip("Bypass this plugin");
                removeBtn.setTooltip("Remove from chain");
                prevBtn.setTooltip("Move earlier in the chain");
                nextBtn.setTooltip("Move later in the chain");
                for (auto* b : { &bypassBtn, &removeBtn, &prevBtn, &nextBtn })
                    addAndMakeVisible(*b);
                bypassBtn.onClick = [this] { if (onBypass) onBypass(); };
                removeBtn.onClick = [this] { if (onRemove) onRemove(); };
                prevBtn.onClick   = [this] { if (onMove)   onMove(-1); };
                nextBtn.onClick   = [this] { if (onMove)   onMove(+1); };
                addAndMakeVisible(wetKnob);
                wetKnob.onChange = [this](float v) { if (onWet) onWet(v); };
            }

            void mouseDown(const juce::MouseEvent&) override
            { if (onSelect) onSelect(); }

            void paint(juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat().reduced(0.5f);
                g.setColour(selected ? juce::Colour(0xff11293a) : juce::Colour(0xff0E1020));
                g.fillRoundedRectangle(r, 8.0f);
                g.setColour(selected ? juce::Colour(0xff22d3ee)
                                     : juce::Colour::fromFloatRGBA(1, 1, 1, 0.08f));
                g.drawRoundedRectangle(r, 8.0f, selected ? 1.5f : 1.0f);

                g.setColour(bypassed ? juce::Colour(0xff606078) : juce::Colour(0xfff0f0f5));
                g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                g.drawText(name, 6, 3, getWidth() - 12, 18,
                           juce::Justification::centred, true);
                if (bypassed)
                {
                    g.setColour(juce::Colour(0xfff59e0b));
                    g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
                    g.drawText("BYPASSED", 6, 19, getWidth() - 12, 9,
                               juce::Justification::centred);
                }
                if (popoutOnly)
                {
                    // Pop-out glyph — this plugin's editor opens in a
                    // floating window (out-of-process view, can't inline)
                    g.setColour(juce::Colour(0xff22d3ee).withAlpha(0.85f));
                    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
                    g.drawText(juce::String::fromUTF8("\xe2\x86\x97"),
                               getWidth() - 15, 2, 12, 11, juce::Justification::centred);
                }
            }

            void resized() override
            {
                int bw = 18, bh = 15, m = 4;
                int by = getHeight() - bh - m;
                bypassBtn.setBounds(m, by, bw, bh);
                removeBtn.setBounds(m + bw + 2, by, bw, bh);
                nextBtn.setBounds(getWidth() - m - bw, by, bw, bh);
                prevBtn.setBounds(getWidth() - m - bw * 2 - 2, by, bw, bh);
                // Wet/dry knob — centred between name row and button row
                wetKnob.setBounds((getWidth() - 22) / 2, 20, 22, 22);
            }
        };

        // Strip content — paints the left-to-right connector line behind blocks
        struct StripContent : juce::Component
        {
            int lineY = 0, lineEndX = 0;
            void paint(juce::Graphics& g) override
            {
                if (lineEndX > 12)
                {
                    g.setColour(juce::Colour(0xff22d3ee).withAlpha(0.35f));
                    g.drawLine(12.0f, (float)lineY, (float)lineEndX, (float)lineY, 1.5f);
                }
            }
        };

        juce::Viewport   stripView;
        StripContent     stripContent;
        juce::TextButton addBlock { "+" };
        ChainWetKnob     masterKnob;   // whole-chain wet/dry, fixed right of strip
        std::vector<std::unique_ptr<Block>> blocks;
        std::vector<ChainHost::SlotInfo>    slotInfos;
        int selectedIdx = -1;

        // JUCE-side clip: the inline editor lives INSIDE this holder, which is
        // locked to the display area. Descendant painting (e.g. an AU editor
        // component's white background) cannot leak outside it — that leak was
        // the white bands around plugins in 2.9.20.
        struct InlineHolder : juce::Component
        {
            std::function<void()> onChildBounds;
            void childBoundsChanged(juce::Component*) override
            { if (onChildBounds) onChildBounds(); }
        };
        InlineHolder inlineHolder;

        // Inline hosted editor — at most ONE alive at any moment
        std::unique_ptr<juce::AudioProcessorEditor> inlineEditor;
        int  inlineSlot  = -1;
        int  realW = 0, realH = 0;  // actual native NSView size (JUCE sizes lie)
        int  framePolls  = 0;
        bool settled     = false;   // real native frame captured; timer in maintenance mode
        bool layoutGuard = false;

        std::unique_ptr<ChainEditorWindow> popout;
        int popoutSlot = -1;

        // Card header row controls — act on the SELECTED slot
        juce::TextButton cardBypassBtn { "B" };
        juce::TextButton cardRemoveBtn { "X" };
        juce::TextButton popBtn { juce::String::fromUTF8("\xe2\x86\x97") }; // expand to floating window

        // SUGGESTED SETTINGS box content — wraps, scrolls when it overflows
        juce::TextEditor settingsBox;
        juce::TooltipWindow tooltipWindow { this, 600 };

        juce::String statusText;

        std::function<void(int)>        onSelectSlot;
        std::function<void(int)>        onRemoveSlot;
        std::function<void(int)>        onBypassSlot;
        std::function<void(int, int)>   onMoveSlot;
        std::function<void()>           onAddClick;
        std::function<void(int, float)> onSlotWet;    // slot idx, wet 0..1
        std::function<void(float)>      onMasterWet;  // wet 0..1
        std::function<juce::AudioProcessorEditor*(int)> onCreateEditor;

        ChainListPanel()
        {
            // Added first: stays at the back of the z-order so the strip,
            // header row and pop-out button remain clickable above it
            addChildComponent(inlineHolder);
            inlineHolder.setInterceptsMouseClicks(false, true);
            inlineHolder.onChildBounds = [this]
            {
                // Hosted editor resized itself — re-centre within the FIXED
                // container (never resize the container)
                if (layoutGuard || inlineEditor == nullptr) return;
                int w = 0, h = 0;
                if (NativeClip::getPluginViewSize(this, w, h) && w > 100 && h > 60)
                { realW = w; realH = h; }
                layoutInline();
                attachNative(false);
            };

            addAndMakeVisible(stripView);
            stripView.setViewedComponent(&stripContent, false);
            stripView.setScrollBarsShown(false, true, false, true);
            stripView.setScrollBarThickness(8);

            addBlock.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff141626));
            addBlock.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
            addBlock.onClick = [this] { if (onAddClick) onAddClick(); };
            stripContent.addAndMakeVisible(addBlock);

            // Master chain wet/dry — fixed at the right edge of the strip
            // (outside the scrolling viewport, always visible)
            masterKnob.caption = "MIX";
            masterKnob.onChange = [this](float v) { if (onMasterWet) onMasterWet(v); };
            addAndMakeVisible(masterKnob);

            popBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xcc0E1020));
            popBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
            popBtn.setTooltip("Open in floating window at native size");
            popBtn.onClick = [this] { openPopoutForSelected(); };
            addChildComponent(popBtn);

            // Card header B / X — same actions as the strip blocks
            auto cardStyle = [](juce::TextButton& b, juce::Colour fg) {
                b.setColour(juce::TextButton::buttonColourId, juce::Colour(0xcc0E1020));
                b.setColour(juce::TextButton::textColourOffId, fg);
            };
            cardStyle(cardBypassBtn, juce::Colour(0xffa0a0b8));
            cardStyle(cardRemoveBtn, juce::Colour(0xffef4444));
            cardBypassBtn.setTooltip("Bypass this plugin");
            cardRemoveBtn.setTooltip("Remove from chain");
            cardBypassBtn.onClick = [this] {
                if (selectedIdx >= 0 && onBypassSlot) onBypassSlot(selectedIdx);
            };
            cardRemoveBtn.onClick = [this] {
                if (selectedIdx >= 0 && onRemoveSlot) onRemoveSlot(selectedIdx);
            };
            addChildComponent(cardBypassBtn);
            addChildComponent(cardRemoveBtn);

            settingsBox.setMultiLine(true, true);
            settingsBox.setReadOnly(true);
            settingsBox.setScrollbarsShown(true);
            settingsBox.setCaretVisible(false);
            settingsBox.setColour(juce::TextEditor::backgroundColourId,
                                  juce::Colours::transparentBlack);
            settingsBox.setColour(juce::TextEditor::outlineColourId,
                                  juce::Colours::transparentBlack);
            settingsBox.setColour(juce::TextEditor::focusedOutlineColourId,
                                  juce::Colours::transparentBlack);
            settingsBox.setColour(juce::TextEditor::textColourId, juce::Colour(0xff22d3ee));
            settingsBox.setFont(juce::Font(juce::FontOptions(11.5f)));
            addChildComponent(settingsBox);
        }

        ~ChainListPanel() override { closeAllEditors(); }

        // ---- Editor lifecycle (ONE at a time, always close-before-open) ----

        void closeInline()
        {
            stopTimer();
            if (inlineEditor)
            {
                inlineHolder.removeChildComponent(inlineEditor.get());
                inlineEditor.reset();
                NativeClip::detach(this);   // remove the clip container too
            }
            inlineHolder.setVisible(false);
            inlineSlot = -1;
            realW = realH = 0;
            settled = false;
        }

        void closePopout()
        {
            if (popout) { popout->setVisible(false); popout.reset(); }
            popoutSlot = -1;
        }

        void closeAllEditors() { closeInline(); closePopout(); }

        // Open slot i's editor inline in the display area. The current editor
        // (inline or floating) is fully destroyed FIRST — never two at once.
        void showInline(int i)
        {
            closeAllEditors();
            if (!onCreateEditor || i < 0 || i >= (int)slotInfos.size()) return;

            // Known popout-only plugin (out-of-process editor): go straight
            // to the floating window — no failed inline attempt, no timeout.
            // Format-qualified: a VST3 build of the same plugin may inline fine.
            if (ChainHost::isPopoutOnly(slotInfos[(size_t)i].name,
                                        slotInfos[(size_t)i].format))
            {
                statusText = "Opens in a floating window (plugin limitation)";
                openPopoutForSelected();
                repaint();
                return;
            }

           #if ! JUCE_MAC
            // Off macOS there is no NativeClip container, so the hosted view can
            // be neither measured nor clipped and inline hosting cannot be made
            // safe. Take the same floating-window path, but decide it UP FRONT.
            //
            // Deciding here rather than letting the poll below time out matters
            // twice over: the timeout costs ~5s of dead UI per selection, and it
            // would then persist a popout_only mark asserting "THIS PLUGIN's
            // editor cannot inline" — a platform fact recorded as a plugin fact.
            // That file is per-platform (%APPDATA% here, ~/Library on the Mac),
            // so it cannot reach the Mac, but it would still wrongly condemn the
            // plugin on any later Windows build that CAN inline. No status suffix
            // for the same reason: nothing here is a limitation of the plugin.
            statusText = "Opens in a floating window";
            openPopoutForSelected();
            repaint();
            return;
           #endif

            juce::AudioProcessorEditor* ed = nullptr;
            try { ed = onCreateEditor(i); } catch (...) {}
            if (!ed) { statusText = "Failed: could not open editor"; repaint(); return; }

            statusText.clear();
            inlineEditor.reset(ed);
            inlineSlot = i;
            inlineHolder.setVisible(true);
            inlineHolder.addAndMakeVisible(*inlineEditor);
            layoutInline();
            attachNative(true);   // create container, reparent, clip, centre + log

            // JUCE-reported editor sizes are unreliable — poll the native
            // NSView frame while it's a degenerate placeholder (1x1, 40x30 …)
            framePolls = 0;
            settled = false;
            startTimer(100);
            repaint();
        }

        // Reassert the fixed clipping container over the display area and
        // re-centre the plugin view inside it. The container's frame comes
        // from OUR layout only — this never resizes it to the plugin.
        // The editor's reported size rides along so degenerate out-of-process
        // proxies can have it imposed (hypothesis A).
        void attachNative(bool log)
        {
            if (inlineEditor)
                NativeClip::attach(this, displayArea(), log,
                                   inlineEditor->getWidth(), inlineEditor->getHeight());
        }

        void timerCallback() override
        {
            if (!inlineEditor) { stopTimer(); return; }
            if (!settled)
            {
                // Sizes up to 100x60 are treated as placeholders — Waves-style
                // out-of-process views report 40x30 until the remote UI fills
                // in. Wait up to ~5s before falling back to a hint.
                ++framePolls;
                int w = 0, h = 0;
                bool got = NativeClip::getPluginViewSize(this, w, h)
                        && w > 100 && h > 60;
                if (got) { realW = w; realH = h; }
                if (got || framePolls >= 50)
                {
                    settled = true;
                    if (!got)
                    {
                        // Never grew past a placeholder: out-of-process
                        // editors (WaveShell etc.) render in a view service
                        // and won't size inline, but work in their own
                        // window. Remember the plugin and open the pop-out
                        // automatically — first-class, not an error.
                        if (inlineSlot >= 0 && inlineSlot < (int)slotInfos.size())
                        {
                            ChainHost::markPopoutOnly(slotInfos[(size_t)inlineSlot].name,
                                                      slotInfos[(size_t)inlineSlot].format);
                            for (auto& bl : blocks)
                                if (bl->slotIdx == inlineSlot)
                                { bl->popoutOnly = true; bl->repaint(); }
                        }
                        statusText = "Opens in a floating window (plugin limitation)";
                        openPopoutForSelected();   // destroys the inline editor first
                        repaint();
                        startTimer(400);
                        return;
                    }
                    layoutInline();
                    attachNative(true);   // log the settled geometry
                    repaint();
                    startTimer(400);      // switch to maintenance cadence
                }
                return;
            }
            // Maintenance: keep the plugin view parented in the fixed
            // container and aligned, and pick up LATE size changes —
            // out-of-process views can fill in many seconds after load.
            int w = 0, h = 0;
            if (NativeClip::getPluginViewSize(this, w, h) && w > 100 && h > 60
                && (w != realW || h != realH))
            {
                realW = w;
                realH = h;
                if (statusText.startsWith("Editor didn't")) statusText.clear();
                layoutInline();
                repaint();
            }
            attachNative(false);
        }

        bool hasSelection() const
        {
            return selectedIdx >= 0 && selectedIdx < (int)slotInfos.size();
        }

        // True while any editor is up (inline or floating) — the auto-open
        // pass after an AI build stops at the first slot that yields one.
        bool anyEditorOpen() const
        {
            return inlineEditor != nullptr || popout != nullptr;
        }

        // Framed card layout, top to bottom: card header row (B/X + name +
        // pop-out), plugin box (the clip container, inset on all sides),
        // SUGGESTED SETTINGS box, chain strip. The plugin box IS the
        // container clip frame — containment policy is unchanged.
        // ---- Settings-restore notes -------------------------------------
        // Plain sentences naming any slot whose hosted settings did not save
        // or did not restore. A slot sitting at its defaults must never look
        // restored, so this band is persistent (not a toast) and stays until
        // the user clicks it away.
        //
        // ONE height source, as with every other reserved band here:
        // noteBandH() authors it, displayArea() and paint() consume it, and
        // nothing measures the text a second time.
        static constexpr int kNoteLineH  = 17;
        static constexpr int kNoteMaxRow = 3;
        juce::StringArray stateNotes;
        std::function<void()> onDismissNotes;

        int noteBandH() const
        {
            if (stateNotes.isEmpty()) return 0;
            return juce::jmin(kNoteMaxRow, stateNotes.size()) * kNoteLineH + 14 + kCardGap;
        }

        juce::Rectangle<int> noteBandRect() const
        {
            if (stateNotes.isEmpty()) return {};
            return { kCardMargin, 4 + kCardHeaderH + kCardGap,
                     juce::jmax(50, getWidth() - kCardMargin * 2),
                     noteBandH() - kCardGap };
        }

        void setStateNotes(const juce::StringArray& notes)
        {
            if (notes == stateNotes) return;
            const bool bandChanged = notes.isEmpty() != stateNotes.isEmpty()
                                  || juce::jmin(kNoteMaxRow, notes.size())
                                     != juce::jmin(kNoteMaxRow, stateNotes.size());
            stateNotes = notes;
            if (bandChanged) resized();   // the plugin view sits below the band
            repaint();
        }

        juce::Rectangle<int> displayArea() const
        {
            int top    = 4 + kCardHeaderH + kCardGap + noteBandH();
            int bottom = getHeight() - kStripH - 8 - kSettingsH - kCardGap;
            return { kCardMargin, top,
                     juce::jmax(50, getWidth() - kCardMargin * 2),
                     juce::jmax(50, bottom - top) };
        }

        juce::Rectangle<int> settingsBoxRect() const
        {
            auto area = displayArea();
            return { area.getX(), area.getBottom() + kCardGap,
                     area.getWidth(), kSettingsH };
        }

        // Sync card controls + settings text with the current selection
        void updateCard()
        {
            bool sel = hasSelection();
            cardBypassBtn.setVisible(sel);
            cardRemoveBtn.setVisible(sel);
            popBtn.setVisible(sel);
            settingsBox.setVisible(sel);
            if (sel)
            {
                const auto& s = slotInfos[(size_t)selectedIdx];
                bool hasGuidance = s.settings.isNotEmpty();
                settingsBox.setColour(juce::TextEditor::textColourId,
                                      hasGuidance ? juce::Colour(0xff22d3ee)
                                                  : juce::Colour(0xff606078));
                juce::String txt = hasGuidance
                    ? s.settings
                    : juce::String("No suggested settings for this plugin");
                if (settingsBox.getText() != txt)
                    settingsBox.setText(txt, false);
            }
        }

        // Size the JUCE editor component to the plugin's NATIVE size — never
        // clamp it: JUCE mirrors the component size onto the native view, so
        // clamping would shrink the plugin view instead of trimming it. The
        // holder clips JUCE-side painting to the display area; the NSView
        // container clips the native side. Top-aligned, centred-x — matching
        // the native alignment policy exactly.
        void layoutInline()
        {
            if (!inlineEditor) return;
            auto area = displayArea();
            inlineHolder.setBounds(area);
            int pw = realW > 8 ? realW : inlineEditor->getWidth();
            int ph = realH > 8 ? realH : inlineEditor->getHeight();
            pw = juce::jmax(pw, 40);
            ph = juce::jmax(ph, 30);
            layoutGuard = true;
            inlineEditor->setBounds((area.getWidth() - pw) / 2, 0, pw, ph);
            layoutGuard = false;
        }

        void selectSlot(int i)
        {
            selectedIdx = i;
            if (onSelectSlot) onSelectSlot(i);
            for (auto& bl : blocks)
            { bl->selected = (bl->slotIdx == i); bl->repaint(); }
            if (inlineSlot != i || inlineEditor == nullptr)
                showInline(i);
            popBtn.setVisible(selectedIdx >= 0);
            // Header may have appeared/disappeared with the new selection —
            // re-run layout so the clip container is re-asserted at the new
            // display-area rect.
            resized();
            repaint();
        }

        // Fallback: open the selected slot's editor in a floating window at
        // native size (closes the inline editor first — one at a time).
        void openPopoutForSelected()
        {
            if (selectedIdx < 0 || selectedIdx >= (int)slotInfos.size() || !onCreateEditor)
                return;
            int i = selectedIdx;
            closeAllEditors();
            juce::AudioProcessorEditor* ed = nullptr;
            try { ed = onCreateEditor(i); } catch (...) {}
            if (!ed) return;
            popout = std::make_unique<ChainEditorWindow>(slotInfos[(size_t)i].name, ed);
            popoutSlot = i;
            // Closing the floating window returns the editor to the plugin
            // box. Deferred: the window can't be destroyed from inside its
            // own close callback.
            popout->onCloseRequest = [safe = juce::Component::SafePointer<ChainListPanel>(this)]
            {
                juce::MessageManager::callAsync([safe]
                {
                    if (safe == nullptr) return;
                    int s = safe->popoutSlot;
                    safe->closePopout();
                    // Popout-only plugins must NOT bounce back inline (that
                    // would immediately reopen the window they just closed);
                    // clicking the block reopens it on demand.
                    if (s >= 0 && s == safe->selectedIdx
                        && s < (int)safe->slotInfos.size()
                        && !ChainHost::isPopoutOnly(safe->slotInfos[(size_t)s].name,
                                                    safe->slotInfos[(size_t)s].format))
                        safe->showInline(s);   // sequential: popout destroyed first
                    safe->repaint();
                });
            };
            if (auto* topComp = getTopLevelComponent())
            {
                auto mb = topComp->getScreenBounds();
                popout->setTopLeftPosition(mb.getX() + 60, mb.getY() + 60);
            }
            popout->toFront(true);
            repaint();
        }

        // Keep editor-slot bookkeeping in sync when the chain mutates.
        // Call BEFORE ChainHost destroys/reorders the underlying processors.
        void noteSlotRemoved(int i)
        {
            if (inlineSlot == i || popoutSlot == i) closeAllEditors();
            if (inlineSlot > i) --inlineSlot;
            if (popoutSlot > i) --popoutSlot;
        }

        void noteSlotMoved(int i, int j)
        {
            auto remap = [i, j](int s) { return s == i ? j : (s == j ? i : s); };
            inlineSlot = remap(inlineSlot);
            popoutSlot = remap(popoutSlot);
        }

        void rebuild(const std::vector<ChainHost::SlotInfo>& slots, int selIdx)
        {
            slotInfos = slots;
            if ((int)slotInfos.size() > kMaxSlots)
                slotInfos.resize(kMaxSlots);
            selectedIdx = juce::jlimit(-1, (int)slotInfos.size() - 1, selIdx);

            for (auto& bl : blocks) stripContent.removeChildComponent(bl.get());
            blocks.clear();
            for (int i = 0; i < (int)slotInfos.size(); ++i)
            {
                auto bl = std::make_unique<Block>();
                bl->name     = slotInfos[(size_t)i].name;
                bl->slotIdx  = i;
                bl->bypassed = slotInfos[(size_t)i].bypassed;
                bl->selected = (i == selectedIdx);
                bl->popoutOnly = ChainHost::isPopoutOnly(bl->name, slotInfos[(size_t)i].format);
                int ci = i;
                bl->onSelect = [this, ci] { selectSlot(ci); };
                bl->onBypass = [this, ci] { if (onBypassSlot) onBypassSlot(ci); };
                bl->onRemove = [this, ci] { if (onRemoveSlot) onRemoveSlot(ci); };
                bl->onMove   = [this, ci](int dir) { if (onMoveSlot) onMoveSlot(ci, dir); };
                bl->wetKnob.setValue(slotInfos[(size_t)i].wet);
                bl->wetKnob.setVisible(!slotInfos[(size_t)i].bypassed);
                bl->onWet    = [this, ci](float v) { if (onSlotWet) onSlotWet(ci, v); };
                bl->prevBtn.setEnabled(i > 0);
                bl->nextBtn.setEnabled(i < (int)slotInfos.size() - 1);
                stripContent.addAndMakeVisible(*bl);
                blocks.push_back(std::move(bl));
            }
            layoutStrip();
            popBtn.setVisible(selectedIdx >= 0);

            // Bring the inline editor in line with the selection
            if (selectedIdx < 0)
                closeAllEditors();
            else if ((inlineSlot != selectedIdx || inlineEditor == nullptr)
                     && !(popout != nullptr && popoutSlot == selectedIdx))
                showInline(selectedIdx);
            resized();   // header visibility may have changed — re-assert clip rect
            repaint();
        }

        void layoutStrip()
        {
            const int contentH = kStripH - 10;  // leave room for the h-scrollbar
            int x = 12;
            int y = (contentH - kBlockH) / 2;
            for (auto& bl : blocks)
            {
                bl->setBounds(x, y, kBlockW, kBlockH);
                x += kBlockW + kBlockGap;
            }
            bool canAdd = (int)blocks.size() < kMaxSlots;
            addBlock.setVisible(canAdd);
            if (canAdd)
            {
                addBlock.setBounds(x, y + (kBlockH - kAddW) / 2, kAddW, kAddW);
                x += kAddW + 12;
            }
            stripContent.lineY    = y + kBlockH / 2;
            stripContent.lineEndX = blocks.empty() ? 0 : x - 12;
            stripContent.setSize(juce::jmax(x, stripView.getWidth()), contentH);
            stripContent.repaint();
        }

        void paint(juce::Graphics& g) override
        {
            gEjZdbgPaint("ChainListPanel", *this); // TEMP DEBUG [zdbg]
            g.fillAll(juce::Colour(0xff0A0C18));

            // Settings-restore notes. Geometry comes from noteBandRect();
            // this measures nothing.
            if (auto band = noteBandRect(); !band.isEmpty())
            {
                g.setColour(juce::Colour(0xff2A1F08));
                g.fillRoundedRectangle(band.toFloat(), 8.0f);
                g.setColour(juce::Colour(0xffE0A83A).withAlpha(0.55f));
                g.drawRoundedRectangle(band.toFloat().reduced(0.5f), 8.0f, 1.0f);
                g.setColour(juce::Colour(0xffE8C070));
                g.setFont(juce::Font(juce::FontOptions(11.5f)));
                int shown = juce::jmin(kNoteMaxRow, stateNotes.size());
                for (int i = 0; i < shown; ++i)
                {
                    juce::String line = stateNotes[i];
                    if (i == kNoteMaxRow - 1 && stateNotes.size() > kNoteMaxRow)
                        line = line + "   (+" + juce::String(stateNotes.size() - kNoteMaxRow)
                             + " more)";
                    g.drawText(line, band.getX() + 12, band.getY() + 7 + i * kNoteLineH,
                               band.getWidth() - 24, kNoteLineH,
                               juce::Justification::centredLeft, true);
                }
                g.setColour(juce::Colour(0xff8A7040));
                g.setFont(juce::Font(juce::FontOptions(9.5f)));
                g.drawText("click to dismiss", band.getX() + 12,
                           band.getBottom() - 12, band.getWidth() - 24, 10,
                           juce::Justification::centredRight);
            }

            auto area = displayArea();

            if (hasSelection())
            {
                const auto& s = slotInfos[(size_t)selectedIdx];

                // Plugin box card — the native container paints its own bg +
                // border on top of this; drawing it here too keeps the frame
                // visible during load and while popped out
                auto cardBorder = juce::Colour(0xff22d3ee).withAlpha(0.3f);
                g.setColour(juce::Colour(0xff080A12));
                g.fillRoundedRectangle(area.toFloat(), 8.0f);
                g.setColour(cardBorder);
                g.drawRoundedRectangle(area.toFloat().reduced(0.5f), 8.0f, 1.0f);

                // Card header row — name centred between B/X (left) and ↗ (right)
                g.setColour(s.bypassed ? juce::Colour(0xff606078) : juce::Colour(0xfff0f0f5));
                g.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
                int nameInset = kCardMargin + 56;
                g.drawText(s.name + (s.bypassed ? "  (bypassed)" : ""),
                           nameInset, 4, getWidth() - nameInset * 2, kCardHeaderH - 4,
                           juce::Justification::centred, true);

                // SUGGESTED SETTINGS box card
                auto sb = settingsBoxRect().toFloat();
                g.setColour(juce::Colour(0xff080A12));
                g.fillRoundedRectangle(sb, 8.0f);
                g.setColour(cardBorder);
                g.drawRoundedRectangle(sb.reduced(0.5f), 8.0f, 1.0f);
                g.setColour(juce::Colour(0xffa0a0b8));
                g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
                g.drawText("SUGGESTED SETTINGS", (int)sb.getX() + 10, (int)sb.getY() + 5,
                           200, 12, juce::Justification::centredLeft);
            }

            // Display-area messages
            if (statusText.isNotEmpty())
            {
                bool isErr = statusText.startsWith("Failed") || statusText.startsWith("Error");
                bool isLd  = statusText.startsWith("Loading");
                g.setColour(isErr ? C::red : isLd ? C::blue2 : C::text2);
                g.setFont(juce::Font(juce::FontOptions(13.0f)));
                g.drawText(statusText, area.reduced(16), juce::Justification::centred, true);
            }
            else if (slotInfos.empty())
            {
                int cy = area.getCentreY() - 34;
                g.setColour(juce::Colour(0xffa0a0b8));
                g.setFont(juce::Font(juce::FontOptions(13.0f)));
                g.drawText("Press + to add a plugin, or ask the AI assistant",
                           0, cy, getWidth(), 20, juce::Justification::centred);
                g.drawText("to build a chain for you.",
                           0, cy + 20, getWidth(), 20, juce::Justification::centred);
            }
            else if (popout != nullptr && popoutSlot == selectedIdx && inlineEditor == nullptr)
            {
                bool po = selectedIdx < (int)slotInfos.size()
                       && ChainHost::isPopoutOnly(slotInfos[(size_t)selectedIdx].name,
                                                  slotInfos[(size_t)selectedIdx].format);
                g.setColour(juce::Colour(0xffa0a0b8));
                g.setFont(juce::Font(juce::FontOptions(12.0f)));
                g.drawText(po ? "This plugin opens in a floating window (plugin limitation)."
                              : "Editor is open in a floating window - click the plugin block to bring it back.",
                           area.reduced(16), juce::Justification::centred, true);
            }
        }

        void resized() override
        {
            // Card header row: B / X left, pop-out right (name painted centred)
            cardBypassBtn.setBounds(kCardMargin, 6, 24, 22);
            cardRemoveBtn.setBounds(kCardMargin + 26, 6, 24, 22);
            popBtn.setBounds(getWidth() - kCardMargin - 26, 6, 26, 22);

            // Settings text sits inside its card, below the tiny caps label
            auto sb = settingsBoxRect();
            settingsBox.setBounds(sb.getX() + 8, sb.getY() + 18,
                                  sb.getWidth() - 16, sb.getHeight() - 24);

            stripView.setBounds(0, getHeight() - kStripH,
                                juce::jmax(50, getWidth() - kMasterW), kStripH);
            masterKnob.setBounds(getWidth() - kMasterW + 9,
                                 getHeight() - kStripH + 6, 44, 54);
            updateCard();
            layoutStrip();
            layoutInline();
            attachNative(false);   // re-assert the clip frame at the new rect
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            if (!stateNotes.isEmpty() && noteBandRect().contains(e.getPosition()))
                if (onDismissNotes) onDismissNotes();
        }

        // The container frame is peer-relative — track pure moves too
        void moved() override { attachNative(false); }
    };
    ChainListPanel chainListPanel;
    int chainSelectedSlot_ = -1;
    bool chainRemovePending_ = false;  // one deferred slot-removal at a time

    // CHAIN tab AI assistant sidebar collapse — when collapsed the plugin
    // display area takes the full tab width.
    bool chainChatCollapsed_ = false;
    // Width of the sidebar collapse chevron. ONE constant, so the rack
    // strip's Save / Save As / Open cursor and the button itself cannot
    // disagree about how much space it takes.
    static constexpr int kChainToggleW = 28;
    juce::TextButton chainChatToggleBtn { ">" };
    // "n/15" slots-used counter — sits left of the Aa button in the AI
    // ASSISTANT header (replaces the usage counter on this tab).
    juce::Label chainSlotCountLabel;

    // Warning overlay (shown once when CHAIN tab first opened)
    juce::Component  chainWarnOverlay;
    juce::Label      chainWarnLabel;
    juce::TextButton chainWarnOkBtn { "I understand, continue" };

    // Chat text scaling (user-adjustable via Aa button in chat header).
    // Stored as a multiplier applied to the base 12pt message font. Cycles
    // through preset steps on each click and persists across sessions.
    // Multiplier on the 12pt base font. Default 1.25 = 15px message text
    // (Claude/ChatGPT territory); loadChatTextScale() overrides it only when
    // the user has an explicit persisted Aa choice.
    float chatTextScale = 1.25f;
    void cycleChatTextScale();
    void loadChatTextScale();
    void saveChatTextScale();
    
    bool pluginsSent = false;
    int scannedPluginCount = 0;
    bool wasScanning = false;
    int refreshCounter = 0;
    bool pendingAutoFeedback = false; // waiting for WAV save before triggering AI feedback
    bool settingsFetched = false; // true once we've loaded settings from server at least once
    
    // Waveform play buttons in chat — actual button components overlaid on waveform cards
    static constexpr int kMaxWavePlayBtns = 8;
    std::array<juce::TextButton, kMaxWavePlayBtns> wavePlayOverlays;
    std::array<juce::String, kMaxWavePlayBtns> wavePlayPaths;
    std::array<float, kMaxWavePlayBtns> wavePlayDurations {};
    int activeWavePlayBtns = 0;

    // "Build this chain" buttons — one per assistant reply that contains a chain block
    static constexpr int kMaxChainBuildBtns = 8;
    std::array<juce::TextButton, kMaxChainBuildBtns> chainBuildBtns;
    std::array<juce::String, kMaxChainBuildBtns> chainBuildJsons;
    int activeChainBuildBtns = 0;

    // ---- Chain-edit preview cards (Phase 1c) --------------------------------
    // One "Apply changes" button per assistant reply carrying a CHAIN_EDIT
    // block; the op list is painted above it in plain language. NEVER
    // mutates silently: ops run only on Apply, through
    // ChainHost::applyChainEdits (staleness-guarded, stop-at-failure).
    std::array<juce::TextButton, kMaxChainBuildBtns> editApplyBtns;
    std::array<int, kMaxChainBuildBtns> editApplyMsgIdx { };
    int activeEditApplyBtns = 0;
    // "Suggest an alternative" pills on retired cards whose failure was a
    // plugin LOAD failure (never staleness/invalid aborts — re-asking is
    // right there). Tap auto-sends the stored follow-up through the normal
    // send path (1b answer-tap machinery); NEVER auto-substitutes.
    std::array<juce::TextButton, kMaxChainBuildBtns> editAltBtns;
    std::array<int, kMaxChainBuildBtns> editAltMsgIdx { };
    int activeEditAltBtns = 0;
    // ---- Result-bubble chip LIST (build failures) ----
    // A build-result bubble carries a LIST of chips (Suggest alternative(s),
    // Stop suggesting X). ONE layout pass (layoutResultChips) produces the
    // rects; altPillH is the single height source both measure and paint
    // consume; paint measures nothing. resultChipRow lists (label,kind) so
    // the two passes cannot disagree on WHICH chips exist. kind: 0 alt,
    // 1 exclude.
    static constexpr int kMaxResultChips = 4;
    std::array<juce::TextButton, kMaxResultChips> resultChipBtns;
    std::array<int, kMaxResultChips> resultChipMsgIdx { };
    std::array<int, kMaxResultChips> resultChipKind { };
    int activeResultChips = 0;
    struct ResultChip { juce::String label; int kind; };
    std::vector<ResultChip> resultChipList(const ChatMsg& m) const;
    void layoutResultChips(const ChatMsg& m, juce::Rectangle<int> area,
                           std::vector<juce::Rectangle<int>>& rectsOut) const;
    void onResultChipTapped(int msgIdx, int kind);
    // Shared height helpers for the measure + paint passes (must agree)
    // THE single source of the chat scroll extent: summed message heights,
    // consumed by BOTH the timer and resized() so the scroll range is never a
    // second guess (fixes the sidebar "can't scroll up to a long reply" bug).
    int  measureChatContentHeight();
    int  editCardHeight(const ChatMsg& msg) const;
    // Build card (1d follow-up): structured slot lines + Build button —
    // the ops card's visual language applied to CHAIN blocks
    int  chainCardHeight(const ChatMsg& msg) const;
    // AI Compare figure card: single height source + renderer (paint-only, no
    // buttons). figuresData is client-built at compose time and persisted, so
    // both survive an editor recreate with no editor-instance state.
    int  figureCardHeight(const ChatMsg& msg) const;
    void drawCompareFigureCard(juce::Graphics& g, juce::Rectangle<int> area,
                               const ChatMsg& msg);
    void applyChainEditFromMsg(int msgIdx);

    // ---- ASK choice chips (Phase 1b, B2 docked-shelf layout) ----------------
    // Chips live in a SHELF docked seamlessly on top of the chat input (not
    // under the message), shown only while a newest unanswered
    // <<<ECHOJAY_ASK>>> exists. A tap formats the answer as
    // Label (answering: "question") and auto-sends; ANY user send (tap or
    // free text) supersedes the pending ask, so the shelf always vanishes on
    // send. Reflow: the shelf rect participates in the same layout chain as
    // the model-tier banner (chatScrollBottom), so the message viewport
    // shrinks/grows with it and the last message is never hidden.
    //
    // PARITY (spec gate) — these values MIRROR public/app.html .ask-shelf /
    // .ask-chip CSS; keep in sync:
    //   chip: pill (fully rounded), fill cyan 9% (hover 18%), border cyan
    //         40% (hover 70%), text 0xff7FE3F2 @ 12.5px, pad 6v/14h
    //   shelf: input bg + 5% cyan wash, radius 12px top corners only,
    //          hint "or type below" muted after the chips
    struct AskChipLnF : juce::LookAndFeel_V4
    {
        void drawButtonBackground(juce::Graphics& g, juce::Button& b,
                                  const juce::Colour&, bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced(0.5f);
            const auto cyan = juce::Colour(0xff22d3ee);
            g.setColour(cyan.withAlpha(over || down ? 0.18f : 0.09f));
            g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
            g.setColour(cyan.withAlpha(over || down ? 0.70f : 0.40f));
            g.drawRoundedRectangle(r, r.getHeight() * 0.5f, 1.0f);
        }
        juce::Font getTextButtonFont(juce::TextButton&, int) override
        { return juce::Font(juce::FontOptions(12.5f)); }
    };
    AskChipLnF askChipLnF_;   // declared BEFORE the buttons: destroyed after them
    static constexpr int kMaxAskChips = 4;
    static constexpr int kAskChipH = 27;   // 12.5px text + 6px vertical padding
    std::array<juce::TextButton, kMaxAskChips> askChipBtns;
    std::array<juce::String, kMaxAskChips> askChipLabels;
    std::array<juce::String, kMaxAskChips> askChipIntents;   // ""|"edit"|"build" (3-pre)
    // ---- Channel selection (banner dropdown; the chip bar is deleted) ----
    // THE channel selector: clicking the banner opens a menu of LIVE
    // channels from the registry (current one ticked; an offline current
    // channel listed dimmed). Selecting uses the same open-or-pend path
    // the chips used. EXACTLY ONE selector is visible at any time — the
    // composer pill hides wherever the banner shows.
    void showChannelBannerMenu();
    void openChannelByUid(const juce::String& uid);
    // Label for the no-channel state (dropdown first entry + banner text in
    // a main chat on the Link tab). SAME name source as the Link Monitor's
    // top row (getEffectiveChannelName) so the two cannot disagree. The
    // no-channel state genuinely means both "this EchoJay's rack" (builds)
    // and "the whole project" (analysis/capture) — one label, one state.
    juce::String mainContextLabel() const;
    // Item 2: material context the model reasons about. Channel chat -> the
    // channel's resolved name (let the model infer the material, never
    // classify client-side). Main chat -> the header material hint.
    juce::String materialContextName(const juce::String& mainDefault) const;
    // Item 1: THE capture name (single source: chat-revision + project). Used
    // to stamp the snapshot at press and as the review passName, so the
    // snapshot, review and card never derive from different counters.
    juce::String computeNextCaptureName() const;
    // Item 3: capture-button width authored in resized() (captureBtnMaxW_);
    // the state block fits the label to it, truncating the CHANNEL NAME and
    // keeping "Capture" (the part that says what the button does). Decoupled
    // via the stored width so the timer and resized() never fight the text.
    int captureBtnMaxW_ = 64;
    juce::String fitCaptureLabel(const juce::String& full) const;
    // Item 4a: set the capture button to the fitted label for the CURRENT
    // active chat/state. Called on chat activation (not only the capture
    // state block), so switching into a channel chat refreshes it at once.
    void refreshCaptureButtonLabel();
    // Reset to the clean main state (no active chat, no pending, no router
    // selection). Selecting the main entry in the dropdown and New chat
    // from an empty channel context both come through HERE — the main
    // entry is a LABEL for the existing no-channel state, never a new
    // targeting destination (no sentinel uid exists anywhere).
    void resetToMainContext();
    juce::String askChipQuestion_;     // question of the chips currently shown
    int  askChipMsgIdx_ = -1;          // chatMessages index the chips belong to
    int  activeAskChips = 0;
    juce::Rectangle<int> askShelfRect_, askShelfHintRect_;
    bool askShelfVisible_ = false;
    // Newest assistant message with an unanswered ask, -1 if none
    int findNewestUnansweredAsk() const;
    // Mark every pending ask answered (any user send supersedes the question)
    void supersedePendingAsks();
    // Shared by the layout pass (height reservation + button placement) and
    // the paint pass (shelf background + hint) — they MUST agree. Flows
    // chips + trailing hint into rows for the given shelf width; rects are
    // relative to the shelf origin. Returns total shelf height (0 = no shelf).
    int measureAskShelf(const ChatMsg& msg, int shelfW,
                        std::vector<juce::Rectangle<int>>* chipRectsOut = nullptr,
                        juce::StringArray* labelsOut = nullptr,
                        juce::String* questionOut = nullptr,
                        juce::Rectangle<int>* hintRectOut = nullptr,
                        juce::StringArray* intentsOut = nullptr);

    // ---- AI-proposed Link gain (APPLY cards) --------------------------------
    // The assistant may emit a <<<ECHOJAY_GAIN>>> block of measurement-backed
    // proposals; each renders as a painted card in the reply with an Apply
    // button (then Applied + Undo). NEVER auto-applies. The applied flag +
    // previous gain (for undo) are stored INSIDE the proposal JSON on the
    // message (ChatMsg.gainData) and mirrored to the workspace, so they
    // survive chat switches and reload (the _chain persistence pattern).
    // Painted click zones, rebuilt each chat paint, address a proposal by
    // (message index, proposal index) so Apply can mutate + re-persist it.
    struct GainCardZone {
        juce::Rectangle<int> rect;
        int   msgIndex = -1;     // index into chatMessages
        int   propIndex = -1;    // index into that message's proposals array
        juce::String uid;        // resolved Link address to send to
        float proposed = 0.0f;   // dB to apply
        bool  isUndo = false;    // Undo button (else Apply)
    };
    std::vector<GainCardZone> gainCardZones_;
    static constexpr int kGainCardH = 52;
    // Build the LINK LEVELS context + proposal format/grounding instructions
    // for a chat turn; empty when there are no live Links to reason about.
    juce::String buildLinkLevelsContext();
    // Resolve a proposal's linkId (name or uid) to a sendable address.
    juce::String resolveLinkProposalAddr(const juce::String& linkId) const;
    void applyGainProposal(const GainCardZone& z);
    void showChainPluginPicker();                       // "+" button popup
    void loadChainFromJson(const juce::String& chainJson);

    // ---- Link chain send side ----
    // Destination is never ambiguous under the router rule: a channel chat
    // builds on ITS channel, a main chat on the local rack (the target
    // menu and suggestedTarget were deleted together).
    void sendChainToLink(const juce::String& linkUid, const juce::String& chainJson);
    // chainJson: the REQUESTED chain — used ONLY for role wording when a
    // failed plugin has no surviving built neighbour; anchors themselves
    // derive from the ack's built-only entries, never the request.
    void pollLinkChainAck(const juce::String& linkUid, int seq, int attemptsLeft,
                          const juce::String& chainJson = juce::String());
    // ---- Phase R: Link-targeted edits over the v:2 cmd/ack transport ----
    LinkShm::RackSidecar readLinkRackSidecar(const juce::String& uid) const;
    void applyChainEditToLink(int msgIdx);
    int  sendChainEditToLink(const juce::String& linkUid,
                             const juce::String& editJson);   // returns seq, -1 on failure
    void retireLinkEditCard(const juce::String& editDataKey,
                            const juce::String& chatIdAtApply,
                            const juce::String& summary,
                            const juce::String& altPrompt,
                            const juce::String& altLabel,
                            const juce::String& resultBubble);
    void pollLinkEditAck(const juce::String& linkUid, int seq, int attemptsLeft,
                         const juce::String& editDataKey, const juce::String& chatIdAtApply,
                         const juce::String& targetLabel, int totalOps,
                         const std::vector<ChainHost::ChainEditOp>& opsForAlt,
                         const juce::StringArray& baseSlots);
    // ONE author for the "Suggest an alternative" follow-up (local apply +
    // Link ack both consume it — the 1c prompt rules cannot drift apart)
    void buildEditAltFollowUp(const juce::StringArray& results,
                              const std::vector<ChainHost::ChainEditOp>& opsForAlt,
                              const juce::StringArray& baseSlots,
                              juce::String& altPromptOut, juce::String& altLabelOut);

    // ---- LINK tab remote Active control ------------------------------------
    // Per-row toggle writes ctrl-cmd-<id>.json {v:1, seq, active}; the Link
    // applies it (authority stays with the Link) and acks; the row shows a
    // pending style until the ack, and a NO RESP state on timeout.
    struct LinkCtrlPending {
        juce::String addr;   // the Link's ADDRESS (uid; legacy name-derived fallback)
        int  seq = 0;
        bool target = false;
        bool timedOut = false;
        bool isGain = false;    // this pending is a gain change (else Active)
        float gainDb = 0.0f;    // desired gain for optimistic row display
    };
    std::vector<LinkCtrlPending> linkCtrlPending_;

    // LINK MONITOR row list — scrollable. The Mix Bus card stays PINNED in
    // the panel painter above; only Link rows live in this viewport child.
    // Rows render in LIST order: alphabetical by name, Untitleds last (by
    // uid, so the list is stable while scrolling). The viewport preserves
    // scroll position across 10Hz data refreshes and tab switches; the
    // shared LookAndFeel supplies the thin EchoJay scrollbar.
    struct LinkListView : juce::Component
    {
        EchoJayEditor* owner = nullptr;
        // toggle zones in LOCAL coords, carrying the row's ADDRESS (uid)
        std::vector<std::pair<juce::Rectangle<int>, juce::String>> zones;
        // gain slider track per row: {trackRect, addr}. The slider is
        // custom-painted; drag/throttle state lives below.
        struct GainZone { juce::Rectangle<int> rect; juce::String addr; };
        std::vector<GainZone> gainZones;
        // placement chips: {rect, addr} — click opens the bus/insert chooser
        std::vector<std::pair<juce::Rectangle<int>, juce::String>> placementZones;
        // Active drag: which row's slider is being dragged, the live value,
        // and a ~10Hz send throttle (final value always sent on mouseUp).
        juce::String  dragAddr;
        float         dragValue = 0.0f;
        uint32_t      lastGainSendMs = 0;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        // dB<->x for a track rect (shared by paint + drag)
        static float gainFromX(int x, juce::Rectangle<int> track);
        static int   xFromGain(float db, juce::Rectangle<int> track);
    };
    static constexpr int kLinkRowH = 64, kLinkRowGap = 8;
    LinkListView   linkListView_;
    juce::Viewport linkListViewport_;
    // uid for a named Link from the registry; legacy name-derived fallback
    // for pre-uid Links (empty uid in their slots)
    juce::String linkAddrForName(const juce::String& linkName) const;

    // LINK tab mini meter strips — last frame + staleness per Link name.
    // fresh = seq advanced within the last second; otherwise the strip
    // freezes on last-known values and dims (no fake motion). The 10Hz data
    // feed is rendered at the UI frame rate through per-value smoothing
    // (meter attack/release), and level history lives in a small ribbon
    // ring (~4s at 10Hz).
    struct LinkStripState {
        LinkMeterFrame frame;              // latest raw frame
        uint32_t lastSeq = 0;
        uint32_t lastChangeMs = 0;
        bool has = false;
        // Smoothed render values — updated each paint tick toward frame
        float smMom = -100.0f, smInt = -100.0f;
        // Smoothed loudness-suite readouts (display-side, updated per paint
        // tick toward the latest frame; frozen when stale)
        float smShort = -100.0f, smLra = 0.0f, smPsr = 0.0f, smPlr = 0.0f;
    };
    std::map<juce::String, LinkStripState> linkStripStates_;
    LinkStripState linkHostStrip_;         // the Mix Bus (this instance) row
    // Mix Bus audio liveness: the host can idle the MAIN plugin's channel
    // too — detect the local engine freezing (values unchanged ~1s) and
    // apply the same audioStale treatment as Link rows
    float    lastHostMom_ = 0.0f, lastHostRms_ = 0.0f;
    uint32_t lastHostAdvanceMs_ = 0;
    // Shared renderer for host + Link rows (loudness-suite readout cells)
    void paintLinkMeterStrip(juce::Graphics& g, int stripX, int stripR,
                             int rowY, int rowH, LinkStripState& st,
                             bool fresh, float dim);
    void sendLinkActiveCommand(const juce::String& linkAddr, bool active);
    // Remote gain: ctrl-cmd carrying the CURRENT active + a new absolute
    // gainDb field, acked like Active. Authority stays with the Link.
    void sendLinkGainCommand(const juce::String& linkAddr, float gainDb);
    // Remote placement declaration (0 unset, 1 bus, 2 insert) via ctrl-cmd.
    void sendLinkPlacementCommand(const juce::String& linkAddr, int placement);
    void showLinkPlacementMenu(const juce::String& linkAddr);
    // AI-driven level match: compute the absolute gain that lands this Link's
    // integrated loudness at targetLufs (from its freshest frame + current
    // gain), then send it. Returns the dB that WOULD be applied for the
    // proposal text; applies only when apply=true (never auto).
    float computeLinkMatchGain(const juce::String& linkAddr, float targetLufs) const;
    void  showLinkGainMenu(const juce::String& linkAddr);   // adjust/match/reset
    // Current gain a row should DISPLAY: optimistic pending value while a
    // gain command is in flight, else the registry-reported gain.
    float linkRowDisplayGain(const juce::String& linkAddr) const;
    void pollLinkCtrlAck(const juce::String& linkAddr, int seq, int attemptsLeft);
    void promptForFailedPlugins(juce::StringArray failed);
    void showNextFailPrompt(juce::StringArray names, int idx);
    // Shared disable action (local + Link build failures): untick in the
    // scanner (plugin_disabled.json), refresh checklist, rebuild recommendable.
    void disablePluginByName(const juce::String& name);
    // Link build results with load_failed entries: one dialog, per-plugin
    // "don't suggest again" toggle rows (no modal chain).
    std::set<juce::String> chainFailSessionSeen_; // names user chose "Keep it" this session

    juce::String currentlyPlayingChatWav;
    std::unique_ptr<juce::ChildProcess> chatPlaybackProcess;
    double chatPlaybackStartTime = 0; // Time::getMillisecondCounterHiRes() when play started
    float chatPlaybackDuration = 0;   // duration of currently playing wav
    float chatPlaybackOffset = 0;     // seek offset in seconds
    void onWavePlayClick(int index);
    void onWaveSeekClick(int index, float fraction);
    void startChatPlayback(const juce::String& wavPath, float offset = 0);
    void stopChatPlayback();
    
    // Waveform display
    std::vector<WaveformRecorder::ThumbnailPoint> frozenWaveform; // snapshot after capture stops
    bool waveformFrozen = false;
    bool captureWasSilent = false; // must go silent before unfreeze
    int unfreezeCountdown = 0; // timer ticks until auto-unfreeze (0 = inactive)
    juce::TextButton playbackBtn { "Play" };
    juce::TextButton abSyncBtn { "Sync" };
    juce::Label wavSavedLabel;
    
    // Simple playback engine (plays back through system default output)
    bool isPlayingBack = false;
    int playbackPosition = 0;
    void startPlayback();
    void stopPlayback();
    
    // NOT OWNED HERE ANY MORE. The one client lives on the processor so it
    // outlives this editor, which Logic destroys on every Link window
    // switch. Bound in the constructor's init list from processorRef, and
    // the name stays `api` so all 73 call sites read unchanged.
    // Session C: the unread generation this editor has already drawn. The
    // COUNTS live on the processor; this is the only dashboard state the
    // editor owns, and losing it on an editor recreation is harmless because
    // the constructor re-reads it.
    int dashUnreadSeen_ = 0;

    EchoJayAPI& api;
    EchoJayWorkspace workspace { api };

    // =========================================================================
    //  Link tab — live auto-discovery panel
    // =========================================================================
    void paintLinkMonitorPanel(juce::Graphics& g, juce::Rectangle<int> area);
    int  linkRefreshTick = 0;

    // Update notification
    bool updateAvailable = false;
    bool updateDismissed = false;
    int updateCheckCounter = 0;
    static constexpr int kUpdateCheckInterval = 20 * 60 * 360; // ~6 hours at 20fps
    
    // Update overlay child component — drawn ON TOP of all other children including
    // particleVisual. Paints its own dark background + card. Handles its own clicks.
    // State machine: Idle → Downloading (with progress) → ReadyToInstall → done
    // (or Failed). The editor owns the actual download thread and just flips
    // these fields + repaints; the overlay is pure presentation.
    struct UpdateOverlay : public juce::Component
    {
        enum class State { Idle, Downloading, ReadyToInstall, Failed };
        State state { State::Idle };
        float progress = 0.0f;          // 0..1 download progress
        juce::String errorText;         // shown when state == Failed
        
        std::function<void()> onDownload;   // user clicked Download Update (Idle)
        std::function<void()> onInstall;    // user clicked Install Now (ReadyToInstall)
        std::function<void()> onRetry;      // user clicked Try Again (Failed)
        std::function<void()> onDismiss;    // user clicked Not now / Close
        
        juce::String latestVersionStr;
        juce::String currentVersionStr;
        using C = EchoJayLookAndFeel::Colours;
        
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
    };
    UpdateOverlay updateOverlay;

    // ---- Plugin review / checklist -------------------------------------
    // Shared checklist component (one instance for the post-scan review
    // overlay, one for the Settings view). Each lives inside a Viewport so a
    // long list scrolls. The post-scan overlay is a dark backdrop + card that
    // appears after a scan completes, prompting the user to untick plugins
    // they don't own (chiefly unlicensed Waves from a WaveShell). The Settings
    // checklist is always available to edit the same state later.
    struct PluginReviewOverlay : public juce::Component
    {
        bool visibleState = false;
        std::function<void()> onDone;        // "Done" / "Save"
        std::function<void(bool)> onSelectAll; // true=all, false=none
        std::function<void()> onAddManual;   // "+ Add plugin"
        using C = EchoJayLookAndFeel::Colours;
        juce::Rectangle<int> cardBounds, doneBtn, allBtn, noneBtn, addBtn, closeBtn, searchBounds;
        juce::String hintText; // "Showing N of M — search to narrow"
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
    };
    PluginReviewOverlay reviewOverlay;
    juce::Viewport reviewViewport;
    std::unique_ptr<PluginChecklistComponent> reviewChecklist;
    // Search box inside the popup overlay — filters the checklist as you type.
    juce::TextEditor reviewSearchBox;

    // Settings plugin area: instead of a giant inline list, Settings shows a
    // search box (inline filtered results) plus a "View all" button that opens
    // the same scrollable popup used after a scan. This keeps Settings light
    // even when the user has thousands of plugins.
    // Settings PLUGINS row is now compact: a count line ("2009 plugins
    // detected") plus a "View all" button that opens the scrollable popup
    // (which has its own search + collapsible sections). The inline checklist
    // and search box are still kept as members for the popup path but are NOT
    // shown inline in Settings anymore.
    // Settings PLUGINS row: a scan button (identical to the header one — shows
    // the count, opens the scan menu) with "View all" right beside it, and a
    // Help & Support button filling the other half (opens the website).
    juce::Viewport settingsPluginViewport;
    std::unique_ptr<PluginChecklistComponent> settingsChecklist;
    juce::TextEditor settingsPluginSearchBox;
    juce::TextButton viewAllPluginsBtn { "View all" };
    juce::TextButton settingsScanBtn { "Scan Plugins" };
    juce::TextButton settingsHelpBtn { "Help & Support" };
    // Opens https://www.echojay.ai/manual — sits beside Save on the bottom
    // row (left cluster), same quiet link style as Help & Support
    juce::TextButton settingsManualBtn { "Manual" };

    // Scrollable Settings: the settings-only children live inside
    // settingsContent_, viewed through settingsViewport_. resized() stays
    // the single layout authority: it lays the content out in CONTENT
    // coordinates and grows the content past the viewport when the window
    // is short, so every section (Usage included) stays reachable and the
    // Save row lays out WITHIN the flow instead of floating over other
    // controls. The painted labels/cards render in the content's paint
    // hook so they scroll and clip with it.
    struct SettingsContent : juce::Component
    {
        std::function<void(juce::Graphics&)> paintFn;
        void paint(juce::Graphics& g) override { if (paintFn) paintFn(g); }
    };
    juce::Viewport settingsViewport_;
    SettingsContent settingsContent_;
    // Debug: dumps MeterEngine::getMeterDataJSON() to ~/Documents/EchoJay/
    // meter-debug.json + appends a one-line summary to meter-debug.log
    juce::TextButton dumpMetersBtn { "Dump meters" };

    // Commits the checklists' local selections to the scanner + server. Set in
    // the constructor; invoked on review-popup Done and on Settings Save.
    std::function<void()> commitChecklistFn;

    void applyReviewModalState();   // review-modal open/close: one path for all Chain-tab visibility
    Tab reviewReturnTab_ { Tab::Visualisation }; // tab to restore when the review modal closes
    void showPluginReview();
    void hidePluginReview();

    // ---- Chat sidebar (Phase 2a) -------------------------------------------
    static constexpr int kSidebarW = 210;

    // Which albums are collapsed (by album id). Default = all expanded.
    // Album ids + "proj:<name>" keys the user has collapsed. Persisted to a
    // global prefs file so songs stay collapsed across restart / project
    // reopen (the active chat's project still force-expands in refreshRows).
    std::set<juce::String> collapsedAlbums;
    void loadCollapsedState();
    void saveCollapsedState() const;

    // Currently open chat id (empty = none)
    juce::String currentChatId;

    // Temporary debug readout shown in the status-line slot (Chat tab)
    juce::String sidebarDebugText;

    // ListBoxModel for the chat sidebar. Owns the flat row list and
    // routes clicks back into the editor via callbacks.
    // Sidebar toolbar (above the ListBox)
    static constexpr int kSidebarToolbarH = 30;
    juce::TextButton sidebarNewChatBtn  { "+ New chat"  };
    juce::TextButton sidebarNewAlbumBtn { "+ New album" };

    struct ChatSidebarModel : public juce::ListBoxModel
    {
        struct Row {
            enum class Kind { SectionTitle, AlbumHeader, ProjectHeader, ChatRow, ReviewRow };
            Kind         kind   = Kind::SectionTitle;
            juce::String id;      // AlbumHeader: album id; ProjectHeader: project name
            juce::String label;
            juce::String meta;
            bool         collapsed = false;
            bool         active    = false;
            bool         pinned    = false;   // ChatRow: draws the pin glyph
            int          indent    = 0;
        };
        std::vector<Row> rows;

        std::function<void(const juce::String&)> onChatClicked;
        std::function<void(const juce::String&)> onAlbumToggled;
        // Project header collapse toggle + right-click "Move to album..."
        std::function<void(const juce::String& projectName)> onProjectToggled;
        std::function<void(const juce::String& projectName)> onProjectContextMenu;
        // Right-click on a chat row — editor shows rename/delete/move menu
        std::function<void(const juce::String& chatId)> onChatContextMenu;
        // Right-click on an album header — editor shows rename/delete menu
        std::function<void(const juce::String& albumId)> onAlbumContextMenu;

        int  getNumRows() override { return (int)rows.size(); }
        void paintListBoxItem(int rowNum, juce::Graphics& g,
                              int width, int height, bool isSelected) override;
        void listBoxItemClicked(int rowNum, const juce::MouseEvent&) override;

        void refreshRows(const std::vector<WsChat>&,
                         const std::vector<WsAlbum>&,
                         const std::vector<WsReview>&,
                         const std::vector<WsPinnedProject>& pinnedProjects,
                         const std::set<juce::String>& collapsed,
                         const juce::String& activeChatId);
    };
    std::unique_ptr<ChatSidebarModel> sidebarModel;
    juce::ListBox chatSidebar { {}, nullptr };

    void loadChatFromWorkspace(const juce::String& chatId);
    void createNewChat();
    // The trackName (song/project) a NEW chat should get: the current header
    // project name if set, else the session's auto-project ("Untitled, <date>",
    // created once per session and adopted in place when a real name arrives).
    // Never returns empty, so a new chat is never Ungrouped.
    juce::String newChatProjectName();
    juce::String ensureSessionAutoProject();   // the session auto-project name
    // Adoption: rename the session auto-project's chats to the real name in
    // place (called when the user sets the header project name mid-session).
    void adoptSessionAutoProjectName(const juce::String& realName);
    void createNewAlbum();
    void showMoveToAlbumMenu(const juce::String& chatId);
    // Project (song) row context menu: Move to album... / New album... / Remove.
    void showMoveProjectToAlbumMenu(const juce::String& projectName);
    void showAlbumContextMenu(const juce::String& albumId);
    juce::String getCurrentAlbumId() const; // album containing currentChatId
    // Returns the reviewId of the created review (empty string if not logged in)
    juce::String createReviewFromCapture(const CaptureSnapshot& snap, const juce::String& wavPath,
                                         const juce::String& linkUid = juce::String());

    // Shows the plugin scan menu (Scan Now / Add Folder / manage folders)
    // anchored to the given component. Shared by the header scan button and
    // the Settings scan button so both behave identically.
    void showScanMenu(juce::Component* target);
    void layoutPluginReview();
    
    // In-plugin installer download state. Set when the user clicks Download
    // Update in the overlay; the path is what we hand to Process::openDocument
    // when they then click Install Now. Mutated from the download worker
    // thread — but only via MessageManager::callAsync back to the UI thread,
    // so no extra synchronisation here.
    juce::File downloadedInstallerFile;
    // Liveness token shared with the download thread so we can ignore late
    // callbacks if the editor is destroyed mid-download.
    std::shared_ptr<std::atomic<bool>> updateDownloadAlive {
        std::make_shared<std::atomic<bool>>(true) };
    // Cooperative cancel flag for the download thread. The dismiss/cancel
    // path flips this; the worker polls it between chunks and bails out
    // cleanly. shared_ptr so its lifetime outlives any single download.
    std::shared_ptr<std::atomic<bool>> updateDownloadCancelled {
        std::make_shared<std::atomic<bool>>(false) };
    void startUpdateDownload();
    void launchDownloadedInstaller();
    
    using C = EchoJayLookAndFeel::Colours;
    EchoJayLookAndFeel lnf;

    // ---- Meter hover tooltips ------------------------------------------
    // The METERS painters record {rect, text} zones each frame (editor
    // coordinates); getTooltip() hit-tests the mouse against them. One
    // shared TooltipWindow serves these AND the Button tooltips (chain card
    // header, strip B/X). ~700ms hover delay.
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> meterTips_;
    void addMeterTip(const juce::Rectangle<int>& r, const juce::String& t)
    { meterTips_.emplace_back(r, t); }
public:
    juce::String getTooltip() override;
private:
    juce::TooltipWindow tooltipWindow_ { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayEditor)
};
