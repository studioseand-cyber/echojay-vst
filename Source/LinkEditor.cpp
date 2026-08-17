#include "LinkEditor.h"
#include "ChainPluginPicker.h"   // P13: the searchable "+" picker (shared with the main plugin)
#include "EchoJayLogo.h"
#include "EchoJayFieldStyle.h"   // EchoJayChrome::kFieldCorner (shared field radius)

static const juce::Colour kBg     { 0xff0A0C18 };
static const juce::Colour kCard   { 0xff161B22 };
static const juce::Colour kBorder { 0xff30363D };
static const juce::Colour kText   { 0xffE6EDF3 };
static const juce::Colour kText2  { 0xff8B949E };
static const juce::Colour kCyan   { 0xff22d3ee };

// Rounds a TextEditor's corners to the shared field radius, matching the main
// plugin's Project name box. Reads the editor's own colours, so fill and border
// colour are unchanged — only the corner radius differs from the JUCE default.
struct RoundFieldLnF : juce::LookAndFeel_V4
{
    void fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                  juce::TextEditor& editor) override
    {
        g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle(0.0f, 0.0f, (float)width, (float)height,
                               EchoJayChrome::kFieldCorner);
    }
    void drawTextEditorOutline(juce::Graphics& g, int width, int height,
                               juce::TextEditor& editor) override
    {
        const bool focused = editor.hasKeyboardFocus(true);
        g.setColour(editor.findColour(focused ? juce::TextEditor::focusedOutlineColourId
                                              : juce::TextEditor::outlineColourId));
        auto bounds = juce::Rectangle<float>(0, 0, (float)width, (float)height).reduced(0.5f);
        g.drawRoundedRectangle(bounds, EchoJayChrome::kFieldCorner, focused ? 1.5f : 1.0f);
    }
};

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
    // Vertically centre placeholder + entered text (matches the main plugin's
    // project field): centredLeft centres within (height - topIndent - bottom),
    // and a zero top indent keeps it from sitting high in the field.
    nameField.setJustification(juce::Justification::centredLeft);
    nameField.setIndents(6, 0);
    nameField.setColour(juce::TextEditor::backgroundColourId, kCard);
    nameField.setColour(juce::TextEditor::textColourId, kText);
    nameField.setColour(juce::TextEditor::outlineColourId, kBorder);
    nameField.setColour(juce::TextEditor::focusedOutlineColourId, kCyan);
    // Round the corners to match the main plugin's Project name box (shared
    // radius). Colours/fill/size are untouched — the LnF reads the field's own.
    nameFieldLnF = std::make_unique<RoundFieldLnF>();
    nameField.setLookAndFeel(nameFieldLnF.get());
    // Phase N: the DAW-provided track name becomes the placeholder when the
    // user hasn't typed one — typing still overrides (linkName wins). Kept
    // fresh in timerCallback; the generic hint returns if the host name goes.
    lastPlaceholderHostName_ = proc.getHostTrackName();
    nameField.setTextToShowWhenEmpty(
        lastPlaceholderHostName_.isNotEmpty()
            ? lastPlaceholderHostName_ + " (from DAW)"
            : juce::String("Name this link (e.g. Drums)"),
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

    // Placement — compact header dropdown (Bus / Channel / dim "Placement")
    // plus a "?" popover. No modal prompt; an unset Link is never nagged.
    placementBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff141626));
    placementBtn.setColour(juce::TextButton::textColourOffId, kText2);
    placementBtn.onClick = [this] { showPlacementChooser(); };
    addAndMakeVisible(placementBtn);
    updatePlacementBtn();

    placementHelp.setTooltip("What is a bus vs a channel?");
    placementHelp.onClick = [this] { showPlacementHelp(); };
    addAndMakeVisible(placementHelp);

    // Keep the toggle/name/gain in sync when the processor state changes
    // OUTSIDE this editor (project state restore, remote Active/gain command)
    proc.onLinkStateChanged = [safe = juce::Component::SafePointer<LinkEditor>(this)]
    {
        if (safe == nullptr) return;
        safe->toggleBtn.setToggleState(safe->proc.linkOn.load(), juce::dontSendNotification);
        safe->nameField.setText(safe->proc.linkName, juce::dontSendNotification);
        safe->gainSlider.setValue(safe->proc.getGainDb(), juce::dontSendNotification);
        safe->updatePlacementBtn();   // remote/restore placement change
        safe->repaint();
    };

    // STAGE 1 REMOTE EDITOR: the main plugin asks this Link to raise one of
    // its own hosted editors. Registered here and cleared in the destructor,
    // so its very existence is the "this Link has a window" signal the
    // command needs -- a closed Link leaves the callback null and the ack
    // reports that honestly instead of the main plugin guessing.
    //
    // hostIdx, not model index: the wire carries the RACK slot, which is what
    // the sidecar publishes and what the main plugin's Chain tab is showing.
    proc.onOpenSlotEditor = [safe = juce::Component::SafePointer<LinkEditor>(this)](int hostIdx) -> bool
    {
        if (safe == nullptr) return false;
        const int mi = safe->chainPanel.modelIdxForHostIdx(hostIdx);
        if (mi < 0) return false;              // missing/unresolved slot
        safe->chainPanel.selectSlot(mi);       // selects AND opens its editor
        // Bring the Link's own window forward: the point of the feature is
        // that the editor is reachable, and a raised editor behind another
        // window has not been raised.
        if (auto* top = safe->getTopLevelComponent()) top->toFront(true);
        safe->chainPanel.raiseOpenEditor();
        return true;
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
    nameField.setLookAndFeel(nullptr);   // before nameFieldLnF is destroyed
    stopTimer();
    proc.onChainModelChanged = nullptr;
    proc.onChainAboutToChange = nullptr;
    proc.onLinkStateChanged = nullptr;
    proc.onOpenSlotEditor   = nullptr;   // null again = "no window", see above
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

    // Phase N: host track name can arrive at any time after open (the
    // callback never fires in the ctor) — refresh the placeholder when it
    // changes. Repaint only the field, and only when it is showing the
    // placeholder (empty), so steady state stays repaint-free.
    auto hn = proc.getHostTrackName();
    if (hn != lastPlaceholderHostName_)
    {
        lastPlaceholderHostName_ = hn;
        nameField.setTextToShowWhenEmpty(
            hn.isNotEmpty() ? hn + " (from DAW)"
                            : juce::String("Name this link (e.g. Drums)"),
            kText2.withAlpha(0.6f));
        if (nameField.getText().isEmpty())
            nameField.repaint();
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

        juce::String status = chainPanel.statusText;   // incl. pop-out messages
        if (!proc.chainLayoutSupported())
            status += (status.isEmpty() ? "" : "  |  ")
                    + juce::String("unsupported channel layout (chain bypassed)");
        if (auto note = proc.getMonoFoldNote(); note.isNotEmpty())
            status += (status.isEmpty() ? "" : "  |  ") + note;
        // Only draw when there's a real status (pop-out / unsupported / mono
        // note). The old Active/Inactive role line is removed; when empty the
        // slot is skipped entirely so nothing leaves a gap under "No chain".
        if (status.isNotEmpty())
        {
            g.setColour(kText2);
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            g.drawText(status, 16, kHeaderH + 30, getWidth() - 32, 14,
                       juce::Justification::centredLeft);
        }
    }

    // ---- Full-view empty-state copy: state plainly what Link measures ----
    // Shown under the header when there's no chain yet (full mode). Always
    // reminds that Link measures at its insert point and belongs on a bus
    // for level work; adds a nudge while placement is still unset.
    if (!miniMode && proc.getChainModel().empty())
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

    const int placeW   = 78;   // placement selector ("Bus"/"Channel"/"Placement")
    const int placeGap = 4;    // between the value and the "?" glyph
    const int helpW    = 16;   // subtle inline "?" (no border)
    const int placeCluster = placeW + placeGap + helpW;

    // Name field must start clear of the "EchoJay Link" lockup (logo + word).
    juce::Image logo = juce::ImageCache::getFromMemory(echoJayLogoPNG,
                                                        (int)echoJayLogoPNGSize);
    const float logoW = logo.isValid()
        ? kHeaderH * 0.8f * ((float)logo.getWidth() / (float)logo.getHeight())
        : 90.0f;
    // 12 = logoX, +6 gap, +34 ≈ width of "Link" @13pt bold, +18 clearance margin.
    int fieldX = (int)(12 + logoW + 6) + 34 + 18;

    const int   toggleW  = 76;
    const float lightD   = 10.0f;
    const int   lightGap = 6;    // dot sits snug after the Active label
    const int   groupGap = 16;   // normal spacing before the placement control

    // Reserve space so the name field never pushes the Active/dot/placement
    // group into the gain cluster.
    int clusterW = 12 + toggleW + lightGap + (int)lightD + groupGap + placeCluster + 10;
    int fieldW = juce::jlimit(80, 260, gainCapX - clusterW - fieldX - 8);
    nameField.setBounds(fieldX, (kHeaderH - 26) / 2, fieldW, 26);

    // Active checkbox + label + status dot form ONE group.
    const int toggleX = fieldX + fieldW + 12;
    toggleBtn.setBounds(toggleX, (kHeaderH - 24) / 2, toggleW, 24);
    lightBounds = { (float)(toggleX + toggleW + lightGap),
                    (kHeaderH - lightD) * 0.5f, lightD, lightD };
    const int placeX = toggleX + toggleW + lightGap + (int)lightD + groupGap;

    if (!miniMode)
    {
        // Full: placement selector + "?" + gain in the header
        placementBtn.setBounds(placeX, (kHeaderH - 24) / 2, placeW, 24);
        placementHelp.setBounds(placeX + placeW + placeGap, (kHeaderH - 24) / 2, helpW, 24);
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
                             juce::jmax(120, getWidth() - 32 - gainCapW - 4 - placeCluster - 8), 22);
        placementBtn.setBounds(getWidth() - 16 - placeCluster, gy, placeW, 24);
        placementHelp.setBounds(getWidth() - 16 - helpW, gy, helpW, 24);
    }
}

void LinkEditor::mouseDown(const juce::MouseEvent& e)
{
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

    // P13 (17 Aug 2026): the searchable picker shared with the main plugin
    auto safeThis = juce::Component::SafePointer<LinkEditor>(this);
    ChainPluginPicker::show(chainPanel.addBlock, avail,
        [safeThis](const juce::PluginDescription& desc)
        {
            if (safeThis == nullptr) return;
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
            placementBtn.setButtonText("Bus");
            placementBtn.setColour(juce::TextButton::textColourOffId, kCyan);
            break;
        case LinkProcessor::PlacementInsert:
            // Channel is a deliberate, valid value — same accent as Bus.
            placementBtn.setButtonText("Channel");
            placementBtn.setColour(juce::TextButton::textColourOffId, kCyan);
            break;
        case LinkProcessor::PlacementSend:
            placementBtn.setButtonText("Send");
            placementBtn.setColour(juce::TextButton::textColourOffId, kCyan);
            break;
        default:
            // Only the UNSET state is dim; never amber/nagging. Behaves as
            // Channel until set.
            placementBtn.setButtonText("Placement");
            placementBtn.setColour(juce::TextButton::textColourOffId,
                                   kText2.withAlpha(0.55f));
            break;
    }
    placementBtn.setTooltip("Is this Link on a bus or a channel? Tap '?' for help.");
}

void LinkEditor::applyPlacement(int p)
{
    proc.setPlacement(p);
    updatePlacementBtn();
    repaint();
}

void LinkEditor::showPlacementHelp()
{
    // Non-modal popover (CallOutBox). Sizes itself EXACTLY to its content with
    // even padding all round — no empty gap. Wrapped-paragraph heights are
    // measured with the same TextLayout used to paint them, so nothing clips.
    struct HelpText : juce::Component
    {
        enum { PAD = 14, W = 300, GAP = 8 };
        const juce::String busBody =
            "A channel that other tracks are routed into, like a vocal bus, drum "
            "bus, or the master. EchoJay can compare its level to your other buses.";
        const juce::String chBody =
            "A single instrument or audio track, like the lead vocal or the kick. "
            "EchoJay reads the level before your fader, so it cannot compare it to "
            "other tracks.";
        float busH = 0.0f, chH = 0.0f;

        static juce::TextLayout layoutFor(const juce::String& s, float w)
        {
            juce::AttributedString as;
            as.append(s, juce::Font(juce::FontOptions(11.5f)), kText2);
            juce::TextLayout tl; tl.createLayout(as, w);
            return tl;
        }
        HelpText()
        {
            const float bw = (float)(W - 2 * PAD);
            busH = layoutFor(busBody, bw).getHeight();
            chH  = layoutFor(chBody,  bw).getHeight();
            setSize(W, PAD + 18 + 6 + 15 + (int)std::ceil(busH) + GAP
                        + 15 + (int)std::ceil(chH) + GAP + 15 + PAD);
        }
        void paint(juce::Graphics& g) override
        {
            g.fillAll(kCard);
            auto r = getLocalBounds().reduced(PAD);
            g.setColour(kText);
            g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
            g.drawText("Where is this Link?", r.removeFromTop(18), juce::Justification::topLeft);
            r.removeFromTop(6);
            auto para = [&](const juce::String& lead, const juce::String& body, float bh)
            {
                g.setColour(kCyan);
                g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
                g.drawText(lead, r.removeFromTop(15), juce::Justification::topLeft);
                auto area = r.removeFromTop((int)std::ceil(bh));
                layoutFor(body, (float)area.getWidth()).draw(g, area.toFloat());
                r.removeFromTop(GAP);
            };
            para("Bus", busBody, busH);
            para("Channel", chBody, chH);
            g.setColour(kText2.withAlpha(0.7f));
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawText("Not sure? Leave it on Channel.", r.removeFromTop(15),
                       juce::Justification::topLeft);
        }
    };

    juce::CallOutBox::launchAsynchronously(std::make_unique<HelpText>(),
                                           placementHelp.getScreenBounds(), nullptr);
}

// Subtle inline "?" glyph — dim, brightens on hover; no border.
LinkEditor::HelpGlyph::HelpGlyph()
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setInterceptsMouseClicks(true, false);
}
void LinkEditor::HelpGlyph::mouseEnter(const juce::MouseEvent&) { hover = true;  repaint(); }
void LinkEditor::HelpGlyph::mouseExit (const juce::MouseEvent&) { hover = false; repaint(); }
void LinkEditor::HelpGlyph::mouseUp(const juce::MouseEvent& e)
{
    if (onClick && getLocalBounds().contains(e.getPosition())) onClick();
}
void LinkEditor::HelpGlyph::paint(juce::Graphics& g)
{
    g.setColour(hover ? kText : kText2.withAlpha(0.55f));
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    g.drawText("?", getLocalBounds(), juce::Justification::centred);
}

void LinkEditor::showPlacementChooser()
{
    juce::PopupMenu m;
    m.addSectionHeader("Where is this Link?");
    const int cur = proc.getPlacement();
    m.addItem(1, "Bus",     true, cur == LinkProcessor::PlacementBus);
    m.addItem(2, "Channel", true, cur == LinkProcessor::PlacementInsert);
    m.addItem(3, "Send",    true, cur == LinkProcessor::PlacementSend);
    auto safeThis = juce::Component::SafePointer<LinkEditor>(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&placementBtn),
        [safeThis](int r)
        {
            if (safeThis == nullptr || r == 0) return;
            safeThis->applyPlacement(r == 1 ? LinkProcessor::PlacementBus
                                  : r == 3 ? LinkProcessor::PlacementSend
                                           : LinkProcessor::PlacementInsert);
        });
}

