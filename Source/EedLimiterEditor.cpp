/*
    EedLimiterEditor.cpp  —  see EedLimiterEditor.h.
*/

#include "EedLimiterEditor.h"

using namespace echojay::device;

// Qualified alias, not the unqualified name: `Colours` is ambiguous against
// juce::Colours, which JuceHeader.h pulls into scope. Same convention as
// SurgicalEqEditor and DeviceEditorBase.
using C = echojay::device::Colours;

namespace
{
    constexpr int kDefaultW = 340;
    constexpr int kDefaultH = 200;

    const KnobSpec kKnobs[] = {
        { EedLimiterProcessor::kCeilingDb,   "CEILING",   " dB", 2, 0.0 },
        { EedLimiterProcessor::kReleaseMs,   "RELEASE",   " ms", 0, 80.0 },
        { EedLimiterProcessor::kLookaheadMs, "LOOKAHEAD", " ms", 2, 0.0 },
    };
}

EedLimiterEditor::EedLimiterEditor (EedLimiterProcessor& p)
    // 12 dB of scale: a limiter working properly lives in the first few dB, and
    // a 24 dB scale would make normal operation look like nothing is happening.
    : EedDynamicsFaceEditor (p, "LIMITER", "brick wall, lookahead",
                             kKnobs, (int) std::size (kKnobs),
                             12.0f,
                             [&p] { return p.gainReductionDb(); },
                             kDefaultW, kDefaultH),
      limiter_ (p)
{
    latencyLabel_.setJustificationType (juce::Justification::centred);
    latencyLabel_.setFont (uiFont (9.0f));
    latencyLabel_.setColour (juce::Label::textColourId, C::text3);
    addAndMakeVisible (latencyLabel_);

    refreshExtras();
}

void EedLimiterEditor::layoutExtraContent (juce::Rectangle<int> area)
{
    latencyLabel_.setBounds (area);
}

void EedLimiterEditor::refreshExtras()
{
    const int    samples = limiter_.getLatencySamples();
    const double sr      = limiter_.getSampleRate();

    juce::String txt = "latency " + juce::String (samples) + " samples";
    if (sr > 0.0 && samples > 0)
        txt += " (" + juce::String (samples * 1000.0 / sr, 2) + " ms)";
    txt += " - reported to the host";

    if (latencyLabel_.getText() != txt)
        latencyLabel_.setText (txt, juce::dontSendNotification);
}
