// limiter_wall_guard (18 Sep 2026; rewritten 8 Oct 2026 for the v2 contract): EedLimiterProcessor holds its ceiling and
// reports the delay it actually applies.
//   TRUE PEAK is judged by THE ARBITER - exact band-limited reconstruction (tools/limiter_ab_guard/ejdsp.h,
//   truePeakExact: FFT zero-padding 16x + parabolic peak), which shares no code with the limiter's detector. The
//   gate of 8 Oct 2026 found +0.83..+1.11 dBTP overs that the previous 4x/24-tap meter and the limiter's own 96-tap
//   detector both under-read; a meter of the same family as the thing it judges proves nothing. The 96-tap reading
//   is printed alongside, never as the verdict. Over = more than +0.02 dB above the ceiling.
//   LATENCY: the reported number must equal the MEASURED delay of an impulse through the processor, and must be the
//   same for every setting (2 ms / 10 ms lookahead, true peak on / off) at a given sample rate; checked at 44.1, 48
//   and 96 kHz. Transparency: a sub-ceiling signal comes out identical once the delay is compensated.
// Bursts of 3 ms, 50 ms and 1 s at +6 dBFS, lookahead 2 ms and 10 ms, transparent mode, true_peak on, ceiling -0.1.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EedLimiterProcessor.h"
#include "../limiter_ab_guard/ejdsp.h"
#include <cstdio>
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void setParams (EedLimiterProcessor& lim, double ceiling, bool tp, double la)
{
    auto* pp = new juce::DynamicObject(); pp->setProperty ("ceiling_db", ceiling); pp->setProperty ("true_peak", tp ? 1 : 0); pp->setProperty ("lookahead_ms", la);
    auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); int a = 0, s = 0; lim.applyStructured (juce::var (w), EedDeviceProcessor::ParamSource::Assistant, &a, &s);
}
// the measured delay: a 0.25 impulse through a prepared processor, found by its peak
int measureDelay (double sr, bool tp, double la)
{
    EedLimiterProcessor lim; lim.setPlayConfigDetails (2, 2, sr, 512); lim.prepareToPlay (sr, 512); setParams (lim, 0.0, tp, la);
    juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; int best = -1; float bestV = 0; const int total = 8192;
    for (int pos = 0; pos < total; pos += 512) { b.clear(); if (pos == 0) { b.setSample (0, 0, 0.25f); b.setSample (1, 0, 0.25f); } lim.processBlock (b, m); for (int i = 0; i < 512; ++i) { const float v = std::abs (b.getSample (0, i)); if (v > bestV) { bestV = v; best = pos + i; } } }
    return best;
}
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("limiter_wall_guard: EchoJay Limiter holds -0.1 dB on bursts (ARBITER), and reports the delay it applies\n");
    for (double la : { 2.0, 10.0 })
    for (int burstSamples : { 144, 2400, 48000 })
    {
        const double sr = 48000.0;
        EedLimiterProcessor lim; lim.setPlayConfigDetails (2, 2, sr, 512); lim.prepareToPlay (sr, 512); setParams (lim, -0.1, true, la);
        const int lat = lim.getLatencySamples();
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; float pk = 0.0f, grMax = 0.0f; juce::Random rng (7);
        const int total = 48000 * 2 + lat; int pos = 0; std::vector<double> outL; outL.reserve ((size_t) total);
        while (pos < total)
        {
            for (int i = 0; i < 512; ++i, ++pos) { const bool inBurst = pos >= 24000 && pos < 24000 + burstSamples; const float v = inBurst ? (rng.nextFloat() * 2.0f - 1.0f) * 2.0f : 0.0f; b.setSample (0, i, v); b.setSample (1, i, v); }
            lim.processBlock (b, m);
            pk = juce::jmax (pk, b.getMagnitude (0, 0, 512), b.getMagnitude (1, 0, 512));
            for (int i = 0; i < 512; ++i) outL.push_back (b.getSample (0, i));
            grMax = juce::jmax (grMax, -lim.gainReductionDb());
        }
        const float pkDb = 20.0f * std::log10 (juce::jmax (pk, 1e-9f));
        const juce::String label = "lookahead " + juce::String (la, 0) + " ms, burst " + juce::String (burstSamples / 48.0, 0) + " ms at +6 dBFS";
        check (pkDb <= -0.1f + 0.01f, label + ": output SAMPLE peak <= -0.1 dBFS", "peak " + juce::String (pkDb, 2) + " dBFS, GR max " + juce::String (grMax, 2) + " dB");
        const auto arb = ejdsp::truePeakExact (outL, std::pow (10.0, -0.1 / 20.0), 0.02); const double arbDb = ejdsp::dB (arb.peakLin);
        const auto m96 = ejdsp::truePeak (outL, std::pow (10.0, -0.1 / 20.0), 0.02);
        check (arb.overs == 0 && arbDb <= -0.1 + 0.02, label + ": output TRUE peak <= -0.1 dBTP + 0.02 by the ARBITER (exact reconstruction), zero overs", juce::String (arbDb, 3) + " dBTP exact, " + juce::String ((int) arb.overs) + " overs; 96-tap meter " + juce::String (ejdsp::dB (m96.peakLin), 3) + " dBTP (alongside, not the verdict)");
        check (grMax >= 5.5f, label + ": the wall reduced about 6 dB", juce::String (grMax, 2) + " dB");
        check (lat == measureDelay (sr, true, la), label + ": reported latency == the MEASURED delay of an impulse", "reported " + juce::String (lat) + ", measured " + juce::String (measureDelay (sr, true, la)));
    }
    std::printf ("== latency is one number per sample rate: every setting, reported == measured ==\n");
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        int reported[4], measured[4]; int k = 0;
        for (bool tp : { true, false }) for (double la : { 2.0, 10.0 }) { EedLimiterProcessor lim; lim.setPlayConfigDetails (2, 2, sr, 512); lim.prepareToPlay (sr, 512); setParams (lim, 0.0, tp, la); reported[k] = lim.getLatencySamples(); measured[k] = measureDelay (sr, tp, la); ++k; }
        bool same = true, eq = true; for (int i = 0; i < 4; ++i) { same = same && reported[i] == reported[0]; eq = eq && reported[i] == measured[i]; }
        check (same && eq && reported[0] > 0, juce::String (sr, 0) + " Hz: TP on/off x lookahead 2/10 ms report ONE latency and it is the measured delay", "reported " + juce::String (reported[0]) + "/" + juce::String (reported[1]) + "/" + juce::String (reported[2]) + "/" + juce::String (reported[3]) + ", measured " + juce::String (measured[0]) + "/" + juce::String (measured[1]) + "/" + juce::String (measured[2]) + "/" + juce::String (measured[3]));
    }
    std::printf ("== transparency with the delay compensated ==\n");
    {
        const double sr = 48000.0; EedLimiterProcessor lim; lim.setPlayConfigDetails (2, 2, sr, 512); lim.prepareToPlay (sr, 512); setParams (lim, 0.0, true, 2.0); const int lat = lim.getLatencySamples();
        auto probe = [] (int n) { return 0.3f * std::sin (0.05f * (float) n) + 0.2f * std::sin (0.31f * (float) n + 1.0f); };   // peaks ~-6 dBFS, never near the ceiling
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; float worst = 0.0f; const int total = lat + 512 * 8;
        for (int pos = 0; pos < total; pos += 512) { for (int i = 0; i < 512; ++i) { b.setSample (0, i, probe (pos + i)); b.setSample (1, i, probe (pos + i)); } lim.processBlock (b, m); for (int i = 0; i < 512; ++i) { const int n = pos + i; if (n >= lat + 512) worst = juce::jmax (worst, std::abs (b.getSample (0, i) - probe (n - lat))); } }
        check (worst < 1.0e-4f, "a sub-ceiling signal comes out identical, delayed by exactly the reported latency", "worst delta " + juce::String (worst, 7) + " at latency " + juce::String (lat));
    }
    std::printf ("\n==== limiter_wall_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
