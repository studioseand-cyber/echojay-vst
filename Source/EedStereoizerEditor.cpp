/*
    EedStereoizerEditor.cpp  —  see EedStereoizerEditor.h.
*/

#include "EedStereoizerEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Four dials and a header. The rack sizes it down from here if it has to,
    // and layoutContent survives that.
    constexpr int kDefaultW = 560;
    constexpr int kDefaultH = 150;
    constexpr int kGap      = 20;
}

EedStereoizerEditor::EedStereoizerEditor (EedStereoizerProcessor& p)
    : DeviceEditorBase (p, "STEREOIZER", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("Haas widener, mono-safe");

    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here.
        const auto* spec = EedStereoizerProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));

        // Through the schema path, exactly as an AI move goes. Each knob pushes
        // only its OWN param, so a param the AI just moved is not overwritten by
        // a stale reading of a different knob.
        k.onValueChange = [this, &k, id]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (juce::String (id), k.getRealValue());
        };
        addAndMakeVisible (k);
    };

    // 0 is OFF for both of these, not a setting — say so, or the bottom of the
    // dial's travel looks like a value rather than a bypass.
    haasKnob_.formatValue = [] (double v)
    {
        return v < 0.05 ? juce::String ("OFF") : juce::String (v, 1) + " ms";
    };
    monoKnob_.formatValue = [] (double v)
    {
        return v < 0.5 ? juce::String ("OFF")
                       : juce::String (juce::roundToInt (v)) + " Hz";
    };

    setup (widthKnob_, EedStereoizerProcessor::kWidth,         0.0, 0, " %",  "WIDTH");
    setup (haasKnob_,  EedStereoizerProcessor::kHaasMs,        0.0, 1, " ms", "HAAS");
    setup (monoKnob_,  EedStereoizerProcessor::kMonoMakerHz, 120.0, 0, " Hz", "MONO");
    setup (mixKnob_,   EedStereoizerProcessor::kMix,           0.0, 0, " %",  "MIX");

    // The AI can move these while the editor is open, so poll for changes the UI
    // did not make.
    startTimerHz (15);
}

EedStereoizerEditor::~EedStereoizerEditor()
{
    stopTimer();
}

void EedStereoizerEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // Four dial columns centred as a group, so the device stays balanced at
    // whatever width the rack gives it.
    const int rowW = kKnobW * 4 + kGap * 3;
    auto row = content.withSizeKeepingCentre (juce::jmin (rowW, content.getWidth()),
                                              juce::jmin (kKnobH, content.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - kGap * 3) / 4);

    widthKnob_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (kGap);
    haasKnob_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (kGap);
    monoKnob_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (kGap);
    mixKnob_.setBounds (row.removeFromLeft (colW));
}

void EedStereoizerEditor::syncFromProcessor()
{
    // Only write when it actually moved: setRealValue would otherwise fight a
    // drag in progress by snapping the knob to the value it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    auto pull = [this] (EchoJayDeviceKnob& k, const char* id)
    {
        const double v = proc_.getParamValue (juce::String (id));
        if (std::abs (v - k.getRealValue()) > 1.0e-4)
            k.setRealValue (v);
    };

    pull (widthKnob_, EedStereoizerProcessor::kWidth);
    pull (haasKnob_,  EedStereoizerProcessor::kHaasMs);
    pull (monoKnob_,  EedStereoizerProcessor::kMonoMakerHz);
    pull (mixKnob_,   EedStereoizerProcessor::kMix);
}

void EedStereoizerEditor::timerCallback()
{
    syncFromProcessor();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
