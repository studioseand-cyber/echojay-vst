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
#include "EedLimiterEditor.h"
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
    std::printf ("== ZERO DIFFERENCE (8 Oct 2026, session G's test): the real processor at the schema defaults == limv2::transparent() ==\n");
    {
        const double sr = 48000.0; juce::Random rng (23); const int total = 48000 * 2;
        auto signal = [&] (int n) { const bool burst = (n / 4800) % 3 == 0; return burst ? (rng.nextFloat() * 2.0f - 1.0f) * 1.2f : 0.3f * std::sin (0.009f * (float) n) + 0.05f * std::sin (0.7f * (float) n); };
        EedLimiterProcessor proc; proc.setPlayConfigDetails (2, 2, sr, 512); proc.prepareToPlay (sr, 512);
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("input_db", 9.0); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); int a = 0, k = 0; proc.applyStructured (juce::var (w), EedDeviceProcessor::ParamSource::Assistant, &a, &k); }
        proc.prepareToPlay (sr, 512);   // the parameters landed before prepare, as a restored chain's do (the ceiling ramp is not in play)
        echojay::limv2::Core core; auto t = echojay::limv2::transparent(); t.maxLookaheadMs = t.lookaheadMs * EedLimiterProcessor::kMaxLookaheadMs / 0.18; core.prepare (sr, t); core.setFixedLatency (true); core.setInputGainDb (9.0); core.setCeilingDb (0.0); core.setTruePeak (true); core.reset();
        check (proc.getLatencySamples() == core.latencySamples(), "the processor reports the core's fixed latency", juce::String (proc.getLatencySamples()) + " vs " + juce::String (core.latencySamples()));
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; std::vector<float> cl (512), cr (512); double worst = 0; long long differing = 0;
        juce::Random rngA (23), rngB (23);
        for (int pos = 0; pos < total; pos += 512)
        {
            for (int i = 0; i < 512; ++i) { rng = rngA; const float v = signal (pos + i); rngA = rng; b.setSample (0, i, v); b.setSample (1, i, v); }
            for (int i = 0; i < 512; ++i) { rng = rngB; const float v = signal (pos + i); rngB = rng; cl[(size_t) i] = v; cr[(size_t) i] = v; }
            proc.processBlock (b, m); float* p[2] = { cl.data(), cr.data() }; core.process (p, 2, 512);
            for (int i = 0; i < 512; ++i) { const double d = std::abs ((double) b.getSample (0, i) - (double) cl[(size_t) i]); worst = std::max (worst, d); if (d != 0.0) ++differing; }
        }
        check (worst == 0.0 && differing == 0, "EedLimiterProcessor at its defaults (ceiling 0.0, TRUE PK on, lookahead 0.18, attack 275, release 400, link 75/100) renders SAMPLE-IDENTICAL to limv2::transparent() over 2 s", "worst " + juce::String (worst, 9) + ", differing samples " + juce::String (differing));
    }
    std::printf ("== state migration (8 Oct 2026): a pre-v2 state at the OLD defaults loads as the NEW defaults; anything else literally ==\n");
    {
        const double sr = 48000.0;
        auto stateOf = [] (const juce::String& paramsJson) { const juce::String json = "{\"bypassed\":false,\"params\":" + paramsJson + "}"; return json; };
        auto render = [&] (EedLimiterProcessor& lim) { lim.setPlayConfigDetails (2, 2, sr, 512); lim.prepareToPlay (sr, 512); juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; juce::Random rng (11); std::vector<float> out; const int total = 48000 * 2;
            for (int pos = 0; pos < total; pos += 512) { for (int i = 0; i < 512; ++i) { const int n = pos + i; const bool burst = (n / 4800) % 2 == 0; const float v = burst ? (rng.nextFloat() * 2.0f - 1.0f) * 1.5f : 0.2f * std::sin (0.013f * (float) n); b.setSample (0, i, v); b.setSample (1, i, v); } lim.processBlock (b, m); for (int i = 0; i < 512; ++i) out.push_back (b.getSample (0, i)); } return out; };
        // (a) old-format state (no attack_ms) at the OLD defaults == a fresh instance at the NEW defaults, sample for sample
        EedLimiterProcessor fresh; auto* fp = new juce::DynamicObject(); fp->setProperty ("ceiling_db", 0.0); fp->setProperty ("true_peak", 1); fp->setProperty ("input_db", 8.0); auto* fw = new juce::DynamicObject(); fw->setProperty ("params", juce::var (fp)); int a1 = 0, s1 = 0; fresh.applyStructured (juce::var (fw), EedDeviceProcessor::ParamSource::Assistant, &a1, &s1);
        EedLimiterProcessor old; const juce::String oldState = stateOf ("{\"ceiling_db\":0.0,\"input_db\":8.0,\"release_ms\":50.0,\"lookahead_ms\":2.0,\"mode\":0,\"true_peak\":1,\"sc_hpf_hz\":0.0}");
        old.setStateInformation (oldState.toRawUTF8(), (int) oldState.getNumBytesAsUTF8());
        check (std::abs (old.getParamValue ("lookahead_ms") - 0.18) < 1e-9 && std::abs (old.getParamValue ("release_ms") - 400.0) < 1e-9 && std::abs (old.getParamValue ("attack_ms") - 275.0) < 1e-9 && std::abs (old.getParamValue ("link_pct") - 75.0) < 1e-9, "a pre-v2 state at the old defaults (lookahead 2.0, release 50, no attack_ms) loads lookahead 0.18, release 400, attack 275, link 75", "la " + juce::String (old.getParamValue ("lookahead_ms")) + " rel " + juce::String (old.getParamValue ("release_ms")));
        const auto A = render (fresh), B = render (old); double worst = 0; for (size_t i = 0; i < A.size(); ++i) worst = std::max (worst, (double) std::abs (A[i] - B[i]));
        check (worst == 0.0, "...and renders SAMPLE-IDENTICAL to a fresh instance at the new defaults (2 s of bursts)", "worst diff " + juce::String (worst, 9));
        // (b) old-format state with NON-default values keeps them literally
        EedLimiterProcessor keep; const juce::String keepState = stateOf ("{\"ceiling_db\":-1.0,\"input_db\":3.0,\"release_ms\":200.0,\"lookahead_ms\":1.0,\"mode\":0,\"true_peak\":0,\"sc_hpf_hz\":40.0}");
        keep.setStateInformation (keepState.toRawUTF8(), (int) keepState.getNumBytesAsUTF8());
        check (std::abs (keep.getParamValue ("lookahead_ms") - 1.0) < 1e-9 && std::abs (keep.getParamValue ("release_ms") - 200.0) < 1e-9 && std::abs (keep.getParamValue ("ceiling_db") + 1.0) < 1e-9 && keep.getParamValue ("true_peak") < 0.5 && std::abs (keep.getParamValue ("attack_ms") - 275.0) < 1e-9, "a pre-v2 state with its own values (lookahead 1.0, release 200, ceiling -1, TP off) keeps every one; attack_ms at its default", "la " + juce::String (keep.getParamValue ("lookahead_ms")) + " rel " + juce::String (keep.getParamValue ("release_ms")) + " ceil " + juce::String (keep.getParamValue ("ceiling_db")));
        // (c) a v2 state (attack_ms present) that really says lookahead 2.0 / release 50 is NOT migrated
        EedLimiterProcessor v2s; const juce::String v2State = stateOf ("{\"ceiling_db\":0.0,\"input_db\":0.0,\"release_ms\":50.0,\"lookahead_ms\":2.0,\"attack_ms\":100.0,\"mode\":0,\"true_peak\":1}");
        v2s.setStateInformation (v2State.toRawUTF8(), (int) v2State.getNumBytesAsUTF8());
        check (std::abs (v2s.getParamValue ("lookahead_ms") - 2.0) < 1e-9 && std::abs (v2s.getParamValue ("release_ms") - 50.0) < 1e-9 && std::abs (v2s.getParamValue ("attack_ms") - 100.0) < 1e-9, "a v2 state (attack_ms present) saying lookahead 2.0 / release 50 loads literally - the user set them", "la " + juce::String (v2s.getParamValue ("lookahead_ms")) + " rel " + juce::String (v2s.getParamValue ("release_ms")));
    }
    std::printf ("== the v2 PANEL (9 Oct 2026): the dials show the processor (the migration included) and drive its parameters ==\n");
    {
        auto stateOf2 = [] (const char* params) { return juce::String ("{\"params\":") + params + "}"; };
        // (d) an old chain through the migration: what the DIALS show
        EedLimiterProcessor old; const juce::String oldState = stateOf2 ("{\"ceiling_db\":0.0,\"input_db\":8.0,\"release_ms\":50.0,\"lookahead_ms\":2.0,\"mode\":0,\"true_peak\":1,\"sc_hpf_hz\":0.0}");
        old.setStateInformation (oldState.toRawUTF8(), (int) oldState.getNumBytesAsUTF8());
        std::unique_ptr<juce::AudioProcessorEditor> edBase (old.createEditor()); auto* ed = dynamic_cast<EedLimiterEditor*> (edBase.get());
        check (ed != nullptr, "the limiter's editor is the v2 panel's editor (EedLimiterEditor on DeviceEditorBase)", edBase != nullptr ? edBase->getName() : "null");
        if (ed != nullptr)
        {
            const auto m = ed->currentModel();
            check (std::abs (m.lookaheadMs - 0.18) < 1e-9 && std::abs (m.releaseMs - 400.0) < 1e-9 && std::abs (m.attackMs - 275.0) < 1e-9 && std::abs (m.linkPct - 75.0) < 1e-9 && std::abs (m.releaseLinkPct - 100.0) < 1e-9 && std::abs (m.gainDb - 8.0) < 1e-9 && m.truePeak,
                   "an old-format chain at the old defaults opens with the dials at the MIGRATED values (0.18 / 400 / 275 / 75 / 100), gain 8 kept, TRUE PK on",
                   "la " + juce::String (m.lookaheadMs) + " rel " + juce::String (m.releaseMs) + " atk " + juce::String (m.attackMs) + " link " + juce::String (m.linkPct) + "/" + juce::String (m.releaseLinkPct) + " gain " + juce::String (m.gainDb));
            // (e) every dial drives its parameter through the same funnel as the assistant's moves
            struct Move { const char* id; double v; }; const Move moves[] = { { "input_db", 6.0 }, { "ceiling_db", -1.0 }, { "lookahead_ms", 1.0 }, { "attack_ms", 500.0 }, { "release_ms", 200.0 }, { "link_pct", 50.0 }, { "release_link_pct", 80.0 }, { "sc_hpf_hz", 100.0 } };
            juce::String bad;
            for (const auto& mv : moves) { if (! ed->setDialForTest (mv.id, mv.v)) bad += juce::String (mv.id) + " (no dial) "; else if (std::abs (old.getParamValue (mv.id) - mv.v) > 1e-6) bad += juce::String (mv.id) + " = " + juce::String (old.getParamValue (mv.id)) + " not " + juce::String (mv.v) + "; "; }
            check (bad.isEmpty(), "moving each of the 8 dials (incl. attack_ms, link_pct, release_link_pct) sets that parameter on the processor to the dialled value", bad.isEmpty() ? "all 8 agree" : bad);
            // (f) a move from outside (the assistant, a restored chain) shows on the dials after the sync the timer runs
            old.setParamValue ("attack_ms", 900.0); old.setParamValue ("release_link_pct", 30.0); ed->syncFromProcessor();
            const auto m2 = ed->currentModel();
            check (std::abs (m2.attackMs - 900.0) < 1e-9 && std::abs (m2.releaseLinkPct - 30.0) < 1e-9, "a parameter set from outside the panel shows on its dial after the editor's sync", "atk " + juce::String (m2.attackMs) + " rls link " + juce::String (m2.releaseLinkPct));
            // (g) the panel's picture is fed: after a prepared processor runs audio, the tap has columns and hops
            old.setPlayConfigDetails (2, 2, 48000.0, 512); old.prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer mb; for (int blk = 0; blk < 200; ++blk) { for (int i = 0; i < 512; ++i) { const float v = 0.9f * std::sin (0.05f * (float) (blk * 512 + i)); b.setSample (0, i, v); b.setSample (1, i, v); } old.processBlock (b, mb); }
            check (old.meterTap().columnHead() > 0 && old.meterTap().hopHead() >= 20, "the processor feeds the panel's meter tap (columns and 100 ms hops arrive while audio runs)", "columns " + juce::String (old.meterTap().columnHead()) + " hops " + juce::String (old.meterTap().hopHead()));
        }
    }
    std::printf ("\n==== limiter_wall_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
