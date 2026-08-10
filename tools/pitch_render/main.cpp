/*
    pitch_render  —  offline RENDER harness for the P1 shifter
    (PITCH_CORRECTION_SPEC.md §2.4).

    WHY THIS EXISTS. Every P1 number so far is measured, and none of it has been
    LISTENED to. This worktree must never install to the system plug-in folders
    (PARALLEL_SESSIONS.md: every worktree builds the same identifier to the same
    path, so an install silently overwrites whatever Logic is loading), which
    means a DAW is not available to audition through. An offline render is
    therefore the only honest way to hear the shift, and artefacts - buzz on
    consonants, warble on sustains, a formant that moved when it should not
    have - are things measurement can miss and ears cannot.

    It writes a SET of files into one folder, always including the DRY source,
    because judging an artefact means comparing against the thing it came from.

    Separate CMake target, EXCLUDE_FROM_ALL, not a dependency of any plugin
    target - same rule as pitch_probe.

    Usage:
        EchoJayPitchRender <audio-file> [--out <dir>] [--voice-type <id>]
                           [--tracking <id>] [--set | --semitones <n>]
                           [--target-hz <hz>] [--formant <preserve|off>]

    --set (the default) renders the whole comparison set in one pass.
*/

#include <JuceHeader.h>

#include "EedPitchEngine.h"
#include "EedPsolaEngine.h"
#include "EedPitchCorrect.h"

#include <cmath>
#include <cstdio>
#include <vector>

using echojay::PitchEngine;
using echojay::PitchReading;
using echojay::PsolaEngine;
using echojay::PitchCorrect;

namespace
{

struct RenderOpts
{
    int   voiceType   = PitchEngine::kAltoTenor;
    int   tracking    = PitchEngine::kNormal;
    int   formantMode = PsolaEngine::kFormantPreserve;
    float semitones   = 0.0f;    // relative shift; ignored when targetHz > 0
    float targetHz    = 0.0f;    // absolute fixed target

    // P2: when correct is on, the musical layer picks the target instead.
    bool  correct     = false;
    float retuneMs    = 120.0f;
    float flex        = 55.0f;
    float humanize    = 60.0f;
    int   keyRoot     = 0;
    int   scaleMask   = 0b111111111111;   // chromatic
    bool  ignoreVib   = true;
    float naturalVib  = 100.0f;
};

// One render pass. Returns the processed audio, already latency-COMPENSATED so
// every file in the set lines up against the dry when dropped on a timeline -
// otherwise every comparison starts with a manual nudge and half of them get
// judged misaligned.
juce::AudioBuffer<float> renderOne (const juce::AudioBuffer<float>& src, double fs,
                                    const RenderOpts& o, int& latencyOut)
{
    const int numCh = src.getNumChannels();
    const int numS  = src.getNumSamples();

    PitchEngine det;
    det.prepare (fs, 1024);
    det.setVoiceType (o.voiceType);
    det.setTracking (o.tracking);

    float worst = PitchEngine::voiceRange (0).fMinHz;
    for (int t = 1; t < PitchEngine::kNumVoiceTypes; ++t)
        worst = juce::jmin (worst, PitchEngine::voiceRange (t).fMinHz);

    // One shifter per channel: the detector runs on the mid sum, but each
    // channel has to be shifted with the same decisions.
    std::vector<std::unique_ptr<PsolaEngine>> shifters;
    for (int ch = 0; ch < numCh; ++ch)
    {
        auto e = std::make_unique<PsolaEngine>();
        e->prepare (fs, 1024, PitchEngine::voiceRange (o.voiceType).fMinHz, worst);
        e->setFormantMode (o.formantMode);
        shifters.push_back (std::move (e));
    }
    latencyOut = shifters[0]->latencySamples();

    juce::AudioBuffer<float> out (numCh, numS);
    out.clear();

    PitchCorrect corr;
    corr.prepare (fs, det.inputHopLength (o.voiceType));
    corr.initDegrees();
    for (int sdeg = 0; sdeg < 12; ++sdeg)
        corr.setDegree (sdeg, (o.scaleMask >> sdeg) & 1, 0.0f);
    corr.setKeyRoot (o.keyRoot);
    corr.setRetuneMs (o.retuneMs);
    corr.setFlex (o.flex);
    corr.setHumanize (o.humanize);
    corr.setIgnoreVibrato (o.ignoreVib);
    corr.setNaturalVibrato (o.naturalVib);
    corr.reset();

    const float ratio = std::pow (2.0f, o.semitones / 12.0f);

    constexpr int kBlock = 256;
    std::vector<float> mid ((size_t) kBlock);

    for (int pos = 0; pos + kBlock <= numS; pos += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            double a = 0.0;
            for (int ch = 0; ch < numCh; ++ch) a += src.getReadPointer (ch)[pos + i];
            mid[(size_t) i] = (float) (a / juce::jmax (1, numCh));
        }
        det.process (mid.data(), nullptr, kBlock);
        const PitchReading r = det.getReading();

        // A SEMITONE shift is relative, so the target has to follow the
        // detected pitch. A fixed target does not. Both drive the identical
        // engine path; only where the number comes from differs.
        const float target = o.correct
                           ? corr.process (r.f0Hz, r.voiced)
                           : (o.targetHz > 0.0f
                                ? o.targetHz
                                : (r.voiced && r.f0Hz > 0.0f ? r.f0Hz * ratio : 0.0f));

        for (int ch = 0; ch < numCh; ++ch)
        {
            shifters[(size_t) ch]->setTargetHz (target);
            shifters[(size_t) ch]->process (src.getReadPointer (ch) + pos,
                                            out.getWritePointer (ch) + pos,
                                            kBlock, r.f0Hz, r.voiced);
        }
    }

    // Latency-compensate: slide the output forward so it sits against the dry.
    juce::AudioBuffer<float> aligned (numCh, numS);
    aligned.clear();
    const int lat = latencyOut;
    for (int ch = 0; ch < numCh; ++ch)
        if (numS > lat)
            aligned.copyFrom (ch, 0, out, ch, lat, numS - lat);
    return aligned;
}

bool writeWavRaw (const juce::File& f, const juce::AudioBuffer<float>& buf, double fs);

// UNIFORM headroom across the whole set, including the dry.
//
// Shifting raises peaks - a 24-bit writer clamps at 1.0 and several renders
// measured EXACTLY 1.000, i.e. clipped. Clipping in a listening set is worse
// than useless: it is an artefact that did not come from the shifter, and it
// is exactly the kind of thing that gets blamed on PSOLA. Trimming every file
// by the SAME amount keeps the set level-matched, so an A/B still compares
// what it claims to compare.
constexpr float kHeadroomGain = 0.5f;      // -6 dB

bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& bufIn, double fs,
               float& peakOut)
{
    juce::AudioBuffer<float> buf (bufIn.getNumChannels(), bufIn.getNumSamples());
    peakOut = 0.0f;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        const float* src = bufIn.getReadPointer (ch);
        float* dst = buf.getWritePointer (ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            peakOut = juce::jmax (peakOut, std::abs (src[i]));
            dst[i] = src[i] * kHeadroomGain;
        }
    }
    return writeWavRaw (f, buf, fs);
}

bool writeWavRaw (const juce::File& f, const juce::AudioBuffer<float>& buf, double fs)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    if (os == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> w (
        fmt.createWriterFor (os.get(), fs, (unsigned int) buf.getNumChannels(), 24, {}, 0));
    if (w == nullptr) return false;
    os.release();                       // the writer owns it now
    return w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

int usage()
{
    std::printf (
        "pitch_render - offline render harness for the P1 shifter\n\n"
        "  EchoJayPitchRender <audio-file> [options]\n\n"
        "    --out <dir>         output folder (default: alongside the input)\n"
        "    --voice-type <id>   soprano | alto_tenor | low_male | instrument | bass\n"
        "    --tracking <id>     relaxed | normal | tight\n"
        "    --set               render the whole comparison set (default)\n"
        "    --semitones <n>     render ONE relative shift instead\n"
        "    --target-hz <hz>    render ONE fixed-target shift instead\n"
        "    --formant <mode>    preserve | off   (single renders only)\n\n"
        "  Every render is latency-compensated, so the files line up against\n"
        "  the dry on a timeline without nudging.\n");
    return 1;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::String path, outDir;
    RenderOpts base;
    bool wholeSet = true;
    bool haveSingle = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] () -> juce::String { return ++i < argc ? juce::String (argv[i]) : juce::String(); };

        if      (a == "--set")   wholeSet = true;
        else if (a == "--out")   outDir = next();
        else if (a == "--help" || a == "-h") return usage();
        else if (a == "--voice-type")
        {
            const juce::String want = next();
            int found = -1;
            for (int t = 0; t < PitchEngine::kNumVoiceTypes; ++t)
                if (want.equalsIgnoreCase (PitchEngine::voiceRange (t).id)) found = t;
            if (found < 0) { std::printf ("unknown voice type: %s\n", want.toRawUTF8()); return usage(); }
            base.voiceType = found;
        }
        else if (a == "--tracking")
        {
            const juce::String want = next();
            const char* ids[] = { "relaxed", "normal", "tight" };
            int found = -1;
            for (int t = 0; t < PitchEngine::kNumTracking; ++t)
                if (want.equalsIgnoreCase (ids[t])) found = t;
            if (found < 0) { std::printf ("unknown tracking: %s\n", want.toRawUTF8()); return usage(); }
            base.tracking = found;
        }
        else if (a == "--semitones") { base.semitones = next().getFloatValue(); wholeSet = false; haveSingle = true; }
        else if (a == "--target-hz") { base.targetHz  = next().getFloatValue(); wholeSet = false; haveSingle = true; }
        else if (a == "--formant")
        {
            const juce::String want = next();
            base.formantMode = want.equalsIgnoreCase ("off") ? PsolaEngine::kFormantOff
                                                             : PsolaEngine::kFormantPreserve;
        }
        else if (a.startsWith ("-")) return usage();
        else path = a;
    }
    if (path.isEmpty()) return usage();
    (void) haveSingle;

    const juce::File in (juce::File::getCurrentWorkingDirectory().getChildFile (path));
    if (! in.existsAsFile()) { std::printf ("no such file: %s\n", in.getFullPathName().toRawUTF8()); return 1; }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (in));
    if (reader == nullptr) { std::printf ("could not read as audio\n"); return 1; }

    const double fs   = reader->sampleRate;
    const int    numS = (int) reader->lengthInSamples;
    const int    numC = (int) juce::jmin ((unsigned int) 2, reader->numChannels);
    if (numS <= 0 || numC <= 0) { std::printf ("empty file\n"); return 1; }

    juce::AudioBuffer<float> src (numC, numS);
    reader->read (&src, 0, numS, 0, true, numC > 1);

    juce::File dir = outDir.isNotEmpty()
                   ? juce::File::getCurrentWorkingDirectory().getChildFile (outDir)
                   : in.getParentDirectory().getChildFile (in.getFileNameWithoutExtension() + " - pitch renders");
    dir.createDirectory();

    std::printf ("source   : %s\n", in.getFileName().toRawUTF8());
    std::printf ("rate     : %.0f Hz, %d ch, %.1f s\n", fs, numC, (double) numS / fs);
    std::printf ("voice    : %s, tracking %s\n",
                 PitchEngine::voiceRange (base.voiceType).id,
                 (const char*[]) { "relaxed", "normal", "tight" }[base.tracking]);
    std::printf ("out      : %s\n\n", dir.getFullPathName().toRawUTF8());

    // The dry ALWAYS goes in the folder: an artefact is only judgeable next to
    // the thing it came from.
    std::printf ("headroom : every file trimmed %.1f dB, uniformly, so the set stays\n"
                 "           level-matched and nothing clips\n\n",
                 20.0 * std::log10 (kHeadroomGain));

    float dryPeak = 0.0f;
    writeWav (dir.getChildFile ("00 dry (source).wav"), src, fs, dryPeak);
    std::printf ("  %-58s peak %.3f\n", "00 dry (source).wav", dryPeak);

    struct Job { juce::String name; RenderOpts o; };
    std::vector<Job> jobs;

    if (wholeSet)
    {
        RenderOpts fixed = base;
        fixed.targetHz = 220.0f;
        jobs.push_back ({ "01 fixed target 220Hz (every note to A3).wav", fixed });

        // Named so the ordering on disk matches the ordering by ear.
        const int semis[] = { -12, -7, -3, -1, 1, 3, 7, 12 };
        int idx = 2;
        for (int st : semis)
        {
            RenderOpts o = base;
            o.semitones = (float) st;
            juce::String nm;
            nm << juce::String (idx++).paddedLeft ('0', 2) << " shift "
               << (st > 0 ? "+" : "-") << juce::String (std::abs (st)).paddedLeft ('0', 2)
               << "st (formants preserved).wav";
            jobs.push_back ({ nm, o });
        }

        // ---- P2: THE TWO CHARACTERS, side by side ---------------------
        // The spec's headline claim is that these are the same machine at two
        // points in its parameter space, not two algorithms. Rendering them
        // from one code path with only the numbers changed is what makes that
        // checkable by ear.
        {
            // BOTH characters run CHROMATIC, and that is deliberate. The thing
            // being compared is retune speed, flex and humanize - so the scale
            // has to be held constant, and chromatic is the honest constant
            // when the take's key is not known. Forcing a scale the singer is
            // not in makes `natural` measure WORSE than the dry signal (46
            // cents from a degree against 13), because partial correction
            // toward foreign notes lands between semitones. That is the
            // corrector working, and the wrong scale being asked for.
            RenderOpts hard = base;
            hard.correct = true; hard.retuneMs = 0.0f; hard.flex = 0.0f;
            hard.humanize = 0.0f; hard.ignoreVib = false; hard.naturalVib = 0.0f;
            hard.scaleMask = 0b111111111111;
            jobs.push_back ({ "30 CHARACTER A - hard tuned (retune 0, flex 0, chromatic).wav", hard });

            RenderOpts nat = base;
            nat.correct = true; nat.retuneMs = 120.0f; nat.flex = 55.0f;
            nat.humanize = 60.0f; nat.ignoreVib = true; nat.naturalVib = 100.0f;
            nat.scaleMask = 0b111111111111;
            jobs.push_back ({ "31 CHARACTER B - natural (retune 120, flex 55, human 60, chromatic).wav", nat });

            // What forcing a scale the take is NOT in actually sounds like -
            // kept because it is the failure mode a user will hit first, and
            // hearing it is the fastest way to learn to check the key.
            RenderOpts wrongKey = nat;
            wrongKey.scaleMask = 0b010110101101;      // C natural minor
            jobs.push_back ({ "32 WRONG KEY demo - natural forced to C minor.wav", wrongKey });
        }

        // The formant A/B, at the shift where it is most audible.
        for (int st : { -7, 7 })
        {
            RenderOpts keep = base; keep.semitones = (float) st;
            keep.formantMode = PsolaEngine::kFormantPreserve;
            RenderOpts move = base; move.semitones = (float) st;
            move.formantMode = PsolaEngine::kFormantOff;

            juce::String sign = st > 0 ? "+" : "-";
            juce::String mag  = juce::String (std::abs (st)).paddedLeft ('0', 2);
            jobs.push_back ({ "20 FORMANT AB " + sign + mag + "st A - preserve (voice stays).wav", keep });
            jobs.push_back ({ "21 FORMANT AB " + sign + mag + "st B - off (chipmunk).wav", move });
        }
    }
    else
    {
        juce::String nm = "render";
        if (base.targetHz > 0.0f) nm = "fixed target " + juce::String (base.targetHz, 1) + "Hz";
        else                      nm = "shift " + juce::String (base.semitones, 1) + "st";
        nm << (base.formantMode == PsolaEngine::kFormantOff ? " (formant off)" : " (formants preserved)");
        jobs.push_back ({ nm + ".wav", base });
    }

    for (const auto& j : jobs)
    {
        int lat = 0;
        const juce::AudioBuffer<float> outBuf = renderOne (src, fs, j.o, lat);
        float pk = 0.0f;
        const bool ok = writeWav (dir.getChildFile (j.name), outBuf, fs, pk);
        std::printf ("  %-58s peak %.3f%s  (latency %d smp / %.1f ms, compensated)\n",
                     j.name.toRawUTF8(), pk,
                     ok ? "" : "  WRITE FAILED",
                     lat, 1000.0 * lat / fs);
    }

    std::printf ("\n%d files in %s\n", (int) jobs.size() + 1, dir.getFullPathName().toRawUTF8());
    return 0;
}
