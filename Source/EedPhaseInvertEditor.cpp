/*
    EedPhaseInvertEditor.cpp  —  see EedPhaseInvertEditor.h.
*/

#include "EedPhaseInvertEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    constexpr int kDefaultW = 340;
    constexpr int kDefaultH = 130;
    constexpr int kBtnW     = 110;
}

EedPhaseInvertEditor::EedPhaseInvertEditor (EedPhaseInvertProcessor& p)
    : DeviceEditorBase (p, "PHASE", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("per-channel polarity");

    auto setup = [this] (juce::TextButton& b, const char* id)
    {
        styleButton (b, true);
        b.setToggleState (proc_.getParamValue (juce::String (id)) >= 0.5,
                          juce::dontSendNotification);
        // Through the schema path, exactly as an AI move would — one path, so a
        // click and a dial cannot disagree.
        b.onClick = [this, &b, id]
        {
            proc_.setParamValue (juce::String (id), b.getToggleState() ? 1.0 : 0.0);
        };
        addAndMakeVisible (b);
    };

    setup (leftBtn_,  EedPhaseInvertProcessor::kInvertL);
    setup (rightBtn_, EedPhaseInvertProcessor::kInvertR);

    startTimerHz (15);
}

EedPhaseInvertEditor::~EedPhaseInvertEditor()
{
    stopTimer();
}

void EedPhaseInvertEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    const int pairW = kBtnW * 2 + 16;
    auto row = content.withSizeKeepingCentre (juce::jmin (pairW, content.getWidth()),
                                              juce::jmin (kRowH, content.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - 16) / 2);
    leftBtn_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (16);
    rightBtn_.setBounds (row.removeFromLeft (colW));
}

void EedPhaseInvertEditor::syncFromProcessor()
{
    // The AI can flip these while the editor is open.
    const bool l = proc_.getParamValue (EedPhaseInvertProcessor::kInvertL) >= 0.5;
    const bool r = proc_.getParamValue (EedPhaseInvertProcessor::kInvertR) >= 0.5;

    if (leftBtn_.getToggleState()  != l) leftBtn_.setToggleState  (l, juce::dontSendNotification);
    if (rightBtn_.getToggleState() != r) rightBtn_.setToggleState (r, juce::dontSendNotification);
}

void EedPhaseInvertEditor::timerCallback()
{
    syncFromProcessor();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
