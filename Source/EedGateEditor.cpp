/*
    EedGateEditor.cpp  —  see EedGateEditor.h.
*/

#include "EedGateEditor.h"

using namespace echojay::device;

namespace
{
    // Wider than before: the header carries a MODE selector inboard of BYPASS,
    // and ten dials wrap onto a second line.
    constexpr int kDefaultW = 540;
    constexpr int kCurveH   = 132;
    constexpr int kDefaultH = 190 + metrics::kKnobH + 4 + kCurveH + 6;

    constexpr int kModeW = 84;

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

        // The selective-trigger pair, side by side and in frequency order, so
        // "band the detector between these two" reads off the panel.
        { EedGateProcessor::kScHpfHz,      "SC HPF", " Hz", 0, 120.0 },
        { EedGateProcessor::kScLpfHz,      "SC LPF", " Hz", 0, 2000.0 },
        { EedGateProcessor::kLookaheadMs,  "LOOK",   " ms", 2, 0.0 },
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
    bindChoiceBox (modeBox_, EedGateProcessor::kMode,
                   EedGateProcessor::schema(), proc_, &suppressCallbacks_);
    addAndMakeVisible (modeBox_);

    curve_.setCaption ("GATE");
    curve_.setFloorDb (kFloorDb);
    addAndMakeVisible (curve_);

    // Seed it before the first timer tick, so the curve is already correct in
    // the frame the editor opens in rather than one 50 ms later.
    refreshExtras();
}

void EedGateEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    modeBox_.setBounds (bar.removeFromRight (juce::jmin (kModeW, juce::jmax (0, bar.getWidth())))
                           .reduced (0, 3));
    bar.removeFromRight (6);
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

    // The AI can flip the mode while the editor is open, so the selector follows
    // the processor rather than assuming it is the only thing that writes it.
    {
        const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);
        syncChoiceBox (modeBox_, EedGateProcessor::kMode, proc_);
    }

    // The caption and the hint say which of the two devices this currently is.
    // A ducker labelled GATE is a control surface lying about its own function.
    const bool ducking = proc_.isDucking();

    const juce::String hint = ducking
        ? "duck - pulls DOWN what is louder than the threshold"
        : "hold + hysteresis, attenuates by range";

    if (hint != lastHint_)
    {
        lastHint_ = hint;
        setHeaderHint (hint);
        curve_.setCaption (ducking ? "DUCK" : "GATE");
    }

    // ---- analytic: the step, from the dialled values ----------------------
    // Ratio and knee are passed as 0 rather than omitted, because neither Gate
    // nor Duck mode uses them — passing anything else would suggest they matter
    // here, and a reader checking whether the picture is honest should not have
    // to open GainCurve to find out that they do not.
    //
    // The MODE comes from the core, not from a second reading of the param: the
    // step in the picture turns over because the DSP's step did.
    curve_.setCurve ((float) proc_.getParamValue (EedGateProcessor::kThresholdDb),
                     0.0f,                                       // ratio: unused
                     0.0f,                                       // knee:  unused
                     (float) proc_.getParamValue (EedGateProcessor::kRangeDb),
                     proc_.dynamicsMode());

    // The second edge. This is the parameter a gate is hardest to set by ear —
    // you cannot hear the chatter you are avoiding — so it is the one most worth
    // a picture.
    curve_.setHysteresisDb ((float) proc_.getParamValue (EedGateProcessor::kHysteresisDb));

    // ---- the live half: which side of the step the signal LIVES on ---------
    // A gate is the face this helps most: the glow straddling the two edges of
    // the hysteresis band is exactly the chatter the device is there to avoid,
    // and it is a shape rather than something you have to catch happening.
    curve_.setDimmed (byp);
    curve_.setDwellSource (&proc_.dwellHistogram(), ! byp);
    curve_.setInputLevelDb (byp ? echojay::viz::TransferCurveView::kNoLevel
                                : proc_.detectorLevelDb());
    curve_.setGainReductionDb (byp ? 0.0f : proc_.gainReductionDb());
}
