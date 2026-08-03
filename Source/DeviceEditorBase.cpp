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

    // The default size has to be declared HERE — the rack reads getWidth() /
    // getHeight() straight off a freshly created editor to decide what to open it
    // at, so it must be right the instant the constructor returns.
    //
    // But juce::Component::setBounds dispatches resized() SYNCHRONOUSLY, and this
    // is a base constructor: the subclass is not built and the vtable is still
    // this class's, so resized()'s call to the pure virtual layoutContent() is a
    // __cxa_pure_virtual abort — the whole plugin dying the moment ANY device
    // editor opens (which is what took out every Wave 0 device at once).
    //
    // So the size is set, the base's own geometry is computed, and the dispatch
    // into the subclass is deferred to flushPendingLayout() — which runs at the
    // first moment the subclass is guaranteed to exist.
    const juce::ScopedValueSetter<bool> building (inConstructor_, true);
    setSize (defaultWidth, defaultHeight);
}

DeviceEditorBase::~DeviceEditorBase()
{
    setLookAndFeel (nullptr);
}

void DeviceEditorBase::resized()
{
    // juce::Rectangle::reduced happily returns a NEGATIVE-sized rectangle when the
    // component is smaller than twice the padding — which a collapsed rack slot
    // is. That negative would flow straight into contentBounds_ and out to every
    // device's layoutContent, and a device is promised it can trust that rect. So
    // the padding never takes more than there is.
    const auto full = getLocalBounds();
    auto r = full.reduced (juce::jmin (kPad, full.getWidth()  / 2),
                           juce::jmin (kPad, full.getHeight() / 2));

    // Clamp rather than assume: the rack can lay an editor out smaller than its
    // declared default, and a negative-height content rect is how controls end
    // up drawn on top of each other.
    const int topH = juce::jlimit (0, juce::jmax (0, r.getHeight()), kTopH);
    headerBounds_ = r.removeFromTop (topH);
    r.removeFromTop (6);
    contentBounds_ = r;

    // Everything above is the base's own geometry and is always safe to compute.
    // Everything below reaches into the subclass, so during the base constructor
    // it is recorded as owed instead of called.
    if (inConstructor_)
    {
        layoutPending_ = true;
        return;
    }

    layoutPending_ = false;

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

// Both of these are reached only from JUCE, never from a constructor, so by the
// time either runs the subclass is fully built and the deferred layout can be
// dispatched. Being added to a parent comes first when hosted inline; painting
// covers everything else (an offscreen render, a popout).
void DeviceEditorBase::parentHierarchyChanged() { flushPendingLayout(); }

void DeviceEditorBase::flushPendingLayout()
{
    if (! layoutPending_) return;
    resized();                      // clears layoutPending_
}

void DeviceEditorBase::paint (juce::Graphics& g)
{
    // A device must never be painted with its controls still unplaced: at the
    // default size nothing else would trigger a layout, and every child would
    // draw at 0x0 in the top-left corner.
    flushPendingLayout();

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
