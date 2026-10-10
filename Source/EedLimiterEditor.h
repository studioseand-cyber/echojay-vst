/*
    EedLimiterEditor.h  -  the editor for "EchoJay Limiter" (v2 panel, wired 9 Oct 2026 after Sean's approval).

    The shared device shell (DeviceEditorBase: logo, title, hint, BYPASS hard right) carries the header; the style
    box and the TRUE PK switch sit in it, inboard of BYPASS, as before. Everything under the header is
    EedLimiterPanelV2 - the scrolling display, the IN/OUT/GR bars, GAIN and CEILING, the LUFS column, the ADVANCED
    row (LOOKAHEAD, ATTACK, RELEASE, LINK, RLS LINK, SC HPF). The panel runs headerless here (its own header row is
    for the offline preview).

    DATA PATH. The processor's limv2::MeterTap feeds the panel (lock-free, per sample from the engine). The dials go
    the other way through one callback: every id the panel names is a schema id, set with setParamValue - the same
    funnel the assistant's moves use - and "bypass" goes to setBypassed. A 10 Hz timer reads the processor back into
    the panel's Model (so an assistant move, a restored chain, or the migration shows on the dials), only when a
    value changed, so a dial under the mouse is never fought.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedLimiterProcessor.h"
#include "EedLimiterPanelV2.h"
#include "EedDynamicsEditorSupport.h"

class EedLimiterEditor : public DeviceEditorBase, private juce::Timer
{
public:
    // 21m (22 Sep 2026): Threshold READOUT = ceiling - input gain (display only; the DSP clamps at the ceiling, the input gain pushes into it)
    static juce::String thresholdReadout (double ceilingDb, double inputDb) { return "threshold " + juce::String (ceilingDb - inputDb, 1) + " dB"; }
    explicit EedLimiterEditor (EedLimiterProcessor& p);
    ~EedLimiterEditor() override;

    // What the dials show right now - the processor read back into the panel's Model (tests: the migration shows)
    EedLimiterPanelV2::Model currentModel() const { return panel_.model(); }
    // Move a dial as a user would (tests: a dial drives its parameter); false if no dial has that id
    bool setDialForTest (const juce::String& id, double value) { return panel_.setDialValue (id, value); }
    // Read the processor into the Model now (what the timer does), callable by a test without a message loop
    void syncFromProcessor();

protected:
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override { syncFromProcessor(); }
    EedLimiterPanelV2::Model readModel() const;

    EedLimiterProcessor& limiter_;
    EedLimiterPanelV2    panel_;
    juce::ComboBox       modeBox_;
    juce::TextButton     truePeakBtn_;
    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLimiterEditor)
};
