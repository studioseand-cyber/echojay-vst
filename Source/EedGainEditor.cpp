/*
    EedGainEditor.cpp  —  see EedGainEditor.h.
*/

#include "EedGainEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Two dials, a header, and the I/O meters seated above them. The rack sizes
    // it down from here if it has to, and layoutContent survives that — the
    // meters are the first thing to give up their room.
    constexpr int kDefaultW = 360;

    // 34 is what a captioned VizView needs to draw anything: 4 inset + 11
    // caption + 14 minimum plot + 4 inset. Below it the caption is dropped and
    // the bar reads as an unlabelled smear, so the pair is dropped instead.
    constexpr int kMeterH   = 34;
    constexpr int kMeterGap = 4;
    constexpr int kMetersH  = kMeterH * 2 + kMeterGap;
    constexpr int kDefaultH = 150 + kMetersH + 6;
}

EedGainEditor::EedGainEditor (EedGainProcessor& p)
    : DeviceEditorBase (p, "GAIN", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("level + pan");

    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial, and widening one
        // without the other is impossible.
        const auto* spec = EedGainProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this] { if (! suppressCallbacks_) pushToProcessor(); };
        addAndMakeVisible (k);
    };

    setup (levelKnob_, EedGainProcessor::kLevelDb, 0.0, 1, " dB", "LEVEL");
    setup (panKnob_,   EedGainProcessor::kPan,     0.0, 2, "",    "PAN");

    // A shared floor on both meters, or the comparison the stack exists to make
    // is being drawn on two different scales.
    for (auto* m : { &inMeter_, &outMeter_ })
    {
        m->setFloorDb ((float) echojay::GainEngine::kMinDb);
        addAndMakeVisible (*m);
    }
    inMeter_.setCaption ("IN");
    outMeter_.setCaption ("OUT");

    // 20 Hz rather than the 15 the dials alone needed: a level meter at 15 Hz
    // reads as a series of stills, and the peak hold is specified in frames.
    startTimerHz (20);
}

EedGainEditor::~EedGainEditor()
{
    stopTimer();
}

void EedGainEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // The meters are reserved from the TOP out of whatever is left once the dial
    // row is whole — they are the readout, the dials are the device, and a short
    // rack slot loses the readout first (DeviceEditorBase.h's inline-hosting
    // contract). They go as a PAIR: one meter alone is not a comparison.
    {
        const int want = juce::jmin (kMetersH,
                                     juce::jmax (0, content.getHeight() - kKnobH - 6));
        const bool room = want >= kMetersH && content.getWidth() >= 80;

        inMeter_.setVisible (room);
        outMeter_.setVisible (room);

        if (room)
        {
            auto band = content.removeFromTop (want);
            content.removeFromTop (6);

            inMeter_.setBounds (band.removeFromTop (kMeterH));
            band.removeFromTop (kMeterGap);
            outMeter_.setBounds (band.removeFromTop (kMeterH));
        }
    }

    // Two dial columns, centred as a pair, so the device stays balanced at any
    // width the rack gives it.
    const int pairW = kKnobW * 2 + 24;
    auto row = content.withSizeKeepingCentre (juce::jmin (pairW, content.getWidth()),
                                              juce::jmin (kKnobH, content.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - 24) / 2);
    levelKnob_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (24);
    panKnob_.setBounds (row.removeFromLeft (colW));
}

void EedGainEditor::pushToProcessor()
{
    // Through the schema path, exactly as an AI move would.
    proc_.setParamValue (EedGainProcessor::kLevelDb, levelKnob_.getRealValue());
    proc_.setParamValue (EedGainProcessor::kPan,     panKnob_.getRealValue());
}

void EedGainEditor::syncFromProcessor()
{
    const double lvl = proc_.getParamValue (EedGainProcessor::kLevelDb);
    const double pan = proc_.getParamValue (EedGainProcessor::kPan);

    // Only write when it actually moved: setRealValue would otherwise fight a
    // drag in progress by snapping the knob to the value it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    if (std::abs (lvl - levelKnob_.getRealValue()) > 1.0e-4)
        levelKnob_.setRealValue (lvl);
    if (std::abs (pan - panKnob_.getRealValue()) > 1.0e-4)
        panKnob_.setRealValue (pan);
}

void EedGainEditor::refreshMeters()
{
    const bool byp = proc_.isBypassed();
    inMeter_.setDimmed (byp);
    outMeter_.setDimmed (byp);

    // A bypassed device stops writing its taps (processBlock returns early), so
    // reading them would animate a picture of processing that is not happening.
    // Dimmed and left alone, the same choice the goniometer and GR meter make.
    if (byp || ! inMeter_.isVisible()) return;

    inMeter_.setLevelLinear  (proc_.inputPeak(),  proc_.inputRms());
    outMeter_.setLevelLinear (proc_.outputPeak(), proc_.outputRms());
}

void EedGainEditor::timerCallback()
{
    syncFromProcessor();
    refreshMeters();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
