/*
    EedDynamicsFaceEditor.cpp  —  see EedDynamicsFaceEditor.h.
*/

#include "EedDynamicsFaceEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

EedDynamicsFaceEditor::EedDynamicsFaceEditor (EedDeviceProcessor& proc,
                                              const juce::String& title,
                                              const juce::String& hint,
                                              const KnobSpec* specs, int numSpecs,
                                              float meterMaxDb,
                                              std::function<float()> grSource,
                                              int defaultWidth, int defaultHeight)
    : DeviceEditorBase (proc, title, defaultWidth, defaultHeight),
      proc_ (proc), specs_ (specs), numSpecs_ (juce::jmax (0, numSpecs)),
      meter_ (meterMaxDb), grSource_ (std::move (grSource))
{
    setHeaderHint (hint);

    for (int i = 0; i < numSpecs_; ++i)
    {
        auto* k = knobs_.add (new EchoJayDeviceKnob());
        if (bindKnob (*k, specs_[i], proc_.paramSchema(), proc_, &suppressCallbacks_))
            addAndMakeVisible (k);
    }

    if (grSource_ != nullptr)
    {
        meter_.setCaption ("GR");
        addAndMakeVisible (meter_);
    }

    // Fast enough that the meter reads as motion rather than as steps, slow
    // enough that six of these open at once cost nothing.
    startTimerHz (20);
}

EedDynamicsFaceEditor::~EedDynamicsFaceEditor()
{
    stopTimer();
}

void EedDynamicsFaceEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // The meter is reserved FIRST, from the bottom. When the rack lays the
    // editor out shorter than it wants, the dials give up room and the meter
    // survives — it is the readout that shows the device is doing anything at
    // all, so it is the last thing that should vanish.
    juce::Rectangle<int> meterArea;
    if (grSource_ != nullptr)
    {
        const int meterH = juce::jmin (18, juce::jmax (0, content.getHeight() - kKnobH));
        if (meterH > 0)
        {
            meterArea = content.removeFromBottom (meterH);
            content.removeFromBottom (6);
        }
        meter_.setVisible (meterH > 0);
    }

    const int extraH = extraContentHeight();
    juce::Rectangle<int> extraArea;
    if (extraH > 0)
    {
        const int h = juce::jmin (extraH, juce::jmax (0, content.getHeight() - kKnobH));
        if (h > 0)
        {
            extraArea = content.removeFromBottom (h);
            content.removeFromBottom (4);
        }
    }

    layoutKnobRow (content, knobs_.getRawDataPointer(), knobs_.size());

    if (! extraArea.isEmpty()) layoutExtraContent (extraArea);
    if (! meterArea.isEmpty()) meter_.setBounds (meterArea);
}

void EedDynamicsFaceEditor::timerCallback()
{
    {
        const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);
        syncKnobs (knobs_.getRawDataPointer(), specs_, knobs_.size(), proc_);
    }

    const bool byp = proc_.isBypassed();
    if (bypassButton().getToggleState() != byp)
        bypassButton().setToggleState (byp, juce::dontSendNotification);

    if (grSource_ != nullptr)
    {
        meter_.setDimmed (byp);
        if (! byp) meter_.setGainReductionDb (grSource_());
    }

    refreshExtras();
}
