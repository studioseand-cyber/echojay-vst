/*
    EedAutoPanEditor.cpp  —  see EedAutoPanEditor.h.
*/

#include "EedAutoPanEditor.h"

namespace
{
    constexpr int kDefaultW = 430;
    constexpr int kDefaultH = 150;
}

EedAutoPanEditor::EedAutoPanEditor (EedAutoPanProcessor& p)
    : EedModEditorBase (p, "AUTO PAN", kDefaultW, kDefaultH)
{
    setHeaderHint ("constant-power stereo movement");

    using P = EedAutoPanProcessor;

    addHeaderToggle (P::kSync, "SYNC");

    addParamKnob (P::kRateHz,      "RATE",  2, " Hz",  2.0);
    addParamKnob (P::kDivision,    "DIV",   0, "",     0.0, echojay::mod::divisionReadout);
    addParamKnob (P::kDepth,       "DEPTH", 0, " %");
    addParamKnob (P::kShape,       "SHAPE", 0, "",     0.0, echojay::mod::shapeReadout);
    addParamKnob (P::kStereoPhase, "PHASE", 0, " deg");

    setRateGroup (P::kSync, P::kRateHz, P::kDivision);
    finishSetup();
}
