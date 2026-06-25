#include "LinkEditor.h"
#include "EchoJayLogo.h"

static const juce::Colour kBg     { 0xff0D1117 };
static const juce::Colour kCard   { 0xff161B22 };
static const juce::Colour kBorder { 0xff30363D };
static const juce::Colour kText   { 0xffE6EDF3 };
static const juce::Colour kText2  { 0xff8B949E };
static const juce::Colour kCyan   { 0xff22d3ee };

LinkEditor::LinkEditor(LinkProcessor& p)
    : AudioProcessorEditor(&p), proc(p)
{
    setSize(380, 200);

    // Name field
    nameField.setText(proc.linkName, juce::dontSendNotification);
    nameField.setFont(juce::Font(juce::FontOptions(14.0f)));
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

    nameLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    nameLabel.setColour(juce::Label::textColourId, kText2);
    addAndMakeVisible(nameLabel);
}

LinkEditor::~LinkEditor() {}

void LinkEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBg);

    // Title bar
    g.setColour(kCard);
    g.fillRect(0, 0, getWidth(), 36);
    g.setColour(kBorder);
    g.drawHorizontalLine(36, 0.f, (float)getWidth());

    // Logo in header
    juce::Image logo = juce::ImageCache::getFromMemory(echoJayLogoPNG,
                                                        (int)echoJayLogoPNGSize);
    const float headerH = 36.0f;
    const float logoH   = 22.0f;
    const float logoX   = 12.0f;
    const float logoY   = (headerH - logoH) * 0.5f;

    if (logo.isValid())
    {
        float aspect = (float)logo.getWidth() / (float)logo.getHeight();
        float logoW  = logoH * aspect;
        g.drawImage(logo,
                    (int)logoX, (int)logoY, (int)logoW, (int)logoH,
                    0, 0, logo.getWidth(), logo.getHeight());

        // "Link" label after logo
        g.setColour(kCyan);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText("Link",
                   (int)(logoX + logoW + 6.0f), 0,
                   80, 36,
                   juce::Justification::centredLeft);
    }
    else
    {
        // Fallback if logo unavailable
        g.setColour(kCyan);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText("ECHOJAY LINK", 16, 0, getWidth() - 32, 36,
                   juce::Justification::centredLeft);
    }

    // Card outline
    g.setColour(kBorder);
    g.drawRoundedRectangle(12.f, 48.f, (float)getWidth() - 24.f, 96.f, 6.f, 1.f);

    // ---- Diagnostic line ----
    auto& d = proc.diag;
    juce::String regStr  = d.regOpened  ? "opened"
                         : (d.regKey.isEmpty() ? "not tried"
                                                : "FAILED errno " + juce::String(d.regErrno));
    juce::String slotStr = (d.slotIdx >= 0) ? juce::String(d.slotIdx) : "NONE";
    juce::String wroteStr = proc.didWrite.load() ? "yes" : "no";
    juce::String diagLine = "registry: " + regStr
                          + "  |  slot: " + slotStr
                          + "  |  wrote: " + wroteStr;
    juce::String keyLine  = "dir: " + (d.regKey.isEmpty() ? "(none)" : d.regKey);

    g.setFont(juce::Font(juce::FontOptions(9.5f)));
    g.setColour(kText2.withAlpha(0.7f));
    g.drawText(diagLine, 12, 150, getWidth() - 24, 14, juce::Justification::centredLeft);
    g.setColour(kText2.withAlpha(0.5f));
    g.drawText(keyLine,  12, 163, getWidth() - 24, 13, juce::Justification::centredLeft);

    // Status light
    const juce::Colour lightColour = proc.linkOn.load()
                                   ? juce::Colour(0xff22c55e)   // green
                                   : juce::Colour(0xffef4444);  // red
    g.setColour(lightColour);
    g.fillEllipse(lightBounds);

    // Glow ring
    g.setColour(lightColour.withAlpha(0.25f));
    g.fillEllipse(lightBounds.expanded(3.0f));

    // Status text next to light
    g.setColour(kText2);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    const juce::String statusTxt = proc.linkOn ? "On" : "Off";
    g.drawText(statusTxt,
               (int)(lightBounds.getRight() + 6.0f),
               (int)(lightBounds.getY() - 2.0f),
               40, (int)(lightBounds.getHeight() + 4.0f),
               juce::Justification::centredLeft);
}

void LinkEditor::resized()
{
    const int pad = 20;
    const int fW  = getWidth() - pad * 2;

    nameLabel.setBounds(pad, 52, fW, 16);
    nameField.setBounds(pad, 70, fW, 28);
    toggleBtn.setBounds(pad, 106, 80, 24);

    // Status light: 10px circle, vertically centred in toggle row, after toggle
    const float lightD = 10.0f;
    const float lightX = (float)(pad + 80 + 12);
    const float lightY = 106.0f + (24.0f - lightD) * 0.5f;
    lightBounds = { lightX, lightY, lightD, lightD };
}
