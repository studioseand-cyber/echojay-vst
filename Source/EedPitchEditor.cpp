/*
    EedPitchEditor.cpp  —  see EedPitchEditor.h.
*/

#include "EedPitchEditor.h"
#include <algorithm>

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
    setHeaderHint ("RETUNE 0 hard - 400 transparent");

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
    setupKnob (retuneKnob_, EedPitchProcessor::kRetune, 50.0, 0, "", "RETUNE");   // the 0-400 dial (round 46): drives retune_speed_ms + depth
    // The retune FLOOR readout (30 Aug 2026 ruling): the dial keeps 0 so
    // muscle memory from other correctors finds it, but the readout states
    // what it actually is - "0 (6 ms)". Twice now this project was rescued
    // by showing a hidden derived value (reference provenance, placement
    // caption); same principle, one string.
    // The dial number, Antares-style, and nothing else (round 51: the
    // off-curve state is gone - the dial always means what it says).
    retuneKnob_.formatValue = [] (double v) { return juce::String ((int) std::lround (v)); };
    setupKnob (refKnob_,    EedPitchProcessor::kReferenceHz, 0.0, 1, " Hz", "REF");
    setupKnob (flexKnob_,   EedPitchProcessor::kFlex,      0.0, 0, " %",  "FLEX");
    setupKnob (humanKnob_,  EedPitchProcessor::kHumanize,  0.0, 0, " %",  "HUMAN");
    setupKnob (depthKnob_,  EedPitchProcessor::kDepth,     100.0, 0, " %",  "DEPTH");   // 5 Sep 2026: how much of the correction is applied - the gentleness control
    // ADVANCED dials.
    setupKnob (seamKnob_,   EedPitchProcessor::kSeamAttackMs, 60.0, 0, " ms", "SEAM");
    setupKnob (mixKnob_,    EedPitchProcessor::kMix,       100.0, 0, " %",  "MIX");
    setupKnob (outKnob_,    EedPitchProcessor::kOutputDb,    0.0, 1, " dB", "OUT");
    setupKnob (vibDepthKnob_, EedPitchProcessor::kVibDepth, 0.0, 0, " c",  "VIB DEPTH");
    setupKnob (vibRateKnob_,  EedPitchProcessor::kVibRate,  5.5, 1, " Hz", "VIB RATE");
    setupKnob (vibOnsetKnob_, EedPitchProcessor::kVibOnset, 300.0, 0, " ms", "VIB ONSET");
    // FORMANT SHIFT drives formant_mode: the mode switch does nothing by itself
    // for any correction under 2.5 st (round 35), so it is internal and the
    // dial that needs it sets it - non-zero shift = shift mode, zero = preserve.
    setupKnob (fshiftKnob_, EedPitchProcessor::kFormantShift, 0.0, 1, " st", "F.SHIFT");
    fshiftKnob_.onValueChange = [this]
    {
        if (suppressCallbacks_) return;
        const double v = fshiftKnob_.getRealValue();
        proc_.setParamValue (EedPitchProcessor::kFormantMode,
                             std::abs (v) > 0.05 ? (double) echojay::PsolaEngine::kFormantShift
                                                 : (double) echojay::PsolaEngine::kFormantPreserve);
        proc_.setParamValue (EedPitchProcessor::kFormantShift, v);
    };

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
    setupCombo (vibShapeBox_, EedPitchProcessor::kVibShape, "VIB ");

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

    // KEEP VIBRATO: natural_vibrato is a two-state control as shipped (100
    // keeps the singer's vibrato on the shift path; anything else removes it
    // on the legacy path - measured, UI_SIMPLIFICATION round 33/43). The
    // button says so. The continuous gain is a named backlog item.
    styleButton (keepVibBtn_, true);
    keepVibBtn_.setToggleState (proc_.getParamValue (EedPitchProcessor::kNaturalVib) >= 50.0,
                                juce::dontSendNotification);
    keepVibBtn_.onClick = [this]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (EedPitchProcessor::kNaturalVib, keepVibBtn_.getToggleState() ? 100.0 : 0.0);
    };
    addAndMakeVisible (keepVibBtn_);

    // ADVANCED: shows the engineering panel in place of the front row.
    styleButton (advBtn_, true);
    advBtn_.onClick = [this] { advanced_ = advBtn_.getToggleState(); resized(); repaint(); };
    addAndMakeVisible (advBtn_);

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
    const int aw = juce::jmin (84, bar.getWidth());
    advBtn_.setBounds (bar.removeFromRight (aw).reduced (0, 3));
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

    const juce::Rectangle<int> none;
    auto dial = [] (echojay::device::EchoJayDeviceKnob& k, juce::Rectangle<int>& row)
    { k.setBounds (row.removeFromLeft (juce::jmin (kKnobW, row.getWidth()))); };

    // Visibility follows the panel. Front controls: always. Advanced
    // controls and the readouts: only in the advanced panel.
    juce::Component* const advancedSet[] = { &modeBox_, &trackBox_, &latencyBtn_, &correctBtn_, &seamKnob_,
                                             &mixKnob_, &outKnob_, &fshiftKnob_, &depthKnob_, &vibDepthKnob_, &vibRateKnob_,
                                             &vibOnsetKnob_, &vibShapeBox_ };
    juce::Component* const frontSet[]    = { &retuneKnob_, &flexKnob_, &humanKnob_, &refKnob_, &refAutoBtn_,
                                             &keyBox_, &scaleBox_, &keyAutoBtn_, &keepVibBtn_, &vibBtn_ };
    for (auto* c : advancedSet) c->setVisible (advanced_);
    for (auto* c : frontSet)    c->setVisible (! advanced_);

    // The key ATTRIBUTION line, in both panels: where the key and the
    // reference came from is what makes a wrong one diagnosable, and it
    // is the badge's explanation.
    keyAttrBounds_ = content.removeFromBottom (juce::jmin (16, content.getHeight() / 5));

    if (! advanced_)
    {
        // FRONT: one row of controls under the ribbon, which takes the rest.
        auto row = content.removeFromBottom (juce::jmin (kKnobH, content.getHeight() / 2));
        dial (retuneKnob_, row); dial (flexKnob_, row); dial (humanKnob_, row);
        row.removeFromLeft (juce::jmin (6, row.getWidth()));
        // KEY / SCALE with the AUTO badge beside them: the badge lights when
        // a bus is feeding the key; picking a key or scale overrides it;
        // clicking the badge returns to auto.
        auto col = row.removeFromLeft (juce::jmin (104, row.getWidth()));
        const int comboH = juce::jmax (1, col.getHeight() / 2);
        keyBox_.setBounds (col.removeFromTop (comboH).reduced (1));
        scaleBox_.setBounds (col.removeFromTop (comboH).reduced (1));
        auto badge = row.removeFromLeft (juce::jmin (40, row.getWidth()));
        keyAutoBtn_.setBounds (badge.reduced (1, comboH / 4));
        row.removeFromLeft (juce::jmin (6, row.getWidth()));
        // REF dial with its own badge.
        dial (refKnob_, row);
        auto rb = row.removeFromLeft (juce::jmin (40, row.getWidth()));
        refAutoBtn_.setBounds (rb.reduced (1, rb.getHeight() / 4));
        row.removeFromLeft (juce::jmin (6, row.getWidth()));
        // The two switches.
        auto sw = row.removeFromLeft (juce::jmin (96, row.getWidth()));
        keepVibBtn_.setBounds (sw.removeFromTop (juce::jmin (kRowH, sw.getHeight())).reduced (1));
        vibBtn_.setBounds (sw.removeFromTop (juce::jmin (kRowH, sw.getHeight())).reduced (1));
        // Round 49 (Sean): no off-curve strip - the dial's own "(off)" is the
        // indication, and the paragraph was the first thing a user saw.
        offCurveBounds_ = {};
        content.removeFromBottom (juce::jmin (4, content.getHeight()));
        ribbonBounds_ = content;
        // Round 50 (Sean's screenshot): NOTHING advanced carries over - every
        // advanced rectangle is cleared here, not just the panels.
        notePanel_ = numbersPanel_ = guardPanel_ = latencyBounds_ = numbersBox_ = none;
        advGroups_.clear(); advCaptions_.clear();
        return;
    }

    // ADVANCED (round 47 layout): three FRAMED GROUPS, each with a title strip
    // and its own captions, laid out bottom-up. The readouts take what the
    // groups leave; the ribbon shows only if a useful strip remains.
    advCaptions_.clear(); advGroups_.clear();
    constexpr int kTitleH = 12, kCapStripH = 10, kFramePad = 4;
    auto takeBottom = [&] (int h) { return content.removeFromBottom (juce::jmin (h, content.getHeight())); };
    auto captioned = [&] (juce::Rectangle<int>& caps, juce::Rectangle<int>& row, int w, const char* cap) -> juce::Rectangle<int>
    {
        w = juce::jmin (w, row.getWidth());
        advCaptions_.push_back ({ caps.removeFromLeft (w), cap });
        auto r = row.removeFromLeft (w);
        caps.removeFromLeft (juce::jmin (6, caps.getWidth())); row.removeFromLeft (juce::jmin (6, row.getWidth()));
        return r;
    };

    offCurveBounds_ = {};
    content.removeFromBottom (juce::jmin (2, content.getHeight()));

    // Round 50 (Sean's screenshot: the box overflowed its frame): the READOUTS
    // group is reserved FIRST, at the height its rows need, and the three
    // control groups shrink into what remains - never the other way round.
    constexpr int kReadoutRows = 4, kReadoutRowH = 12;
    const int readoutsWant = kTitleH + kReadoutRows * kReadoutRowH + 6 + 4;
    const int groupsWant   = (kTitleH + kKnobH + kFramePad) * 2 + (kTitleH + kCapStripH + kRowH + 13 + kFramePad) + 8;
    const int readoutsH    = juce::jlimit (0, content.getHeight(), juce::jmax (readoutsWant, content.getHeight() - groupsWant));
    auto readoutsArea      = content.removeFromTop (readoutsH);
    content.removeFromTop (juce::jmin (4, content.getHeight()));
    const int groupShare   = juce::jmax (0, (content.getHeight() - 8) / 3);   // each control group gets a third of what is left

    // (1) VIBRATO GENERATOR: three dials + the shape choice, one frame.
    {
        auto grp = takeBottom (juce::jmin (kTitleH + kKnobH + kFramePad, groupShare));
        advGroups_.push_back ({ grp, "VIBRATO GENERATOR  (added vibrato; the singer's own is KEEP VIBRATO on the front)" });
        auto inner = grp.reduced (kFramePad, 0); inner.removeFromTop (kTitleH);
        auto row = inner.removeFromTop (juce::jmin (kKnobH, inner.getHeight()));
        dial (vibDepthKnob_, row); dial (vibRateKnob_, row); dial (vibOnsetKnob_, row);
        row.removeFromLeft (juce::jmin (6, row.getWidth()));
        auto cell = row.removeFromLeft (juce::jmin (110, row.getWidth()));
        advCaptions_.push_back ({ cell.removeFromTop (kCapStripH), "SHAPE" });
        vibShapeBox_.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), juce::jmin (kRowH, cell.getHeight())).reduced (1, 0));
    }
    content.removeFromBottom (juce::jmin (4, content.getHeight()));

    // (2) CORRECTION: mode, tracking, lookahead, master enable - captions
    //     above, the latency explanation on its own full-width line below.
    {
        auto grp = takeBottom (juce::jmin (kTitleH + kCapStripH + kRowH + 13 + kFramePad, groupShare));
        advGroups_.push_back ({ grp, "CORRECTION" });
        auto inner = grp.reduced (kFramePad, 0); inner.removeFromTop (kTitleH);
        auto caps = inner.removeFromTop (kCapStripH);
        auto row  = inner.removeFromTop (juce::jmin (kRowH, inner.getHeight()));
        modeBox_.setBounds    (captioned (caps, row, 120, "MODE").reduced (1, 1));
        trackBox_.setBounds   (captioned (caps, row, 100, "TRACKING").reduced (1, 1));
        latencyBtn_.setBounds (captioned (caps, row, 130, "LOOKAHEAD").reduced (1, 1));
        correctBtn_.setBounds (captioned (caps, row,  90, "MASTER ENABLE").reduced (1, 1));
        latencyBounds_ = inner;   // the whole remaining line: nothing sits beside it
    }
    content.removeFromBottom (juce::jmin (4, content.getHeight()));

    // (3) ENGINE: the dials. DEPTH is the curve override (round 46).
    {
        auto grp = takeBottom (juce::jmin (kTitleH + kKnobH + kFramePad, groupShare));
        advGroups_.push_back ({ grp, "ENGINE  (DEPTH here is the override: turning it leaves the RETUNE curve)" });
        auto inner = grp.reduced (kFramePad, 0); inner.removeFromTop (kTitleH);
        auto row = inner.removeFromTop (juce::jmin (kKnobH, inner.getHeight()));
        dial (seamKnob_, row); dial (mixKnob_, row); dial (outKnob_, row); dial (fshiftKnob_, row); dial (depthKnob_, row);
    }
    content.removeFromBottom (juce::jmin (4, content.getHeight()));

    // (4) READOUTS: note + tuner bar | numbers | guard log, framed; then the ribbon.
    {
        // The area reserved above, plus whatever the groups did not need.
        auto grp = readoutsArea.withHeight (readoutsArea.getHeight() + juce::jmax (0, content.getHeight()));
        content = {};
        advGroups_.push_back ({ grp, "READOUTS" });
        auto inner = grp.reduced (kFramePad, 0); inner.removeFromTop (kTitleH);
        notePanel_    = inner.removeFromLeft (inner.getWidth() * 2 / 5);
        // Round 49 (Sean): the numbers in ONE bounded box at the top right -
        // F0 / CONF / STATE / IN on its left, the octave-guard figures on its
        // right - not spread across the group. The box is drawn from numbersBox_.
        inner.removeFromLeft (juce::jmin (8, inner.getWidth()));
        numbersBox_   = inner.removeFromTop (juce::jmin (inner.getHeight(), kReadoutRows * kReadoutRowH + 6));
        auto box      = numbersBox_.reduced (6, 3);
        guardPanel_   = box.removeFromRight (juce::jmax (0, box.getWidth() * 9 / 20));
        numbersPanel_ = box;
    }
    ribbonBounds_ = {};
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

    if (advanced_)
    {
        for (const auto& grp : advGroups_)
        {
            if (grp.r.isEmpty()) continue;
            g.setColour (C::border2);
            g.drawRoundedRectangle (grp.r.toFloat().reduced (0.5f), 3.0f, 1.0f);
            g.setColour (C::text2);
            g.setFont (uiFont (9.0f, true));
            g.drawText (grp.title, grp.r.reduced (6, 0).removeFromTop (12), juce::Justification::centredLeft);
        }
        g.setColour (C::text3);
        g.setFont (uiFont (9.0f, true));
        for (const auto& c : advCaptions_)
            if (! c.r.isEmpty()) g.drawText (c.text, c.r, juce::Justification::centred);
    }
    if (! numbersBox_.isEmpty())
    {
        g.setColour (C::bg4);
        g.fillRoundedRectangle (numbersBox_.toFloat(), 3.0f);
        g.setColour (C::border2);
        g.drawRoundedRectangle (numbersBox_.toFloat().reduced (0.5f), 3.0f, 1.0f);
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
    // Rows and fonts follow the area (round 47): four rows must fit whatever
    // the advanced layout leaves, never overprint the row below.
    // Round 50: rows are EXACTLY a quarter of the area - a minimum row height
    // is how the box overflowed its frame in Sean's screenshot.
    const int rowH = juce::jmax (1, area.getHeight() / 4);
    const float valPt = juce::jlimit (6.0f, 11.0f, rowH - 2.0f), labPt = juce::jlimit (6.0f, 9.0f, rowH - 3.0f);
    auto row = [&] (const char* label, const juce::String& value, juce::Colour col)
    {
        auto rr = area.removeFromTop (rowH);
        g.setColour (C::text3);
        g.setFont (uiFont (labPt));
        g.drawText (label, rr.removeFromLeft (rr.getWidth() * 2 / 5),
                    juce::Justification::centredLeft);
        g.setColour (col);
        g.setFont (uiFont (valPt, true));
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
    const int rowH = juce::jmax (1, area.getHeight() / 4);   // round 50: no minimum (see paintNumbers)
    const float bigPt = juce::jlimit (6.0f, 11.0f, rowH - 2.0f), midPt = juce::jlimit (6.0f, 10.0f, rowH - 2.0f), smallPt = juce::jlimit (6.0f, 9.0f, rowH - 3.0f);

    g.setColour (C::text3);
    g.setFont (uiFont (smallPt, true));
    g.drawText ("OCTAVE GUARD", area.removeFromTop (rowH), juce::Justification::centredLeft);

    // Fires-constantly means the WINDOW is wrong for the material (spec
    // §2.1), so the rate carries the amber/red judgement, not the count.
    const double rate = r.voicedHops > 0 ? 100.0 * r.guardFires / r.voicedHops : 0.0;
    const juce::Colour rateCol = rate < 2.0 ? C::green : rate < 10.0 ? C::amber : C::red;

    g.setColour (r.guardFires > 0 ? C::text : C::text2);
    g.setFont (uiFont (bigPt, true));
    g.drawText (juce::String (r.guardFires) + " fires",
                area.removeFromTop (rowH), juce::Justification::centredLeft);

    g.setColour (rateCol);
    g.setFont (uiFont (midPt, true));
    g.drawText (juce::String (rate, 1) + "% of voiced",
                area.removeFromTop (rowH), juce::Justification::centredLeft);

    g.setColour (C::text3);
    g.setFont (uiFont (smallPt));
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
    // Round 49 (Sean: "bottom one should stay whether set by hand or not"):
    // the line is UNCONDITIONAL - key, reference and voice, in every state.
    const juce::String refLine =
        ! st.refAuto        ? "   ref " + juce::String (st.refApplied, 1) + " Hz (by hand)"
        : st.refSelfIgnored ? "   ref 440.0 Hz (auto: only this track measurable - not followed)"
        : "   ref " + juce::String (st.refApplied, 1) + " Hz (auto)";
    if (voiceLine.isEmpty())
    {
        const auto* sp = EedPitchProcessor::schema().find (EedPitchProcessor::kVoiceType);
        if (sp != nullptr)
            voiceLine = "   voice " + juce::String (sp->choiceLabel (proc_.getParamValue (EedPitchProcessor::kVoiceType))).toUpperCase().replace ("_", "/");   // as the combo shows it
    }
    static const char* kNames[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    const bool keyAuto = proc_.getParamValue (EedPitchProcessor::kKeySource) < 0.5;
    juce::String keyLine;
    juce::Colour col = C::text3;
    if (! keyAuto)
    {
        const auto* sc = EedPitchProcessor::schema().find (EedPitchProcessor::kScale);
        const int root = (int) proc_.getParamValue (EedPitchProcessor::kKeyRoot);
        keyLine = "key " + juce::String (kNames[juce::jlimit (0, 11, root)]) + " "
                + (sc != nullptr ? juce::String (sc->choiceLabel (proc_.getParamValue (EedPitchProcessor::kScale))) : juce::String())
                + " (by hand)";
    }
    else if (st.applied)
    {
        // Greyed in auto: these are a reading, not the user's values to edit.
        keyLine << "key auto: " << kNames[st.root % 12] << (st.minor ? " minor" : " major")
                << "  " << juce::String (st.tuningHz, 1) << " Hz"
                << "  conf " << juce::String (st.conf, 2);
        if (st.sourceName.isNotEmpty()) keyLine << "   from \"" << st.sourceName << "\"";
    }
    else
    {
        // The fallback is SHOWN, never silent: a user who cannot see that the
        // key was rejected reads chromatic correction as the device misbehaving.
        col = C::amber;
        keyLine = st.keySelfIgnored ? juce::String ("key auto: only this track measurable - not followed - using CHROMATIC")
                : st.conf > 0.0f    ? "key auto: confidence " + juce::String (st.conf, 2) + " too low - using CHROMATIC"
                                    : juce::String ("key auto: no bus key yet - using CHROMATIC");
    }
    if (voiceLine.contains ("suggests")) col = C::amber;
    g.setColour (col);
    g.setFont (uiFont (9.0f, col == C::amber));
    g.drawText (keyLine + refLine + voiceLine, area, juce::Justification::centredLeft);
}

void EedPitchEditor::paintLatencyMode (juce::Graphics& g, juce::Rectangle<int> area)
{
    // One line, full width, under the CORRECTION controls: the cost of the
    // lookahead choice, stated where the choice is made, and nowhere near a
    // button (round 47: the two-line caption overlapped CORRECT and truncated).
    if (area.getHeight() < 8) return;
    const bool low = latencyBtn_.getToggleState();
    g.setColour (low ? C::amber : C::text3);
    g.setFont (uiFont (9.0f, low));
    g.drawText (low ? "TRACKING: shorter lookahead, trades onset accuracy - for a singer monitoring through the plugin"
                    : "MIXING: full lookahead (" + juce::String (currentLatencyMs (false)) + " ms, the host compensates) - switch to TRACKING if someone is singing through it",
                area, juce::Justification::centredLeft);
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


    auto syncKnob = [this] (echojay::device::EchoJayDeviceKnob& k, const char* id)
    {
        const double v = proc_.getParamValue (id);
        if (std::abs (v - k.getRealValue()) > 1.0e-4) k.setRealValue (v);
    };
    syncKnob (retuneKnob_, EedPitchProcessor::kRetune);
    {
        // Round 50 (Sean's screenshot: REF read 439.2 while the line said 440
        // auto): the round-25 display rule, applied to the front control -
        // WHILE ON AUTO, SHOW THE APPLIED VALUE GREYED, NOT THE STORED FIELD.
        // Turning the knob still writes reference_hz, which takes manual.
        const bool refAutoNow = proc_.getParamValue (EedPitchProcessor::kRefSource) < 0.5;
        if (refAutoNow)
        {
            const double ap = (double) proc_.autoKeyState().refApplied;
            if (std::abs (ap - refKnob_.getRealValue()) > 1.0e-3) refKnob_.setRealValue (ap);
        }
        else syncKnob (refKnob_, EedPitchProcessor::kReferenceHz);
    }
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
    syncKnob (depthKnob_,  EedPitchProcessor::kDepth);
    syncKnob (seamKnob_,   EedPitchProcessor::kSeamAttackMs);
    syncKnob (mixKnob_,    EedPitchProcessor::kMix);
    syncKnob (outKnob_,    EedPitchProcessor::kOutputDb);
    syncKnob (fshiftKnob_, EedPitchProcessor::kFormantShift);
    syncKnob (vibDepthKnob_, EedPitchProcessor::kVibDepth);
    syncKnob (vibRateKnob_,  EedPitchProcessor::kVibRate);
    syncKnob (vibOnsetKnob_, EedPitchProcessor::kVibOnset);

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
    syncBox (vibShapeBox_, EedPitchProcessor::kVibShape);

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

    auto syncToggle = [this] (juce::TextButton& b, const char* id)
    {
        const bool want = proc_.getParamValue (id) >= 0.5;
        if (b.getToggleState() != want) b.setToggleState (want, juce::dontSendNotification);
    };
    syncToggle (correctBtn_, EedPitchProcessor::kCorrect);
    syncToggle (vibBtn_,     EedPitchProcessor::kIgnoreVib);
    {
        const bool keep = proc_.getParamValue (EedPitchProcessor::kNaturalVib) >= 50.0;
        if (keepVibBtn_.getToggleState() != keep) keepVibBtn_.setToggleState (keep, juce::dontSendNotification);
    }

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
        EedPitchProcessor::kVoiceType,     // voiceBox_ (header; FRONT)
        EedPitchProcessor::kKeyRoot,       // keyBox_
        EedPitchProcessor::kScale,         // scaleBox_
        EedPitchProcessor::kKeySource,     // keyAutoBtn_ - the AUTO badge on KEY/SCALE
        EedPitchProcessor::kRetune,        // retuneKnob_ - the 0-400 dial (round 46)
        EedPitchProcessor::kFlex,          // flexKnob_
        EedPitchProcessor::kHumanize,      // humanKnob_
        EedPitchProcessor::kDepth,         // depthKnob_ (ADVANCED since round 46: the override off the curve)
        EedPitchProcessor::kNaturalVib,    // keepVibBtn_ - KEEP VIBRATO, the two-state control it is
        EedPitchProcessor::kIgnoreVib,     // vibBtn_
        EedPitchProcessor::kReferenceHz,   // refKnob_
        EedPitchProcessor::kRefSource,     // refAutoBtn_ - the AUTO badge on REF
        EedPitchProcessor::kMode,          // modeBox_ (ADVANCED)
        EedPitchProcessor::kTracking,      // trackBox_ (ADVANCED)
        EedPitchProcessor::kLowLatency,    // latencyBtn_ (ADVANCED)
        EedPitchProcessor::kCorrect,       // correctBtn_ (ADVANCED - the chain has the slot on/off)
        EedPitchProcessor::kSeamAttackMs,  // seamKnob_ (ADVANCED)
        EedPitchProcessor::kMix,           // mixKnob_ (ADVANCED)
        EedPitchProcessor::kOutputDb,      // outKnob_ (ADVANCED)
        EedPitchProcessor::kFormantShift,  // fshiftKnob_ (ADVANCED)
        EedPitchProcessor::kFormantMode,   // set by fshiftKnob_ (non-zero shift = shift mode; the switch alone does nothing under 2.5 st)
        EedPitchProcessor::kVibDepth,      // vibDepthKnob_ (ADVANCED)
        EedPitchProcessor::kVibRate,       // vibRateKnob_ (ADVANCED)
        EedPitchProcessor::kVibOnset,      // vibOnsetKnob_ (ADVANCED)
        EedPitchProcessor::kVibShape,      // vibShapeBox_ (ADVANCED)
    };
    return ids;
}

// ---------------------------------------------------------------------------
// Round 50: the layout audit (see the suite: EJ_EDITOR_SNAP and the always-on
// bounds check at several sizes)
// ---------------------------------------------------------------------------
juce::StringArray EedPitchEditor::auditLayout() const
{
    juce::StringArray bad;
    const auto content = contentBounds();
    auto within = [] (juce::Rectangle<int> a, juce::Rectangle<int> b) { return b.contains (a); };
    auto name = [&] (const juce::Component* c) -> juce::String
    {
        const std::pair<const juce::Component*, const char*> names[] = {
            { &retuneKnob_, "RETUNE" }, { &flexKnob_, "FLEX" }, { &humanKnob_, "HUMAN" }, { &depthKnob_, "DEPTH" }, { &refKnob_, "REF" },
            { &seamKnob_, "SEAM" }, { &mixKnob_, "MIX" }, { &outKnob_, "OUT" }, { &fshiftKnob_, "F.SHIFT" }, { &vibDepthKnob_, "VIB DEPTH" },
            { &vibRateKnob_, "VIB RATE" }, { &vibOnsetKnob_, "VIB ONSET" }, { &keyBox_, "KEY" }, { &scaleBox_, "SCALE" }, { &modeBox_, "MODE" },
            { &trackBox_, "TRACKING" }, { &vibShapeBox_, "SHAPE" }, { &keyAutoBtn_, "KEY AUTO" }, { &refAutoBtn_, "REF AUTO" },
            { &keepVibBtn_, "KEEP VIBRATO" }, { &vibBtn_, "IGN VIB" }, { &latencyBtn_, "LOOKAHEAD" }, { &correctBtn_, "CORRECT" } };
        for (const auto& n : names) if (n.first == c) return n.second;
        return "?";
    };
    const juce::Component* frontSet[] = { &retuneKnob_, &flexKnob_, &humanKnob_, &refKnob_, &refAutoBtn_, &keyBox_, &scaleBox_, &keyAutoBtn_, &keepVibBtn_, &vibBtn_ };
    const juce::Component* advSet[]   = { &modeBox_, &trackBox_, &latencyBtn_, &correctBtn_, &seamKnob_, &mixKnob_, &outKnob_, &fshiftKnob_, &depthKnob_,
                                          &vibDepthKnob_, &vibRateKnob_, &vibOnsetKnob_, &vibShapeBox_ };
    auto groupFor = [&] (juce::Rectangle<int> r) -> const Group*
    { for (const auto& grp : advGroups_) if (within (r, grp.r)) return &grp; return nullptr; };

    if (! advanced_)
    {
        for (auto* c : frontSet)
        {
            if (! c->isVisible()) bad.add ("front: " + name (c) + " not visible");
            else if (! within (c->getBounds(), content)) bad.add ("front: " + name (c) + " outside the content area " + c->getBounds().toString());
        }
        for (auto* c : advSet) if (c->isVisible()) bad.add ("front: advanced control " + name (c) + " visible");
        if (! numbersBox_.isEmpty())   bad.add ("front: READOUTS numbers box carried over " + numbersBox_.toString());
        if (! notePanel_.isEmpty())    bad.add ("front: note panel carried over");
        if (! numbersPanel_.isEmpty()) bad.add ("front: numbers panel carried over");
        if (! guardPanel_.isEmpty())   bad.add ("front: guard panel carried over");
        if (! latencyBounds_.isEmpty()) bad.add ("front: latency line carried over");
        if (! advGroups_.empty() || ! advCaptions_.empty()) bad.add ("front: advanced groups/captions carried over");
        if (ribbonBounds_.isEmpty() || ! within (ribbonBounds_, content)) bad.add ("front: ribbon missing or outside the content");
        return bad;
    }
    for (auto* c : advSet)
    {
        if (! c->isVisible()) { bad.add ("advanced: " + name (c) + " not visible"); continue; }
        if (! within (c->getBounds(), content)) bad.add ("advanced: " + name (c) + " outside the content area " + c->getBounds().toString());
        if (groupFor (c->getBounds()) == nullptr) bad.add ("advanced: " + name (c) + " not inside any group frame " + c->getBounds().toString());
    }
    for (auto* c : frontSet) if (c->isVisible()) bad.add ("advanced: front control " + name (c) + " visible");
    for (const auto& grp : advGroups_) if (! within (grp.r, content)) bad.add ("advanced: group '" + grp.title.upToFirstOccurrenceOf (" ", false, false) + "' outside the content " + grp.r.toString());
    for (const auto& cap : advCaptions_) if (groupFor (cap.r) == nullptr) bad.add ("advanced: caption '" + cap.text + "' outside every group " + cap.r.toString());
    const Group* readouts = nullptr; for (const auto& grp : advGroups_) if (grp.title == "READOUTS") readouts = &grp;
    if (readouts == nullptr) bad.add ("advanced: no READOUTS group");
    else
    {
        if (! within (numbersBox_, readouts->r)) bad.add ("advanced: numbers box outside READOUTS " + numbersBox_.toString() + " vs " + readouts->r.toString());
        if (! within (notePanel_, readouts->r)) bad.add ("advanced: note panel outside READOUTS");
        if (! within (numbersPanel_, numbersBox_)) bad.add ("advanced: numbers panel outside its box");
        if (! within (guardPanel_, numbersBox_)) bad.add ("advanced: guard panel outside its box");
        // The painters draw exactly four rows of area/4 each, so the rows fit by
        // construction; what can still go wrong is a box too short to READ.
        if (numbersPanel_.getHeight() < 4 * 8) bad.add ("advanced: numbers rows too short to read (" + juce::String (numbersPanel_.getHeight()) + " px for 4 rows)");
        if (guardPanel_.getHeight() < 4 * 8) bad.add ("advanced: guard rows too short to read (" + juce::String (guardPanel_.getHeight()) + " px for 4 rows)");
    }
    const Group* corr = nullptr; for (const auto& grp : advGroups_) if (grp.title == "CORRECTION") corr = &grp;
    if (corr != nullptr && ! within (latencyBounds_, corr->r)) bad.add ("advanced: latency line outside CORRECTION");
    for (const auto& grp : advGroups_)
        for (const auto& other : advGroups_)
            if (&grp != &other && grp.r.intersects (other.r)) bad.add ("advanced: groups overlap: " + grp.title.upToFirstOccurrenceOf (" ", false, false) + " / " + other.title.upToFirstOccurrenceOf (" ", false, false));
    return bad;
}
