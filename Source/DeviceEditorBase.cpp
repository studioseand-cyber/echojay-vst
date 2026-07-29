/*
    DeviceEditorBase.cpp  —  see DeviceEditorBase.h.

    The header layout and paint are lifted from SurgicalEqEditor unchanged, so
    the EQ looks pixel-identical after being refactored onto this base.
*/

#include "DeviceEditorBase.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

// Qualified alias, not `using namespace`: unqualified `Colours` is ambiguous
// against juce::Colours, which JuceHeader.h pulls into scope. Same convention as
// SurgicalEqEditor.
using C = echojay::device::Colours;

namespace
{
    // Logo box width in the header. The asset is drawn to fit this, matching the
    // main plugin header.
    constexpr int   kLogoW        = 104;
    constexpr float kLogoFontSize = 13.0f;
    constexpr int   kBypassW      = 74;
}

DeviceEditorBase::DeviceEditorBase (EedDeviceProcessor& proc,
                                    const juce::String& title,
                                    int defaultWidth, int defaultHeight)
    : juce::AudioProcessorEditor (proc), proc_ (proc), title_ (title)
{
    setLookAndFeel (&lnf_);

    styleButton (bypassBtn_, true);
    bypassBtn_.setToggleState (proc_.isBypassed(), juce::dontSendNotification);
    bypassBtn_.onClick = [this]
    {
        proc_.setBypassed (bypassBtn_.getToggleState());
        repaint();
    };
    addAndMakeVisible (bypassBtn_);

    setSize (defaultWidth, defaultHeight);
}

DeviceEditorBase::~DeviceEditorBase()
{
    setLookAndFeel (nullptr);
}

void DeviceEditorBase::resized()
{
    auto r = getLocalBounds().reduced (kPad);

    // Clamp rather than assume: the rack can lay an editor out smaller than its
    // declared default, and a negative-height content rect is how controls end
    // up drawn on top of each other.
    const int topH = juce::jlimit (0, juce::jmax (0, r.getHeight()), kTopH);
    headerBounds_ = r.removeFromTop (topH);
    r.removeFromTop (6);
    contentBounds_ = r;

    auto top = headerBounds_;

    // Device-global controls outboard of BYPASS, at full header height so a dial
    // fits (the EQ's OUT knob).
    layoutHeaderTrailing (top);

    // Buttons sit on a button-height line centred in the taller header, so they
    // stay level with the logo and the title.
    auto bar = top.withSizeKeepingCentre (top.getWidth(),
                                          juce::jmin (kBarH, top.getHeight()));
    bypassBtn_.setBounds (bar.removeFromRight (kBypassW).reduced (0, 3));
    bar.removeFromRight (6);

    layoutHeaderLeading (bar);

    textRight_ = bar.getRight();

    layoutContent (contentBounds_);
}

void DeviceEditorBase::paint (juce::Graphics& g)
{
    g.fillAll (C::bg);

    // The header band is DIAL height, but the logo / title / hint belong on the
    // same line as the global buttons — so they are drawn on the same kBarH strip
    // centred in it that resized() places those buttons on, rather than filling
    // the taller band (which would scale the logo up out of step with them).
    const auto bar = headerBounds_.withSizeKeepingCentre (
        headerBounds_.getWidth(), juce::jmin (kBarH, headerBounds_.getHeight()));

    // The real logo asset, the same one the main plugin header draws, scaled into
    // the bar rather than re-lettered as text.
    const juce::Rectangle<float> logoBox ((float) bar.getX(), (float) bar.getY(),
                                          (float) kLogoW, (float) bar.getHeight());
    EchoJayLookAndFeel::drawLogo (g, logoBox, kLogoFontSize);

    const int afterLogo = (int) logoBox.getRight() + 6;

    const auto titleFont = uiFont (11.0f, true);
    const int  titleW    = juce::GlyphArrangement::getStringWidthInt (titleFont, title_) + 6;

    g.setColour (C::text2);
    g.setFont (titleFont);
    g.drawText (title_, afterLogo, bar.getY(), titleW, bar.getHeight(),
                juce::Justification::centredLeft);

    textLeft_ = afterLogo + titleW + 6;

    if (hint_.isNotEmpty())
    {
        // The hint is the first thing to give up room when the editor is narrow.
        g.setColour (C::text3);
        g.setFont (uiFont (9.0f));
        g.drawText (hint_, textLeft_, bar.getY(),
                    juce::jmax (0, textRight_ - textLeft_), bar.getHeight(),
                    juce::Justification::centredLeft);
    }

    paintContent (g);
}
