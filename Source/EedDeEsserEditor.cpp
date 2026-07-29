/*
    EedDeEsserEditor.cpp  —  see EedDeEsserEditor.h.
*/

#include "EedDeEsserEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Wider than the five dials need: the top strip carries two views side by
    // side, and a frequency axis squeezed under 240 px cannot separate 5 kHz
    // from 8 kHz, which is the distinction this device exists to make.
    constexpr int kDefaultW = 560;
    constexpr int kCurveH   = 132;
    constexpr int kDefaultH = 190 + kCurveH + 6;

    constexpr int kMinCurveH = 54;

    // The transfer curve's share of the strip, and the width below which it is
    // dropped so the band view keeps a usable axis. The BAND view is the one
    // that survives, because a de-esser without a frequency axis is a
    // compressor with an unexplained filter in it.
    constexpr int kTransferW    = 190;
    constexpr int kMinBandW     = 200;
    constexpr int kViewGap      = 6;

    constexpr int kModeW   = 62;
    constexpr int kListenW = 62;

    const KnobSpec kKnobs[] = {
        // Frequency is skewed to 4 kHz so mid-travel lands in vocal sibilance
        // rather than in the middle of a linear 1-16 kHz sweep, which is 8.5 kHz
        // and above almost every "s" worth chasing.
        { EedDeEsserProcessor::kFreqHz,      "FREQ",    " Hz", 0, 4000.0 },
        { EedDeEsserProcessor::kThresholdDb, "THRESH",  " dB", 1, 0.0 },
        { EedDeEsserProcessor::kRangeDb,     "RANGE",   " dB", 1, 0.0 },
        { EedDeEsserProcessor::kAttackMs,    "ATTACK",  " ms", 2, 1.0 },
        { EedDeEsserProcessor::kReleaseMs,   "RELEASE", " ms", 0, 60.0 },
    };
}

EedDeEsserEditor::EedDeEsserEditor (EedDeEsserProcessor& p)
    : EedDynamicsFaceEditor (p, "DE-ESSER", "band-triggered sibilance control",
                             kKnobs, (int) std::size (kKnobs),
                             24.0f,
                             [&p] { return p.gainReductionDb(); },
                             kDefaultW, kDefaultH),
      deEsser_ (p)
{
    styleButton (modeBtn_,   true);
    styleButton (listenBtn_, true);

    // Both switches go through setParamValue, exactly as an AI move does, so a
    // click and a dialled `mode` cannot end up meaning different things.
    modeBtn_.setToggleState (deEsser_.isSplitMode(), juce::dontSendNotification);
    modeBtn_.onClick = [this]
    {
        deEsser_.setParamValue (EedDeEsserProcessor::kMode,
                                modeBtn_.getToggleState() ? 1.0 : 0.0);
        refreshSwitchText();
    };
    addAndMakeVisible (modeBtn_);

    listenBtn_.setToggleState (deEsser_.isListening(), juce::dontSendNotification);
    listenBtn_.onClick = [this]
    {
        deEsser_.setParamValue (EedDeEsserProcessor::kListen,
                                listenBtn_.getToggleState() ? 1.0 : 0.0);
        refreshSwitchText();
    };
    addAndMakeVisible (listenBtn_);

    curve_.setCaption ("TRANSFER");
    // The sidechain is a narrow band, so its level sits well below the full
    // signal's. -60 keeps a quiet "s" on the plot instead of pinned at the left
    // edge with nothing to read.
    curve_.setFloorDb (-60.0f);
    addAndMakeVisible (curve_);

    band_.setCaption ("BAND");
    addAndMakeVisible (band_);

    refreshSwitchText();

    // Seed both before the first timer tick, so they are correct in the frame
    // the editor opens in rather than one 50 ms later.
    refreshExtras();
}

int EedDeEsserEditor::topContentHeight() const
{
    return kCurveH;
}

void EedDeEsserEditor::layoutTopContent (juce::Rectangle<int> area)
{
    const bool anyRoom = area.getHeight() >= kMinCurveH && area.getWidth() >= 120;

    if (! anyRoom)
    {
        curve_.setVisible (false);
        band_.setVisible (false);
        return;
    }

    // Both only when the band view still gets a readable axis after the transfer
    // curve has taken its share. Otherwise the band view takes the whole strip.
    const bool bothFit = area.getWidth() >= kTransferW + kViewGap + kMinBandW;

    curve_.setVisible (bothFit);
    band_.setVisible (true);

    if (bothFit)
    {
        curve_.setBounds (area.removeFromLeft (kTransferW));
        area.removeFromLeft (kViewGap);
    }

    band_.setBounds (area);
}

void EedDeEsserEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    // Filled from the right, inboard of BYPASS, so LISTEN sits nearest it —
    // the switch most likely to be reached for in a hurry.
    listenBtn_.setBounds (bar.removeFromRight (juce::jmin (kListenW, juce::jmax (0, bar.getWidth())))
                              .reduced (0, 3));
    bar.removeFromRight (6);
    modeBtn_.setBounds (bar.removeFromRight (juce::jmin (kModeW, juce::jmax (0, bar.getWidth())))
                            .reduced (0, 3));
    bar.removeFromRight (6);
}

void EedDeEsserEditor::refreshSwitchText()
{
    // The button says what the device IS doing, not what clicking it would do.
    // A two-state control labelled with its action is the classic ambiguity.
    modeBtn_.setButtonText (modeBtn_.getToggleState() ? "SPLIT" : "WIDE");
    listenBtn_.setButtonText ("LISTEN");

    setHeaderHint (listenBtn_.getToggleState()
                       ? "LISTEN on - monitoring the detector band, not the result"
                       : "band-triggered sibilance control");
}

void EedDeEsserEditor::refreshExtras()
{
    // The AI can flip these while the editor is open, so the switches follow the
    // processor rather than assuming they are the only thing that writes it.
    bool changed = false;

    if (modeBtn_.getToggleState() != deEsser_.isSplitMode())
    {
        modeBtn_.setToggleState (deEsser_.isSplitMode(), juce::dontSendNotification);
        changed = true;
    }
    if (listenBtn_.getToggleState() != deEsser_.isListening())
    {
        listenBtn_.setToggleState (deEsser_.isListening(), juce::dontSendNotification);
        changed = true;
    }

    if (changed) refreshSwitchText();

    const bool byp    = deEsser_.isBypassed();
    const bool listen = deEsser_.isListening();
    const bool split  = deEsser_.isSplitMode();

    const float freq   = (float) deEsser_.getParamValue (EedDeEsserProcessor::kFreqHz);
    const float thresh = (float) deEsser_.getParamValue (EedDeEsserProcessor::kThresholdDb);
    const float range  = (float) deEsser_.getParamValue (EedDeEsserProcessor::kRangeDb);

    // ---- HOW MUCH: the curve the sidechain level is compressed against ------
    // Ratio and knee come from the processor's published constants rather than
    // literals here: this device fixes both instead of publishing them, so
    // getParamValue has nothing to give and a re-typed 8.0 is a second copy
    // waiting to disagree with the DSP.
    curve_.setCurve (thresh,
                     EedDeEsserProcessor::kFixedRatio,
                     EedDeEsserProcessor::kFixedKneeDb,
                     range,
                     echojay::DynamicsMode::Compress);

    // ---- WHERE: the band, and what the device does to the spectrum ---------
    band_.setBand (freq, EedDeEsserProcessor::kSidechainQ);
    band_.setSplitMode (split);
    band_.setThresholdDb (thresh);

    // The plot runs to the range knob's own ceiling, so the picture never runs
    // out of depth before the device does.
    band_.setDepthDb (juce::jmax (12.0f, range));
    band_.setListening (listen);

    // ---- the live floats ----------------------------------------------------
    // Note this is the SIDECHAIN's level, not the input's: the dot's position on
    // the transfer curve is "how much sibilance is there", which is the only
    // reading against which this device's threshold means anything.
    const float level = deEsser_.detectorLevelDb();
    const float gr    = deEsser_.gainReductionDb();

    curve_.setDimmed (byp);
    curve_.setInputLevelDb (byp ? echojay::viz::TransferCurveView::kNoLevel : level);
    curve_.setGainReductionDb (byp ? 0.0f : gr);

    band_.setDimmed (byp);
    band_.setBandLevelDb (byp ? echojay::viz::DeEsserBandView::kNoLevel : level);

    // In LISTEN the device is outputting the band, not the processed signal, so
    // the "what moves" curve would be drawing processing that is not reaching
    // the output. Flattened to zero, and the view says why.
    band_.setGainReductionDb ((byp || listen) ? 0.0f : gr);
}
