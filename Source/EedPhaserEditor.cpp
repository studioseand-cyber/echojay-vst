/*
    EedPhaserEditor.cpp  —  see EedPhaserEditor.h.
*/

#include "EedPhaserEditor.h"

#include <cmath>

namespace
{
    constexpr int kDefaultW = 580;
    constexpr int kTopH     = 116;
    constexpr int kDefaultH = 150 + kTopH + 6;

    constexpr int kMinTopH  = 46;

    constexpr int kMinPairW = 300;
    constexpr int kScopeW   = 150;
    constexpr int kViewGap  = 6;
}

EedPhaserEditor::EedPhaserEditor (EedPhaserProcessor& p)
    : EedModEditorBase (p, "PHASER", kDefaultW, kDefaultH), proc_ (p)
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

    scope_.setCaption ("LFO");
    sweep_.setCaption ("NOTCHES");
    addAndMakeVisible (scope_);
    addAndMakeVisible (sweep_);

    refreshExtras();
    finishSetup();
}

int EedPhaserEditor::topContentHeight() const
{
    return kTopH;
}

void EedPhaserEditor::layoutTopContent (juce::Rectangle<int> area)
{
    const bool room = area.getHeight() >= kMinTopH && area.getWidth() >= 80;

    scope_.setVisible (room);
    sweep_.setVisible (false);
    if (! room) return;

    if (area.getWidth() >= kMinPairW)
    {
        scope_.setBounds (area.removeFromLeft (kScopeW));
        area.removeFromLeft (kViewGap);
        sweep_.setBounds (area);
        sweep_.setVisible (true);
    }
    else
    {
        scope_.setBounds (area);
    }
}

float EedPhaserEditor::lfoPhase() const
{
    return proc_.engine().lfo().displayPhase();
}

void EedPhaserEditor::refreshExtras()
{
    using P = EedPhaserProcessor;

    const bool  byp    = proc_.isBypassed();
    const float depth  = (float) proc_.getParamValue (P::kDepth);
    const float centre = (float) proc_.getParamValue (P::kCentreFreq);
    const float fb     = (float) proc_.getParamValue (P::kFeedback);
    const float mix    = (float) proc_.getParamValue (P::kMix);
    const int   stages = (int)   proc_.getParamValue (P::kStages);
    const float phase  = lfoPhase();

    // Like the chorus, the phaser pins its LFO to a sine: the corner frequency
    // is a filter coefficient, and a stepped waveform in one is a click. There
    // is no shape param to read, so the scope is told what the engine does.
    scope_.setShape (echojay::LfoCore::kSine);
    scope_.setDepthPercent (depth);
    scope_.setStereoPhaseDeg (echojay::PhaserEngine::kStereoSpread01 * 360.0f);
    scope_.setDimmed (byp);
    scope_.setPhase (phase);

    // ---- where the notches currently are -----------------------------------
    // Through PhaserEngine::sweptHz — the engine's OWN static, not a second copy
    // of the sweep law. If the DSP's two-octave range ever changes, the picture
    // changes with it rather than quietly lying.
    const float depth01 = juce::jlimit (0.0f, 1.0f, depth * 0.01f);
    const float lfoV    = echojay::LfoCore::shapeAt (echojay::LfoCore::kSine, phase) * depth01;
    const float sweptHz = echojay::PhaserEngine::sweptHz (centre, lfoV);

    // Indicative depth, matching SweepView's stated contract: feedback is what
    // sharpens and deepens a phaser's notches, and MIX 0 flattens them, because
    // the notches only exist where the wet path meets the dry one.
    const float mix01   = juce::jlimit (0.0f, 1.0f, mix * 0.01f);
    const float depthDb = -(6.0f + 14.0f * std::abs (fb) * 0.01f) * mix01;

    sweep_.setDimmed (byp);
    sweep_.setSweep (sweptHz, stages, depthDb);
}
