/*
    EedAutoPanEditor.cpp  —  see EedAutoPanEditor.h.
*/

#include "EedAutoPanEditor.h"

namespace
{
    // 560 rather than 430: the depth pass's WIDTH and SMOOTH dials make seven
    // columns, and the header needs room for the MODE selector — whose longest
    // entry, "CONSTANT_POWER", is the widest selector text in the cluster.
    constexpr int kDefaultW    = 560;
    constexpr int kScopeH      = 100;
    constexpr int kPositionH   = 30;
    constexpr int kTopGap      = 4;
    constexpr int kTopH        = kScopeH + kTopGap + kPositionH;
    constexpr int kDefaultH    = 150 + kTopH + 6;

    constexpr int kModeW = 132;

    // Below this the waveform is a smear rather than a reading.
    constexpr int kMinScopeH = 46;
}

EedAutoPanEditor::EedAutoPanEditor (EedAutoPanProcessor& p)
    : EedModEditorBase (p, "AUTO PAN", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("constant-power stereo movement");

    using P = EedAutoPanProcessor;

    addHeaderToggle (P::kSync, "SYNC");
    addHeaderChoice (P::kMode, kModeW);

    addParamKnob (P::kRateHz,      "RATE",   2, " Hz",  2.0);
    addParamKnob (P::kDivision,    "DIV",    0, "",     0.0, echojay::mod::divisionReadout);
    addParamKnob (P::kDepth,       "DEPTH",  0, " %");
    addParamKnob (P::kShape,       "SHAPE",  0, "",     0.0, echojay::mod::shapeReadout);
    addParamKnob (P::kStereoPhase, "PHASE",  0, " deg");
    addParamKnob (P::kWidth,       "WIDTH",  0, " %");
    addParamKnob (P::kSmoothing,   "SMOOTH", 1, " ms", 20.0);

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
    const float width  = (float) proc_.getParamValue (P::kWidth) * 0.01f;
    const float phase  = lfoPhase();

    // The scope shows the pan POSITION the engine computes: depth scaled by
    // width, because that is exactly the product the pan law is fed. Without
    // width in the picture, width 50 would look like a full-field sweep above
    // a device confining itself to half of one.
    scope_.setShape          (shape);
    scope_.setDepthPercent   (depth * width);
    scope_.setStereoPhaseDeg (stereo);
    scope_.setDimmed (byp);
    scope_.setPhase (phase);

    // The two channels' positions, from the SAME phase tap and the SAME shape
    // table the scope draws — so the dot on the strip and the playhead on the
    // curve are two views of one number and cannot drift apart. This is exactly
    // what AutoPanEngine feeds its pan law: the LFO output, depth-scaled, with
    // the right channel offset by stereo_phase, all scaled by width.
    const float depth01 = juce::jlimit (0.0f, 1.0f, depth * 0.01f) * width;
    const float offset  = stereo * (1.0f / 360.0f);

    // displayShapeAt, not shapeAt: for RANDOM the scope draws a representative
    // staircase (see LfoScopeView.h), and the dot has to sit on the curve that
    // is actually drawn rather than on the live value the plot cannot show.
    const float panL = echojay::viz::LfoScopeView::displayShapeAt (shape, phase) * depth01;
    const float panR = echojay::viz::LfoScopeView::displayShapeAt (shape, phase + offset) * depth01;

    position_.setDimmed (byp);
    position_.setPositions (panL, panR);

    juce::String h;
    switch (proc_.engine().getMode())
    {
        case echojay::AutoPanMode::Linear:   h = "linear crossfade movement"; break;
        case echojay::AutoPanMode::Binaural: h = "binaural - delay-cued placement"; break;
        case echojay::AutoPanMode::ConstantPower:
        default:                             h = "constant-power stereo movement"; break;
    }
    if (h != lastHint_)
    {
        lastHint_ = h;
        setHeaderHint (h);
    }
}
