#pragma once
#include <JuceHeader.h>
#include "LinkProcessor.h"

class LinkEditor : public juce::AudioProcessorEditor
{
public:
    explicit LinkEditor(LinkProcessor&);
    ~LinkEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    LinkProcessor& proc;

    juce::Label        nameLabel   { {}, "Link name" };
    juce::TextEditor   nameField;
    juce::ToggleButton toggleBtn   { "Active" };

    // status light bounds — set in resized(), read in paint()
    juce::Rectangle<float> lightBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LinkEditor)
};
