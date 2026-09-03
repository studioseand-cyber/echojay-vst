/*
    EedPitchEditor.cpp  —  see EedPitchEditor.h.
*/

#include "EedPitchEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;
using C = echojay::device::Colours;

namespace
{
    // The size the rack opens at. layoutContent must still survive being given
    // less than this — that is the inline-hosting contract.
    constexpr int kDefaultW = 620;
    constexpr int kDefaultH = 400;
}

EedPitchEditor::EedPitchEditor (EedPitchProcessor& p)
    : DeviceEditorBase (p, "PITCH", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("set CORRECT, pick KEY and SCALE, dial RETUNE for character");

    // voice_type items ARE the schema's choices, in the schema's order.
    styleCombo (voiceBox_);
    if (const auto* spec = EedPitchProcessor::schema().find (EedPitchProcessor::kVoiceType))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            voiceBox_.addItem (juce::String (spec->choices[i]).toUpperCase()
                                   .replaceCharacter ('_', '/'),
                               (int) i + 1);
        voiceBox_.setSelectedId (
            (int) proc_.getParamValue (EedPitchProcessor::kVoiceType) + 1,
            juce::dontSendNotification);
    }
    voiceBox_.onChange = [this]
    {
        if (! suppressCallbacks_ && voiceBox_.getSelectedId() > 0)
            proc_.setParamValue (EedPitchProcessor::kVoiceType,
                                 (double) (voiceBox_.getSelectedId() - 1));
    };
    addAndMakeVisible (voiceBox_);

    styleCombo (trackBox_);
    if (const auto* spec = EedPitchProcessor::schema().find (EedPitchProcessor::kTracking))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            trackBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);
        trackBox_.setSelectedId (
            (int) proc_.getParamValue (EedPitchProcessor::kTracking) + 1,
            juce::dontSendNotification);
    }
    trackBox_.onChange = [this]
    {
        if (! suppressCallbacks_ && trackBox_.getSelectedId() > 0)
            proc_.setParamValue (EedPitchProcessor::kTracking,
                                 (double) (trackBox_.getSelectedId() - 1));
    };
    addAndMakeVisible (trackBox_);

    styleCombo (formantBox_);
    if (const auto* spec = EedPitchProcessor::schema().find (EedPitchProcessor::kFormantMode))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
        {
            // `off` is a transpose-time control - measured identical to
            // preserve at correction-sized shifts (the gate asserts it) -
            // so its item says when it matters instead of posing as an
            // equal choice.
            juce::String label = "FORM " + juce::String (spec->choices[i]).toUpperCase();
            if (juce::String (spec->choices[i]) == "off") label << " (TRANSPOSE)";
            formantBox_.addItem (label, (int) i + 1);
        }
        formantBox_.setSelectedId (
            (int) proc_.getParamValue (EedPitchProcessor::kFormantMode) + 1,
            juce::dontSendNotification);
    }
    formantBox_.onChange = [this]
    {
        if (! suppressCallbacks_ && formantBox_.getSelectedId() > 0)
            proc_.setParamValue (EedPitchProcessor::kFormantMode,
                                 (double) (formantBox_.getSelectedId() - 1));
    };
    addAndMakeVisible (formantBox_);

    // The P1 target. Range comes from the SCHEMA, never re-typed, so the dial
    // physically cannot travel somewhere the AI is not allowed to dial.
    if (const auto* spec = EedPitchProcessor::schema().find (EedPitchProcessor::kTargetHz))
    {
        targetKnob_.setSpec (spec->min, spec->max, 400.0, 1, " Hz", "TARGET", spec->def);
        targetKnob_.setRealValue (proc_.getParamValue (EedPitchProcessor::kTargetHz));
        targetKnob_.onValueChange = [this]
        {
            if (! suppressCallbacks_)
                proc_.setParamValue (EedPitchProcessor::kTargetHz,
                                     targetKnob_.getRealValue());
        };
        addAndMakeVisible (targetKnob_);
    }

    // Momentary action, driven THROUGH the schema path like an AI move.
    styleButton (resetBtn_, false);
    resetBtn_.onClick = [this]
    { proc_.setParamValue (EedPitchProcessor::kResetStats, 1.0); };
    addAndMakeVisible (resetBtn_);

    // ---- P2 controls ------------------------------------------------------
    auto setupKnob = [this] (echojay::device::EchoJayDeviceKnob& k, const char* id,
                             double skew, int dp, const char* suffix, const char* cap)
    {
        if (const auto* spec = EedPitchProcessor::schema().find (id))
        {
            k.setSpec (spec->min, spec->max, skew, dp, suffix, cap, spec->def);
            k.setRealValue (proc_.getParamValue (id));
            k.onValueChange = [this, &k, id]
            {
                if (! suppressCallbacks_) proc_.setParamValue (id, k.getRealValue());
            };
            addAndMakeVisible (k);
        }
    };
    setupKnob (retuneKnob_, EedPitchProcessor::kRetuneMs, 80.0, 0, " ms", "RETUNE");
    // The retune FLOOR readout (30 Aug 2026 ruling): the dial keeps 0 so
    // muscle memory from other correctors finds it, but the readout states
    // what it actually is - "0 (6 ms)". Twice now this project was rescued
    // by showing a hidden derived value (reference provenance, placement
    // caption); same principle, one string.
    retuneKnob_.formatValue = [this] (double v)
    {
        // Load-clamp memory first (the cap): "150 (was 400)".
        const float was = proc_.retuneWasMs();
        if (was > 0.0f)
            return juce::String ((int) std::lround (v)) + " (was " +
                   juce::String ((int) std::lround (was)) + ")";
        const double eff = (double) proc_.retuneEffectiveMs();
        if (v + 0.01 < eff)
            return juce::String ((int) std::lround (v)) + " (" +
                   juce::String (eff, 0) + " ms)";
        return juce::String ((int) std::lround (v)) + " ms";
    };
    setupKnob (refKnob_,    EedPitchProcessor::kReferenceHz, 0.0, 1, " Hz", "REF");
    setupKnob (flexKnob_,   EedPitchProcessor::kFlex,      0.0, 0, " %",  "FLEX");
    setupKnob (humanKnob_,  EedPitchProcessor::kHumanize,  0.0, 0, " %",  "HUMAN");

    auto setupCombo = [this] (juce::ComboBox& b, const char* id, const char* prefix)
    {
        styleCombo (b);
        if (const auto* spec = EedPitchProcessor::schema().find (id))
        {
            for (std::size_t i = 0; i < spec->choices.size(); ++i)
                b.addItem (juce::String (prefix) + juce::String (spec->choices[i]).toUpperCase()
                               .replaceCharacter ('_', ' '),
                           (int) i + 1);
            b.setSelectedId ((int) proc_.getParamValue (id) + 1, juce::dontSendNotification);
        }
        b.onChange = [this, &b, id]
        {
            if (! suppressCallbacks_ && b.getSelectedId() > 0)
                proc_.setParamValue (id, (double) (b.getSelectedId() - 1));
        };
        addAndMakeVisible (b);
    };
    // correction_mode is the headline control, so it leads the row.
    setupCombo (modeBox_,  EedPitchProcessor::kMode,    "");
    setupCombo (keyBox_,   EedPitchProcessor::kKeyRoot, "KEY ");
    setupCombo (scaleBox_, EedPitchProcessor::kScale,   "");

    auto setupToggle = [this] (juce::TextButton& b, const char* id)
    {
        styleButton (b, true);
        b.setToggleState (proc_.getParamValue (id) >= 0.5, juce::dontSendNotification);
        b.onClick = [this, &b, id]
        {
            if (! suppressCallbacks_)
                proc_.setParamValue (id, b.getToggleState() ? 1.0 : 0.0);
        };
        addAndMakeVisible (b);
    };
    setupToggle (correctBtn_, EedPitchProcessor::kCorrect);
    setupToggle (vibBtn_,     EedPitchProcessor::kIgnoreVib);

    // THE WAY BACK TO AUTO. Touching key or scale takes manual control (by
    // design - an edit the next block silently overwrites would look
    // broken), but until this button that was a TRAPDOOR: one click on the
    // key combo and the detected-key feature - the reason this device lives
    // inside EchoJay at all - was gone for the life of the instance. The
    // toggle reads key_source; lit means auto, and returning to auto
    // re-applies the detected key and scale on the next block, which the
    // combos then show (dimmed) within a timer tick.
    styleButton (keyAutoBtn_, true);
    keyAutoBtn_.setToggleState (
        proc_.getParamValue (EedPitchProcessor::kKeySource) < 0.5,
        juce::dontSendNotification);
    keyAutoBtn_.onClick = [this]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (EedPitchProcessor::kKeySource,
                             keyAutoBtn_.getToggleState() ? 0.0 : 1.0);
    };
    addAndMakeVisible (keyAutoBtn_);

    // The reference's way back to auto - same shape as the key's. Turning
    // the REF knob takes manual control (the param flips the mode); this
    // returns it, and the attribution line says what auto then follows.
    styleButton (refAutoBtn_, true);
    refAutoBtn_.setToggleState (
        proc_.getParamValue (EedPitchProcessor::kRefSource) < 0.5,
        juce::dontSendNotification);
    refAutoBtn_.onClick = [this]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (EedPitchProcessor::kRefSource,
                             refAutoBtn_.getToggleState() ? 0.0 : 1.0);
    };
    addAndMakeVisible (refAutoBtn_);

    // THE LATENCY MODE. Deliberately a mode button rather than a checkbox: the
    // user is choosing between two WORKFLOWS, and the number it costs is the
    // whole point of the choice, so it is printed on the control.
    //
    // It is also deliberately MANUAL. Auto-switching from transport or
    // record-arm state would change the reported latency at the moment record
    // engages, forcing the host to rebuild delay compensation exactly when
    // that is most audible and least expected.
    styleButton (latencyBtn_, true);
    latencyBtn_.setToggleState (proc_.getParamValue (EedPitchProcessor::kLowLatency) >= 0.5,
                                juce::dontSendNotification);
    latencyBtn_.onClick = [this]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (EedPitchProcessor::kLowLatency,
                             latencyBtn_.getToggleState() ? 1.0 : 0.0);
        refreshLatencyButton();
        repaint();
    };
    addAndMakeVisible (latencyBtn_);
    refreshLatencyButton();

    // 30 Hz: the readout is live data, and the poll keeps the combo honest
    // while the AI dials it.
    startTimerHz (30);
}

EedPitchEditor::~EedPitchEditor()
{
    stopTimer();
}

// ---------------------------------------------------------------------------
void EedPitchEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    const int w = juce::jmin (110, bar.getWidth());
    voiceBox_.setBounds (bar.removeFromRight (w).reduced (0, 3));
    bar.removeFromRight (6);
    const int tw = juce::jmin (84, bar.getWidth());
    trackBox_.setBounds (bar.removeFromRight (tw).reduced (0, 3));
    bar.removeFromRight (6);
    const int fw = juce::jmin (108, bar.getWidth());
    formantBox_.setBounds (bar.removeFromRight (fw).reduced (0, 3));
    bar.removeFromRight (6);
}

int EedPitchEditor::currentLatencyMs (bool lowLatency) const
{
    const auto& vr = echojay::PitchEngine::voiceRange (
        (int) proc_.getParamValue (EedPitchProcessor::kVoiceType));
    const double fs = proc_.getSampleRate() > 0.0 ? proc_.getSampleRate() : 48000.0;
    const int smp = echojay::PsolaEngine::latencyFor (fs, vr.fMinHz,
        lowLatency ? EedPitchProcessor::kLookaheadTracking
                   : EedPitchProcessor::kLookaheadMixing);
    return (int) std::lround (1000.0 * smp / fs);
}

void EedPitchEditor::refreshLatencyButton()
{
    const bool low = latencyBtn_.getToggleState();
    latencyBtn_.setButtonText ((low ? "TRACKING  " : "MIXING  ")
                                 + juce::String (currentLatencyMs (low)) + " ms");
}

void EedPitchEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    content.reduce (juce::jmin (kPad, content.getWidth() / 4),
                    juce::jmin (6, content.getHeight() / 4));

    // THE RIBBON gets the top half of the content: it is the signature view,
    // and the numbers are supporting detail rather than the point.
    ribbonBounds_ = content.removeFromTop (juce::jmax (0, content.getHeight() / 2));
    content.removeFromTop (juce::jmin (4, content.getHeight()));

    // The key ATTRIBUTION line, directly under the readout: where the key
    // came from is what makes a wrong key diagnosable.
    keyAttrBounds_ = content.removeFromBottom (juce::jmin (16, content.getHeight() / 5));

    // The latency MODE gets its own band, full width, because it is a
    // workflow decision rather than a tweak.
    {
        auto band = content.removeFromBottom (juce::jmin (34, content.getHeight() / 3));
        latencyBounds_ = band;
        latencyBtn_.setBounds (band.removeFromLeft (juce::jmin (150, band.getWidth()))
                                   .reduced (2, 5));
    }

    // P2 controls: dials, then key/scale, then the two switches.
    {
        auto row = content.removeFromBottom (juce::jmin (kKnobH, content.getHeight() / 2));
        auto dial = [&row] (echojay::device::EchoJayDeviceKnob& k)
        {
            k.setBounds (row.removeFromLeft (juce::jmin (kKnobW, row.getWidth())));
        };
        dial (retuneKnob_); dial (flexKnob_); dial (humanKnob_);
        row.removeFromLeft (juce::jmin (6, row.getWidth()));

        auto col = row.removeFromLeft (juce::jmin (116, row.getWidth()));
        // THREE rows share the column EVENLY. Stacking them at kRowH each
        // needed 72 px in a 58 px band, so the third row - the SCALE combo -
        // was silently squeezed to a ~10 px sliver: present in the code,
        // synced, dialable by the model, and invisible to the user, who
        // reasonably reported "the panel shows KEY only". A control the
        // model can set and the user cannot see is invisible state.
        const int comboH = juce::jmax (1, col.getHeight() / 3);
        modeBox_.setBounds (col.removeFromTop (comboH).reduced (1));
        keyBox_.setBounds (col.removeFromTop (comboH).reduced (1));
        scaleBox_.setBounds (col.removeFromTop (comboH).reduced (1));

        // The AUTO/MANUAL toggle sits beside the two controls it governs,
        // spanning the key and scale rows.
        auto kcol = row.removeFromLeft (juce::jmin (46, row.getWidth()));
        kcol.removeFromTop (comboH);
        keyAutoBtn_.setBounds (kcol.reduced (1));

        row.removeFromLeft (juce::jmin (6, row.getWidth()));
        auto sw = row.removeFromLeft (juce::jmin (80, row.getWidth()));
        correctBtn_.setBounds (sw.removeFromTop (juce::jmin (kRowH, sw.getHeight())).reduced (1));
        vibBtn_.setBounds (sw.removeFromTop (juce::jmin (kRowH, sw.getHeight())).reduced (1));
    }

    // The target dial sits hard right; the REF control stacks beneath it
    // (knob + its AUTO), and the readout columns share the rest.
    {
        auto rcol = content.removeFromRight (juce::jmin (kKnobW, content.getWidth()));
        targetKnob_.setBounds (rcol.removeFromTop (juce::jmin (kKnobH, rcol.getHeight())));
        refKnob_.setBounds (rcol.removeFromTop (juce::jmin (kKnobH, rcol.getHeight())));
        refAutoBtn_.setBounds (rcol.removeFromTop (juce::jmin (kRowH, rcol.getHeight()))
                                   .reduced (1));
    }
    content.removeFromRight (juce::jmin (6, content.getWidth()));

    // Three columns: note + tuner bar | numbers | guard log.
    notePanel_    = content.removeFromLeft (content.getWidth() * 2 / 5);
    guardPanel_   = content.removeFromRight (juce::jmax (0, content.getWidth() / 2));
    numbersPanel_ = content;

    // RESET pinned to the guard panel's bottom.
    auto btnArea = guardPanel_;
    resetBtn_.setBounds (btnArea.removeFromBottom (juce::jmin (kRowH, btnArea.getHeight()))
                                .removeFromLeft (juce::jmin (74, btnArea.getWidth()))
                                .translated (8, 0));
    guardPanel_.removeFromBottom (juce::jmin (kRowH + 4, guardPanel_.getHeight()));
}

void EedPitchEditor::paintContent (juce::Graphics& g)
{
    const echojay::PitchReading r = proc_.engine().getReading();
    if (r.voiced && r.f0Hz > 0.0f)
    { fitLogHz_ += (std::log ((double) r.f0Hz) - fitLogHz_)
                   * (fitN_ < 200 ? 1.0 / (++fitN_) : 0.005); }
    const float dim = proc_.isBypassed() ? 0.4f : 1.0f;
    g.setOpacity (dim);

    if (! ribbonBounds_.isEmpty())
    {
        std::array<bool, 12> degs {};
        for (int i = 0; i < 12; ++i) degs[(size_t) i] = proc_.corrector().degreeEnabled (i);
        proc_.ribbon().paint (g, ribbonBounds_, degs,
                              proc_.corrector().getKeyRoot(), proc_.isBypassed());
    }

    if (! notePanel_.isEmpty())    paintNotePanel  (g, notePanel_, r);
    if (! numbersPanel_.isEmpty()) paintNumbers    (g, numbersPanel_, r);
    if (! guardPanel_.isEmpty())   paintGuardPanel (g, guardPanel_, r);
    if (! latencyBounds_.isEmpty()) paintLatencyMode (g, latencyBounds_);
    if (! keyAttrBounds_.isEmpty()) paintKeyAttribution (g, keyAttrBounds_);

    g.setOpacity (1.0f);
}

void EedPitchEditor::paintNotePanel (juce::Graphics& g, juce::Rectangle<int> area,
                                     const echojay::PitchReading& r)
{
    char note[8];
    float cents = 0.0f;
    echojay::PitchEngine::noteName (r.voiced ? r.f0Hz : 0.0f, note, sizeof (note), &cents);

    // The note, large; cents colour-graded like a tuner.
    auto noteArea = area.removeFromTop (area.getHeight() * 6 / 10);
    const juce::Colour centsCol = ! r.voiced ? C::text3
                                : std::fabs (cents) < 10.0f ? C::green
                                : std::fabs (cents) < 35.0f ? C::amber : C::red;

    g.setColour (r.voiced ? C::text : C::text3);
    g.setFont (uiFont (juce::jmin (34.0f, noteArea.getHeight() * 0.8f), true));
    g.drawText (r.voiced ? juce::String (note) : juce::String ("-"),
                noteArea.removeFromLeft (noteArea.getWidth() / 2),
                juce::Justification::centredRight);

    g.setColour (centsCol);
    g.setFont (uiFont (juce::jmin (15.0f, noteArea.getHeight() * 0.4f), true));
    g.drawText (r.voiced ? juce::String::formatted ("%+.1f c", cents) : juce::String ("unvoiced"),
                noteArea.reduced (6, 0), juce::Justification::centredLeft);

    // A +/-50 cent tuner bar under it.
    auto barArea = area.reduced (4, 0).removeFromTop (juce::jmax (6, area.getHeight() / 3))
                       .toFloat();
    if (barArea.getHeight() >= 5.0f)
    {
        g.setColour (C::bg4);
        g.fillRoundedRectangle (barArea, 3.0f);
        g.setColour (C::border2);
        g.drawVerticalLine ((int) barArea.getCentreX(),
                            barArea.getY() + 1, barArea.getBottom() - 1);
        if (r.voiced)
        {
            const float x = barArea.getCentreX()
                          + juce::jlimit (-1.0f, 1.0f, cents / 50.0f)
                              * (barArea.getWidth() * 0.5f - 3.0f);
            g.setColour (centsCol);
            g.fillRoundedRectangle (x - 1.5f, barArea.getY() + 1.0f,
                                    3.0f, barArea.getHeight() - 2.0f, 1.5f);
        }
    }
}

void EedPitchEditor::paintNumbers (juce::Graphics& g, juce::Rectangle<int> area,
                                   const echojay::PitchReading& r)
{
    const int rowH = juce::jmax (12, area.getHeight() / 4);
    auto row = [&] (const char* label, const juce::String& value, juce::Colour col)
    {
        auto rr = area.removeFromTop (rowH);
        g.setColour (C::text3);
        g.setFont (uiFont (9.0f));
        g.drawText (label, rr.removeFromLeft (rr.getWidth() * 2 / 5),
                    juce::Justification::centredLeft);
        g.setColour (col);
        g.setFont (uiFont (11.0f, true));
        g.drawText (value, rr, juce::Justification::centredLeft);
    };

    row ("F0", r.voiced ? juce::String (r.f0Hz, 2) + " Hz" : "-",
         r.voiced ? C::text : C::text3);
    // The gate is shown next to the number it gates: during P1 the question
    // "was this frame even tracked?" is asked constantly, and it should be
    // answerable from the readout rather than from the source.
    const float floorConf = proc_.engine().trackingFloor();
    row ("CONF", juce::String (r.confidence, 2)
                   + "  >=" + juce::String (floorConf, 2),
         ! r.voiced ? C::text3
         : r.confidence >= floorConf + 0.10f ? C::green : C::amber);
    row ("STATE", r.voiced ? "VOICED" : "UNVOICED", r.voiced ? C::blue2 : C::text3);
    row ("IN", r.rmsDb <= -119.0f ? juce::String ("-inf dB")
                                  : juce::String (r.rmsDb, 1) + " dB", C::text2);
}

void EedPitchEditor::paintGuardPanel (juce::Graphics& g, juce::Rectangle<int> area,
                                      const echojay::PitchReading& r)
{
    area.removeFromLeft (juce::jmin (8, area.getWidth() / 8));
    const int rowH = juce::jmax (12, area.getHeight() / 4);

    g.setColour (C::text3);
    g.setFont (uiFont (9.0f, true));
    g.drawText ("OCTAVE GUARD", area.removeFromTop (rowH), juce::Justification::centredLeft);

    // Fires-constantly means the WINDOW is wrong for the material (spec
    // §2.1), so the rate carries the amber/red judgement, not the count.
    const double rate = r.voicedHops > 0 ? 100.0 * r.guardFires / r.voicedHops : 0.0;
    const juce::Colour rateCol = rate < 2.0 ? C::green : rate < 10.0 ? C::amber : C::red;

    g.setColour (r.guardFires > 0 ? C::text : C::text2);
    g.setFont (uiFont (11.0f, true));
    g.drawText (juce::String (r.guardFires) + " fires",
                area.removeFromTop (rowH), juce::Justification::centredLeft);

    g.setColour (rateCol);
    g.setFont (uiFont (10.0f, true));
    g.drawText (juce::String (rate, 1) + "% of voiced",
                area.removeFromTop (rowH), juce::Justification::centredLeft);

    g.setColour (C::text3);
    g.setFont (uiFont (9.0f));
    g.drawText (juce::String (r.voicedHops) + " / " + juce::String (r.totalHops) + " hops voiced",
                area.removeFromTop (rowH), juce::Justification::centredLeft);
}

void EedPitchEditor::paintKeyAttribution (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto st = proc_.autoKeyState();
    // The reference line is ALWAYS shown with its provenance (29 Aug 2026
    // ruling): a vocal silently tuned to a grid derived from itself was
    // invisible state that cost days. One suffix tells the whole story.
    // Voice-fit line (1 Sep 2026): amber ONLY when the running pitch
    // evidence suggests a different voice_type - the mismatch that cost
    // this project a full investigation round (alto_tenor default on
    // low-male material; schema's own text warns it causes octave errors).
    juce::String voiceLine;
    if (fitN_ >= 100)
    {
        const int cur = (int) proc_.getParamValue (EedPitchProcessor::kVoiceType);
        int best = cur; double bestD = 1e9;
        for (int t = 0; t < echojay::PitchEngine::kNumVoiceTypes; ++t)
        {
            const auto& vr = echojay::PitchEngine::voiceRange (t);
            const double centre = 0.5 * (std::log ((double) vr.fMinHz)
                                       + std::log ((double) vr.fMaxHz));
            const double d = std::fabs (fitLogHz_ - centre);
            if (d < bestD) { bestD = d; best = t; }
        }
        if (best != cur)
        {
            const auto* sp = EedPitchProcessor::schema().find (EedPitchProcessor::kVoiceType);
            if (sp != nullptr)
                voiceLine = "   voice " + juce::String (sp->choiceLabel (cur))
                          + " - range suggests " + juce::String (sp->choiceLabel (best));
        }
    }
    const juce::String refLine =
        ! st.refAuto        ? "   ref " + juce::String (st.refApplied, 1) + " Hz (manual)"
        : st.refSelfIgnored ? "   ref 440.0 Hz (auto: only this track measurable - not followed)"
        : "   ref " + juce::String (st.refApplied, 1) + " Hz (auto)";
    if (! st.active)
    {
        g.setColour (C::text3);
        g.setFont (uiFont (9.0f));
        if (voiceLine.isNotEmpty()) g.setColour (C::amber);
        g.drawText ("key set by hand" + refLine + voiceLine, area, juce::Justification::centredLeft);
        return;
    }

    if (st.applied)
    {
        static const char* kNames[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        // GREYED, because in auto these are not the user's values to edit -
        // they are a reading, and showing them as live controls would invite
        // an edit that the next block silently overwrites.
        g.setColour (C::text3);
        g.setFont (uiFont (9.0f));
        juce::String line;
        line << "auto: " << kNames[st.root % 12] << (st.minor ? " minor" : " major")
             << "  " << juce::String (st.tuningHz, 1) << " Hz"
             << "  conf " << juce::String (st.conf, 2);
        if (st.sourceName.isNotEmpty()) line << "   from \"" << st.sourceName << "\"";
        line << refLine << voiceLine;
        if (voiceLine.isNotEmpty()) g.setColour (C::amber);
        g.drawText (line, area, juce::Justification::centredLeft);
        return;
    }

    // The fallback is SHOWN, never silent. A user who cannot see that the key
    // was rejected will read chromatic correction as the device misbehaving.
    g.setColour (C::amber);
    g.setFont (uiFont (9.0f, true));
    g.drawText ((st.keySelfIgnored
                    ? juce::String ("auto: only this track measurable - key not followed - using CHROMATIC")
                  : st.conf > 0.0f
                    ? "auto: key confidence " + juce::String (st.conf, 2)
                        + " too low - using CHROMATIC"
                    : juce::String ("auto: no key detected - using CHROMATIC"))
                    + refLine,
                area, juce::Justification::centredLeft);
}

void EedPitchEditor::paintLatencyMode (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto text = area;
    text.removeFromLeft (juce::jmin (156, text.getWidth()));
    if (text.getWidth() < 40) return;

    const bool low = latencyBtn_.getToggleState();

    // The COST, stated where the choice is made. A control that hides what it
    // trades is a control the user cannot reason about.
    g.setColour (low ? C::amber : C::text3);
    g.setFont (uiFont (9.0f, low));
    g.drawText (low ? "shorter lookahead - trades transient accuracy at onsets"
                    : "full lookahead - host compensates the delay",
                text.removeFromTop (text.getHeight() / 2),
                juce::Justification::centredLeft);

    g.setColour (C::text3);
    g.setFont (uiFont (9.0f));
    g.drawText (low ? "for a singer monitoring through the plugin"
                    : "for mixing; switch to TRACKING if someone is singing through it",
                text, juce::Justification::centredLeft);
}

// ---------------------------------------------------------------------------
void EedPitchEditor::syncFromProcessor()
{
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    const int want = (int) proc_.getParamValue (EedPitchProcessor::kVoiceType) + 1;
    if (voiceBox_.getSelectedId() != want)
        voiceBox_.setSelectedId (want, juce::dontSendNotification);

    const int wantTrk = (int) proc_.getParamValue (EedPitchProcessor::kTracking) + 1;
    if (trackBox_.getSelectedId() != wantTrk)
        trackBox_.setSelectedId (wantTrk, juce::dontSendNotification);

    const int wantFm = (int) proc_.getParamValue (EedPitchProcessor::kFormantMode) + 1;
    if (formantBox_.getSelectedId() != wantFm)
        formantBox_.setSelectedId (wantFm, juce::dontSendNotification);

    const double wantHz = proc_.getParamValue (EedPitchProcessor::kTargetHz);
    if (std::abs (wantHz - targetKnob_.getRealValue()) > 1.0e-4)
        targetKnob_.setRealValue (wantHz);

    auto syncKnob = [this] (echojay::device::EchoJayDeviceKnob& k, const char* id)
    {
        const double v = proc_.getParamValue (id);
        if (std::abs (v - k.getRealValue()) > 1.0e-4) k.setRealValue (v);
    };
    syncKnob (retuneKnob_, EedPitchProcessor::kRetuneMs);
    syncKnob (refKnob_,    EedPitchProcessor::kReferenceHz);
    {
        // Dim the REF knob in auto - it shows the manual FIELD, which auto
        // is not using; the attribution line carries the live grid.
        const bool refAuto = proc_.getParamValue (EedPitchProcessor::kRefSource) < 0.5;
        const float ra = refAuto ? 0.45f : 1.0f;
        if (std::abs (refKnob_.getAlpha() - ra) > 0.01f) refKnob_.setAlpha (ra);
        if (refAutoBtn_.getToggleState() != refAuto)
            refAutoBtn_.setToggleState (refAuto, juce::dontSendNotification);
    }
    syncKnob (flexKnob_,   EedPitchProcessor::kFlex);
    syncKnob (humanKnob_,  EedPitchProcessor::kHumanize);

    auto syncBox = [this] (juce::ComboBox& b, const char* id)
    {
        const int want = (int) proc_.getParamValue (id) + 1;
        if (b.getSelectedId() != want) b.setSelectedId (want, juce::dontSendNotification);
    };
    // The mode display follows the params, so a hand-turned knob shows custom
    // here without anything else having to notice.
    syncBox (modeBox_,  EedPitchProcessor::kMode);
    syncBox (keyBox_,   EedPitchProcessor::kKeyRoot);
    syncBox (scaleBox_, EedPitchProcessor::kScale);

    // In key_source = auto the key and scale combos show the DETECTED values
    // (the syncs above read them back) and dim, because they are a reading,
    // not the user's values. They stay CLICKABLE - selecting a value is how
    // the user takes manual control, and the underlying params already flip
    // key_source to manual on any hand write. The attribution line under the
    // ribbon names the source either way.
    {
        const bool keyAuto = proc_.getParamValue (EedPitchProcessor::kKeySource) < 0.5;
        const float alpha = keyAuto ? 0.45f : 1.0f;
        if (std::abs (keyBox_.getAlpha() - alpha) > 0.01f)
        {
            keyBox_.setAlpha (alpha);
            scaleBox_.setAlpha (alpha);
        }
        if (keyAutoBtn_.getToggleState() != keyAuto)
            keyAutoBtn_.setToggleState (keyAuto, juce::dontSendNotification);
    }

    // The formant combo rests DIM on preserve (the default, and the right
    // answer for correction) and lights up only when the setting departs -
    // off and shift are not equal-prominence choices when one of them does
    // nothing at correction-sized shifts and the other costs fidelity.
    {
        const bool departed =
            (int) proc_.getParamValue (EedPitchProcessor::kFormantMode)
                != (int) echojay::PsolaEngine::kFormantPreserve;
        const float alpha = departed ? 1.0f : 0.55f;
        if (std::abs (formantBox_.getAlpha() - alpha) > 0.01f)
            formantBox_.setAlpha (alpha);
    }

    auto syncToggle = [this] (juce::TextButton& b, const char* id)
    {
        const bool want = proc_.getParamValue (id) >= 0.5;
        if (b.getToggleState() != want) b.setToggleState (want, juce::dontSendNotification);
    };
    syncToggle (correctBtn_, EedPitchProcessor::kCorrect);
    syncToggle (vibBtn_,     EedPitchProcessor::kIgnoreVib);

    // The AI can move low_latency and voice_type, and both change the number
    // printed on the mode button.
    const bool wantLow = proc_.getParamValue (EedPitchProcessor::kLowLatency) >= 0.5;
    if (latencyBtn_.getToggleState() != wantLow)
        latencyBtn_.setToggleState (wantLow, juce::dontSendNotification);
    refreshLatencyButton();
}

void EedPitchEditor::timerCallback()
{
    syncFromProcessor();

    // Retune trace drain (3 Sep 2026 investigation): while this editor is
    // open, per-hop decision records stream to a CSV the offline analysis
    // reads. Message thread; append-only; ~200 rows/s of ~60 bytes.
    {
        EedPitchProcessor::TraceRec recs[512];
        const int n = proc_.drainTrace (recs, 512);
        if (n > 0)
        {
            if (! traceFile_.is_open())
            {
                const auto path = juce::File::getSpecialLocation(
                        juce::File::userApplicationDataDirectory)
                    .getChildFile ("EchoJay").getChildFile ("pitch_trace.csv");
                path.getParentDirectory().createDirectory();
                const bool fresh = ! path.existsAsFile();
                traceFile_.open (path.getFullPathName().toRawUTF8(),
                                 std::ios::app);
                if (fresh && traceFile_.is_open())
                    traceFile_ << "tSec,f0Hz,inC,slowC,oscC,aimC,appliedC\n";
            }
            if (traceFile_.is_open())
            {
                for (int i = 0; i < n; ++i)
                    traceFile_ << recs[i].tSec << ',' << recs[i].f0Hz << ','
                               << recs[i].inC << ',' << recs[i].slowC << ','
                               << recs[i].oscC << ',' << recs[i].aimC << ','
                               << recs[i].envC << '\n';
                traceFile_.flush();
            }
        }
    }

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);

    // The readout is pure paint; repaint just the content band.
    repaint (contentBounds());
}

// The hand-control inventory, one entry per control wired above. The audit
// in tools/pitch_mode_test walks the schema against this and fails the build
// on any param that is neither here nor exempted-with-reason there.
const std::vector<const char*>& EedPitchEditor::handControlledParams()
{
    static const std::vector<const char*> ids = {
        EedPitchProcessor::kVoiceType,     // voiceBox_
        EedPitchProcessor::kTracking,      // trackBox_
        EedPitchProcessor::kFormantMode,   // formantBox_
        EedPitchProcessor::kMode,          // modeBox_
        EedPitchProcessor::kKeyRoot,       // keyBox_
        EedPitchProcessor::kScale,         // scaleBox_
        EedPitchProcessor::kKeySource,     // keyAutoBtn_ - closes the one-way gap
        EedPitchProcessor::kCorrect,       // correctBtn_
        EedPitchProcessor::kIgnoreVib,     // vibBtn_
        EedPitchProcessor::kLowLatency,    // latencyBtn_
        EedPitchProcessor::kRetuneMs,      // retuneKnob_
        EedPitchProcessor::kFlex,          // flexKnob_
        EedPitchProcessor::kHumanize,      // humanKnob_
        EedPitchProcessor::kTargetHz,      // targetKnob_
        EedPitchProcessor::kReferenceHz,   // refKnob_ - the control Sean lacked (29 Aug 2026)
        EedPitchProcessor::kRefSource,     // refAutoBtn_ - the way back to auto
        EedPitchProcessor::kResetStats,    // resetBtn_
    };
    return ids;
}
