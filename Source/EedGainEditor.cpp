/*
    EedGainEditor.cpp  —  see EedGainEditor.h.
*/

#include "EedGainEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Up to four dials, a switch strip, a header, and the I/O meters seated
    // above them all. The rack sizes it down from here if it has to, and
    // layoutContent survives that — the meters are the first thing to give up
    // their room. 420 rather than the shipped 360: the mid_side mode's two
    // extra dials have to fit without crowding.
    constexpr int kDefaultW = 420;

    // 34 is what a captioned VizView needs to draw anything: 4 inset + 11
    // caption + 14 minimum plot + 4 inset. Below it the caption is dropped and
    // the bar reads as an unlabelled smear, so the pair is dropped instead.
    constexpr int kMeterH   = 34;
    constexpr int kMeterGap = 4;
    constexpr int kMetersH  = kMeterH * 2 + kMeterGap;

    constexpr int kBtnW     = 64;
    constexpr int kBtnGap   = 8;

    // The switch strip sits under the dial row, so the default height grows by
    // one flat-control row over what shipped.
    constexpr int kDefaultH = 150 + echojay::device::metrics::kRowH + 8
                            + kMetersH + 6;

    // Wide enough for "MID/SIDE".
    constexpr int kModeW    = 96;
}

EedGainEditor::EedGainEditor (EedGainProcessor& p)
    : DeviceEditorBase (p, "GAIN", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("level + pan");

    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial, and widening one
        // without the other is impossible.
        const auto* spec = EedGainProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this] { if (! suppressCallbacks_) pushToProcessor(); };
        addAndMakeVisible (k);
    };

    setup (levelKnob_, EedGainProcessor::kLevelDb, 0.0, 1, " dB", "LEVEL");
    setup (panKnob_,   EedGainProcessor::kPan,     0.0, 2, "",    "PAN");
    setup (midKnob_,   EedGainProcessor::kMidDb,   0.0, 1, " dB", "MID");
    setup (sideKnob_,  EedGainProcessor::kSideDb,  0.0, 1, " dB", "SIDE");

    // The MODE selector, in the header where every device in the suite puts its
    // character switch. Items ARE the schema's choices, in the schema's order,
    // so the list a user sees and the list the model is taught cannot drift.
    styleCombo (modeBox_);
    if (const auto* spec = EedGainProcessor::schema().find (EedGainProcessor::kMode))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            modeBox_.addItem (juce::String (spec->choices[i]).toUpperCase()
                                  .replace ("_", "/"), (int) i + 1);

        modeBox_.setSelectedId ((int) proc_.getParamValue (EedGainProcessor::kMode) + 1,
                                juce::dontSendNotification);
    }
    modeBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;

        proc_.setParamValue (EedGainProcessor::kMode,
                             (double) (modeBox_.getSelectedId() - 1));

        // The mode decides whether MID/SIDE are on the panel at all, so a
        // change has to relayout now rather than wait for a resize.
        refreshModeState();
        resized();
    };
    addAndMakeVisible (modeBox_);

    // The switch strip: mono sum and per-channel polarity, driven through the
    // schema exactly as an AI move goes.
    auto setupToggle = [this] (juce::TextButton& b, const char* id)
    {
        styleButton (b, true);
        b.setToggleState (proc_.getParamValue (juce::String (id)) >= 0.5,
                          juce::dontSendNotification);
        b.onClick = [this, &b, id]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (juce::String (id), b.getToggleState() ? 1.0 : 0.0);
        };
        addAndMakeVisible (b);
    };

    setupToggle (monoBtn_,   EedGainProcessor::kMono);
    setupToggle (phaseLBtn_, EedGainProcessor::kPhaseLeft);
    setupToggle (phaseRBtn_, EedGainProcessor::kPhaseRight);

    refreshModeState();

    // A shared floor on both meters, or the comparison the stack exists to make
    // is being drawn on two different scales.
    for (auto* m : { &inMeter_, &outMeter_ })
    {
        m->setFloorDb ((float) echojay::GainEngine::kMinDb);
        addAndMakeVisible (*m);
    }
    inMeter_.setCaption ("IN");
    outMeter_.setCaption ("OUT");

    // 20 Hz rather than the 15 the dials alone needed: a level meter at 15 Hz
    // reads as a series of stills, and the peak hold is specified in frames.
    startTimerHz (20);
}

EedGainEditor::~EedGainEditor()
{
    stopTimer();
}

void EedGainEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // The meters are reserved from the TOP out of whatever is left once the dial
    // row is whole — they are the readout, the dials are the device, and a short
    // rack slot loses the readout first (DeviceEditorBase.h's inline-hosting
    // contract). They go as a PAIR: one meter alone is not a comparison.
    {
        const int want = juce::jmin (kMetersH,
                                     juce::jmax (0, content.getHeight() - kKnobH
                                                    - kRowH - 8 - 6));
        const bool room = want >= kMetersH && content.getWidth() >= 80;

        inMeter_.setVisible (room);
        outMeter_.setVisible (room);

        if (room)
        {
            auto band = content.removeFromTop (want);
            content.removeFromTop (6);

            inMeter_.setBounds (band.removeFromTop (kMeterH));
            band.removeFromTop (kMeterGap);
            outMeter_.setBounds (band.removeFromTop (kMeterH));
        }
    }

    // The dial row: LEVEL + PAN always, MID + SIDE only in mid_side mode — and
    // out of the WIDTH calculation too, so the pair re-centres in stereo mode
    // rather than sitting beside a hole.
    const bool ms = midSideActive();
    midKnob_.setVisible (ms);
    sideKnob_.setVisible (ms);

    const int n = ms ? 4 : 2;
    const int rowsW = kKnobW * n + 24 * (n - 1);
    auto row = content.removeFromTop (juce::jmin (kKnobH, content.getHeight()))
                      .withSizeKeepingCentre (juce::jmin (rowsW, content.getWidth()),
                                              juce::jmin (kKnobH, content.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - 24 * (n - 1)) / n);
    levelKnob_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (24);
    panKnob_.setBounds (row.removeFromLeft (colW));
    if (ms)
    {
        row.removeFromLeft (24);
        midKnob_.setBounds (row.removeFromLeft (colW));
        row.removeFromLeft (24);
        sideKnob_.setBounds (row.removeFromLeft (colW));
    }

    if (content.getHeight() > 8) content.removeFromTop (8);
    if (content.isEmpty()) return;

    // The switch strip, centred as a group under the dials.
    auto strip = content.removeFromTop (juce::jmin (kRowH, content.getHeight()));
    const int wanted = kBtnW * 3 + kBtnGap * 2;
    strip = strip.withSizeKeepingCentre (juce::jmin (wanted, strip.getWidth()),
                                         strip.getHeight());

    const int btnW = juce::jmax (1, (strip.getWidth() - kBtnGap * 2) / 3);
    monoBtn_.setBounds (strip.removeFromLeft (btnW));
    strip.removeFromLeft (kBtnGap);
    phaseLBtn_.setBounds (strip.removeFromLeft (btnW));
    strip.removeFromLeft (kBtnGap);
    phaseRBtn_.setBounds (strip.removeFromLeft (btnW));
}

void EedGainEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    modeBox_.setBounds (
        bar.removeFromRight (juce::jmin (kModeW, juce::jmax (0, bar.getWidth())))
           .reduced (0, 3));
    bar.removeFromRight (6);
}

bool EedGainEditor::midSideActive() const
{
    return proc_.engine().getMode() == echojay::GainMode::MidSide;
}

void EedGainEditor::refreshModeState()
{
    const int want = (int) proc_.getParamValue (EedGainProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != want)
        modeBox_.setSelectedId (want, juce::dontSendNotification);

    setHeaderHint (midSideActive() ? "mid/side gain + level + pan"
                                   : "level + pan");
}

void EedGainEditor::pushToProcessor()
{
    // Through the schema path, exactly as an AI move would.
    proc_.setParamValue (EedGainProcessor::kLevelDb, levelKnob_.getRealValue());
    proc_.setParamValue (EedGainProcessor::kPan,     panKnob_.getRealValue());
    proc_.setParamValue (EedGainProcessor::kMidDb,   midKnob_.getRealValue());
    proc_.setParamValue (EedGainProcessor::kSideDb,  sideKnob_.getRealValue());
}

void EedGainEditor::syncFromProcessor()
{
    // Only write when it actually moved: setRealValue would otherwise fight a
    // drag in progress by snapping the knob to the value it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    auto pull = [this] (EchoJayDeviceKnob& k, const char* id)
    {
        const double v = proc_.getParamValue (juce::String (id));
        if (std::abs (v - k.getRealValue()) > 1.0e-4)
            k.setRealValue (v);
    };

    pull (levelKnob_, EedGainProcessor::kLevelDb);
    pull (panKnob_,   EedGainProcessor::kPan);
    pull (midKnob_,   EedGainProcessor::kMidDb);
    pull (sideKnob_,  EedGainProcessor::kSideDb);

    auto pullToggle = [this] (juce::TextButton& b, const char* id)
    {
        const bool on = proc_.getParamValue (juce::String (id)) >= 0.5;
        if (b.getToggleState() != on)
            b.setToggleState (on, juce::dontSendNotification);
    };

    pullToggle (monoBtn_,   EedGainProcessor::kMono);
    pullToggle (phaseLBtn_, EedGainProcessor::kPhaseLeft);
    pullToggle (phaseRBtn_, EedGainProcessor::kPhaseRight);

    // The AI can switch the mode while the editor is open, and the mode decides
    // whether MID/SIDE are on the panel — so a move from outside relayouts too.
    const int wantMode = (int) proc_.getParamValue (EedGainProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != wantMode)
    {
        refreshModeState();
        resized();
    }
}

void EedGainEditor::refreshMeters()
{
    const bool byp = proc_.isBypassed();
    inMeter_.setDimmed (byp);
    outMeter_.setDimmed (byp);

    // A bypassed device stops writing its taps (processBlock returns early), so
    // reading them would animate a picture of processing that is not happening.
    // Dimmed and left alone, the same choice the goniometer and GR meter make.
    if (byp || ! inMeter_.isVisible()) return;

    inMeter_.setLevelLinear  (proc_.inputPeak(),  proc_.inputRms());
    outMeter_.setLevelLinear (proc_.outputPeak(), proc_.outputRms());
}

void EedGainEditor::timerCallback()
{
    syncFromProcessor();
    refreshMeters();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
