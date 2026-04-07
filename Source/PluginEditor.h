#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EchoJayAPI.h"
#include "EchoJayLookAndFeel.h"

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
    void mouseDown(const juce::MouseEvent&) override;
    
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;

private:
    void timerCallback() override;
    void textEditorReturnKeyPressed(juce::TextEditor&) override;
    
    void sendChatMessage(const juce::String& msg);
    void requestAIFeedback(const CaptureSnapshot& snap);
    void layoutChatMessages();
    
    void showLoginScreen();
    void showMainScreen();
    void attemptLogin();
    void handleLogout();
    
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
                  juce::Colour startCol, juce::Colour endCol, const juce::String& unit = "dB");
    
    EchoJayProcessor& processorRef;
    
    enum class Screen { Login, Main };
    Screen currentScreen { Screen::Login };
    
    enum class View { Meters, Compare, Settings };
    View currentView { View::Meters };
    
    bool compareVisible = false;
    bool dragHovering = false;
    
    // Login
    juce::TextEditor emailInput;
    juce::TextEditor passwordInput;
    juce::TextButton loginBtn { "Log In" };
    juce::Label loginErrorLabel;
    juce::Label loginTitle;
    juce::Label loginSubtitle;
    bool loginLoading = false;
    
    // Top bar — left group
    juce::TextButton captureBtn { "Capture" };
    juce::TextButton resetBtn { "Reset" };
    juce::TextButton compareBtn { "Compare" };
    juce::TextButton settingsBtn { "Settings" };
    juce::ComboBox channelTypeBox;
    juce::ComboBox genreBox;
    juce::Label statusLabel;
    juce::Label durationLabel;
    juce::Label detectedLabel;
    juce::Label passLabel;
    
    // Top bar — right group (stacked)
    juce::Label userLabel;
    juce::Label usageLabel;
    juce::TextButton scanBtn { "Scan Plugins" };
    juce::TextButton logoutBtn { "Log Out" };
    
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
    
    // Compare card waveform positions — stored during paint, overlays positioned in timer
    struct CompareWavePos { juce::Rectangle<int> bounds; juce::String wavPath; float duration; };
    std::vector<CompareWavePos> compareWavePositions;
    
    // Transparent click catcher for compare waveform area
    juce::Component compareClickCatcher;    static constexpr int kMaxRefRemoveBtns = 8;
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
    std::array<juce::ToggleButton, 11> dawButtons;
    juce::TextButton saveSettingsBtn { "Save" };
    juce::Label settingsSavedLabel;
    
    // Chat
    struct ChatMsg {
        juce::String role, content;
        bool hasWaveform = false;
        std::vector<WaveformRecorder::ThumbnailPoint> waveform;
        juce::String wavFilename;
        juce::String wavFilePath;
        float durationSeconds = 0;
        float lufs = -100;
    };
    std::vector<ChatMsg> chatMessages;
    bool chatLoading = false;
    juce::Viewport chatScroll;
    juce::Component chatContent;
    juce::TextEditor chatInput;
    juce::TextButton chatSendBtn { "Send" };
    
    bool pluginsSent = false;
    int scannedPluginCount = 0;
    int refreshCounter = 0;
    bool settingsFetched = false; // true once we've loaded settings from server at least once
    
    // Waveform play buttons in chat — actual button components overlaid on waveform cards
    static constexpr int kMaxWavePlayBtns = 8;
    std::array<juce::TextButton, kMaxWavePlayBtns> wavePlayOverlays;
    std::array<juce::String, kMaxWavePlayBtns> wavePlayPaths;
    std::array<float, kMaxWavePlayBtns> wavePlayDurations {};
    int activeWavePlayBtns = 0;
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
    juce::TextButton playbackBtn { "Play" };
    juce::Label wavSavedLabel;
    
    // Simple playback engine (plays back through system default output)
    bool isPlayingBack = false;
    int playbackPosition = 0;
    void startPlayback();
    void stopPlayback();
    
    EchoJayAPI api;
    juce::StringArray chatRoles, chatContents;
    
    using C = EchoJayLookAndFeel::Colours;
    EchoJayLookAndFeel lnf;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayEditor)
};
