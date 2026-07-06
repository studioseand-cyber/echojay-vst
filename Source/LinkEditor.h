#pragma once
#include <JuceHeader.h>
#include "LinkProcessor.h"

// EchoJay Link editor — resizable window sized to host plugin UIs.
// Layout: compact header row (logo, name, Active toggle, status light),
// display area filling most of the window (inline hosting lands here in
// phase 2), chain strip along the bottom, status line above the strip.
class LinkEditor : public juce::AudioProcessorEditor
{
public:
    explicit LinkEditor(LinkProcessor&);
    ~LinkEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    LinkProcessor& proc;

    static constexpr int kHeaderH = 40;
    static constexpr int kStatusH = 18;
    static constexpr int kStripH  = 76;

    juce::TextEditor   nameField;
    juce::ToggleButton toggleBtn { "Active" };

    // status light bounds — set in resized(), read in paint()
    juce::Rectangle<float> lightBounds;

    // Latest build result summary shown on the status line
    juce::String statusLine;

    juce::Rectangle<int> displayArea() const;
    juce::Rectangle<int> stripArea() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LinkEditor)
};
