/*
    EedLimiterEditor.cpp  -  see EedLimiterEditor.h.
*/

#include "EedLimiterEditor.h"
#include "ChainWetKnob.h"   // the EchoJay filmstrip the panel's dials draw with

using namespace echojay::device;

namespace
{
    constexpr int kModeW = 104, kTruePeakW = 62;
    // the panel's default plus the shell's header
    constexpr int kDefaultW = EedLimiterPanelV2::kDefaultW, kDefaultH = EedLimiterPanelV2::kDefaultH + 34;
}

EedLimiterEditor::EedLimiterEditor (EedLimiterProcessor& p)
    : DeviceEditorBase (p, "LIMITER", kDefaultW, kDefaultH), limiter_ (p), panel_ (p.meterTap())
{
    setHeaderHint ("true peak, fixed latency");

    bindChoiceBox (modeBox_, EedLimiterProcessor::kMode, EedLimiterProcessor::schema(), limiter_, &suppressCallbacks_);
    addAndMakeVisible (modeBox_);
    bindToggle (truePeakBtn_, EedLimiterProcessor::kTruePeak, "TRUE PK", EedLimiterProcessor::schema(), limiter_, &suppressCallbacks_);
    addAndMakeVisible (truePeakBtn_);

    panel_.setHeaderless (true);
    panel_.setKnobFilmstrip (ChainWetKnob::filmstrip());
    addAndMakeVisible (panel_);

    syncFromProcessor();
    auto m = panel_.model();
    m.onChange = [this] (const juce::String& id, double value)
    {
        if (id == "bypass") { limiter_.setBypassed (value >= 0.5); bypassButton().setToggleState (value >= 0.5, juce::dontSendNotification); return; }
        limiter_.setParamValue (id, value);   // every dial id is a schema id: input_db, ceiling_db, mode, true_peak, lookahead_ms, attack_ms, release_ms, link_pct, release_link_pct, sc_hpf_hz
    };
    panel_.setModel (m);
    startTimerHz (10);
}

EedLimiterEditor::~EedLimiterEditor() { stopTimer(); }

EedLimiterPanelV2::Model EedLimiterEditor::readModel() const
{
    EedLimiterPanelV2::Model m = panel_.model();   // keeps onChange
    m.gainDb         = limiter_.getParamValue (EedLimiterProcessor::kInputDb);
    m.ceilingDb      = limiter_.getParamValue (EedLimiterProcessor::kCeilingDb);
    m.style          = (int) std::lround (limiter_.getParamValue (EedLimiterProcessor::kMode));
    m.truePeak       = limiter_.getParamValue (EedLimiterProcessor::kTruePeak) >= 0.5;
    m.bypassed       = limiter_.isBypassed();
    m.lookaheadMs    = limiter_.getParamValue (EedLimiterProcessor::kLookaheadMs);
    m.attackMs       = limiter_.getParamValue (EedLimiterProcessor::kAttackMs);
    m.releaseMs      = limiter_.getParamValue (EedLimiterProcessor::kReleaseMs);
    m.linkPct        = limiter_.getParamValue (EedLimiterProcessor::kLinkPct);
    m.releaseLinkPct = limiter_.getParamValue (EedLimiterProcessor::kReleaseLinkPct);
    m.scHpfHz        = limiter_.getParamValue (EedLimiterProcessor::kScHpfHz);
    m.latencySamples = limiter_.getLatencySamples();
    m.sampleRate     = limiter_.getSampleRate();
    return m;
}

void EedLimiterEditor::syncFromProcessor()
{
    const auto m = readModel();
    if (! m.sameValues (panel_.model())) panel_.setModel (m);
    {
        const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);
        syncChoiceBox (modeBox_, EedLimiterProcessor::kMode, limiter_);
        syncToggle (truePeakBtn_, EedLimiterProcessor::kTruePeak, limiter_);
    }
}

void EedLimiterEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    // Filled from the right, inboard of BYPASS, so MODE ends up leftmost: it is the control that decides what the rest means.
    truePeakBtn_.setBounds (bar.removeFromRight (juce::jmin (kTruePeakW, juce::jmax (0, bar.getWidth()))).reduced (0, 3));
    bar.removeFromRight (6);
    modeBox_.setBounds (bar.removeFromRight (juce::jmin (kModeW, juce::jmax (0, bar.getWidth()))).reduced (0, 3));
    bar.removeFromRight (6);
}

void EedLimiterEditor::layoutContent (juce::Rectangle<int> content)
{
    panel_.setBounds (content);
}
