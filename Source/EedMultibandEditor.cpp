/*
    EedMultibandEditor.cpp  —  see EedMultibandEditor.h.
*/

#include "EedMultibandEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

using C = echojay::device::Colours;

namespace
{
    constexpr int kDefaultW = 560;
    // The band-range line paintContent draws under the selector strip. Declared
    // here because layoutContent has to reserve it — it is drawn text, not a
    // component, so nothing else will.
    constexpr int kBandLabelH = 11;

    // Four curves across a 560 px panel is ~134 px each, which is enough to read
    // a knee off. Shorter than the single-curve faces' 132 because there are
    // four of them and the editor already carries three rows below.
    constexpr int kCurveH   = 108;
    constexpr int kDefaultH = 300 + kCurveH + 8 + kBandLabelH;

    // Below either of these the four plots are smudges rather than readings, so
    // the whole strip is dropped and the controls keep the room.
    constexpr int kMinCurveH  = 48;
    constexpr int kMinCurveW  = 60;      // per curve
    constexpr int kCurveGap   = 4;


    // Every band is a compressor over the same -60..0 working range as the
    // single-band face, so they share its floor and read against each other.
    constexpr float kFloorDb = -60.0f;

    // Per-band dial presentation. The ids are filled in per selection, since
    // they carry the band number.
    struct BandKnobLook { const char* leaf; const char* caption; const char* suffix;
                          int decimals; double skewMid; };

    const BandKnobLook kBandLook[] = {
        { EedMultibandProcessor::kThresholdDb, "THRESH",  " dB", 1, 0.0 },
        { EedMultibandProcessor::kRatio,       "RATIO",   ":1",  2, 4.0 },
        { EedMultibandProcessor::kAttackMs,    "ATTACK",  " ms", 1, 15.0 },
        { EedMultibandProcessor::kReleaseMs,   "RELEASE", " ms", 0, 200.0 },
        { EedMultibandProcessor::kKneeDb,      "KNEE",    " dB", 1, 0.0 },
        { EedMultibandProcessor::kMakeupDb,    "MAKEUP",  " dB", 1, 0.0 },
    };
    static_assert ((int) std::size (kBandLook) == 6, "kBandLook and kBandKnobs must match");

    const KnobSpec kCrossoverLook[3] = {
        { EedMultibandProcessor::kCrossover1Hz, "XOVER 1", " Hz", 0, 200.0 },
        { EedMultibandProcessor::kCrossover2Hz, "XOVER 2", " Hz", 0, 900.0 },
        { EedMultibandProcessor::kCrossover3Hz, "XOVER 3", " Hz", 0, 5000.0 },
    };
}

// ---------------------------------------------------------------------------
EedMultibandEditor::EedMultibandEditor (EedMultibandProcessor& p)
    : DeviceEditorBase (p, "4-BAND COMP", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("Linkwitz-Riley split, one compressor per band");

    for (int i = 0; i < 3; ++i)
        if (bindKnob (crossoverKnobs_[i], kCrossoverLook[i],
                      EedMultibandProcessor::schema(), proc_, &suppressCallbacks_))
            addAndMakeVisible (crossoverKnobs_[i]);

    for (int b = 0; b < kNumBands; ++b)
    {
        auto& btn = bandButtons_[b];
        styleButton (btn, false);          // radio behaviour, driven by selectBand
        btn.setButtonText (juce::String (b + 1));
        btn.onClick = [this, b] { selectBand (b); };
        addAndMakeVisible (btn);

        // A meter per band, always live. Editing band 2 while watching band 4
        // work is the whole reason a multiband is easier than four EQs.
        auto* m = bandMeters_.add (new GrMeter (24.0f));
        m->setCaption ("B" + juce::String (b + 1));
        addAndMakeVisible (m);

        // And a curve per band, for the same reason: four settings that each
        // look reasonable alone can be obviously wrong together.
        auto* c = bandCurves_.add (new echojay::viz::TransferCurveView());
        c->setCaption ("B" + juce::String (b + 1));
        c->setFloorDb (kFloorDb);
        addAndMakeVisible (c);
    }

    styleButton (bandBypassBtn_, true);
    bandBypassBtn_.setButtonText ("BAND BYP");
    bandBypassBtn_.onClick = [this]
    {
        proc_.setParamValue (EedMultibandProcessor::bandParamId (
                                 selectedBand_, EedMultibandProcessor::kBypass),
                             bandBypassBtn_.getToggleState() ? 1.0 : 0.0);
    };
    addAndMakeVisible (bandBypassBtn_);

    for (auto& k : bandKnobs_) addAndMakeVisible (k);

    selectBand (0);
    startTimerHz (20);
}

EedMultibandEditor::~EedMultibandEditor()
{
    stopTimer();
}

// ---------------------------------------------------------------------------
void EedMultibandEditor::rebindBandKnobs()
{
    for (int i = 0; i < kBandKnobs; ++i)
    {
        // The juce::String owns the characters; the KnobSpec points at them. Both
        // are members, so the pointer outlives every use of it.
        bandSpecIds_[i] = EedMultibandProcessor::bandParamId (selectedBand_,
                                                              kBandLook[i].leaf);

        bandSpecs_[i] = KnobSpec { bandSpecIds_[i].toRawUTF8(),
                                   kBandLook[i].caption,
                                   kBandLook[i].suffix,
                                   kBandLook[i].decimals,
                                   kBandLook[i].skewMid };

        const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);
        bindKnob (bandKnobs_[i], bandSpecs_[i], EedMultibandProcessor::schema(),
                  proc_, &suppressCallbacks_);
    }
}

void EedMultibandEditor::selectBand (int band)
{
    selectedBand_ = juce::jlimit (0, kNumBands - 1, band);

    for (int b = 0; b < kNumBands; ++b)
        bandButtons_[b].setToggleState (b == selectedBand_, juce::dontSendNotification);

    rebindBandKnobs();

    bandBypassBtn_.setToggleState (proc_.isBandBypassed (selectedBand_),
                                   juce::dontSendNotification);

    // Move the highlight immediately rather than on the next timer tick — a
    // selector that takes 50 ms to respond reads as a dropped click.
    refreshCurves();
    repaint();
}

// ---------------------------------------------------------------------------
void EedMultibandEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // The four curves take the top strip, and are the FIRST thing given up when
    // the rack lays this editor out short — same shrink policy the face editor
    // applies to its single curve. They are the only part of this panel that is
    // purely a readout of numbers the controls below already state.
    //
    // Reserved before the crossover row so that they line up over the band
    // buttons two rows down, which is what makes "curve 3 is band 3" need no
    // label beyond the caption each already carries.
    {
        const int perCurveW = (content.getWidth() - (kNumBands - 1) * kCurveGap) / kNumBands;
        const int availH    = content.getHeight() - kKnobH - 8;
        const int h         = juce::jmin (kCurveH, juce::jmax (0, availH));

        const bool room = h >= kMinCurveH && perCurveW >= kMinCurveW;

        for (auto* c : bandCurves_) c->setVisible (room);

        if (room)
        {
            auto strip = content.removeFromTop (h);
            content.removeFromTop (8);

            for (int b = 0; b < kNumBands; ++b)
            {
                // The last one takes the remainder, so integer division cannot
                // leave a stripe of unused panel on the right.
                auto col = strip.removeFromLeft (b == kNumBands - 1 ? strip.getWidth()
                                                                    : perCurveW);
                if (auto* c = bandCurves_[b]) c->setBounds (col);
                if (b < kNumBands - 1) strip.removeFromLeft (kCurveGap);
            }
        }
    }

    // Crossovers next: they define the bands, so they read before the bands do.
    if (content.getHeight() > kKnobH)
    {
        auto row = content.removeFromTop (kKnobH);
        EchoJayDeviceKnob* xptrs[3] = { &crossoverKnobs_[0], &crossoverKnobs_[1],
                                        &crossoverKnobs_[2] };
        layoutKnobRow (row, xptrs, 3, 24);
        content.removeFromTop (8);
    }

    // The band strip: a select button and a meter per band, side by side.
    const int stripH = juce::jmin (34, juce::jmax (0, content.getHeight() - kKnobH - 8));
    if (stripH > 0)
    {
        bandRowBounds_ = content.removeFromTop (stripH);

        // 14, not 8: paintContent draws each band's frequency range in an 11 px
        // line immediately BELOW this strip, and that text is not a component,
        // so nothing reserves the space for it. At 8 it lands on top of the dial
        // captions underneath.
        content.removeFromTop (kBandLabelH + 3);

        auto strip = bandRowBounds_;
        const int colW = juce::jmax (1, strip.getWidth() / kNumBands);

        for (int b = 0; b < kNumBands; ++b)
        {
            auto col = strip.removeFromLeft (b == kNumBands - 1 ? strip.getWidth() : colW)
                            .reduced (3, 0);
            bandButtons_[b].setBounds (col.removeFromLeft (juce::jmin (28, col.getWidth())));
            col.removeFromLeft (4);
            if (auto* m = bandMeters_[b]) m->setBounds (col);
        }
    }
    else
    {
        bandRowBounds_ = {};
    }

    // The selected band's bypass sits at the right of the dial row, where it
    // reads as belonging to the band being edited rather than to the device.
    if (content.getHeight() > 0 && content.getWidth() > 90)
    {
        auto side = content.removeFromRight (78);
        bandBypassBtn_.setBounds (side.withSizeKeepingCentre (
            juce::jmin (74, side.getWidth()), juce::jmin (kRowH, side.getHeight())));
        content.removeFromRight (6);
    }

    EchoJayDeviceKnob* ptrs[kBandKnobs];
    for (int i = 0; i < kBandKnobs; ++i) ptrs[i] = &bandKnobs_[i];
    layoutKnobRow (content, ptrs, kBandKnobs);
}

void EedMultibandEditor::paintContent (juce::Graphics& g)
{
    if (bandRowBounds_.isEmpty()) return;

    // The band ranges, in Hz, under the selector — the numbers that say what
    // "band 3" actually means right now. They move when a crossover is dialled,
    // which is the feedback that makes the crossover dials legible.
    const double f1 = proc_.crossoverHz (0);
    const double f2 = proc_.crossoverHz (1);
    const double f3 = proc_.crossoverHz (2);

    auto hz = [] (double f)
    {
        return f >= 1000.0 ? juce::String (f / 1000.0, f >= 10000.0 ? 0 : 1) + "k"
                           : juce::String ((int) std::round (f));
    };

    const juce::String labels[kNumBands] = {
        "< " + hz (f1),
        hz (f1) + " - " + hz (f2),
        hz (f2) + " - " + hz (f3),
        "> " + hz (f3)
    };

    g.setFont (uiFont (8.0f));
    const int colW = juce::jmax (1, bandRowBounds_.getWidth() / kNumBands);

    for (int b = 0; b < kNumBands; ++b)
    {
        g.setColour (b == selectedBand_ ? C::blue2 : C::text3);
        g.drawText (labels[b],
                    bandRowBounds_.getX() + b * colW,
                    bandRowBounds_.getBottom(),
                    colW, kBandLabelH, juce::Justification::centred);
    }
}

// ---------------------------------------------------------------------------
void EedMultibandEditor::syncAll()
{
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    EchoJayDeviceKnob* xptrs[3] = { &crossoverKnobs_[0], &crossoverKnobs_[1],
                                    &crossoverKnobs_[2] };
    syncKnobs (xptrs, kCrossoverLook, 3, proc_);

    EchoJayDeviceKnob* bptrs[kBandKnobs];
    for (int i = 0; i < kBandKnobs; ++i) bptrs[i] = &bandKnobs_[i];
    syncKnobs (bptrs, bandSpecs_, kBandKnobs, proc_);
}

void EedMultibandEditor::refreshCurves()
{
    const bool deviceByp = proc_.isBypassed();

    for (int b = 0; b < kNumBands; ++b)
    {
        auto* c = bandCurves_[b];
        if (c == nullptr) continue;

        const bool bandOff = deviceByp || proc_.isBandBypassed (b);

        // ---- analytic: this band's own curve ------------------------------
        // Read through getParamValue with the band's own prefixed ids, which is
        // the same path the dials and an AI's comp_bands move take — so the
        // four pictures cannot be looking at different numbers than the four
        // compressors are running.
        auto bandParam = [this, b] (const char* leaf)
        {
            return (float) proc_.getParamValue (
                EedMultibandProcessor::bandParamId (b, leaf));
        };

        c->setCurve (bandParam (EedMultibandProcessor::kThresholdDb),
                     bandParam (EedMultibandProcessor::kRatio),
                     bandParam (EedMultibandProcessor::kKneeDb),
                     0.0f,                                  // no range cap on a band
                     EedMultibandProcessor::kBandMode);

        c->setMakeupDb (bandParam (EedMultibandProcessor::kMakeupDb));

        // ---- one float tap, per band ---------------------------------------
        // Each band's OWN detector: a single input level would put the same dot
        // on all four plots and say nothing about which band is being worked.
        c->setInputLevelDb (bandOff ? echojay::viz::TransferCurveView::kNoLevel
                                    : proc_.detectorLevelDb (b));
        c->setGainReductionDb (bandOff ? 0.0f : proc_.gainReductionDb (b));

        // Two independent fades, and they are not the same fade. DIMMED means
        // "this band is bypassed and is not doing what the picture shows", and
        // it takes the dot away because there is nothing to report. SELECTED is
        // only about which band you are editing — an unselected band recedes but
        // keeps its dot, because "one plot in front, four plots live" is the
        // entire reason four are drawn instead of one.
        c->setDimmed (bandOff);
        c->setSelected (b == selectedBand_);
    }
}

void EedMultibandEditor::timerCallback()
{
    syncAll();

    const bool byp = proc_.isBypassed();
    if (bypassButton().getToggleState() != byp)
        bypassButton().setToggleState (byp, juce::dontSendNotification);

    const bool bandByp = proc_.isBandBypassed (selectedBand_);
    if (bandBypassBtn_.getToggleState() != bandByp)
        bandBypassBtn_.setToggleState (bandByp, juce::dontSendNotification);

    for (int b = 0; b < kNumBands; ++b)
    {
        auto* m = bandMeters_[b];
        if (m == nullptr) continue;

        // A band that is bypassed is dimmed rather than left showing its last
        // reduction, which it is no longer applying.
        const bool off = byp || proc_.isBandBypassed (b);
        m->setDimmed (off);
        if (! off) m->setGainReductionDb (proc_.gainReductionDb (b));
    }

    refreshCurves();

    // The band labels follow the crossovers, which the AI can move.
    repaint (bandRowBounds_.withHeight (bandRowBounds_.getHeight() + 12));
}
