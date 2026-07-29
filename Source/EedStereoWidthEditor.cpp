/*
    EedStereoWidthEditor.cpp  —  see EedStereoWidthEditor.h.
*/

#include "EedStereoWidthEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Three dials, a header, and a goniometer seated above them. The rack sizes
    // it down from here if it has to, and layoutContent survives that — the
    // scope is the first thing to give up its room.
    constexpr int kDefaultW = 460;
    constexpr int kScopeH   = 168;
    constexpr int kDefaultH = 150 + kScopeH + 6;
    constexpr int kGap      = 20;

    // Below this the Lissajous is a blob rather than an image, so it is dropped
    // instead of drawn uselessly small.
    constexpr int kMinScopeH = 60;
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

    addAndMakeVisible (scope_);

    // The AI can move these while the editor is open, so poll for changes the UI
    // did not make. 20 Hz rather than the 15 the dials alone needed: the scope
    // rides the same timer, and below about 20 Hz a Lissajous reads as a series
    // of stills instead of as a live image.
    startTimerHz (20);
}

EedStereoWidthEditor::~EedStereoWidthEditor()
{
    stopTimer();
}

void EedStereoWidthEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // The scope is reserved from the TOP out of whatever is left once the dial
    // row is whole — it is the readout, the dials are the device, and a rack
    // slot laid out short loses the readout first (the inline-hosting contract
    // in DeviceEditorBase.h).
    {
        const int want = juce::jmin (kScopeH,
                                     juce::jmax (0, content.getHeight() - kKnobH - 6));
        const bool room = want >= kMinScopeH && content.getWidth() >= 80;

        scope_.setVisible (room);
        if (room)
        {
            auto band = content.removeFromTop (want);
            content.removeFromTop (6);

            // Centred and only as wide as it needs to be. The Lissajous itself
            // is square (a stretched vectorscope reports a width the signal does
            // not have), so letting the component span a 460-wide panel would
            // buy nothing but a very long correlation bar with a small picture
            // stranded in the middle of it.
            const int w = juce::jmin (band.getWidth(), want + 60);
            scope_.setBounds (band.withSizeKeepingCentre (w, band.getHeight()));
        }
    }

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

void EedStereoWidthEditor::refreshScope()
{
    const bool byp = proc_.isBypassed();
    scope_.setDimmed (byp);

    // A bypassed device stops writing its tap (processBlock returns early), so
    // reading it would draw a frozen picture of processing that is not
    // happening. Dimmed and left alone is the same choice the GR meter makes.
    if (byp || ! scope_.isVisible()) return;

    const int n = proc_.scopeTap().read (scopeL_.data(), scopeR_.data(), kScopeFrame);
    if (n > 0)
        scope_.setSamples (scopeL_.data(), scopeR_.data(), n);
}

void EedStereoWidthEditor::timerCallback()
{
    syncFromProcessor();
    refreshScope();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
