/*
    EedTapeEditor.cpp  —  see EedTapeEditor.h.
*/

#include "EedTapeEditor.h"
#include "EedHarmonicAnalysis.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    constexpr int kDefaultW = 560;

    constexpr int kVizH    = 92;
    constexpr int kMinVizH = 44;

    // The transport strip. Slim on purpose: it carries one number, and giving it
    // more height would make it look like the most important thing on the panel.
    constexpr int kMotionH = 22;

    constexpr int kDefaultH = kTopH + 6 + kVizH + 4 + kMotionH + 6 + kRowH + 6
                            + kKnobH + 8 + kKnobH + 2 * kPad;

    constexpr int kGap    = 12;
    constexpr int kRowGap = 8;
    constexpr int kCols   = 5;
}

EedTapeEditor::EedTapeEditor (EedTapeProcessor& p)
    : DeviceEditorBase (p, "TAPE", kDefaultW, kDefaultH), proc_ (p)
{
    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial.
        const auto* spec = EedTapeProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this, id, &k]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (id, k.getRealValue());
            if (id == EedTapeProcessor::kSpeedIps) refreshSpeedHint();
        };
        addAndMakeVisible (k);
    };

    setup (speedKnob_,     EedTapeProcessor::kSpeedIps, 12.0, 1, " ips", "SPEED");
    setup (driveKnob_,     EedTapeProcessor::kDriveDb,   0.0, 1, " dB",  "DRIVE");
    setup (biasKnob_,      EedTapeProcessor::kBias,      0.0, 0, " %",   "BIAS");
    setup (bumpKnob_,      EedTapeProcessor::kHeadBump,  0.0, 1, " dB",  "BUMP");
    setup (hissKnob_,      EedTapeProcessor::kHiss,      0.0, 0, " %",   "HISS");
    setup (wowKnob_,       EedTapeProcessor::kWow,       0.0, 0, " %",   "WOW");
    setup (flutterKnob_,   EedTapeProcessor::kFlutter,   0.0, 0, " %",   "FLUTTER");
    setup (crosstalkKnob_, EedTapeProcessor::kCrosstalk, 0.0, 0, " %",   "XTALK");
    setup (mixKnob_,       EedTapeProcessor::kMix,       0.0, 0, " %",   "MIX");
    setup (outKnob_,       EedTapeProcessor::kOutputDb,  0.0, 1, " dB",  "OUT");

    // The machine selector: its items ARE the schema's choices, in the schema's
    // order, so the list a user sees and the list the model is taught cannot
    // drift apart.
    styleCombo (modeBox_);
    if (const auto* spec = EedTapeProcessor::schema().find (EedTapeProcessor::kMode))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            modeBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

        modeBox_.setSelectedId ((int) proc_.getParamValue (EedTapeProcessor::kMode) + 1,
                                juce::dontSendNotification);
    }
    modeBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (EedTapeProcessor::kMode, modeBox_.getSelectedId() - 1);
        refreshSpeedHint();
    };
    addAndMakeVisible (modeBox_);

    refreshSpeedHint();

    // ---- the signature visualisation --------------------------------------
    shaper_.setCaption ("TAPE CURVE");
    addAndMakeVisible (shaper_);

    bars_.setNumHarmonics (echojay::viz::HarmonicBars::kMaxHarmonics);   // 1..8
    bars_.setFloorDb (-60.0f);
    addAndMakeVisible (bars_);

    addAndMakeVisible (motion_);

    frame_.assign ((std::size_t) echojay::viz::SpectrumTap::size, 0.0f);

    refreshTransfer();

    startTimerHz (15);
}

void EedTapeEditor::refreshTransfer()
{
    const float drive   = proc_.engine().getDriveDb();
    const float bias    = proc_.engine().getBias();
    const int   machine = (int) proc_.engine().getMachine();

    if (std::abs (drive - lastDriveDb_) < 0.01f && std::abs (bias - lastBias_) < 0.01f
        && machine == lastMachine_)
        return;

    lastDriveDb_ = drive;
    lastBias_    = bias;
    lastMachine_ = machine;

    // The DSP's own arithmetic: the same shapeBiased() the audio thread calls,
    // with the same EFFECTIVE drive gain (the machine's extra dB included), the
    // same bias offset and the same compensation. The BIAS knob is the reason
    // this device's curve is worth drawing at all — it is the one control here
    // whose whole effect is a change in the curve's symmetry, and it is
    // invisible in every other readout.
    const auto& spec = echojay::tapeMachineSpec (proc_.engine().getMachine());
    const float g = std::pow (10.0f, (drive + spec.driveDb) * 0.05f);
    const float k = echojay::harmonic::driveCompensation (echojay::harmonic::Curve::Tape, g);
    const float b = echojay::TapeEngine::biasOffset (bias);

    shaper_.setTransfer ([g, k, b] (float x)
    {
        return echojay::harmonic::shapeBiased (echojay::harmonic::Curve::Tape, x * g, b) * k;
    });

    shaper_.setCurveName (std::abs (bias) < 0.5f ? juce::String ("TAPE")
                                                 : juce::String ("TAPE / BIAS"));
}

void EedTapeEditor::refreshBars()
{
    const double sr = proc_.getSampleRate();
    if (sr <= 0.0) return;

    const int n = proc_.spectrumTap().read (frame_.data(), (int) frame_.size());
    if (n <= 0) return;

    const auto est = echojay::harmonic::estimateFundamental (frame_.data(), n, sr);

    if (! est.locked)
    {
        float floorBars[echojay::viz::HarmonicBars::kMaxHarmonics];
        for (auto& v : floorBars) v = -60.0f;
        bars_.setMagnitudesDb (floorBars, (int) std::size (floorBars));
        return;
    }

    const int usable = echojay::harmonic::wholeCycleLength (n, sr, est.hz);
    if (usable <= 0) return;

    bars_.analyse (frame_.data(), usable, sr, est.hz);
}

EedTapeEditor::~EedTapeEditor()
{
    stopTimer();
}

void EedTapeEditor::refreshSpeedHint()
{
    const float ips  = proc_.engine().getSpeedIps();
    const int   mode = (int) proc_.engine().getMachine();
    if (std::abs (ips - lastHintSpeed_) < 0.05f && mode == lastHintMode_) return;
    lastHintSpeed_ = ips;
    lastHintMode_  = mode;

    // What the speed dial and the machine are really doing, in the units the
    // effect is heard in (hfLossHz already folds the machine's darkness in).
    setHeaderHint ("bump " + juce::String (proc_.engine().headBumpHz(), 0) + " Hz  /  "
                   "top " + juce::String (proc_.engine().hfLossHz() / 1000.0f, 1) + " kHz");
}

void EedTapeEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    EchoJayDeviceKnob* rows[2][kCols] = {
        { &speedKnob_, &driveKnob_,   &biasKnob_,      &bumpKnob_, &hissKnob_ },
        { &wowKnob_,   &flutterKnob_, &crosstalkKnob_, &mixKnob_,  &outKnob_  },
    };

    auto r = content;

    // Controls first, pictures second. Within the pictures, the transport strip
    // goes before the curve and the bars: it is one line tall, and it is the
    // only readout of WOW and FLUTTER, whose effect is inaudible on a lot of
    // material. The curve and the bars restate things the dials already say.
    const int controlsH = kRowH + 6 + kKnobH * 2 + kRowGap;
    int spare = juce::jmax (0, r.getHeight() - controlsH);

    const int vizH     = juce::jmin (kVizH, juce::jmax (0, spare - (kMotionH + 4)));
    const bool showViz = vizH >= kMinVizH;

    shaper_.setVisible (showViz);
    bars_.setVisible (showViz);

    if (showViz)
    {
        auto viz = r.removeFromTop (vizH);
        if (r.getHeight() > 4) r.removeFromTop (4);
        spare -= vizH + 4;

        const int barsW = juce::jlimit (70, 160, (int) ((float) viz.getWidth() * 0.40f));
        bars_.setBounds (viz.removeFromRight (barsW));
        if (viz.getWidth() > 6) viz.removeFromRight (6);
        shaper_.setBounds (viz);
    }

    const bool showMotion = spare >= kMotionH;
    motion_.setVisible (showMotion);

    if (showMotion)
    {
        motion_.setBounds (r.removeFromTop (kMotionH));
        if (r.getHeight() > 6) r.removeFromTop (6);
    }

    // The machine selector, centred above the dials.
    auto modeRow = r.removeFromTop (juce::jmin (kRowH, r.getHeight()));
    modeBox_.setBounds (modeRow.withSizeKeepingCentre (
        juce::jmin (170, modeRow.getWidth()), modeRow.getHeight()));

    if (r.getHeight() > 6) r.removeFromTop (6);

    // Both rows get the same height, so a rack that squeezes the device shrinks
    // them together rather than starving the second one.
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

void EedTapeEditor::syncFromProcessor()
{
    // Only write when it actually moved: setRealValue would otherwise fight a
    // drag in progress by snapping the knob to the value it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    struct { EchoJayDeviceKnob* knob; const char* id; } bound[] = {
        { &speedKnob_,     EedTapeProcessor::kSpeedIps  },
        { &driveKnob_,     EedTapeProcessor::kDriveDb   },
        { &biasKnob_,      EedTapeProcessor::kBias      },
        { &bumpKnob_,      EedTapeProcessor::kHeadBump  },
        { &hissKnob_,      EedTapeProcessor::kHiss      },
        { &wowKnob_,       EedTapeProcessor::kWow       },
        { &flutterKnob_,   EedTapeProcessor::kFlutter   },
        { &crosstalkKnob_, EedTapeProcessor::kCrosstalk },
        { &mixKnob_,       EedTapeProcessor::kMix       },
        { &outKnob_,       EedTapeProcessor::kOutputDb  },
    };

    for (auto& b : bound)
    {
        const double v = proc_.getParamValue (b.id);
        if (std::abs (v - b.knob->getRealValue()) > 1.0e-4)
            b.knob->setRealValue (v);
    }

    const int modeId = (int) proc_.getParamValue (EedTapeProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != modeId)
        modeBox_.setSelectedId (modeId, juce::dontSendNotification);

    refreshSpeedHint();
}

void EedTapeEditor::timerCallback()
{
    syncFromProcessor();

    const bool bypassed = proc_.isBypassed();
    if (bypassButton().getToggleState() != bypassed)
        bypassButton().setToggleState (bypassed, juce::dontSendNotification);

    shaper_.setDimmed (bypassed);
    bars_.setDimmed (bypassed);
    motion_.setDimmed (bypassed);

    refreshTransfer();
    shaper_.setInputLevel (proc_.engine().inputLevel());
    motion_.setOffset (proc_.engine().transportOffset());

    if (bars_.isVisible()) refreshBars();
}
