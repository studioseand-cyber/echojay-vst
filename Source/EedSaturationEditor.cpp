/*
    EedSaturationEditor.cpp  —  see EedSaturationEditor.h.
*/

#include "EedSaturationEditor.h"
#include "EedHarmonicAnalysis.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // The size the rack opens at. layoutContent must still survive being given
    // less than this — that is the inline-hosting contract.
    constexpr int kDefaultW = 460;

    // The visualisation band: the curve and the bars, side by side.
    constexpr int kVizH = 96;

    // Below this the curve is a smear and the bars are stubs, so the whole band
    // is dropped rather than drawn uselessly small — the same policy the
    // dynamics faces use for their transfer curve.
    constexpr int kMinVizH = 44;

    constexpr int kDefaultH = kTopH + 6 + kVizH + 6 + kRowH + 6 + kKnobH + 2 * kPad;

    constexpr int kGap = 14;
}

EedSaturationEditor::EedSaturationEditor (EedSaturationProcessor& p)
    : DeviceEditorBase (p, "SATURATION", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("oversampled waveshaping");

    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial, and widening one
        // without the other becomes impossible.
        const auto* spec = EedSaturationProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this, id, &k]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (id, k.getRealValue());
        };
        addAndMakeVisible (k);
    };

    setup (driveKnob_, EedSaturationProcessor::kDriveDb,  0.0, 1, " dB", "DRIVE");
    setup (toneKnob_,  EedSaturationProcessor::kToneDb,   0.0, 1, " dB", "TONE");
    setup (mixKnob_,   EedSaturationProcessor::kMix,      0.0, 0, " %",  "MIX");
    setup (outKnob_,   EedSaturationProcessor::kOutputDb, 0.0, 1, " dB", "OUT");

    // The selector's items ARE the schema's choices, in the schema's order, so
    // the list a user sees and the list the model is taught cannot drift apart.
    styleCombo (typeBox_);
    if (const auto* spec = EedSaturationProcessor::schema().find (EedSaturationProcessor::kType))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            typeBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

        typeBox_.setSelectedId ((int) proc_.getParamValue (EedSaturationProcessor::kType) + 1,
                                juce::dontSendNotification);
    }
    typeBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (EedSaturationProcessor::kType, typeBox_.getSelectedId() - 1);
    };
    addAndMakeVisible (typeBox_);

    // ---- the signature visualisation --------------------------------------
    shaper_.setCaption ("CURVE");
    addAndMakeVisible (shaper_);

    bars_.setNumHarmonics (echojay::viz::HarmonicBars::kMaxHarmonics);   // 1..8
    bars_.setFloorDb (-60.0f);
    addAndMakeVisible (bars_);

    frame_.assign ((std::size_t) echojay::viz::SpectrumTap::size, 0.0f);

    // Seeded before the first timer tick so the curve is already right in the
    // frame the editor opens in, rather than one 66 ms later.
    refreshTransfer();

    // The AI can move these while the editor is open, so poll for changes the UI
    // did not make. 15 Hz is plenty for five numbers and costs nothing.
    startTimerHz (15);
}

void EedSaturationEditor::refreshTransfer()
{
    const auto  curve = proc_.core().getCurve();
    const float drive = proc_.core().getDriveDb();

    if (haveTransfer_ && curve == lastCurve_ && std::abs (drive - lastDriveDb_) < 0.01f)
        return;

    lastCurve_    = curve;
    lastDriveDb_  = drive;
    haveTransfer_ = true;

    // The drawn curve is the DSP's own arithmetic, not a lookalike: the same
    // shape() the audio thread calls, with the same drive gain and the same
    // compensation. A change to the shapers redraws this with no edit here.
    const float g = std::pow (10.0f, drive * 0.05f);
    const float k = echojay::harmonic::driveCompensation (curve, g);

    shaper_.setTransfer ([curve, g, k] (float x)
    {
        return echojay::harmonic::shape (curve, x * g) * k;
    });

    shaper_.setCurveName (juce::String (echojay::harmonic::curveName (curve)).toUpperCase());
}

void EedSaturationEditor::refreshBars()
{
    const double sr = proc_.getSampleRate();
    if (sr <= 0.0) return;                       // not prepared yet

    const int n = proc_.spectrumTap().read (frame_.data(), (int) frame_.size());
    if (n <= 0) return;

    const auto est = echojay::harmonic::estimateFundamental (frame_.data(), n, sr);

    // No fundamental means no reading. Draining the bars to the floor is the
    // honest answer: harmonic RATIOS measured against noise are a pattern in
    // noise, and because they would move, they would look like they meant
    // something.
    if (! est.locked)
    {
        float floorBars[echojay::viz::HarmonicBars::kMaxHarmonics];
        for (auto& v : floorBars) v = -60.0f;
        bars_.setMagnitudesDb (floorBars, (int) std::size (floorBars));
        return;
    }

    // Trimmed to a whole number of cycles so the fundamental and every harmonic
    // land exactly on a Goertzel bin centre. Untrimmed, the fundamental's own
    // leakage (about -13 dB) would swamp the harmonics being measured — and
    // would do it WORST when the device is cleanest.
    const int usable = echojay::harmonic::wholeCycleLength (n, sr, est.hz);
    if (usable <= 0) return;

    bars_.analyse (frame_.data(), usable, sr, est.hz);
}

EedSaturationEditor::~EedSaturationEditor()
{
    stopTimer();
}

void EedSaturationEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    auto r = content;

    // The controls are reserved FIRST and the picture gets what is left. That
    // ordering is the shrink policy: a rack slot laid out short loses the
    // visualisation, never the dials — the viz is purely a readout of things the
    // controls already state, so it is the only part that can go.
    const int controlsH = kRowH + 6 + kKnobH;
    const int vizH      = juce::jmin (kVizH, juce::jmax (0, r.getHeight() - controlsH));
    const bool showViz  = vizH >= kMinVizH;

    shaper_.setVisible (showViz);
    bars_.setVisible (showViz);

    if (showViz)
    {
        auto viz = r.removeFromTop (vizH);
        if (r.getHeight() > 6) r.removeFromTop (6);

        // The curve gets the larger share: it is a shape and needs the width,
        // where the bars are eight verticals and read fine narrow.
        const int barsW = juce::jlimit (70, 150, (int) ((float) viz.getWidth() * 0.42f));
        bars_.setBounds (viz.removeFromRight (barsW));
        if (viz.getWidth() > 6) viz.removeFromRight (6);
        shaper_.setBounds (viz);
    }

    auto typeRow = r.removeFromTop (juce::jmin (kRowH, r.getHeight()));
    typeBox_.setBounds (typeRow.withSizeKeepingCentre (
        juce::jmin (170, typeRow.getWidth()), typeRow.getHeight()));

    if (r.getHeight() > 6) r.removeFromTop (6);

    // Four dial columns, centred as a group, so the device stays balanced at any
    // width the rack gives it.
    const int wanted = kKnobW * 4 + kGap * 3;
    auto row = r.withSizeKeepingCentre (juce::jmin (wanted, r.getWidth()),
                                        juce::jmin (kKnobH, r.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - kGap * 3) / 4);
    EchoJayDeviceKnob* dials[] = { &driveKnob_, &toneKnob_, &mixKnob_, &outKnob_ };
    for (int i = 0; i < 4; ++i)
    {
        dials[i]->setBounds (row.removeFromLeft (colW));
        if (i < 3 && row.getWidth() > kGap) row.removeFromLeft (kGap);
    }
}

void EedSaturationEditor::syncFromProcessor()
{
    // Only write when it actually moved: setRealValue would otherwise fight a
    // drag in progress by snapping the knob to the value it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    struct { EchoJayDeviceKnob* knob; const char* id; } bound[] = {
        { &driveKnob_, EedSaturationProcessor::kDriveDb  },
        { &toneKnob_,  EedSaturationProcessor::kToneDb   },
        { &mixKnob_,   EedSaturationProcessor::kMix      },
        { &outKnob_,   EedSaturationProcessor::kOutputDb },
    };

    for (auto& b : bound)
    {
        const double v = proc_.getParamValue (b.id);
        if (std::abs (v - b.knob->getRealValue()) > 1.0e-4)
            b.knob->setRealValue (v);
    }

    const int typeId = (int) proc_.getParamValue (EedSaturationProcessor::kType) + 1;
    if (typeBox_.getSelectedId() != typeId)
        typeBox_.setSelectedId (typeId, juce::dontSendNotification);
}

void EedSaturationEditor::timerCallback()
{
    syncFromProcessor();

    const bool bypassed = proc_.isBypassed();
    if (bypassButton().getToggleState() != bypassed)
        bypassButton().setToggleState (bypassed, juce::dontSendNotification);

    // A bypassed device greys its pictures rather than leaving them showing
    // processing it is not doing.
    shaper_.setDimmed (bypassed);
    bars_.setDimmed (bypassed);

    refreshTransfer();
    shaper_.setInputLevel (proc_.core().inputLevel());

    if (bars_.isVisible()) refreshBars();
}
