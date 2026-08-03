/*
    EedCompressorEditor.cpp  —  see EedCompressorEditor.h.
*/

#include "EedCompressorEditor.h"

using namespace echojay::device;

namespace
{
    // Eleven dials on two lines, a meter under them, and a transfer curve seated
    // above. The rack sizes down from here, and the curve is the first thing to
    // give up its room (EedDynamicsFaceEditor::layoutContent).
    //
    // 560 wide fits eight dial columns on the first line, so the seven original
    // knobs stay together and the depth pass's four wrap onto the second — which
    // is the grouping a user would have chosen anyway.
    constexpr int kDefaultW = 560;
    constexpr int kCurveH   = 132;

    // Two dial rows rather than one: 190 was the single-row height, and a second
    // row of kKnobH plus its gap is what the depth pass costs.
    constexpr int kDefaultH = 190 + metrics::kKnobH + 4 + kCurveH + 6;

    // Below this the curve is a smear rather than a reading, so it is dropped
    // entirely instead of drawn uselessly small.
    constexpr int kMinCurveH = 54;

    constexpr int kModeW     = 92;
    constexpr int kDetectorW = 66;
    constexpr int kAutoW     = 58;

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
        { EedCompressorProcessor::kRangeDb,     "RANGE",   " dB", 1, 0.0 },
        { EedCompressorProcessor::kMakeupDb,    "MAKEUP",  " dB", 1, 0.0 },
        { EedCompressorProcessor::kMix,         "MIX",     " %",  0, 0.0 },

        // Skewed to 120 Hz: the whole point of a sidechain high-pass is the
        // 80-150 Hz region, which is the first fifth of a 0-500 range.
        { EedCompressorProcessor::kScHpfHz,     "SC HPF",  " Hz", 0, 120.0 },
        { EedCompressorProcessor::kLookaheadMs, "LOOK",    " ms", 2, 0.0 },
        { EedCompressorProcessor::kStereoLink,  "LINK",    " %",  0, 0.0 },
    };
}

EedCompressorEditor::EedCompressorEditor (EedCompressorProcessor& p)
    : EedDynamicsFaceEditor (p, "COMPRESSOR", "stereo-linked compression",
                             kKnobs, (int) std::size (kKnobs),
                             24.0f,
                             [&p] { return p.gainReductionDb(); },
                             kDefaultW, kDefaultH),
      proc_ (p)
{
    // All three go through setParamValue, exactly as an AI move does, so a click
    // and a dialled param cannot end up meaning different things.
    bindChoiceBox (modeBox_, EedCompressorProcessor::kMode,
                   EedCompressorProcessor::schema(), proc_, &suppressCallbacks_);
    addAndMakeVisible (modeBox_);

    bindChoiceBox (detectorBox_, EedCompressorProcessor::kDetector,
                   EedCompressorProcessor::schema(), proc_, &suppressCallbacks_);
    addAndMakeVisible (detectorBox_);

    bindToggle (autoRelBtn_, EedCompressorProcessor::kAutoRelease, "AUTO REL",
                EedCompressorProcessor::schema(), proc_, &suppressCallbacks_);
    addAndMakeVisible (autoRelBtn_);

    curve_.setCaption ("TRANSFER");
    curve_.setFloorDb (-60.0f);
    addAndMakeVisible (curve_);

    // Seed it before the first timer tick, so the curve is already correct in
    // the frame the editor opens in rather than one 50 ms later.
    refreshHint();
    refreshExtras();
}

void EedCompressorEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    // Filled from the RIGHT, inboard of BYPASS, so MODE ends up leftmost of the
    // three — it is the one that changes what everything else does, so it reads
    // first even though it is placed last.
    auto take = [&bar] (juce::Component& c, int want)
    {
        c.setBounds (bar.removeFromRight (juce::jmin (want, juce::jmax (0, bar.getWidth())))
                        .reduced (0, 3));
        bar.removeFromRight (6);
    };

    take (autoRelBtn_,  kAutoW);
    take (detectorBox_, kDetectorW);
    take (modeBox_,     kModeW);
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

void EedCompressorEditor::refreshHint()
{
    // What the character mode MADE of the dialled times, not what the dials say.
    // The knobs already show the dialled numbers; this line is the only place the
    // effective ones appear, and without it a mode that quarters the attack is
    // invisible on the panel.
    const juce::String mode (echojay::characterName (proc_.character()));

    juce::String hint = mode + " - attack "
                      + juce::String (proc_.effectiveAttackMs(), 1) + " ms, release "
                      + juce::String (proc_.effectiveReleaseMs(), 0) + " ms";

    if (proc_.getParamValue (EedCompressorProcessor::kAutoRelease) >= 0.5)
        hint += " (auto)";

    // Only when it actually changed: setHeaderHint repaints, and this is called
    // from a 20 Hz timer. Repainting the whole editor five times a second to
    // redraw the same string is the kind of cost that only shows up with six
    // devices open at once.
    if (hint == lastHint_) return;

    lastHint_ = hint;
    setHeaderHint (hint);
}

void EedCompressorEditor::refreshExtras()
{
    const bool byp = proc_.isBypassed();

    // The AI can move these while the editor is open, so the controls follow the
    // processor rather than assuming they are the only thing that writes it.
    {
        const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

        syncChoiceBox (modeBox_,     EedCompressorProcessor::kMode,     proc_);
        syncChoiceBox (detectorBox_, EedCompressorProcessor::kDetector, proc_);
        syncToggle    (autoRelBtn_,  EedCompressorProcessor::kAutoRelease, proc_);
    }

    // Unconditionally, because the effective times move with the ATTACK and
    // RELEASE dials as well as with the mode. refreshHint is its own repaint
    // gate, so calling it every tick costs a string compare.
    refreshHint();

    // ---- analytic: the curve is a pure function of the dialled values -------
    // Read through the same getParamValue path the knobs and an AI move use, so
    // the picture cannot be looking at a different set of numbers than the DSP.
    // The KNEE is the one exception, and deliberately: the character mode
    // reshapes it, so the picture has to be drawn from what the core is running
    // rather than from what the dial says.
    curve_.setCurve ((float) proc_.getParamValue (EedCompressorProcessor::kThresholdDb),
                     (float) proc_.getParamValue (EedCompressorProcessor::kRatio),
                     proc_.effectiveKneeDb(),
                     (float) proc_.getParamValue (EedCompressorProcessor::kRangeDb),
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
