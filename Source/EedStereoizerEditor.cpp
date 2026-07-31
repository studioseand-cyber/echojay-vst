/*
    EedStereoizerEditor.cpp  —  see EedStereoizerEditor.h.
*/

#include "EedStereoizerEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Four dials, a header, and a goniometer seated above them. The rack sizes
    // it down from here if it has to, and layoutContent survives that — the
    // scope is the first thing to give up its room.
    constexpr int kDefaultW = 560;
    constexpr int kScopeH   = 168;
    constexpr int kDefaultH = 150 + kScopeH + 6;
    constexpr int kGap      = 20;

    // Wide enough for "DIMENSION".
    constexpr int kModeW    = 104;

    // Below this the Lissajous is a blob rather than an image, so it is dropped
    // instead of drawn uselessly small.
    constexpr int kMinScopeH = 60;
}

EedStereoizerEditor::EedStereoizerEditor (EedStereoizerProcessor& p)
    : DeviceEditorBase (p, "STEREOIZER", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("Haas widener, mono-safe");

    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here.
        const auto* spec = EedStereoizerProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));

        // Through the schema path, exactly as an AI move goes. Each knob pushes
        // only its OWN param, so a param the AI just moved is not overwritten by
        // a stale reading of a different knob.
        k.onValueChange = [this, &k, id]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (juce::String (id), k.getRealValue());
        };
        addAndMakeVisible (k);
    };

    // 0 is OFF for both of these, not a setting — say so, or the bottom of the
    // dial's travel looks like a value rather than a bypass.
    haasKnob_.formatValue = [] (double v)
    {
        return v < 0.05 ? juce::String ("OFF") : juce::String (v, 1) + " ms";
    };
    monoKnob_.formatValue = [] (double v)
    {
        return v < 0.5 ? juce::String ("OFF")
                       : juce::String (juce::roundToInt (v)) + " Hz";
    };

    setup (widthKnob_, EedStereoizerProcessor::kWidth,         0.0, 0, " %",  "WIDTH");
    setup (haasKnob_,  EedStereoizerProcessor::kHaasMs,        0.0, 1, " ms", "HAAS");
    setup (monoKnob_,  EedStereoizerProcessor::kMonoMakerHz, 120.0, 0, " Hz", "MONO");
    setup (mixKnob_,   EedStereoizerProcessor::kMix,           0.0, 0, " %",  "MIX");

    // The MODE selector, in the header where every device in the suite puts its
    // character switch. Items ARE the schema's choices, in the schema's order,
    // so the list a user sees and the list the model is taught cannot drift.
    styleCombo (modeBox_);
    if (const auto* spec = EedStereoizerProcessor::schema().find (EedStereoizerProcessor::kMode))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            modeBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

        modeBox_.setSelectedId ((int) proc_.getParamValue (EedStereoizerProcessor::kMode) + 1,
                                juce::dontSendNotification);
    }
    modeBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;

        proc_.setParamValue (EedStereoizerProcessor::kMode,
                             (double) (modeBox_.getSelectedId() - 1));

        // The mode decides whether HAAS is on the panel at all, so a change has
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

EedStereoizerEditor::~EedStereoizerEditor()
{
    stopTimer();
}

void EedStereoizerEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // The scope is reserved from the TOP out of whatever is left once the dial
    // row is whole — it is the readout, the dials are the device, and a rack
    // slot laid out short loses the readout first (the inline-hosting contract
    // in DeviceEditorBase.h).
    {
        const int want = juce::jmin (kScopeH,
                                     juce::jmax (0, content.getHeight() - kKnobH - 6));
        const bool room = want >= kMinScopeH && content.getWidth() >= 80;

        scope_.setVisible (room);
        if (room)
        {
            auto band = content.removeFromTop (want);
            content.removeFromTop (6);

            // Centred and only as wide as it needs to be. The Lissajous itself
            // is square (a stretched vectorscope reports a width the signal does
            // not have), so letting the component span a 560-wide panel would
            // buy nothing but a very long correlation bar with a small picture
            // stranded in the middle of it.
            const int w = juce::jmin (band.getWidth(), want + 60);
            scope_.setBounds (band.withSizeKeepingCentre (w, band.getHeight()));
        }
    }

    // Dial columns centred as a group, so the device stays balanced at
    // whatever width the rack gives it. HAAS drops out of the row entirely
    // outside haas mode — and out of the WIDTH calculation too, so the rest of
    // the row re-centres rather than leaving a hole where it was.
    const bool showHaas = haasKnobVisible();
    haasKnob_.setVisible (showHaas);

    const int n = showHaas ? 4 : 3;
    const int rowW = kKnobW * n + kGap * (n - 1);
    auto row = content.withSizeKeepingCentre (juce::jmin (rowW, content.getWidth()),
                                              juce::jmin (kKnobH, content.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - kGap * (n - 1)) / n);

    widthKnob_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (kGap);
    if (showHaas)
    {
        haasKnob_.setBounds (row.removeFromLeft (colW));
        row.removeFromLeft (kGap);
    }
    monoKnob_.setBounds (row.removeFromLeft (colW));
    row.removeFromLeft (kGap);
    mixKnob_.setBounds (row.removeFromLeft (colW));
}

void EedStereoizerEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    modeBox_.setBounds (
        bar.removeFromRight (juce::jmin (kModeW, juce::jmax (0, bar.getWidth())))
           .reduced (0, 3));
    bar.removeFromRight (6);
}

bool EedStereoizerEditor::haasKnobVisible() const
{
    return proc_.engine().getStereoizerMode() == echojay::StereoizerMode::Haas;
}

void EedStereoizerEditor::refreshModeState()
{
    const int want = (int) proc_.getParamValue (EedStereoizerProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != want)
        modeBox_.setSelectedId (want, juce::dontSendNotification);

    switch (proc_.engine().getStereoizerMode())
    {
        case echojay::StereoizerMode::Comb:      setHeaderHint ("comb widener, mono-safe");   break;
        case echojay::StereoizerMode::Dimension: setHeaderHint ("chorus widener, mono-safe"); break;
        case echojay::StereoizerMode::Haas:
        default:                                 setHeaderHint ("Haas widener, mono-safe");   break;
    }
}

void EedStereoizerEditor::syncFromProcessor()
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

    pull (widthKnob_, EedStereoizerProcessor::kWidth);
    pull (haasKnob_,  EedStereoizerProcessor::kHaasMs);
    pull (monoKnob_,  EedStereoizerProcessor::kMonoMakerHz);
    pull (mixKnob_,   EedStereoizerProcessor::kMix);

    // The AI can switch the mode while the editor is open, and the mode decides
    // whether HAAS is on the panel — so a move from outside relayouts too.
    const int wantMode = (int) proc_.getParamValue (EedStereoizerProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != wantMode)
    {
        refreshModeState();
        resized();
    }
}

void EedStereoizerEditor::refreshScope()
{
    const bool byp = proc_.isBypassed();
    scope_.setDimmed (byp);

    // A bypassed device stops writing its tap (processBlock returns early), so
    // reading it would draw a frozen picture of processing that is not
    // happening. Dimmed and left alone is the same choice the GR meter makes.
    if (byp || ! scope_.isVisible()) return;

    // The crossover hint, and the reason it is worth the two lines: the scope is
    // full-band, so the mono-maker's work is invisible in it. Everything under
    // the corner is already collapsed to the vertical, and without the caption
    // that reads as "the widener is not doing much down there" rather than as
    // "this is deliberate". Naming the frequency turns an absence into a
    // statement. VizView::setCaption self-guards on change, and captionHz_ keeps
    // the string from being rebuilt on a tick where nothing moved.
    {
        const int hz = juce::roundToInt (
            proc_.getParamValue (EedStereoizerProcessor::kMonoMakerHz));

        if (hz != captionHz_)
        {
            captionHz_ = hz;
            scope_.setCaption (hz > 0 ? "STEREO FIELD   MONO < " + juce::String (hz) + " Hz"
                                      : juce::String ("STEREO FIELD"));
        }
    }

    const int n = proc_.scopeTap().read (scopeL_.data(), scopeR_.data(), kScopeFrame);
    if (n > 0)
        scope_.setSamples (scopeL_.data(), scopeR_.data(), n);
}

void EedStereoizerEditor::timerCallback()
{
    syncFromProcessor();
    refreshScope();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
