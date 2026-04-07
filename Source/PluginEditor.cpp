#include "PluginEditor.h"
#include <cmath>

// ============================================================================
// Constructor
// ============================================================================

EchoJayEditor::EchoJayEditor(EchoJayProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    setLookAndFeel(&lnf);
    setSize(900, 580);
    setResizable(true, true);
    setResizeLimits(630, 406, 1800, 1160); // ~0.7x to ~2x
    getConstrainer()->setFixedAspectRatio(900.0 / 580.0);

    // --- Channel type ---
    // Grouped channel type dropdown
    channelTypeBox.addItem("Full Mix", 1);
    channelTypeBox.addSeparator();
    channelTypeBox.addSectionHeading("Vocals");
    channelTypeBox.addItem("Lead Vocal", 2);
    channelTypeBox.addItem("Backing Vocal", 3);
    channelTypeBox.addItem("Adlibs", 4);
    channelTypeBox.addItem("Vocal Bus", 5);
    channelTypeBox.addSeparator();
    channelTypeBox.addSectionHeading("Drums");
    channelTypeBox.addItem("Kick", 6);
    channelTypeBox.addItem("Snare", 7);
    channelTypeBox.addItem("Hi-Hat", 8);
    channelTypeBox.addItem("Overheads", 9);
    channelTypeBox.addItem("Drum Bus", 10);
    channelTypeBox.addItem("Percussion", 11);
    channelTypeBox.addSeparator();
    channelTypeBox.addSectionHeading("Bass");
    channelTypeBox.addItem("Bass / 808", 12);
    channelTypeBox.addItem("Bass Guitar", 13);
    channelTypeBox.addItem("Sub Bass", 14);
    channelTypeBox.addItem("Synth Bass", 15);
    channelTypeBox.addSeparator();
    channelTypeBox.addSectionHeading("Keys & Guitar");
    channelTypeBox.addItem("Piano", 16);
    channelTypeBox.addItem("Keys", 17);
    channelTypeBox.addItem("Acoustic Guitar", 18);
    channelTypeBox.addItem("Electric Guitar", 19);
    channelTypeBox.addItem("Guitar Bus", 20);
    channelTypeBox.addSeparator();
    channelTypeBox.addSectionHeading("Synths");
    channelTypeBox.addItem("Synth Lead", 21);
    channelTypeBox.addItem("Synth Pad", 22);
    channelTypeBox.addItem("Synth Pluck", 23);
    channelTypeBox.addItem("Synth Bus", 24);
    channelTypeBox.addSeparator();
    channelTypeBox.addSectionHeading("Strings & Brass");
    channelTypeBox.addItem("Strings", 25);
    channelTypeBox.addItem("Brass", 26);
    channelTypeBox.addItem("Woodwind", 27);
    channelTypeBox.addItem("Orchestral", 28);
    channelTypeBox.addSeparator();
    channelTypeBox.addSectionHeading("FX & Other");
    channelTypeBox.addItem("FX", 29);
    channelTypeBox.addItem("Reverb", 30);
    channelTypeBox.addItem("Delay", 31);
    channelTypeBox.addItem("Foley", 32);
    channelTypeBox.addItem("Ambient", 33);
    channelTypeBox.addSeparator();
    channelTypeBox.addSectionHeading("Buses");
    channelTypeBox.addItem("Master Bus", 34);
    channelTypeBox.addItem("Instrument Bus", 35);
    channelTypeBox.addItem("Music Bus", 36);
    channelTypeBox.setSelectedId(static_cast<int>(processorRef.getChannelType()) + 1, juce::dontSendNotification);
    channelTypeBox.onChange = [this] {
        processorRef.setChannelType(static_cast<ChannelType>(channelTypeBox.getSelectedId() - 1));
    };
    channelTypeBox.setColour(juce::ComboBox::backgroundColourId, C::bg3);
    channelTypeBox.setColour(juce::ComboBox::textColourId, C::text);
    channelTypeBox.setColour(juce::ComboBox::outlineColourId, C::border2);
    addAndMakeVisible(channelTypeBox);

    // --- Genre ---
    juce::StringArray genres = { "Hip-Hop","Drill","Trap","Pop","R&B","EDM","House",
        "Techno","Rock","Jazz","Classical","Lo-Fi","Afrobeat","Phonk","DnB",
        "Reggaeton","Country","Metal","Ambient","Gospel","Funk","Soul" };
    genreBox.addItemList(genres, 1);
    genreBox.setSelectedId(1, juce::dontSendNotification);
    genreBox.onChange = [this] { processorRef.setGenre(genreBox.getText()); };
    genreBox.setColour(juce::ComboBox::backgroundColourId, C::bg3);
    genreBox.setColour(juce::ComboBox::textColourId, C::text);
    genreBox.setColour(juce::ComboBox::outlineColourId, C::border2);
    addAndMakeVisible(genreBox);

    // --- Capture ---
    captureBtn.setColour(juce::TextButton::buttonColourId, C::blue);
    captureBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    captureBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    captureBtn.onClick = [this] {
        auto s = processorRef.getCaptureState();
        if (s == CaptureState::Idle || s == CaptureState::Complete) processorRef.startCapture();
        else if (s == CaptureState::Capturing) processorRef.stopCapture();
    };
    addAndMakeVisible(captureBtn);

    // (Reset button removed — auto-unfreeze handles this)

    // --- Playback ---
    playbackBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    playbackBtn.setColour(juce::TextButton::textColourOnId, C::green);
    playbackBtn.setColour(juce::TextButton::textColourOffId, C::green);
    playbackBtn.setButtonText("\xe2\x96\xb6");  // ▶ play triangle
    playbackBtn.onClick = [this] {
        if (isPlayingBack) stopPlayback();
        else startPlayback();
    };
    playbackBtn.setVisible(false);
    addAndMakeVisible(playbackBtn);

    wavSavedLabel.setColour(juce::Label::textColourId, C::text3);
    wavSavedLabel.setFont(juce::Font(juce::FontOptions(9.0f)));
    wavSavedLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(wavSavedLabel);

    // --- Compare ---
    compareBtn.setColour(juce::TextButton::buttonColourId, C::purple);
    compareBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    compareBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    compareBtn.onClick = [this] {
        if (currentView == View::Compare) { hideCompareView(); currentView = View::Meters; }
        else { hideSettingsView(); currentView = View::Compare; showCompareView(); }
    };
    addAndMakeVisible(compareBtn);

    // --- Settings ---
    settingsBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    settingsBtn.setColour(juce::TextButton::textColourOnId, C::text2);
    settingsBtn.setColour(juce::TextButton::textColourOffId, C::text2);
    settingsBtn.onClick = [this] {
        if (currentView == View::Settings) { hideSettingsView(); currentView = View::Meters; }
        else { hideCompareView(); showSettingsView(); currentView = View::Settings; }
    };
    addAndMakeVisible(settingsBtn);

    // --- Right-side top bar controls ---
    scanBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    scanBtn.setColour(juce::TextButton::textColourOnId, C::purple);
    scanBtn.setColour(juce::TextButton::textColourOffId, C::purple);
    scanBtn.onClick = [this] { processorRef.getPluginScanner().startScan(); };
    addAndMakeVisible(scanBtn);

    logoutBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    logoutBtn.setColour(juce::TextButton::textColourOnId, C::text3);
    logoutBtn.setColour(juce::TextButton::textColourOffId, C::text3);
    logoutBtn.onClick = [this] { handleLogout(); };
    addAndMakeVisible(logoutBtn);

    userLabel.setColour(juce::Label::textColourId, C::text2);
    userLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    userLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(userLabel);

    usageLabel.setColour(juce::Label::textColourId, C::text3);
    usageLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    usageLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(usageLabel);

    // --- Compare view controls ---
    saveSettingsBtn.setColour(juce::TextButton::buttonColourId, C::blue);
    saveSettingsBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    saveSettingsBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    saveSettingsBtn.onClick = [this] { saveSettingsToServer(); };
    saveSettingsBtn.setVisible(false);
    addAndMakeVisible(saveSettingsBtn);

    // Settings form fields
    auto mkField = [&](juce::TextEditor& ed, const juce::String& placeholder, bool multiLine = false) {
        ed.setMultiLine(multiLine);
        ed.setTextToShowWhenEmpty(placeholder, C::text3);
        ed.setColour(juce::TextEditor::backgroundColourId, C::bg3);
        ed.setColour(juce::TextEditor::textColourId, C::text);
        ed.setColour(juce::TextEditor::outlineColourId, C::border2);
        ed.setFont(juce::Font(juce::FontOptions(13.0f)));
        if (!multiLine)
            ed.setIndents(8, 6); // left indent, top indent for vertical centering
        else
            ed.setIndents(8, 4);
        ed.setVisible(false);
        addAndMakeVisible(ed);
    };
    mkField(settingsName, "Your name");
    mkField(settingsMonitors, "e.g. PMC 6-2, Yamaha NS-10");
    mkField(settingsHeadphones, "e.g. Audeze LCD-X, Sennheiser HD600");
    mkField(settingsGenres, "e.g. Hip-Hop, R&B, Drill, Pop");
    mkField(settingsPlugins, "List your main plugins, one per line or comma-separated.", true);

    settingsExpLevel.addItem("Beginner - Just starting out", 1);
    settingsExpLevel.addItem("Intermediate - 1-3 years", 2);
    settingsExpLevel.addItem("Advanced - 3-10 years", 3);
    settingsExpLevel.addItem("Expert - 10+ years, working engineer", 4);
    settingsExpLevel.setColour(juce::ComboBox::backgroundColourId, C::bg3);
    settingsExpLevel.setColour(juce::ComboBox::textColourId, C::text);
    settingsExpLevel.setColour(juce::ComboBox::outlineColourId, C::border2);
    settingsExpLevel.setVisible(false);
    addAndMakeVisible(settingsExpLevel);

    juce::StringArray dawNames = { "Logic Pro", "Ableton Live", "FL Studio", "Pro Tools",
        "Studio One", "Cubase", "Reaper", "Reason", "Bitwig", "GarageBand", "Other" };
    for (int i = 0; i < 11; ++i)
    {
        dawButtons[i].setButtonText(dawNames[i]);
        dawButtons[i].setColour(juce::ToggleButton::textColourId, C::text2);
        dawButtons[i].setColour(juce::ToggleButton::tickColourId, C::blue);
        dawButtons[i].setVisible(false);
        addAndMakeVisible(dawButtons[i]);
    }
    settingsSavedLabel.setColour(juce::Label::textColourId, C::green);
    settingsSavedLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    settingsSavedLabel.setJustificationType(juce::Justification::centredLeft);
    settingsSavedLabel.setVisible(false);
    addAndMakeVisible(settingsSavedLabel);

    loadRefBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    loadRefBtn.setColour(juce::TextButton::textColourOnId, C::purple);
    loadRefBtn.setColour(juce::TextButton::textColourOffId, C::purple);
    loadRefBtn.onClick = [this] { loadReferenceFile(); };
    loadRefBtn.setVisible(false); // removed from UI
    // addAndMakeVisible(loadRefBtn);

    aiCompareBtn.setColour(juce::TextButton::buttonColourId, C::blue);
    aiCompareBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    aiCompareBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    aiCompareBtn.onClick = [this] { runAICompare(); };
    aiCompareBtn.setVisible(false);
    addAndMakeVisible(aiCompareBtn);

    compareSlotABox.setColour(juce::ComboBox::backgroundColourId, C::bg3);
    compareSlotABox.setColour(juce::ComboBox::textColourId, C::text);
    compareSlotABox.setColour(juce::ComboBox::outlineColourId, C::border2);
    compareSlotABox.setVisible(false);
    addAndMakeVisible(compareSlotABox);
    compareSlotBBox.setColour(juce::ComboBox::backgroundColourId, C::bg3);
    compareSlotBBox.setColour(juce::ComboBox::textColourId, C::text);
    compareSlotBBox.setColour(juce::ComboBox::outlineColourId, C::border2);
    compareSlotBBox.setVisible(false);
    addAndMakeVisible(compareSlotBBox);

    // Play buttons for each compare slot
    auto setupPlayBtn = [&](juce::TextButton& btn, juce::ComboBox& slot) {
        btn.setColour(juce::TextButton::buttonColourId, C::purple);
        btn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        btn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        btn.setVisible(false);
        btn.onClick = [this, &slot, &btn]() {
            auto wavPath = getCompareSlotWavPath(slot.getSelectedId());
            if (wavPath.isEmpty()) return;
            if (currentlyPlayingChatWav == wavPath) {
                stopChatPlayback();
                btn.setButtonText(">");
            } else {
                stopChatPlayback();
                playSlotABtn.setButtonText(">");
                playSlotBBtn.setButtonText(">");
                startChatPlayback(wavPath, 0);
                btn.setButtonText("||");
            }
        };
        addAndMakeVisible(btn);
    };
    setupPlayBtn(playSlotABtn, compareSlotABox);
    setupPlayBtn(playSlotBBtn, compareSlotBBox);

    refStatusLabel.setColour(juce::Label::textColourId, C::purple);
    refStatusLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    refStatusLabel.setJustificationType(juce::Justification::centredLeft);
    refStatusLabel.setVisible(false);
    // Removed from UI — no longer shown

    // Preset controls
    presetBox.setColour(juce::ComboBox::backgroundColourId, C::bg3);
    presetBox.setColour(juce::ComboBox::textColourId, C::text);
    presetBox.setColour(juce::ComboBox::outlineColourId, C::border2);
    presetBox.setTextWhenNothingSelected("Reference Presets...");
    presetBox.onChange = [this] {
        int sel = presetBox.getSelectedId();
        if (sel > 0 && sel <= presetNames.size())
            loadPreset(presetNames[sel - 1]);
    };
    presetBox.setVisible(false);
    addAndMakeVisible(presetBox);
    
    savePresetBtn.setColour(juce::TextButton::buttonColourId, C::purple);
    savePresetBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    savePresetBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    savePresetBtn.onClick = [this] {
        auto refs = processorRef.getReferenceAnalyser().getReferences();
        if (refs.empty()) { refStatusLabel.setText("No references to save", juce::dontSendNotification); return; }
        
        // Simple name dialog using AlertWindow
        auto* aw = new juce::AlertWindow("Save Preset", "Name this reference preset:", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor("name", processorRef.getGenre() + " References", "Preset name:");
        aw->addButton("Save", 1);
        aw->addButton("Cancel", 0);
        aw->setLookAndFeel(&lnf);
        aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int result) {
            if (result == 1) {
                auto name = aw->getTextEditorContents("name").trim();
                if (name.isNotEmpty()) {
                    saveCurrentPreset(name);
                    loadPresetList();
                    refStatusLabel.setText("Preset saved: " + name, juce::dontSendNotification);
                }
            }
            delete aw;
        }));
    };
    savePresetBtn.setVisible(false);
    addAndMakeVisible(savePresetBtn);
    
    deletePresetBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    deletePresetBtn.setColour(juce::TextButton::textColourOnId, C::red);
    deletePresetBtn.setColour(juce::TextButton::textColourOffId, C::red);
    deletePresetBtn.onClick = [this] {
        int sel = presetBox.getSelectedId();
        if (sel > 0 && sel <= presetNames.size()) {
            juce::String filePath = presetNames[sel - 1];
            juce::String displayName = juce::File(filePath).getFileNameWithoutExtension();
            auto* aw = new juce::AlertWindow("Delete Preset", 
                "Are you sure you want to delete \"" + displayName + "\"?", 
                juce::MessageBoxIconType::WarningIcon);
            aw->addButton("Delete", 1);
            aw->addButton("Cancel", 0);
            aw->setLookAndFeel(&lnf);
            aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, filePath, displayName](int result) {
                if (result == 1) {
                    deletePreset(filePath);
                    loadPresetList();
                    refStatusLabel.setText("Deleted: " + displayName, juce::dontSendNotification);
                }
                delete aw;
            }));
        }
    };
    deletePresetBtn.setVisible(false);
    addAndMakeVisible(deletePresetBtn);

    // Compare click catcher — transparent component that catches clicks on waveform bars
    compareClickCatcher.setInterceptsMouseClicks(true, false);
    compareClickCatcher.addMouseListener(this, false);
    compareClickCatcher.setVisible(false);
    addAndMakeVisible(compareClickCatcher);

    // Reference remove buttons (X on each tag in the drop zone)
    for (int i = 0; i < kMaxRefRemoveBtns; ++i)
    {
        refRemoveBtns[(size_t)i].setButtonText("x");
        refRemoveBtns[(size_t)i].setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        refRemoveBtns[(size_t)i].setColour(juce::TextButton::textColourOffId, C::text3);
        refRemoveBtns[(size_t)i].setVisible(false);
        refRemoveBtns[(size_t)i].onClick = [this, i]() {
            processorRef.getReferenceAnalyser().removeReference(i);
            if (currentView == View::Compare) showCompareView();
            repaint();
        };
        addAndMakeVisible(refRemoveBtns[(size_t)i]);
    }

    // --- Login screen components ---
    loginTitle.setText("EchoJay", juce::dontSendNotification);
    loginTitle.setColour(juce::Label::textColourId, C::blue);
    loginTitle.setFont(juce::Font(juce::FontOptions(32.0f, juce::Font::bold)));
    loginTitle.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(loginTitle);

    loginSubtitle.setText("Log in to get AI mix feedback", juce::dontSendNotification);
    loginSubtitle.setColour(juce::Label::textColourId, C::text3);
    loginSubtitle.setFont(juce::Font(juce::FontOptions(14.0f)));
    loginSubtitle.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(loginSubtitle);

    emailInput.setTextToShowWhenEmpty("Email", C::text3);
    emailInput.setColour(juce::TextEditor::backgroundColourId, C::bg3);
    emailInput.setColour(juce::TextEditor::textColourId, C::text);
    emailInput.setColour(juce::TextEditor::outlineColourId, C::border2);
    emailInput.setFont(juce::Font(juce::FontOptions(14.0f)));
    emailInput.setIndents(10, 8);
    addAndMakeVisible(emailInput);

    passwordInput.setTextToShowWhenEmpty("Password", C::text3);
    passwordInput.setPasswordCharacter('*');
    passwordInput.setColour(juce::TextEditor::backgroundColourId, C::bg3);
    passwordInput.setColour(juce::TextEditor::textColourId, C::text);
    passwordInput.setColour(juce::TextEditor::outlineColourId, C::border2);
    passwordInput.setFont(juce::Font(juce::FontOptions(14.0f)));
    passwordInput.setIndents(10, 8);
    passwordInput.onReturnKey = [this] { attemptLogin(); };
    addAndMakeVisible(passwordInput);

    loginBtn.setColour(juce::TextButton::buttonColourId, C::blue);
    loginBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    loginBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    loginBtn.onClick = [this] { attemptLogin(); };
    addAndMakeVisible(loginBtn);

    loginErrorLabel.setColour(juce::Label::textColourId, C::red);
    loginErrorLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    loginErrorLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(loginErrorLabel);

    // Check persisted login
    if (api.isLoggedIn())
    {
        currentScreen = Screen::Main;
        api.refreshUserInfo([this](bool success) {
            if (!success && !api.isLoggedIn()) { currentScreen = Screen::Login; showLoginScreen(); }
            repaint();
        });
        showMainScreen();
    }
    else
    {
        currentScreen = Screen::Login;
        showLoginScreen();
    }

    auto mkLabel = [&](juce::Label& l, juce::Colour col) {
        l.setColour(juce::Label::textColourId, col);
        l.setFont(juce::Font(juce::FontOptions(12.0f)));
        l.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(l);
    };
    mkLabel(statusLabel, C::text2);
    mkLabel(durationLabel, C::text3);
    mkLabel(detectedLabel, C::amber);
    mkLabel(passLabel, C::text3);

    // Chat
    chatInput.setMultiLine(true, true); // multi-line with word wrap
    chatInput.setReturnKeyStartsNewLine(false); // Enter still sends
    chatInput.setScrollbarsShown(true);
    chatInput.setTextToShowWhenEmpty("Ask about your mix...", C::text3);
    chatInput.setColour(juce::TextEditor::backgroundColourId, C::bg3);
    chatInput.setColour(juce::TextEditor::textColourId, C::text);
    chatInput.setColour(juce::TextEditor::outlineColourId, C::border2);
    chatInput.setFont(juce::Font(juce::FontOptions(13.0f)));
    chatInput.setIndents(8, 8);
    chatInput.addListener(this);
    addAndMakeVisible(chatInput);

    chatSendBtn.setColour(juce::TextButton::buttonColourId, C::blue);
    chatSendBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    chatSendBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    chatSendBtn.onClick = [this] {
        auto t = chatInput.getText().trim();
        if (t.isNotEmpty()) sendChatMessage(t);
    };
    addAndMakeVisible(chatSendBtn);

    addAndMakeVisible(chatScroll);
    chatScroll.setViewedComponent(&chatContent, false);
    chatScroll.setScrollBarsShown(true, false);
    chatScroll.addMouseListener(this, true);
    chatContent.setInterceptsMouseClicks(true, true);
    chatContent.addMouseListener(this, true);

    // Waveform play overlay buttons (invisible, positioned over waveform cards)
    for (int i = 0; i < kMaxWavePlayBtns; ++i)
    {
        wavePlayOverlays[(size_t)i].setAlpha(1.0f);
        wavePlayOverlays[(size_t)i].setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        wavePlayOverlays[(size_t)i].setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        wavePlayOverlays[(size_t)i].setInterceptsMouseClicks(true, false);
        wavePlayOverlays[(size_t)i].setVisible(false);
        // Use lambda that checks click position: left 30px = play/stop, rest = seek
        wavePlayOverlays[(size_t)i].onClick = [this, i]() {
            auto mousePos = wavePlayOverlays[(size_t)i].getMouseXYRelative();
            int playBtnArea = 30; // play button circle is ~22px + padding
            if (mousePos.x <= playBtnArea)
            {
                onWavePlayClick(i);
            }
            else
            {
                // Seek: calculate fraction within waveform area
                int wfStart = playBtnArea;
                int wfWidth = wavePlayOverlays[(size_t)i].getWidth() - wfStart - 6;
                if (wfWidth > 0)
                {
                    float frac = juce::jlimit(0.0f, 1.0f, (float)(mousePos.x - wfStart) / (float)wfWidth);
                    onWaveSeekClick(i, frac);
                }
            }
        };
        addAndMakeVisible(wavePlayOverlays[(size_t)i]);
    }

    startTimerHz(20);
}

EchoJayEditor::~EchoJayEditor() {
    stopChatPlayback();
    stopPlayback(); stopTimer(); setLookAndFeel(nullptr);
}

// ============================================================================
// Auth
// ============================================================================

void EchoJayEditor::showLoginScreen()
{
    currentScreen = Screen::Login;
    
    // Reset view state
    if (currentView == View::Settings) hideSettingsView();
    if (currentView == View::Compare) hideCompareView();
    currentView = View::Meters;
    
    loginTitle.setVisible(true); loginSubtitle.setVisible(true);
    emailInput.setVisible(true); passwordInput.setVisible(true);
    loginBtn.setVisible(true); loginErrorLabel.setVisible(true);

    juce::Component* mainComps[] = { &captureBtn, &scanBtn,
        &channelTypeBox, &genreBox, &statusLabel, &durationLabel, &detectedLabel,
        &passLabel, &userLabel, &usageLabel, &chatInput, &chatSendBtn, &chatScroll,
        &compareBtn, &settingsBtn, &playbackBtn, &wavSavedLabel };
    for (auto* c : mainComps) c->setVisible(false);
    logoutBtn.setVisible(false);
    settingsName.setVisible(false); settingsMonitors.setVisible(false);
    settingsHeadphones.setVisible(false); settingsGenres.setVisible(false);
    settingsPlugins.setVisible(false); settingsExpLevel.setVisible(false);
    saveSettingsBtn.setVisible(false); settingsSavedLabel.setVisible(false);
    for (auto& b : dawButtons) b.setVisible(false);
    
    // Also hide compare fields
    aiCompareBtn.setVisible(false);
    compareSlotABox.setVisible(false); compareSlotBBox.setVisible(false); playSlotABtn.setVisible(false); playSlotBBtn.setVisible(false);
    refStatusLabel.setVisible(false);
    presetBox.setVisible(false); savePresetBtn.setVisible(false); deletePresetBtn.setVisible(false); for (auto& b : refRemoveBtns) b.setVisible(false); compareClickCatcher.setVisible(false);

    resized(); repaint();
}

void EchoJayEditor::showMainScreen()
{
    currentScreen = Screen::Main;
    loginTitle.setVisible(false); loginSubtitle.setVisible(false);
    emailInput.setVisible(false); passwordInput.setVisible(false);
    loginBtn.setVisible(false); loginErrorLabel.setVisible(false);

    juce::Component* mainComps[] = { &captureBtn, &scanBtn,
        &channelTypeBox, &genreBox, &statusLabel, &durationLabel, &detectedLabel,
        &passLabel, &userLabel, &usageLabel, &chatInput, &chatSendBtn, &chatScroll,
        &compareBtn, &settingsBtn, &wavSavedLabel };
    for (auto* c : mainComps) c->setVisible(true);
    logoutBtn.setVisible(false); // logout only visible in Settings
    // playbackBtn visibility is managed by timerCallback based on WAV state

    auto info = api.getUserInfo();
    juce::String userText = info.displayName;
    juce::String tierTag = info.tier == "studio" ? "STUDIO" : info.tier == "pro" ? "PRO" : info.tier == "its_platinum" ? "ITS" : "";
    if (tierTag.isNotEmpty()) userText += "  " + tierTag;
    userLabel.setText(userText, juce::dontSendNotification);
    userLabel.setColour(juce::Label::textColourId, info.isPaidTier() ? C::purple : C::text2);

    int remaining = api.getRemainingMessages();
    int limit = info.messageLimit;
    usageLabel.setText(juce::String(remaining) + "/" + juce::String(limit) + " messages left",
                       juce::dontSendNotification);

    api.fetchSettings([this](bool success) {
        if (success) {
            settingsFetched = true;
            auto s = api.getUserSettings();
            settingsName.setText(s.name, false);
            settingsMonitors.setText(s.monitors, false);
            settingsHeadphones.setText(s.headphones, false);
            settingsGenres.setText(s.genres, false);
            settingsPlugins.setText(s.plugins, false);
            if (s.experienceLevel == "Beginner") settingsExpLevel.setSelectedId(1, juce::dontSendNotification);
            else if (s.experienceLevel == "Intermediate") settingsExpLevel.setSelectedId(2, juce::dontSendNotification);
            else if (s.experienceLevel == "Advanced") settingsExpLevel.setSelectedId(3, juce::dontSendNotification);
            else if (s.experienceLevel == "Expert") settingsExpLevel.setSelectedId(4, juce::dontSendNotification);
            juce::StringArray dawN = { "Logic Pro","Ableton Live","FL Studio","Pro Tools",
                "Studio One","Cubase","Reaper","Reason","Bitwig","GarageBand","Other" };
            for (int i = 0; i < 11; ++i)
                dawButtons[i].setToggleState(s.daws.contains(dawN[i]), juce::dontSendNotification);
        }
    });
    resized(); repaint();
}

void EchoJayEditor::attemptLogin()
{
    auto email = emailInput.getText().trim();
    auto password = passwordInput.getText();
    if (email.isEmpty() || password.isEmpty()) {
        loginErrorLabel.setText("Enter your email and password", juce::dontSendNotification);
        return;
    }
    loginLoading = true;
    loginBtn.setButtonText("Logging in...");
    loginBtn.setEnabled(false);
    loginErrorLabel.setText("", juce::dontSendNotification);

    api.login(email, password, [this](bool success, const juce::String& error) {
        loginLoading = false;
        loginBtn.setButtonText("Log In");
        loginBtn.setEnabled(true);
        if (success) { passwordInput.clear(); showMainScreen(); }
        else loginErrorLabel.setText(error, juce::dontSendNotification);
        repaint();
    });
}

void EchoJayEditor::handleLogout()
{
    api.logout();
    chatMessages.clear(); chatRoles.clear(); chatContents.clear();
    showLoginScreen();
}

// ============================================================================
// Drag & Drop
// ============================================================================

bool EchoJayEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files) {
        auto ext = juce::File(f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".aiff" ||
            ext == ".aif" || ext == ".ogg" || ext == ".m4a")
            return true;
    }
    return false;
}

void EchoJayEditor::fileDragEnter(const juce::StringArray&, int, int) { dragHovering = true; repaint(); }
void EchoJayEditor::fileDragExit(const juce::StringArray&) { dragHovering = false; repaint(); }

void EchoJayEditor::filesDropped(const juce::StringArray& files, int, int)
{
    dragHovering = false;
    for (auto& f : files) {
        juce::File file(f);
        auto ext = file.getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".aiff" ||
            ext == ".aif" || ext == ".ogg" || ext == ".m4a")
        {
            refStatusLabel.setText("Analysing " + file.getFileName() + "...", juce::dontSendNotification);
            
            // Copy file to EchoJay folder first to avoid sandbox issues
            auto destFolder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                  .getChildFile("EchoJay").getChildFile("References");
            destFolder.createDirectory();
            auto destFile = destFolder.getChildFile(file.getFileName());
            if (!destFile.existsAsFile())
                file.copyFileTo(destFile);
            
            auto fileToAnalyse = destFile.existsAsFile() ? destFile : file;
            processorRef.getReferenceAnalyser().analyseFile(fileToAnalyse, [this, file](bool success, const juce::String& error) {
                if (success) refStatusLabel.setText(juce::String(processorRef.getReferenceAnalyser().getReferenceCount()) + " reference(s) loaded", juce::dontSendNotification);
                else refStatusLabel.setText("Error: " + error, juce::dontSendNotification);
                if (currentView == View::Compare) showCompareView();
                repaint();
            });
        }
    }
    repaint();
}

// ============================================================================
// Reference Loading
// ============================================================================

void EchoJayEditor::loadReferenceFile()
{
    auto chooser = std::make_shared<juce::FileChooser>(
        "Select Reference Track", juce::File(), "*.wav;*.mp3;*.flac;*.aiff;*.aif;*.ogg;*.m4a");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (file.existsAsFile()) {
                refStatusLabel.setText("Analysing " + file.getFileName() + "...", juce::dontSendNotification);
                processorRef.getReferenceAnalyser().analyseFile(file, [this](bool success, const juce::String& error) {
                    if (success) refStatusLabel.setText(juce::String(processorRef.getReferenceAnalyser().getReferenceCount()) + " reference(s) loaded", juce::dontSendNotification);
                    else refStatusLabel.setText("Error: " + error, juce::dontSendNotification);
                    repaint();
                });
            }
        });
}

// ============================================================================
// Compare View
// ============================================================================

void EchoJayEditor::showCompareView()
{
    compareVisible = true;
    compareBtn.setButtonText("Back");
    compareBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    aiCompareBtn.setVisible(true);
    compareSlotABox.setVisible(true); compareSlotBBox.setVisible(true);
    refStatusLabel.setVisible(true);
    presetBox.setVisible(true); savePresetBtn.setVisible(true); deletePresetBtn.setVisible(true);
    loadPresetList();

    compareSlotABox.clear(); compareSlotBBox.clear();
    auto snaps = processorRef.getSnapshots();
    for (int i = 0; i < (int)snaps.size(); ++i) {
        compareSlotABox.addItem(snaps[i].name.substring(0, 30), i + 1);
        compareSlotBBox.addItem(snaps[i].name.substring(0, 30), i + 1);
    }
    auto refs = processorRef.getReferenceAnalyser().getReferences();
    int refOffset = (int)snaps.size() + 100;
    for (int i = 0; i < (int)refs.size(); ++i) {
        juce::String label = refs[i].name.substring(0, 25) + " (Ref)";
        compareSlotABox.addItem(label, refOffset + i);
        compareSlotBBox.addItem(label, refOffset + i);
    }
    if (snaps.size() > 0) compareSlotABox.setSelectedId(1);
    if (snaps.size() > 1) compareSlotBBox.setSelectedId(2);
    else if (refs.size() > 0) compareSlotBBox.setSelectedId(refOffset);
    resized(); repaint();
}

void EchoJayEditor::hideCompareView()
{
    compareVisible = false;
    compareBtn.setButtonText("Compare");
    compareBtn.setColour(juce::TextButton::buttonColourId, C::purple);
    aiCompareBtn.setVisible(false);
    compareSlotABox.setVisible(false); compareSlotBBox.setVisible(false); playSlotABtn.setVisible(false); playSlotBBtn.setVisible(false);
    refStatusLabel.setVisible(false);
    presetBox.setVisible(false); savePresetBtn.setVisible(false); deletePresetBtn.setVisible(false); for (auto& b : refRemoveBtns) b.setVisible(false); compareClickCatcher.setVisible(false);
    resized(); repaint();
}

void EchoJayEditor::runAICompare()
{
    if (!api.canSendMessage()) {
        chatMessages.push_back({"assistant", "Daily message limit reached."});
        repaint(); return;
    }
    int idA = compareSlotABox.getSelectedId();
    int idB = compareSlotBBox.getSelectedId();
    if (idA == 0 || idB == 0) return;

    auto snaps = processorRef.getSnapshots();
    auto refs = processorRef.getReferenceAnalyser().getReferences();
    int refOffset = (int)snaps.size() + 100;
    juce::String compareCtx;

    bool aIsRef = idA >= refOffset, bIsRef = idB >= refOffset;
    if (!aIsRef && bIsRef) {
        int ci = idA - 1, ri = idB - refOffset;
        if (ci >= 0 && ci < (int)snaps.size() && ri >= 0 && ri < (int)refs.size())
            compareCtx = processorRef.buildCompareContext(snaps[ci], refs[ri]);
    } else if (!aIsRef && !bIsRef) {
        int ia = idA - 1, ib = idB - 1;
        if (ia >= 0 && ia < (int)snaps.size() && ib >= 0 && ib < (int)snaps.size())
            compareCtx = processorRef.buildCompareContext(snaps[ia], snaps[ib]);
    } else if (aIsRef && !bIsRef) {
        int ri = idA - refOffset, ci = idB - 1;
        if (ci >= 0 && ci < (int)snaps.size() && ri >= 0 && ri < (int)refs.size())
            compareCtx = processorRef.buildCompareContext(snaps[ci], refs[ri]);
    }

    if (compareCtx.isEmpty()) {
        chatMessages.push_back({"assistant", "Select two different items to compare."});
        repaint(); return;
    }

    chatMessages.push_back({"user", "Compare these mixes"});
    chatLoading = true; repaint();
    chatRoles.add("user");
    chatContents.add("Give me a detailed comparison of these two.\n\n" + compareCtx);

    auto sysPrompt = EchoJayAPI::buildSystemPrompt(
        processorRef.getEffectiveChannelName(), processorRef.getGenre(),
        processorRef.getPluginScanner().getPluginNamesString());

    api.sendChat(chatRoles, chatContents, sysPrompt,
        [this](const juce::String& reply, bool success) {
            chatLoading = false;
            chatMessages.push_back({"assistant", reply});
            if (success) { chatRoles.add("assistant"); chatContents.add(reply); }
            repaint();
        });
}

void EchoJayEditor::paintCompareView(juce::Graphics& g, juce::Rectangle<int> area)
{
    auto snaps = processorRef.getSnapshots();
    auto refs = processorRef.getReferenceAnalyser().getReferences();
    int refOffset = (int)snaps.size() + 100;
    auto ff = [](float v) { return v > -99.0f ? juce::String(v, 1) : juce::String("N/A"); };

    int pad = 10;
    int cardW = (area.getWidth() - 12) / 2;
    int aX = area.getX(), aW = area.getWidth();
    
    // Build compare wave positions (don't clear — timer uses them between paints)
    std::vector<CompareWavePos> newWavePositions;
    activeWavePlayBtns = 0;
    int cy = area.getY(); // current Y cursor

    // Preset row is at cy (positioned by resized), skip it
    cy += 26; // preset row height + gap

    // Reference drop zone — sized for 4 tags per row, 3 rows
    int dropH = 82; // fits 3 rows of tags + padding
    g.setColour(C::bg3);
    g.fillRoundedRectangle((float)aX, (float)cy, (float)aW, (float)dropH, 8.0f);
    g.setColour(C::purple.withAlpha(0.3f));
    g.drawRoundedRectangle((float)aX + 0.5f, (float)cy + 0.5f, (float)aW - 1.0f, (float)dropH - 1.0f, 8.0f, 1.0f);
    
    activeRefRemoveBtns = 0;
    
    if (refs.empty())
    {
        g.setColour(C::purple);
        g.fillRoundedRectangle((float)aX + 10, (float)cy + (float)(dropH - 18) / 2, 64.0f, 18.0f, 4.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.drawText("REFERENCE", aX + 10, cy + (dropH - 18) / 2, 64, 18, juce::Justification::centred);
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.drawText("Drop a reference track to compare", aX + 82, cy, aW - 90, dropH, juce::Justification::centredLeft);
    }
    else
    {
        int tagPad = 6;
        int tagGap = 4;
        int tagsPerRow = 4;
        int tagW = (aW - tagPad * 2 - tagGap * (tagsPerRow - 1)) / tagsPerRow;
        int tagH = 20;
        int tagRowGap = 4;
        
        for (int i = 0; i < (int)refs.size() && i < 12; ++i)
        {
            int col = i % tagsPerRow;
            int row = i / tagsPerRow;
            int tagX = aX + tagPad + col * (tagW + tagGap);
            int tagY2 = cy + tagPad + row * (tagH + tagRowGap);
            
            // Tag pill
            g.setColour(C::purple.withAlpha(0.2f));
            g.fillRoundedRectangle((float)tagX, (float)tagY2, (float)tagW, (float)tagH, 6.0f);
            g.setColour(C::purple.withAlpha(0.5f));
            g.drawRoundedRectangle((float)tagX + 0.5f, (float)tagY2 + 0.5f, (float)tagW - 1.0f, (float)tagH - 1.0f, 6.0f, 1.0f);
            
            // Name (truncated)
            g.setColour(C::purple);
            g.setFont(juce::Font(juce::FontOptions(8.0f)));
            g.drawText(refs[(size_t)i].name, tagX + 4, tagY2, tagW - 18, tagH, juce::Justification::centredLeft);
            
            // X button
            g.setColour(C::text3);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText("x", tagX + tagW - 14, tagY2, 12, tagH, juce::Justification::centred);
            
            // Overlay remove button
            if (activeRefRemoveBtns < kMaxRefRemoveBtns)
            {
                int idx = activeRefRemoveBtns++;
                refRemoveBtns[(size_t)idx].setBounds(tagX + tagW - 16, tagY2, 16, tagH);
                refRemoveBtns[(size_t)idx].setVisible(true);
                refRemoveBtns[(size_t)idx].toFront(false);
            }
        }
    }
    
    for (int i = activeRefRemoveBtns; i < kMaxRefRemoveBtns; ++i)
        refRemoveBtns[(size_t)i].setVisible(false);

    cy += dropH + 4; // below drop zone
    // Dropdowns are at cy (positioned by resized), skip
    cy += 26; // dropdown height + gap
    int dropdownY = cy;
    
    // Waveform bars above cards — separate from cards so overlays work
    int wfBarH = 28;
    auto snapsLocal = processorRef.getSnapshots();
    auto refsLocal = processorRef.getReferenceAnalyser().getReferences();
    int refOffsetLocal = (int)snapsLocal.size() + 100;
    
    auto drawWaveBar = [&](int wx, int wy, int ww, const std::vector<float>& waveform, 
                           const juce::String& wavPath, float dur, bool isRef)
    {
        g.setColour(C::bg3);
        g.fillRoundedRectangle((float)wx, (float)wy, (float)ww, (float)wfBarH, 6.0f);
        
        if (!waveform.empty())
        {
            // Play circle
            int playR = 18;
            int playX2 = wx + 4;
            int playY2 = wy + (wfBarH - playR) / 2;
            bool playing = (currentlyPlayingChatWav == wavPath && wavPath.isNotEmpty());
            g.setColour(playing ? C::red.withAlpha(0.6f) : C::purple.withAlpha(0.5f));
            g.fillEllipse((float)playX2, (float)playY2, (float)playR, (float)playR);
            g.setColour(juce::Colours::white);
            if (playing) {
                g.fillRect((float)playX2 + 6.0f, (float)playY2 + 6.0f, 6.0f, 6.0f);
            } else {
                juce::Path tri;
                tri.addTriangle((float)playX2 + 7, (float)playY2 + 4, (float)playX2 + 7, (float)playY2 + 14, (float)playX2 + 14, (float)playY2 + 9);
                g.fillPath(tri);
            }
            
            // Waveform
            int wfStartX = wx + playR + 10;
            int wfBarW = ww - playR - 14;
            int numPts = (int)waveform.size();
            if (numPts > 0 && wfBarW > 0)
            {
                float pxPerPt = (float)wfBarW / (float)numPts;
                float centreY2 = (float)wy + (float)wfBarH * 0.5f;
                float halfH2 = (float)wfBarH * 0.38f;
                for (int i2 = 0; i2 < numPts; ++i2)
                {
                    float px = (float)wfStartX + (float)i2 * pxPerPt;
                    float h = waveform[(size_t)i2] * halfH2;
                    float frac = (float)i2 / (float)numPts;
                    g.setColour(C::blue.interpolatedWith(C::purple, frac).withAlpha(0.6f));
                    g.fillRect(px, centreY2 - h, std::max(1.0f, pxPerPt - 0.3f), h * 2.0f);
                }
                
                // Playback cursor
                if (playing && chatPlaybackDuration > 0)
                {
                    double elapsed = (juce::Time::getMillisecondCounterHiRes() - chatPlaybackStartTime) / 1000.0 + chatPlaybackOffset;
                    float playFrac = juce::jlimit(0.0f, 1.0f, (float)(elapsed / chatPlaybackDuration));
                    float cursorX = (float)wfStartX + playFrac * (float)wfBarW;
                    g.setColour(juce::Colours::white);
                    g.drawVerticalLine((int)cursorX, (float)wy + 2, (float)(wy + wfBarH - 2));
                }
            }
        }
        
        // Store position for overlay
        if (wavPath.isNotEmpty())
            newWavePositions.push_back({ { wx, wy, ww, wfBarH }, wavPath, dur });
    };
    
    // Helper to draw placeholder waveform bar
    auto drawEmptyWaveBar = [&](int wx, int wy, int ww) {
        g.setColour(C::bg3);
        g.fillRoundedRectangle((float)wx, (float)wy, (float)ww, (float)wfBarH, 6.0f);
        // Fake greyed-out waveform
        juce::Random rng(42); // fixed seed for consistent look
        float centreY2 = (float)wy + (float)wfBarH * 0.5f;
        int numBars = ww / 3;
        for (int i2 = 0; i2 < numBars; ++i2)
        {
            float px = (float)wx + 4.0f + (float)i2 * 3.0f;
            float h = (2.0f + rng.nextFloat() * 6.0f);
            g.setColour(C::text3.withAlpha(0.12f));
            g.fillRect(px, centreY2 - h, 2.0f, h * 2.0f);
        }
    };
    
    // Draw waveform bars for each slot
    int idALocal = compareSlotABox.getSelectedId();
    int idBLocal = compareSlotBBox.getSelectedId();
    
    if (idALocal > 0)
    {
        if (idALocal >= refOffsetLocal && (idALocal - refOffsetLocal) < (int)refsLocal.size()) {
            auto& r = refsLocal[(size_t)(idALocal - refOffsetLocal)];
            drawWaveBar(aX, cy + 4, cardW, r.waveformThumbnail, r.path, r.durationSeconds, true);
        } else if ((idALocal - 1) < (int)snapsLocal.size()) {
            auto& s = snapsLocal[(size_t)(idALocal - 1)];
            drawWaveBar(aX, cy + 4, cardW, s.waveformThumbnail, s.wavFilePath, s.durationSeconds, false);
        }
    }
    else
        drawEmptyWaveBar(aX, cy + 4, cardW);
    
    if (idBLocal > 0)
    {
        if (idBLocal >= refOffsetLocal && (idBLocal - refOffsetLocal) < (int)refsLocal.size()) {
            auto& r = refsLocal[(size_t)(idBLocal - refOffsetLocal)];
            drawWaveBar(aX + cardW + 12, cy + 4, cardW, r.waveformThumbnail, r.path, r.durationSeconds, true);
        } else if ((idBLocal - 1) < (int)snapsLocal.size()) {
            auto& s = snapsLocal[(size_t)(idBLocal - 1)];
            drawWaveBar(aX + cardW + 12, cy + 4, cardW, s.waveformThumbnail, s.wavFilePath, s.durationSeconds, false);
        }
    }
    else
        drawEmptyWaveBar(aX + cardW + 12, cy + 4, cardW);
    
    cy += wfBarH + 8;
    
    // Cards below waveform bars
    int cardY = cy;
    int cardH = area.getY() + area.getHeight() - cardY - 36;
    
    auto drawCompareCard = [&](juce::Rectangle<int> card, const MeterData& d, const juce::String& title,
                                bool isRef, const std::vector<float>& waveform, float dur,
                                const std::array<float, 64>& eqCurve, const juce::String& wavPath)
    {
        g.setColour(C::bg2);
        g.fillRoundedRectangle(card.toFloat(), 8.0f);
        g.setColour(isRef ? C::purple.withAlpha(0.4f) : C::border);
        g.drawRoundedRectangle(card.toFloat(), 8.0f, 1.0f);
        
        int cx = card.getX() + 8, cy = card.getY() + 6, cw = card.getWidth() - 16;
        
        // Reference badge
        if (isRef)
        {
            g.setColour(C::purple);
            g.fillRoundedRectangle((float)cx, (float)cy, 58.0f, 14.0f, 3.0f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
            g.drawText("REFERENCE", cx, cy, 58, 14, juce::Justification::centred);
            cy += 16;
        }
        
        // Title
        g.setColour(C::text);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(title, cx, cy, cw, 13, juce::Justification::centredLeft);
        
        // Subtitle: genre + duration
        int mins = (int)dur / 60, secs = (int)dur % 60;
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        juce::String sub = processorRef.getGenre() + " - " + juce::String::formatted("%d:%02d", mins, secs);
        g.drawText(sub, cx, cy + 12, cw, 10, juce::Justification::centredLeft);
        cy += 24;
        
        // Meter rows — compact
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        auto row = [&](const juce::String& lbl, const juce::String& val, juce::Colour col) {
            g.setColour(C::text3);
            g.drawText(lbl, cx, cy, cw / 2, 13, juce::Justification::centredLeft);
            g.setColour(col);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText(val, cx + cw / 2, cy, cw / 2, 13, juce::Justification::centredRight);
            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            cy += 14;
        };
        
        row("Integrated", ff(d.integrated) + " LUFS", d.integrated > -14 ? C::amber : C::green);
        row("True Peak L", ff(d.truePeakL) + " dBTP", d.truePeakL > -0.1f ? C::red : C::green);
        row("True Peak R", ff(d.truePeakR) + " dBTP", d.truePeakR > -0.1f ? C::red : C::green);
        row("LRA", ff(d.loudnessRange) + " LU", C::text2);
        row("Crest", juce::String(d.crestFactor, 1) + " dB", d.crestFactor > 12 ? C::green : C::amber);
        row("Width", juce::String(d.width, 1) + " %", C::blue2);
        row("Correlation", juce::String(d.correlation, 2), d.correlation < 0.5f ? C::amber : C::green);
        row("DC", juce::String(d.dcOffset, 2) + " mV", std::abs(d.dcOffset) > 5 ? C::red : C::text3);
        
        // EQ Curve
        cy += 2;
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
        g.drawText("EQ CURVE", cx, cy, cw, 10, juce::Justification::centredLeft);
        cy += 11;
        
        int eqH = std::max(24, card.getY() + card.getHeight() - cy - 4);
        g.setColour(C::bg.withAlpha(0.5f));
        g.fillRoundedRectangle((float)cx, (float)cy, (float)cw, (float)eqH, 4.0f);
        
        if (eqH > 8)
        {
            juce::Path eqPath;
            float minDB = -80.0f, maxDB = 0.0f;
            for (int i = 0; i < 64; ++i)
            {
                float xp = (float)cx + ((float)i / 63.0f) * (float)cw;
                float db = juce::jlimit(minDB, maxDB, eqCurve[(size_t)i]);
                float yp = (float)cy + (float)eqH * (1.0f - (db - minDB) / (maxDB - minDB));
                if (i == 0) eqPath.startNewSubPath(xp, yp);
                else eqPath.lineTo(xp, yp);
            }
            g.setColour(isRef ? C::purple : C::blue);
            g.strokePath(eqPath, juce::PathStrokeType(1.5f));
            
            juce::Path fillPath = eqPath;
            fillPath.lineTo((float)(cx + cw), (float)(cy + eqH));
            fillPath.lineTo((float)cx, (float)(cy + eqH));
            fillPath.closeSubPath();
            g.setColour((isRef ? C::purple : C::blue).withAlpha(0.12f));
            g.fillPath(fillPath);
        }
    };

    // Draw cards
    int idA = compareSlotABox.getSelectedId();
    auto cardARect = juce::Rectangle<int>(area.getX(), cardY, cardW, cardH);
    if (idA > 0) {
        if (idA >= refOffset && (idA - refOffset) < (int)refs.size()) {
            auto& r = refs[(size_t)(idA - refOffset)];
            drawCompareCard(cardARect, r.data, r.name, true, r.waveformThumbnail, r.durationSeconds, r.eqCurve, r.path);
        } else if ((idA - 1) < (int)snaps.size()) {
            auto& s = snaps[(size_t)(idA - 1)];
            drawCompareCard(cardARect, s.averagedData, s.name, false, s.waveformThumbnail, s.durationSeconds, s.eqCurve, s.wavFilePath);
        }
    } else {
        // Empty card placeholder
        g.setColour(C::bg2);
        g.fillRoundedRectangle(cardARect.toFloat(), 8.0f);
        g.setColour(C::border);
        g.drawRoundedRectangle(cardARect.toFloat(), 8.0f, 1.0f);
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText("Capture a pass or select\na mix from the dropdown", cardARect, juce::Justification::centred);
    }
    
    int idB = compareSlotBBox.getSelectedId();
    auto cardBRect = juce::Rectangle<int>(area.getX() + cardW + 12, cardY, cardW, cardH);
    if (idB > 0) {
        if (idB >= refOffset && (idB - refOffset) < (int)refs.size()) {
            auto& r = refs[(size_t)(idB - refOffset)];
            drawCompareCard(cardBRect, r.data, r.name, true, r.waveformThumbnail, r.durationSeconds, r.eqCurve, r.path);
        } else if ((idB - 1) < (int)snaps.size()) {
            auto& s = snaps[(size_t)(idB - 1)];
            drawCompareCard(cardBRect, s.averagedData, s.name, false, s.waveformThumbnail, s.durationSeconds, s.eqCurve, s.wavFilePath);
        }
    } else {
        // Empty card placeholder
        g.setColour(C::bg2);
        g.fillRoundedRectangle(cardBRect.toFloat(), 8.0f);
        g.setColour(C::border);
        g.drawRoundedRectangle(cardBRect.toFloat(), 8.0f, 1.0f);
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText("Drop a reference track\nor select from dropdown", cardBRect, juce::Justification::centred);
    }

    // Drag-drop overlay
    if (dragHovering) {
        g.setColour(C::purple.withAlpha(0.12f));
        g.fillRoundedRectangle(area.toFloat(), 12.0f);
        g.setColour(C::purple.withAlpha(0.6f));
        g.drawRoundedRectangle(area.expanded(2).toFloat(), 12.0f, 2.0f);
        g.setColour(C::purple);
        g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        g.drawText("Drop reference track here", area, juce::Justification::centred);
    }
    
    // Update positions for timer to use
    compareWavePositions = newWavePositions;
}

// ============================================================================
// Settings View
// ============================================================================

void EchoJayEditor::showSettingsView()
{
    currentView = View::Settings;
    settingsBtn.setButtonText("Back");
    settingsName.setVisible(true); settingsMonitors.setVisible(true);
    settingsHeadphones.setVisible(true); settingsGenres.setVisible(true);
    settingsPlugins.setVisible(true); settingsExpLevel.setVisible(true);
    saveSettingsBtn.setVisible(true); settingsSavedLabel.setVisible(true);
    for (auto& b : dawButtons) b.setVisible(true);
    
    auto populateFields = [this]() {
        auto s = api.getUserSettings();
        settingsName.setText(s.name, false);
        settingsMonitors.setText(s.monitors, false);
        settingsHeadphones.setText(s.headphones, false);
        settingsGenres.setText(s.genres, false);
        settingsPlugins.setText(s.plugins, false);
        
        if (s.experienceLevel == "Beginner") settingsExpLevel.setSelectedId(1, juce::dontSendNotification);
        else if (s.experienceLevel == "Intermediate") settingsExpLevel.setSelectedId(2, juce::dontSendNotification);
        else if (s.experienceLevel == "Advanced") settingsExpLevel.setSelectedId(3, juce::dontSendNotification);
        else if (s.experienceLevel == "Expert") settingsExpLevel.setSelectedId(4, juce::dontSendNotification);
        
        juce::StringArray dawN = { "Logic Pro","Ableton Live","FL Studio","Pro Tools",
            "Studio One","Cubase","Reaper","Reason","Bitwig","GarageBand","Other" };
        for (int i = 0; i < 11; ++i)
            dawButtons[(size_t)i].setToggleState(s.daws.contains(dawN[i]), juce::dontSendNotification);
    };
    
    // Only show cached data if we've already fetched from server
    if (settingsFetched)
        populateFields();
    else
        settingsSavedLabel.setText("Loading...", juce::dontSendNotification);
    
    // Always re-fetch from server
    api.fetchSettings([this, populateFields](bool success) {
        if (success) {
            settingsFetched = true;
            populateFields();
            settingsSavedLabel.setText("", juce::dontSendNotification);
            repaint();
        } else {
            settingsSavedLabel.setText("Could not load settings", juce::dontSendNotification);
        }
    });
    
    resized(); repaint();
}

void EchoJayEditor::hideSettingsView()
{
    if (currentView != View::Settings) return;
    currentView = View::Meters;
    settingsBtn.setButtonText("Settings");
    settingsName.setVisible(false); settingsMonitors.setVisible(false);
    settingsHeadphones.setVisible(false); settingsGenres.setVisible(false);
    settingsPlugins.setVisible(false); settingsExpLevel.setVisible(false);
    saveSettingsBtn.setVisible(false); settingsSavedLabel.setVisible(false);
    for (auto& b : dawButtons) b.setVisible(false);
    resized(); repaint();
}

void EchoJayEditor::saveSettingsToServer()
{
    UserSettings s;
    s.name = settingsName.getText(); s.monitors = settingsMonitors.getText();
    s.headphones = settingsHeadphones.getText(); s.genres = settingsGenres.getText();
    s.plugins = settingsPlugins.getText();
    switch (settingsExpLevel.getSelectedId()) {
        case 1: s.experienceLevel = "Beginner"; break;
        case 2: s.experienceLevel = "Intermediate"; break;
        case 3: s.experienceLevel = "Advanced"; break;
        case 4: s.experienceLevel = "Expert"; break;
        default: break;
    }
    juce::StringArray dawN = { "Logic Pro","Ableton Live","FL Studio","Pro Tools",
        "Studio One","Cubase","Reaper","Reason","Bitwig","GarageBand","Other" };
    for (int i = 0; i < 11; ++i)
        if (dawButtons[i].getToggleState()) s.daws.add(dawN[i]);
    settingsSavedLabel.setText("Saving...", juce::dontSendNotification);
    api.saveUserSettings(s, [this](bool success) {
        settingsSavedLabel.setText(success ? "Saved" : "Failed to save", juce::dontSendNotification);
        if (success)
            juce::Timer::callAfterDelay(3000, [this]() {
                settingsSavedLabel.setText("", juce::dontSendNotification);
            });
    });
}

void EchoJayEditor::paintSettingsView(juce::Graphics& g, juce::Rectangle<int> area)
{
    int x = area.getX(), w = area.getWidth();
    int fh = 30, labelGap = 18;
    int y = area.getY();
    
    // === Account info at top of settings ===
    if (api.isLoggedIn())
    {
        auto info = api.getUserInfo();
        int remaining = api.getRemainingMessages();
        int limit = info.messageLimit;
        int used = limit - remaining;
        
        // Usage count on same line as YOUR NAME label
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        juce::String usageStr = juce::String(used) + "/" + juce::String(limit) + " messages";
        g.drawText(usageStr, x + w - 90, y, 90, 14, juce::Justification::centredRight);
    }
    
    auto label = [&](const juce::String& text) {
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(text, x, y, w, 14, juce::Justification::centredLeft);
        y += labelGap + fh + 8;
    };
    
    label("YOUR NAME");
    
    // DAW(S) — label then skip past the toggle buttons
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawText("DAW(S)", x, y, w, 14, juce::Justification::centredLeft);
    // DAW buttons take ~4 rows of 26px + gaps
    int sw = juce::jmin(560, w);
    int bw = (sw - 20) / 3, bh2 = 26;
    int dawRows = (11 + 2) / 3; // ceil(11/3) = 4 rows
    y += labelGap + dawRows * (bh2 + 3) + 8;
    
    label("EXPERIENCE LEVEL");
    label("MAIN MONITORS / SPEAKERS");
    label("HEADPHONES");
    label("GENRES YOU WORK WITH");
    
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawText("YOUR PLUGINS", x, y, w, 14, juce::Justification::centredLeft);
}

// ============================================================================
// Section Panel Helpers
// ============================================================================

void EchoJayEditor::drawPanel(juce::Graphics& g, juce::Rectangle<int> area,
                               const juce::String& title, juce::Colour titleCol)
{
    g.setColour(C::bg2);
    g.fillRoundedRectangle(area.toFloat(), 10.0f);
    g.setColour(C::border);
    g.drawRoundedRectangle(area.toFloat(), 10.0f, 1.0f);

    if (title.isNotEmpty()) {
        g.setColour(titleCol);
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText(title, area.getX() + 14, area.getY() + 8, area.getWidth() - 28, 16,
                   juce::Justification::centredLeft);
    }
}

void EchoJayEditor::drawHBar(juce::Graphics& g, int x, int y, int w, int h,
                              const juce::String& label, float valuedB, float minDb, float maxDb,
                              juce::Colour startCol, juce::Colour endCol, const juce::String& unit)
{
    float n = juce::jlimit(0.0f, 1.0f, (valuedB - minDb) / (maxDb - minDb));
    int labelW = 44, valueW = 60, barX = x + labelW, barW = w - labelW - valueW - 8;

    // Label
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    g.drawText(label, x, y, labelW, h, juce::Justification::centredLeft);

    // Track
    g.setColour(C::bg3);
    auto track = juce::Rectangle<float>((float)barX, (float)y + 2, (float)barW, (float)h - 4);
    g.fillRoundedRectangle(track, 3.0f);

    // Fill bar with gradient
    if (n > 0.005f) {
        auto fill = track.withWidth(track.getWidth() * n);
        juce::ColourGradient grad(startCol, fill.getX(), 0, endCol, fill.getRight(), 0, false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(fill, 3.0f);
    }

    // Clip indicator tick at end of track
    g.setColour(C::blue.withAlpha(0.3f));
    g.fillRect((float)(barX + barW - 2), (float)y + 2, 2.0f, (float)h - 4);

    // Value
    g.setColour(C::text);
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    juce::String valStr = valuedB > -99 ? juce::String(valuedB, 1) : "N/A";
    g.drawText(valStr + " " + unit, barX + barW + 4, y, valueW, h, juce::Justification::centredLeft);
}

// ============================================================================
// Section Painters (matching web app layout)
// ============================================================================

void EchoJayEditor::paintLoudnessPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md)
{
    drawPanel(g, area, "LOUDNESS - EBU R128", C::purple);

    int innerX = area.getX() + 14, innerW = area.getWidth() - 28;
    int y = area.getY() + 30;
    int cellW = innerW / 4;

    auto ff = [](float v) -> juce::String { return v > -99 ? juce::String(v, 1) : "--"; };

    // 4 big numbers: Momentary, Short Term, Integrated, LRA
    struct LufsItem { const char* label; float val; const char* unit; juce::Colour col; };
    LufsItem items[] = {
        { "Momentary",  md.momentary,     "LUFS", md.momentary > -6 ? C::red : C::green },
        { "Short Term", md.shortTerm,     "LUFS", C::blue2 },
        { "Integrated", md.integrated,    "LUFS", C::green },
        { "LRA",        md.loudnessRange, "LU",   C::text }
    };
    for (int i = 0; i < 4; ++i) {
        int cx = innerX + i * cellW;
        // Sub-label
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        g.drawText(items[i].label, cx, y, cellW, 14, juce::Justification::centred);
        // Big value
        g.setColour(items[i].col);
        g.setFont(juce::Font(juce::FontOptions(26.0f, juce::Font::bold)));
        g.drawText(ff(items[i].val), cx, y + 14, cellW, 32, juce::Justification::centred);
        // Unit
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        g.drawText(items[i].unit, cx, y + 46, cellW, 12, juce::Justification::centred);
    }
}

void EchoJayEditor::paintLevelsPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md)
{
    drawPanel(g, area, "LEVELS", C::green);

    int y = area.getY() + 28, x = area.getX() + 14, w = area.getWidth() - 28;
    int barH = 14, gap = 4;

    drawHBar(g, x, y, w, barH, "RMS L", md.rmsL, -60, 0, C::green, C::green); y += barH + gap;
    drawHBar(g, x, y, w, barH, "RMS R", md.rmsR, -60, 0, C::green, C::green); y += barH + gap + 2;

    drawHBar(g, x, y, w, barH, "PEAK L", md.peakL, -60, 0, C::amber, C::amber); y += barH + gap;
    drawHBar(g, x, y, w, barH, "PEAK R", md.peakR, -60, 0, C::amber, C::amber); y += barH + gap + 2;

    float tpL = md.truePeakL, tpR = md.truePeakR;
    drawHBar(g, x, y, w, barH, "TP L", tpL, -60, 0,
             tpL > -1 ? C::red : C::amber, tpL > -1 ? C::red : C::amber); y += barH + gap;
    drawHBar(g, x, y, w, barH, "TP R", tpR, -60, 0,
             tpR > -1 ? C::red : C::amber, tpR > -1 ? C::red : C::amber); y += barH + gap + 11;

    // Crest + DC offset boxes at bottom — fit within panel padding
    int boxGap = 6;
    int boxW = (w - boxGap) / 2;
    auto drawSmallBox = [&](int bx, int by, int bw, const juce::String& lbl, const juce::String& val,
                            juce::Colour col, const juce::String& unit2) {
        g.setColour(C::bg3);
        g.fillRoundedRectangle((float)bx, (float)by, (float)bw, 38.0f, 6.0f);
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.drawText(lbl, bx, by + 2, bw, 12, juce::Justification::centred);
        g.setColour(col);
        g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        g.drawText(val, bx, by + 12, bw, 20, juce::Justification::centred);
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        g.drawText(unit2, bx, by + 28, bw, 10, juce::Justification::centred);
    };
    drawSmallBox(x, y + 5, boxW, "CREST", juce::String(md.crestFactor, 1), C::amber, "dB");
    drawSmallBox(x + boxW + boxGap, y + 5, boxW, "DC OFFSET", juce::String(md.dcOffset, 2),
                 std::abs(md.dcOffset) > 5 ? C::red : C::text3, "mV");
}

void EchoJayEditor::paintStereoPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md)
{
    drawPanel(g, area, "STEREO IMAGE", C::blue2);

    int x = area.getX() + 14, y = area.getY() + 28, w = area.getWidth() - 28;

    // WIDTH bar
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    g.drawText("WIDTH", x, y, 60, 14, juce::Justification::centredLeft);

    float widthN = juce::jlimit(0.0f, 1.0f, md.width / 100.0f);
    auto wTrack = juce::Rectangle<float>((float)(x + 64), (float)y, (float)(w - 130), 14.0f);
    g.setColour(C::bg3);
    g.fillRoundedRectangle(wTrack, 3.0f);
    if (widthN > 0.01f) {
        auto wFill = wTrack.withWidth(wTrack.getWidth() * widthN);
        juce::ColourGradient grad(C::purple.withAlpha(0.5f), wFill.getX(), 0,
                                   C::blue2, wFill.getRight(), 0, false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(wFill, 3.0f);
    }
    g.setColour(C::text);
    g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    g.drawText(juce::String(md.width, 1) + " %", (int)wTrack.getRight() + 4, y, 60, 14,
               juce::Justification::centredRight);

    y += 22;

    // CORRELATION bar (centred: -1 to +1)
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    g.drawText("CORR", x, y, 60, 14, juce::Justification::centredLeft);

    auto cTrack = juce::Rectangle<float>((float)(x + 64), (float)y, (float)(w - 130), 14.0f);
    g.setColour(C::bg3);
    g.fillRoundedRectangle(cTrack, 3.0f);

    float corrN = (md.correlation + 1.0f) / 2.0f; // normalise -1..+1 to 0..1
    float centre = cTrack.getX() + cTrack.getWidth() * 0.5f;
    float fillStart = (corrN < 0.5f) ? cTrack.getX() + cTrack.getWidth() * corrN : centre;
    float fillEnd   = (corrN < 0.5f) ? centre : cTrack.getX() + cTrack.getWidth() * corrN;
    juce::Colour corrCol = md.correlation < 0 ? C::red : C::green;
    g.setColour(corrCol.withAlpha(0.7f));
    g.fillRoundedRectangle(fillStart, cTrack.getY(), fillEnd - fillStart, cTrack.getHeight(), 3.0f);

    // Centre tick
    g.setColour(C::text3.withAlpha(0.5f));
    g.fillRect(centre - 0.5f, cTrack.getY(), 1.0f, cTrack.getHeight());

    // Labels -1, 0, +1
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    g.setColour(C::text3);
    g.drawText("-1", (int)cTrack.getX(), y + 14, 16, 10, juce::Justification::centred);
    g.drawText("0", (int)centre - 4, y + 14, 8, 10, juce::Justification::centred);
    g.drawText("+1", (int)cTrack.getRight() - 16, y + 14, 16, 10, juce::Justification::centred);

    g.setColour(C::text);
    g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    g.drawText(juce::String(md.correlation, 2), (int)cTrack.getRight() + 4, y, 60, 14,
               juce::Justification::centredRight);

    // === GONIOMETER / VECTORSCOPE ===
    y += 30;
    int maxGonioH = area.getY() + area.getHeight() - y - 4;
    int gonioSize2 = std::min(w, maxGonioH);
    if (gonioSize2 < 40) return;
    
    int gx = x + (w - gonioSize2) / 2;
    int gy = y;
    auto gonioRect = juce::Rectangle<float>((float)gx, (float)gy, (float)gonioSize2, (float)gonioSize2);
    
    // Clip to goniometer rectangle
    g.saveState();
    g.reduceClipRegion(gonioRect.toNearestInt());
    
    // Dark background
    g.setColour(C::bg.withAlpha(0.8f));
    g.fillRoundedRectangle(gonioRect, 4.0f);
    
    // Cross-hair axes (L-R horizontal, M-S vertical as rotated 45°)
    float gcx = gonioRect.getCentreX(), gcy = gonioRect.getCentreY();
    float gr = gonioSize2 * 0.45f;
    g.setColour(C::text3.withAlpha(0.15f));
    // Diagonal axes (L and R channels)
    g.drawLine(gcx - gr, gcy + gr, gcx + gr, gcy - gr, 0.5f); // L axis
    g.drawLine(gcx - gr, gcy - gr, gcx + gr, gcy + gr, 0.5f); // R axis
    // Horizontal and vertical centre lines
    g.drawLine(gcx - gr, gcy, gcx + gr, gcy, 0.5f);
    g.drawLine(gcx, gcy - gr, gcx, gcy + gr, 0.5f);
    
    // Plot L/R sample pairs as a vectorscope (Lissajous)
    // Rotate 45° so mono signal goes straight up
    constexpr int gN = MeterData::gonioSize;
    int gWPos = md.gonioWritePos;
    
    for (int i = 0; i < gN; ++i)
    {
        int idx = (gWPos + i) % gN;
        float sL = md.gonioL[(size_t)idx];
        float sR = md.gonioR[(size_t)idx];
        
        // Rotate 45°: mid = L+R (vertical), side = L-R (horizontal)
        float plotX = (sL - sR) * 0.707f; // side
        float plotY = -(sL + sR) * 0.707f; // mid (negative = up)
        
        float px = gcx + plotX * gr;
        float py = gcy + plotY * gr;
        
        // Fade older samples
        float age = (float)i / (float)gN;
        float alpha = age * 0.6f + 0.05f;
        
        // Colour based on position: blue in centre, purple at edges
        g.setColour(C::blue.interpolatedWith(C::purple, std::min(1.0f, std::sqrt(plotX * plotX + plotY * plotY) * 2.0f))
                    .withAlpha(alpha));
        g.fillRect(px, py, 1.5f, 1.5f);
    }
    
    // Axis labels
    g.setColour(C::text3.withAlpha(0.4f));
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    g.drawText("M", (int)gcx - 6, (int)(gcy - gr - 12), 12, 10, juce::Justification::centred);
    g.drawText("L", (int)(gcx - gr - 10), (int)gcy - 5, 10, 10, juce::Justification::centred);
    g.drawText("R", (int)(gcx + gr + 2), (int)gcy - 5, 10, 10, juce::Justification::centred);
    g.restoreState(); // end goniometer clip
}

void EchoJayEditor::paintSpectrumPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md)
{
    drawPanel(g, area, "SPECTRUM", C::purple);

    int x = area.getX() + 14, y = area.getY() + 28;
    int w = area.getWidth() - 28, h = area.getHeight() - 36;
    constexpr int N = MeterData::numSpecBins; // 64
    
    // Bar width with 1px gap — continuous look like Logic's analyser
    int barGap = 1;
    int barW = std::max(2, (w - (N - 1) * barGap) / N);
    int totalW = N * barW + (N - 1) * barGap;
    int xOff = x + (w - totalW) / 2;

    int barMaxH = h - 14;
    
    // Auto-range: find peak, set 66dB display range
    float peakDb = -200.0f;
    for (int i = 0; i < N; ++i)
        if (md.spectrum[(size_t)i] > peakDb) peakDb = md.spectrum[(size_t)i];
    float dbMax = std::max(-20.0f, peakDb + 3.0f);
    float dbMin = dbMax - 66.0f;

    for (int i = 0; i < N; ++i) {
        float db = md.spectrum[(size_t)i];
        float n = juce::jlimit(0.0f, 1.0f, (db - dbMin) / (dbMax - dbMin));
        int bx = xOff + i * (barW + barGap);
        int bH = std::max(0, (int)(n * barMaxH));
        int by = y + barMaxH - bH;

        if (bH > 0) {
            // Gradient: blue at bottom → purple at top
            juce::ColourGradient grad(C::blue.withAlpha(0.85f), (float)bx, (float)(y + barMaxH),
                                       C::purple, (float)bx, (float)y, false);
            g.setGradientFill(grad);
            g.fillRect((float)bx, (float)by, (float)barW, (float)bH);
        }
    }

    // Frequency axis labels
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    struct FreqLabel { const char* text; double freq; };
    FreqLabel labels[] = { {"50",50}, {"100",100}, {"250",250}, {"500",500},
                           {"1k",1000}, {"2k",2000}, {"5k",5000}, {"10k",10000}, {"20k",20000} };
    double logMin = std::log2(20.0), logMax = std::log2(20000.0);
    for (auto& fl : labels) {
        double logF = std::log2(fl.freq);
        double normPos = (logF - logMin) / (logMax - logMin);
        int lx = xOff + (int)(normPos * totalW);
        g.drawText(fl.text, lx - 12, y + barMaxH + 1, 24, 12, juce::Justification::centred);
    }
}

void EchoJayEditor::paintCapturesPanel(juce::Graphics& g, juce::Rectangle<int> area)
{
    auto snaps = processorRef.getSnapshots();
    if (snaps.empty()) return;

    int y = area.getY();
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.drawText("CAPTURES", area.getX(), y, 100, 14, juce::Justification::centredLeft);
    y += 16;

    int show = std::min((int)snaps.size(), 4);
    for (int i = (int)snaps.size() - show; i < (int)snaps.size(); ++i) {
        auto& s = snaps[i];
        auto info = s.name + " | " + channelTypeNames[(int)s.channelType] + " | " +
            juce::String(s.averagedData.integrated, 1) + " LUFS | " +
            juce::String(s.durationSeconds, 1) + "s";
        g.setColour(C::bg3);
        g.fillRoundedRectangle((float)area.getX(), (float)y, (float)area.getWidth(), 22.0f, 4.0f);
        g.setColour(C::text2);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(info, area.getX() + 8, y, area.getWidth() - 16, 22, juce::Justification::centredLeft);
        y += 26;
    }
}

void EchoJayEditor::paintWaveformPanel(juce::Graphics& g, juce::Rectangle<int> area)
{
    drawPanel(g, area, "WAVEFORM", C::blue2);

    int x = area.getX() + 14, y = area.getY() + 26;
    int w = area.getWidth() - 28, h = area.getHeight() - 34;

    if (w < 10 || h < 10) return;

    // Get waveform data: use frozen snapshot if available, otherwise live
    auto& recorder = processorRef.getWaveformRecorder();
    std::vector<WaveformRecorder::ThumbnailPoint> waveform;

    if (waveformFrozen && !frozenWaveform.empty())
        waveform = frozenWaveform;
    else if (recorder.isRecording() || recorder.getRecordedSampleCount() > 0)
        waveform = recorder.getThumbnail();

    if (waveform.empty())
    {
        g.setColour(C::text3.withAlpha(0.3f));
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText("Capture audio to see waveform", x, y, w, h, juce::Justification::centred);
        return;
    }

    int numPts = (int)waveform.size();

    // Draw waveform — centre line
    float centreY = (float)y + (float)h * 0.5f;
    float halfH = (float)h * 0.45f;

    // Centre line
    g.setColour(C::border2);
    g.drawHorizontalLine((int)centreY, (float)x, (float)(x + w));

    // Draw waveform bars
    float pxPerPt = (float)w / (float)numPts;

    // Playback position indicator
    float playFrac = -1.0f;
    if (isPlayingBack && recorder.getRecordedSampleCount() > 0)
        playFrac = (float)playbackPosition / (float)recorder.getRecordedSampleCount();

    for (int i = 0; i < numPts; ++i)
    {
        float px = (float)x + (float)i * pxPerPt;
        float barW = std::max(1.0f, pxPerPt - 0.5f);

        float minV = waveform[(size_t)i].minVal;
        float maxV = waveform[(size_t)i].maxVal;

        float top = centreY - maxV * halfH;
        float bot = centreY - minV * halfH;
        float barH = std::max(1.0f, bot - top);

        // Colour: blue->purple gradient, dim if past playback cursor
        float frac = (float)i / (float)numPts;
        juce::Colour barCol = C::blue.interpolatedWith(C::purple, frac);

        if (playFrac >= 0.0f && frac > playFrac)
            barCol = barCol.withAlpha(0.25f);
        else if (waveformFrozen)
            barCol = barCol.withAlpha(0.7f);
        else
            barCol = barCol.withAlpha(0.85f);

        g.setColour(barCol);
        g.fillRect(px, top, barW, barH);
    }

    // Playback cursor line
    if (playFrac >= 0.0f && playFrac <= 1.0f)
    {
        float cursorX = (float)x + playFrac * (float)w;
        g.setColour(C::green);
        g.drawVerticalLine((int)cursorX, (float)y, (float)(y + h));
    }

    // Recording indicator — pulsing red dot
    if (recorder.isRecording())
    {
        float pulse = 0.5f + 0.5f * std::sin((float)juce::Time::currentTimeMillis() * 0.005f);
        g.setColour(C::red.withAlpha(0.5f + 0.5f * pulse));
        g.fillEllipse((float)(x + w - 12), (float)(y + 2), 8.0f, 8.0f);
    }

    // Duration label bottom right
    float duration = waveformFrozen ? recorder.getRecordedDuration() : recorder.getRecordedDuration();
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.drawText(juce::String::formatted("%d:%02d", (int)duration / 60, (int)duration % 60),
               x + w - 40, y + h - 14, 40, 14, juce::Justification::centredRight);
}

// ============================================================================
// Playback
// ============================================================================

void EchoJayEditor::startPlayback()
{
    auto& recorder = processorRef.getWaveformRecorder();
    if (recorder.getRecordedSampleCount() <= 0) return;

    auto savedPath = recorder.getLastSavedPath();
    if (savedPath.isEmpty()) return;

    juce::File wavFile(savedPath);
    if (!wavFile.existsAsFile()) return;

    // Use JUCE's system audio playback via a simple command
    // On macOS: afplay, on Windows: PowerShell, on Linux: aplay
    // This keeps it simple without needing a full audio device setup
#if JUCE_MAC
    juce::String cmd = "afplay \"" + wavFile.getFullPathName() + "\" &";
    std::system(cmd.toRawUTF8());
#elif JUCE_WINDOWS
    juce::String cmd = "powershell -c \"(New-Object Media.SoundPlayer '" + wavFile.getFullPathName() + "').PlaySync()\" &";
    std::system(cmd.toRawUTF8());
#endif

    isPlayingBack = true;
    playbackPosition = 0;
    playbackBtn.setButtonText("\xe2\x96\xa0");  // ■ stop square
    playbackBtn.setColour(juce::TextButton::textColourOffId, C::red);
    playbackBtn.setColour(juce::TextButton::textColourOnId, C::red);

    // Estimate playback end (can't track external process precisely, but approximate)
    float durationMs = recorder.getRecordedDuration() * 1000.0f;
    juce::Timer::callAfterDelay((int)durationMs + 500, [this]() {
        stopPlayback();
    });
}

void EchoJayEditor::stopPlayback()
{
    if (!isPlayingBack) return;
    isPlayingBack = false;
    playbackPosition = 0;
    stopChatPlayback();

    playbackBtn.setButtonText("\xe2\x96\xb6");  // play triangle
    playbackBtn.setColour(juce::TextButton::textColourOffId, C::green);
    playbackBtn.setColour(juce::TextButton::textColourOnId, C::green);
}

// ============================================================================
// Paint
// ============================================================================

void EchoJayEditor::paint(juce::Graphics& g)
{
    auto bounds = juce::Rectangle<int>(0, 0, 900, 580);
    g.fillAll(C::bg);

    // === Login Screen ===
    if (currentScreen == Screen::Login) {
        int cx = bounds.getCentreX(), cy = bounds.getCentreY() - 40;
        g.setGradientFill(juce::ColourGradient(
            C::blue.withAlpha(0.06f), (float)cx, (float)cy,
            juce::Colours::transparentBlack, (float)cx, (float)(cy + 300), true));
        g.fillRect(bounds);
        EchoJayLookAndFeel::drawGrainOverlay(g, bounds, 0.015f);
        return;
    }

    // === Main Screen ===
    int topH = 32;
    int chatW = juce::jlimit(280, 420, bounds.getWidth() * 35 / 100);
    int mW = bounds.getWidth() - chatW;

    // Top bar background
    g.setColour(C::bg2);
    g.fillRect(0, 0, bounds.getWidth(), topH);
    g.setColour(C::border);
    g.drawHorizontalLine(topH - 1, 0.0f, (float)bounds.getWidth());
    EchoJayLookAndFeel::drawLogo(g, juce::Rectangle<float>(12, 0, 70, (float)topH), 18.0f);
    
    // Tier badge next to logo
    if (api.isLoggedIn())
    {
        EchoJayLookAndFeel::drawTierBadge(g, 82, (topH - 16) / 2, api.getUserInfo().tier);
    }
    EchoJayLookAndFeel::drawGrainOverlay(g, juce::Rectangle<int>(0, 0, bounds.getWidth(), topH), 0.015f);

    // Vertical divider
    g.setColour(C::border);
    g.drawVerticalLine(mW, (float)topH, (float)bounds.getHeight());

    // === Meter Panel Content ===
    int pad = 10;
    int contentY = topH + 6;
    int contentW = mW - pad * 2;

    if (currentView == View::Compare) {
        auto cArea = juce::Rectangle<int>(pad, topH + 4, contentW, bounds.getHeight() - topH - 16);
        paintCompareView(g, cArea);
    }
    else if (currentView == View::Settings) {
        auto sArea = juce::Rectangle<int>(pad, topH + 12, contentW, bounds.getHeight() - topH - 24);
        paintSettingsView(g, sArea);
    }
    else
    {
        // Decide which meter data to show:
        // - While capturing or idle: live meters
        // - When complete (frozen): show the capture snapshot data
        auto state = processorRef.getCaptureState();
        auto md = (state == CaptureState::Complete && waveformFrozen)
                      ? processorRef.getLatestSnapshot().averagedData
                      : processorRef.getMeterEngine().getMeterData();
        int secGap = 8;
        int y = contentY;

        // WAVEFORM panel (above meters — shown when recording or has data)
        auto& recorder = processorRef.getWaveformRecorder();
        bool hasWaveform = recorder.getRecordedSampleCount() > 0 || !frozenWaveform.empty();
        if (hasWaveform)
        {
            int waveH = 72;
            paintWaveformPanel(g, { pad, y, contentW, waveH });
            y += waveH + secGap;
        }

        // LOUDNESS panel
        int loudH = 98;
        paintLoudnessPanel(g, { pad, y, contentW, loudH }, md);
        y += loudH + secGap;

        // LEVELS + STEREO IMAGE side by side
        int levelsW = (contentW - secGap) * 50 / 100;
        int stereoW = contentW - levelsW - secGap;
        int remainH = bounds.getHeight() - y - 160;
        int levStereoH = std::min(300, hasWaveform ? (remainH * 45 / 100) : (remainH * 50 / 100));
        levStereoH = std::max(210, levStereoH);
        paintLevelsPanel(g, { pad, y, levelsW, levStereoH }, md);
        paintStereoPanel(g, { pad + levelsW + secGap, y, stereoW, levStereoH }, md);
        y += levStereoH + secGap;

        // SPECTRUM panel — extends to bottom
        int specH = std::max(80, bounds.getHeight() - y - pad);
        paintSpectrumPanel(g, { pad, y, contentW, specH }, md);
    }

    // === Chat Panel ===
    int chatX = mW + 1;

    // Chat header — "AI ASSISTANT" bold, usage count right
    g.setColour(C::bg2);
    g.fillRect(chatX, topH, chatW, 32);
    g.setColour(C::border);
    g.drawHorizontalLine(topH + 31, (float)chatX, (float)(chatX + chatW));

    g.setColour(C::text2);
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    g.drawText("AI ASSISTANT", chatX + 14, topH, chatW, 32, juce::Justification::centredLeft);

    if (api.isLoggedIn())
    {
        int remaining = api.getRemainingMessages();
        int limit = api.getUserInfo().messageLimit;
        int used = limit - remaining;
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.drawText(juce::String(used) + "/" + juce::String(limit),
                   chatX + chatW - 56, topH, 44, 32, juce::Justification::centredRight);
    }

    // Chat messages
    int chatTop2 = topH + 32;
    int chatBottomEdge = bounds.getHeight() - 58;
    int scrollOffset = chatScroll.getViewPositionY();
    int avatarSize = 24;
    int maxBubbleW = chatW - avatarSize - 24;

    // Draw messages with scroll offset
    g.saveState();
    g.reduceClipRegion(chatX, chatTop2, chatW, chatBottomEdge - chatTop2);
    // Track waveform card positions for overlay buttons
    activeWavePlayBtns = 0;

    int msgY = 8 - scrollOffset;
    for (auto& msg : chatMessages) {
        bool isUser = (msg.role == "user");
        
        juce::AttributedString as;
        as.append(msg.content, juce::Font(juce::FontOptions(12.0f)),
                  isUser ? C::text : C::text2);
        juce::TextLayout layout;
        layout.createLayout(as, (float)(maxBubbleW - 20));
        int textH = (int)layout.getHeight() + 20;
        
        // Extra height for waveform card
        int waveCardH = 0;
        if (msg.hasWaveform && !msg.waveform.empty())
            waveCardH = 36; // play button + waveform only
        
        int tH = textH + waveCardH;
        int drawY = chatTop2 + msgY;

        if (drawY + tH > chatTop2 - 50 && drawY < chatBottomEdge + 50)
        {
            if (isUser)
            {
                int avX = chatX + chatW - avatarSize - 4;
                int avY = drawY + 2;
                g.setColour(C::bg4);
                g.fillEllipse((float)avX, (float)avY, (float)avatarSize, (float)avatarSize);
                g.setColour(C::text3);
                g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                g.drawText("U", avX, avY, avatarSize, avatarSize, juce::Justification::centred);
                
                int bubbleX = chatX + chatW - maxBubbleW - 8;
                int bubbleW = maxBubbleW - avatarSize + 4;
                g.setColour(C::bg4);
                g.fillRoundedRectangle((float)bubbleX, (float)drawY, (float)bubbleW, (float)tH, 10.0f);
                layout.draw(g, { (float)(bubbleX + 10), (float)(drawY + 10), (float)(bubbleW - 20), (float)(textH - 20) });
                
                // Waveform card — play button + waveform only
                if (msg.hasWaveform && !msg.waveform.empty())
                {
                    int cardX = bubbleX + 8;
                    int cardY = drawY + textH - 2;
                    int cardW = bubbleW - 16;
                    int cardH = waveCardH - 4;
                    
                    // Card background
                    g.setColour(C::bg3);
                    g.fillRoundedRectangle((float)cardX, (float)cardY, (float)cardW, (float)cardH, 6.0f);
                    
                    // Play/Stop button circle
                    int playBtnSize = 22;
                    int playX = cardX + 6;
                    int playY = cardY + (cardH - playBtnSize) / 2;
                    bool isPlaying = (msg.wavFilePath.isNotEmpty() && currentlyPlayingChatWav == msg.wavFilePath);
                    
                    g.setColour(isPlaying ? C::red.withAlpha(0.7f) : C::purple.withAlpha(0.6f));
                    g.fillEllipse((float)playX, (float)playY, (float)playBtnSize, (float)playBtnSize);
                    g.setColour(juce::Colours::white);
                    
                    if (isPlaying)
                    {
                        // Stop square
                        g.fillRect((float)playX + 7.0f, (float)playY + 7.0f, 8.0f, 8.0f);
                    }
                    else
                    {
                        // Play triangle
                        juce::Path tri;
                        float triX = (float)playX + 8.0f, triY2 = (float)playY + 5.0f;
                        tri.addTriangle(triX, triY2, triX, triY2 + 12.0f, triX + 8.0f, triY2 + 6.0f);
                        g.fillPath(tri);
                    }
                    
                    // Position overlay button on this card
                    if (msg.wavFilePath.isNotEmpty() && activeWavePlayBtns < kMaxWavePlayBtns)
                    {
                        int idx = activeWavePlayBtns++;
                        wavePlayPaths[(size_t)idx] = msg.wavFilePath;
                        wavePlayDurations[(size_t)idx] = msg.durationSeconds;
                        wavePlayOverlays[(size_t)idx].setBounds(cardX, cardY, cardW, cardH);
                        
                        // Only show if within the visible chat scroll area
                        auto scrollBounds = chatScroll.getBounds();
                        bool inView = cardY >= scrollBounds.getY() && (cardY + cardH) <= scrollBounds.getBottom();
                        wavePlayOverlays[(size_t)idx].setVisible(inView);
                        if (inView) wavePlayOverlays[(size_t)idx].toFront(false);
                    }
                    
                    // Waveform fills the rest
                    int wfX = playX + playBtnSize + 6;
                    int wfY = cardY + 4;
                    int wfW = cardX + cardW - wfX - 6;
                    int wfH = cardH - 8;
                    int numPts = (int)msg.waveform.size();
                    if (numPts > 0 && wfW > 0)
                    {
                        float pxPerPt = (float)wfW / (float)numPts;
                        float centreY2 = (float)wfY + (float)wfH * 0.5f;
                        float halfH2 = (float)wfH * 0.45f;
                        for (int i = 0; i < numPts; ++i)
                        {
                            float px = (float)wfX + (float)i * pxPerPt;
                            float top2 = centreY2 - msg.waveform[(size_t)i].maxVal * halfH2;
                            float bot2 = centreY2 - msg.waveform[(size_t)i].minVal * halfH2;
                            float bH2 = std::max(1.0f, bot2 - top2);
                            float frac = (float)i / (float)numPts;
                            
                            // Dim waveform past the playback cursor
                            float alpha = 0.7f;
                            if (isPlaying && chatPlaybackDuration > 0)
                            {
                                double elapsed = (juce::Time::getMillisecondCounterHiRes() - chatPlaybackStartTime) / 1000.0 + chatPlaybackOffset;
                                float playFrac = (float)(elapsed / chatPlaybackDuration);
                                if (frac > playFrac) alpha = 0.25f;
                            }
                            
                            g.setColour(C::blue.interpolatedWith(C::purple, frac).withAlpha(alpha));
                            g.fillRect(px, top2, std::max(1.0f, pxPerPt - 0.5f), bH2);
                        }
                        
                        // Playback cursor line
                        if (isPlaying && chatPlaybackDuration > 0)
                        {
                            double elapsed = (juce::Time::getMillisecondCounterHiRes() - chatPlaybackStartTime) / 1000.0 + chatPlaybackOffset;
                            float playFrac = juce::jlimit(0.0f, 1.0f, (float)(elapsed / chatPlaybackDuration));
                            float cursorX = (float)wfX + playFrac * (float)wfW;
                            g.setColour(juce::Colours::white);
                            g.drawVerticalLine((int)cursorX, (float)wfY, (float)(wfY + wfH));
                        }
                    }
                }
            }
            else
            {
                int avX = chatX + 6;
                int avY = drawY + 2;
                g.setColour(C::purple);
                g.fillEllipse((float)avX, (float)avY, (float)avatarSize, (float)avatarSize);
                g.setColour(juce::Colours::white);
                g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                g.drawText("E", avX, avY, avatarSize, avatarSize, juce::Justification::centred);
                
                int bubbleX = avX + avatarSize + 6;
                int bubbleW = chatW - avatarSize - 20;
                g.setColour(C::bg3);
                g.fillRoundedRectangle((float)bubbleX, (float)drawY, (float)bubbleW, (float)tH, 10.0f);
                layout.draw(g, { (float)(bubbleX + 10), (float)(drawY + 10), (float)(bubbleW - 20), (float)(tH - 20) });
            }
        }
        msgY += tH + 10;
    }
    
    if (chatLoading) {
        int drawY = chatTop2 + msgY;
        if (drawY < chatBottomEdge) {
            // Pulsing dots animation
            int avX = chatX + 6;
            g.setColour(C::purple.withAlpha(0.5f));
            g.fillEllipse((float)avX, (float)drawY + 2, (float)avatarSize, (float)avatarSize);
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawText("E", avX, drawY + 2, avatarSize, avatarSize, juce::Justification::centred);
            
            g.setColour(C::text3);
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText("Analysing...", avX + avatarSize + 12, drawY + 4, 100, 20, juce::Justification::centredLeft);
        }
    }

    g.restoreState();
    
    // Hide unused overlay buttons
    for (int i = activeWavePlayBtns; i < kMaxWavePlayBtns; ++i)
        wavePlayOverlays[(size_t)i].setVisible(false);
}

// ============================================================================
// Resized
// ============================================================================

void EchoJayEditor::resized()
{
    // Scale the entire UI proportionally — internal layout fixed at 900x580
    const float baseW = 900.0f, baseH = 580.0f;
    float scaleX = (float)getWidth() / baseW;
    float scaleY = (float)getHeight() / baseH;
    float scale = std::min(scaleX, scaleY); // uniform scale, preserving aspect ratio
    setTransform(juce::AffineTransform::scale(scale));
    
    // All layout below uses the fixed 900x580 coordinate space
    auto b = juce::Rectangle<int>(0, 0, (int)baseW, (int)baseH);

    if (currentScreen == Screen::Login) {
        int formW = juce::jmin(340, b.getWidth() - 60);
        int formX = (b.getWidth() - formW) / 2;
        int y = b.getHeight() / 2 - 120;
        loginTitle.setBounds(formX, y, formW, 40); y += 44;
        loginSubtitle.setBounds(formX, y, formW, 24); y += 40;
        emailInput.setBounds(formX, y, formW, 36); y += 44;
        passwordInput.setBounds(formX, y, formW, 36); y += 44;
        loginBtn.setBounds(formX, y, formW, 38); y += 48;
        loginErrorLabel.setBounds(formX, y, formW, 20);
        return;
    }

    int topH = 32;
    int chatW = juce::jlimit(240, 380, b.getWidth() * 32 / 100);
    int mW = b.getWidth() - chatW;

    // === Single top bar row ===
    int ty = 4, bh = 24;
    int tx = 122;
    channelTypeBox.setBounds(tx, ty, 100, bh); tx += 104;
    genreBox.setBounds(tx, ty, 80, bh); tx += 84;
    captureBtn.setBounds(tx, ty, 64, bh); tx += 68;
    compareBtn.setBounds(tx, ty, 64, bh); tx += 68;
    settingsBtn.setBounds(tx, ty, 52, bh); tx += 56;
    scanBtn.setBounds(tx, ty, 78, bh); tx += 82;
    
    // Detected label — fixed position in top-right, right-aligned
    int detW = 140;
    detectedLabel.setBounds(900 - detW - 8, ty, detW, bh);
    detectedLabel.setJustificationType(juce::Justification::centredRight);
    detectedLabel.setVisible(false);

    // Row 2 labels hidden — info lives elsewhere now
    statusLabel.setBounds(0, -20, 1, 1);
    durationLabel.setBounds(0, -20, 1, 1);
    passLabel.setBounds(0, -20, 1, 1);
    playbackBtn.setBounds(0, -20, 1, 1);
    wavSavedLabel.setBounds(0, -20, 1, 1);
    userLabel.setBounds(0, -20, 1, 1);
    usageLabel.setBounds(0, -20, 1, 1);

    // Chat input — 2-line height, Send centred vertically
    int inH = 52; // ~2 lines of 13px font
    int sendW = 56;
    int sendH = 30;
    int chatPadL = -20; // 20px past the panel divider
    int inputY = b.getHeight() - inH - 10;
    chatInput.setBounds(mW + chatPadL, inputY, chatW - sendW - chatPadL - 4, inH);
    int sendY = inputY + (inH - sendH) / 2; // vertically centred
    chatSendBtn.setBounds(mW + chatW - sendW - 2, sendY, sendW, sendH);
    chatScroll.setBounds(mW + 2, topH + 32, chatW - 4, b.getHeight() - topH - 32 - inH - 4);
    chatContent.setSize(chatW - 16, std::max(100, chatScroll.getHeight()));

    // Logout button lives in Settings view now (positioned there)
    if (currentView != View::Settings)
        logoutBtn.setVisible(false);

    // Compare controls — aligned with paint area starting at (pad, topH+4)
    if (currentView == View::Compare) {
        int cPad = 10;
        int cW = mW - cPad * 2 - 24; // 24px less on right
        int cCardW = (cW - 12) / 2;
        int cy2 = topH + 4;
        
        // Preset row
        presetBox.setBounds(cPad, cy2, cW - 180, 22);
        savePresetBtn.setBounds(cPad + cW - 174, cy2, 84, 22);
        deletePresetBtn.setBounds(cPad + cW - 84, cy2, 80, 22);
        cy2 += 26;
        
        cy2 += 82 + 4;
        
        // Pass dropdowns — full width, no play buttons
        compareSlotABox.setBounds(cPad, cy2, cCardW - 4, 22);
        compareSlotBBox.setBounds(cPad + cCardW + 10, cy2, cCardW - 3, 22);
        playSlotABtn.setVisible(false);
        playSlotBBtn.setVisible(false);
        
        // Click catcher covers the entire area below dropdowns (waveform bars + cards)
        int wfBarY = cy2 + 26;
        int catcherH = 580 - wfBarY - 40;
        compareClickCatcher.setBounds(cPad, wfBarY, cW, catcherH);
        compareClickCatcher.setVisible(true);
        compareClickCatcher.toFront(false);
        
        // AI Compare button centred at bottom
        int btnY = 580 - 36;
        int mwHalf = mW / 2;
        aiCompareBtn.setBounds(mwHalf - 65, btnY, 100, 26);
    }

    // Settings layout — consistent Y tracking matching paintSettingsView
    if (currentView == View::Settings) {
        int sx = 20, sy = topH + 18, sw = juce::jmin(560, mW - 40);
        int fh = 30, labelGap = 18; // label height + space before field
        
        // YOUR NAME
        sy += labelGap;
        settingsName.setBounds(sx, sy, sw, fh); sy += fh + 8;
        
        // DAW(S)
        sy += labelGap;
        int bw = (sw - 20) / 3, bh2 = 26, bx = sx;
        for (int i = 0; i < 11; ++i) {
            dawButtons[(size_t)i].setBounds(bx, sy, bw, bh2);
            bx += bw + 4;
            if (bx + bw > sx + sw) { bx = sx; sy += bh2 + 3; }
        }
        sy += bh2 + 8;
        
        // EXPERIENCE LEVEL
        sy += labelGap;
        settingsExpLevel.setBounds(sx, sy, sw, fh); sy += fh + 8;
        
        // MONITORS
        sy += labelGap;
        settingsMonitors.setBounds(sx, sy, sw, fh); sy += fh + 8;
        
        // HEADPHONES
        sy += labelGap;
        settingsHeadphones.setBounds(sx, sy, sw, fh); sy += fh + 8;
        
        // GENRES
        sy += labelGap;
        settingsGenres.setBounds(sx, sy, sw, fh); sy += fh + 8;
        
        // PLUGINS — fills remaining space but guarantees save row is visible
        sy += labelGap;
        int saveRowY = b.getHeight() - 44; // 30px button + 14px margin
        int plugH = juce::jmax(40, saveRowY - sy - 8);
        settingsPlugins.setBounds(sx, sy, sw, plugH);
        
        // Save + Logout row — always pinned to bottom
        saveSettingsBtn.setBounds(sx, saveRowY, 100, 30);
        settingsSavedLabel.setBounds(sx + 110, saveRowY, 150, 30);
        logoutBtn.setBounds(sx + sw - 80, saveRowY, 80, 30);
        logoutBtn.setVisible(true);
    }
}

// ============================================================================
// Timer
// ============================================================================

void EchoJayEditor::timerCallback()
{
    auto state = processorRef.getCaptureState();
    if (state == CaptureState::Capturing) {
        captureBtn.setButtonText("Stop");
        captureBtn.setColour(juce::TextButton::buttonColourId, C::red);
        float dur = processorRef.getCaptureDuration();
        durationLabel.setText(juce::String::formatted("%d:%02d", (int)dur / 60, (int)dur % 60), juce::dontSendNotification);
        statusLabel.setText("Capturing...", juce::dontSendNotification);
        waveformFrozen = false; // live while capturing
    } else {
        captureBtn.setButtonText("Capture");
        captureBtn.setColour(juce::TextButton::buttonColourId, C::blue);
        statusLabel.setText(state == CaptureState::Complete ? "Complete" : "", juce::dontSendNotification);

        // Freeze waveform on capture complete (once)
        if (state == CaptureState::Complete && !waveformFrozen)
        {
            frozenWaveform = processorRef.getWaveformRecorder().getThumbnail();
            waveformFrozen = true;
            captureWasSilent = false; // reset: need silence before unfreeze
        }

        // Track if audio has gone silent since capture completed
        if (state == CaptureState::Complete && waveformFrozen && processorRef.isAudioSilent())
            captureWasSilent = true;

        // Unfreeze only after audio went silent AND then resumed
        // This prevents immediate unfreeze when Stop button is pressed while DAW plays
        if (state == CaptureState::Complete && waveformFrozen && captureWasSilent && !processorRef.isAudioSilent())
        {
            processorRef.resetCapture();
            waveformFrozen = false;
            frozenWaveform.clear();
            wavSavedLabel.setText("", juce::dontSendNotification);
            captureWasSilent = false;
        }
    }

    // Show/hide playback button based on whether we have a saved WAV
    {
        auto savedPath = processorRef.getWaveformRecorder().getLastSavedPath();
        bool hasWav = savedPath.isNotEmpty() && juce::File(savedPath).existsAsFile();
        playbackBtn.setVisible(hasWav && currentView == View::Meters);

        if (hasWav && wavSavedLabel.getText().isEmpty())
        {
            juce::File f(savedPath);
            wavSavedLabel.setText("Saved: " + f.getFileName(), juce::dontSendNotification);
        }
    }

    // Approximate playback position for cursor animation
    if (isPlayingBack)
    {
        auto& rec = processorRef.getWaveformRecorder();
        float durS = rec.getRecordedDuration();
        if (durS > 0)
        {
            // Increment position at roughly real time (30fps timer)
            playbackPosition += (int)(rec.getRecordedSampleRate() / 30.0);
            if (playbackPosition >= rec.getRecordedSampleCount())
                stopPlayback();
        }
    }

    detectedLabel.setVisible(false);

    int passes = processorRef.getSnapshotCount();
    passLabel.setText(passes > 0 ? juce::String(passes) + " pass" + (passes > 1 ? "es" : "") : "", juce::dontSendNotification);

    auto& sc = processorRef.getPluginScanner();
    if (sc.isScanning()) {
        scanBtn.setButtonText("Scanning " + juce::String((int)(sc.getProgress() * 100)) + "%");
        scanBtn.setEnabled(false);
    } else {
        int c = sc.getPluginCount();
        if (c > 0 && c != scannedPluginCount) {
            scannedPluginCount = c;
            api.updatePluginsFromScanner(sc.getPluginNamesString());
            if (currentView == View::Settings)
                settingsPlugins.setText(api.getUserSettings().plugins, false);
            // Only auto-save if we've fetched settings from server first
            // (prevents overwriting web app data with empty local cache)
            if (settingsFetched)
                api.saveUserSettings(api.getUserSettings(), nullptr);
        }
        scanBtn.setButtonText(c > 0 ? juce::String(c) + " Plugins" : "Scan Plugins");
        scanBtn.setEnabled(true);
    }

    if (processorRef.shouldAutoFeedback()) {
        auto snap = processorRef.getLatestSnapshot();
        if (snap.durationSeconds >= 1.0f)
            requestAIFeedback(snap);
        
        // Refresh compare dropdowns if we're on the compare view
        if (currentView == View::Compare)
        {
            int prevSelA = compareSlotABox.getSelectedId();
            int prevSelB = compareSlotBBox.getSelectedId();
            compareSlotABox.clear(juce::dontSendNotification);
            compareSlotBBox.clear(juce::dontSendNotification);
            auto snaps2 = processorRef.getSnapshots();
            for (int i = 0; i < (int)snaps2.size(); ++i) {
                compareSlotABox.addItem(snaps2[(size_t)i].name.substring(0, 30), i + 1);
                compareSlotBBox.addItem(snaps2[(size_t)i].name.substring(0, 30), i + 1);
            }
            auto refs2 = processorRef.getReferenceAnalyser().getReferences();
            int refOff = (int)snaps2.size() + 100;
            for (int i = 0; i < (int)refs2.size(); ++i) {
                compareSlotABox.addItem(refs2[(size_t)i].name.substring(0, 25) + " (Ref)", refOff + i);
                compareSlotBBox.addItem(refs2[(size_t)i].name.substring(0, 25) + " (Ref)", refOff + i);
            }
            // Keep previous selections or auto-select the new pass
            if (prevSelA > 0) compareSlotABox.setSelectedId(prevSelA, juce::dontSendNotification);
            else if (snaps2.size() > 0) compareSlotABox.setSelectedId((int)snaps2.size(), juce::dontSendNotification);
            if (prevSelB > 0) compareSlotBBox.setSelectedId(prevSelB, juce::dontSendNotification);
        }
    }

    if (currentScreen == Screen::Main && api.isLoggedIn()) {
        auto info = api.getUserInfo();
        int remaining = api.getRemainingMessages();
        usageLabel.setText(juce::String(remaining) + "/" + juce::String(info.messageLimit) + " messages left",
                          juce::dontSendNotification);
        
        // Periodic refresh every 60 seconds to sync usage with web app
        refreshCounter++;
        if (refreshCounter >= 1800) // 30fps * 60s
        {
            refreshCounter = 0;
            api.refreshUserInfo(nullptr);
        }
        
        // If displayName is still empty but we have settings name, use that
        if (info.displayName.isEmpty() && api.getUserSettings().name.isNotEmpty())
        {
            // Can't modify via getUserInfo() — trigger a refresh instead
            api.refreshUserInfo(nullptr);
        }
    }

    if (currentScreen == Screen::Main && !api.isLoggedIn())
        showLoginScreen();

    // Update chat scroll content size and auto-scroll to bottom
    if (currentScreen == Screen::Main && !chatMessages.empty())
    {
        int chatW2 = chatScroll.getWidth();
        int avatarSz = 24;
        int maxBW = chatW2 - avatarSz - 20;
        int totalH = 4;
        for (auto& msg : chatMessages) {
            juce::AttributedString as;
            as.append(msg.content, juce::Font(juce::FontOptions(12.0f)), C::text);
            juce::TextLayout layout;
            layout.createLayout(as, (float)(maxBW - 20));
            int h = (int)layout.getHeight() + 20 + 8; // text padding + gap between bubbles
            if (msg.hasWaveform && !msg.waveform.empty()) h += 36;
            totalH += h;
        }
        if (chatLoading) totalH += 30;
        
        int visH = chatScroll.getHeight();
        if (chatContent.getHeight() != std::max(visH, totalH))
        {
            chatContent.setSize(chatW2 - 8, std::max(visH, totalH));
            if (totalH > visH)
                chatScroll.setViewPosition(0, totalH - visH);
        }
    }

    // Auto-refresh compare view when references change
    if (currentView == View::Compare)
    {
        int curRefCount = processorRef.getReferenceAnalyser().getReferenceCount();
        if (curRefCount != lastRefCount)
        {
            lastRefCount = curRefCount;
            // Refresh dropdowns
            int prevA = compareSlotABox.getSelectedId();
            int prevB = compareSlotBBox.getSelectedId();
            compareSlotABox.clear(juce::dontSendNotification);
            compareSlotBBox.clear(juce::dontSendNotification);
            auto snaps2 = processorRef.getSnapshots();
            for (int i = 0; i < (int)snaps2.size(); ++i) {
                compareSlotABox.addItem(snaps2[(size_t)i].name.substring(0, 30), i + 1);
                compareSlotBBox.addItem(snaps2[(size_t)i].name.substring(0, 30), i + 1);
            }
            auto refs2 = processorRef.getReferenceAnalyser().getReferences();
            int refOff = (int)snaps2.size() + 100;
            for (int i = 0; i < (int)refs2.size(); ++i) {
                compareSlotABox.addItem(refs2[(size_t)i].name.substring(0, 25) + " (Ref)", refOff + i);
                compareSlotBBox.addItem(refs2[(size_t)i].name.substring(0, 25) + " (Ref)", refOff + i);
            }
            if (prevA > 0) compareSlotABox.setSelectedId(prevA, juce::dontSendNotification);
            if (prevB > 0) compareSlotBBox.setSelectedId(prevB, juce::dontSendNotification);
            // Auto-select new reference in slot B if nothing was selected
            if (compareSlotBBox.getSelectedId() == 0 && refs2.size() > 0)
                compareSlotBBox.setSelectedId(refOff + (int)refs2.size() - 1, juce::dontSendNotification);
        }
    }

    // Position compare waveform bar overlays
    if (currentView == View::Compare && !compareWavePositions.empty())
    {
        activeWavePlayBtns = 0;
        for (auto& wp : compareWavePositions)
        {
            if (activeWavePlayBtns >= kMaxWavePlayBtns) break;
            int idx = activeWavePlayBtns++;
            wavePlayPaths[(size_t)idx] = wp.wavPath;
            wavePlayDurations[(size_t)idx] = wp.duration;
            wavePlayOverlays[(size_t)idx].setBounds(wp.bounds);
            wavePlayOverlays[(size_t)idx].setVisible(true);
            wavePlayOverlays[(size_t)idx].toFront(false);
        }
        for (int i = activeWavePlayBtns; i < kMaxWavePlayBtns; ++i)
        {
            wavePlayOverlays[(size_t)i].setVisible(false);
            wavePlayOverlays[(size_t)i].setBounds(-100, -100, 1, 1);
        }
    }
    else if (currentView == View::Compare)
    {
        for (int i = 0; i < kMaxWavePlayBtns; ++i)
        {
            wavePlayOverlays[(size_t)i].setVisible(false);
            wavePlayOverlays[(size_t)i].setBounds(-100, -100, 1, 1);
        }
    }
    else if (currentView != View::Meters)
    {
        for (int i = 0; i < kMaxWavePlayBtns; ++i)
            wavePlayOverlays[(size_t)i].setVisible(false);
    }

    repaint();
}

void EchoJayEditor::textEditorReturnKeyPressed(juce::TextEditor& ed)
{
    if (&ed == &chatInput) {
        auto t = chatInput.getText().trim();
        if (t.isNotEmpty()) sendChatMessage(t);
    }
}

void EchoJayEditor::sendChatMessage(const juce::String& msg)
{
    chatMessages.push_back({"user", msg});
    chatInput.clear();
    chatLoading = true;
    repaint();

    auto md = processorRef.getMeterEngine().getMeterData();
    auto ff = [](float v) -> juce::String { return v > -99 ? juce::String(v, 1) : "N/A"; };

    juce::String ctx = "\n\n[METER: " + processorRef.getEffectiveChannelName() + " (" + processorRef.getGenre() + ")] " +
        "Int " + ff(md.integrated) + " LUFS | Mom " + ff(md.momentary) + " | ST " + ff(md.shortTerm) +
        " | LRA " + juce::String(md.loudnessRange, 1) + " LU | RMS " + ff(md.rmsL) + "/" + ff(md.rmsR) +
        " | TP " + ff(md.truePeakL) + "/" + ff(md.truePeakR) + " | Crest " + juce::String(md.crestFactor, 1) +
        " | Width " + juce::String(md.width, 1) + "% | Corr " + juce::String(md.correlation, 2);

    chatRoles.add("user");
    chatContents.add(msg + ctx);

    auto sysPrompt = EchoJayAPI::buildSystemPrompt(
        processorRef.getEffectiveChannelName(), processorRef.getGenre(),
        processorRef.getPluginScanner().getPluginNamesString());

    api.sendChat(chatRoles, chatContents, sysPrompt,
        [this](const juce::String& reply, bool success) {
            chatLoading = false;
            if (success) {
                chatMessages.push_back({"assistant", reply});
                chatRoles.add("assistant");
                chatContents.add(reply);
            } else {
                chatMessages.push_back({"assistant", reply});
            }
            repaint();
        });
}

void EchoJayEditor::requestAIFeedback(const CaptureSnapshot& snap)
{
    auto ff = [](float v) -> juce::String { return v > -99 ? juce::String(v, 1) : "N/A"; };
    auto& d = snap.averagedData;
    juce::String ch = channelTypeNames[(int)snap.channelType];

    // Build chat message with waveform only — no text above the card
    juce::String captureMsg = "";

    ChatMsg cm;
    cm.role = "user";
    cm.content = captureMsg;
    cm.hasWaveform = !frozenWaveform.empty();
    cm.waveform = frozenWaveform;
    cm.durationSeconds = snap.durationSeconds;
    cm.lufs = snap.averagedData.integrated;
    auto wavPath = processorRef.getWaveformRecorder().getLastSavedPath();
    if (wavPath.isNotEmpty())
    {
        cm.wavFilename = juce::File(wavPath).getFileName();
        cm.wavFilePath = wavPath;
    }
    
    chatMessages.push_back(cm);
    chatLoading = true;
    repaint();

    int mins = (int)snap.durationSeconds / 60;
    int secs = (int)snap.durationSeconds % 60;
    juce::String durStr = juce::String(mins) + ":" + juce::String(secs).paddedLeft('0', 2);
    
    bool isFullMix = (ch == "Full Mix" || ch == "Master Bus");
    bool isIndividual = !isFullMix;
    
    juce::String meterCtx = "\n\n[" + (isFullMix ? "FULL TRACK" : ch.toUpperCase()) + " ANALYSIS: \"" + snap.name + "\" (" + durStr + ")]\n";
    
    if (isFullMix)
        meterCtx += "Loudness: Integrated " + ff(d.integrated) + " LUFS\n";
    // Don't send LUFS for individual channels
    
    meterCtx += "Dynamics: Avg Crest " + juce::String(d.crestFactor, 1) + " dB";
    
    // True peak threshold: 0 dBTP for individual channels (gain staging matters), 1.5 for full mix
    float tpThreshold = isIndividual ? 0.0f : 1.5f;
    if (d.truePeakL > tpThreshold || d.truePeakR > tpThreshold)
    {
        if (isIndividual)
            meterCtx += "\nTrue Peak: L " + ff(d.truePeakL) + " dBTP | R " + ff(d.truePeakR) + " dBTP — CLIPPING. Check gain staging. If the distortion is intentional that's fine, but flag it.";
        else
            meterCtx += "\nTrue Peak: L " + ff(d.truePeakL) + " dBTP | R " + ff(d.truePeakR) + " dBTP — clipping (could be intentional for character, ask before assuming it's a problem)";
    }
    
    // Stereo data — different rules for individual channels
    if (isFullMix)
    {
        if (d.width < 10.0f)
            meterCtx += "\nStereo: Width " + juce::String(d.width, 1) + "% — narrow";
        else if (d.width > 55.0f)
            meterCtx += "\nStereo: Width " + juce::String(d.width, 1) + "% — wide";
        if (d.correlation < 0.0f)
            meterCtx += "\nCorrelation: " + juce::String(d.correlation, 2) + " — PHASE ISSUES";
    }
    // For individual channels: don't send correlation, don't flag bass width
    // Only flag width if pads/overheads are unexpectedly mono
    else if (ch == "Synth Pad" || ch == "Overheads" || ch == "Orchestral" || ch == "Strings")
    {
        if (d.width < 5.0f)
            meterCtx += "\nStereo: Width " + juce::String(d.width, 1) + "% — unexpectedly mono for this type of element";
    }
    
    meterCtx += "\n";

    // Partial analysis warning
    if (snap.durationSeconds < 5.0f)
        meterCtx += "\n⚠ PARTIAL ANALYSIS: Only " + juce::String((int)snap.durationSeconds) + 
            "s captured. These readings may not represent the full track. Keep your review brief and suggest they capture more of the track for a better analysis.";
    else if (snap.durationSeconds < 15.0f)
        meterCtx += "\n(Note: " + juce::String((int)snap.durationSeconds) + "s captured)";

    // Smart auto-comparison with previous pass (Full Mix and buses only)
    auto snaps = processorRef.getSnapshots();
    bool isBusType = (ch == "Full Mix" || ch == "Master Bus" || ch == "Drum Bus" || 
                      ch == "Instrument Bus" || ch == "Music Bus" || ch == "Vocal Bus" || 
                      ch == "Guitar Bus" || ch == "Synth Bus");
    if (snaps.size() >= 2 && isBusType)
    {
        auto& prev = snaps[snaps.size() - 2];
        auto& pd = prev.averagedData;
        float lufsDiff = std::abs(d.integrated - pd.integrated);
        float crestDiff = std::abs(d.crestFactor - pd.crestFactor);
        float widthDiff = std::abs(d.width - pd.width);
        
        // Check if everything changed drastically — might be a different song
        if (lufsDiff > 6.0f && crestDiff > 5.0f)
        {
            meterCtx += "\n\n[PREVIOUS CAPTURE: " + prev.name + " — LUFS " + ff(pd.integrated) + 
                ", Crest " + juce::String(pd.crestFactor, 1) + "dB]";
            meterCtx += "\n⚠ The numbers have changed dramatically from the previous capture. Ask if this is a different song or section, because the comparison won't be meaningful if it is.";
        }
        // Only mention previous if there are meaningful changes
        else if (lufsDiff > 2.0f || crestDiff > 3.0f || widthDiff > 20.0f)
        {
            meterCtx += "\n\n[PREVIOUS CAPTURE: " + prev.name + " — LUFS " + ff(pd.integrated) + 
                ", Crest " + juce::String(pd.crestFactor, 1) + "dB]";
            meterCtx += "\nBriefly note what changed from the previous pass — but keep it to one sentence. Don't do a full comparison, that's what the Compare view is for.";
        }
        // If changes are tiny, don't send previous data at all
    }

    chatRoles.add("user");
    chatContents.add("Give me feedback on this capture.\n\n" + meterCtx);

    auto sysPrompt = EchoJayAPI::buildSystemPrompt(
        ch, processorRef.getGenre(),
        processorRef.getPluginScanner().getPluginNamesString());

    api.sendChat(chatRoles, chatContents, sysPrompt,
        [this](const juce::String& reply, bool success) {
            chatLoading = false;
            chatMessages.push_back({"assistant", reply});
            if (success) { chatRoles.add("assistant"); chatContents.add(reply); }
            repaint();
        });
}

void EchoJayEditor::layoutChatMessages() {}

// ============================================================================
// Reference Presets
// ============================================================================

juce::File EchoJayEditor::getPresetsFolder()
{
    auto folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                      .getChildFile("EchoJay").getChildFile("Presets");
    folder.createDirectory();
    return folder;
}

void EchoJayEditor::loadPresetList()
{
    int prevId = presetBox.getSelectedId();
    juce::String prevPath;
    if (prevId > 0 && prevId <= presetNames.size())
        prevPath = presetNames[prevId - 1];
    
    presetNames.clear();
    presetBox.clear(juce::dontSendNotification);
    
    auto folder = getPresetsFolder();
    auto files = folder.findChildFiles(juce::File::findFiles, false, "*.json");
    files.sort();
    
    int id = 1;
    int restoreId = 0;
    for (auto& f : files)
    {
        juce::String name = f.getFileNameWithoutExtension();
        juce::String fullPath = f.getFullPathName();
        presetNames.add(fullPath);
        presetBox.addItem(name, id);
        if (fullPath == prevPath)
            restoreId = id;
        id++;
    }
    
    if (restoreId > 0)
        presetBox.setSelectedId(restoreId, juce::dontSendNotification);
}

void EchoJayEditor::saveCurrentPreset(const juce::String& name)
{
    auto refs = processorRef.getReferenceAnalyser().getReferences();
    if (refs.empty()) return;
    
    auto preset = std::make_unique<juce::DynamicObject>();
    preset->setProperty("name", name);
    preset->setProperty("created", juce::Time::currentTimeMillis());
    
    juce::Array<juce::var> refsArray;
    for (auto& r : refs)
    {
        auto refObj = std::make_unique<juce::DynamicObject>();
        refObj->setProperty("name", r.name);
        refObj->setProperty("path", r.path);
        refObj->setProperty("duration", r.durationSeconds);
        
        // Store meter data
        auto meterObj = std::make_unique<juce::DynamicObject>();
        meterObj->setProperty("integrated", r.data.integrated);
        meterObj->setProperty("truePeakL", r.data.truePeakL);
        meterObj->setProperty("truePeakR", r.data.truePeakR);
        meterObj->setProperty("loudnessRange", r.data.loudnessRange);
        meterObj->setProperty("crestFactor", r.data.crestFactor);
        meterObj->setProperty("width", r.data.width);
        meterObj->setProperty("correlation", r.data.correlation);
        meterObj->setProperty("dcOffset", r.data.dcOffset);
        meterObj->setProperty("rmsL", r.data.rmsL);
        meterObj->setProperty("rmsR", r.data.rmsR);
        meterObj->setProperty("peakL", r.data.peakL);
        meterObj->setProperty("peakR", r.data.peakR);
        refObj->setProperty("meters", juce::var(meterObj.release()));
        
        // Store EQ curve
        juce::Array<juce::var> eqArr;
        for (int i = 0; i < 64; ++i)
            eqArr.add(r.eqCurve[(size_t)i]);
        refObj->setProperty("eqCurve", eqArr);
        
        // Store waveform thumbnail (compact: store every 4th point)
        juce::Array<juce::var> wfArr;
        for (int i = 0; i < (int)r.waveformThumbnail.size(); i += 4)
            wfArr.add(r.waveformThumbnail[(size_t)i]);
        refObj->setProperty("waveform", wfArr);
        
        refsArray.add(juce::var(refObj.release()));
    }
    preset->setProperty("references", refsArray);
    
    juce::String json = juce::JSON::toString(juce::var(preset.release()), true);
    
    // Sanitise filename
    juce::String safeName = name.replaceCharacters(":/\\\"'", "-----");
    auto file = getPresetsFolder().getChildFile(safeName + ".json");
    file.replaceWithText(json);
}

void EchoJayEditor::loadPreset(const juce::String& filePath)
{
    juce::File file(filePath);
    if (!file.existsAsFile()) { refStatusLabel.setText("Preset file not found", juce::dontSendNotification); return; }
    
    auto json = juce::JSON::parse(file.loadFileAsString());
    if (!json.isObject()) return;
    
    auto* root = json.getDynamicObject();
    if (!root || !root->hasProperty("references")) return;
    
    // Clear existing references
    processorRef.getReferenceAnalyser().clearAll();
    
    // References folder where dropped files are copied
    auto refsFolder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                          .getChildFile("EchoJay").getChildFile("References");
    
    auto refsVar = root->getProperty("references");
    if (auto* refsArr = refsVar.getArray())
    {
        for (auto& refVar : *refsArr)
        {
            auto* refObj = refVar.getDynamicObject();
            if (!refObj) continue;
            
            juce::String refPath = refObj->getProperty("path").toString();
            juce::String refName = refObj->getProperty("name").toString();
            
            // Try multiple locations for the file
            juce::File refFile(refPath);
            if (!refFile.existsAsFile())
            {
                // Try the References folder copy
                auto altFile = refsFolder.getChildFile(juce::File(refPath).getFileName());
                if (altFile.existsAsFile())
                    refFile = altFile;
                else
                {
                    // Try with common extensions
                    for (auto ext : { ".wav", ".mp3", ".flac", ".aiff", ".m4a" })
                    {
                        auto tryFile = refsFolder.getChildFile(refName + ext);
                        if (tryFile.existsAsFile()) { refFile = tryFile; break; }
                    }
                }
            }
            
            if (refFile.existsAsFile())
            {
                processorRef.getReferenceAnalyser().analyseFile(refFile, [this, refName](bool success, const juce::String& err) {
                    if (success)
                        refStatusLabel.setText("Loaded: " + refName, juce::dontSendNotification);
                    else
                        refStatusLabel.setText("Error: " + err, juce::dontSendNotification);
                    // Don't call showCompareView — the timer auto-refresh handles dropdown updates
                    repaint();
                });
            }
            else
            {
                refStatusLabel.setText("File not found: " + refName, juce::dontSendNotification);
            }
        }
    }
    
    refStatusLabel.setText("Loading preset: " + file.getFileNameWithoutExtension(), juce::dontSendNotification);
    repaint();
}

void EchoJayEditor::deletePreset(const juce::String& filePath)
{
    juce::File file(filePath);
    if (file.existsAsFile())
        file.deleteFile();
}

juce::String EchoJayEditor::getCompareSlotWavPath(int selectedId)
{
    if (selectedId <= 0) return {};
    auto snaps = processorRef.getSnapshots();
    auto refs = processorRef.getReferenceAnalyser().getReferences();
    int refOffset = (int)snaps.size() + 100;
    
    if (selectedId >= refOffset && (selectedId - refOffset) < (int)refs.size())
        return refs[(size_t)(selectedId - refOffset)].path;
    else if ((selectedId - 1) < (int)snaps.size())
        return snaps[(size_t)(selectedId - 1)].wavFilePath;
    return {};
}

void EchoJayEditor::mouseDown(const juce::MouseEvent& e)
{
    if (currentView == View::Compare)
    {
        auto pos = e.getEventRelativeTo(this).getPosition();
        
        // Right-click on card area — rename pass inline
        if (e.mods.isPopupMenu())
        {
            auto checkRename = [&](juce::ComboBox& box) {
                int sel = box.getSelectedId();
                if (sel <= 0) return;
                auto snaps = processorRef.getSnapshots();
                int refOffset = (int)snaps.size() + 100;
                if (sel < refOffset && (sel - 1) < (int)snaps.size())
                {
                    int idx = sel - 1;
                    // Show popup menu with rename option
                    juce::PopupMenu menu;
                    menu.setLookAndFeel(&lnf);
                    menu.addItem(1, "Rename \"" + snaps[(size_t)idx].name + "\"");
                    menu.addItem(2, "Delete Pass");
                    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(box),
                        [this, idx](int result) {
                            if (result == 1) {
                                // Inline rename via a popup text input
                                auto snaps2 = processorRef.getSnapshots();
                                if (idx < (int)snaps2.size()) {
                                    auto* te = new juce::TextEditor();
                                    te->setFont(juce::Font(juce::FontOptions(12.0f)));
                                    te->setText(snaps2[(size_t)idx].name);
                                    te->selectAll();
                                    te->setBounds(getWidth() / 2 - 100, getHeight() / 2 - 14, 200, 28);
                                    te->setColour(juce::TextEditor::backgroundColourId, C::bg3);
                                    te->setColour(juce::TextEditor::textColourId, C::text);
                                    te->setColour(juce::TextEditor::outlineColourId, C::purple);
                                    te->setColour(juce::TextEditor::focusedOutlineColourId, C::purple);
                                    addAndMakeVisible(te);
                                    te->grabKeyboardFocus();
                                    te->onReturnKey = [this, te, idx]() {
                                        auto newName = te->getText().trim();
                                        if (newName.isNotEmpty())
                                            processorRef.renameSnapshot(idx, newName);
                                        juce::MessageManager::callAsync([te]() { delete te; });
                                        if (currentView == View::Compare)
                                            showCompareView();
                                        repaint();
                                    };
                                    te->onFocusLost = [this, te]() {
                                        juce::MessageManager::callAsync([te]() { delete te; });
                                        repaint();
                                    };
                                }
                            } else if (result == 2) {
                                processorRef.deleteSnapshot(idx);
                                if (currentView == View::Compare)
                                    showCompareView();
                                repaint();
                            }
                        });
                }
            };
            checkRename(compareSlotABox);
            checkRename(compareSlotBBox);
            return;
        }
        
        // Left-click on waveform bars — play/seek
        if (!compareWavePositions.empty())
        {
            for (auto& wp : compareWavePositions)
            {
                if (wp.bounds.expanded(4, 8).contains(pos) && wp.wavPath.isNotEmpty())
                {
                    int playBtnArea = 26;
                    int wfStartX = wp.bounds.getX() + playBtnArea;
                    int wfEndX = wp.bounds.getRight() - 4;
                    
                    if (pos.x <= wfStartX)
                    {
                        if (currentlyPlayingChatWav == wp.wavPath)
                            stopChatPlayback();
                        else
                            startChatPlayback(wp.wavPath, 0);
                    }
                    else
                    {
                        float fraction = juce::jlimit(0.0f, 1.0f, (float)(pos.x - wfStartX) / (float)(wfEndX - wfStartX));
                        float seekTime = fraction * wp.duration;
                        startChatPlayback(wp.wavPath, seekTime);
                    }
                    repaint();
                    return;
                }
            }
        }
    }
}

void EchoJayEditor::stopChatPlayback()
{
    if (chatPlaybackProcess != nullptr)
    {
        chatPlaybackProcess->kill();
        chatPlaybackProcess.reset();
    }
    currentlyPlayingChatWav.clear();
    chatPlaybackStartTime = 0;
    chatPlaybackDuration = 0;
    chatPlaybackOffset = 0;
    playSlotABtn.setButtonText(">");
    playSlotBBtn.setButtonText(">");
}

void EchoJayEditor::startChatPlayback(const juce::String& wavPath, float offset)
{
    stopChatPlayback();
    
    juce::File wavFile(wavPath);
    if (!wavFile.existsAsFile()) return;
    
    juce::File fileToPlay = wavFile;
    
    // If seeking, create a temp trimmed WAV starting from the offset
    if (offset > 0.1f)
    {
        juce::AudioFormatManager formatMgr;
        formatMgr.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formatMgr.createReaderFor(wavFile));
        if (reader != nullptr)
        {
            juce::int64 startSample = (juce::int64)(offset * reader->sampleRate);
            juce::int64 numSamples = reader->lengthInSamples - startSample;
            if (numSamples > 0)
            {
                juce::File tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                         .getChildFile("echojay_seek_temp.wav");
                tempFile.deleteFile();
                
                juce::WavAudioFormat wavFormat;
                auto* outStream = new juce::FileOutputStream(tempFile);
                if (!outStream->failedToOpen())
                {
                    std::unique_ptr<juce::AudioFormatWriter> writer(
                        wavFormat.createWriterFor(outStream, reader->sampleRate,
                                                   (unsigned int)reader->numChannels, 16, {}, 0));
                    if (writer != nullptr)
                    {
                        writer->writeFromAudioReader(*reader, startSample, numSamples);
                        writer.reset();
                        fileToPlay = tempFile;
                    }
                }
            }
        }
    }
    
    chatPlaybackProcess = std::make_unique<juce::ChildProcess>();
    juce::StringArray args;
    args.add("/usr/bin/afplay");
    args.add(fileToPlay.getFullPathName());
    
    if (chatPlaybackProcess->start(args))
    {
        currentlyPlayingChatWav = wavPath;
        chatPlaybackStartTime = juce::Time::getMillisecondCounterHiRes();
        chatPlaybackOffset = offset;
        
        // Find duration — search chat messages and compare positions
        for (auto& msg : chatMessages)
            if (msg.wavFilePath == wavPath)
                { chatPlaybackDuration = msg.durationSeconds; break; }
        if (chatPlaybackDuration <= 0)
            for (auto& wp : compareWavePositions)
                if (wp.wavPath == wavPath)
                    { chatPlaybackDuration = wp.duration; break; }
        
        // Auto-clear after remaining duration
        float remaining = chatPlaybackDuration - offset;
        if (remaining > 0)
            juce::Timer::callAfterDelay((int)(remaining * 1000) + 500, [this]() {
                if (chatPlaybackProcess != nullptr && !chatPlaybackProcess->isRunning())
                    stopChatPlayback();
                repaint();
            });
    }
    else
    {
        chatPlaybackProcess.reset();
    }
    repaint();
}

void EchoJayEditor::onWavePlayClick(int index)
{
    if (index < 0 || index >= kMaxWavePlayBtns) return;
    juce::String wavPath = wavePlayPaths[(size_t)index];
    if (wavPath.isEmpty()) return;
    
    if (currentlyPlayingChatWav == wavPath)
    {
        // Toggle off
        stopChatPlayback();
        repaint();
        return;
    }
    
    startChatPlayback(wavPath, 0);
}

void EchoJayEditor::onWaveSeekClick(int index, float fraction)
{
    if (index < 0 || index >= kMaxWavePlayBtns) return;
    juce::String wavPath = wavePlayPaths[(size_t)index];
    if (wavPath.isEmpty()) return;
    
    float dur = wavePlayDurations[(size_t)index];
    float seekTime = fraction * dur;
    startChatPlayback(wavPath, seekTime);
}
