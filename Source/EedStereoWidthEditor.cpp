/*
    EedStereoWidthEditor.cpp  —  see EedStereoWidthEditor.h.
*/

#include "EedStereoWidthEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Three dials and a header. The rack sizes it down from here if it has to,
    // and layoutContent survives that.
    constexpr int kDefaultW = 460;
    constexpr int kDefaultH = 150;
    constexpr int kGap      = 20;
}

EedStereoWidthEditor::EedStereoWidthEditor (EedStereoWidthProcessor& p)
    : DeviceEditorBase (p, "STEREO WIDTH", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("width + bass mono, mono-safe");

    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial, and widening
        // one without the other is impossible.
        const auto* spec = EedStereoWidthProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));

        // Drive the processor THROUGH the schema path, exactly as an AI move
        // does. Each knob pushes only its OWN param, so a knob the AI just moved
        // is not overwritten by a stale reading of another one.
        k.onValueChange = [this, &k, id]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (juce::String (id), k.getRealValue());
        };
        addAndMakeVisible (k);
    };

    // 0 Hz is OFF, not "a filter at 0 Hz" — the readout has to say so, or the
    // bottom of the dial's travel looks like a setting rather than a bypass.
    bassMonoKnob_.formatValue = [] (double v)
    {
        return v < 0.5 ? juce::String ("OFF")
                       : juce::String (juce::roundToInt (v)) + " Hz";
    };

    setup (widthKnob_,    EedStereoWidthProcessor::kWidth,        0.0,   0, " %",  "WIDTH");
    setup (bassMonoKnob_, EedStereoWidthProcessor::kBassMonoHz, 120.0,   0, " Hz", "BASS MONO");
    setup (trimKnob_,     EedStereoWidthProcessor::kOutputTrimDb, 0.0,   1, " dB", "TRIM");

    // The AI can move these while the editor is open, so poll for changes the UI
    // did not make. 15 Hz is plenty for three numbers and costs nothing.
    startTimerHz (15);
}

EedStereoWidthEditor::~EedStereoWidthEditor()
{
    stopTimer();
}

void EedStereoWidthEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // Three dial columns centred as a group, so the device stays balanced at
    // whatever width the rack gives it.
    const int rowW = kKnobW * 3 + kGap * 2;
    auto row = content.withSizeKeepingCentre (juce::jmin (rowW, content.getWidth()),
                                              juce::jmin (kKnobH, content.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - kGap * 2) / 3);

    widthKnob_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (kGap);
    bassMonoKnob_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (kGap);
    trimKnob_.setBounds (row.removeFromLeft (colW));
}

void EedStereoWidthEditor::syncFromProcessor()
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

    pull (widthKnob_,    EedStereoWidthProcessor::kWidth);
    pull (bassMonoKnob_, EedStereoWidthProcessor::kBassMonoHz);
    pull (trimKnob_,     EedStereoWidthProcessor::kOutputTrimDb);
}

void EedStereoWidthEditor::timerCallback()
{
    syncFromProcessor();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
