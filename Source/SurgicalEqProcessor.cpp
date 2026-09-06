/*
    SurgicalEqProcessor.cpp  —  see SurgicalEqProcessor.h.

    Not compiled in the Cowork sandbox (no JUCE there); built in the normal
    EchoJay project. Written against the JUCE EchoJay already links.
*/

#include "SurgicalEqProcessor.h"
#include "SurgicalEqEditor.h"
#include "EedDeviceRegistry.h"
#include "EqResonanceHunt.h"
#include "EqNote.h"
#include "EqPresets.h"

#include <type_traits>
#include <vector>

// Nothing constructs a SurgicalEqProcessor yet (ChainHost integration is the
// last step of the plan), so merely compiling this file would NOT catch an
// unimplemented juce::AudioProcessor pure virtual — the class would just be
// silently abstract until the day ChainHost tries to `new` it. Assert it is
// concrete now, so that failure mode surfaces here instead of at integration.
static_assert (! std::is_abstract<SurgicalEqProcessor>::value,
               "SurgicalEqProcessor is abstract: a juce::AudioProcessor pure "
               "virtual is unimplemented.");

using echojay::BandSpec;
using echojay::BandType;
using echojay::EqMove;

// ---------------------------------------------------------------------------
SurgicalEqProcessor::SurgicalEqProcessor()
{
    // Buses come from EedDeviceProcessor (stereo in/out, mono tolerated).
    // bands_ default to BandSpec{} (all disabled) — a clean, transparent EQ.
}

// ---- the dialable contract (device-global knobs) --------------------------
const echojay::ParamSchema& SurgicalEqProcessor::schema()
{
    // The ±24 dB trim range matches the OUT dial in the editor and the clamp in
    // applyEqSettings — one number, three readers.
    static const echojay::ParamSchema s ({
        { kOutputDb, "dB", -24.0, 24.0, 0.0,
          "device output trim, applied after the bands", false },

        { kAutoGain, "", 0.0, 1.0, 0.0,
          "cancel the loudness change the bands cause, so a tonal move can be "
          "judged without a level change confounding it", true },

        { kPhaseMode, "", 0.0, 1.0, 0.0,
          "zero = minimum-phase, no latency (default, right for tracking); "
          "linear = linear-phase FIR for mastering/parallel/phase-critical "
          "work, adds ~53 ms latency the host compensates", false,
          { "zero", "linear" } },

        { kMsMode, "", 0.0, 1.0, 0.0,
          "analyzer view only: overlay separate mid and side spectrum traces; "
          "per-band ROUTING is the band's own channel field, not this", true },
    });
    return s;
}

bool SurgicalEqProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kOutputDb)  { setOutputDb ((float) value);   return true; }
    if (id == kAutoGain)  { setAutoGain (value >= 0.5);    return true; }
    if (id == kPhaseMode) { setPhaseMode (value >= 0.5 ? PhaseMode::Linear
                                                       : PhaseMode::Zero); return true; }
    if (id == kMsMode)    { setMsMode (value >= 0.5);      return true; }
    return false;
}

double SurgicalEqProcessor::getParamValue (const juce::String& id) const
{
    if (id == kOutputDb)  return (double) getOutputDb();
    if (id == kAutoGain)  return getAutoGain() ? 1.0 : 0.0;
    if (id == kPhaseMode) return getPhaseMode() == PhaseMode::Linear ? 1.0 : 0.0;
    if (id == kMsMode)    return getMsMode() ? 1.0 : 0.0;
    return 0.0;
}

// ---- audio ----------------------------------------------------------------
void SurgicalEqProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    blockSize_  = samplesPerBlock;
    engine_.prepare (sampleRate, samplesPerBlock, 2);
    pushToEngine();

    // Linear mode delays; tell the host again on every prepare (the engine's
    // latency is mode-dependent, not rate-dependent, but re-reporting is free
    // and hosts re-query here anyway).
    setLatencySamples (engine_.latencySamples());

    // Not RT: clear the taps so a restart never shows the previous session's
    // audio decaying out of the analyzer.
    preRing_.clear();
    postRing_.clear();
    preSideRing_.clear();
    postSideRing_.clear();
}

void SurgicalEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Pre taps: the signal arriving, which is what you are EQing against —
    // the mid lane feeds both the analyzer and the resonance hunt.
    preRing_.push (buffer, false);
    preSideRing_.push (buffer, true);

    // engine handles bypass/solo internally; passthrough is a no-op copy-free path
    engine_.process (buffer.getArrayOfWritePointers(),
                     buffer.getNumChannels(), buffer.getNumSamples());

    // Post taps: the same block after the curve, so the analyzer can show what
    // the EQ actually did rather than only what went in.
    postRing_.push (buffer, false);
    postSideRing_.push (buffer, true);
}

// ---- analysis rings -------------------------------------------------------
int SurgicalEqProcessor::readAnalysis (float* dest, int maxSamples,
                                       bool postEq, bool side) const noexcept
{
    if (side)
        return postEq ? postSideRing_.read (dest, maxSamples)
                      : preSideRing_.read  (dest, maxSamples);
    return postEq ? postRing_.read (dest, maxSamples)
                  : preRing_.read  (dest, maxSamples);
}

// ---- phase mode -----------------------------------------------------------
void SurgicalEqProcessor::setPhaseMode (PhaseMode m)
{
    if (engine_.getPhaseMode() == m) return;
    engine_.setPhaseMode (m);
    // A LIVE re-report, not just at prepare: the host has to re-compensate the
    // moment the mode flips, exactly like the Harmonic cluster's oversampling.
    setLatencySamples (engine_.latencySamples());
}

double SurgicalEqProcessor::getTailLengthSeconds() const
{
    return sampleRate_ > 0.0 ? (double) engine_.latencySamples() / sampleRate_ : 0.0;
}

// ---- editor ---------------------------------------------------------------
juce::AudioProcessorEditor* SurgicalEqProcessor::createEditor()
{
    return new SurgicalEqEditor (*this);
}

// ---- band model -----------------------------------------------------------
void SurgicalEqProcessor::setBand (int index, const BandSpec& spec)
{
    if (index < 0 || index >= kNumBands) return;
    const juce::ScopedLock sl (modelLock_);
    bands_[index] = spec;
    engine_.setBand (index, spec);
}

BandSpec SurgicalEqProcessor::getBand (int index) const
{
    const juce::ScopedLock sl (modelLock_);
    if (index < 0 || index >= kNumBands) return {};
    return bands_[index];
}

void SurgicalEqProcessor::setBypassed (bool b)
{
    bypassed_.store (b);
    engine_.setBypassed (b);
}

void SurgicalEqProcessor::setSoloBand (int index) { engine_.setSoloBand (index); }
int  SurgicalEqProcessor::getSoloBand() const     { return engine_.getSoloBand(); }

void SurgicalEqProcessor::setOutputDb (float db) { engine_.setOutputDb (db); }
void SurgicalEqProcessor::setAutoGain (bool on)  { engine_.setAutoGain (on); }

void SurgicalEqProcessor::pushToEngine()
{
    engine_.setBands (bands_, kNumBands);
}

// ---- AI apply (exact) -----------------------------------------------------
BandSpec SurgicalEqProcessor::specFromVar (const juce::var& e, bool& disableOut,
                                           int& bandOut, bool& okOut)
{
    BandSpec s;
    okOut = true;
    disableOut = (bool) e.getProperty ("disable", false);
    bandOut    = (int)  e.getProperty ("band", -1);      // 1-based, -1 = auto

    // type (defaults to bell if absent/unrecognised)
    BandType t = BandType::Bell;
    if (e.hasProperty ("type"))
    {
        const juce::String ts = e.getProperty ("type", "bell").toString();
        echojay::parseBandType (ts.toRawUTF8(), t);      // leaves Bell on failure
    }
    s.type = t;

    s.freqHz = (float) (double) e.getProperty ("freq_hz", (double) s.freqHz);

    // A musical `note` ("G5", "C#3") resolves to freq_hz when freq_hz is
    // absent — explicit Hz always wins, so a move carrying both is not
    // second-guessed. An unparseable note leaves the default rather than
    // guessing a pitch.
    if (! e.hasProperty ("freq_hz") && e.hasProperty ("note"))
    {
        const juce::String note = e.getProperty ("note", "").toString().trim();
        float hz = 0.0f;
        if (echojay::parseNoteToFreq (note.toRawUTF8(), hz))
            s.freqHz = hz;
    }

    s.gainDb = (float) (double) e.getProperty ("gain_db", (double) s.gainDb);
    s.q      = (float) (double) e.getProperty ("q",       (double) s.q);
    s.slopeDbPerOct = (int) e.getProperty ("slope_db_oct", s.slopeDbPerOct);
    s.enabled = (bool) e.getProperty ("enabled", true);  // a set enables by default

    // Per-band routing (P2), tolerant like every other name here. Unknown
    // labels leave the default (stereo) rather than guessing a lane.
    if (e.hasProperty ("channel"))
    {
        const juce::String ch = e.getProperty ("channel", "stereo").toString();
        echojay::parseBandChannel (ch.toRawUTF8(), s.channel);
    }

    if (e.hasProperty ("dynamic"))
    {
        const juce::var d = e.getProperty ("dynamic", juce::var());
        if (d.isObject())
        {
            s.dynamic     = true;
            s.thresholdDb = (float) (double) d.getProperty ("threshold_db", 0.0);
            s.rangeDb     = (float) (double) d.getProperty ("range_db",     0.0);
            s.attackMs    = (float) (double) d.getProperty ("attack_ms",   10.0);
            s.releaseMs   = (float) (double) d.getProperty ("release_ms", 100.0);
        }
    }
    return s;
}

juce::String SurgicalEqProcessor::applyEqBands (const juce::var& eqBandsArray,
                                                int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;

    if (! eqBandsArray.isArray())
        return "eq_bands: expected an array";

    auto* arr = eqBandsArray.getArray();
    const int n = arr != nullptr ? arr->size() : 0;
    if (n == 0) return "eq_bands: empty";

    std::vector<EqMove> moves;
    moves.reserve ((size_t) n);
    juce::StringArray descs;

    for (int i = 0; i < n; ++i)
    {
        bool disable = false, ok = true; int band = -1;
        const BandSpec s = specFromVar ((*arr)[i], disable, band, ok);
        if (! ok) continue;

        EqMove mv; mv.band = band; mv.disable = disable; mv.spec = s;
        moves.push_back (mv);

        if (disable)
            descs.add ("disable band " + juce::String (band));
        else
        {
            juce::String d = juce::String (echojay::bandTypeToString (s.type))
                           + " " + juce::String ((int) s.freqHz) + "Hz";
            if (s.type == BandType::Bell || s.type == BandType::LowShelf || s.type == BandType::HighShelf)
                d += " " + juce::String (s.gainDb, 1) + "dB";
            d += " Q" + juce::String (s.q, 2);
            if (s.dynamic) d += " dyn(" + juce::String (s.rangeDb, 1) + "dB@"
                                        + juce::String (s.thresholdDb, 0) + ")";
            if (s.channel != echojay::BandChannel::Stereo)
            {
                d += juce::String (" [") + echojay::bandChannelToString (s.channel) + "]";
                // The reported no-op, not a crash: on a mono stream a side- or
                // right-routed band has no lane to act on.
                if (getTotalNumInputChannels() < 2
                    && (s.channel == echojay::BandChannel::Side
                     || s.channel == echojay::BandChannel::Right))
                    d += " (mono input: no effect)";
            }
            descs.add (d);
        }
    }

    int skipped = 0, applied = 0;
    {
        const juce::ScopedLock sl (modelLock_);
        applied = echojay::applyEqMoves (bands_, kNumBands,
                                         moves.data(), (int) moves.size(), &skipped);
        pushToEngine();
    }

    if (appliedOut != nullptr) *appliedOut = applied;
    if (skippedOut != nullptr) *skippedOut = skipped;

    juce::String summary = "EQ: applied " + juce::String (applied) + " band(s) — "
                         + descs.joinIntoString ("; ");
    if (skipped > 0) summary += "  (" + juce::String (skipped) + " skipped: EQ full)";
    return summary;
}

// ---- device-global settings ------------------------------------------------
juce::String SurgicalEqProcessor::applyEqSettings (const juce::var& settings)
{
    if (! settings.isObject()) return {};

    juce::StringArray notes;

    // Merge semantics throughout: a key that isn't present leaves the current
    // value alone. That is what lets a turn flip auto_gain without having to
    // restate the output trim it doesn't care about.
    if (settings.hasProperty ("output_db"))
    {
        const float db = juce::jlimit (-24.0f, 24.0f,
                                       (float) (double) settings.getProperty ("output_db", 0.0));
        setOutputDb (db);
        notes.add ("output " + juce::String (db, 1) + " dB");
    }

    if (settings.hasProperty ("auto_gain"))
    {
        const bool on = (bool) settings.getProperty ("auto_gain", false);
        setAutoGain (on);
        notes.add (on ? "auto-gain on (" + juce::String (autoGainDbTarget(), 1) + " dB)"
                      : juce::String ("auto-gain off"));
    }

    if (settings.hasProperty ("phase_mode"))
    {
        const juce::String pm = settings.getProperty ("phase_mode", "zero")
                                        .toString().trim().toLowerCase();
        const bool linear = pm.startsWith ("lin") || pm == "1";
        setPhaseMode (linear ? PhaseMode::Linear : PhaseMode::Zero);
        notes.add (juce::String ("phase ") + (linear ? "linear" : "zero")
                   + (linear ? " (" + juce::String (
                        sampleRate_ > 0.0 ? engine_.latencySamples() * 1000.0 / sampleRate_
                                          : 0.0, 1) + " ms latency)"
                             : juce::String()));
    }

    if (settings.hasProperty ("ms_mode"))
    {
        const bool ms = (bool) settings.getProperty ("ms_mode", false);
        setMsMode (ms);
        notes.add (juce::String ("analyzer M/S view ") + (ms ? "on" : "off"));
    }

    if (notes.isEmpty()) return {};
    return "settings: " + notes.joinIntoString (", ");
}

// ---- eq_preset (P5) --------------------------------------------------------
juce::String SurgicalEqProcessor::applyEqPreset (const juce::String& name)
{
    const auto* p = echojay::findEqPreset (name.toRawUTF8());
    if (p == nullptr)
    {
        // An honest miss, WITH the menu — the model can immediately self-correct.
        juce::StringArray names;
        for (const auto& d : echojay::kEqPresets) names.add (d.name);
        return "preset \"" + name + "\" unknown (built-ins: "
             + names.joinIntoString (", ") + ")";
    }

    // A preset is a BASE: full replace of the band model, laid down by
    // explicit index. Explicit eq_bands in the same move merge on top of this
    // (funnel order), which is what makes "preset, then tweak" one move.
    {
        const juce::ScopedLock sl (modelLock_);
        for (int i = 0; i < kNumBands; ++i) bands_[i] = BandSpec {};
        for (int i = 0; i < p->numBands && i < kNumBands; ++i) bands_[i] = p->bands[i];
        pushToEngine();
    }
    setOutputDb (p->outputDb);
    setAutoGain (p->autoGain);

    return "preset \"" + juce::String (p->name) + "\" ("
         + juce::String (p->numBands) + " bands): " + p->blurb;
}

// ---- eq_action (P3) --------------------------------------------------------
juce::String SurgicalEqProcessor::applyEqAction (const juce::var& action,
                                                 int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;

    const juce::String type = action.isObject()
                                ? action.getProperty ("type", "").toString().trim()
                                : action.toString().trim();
    if (type.isEmpty()) return {};

    const juce::String norm = type.toLowerCase()
                                  .retainCharacters ("abcdefghijklmnopqrstuvwxyz");
    if (norm != "tameresonances" && norm != "huntresonances" && norm != "findresonances")
        return "action \"" + type + "\" is not one this EQ knows (have: tame_resonances)";

    // ---- parameters, all optional -----------------------------------------
    echojay::ResonanceHuntParams hp;
    int  sensIdx = 1;                                       // medium
    bool dynamic = true;
    if (action.isObject())
    {
        const juce::String sens = action.getProperty ("sensitivity", "medium")
                                        .toString().trim().toLowerCase();
        if      (sens.startsWith ("l")) sensIdx = 0;
        else if (sens.startsWith ("h")) sensIdx = 2;
        hp.marginDb = echojay::resonanceMarginForSensitivity (sensIdx);

        const juce::var range = action.getProperty ("range_hz", juce::var());
        if (range.isArray() && range.getArray()->size() >= 2)
        {
            hp.loHz = juce::jlimit (20.0f, 20000.0f,
                                    (float) (double) (*range.getArray())[0]);
            hp.hiHz = juce::jlimit (hp.loHz, 20000.0f,
                                    (float) (double) (*range.getArray())[1]);
        }
        hp.maxPeaks = juce::jlimit (1, 8, (int) action.getProperty ("max_bands", 4));
        dynamic     = (bool) action.getProperty ("dynamic", true);
    }
    static const char* kSensNames[] = { "low", "medium", "high" };

    // ---- capture: the newest ~2 s of PRE signal ---------------------------
    const int want = (int) juce::jmin ((double) kHuntRingSize,
                                       juce::jmax (sampleRate_, 1.0) * 2.0);
    std::vector<float> capture ((size_t) juce::jmax (want, 2048), 0.0f);
    const int got = readAnalysis (capture.data(), (int) capture.size(), false);

    double sumSq = 0.0;
    for (int i = 0; i < got; ++i) sumSq += (double) capture[(size_t) i] * capture[(size_t) i];
    const bool silent = got <= 0 || std::sqrt (sumSq / juce::jmax (got, 1)) < 1.0e-5;
    if (silent)
        return "hunt: no signal to analyse - play audio through the EQ, then run it again";

    echojay::ResonancePeak peaks[8];
    const int found = echojay::findResonances (capture.data(), got, sampleRate_,
                                               hp, peaks, 8);
    if (found == 0)
        return juce::String ("hunt: nothing stood out at ") + kSensNames[sensIdx]
             + " sensitivity in " + juce::String ((int) hp.loHz) + "-"
             + juce::String ((int) hp.hiHz) + " Hz";

    // ---- place the bands through the SAME merge as any move ---------------
    // Auto-allocate every one: a hunt proposes, it never overwrites a band
    // someone (human or model) already dialled.
    std::vector<EqMove> moves;
    juce::StringArray descs;
    for (int i = 0; i < found; ++i)
    {
        const auto& pk = peaks[i];
        EqMove mv;                                          // band = -1: auto
        BandSpec s;
        s.freqHz  = pk.freqHz;
        s.channel = echojay::BandChannel::Stereo;
        if (dynamic)
        {
            s.type        = BandType::Bell;
            s.gainDb      = 0.0f;                           // rests flat…
            s.q           = pk.q;
            s.dynamic     = true;                           // …ducks when it rings
            s.thresholdDb = juce::jlimit (-60.0f, 0.0f, pk.envelopeDb);
            s.rangeDb     = -juce::jlimit (1.0f, 12.0f, pk.prominenceDb);
            s.attackMs    = 3.0f;
            s.releaseMs   = 150.0f;
        }
        else
        {
            s.type = BandType::Notch;
            s.q    = pk.q;
        }
        mv.spec = s;
        moves.push_back (mv);

        char note[16];
        descs.add (juce::String ((int) pk.freqHz) + " Hz"
                 + (echojay::describeFreqAsNote (pk.freqHz, note, sizeof (note))
                        ? " (" + juce::String (note) + ")" : juce::String())
                 + " Q" + juce::String (pk.q, 1)
                 + (dynamic ? " dyn " + juce::String (-juce::jlimit (1.0f, 12.0f,
                                                       pk.prominenceDb), 1) + " dB"
                            : juce::String (" notch")));
    }

    int applied = 0, skipped = 0;
    {
        const juce::ScopedLock sl (modelLock_);
        applied = echojay::applyEqMoves (bands_, kNumBands,
                                         moves.data(), (int) moves.size(), &skipped);
        pushToEngine();
    }
    if (appliedOut != nullptr) *appliedOut = applied;
    if (skippedOut != nullptr) *skippedOut = skipped;

    juce::String summary = "hunt (" + juce::String (kSensNames[sensIdx])
                         + (dynamic ? ", dynamic" : ", static") + "): tamed "
                         + juce::String (applied) + " resonance(s) - "
                         + descs.joinIntoString ("; ");
    if (skipped > 0) summary += " (" + juce::String (skipped) + " skipped: EQ full)";
    return summary;
}

// ---- the one funnel --------------------------------------------------------
juce::String SurgicalEqProcessor::applyStructured (const juce::var& structured,
                                                   ParamSource src,
                                                   int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;

    // Legacy shape: a bare array IS eq_bands. Unchanged behaviour, forever.
    if (structured.isArray())
        return applyEqBands (structured, appliedOut, skippedOut);

    if (! structured.isObject()) return {};

    juce::StringArray parts;

    // Order is load-bearing (SURGICAL_EQ_ENHANCEMENTS §0): a preset lays the
    // foundation, explicit settings and bands override it, and an action runs
    // last so it operates on the finished state rather than a half-built one.
    if (structured.hasProperty ("eq_preset"))
    {
        const juce::String name = structured.getProperty ("eq_preset", "").toString().trim();
        if (name.isNotEmpty())
        {
            const auto p = applyEqPreset (name);
            if (p.isNotEmpty()) parts.add (p);
        }
    }

    if (structured.hasProperty ("eq_settings"))
    {
        const auto s = applyEqSettings (structured.getProperty ("eq_settings", juce::var()));
        if (s.isNotEmpty()) parts.add (s);
    }

    // The UNIVERSAL flat path (BUILTIN_SUITE_PLAN.md §3). Resolved alongside
    // eq_settings because it addresses the same device-global knobs by their
    // schema ids — a backend that has been generalised to `params` for the other
    // 18 devices must not have to special-case the EQ to trim its output.
    //
    // eq_settings is NOT deprecated by this: it carries keys (phase_mode) that a
    // numeric schema cannot express, and every already-deployed move uses it.
    if (structured.hasProperty ("params"))
    {
        const auto p = applyParams (structured.getProperty ("params", juce::var()), src);
        if (p.isNotEmpty()) parts.add (p);
    }

    if (structured.hasProperty ("eq_bands"))
    {
        const juce::var bands = structured.getProperty ("eq_bands", juce::var());
        if (bands.isArray())
        {
            const auto b = applyEqBands (bands, appliedOut, skippedOut);
            if (b.isNotEmpty()) parts.add (b);
        }
    }

    if (structured.hasProperty ("eq_action"))
    {
        // Runs LAST by design: an action operates on the finished state, so a
        // hunt in the same move as a preset tames what the preset left.
        int aApplied = 0, aSkipped = 0;
        const auto a = applyEqAction (structured.getProperty ("eq_action", juce::var()),
                                      &aApplied, &aSkipped);
        if (a.isNotEmpty()) parts.add (a);
        if (appliedOut != nullptr) *appliedOut += aApplied;
        if (skippedOut != nullptr) *skippedOut += aSkipped;
    }

    if (parts.isEmpty()) return {};
    return parts.joinIntoString ("; ");
}

juce::var SurgicalEqProcessor::currentEqSettingsVar() const
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("output_db", (double) getOutputDb());
    o->setProperty ("auto_gain", getAutoGain());
    o->setProperty ("phase_mode", getPhaseMode() == PhaseMode::Linear ? "linear" : "zero");
    o->setProperty ("ms_mode", msMode_);
    return juce::var (o.get());
}

juce::var SurgicalEqProcessor::currentEqBandsVar() const
{
    juce::Array<juce::var> out;
    const juce::ScopedLock sl (modelLock_);
    for (int i = 0; i < kNumBands; ++i)
    {
        const BandSpec& s = bands_[i];
        if (! s.enabled) continue;

        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("band", i + 1);                  // 1-based, for exact restore
        o->setProperty ("type", echojay::bandTypeToString (s.type));
        o->setProperty ("freq_hz", (double) s.freqHz);
        o->setProperty ("gain_db", (double) s.gainDb);
        o->setProperty ("q", (double) s.q);
        o->setProperty ("slope_db_oct", s.slopeDbPerOct);
        // Absent means stereo — a v2-or-older state (and any move that never
        // routed) reads back byte-identical to what it always was.
        if (s.channel != echojay::BandChannel::Stereo)
            o->setProperty ("channel", echojay::bandChannelToString (s.channel));
        if (s.dynamic)
        {
            juce::DynamicObject::Ptr d = new juce::DynamicObject();
            d->setProperty ("threshold_db", (double) s.thresholdDb);
            d->setProperty ("range_db",     (double) s.rangeDb);
            d->setProperty ("attack_ms",    (double) s.attackMs);
            d->setProperty ("release_ms",   (double) s.releaseMs);
            o->setProperty ("dynamic", juce::var (d.get()));
        }
        out.add (juce::var (o.get()));
    }
    return out;
}

// ---- state ----------------------------------------------------------------
void SurgicalEqProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    // v3 adds the per-band `channel` field (P2) and live phase_mode DSP (P4).
    // v2 (settings, no channel) and v1 (bands only) still read — absent keys
    // land on defaults; the version is here to make the history explicit
    // rather than inferred from missing keys.
    root->setProperty ("v", 3);
    root->setProperty ("bypassed", bypassed_.load());
    root->setProperty ("eq_bands", currentEqBandsVar());
    root->setProperty ("eq_settings", currentEqSettingsVar());

    const juce::String json = juce::JSON::toString (juce::var (root.get()), true);
    juce::MemoryOutputStream mos (dest, false);
    mos.writeText (json, false, false, nullptr);
}

void SurgicalEqProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const juce::String json = juce::String::createStringFromData (data, sizeInBytes);
    const juce::var parsed = juce::JSON::parse (json);
    if (! parsed.isObject()) return;

    setBypassed ((bool) parsed.getProperty ("bypassed", false));

    // Settings are a full replace on restore, not a merge: a saved state is
    // the whole device, and a v1 file (no eq_settings) must load as DEFAULTS
    // rather than inheriting whatever the previous instance happened to have.
    setOutputDb (0.0f);
    setAutoGain (false);
    setPhaseMode (PhaseMode::Zero);
    setMsMode (false);
    applyEqSettings (parsed.getProperty ("eq_settings", juce::var()));

    // full replace: clear model, then apply the saved bands by explicit index
    {
        const juce::ScopedLock sl (modelLock_);
        for (int i = 0; i < kNumBands; ++i) bands_[i] = BandSpec {};
        pushToEngine();     // the cleared model is what auto-gain must integrate
    }
    applyEqBands (parsed.getProperty ("eq_bands", juce::var()));
}

// ---------------------------------------------------------------------------
// registration — the EQ is a registry entry, not a special case
// ---------------------------------------------------------------------------
// Identifier and uid are the ones the EQ has always used. They are written into
// saved chain XML, so changing them would orphan every session that already has
// an EchoJay EQ in its chain.
namespace
{
    BuiltinDevice makeSurgicalEqDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay EQ";
        d.category        = "EQ";
        d.descriptiveName = "EchoJay surgical EQ (built in)";
        d.summary         = "EchoJay's own fully-parametric EQ, dialled to EXACT values "
                            "rather than approximated. Prefer it for surgical moves: "
                            "specific-frequency cuts and boosts, high-pass/low-pass "
                            "cleanup, notching resonances, and dynamic de-essing. "
                            "Bands take an optional channel (stereo|mid|side|left|right) "
                            "for M/S or per-side moves, and a musical note (\"G5\") in "
                            "place of freq_hz. eq_action tame_resonances hunts and tames "
                            "resonant peaks from the live signal; eq_preset loads a named "
                            "starting point.";
        d.identifier      = "echojay:builtin:eq";
        d.uid             = 0x456A4551;   // 'EjEQ' — frozen, matched on restore
        d.aliases         = { "EchoJayEQ", "EchoJay Surgical EQ" };
        d.schema          = SurgicalEqProcessor::schema();
        d.create          = [] { return std::make_unique<SurgicalEqProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar surgicalEqRegistrar { makeSurgicalEqDevice() };
}
