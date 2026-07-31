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

    constexpr int kDefaultH = kTopH + 6 + kVizH + 6 + kRowH + 6
                            + kKnobH + 8 + kKnobH + 2 * kPad;

    constexpr int kGap    = 14;
    constexpr int kRowGap = 8;
    constexpr int kCols   = 3;
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

    // HPF reads in Hz with no decimals, and its zero means "off".
    hpfKnob_.formatValue = [] (double hz)
    {
        return hz <= 0.5 ? juce::String ("OFF") : juce::String (hz, 0) + " Hz";
    };

    setup (driveKnob_, EedSaturationProcessor::kDriveDb,  0.0,   1, " dB", "DRIVE");
    setup (biasKnob_,  EedSaturationProcessor::kBias,     0.0,   0, " %",  "BIAS");
    setup (hpfKnob_,   EedSaturationProcessor::kHpfHz,  120.0,   0, "",    "HPF");
    setup (toneKnob_,  EedSaturationProcessor::kToneDb,   0.0,   1, " dB", "TONE");
    setup (mixKnob_,   EedSaturationProcessor::kMix,      0.0,   0, " %",  "MIX");
    setup (outKnob_,   EedSaturationProcessor::kOutputDb, 0.0,   1, " dB", "OUT");

    // Each selector's items ARE the schema's choices, in the schema's order, so
    // the list a user sees and the list the model is taught cannot drift apart.
    auto setupCombo = [this] (juce::ComboBox& box, const char* id)
    {
        styleCombo (box);
        if (const auto* spec = EedSaturationProcessor::schema().find (id))
        {
            for (std::size_t i = 0; i < spec->choices.size(); ++i)
                box.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

            box.setSelectedId ((int) proc_.getParamValue (id) + 1,
                               juce::dontSendNotification);
        }
        box.onChange = [this, id, &box]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (id, box.getSelectedId() - 1);
        };
        addAndMakeVisible (box);
    };

    setupCombo (typeBox_,     EedSaturationProcessor::kType);
    setupCombo (emphasisBox_, EedSaturationProcessor::kEmphasis);
    setupCombo (osBox_,       EedSaturationProcessor::kOversample);

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
    const auto  emph  = proc_.core().getEmphasis();
    const float drive = proc_.core().getDriveDb();
    const float bias  = proc_.core().getBias();

    if (haveTransfer_ && curve == lastCurve_ && emph == lastEmphasis_
        && std::abs (drive - lastDriveDb_) < 0.01f
        && std::abs (bias - lastBias_) < 0.01f)
        return;

    lastCurve_    = curve;
    lastEmphasis_ = emph;
    lastDriveDb_  = drive;
    lastBias_     = bias;
    haveTransfer_ = true;

    // The drawn curve is the DSP's own arithmetic, not a lookalike: the same
    // shapeEmphasisBiased() the audio thread calls, with the same drive gain,
    // the same bias offset and the same compensation. A change to the shapers
    // redraws this with no edit here.
    const float g = std::pow (10.0f, drive * 0.05f);
    const float k = echojay::harmonic::driveCompensation (curve, emph, g);
    const float b = echojay::harmonic::HarmonicCore::biasOffset (bias);

    shaper_.setTransfer ([curve, emph, g, k, b] (float x)
    {
        return echojay::harmonic::shapeEmphasisBiased (curve, emph, x * g, b) * k;
    });

    juce::String name = juce::String (echojay::harmonic::curveName (curve)).toUpperCase();
    if (emph != echojay::harmonic::Emphasis::Both)
        name += " / " + juce::String (echojay::harmonic::emphasisName (emph)).toUpperCase();
    shaper_.setCurveName (name);
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
    const int controlsH = kRowH + 6 + kKnobH * 2 + kRowGap;
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

    // The selector row: type, emphasis, oversampling, side by side. Type gets
    // the widest box (its labels are longest); OS the narrowest.
    auto selRow = r.removeFromTop (juce::jmin (kRowH, r.getHeight()));
    {
        const int wanted = juce::jmin (selRow.getWidth(), 150 + 6 + 120 + 6 + 76);
        auto row = selRow.withSizeKeepingCentre (wanted, selRow.getHeight());

        const float unit = (float) (row.getWidth() - 12) / (150.0f + 120.0f + 76.0f);
        typeBox_.setBounds (row.removeFromLeft ((int) (150.0f * unit)));
        if (row.getWidth() > 6) row.removeFromLeft (6);
        emphasisBox_.setBounds (row.removeFromLeft ((int) (120.0f * unit)));
        if (row.getWidth() > 6) row.removeFromLeft (6);
        osBox_.setBounds (row);
    }

    if (r.getHeight() > 6) r.removeFromTop (6);

    // Two rows of three dials, centred as a group, so the device stays balanced
    // at any width the rack gives it. Both rows share a height, so a squeezed
    // slot shrinks them together rather than starving the second one.
    EchoJayDeviceKnob* rows[2][kCols] = {
        { &driveKnob_, &biasKnob_, &hpfKnob_ },
        { &toneKnob_,  &mixKnob_,  &outKnob_ },
    };

    const int rowH = juce::jmax (1, juce::jmin (kKnobH, (r.getHeight() - kRowGap) / 2));

    for (int rowIndex = 0; rowIndex < 2; ++rowIndex)
    {
        if (r.getHeight() <= 0) return;

        auto band = r.removeFromTop (juce::jmin (rowH, r.getHeight()));
        if (rowIndex == 0 && r.getHeight() > kRowGap) r.removeFromTop (kRowGap);

        const int wanted = kKnobW * kCols + kGap * (kCols - 1);
        auto row = band.withSizeKeepingCentre (juce::jmin (wanted, band.getWidth()),
                                               band.getHeight());

        const int colW = juce::jmax (1, (row.getWidth() - kGap * (kCols - 1)) / kCols);
        for (int c = 0; c < kCols; ++c)
        {
            rows[rowIndex][c]->setBounds (row.removeFromLeft (colW));
            if (c < kCols - 1 && row.getWidth() > kGap) row.removeFromLeft (kGap);
        }
    }
}

void EedSaturationEditor::syncFromProcessor()
{
    // Only write when it actually moved: setRealValue would otherwise fight a
    // drag in progress by snapping the knob to the value it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    struct { EchoJayDeviceKnob* knob; const char* id; } bound[] = {
        { &driveKnob_, EedSaturationProcessor::kDriveDb  },
        { &biasKnob_,  EedSaturationProcessor::kBias     },
        { &hpfKnob_,   EedSaturationProcessor::kHpfHz    },
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

    struct { juce::ComboBox* box; const char* id; } combos[] = {
        { &typeBox_,     EedSaturationProcessor::kType       },
        { &emphasisBox_, EedSaturationProcessor::kEmphasis   },
        { &osBox_,       EedSaturationProcessor::kOversample },
    };

    for (auto& c : combos)
    {
        const int wanted = (int) proc_.getParamValue (c.id) + 1;
        if (c.box->getSelectedId() != wanted)
            c.box->setSelectedId (wanted, juce::dontSendNotification);
    }
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
