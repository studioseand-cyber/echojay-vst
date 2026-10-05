// probe_dynamics.h - EchoJayProbe's DRUM-HIT and LEVEL-RAMP MODES (roadmap 2.8, transient shapers and gates).
// PROTOTYPE, 5 Oct 2026 (overnight run 2, R5).
//
//   EchoJayProbe "<name>" "<identifier>" <uidHex> --hits [db=-6] [hz=997] [decay_ms=150] [period_ms=600] [hits=4] [win_ms=1]
//                [set=<idx>:<norm>,...] [reset=0|1]
//   EchoJayProbe "<name>" "<identifier>" <uidHex> --ramp [from=-60] [to=-6] [up_s=3] [down_s=3] [hz=997] [win_ms=5]
//                [set=<idx>:<norm>,...] [reset=0|1]
//
// IT MEASURES; IT DOES NOT DERIVE (decision D2).
// --hits: a drum-like burst - a sine at `hz` whose envelope rises in one sample to `db` dBFS peak and falls exponentially
//   with the time constant `decay_ms` (-60 dB at ~6.9 time constants) - repeated `hits` times every `period_ms`, after a
//   settling second of silence. Per window of `win_ms`: the input's and output's RMS and peak, on the input's clock (the
//   output aligned by the plugin's reported latency). EJ Map reads the transient (the first windows after each onset)
//   and the sustain (the body) against the input, per position of the attack / sustain control.
// --ramp: a sine at `hz` whose level rises linearly in dB from `from` to `to` over `up_s`, holds nothing, and falls back over
//   `down_s`. Per window: input and output RMS. EJ Map reads the level at which a gate opens on the way up and closes on
//   the way down (hysteresis), and how far it attenuates when closed (range).
//
// OUTPUT LINES (tab separated):
//   hits    proto 1  db <dBFS> hz <f> decay_ms <ms> period_ms <ms> hits <n> win_ms <ms>
//   hwin    t_ms <t> hit <k or -1> in_db <rms> out_db <rms> in_peak_db <d> out_peak_db <d>     (t from the first hit's onset; hit = the index of the hit the window belongs to)
//   hdone   windows <n> nonfinite <n>
//   ramp    proto 1  from <dBFS> to <dBFS> up_s <s> down_s <s> hz <f> win_ms <ms>
//   rwin    t_ms <t> seg <up|down> in_db <rms> out_db <rms>
//   rdone   windows <n> nonfinite <n>
#pragma once

#include "probe_sweep.h"

namespace ejprobe
{

struct HitsSpec { double dbfs = -6.0, hz = 997.0, decayMs = 150.0, periodMs = 600.0, winMs = 1.0; int hits = 4; std::vector<std::pair<int, float>> sets; bool reset = false; };
struct RampSpec { double fromDb = -60.0, toDb = -6.0, upS = 3.0, downS = 3.0, hz = 997.0, winMs = 5.0; std::vector<std::pair<int, float>> sets; bool reset = false; };

inline bool parseSets (const juce::String& v, std::vector<std::pair<int, float>>& sets)
{
    for (auto& t : juce::StringArray::fromTokens (v, ",", "")) sets.push_back ({ t.upToFirstOccurrenceOf (":", false, false).getIntValue(), (float) t.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
    return true;
}

inline bool parseHitsArgs (int argc, char** argv, int first, HitsSpec& s, juce::String& why)
{
    for (int i = first; i < argc; ++i)
    {
        const juce::String a = juce::String::fromUTF8 (argv[i]);
        const auto k = a.upToFirstOccurrenceOf ("=", false, false), v = a.fromFirstOccurrenceOf ("=", false, false);
        if      (k == "db")        s.dbfs = v.getDoubleValue();
        else if (k == "hz")        s.hz = v.getDoubleValue();
        else if (k == "decay_ms")  s.decayMs = v.getDoubleValue();
        else if (k == "period_ms") s.periodMs = v.getDoubleValue();
        else if (k == "hits")      s.hits = v.getIntValue();
        else if (k == "win_ms")    s.winMs = v.getDoubleValue();
        else if (k == "reset")     s.reset = v.getIntValue() != 0;
        else if (k == "set")       parseSets (v, s.sets);
        else { why = "unknown hits argument '" + a + "'"; return false; }
    }
    if (s.dbfs > 0.0 || s.hz < 5.0 || s.decayMs < 5.0 || s.periodMs < s.decayMs * 2.0 || s.hits < 1 || s.hits > 32 || s.winMs < 0.25) { why = "db, hz, decay_ms, period_ms, hits or win_ms out of range"; return false; }
    return true;
}
inline bool parseRampArgs (int argc, char** argv, int first, RampSpec& s, juce::String& why)
{
    for (int i = first; i < argc; ++i)
    {
        const juce::String a = juce::String::fromUTF8 (argv[i]);
        const auto k = a.upToFirstOccurrenceOf ("=", false, false), v = a.fromFirstOccurrenceOf ("=", false, false);
        if      (k == "from")   s.fromDb = v.getDoubleValue();
        else if (k == "to")     s.toDb = v.getDoubleValue();
        else if (k == "up_s")   s.upS = v.getDoubleValue();
        else if (k == "down_s") s.downS = v.getDoubleValue();
        else if (k == "hz")     s.hz = v.getDoubleValue();
        else if (k == "win_ms") s.winMs = v.getDoubleValue();
        else if (k == "reset")  s.reset = v.getIntValue() != 0;
        else if (k == "set")    parseSets (v, s.sets);
        else { why = "unknown ramp argument '" + a + "'"; return false; }
    }
    if (s.toDb <= s.fromDb || s.toDb > 0.0 || s.upS < 0.2 || s.downS < 0.2 || s.hz < 5.0 || s.winMs < 0.25) { why = "from, to, up_s, down_s, hz or win_ms out of range"; return false; }
    return true;
}

// the shared preamble: configure, prepare, print the params, land the writes; false = refused (said)
inline bool dynamicsPreamble (juce::AudioPluginInstance& p, SweepRenderer& r, const std::vector<std::pair<int, float>>& sets, bool reset)
{
    std::printf ("config\tmain_in\t%d\tmain_out\t%d\tlatency\t%d\n", r.mainIn, r.mainOut, p.getLatencySamples());
    if (r.mainIn == 0 || r.mainOut == 0) { std::printf ("refused no main input or output bus\n"); return false; }
    auto ps = p.getParameters();
    for (int i = 0; i < ps.size(); ++i) if (auto* q = ps[i]) std::printf ("param\t%d\t%.6f\t%s\t%s\n", i, q->getValue(), clean (q->getName (128)).toRawUTF8(), clean (q->getCurrentValueAsText()).toRawUTF8());
    for (const auto& [idx, norm] : sets)
    {
        if (! juce::isPositiveAndBelow (idx, ps.size()) || ps[idx] == nullptr) { std::printf ("refused set: no parameter %d\n", idx); return false; }
        auto& q = *ps[idx];
        q.setValueNotifyingHost (norm);
        const auto l = landWrite (q, norm, &r);
        juce::String text; int reads = 0; stableText (q, text, reads);
        std::printf ("set\t%d\t%.6f\tconfirm_ms\t%.1f\tgetValue\t%.6f\tlanded_by\t%s\trender_blocks\t%d\ttext\t%s\n", idx, norm, l.ms, l.read, l.by, l.blocks, clean (text).toRawUTF8());
        if (l.ms < 0) { std::printf ("refused set_unlanded %d\n", idx); return false; }
    }
    if (reset) p.reset();
    // a settling second of silence: the measurement starts from a quiet, settled plugin
    for (long long done = 0; done < (long long) r.sr; done += r.block) { r.io.clear(); r.midi.clear(); p.processBlock (r.io, r.midi); }
    return true;
}

// THE RENDER LOOP shared by both modes: gen(t) gives the input sample at time t (samples); per window the printer is called
// with (tMsOnInputClock, inRms, outRms, inPeak, outPeak). Latency alignment as the burst mode's (a ring of generated samples).
template <typename Gen, typename Print>
inline void renderWindows (juce::AudioPluginInstance& p, SweepRenderer& r, long long total, long long winN, Gen gen, Print print, long long& windows, long long& nonFinite)
{
    const int latency = juce::jmax (0, p.getLatencySamples());
    std::vector<float> ring ((size_t) latency + (size_t) r.block + 1, 0.0f); size_t ringPos = 0;
    auto delayed = [&] (size_t writePos) { return ring[(writePos + ring.size() - (size_t) latency) % ring.size()]; };
    double inSs = 0.0, outSs = 0.0, inPk = 0.0, outPk = 0.0; long long inWin = 0; long long t = 0;
    auto flush = [&] (long long tEnd)
    {
        const double inDb = inWin > 0 ? 20.0 * std::log10 (std::sqrt (inSs / (double) inWin) + 1e-30) : -999.0;
        const double outDb = inWin > 0 ? 20.0 * std::log10 (std::sqrt (outSs / ((double) inWin * juce::jmax (1, r.mainOut))) + 1e-30) : -999.0;
        const long long tIn = (tEnd - inWin / 2) - latency;
        print (1000.0 * (double) tIn / r.sr, inDb, outDb, 20.0 * std::log10 (inPk + 1e-30), 20.0 * std::log10 (outPk + 1e-30));
        inSs = outSs = 0.0; inPk = outPk = 0.0; inWin = 0; ++windows;
    };
    const long long renderTotal = total + latency;
    while (t < renderTotal)
    {
        r.io.clear();
        const int n = (int) juce::jmin ((long long) r.block, renderTotal - t);
        for (int i = 0; i < n; ++i) { const float v = gen (t + i); for (int ch = 0; ch < r.mainIn; ++ch) r.io.setSample (ch, i, v); }
        r.midi.clear(); p.processBlock (r.io, r.midi);
        for (int i = 0; i < n; ++i)
        {
            const float g = gen (t + i);
            ring[ringPos % ring.size()] = g; const float gIn = delayed (ringPos); ++ringPos;
            if (t + i < latency) continue;
            inSs += (double) gIn * gIn; inPk = juce::jmax (inPk, (double) std::abs (gIn));
            for (int ch = 0; ch < r.mainOut; ++ch) { const float d = r.io.getSample (ch, i); if (! std::isfinite (d)) { ++nonFinite; continue; } outSs += (double) d * d; outPk = juce::jmax (outPk, (double) std::abs (d)); }
            ++inWin;
            if (inWin >= winN) flush (t + i + 1);
        }
        t += n;
    }
    if (inWin > 0) flush (renderTotal);
}

inline void runHits (juce::AudioPluginInstance& p, const HitsSpec& s, const RenderSpec& rs = {})
{
    std::printf ("hits\tproto\t1\tdb\t%.2f\thz\t%.3f\tdecay_ms\t%.2f\tperiod_ms\t%.2f\thits\t%d\twin_ms\t%.2f\n", s.dbfs, s.hz, s.decayMs, s.periodMs, s.hits, s.winMs);
    configureAndPrepare (p, rs);
    SweepRenderer r (p, rs.sampleRate, rs.block, s.hz);
    if (! dynamicsPreamble (p, r, s.sets, s.reset)) return;
    stage ("hits");
    const double sr = rs.sampleRate;
    const long long period = (long long) std::llround (s.periodMs * 0.001 * sr), total = period * s.hits;
    const long long winN = juce::jmax (1LL, (long long) std::llround (s.winMs * 0.001 * sr));
    const double amp = std::pow (10.0, s.dbfs / 20.0), tau = s.decayMs * 0.001 * sr, step = juce::MathConstants<double>::twoPi * s.hz / sr;
    auto gen = [&] (long long t) -> float { if (t >= total) return 0.0f; const long long within = t % period; return (float) (amp * std::exp (-(double) within / tau) * std::sin (step * (double) t)); };
    long long windows = 0, nonFinite = 0;
    renderWindows (p, r, total, winN, gen, [&] (double tMs, double inDb, double outDb, double inPk, double outPk)
    {
        const int hit = tMs < 0.0 ? -1 : (int) (tMs / s.periodMs);
        std::printf ("hwin\tt_ms\t%.3f\thit\t%d\tin_db\t%.3f\tout_db\t%.3f\tin_peak_db\t%.3f\tout_peak_db\t%.3f\n", tMs, hit < s.hits ? hit : -1, inDb, outDb, inPk, outPk);
    }, windows, nonFinite);
    std::printf ("hdone\twindows\t%lld\tnonfinite\t%lld\n", windows, nonFinite);
    stage ("done");
}

inline void runRamp (juce::AudioPluginInstance& p, const RampSpec& s, const RenderSpec& rs = {})
{
    std::printf ("ramp\tproto\t1\tfrom\t%.2f\tto\t%.2f\tup_s\t%.3f\tdown_s\t%.3f\thz\t%.3f\twin_ms\t%.2f\n", s.fromDb, s.toDb, s.upS, s.downS, s.hz, s.winMs);
    configureAndPrepare (p, rs);
    SweepRenderer r (p, rs.sampleRate, rs.block, s.hz);
    if (! dynamicsPreamble (p, r, s.sets, s.reset)) return;
    stage ("ramp");
    const double sr = rs.sampleRate;
    const long long nUp = (long long) std::llround (s.upS * sr), nDown = (long long) std::llround (s.downS * sr), total = nUp + nDown;
    const long long winN = juce::jmax (1LL, (long long) std::llround (s.winMs * 0.001 * sr));
    const double step = juce::MathConstants<double>::twoPi * s.hz / sr;
    auto levelAt = [&] (long long t) { if (t >= total) return s.fromDb; if (t < nUp) return s.fromDb + (s.toDb - s.fromDb) * (double) t / (double) nUp; return s.toDb - (s.toDb - s.fromDb) * (double) (t - nUp) / (double) nDown; };
    auto gen = [&] (long long t) -> float { if (t >= total) return 0.0f; return (float) (std::pow (10.0, levelAt (t) / 20.0) * std::sin (step * (double) t)); };
    long long windows = 0, nonFinite = 0;
    renderWindows (p, r, total, winN, gen, [&] (double tMs, double inDb, double outDb, double, double)
    {
        std::printf ("rwin\tt_ms\t%.3f\tseg\t%s\tin_db\t%.3f\tout_db\t%.3f\n", tMs, tMs < s.upS * 1000.0 ? "up" : "down", inDb, outDb);
    }, windows, nonFinite);
    std::printf ("rdone\twindows\t%lld\tnonfinite\t%lld\n", windows, nonFinite);
    stage ("done");
}

} // namespace ejprobe
