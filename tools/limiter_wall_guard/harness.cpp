// limiter_wall_guard (18 Sep 2026): EedLimiterProcessor holds its ceiling. Bursts of 3 ms, 50 ms and 1 s at +6 dBFS,
// lookahead 2 ms and 10 ms, transparent mode, true_peak on, ceiling -0.1: the output SAMPLE peak never exceeds the
// ceiling and the output TRUE PEAK (an independent 4x meter here) stays within 0.1 dB of it.
// RED on the lib before the wall: +2.17 dBFS out for a +6 dBFS 1 s burst at 2 ms lookahead; +4.55 dBFS on 3 ms at 10 ms.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EedLimiterProcessor.h"
#include <cstdio>
namespace { int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
struct TP4 { float coef[4][24]; std::vector<float> h; int pos = 0; float mx = 0;
    TP4() { h.assign (24, 0.0f); for (int ph = 0; ph < 4; ++ph) { double sum = 0; for (int k = 0; k < 24; ++k) { const double x = (k - 11.5) - ph / 4.0 + 0.5; const double sinc = x == 0 ? 1 : std::sin (M_PI * x) / (M_PI * x); const double w = 0.42 - 0.5 * std::cos (2 * M_PI * (k + 0.5) / 24) + 0.08 * std::cos (4 * M_PI * (k + 0.5) / 24); coef[ph][k] = (float) (sinc * w); sum += sinc * w; } for (int k = 0; k < 24; ++k) coef[ph][k] = (float) (coef[ph][k] / sum); } }
    void push (float x) { h[(size_t) pos] = x; for (int ph = 0; ph < 4; ++ph) { float a = 0; int idx = pos; for (int k = 0; k < 24; ++k) { a += coef[ph][k] * h[(size_t) idx]; idx = idx == 0 ? 23 : idx - 1; } mx = std::max (mx, std::abs (a)); } pos = (pos + 1) % 24; }
    float db() const { return mx > 0 ? 20 * std::log10 (mx) : -200; } }; }
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("limiter_wall_guard: EchoJay Limiter holds -0.1 dB on bursts\n");
    for (double la : { 2.0, 10.0 })
    for (int burstSamples : { 144, 2400, 48000 })
    {
        EedLimiterProcessor lim; lim.prepareToPlay (48000.0, 512);
        auto* pp = new juce::DynamicObject(); pp->setProperty ("ceiling_db", -0.1); pp->setProperty ("true_peak", 1); pp->setProperty ("lookahead_ms", la);
        auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); int a = 0, s = 0; lim.applyStructured (juce::var (w), EedDeviceProcessor::ParamSource::Assistant, &a, &s);
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; float pk = 0.0f, grMax = 0.0f; juce::Random rng (7); TP4 tp;
        const int total = 48000 * 2; int pos = 0;
        while (pos < total)
        {
            for (int i = 0; i < 512; ++i, ++pos) { const bool inBurst = pos >= 24000 && pos < 24000 + burstSamples; const float v = inBurst ? (rng.nextFloat() * 2.0f - 1.0f) * 2.0f : 0.0f; b.setSample (0, i, v); b.setSample (1, i, v); }
            lim.processBlock (b, m);
            pk = juce::jmax (pk, b.getMagnitude (0, 0, 512), b.getMagnitude (1, 0, 512));
            for (int i = 0; i < 512; ++i) tp.push (b.getSample (0, i));
            grMax = juce::jmax (grMax, -lim.gainReductionDb());
        }
        const float pkDb = 20.0f * std::log10 (juce::jmax (pk, 1e-9f));
        const juce::String label = "lookahead " + juce::String (la, 0) + " ms, burst " + juce::String (burstSamples / 48.0, 0) + " ms at +6 dBFS";
        check (pkDb <= -0.1f + 0.01f, label + ": output SAMPLE peak <= -0.1 dBFS", "peak " + juce::String (pkDb, 2) + " dBFS, GR max " + juce::String (grMax, 2) + " dB");
        check (tp.db() >= -0.25f && tp.db() <= -0.10f, label + ": output TRUE peak within [-0.25, -0.10] dBTP by the independent 4x meter, never above -0.10 (18e item 12: margin 0.2 -> 0.1 dB)", juce::String (tp.db(), 2) + " dBTP");
        check (grMax >= 5.5f, label + ": the wall reduced about 6 dB", juce::String (grMax, 2) + " dB");
        {   // CHECK 1 (latency): the reported latency == the lookahead in samples + the true-peak interpolator's group delay (16 at any rate)
            const int want = (int) std::lround (la * 0.001 * 48000.0) + echojay::TruePeakInterp::kDelay;
            check (lim.getLatencySamples() == want, label + ": reported latency == lookahead samples + interpolator delay (" + juce::String (want) + ")", "reported " + juce::String (lim.getLatencySamples()));
        }
    }
    std::printf ("\n==== limiter_wall_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
