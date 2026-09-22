#pragma once
#include "DeviceEditorBase.h"
#include "EedLevelProcessor.h"
#include "EchoJayDeviceLookAndFeel.h"

// EchoJay Level (18e): one GAIN dial and the two readouts the device exists for - IN and OUT short-term LUFS + true peak.
class EedLevelEditor : public DeviceEditorBase, private juce::Timer
{
public:
    explicit EedLevelEditor (EedLevelProcessor& p);
    ~EedLevelEditor() override { stopTimer(); }
    // 22 Sep 2026 (item 8): the card's readout tags are built through the UTF-8-safe constructor - the arrow is U+2192,
    // never the mis-decoded "OUT â limiter" a raw const char* round-trip produced.
    static juce::String inTag()  { return juce::String::fromUTF8 ("IN  "); }
    static juce::String outTag() { return juce::String::fromUTF8 ("OUT \xe2\x86\x92 limiter  "); }
protected:
    void layoutContent (juce::Rectangle<int> content) override;
private:
    void timerCallback() override;
    EedLevelProcessor& proc_;
    echojay::device::EchoJayDeviceKnob gainKnob_;
    juce::Label inLabel_, outLabel_, grLabel_, targetLabel_;
    bool suppressCallbacks_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLevelEditor)
};
