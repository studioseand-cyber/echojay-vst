/*
    EedPhaseInvertEditor.cpp  —  see EedPhaseInvertEditor.h.
*/

#include "EedPhaseInvertEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

// Qualified alias, not the unqualified name: `Colours` is ambiguous against
// juce::Colours, which JuceHeader.h pulls into scope. Same convention as
// DeviceEditorBase.cpp and SurgicalEqEditor.
using C = echojay::device::Colours;

namespace
{
    constexpr int kDefaultW = 340;
    constexpr int kCorrH    = 30;
    constexpr int kDefaultH = 130 + kCorrH;
    constexpr int kBtnW     = 110;

    // Track geometry. Short and wide on purpose: this is a readout of one
    // number, and giving it a tall panel would make it look like the device.
    constexpr float kTrackH   = 5.0f;
    constexpr float kDotR     = 4.5f;
    constexpr int   kLabelW   = 34;
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

    // The strip comes off the BOTTOM, not the top as on every other device: it
    // reads as the result of the two switches above it, and the switches are
    // what the eye should land on first. A short rack slot drops it, leaving the
    // device exactly as it was before there was anything to draw.
    corrBounds_ = {};
    if (content.getHeight() >= kRowH + kCorrH && content.getWidth() >= 120)
        corrBounds_ = content.removeFromBottom (kCorrH);

    const int pairW = kBtnW * 2 + 16;
    auto row = content.withSizeKeepingCentre (juce::jmin (pairW, content.getWidth()),
                                              juce::jmin (kRowH, content.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - 16) / 2);
    leftBtn_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (16);
    rightBtn_.setBounds (row.removeFromLeft (colW));
}

void EedPhaseInvertEditor::paintContent (juce::Graphics& g)
{
    if (corrBounds_.isEmpty()) return;

    const float c   = proc_.correlation();
    const bool  live = std::abs (c) <= 1.0f && ! proc_.isBypassed();

    auto r = corrBounds_.toFloat().reduced (6.0f, 0.0f);

    // "CORR", then the track. The label is left of the track rather than above
    // it so the whole thing stays one line tall.
    auto labelBox = r.removeFromLeft ((float) kLabelW);
    g.setColour (C::text3.withAlpha (live ? 0.75f : 0.35f));
    g.setFont (uiFont (8.0f, true));
    g.drawText ("CORR", labelBox, juce::Justification::centredLeft);

    if (r.getWidth() < 24.0f) return;

    auto track = r.withSizeKeepingCentre (r.getWidth(), kTrackH);

    g.setColour (C::bg3);
    g.fillRoundedRectangle (track, kTrackH * 0.5f);

    // Ticks at -1, 0 and +1 — without them a dot on a bare track shows movement
    // but not WHERE, and -1 is the whole point of this device.
    const float midY = track.getCentreY();
    for (float t : { 0.0f, 0.5f, 1.0f })
    {
        const float x = track.getX() + t * track.getWidth();
        g.setColour (C::border2.withAlpha (t == 0.5f ? 0.9f : 0.55f));
        g.fillRect (x - 0.5f, midY - 6.0f, 1.0f, 12.0f);
    }

    if (! live)
    {
        // Mono, or bypassed: say so in words rather than parking the dot at a
        // position that would read as a measurement.
        g.setColour (C::text3.withAlpha (0.4f));
        g.setFont (uiFont (8.0f));
        g.drawText (proc_.isBypassed() ? "bypassed" : "mono",
                    track.withHeight (12.0f).withY (midY - 6.0f),
                    juce::Justification::centred);
        return;
    }

    // -1 at the left, +1 at the right.
    const float x = track.getX() + (c + 1.0f) * 0.5f * track.getWidth();

    // Red below zero: negative correlation is the fold-down cancellation, which
    // is the one reading here that is a WARNING rather than a state.
    g.setColour (c < -0.05f ? C::red : C::blue2);
    g.fillEllipse (x - kDotR, midY - kDotR, kDotR * 2.0f, kDotR * 2.0f);
}

void EedPhaseInvertEditor::refreshCorrelation()
{
    const float c = proc_.correlation();

    // Repaint only when the dot would actually land somewhere else. The strip is
    // the only thing on this device that changes on its own, so gating it here
    // is what keeps an idle Phase Invert at zero CPU.
    if (std::abs (c - shownCorr_) < 0.01f) return;

    shownCorr_ = c;
    if (! corrBounds_.isEmpty()) repaint (corrBounds_);
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
    refreshCorrelation();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
