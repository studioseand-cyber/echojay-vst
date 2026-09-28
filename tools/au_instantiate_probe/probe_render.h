// probe_render.h - EchoJayProbe's RENDER MODE (feat/ejmap-cert, step 3 of the certification plan).
//
//   EchoJayProbe "<name>" "<identifier>" <uidHex> --render-test
//
// Everything that MEASURES audio lives in this file, so the upstream probe source changes by one include, one flag
// and one dispatch branch, and a future subtree pull from merge/kathy-2026-09-06 stays conflict-light.
//
// WHY RENDERING IS BACK, AND WHY THIS IS NOT THE CODE 3c0dbf2 REMOVED. On 18 Sep the probe's render step was taken
// out after AMEK Mastering Compressor segfaulted in renderGetInput inside the host's input callback (exit 139, both
// architectures). That code rendered a FIXED 2-channel buffer with no enableAllBuses and no setPlayConfigDetails, so
// a plugin declaring more input channels than two (a sidechain, say) would read channels the buffer did not have. That is a
// plausible cause and was never measured. This mode does what EJ Map's PluginHost does - enableAllBuses, then
// setPlayConfigDetails with the plugin's own channel totals, then prepareToPlay - and sizes the buffer from those
// totals. Retesting AMEK through it is the first job of this mode.
//
// IT MEASURES; IT DOES NOT DERIVE (decision D2, 28 Sep). It prints raw RMS and peak in plain dBFS
// (20*log10 of the linear value, no sine-peak correction), per output channel, plus a hash of every output sample.
// Level conventions, verdicts and anything else that only needs re-computing belong to EJ Map, so changing them never
// costs a rebuild and a re-sign of this binary.
//
// SIDECHAIN POLICY: ENABLED, FED SILENCE, for every certification render (ruled 28 Sep 2026). Every bus the
// plugin declares stays enabled at its declared layout, and every input channel outside the main bus is zero.
//   - Disabling a declared bus measures a configuration the plugin never ships in. A DAW instantiates the sidechain
//     bus whether or not anything is routed to it.
//   - It is the conservative choice: the harness stops altering the declared layout.
//   - It is the AMEK hypothesis. The first version of this mode called setPlayConfigDetails, and JUCE's
//     setPlayConfigDetails ends in an UNCONDITIONAL disableNonMainBuses() ("if the user is using this method then
//     they do not want any side-buses", juce_AudioProcessor.cpp:366). So enableAllBuses turned AMEK Mastering
//     Compressor's stereo sidechain on and the next line turned it off (it read disabled, 0 channels), and AMEK then
//     crashed in its own render. API-2500's mono sidechain survived only because that disable FAILED - a jassert,
//     so nothing in a release build. EJ Map's PluginHost uses the same sequence, so its M9 renders also ran with
//     sidechains off wherever the plugin allowed it.
// So configure = enableAllBuses + setRateAndBufferSizeDetails (rate and block only, no bus changes) + prepareToPlay.
// THE RISK, recorded rather than hidden: a compressor that defaults to EXTERNAL sidechain keying keys off silence and
// never compresses, so a sweep reads flat at every position and level. That lands in the existing `flat` result, not
// a new one, but it must be readable as such. The "sidechain" lines below state the state as measured, and the fixture
// must carry it, so a flat result is not blamed on the threshold.
//
// The stimulus is EJ Map's renderSine, unchanged: 997 Hz at -12 dBFS, 0.5 s of silence first, then 2 s of tone, with
// the first 0.25 s of tone discarded from the level readings. It drives the MAIN input bus only; every other input
// channel (a sidechain) is held at zero, and the output says which channels were driven.
//
// The output is line-oriented and tab-separated, like the other modes. A "stage" line is flushed before each step
// that runs plugin code, so a crash names the step it died in. A bound on the whole render is the CALLER's job: the
// driver owns every timeout (docs/EJMAP_CERT_DRIVER.md).
#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ejprobe
{

struct RenderSpec
{
    double sampleRate = 48000.0;
    int    block      = 512;
    double hz         = 997.0;
    double dbfs       = -12.0;
    double prerollS   = 0.5;
    double toneS      = 2.0;
    double discardS   = 0.25;
};

inline juce::String clean (const juce::String& t) { return t.replace ("\t", " ").replace ("\n", " ").replace ("\r", " "); }

inline void stage (const char* s) { std::printf ("stage\t%s\n", s); std::fflush (stdout); }

inline double toDb (double linear) { return linear > 0.0 ? 20.0 * std::log10 (linear) : -999.0; }

// Every bus as the plugin declares it, then whether it would accept the three layouts certification cares about.
// The layout questions change nothing: checkBusesLayoutSupported only asks.
inline void printBuses (juce::AudioPluginInstance& p, const char* when)
{
    for (const bool isInput : { true, false })
        for (int b = 0; b < p.getBusCount (isInput); ++b)
            if (auto* bus = p.getBus (isInput, b))
                std::printf ("bus\t%s\t%s\t%d\t%s\t%d\t%s\t%s\n", when, isInput ? "in" : "out", b,
                             clean (bus->getName()).toRawUTF8(), bus->getNumberOfChannels(),
                             clean (bus->getCurrentLayout().getDescription()).toRawUTF8(),
                             bus->isEnabled() ? "enabled" : "disabled");
}

inline void printLayoutSupport (juce::AudioPluginInstance& p)
{
    struct Ask { const char* name; juce::AudioChannelSet in, out; };
    const Ask asks[] = { { "mono",           juce::AudioChannelSet::mono(),   juce::AudioChannelSet::mono() },
                         { "stereo",         juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo() },
                         { "mono_to_stereo", juce::AudioChannelSet::mono(),   juce::AudioChannelSet::stereo() } };
    for (const auto& a : asks)
    {
        auto layout = p.getBusesLayout();
        if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
        { std::printf ("layout\t%s\tunaskable (no main %s bus)\n", a.name, layout.inputBuses.isEmpty() ? "input" : "output"); continue; }
        layout.inputBuses.getReference (0)  = a.in;
        layout.outputBuses.getReference (0) = a.out;
        std::printf ("layout\t%s\t%s\n", a.name, p.checkBusesLayoutSupported (layout) ? "supported" : "not_supported");
    }
}

// CONFIGURE AND PREPARE, the one place the SIDECHAIN POLICY is applied, so every mode that renders gets it.
inline void configureAndPrepare (juce::AudioPluginInstance& p, const RenderSpec& s)
{
    stage ("configure");
    // NOT setPlayConfigDetails: it ends in an unconditional disableNonMainBuses(). See SIDECHAIN POLICY above.
    const bool allEnabled = p.enableAllBuses();
    p.setRateAndBufferSizeDetails (s.sampleRate, s.block);
    std::printf ("policy\tsidechain\tenabled_silent\tenable_all_buses\t%s\n", allEnabled ? "ok" : "refused");
    printBuses (p, "render");
    for (int b = 1; b < p.getBusCount (true); ++b)
        if (auto* bus = p.getBus (true, b))
            std::printf ("sidechain\t%d\t%s\t%d\t%s\t%s\n", b, clean (bus->getName()).toRawUTF8(),
                         bus->getNumberOfChannels(), bus->isEnabled() ? "enabled" : "disabled",
                         bus->isEnabled() && bus->getNumberOfChannels() > 0 ? "silent" : "absent");

    stage ("prepare");
    p.prepareToPlay (s.sampleRate, s.block);
}

// The number of main-bus input channels the stimulus drives (0 when there is no enabled main input).
inline int mainInputChannels (juce::AudioPluginInstance& p)
{
    return p.getBusCount (true) > 0 && p.getBus (true, 0) != nullptr && p.getBus (true, 0)->isEnabled()
             ? p.getBus (true, 0)->getNumberOfChannels() : 0;
}

// The render. Returns nothing: every result is printed, and a crash is attributed by the last flushed stage line.
inline void runRenderTest (juce::AudioPluginInstance& p, const RenderSpec& s = {})
{
    std::printf ("render\tproto\t1\n");
    printBuses (p, "declared");
    printLayoutSupport (p);
    configureAndPrepare (p, s);

    const int totalIn  = p.getTotalNumInputChannels();
    const int totalOut = p.getTotalNumOutputChannels();
    const int chans    = juce::jmax (2, totalIn, totalOut);
    const int mainIn   = mainInputChannels (p);
    std::printf ("config\tin\t%d\tout\t%d\tbuffer\t%d\tmain_in\t%d\tsr\t%.0f\tblock\t%d\tlatency\t%d\n",
                 totalIn, totalOut, chans, mainIn, s.sampleRate, s.block, p.getLatencySamples());
    juce::String driven;
    for (int ch = 0; ch < mainIn; ++ch) driven << (ch ? "," : "") << ch;
    std::printf ("stimulus\thz\t%.1f\tdbfs\t%.1f\tpreroll_s\t%.2f\ttone_s\t%.2f\tdiscard_s\t%.2f\tdriven\t%s\n",
                 s.hz, s.dbfs, s.prerollS, s.toneS, s.discardS, driven.isEmpty() ? "none" : driven.toRawUTF8());

    juce::AudioBuffer<float> io (chans, s.block);
    juce::MidiBuffer midi;
    const double amp  = std::pow (10.0, s.dbfs / 20.0);
    const double step = juce::MathConstants<double>::twoPi * s.hz / s.sampleRate;
    double phase = 0.0;
    const int prerollBlocks = (int) (s.prerollS * s.sampleRate) / s.block;
    const int toneBlocks    = (int) (s.toneS    * s.sampleRate) / s.block;
    const int discardBlocks = (int) (s.discardS * s.sampleRate) / s.block;

    std::vector<double> sumSq ((size_t) totalOut, 0.0), peak ((size_t) totalOut, 0.0);
    std::uint64_t hash = 1469598103934665603ULL;             // FNV-1a 64 over the raw bits of every output sample
    long long hashed = 0, measured = 0, nonFinite = 0;

    stage ("render");
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    for (int k = 0; k < prerollBlocks + toneBlocks; ++k)
    {
        const bool tone = k >= prerollBlocks;
        io.clear();
        if (tone)
            for (int n = 0; n < s.block; ++n)
            {
                const float v = (float) (amp * std::sin (phase));
                for (int ch = 0; ch < mainIn; ++ch) io.setSample (ch, n, v);
                phase += step;
                if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            }
        midi.clear();
        p.processBlock (io, midi);

        const bool counted = tone && (k - prerollBlocks) >= discardBlocks;
        for (int ch = 0; ch < totalOut; ++ch)
        {
            const float* d = io.getReadPointer (ch);
            for (int n = 0; n < s.block; ++n)
            {
                std::uint32_t bits; std::memcpy (&bits, &d[n], sizeof bits);
                for (int b = 0; b < 4; ++b) { hash ^= (bits >> (8 * b)) & 0xffu; hash *= 1099511628211ULL; }
                ++hashed;
                if (! std::isfinite (d[n])) { ++nonFinite; continue; }
                if (counted)
                {
                    sumSq[(size_t) ch] += (double) d[n] * d[n];
                    peak[(size_t) ch]  = juce::jmax (peak[(size_t) ch], (double) std::abs (d[n]));
                }
            }
        }
        if (counted) measured += s.block;
    }
    const double wallMs = juce::Time::getMillisecondCounterHiRes() - t0;

    for (int ch = 0; ch < totalOut; ++ch)
        std::printf ("out\t%d\trms_dbfs\t%.4f\tpeak_dbfs\t%.4f\n", ch,
                     toDb (measured > 0 ? std::sqrt (sumSq[(size_t) ch] / (double) measured) : 0.0),
                     toDb (peak[(size_t) ch]));
    std::printf ("hash\tfnv1a64\t%016llx\tsamples\t%lld\n", (unsigned long long) hash, hashed);
    std::printf ("nonfinite\t%lld\n", nonFinite);
    std::printf ("timing\twall_ms\t%.1f\taudio_s\t%.3f\n", wallMs, (prerollBlocks + toneBlocks) * s.block / s.sampleRate);
    stage ("done");
}

} // namespace ejprobe
