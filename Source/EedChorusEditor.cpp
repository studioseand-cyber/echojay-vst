/*
    EedChorusEditor.cpp  —  see EedChorusEditor.h.
*/

#include "EedChorusEditor.h"

namespace
{
    constexpr int kDefaultW = 580;
    constexpr int kDefaultH = 150;
}

EedChorusEditor::EedChorusEditor (EedChorusProcessor& p)
    : EedModEditorBase (p, "CHORUS", kDefaultW, kDefaultH)
{
    setHeaderHint ("drifting detuned voices");

    using P = EedChorusProcessor;

    addHeaderToggle (P::kSync, "SYNC");

    addParamKnob (P::kRateHz,   "RATE",   2, " Hz", 2.0);
    addParamKnob (P::kDivision, "DIV",    0, "",    0.0, echojay::mod::divisionReadout);
    addParamKnob (P::kDepth,    "DEPTH",  0, " %");
    // Skewed low: the difference between 2 ms and 8 ms is the difference between
    // a flanger and a chorus, and it deserves more of the dial than 40-50 ms does.
    addParamKnob (P::kDelayMs,  "DELAY",  1, " ms", 10.0);
    addParamKnob (P::kVoices,   "VOICES", 0, "",    0.0,
                  [] (double v) { return juce::String ((int) std::lround (v)); });
    addParamKnob (P::kFeedback, "FDBK",   0, " %");
    addParamKnob (P::kMix,      "MIX",    0, " %");

    setRateGroup (P::kSync, P::kRateHz, P::kDivision);
    finishSetup();
}
