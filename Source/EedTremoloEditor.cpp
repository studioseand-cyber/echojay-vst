/*
    EedTremoloEditor.cpp  —  see EedTremoloEditor.h.
*/

#include "EedTremoloEditor.h"

namespace
{
    // Six dials in a row, the scope seated above. The rack sizes down from here
    // and the scope is the first thing to give up its room.
    constexpr int kDefaultW = 500;
    constexpr int kScopeH   = 108;
    constexpr int kDefaultH = 150 + kScopeH + 6;

    // Below this the waveform is a smear rather than a reading, so it is dropped
    // entirely instead of drawn uselessly small.
    constexpr int kMinScopeH = 46;
}

EedTremoloEditor::EedTremoloEditor (EedTremoloProcessor& p)
    : EedModEditorBase (p, "TREMOLO", kDefaultW, kDefaultH), proc_ (p)
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

    scope_.setCaption ("GAIN");

    // A tremolo's modulator is a GAIN: it rests at unity and only ever dips.
    // Drawn bipolar it would claim the signal spends half its cycle louder than
    // the input, which is the one thing this device never does — and it would
    // put the resting position in the middle of the plot rather than at the top
    // where the ear puts it. LfoScopeView has the mode for exactly this.
    scope_.setUnipolar (true);
    addAndMakeVisible (scope_);

    // Seeded before the first timer tick, so the picture is already right in the
    // frame the editor opens in rather than one 50 ms later.
    refreshExtras();

    finishSetup();
}

int EedTremoloEditor::topContentHeight() const
{
    return kScopeH;
}

void EedTremoloEditor::layoutTopContent (juce::Rectangle<int> area)
{
    const bool room = area.getHeight() >= kMinScopeH && area.getWidth() >= 80;
    scope_.setVisible (room);
    if (room) scope_.setBounds (area);
}

float EedTremoloEditor::lfoPhase() const
{
    return proc_.engine().lfo().displayPhase();
}

void EedTremoloEditor::refreshExtras()
{
    using P = EedTremoloProcessor;

    const bool byp = proc_.isBypassed();

    // Analytic: read through the same getParamValue path the knobs and an AI
    // move use, so the picture cannot be looking at a different set of numbers
    // than the DSP is.
    scope_.setShape          ((int)   proc_.getParamValue (P::kShape));
    scope_.setDepthPercent   ((float) proc_.getParamValue (P::kDepth));
    scope_.setStereoPhaseDeg ((float) proc_.getParamValue (P::kStereoPhase));

    scope_.setDimmed (byp);
    scope_.setPhase (lfoPhase());
}
