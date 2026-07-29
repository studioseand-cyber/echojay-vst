/*
    EedGainEditor.cpp  —  see EedGainEditor.h.
*/

#include "EedGainEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Small: two dials and a header. The rack sizes it down from here if it has
    // to, and layoutContent survives that.
    constexpr int kDefaultW = 360;
    constexpr int kDefaultH = 150;
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

    // The AI can move these while the editor is open, so poll for changes the UI
    // did not make. 15 Hz is plenty for two numbers and costs nothing.
    startTimerHz (15);
}

EedGainEditor::~EedGainEditor()
{
    stopTimer();
}

void EedGainEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

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

void EedGainEditor::timerCallback()
{
    syncFromProcessor();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
