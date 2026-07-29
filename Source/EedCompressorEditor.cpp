/*
    EedCompressorEditor.cpp  —  see EedCompressorEditor.h.
*/

#include "EedCompressorEditor.h"

using namespace echojay::device;

namespace
{
    // Seven dials on one line, a meter under them, and a transfer curve seated
    // above. The rack sizes down from here, and the curve is the first thing to
    // give up its room (EedDynamicsFaceEditor::layoutContent).
    constexpr int kDefaultW = 540;
    constexpr int kCurveH   = 132;
    constexpr int kDefaultH = 190 + kCurveH + 6;

    // Below this the curve is a smear rather than a reading, so it is dropped
    // entirely instead of drawn uselessly small.
    constexpr int kMinCurveH = 54;

    // The device's whole front panel, in one table. Ranges and defaults are NOT
    // here — they come from the schema, so a knob cannot travel somewhere the AI
    // is not allowed to dial.
    const KnobSpec kKnobs[] = {
        { EedCompressorProcessor::kThresholdDb, "THRESH",  " dB", 1, 0.0 },
        { EedCompressorProcessor::kRatio,       "RATIO",   ":1",  2, 4.0 },

        // Attack and release are skewed so their mid-travel sits on a usable
        // value: the interesting part of a 0.1-200 ms range lives in its first
        // tenth, and a linear dial spends most of its throw where nobody sets it.
        { EedCompressorProcessor::kAttackMs,    "ATTACK",  " ms", 1, 10.0 },
        { EedCompressorProcessor::kReleaseMs,   "RELEASE", " ms", 0, 150.0 },

        { EedCompressorProcessor::kKneeDb,      "KNEE",    " dB", 1, 0.0 },
        { EedCompressorProcessor::kMakeupDb,    "MAKEUP",  " dB", 1, 0.0 },
        { EedCompressorProcessor::kMix,         "MIX",     " %",  0, 0.0 },
    };
}

EedCompressorEditor::EedCompressorEditor (EedCompressorProcessor& p)
    : EedDynamicsFaceEditor (p, "COMPRESSOR", "stereo-linked RMS compression",
                             kKnobs, (int) std::size (kKnobs),
                             24.0f,
                             [&p] { return p.gainReductionDb(); },
                             kDefaultW, kDefaultH),
      proc_ (p)
{
    curve_.setCaption ("TRANSFER");
    curve_.setFloorDb (-60.0f);
    addAndMakeVisible (curve_);

    // Seed it before the first timer tick, so the curve is already correct in
    // the frame the editor opens in rather than one 50 ms later.
    refreshExtras();
}

int EedCompressorEditor::topContentHeight() const
{
    return kCurveH;
}

void EedCompressorEditor::layoutTopContent (juce::Rectangle<int> area)
{
    // An area shorter than kMinCurveH is a rack slot that does not have room
    // for a picture; hiding it is what keeps the dials whole.
    const bool room = area.getHeight() >= kMinCurveH && area.getWidth() >= 80;

    curve_.setVisible (room);
    if (room) curve_.setBounds (area);
}

void EedCompressorEditor::refreshExtras()
{
    const bool byp = proc_.isBypassed();

    // ---- analytic: the curve is a pure function of the dialled values -------
    // Read through the same getParamValue path the knobs and an AI move use, so
    // the picture cannot be looking at a different set of numbers than the DSP.
    curve_.setCurve ((float) proc_.getParamValue (EedCompressorProcessor::kThresholdDb),
                     (float) proc_.getParamValue (EedCompressorProcessor::kRatio),
                     (float) proc_.getParamValue (EedCompressorProcessor::kKneeDb),
                     0.0f,                                   // a compressor has no range cap
                     echojay::DynamicsMode::Compress);

    curve_.setMakeupDb ((float) proc_.getParamValue (EedCompressorProcessor::kMakeupDb));

    // ---- the live half: WHERE ON THAT CURVE THE SIGNAL LIVES ---------------
    // The dwell histogram, accumulated per sample by the core and eased by the
    // view on its own 60 Hz timer. A threshold set 10 dB above where the music
    // actually sits looks identical on the knobs and is obvious the moment the
    // curve glows nowhere near its knee.
    curve_.setDimmed (byp);
    curve_.setDwellSource (&proc_.dwellHistogram(), ! byp);
    curve_.setInputLevelDb (byp ? echojay::viz::TransferCurveView::kNoLevel
                                : proc_.detectorLevelDb());
    curve_.setGainReductionDb (byp ? 0.0f : proc_.gainReductionDb());
}
