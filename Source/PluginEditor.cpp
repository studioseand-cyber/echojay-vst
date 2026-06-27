#include "PluginEditor.h"
#include <cmath>

namespace
{
// Shared logo image cache. This MUST be explicitly cleared during editor
// teardown (see ~EchoJayEditor), not left to destruct at DLL unload.
//
// Root cause of the Windows/Cubase "freeze a couple of seconds after removal,
// on its own, no click" hang (confirmed by a forced dump: APPLICATION_HANG
// HungIn_LoaderLock in EchoJay.vst3): a juce::Image held in a function-local
// static stays alive for the lifetime of the DLL. When the host unloads the
// plugin, Windows runs the DLL's static destructors UNDER THE LOADER LOCK. If
// that static Image still owns a GPU/OpenGL-cached texture, freeing it calls
// into the D3D/WARP driver (d3d11 -> d3d10warp), which itself needs the loader
// lock -> deadlock -> host frozen, no crash, Windows-only. Clearing the cache
// (and JUCE's image cache) during normal editor destruction means nothing
// GPU-backed survives to DLL-unload time, so the loader-lock teardown has
// nothing to do and cannot hang.
juce::Image getLogoImage()
{
    // Reference-counted; released by ImageCache::releaseUnusedImages() during
    // editor teardown, so it never survives to DLL unload (see note below).
    return juce::ImageCache::getFromMemory(echoJayLogoPNG, (int)echoJayLogoPNGSize);
}

void clearLogoImageCache()
{
    juce::ImageCache::releaseUnusedImages();
}

struct ChannelPromptOption
{
    const char* label;
    ChannelType type;
    int groupIndex;
};

static constexpr const char* kChannelPromptGroups[] = {
    "Vocals",
    "Drums",
    "Bass",
    "Keys & Guitar",
    "Synths",
    "Strings & Brass",
    "FX & Other",
    "Buses"
};

static constexpr ChannelPromptOption kChannelPromptOptions[] = {
    { "Lead Vocal", ChannelType::LeadVocal, 0 },
    { "Backing Vocal", ChannelType::BackingVocal, 0 },
    { "Adlibs", ChannelType::Adlibs, 0 },
    { "Vocal Bus", ChannelType::VocalBus, 0 },
    { "Kick", ChannelType::Kick, 1 },
    { "Snare", ChannelType::Snare, 1 },
    { "Hi-Hat", ChannelType::HiHat, 1 },
    { "Overheads", ChannelType::Overheads, 1 },
    { "Drum Bus", ChannelType::DrumBus, 1 },
    { "Percussion", ChannelType::Percussion, 1 },
    { "Bass / 808", ChannelType::Bass808, 2 },
    { "Bass Guitar", ChannelType::BassGuitar, 2 },
    { "Sub Bass", ChannelType::SubBass, 2 },
    { "Synth Bass", ChannelType::SynthBass, 2 },
    { "Piano", ChannelType::Piano, 3 },
    { "Keys", ChannelType::Keys, 3 },
    { "Acoustic Guitar", ChannelType::AcousticGuitar, 3 },
    { "Electric Guitar", ChannelType::ElectricGuitar, 3 },
    { "Guitar Bus", ChannelType::GuitarBus, 3 },
    { "Synth Lead", ChannelType::SynthLead, 4 },
    { "Synth Pad", ChannelType::SynthPad, 4 },
    { "Synth Pluck", ChannelType::SynthPluck, 4 },
    { "Synth Bus", ChannelType::SynthBus, 4 },
    { "Strings", ChannelType::Strings, 5 },
    { "Brass", ChannelType::Brass, 5 },
    { "Woodwind", ChannelType::Woodwind, 5 },
    { "Orchestral", ChannelType::Orchestral, 5 },
    { "FX", ChannelType::FX, 6 },
    { "Reverb", ChannelType::Reverb, 6 },
    { "Delay", ChannelType::Delay, 6 },
    { "Foley", ChannelType::Foley, 6 },
    { "Ambient", ChannelType::Ambient, 6 },
    { "Master Bus", ChannelType::MasterBus, 7 },
    { "Instrument Bus", ChannelType::InstrumentBus, 7 },
    { "Music Bus", ChannelType::MusicBus, 7 }
};

// Genre prompt data — matches web app menu structure
struct GenrePromptOption
{
    const char* label;
    int groupIndex;
};

static constexpr const char* kGenrePromptGroups[] = {
    "Popular",
    "Electronic",
    "Rock & Alt",
    "Other"
};

static constexpr GenrePromptOption kGenrePromptOptions[] = {
    // Popular (0)
    { "Hip-Hop", 0 }, { "Pop", 0 }, { "R&B", 0 }, { "Rap", 0 }, { "Trap", 0 }, { "Drill", 0 },
    // Electronic (1)
    { "EDM", 1 }, { "House", 1 }, { "Techno", 1 }, { "Drum & Bass", 1 },
    { "Dubstep", 1 }, { "Trance", 1 }, { "Garage", 1 }, { "Bass / Dub", 1 },
    // Rock & Alt (2)
    { "Rock", 2 }, { "Indie", 2 }, { "Punk", 2 }, { "Metal", 2 }, { "Alt Rock", 2 }, { "Grunge", 2 },
    // Other (3)
    { "Jazz", 3 }, { "Classical", 3 }, { "Country", 3 }, { "Reggae", 3 },
    { "Soul", 3 }, { "Funk", 3 }, { "Gospel", 3 }, { "Blues", 3 },
    { "Lo-Fi", 3 }, { "Ambient", 3 }, { "Latin", 3 }, { "Afrobeat", 3 },
    { "Dancehall", 3 }, { "Grime", 3 }, { "Phonk", 3 }, { "Jersey Club", 3 }
};

// --- Update-dismissed persistence -------------------------------------------
// Stored at ~/Library/Application Support/EchoJay/update_dismissed.json
// Format: { "version": "1.2.0", "dismissedAtSeconds": 1714678800 }
//   - version: which latestVersion the user dismissed
//   - dismissedAtSeconds: Unix epoch when they clicked Not Now
// We re-prompt 3 days after the last dismissal, OR immediately if a newer
// version appears.

// Returns true if `a` is strictly newer than `b`, using dot-separated numeric
// comparison. Handles 1.10.0 > 1.2.0 correctly (numeric, not lexicographic).
// Trailing components in either string are treated as 0 (so "1.2" == "1.2.0").
// Non-numeric tokens are treated as 0.
static bool isVersionNewer(const juce::String& a, const juce::String& b)
{
    auto partsA = juce::StringArray::fromTokens(a, ".", "");
    auto partsB = juce::StringArray::fromTokens(b, ".", "");
    int n = juce::jmax(partsA.size(), partsB.size());
    for (int i = 0; i < n; ++i)
    {
        int ai = (i < partsA.size()) ? partsA[i].getIntValue() : 0;
        int bi = (i < partsB.size()) ? partsB[i].getIntValue() : 0;
        if (ai > bi) return true;
        if (ai < bi) return false;
    }
    return false;  // equal — not newer
}

static juce::File updateDismissFile()
{
    auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    return appData.getChildFile("Application Support/EchoJay/update_dismissed.json");
   #else
    return appData.getChildFile("EchoJay/update_dismissed.json");
   #endif
}

// Returns true if we should suppress the overlay because the user dismissed
// THIS version less than 3 days ago.
static bool isUpdateDismissalActive(const juce::String& versionToCheck)
{
    auto f = updateDismissFile();
    if (!f.existsAsFile()) return false;
    auto json = juce::JSON::parse(f);
    auto* obj = json.getDynamicObject();
    if (obj == nullptr) return false;
    auto storedVersion = obj->getProperty("version").toString();
    auto dismissedAt = (juce::int64)(double)obj->getProperty("dismissedAtSeconds");
    if (storedVersion != versionToCheck) return false;  // different version → re-prompt
    auto nowSec = juce::Time::currentTimeMillis() / 1000;
    auto threeDaysSec = (juce::int64)(3 * 24 * 60 * 60);
    return (nowSec - dismissedAt) < threeDaysSec;
}

static void recordUpdateDismissal(const juce::String& versionDismissed)
{
    auto f = updateDismissFile();
    f.getParentDirectory().createDirectory();
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("version", versionDismissed);
    obj->setProperty("dismissedAtSeconds", (double)(juce::Time::currentTimeMillis() / 1000));
    f.replaceWithText(juce::JSON::toString(juce::var(obj.get())));
}

}

// Static: genre prompt dismissed flag — persists across all instances for the DAW session
bool EchoJayEditor::genrePromptDismissedThisSession = false;

// ============================================================================
// Constructor
// ============================================================================

EchoJayEditor::EchoJayEditor(EchoJayProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    setLookAndFeel(&lnf);
    setSize(900, 580);
    setResizable(true, true);
    setResizeLimits(900, 580, 1800, 1200);
    setWantsKeyboardFocus(true);
    setOpaque(true);

    // Wire workspace load callback — repaint so Chat tab status line updates
    workspace.onLoaded = [this]()
    {
        DBG("[EchoJayWorkspace] onLoaded — chats:" << (int)workspace.getChats().size()
            << "  albums:" << (int)workspace.getAlbums().size()
            << "  reviews:" << (int)workspace.getReviews().size());
        if (sidebarModel)
        {
            sidebarModel->refreshRows(workspace.getChats(), workspace.getAlbums(),
                                      workspace.getReviews(), collapsedAlbums, currentChatId);
            chatSidebar.updateContent();
        }
        repaint();
    };

    // Stock plugins are injected from the auto-detected host DAW (set in the
    // processor via juce::PluginHostType), not from the Settings selection, so
    // there's nothing to seed here.

    // ---- Plugin review / checklist setup -------------------------------
    auto& scannerRef = processorRef.getPluginScanner();

    reviewChecklist = std::make_unique<PluginChecklistComponent>(scannerRef);
    settingsChecklist = std::make_unique<PluginChecklistComponent>(scannerRef);

    // When either checklist changes, persist to server (if settings already
    // fetched) and refresh the AI plugin feed + the other checklist so both
    // views stay consistent.
    // Ticking is now a LOCAL, instant operation inside the checklist (it just
    // flips a bool in an in-memory set and repaints one row — no scanner, disk,
    // or network touch per click). The expensive commit happens once, on Done /
    // Save, via commitChecklist(). onChanged only fires cheaply so we can note
    // there are unsaved changes if we ever want to.
    auto commitChecklist = [this]()
    {
        // Push both checklists' local selections to the scanner (only the one
        // with changes will do anything), then persist + sync once.
        if (reviewChecklist)   reviewChecklist->commit();
        if (settingsChecklist) settingsChecklist->commit();

        auto& sc = processorRef.getPluginScanner();
        sc.saveEnabledState();
        api.updatePluginsFromScanner(sc.getFullPluginList());
        if (settingsFetched)
            api.saveUserSettings(api.getUserSettings(), nullptr);
    };
    // Stash it so other handlers (Settings Save) can reuse it.
    commitChecklistFn = commitChecklist;

    auto onChecklistChanged = [this]() { /* local only; nothing to do per tick */ };
    reviewChecklist->onChanged = onChecklistChanged;
    settingsChecklist->onChanged = onChecklistChanged;

    reviewViewport.setViewedComponent(reviewChecklist.get(), false);
    reviewViewport.setScrollBarsShown(true, false);
    settingsPluginViewport.setViewedComponent(settingsChecklist.get(), false);
    settingsPluginViewport.setScrollBarsShown(true, false);

    addChildComponent(settingsPluginViewport);

    // Review overlay button wiring. Done COMMITS the local selection, then
    // closes — this is the single point where ticks are saved.
    reviewOverlay.onDone = [this]()
    {
        if (commitChecklistFn) commitChecklistFn();
        hidePluginReview();
    };
    reviewOverlay.onSelectAll = [this](bool all)
    {
        if (reviewChecklist) reviewChecklist->selectAllVisible(all);
    };
    reviewOverlay.onAddManual = [this]()
    {
        auto* w = new juce::AlertWindow("Add Plugin",
            "Enter the plugin name as you'd refer to it (e.g. \"FabFilter Pro-Q 4\").",
            juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", "", "Plugin name:");
        w->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<EchoJayEditor> safeThis(this);
        w->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, w](int r)
            {
                std::unique_ptr<juce::AlertWindow> holder(w);
                if (safeThis == nullptr) return;
                if (r == 1)
                {
                    auto name = w->getTextEditorContents("name").trim();
                    if (name.isNotEmpty() && safeThis->reviewChecklist)
                        safeThis->reviewChecklist->addManual(name);
                }
            }), false);
    };
    addChildComponent(reviewOverlay);
    reviewOverlay.addAndMakeVisible(reviewViewport);

    // Search box inside the popup overlay. Filters the review checklist live.
    auto styleSearch = [this](juce::TextEditor& ed, const juce::String& placeholder)
    {
        ed.setMultiLine(false);
        ed.setTextToShowWhenEmpty(placeholder, C::text3);
        ed.setColour(juce::TextEditor::backgroundColourId, C::bg3);
        ed.setColour(juce::TextEditor::textColourId, C::text);
        ed.setColour(juce::TextEditor::outlineColourId, C::border2);
        ed.setFont(juce::Font(juce::FontOptions(13.0f)));
        ed.setIndents(8, 6);
    };
    styleSearch(reviewSearchBox, "Search plugins...");
    reviewOverlay.addAndMakeVisible(reviewSearchBox);
    reviewSearchBox.onTextChange = [this]()
    {
        if (reviewChecklist)
        {
            reviewChecklist->setFilter(reviewSearchBox.getText());
            layoutPluginReview();
            reviewOverlay.repaint();
        }
    };

    // Settings: search box (inline filtered results) + "View all" button that
    // opens the popup. The inline viewport shows the filtered subset; with no
    // search it shows the capped first rows and the user clicks View all to
    // browse/scroll everything in the popup.
    // Settings no longer shows an inline plugin list or search box. It shows a
    // scan button (same behaviour as the header one), a View all button, and a
    // Help & Support button. The search box member is kept (unused inline) only
    // so existing references compile; search now lives inside the popup.
    styleSearch(settingsPluginSearchBox, "Search your plugins...");
    addChildComponent(settingsPluginSearchBox); // not shown inline anymore

    // Settings scan button — identical menu/behaviour to the header scan
    // button. Its label tracks the plugin count (kept in sync in timerCallback
    // alongside the header button).
    settingsScanBtn.setColour(juce::TextButton::buttonColourId, C::bg4);
    settingsScanBtn.setColour(juce::TextButton::textColourOffId, C::purple);
    settingsScanBtn.setColour(juce::TextButton::textColourOnId, C::purple);
    addChildComponent(settingsScanBtn);
    settingsScanBtn.onClick = [this]() { showScanMenu(&settingsScanBtn); };

    viewAllPluginsBtn.setColour(juce::TextButton::buttonColourId, C::bg4);
    viewAllPluginsBtn.setColour(juce::TextButton::textColourOffId, C::text2);
    addChildComponent(viewAllPluginsBtn);
    viewAllPluginsBtn.onClick = [this]()
    {
        if (reviewChecklist)
        {
            reviewChecklist->setFilter(juce::String());
            reviewChecklist->refresh();
        }
        reviewSearchBox.setText(juce::String(), juce::dontSendNotification);
        showPluginReview();
    };

    // Help & Support — a quiet link-style button (transparent, muted text) on
    // the bottom row near Log Out, so it reads as an app action, not a plugins
    // control. Opens the EchoJay support page.
    settingsHelpBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    settingsHelpBtn.setColour(juce::TextButton::textColourOffId, C::text3);
    settingsHelpBtn.setColour(juce::TextButton::textColourOnId, C::text2);
    addChildComponent(settingsHelpBtn);
    settingsHelpBtn.onClick = [this]()
    {
        juce::URL("https://www.echojay.ai/support").launchInDefaultBrowser();
    };


    // --- Channel type ---
    // Grouped channel type dropdown — uses PopupMenu with submenus via getRootMenu()
    loadCustomChannels();
    rebuildChannelTypeBox();
    channelTypeBox.setColour(juce::ComboBox::backgroundColourId, C::bg3);
    channelTypeBox.setColour(juce::ComboBox::textColourId, C::text);
    channelTypeBox.setColour(juce::ComboBox::outlineColourId, C::border2);
    addAndMakeVisible(channelTypeBox);

    // --- Genre ---
    loadCustomGenres();
    rebuildGenreBox();
    genreBox.setColour(juce::ComboBox::backgroundColourId, C::bg3);
    genreBox.setColour(juce::ComboBox::textColourId, C::text);
    genreBox.setColour(juce::ComboBox::outlineColourId, C::border2);
    addAndMakeVisible(genreBox);

    // --- Project input ---
    projectInput.setFont(juce::Font(juce::FontOptions(12.0f)));
    projectInput.setTextToShowWhenEmpty("Project name...", C::text3.withAlpha(0.5f));
    projectInput.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff111520));
    projectInput.setColour(juce::TextEditor::outlineColourId, C::border2);
    projectInput.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff22d3ee).withAlpha(0.6f));
    projectInput.setColour(juce::TextEditor::textColourId, C::text);
    projectInput.setText(processorRef.getProjectName(), juce::dontSendNotification);
    projectInput.onTextChange = [this] {
        juce::String proj = projectInput.getText().trim();
        processorRef.setProjectName(proj);
        // Immediately rename the current chat to match the project name
        if (currentChatId.isNotEmpty())
        {
            juce::String newTitle = proj;
            if (proj.isEmpty()) {
                for (auto& ch : workspace.getChats()) {
                    if (ch.id == currentChatId) {
                        if (ch.created.isNotEmpty())
                            newTitle = juce::Time::fromISO8601(ch.created).formatted("%d %b %Y, %H:%M");
                        break;
                    }
                }
            }
            workspace.setChatTitle(currentChatId, newTitle);
            workspace.setChatTrackName(currentChatId, proj);
            if (sidebarModel)
            {
                sidebarModel->refreshRows(workspace.getChats(), workspace.getAlbums(),
                                          workspace.getReviews(), collapsedAlbums, currentChatId);
                chatSidebar.updateContent();
            }
        }
    };
    addAndMakeVisible(projectInput);

    // --- Capture ---
    captureBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    captureBtn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff22d3ee));
    captureBtn.setColour(juce::TextButton::textColourOffId, C::text);
    captureBtn.onClick = [this] {
        auto s = processorRef.getCaptureState();
        if (s == CaptureState::Idle || s == CaptureState::Complete)
        {
            waveformFrozen = false;
            frozenWaveform.clear();
            unfreezeCountdown = 0;
            captureWasSilent = false;
            processorRef.startCapture();
        }
        else if (s == CaptureState::Capturing)
        {
            processorRef.stopCapture();
        }
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

    abSyncBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    abSyncBtn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff22d3ee));
    abSyncBtn.setColour(juce::TextButton::textColourOffId, C::text3);
    abSyncBtn.setButtonText("Sync");
    abSyncBtn.onClick = [this] {
        bool newState = !processorRef.abSyncToDAW.load();
        processorRef.abSyncToDAW.store(newState);
        abSyncBtn.setColour(juce::TextButton::textColourOffId, 
            newState ? juce::Colour(0xff22d3ee) : C::text3);
        repaint();
    };
    abSyncBtn.setVisible(false);
    abSyncBtn.setBounds(-100, -100, 1, 1);
    addAndMakeVisible(abSyncBtn);

    wavSavedLabel.setColour(juce::Label::textColourId, C::text3);
    wavSavedLabel.setFont(juce::Font(juce::FontOptions(9.0f)));
    wavSavedLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(wavSavedLabel);

    // --- Particle Visual ---
    // Holder is a child of the editor; particleVisual lives inside the holder.
    // This makes the macOS GL overlay show the holder's opaque bg during GL init
    // instead of the desktop/host's underlying surface (JUCE+macOS limitation).
    addChildComponent(particleVisualHolder);
    particleVisual = std::make_unique<ParticleVisual>();
    particleVisualHolder.addAndMakeVisible(*particleVisual);
    
    // Restore visual state from processor (persisted with DAW session)
    particleVisual->currentPreset = (ParticleVisual::Preset)juce::jlimit(0, 3, processorRef.visualPreset);
    particleVisual->currentTheme = (ParticleVisual::Theme)juce::jlimit(0, 3, processorRef.visualTheme);
    visualMode = processorRef.visualModeOn;
    
    // --- Update Overlay (real child component so it draws ON TOP of particleVisual) ---
    updateOverlay.setVisible(false);
    updateOverlay.onDownload = [this]() {
        // If we have a direct download URL for this platform, do the
        // in-plugin download flow. Otherwise fall back to the legacy
        // browser-open behaviour. (This also covers very early launches
        // where the remote config hasn't loaded yet — better to do
        // something useful than block.)
       #if JUCE_MAC
        bool haveDirect = EchoJayAPI::downloadUrlMac.isNotEmpty();
       #elif JUCE_WINDOWS
        bool haveDirect = EchoJayAPI::downloadUrlWin.isNotEmpty();
       #else
        bool haveDirect = false;
       #endif
        if (haveDirect)
        {
            startUpdateDownload();
        }
        else
        {
            auto url = EchoJayAPI::updateUrl.isNotEmpty()
                         ? EchoJayAPI::updateUrl
                         : juce::String("https://www.echojay.ai/?noredirect#plugin");
            juce::URL(url).launchInDefaultBrowser();
        }
    };
    updateOverlay.onInstall = [this]() { launchDownloadedInstaller(); };
    updateOverlay.onRetry = [this]() {
        // Reset to idle and immediately re-kick the download. Saves the
        // user a redundant click.
        updateOverlay.state = UpdateOverlay::State::Idle;
        updateOverlay.progress = 0.0f;
        updateOverlay.errorText.clear();
        startUpdateDownload();
    };
    updateOverlay.onDismiss = [this]() {
        // If a download is in flight, signal it to stop so we don't keep
        // streaming bytes after the user has clicked away. Safe to set
        // unconditionally — worker only checks during a download.
        updateDownloadCancelled->store(true);
        updateDismissed = true;
        recordUpdateDismissal(EchoJayAPI::latestVersion);
        updateAvailable = false;
        updateOverlay.setVisible(false);
        resized();  // bring particleVisual back
        repaint();
    };
    addChildComponent(updateOverlay);

    // --- Compare ---
    compareBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    compareBtn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff22d3ee));
    compareBtn.setColour(juce::TextButton::textColourOffId, C::text);
    compareBtn.onClick = [this] {
        if (currentView == View::Compare) { currentView = View::Meters; hideCompareView(); }
        else { hideSettingsView(); currentView = View::Compare; showCompareView(); }
    };
    addAndMakeVisible(compareBtn);

    // --- Settings ---
    settingsBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    settingsBtn.setColour(juce::TextButton::textColourOnId, C::text);
    settingsBtn.setColour(juce::TextButton::textColourOffId, C::text2);
    settingsBtn.onClick = [this] {
        if (currentView == View::Settings) { hideSettingsView(); currentView = View::Meters; }
        else { hideCompareView(); showSettingsView(); currentView = View::Settings; }
    };
    addAndMakeVisible(settingsBtn);

    // --- Right-side top bar controls ---
    scanBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    scanBtn.setColour(juce::TextButton::textColourOnId, C::purple);
    scanBtn.setColour(juce::TextButton::textColourOffId, C::purple);
    // Click opens a menu: Scan Now / Add Folder / list of custom folders to
    // remove. Keeps the default action (Scan Now) one step away while letting
    // users add or prune extra scan locations without a settings trip.
    scanBtn.onClick = [this] { showScanMenu(&scanBtn); };
    addAndMakeVisible(scanBtn);

    logoutBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    logoutBtn.setColour(juce::TextButton::textColourOnId, C::text3);
    logoutBtn.setColour(juce::TextButton::textColourOffId, C::text3);
    logoutBtn.onClick = [this] { handleLogout(); };
    addAndMakeVisible(logoutBtn);

    channelPromptBlocker.setVisible(false);
    addChildComponent(channelPromptBlocker);

    channelPromptTitle.setText("What type of channel is this?", juce::dontSendNotification);
    channelPromptTitle.setColour(juce::Label::textColourId, C::text);
    channelPromptTitle.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
    channelPromptTitle.setJustificationType(juce::Justification::centred);
    channelPromptTitle.setVisible(false);
    addAndMakeVisible(channelPromptTitle);

    channelPromptSubtitle.setText("Pick the source once and EchoJay will remember it for this plugin instance.", juce::dontSendNotification);
    channelPromptSubtitle.setColour(juce::Label::textColourId, C::text3);
    channelPromptSubtitle.setFont(juce::Font(juce::FontOptions(12.0f)));
    channelPromptSubtitle.setJustificationType(juce::Justification::centred);
    channelPromptSubtitle.setVisible(false);
    addAndMakeVisible(channelPromptSubtitle);

    for (int i = 0; i < kChannelPromptGroupCount; ++i)
    {
        channelPromptGroupLabels[(size_t)i].setText(kChannelPromptGroups[i], juce::dontSendNotification);
        channelPromptGroupLabels[(size_t)i].setColour(juce::Label::textColourId, C::amber);
        channelPromptGroupLabels[(size_t)i].setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        channelPromptGroupLabels[(size_t)i].setJustificationType(juce::Justification::centredLeft);
        channelPromptGroupLabels[(size_t)i].setVisible(false);
        addAndMakeVisible(channelPromptGroupLabels[(size_t)i]);
    }

    for (int i = 0; i < kChannelPromptOptionCount; ++i)
    {
        auto& button = channelPromptButtons[(size_t)i];
        button.setButtonText(kChannelPromptOptions[i].label);
        button.setColour(juce::TextButton::buttonColourId, C::bg3);
        button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        button.setColour(juce::TextButton::buttonOnColourId, C::blue);
        button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        button.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        button.setVisible(false);
        button.onClick = [this, i] { selectChannelPromptType(kChannelPromptOptions[i].type); };
        addAndMakeVisible(button);
    }

    channelPromptSkipBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff06b6d4));
    channelPromptSkipBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    channelPromptSkipBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    channelPromptSkipBtn.setButtonText("Mix Bus");
    channelPromptSkipBtn.setVisible(false);
    channelPromptSkipBtn.onClick = [this] { dismissChannelPrompt(); };
    addAndMakeVisible(channelPromptSkipBtn);
    
    customChannelBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    customChannelBtn.setColour(juce::TextButton::textColourOffId, C::purple);
    customChannelBtn.setColour(juce::TextButton::textColourOnId, C::purple);
    customChannelBtn.setVisible(false);
    customChannelBtn.onClick = [this] {
        auto* te = new juce::TextEditor();
        te->setFont(juce::Font(juce::FontOptions(13.0f)));
        te->setTextToShowWhenEmpty("Type instrument name...", C::text3);
        te->setBounds(getWidth() / 2 - 110, customChannelBtn.getY() - 34, 220, 28);
        te->setColour(juce::TextEditor::backgroundColourId, C::bg3);
        te->setColour(juce::TextEditor::textColourId, C::text);
        te->setColour(juce::TextEditor::outlineColourId, C::purple);
        te->setColour(juce::TextEditor::focusedOutlineColourId, C::purple);
        addAndMakeVisible(te);
        te->toFront(true);
        te->grabKeyboardFocus();
        te->onReturnKey = [this, te]() {
            auto name = te->getText().trim();
            if (name.isNotEmpty()) {
                processorRef.setCustomChannelName(name);
                processorRef.setChannelType(ChannelType::Other);
                processorRef.setChannelTypePromptDismissed(true);
                addCustomChannelToList(name);
                rebuildChannelTypeBox();
                updateChannelPromptVisibility();
                resized();
            }
            juce::MessageManager::callAsync([te]() { delete te; });
        };
        te->onFocusLost = [this, te]() {
            auto name = te->getText().trim();
            if (name.isNotEmpty()) {
                processorRef.setCustomChannelName(name);
                processorRef.setChannelType(ChannelType::Other);
                processorRef.setChannelTypePromptDismissed(true);
                addCustomChannelToList(name);
                rebuildChannelTypeBox();
            }
            juce::MessageManager::callAsync([te]() { delete te; });
        };
    };
    addAndMakeVisible(customChannelBtn);

    // --- Session-level genre prompt ---
    genrePromptTitle.setText("What genre is this project?", juce::dontSendNotification);
    genrePromptTitle.setColour(juce::Label::textColourId, C::text);
    genrePromptTitle.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    genrePromptTitle.setJustificationType(juce::Justification::centred);
    genrePromptTitle.setVisible(false);
    addAndMakeVisible(genrePromptTitle);

    genrePromptSubtitle.setText("EchoJay uses this to judge loudness targets. Applies to all instances this session.", juce::dontSendNotification);
    genrePromptSubtitle.setColour(juce::Label::textColourId, C::text3);
    genrePromptSubtitle.setFont(juce::Font(juce::FontOptions(11.0f)));
    genrePromptSubtitle.setJustificationType(juce::Justification::centred);
    genrePromptSubtitle.setVisible(false);
    addAndMakeVisible(genrePromptSubtitle);

    for (int i = 0; i < kGenreGroupCount; ++i)
    {
        genrePromptGroupLabels[(size_t)i].setText(kGenrePromptGroups[i], juce::dontSendNotification);
        genrePromptGroupLabels[(size_t)i].setColour(juce::Label::textColourId, C::amber);
        genrePromptGroupLabels[(size_t)i].setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        genrePromptGroupLabels[(size_t)i].setJustificationType(juce::Justification::centredLeft);
        genrePromptGroupLabels[(size_t)i].setVisible(false);
        addAndMakeVisible(genrePromptGroupLabels[(size_t)i]);
    }

    for (int i = 0; i < kGenreOptionCount; ++i)
    {
        auto& btn = genrePromptButtons[(size_t)i];
        btn.setButtonText(kGenrePromptOptions[i].label);
        btn.setColour(juce::TextButton::buttonColourId, C::bg3);
        btn.setColour(juce::TextButton::textColourOffId, C::text2);
        btn.setColour(juce::TextButton::buttonOnColourId, C::purple);
        btn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        btn.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        btn.setVisible(false);
        btn.onClick = [this, i] { dismissGenrePrompt(kGenrePromptOptions[i].label); };
        addAndMakeVisible(btn);
    }

    genrePromptCustomBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    genrePromptCustomBtn.setColour(juce::TextButton::textColourOffId, C::purple);
    genrePromptCustomBtn.setColour(juce::TextButton::textColourOnId, C::purple);
    genrePromptCustomBtn.setVisible(false);
    genrePromptCustomBtn.onClick = [this] {
        auto* te = new juce::TextEditor();
        te->setFont(juce::Font(juce::FontOptions(13.0f)));
        te->setTextToShowWhenEmpty("Type genre name...", C::text3);
        te->setBounds(getWidth() / 2 - 110, genrePromptCustomBtn.getY() - 34, 220, 28);
        te->setColour(juce::TextEditor::backgroundColourId, C::bg3);
        te->setColour(juce::TextEditor::textColourId, C::text);
        te->setColour(juce::TextEditor::outlineColourId, C::purple);
        te->setColour(juce::TextEditor::focusedOutlineColourId, C::purple);
        addAndMakeVisible(te);
        te->toFront(true);
        te->grabKeyboardFocus();
        te->onReturnKey = [this, te]() {
            auto name = te->getText().trim();
            if (name.isNotEmpty()) {
                addCustomGenreToList(name);
                rebuildGenreBox();
                dismissGenrePrompt(name);
            }
            juce::MessageManager::callAsync([te]() { delete te; });
        };
        te->onFocusLost = [this, te]() {
            auto name = te->getText().trim();
            if (name.isNotEmpty()) {
                addCustomGenreToList(name);
                rebuildGenreBox();
                dismissGenrePrompt(name);
            }
            juce::MessageManager::callAsync([te]() { delete te; });
        };
    };
    addAndMakeVisible(genrePromptCustomBtn);

    userLabel.setColour(juce::Label::textColourId, C::text2);
    userLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    userLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(userLabel);

    usageLabel.setColour(juce::Label::textColourId, C::text3);
    usageLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    usageLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(usageLabel);

    // --- Compare view controls ---
    saveSettingsBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff06b6d4));
    saveSettingsBtn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff22d3ee));
    saveSettingsBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
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
    mkField(settingsPlugins, "Write plugins here or click Scan Plugins above", true);

    settingsExpLevel.addItem("Beginner - Just starting out", 1);
    settingsExpLevel.addItem("Intermediate - 1-3 years", 2);
    settingsExpLevel.addItem("Advanced - 3-10 years", 3);
    settingsExpLevel.addItem("Expert - 10+ years, working engineer", 4);
    settingsExpLevel.setColour(juce::ComboBox::backgroundColourId, C::bg3);
    settingsExpLevel.setColour(juce::ComboBox::textColourId, C::text);
    settingsExpLevel.setColour(juce::ComboBox::outlineColourId, C::border2);
    settingsExpLevel.setVisible(false);
    addAndMakeVisible(settingsExpLevel);
    
    // Chat language picker. Items are populated from EchoJayAPI's canonical
    // list so the codes here stay aligned with the codes the API validates
    // against. ComboBox item IDs are 1-indexed (0 means "no selection"), so
    // we add 1 to the array index. Saves immediately on change — no need
    // to wait for the Save button since this is a local preference.
    {
        const auto& langs = EchoJayAPI::chatLanguageList();
        for (int i = 0; i < langs.size(); ++i)
            settingsLanguage.addItem(langs.getReference(i).second, i + 1);
        // Set current selection based on the saved preference.
        auto currentCode = EchoJayAPI::getChatLanguage();
        for (int i = 0; i < langs.size(); ++i)
        {
            if (langs.getReference(i).first == currentCode)
            {
                settingsLanguage.setSelectedId(i + 1, juce::dontSendNotification);
                break;
            }
        }
        settingsLanguage.setColour(juce::ComboBox::backgroundColourId, C::bg3);
        settingsLanguage.setColour(juce::ComboBox::textColourId, C::text);
        settingsLanguage.setColour(juce::ComboBox::outlineColourId, C::border2);
        settingsLanguage.onChange = [this] {
            int id = settingsLanguage.getSelectedId();
            if (id < 1) return;
            const auto& list = EchoJayAPI::chatLanguageList();
            int idx = id - 1;
            if (idx < 0 || idx >= list.size()) return;
            EchoJayAPI::setChatLanguage(list.getReference(idx).first);
        };
        settingsLanguage.setVisible(false);
        addAndMakeVisible(settingsLanguage);
    }

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

    aiCompareBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff06b6d4));
    aiCompareBtn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff22d3ee));
    aiCompareBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
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
        btn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        btn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff22d3ee));
        btn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
        btn.setVisible(false);
        btn.onClick = [this, &slot, &btn]() {
            auto wavPath = getCompareSlotWavPath(slot.getSelectedId());
            if (wavPath.isEmpty()) return;
            
            // If this file is currently playing, pause it
            if (currentlyPlayingChatWav == wavPath) {
                stopChatPlayback();
                btn.setButtonText(">");
            }
            // If this file is paused (AB has it loaded and paused), resume
            else if (processorRef.abPaused.load() && processorRef.abFilePath == wavPath) {
                // Calculate how far through the file we are (in seconds)
                double elapsedSeconds = 0.0;
                if (processorRef.abSampleCount > 0 && processorRef.abSampleRate > 0)
                    elapsedSeconds = (double)processorRef.abPlaybackPos / processorRef.abSampleRate;
                
                processorRef.resumeAB();
                currentlyPlayingChatWav = wavPath;
                // Set start time in the past so (now - startTime) + offset = elapsedSeconds
                chatPlaybackStartTime = juce::Time::getMillisecondCounterHiRes() - elapsedSeconds * 1000.0;
                chatPlaybackOffset = 0;
                playSlotABtn.setButtonText(">");
                playSlotBBtn.setButtonText(">");
                btn.setButtonText("||");
            }
            // Otherwise start fresh
            else {
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

    refStatusLabel.setColour(juce::Label::textColourId, juce::Colour(0xffFF6B9D));
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
        if (sel == 1) {
            // "Clear All" selected
            processorRef.getReferenceAnalyser().clearAll();
            presetBox.setSelectedId(0, juce::dontSendNotification);
            refStatusLabel.setText("References cleared", juce::dontSendNotification);
            if (currentView == View::Compare)
                showCompareView();
            repaint();
        } else if (sel > 1 && (sel - 2) < presetNames.size()) {
            loadPreset(presetNames[sel - 2]);
        }
    };
    presetBox.setVisible(false);
    addAndMakeVisible(presetBox);
    
    savePresetBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    savePresetBtn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff22d3ee));
    savePresetBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
    savePresetBtn.onClick = [this] {
        auto refs = processorRef.getReferenceAnalyser().getReferences();
        if (refs.empty()) { refStatusLabel.setText("No references to save", juce::dontSendNotification); return; }
        
        // Simple name dialog using AlertWindow
        auto* aw = new juce::AlertWindow("Save Preset", "Name this reference preset:", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor("name", "", "Preset name:");
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
        if (sel > 1 && (sel - 2) < presetNames.size()) {
            juce::String filePath = presetNames[sel - 2];
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
    // The "EchoJay" wordmark used to live here as a 32pt label. It's now
    // painted as the actual PNG logo in paint() (Login branch). We keep
    // the loginTitle Label declared and laid out so the rest of the form
    // spacing is undisturbed, but with empty text it draws nothing.
    loginTitle.setText("", juce::dontSendNotification);
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

    signUpLabel.setText("Don't have an account?", juce::dontSendNotification);
    signUpLabel.setColour(juce::Label::textColourId, C::text3);
    signUpLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    signUpLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(signUpLabel);

    signUpBtn.setColour(juce::TextButton::buttonColourId, C::purple);
    signUpBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    signUpBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    signUpBtn.onClick = [this] {
        juce::URL("https://www.echojay.ai/app").launchInDefaultBrowser();
    };
    addAndMakeVisible(signUpBtn);

    // Restore chat history from processor (persists across editor destroy/recreate)
    for (auto& entry : processorRef.chatHistory)
    {
        ChatMsg cm;
        cm.role = entry.role;
        cm.content = entry.content;
        cm.durationSeconds = entry.durationSeconds;
        cm.lufs = entry.lufs;
        cm.wavFilename = entry.wavFilename;
        cm.wavFilePath = entry.wavFilePath;
        if (entry.hasWaveform && !entry.waveform.empty())
        {
            cm.hasWaveform = true;
            for (auto& peak : entry.waveform)
            {
                WaveformRecorder::ThumbnailPoint pt;
                pt.maxVal = peak;
                pt.minVal = -peak;
                cm.waveform.push_back(pt);
            }
        }
        chatMessages.push_back(cm);
    }

    // Check persisted login
    if (api.isLoggedIn())
    {
        currentScreen = Screen::Main;
        loginTitle.setVisible(false); loginSubtitle.setVisible(false);
        emailInput.setVisible(false); passwordInput.setVisible(false);
        loginBtn.setVisible(false); loginErrorLabel.setVisible(false);
        signUpLabel.setVisible(false); signUpBtn.setVisible(false);
        showMainScreen();
        // No network calls here — periodic refresh in timer handles sync
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
    addChildComponent(chatInput);

    chatSendBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff06b6d4));
    chatSendBtn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff22d3ee));
    chatSendBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
    chatSendBtn.onClick = [this] {
        auto t = chatInput.getText().trim();
        if (t.isNotEmpty()) sendChatMessage(t);
    };
    addChildComponent(chatSendBtn);

    // "Aa" text-size toggle sits in the chat header. Cycles a preset list
    // of scales so users can bump chat readability without a settings trip.
    // Uses a subtle filled background so users can actually spot it.
    loadChatTextScale();
    chatTextSizeBtn.setColour(juce::TextButton::buttonColourId, C::bg3);
    chatTextSizeBtn.setColour(juce::TextButton::buttonOnColourId, C::bg4);
    chatTextSizeBtn.setColour(juce::TextButton::textColourOnId, C::text);
    chatTextSizeBtn.setColour(juce::TextButton::textColourOffId, C::text);
    chatTextSizeBtn.onClick = [this] { cycleChatTextScale(); };
    addChildComponent(chatTextSizeBtn);

    upgradeBtn.setColour(juce::TextButton::buttonColourId, C::purple);
    upgradeBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    upgradeBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    upgradeBtn.onClick = [this] {
        juce::URL("https://www.echojay.ai/?noredirect#pricing").launchInDefaultBrowser();
    };
    addChildComponent(upgradeBtn);

    // CHAIN tab — determine which plugin format is loadable in this host.
    // Hosting a VST3 from inside an AU sandbox (Logic) is blocked by the OS; hosting
    // an AU from inside a VST3 host is equally unsupported. Filter to only the format
    // that matches our own wrapper so users never see dead entries.
    switch (processorRef.wrapperType)
    {
        case juce::AudioProcessor::wrapperType_AudioUnit: chainFormatFilter_ = "AudioUnit"; break;
        case juce::AudioProcessor::wrapperType_VST3:      chainFormatFilter_ = "VST3";      break;
        default: break; // Standalone / unknown — show all
    }
    chainListModel = std::make_unique<ChainPluginListModel>();
    chainListModel->onRowSelected = [this](int) { /* selection handled at load time */ };
    chainListModel->onRowDoubleClicked = [this](int) { chainLoadBtn.triggerClick(); };
    chainPluginList.setModel(chainListModel.get());
    chainPluginList.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff1a1a1a));
    chainPluginList.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff333333));
    chainPluginList.setRowHeight(22);
    addChildComponent(chainPluginList);

    chainSearchBox.setTextToShowWhenEmpty("Search plugins...", juce::Colour(0xff555555));
    chainSearchBox.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1e1e1e));
    chainSearchBox.setColour(juce::TextEditor::textColourId, juce::Colour(0xffcccccc));
    chainSearchBox.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff333333));
    chainSearchBox.onTextChange = [this] {
        auto& ch = processorRef.getChainHost();
        chainListModel->items = ch.getFilteredPlugins(chainSearchBox.getText(), chainFormatFilter_);
        chainPluginList.updateContent();
    };
    addChildComponent(chainSearchBox);

    chainScanBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a4d7a));
    chainScanBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    chainScanBtn.onClick = [this] {
        processorRef.getChainHost().startScan();
    };
    addChildComponent(chainScanBtn);

    chainStatusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff888888));
    chainStatusLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    addChildComponent(chainStatusLabel);

    // Debug: raw chain JSON viewer — shown after each build, temporary
    chainDebugJsonBox.setMultiLine(true, true);
    chainDebugJsonBox.setReadOnly(true);
    chainDebugJsonBox.setScrollbarsShown(true);
    chainDebugJsonBox.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff101810));
    chainDebugJsonBox.setColour(juce::TextEditor::textColourId, juce::Colour(0xff88cc88));
    chainDebugJsonBox.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff2a4a2a));
    chainDebugJsonBox.setFont(juce::Font(juce::FontOptions(9.5f)));
    chainDebugJsonBox.setText("(chain JSON will appear here after Build)", false);
    addChildComponent(chainDebugJsonBox);

    // "Add to Chain" — appends the selected list entry as a new slot
    chainLoadBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a6b2a));
    chainLoadBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    chainLoadBtn.onClick = [this] {
        int row = chainPluginList.getSelectedRow();
        if (row < 0 || row >= chainListModel->items.size()) return;
        auto desc = chainListModel->items[row];
        auto& ch = processorRef.getChainHost();
        chainStatusLabel.setText("Loading " + desc.name + "...", juce::dontSendNotification);
        chainLoadBtn.setEnabled(false);
        chainEditorHolder.statusText = "Loading " + desc.name + "...";
        chainEditorHolder.repaint();
        ch.loadPluginAsync(desc, [this, desc](const juce::String& err)
        {
            chainLoadBtn.setEnabled(true);
            if (err.isNotEmpty())
            {
                chainEditorHolder.statusText = "Failed: " + err;
                chainEditorHolder.repaint();
                chainStatusLabel.setText("Failed: " + err, juce::dontSendNotification);
                return;
            }
            auto& ch2 = processorRef.getChainHost();
            // Select the newly added slot (last one)
            chainSelectedSlot_ = ch2.getNumSlots() - 1;
            chainRackStrip.rebuild(ch2.getAllSlotInfos(), chainSelectedSlot_);
            chainStatusLabel.setText(juce::String(ch2.getNumSlots()) + " slot(s) in chain",
                                     juce::dontSendNotification);
            resized();
            // Show the new slot's editor
            chainEditorHolder.statusText = {};
            chainEditorHolder.setHostedEditor(ch2.createEditorForSlot(chainSelectedSlot_));
            repaint();
        });
    };
    addChildComponent(chainLoadBtn);

    chainRecommendLabel.setColour(juce::Label::textColourId, juce::Colour(0xff557755));
    chainRecommendLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    addChildComponent(chainRecommendLabel);

    addChildComponent(chainEditorHolder);

    // Rack strip — callbacks
    chainRackStrip.onSelectSlot = [this](int i) {
        auto& ch = processorRef.getChainHost();
        if (i < 0 || i >= ch.getNumSlots()) return;
        chainSelectedSlot_ = i;
        chainRackStrip.rebuild(ch.getAllSlotInfos(), chainSelectedSlot_);
        chainEditorHolder.statusText = {};
        chainEditorHolder.settingsHint = ch.getSlotInfo(i).settings;
        chainEditorHolder.setHostedEditor(ch.createEditorForSlot(i));
        repaint();
    };
    chainRackStrip.onRemoveSlot = [this](int i) {
        auto& ch = processorRef.getChainHost();
        ch.removeSlot(i);
        chainSelectedSlot_ = juce::jlimit(-1, ch.getNumSlots() - 1, chainSelectedSlot_);
        chainRackStrip.rebuild(ch.getAllSlotInfos(), chainSelectedSlot_);
        chainEditorHolder.statusText = {};
        chainEditorHolder.settingsHint = chainSelectedSlot_ >= 0
            ? ch.getSlotInfo(chainSelectedSlot_).settings : juce::String{};
        chainEditorHolder.setHostedEditor(chainSelectedSlot_ >= 0
            ? ch.createEditorForSlot(chainSelectedSlot_) : nullptr);
        chainStatusLabel.setText(ch.getNumSlots() > 0
            ? juce::String(ch.getNumSlots()) + " slot(s) in chain"
            : "Chain empty", juce::dontSendNotification);
        repaint();
    };
    chainRackStrip.onBypassSlot = [this](int i) {
        auto& ch = processorRef.getChainHost();
        if (i < 0 || i >= ch.getNumSlots()) return;
        ch.setSlotBypassed(i, !ch.getSlotInfo(i).bypassed);
        chainRackStrip.rebuild(ch.getAllSlotInfos(), chainSelectedSlot_);
        repaint();
    };
    chainRackStrip.onMoveSlot = [this](int i, int dir) {
        auto& ch = processorRef.getChainHost();
        ch.moveSlot(i, dir);
        // Track selection with the moved slot
        int newSel = chainSelectedSlot_;
        if (chainSelectedSlot_ == i) newSel = i + dir;
        chainSelectedSlot_ = juce::jlimit(0, ch.getNumSlots() - 1, newSel);
        chainRackStrip.rebuild(ch.getAllSlotInfos(), chainSelectedSlot_);
        repaint();
    };
    chainRackStrip.onAddClick = [this] {
        // Scroll the plugin list into view and focus it
        chainPluginList.grabKeyboardFocus();
    };
    addChildComponent(chainRackStrip);

    // Warning overlay
    chainWarnLabel.setText("Hosting third-party plugins is experimental.\nSave your project before loading a plugin.",
                           juce::dontSendNotification);
    chainWarnLabel.setColour(juce::Label::textColourId, juce::Colour(0xffdddddd));
    chainWarnLabel.setJustificationType(juce::Justification::centred);
    chainWarnLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    chainWarnOverlay.addAndMakeVisible(chainWarnLabel);
    chainWarnOkBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a4d7a));
    chainWarnOkBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    chainWarnOkBtn.onClick = [this] {
        processorRef.getChainHost().chainWarningDismissed = true;
        chainWarnOverlay.setVisible(false);
    };
    chainWarnOverlay.addAndMakeVisible(chainWarnOkBtn);
    addChildComponent(chainWarnOverlay);

    // Chat sidebar — ListBox + toolbar (Phase 2a/2b).
    sidebarModel = std::make_unique<ChatSidebarModel>();
    sidebarModel->onChatClicked = [this](const juce::String& id)
    {
        loadChatFromWorkspace(id);
    };
    sidebarModel->onAlbumToggled = [this](const juce::String& id)
    {
        if (collapsedAlbums.count(id))
            collapsedAlbums.erase(id);
        else
            collapsedAlbums.insert(id);
        sidebarModel->refreshRows(workspace.getChats(), workspace.getAlbums(),
                                  workspace.getReviews(), collapsedAlbums, currentChatId);
        chatSidebar.updateContent();
        repaint();
    };
    sidebarModel->onChatContextMenu = [this](const juce::String& id)
    {
        showMoveToAlbumMenu(id);
    };
    sidebarModel->onAlbumContextMenu = [this](const juce::String& id)
    {
        showAlbumContextMenu(id);
    };
    chatSidebar.setModel(sidebarModel.get());
    chatSidebar.setRowHeight(36);
    chatSidebar.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff080A12));
    chatSidebar.setColour(juce::ListBox::outlineColourId, juce::Colour(0x00000000));
    chatSidebar.setOutlineThickness(0);
    addChildComponent(chatSidebar);

    // Sidebar toolbar buttons
    sidebarNewChatBtn.onClick = [this]  { createNewChat();  };
    sidebarNewAlbumBtn.onClick = [this] { createNewAlbum(); };
    addChildComponent(sidebarNewChatBtn);
    addChildComponent(sidebarNewAlbumBtn);

    addChildComponent(chatScroll);
    chatScroll.setViewedComponent(&chatContent, false);
    chatScroll.setScrollBarsShown(true, false);
    chatContent.setInterceptsMouseClicks(true, true);
    
    // Repaint the editor on every scroll so the avatars (which we paint
    // ourselves into the editor, not into chatContent) stay in sync with the
    // viewport's repaint regions. Without this, the avatar's right edge tears
    // because JUCE only invalidates part of it when chatScroll scrolls.
    chatScroll.onScroll = [this]() { repaint(); };
    
    // Forward clicks on chat viewport to wave card hit testing
    chatScroll.onClickCheck = [this](const juce::MouseEvent& e) -> bool {
        auto pos = e.getEventRelativeTo(this).getPosition();
        for (auto& wp : chatWavePositions)
        {
            if (wp.bounds.contains(pos))
            {
                int localX = pos.x - wp.bounds.getX();
                int playBtnArea = 30;
                if (localX <= playBtnArea)
                {
                    for (int j = 0; j < activeWavePlayBtns; ++j)
                    {
                        if (wavePlayPaths[(size_t)j] == wp.wavPath)
                        { onWavePlayClick(j); break; }
                    }
                }
                else
                {
                    int wfStart = playBtnArea;
                    int wfWidth = wp.bounds.getWidth() - wfStart - 6;
                    if (wfWidth > 0)
                    {
                        float frac = juce::jlimit(0.0f, 1.0f, (float)(localX - wfStart) / (float)wfWidth);
                        for (int j = 0; j < activeWavePlayBtns; ++j)
                        {
                            if (wavePlayPaths[(size_t)j] == wp.wavPath)
                            { onWaveSeekClick(j, frac); break; }
                        }
                    }
                }
                repaint();
                return true;
            }
        }
        return false;
    };

    // Waveform play overlay buttons
    for (int i = 0; i < kMaxWavePlayBtns; ++i)
    {
        wavePlayOverlays[(size_t)i].setAlpha(0.0f);
        wavePlayOverlays[(size_t)i].setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        wavePlayOverlays[(size_t)i].setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        wavePlayOverlays[(size_t)i].setColour(juce::TextButton::textColourOnId, juce::Colours::transparentBlack);
        wavePlayOverlays[(size_t)i].setColour(juce::TextButton::textColourOffId, juce::Colours::transparentBlack);
        wavePlayOverlays[(size_t)i].setInterceptsMouseClicks(true, false);
        wavePlayOverlays[(size_t)i].setVisible(false);
        wavePlayOverlays[(size_t)i].onClick = [this, i]() {
            auto mousePos = wavePlayOverlays[(size_t)i].getMouseXYRelative();
            int playBtnArea = 30;
            if (mousePos.x <= playBtnArea)
            {
                onWavePlayClick(i);
            }
            else
            {
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

    // "Build this chain" overlay buttons (one per assistant chain reply)
    for (int i = 0; i < kMaxChainBuildBtns; ++i)
    {
        chainBuildBtns[(size_t)i].setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1a2e1a));
        chainBuildBtns[(size_t)i].setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2a4a2a));
        chainBuildBtns[(size_t)i].setColour(juce::TextButton::textColourOffId, juce::Colour(0xff77dd77));
        chainBuildBtns[(size_t)i].setVisible(false);
        chainBuildBtns[(size_t)i].onClick = [this, i]() { loadChainFromJson(chainBuildJsons[(size_t)i]); };
        addAndMakeVisible(chainBuildBtns[(size_t)i]);
    }

    // Link tab has no persistent child components — painted directly.

    startTimerHz(20);
}

EchoJayEditor::~EchoJayEditor() {
    ejTeardownLog("~EchoJayEditor enter");
    // If the user changed ticks and closed without hitting Done, commit the
    // local selection now so it isn't lost (disk persist; server is best-effort
    // and skipped during teardown).
    if ((reviewChecklist && reviewChecklist->hasUncommittedChanges()) ||
        (settingsChecklist && settingsChecklist->hasUncommittedChanges()))
    {
        if (reviewChecklist)   reviewChecklist->commit();
        if (settingsChecklist) settingsChecklist->commit();
        processorRef.getPluginScanner().saveEnabledState();
    }
    ejTeardownLog("editor checklist commit done");
    // Tell any in-flight update download to stop. The alive flag protects
    // against UAF after destruction; the cancel flag lets the worker exit
    // its read loop early instead of finishing the download into a void.
    updateDownloadAlive->store(false);
    updateDownloadCancelled->store(true);
    stopChatPlayback();
    ejTeardownLog("editor playback stopped, stopping GL/timer...");
    stopPlayback(); stopTimer();

    // Detach the OpenGL context HERE, explicitly, while the editor and its
    // child component tree are still fully alive and we are on a clean message
    // thread. Previously detach() only happened later inside ~ParticleVisual,
    // during member destruction (after this destructor body returned, the
    // "members destruct next" point in the teardown log). On Windows, calling
    // OpenGLContext::detach() while the owning component is mid-destruction and
    // continuous repainting is still live deadlocks the message thread against
    // the GL render thread: the host freezes with no crash dump, and only on
    // full plugin removal (window-close/disable never destroys the processor or
    // these members). Driving stop() from here, before particleVisual is
    // destroyed, breaks that ordering and lets detach() complete normally.
    if (particleVisual != nullptr)
    {
        ejTeardownLog("detaching GL context...");
        particleVisual->stop();
        ejTeardownLog("GL context detached");
    }

    // Release any GPU/OpenGL-backed cached images now, during normal teardown,
    // so nothing survives to be freed under the Windows loader lock at DLL
    // unload (which deadlocks the host, see note on getLogoImage()).
    clearLogoImageCache();
    ejTeardownLog("logo/image cache cleared");

    setLookAndFeel(nullptr);
    ejTeardownLog("~EchoJayEditor exit");
}

void EchoJayEditor::visibilityChanged()
{
    if (isVisible())
    {
        // Reset the dismissed flag for hosts that hide/show the editor.
        updateDismissed = false;
        
        // Refresh tier/usage from the server. The common upgrade flow is
        // "user opens browser, pays for Pro, switches back to the DAW" —
        // and switching back to the DAW is exactly when the plugin
        // window regains focus. Triggering a refresh here means upgrades
        // show up the moment the user returns instead of after the
        // 60-second periodic refresh.
        if (api.isLoggedIn())
            api.refreshUserInfo(nullptr);
        
        // Lazily attach the OpenGL context AFTER returning to the host's
        // message loop. Doing this synchronously in the editor constructor
        // (or even directly here) froze Fender Studio Pro on Windows.
        // The 50ms delay is large enough that the host has fully returned
        // from its construction call, small enough to be invisible to
        // users. We use a SafePointer so a quick close-before-attach
        // doesn't UAF the editor.
        if (visualMode)
        {
            juce::Component::SafePointer<EchoJayEditor> safeThis(this);
            juce::Timer::callAfterDelay(50, [safeThis]() {
                if (safeThis == nullptr) return;
                if (safeThis->particleVisual != nullptr)
                    safeThis->particleVisual->start();
            });
        }
    }
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
    signUpLabel.setVisible(true); signUpBtn.setVisible(true);

    juce::Component* mainComps[] = { &captureBtn, &scanBtn,
        &channelTypeBox, &genreBox, &projectInput, &statusLabel, &durationLabel, &detectedLabel,
        &passLabel, &userLabel, &usageLabel, &chatInput, &chatSendBtn, &chatScroll,
        &compareBtn, &settingsBtn, &playbackBtn, &wavSavedLabel, &upgradeBtn };
    for (auto* c : mainComps) c->setVisible(false);
    logoutBtn.setVisible(false);
    settingsName.setVisible(false); settingsMonitors.setVisible(false);
    settingsHeadphones.setVisible(false); settingsGenres.setVisible(false);
    settingsPlugins.setVisible(false); settingsExpLevel.setVisible(false);
    settingsLanguage.setVisible(false);
    saveSettingsBtn.setVisible(false); settingsSavedLabel.setVisible(false);
    for (auto& b : dawButtons) b.setVisible(false);
    settingsPluginViewport.setVisible(false);
    settingsPluginSearchBox.setVisible(false);
    viewAllPluginsBtn.setVisible(false);
    settingsScanBtn.setVisible(false);
    settingsHelpBtn.setVisible(false);
    
    // Also hide compare fields
    aiCompareBtn.setVisible(false);
    compareSlotABox.setVisible(false); compareSlotBBox.setVisible(false); playSlotABtn.setVisible(false); playSlotBBtn.setVisible(false);
    refStatusLabel.setVisible(false);
    presetBox.setVisible(false); savePresetBtn.setVisible(false); deletePresetBtn.setVisible(false); for (auto& b : refRemoveBtns) b.setVisible(false); compareClickCatcher.setVisible(false);

    channelPromptVisible = false;
    genrePromptVisible = false;
    updateChannelPromptVisibility();
    genrePromptTitle.setVisible(false);
    genrePromptSubtitle.setVisible(false);
    genrePromptCustomBtn.setVisible(false);
    for (auto& label : genrePromptGroupLabels) label.setVisible(false);
    for (auto& btn : genrePromptButtons) btn.setVisible(false);
    resized(); repaint();
}

void EchoJayEditor::showMainScreen()
{
    currentScreen = Screen::Main;
    loginTitle.setVisible(false); loginSubtitle.setVisible(false);
    emailInput.setVisible(false); passwordInput.setVisible(false);
    loginBtn.setVisible(false); loginErrorLabel.setVisible(false);
    signUpLabel.setVisible(false); signUpBtn.setVisible(false);

    bool promptWillShow = !processorRef.isChannelTypePromptDismissed()
                          && processorRef.getChannelType() == ChannelType::FullMix;
    bool genrePromptWillShow = !genrePromptDismissedThisSession && !promptWillShow;

    juce::Component* mainComps[] = { &captureBtn, &scanBtn,
        &channelTypeBox, &genreBox, &projectInput, &statusLabel, &durationLabel, &detectedLabel,
        &passLabel, &userLabel, &usageLabel,
        &compareBtn, &settingsBtn, &wavSavedLabel };
    for (auto* c : mainComps) c->setVisible(true);
    // Only show chat if no prompt overlay is covering it
    bool chatVisible = !promptWillShow && !genrePromptWillShow;
    chatInput.setVisible(chatVisible);
    chatSendBtn.setVisible(chatVisible);
    chatTextSizeBtn.setVisible(chatVisible);
    chatScroll.setVisible(chatVisible);
    logoutBtn.setVisible(false); // logout only visible in Settings
    // playbackBtn visibility is managed by timerCallback based on WAV state

    auto info = api.getUserInfo();
    juce::String userText = info.displayName;
    if (info.tierLevel >= 2) userText += "  STUDIO";
    else if (info.tierLevel >= 1) userText += "  PRO";
    userLabel.setText(userText, juce::dontSendNotification);
    userLabel.setColour(juce::Label::textColourId, info.isPro() ? C::purple : C::text2);

    int remaining = api.getRemainingMessages();
    int limit = info.messageLimit;
    int used = limit - remaining;
    juce::String usageStr = juce::String(used) + "/" + juce::String(limit);
    if (info.credits > 0)
        usageStr += " (+" + juce::String(info.credits) + " credits)";
    usageLabel.setText(usageStr, juce::dontSendNotification);

    updateChannelPromptVisibility();
    updateGenrePromptVisibility();
    
    // Pre-fetch user settings so plugin scan won't save empty fields
    if (!settingsFetched) {
        auto safeThis2 = juce::Component::SafePointer<EchoJayEditor>(this);
        api.fetchSettings([safeThis2](bool success) {
            if (safeThis2 && success)
                safeThis2->settingsFetched = true;
        });
    }

    // Load workspace data on plugin open
    workspace.requestLoad();

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

    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);
    api.login(email, password, [safeThis](bool success, const juce::String& error) {
        if (safeThis == nullptr)
            return;
        safeThis->loginLoading = false;
        safeThis->loginBtn.setButtonText("Log In");
        safeThis->loginBtn.setEnabled(true);
        if (success) { safeThis->passwordInput.clear(); safeThis->showMainScreen(); }
        else safeThis->loginErrorLabel.setText(error, juce::dontSendNotification);
        safeThis->repaint();
    });
}

void EchoJayEditor::handleLogout()
{
    api.logout();
    chatMessages.clear(); processorRef.chatHistory.clear(); processorRef.chatRoles.clear(); processorRef.chatContents.clear();
    showLoginScreen();
}

bool EchoJayEditor::shouldShowChannelPrompt() const
{
    return currentScreen == Screen::Main
        && !processorRef.isChannelTypePromptDismissed()
        && processorRef.getChannelType() == ChannelType::FullMix;
}

void EchoJayEditor::updateChannelPromptVisibility()
{
    channelPromptVisible = shouldShowChannelPrompt();

    channelPromptBlocker.setVisible(false); // no longer used for blocking
    channelPromptTitle.setVisible(channelPromptVisible);
    channelPromptSubtitle.setVisible(channelPromptVisible);
    channelPromptSkipBtn.setVisible(channelPromptVisible);
    customChannelBtn.setVisible(channelPromptVisible);
    chatScroll.setVisible(currentScreen == Screen::Main && !channelPromptVisible && !genrePromptVisible);
    chatInput.setVisible(currentScreen == Screen::Main && !channelPromptVisible && !genrePromptVisible);
    chatSendBtn.setVisible(currentScreen == Screen::Main && !channelPromptVisible && !genrePromptVisible);
    chatTextSizeBtn.setVisible(currentScreen == Screen::Main && !channelPromptVisible && !genrePromptVisible);
    
    // Disable top bar action buttons when prompt overlays are showing
    bool promptActive = channelPromptVisible || genrePromptVisible;
    compareBtn.setEnabled(!promptActive);
    captureBtn.setEnabled(!promptActive);
    settingsBtn.setEnabled(!promptActive);

    for (auto& label : channelPromptGroupLabels)
        label.setVisible(channelPromptVisible);

    for (auto& button : channelPromptButtons)
        button.setVisible(channelPromptVisible);

    if (channelPromptVisible)
    {
        channelPromptTitle.toFront(false);
        channelPromptSubtitle.toFront(false);
        for (auto& label : channelPromptGroupLabels)
            label.toFront(false);
        for (auto& button : channelPromptButtons)
            button.toFront(false);
        channelPromptSkipBtn.toFront(false);
        customChannelBtn.toFront(false);
    }

    repaint();
}

void EchoJayEditor::selectChannelPromptType(ChannelType type)
{
    channelTypeBox.setSelectedId(static_cast<int>(type) + 1, juce::sendNotificationSync);
}

void EchoJayEditor::dismissChannelPrompt()
{
    processorRef.setChannelType(ChannelType::FullMix);
    channelTypeBox.setSelectedId(1, juce::dontSendNotification);
    processorRef.setChannelTypePromptDismissed(true);
    updateChannelPromptVisibility();
    compareBtn.setEnabled(true);
    captureBtn.setEnabled(true);
    settingsBtn.setEnabled(true);
    // After channel prompt dismisses, check if genre prompt should show
    updateGenrePromptVisibility();
    resized();
}

// ============================================================================
// Genre Prompt (once per session)
// ============================================================================

bool EchoJayEditor::shouldShowGenrePrompt() const
{
    return currentScreen == Screen::Main
        && !genrePromptDismissedThisSession
        && !channelPromptVisible;  // don't overlap with channel prompt
}

void EchoJayEditor::updateGenrePromptVisibility()
{
    genrePromptVisible = shouldShowGenrePrompt();

    genrePromptTitle.setVisible(genrePromptVisible);
    genrePromptSubtitle.setVisible(genrePromptVisible);
    genrePromptCustomBtn.setVisible(genrePromptVisible);

    for (auto& label : genrePromptGroupLabels)
        label.setVisible(genrePromptVisible);

    for (auto& btn : genrePromptButtons)
        btn.setVisible(genrePromptVisible);

    if (genrePromptVisible)
    {
        // Hide chat and disable action buttons while genre prompt is up
        chatScroll.setVisible(false);
        chatInput.setVisible(false);
        chatSendBtn.setVisible(false);
        chatTextSizeBtn.setVisible(false);
        compareBtn.setEnabled(false);
        captureBtn.setEnabled(false);
        settingsBtn.setEnabled(false);

        genrePromptTitle.toFront(false);
        genrePromptSubtitle.toFront(false);
        for (auto& label : genrePromptGroupLabels)
            label.toFront(false);
        for (auto& btn : genrePromptButtons)
            btn.toFront(false);
        genrePromptCustomBtn.toFront(false);
    }

    repaint();
}

void EchoJayEditor::dismissGenrePrompt(const juce::String& selectedGenre)
{
    genrePromptDismissedThisSession = true;
    processorRef.setGenre(selectedGenre);

    // Sync the genre dropdown — find by text since IDs vary with submenus
    rebuildGenreBox(); // ensure custom genres are in the list
    
    updateGenrePromptVisibility();
    // Restore chat and button state
    chatScroll.setVisible(currentScreen == Screen::Main);
    chatInput.setVisible(currentScreen == Screen::Main);
    chatSendBtn.setVisible(currentScreen == Screen::Main);
    chatTextSizeBtn.setVisible(currentScreen == Screen::Main);
    compareBtn.setEnabled(true);
    captureBtn.setEnabled(true);
    settingsBtn.setEnabled(true);
    resized();
    repaint();
}

// --- UpdateOverlay implementation ---
// This is a real child component so it always paints ON TOP of particleVisual,
// just like the channel/genre prompt UI (which uses real Components/Buttons).
//
// State diagram:
//   Idle           — title + version info + [Download Update] + "Not now"
//   Downloading    — title + version info + progress bar + percentage + Cancel
//   ReadyToInstall — title + "Ready to install"   + [Install Now]      + "Close"
//   Failed         — title + error text           + [Try Again]        + "Close"
//
// We give Downloading / ReadyToInstall / Failed a slightly taller card so
// the progress / status text fits without crowding the buttons.
void EchoJayEditor::UpdateOverlay::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    
    // Dark backdrop covering the whole editor area we occupy
    g.setColour(juce::Colours::black.withAlpha(0.72f));
    g.fillRect(bounds);
    
    // Card centred. Taller in non-idle states to fit progress / error text.
    int cardW = 340;
    int cardH = (state == State::Idle) ? 180 : 200;
    int cardX = (bounds.getWidth() - cardW) / 2;
    int cardY = (bounds.getHeight() - cardH) / 2;
    auto card = juce::Rectangle<int>(cardX, cardY, cardW, cardH);
    
    g.setColour(C::bg2);
    g.fillRoundedRectangle(card.toFloat(), 16.0f);
    g.setColour(C::border2);
    g.drawRoundedRectangle(card.toFloat(), 16.0f, 1.0f);
    
    // Title — same for every state, the body differs
    g.setColour(C::text);
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText("Update Available", card.getX(), card.getY() + 24, card.getWidth(), 24, juce::Justification::centred);
    
    g.setColour(C::text2);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("EchoJay " + latestVersionStr,
               card.getX(), card.getY() + 54, card.getWidth(), 20, juce::Justification::centred);
    g.drawText("You're running v" + currentVersionStr,
               card.getX(), card.getY() + 74, card.getWidth(), 20, juce::Justification::centred);
    
    int btnW = 140, btnH = 34;
    auto primaryBtn = juce::Rectangle<float>(
        (float)(card.getCentreX() - btnW / 2),
        (float)(card.getY() + (state == State::Idle ? 110 : 130)),
        (float)btnW, (float)btnH);
    
    juce::String primaryLabel;
    bool drawProgress = false;
    juce::String secondaryLabel = "Not now";
    
    switch (state)
    {
        case State::Idle:
            primaryLabel = "Download Update";
            break;
        case State::Downloading:
            // Progress bar instead of a button. Cancel link below.
            drawProgress = true;
            primaryLabel = juce::String((int)(progress * 100.0f)) + "%";
            secondaryLabel = "Cancel";
            break;
        case State::ReadyToInstall:
            primaryLabel = "Install Now";
            secondaryLabel = "Close";
            break;
        case State::Failed:
            primaryLabel = "Try Again";
            secondaryLabel = "Close";
            break;
    }
    
    if (drawProgress)
    {
        // Replace the line "EchoJay vX.Y.Z" with a transient "Downloading..."
        // status so the version text doesn't fight the progress bar for
        // attention. We rely on overpainting since the title block above
        // already drew the version — clean it with a bg2 fill first.
        g.setColour(C::bg2);
        g.fillRect(card.getX() + 1, card.getY() + 54, card.getWidth() - 2, 20);
        g.setColour(C::text2);
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText("Downloading EchoJay " + latestVersionStr + "...",
                   card.getX(), card.getY() + 54, card.getWidth(), 20, juce::Justification::centred);
        
        // Progress bar — same dims as the primary button rect for consistency.
        auto barX = primaryBtn.getX();
        auto barY = primaryBtn.getY();
        auto barW = primaryBtn.getWidth();
        auto barH = primaryBtn.getHeight();
        // Track background
        g.setColour(C::bg3);
        g.fillRoundedRectangle(barX, barY, barW, barH, 8.0f);
        // Filled portion — gradient matches the brand button
        float fillW = juce::jlimit(0.0f, 1.0f, progress) * barW;
        if (fillW > 0.5f)
        {
            g.saveState();
            g.reduceClipRegion(juce::Rectangle<int>((int)barX, (int)barY, (int)fillW, (int)barH));
            g.setGradientFill(juce::ColourGradient(C::blue, barX, 0, C::purple, barX + barW, 0, false));
            g.fillRoundedRectangle(barX, barY, barW, barH, 8.0f);
            g.restoreState();
        }
        // Percentage label centred over the bar
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(primaryLabel, primaryBtn.toNearestInt(), juce::Justification::centred);
    }
    else if (state == State::Failed)
    {
        // Show the error text in place of the second info line so the user
        // sees WHY the retry button is there. Errors tend to be short
        // (server msg, "network unavailable", etc.) — single line is fine.
        g.setColour(C::bg2);
        g.fillRect(card.getX() + 1, card.getY() + 54, card.getWidth() - 2, 40);
        g.setColour(juce::Colour(0xffff8a8a)); // soft red
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        auto errText = errorText.isNotEmpty() ? errorText : juce::String("Download failed");
        g.drawText(errText, card.getX() + 18, card.getY() + 60, card.getWidth() - 36, 28,
                   juce::Justification::centredTop);
        
        // Primary button (Try Again) — outline style rather than filled gradient
        // so it doesn't read as "everything's fine".
        g.setColour(C::bg3);
        g.fillRoundedRectangle(primaryBtn, 8.0f);
        g.setColour(C::border2);
        g.drawRoundedRectangle(primaryBtn, 8.0f, 1.0f);
        g.setColour(C::text);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(primaryLabel, primaryBtn.toNearestInt(), juce::Justification::centred);
    }
    else
    {
        // Idle and ReadyToInstall both use the brand gradient button.
        if (state == State::ReadyToInstall)
        {
            // Subtitle change: instead of "You're running vX" show "Ready to install"
            g.setColour(C::bg2);
            g.fillRect(card.getX() + 1, card.getY() + 74, card.getWidth() - 2, 20);
            g.setColour(C::text3);
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText("Ready to install. The installer will open next.",
                       card.getX(), card.getY() + 74, card.getWidth(), 20, juce::Justification::centred);
        }
        g.setGradientFill(juce::ColourGradient(C::blue, primaryBtn.getX(), 0, C::purple, primaryBtn.getRight(), 0, false));
        g.fillRoundedRectangle(primaryBtn, 8.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(primaryLabel, primaryBtn.toNearestInt(), juce::Justification::centred);
    }
    
    // Secondary link at the bottom of the card
    int secY = card.getY() + cardH - 26;
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText(secondaryLabel, card.getX(), secY, card.getWidth(), 20, juce::Justification::centred);
}

void EchoJayEditor::UpdateOverlay::mouseDown(const juce::MouseEvent& e)
{
    auto pos = e.getPosition();
    auto bounds = getLocalBounds();
    int cardW = 340;
    int cardH = (state == State::Idle) ? 180 : 200;
    int cardX = (bounds.getWidth() - cardW) / 2;
    int cardY = (bounds.getHeight() - cardH) / 2;
    
    int btnW = 140, btnH = 34;
    int btnX = bounds.getWidth() / 2 - btnW / 2;
    int btnY = cardY + (state == State::Idle ? 110 : 130);
    
    bool inPrimary = (pos.x >= btnX && pos.x <= btnX + btnW
                   && pos.y >= btnY && pos.y <= btnY + btnH);
    int secY = cardY + cardH - 26;
    bool inSecondary = (pos.y >= secY && pos.y <= secY + 20
                     && pos.x >= cardX && pos.x <= cardX + cardW);
    
    // Click outside the card — treat as dismiss/close. We use onDismiss
    // for this in every state; Cancel-during-download also flows through
    // onDismiss because cancelling AND closing the overlay are the same
    // user intent here.
    bool outsideCard = (pos.x < cardX || pos.x > cardX + cardW
                     || pos.y < cardY || pos.y > cardY + cardH);
    
    // Primary button action depends on state.
    if (inPrimary)
    {
        switch (state)
        {
            case State::Idle:
                if (onDownload) onDownload();
                break;
            case State::Downloading:
                // Clicking the progress bar does nothing — only the
                // explicit Cancel link cancels.
                break;
            case State::ReadyToInstall:
                if (onInstall) onInstall();
                break;
            case State::Failed:
                if (onRetry) onRetry();
                break;
        }
        return;
    }
    
    if (inSecondary || outsideCard)
    {
        if (onDismiss) onDismiss();
        return;
    }
    // Click inside card but not on a control — consume silently.
}

// ============================================================================
// Plugin review overlay
// ============================================================================
// A dark backdrop + card containing the scrollable plugin checklist. Shown
// after a scan so the user can untick plugins they don't own. The viewport
// (holding the shared PluginChecklistComponent) is positioned by
// layoutPluginReview(); this overlay paints the card chrome and the buttons
// around it, and hit-tests clicks on those buttons.

void EchoJayEditor::PluginReviewOverlay::paint(juce::Graphics& g)
{
    // Dim everything behind the card.
    g.fillAll(juce::Colours::black.withAlpha(0.6f));

    // Card.
    g.setColour(C::bg2);
    g.fillRoundedRectangle(cardBounds.toFloat(), 12.0f);
    g.setColour(C::border2);
    g.drawRoundedRectangle(cardBounds.toFloat(), 12.0f, 1.0f);

    int pad = 18;
    int x = cardBounds.getX() + pad;
    int w = cardBounds.getWidth() - pad * 2;
    int y = cardBounds.getY() + pad;

    // Title + close.
    g.setColour(C::text);
    g.setFont(juce::Font(juce::FontOptions(17.0f)).boldened());
    g.drawText("Review your plugins", x, y, w - 24, 24, juce::Justification::centredLeft);

    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(18.0f)));
    g.drawText("X", closeBtn, juce::Justification::centred);

    y += 30;
    g.setColour(C::text2);
    g.setFont(juce::Font(juce::FontOptions(12.5f)));
    g.drawFittedText(
        "Untick anything you don't actually own or don't want suggested. "
        "Waves plugins are listed from your WaveShell, so untick any you're "
        "not licensed for. You can change this any time in Settings.",
        x, y, w, 40, juce::Justification::topLeft, 3);

    // Select all / none + add, drawn as text buttons.
    auto drawBtn = [&g](juce::Rectangle<int> r, const juce::String& label,
                        juce::Colour bg, juce::Colour fg)
    {
        g.setColour(bg);
        g.fillRoundedRectangle(r.toFloat(), 6.0f);
        g.setColour(fg);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawText(label, r, juce::Justification::centred);
    };
    drawBtn(allBtn,  "Tick all",   C::bg4, C::text2);
    drawBtn(noneBtn, "Untick all", C::bg4, C::text2);
    drawBtn(addBtn,  "+ Add plugin", C::bg4, C::text2);
    drawBtn(doneBtn, "Done", C::purple, juce::Colours::white);

    // "Showing N of M — search to narrow" hint, drawn just under the search
    // box when the list was capped. searchBounds is set by layoutPluginReview.
    if (hintText.isNotEmpty())
    {
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(hintText,
                   searchBounds.getX(), searchBounds.getBottom() + 2,
                   searchBounds.getWidth(), 14,
                   juce::Justification::centredLeft);
    }
}

void EchoJayEditor::PluginReviewOverlay::mouseDown(const juce::MouseEvent& e)
{
    auto p = e.getPosition();
    if (closeBtn.contains(p) || doneBtn.contains(p)) { if (onDone) onDone(); return; }
    if (allBtn.contains(p))   { if (onSelectAll) onSelectAll(true);  return; }
    if (noneBtn.contains(p))  { if (onSelectAll) onSelectAll(false); return; }
    if (addBtn.contains(p))   { if (onAddManual) onAddManual();      return; }
    // Click on the dark backdrop (outside the card) dismisses.
    if (! cardBounds.contains(p)) { if (onDone) onDone(); return; }
}

void EchoJayEditor::showScanMenu(juce::Component* target)
{
    auto& sc = processorRef.getPluginScanner();
    juce::PopupMenu menu;
    menu.addItem(1, "Scan Now", ! sc.isScanning());
    menu.addItem(2, "Add Folder...");

    auto folders = sc.getCustomFolders();
    if (folders.size() > 0)
    {
        menu.addSeparator();
        juce::PopupMenu removeSub;
        for (int i = 0; i < folders.size(); ++i)
        {
            auto path = folders[i];
            auto leaf = juce::File(path).getFileName();
            removeSub.addItem(100 + i, "Remove: " + leaf);
        }
        menu.addSubMenu("Custom Folders (" + juce::String(folders.size()) + ")", removeSub);
    }

    juce::Component::SafePointer<EchoJayEditor> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target),
        [safeThis](int result) {
            if (safeThis == nullptr) return;
            auto* scanner = &safeThis->processorRef.getPluginScanner();
            if (result == 1)
            {
                scanner->startScan();
            }
            else if (result == 2)
            {
                auto chooser = std::make_shared<juce::FileChooser>(
                    "Add Plugin Scan Folder",
                    juce::File::getSpecialLocation(juce::File::userHomeDirectory));
                chooser->launchAsync(juce::FileBrowserComponent::openMode
                                   | juce::FileBrowserComponent::canSelectDirectories,
                    [safeThis, chooser](const juce::FileChooser& fc) {
                        if (safeThis == nullptr) return;
                        auto picked = fc.getResult();
                        if (picked.isDirectory())
                        {
                            auto* scannerPtr = &safeThis->processorRef.getPluginScanner();
                            scannerPtr->addCustomFolder(picked);
                            scannerPtr->startScan();
                        }
                    });
            }
            else if (result >= 100)
            {
                int idx = result - 100;
                auto folders = scanner->getCustomFolders();
                if (idx >= 0 && (size_t)idx < folders.size())
                {
                    scanner->removeCustomFolder(folders[idx]);
                    scanner->startScan();
                }
            }
        });
}

void EchoJayEditor::showPluginReview()
{
    reviewOverlay.visibleState = true;
    reviewOverlay.setVisible(true);
    resized(); // hide the (OpenGL) particle visualiser so it can't composite
               // over the overlay; also lays everything out
    reviewOverlay.toFront(false);
    layoutPluginReview();
    reviewOverlay.repaint();
    // Let the user type a filter immediately.
    reviewSearchBox.grabKeyboardFocus();
}

void EchoJayEditor::hidePluginReview()
{
    reviewOverlay.visibleState = false;
    reviewOverlay.setVisible(false);
    resized(); // bring the particle visualiser back
    repaint();
}

void EchoJayEditor::layoutPluginReview()
{
    reviewOverlay.setBounds(getLocalBounds());

    // Card centered, generous but bounded.
    int cardW = juce::jmin(520, getWidth() - 60);
    int cardH = juce::jmin(560, getHeight() - 60);
    int cardX = (getWidth() - cardW) / 2;
    int cardY = (getHeight() - cardH) / 2;
    reviewOverlay.cardBounds = { cardX, cardY, cardW, cardH };

    int pad = 18;
    // Close button: top-right of card.
    reviewOverlay.closeBtn = { cardX + cardW - pad - 22, cardY + pad, 22, 22 };

    // Search box: below the title + blurb (title ~30h, blurb ~48h).
    int searchY = cardY + pad + 30 + 48;
    int searchH = 30;
    reviewOverlay.searchBounds = { cardX + pad, searchY, cardW - pad * 2, searchH };
    reviewSearchBox.setBounds(reviewOverlay.searchBounds);

    // Hint line under the search box. When a filter is active, show how many
    // of the total it matches; otherwise show the total library size.
    int hintH = 16;
    bool showHint = reviewChecklist != nullptr;
    if (reviewChecklist)
    {
        bool filtering = reviewSearchBox.getText().trim().isNotEmpty();
        reviewOverlay.hintText = filtering
            ? ("Showing " + juce::String(reviewChecklist->getMatchCount()) + " of "
               + juce::String(reviewChecklist->getTotalCount()) + " plugins")
            : (juce::String(reviewChecklist->getTotalCount()) + " plugins — tap a section to expand");
    }

    // List viewport: below search (+ hint), above the button row.
    int listTop = searchY + searchH + 6 + (showHint ? hintH : 0);
    int btnRowH = 34;
    int listBottom = cardY + cardH - pad - btnRowH - 10;
    int listX = cardX + pad;
    int listW = cardW - pad * 2;
    reviewViewport.setBounds(listX, listTop, listW, juce::jmax(40, listBottom - listTop));
    if (reviewChecklist)
        reviewChecklist->setBounds(0, 0, listW - 12, reviewChecklist->getContentHeight());

    // Button row.
    int by = cardY + cardH - pad - btnRowH;
    reviewOverlay.allBtn  = { cardX + pad,            by, 80,  btnRowH };
    reviewOverlay.noneBtn = { cardX + pad + 88,       by, 84,  btnRowH };
    reviewOverlay.addBtn  = { cardX + pad + 180,      by, 96,  btnRowH };
    reviewOverlay.doneBtn = { cardX + cardW - pad - 90, by, 90, btnRowH };
}


//
// Flow: user clicks Download Update → we resolve the platform-specific URL
// from the remote config → spawn a background thread that streams the
// response body to a file in ~/Downloads (or %USERPROFILE%\Downloads on
// Windows) → on completion, flip the overlay to ReadyToInstall.
//
// When the user then clicks Install Now, we hand the file off to
// Process::openDocument which on macOS launches the .pkg in Installer.app
// (admin password prompt and wizard included) and on Windows runs the
// .exe installer. The DAW restart at the end is still the user's job —
// no way around that without a separate launcher app.

void EchoJayEditor::startUpdateDownload()
{
    // Resolve URL + target file name for the current platform.
   #if JUCE_MAC
    auto downloadUrl = EchoJayAPI::downloadUrlMac;
    auto extHint = ".pkg";
   #elif JUCE_WINDOWS
    auto downloadUrl = EchoJayAPI::downloadUrlWin;
    auto extHint = ".exe";
   #else
    auto downloadUrl = juce::String();
    auto extHint = "";
   #endif
    
    if (downloadUrl.isEmpty())
    {
        updateOverlay.state = UpdateOverlay::State::Failed;
        updateOverlay.errorText = "No installer URL available for this platform.";
        repaint();
        return;
    }
    
    // Pick a sensible destination filename. Prefer the leaf of the URL
    // so version info from the URL is preserved (e.g. EchoJay-v1.3.0-Installer.pkg).
    // If the URL doesn't end in a clean filename, fall back to a synthesised one.
    juce::String leaf;
    {
        auto u = downloadUrl;
        int slash = u.lastIndexOfChar('/');
        if (slash > 0) leaf = u.substring(slash + 1);
        // Strip query string and fragment off the end.
        int q = leaf.indexOfChar('?');
        if (q > 0) leaf = leaf.substring(0, q);
        int h = leaf.indexOfChar('#');
        if (h > 0) leaf = leaf.substring(0, h);
        if (! leaf.endsWithIgnoreCase(extHint))
            leaf = "EchoJay-v" + EchoJayAPI::latestVersion + "-Installer" + extHint;
    }
    auto downloadsDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                            .getChildFile("Downloads");
    if (! downloadsDir.isDirectory())
        downloadsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto destFile = downloadsDir.getChildFile(leaf);
    
    // Flip the overlay to Downloading and kick the worker.
    updateOverlay.state = UpdateOverlay::State::Downloading;
    updateOverlay.progress = 0.0f;
    updateOverlay.errorText.clear();
    repaint();
    
    // Capture alive token + cancel flag + safe self-pointer so the worker
    // can talk back to the UI thread without holding `this` directly.
    // Worker MUST NOT touch any editor members directly — every UI update
    // goes through MessageManager::callAsync.
    auto aliveCopy = updateDownloadAlive;
    auto cancelCopy = updateDownloadCancelled;
    cancelCopy->store(false);   // fresh start for this download
    auto urlCopy = downloadUrl;
    auto destPath = destFile.getFullPathName();
    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);
    
    juce::Thread::launch([aliveCopy, cancelCopy, urlCopy, destPath, safeThis]() {
        auto reportFailure = [aliveCopy, safeThis](juce::String msg) {
            if (! aliveCopy->load()) return;
            juce::MessageManager::callAsync([aliveCopy, safeThis, msg]() {
                ejTeardownLog("[callAsync] download-fail firing");
                if (! aliveCopy->load()) { ejTeardownLog("[callAsync] download-fail: alive=false, bailing"); return; }
                if (safeThis == nullptr) { ejTeardownLog("[callAsync] download-fail: safeThis=null, bailing"); return; }
                safeThis->updateOverlay.state = UpdateOverlay::State::Failed;
                safeThis->updateOverlay.errorText = msg;
                safeThis->repaint();
                ejTeardownLog("[callAsync] download-fail done");
            });
        };
        
        juce::URL url(urlCopy);
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs(60000)
                           .withStatusCode(&statusCode);
        auto stream = url.createInputStream(options);
        if (stream == nullptr || statusCode < 200 || statusCode >= 400)
        {
            reportFailure("Couldn't reach the download server (HTTP "
                          + juce::String(statusCode) + ").");
            return;
        }
        
        juce::int64 totalBytes = stream->getTotalLength(); // may be -1 if server didn't send Content-Length
        juce::int64 readSoFar = 0;
        
        juce::File destFile(destPath);
        // If there's an old partial in the way, replace it.
        if (destFile.existsAsFile()) destFile.deleteFile();
        std::unique_ptr<juce::FileOutputStream> out(destFile.createOutputStream());
        if (out == nullptr || out->failedToOpen())
        {
            reportFailure("Couldn't write to " + destFile.getFullPathName() + ".");
            return;
        }
        
        // Stream in chunks. 64KB is plenty for ~16MB installers and keeps
        // the progress bar smooth without spamming the UI thread.
        constexpr int kChunk = 64 * 1024;
        juce::HeapBlock<char> buffer(kChunk);
        double lastReportSec = 0.0;
        while (! stream->isExhausted())
        {
            if (! aliveCopy->load()) { destFile.deleteFile(); return; }
            
            // User clicked Cancel/Close while we were downloading. The
            // dismiss callback flips this flag; the worst case is one
            // extra chunk before we notice and bail out.
            if (cancelCopy->load())
            {
                destFile.deleteFile();
                return;
            }
            
            int got = stream->read(buffer.getData(), kChunk);
            if (got <= 0) break;
            if (! out->write(buffer.getData(), (size_t)got))
            {
                reportFailure("Disk write failed (out of space?).");
                return;
            }
            readSoFar += got;
            
            // Throttle UI updates to ~10/sec — anything more is wasted
            // repaints. Always force a final report when done.
            double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
            if (nowSec - lastReportSec > 0.1 || (totalBytes > 0 && readSoFar >= totalBytes))
            {
                lastReportSec = nowSec;
                float frac = totalBytes > 0
                               ? (float)((double)readSoFar / (double)totalBytes)
                               : 0.0f;
                if (! aliveCopy->load()) { destFile.deleteFile(); return; }
                juce::MessageManager::callAsync([aliveCopy, safeThis, frac]() {
                    ejTeardownLog("[callAsync] download-progress firing");
                    if (! aliveCopy->load()) { ejTeardownLog("[callAsync] download-progress: alive=false, bailing"); return; }
                    if (safeThis == nullptr) { ejTeardownLog("[callAsync] download-progress: safeThis=null, bailing"); return; }
                    if (safeThis->updateOverlay.state != UpdateOverlay::State::Downloading) return;
                    safeThis->updateOverlay.progress = juce::jlimit(0.0f, 1.0f, frac);
                    safeThis->repaint();
                    ejTeardownLog("[callAsync] download-progress done");
                });
            }
        }
        out->flush();
        out.reset();
        
        if (! aliveCopy->load()) { destFile.deleteFile(); return; }
        
        // Sanity check: did we end up with a non-empty file? An empty
        // result is treated as failure even if the HTTP status was 200.
        if (! destFile.existsAsFile() || destFile.getSize() < 1024)
        {
            destFile.deleteFile();
            reportFailure("Downloaded file was empty or truncated.");
            return;
        }
        
        // Success — flip state and stash the path for the install step.
        juce::MessageManager::callAsync([aliveCopy, safeThis, destPath]() {
            ejTeardownLog("[callAsync] download-success firing");
            if (! aliveCopy->load()) { ejTeardownLog("[callAsync] download-success: alive=false, bailing"); return; }
            if (safeThis == nullptr) { ejTeardownLog("[callAsync] download-success: safeThis=null, bailing"); return; }
            safeThis->downloadedInstallerFile = juce::File(destPath);
            safeThis->updateOverlay.state = UpdateOverlay::State::ReadyToInstall;
            safeThis->updateOverlay.progress = 1.0f;
            safeThis->repaint();
            ejTeardownLog("[callAsync] download-success done");
        });
    });
}

void EchoJayEditor::launchDownloadedInstaller()
{
    if (! downloadedInstallerFile.existsAsFile())
    {
        updateOverlay.state = UpdateOverlay::State::Failed;
        updateOverlay.errorText = "The downloaded installer is missing.";
        repaint();
        return;
    }
    
    // Process::openDocument hands the file to the OS's default handler:
    // macOS → Installer.app for .pkg (DAW user is then prompted for admin
    // creds and clicks through the wizard); Windows → ShellExecute on .exe
    // which runs the NSIS/Inno installer. We dismiss our overlay so the
    // user sees the system installer cleanly. They'll need to restart
    // their DAW after install — that's still on them.
    bool launched = juce::Process::openDocument(downloadedInstallerFile.getFullPathName(),
                                                  juce::String());
    if (! launched)
    {
        updateOverlay.state = UpdateOverlay::State::Failed;
        updateOverlay.errorText = "Couldn't launch the installer. Open it from your Downloads folder.";
        repaint();
        return;
    }
    
    // Treat "installer opened" the same as the user choosing to update —
    // suppress the overlay for this version so we don't nag them again
    // on the next timer tick.
    updateDismissed = true;
    recordUpdateDismissal(EchoJayAPI::latestVersion);
    updateAvailable = false;
    updateOverlay.setVisible(false);
    resized();
    repaint();
}


void EchoJayEditor::paintGenrePromptOverlay(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour(juce::Colours::black.withAlpha(0.72f));
    g.fillRect(bounds);

    auto card = bounds.reduced(60, 50);
    g.setColour(C::bg2);
    g.fillRoundedRectangle(card.toFloat(), 16.0f);
    g.setColour(C::border2);
    g.drawRoundedRectangle(card.toFloat(), 16.0f, 1.0f);

    auto accent = card.removeFromTop(6).reduced(24, 0);
    g.setColour(C::purple.withAlpha(0.85f));
    g.fillRoundedRectangle(accent.removeFromLeft(card.getWidth() / 3).toFloat(), 3.0f);
    g.setColour(C::blue.withAlpha(0.85f));
    g.fillRoundedRectangle(accent.toFloat(), 3.0f);
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
            
            // Copy file to EchoJay folder to avoid sandbox/permission issues.
            // Always overwrite — the source file may have changed.
            auto destFolder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                  .getChildFile("EchoJay").getChildFile("References");
            destFolder.createDirectory();
            auto destFile = destFolder.getChildFile(file.getFileName());
            file.copyFileTo(destFile);
            
            auto fileToAnalyse = destFile.existsAsFile() ? destFile : file;
            
            // Safety: if a previous analysis got stuck, force-reset the flag
            auto& analyser = processorRef.getReferenceAnalyser();
            analyser.forceResetIfStuck();
            
            analyser.analyseFile(fileToAnalyse, [this, file](bool success, const juce::String& error) {
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
    compareBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
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
    compareBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    aiCompareBtn.setVisible(false);
    // Don't stop AB playback — let ref keep playing through plugin when switching views
    compareSlotABox.setVisible(false); compareSlotBBox.setVisible(false); playSlotABtn.setVisible(false); playSlotBBtn.setVisible(false);
    refStatusLabel.setVisible(false);
    presetBox.setVisible(false); savePresetBtn.setVisible(false); deletePresetBtn.setVisible(false); for (auto& b : refRemoveBtns) b.setVisible(false); compareClickCatcher.setVisible(false);
    resized(); repaint();
}

void EchoJayEditor::runAICompare()
{
    if (!api.canSendMessage()) {
        auto msg = api.getLimitReachedMessage();
        chatMessages.push_back({"assistant", msg});
        processorRef.chatHistory.push_back({"assistant", msg});
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
        if (ia >= 0 && ia < (int)snaps.size() && ib >= 0 && ib < (int)snaps.size() && ia != ib)
            compareCtx = processorRef.buildCompareContext(snaps[ia], snaps[ib]);
    } else if (aIsRef && !bIsRef) {
        int ri = idA - refOffset, ci = idB - 1;
        if (ci >= 0 && ci < (int)snaps.size() && ri >= 0 && ri < (int)refs.size())
            compareCtx = processorRef.buildCompareContext(snaps[ci], refs[ri]);
    } else if (aIsRef && bIsRef) {
        int ra = idA - refOffset, rb = idB - refOffset;
        if (ra >= 0 && ra < (int)refs.size() && rb >= 0 && rb < (int)refs.size() && ra != rb)
            compareCtx = processorRef.buildCompareContext(refs[ra], refs[rb]);
    }

    if (compareCtx.isEmpty()) {
        chatMessages.push_back({"assistant", "Select two different items to compare."});
        processorRef.chatHistory.push_back({"assistant", "Select two different items to compare."});
        repaint(); return;
    }

    chatMessages.push_back({"user", "Compare these mixes"});
    processorRef.chatHistory.push_back({"user", "Compare these mixes"});
    chatLoading = true; repaint();
    processorRef.chatRoles.add("user");
    processorRef.chatContents.add("Give me a detailed comparison of these two.\n\n" + compareCtx);

    auto sysPrompt = EchoJayAPI::buildSystemPrompt(
        processorRef.getEffectiveChannelName(), processorRef.getGenre(),
        processorRef.getPluginScanner().getPluginSummary());

    // SafePointer guard. The API's alive flag protects the API object's
    // lifetime, but the API outlives the editor: when the user removes the
    // plugin, Cubase destroys the editor first and keeps the processor (and
    // API) around for many seconds while it saves state. An in-flight chat
    // request that completes in that window fires this callback against a
    // destroyed editor. The lambda then sits in the host message queue and
    // runs on the user's NEXT mouse click — the "1 second after first click"
    // freeze. Bailing on a null SafePointer makes the late callback a no-op.
    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);
    api.sendChat(processorRef.chatRoles, processorRef.chatContents, sysPrompt,
        [safeThis](const juce::String& reply, bool success) {
            if (safeThis == nullptr) return;
            safeThis->chatLoading = false;
            safeThis->chatMessages.push_back({"assistant", reply});
            safeThis->processorRef.chatHistory.push_back({"assistant", reply});
            if (success) { safeThis->processorRef.chatRoles.add("assistant"); safeThis->processorRef.chatContents.add(reply); }
            safeThis->repaint();
        });
}

// =============================================================================
//  Link tab painter — auto-discovery list
// =============================================================================
void EchoJayEditor::paintLinkMonitorPanel(juce::Graphics& g, juce::Rectangle<int> area)
{
    using C = EchoJayLookAndFeel::Colours;
    const int pad = 32;
    int y = area.getY() + 16;
    const int fw = area.getWidth() - pad * 2;

    // Title
    g.setColour(C::blue2);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    g.drawText("LINK MONITOR", pad, y, fw, 18, juce::Justification::centredLeft);
    y += 26;

    // Consumer diagnostic bar
    {
        const auto& cd = processorRef.consumerDiag;
        juce::String regStr = cd.regOpened ? "opened"
                            : (cd.regKey.isEmpty() ? "not tried"
                                                   : "FAILED errno " + juce::String(cd.regErrno));
        juce::String diagLine = "registry: " + regStr
                              + "  |  active slots: " + juce::String(cd.activeSlotCount)
                              + "  |  names: " + (cd.nameList.isEmpty() ? "(none)" : cd.nameList);
        juce::String keyLine  = "dir: " + (cd.regKey.isEmpty() ? "(none)" : cd.regKey);

        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.drawText(diagLine, pad, y,      fw, 14, juce::Justification::centredLeft);
        g.drawText(keyLine,  pad, y + 14, fw, 13, juce::Justification::centredLeft);
    }
    y += 34;

    // Host channel card — always shown first, above Links
    {
        const int cardH = 46;
        g.setColour(C::bg3.brighter(0.04f));
        g.fillRoundedRectangle((float)pad, (float)y, (float)fw, (float)cardH, 6.f);
        g.setColour(C::blue2.withAlpha(0.4f));
        g.drawRoundedRectangle((float)pad + 0.5f, (float)y + 0.5f, (float)fw - 1.f, (float)cardH - 1.f, 6.f, 1.f);

        juce::String hostLabel = processorRef.getEffectiveChannelName();
        if (hostLabel.isEmpty()) hostLabel = "Host Channel";

        g.setColour(C::text);
        g.setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
        g.drawText(hostLabel, pad + 12, y + 8, fw - 24, 16, juce::Justification::centredLeft);

        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        auto md = processorRef.getMeterEngine().getMeterData();
        juce::String hostStats = "Integrated: " + juce::String(md.integrated, 1) + " LUFS"
            + "  Crest: " + juce::String(md.crestFactor, 1) + " dB";
        g.drawText(hostStats, pad + 12, y + 26, fw - 24, 13, juce::Justification::centredLeft);

        y += cardH + 6;
    }

    const auto& slots = processorRef.getLinkSlotInfos();

    if (slots.empty())
    {
        // Empty state
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        const int emptyY = area.getY() + area.getHeight() / 2 - 20;
        g.drawText("No active Links detected.", pad, emptyY, fw, 18,
                   juce::Justification::centred);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText("Add an EchoJay Link plugin to a channel and turn it on.",
                   pad, emptyY + 22, fw, 14, juce::Justification::centred);
        return;
    }

    // One card per slot
    const int cardH = 54;
    const int dotD  = 10;

    for (const auto& slot : slots)
    {
        const int cardX = pad;
        const int cardW = fw;

        // Card background
        g.setColour(C::bg3);
        g.fillRoundedRectangle((float)cardX, (float)y, (float)cardW, (float)cardH, 6.f);
        g.setColour(C::border2);
        g.drawRoundedRectangle((float)cardX, (float)y, (float)cardW, (float)cardH, 6.f, 1.f);

        // Connection dot
        const juce::Colour dotCol = slot.connected
            ? juce::Colour(0xff22c55e)   // green
            : juce::Colour(0xffef4444);  // red
        const float dotX = (float)(cardX + cardW - dotD - 10);
        const float dotY = (float)(y + (cardH - dotD) / 2);
        g.setColour(dotCol.withAlpha(0.25f));
        g.fillEllipse(dotX - 3.f, dotY - 3.f, (float)(dotD + 6), (float)(dotD + 6));
        g.setColour(dotCol);
        g.fillEllipse(dotX, dotY, (float)dotD, (float)dotD);

        // Name
        g.setColour(C::text);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(slot.name.isEmpty() ? "(unnamed)" : slot.name,
                   cardX + 14, y + 8, cardW - dotD - 36, 18,
                   juce::Justification::centredLeft);

        // Stats line
        juce::String stats;
        if (slot.connected)
            stats = "sr " + juce::String((int)slot.sampleRate)
                  + "  |  frames " + juce::String(slot.framesRead);
        else
            stats = "waiting for audio...";

        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(stats, cardX + 14, y + 30, cardW - dotD - 36, 14,
                   juce::Justification::centredLeft);

        y += cardH + 8;
        if (y > area.getBottom() - cardH) break; // don't overflow
    }
}

// =============================================================================

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
        g.setColour(juce::Colour(0xffFF6B9D));
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
            g.setColour(juce::Colour(0xffFF6B9D).withAlpha(0.15f));
            g.fillRoundedRectangle((float)tagX, (float)tagY2, (float)tagW, (float)tagH, 6.0f);
            g.setColour(juce::Colour(0xffFF6B9D).withAlpha(0.4f));
            g.drawRoundedRectangle((float)tagX + 0.5f, (float)tagY2 + 0.5f, (float)tagW - 1.0f, (float)tagH - 1.0f, 6.0f, 1.0f);
            
            // Name (truncated)
            g.setColour(juce::Colour(0xffFF8FAB));
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
            bool playing = (currentlyPlayingChatWav == wavPath && wavPath.isNotEmpty())
                        || (processorRef.abPlayingRef.load() && processorRef.abFilePath == wavPath);
            g.setColour(playing ? (isRef ? juce::Colour(0xffFF6B9D).withAlpha(0.5f) : juce::Colour(0xff06b6d4).withAlpha(0.4f)) : (isRef ? juce::Colour(0xffFF6B9D).withAlpha(0.3f) : juce::Colour(0xff06b6d4).withAlpha(0.15f)));
            g.fillEllipse((float)playX2, (float)playY2, (float)playR, (float)playR);
            g.setColour(juce::Colours::white);
            if (playing) {
                // Pause icon: two vertical bars
                g.fillRect((float)playX2 + 5.0f, (float)playY2 + 5.0f, 3.0f, 8.0f);
                g.fillRect((float)playX2 + 10.0f, (float)playY2 + 5.0f, 3.0f, 8.0f);
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
                
                // Calculate play fraction for colouring
                float cardPlayFrac = -1.0f;
                if (playing) {
                    if (processorRef.abPlayingRef.load() && processorRef.abSampleCount > 0)
                        cardPlayFrac = (float)processorRef.abPlaybackPos / (float)processorRef.abSampleCount;
                    else if (chatPlaybackDuration > 0 && chatPlaybackStartTime > 0) {
                        double el2 = (juce::Time::getMillisecondCounterHiRes() - chatPlaybackStartTime) / 1000.0 + chatPlaybackOffset;
                        cardPlayFrac = (float)(el2 / chatPlaybackDuration);
                    }
                    cardPlayFrac = juce::jlimit(0.0f, 1.0f, cardPlayFrac);
                }
                
                for (int i2 = 0; i2 < numPts; ++i2)
                {
                    float px = (float)wfStartX + (float)i2 * pxPerPt;
                    float h = waveform[(size_t)i2] * halfH2;
                    float frac = (float)i2 / (float)numPts;
                    
                    if (cardPlayFrac >= 0 && frac <= cardPlayFrac) {
                        // Played portion
                        if (isRef) {
                            juce::Colour playedCol = juce::Colour(0xffFF6B9D).interpolatedWith(
                                juce::Colour(0xffFF8FAB), frac / juce::jmax(0.01f, cardPlayFrac));
                            g.setColour(playedCol.withAlpha(0.85f));
                        } else {
                            juce::Colour playedCol = juce::Colour(0xff06b6d4).interpolatedWith(
                                juce::Colour(0xff22d3ee), frac / juce::jmax(0.01f, cardPlayFrac));
                            g.setColour(playedCol.withAlpha(0.85f));
                        }
                    } else {
                        // Unplayed or not playing
                        if (isRef)
                            g.setColour(juce::Colour(0xffFF6B9D).withAlpha(playing ? 0.2f : 0.5f));
                        else
                            g.setColour(C::blue.withAlpha(playing ? 0.2f : 0.5f));
                    }
                    g.fillRect(px, centreY2 - h, std::max(1.0f, pxPerPt - 0.3f), h * 2.0f);
                }
                
                // Playback cursor
                if (playing && cardPlayFrac >= 0)
                {
                    float cursorX = (float)wfStartX + cardPlayFrac * (float)wfBarW;
                    g.setColour((isRef ? juce::Colour(0xffFFB3C6) : juce::Colour(0xff67e8f9)).withAlpha(0.9f));
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
        g.setColour(isRef ? juce::Colour(0xffFF6B9D).withAlpha(0.35f) : C::border);
        g.drawRoundedRectangle(card.toFloat(), 8.0f, 1.0f);
        
        int cx = card.getX() + 8, cy = card.getY() + 6, cw = card.getWidth() - 16;
        
        // Reference badge
        if (isRef)
        {
            g.setColour(juce::Colour(0xffFF6B9D));
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
        
        // Subtitle: duration (+ genre for passes only)
        int mins = (int)dur / 60, secs = (int)dur % 60;
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        juce::String sub = isRef 
            ? juce::String::formatted("%d:%02d", mins, secs)
            : processorRef.getGenre() + " - " + juce::String::formatted("%d:%02d", mins, secs);
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
            g.setColour(isRef ? juce::Colour(0xffFF6B9D) : C::blue);
            g.strokePath(eqPath, juce::PathStrokeType(1.5f));
            
            juce::Path fillPath = eqPath;
            fillPath.lineTo((float)(cx + cw), (float)(cy + eqH));
            fillPath.lineTo((float)cx, (float)(cy + eqH));
            fillPath.closeSubPath();
            g.setColour((isRef ? juce::Colour(0xffFF6B9D) : C::blue).withAlpha(0.12f));
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
    settingsExpLevel.setVisible(true);
    // settingsPlugins (freeform box) removed — replaced by settingsPluginViewport.
    settingsLanguage.setVisible(true);
    saveSettingsBtn.setVisible(true); settingsSavedLabel.setVisible(true);
    for (auto& b : dawButtons) b.setVisible(true);

    // Plugins row: scan button + View all + Help & Support. No inline list.
    settingsScanBtn.setVisible(true);
    viewAllPluginsBtn.setVisible(true);
    settingsHelpBtn.setVisible(true);
    settingsPluginViewport.setVisible(false);
    settingsPluginSearchBox.setVisible(false);
    
    // Refresh tier/usage when Settings opens — this is where users go
    // to "check" an upgrade after paying on the website. Catching it
    // here means the tier badge and message limit reflect the upgrade
    // immediately, not after the 60-second periodic refresh.
    if (api.isLoggedIn())
        api.refreshUserInfo(nullptr);
    
    auto populateFields = [this]() {
        auto s = api.getUserSettings();
        settingsName.setText(s.name, false);
        settingsMonitors.setText(s.monitors, false);
        settingsHeadphones.setText(s.headphones, false);
        settingsGenres.setText(s.genres, false);
        // settingsPlugins removed — the checklist (settingsChecklist) reflects
        // the scanner state and is refreshed when the Settings view opens.
        
        if (s.experienceLevel == "Beginner") settingsExpLevel.setSelectedId(1, juce::dontSendNotification);
        else if (s.experienceLevel == "Intermediate") settingsExpLevel.setSelectedId(2, juce::dontSendNotification);
        else if (s.experienceLevel == "Advanced") settingsExpLevel.setSelectedId(3, juce::dontSendNotification);
        else if (s.experienceLevel == "Expert") settingsExpLevel.setSelectedId(4, juce::dontSendNotification);
        
        juce::StringArray dawN = { "Logic Pro","Ableton Live","FL Studio","Pro Tools",
            "Studio One","Cubase","Reaper","Reason","Bitwig","GarageBand","Other" };
        for (int i = 0; i < 11; ++i)
            dawButtons[(size_t)i].setToggleState(s.daws.contains(dawN[i]), juce::dontSendNotification);
        // Note: the DAW selection no longer drives stock plugin injection
        // (that's auto-detected from the host). These toggles still record the
        // user's DAWs for the server/AI context.
    };
    
    // Only show cached data if we've already fetched from server
    if (settingsFetched)
        populateFields();
    else
        settingsSavedLabel.setText("Loading...", juce::dontSendNotification);
    
    // Always re-fetch from server
    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);
    api.fetchSettings([safeThis, populateFields](bool success) {
        if (safeThis == nullptr)
            return;
        if (success) {
            safeThis->settingsFetched = true;
            populateFields();
            safeThis->settingsSavedLabel.setText("", juce::dontSendNotification);
            safeThis->repaint();
        } else {
            safeThis->settingsSavedLabel.setText("Could not load settings", juce::dontSendNotification);
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
    settingsLanguage.setVisible(false);
    saveSettingsBtn.setVisible(false); settingsSavedLabel.setVisible(false);
    for (auto& b : dawButtons) b.setVisible(false);
    settingsPluginViewport.setVisible(false);
    settingsPluginSearchBox.setVisible(false);
    viewAllPluginsBtn.setVisible(false);
    settingsScanBtn.setVisible(false);
    settingsHelpBtn.setVisible(false);
    resized(); repaint();
}

// ============================================================================
// V2 Tab Shell
// ============================================================================

void EchoJayEditor::switchToTab(Tab t)
{
    if (currentTab == t) return;
    currentTab = t;

    // Tear down any active overlay views
    if (currentView == View::Compare) { currentView = View::Meters; hideCompareView(); }
    if (currentView == View::Settings) hideSettingsView();

    // (Link tab has no persistent child components — all painted)

    // Particle visual: authoritative start/stop on every tab switch.
    // The GL context uses continuous repainting — setVisible(false) alone does not
    // stop it from rendering. We must detach (stop) when leaving Visualisation and
    // re-attach (start) when entering it.
    if (t == Tab::Visualisation)
    {
        visualMode = true;
        particleVisualHolder.setVisible(true);
        if (particleVisual != nullptr)
            particleVisual->start();
    }
    else
    {
        // Clear visualMode so resized() cannot re-show the holder via its
        // "visualMode && currentView == View::Meters" branch.
        visualMode = false;
        particleVisualHolder.setVisible(false);
        if (particleVisual != nullptr)
            particleVisual->stop();
    }

    // Per-tab setup
    switch (t)
    {
        case Tab::Visualisation:
            chainScanBtn.setVisible(false);
            chainStatusLabel.setVisible(false);
            chainDebugJsonBox.setVisible(false);
            chainRecommendLabel.setVisible(false);
            chainSearchBox.setVisible(false);
            chainPluginList.setVisible(false);
            chainLoadBtn.setVisible(false);
            chainRackStrip.setVisible(false);
            chainEditorHolder.setVisible(false);
            chainWarnOverlay.setVisible(false);
            visualMode = true; // Visualisation tab always shows the particle visual
            chatSidebar.setVisible(false);
            if (!compactMode)
            {
                chatScroll.setVisible(true);
                chatInput.setVisible(true);
                chatSendBtn.setVisible(true);
            }
            break;
        case Tab::Chat:
            chainScanBtn.setVisible(false);
            chainStatusLabel.setVisible(false);
            chainDebugJsonBox.setVisible(false);
            chainRecommendLabel.setVisible(false);
            chainSearchBox.setVisible(false);
            chainPluginList.setVisible(false);
            chainLoadBtn.setVisible(false);
            chainRackStrip.setVisible(false);
            chainEditorHolder.setVisible(false);
            chainWarnOverlay.setVisible(false);
            if (!compactMode)
            {
                chatSidebar.setVisible(true);
                chatScroll.setVisible(true);
                chatInput.setVisible(true);
                chatSendBtn.setVisible(true);
            }
            // Load workspace on first open only. Do NOT reload on every tab
            // switch — that would overwrite in-flight local mutations (captures)
            // with a stale server copy before they have been persisted.
            if (workspace.getLoadState() == EchoJayWorkspace::LoadState::Idle)
                workspace.requestLoad();
            break;

        case Tab::Meters:
            chainScanBtn.setVisible(false);
            chainStatusLabel.setVisible(false);
            chainDebugJsonBox.setVisible(false);
            chainRecommendLabel.setVisible(false);
            chainSearchBox.setVisible(false);
            chainPluginList.setVisible(false);
            chainLoadBtn.setVisible(false);
            chainRackStrip.setVisible(false);
            chainEditorHolder.setVisible(false);
            chainWarnOverlay.setVisible(false);
            // Split layout (meters left, chat right) — same as Visualisation.
            // Tear down settings/compare so their content can't bleed in.
            if (currentView == View::Settings) hideSettingsView();
            if (currentView == View::Compare)  { hideCompareView(); }
            currentView = View::Meters;
            visualMode = false; // force standard meter readout (not particle visual)
            chatSidebar.setVisible(false);
            if (!compactMode)
            {
                chatScroll.setVisible(true);
                chatInput.setVisible(true);
                chatSendBtn.setVisible(true);
            }
            break;

        case Tab::Compare:
            chainScanBtn.setVisible(false);
            chainStatusLabel.setVisible(false);
            chainDebugJsonBox.setVisible(false);
            chainRecommendLabel.setVisible(false);
            chainSearchBox.setVisible(false);
            chainPluginList.setVisible(false);
            chainLoadBtn.setVisible(false);
            chainRackStrip.setVisible(false);
            chainEditorHolder.setVisible(false);
            chainWarnOverlay.setVisible(false);
            currentView = View::Compare;
            chatSidebar.setVisible(false);
            showCompareView(); // sets up compare components and calls resized/repaint
            // Chat panel stays visible on Compare (right-side split, same as Visualisation/Meters)
            if (!compactMode)
            {
                chatScroll.setVisible(true);
                chatInput.setVisible(true);
                chatSendBtn.setVisible(true);
            }
            return; // showCompareView already called resized/repaint

        case Tab::Settings:
            chainScanBtn.setVisible(false);
            chainStatusLabel.setVisible(false);
            chainDebugJsonBox.setVisible(false);
            chainRecommendLabel.setVisible(false);
            chainSearchBox.setVisible(false);
            chainPluginList.setVisible(false);
            chainLoadBtn.setVisible(false);
            chainRackStrip.setVisible(false);
            chainEditorHolder.setVisible(false);
            chainWarnOverlay.setVisible(false);
            chatSidebar.setVisible(false);
            showSettingsView(); // sets currentView = Settings, calls resized/repaint
            chatScroll.setVisible(false);
            chatInput.setVisible(false);
            chatSendBtn.setVisible(false);
            chatTextSizeBtn.setVisible(false);
            return; // showSettingsView already called resized/repaint

        case Tab::Chain:
        {
            chatSidebar.setVisible(false);
            chatScroll.setVisible(false);
            chatInput.setVisible(false);
            chatSendBtn.setVisible(false);
            chatTextSizeBtn.setVisible(false);
            upgradeBtn.setVisible(false);
            // Show CHAIN tab components
            chainScanBtn.setVisible(true);
            chainStatusLabel.setVisible(true);
            chainDebugJsonBox.setVisible(true);
            chainRecommendLabel.setVisible(true);
            chainSearchBox.setVisible(true);
            chainPluginList.setVisible(true);
            chainLoadBtn.setVisible(true);
            chainRackStrip.setVisible(true);
            chainEditorHolder.setVisible(true);
            {
                auto& ch = processorRef.getChainHost();
                // Auto-start a refresh on first open
                if (ch.getNumPlugins() == 0 && !ch.isScanning())
                    ch.startScan();
                chainListModel->items = ch.getFilteredPlugins(chainSearchBox.getText(), chainFormatFilter_);
                chainPluginList.updateContent();
                // Rebuild rack strip from current chain state
                chainRackStrip.rebuild(ch.getAllSlotInfos(), chainSelectedSlot_);
                // Rebuild resolver and update readout
                ch.buildRecommendable(processorRef.getPluginScanner().getPlugins(), chainFormatFilter_);
                chainRecommendLabel.setText(
                    "recommendable: " + juce::String(ch.getRecommendableCount())
                    + " resolved (" + juce::String(ch.getEnabledInputCount()) + " enabled, "
                    + juce::String(ch.getUnmatchedCount()) + " unmatched)",
                    juce::dontSendNotification);
                // Status label
                if (ch.getNumSlots() > 0)
                    chainStatusLabel.setText(juce::String(ch.getNumSlots()) + " slot(s) in chain",
                                             juce::dontSendNotification);
                else if (ch.isScanning())
                    chainStatusLabel.setText("Reading plugin list...", juce::dontSendNotification);
                else if (ch.getNumPlugins() == 0)
                    chainStatusLabel.setText("No plugins found. Click Refresh.", juce::dontSendNotification);
                else
                    chainStatusLabel.setText(juce::String(ch.getNumPlugins()) + " plugins available",
                                             juce::dontSendNotification);
                // Show warning if not yet dismissed
                if (!ch.chainWarningDismissed)
                {
                    chainWarnOverlay.setVisible(true);
                    chainWarnOverlay.toFront(false);
                }
            }
            break;
        }

        case Tab::Link:
            chainScanBtn.setVisible(false);
            chainStatusLabel.setVisible(false);
            chainDebugJsonBox.setVisible(false);
            chainRecommendLabel.setVisible(false);
            chainSearchBox.setVisible(false);
            chainPluginList.setVisible(false);
            chainLoadBtn.setVisible(false);
            chainRackStrip.setVisible(false);
            chainEditorHolder.setVisible(false);
            chainWarnOverlay.setVisible(false);
            chatSidebar.setVisible(false);
            upgradeBtn.setVisible(false);
            if (!compactMode)
            {
                chatScroll.setVisible(true);
                chatInput.setVisible(true);
                chatSendBtn.setVisible(true);
            }
            // Trigger an immediate refresh so the list isn't blank on tab switch
            processorRef.refreshLinkRegistry();
            break;
    }

    // Hide chain build buttons on every tab switch; the paint loop re-shows them
    // on the next repaint if the chat area is visible and buttons are in view.
    for (int i = 0; i < kMaxChainBuildBtns; ++i)
        chainBuildBtns[(size_t)i].setVisible(false);

    resized();
    repaint();
}

void EchoJayEditor::saveSettingsToServer()
{
    UserSettings s;
    s.name = settingsName.getText(); s.monitors = settingsMonitors.getText();
    s.headphones = settingsHeadphones.getText(); s.genres = settingsGenres.getText();
    // Commit the Settings checklist's local tick selection to the scanner
    // first, so the plugin list reflects any changes the user made here. Then
    // read the full list. (The checklist is local-until-commit for speed.)
    if (settingsChecklist) settingsChecklist->commit();
    processorRef.getPluginScanner().saveEnabledState();
    // Plugins now come from the checklist (the freeform box was removed). The
    // scanner's enabled-effects list is the source of truth; merge it the same
    // way updatePluginsFromScanner does so manual web-app entries are kept.
    api.updatePluginsFromScanner(processorRef.getPluginScanner().getFullPluginList());
    s.plugins = api.getUserSettings().plugins;
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

    // The DAW selection is saved for server/AI context but does NOT drive
    // stock plugin injection — that's auto-detected from the host DAW.

    settingsSavedLabel.setText("Saving...", juce::dontSendNotification);
    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);
    api.saveUserSettings(s, [safeThis](bool success) {
        if (safeThis == nullptr)
            return;
        safeThis->settingsSavedLabel.setText(success ? "Saved" : "Failed to save", juce::dontSendNotification);
        if (success)
            juce::Timer::callAfterDelay(3000, [safeThis]() {
                if (safeThis == nullptr)
                    return;
                safeThis->settingsSavedLabel.setText("", juce::dontSendNotification);
            });
    });
}

// ============================================================================
// Chat sidebar — Phase 2a
// ============================================================================

// ============================================================================
// ChatSidebarModel — ListBoxModel implementation
// ============================================================================

void EchoJayEditor::ChatSidebarModel::refreshRows(
    const std::vector<WsChat>&  chats,
    const std::vector<WsAlbum>& albums,
    const std::vector<WsReview>& reviews,
    const std::set<juce::String>& collapsedSet,
    const juce::String& activeChatId)
{
    rows.clear();

    // Build set of chat ids belonging to at least one album
    std::set<juce::String> albumedIds;
    for (auto& album : albums)
        for (auto& cid : album.chatIds)
            albumedIds.insert(cid);

    // --- Section: Albums & Chats ---
    {
        Row r;
        r.kind  = Row::Kind::SectionTitle;
        r.label = "ALBUMS & CHATS";
        rows.push_back(r);
    }

    for (auto& album : albums)
    {
        bool collapsed = (collapsedSet.count(album.id) > 0);
        int chatCount = 0;
        for (auto& cid : album.chatIds)
            for (auto& ch : chats)
                if (ch.id == cid && !ch.messages.empty()) ++chatCount;

        // Strip any leading non-ASCII characters (e.g. folder emoji stored by
        // the web app) so the name renders cleanly in JUCE's default font.
        juce::String albumName = album.name;
        while (albumName.isNotEmpty() && (albumName[0] < 0 || albumName[0] > 127))
            albumName = albumName.substring(1);
        albumName = albumName.trimStart();

        Row header;
        header.kind      = Row::Kind::AlbumHeader;
        header.id        = album.id;
        header.collapsed = collapsed;
        header.label     = albumName;
        if (chatCount > 0) header.label += "  (" + juce::String(chatCount) + ")";
        rows.push_back(header);

        if (!collapsed)
        {
            for (auto& cid : album.chatIds)
            {
                const WsChat* chat = nullptr;
                for (auto& ch : chats)
                    if (ch.id == cid && !ch.messages.empty()) { chat = &ch; break; }
                if (!chat) continue;

                Row r;
                r.kind   = Row::Kind::ChatRow;
                r.id     = chat->id;
                r.label  = chat->title.isEmpty() ? "Untitled" : chat->title;
                r.active = (chat->id == activeChatId);
                r.indent = 16;

                auto raw = chat->created;
                auto s = raw.upToFirstOccurrenceOf("T", false, false);
                r.meta = (s.length() == 10) ? s.substring(5) : raw.substring(0, 10);
                if (chat->revisionCount > 0)
                    r.meta += "  -" + juce::String(chat->revisionCount) + " rev";

                rows.push_back(r);
            }
        }
    }

    // --- Section: Ungrouped Chats ---
    std::vector<const WsChat*> ungrouped;
    for (auto& ch : chats)
        if (!ch.messages.empty() && albumedIds.count(ch.id) == 0)
            ungrouped.push_back(&ch);

    if (!ungrouped.empty())
    {
        Row sec;
        sec.kind  = Row::Kind::SectionTitle;
        sec.label = "CHATS";
        rows.push_back(sec);

        for (auto* chat : ungrouped)
        {
            Row r;
            r.kind   = Row::Kind::ChatRow;
            r.id     = chat->id;
            r.label  = chat->title.isEmpty() ? "Untitled" : chat->title;
            r.active = (chat->id == activeChatId);

            auto raw = chat->created;
            auto s = raw.upToFirstOccurrenceOf("T", false, false);
            r.meta = (s.length() == 10) ? s.substring(5) : raw.substring(0, 10);
            if (chat->revisionCount > 0)
                r.meta += "  -" + juce::String(chat->revisionCount) + " rev";

            rows.push_back(r);
        }
    }

    // --- Section: Mix Reviews ---
    if (!reviews.empty())
    {
        Row sec;
        sec.kind  = Row::Kind::SectionTitle;
        sec.label = "MIX REVIEWS";
        rows.push_back(sec);

        for (auto& rev : reviews)
        {
            Row r;
            r.kind  = Row::Kind::ReviewRow;
            r.id    = rev.id;
            r.label = rev.fileName.isEmpty() ? "Untitled" : rev.fileName;

            auto raw = rev.date;
            auto s = raw.upToFirstOccurrenceOf("T", false, false);
            r.meta = (s.length() == 10) ? s.substring(5) : raw.substring(0, 10);
            if (rev.genre.isNotEmpty())            r.meta += "  -" + rev.genre;
            else if (rev.channelType.isNotEmpty()) r.meta += "  -" + rev.channelType;

            rows.push_back(r);
        }
    }
}

void EchoJayEditor::ChatSidebarModel::paintListBoxItem(
    int rowNum, juce::Graphics& g, int width, int height, bool /*isSelected*/)
{
    using C = EchoJayLookAndFeel::Colours;

    if (rowNum < 0 || rowNum >= (int)rows.size()) return;
    const auto& row = rows[(size_t)rowNum];

    const int padX = 10;

    if (row.kind == Row::Kind::SectionTitle)
    {
        g.setColour(juce::Colour(0xff080A12));
        g.fillRect(0, 0, width, height);
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
        g.drawText(row.label, padX, 0, width - padX * 2, height,
                   juce::Justification::centredLeft);
        return;
    }

    if (row.kind == Row::Kind::AlbumHeader)
    {
        g.setColour(C::bg2);
        g.fillRect(0, 0, width, height);
        g.setColour(C::border);
        g.drawHorizontalLine(height - 1, 0.f, (float)width);

        // Draw collapse/expand triangle with Graphics (no text glyph — avoids
        // JUCE Latin-1 misinterpretation of u8 Unicode literals).
        {
            float cx = (float)padX + 5.0f;
            float cy = (float)height * 0.5f;
            juce::Path tri;
            if (row.collapsed)
            {
                // Right-pointing triangle
                tri.addTriangle(cx - 3.0f, cy - 5.0f,
                                cx - 3.0f, cy + 5.0f,
                                cx + 4.0f, cy);
            }
            else
            {
                // Down-pointing triangle
                tri.addTriangle(cx - 5.0f, cy - 3.0f,
                                cx + 5.0f, cy - 3.0f,
                                cx,        cy + 4.0f);
            }
            g.setColour(C::text3);
            g.fillPath(tri);
        }

        g.setColour(C::text2);
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText(row.label, padX + 14, 0, width - padX * 2 - 14, height,
                   juce::Justification::centredLeft);
        return;
    }

    if (row.kind == Row::Kind::ChatRow)
    {
        g.setColour(row.active ? C::bg4 : C::bg);
        g.fillRect(0, 0, width, height);
        if (row.active)
        {
            g.setColour(C::blue);
            g.fillRect(0, 0, 3, height);
        }
        g.setColour(C::border);
        g.drawHorizontalLine(height - 1, 0.f, (float)width);

        g.setColour(row.active ? C::text : C::text2);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(row.label, padX + row.indent, 0,
                   width - padX - row.indent, height - 14,
                   juce::Justification::bottomLeft);

        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        g.drawText(row.meta, padX + row.indent, height - 16,
                   width - padX - row.indent, 14,
                   juce::Justification::bottomLeft);
        return;
    }

    if (row.kind == Row::Kind::ReviewRow)
    {
        g.setColour(C::bg);
        g.fillRect(0, 0, width, height);
        g.setColour(C::border);
        g.drawHorizontalLine(height - 1, 0.f, (float)width);

        g.setColour(C::text2);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(row.label, padX, 0, width - padX * 2, height - 14,
                   juce::Justification::bottomLeft);

        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        g.drawText(row.meta, padX, height - 16, width - padX * 2, 14,
                   juce::Justification::bottomLeft);
        return;
    }
}

void EchoJayEditor::ChatSidebarModel::listBoxItemClicked(
    int rowNum, const juce::MouseEvent& e)
{
    if (rowNum < 0 || rowNum >= (int)rows.size()) return;
    const auto& row = rows[(size_t)rowNum];

    if (row.kind == Row::Kind::AlbumHeader)
    {
        if (e.mods.isPopupMenu())
        {
            if (onAlbumContextMenu) onAlbumContextMenu(row.id);
        }
        else if (onAlbumToggled)
        {
            onAlbumToggled(row.id);
        }
        return;
    }

    if (row.kind == Row::Kind::ChatRow)
    {
        if (e.mods.isPopupMenu())
        {
            if (onChatContextMenu) onChatContextMenu(row.id);
        }
        else
        {
            if (onChatClicked) onChatClicked(row.id);
        }
    }
    // SectionTitle and ReviewRow: no action
}

// Legacy free function kept for the date formatting used elsewhere
#if 0  // Old paintChatSidebar removed — replaced by ChatSidebarModel above
void EchoJayEditor::paintChatSidebar(juce::Graphics& g, juce::Rectangle<int> area)
{
    const int x     = area.getX();
    const int y     = area.getY();
    const int w     = area.getWidth();
    const int fullH = area.getHeight();

    // Background + right border
    g.setColour(juce::Colour(0xff080A12));
    g.fillRect(area);
    g.setColour(C::border);
    g.drawVerticalLine(x + w - 1, (float)y, (float)(y + fullH));

    // Scroll offset
    int scrollY = chatSidebarScroll.getViewPositionY();

    // Rebuild row list each frame (cheap — list is small)
    sidebarRows.clear();

    const int rowH    = 36;
    const int secH    = 24;
    const int padX    = 10;
    const int indent  = 16; // chat rows indented under album header
    int cy = y - scrollY;   // current Y in window coords (scrolled)

    auto& chats   = workspace.getChats();
    auto& albums  = workspace.getAlbums();
    auto& reviews = workspace.getReviews();

    // ---- Section: Albums & Chats ----------------------------------------
    // Section title
    {
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
        g.drawText("ALBUMS & CHATS", x + padX, cy, w - padX * 2, secH,
                   juce::Justification::centredLeft);
        SidebarRow sr;
        sr.kind = SidebarRow::Kind::SectionTitle;
        sr.bounds = { 0, cy - y + scrollY, w, secH };
        sidebarRows.push_back(sr);
        cy += secH;
    }

    // Build a set of chat ids that belong to at least one album
    std::set<juce::String> albumedChatIds;
    for (auto& album : albums)
        for (auto& cid : album.chatIds)
            albumedChatIds.insert(cid);

    // Albums
    for (auto& album : albums)
    {
        bool collapsed = (collapsedAlbums.count(album.id) > 0);

        // Count chats in this album that have messages
        int chatCount = 0;
        for (auto& cid : album.chatIds)
            for (auto& ch : chats)
                if (ch.id == cid && !ch.messages.empty()) chatCount++;

        bool isHovered = false; // future hover state

        // Album header row
        bool rowVis = (cy + rowH > y && cy < y + fullH);
        if (rowVis)
        {
            juce::Rectangle<int> rowRect(x, cy, w, rowH);
            g.setColour(C::bg2);
            g.fillRect(rowRect);
            g.setColour(C::border);
            g.drawHorizontalLine(cy + rowH - 1, (float)x, (float)(x + w));

            // Arrow — drawn path, not a text glyph
            {
                float tcx = (float)(x + padX) + 5.0f;
                float tcy = (float)cy + (float)rowH * 0.5f;
                juce::Path tri;
                if (collapsed)
                {
                    tri.addTriangle(tcx - 3.0f, tcy - 5.0f,
                                    tcx - 3.0f, tcy + 5.0f,
                                    tcx + 4.0f, tcy);
                }
                else
                {
                    tri.addTriangle(tcx - 5.0f, tcy - 3.0f,
                                    tcx + 5.0f, tcy - 3.0f,
                                    tcx,        tcy + 4.0f);
                }
                g.setColour(C::text3);
                g.fillPath(tri);
            }

            // Name
            g.setColour(C::text2);
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            juce::String label = album.name;
            if (chatCount > 0) label += "  (" + juce::String(chatCount) + ")";
            g.drawText(label, x + padX + 14, cy, w - padX * 2 - 14, rowH,
                       juce::Justification::centredLeft);
        }
        {
            SidebarRow sr;
            sr.kind = SidebarRow::Kind::AlbumHeader;
            sr.id = album.id;
            sr.bounds = { 0, cy - y + scrollY, w, rowH };
            sidebarRows.push_back(sr);
        }
        cy += rowH;

        // Chat rows within this album (if expanded)
        if (!collapsed)
        {
            for (auto& cid : album.chatIds)
            {
                const WsChat* chat = nullptr;
                for (auto& ch : chats)
                    if (ch.id == cid && !ch.messages.empty()) { chat = &ch; break; }
                if (!chat) continue;

                bool active = (chat->id == currentChatId);

                if (cy + rowH > y && cy < y + fullH)
                {
                    juce::Rectangle<int> rowRect(x, cy, w, rowH);
                    g.setColour(active ? C::bg4 : C::bg);
                    g.fillRect(rowRect);
                    if (active)
                    {
                        g.setColour(C::blue);
                        g.fillRect(x, cy, 3, rowH);
                    }
                    g.setColour(C::border);
                    g.drawHorizontalLine(cy + rowH - 1, (float)x, (float)(x + w));

                    g.setColour(active ? C::text : C::text2);
                    g.setFont(juce::Font(juce::FontOptions(11.0f)));
                    g.drawText(chat->title.isEmpty() ? "Untitled" : chat->title,
                               x + indent + padX, cy, w - indent - padX * 2, rowH - 14,
                               juce::Justification::bottomLeft);

                    juce::String meta = formatSidebarDate(chat->created);
                    if (chat->revisionCount > 0) meta += "  -" + juce::String(chat->revisionCount) + " rev";
                    g.setColour(C::text3);
                    g.setFont(juce::Font(juce::FontOptions(9.5f)));
                    g.drawText(meta, x + indent + padX, cy + rowH - 16, w - indent - padX * 2, 14,
                               juce::Justification::bottomLeft);
                }
                {
                    SidebarRow sr;
                    sr.kind = SidebarRow::Kind::ChatRow;
                    sr.id = chat->id;
                    sr.bounds = { 0, cy - y + scrollY, w, rowH };
                    sidebarRows.push_back(sr);
                }
                cy += rowH;
            }
        }
    }

    // ---- Section: Ungrouped Chats ----------------------------------------
    // Collect chats with no albumId (or albumId not matching any album)
    std::vector<const WsChat*> ungrouped;
    for (auto& ch : chats)
    {
        if (ch.messages.empty()) continue;
        if (albumedChatIds.count(ch.id) == 0)
            ungrouped.push_back(&ch);
    }

    if (!ungrouped.empty())
    {
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
        if (cy + secH > y && cy < y + fullH)
            g.drawText("CHATS", x + padX, cy, w - padX * 2, secH,
                       juce::Justification::centredLeft);
        {
            SidebarRow sr;
            sr.kind = SidebarRow::Kind::SectionTitle;
            sr.bounds = { 0, cy - y + scrollY, w, secH };
            sidebarRows.push_back(sr);
        }
        cy += secH;

        for (auto* chat : ungrouped)
        {
            bool active = (chat->id == currentChatId);

            if (cy + rowH > y && cy < y + fullH)
            {
                juce::Rectangle<int> rowRect(x, cy, w, rowH);
                g.setColour(active ? C::bg4 : C::bg);
                g.fillRect(rowRect);
                if (active)
                {
                    g.setColour(C::blue);
                    g.fillRect(x, cy, 3, rowH);
                }
                g.setColour(C::border);
                g.drawHorizontalLine(cy + rowH - 1, (float)x, (float)(x + w));

                g.setColour(active ? C::text : C::text2);
                g.setFont(juce::Font(juce::FontOptions(11.0f)));
                g.drawText(chat->title.isEmpty() ? "Untitled" : chat->title,
                           x + padX, cy, w - padX * 2, rowH - 14,
                           juce::Justification::bottomLeft);

                juce::String meta = formatSidebarDate(chat->created);
                if (chat->revisionCount > 0) meta += "  -" + juce::String(chat->revisionCount) + " rev";
                g.setColour(C::text3);
                g.setFont(juce::Font(juce::FontOptions(9.5f)));
                g.drawText(meta, x + padX, cy + rowH - 16, w - padX * 2, 14,
                           juce::Justification::bottomLeft);
            }
            {
                SidebarRow sr;
                sr.kind = SidebarRow::Kind::ChatRow;
                sr.id = chat->id;
                sr.bounds = { 0, cy - y + scrollY, w, rowH };
                sidebarRows.push_back(sr);
            }
            cy += rowH;
        }
    }

    // ---- Section: Mix Reviews -------------------------------------------
    if (!reviews.empty())
    {
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
        if (cy + secH > y && cy < y + fullH)
            g.drawText("MIX REVIEWS", x + padX, cy, w - padX * 2, secH,
                       juce::Justification::centredLeft);
        {
            SidebarRow sr;
            sr.kind = SidebarRow::Kind::SectionTitle;
            sr.bounds = { 0, cy - y + scrollY, w, secH };
            sidebarRows.push_back(sr);
        }
        cy += secH;

        for (auto& rev : reviews)
        {
            if (cy + rowH > y && cy < y + fullH)
            {
                juce::Rectangle<int> rowRect(x, cy, w, rowH);
                g.setColour(C::bg);
                g.fillRect(rowRect);
                g.setColour(C::border);
                g.drawHorizontalLine(cy + rowH - 1, (float)x, (float)(x + w));

                g.setColour(C::text2);
                g.setFont(juce::Font(juce::FontOptions(11.0f)));
                juce::String fname = rev.fileName.isEmpty() ? "Untitled" : rev.fileName;
                g.drawText(fname, x + padX, cy, w - padX * 2, rowH - 14,
                           juce::Justification::bottomLeft);

                juce::String meta = formatSidebarDate(rev.date);
                if (rev.genre.isNotEmpty()) meta += "  -" + rev.genre;
                else if (rev.channelType.isNotEmpty()) meta += "  -" + rev.channelType;
                g.setColour(C::text3);
                g.setFont(juce::Font(juce::FontOptions(9.5f)));
                g.drawText(meta, x + padX, cy + rowH - 16, w - padX * 2, 14,
                           juce::Justification::bottomLeft);
            }
            {
                SidebarRow sr;
                sr.kind = SidebarRow::Kind::ReviewRow;
                sr.id = rev.id;
                sr.bounds = { 0, cy - y + scrollY, w, rowH };
                sidebarRows.push_back(sr);
            }
            cy += rowH;
        }
    }

    // Update content component height so the viewport can scroll
    int totalH = cy - y + scrollY;
    if (totalH != chatSidebarContent.getHeight())
        chatSidebarContent.setSize(w, juce::jmax(fullH, totalH));
}
#endif // old paintChatSidebar

void EchoJayEditor::loadChatFromWorkspace(const juce::String& chatId)
{
    for (auto& ch : workspace.getChats())
    {
        if (ch.id != chatId) continue;

        currentChatId = chatId;
        chatMessages.clear();
        processorRef.chatHistory.clear();
        processorRef.chatRoles.clear();
        processorRef.chatContents.clear();

        // Load index.json once for wav path lookups
        juce::File captureDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                    .getChildFile("EchoJay").getChildFile("Captures");
        juce::var indexVar;
        {
            juce::File indexFile = captureDir.getChildFile("index.json");
            if (indexFile.existsAsFile())
                indexVar = juce::JSON::parse(indexFile.loadFileAsString());
        }

        for (auto& msg : ch.messages)
        {
            ChatMsg cm;
            cm.role      = msg.role;
            cm.content   = msg.content;
            cm.reviewId  = msg.reviewId;
            cm.chainData = msg.chainJson;  // restore persisted chain block → rebuilds Build button

            // Reconstruct capture block from stored review.
            // Helper lambda — populates cm from a resolved WsReview.
            auto populateFromReview = [&](const WsReview& rv)
            {
                cm.reviewId = rv.id;
                if (rv.label.isNotEmpty() && cm.content.isEmpty())
                    cm.content = rv.label;
                if (rv.waveform.isArray())
                {
                    for (int wi = 0; wi < rv.waveform.size(); ++wi)
                    {
                        WaveformRecorder::ThumbnailPoint pt;
                        if (auto* obj = rv.waveform[wi].getDynamicObject())
                        {
                            // {x, n} format: x = positive peak, n = negative peak
                            pt.maxVal = (float)(double)obj->getProperty("x");
                            pt.minVal = (float)(double)obj->getProperty("n");
                        }
                        else
                        {
                            // Flat float format (old plugin captures)
                            float peak = (float)(double)rv.waveform[wi];
                            pt.maxVal =  peak;
                            pt.minVal = -peak;
                        }
                        cm.waveform.push_back(pt);
                    }
                    cm.hasWaveform = !cm.waveform.empty();
                }
                cm.durationSeconds = rv.data.duration;
                cm.lufs            = rv.data.integ;
                cm.audioUrl        = rv.audioUrl;
                cm.origin          = rv.origin;
                if (auto* idxObj = indexVar.getDynamicObject())
                {
                    juce::String wavName = idxObj->getProperty(rv.id).toString();
                    if (wavName.isNotEmpty())
                    {
                        juce::File wavFile = captureDir.getChildFile(wavName);
                        if (wavFile.existsAsFile())
                        {
                            cm.wavFilename = wavName;
                            cm.wavFilePath = wavFile.getFullPathName();
                        }
                    }
                }
            };

            if (msg.reviewId.isNotEmpty())
            {
                // Primary path: message carries _reviewId — direct lookup.
                for (auto& rv : workspace.getReviews())
                    if (rv.id == msg.reviewId) { populateFromReview(rv); break; }
            }
            else if (msg.role == "user" && ch.trackName.isNotEmpty())
            {
                // Fallback for older messages without _reviewId: match review by
                // fileName or label equalling the chat's trackName.
                for (auto& rv : workspace.getReviews())
                    if (rv.fileName == ch.trackName || rv.label == ch.trackName)
                        { populateFromReview(rv); break; }
            }

            chatMessages.push_back(cm);

            // Populate API context window
            processorRef.chatRoles.add(msg.role);
            processorRef.chatContents.add(msg.content);

            // Mirror into processor chat history for persistence
            EchoJayProcessor::ChatEntry ce;
            ce.role    = msg.role;
            ce.content = msg.content;
            if (cm.hasWaveform)
            {
                ce.hasWaveform     = true;
                ce.durationSeconds = cm.durationSeconds;
                ce.lufs            = cm.lufs;
                ce.wavFilename     = cm.wavFilename;
                ce.wavFilePath     = cm.wavFilePath;
                for (auto& pt : cm.waveform)
                    ce.waveform.push_back(std::max(std::abs(pt.maxVal), std::abs(pt.minVal)));
            }
            processorRef.chatHistory.push_back(ce);
        }

        juce::String title = ch.title.isEmpty() ? "Untitled" : ch.title;
        sidebarDebugText = "opened " + title + " (" + juce::String((int)ch.messages.size()) + " msgs)";

        // Refresh the sidebar so the active row highlight updates
        if (sidebarModel)
        {
            sidebarModel->refreshRows(workspace.getChats(), workspace.getAlbums(),
                                      workspace.getReviews(), collapsedAlbums, currentChatId);
            chatSidebar.updateContent();
        }

        layoutChatMessages();
        // Scroll to bottom
        chatScroll.setViewPositionProportionately(0.0, 1.0);
        repaint();
        return;
    }
}

// ============================================================================
// Sidebar column management — Phase 2b
// ============================================================================

juce::String EchoJayEditor::getCurrentAlbumId() const
{
    if (currentChatId.isEmpty()) return {};
    for (auto& a : workspace.getAlbums())
        if (a.chatIds.contains(currentChatId))
            return a.id;
    return {};
}

void EchoJayEditor::createNewChat()
{
    // Don't create another empty chat if the current one is already empty
    if (chatMessages.empty()) return;

    WsChat c;
    c.id           = juce::String(juce::Time::currentTimeMillis());
    c.title        = "New chat";
    c.created      = juce::Time::getCurrentTime().toISO8601(true);
    c.trackName    = "";
    c.albumId      = getCurrentAlbumId();
    c.revisionCount = 0;
    // messages intentionally empty — won't appear in sidebar until first send

    workspace.addChat(c);
    // No mutation sync yet — nothing to persist until a message is added

    currentChatId = c.id;
    chatMessages.clear();
    processorRef.chatHistory.clear();
    processorRef.chatRoles.clear();
    processorRef.chatContents.clear();
    sidebarDebugText = "";

    // Refresh sidebar (the new chat won't show — empty messages are filtered)
    if (sidebarModel)
    {
        sidebarModel->refreshRows(workspace.getChats(), workspace.getAlbums(),
                                  workspace.getReviews(), collapsedAlbums, currentChatId);
        chatSidebar.updateContent();
    }

    layoutChatMessages();
    chatInput.grabKeyboardFocus();
    repaint();
}

void EchoJayEditor::createNewAlbum()
{
    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);

    auto* dlg = new juce::AlertWindow("New Album", "Album name:",
                                       juce::MessageBoxIconType::QuestionIcon);
    dlg->addTextEditor("name", "", "Name:");
    dlg->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dlg->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dlg->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, dlg](int result)
        {
            juce::String trimmed = dlg->getTextEditorContents("name").trim();
            delete dlg;
            if (safeThis == nullptr || result != 1 || trimmed.isEmpty()) return;

            WsAlbum a;
            a.id      = juce::String(juce::Time::currentTimeMillis());
            a.name    = trimmed;
            a.created = juce::Time::getCurrentTime().toISO8601(true);

            safeThis->workspace.addAlbum(a);
            safeThis->workspace.requestMutationSync();

            if (safeThis->sidebarModel)
            {
                safeThis->sidebarModel->refreshRows(
                    safeThis->workspace.getChats(),
                    safeThis->workspace.getAlbums(),
                    safeThis->workspace.getReviews(),
                    safeThis->collapsedAlbums,
                    safeThis->currentChatId);
                safeThis->chatSidebar.updateContent();
            }
            safeThis->repaint();
        }), true);
}

void EchoJayEditor::showMoveToAlbumMenu(const juce::String& chatId)
{
    auto& albums = workspace.getAlbums();

    // Current title for the rename dialog default
    juce::String currentTitle;
    for (auto& ch : workspace.getChats())
        if (ch.id == chatId) { currentTitle = ch.title; break; }

    // Which album (if any) currently owns this chat?
    juce::String currentAlbum;
    for (auto& a : albums)
        if (a.chatIds.contains(chatId)) { currentAlbum = a.id; break; }

    juce::PopupMenu menu;

    // Rename / Delete at top
    int itemId = 1;
    std::vector<juce::String> actions;
    menu.addItem(itemId++, "Rename...");  actions.push_back("__rename__");
    menu.addItem(itemId++, "Delete");               actions.push_back("__delete__");
    menu.addSeparator();

    // Move to album sub-section
    juce::PopupMenu albumSub;
    int albumSubBase = itemId;
    for (auto& a : albums)
    {
        bool isCurrent = (a.id == currentAlbum);
        albumSub.addItem(itemId++, (isCurrent ? "* " : "  ") + a.name);
        actions.push_back(a.id);
    }
    if (!currentAlbum.isEmpty())
    {
        albumSub.addSeparator();
        albumSub.addItem(itemId++, "Remove from album");
        actions.push_back("__remove__");
    }
    albumSub.addSeparator();
    albumSub.addItem(itemId++, "New album...");
    actions.push_back("__new__");

    (void)albumSubBase;
    menu.addSubMenu("Move to album", albumSub);

    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withParentComponent(this),
        [safeThis, chatId, actions, currentTitle](int result)
        {
            if (safeThis == nullptr || result <= 0) return;
            juce::String action = actions[(size_t)(result - 1)];

            auto doRefreshAndSync = [safeThis]()
            {
                if (safeThis->sidebarModel)
                {
                    safeThis->sidebarModel->refreshRows(
                        safeThis->workspace.getChats(),
                        safeThis->workspace.getAlbums(),
                        safeThis->workspace.getReviews(),
                        safeThis->collapsedAlbums,
                        safeThis->currentChatId);
                    safeThis->chatSidebar.updateContent();
                }
                safeThis->workspace.requestMutationSync();
                safeThis->repaint();
            };

            if (action == "__rename__")
            {
                auto* dlg = new juce::AlertWindow("Rename Chat", "New name:",
                                                   juce::MessageBoxIconType::QuestionIcon);
                dlg->addTextEditor("name", currentTitle, "Name:");
                dlg->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
                dlg->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                dlg->enterModalState(true,
                    juce::ModalCallbackFunction::create([safeThis, dlg, chatId, doRefreshAndSync](int r)
                    {
                        juce::String trimmed = dlg->getTextEditorContents("name").trim();
                        delete dlg;
                        if (safeThis == nullptr || r != 1 || trimmed.isEmpty()) return;
                        safeThis->workspace.setChatTitle(chatId, trimmed);
                        doRefreshAndSync();
                    }), true);
            }
            else if (action == "__delete__")
            {
                juce::AlertWindow::showOkCancelBox(
                    juce::MessageBoxIconType::WarningIcon,
                    "Delete Chat",
                    "Delete this chat? This cannot be undone.",
                    "Delete", "Cancel",
                    nullptr,
                    juce::ModalCallbackFunction::create([safeThis, chatId, doRefreshAndSync](int r)
                    {
                        if (safeThis == nullptr || r != 1) return;
                        // Clear message view if we're deleting the current chat
                        if (safeThis->currentChatId == chatId)
                        {
                            safeThis->currentChatId = {};
                            safeThis->chatMessages.clear();
                            safeThis->processorRef.chatHistory.clear();
                            safeThis->processorRef.chatRoles.clear();
                            safeThis->processorRef.chatContents.clear();
                        }
                        safeThis->workspace.removeChat(chatId);
                        doRefreshAndSync();
                    }));
            }
            else if (action == "__remove__")
            {
                safeThis->workspace.removeChatFromAlbum(chatId);
                doRefreshAndSync();
            }
            else if (action == "__new__")
            {
                auto* dlg2 = new juce::AlertWindow("New Album", "Album name:",
                                                    juce::MessageBoxIconType::QuestionIcon);
                dlg2->addTextEditor("name", "", "Name:");
                dlg2->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
                dlg2->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                dlg2->enterModalState(true,
                    juce::ModalCallbackFunction::create([safeThis, dlg2, chatId, doRefreshAndSync](int r)
                    {
                        juce::String trimmed = dlg2->getTextEditorContents("name").trim();
                        delete dlg2;
                        if (safeThis == nullptr || r != 1 || trimmed.isEmpty()) return;
                        WsAlbum a;
                        a.id      = juce::String(juce::Time::currentTimeMillis());
                        a.name    = trimmed;
                        a.created = juce::Time::getCurrentTime().toISO8601(true);
                        safeThis->workspace.addAlbum(a);
                        safeThis->workspace.moveChatToAlbum(chatId, a.id);
                        doRefreshAndSync();
                    }), true);
            }
            else
            {
                // Assign to an existing album
                safeThis->workspace.moveChatToAlbum(chatId, action);
                doRefreshAndSync();
            }
        });
}

void EchoJayEditor::showAlbumContextMenu(const juce::String& albumId)
{
    juce::String currentName;
    for (auto& a : workspace.getAlbums())
        if (a.id == albumId) { currentName = a.name; break; }

    juce::PopupMenu menu;
    menu.addItem(1, "Rename...");
    menu.addItem(2, "Delete");

    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withParentComponent(this),
        [safeThis, albumId, currentName](int result)
        {
            if (safeThis == nullptr || result <= 0) return;

            auto doRefreshAndSync = [safeThis]()
            {
                if (safeThis->sidebarModel)
                {
                    safeThis->sidebarModel->refreshRows(
                        safeThis->workspace.getChats(),
                        safeThis->workspace.getAlbums(),
                        safeThis->workspace.getReviews(),
                        safeThis->collapsedAlbums,
                        safeThis->currentChatId);
                    safeThis->chatSidebar.updateContent();
                }
                safeThis->workspace.requestMutationSync();
                safeThis->repaint();
            };

            if (result == 1) // Rename
            {
                auto* dlg = new juce::AlertWindow("Rename Album", "New name:",
                                                   juce::MessageBoxIconType::QuestionIcon);
                dlg->addTextEditor("name", currentName, "Name:");
                dlg->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
                dlg->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                dlg->enterModalState(true,
                    juce::ModalCallbackFunction::create([safeThis, dlg, albumId, doRefreshAndSync](int r)
                    {
                        juce::String trimmed = dlg->getTextEditorContents("name").trim();
                        delete dlg;
                        if (safeThis == nullptr || r != 1 || trimmed.isEmpty()) return;
                        safeThis->workspace.setAlbumName(albumId, trimmed);
                        doRefreshAndSync();
                    }), true);
            }
            else if (result == 2) // Delete
            {
                juce::AlertWindow::showOkCancelBox(
                    juce::MessageBoxIconType::WarningIcon,
                    "Delete Album",
                    "Delete this album? Its chats will become ungrouped.",
                    "Delete", "Cancel",
                    nullptr,
                    juce::ModalCallbackFunction::create([safeThis, albumId, doRefreshAndSync](int r)
                    {
                        if (safeThis == nullptr || r != 1) return;
                        safeThis->workspace.removeAlbum(albumId);
                        doRefreshAndSync();
                    }));
            }
        });
}

juce::String EchoJayEditor::createReviewFromCapture(const CaptureSnapshot& snap,
                                                      const juce::String& wavPath)
{
    if (!api.isLoggedIn()) return {};

    juce::String reviewId = juce::String(juce::Time::currentTimeMillis());
    juce::String fileName = juce::File(wavPath).getFileName();
    auto& d = snap.averagedData;

    // ---- Build waveform var (array of {x, n} objects matching web format) ----
    juce::Array<juce::var> wfArr;
    for (auto& pt : frozenWaveform)
    {
        auto* wfObj = new juce::DynamicObject();
        wfObj->setProperty("x", (double)pt.maxVal);
        wfObj->setProperty("n", (double)pt.minVal);
        wfArr.add(juce::var(wfObj));
    }

    // ---- Build WsReview ----
    WsReview rev;
    rev.id          = reviewId;
    rev.fileName    = fileName;
    rev.genre       = processorRef.getGenre();
    rev.stemType    = "mix";
    rev.channelType = snap.getChannelDisplayName();
    rev.date        = juce::Time::getCurrentTime().toISO8601(true);
    rev.audioUrl    = "";
    rev.origin      = "plugin";
    rev.label       = snap.name;    // passName — used to reconstruct the bubble label on reload
    rev.waveform    = juce::var(wfArr);

    rev.data.integ    = d.integrated;
    rev.data.range    = d.loudnessRange;
    rev.data.rmsL     = d.rmsL;
    rev.data.rmsR     = d.rmsR;
    rev.data.peakL    = d.peakMaxL;
    rev.data.peakR    = d.peakMaxR;
    rev.data.tpL      = d.truePeakMaxL;
    rev.data.tpR      = d.truePeakMaxR;
    rev.data.width    = d.width;
    rev.data.corr     = d.correlation;
    rev.data.crest    = d.crestFactor;
    rev.data.dc       = d.dcOffset;
    rev.data.duration = snap.durationSeconds;

    // Multi-channel: populate per-channel measurements
    for (auto& sch : snap.channels)
    {
        WsChannelMeasurements wsch;
        wsch.name = sch.name;
        auto& md = sch.meterData;
        wsch.data.integ    = md.integrated;
        wsch.data.range    = md.loudnessRange;
        wsch.data.rmsL     = md.rmsL;
        wsch.data.rmsR     = md.rmsR;
        wsch.data.peakL    = md.peakMaxL;
        wsch.data.peakR    = md.peakMaxR;
        wsch.data.tpL      = md.truePeakMaxL;
        wsch.data.tpR      = md.truePeakMaxR;
        wsch.data.width    = md.width;
        wsch.data.corr     = md.correlation;
        wsch.data.crest    = md.crestFactor;
        wsch.data.dc       = md.dcOffset;
        wsch.data.duration = snap.durationSeconds;
        rev.channels.push_back(wsch);
    }

    // ---- Write index.json entry ----
    juce::File captureDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                .getChildFile("EchoJay")
                                .getChildFile("Captures");
    captureDir.createDirectory();
    juce::File indexFile = captureDir.getChildFile("index.json");

    auto* indexObj = new juce::DynamicObject();
    if (indexFile.existsAsFile())
    {
        auto parsed = juce::JSON::parse(indexFile.loadFileAsString());
        if (auto* existing = parsed.getDynamicObject())
            for (auto& prop : existing->getProperties())
                indexObj->setProperty(prop.name, prop.value);
    }
    if (snap.channels.empty())
    {
        // Single-channel (host-only): existing format
        indexObj->setProperty(reviewId, fileName);
    }
    else
    {
        // Multi-channel: extended format { host: "...", channels: [...] }
        auto* entry = new juce::DynamicObject();
        entry->setProperty("host", fileName);
        juce::Array<juce::var> chNames;
        for (size_t ci = 1; ci < snap.channels.size(); ++ci)
            chNames.add(snap.channels[ci].name);
        entry->setProperty("channels", juce::var(chNames));
        indexObj->setProperty(reviewId, juce::var(entry));
    }
    indexFile.replaceWithText(juce::JSON::toString(juce::var(indexObj), false));

    // ---- Add to workspace and sync ----
    workspace.addReview(rev);
    workspace.requestMutationSync();

    // Refresh sidebar column (MIX REVIEWS section)
    if (sidebarModel)
    {
        sidebarModel->refreshRows(workspace.getChats(), workspace.getAlbums(),
                                  workspace.getReviews(), collapsedAlbums, currentChatId);
        chatSidebar.updateContent();
    }

    return reviewId;
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
        juce::String usageStr = juce::String::fromUTF8("v") + ProjectInfo::versionString
                              + " - "
                              + juce::String(used) + "/" + juce::String(limit);
        if (info.credits > 0)
            usageStr += " (+" + juce::String(info.credits) + ")";
        g.drawText(usageStr, x + w - 200, y, 200, 14, juce::Justification::centredRight);
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
    
    // EXPERIENCE LEVEL + CHAT LANGUAGE labels — side-by-side, mirroring the
    // half-width field layout in resized(). We hand-draw these two labels
    // and advance Y manually instead of using the label() helper, then
    // continue with the helper for the rest.
    {
        int halfGap = 8;
        int halfW = (juce::jmin(560, w) - halfGap) / 2;
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("EXPERIENCE LEVEL", x, y, halfW, 14, juce::Justification::centredLeft);
        g.drawText("CHAT LANGUAGE",    x + halfW + halfGap, y, halfW, 14, juce::Justification::centredLeft);
        y += labelGap + fh + 8;
    }
    
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
                              juce::Colour startCol, juce::Colour endCol, const juce::String& unit,
                              float displayValue)
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

    // Value — use displayValue if provided (for showing held max while bar decays)
    float showVal = (displayValue > -99.0f) ? displayValue : valuedB;
    g.setColour(C::text);
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    juce::String valStr = showVal > -99 ? juce::String(showVal, 1) : "N/A";
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

    // When a capture is frozen, surface the highest momentary / short-term
    // reached over the whole capture beneath the main figure.
    bool frozen = (processorRef.getCaptureState() == CaptureState::Complete && waveformFrozen);

    // 4 big numbers: Momentary, Short Term, Integrated, LRA
    struct LufsItem { const char* label; float val; const char* unit; juce::Colour col; float maxVal; };
    LufsItem items[] = {
        { "Momentary",  md.momentary,     "LUFS", md.momentary > -6 ? C::red : C::green, md.momentaryMax },
        { "Short Term", md.shortTerm,     "LUFS", C::blue2, md.shortTermMax },
        { "Integrated", md.integrated,    "LUFS", C::green, -100.0f },
        { "LRA",        md.loudnessRange, "LU",   C::text, -100.0f }
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
        // Capture max (momentary / short-term only)
        if (frozen && items[i].maxVal > -99.0f) {
            g.setColour(C::text3);
            g.setFont(juce::Font(juce::FontOptions(8.0f)));
            g.drawText("max " + ff(items[i].maxVal), cx, y + 57, cellW, 10, juce::Justification::centred);
        }
    }
}

void EchoJayEditor::paintLevelsPanel(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md)
{
    bool frozen = (processorRef.getCaptureState() == CaptureState::Complete && waveformFrozen);
    // When frozen, PEAK L/R are the highest peaks held over the capture and
    // RMS L/R is the full-capture RMS (set in PluginProcessor::stopCapture),
    // so flag the panel as showing capture figures rather than live levels.
    drawPanel(g, area, frozen ? "LEVELS - CAPTURE" : "LEVELS", C::green);

    int y = area.getY() + 28, x = area.getX() + 14, w = area.getWidth() - 28;
    int barH = 14, gap = 4;

    drawHBar(g, x, y, w, barH, "RMS L", md.rmsL, -60, 0, C::green, C::green); y += barH + gap;
    drawHBar(g, x, y, w, barH, "RMS R", md.rmsR, -60, 0, C::green, C::green); y += barH + gap + 2;

    drawHBar(g, x, y, w, barH, "PEAK L", md.peakL, -60, 0, C::amber, C::amber); y += barH + gap;
    drawHBar(g, x, y, w, barH, "PEAK R", md.peakR, -60, 0, C::amber, C::amber); y += barH + gap + 2;

    float tpL = md.truePeakL, tpR = md.truePeakR;
    float tpBarL = md.truePeakBarL, tpBarR = md.truePeakBarR;
    drawHBar(g, x, y, w, barH, "TP L", tpBarL, -60, 0,
             tpL > -1 ? C::red : C::amber, tpL > -1 ? C::red : C::amber,
             "dB", tpL); y += barH + gap;
    drawHBar(g, x, y, w, barH, "TP R", tpBarR, -60, 0,
             tpR > -1 ? C::red : C::amber, tpR > -1 ? C::red : C::amber,
             "dB", tpR); y += barH + gap + 11;

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

    // Make clear whether width/correlation are the live meter values (which
    // track the last ~1.5s) or the averaged values for a completed capture.
    {
        bool frozen = (processorRef.getCaptureState() == CaptureState::Complete && waveformFrozen);
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        g.drawText(frozen ? "capture average" : "live",
                   area.getX(), area.getY() + 4, area.getWidth() - 12, 12,
                   juce::Justification::centredRight);
    }

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
    
    int barMaxH = h - 14;
    
    // Update peak hold — rise instantly, fall slowly
    if (!spectrumPeakHoldInit) {
        spectrumPeakHold = md.spectrum;
        spectrumPeakHoldInit = true;
    }
    for (int i = 0; i < N; ++i) {
        if (md.spectrum[(size_t)i] > spectrumPeakHold[(size_t)i])
            spectrumPeakHold[(size_t)i] = md.spectrum[(size_t)i];
        else
            spectrumPeakHold[(size_t)i] -= 0.35f; // slow decay in dB per frame
    }
    
    // Auto-range: find peak from live spectrum and peak hold only (held uses its own frozen range)
    float peakDb = -200.0f;
    for (int i = 0; i < N; ++i) {
        if (md.spectrum[(size_t)i] > peakDb) peakDb = md.spectrum[(size_t)i];
        if (spectrumPeakHold[(size_t)i] > peakDb) peakDb = spectrumPeakHold[(size_t)i];
    }
    float dbMax = std::max(-20.0f, peakDb + 3.0f);
    float dbMin = dbMax - 66.0f;
    
    double logMin = std::log2(20.0), logMax = std::log2(20000.0);
    
    // Helper: get interpolated dB value at fractional bin position (cubic)
    auto getInterpolatedDb = [&](const std::array<float, 64>& spectrum, float fBin) -> float {
        int i0 = juce::jlimit(0, N - 1, (int)fBin);
        int i1 = juce::jlimit(0, N - 1, i0 + 1);
        int im1 = juce::jlimit(0, N - 1, i0 - 1);
        int i2 = juce::jlimit(0, N - 1, i0 + 2);
        float t = fBin - (float)i0;
        // Catmull-Rom interpolation
        float a = spectrum[(size_t)im1], b = spectrum[(size_t)i0];
        float c = spectrum[(size_t)i1], d = spectrum[(size_t)i2];
        float v = b + 0.5f * t * (c - a + t * (2.0f*a - 5.0f*b + 4.0f*c - d + t * (3.0f*(b-c) + d - a)));
        return v;
    };
    
    // Number of interpolated points for smooth curve
    int numInterp = juce::jmin(w, 256);
    
    // Helper lambda to build a smooth spectrum path (filled to bottom)
    auto buildSpectrumPath = [&](const std::array<float, 64>& spectrum) -> juce::Path {
        juce::Path path;
        path.startNewSubPath((float)x, (float)(y + barMaxH));
        
        for (int i = 0; i < numInterp; ++i) {
            float fBin = (float)i / (float)(numInterp - 1) * (float)(N - 1);
            float db = juce::jlimit(dbMin, dbMax, getInterpolatedDb(spectrum, fBin));
            float n2 = (db - dbMin) / (dbMax - dbMin);
            double binFreq = 20.0 * std::pow(2.0, (double)fBin / (double)N * (logMax - logMin));
            double normPos = (std::log2(binFreq) - logMin) / (logMax - logMin);
            float px = (float)x + (float)(normPos * w);
            float py = (float)(y + barMaxH) - n2 * (float)barMaxH;
            path.lineTo(px, py);
        }
        
        path.lineTo((float)(x + w), (float)(y + barMaxH));
        path.closeSubPath();
        return path;
    };
    
    // Helper to build just the top line (no fill)
    auto buildSpectrumLine = [&](const std::array<float, 64>& spectrum) -> juce::Path {
        juce::Path path;
        bool started = false;
        
        for (int i = 0; i < numInterp; ++i) {
            float fBin = (float)i / (float)(numInterp - 1) * (float)(N - 1);
            float db = juce::jlimit(dbMin, dbMax, getInterpolatedDb(spectrum, fBin));
            float n2 = (db - dbMin) / (dbMax - dbMin);
            double binFreq = 20.0 * std::pow(2.0, (double)fBin / (double)N * (logMax - logMin));
            double normPos = (std::log2(binFreq) - logMin) / (logMax - logMin);
            float px = (float)x + (float)(normPos * w);
            float py = (float)(y + barMaxH) - n2 * (float)barMaxH;
            if (!started) { path.startNewSubPath(px, py); started = true; }
            else path.lineTo(px, py);
        }
        return path;
    };
    
    g.saveState();
    g.reduceClipRegion(x, y, w, h);
    
    // Draw held spectrum first (behind) if valid — fades out smoothly
    if (heldSpectrumValid && heldSpectrumAlpha > 0.01f) {
        // Build held path using its own frozen dB range so it doesn't duck
        juce::Path heldPath;
        heldPath.startNewSubPath((float)x, (float)(y + barMaxH));
        for (int i = 0; i < numInterp; ++i) {
            float fBin = (float)i / (float)(numInterp - 1) * (float)(N - 1);
            float db = juce::jlimit(heldDbMin, heldDbMax, getInterpolatedDb(heldSpectrum, fBin));
            float n2 = (db - heldDbMin) / (heldDbMax - heldDbMin);
            double binFreq = 20.0 * std::pow(2.0, (double)fBin / (double)N * (logMax - logMin));
            double normPos = (std::log2(binFreq) - logMin) / (logMax - logMin);
            float px = (float)x + (float)(normPos * w);
            float py = (float)(y + barMaxH) - n2 * (float)barMaxH;
            heldPath.lineTo(px, py);
        }
        heldPath.lineTo((float)(x + w), (float)(y + barMaxH));
        heldPath.closeSubPath();
        
        bool heldIsRef = !processorRef.abPlayingRef.load();
        float ha = heldSpectrumAlpha;
        
        if (heldIsRef) {
            juce::ColourGradient heldGrad(
                juce::Colour(0xffFF6B9D).withAlpha(0.2f * ha), (float)x, (float)(y + barMaxH),
                juce::Colour(0xffFF8FAB).withAlpha(0.25f * ha), (float)x, (float)y, false);
            g.setGradientFill(heldGrad);
        } else {
            juce::ColourGradient heldGrad(
                C::blue.withAlpha(0.25f * ha), (float)x, (float)(y + barMaxH),
                juce::Colour(0xff60A5FA).withAlpha(0.3f * ha), (float)x, (float)y, false);
            g.setGradientFill(heldGrad);
        }
        g.fillPath(heldPath);
        
        // Held spectrum outline
        g.setColour(heldIsRef ? juce::Colour(0xffFF6B9D).withAlpha(0.35f * ha) : C::blue.withAlpha(0.4f * ha));
        g.strokePath(heldPath, juce::PathStrokeType(1.0f));
    }
    
    // Draw active spectrum on top
    auto activePath = buildSpectrumPath(md.spectrum);
    bool isRef = processorRef.abPlayingRef.load();
    
    // Peak hold: translucent fill between current and peak
    auto peakPath = buildSpectrumPath(spectrumPeakHold);
    if (isRef && processorRef.abActive.load()) {
        g.setColour(juce::Colour(0xffFF6B9D).withAlpha(0.1f));
    } else {
        g.setColour(C::blue.withAlpha(0.1f));
    }
    g.fillPath(peakPath);
    
    // Active spectrum fill
    if (isRef && processorRef.abActive.load()) {
        juce::ColourGradient activeGrad(
            juce::Colour(0xffFF6B9D).withAlpha(0.4f), (float)x, (float)(y + barMaxH),
            juce::Colour(0xffFF8FAB).withAlpha(0.55f), (float)x, (float)y, false);
        g.setGradientFill(activeGrad);
    } else {
        juce::ColourGradient activeGrad(
            C::blue.withAlpha(0.5f), (float)x, (float)(y + barMaxH),
            C::purple.withAlpha(0.65f), (float)x, (float)y, false);
        g.setGradientFill(activeGrad);
    }
    g.fillPath(activePath);
    
    // Active spectrum outline
    g.setColour(isRef && processorRef.abActive.load() 
        ? juce::Colour(0xffFF8FAB).withAlpha(0.6f) 
        : juce::Colours::white.withAlpha(0.35f));
    g.strokePath(activePath, juce::PathStrokeType(1.2f));
    
    // Peak hold line — thin bright line at the peak
    auto peakLine = buildSpectrumLine(spectrumPeakHold);
    if (isRef && processorRef.abActive.load()) {
        g.setColour(juce::Colour(0xffFF8FAB).withAlpha(0.35f));
    } else {
        g.setColour(juce::Colours::white.withAlpha(0.25f));
    }
    g.strokePath(peakLine, juce::PathStrokeType(1.0f));
    
    g.restoreState();

    // Frequency axis labels
    g.setColour(C::text3);
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    struct FreqLabel { const char* text; double freq; };
    FreqLabel labels[] = { {"50",50}, {"100",100}, {"250",250}, {"500",500},
                           {"1k",1000}, {"2k",2000}, {"5k",5000}, {"10k",10000}, {"20k",20000} };
    double logMin2 = std::log2(20.0), logMax2 = std::log2(20000.0);
    for (auto& fl : labels) {
        double logF = std::log2(fl.freq);
        double normPos = (logF - logMin2) / (logMax2 - logMin2);
        int lx = x + (int)(normPos * w);
        g.drawText(fl.text, lx - 12, y + barMaxH + 1, 24, 12, juce::Justification::centred);
    }
    
    // A/B indicator
    if (processorRef.abActive.load() && heldSpectrumValid) {
        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.setColour(isRef ? juce::Colour(0xffFF6B9D) : C::blue);
        g.drawText(isRef ? "REF" : "DAW", x + w - 30, y - 14, 30, 12, juce::Justification::centredRight);
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
        auto info = s.name + " | " + s.getChannelDisplayName() + " | " +
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

    // Draw waveform as smooth envelope path
    float centreY = (float)y + (float)h * 0.5f;
    float halfH = (float)h * 0.45f;
    float pxPerPt = (float)w / (float)numPts;

    // Clip to waveform area
    g.saveState();
    g.reduceClipRegion(x, y, w, h);

    // Centre line
    g.setColour(C::border2);
    g.drawHorizontalLine((int)centreY, (float)x, (float)(x + w));

    // Playback position indicator
    float playFrac = -1.0f;
    if (isPlayingBack && recorder.getRecordedSampleCount() > 0)
        playFrac = (float)playbackPosition / (float)recorder.getRecordedSampleCount();

    // Build top and bottom envelope paths
    juce::Path topPath, bottomPath;
    topPath.startNewSubPath((float)x, centreY);
    bottomPath.startNewSubPath((float)x, centreY);
    
    for (int i = 0; i < numPts; ++i)
    {
        float px = (float)x + ((float)i + 0.5f) * pxPerPt;
        float maxV = waveform[(size_t)i].maxVal;
        float minV = waveform[(size_t)i].minVal;
        topPath.lineTo(px, centreY - maxV * halfH);
        bottomPath.lineTo(px, centreY - minV * halfH);
    }
    
    topPath.lineTo((float)(x + w), centreY);
    bottomPath.lineTo((float)(x + w), centreY);

    // Draw played portion with gradient
    if (playFrac >= 0.0f)
    {
        float cursorPx = (float)x + playFrac * (float)w;
        
        // Played portion
        g.saveState();
        g.reduceClipRegion(x, y, (int)(cursorPx - x), h);
        juce::ColourGradient playedGrad(C::blue.withAlpha(0.85f), (float)x, centreY,
                                         C::purple.withAlpha(0.85f), cursorPx, centreY, false);
        g.setGradientFill(playedGrad);
        g.fillPath(topPath);
        g.fillPath(bottomPath);
        g.restoreState();
        
        // Unplayed portion
        g.saveState();
        g.reduceClipRegion((int)cursorPx, y, x + w - (int)cursorPx, h);
        g.setColour(C::blue.interpolatedWith(C::purple, 0.5f).withAlpha(0.2f));
        g.fillPath(topPath);
        g.fillPath(bottomPath);
        g.restoreState();
    }
    else
    {
        // No playback — full gradient fill
        float alpha = waveformFrozen ? 0.7f : 0.85f;
        juce::ColourGradient grad(C::blue.withAlpha(alpha), (float)x, centreY,
                                   C::purple.withAlpha(alpha), (float)(x + w), centreY, false);
        g.setGradientFill(grad);
        g.fillPath(topPath);
        g.fillPath(bottomPath);
    }

    // Playback cursor line
    if (playFrac >= 0.0f && playFrac <= 1.0f)
    {
        float cursorX = (float)x + playFrac * (float)w;
        g.setColour(C::green);
        g.drawVerticalLine((int)cursorX, (float)y, (float)(y + h));
    }

    // Restore clip region so recording dot and duration text aren't clipped
    g.restoreState();

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
    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);
    juce::Timer::callAfterDelay((int)durationMs + 500, [safeThis]() {
        if (safeThis == nullptr)
            return;
        safeThis->stopPlayback();
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
    auto bounds = getLocalBounds();
    g.fillAll(C::bg);


    // === Login Screen ===
    if (currentScreen == Screen::Login) {
        int cx = bounds.getCentreX(), cy = bounds.getCentreY() - 40;
        g.setGradientFill(juce::ColourGradient(
            C::blue.withAlpha(0.06f), (float)cx, (float)cy,
            juce::Colours::transparentBlack, (float)cx, (float)(cy + 300), true));
        g.fillRect(bounds);
        
        // Logo above the form. We paint this rather than using a Label
        // because it's a PNG with gradient styling that can't be reproduced
        // with text. Bounds match the loginTitle slot from resized() so
        // the form layout doesn't shift. Centered manually since drawLogo
        // left-aligns within its bounds.
        {
            int formW = juce::jmin(340, bounds.getWidth() - 60);
            int formX = (bounds.getWidth() - formW) / 2;
            int titleY = bounds.getHeight() / 2 - 120;
            int titleH = 40;
            
            juce::Image cachedLogo = getLogoImage();
            if (cachedLogo.isValid())
            {
                float aspect = (float)cachedLogo.getWidth() / (float)cachedLogo.getHeight();
                float drawH = (float)titleH;
                float drawW = drawH * aspect;
                // Clamp width to the form width so the logo doesn't overflow
                // on very narrow windows; recalc height to preserve aspect.
                if (drawW > (float)formW)
                {
                    drawW = (float)formW;
                    drawH = drawW / aspect;
                }
                float x = (float)bounds.getCentreX() - drawW * 0.5f;
                float y = (float)titleY + ((float)titleH - drawH) * 0.5f;
                g.setOpacity(1.0f);
                g.drawImage(cachedLogo,
                            juce::Rectangle<float>(x, y, drawW, drawH),
                            juce::RectanglePlacement::stretchToFit);
            }
        }
        
        EchoJayLookAndFeel::drawGrainOverlay(g, bounds, 0.015f);
        return;
    }

    // === Loading Screen ===
    if (currentScreen == Screen::Loading) {
        g.setColour(C::bg);
        g.fillRect(bounds);
        
        // Logo
        EchoJayLookAndFeel::drawLogo(g, juce::Rectangle<float>(
            (float)(bounds.getCentreX() - 60), (float)(bounds.getCentreY() - 40), 120.0f, 36.0f), 28.0f);
        
        // Animated dots
        int dotCount = ((int)(juce::Time::getMillisecondCounter() / 400)) % 4;
        juce::String dots;
        for (int i = 0; i < dotCount; ++i) dots += ".";
        
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText("Connecting" + dots, bounds.getCentreX() - 50, bounds.getCentreY() + 10, 100, 24, juce::Justification::centred);
        
        EchoJayLookAndFeel::drawGrainOverlay(g, bounds, 0.015f);
        return;
    }

    // === Main Screen ===
    // topH = 32px header + 28px tab bar = 60; content starts at y=60.
    int topH = 32 + kTabBarH;
    bool chatOnlyMode   = (currentTab == Tab::Chat);
    bool comingSoonTab  = (currentTab == Tab::Chain);
    bool linkMonitorTab = (currentTab == Tab::Link);
    int chatW, mW;

    if (compactMode || chatOnlyMode)
    {
        chatW = bounds.getWidth();
        mW = 0;
    }
    else if (visualOnlyMode || comingSoonTab)
    {
        chatW = 0;
        mW = bounds.getWidth();
    }
    else
    {
        chatW = juce::jlimit(280, 420, bounds.getWidth() * 35 / 100);
        mW = bounds.getWidth() - chatW;
    }

    // Top bar background (32px header band only)
    g.setColour(C::bg2);
    g.fillRect(0, 0, bounds.getWidth(), 32);
    g.setColour(C::border);
    g.drawHorizontalLine(31, 0.0f, (float)bounds.getWidth());
    EchoJayLookAndFeel::drawLogo(g, juce::Rectangle<float>(12, 0, 110, 32.0f), 18.0f);

    // Tier badge next to logo
    if (api.isLoggedIn())
    {
        auto info = api.getUserInfo();
        if (info.tierLevel >= 1)
            EchoJayLookAndFeel::drawTierBadge(g, 118, 8, info.tierLevel);
        else
        {
            // FREE badge — subtle grey pill
            auto freeBounds = juce::Rectangle<float>(118.0f, 8.0f, 36.0f, 16.0f);
            g.setColour(C::bg4);
            g.fillRoundedRectangle(freeBounds, 4.0f);
            g.setColour(C::text3);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText("FREE", freeBounds, juce::Justification::centred);
        }
    }
    EchoJayLookAndFeel::drawGrainOverlay(g, juce::Rectangle<int>(0, 0, bounds.getWidth(), 32), 0.015f);

    // Teal separators between header buttons
    if (!compactMode && !visualOnlyMode && currentScreen == Screen::Main)
    {
        g.setColour(juce::Colour(0xff06b6d4).withAlpha(0.2f));
        auto drawSep = [&](int x) {
            g.drawVerticalLine(x, 6.0f, 26.0f); // within 32px header band
        };
        // Separators between: [channel|genre|Capture|Compare|Settings|Plugins]
        auto cmpBounds = compareBtn.getBounds();
        auto sBounds = settingsBtn.getBounds();
        auto scBounds = scanBtn.getBounds();
        if (cmpBounds.getX() > 0) drawSep(cmpBounds.getX() - 2);
        if (sBounds.getX() > 0) drawSep(sBounds.getX() - 2);
        if (scBounds.getX() > 0) drawSep(scBounds.getX() - 2);
    }

    // Compact/expand toggle — top right of main top bar
    if (!visualOnlyMode)
    {
        int iconX = bounds.getWidth() - 24;
        int iconY = 8;
        int s = 16;
        g.setColour(C::text3);
        if (compactMode)
        {
            // Expand icon — two arrows pointing outward
            g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + 2, (float)iconY + s - 6, 1.5f);
            g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + 6, (float)iconY + s - 2, 1.5f);
            g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s - 2, (float)iconY + 6, 1.5f);
            g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s - 6, (float)iconY + 2, 1.5f);
        }
        else
        {
            // Compact icon — two arrows pointing inward
            g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + s/2, (float)iconY + s/2, (float)iconX + s/2, (float)iconY + s/2 + 4, 1.5f);
            g.drawLine((float)iconX + s/2, (float)iconY + s/2, (float)iconX + s/2 - 4, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + s/2, (float)iconY + s/2, (float)iconX + s/2, (float)iconY + s/2 - 4, 1.5f);
            g.drawLine((float)iconX + s/2, (float)iconY + s/2, (float)iconX + s/2 + 4, (float)iconY + s/2, 1.5f);
        }
    }

    // Visual-only toggle icon — diamond/eye icon, second from right
    if (!compactMode && !visualOnlyMode)
    {
        int iconX2 = bounds.getWidth() - 48;
        int iconY2 = 8;
        int s = 16;
        g.setColour(visualMode ? juce::Colour(0xff06b6d4) : C::text3);
        // Diamond shape
        g.drawLine((float)iconX2 + s/2, (float)iconY2 + 2,
                   (float)iconX2 + s - 2, (float)iconY2 + s/2, 1.5f);
        g.drawLine((float)iconX2 + s - 2, (float)iconY2 + s/2,
                   (float)iconX2 + s/2, (float)iconY2 + s - 2, 1.5f);
        g.drawLine((float)iconX2 + s/2, (float)iconY2 + s - 2,
                   (float)iconX2 + 2, (float)iconY2 + s/2, 1.5f);
        g.drawLine((float)iconX2 + 2, (float)iconY2 + s/2,
                   (float)iconX2 + s/2, (float)iconY2 + 2, 1.5f);
        // Center dot
        g.fillEllipse((float)iconX2 + s/2 - 2, (float)iconY2 + s/2 - 2, 4.0f, 4.0f);
    }

    // Visual-only mode — expand/exit icon in top right
    if (visualOnlyMode)
    {
        int iconX = bounds.getWidth() - 24;
        int iconY = 8;
        int s = 16;
        g.setColour(C::text3);
        g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
        g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
        g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + 2, (float)iconY + s - 6, 1.5f);
        g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + 6, (float)iconY + s - 2, 1.5f);
        g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s - 2, (float)iconY + 6, 1.5f);
        g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s - 6, (float)iconY + 2, 1.5f);
    }

    // === V2 Tab Bar (y=32, height=kTabBarH) ===
    if (!visualOnlyMode)
    {
        static constexpr const char* kTabNames[] = {
            "VISUALISATION", "METERS", "CHAT", "COMPARE", "LINK", "CHAIN", "SETTINGS"
        };
        constexpr int kTabCount = 7;
        const int W  = bounds.getWidth();
        const int tabW = W / kTabCount;
        const int tabY = 32;

        // Tab bar background
        g.setColour(C::bg);
        g.fillRect(0, tabY, W, kTabBarH);
        // Bottom border
        g.setColour(C::border);
        g.drawHorizontalLine(tabY + kTabBarH - 1, 0.0f, (float)W);

        for (int i = 0; i < kTabCount; ++i)
        {
            const int tx = i * tabW;
            const int tw = (i == kTabCount - 1) ? W - tx : tabW;
            const Tab t  = static_cast<Tab>(i);
            const bool active = (currentTab == t);

            if (active)
            {
                g.setColour(juce::Colour(0xff06b6d4).withAlpha(0.09f));
                g.fillRect(tx, tabY, tw, kTabBarH - 1);
                g.setColour(juce::Colour(0xff22d3ee));
                g.fillRect(tx + 2, tabY + kTabBarH - 2, tw - 4, 2);
            }

            g.setColour(active ? juce::Colour(0xff22d3ee) : C::text3);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText(kTabNames[i], tx, tabY, tw, kTabBarH, juce::Justification::centred);
        }
    }

    // CHAIN tab panel backgrounds and headers
    if (comingSoonTab)
    {
        int leftW = juce::jmin(300, bounds.getWidth() / 3);
        g.setColour(juce::Colour(0xff161616));
        g.fillRect(0, topH, leftW, bounds.getHeight() - topH);
        // Panel header
        g.setColour(juce::Colour(0xff1e1e1e));
        g.fillRect(0, topH, leftW, 32);
        g.setColour(juce::Colour(0xff333333));
        g.drawHorizontalLine(topH + 31, 0.0f, (float)leftW);
        g.setColour(juce::Colour(0xffaaaaaa));
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText("PLUGINS", 14, topH, leftW - 14, 32, juce::Justification::centredLeft);
        // Divider between left panel and editor
        g.setColour(juce::Colour(0xff2a2a2a));
        g.drawVerticalLine(leftW, (float)topH, (float)bounds.getHeight());
        // Editor panel header
        g.setColour(juce::Colour(0xff1e1e1e));
        g.fillRect(leftW + 1, topH, bounds.getWidth() - leftW - 1, 32);
        g.setColour(juce::Colour(0xff333333));
        g.drawHorizontalLine(topH + 31, (float)(leftW + 1), (float)bounds.getWidth());
        g.setColour(juce::Colour(0xffaaaaaa));
        g.drawText("PLUGIN EDITOR", leftW + 14, topH, bounds.getWidth() - leftW - 28, 32,
                   juce::Justification::centredLeft);
    }

    // Link monitor panel (left side only — chat occupies the right)
    if (linkMonitorTab)
    {
        paintLinkMonitorPanel(g, juce::Rectangle<int>(0, topH, mW, bounds.getHeight() - topH));
        // Divider between link monitor and chat panel
        if (!compactMode)
        {
            g.setColour(C::border);
            g.drawVerticalLine(mW, (float)topH, (float)bounds.getHeight());
        }
    }

    if (!compactMode && !chatOnlyMode && (!visualOnlyMode || (visualOnlyMode && !visualMode)) && !comingSoonTab && !linkMonitorTab)
    {
    // Vertical divider (not in visual-only / metersOnly mode)
    if (!visualOnlyMode) {
        g.setColour(C::border);
        g.drawVerticalLine(mW, (float)topH, (float)bounds.getHeight());
    }

    // === Meter Panel Content ===
    int pad = 10;
    int contentY = topH + 6;
    int contentW = mW - pad * 2;
    int abBarOffset = abBarShowing ? kAbBarH : 0; // shrink content when AB bar showing

    if (currentView == View::Compare && currentTab == Tab::Compare) {
        auto cArea = juce::Rectangle<int>(pad, topH + 4, contentW, bounds.getHeight() - topH - 16 - abBarOffset);
        paintCompareView(g, cArea);
    }
    else if (currentView == View::Settings && currentTab == Tab::Settings) {
        auto sArea = juce::Rectangle<int>(pad, topH + 12, contentW, bounds.getHeight() - topH - 24 - abBarOffset);
        paintSettingsView(g, sArea);
    }
    else if (channelPromptVisible || genrePromptVisible)
    {
        // Don't paint meters behind the prompt overlay
    }
    else if (visualMode)
    {
        // Visual mode — particle visual is a child component (OpenGL), just paint the number strip
        int stripH = 28;
        auto stripArea = juce::Rectangle<int>(0, bounds.getHeight() - stripH - abBarOffset, mW, stripH);
        // After a capture is frozen, show the capture's averaged data (same as the
        // standard meter view does) rather than the live EMA values. Otherwise the
        // strip keeps reporting the last ~1.5s of audio — e.g. correlation reading
        // the moment you stopped instead of the capture average.
        auto stripState = processorRef.getCaptureState();
        auto md2 = (stripState == CaptureState::Complete && waveformFrozen)
                       ? processorRef.getLatestSnapshot().averagedData
                       : processorRef.getMeterEngine().getMeterData();
        particleVisual->paintNumberStrip(g, stripArea, md2);
    }
    else
    {
        // Standard meter view
        auto state = processorRef.getCaptureState();
        auto md = (state == CaptureState::Complete && waveformFrozen)
                      ? processorRef.getLatestSnapshot().averagedData
                      : processorRef.getMeterEngine().getMeterData();
        int secGap = 8;
        int y = contentY;

        auto& recorder = processorRef.getWaveformRecorder();
        bool hasWaveform = recorder.getRecordedSampleCount() > 0 || !frozenWaveform.empty();
        if (hasWaveform)
        {
            int waveH = 72;
            paintWaveformPanel(g, { pad, y, contentW, waveH });
            y += waveH + secGap;
        }

        int loudH = 98;
        loudnessPanelBounds = { pad, y, contentW, loudH };
        paintLoudnessPanel(g, loudnessPanelBounds, md);
        y += loudH + secGap;

        int levelsW = (contentW - secGap) * 50 / 100;
        int stereoW = contentW - levelsW - secGap;
        int remainH = bounds.getHeight() - y - 160;
        int levStereoH = std::min(300, hasWaveform ? (remainH * 45 / 100) : (remainH * 50 / 100));
        levStereoH = std::max(210, levStereoH);
        paintLevelsPanel(g, { pad, y, levelsW, levStereoH }, md);
        paintStereoPanel(g, { pad + levelsW + secGap, y, stereoW, levStereoH }, md);
        y += levStereoH + secGap;

        int specH = std::max(80, bounds.getHeight() - y - pad - 20 - abBarOffset);
        paintSpectrumPanel(g, { pad, y, contentW, specH }, md);
    }
    
    // Preset / theme selector strip — VISUALISATION tab only, below the particle visual
    if (currentTab == Tab::Visualisation && visualMode && !channelPromptVisible && !genrePromptVisible && !visualOnlyMode)
    {
        int numStripH = 28;
        int stripH = 30;
        int stripY = bounds.getHeight() - numStripH - stripH - abBarOffset;

        g.setColour(C::bg2);
        g.fillRect(0, stripY, mW, stripH);
        g.setColour(C::border);
        g.drawHorizontalLine(stripY, 0.0f, (float)mW);

        int arrowsX = 4;
        int arrowsW = mW - arrowsX - 4;
        int midX = arrowsX + arrowsW / 2;

        // Preset arrows (left half)
        g.setColour(C::text2);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("<", arrowsX + 2, stripY, 12, stripH, juce::Justification::centred);
        g.setColour(C::text);
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        auto presetName = juce::String(ParticleVisual::getPresetName(particleVisual->currentPreset));
        g.drawText(presetName, arrowsX + 14, stripY, midX - arrowsX - 28, stripH, juce::Justification::centred);
        g.setColour(C::text2);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(">", midX - 14, stripY, 12, stripH, juce::Justification::centred);

        // Dot separator
        g.setColour(C::text3);
        g.fillEllipse((float)midX - 1.5f, (float)stripY + stripH / 2.0f - 1.5f, 3.0f, 3.0f);

        // Theme arrows (right half)
        g.setColour(C::text2);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("<", midX + 4, stripY, 12, stripH, juce::Justification::centred);
        g.setColour(juce::Colour(0xff06b6d4));
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        auto themeName = juce::String(ParticleVisual::getThemeName(particleVisual->currentTheme));
        g.drawText(themeName, midX + 16, stripY, arrowsX + arrowsW - midX - 30, stripH, juce::Justification::centred);
        g.setColour(C::text2);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(">", arrowsX + arrowsW - 14, stripY, 12, stripH, juce::Justification::centred);
    }

    } // end if (!compactMode && !visualOnlyMode)

    // A/B transport bar at very bottom — shows on ALL views when a ref is loaded
    if (processorRef.abActive.load() && !compactMode && !visualOnlyMode)
    {
        int abBarH = 32;
        int abBarY = bounds.getHeight() - abBarH;
        bool isRef = processorRef.abPlayingRef.load();
        int fullW = bounds.getWidth();
        
        // Background
        g.setColour(juce::Colour(0xff080A12));
        g.fillRect(0, abBarY, fullW, abBarH);
        g.setColour(juce::Colour(0xff1A1D30));
        g.drawHorizontalLine(abBarY, 0.0f, (float)fullW);
        
        // Play/pause button - centred icon
        int btnX = 6;
        int btnY2 = abBarY + (abBarH - 22) / 2;
        int btnS = 22;
        bool abIsRef = [&]() {
            auto refs = processorRef.getReferenceAnalyser().getReferences();
            for (auto& r : refs) if (r.path == processorRef.abFilePath) return true;
            return false;
        }();
        juce::Colour abAccent = abIsRef ? juce::Colour(0xffFF6B9D) : juce::Colour(0xff06b6d4);
        g.setColour(abAccent.withAlpha(0.12f));
        g.fillRoundedRectangle((float)btnX, (float)btnY2, (float)btnS, (float)btnS, 5.0f);
        g.setColour(abAccent.withAlpha(0.4f));
        g.drawRoundedRectangle((float)btnX, (float)btnY2, (float)btnS, (float)btnS, 5.0f, 1.0f);
        g.setColour(abIsRef ? juce::Colour(0xffFF8FAB) : juce::Colour(0xff22d3ee));
        if (isRef) {
            // Pause icon - centred
            float cx = (float)btnX + (float)btnS * 0.5f;
            float cy = (float)btnY2 + (float)btnS * 0.5f;
            g.fillRect(cx - 4.0f, cy - 5.0f, 3.0f, 10.0f);
            g.fillRect(cx + 1.0f, cy - 5.0f, 3.0f, 10.0f);
        } else {
            // Play icon - centred
            float cx = (float)btnX + (float)btnS * 0.5f;
            float cy = (float)btnY2 + (float)btnS * 0.5f;
            juce::Path tri;
            tri.addTriangle(cx - 3.0f, cy - 6.0f, cx - 3.0f, cy + 6.0f, cx + 5.0f, cy);
            g.fillPath(tri);
        }
        
        // Sync button — next to play button
        bool syncOn = processorRef.abSyncToDAW.load();
        int syncX = 33;
        int syncW = 32;
        int syncBtnY = abBarY + (abBarH - 16) / 2;
        if (syncOn) {
            g.setColour(juce::Colour(0xff06b6d4).withAlpha(0.15f));
            g.fillRoundedRectangle((float)syncX, (float)syncBtnY, (float)syncW, 16.0f, 4.0f);
            g.setColour(juce::Colour(0xff06b6d4).withAlpha(0.4f));
            g.drawRoundedRectangle((float)syncX, (float)syncBtnY, (float)syncW, 16.0f, 4.0f, 1.0f);
        }
        g.setColour(syncOn ? juce::Colour(0xff22d3ee) : C::text3);
        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.drawText("SYNC", syncX, abBarY, syncW, abBarH, juce::Justification::centred);
        
        // Ref name
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        juce::String refName = juce::File(processorRef.abFilePath).getFileNameWithoutExtension();
        if (refName.length() > 20) refName = refName.substring(0, 17) + "...";
        g.setColour(abIsRef ? juce::Colour(0xffFF8FAB) : juce::Colour(0xff22d3ee));
        g.drawText(refName, 68, abBarY, 100, abBarH, juce::Justification::centredLeft);
        
        // Waveform area - rounded, no grey box background
        int wfX = 170;
        int wfW = fullW - wfX - 28;
        int wfY2 = abBarY + 3;
        int wfH2 = abBarH - 6;
        
        if (wfW > 20 && processorRef.abSampleCount > 0) {
            float playFrac = (float)processorRef.abPlaybackPos / (float)processorRef.abSampleCount;
            
            // Rounded clip region for waveform
            g.saveState();
            g.reduceClipRegion(juce::Rectangle<int>(wfX, wfY2, wfW, wfH2).toFloat().withTrimmedLeft(0).toNearestInt());
            
            // Subtle rounded background
            g.setColour(juce::Colour(0xff0A0C18));
            g.fillRoundedRectangle((float)wfX, (float)wfY2, (float)wfW, (float)wfH2, 6.0f);
            
            // Draw smooth waveform using the buffer
            {
                std::lock_guard<std::mutex> lock(processorRef.abMutex);
                if (processorRef.abBuffer.getNumSamples() > 0) {
                    int numPts = juce::jmin(wfW * 2, 600);
                    float centreY3 = (float)wfY2 + (float)wfH2 * 0.5f;
                    float halfH3 = (float)wfH2 * 0.42f;
                    const float* ch0 = processorRef.abBuffer.getReadPointer(0);
                    int totalSamples = processorRef.abBuffer.getNumSamples();
                    
                    // Build smooth path for top and bottom envelope
                    juce::Path topPath, bottomPath;
                    topPath.startNewSubPath((float)wfX, centreY3);
                    bottomPath.startNewSubPath((float)wfX, centreY3);
                    
                    for (int i = 0; i < numPts; ++i) {
                        int sampleIdx = (int)((float)i / (float)numPts * (float)totalSamples);
                        int endIdx = (int)((float)(i + 1) / (float)numPts * (float)totalSamples);
                        float maxVal = 0.0f, minVal = 0.0f;
                        for (int s = sampleIdx; s < endIdx && s < totalSamples; s += 2) {
                            maxVal = std::max(maxVal, ch0[s]);
                            minVal = std::min(minVal, ch0[s]);
                        }
                        float px = (float)wfX + (float)i / (float)numPts * (float)wfW;
                        topPath.lineTo(px, centreY3 - maxVal * halfH3);
                        bottomPath.lineTo(px, centreY3 - minVal * halfH3);
                    }
                    
                    topPath.lineTo((float)(wfX + wfW), centreY3);
                    bottomPath.lineTo((float)(wfX + wfW), centreY3);
                    
                    // Draw played portion — purple to pink gradient glow
                    float cursorPx = (float)wfX + playFrac * (float)wfW;
                    g.saveState();
                    g.reduceClipRegion(wfX, wfY2, (int)(cursorPx - wfX), wfH2);
                    {
                        juce::Colour wfCol1 = abIsRef ? juce::Colour(0xffFF6B9D) : juce::Colour(0xff06b6d4);
                        juce::Colour wfCol2 = abIsRef ? juce::Colour(0xffFF8FAB) : juce::Colour(0xff22d3ee);
                        juce::Colour wfCol3 = abIsRef ? juce::Colour(0xffFFB3C6) : juce::Colour(0xff67e8f9);
                        juce::ColourGradient grad(
                            wfCol1.withAlpha(0.7f), (float)wfX, centreY3,
                            wfCol2.withAlpha(0.75f), cursorPx, centreY3, false);
                        grad.addColour(0.5, wfCol3.withAlpha(0.7f));
                        g.setGradientFill(grad);
                        g.fillPath(topPath);
                        g.fillPath(bottomPath);
                    }
                    g.restoreState();
                    
                    // Draw unplayed portion
                    g.saveState();
                    g.reduceClipRegion((int)cursorPx, wfY2, wfX + wfW - (int)cursorPx, wfH2);
                    g.setColour(C::text3.withAlpha(0.2f));
                    g.fillPath(topPath);
                    g.fillPath(bottomPath);
                    g.restoreState();
                }
            }
            
            g.restoreState();
            
            // Playback cursor
            float cursorX2 = (float)wfX + playFrac * (float)wfW;
            g.setColour((abIsRef ? juce::Colour(0xffFFB3C6) : juce::Colour(0xff67e8f9)).withAlpha(0.9f));
            g.drawVerticalLine((int)cursorX2, (float)wfY2 + 1, (float)(wfY2 + wfH2 - 1));
        }
        
        // X button
        g.setColour(C::text3);
        g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        g.drawText("x", fullW - 24, abBarY, 20, abBarH, juce::Justification::centred);
    }
    
    // Visual-only mode — METERS tab + preset/theme strip + number strip at bottom
    if (visualOnlyMode)
    {
        int stripH = 28;
        int toggleH = 32;
        
        int toggleY = visualMode ? bounds.getHeight() - stripH - toggleH 
                                  : bounds.getHeight() - toggleH;
        g.setColour(C::bg2);
        g.fillRect(0, toggleY, bounds.getWidth(), toggleH);
        g.setColour(C::border);
        g.drawHorizontalLine(toggleY, 0.0f, (float)bounds.getWidth());
        
        int fullW = bounds.getWidth();
        
        if (visualMode) {
            // METERS button on left + preset/theme arrows
            int toggleBtnW = 70;
            int toggleBtnX = 8;
            int toggleBtnBY = toggleY + (toggleH - 22) / 2;
            
            {
                auto mp = getMouseXYRelative();
                bool hov = mp.x >= toggleBtnX && mp.x < toggleBtnX + toggleBtnW && mp.y >= toggleY && mp.y < toggleY + toggleH;
                g.setColour(hov ? juce::Colour(0xff22d3ee) : juce::Colour(0xff0891b2));
            }
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawText("METERS", toggleBtnX, toggleY, toggleBtnW, toggleH, juce::Justification::centred);
            g.fillRect(toggleBtnX + toggleBtnW / 4, toggleY + toggleH - 2, toggleBtnW / 2, 2);
            
            // Preset/Theme arrows
            int arrowsX = toggleBtnX + toggleBtnW + 8;
            int arrowsW = fullW - arrowsX - 4;
            int arrowsMidX = arrowsX + arrowsW / 2;
            
            g.setColour(C::text2);
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawText("<", arrowsX + 4, toggleY, 14, toggleH, juce::Justification::centred);
            g.setColour(C::text);
            auto presetName2 = juce::String(ParticleVisual::getPresetName(particleVisual->currentPreset));
            g.drawText(presetName2, arrowsX + 18, toggleY, arrowsMidX - arrowsX - 36, toggleH, juce::Justification::centred);
            g.setColour(C::text2);
            g.drawText(">", arrowsMidX - 18, toggleY, 14, toggleH, juce::Justification::centred);
            
            g.setColour(C::text3);
            g.fillEllipse((float)arrowsMidX - 1.5f, (float)toggleY + toggleH / 2.0f - 1.5f, 3.0f, 3.0f);
            
            g.setColour(C::text2);
            g.drawText("<", arrowsMidX + 4, toggleY, 14, toggleH, juce::Justification::centred);
            g.setColour(juce::Colour(0xff06b6d4));
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            auto themeName2 = juce::String(ParticleVisual::getThemeName(particleVisual->currentTheme));
            g.drawText(themeName2, arrowsMidX + 18, toggleY, fullW - arrowsMidX - 36, toggleH, juce::Justification::centred);
            g.setColour(C::text2);
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawText(">", fullW - 18, toggleY, 14, toggleH, juce::Justification::centred);
        } else {
            // In meters mode — single centred VISUAL button
            int toggleBtnW = 120;
            int toggleBtnX = (fullW - toggleBtnW) / 2;
            int toggleBtnBY = toggleY + (toggleH - 22) / 2;
            
            {
                auto mp = getMouseXYRelative();
                bool hov = mp.x >= toggleBtnX && mp.x < toggleBtnX + toggleBtnW && mp.y >= toggleY && mp.y < toggleY + toggleH;
                g.setColour(hov ? juce::Colour(0xff22d3ee) : juce::Colour(0xff0891b2));
            }
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawText("VISUALISATION", toggleBtnX, toggleY, toggleBtnW, toggleH, juce::Justification::centred);
            g.fillRect(toggleBtnX + toggleBtnW / 4, toggleY + toggleH - 2, toggleBtnW / 2, 2);
        }
        
        // Number strip (only when in visual mode)
        if (visualMode) {
            auto stripArea = juce::Rectangle<int>(0, bounds.getHeight() - stripH, bounds.getWidth(), stripH);
            auto md2 = processorRef.getMeterEngine().getMeterData();
            particleVisual->paintNumberStrip(g, stripArea, md2);
        }
    }

    // === Chat Panel ===
    int chatX = compactMode ? 0 : (visualOnlyMode ? bounds.getWidth() : mW + 1);

    // Sidebar (Chat tab only) — rendered by the chatSidebar ListBox child component.
    // We just shift chatX/chatW here so the message area doesn't overlap the sidebar,
    // and draw the right-edge border line (the ListBox doesn't paint outside its bounds).
    bool hasSidebar = (currentTab == Tab::Chat && !compactMode && !visualOnlyMode);
    if (hasSidebar)
    {
        g.setColour(C::border);
        g.drawVerticalLine(chatX + kSidebarW - 1, (float)topH, (float)bounds.getHeight());
        chatX += kSidebarW;
        chatW -= kSidebarW;
    }

    if (!visualOnlyMode) {
    // Chat header — "AI ASSISTANT" bold, usage count right
    g.setColour(C::bg2);
    g.fillRect(chatX, topH, chatW, 32);
    g.setColour(C::border);
    g.drawHorizontalLine(topH + 31, (float)chatX, (float)(chatX + chatW));

    g.setColour(C::text2);
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    g.drawText("AI ASSISTANT", chatX + 14, topH, chatW, 32, juce::Justification::centredLeft);

    // (Credit counter intentionally removed from chat header — it lives in
    // the Settings panel only. Keeps the chat strip clean and avoids
    // duplicating info that's one click away.)

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
    activeChainBuildBtns = 0;
    chatWavePositions.clear();

    int msgY = 8 - scrollOffset;
    const float chatMsgFontSize = 12.0f * chatTextScale;
    for (auto& msg : chatMessages) {
        bool isUser = (msg.role == "user");

        // Capture messages get a unified card (waveform + label row) instead of
        // the text-bubble + waveform-card pattern. Always 56 px regardless of
        // content length so the layout stays predictable.
        const bool isCaptureMsg = isUser && msg.reviewId.isNotEmpty();
        static constexpr int kCaptureMsgH = 56;

        juce::AttributedString as;
        as.append(msg.content, juce::Font(juce::FontOptions(chatMsgFontSize)),
                  isUser ? C::text : C::text2);
        juce::TextLayout layout;
        layout.createLayout(as, (float)(maxBubbleW - 20));
        int textH = (int)layout.getHeight() + 20;

        // Extra height for waveform card
        int waveCardH = 0;
        if (!isCaptureMsg && msg.hasWaveform && !msg.waveform.empty())
            waveCardH = 36; // play button + waveform only

        // Extra height for "Build this chain" button on assistant messages with a chain block
        static constexpr int kChainBtnH = 26;
        bool hasChainBtn = !isUser && msg.chainData.isNotEmpty();
        int chainAreaH = hasChainBtn ? (kChainBtnH + 6) : 0;

        int tH = isCaptureMsg ? kCaptureMsgH : (textH + waveCardH + chainAreaH);
        int drawY = chatTop2 + msgY;

        if (drawY + tH > chatTop2 - 50 && drawY < chatBottomEdge + 50)
        {
            if (isUser)
            {
                int avX = chatX + chatW - avatarSize - 4;
                int avY = drawY + 2;
                // Add a 2px buffer either side so the avatar's antialiased edge
                // doesn't poke through the chat region boundary during scrolling.
                if (avY >= chatTop2 + 2 && avY + avatarSize <= chatBottomEdge - 2)
                {
                    // Snap circle coords to integer pixel boundaries (see E avatar comment).
                    const float fAvX = std::floor((float)avX);
                    const float fAvY = std::floor((float)avY);
                    const float fAvSize = (float)avatarSize;
                    g.setColour(C::bg4);
                    g.fillEllipse(fAvX, fAvY, fAvSize, fAvSize);
                    g.setColour(C::text3);
                    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                    g.drawText("U", avX, avY, avatarSize, avatarSize, juce::Justification::centred);
                }

                if (isCaptureMsg)
                {
                    // ── Unified capture card (waveform section + label row) ──────────
                    // Layout: top 38px = inner bg3 card with play button + waveform bars
                    //         bottom 18px = label line (passName · LUFS · duration)
                    int bubbleW = maxBubbleW;
                    int bubbleX = chatX + chatW - avatarSize - 12 - bubbleW;

                    // Outer card background
                    g.setColour(C::bg4);
                    g.fillRoundedRectangle((float)bubbleX, (float)drawY, (float)bubbleW, (float)kCaptureMsgH, 10.0f);

                    // Inner waveform card (top section)
                    int cardX = bubbleX + 8;
                    int cardY = drawY + 4;
                    int cardW = bubbleW - 16;
                    int cardH = 34;
                    g.setColour(C::bg3);
                    g.fillRoundedRectangle((float)cardX, (float)cardY, (float)cardW, (float)cardH, 6.0f);

                    // Origin-based playback controls
                    // "plugin" origin (or empty for old records) = local AB playback.
                    // Any other origin = web capture → link to echojay.ai, no in-plugin play.
                    const bool isPluginOrigin = msg.origin == "plugin" || msg.origin.isEmpty();
                    const bool hasLocalFile   = msg.wavFilePath.isNotEmpty();

                    int playBtnSize = 22;
                    int playX = cardX + 6;
                    int playY = cardY + (cardH - playBtnSize) / 2;
                    bool isPlaying = false;

                    if (!isPluginOrigin)
                    {
                        // Web capture — show "Open in echojay.ai" link button (amber)
                        g.setColour(juce::Colour(0xffF59E0B).withAlpha(0.18f));
                        g.fillEllipse((float)playX, (float)playY, (float)playBtnSize, (float)playBtnSize);
                        g.setColour(juce::Colour(0xffFBBF24));
                        // Draw external-link arrow: → with a small cap
                        float ax = (float)playX + 7.0f, ay = (float)playY + 11.0f;
                        g.drawLine(ax, ay, ax + 7.0f, ay, 1.5f);
                        g.drawLine(ax + 4.0f, ay - 3.0f, ax + 7.0f, ay, 1.5f);
                        g.drawLine(ax + 4.0f, ay + 3.0f, ax + 7.0f, ay, 1.5f);
                    }
                    else if (hasLocalFile)
                    {
                        // Plugin capture with local file — play/stop button (cyan/pink)
                        isPlaying = (currentlyPlayingChatWav == msg.wavFilePath)
                                 || (processorRef.abPlayingRef.load() && processorRef.abFilePath == msg.wavFilePath);
                        g.setColour(isPlaying ? juce::Colour(0xffFF6B9D).withAlpha(0.35f)
                                              : juce::Colour(0xff06b6d4).withAlpha(0.15f));
                        g.fillEllipse((float)playX, (float)playY, (float)playBtnSize, (float)playBtnSize);
                        g.setColour(isPlaying ? juce::Colour(0xffFF8FAB) : juce::Colour(0xff22d3ee));
                        if (isPlaying)
                        {
                            g.fillRect((float)playX + 7.0f, (float)playY + 7.0f, 8.0f, 8.0f);
                        }
                        else
                        {
                            juce::Path tri;
                            float triX = (float)playX + 8.0f, triY0 = (float)playY + 5.0f;
                            tri.addTriangle(triX, triY0, triX, triY0 + 12.0f, triX + 8.0f, triY0 + 6.0f);
                            g.fillPath(tri);
                        }
                    }
                    // else: plugin origin, no local file — no button drawn; label row explains

                    // Overlay (transparent click catcher over inner card)
                    // Web captures get "__open_web__" as the path; onWavePlayClick opens browser.
                    juce::String playKey;
                    if (!isPluginOrigin)
                        playKey = "__open_web__";
                    else if (hasLocalFile)
                        playKey = msg.wavFilePath;

                    if (playKey.isNotEmpty() && activeWavePlayBtns < kMaxWavePlayBtns)
                    {
                        int idx = activeWavePlayBtns++;
                        wavePlayPaths[(size_t)idx] = playKey;
                        wavePlayDurations[(size_t)idx] = msg.durationSeconds;
                        auto scrollBounds = chatScroll.getBounds();
                        bool inView = cardY >= scrollBounds.getY() && (cardY + cardH) <= scrollBounds.getBottom();
                        if (inView) {
                            wavePlayOverlays[(size_t)idx].setBounds(cardX, cardY, cardW, cardH);
                            wavePlayOverlays[(size_t)idx].setVisible(true);
                            wavePlayOverlays[(size_t)idx].toFront(false);
                            chatWavePositions.push_back({ {cardX, cardY, cardW, cardH}, playKey, msg.durationSeconds });
                        } else {
                            wavePlayOverlays[(size_t)idx].setBounds(-100, -100, 1, 1);
                            wavePlayOverlays[(size_t)idx].setVisible(false);
                        }
                    }

                    // Waveform bars (shown for all origins when waveform data is available)
                    // Always render at a fixed display density using linear interpolation so
                    // that ~80-bar web captures look identical to ~435-bar plugin captures.
                    int wfX = playX + playBtnSize + 6;
                    int wfY = cardY + 4;
                    int wfW = cardX + cardW - wfX - 6;
                    int wfH = cardH - 8;
                    int numPts = (int)msg.waveform.size();
                    if (numPts > 0 && wfW > 0)
                    {
                        constexpr int kDisplayBars = 400;
                        const int displayBars = kDisplayBars;
                        g.saveState();
                        g.reduceClipRegion(wfX, wfY, wfW, wfH);
                        float pxPerBar = (float)wfW / (float)displayBars;
                        float centreY2 = (float)wfY + (float)wfH * 0.5f;
                        float halfH2   = (float)wfH * 0.45f;
                        for (int i = 0; i < displayBars; ++i)
                        {
                            // Map display bar → fractional source position, then lerp
                            float srcPos = (float)i / (float)(displayBars - 1) * (float)(numPts - 1);
                            int   s0     = (int)srcPos;
                            int   s1     = juce::jmin(s0 + 1, numPts - 1);
                            float t      = srcPos - (float)s0;
                            float maxV   = msg.waveform[(size_t)s0].maxVal * (1.0f - t)
                                         + msg.waveform[(size_t)s1].maxVal * t;
                            float minV   = msg.waveform[(size_t)s0].minVal * (1.0f - t)
                                         + msg.waveform[(size_t)s1].minVal * t;

                            float px   = (float)wfX + (float)i * pxPerBar;
                            float top2 = centreY2 - maxV * halfH2;
                            float bot2 = centreY2 - minV * halfH2;
                            float bH2  = std::max(1.0f, bot2 - top2);
                            float frac = (float)i / (float)displayBars;
                            float alpha = 0.7f;
                            if (isPlaying)
                            {
                                float pf = 0.0f;
                                if (processorRef.abPlayingRef.load() && processorRef.abSampleCount > 0)
                                    pf = (float)processorRef.abPlaybackPos / (float)processorRef.abSampleCount;
                                else if (chatPlaybackDuration > 0 && chatPlaybackStartTime > 0) {
                                    double el = (juce::Time::getMillisecondCounterHiRes() - chatPlaybackStartTime) / 1000.0 + chatPlaybackOffset;
                                    pf = (float)(el / chatPlaybackDuration);
                                }
                                if (frac > pf) alpha = 0.25f;
                            }
                            g.setColour(C::blue.interpolatedWith(C::purple, frac).withAlpha(alpha));
                            g.fillRect(px, top2, std::max(1.0f, pxPerBar - 0.5f), bH2);
                        }
                        if (isPlaying)
                        {
                            float pf = 0.0f;
                            if (processorRef.abPlayingRef.load() && processorRef.abSampleCount > 0)
                                pf = (float)processorRef.abPlaybackPos / (float)processorRef.abSampleCount;
                            else if (chatPlaybackDuration > 0 && chatPlaybackStartTime > 0) {
                                double el = (juce::Time::getMillisecondCounterHiRes() - chatPlaybackStartTime) / 1000.0 + chatPlaybackOffset;
                                pf = (float)(el / chatPlaybackDuration);
                            }
                            pf = juce::jlimit(0.0f, 1.0f, pf);
                            g.setColour(juce::Colours::white);
                            g.drawVerticalLine((int)((float)wfX + pf * (float)wfW), (float)wfY, (float)(wfY + wfH));
                        }
                        g.restoreState();
                    }

                    // Label row
                    juce::String labelLine;
                    if (isPluginOrigin && !hasLocalFile)
                    {
                        // File is not on this machine
                        labelLine = "Audio not on this device";
                    }
                    else
                    {
                        labelLine = msg.content;
                        if (msg.lufs > -99.f)
                            labelLine += "   " + juce::String(msg.lufs, 1) + " LUFS";
                        if (msg.durationSeconds > 0.1f)
                        {
                            int secs = (int)msg.durationSeconds;
                            int mins = secs / 60; secs %= 60;
                            labelLine += "   " + (mins > 0 ? juce::String(mins) + ":" : "")
                                        + juce::String(secs).paddedLeft('0', 2) + "s";
                        }
                    }
                    g.setColour(C::text3);
                    g.setFont(juce::Font(juce::FontOptions(10.0f)));
                    g.drawText(labelLine, bubbleX + 12, drawY + kCaptureMsgH - 17,
                               bubbleW - 24, 14, juce::Justification::centredLeft);
                }
                else
                {
                    // ── Regular text bubble + optional waveform card ─────────────────
                    int textRenderedW = (int)std::ceil(layout.getWidth()) + 20;
                    int bubbleW = (msg.hasWaveform && !msg.waveform.empty())
                                    ? maxBubbleW
                                    : juce::jlimit(40, maxBubbleW, textRenderedW);
                    int bubbleX = chatX + chatW - avatarSize - 12 - bubbleW;
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

                        g.setColour(C::bg3);
                        g.fillRoundedRectangle((float)cardX, (float)cardY, (float)cardW, (float)cardH, 6.0f);

                        int playBtnSize = 22;
                        int playX = cardX + 6;
                        int playY = cardY + (cardH - playBtnSize) / 2;
                        bool isPlaying = (msg.wavFilePath.isNotEmpty() && currentlyPlayingChatWav == msg.wavFilePath)
                                      || (processorRef.abPlayingRef.load() && processorRef.abFilePath == msg.wavFilePath);

                        g.setColour(isPlaying ? juce::Colour(0xffFF6B9D).withAlpha(0.35f)
                                              : juce::Colour(0xff06b6d4).withAlpha(0.15f));
                        g.fillEllipse((float)playX, (float)playY, (float)playBtnSize, (float)playBtnSize);
                        g.setColour(isPlaying ? juce::Colour(0xffFF8FAB) : juce::Colour(0xff22d3ee));
                        if (isPlaying)
                        {
                            g.fillRect((float)playX + 7.0f, (float)playY + 7.0f, 8.0f, 8.0f);
                        }
                        else
                        {
                            juce::Path tri;
                            float triX = (float)playX + 8.0f, triY2 = (float)playY + 5.0f;
                            tri.addTriangle(triX, triY2, triX, triY2 + 12.0f, triX + 8.0f, triY2 + 6.0f);
                            g.fillPath(tri);
                        }

                        if (msg.wavFilePath.isNotEmpty() && activeWavePlayBtns < kMaxWavePlayBtns)
                        {
                            int idx = activeWavePlayBtns++;
                            wavePlayPaths[(size_t)idx] = msg.wavFilePath;
                            wavePlayDurations[(size_t)idx] = msg.durationSeconds;
                            auto scrollBounds = chatScroll.getBounds();
                            bool inView = cardY >= scrollBounds.getY() && (cardY + cardH) <= scrollBounds.getBottom();
                            if (inView) {
                                wavePlayOverlays[(size_t)idx].setBounds(cardX, cardY, cardW, cardH);
                                wavePlayOverlays[(size_t)idx].setVisible(true);
                                wavePlayOverlays[(size_t)idx].toFront(false);
                                chatWavePositions.push_back({ {cardX, cardY, cardW, cardH}, msg.wavFilePath, msg.durationSeconds });
                            } else {
                                wavePlayOverlays[(size_t)idx].setBounds(-100, -100, 1, 1);
                                wavePlayOverlays[(size_t)idx].setVisible(false);
                            }
                        }

                        int wfX = playX + playBtnSize + 6;
                        int wfY = cardY + 4;
                        int wfW = cardX + cardW - wfX - 6;
                        int wfH = cardH - 8;
                        int numPts = (int)msg.waveform.size();
                        if (numPts > 0 && wfW > 0)
                        {
                            g.saveState();
                            g.reduceClipRegion(wfX, wfY, wfW, wfH);
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
                                float alpha = 0.7f;
                                if (isPlaying)
                                {
                                    float playFrac2 = 0.0f;
                                    if (processorRef.abPlayingRef.load() && processorRef.abSampleCount > 0)
                                        playFrac2 = (float)processorRef.abPlaybackPos / (float)processorRef.abSampleCount;
                                    else if (chatPlaybackDuration > 0 && chatPlaybackStartTime > 0) {
                                        double el = (juce::Time::getMillisecondCounterHiRes() - chatPlaybackStartTime) / 1000.0 + chatPlaybackOffset;
                                        playFrac2 = (float)(el / chatPlaybackDuration);
                                    }
                                    if (frac > playFrac2) alpha = 0.25f;
                                }
                                g.setColour(C::blue.interpolatedWith(C::purple, frac).withAlpha(alpha));
                                g.fillRect(px, top2, std::max(1.0f, pxPerPt - 0.5f), bH2);
                            }
                            if (isPlaying)
                            {
                                float playFrac3 = 0.0f;
                                if (processorRef.abPlayingRef.load() && processorRef.abSampleCount > 0)
                                    playFrac3 = (float)processorRef.abPlaybackPos / (float)processorRef.abSampleCount;
                                else if (chatPlaybackDuration > 0 && chatPlaybackStartTime > 0) {
                                    double el = (juce::Time::getMillisecondCounterHiRes() - chatPlaybackStartTime) / 1000.0 + chatPlaybackOffset;
                                    playFrac3 = (float)(el / chatPlaybackDuration);
                                }
                                playFrac3 = juce::jlimit(0.0f, 1.0f, playFrac3);
                                float cursorX = (float)wfX + playFrac3 * (float)wfW;
                                g.setColour(juce::Colours::white);
                                g.drawVerticalLine((int)cursorX, (float)wfY, (float)(wfY + wfH));
                            }
                            g.restoreState();
                        }
                    }
                }
            }
            else
            {
                int avX = chatX + 6;
                int avY = drawY + 2;
                // 2px buffer so AA fringes don't bleed through the clip edge while scrolling.
                if (avY >= chatTop2 + 2 && avY + avatarSize <= chatBottomEdge - 2)
                {
                    // Snap circle coords to integer pixel boundaries so the
                    // ellipse rasterizes identically every frame during scroll.
                    // Without this, sub-pixel positions cause the right edge
                    // of the ring to flatten/jag as scrollOffset changes.
                    const float fAvX = std::floor((float)avX);
                    const float fAvY = std::floor((float)avY);
                    const float fAvSize = (float)avatarSize;
                    g.setColour(C::purple);
                    g.fillEllipse(fAvX, fAvY, fAvSize, fAvSize);
                    g.setColour(juce::Colours::white);
                    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                    g.drawText("E", avX, avY, avatarSize, avatarSize, juce::Justification::centred);
                }
                
                int bubbleX = avX + avatarSize + 6;
                int bubbleW = chatW - avatarSize - 20;
                int bubbleH = tH - chainAreaH;
                g.setColour(C::bg3);
                g.fillRoundedRectangle((float)bubbleX, (float)drawY, (float)bubbleW, (float)bubbleH, 10.0f);
                layout.draw(g, { (float)(bubbleX + 10), (float)(drawY + 10), (float)(bubbleW - 20), (float)(bubbleH - 20) });

                // "Build this chain" button
                if (hasChainBtn && activeChainBuildBtns < kMaxChainBuildBtns)
                {
                    int btnIdx = activeChainBuildBtns++;
                    chainBuildJsons[(size_t)btnIdx] = msg.chainData;
                    int btnX = bubbleX + 10;
                    int btnY = drawY + bubbleH + 4;
                    int btnW = juce::jmin(160, bubbleW - 20);
                    chainBuildBtns[(size_t)btnIdx].setButtonText("Build this chain");
                    auto scrollBounds2 = chatScroll.getBounds();
                    bool btnInView = btnY >= scrollBounds2.getY()
                                  && (btnY + kChainBtnH) <= scrollBounds2.getBottom();
                    if (btnInView)
                    {
                        chainBuildBtns[(size_t)btnIdx].setBounds(btnX, btnY, btnW, kChainBtnH);
                        chainBuildBtns[(size_t)btnIdx].setVisible(true);
                        chainBuildBtns[(size_t)btnIdx].toFront(false);
                    }
                    else
                    {
                        chainBuildBtns[(size_t)btnIdx].setBounds(-100, -100, 1, 1);
                        chainBuildBtns[(size_t)btnIdx].setVisible(false);
                    }
                }
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
            g.setFont(juce::Font(juce::FontOptions(12.0f * chatTextScale)));
            g.drawText("Analysing...", avX + avatarSize + 12, drawY + 4, 100, 20, juce::Justification::centredLeft);
        }
    }

    g.restoreState();
    
    // Hide unused overlay buttons
    for (int i = activeWavePlayBtns; i < kMaxWavePlayBtns; ++i)
        wavePlayOverlays[(size_t)i].setVisible(false);
    for (int i = activeChainBuildBtns; i < kMaxChainBuildBtns; ++i)
        chainBuildBtns[(size_t)i].setVisible(false);

    } // end if (!visualOnlyMode) — chat section

    if (channelPromptVisible)
        paintChannelPromptOverlay(g, bounds);
    else if (genrePromptVisible)
        paintGenrePromptOverlay(g, bounds);
    
    // Update overlay is now a separate child component (updateOverlay) — it
    // paints itself ON TOP of all other children. See UpdateOverlay::paint.
}

void EchoJayEditor::paintChannelPromptOverlay(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour(juce::Colours::black.withAlpha(0.72f));
    g.fillRect(bounds);

    auto card = bounds.reduced(40, 34);
    g.setColour(C::bg2);
    g.fillRoundedRectangle(card.toFloat(), 16.0f);
    g.setColour(C::border2);
    g.drawRoundedRectangle(card.toFloat(), 16.0f, 1.0f);

    auto accent = card.removeFromTop(6).reduced(24, 0);
    g.setColour(C::blue.withAlpha(0.85f));
    g.fillRoundedRectangle(accent.removeFromLeft(card.getWidth() / 3).toFloat(), 3.0f);
    g.setColour(C::purple.withAlpha(0.85f));
    g.fillRoundedRectangle(accent.toFloat(), 3.0f);
}

// ============================================================================
// Resized
// ============================================================================

void EchoJayEditor::resized()
{
    // No transform — layout scales to actual window size
    auto b = getLocalBounds();

    // Update overlay covers the full editor and is always brought to front
    updateOverlay.setBounds(b);
    if (updateOverlay.isVisible())
        updateOverlay.toFront(false);

    // Plugin review overlay tracks the window and stays on top while shown.
    if (reviewOverlay.visibleState)
    {
        layoutPluginReview();
        reviewOverlay.toFront(false);
    }

    // channelPromptBlocker removed — overlay is painted in paint()

    if (currentScreen == Screen::Login) {
        // Hide all overlay components
        for (int i = 0; i < kMaxWavePlayBtns; ++i)
            wavePlayOverlays[(size_t)i].setVisible(false);
        upgradeBtn.setVisible(false);
        playbackBtn.setVisible(false);
        chatScroll.setVisible(false);
        chatInput.setVisible(false);
        chatSendBtn.setVisible(false);
        chatTextSizeBtn.setVisible(false);
        
        int formW = juce::jmin(340, b.getWidth() - 60);
        int formX = (b.getWidth() - formW) / 2;
        int y = b.getHeight() / 2 - 120;
        loginTitle.setBounds(formX, y, formW, 40); y += 44;
        loginSubtitle.setBounds(formX, y, formW, 24); y += 40;
        emailInput.setBounds(formX, y, formW, 36); y += 44;
        passwordInput.setBounds(formX, y, formW, 36); y += 44;
        loginBtn.setBounds(formX, y, formW, 38); y += 48;
        loginErrorLabel.setBounds(formX, y, formW, 20); y += 30;
        signUpLabel.setBounds(formX, y, formW, 18); y += 22;
        signUpBtn.setBounds(formX + (formW - 120) / 2, y, 120, 32);
        return;
    }

    // Loading screen — nothing to layout, paint() handles it
    if (currentScreen == Screen::Loading)
        return;

    int topH = 32 + kTabBarH; // 32px header + 28px tab bar
    bool chatOnlyMode  = (currentTab == Tab::Chat);
    bool comingSoonTab  = (currentTab == Tab::Chain);
    bool linkMonitorTab = (currentTab == Tab::Link);
    int chatW, mW;

    if (compactMode || chatOnlyMode)
    {
        chatW = b.getWidth();
        mW = 0;
    }
    else if (visualOnlyMode || comingSoonTab)
    {
        chatW = 0;
        mW = b.getWidth();
    }
    else
    {
        chatW = juce::jlimit(240, 380, b.getWidth() * 32 / 100);
        mW = b.getWidth() - chatW;
    }

    // Link tab: no child components to lay out — painted directly

    // CHAIN tab layout
    if (comingSoonTab)
    {
        int leftW   = juce::jmin(300, b.getWidth() / 3);
        int abOff3  = abBarShowing ? kAbBarH : 0;
        int contentH = b.getHeight() - topH - abOff3;

        // Rack strip occupies the bottom of the content area, full width
        static constexpr int kRackH = 72;
        int rackY = topH + contentH - kRackH;
        chainRackStrip.setBounds(0, rackY, b.getWidth(), kRackH);

        // Upper section height (excluding tab header and rack strip)
        int upperH = contentH - 32 - kRackH; // 32 = tab header
        int y = topH + 32 + 8;               // below header strip

        chainScanBtn.setBounds(10, y, 76, 24);
        chainStatusLabel.setBounds(94, y, leftW - 104, 24);
        y += 28;

        static constexpr int kDebugJsonH = 62;
        chainDebugJsonBox.setBounds(10, y, leftW - 20, kDebugJsonH);
        y += kDebugJsonH + 4;

        chainSearchBox.setBounds(10, y, leftW - 20, 22);
        y += 28;

        int listBottom = topH + 32 + upperH - 50;  // leaves room for Load btn + recommend label
        int listH = juce::jmax(40, listBottom - y);
        chainPluginList.setBounds(10, y, leftW - 20, listH);

        int btnY = topH + 32 + upperH - 42;
        chainLoadBtn.setBounds(10, btnY, 110, 24);
        chainRecommendLabel.setBounds(10, btnY + 26, leftW - 20, 14);

        // Editor holder: right panel, between header and rack strip
        int editorX = leftW + 1;
        int editorW = b.getWidth() - editorX;
        chainEditorHolder.setBounds(editorX, topH + 32, editorW, upperH);

        // Warning overlay: centered in the whole CHAIN area
        if (chainWarnOverlay.isVisible())
        {
            int ovW = juce::jmin(480, b.getWidth() - 40);
            int ovH = 130;
            int ovX = (b.getWidth() - ovW) / 2;
            int ovY = topH + (b.getHeight() - topH - ovH) / 2;
            chainWarnOverlay.setBounds(ovX, ovY, ovW, ovH);
            chainWarnLabel.setBounds(10, 10, ovW - 20, 60);
            chainWarnOkBtn.setBounds((ovW - 200) / 2, 80, 200, 32);
        }
    }

    // Position particle visual — use paint formula for mW to match divider
    int abOff = abBarShowing ? kAbBarH : 0;
    int paintChatW = juce::jlimit(280, 420, b.getWidth() * 35 / 100);
    int paintMW = b.getWidth() - paintChatW;
    // particleVisualHolder may only be shown on Visualisation or Meters tabs.
    const bool isVisualTab = (currentTab == Tab::Visualisation || currentTab == Tab::Meters);
    if (isVisualTab && visualMode && !compactMode && !visualOnlyMode && currentView == View::Meters)
    {
        int stripH = 28;  // number strip
        int selectorH = 30; // preset/theme selector strip
        particleVisualHolder.setBounds(0, topH, paintMW - 1, b.getHeight() - topH - stripH - selectorH - abOff);
        particleVisualHolder.setVisible(!channelPromptVisible && !genrePromptVisible
                                        && !updateOverlay.isVisible()
                                        && !reviewOverlay.visibleState);
    }
    else if (isVisualTab && visualOnlyMode)
    {
        if (visualMode) {
            int stripH = 28;
            int toggleH = 24;
            particleVisualHolder.setBounds(0, topH, b.getWidth() - 2, b.getHeight() - topH - stripH - toggleH - abOff);
            particleVisualHolder.setVisible(!channelPromptVisible && !genrePromptVisible
                                            && !updateOverlay.isVisible()
                                            && !reviewOverlay.visibleState);
        } else {
            // Meters mode in visual-only — hide particle visual
            particleVisualHolder.setVisible(false);
        }
    }
    else
    {
        particleVisualHolder.setVisible(false);
    }

    // === Single top bar row ===
    int ty = 4, bh = 24;
    int tx = 159;
    
    if (visualOnlyMode) {
        // In visual-only: capture button right after logo+badge
        captureBtn.setBounds(tx, ty, 64, bh);
        channelTypeBox.setBounds(0, -20, 1, 1);
        genreBox.setBounds(0, -20, 1, 1);
        projectInput.setBounds(0, -20, 1, 1);
    } else {
        channelTypeBox.setBounds(tx, ty, 100, bh); tx += 104;
        genreBox.setBounds(tx, ty, 95, bh); tx += 99;
        projectInput.setBounds(tx, ty, 90, bh); tx += 94;
        captureBtn.setBounds(tx, ty, 64, bh); tx += 68;
    }
    
    if (compactMode)
    {
        // Hide full-mode-only top bar buttons
        compareBtn.setBounds(0, -20, 1, 1);
        settingsBtn.setBounds(0, -20, 1, 1);
        scanBtn.setBounds(0, -20, 1, 1);
        abSyncBtn.setBounds(-100, -100, 1, 1);
    }
    else
    {
        compareBtn.setBounds(tx, ty, 64, bh); tx += 68;
        settingsBtn.setBounds(tx, ty, 52, bh); tx += 56;
        scanBtn.setBounds(tx, ty, 78, bh); tx += 82;
    }
    
    // Detected label — fixed position in top-right, right-aligned
    int detW = 140;
    detectedLabel.setBounds(b.getWidth() - detW - 8, ty, detW, bh);
    detectedLabel.setJustificationType(juce::Justification::centredRight);
    detectedLabel.setVisible(false);

    // Row 2 labels hidden — info lives elsewhere now
    statusLabel.setBounds(0, -20, 1, 1);
    durationLabel.setBounds(0, -20, 1, 1);
    passLabel.setBounds(0, -20, 1, 1);
    playbackBtn.setBounds(0, -20, 1, 1);
    wavSavedLabel.setBounds(0, -20, 1, 1);
    userLabel.setBounds(0, -20, 1, 1);
    // usageLabel positioning moved to the chat-header block below — it
    // now lives in the AI ASSISTANT strip next to the Aa button.

    // Sidebar ListBox + toolbar — Chat tab only, full height below tab bar.
    // Narrow chatW so all subsequent chat-area bounds use the reduced width.
    bool resSidebar = (currentTab == Tab::Chat && !compactMode && !visualOnlyMode);
    int sidebarOffsetX = resSidebar ? kSidebarW : 0;
    if (resSidebar) chatW -= kSidebarW;
    if (resSidebar)
    {
        int sbX = compactMode ? 0 : mW;
        // Toolbar: two equal-width buttons at top
        int halfW = kSidebarW / 2;
        sidebarNewChatBtn.setBounds(sbX,           topH, halfW,          kSidebarToolbarH);
        sidebarNewAlbumBtn.setBounds(sbX + halfW,  topH, kSidebarW - halfW, kSidebarToolbarH);
        sidebarNewChatBtn.setVisible(true);
        sidebarNewAlbumBtn.setVisible(true);
        // ListBox fills the rest
        chatSidebar.setBounds(sbX, topH + kSidebarToolbarH,
                              kSidebarW, b.getHeight() - topH - kSidebarToolbarH);
        chatSidebar.setVisible(true);
    }
    else
    {
        sidebarNewChatBtn.setVisible(false);
        sidebarNewAlbumBtn.setVisible(false);
        chatSidebar.setVisible(false);
    }

    // Chat input — 2-line height, Send centred vertically
    int inH = 52; // ~2 lines of 13px font
    int sendW = 56;
    int sendH = 30;
    int chatPadL = (compactMode || chatOnlyMode) ? 8 : -20; // compact/chat-only: normal padding; full: 20px past divider
    int chatStartX = (compactMode ? 0 : mW) + sidebarOffsetX;
    int abOff4 = abBarShowing ? kAbBarH : 0;
    int inputPad = compactMode ? 16 : 10;
    int inputY = b.getHeight() - inH - inputPad - abOff4;
    chatInput.setBounds(chatStartX + chatPadL, inputY, chatW - sendW - chatPadL - 4, inH);
    int sendY = inputY + (inH - sendH) / 2; // vertically centred
    chatSendBtn.setBounds(chatStartX + chatW - sendW - 2, sendY, sendW, sendH);

    // Aa text-size button — sits in the chat header strip. Header is at
    // top = topH, height = 32. The usage counter sits just to the LEFT
    // of the Aa button so users can see how many messages they have left
    // without leaving the chat view.
    {
        int aaW = 26, aaH = 22;
        int rightMargin = 8;
        int aaX = chatStartX + chatW - rightMargin - aaW;
        int aaY = topH + (32 - aaH) / 2;
        chatTextSizeBtn.setBounds(aaX, aaY, aaW, aaH);
        chatTextSizeBtn.setVisible(chatScroll.isVisible());

        // Counter label — right-aligned, ending 6px before the Aa button.
        int counterW = 130;
        int counterH = 16;
        int counterX = aaX - counterW - 6;
        int counterY = topH + (32 - counterH) / 2;
        usageLabel.setBounds(counterX, counterY, counterW, counterH);
        usageLabel.setJustificationType(juce::Justification::centredRight);
        usageLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::plain)));
        usageLabel.setColour(juce::Label::textColourId, C::text3);
        usageLabel.setVisible(chatScroll.isVisible());
    }
    
    // Chat scroll: top = below chat header, bottom = above chat input with gap.
    // The assistant avatar is painted by the editor at chatX+6 with width 24,
    // so the viewport must start AFTER the avatar (chatX + 30 + a small gap)
    // — otherwise macOS's scroll-blit optimisation blits the avatar's old
    // pixels along with the bubble area, causing the avatar's right edge to
    // tear during scroll. The user (U) avatar is on the right side and lives
    // outside the viewport's right edge by symmetry of chatW - 4.
    int chatAvatarReserve = 32; // 6px left margin + 24px avatar + 2px gap
    int chatScrollTop = topH + 32;
    int chatScrollBottom = inputY - 8;
    int chatScrollH = juce::jmax(50, chatScrollBottom - chatScrollTop);
    chatScroll.setBounds(chatStartX + chatAvatarReserve, chatScrollTop,
                         chatW - chatAvatarReserve - 2, chatScrollH);
    // Set chatContent width here, but DO NOT cap height to viewport — the timer
    // callback computes the real content height from the message list and sets it
    // there. Capping to viewport height was the bug that broke scrolling.
    int currentContentH = std::max(chatContent.getHeight(), chatScroll.getHeight());
    chatContent.setSize(chatW - chatAvatarReserve - 4, currentContentH);
    
    // In compact mode, ensure chat components are on top
    if (compactMode) {
        chatScroll.toFront(false);
        chatInput.toFront(false);
        chatSendBtn.toFront(false);
    }

    // Hide chat when channel prompt overlay is showing
    if (channelPromptVisible)
    {
        chatInput.setVisible(false);
        chatSendBtn.setVisible(false);
        chatTextSizeBtn.setVisible(false);
        chatScroll.setVisible(false);
    }
    
    // Hide chat in visual-only mode
    if (visualOnlyMode)
    {
        chatInput.setVisible(false);
        chatSendBtn.setVisible(false);
        chatTextSizeBtn.setVisible(false);
        chatScroll.setVisible(false);
    }

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
        compareSlotBBox.setBounds(cPad + cCardW + 12, cy2, cCardW - 4, 22);
        playSlotABtn.setVisible(false);
        playSlotBBtn.setVisible(false);
        
        // Click catcher covers the ENTIRE compare area for reliable drag-and-drop
        // Using actual bounds instead of hardcoded values
        int catcherTop = topH + 4;
        int catcherH = getHeight() - catcherTop - 10;
        compareClickCatcher.setBounds(0, catcherTop, mW, catcherH);
        compareClickCatcher.setVisible(true);
        compareClickCatcher.toFront(false);
        
        // Bring interactive elements in front of the catcher
        compareSlotABox.toFront(false);
        compareSlotBBox.toFront(false);
        aiCompareBtn.toFront(false);
        presetBox.toFront(false);
        savePresetBtn.toFront(false);
        deletePresetBtn.toFront(false);
        refStatusLabel.toFront(false);
        for (auto& b : refRemoveBtns) b.toFront(false);
        // Chat input overlaps the divider by 20px — keep it above the catcher
        chatInput.toFront(false);
        chatSendBtn.toFront(false);
        chatScroll.toFront(false);
        
        // AI Compare button centred at bottom
        int abOff2 = abBarShowing ? kAbBarH : 0;
        int btnY = getHeight() - 36 - abOff2;
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
        
        // EXPERIENCE LEVEL + CHAT LANGUAGE — on the same row, half-width each.
        // The 8px gap matches the inter-field vertical spacing used elsewhere,
        // so the row visually balances with the rest of the form.
        sy += labelGap;
        {
            int halfGap = 8;
            int halfW = (sw - halfGap) / 2;
            settingsExpLevel.setBounds(sx, sy, halfW, fh);
            settingsLanguage.setBounds(sx + halfW + halfGap, sy, halfW, fh);
        }
        sy += fh + 8;
        
        // MONITORS
        sy += labelGap;
        settingsMonitors.setBounds(sx, sy, sw, fh); sy += fh + 8;
        
        // HEADPHONES
        sy += labelGap;
        settingsHeadphones.setBounds(sx, sy, sw, fh); sy += fh + 8;
        
        // GENRES
        sy += labelGap;
        settingsGenres.setBounds(sx, sy, sw, fh); sy += fh + 8;
        
        // PLUGINS — a scan button (same behaviour as the header one) with
        // "View all" beside it. (Help & Support lives down on the Save/Log Out
        // row, since it's an app action, not a plugins control.)
        sy += labelGap;

        int abOff3 = abBarShowing ? kAbBarH : 0;
        int saveRowY = b.getHeight() - 44 - abOff3;

        int viewAllW = 90, rowGap = 8;
        settingsScanBtn.setBounds(sx, sy, sw - viewAllW - rowGap, fh);
        viewAllPluginsBtn.setBounds(sx + sw - viewAllW, sy, viewAllW, fh);
        sy += fh + 8;
        
        // Save + Logout row — always pinned to bottom
        saveSettingsBtn.setBounds(sx, saveRowY, 100, 30);
        settingsSavedLabel.setBounds(sx + 110, saveRowY, 150, 30);
        logoutBtn.setBounds(sx + sw - 80, saveRowY, 80, 30);
        // Help & Support sits just left of Log Out — an app-level link, kept
        // visually distinct from the plugins controls above.
        settingsHelpBtn.setBounds(sx + sw - 80 - 8 - 120, saveRowY, 120, 30);
        logoutBtn.setVisible(true);
    }

    auto card = b.reduced(40, 34);
    channelPromptTitle.setBounds(card.getX() + 40, card.getY() + 26, card.getWidth() - 80, 30);
    channelPromptSubtitle.setBounds(card.getX() + 60, card.getY() + 58, card.getWidth() - 120, 22);

    auto gridArea = card.reduced(32, 88);
    gridArea.removeFromBottom(42);
    const int columns = 4;
    const int columnGap = 12;
    const int rowGap = 22;
    const int groupW = (gridArea.getWidth() - columnGap * (columns - 1)) / columns;
    std::array<int, kChannelPromptGroupCount> groupOptionCounts {};
    for (const auto& option : kChannelPromptOptions)
        ++groupOptionCounts[(size_t)option.groupIndex];

    const int topRowMaxCount = juce::jmax(
        juce::jmax(groupOptionCounts[0], groupOptionCounts[1]),
        juce::jmax(groupOptionCounts[2], groupOptionCounts[3]));
    const int bottomRowMaxCount = juce::jmax(
        juce::jmax(groupOptionCounts[4], groupOptionCounts[5]),
        juce::jmax(groupOptionCounts[6], groupOptionCounts[7]));

    auto computeRowHeight = [](int optionCount) {
        const int headingHeight = 18;
        const int headingGap = 6;
        const int buttonGap = 4;
        const int buttonHeight = 18;
        return headingHeight + headingGap + optionCount * buttonHeight + juce::jmax(0, optionCount - 1) * buttonGap;
    };

    const int topRowHeight = computeRowHeight(topRowMaxCount);
    const int bottomRowHeight = computeRowHeight(bottomRowMaxCount);
    const int totalRowsHeight = topRowHeight + bottomRowHeight + rowGap;
    const int gridTop = gridArea.getY() + juce::jmax(0, (gridArea.getHeight() - totalRowsHeight) / 2);
    const int rowTops[2] = { gridTop, gridTop + topRowHeight + rowGap };
    const int rowHeights[2] = { topRowHeight, bottomRowHeight };

    for (int i = 0; i < kChannelPromptGroupCount; ++i)
    {
        const int col = i % columns;
        const int row = i / columns;
        auto groupBounds = juce::Rectangle<int>(
            gridArea.getX() + col * (groupW + columnGap),
            rowTops[row],
            groupW,
            rowHeights[row]);

        channelPromptGroupLabels[(size_t)i].setBounds(groupBounds.getX(), groupBounds.getY(), groupBounds.getWidth(), 18);

        auto buttonsArea = groupBounds.withTrimmedTop(24);
        const int buttonGap = 4;
        const int buttonHeight = 18;

        int localIndex = 0;
        for (int optionIndex = 0; optionIndex < kChannelPromptOptionCount; ++optionIndex)
        {
            if (kChannelPromptOptions[optionIndex].groupIndex != i)
                continue;

            channelPromptButtons[(size_t)optionIndex].setBounds(
                buttonsArea.getX(),
                buttonsArea.getY() + localIndex * (buttonHeight + buttonGap),
                buttonsArea.getWidth(),
                buttonHeight);
            ++localIndex;
        }
    }

    channelPromptSkipBtn.setBounds(card.getCentreX() - 115, card.getBottom() - 40, 110, 28);
    customChannelBtn.setBounds(card.getCentreX() + 5, card.getBottom() - 40, 110, 28);

    // Genre prompt layout — 4 columns with group headers (like channel prompt)
    {
        auto genreCard = b.reduced(40, 34);
        genrePromptTitle.setBounds(genreCard.getX() + 40, genreCard.getY() + 26, genreCard.getWidth() - 80, 28);
        genrePromptSubtitle.setBounds(genreCard.getX() + 60, genreCard.getY() + 56, genreCard.getWidth() - 120, 20);

        auto gridArea2 = genreCard.reduced(32, 88);
        gridArea2.removeFromBottom(42);
        const int columns = 4;
        const int columnGap = 12;
        const int rowGap = 22;
        const int groupW = (gridArea2.getWidth() - columnGap * (columns - 1)) / columns;

        // Count options per group
        std::array<int, kGenreGroupCount> groupOptionCounts {};
        for (int i = 0; i < kGenreOptionCount; ++i)
            ++groupOptionCounts[(size_t)kGenrePromptOptions[i].groupIndex];

        // Compute row heights
        auto computeGenreRowHeight = [](int optionCount) {
            const int headingH = 18, headingGap = 6, btnGap = 4, btnH = 18;
            return headingH + headingGap + optionCount * btnH + juce::jmax(0, optionCount - 1) * btnGap;
        };

        // All 4 groups in a single row
        int maxHeight = 0;
        for (int i = 0; i < kGenreGroupCount; ++i)
            maxHeight = juce::jmax(maxHeight, computeGenreRowHeight(groupOptionCounts[(size_t)i]));

        const int gridTop = gridArea2.getY() + juce::jmax(0, (gridArea2.getHeight() - maxHeight) / 2);

        for (int i = 0; i < kGenreGroupCount; ++i)
        {
            auto groupBounds = juce::Rectangle<int>(
                gridArea2.getX() + i * (groupW + columnGap),
                gridTop, groupW, maxHeight);

            genrePromptGroupLabels[(size_t)i].setBounds(groupBounds.getX(), groupBounds.getY(), groupBounds.getWidth(), 18);

            auto buttonsArea = groupBounds.withTrimmedTop(24);
            const int btnGap = 4, btnH = 18;
            int localIdx = 0;
            for (int optIdx = 0; optIdx < kGenreOptionCount; ++optIdx)
            {
                if (kGenrePromptOptions[optIdx].groupIndex != i) continue;
                genrePromptButtons[(size_t)optIdx].setBounds(
                    buttonsArea.getX(),
                    buttonsArea.getY() + localIdx * (btnH + btnGap),
                    buttonsArea.getWidth(), btnH);
                ++localIdx;
            }
        }

        genrePromptCustomBtn.setBounds(genreCard.getCentreX() - 55, genreCard.getBottom() - 60, 110, 28);
    }
}

// ============================================================================
// Timer
// ============================================================================

void EchoJayEditor::timerCallback()
{
    // Loading screen timeout — if network calls take too long, show main anyway
    if (currentScreen == Screen::Loading)
    {
        refreshCounter++;
        if (refreshCounter > 100) // 5 seconds at 20fps
        {
            refreshCounter = 0;
            showMainScreen();
        }
        // Hide any overlay components that might show through
        for (int i = 0; i < kMaxWavePlayBtns; ++i)
            wavePlayOverlays[(size_t)i].setVisible(false);
        upgradeBtn.setVisible(false);
        playbackBtn.setVisible(false);
        repaint(); // animate the dots
        return; // don't process anything else while loading
    }

    // Enforce chat hidden while channel prompt overlay is showing
    if (channelPromptVisible)
    {
        chatInput.setVisible(false);
        chatSendBtn.setVisible(false);
        chatTextSizeBtn.setVisible(false);
        chatScroll.setVisible(false);
        upgradeBtn.setVisible(false);
    }
    
    // Safety: re-enable buttons if no prompts are active
    // (handles edge cases where dismiss didn't re-enable properly)
    if (!channelPromptVisible && !genrePromptVisible)
    {
        if (!captureBtn.isEnabled())  captureBtn.setEnabled(true);
        if (!compareBtn.isEnabled())  compareBtn.setEnabled(true);
        if (!settingsBtn.isEnabled()) settingsBtn.setEnabled(true);
    }

    // Update CHAIN tab scan status
    if (currentTab == Tab::Chain)
    {
        auto& ch = processorRef.getChainHost();
        bool wasScanning = ch.isScanning();
        if (wasScanning)
        {
            int pct = (int)(ch.getScanProgress() * 100.0f);
            chainStatusLabel.setText("Reading plugins... " + juce::String(pct) + "%",
                                     juce::dontSendNotification);
            chainListModel->items = ch.getFilteredPlugins(chainSearchBox.getText(), chainFormatFilter_);
            chainPluginList.updateContent();
        }
        else
        {
            // Rebuild resolver once after scan completes (entries_ has stabilised)
            // We detect completion by checking if the recommendable count is still 0
            // while the entries list is non-empty.  A proper "scan just finished" flag
            // isn't exposed, so we rebuild whenever the numbers look stale.
            bool entriesReady = ch.getNumPlugins() > 0;
            bool resolverStale = (ch.getEnabledInputCount() == 0 && entriesReady);
            if (resolverStale)
            {
                ch.buildRecommendable(processorRef.getPluginScanner().getPlugins(), chainFormatFilter_);
                chainRecommendLabel.setText(
                    "recommendable: " + juce::String(ch.getRecommendableCount())
                    + " resolved (" + juce::String(ch.getEnabledInputCount()) + " enabled, "
                    + juce::String(ch.getUnmatchedCount()) + " unmatched)",
                    juce::dontSendNotification);
            }

            if (ch.getNumSlots() > 0)
                chainStatusLabel.setText(juce::String(ch.getNumSlots()) + " slot(s) in chain",
                                         juce::dontSendNotification);
            else if (entriesReady)
                chainStatusLabel.setText(juce::String(ch.getNumPlugins()) + " plugins available",
                                         juce::dontSendNotification);
        }
    }

    auto state = processorRef.getCaptureState();

    // Auto-stop plugin-routed playback when WAV reaches end
    if (processorRef.abActive.load() && processorRef.abPlayingRef.load() && processorRef.abSampleCount > 0)
    {
        if (processorRef.abPlaybackPos >= processorRef.abSampleCount - 1)
        {
            processorRef.stopAB();
            currentlyPlayingChatWav.clear();
            chatPlaybackStartTime = 0;
            chatPlaybackDuration = 0;
            chatPlaybackOffset = 0;
            playSlotABtn.setButtonText(">");
            playSlotBBtn.setButtonText(">");
        }
    }
    
    // Resize window when AB bar appears/disappears
    bool shouldShowAbBar = processorRef.abActive.load() && !compactMode && !visualOnlyMode;
    if (shouldShowAbBar && !abBarShowing) {
        abBarShowing = true;
        setSize(getWidth(), getHeight() + kAbBarH);
    } else if (!shouldShowAbBar && abBarShowing) {
        abBarShowing = false;
        setSize(getWidth(), getHeight() - kAbBarH);
    }
    
    // ALWAYS bring header buttons to front so overlays can't block them
    captureBtn.toFront(false);
    compareBtn.toFront(false);
    settingsBtn.toFront(false);
    scanBtn.toFront(false);
    channelTypeBox.toFront(false);
    genreBox.toFront(false);
    
    // Spectrum A/B hold — capture spectrum snapshot when switching between ref and DAW
    bool isPlayingRef = processorRef.abPlayingRef.load();
    if (isPlayingRef != wasPlayingRef && processorRef.abActive.load()) {
        // State changed — freeze the peak hold spectrum (the full envelope, not a single frame)
        heldSpectrum = spectrumPeakHold;
        heldSpectrumValid = true;
        heldSpectrumAlpha = 1.0f;
        // Capture the dB range so held spectrum doesn't shift with new signal
        float hPeak = -200.0f;
        for (int i = 0; i < 64; ++i)
            if (heldSpectrum[(size_t)i] > hPeak) hPeak = heldSpectrum[(size_t)i];
        heldDbMax = std::max(-20.0f, hPeak + 3.0f);
        heldDbMin = heldDbMax - 66.0f;
        // Reset peak hold so it starts fresh for the new source
        spectrumPeakHoldInit = false;
    }
    wasPlayingRef = isPlayingRef;
    // Always store current spectrum for next frame's potential hold
    {
        auto mdNow = processorRef.getMeterEngine().getMeterData();
        prevFrameSpectrum = mdNow.spectrum;
    }
    // Smooth fade out held spectrum over ~3 seconds
    if (heldSpectrumValid) {
        heldSpectrumAlpha -= 0.005f;
        if (heldSpectrumAlpha <= 0.0f) {
            heldSpectrumAlpha = 0.0f;
            heldSpectrumValid = false;
        }
    }

    // Feed meter data to particle visual
    if (particleVisual && particleVisual->isVisible())
    {
        auto md = processorRef.getMeterEngine().getMeterData();
        particleVisual->updateMeterData(md);
    }
    
    // Capture animation — detect state transitions for visual
    {
        bool isCapturing = (state == CaptureState::Capturing);
        if (isCapturing && !wasCapturing && particleVisual) {
            particleVisual->triggerCaptureImplode();
            captureAnimTriggered = true;
        }
        if (!isCapturing && wasCapturing && captureAnimTriggered && particleVisual) {
            particleVisual->triggerCaptureRelease();
            captureAnimTriggered = false;
        }
        wasCapturing = isCapturing;
    }

    if (state == CaptureState::Capturing) {
        captureBtn.setButtonText("Stop");
        captureBtn.setColour(juce::TextButton::buttonColourId, C::red);
        float dur = processorRef.getCaptureDuration();
        durationLabel.setText(juce::String::formatted("%d:%02d", (int)dur / 60, (int)dur % 60), juce::dontSendNotification);
        statusLabel.setText("Capturing...", juce::dontSendNotification);
        waveformFrozen = false; // live while capturing
    } else {
        captureBtn.setButtonText("Capture");
        captureBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        statusLabel.setText(state == CaptureState::Complete ? "Complete" : "", juce::dontSendNotification);

        // Freeze waveform on capture complete (once — only if we haven't already unfrozen)
        if (state == CaptureState::Complete && !waveformFrozen && unfreezeCountdown != -1)
        {
            frozenWaveform = processorRef.getWaveformRecorder().getThumbnail();
            waveformFrozen = true;
            captureWasSilent = false;
            unfreezeCountdown = 60;   // ~2 seconds at 30fps — then meters go back to live
        }

        // Tick down the unfreeze timer
        if (state == CaptureState::Complete && waveformFrozen && unfreezeCountdown > 0)
        {
            unfreezeCountdown--;
            if (unfreezeCountdown == 0)
            {
                // Timer expired — unfreeze meters back to live
                waveformFrozen = false;
                unfreezeCountdown = -1; // mark as "already unfrozen" so we don't re-freeze
            }
        }

        // Track if audio has gone silent since capture completed
        if (state == CaptureState::Complete && processorRef.isAudioSilent())
            captureWasSilent = true;

        // Full reset after audio went silent AND then resumed
        if (state == CaptureState::Complete && captureWasSilent && !processorRef.isAudioSilent())
        {
            processorRef.resetCapture();
            waveformFrozen = false;
            frozenWaveform.clear();
            wavSavedLabel.setText("", juce::dontSendNotification);
            captureWasSilent = false;
            unfreezeCountdown = 0;
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
        settingsScanBtn.setButtonText("Scanning " + juce::String((int)(sc.getProgress() * 100)) + "%");
        settingsScanBtn.setEnabled(false);
        wasScanning = true;
    } else {
        int c = sc.getPluginCount();
        bool scanJustFinished = wasScanning;
        wasScanning = false;
        if (c > 0 && (c != scannedPluginCount || scanJustFinished)) {
            scannedPluginCount = c;
            api.updatePluginsFromScanner(sc.getFullPluginList());
            if (currentView == View::Settings && settingsChecklist)
                settingsChecklist->refresh();
            // Only save to server if we've already fetched settings,
            // otherwise we'd overwrite name/monitors/etc with empty values
            if (settingsFetched)
                api.saveUserSettings(api.getUserSettings(), nullptr);

            // Pop the review checklist after a scan completes so the user can
            // untick anything they don't own (chiefly unlicensed Waves
            // plugins surfaced from a WaveShell). Only on a genuine scan
            // finish, not on every cache-count tick.
            if (scanJustFinished)
            {
                if (reviewChecklist) reviewChecklist->refresh();
                if (settingsChecklist) settingsChecklist->refresh();
                showPluginReview();
            }
        }
        scanBtn.setButtonText(c > 0 ? juce::String(c) + " Plugins" : "Scan Plugins");
        scanBtn.setEnabled(true);
        settingsScanBtn.setButtonText(c > 0 ? juce::String(c) + " Plugins" : "Scan Plugins");
        settingsScanBtn.setEnabled(true);
    }

    // Auto-feedback: trigger AI review after capture completes
    // Deferred until WAV save finishes (runs on background thread)
    if (processorRef.shouldAutoFeedback())
        pendingAutoFeedback = true;
    
    if (pendingAutoFeedback) {
        auto snap = processorRef.getLatestSnapshot();
        auto savedPath = processorRef.getWaveformRecorder().getLastSavedPath();
        if (snap.durationSeconds >= 0.1f && savedPath.isNotEmpty())
        {
            pendingAutoFeedback = false;

            // ── 1. Ensure a current chat ────────────────────────────────────
            // Use whatever is already open; only create one if there is none.
            if (currentChatId.isEmpty())
            {
                juce::String proj = processorRef.getProjectName().trim();
                WsChat nc;
                nc.id           = juce::String(juce::Time::currentTimeMillis());
                nc.title        = proj.isEmpty() ? juce::Time::getCurrentTime().formatted("%d %b %Y, %H:%M") : proj;
                nc.trackName    = proj;
                nc.created      = juce::Time::getCurrentTime().toISO8601(true);
                nc.albumId      = getCurrentAlbumId();
                nc.revisionCount = 0;
                workspace.addChat(nc);
                currentChatId = nc.id;
                chatMessages.clear();
                processorRef.chatHistory.clear();
                processorRef.chatRoles.clear();
                processorRef.chatContents.clear();
                if (sidebarModel)
                {
                    sidebarModel->refreshRows(workspace.getChats(), workspace.getAlbums(),
                                              workspace.getReviews(), collapsedAlbums, currentChatId);
                    chatSidebar.updateContent();
                }
            }

            // ── 2. Compute version and passName ────────────────────────────
            int version = 1;
            const WsReview* prevReview = nullptr;
            for (auto& chat : workspace.getChats())
            {
                if (chat.id != currentChatId) continue;
                version = chat.revisionCount + 1;
                // Find the previous capture review in this chat (last _reviewId)
                for (int mi = (int)chat.messages.size() - 1; mi >= 0; --mi)
                {
                    if (chat.messages[(size_t)mi].reviewId.isNotEmpty())
                    {
                        juce::String prevId = chat.messages[(size_t)mi].reviewId;
                        for (auto& rv : workspace.getReviews())
                            if (rv.id == prevId) { prevReview = &rv; break; }
                        break;
                    }
                }
                break;
            }
            juce::String proj = processorRef.getProjectName().trim();
            juce::String passName = proj.isEmpty()
                ? ("Capture v" + juce::String(version))
                : (proj + " v" + juce::String(version));

            // ── 3. Create review (uses passName for fileName) ──────────────
            // Override snap.name so the WAV filename uses passName
            snap.name = passName;
            juce::String reviewId = createReviewFromCapture(snap, savedPath);

            // Push PERSISTED user message — content = passName (survives save/load
            // cleanly), _reviewId links it to the review for waveform reconstruction.
            workspace.appendMessageToChat(currentChatId, "user", passName, reviewId);
            // Persist immediately so a background sync cannot clobber this message.
            workspace.requestMutationSync();

            // Local display
            ChatMsg displayCm;
            displayCm.role     = "user";
            displayCm.content  = passName;
            displayCm.reviewId = reviewId;
            displayCm.origin   = "plugin";
            displayCm.hasWaveform = !frozenWaveform.empty();
            displayCm.waveform    = frozenWaveform;
            displayCm.durationSeconds = snap.durationSeconds;
            displayCm.lufs    = snap.averagedData.integrated;
            if (savedPath.isNotEmpty())
            {
                displayCm.wavFilename = juce::File(savedPath).getFileName();
                displayCm.wavFilePath = savedPath;
            }
            chatMessages.push_back(displayCm);
            {
                EchoJayProcessor::ChatEntry ce;
                ce.role    = "user";
                ce.content = passName;  // passName is the display label for AI context
                ce.hasWaveform = displayCm.hasWaveform;
                ce.durationSeconds = displayCm.durationSeconds;
                ce.lufs    = displayCm.lufs;
                ce.wavFilename = displayCm.wavFilename;
                ce.wavFilePath = displayCm.wavFilePath;
                if (displayCm.hasWaveform)
                    for (auto& pt : displayCm.waveform)
                        ce.waveform.push_back(std::max(std::abs(pt.maxVal), std::abs(pt.minVal)));
                processorRef.chatHistory.push_back(std::move(ce));
            }

            // ── 4. Fire AI feedback ────────────────────────────────────────
            requestAIFeedback(snap, currentChatId, reviewId, passName, version, prevReview);
        
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
                if (prevSelA > 0) compareSlotABox.setSelectedId(prevSelA, juce::dontSendNotification);
                else if (snaps2.size() > 0) compareSlotABox.setSelectedId((int)snaps2.size(), juce::dontSendNotification);
                if (prevSelB > 0) compareSlotBBox.setSelectedId(prevSelB, juce::dontSendNotification);
            }
        }
    }

    if (currentScreen == Screen::Main && api.isLoggedIn()) {
        auto info = api.getUserInfo();
        int remaining = api.getRemainingMessages();
        int used = info.messageLimit - remaining;
        juce::String usageStr = juce::String(used) + "/" + juce::String(info.messageLimit);
        if (info.credits > 0)
            usageStr += " (+" + juce::String(info.credits) + " credits)";
        usageLabel.setText(usageStr, juce::dontSendNotification);
        
        // Show upgrade button when free/pro user is out of messages (and credits)
        bool showUpgrade = info.tierLevel < 2 && !api.canSendMessage() && !channelPromptVisible && !genrePromptVisible;
        upgradeBtn.setVisible(showUpgrade);
        if (showUpgrade)
        {
            upgradeBtn.setButtonText(info.tierLevel == 0 ? "Upgrade to Pro" : "Upgrade to Studio");
            // Centre a compact button in the chat input area
            auto cb = chatInput.getBounds();
            int btnW = 140;
            int btnH = 30;
            int btnX = cb.getX() + (cb.getWidth() - btnW) / 2 + 20;
            int btnY = cb.getY() + (cb.getHeight() - btnH) / 2;
            upgradeBtn.setBounds(btnX, btnY, btnW, btnH);
            chatInput.setVisible(false);
            chatSendBtn.setVisible(false);
            chatTextSizeBtn.setVisible(false);
        }
        
        // Colour the usage label red when out of messages
        usageLabel.setColour(juce::Label::textColourId, !api.canSendMessage() ? C::red : C::text3);
        
        // Periodic refresh every 5 minutes to sync usage/subscription.
        // Visibility-change and Settings-open refreshes (see elsewhere)
        // cover the common upgrade-detection cases. The periodic timer
        // is a safety net for the user who keeps the plugin open and
        // visible for long stretches without interacting with Settings.
        refreshCounter++;
        if (refreshCounter >= 6000) // 20fps * 300s
        {
            refreshCounter = 0;
            api.refreshUserInfo(nullptr);
        }
        
        // If displayName is still empty, try once on first refresh cycle
        if (info.displayName.isEmpty() && refreshCounter == 100)
        {
            api.refreshUserInfo(nullptr);
        }
    }

    if (currentScreen == Screen::Main && !api.isLoggedIn())
        showLoginScreen();

    // Update chat scroll content size and auto-scroll to bottom
    if (currentScreen == Screen::Main && !chatMessages.empty())
    {
        // Mirror the paint code's geometry exactly so the scroll range matches the
        // actual rendered height. Paint uses chatW (panel width), avatar 24, and
        // -24 for bubble margin; the scroll viewport now starts at chatX + 32
        // (avatar reserve) and is chatW - 34 wide — see resized() for details.
        int chatAvatarReserve = 32;
        int chatW2 = chatScroll.getWidth() + chatAvatarReserve + 2; // recover chat panel width
        int avatarSz = 24;
        int maxBW = chatW2 - avatarSz - 24; // matches paint maxBubbleW
        int totalH = 8; // matches paint msgY initial value
        const float chatMsgFontSize2 = 12.0f * chatTextScale;
        for (auto& msg : chatMessages) {
            const bool isCaptureMsg2 = (msg.role == "user") && msg.reviewId.isNotEmpty();
            int tH;
            if (isCaptureMsg2)
            {
                tH = 56; // unified capture card — must match kCaptureMsgH in paint
            }
            else
            {
                juce::AttributedString as;
                as.append(msg.content, juce::Font(juce::FontOptions(chatMsgFontSize2)), C::text);
                juce::TextLayout layout;
                layout.createLayout(as, (float)(maxBW - 20));
                int textH = (int)layout.getHeight() + 20;
                int waveCardH = (msg.hasWaveform && !msg.waveform.empty()) ? 36 : 0;
                int chainAreaH2 = (msg.role == "assistant" && msg.chainData.isNotEmpty()) ? 32 : 0;
                tH = textH + waveCardH + chainAreaH2;
            }
            totalH += tH + 10; // matches paint msgY += tH + 10
        }
        if (chatLoading) totalH += 30;
        totalH += 8; // bottom padding so last message isn't flush against the edge
        
        int visH = chatScroll.getHeight();
        if (chatContent.getHeight() != std::max(visH, totalH))
        {
            chatContent.setSize(chatScroll.getWidth() - 4, std::max(visH, totalH));
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
    // FIRST: hide all overlays if login screen, update prompt, or channel/genre prompt is showing
    bool hideOverlays = (currentScreen != Screen::Main)
                     || (updateAvailable && !updateDismissed)
                     || channelPromptVisible
                     || genrePromptVisible;
    
    if (hideOverlays)
    {
        for (int i = 0; i < kMaxWavePlayBtns; ++i)
        {
            wavePlayOverlays[(size_t)i].setVisible(false);
            wavePlayOverlays[(size_t)i].setBounds(-100, -100, 1, 1);
        }
        activeWavePlayBtns = 0;
        upgradeBtn.setVisible(false);
        playbackBtn.setVisible(false);
    }
    else if (currentView == View::Compare && !compareWavePositions.empty())
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
    else if (currentView == View::Meters)
    {
        // On Meters view — bring any existing chat overlays to front
        // (chat paint positions them, but they may be hidden behind other components)
        for (int i = 0; i < kMaxWavePlayBtns; ++i)
        {
            if (wavePlayOverlays[(size_t)i].isVisible())
                wavePlayOverlays[(size_t)i].toFront(false);
        }
    }
    else if (currentView != View::Meters)
    {
        for (int i = 0; i < kMaxWavePlayBtns; ++i)
            wavePlayOverlays[(size_t)i].setVisible(false);
    }

    // Periodic update check — every ~6 hours, re-fetch remote config
    updateCheckCounter++;
    if (updateCheckCounter >= kUpdateCheckInterval)
    {
        updateCheckCounter = 0;
        EchoJayAPI::remoteConfigLoaded = false;
        api.fetchRemoteConfig();
    }
    
    // Check if an update is available. updateDismissed is the in-session flag
    // (set on click). isUpdateDismissalActive reads the persisted-to-disk flag
    // which suppresses re-prompts for 3 days unless a newer version appears.
    // Don't show update overlay while channel/genre prompts are active — those
    // are first-time setup and take priority.
    if (!updateDismissed && EchoJayAPI::latestVersion.isNotEmpty()
        && !channelPromptVisible && !genrePromptVisible
        && currentScreen == Screen::Main)
    {
        auto current = juce::String(ProjectInfo::versionString);
        bool versionDiffers = isVersionNewer(EchoJayAPI::latestVersion, current);
        bool recentlyDismissed = isUpdateDismissalActive(EchoJayAPI::latestVersion);
        bool wasAvailable = updateAvailable;
        updateAvailable = versionDiffers && !recentlyDismissed;
        if (updateAvailable && !wasAvailable)
        {
            // Edge transition: configure the overlay child for first-show.
            updateOverlay.latestVersionStr = EchoJayAPI::latestVersion;
            updateOverlay.currentVersionStr = current;
            updateOverlay.setBounds(getLocalBounds());
            updateOverlay.setVisible(true);
            resized();  // re-run particleVisual visibility check
        }
    }
    else if (channelPromptVisible || genrePromptVisible || currentScreen != Screen::Main)
    {
        // Hide overlay while higher-priority UI is showing
        if (updateOverlay.isVisible())
        {
            updateOverlay.setVisible(false);
            resized();
        }
    }
    
    // Keep updateOverlay on top while visible — same pattern as wavePlayOverlays.
    // toFront() must run every tick because other components (esp. particleVisual)
    // can otherwise end up above it on z-order resets.
    if (updateOverlay.isVisible())
    {
        updateOverlay.setBounds(getLocalBounds());
        updateOverlay.toFront(false);
    }

    // -------------------------------------------------------------------------
    //  Link registry refresh every 10 ticks (~500 ms at 20 fps)
    // -------------------------------------------------------------------------
    linkRefreshTick++;
    if (linkRefreshTick >= 10)
    {
        linkRefreshTick = 0;
        processorRef.refreshLinkRegistry();
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
    // Ensure we have an active workspace chat to write into
    if (currentChatId.isEmpty())
    {
        WsChat c;
        c.id      = juce::String(juce::Time::currentTimeMillis());
        c.title   = "New chat";
        c.created = juce::Time::getCurrentTime().toISO8601(true);
        workspace.addChat(c);
        currentChatId = c.id;
    }

    // Push user turn to display list
    chatMessages.push_back({"user", msg});
    processorRef.chatHistory.push_back({"user", msg});

    // Push to workspace (single source of truth)
    bool isFirstMessage = workspace.appendMessageToChat(currentChatId, "user", msg);
    if (isFirstMessage)
    {
        // Title priority: project name (set on chat creation) > first user message >
        // created date. Capture chats have trackName = passName so they keep it.
        // Manual "New chat" entries have no trackName — title them from the message.
        const auto& chats = workspace.getChats();
        auto chatIt = std::find_if(chats.begin(), chats.end(),
            [this](const WsChat& c) { return c.id == currentChatId; });
        if (chatIt != chats.end() && chatIt->trackName.isEmpty())
        {
            // Use first message text (trimmed to 40 chars) as the title
            juce::String newTitle = msg.trim().substring(0, 40);
            if (newTitle.isEmpty())
            {
                // Fallback: created date/time
                if (chatIt->created.isNotEmpty())
                    newTitle = juce::Time::fromISO8601(chatIt->created).formatted("%d %b, %H:%M");
            }
            if (newTitle.isNotEmpty())
                workspace.setChatTitle(currentChatId, newTitle);
        }
    }

    // Refresh sidebar — chat now appears if it was previously empty
    if (sidebarModel)
    {
        sidebarModel->refreshRows(workspace.getChats(), workspace.getAlbums(),
                                  workspace.getReviews(), collapsedAlbums, currentChatId);
        chatSidebar.updateContent();
    }

    chatInput.clear();
    chatLoading = true;
    repaint();

    // Attach a live meter snapshot to every chat message. We tag it
    // [LIVE METER] (not [METER] or [CAPTURE]) so Claude knows this is
    // continuously-running data with varying precision per metric:
    //   - momentary (400ms window) and short-term (3s window) — real
    //     and immediate
    //   - integrated LUFS — averaged from whenever the integrator was
    //     last reset, could be very recent or stale
    //   - rms / peak / crest / width / correlation — current state
    // The system prompt's [LIVE METER] handling tells Claude to use
    // the data, qualify once that Capture gives averaged-over-window
    // precision, then move on. If the user's question is general
    // (not about the mix), the QUESTION ROUTING block tells Claude
    // to ignore the meter data and answer normally.
    auto md = processorRef.getMeterEngine().getMeterData();
    auto ff = [](float v) -> juce::String { return v > -99 ? juce::String(v, 1) : "N/A"; };
    juce::String ctx;
    if (md.isSilent) {
        // No audio playing through the plugin right now. Last meter
        // values would be stale ghosts from the previous playback
        // window; instead, tell the model there's no signal so it
        // continues the conversation about earlier captures (or the
        // user's general question) rather than analysing dead numbers.
        ctx = "\n\n[LIVE METER: " + processorRef.getEffectiveChannelName()
            + " (" + processorRef.getGenre() + ")] NO SIGNAL (playback stopped or no audio routed to plugin)";
    } else {
        ctx = "\n\n[LIVE METER: " + processorRef.getEffectiveChannelName() + " (" + processorRef.getGenre() + ")] " +
            "Int " + ff(md.integrated) + " LUFS | Mom " + ff(md.momentary) + " | ST " + ff(md.shortTerm) +
            " | LRA " + juce::String(md.loudnessRange, 1) + " LU | RMS " + ff(md.rmsL) + "/" + ff(md.rmsR) +
            " | TP " + ff(md.truePeakL) + "/" + ff(md.truePeakR) + " | Crest " + juce::String(md.crestFactor, 1) +
            " | Width " + juce::String(md.width, 1) + "% | Corr " + juce::String(md.correlation, 2) +
            " | S/M " + juce::String(md.sideToMidRatio, 2) +
            " | Corr-sub " + juce::String(md.corrSub, 2) +
            " | Corr-mid " + juce::String(md.corrMid, 2) +
            " | Corr-top " + juce::String(md.corrTop, 2);
    }
    juce::String userContent = msg + ctx;

    // Plugin/chain injection: always include chain instructions (with full plugin list)
    // when the user has recommendable plugins resolved — the model decides whether to
    // return a chain block. Fall back to plain plugin list only when no plugins are
    // resolved yet (e.g. scanner hasn't run).
    auto& chainHost = processorRef.getChainHost();
    juce::StringArray recommendable = chainHost.getRecommendableNames();

    juce::String chainInjection;
    if (recommendable.size() > 0)
    {
        chainInjection = EchoJayAPI::buildChainInjection(recommendable);
        userContent += chainInjection;
    }
    else if (EchoJayAPI::messageNeedsPlugins(msg))
    {
        userContent += EchoJayAPI::buildPluginInjection(
            processorRef.getPluginScanner().getFullPluginList());
    }

    processorRef.chatRoles.add("user");
    processorRef.chatContents.add(userContent);

    auto sysPrompt = EchoJayAPI::buildSystemPrompt(
        processorRef.getEffectiveChannelName(), processorRef.getGenre(),
        processorRef.getPluginScanner().getPluginSummary());

    juce::String activeChatId = currentChatId; // capture before async
    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);
    api.sendChat(processorRef.chatRoles, processorRef.chatContents, sysPrompt,
        [safeThis, activeChatId](const juce::String& reply, bool success) {
            if (safeThis == nullptr)
                return;
            safeThis->chatLoading = false;

            juce::String visibleReply = reply;
            juce::String chainJson;
            // Always try to extract a chain block; the model may or may not have included one.
            if (success)
                EchoJayAPI::extractChainBlock(visibleReply, chainJson);

            // If extractChainBlock returned partial/truncated JSON, try bracket-depth salvage
            // before falling through to the name-scan fallback.
            if (!chainJson.isEmpty() && success)
            {
                auto parsed = juce::JSON::parse(chainJson);
                if (!parsed.isObject())
                {
                    juce::String salvaged = EchoJayAPI::salvagePartialChain(chainJson);
                    if (salvaged.isNotEmpty())
                        DBG("EchoJay chain salvage: recovered partial block");
                    chainJson = salvaged; // empty if nothing recoverable → falls to name-scan
                }
            }

            // Client-side fallback: if the model described a chain in prose but omitted the
            // machine block (or it couldn't be salvaged), reconstruct from mention-order scanning.
            // Triggers when ≥2 recommendable plugin names appear in the reply in order.
            if (chainJson.isEmpty() && success)
            {
                juce::StringArray recommNames = safeThis->processorRef.getChainHost().getRecommendableNames();
                struct Mention { juce::String name; int pos; };
                std::vector<Mention> mentions;
                juce::String lowerReply = visibleReply.toLowerCase();
                for (auto& n : recommNames)
                {
                    int p = lowerReply.indexOf(n.toLowerCase());
                    if (p >= 0)
                        mentions.push_back({ n, p });
                }
                if ((int)mentions.size() >= 2)
                {
                    std::sort(mentions.begin(), mentions.end(),
                              [](const Mention& a, const Mention& b) { return a.pos < b.pos; });
                    juce::String arr;
                    for (int i = 0; i < (int)mentions.size(); ++i)
                    {
                        if (i > 0) arr += ",";
                        arr += "{\"name\":\"" + mentions[i].name + "\",\"role\":\"from reply\"}";
                    }
                    chainJson = "{\"chain\":[" + arr + "],\"explanation\":\"Chain extracted from reply text\"}";
                    DBG("EchoJay chain fallback: synthesised block from " + juce::String(mentions.size()) + " mentions");
                }
            }


            if (success) {
                ChatMsg cm;
                cm.role      = "assistant";
                cm.content   = visibleReply;
                cm.chainData = chainJson;   // empty if model didn't return a chain block
                safeThis->chatMessages.push_back(cm);
                safeThis->processorRef.chatHistory.push_back({"assistant", visibleReply});
                safeThis->processorRef.chatRoles.add("assistant");
                safeThis->processorRef.chatContents.add(visibleReply);
            } else {
                safeThis->chatMessages.push_back({"assistant", reply});
                safeThis->processorRef.chatHistory.push_back({"assistant", reply});
            }

            // Mirror assistant turn to workspace and persist (visibleReply has block stripped; chain stored separately)
            safeThis->workspace.appendMessageToChat(activeChatId, "assistant", visibleReply,
                                                    {}, chainJson);
            if (safeThis->sidebarModel)
            {
                safeThis->sidebarModel->refreshRows(
                    safeThis->workspace.getChats(),
                    safeThis->workspace.getAlbums(),
                    safeThis->workspace.getReviews(),
                    safeThis->collapsedAlbums,
                    safeThis->currentChatId);
                safeThis->chatSidebar.updateContent();
            }
            safeThis->workspace.requestMutationSync();
            safeThis->repaint();
        });
}

void EchoJayEditor::loadChainFromJson(const juce::String& chainJson)
{
    auto parsedVar = juce::JSON::parse(chainJson);
    if (!parsedVar.isObject()) return;
    auto* obj = parsedVar.getDynamicObject();
    if (!obj || !obj->hasProperty("chain")) return;
    auto chainArr = obj->getProperty("chain");
    if (!chainArr.isArray()) return;

    auto& ch = processorRef.getChainHost();
    juce::StringArray recommNames = ch.getRecommendableNames();

    // Collect names+settings from chain JSON, filtering to only those in recommendable
    struct SlotSpec { juce::String name; juce::String settings; };
    std::vector<SlotSpec> slots;
    for (int i = 0; i < chainArr.size(); ++i)
    {
        auto entry = chainArr[i];
        if (!entry.isObject()) continue;
        auto* entryObj = entry.getDynamicObject();
        if (!entryObj || !entryObj->hasProperty("name")) continue;
        juce::String name     = entryObj->getProperty("name").toString().trim();
        juce::String settings = entryObj->getProperty("settings").toString().trim();
        bool found = false;
        for (auto& r : recommNames)
            if (r.equalsIgnoreCase(name)) { found = true; break; }
        if (found) slots.push_back({ name, settings });
        else DBG("loadChainFromJson: skipping unknown name: " + name);
    }
    if (slots.empty()) return;

    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);

    auto doLoad = [safeThis, slots, chainJson]()
    {
        if (safeThis == nullptr) return;

        // Clear existing rack
        auto& ch2 = safeThis->processorRef.getChainHost();
        int n = ch2.getNumSlots();
        for (int i = n - 1; i >= 0; --i) ch2.removeSlot(i);
        safeThis->chainSelectedSlot_ = -1;
        safeThis->chainEditorHolder.setHostedEditor(nullptr);

        // Switch to Chain tab so the user sees loading progress
        safeThis->switchToTab(Tab::Chain);

        // Load slots sequentially; after each success, store settings on the slot
        auto loadNextPtr = std::make_shared<std::function<void()>>();
        auto idx         = std::make_shared<int>(0);
        auto skipped     = std::make_shared<juce::StringArray>();

        *loadNextPtr = [safeThis, slots, idx, skipped, loadNextPtr, chainJson]() mutable
        {
            if (safeThis == nullptr) return;
            if (*idx >= (int)slots.size())
            {
                auto& ch3 = safeThis->processorRef.getChainHost();
                safeThis->chainSelectedSlot_ = ch3.getNumSlots() > 0 ? 0 : -1;
                safeThis->chainRackStrip.rebuild(ch3.getAllSlotInfos(), safeThis->chainSelectedSlot_);
                if (safeThis->chainSelectedSlot_ >= 0)
                {
                    safeThis->chainEditorHolder.statusText = {};
                    safeThis->chainEditorHolder.setHostedEditor(
                        ch3.createEditorForSlot(safeThis->chainSelectedSlot_));
                    // Show settings for first slot
                    safeThis->chainEditorHolder.settingsHint =
                        ch3.getSlotInfo(safeThis->chainSelectedSlot_).settings;
                    safeThis->chainEditorHolder.repaint();
                }
                juce::String status = juce::String(ch3.getNumSlots()) + " slot(s) loaded";
                if (!skipped->isEmpty())
                    status += " (" + skipped->joinIntoString(", ") + " failed)";
                safeThis->chainStatusLabel.setText(status, juce::dontSendNotification);
                // Debug: show full raw chain JSON so we can verify settings fields
                safeThis->chainDebugJsonBox.setText(chainJson, false);
                safeThis->resized();
                safeThis->repaint();
                if (!skipped->isEmpty())
                    safeThis->promptForFailedPlugins(*skipped);
                return;
            }
            int i = (*idx)++;
            juce::String name     = slots[i].name;
            juce::String settings = slots[i].settings;
            auto& ch3 = safeThis->processorRef.getChainHost();
            ch3.loadByRecommendedName(name,
                [safeThis, name, settings, skipped, loadNextPtr](const juce::String& err) mutable
                {
                    if (err.isNotEmpty())
                    {
                        DBG("AI chain load failed for \"" + name + "\": " + err);
                        skipped->add(name);
                    }
                    else if (settings.isNotEmpty())
                    {
                        // Store settings on the just-loaded slot (last slot in chain)
                        auto& ch4 = safeThis->processorRef.getChainHost();
                        ch4.setSlotSettings(ch4.getNumSlots() - 1, settings);
                    }
                    (*loadNextPtr)();
                });
        };
        (*loadNextPtr)();
    };

    if (ch.getNumSlots() > 0)
    {
        juce::AlertWindow::showOkCancelBox(
            juce::AlertWindow::QuestionIcon,
            "Replace chain?",
            juce::String(ch.getNumSlots()) + " slot(s) will be cleared. Build the AI chain?",
            "Build", "Cancel", nullptr,
            juce::ModalCallbackFunction::create([doLoad](int result) mutable
            {
                if (result == 1) doLoad();
            }));
    }
    else
    {
        doLoad();
    }
}

void EchoJayEditor::promptForFailedPlugins(juce::StringArray failed)
{
    // Filter out plugins the user already chose "Keep it" this session
    juce::StringArray toPrompt;
    for (auto& name : failed)
        if (chainFailSessionSeen_.find(name) == chainFailSessionSeen_.end())
            toPrompt.add(name);
    if (!toPrompt.isEmpty())
        showNextFailPrompt(toPrompt, 0);
}

void EchoJayEditor::showNextFailPrompt(juce::StringArray names, int idx)
{
    if (idx >= names.size()) return;
    juce::String name = names[idx];

    auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);

    juce::AlertWindow::showOkCancelBox(
        juce::AlertWindow::WarningIcon,
        "Plugin failed to load",
        "\"" + name + "\" failed to load (this often means it isn't licensed).\n"
        "Stop EchoJay from suggesting it?",
        "Don't suggest again", "Keep it",
        nullptr,
        juce::ModalCallbackFunction::create([safeThis, names, idx, name](int result) mutable
        {
            if (!safeThis) return;
            if (result == 1) // "Don't suggest again"
            {
                auto& scanner = safeThis->processorRef.getPluginScanner();
                for (auto& p : scanner.getPlugins())
                {
                    if (p.name.equalsIgnoreCase(name))
                    {
                        scanner.setPluginEnabled(p.uid, false);
                        scanner.saveEnabledState();
                        // Keep Settings checklist in sync
                        if (safeThis->settingsChecklist)
                            safeThis->settingsChecklist->refresh();
                        // Rebuild resolver so the AI no longer sees this plugin
                        auto& ch = safeThis->processorRef.getChainHost();
                        ch.buildRecommendable(scanner.getPlugins(), safeThis->chainFormatFilter_);
                        break;
                    }
                }
            }
            else // "Keep it" — don't prompt again this session
            {
                safeThis->chainFailSessionSeen_.insert(name);
            }
            safeThis->showNextFailPrompt(names, idx + 1);
        }));
}

void EchoJayEditor::requestAIFeedback(const CaptureSnapshot& snap,
                                       const juce::String& chatId,
                                       const juce::String& reviewId,
                                       const juce::String& passName,
                                       int version,
                                       const WsReview* prevReview)
{
    auto ff = [](float v) -> juce::String { return v > -99 ? juce::String(v, 1) : "N/A"; };
    auto& d = snap.averagedData;
    juce::String ch = snap.getChannelDisplayName();
    juce::String proj = processorRef.getProjectName().trim();

    // ── User-visible prompt ────────────────────────────────────────────────
    // Already pushed as persisted workspace message in the capture handler.
    // Here we build what actually goes to the API (with hidden meter context).
    // The display message was already pushed to chatMessages in the caller.

    chatLoading = true;
    repaint();

    bool isFullMix = (ch == "Mix Bus" || ch == "Master Bus" || ch == "Music Bus" || ch == "Instrument Bus");
    bool isIndividual = !isFullMix;
    
    // Track whether we flagged anything (individual channels only)
    bool flaggedAnything = false;
    
    // Build flags first before constructing the context string
    juce::String flagsStr;
    
    if (isFullMix)
    {
        // Full mix: always send loudness and crest
        flagsStr += "Loudness: Integrated " + ff(d.integrated) + " LUFS\n";
        flagsStr += "Dynamics: Avg Crest " + juce::String(d.crestFactor, 1) + " dB\n";
    }
    else
    {
        // Individual channels: only flag crest if drastically off
        if (d.crestFactor < 3.0f)
        {
            flagsStr += "Dynamics: Crest " + juce::String(d.crestFactor, 1) + " dB — extremely squashed\n";
            flaggedAnything = true;
        }
        
        // Include LUFS quietly for sanity checking — the AI is told not to mention it,
        // but uses it to detect if the readings don't match the channel type
        flagsStr += "(Internal — do not mention to user) Integrated: " + ff(d.integrated) + " LUFS, LRA: " + juce::String(d.loudnessRange, 1) + " LU, Crest: " + juce::String(d.crestFactor, 1) + " dB\n";
    }
    
    // True peak: flag clipping
    float tpThreshold = isIndividual ? 0.0f : 2.0f;
    if (d.truePeakL > tpThreshold || d.truePeakR > tpThreshold)
    {
        flagsStr += "True Peak: L " + ff(d.truePeakL) + " dBTP | R " + ff(d.truePeakR) + " dBTP — clipping\n";
        flaggedAnything = true;
    }
    
    // Stereo data
    if (isFullMix)
    {
        if (d.width < 10.0f)
            flagsStr += "Stereo: Width " + juce::String(d.width, 1) + "% — narrow\n";
        else if (d.width > 55.0f)
            flagsStr += "Stereo: Width " + juce::String(d.width, 1) + "% — wide\n";
        if (d.correlation < 0.0f)
            flagsStr += "Correlation: " + juce::String(d.correlation, 2) + " — PHASE ISSUES\n";
    }
    else if (ch == "Synth Pad" || ch == "Overheads" || ch == "Orchestral" || ch == "Strings")
    {
        if (d.width < 5.0f)
        {
            flagsStr += "Stereo: Width " + juce::String(d.width, 1) + "% — unexpectedly mono\n";
            flaggedAnything = true;
        }
    }
    
    // ============ Spectrum analysis ============
    // 64 log-spaced bins: 20Hz–20kHz. Compute average dB per musical band.
    // Bin mapping (approx at 44.1k): bin 0 ≈ 20Hz, bin 63 ≈ 20kHz
    // We define 6 bands by bin ranges (log-spaced, so each octave is ~6.4 bins):
    //   Sub   (20-80Hz)   bins  0-11
    //   Low   (80-250Hz)  bins 12-21
    //   LMid  (250-1kHz)  bins 22-33
    //   Mid   (1k-4kHz)   bins 34-44
    //   UMid  (4k-10kHz)  bins 45-53
    //   High  (10k-20kHz) bins 54-63
    struct FreqBand { const char* name; int lo; int hi; float avgDb; };
    FreqBand bands[] = {
        { "Sub (20-80Hz)",     0, 11, -120.0f },
        { "Low (80-250Hz)",   12, 21, -120.0f },
        { "Low-Mid (250Hz-1kHz)", 22, 33, -120.0f },
        { "Mid (1-4kHz)",     34, 44, -120.0f },
        { "Upper-Mid (4-10kHz)", 45, 53, -120.0f },
        { "High (10-20kHz)",  54, 63, -120.0f }
    };
    constexpr int numBands = 6;
    
    for (int b = 0; b < numBands; ++b)
    {
        float sum = 0.0f;
        int count = bands[b].hi - bands[b].lo + 1;
        for (int i = bands[b].lo; i <= bands[b].hi; ++i)
            sum += d.spectrum[(size_t)i];
        bands[b].avgDb = sum / (float)count;
    }
    
    // Find overall peak band for context
    float peakBandDb = -120.0f;
    int peakBandIdx = 0;
    for (int b = 0; b < numBands; ++b)
        if (bands[b].avgDb > peakBandDb) { peakBandDb = bands[b].avgDb; peakBandIdx = b; }
    
    // ============ Per-band crest (peak - avg) ============
    // Crest tells us the CHARACTER of the energy in each band, not just whether
    // it's there. A hi-hat has very high crest (>15dB) in the upper bands —
    // sharp transients. An 808 has low crest (4-8dB) in the sub — sustained tone.
    // A snare has high crest in the mids. A vocal sits at medium crest.
    // This is what lets us tell channel mismatches apart, not just "is the band empty?".
    float bandCrest[numBands];
    bool hasCrestData = snap.hasDualSpectrum;
    if (hasCrestData)
    {
        for (int b = 0; b < numBands; ++b)
        {
            float pkSum = 0.0f, avSum = 0.0f;
            int cnt = bands[b].hi - bands[b].lo + 1;
            for (int i = bands[b].lo; i <= bands[b].hi; ++i)
            {
                pkSum += snap.peakSpectrum[(size_t)i];
                avSum += snap.avgSpectrum[(size_t)i];
            }
            float pkDb = pkSum / (float)cnt;
            float avDb = avSum / (float)cnt;
            bandCrest[b] = pkDb - avDb;
        }
    }
    else
    {
        for (int b = 0; b < numBands; ++b) bandCrest[b] = 0.0f;
    }
    
    // ============ Channel-shape check ============
    // Build a "fingerprint" check for the selected channel type. If the actual
    // capture's energy profile doesn't match the fingerprint, emit a
    // CHANNEL SHAPE UNUSUAL flag — the AI is instructed to raise it as a
    // question, not a verdict. Unusual readings can be intentional (distortion,
    // layering, sidechaining), so we frame this as "worth checking" rather than
    // "this is wrong".
    //
    // Band indices: 0=Sub, 1=Low, 2=LMid, 3=Mid, 4=UMid, 5=High
    juce::String channelMismatchFlag;
    if (isIndividual && hasCrestData)
    {
        // How "active" is each band relative to the peak band?
        auto bandIsActive = [&](int b) {
            return (peakBandDb - bands[b].avgDb) < 25.0f && bands[b].avgDb > -90.0f;
        };
        bool subActive  = bandIsActive(0);
        bool lowActive  = bandIsActive(1);
        bool midActive  = bandIsActive(3);
        bool umidActive = bandIsActive(4);
        bool highActive = bandIsActive(5);
        
        auto guessShape = [&]() -> juce::String {
            // Peak in highs/upper-mids with very high crest = transient cymbal/hat
            if ((peakBandIdx >= 4) && bandCrest[peakBandIdx] > 14.0f && !subActive && !lowActive)
                return "a hi-hat or cymbal/percussion (peak energy in highs with sharp transients)";
            // Peak in sub/low with low crest = sustained bass
            if (peakBandIdx <= 1 && bandCrest[peakBandIdx] < 9.0f && !midActive && !umidActive && !highActive)
                return "a sustained bass / 808 / sub (peak in low end, no transient character up top)";
            // Peak in sub/low with HIGH crest = kick
            if (peakBandIdx <= 1 && bandCrest[peakBandIdx] > 12.0f)
                return "a kick drum (transient low-end energy)";
            // Peak in low-mid/mid with high crest = snare
            if ((peakBandIdx == 2 || peakBandIdx == 3) && bandCrest[peakBandIdx] > 10.0f)
                return "a snare or drum-like transient element";
            // Energy across most bands with moderate crest everywhere = full mix
            int activeCount = (int)subActive + (int)lowActive + (int)midActive + (int)umidActive + (int)highActive;
            if (activeCount >= 4)
                return "a full mix or multi-element bus (energy spread across most bands)";
            // Peak in mid with moderate crest = vocal/melodic lead
            if (peakBandIdx == 3 && bandCrest[3] > 5.0f && bandCrest[3] < 12.0f)
                return "a vocal or melodic lead (mid-range with moderate dynamics)";
            return juce::String();
        };
        
        // Per-channel mismatch rules. Each returns true when capture does NOT match channel.
        bool mismatch = false;
        juce::String reason;
        
        // Bass-family channels: should peak low, have sustained (low-crest) lows
        if (ch == "Bass / 808" || ch == "Sub Bass" || ch == "Synth Bass")
        {
            if (peakBandIdx >= 3) // peak in mids or higher
            { mismatch = true; reason = "peak energy is in the upper bands, not the low end"; }
            else if (peakBandIdx <= 1 && bandCrest[peakBandIdx] > 14.0f)
            { mismatch = true; reason = "the low-end character is highly transient — more like a kick than a sustained bass"; }
            else if (highActive && bandCrest[5] > 14.0f && !subActive)
            { mismatch = true; reason = "high-frequency transient energy with no sub — looks more like a hat or percussion"; }
        }
        else if (ch == "Bass Guitar")
        {
            if (peakBandIdx >= 4)
            { mismatch = true; reason = "peak energy is in the high band, not the bass range"; }
        }
        else if (ch == "Kick")
        {
            // Kick can have low crest if heavily compressed but should peak low
            if (peakBandIdx >= 4)
            { mismatch = true; reason = "peak energy is in the high frequencies, not the low end where a kick lives"; }
        }
        else if (ch == "Hi-Hat")
        {
            if (peakBandIdx <= 1)
            { mismatch = true; reason = "peak energy is in the sub/low — hi-hats live in the upper bands"; }
            else if (peakBandIdx <= 3 && bandCrest[5] < 6.0f)
            { mismatch = true; reason = "no transient character in the highs — doesn't look like a hi-hat"; }
        }
        else if (ch == "Snare")
        {
            if (peakBandIdx == 0) // peak in sub
            { mismatch = true; reason = "peak energy is in the sub — that's not where a snare lives"; }
        }
        else if (ch == "Lead Vocal" || ch == "Backing Vocal" || ch == "Adlibs" || ch == "Vocal Bus")
        {
            // Peak in sub = definitely not a vocal
            if (peakBandIdx == 0)
            { mismatch = true; reason = "peak energy is in the sub-bass range — not characteristic of a vocal"; }
            else
            {
                // Loosened: 4+ active bands (was 5+) catches mixes where mastering HPF
                // pushes the sub band 25-35dB below peak, which fails the strict 5-band test.
                int activeCount = (int)subActive + (int)lowActive + (int)midActive + (int)umidActive + (int)highActive;
                // Sustained character in low end = bass + kick = full mix shape, not a vocal
                bool sustainedSub = subActive && bandCrest[0] < 10.0f;
                bool sustainedLow = lowActive && bandCrest[1] < 9.0f;
                
                if (activeCount >= 4 && (sustainedSub || sustainedLow))
                { mismatch = true; reason = "energy is spread across the frequency range with sustained low-end content — looks more like a full mix or instrumental than an isolated vocal"; }
                // Or: any vocal channel with significant sub presence is suspicious — vocals
                // shouldn't have meaningful sub energy unless they're heavily processed/layered
                else if (subActive && (peakBandDb - bands[0].avgDb) < 18.0f)
                { mismatch = true; reason = "there's significant sub-bass content for a vocal channel — vocals usually sit above 100Hz, so this could be a full mix or unusual processing"; }
            }
        }
        else if (ch == "Sub Bass")
        {
            if (peakBandIdx >= 2)
            { mismatch = true; reason = "peak energy is above the sub range — sub-bass should sit below 80Hz"; }
        }
        else if (ch == "Piano" || ch == "Keys" || ch == "Acoustic Guitar" || ch == "Electric Guitar")
        {
            if (peakBandIdx == 0 && bandCrest[0] > 12.0f && !midActive)
            { mismatch = true; reason = "peak is a low transient, not a melodic mid-range element"; }
        }
        // Drum/percussion buses: should be transient (high crest) somewhere
        else if (ch == "Drum Bus" || ch == "Percussion")
        {
            if (bandCrest[peakBandIdx] < 6.0f)
            { mismatch = true; reason = "no transient character — drums/percussion should show sharper peaks than this"; }
        }
        
        if (mismatch)
        {
            juce::String guess = guessShape();
            channelMismatchFlag = "CHANNEL SHAPE UNUSUAL: The reading is unusual for a \"" + ch + "\" — " + reason;
            if (guess.isNotEmpty())
                channelMismatchFlag += ". The shape is closer to what we'd expect from " + guess;
            channelMismatchFlag += ". Mention this to the user as something worth checking — frame it as a question, not a verdict (e.g. 'these readings are a bit unusual for an 808 — is this definitely a bass?'). The user knows what they captured better than the meters do, so it might be a genuine creative choice (heavy distortion, layering, sidechaining etc) — leave room for that.\n";
            flaggedAnything = true;
        }
    }
    
    // ============ Bus-type shape check ============
    // Mix Bus / Master Bus / Music Bus channels expect a full mix. If someone
    // accidentally puts a single element through (vocal, hat, bass alone), the
    // shape is obviously wrong. Flag it so the AI can ask whether the right
    // channel was used. Same questioning tone as channel-shape mismatches above.
    if (hasCrestData && (ch == "Mix Bus" || ch == "Master Bus" || ch == "Music Bus") && channelMismatchFlag.isEmpty())
    {
        auto bandIsActive = [&](int b) {
            return (peakBandDb - bands[b].avgDb) < 25.0f && bands[b].avgDb > -90.0f;
        };
        bool subActive  = bandIsActive(0);
        bool lowActive  = bandIsActive(1);
        bool midActive  = bandIsActive(3);
        bool umidActive = bandIsActive(4);
        bool highActive = bandIsActive(5);
        int activeCount = (int)subActive + (int)lowActive + (int)midActive + (int)umidActive + (int)highActive;
        bool sustainedLowEnd = (subActive && bandCrest[0] < 12.0f) || (lowActive && bandCrest[1] < 12.0f);
        
        bool busShapeOdd = false;
        juce::String busReason, busGuess;
        
        // Less than 3 active bands = probably not a full mix (full mixes spread across spectrum)
        if (activeCount < 3)
        { busShapeOdd = true; busReason = "energy is concentrated in only " + juce::String(activeCount) + " band(s) — a full mix usually has energy across most of the spectrum"; busGuess = "an isolated element rather than a full mix"; }
        // Peak in mids/upper-mids with no sustained low end = vocal/single melodic element
        else if ((peakBandIdx == 3 || peakBandIdx == 4) && !sustainedLowEnd && !subActive)
        { busShapeOdd = true; busReason = "the energy is sitting up in the mids with no sustained low end"; busGuess = "an isolated vocal or single melodic element"; }
        // Peak in highs with no low/mid energy = hats/cymbals
        else if (peakBandIdx >= 4 && !lowActive && !midActive)
        { busShapeOdd = true; busReason = "energy is concentrated in the highs with very little low or mid content"; busGuess = "an isolated hi-hat, cymbal, or percussion stem"; }
        // Peak in sub/low with no mids/uppers = bass alone
        else if (peakBandIdx <= 1 && !midActive && !umidActive)
        { busShapeOdd = true; busReason = "energy is concentrated in the low end with no mids or highs"; busGuess = "an isolated bass, kick, or sub element"; }
        
        if (busShapeOdd)
        {
            channelMismatchFlag = "CHANNEL SHAPE UNUSUAL: The reading is unusual for a \"" + ch + "\" — " + busReason
                + ". The shape is closer to what we'd expect from " + busGuess
                + ". Mention this to the user as something worth checking — frame it as a question, not a verdict (e.g. 'these readings look more like a single element than a full mix — did you mean to capture this on the Mix Bus?'). They might have a stripped-back section playing or be testing something unusual, so leave room for that.\n";
            flaggedAnything = true;
        }
    }
    
    // ============ Bus-type drift check ============
    // For Mix Bus / Master Bus / Music Bus, if a subsequent capture in the same
    // session is drastically different from the previous one, flag it. This catches
    // accidents like running a vocal through Mix Bus by mistake when the previous
    // capture was the actual full mix. Only fires if 2+ drift signals trigger
    // together, only within a session (where there's a previous AI response).
    if (hasCrestData && (ch == "Mix Bus" || ch == "Master Bus" || ch == "Music Bus") && channelMismatchFlag.isEmpty())
    {
        auto driftSnaps = processorRef.getSnapshots();
        bool hasSessionContinuity = false;
        for (auto& entry : processorRef.chatHistory)
            if (entry.role == "assistant") { hasSessionContinuity = true; break; }
        
        // Find most recent prior snapshot of same channel type with dual spectrum
        int prevDriftIdx = -1;
        for (int i = (int)driftSnaps.size() - 2; i >= 0; --i)
        {
            if (driftSnaps[(size_t)i].channelType == snap.channelType && driftSnaps[(size_t)i].hasDualSpectrum)
            { prevDriftIdx = i; break; }
        }
        
        if (prevDriftIdx >= 0 && hasSessionContinuity)
        {
            auto& prev = driftSnaps[(size_t)prevDriftIdx];
            int driftFlags = 0;
            juce::StringArray driftReasons;
            
            // LUFS shift > 5 dB
            float lufsDiff = std::abs(snap.averagedData.integrated - prev.averagedData.integrated);
            if (lufsDiff > 5.0f)
            { driftFlags++; driftReasons.add("loudness has shifted by " + juce::String((int)lufsDiff) + " LUFS"); }
            
            // Crest shift > 4 dB
            float crestDiff = std::abs(snap.averagedData.crestFactor - prev.averagedData.crestFactor);
            if (crestDiff > 4.0f)
            { driftFlags++; driftReasons.add("dynamic range has changed significantly"); }
            
            // Peak band shift by 2+ positions
            int prevPeakBand = 0;
            float prevPeakDb = -120.0f;
            for (int b = 0; b < numBands; ++b)
            {
                float sum = 0.0f;
                int cnt = bands[b].hi - bands[b].lo + 1;
                for (int i = bands[b].lo; i <= bands[b].hi; ++i)
                    sum += prev.avgSpectrum[(size_t)i];
                float avg = sum / (float)cnt;
                if (avg > prevPeakDb) { prevPeakDb = avg; prevPeakBand = b; }
            }
            if (std::abs(peakBandIdx - prevPeakBand) >= 2)
            { driftFlags++; driftReasons.add("the spectrum shape has moved — a different frequency range is dominant now"); }
            
            // Width shift > 30%
            float widthDiff = std::abs(snap.averagedData.width - prev.averagedData.width);
            if (widthDiff > 30.0f)
            { driftFlags++; driftReasons.add("stereo width has changed dramatically"); }
            
            if (driftFlags >= 2)
            {
                channelMismatchFlag = "CAPTURE DRIFT: This " + ch + " capture is significantly different from the previous one — " 
                    + driftReasons.joinIntoString(", ") 
                    + ". Open by mentioning this is a big shift from the previous pass and ask what's different — could be a different song or section, an element captured by accident, or major mix changes. Don't assume it's a mistake; frame it as a question.\n";
                flaggedAnything = true;
            }
        }
    }
    
    // "Empty" threshold: if a band is more than 50dB below the peak band, 
    // or below -100dB absolute, consider it empty.
    // Note: averaged spectrum on quiet sources can sit around -60 to -80dB,
    // so -80 was way too aggressive — caused false "empty" flags on real signals.
    constexpr float emptyRelativeThreshold = 50.0f;
    constexpr float emptyAbsoluteThreshold = -100.0f;
    
    // Build a compact spectrum summary for the AI
    juce::String spectrumFlags;
    
    // Count how many bands are essentially empty
    int emptyBandCount = 0;
    juce::String emptyBandNames;
    int activeBandCount = 0;
    juce::String activeBandSummary;
    
    for (int b = 0; b < numBands; ++b)
    {
        bool isEmpty = (bands[b].avgDb < emptyAbsoluteThreshold) ||
                       (peakBandDb - bands[b].avgDb > emptyRelativeThreshold);
        if (isEmpty)
        {
            emptyBandCount++;
            if (emptyBandNames.isNotEmpty()) emptyBandNames += ", ";
            emptyBandNames += bands[b].name;
        }
        else
        {
            activeBandCount++;
            if (activeBandSummary.isNotEmpty()) activeBandSummary += ", ";
            activeBandSummary += juce::String(bands[b].name) + ": " + juce::String(bands[b].avgDb, 0) + "dB";
        }
    }
    
    if (isFullMix)
    {
        // Full mix: ONLY flag spectrum when something is drastically wrong.
        // Normal spectrum shapes — even unusual ones — are creative choices.
        // We flag: empty bands (entire range missing) and sustained near-0dBFS energy.
        // We do NOT flag tonal imbalance — a pop vocal/guitar track with no sub bass
        // is normal, and a bass-heavy hip-hop track is also normal. That's genre, not a problem.
        
        if (emptyBandCount > 0)
        {
            flagsStr += "Spectrum: " + activeBandSummary + "\n";
            flagsStr += juce::String("SPECTRUM ISSUE: No energy in ") + emptyBandNames + " — entire frequency range(s) missing from this mix\n";
        }
        
        // Check for any band peaking near 0dBFS (sustained clipping-level energy)
        for (int b = 0; b < numBands; ++b)
        {
            if (bands[b].avgDb > -3.0f)
            {
                if (emptyBandCount == 0) flagsStr += "Spectrum: " + activeBandSummary + "\n";
                flagsStr += juce::String("SPECTRUM ISSUE: ") + bands[b].name + " is averaging " + juce::String(bands[b].avgDb, 0) + "dB — extremely hot\n";
                break;
            }
        }
        // If spectrum looks normal — don't send it at all.
    }
    else
    {
        // Individual channels: flag spectrum anomalies based on channel type
        
        // --- Channels that SHOULD have low-end energy ---
        bool expectsLowEnd = (ch == "Kick" || ch == "Bass / 808" || ch == "Bass Guitar" || 
                              ch == "Sub Bass" || ch == "Synth Bass" || ch == "Drum Bus");
        
        // --- Channels that should NOT have significant low-end ---
        bool expectsNoLowEnd = (ch == "Hi-Hat" || ch == "Percussion" || ch == "Synth Pluck" ||
                                ch == "Adlibs" || ch == "FX");
        
        // --- Channels that should have presence/mid energy ---
        bool expectsMids = (ch == "Lead Vocal" || ch == "Backing Vocal" || ch == "Snare" ||
                            ch == "Electric Guitar" || ch == "Acoustic Guitar" || ch == "Piano" ||
                            ch == "Keys" || ch == "Synth Lead" || ch == "Brass");
        
        // Only include spectrum details when there's something to flag
        // (individual warnings below will set flaggedAnything and add specific text)
        
        // Flag: channel expects low-end but has none
        bool subEmpty = (bands[0].avgDb < emptyAbsoluteThreshold) || 
                        (peakBandDb - bands[0].avgDb > emptyRelativeThreshold);
        bool lowEmpty = (bands[1].avgDb < emptyAbsoluteThreshold) || 
                        (peakBandDb - bands[1].avgDb > emptyRelativeThreshold);
        
        if (expectsLowEnd && subEmpty && lowEmpty)
        {
            flagsStr += "SPECTRUM WARNING: No sub or low frequency content detected — unusual for " + ch.toLowerCase() + ". Possible HPF issue or wrong channel type selected.\n";
            flaggedAnything = true;
        }
        else if (expectsLowEnd && subEmpty && !lowEmpty)
        {
            flagsStr += "SPECTRUM NOTE: No sub content below 80Hz — the low end starts from around 80Hz upward.\n";
            flaggedAnything = true;
        }
        
        // Flag: channel shouldn't have low-end but does
        if (expectsNoLowEnd && !subEmpty)
        {
            float subLevel = bands[0].avgDb;
            if (subLevel > -50.0f)
            {
                flagsStr += "SPECTRUM WARNING: Sub energy detected at " + juce::String(subLevel, 0) + "dB — likely bleed or missing HPF.\n";
                flaggedAnything = true;
            }
        }
        if (expectsNoLowEnd && !lowEmpty)
        {
            float lowLevel = bands[1].avgDb;
            if (lowLevel > -40.0f)
            {
                flagsStr += "SPECTRUM WARNING: Low-end energy detected at " + juce::String(lowLevel, 0) + "dB — bleed or missing HPF.\n";
                flaggedAnything = true;
            }
        }
        
        // Flag: vocal/lead channels with no presence range
        bool midEmpty = (bands[3].avgDb < emptyAbsoluteThreshold) || 
                        (peakBandDb - bands[3].avgDb > emptyRelativeThreshold);
        if (expectsMids && midEmpty)
        {
            flagsStr += "SPECTRUM WARNING: No mid-range energy (1-4kHz) — this will lack presence and clarity.\n";
            flaggedAnything = true;
        }
        
        // Flag: extreme case — almost all energy in one band only
        if (activeBandCount <= 1 && emptyBandCount >= 4)
        {
            flagsStr += juce::String("SPECTRUM WARNING: Energy concentrated in only ") + juce::String(activeBandCount) + " band(s) — " + 
                juce::String(emptyBandCount) + " bands are empty. This is very unusual and suggests heavy filtering, wrong channel, or a processing issue.\n";
            flaggedAnything = true;
        }
        
        // Flag: everything is empty (silence or near-silence)
        if (emptyBandCount == numBands)
        {
            flagsStr += "SPECTRUM WARNING: No significant energy in any frequency band — the signal may be extremely quiet or silent.\n";
            flaggedAnything = true;
        }
    }
    
    // Prepend the channel-shape flag so it sits at the top of the report.
    // The AI is instructed (in the system prompt) to raise this as a question
    // rather than a verdict, since unusual readings can be intentional.
    if (channelMismatchFlag.isNotEmpty())
        flagsStr = channelMismatchFlag + flagsStr;
    
    // For individual channels with a shape flag, attach a compact band profile
    // showing where the energy actually IS — this gives the AI concrete data to
    // reference when raising the question (and prevents it from guessing).
    if (isIndividual && hasCrestData && channelMismatchFlag.isNotEmpty())
    {
        juce::String profile = "BAND PROFILE (avg dB | crest dB): ";
        const char* shortNames[] = { "Sub", "Low", "LMid", "Mid", "UMid", "High" };
        for (int b = 0; b < numBands; ++b)
        {
            if (b > 0) profile += ", ";
            profile += juce::String(shortNames[b]) + " " + juce::String(bands[b].avgDb, 0) + "/" + juce::String(bandCrest[b], 0);
        }
        profile += " (high crest = transient/sharp, low crest = sustained/tonal)\n";
        flagsStr += profile;
    }
    
    // Helper: build a plain-language band profile for follow-up reference.
    // Always include this when nothing's flagged so the AI has real data to
    // describe if the user asks about frequencies — without it, the AI claims
    // "no spectrum data" which is a lie (and looks broken).
    auto buildReferenceBandProfile = [&]() -> juce::String {
        if (!hasCrestData)
            return juce::String();
        const char* shortNames[] = { "Sub", "Low", "LMid", "Mid", "UMid", "High" };
        // Find the loudest band for plain-language summary
        int loudest = 0;
        float loudestDb = -999.0f;
        for (int b = 0; b < numBands; ++b)
            if (bands[b].avgDb > loudestDb) { loudestDb = bands[b].avgDb; loudest = b; }
        // Find the quietest of the active bands
        int quietest = 0;
        float quietestDb = 999.0f;
        for (int b = 0; b < numBands; ++b)
            if (bands[b].avgDb < quietestDb && bands[b].avgDb > -90.0f) { quietestDb = bands[b].avgDb; quietest = b; }
        
        juce::String p = "[SPECTRUM REFERENCE — DO NOT mention this on the initial review. ONLY use if the user asks about frequencies, tonal balance, the spectrum, or the lows/mids/highs. When they ask, describe in plain language — never quote dB values or band names.]\n";
        p += "Band profile (avg dB / crest dB): ";
        for (int b = 0; b < numBands; ++b)
        {
            if (b > 0) p += ", ";
            p += juce::String(shortNames[b]) + " " + juce::String(bands[b].avgDb, 0) + "/" + juce::String(bandCrest[b], 0);
        }
        // Plain-language summary of overall shape
        p += "\nShape summary: ";
        const char* plainNames[] = { "sub-bass", "low end", "low-mids", "mids", "upper-mids", "highs" };
        if (loudest != quietest)
            p += juce::String("loudest in the ") + plainNames[loudest] + ", quietest in the " + plainNames[quietest];
        else
            p += juce::String("fairly even across bands");
        // Note about transients vs sustained based on average crest
        float avgCrestAll = 0.0f;
        for (int b = 0; b < numBands; ++b) avgCrestAll += bandCrest[b];
        avgCrestAll /= (float)numBands;
        if (avgCrestAll > 12.0f)
            p += "; transient/punchy character overall";
        else if (avgCrestAll < 6.0f)
            p += "; sustained/dense character overall";
        else
            p += "; mixed dynamic character";
        p += "\n";
        return p;
    };
    
    // Build the actual context string
    juce::String meterCtx;
    int mins = (int)snap.durationSeconds / 60;
    int secs = (int)snap.durationSeconds % 60;
    juce::String durStr = juce::String(mins) + ":" + juce::String(secs).paddedLeft('0', 2);
    
    if (!flaggedAnything)
    {
        // Nothing flagged — but we still include the spectrum band profile so the
        // AI can answer follow-up frequency questions truthfully. The header tells
        // the AI the meters aren't flagging anything, and the SPECTRUM REFERENCE
        // block (added below) tells it to only use that data on follow-up.
        meterCtx = "\n\n[" + (isFullMix ? "FULL TRACK" : ch.toUpperCase()) + 
                   (isIndividual ? " CHANNEL" : " ANALYSIS") + 
                   ": \"" + snap.name + "\" (" + durStr + ") — meters aren't flagging anything]\n";
        // For full mix we ALWAYS emit LUFS and crest values, even when nothing
        // is flagged — without these the AI hallucinates plausible-sounding
        // numbers (consistently inventing things like "-9 LUFS, 8dB crest").
        // flagsStr contains those lines for full mix even on no-flag captures.
        if (isFullMix)
            meterCtx += flagsStr;
        meterCtx += buildReferenceBandProfile();
    }
    else
    {
        meterCtx = "\n\n[" + (isFullMix ? "FULL TRACK" : ch.toUpperCase()) + " ANALYSIS: \"" + snap.name + "\" (" + durStr + ")]\n";
        meterCtx += flagsStr;
        // Also append the spectrum reference for follow-up — even when something
        // IS flagged, the user might ask about a different aspect of the spectrum
        // than what was flagged, and we want the AI to be able to answer.
        meterCtx += buildReferenceBandProfile();
    }

    // Partial analysis warning — skip for inherently short elements and buses
    bool isShortElement = (ch == "Kick" || ch == "Snare" || ch == "Hi-Hat" || 
                           ch == "Percussion" || ch == "Synth Pluck" || ch == "FX");
    bool isBusType = (ch == "Drum Bus" || ch == "Instrument Bus" || ch == "Music Bus" || 
                      ch == "Vocal Bus" || ch == "Guitar Bus" || ch == "Synth Bus");
    if (!isShortElement && !isBusType)
    {
        if (snap.durationSeconds < 5.0f)
            meterCtx += "\n⚠ PARTIAL ANALYSIS: Only " + juce::String((int)snap.durationSeconds) + 
                "s captured. These readings may not represent the full track. Keep your review brief and suggest they capture more of the track for a better analysis.";
        else if (snap.durationSeconds < 15.0f)
            meterCtx += "\n(Note: " + juce::String((int)snap.durationSeconds) + "s captured)";
    }

    // (Auto-comparison now uses prevReview from workspace chat history, not snapshots)
    
    // ── Approach angle (individual channels only) ─────────────────────────
    bool angleNeedsPlugins = false;
    if (isIndividual)
    {
        const char* angles[] = {
            "If nothing is wrong, offer to build a processing chain using their plugins with specific settings.",
            "If nothing is wrong, ask what they're going for with this sound — what vibe or direction.",
            "If nothing is wrong, suggest one creative technique they could try with this element. Be specific.",
            "If nothing is wrong, ask what part of the sound they're least happy with.",
            "If nothing is wrong, pick an interesting plugin from their list and suggest how it could colour this sound.",
            "If nothing is wrong, ask if they want to A/B this against a reference in Compare Mixes.",
            "If nothing is wrong, offer a quick tip relevant to this type of channel — something practical, not generic."
        };
        int angleIdx = (int)(juce::Time::currentTimeMillis() % 7);
        meterCtx += "\n[APPROACH: " + juce::String(angles[angleIdx]) + "]\n";
        angleNeedsPlugins = (angleIdx == 0 || angleIdx == 4);
    }

    // ── Multi-channel context (appended to meterCtx when Links active) ────
    if (!snap.channels.empty())
    {
        auto ff2 = [](float v) -> juce::String { return v > -99 ? juce::String(v, 1) : "N/A"; };
        juce::String mcCtx = "\n\n[MULTI-CHANNEL CAPTURE — " + juce::String((int)snap.channels.size()) + " channels]\n";
        mcCtx += "Channels captured: ";
        for (size_t ci = 0; ci < snap.channels.size(); ++ci)
        {
            if (ci > 0) mcCtx += ", ";
            mcCtx += snap.channels[ci].name;
        }
        mcCtx += "\n\n";
        for (size_t ci = 0; ci < snap.channels.size(); ++ci)
        {
            auto& sch = snap.channels[ci];
            auto& md = sch.meterData;
            juce::String header = (ci == 0)
                ? ("[HOST — " + sch.name + "]\n")
                : ("[LINK — " + sch.name + "]\n");
            mcCtx += header;
            mcCtx += "(Internal — do not show raw numbers) ";
            mcCtx += "Integrated: " + ff2(md.integrated) + " LUFS | LRA: " + ff2(md.loudnessRange) + " LU\n";
            mcCtx += "Peak: L " + ff2(md.peakMaxL) + " / R " + ff2(md.peakMaxR) + " dBFS\n";
            mcCtx += "RMS: L " + ff2(md.rmsL) + " / R " + ff2(md.rmsR) + " dB\n";
            mcCtx += "Crest: " + ff2(md.crestFactor) + " dB | Width: " + ff2(md.width) + "% | Corr: " + ff2(md.correlation) + "\n";
            mcCtx += "\n";
        }
        mcCtx += "[Read the whole session: note any per-channel issues and how they relate. "
                 "If the host mix looks fine, check whether the individual channels suggest a balance or "
                 "dynamics issue that might not be obvious from the full mix alone.]\n";
        meterCtx += mcCtx;
    }

    // ── Build API user content ─────────────────────────────────────────────
    // If this is the first capture in the chat (no prevReview): baseline review.
    // If there IS a previous revision: user message says "v<N> of <project>",
    // plus a hidden block of the previous revision's data for comparison.
    juce::String captureContent;
    if (prevReview == nullptr)
    {
        // First capture — baseline review
        captureContent = passName + "\n\nGive me feedback on this capture.\n\n" + meterCtx;
    }
    else
    {
        // Compare to previous revision
        auto& pr = prevReview->data;
        juce::String prevBlock =
            "\n\n[PREVIOUS REVISION DATA — do not show these numbers to the user, "
            "use them to compare against the current meter data:]\n"
            "Loudness: Integrated " + ff(pr.integ) + " LUFS | LRA " + ff(pr.range) + " LU\n"
            "Levels: Avg RMS " + ff(pr.rmsL) + "/" + ff(pr.rmsR) + " dB"
            " | Peak Max " + ff(pr.peakL) + "/" + ff(pr.peakR) + " dB"
            " | TP Max " + ff(pr.tpL) + "/" + ff(pr.tpR) + " dBTP\n"
            "Dynamics: Crest " + ff(pr.crest) + " dB | DC " + ff(pr.dc) + " mV\n"
            "Stereo: Width " + ff(pr.width) + "% | Correlation " + ff(pr.corr) + "\n";

        juce::String userLabel = proj.isEmpty()
            ? ("Here's v" + juce::String(version) + ". Compare to the previous version.")
            : ("Here's v" + juce::String(version) + " of " + proj + ". Compare to the previous version.");

        captureContent = userLabel + prevBlock + meterCtx;
    }

    if (angleNeedsPlugins)
        captureContent += EchoJayAPI::buildPluginInjection(
            processorRef.getPluginScanner().getFullPluginList());

    processorRef.chatRoles.add("user");
    processorRef.chatContents.add(captureContent);

    auto sysPrompt = EchoJayAPI::buildSystemPrompt(
        ch, processorRef.getGenre(),
        processorRef.getPluginScanner().getPluginSummary());

    auto safeThis2 = juce::Component::SafePointer<EchoJayEditor>(this);
    juce::String captureChatId = chatId;
    api.sendChat(processorRef.chatRoles, processorRef.chatContents, sysPrompt,
        [safeThis2, captureChatId](const juce::String& reply, bool success) {
            if (safeThis2 == nullptr)
                return;
            safeThis2->chatLoading = false;
            safeThis2->chatMessages.push_back({"assistant", reply});
            safeThis2->processorRef.chatHistory.push_back({"assistant", reply});
            if (success) { safeThis2->processorRef.chatRoles.add("assistant"); safeThis2->processorRef.chatContents.add(reply); }

            // Mirror assistant turn into the capture chat, increment revisionCount, persist
            if (captureChatId.isNotEmpty())
            {
                safeThis2->workspace.appendMessageToChat(captureChatId, "assistant", reply);
                safeThis2->workspace.incrementChatRevisionCount(captureChatId);
                if (safeThis2->sidebarModel)
                {
                    safeThis2->sidebarModel->refreshRows(
                        safeThis2->workspace.getChats(),
                        safeThis2->workspace.getAlbums(),
                        safeThis2->workspace.getReviews(),
                        safeThis2->collapsedAlbums,
                        safeThis2->currentChatId);
                    safeThis2->chatSidebar.updateContent();
                }
                safeThis2->workspace.requestMutationSync();
            }

            safeThis2->repaint();
        });
}

void EchoJayEditor::layoutChatMessages() {}

// ============================================================================
// Custom Channel Management
// ============================================================================

void EchoJayEditor::rebuildChannelTypeBox()
{
    channelTypeBox.clear(juce::dontSendNotification);
    
    // Use getRootMenu() to build submenus
    auto* root = channelTypeBox.getRootMenu();
    
    // ID scheme: built-in types use (int)ChannelType + 1
    // Custom channels use 1000 + index
    root->addItem(1, "Mix Bus");  // ChannelType::FullMix = 0, ID = 1
    root->addSeparator();
    
    struct SubGroup { const char* name; std::vector<std::pair<juce::String, int>> items; };
    SubGroup groups[] = {
        { "Vocals", { {"Lead Vocal", 2}, {"Backing Vocal", 3}, {"Adlibs", 4}, {"Vocal Bus", 5} } },
        { "Drums", { {"Kick", 6}, {"Snare", 7}, {"Hi-Hat", 8}, {"Overheads", 9}, {"Drum Bus", 10}, {"Percussion", 11} } },
        { "Bass", { {"Bass / 808", 12}, {"Bass Guitar", 13}, {"Sub Bass", 14}, {"Synth Bass", 15} } },
        { "Keys & Guitar", { {"Piano", 16}, {"Keys", 17}, {"Acoustic Guitar", 18}, {"Electric Guitar", 19}, {"Guitar Bus", 20} } },
        { "Synths", { {"Synth Lead", 21}, {"Synth Pad", 22}, {"Synth Pluck", 23}, {"Synth Bus", 24} } },
        { "Strings & Brass", { {"Strings", 25}, {"Brass", 26}, {"Woodwind", 27}, {"Orchestral", 28} } },
        { "FX & Other", { {"FX", 29}, {"Reverb", 30}, {"Delay", 31}, {"Foley", 32}, {"Ambient", 33} } },
        { "Buses", { {"Master Bus", 34}, {"Instrument Bus", 35}, {"Music Bus", 36} } }
    };
    
    for (auto& g : groups)
    {
        juce::PopupMenu sub;
        for (auto& item : g.items)
            sub.addItem(item.second, item.first);
        root->addSubMenu(g.name, sub);
    }
    
    // Custom channels section
    if (!customChannelNames.isEmpty())
    {
        root->addSeparator();
        juce::PopupMenu customSub;
        for (int i = 0; i < customChannelNames.size(); ++i)
            customSub.addItem(1000 + i, customChannelNames[i]);
        root->addSubMenu("Custom", customSub);
    }
    
    // Add new custom option
    root->addSeparator();
    root->addItem(999, "Add Custom...");
    
    // Restore selection
    if (processorRef.getChannelType() == ChannelType::Other && processorRef.getCustomChannelName().isNotEmpty())
    {
        // Find the custom channel in the list
        int idx = customChannelNames.indexOf(processorRef.getCustomChannelName());
        if (idx >= 0)
            channelTypeBox.setSelectedId(1000 + idx, juce::dontSendNotification);
        else
            channelTypeBox.setText(processorRef.getCustomChannelName(), juce::dontSendNotification);
    }
    else
    {
        channelTypeBox.setSelectedId(static_cast<int>(processorRef.getChannelType()) + 1, juce::dontSendNotification);
    }
    
    channelTypeBox.onChange = [this] {
        int sel = channelTypeBox.getSelectedId();
        if (sel == 999)
        {
            // "Add Custom..." — show text input
            auto* te = new juce::TextEditor();
            te->setFont(juce::Font(juce::FontOptions(12.0f)));
            te->setTextToShowWhenEmpty("Type instrument name...", C::text3);
            te->setBounds(channelTypeBox.getX(), channelTypeBox.getBottom() + 2, channelTypeBox.getWidth() + 40, 24);
            te->setColour(juce::TextEditor::backgroundColourId, C::bg3);
            te->setColour(juce::TextEditor::textColourId, C::text);
            te->setColour(juce::TextEditor::outlineColourId, C::purple);
            te->setColour(juce::TextEditor::focusedOutlineColourId, C::purple);
            addAndMakeVisible(te);
            te->toFront(true);
            te->grabKeyboardFocus();
            te->onReturnKey = [this, te]() {
                auto name = te->getText().trim();
                if (name.isNotEmpty()) {
                    processorRef.setCustomChannelName(name);
                    processorRef.setChannelType(ChannelType::Other);
                    addCustomChannelToList(name);
                    rebuildChannelTypeBox();
                }
                processorRef.setChannelTypePromptDismissed(true);
                updateChannelPromptVisibility();
                juce::MessageManager::callAsync([te]() { delete te; });
                resized();
            };
            te->onFocusLost = [this, te]() {
                auto name = te->getText().trim();
                if (name.isNotEmpty()) {
                    processorRef.setCustomChannelName(name);
                    processorRef.setChannelType(ChannelType::Other);
                    addCustomChannelToList(name);
                    rebuildChannelTypeBox();
                }
                juce::MessageManager::callAsync([te]() { delete te; });
            };
        }
        else if (sel >= 1000)
        {
            // Custom channel selected
            int idx = sel - 1000;
            if (idx < customChannelNames.size()) {
                processorRef.setCustomChannelName(customChannelNames[idx]);
                processorRef.setChannelType(ChannelType::Other);
            }
            processorRef.setChannelTypePromptDismissed(true);
            updateChannelPromptVisibility();
            resized();
        }
        else if (sel > 0)
        {
            processorRef.setChannelType(static_cast<ChannelType>(sel - 1));
            processorRef.setChannelTypePromptDismissed(true);
            updateChannelPromptVisibility();
            resized();
        }
    };
}

void EchoJayEditor::addCustomChannelToList(const juce::String& name)
{
    if (name.isEmpty()) return;
    // Don't add duplicates
    for (auto& existing : customChannelNames)
        if (existing.equalsIgnoreCase(name)) return;
    customChannelNames.add(name);
    customChannelNames.sort(true);
    saveCustomChannels();
}

void EchoJayEditor::loadCustomChannels()
{
    auto file = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile("EchoJay").getChildFile("custom_channels.txt");
    if (file.existsAsFile())
    {
        customChannelNames.clear();
        customChannelNames.addTokens(file.loadFileAsString(), "\n", "");
        customChannelNames.removeEmptyStrings();
        customChannelNames.trim();
    }
}

void EchoJayEditor::saveCustomChannels()
{
    auto folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                      .getChildFile("EchoJay");
    folder.createDirectory();
    folder.getChildFile("custom_channels.txt").replaceWithText(customChannelNames.joinIntoString("\n"));
}

// ============================================================================
// Genre Box (submenu-based, like channel type box)
// ============================================================================

void EchoJayEditor::rebuildGenreBox()
{
    genreBox.clear(juce::dontSendNotification);
    auto* root = genreBox.getRootMenu();

    // ID scheme: built-in genres use index + 1
    // Custom genres use 500 + index
    // "Add Custom..." uses 999

    struct GenreSubGroup { const char* name; juce::StringArray items; };
    GenreSubGroup groups[] = {
        { "Popular",     { "Hip-Hop", "Pop", "R&B", "Rap", "Trap", "Drill" } },
        { "Electronic",  { "EDM", "House", "Techno", "Drum & Bass", "Dubstep", "Trance", "Garage", "Bass / Dub" } },
        { "Rock & Alt",  { "Rock", "Indie", "Punk", "Metal", "Alt Rock", "Grunge" } },
        { "Other",       { "Jazz", "Classical", "Country", "Reggae", "Soul", "Funk", "Gospel", "Blues",
                           "Lo-Fi", "Ambient", "Latin", "Afrobeat", "Dancehall",
                           "Grime", "Phonk", "Jersey Club" } }
    };

    int id = 1;
    for (auto& g : groups)
    {
        juce::PopupMenu sub;
        for (auto& item : g.items)
        {
            sub.addItem(id, item);
            ++id;
        }
        root->addSubMenu(g.name, sub);
    }

    // Custom genres section
    if (!customGenreNames.isEmpty())
    {
        root->addSeparator();
        juce::PopupMenu customSub;
        for (int i = 0; i < customGenreNames.size(); ++i)
            customSub.addItem(500 + i, customGenreNames[i]);
        root->addSubMenu("Custom", customSub);
    }

    // Add new custom option
    root->addSeparator();
    root->addItem(999, "Add Custom...");

    // Restore selection from processor state
    juce::String savedGenre = processorRef.getGenre();

    genreBox.onChange = nullptr; // suppress while restoring

    // Check built-in genres
    int checkId = 1;
    bool found = false;
    for (auto& g : groups)
    {
        for (auto& item : g.items)
        {
            if (item.compareIgnoreCase(savedGenre) == 0)
            {
                genreBox.setSelectedId(checkId, juce::dontSendNotification);
                found = true;
                break;
            }
            ++checkId;
        }
        if (found) break;
    }

    // Check custom genres
    if (!found)
    {
        for (int i = 0; i < customGenreNames.size(); ++i)
        {
            if (customGenreNames[i].compareIgnoreCase(savedGenre) == 0)
            {
                genreBox.setSelectedId(500 + i, juce::dontSendNotification);
                found = true;
                break;
            }
        }
    }

    if (!found && savedGenre.isNotEmpty())
        genreBox.setText(savedGenre, juce::dontSendNotification);
    else if (!found)
        genreBox.setSelectedId(1, juce::dontSendNotification); // Hip-Hop default

    genreBox.onChange = [this] {
        int sel = genreBox.getSelectedId();
        if (sel == 999)
        {
            // "Add Custom..." — show text input
            auto* te = new juce::TextEditor();
            te->setFont(juce::Font(juce::FontOptions(12.0f)));
            te->setTextToShowWhenEmpty("Type genre name...", C::text3);
            te->setBounds(genreBox.getBounds().expanded(40, 0).translated(0, 28));
            te->setColour(juce::TextEditor::backgroundColourId, C::bg3);
            te->setColour(juce::TextEditor::textColourId, C::text);
            te->setColour(juce::TextEditor::outlineColourId, C::purple);
            te->setColour(juce::TextEditor::focusedOutlineColourId, C::purple);
            addAndMakeVisible(te);
            te->toFront(true);
            te->grabKeyboardFocus();
            te->onReturnKey = [this, te]() {
                auto name = te->getText().trim();
                if (name.isNotEmpty()) {
                    addCustomGenreToList(name);
                    processorRef.setGenre(name);
                    rebuildGenreBox();
                }
                juce::MessageManager::callAsync([te]() { delete te; });
            };
            te->onFocusLost = [this, te]() {
                auto name = te->getText().trim();
                if (name.isNotEmpty()) {
                    addCustomGenreToList(name);
                    processorRef.setGenre(name);
                    rebuildGenreBox();
                }
                juce::MessageManager::callAsync([te]() { delete te; });
            };
        }
        else
        {
            processorRef.setGenre(genreBox.getText());
        }
    };
}

void EchoJayEditor::addCustomGenreToList(const juce::String& name)
{
    if (name.isEmpty()) return;
    for (auto& existing : customGenreNames)
        if (existing.equalsIgnoreCase(name)) return;
    customGenreNames.add(name);
    customGenreNames.sort(true);
    saveCustomGenres();
}

void EchoJayEditor::loadCustomGenres()
{
    auto file = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile("EchoJay").getChildFile("custom_genres.txt");
    if (file.existsAsFile())
    {
        customGenreNames.clear();
        customGenreNames.addTokens(file.loadFileAsString(), "\n", "");
        customGenreNames.removeEmptyStrings();
        customGenreNames.trim();
    }
}

void EchoJayEditor::saveCustomGenres()
{
    auto folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                      .getChildFile("EchoJay");
    folder.createDirectory();
    folder.getChildFile("custom_genres.txt").replaceWithText(customGenreNames.joinIntoString("\n"));
}

// ============================================================================
// Chat text scaling
// ============================================================================
// Cycles the chat message font size through a small preset list. Stored as a
// multiplier on the base 12pt font (so 1.0 = stock, 1.6 = noticeably bigger).
// Persisted to ~/Documents/EchoJay/chat_text_scale.txt so the user's choice
// survives across sessions and across plugin instances.

static constexpr float kChatTextScalePresets[] = { 1.0f, 1.2f, 1.4f, 1.6f, 0.9f };
static constexpr int kChatTextScalePresetCount = (int)(sizeof(kChatTextScalePresets) / sizeof(float));

void EchoJayEditor::cycleChatTextScale()
{
    // Find current index in the preset list (nearest match) and advance.
    int curIdx = 0;
    float bestDelta = 1e9f;
    for (int i = 0; i < kChatTextScalePresetCount; ++i)
    {
        float d = std::abs(kChatTextScalePresets[i] - chatTextScale);
        if (d < bestDelta) { bestDelta = d; curIdx = i; }
    }
    int next = (curIdx + 1) % kChatTextScalePresetCount;
    chatTextScale = kChatTextScalePresets[next];
    saveChatTextScale();

    // Recompute chat content height immediately (so the scroll range is right)
    // and trigger a full repaint. The timer would catch this on its next tick
    // but doing it now avoids a visible jump.
    resized();
    repaint();
}

void EchoJayEditor::loadChatTextScale()
{
    auto file = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile("EchoJay").getChildFile("chat_text_scale.txt");
    if (file.existsAsFile())
    {
        float v = file.loadFileAsString().trim().getFloatValue();
        if (v >= 0.7f && v <= 2.5f) // sanity clamp against junk on disk
            chatTextScale = v;
    }
}

void EchoJayEditor::saveChatTextScale()
{
    auto folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                      .getChildFile("EchoJay");
    folder.createDirectory();
    folder.getChildFile("chat_text_scale.txt").replaceWithText(juce::String(chatTextScale, 2));
}

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
    if (prevId > 1 && (prevId - 2) < presetNames.size())
        prevPath = presetNames[prevId - 2];
    
    presetNames.clear();
    presetBox.clear(juce::dontSendNotification);
    
    // First item: Clear All
    presetBox.addItem("Clear All", 1);
    presetBox.addSeparator();
    
    auto folder = getPresetsFolder();
    auto files = folder.findChildFiles(juce::File::findFiles, false, "*.json");
    files.sort();
    
    int id = 2;  // offset by 1 for Clear All
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

// ============ Keyboard Shortcuts ============

bool EchoJayEditor::keyPressed(const juce::KeyPress& key)
{
    // Don't handle keys when typing in text editors
    if (chatInput.hasKeyboardFocus(false) || emailInput.hasKeyboardFocus(false) ||
        passwordInput.hasKeyboardFocus(false) || settingsName.hasKeyboardFocus(false) ||
        settingsMonitors.hasKeyboardFocus(false) || settingsHeadphones.hasKeyboardFocus(false) ||
        settingsGenres.hasKeyboardFocus(false) || settingsPlugins.hasKeyboardFocus(false) ||
        reviewSearchBox.hasKeyboardFocus(false) || settingsPluginSearchBox.hasKeyboardFocus(false))
        return false;


    // Spacebar — stop capture or toggle AB playback
    if (key == juce::KeyPress::spaceKey && currentScreen == Screen::Main
        && !channelPromptVisible && !genrePromptVisible)
    {
        auto s = processorRef.getCaptureState();
        if (s == CaptureState::Capturing)
        {
            processorRef.stopCapture();
            return true;
        }
        
        // Toggle AB playback with spacebar
        if (processorRef.abActive.load())
        {
            if (processorRef.abPlayingRef.load()) {
                processorRef.pauseAB();
                currentlyPlayingChatWav.clear();
            } else {
                processorRef.resumeAB();
                currentlyPlayingChatWav = processorRef.abFilePath;
            }
            repaint();
            return true;
        }
    }

    return false;
}

void EchoJayEditor::mouseDown(const juce::MouseEvent& e)
{
    auto pos = e.getEventRelativeTo(this).getPosition();

    // Update overlay clicks are handled by the UpdateOverlay child component itself.

    // Tab bar click — y=32..60 (below the 32px header, height=kTabBarH)
    if (currentScreen == Screen::Main && !visualOnlyMode
        && pos.y >= 32 && pos.y < 32 + kTabBarH)
    {
        constexpr int kTabCount = 7;
        int tabW = getWidth() / kTabCount;
        int idx = juce::jlimit(0, kTabCount - 1, pos.x / juce::jmax(1, tabW));
        switchToTab(static_cast<Tab>(idx));
        return;
    }

    // Chat wave card click — direct hit testing (works on Windows where overlays fail)
    if (currentScreen == Screen::Main && !chatWavePositions.empty())
    {
        for (int i = 0; i < (int)chatWavePositions.size(); ++i)
        {
            auto& wp = chatWavePositions[(size_t)i];
            if (wp.bounds.contains(pos))
            {
                int localX = pos.x - wp.bounds.getX();
                int playBtnArea = 30;
                if (localX <= playBtnArea)
                {
                    // Find matching index in wavePlayPaths
                    for (int j = 0; j < activeWavePlayBtns; ++j)
                    {
                        if (wavePlayPaths[(size_t)j] == wp.wavPath)
                        {
                            onWavePlayClick(j);
                            break;
                        }
                    }
                }
                else
                {
                    // Seek
                    int wfStart = playBtnArea;
                    int wfWidth = wp.bounds.getWidth() - wfStart - 6;
                    if (wfWidth > 0)
                    {
                        float frac = juce::jlimit(0.0f, 1.0f, (float)(localX - wfStart) / (float)wfWidth);
                        for (int j = 0; j < activeWavePlayBtns; ++j)
                        {
                            if (wavePlayPaths[(size_t)j] == wp.wavPath)
                            {
                                onWaveSeekClick(j, frac);
                                break;
                            }
                        }
                    }
                }
                repaint();
                return;
            }
        }
    }
    
    // Visual-only mode — click expand icon to exit
    if (visualOnlyMode && currentScreen == Screen::Main && pos.y < 32 && pos.x > getLocalBounds().getWidth() - 30)
    {
        toggleVisualOnlyMode();
        return;
    }
    
    // Visual-only mode — single toggle button + preset/theme arrows
    if (visualOnlyMode && currentScreen == Screen::Main)
    {
        int stripH = 28;
        int toggleH = 32;
        int toggleY = getHeight() - stripH - toggleH;
        if (!visualMode) toggleY = getHeight() - toggleH;
        int fullW = getWidth();
        
        if (pos.y >= toggleY && pos.y < toggleY + toggleH)
        {
            if (visualMode) {
                // METERS button at left (x=8, w=70)
                if (pos.x < 78) {
                    visualMode = false;
                    resized();
                    repaint();
                    return;
                }
                // Preset/theme — generous split zones
                int arrowsX = 86;
                int arrowsW = fullW - arrowsX - 4;
                int midX = arrowsX + arrowsW / 2;
                
                if (pos.x >= arrowsX && pos.x < fullW) {
                    if (pos.x < midX) {
                        // Preset: left half = prev, right half = next
                        int presetMid = arrowsX + (midX - arrowsX) / 2;
                        if (pos.x < presetMid)
                            particleVisual->prevPreset();
                        else
                            particleVisual->nextPreset();
                        processorRef.visualPreset = (int)particleVisual->currentPreset;
                    } else {
                        // Theme: left half = prev, right half = next
                        int themeMid = midX + (arrowsX + arrowsW - midX) / 2;
                        if (pos.x < themeMid)
                            particleVisual->prevTheme();
                        else
                            particleVisual->nextTheme();
                        processorRef.visualTheme = (int)particleVisual->currentTheme;
                    }
                    repaint();
                    return;
                }
                return;
            } else {
                // Single VISUALISATION button — click anywhere toggles
                visualMode = true;
                resized();
                repaint();
                return;
            }
        }
    }
    
    // Compact/expand toggle — top right of top bar
    if (currentScreen == Screen::Main && !visualOnlyMode && pos.y < 32 && pos.x > getLocalBounds().getWidth() - 30)
    {
        toggleCompactMode();
        return;
    }
    
    // Visual-only mode toggle — diamond icon, second from right in top bar
    if (currentScreen == Screen::Main && !compactMode && !visualOnlyMode
        && pos.y < 32 && pos.x > getLocalBounds().getWidth() - 54 && pos.x <= getLocalBounds().getWidth() - 30)
    {
        toggleVisualOnlyMode();
        return;
    }
    
    // A/B transport bar clicks (bottom bar)
    if (processorRef.abActive.load() && !compactMode && !visualOnlyMode)
    {
        int abBarH = 32;
        int abBarY = getHeight() - abBarH;
        int fullW = getWidth();
        
        if (pos.y >= abBarY && pos.y < abBarY + abBarH)
        {
            // X button to stop
            if (pos.x >= fullW - 24) {
                processorRef.stopAB();
                repaint();
                return;
            }
            // Play/pause button area
            if (pos.x < 32) {
                bool wasRef = processorRef.abPlayingRef.load();
                if (wasRef) {
                    processorRef.pauseAB();
                    currentlyPlayingChatWav.clear();
                } else {
                    processorRef.resumeAB();
                    currentlyPlayingChatWav = processorRef.abFilePath;
                }
                repaint();
                return;
            }
            // Sync button area (right after play button)
            if (pos.x >= 32 && pos.x < 68) {
                processorRef.abSyncToDAW.store(!processorRef.abSyncToDAW.load());
                repaint();
                return;
            }
            // Waveform click — seek to position
            int wfX = 170;
            int wfW = fullW - wfX - 28;
            if (pos.x >= wfX && pos.x < wfX + wfW && wfW > 0 && processorRef.abSampleCount > 0) {
                float seekFrac = (float)(pos.x - wfX) / (float)wfW;
                int seekPos = (int)(seekFrac * (float)processorRef.abSampleCount);
                processorRef.abPlaybackPos = juce::jlimit(0, processorRef.abSampleCount - 1, seekPos);
                if (!processorRef.abPlayingRef.load())
                    processorRef.resumeAB();
                repaint();
                return;
            }
        }
    }
    
    // Preset/theme selector strip click — VISUALISATION tab only
    if (currentScreen == Screen::Main && currentTab == Tab::Visualisation
        && visualMode && !compactMode && !visualOnlyMode
        && !channelPromptVisible && !genrePromptVisible)
    {
        int paintCW = juce::jlimit(280, 420, getWidth() * 35 / 100);
        int mW2 = getWidth() - paintCW;
        int numStripH = 28;
        int stripH = 30;
        int abOff5 = abBarShowing ? kAbBarH : 0;
        int stripY = getHeight() - numStripH - stripH - abOff5;

        if (pos.x < mW2 && pos.y >= stripY && pos.y < stripY + stripH)
        {
            int arrowsX = 4;
            int arrowsW = mW2 - arrowsX - 4;
            int midX = arrowsX + arrowsW / 2;

            if (pos.x < midX) {
                // Preset: left of centre = prev, right of centre = next
                int presetMid = arrowsX + (midX - arrowsX) / 2;
                if (pos.x < presetMid) particleVisual->prevPreset();
                else                   particleVisual->nextPreset();
                processorRef.visualPreset = (int)particleVisual->currentPreset;
            } else {
                // Theme: left of centre = prev, right of centre = next
                int themeMid = midX + (arrowsX + arrowsW - midX) / 2;
                if (pos.x < themeMid) particleVisual->prevTheme();
                else                  particleVisual->nextTheme();
                processorRef.visualTheme = (int)particleVisual->currentTheme;
            }
            repaint();
            return;
        }
    }

    // Click on loudness panel — reset integrated LUFS
    if (currentScreen == Screen::Main && currentView == View::Meters && !compactMode
        && !channelPromptVisible && !genrePromptVisible
        && loudnessPanelBounds.contains(pos))
    {
        processorRef.getMeterEngine().resetIntegrated();
        repaint();
        return;
    }
    
    // Logo click — open landing page
    if (currentScreen == Screen::Main && pos.x < 120 && pos.y < 32)
    {
        if (e.mods.isPopupMenu())
        {
            // Right-click on logo/top bar — UI size menu
            juce::PopupMenu sizeMenu;
            sizeMenu.setLookAndFeel(&lnf);
            
            sizeMenu.addItem(2, "Default (900 x 580)");
            sizeMenu.addItem(3, "Large (1080 x 696)");
            sizeMenu.addItem(4, "Extra Large (1260 x 812)");
            sizeMenu.showMenuAsync(juce::PopupMenu::Options(),
                [this](int result) {
                    if (result == 2) setSize(900, 580);
                    else if (result == 3) setSize(1080, 696);
                    else if (result == 4) setSize(1260, 812);
                });
            return;
        }
        juce::URL("https://www.echojay.ai/?noredirect").launchInDefaultBrowser();
        return;
    }
    
    // Right-click anywhere on top bar — also show size menu
    if (currentScreen == Screen::Main && pos.y < 32 && e.mods.isPopupMenu())
    {
        juce::PopupMenu sizeMenu;
        sizeMenu.setLookAndFeel(&lnf);
        
        sizeMenu.addItem(2, "Default (900 x 580)");
        sizeMenu.addItem(3, "Large (1080 x 696)");
        sizeMenu.addItem(4, "Extra Large (1260 x 812)");
        sizeMenu.showMenuAsync(juce::PopupMenu::Options(),
            [this](int result) {
                if (result == 2) setSize(900, 580);
                else if (result == 3) setSize(1080, 696);
                else if (result == 4) setSize(1260, 812);
            });
        return;
    }

    if (currentView == View::Compare)
    {
        
        // Right-click on card area — rename/delete pass
        if (e.mods.isPopupMenu())
        {
            auto snaps = processorRef.getSnapshots();
            int refOffset = (int)snaps.size() + 100;
            
            // Determine which card (A or B) was clicked based on mouse position
            struct SlotInfo { juce::ComboBox* box; int id; };
            std::vector<SlotInfo> slotsToCheck;
            
            // Use card positions: nearer to slot A or slot B
            int distA = std::abs(pos.x - compareSlotABox.getBounds().getCentreX());
            int distB = std::abs(pos.x - compareSlotBBox.getBounds().getCentreX());
            if (distA <= distB)
                slotsToCheck.push_back({ &compareSlotABox, compareSlotABox.getSelectedId() });
            else
                slotsToCheck.push_back({ &compareSlotBBox, compareSlotBBox.getSelectedId() });
            
            for (auto& slot : slotsToCheck)
            {
                int sel = slot.id;
                if (sel <= 0 || sel >= refOffset) continue;
                if ((sel - 1) >= (int)snaps.size()) continue;
                
                int idx = sel - 1;
                juce::PopupMenu menu;
                menu.setLookAndFeel(&lnf);
                menu.addItem(1, "Rename \"" + snaps[(size_t)idx].name + "\"");
                menu.addItem(2, "Delete Pass");
                auto* targetBox = slot.box;
                menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(*targetBox),
                    [this, idx, targetBox](int result) {
                        if (result == 1) {
                            auto snaps2 = processorRef.getSnapshots();
                            if (idx < (int)snaps2.size()) {
                                auto* te = new juce::TextEditor();
                                te->setFont(juce::Font(juce::FontOptions(12.0f)));
                                te->setText(snaps2[(size_t)idx].name);
                                te->selectAll();
                                te->setBounds(targetBox->getX(), targetBox->getBottom() + 4, targetBox->getWidth(), 24);
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

void EchoJayEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    auto pos = e.getEventRelativeTo(this).getPosition();
    
    if (currentView != View::Compare) return;
    
    auto snaps = processorRef.getSnapshots();
    int refOffset = (int)snaps.size() + 100;
    
    // Determine which card by checking if click is nearer to slot A or slot B
    int distA = std::abs(pos.x - compareSlotABox.getBounds().getCentreX());
    int distB = std::abs(pos.x - compareSlotBBox.getBounds().getCentreX());
    auto& box = (distA <= distB) ? compareSlotABox : compareSlotBBox;
    int sel = box.getSelectedId();
    
    if (sel <= 0 || sel >= refOffset) return;
    if ((sel - 1) >= (int)snaps.size()) return;
    
    int idx = sel - 1;
    auto* te = new juce::TextEditor();
    te->setFont(juce::Font(juce::FontOptions(12.0f)));
    te->setText(snaps[(size_t)idx].name);
    te->selectAll();
    // Position rename field directly below the clicked card's dropdown
    te->setBounds(box.getX(), box.getBottom() + 4, box.getWidth(), 24);
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

void EchoJayEditor::stopChatPlayback()
{
    if (chatPlaybackProcess != nullptr)
    {
        chatPlaybackProcess->kill();
        chatPlaybackProcess.reset();
    }
    // Pause plugin-routed playback (keeps position for resume)
    if (processorRef.abActive.load())
        processorRef.pauseAB();
    
    currentlyPlayingChatWav.clear();
    chatPlaybackStartTime = 0;
    // Don't clear duration — needed for resume offset calculation
    chatPlaybackOffset = 0;
    playSlotABtn.setButtonText(">");
    playSlotBBtn.setButtonText(">");
}

void EchoJayEditor::toggleCompactMode()
{
    compactMode = !compactMode;
    
    if (compactMode)
    {
        // Save current size so we can restore it
        fullModeWidth = getWidth();
        fullModeHeight = getHeight();
        
        // Switch to compact — chat only
        setResizeLimits(420, 500, 600, 900);
        setSize(450, 550);
        
        // Hide meter-side UI and force back to meters view
        compareBtn.setVisible(false);
        settingsBtn.setVisible(false);
        scanBtn.setVisible(false);
        if (currentView == View::Compare) { hideCompareView(); currentView = View::Meters; }
        if (currentView == View::Settings) { hideSettingsView(); currentView = View::Meters; }
    }
    else
    {
        // Restore full mode
        setResizeLimits(900, 580, 1800, 1200);
        setSize(fullModeWidth, fullModeHeight);
        
        compareBtn.setVisible(true);
        settingsBtn.setVisible(true);
        scanBtn.setVisible(true);
    }
    
    resized();
    repaint();
}

void EchoJayEditor::toggleVisualMode()
{
    visualMode = !visualMode;
    
    if (visualMode) {
        if (currentView == View::Compare) { hideCompareView(); currentView = View::Meters; }
        if (currentView == View::Settings) { hideSettingsView(); currentView = View::Meters; }
    }
    
    processorRef.visualModeOn = visualMode;
    
    // Attach/detach the OpenGL context to match the toggle. Saves GPU
    // cycles when visuals are off, and means a Windows user who's hit
    // the freeze can recover by toggling off (they'd already need a way
    // in via Settings, but at least it's recoverable).
    if (particleVisual != nullptr)
    {
        if (visualMode) particleVisual->start();
        else            particleVisual->stop();
    }
    
    resized();
    repaint();
}

void EchoJayEditor::toggleVisualOnlyMode()
{
    visualOnlyMode = !visualOnlyMode;
    
    if (visualOnlyMode)
    {
        // Save current size
        visualOnlyWidth = getWidth();
        visualOnlyHeight = getHeight();
        
        visualMode = true;
        
        // Force meters view
        if (currentView == View::Compare) { hideCompareView(); currentView = View::Meters; }
        if (currentView == View::Settings) { hideSettingsView(); currentView = View::Meters; }
        
        // Calculate meter panel width (same as normal layout)
        int chatW = juce::jlimit(240, 380, getWidth() * 32 / 100);
        int mW = getWidth() - chatW;
        
        // Shrink window to just the visual panel width
        setResizeLimits(400, 450, 1200, 1200);
        setSize(mW, getHeight());
        
        // Hide chat and meter-only UI (keep capture button visible)
        compareBtn.setVisible(false);
        settingsBtn.setVisible(false);
        scanBtn.setVisible(false);
        channelTypeBox.setVisible(false);
        genreBox.setVisible(false);
        chatInput.setVisible(false);
        chatSendBtn.setVisible(false);
        chatTextSizeBtn.setVisible(false);
        chatScroll.setVisible(false);
    }
    else
    {
        // Restore full mode
        setResizeLimits(900, 580, 1800, 1200);
        setSize(visualOnlyWidth, visualOnlyHeight);
        
        captureBtn.setVisible(true);
        if (!compactMode) {
            compareBtn.setVisible(true);
            settingsBtn.setVisible(true);
            scanBtn.setVisible(true);
        }
        channelTypeBox.setVisible(true);
        genreBox.setVisible(true);
        chatInput.setVisible(true);
        chatSendBtn.setVisible(true);
        chatTextSizeBtn.setVisible(true);
        chatScroll.setVisible(true);
    }
    
    resized();
    repaint();
}

void EchoJayEditor::startChatPlayback(const juce::String& wavPath, float offset)
{
    stopChatPlayback();

    // Helper: find duration from chat messages (matches wavFilePath or audioUrl)
    auto findDuration = [&]() -> float {
        for (auto& msg : chatMessages)
            if (msg.wavFilePath == wavPath || msg.audioUrl == wavPath)
                return msg.durationSeconds;
        for (auto& wp : compareWavePositions)
            if (wp.wavPath == wavPath)
                return wp.duration;
        return 0.f;
    };

    juce::File wavFile(wavPath);
    if (!wavFile.existsAsFile()) return;

    // Route playback through the plugin output (AB system) on all views
    {
        // If same file is paused and no seek offset, resume from where we paused
        if (processorRef.abPaused.load() && processorRef.abFilePath == wavPath && offset < 0.1f)
        {
            processorRef.resumeAB();
        }
        else
        {
            processorRef.loadABFile(wavPath, (double)offset);
        }
        currentlyPlayingChatWav = wavPath;
        chatPlaybackStartTime = juce::Time::getMillisecondCounterHiRes();
        chatPlaybackOffset = offset;
        chatPlaybackDuration = findDuration();
        return;
    }
    
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
        chatPlaybackDuration = findDuration();

        // Auto-clear after remaining duration
        float remaining = chatPlaybackDuration - offset;
        if (remaining > 0)
        {
            auto safeThis = juce::Component::SafePointer<EchoJayEditor>(this);
            juce::Timer::callAfterDelay((int)(remaining * 1000) + 500, [safeThis]() {
                if (safeThis == nullptr)
                    return;
                if (safeThis->chatPlaybackProcess != nullptr && !safeThis->chatPlaybackProcess->isRunning())
                    safeThis->stopChatPlayback();
                safeThis->repaint();
            });
        }
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

    // Web capture — open echojay.ai in the browser instead of playing
    if (wavPath == "__open_web__")
    {
        juce::URL("https://www.echojay.ai").launchInDefaultBrowser();
        return;
    }

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
