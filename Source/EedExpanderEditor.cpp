/*
    EedExpanderEditor.cpp  —  see EedExpanderEditor.h.
*/

#include "EedExpanderEditor.h"

using namespace echojay::device;

namespace
{
    constexpr int kDefaultW = 420;
    constexpr int kCurveH   = 132;
    constexpr int kDefaultH = 190 + kCurveH + 6;

    constexpr int kMinCurveH = 54;

    // The expander threshold reaches -80 dB, same as the gate's, so the plot
    // runs as deep.
    constexpr float kFloorDb = -90.0f;

    const KnobSpec kKnobs[] = {
        { EedExpanderProcessor::kThresholdDb, "THRESH",  " dB", 1, 0.0 },
        { EedExpanderProcessor::kRatio,       "RATIO",   ":1",  2, 3.0 },
        { EedExpanderProcessor::kAttackMs,    "ATTACK",  " ms", 2, 2.0 },
        { EedExpanderProcessor::kReleaseMs,   "RELEASE", " ms", 0, 250.0 },
        { EedExpanderProcessor::kRangeDb,     "RANGE",   " dB", 1, 0.0 },
    };
}

EedExpanderEditor::EedExpanderEditor (EedExpanderProcessor& p)
    // Scale to the range knob's own ceiling, so the meter never runs out of
    // travel before the device does.
    : EedDynamicsFaceEditor (p, "EXPANDER", "downward expansion, range-limited",
                             kKnobs, (int) std::size (kKnobs),
                             60.0f,
                             [&p] { return p.gainReductionDb(); },
                             kDefaultW, kDefaultH),
      proc_ (p)
{
    curve_.setCaption ("TRANSFER");
    curve_.setFloorDb (kFloorDb);
    addAndMakeVisible (curve_);

    refreshExtras();
}

int EedExpanderEditor::topContentHeight() const
{
    return kCurveH;
}

void EedExpanderEditor::layoutTopContent (juce::Rectangle<int> area)
{
    const bool room = area.getHeight() >= kMinCurveH && area.getWidth() >= 80;

    curve_.setVisible (room);
    if (room) curve_.setBounds (area);
}

void EedExpanderEditor::refreshExtras()
{
    const bool byp = proc_.isBypassed();

    // ---- analytic: the slope and where it stops ----------------------------
    // The knee comes from the processor's published constant rather than a 6.0
    // typed here: this device fixes its knee instead of publishing it, so
    // getParamValue has nothing to give and a literal would be a second copy
    // waiting to disagree with the DSP.
    curve_.setCurve ((float) proc_.getParamValue (EedExpanderProcessor::kThresholdDb),
                     (float) proc_.getParamValue (EedExpanderProcessor::kRatio),
                     EedExpanderProcessor::kFixedKneeDb,
                     (float) proc_.getParamValue (EedExpanderProcessor::kRangeDb),
                     echojay::DynamicsMode::Expand);

    // ---- the live half: where on the slope the signal LIVES ----------------
    curve_.setDimmed (byp);
    curve_.setDwellSource (&proc_.dwellHistogram(), ! byp);
    curve_.setInputLevelDb (byp ? echojay::viz::TransferCurveView::kNoLevel
                                : proc_.detectorLevelDb());
    curve_.setGainReductionDb (byp ? 0.0f : proc_.gainReductionDb());
}
