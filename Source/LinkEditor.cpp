#include "LinkEditor.h"
#include "EchoJayLogo.h"

static const juce::Colour kBg     { 0xff0A0C18 };
static const juce::Colour kCard   { 0xff161B22 };
static const juce::Colour kBorder { 0xff30363D };
static const juce::Colour kText   { 0xffE6EDF3 };
static const juce::Colour kText2  { 0xff8B949E };
static const juce::Colour kCyan   { 0xff22d3ee };

LinkEditor::LinkEditor(LinkProcessor& p)
    : AudioProcessorEditor(&p), proc(p), chainPanel(p)
{
    // Resizable, sized to host plugin UIs — same conventions as the main
    // plugin. Size persists in the processor state.
    setResizable(true, true);
    setResizeLimits(900, 580, 1800, 1200);
    setSize(juce::jlimit(900, 1800, proc.editorW),
            juce::jlimit(580, 1200, proc.editorH));

    // Tooltip styling — Link has no custom LookAndFeel, so match the main
    // plugin's dark navy / cyan tooltip look via the colour ids the default
    // LookAndFeel's drawTooltip uses
    tooltipWindow_.setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xff0A0C18));
    tooltipWindow_.setColour(juce::TooltipWindow::textColourId,       juce::Colour(0xfff0f0f5));
    tooltipWindow_.setColour(juce::TooltipWindow::outlineColourId,    juce::Colour(0xff22d3ee).withAlpha(0.35f));

    // Name field
    nameField.setText(proc.linkName, juce::dontSendNotification);
    nameField.setFont(juce::Font(juce::FontOptions(13.0f)));
    nameField.setColour(juce::TextEditor::backgroundColourId, kCard);
    nameField.setColour(juce::TextEditor::textColourId, kText);
    nameField.setColour(juce::TextEditor::outlineColourId, kBorder);
    nameField.setColour(juce::TextEditor::focusedOutlineColourId, kCyan);
    nameField.setTextToShowWhenEmpty("Name this link (e.g. Drums)",
                                     kText2.withAlpha(0.6f));
    nameField.onTextChange = [this]
    {
        proc.linkName = nameField.getText();
        proc.updateShmState();
        repaint();
    };
    addAndMakeVisible(nameField);

    // Toggle. onClick, NOT onStateChange: onStateChange also fires on
    // hover/press, so a toggle whose VISIBLE state had gone stale (state
    // restored while the editor was open) would write the stale value back
    // into the processor on a mere mouse-over — a re-activation path.
    toggleBtn.setToggleState(proc.linkOn.load(), juce::dontSendNotification);
    EchoJay_NSLog(("EJLinkState: editor open, toggle set to "
                   + juce::String((int)proc.linkOn.load())).toRawUTF8());
    toggleBtn.setColour(juce::ToggleButton::textColourId, kText);
    toggleBtn.setColour(juce::ToggleButton::tickColourId, kCyan);
    toggleBtn.setColour(juce::ToggleButton::tickDisabledColourId, kText2);
    toggleBtn.onClick = [this]
    {
        EchoJay_NSLog(("EJLinkState: editor toggle -> "
                       + juce::String((int)toggleBtn.getToggleState())).toRawUTF8());
        proc.linkOn.store(toggleBtn.getToggleState());
        proc.updateShmState();
        repaint();
    };
    addAndMakeVisible(toggleBtn);

    // Gain slider — range/step/default, double-click to 0, " dB" readout.
    gainSlider.setRange(LinkProcessor::kGainMinDb, LinkProcessor::kGainMaxDb, 0.1);
    gainSlider.setValue(proc.getGainDb(), juce::dontSendNotification);
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setNumDecimalPlacesToDisplay(1);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 22);
    gainSlider.setColour(juce::Slider::backgroundColourId, kBorder);
    gainSlider.setColour(juce::Slider::trackColourId, kCyan.withAlpha(0.55f));
    gainSlider.setColour(juce::Slider::thumbColourId, kCyan);
    gainSlider.setColour(juce::Slider::textBoxTextColourId, kText);
    gainSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    gainSlider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    gainSlider.setTooltip("Link gain (post-chain). Double-click to reset to 0 dB.");
    gainSlider.onValueChange = [this]
    {
        // Glide (snapSmoothing=false): a live drag must not zipper
        proc.setGainDb((float)gainSlider.getValue());
    };
    addAndMakeVisible(gainSlider);

    gainCaption.setText("GAIN", juce::dontSendNotification);
    gainCaption.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
    gainCaption.setColour(juce::Label::textColourId, kText2);
    gainCaption.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(gainCaption);

    // Placement — header button opens the chooser; prompt buttons apply it
    placementBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff141626));
    placementBtn.setColour(juce::TextButton::textColourOffId, kText2);
    placementBtn.onClick = [this] { showPlacementChooser(); };
    addAndMakeVisible(placementBtn);
    updatePlacementBtn();

    // Placement prompt overlay — the whole prompt is this one child component,
    // added last so it sits above the header + chain panel. Its own paint()
    // owns the scrim/card; LinkEditor::paint never draws prompt pixels.
    placementPrompt.onChoose = [this](int c)
    {
        applyPlacement(c == 1 ? LinkProcessor::PlacementBus
                              : LinkProcessor::PlacementInsert);
    };
    addChildComponent(placementPrompt);

    // First-use prompt: ask once while placement is unset
    placementPromptVisible = (proc.getPlacement() == LinkProcessor::PlacementUnset);

    // Keep the toggle/name/gain in sync when the processor state changes
    // OUTSIDE this editor (project state restore, remote Active/gain command)
    proc.onLinkStateChanged = [safe = juce::Component::SafePointer<LinkEditor>(this)]
    {
        if (safe == nullptr) return;
        safe->toggleBtn.setToggleState(safe->proc.linkOn.load(), juce::dontSendNotification);
        safe->nameField.setText(safe->proc.linkName, juce::dontSendNotification);
        safe->gainSlider.setValue(safe->proc.getGainDb(), juce::dontSendNotification);
        safe->updatePlacementBtn();   // remote/restore placement change
        // Re-derive the prompt state from the (possibly remotely-set) placement
        // and re-run layout so child visibility matches BEFORE the repaint. A
        // placement set from outside (e.g. the main plugin's suggested target)
        // used to leave placementPromptVisible and the header controls out of
        // sync, so the prompt copy ghosted behind the Active toggle / placement
        // button. Keeping the flag, layout and paint consistent — with a full
        // repaint() that clears the whole surface — prevents any stray text.
        const bool wantPrompt =
            (safe->proc.getPlacement() == LinkProcessor::PlacementUnset);
        if (wantPrompt != safe->placementPromptVisible)
        {
            safe->placementPromptVisible = wantPrompt;
            safe->resized();
        }
        safe->repaint();
    };

    // Chain panel fills everything below the header
    addAndMakeVisible(chainPanel);
    chainPanel.onAddClick = [this] { showChainPluginPicker(); };
    chainPanel.rebuild();

    // Editors must close BEFORE the processor tears slots down
    proc.onChainAboutToChange = [safe = juce::Component::SafePointer<LinkEditor>(this)]
    {
        if (safe != nullptr) safe->chainPanel.closeAllEditors();
    };
    proc.onChainModelChanged = [safe = juce::Component::SafePointer<LinkEditor>(this)]
    {
        if (safe != nullptr) safe->chainPanel.rebuild();
    };

    // Poll the mono fold-down note at a low rate (repaints only on change).
    lastMonoNote_ = proc.getMonoFoldNote();
    startTimer(250);

    // Lay out now that every child (incl. the placement overlay, added before
    // the chain panel) exists, so the overlay's bounds / z-order / visibility
    // are correct on first show.
    resized();
}

LinkEditor::~LinkEditor()
{
    stopTimer();
    proc.onChainModelChanged = nullptr;
    proc.onChainAboutToChange = nullptr;
    proc.onLinkStateChanged = nullptr;
    chainPanel.closeAllEditors();
}

void LinkEditor::timerCallback()
{
    // Repaint only when the mono fold-down note appears/disappears/changes, so
    // steady-state playback never triggers a repaint (which could flicker the
    // hosted native editor). Both status lines (mini in paint(), full in the
    // chain panel) refresh.
    auto note = proc.getMonoFoldNote();
    if (note != lastMonoNote_)
    {
        lastMonoNote_ = note;
        repaint();
        chainPanel.repaint();
    }
}

void LinkEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBg);

    // ---- Header row ----
    // Fill the header band with the app background (same near-black the main
    // plugin uses behind its logo) — the previous lighter card fill lowered the
    // wordmark's contrast and read as a dimmed logo.
    g.setColour(kBg);
    g.fillRect(0, 0, getWidth(), kHeaderH);
    g.setColour(kBorder);
    g.drawHorizontalLine(kHeaderH, 0.f, (float)getWidth());

    juce::Image logo = juce::ImageCache::getFromMemory(echoJayLogoPNG,
                                                        (int)echoJayLogoPNGSize);
    const float logoX = 12.0f;
    if (logo.isValid())
    {
        // Match the main plugin's drawLogo treatment exactly: full opacity,
        // height*0.8, high-quality stretchToFit float draw (not the int overload).
        float aspect = (float)logo.getWidth() / (float)logo.getHeight();
        float logoH  = kHeaderH * 0.8f;
        float logoW  = logoH * aspect;
        float logoY  = (kHeaderH - logoH) * 0.5f;
        g.setOpacity(1.0f);
        g.drawImage(logo, juce::Rectangle<float>(logoX, logoY, logoW, logoH),
                    juce::RectanglePlacement::stretchToFit);
        g.setColour(kCyan);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText("Link", (int)(logoX + logoW + 6.0f), 0, 80, kHeaderH,
                   juce::Justification::centredLeft);
    }

    // Status light
    const juce::Colour lightColour = proc.linkOn.load()
                                   ? juce::Colour(0xff22c55e)
                                   : juce::Colour(0xffef4444);
    g.setColour(lightColour);
    g.fillEllipse(lightBounds);
    g.setColour(lightColour.withAlpha(0.25f));
    g.fillEllipse(lightBounds.expanded(3.0f));

    // Minimise / restore icon — top right of the header (same arrow style
    // as the main plugin's mode toggles)
    {
        int iconX = getWidth() - 24, iconY = (kHeaderH - 16) / 2, s = 16;
        g.setColour(kText2);
        if (miniMode)
        {
            // Expand — arrows outward
            g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + 2, (float)iconY + s - 6, 1.5f);
            g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + 6, (float)iconY + s - 2, 1.5f);
            g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s - 2, (float)iconY + 6, 1.5f);
            g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s - 6, (float)iconY + 2, 1.5f);
        }
        else
        {
            // Minimise — arrows inward
            g.drawLine((float)iconX + 2, (float)iconY + s - 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + s - 2, (float)iconY + 2, (float)iconX + s/2, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + s/2, (float)iconY + s/2, (float)iconX + s/2, (float)iconY + s/2 + 4, 1.5f);
            g.drawLine((float)iconX + s/2, (float)iconY + s/2, (float)iconX + s/2 - 4, (float)iconY + s/2, 1.5f);
            g.drawLine((float)iconX + s/2, (float)iconY + s/2, (float)iconX + s/2, (float)iconY + s/2 - 4, 1.5f);
            g.drawLine((float)iconX + s/2, (float)iconY + s/2, (float)iconX + s/2 + 4, (float)iconY + s/2, 1.5f);
        }
    }

    // ---- Mini body: chain summary + status line, nothing else ----
    if (miniMode)
    {
        auto& model = proc.getChainModel();
        int missing = 0;
        for (auto& sl : model) if (sl.missing) ++missing;
        juce::String summary = model.empty()
            ? "No chain"
            : juce::String((int)model.size()) + " plugin"
              + ((int)model.size() == 1 ? "" : "s");
        if (missing > 0)
            summary += " (" + juce::String(missing) + " missing)";

        g.setColour(kText);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(summary, 16, kHeaderH + 8, getWidth() - 32, 18,
                   juce::Justification::centredLeft);

        juce::String status = chainPanel.statusText;
        if (!proc.chainLayoutSupported())
            status += (status.isEmpty() ? "" : "  |  ")
                    + juce::String("unsupported channel layout (chain bypassed)");
        if (auto note = proc.getMonoFoldNote(); note.isNotEmpty())
            status += (status.isEmpty() ? "" : "  |  ") + note;
        if (status.isEmpty())
            status = proc.linkOn.load() ? "Active - feeding EchoJay"
                                        : "Inactive - capture/meter role dormant";
        g.setColour(kText2);
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.drawText(status, 16, kHeaderH + 30, getWidth() - 32, 14,
                   juce::Justification::centredLeft);
    }

    // ---- Full-view empty-state copy: state plainly what Link measures ----
    // Shown under the header when there's no chain yet (full mode). Always
    // reminds that Link measures at its insert point and belongs on a bus
    // for level work; adds a nudge while placement is still unset.
    if (!miniMode && !placementPromptVisible && proc.getChainModel().empty())
    {
        auto area = juce::Rectangle<int>(0, kHeaderH + 1, getWidth(),
                                         getHeight() - kHeaderH - 1);
        g.setColour(kText);
        g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
        g.drawText("No chain yet", area.getX(), area.getY() + 60, area.getWidth(), 24,
                   juce::Justification::centred);
        g.setColour(kText2);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawFittedText(
            "EchoJay Link measures at its insert point, before the channel fader. "
            "For level work, place it on a bus or aux where its loudness is what "
            "reaches the mix. Use the + button to build a chain here.",
            area.getX() + area.getWidth() / 2 - 210, area.getY() + 92, 420, 60,
            juce::Justification::centredTop, 3);
        if (proc.getPlacement() == LinkProcessor::PlacementUnset)
        {
            g.setColour(juce::Colour(0xfff59e0b));
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            g.drawText("Tip: set this Link's placement in the header.",
                       area.getX(), area.getY() + 156, area.getWidth(), 16,
                       juce::Justification::centred);
        }
    }

    // The placement prompt is a dedicated child overlay (placementPrompt) — it
    // paints itself, never here, so no prompt pixels can survive in the header.
}

void LinkEditor::resized()
{
    // Persist window size in the processor state — full layout only, the
    // mini window's size must not clobber the saved editor size
    if (!miniMode)
    {
        proc.editorW = getWidth();
        proc.editorH = getHeight();
    }

    // Gain group is right-aligned just left of the minimise icon; the name
    // field takes the slack in the middle so nothing collides at min width.
    const int miniIconX  = getWidth() - 30;
    const int gainSlideW = 150;   // track + " dB" textbox
    const int gainCapW   = 34;
    const int gainRight  = miniIconX - 8;
    const int gainX      = gainRight - gainSlideW;
    const int gainCapX   = gainX - gainCapW - 2;

    const int placeW = 104;   // placement button in the cluster
    int fieldX = 150;
    // Name field ends before the toggle + light + placement + gain cluster.
    int clusterW = 12 + 76 + 10 + 10 + 8 + placeW + 10;
    int fieldW = juce::jlimit(80, 260, gainCapX - clusterW - fieldX - 8);
    nameField.setBounds(fieldX, (kHeaderH - 26) / 2, fieldW, 26);
    toggleBtn.setBounds(fieldX + fieldW + 12, (kHeaderH - 24) / 2, 76, 24);

    const float lightD = 10.0f;
    lightBounds = { (float)(fieldX + fieldW + 12 + 76 + 10),
                    (kHeaderH - lightD) * 0.5f, lightD, lightD };

    if (!miniMode)
    {
        // Full: placement + gain in the header
        const int placeX = fieldX + fieldW + 12 + 76 + 10 + 10 + 8;
        placementBtn.setBounds(placeX, (kHeaderH - 24) / 2, placeW, 24);
        gainCaption.setBounds(gainCapX, (kHeaderH - 22) / 2, gainCapW, 22);
        gainSlider.setBounds(gainX, (kHeaderH - 22) / 2, gainSlideW, 22);
        chainPanel.setBounds(0, kHeaderH + 1, getWidth(), getHeight() - kHeaderH - 1);
    }
    else
    {
        // Mini: header stays name+toggle+light; gain + placement go in the
        // body (the narrow header can't hold them)
        int gy = kHeaderH + 52;
        gainCaption.setBounds(16, gy, gainCapW, 22);
        gainSlider.setBounds(16 + gainCapW + 4, gy,
                             juce::jmax(120, getWidth() - 32 - gainCapW - 4 - placeW - 8), 22);
        placementBtn.setBounds(getWidth() - 16 - placeW, gy, placeW, 24);
    }

    // Placement prompt overlay covers the whole editor and lays out its own
    // cards. Bring it to front (it was added before the chain panel) so it sits
    // above everything, then toggle visibility. Hiding it makes JUCE repaint
    // the region behind it, so the header always returns clean.
    placementPrompt.setBounds(getLocalBounds());
    if (placementPromptVisible)
        placementPrompt.toFront(false);
    placementPrompt.setVisible(placementPromptVisible);

    // Header/chain children are hidden while the prompt is up (it occludes the
    // whole editor); the prompt is the only interactive surface then.
    const bool showMain = !placementPromptVisible;
    nameField.setVisible(showMain);
    toggleBtn.setVisible(showMain);
    gainSlider.setVisible(showMain);
    gainCaption.setVisible(showMain);
    placementBtn.setVisible(showMain);
    if (!miniMode) chainPanel.setVisible(showMain);
}

void LinkEditor::mouseDown(const juce::MouseEvent& e)
{
    if (placementPromptVisible) return;   // prompt is modal
    auto pos = e.getPosition();
    if (pos.y < kHeaderH && pos.x > getWidth() - 30)
    {
        toggleMiniMode();
        return;
    }
}

void LinkEditor::toggleMiniMode()
{
    miniMode = !miniMode;

    if (miniMode)
    {
        miniSavedW_ = getWidth();
        miniSavedH_ = getHeight();
        // Hosted editors (inline or pop-out) close cleanly on entering mini;
        // restore re-opens the selected slot inline via the rebuild path
        chainPanel.closeAllEditors();
        chainPanel.setVisible(false);
        setResizeLimits(340, 120, 700, 160);
        setSize(430, 128);
    }
    else
    {
        setResizeLimits(900, 580, 1800, 1200);
        setSize(miniSavedW_ > 0 ? miniSavedW_ : juce::jlimit(900, 1800, proc.editorW),
                miniSavedH_ > 0 ? miniSavedH_ : juce::jlimit(580, 1200, proc.editorH));
        chainPanel.setVisible(true);
        chainPanel.rebuild();
    }

    resized();
    repaint();
}

void LinkEditor::showChainPluginPicker()
{
    auto& ch = proc.getChainHost();
    auto plugins = ch.getFilteredPlugins("", proc.chainPickerFormat());

    // Respect the Settings checklist — same disabled set builds honour
    juce::Array<juce::PluginDescription> avail;
    for (auto& p : plugins)
        if (!proc.isPluginDisabledByName(p.name))
            avail.add(p);
    if (avail.isEmpty()) return;

    juce::PopupMenu menu;
    menu.addSectionHeader("ADD PLUGIN TO CHAIN");
    for (int i = 0; i < avail.size(); ++i)
    {
        const auto& p = avail.getReference(i);
        bool isAU = p.pluginFormatName == "AudioUnit";
        menu.addItem(i + 1, p.name + "  [" + (isAU ? "AU" : "VST3") + "]");
    }

    auto safeThis = juce::Component::SafePointer<LinkEditor>(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(&chainPanel)
            .withMaximumNumColumns(1)
            .withMinimumNumColumns(1),
        [safeThis, avail](int result)
        {
            if (safeThis == nullptr || result <= 0 || result > avail.size()) return;
            auto desc = avail[result - 1];
            safeThis->chainPanel.statusText = "Loading " + desc.name + "...";
            safeThis->chainPanel.repaint();

            safeThis->proc.addChainPluginManually(desc,
                [safeThis](const juce::String& err)
            {
                if (safeThis == nullptr) return;
                if (err.isNotEmpty())
                {
                    safeThis->chainPanel.statusText = "Failed: " + err;
                    safeThis->chainPanel.repaint();
                    return;
                }
                // notifyChainModel already rebuilt the strip; select the new
                // last slot so its editor opens inline (same as the main "+")
                safeThis->chainPanel.statusText = {};
                safeThis->chainPanel.selectedIdx =
                    (int)safeThis->proc.getChainModel().size() - 1;
                safeThis->chainPanel.rebuild();
            });
        });
}

// ============================================================================
//  Placement declaration (bus vs insert) — see LinkProcessor::Placement
// ============================================================================
void LinkEditor::updatePlacementBtn()
{
    switch (proc.getPlacement())
    {
        case LinkProcessor::PlacementBus:
            placementBtn.setButtonText("On a bus");
            placementBtn.setColour(juce::TextButton::textColourOffId, kCyan);
            break;
        case LinkProcessor::PlacementInsert:
            placementBtn.setButtonText("On a track");
            placementBtn.setColour(juce::TextButton::textColourOffId, kText2);
            break;
        default:
            placementBtn.setButtonText("Set placement");
            placementBtn.setColour(juce::TextButton::textColourOffId,
                                   juce::Colour(0xfff59e0b));   // amber: needs answering
            break;
    }
    placementBtn.setTooltip("Where this Link sits. Link measures at its insert point; "
                            "for level work it belongs on a bus.");
}

void LinkEditor::applyPlacement(int p)
{
    proc.setPlacement(p);
    placementPromptVisible = false;
    updatePlacementBtn();
    resized();
    repaint();
}

void LinkEditor::showPlacementChooser()
{
    juce::PopupMenu m;
    m.addSectionHeader("Where did you put this Link?");
    const int cur = proc.getPlacement();
    m.addItem(1, "On a bus or group",  true, cur == LinkProcessor::PlacementBus);
    m.addItem(2, "On a normal track",  true, cur == LinkProcessor::PlacementInsert);
    auto safeThis = juce::Component::SafePointer<LinkEditor>(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&placementBtn),
        [safeThis](int r)
        {
            if (safeThis == nullptr || r == 0) return;
            safeThis->applyPlacement(r == 1 ? LinkProcessor::PlacementBus
                                            : LinkProcessor::PlacementInsert);
        });
}

// ---------------------------------------------------------------------------
//  PlacementPromptOverlay — owns ALL prompt painting (see the header comment).
// ---------------------------------------------------------------------------
LinkEditor::PlacementPromptOverlay::PlacementPromptOverlay()
{
    auto style = [](juce::TextButton& b)
    {
        b.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        b.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    };
    style(busBtn);
    style(insertBtn);
    busBtn.onClick    = [this] { if (onChoose) onChoose(1); };
    insertBtn.onClick = [this] { if (onChoose) onChoose(2); };
    addAndMakeVisible(busBtn);
    addAndMakeVisible(insertBtn);
    // Modal: catch clicks in the empty scrim (block the header beneath) while
    // still letting the two option buttons receive theirs.
    setInterceptsMouseClicks(true, true);
}

void LinkEditor::PlacementPromptOverlay::resized()
{
    const int w = juce::jmin(480, getWidth() - 60);
    const int h = 340;
    juce::Rectangle<int> card((getWidth() - w) / 2, (getHeight() - h) / 2, w, h);
    auto r = card.reduced(24, 22);
    r.removeFromTop(30);   // title
    r.removeFromTop(38);   // subtitle (2 lines)
    r.removeFromTop(10);
    card1_ = r.removeFromTop(74);
    r.removeFromTop(10);
    card2_ = r.removeFromTop(74);
    busBtn.setBounds(card1_);
    insertBtn.setBounds(card2_);
}

void LinkEditor::PlacementPromptOverlay::paint(juce::Graphics& g)
{
    // Scrim + centred card. Plain-language: ask what the user DID, not signal
    // flow. Two option cards (bold line + explainer), then a dim "not sure".
    g.fillAll(juce::Colour(0xcc0A0C18));

    const int w = juce::jmin(480, getWidth() - 60);
    const int h = 340;
    juce::Rectangle<int> card((getWidth() - w) / 2, (getHeight() - h) / 2, w, h);
    g.setColour(kCard);
    g.fillRoundedRectangle(card.toFloat(), 12.0f);
    g.setColour(kBorder);
    g.drawRoundedRectangle(card.toFloat(), 12.0f, 1.0f);

    auto r = card.reduced(24, 22);
    g.setColour(kText);
    g.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
    g.drawText("Where did you put this Link?", r.removeFromTop(30),
               juce::Justification::centredLeft);
    g.setColour(kText2);
    g.setFont(juce::Font(juce::FontOptions(11.5f)));
    g.drawFittedText("This tells EchoJay whether it can compare this Link's level to "
                     "your other tracks.",
                     r.removeFromTop(38), juce::Justification::topLeft, 2);

    // One option card: bold line + explainer, using the rects from resized()
    auto optionCard = [&](juce::Rectangle<int> cr, const juce::String& bold,
                          const juce::String& explain)
    {
        g.setColour(juce::Colour(0xff141b2e));
        g.fillRoundedRectangle(cr.toFloat(), 8.0f);
        g.setColour(kCyan.withAlpha(0.35f));
        g.drawRoundedRectangle(cr.toFloat(), 8.0f, 1.0f);
        auto ir = cr.reduced(14, 10);
        g.setColour(kCyan);
        g.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
        g.drawText(bold, ir.removeFromTop(20), juce::Justification::centredLeft);
        ir.removeFromTop(2);
        g.setColour(kText2);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawFittedText(explain, ir, juce::Justification::topLeft, 3);
    };
    optionCard(card1_, "On a bus or group",
               "A channel that other tracks are routed into, like a vocal bus, drum "
               "bus, or the master. EchoJay can compare levels here.");
    optionCard(card2_, "On a normal track",
               "Directly on an instrument or audio track, like the lead vocal or the "
               "kick. EchoJay reads the level before your fader, so it will not "
               "compare it to other tracks.");

    g.setColour(kText2.withAlpha(0.6f));
    g.setFont(juce::Font(juce::FontOptions(10.5f)));
    g.drawText("Not sure? Pick 'On a normal track'. You can change this any time "
               "from the Link's header.",
               card.getX() + 24, card2_.getBottom() + 8,
               card.getWidth() - 48, 16, juce::Justification::centredLeft);
}
