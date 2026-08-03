/*
    EedTemplateEditor.h  —  SKELETON. Copy to Source/, rename Template -> your
    device. Not compiled where it sits (see DeviceTemplate/README.md).

    DeviceEditorBase supplies the EchoJay identity: dark background, logo
    top-left, device title, BYPASS, the shared filmstrip dial, and the inline
    chain-hosting sizing contract. A device editor is then "place my controls".
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedTemplateProcessor.h"

class EedTemplateEditor : public DeviceEditorBase,
                          private juce::Timer
{
public:
    explicit EedTemplateEditor (EedTemplateProcessor& p);
    ~EedTemplateEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

    // Optional: device-global controls in the header, either side of BYPASS.
    // void layoutHeaderTrailing (juce::Rectangle<int>& bar) override;
    // void layoutHeaderLeading  (juce::Rectangle<int>& bar) override;

    // Optional: extra painting under the header (meters, curves).
    // void paintContent (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void syncFromProcessor();

    EedTemplateProcessor& proc_;

    echojay::device::EchoJayDeviceKnob amountKnob_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedTemplateEditor)
};
