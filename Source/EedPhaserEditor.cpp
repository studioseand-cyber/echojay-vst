/*
    EedPhaserEditor.cpp  —  see EedPhaserEditor.h.
*/

#include "EedPhaserEditor.h"

#include <cmath>

namespace
{
    // 620 rather than 580: the depth pass's SPREAD dial makes eight columns,
    // which fit in one row at this width, and the header gains room for the
    // MODE selector without crowding the title.
    constexpr int kDefaultW = 620;
    constexpr int kTopH     = 116;
    constexpr int kDefaultH = 150 + kTopH + 6;

    constexpr int kMinTopH  = 46;

    constexpr int kMinPairW = 300;
    constexpr int kScopeW   = 150;
    constexpr int kViewGap  = 6;

    // Wide enough for "VINTAGE", the longest of the three.
    constexpr int kModeW = 92;
}

EedPhaserEditor::EedPhaserEditor (EedPhaserProcessor& p)
    : EedModEditorBase (p, "PHASER", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("sweeping allpass notches");

    using P = EedPhaserProcessor;

    addHeaderToggle (P::kSync, "SYNC");
    addHeaderChoice (P::kMode, kModeW);

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
    addParamKnob (P::kSpread,     "SPREAD", 0, " deg");

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
    const float spread = (float) proc_.getParamValue (P::kSpread);
    const float phase  = lfoPhase();

    // Like the chorus, the phaser pins its LFO to a sine: the corner frequency
    // is a filter coefficient, and a stepped waveform in one is a click. There
    // is no shape param to read, so the scope is told what the engine does.
    scope_.setShape (echojay::LfoCore::kSine);
    scope_.setDepthPercent (depth);
    scope_.setStereoPhaseDeg (spread);
    scope_.setDimmed (byp);
    scope_.setPhase (phase);

    // ---- where the notches currently are -----------------------------------
    // Through PhaserEngine::sweptHz — the engine's OWN static, not a second copy
    // of the sweep law. If the DSP's two-octave range ever changes, the picture
    // changes with it rather than quietly lying.
    //
    // effectiveStages(), not the STAGES dial: vintage caps the cascade at 4,
    // and drawing the dialled 12 would show notches above a device playing two.
    const float depth01 = juce::jlimit (0.0f, 1.0f, depth * 0.01f);
    const float lfoV    = echojay::LfoCore::shapeAt (echojay::LfoCore::kSine, phase) * depth01;
    const float sweptHz = echojay::PhaserEngine::sweptHz (centre, lfoV);
    const int   stages  = proc_.engine().effectiveStages();

    // Indicative depth, matching SweepView's stated contract: feedback is what
    // sharpens and deepens a phaser's notches, and MIX 0 flattens them, because
    // the notches only exist where the wet path meets the dry one.
    const float mix01   = juce::jlimit (0.0f, 1.0f, mix * 0.01f);
    const float depthDb = -(6.0f + 14.0f * std::abs (fb) * 0.01f) * mix01;

    sweep_.setDimmed (byp);
    sweep_.setSweep (sweptHz, stages, depthDb);

    // The hint names the mode, and in vintage the stage CAP: the STAGES dial
    // still turns past 4 there, and without the caption the capped cascade
    // would look like a dial doing nothing.
    juce::String h;
    switch (proc_.engine().getMode())
    {
        case echojay::PhaserMode::Vintage:
            h = "vintage - warm, " + juce::String (stages) + " stages, hot feedback";
            break;
        case echojay::PhaserMode::Stereo:
            h = "stereo - notches interleave across the field";
            break;
        case echojay::PhaserMode::Modern:
        default:
            h = "sweeping allpass notches";
            break;
    }
    if (h != lastHint_)
    {
        lastHint_ = h;
        setHeaderHint (h);
    }
}
