/*
    EedStereoWidthEditor.cpp  —  see EedStereoWidthEditor.h.
*/

#include "EedStereoWidthEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // A goniometer over up to two dial rows. The second row exists for the
    // multiband mode; in `full` mode the panel runs one row and the scope keeps
    // the difference, so switching modes never resizes the device. The rack
    // sizes it down from here if it has to, and layoutContent survives that —
    // the scope is the first thing to give up its room.
    constexpr int kDefaultW = 460;
    constexpr int kScopeH   = 168;
    constexpr int kRowGap   = 8;
    constexpr int kDefaultH = 150 + echojay::device::metrics::kKnobH + kRowGap
                            + kScopeH + 6;
    constexpr int kGap      = 20;

    // Tighter column gap for the five-dial multiband row.
    constexpr int kGapMB    = 10;

    // Wide enough for "MULTIBAND".
    constexpr int kModeW    = 104;

    // Below this the Lissajous is a blob rather than an image, so it is dropped
    // instead of drawn uselessly small.
    constexpr int kMinScopeH = 60;
}

EedStereoWidthEditor::EedStereoWidthEditor (EedStereoWidthProcessor& p)
    : DeviceEditorBase (p, "STEREO WIDTH", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("width + bass mono, mono-safe");

    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial, and widening
        // one without the other is impossible.
        const auto* spec = EedStereoWidthProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));

        // Drive the processor THROUGH the schema path, exactly as an AI move
        // does. Each knob pushes only its OWN param, so a knob the AI just moved
        // is not overwritten by a stale reading of another one.
        k.onValueChange = [this, &k, id]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (juce::String (id), k.getRealValue());
        };
        addAndMakeVisible (k);
    };

    // 0 Hz is OFF, not "a filter at 0 Hz" — the readout has to say so, or the
    // bottom of the dial's travel looks like a setting rather than a bypass.
    bassMonoKnob_.formatValue = [] (double v)
    {
        return v < 0.5 ? juce::String ("OFF")
                       : juce::String (juce::roundToInt (v)) + " Hz";
    };

    setup (widthKnob_,    EedStereoWidthProcessor::kWidth,        0.0,   0, " %",  "WIDTH");
    setup (bassMonoKnob_, EedStereoWidthProcessor::kBassMonoHz, 120.0,   0, " Hz", "BASS MONO");
    setup (trimKnob_,     EedStereoWidthProcessor::kOutputTrimDb, 0.0,   1, " dB", "TRIM");

    setup (rotationKnob_,  EedStereoWidthProcessor::kRotation,      0.0, 0, " deg", "ROTATE");
    setup (widthLowKnob_,  EedStereoWidthProcessor::kWidthLow,      0.0, 0, " %",   "LOW");
    setup (widthMidKnob_,  EedStereoWidthProcessor::kWidthMid,      0.0, 0, " %",   "MID");
    setup (widthHighKnob_, EedStereoWidthProcessor::kWidthHigh,     0.0, 0, " %",   "HIGH");
    setup (xoverLowKnob_,  EedStereoWidthProcessor::kXoverLowHz,  180.0, 0, " Hz",  "X.LOW");
    setup (xoverHighKnob_, EedStereoWidthProcessor::kXoverHighHz, 3500.0, 0, " Hz", "X.HIGH");

    // The MODE selector, in the header where every device in the suite puts its
    // character switch. Items ARE the schema's choices, in the schema's order,
    // so the list a user sees and the list the model is taught cannot drift.
    styleCombo (modeBox_);
    if (const auto* spec = EedStereoWidthProcessor::schema().find (EedStereoWidthProcessor::kMode))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            modeBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

        modeBox_.setSelectedId ((int) proc_.getParamValue (EedStereoWidthProcessor::kMode) + 1,
                                juce::dontSendNotification);
    }
    modeBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;

        proc_.setParamValue (EedStereoWidthProcessor::kMode,
                             (double) (modeBox_.getSelectedId() - 1));

        // The mode decides which dials are on the panel at all, so a change has
        // to relayout now rather than wait for a resize that may never come.
        refreshModeState();
        resized();
    };
    addAndMakeVisible (modeBox_);

    addAndMakeVisible (scope_);

    refreshModeState();

    // The AI can move these while the editor is open, so poll for changes the UI
    // did not make. 20 Hz rather than the 15 the dials alone needed: the scope
    // rides the same timer, and below about 20 Hz a Lissajous reads as a series
    // of stills instead of as a live image.
    startTimerHz (20);
}

EedStereoWidthEditor::~EedStereoWidthEditor()
{
    stopTimer();
}

bool EedStereoWidthEditor::multibandActive() const
{
    return proc_.engine().getWidthMode() == echojay::WidthMode::Multiband;
}

void EedStereoWidthEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    const bool mb = multibandActive();

    // Which dials are on the panel is the mode's decision, made here so a
    // resize can never disagree with it: `full` runs one row around the single
    // WIDTH dial, `multiband` swaps that dial for the band row.
    widthKnob_.setVisible (! mb);
    for (auto* k : { &widthLowKnob_, &widthMidKnob_, &widthHighKnob_,
                     &xoverLowKnob_, &xoverHighKnob_ })
        k->setVisible (mb);

    const int rowsH = mb ? kKnobH * 2 + kRowGap : kKnobH;

    // The scope is reserved from the TOP out of whatever is left once the dial
    // rows are whole — it is the readout, the dials are the device, and a rack
    // slot laid out short loses the readout first (the inline-hosting contract
    // in DeviceEditorBase.h). In `full` mode the second row's space goes to the
    // scope rather than to a hole, so the mode switch never resizes the device.
    {
        const int want = juce::jmax (0, content.getHeight() - rowsH - 6);
        const bool room = want >= kMinScopeH && content.getWidth() >= 80;

        scope_.setVisible (room);
        if (room)
        {
            auto band = content.removeFromTop (want);
            content.removeFromTop (6);

            // Centred and only as wide as it needs to be. The Lissajous itself
            // is square (a stretched vectorscope reports a width the signal does
            // not have), so letting the component span a 460-wide panel would
            // buy nothing but a very long correlation bar with a small picture
            // stranded in the middle of it.
            const int w = juce::jmin (band.getWidth(), juce::jmin (kScopeH, want) + 60);
            scope_.setBounds (band.withSizeKeepingCentre (w, band.getHeight()));
        }
    }

    // Dial columns centred as a group, so the device stays balanced at
    // whatever width the rack gives it.
    auto placeRow = [] (juce::Rectangle<int> row, int gap,
                        std::initializer_list<EchoJayDeviceKnob*> knobs)
    {
        const int n = (int) knobs.size();
        if (n == 0 || row.isEmpty()) return;

        const int wanted = kKnobW * n + gap * (n - 1);
        auto strip = row.withSizeKeepingCentre (juce::jmin (wanted, row.getWidth()),
                                                row.getHeight());

        const int colW = juce::jmax (1, (strip.getWidth() - gap * (n - 1)) / n);
        for (auto* k : knobs)
        {
            k->setBounds (strip.removeFromLeft (colW));
            strip.removeFromLeft (gap);
        }
    };

    const int rowH = juce::jmin (kKnobH, content.getHeight());

    if (mb)
    {
        // Row 1: the three band widths and their crossovers — the mode's whole
        // point. Row 2: everything that is mode-independent.
        placeRow (content.removeFromTop (rowH), kGapMB,
                  { &widthLowKnob_, &widthMidKnob_, &widthHighKnob_,
                    &xoverLowKnob_, &xoverHighKnob_ });

        if (content.getHeight() > kRowGap) content.removeFromTop (kRowGap);

        placeRow (content.removeFromTop (juce::jmin (rowH, content.getHeight())), kGap,
                  { &rotationKnob_, &bassMonoKnob_, &trimKnob_ });
    }
    else
    {
        placeRow (content.removeFromTop (rowH), kGap,
                  { &widthKnob_, &rotationKnob_, &bassMonoKnob_, &trimKnob_ });
    }
}

void EedStereoWidthEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    modeBox_.setBounds (
        bar.removeFromRight (juce::jmin (kModeW, juce::jmax (0, bar.getWidth())))
           .reduced (0, 3));
    bar.removeFromRight (6);
}

void EedStereoWidthEditor::refreshModeState()
{
    const int want = (int) proc_.getParamValue (EedStereoWidthProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != want)
        modeBox_.setSelectedId (want, juce::dontSendNotification);

    setHeaderHint (multibandActive() ? "3-band width + bass mono, mono-safe"
                                     : "width + bass mono, mono-safe");
}

void EedStereoWidthEditor::syncFromProcessor()
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

    pull (widthKnob_,     EedStereoWidthProcessor::kWidth);
    pull (bassMonoKnob_,  EedStereoWidthProcessor::kBassMonoHz);
    pull (trimKnob_,      EedStereoWidthProcessor::kOutputTrimDb);
    pull (rotationKnob_,  EedStereoWidthProcessor::kRotation);
    pull (widthLowKnob_,  EedStereoWidthProcessor::kWidthLow);
    pull (widthMidKnob_,  EedStereoWidthProcessor::kWidthMid);
    pull (widthHighKnob_, EedStereoWidthProcessor::kWidthHigh);
    pull (xoverLowKnob_,  EedStereoWidthProcessor::kXoverLowHz);
    pull (xoverHighKnob_, EedStereoWidthProcessor::kXoverHighHz);

    // The AI can switch the mode while the editor is open, and the mode decides
    // which dials are on the panel — so a move from outside relayouts too.
    const int wantMode = (int) proc_.getParamValue (EedStereoWidthProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != wantMode)
    {
        refreshModeState();
        resized();
    }
}

void EedStereoWidthEditor::refreshScope()
{
    const bool byp = proc_.isBypassed();
    scope_.setDimmed (byp);

    // A bypassed device stops writing its tap (processBlock returns early), so
    // reading it would draw a frozen picture of processing that is not
    // happening. Dimmed and left alone is the same choice the GR meter makes.
    if (byp || ! scope_.isVisible()) return;

    const int n = proc_.scopeTap().read (scopeL_.data(), scopeR_.data(), kScopeFrame);
    if (n > 0)
        scope_.setSamples (scopeL_.data(), scopeR_.data(), n);
}

void EedStereoWidthEditor::timerCallback()
{
    syncFromProcessor();
    refreshScope();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
