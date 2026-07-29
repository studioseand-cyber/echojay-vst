/*
    EedTremoloEditor.cpp  —  see EedTremoloEditor.h.
*/

#include "EedTremoloEditor.h"

namespace
{
    // Six dials in a row plus the header's SYNC. The rack sizes it down from
    // here if it has to, and the base's flow layout wraps rather than clipping.
    constexpr int kDefaultW = 500;
    constexpr int kDefaultH = 150;
}

EedTremoloEditor::EedTremoloEditor (EedTremoloProcessor& p)
    : EedModEditorBase (p, "TREMOLO", kDefaultW, kDefaultH)
{
    setHeaderHint ("rhythmic level modulation");

    using P = EedTremoloProcessor;

    addHeaderToggle (P::kSync, "SYNC");

    // Rate is skewed so the musically useful 1-6 Hz range occupies the middle of
    // the dial's travel instead of the first few degrees of it.
    addParamKnob (P::kRateHz,      "RATE",  2, " Hz",  2.0);
    addParamKnob (P::kDivision,    "DIV",   0, "",     0.0, echojay::mod::divisionReadout);
    addParamKnob (P::kDepth,       "DEPTH", 0, " %");
    addParamKnob (P::kShape,       "SHAPE", 0, "",     0.0, echojay::mod::shapeReadout);
    addParamKnob (P::kStereoPhase, "PHASE", 0, " deg");
    addParamKnob (P::kMix,         "MIX",   0, " %");

    setRateGroup (P::kSync, P::kRateHz, P::kDivision);
    finishSetup();
}
