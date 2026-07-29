/*
    EedGateEditor.cpp  —  see EedGateEditor.h.
*/

#include "EedGateEditor.h"

using namespace echojay::device;

namespace
{
    constexpr int kDefaultW = 480;
    constexpr int kCurveH   = 132;
    constexpr int kDefaultH = 190 + kCurveH + 6;

    // Below this the curve is a smear rather than a reading, so it is dropped
    // entirely instead of drawn uselessly small. Same threshold the compressor
    // uses — the shrink behaviour should not differ device to device.
    constexpr int kMinCurveH = 54;

    // A gate lives further down than a compressor does: its threshold dials to
    // -80 dB and its range reaches 80. A -60 floor would put the whole
    // interesting part of the picture off the bottom of the plot.
    constexpr float kFloorDb = -90.0f;

    const KnobSpec kKnobs[] = {
        { EedGateProcessor::kThresholdDb,  "THRESH", " dB", 1, 0.0 },
        { EedGateProcessor::kRangeDb,      "RANGE",  " dB", 1, 0.0 },
        { EedGateProcessor::kAttackMs,     "ATTACK", " ms", 2, 1.0 },
        { EedGateProcessor::kHoldMs,       "HOLD",   " ms", 0, 50.0 },
        { EedGateProcessor::kReleaseMs,    "RELEASE"," ms", 0, 200.0 },
        { EedGateProcessor::kHysteresisDb, "HYST",   " dB", 1, 0.0 },
    };
}

EedGateEditor::EedGateEditor (EedGateProcessor& p)
    // The meter scale follows the gate's own reach: a gate routinely pulls 40 dB
    // down, and a 24 dB scale would sit pinned at the bottom saying nothing.
    : EedDynamicsFaceEditor (p, "GATE", "hold + hysteresis, attenuates by range",
                             kKnobs, (int) std::size (kKnobs),
                             60.0f,
                             [&p] { return p.gainReductionDb(); },
                             kDefaultW, kDefaultH),
      proc_ (p)
{
    curve_.setCaption ("GATE");
    curve_.setFloorDb (kFloorDb);
    addAndMakeVisible (curve_);

    // Seed it before the first timer tick, so the curve is already correct in
    // the frame the editor opens in rather than one 50 ms later.
    refreshExtras();
}

int EedGateEditor::topContentHeight() const
{
    return kCurveH;
}

void EedGateEditor::layoutTopContent (juce::Rectangle<int> area)
{
    const bool room = area.getHeight() >= kMinCurveH && area.getWidth() >= 80;

    curve_.setVisible (room);
    if (room) curve_.setBounds (area);
}

void EedGateEditor::refreshExtras()
{
    const bool byp = proc_.isBypassed();

    // ---- analytic: the step, from the dialled values ----------------------
    // Ratio and knee are passed as 0 rather than omitted, because Gate mode
    // ignores both — passing anything else would suggest they matter here, and
    // a reader checking whether the picture is honest should not have to open
    // GainCurve to find out that they do not.
    curve_.setCurve ((float) proc_.getParamValue (EedGateProcessor::kThresholdDb),
                     0.0f,                                       // ratio: unused by a gate
                     0.0f,                                       // knee:  unused by a gate
                     (float) proc_.getParamValue (EedGateProcessor::kRangeDb),
                     echojay::DynamicsMode::Gate);

    // The second edge. This is the parameter a gate is hardest to set by ear —
    // you cannot hear the chatter you are avoiding — so it is the one most worth
    // a picture.
    curve_.setHysteresisDb ((float) proc_.getParamValue (EedGateProcessor::kHysteresisDb));

    // ---- one float tap: which side of the step the signal is on ------------
    curve_.setDimmed (byp);
    curve_.setInputLevelDb (byp ? echojay::viz::TransferCurveView::kNoLevel
                                : proc_.detectorLevelDb());
    curve_.setGainReductionDb (byp ? 0.0f : proc_.gainReductionDb());
}
