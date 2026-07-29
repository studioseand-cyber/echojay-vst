/*
    EedPhaserEditor.cpp  —  see EedPhaserEditor.h.
*/

#include "EedPhaserEditor.h"

namespace
{
    constexpr int kDefaultW = 580;
    constexpr int kDefaultH = 150;
}

EedPhaserEditor::EedPhaserEditor (EedPhaserProcessor& p)
    : EedModEditorBase (p, "PHASER", kDefaultW, kDefaultH)
{
    setHeaderHint ("sweeping allpass notches");

    using P = EedPhaserProcessor;

    addHeaderToggle (P::kSync, "SYNC");

    addParamKnob (P::kRateHz,     "RATE",   2, " Hz", 2.0);
    addParamKnob (P::kDivision,   "DIV",    0, "",    0.0, echojay::mod::divisionReadout);
    addParamKnob (P::kDepth,      "DEPTH",  0, " %");
    addParamKnob (P::kStages,     "STAGES", 0, "",    0.0,
                  [] (double v) { return juce::String ((int) std::lround (v)); });
    addParamKnob (P::kFeedback,   "FDBK",   0, " %");
    // Frequency dials are logarithmic in EchoJay: 800 Hz sits mid-travel so the
    // musically dense low half is not crushed into the first inch of the sweep.
    addParamKnob (P::kCentreFreq, "FREQ",   0, " Hz", 800.0);
    addParamKnob (P::kMix,        "MIX",    0, " %");

    setRateGroup (P::kSync, P::kRateHz, P::kDivision);
    finishSetup();
}
