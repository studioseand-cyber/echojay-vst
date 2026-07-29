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
    constexpr int kDefaultH = 300;

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
    repaint();
}

// ---------------------------------------------------------------------------
void EedMultibandEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // Crossovers on top: they define the bands, so they read first.
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
        content.removeFromTop (8);

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
                    colW, 11, juce::Justification::centred);
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

    // The band labels follow the crossovers, which the AI can move.
    repaint (bandRowBounds_.withHeight (bandRowBounds_.getHeight() + 12));
}
