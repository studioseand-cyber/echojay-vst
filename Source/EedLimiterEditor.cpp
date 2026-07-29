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
    // Wider than the three dials need. A limiter's picture is read across the
    // top few dB, and at 340 px that span is too narrow to see the curve leave
    // the unity diagonal.
    constexpr int kDefaultW = 400;
    constexpr int kCurveH   = 132;
    constexpr int kDefaultH = 200 + kCurveH + 6;

    constexpr int kMinCurveH = 54;

    // A limiter works in the top of the range: its ceiling dials to -24 at the
    // very lowest. A -90 floor like the gate's would squeeze everything that
    // matters into the top quarter of the plot.
    constexpr float kFloorDb = -36.0f;

    // And the axis runs ABOVE 0 dBFS, which no other face needs. A limiter's
    // input is routinely over full scale — that is the situation it exists for —
    // so an axis stopping at 0 shows the wall only in the last fraction of a dB
    // and the picture reads as "this device does nothing". +12 puts the flat top
    // squarely on the plot.
    constexpr float kAxisTopDb = 12.0f;

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
    curve_.setCaption ("CEILING");
    curve_.setFloorDb (kFloorDb);
    curve_.setAxisTopDb (kAxisTopDb);
    addAndMakeVisible (curve_);

    latencyLabel_.setJustificationType (juce::Justification::centred);
    latencyLabel_.setFont (uiFont (9.0f));
    latencyLabel_.setColour (juce::Label::textColourId, C::text3);
    addAndMakeVisible (latencyLabel_);

    refreshExtras();
}

int EedLimiterEditor::topContentHeight() const
{
    return kCurveH;
}

void EedLimiterEditor::layoutTopContent (juce::Rectangle<int> area)
{
    const bool room = area.getHeight() >= kMinCurveH && area.getWidth() >= 80;

    curve_.setVisible (room);
    if (room) curve_.setBounds (area);
}

void EedLimiterEditor::layoutExtraContent (juce::Rectangle<int> area)
{
    latencyLabel_.setBounds (area);
}

void EedLimiterEditor::refreshExtras()
{
    const bool  byp     = limiter_.isBypassed();
    const float ceiling = (float) limiter_.getParamValue (EedLimiterProcessor::kCeilingDb);

    // ---- analytic: the brick wall ------------------------------------------
    // Ceiling IS the threshold — that is how the device is built, and saying so
    // here rather than adding a separate concept keeps the picture on the same
    // number the DSP clamps to. Ratio is ignored in Limit mode (the slope is
    // 1.0 by definition) and the knee is 0 to match the processor.
    curve_.setCurve (ceiling,
                     0.0f,                                    // ratio: unused by Limit
                     0.0f,                                    // knee:  hard, as the DSP runs it
                     0.0f,                                    // no range cap on a limiter
                     echojay::DynamicsMode::Limit);

    curve_.setCeilingLineDb (ceiling);

    // ---- the live half: how much of the signal is INTO the wall ------------
    curve_.setDimmed (byp);
    curve_.setDwellSource (&limiter_.dwellHistogram(), ! byp);
    curve_.setInputLevelDb (byp ? echojay::viz::TransferCurveView::kNoLevel
                                : limiter_.detectorLevelDb());
    curve_.setGainReductionDb (byp ? 0.0f : limiter_.gainReductionDb());

    // ---- the latency line ---------------------------------------------------
    const int    samples = limiter_.getLatencySamples();
    const double sr      = limiter_.getSampleRate();

    juce::String txt = "latency " + juce::String (samples) + " samples";
    if (sr > 0.0 && samples > 0)
        txt += " (" + juce::String (samples * 1000.0 / sr, 2) + " ms)";
    txt += " - reported to the host";

    if (latencyLabel_.getText() != txt)
        latencyLabel_.setText (txt, juce::dontSendNotification);
}
