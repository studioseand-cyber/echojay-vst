#include "EedLevelEditor.h"
namespace { constexpr int kDefaultW = 300, kDefaultH = 150 + 3 * 18 + 12; }
using C = echojay::device::Colours;
EedLevelEditor::EedLevelEditor (EedLevelProcessor& p) : DeviceEditorBase (p, "LEVEL", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("loudness loop gain");
    if (const auto* spec = EedLevelProcessor::schema().find (EedLevelProcessor::kGainDb))
    {
        gainKnob_.setSpec (spec->min, spec->max, 0.0, 1, " dB", "GAIN", spec->def);
        gainKnob_.setRealValue (proc_.getParamValue (EedLevelProcessor::kGainDb));
        gainKnob_.onValueChange = [this] { if (! suppressCallbacks_) proc_.setParamValue (EedLevelProcessor::kGainDb, gainKnob_.getRealValue()); };
        addAndMakeVisible (gainKnob_);
    }
    for (auto* l : { &inLabel_, &outLabel_, &targetLabel_ })
    {
        l->setJustificationType (juce::Justification::centred);
        l->setFont (echojay::device::uiFont (10.0f));
        l->setColour (juce::Label::textColourId, C::text2);
        addAndMakeVisible (*l);
    }
    startTimerHz (10);
}
void EedLevelEditor::layoutContent (juce::Rectangle<int> content)
{
    auto rows = content.removeFromBottom (3 * 18 + 6);
    gainKnob_.setBounds (content.withSizeKeepingCentre (juce::jmin (content.getWidth(), 120), juce::jmin (content.getHeight(), 120)));
    inLabel_.setBounds (rows.removeFromTop (18)); outLabel_.setBounds (rows.removeFromTop (18)); targetLabel_.setBounds (rows.removeFromTop (18));
}
void EedLevelEditor::timerCallback()
{
    {   const juce::ScopedValueSetter<bool> g (suppressCallbacks_, true);
        const double v = proc_.getParamValue (EedLevelProcessor::kGainDb);
        if (std::abs (gainKnob_.getRealValue() - v) > 0.005) gainKnob_.setRealValue (v); }
    auto fmt = [] (const echojay::LevelTally::Snapshot& s, const char* tag)
    {
        juce::String t (tag);
        t += std::isfinite (s.shortTermDb) ? juce::String (s.shortTermDb, 1) + " LUFS-S" : juce::String ("-- LUFS-S");
        t += "  " + (s.truePeakDb > -150.0f ? juce::String (s.truePeakDb, 1) + " dBTP" : juce::String ("-- dBTP"));
        return t;
    };
    inLabel_.setText (fmt (proc_.inputLevels(), "IN  "), juce::dontSendNotification);
    outLabel_.setText (fmt (proc_.outputLevels(), "OUT "), juce::dontSendNotification);
    targetLabel_.setText ("target " + juce::String (proc_.targetLufs(), 1) + " LUFS (" + juce::String (EedLevelProcessor::optionName (proc_.loudnessOption())) + ")", juce::dontSendNotification);
}
