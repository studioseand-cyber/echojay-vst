/*
  EjmapCategorise.h - CATEGORISING THE UNCATEGORISED (Kathy's NEXT BUILD item F, 7 Oct 2026; CATEGORISE_524_FIX.md read first:
  the server's two-arm categoriser is a queue now, and this proposes OFFLINE, writing nothing to Sean's categories).

  Three steps, each on the record so the review sheet shows why:
    1. OUT OF SCOPE by the AU type code and the name: aumu / augn / aumi (instruments, generators, MIDI processors) -> instrument /
       midi (aumf, a music effect, stays in scope); meter / analyser / analyzer / scope / tuner-display words -> meter; utility words (gain, trim,
       pan, routing, dither, test tone, phase, downmix, stereo-to-mono, send, return, utility, latency) -> utility.
    2. A PROPOSAL from the name, the vendor's line and the ledger's `kind` (the two-arm models' kind string, where one was
       written): the category words -> dynamics / eq / reverb / delay / saturation / pitch / modulation; several -> unsettled.
    3. THE SIGNATURE decides, from four short probe reads at the instantiate state:
         response at -30 and -12 dBFS (121 tones): gain per tone -> level-dependent (the gain at -12 differs from the gain
           at -30 by more than kLevelDb on the same tones) = dynamics; frequency-dependent and level-independent (the gain
           varies across the tones by more than kFreqDb, the same at both levels) = eq
         tail (burst then silence): a decay read (RT60) = reverb; discrete repeats = delay
         harmonics at -30 and -6 dBFS: THD rising with level by more than kThdRiseDb = saturation
         a static detuned note: the output's pitch moved from the input's by more than kPitchCents = pitch; sidebands beside
           the fundamental (modulationOf) = modulation
       The signatures are tried in the order delay, reverb (the words break a tail's reverb / delay tie), pitch, modulation,
       dynamics (a gain moving with level without audible THD), saturation (audible THD, over -50 dB, rising), eq: the first
       that holds decides. None holding, or two
       in conflict with the proposal, = unsettled, on the review sheet with everything read.
  Nothing is exported, nothing is written to categories.json. PURE here (pins CT1-CT6); the driver runs the probe.
*/

#pragma once

#include "EjmapTailRoles.h"
#include "EjmapEq.h"
#include "EjmapReverbDelay.h"
#include "EjmapSaturation.h"
#include "EjmapPitch.h"
#include "EjmapRoleEvidence.h"

namespace ejmap::categorise
{

inline constexpr double kLevelDb = 1.0, kFreqDb = 3.0, kThdRiseDb = 6.0, kThdAudibleDb = -50.0, kPitchCents = 15.0, kTailS = 0.25, kReverbS = 1.0, kSidebandDb = -30.0, kReleaseTailS = 1.5;   // a sideband under -30 dB is the analysis' own floor (-41.6 on a clean unit), never modulation

inline juce::StringArray tokens (const juce::String& s) { auto t = juce::StringArray::fromTokens (s.toLowerCase(), " -_/()[]:.,", "\"'"); t.removeEmptyStrings(); return t; }
inline bool hasAny (const juce::StringArray& tk, std::initializer_list<const char*> ws) { for (const auto& t : tk) for (const char* w : ws) if (t == w) return true; return false; }

// 1. out of scope
inline juce::String outOfScope (const juce::String& typeCode, const juce::String& name, const juce::String& kind)
{
    const auto t4 = typeCode.substring (0, 4);
    if (t4 == "aumu" || t4 == "augn" || t4 == "aumi") return t4 == "aumi" ? "midi" : "instrument";   // aumf (a music effect: an effect taking MIDI) stays IN scope
    const auto tk = tokens (name + " " + kind);
    if (hasAny (tk, { "meter", "meters", "metering", "analyser", "analyzer", "analysis", "scope", "spectrum", "lufs", "loudness", "visualizer", "visualiser", "oscilloscope", "correlation" })) return "meter";
    if (hasAny (tk, { "utility", "trim", "pan", "panner", "routing", "router", "dither", "tone", "generator", "phase", "polarity", "downmix", "downmixer", "mono", "send", "return", "latency", "delaycomp", "monitor", "monitoring", "switcher", "matrix", "patch" })) return "utility";
    return {};
}

// 2. the proposal from words
inline juce::StringArray categoriesFromWords (const juce::String& name, const juce::String& vendor, const juce::String& kind)
{
    const auto tk = tokens (name + " " + kind); juce::StringArray out; (void) vendor;
    if (hasAny (tk, { "comp", "compressor", "compression", "limiter", "limit", "gate", "expander", "dynamics", "leveler", "leveller", "maximizer", "maximiser", "transient", "deesser", "de-esser", "ducker", "opto", "vca", "fet", "bus", "buss", "sbc" })) out.add ("dynamics");
    if (hasAny (tk, { "eq", "equalizer", "equaliser", "filter", "tilt", "shelf", "hpf", "lpf", "lowcut", "highcut", "notch", "exciter", "enhancer" })) out.add ("eq");
    if (hasAny (tk, { "reverb", "verb", "plate", "hall", "room", "chamber", "spring", "ambience", "convolution" })) out.add ("reverb");
    if (hasAny (tk, { "delay", "echo", "tap", "taps", "repeater", "slapback" })) out.add ("delay");
    if (hasAny (tk, { "saturation", "saturator", "sat", "distortion", "drive", "overdrive", "tape", "tube", "valve", "clipper", "clip", "fuzz", "bitcrusher", "crusher", "amp", "amplifier", "cabinet", "console", "preamp", "warmth", "harmonics", "subharmonic", "bass" })) out.add ("saturation");
    if (hasAny (tk, { "pitch", "tuner", "tune", "autotune", "harmonizer", "harmoniser", "vocoder", "formant", "octave", "shifter", "doubler", "transpose" })) out.add ("pitch");
    if (hasAny (tk, { "chorus", "flanger", "phaser", "tremolo", "vibrato", "modulation", "rotary", "leslie", "ensemble", "wobble", "stereo", "imager", "widener", "width", "haas", "auto-pan", "autopan" })) out.add ("modulation");
    return out;
}

// 3. the signatures from the probe reads
struct Signatures
{
    bool responseRead = false, tailRead = false, harmRead = false, pitchRead = false;
    double levelDepDb = 0.0, freqSpanDb = 0.0;            // the largest gain move between the two levels on one tone; the gain span across tones at -30
    std::optional<double> rt60s; int repeats = 0;          // the tail
    double thdLoDb = -999.0, thdHiDb = -999.0;             // THD at the two levels
    double pitchMoveCents = 0.0; std::optional<double> sidebandDb, harmDb;
    juce::StringArray notes;
};
inline double medianOf (std::vector<double> v) { if (v.empty()) return 0.0; std::sort (v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]); }
// the response at two levels: gain per tone at each; level dependence = the median |gain(-12) - gain(-30)| over the tones; frequency span = p90 - p10 of the gain at -30
inline void readResponses (Signatures& s, const eq::Response& lo, const eq::Response& hi)
{
    if (! lo.ok || ! hi.ok || lo.positions.empty() || hi.positions.empty()) { s.notes.add ("response: not read at both levels"); return; }
    const auto& a = lo.positions.front(); const auto& b = hi.positions.front();
    std::vector<double> gA, moves;
    for (const auto& t : a.tones) if (t.inDb > -500.0 && t.outDb > -500.0 && t.hz >= 60.0 && t.hz <= 16000.0) gA.push_back (t.outDb - t.inDb);
    for (size_t i = 0; i < a.tones.size() && i < b.tones.size(); ++i) { const auto& ta = a.tones[i]; const auto& tb = b.tones[i]; if (ta.inDb > -500.0 && ta.outDb > -500.0 && tb.inDb > -500.0 && tb.outDb > -500.0 && ta.hz >= 60.0 && ta.hz <= 16000.0) moves.push_back (std::abs ((tb.outDb - tb.inDb) - (ta.outDb - ta.inDb))); }
    if (gA.size() < 10) { s.notes.add ("response: fewer than 10 readable tones"); return; }
    std::sort (gA.begin(), gA.end()); s.freqSpanDb = gA[(size_t) std::floor (0.9 * (double) (gA.size() - 1))] - gA[(size_t) std::floor (0.1 * (double) (gA.size() - 1))];
    s.levelDepDb = medianOf (moves); s.responseRead = true;
}
inline void readTail (Signatures& s, const reverbdelay::Tail& t)
{
    if (! t.ok) { s.notes.add ("tail: " + (t.refused.isNotEmpty() ? t.refused : juce::String ("not read"))); return; }
    const auto d = reverbdelay::decayOf (t); if (d.ok && (d.t30 ? d.t30RT60s : d.t20RT60s) > 0.0) s.rt60s = d.t30 ? d.t30RT60s : d.t20RT60s;
    const auto o = reverbdelay::onsetsOf (t); s.repeats = (int) o.repeats.size(); s.tailRead = true;
}
inline void readHarmonics (Signatures& s, const saturation::HarmResponse& lo, const saturation::HarmResponse& hi)
{
    if (! lo.ok || ! hi.ok || lo.positions.empty() || hi.positions.empty()) { s.notes.add ("harmonics: not read at both levels"); return; }
    const auto a = saturation::readingFor (lo.positions.front(), -30.0), b = saturation::readingFor (hi.positions.front(), -6.0);
    if (! a.valid || ! b.valid) { s.notes.add ("harmonics: the fundamental was not read at both levels"); return; }
    s.thdLoDb = a.thdDb; s.thdHiDb = b.thdDb; s.sidebandDb = b.sidebandDb; s.harmDb = b.thdDb; s.harmRead = true;
}
inline void readPitch (Signatures& s, const pitch::PitchMeasured& m, double detuneCents)
{
    if (! m.ok || m.positions.empty()) { s.notes.add ("pitch: " + (m.refused.isNotEmpty() ? m.refused : juce::String ("not read"))); return; }
    const auto r = pitch::deriveStrength (m.positions.front(), detuneCents);
    if (r.result != "measured") { s.notes.add ("pitch: " + r.reason); return; }
    s.pitchMoveCents = std::abs (r.residualCents - r.inputCents); s.pitchRead = true;
}

struct Decision { juce::String category, why; bool settled = false; juce::StringArray also; };
// FLAT AT DEFAULT (Kathy, 20 Oct - group 2): a unit whose drive sits at zero shows no signature at all (Lindell 80 / 50 / 69 Bus with THD
// down, Denise Bass XL's Drive, Orange Clip's Input Level). Its DRIVE / INPUT control is found by name, raised to kDriveRaiseNorm, written
// as part of the neutral setting, and the reads are taken again. Every read is RELATIVE - the gain move between two levels at the same
// setting, the spread across the tones, the harmonics against the fundamental - so the louder output a raised drive gives is never the
// effect itself; the decision says how far the level moved and that it is not the signature
inline constexpr double kDriveRaiseNorm = 0.75;
// RAISED FROM ITS DEFAULT (21 Oct, the first real run: StandardCLIP opens its Input Gain above 0.75, and 0.75 took it DOWN 12 dB): the
// drive goes three quarters of the way from its default to the top - 0.75 from 0, never below where it opened; none when it opens at the top
inline std::optional<double> raisedNorm (double defNorm)
{ if (defNorm >= 0.99) return std::nullopt; return juce::jmax (defNorm, defNorm + kDriveRaiseNorm * (1.0 - juce::jmax (0.0, defNorm))); }
// the probe's opening values ("param <idx> <norm> <name> <text>", printed before any write by the response mode)
inline std::optional<double> openingNorm (const juce::String& probeOut, int index)
{
    for (const auto& line : juce::StringArray::fromLines (probeOut))
    {   const auto f = juce::StringArray::fromTokens (line, "\t", ""); if (f.size() >= 3 && f[0] == "param" && f[1].getIntValue() == index && f[1].containsOnly ("0123456789")) return f[2].getDoubleValue(); }
    return std::nullopt;
}
inline bool flatAtDefault (const Signatures& s)
{
    return s.responseRead && s.freqSpanDb < kFreqDb && s.levelDepDb < kLevelDb && ! (s.tailRead && s.rt60s && *s.rt60s >= kTailS)
        && ! (s.harmRead && s.thdHiDb >= kThdAudibleDb);
}
// the drive control: automatable, non-meter controls only (the caller filters); first by the drive words, then an input / pre gain;
// never an output, a mix, a meter, a ceiling, a threshold or a switch
// a SWITCH is never the drive (21 Oct, S1 Imager: "MS/LR Input" is a mode, and raising it changed nothing but the routing): the caller
// leaves out the controls with 2..4 steps; the words of a routing ("ms", "lr", "mid", "side", "stereo") are excluded too
inline std::optional<std::pair<int, juce::String>> driveControl (const std::vector<std::pair<int, juce::String>>& controls)
{
    auto words = [] (const juce::String& n) { return tailroles::wordsOf (n); };
    auto excluded = [] (const juce::StringArray& w) {
        for (const char* t : { "out", "output", "mix", "dry", "wet", "meter", "peak", "ceiling", "threshold", "solo", "link", "mode", "type", "bypass", "power", "phase", "mute", "unity", "trim", "cut", "freq", "frequency", "q", "ms", "lr", "mid", "side", "stereo" })
            if (w.contains (t)) return true;
        return false; };
    for (const auto* tier : { "drive thd saturation sat heat push crunch grit character colour color warmth enhancement enhance excite boost strength amount",
                              "input pre" })
    {
        const auto tierWords = juce::StringArray::fromTokens (tier, " ", "");
        for (const auto& [i, n] : controls)
        {   const auto w = words (n); if (excluded (w)) continue;
            for (const auto& t : tierWords) if (w.contains (t)) return std::make_pair (i, n); }
    }
    return std::nullopt;
}
   // also (20 Oct): a tail with both roles is measured as both
// releaseRule (12 Oct, Kathy): a compressor's RELEASE after the burst reads as a short tail, and the tail check ran first (Lindell 254E:
// RT60 0.40 s, the gain moves 3.14 dB, flat to 0.14 dB -> "reverb or delay"). A gain that moves with level AND is flat across the tones
// (span under kFreqDb: a broadband gain change, not a filter) now wins over a tail shorter than kReleaseTailS. From the data (12 Oct):
// compressors' tails 0.40 (254E) and 0.66 s (XLA-3), flat (0.14, 0.50 dB); reverbs / delays with a tail never level-dependent over
// 0.62 dB; the level-dependent tails in Sean's 605 (CLA Effects 34.7 dB span, FilterFreak2 20.6, Absynth 5 MFX 4.95) are NOT flat and
// stay reverb / delay for review. kReleaseTailS 1.5 s = over twice the longest compressor release seen. false = the rule before 12 Oct
inline Decision decide (const Signatures& s, const juce::StringArray& proposed, bool releaseRule = true, const tailroles::Roles* roles = nullptr)
{
    Decision d;
    auto settle = [&] (const char* c, const juce::String& why) { d.category = c; d.why = why; d.settled = true; return d; };
    // a TAIL is time-based: discrete repeats = delay; otherwise the tail alone cannot tell a feedback delay's decay from a reverb's
    // (bx_delay2500 at instantiate read RT60 2.85 s and no repeat), so the words break the tie and say so; no word = unsettled between the two
    if (s.tailRead && s.repeats >= 2 && (! s.rt60s || *s.rt60s < 1.5)) return settle ("delay", juce::String (s.repeats) + " discrete repeat(s) after the burst");
    if (releaseRule && s.tailRead && s.rt60s && *s.rt60s >= kTailS && *s.rt60s < kReleaseTailS && s.responseRead && s.levelDepDb >= kLevelDb && s.freqSpanDb < kFreqDb)
        return settle ("dynamics", "the gain moves " + juce::String (s.levelDepDb, 2) + " dB between -30 and -12 dBFS, flat across the tones (span " + juce::String (s.freqSpanDb, 2)
                       + " dB): the RT60 " + juce::String (*s.rt60s, 2) + " s tail is a release (under " + juce::String (kReleaseTailS, 1) + " s), not a reverb or delay");
    if (s.tailRead && s.rt60s && *s.rt60s >= kTailS)
    {
        const auto tl = "a tail (RT60 " + juce::String (*s.rt60s, 2) + " s, " + juce::String (s.repeats) + " repeat(s) found)";
        if (proposed.contains ("delay") && ! proposed.contains ("reverb")) return settle ("delay", tl + "; the words say delay");
        if (proposed.contains ("reverb") && ! proposed.contains ("delay")) return settle ("reverb", tl + "; the words say reverb");
        // 20 Oct (Kathy): the CONTROLS decide, section by section - never the categoriser picking one; review only when neither role is found
        if (roles != nullptr && (roles->delay || roles->reverb))
        {
            const auto secs = [] (const juce::StringArray& a) { return "'" + a.joinIntoString ("', '") + "'"; };
            if (roles->delay && roles->reverb)
            { d.category = "reverb"; d.also = { "delay" }; d.settled = true;
              d.why = tl + "; the controls hold a delay (" + secs (roles->delaySections) + ") and a reverb (" + secs (roles->reverbSections) + "): both measured, section by section"; return d; }
            if (roles->delay) return settle ("delay", tl + "; the controls hold a delay (" + secs (roles->delaySections) + ") and no reverb");
            return settle ("reverb", tl + "; the controls hold a reverb (" + secs (roles->reverbSections) + ") and no delay");
        }
        if (*s.rt60s >= kReverbS && s.repeats == 0 && proposed.isEmpty()) { d.category = "reverb"; d.why = tl + " and no word: leaning reverb (a feedback delay reads the same): review"; return d; }
        d.category = proposed.contains ("delay") ? "delay" : "reverb"; d.why = tl + "; reverb or delay: review"; return d;
    }
    if (s.pitchRead && s.pitchMoveCents >= kPitchCents) return settle ("pitch", "the output's pitch moved " + juce::String (s.pitchMoveCents, 1) + " cents from the input's");
    if (s.harmRead && s.sidebandDb && s.harmDb && *s.sidebandDb > kSidebandDb && *s.sidebandDb > *s.harmDb) return settle ("modulation", "energy beside the fundamental (" + juce::String (*s.sidebandDb, 1) + " dB) over the harmonics (" + juce::String (*s.harmDb, 1) + ")");
    // DYNAMICS before saturation: a gain that moves with level and no audible THD is a compressor (Lindell 354E: 2.67 dB, THD -59.8);
    // audible THD rising with level is saturation (over -50 dB at -6 dBFS: a console emulation's -72 is colour, not a saturator)
    if (s.responseRead && s.levelDepDb >= kLevelDb && ! (s.harmRead && s.thdHiDb >= kThdAudibleDb)) return settle ("dynamics", "the gain moves " + juce::String (s.levelDepDb, 2) + " dB between -30 and -12 dBFS on the same tones" + (s.harmRead ? "; THD " + juce::String (s.thdHiDb, 1) + " dB at -6 (not audible)" : juce::String()));
    if (s.harmRead && s.thdHiDb >= kThdAudibleDb && s.thdHiDb - s.thdLoDb >= kThdRiseDb) return settle ("saturation", "THD rises " + juce::String (s.thdHiDb - s.thdLoDb, 1) + " dB from -30 to -6 dBFS (" + juce::String (s.thdHiDb, 1) + " dB at -6)" + (s.responseRead && s.levelDepDb >= kLevelDb ? "; the gain also moves " + juce::String (s.levelDepDb, 2) + " dB with level (a coloured compressor reads here too)" : juce::String()));
    if (s.responseRead && s.levelDepDb >= kLevelDb) return settle ("dynamics", "the gain moves " + juce::String (s.levelDepDb, 2) + " dB between -30 and -12 dBFS on the same tones");
    if (s.responseRead && s.freqSpanDb >= kFreqDb && s.levelDepDb < kLevelDb) return settle ("eq", "the gain spans " + juce::String (s.freqSpanDb, 1) + " dB across the tones, level-independent");
    // nothing held: the proposal alone decides only when it is a single word AND the reads say "inert at instantiate" is plausible
    if (proposed.size() == 1 && s.responseRead && s.freqSpanDb < kFreqDb && s.levelDepDb < kLevelDb) { d.category = proposed[0]; d.why = "no signature at the instantiate state (flat, level-independent, no tail, no THD rise); the name's single proposal stands, unsettled: review"; return d; }
    d.category = proposed.size() == 1 ? proposed[0] : juce::String(); d.why = "no signature held" + (proposed.isEmpty() ? juce::String ("; no word proposed") : "; proposed by words: " + proposed.joinIntoString (" / ")) + ": review"; return d;
}

struct NeutralWrite { int index = -1; double norm = 0.0; juce::String control, text, why; };
struct Row { juce::String product, vendor, typeCode, kind, identity, outOfScope, category, why; juce::StringArray proposed, also; bool settled = false, measured = false; Signatures sig; double seconds = 0.0;
             std::vector<NeutralWrite> neutralWrites; std::optional<double> levelMoveDb; std::optional<Signatures> sigAtDefault; };
// the broadband LEVEL of a response at its first position: the median gain (out - in) over the readable tones, 60 Hz .. 16 kHz
inline std::optional<double> levelOf (const eq::Response& r)
{
    if (! r.ok || r.positions.empty()) return std::nullopt;
    std::vector<double> g; for (const auto& t : r.positions.front().tones) if (t.inDb > -500.0 && t.outDb > -500.0 && t.hz >= 60.0 && t.hz <= 16000.0) g.push_back (t.outDb - t.inDb);
    if (g.size() < 10) return std::nullopt; return medianOf (g);
}
// DECIDE AGAIN WITH THE DRIVE RAISED (Kathy, 20 Oct - group 2). The reads are taken again at the raised drive and decided by the same
// rule; a LEVEL change is never the effect: when the raised reads are still flat at default the answer stays unsettled, whatever the
// output level did, and the why says how far it moved
inline Decision decideRaised (const Signatures& raised, const juce::StringArray& proposed, const tailroles::Roles* roles,
                              const NeutralWrite& drive, std::optional<double> levelMoveDb)
{
    const auto lvl = levelMoveDb ? "; the output level moved " + juce::String (*levelMoveDb, 1) + " dB (a level change, not counted as the signature)" : juce::String ("; the level move was not read");
    const auto raisedWhy = "flat at default; '" + drive.control + "' raised to " + juce::String (drive.norm, 2) + (drive.text.isNotEmpty() ? " ('" + drive.text + "')" : juce::String()) + " as part of the neutral setting" + lvl;
    if (flatAtDefault (raised))
    { Decision d; d.category = proposed.size() == 1 ? proposed[0] : juce::String(); d.why = raisedWhy + "; still no signature with it raised: review"; return d; }
    auto d = decide (raised, proposed, true, roles); d.why = raisedWhy + "; then " + d.why; return d;
}
inline juce::var sigVar (const Signatures& s)
{
    auto* sg = new juce::DynamicObject();
    if (s.responseRead) { sg->setProperty ("level_dependence_db", std::round (s.levelDepDb * 100.0) / 100.0); sg->setProperty ("frequency_span_db", std::round (s.freqSpanDb * 100.0) / 100.0); }
    if (s.tailRead) { sg->setProperty ("rt60_s", s.rt60s ? juce::var (std::round (*s.rt60s * 100.0) / 100.0) : juce::var()); sg->setProperty ("repeats", s.repeats); }
    if (s.harmRead) { sg->setProperty ("thd_db_at_m30", std::round (s.thdLoDb * 10.0) / 10.0); sg->setProperty ("thd_db_at_m6", std::round (s.thdHiDb * 10.0) / 10.0); if (s.sidebandDb) sg->setProperty ("sideband_db", std::round (*s.sidebandDb * 10.0) / 10.0); }
    if (s.pitchRead) sg->setProperty ("pitch_move_cents", std::round (s.pitchMoveCents * 10.0) / 10.0);
    juce::Array<juce::var> ns; for (const auto& n : s.notes) ns.add (n); sg->setProperty ("notes", ns);
    return juce::var (sg);
}
inline juce::var rowVar (const Row& r)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("product", r.product); o->setProperty ("vendor", r.vendor); o->setProperty ("type", r.typeCode); o->setProperty ("identity", r.identity); if (r.kind.isNotEmpty()) o->setProperty ("kind", r.kind);
    if (r.outOfScope.isNotEmpty()) { o->setProperty ("out_of_scope", r.outOfScope); o->setProperty ("category", r.outOfScope); o->setProperty ("settled", true); return juce::var (o); }
    o->setProperty ("proposed_by_words", r.proposed.joinIntoString (", ")); o->setProperty ("category", r.category.isNotEmpty() ? juce::var (r.category) : juce::var()); o->setProperty ("settled", r.settled); o->setProperty ("why", r.why); o->setProperty ("measured", r.measured);
    if (! r.also.isEmpty()) o->setProperty ("also_categories", juce::Array<juce::var> (r.also.begin(), r.also.size()));   // 20 Oct: measured as both
    if (! r.neutralWrites.empty())   // 20 Oct (group 2): the drive raised for the reads, part of the plugin's neutral setting
    {   juce::Array<juce::var> nw;
        for (const auto& w : r.neutralWrites) { auto* x = new juce::DynamicObject(); x->setProperty ("index", w.index); x->setProperty ("control", w.control); x->setProperty ("norm", w.norm);
                                                if (w.text.isNotEmpty()) x->setProperty ("text", w.text); x->setProperty ("why", w.why); nw.add (juce::var (x)); }
        o->setProperty ("neutral_writes", nw); if (r.levelMoveDb) o->setProperty ("level_move_db", *r.levelMoveDb); }
    if (r.measured)
    {
        o->setProperty ("signatures", sigVar (r.sig));
        if (r.sigAtDefault) o->setProperty ("signatures_at_default", sigVar (*r.sigAtDefault));   // 20 Oct: the reads before the drive was raised
        o->setProperty ("seconds", std::round (r.seconds));
    }
    return juce::var (o);
}
inline juce::String reviewSheet (const std::vector<Row>& rows)
{
    juce::String s; int scope = 0, settled = 0, unsettled = 0; std::map<juce::String, int> byCat;
    for (const auto& r : rows) { if (r.outOfScope.isNotEmpty()) { ++scope; ++byCat["out of scope: " + r.outOfScope]; } else if (r.settled) { ++settled; ++byCat[r.category]; } else ++unsettled; }
    s << "CATEGORY PROPOSALS: " << (int) rows.size() << " product(s): " << scope << " out of scope, " << settled << " settled by signature, " << unsettled << " unsettled (below). Nothing written to categories.json.\n";
    for (const auto& [c, n] : byCat) s << "  " << c << ": " << n << "\n";
    s << "\nUNSETTLED - for review (what the words proposed, what the reads said):\n";
    for (const auto& r : rows) if (r.outOfScope.isEmpty() && ! r.settled) s << "  " << r.product << " [" << r.vendor << (r.kind.isNotEmpty() ? ", kind '" + r.kind + "'" : juce::String()) << "]: " << (r.category.isNotEmpty() ? "leaning " + r.category + "; " : juce::String()) << r.why << "\n";
    s << "\nSETTLED (category: why):\n";
    for (const auto& r : rows) if (r.outOfScope.isEmpty() && r.settled) s << "  " << r.product << " -> " << r.category << ": " << r.why << (r.proposed.isEmpty() || r.proposed.contains (r.category) ? juce::String() : "  (the words proposed " + r.proposed.joinIntoString (" / ") + ")") << "\n";
    return s;
}

} // namespace ejmap::categorise
