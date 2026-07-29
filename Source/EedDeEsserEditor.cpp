/*
    EedDeEsserEditor.cpp  —  see EedDeEsserEditor.h.
*/

#include "EedDeEsserEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    constexpr int kDefaultW = 460;
    constexpr int kDefaultH = 190;

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

    refreshSwitchText();
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
}
