#include "LinkEditor.h"
#include "EchoJayLogo.h"

static const juce::Colour kBg     { 0xff0A0C18 };
static const juce::Colour kCard   { 0xff161B22 };
static const juce::Colour kPanel  { 0xff080A12 };
static const juce::Colour kBorder { 0xff30363D };
static const juce::Colour kText   { 0xffE6EDF3 };
static const juce::Colour kText2  { 0xff8B949E };
static const juce::Colour kCyan   { 0xff22d3ee };
static const juce::Colour kCoral  { 0xffff6d5a };

LinkEditor::LinkEditor(LinkProcessor& p)
    : AudioProcessorEditor(&p), proc(p)
{
    // Resizable, sized to host plugin UIs — same conventions as the main
    // plugin. Size persists in the processor state.
    setResizable(true, true);
    setResizeLimits(900, 580, 1800, 1200);
    setSize(juce::jlimit(900, 1800, proc.editorW),
            juce::jlimit(580, 1200, proc.editorH));

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

    // Chain model changes repaint the strip/status (phase-2 adds the panel)
    proc.onChainModelChanged = [safe = juce::Component::SafePointer<LinkEditor>(this)]
    {
        if (safe != nullptr) safe->repaint();
    };
}

LinkEditor::~LinkEditor()
{
    proc.onChainModelChanged = nullptr;
    proc.onChainAboutToChange = nullptr;
}

juce::Rectangle<int> LinkEditor::displayArea() const
{
    return { 12, kHeaderH + 8,
             getWidth() - 24,
             getHeight() - kHeaderH - 8 - kStatusH - kStripH - 12 };
}

juce::Rectangle<int> LinkEditor::stripArea() const
{
    return { 0, getHeight() - kStripH, getWidth(), kStripH };
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

    // Status light (bounds from resized)
    const juce::Colour lightColour = proc.linkOn.load()
                                   ? juce::Colour(0xff22c55e)
                                   : juce::Colour(0xffef4444);
    g.setColour(lightColour);
    g.fillEllipse(lightBounds);
    g.setColour(lightColour.withAlpha(0.25f));
    g.fillEllipse(lightBounds.expanded(3.0f));

    // ---- Display area (inline hosting lands here in phase 2) ----
    auto area = displayArea();
    g.setColour(kPanel);
    g.fillRoundedRectangle(area.toFloat(), 8.0f);
    g.setColour(kCyan.withAlpha(0.15f));
    g.drawRoundedRectangle(area.toFloat().reduced(0.5f), 8.0f, 1.0f);

    auto& model = proc.getChainModel();
    if (model.empty())
    {
        g.setColour(kText2);
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText(proc.isChainBuilding() ? "Building chain..."
                                          : "No chain loaded",
                   area, juce::Justification::centred);
        if (!proc.isChainBuilding())
        {
            g.setColour(kText2.withAlpha(0.6f));
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            g.drawText("Chains arrive from EchoJay V2 - use the Build button in chat",
                       area.withTrimmedTop(44), juce::Justification::centredTop);
        }
    }

    // ---- Status line ----
    {
        int sy = getHeight() - kStripH - kStatusH;
        g.setColour(kText2.withAlpha(0.8f));
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        juce::String line = statusLine;
        if (line.isEmpty() && !model.empty())
        {
            int missing = 0;
            for (auto& s : model) if (s.missing) ++missing;
            line = juce::String((int)model.size()) + " slot(s)";
            if (missing > 0) line += ", " + juce::String(missing) + " missing";
            if (!proc.chainLayoutSupported()) line += "  |  unsupported channel layout (chain bypassed)";
        }
        g.drawText(line, 12, sy, getWidth() - 24, kStatusH,
                   juce::Justification::centredLeft);
    }

    // ---- Chain strip (simple name list in phase 1; blocks in phase 2) ----
    {
        auto strip = stripArea();
        g.setColour(kCard);
        g.fillRect(strip);
        g.setColour(kBorder);
        g.drawHorizontalLine(strip.getY(), 0.f, (float)getWidth());
        int x = 12;
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        for (auto& s : model)
        {
            int wPx = juce::jmax(80, 12 + s.name.length() * 6);
            auto r = juce::Rectangle<int>(x, strip.getY() + 14, wPx, kStripH - 28);
            g.setColour(s.missing ? kCoral.withAlpha(0.15f) : kPanel);
            g.fillRoundedRectangle(r.toFloat(), 6.0f);
            g.setColour(s.missing ? kCoral.withAlpha(0.6f)
                                  : kCyan.withAlpha(0.25f));
            g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 6.0f, 1.0f);
            g.setColour(s.missing ? kCoral : (s.bypassed ? kText2 : kText));
            g.drawText(s.name, r.reduced(6, 0), juce::Justification::centred, true);
            x += wPx + 10;
            if (x > getWidth()) break;
        }
    }
}

void LinkEditor::resized()
{
    // Persist window size in the processor state
    proc.editorW = getWidth();
    proc.editorH = getHeight();

    // Compact header row: [logo Link] [name field] [Active] [light]
    int fieldX = 150;
    int fieldW = juce::jmin(280, getWidth() - fieldX - 170);
    nameField.setBounds(fieldX, (kHeaderH - 26) / 2, fieldW, 26);
    toggleBtn.setBounds(fieldX + fieldW + 12, (kHeaderH - 24) / 2, 76, 24);

    const float lightD = 10.0f;
    lightBounds = { (float)(fieldX + fieldW + 12 + 76 + 10),
                    (kHeaderH - lightD) * 0.5f, lightD, lightD };
}
