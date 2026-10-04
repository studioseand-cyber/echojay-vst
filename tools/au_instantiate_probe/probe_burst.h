// probe_burst.h - EchoJayProbe's TONE-BURST MODE (roadmap 2.3, compressor timing). PROTOTYPE, 5 Oct 2026 (Phase B2).
//
//   EchoJayProbe "<name>" "<identifier>" <uidHex> --burst quiet=<dBFS> loud=<dBFS> [pre=1.0] [hold=2.0] [post=3.0]
//                [hz=997] [win_ms=5] [set=<idx>:<norm>,...] [reset=0|1]
//
// IT MEASURES; IT DOES NOT DERIVE (decision D2). A phase-continuous sine at `quiet` for `pre` seconds, stepping to `loud`
// for `hold` seconds, stepping back to `quiet` for `post` seconds; the preconditions (`set=`) are written first, the
// amount control among them, so the caller chooses the position. The output's and the input's RMS are printed per window
// of `win_ms` over the whole render, relative to the step times, so EJ Map reads the gain reduction over time, the attack
// (time to 63 % of the final GR after the step up) and the release (time to 37 % of the GR after the step down), and
// never trusts a label. Every write goes through the same three-step verify as the sweep (probe_write.h).
//
// OUTPUT LINES (tab separated):
//   burst   proto 1  quiet <dBFS> loud <dBFS> pre_s <s> hold_s <s> post_s <s> hz <f> win_ms <ms>
//   config  main_in <n> main_out <n> latency <samples>
//   param / set  as the sweep prints them
//   bwin    t_ms <t> seg <pre|loud|post> in_db <rms dBFS> out_db <rms dBFS, power mean over the main outputs>   (t on the input's clock: the
//           output is aligned by the plugin's reported latency, so a step reads where it was made, not where it arrived)
//   bdone   windows <n> nonfinite <n>
#pragma once

#include "probe_sweep.h"

namespace ejprobe
{

struct BurstSpec
{
    double quietDb = -30.0, loudDb = -20.0, preS = 1.0, holdS = 2.0, postS = 3.0, hz = 997.0, winMs = 5.0;
    std::vector<std::pair<int, float>> sets;
    bool reset = false;
};

inline bool parseBurstArgs (int argc, char** argv, int first, BurstSpec& s, juce::String& why)
{
    for (int i = first; i < argc; ++i)
    {
        const juce::String a = juce::String::fromUTF8 (argv[i]);
        const auto k = a.upToFirstOccurrenceOf ("=", false, false), v = a.fromFirstOccurrenceOf ("=", false, false);
        if      (k == "quiet")  s.quietDb = v.getDoubleValue();
        else if (k == "loud")   s.loudDb = v.getDoubleValue();
        else if (k == "pre")    s.preS = v.getDoubleValue();
        else if (k == "hold")   s.holdS = v.getDoubleValue();
        else if (k == "post")   s.postS = v.getDoubleValue();
        else if (k == "hz")     s.hz = v.getDoubleValue();
        else if (k == "win_ms") s.winMs = v.getDoubleValue();
        else if (k == "reset")  s.reset = v.getIntValue() != 0;
        else if (k == "set")
            for (auto& t : juce::StringArray::fromTokens (v, ",", ""))
                s.sets.push_back ({ t.upToFirstOccurrenceOf (":", false, false).getIntValue(), (float) t.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
        else { why = "unknown burst argument '" + a + "'"; return false; }
    }
    if (s.loudDb <= s.quietDb) { why = "loud must be above quiet"; return false; }
    if (s.preS < 0.1 || s.holdS < 0.1 || s.postS < 0.1 || s.winMs < 1.0) { why = "pre, hold, post or win_ms out of range"; return false; }
    return true;
}

inline void runBurst (juce::AudioPluginInstance& p, const BurstSpec& s, const RenderSpec& rs = {})
{
    std::printf ("burst\tproto\t1\tquiet\t%.2f\tloud\t%.2f\tpre_s\t%.3f\thold_s\t%.3f\tpost_s\t%.3f\thz\t%.3f\twin_ms\t%.2f\n", s.quietDb, s.loudDb, s.preS, s.holdS, s.postS, s.hz, s.winMs);
    configureAndPrepare (p, rs);
    SweepRenderer r (p, rs.sampleRate, rs.block, s.hz);
    std::printf ("config\tmain_in\t%d\tmain_out\t%d\tlatency\t%d\n", r.mainIn, r.mainOut, p.getLatencySamples());
    if (r.mainIn == 0 || r.mainOut == 0) { std::printf ("refused no main input or output bus\n"); return; }
    auto ps = p.getParameters();
    for (int i = 0; i < ps.size(); ++i) if (auto* q = ps[i]) std::printf ("param\t%d\t%.6f\t%s\t%s\n", i, q->getValue(), clean (q->getName (128)).toRawUTF8(), clean (q->getCurrentValueAsText()).toRawUTF8());
    for (const auto& [idx, norm] : s.sets)
    {
        if (! juce::isPositiveAndBelow (idx, ps.size()) || ps[idx] == nullptr) { std::printf ("refused set: no parameter %d\n", idx); return; }
        auto& q = *ps[idx];
        q.setValueNotifyingHost (norm);
        const auto l = landWrite (q, norm, &r);
        juce::String text; int reads = 0; stableText (q, text, reads);
        std::printf ("set\t%d\t%.6f\tconfirm_ms\t%.1f\tgetValue\t%.6f\tlanded_by\t%s\trender_blocks\t%d\ttext\t%s\n", idx, norm, l.ms, l.read, l.by, l.blocks, clean (text).toRawUTF8());
        if (l.ms < 0) { std::printf ("refused set_unlanded %d\n", idx); return; }
    }
    if (s.reset) p.reset();
    stage ("burst");
    const double sr = rs.sampleRate;
    const long long nPre = (long long) std::llround (s.preS * sr), nHold = (long long) std::llround (s.holdS * sr), nPost = (long long) std::llround (s.postS * sr), total = nPre + nHold + nPost;
    const long long winN = juce::jmax (1LL, (long long) std::llround (s.winMs * 0.001 * sr));
    const double ampQ = std::pow (10.0, s.quietDb / 20.0), ampL = std::pow (10.0, s.loudDb / 20.0);
    const double step = juce::MathConstants<double>::twoPi * s.hz / sr;
    double phase = 0.0; long long t = 0, nonFinite = 0, windows = 0;
    double inSs = 0.0, outSs = 0.0; long long inWin = 0;
    const int latency = juce::jmax (0, p.getLatencySamples());   // see LATENCY ALIGNMENT below
    auto flush = [&] (long long tEnd)
    {
        const double inDb = inWin > 0 ? 20.0 * std::log10 (std::sqrt (inSs / (double) inWin) + 1e-30) : -999.0;
        const double outDb = inWin > 0 ? 20.0 * std::log10 (std::sqrt (outSs / ((double) inWin * juce::jmax (1, r.mainOut))) + 1e-30) : -999.0;
        const long long tMid = tEnd - inWin / 2;
        const long long tIn = tMid - latency;      // the input time this output window answers to
        const char* seg = tIn < nPre ? "pre" : tIn < nPre + nHold ? "loud" : "post";
        std::printf ("bwin\tt_ms\t%.2f\tseg\t%s\tin_db\t%.3f\tout_db\t%.3f\n", 1000.0 * (double) tIn / sr, seg, inDb, outDb);   // t_ms on the INPUT's clock
        inSs = outSs = 0.0; inWin = 0; ++windows;
    };
    // LATENCY ALIGNMENT: the output of sample t left the plugin `latency` samples after the input that caused it, so the
    // input RMS of a window is taken over the generated samples `latency` earlier (a ring of the last generated samples);
    // without it the window straddling a step reads a +16 dB spike (SBC, latency 52: the release "recovered" in 1 ms).
    std::vector<float> ring ((size_t) latency + (size_t) rs.block + 1, 0.0f); size_t ringPos = 0;
    auto delayed = [&] (size_t writePos) { return ring[(writePos + ring.size() - (size_t) latency) % ring.size()]; };
    while (t < total)
    {
        r.io.clear();
        const int n = (int) juce::jmin ((long long) rs.block, total - t);
        for (int i = 0; i < n; ++i)
        {
            const long long tt = t + i;
            const double amp = (tt >= nPre && tt < nPre + nHold) ? ampL : ampQ;
            const float v = (float) (amp * std::sin (phase));
            phase += step; if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            for (int ch = 0; ch < r.mainIn; ++ch) r.io.setSample (ch, i, v);
        }
        r.midi.clear();
        p.processBlock (r.io, r.midi);
        for (int i = 0; i < n; ++i)
        {
            // the generated sample for slot i again (phase is past the block: step back), written into the ring, read back `latency` earlier
            const long long tt = t + i;
            const double amp = (tt >= nPre && tt < nPre + nHold) ? ampL : ampQ;
            const float g = (float) (amp * std::sin (phase - step * (double) (n - i)));
            ring[ringPos % ring.size()] = g; const float gIn = delayed (ringPos); ++ringPos;
            inSs += (double) gIn * gIn;
            for (int ch = 0; ch < r.mainOut; ++ch) { const float d = r.io.getSample (ch, i); if (! std::isfinite (d)) { ++nonFinite; continue; } outSs += (double) d * d; }
            ++inWin;
            if (inWin >= winN) flush (t + i + 1);
        }
        t += n;
    }
    if (inWin > 0) flush (total);
    std::printf ("bdone\twindows\t%lld\tnonfinite\t%lld\n", windows, nonFinite);
    stage ("done");
}

} // namespace ejprobe
