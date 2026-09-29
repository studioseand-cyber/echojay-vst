// loudness_loop_guard v2 (18e, 19 Sep 2026) on the REAL objects: EchoJayProcessor -> ChainHost -> [EchoJay Level, <limiter>],
// the real chain tallies (LUFS, max short-term, 4x true peak) and the real LoudnessLoop.
//   The loop drives the LEVEL slot's gain_db, never a limiter param; measures the loudest 3 s across a 10 s counted
//   window; asks before applying (nothing moves until "go"); refuses a quiet window; keeps tracking after applying and
//   proposes a back-off (never applies) when a later section runs > 1 dB over; the verbs are deterministic; the GR text
//   reads "working X dB average, up to Y dB on the hits"; arm-after-exact-apply stays GREEN.
// RED on 18c/18d (EJ_LIB/EJ_SRC_ROOT = the pre-round lib + headers): no EJ_LOUDNESSLOOP_V2, every v2 leg reports FAIL.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"   // 21t-l item 5: editCarriesAdd
#include "EedLimiterProcessor.h"   // force-link the built-in's registrar
#include "EedDeviceRegistry.h"
#include "EchoJayFileLog.h"
#include "EchoJayReadingGate.h"
#include "EJCalibLoop.h"   // 21t-d: the calibration loop under test
#include "EJLevelRecord.h" // 21t-i: the stored level record under test
#include "EJReadbackSearch.h" // 21t-j: parseDisplayDb, the product's own parse, used by the cross-check legs
#include <cstdio>
#include <memory>
#include <cmath>
#include <deque>
#ifdef EJ_LOUDNESSLOOP_V2
#include "EedLevelProcessor.h"
#include "EedGainProcessor.h"   // 21m ruling 2: the +4 dB stand-in
#endif
struct EchoJayAPIRequestPin { static juce::String body (EchoJayAPI& a, const juce::StringArray& r, const juce::StringArray& c, const juce::String& sys, const juce::String& mb) { return a.buildChatRequestBody (r, c, sys, mb); } };   // 21m unityChain leg
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); }
    static void applyExact (ChainHost& h, int i) { h.applyStructuredIfReady (i, ChainHost::DialTrigger::settingsAttached); }
#ifdef EJ_LOUDNESSLOOP_MANNERS
    // 18g: the slot's dial READBACK says its ceiling applied (what a mapped bx_limiter reports after a build)
    static void markCeilingApplied (ChainHost& h, int i) { auto& s = h.slots_[(size_t) i]; s.dialApplied = juce::StringArray { "Ceiling" }; s.dialAppliedCount = 1; s.dialStatus = ChainHost::DialStatus::applied; }
#endif
};
namespace {
int measuredLines (const juce::StringArray& logs) { int n = 0; for (const auto& l : logs) if (l.startsWith ("EJLoudness: measured:")) ++n; return n; }
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::String f1 (float v) { return juce::String (v, 2); }

// ---- an INDEPENDENT meter (not the tally): BS.1770 K-weighting biquads at 48 kHz, 400 ms blocks with 75 % overlap,
// -70 abs / -10 rel gating, stereo power sum; true peak by a 4x linear-phase sinc interpolator of its own (24 taps).
struct IndependentMeter
{
    double b0a = 1.53512485958697, b1a = -2.69169618940638, b2a = 1.19839281085285, a1a = -1.69065929318241, a2a = 0.73248077421585;
    double b0b = 1.0, b1b = -2.0, b2b = 1.0, a1b = -1.99004745483398, a2b = 0.99007225036621;
    struct BQ { double z1 = 0, z2 = 0; double run (double x, double b0, double b1, double b2, double a1, double a2) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    BQ l1, l2, r1, r2;
    std::vector<double> hop; double acc = 0; int accN = 0; const int hopN = 4800; std::deque<double> ring; std::vector<double> blocks;
    float tpMax = 0; std::vector<float> hl, hr; int hpos = 0; float coef[4][24];
    IndependentMeter() { hl.assign (24, 0.0f); hr.assign (24, 0.0f);
        for (int ph = 0; ph < 4; ++ph) { double sum = 0; for (int k = 0; k < 24; ++k) { const double x = (k - 11.5) - ph / 4.0 + 0.5; const double sinc = x == 0 ? 1 : std::sin (M_PI * x) / (M_PI * x); const double w = 0.42 - 0.5 * std::cos (2 * M_PI * (k + 0.5) / 24) + 0.08 * std::cos (4 * M_PI * (k + 0.5) / 24); coef[ph][k] = (float) (sinc * w); sum += coef[ph][k]; } for (int k = 0; k < 24; ++k) coef[ph][k] /= (float) sum; } }
    void reset() { hop.clear(); acc = 0; accN = 0; ring.clear(); blocks.clear(); tpMax = 0; l1 = l2 = r1 = r2 = BQ(); }
    void push (const float* L, const float* R, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const double kl = l2.run (l1.run (L[i], b0a, b1a, b2a, a1a, a2a), b0b, b1b, b2b, a1b, a2b);
            const double kr = r2.run (r1.run (R[i], b0a, b1a, b2a, a1a, a2a), b0b, b1b, b2b, a1b, a2b);
            acc += kl * kl + kr * kr; ++accN;
            if (accN == hopN) { ring.push_back (acc / hopN); if (ring.size() > 4) ring.pop_front(); if (ring.size() == 4) { double m = 0; for (auto v : ring) m += v; blocks.push_back (m / 4.0); } acc = 0; accN = 0; }
            hl[(size_t) hpos] = L[i]; hr[(size_t) hpos] = R[i];
            for (int ph = 0; ph < 4; ++ph) { float al = 0, ar = 0; int idx = hpos; for (int k = 0; k < 24; ++k) { al += coef[ph][k] * hl[(size_t) idx]; ar += coef[ph][k] * hr[(size_t) idx]; idx = idx == 0 ? 23 : idx - 1; } tpMax = std::max (tpMax, std::max (std::abs (al), std::abs (ar))); }
            hpos = (hpos + 1) % 24;
        }
    }
    float lufs() const
    {
        std::vector<double> g; for (auto p : blocks) { const double l = -0.691 + 10 * std::log10 (std::max (p, 1e-20)); if (l > -70) g.push_back (p); }
        if (g.empty()) return -200;
        double m = 0; for (auto p : g) m += p; m /= g.size(); const double rel = -0.691 + 10 * std::log10 (m) - 10;
        double m2 = 0; int n2 = 0; for (auto p : g) { const double l = -0.691 + 10 * std::log10 (p); if (l > rel) { m2 += p; ++n2; } }
        return n2 ? (float) (-0.691 + 10 * std::log10 (m2 / n2)) : -200;
    }
    float truePeakDb() const { return tpMax > 0 ? 20 * std::log10 (tpMax) : -200; }
};

struct Programme { juce::Random rng { 4242 }; float amp = 0.1f; bool peaky = false; int blockCount = 0; float burst = 5.0f; int spikeAtBlock = -1; float spike = 20.0f; int spikeEvery = 0; };   // spikeEvery: a rare transient once every N blocks (0 = off)   // spike: ONE block's hit at spike x (a single ~+26 dB transient)   // burst: the hit's gain over the bed (5x = +14 dB); K1 uses 12x
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } }
void feed (EchoJayProcessor& p, Programme& prog, int blocks, bool silent, LoudnessLoop* loop, IndependentMeter* ind, float gainDb = 0.0f)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    const float g = std::pow (10.0f, gainDb / 20.0f);
    for (int b = 0; b < blocks; ++b)
    {
        const bool burst = prog.peaky && (prog.blockCount % 12) == 0;   // a drum hit every ~128 ms: 3 ms of noise at +14 dB
        for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = silent ? 0.0f : (prog.rng.nextFloat() * 2.0f - 1.0f) * prog.amp * g * ((burst && i < 144) ? ((prog.blockCount == prog.spikeAtBlock || (prog.spikeEvery > 0 && prog.blockCount % prog.spikeEvery == 0)) ? prog.spike : prog.burst) : 1.0f); }
        ++prog.blockCount;
        p.processBlock (buf, midi);
        if (ind != nullptr) ind->push (buf.getReadPointer (0), buf.getReadPointer (1), 512);
        if (loop != nullptr && (b % 23) == 22) loop->tickNow();   // ~250 ms of audio per tick, as the plugin's timer would
    }
}
float calibrate (EchoJayProcessor& p, Programme& prog, float wantLufs)
{
    p.getChainHost().resetAllLevels(); feed (p, prog, 600, false, nullptr, nullptr);
    const float have = p.getChainHost().getChainInLevels().levelDb;
    prog.amp *= std::pow (10.0f, (wantLufs - have) / 20.0f);
    p.getChainHost().resetAllLevels(); feed (p, prog, 600, false, nullptr, nullptr);
    return p.getChainHost().getChainInLevels().levelDb;
}
float numberAfter (const juce::String& text, const juce::String& key)
{
    const int i = text.indexOf (key); if (i < 0) return std::numeric_limits<float>::quiet_NaN();
    return text.substring (i + key.length()).trim().initialSectionContainingOnly ("-0123456789.+").getFloatValue();
}
#ifdef EJ_LOUDNESSLOOP_V2
// A stand-in for a THIRD-PARTY limiter sitting last: a hard clipper at -0.5 dBFS that is not an EchoJay device.
struct EJTestLimiter final : juce::AudioProcessor
{
    EJTestLimiter() : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true).withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Test Limiter"; }
    void prepareToPlay (double, int) override {} void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override { const float c = 0.944f; for (int ch = 0; ch < b.getNumChannels(); ++ch) { auto* d = b.getWritePointer (ch); for (int i = 0; i < b.getNumSamples(); ++i) d[i] = juce::jlimit (-c, c, d[i]); } }
    double getTailLengthSeconds() const override { return 0.0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
};
struct EJTestSoftLimiter final : juce::AudioProcessor
{
    EJTestSoftLimiter() : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true).withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Test Soft Limiter"; }
    void prepareToPlay (double, int) override {} void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override { for (int ch = 0; ch < b.getNumChannels(); ++ch) { auto* d = b.getWritePointer (ch); for (int i = 0; i < b.getNumSamples(); ++i) { const float x = d[i]; d[i] = (x < 0 ? -1.0f : 1.0f) * std::pow (std::abs (x), 0.6f); } } }
    double getTailLengthSeconds() const override { return 0.0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
};
// 21t-d: a compressor whose GAIN REDUCTION RISES WITH INPUT above a fixed knee - the one property the
// calibration loop depends on. Above the knee it applies (over-knee / ratio) of reduction, so driving it harder
// makes it work harder, which is what the drive steps are for. Linear, memoryless, no ballistics: the loop is
// being tested, not a compressor design.
struct EJTestCompressor final : juce::AudioProcessor
{
    EJTestCompressor() : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true).withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Test Compressor"; }
    void prepareToPlay (double, int) override {} void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        // The knee sits just below the fixture's programme level, so the band (2-3 dB of GR) is reached with a
        // couple of dB of drive - which is what a real compressor set near its threshold does, and what makes
        // the step budget a meaningful test rather than an arithmetic impossibility.
        const float kneeDb = -27.0f, ratio = 4.0f;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float a = std::abs (d[i]);
                if (a <= 1.0e-7f) continue;
                const float db = juce::Decibels::gainToDecibels (a);
                if (db <= kneeDb) continue;
                const float over = db - kneeDb;
                const float outDb = kneeDb + over / ratio;
                d[i] *= juce::Decibels::decibelsToGain (outDb - db);
            }
        }
    }
    double getTailLengthSeconds() const override { return 0.0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
};
struct EJTestBypass final : juce::AudioProcessor
{
    EJTestBypass() : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true).withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Test Bypass"; }
    void prepareToPlay (double, int) override {} void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
};
BuiltinDevice makeTestBypass() { BuiltinDevice d; d.name = "EJ Test Bypass"; d.category = "Dynamics"; d.descriptiveName = d.name; d.summary = "harness stand-in: identity (the pre-limiter truth)"; d.identifier = "echojay:test:bypass"; d.uid = 0x454A5442; d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new EJTestBypass()); }; return d; }
const BuiltinDeviceRegistrar testBypassReg { makeTestBypass() };
BuiltinDevice makeTestSoftLimiter() { BuiltinDevice d; d.name = "EJ Test Soft Limiter"; d.category = "Dynamics"; d.descriptiveName = d.name; d.summary = "harness stand-in: 0.6 dB out per dB in"; d.identifier = "echojay:test:softlimiter"; d.uid = 0x454A5453; d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new EJTestSoftLimiter()); }; return d; }
const BuiltinDeviceRegistrar testSoftLimReg { makeTestSoftLimiter() };
BuiltinDevice makeTestLimiter() { BuiltinDevice d; d.name = "EJ Test Limiter"; d.category = "Dynamics"; d.descriptiveName = d.name; d.summary = "harness stand-in for a third-party limiter"; d.identifier = "echojay:test:limiter"; d.uid = 0x454A544C; d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new EJTestLimiter()); }; return d; }
const BuiltinDeviceRegistrar testLimReg { makeTestLimiter() };

struct Rig
{
    EchoJayProcessor proc; ChainHost& h; LoudnessLoop& loop; juce::StringArray bubbles, logs; Programme prog;
    int levelSlot = -1, limSlot = -1;
    // 18g: ceilingReadBack = the third-party limiter's dial readback confirms its ceiling (a mapped limiter after a build);
    // false = no readback (no map), which the safety net substitutes. limiterName picks the stand-in.
    // 21m ruling 2: gainSlot = an "EchoJay Gain" between the Level and the limiter (the +4 dB stand-in for a plugin that adds level)
    Rig (bool thirdPartyLast, bool ceilingReadBack = true, const char* limiterName = "EJ Test Limiter", bool gainSlot = false) : h (proc.getChainHost()), loop (proc.loudnessLoop())
    {
        proc.prepareToPlay (48000.0, 512);
        const auto* lv = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
        const auto* lm = BuiltinDeviceRegistry::instance().findByName (thirdPartyLast ? limiterName : "EchoJay Limiter");
        check (lv != nullptr && lm != nullptr, "precondition: EchoJay Level and the limiter are registered");
        EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*lv));
        if (gainSlot)
        {
            const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
            check (gn != nullptr, "precondition: EchoJay Gain is registered"); if (gn == nullptr) return;
            EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*gn));
        }
        EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*lm));
        levelSlot = 0; limSlot = gainSlot ? 2 : 1;
#ifdef EJ_LOUDNESSLOOP_MANNERS
        if (thirdPartyLast && ceilingReadBack) EchoJayBorrowHostTestAccess::markCeilingApplied (h, limSlot);
#else
        juce::ignoreUnused (ceilingReadBack);
#endif
        loop.onBubble = [this] (const LoudnessLoop::Bubble& b) { if (b.text.startsWith ("Listening") || b.text.startsWith ("Checking...")) return; bubbles.add (b.text); };
        loop.logLine = [this] (const juce::String& l) { logs.add (l); };
        loop.isPlaying = [] { return true; };
    }
    void setTarget (float t, double limiterInputDb = 0.0)
    {
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 0.0); pp->setProperty ("target_lufs", (double) t); pp->setProperty ("loudness_option", 0);
          auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (levelSlot, juce::var (w)); }
        if (h.getSlotInfo (limSlot).name == "EchoJay Limiter")
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("input_db", limiterInputDb); pp->setProperty ("ceiling_db", -0.1); pp->setProperty ("true_peak", 1);
          auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (limSlot, juce::var (w)); }
    }
    float levelGain() const { return (float) dynamic_cast<EedLevelProcessor*> (h.getSlotProcessor (levelSlot))->gainDb(); }
    void setGainDb (float db)   // the Gain stand-in (slot 1 when the Rig has one), through the schema path
    { auto* pp = new juce::DynamicObject(); pp->setProperty ("level_db", (double) db); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (1, juce::var (w)); }
    double limiterInput() const { auto* l = dynamic_cast<EedLimiterProcessor*> (h.getSlotProcessor (limSlot)); return l ? l->inputDb() : 0.0; }
    juce::String last() const { return bubbles.isEmpty() ? juce::String() : bubbles[bubbles.size() - 1]; }
    // feed until the loop leaves waitAudio/measuring (a proposal, a hold or a rejection), bounded
    void runWindow (float gainDb = 0.0f, IndependentMeter* ind = nullptr) {
#ifdef EJ_LOUDNESSLOOP_MANNERS
        if (loop.state() == LoudnessLoop::State::armed) loop.listen();   // 18g: the window starts on Listen
#endif
        for (int k = 0; k < 16 && (loop.state() == LoudnessLoop::State::waitAudio || loop.state() == LoudnessLoop::State::measuring); ++k) feed (proc, prog, 100, false, &loop, ind, gainDb); }
    // after applying the loop TRACKS: feed while it tracks (bounded), so a louder section can raise a back-off proposal
    void runTracking (float gainDb, int rounds = 16) { for (int k = 0; k < rounds && loop.state() == LoudnessLoop::State::tracking; ++k) feed (proc, prog, 100, false, &loop, nullptr, gainDb); }
};
#endif
} // namespace

// ---- RUNNING THIS GUARD UNDER AddressSanitizer (21r, 24 Sep 2026) ------------------------------------------
// ASan puts a redzone around every stack object, and this guard holds several EchoJayProcessors in one frame, so
// it overflows the 8 MB main stack before it asserts anything ("stack-overflow ... in main"). The work therefore
// runs on a thread with room when - and only when - the binary is sanitized. NOT ONE ASSERTION CHANGES: the
// ordinary build calls guardMain() directly, exactly as before.
#if defined(__has_feature)
 #if __has_feature(address_sanitizer)
  #define EJ_UNDER_ASAN 1
 #endif
#endif
#if defined(EJ_UNDER_ASAN)
 #include <pthread.h>
#endif

static int guardMain()
{
    (void) EedGainProcessor::schema();   // the static archive links the Gain registrar only when referenced (as level_slot_guard)
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_loudloop_" + juce::String (juce::Time::getMillisecondCounter()));
    std::printf ("loudness_loop_guard v2 (18e): the Level slot, max short-term, ask-before-apply, the verbs, tracking\n");
    (void) EedLimiterProcessor::schema();   // reference the limiter so the static archive links its registrar object on every build (the v2 legs reference it only under EJ_LOUDNESSLOOP_V2)
    const auto* dev = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
    check (dev != nullptr, "precondition: EchoJay Limiter is registered");
    if (! dev) return 2;
#ifndef EJ_LOUDNESSLOOP_V2
    for (const char* leg : { "A. ask before applying: nothing moves until go", "A. the loop drives the Level slot, the limiter's input_db is untouched", "A. after go the second window lands within +-1 dB and the loop tracks",
                             "B. quiet section: the window is refused, nothing applied", "C. chorus after verse: a back-off is PROPOSED, never applied", "D. third-party limiter last: the target is reached through the Level slot",
                             "E. verbs: push it / a bit louder / a bit softer / check the level again / undo / leave it", "F. GR text: working X dB average, up to Y dB on the hits", "G. arm after the exact built-in apply (18d) stays GREEN",
                             "H. peaky programme: output true peak <= -0.1 dBTP by the independent meter", "I. the live-shaped chain ARMS (the loop inserts the Level slot it needs; RED as it stood: not armed, the insertion lived in the editor)",
                             "A. the proposal bubble carries [Go] [Leave it]", "B. quiet section: the loop ASKS (state quietAsked), nothing applied until a pill", "C. the back-off bubble carries [Back off] [Leave it]", "E. the result bubble (on target) carries [Undo] [A bit louder] [A bit softer] [Push it]" })
        check (false, leg, "no LoudnessLoop v2 on this build (18c/18d)");
#else
    std::printf ("== A. ask before applying: -18 programme, target -9, Level 0 dB ==\n");
    {
        Rig r (false); r.setTarget (-9.0f, 0.0);
        const float cal = calibrate (r.proc, r.prog, -18.0f); check (std::abs (cal + 18.0f) < 0.6f, "programme calibrated at the chain input to -18 LUFS", f1 (cal));
        check (r.loop.armFromChain(), "armed from the Level slot's params", r.logs.joinIntoString (" | ").substring (0, 200));
        check (r.loop.armSource() == "level_params" && r.loop.levelSlot() == 0 && r.loop.limiterSlot() == 1, "arm source level_params, Level slot 0, limiter slot 1", r.loop.armSource());
        r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::proposed, "after the window the loop PROPOSES (state proposed)", juce::String ((int) r.loop.state()));
        check (std::abs (r.levelGain()) < 0.01f, "A. ask before applying: nothing moves until go", "Level gain " + f1 (r.levelGain()) + " dB");
        const auto prop = r.last();
        check (prop.startsWith ("Measured -1") && prop.contains ("(loudest 3 s)") && prop.contains ("Push +") && prop.contains ("to reach -9.0?"), "the proposal reads \"Measured -x LUFS (loudest 3 s). Push +y dB to reach -9.0?\"", prop);
#ifdef EJ_LOUDNESSLOOP_PILLS
        check (r.loop.lastKind() == LoudnessLoop::Bubble::Kind::proposal && r.loop.lastPills().joinIntoString ("|") == "Go|Leave it", "A. the proposal bubble carries [Go] [Leave it]", r.loop.lastPills().joinIntoString ("|"));
#else
        check (false, "A. the proposal bubble carries [Go] [Leave it]", "no pills on this build");
#endif
        const float proposed = numberAfter (prop, "Push ");
        check (proposed >= 5.5f && proposed <= 6.0f, "the proposed trim is the pass clamp (+6 of the ~+9 needed)", f1 (proposed));
        check (r.loop.go(), "go applies");
        check (std::abs (r.levelGain() - proposed) < 0.05f, "A. the loop drives the Level slot, the limiter's input_db is untouched", "Level " + f1 (r.levelGain()) + " dB, limiter input_db " + f1 ((float) r.limiterInput()));
        check (std::abs (r.limiterInput()) < 0.01, "limiter input_db still 0.0", f1 ((float) r.limiterInput()));
        // 21 Sep 2026 (loop manners): Go is a level verb - it applies, says so, and NOTHING measures until Check
#ifdef EJ_LOUDNESSLOOP_MANNERS21
        check (r.loop.state() == LoudnessLoop::State::hold && r.last().startsWith ("Applied +") && r.last().contains (" dB (Level now +") && r.last().endsWith ("). How's it sounding?") && r.loop.lastPills().joinIntoString ("|") == "Check|A bit louder|A bit softer|Undo|Done",
               "M2. after Go the bubble reads \"Applied +X dB (Level now +Y). How's it sounding?\" with [Check] [A bit louder] [A bit softer] [Undo] [Done] in that order", r.last() + " | " + r.loop.lastPills().joinIntoString ("|"));
#else
        check (false, "M2. after Go the bubble reads \"Applied +X dB (Level now +Y). How's it sounding?\" with [Check] [A bit louder] [A bit softer] [Undo] [Done] in that order", "no MANNERS21 on this build: " + r.last());
#endif
        {   // M1: 20 s of counted audio after Go - zero EJLoudness measurement lines and no new bubble until Check is issued, then exactly one
            const int nb = r.loop.bubbleCount(), nm = measuredLines (r.logs);
            feed (r.proc, r.prog, 1875, false, &r.loop, nullptr, 0.0f);   // 1875 blocks of 512 at 48 kHz = 20 s of counted audio
            check (r.loop.state() == LoudnessLoop::State::hold && measuredLines (r.logs) == nm && r.loop.bubbleCount() == nb, "M1. 20 s of counted audio after Go: zero measurement lines, no new proposal bubble (the loop holds)", "state " + juce::String ((int) r.loop.state()) + " measured +" + juce::String (measuredLines (r.logs) - nm) + " bubbles +" + juce::String (r.loop.bubbleCount() - nb));
            check (r.loop.check() && r.loop.state() == LoudnessLoop::State::waitAudio, "M1. Check starts one window");
            r.runWindow();
            check (measuredLines (r.logs) == nm + 1 && r.loop.bubbleCount() == nb + 1 && r.loop.state() == LoudnessLoop::State::proposed, "M1. ...then exactly one measurement and one proposal bubble", "measured +" + juce::String (measuredLines (r.logs) - nm) + " bubbles +" + juce::String (r.loop.bubbleCount() - nb) + " | " + r.last());
        }
        check (r.loop.lastPills().joinIntoString ("|") == "Go|Leave it|Undo", "M3. the second proposal (it follows an apply) carries [Go] [Leave it] [Undo]", r.loop.lastPills().joinIntoString ("|"));
        r.loop.go(); r.loop.check(); r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::tracking && r.last().startsWith ("Hitting -") && r.last().contains ("on target"), "A. after go + Check the second window lands within +-1 dB and the loop tracks", r.last() + " | Level " + f1 (r.levelGain()));
        check (r.logs.size() >= 6 && r.logs[0].startsWith ("EJLoudness: armed") && r.logs.joinIntoString ("\n").contains ("EJLoudness: measured:") && r.logs.joinIntoString ("\n").contains ("EJLoudness: applied on go"), "item 5: EJLoudness lines for arm, measurement and apply", r.logs.joinIntoString (" | ").substring (0, 300));
    }
    std::printf ("== B. quiet section: build-time input -18, the window plays at -24 ==\n");
    {
        Rig r (false); r.setTarget (-9.0f, 0.0);
        calibrate (r.proc, r.prog, -18.0f);
        r.loop.armFromChain();
        check (std::abs (r.loop.buildInputLufs() + 18.0f) < 0.8f, "the build-time integrated input is captured at arm (-18)", f1 (r.loop.buildInputLufs()));
        r.runWindow (-6.0f);   // the verse: 6 dB under
#ifdef EJ_LOUDNESSLOOP_PILLS
        check (std::abs (r.levelGain()) < 0.01f && r.loop.state() == LoudnessLoop::State::quietAsked, "B. quiet section: the loop ASKS (state quietAsked), nothing applied until a pill", "Level " + f1 (r.levelGain()) + " state " + juce::String ((int) r.loop.state()));
        check (r.last().startsWith ("This is quieter than the section the chain was built on (") && r.last().contains ("at build). Is this the loudest part of the song?") && r.loop.lastPills().joinIntoString ("|") == "Listen again|This is the loudest part", "B. the quiet bubble names both numbers and carries [Listen again] [This is the loudest part]", r.last() + " " + r.loop.lastPills().joinIntoString ("|"));
        const int nb = r.loop.bubbleCount();
#ifdef EJ_LOUDNESSLOOP_MANNERS
        check (r.loop.listenAgain() && r.loop.state() == LoudnessLoop::State::waitAudio && r.loop.lastBubble() == "Listening..." && r.loop.bubbleCount() == nb, "B. [Listen again] re-arms the window with one bubble (18g: the Listening... progress bubble)", r.loop.lastBubble());
#else
        check (r.loop.listenAgain() && r.loop.state() == LoudnessLoop::State::waitAudio && r.last() == "Play the loudest part and I'll check again." && r.loop.bubbleCount() == nb + 1, "B. [Listen again] re-arms the window with one bubble", r.last());
#endif
        r.runWindow (-6.0f);   // quiet again -> asked again
        check (r.loop.state() == LoudnessLoop::State::quietAsked, "B. a quiet window asks again");
        const float heldBefore = r.levelGain();
        check (r.loop.loudestPart() && r.loop.state() == LoudnessLoop::State::proposed && r.last().startsWith ("Measured -") && r.loop.lastPills().joinIntoString ("|") == "Go|Leave it", "B. [This is the loudest part] proceeds to the normal proposal (Go / Leave it) from the held measurement", r.last() + " | pills " + r.loop.lastPills().joinIntoString ("|"));
        check (std::abs (r.levelGain() - heldBefore) < 0.01f, "B. ...and still nothing applied until Go", f1 (r.levelGain()));
        check (r.logs.joinIntoString ("\n").contains ("window quiet") && r.logs.joinIntoString ("\n").contains ("quiet window accepted as the loudest part"), "B. logged: window quiet, then accepted");
#else
        check (false, "B. quiet section: the loop ASKS (state quietAsked), nothing applied until a pill", "no pills on this build");
        check (false, "B. [This is the loudest part] proceeds to the normal proposal (Go / Leave it) from the held measurement", "no pills on this build");
        check (false, "B. [Listen again] re-arms the window with one bubble", "no pills on this build");
#endif
    }
    std::printf ("== C. chorus after verse: applied on a -18 window, then a +3 dB section ==\n");
    {
        Rig r (false); r.setTarget (-9.0f, 0.0);
        calibrate (r.proc, r.prog, -12.0f);   // needs +3 (within one pass)
        r.loop.armFromChain(); r.runWindow(); r.loop.go(); r.loop.check(); r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::tracking, "on target and tracking", r.last());
        const float g0 = r.levelGain();
        r.runTracking (+3.0f);   // a louder section while the loop tracks
        check (r.loop.state() == LoudnessLoop::State::proposed && r.last().startsWith ("That section is louder than the one I levelled on (") && r.last().contains ("over the target) - back off -"), "C. chorus after verse: a back-off is PROPOSED, never applied", r.last());
        check (std::abs (r.levelGain() - g0) < 0.01f, "the Level gain did not move on its own", f1 (r.levelGain()) + " vs " + f1 (g0));
#ifdef EJ_LOUDNESSLOOP_PILLS
        check (r.loop.lastPills().joinIntoString ("|") == "Back off|Leave it", "C. the back-off bubble carries [Back off] [Leave it]", r.loop.lastPills().joinIntoString ("|"));
        check (r.loop.backOff() && r.levelGain() < g0 - 1.0f, "C. [Back off] applies the back-off", f1 (r.levelGain()));
#else
        check (false, "C. the back-off bubble carries [Back off] [Leave it]", "no pills on this build");
        r.loop.go(); check (r.levelGain() < g0 - 1.0f, "go applies the back-off", f1 (r.levelGain()));
#endif
    }
    std::printf ("== D. a THIRD-PARTY limiter last (EJ Test Limiter, a clipper): the Level slot reaches the target ==\n");
    {
        Rig r (true); r.setTarget (-9.0f);
        calibrate (r.proc, r.prog, -14.0f);
        check (r.loop.armFromChain() && r.loop.limiterSlot() == 1 && r.h.getSlotInfo (1).name == "EJ Test Limiter", "armed with a non-EchoJay limiter last", r.logs.joinIntoString (" | ").substring (0, 160));
        r.runWindow(); r.loop.go(); r.loop.check(); r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::tracking || (r.loop.state() == LoudnessLoop::State::proposed && std::abs (numberAfter (r.last(), "Push ")) < 1.0f), "D. third-party limiter last: the target is reached through the Level slot", r.last() + " | Level " + f1 (r.levelGain()));
#ifdef EJ_LOUDNESSLOOP_MANNERS
        check (r.last().contains ("limiter working ~") && r.last().contains ("dB on the hits (worst peak ") && ! r.last().contains ("not an EchoJay device") && ! r.last().contains ("average"), "18g (4) third-party limiter: the GR reads \"limiter working ~X dB on the hits (worst peak Y)\" (21m: typical + worst, no average), never \"not an EchoJay device\"", r.last());
#else
        check (false, "18g (4) third-party limiter: the GR reads \"limiter working ~X dB (estimated)\", never \"not an EchoJay device\"", "no 18g on this build: " + r.last());
#endif
    }
    std::printf ("== E. the verbs ==\n");
    {
        Rig r (false); r.setTarget (-9.0f, 0.0);
        calibrate (r.proc, r.prog, -12.0f);
        r.loop.armFromChain(); r.runWindow(); r.loop.go(); r.loop.check(); r.runWindow();
        const float g0 = r.levelGain();
#ifdef EJ_LOUDNESSLOOP_PILLS
        check (r.loop.lastKind() == LoudnessLoop::Bubble::Kind::result && r.loop.lastPills().joinIntoString ("|") == "Undo|A bit louder|A bit softer|Done", "E. the result bubble (on target) carries [Undo] [A bit louder] [A bit softer] [Done] - no Push it on target (18h item 3)", r.loop.lastPills().joinIntoString ("|"));
        { const int nb = r.loop.bubbleCount(); r.loop.nudgeTarget (+1.0f); check (r.loop.bubbleCount() == nb + 1, "E. a verb after the result bubble produces exactly ONE reply bubble (a bit louder)", juce::String (r.loop.bubbleCount() - nb)); r.loop.nudgeTarget (-1.0f); }
        { const int nb = r.loop.bubbleCount(); r.loop.leaveIt(); check (r.loop.bubbleCount() == nb + 1 && r.loop.lastPills().joinIntoString ("|") == "Undo|A bit louder|A bit softer|Done", "E. leave it (on target): one bubble, the result pills without Push it (18h)", r.last() + " [" + r.loop.lastPills().joinIntoString ("|") + "]"); }
        { const int nb = r.loop.bubbleCount(); r.loop.recheck(); check (r.loop.bubbleCount() == nb + 1, "E. check the level again: one bubble"); r.runWindow(); }
#else
        check (false, "E. the result bubble (on target) carries [Undo] [A bit louder] [A bit softer] [Push it]", "no pills on this build");
        check (false, "E. a verb after the result bubble produces exactly ONE reply bubble (a bit louder)", "no pills on this build");
#endif
#ifdef EJ_LOUDNESSLOOP_VERBS18H
        // 18h (item 4): a level verb APPLIES and asks - one bubble, the after-verb pills, NO window runs until Check
        { const float g1 = r.levelGain(); const int nb = r.loop.bubbleCount(); r.loop.nudgeTarget (+1.0f);
          check (std::abs (r.loop.target() + 8.0f) < 0.01f && std::abs (r.levelGain() - g1 - 1.0f) < 0.05f && r.loop.state() == LoudnessLoop::State::hold && r.loop.bubbleCount() == nb + 1 && r.last().startsWith ("Applied +1.0 dB (Level now ") && r.last().endsWith ("). How's it sounding?") && r.loop.lastPills().joinIntoString ("|") == "Check|A bit louder|A bit softer|Undo|Done", "K1. a bit louder: Level +1 now, target -8, ONE bubble \"Applied +1.0 dB (Level now +Y). How's it sounding?\" with [Check] [A bit louder] [A bit softer] [Undo] [Done], no window", r.last() + " [" + r.loop.lastPills().joinIntoString ("|") + "] state " + juce::String ((int) r.loop.state())); }
        { const int nMeasured = r.logs.joinIntoString ("\n").indexOf ("measured:"); juce::ignoreUnused (nMeasured);
          const juce::String before = r.logs.joinIntoString ("\n"); const int cnt0 = juce::StringArray::fromLines (before).size();
          for (int k = 0; k < 16; ++k) feed (r.proc, r.prog, 100, false, &r.loop, nullptr, 0.0f);   // 16 s of audio after the verb
          check (r.loop.state() == LoudnessLoop::State::hold && juce::StringArray::fromLines (r.logs.joinIntoString ("\n")).size() == cnt0, "K1. ...and NO automatic check follows a verb (16 s of audio: nothing measured, nothing logged)", "state " + juce::String ((int) r.loop.state())); }
        check (r.loop.check() && r.loop.state() == LoudnessLoop::State::waitAudio, "K1. Check starts one window");
        r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::tracking && r.last().startsWith ("Hitting -") && r.last().contains ("on target") && r.loop.lastPills().joinIntoString ("|") == "Undo|A bit louder|A bit softer|Done", "K1. ...which REPORTS on target with the result pills (no Push it, no proposal)", r.last() + " [" + r.loop.lastPills().joinIntoString ("|") + "]");
        { const float g2 = r.levelGain(); r.loop.nudgeTarget (-1.0f); check (std::abs (r.loop.target() + 9.0f) < 0.01f && std::abs (r.levelGain() - g2 + 1.0f) < 0.05f && r.last().startsWith ("Applied -1.0 dB (Level now ") && r.loop.lastPills().joinIntoString ("|") == "Check|A bit louder|A bit softer|Undo|Done", "K1. a bit softer: Level -1 now, target back to -9, the same bubble and pills", r.last()); }
        r.loop.check(); r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::tracking && r.last().contains ("on target"), "K1. Check after it: on target (-9)", r.last());
        r.loop.leaveIt(); check (r.loop.state() == LoudnessLoop::State::hold && r.last().startsWith ("Leaving it at"), "leave it: holds, says where", r.last());
        // Push it: offered only when SHORT - make the loop short by a softer target-side nudge... simpler: push it from here applies the shortfall (~0) and asks
        { const float before = r.levelGain(), shortfall = r.loop.target() - r.loop.lastMeasured(); check (r.loop.pushIt(), "push it applies immediately"); check (std::abs ((r.levelGain() - before) - juce::jlimit (-6.0f, 6.0f, shortfall)) < 0.05f && r.last().startsWith ("Applied ") && r.last().endsWith ("). How's it sounding?") && r.loop.state() == LoudnessLoop::State::hold, "K1. push it: moved the Level by the shortfall, the after-verb bubble, no window", f1 (before) + " -> " + f1 (r.levelGain()) + " (shortfall " + f1 (shortfall) + ") " + r.last()); }
#else
        r.loop.nudgeTarget (+1.0f); check (std::abs (r.loop.target() + 8.0f) < 0.01f && r.last().startsWith ("Target now -8.0"), "a bit louder: target -8, one more pass", r.last());
        r.runWindow(); check (r.loop.state() == LoudnessLoop::State::proposed && numberAfter (r.last(), "Push ") > 0.5f, "...which proposes about +1", r.last());
        r.loop.go(); r.loop.check(); r.runWindow();
        r.loop.nudgeTarget (-1.0f); check (std::abs (r.loop.target() + 9.0f) < 0.01f, "a bit softer: target back to -9");
        r.runWindow(); check (r.loop.state() == LoudnessLoop::State::proposed && numberAfter (r.last(), "Push ") < -0.5f, "...which proposes about -1", r.last());
        r.loop.leaveIt(); check (r.loop.state() == LoudnessLoop::State::hold && r.last().startsWith ("Leaving it at"), "leave it: holds, says where", r.last());
        r.loop.recheck(); check (r.loop.state() == LoudnessLoop::State::waitAudio && r.last().startsWith ("Checking the level again"), "check the level again: a new window");
        r.runWindow(); check (r.loop.state() == LoudnessLoop::State::proposed, "...which proposes");
        const float before = r.levelGain(); check (r.loop.pushIt(), "push it applies immediately"); check (std::abs (r.levelGain() - before) > 0.4f, "...and moved the Level by the shortfall", f1 (before) + " -> " + f1 (r.levelGain()));
#endif
        check (r.loop.undo() && std::abs (r.levelGain()) < 0.01f && (r.last().startsWith ("Restored the Level slot to +0.0 dB") || (r.last().startsWith ("Applied ") && r.last().contains ("(Level now +0.0). How's it sounding?"))), "E. verbs: push it / a bit louder / a bit softer / check the level again / undo / leave it", r.last() + " | Level " + f1 (r.levelGain()) + " (g0 " + f1 (g0) + ")");
    }
    std::printf ("== F. GR text on a PEAKY programme + H. true peak by the independent meter ==\n");
    {
        Rig r (false); r.setTarget (-9.0f, 0.0); r.prog.peaky = true;
        calibrate (r.proc, r.prog, -15.0f);
        r.loop.armFromChain(); r.runWindow();
        // 22 Sep 2026 (item 5): on a peaky programme the first proposal is CAPPED (the limiter would work > 4 dB) - driving into the
        // limiter is now the user's [Push it anyway]; then Check runs the window and the GR text reads as before
        // 21m: the cap is on the TYPICAL hits, so this programme may or may not cap - either way drive into the limiter (Push it anyway / Go)
        if (r.last().contains ("is as loud as this goes")) r.loop.pushIt(); else r.loop.go();
        r.loop.check();
        IndependentMeter ind; r.runWindow (0.0f, &ind);
        const auto last = r.last();
        check (last.contains ("limiter working ") && last.contains (" dB on the hits (worst peak ") && ! last.contains ("average"), "F. GR text (21m): \"limiter working X dB on the hits (worst peak Y)\" - typical + worst, no average", last);
        const float worst = numberAfter (last, "worst peak ");
        check (std::isfinite (worst) && worst > 0.5f && worst >= r.loop.grMax() - 0.05f, "...the worst-peak figure carries the EchoJay Limiter's own max GR (> 0.5 dB)", f1 (worst) + " vs grMax " + f1 (r.loop.grMax()));
        check (ind.truePeakDb() <= -0.1f + 0.05f, "H. peaky programme: output true peak <= -0.1 dBTP by the independent meter", f1 (ind.truePeakDb()) + " dBTP");
    }
    std::printf ("== G. 18d: arm AFTER the exact built-in apply (the live order) - on the Level slot ==\n");
    {
        Rig r (false);
        r.h.setSlotSettings (r.levelSlot, "Level 0 dB to the -9 LUFS target");   // the text first, as a build attaches it
        r.setTarget (-9.0f, 8.8);                                                  // then the structured settings: the exact apply replaces the text
        EchoJayBorrowHostTestAccess::applyExact (r.h, r.levelSlot); EchoJayBorrowHostTestAccess::applyExact (r.h, r.limSlot);
        check (r.h.getSlotInfo (r.levelSlot).settings.startsWith ("Applied automatically"), "the exact apply replaced the Level slot's text", r.h.getSlotInfo (r.levelSlot).settings.substring (0, 60));
        check (r.loop.armFromChain() && std::abs (r.loop.target() + 9.0f) < 0.01f && r.loop.armSource() == "level_params", "G. arm after the exact built-in apply (18d) stays GREEN", r.loop.armSource());
        r.loop.leaveIt();
    }
    std::printf ("== I. COMPATIBILITY: a LIVE-shaped chain (88x7asebn: target_lufs on the EchoJay Limiter with input_db +8.8, NO Level slot) ==\n");
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512); auto& h = proc.getChainHost();
        const auto* eq = BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ");
        if (eq) EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*eq));
        EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*dev));
        const int limAt = h.getNumSlots() - 1;
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("input_db", 8.8); pp->setProperty ("ceiling_db", -0.1); pp->setProperty ("true_peak", 1); pp->setProperty ("target_lufs", -9.0); pp->setProperty ("loudness_option", "commercial");
          auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (limAt, juce::var (w)); }
        auto& loop = proc.loudnessLoop(); juce::StringArray logs, bubbles; loop.logLine = [&] (const juce::String& l) { logs.add (l); }; loop.onBubble = [&] (const LoudnessLoop::Bubble& b) { if (! b.text.startsWith ("Listening") && ! b.text.startsWith ("Checking...")) bubbles.add (b.text); }; loop.isPlaying = [] { return true; };
        Programme prog; calibrate (proc, prog, -15.0f);
        const bool armed = loop.armFromChain();
        check (armed, "I. the live-shaped chain ARMS (the loop inserts the Level slot it needs; RED as it stood: not armed, the insertion lived in the editor)", logs.joinIntoString (" | ").substring (0, 200));
        check (h.getNumSlots() == limAt + 2 && h.getSlotInfo (limAt).name == "EchoJay Level" && h.getSlotInfo (limAt + 1).name == "EchoJay Limiter", "I. EchoJay Level was inserted immediately before the limiter", h.getSlotInfo (0).name + " | " + h.getSlotInfo (1).name + (h.getNumSlots() > 2 ? " | " + h.getSlotInfo (2).name : juce::String()));
        check (armed && loop.armSource() == "level_params" && std::abs (loop.target() + 9.0f) < 0.01f && loop.levelSlot() == limAt && loop.limiterSlot() == limAt + 1, "I. armed from the inserted Level slot's params, target -9 (copied from the limiter), limiter slot = the last", loop.armSource() + " " + juce::String (loop.levelSlot()) + "/" + juce::String (loop.limiterSlot()));
        auto* lim = dynamic_cast<EedLimiterProcessor*> (h.getSlotProcessor (limAt + 1));
        auto* lv  = dynamic_cast<EedLevelProcessor*> (h.getSlotProcessor (limAt));
        check (lim != nullptr && std::abs (lim->inputDb() - 8.8) < 0.01 && lv != nullptr && std::abs (lv->gainDb()) < 0.01, "I. the server's input_db +8.8 stays on the limiter; the Level starts at 0", lim ? juce::String (lim->inputDb(), 2) : "no limiter");
#ifdef EJ_LOUDNESSLOOP_MANNERS
        loop.listen();   // 18g: the window starts on Listen
#endif
        for (int k = 0; k < 16 && (loop.state() == LoudnessLoop::State::waitAudio || loop.state() == LoudnessLoop::State::measuring); ++k) feed (proc, prog, 100, false, &loop, nullptr);
        check (loop.state() == LoudnessLoop::State::proposed || loop.state() == LoudnessLoop::State::tracking, "I. a window measured -> a proposal (or on target)", bubbles.isEmpty() ? juce::String() : bubbles[bubbles.size() - 1]);
        if (loop.state() == LoudnessLoop::State::proposed) loop.go();
        check (lv != nullptr && lim != nullptr && std::abs (lim->inputDb() - 8.8) < 0.01, "I. after go the loop DROVE THE LEVEL SLOT and the limiter's input_db is still +8.8", "Level " + juce::String (lv ? lv->gainDb() : 0.0, 2) + " dB, limiter input_db " + juce::String (lim ? lim->inputDb() : 0.0, 2));
        check (logs.joinIntoString ("\n").contains ("EJLoudness: inserted EchoJay Level at slot"), "I. the insertion is logged as EJLoudness", logs.joinIntoString (" | ").substring (0, 160));
    }
#endif

    std::printf ("== J. 18g loop manners: explicit Listen / Check, +-1 dB with step scaling and 3 proposals, Done, the GR estimate, the ceiling safety net ==\n");
#ifdef EJ_LOUDNESSLOOP_MANNERS
    {   // J1: no window before Listen
        Rig r (false); r.setTarget (-9.0f, 0.0); calibrate (r.proc, r.prog, -18.0f);
        r.loop.armFromChain();
        check (r.loop.state() == LoudnessLoop::State::armed && r.last() == "Cue the loudest section, press play, then tap Listen." && r.loop.lastPills().joinIntoString ("|") == "Listen", "J1. the arm bubble reads \"Cue the loudest section, press play, then tap Listen\" with [Listen]", r.last() + " [" + r.loop.lastPills().joinIntoString ("|") + "]");
        for (int k = 0; k < 16; ++k) feed (r.proc, r.prog, 100, false, &r.loop, nullptr, 0.0f);   // 16 x ~1 s of the loudest part, ticking - no Listen
        check (r.loop.state() == LoudnessLoop::State::armed && ! r.logs.joinIntoString ("\n").contains ("measured:") && std::abs (r.levelGain()) < 0.01f, "J1. NO window runs on the first audio: 16 s of audio without Listen measures nothing, proposes nothing", "state " + juce::String ((int) r.loop.state()));
        check (r.loop.listen() && r.loop.state() == LoudnessLoop::State::waitAudio, "J1. Listen starts the window");
        r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::proposed && r.loop.lastPills().joinIntoString ("|") == "Go|Leave it", "J1. ...which measures and proposes with [Go] [Leave it]", r.last() + " | pills " + r.loop.lastPills().joinIntoString ("|"));
        // 21 Sep 2026 (loop manners): after Go the loop HOLDS - no automatic check while the audio continues (M1 in leg A) and no
        // "Tap Check" prompt when it stops: the after-Go bubble already carries [Check]
        r.loop.go();
        check (r.loop.state() == LoudnessLoop::State::hold && r.last().startsWith ("Applied "), "J2. after Go: hold with the after-verb bubble, no window runs", r.last());
        { const int nb = r.loop.bubbleCount(); feed (r.proc, r.prog, 23 * 10, true, &r.loop, nullptr, 0.0f);   // 10 ticks of silence after Go
          check (r.loop.state() == LoudnessLoop::State::hold && r.loop.bubbleCount() == nb && r.last().startsWith ("Applied "), "J2b. ...and 10 silent ticks change nothing (no \"Tap Check\" bubble, no measurement)", r.last()); }
        check (r.loop.check() && r.loop.state() == LoudnessLoop::State::waitAudio, "J2b. Check starts the window");
        r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::proposed || r.loop.state() == LoudnessLoop::State::tracking, "J2b. ...which measures", r.last());
    }
    {   // J3: the limiter-nonlinearity fixture (EJ Test Soft Limiter: 0.6 dB out per dB in) - on target in <= 2 proposals
        // -25.5 in -> about -13.5 out of the 0.6x device: ~4.5 dB needed, under the 6 dB pass clamp. The unscaled loop (as it
        // stood, tolerance 0.5) needs 3 shrinking passes (2.7, 1.1, 0.4); the scaled loop lands in 2. (At -22 in the need was
        // only 2.3 dB and BOTH loops converged in <= 2 - the fixture did not discriminate; Sean's HOLD, 20 Sep.)
        Rig r (true, true, "EJ Test Soft Limiter"); r.setTarget (-9.0f); calibrate (r.proc, r.prog, -25.5f);
        r.loop.armFromChain(); r.loop.listen(); r.runWindow();
        int proposals = 0; float lastNeeded = 99.0f;
        for (int k = 0; k < 6 && r.loop.state() == LoudnessLoop::State::proposed; ++k) { ++proposals; r.loop.go(); r.loop.check(); r.runWindow(); }
        lastNeeded = r.loop.target() - r.loop.lastMeasured();
        check (r.loop.state() == LoudnessLoop::State::tracking && std::abs (lastNeeded) <= 1.0f, "J3. the 0.6x fixture lands within +-1.0 dB of the target", "needed " + f1 (lastNeeded) + " state " + juce::String ((int) r.loop.state()));
        check (proposals <= 2, "J3. ...in at most 2 proposals (step scaled by achieved/commanded; RED as it stood: 3 shrinking passes)", juce::String (proposals) + " proposal(s)");
        check (r.logs.joinIntoString ("\n").contains ("step scaling: commanded") && r.logs.joinIntoString ("\n").contains ("ratio 0.6"), "J3. the scaling is logged (ratio 0.6x)", r.logs.joinIntoString (" | ").fromLastOccurrenceOf ("step scaling", true, false).substring (0, 120));
        check (r.last().contains ("limiter working ~"), "J3. the GR line on this third-party fixture is the estimate (the ~ mark)", r.last());
        // J4: Done ends the watch - a louder section afterwards proposes nothing
        const int nb = r.loop.bubbleCount();
        check (r.loop.done() && r.loop.state() == LoudnessLoop::State::hold && r.loop.bubbleCount() == nb + 1 && r.last().startsWith ("Done - Level "), "J4. Done: one bubble, the loop holds", r.last());
        const float g0 = r.levelGain(); const int nb2 = r.loop.bubbleCount();
        for (int k = 0; k < 16; ++k) feed (r.proc, r.prog, 100, false, &r.loop, nullptr, +4.0f);   // a louder section after Done
        check (r.loop.state() == LoudnessLoop::State::hold && r.loop.bubbleCount() == nb2 && std::abs (r.levelGain() - g0) < 0.01f, "J4. after Done a louder section is neither measured nor proposed on (the watch is over)", "state " + juce::String ((int) r.loop.state()));
    }
    {   // J5: the GR estimate (Level OUT - chain OUT) against the EchoJay Limiter's REAL GR on a steadily limited programme
        Rig r (false); r.setTarget (-9.0f, 0.0); calibrate (r.proc, r.prog, +2.0f);   // noise at +2 LUFS in (peaks ~+3 dBFS): the -0.1 dBTP wall works steadily
        r.loop.armFromChain(); r.loop.listen(); r.runWindow();
        const float real = r.loop.grAvg(), est = r.loop.grEstimateDb();
        check (std::isfinite (est) && real > 0.5f && std::abs (est - real) <= 1.0f, "J5. the estimate lands within 1 dB of the EchoJay Limiter's real GR", "estimate " + f1 (est) + " vs real " + f1 (real) + " dB");
    }
#ifdef EJ_LOUDNESSLOOP_VERBS18H
    {   // K5 (18h item 5): the THIRD-PARTY fixture (EJ Test Limiter: a clipper at -0.5 dBFS, no GR readout). Truth from OUTSIDE the
        // loop: the SAME seeded programme through a second rig whose last slot is an identity stand-in (EJ Test Bypass) - its output
        // by the INDEPENDENT meter is the pre-limiter signal (true peak and loudness); the clipper rig's output by the same meter is
        // the post-limiter signal. (A sample-peak formula was wrong: white noise carries ~5 dB of intersample overshoot, so a clipper
        // at -0.5 dBFS still reads +5.7 dBTP - Sean's observation (b) in miniature.)
        // 22 Sep 2026 (item 5): the target sits ABOVE the programme (+6) so the first proposal is a capped push; [Push it anyway] then drives
        // both rigs into the clipper by the same +6 (arming had clamped the opening gain below 0 dB: peaks ~+9 dBTP over a -0.5 ceiling)
        Rig r (true, true); r.setTarget (+6.0f); calibrate (r.proc, r.prog, +3.0f);   // +3 LUFS in: sample peaks ~+1.7 dBFS, over the -0.5 dBFS clip
        Rig b (true, true, "EJ Test Bypass"); b.setTarget (+6.0f); calibrate (b.proc, b.prog, +3.0f);
        // 22 Sep 2026 (item 5): arming CLAMPS the opening gain (peaks +7 dBTP over a -0.5 ceiling -> the Level opens below 0 dB) and the
        // first proposal is capped, so both rigs are driven into the clipper the user's way - [Push it anyway] (+6, the same on both) -
        // and the estimate window is measured after that push with fresh tallies and fresh independent meters
        r.loop.armFromChain(); r.loop.listen(); r.runWindow(); r.loop.pushIt(); r.loop.check(); IndependentMeter ind; r.runWindow (0.0f, &ind);
        b.loop.armFromChain(); b.loop.listen(); b.runWindow(); b.loop.pushIt(); b.loop.check(); IndependentMeter indB; b.runWindow (0.0f, &indB);
        feed (r.proc, r.prog, 400, false, nullptr, nullptr, 0.0f); feed (b.proc, b.prog, 400, false, nullptr, nullptr, 0.0f);   // 21m: a full 3 s window on both tallies after the loop's own window (its tracking reset empties the chain-out ring)
        const float g = r.levelGain();
        const auto in = r.h.getChainInLevels(), out = r.h.getChainOutLevels();
        const float truthLoud = (in.shortTermDb + g) - out.shortTermDb;   // the latest full 3 s window on both tallies (the max hold restarts when the loop starts tracking - 21m)
        const float truthPeak = indB.truePeakDb() - ind.truePeakDb();
        const float estLoud = r.loop.grEstimateDb(), estPeak = r.loop.grPeakEstimateDb();
        std::printf ("  K5 inputs: amp %.3f / %.3f, gain %+.2f / %+.2f dB | chain in maxST %.2f, chain out maxST %.2f | Level OUT TP %.2f, chain OUT TP %.2f (tallies) | independent TP clipper %.2f, bypass %.2f\n", r.prog.amp, b.prog.amp, g, b.levelGain(), in.maxShortTermDb, out.maxShortTermDb, dynamic_cast<EedLevelProcessor*> (r.h.getSlotProcessor (r.levelSlot))->outputLevels().truePeakDb, out.truePeakDb, ind.truePeakDb(), indB.truePeakDb());
        check (std::abs (r.prog.amp - b.prog.amp) < 1e-4f && std::abs (g - b.levelGain()) < 0.01f, "K5. the two rigs ran the same programme at the same gain (the bypass rig is the pre-limiter truth)", juce::String (r.prog.amp, 4) + " / " + juce::String (b.prog.amp, 4));
        check (std::isfinite (estLoud) && truthLoud > 0.3f && std::abs (estLoud - truthLoud) <= 1.0f, "K5. third-party limiter: the loudness GR estimate is within 1 dB of the truth (chain in + gain - chain out), and the clipper is working", "estimate " + f1 (estLoud) + " vs truth " + f1 (truthLoud) + " dB");
        check (std::isfinite (estPeak) && truthPeak > 0.5f && std::abs (estPeak - truthPeak) <= 1.0f, "K5. ...and the PEAK GR estimate (Level OUT true peak - chain OUT true peak) is within 1 dB of the independent pre/post true-peak difference", "estimate " + f1 (estPeak) + " vs truth " + f1 (truthPeak) + " dB");
        check (r.last().contains ("limiter working ~") && r.last().contains ("dB on the hits (worst peak ") && ! r.last().contains ("average"), "K5. the bubble reads \"limiter working ~X dB on the hits (worst peak Y)\" (21m)", r.last());
        check (r.logs.joinIntoString ("\n").contains ("true peak: Level OUT TP ") && r.logs.joinIntoString ("\n").contains (" dBTP, chain OUT TP "), "K5. the EJLoudness log carries the true-peak VALUES (Level OUT TP, chain OUT TP), not only their difference", r.logs.joinIntoString (" | ").fromLastOccurrenceOf ("true peak:", true, false).substring (0, 120));
    }
#else
    for (const char* leg : { "K1. a bit louder: Level +1 now, target -8, ONE bubble \"Applied +1.0 dB (Level now +Y). How's it sounding?\" with [Check] [A bit louder] [A bit softer] [Undo] [Done], no window", "K1. ...and NO automatic check follows a verb (16 s of audio: nothing measured, nothing logged)", "K1. ...which REPORTS on target with the result pills (no Push it, no proposal)", "K1. push it: moved the Level by the shortfall, the after-verb bubble, no window",
                             "K5. third-party limiter: the loudness GR estimate is within 1 dB of the truth (chain in + gain - chain out), and the clipper is working", "K5. ...and the PEAK GR estimate (Level OUT true peak - chain OUT true peak) is within 1 dB of the independent pre/post true-peak difference", "K5. the bubble reads \"limiter working ~X dB (estimated), up to ~Y dB on the hits\"" })
        check (false, leg, "no 18h on this build");
#endif
    {   // J6: the ceiling safety net - a third-party limiter with NO ceiling readback is replaced by EchoJay Limiter, said in one line
        Rig r (true, false); r.setTarget (-9.0f); calibrate (r.proc, r.prog, -14.0f);
        const int nb = r.loop.bubbleCount();
        check (r.loop.armFromChain() && r.h.getSlotInfo (1).name == "EchoJay Limiter" && r.loop.limiterSlot() == 1 && r.h.getSlotInfo (0).name == "EchoJay Level", "J6. ceiling readback absent -> EchoJay Limiter substituted at the last slot, the Level slot untouched", r.h.getSlotInfo (0).name + " | " + r.h.getSlotInfo (1).name);
        check (r.loop.bubbleCount() == nb + 2 && r.bubbles.size() >= 2 && r.bubbles[r.bubbles.size() - 2] == "EJ Test Limiter's ceiling could not be confirmed, so EchoJay Limiter holds the ceiling instead (-0.1 dBTP).", "J6. ...said in one line before the arm bubble", r.bubbles.size() >= 2 ? r.bubbles[r.bubbles.size() - 2] : juce::String ("(none)"));
        check (r.logs.joinIntoString ("\n").contains ("substituted EchoJay Limiter for EJ Test Limiter"), "J6. ...and logged");
        r.loop.listen(); r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::proposed || r.loop.state() == LoudnessLoop::State::tracking, "J6. the loop then runs against the substituted limiter (real GR)", r.last());
    }
    {   // J6b: a third-party limiter WITH a ceiling readback is kept
        Rig r (true, true); r.setTarget (-9.0f); calibrate (r.proc, r.prog, -14.0f);
        check (r.loop.armFromChain() && r.h.getSlotInfo (1).name == "EJ Test Limiter", "J6. a third-party limiter whose ceiling READ BACK is kept", r.h.getSlotInfo (1).name);
    }
#else
    for (const char* leg : { "J1. the arm bubble reads \"Cue the loudest section, press play, then tap Listen\" with [Listen]", "J1. NO window runs on the first audio: 16 s of audio without Listen measures nothing, proposes nothing",
                             "J2. after Go: hold with the after-verb bubble, no window runs", "J2b. ...and 10 silent ticks change nothing (no \"Tap Check\" bubble, no measurement)", "J2b. Check starts the window", "J2b. ...which measures",
                             "J3. ...in at most 2 proposals (step scaled by achieved/commanded; RED as it stood: 3 shrinking passes)", "J4. Done: one bubble, the loop holds", "J4. after Done a louder section is neither measured nor proposed on (the watch is over)",
                             "J5. the estimate lands within 1 dB of the EchoJay Limiter's real GR", "J6. ceiling readback absent -> EchoJay Limiter substituted at the last slot, the Level slot untouched" })
        check (false, leg, "no 18g on this build");
    {   // the 0.6x fixture AS IT STOOD, for the record: how many proposals does the unscaled loop need?
        Rig r (true, true, "EJ Test Soft Limiter"); r.setTarget (-9.0f); calibrate (r.proc, r.prog, -25.5f);
        r.loop.armFromChain(); r.runWindow(); int proposals = 0;
        for (int k = 0; k < 6 && r.loop.state() == LoudnessLoop::State::proposed; ++k) { ++proposals; r.loop.go(); r.loop.check(); r.runWindow(); }
        check (proposals <= 2, "J3 (AS IT STOOD): the unscaled loop on the 0.6x fixture converges in at most 2 proposals - this build's count", juce::String (proposals) + " proposal(s), last: " + r.last());
    }
#endif
    std::printf ("== K. 22 Sep 2026 (item 5): the GR cap per loudness option, the capped proposal, the opening-gain clamp, the estimated-GR line ==\n");
    {
        Rig r (true); r.setTarget (-8.0f, 0.0); r.prog.peaky = true; r.prog.burst = 30.0f;   // third-party limiter (hard clip -0.5 dBTP), Commercial (option 0) -> cap 6 dB (21m); hits typically ~+7 dBTP so the typical reduction is over the cap
        const float cal = calibrate (r.proc, r.prog, -18.0f); check (std::abs (cal + 18.0f) < 0.8f, "K. peaky programme calibrated to -18 LUFS", f1 (cal));
        check (r.loop.armFromChain(), "K. armed (third-party limiter last, Commercial)", r.logs.joinIntoString (" | ").substring (0, 200));
        r.runWindow();
        const auto prop = r.last(); const auto logAll = r.logs.joinIntoString ("\n");
        check (r.loop.state() == LoudnessLoop::State::proposed && prop.contains ("is as loud as this goes with the limiter working <=6 dB. Push to -8.0 anyway?"),
               "K1. the proposal is CAPPED by limiter GR on the hits (Commercial <= 6 dB, 21m): \"<level> is as loud as this goes with the limiter working <=6 dB. Push to -8.0 anyway?\"", prop);
        check (r.loop.lastPills().joinIntoString ("|") == "Push it anyway|Leave it", "K2. the capped proposal carries [Push it anyway] [Leave it]", r.loop.lastPills().joinIntoString ("|"));
        check (logAll.contains ("GR cap: typical hit true peak") && logAll.contains ("> cap 6.0 (commercial) -> trim +"), "K1. the cap arithmetic is logged (typical hit true peak + trim - ceiling > cap -> trim; 21m: the top-20 % block measure)", logAll.fromLastOccurrenceOf ("GR cap", false, false).substring (0, 160));
        check (logAll.contains ("true peak: Level OUT TP ") && logAll.contains ("(third-party limiter: the hits figure is the report)"),
               "K4 (ruling 4): with a third-party limiter the measured line is followed by the true-peak line (Level OUT TP, chain OUT TP, hits) and the hits figure is the report", logAll.fromLastOccurrenceOf ("true peak:", true, false).substring (0, 140));
        const float cappedGain = numberAfter (prop, "(loudest 3 s). ") - r.loop.lastMeasured();   // the capped level minus the measured = the capped trim
        check (cappedGain > 0.3f && cappedGain < 6.5f, "K1. the capped trim is positive and under the cap (the limiter would work <= 6 dB)", f1 (cappedGain));
        check (r.loop.pushIt() && r.levelGain() > cappedGain + 0.5f, "K2. Push it anyway applies the UNCAPPED step (the pass clamp, +6)", f1 (r.levelGain()));
    }
    {
        Rig r (false); r.setTarget (-8.0f, 0.0); r.prog.peaky = true;
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 12.0); pp->setProperty ("target_lufs", -8.0); pp->setProperty ("loudness_option", 0); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); r.h.setSlotStructuredSettings (r.levelSlot, juce::var (w)); }
        check (std::abs (r.levelGain() - 12.0f) < 0.01f, "K3. the build opened the Level at +12 dB (the server's estimate)", f1 (r.levelGain()));
        r.h.resetAllLevels(); feed (r.proc, r.prog, 900, false, nullptr, nullptr, 0.0f);   // ~9.6 s of the peaky programme: the chain-in tally knows its true peak
        const auto in = r.h.getChainInLevels(); const float maxOpen = -0.1f + 3.0f - in.truePeakDb;
        check (in.known && in.truePeakDb > -60.0f, "K3. the chain-in tally carries the build-time true peak", f1 (in.truePeakDb) + " dBTP known=" + juce::String ((int) in.known));
        check (r.loop.armFromChain(), "K3. armed");
        check (r.levelGain() <= maxOpen + 0.05f && r.levelGain() < 11.9f && r.logs.joinIntoString ("\n").contains ("opening gain capped:"),
               "K3. opening gain at build = min (estimate, ceiling + 3 dB - build-time true peak): +12 is clamped so peaks never open more than 3 dB over the ceiling", "Level " + f1 (r.levelGain()) + " dB, allowed " + f1 (maxOpen) + " (ceiling -0.1 + 3 - TP " + f1 (in.truePeakDb) + ")");
    }
    std::printf ("== L. 22 Sep 2026 rulings 5b + 2: the opening-gain FLOOR (-6.0) with the card's reason, and the complaint verb (softer x2) ==\n");
    {
        Rig r (false); r.setTarget (-8.0f, 0.0); r.prog.peaky = true; r.prog.burst = 40.0f;   // hits far above the ceiling: ceiling + 3 - TP is well below -6
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 6.0); pp->setProperty ("target_lufs", -8.0); pp->setProperty ("loudness_option", 0); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); r.h.setSlotStructuredSettings (r.levelSlot, juce::var (w)); }
        r.h.resetAllLevels(); feed (r.proc, r.prog, 900, false, nullptr, nullptr, 0.0f);
        const auto in = r.h.getChainInLevels(); const float raw = -0.1f + 3.0f - in.truePeakDb;
        check (in.known && raw < -6.5f, "L1. precondition: ceiling + 3 - build-time true peak is below the floor", f1 (raw) + " (TP " + f1 (in.truePeakDb) + ")");
        check (r.loop.armFromChain(), "L1. armed");
        check (std::abs (r.levelGain() + 6.0f) < 0.01f && r.logs.joinIntoString ("\n").contains ("floored at -6.0"), "L1 (5b). the opening gain is FLOORED at -6.0 dB", "Level " + f1 (r.levelGain()));
        check (r.h.getSlotInfo (r.levelSlot).settings == "Level -6.0 dB: the mix already peaks above the ceiling", "L1 (5b). the chain card's Level line reads \"Level -6.0 dB: the mix already peaks above the ceiling\"", r.h.getSlotInfo (r.levelSlot).settings);
    }
    {
        Rig r (false); r.setTarget (-9.0f, 0.0);
        calibrate (r.proc, r.prog, -18.0f); r.loop.armFromChain(); r.runWindow(); r.loop.go();
        const float before = r.levelGain(), tBefore = r.loop.target(); const int nb = r.loop.bubbleCount();
        check (r.loop.backOffComplaint() && std::abs (r.levelGain() - (before - 2.0f)) < 0.05f && std::abs (r.loop.target() - (tBefore - 2.0f)) < 0.01f && r.loop.bubbleCount() == nb + 1 && r.last().startsWith ("Applied -2.0 dB (Level now "),
               "L2 (item 2, client half). a complaint after the apply = the softer step twice: Level -2, target -2, ONE bubble \"Applied -2.0 dB (Level now ...)\"", r.last() + " | Level " + f1 (before) + " -> " + f1 (r.levelGain()));
    }
    std::printf ("== N. 22 Sep 2026 (21m item 1): the loop tracks its Level slot by IDENTITY - an insert before it keeps the loop armed on the same Level; removing the Level stops it ==\n");
    {
        Rig r (false); r.setTarget (-9.0f, 0.0);
        calibrate (r.proc, r.prog, -18.0f);
        check (r.loop.armFromChain() && r.loop.levelSlot() == 0, "N0. armed on the Level at slot 0", juce::String (r.loop.levelSlot()));
        auto* levelBefore = r.h.getSlotProcessor (0);
        const auto* byp = BuiltinDeviceRegistry::instance().findByName ("EJ Test Bypass");
        check (byp != nullptr && r.h.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*byp), 0).isEmpty() && r.h.getNumSlots() == 3 && r.h.getSlotInfo (1).name == "EchoJay Level", "N1. a slot inserted BEFORE the Level moves it to index 1", r.h.getSlotInfo (0).name + " | " + r.h.getSlotInfo (1).name);
        r.loop.tickNow();
        check (r.loop.state() != LoudnessLoop::State::hold && r.loop.levelSlot() == 1 && r.h.getSlotProcessor (1) == levelBefore && ! r.last().contains ("no longer in the chain"),
               "N1. the loop stays ARMED on the SAME Level instance (now slot 1), no \"no longer in the chain\" (RED today: index 0 is the inserted slot, the loop stops)", "state " + juce::String ((int) r.loop.state()) + " slot " + juce::String (r.loop.levelSlot()) + " | " + r.last());
        r.runWindow();
        check (r.loop.state() == LoudnessLoop::State::proposed && r.loop.go() && std::abs ((float) dynamic_cast<EedLevelProcessor*> (r.h.getSlotProcessor (1))->gainDb() - 6.0f) < 0.1f, "N1. ...and Go drives that same Level (slot 1 gained +6)", f1 ((float) dynamic_cast<EedLevelProcessor*> (r.h.getSlotProcessor (1))->gainDb()));
        r.loop.check();   // a window is running (after Go the loop holds by design and says nothing until Check)
        r.h.removeSlot (1); r.loop.tickNow();
        check (r.loop.state() == LoudnessLoop::State::hold && r.last().contains ("The Level slot is no longer in the chain"), "N2. removing the Level itself stops the loop with the message", r.last());
    }
    std::printf ("== O. 22 Sep 2026 (21m item 3): the cap is on the TYPICAL reduction (top 20 %% of 100 ms blocks), the worst peak is shown beside it ==\n");
    {   // one 9 dB transient among hits typically ~4 dB over the ceiling -> NOT capped under Commercial (6)
        Rig r (true); r.setTarget (-10.5f, 0.0); r.prog.peaky = true;   // target ~1.5 dB above the measured level: a small trim, so the projected typical stays under the cap
        calibrate (r.proc, r.prog, -18.0f); r.prog.burst = 18.0f;   // the hits AFTER calibration: typically ~3-4 dB over the -0.5 dBTP clip
        r.loop.armFromChain(); r.prog.spikeEvery = 300; r.prog.spike = 45.0f;   // ONE hit at 45x (~+8 dB over the others) about once per 3 s window
        r.runWindow();
        const auto hm = r.loop.hitsMeasure(); const auto prop = r.last();
        std::printf ("  O1 measure: typical %.1f dB, worst %.1f dB over %d blocks | %s\n", hm.typicalDb, hm.worstDb, hm.blocks, prop.toRawUTF8());
        check (hm.blocks >= 20 && std::isfinite (hm.typicalDb) && hm.worstDb >= hm.typicalDb + 3.0f, "O1. the measure separates the single transient (worst) from the typical hits (worst >= typical + 3 dB)", f1 (hm.typicalDb) + " / " + f1 (hm.worstDb));
        check (prop.contains ("limiter working ~") && prop.contains (" dB on the hits (worst peak ") && ! prop.contains ("average"), "O1. the bubble reads \"limiter working ~X dB on the hits (worst peak Y)\"", prop);
        check (! prop.contains ("is as loud as this goes"), "O1. with the hits typically under the Commercial cap the proposal is NOT capped (the single 9 dB transient no longer caps it)", prop);
        check (r.logs.joinIntoString ("\n").contains ("hits: typical ") && r.logs.joinIntoString ("\n").contains ("blocks, worst "), "O1. the EJLoudness log carries both figures (for the re-calibration)", r.logs.joinIntoString (" | ").fromLastOccurrenceOf ("hits: typical", true, false).substring (0, 90));
    }
    {   // hits typically ABOVE the Commercial cap -> capped, with the capped-proposal wording
        Rig r (true); r.setTarget (-8.0f, 0.0); r.prog.peaky = true;
        calibrate (r.proc, r.prog, -18.0f); r.prog.burst = 30.0f;   // the hits AFTER calibration
        r.loop.armFromChain(); r.runWindow();
        const auto hm = r.loop.hitsMeasure(); const auto prop = r.last();
        std::printf ("  O2 measure: typical %.1f dB, worst %.1f dB over %d blocks | %s\n", hm.typicalDb, hm.worstDb, hm.blocks, prop.toRawUTF8());
        check (prop.contains ("is as loud as this goes with the limiter working <=6 dB. Push to -8.0 anyway?") && r.loop.lastPills().joinIntoString ("|").startsWith ("Push it anyway|Leave it"), "O2. hits typically above the cap -> capped with the capped-proposal wording", prop);
    }
    // ---- 21t-k item 3 (28 Sep 2026 ruling): THE LEVEL SENSOR MEASURES THE PLUGIN, NOT THE STAGING ---------
    // Sean's Zip build: pre -15 on EchoJay's own staging, a plugin doing nothing, and the sensor reported
    // "the slot was 15.0 dB quieter out than in" - so the hold tried to put 15 dB back and clamped at +12. The
    // in tap now sits AFTER the pre-trim and the out tap BEFORE the slot output gain, so a unity plugin reads
    // ZERO whatever the staging is.
    std::printf ("== 21t-k item 3: the level sensor measures the PLUGIN ==\n");
    {
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (0.0f);
        calibrate (r.proc, r.prog, -18.0f);
        feed (r.proc, r.prog, 400, false, nullptr, nullptr);
        const auto flat = r.h.getSlotLevels (1);
        check (flat.measured && std::abs (flat.out.shortTermDb - flat.in.shortTermDb) < 0.35f,
               "21t-k 3. a UNITY slot with no staging reads a level change of 0",
               f1 (flat.out.shortTermDb - flat.in.shortTermDb) + " dB");
        // ...and now the staging Sean's build wrote: -15 in, +15 back on the slot's own output gain.
        r.h.setSlotPreTrimDb (1, -15.0f);
        r.h.setSlotOutGainDb (1, 15.0f);
        r.h.resetSlotShortTermStats (1, "the leg set the staging");
        feed (r.proc, r.prog, 600, false, nullptr, nullptr);
        const auto staged = r.h.getSlotLevels (1);
        check (staged.measured && std::abs (staged.out.shortTermDb - staged.in.shortTermDb) < 0.35f,
               "21t-k 3. ...and the SAME unity slot with pre -15 / output +15 still reads 0  (RED as it stood: "
               "\"the slot was 15.0 dB quieter out than in\", which is EchoJay's own staging, not the plugin)",
               f1 (staged.out.shortTermDb - staged.in.shortTermDb) + " dB");
        r.h.setSlotPreTrimDb (1, 0.0f); r.h.setSlotOutGainDb (1, 0.0f);
    }

    std::printf ("== P. 22 Sep 2026 (21m ruling 2): gain staging - each slot's unity trim, measured at Listen, applied inside its blend node ==\n");
    {
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (4.0f);
        check (r.h.getNumSlots() == 3 && r.h.getSlotInfo (1).name == "EchoJay Gain" && std::abs (r.h.getSlotTrimDb (1)) < 0.01f && r.h.slotTrimText (1).isEmpty(),
               "P0. the +4 dB Gain sits between the Level and the limiter with no trim before Listen", r.h.slotTrimText (1));
        const float cal = calibrate (r.proc, r.prog, -18.0f); check (std::abs (cal + 18.0f) < 0.6f, "P0. programme calibrated at the chain input to -18 LUFS", f1 (cal));
        { feed (r.proc, r.prog, 400, false, nullptr, nullptr); const auto lv0 = r.h.getSlotLevels (1);
          check (lv0.measured && std::abs ((lv0.out.shortTermDb - lv0.in.shortTermDb) - 4.0f) < 0.3f, "P0. before Listen the slot adds +4 dB (out - in, short-term)", f1 (lv0.out.shortTermDb - lv0.in.shortTermDb)); }
        check (r.loop.armFromChain() && r.loop.levelSlot() == 0 && r.loop.limiterSlot() == 2, "P0. armed: Level slot 0, limiter slot 2", r.loop.armSource());
        r.runWindow();
        const float trim = r.h.getSlotTrimDb (1);
        check (std::abs (trim + 4.0f) < 0.3f, "P1. after Listen the +4 dB slot carries a -4.0 (+-0.3) dB unity trim", f1 (trim));
        feed (r.proc, r.prog, 400, false, nullptr, nullptr);   // the tallies settle on the trimmed output
        const auto lv = r.h.getSlotLevels (1);
        // SUPERSEDED BY R2 (24 Sep 2026): the match trim is a COMPARE DEVICE. It is not in the path during normal
        // playback, so the slot does NOT end at unity then - it ends where the plugin left it, and the trim is what
        // an A/B would apply. The assertion is inverted here rather than deleted, and the unity claim is re-made
        // below WITH a compare running, which is the only state it was ever true in.
        check (lv.measured && std::abs ((lv.out.shortTermDb - lv.in.shortTermDb) - 4.0f) < 0.3f,
               "P1. ...and with NO compare running the slot still reads its own +4 dB - the match trim is not in the path "
               "(ruling R2, 24 Sep 2026; this leg used to assert unity here)", f1 (lv.out.shortTermDb - lv.in.shortTermDb));
        {   // ...and with a compare running it lands at unity, which is what the trim is for
            r.h.setCompareActive (true);
            feed (r.proc, r.prog, 400, false, nullptr, nullptr);
            const auto lvc = r.h.getSlotLevels (1);
            check (lvc.measured && std::abs (lvc.out.shortTermDb - lvc.in.shortTermDb) < 0.4f,
                   "P1. ...and WITH a compare running the same slot lands at unity (that is what match is for)",
                   f1 (lvc.out.shortTermDb - lvc.in.shortTermDb));
            r.h.setCompareActive (false);
            feed (r.proc, r.prog, 200, false, nullptr, nullptr);
        }
        check (r.h.slotTrimText (1).endsWith (" dB match") && r.h.slotTrimText (1).startsWith ("-") && r.h.getSlotInfo (1).trimText == r.h.slotTrimText (1),
               "P1. the tile and the card line read \"-X.X dB match\" from the same atomic", r.h.slotTrimText (1));
        check (std::abs (r.h.getSlotTrimDb (0)) < 0.01f && std::abs (r.h.getSlotTrimDb (2)) < 0.01f && r.h.slotTrimText (0).isEmpty() && r.h.slotTrimText (2).isEmpty(),
               "P2. the Level slot and the last limiter are exempt (no trim, no text)", f1 (r.h.getSlotTrimDb (0)) + " / " + f1 (r.h.getSlotTrimDb (2)));
        check (r.logs.joinIntoString ("\n").contains ("unity trim: slot 1 EchoJay Gain: in ") && r.logs.joinIntoString ("\n").contains ("post ") && r.logs.joinIntoString ("\n").contains ("unity trims changed: 1"), "P2. the EJLoudness log carries the trim line (21p: the peaks and BOTH trims)", r.logs.joinIntoString (" | ").fromFirstOccurrenceOf ("unity", false, false).substring (0, 160));
        // the loop's opening gain assumes a unity chain: after Go the output lands on the target even though the window measured the un-trimmed chain
        {   // the window measured the UN-trimmed chain (-14 = -18 programme + 4 dB); the proposal must be computed from the unity chain (-18): needed +9, not +5
            juce::String ml; for (const auto& l : r.logs) if (l.contains ("measured: max short-term")) ml = l;
            // SUPERSEDED BY R2 (24 Sep 2026): ONE FIGURE EVERYWHERE. The proposal used to be computed from the
            // window PLUS the trim change; with the match trims out of the path there is nothing to correct for,
            // and the window IS the chain output. The leg now asserts the window is reported as measured.
            check (ml.contains ("measured: max short-term -14.0 LUFS") && ml.contains ("needed +5.0 dB"),
                   "P5. the proposal reports the window as measured, with no trim correction "
                   "(ruling R2, 24 Sep 2026; this leg used to assert -18.0 / +9.0)", ml.substring (0, 170));
            juce::String gc; for (const auto& l : r.logs) if (l.contains ("GR cap")) gc = l; std::printf ("  P5 cap line: %s\n", gc.substring (0, 200).toRawUTF8());
        }
        check (r.loop.state() == LoudnessLoop::State::proposed && r.loop.go(), "P5. the proposal is offered and Go applies it", juce::String ((int) r.loop.state()));
        check (r.levelGain() > 3.5f && r.levelGain() < 7.0f,
               "P5. ...and the Level moves by what THAT figure asks for (ruling R2; it used to be the +9 unity ask)",
               f1 (r.levelGain()));
        feed (r.proc, r.prog, 600, false, nullptr, nullptr);
        { const auto co = r.h.getChainOutLevels(); std::printf ("  P5 chain out after Go: %.2f LUFS-S (Level %+.2f dB)\n", co.shortTermDb, r.levelGain()); }
        // persistence: the trim rides the saved chain like the pre-gain
        const auto slots = r.h.buildChainSlotsVar(); const auto state = r.h.getCachedSlotStatesVar (ChainHost::kApiStateMaxSlotBytes, ChainHost::kApiStateMaxTotalBytes, "guard");
        { auto p2Heap = std::make_unique<EchoJayProcessor>(); auto& p2 = *p2Heap; p2.prepareToPlay (48000.0, 512); auto& h2 = p2.getChainHost(); h2.restoreSavedChain (slots, state); pumpMs (150);   // heap, not main's stack
          check (h2.getNumSlots() == 3 && std::abs (h2.getSlotTrimDb (1) - trim) < 0.01f && ! h2.getSlotKeepLevel (1) && h2.slotTrimText (1) == r.h.slotTrimText (1),
                 "P4. the trim persists across save/reopen (buildChainSlotsVar -> restoreSavedChain)", juce::String (h2.getNumSlots()) + " slots, trim " + f1 (h2.getSlotTrimDb (1))); }
    }
    {   // the keep flag holds
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (4.0f); r.h.setSlotKeepLevel (1, true);
        calibrate (r.proc, r.prog, -18.0f); r.loop.armFromChain(); r.runWindow();
        check (std::abs (r.h.getSlotTrimDb (1)) < 0.01f && r.h.slotTrimText (1) == "level kept" && r.h.getSlotInfo (1).keepLevel, "P3. the keep flag holds: no trim, the text reads \"level kept\"", r.h.slotTrimText (1));
        feed (r.proc, r.prog, 400, false, nullptr, nullptr); const auto lv = r.h.getSlotLevels (1);
        check (lv.measured && std::abs ((lv.out.shortTermDb - lv.in.shortTermDb) - 4.0f) < 0.3f, "P3. ...and the slot still adds its +4 dB", f1 (lv.out.shortTermDb - lv.in.shortTermDb));
        check (r.logs.joinIntoString ("\n").contains ("EchoJay Gain: kept ("), "P3. the log says the slot was kept", r.logs.joinIntoString (" | ").fromFirstOccurrenceOf ("unity", false, false).substring (0, 120));
        const auto slots = r.h.buildChainSlotsVar(); const auto state = r.h.getCachedSlotStatesVar (ChainHost::kApiStateMaxSlotBytes, ChainHost::kApiStateMaxTotalBytes, "guard");
        { auto p2Heap = std::make_unique<EchoJayProcessor>(); auto& p2 = *p2Heap; p2.prepareToPlay (48000.0, 512); auto& h2 = p2.getChainHost(); h2.restoreSavedChain (slots, state); pumpMs (150);   // heap, not main's stack
          check (h2.getNumSlots() == 3 && h2.getSlotKeepLevel (1) && h2.slotTrimText (1) == "level kept", "P4. the keep flag persists across save/reopen", h2.slotTrimText (1)); }
    }
    std::printf ("== Q. 22 Sep 2026 (21m ruling 1, unityChain): the chat / chat-stream body carries \"unityChain\": true while the rack's per-slot trims are active; absent on an empty rack ==\n");
    {
        auto bodyOf = [] (EchoJayProcessor& p, ChainHost& h) { p.getApi().setUnityChain (h.hasActiveTrims()); return EchoJayAPIRequestPin::body (p.getApi(), juce::StringArray { "user" }, juce::StringArray { "make it brighter" }, "You're EchoJay.", {}); };
        { auto pe = std::make_unique<EchoJayProcessor>(); pe->prepareToPlay (48000.0, 512); auto& he = pe->getChainHost();
          const auto b = bodyOf (*pe, he);
          check (he.getNumSlots() == 0 && ! he.hasActiveTrims() && ! b.contains ("unityChain") && b.contains ("\"appVersion\""), "Q1. an EMPTY rack: no trims, the body has NO unityChain field (RED as it stood: the field did not exist either way - compile refusal)", b.substring (0, 120)); }
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (4.0f);
        calibrate (r.proc, r.prog, -18.0f); r.loop.armFromChain();
        { const auto b = bodyOf (r.proc, r.h); check (! r.h.hasActiveTrims() && ! b.contains ("unityChain"), "Q2. a populated rack BEFORE Listen: no trims yet, no field", b.substring (0, 100)); }
        r.runWindow();
        { const auto b = bodyOf (r.proc, r.h); juce::StringArray ul; for (const auto& l : r.logs) if (l.contains ("unity") || l.contains ("measured:")) ul.add (l.substring (0, 90));
          check (r.h.hasActiveTrims() && b.contains ("\"unityChain\":true"), "Q3. after Listen the +4 dB slot carries its trim -> the body carries \"unityChain\":true (both chat and chat-stream build through buildChatRequestBody)",
                 "trim " + f1 (r.h.getSlotTrimDb (1)) + " state " + juce::String ((int) r.loop.state()) + " bypassed " + juce::String ((int) r.h.getSlotInfo (1).bypassed) + " | " + ul.joinIntoString (" || ").substring (0, 300) + " | " + b.fromFirstOccurrenceOf ("\"appVersion\"", false, false).substring (0, 60)); }
        r.h.setSlotBypassed (1, true);
        { const auto b = bodyOf (r.proc, r.h); check (! r.h.hasActiveTrims() && ! b.contains ("unityChain"), "Q4. the trimmed slot bypassed -> no live trim, the field is absent again", b.substring (0, 60)); }
    }
    std::printf ("== V21P. 23 Sep 2026 (21p items 1-4): the validity gate, the per-slot picture, the pre-trim, and the plugin's own log ==\n");
    {   // (1) a reading taken with the transport STOPPED writes nothing, and says so
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (4.0f);
        calibrate (r.proc, r.prog, -18.0f); r.loop.armFromChain();
        r.h.setSlotTrimDb (1, 0.0f);
        r.loop.isPlaying = [] { return false; }; r.loop.transportKnown = [] { return true; };   // the host SAYS it is stopped
        juce::StringArray lines; const int changed = r.h.measureUnityTrims (0, 2, &lines, r.loop.rollingOrUnknown());
        check (changed == 0 && std::abs (r.h.getSlotTrimDb (1)) < 0.01f, "V1. transport stopped: NO trim is written  (RED as it stood: the pass accepted any finite reading)", "changed " + juce::String (changed) + " trim " + f1 (r.h.getSlotTrimDb (1)));
        check (lines.joinIntoString ("|").contains ("NO READING") && lines.joinIntoString ("|").contains ("transport"), "V1. ...and the line says why", lines.joinIntoString (" | ").substring (0, 120));
        check (! r.h.slotPicture (1).valid && r.h.slotPictureText (1) == "no reading", "V1. the slot's picture is \"no reading\", never a floor number", r.h.slotPictureText (1));
    }
    {   // (1b) SILENCE with the transport rolling is still not a reading
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (4.0f);
        r.h.resetAllLevels(); feed (r.proc, r.prog, 400, true, nullptr, nullptr);   // silent blocks
        juce::StringArray lines; const int changed = r.h.measureUnityTrims (0, 2, &lines, true);
        check (changed == 0 && ! r.h.slotPicture (1).valid, "V1b. silence with the transport rolling: no trim, no picture  (RED as it stood: -245 is finite, so a trim was written from it)", "changed " + juce::String (changed) + " | " + lines.joinIntoString (" | ").substring (0, 110));
        check (echojay::ReadingGate::kFloorLufs == -60.0f && echojay::ReadingGate::kFloorTruePeakDb == -60.0f, "V1b. the floors are -60 LUFS-S and -60 dBTP");
        { LoudnessLoop l2 (r.h);
          l2.isPlaying = [] { return false; }; l2.transportKnown = [] { return false; };
          check (l2.rollingOrUnknown(), "V1c. a host that never publishes a transport is UNKNOWN, not stopped: it does not block the loop");
          l2.transportKnown = [] { return true; };
          check (! l2.rollingOrUnknown(), "V1c. ...and a host that SAYS stopped does"); }
    }
    {   // (2) + (3): a real window gives every slot a picture, and a slot driven over -3 dBTP gets a PRE-trim
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (4.0f);
        calibrate (r.proc, r.prog, -18.0f);
        r.prog.amp *= juce::Decibels::decibelsToGain (16.0f);   // drive the chain so slot 1's INPUT sits about -1 dBTP
        r.loop.armFromChain(); r.runWindow();
        const auto pic = r.h.slotPicture (1);
        check (pic.valid && pic.inTpDb > -60.0f && pic.outTpDb > -60.0f, "V2. after Listen the slot carries a picture: input peak, output peak", "in " + f1 (pic.inTpDb) + " out " + f1 (pic.outTpDb) + " dBTP");
        check (r.h.slotPictureText (1).contains ("dBTP"), "V2. ...and the strip line reads it", r.h.slotPictureText (1));
        const auto card = r.h.listenCardLines();
        check (card.size() >= 1 && card.joinIntoString ("|").contains ("EchoJay Gain"), "V2. the Listen card carries one line per slot, by name  (RED as it stood: no per-slot numbers existed)", card.joinIntoString (" | ").substring (0, 150));
        std::printf ("  V2 card: %s\n", card.joinIntoString (" | ").substring (0, 220).toRawUTF8());
        if (pic.inTpDb > ChainHost::kSlotInputCeilingDb)
        {
            check (r.h.getSlotPreTrimDb (1) < -0.05f, "V3. a slot driven over -3 dBTP gets a PRE-trim on its input  (RED as it stood: no pre-trim existed)", "pre " + f1 (r.h.getSlotPreTrimDb (1)) + " dB for an input of " + f1 (pic.inTpDb) + " dBTP");
            check (std::abs ((pic.inTpDb + r.h.getSlotPreTrimDb (1)) - ChainHost::kSlotInputCeilingDb) < 0.35f, "V3. ...sized to land the input ON the ceiling, not below it", f1 (pic.inTpDb + r.h.getSlotPreTrimDb (1)) + " dBTP");
            check (card.joinIntoString ("|").contains ("pre ") && card.joinIntoString ("|").contains ("post "), "V3. ...and BOTH trims are reported per slot", card.joinIntoString (" | ").substring (0, 150));
            check (card.joinIntoString ("|").contains ("over -3 dBTP into this slot"), "V2. a slot over the line is FLAGGED by name in the card", card.joinIntoString (" | ").substring (0, 160));
        }
        else
            check (false, "V3. the fixture drove slot 1 over -3 dBTP", "input was " + f1 (pic.inTpDb) + " dBTP - fixture too quiet");
    }
    // ================= 21s-b: R1, R2, R3 ==========================================================
    {   // R1: a stale trim on an EXEMPT slot is cleared, said out loud, and shown
        std::printf ("\n== R1 (21s-b): the Level and Limiter slots carry no trim, ever ==\n");
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (4.0f);
        calibrate (r.proc, r.prog, -18.0f);
        // the defect, staged: a pre-trim left on the Level slot by an earlier build
        r.h.setSlotPreTrimDb (r.levelSlot, -2.9f);
        r.h.setSlotTrimDb (r.limSlot, 1.7f);
        check (std::abs (r.h.getSlotPreTrimDb (r.levelSlot) + 2.9f) < 0.01f, "R1. fixture: a stale -2.9 dB pre-trim sits on the Level slot");
        juce::StringArray lines;
        r.h.measureUnityTrims (r.levelSlot, r.limSlot, &lines, true);
        const auto joined = lines.joinIntoString (" | ");
        check (std::abs (r.h.getSlotPreTrimDb (r.levelSlot)) < 0.01f,
               "R1. the Level slot's pre-trim is CLEARED  (RED as it stood: it survived every Listen, unseen)",
               f1 (r.h.getSlotPreTrimDb (r.levelSlot)));
        check (joined.contains ("exempt trim cleared: EchoJay Level pre -2.9"),
               "R1. ...and the value found is LOGGED before it is zeroed", joined.substring (0, 150));
        check (joined.contains ("exempt trim cleared") && joined.contains ("post 1.7"),
               "R1. ...the limiter's post trim too");
        check (r.h.slotPictureText (r.levelSlot).contains ("exempt: pre"),
               "R1. ...and an exempt slot's line now SHOWS its pair instead of saying nothing",
               r.h.slotPictureText (r.levelSlot));
        // the arithmetic the stale trim used to break: Level OUT = the slot before it + the Level's gain
        // The arithmetic the stale trim used to break. The Level is the FIRST slot in this fixture, so what feeds
        // it is the chain input - and with the exempt pre-trim cleared, its own gain is the whole difference.
        r.h.resetAllLevels(); r.loop.armFromChain(); r.runWindow();
        auto* lv = dynamic_cast<EedLevelProcessor*> (r.h.getSlotProcessor (r.levelSlot));
        if (lv != nullptr)
        {
            const float inTp   = lv->inputLevels().truePeakDb;
            const float outTp  = lv->outputLevels().truePeakDb;
            const float expect = inTp + (float) lv->gainDb();
            check (std::abs (outTp - expect) < 0.2f,
                   "R1. Level OUT TP = its input + its own gain, within 0.2 dB - nothing hidden in front of it "
                   "(the 2.9 dB that went missing)",
                   "in " + f1 (inTp) + " + gain " + f1 ((float) lv->gainDb()) + " = " + f1 (expect) + ", read " + f1 (outTp));
        }
        else check (false, "R1. fixture: the Level slot is an EchoJay Level");
    }
    {   // R2: ONE figure everywhere, and the match trim is a compare device
        std::printf ("\n== R2 (21s-b): the match trim is compare-only, and the proposal and Done agree ==\n");
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (4.0f);
        check (! r.h.compareActive(), "R2. a compare is not running by default");
        calibrate (r.proc, r.prog, -14.0f);
        r.loop.armFromChain(); r.runWindow();
        const float atProposal = r.loop.lastMeasured();
        juce::String doneLine;
        r.logs.clear();
        const bool doneOk = r.loop.done();
        for (const auto& l : r.logs) if (l.contains ("done:")) doneLine = l;
        check (doneOk && doneLine.isNotEmpty(), "R2. Done reports a figure",
               juce::String (doneOk ? "done ok, " : "done refused, ") + juce::String (r.logs.size()) + " line(s): "
               + r.logs.joinIntoString (" | ").substring (0, 120));
        check (doneLine.contains (juce::String (atProposal, 1)),   // the loop prints one decimal; f1 here is two
               "R2. ...and it is the SAME figure the proposal used  (RED as it stood: -11.3 at the proposal, -14.9 at Done)",
               "proposal " + f1 (atProposal) + " / " + doneLine);
        r.h.setCompareActive (true);
        check (r.h.compareActive(), "R2. the compare flag is the one switch the blend nodes read");
        r.h.setCompareActive (false);
    }
    {   // R3: the loudness pair, and what a dynamics slot is doing
        std::printf ("\n== R3 (21s-b): per-slot loudness in and out, and 'working X dB' ==\n");
        Rig r (false, true, "EJ Test Limiter", true); r.setTarget (-9.0f, 0.0); r.setGainDb (4.0f);
        calibrate (r.proc, r.prog, -18.0f);
        r.loop.armFromChain(); r.runWindow();
        const auto txt = r.h.slotPictureText (1);
        const auto card = r.h.listenCardLines().joinIntoString (" | ");
        check (txt.contains ("LUFS"), "R3. the strip line carries the loudness pair as well as the peaks  (RED as it stood: dBTP only)", txt);
        check (card.contains ("LUFS"), "R3. ...and so does the Listen card", card.substring (0, 200));
        const auto lim = r.h.listenCardLines();
        bool anyWorking = false;
        for (const auto& l : lim) if (l.contains ("working ")) anyWorking = true;
        check (anyWorking || ! r.h.slotPicture (r.limSlot).dynamics,
               "R3. a dynamics-role slot reports how hard it is working (in - out, in LUFS)", card.substring (0, 200));
    }

    {   // (4) the plugin's own rolling log exists and carries the Listen lines
        Rig r2 (false, true, "EJ Test Limiter", true);
        const juce::File f (juce::String (echojay::FileLog::instance().currentPath()));
        EchoJay_NSLog ("EJGuard: 21p item 4 marker");
        check (f.existsAsFile(), "V4. the plugin writes its own log at " + f.getParentDirectory().getFullPathName() + "  (RED as it stood: no such file)", f.getFullPathName());
        const auto text = f.loadFileAsString();
        check (text.contains ("EJGuard: 21p item 4 marker"), "V4. ...and every EchoJay_NSLog line lands in it");
        // the loop's own lines reach this file through logLine -> EchoJay_NSLog, which the editor wires
        // (PluginEditor.cpp: "loop.logLine = [](line) { EchoJay_NSLog(line); }"); THIS harness replaces logLine with
        // its own collector, so the leg proves the route rather than re-proving the collector.
        { LoudnessLoop probeLoop (r2.h); probeLoop.logLine = [] (const juce::String& l) { EchoJay_NSLog (l.toRawUTF8()); };
          probeLoop.logLine ("EJLoudness: slot picture: guard route check");
          const auto after = f.loadFileAsString();
          check (after.contains ("EJLoudness: slot picture: guard route check"), "V4. ...and a loop line routed the way the editor routes it lands in the file", after.substring (juce::jmax (0, after.length() - 120)).replace ("\n", " | ")); }
        check (text.contains ("EJ"), "V4. ...the file is the plugin's own log, not an empty file", juce::String (text.length()) + " bytes");
        check (echojay::FileLog::kFiles == 5 && echojay::FileLog::kMaxBytes == 2L * 1024L * 1024L, "V4. five files of 2 MB", juce::String (echojay::FileLog::kFiles));
    }
    std::printf ("\n==== loudness_loop_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    // ---- 21t-d: the compressor calibration loop --------------------------------------------------------------
    // Driven by the SHIPPED state machine (EJCalibLoop.h, the one both binaries compile), over windows measured
    // from the SHIPPED synthetic compressor: its GR rises with input above a fixed knee, which is the property
    // the drive steps depend on. A window is handed in as the host would compute it - slot-in LUFS minus
    // slot-out LUFS - so what is under test is the loop's decisions, not a re-description of them.
    {
        std::printf ("\n== 21t-d: the calibration loop drives to the band, and says what it did ==\n");
        // GR as a function of drive, measured from the real processor: the same knee/ratio the plugin applies.
        auto grAtDrive = [] (float driveDb)
        {
            EJTestCompressor comp;
            comp.prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> b (2, 4800);
            juce::Random rng (77);
            const float baseDb = -26.0f;                       // quiet enough that 0 dB drive works ~0 dB
            const float amp = juce::Decibels::decibelsToGain (baseDb + driveDb);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i)
                    b.setSample (ch, i, amp * (rng.nextFloat() * 2.0f - 1.0f));
            auto rms = [&b] { double s = 0.0; for (int i = 0; i < b.getNumSamples(); ++i) { const double v = b.getSample (0, i); s += v * v; }
                              return juce::Decibels::gainToDecibels ((float) std::sqrt (s / b.getNumSamples())); };
            const float inDb = rms();
            juce::MidiBuffer m; comp.processBlock (b, m);
            const float outDb = rms();
            return inDb - outDb;   // GR = in - out, the same figure the slot tallies give
        };
        check (grAtDrive (0.0f) < 1.0f && grAtDrive (12.0f) > grAtDrive (0.0f) + 2.0f,
               "21t-d. fixture: the synthetic compressor's GR RISES with drive",
               juce::String (grAtDrive (0.0f), 2) + " dB at 0, " + juce::String (grAtDrive (12.0f), 2) + " dB at +12");

        {   // (1) it drives to the band and ENDS there - the RED is a drive that never moves and a card that
            //     never leaves "Listening".
            echojay::CalibLoop loop;
            loop.begin ("EJ Test Compressor", 0, 2.0f, 3.0f, 0.0f);
            check (loop.card() == "Listening... play the loudest part",
                   "21t-d (1). it opens on \"Listening... play the loudest part\"", loop.card());
            juce::StringArray logs; bool done = false; float drive = 0.0f; juce::String closing;
            for (int w = 0; w < 30 && ! done; ++w)
            {
                echojay::CalibLoop::Window win; win.measured = true; win.silent = false; win.grDb = grAtDrive (drive);
                const auto st = loop.onWindow (win, 3000.0);
                logs.add (st.logLine);
                if (st.writeDrive) { drive = st.newPre; check (std::abs (st.newPost + st.newPre) < 1.0e-4f, "21t-d (1). the post-trim mirrors the drive", juce::String (st.newPre, 1) + " / " + juce::String (st.newPost, 1)); }
                if (st.finished) { done = true; closing = st.closing; }
            }
            check (done, "21t-d (1). the loop ENDS", juce::String (logs.size()) + " window(s)");
            check (drive > 0.5f,
                   "21t-d (1). ...by moving the drive, 1 dB at a time  (RED as it stood: nothing moved it)",
                   juce::String (drive, 1) + " dB");
            check (loop.state == echojay::CalibLoop::State::Adjusted,
                   "21t-d (1). ...and it ends ADJUSTED, in the band", juce::String ((int) loop.state));
            check (loop.lastGr >= 2.0f && loop.lastGr <= 3.0f,
                   "21t-d (1). ...with the last measured GR inside 2-3 dB", juce::String (loop.lastGr, 2));
            check (loop.card().contains ("working"),
                   "21t-d (1). ...and the card left \"Listening\" for a live figure", loop.card());
            check (closing.startsWith ("Adjusted the EJ Test Compressor to ") && closing.contains ("working"),
                   "21t-d (1). the closing names what was adjusted in ONE clause, then asks about the chain", closing);
            check (! closing.contains ("More") && ! closing.contains ("Less") && ! closing.contains ("Fine"),
                   "21t-d (1). ...and offers no pills - the user answers in words");
            check (logs[0].startsWith ("EJThreshold: \"EJ Test Compressor\" window 1 gr=") && logs[0].contains ("state="),
                   "21t-d (1). every window logs gr, pre, post and state", logs[0]);
            // never the same closing question twice running
            echojay::CalibLoop l2 = loop; l2.state = echojay::CalibLoop::State::Adjusted;
            const auto q1 = loop.closingMessage(), q2 = loop.closingMessage();
            check (q1.fromLastOccurrenceOf (". ", false, false) != q2.fromLastOccurrenceOf (". ", false, false),
                   "21t-d (1). ...and the question is never the same one twice running",
                   q1.fromLastOccurrenceOf (". ", false, false) + " | " + q2.fromLastOccurrenceOf (". ", false, false));
        }

        {   // (2) no audio: it pauses at 30 s and resumes, and nothing is written while it waits
            std::printf ("\n== 21t-d (2): silence pauses the loop at 30 s, and it resumes ==\n");
            echojay::CalibLoop loop;
            loop.begin ("EJ Test Compressor", 0, 2.0f, 3.0f, 0.0f);
            bool wrote = false;
            for (int w = 0; w < 9; ++w)   // 9 x 3 s = 27 s: not yet
            {
                echojay::CalibLoop::Window win; win.measured = true; win.silent = true;
                const auto st = loop.onWindow (win, 3000.0);
                if (st.writeDrive) wrote = true;
            }
            check (loop.state == echojay::CalibLoop::State::Listening,
                   "21t-d (2). 27 s of silence is not yet a pause", juce::String ((int) loop.state));
            {
                echojay::CalibLoop::Window win; win.measured = true; win.silent = true;
                const auto st = loop.onWindow (win, 3000.0);
                check (loop.state == echojay::CalibLoop::State::Waiting && st.card.startsWith ("Waiting for playback"),
                       "21t-d (2). at 30 s the card says \"Waiting for playback - play the loudest part of this "
                       "channel\"", st.card);
                check (st.logLine.contains ("state=waiting"), "21t-d (2). ...and the log says waiting", st.logLine);
            }
            check (! wrote, "21t-d (2). nothing was written while it waited");
            const float driveBefore = loop.preDb; const int stepsBefore = loop.steps;
            {   // signal returns
                echojay::CalibLoop::Window win; win.measured = true; win.silent = false; win.grDb = 0.2f;
                const auto st = loop.onWindow (win, 3000.0);
                check (loop.state != echojay::CalibLoop::State::Waiting,
                       "21t-d (2). ...and it RESUMES when signal returns", st.card);
                check (loop.preDb != driveBefore || loop.steps == stepsBefore + 1 || st.writeDrive,
                       "21t-d (2). ...and starts working again on the first real window",
                       juce::String (loop.steps) + " step(s)");
            }
        }

        {   // (3) the clamp: the band cannot be reached by drive alone, and the line says so honestly
            std::printf ("\n== 21t-d (3): the drive limit ends it with the figure it measured ==\n");
            echojay::CalibLoop loop;
            loop.begin ("EJ Test Compressor", 0, 2.0f, 3.0f, 11.0f);   // one step from the +12 limit
            juce::String closing; bool done = false;
            for (int w = 0; w < 20 && ! done; ++w)
            {
                echojay::CalibLoop::Window win; win.measured = true; win.silent = false; win.grDb = 0.4f;   // never reaches the band
                const auto st = loop.onWindow (win, 3000.0);
                if (st.finished) { done = true; closing = st.closing; }
            }
            check (done && loop.state == echojay::CalibLoop::State::Clamped,
                   "21t-d (3). it ends CLAMPED rather than driving past +/-12", juce::String (loop.preDb, 1) + " dB");
            check (std::abs (loop.preDb) <= 12.0f + 1.0e-4f,
                   "21t-d (3). ...and the drive never left the limit", juce::String (loop.preDb, 1));
            check (closing.contains ("could not get") && closing.contains ("0.4") && closing.contains ("band"),
                   "21t-d (3). ...and the line names the figure it MEASURED, not one it did not", closing);
        }

        {   // (6) A TUNER-ONLY BUILD LEAVES THE GAIN ALONE AND STARTS NO LOOP (21t-e ruling)
            std::printf ("\n== 21t-e: a chain with no compressor gets no drive and no loop ==\n");
            // The loop's own entry point is the only thing that can start it, and it is asked about a chain that
            // has no dynamics slot in it. Nothing about the fixture is a stand-in: this is the shipped struct.
            echojay::CalibLoop none;
            check (! none.active() && ! none.running(),
                   "21t-e. a loop that was never begun is not running", juce::String ((int) none.state));
            // ...and one that IS begun for a compressor is - so the difference is the chain, not the code path.
            echojay::CalibLoop comp;
            comp.begin ("EJ Test Compressor", 0, 2.0f, 3.0f, 0.0f);
            check (comp.running() && comp.plugin == "EJ Test Compressor",
                   "21t-e. ...while a compressor's loop is", comp.plugin);
            // The retirement itself is a V2-side fact (no compose-time pre-gain), asserted where it lives: the
            // ui_guard leg "a tuner-only build applies no pre-gain". Stated here so the two are not read as one.
        }

        {   // (5) HEADROOM: the drive limit is min(+12, the drive that brings the input to -3 dBTP)
            std::printf ("\n== 21t-d (5): the drive stops where the INPUT runs out of headroom ==\n");
            echojay::CalibLoop loop;
            loop.begin ("EJ Test Compressor", 0, 2.0f, 3.0f, 0.0f);
            // The fixture's slot input sits at -9 dBTP at 0 dB of drive, and it RISES WITH THE DRIVE, because a
            // pre-gain is what the drive is. -3 is the ceiling, so +6 dB is the last drive that does not clip it.
            float drive = 0.0f; juce::String closing; bool done = false;
            for (int w = 0; w < 30 && ! done; ++w)
            {
                echojay::CalibLoop::Window win;
                win.measured = true; win.silent = false;
                win.grDb = 0.5f;                       // never reaches the band, so it keeps asking for more drive
                win.inTruePeakDb = -9.0f + drive;      // the input follows the drive, as it does in the rack
                const auto st = loop.onWindow (win, 3000.0);
                if (st.writeDrive) drive = st.newPre;
                if (st.finished) { done = true; closing = st.closing; }
            }
            check (done && std::abs (drive - 6.0f) < 0.01f,
                   "21t-d (5). it stops at +6 dB - the drive that puts a -9 dBTP input at the -3 ceiling  "
                   "(RED as it stood: it drove to the step budget and clipped the input on the way)",
                   juce::String (drive, 1) + " dB");
            check (closing.contains ("band not reached - drive limited by headroom at +6.0 dB, working 0.5 dB"),
                   "21t-d (5). ...and says so in the ruled words", closing);
            check (loop.state == echojay::CalibLoop::State::Clamped && loop.headroomStopped,
                   "21t-d (5). ...as a HEADROOM stop, not a step-budget one");
            // With headroom to spare the limit is the ordinary +12 again.
            echojay::CalibLoop loop2;
            loop2.begin ("EJ Test Compressor", 0, 2.0f, 3.0f, 0.0f);
            float drive2 = 0.0f; bool done2 = false;
            for (int w = 0; w < 30 && ! done2; ++w)
            {
                echojay::CalibLoop::Window win;
                win.measured = true; win.silent = false; win.grDb = 0.5f;
                win.inTruePeakDb = -30.0f + drive2;    // acres of headroom
                const auto st = loop2.onWindow (win, 3000.0);
                if (st.writeDrive) drive2 = st.newPre;
                if (st.finished) done2 = true;
            }
            check (done2 && ! loop2.headroomStopped && loop2.steps == echojay::CalibLoop::kMaxSteps,
                   "21t-d (5). ...and with headroom to spare it is the step budget that ends it, as before",
                   juce::String (drive2, 1) + " dB after " + juce::String (loop2.steps) + " step(s)");
        }

        {   // (4) the handover: the step count survives the move to the other host
            std::printf ("\n== 21t-d (4): a handover continues the loop, it does not restart it ==\n");
            echojay::CalibLoop loop;
            loop.begin ("EJ Test Compressor", 0, 2.0f, 3.0f, 0.0f);
            for (int w = 0; w < 4; ++w)
            {
                echojay::CalibLoop::Window win; win.measured = true; win.silent = false; win.grDb = 0.3f;
                loop.onWindow (win, 3000.0);
            }
            const int stepsAtHandover = loop.steps; const float driveAtHandover = loop.preDb;
            check (stepsAtHandover > 0, "21t-d (4). fixture: the loop has already taken steps", juce::String (stepsAtHandover));
            const auto hl = loop.onHandover();
            check (hl.contains ("state=handover"), "21t-d (4). the handover is logged", hl);
            check (loop.steps == stepsAtHandover && std::abs (loop.preDb - driveAtHandover) < 1.0e-4f,
                   "21t-d (4). ...with the step count and the drive UNCHANGED",
                   juce::String (loop.steps) + " step(s), " + juce::String (loop.preDb, 1) + " dB");
            {   // the first window on the new host is seen and NOT judged
                echojay::CalibLoop::Window win; win.measured = true; win.silent = false; win.grDb = 0.3f;
                const auto st = loop.onWindow (win, 3000.0);
                check (! st.writeDrive && loop.steps == stepsAtHandover,
                       "21t-d (4). ...and the first window on the new host is not judged - it is that host's first",
                       juce::String (loop.steps));
            }
            {   // the next one is
                echojay::CalibLoop::Window win; win.measured = true; win.silent = false; win.grDb = 0.3f;
                const auto st = loop.onWindow (win, 3000.0);
                check (st.writeDrive && loop.steps == stepsAtHandover + 1,
                       "21t-d (4). ...and the one after it continues the count",
                       juce::String (loop.steps));
            }
        }

        {   // a dropped window is not a measurement
            std::printf ("\n== 21t-d: a window with dropped frames is not a measurement ==\n");
            echojay::CalibLoop loop;
            loop.begin ("EJ Test Compressor", 0, 2.0f, 3.0f, 0.0f);
            echojay::CalibLoop::Window bad; bad.measured = false; bad.silent = false; bad.grDb = 0.0f;
            const auto st = loop.onWindow (bad, 3000.0);
            check (! st.writeDrive && loop.steps == 0 && loop.noSignalMs == 0.0,
                   "21t-d. a dropped window advances nothing, and is not counted as silence",
                   juce::String (loop.steps) + " step(s), " + juce::String (loop.noSignalMs, 0) + " ms");
            echojay::CalibLoop::Window badQuiet; badQuiet.measured = false; badQuiet.silent = true;
            loop.onWindow (badQuiet, 3000.0);
            check (loop.noSignalMs == 3000.0,
                   "21t-d. ...but a dropped window that was SILENT does count toward the 30 s clock",
                   juce::String (loop.noSignalMs, 0) + " ms");
        }
    }

    // ---- 21t-g (6): PASSIVE IS THE DEFAULT, AND IT IS QUIET ----------------------------------------------
    // The whole point of the revision: by the time a user asks for a compressor the client has usually been
    // keeping figures for minutes, so the loop should set the thing and say nothing until it is done. These legs
    // drive the real CalibLoop - the same header both binaries compile - and assert what the USER would see.
    {
        std::printf ("\n== 21t-g (6): a passive loop shows no card, asks for nothing, and reports once ==\n");
        auto inBand  = [] { echojay::CalibLoop::Window w; w.measured = true; w.grDb = 2.5f; w.inTruePeakDb = -12.0f; return w; };
        auto tooLittle = [] { echojay::CalibLoop::Window w; w.measured = true; w.grDb = 0.4f; w.inTruePeakDb = -12.0f; return w; };

        // (a) PASSIVE, out of band: it steps, and never says a word while it works.
        {
            echojay::CalibLoop::Config cfg;
            cfg.plugin = "EJ Test Compressor"; cfg.slot = 1; cfg.lo = 2.0f; cfg.hi = 3.0f;
            cfg.mode = echojay::CalibLoop::Mode::Passive;      // the default; stated here because it is the subject
            cfg.startDb = 0.0f;
            echojay::CalibLoop loop; loop.begin (cfg);
            check (loop.card().isEmpty(), "21t-g (6a). a passive loop draws NO card before it has measured anything",
                   loop.card().isEmpty() ? juce::String ("(silent)") : loop.card());
            juce::String anyCard;
            for (int i = 0; i < 3; ++i)
            {
                const auto st = loop.onWindow (tooLittle(), 3000.0);
                if (st.card.isNotEmpty()) anyCard = st.card;
                if (loop.card().isNotEmpty()) anyCard = loop.card();
            }
            check (anyCard.isEmpty(),
                   "21t-g (6a). ...and none while it steps - no \"Listening\", no \"play the loudest part\"",
                   anyCard.isEmpty() ? juce::String ("(silent)") : anyCard);
            // 28 Sep 2026 supersedes the 21t-i reading of this leg: THE BUILD LANDS IT. Passive steps again, but
            // only as the tail of the build and only inside the settle's budget - at most three steps - and it
            // still does all of it without a card or a prompt, which is what the two assertions above pin.
            check (loop.steps >= 1 && loop.steps <= echojay::CalibLoop::kSettleMaxSteps
                   && loop.mode == echojay::CalibLoop::Mode::Passive
                   && std::abs (loop.preDb - (cfg.startDb + (float) loop.steps)) < 0.001f,
                   "21t-g (6a) as re-ruled twice. ...and it steps TOWARD THE BAND, one dB at a time and never "
                   "more than the settle's three, silently",
                   juce::String (loop.steps) + " step(s) of at most "
                   + juce::String (echojay::CalibLoop::kSettleMaxSteps)
                   + ", drive " + juce::String (loop.preDb, 1));
            // Silence in passive is silent too: no "Waiting for playback" prompt.
            echojay::CalibLoop::Window quiet; quiet.measured = true; quiet.silent = true;
            for (int i = 0; i < 11; ++i) loop.onWindow (quiet, 3000.0);   // past the 30 s clock
            check (loop.card().isEmpty(),
                   "21t-g (6a). ...and 30 s of silence asks for nothing either (the listen pass would prompt)",
                   loop.card().isEmpty() ? juce::String ("(silent)") : loop.card());
            // 21t-i: THERE IS NO CLOSING LINE IN PASSIVE. Two in-band windows used to end the loop and post
            // "Adjusted the ... drive to N dB"; the ruling replaced that with the measure-and-ask question, and the
            // loop does not end at all - it waits for the user. Both facts are asserted.
            const auto s1 = loop.onWindow (inBand(), 3000.0);
            const auto s2 = loop.onWindow (inBand(), 3000.0);
            const auto s3 = loop.onWindow (inBand(), 3000.0);
            const juce::String closing = s1.closing + s2.closing + s3.closing;
            check (closing.isEmpty() && loop.closingMessage().isEmpty(),
                   "21t-g (6a) as re-ruled. ...and it never posts a closing line",
                   closing.isEmpty() ? juce::String ("(none)") : closing);
            check (loop.running(),
                   "21t-g (6a) as re-ruled. ...and it does not close itself: the next word is the user's",
                   loop.running() ? juce::String ("still listening") : juce::String ("CLOSED"));
        }

        // (b) PASSIVE and already in band: nothing moved, so nothing is said at all.
        {
            echojay::CalibLoop::Config cfg;
            cfg.plugin = "EJ Test Compressor"; cfg.slot = 1; cfg.lo = 2.0f; cfg.hi = 3.0f;
            echojay::CalibLoop loop; loop.begin (cfg);
            juce::String said;
            for (int i = 0; i < 3; ++i) { const auto st = loop.onWindow (inBand(), 3000.0); said += st.closing; }
            // 21t-i: a compressor already in band is the ordinary case, and it is now also the case the ruling
            // cares most about: nothing moves, and the user is TOLD what it is doing rather than left guessing.
            check (loop.running() && loop.steps == 0,
                   "21t-g (6b) as re-ruled. a compressor already in band moves nothing and stays measuring",
                   juce::String (loop.steps) + " step(s), " + (loop.running() ? "running" : "CLOSED"));
            check (said.isEmpty() && loop.closingMessage().isEmpty(),
                   "21t-g (6b). ...and there is no closing line - the only line it ever posts is the question",
                   said.isEmpty() ? juce::String ("(silent)") : said);
            check (loop.askOwed.isNotEmpty() && loop.askOwed.contains ("gain reduction"),
                   "21t-g (6b) as re-ruled. ...and after two judged windows it says what it measured and asks",
                   loop.askOwed);
        }

        // (c) LISTEN is exactly what it was: a card, a prompt, and a closing line with a question.
        {
            echojay::CalibLoop::Config cfg;
            cfg.plugin = "EJ Test Compressor"; cfg.slot = 1; cfg.lo = 2.0f; cfg.hi = 3.0f;
            cfg.mode = echojay::CalibLoop::Mode::Listen;
            echojay::CalibLoop loop; loop.begin (cfg);
            check (loop.card().contains ("Listening"),
                   "21t-g (6c). a LISTEN loop still asks for playback, word for word as before", loop.card());
            loop.onWindow (tooLittle(), 3000.0);
            loop.onWindow (tooLittle(), 3000.0);
            const auto a = loop.onWindow (inBand(), 3000.0);
            const auto b = loop.onWindow (inBand(), 3000.0);
            const auto c2 = loop.onWindow (inBand(), 3000.0);
            const juce::String closing = a.closing + b.closing + c2.closing;
            check (closing.contains ("Adjusted the") && closing.containsChar ('?'),
                   "21t-g (6c). ...and its closing line still ends in a question about the chain", closing);
        }

        // (d) THE THRESHOLD ACTUATOR: the named control moves, in the sense the profile gave, and a param ARRAY
        // moves together. The drive is left exactly where staging put it.
        {
            echojay::CalibLoop::Config cfg;
            cfg.plugin = "UnFairchild 670M II"; cfg.slot = 0; cfg.lo = 2.0f; cfg.hi = 3.0f;
            cfg.actuator = echojay::CalibLoop::Actuator::Threshold;
            // 21t-i: LISTEN, because automatic stepping is what this leg is about and LISTEN is where the ruling
            // keeps it. The passive side of the same actuator is asserted below, and in the 21t-i legs.
            cfg.mode = echojay::CalibLoop::Mode::Listen;
            cfg.params.add ("Thresh L"); cfg.params.add ("Thresh R");   // paired: stepped as one
            cfg.senseSign = -1;                                          // lower_is_harder, the dB case
            cfg.startDb = -18.0f; cfg.minDb = -40.0f; cfg.maxDb = 0.0f;
            echojay::CalibLoop loop; loop.begin (cfg);
            const auto st = loop.onWindow (tooLittle(), 3000.0);
            check (st.writeParams && ! st.writeDrive
                   && st.paramNames.size() == 2 && std::abs (st.paramValue - (-19.0f)) < 0.001f,
                   "21t-g (6d). too little reduction LOWERS the threshold by 1 dB - both sides of the pair, and "
                   "the drive is not touched",
                   st.paramNames.joinIntoString (" + ") + " = " + juce::String (st.paramValue, 1)
                   + " / writeDrive " + (st.writeDrive ? "yes" : "no"));
            // ...and the other direction raises it.
            echojay::CalibLoop::Window tooMuch; tooMuch.measured = true; tooMuch.grDb = 7.0f; tooMuch.inTruePeakDb = -12.0f;
            loop.onWindow (tooLittle(), 3000.0);          // the fresh window after a move is not judged
            const auto up = loop.onWindow (tooMuch, 3000.0);
            check (up.writeParams && up.paramValue > st.paramValue,
                   "21t-g (6d). ...and too MUCH reduction raises it again, by the same step",
                   juce::String (st.paramValue, 1) + " -> " + juce::String (up.paramValue, 1));
            // The profile's range is the limit, and hitting it ends the loop saying the band was not reached.
            echojay::CalibLoop::Config edge = cfg;
            edge.startDb = -39.5f;                        // one step from the floor
            echojay::CalibLoop loop2; loop2.begin (edge);
            const auto e1 = loop2.onWindow (tooLittle(), 3000.0);
            check (e1.finished && ! e1.writeParams,
                   "21t-g (6d). ...and the profile's own range stops it rather than dialling past the control",
                   juce::String (e1.finished ? "finished" : "still running")
                   + ", threshold left at " + juce::String (loop2.value, 1));
            check (e1.closing.contains ("could not get") && e1.closing.contains ("UnFairchild 670M II"),
                   "21t-g (6d). ...and a LISTEN pass says the band was not reached, and names the figure it did "
                   "measure", e1.closing.substring (0, 110));
            // 21t-i: THE SAME RANGE, IN PASSIVE, binds the one step a comparative buys - and binding it moves
            // nothing at all, rather than ending a loop that in this mode does not end.
            {
                echojay::CalibLoop::Config edgeP = edge; edgeP.mode = echojay::CalibLoop::Mode::Passive;
                echojay::CalibLoop loop4; loop4.begin (edgeP);
                loop4.onWindow (tooLittle(), 3000.0);
                loop4.onWindow (tooLittle(), 3000.0);
                loop4.retarget (5.0f, 6.0f);                 // the user asked for more; the floor is one step away
                int writes = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const auto st4 = loop4.onWindow (tooLittle(), 3000.0);
                    if (st4.writeParams || st4.writeDrive) ++writes;
                }
                // 28 Sep 2026: the settle's own first step already took the half dB the floor allows and CLAMPED
                // there, so the comparative that follows moves nothing at all. Both halves are the point: the
                // profile's floor binds automatic movement and the user's word alike, and neither ends the loop.
                check (writes == 0 && std::abs (loop4.value - (-40.0f)) < 0.001f && loop4.running(),
                       "21t-i (6d) as re-ruled. ...in passive the floor CLAMPS the settle, and the comparative "
                       "that follows moves nothing rather than dialling past the control",
                       juce::String (writes) + " write(s) after the comparative, threshold "
                       + juce::String (loop4.value, 1) + ", " + (loop4.running() ? "running" : "CLOSED"));
                check (loop4.closingMessage().isEmpty(),
                       "21t-i (6d). ...and it still posts no closing line", loop4.closingMessage());
            }
            // A handover carries the knob: the state that rides the sidecar must not turn a threshold pass into a
            // drive pass halfway through.
            const auto back = echojay::CalibLoop::fromVar (loop.toVar());
            check (back.actuator == echojay::CalibLoop::Actuator::Threshold
                   && back.params.size() == 2 && back.mode == echojay::CalibLoop::Mode::Listen
                   && std::abs (back.value - loop.value) < 0.001f,
                   "21t-g (6d). ...and the mode, the knob and its value survive the sidecar round trip",
                   back.params.joinIntoString (" + ") + " @ " + juce::String (back.value, 1)
                   + (back.mode == echojay::CalibLoop::Mode::Passive ? " passive" : " listen"));
        }
    }

    // ---- 21t-g (6b): THE WIRE SHAPE, FROM JSON TEXT, WITH THE SERVER'S LITERALS --------------------------
    // The block is parsed by ONE function both binaries compile (CalibLoop::configFromBlock), so this is where the
    // contract is asserted: the literals B emits and nothing looser, param as a string AND as an array, start_db
    // present and null, and the threshold-with-no-sense violation that must fall back to the drive.
    {
        std::printf ("\n== 21t-g (6b): the calibration block's wire shape, parsed from JSON text ==\n");
        auto parse = [] (const char* json, int numSlots, echojay::CalibLoop::Config& cfg, juce::String& why)
        {
            const auto v = juce::JSON::parse (juce::String (json));
            return echojay::CalibLoop::configFromBlock (v, numSlots, false, "UnFairchild 670M II", cfg, why);
        };

        // (i) the full threshold shape, param as a STRING
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = parse (R"({"source":"tally","heard_s":120,"measure":"short90","mode":"passive",
                                       "actuator":"threshold","slot":1,"param":"Thresh","start_db":-13.4,
                                       "sense":"lower_is_harder","min_db":-15,"max_db":15,"gr_target_db":[2,3]})",
                                   2, c, why);
            check (ok && c.startFromBlock,
                   "21t-j. ...and a block that DID carry start_db says so, which is the only thing the "
                   "\"a block carrying start_db is the step\" rule may consult",
                   c.startFromBlock ? "carried" : "NOT carried");
            check (ok && c.mode == echojay::CalibLoop::Mode::Passive
                   && c.actuator == echojay::CalibLoop::Actuator::Threshold
                   && c.params.size() == 1 && c.params[0] == "Thresh" && c.senseSign == -1
                   && std::abs (c.startDb - (-13.4f)) < 0.01f && c.slot == 0
                   && std::abs (c.minDb - (-15.0f)) < 0.01f && std::abs (c.maxDb - 15.0f) < 0.01f
                   && std::abs (c.lo - 2.0f) < 0.01f && std::abs (c.hi - 3.0f) < 0.01f && why.isEmpty(),
                   "21t-g (6b). mode \"passive\", actuator \"threshold\", param \"Thresh\", sense "
                   "\"lower_is_harder\", start_db -13.4, min/max, gr_target_db - all accepted, nothing flagged",
                   c.params.joinIntoString (",") + " @ " + juce::String (c.startDb, 1) + " slot "
                   + juce::String (c.slot) + (why.isEmpty() ? juce::String() : " why: " + why));
        }
        // (ii) param as an ARRAY - a pair, start_db applying to both
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = parse (R"({"mode":"passive","actuator":"threshold","slot":1,
                                       "param":["Input L","Input R"],"start_db":-18.2,
                                       "sense":"higher_is_harder","gr_target_db":[2,3]})", 2, c, why);
            // 28 Sep 2026 (21t-j): THE KIND DECIDES THE SENSE. This block says actuator "threshold", so the step
            // is lower-is-harder whatever the wire's `sense` claims; the contradiction is NAMED in the log and not
            // followed. What survives from the old reading is the pair: two params, one start_db.
            check (ok && c.params.size() == 2 && c.params[0] == "Input L" && c.params[1] == "Input R"
                   && c.senseSign == -1 && std::abs (c.startDb - (-18.2f)) < 0.01f
                   && why.contains ("contradicts actuator"),
                   "21t-g (6b) as re-ruled. param as an ARRAY is a pair and start_db applies to every entry; a "
                   "\"sense\" that contradicts the actuator KIND is named and not followed",
                   c.params.joinIntoString (" + ") + " @ " + juce::String (c.startDb, 1)
                   + " sense " + juce::String (c.senseSign)
                   + (why.isEmpty() ? juce::String (" (nothing flagged)") : " why: " + why.trim()));
        }
        // (iii) start_db NULL - the server heard under 30 s and set nothing: nothing is written
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = parse (R"({"mode":"listen","actuator":"threshold","slot":1,"param":"Thresh",
                                       "start_db":null,"sense":"lower_is_harder","heard_s":12,
                                       "measure":"int","source":"listen","gr_target_db":[2,3]})", 2, c, why);
            check (ok && c.mode == echojay::CalibLoop::Mode::Listen && ! (c.startDb == c.startDb) && why.isEmpty(),
                   "21t-g (6b). start_db null on a LISTEN block parses, and leaves the value unset (NaN) so the "
                   "opening write is skipped",
                   juce::String (c.mode == echojay::CalibLoop::Mode::Listen ? "listen" : "passive")
                   + ", start " + (c.startDb == c.startDb ? juce::String (c.startDb, 1) : juce::String ("unset")));
        }
        // (iv) actuator "threshold" with sense null. 28 Sep 2026 (21t-j): NOT a violation any more - the KIND
        // supplies the direction, so a threshold stays a threshold and the log says where the sense came from.
        // Before the general compressor rule this fell back to the drive, which is what wrote Input -22.2.
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = parse (R"({"mode":"passive","actuator":"threshold","slot":1,"param":"Thresh",
                                       "start_db":-13.4,"sense":null,"gr_target_db":[2,3]})", 2, c, why);
            check (ok && c.actuator == echojay::CalibLoop::Actuator::Threshold
                   && c.params.size() == 1 && c.params[0] == "Thresh"
                   && c.senseSign == -1 && why.contains ("from the kind"),
                   "21t-g (6b) as re-ruled. threshold with sense NULL stays a THRESHOLD - lower-is-harder comes "
                   "from the kind - and the log says so rather than the direction being guessed",
                   juce::String (c.params.joinIntoString (" + ")) + " sense " + juce::String (c.senseSign)
                   + " why: " + why.trim().substring (0, 110));
        }
        // (iv-b) actuator "drive" with start_db NULL - an unprofiled compressor in passive mode. The parser must
        // leave the value UNSET (NaN), never 0, so the caller opens from the slot_pre_gain_db staging already
        // written and steps from there; opening at 0 would undo the staging in one move.
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = parse (R"({"mode":"passive","actuator":"drive","slot":1,"start_db":null,
                                       "sense":null,"gr_target_db":[2,3]})", 2, c, why);
            check (ok && c.actuator == echojay::CalibLoop::Actuator::Drive && ! (c.startDb == c.startDb)
                   && ! c.startFromBlock,
                   "21t-g (6b/c). drive with start_db null leaves the opening value UNSET and says the BLOCK did "
                   "not carry one, so the caller can open from the staging without that substitution being read "
                   "later as a move the server already made",
                   juce::String (c.startDb == c.startDb ? juce::String (c.startDb, 2) : juce::String ("unset"))
                   + (why.isEmpty() ? juce::String() : ", why: " + why.trim()));
            // ...and the loop, begun from the staged value, moves FROM THERE. 28 Sep 2026: the settle moves it
            // first, as the tail of the build, and a comparative moves it again afterwards - both from the
            // staging. Opening at 0 would undo the staging in one move, which is what this leg has always been
            // about; what changed is only WHO takes the first step.
            echojay::CalibLoop::Config staged = c; staged.startDb = 4.0f;    // what staging wrote on the slot
            echojay::CalibLoop loop; loop.begin (staged);
            float heard = 90.0f;
            auto next = [&heard]
            {
                echojay::CalibLoop::Window w; w.measured = true; w.grDb = 0.4f; w.inTruePeakDb = -12.0f;
                heard += 3.0f; w.heardSeconds = heard;   // heard time must advance or the window is stale
                return w;
            };
            const auto first = loop.onWindow (next(), 3000.0);
            check (first.writeDrive && std::abs (first.newPre - 5.0f) < 0.01f
                   && std::abs (loop.preDb - 5.0f) < 0.01f,
                   "21t-i (6b/c) as re-ruled. the settle's FIRST step moves one dB from the staging, never from "
                   "zero", juce::String (first.newPre, 1) + " dB");
            // Let the settle spend its budget and land.
            for (int i = 0; i < 20 && ! loop.landed; ++i) loop.onWindow (next(), 3000.0);
            const float settled = loop.preDb;
            check (loop.landed && settled > 4.0f + 0.5f && loop.steps <= echojay::CalibLoop::kSettleMaxSteps,
                   "21t-i (6b/c) as re-ruled. ...and the settle lands having stepped UP FROM THE STAGING, inside "
                   "its budget, never from zero",
                   juce::String (loop.steps) + " step(s), 4.0 -> " + juce::String (settled, 1) + " dB, "
                   + (loop.landed ? "landed" : "STILL SETTLING"));
            loop.retarget (5.0f, 6.0f);                  // the user said "more"
            int writes = 0; float after = settled;
            for (int i = 0; i < 20; ++i)
            {
                const auto st = loop.onWindow (next(), 3000.0);
                if (st.writeDrive) { ++writes; after = st.newPre; }
            }
            check (writes == 1 && std::abs (after - (settled + 1.0f)) < 0.01f,
                   "21t-g (6b/c) as re-ruled. ...and the ONE step a comparative buys moves ONE dB from where the "
                   "settle left it, not from zero and not twice",
                   juce::String (writes) + " write(s), " + juce::String (settled, 1) + " -> "
                   + juce::String (after, 1) + " dB");
        }

        // (iv-c) 21t-k item 3 (28 Sep 2026 ruling): A DRIVE BLOCK WITH A PARAM AND A START IS CONTRADICTORY.
        // Sean's Zip build: actuator "drive", param "Threshold", start_db -15.00 from the working position. The
        // kind won, so the -15 landed on EchoJay's own staging pre-gain instead of the plugin's Threshold, a
        // -35 dB vocal reached the detector at -50, and the compressor did nothing for eighteen windows.
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = parse (R"({"source":"working_position","mode":"passive","actuator":"drive","slot":1,
                                       "param":"Threshold","start_db":-15.0,"measure":"short90",
                                       "gr_target_db":[2,3]})", 2, c, why);
            check (ok && ! (c.startDb == c.startDb) && ! c.startFromBlock,
                   "21t-k 3. a drive block carrying a param AND a start is refused: the start is IGNORED and the "
                   "drive opens at the staging  (RED as it stood: -15.0 went onto the staging pre-gain and the "
                   "plugin never compressed)",
                   c.startDb == c.startDb ? juce::String (c.startDb, 2) + " dB taken" : juce::String ("unset"));
            check (why.contains ("a drive block carries a param and a start"),
                   "21t-k 3. ...and the log says so, in the ruled words", why.trim().substring (0, 130));
            // ...and the loop opened from the staging moves FROM THERE, not from the block's number.
            echojay::CalibLoop::Config staged = c; staged.startDb = -3.0f;   // what the slot actually carries
            echojay::CalibLoop l; l.begin (staged);
            check (std::abs (l.preDb - (-3.0f)) < 0.001f,
                   "21t-k 3. ...so the pre-slot gain stays at the staging", juce::String (l.preDb, 2) + " dB");
        }

        // (v) a mode or actuator string that is not one of the two is flagged, and the SAFE one is used
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = parse (R"({"mode":"quiet","actuator":"knob","slot":1,"gr_target_db":[2,3]})", 2, c, why);
            check (ok && c.mode == echojay::CalibLoop::Mode::Passive
                   && c.actuator == echojay::CalibLoop::Actuator::Drive
                   && why.contains ("mode \"quiet\"") && why.contains ("actuator \"knob\""),
                   "21t-g (6b). a mode or actuator outside the two literals is NAMED in the log and the quiet, "
                   "safe choice is taken (passive, drive)", why.trim().substring (0, 130));
        }
        // (v-b) 21t-h: THE OTHER SIDE'S OWN TEXT, from a fixture file that is a verbatim paste of
        // CONTRACT_GROUPS_2026-09-22.md - comments and all. A leg that parses JSON its own author wrote proves the
        // author agrees with himself; this one proves we read what B actually publishes. When B pastes a new block,
        // it replaces that file.
        {
            auto f = juce::File (__FILE__).getParentDirectory()
                        .getChildFile ("fixtures").getChildFile ("calibration_blocks_from_contract.txt");
            check (f.existsAsFile(), "21t-h. fixture: the contract's own blocks are on disk", f.getFileName());
            if (f.existsAsFile())
            {
                juce::StringArray blocks;
                {
                    juce::String body;
                    for (const auto& line : juce::StringArray::fromLines (f.loadFileAsString()))
                    {
                        if (line.trim().startsWith ("#")) continue;                 // the provenance note
                        if (line.trim() == "---") { blocks.add (body); body.clear(); continue; }
                        auto l = line;
                        const int c = l.indexOf ("//");                             // the paste's own comments
                        if (c >= 0) l = l.substring (0, c);
                        // ...and the paste's own ELLIPSIS. B's second block ends "start_db": -18.2, ... which is
                        // documentation shorthand for "and the rest", not a wire sample. Stripped exactly as the
                        // comments are, and named here so nobody mistakes it for the server sending "...".
                        const int e = l.indexOf ("...");
                        if (e >= 0) l = l.substring (0, e).trimEnd().trimCharactersAtEnd (",");
                        body << l << "\n";
                    }
                    if (body.trim().isNotEmpty()) blocks.add (body);
                }
                check (blocks.size() == 2, "21t-h. ...both of them", juce::String (blocks.size()) + " block(s)");
                for (int bi = 0; bi < blocks.size(); ++bi)
                {
                    // Each paste is the OBJECT'S BODY ("calibration": { ... }); wrap it so it parses as one object.
                    const auto wrapped = "{" + blocks[bi].trim().trimCharactersAtEnd (",") + "}";
                    const auto v = juce::JSON::parse (wrapped);
                    const auto block = v.getProperty ("calibration", juce::var());
                    check (block.getDynamicObject() != nullptr,
                           "21t-h. block " + juce::String (bi + 1) + " from the contract parses as an object",
                           wrapped.substring (0, 60).replace ("\n", " "));
                    echojay::CalibLoop::Config c; juce::String why;
                    const bool ok = echojay::CalibLoop::configFromBlock (block, 2, false, "UnFairchild 670M II", c, why);
                    std::printf ("    contract block %d -> ok=%d why=\"%s\"\n", bi + 1, (int) ok, why.trim().toRawUTF8());
                    if (bi == 0)
                    {
                        // B'S 1-BASED RULING, in B's own text (CONTRACT_GROUPS as at 10:30 on 27 Sep): "slot": 1
                        // with the comment "WHICH slot, 1-BASED, as [CURRENT CHAIN] numbers them". This client has
                        // been 1-based throughout, so the block parses whole - which is the fact that matters after
                        // Sean's 10:15 rejection, and it is asserted against B's document rather than against mine.
                        check (ok && c.slot == 0 && c.actuator == echojay::CalibLoop::Actuator::Threshold
                               && c.params.size() == 1 && c.params[0] == "Thresh" && c.senseSign == -1
                               && std::abs (c.startDb - (-13.4f)) < 0.01f
                               && std::abs (c.minDb - (-15.0f)) < 0.01f && std::abs (c.maxDb - 15.0f) < 0.01f,
                               "21t-h. B's OWN block parses whole: wire slot 1 -> index 0, threshold, Thresh, "
                               "lower_is_harder, start -13.4, range -15..15",
                               "slot " + juce::String (c.slot) + ", " + c.params.joinIntoString (",") + " @ "
                               + juce::String (c.startDb, 1) + (why.isEmpty() ? juce::String() : ", why: " + why));
                        // ...and a 0-BASED slot - what the server WAS sending at 10:15 - is refused with the WIRE
                        // value in the line, so a base mismatch is a glance and not a puzzle about an empty rack.
                        {
                            // THE 0-BASED VARIANT BY TEXT, not by cloning the object. Two attempts at cloning it
                            // segfaulted this guard on both legs (a DynamicObject::clone() Ptr released before the
                            // var took its reference), and there is nothing to be gained here from object surgery:
                            // the block is text on the wire, so the variant is made the way it arrives.
                            const auto zeroText = wrapped.replace ("\"slot\": 1", "\"slot\": 0");
                            const auto zeroVar = juce::JSON::parse (zeroText);
                            echojay::CalibLoop::Config c0; juce::String why0;
                            const bool ok0 = echojay::CalibLoop::configFromBlock (
                                                 zeroVar.getProperty ("calibration", juce::var()), 2, false, "x", c0, why0);
                            check (! ok0 && why0.contains ("wire slot 0") && why0.contains ("rack has 2 slot(s)"),
                                   "21t-h. ...and the 0-BASED slot the server sent at 10:15 is refused with the WIRE "
                                   "value named (it used to print the converted number and read like an empty rack)",
                                   why0.trim());
                        }
                    }
                    else
                    {
                        // B'S SECOND BLOCK CARRIES NO "slot" - the field is in the first example, and this one shows
                        // the four tally fields beside the actuator. A block with no slot must therefore be REFUSED
                        // (there is nothing to start a loop on), and the refusal says "(absent)" rather than
                        // inventing an index. That is the assertion; the earlier one assumed a slot this paste does
                        // not have, which was me asserting my own idea of B's text again.
                        check (! ok && why.contains ("wire slot (absent)"),
                               "21t-h. ...and B's tally-source block, which carries NO slot, starts nothing - the "
                               "refusal says the slot was absent rather than inventing one",
                               why.trim().substring (0, 80));
                    }
                }
            }
        }

        // (vi) a slot this rack does not have starts nothing
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = parse (R"({"mode":"passive","actuator":"drive","slot":9,"gr_target_db":[2,3]})", 2, c, why);
            // 21t-h: the wording changed on purpose - the line now names the WIRE value, so the assertion follows
            // the new text rather than the old.
            check (! ok && why.contains ("wire slot 9") && why.contains ("rack has 2 slot(s)"),
                   "21t-g (6b). a slot the rack does not have starts nothing, and says which - by its WIRE value",
                   why.trim().substring (0, 90));
        }
        // (vii) source and measure outside their literals are flagged (they are logged, not acted on)
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = parse (R"({"mode":"passive","actuator":"drive","slot":1,"source":"guess",
                                       "measure":"rms","gr_target_db":[2,3]})", 2, c, why);
            check (ok && why.contains ("source \"guess\"") && why.contains ("measure \"rms\""),
                   "21t-g (6b). source and measure outside their literals are named too - a new value shows up in "
                   "the log instead of passing as one of ours", why.trim().substring (0, 120));
        }
        // (viii) ALL FOUR `source` VALUES ARE ACCEPTED SILENTLY. "measured" arrived on 27 Sep (a start from the
        // turn's own meter reading) and "working_position" the same day; "listen" means an ASKED calibration and
        // only that. The field is logged, not acted on - but a legitimate value flagged as "NOT AS CONTRACTED"
        // teaches the reader to ignore the one line that reports real breaks, so each is accepted by name.
        {
            const char* sources[] = { "tally", "measured", "working_position", "listen" };
            juce::String flagged;
            for (const auto* src : sources)
            {
                echojay::CalibLoop::Config c; juce::String why;
                const juce::String json = juce::String (R"({"mode":"passive","actuator":"drive","slot":1,"source":")")
                                        + src + R"(","measure":"short90","gr_target_db":[2,3]})";
                const bool ok = parse (json.toRawUTF8(), 2, c, why);
                if (! ok || why.isNotEmpty()) flagged << src << " (" << why.trim() << ") ";
            }
            check (flagged.isEmpty(),
                   "21t-g (6b). every `source` the server emits parses with NOTHING flagged: tally, measured, "
                   "working_position, listen",
                   flagged.isEmpty() ? juce::String ("all four clean") : flagged);
        }
    }

    // ---- 21t-f (5): SHORT90 IS THE p90 OF THE CLOSED SHORT-TERM WINDOWS ----------------------------------
    // The tally already had a p90, over the 400 ms MOMENTARY histogram. SHORT90 is a different quantity and
    // this leg is what stops the two being confused: it drives a real EchoJayLevelTally with programme that
    // SITS at one level and spends a little time much louder, and asserts that SHORT90 follows where the
    // programme sits while SHORTMAX follows the loudest moment. On a tree without the field it does not compile.
    {
        std::printf ("\n== 21t-f (5): SHORT90 - where the programme SITS, not its loudest moment ==\n");
        echojay::LevelTally t { echojay::LevelTally::Weighting::K };
        t.prepare (48000.0);
        const int block = 4800;                       // 100 ms hops, the tally's own granularity
        std::vector<float> L ((size_t) block), R ((size_t) block);
        auto feed = [&] (float amp, int hops)
        {
            for (int h = 0; h < hops; ++h)
            {
                for (int i = 0; i < block; ++i)
                {
                    const float v = amp * std::sin (2.0f * juce::MathConstants<float>::pi * 1000.0f
                                                    * (float) ((h * block + i) % 48000) / 48000.0f);
                    L[(size_t) i] = v; R[(size_t) i] = v;
                }
                t.push (L.data(), R.data(), block);
            }
        };
        // THE PROPORTIONS ARE THE POINT, and the first cut of this leg got them wrong: p90 is the 90th
        // percentile of ALL closed windows, so a 6 s loud passage after 15 s of programme IS the top tenth and
        // p90 landed ON it. A p90 only stays with the programme while the loud material is a small minority of
        // the windows - which is exactly the real case it is for. 60 s at one level, then 1.5 s about 10 dB
        // louder: ~45 of ~615 windows are touched by the loud passage (7 %), so SHORTMAX goes to the loud level
        // and SHORT90 stays with the programme.
        feed (0.05f, 600);
        const auto quietOnly = t.snapshot();
        feed (0.16f, 15);
        const auto both = t.snapshot();
        check (quietOnly.shortTermP90Db == quietOnly.shortTermP90Db,
               "21t-f (5). SHORT90 has a reading once windows have closed",
               juce::String (quietOnly.shortTermP90Db, 2));
        check (both.maxShortTermDb > both.shortTermP90Db + 3.0f,
               "21t-f (5). the loudest 3 s window is well above SHORT90 - the loud passage moves SHORTMAX, not "
               "where the programme sits",
               "SHORTMAX " + juce::String (both.maxShortTermDb, 1) + " vs SHORT90 "
               + juce::String (both.shortTermP90Db, 1));
        check (both.shortTermP90Db >= quietOnly.shortTermP90Db - 0.01f
               && both.shortTermP90Db <= quietOnly.shortTermP90Db + 1.5f,
               "21t-f (5). ...and SHORT90 itself moves only as far as the distribution moved it",
               juce::String (quietOnly.shortTermP90Db, 1) + " -> " + juce::String (both.shortTermP90Db, 1));
        t.resetShortTermMax();
        feed (0.05f, 5);                              // fewer than 30 hops: no window has closed since the reset
        const auto afterReset = t.snapshot();
        check (! (afterReset.shortTermP90Db == afterReset.shortTermP90Db)
               && ! (afterReset.maxShortTermDb == afterReset.maxShortTermDb),
               "21t-f (5). the reset clears SHORT90 with the max hold - both describe the same window, so they "
               "start together", "SHORT90 " + juce::String (afterReset.shortTermP90Db, 1));
    }

    // ===============================================================================================
    // 21t-i (27 Sep 2026 ruling): MEASURE AND ASK. No automatic stepping after a build. The compressor is set
    // ONCE, from the block; after two judged windows the loop reports what it measured and asks; a comparative
    // arrives as a re-targeted block and buys exactly ONE step. One leg per clause of the ruling.
    // ===============================================================================================
    {
        std::printf ("\n== 21t-i: set once, then measure and ask ==\n");
        auto win = [] (float gr, float heard)
        {
            echojay::CalibLoop::Window w;
            w.measured = true; w.silent = false; w.grDb = gr; w.inTruePeakDb = -12.0f; w.heardSeconds = heard;
            return w;
        };
        auto passiveDrive = [] ()
        {
            echojay::CalibLoop::Config c;
            c.plugin = "NEOLD U2A"; c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
            c.mode = echojay::CalibLoop::Mode::Passive;
            c.actuator = echojay::CalibLoop::Actuator::Drive;
            c.startDb = -6.0f;
            echojay::CalibLoop l; l.begin (c); return l;
        };

        // (1) SET ONCE: twelve judged windows a long way out of band, and NOT ONE write. Before this round the
        // same twelve windows walked the drive six times and closed the loop.
        {
            auto l = passiveDrive();
            int writes = 0, lines = 0;
            for (int i = 0; i < 12; ++i)
            {
                const auto st = l.onWindow (win (20.0f, 30.0f + (float) i * 3.0f), 3000.0);
                if (st.writeDrive || st.writeParams) ++writes;
                if (st.logLine.isNotEmpty()) ++lines;
            }
            // 28 Sep 2026 (the settle ruling) SUPERSEDES "set once": the BUILD lands it, taking at most three
            // steps within 15 s of heard audio. What survives of the older ruling is the part after landing, and
            // that is what this leg asserts now: the settle spends its budget and then nothing moves at all.
            check (writes <= 3, "21t-i (1) as re-ruled. the settle takes AT MOST THREE steps, however far out",
                   juce::String (writes) + " write(s)");
            // 29 Sep 2026 (21t-m item 1) RE-STATES THIS ONE AGAIN, and the re-statement is forced, not chosen:
            // the hold now runs AFTER landing rather than between drive steps, so "after landing nothing moves"
            // is no longer true as written - the hold's one write is a write after landing and is the point of
            // the round. What the ruling actually promises is that the ACTUATOR never moves again and the hold
            // writes AT MOST TWICE, so that is what is counted here, separately.
            int driveAfterLanding = 0, holdAfterLanding = 0;
            for (int i = 0; i < 12; ++i)
            { const auto st = l.onWindow (win (20.0f, 200.0f + (float) i * 3.0f), 3000.0);
              if (st.writeDrive || st.writeParams) ++driveAfterLanding;
              if (st.writeSlotGain || st.writeOutput) ++holdAfterLanding; }
            check (l.landed && driveAfterLanding == 0,
                   "21t-i (1) as re-ruled twice. ...and AFTER LANDING twelve judged windows 17 dB out of band "
                   "move THE ACTUATOR not at all",
                   juce::String (driveAfterLanding) + " actuator write(s) after landing");
            check (holdAfterLanding <= 2,
                   "21t-m (1). ...and the hold writes AT MOST TWICE over those twelve windows - once on the "
                   "landed drive and at most one refinement, never once per window",
                   juce::String (holdAfterLanding) + " hold write(s) after landing");
            check (lines == 12, "21t-i (1). ...and every judged window still prints its line, so a loop sitting "
                   "in band and a loop that has stalled cannot look the same",
                   juce::String (lines) + " line(s) of 12");
            check (l.settleSteps <= 3,
                   "21t-i (1) as re-ruled. ...and the knob moved only inside the settle's budget",
                   juce::String (l.settleSteps) + " step(s), drive " + juce::String (l.preDb, 2) + " dB");
        }

        // (2) THE QUESTION, after two judged windows, carrying the measured figure.
        //
        // 21t-i re-cut: the "set from N s" clause quotes THE BLOCK's heard_s, so the config carries one. The window
        // figures below are deliberately different (78 s, 80 s): they are the slot tally's age and the sentence must
        // not quote them.
        {
            auto l = passiveDrive();
            l.blockHeardS = 80.0f;
            // 28 Sep 2026: THERE IS NO ASK STATE. The build's own line completes in place when the settle lands,
            // and that line carries the plugin, what it was set from, the measured figure and the two words.
            juce::String completion; float h = 78.0f;
            for (int i = 0; i < 12 && completion.isEmpty(); ++i)
            { h += 3.0f; const auto st = l.onWindow (win (2.5f, h), 3000.0); if (st.ask.isNotEmpty()) completion = st.ask; }
            check (completion.isNotEmpty(), "21t-i (2) as re-ruled. the settle completes the build's line", completion);
            check (completion.contains ("NEOLD U2A") && completion.contains ("80 s")
                   && completion.contains ("2.5 dB of gain reduction")
                   && completion.contains ("ease off") && completion.contains ("more"),
                   "21t-i (2) as re-ruled. ...naming the plugin, what it was set from, the MEASURED figure, and "
                   "the two words", completion);
            juce::String after;
            for (int i = 0; i < 6; ++i)
            { h += 3.0f; const auto st = l.onWindow (win (2.5f, h), 3000.0); if (st.ask.isNotEmpty()) after = st.ask; }
            check (after.isEmpty(), "21t-i (2) as re-ruled. ...and it says it ONCE, not on every window after",
                   after.isEmpty() ? juce::String ("silent") : after);
            check (l.closingMessage().isEmpty(),
                   "21t-i (2). ...and there is no closing line in this mode at all", l.closingMessage());
        }

        // (3) ONE STEP PER COMPARATIVE: the band moves up (the user said "more"), and exactly one 1 dB step lands.
        {
            auto l = passiveDrive();
            l.onWindow (win (5.4f, 78.0f), 3000.0);
            l.onWindow (win (5.4f, 80.0f), 3000.0);
            // 28 Sep 2026: the build has already taken its own step by here, so the comparative is measured
            // against WHERE THE SETTLE LEFT IT, not against the value the block opened with. One word, one move.
            const float before = l.preDb;
            l.retarget (5.0f, 6.0f);
            int writes = 0; float landed = before; juce::String completion;
            for (int i = 0; i < 12; ++i)
            {
                const auto st = l.onWindow (win (5.4f, 84.0f + (float) i * 3.0f), 3000.0);
                if (st.writeDrive) { ++writes; landed = st.newPre; }
                if (st.ask.isNotEmpty()) completion = st.ask;
            }
            check (writes == 1, "21t-i (3). a comparative buys EXACTLY ONE step over twelve more windows",
                   juce::String (writes) + " write(s)");
            check (std::abs (landed - (before + 1.0f)) < 0.001f,
                   "21t-i (3) as re-ruled. ...one dB harder than WHERE THE SETTLE LEFT IT, and no further",
                   juce::String (before, 2) + " -> " + juce::String (landed, 2) + " dB");
            check (completion.isNotEmpty() && completion.contains ("gain reduction"),
                   "21t-i (3) as re-ruled. ...and the line completes again in place with the new figure",
                   completion.isEmpty() ? juce::String ("(no completion)") : completion);
        }

        // (4) NO STEP WITHOUT ONE: a block whose band did NOT move is not a comparative, and buys nothing.
        {
            auto l = passiveDrive();
            l.onWindow (win (5.4f, 78.0f), 3000.0);
            l.onWindow (win (5.4f, 80.0f), 3000.0);
            l.retarget (2.0f, 3.0f);
            int writes = 0;
            for (int i = 0; i < 6; ++i)
            {
                const auto st = l.onWindow (win (5.4f, 84.0f), 3000.0);
                if (st.writeDrive || st.writeParams) ++writes;
            }
            check (writes == 0, "21t-i (4). a re-target that did not move the band moves no knob",
                   juce::String (writes) + " write(s)");
        }

        // (5) THE THRESHOLD ACTUATOR takes the BLOCK's step size, and the drive always takes 1 dB.
        {
            echojay::CalibLoop::Config c; juce::String why;
            const auto v = juce::JSON::parse (juce::String (
                R"({"mode":"passive","actuator":"threshold","slot":1,"param":"Thresh","start_db":-14.0,
                    "sense":"lower_is_harder","step":3,"min_db":-40,"max_db":0,"gr_target_db":[2,3]})"));
            const bool ok = echojay::CalibLoop::configFromBlock (v, 1, false, "Pro-C 2", c, why);
            check (ok && std::abs (c.stepDb - 3.0f) < 0.001f,
                   "21t-i (5). \"step\" on the wire is read as the threshold's step size",
                   juce::String (c.stepDb, 1) + " dB" + (why.isEmpty() ? juce::String() : " why: " + why));
            echojay::CalibLoop l; l.begin (c);
            // 28 Sep 2026: the settle takes the block's step too, so the first move off -14.0 is three dB, and the
            // heard time has to ADVANCE on every window or the loop rejects it as stale.
            float heard = 60.0f;
            auto step3 = [&heard, &win] { heard += 3.0f; return win (1.0f, heard); };
            const auto s1 = l.onWindow (step3(), 3000.0);
            check (s1.writeParams && std::abs (s1.paramValue - (-17.0f)) < 0.001f,
                   "21t-i (5) as re-ruled. ...and the SETTLE moves the threshold by the block's three dB, in the "
                   "harder direction (lower is harder)", juce::String (s1.paramValue, 1) + " dB");
            for (int i = 0; i < 20 && ! l.landed; ++i) l.onWindow (step3(), 3000.0);
            const float settled = l.value;
            l.retarget (4.0f, 5.0f, c.stepDb);
            float landed = -999.0f; int writes = 0;
            for (int i = 0; i < 12; ++i)
            {
                const auto st = l.onWindow (step3(), 3000.0);
                if (st.writeParams) { ++writes; landed = st.paramValue; }
            }
            check (writes == 1 && std::abs (landed - (settled - 3.0f)) < 0.001f,
                   "21t-i (5) as re-ruled. ...and one comparative moves it by THREE dB, once, from where the "
                   "settle left it",
                   juce::String (writes) + " write(s), " + juce::String (settled, 1) + " -> "
                   + juce::String (landed, 1) + " dB");
        }

        // (7) WHAT THE SENTENCE QUOTES (21t-i re-cut ruling): the BLOCK's heard_s, never the slot tally's heard
        // time. The slot's figure is the age of that slot's own measurement - it starts when the plugin is
        // inserted - so a compressor set by the server from two minutes of the track would have been described to
        // the user as set from three seconds of it.
        {
            echojay::CalibLoop::Config c;
            c.plugin = "NEOLD U2A"; c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
            c.mode = echojay::CalibLoop::Mode::Passive;
            c.actuator = echojay::CalibLoop::Actuator::Drive;
            c.startDb = -6.0f; c.heardS = 120.0f;
            echojay::CalibLoop l; l.begin (c);
            // 28 Sep 2026: the sentence arrives when the SETTLE LANDS rather than after two windows. What it
            // quotes is unchanged and is what this leg is about.
            juce::String ask; float h = 0.0f;
            for (int i = 0; i < 20 && ask.isEmpty(); ++i)
            { h += 3.0f; const auto w = l.onWindow (win (5.4f, h), 3000.0); ask = w.ask; }
            echojay::CalibLoop::Step st; st.ask = ask;
            check (st.ask.contains ("set from 120 s of this track"),
                   "21t-i (7). the question quotes the BLOCK's heard_s  (RED as it stood: it quoted the slot "
                   "tally's heard time, which is the age of the slot)", st.ask);
            check (! st.ask.contains (" 3 s ") && ! st.ask.contains (" 6 s "),
                   "21t-i (7). ...and never the slot's own figure", st.ask);
        }
        // ...and a WORKING POSITION start has no heard time to quote, so it says so.
        {
            echojay::CalibLoop::Config c;
            c.plugin = "Tube-Tech CL 1B"; c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
            c.mode = echojay::CalibLoop::Mode::Passive;
            c.actuator = echojay::CalibLoop::Actuator::Drive;
            c.startDb = 0.0f; c.working = true;
            echojay::CalibLoop l; l.begin (c);
            juce::String ask; float h = 40.0f;
            for (int i = 0; i < 20 && ask.isEmpty(); ++i)
            { h += 3.0f; const auto w = l.onWindow (win (2.5f, h), 3000.0); ask = w.ask; }
            echojay::CalibLoop::Step st; st.ask = ask;
            check (st.ask.contains ("set from the working position") && ! st.ask.contains (" s of this track"),
                   "21t-i (7). source \"working_position\": the sentence says where it came from instead of "
                   "quoting a measurement nobody made", st.ask);
        }
        // ...and a tally block with NO heard_s quotes nothing rather than inventing a number.
        {
            echojay::CalibLoop::Config c;
            c.plugin = "Pro-C 2"; c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
            c.mode = echojay::CalibLoop::Mode::Passive;
            c.actuator = echojay::CalibLoop::Actuator::Drive;
            c.startDb = 0.0f;                                  // heardS left NaN
            echojay::CalibLoop l; l.begin (c);
            juce::String ask; float h = 55.0f;
            for (int i = 0; i < 20 && ask.isEmpty(); ++i)
            { h += 3.0f; const auto w = l.onWindow (win (2.5f, h), 3000.0); ask = w.ask; }
            echojay::CalibLoop::Step st; st.ask = ask;
            // The completed line's shape, ruled 28 Sep: "<plugin> on." then the figure. The clause is still
            // OMITTED rather than stubbed, which is the whole of this leg.
            check (st.ask.startsWith ("Pro-C 2 on. Doing about")
                   && ! st.ask.contains ("set from") && st.ask.contains ("gain reduction"),
                   "21t-i (7). a block with no heard_s and no working-position flag omits the clause entirely - it "
                   "quotes no figure and does not read as a stutter",
                   st.ask);
        }

        // (8) THE NUDGE (21t-i re-cut): "harder"/"softer" on the block decides the direction on its own, with the
        // band unchanged or absent entirely.
        {
            echojay::CalibLoop::Config c;
            c.plugin = "NEOLD U2A"; c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
            c.mode = echojay::CalibLoop::Mode::Passive;
            c.actuator = echojay::CalibLoop::Actuator::Drive;
            c.startDb = -6.0f; c.heardS = 90.0f;
            echojay::CalibLoop l; l.begin (c);
            float h = 90.0f;
            auto w = [&h, &win] (float gr) { h += 3.0f; return win (gr, h); };
            for (int i = 0; i < 20 && ! l.landed; ++i) l.onWindow (w (5.4f), 3000.0);   // the build lands first
            const float before = l.preDb;
            l.retarget (2.0f, 3.0f, c.stepDb, -1, true);        // THE SAME BAND, nudge "softer"
            int writes = 0; float landed = before;
            for (int i = 0; i < 12; ++i)
            {
                const auto st = l.onWindow (w (5.4f), 3000.0);
                if (st.writeDrive) { ++writes; landed = st.newPre; }
            }
            check (writes == 1 && std::abs (landed - (before - 1.0f)) < 0.001f,
                   "21t-i (8) as re-ruled. a re-target with the SAME band and nudge \"softer\" buys exactly one "
                   "softer step from where the settle left it  (RED as it stood: an unmoved band bought nothing)",
                   juce::String (writes) + " write(s), " + juce::String (before, 2) + " -> "
                   + juce::String (landed, 2) + " dB");
        }
        // ...and with NO band at all, the loop keeps the band it has and still takes the step.
        {
            echojay::CalibLoop::Config c;
            c.plugin = "NEOLD U2A"; c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
            c.mode = echojay::CalibLoop::Mode::Passive;
            c.actuator = echojay::CalibLoop::Actuator::Drive;
            c.startDb = -6.0f;
            echojay::CalibLoop l; l.begin (c);
            float h = 90.0f;
            auto w = [&h, &win] (float gr) { h += 3.0f; return win (gr, h); };
            for (int i = 0; i < 20 && ! l.landed; ++i) l.onWindow (w (5.4f), 3000.0);
            const float before = l.preDb;
            l.retarget (0.0f, 0.0f, 0.0f, 1, false);            // no band on the block
            int writes = 0; float landed = before;
            for (int i = 0; i < 12; ++i)
            {
                const auto st = l.onWindow (w (5.4f), 3000.0);
                if (st.writeDrive) { ++writes; landed = st.newPre; }
            }
            check (writes == 1 && std::abs (landed - (before + 1.0f)) < 0.001f
                   && std::abs (l.lo - 2.0f) < 0.001f && std::abs (l.hi - 3.0f) < 0.001f,
                   "21t-i (8) as re-ruled. ...and a block with no band keeps the loop's band and still takes the "
                   "nudge's step",
                   juce::String (writes) + " write(s), " + juce::String (before, 2) + " -> "
                   + juce::String (landed, 2) + " dB, band "
                   + juce::String (l.lo, 1) + "-" + juce::String (l.hi, 1));
        }
        // ...and the wire literals: "harder"/"softer" parse, anything else is named in the log and ignored.
        {
            echojay::CalibLoop::Config c1, c2, c3; juce::String w1, w2, w3;
            const auto mk = [] (const char* json) { return juce::JSON::parse (juce::String (json)); };
            echojay::CalibLoop::configFromBlock (mk (R"({"mode":"passive","actuator":"drive","slot":1,
                                                         "nudge":"harder","gr_target_db":[2,3]})"), 1, false, "X", c1, w1);
            echojay::CalibLoop::configFromBlock (mk (R"({"mode":"passive","actuator":"drive","slot":1,
                                                         "nudge":"softer","gr_target_db":[2,3]})"), 1, false, "X", c2, w2);
            echojay::CalibLoop::configFromBlock (mk (R"({"mode":"passive","actuator":"drive","slot":1,
                                                         "nudge":"a bit less","gr_target_db":[2,3]})"), 1, false, "X", c3, w3);
            check (c1.nudge == 1 && w1.isEmpty() && c2.nudge == -1 && w2.isEmpty(),
                   "21t-i (8). \"harder\" and \"softer\" parse clean, nothing flagged",
                   "harder=" + juce::String (c1.nudge) + " softer=" + juce::String (c2.nudge));
            check (c3.nudge == 0 && w3.contains ("nudge \"a bit less\""),
                   "21t-i (8). ...and an unknown nudge literal is NAMED in the log and ignored, never guessed",
                   w3.trim());
        }
        // ...and heard_s / source / nudge together, off the wire, in one block.
        {
            echojay::CalibLoop::Config c; juce::String why;
            const auto v = juce::JSON::parse (juce::String (
                R"({"source":"measured","heard_s":84,"measure":"short90","mode":"passive","actuator":"drive",
                    "slot":1,"start_db":-3.0,"sense":null,"nudge":"harder","step":2,"gr_target_db":[2,3]})"));
            const bool ok = echojay::CalibLoop::configFromBlock (v, 1, false, "NEOLD U2A", c, why);
            check (ok && std::abs (c.heardS - 84.0f) < 0.01f && ! c.working && c.nudge == 1 && c.haveBand
                   && std::abs (c.stepDb - 2.0f) < 0.01f && why.isEmpty(),
                   "21t-i (8). heard_s, source \"measured\", nudge and step all parse off one block with nothing "
                   "flagged",
                   "heard_s " + juce::String (c.heardS, 0) + ", nudge " + juce::String (c.nudge)
                   + ", step " + juce::String (c.stepDb, 1) + (why.isEmpty() ? juce::String() : " why: " + why));
            echojay::CalibLoop l; l.begin (c);
            juce::String ask; float h = 6.0f;
            for (int i = 0; i < 20 && ask.isEmpty(); ++i)
            { h += 3.0f; const auto wnd = l.onWindow (win (5.4f, h), 3000.0); ask = wnd.ask; }
            echojay::CalibLoop::Step st; st.ask = ask;
            check (st.ask.contains ("set from 84 s of this track"),
                   "21t-i (8). ...and the sentence quotes that block's 84 s, with the slot only 12 s old", st.ask);
        }

        // (6) NOTHING HEARD: after 30 s of silence the loop asks for playback, once, and never quotes a figure
        // it does not have.
        {
            auto l = passiveDrive();
            echojay::CalibLoop::Window silent; silent.measured = true; silent.silent = true;
            juce::StringArray asks;
            for (int i = 0; i < 20; ++i)
            {
                const auto st = l.onWindow (silent, 3000.0);
                if (st.ask.isNotEmpty()) asks.add (st.ask);
            }
            check (asks.size() == 1, "21t-i (6). 30 s with nothing heard asks for playback, ONCE",
                   juce::String (asks.size()) + " line(s)");
            check (asks.size() == 1 && asks[0].contains ("play it and I'll tell you what it's doing"),
                   "21t-i (6). ...in the ruled words", asks.isEmpty() ? juce::String ("(none)") : asks[0]);
        }
    }

    // ===============================================================================================
    // 21t-l item 5 (29 Sep 2026 ruling): A CALIBRATION BLOCK ON AN EDIT TURN WAITS FOR THE OPS.
    // Sean's 10:45:07 edit: the block was judged at 10:45:13 against a rack with no slots ("block not usable -
    // wire slot 1 ... rack has 0 slot(s); nothing started") and the Acme Opticom XLA-3 arrived at 10:45:15, so
    // the settle never ran and the compressor sat at -7 dB of gain reduction. An edit that ADDS is held until
    // the ops land and the dial settles, the same road a build takes; "rack has 0 slots" is a refusal only when
    // the reply carries no add.
    // ===============================================================================================
    {
        std::printf ("\n== 21t-l item 5: a calibration block on an edit turn waits for the ops ==\n");
        const juce::String withAdd =
            R"({"edit":[{"op":"add","name":"Acme Opticom XLA-3","after":0}],)"
            R"("calibration":{"source":"tally","mode":"passive","actuator":"drive","slot":1,)"
            R"("gr_target_db":[2,3]}})";
        const juce::String setOnly =
            R"({"edit":[{"op":"set","slot":1,"settings_structured":{"params":{"threshold_db":-18}}}],)"
            R"("calibration":{"source":"tally","mode":"passive","actuator":"drive","slot":1,)"
            R"("gr_target_db":[2,3]}})";
        check (EchoJayEditor::editCarriesAdd (withAdd),
               "21t-l 5. an edit that ADDS a plugin is one whose block must wait for the ops  (RED as it stood: "
               "the block was judged the instant the apply returned, against a rack with no slots)");
        check (! EchoJayEditor::editCarriesAdd (setOnly),
               "21t-l 5. ...and an edit that only SETS does not wait - there is a rack already, so \"rack has 0 "
               "slots\" is a real refusal there");
        check (! EchoJayEditor::editCarriesAdd ({}),
               "21t-l 5. ...and an empty edit adds nothing");
        // ...and the refusal itself is unchanged for the case that IS a refusal: a block naming a slot the rack
        // does not have, on a reply that adds nothing.
        {
            echojay::CalibLoop::Config c; juce::String why;
            const bool ok = echojay::CalibLoop::configFromBlock (
                juce::JSON::parse (setOnly).getProperty ("calibration", juce::var()),
                0, false, {}, c, why);
            check (! ok && why.isNotEmpty(),
                   "21t-l 5. ...and a block naming slot 1 of an EMPTY rack is still refused, with the reason",
                   why.trim().substring (0, 110));
        }
    }

    // ===============================================================================================
    // 21t-j (owed): THE GR-METER CROSS-CHECK. B's block says a control reads as dB; whether it does is a fact
    // about the plugin, and the product writes down what it found rather than believing the claim. Five windows
    // after a build, then quiet - and the line carries the RAW position beside the printed text, because a
    // control printing "0.41" for a compressor doing 4 dB is exactly the confusion this exists to catch.
    // ===============================================================================================
    {
        std::printf ("\n== 21t-j: the GR-meter cross-check ==\n");
        echojay::CalibLoop::Config c;
        c.plugin = "Purple Audio MC 77"; c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
        c.mode = echojay::CalibLoop::Mode::Passive;
        c.actuator = echojay::CalibLoop::Actuator::Drive;
        c.startDb = 0.0f;
        c.senseParams.add ("GR Meter L"); c.grReadable = true;
        echojay::CalibLoop l; l.begin (c);
        check (l.senseLogsOwed == echojay::CalibLoop::kSenseLogWindows,
               "21t-j (cross-check). a build that names a sense control owes FIVE windows of it",
               juce::String (l.senseLogsOwed));
        // THE PARSE IS THE PRODUCT'S OWN, not a bool set by hand: a leg that decides for itself whether the text
        // parsed would assert a fiction. Three texts, one per window: a real dB meter, the same control printing
        // its bare normalised position, and a control printing nothing numeric at all.
        const char* texts[5] = { "4.2 dB", "0.41", "--", "-3.5 dB", "GR" };   // "--" is an absence since 21t-j
        juce::StringArray lines;
        for (int i = 0; i < 12; ++i)
        {
            int n = 0;
            if (! l.takeSenseLog (n)) continue;
            const juce::String text (texts[(n - 1) % 5]);
            double db = 0.0;
            const bool okParse = echojay::parseDisplayDb (text, db) && db > -1.0e8 && db < 1.0e8;
            lines.add (echojay::CalibLoop::senseCrossCheckLine (c.plugin, c.senseParams[0],
                                                               0.4100f, text, okParse, (float) db, n));
        }
        check (lines.size() == 5,
               "21t-j (cross-check). ...FIVE and no more - a check, not a running commentary",
               juce::String (lines.size()) + " line(s)");
        for (const auto& ln : lines) std::printf ("    %s\n", ln.toRawUTF8());
        check (lines.size() == 5 && lines[0].contains ("window 1/5") && lines[4].contains ("window 5/5"),
               "21t-j (cross-check). ...numbered, so a truncated run is visible in the log",
               lines.isEmpty() ? juce::String ("(none)") : lines[0]);
        check (! lines.isEmpty() && lines[0].contains ("raw=0.4100") && lines[0].contains ("text=\"4.2 dB\"")
               && lines[0].contains ("parsed=4.20 dB") && lines[0].contains ("prints_db=yes")
               && ! lines[0].contains ("NOTE:"),
               "21t-j (cross-check). a real dB meter: the line carries the raw position, the printed text, the "
               "parse and the verdict, and nothing else", lines.isEmpty() ? juce::String ("(none)") : lines[0]);
        // THE CASE THIS EXISTS FOR. parseDisplayDb reads "0.41" as 0.41 dB - correctly, it is a number - so the
        // verdict is prints_db=yes and the TELL is that the figure equals the raw position. The line says that
        // out loud rather than leaving someone to notice it.
        check (lines.size() > 1 && lines[1].contains ("text=\"0.41\"") && lines[1].contains ("parsed=0.41 dB")
               && lines[1].contains ("prints_db=yes")
               && lines[1].contains ("the parsed figure equals the raw position"),
               "21t-j (cross-check). a control printing its BARE POSITION parses as dB - and the line says the "
               "figure equals the raw position, which is the observation, not a guess",
               lines.size() > 1 ? lines[1] : juce::String ("(none)"));
        check (lines.size() > 2 && lines[2].contains ("text=\"--\"") && lines[2].contains ("parsed=NO")
               && lines[2].contains ("prints_db=no"),
               "21t-j (cross-check). ...and a control printing nothing numeric says prints_db=no, which is a "
               "finding, not an absence", lines.size() > 2 ? lines[2] : juce::String ("(none)"));
        { echojay::CalibLoop::Config plain = c; plain.senseParams.clear(); plain.grReadable = false;
          echojay::CalibLoop l2; l2.begin (plain); int n = 0;
          check (l2.senseLogsOwed == 0 && ! l2.takeSenseLog (n),
                 "21t-j (cross-check). a build with NO sense control logs nothing at all",
                 juce::String (l2.senseLogsOwed)); }
    }

    // ===============================================================================================
    // 21t-j (28 Sep 2026 ruling): THE SETTLE IS THE TAIL OF THE BUILD. One build, one line: the build posts the
    // opening line, the settle completes THAT line in place, at most three steps within 15 s of HEARD audio, and
    // after landing nothing moves except on a comparative.
    // ===============================================================================================
    {
        std::printf ("\n== 21t-j: the settle is the tail of the build ==\n");
        // A window with the two ruled sensors on it: the crest difference and the level change.
        auto win = [] (float gr, float levelChange, float heard)
        {
            echojay::CalibLoop::Window w;
            w.measured = true; w.silent = false;
            w.grDb = gr; w.levelChangeDb = levelChange; w.heardSeconds = heard; w.inTruePeakDb = -12.0f;
            return w;
        };
        auto buildLoop = [] ()
        {
            echojay::CalibLoop::Config c;
            c.plugin = "Mock Comp"; c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
            c.mode = echojay::CalibLoop::Mode::Passive;
            c.actuator = echojay::CalibLoop::Actuator::Drive;
            c.startDb = 0.0f; c.heardS = 90.0f;
            echojay::CalibLoop l; l.begin (c); return l;
        };
        {   // THE OPENING LINE, at the build, before any audio.
            auto l = buildLoop();
            check (l.askOwed.contains ("Mock Comp on") && l.askOwed.contains ("landing it as it plays"),
                   "21t-j (settle). the BUILD posts one line and it promises what the product then does",
                   l.askOwed);
            check (! l.askOwed.contains ("How's that sounding"),
                   "21t-j (settle). ...and it asks for nothing: there is no ask state any more", l.askOwed);
            check (l.settling && ! l.landed, "21t-j (settle). ...and the settle is open");
        }
        {   // IT LANDS, and the SAME line completes in place.
            auto l = buildLoop();
            l.askOwed.clear();
            float heard = 90.0f;
            juce::String completion; bool replaces = false; int writes = 0;
            for (int i = 0; i < 12 && completion.isEmpty(); ++i)
            {
                heard += 3.0f;
                const auto st = l.onWindow (win (2.5f, 0.2f, heard), 3000.0);
                if (st.writeDrive || st.writeParams || st.writeSlotGain) ++writes;
                if (st.ask.isNotEmpty()) { completion = st.ask; replaces = st.askReplacesOpening; }
            }
            check (completion.isNotEmpty() && replaces,
                   "21t-j (settle). the settle completes the SAME line in place, not a second message", completion);
            check (completion.contains ("2.5 dB of gain reduction"),
                   "21t-j (settle). ...quoting the MEASURED crest difference, positive", completion);
            check (l.landed && ! l.settling, "21t-j (settle). ...and it has landed");
            check (l.settleSteps == 0 && writes == 0,
                   "21t-j (settle). ...with no step at all, because it was already in band",
                   juce::String (l.settleSteps) + " step(s), " + juce::String (writes) + " write(s)");
        }
        {   // OUT OF BAND: at most three steps, and it lands anyway.
            auto l = buildLoop();
            l.askOwed.clear();
            float heard = 90.0f; int writes = 0; juce::String completion;
            for (int i = 0; i < 30 && completion.isEmpty(); ++i)
            {
                heard += 3.0f;
                const auto st = l.onWindow (win (0.2f, 0.2f, heard), 3000.0);
                if (st.writeDrive) ++writes;
                if (st.ask.isNotEmpty()) completion = st.ask;
            }
            check (l.settleSteps <= 3 && writes <= 3,
                   "21t-j (settle). NEVER MORE THAN THREE STEPS, however far out of band it is",
                   juce::String (l.settleSteps) + " step(s), " + juce::String (writes) + " write(s)");
            check (completion.isNotEmpty() && l.landed,
                   "21t-j (settle). ...and it still lands, and still says what it landed on", completion);
            check (completion.contains ("less than the 2-3 I'm after"),
                   "21t-j (settle). ...naming the band when the figure is outside it", completion);
        }
        {   // A BUILD WITH NO AUDIO: one line, no second write, no state change.
            auto l = buildLoop();
            const auto opening = l.askOwed; l.askOwed.clear();
            echojay::CalibLoop::Window silent; silent.measured = true; silent.silent = true;
            int writes = 0; juce::String said;
            for (int i = 0; i < 40; ++i)
            {
                const auto st = l.onWindow (silent, 3000.0);
                if (st.writeDrive || st.writeParams || st.writeSlotGain) ++writes;
                if (st.ask.isNotEmpty()) said = st.ask;
            }
            check (writes == 0 && ! l.landed,
                   "21t-j (settle). a build with NO AUDIO moves nothing and does not land - it waits, for as long "
                   "as it takes", juce::String (writes) + " write(s)");
            check (opening.contains ("landing it as it plays"),
                   "21t-j (settle). ...and the line it posted still says what it is waiting for", opening);
            // ...and audio arriving much later completes THE SAME line, one line not two.
            float heard = 90.0f; juce::String completion;
            for (int i = 0; i < 12 && completion.isEmpty(); ++i)
            { heard += 3.0f; const auto st = l.onWindow (win (2.4f, 0.1f, heard), 3000.0);
              if (st.ask.isNotEmpty()) completion = st.ask; }
            check (completion.isNotEmpty() && l.landed,
                   "21t-j (settle). audio arriving later completes the same line - one line, not two", completion);
        }
        {   // THE HEARD-AUDIO BUDGET, not the clock: 15 s of heard audio ends the settle.
            auto l = buildLoop();
            l.askOwed.clear();
            float heard = 90.0f; juce::String completion;
            for (int i = 0; i < 40 && completion.isEmpty(); ++i)
            { heard += 3.0f; const auto st = l.onWindow (win (0.1f, 0.0f, heard), 3000.0);
              if (st.ask.isNotEmpty()) completion = st.ask; }
            // The figure counts ALL heard audio inside the settle, including the windows spent waiting for three
            // fresh ones after each write - which is what "15 s of heard audio" means for a settle that is
            // allowed to step. What matters is that the budget ENDS it: it landed, and it did not run on.
            check (l.landed && l.settleHeardS >= 15.0f && completion.isNotEmpty(),
                   "21t-j (settle). the budget is HEARD seconds, and spending it lands the line",
                   juce::String (l.settleHeardS, 1) + " s heard, " + juce::String (l.settleSteps) + " step(s)");
        }
        {   // A USER EDIT CANCELS IT: the line closes with the setting as it stands.
            auto l = buildLoop();
            l.askOwed.clear();
            l.onWindow (win (0.2f, 0.0f, 93.0f), 3000.0);
            const auto line = l.cancelSettle ("an edit moved slot 1");
            check (line.isNotEmpty() && l.landed && ! l.settling,
                   "21t-j (settle). a user edit CANCELS the settle", line.substring (0, 80));
            check (l.askOwed.contains ("Mock Comp on") && ! l.askOwed.contains ("landing it as it plays"),
                   "21t-j (settle). ...and the line closes with the setting as it stands", l.askOwed);
        }
        {   // AFTER LANDING nothing moves for a long run of windows.
            auto l = buildLoop();
            l.askOwed.clear();
            float heard = 90.0f; juce::String completion;
            for (int i = 0; i < 12 && completion.isEmpty(); ++i)
            { heard += 3.0f; const auto st = l.onWindow (win (2.5f, 0.1f, heard), 3000.0);
              if (st.ask.isNotEmpty()) completion = st.ask; }
            int writes = 0, lines = 0;
            for (int i = 0; i < 40; ++i)     // 120 s of windows after landing
            { heard += 3.0f; const auto st = l.onWindow (win (0.1f, 4.0f, heard), 3000.0);
              if (st.writeDrive || st.writeParams || st.writeSlotGain) ++writes;
              if (st.ask.isNotEmpty()) ++lines; }
            check (writes == 0 && lines == 0,
                   "21t-j (settle). AFTER LANDING nothing moves and nothing is said, for 120 s of windows, even "
                   "with the figures well out of band", juce::String (writes) + " write(s), "
                   + juce::String (lines) + " line(s)");
        }
    }

    // ===============================================================================================
    // 21t-i (27 Sep 2026 ruling): THE STORED LEVEL RECORD, at the level of the object every block is composed
    // from. The two-Link, session-reload and reset legs live in ui_guard, where there is a processor.
    // ===============================================================================================
    {
        std::printf ("\n== 21t-i: the stored level record ==\n");
        echojay::LevelTally t (echojay::LevelTally::Weighting::K);
        t.prepare (48000.0);
        auto feed = [&t] (float amp, int hops)
        {
            std::vector<float> buf (4800, 0.0f);
            for (int h = 0; h < hops; ++h)
            {
                for (size_t i = 0; i < buf.size(); ++i)
                    buf[i] = amp * std::sin (6.2831853f * 440.0f * (float) i / 48000.0f);
                t.push (buf.data(), buf.data(), (int) buf.size());
            }
        };
        feed (0.25f, 80);
        echojay::LevelRecord rec;
        rec.updateFromTally (t.snapshot(), 1000000, true);
        check (rec.valid && rec.heardAnything(),
               "21t-i (record). a record made from a tally that has heard audio is valid and has HEARD",
               "HEARD " + juce::String (rec.heardSeconds, 1) + " s, INT " + juce::String (rec.intLufs, 1));
        const auto kept = rec;
        rec.updateFromTally (t.snapshot(), 1100000, true);
        check (std::abs (rec.intLufs - kept.intLufs) < 0.01f && rec.heardSeconds >= kept.heardSeconds,
               "21t-i (record). the transport stopping loses nothing",
               juce::String (rec.intLufs, 1) + " LUFS, HEARD " + juce::String (rec.heardSeconds, 1) + " s");
        t.reset(); t.resetShortTermMax(); feed (0.0005f, 2);
        rec.updateFromTally (t.snapshot(), 1200000, true);
        check (std::abs (rec.intLufs - kept.intLufs) < 0.01f,
               "21t-i (record). a tally that has just been reset cannot erase a figure the record already holds",
               juce::String (rec.intLufs, 1) + " LUFS");
        const auto back = echojay::LevelRecord::fromVar (rec.toVar());
        check (back.valid && std::abs (back.intLufs - rec.intLufs) < 0.01f
               && std::abs (back.heardSeconds - rec.heardSeconds) < 0.01f
               && back.updatedMs == rec.updatedMs,
               "21t-i (record). it survives the round trip through var, figure for figure, with its timestamp",
               "INT " + juce::String (back.intLufs, 1) + ", HEARD " + juce::String (back.heardSeconds, 1)
               + " s, updated " + juce::String (back.updatedMs));
        check (! echojay::LevelRecord{}.heardAnything(),
               "21t-i (record). an empty record is the ONLY thing that reads as no signal", "empty");
        const auto line = back.tokens (back.updatedMs + 7000);
        check (line.contains ("INT ") && line.contains ("SHORTMAX ") && line.contains ("SHORT90 ")
               && line.contains ("PEAK ") && line.contains ("PSR ") && line.contains ("HEARD ")
               && line.contains ("AGE 7") && ! line.contains ("AGE 7s"),
               "21t-i (record). the line carries every ruled token, and AGE as the ruled integer with NO unit "
               "suffix (27 Sep ruling: \"AGE <seconds>\")", line);
    }

    return failures == 0 ? 0 : 1;
}

int main()

{
#if defined(EJ_UNDER_ASAN)
    pthread_attr_t attr;
    pthread_attr_init (&attr);
    pthread_attr_setstacksize (&attr, 512ull * 1024ull * 1024ull);
    pthread_t th {};
    static int rc = 0;
    auto entry = [] (void*) -> void* { rc = guardMain(); return nullptr; };
    if (pthread_create (&th, &attr, entry, nullptr) != 0) return guardMain();
    pthread_join (th, nullptr);
    return rc;
#else
    return guardMain();
#endif
}
