/*
    pitch_probe  —  the OFFLINE validation harness for EedPitchEngine
    (PITCH_CORRECTION_SPEC.md P0).

    WHY THIS EXISTS. The g++ suite in test/pitch_engine_test.cpp proves the
    estimator on synthetic material: sawtooth, vibrato, saw+noise. That
    material may FLATTER the detector, and in a way that is invisible until
    PSOLA is sitting on top of it. Real singing has consonants, breath, creak,
    glottal fry, scoops into notes, and a room's noise floor underneath all of
    it — none of which a generator produces. This runs the SAME engine, with
    the SAME hop-by-hop publishing the audio thread uses, over a real file, and
    prints the one number that decides whether P0 is solid:

        octave-guard fires as a percentage of voiced hops.

    Spec §2.1: "Log how often the guard fires - if it fires constantly the
    window is wrong for the material, not the guard." On ordinary singing at
    the right voice_type that should be a couple of percent at most. Much above
    that and the window sizing is wrong for real material, and that has to be
    found HERE, not once a corrector is stacked on top and every artefact looks
    like a PSOLA bug.

    It is deliberately a separate CMake target (EXCLUDE_FROM_ALL) and NOT a
    dependency of any plugin target: a validation tool must never be able to
    break, slow, or link itself into a shipping build.

    Usage:
        EchoJayPitchProbe <audio-file> [options]
          --voice-type <soprano|alto_tenor|low_male|instrument|bass>
          --all               run all five voice types over the same file
          --summary-only      suppress the per-hop table
          --csv               per-hop table as CSV (implies machine parsing)

    Any sample rate is accepted and handled NATIVELY - the engine derives its
    decimation and window from the rate it is prepared at, so nothing is
    resampled and the probe measures the engine as the plugin runs it.
*/

#include <JuceHeader.h>

#include "EedPitchEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using echojay::PitchEngine;
using echojay::PitchReading;

namespace
{

// ---------------------------------------------------------------------------
// One published hop.
// ---------------------------------------------------------------------------
struct Hop
{
    double timeS      = 0.0;
    bool   voiced     = false;
    float  f0Hz       = 0.0f;
    float  cents      = 0.0f;   // deviation from the nearest equal-tempered note
    float  confidence = 0.0f;
    float  rmsDb      = -120.0f;
    bool   guardFired = false;
    char   note[8]    = { '-', 0 };
};

struct Summary
{
    int    totalHops = 0, voicedHops = 0, guardFires = 0;
    double medianConfidence = 0.0;
    double largestJumpCents = 0.0;   // between ADJACENT voiced hops
    double largestJumpAtS   = 0.0;
    int    nearOctaveJumps  = 0;     // adjacent-voiced jumps over 600 cents
};

// ---------------------------------------------------------------------------
// Drive the engine one ANALYSIS HOP at a time.
//
// The engine publishes on its own internal cadence, so the driver does not
// assume one hop per call: it watches totalHops advance and only records a row
// when it actually did. That keeps the probe correct through the warm-up
// frames (which publish nothing) without special-casing them.
// ---------------------------------------------------------------------------
std::vector<Hop> runFile (const juce::AudioBuffer<float>& audio, double fs,
                          int voiceType, int tracking, Summary& sumOut)
{
    PitchEngine engine;
    engine.prepare (fs, 4096);
    engine.setVoiceType (voiceType);
    engine.setTracking (tracking);

    const int hopSamples = std::max (1, engine.inputHopLength (voiceType));
    const int numCh      = audio.getNumChannels();
    const int numSamples = audio.getNumSamples();

    const float* l = audio.getReadPointer (0);
    const float* r = numCh > 1 ? audio.getReadPointer (1) : nullptr;

    std::vector<Hop> hops;
    hops.reserve ((size_t) (numSamples / hopSamples + 4));

    uint32_t prevTotal = 0, prevFires = 0;

    for (int pos = 0; pos + hopSamples <= numSamples; pos += hopSamples)
    {
        engine.process (l + pos, r != nullptr ? r + pos : nullptr, hopSamples);

        const PitchReading rd = engine.getReading();
        if (rd.totalHops == prevTotal) continue;         // warm-up: nothing published

        Hop h;
        h.timeS      = (double) (pos + hopSamples) / fs;
        h.voiced     = rd.voiced;
        h.f0Hz       = rd.f0Hz;
        h.confidence = rd.confidence;
        h.rmsDb      = rd.rmsDb;
        h.guardFired = rd.guardFires > prevFires;
        if (rd.voiced)
            PitchEngine::noteName (rd.f0Hz, h.note, sizeof (h.note), &h.cents);
        else
            std::snprintf (h.note, sizeof (h.note), "-");

        hops.push_back (h);
        prevTotal = rd.totalHops;
        prevFires = rd.guardFires;
    }

    // ---- summary ----------------------------------------------------------
    const PitchReading fin = engine.getReading();
    sumOut.totalHops  = (int) fin.totalHops;
    sumOut.voicedHops = (int) fin.voicedHops;
    sumOut.guardFires = (int) fin.guardFires;

    std::vector<float> confs;
    confs.reserve (hops.size());
    for (const auto& h : hops)
        if (h.voiced) confs.push_back (h.confidence);
    if (! confs.empty())
    {
        std::sort (confs.begin(), confs.end());
        sumOut.medianConfidence = confs[confs.size() / 2];
    }

    // Largest SINGLE-FRAME jump: adjacent voiced hops only. A jump measured
    // across an unvoiced gap is a new phrase, not a detector error, and
    // counting it would hide the thing this number exists to expose.
    for (size_t i = 1; i < hops.size(); ++i)
    {
        if (! hops[i].voiced || ! hops[i - 1].voiced) continue;
        if (hops[i].f0Hz <= 0.0f || hops[i - 1].f0Hz <= 0.0f) continue;

        const double jump = std::fabs (PitchEngine::centsBetween (hops[i].f0Hz,
                                                                  hops[i - 1].f0Hz));
        if (jump > sumOut.largestJumpCents)
        {
            sumOut.largestJumpCents = jump;
            sumOut.largestJumpAtS   = hops[i].timeS;
        }
        if (jump > 600.0) ++sumOut.nearOctaveJumps;
    }

    return hops;
}

void printHops (const std::vector<Hop>& hops, bool csv)
{
    if (csv)
    {
        std::printf ("time_s,f0_hz,note,cents,confidence,voiced,guard,rms_db\n");
        for (const auto& h : hops)
            std::printf ("%.4f,%.3f,%s,%.1f,%.3f,%d,%d,%.2f\n",
                         h.timeS, h.f0Hz, h.note, h.cents, h.confidence,
                         h.voiced ? 1 : 0, h.guardFired ? 1 : 0, h.rmsDb);
        return;
    }

    std::printf ("    time      f0 Hz  note   cents   conf  V  guard\n");
    std::printf ("  ------------------------------------------------\n");
    for (const auto& h : hops)
    {
        if (h.voiced)
            std::printf ("  %7.3f  %9.2f  %-4s  %+6.1f  %5.2f  V  %s\n",
                         h.timeS, h.f0Hz, h.note, h.cents, h.confidence,
                         h.guardFired ? "GUARD" : "");
        else
            std::printf ("  %7.3f  %9s  %-4s  %6s  %5.2f  -  %s\n",
                         h.timeS, "-", "-", "-", h.confidence,
                         h.guardFired ? "GUARD" : "");
    }
}

void printSummary (const char* voiceId, const Summary& s)
{
    const double voicedPct = s.totalHops  > 0 ? 100.0 * s.voicedHops / s.totalHops  : 0.0;
    const double guardPct  = s.voicedHops > 0 ? 100.0 * s.guardFires / s.voicedHops : 0.0;

    std::printf ("\n  == summary: voice_type = %s ==\n", voiceId);
    std::printf ("     total hops          : %d\n", s.totalHops);
    std::printf ("     voiced              : %d (%.1f%%)\n", s.voicedHops, voicedPct);
    std::printf ("     octave-guard fires  : %d (%.2f%% of voiced hops)%s\n",
                 s.guardFires, guardPct,
                 // The spec's own reading of this number, printed so a run is
                 // self-interpreting rather than needing the spec open.
                 guardPct > 2.0 ? "   <-- OVER 2%: window may be wrong for this material"
                                : "");
    std::printf ("     median confidence   : %.3f  (over voiced hops)\n", s.medianConfidence);
    std::printf ("     largest frame jump  : %.1f cents (at %.3f s)\n",
                 s.largestJumpCents, s.largestJumpAtS);
    std::printf ("     jumps over 600 c    : %d  (adjacent voiced hops; octave-error proxy)\n",
                 s.nearOctaveJumps);

    // The headline regression number. Normalised per 1000 voiced hops so runs
    // over different files and different voice types (which voice a different
    // number of hops) compare directly - a raw count would make a setting look
    // better simply for having found less pitch.
    const double per1000 = s.voicedHops > 0 ? 1000.0 * s.nearOctaveJumps / s.voicedHops : 0.0;
    std::printf ("     RESIDUAL            : %.2f jumps > 600 c per 1000 voiced hops\n",
                 per1000);
}

int usage()
{
    std::printf (
        "pitch_probe - offline validation harness for EedPitchEngine (P0)\n\n"
        "  EchoJayPitchProbe <audio-file> [options]\n\n"
        "    --voice-type <id>   soprano | alto_tenor | low_male | instrument | bass\n"
        "                        (default: alto_tenor)\n"
        "    --tracking <id>     relaxed | normal | tight   (default: normal)\n"
        "    --all               run the file through all five voice types\n"
        "    --summary-only      suppress the per-hop table\n"
        "    --csv               per-hop table as CSV\n\n"
        "  Any sample rate is handled natively; nothing is resampled.\n");
    return 1;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::String path;
    int  voiceType   = PitchEngine::kAltoTenor;
    int  tracking    = PitchEngine::kNormal;
    bool allTypes    = false, summaryOnly = false, csv = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if      (a == "--all")          allTypes    = true;
        else if (a == "--summary-only") summaryOnly = true;
        else if (a == "--csv")          csv         = true;
        else if (a == "--help" || a == "-h") return usage();
        else if (a == "--voice-type")
        {
            if (++i >= argc) return usage();
            const juce::String want (argv[i]);
            int found = -1;
            for (int t = 0; t < PitchEngine::kNumVoiceTypes; ++t)
                if (want.equalsIgnoreCase (PitchEngine::voiceRange (t).id)) found = t;
            if (found < 0)
            {
                std::printf ("unknown voice type: %s\n", want.toRawUTF8());
                return usage();
            }
            voiceType = found;
        }
        else if (a == "--tracking")
        {
            if (++i >= argc) return usage();
            const juce::String want (argv[i]);
            int found = -1;
            const char* ids[] = { "relaxed", "normal", "tight" };
            for (int t = 0; t < PitchEngine::kNumTracking; ++t)
                if (want.equalsIgnoreCase (ids[t])) found = t;
            if (found < 0) { std::printf ("unknown tracking: %s\n", want.toRawUTF8()); return usage(); }
            tracking = found;
        }
        else if (a.startsWith ("-")) return usage();
        else path = a;
    }

    if (path.isEmpty()) return usage();

    const juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (path));
    if (! file.existsAsFile())
    {
        std::printf ("no such file: %s\n", file.getFullPathName().toRawUTF8());
        return 1;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
    {
        std::printf ("could not read as audio: %s\n", file.getFullPathName().toRawUTF8());
        return 1;
    }

    const double fs   = reader->sampleRate;
    const int    numS = (int) reader->lengthInSamples;
    const int    numC = (int) juce::jmin ((unsigned int) 2, reader->numChannels);
    if (numS <= 0 || numC <= 0) { std::printf ("empty file\n"); return 1; }

    juce::AudioBuffer<float> audio (numC, numS);
    reader->read (&audio, 0, numS, 0, true, numC > 1);

    std::printf ("file      : %s\n", file.getFileName().toRawUTF8());
    std::printf ("rate      : %.0f Hz (handled natively, not resampled)\n", fs);
    std::printf ("channels  : %d%s\n", numC, numC > 1 ? " (analysed as the mid sum)" : "");
    std::printf ("length    : %.2f s\n", (double) numS / fs);
    {
        const char* ids[] = { "relaxed", "normal", "tight" };
        std::printf ("tracking  : %s (confidence floor %.2f)\n",
                     ids[tracking], PitchEngine::trackingConfidence (tracking));
    }

    const int first = allTypes ? 0 : voiceType;
    const int last  = allTypes ? PitchEngine::kNumVoiceTypes - 1 : voiceType;

    for (int t = first; t <= last; ++t)
    {
        const auto& vr = PitchEngine::voiceRange (t);
        std::printf ("\n----------------------------------------------------------\n");
        std::printf ("voice_type: %-11s search %.0f-%.0f Hz\n", vr.id, vr.fMinHz, vr.fMaxHz);

        Summary s;
        const std::vector<Hop> hops = runFile (audio, fs, t, tracking, s);
        if (! summaryOnly) printHops (hops, csv);
        printSummary (vr.id, s);
    }

    return 0;
}
