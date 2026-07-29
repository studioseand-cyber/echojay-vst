/*
    EedAutoPanEditor.cpp  —  see EedAutoPanEditor.h.
*/

#include "EedAutoPanEditor.h"

namespace
{
    constexpr int kDefaultW    = 430;
    constexpr int kScopeH      = 100;
    constexpr int kPositionH   = 30;
    constexpr int kTopGap      = 4;
    constexpr int kTopH        = kScopeH + kTopGap + kPositionH;
    constexpr int kDefaultH    = 150 + kTopH + 6;

    // Below this the waveform is a smear rather than a reading.
    constexpr int kMinScopeH = 46;
}

EedAutoPanEditor::EedAutoPanEditor (EedAutoPanProcessor& p)
    : EedModEditorBase (p, "AUTO PAN", kDefaultW, kDefaultH), proc_ (p)
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

    // Bipolar, unlike the tremolo's: here the modulator IS the pan position, and
    // its centre is the centre of the field rather than unity gain.
    scope_.setCaption ("PAN");
    addAndMakeVisible (scope_);
    addAndMakeVisible (position_);

    refreshExtras();
    finishSetup();
}

int EedAutoPanEditor::topContentHeight() const
{
    return kTopH;
}

void EedAutoPanEditor::layoutTopContent (juce::Rectangle<int> area)
{
    const bool room = area.getHeight() >= kMinScopeH && area.getWidth() >= 80;

    scope_.setVisible (room);
    position_.setVisible (false);
    if (! room) return;

    // The strip is given up before the scope is: it is the smaller reading, and
    // a rack slot that only has room for one picture should keep the one that
    // shows the movement.
    if (area.getHeight() >= kMinScopeH + kTopGap + kPositionH)
    {
        position_.setBounds (area.removeFromBottom (kPositionH));
        area.removeFromBottom (kTopGap);
        position_.setVisible (true);
    }

    scope_.setBounds (area);
}

float EedAutoPanEditor::lfoPhase() const
{
    return proc_.engine().lfo().displayPhase();
}

void EedAutoPanEditor::refreshExtras()
{
    using P = EedAutoPanProcessor;

    const bool  byp    = proc_.isBypassed();
    const int   shape  = (int)   proc_.getParamValue (P::kShape);
    const float depth  = (float) proc_.getParamValue (P::kDepth);
    const float stereo = (float) proc_.getParamValue (P::kStereoPhase);
    const float phase  = lfoPhase();

    scope_.setShape          (shape);
    scope_.setDepthPercent   (depth);
    scope_.setStereoPhaseDeg (stereo);
    scope_.setDimmed (byp);
    scope_.setPhase (phase);

    // The two channels' positions, from the SAME phase tap and the SAME shape
    // table the scope draws — so the dot on the strip and the playhead on the
    // curve are two views of one number and cannot drift apart. This is exactly
    // what AutoPanEngine feeds its pan law: the LFO output, depth-scaled, with
    // the right channel offset by stereo_phase.
    const float depth01 = juce::jlimit (0.0f, 1.0f, depth * 0.01f);
    const float offset  = stereo * (1.0f / 360.0f);

    const float panL = echojay::LfoCore::shapeAt (shape, phase) * depth01;
    const float panR = echojay::LfoCore::shapeAt (shape, phase + offset) * depth01;

    position_.setDimmed (byp);
    position_.setPositions (panL, panR);
}
