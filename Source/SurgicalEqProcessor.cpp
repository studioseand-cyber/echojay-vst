/*
    SurgicalEqProcessor.cpp  —  see SurgicalEqProcessor.h.

    Not compiled in the Cowork sandbox (no JUCE there); built in the normal
    EchoJay project. Written against the JUCE EchoJay already links.
*/

#include "SurgicalEqProcessor.h"
#include "SurgicalEqEditor.h"

#include <type_traits>

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
    : juce::AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // bands_ default to BandSpec{} (all disabled) — a clean, transparent EQ.
}

// ---- audio ----------------------------------------------------------------
void SurgicalEqProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    blockSize_  = samplesPerBlock;
    engine_.prepare (sampleRate, samplesPerBlock, 2);
    pushToEngine();

    // Not RT: clear the taps so a restart never shows the previous session's
    // audio decaying out of the analyzer.
    preRing_.clear();
    postRing_.clear();
}

bool SurgicalEqProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;   // in must match out
}

void SurgicalEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Pre tap: the signal arriving, which is what you are EQing against.
    preRing_.push (buffer);

    // engine handles bypass/solo internally; passthrough is a no-op copy-free path
    engine_.process (buffer.getArrayOfWritePointers(),
                     buffer.getNumChannels(), buffer.getNumSamples());

    // Post tap: the same block after the curve, so the analyzer can show what
    // the EQ actually did rather than only what went in.
    postRing_.push (buffer);
}

// ---- analysis rings -------------------------------------------------------
void SurgicalEqProcessor::AnalysisRing::push (const juce::AudioBuffer<float>& buffer) noexcept
{
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n     = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0) return;

    const uint32_t w = write.load (std::memory_order_relaxed);

    if (numCh == 1)
    {
        const float* src = buffer.getReadPointer (0);
        for (int i = 0; i < n; ++i)
            data[(size_t) ((w + (uint32_t) i) & kAnalysisRingMask)] = src[i];
    }
    else
    {
        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getReadPointer (1);
        for (int i = 0; i < n; ++i)
            data[(size_t) ((w + (uint32_t) i) & kAnalysisRingMask)] = 0.5f * (l[i] + r[i]);
    }

    // Release: the samples above must be visible before the index that claims
    // they exist. Unsigned wraparound of the index is well-defined and the
    // mask makes it the correct ring position regardless.
    write.store (w + (uint32_t) n, std::memory_order_release);
}

int SurgicalEqProcessor::AnalysisRing::read (float* dest, int maxSamples) const noexcept
{
    if (dest == nullptr || maxSamples <= 0) return 0;

    const int      n     = juce::jmin (maxSamples, kAnalysisRingSize);
    const uint32_t w     = write.load (std::memory_order_acquire);
    const uint32_t start = w - (uint32_t) n;      // newest n samples

    for (int i = 0; i < n; ++i)
        dest[i] = data[(size_t) ((start + (uint32_t) i) & kAnalysisRingMask)];

    return n;
}

void SurgicalEqProcessor::AnalysisRing::clear() noexcept
{
    data.fill (0.0f);
    write.store (0, std::memory_order_release);
}

int SurgicalEqProcessor::readAnalysis (float* dest, int maxSamples, bool postEq) const noexcept
{
    return postEq ? postRing_.read (dest, maxSamples)
                  : preRing_.read  (dest, maxSamples);
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
    s.gainDb = (float) (double) e.getProperty ("gain_db", (double) s.gainDb);
    s.q      = (float) (double) e.getProperty ("q",       (double) s.q);
    s.slopeDbPerOct = (int) e.getProperty ("slope_db_oct", s.slopeDbPerOct);
    s.enabled = (bool) e.getProperty ("enabled", true);  // a set enables by default

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

    // phase_mode / ms_mode are parsed and stored here but not acted on until
    // P4 / P2. Storing them from P1 keeps the schema stable: the backend can
    // emit them as soon as they are documented without the client dropping the
    // key on the floor, and state files do not change shape when the DSP lands.
    if (settings.hasProperty ("phase_mode"))
    {
        const juce::String pm = settings.getProperty ("phase_mode", "zero")
                                        .toString().trim().toLowerCase();
        const bool linear = pm.startsWith ("lin");
        setPhaseMode (linear ? PhaseMode::Linear : PhaseMode::Zero);
        notes.add (juce::String ("phase ") + (linear ? "linear" : "zero")
                   + (linear ? " (stored; DSP lands in P4)" : ""));
    }

    if (settings.hasProperty ("ms_mode"))
    {
        const bool ms = (bool) settings.getProperty ("ms_mode", false);
        setMsMode (ms);
        notes.add (juce::String ("M/S view ") + (ms ? "on (stored; DSP lands in P2)" : "off"));
    }

    if (notes.isEmpty()) return {};
    return "settings: " + notes.joinIntoString (", ");
}

// ---- the one funnel --------------------------------------------------------
juce::String SurgicalEqProcessor::applyStructured (const juce::var& structured,
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
        // P5. Named so a move carrying one is reported honestly instead of
        // being silently ignored — a preset that quietly does nothing is worse
        // than one that says it isn't here yet.
        const juce::String name = structured.getProperty ("eq_preset", "").toString().trim();
        if (name.isNotEmpty())
            parts.add ("preset \"" + name + "\" not available yet (phase 5)");
    }

    if (structured.hasProperty ("eq_settings"))
    {
        const auto s = applyEqSettings (structured.getProperty ("eq_settings", juce::var()));
        if (s.isNotEmpty()) parts.add (s);
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
        // P3. Same honesty rule as eq_preset.
        const juce::var a = structured.getProperty ("eq_action", juce::var());
        const juce::String type = a.isObject() ? a.getProperty ("type", "").toString().trim()
                                               : a.toString().trim();
        if (type.isNotEmpty())
            parts.add ("action \"" + type + "\" not available yet (phase 3)");
    }

    if (parts.isEmpty()) return {};
    return parts.joinIntoString ("; ");
}

juce::var SurgicalEqProcessor::currentEqSettingsVar() const
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("output_db", (double) getOutputDb());
    o->setProperty ("auto_gain", getAutoGain());
    o->setProperty ("phase_mode", phaseMode_ == PhaseMode::Linear ? "linear" : "zero");
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
    // v2 adds the eq_settings sibling. v1 (bands only) still reads — see
    // setStateInformation; the version is here to make that explicit rather
    // than inferred from a missing key.
    root->setProperty ("v", 2);
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
