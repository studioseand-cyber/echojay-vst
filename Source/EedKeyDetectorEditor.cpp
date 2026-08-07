/*
    EedKeyDetectorEditor.cpp  —  see EedKeyDetectorEditor.h.
*/

#include "EedKeyDetectorEditor.h"
#include "EqNote.h"     // describeFreqAsNote — the root readout ("F#2")

using namespace echojay::device;
using namespace echojay::device::metrics;
using echojay::viz::DwellGlow;

// Qualified alias, not `using namespace`: unqualified `Colours` is ambiguous
// against juce::Colours (same convention as DeviceEditorBase.cpp).
using C = echojay::device::Colours;

namespace
{
    // Wheel + readout above, five dials + switch column below.
    constexpr int kDefaultW = 470;
    constexpr int kDefaultH = 320;

    constexpr int kGap  = 8;
    constexpr int kBtnW = 78;

    // The wheel eases at the dwell glow's time constant so the two families
    // move with the same hand.
    constexpr float kEaseTauSec = 0.120f;

    // Same visible-floor idea as the glow: below this fraction of the peak a
    // segment draws cold instead of faintly hot.
    constexpr float kSegFloor = 0.09f;
}

EedKeyDetectorEditor::EedKeyDetectorEditor (EedKeyDetectorProcessor& p)
    : DeviceEditorBase (p, "KEY DETECTOR", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("press ANALYSE while playing - it listens, commits, holds");

    auto setupKnob = [this] (EchoJayDeviceKnob& k, const char* id,
                             double skewMid, int decimals, const juce::String& suffix,
                             const juce::String& caption)
    {
        const auto* spec = EedKeyDetectorProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this, id, &k]
        {
            if (! suppressCallbacks_)
                proc_.setParamValue (juce::String (id), k.getRealValue());
        };
        addAndMakeVisible (k);
    };

    setupKnob (windowKnob_, EedKeyDetectorProcessor::kWindowS,      8.0, 0, " s",  "WINDOW");
    setupKnob (sensKnob_,   EedKeyDetectorProcessor::kSensitivity,  0.0, 0, " %",  "SENS");
    setupKnob (tuningKnob_, EedKeyDetectorProcessor::kTuningHz,     0.0, 1, " Hz", "TUNING");
    setupKnob (lowKnob_,    EedKeyDetectorProcessor::kLowHz,      120.0, 0, " Hz", "LOW");
    setupKnob (highKnob_,   EedKeyDetectorProcessor::kHighHz,    3500.0, 0, " Hz", "HIGH");

    auto setupToggle = [this] (juce::TextButton& b, const char* id)
    {
        styleButton (b, true);
        b.setToggleState (proc_.getParamValue (juce::String (id)) >= 0.5,
                          juce::dontSendNotification);
        b.onClick = [this, &b, id]
        {
            if (! suppressCallbacks_)
                proc_.setParamValue (juce::String (id), b.getToggleState() ? 1.0 : 0.0);
        };
        addAndMakeVisible (b);
    };

    setupToggle (holdBtn_,     EedKeyDetectorProcessor::kHold);
    setupToggle (contBtn_,     EedKeyDetectorProcessor::kContinuous);
    setupToggle (autoTuneBtn_, EedKeyDetectorProcessor::kAutoTuning);
    setupToggle (hpssBtn_,     EedKeyDetectorProcessor::kHpss);

    // ANALYSE and RESET are momentary actions, not switches.
    styleButton (analyseBtn_, false);
    analyseBtn_.onClick = [this]
    { proc_.setParamValue (EedKeyDetectorProcessor::kAnalyse, 1.0); };
    addAndMakeVisible (analyseBtn_);

    styleButton (resetBtn_, false);
    resetBtn_.onClick = [this]
    { proc_.setParamValue (EedKeyDetectorProcessor::kReset, 1.0); };
    addAndMakeVisible (resetBtn_);

    // mode_lock items ARE the schema's choices, in the schema's order.
    styleCombo (modeLockBox_);
    if (const auto* spec = EedKeyDetectorProcessor::schema().find (EedKeyDetectorProcessor::kModeLock))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            modeLockBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);
        modeLockBox_.setSelectedId (
            (int) proc_.getParamValue (EedKeyDetectorProcessor::kModeLock) + 1,
            juce::dontSendNotification);
    }
    modeLockBox_.onChange = [this]
    {
        if (! suppressCallbacks_ && modeLockBox_.getSelectedId() > 0)
            proc_.setParamValue (EedKeyDetectorProcessor::kModeLock,
                                 (double) (modeLockBox_.getSelectedId() - 1));
    };
    addAndMakeVisible (modeLockBox_);

    lastTickMs_ = juce::Time::getMillisecondCounterHiRes();

    // 30 Hz: enough for the wheel's ease to read as motion, and the poll that
    // keeps the controls honest while the AI dials them.
    startTimerHz (30);
}

EedKeyDetectorEditor::~EedKeyDetectorEditor()
{
    stopTimer();
}

// ---------------------------------------------------------------------------
void EedKeyDetectorEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    const int w = juce::jmin (86, bar.getWidth());
    modeLockBox_.setBounds (bar.removeFromRight (w).reduced (0, 3));
    bar.removeFromRight (6);
}

void EedKeyDetectorEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // Dial row pinned to the bottom; wheel left and readout right share the rest.
    auto dialRow = content.removeFromBottom (juce::jmin (kKnobH, content.getHeight()));
    content.removeFromBottom (juce::jmin (kGap, content.getHeight()));

    {
        auto r = dialRow;
        const int cols = 5;
        const int colW = juce::jmax (1, juce::jmin (kKnobW, r.getWidth() / cols));
        windowKnob_.setBounds (r.removeFromLeft (colW));
        sensKnob_  .setBounds (r.removeFromLeft (colW));
        tuningKnob_.setBounds (r.removeFromLeft (colW));
        lowKnob_   .setBounds (r.removeFromLeft (colW));
        highKnob_  .setBounds (r.removeFromLeft (colW));

        // Switches live to the right of the dials, stacked two rows.
        r.removeFromLeft (juce::jmin (kGap, r.getWidth()));
        if (r.getWidth() > 40)
        {
            auto top = r.removeFromTop (r.getHeight() / 2).reduced (0, 2);
            auto bot = r.reduced (0, 2);
            const int bw = juce::jmax (1, top.getWidth() / 2 - 2);
            contBtn_    .setBounds (top.removeFromLeft (bw)); top.removeFromLeft (4);
            hpssBtn_    .setBounds (top);
            autoTuneBtn_.setBounds (bot.removeFromLeft (bw)); bot.removeFromLeft (4);
            holdBtn_    .setBounds (bot);
        }
        else
        {
            contBtn_.setBounds ({}); hpssBtn_.setBounds ({});
            autoTuneBtn_.setBounds ({}); holdBtn_.setBounds ({});
        }
    }

    // Wheel: the biggest square that fits the left half.
    const int wheelSide = juce::jmax (1, juce::jmin (content.getHeight(),
                                                     content.getWidth() / 2));
    wheelBounds_ = content.removeFromLeft (wheelSide)
                          .withSizeKeepingCentre (wheelSide, wheelSide);
    content.removeFromLeft (juce::jmin (kGap, content.getWidth()));

    // Readout right: action buttons on the bottom line, text above.
    auto right = content;
    auto btnRow = right.removeFromBottom (juce::jmin (kRowH + 4, right.getHeight()))
                       .reduced (0, 2);
    const int bw = juce::jmax (1, juce::jmin (kBtnW, btnRow.getWidth() / 2 - 2));
    analyseBtn_.setBounds (btnRow.removeFromLeft (bw)); btnRow.removeFromLeft (6);
    resetBtn_  .setBounds (btnRow.removeFromLeft (juce::jmax (1, juce::jmin (kBtnW - 16,
                                                    btnRow.getWidth()))));
    readoutBounds_ = right;
}

// ---------------------------------------------------------------------------
void EedKeyDetectorEditor::paintContent (juce::Graphics& g)
{
    if (! wheelBounds_.isEmpty())
        paintWheel (g, wheelBounds_.toFloat().reduced (4.0f));
    if (! readoutBounds_.isEmpty())
        paintReadout (g, readoutBounds_);
}

void EedKeyDetectorEditor::paintWheel (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto centre = area.getCentre();
    const float rOut  = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
    if (rOut < 12.0f) return;
    const float rIn   = rOut * 0.62f;

    const auto reading = proc_.engine().getReading();
    const bool dim     = proc_.isBypassed();

    constexpr float kTwoPi = juce::MathConstants<float>::twoPi;
    const float segAngle   = kTwoPi / 12.0f;

    for (int c = 0; c < 12; ++c)
    {
        // C at 12 o'clock, clockwise.
        const float a0 = -juce::MathConstants<float>::halfPi
                       + (float) c * segAngle - segAngle * 0.5f + 0.015f;
        const float a1 = a0 + segAngle - 0.03f;

        juce::Path seg;
        seg.addArc (centre.x - rOut, centre.y - rOut, rOut * 2, rOut * 2,
                    a0 + juce::MathConstants<float>::halfPi,
                    a1 + juce::MathConstants<float>::halfPi, true);
        seg.addArc (centre.x - rIn, centre.y - rIn, rIn * 2, rIn * 2,
                    a1 + juce::MathConstants<float>::halfPi,
                    a0 + juce::MathConstants<float>::halfPi, false);
        seg.closeSubPath();

        // The dwell glow's floor-then-shape and its colour ramp, verbatim
        // via DwellGlow::heatColour — one picture language for the family.
        float v = chromaShown_[(std::size_t) c];
        v = (v - kSegFloor) / (1.0f - kSegFloor);
        v = v > 0.0f ? v * v : 0.0f;   // the glow's contrast of 2

        const float alpha = dim ? 0.25f : 1.0f;
        if (v > 0.004f)
            g.setColour (DwellGlow::heatColour (v).withAlpha (
                             juce::jlimit (0.0f, 1.0f, (0.15f + 0.85f * v) * alpha)));
        else
            g.setColour (C::bg3.withAlpha (alpha));
        g.fillPath (seg);

        // Cold outline, drawn whatever the chroma says (the diagram survives
        // silence — DwellGlow::kColdAlpha's job here).
        g.setColour (C::blue.withAlpha (DwellGlow::kColdAlpha * 0.5f * alpha));
        g.strokePath (seg, juce::PathStrokeType (1.0f));

        // Note name just outside the ring; the root's is emphasised.
        const float mid  = (a0 + a1) * 0.5f;
        const float rTxt = rOut * 0.885f;
        const bool  isRoot = reading.valid && c == reading.root;
        g.setColour (isRoot ? juce::Colours::white.withAlpha (alpha)
                            : C::text3.withAlpha (alpha));
        g.setFont (uiFont (isRoot ? 10.0f : 9.0f, isRoot));
        g.drawText (echojay::KeyEngine::pitchClassName (c),
                    juce::Rectangle<float> (24, 12).withCentre (
                        { centre.x + rTxt * std::cos (mid),
                          centre.y + rTxt * std::sin (mid) }),
                    juce::Justification::centred);
    }

    // The detected key, large, in the middle; confidence right under it.
    {
        const juce::Rectangle<float> hub (rIn * 1.35f, rIn * 1.1f);
        auto box = hub.withCentre (centre);

        if (proc_.engine().isCollecting())
        {
            g.setColour (C::blue2);
            g.setFont (uiFont (11.0f, true));
            g.drawText ("LISTENING", box.removeFromTop (box.getHeight() * 0.55f),
                        juce::Justification::centredBottom);
            g.setColour (C::text3);
            g.setFont (uiFont (9.0f));
            g.drawText (juce::String (juce::roundToInt (
                            proc_.engine().collectionProgress() * 100.0f)) + "%",
                        box, juce::Justification::centredTop);
        }
        else if (reading.valid)
        {
            char name[24];
            echojay::KeyEngine::keyName (reading.root, reading.minor, name, sizeof (name));
            g.setColour (juce::Colours::white.withAlpha (proc_.isBypassed() ? 0.4f : 1.0f));
            g.setFont (uiFont (juce::jmin (17.0f, rIn * 0.42f), true));
            g.drawText (name, box.removeFromTop (box.getHeight() * 0.58f),
                        juce::Justification::centredBottom);
            g.setColour (DwellGlow::heatColour (reading.confidence));
            g.setFont (uiFont (10.0f, true));
            g.drawText (juce::String (reading.confidence, 2),
                        box, juce::Justification::centredTop);
        }
        else
        {
            g.setColour (C::text3);
            g.setFont (uiFont (10.0f));
            g.drawText ("no reading", box, juce::Justification::centred);
        }
    }
}

void EedKeyDetectorEditor::paintReadout (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto reading = proc_.engine().getReading();
    auto r = area;

    // Confidence meter: a thin bar coloured by the glow ramp.
    {
        auto row = r.removeFromTop (16);
        g.setColour (C::text3);
        g.setFont (uiFont (9.0f, true));
        g.drawText ("CONFIDENCE", row.removeFromLeft (70), juce::Justification::centredLeft);
        auto bar = row.reduced (0, 4);
        g.setColour (C::bg3);
        g.fillRoundedRectangle (bar.toFloat(), 2.0f);
        if (reading.valid)
        {
            auto fill = bar.toFloat();
            fill.setWidth (fill.getWidth() * juce::jlimit (0.0f, 1.0f, reading.confidence));
            g.setColour (DwellGlow::heatColour (reading.confidence));
            g.fillRoundedRectangle (fill, 2.0f);
        }
    }
    r.removeFromTop (4);

    g.setFont (uiFont (10.0f));
    auto line = [&] (const juce::String& label, const juce::String& value)
    {
        if (r.getHeight() < 13) return;
        auto row = r.removeFromTop (14);
        g.setColour (C::text3);
        g.drawText (label, row.removeFromLeft (70), juce::Justification::centredLeft);
        g.setColour (C::text);
        g.drawText (value, row, juce::Justification::centredLeft);
    };

    if (reading.valid)
    {
        char note[16];
        line ("TUNING", juce::String (reading.tuningHz, 1) + " Hz ("
                          + (reading.tuningCents >= 0 ? "+" : "")
                          + juce::String (reading.tuningCents, 1) + " c)");
        if (echojay::describeFreqAsNote (reading.rootHz, note, (int) sizeof (note)))
            line ("ROOT", juce::String (reading.rootHz, 2) + " Hz ("
                            + juce::String (note) + ")");
        line ("ANALYSED", juce::String (reading.analysedSeconds, 1) + " s"
                            + (reading.committed ? ", committed" : ", continuous"));

        if (reading.numAlternates > 0 && r.getHeight() >= 26)
        {
            r.removeFromTop (2);
            g.setColour (C::text3);
            g.setFont (uiFont (9.0f, true));
            g.drawText ("ALTERNATES", r.removeFromTop (12), juce::Justification::centredLeft);
            g.setFont (uiFont (9.0f));
            for (int i = 0; i < reading.numAlternates && r.getHeight() >= 12; ++i)
            {
                const auto& a = reading.alternates[(std::size_t) i];
                char name[24];
                echojay::KeyEngine::keyName (a.root, a.minor, name, sizeof (name));
                g.setColour (C::text2);
                g.drawText (juce::String (name) + "  (" + juce::String (a.score, 2) + ")",
                            r.removeFromTop (12), juce::Justification::centredLeft);
            }
        }
    }
    else
    {
        g.setColour (C::text3);
        line ("STATUS", proc_.engine().isCollecting() ? "listening..." : "no reading yet");
    }
}

// ---------------------------------------------------------------------------
void EedKeyDetectorEditor::syncFromProcessor()
{
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    auto syncKnob = [this] (EchoJayDeviceKnob& k, const char* id)
    {
        const double v = proc_.getParamValue (juce::String (id));
        if (std::abs (v - k.getRealValue()) > 1.0e-4) k.setRealValue (v);
    };
    syncKnob (windowKnob_, EedKeyDetectorProcessor::kWindowS);
    syncKnob (sensKnob_,   EedKeyDetectorProcessor::kSensitivity);
    syncKnob (tuningKnob_, EedKeyDetectorProcessor::kTuningHz);
    syncKnob (lowKnob_,    EedKeyDetectorProcessor::kLowHz);
    syncKnob (highKnob_,   EedKeyDetectorProcessor::kHighHz);

    auto syncToggle = [this] (juce::TextButton& b, const char* id)
    {
        const bool on = proc_.getParamValue (juce::String (id)) >= 0.5;
        if (b.getToggleState() != on) b.setToggleState (on, juce::dontSendNotification);
    };
    syncToggle (holdBtn_,     EedKeyDetectorProcessor::kHold);
    syncToggle (contBtn_,     EedKeyDetectorProcessor::kContinuous);
    syncToggle (autoTuneBtn_, EedKeyDetectorProcessor::kAutoTuning);
    syncToggle (hpssBtn_,     EedKeyDetectorProcessor::kHpss);

    const int lockIdx = (int) proc_.getParamValue (EedKeyDetectorProcessor::kModeLock);
    if (modeLockBox_.getSelectedId() != lockIdx + 1)
        modeLockBox_.setSelectedId (lockIdx + 1, juce::dontSendNotification);

    // The dial the tuning hint rides dims when auto-tuning has taken over.
    tuningKnob_.setDimmed (proc_.getParamValue (
                               EedKeyDetectorProcessor::kAutoTuning) >= 0.5);

    analyseBtn_.setButtonText (proc_.engine().isCollecting() ? "LISTENING" : "ANALYSE");
}

void EedKeyDetectorEditor::timerCallback()
{
    syncFromProcessor();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);

    // Ease the wheel toward the live chroma — measured dt, exactly like
    // DwellGlow::tick, so a stalled frame cannot snap the picture.
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float  dt    = juce::jlimit (0.001f, 0.25f,
                                       (float) ((nowMs - lastTickMs_) * 0.001));
    lastTickMs_ = nowMs;

    float target[12];
    proc_.engine().getLiveChroma (target);

    const float k = 1.0f - std::exp (-dt / kEaseTauSec);
    bool moving = false;
    for (int c = 0; c < 12; ++c)
    {
        const float d = target[c] - chromaShown_[(std::size_t) c];
        chromaShown_[(std::size_t) c] += d * k;
        moving = moving || std::abs (d) > 1.0e-3f;
    }

    if (moving || proc_.engine().isCollecting())
        repaint (wheelBounds_.getUnion (readoutBounds_));
}
