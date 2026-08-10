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
    constexpr int kDefaultW = 540;
    constexpr int kDefaultH = 170;
}

EedPitchEditor::EedPitchEditor (EedPitchProcessor& p)
    : DeviceEditorBase (p, "PITCH", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("P1 - fixed-target shift; TARGET 0 is passthrough");

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
}

void EedPitchEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    content.reduce (juce::jmin (kPad, content.getWidth() / 4),
                    juce::jmin (6, content.getHeight() / 4));

    // The target dial sits hard right; the readout columns share the rest.
    targetKnob_.setBounds (content.removeFromRight (
        juce::jmin (kKnobW, content.getWidth()))
            .withHeight (juce::jmin (kKnobH, content.getHeight())));
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
    const float dim = proc_.isBypassed() ? 0.4f : 1.0f;
    g.setOpacity (dim);

    if (! notePanel_.isEmpty())    paintNotePanel  (g, notePanel_, r);
    if (! numbersPanel_.isEmpty()) paintNumbers    (g, numbersPanel_, r);
    if (! guardPanel_.isEmpty())   paintGuardPanel (g, guardPanel_, r);

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

    const double wantHz = proc_.getParamValue (EedPitchProcessor::kTargetHz);
    if (std::abs (wantHz - targetKnob_.getRealValue()) > 1.0e-4)
        targetKnob_.setRealValue (wantHz);
}

void EedPitchEditor::timerCallback()
{
    syncFromProcessor();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);

    // The readout is pure paint; repaint just the content band.
    repaint (contentBounds());
}
