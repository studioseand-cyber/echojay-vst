#pragma once
#include <JuceHeader.h>
#include <set>
#include <map>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "NativeClip.h"
#include "EchoJayAPI.h"
#include "EchoJayLookAndFeel.h"
#include "ParticleVisual.h"
#include "PluginChecklist.h"
#include "EchoJayWorkspace.h"

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
    
    void sendChatMessage(const juce::String& msg);

    // prevReview is non-null when this is not the first capture in the chat.
    void requestAIFeedback(const CaptureSnapshot& snap,
                           const juce::String& chatId,
                           const juce::String& reviewId,
                           const juce::String& passName,
                           int version,
                           const WsReview* prevReview);
    void layoutChatMessages();
    
    void showLoginScreen();
    void showMainScreen();
    void attemptLogin();
    void handleLogout();
    bool shouldShowChannelPrompt() const;
    void updateChannelPromptVisibility();
    void selectChannelPromptType(ChannelType type);
    void dismissChannelPrompt();
    void paintChannelPromptOverlay(juce::Graphics& g, juce::Rectangle<int> bounds);
    
    void showCompareView();
    void hideCompareView();
    void loadReferenceFile();
    void runAICompare();
    void paintCompareView(juce::Graphics& g, juce::Rectangle<int> area);
    // Spectrum panel for Compare tab: independent per-panel static snapshot,
    // no shared peak-hold state (avoids top/bottom panel interference).
    void paintCompareSpectrum(juce::Graphics& g, juce::Rectangle<int> area,
                              const MeterData& md, bool isLive);
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

    // V2 six-tab shell
    enum class Tab { Visualisation, Meters, Chat, Compare, Link, Chain, Settings };
    Tab currentTab { Tab::Visualisation };
    static constexpr int kTabBarH = 28;
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

    // SPECTRUM panel view mode — false = spectrum curve (default), true =
    // scrolling spectrogram waterfall. Toggled in the panel header, persisted
    // to ~/Documents/EchoJay/spectrogram_mode.txt (same pattern as chat scale).
    bool spectrogramMode_ = false;
    void loadSpectrogramMode();
    void saveSpectrogramMode() const;
    // Persistent history image (kSpecHistFrames x 64) — blit-scrolled one
    // column per new frame, then drawn scaled into the panel rect.
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
    juce::Label loginSubtitle;
    juce::TextButton signUpBtn { "Sign Up" };
    juce::Label signUpLabel;
    bool loginLoading = false;
    
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
    juce::Label passLabel;
    
    // Top bar — right group (stacked)
    juce::Label userLabel;
    juce::Label usageLabel;
    juce::TextButton scanBtn { "Scan Plugins" };
    juce::TextButton logoutBtn { "Log Out" };
    
    // First-open channel prompt
    juce::Component channelPromptBlocker;
    juce::Label channelPromptTitle;
    juce::Label channelPromptSubtitle;
    static constexpr int kChannelPromptGroupCount = 8;
    static constexpr int kChannelPromptOptionCount = 35;
    std::array<juce::Label, kChannelPromptGroupCount> channelPromptGroupLabels;
    std::array<juce::TextButton, kChannelPromptOptionCount> channelPromptButtons;
    juce::TextButton channelPromptSkipBtn { "Mix Bus" };
    juce::TextButton customChannelBtn { "Custom..." };
    bool channelPromptVisible = false;
    
    // Session-level genre prompt — shown once per DAW session across all instances
    // Genre-prompt dismissal lives in the PROCESSOR (serialised with state);
    // the old editor static re-prompted after project reload / process recycle.
    bool genrePromptVisible = false;
    juce::Label genrePromptTitle;
    juce::Label genrePromptSubtitle;
    static constexpr int kGenreGroupCount = 4;
    static constexpr int kGenreOptionCount = 36; // built-in genres (excludes Custom button)
    std::array<juce::Label, kGenreGroupCount> genrePromptGroupLabels;
    std::array<juce::TextButton, kGenreOptionCount> genrePromptButtons;
    juce::TextButton genrePromptCustomBtn { "Custom..." };

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

    // Project-name prompt (channel/genre-style, shown ONCE when the instance
    // has no own name AND no shared session value exists) + session sharing
    bool projectPromptVisible = false;
    juce::Label projectPromptTitle, projectPromptSubtitle;
    juce::TextEditor projectPromptInput;
    juce::TextButton projectPromptOkBtn { "Continue" };
    juce::TextButton projectPromptSkipBtn { "Skip" };
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

    // ONE source of truth for "an assistant input row exists here", used by
    // the out-of-credits gate (upgrade button replaces the row). Every tab
    // with the sidebar counts — Chat, Compare, Meters, Visualisation, Link,
    // Chain (uncollapsed) — plus the chat-only compact window. A hand-listed
    // subset here is exactly how Meters/Vis/Link once kept accepting text
    // while Chat showed the upgrade prompt.
    bool assistantInputContext() const;

    // Credits counter colour from api.getCreditsWarnLevel():
    // dim (>3 usable) / amber (1-3) / coral (0, matches the input gate)
    juce::Colour creditsWarnColour() const;

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
    juce::ComboBox compareSlotABox;
    juce::ComboBox compareSlotBBox;
    juce::Label refStatusLabel;
    // Stage 1: meter-type selector (Waveform / Spectrum / Levels / Stereo Image / Loudness)
    std::array<juce::TextButton, 5> compareMeterBtns;
    int compareMeterId_ = 0; // 0=Waveform, 1=Spectrum, 2=Levels, 3=Stereo, 4=Loudness

    // Stage 2: per-slot source selection
    struct CompareSlotState {
        enum class Kind { Empty, Live, Snapshot, WsCapture, Reference };
        Kind kind = Kind::Empty;
        int  index = -1;          // Snapshot or Reference index
        juce::String wsReviewId;  // WsCapture: review ID from workspace
        juce::String label;       // display name shown in the slot button
    };
    CompareSlotState compareTop_, compareBot_;
    juce::TextButton compareTopSlotBtn_;
    juce::TextButton compareBotSlotBtn_;
    void openCompareSlotMenu(bool isTop);
    void updateCompareSlotBtn(bool isTop);
    MeterData getSlotMeterData(const CompareSlotState& slot) const;
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
    };
    std::vector<ChatMsg> chatMessages;
    bool chatLoading = false;
    
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
    juce::TextButton chatTextSizeBtn { "Aa" };
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
        static constexpr int kBlockH   = 50;
        static constexpr int kBlockGap = 26;   // connector-line gap between blocks
        static constexpr int kAddW     = 40;   // "+" block

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

            std::function<void()>    onSelect;
            std::function<void()>    onBypass;
            std::function<void()>    onRemove;
            std::function<void(int)> onMove;

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

        std::function<void(int)>      onSelectSlot;
        std::function<void(int)>      onRemoveSlot;
        std::function<void(int)>      onBypassSlot;
        std::function<void(int, int)> onMoveSlot;
        std::function<void()>         onAddClick;
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

        // Framed card layout, top to bottom: card header row (B/X + name +
        // pop-out), plugin box (the clip container, inset on all sides),
        // SUGGESTED SETTINGS box, chain strip. The plugin box IS the
        // container clip frame — containment policy is unchanged.
        juce::Rectangle<int> displayArea() const
        {
            int top    = 4 + kCardHeaderH + kCardGap;
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
            g.fillAll(juce::Colour(0xff0A0C18));

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
                g.setColour(isErr ? juce::Colour(0xffdd6666)
                                  : isLd ? juce::Colour(0xff88aadd) : juce::Colour(0xffa0a0b8));
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

            stripView.setBounds(0, getHeight() - kStripH, getWidth(), kStripH);
            updateCard();
            layoutStrip();
            layoutInline();
            attachNative(false);   // re-assert the clip frame at the new rect
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
    juce::TextButton chainChatToggleBtn { "AI >" };
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
    void showChainPluginPicker();                       // "+" button popup
    void loadChainFromJson(const juce::String& chainJson);

    // ---- Link chain send side (phase 1) ----
    // Build button target menu: "Build here" (default) + live Link
    // instances. An AI suggestedTarget matching a live Link is pre-selected
    // (ticked + "suggested" marker); the user always confirms.
    void showChainBuildTargetMenu(const juce::String& chainJson);
    void sendChainToLink(const juce::String& linkName, const juce::String& chainJson);
    void pollLinkChainAck(const juce::String& linkName, int seq, int attemptsLeft);

    // ---- LINK tab remote Active control ------------------------------------
    // Per-row toggle writes ctrl-cmd-<id>.json {v:1, seq, active}; the Link
    // applies it (authority stays with the Link) and acks; the row shows a
    // pending style until the ack, and a NO RESP state on timeout.
    struct LinkCtrlPending {
        juce::String addr;   // the Link's ADDRESS (uid; legacy name-derived fallback)
        int  seq = 0;
        bool target = false;
        bool timedOut = false;
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
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
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
    void pollLinkCtrlAck(const juce::String& linkAddr, int seq, int attemptsLeft);
    void promptForFailedPlugins(juce::StringArray failed);
    void showNextFailPrompt(juce::StringArray names, int idx);
    // Shared disable action (local + Link build failures): untick in the
    // scanner (plugin_disabled.json), refresh checklist, rebuild recommendable.
    void disablePluginByName(const juce::String& name);
    // Link build results with load_failed entries: one dialog, per-plugin
    // "don't suggest again" toggle rows (no modal chain).
    void showLinkBuildResults(const juce::String& summary, juce::StringArray loadFailed);
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
    
    EchoJayAPI api;
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
    // Debug: dumps MeterEngine::getMeterDataJSON() to ~/Documents/EchoJay/
    // meter-debug.json + appends a one-line summary to meter-debug.log
    juce::TextButton dumpMetersBtn { "Dump meters" };

    // Commits the checklists' local selections to the scanner + server. Set in
    // the constructor; invoked on review-popup Done and on Settings Save.
    std::function<void()> commitChecklistFn;

    void showPluginReview();
    void hidePluginReview();

    // ---- Chat sidebar (Phase 2a) -------------------------------------------
    static constexpr int kSidebarW = 210;

    // Which albums are collapsed (by album id). Default = all expanded.
    std::set<juce::String> collapsedAlbums;

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
            enum class Kind { SectionTitle, AlbumHeader, ChatRow, ReviewRow };
            Kind         kind   = Kind::SectionTitle;
            juce::String id;
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
                         const std::set<juce::String>& collapsed,
                         const juce::String& activeChatId);
    };
    std::unique_ptr<ChatSidebarModel> sidebarModel;
    juce::ListBox chatSidebar { {}, nullptr };

    void loadChatFromWorkspace(const juce::String& chatId);
    void createNewChat();
    void createNewAlbum();
    void showMoveToAlbumMenu(const juce::String& chatId);
    void showAlbumContextMenu(const juce::String& albumId);
    juce::String getCurrentAlbumId() const; // album containing currentChatId
    // Returns the reviewId of the created review (empty string if not logged in)
    juce::String createReviewFromCapture(const CaptureSnapshot& snap, const juce::String& wavPath);

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
