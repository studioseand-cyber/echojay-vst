/*
    EedTemplateEditor.cpp  —  SKELETON. Copy to Source/, rename Template -> your
    device. Not compiled where it sits (see DeviceTemplate/README.md).
*/

#include "EedTemplateEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // The size the rack opens at. layoutContent must still survive being given
    // less than this — that is the inline-hosting contract.
    constexpr int kDefaultW = 360;
    constexpr int kDefaultH = 150;
}

EedTemplateEditor::EedTemplateEditor (EedTemplateProcessor& p)
    : DeviceEditorBase (p, "TEMPLATE", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("what this device does, in three words");

    // Ranges come from the SCHEMA, never re-typed here: the knob physically
    // cannot travel somewhere the AI is not allowed to dial, and widening one
    // without the other becomes impossible.
    if (const auto* spec = EedTemplateProcessor::schema().find (EedTemplateProcessor::kAmount))
    {
        amountKnob_.setSpec (spec->min, spec->max,
                             0.0,               // skew midpoint, 0 = linear
                             1,                 // decimals in the readout
                             " %",              // unit suffix
                             "AMOUNT",          // caption
                             spec->def);
        amountKnob_.setRealValue (proc_.getParamValue (EedTemplateProcessor::kAmount));

        // Drive the processor THROUGH the schema path, exactly as an AI move
        // does — one path, so a knob turn and a dial cannot disagree.
        amountKnob_.onValueChange = [this]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (EedTemplateProcessor::kAmount, amountKnob_.getRealValue());
        };
        addAndMakeVisible (amountKnob_);
    }

    // The AI can move these while the editor is open, so poll for changes the UI
    // did not make.
    startTimerHz (15);
}

EedTemplateEditor::~EedTemplateEditor()
{
    stopTimer();
}

void EedTemplateEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    amountKnob_.setBounds (content.withSizeKeepingCentre (
        juce::jmin (kKnobW, content.getWidth()),
        juce::jmin (kKnobH, content.getHeight())));
}

void EedTemplateEditor::syncFromProcessor()
{
    const double v = proc_.getParamValue (EedTemplateProcessor::kAmount);

    // Only write when it actually moved: setRealValue would otherwise fight a
    // drag in progress by snapping the knob to the value it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);
    if (std::abs (v - amountKnob_.getRealValue()) > 1.0e-4)
        amountKnob_.setRealValue (v);
}

void EedTemplateEditor::timerCallback()
{
    syncFromProcessor();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
