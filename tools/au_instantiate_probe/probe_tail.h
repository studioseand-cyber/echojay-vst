// probe_tail.h - EchoJayProbe's TAIL MODE (roadmap 2.7, reverb and delay). PROTOTYPE, 5 Oct 2026 (overnight run 2, R4).
//
//   EchoJayProbe "<name>" "<identifier>" <uidHex> --tail [db=-12] [burst_ms=200] [tail_s=6] [hz=997] [win_ms=1] [tempo=0]
//                [set=<idx>:<norm>,...] [reset=0|1]
//
// IT MEASURES; IT DOES NOT DERIVE (decision D2). A sine burst (`burst_ms` at `db` dBFS peak, `hz`) then silence for
// `tail_s`; the preconditions (`set=`) are written first (mix, time, feedback, decay - the caller chooses). The output's
// RMS per window of `win_ms` is printed over the whole render on the INPUT's clock (the output aligned by the plugin's
// reported latency, as the burst mode does), with the input's RMS beside it, so EJ Map reads the dry level at the burst's
// onset, the wet level and its decay after the burst ends, the time to the first echo, and the spacing and fall of the
// repeats - and never trusts a label. The windows also carry the output's peak so a single echo in a quiet window is seen.
//
// HOST TEMPO (`tempo=<bpm>`, > 0): the plugin is given a playhead - playing, 4/4, the given bpm, ppq advancing with the
// render - so a tempo-synced delay has a tempo to sync to. tempo=0 (default) leaves the plugin with no playhead, as the
// other modes do. The playhead is this mode's own and is removed before the plugin is released.
//
// OUTPUT LINES (tab separated):
//   tail    proto 1  db <dBFS> burst_ms <ms> tail_s <s> hz <f> win_ms <ms> tempo <bpm>
//   config  main_in <n> main_out <n> latency <samples>
//   param / set  as the sweep prints them
//   twin    t_ms <t> seg <burst|tail> in_db <rms dBFS> out_db <rms dBFS, power mean over the main outputs> out_peak_db <dBFS>
//   tdone   windows <n> nonfinite <n>
#pragma once

#include "probe_sweep.h"

namespace ejprobe
{

struct TailSpec
{
    double dbfs = -12.0, burstMs = 200.0, tailS = 6.0, hz = 997.0, winMs = 1.0, tempo = 0.0;
    std::vector<std::pair<int, float>> sets;
    bool reset = false;
};

inline bool parseTailArgs (int argc, char** argv, int first, TailSpec& s, juce::String& why)
{
    for (int i = first; i < argc; ++i)
    {
        const juce::String a = juce::String::fromUTF8 (argv[i]);
        const auto k = a.upToFirstOccurrenceOf ("=", false, false), v = a.fromFirstOccurrenceOf ("=", false, false);
        if      (k == "db")       s.dbfs = v.getDoubleValue();
        else if (k == "burst_ms") s.burstMs = v.getDoubleValue();
        else if (k == "tail_s")   s.tailS = v.getDoubleValue();
        else if (k == "hz")       s.hz = v.getDoubleValue();
        else if (k == "win_ms")   s.winMs = v.getDoubleValue();
        else if (k == "tempo")    s.tempo = v.getDoubleValue();
        else if (k == "reset")    s.reset = v.getIntValue() != 0;
        else if (k == "set")
            for (auto& t : juce::StringArray::fromTokens (v, ",", ""))
                s.sets.push_back ({ t.upToFirstOccurrenceOf (":", false, false).getIntValue(), (float) t.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
        else { why = "unknown tail argument '" + a + "'"; return false; }
    }
    if (s.burstMs < 1.0 || s.tailS < 0.1 || s.tailS > 60.0 || s.winMs < 0.25 || s.hz < 5.0 || s.dbfs > 0.0) { why = "db, burst_ms, tail_s, win_ms or hz out of range"; return false; }
    if (s.tempo < 0.0 || s.tempo > 400.0) { why = "tempo out of range"; return false; }
    return true;
}

// A HOST PLAYHEAD for the tempo-synced case: playing, 4/4, `bpm`, the position advancing with every block rendered.
struct TailPlayHead : juce::AudioPlayHead
{
    double sr = 48000.0, bpm = 120.0; juce::int64 samples = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo pi; pi.setIsPlaying (true); pi.setIsRecording (false); pi.setBpm (bpm); pi.setTimeSignature (TimeSignature { 4, 4 });
        pi.setTimeInSamples (samples); pi.setTimeInSeconds ((double) samples / sr);
        const double ppq = (double) samples / sr * bpm / 60.0; pi.setPpqPosition (ppq); pi.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        return pi;
    }
};

inline void runTail (juce::AudioPluginInstance& p, const TailSpec& s, const RenderSpec& rs = {})
{
    std::printf ("tail\tproto\t1\tdb\t%.2f\tburst_ms\t%.2f\ttail_s\t%.3f\thz\t%.3f\twin_ms\t%.2f\ttempo\t%.2f\n", s.dbfs, s.burstMs, s.tailS, s.hz, s.winMs, s.tempo);
    TailPlayHead head; head.sr = rs.sampleRate; head.bpm = s.tempo;
    if (s.tempo > 0.0) p.setPlayHead (&head);   // before prepare: a plugin may read the tempo there
    configureAndPrepare (p, rs);
    SweepRenderer r (p, rs.sampleRate, rs.block, s.hz);
    std::printf ("config\tmain_in\t%d\tmain_out\t%d\tlatency\t%d\n", r.mainIn, r.mainOut, p.getLatencySamples());
    if (r.mainIn == 0 || r.mainOut == 0) { std::printf ("refused no main input or output bus\n"); if (s.tempo > 0.0) p.setPlayHead (nullptr); return; }
    auto ps = p.getParameters();
    for (int i = 0; i < ps.size(); ++i) if (auto* q = ps[i]) std::printf ("param\t%d\t%.6f\t%s\t%s\n", i, q->getValue(), clean (q->getName (128)).toRawUTF8(), clean (q->getCurrentValueAsText()).toRawUTF8());
    for (const auto& [idx, norm] : s.sets)
    {
        if (! juce::isPositiveAndBelow (idx, ps.size()) || ps[idx] == nullptr) { std::printf ("refused set: no parameter %d\n", idx); if (s.tempo > 0.0) p.setPlayHead (nullptr); return; }
        auto& q = *ps[idx];
        q.setValueNotifyingHost (norm);
        const auto l = landWrite (q, norm, &r);
        head.samples += (juce::int64) l.blocks * rs.block;
        juce::String text; int reads = 0; stableText (q, text, reads);
        std::printf ("set\t%d\t%.6f\tconfirm_ms\t%.1f\tgetValue\t%.6f\tlanded_by\t%s\trender_blocks\t%d\ttext\t%s\n", idx, norm, l.ms, l.read, l.by, l.blocks, clean (text).toRawUTF8());
        if (l.ms < 0) { std::printf ("refused set_unlanded %d\n", idx); if (s.tempo > 0.0) p.setPlayHead (nullptr); return; }
    }
    if (s.reset) p.reset();
    // A SETTLING RENDER of silence first (a tempo-synced delay reads its tempo on the first blocks; a reverb's modulation
    // starts): half a second, nothing printed - the measurement starts from a quiet plugin
    for (long long done = 0; done < (long long) (0.5 * rs.sampleRate); done += rs.block) { r.io.clear(); r.midi.clear(); p.processBlock (r.io, r.midi); head.samples += rs.block; }
    stage ("tail");
    const double sr = rs.sampleRate;
    const long long nBurst = (long long) std::llround (s.burstMs * 0.001 * sr), nTail = (long long) std::llround (s.tailS * sr), total = nBurst + nTail;
    const long long winN = juce::jmax (1LL, (long long) std::llround (s.winMs * 0.001 * sr));
    const double amp = std::pow (10.0, s.dbfs / 20.0);
    const double step = juce::MathConstants<double>::twoPi * s.hz / sr;
    double phase = 0.0; long long t = 0, nonFinite = 0, windows = 0;
    double inSs = 0.0, outSs = 0.0, outPeak = 0.0; long long inWin = 0;
    const int latency = juce::jmax (0, p.getLatencySamples());
    auto flush = [&] (long long tEnd)
    {
        const double inDb = inWin > 0 ? 20.0 * std::log10 (std::sqrt (inSs / (double) inWin) + 1e-30) : -999.0;
        const double outDb = inWin > 0 ? 20.0 * std::log10 (std::sqrt (outSs / ((double) inWin * juce::jmax (1, r.mainOut))) + 1e-30) : -999.0;
        const long long tMid = tEnd - inWin / 2;
        const long long tIn = tMid - latency;
        const char* seg = tIn < nBurst ? "burst" : "tail";
        std::printf ("twin\tt_ms\t%.3f\tseg\t%s\tin_db\t%.3f\tout_db\t%.3f\tout_peak_db\t%.3f\n", 1000.0 * (double) tIn / sr, seg, inDb, outDb, 20.0 * std::log10 (outPeak + 1e-30));
        inSs = outSs = 0.0; outPeak = 0.0; inWin = 0; ++windows;
    };
    std::vector<float> ring ((size_t) latency + (size_t) rs.block + 1, 0.0f); size_t ringPos = 0;
    auto delayed = [&] (size_t writePos) { return ring[(writePos + ring.size() - (size_t) latency) % ring.size()]; };
    // the render runs `latency` samples past the end so the last output answering to the input is seen
    const long long renderTotal = total + latency;
    while (t < renderTotal)
    {
        r.io.clear();
        const int n = (int) juce::jmin ((long long) rs.block, renderTotal - t);
        for (int i = 0; i < n; ++i)
        {
            const long long tt = t + i;
            const float v = tt < nBurst ? (float) (amp * std::sin (phase)) : 0.0f;
            phase += step; if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            for (int ch = 0; ch < r.fedIn; ++ch) r.io.setSample (ch, i, v);
        }
        r.midi.clear();
        p.processBlock (r.io, r.midi); head.samples += rs.block;
        for (int i = 0; i < n; ++i)
        {
            const long long tt = t + i;
            const float g = tt < nBurst ? (float) (amp * std::sin (phase - step * (double) (n - i))) : 0.0f;
            ring[ringPos % ring.size()] = g; const float gIn = delayed (ringPos); ++ringPos;
            if (tt < latency) continue;   // output before the first aligned input sample: not a window
            inSs += (double) gIn * gIn;
            for (int ch = 0; ch < r.mainOut; ++ch) { const float d = r.io.getSample (ch, i); if (! std::isfinite (d)) { ++nonFinite; continue; } outSs += (double) d * d; outPeak = juce::jmax (outPeak, (double) std::abs (d)); }
            ++inWin;
            if (inWin >= winN) flush (t + i + 1);
        }
        t += n;
    }
    if (inWin > 0) flush (renderTotal);
    std::printf ("tdone\twindows\t%lld\tnonfinite\t%lld\n", windows, nonFinite);
    if (s.tempo > 0.0) p.setPlayHead (nullptr);
    stage ("done");
}

} // namespace ejprobe
