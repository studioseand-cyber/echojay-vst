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
protected:
    void layoutContent (juce::Rectangle<int> content) override;
private:
    void timerCallback() override;
    EedLevelProcessor& proc_;
    echojay::device::EchoJayDeviceKnob gainKnob_;
    juce::Label inLabel_, outLabel_, targetLabel_;
    bool suppressCallbacks_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLevelEditor)
};
