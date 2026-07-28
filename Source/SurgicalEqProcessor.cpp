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

    // Not RT: clear the tap so a restart never shows the previous session's
    // audio decaying out of the analyzer.
    analysisRing_.fill (0.0f);
    analysisWrite_.store (0, std::memory_order_release);
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

    // BEFORE the engine: the analyzer shows the input, which is what you are
    // EQing against. Taking it after would show the result of the curve and
    // make every cut look like it had already been made.
    pushAnalysis (buffer);

    // engine handles bypass/solo internally; passthrough is a no-op copy-free path
    engine_.process (buffer.getArrayOfWritePointers(),
                     buffer.getNumChannels(), buffer.getNumSamples());
}

void SurgicalEqProcessor::pushAnalysis (const juce::AudioBuffer<float>& buffer) noexcept
{
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n     = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0) return;

    const uint32_t w = analysisWrite_.load (std::memory_order_relaxed);

    if (numCh == 1)
    {
        const float* src = buffer.getReadPointer (0);
        for (int i = 0; i < n; ++i)
            analysisRing_[(size_t) ((w + (uint32_t) i) & kAnalysisRingMask)] = src[i];
    }
    else
    {
        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getReadPointer (1);
        for (int i = 0; i < n; ++i)
            analysisRing_[(size_t) ((w + (uint32_t) i) & kAnalysisRingMask)]
                = 0.5f * (l[i] + r[i]);
    }

    // Release: the samples above must be visible before the index that claims
    // they exist. Unsigned wraparound of the index is well-defined and the
    // mask makes it the correct ring position regardless.
    analysisWrite_.store (w + (uint32_t) n, std::memory_order_release);
}

int SurgicalEqProcessor::readAnalysis (float* dest, int maxSamples) const noexcept
{
    if (dest == nullptr || maxSamples <= 0) return 0;

    const int      n     = juce::jmin (maxSamples, kAnalysisRingSize);
    const uint32_t w     = analysisWrite_.load (std::memory_order_acquire);
    const uint32_t start = w - (uint32_t) n;      // newest n samples

    for (int i = 0; i < n; ++i)
        dest[i] = analysisRing_[(size_t) ((start + (uint32_t) i) & kAnalysisRingMask)];

    return n;
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
    root->setProperty ("v", 1);
    root->setProperty ("bypassed", bypassed_.load());
    root->setProperty ("eq_bands", currentEqBandsVar());

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

    // full replace: clear model, then apply the saved bands by explicit index
    {
        const juce::ScopedLock sl (modelLock_);
        for (int i = 0; i < kNumBands; ++i) bands_[i] = BandSpec {};
    }
    applyEqBands (parsed.getProperty ("eq_bands", juce::var()));
}
