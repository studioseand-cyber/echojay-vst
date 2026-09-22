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
#include "EedLimiterProcessor.h"   // force-link the built-in's registrar
#include "EedDeviceRegistry.h"
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

int main()
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
        check (lv.measured && std::abs (lv.out.shortTermDb - lv.in.shortTermDb) < 0.3f, "P1. ...and the slot ends +0.0 +-0.3 dB (out - in over the short-term window)", f1 (lv.out.shortTermDb - lv.in.shortTermDb));
        check (r.h.slotTrimText (1).endsWith (" dB match") && r.h.slotTrimText (1).startsWith ("-") && r.h.getSlotInfo (1).trimText == r.h.slotTrimText (1),
               "P1. the tile and the card line read \"-X.X dB match\" from the same atomic", r.h.slotTrimText (1));
        check (std::abs (r.h.getSlotTrimDb (0)) < 0.01f && std::abs (r.h.getSlotTrimDb (2)) < 0.01f && r.h.slotTrimText (0).isEmpty() && r.h.slotTrimText (2).isEmpty(),
               "P2. the Level slot and the last limiter are exempt (no trim, no text)", f1 (r.h.getSlotTrimDb (0)) + " / " + f1 (r.h.getSlotTrimDb (2)));
        check (r.logs.joinIntoString ("\n").contains ("unity trim: slot 1 EchoJay Gain: out-in ") && r.logs.joinIntoString ("\n").contains ("unity trims changed: 1"), "P2. the EJLoudness log carries the trim line", r.logs.joinIntoString (" | ").fromFirstOccurrenceOf ("unity", false, false).substring (0, 160));
        // the loop's opening gain assumes a unity chain: after Go the output lands on the target even though the window measured the un-trimmed chain
        {   // the window measured the UN-trimmed chain (-14 = -18 programme + 4 dB); the proposal must be computed from the unity chain (-18): needed +9, not +5
            juce::String ml; for (const auto& l : r.logs) if (l.contains ("measured: max short-term")) ml = l;
            check (ml.contains ("measured: max short-term -18.0 LUFS") && ml.contains ("needed +9.0 dB"), "P5. the proposal is computed from the UNITY chain: measured -18.0 (the -14.0 window plus the -4 dB trim), needed +9.0 (RED as it stood: -14.0 / +5.0)", ml.substring (0, 140));
            juce::String gc; for (const auto& l : r.logs) if (l.contains ("GR cap")) gc = l; std::printf ("  P5 cap line: %s\n", gc.substring (0, 200).toRawUTF8());
        }
        check (r.loop.state() == LoudnessLoop::State::proposed && r.loop.go(), "P5. the proposal is offered and Go applies it", juce::String ((int) r.loop.state()));
        check (r.levelGain() > 5.5f, "P5. ...and the Level moves by more than the +5 the un-trimmed window would have asked (the +9 unity ask, or its GR-capped value)", f1 (r.levelGain()));
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
        { const auto b = bodyOf (r.proc, r.h); check (r.h.hasActiveTrims() && b.contains ("\"unityChain\":true"), "Q3. after Listen the +4 dB slot carries its trim -> the body carries \"unityChain\":true (both chat and chat-stream build through buildChatRequestBody)", b.fromFirstOccurrenceOf ("\"appVersion\"", false, false).substring (0, 80)); }
        r.h.setSlotBypassed (1, true);
        { const auto b = bodyOf (r.proc, r.h); check (! r.h.hasActiveTrims() && ! b.contains ("unityChain"), "Q4. the trimmed slot bypassed -> no live trim, the field is absent again", b.substring (0, 60)); }
    }
    std::printf ("\n==== loudness_loop_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
