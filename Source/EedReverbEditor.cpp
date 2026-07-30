/*
    EedReverbEditor.cpp  —  see EedReverbEditor.h.
*/

#include "EedReverbEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Six dial columns across, two rows, with the decay envelope seated above
    // them. The rack sizes this down if it has to, and layoutContent survives
    // that — the view is the first thing to give up its room.
    //
    // 480 rather than 440: six columns fit at this width, so the depth pass's two
    // dials join the existing nine as 6 + 5 instead of forcing a third row — and
    // the header has room for the ALGORITHM selector without crowding the title.
    constexpr int kDefaultW = 480;
    constexpr int kDecayH   = 112;
    constexpr int kDefaultH = 236 + kDecayH + 6;
    constexpr int kGap      = 8;

    // Wide enough for "AMBIENCE", the longest of the five.
    constexpr int kAlgoW = 112;

    // Below this the envelope is a sliver with no readable shape to it, so the
    // view is dropped rather than drawn uselessly small.
    constexpr int kMinDecayH = 46;
}

EedReverbEditor::EedReverbEditor (EedReverbProcessor& p)
    : DeviceEditorBase (p, "REVERB", kDefaultW, kDefaultH), proc_ (p)
{
    auto setupKnob = [this] (EchoJayDeviceKnob& k, const char* id,
                             double skewMid, int decimals, const juce::String& suffix,
                             const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial.
        const auto* spec = EedReverbProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this, id, &k] { if (! suppressCallbacks_) pushKnob (id, k); };
        addAndMakeVisible (k);
    };

    // Decay and the low cut are heard logarithmically, so their skew mid-points
    // put the musically busy part of the range under the middle of the dial:
    // most of the useful decay settings live under 4 seconds, not under 10.
    setupKnob (sizeKnob_,      EedReverbProcessor::kSize,         0.0, 0, " %",  "SIZE");
    setupKnob (decayKnob_,     EedReverbProcessor::kDecayS,       2.5, 2, " s",  "DECAY");
    setupKnob (predelayKnob_,  EedReverbProcessor::kPredelayMs,  40.0, 0, " ms", "PREDLY");
    setupKnob (mixKnob_,       EedReverbProcessor::kMix,          0.0, 0, " %",  "MIX");
    setupKnob (dampingKnob_,   EedReverbProcessor::kDamping,      0.0, 0, " %",  "DAMP");
    setupKnob (lowCutKnob_,    EedReverbProcessor::kLowCutHz,   150.0, 0, " Hz", "LOCUT");
    setupKnob (widthKnob_,     EedReverbProcessor::kWidth,        0.0, 0, " %",  "WIDTH");
    setupKnob (earlyLateKnob_, EedReverbProcessor::kEarlyLate,    0.0, 0, " %",  "EARLY/LATE");
    setupKnob (modDepthKnob_,  EedReverbProcessor::kModDepth,     0.0, 0, " %",  "MOD");
    setupKnob (diffusionKnob_, EedReverbProcessor::kDiffusion,    0.0, 0, " %",  "DIFFUSE");
    setupKnob (duckKnob_,      EedReverbProcessor::kDuck,         0.0, 0, " %",  "DUCK");

    // The selector's items ARE the schema's choices, in the schema's order, so the
    // list a user sees and the list the model is taught cannot drift apart. Item
    // ids are index + 1 because a JUCE ComboBox treats 0 as "nothing selected".
    styleCombo (algorithmBox_);
    if (const auto* spec = EedReverbProcessor::schema().find (EedReverbProcessor::kAlgorithm))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            algorithmBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

        algorithmBox_.setSelectedId (
            (int) proc_.getParamValue (EedReverbProcessor::kAlgorithm) + 1,
            juce::dontSendNotification);
    }
    algorithmBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;

        // Through setParamValue, exactly as an AI move does, so a click and a
        // dialled `algorithm` cannot end up meaning different things.
        proc_.setParamValue (EedReverbProcessor::kAlgorithm,
                             (double) (algorithmBox_.getSelectedId() - 1));
        refreshHint();
        refreshDecay();
    };
    addAndMakeVisible (algorithmBox_);

    decay_.setCaption ("DECAY");
    addAndMakeVisible (decay_);

    refreshHint();
    refreshDecay();

    // The AI can move any of these while the editor is open, so poll for changes
    // the UI did not make. 15 Hz: the envelope is analytic, so it only redraws
    // when a param actually moves — there is no signal to keep up with.
    startTimerHz (15);
}

EedReverbEditor::~EedReverbEditor()
{
    stopTimer();
}

// ---------------------------------------------------------------------------
void EedReverbEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // The envelope is reserved from the TOP out of whatever is left once both
    // dial rows are whole. It is the readout; the dials are the device, and a
    // short rack slot loses the readout first (the inline-hosting contract in
    // DeviceEditorBase.h).
    {
        const int needed = kKnobH * 2 + kGap;
        const int want   = juce::jmin (kDecayH, juce::jmax (0, content.getHeight() - needed));
        const bool room  = want >= kMinDecayH && content.getWidth() >= 80;

        decay_.setVisible (room);
        if (room)
        {
            decay_.setBounds (content.removeFromTop (want));
            content.removeFromTop (6);
        }
    }

    auto placeRow = [] (juce::Rectangle<int> row,
                        std::initializer_list<EchoJayDeviceKnob*> knobs)
    {
        const int n = (int) knobs.size();
        if (n == 0 || row.isEmpty()) return;

        const int wanted = kKnobW * n + kGap * (n - 1);
        auto strip = row.withSizeKeepingCentre (juce::jmin (wanted, row.getWidth()),
                                                row.getHeight());

        const int colW = juce::jmax (1, (strip.getWidth() - kGap * (n - 1)) / n);
        for (auto* k : knobs)
        {
            k->setBounds (strip.removeFromLeft (colW));
            strip.removeFromLeft (kGap);
        }
    };

    // Two rows, each taking half of whatever height the rack gave us. Clamped to
    // the dial's natural height so a tall slot does not stretch the knobs.
    const int rowH = juce::jmin (kKnobH, juce::jmax (1, (content.getHeight() - kGap) / 2));

    // Top row: the controls that define the space itself — including DUCK, which
    // is about how the space SITS in a mix rather than about what happens inside
    // it, and which a user reaches for immediately after the mix knob.
    placeRow (content.removeFromTop (rowH),
              { &sizeKnob_, &decayKnob_, &predelayKnob_, &mixKnob_, &duckKnob_ });

    if (content.getHeight() > kGap) content.removeFromTop (kGap);

    // Bottom row: what happens inside it.
    placeRow (content.removeFromTop (juce::jmin (rowH, content.getHeight())),
              { &dampingKnob_, &lowCutKnob_, &diffusionKnob_, &widthKnob_,
                &earlyLateKnob_, &modDepthKnob_ });
}

void EedReverbEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    algorithmBox_.setBounds (
        bar.removeFromRight (juce::jmin (kAlgoW, juce::jmax (0, bar.getWidth())))
           .reduced (0, 3));
    bar.removeFromRight (6);
}

// ---------------------------------------------------------------------------
void EedReverbEditor::pushKnob (const char* id, const EchoJayDeviceKnob& k)
{
    // Through the schema path, exactly as an AI move would.
    proc_.setParamValue (juce::String (id), k.getRealValue());
    refreshHint();
    refreshDecay();
}

void EedReverbEditor::refreshDecay()
{
    // Analytic: read through the schema, exactly as the AI writes it, so the
    // drawing cannot disagree with the device about what it is set to.
    //
    // The RAW decay_s, not effectiveDecaySeconds(): the view draws damping as
    // its own second envelope, so handing it an already-damped RT60 would apply
    // damping twice and draw a tail shorter than the one you hear.
    decay_.setDecay ((float) proc_.getParamValue (EedReverbProcessor::kDecayS),
                     (float) proc_.getParamValue (EedReverbProcessor::kPredelayMs),
                     (float) proc_.getParamValue (EedReverbProcessor::kDamping),
                     (float) proc_.getParamValue (EedReverbProcessor::kSize));

    // So a reverb at 10% wet does not draw itself as loud as one at 100%.
    decay_.setMixPercent ((float) proc_.getParamValue (EedReverbProcessor::kMix));

    decay_.setDimmed (proc_.isBypassed());
}

void EedReverbEditor::refreshHint()
{
    const auto& e = proc_.engine();

    // effectiveDecaySeconds, not the decay knob: the damping filter is inside
    // the feedback loop, so it shortens the tail it is filtering. Reporting the
    // asked-for number here would be a readout the ear disagrees with.
    // The algorithm is named first, because the same size percentage is a very
    // different room in `ambience` and in `hall` — roomSizeMetres() already scales
    // with the algorithm, and without the name the number looks like it moved on
    // its own.
    juce::String h;
    h << echojay::reverbAlgorithmName (e.getAlgorithm()) << " - "
      << juce::String (e.roomSizeMetres(), 0) << " m, "
      << juce::String (e.effectiveDecaySeconds(), 1) << " s tail";

    if (e.getDuckPct() > 0.0f)
        h << ", ducking " << juce::String (e.getDuckPct(), 0) << "%";

    // setHeaderHint repaints, so only touch it when the text actually changed —
    // otherwise the 15 Hz timer repaints the whole editor forever.
    if (h == lastHint_) return;
    lastHint_ = h;
    setHeaderHint (h);
}

void EedReverbEditor::syncFromProcessor()
{
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    // Only write when a value actually moved: setRealValue would otherwise fight
    // a drag in progress by snapping the knob to the value it just sent.
    auto pull = [this] (EchoJayDeviceKnob& k, const char* id, double tol)
    {
        const double v = proc_.getParamValue (juce::String (id));
        if (std::abs (v - k.getRealValue()) > tol) k.setRealValue (v);
    };

    pull (sizeKnob_,      EedReverbProcessor::kSize,       1.0e-3);
    pull (decayKnob_,     EedReverbProcessor::kDecayS,     1.0e-4);
    pull (predelayKnob_,  EedReverbProcessor::kPredelayMs, 1.0e-3);
    pull (mixKnob_,       EedReverbProcessor::kMix,        1.0e-3);
    pull (dampingKnob_,   EedReverbProcessor::kDamping,    1.0e-3);
    pull (lowCutKnob_,    EedReverbProcessor::kLowCutHz,   1.0e-3);
    pull (widthKnob_,     EedReverbProcessor::kWidth,      1.0e-3);
    pull (earlyLateKnob_, EedReverbProcessor::kEarlyLate,  1.0e-3);
    pull (modDepthKnob_,  EedReverbProcessor::kModDepth,   1.0e-3);
    pull (diffusionKnob_, EedReverbProcessor::kDiffusion,  1.0e-3);
    pull (duckKnob_,      EedReverbProcessor::kDuck,       1.0e-3);

    // The AI can switch the algorithm while the editor is open, so the selector
    // follows the processor rather than assuming it is the only thing writing it.
    const int algo = (int) proc_.getParamValue (EedReverbProcessor::kAlgorithm) + 1;
    if (algorithmBox_.getSelectedId() != algo)
        algorithmBox_.setSelectedId (algo, juce::dontSendNotification);
}

void EedReverbEditor::timerCallback()
{
    syncFromProcessor();
    refreshHint();
    refreshDecay();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
