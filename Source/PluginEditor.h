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
    // DEV ONLY (`/eqtest {...}`): apply a hand-written eq_bands JSON to the
    // built-in EQ, so the exact-apply path is testable before the backend
    // emits eq_bands. Gated on ChainHost::devModeActive() at the call site.
    void handleDevEqTest(const juce::String& jsonArg);
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

    // The conversation-conduct rule ("don't ask which channel; don't
    // volunteer capture/meter status") shared VERBATIM by both chat
    // surfaces: the [TARGET CHANNEL] declaration (Link-targeted chats) and
    // the [THIS CHANNEL] declaration (the main plugin's own chat). Each
    // block builder supplies ONLY its channel-identity clause; the conduct
    // tail lives once in chainConductRule, so the two surfaces cannot
    // drift. Pure statics so the self-test binary reads the exact bytes the
    // compose path appends (tools/chainguidance_test).
    // WORDING CONSTRAINT, for the tail AND every identity clause: the text
    // must stay safe against the server's typed-portion classifiers — no
    // EDIT_REQUEST_RE verb families (remove/swap/take out/get rid of/
    // reorder/move/bypass/insert/replace...), and no chain-request verb
    // followed by "chain" within 30 chars (CHAIN_REQUEST_RE): "chain"
    // always PRECEDES its verbs here ("every chain you build or edit").
    // The server's regex source is not in this repo; the self-test lints
    // the documented constraint, but reworded text must still be checked
    // against the live patterns, not assumed safe.
    static juce::String chainConductRule(const juce::String& identityClause);
    static juce::String targetChannelDeclaration(const juce::String& channelPhrase);
    static juce::String mainChannelDeclaration();
    static bool runChainGuidanceSelfTest();
    friend struct EchoJayChainGuidanceTestAccess;

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
    void paintKeyPanel(juce::Graphics& g, juce::Rectangle<int> area);
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

    // ---- Detected key: the shared source collector -----------------------
    // THE ONE precedence walk now lives on the PROCESSOR
    // (EchoJayProcessor::collectKeySources) so the KeyFeed that EchoJay
    // Pitch follows is published with or without an open window — when the
    // walk lived here, closing the editor froze the key at its last value.
    // The editor keeps aliases and a delegating call so its UI code reads
    // the very same ranking; there is still exactly one walk.
    using KeySourceReading = EchoJayProcessor::KeySourceReading;
    using KeySources       = EchoJayProcessor::KeySources;
    KeySources collectKeySources() { return processorRef.collectKeySources(); }

    // ---- KEY panel (Meters middle row, KEY_PRECONDITION_SPEC.md §1) ------
    // The UI cache is refreshed at 2 Hz on the editor timer (the walk copies
    // snapshots under a mutex — too heavy for 20 Hz paint); paint reads this
    // cache. keyChromaShown_ eases toward the primary source's
    // chroma at the DwellGlow time constant so the wheel moves with the same
    // hand as the device's.
    KeySources keySources_;
    int keySourcesDiv_ = 0;
    std::array<float, 12> keyChromaShown_ {};
    juce::Rectangle<int> keyPanelBounds_, keyReanalyseRect_;
    // RE-ANALYSE feedback: the remote pass has no visible activity, so the
    // chip says "listening" until the reading's age drops (or 15 s pass).
    juce::uint32 keyReanalyseSentMs_ = 0;
    void triggerKeyReanalyse();
    // §7 source selector: one PopupMenu built from keySources_ (never a
    // second enumeration), opened from the SOURCE chip in both panel forms.
    juce::Rectangle<int> keySourceMenuRect_;
    void showKeySourceMenu();
    static juce::String keySourceShortLabel(const KeySourceReading& s);
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
    /** THE single width authority. Reads the live collapse flag. */
    ColumnLayout computeColumns(int width) const;
    /** The same arithmetic with the collapse stated explicitly, so a caller
        can ask the shape question "would this surface have a sidebar if it
        were not collapsed" without a second list of tabs to maintain. The
        one-argument form forwards to this with the live flag, so there is
        still only ONE implementation of the widths. */
    ColumnLayout computeColumns(int width, bool collapsed) const;

    /** Whether the header's collapse control is on screen. DERIVED FROM
        computeColumns, never from a tab list: a hand-maintained list is the
        duplicate-authority shape that produced the three stale width copies
        f43a698 had to clean up. See the definition for what the two terms
        rule out. */
    bool chatCollapseControlVisible() const;

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
        // STAGED CHAIN (spec section 4): true while chainData holds slots
        // reported by onSlot and the block has NOT closed. The card renders
        // its rows; the Build button does NOT appear until onBlock replaces
        // this with the complete payload and clears the flag. A stream that
        // dies here leaves rows and no button, which is the honest state.
        //
        // NOT PERSISTED, and not an oversight: WsMessage has no counterpart,
        // same as provisionalId and clientAskKind. A reloaded chat can only
        // ever hold a finished chain, so there is nothing for a persisted
        // copy to say.
        bool chainProvisional = false;
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
        // Split call: non-zero on the PROVISIONAL bubble rendered from the
        // classifier's preamble. A rendering artefact, never history.
        //
        // FOUR STORES, ONE DESTINATION. A provisional bubble reaches
        // chatMessages and nothing else:
        //   chatMessages              <- here, and only here
        //   processorRef.chatHistory  <- no: replayed on editor recreate
        //   chatRoles / chatContents  <- no: this IS the API history, and a
        //                                phantom assistant turn there would
        //                                ride out inside the next send's
        //                                12-message trim
        //   workspace (/api/data)     <- no: round-trips to the server and
        //                                comes back on reload
        // WsMessage has no counterpart to this field, deliberately: there is
        // nothing to serialise it into, so the persistence side cannot carry
        // a provisional even by accident.
        //
        // An ID rather than a bool because the drop/replace sites are
        // reached asynchronously and a captured INDEX can drift — a second
        // send landing first shifts the vector under the first send's
        // callback. Identity survives that; an index does not.
        int provisionalId = 0;
        // Which client-rendered ASK this message IS, when the client built it
        // rather than the model ("channel_mismatch"). Empty for every other
        // message, including model-authored ASK blocks.
        //
        // NOT PERSISTED, AND NOT AN OVERSIGHT. WsMessage has no counterpart
        // and must not grow one. The only reader is the switch guard, which
        // runs IN-SESSION immediately after a chip tap to answer "is the
        // newest assistant turn the mismatch question I just rendered?" — a
        // question that only has meaning between rendering it and acting on
        // it. Same lifetime and same reasoning as provisionalId above. A
        // reloaded chat has no tap in flight, so there is nothing for a
        // persisted copy to answer.
        juce::String clientAskKind;
    };
    std::vector<ChatMsg> chatMessages;
    // THE shared display source: both text-layout passes (measure + paint)
    // MUST get their string from here so heights and pixels cannot disagree.
    //
    // Edit turns show the CARD ONLY (9 Aug 2026): the preamble is written
    // before the outcome exists, so it can only ever be a prediction
    // rendered as a statement - "Dialling Ratio to 4" sat above three
    // surfaces saying nothing was written. Instruction is a ceiling, never
    // a floor: four rounds of it were each absorbed at the surface and none
    // guaranteed. The card is the one place proposal grammar is TRUE,
    // because it has an Apply button and speaks before the outcome by
    // design. Keyed on editData non-empty, so blockless replies (declines,
    // advice, ASK) keep their prose untouched. PLUGIN DISPLAY ONLY: the
    // model still writes the preamble (protocol unchanged, other clients
    // unaffected); the web app renders its own and is a separate, later
    // decision.
    static const juce::String& displayedText(const ChatMsg& m)
    {
        static const juce::String kCardOnly;
        if (m.role == "assistant" && m.editData.isNotEmpty()) return kCardOnly;
        return m.displayText.isNotEmpty() ? m.displayText : m.content;
    }
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
    // OWNERSHIP-GATED (14 Aug 2026): f2941ad gated the stage row's WRITERS
    // (thinking promotion, switch-site clears), but the row also renders
    // from chatLoading alone -- which survives a chat switch -- so the
    // "Thinking" state followed the user into whichever chat they opened
    // while the message paints correctly suppressed. This helper is the ONE
    // height source both the paint pass and the measure pass consult, so
    // gating it here gates every renderer at once. stageRowBelongsHere is
    // true when no turn is in flight (dial/apply labels, scan hold and the
    // chain shimmer are view state and show where they were set).
    int stageRowH() const { return (chatLoading || stageStatusText_.isNotEmpty())
                                    && stageRowBelongsHere() ? 30 : 0; }
    bool stageRowBelongsHere() const;
    // Counting half of the gate: bumps the ACTIVE stream turn's suppressed
    // figure (null between turns), so the render line stays a complete
    // account. Called once per suppressing chat switch, never per frame.
    std::function<void()> bumpStreamSuppressed_;
    void noteStageSuppressedIfForeign();
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
    // The edit twin (item 3, 9 Aug 2026): same settle-then-compose contract,
    // scoped to the slots the edit's ops actually touched.
    void finishEditBubbleWhenDialSettled(const juce::String& editJson, int attemptsLeft);
    // Receipt-time consumption of suggestion-only set ops (9 Aug 2026):
    // Apply exists to confirm a CHANGE to the rack; a prose-only set
    // changes nothing (it writes the slot card's suggested-settings text),
    // so offering Apply for one asks the user to confirm an action that
    // does not exist. LOCAL rack only - Link-targeted cards keep their
    // remote round-trip. Executes the prose-only sets immediately under a
    // guard (slot in range AND baseSlots name matches the live slot; a
    // failed guard leaves the op for the normal Apply path and its
    // staleness machinery). Returns the edit JSON with consumed ops
    // removed; allConsumed = the edit array emptied. Mixed batches
    // compose: real ops keep the card and its Apply, suggestions land now.
    juce::String consumeSuggestionSetsAtReceipt(const juce::String& editJson,
                                                bool& allConsumed,
                                                juce::StringArray& consumedNames);
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
        // Bottom pin (14 Aug 2026). While true, new content keeps the view
        // at the bottom (the timer's auto-scroll). Cleared the moment a USER
        // scroll leaves the bottom band, restored when one returns to it, so
        // scrollback stays readable mid-stream instead of being yanked down
        // on every delta. programmaticScroll must be held around every
        // code-driven setSize/setViewPosition on this viewport: those fire
        // visibleAreaChanged exactly like a user scroll, and re-deriving the
        // pin from a move the code itself made is the thrash this flag
        // exists to prevent (content growth alone pushes the bottom away
        // and would unpin every streaming turn).
        bool pinnedToBottom     = true;
        bool programmaticScroll = false;
        static constexpr int kBottomTolerancePx = 24;
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
        void visibleAreaChanged(const juce::Rectangle<int>& area) override
        {
            if (! programmaticScroll)
            {
                const auto* viewed = getViewedComponent();
                const int contentH = viewed != nullptr ? viewed->getHeight() : 0;
                pinnedToBottom = area.getBottom() >= contentH - kBottomTolerancePx;
            }
            if (onScroll) onScroll();
        }
    };
    ChatViewport chatScroll;
    // Follow the bottom AT the content change, not where a disagreement is
    // detected (14 Aug 2026). The timer-side height check could never fire
    // during a streamed turn: paintBubble ran resized() per event, resized()
    // re-synced chatContent's height, and the timer then saw agreement at
    // every tick, so streamed rows rendered below the fold. Every site that
    // changes content height calls this; the timer keeps a backstop role
    // for height changes that happen outside resized(). No-op unless
    // pinned; counts fires into chatFollowFires_ so a turn's render line
    // can say "9 paints, 0 follows", which IS this bug's signature.
    void followBottomIfPinned();
    int  chatFollowFires_ = 0;   // running total; per-turn delta via snapshot
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
    // The channel's MOST RECENT chat by activity (updatedAt else created);
    // "" = none. matchesOut (optional) receives how many chats matched, for
    // the "latest of N" log line. Selection logic: echojay::latestChatForLink.
    juce::String latestChannelChatId(const juce::String& linkUid,
                                     int* matchesOut = nullptr) const;
    bool linkUidLive(const juce::String& uid) const;
    // Capability, not version. False for every Link that does not claim it.
    bool linkUidDialCapable(const juce::String& uid) const;
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
            // Built-in EchoJay devices carry their own cyan badge so they read
            // as part of the plugin, not as something that was scanned.
            bool isBuiltin = ChainHost::isBuiltinDescription(items[row]);
            bool isAU = items[row].pluginFormatName == "AudioUnit";
            juce::Colour tagCol = isBuiltin ? juce::Colour(0xff06b6d4)
                                : isAU      ? juce::Colour(0xff3a7a3a)
                                            : juce::Colour(0xff2a4d7a);
            juce::String tag    = isBuiltin ? "EJ" : (isAU ? "AU" : "VST3");
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
    // chainPluginList and chainSearchBox DELETED (13 Aug 2026, dead-layer
    // sweep): the never-shown inline picker. chainListModel survives - the
    // picker POPUP consumes it.
    // chainScanBtn DELETED (13 Aug 2026): addChildComponent'd, never made
    // visible, and its five weeks of invisibility hid the chain-scan
    // trigger gap. Scan Now (showScanMenu) now runs both scans.

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
    // onFetchError (14 Aug 2026, recall): optional hook invoked when the
    // fetch or parse fails, with the HTTP status and the mapped message.
    // The sidebar status line still shows the error either way; the recall
    // path uses the hook for its own chat-visible message and log line.
    // onSlotsParsed: optional hook invoked once the record's slots array
    // validated, with the INTENDED plugin names in saved order. The recall
    // path needs the order for the alternatives offer's surviving-neighbour
    // anchors; nothing else exposes it after the fetch callback ends.
    void openSavedChain(const juce::String& id, const juce::String& name,
                        std::function<void(int statusCode, const juce::String& err)> onFetchError = {},
                        std::function<void(const juce::StringArray& intendedNames)> onSlotsParsed = {});
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
    // chainStatusLabel DELETED (13 Aug 2026): same corpse as chainScanBtn,
    // invisible since birth, and it swallowed the staleness warning one
    // commit after the button swallowed the scan trigger.
    // chainListInfoLabel DELETED (14 Aug 2026): the scan count + date it
    // carried lives in the EJScan log lines; its 150px header slot now
    // holds the New chat button, reachable from every tab (the sidebar's
    // + New chat only exists on the Chat surface).
    juce::TextButton headerNewChatBtn { "+ New chat" };
    // chainLoadBtn, chainRecommendLabel and chainDebugJsonBox DELETED
    // (13 Aug 2026, dead-layer sweep). The resolver coverage triple the
    // label carried now logs from buildRecommendable itself (EJScan:
    // resolver rebuilt), where a release build can see it.
    // Restricts the list to plugins loadable in this wrapper format.
    juce::String chainFormatFilter_;
    // Scan sequencing (13 Aug 2026): the header's Scan Now runs the
    // validating PluginScanner, and the chain scan (ChainHost) rides its
    // COMPLETION, not alongside it, because the chain scan's VST3 rows read
    // the validated cache and running first would read it stale. Set by the
    // Scan Now / folder-change handlers, consumed by the timer's falling-
    // edge watch. Dies with the editor if closed mid-scan, which only costs
    // the ride-along; the next Scan Now queues it again.
    bool chainScanAfterSettings_ = false;
    bool prevSettingsScanning_   = false;
    // Chain-scan falling edge (16 Aug 2026): a Rescan press ran the scan
    // and rewrote the cache, and the feed stayed as it was until a tab
    // switch happened to rebuild it (measured 2m45s on 15 Aug). The timer
    // watches ChainHost::isScanning go false and rebuilds the feed THEN,
    // and tells the user "rescanned, N plugins" where they can see it.
    bool prevChainScanning_      = false;

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
                using Card = EchoJayLookAndFeel::ChainCard;
                auto style = [](juce::TextButton& b, juce::Colour fg) {
                    b.setColour(juce::TextButton::buttonColourId, Card::ctrlFill);
                    b.setColour(juce::TextButton::textColourOffId, fg);
                };
                style(bypassBtn, Card::ctrlText);
                style(removeBtn, Card::ctrlDanger);
                style(prevBtn,   Card::ctrlText);
                style(nextBtn,   Card::ctrlText);
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
                // Consumes EchoJayLookAndFeel::ChainCard, THE plugin-card
                // idiom shared with the Link mixer's rack blocks. Same
                // values as always, now named so the two cannot drift.
                using Card = EchoJayLookAndFeel::ChainCard;
                auto r = getLocalBounds().toFloat().reduced(0.5f);
                g.setColour(selected ? Card::fillSelected : Card::fill);
                g.fillRoundedRectangle(r, Card::corner);
                g.setColour(selected ? Card::edgeSelected : Card::edge);
                g.drawRoundedRectangle(r, Card::corner, selected ? 1.5f : 1.0f);

                g.setColour(bypassed ? Card::nameBypassed : Card::nameOn);
                g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                g.drawText(name, 6, 3, getWidth() - 12, 18,
                           juce::Justification::centred, true);
                if (bypassed)
                {
                    g.setColour(Card::bypAccent);
                    g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
                    g.drawText(Card::bypCaption, 6, 19, getWidth() - 12, 9,
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
        // Inline editor is one of OUR built-in devices: a JUCE component, not
        // a hosted plugin view. Suppresses every NativeClip interaction.
        bool inlineIsBuiltin = false;

        std::unique_ptr<ChainEditorWindow> popout;
        int popoutSlot = -1;

        /** THE RACK SELECTOR. It lives on the PANEL, not on the editor, and
            that is the fix for the bug that hid it: as a panel child its
            visibility follows the panel's automatically, so there is ONE
            authority instead of the editor's switchToTab branches and
            applyReviewModalState having to agree. It sat invisible from
            launch because only the second of those knew about it.

            It also could not stay in the window header strip, which has no
            room: the title takes 14..114, the saved-chain name field EXPANDS
            to fill everything the Save/Save As/Open buttons leave, and those
            are right-aligned from mW-186. Nothing was free. Here it sits at
            the left end of the rack strip, labelling the blocks beside it,
            mirroring the master MIX knob at the other end. */
        juce::TextButton      rackBtn { "RACK" };
        std::function<void()> onRackClick;
        static constexpr int  kRackSelW = 150;   // reserved left of the strip

        // Card header row controls — act on the SELECTED slot
        juce::TextButton cardBypassBtn { "B" };
        juce::TextButton cardRemoveBtn { "X" };
        juce::TextButton popBtn { juce::String::fromUTF8("\xe2\x86\x97") }; // expand to floating window

        // SUGGESTED SETTINGS box content — wraps, scrolls when it overflows
        juce::TextEditor settingsBox;
        juce::TooltipWindow tooltipWindow { this, 600 };

        juce::String statusText;

        /** REMOTE MODE: the panel is showing another Link's rack, read from
            its sidecar rather than from the local ChainHost. Stage 1 carries
            read, bypass, remove and add; move, per-slot wet, master wet and
            the inline editor have no op over the command protocol yet, so
            they are DISABLED WITH A REASON rather than left present and dead.
            A control that does nothing when pressed is worse than one that
            says why it cannot. */
        bool         remote = false;
        // Stage 1: a remote edit session is live for sessionSlot. The slot's
        // click falls through to showInline (the editor comes from
        // onCreateEditor, which returns the EDITING COPY's editor), and the
        // APPLY & RELEASE button shows.
        bool         editingSession = false;
        int          sessionSlot    = -1;
        juce::TextButton applyBtn { "APPLY & RELEASE" };
        std::function<void()> onApplyRelease;
        // THE AFFORDANCE (stage 1 follow-up): a remote slot is edited by
        // selecting it and pressing EDIT, not by the selection click itself.
        // A click selects quietly, like everywhere else in the product; the
        // button starts the session, so a stray click on a block can never
        // engage a lease. It replaces the old resting boundary message,
        // which explained a wall that solo editing removed.
        juce::TextButton editBtn { "EDIT THIS PLUGIN" };
        std::function<void(int)> onEditRequest;
        // Sticky status: rebuild() used to stomp statusText with the resting
        // message on every remote rebuild, which erased the edit flow's own
        // messages ("Reading settings...", refusals, timeouts) and made any
        // failure invisible. Now rebuild only replaces text IT wrote: a flow
        // message survives until the flow itself replaces it.
        juce::String restingHint_;
        /** Remote slot tapped: the editor asks that Link to raise its own.
            Slot index is the panel's (rack) index. */
        std::function<void(int)> onRemoteEditorRequest;
        juce::String remoteName;     // the Link's display name, for the note
        bool         remoteOffline = false;
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
                // Ours already fills the display area; re-running the native
                // measure/attach for it would be pointless and would create a
                // clip container around a component that has no plugin view.
                if (inlineIsBuiltin) { layoutInline(); return; }
                int w = 0, h = 0;
                if (NativeClip::getPluginViewSize(this, w, h) && w > 100 && h > 60)
                { realW = w; realH = h; }
                layoutInline();
                attachNative(false);
            };

            editBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff0e7490));
            editBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            editBtn.setTooltip("Open this plugin here and hear the channel solo, "
                               "live, while you adjust it");
            editBtn.onClick = [this]
            { if (selectedIdx >= 0 && onEditRequest) onEditRequest(selectedIdx); };
            addChildComponent(editBtn);

            applyBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1d4ed8));
            applyBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            applyBtn.setTooltip("Send the edited settings back to the Link and "
                                "restore the channel");
            applyBtn.onClick = [this] { if (onApplyRelease) onApplyRelease(); };
            addChildComponent(applyBtn);

            rackBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff141626));
            rackBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
            rackBtn.setTooltip("Choose which Link's rack this tab shows. "
                               "The Link mixer follows the same selection.");
            rackBtn.onClick = [this] { if (onRackClick) onRackClick(); };
            addAndMakeVisible(rackBtn);

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
                // Only detach a container we actually attached: a built-in
                // never created one.
                if (!inlineIsBuiltin) NativeClip::detach(this);
            }
            inlineHolder.setVisible(false);
            inlineSlot = -1;
            realW = realH = 0;
            settled = false;
            inlineIsBuiltin = false;
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

            // ---- EchoJay's own built-in devices --------------------------
            // Our EQ's editor is a plain juce::Component living in THIS
            // process. None of the NativeClip machinery below applies: there
            // is no foreign NSView to reparent, measure or clip.
            //
            // It also cannot be left to the normal path. getPluginViewSize
            // looks for a hosted plugin view and finds none for a JUCE
            // component, so the poll would run its full ~5s, then mark our own
            // EQ "popout only" and float it — a limitation of nothing,
            // recorded as a limitation of the plugin.
            if (slotInfos[(size_t)i].format == ChainHost::kBuiltinFormat)
            {
                juce::AudioProcessorEditor* ed = nullptr;
                try { ed = onCreateEditor(i); } catch (...) {}
                if (!ed) { statusText = "Failed: could not open editor"; repaint(); return; }

                statusText.clear();
                inlineEditor.reset(ed);
                inlineSlot      = i;
                inlineIsBuiltin = true;
                settled         = true;    // nothing to poll, nothing to wait for
                inlineHolder.setVisible(true);
                inlineHolder.addAndMakeVisible(*inlineEditor);
                layoutInline();            // fills the display area (see below)
                repaint();
                return;
            }

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
            if (inlineIsBuiltin) return;   // no foreign view to clip
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

            // A built-in editor is ours and resizes gracefully, so give it the
            // whole display area rather than centring a fixed native size.
            if (inlineIsBuiltin)
            {
                layoutGuard = true;
                inlineEditor->setBounds(0, 0, area.getWidth(), area.getHeight());
                layoutGuard = false;
                return;
            }

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
            // A REMOTE slot never tries to open an editor here. That instance
            // lives in the Link's process and cannot render in this window,
            // which is architectural and permanent, not a failure -- routing
            // it through showInline produced "Failed: could not open editor",
            // a fault message for a boundary. Instead we ASK THE LINK to open
            // its own, which is the one arrangement where the user edits the
            // real instance in the real signal path and hears it immediately.
            if (remote)
            {
                // A live session's own slot DOES show inline: the editor is
                // the local EDITING COPY (onCreateEditor returns it). Every
                // OTHER remote slot click just SELECTS -- the EDIT button
                // starts the session. The click used to auto-begin one,
                // which coupled selection to a lease and gave the flow no
                // visible starting point.
                if (!(editingSession && i == sessionSlot))
                {
                    popBtn.setVisible(false);
                    resized();   // editBtn visibility follows the selection
                    repaint();
                    return;
                }
            }
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
                // STAGE 1 SCOPE, stated per control rather than by hiding a
                // whole row: wet and move have no op over the command
                // protocol, so on a remote rack they are disabled and their
                // tooltip says why. Bypass and remove stay live because those
                // ops exist and the mixer already sends them.
                bl->wetKnob.setVisible(!remote && !slotInfos[(size_t)i].bypassed);
                bl->onWet    = [this, ci](float v) { if (onSlotWet) onSlotWet(ci, v); };
                bl->prevBtn.setEnabled(!remote && i > 0);
                bl->nextBtn.setEnabled(!remote && i < (int)slotInfos.size() - 1);
                if (remote)
                {
                    const juce::String why = "Reordering another Link's rack is not in this "
                                             "version. Open " + remoteName + " to reorder.";
                    bl->prevBtn.setTooltip(why);
                    bl->nextBtn.setTooltip(why);
                }
                if (remoteOffline)
                {
                    const juce::String off = remoteName + " is offline. This rack is the last "
                                             "thing it published; edits are refused.";
                    bl->bypassBtn.setEnabled(false);
                    bl->removeBtn.setEnabled(false);
                    bl->bypassBtn.setTooltip(off);
                    bl->removeBtn.setTooltip(off);
                }
                stripContent.addAndMakeVisible(*bl);
                blocks.push_back(std::move(bl));
            }
            layoutStrip();
            // MASTER WET is whole-rack and has no remote op either.
            masterKnob.setVisible(!remote);
            // THE INLINE EDITOR IS THE ONE GENUINELY IMPOSSIBLE ITEM, not a
            // deferred one: the plugin instance lives in the Link's process
            // slot, so no protocol addition can render it here. Every editor
            // is closed on entering remote mode and the pop-out is hidden.
            popBtn.setVisible(!remote && selectedIdx >= 0);

            // Bring the inline editor in line with the selection
            if (remote || selectedIdx < 0)
                closeAllEditors();
            // The RESTING HINT names the affordance. Sticky: it only ever
            // replaces text this same line wrote (or emptiness), so the edit
            // flow's own messages -- refusals, sizes, timeouts, all the real
            // walls with their real reasons -- survive rebuilds and stay
            // readable until the flow itself moves on.
            if (remote && !editingSession)
            {
                const juce::String hint =
                    "Select a plugin, then EDIT THIS PLUGIN to adjust it here. "
                    "You will hear " + remoteName + " solo, live, while you edit.";
                if (statusText.isEmpty() || statusText == restingHint_)
                    statusText = hint;
                restingHint_ = hint;
            }
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
            applyBtn.setBounds(getWidth() - kCardMargin - 156, 6, 156, 22);
            applyBtn.setVisible(editingSession);
            editBtn.setBounds(getWidth() - kCardMargin - 156, 6, 156, 22);
            editBtn.setVisible(remote && !editingSession && selectedIdx >= 0);

            // Settings text sits inside its card, below the tiny caps label
            auto sb = settingsBoxRect();
            settingsBox.setBounds(sb.getX() + 8, sb.getY() + 18,
                                  sb.getWidth() - 16, sb.getHeight() - 24);

            // The selector takes the left end of the rack strip and the strip
            // gives up exactly that width, so the two cannot overlap however
            // narrow the window gets. Same shape as the master knob's
            // reservation at the right end.
            rackBtn.setBounds(8, getHeight() - kStripH + 8, kRackSelW - 16, 24);
            stripView.setBounds(kRackSelW, getHeight() - kStripH,
                                juce::jmax(50, getWidth() - kMasterW - kRackSelW),
                                kStripH);
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

    // Sidebar collapse. THE FLAG IS NOT HERE ANY MORE: it is
    // processorRef.chatSidebarCollapsed, one flag for every chat-hosting
    // surface, on the processor so it survives Logic's editor recreate and
    // persisted so it survives a session reload. It used to be an editor
    // member (chainChatCollapsed_) and silently reopened on every Link window
    // switch, which was a live bug rather than a design smell.
    //
    // The assistant show/hide control. ONE button for a global flag, living
    // in the main header rather than in any tab's chrome: a collapsed sidebar
    // is zero width, so no per-tab placement can host the control that
    // reverses it. kChatToggleW went with the chevron it sized.
    juce::TextButton chatCollapseBtn { "Hide AI" };
    // "n/15" slots-used counter — sits left of the Aa button in the AI
    // ASSISTANT header (replaces the usage counter on this tab).
    // chainSlotCountLabel DELETED (13 Aug 2026, dead-layer sweep).

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
    // Caption line height under a slot row. ONE constant, consumed by the
    // height helper and the paint loop, so they cannot drift apart.
    static constexpr int kWhyH = 13;
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
    //   shelf: input bg + 5% cyan wash, radius 12px top corners only
    // AskChipLnF (the pill) is worn by the edit-apply / edit-alt / result
    // chips and by the PILL shelf (client-authored asks). Server-authored
    // questions render as the numbered BriefCard below (16 Aug 2026).
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
    // ---- ASK surfaces (16 Aug 2026, one card): TWO shapes, decided per ask ----
    //   CARD   ONLY the [ASK BRIEF] payload carrying every question of the
    //          brief (paginated locally, ONE message sent when the set is
    //          complete or X closes it). Numbered rows, hairlines, pager + X,
    //          an escape row with "Something else" and Skip. Drawn by
    //          BriefCard below.
    //   PILLS  everything else, whoever wrote it: the server's own build
    //          confirmation ("Ready to build it?"), legacy single ASK
    //          blocks, recall confirm/cancel/here/switch, the compare scope
    //          pair, the classify short-circuit chips. A decision that acts.
    //          The horizontal AskChipLnF flow, no numbering, no counter, no
    //          Skip, no other-answer row.
    // askIsCard decides; measureAskShelf lays out the pills; the card lays
    // itself out from its own preferredHeight.
    static constexpr int kAskChipH = 27;   // pill height: 12.5px text + padding
    // Pill buttons, grown on demand (no cap). Every button is created by
    // ensureAskChipButtons with the SAME onClick (onAskChipTapped(i)), so
    // index i is the only thing that distinguishes them. The arrays below
    // are ALSO the card's tap plumbing: syncAskArraysToCard fills them from
    // the card's current page so a legacy-card row tap and the general-chain
    // row ride the same onAskChipTapped path a pill does.
    std::vector<std::unique_ptr<juce::TextButton>> askChipBtns;
    std::vector<juce::String> askChipLabels;
    std::vector<juce::String> askChipIntents;   // ""|"edit"|"build" (3-pre)
    // The full choice var per row (14 Aug 2026, recall). Rows that need
    // more than label+intent (recall_id, recall_name) read it from here;
    // index-parallel with the vectors above by construction.
    std::vector<juce::var> askChipVars;
    void ensureAskChipButtons(int n);
    void onAskChipTapped(int i);
    static bool askIsCard(const ChatMsg& msg);

    // The wire contract for the whole-brief payload (shared with the server
    // session, 16 Aug 2026; do not vary without saying so):
    //   [ASK BRIEF channel="Lead Vocal"]
    //   1. axis=role  text="What is the vocal doing in the track?"
    //      options="Lead, needs to sit forward and cut" | "Melodic, ..." | ...
    //   2. axis=tuning  text="..."
    //      options="..." | "..."
    // and, once the card is complete, ONE user message:
    //   Brief answers (Lead Vocal)
    //   role: Lead, needs to sit forward and cut
    //   tuning: Skip
    //   space: <typed> something big and washy
    // (chosen option = label verbatim; skipped = the literal Skip; typed =
    // "<typed> " + the words; keyed by AXIS ID; an axis the user never
    // reached before X is OMITTED, and the server logs it as dismissed, so
    // a dismissal never counts as a Skip.) "Just build me a general chain"
    // sends alone and immediately, in the old tap format.
    // extractAskBriefBlock parses the payload out of a reply into the SAME
    // askData JSON every existing consumer reads (brief:true, channel,
    // questions[], plus top-level question/choices = question 1).
    static bool extractAskBriefBlock(juce::String& replyInOut, juce::String& askJsonOut);
    static juce::String kBriefSkipToken() { return "Skip"; }

    struct BriefCard : juce::Component
    {
        struct Q
        {
            juce::String axis, text;
            juce::StringArray labels, intents;
            juce::Array<juce::var> vars;
        };
        enum class Kind { None, Option, Skip, Typed };
        std::vector<Q> qs;
        std::vector<Kind> kinds;            // per question
        std::vector<juce::String> answers;  // per question: label / typed words
        juce::String channel;
        bool briefMode = true;              // always: the card exists for [ASK BRIEF] only
        int page = 0;
        int hoverRow = -1;                  // option row under the mouse
        int activeRow = -1;                 // keyboard-highlighted option row
        bool composerFocused = false;       // mirrored from the composer by the timer
        enum Hit { HitNone, HitOption, HitPrev, HitNext, HitClose, HitOther, HitSkip };
        std::function<void(int qi, int oi)> onOption;
        std::function<void(int qi)>         onSkip;
        std::function<void()>               onClose, onOther;
        std::function<void(int newPage)>    onPage;
        std::function<void(juce::juce_wchar)> onTypeThrough;   // a letter while focused: to the composer
        static constexpr int kHeaderH = 34, kRowH = 32, kPadX = 14, kBottomPad = 6;
        int preferredHeight() const;
        int optionCount() const { return page >= 0 && page < (int) qs.size() ? qs[(size_t) page].labels.size() : 0; }
        bool complete() const;              // every question has a value
        int firstOpenPage(int from) const;  // next page without a value, or -1
        void reset(std::vector<Q> qsIn, const juce::String& channelIn, bool briefModeIn);
        void goTo(int p);
        void paint(juce::Graphics&) override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseDown(const juce::MouseEvent&) override;
        bool keyPressed(const juce::KeyPress&) override;
        void focusLost(FocusChangeType) override { activeRow = -1; repaint(); }
    private:
        Hit hitTest(juce::Point<int> p, int* rowOut) const;
        juce::Rectangle<int> rowRect(int i) const;
        juce::Rectangle<int> escapeRowRect() const;
        juce::Rectangle<int> prevRect() const, nextRect() const, closeRect() const, skipRect() const;
    };
    BriefCard briefCard_;
    // The chat message the card was built from (chatMessages index) and its
    // askData, so a layout pass can tell "same ask, keep the page and the
    // answers" from "a new ask, rebuild".
    juce::String briefCardAskData_;
    void syncAskArraysToCard();             // askChip arrays <- current page
    void onBriefOption(int qi, int oi);
    void onBriefSkip(int qi);
    void onBriefClose();
    void onBriefAdvance();                  // next open page or send
    void sendBriefAnswers(const juce::String& why);
    bool briefTakesTypedAnswer(const juce::String& typed);   // Enter/Send intercept
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
    juce::String askChipQuestion_;     // question of the rows currently shown
    int  askChipMsgIdx_ = -1;          // chatMessages index the rows belong to
    int  activeAskChips = 0;
    juce::Rectangle<int> askShelfRect_;
    bool askShelfVisible_ = false;      // a shelf (card OR pills) is docked
    bool askShelfIsCard_  = false;      // which of the two
    // Scan-window hold (sendChatMessage): when the first send of a session
    // arrives before the recommendable list resolves, the send waits,
    // bounded, rather than riding feed-less and staging chat. 0 = no hold
    // in progress.
    juce::int64 scanHoldStartMs_ = 0;
    // Newest assistant message with an unanswered ask, -1 if none
    int findNewestUnansweredAsk() const;
    // Mark every pending ask answered (any user send supersedes the question)
    void supersedePendingAsks();
    // The ONE layout authority for the PILL shelf (height reservation + pill
    // placement); the paint pass draws only the background inside the rect
    // this produced, so the two cannot disagree. Flows the pills into rows
    // for the given shelf width (no cap); rects are relative to the shelf
    // origin. Returns total shelf height (0 = no shelf). Card asks are not
    // measured here (BriefCard::preferredHeight).
    int measureAskShelf(const ChatMsg& msg, int shelfW,
                        std::vector<juce::Rectangle<int>>* chipRectsOut = nullptr,
                        juce::StringArray* labelsOut = nullptr,
                        juce::String* questionOut = nullptr,
                        juce::StringArray* intentsOut = nullptr,
                        juce::Array<juce::var>* choiceVarsOut = nullptr);

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
    /** THE ONE PREDICATE for a gain card's actionability, consumed by the
        PAINT (deciding whether an Apply button exists at all) and by
        applyGainProposal (validating the press) so creation and validation
        cannot disagree: the exact two-authorities shape that produced the
        dead button. A button exists only when pressing it changes
        something, and both sides now ask the same question. */
    struct GainCardVerdict {
        bool  present = false;      // target resolves to a live address
        bool  insertPoint = false;  // channel/unset placement: the reading is
                                    // pre-fader, so the card CARRIES A CAVEAT
                                    // (it no longer refuses: an insert-point
                                    // match is legitimate gain-staging when
                                    // it says what it is). DERIVED FROM
                                    // PLACEMENT ONLY: the old gate keyed on
                                    // the model's own faderDependent flag, a
                                    // model-controlled input that made two
                                    // identical situations disagree.
        bool  noMove  = false;      // clamped target == current gain
        bool  isBus   = false;      // the MIX BUS sentinel
        float rawG = 0.0f, propG = 0.0f, curG = 0.0f;
        juce::String uid;
        bool actionable() const { return present && !noMove; }
    };
    GainCardVerdict gainCardVerdict(juce::DynamicObject* po) const;
    // Build the LINK LEVELS context + proposal format/grounding instructions
    // for a chat turn; empty when there are no live Links to reason about.
    juce::String buildLinkLevelsContext();
    // The SAME Links, structured, for /api/classify — see the .cpp.
    juce::var buildClassifyLinks() const;

    // ---- split call (classifier) ----
    // The main /api/chat send, deferred so /api/classify can run in front of
    // it. provisionalId is 0 when no provisional bubble was rendered.
    // roles/contents are a SNAPSHOT taken at compose time, not the live
    // arrays. Before the split, sendChat built its body synchronously at the
    // call site, so the history was fixed the moment the user pressed send.
    // The classifier puts up to 3.8s between those two points, and a second
    // send landing inside that window would otherwise fold its user turn
    // into THIS turn's body. juce::String is refcounted, so the copy is a
    // handful of refcount bumps rather than the payload.
    void fireChatMainCall(const juce::String& sysPrompt,
                          const juce::String& activeChatId,
                          const juce::String& turnTargetUid,
                          const juce::String& turnTargetName,
                          int provisionalId,
                          const juce::StringArray& roles,
                          const juce::StringArray& contents);
    // The reply pipeline factored VERBATIM out of fireChatMainCall's
    // completion lambda (spec step 4): extraction, salvage, gated name-scan,
    // feed check, provisional replace/drop, the four stores, workspace sync.
    // Shared by the one-shot callback and the streaming done/error handlers
    // so the two paths CANNOT diverge on what persists. Callers own the
    // SafePointer null check; this runs on the message thread only.
    void handleChatReply(const juce::String& reply, bool success,
                         const juce::String& activeChatId,
                         const juce::String& turnTargetUid,
                         const juce::String& turnTargetName,
                         int provisionalId);
    // Streaming variant of fireChatMainCall (spec step 4, Feature A). NO
    // CALL SITE until step 5 selects turn types onto it. Deltas render into
    // one provisional bubble (chatMessages only); the chain block resolves
    // at once with its Build button; done.reply persists through
    // handleChatReply; chainBlock truncated/missing renders as a FAILED
    // build, never a chatty reply. See the .cpp header comment.
    void fireChatStreamCall(const juce::String& sysPrompt,
                            const juce::String& activeChatId,
                            const juce::String& turnTargetUid,
                            const juce::String& turnTargetName,
                            int provisionalId,
                            const juce::StringArray& roles,
                            const juce::StringArray& contents);
    // Render the classifier's question as the whole turn: no main call.
    // channel_mismatch, rendered entirely client-side: the sentence and both
    // chips are built here, never by the model. See the .cpp.
    void renderChannelMismatch(const juce::String& channelName,
                               const juce::String& activeChatId);
    void renderClassifierQuestion(const juce::String& question,
                                  const juce::var& chips,
                                  const juce::String& activeChatId,
                                  const juce::String& askKind = {});
    // Is the newest assistant turn the client-rendered ASK of this kind?
    // The switch guard's real question, asked exactly rather than
    // approximated by "is there any prior reply at all".
    bool newestAssistantIsClientAsk(const juce::String& kind) const;
    // Staged for the NEXT classify call: which client ASK this turn ANSWERS.
    // Set by a chip tap, consumed and cleared when the request is built.
    juce::String nextClassifyAnswers_;
    // chips -> the ASK shelf's askData, or empty when there is no shelf to
    // draw. needs_scoping nulls its chips ON PURPOSE (prose, no shelf), so
    // empty in must mean empty out.
    static juce::String askDataFromClassifyChips(const juce::String& question,
                                                 const juce::var& chips);
    // Newest assistant turn the user has actually seen, for the classifier's
    // PRIOR REPLY fact. Skips provisional bubbles.
    juce::String priorAssistantForClassify() const;
    // ---- channel switch, carrying the request (chip intent "switch") ----
    // The chooser lists every Link (membership from getLinkDisplayList so
    // unnamed ones appear; offline ones marked, not hidden) with the
    // classifier's ranked candidates first. Selecting one SWITCHES then
    // SEEDS, in that order — see switchChannelCarryingRequest, where the
    // ordering is verified at runtime rather than trusted.
    void openChannelChooser(int chipIdx);
    void switchChannelCarryingRequest(const juce::String& uid);
    // The newest USER-typed text in this conversation, verbatim (chatMessages
    // holds the pre-injection string for user turns, so no stripping).
    juce::String newestUserRequest() const;
    // Provisional-bubble lifetime. findProvisionalIdx returns -1 when it has
    // already gone; dropProvisional is idempotent.
    int  findProvisionalIdx(int provisionalId) const;
    void dropProvisional(int provisionalId);
    int  nextProvisionalId_ = 1;   // 0 means "not provisional"
    // [DETECTED KEY] block (KEY_DETECTOR_SPEC.md §4/§9, precedence §5.3):
    // built from collectKeySources(); names which source won (and which stem,
    // for a channel reading). Empty when no source has a reading.
    juce::String buildDetectedKeyContext();

    // The [SAVED CHAINS] block: names and ids from the user's saved chain
    // library, NEVER slots or state. See the definition for the data-source
    // policy (session rows, else disk cache, never a fetch) and the cap/clip
    // rules. Logs its source and drop count every turn it is considered.
    juce::String buildSavedChainsInjection();

    // ---- Saved-chain recall (14 Aug 2026) ----
    // The receiving half of the server's <<<ECHOJAY_CHAIN_RECALL>>> block.
    // Decision logic is pure and lives in EJRecall.h; these are the side
    // effects. handleChainRecall: local id validation (second, independent
    // check; the server allowlisted already but the plugin does the
    // destructive thing), then load directly on an empty rack or ask
    // through the ASK shelf on a non-empty one (AlertWindow is banned on
    // host-driven paths, see the Logic note near the Link ack consumer).
    struct SavedChainRef { juce::String id, name; };
    // Shared source policy with buildSavedChainsInjection: session rows,
    // else the disk list cache, never a network fetch. Returns the source
    // label ("server" | "sidebar-cache" | "disk-cache" | "none").
    juce::String collectSavedChainRefs(std::vector<SavedChainRef>& out);
    void handleChainRecall(const juce::String& id, const juce::String& name);
    // targetUid empty = the local rack (the original behaviour, wording and
    // all). Non-empty = the recall is destined for that LINK's rack: the
    // question names the channel, and the confirm chip carries
    // recall_target_uid/_name so the tap routes to recallLoadChainToLink.
    void presentRecallReplaceAsk(const juce::String& id, const juce::String& name,
                                 int rackSlots,
                                 const juce::String& targetUid  = {},
                                 const juce::String& targetName = {});
    // The channel-mismatch advisory ask (15 Aug 2026): ONE client-authored
    // ASK whose question is the server's heads-up text and whose chips are
    // "Load it here" / "Choose another channel" - it replaces the plain
    // advisory line PLUS the immediate replace-confirm, so the channel
    // decision comes first and the replace confirmation follows only for
    // the chosen destination (and only when that destination's rack is
    // non-empty). Client-composed: the plugin knows which channels exist,
    // the server does not; neither chip ever sends a chat turn
    // (presentCompareScopeAsk precedent).
    void presentRecallMismatchAsk(const juce::String& id, const juce::String& name,
                                  const juce::String& headsUp);
    // The chooser behind "Choose another channel": openChannelChooser's
    // menu (registry membership, orderSwitchDestinations ordering, offline
    // marked) but the pick LOADS THE SAVED CHAIN on the chosen Link instead
    // of carrying a chat turn over. Dismissing the menu leaves the ask open.
    void openRecallChannelChooser(int chipIdx);
    // recallLoadChain's Link-targeted sibling: fetch the saved chain, map
    // slots+state to chain-cmd entries (name/state/bypassed), and send
    // through the existing sendChainToLink transport. The Link resolves
    // names and applies its own disabled set; the ack poller reports.
    void recallLoadChainToLink(const juce::String& linkUid,
                               const juce::String& id, const juce::String& name);
    // Held channel-mismatch advisory for THIS turn's recall block (kind +
    // text), cleared on consumption and expired with a log line by the
    // next classify result. pendingRecallMismatchAsk_ is true while the
    // mismatch ask is unanswered, so a supersede that none of its own
    // chips initiated (they clear it first) logs as dismissed-without-
    // choosing.
    juce::String recallAdvisoryKind_, recallAdvisoryText_;
    bool pendingRecallMismatchAsk_ = false;
    // openSavedChain plus recall-specific outcome reporting: fetch errors
    // get their own message (404 = deleted elsewhere), and a 6s watchdog
    // logs the loaded/skipped summary (the restore has no single completion
    // event; the AI build path's dial-summary watchdog is the precedent).
    void recallLoadChain(const juce::String& id, const juce::String& name);
    // ONE alternatives composer for skipped/failed slots (14 Aug 2026),
    // factored out of the AI build path's inline block so the recall path
    // reuses it instead of growing a second one. intendedNames is the
    // chain's plugin names in intended order; skippedPlain the subset that
    // did not load; causeClause the honest sentence fragment between "This
    // plugin"/"These plugins" and the name list (load failure and
    // not-found/disabled are different facts and must read as such).
    // Anchors each replacement after its nearest SURVIVING prior neighbour
    // by name, never by index (stored numbering goes stale).
    void composeSkippedAltFollowUp(const juce::StringArray& intendedNames,
                                   const juce::StringArray& skippedPlain,
                                   const juce::String& causeClause,
                                   juce::String& altPromptOut,
                                   juce::String& altLabelOut);
    // ---- TIER 1 key precondition (KEY_PRECONDITION_SPEC.md §2.1) ---------
    // When a typed message genuinely needs the key (§2.2, narrow), no usable
    // reading exists, but a bus Link DOES: add an EchoJay Key Detector to
    // that Link's chain over the existing chain command path and trigger a
    // committed ANALYSE — a deliberate, logged, IDEMPOTENT client step (the
    // Link's rack sidecar is checked; a Link that already has a detector
    // gets analyse re-armed, never a second detector). Returns a feed note
    // describing what was done, "" when nothing fired. Ambiguous intent
    // fires nothing (fail safe). The Tier 2 ask (no Link anywhere) is the
    // server classifier's — not composed here.
    static bool messageNeedsKey(const juce::String& msg);
    juce::String maybeRunKeyPrecondition(const juce::String& typedMsg);
    juce::String keyTier1LastUid_;     // last Link acted on (dedupe/logging)
    juce::uint32 keyTier1SentMs_ = 0;  // don't re-fire within a minute
    // The one in-flight chat stream, when a turn is streaming (spec step 4).
    // chatLoading already serialises sends, so at most one exists. The
    // destructor cancels it: cancel() unblocks the worker's read, closes the
    // socket as the loop abandons, and suppresses every queued callback on
    // both sides of the callAsync hop (spec 2.2) — no delta can land in a
    // half-destroyed editor.
    std::shared_ptr<ChatStreamHandle> activeChatStream_;
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
    /** THE one place a published EQ curve is looked up, so geometry, paint
        and the re-measure trigger cannot disagree about whether a Link has
        one. Returns nullptr for: no cache entry, an invalid entry, a rack
        with no EQ slot, or a slot whose array is not exactly
        LinkShm::kEqCurvePoints long. Never returns an empty-but-valid
        vector, because "no curve" and "a flat curve" are different claims and
        only the caller's null check keeps them apart.

        The pointer is into the processor's rack cache and is valid only until
        the next refreshLinkRackCache; paint uses it within one call. */
    const std::vector<int16_t>* linkEqCurve(const juce::String& uid) const;
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
    // Stale-map ladder, unmapped rung: the one user-pressed pill offering a
    // dialable alternative, prompt constrained to getDialableRecommendable-
    // Names and carrying the real (version, not sonic) reason.
    void composeStaleAltFollowUp(const juce::StringArray& staleNames,
                                 juce::String& altPromptOut,
                                 juce::String& altLabelOut);
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

    // =====================================================================
    //  LINK MIXER: console-style strips (they REPLACED the old row list,
    //  deleted at step 11).
    //  One narrow vertical strip per live Link, laid out horizontally in a
    //  scrolling viewport, with the Mix Bus strip PINNED outside that
    //  viewport (left edge) so the master cannot scroll away. Up to
    //  kRegMaxSlots = 16 Link strips.
    //
    //  GEOMETRY HAS EXACTLY ONE AUTHOR: measureLinkStrips(). resized() is
    //  its only caller. Everything it computes is STORED below. paint()
    //  measures nothing and mouseDown() recomputes nothing; both index the
    //  stored rects. A horizontally scrolling, variable-count,
    //  variable-width strip set is the highest-risk shape for the
    //  two-authorities bug that produced four failures in two days, so this
    //  is not a style preference. See CHAIN_AI_BUILD_SPEC.md, "ONE AUTHOR
    //  FOR BOUNDS, VISIBILITY AND HIT REGIONS".
    // =====================================================================
    struct StripGeom
    {
        juce::String addr;              // Link uid; EMPTY on the Mix Bus strip
        bool isBus = false;
        // TWO COORDINATE SPACES, stated because mixing them is a hit-test
        // bug waiting to happen: for a Link strip every rect below is in
        // linkMixerView_ LOCAL coords (it scrolls); for the Mix Bus strip
        // they are in EDITOR coords (it is painted directly, pinned).
        juce::Rectangle<int> full;      // whole strip body
        juce::Rectangle<int> name;      // resolveLinkDisplayName()
        juce::Rectangle<int> badge;     // BUS / CHANNEL / SET? chip
        juce::Rectangle<int> active;    // merged Active tick + connectivity
        juce::Rectangle<int> data;      // numbers / chain (the content toggle)
        // The fader+meter BAND (8b): one horizontal band, console style, the
        // meter as PERMANENT chrome. Both rects are stored by layOutStrips
        // and neither paint nor hit-testing derives one from the other.
        juce::Rectangle<int> fader;     // fader LANE: full band height, so
                                        // lane and meter share height and
                                        // baseline (the drag target too)
        juce::Rectangle<int> faderImg;  // the CAP AREA: FULL lane height,
                                        // right of the tick lane. The cap
                                        // travels all of it; the dB mapping
                                        // lives on THIS rect
        juce::Rectangle<int> meter;     // fast-peak bars, always present
        juce::Rectangle<int> clip;      // latching clip lamp atop the meter
        // The EQ box, BELOW the data area and directly above the fader+meter
        // band, in both content modes. PRESENT ON EVERY STRIP whether or not
        // the rack has an EQ, and empty only when the data area is too short
        // to give it a usable height.
        //
        // That reverses the earlier "no curve, no rect" rule and the GRID is
        // what makes the reversal honest: grid lines read as chrome, so an
        // empty box with a grid and no curve says "nothing here yet", where a
        // bare box would be ambiguous and a flat line at 0 dB would be a
        // positive claim that the EQ is doing nothing. Placeholder and
        // populated are the same object in two states, so the layout no
        // longer has to know which racks have an EQ.
        juce::Rectangle<int> eq;
    };

    struct LinkMixerView : juce::Component, juce::TooltipClient
    {
        EchoJayEditor* owner = nullptr;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        /** Vertical wheel over a scrollable rack list scrolls THAT list;
            anything else falls through to the Viewport, which scrolls the
            mixer horizontally (JUCE maps a vertical wheel onto a
            single-axis viewport). */
        void mouseWheelMove(const juce::MouseEvent& e,
                            const juce::MouseWheelDetails& w) override;
        /** Position-dependent: narrow strips ellipsise real track names, so
            the full name (and the merged control's status words, which have
            no room to render at 46px) must be reachable somewhere. Served by
            the editor's existing TooltipWindow; the text comes from
            linkStripTooltip, which consumes the SAME stored rects as paint
            and the mouse handlers. */
        juce::String getTooltip() override;
        // Fader drag, the old row list's shape verbatim: which addr is being
        // dragged, the live value (wins over pending/acked in paint), and a
        // ~10Hz send throttle; the final value always goes on mouseUp.
        // lastDragY makes the drag INCREMENTAL, re-anchored on every event:
        // each move applies (lastDragY - y) * rate from the CURRENT value,
        // never recomputing from the drag origin, so pressing or releasing
        // shift mid-drag changes only how fast the NEXT pixel moves and can
        // never jump the cap.
        juce::String  dragAddr;
        float         dragValue = 0.0f;
        int           lastDragY = 0;
        uint32_t      lastGainSendMs = 0;
    };
    LinkMixerView  linkMixerView_;
    juce::Viewport linkMixerViewport_;

    // Strip widths. Narrow is the reference console look: thin strips, many
    // visible. Wide trades count for legibility.
    // 54, not 46, since 8c: 18px could not READ as a meter (two bars, no
    // numbers), and all 16 registry slots at 54+6 still fit the default
    // window's band without scrolling (16*60-6 = 954), so the width costs
    // nothing that matters. Narrow band: fader 16 + gap 4 + meter 26.
    static constexpr int kStripWNarrow = 54;
    static constexpr int kStripWWide   = 96;
    static constexpr int kStripGap     = 6;
    int stripWidth() const
    { return processorRef.linkMixerWide ? kStripWWide : kStripWNarrow; }

    // Link tab vertical layout. ONE named place for every constant, because
    // paintLinkView used to hardcode these while resized() re-added them as
    // `topH + 16 + 26 + 64 + 6` under a comment saying it "mirrors the panel
    // painter's layout constants". That is the smell this rebuild removes.
    static constexpr int kLinkPad     = 32;   // left/right margin
    static constexpr int kLinkTopPad  = 16;
    static constexpr int kLinkTitleH  = 26;   // "LINK MONITOR"
    static constexpr int kLinkCtrlH   = 30;   // view controls row
    // Strip element heights, top to bottom.
    static constexpr int kStripNameH  = 16;
    static constexpr int kStripBadgeH = 17;
    static constexpr int kStripActH   = 20;
    static constexpr int kStripVGap   = 6;
    static constexpr int kStripDataHMin = 120;
    // ---- The EQ curve slot -------------------------------------------------
    // Taken from the BOTTOM OF THE DATA AREA: below the readings (or the chain
    // blocks) and directly above the fader+meter band, which is where the
    // spare space actually sits. It comes out of the data area rather than out
    // of the strip's shared vertical budget, and that is the whole reason the
    // band still lines up across a mixer where only some Links carry an EQ.
    // The band's height is solved ONCE for every strip; if the slot came out
    // of that shared budget, a strip with an EQ would sit its fader lower than
    // its neighbour and the console would read as broken. Only the data area
    // pays, and it is a list that drops its last rows gracefully.
    //
    // WIDTH-DEPENDENT HEIGHT, because the spare space is width dependent. The
    // readings stack at 26px per cell narrow and 18px wide, six cells max, and
    // whole cells only. At a 540px band the data area is 181px: wide fills
    // 108 of that and genuinely leaves a gap, narrow fills 156 and does not.
    // So wide takes 66 (leaving 109, still all six readings) and narrow takes
    // 40 (leaving 135, five readings). Narrow therefore trades ONE reading for
    // the curve, and it trades away LRA, which is already the first thing the
    // numbers painter drops on its own width priority.
    // These are the PREFERRED heights, which is to say a cap. The slot takes
    // the spare space up to this and no more, so at a tall window it is the
    // number below and at a short one it is whatever is genuinely spare. That
    // is what "fill the gap" has to mean: a fixed height would either leave a
    // gap unfilled at 1044px or evict three readings at 424px.
    static constexpr int kStripEqHNarrow = 40;
    static constexpr int kStripEqHWide   = 66;
    // Below this a curve is a smear rather than a small curve, so the slot is
    // dropped WHOLE instead.
    static constexpr int kStripEqHMin = 28;
    // Floor for what is LEFT. Two readings narrow, three chain blocks wide:
    // below that the data area is not showing enough to be worth the space,
    // and the readings are the primary content. The curve fills what they do
    // not need, never the other way round.
    static constexpr int kStripEqMinData = 60;
    // DRAWN range, tighter than LinkShm::kEqCurveClampDeciDb's published plus
    // or minus 30 dB, and deliberately so. The published range would put an
    // ordinary 4 dB cut a pixel or two off the centre line, so every normal EQ
    // move would render as a flat line. Plus or minus 12 dB gives that same
    // cut about 11px in the 66px slot and lets a steep high-pass pin to the
    // floor, which reads correctly as "everything below here is gone".
    static constexpr float kEqDrawRangeDb = 12.0f;
    /** The slot height for a width mode. ONE place, so layout and the
        self-test cannot disagree about which constant applies. */
    static int stripEqH(bool wide) noexcept
        { return wide ? kStripEqHWide : kStripEqHNarrow; }
    // The fader+meter BAND (8b). kFaderHMax/Min now bound the BAND height
    // (they used to bound the lone fader box; the formula is unchanged).
    // Inside the band: a fixed-width fader column on the left (ticks lane +
    // aspect-locked image), the meter taking the remainder on the right.
    // THE SPLIT IS DRIVEN BY THE FADER COLUMN CONSTANT, then the 1:8 aspect
    // lock sets the image height FROM the column width; the meter never
    // negotiates. When the band cannot hold the column plus a minimum meter,
    // THE METER WINS (a mixer cannot do without its meter): the fader column
    // shrinks, and below usefulness it drops entirely (empty rect).
    // Unreachable at shipping widths; defined and tested anyway.
    static constexpr int kFaderHMax = 240;
    static constexpr int kFaderHMin = 144;
    // Rebalanced (visual pass 3): the fader column takes the larger share
    // of the band because column width IS fader height (1:8) and therefore
    // throw and resolution; the meter takes the remainder and keeps its
    // legibility through the gutter, not through fat bars.
    // Wide is a MODEST step up from narrow, not a different instrument:
    // the surplus width becomes breathing room around a centred pair.
    //   narrow 46 inner = 26 fader + 4 gap + 16 meter, 0 spare
    //   wide   88 inner = 32 fader + 4 gap + 28 meter, 24 spare (12 a side)
    static constexpr int kFaderColNarrow  = 26;  // 4 ticks lane + 22 image
    static constexpr int kFaderColWide    = 32;  // 6 ticks lane + 26 image
    static constexpr int kFaderTickLaneN  = 4;
    static constexpr int kFaderTickLaneW  = 6;
    static constexpr int kMeterWNarrow    = 16;
    static constexpr int kMeterWWide      = 28;
    static constexpr int kBandGap         = 4;   // fader column <-> meter
    static constexpr int kMeterWMin       = 12;  // meter survival floor
    // The filmstrip asset (Assets/iron_fader_60.png -> EchoJayFaderFilmstrip.h,
    // the wet-knob pattern). Frame indexing follows ChainWetKnob::paint: ONE
    // filmstrip implementation pattern, not a second.
    static constexpr int kFaderFrames = 128, kFaderFrameW = 60, kFaderFrameH = 480;
    // THE CAP SPRITE. Measured from the asset by alpha bounding box: every
    // frame is TRANSPARENT except a constant 54x113 cap at x[3..56],
    // translated vertically (frame 0's cap top at y 363, frame 127's at 3),
    // and frame 64 carries ZERO pixels of alpha outside that band. THERE IS
    // NO TRACK IN THE ARTWORK, so the track is drawn and ONE frame's cap is
    // cropped at draw time: no asset change, no regenerated header.
    //
    // This retires the full-frame draw and with it the 1:8 lock, which was
    // never the cap's aspect but the FRAME's. It capped image height at
    // eight times image width, so the cap stopped short of the lane however
    // long the lane was: the fault three passes could not reach. The cap now
    // travels the WHOLE lane, and its own 54:113 aspect is preserved by
    // deriving height from width.
    static constexpr int kFaderCapSrcX = 3, kFaderCapSrcW = 54, kFaderCapSrcH = 113;
    static constexpr int kFaderCapSrcY = 64 * kFaderFrameH + 182;   // frame 64
    // Drawn cap WIDTH, now independent of the lane it travels. While the
    // cap was a whole filmstrip frame these were the same number and size
    // was chained to travel; a sprite decouples them, so the cap can be
    // small and still run the full lane. Chosen to read as a fader cap
    // rather than a slab: 18 in the 22-wide narrow area, 20 in the 26-wide
    // one, both centred in the area with the track visible either side.
    static constexpr int kFaderCapWNarrow = 18, kFaderCapWWide = 20;
    static int faderCapW(juce::Rectangle<int> capArea)
    {
        const int w = capArea.getWidth() >= 24 ? kFaderCapWWide : kFaderCapWNarrow;
        return juce::jmax(4, juce::jmin(w, capArea.getWidth()));
    }
    /** Drawn cap height. UNIFORM scale: height derives from the DRAWN WIDTH
        by the sprite's own 113/54, so the cap is never stretched; integer
        rounding is the only deviation, under half a pixel. Always a
        DOWNSCALE, 18 and 20 against a 54-wide source. */
    static int faderCapH(juce::Rectangle<int> capArea)
    {
        return juce::jmax(4, (int)std::round((double)faderCapW(capArea)
                                             * (double)kFaderCapSrcH
                                             / (double)kFaderCapSrcW));
    }
    /** THE TRAVEL BAND, now DERIVED rather than measured off the artwork:
        the cap CENTRE runs from half a cap below the lane top to half a cap
        above the lane bottom, because a cap cannot leave its lane. The old
        12.5 / 12.7 percent frame insets retire with the full-frame draw.
        `capArea` is the full-height rect the cap travels
        (StripGeom::faderImg). ONE pair, consumed by the drag, the ticks and
        the drawn cap alike. */
    static float faderTravelTop(juce::Rectangle<int> capArea)
    { return (float)capArea.getY() + 0.5f * (float)faderCapH(capArea); }
    static float faderTravelBot(juce::Rectangle<int> capArea)
    { return (float)capArea.getBottom() - 0.5f * (float)faderCapH(capArea); }
    /** dB<->y for the fader rect, mapped across the artwork's cap travel
        band (see above). Same range (-24..+12), same 0.1 dB snap. Pure,
        shared by the drag, the travel ticks and the fallback thumb. */
    static float gainFromY(int y, juce::Rectangle<int> capArea)
    {
        const float top = faderTravelTop(capArea), bot = faderTravelBot(capArea);
        const float f = juce::jlimit(0.0f, 1.0f,
            (bot - (float)y) / juce::jmax(1.0f, bot - top));
        return juce::jlimit(-24.0f, 12.0f,
            std::round((-24.0f + f * 36.0f) * 10.0f) / 10.0f);
    }
    static int yFromGain(float db, juce::Rectangle<int> capArea)
    {
        const float top = faderTravelTop(capArea), bot = faderTravelBot(capArea);
        const float f = juce::jlimit(0.0f, 1.0f, (db + 24.0f) / 36.0f);
        return (int)std::round(bot - f * (bot - top));
    }
    /** dB per pixel of travel, derived from the SAME travel constants as
        gainFromY/yFromGain and held to them by the self-test, so fine and
        coarse drags agree about where a dB lives; the fine modifier only
        scales how far a pixel moves. Used by the incremental drag, which
        cannot consume gainFromY deltas directly: those clamp at the travel
        rails, and a FINE drag whose cursor has passed a rail while its
        value has not would stall. */
    static float gainPerPixel(juce::Rectangle<int> capArea)
    {
        return 36.0f / juce::jmax(1.0f, faderTravelBot(capArea)
                                      - faderTravelTop(capArea));
    }
    /** Shift = fine drag at one eighth speed. */
    static constexpr float kFaderFineRatio = 1.0f / 8.0f;

    // Authored by measureLinkStrips(), consumed by paintLinkView() and the
    // mouse handlers. Nothing else writes them.
    std::vector<StripGeom> linkStripGeom_;    // live Links, mixer-view coords
    StripGeom              linkBusGeom_;      // Mix Bus, EDITOR coords
    juce::Rectangle<int>   linkTitleRect_;    // editor coords
    juce::Rectangle<int>   linkCtrlRect_;     // editor coords
    juce::Rectangle<int>   linkStripAreaRect_;// editor coords, the whole band

    // ---- View controls (step 5): two segmented groups in linkCtrlRect_,
    // width (NARROW | WIDE) and content (NUMBERS | METER | CHAIN). Segmented,
    // not cycling, because a cycler hides the options you are not on, and
    // CHAIN is not guessable from a numbers view. The zones are geometry, so
    // they follow the strip rules exactly: laid out by ONE pure function,
    // stored, consumed by paint and the mouse handler, recomputed nowhere.
    // The controls own NO state: selected-segment is read back from the
    // processor's persisted modes at every paint.
    struct CtrlZone { int id = 0; juce::Rectangle<int> rect; };
    // kCtrlMeter (11) retired in 8b with the meter mode; the id is not
    // reused so an old log line can never be misread against a new control.
    static constexpr int kCtrlNarrow  = 0, kCtrlWide  = 1,
                         kCtrlNumbers = 10, kCtrlChain = 12;
    /** Pure. Segments get fixed preferred widths, shrink proportionally to a
        pressable floor if the rect is tight, and any zone that still cannot
        fit inside the rect is DROPPED rather than clipped into an overlap. */
    static void layOutLinkCtrls(juce::Rectangle<int> ctrlRect,
                                std::vector<CtrlZone>& out);
    std::vector<CtrlZone> linkCtrlZones_;     // authored by measureLinkStrips()
    void linkCtrlClicked(int id);
    /** THE strip-set geometry, as a PURE function of its inputs: it reads no
        editor state, so the self-test can call the shipping arithmetic
        instead of carrying a copy of it (the trap tools/art_parity_test fell
        into). measureLinkStrips() gathers the inputs and calls this.

        `band` is the whole strip area in EDITOR coords. busOut comes back in
        those same coords, because the Mix Bus strip is painted directly and
        pinned. linkOut comes back in linkMixerView_ LOCAL coords, x starting
        at 0, because those strips scroll. Mixing the two spaces is a
        hit-test bug, which is why they are documented at every boundary.

        It takes NO EQ input: every strip carries the curve box whether or
        not its rack has an EQ, so the layout no longer depends on the rack
        cache at all. Which strips actually DRAW a curve is a paint-time
        question about data, not a layout-time question about rects. */
    static void layOutStrips(juce::Rectangle<int> band, int stripW,
                             const std::vector<juce::String>& addrs,
                             StripGeom& busOut,
                             std::vector<StripGeom>& linkOut);

    /** Total width the scrolling child needs for `count` strips. ONE formula,
        so resized() and the viewport sizing cannot disagree about it. */
    static int stripsTotalWidth(int count, int stripW)
    { return count <= 0 ? 0 : count * (stripW + kStripGap) - kStripGap; }

    enum class StripHit { None = 0, Fader, Clip, Meter, Badge, Active, Background };
    /** HIT-TEST PRECEDENCE, in ONE place and stated in code rather than left
        to the order handlers happen to test in: fader, then meter, then the
        placement badge, then the merged Active control, then the strip
        background as the fallback that SELECTS the channel. There is no AI
        entry: that button was removed and the eq box is deliberately absent
        too, both falling through to Background. The
        FADER IS TESTED BEFORE THE METER so it wins where the two abut: a
        drag that starts a pixel into the boundary must not be swallowed by
        the meter (which itself just selects, like the background). Pure,
        and it CONSUMES sg's stored rects; nothing here recomputes a bound.
        `p` is in the same space as sg. */
    static StripHit stripHitAt(const StripGeom& sg, juce::Point<int> p);

    /** THE addr derivation: uid when the Link publishes one, legacy
        name-derived fallback otherwise. One place. The old row list carried
        this expression inline in two spots; the mixer must not grow copies. */
    static juce::String linkAddrForSlot(const EchoJayProcessor::LinkSlotInfo& s)
    {
        return s.uid.isNotEmpty() ? s.uid : LinkShm::makeSafeFilePart(s.name);
    }
    /** Entry lookup by addr, by value because getLinkDisplayList() builds its
        vector per call. Returns false for the bus and for vanished Links. */
    bool findLinkEntryByAddr(const juce::String& addr,
                             EchoJayProcessor::LinkDisplayEntry& out) const;
    /** The merged Active/connectivity pending lookup, addr-keyed, VERBATIM
        the old row list's semantics (any pending entry for the addr wins,
        including gain pendings). ONE copy, consumed by paint AND tooltip. */
    void linkPendingFor(const juce::String& addr, bool& pending,
                        bool& timedOut, bool& target) const;
    /** The merged control's label. ONE composition, so the wide-mode strip
        text and the tooltip cannot disagree on the words. "no resp" and
        "Active..." are the old rows' strings kept verbatim; "offline" is new,
        carrying the state the deleted connectivity dot used to show. */
    static juce::String linkActiveLabel(bool connected, bool pending, bool timedOut)
    {
        return !connected ? "offline"
             : timedOut   ? "no resp"
             : pending    ? "Active..." : "Active";
    }
    juce::String linkStripTooltip(const StripGeom& sg, juce::Point<int> p) const;

    /** THE selection predicate, pure so the self-test can hold its truth
        table. ONE selection state: `effectiveUid` is always
        effectiveChannelUid(), read fresh at every use, never cached, so the
        strip and the sidebar banner render the same fact and cannot drift.
        The bus is selected when NO channel is (main context = empty uid).
        A Link strip is selected only on a REAL uid match: the entryUid
        isNotEmpty() term is load-bearing, because without it every legacy
        strip (empty uid) would paint selected whenever the main context is
        active (empty == empty). Legacy strips are never selected. */
    static bool stripSelected(bool isBus, const juce::String& entryUid,
                              const juce::String& effectiveUid)
    {
        return isBus ? effectiveUid.isEmpty()
                     : entryUid.isNotEmpty() && entryUid == effectiveUid;
    }

    // Legacy-tap refusal flash: a tap on a strip that CANNOT be selected
    // (pre-0.5.6 Link, no uid) shows a brief coral outline instead of
    // silently doing nothing. Editor-instance state is allowed HERE, as the
    // exception that proves the rule: this is sub-second visual feedback,
    // and a Logic editor recreate erasing a 700ms flash loses nothing. The
    // SELECTION itself lives on the processor via effectiveChannelUid().
    juce::String linkLegacyFlashAddr_;
    uint32_t     linkLegacyFlashMs_ = 0;
    static constexpr uint32_t kLegacyFlashDurMs = 700;

    // Bus fader drag (the bus strip is editor-painted, so its mouse stream
    // lands here, not on LinkMixerView). Transient drag state only: the
    // VALUE lives on the processor and applies immediately per event; there
    // is no protocol, no ack, no pending display. Same incremental
    // re-anchored model as the channel faders, same shift fine ratio.
    bool busFaderDragging_ = false;
    int  busFaderLastY_    = 0;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    /** The pinned bus strip is editor-painted, so ITS wheel events arrive
        here rather than on LinkMixerView. Consumes only over the bus rack
        list when that list can scroll; everything else keeps today's
        behaviour untouched. */
    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& w) override;

    void measureLinkStrips();
    /** Paint one strip from its stored geometry. Shared by the pinned Mix Bus
        strip and every Link strip so the two cannot drift. Measures nothing.

        `entry` is the strip's resolved display-list entry, nullptr for the
        bus strip. The CALLER resolves it because getLinkDisplayList() builds
        its vector per call: sixteen strips at 20Hz must share one fetch per
        frame, not pay one each. */
    void paintLinkStrip(juce::Graphics& g, const StripGeom& sg,
                        const EchoJayProcessor::LinkDisplayEntry* entry);
    /** The drawn track, its ticks and the cap sprite at `gDb`. ONE painter
        for the channel strips and the bus, so the two cannot drift. */
    void paintFaderLane(juce::Graphics& g, const StripGeom& sg, float gDb);
    /** Routes one press through stripHitAt. `local` is in sg's space.
        numClicks carries the double-click (fader reset to 0 dB). */
    void linkStripMouseDown(const StripGeom& sg, juce::Point<int> local,
                            int numClicks);


    /** Reaches the two pure geometry functions above for
        tools/linkmixer_test without making either of them public. */
    friend struct EchoJayLinkMixerTestAccess;

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
        // Meter-mode RMS (step 8): DE-STEPPING only, not metering. The
        // publisher already applies the 500ms EMA that IS the ballistics;
        // this display pass exists solely to hide the 10Hz publish
        // quantisation at the 20Hz paint rate (worst catch-up ~100ms, tiny
        // beside 500ms, so it cannot double-smooth into mush). PEAK is
        // deliberately NOT here: it is a publisher-held value with its own
        // 3s decay, and interpolating a hold marker would misplace it.
        float smRmsL = -100.0f, smRmsR = -100.0f;
        // Fast-peak DE-STEPPING (the stutter fix): 30Hz publish sampled at
        // 20Hz paint alternates one- and two-step deltas, a 10Hz beat that
        // reads as stutter. Alpha 0.75 catches a step in ~1.5 frames
        // (75ms, worst lag ~1 dB against a 13.3 dB/s release): it hides
        // the sampling quantisation and adds no ballistics, the same
        // argument smRms carries. The CLIP LATCH keeps reading the RAW
        // published value: smoothing may shape motion, never truth.
        float smFastL = -100.0f, smFastR = -100.0f;
        // Clip latches (8c): set at paint when the published fast peak
        // reaches 0 dBFS, cleared by clicking the lamp. Editor-instance
        // state by the same exception as the refusal flash: transient
        // display state, where a Logic editor recreate clearing it is a
        // nuisance, not a lie. The measurement itself is the publisher's.
        bool clipL = false, clipR = false;
    };

    // ---- Step 6: the NUMBERS content mode ----------------------------------
    /** Frame ingest + staleness for one Link strip, the old row list's
        semantics verbatim (addr-keyed state, seq-advance freshness, 1s
        staleness window). Sets `fresh` and `dim` for the caller. */
    void ingestLinkStripFrame(const juce::String& addr, int regIdx,
                              bool slotActive, uint32_t nowMs,
                              bool& fresh, float& dim);
    /** Bus ingest from the local MeterEngine into linkHostStrip_, the old
        pinned-card mapping verbatim (including host audio-liveness detection
        via lastHostMom_/lastHostRms_). Returns fresh (= not silent). */
    bool ingestBusStripFrame(uint32_t nowMs);
    /** ONE smoothing authority for the loudness-suite readouts. Advances st
        toward its frame when fresh; reports PSR/PLR validity. Called exactly
        once per strip per paint, from paintLinkStrip; renderers consume. */
    struct LinkStripDerived { bool psrValid = false, plrValid = false; };
    static LinkStripDerived advanceLinkStripSmoothing(LinkStripState& st, bool fresh);
    /** The numbers renderer: loudness-suite cells stacked vertically in the
        strip's data area. Cells that do not fit the height are DROPPED whole
        (LRA first, then PLR), never crammed or half-drawn.

        `d` comes from the caller's single advanceLinkStripSmoothing call:
        since 8b the numbers area and the meter band are LIVE AT ONCE, so the
        advance moved up into paintLinkStrip, exactly the double-advance the
        helper's comment warned about. Renderers consume, never advance. */
    void paintLinkStripNumbers(juce::Graphics& g, juce::Rectangle<int> area,
                               LinkStripState& st, const LinkStripDerived& d,
                               float dim, bool wide);

    // ---- Step 8: the METER content mode ------------------------------------
    // Console-style: stereo RMS bars plus a publisher-held peak tick. The
    // BALLISTICS ARE THE PUBLISHER'S: RMS is its 500ms EMA, peak its
    // 3s-decay max-hold, both computed per audio block on the Link's side,
    // which is why the 10Hz publish loses no transients. The receiver adds
    // nothing but de-stepping (see LinkStripState::smRmsL) and never fakes a
    // faster peak fall: the held value renders where the publisher put it.
    /** THE dB-to-y mapping for the meter area, pure and shared by the bars,
        the peak ticks and the scale marks, so the three cannot disagree
        about where a decibel lives. NON-LINEAR since 8c, Logic style: the
        top 24 dB take 70% of the height, the floor..-24 the remaining 30%.
        Clamped both ends. */
    static int meterYForDb(float db, juce::Rectangle<int> area)
    {
        db = juce::jlimit(kMeterDbFloor, 0.0f, db);
        const float f = db >= -24.0f
            ? 0.30f + 0.70f * ((db + 24.0f) / 24.0f)
            : 0.30f * ((db - kMeterDbFloor) / (-24.0f - kMeterDbFloor));
        return area.getBottom() - (int)std::round(f * (float)area.getHeight());
    }
    static constexpr float kMeterDbFloor = -60.0f;
    static constexpr int   kMeterMarkCount = 15;
    static constexpr float kMeterMarks[kMeterMarkCount] = {
        0.0f, -3.0f, -6.0f, -9.0f, -12.0f, -15.0f, -18.0f, -21.0f, -24.0f,
        -30.0f, -35.0f, -40.0f, -45.0f, -50.0f, -60.0f };
    /** THE bar-column layout: gutter width and the two bar rects for a
        meter area. ONE pure source consumed by the bar painter AND the
        clip-lamp painter, so the lamp boxes and the bars share x and width
        per channel BY CONSTRUCTION and cannot drift again. */
    static void meterBarRects(juce::Rectangle<int> area,
                              int& gutterOut, juce::Rectangle<int> barsOut[2])
    {
        // 26, not 38: the wide meter came down to 28 and the gutter is
        // what scale legibility actually needs, so full numbering survives
        // on 5px bars. Width off the meter buys fader throw; the gutter
        // keeps the reading.
        gutterOut = area.getWidth() >= 26 ? 16
                  : area.getWidth() >= 24 ? 11 : 6;
        auto bars = area.withTrimmedTop(2).withTrimmedBottom(2)
                        .withTrimmedLeft(gutterOut);
        const int barW = juce::jmax(2, (bars.getWidth() - 2) / 2);
        barsOut[0] = { bars.getX(),            bars.getY(), barW, bars.getHeight() };
        barsOut[1] = { bars.getX() + barW + 2, bars.getY(), barW, bars.getHeight() };
    }
    void paintLinkStripMeter(juce::Graphics& g, juce::Rectangle<int> area,
                             juce::Rectangle<int> clipRect,
                             LinkStripState& st, float dim, bool wide);

    // ---- Step 9: the CHAIN content mode ------------------------------------
    /** The honesty matrix, pure and testable: a missing or unreadable
        sidecar is NO DATA, a parsed sidecar with zero slots is an EMPTY
        rack, and the two must never look the same. */
    enum class ChainDisplayState { NoData, Empty, List };
    static ChainDisplayState chainDisplayState(bool valid, int slotCount)
    {
        return !valid          ? ChainDisplayState::NoData
             : slotCount <= 0  ? ChainDisplayState::Empty
                               : ChainDisplayState::List;
    }
    /** THE one feeder of processorRef.linkRackCache: ~1Hz from the timer
        plus a forced pass on entering CHAIN mode. Paint never reads a file
        (20Hz x 16 strips would be 320 JSON parses a second). */
    void refreshLinkRackCache(bool force);
    uint32_t lastRackCacheMs_ = 0;   // throttle stamp only; the CACHE lives
                                     // on the processor and survives recreate
    /** THE MIX BUS CURVE, which does NOT come from a sidecar. The bus strip
        is the main plugin's own rack, so its curve is read straight off the
        local ChainHost with no file and no 1Hz cache in the way.

        Refreshed on the editor's 20Hz timer, NOT from paint: computing a
        reading inside a painter is the thing the chain reader was written to
        avoid, and paint must stay a consumer. Empty means no EQ in the local
        rack (or it could not answer), which draws no slot, exactly as an
        absent sidecar curve does.

        CONSEQUENCE, stated because it is visible: the bus curve tracks an EQ
        knob at 20Hz while a channel's lags by up to ~1.3s. The two CANNOT
        disagree about the same data, because they describe different racks
        (this plugin's own versus a Link's), so the difference reads as the
        local one being live rather than as two answers to one question. */
    std::vector<int16_t> busEqCurve_;
    void refreshBusEqCurve();
    void paintLinkStripChain(juce::Graphics& g, juce::Rectangle<int> area,
                             const StripGeom& sg,
                             const EchoJayProcessor::LinkDisplayEntry* entry,
                             float dim, bool wide);
    /** The EQ box. Called for EVERY strip with a non-empty sg.eq, with or
        without data: it draws the well and grid unconditionally and adds the
        curve only when `curve` holds exactly LinkShm::kEqCurvePoints. Passing
        an empty vector is the PLACEHOLDER state, not an error.

        DISCRETE, NOT ANIMATED, and deliberately so. It takes no part in
        advanceLinkStripSmoothing and holds no state between paints: it draws
        whatever the last sidecar read carried. A curve that eased toward each
        new value would read as tracking the knob and failing, when what is
        actually happening is a thumbnail refreshing about once a second. A
        clean jump reads as what it is. */
    void paintLinkStripEq(juce::Graphics& g, juce::Rectangle<int> area,
                          const std::vector<int16_t>& curve,
                          float dim, bool wide);

    // ---- Block B/X (bypass / remove over the Link edit protocol) ----------
    // Block rects depend on the CACHED row count, which changes outside
    // resized(), so they use the computeColumns pattern rather than stored
    // rects: ONE pure formula consumed by paint AND the hit test, never two.
    // 20px blocks since the visual pass (15 was too small to read as a
    // card); the count header above them is gone, redundant once the
    // blocks themselves are legible.
    static constexpr int kChainBlockH = 20, kChainBlockGap = 3;
    // NARROW slots: the same shapes, smaller. 15px holds an 8pt name with
    // breathing room, and at a 54px strip that is ten rows in the space the
    // old "N RACK" number occupied with one.
    static constexpr int kChainBlockHNarrow = 15;
    static int chainBlockH(bool wide) noexcept
        { return wide ? kChainBlockH : kChainBlockHNarrow; }
    /** MIDDLE elision: keep the head AND the tail, drop the centre.
        Leading truncation cannot tell "FabFilter Pro-Q 3" from "Pro-C 2"
        from "Pro-L 2" -- all three read "FabFilter P" -- because the
        disambiguating part of a plugin name is almost always its TAIL. Pure
        string surgery with no font involved, so the self-test can check it
        without a graphics context; the caller does the width fitting. */
    static juce::String elideMiddle(const juce::String& s, int head, int tail);
    static constexpr int kBlockCtrlW = 14;         // B and X, each
    /** THE rack row layout: occupied blocks, then INERT empty slots filling
        whatever visible space is left, with the scroll offset ALREADY
        APPLIED to every rect. Pure, and the single author paint, hit
        testing, the tooltip and the wheel all consume, so a scrolled block
        cannot be painted in one place and tested in another (the
        Visualisation preset bug's exact shape).

        Empty slots fill the VISIBLE AREA and nothing more: that is an
        honest statement about the view. ChainHost enforces no rack ceiling
        to quote, so a fixed "capacity" number would be invented. */
    struct ChainRows {
        std::vector<juce::Rectangle<int>> rects;  // occupied first, then empties
        int occupied  = 0;   // rects[0 .. occupied-1] are real plugins
        int maxScroll = 0;   // 0 when everything fits, so the wheel passes through
    };
    static ChainRows layOutChainRows(juce::Rectangle<int> dataRect,
                                     int occupied, int scrollY, bool wide);
    /** Occupied rack size for a strip: host ChainHost for the bus, the
        processor-side sidecar cache for a Link. ONE lookup, four consumers. */
    int linkRackCount(const StripGeom& sg) const;
    int linkChainScrollFor(const StripGeom& sg) const;
    /** Returns true when the strip's rack list consumed the wheel. It only
        does so when it can actually scroll, so a list that fits lets the
        event through to the mixer's horizontal viewport. */
    bool linkChainWheel(const StripGeom& sg, juce::Point<int> local, float deltaY);
    /** B and X inside a block's right end (the space the visual pass left
        clear). 14x13 targets at the block's 15px height: small, but the
        same scale as the segmented controls' pressable floor. */
    static void blockCtrlRects(juce::Rectangle<int> block,
                               juce::Rectangle<int>& bOut,
                               juce::Rectangle<int>& xOut);
    /** Pending block edits: editor-side like linkCtrlPending_ (cosmetic
        across a Logic recreate; the ack plus the sidecar republish carry
        the truth). ONE in flight per Link: seq is epoch-seconds, so two
        sends inside a second would collide. */
    struct LinkBlockPending {
        juce::String uid;          // the Link; "" never occurs (bus is local)
        int  seq      = 0;
        int  slotIdx  = -1;        // 0-based rack slot
        bool isRemove = false;
        bool isAdd    = false;     // stage 1 Chain tab: add-by-name in flight
        juce::String addName;      // the requested plugin, for the failure text
        bool targetOn = false;     // bypass target state
        bool failed   = false;     // failed / stale / timed out
        juce::String reason;       // tooltip text for the failed state
        uint32_t sentMs = 0;
    };
    std::vector<LinkBlockPending> linkBlockPending_;
    void sendBlockEdit(const StripGeom& sg, int slotIdx, bool isRemove);
    /** THE op-send path, extracted from sendBlockEdit so the Chain tab's
        remote rack editing and the mixer's per-strip B/X are ONE sender with
        one pending model. sendBlockEdit is now a thin wrapper that resolves a
        StripGeom to a uid and calls this. A second sender would have meant a
        second staleness story, and baseSlots is the whole safety argument. */
    void sendRackEdit(const juce::String& uid, int slotIdx, bool isRemove);
    /** Stage 1's add. Same transport, same pending model, same baseSlots
        guard; the op adds BY NAME, which is why it can fail on a Link whose
        loadable plugin set differs from this one's. */
    void sendRackAdd(const juce::String& uid, const juce::String& pluginName);
    /** STAGE 1 REMOTE EDITOR. Ask a Link to raise its own editor for a rack
        slot. The instance stays where it is, in its own signal path, so the
        user hears every change instantly; this moves a window, not state and
        not audio. One additive ctrl field, additive exactly like gainDb and
        placement were. */
    /** THE BOUNDARY MESSAGE, and there is exactly one of it. A hosted
        plugin's editor lives in the process that instantiated it, so a Link's
        plugin cannot render in this window. That is architectural and
        permanent, not a fault, and "Failed: could not open editor" described
        it as a failure. This is also the window-closed answer for the remote
        open, because they are the SAME boundary: the editor exists over
        there, and only that window can show it. One message, not two
        explanations of one fact. %LINK% is the channel name from
        resolveLinkDisplayName, the same accessor every other surface uses. */
    static const juce::String kRemoteEditorBoundary;
    void sendOpenSlotEditor(const juce::String& uid, int slotIdx);
    // ---- Stage 1 SOLO editing ------------------------------------------
    void beginRemoteEditSession(const juce::String& uid, int slot0);
    void pollEditPullAck(const juce::String& uid, int slot0, int seq, int attemptsLeft);
    void editProceedWithState(const juce::String& uid, int slot0, const juce::String& b64);
    void commitAndReleaseEditSession();
    void pollEditCommitAck(const juce::String& uid, int seq, int attemptsLeft);
    void editSessionUiTeardown(const juce::String& note);
    void pollOpenSlotAck(const juce::String& uid, int seq, int attemptsLeft,
                         const juce::String& linkName);
    void pollLinkBlockAck(const juce::String& uid, int seq, int attemptsLeft);

    // ---- Chain tab: which rack is being viewed ---------------------------
    /** THE rack the Chain tab shows. NOT new state: it is the mixer's own
        channel selection, read through effectiveChannelUid(), so the two
        surfaces cannot disagree and nothing extra has to survive a Logic
        editor recreate (that selection already does, on the processor).
        Empty means the LOCAL rack, exactly as an empty channel uid means the
        main context everywhere else. */
    juce::String chainViewUid() const { return effectiveChannelUid(); }
    /** Slots for whichever rack chainViewUid() names, in the panel's own
        type. Local reads ChainHost directly; remote converts the sidecar,
        whose RackSidecarSlot carries the same five fields SlotInfo does.
        `valid` distinguishes "this rack is empty" from "no sidecar", which
        the panel must render differently. */
    struct ChainRackView {
        std::vector<ChainHost::SlotInfo> slots;
        bool valid   = false;      // false = no readable sidecar
        bool remote  = false;
        bool offline = false;
        juce::String name;         // display name of the rack's owner
        int  revision = -1;
    };
    /** THE conversion, named and static so the self-test can pin it. Both
        types carry the same five fields, but SlotInfo is a plain aggregate:
        if anyone ever reorders its members, an inline brace-initialiser would
        keep compiling and silently put the format string in the settings
        field. This is the one place that mapping is written down. */
    static ChainHost::SlotInfo slotInfoFromSidecar(const LinkShm::RackSidecarSlot& rs)
    {
        ChainHost::SlotInfo si;
        si.name     = rs.name;
        si.bypassed = rs.bypassed;
        si.settings = rs.settings;
        si.format   = rs.format;
        si.wet      = rs.wet;
        return si;
    }
    ChainRackView chainRackView() const;
    void refreshChainPanelForView(bool force);
    juce::String chainViewSig_;    // change detector for the remote refresh
    // Fix 3: the add's COMPLETION memory. The ok arm of pollLinkBlockAck
    // records the finished add here (then erases the pending); the derived
    // status line writes "Added ..." only once the sidecar cache actually
    // shows the slot, and the record ages out after a few seconds like the
    // chain-save status does. One author reads it: refreshChainPanelForView.
    struct AddDone { juce::String uid, name; uint32_t ms = 0; };
    AddDone      lastAddDone_;
    juce::String lastAddLine_;     // the author's own last write, so it can
                                   // retire text that stopped being true
    void showChainRackMenu();

    std::map<juce::String, LinkStripState> linkStripStates_;
    LinkStripState linkHostStrip_;         // the Mix Bus (this instance) row
    // Mix Bus audio liveness: the host can idle the MAIN plugin's channel
    // too — detect the local engine freezing (values unchanged ~1s) and
    // apply the same audioStale treatment as Link rows
    float    lastHostMom_ = 0.0f, lastHostRms_ = 0.0f;
    uint32_t lastHostAdvanceMs_ = 0;
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
    // settingsPluginViewport DELETED (13 Aug 2026, dead-layer sweep);
    // settingsChecklist is parentless until something readopts it.
    std::unique_ptr<PluginChecklistComponent> settingsChecklist;
    // settingsPluginSearchBox DELETED (13 Aug 2026, dead-layer sweep).
    juce::TextButton viewAllPluginsBtn { "View all" };
    juce::TextButton settingsScanBtn { "Scan Plugins" };
    juce::TextButton settingsHelpBtn { "Help & Support" };
    // Opens https://www.echojay.ai/manual — sits beside Save on the bottom
    // row (left cluster), same quiet link style as Help & Support
    juce::TextButton settingsManualBtn { "Manual" };

    // ---- Settings: WITHHELD FROM THE CHAIN LIST (16 Aug 2026, redefined 17 Aug) ----
    // A plugin is WITHHELD when the user has ticked it in Settings and no
    // route in THIS host reaches the chain feed. Not when one of its rows
    // was dropped: a plugin whose Intel-only VST3 is withheld but whose AU
    // loads is available and must not appear here. The denominator is the
    // ticked Settings rows counted by plugin name (the collapse the feed
    // uses), and it is stated on screen. One verdict per name, first that
    // applies, from the SHIPPING resolver (ChainHost::resolveByName asked
    // for AudioUnit and for VST3; the predicate is never reimplemented):
    //   available     this host's format resolves               -> absent
    //   FormatOnly    only the OTHER format resolves             -> host-dependent, its own group
    //   Crash         a route is crash-blacklisted               -> Re-enable control
    //   IntelOnly     a route is architecture-withheld            -> the one group with a remedy
    //   Vst2          only VST2 rows exist                        -> EchoJay hosts no VST2 anywhere
    //   NotScanned    a .vst3 of that name sits in a vendor       -> a scan defect (P2), named as such
    //                 subfolder the chain scan does not enter
    //   Unreadable    nothing resolves and nothing explains it    -> "could not be read as a plugin"
    // The 17 Aug census on Sean's Mac under Logic: 807 enabled names, 165
    // Intel-only, 88 VST2, 21 not scanned, 14 format-only, 519 in the feed.
    // The previous section applied the host's format filter before counting
    // and therefore read "nothing is withheld" under Logic while 331 VST3
    // rows were withheld: it said the opposite of the truth.
    struct WithheldItem
    {
        juce::String name, vendor, path;
        juce::String date;          // crash rows: from the blacklist line's ISO stamp, may be empty
        bool reenabled = false;     // crash rows: line deleted this session
    };
    struct WithheldGroup
    {
        enum Kind { Crash, IntelOnly, Vst2, NotScanned, Unreadable, FormatOnly } kind = Unreadable;
        juce::String title;         // "Intel only, no Apple Silicon build installed"
        juce::String remedy;        // one line, only where a remedy exists
        juce::String vendorsLine;   // "IK Multimedia 46, iZotope 36, ..." (IntelOnly)
        std::vector<WithheldItem> items;
    };
    std::vector<WithheldGroup> settingsWithheldGroups_;   // only non-empty groups, in the order above
    int  settingsWithheldEnabledNames_ = 0;   // the denominator: ticked plugins counted by name
    int  settingsWithheldCannot_ = 0;         // sum of the groups that cannot be used in THIS host
    bool settingsWithheldExpanded_ = false;
    juce::TextButton settingsWithheldToggleBtn_ { "Show names" };
    std::vector<std::unique_ptr<juce::TextButton>> settingsReenableBtns_;
    static constexpr int kWithheldRowH = 18;
    static constexpr int kWithheldGroupH = 20;
    // ONE geometry for resized() and paintSettingsView (the two must agree,
    // as every other Settings section's paint mirrors its layout).
    struct WithheldLayout
    {
        int labelY = 0;                     // section label (14px)
        juce::Rectangle<int> headline;      // "N of your M enabled plugins cannot be used in this host"
        juce::Rectangle<int> denominator;   // how N and M were counted
        juce::Rectangle<int> toggle;        // Show/Hide names, empty when nothing is withheld
        struct Group { int titleY = 0, remedyY = -1, vendorsY = -1, itemsY = 0; };
        std::vector<Group> groups;          // parallel to settingsWithheldGroups_
        int endY  = 0;                      // next section starts here
    };
    WithheldLayout withheldSectionLayout(int sx, int sy, int sw) const;
    void rebuildSettingsWithheld();
    // The verdict, as a pure function of its inputs so a harness can run the
    // SHIPPING classification against real caches (no copy of the rules).
    // hostFmt = "AudioUnit" | "VST3" | ""; crashByFold = crash rows from the
    // entries cache keyed by folded name; nestedVst3 = folded names of the
    // .vst3 bundles in vendor subfolders. Returns only non-empty groups.
    static std::vector<WithheldGroup> classifyWithheld(const std::vector<ScannedPlugin>& plugins,
                                                       const ChainHost& ch,
                                                       const juce::String& hostFmt,
                                                       const std::map<juce::String, WithheldItem>& crashByFold,
                                                       const std::set<juce::String>& nestedVst3,
                                                       int* enabledNamesOut, int* cannotOut);
    static juce::String foldPluginName(const juce::String& s);
    void reenableWithheldItem(int groupIdx, int itemIdx);
    static juce::File chainBlacklistFile();
    // Section 5 (17 Aug 2026): the sandbox claim, measured not assumed.
    // dev_mode "/vst3test <name>" instantiates a resolved VST3 through
    // ChainHost::asyncCreatePlugin regardless of the host format filter and
    // reports what actually happened. Changes no policy.
    void handleDevVst3Test(const juce::String& name);

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
    // Album ids + "proj:<name>" + "chan:<uid|main>" keys the user has
    // collapsed. Persisted to a global prefs file so songs stay collapsed
    // across restart / project reopen. The active chat's ancestors expand
    // as a ONE-SHOT key erase at activation (expandAncestorsOf), never as a
    // per-render override: the override made the active chain's triangles
    // dead controls whose stored keys flip-flopped with click parity.
    std::set<juce::String> collapsedAlbums;
    void loadCollapsedState();
    void saveCollapsedState() const;
    void expandAncestorsOf(const juce::String& chatId);

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
            enum class Kind { SectionTitle, AlbumHeader, ProjectHeader, ChannelHeader, ChatRow, ReviewRow };
            Kind         kind   = Kind::SectionTitle;
            bool         inflight = false;   // ChatRow: a turn is streaming into
                                             // this chat right now (dot glyph)
            juce::String id;      // AlbumHeader: album id; ProjectHeader: project
                                  // name; ChannelHeader: linkUid
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
        // Channel folder collapse toggle (collapse key "chan:<linkUid>")
        std::function<void(const juce::String& linkUid)> onChannelToggled;

        // BUSY indicator (14 Aug 2026): the chat a turn is streaming into
        // right now. Session state, never persisted, cleared when the turn
        // completes whether or not that chat was opened. Not an unread
        // marker: it says "arriving", not "arrived".
        juce::String inflightChatId;
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
    // ---- Stream ownership (14 Aug 2026: a stream belongs to a chat) ----
    // Re-establish the in-flight turn's provisional rendering (stage row or
    // partial bubble) after a chat switch, IF the newly opened chat owns the
    // stream. Set by fireChatStreamCall, self-guarding on ownership, cleared
    // at done/error. Null when no stream is in flight.
    std::function<void()> restreamRepaint_;
    // Sidebar busy dot: mark chatId as receiving a turn ("" = clear). Session
    // state on the sidebar model only; refreshes the rows.
    void setInflightChat(const juce::String& chatId);
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
