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

    // Toggle
    toggleBtn.setToggleState(proc.linkOn.load(), juce::dontSendNotification);
    toggleBtn.setColour(juce::ToggleButton::textColourId, kText);
    toggleBtn.setColour(juce::ToggleButton::tickColourId, kCyan);
    toggleBtn.setColour(juce::ToggleButton::tickDisabledColourId, kText2);
    toggleBtn.onStateChange = [this]
    {
        proc.linkOn.store(toggleBtn.getToggleState());
        proc.updateShmState();
        repaint();
    };
    addAndMakeVisible(toggleBtn);

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
}

LinkEditor::~LinkEditor()
{
    proc.onChainModelChanged = nullptr;
    proc.onChainAboutToChange = nullptr;
    chainPanel.closeAllEditors();
}

void LinkEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBg);

    // ---- Header row ----
    g.setColour(kCard);
    g.fillRect(0, 0, getWidth(), kHeaderH);
    g.setColour(kBorder);
    g.drawHorizontalLine(kHeaderH, 0.f, (float)getWidth());

    juce::Image logo = juce::ImageCache::getFromMemory(echoJayLogoPNG,
                                                        (int)echoJayLogoPNGSize);
    const float logoH = 22.0f, logoX = 12.0f;
    const float logoY = (kHeaderH - logoH) * 0.5f;
    if (logo.isValid())
    {
        float aspect = (float)logo.getWidth() / (float)logo.getHeight();
        float logoW  = logoH * aspect;
        g.drawImage(logo, (int)logoX, (int)logoY, (int)logoW, (int)logoH,
                    0, 0, logo.getWidth(), logo.getHeight());
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
}

void LinkEditor::resized()
{
    // Persist window size in the processor state
    proc.editorW = getWidth();
    proc.editorH = getHeight();

    int fieldX = 150;
    int fieldW = juce::jmin(280, getWidth() - fieldX - 170);
    nameField.setBounds(fieldX, (kHeaderH - 26) / 2, fieldW, 26);
    toggleBtn.setBounds(fieldX + fieldW + 12, (kHeaderH - 24) / 2, 76, 24);

    const float lightD = 10.0f;
    lightBounds = { (float)(fieldX + fieldW + 12 + 76 + 10),
                    (kHeaderH - lightD) * 0.5f, lightD, lightD };

    chainPanel.setBounds(0, kHeaderH + 1, getWidth(), getHeight() - kHeaderH - 1);
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
