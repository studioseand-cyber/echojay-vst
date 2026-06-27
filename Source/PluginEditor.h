#pragma once
#include <JuceHeader.h>
#include <set>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "EchoJayAPI.h"
#include "EchoJayLookAndFeel.h"
#include "ParticleVisual.h"
#include "PluginChecklist.h"
#include "EchoJayWorkspace.h"

class EchoJayEditor : public juce::AudioProcessorEditor,
                       private juce::Timer,
                       private juce::TextEditor::Listener,
                       public juce::FileDragAndDropTarget
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
    
    void showSettingsView();
    void hideSettingsView();
    void saveSettingsToServer();
    void paintSettingsView(juce::Graphics& g, juce::Rectangle<int> area);
    
    // === Section painters matching web app ===
    void paintLoudnessPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);
    void paintLevelsPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);
    void paintStereoPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);
    void paintSpectrumPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);
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
    void switchToTab(Tab t);

    bool compactMode = false;
    int fullModeWidth = 900;
    int fullModeHeight = 580;
    void toggleCompactMode();
    bool compareVisible = false;
    bool dragHovering = false;
    
    // Visual mode
    bool visualMode = true;          // true when left panel shows particle visual instead of meters
    bool visualOnlyMode = false;     // "screensaver" mode — visual fills whole window, no chat
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
    static bool genrePromptDismissedThisSession;
    bool genrePromptVisible = false;
    juce::Label genrePromptTitle;
    juce::Label genrePromptSubtitle;
    static constexpr int kGenreGroupCount = 4;
    static constexpr int kGenreOptionCount = 36; // built-in genres (excludes Custom button)
    std::array<juce::Label, kGenreGroupCount> genrePromptGroupLabels;
    std::array<juce::TextButton, kGenreOptionCount> genrePromptButtons;
    juce::TextButton genrePromptCustomBtn { "Custom..." };
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
    juce::TextButton playSlotABtn { ">" };
    juce::TextButton playSlotBBtn { ">" };
    juce::Label refStatusLabel;
    
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
    
    juce::String getCompareSlotWavPath(int selectedId);
    
    // Loudness panel bounds for click-to-reset
    juce::Rectangle<int> loudnessPanelBounds;
    
    // Compare card waveform positions — stored during paint, overlays positioned in timer
    struct CompareWavePos { juce::Rectangle<int> bounds; juce::String wavPath; float duration; };
    std::vector<CompareWavePos> compareWavePositions;
    
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
    struct ChainEditorHolder : juce::Component
    {
        std::unique_ptr<juce::AudioProcessorEditor> hosted;
        juce::String statusText;
        juce::String settingsHint;  // AI-suggested dial-in text for the selected slot

        static constexpr int kHintH = 36; // height reserved for hint banner when non-empty

        void resized() override
        {
            if (hosted)
            {
                int hintOffset = settingsHint.isNotEmpty() ? kHintH : 0;
                hosted->setBounds(0, hintOffset, getWidth(), getHeight() - hintOffset);
            }
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff111111));

            // Settings hint banner at top (when a slot with AI guidance is selected)
            if (settingsHint.isNotEmpty())
            {
                g.setColour(juce::Colour(0xff1a2a1a));
                g.fillRect(0, 0, getWidth(), kHintH);
                g.setColour(juce::Colour(0xff334433));
                g.drawHorizontalLine(kHintH - 1, 0.0f, (float)getWidth());
                g.setColour(juce::Colour(0xff88cc88));
                g.setFont(juce::Font(juce::FontOptions(10.5f)));
                g.drawText(settingsHint, 10, 0, getWidth() - 20, kHintH,
                           juce::Justification::centredLeft, true);
            }

            if (!hosted)
            {
                bool isError   = statusText.startsWith("Failed") || statusText.startsWith("Error");
                bool isLoading = statusText.startsWith("Loading");
                g.setColour(isError   ? juce::Colour(0xffdd6666)
                          : isLoading ? juce::Colour(0xff88aadd)
                                      : juce::Colour(0xff555555));
                g.setFont(juce::Font(juce::FontOptions(13.0f)));
                g.drawText(statusText.isNotEmpty() ? statusText : "Add plugins below, then click a slot",
                           getLocalBounds().reduced(16), juce::Justification::centred, true);
            }
        }

        void setHostedEditor(juce::AudioProcessorEditor* e)
        {
            if (hosted) { removeChildComponent(hosted.get()); hosted.reset(); }
            if (e)
            {
                hosted.reset(e);
                addAndMakeVisible(*hosted);
                int hintOffset = settingsHint.isNotEmpty() ? kHintH : 0;
                int pw = juce::jlimit(200, juce::jmax(200, getWidth()),  hosted->getWidth()  > 0 ? hosted->getWidth()  : 400);
                int ph = juce::jlimit(100, juce::jmax(100, getHeight() - hintOffset), hosted->getHeight() > 0 ? hosted->getHeight() : 300);
                hosted->setSize(pw, ph);
                resized();
            }
        }
    };
    ChainEditorHolder chainEditorHolder;

    // Horizontal rack strip — one tile per chain slot, full-width, at bottom
    struct ChainRackStrip : juce::Component
    {
        // One component per slot
        struct Tile : juce::Component
        {
            juce::String name;
            juce::String settings;  // AI-suggested dial-in hint
            int  slotIdx  = 0;
            bool bypassed = false;
            bool selected = false;

            juce::TextButton bypassBtn   { "B" };
            juce::TextButton removeBtn   { "X" };
            juce::TextButton leftBtn     { "<" };
            juce::TextButton rightBtn    { ">" };

            std::function<void()>    onSelect;
            std::function<void()>    onBypass;
            std::function<void()>    onRemove;
            std::function<void(int)> onMove;

            Tile()
            {
                addAndMakeVisible(bypassBtn);
                addAndMakeVisible(removeBtn);
                addAndMakeVisible(leftBtn);
                addAndMakeVisible(rightBtn);
                bypassBtn.onClick  = [this] { if (onBypass) onBypass(); };
                removeBtn.onClick  = [this] { if (onRemove) onRemove(); };
                leftBtn.onClick    = [this] { if (onMove) onMove(-1); };
                rightBtn.onClick   = [this] { if (onMove) onMove(+1); };

                // Style: small, dark
                auto style = [](juce::TextButton& b, juce::Colour bg) {
                    b.setColour(juce::TextButton::buttonColourId, bg);
                    b.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffcccccc));
                };
                style(bypassBtn,  juce::Colour(0xff2a3a2a));
                style(removeBtn,  juce::Colour(0xff3a2a2a));
                style(leftBtn,    juce::Colour(0xff262626));
                style(rightBtn,   juce::Colour(0xff262626));
            }

            void mouseDown(const juce::MouseEvent&) override
            {
                if (onSelect) onSelect();
            }

            void paint(juce::Graphics& g) override
            {
                auto r = getLocalBounds().reduced(3).toFloat();
                g.setColour(selected  ? juce::Colour(0xff1e3d1e)
                          : bypassed  ? juce::Colour(0xff252515)
                                      : juce::Colour(0xff1e1e1e));
                g.fillRoundedRectangle(r, 5.0f);
                g.setColour(selected ? juce::Colour(0xff44aa44) : juce::Colour(0xff3a3a3a));
                g.drawRoundedRectangle(r, 5.0f, 1.0f);
                // Name
                g.setColour(bypassed ? juce::Colour(0xff666666) : juce::Colour(0xffdddddd));
                g.setFont(juce::Font(juce::FontOptions(11.0f)));
                g.drawText(name, 8, 4, getWidth() - 16, 18, juce::Justification::centredLeft, true);
                if (bypassed)
                {
                    g.setColour(juce::Colour(0xffaaaa44));
                    g.setFont(juce::Font(juce::FontOptions(9.0f)));
                    g.drawText("bypassed", 8, 22, getWidth() - 16, 12, juce::Justification::centredLeft);
                }
                else if (settings.isNotEmpty())
                {
                    g.setColour(juce::Colour(0xff669966).withAlpha(0.85f));
                    g.setFont(juce::Font(juce::FontOptions(8.5f)));
                    g.drawText(settings, 8, 22, getWidth() - 16, 12,
                               juce::Justification::centredLeft, true);
                }
            }

            void resized() override
            {
                int bh = 18, bw = 22, m = 4;
                int y = getHeight() - bh - m;
                leftBtn.setBounds(m, y, bw, bh);
                bypassBtn.setBounds(m + bw + 2, y, bw, bh);
                removeBtn.setBounds(m + bw * 2 + 4, y, bw, bh);
                rightBtn.setBounds(getWidth() - bw - m, y, bw, bh);
            }
        };

        juce::Viewport viewport;
        juce::Component content;
        juce::TextButton addBtn { "+" };
        std::vector<std::unique_ptr<Tile>> tiles;
        int selectedIdx = -1;

        std::function<void(int)>      onSelectSlot;
        std::function<void(int)>      onRemoveSlot;
        std::function<void(int)>      onBypassSlot;
        std::function<void(int, int)> onMoveSlot;
        std::function<void()>         onAddClick;

        static constexpr int kTileW = 152;
        static constexpr int kAddW  = 36;

        ChainRackStrip()
        {
            addAndMakeVisible(viewport);
            viewport.setScrollBarsShown(false, true);
            viewport.setViewedComponent(&content, false);
            addBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1e2e1e));
            addBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff77bb77));
            addBtn.onClick = [this] { if (onAddClick) onAddClick(); };
            content.addAndMakeVisible(addBtn);
        }

        void rebuild(const std::vector<ChainHost::SlotInfo>& slots, int selIdx)
        {
            selectedIdx = selIdx;
            for (auto& t : tiles) content.removeChildComponent(t.get());
            tiles.clear();

            for (int i = 0; i < (int)slots.size(); ++i)
            {
                auto tile      = std::make_unique<Tile>();
                tile->name     = slots[i].name;
                tile->settings = slots[i].settings;
                tile->slotIdx  = i;
                tile->bypassed = slots[i].bypassed;
                tile->selected = (i == selIdx);

                int ci = i;
                tile->onSelect = [this, ci] { if (onSelectSlot) onSelectSlot(ci); };
                tile->onBypass = [this, ci] { if (onSelectSlot) onSelectSlot(ci);
                                              if (onBypassSlot) onBypassSlot(ci); };
                tile->onRemove = [this, ci] { if (onRemoveSlot) onRemoveSlot(ci); };
                tile->onMove   = [this, ci](int dir) { if (onMoveSlot) onMoveSlot(ci, dir); };

                content.addAndMakeVisible(*tile);
                tiles.push_back(std::move(tile));
            }
            layoutContent();
        }

        void layoutContent()
        {
            int n = (int)tiles.size();
            int totalW = n * kTileW + kAddW + 16;
            int h = viewport.getHeight() > 0 ? viewport.getHeight() : getHeight();
            content.setSize(juce::jmax(totalW, viewport.getWidth()), h);
            for (int i = 0; i < n; ++i)
                tiles[i]->setBounds(i * kTileW + 4, 4, kTileW - 8, h - 8);
            addBtn.setBounds(n * kTileW + 8, (h - 28) / 2, 28, 28);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff161616));
            g.setColour(juce::Colour(0xff303030));
            g.drawLine(0.0f, 0.0f, (float)getWidth(), 0.0f, 1.0f);
        }

        void resized() override
        {
            viewport.setBounds(getLocalBounds());
            layoutContent();
        }
    };
    ChainRackStrip chainRackStrip;
    int chainSelectedSlot_ = -1;  // which rack tile is currently selected

    // Warning overlay (shown once when CHAIN tab first opened)
    juce::Component  chainWarnOverlay;
    juce::Label      chainWarnLabel;
    juce::TextButton chainWarnOkBtn { "I understand, continue" };

    // Chat text scaling (user-adjustable via Aa button in chat header).
    // Stored as a multiplier applied to the base 12pt message font. Cycles
    // through preset steps on each click and persists across sessions.
    float chatTextScale = 1.0f;
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
    void loadChainFromJson(const juce::String& chainJson);
    void promptForFailedPlugins(juce::StringArray failed);
    void showNextFailPrompt(juce::StringArray names, int idx);
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayEditor)
};
