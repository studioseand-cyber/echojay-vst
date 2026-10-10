/*
  spectral_evidence_measure — COMPARE_REFERENCE_PLAN section 1.6.

  Section 1.6 is explicit that Phase 0 is not verified by reading the diff. It
  is verified by measuring the same pair twice and watching the six band deltas
  move in an explicable direction. This harness is that measurement.

  It drives the SHIPPED code, not a model of it:

    ReferenceAnalyser   the real whole-file pass, producing BOTH
                          eqCurve      the mean of every analysis block
                          data.spectrum the meter's reading after the last block
    echojay::bandDeltas the real six-band reduction, moved into
                        EJSpectralEvidence.h for exactly this reason

  BEFORE  = bandDeltas(capture whole-file average, reference data.spectrum)
  AFTER   = bandDeltas(capture whole-file average, reference eqCurve)

  The capture side is IDENTICAL in both rows on purpose, so the only thing that
  changes is the reference side, which is the thing Phase 0 changes. Varying
  both would leave the direction unattributable.

  A capture's avgSpectrum is the mean of the meter's spectrum across every block
  of the capture, which is what eqCurve is across every block of a file. So a
  file analysed by ReferenceAnalyser is a faithful stand-in for a Mix Bus
  capture, and no capture has to be taken in a DAW to run this.

  READ ONLY ON THE USER'S AUDIO. It opens the files it is given for reading
  and writes nothing into them or beside them. The files under
  ~/Documents/EchoJay/References and ~/Library/EchoJay/codec_cache belong to
  the user and the cache is live. The ONE thing it writes is its own
  synthesised control tone, into the system temp directory, never into the
  library.

  ===========================================================================
  PHASE 1b EXTENSION (commit 2): THE MACRO BANDS, BEFORE THE ACCUMULATION
  ===========================================================================

  Phase 0b fixed the reference side of the SPECTRUM. The six MACRO BANDS were
  never touched: ReferenceAnalyser stores engine.getMeterData() after the last
  block and overrides width and correlation but not macroBandDb, so a stored
  reference's band levels are a ballistic tail with a ~150 ms release. The
  capture side has the same hole, since stopCapture rebuilds spectrum, peaks,
  RMS and crest over the whole capture and leaves macroBandDb alone.

  This extension takes the BEFORE numbers for that change, with the harness
  that will take the AFTER numbers, so the two are comparable by construction.

  THREE MODES:
    --control            synthesise a steady-state file and prove the harness
                         reports tail and whole-file as AGREEING on it. Run
                         this first: if it disagrees, nothing after it means
                         anything.
    --census <folder>    every file in the folder: duration, the tail, the
                         last 500 ms, the whole-file figures, and whether the
                         tail is live or has faded into the floor.
    <ref> <cap>          the original section 1.6 pair measurement, unchanged,
                         plus the macro-band rows for the reference.

  WHOSE ARITHMETIC. The per-band values come from the SHIPPED MeterEngine:
  this harness runs it at the same 2048-sample block size ReferenceAnalyser
  uses and samples MeterData::macroBandDb after every block. The read-out loop
  is the harness's own, and it is deliberately the same loop Phase 1b will add
  to ReferenceAnalyser, which is what makes BEFORE and AFTER comparable. To
  keep that claim checkable rather than asserted, the pair mode ALSO runs the
  real ReferenceAnalyser and compares its eqCurve against the one this loop
  accumulates: they must match exactly, or the loop is not the loop it claims
  to be.

  TWO WHOLE-FILE VARIANTS, because the accumulation has a choice to make and
  it has not been made yet:
    meanDb    the arithmetic mean of the per-block dB values. This is exactly
              what eqCurve already does to the spectrum, so it is the
              precedent, and the one-pole is linear in dB so its mean over a
              whole file converges to the mean of the unsmoothed input with an
              error of order tau/duration.
    meanPow   the mean of the per-block values converted to power, back to dB.
              Energy-correct, and NOT the same number on material with a wide
              dynamic range.
  Both are recorded so that whichever the accumulation picks, its BEFORE
  number already exists and nothing has to be re-measured.

  WHAT THIS CANNOT SEE. MeterEngine does not publish the raw per-band power
  before smoothing, and this commit may not change Source/, so both whole-file
  variants are means of the SMOOTHED per-block value rather than of the raw
  band power. Over a whole file the difference is the settling error above;
  over a few blocks it is not, and this harness should not be used to measure
  a short window.
*/

#include <JuceHeader.h>
#include "ReferenceAnalyser.h"
#include "MeterEngine.h"
#include "EJSpectralEvidence.h"
#include "EJBandScheme.h"
#include <iostream>
#include <iomanip>
#include <vector>

static bool analyseOne (const juce::File& f, ReferenceResult& out)
{
    ReferenceAnalyser ra;
    bool done = false, ok = false;
    juce::String err;
    ra.analyseFile (f, [&] (bool s, const juce::String& e) { ok = s; err = e; done = true; });

    const auto t0 = juce::Time::getMillisecondCounter();
    while (ra.isAnalysing() && juce::Time::getMillisecondCounter() - t0 < 180000)
        juce::Thread::sleep (50);
    juce::Thread::sleep (100);   // let the push under refMutex settle

    if (ra.getReferenceCount() < 1)
    {
        std::cout << "  FAILED to analyse " << f.getFileName() << "  ("
                  << (done ? err.toStdString() : std::string ("timed out")) << ")\n";
        return false;
    }
    out = ra.getReference (0);
    return true;
}

// ===========================================================================
// THE MACRO-BAND PASS (Phase 1b commit 2)
// ===========================================================================

struct MacroRow
{
    bool  ok = false;
    juce::String name;
    double sampleRate = 0.0;
    float  seconds = 0.0f;          // the file's length
    int    frames  = 0;             // blocks the whole-file figures ran over
    int    tailFrames = 0;          // blocks inside the final 500 ms
    float  tailSeconds = 0.0f;      // what those blocks actually cover

    std::array<float, 6> tail    {};   // the shipped value: the last block's reading
    std::array<float, 6> last500 {};   // mean of the blocks covering the final 500 ms
    std::array<float, 6> meanDb  {};   // whole file, mean of per-block dB
    std::array<float, 6> meanPow {};   // whole file, mean of per-block power

    std::array<float, 64> eqCurve {};  // accumulated exactly as ReferenceAnalyser does

    // Phase 1b commit 3: the engine's OWN whole-run accumulator, a mean of
    // POWER, read through the shipped getAccumulatedBands().
    std::array<float, 6> accumDb {};
    bool  accumValid = false;
    int   accumBlocks = 0;
    // Phase 1c: the BOUNDED window, which is what a Live compare slot now reads.
    std::array<float, 6> boundedDb {};
    bool  boundedValid = false;
    int   boundedBlocks = 0;
    float boundedSeconds = 0.0f;
    int   silentBlocks = 0;            // blocks whose six bands are all on the floor
};

/** rel = band minus the mean of the six in the SAME row. Recomputed from the
    row's own dB values, never averaged from per-frame rels: that distinction is
    argued at MeterEngine::reduceMacroWindow and it applies identically here. */
static std::array<float, 6> relsOf (const std::array<float, 6>& db)
{
    float mean = 0.0f; int n = 0;
    for (int i = 0; i < 6; ++i) if (db[(size_t) i] > -119.0f) { mean += db[(size_t) i]; ++n; }
    std::array<float, 6> r {};
    if (n == 0) { r.fill (0.0f); return r; }
    mean /= (float) n;
    for (int i = 0; i < 6; ++i)
        r[(size_t) i] = db[(size_t) i] > -119.0f ? db[(size_t) i] - mean : 0.0f;
    return r;
}

/** The accumulation itself, shared by the file path and the synthesised-buffer
    path so a control and a real reference are measured by the SAME arithmetic.
    Two loops would leave a control that could pass while the thing it vouches
    for was computed differently. */
struct MacroAccum
{
    std::array<double, 6>  sumDb {}, sumPow {};
    std::array<double, 64> specSum {};
    std::vector<std::array<float, 6>> lastBlocks;
    std::array<float, 6> last {};
    int n = 0, want500 = 1;

    void begin (double sr, int blockSize)
    {
        sumDb.fill (0.0); sumPow.fill (0.0); specSum.fill (0.0);
        last.fill (-120.0f); n = 0; lastBlocks.clear();
        want500 = juce::jmax (1, (int) std::ceil (0.5 / ((double) blockSize / sr)));
    }

    void push (const MeterData& md)
    {
        for (int i = 0; i < 6; ++i)
        {
            const float db = md.macroBandDb[(size_t) i];
            sumDb[(size_t) i]  += (double) db;
            sumPow[(size_t) i] += std::pow (10.0, (double) db / 10.0);
        }
        for (int i = 0; i < 64; ++i) specSum[(size_t) i] += (double) md.spectrum[(size_t) i];
        ++n;
        last = md.macroBandDb;
        lastBlocks.push_back (last);
        if ((int) lastBlocks.size() > want500) lastBlocks.erase (lastBlocks.begin());
    }

    bool finish (MacroRow& out, double sr, int blockSize) const
    {
        if (n == 0) return false;
        out.frames      = n;
        out.tail        = last;
        out.tailFrames  = (int) lastBlocks.size();
        out.tailSeconds = (float) ((double) out.tailFrames * (double) blockSize / sr);
        for (int i = 0; i < 6; ++i)
        {
            out.meanDb[(size_t) i] = (float) (sumDb[(size_t) i] / (double) n);
            const double p = sumPow[(size_t) i] / (double) n;
            out.meanPow[(size_t) i] = p > 1e-12 ? (float) (10.0 * std::log10 (p)) : -120.0f;
            double t = 0.0;
            for (auto& b : lastBlocks) t += (double) b[(size_t) i];
            out.last500[(size_t) i] = (float) (t / (double) lastBlocks.size());
        }
        for (int i = 0; i < 64; ++i)
            out.eqCurve[(size_t) i] = (float) (specSum[(size_t) i] / (double) n);
        out.ok = true;
        return true;
    }
};

/** The same pass over an in-memory buffer: no file, no disk, for the controls. */
static bool macroPassBuffer (const juce::AudioBuffer<float>& buf, double sr,
                             const juce::String& name, MacroRow& out)
{
    const int blockSize = 2048;
    const int total = buf.getNumSamples();
    if (sr <= 0.0 || total <= 0) return false;

    MeterEngine engine;
    engine.prepare (sr, blockSize);
    out.name = name; out.sampleRate = sr;
    out.seconds = (float) ((double) total / sr);

    MacroAccum acc; acc.begin (sr, blockSize);
    const int nch = buf.getNumChannels();
    for (int read = 0; read < total; read += blockSize)
    {
        const int want = juce::jmin (blockSize, total - read);
        const float* L = buf.getReadPointer (0, read);
        const float* R = nch >= 2 ? buf.getReadPointer (1, read) : L;
        engine.processBlock (L, R, want);
        const auto md = engine.getMeterData();
        acc.push (md);
        bool allFloor = true;
        for (int i = 0; i < 6; ++i) if (md.macroBandDb[(size_t) i] > -110.0f) allFloor = false;
        if (allFloor) ++out.silentBlocks;
    }
    if (! acc.finish (out, sr, blockSize)) return false;
    const auto a = engine.getAccumulatedBands();
    out.accumValid = a.valid; out.accumBlocks = a.blocks; out.accumDb = a.db;
    const auto bw = engine.getBoundedBands();
    out.boundedValid = bw.valid; out.boundedBlocks = bw.blocks;
    out.boundedDb = bw.db; out.boundedSeconds = bw.seconds;
    return true;
}

/** Run the SHIPPED MeterEngine over a whole file at ReferenceAnalyser's block
    size, sampling the published macro bands after every block. */
static bool macroPass (const juce::File& f, MacroRow& out)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr) return false;

    const double sr = reader->sampleRate;
    const juce::int64 total = reader->lengthInSamples;
    const int nch = (int) reader->numChannels;
    if (sr <= 0.0 || total <= 0) return false;

    const int blockSize = 2048;          // ReferenceAnalyser.cpp:127, deliberately
    MeterEngine engine;
    engine.prepare (sr, blockSize);

    out.name       = f.getFileName();
    out.sampleRate = sr;
    out.seconds    = (float) ((double) total / sr);

    // How many trailing blocks fall inside the final 500 ms.
    const double blockSec = (double) blockSize / sr;
    const int    want500  = juce::jmax (1, (int) std::ceil (0.5 / blockSec));

    std::array<double, 6> sumDb {}, sumPow {};
    std::array<double, 64> specSum {};
    sumDb.fill (0.0); sumPow.fill (0.0); specSum.fill (0.0);
    int n = 0;

    std::vector<std::array<float, 6>> lastBlocks;   // ring of the final blocks

    juce::AudioBuffer<float> buf (juce::jmax (1, nch), blockSize);
    juce::int64 read = 0;
    std::array<float, 6> last {}; last.fill (-120.0f);

    while (read < total)
    {
        const int want = (int) juce::jmin ((juce::int64) blockSize, total - read);
        buf.clear();
        reader->read (&buf, 0, want, read, true, true);
        const float* L = buf.getReadPointer (0);
        const float* R = nch >= 2 ? buf.getReadPointer (1) : L;
        engine.processBlock (L, R, want);

        const auto md = engine.getMeterData();
        for (int i = 0; i < 6; ++i)
        {
            const float db = md.macroBandDb[(size_t) i];
            sumDb[(size_t) i]  += (double) db;
            sumPow[(size_t) i] += std::pow (10.0, (double) db / 10.0);
        }
        // eqCurve, accumulated EXACTLY as ReferenceAnalyser.cpp:215-219 does.
        for (int i = 0; i < 64; ++i) specSum[(size_t) i] += (double) md.spectrum[(size_t) i];
        ++n;

        last = md.macroBandDb;
        lastBlocks.push_back (last);
        if ((int) lastBlocks.size() > want500) lastBlocks.erase (lastBlocks.begin());

        read += want;
    }
    if (n == 0) return false;

    // THE SHIPPED ACCUMULATION. Added to this path as well as the buffer path:
    // without it the pair tables silently omitted the one row the change is
    // about, and an absent row reads like a value of zero to nobody's benefit.
    {
        const auto a = engine.getAccumulatedBands();
        out.accumValid = a.valid; out.accumBlocks = a.blocks; out.accumDb = a.db;
    }

    out.frames      = n;
    out.tail        = last;
    out.tailFrames  = (int) lastBlocks.size();
    out.tailSeconds = (float) ((double) out.tailFrames * blockSec);

    for (int i = 0; i < 6; ++i)
    {
        out.meanDb[(size_t) i]  = (float) (sumDb[(size_t) i] / (double) n);
        const double p = sumPow[(size_t) i] / (double) n;
        out.meanPow[(size_t) i] = p > 1e-12 ? (float) (10.0 * std::log10 (p)) : -120.0f;
        double t = 0.0;
        for (auto& b : lastBlocks) t += (double) b[(size_t) i];
        out.last500[(size_t) i] = (float) (t / (double) lastBlocks.size());
    }
    for (int i = 0; i < 64; ++i)
        out.eqCurve[(size_t) i] = (float) (specSum[(size_t) i] / (double) n);

    out.ok = true;
    return true;
}

static void printRow (const char* label, const std::array<float, 6>& v)
{
    std::cout << "  " << std::left << std::setw (34) << label;
    for (int i = 0; i < 6; ++i)
        std::cout << std::right << std::setw (9) << std::fixed << std::setprecision (2)
                  << v[(size_t) i];
    std::cout << "\n";
}

static void printRowSigned (const char* label, const std::array<float, 6>& v)
{
    std::cout << "  " << std::left << std::setw (34) << label;
    for (int i = 0; i < 6; ++i)
        std::cout << std::right << std::setw (9) << std::showpos << std::fixed
                  << std::setprecision (2) << v[(size_t) i] << std::noshowpos;
    std::cout << "\n";
}

static std::array<float, 6> diffOf (const std::array<float, 6>& a,
                                    const std::array<float, 6>& b)
{
    std::array<float, 6> d {};
    for (int i = 0; i < 6; ++i) d[(size_t) i] = a[(size_t) i] - b[(size_t) i];
    return d;
}

/** A tail is DEAD when every band has decayed into the floor: the file faded to
    silence and the stored reading describes nothing. That is not a wrong number,
    it is an absent one, and it is why six of eight references produced no tonal
    advice at all before Phase 0b. */
static bool tailIsDead (const std::array<float, 6>& t)
{
    for (int i = 0; i < 6; ++i) if (t[(size_t) i] > -110.0f) return false;
    return true;
}

// ---------------------------------------------------------------------------
// THE CONTROL: a steady-state file, written to the system temp directory.
// ---------------------------------------------------------------------------
// On material with no fade the tail and the whole-file figures must agree. If
// they do not, the harness is measuring something other than what it says and
// every number after it is void. Deterministic: a fixed-seed LCG through a
// Voss-style pink filter, so two runs produce the same file.
/** Deterministic pink noise into a buffer. Same Kellet filter the file writer
    uses; the SEED is the only thing that changes between control B's runs. */
static juce::AudioBuffer<float> makePinkBuffer (juce::uint32 seed, double sr, int blocks)
{
    const int total = blocks * 2048;
    juce::AudioBuffer<float> buf (2, total);
    auto white = [&seed] ()
    {
        seed = seed * 1664525u + 1013904223u;
        return (double) ((double) (seed >> 8) / 8388608.0 - 1.0);
    };
    std::array<std::array<double, 7>, 2> st {};
    for (auto& a : st) a.fill (0.0);
    for (int i = 0; i < total; ++i)
        for (int ch = 0; ch < 2; ++ch)
        {
            const double w = white();
            auto& v = st[(size_t) ch];
            v[0] = 0.99886 * v[0] + w * 0.0555179;
            v[1] = 0.99332 * v[1] + w * 0.0750759;
            v[2] = 0.96900 * v[2] + w * 0.1538520;
            v[3] = 0.86650 * v[3] + w * 0.3104856;
            v[4] = 0.55000 * v[4] + w * 0.5329522;
            v[5] = -0.7616 * v[5] - w * 0.0168980;
            const double o = (v[0]+v[1]+v[2]+v[3]+v[4]+v[5]+v[6] + w * 0.5362) * 0.08;
            v[6] = w * 0.115926;
            buf.setSample (ch, i, (float) juce::jlimit (-1.0, 1.0, o));
        }
    return buf;
}

static juce::File writePinkControl (double seconds)
{
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                 .getChildFile ("ej_phase1b_control_pink.wav");
    f.deleteFile();

    const double sr = 44100.0;
    const int total = (int) (sr * seconds);
    juce::AudioBuffer<float> buf (2, total);

    juce::uint32 seed = 0x13579BDFu;
    auto white = [&seed] ()
    {
        seed = seed * 1664525u + 1013904223u;
        return (float) ((double) (seed >> 8) / 8388608.0 - 1.0);   // -1 .. 1
    };
    // Paul Kellet's economy pink filter, one INDEPENDENT state array per channel.
    // (Written as an array on purpose: reaching from &b0 across separate locals
    // as if they were contiguous is undefined behaviour, however reliably it
    // happens to work.)
    std::array<std::array<double, 7>, 2> st {};
    for (auto& a : st) a.fill (0.0);
    for (int i = 0; i < total; ++i)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const double w = (double) white();
            auto& s = st[(size_t) ch];
            s[0] = 0.99886 * s[0] + w * 0.0555179;
            s[1] = 0.99332 * s[1] + w * 0.0750759;
            s[2] = 0.96900 * s[2] + w * 0.1538520;
            s[3] = 0.86650 * s[3] + w * 0.3104856;
            s[4] = 0.55000 * s[4] + w * 0.5329522;
            s[5] = -0.7616 * s[5] - w * 0.0168980;
            const double out = (s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + w * 0.5362) * 0.08;
            s[6] = w * 0.115926;
            buf.setSample (ch, i, (float) juce::jlimit (-1.0, 1.0, out));
        }
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    if (os == nullptr) return {};
    std::unique_ptr<juce::AudioFormatWriter> w (
        wav.createWriterFor (os.release(), sr, 2, 24, {}, 0));
    if (w == nullptr) return {};
    w->writeFromAudioSampleBuffer (buf, 0, total);
    w.reset();
    return f;
}

static void printBands (const char* label, const echojay::BandLevels& b)
{
    const float v[6] { b.sub, b.low, b.lowMid, b.mid, b.highMid, b.high };
    std::cout << "  " << std::left << std::setw (34) << label;
    for (int i = 0; i < 6; ++i)
        std::cout << std::right << std::setw (9) << std::fixed << std::setprecision (2) << v[i];
    std::cout << "\n";
}

static void printDeltas (const char* label, const echojay::BandDeltaSet& d)
{
    std::cout << "  " << std::left << std::setw (34) << label;
    if (! d.valid) { std::cout << "   INVALID (unset or no signal)\n"; return; }
    for (int i = 0; i < 6; ++i)
        std::cout << std::right << std::setw (9) << std::showpos << std::fixed
                  << std::setprecision (2) << d.delta[(size_t) i] << std::noshowpos;
    std::cout << "\n";
}

static const char* kBandHdr =
    "                                          sub      low   lowMid      mid  highMid     high";

/** One file's macro-band rows, absolutes and relatives, with the window and the
    frame count on every row that has one. A figure without its window cannot be
    checked (decision 4 of the parent plan). */
static void reportMacro (const MacroRow& m, bool verbose)
{
    std::cout << "\n  " << m.name << "   " << juce::String (m.seconds, 1) << " s, "
              << juce::String (m.sampleRate / 1000.0, 1) << " kHz, "
              << m.frames << " blocks\n";
    std::cout << kBandHdr << "\n";

    // AFTER Phase 1b commit 4 the comparisons read accumDb, not tail. Both rows
    // are printed so the BEFORE tables in RESULTS_PHASE1B.md stay comparable
    // line for line and the change is visible rather than asserted.
    if (m.accumValid) printRow ("ACCUM ABS (what ships now)", m.accumDb);
    printRow ("tail  ABS  (last block, ~450ms)", m.tail);
    printRow ("last500 ABS", m.last500);
    printRow ("whole meanDb ABS", m.meanDb);
    if (verbose) printRow ("whole meanPow ABS", m.meanPow);
    printRowSigned ("DIFF  whole meanDb - tail", diffOf (m.meanDb, m.tail));

    std::cout << "\n";
    if (m.accumValid) printRowSigned ("ACCUM REL (what ships now)", relsOf (m.accumDb));
    printRowSigned ("tail  REL", relsOf (m.tail));
    printRowSigned ("last500 REL", relsOf (m.last500));
    printRowSigned ("whole meanDb REL", relsOf (m.meanDb));
    if (verbose) printRowSigned ("whole meanPow REL", relsOf (m.meanPow));
    printRowSigned ("DIFF  REL whole - tail", diffOf (relsOf (m.meanDb), relsOf (m.tail)));

    std::cout << "    windows: tail 1 block (" << juce::String (2048.0 / m.sampleRate, 3)
              << " s, ~450 ms of ballistic memory) | last500 " << m.tailFrames
              << " blocks (" << juce::String (m.tailSeconds, 3) << " s) | whole "
              << m.frames << " blocks (" << juce::String (m.seconds, 1) << " s)\n";
    std::cout << "    tail is " << (tailIsDead (m.tail) ? "DEAD (faded into the floor)"
                                                        : "LIVE") << "\n";
}

// ---------------------------------------------------------------------------
// CONTROL A: DETERMINISTIC MULTI-TONE. THIS ONE GATES.
// ---------------------------------------------------------------------------
// Every tone sits on an exact FFT bin centre, a multiple of sampleRate/2048.
// computeSpectrum refills its 2048-sample ring with each 2048-sample block, so
// fftWritePos returns to the same value every block and the window lands on the
// same phase: a tone periodic in 2048 samples therefore yields an IDENTICAL
// frame every block. Frame-to-frame variance is not small here, it is zero.
// A tone off a bin centre would leak differently as its phase walked, putting
// back exactly the variance this control exists to remove.
//
// THE LENGTH IS AN EXACT MULTIPLE OF 2048 on purpose. A partial final block
// feeds fewer samples, shifting the ring alignment, so the last frame would
// legitimately differ from every other one and would read as a defect.
//
// sub gets ONE tone because the band holds only two FFT bins at 44.1 kHz, and
// the lower of them sits on the 20 Hz edge.
static juce::AudioBuffer<float> makeMultiTone (double sr, int blocks)
{
    const int    blockSize = 2048;
    const int    total = blocks * blockSize;
    const double binHz = sr / (double) blockSize;
    const int    ks[14] { 2, 5, 9, 15, 20, 30, 50, 80, 110, 180, 250, 320, 500, 800 };
    const double amp = 0.06;                 // 14 x 0.06 = 0.84 worst-case peak

    juce::AudioBuffer<float> buf (2, total);
    buf.clear();
    for (int i = 0; i < total; ++i)
    {
        double v = 0.0;
        for (int t = 0; t < 14; ++t)
            v += amp * std::sin (2.0 * juce::MathConstants<double>::pi
                                 * ((double) ks[t] * binHz) * (double) i / sr);
        buf.setSample (0, i, (float) v);
        buf.setSample (1, i, (float) v);
    }
    return buf;
}

// ---------------------------------------------------------------------------
// THE SILENCE EXPERIMENT (Phase 1b commit 3)
// ---------------------------------------------------------------------------
// THE CLAIM UNDER TEST: not skipping silent blocks compresses the band
// relatives toward zero. The reading being checked is that this holds for a dB
// mean and NOT for a power mean.
//
//   POWER MEAN. Silence adds zero power, so the sum is unchanged and only the
//   divisor grows: every band's mean power becomes P*(1-f), which in dB is the
//   SAME constant added to all six. The six-band mean shifts by that same
//   constant and it cancels out of the relatives entirely.
//     predicted: relatives EXACTLY unchanged; absolutes all shift by
//     10*log10(1-f) = -0.46 dB at f=0.10, -1.25 at 0.25, -3.01 at 0.50.
//
//   dB MEAN. A silent block contributes the FLOOR, so dB' = (1-f)*dB + f*F and
//   the mean moves the same way; subtracting gives rel' = (1-f)*rel.
//     predicted: relatives scaled by 0.90 / 0.75 / 0.50.
//
// The numbers are free to contradict both. READ ONLY: the file is loaded, the
// silence is prepended and appended IN MEMORY, and nothing is written anywhere.
static juce::AudioBuffer<float> padWithSilence (const juce::AudioBuffer<float>& src,
                                                double fraction)
{
    // f of the RESULT is silence, split evenly before and after, so the test is
    // an intro and an outro rather than one or the other.
    const int n = src.getNumSamples();
    if (fraction <= 0.0) return src;
    const int total = (int) std::llround ((double) n / (1.0 - fraction));
    const int pad   = (total - n) / 2;
    juce::AudioBuffer<float> out (src.getNumChannels(), pad + n + pad);
    out.clear();
    for (int ch = 0; ch < src.getNumChannels(); ++ch)
        out.copyFrom (ch, pad, src, ch, 0, n);
    return out;
}

static int runSilence (const juce::File& f)
{
    std::cout << "=== THE SILENCE EXPERIMENT ===\n\n";
    std::cout << "  file: " << f.getFileName() << "   (READ ONLY, padded in memory only)\n\n";
    std::cout << "  PREDICTED, stated before the run:\n"
                 "    power mean : relatives EXACTLY unchanged at every fraction;\n"
                 "                 absolutes all shift together by 10*log10(1-f),\n"
                 "                 -0.46 / -1.25 / -3.01 dB at f = 0.10 / 0.25 / 0.50\n"
                 "    dB mean    : relatives scaled by (1-f), so 0.90 / 0.75 / 0.50\n"
                 "                 absolutes dragged toward the floor by f*(F - dB)\n"
                 "    the ballistic release is 150 ms, so blocks near the boundary are\n"
                 "    decaying rather than at the floor: the dB-mean compression should\n"
                 "    come in slightly LESS than (1-f) exactly.\n\n";

    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
    if (rd == nullptr) { std::cout << "  cannot read\n"; return 2; }
    juce::AudioBuffer<float> src ((int) juce::jmax (1u, rd->numChannels),
                                  (int) rd->lengthInSamples);
    rd->read (&src, 0, (int) rd->lengthInSamples, 0, true, true);
    const double sr = rd->sampleRate;

    const double fractions[4] { 0.0, 0.10, 0.25, 0.50 };
    std::array<float, 6> basePowRel {}, baseDbRel {}, baseEqRel {};
    std::array<float, 6> basePowAbs {};

    for (int fi = 0; fi < 4; ++fi)
    {
        const auto buf = padWithSilence (src, fractions[fi]);
        MacroRow m;
        if (! macroPassBuffer (buf, sr, f.getFileName(), m))
        { std::cout << "  pass failed\n"; return 1; }

        // Scheme 1: the ENGINE's power mean, through the shipped read-out.
        const auto powAbs = m.accumDb;
        const auto powRel = relsOf (powAbs);
        // Scheme 2: a dB mean of the same six bands (mean of macroBandDb).
        const auto dbRel  = relsOf (m.meanDb);
        // Scheme 3: the dB mean eqCurve actually computes, through the shipped
        // computeBands, which is what the tonal comparison reads today.
        const auto cb = echojay::computeBands (m.eqCurve);
        const float cbv[6] { cb.sub, cb.low, cb.lowMid, cb.mid, cb.highMid, cb.high };
        float cbMean = 0.0f; for (int i = 0; i < 6; ++i) cbMean += cbv[i];
        cbMean /= 6.0f;
        std::array<float, 6> eqRel {};
        for (int i = 0; i < 6; ++i) eqRel[(size_t) i] = cbv[i] - cbMean;

        if (fi == 0)
        { basePowRel = powRel; baseDbRel = dbRel; baseEqRel = eqRel; basePowAbs = powAbs; }

        std::cout << "  ---- silent fraction " << juce::String (fractions[fi], 2)
                  << "   (" << m.frames << " blocks, " << m.silentBlocks
                  << " all-floor, " << juce::String (m.seconds, 1) << " s) ----\n";
        std::cout << kBandHdr << "\n";
        printRow       ("  POWER mean ABS", powAbs);
        printRowSigned ("  POWER mean REL", powRel);
        printRow       ("  dB mean ABS (macro)", m.meanDb);
        printRowSigned ("  dB mean REL (macro)", dbRel);
        printRowSigned ("  dB mean REL (eqCurve bins)", eqRel);

        if (fi > 0)
        {
            printRowSigned ("  POWER ABS shift from f=0", diffOf (powAbs, basePowAbs));
            printRowSigned ("  POWER REL change from f=0", diffOf (powRel, basePowRel));
            // The compression ratio, band by band, for both dB schemes. Only
            // meaningful where the base relative is big enough to divide by.
            auto ratios = [] (const char* lbl, const std::array<float, 6>& now,
                              const std::array<float, 6>& base)
            {
                std::cout << "  " << std::left << std::setw (34) << lbl;
                for (int i = 0; i < 6; ++i)
                {
                    if (std::abs (base[(size_t) i]) < 0.5f)
                        std::cout << std::right << std::setw (9) << "  n/a";
                    else
                        std::cout << std::right << std::setw (9) << std::fixed
                                  << std::setprecision (3)
                                  << (now[(size_t) i] / base[(size_t) i]);
                }
                std::cout << "\n";
            };
            ratios ("  dB REL ratio (macro)",   dbRel, baseDbRel);
            ratios ("  dB REL ratio (eqCurve)", eqRel, baseEqRel);
            std::cout << "    predicted dB REL ratio at this fraction: "
                      << juce::String (1.0 - fractions[fi], 3) << "\n";
            std::cout << "    predicted POWER ABS shift: "
                      << juce::String (10.0 * std::log10 (1.0 - fractions[fi]), 3) << " dB\n";
        }
        std::cout << "\n"; std::cout.flush();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// THE SILENCE DISTORTION, ON REAL MATERIAL (Phase 1b commit 3)
// ---------------------------------------------------------------------------
// The synthetic experiment pads a file with DIGITAL silence, which floors the
// meter harder than anything real: a genuine intro has room tone, preamp hiss
// and a noise floor that still carries power. The library already contains real
// silence with real room tone in it, so this measures the distortion that is
// actually shipping rather than one built to demonstrate the mechanism.
//
// For every file: the eqCurve band relatives as they ship today, over EVERY
// block, against the same relatives over the AUDIBLE blocks only. The gap is
// what the silence is doing to the advice the model reads.
//
// AUDIBLE IS THE ENGINE'S OWN DEFINITION, not a new one: MeterData::isSilent,
// which is peak below kSilenceThreshold for 500 ms. Inventing a second
// definition of silence for the measurement would make it unattributable.
//
// The power-accumulated macro bands are measured on the same files in the same
// pass, because the prediction is that they barely move and a prediction that
// is not measured beside its alternative is not a comparison.
struct SilenceRow
{
    juce::String name;
    int   frames = 0, audible = 0;
    float silentFraction = 0.0f;
    std::array<float, 6> relAll {}, relAudible {}, relDiff {};
    std::array<float, 6> powRelAll {}, powRelAudible {}, powRelDiff {};
    bool  anySignChange = false, powSignChange = false;
    float worstAbs = 0.0f, powWorstAbs = 0.0f;
    bool  ok = false;
};

static bool silencePass (const juce::File& f, SilenceRow& out)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
    if (rd == nullptr) return false;
    const double sr = rd->sampleRate;
    const juce::int64 total = rd->lengthInSamples;
    const int nch = (int) rd->numChannels;
    if (sr <= 0.0 || total <= 0) return false;

    const int blockSize = 2048;
    MeterEngine engine;
    engine.prepare (sr, blockSize);

    std::array<double, 64> sumAll {}, sumAud {};
    sumAll.fill (0.0); sumAud.fill (0.0);
    std::array<double, 6> powAll {}, powAud {};
    powAll.fill (0.0); powAud.fill (0.0);
    int nAll = 0, nAud = 0;
    double prevPow[6] = {};

    juce::AudioBuffer<float> buf (juce::jmax (1, nch), blockSize);
    juce::int64 read = 0;
    while (read < total)
    {
        const int want = (int) juce::jmin ((juce::int64) blockSize, total - read);
        buf.clear();
        rd->read (&buf, 0, want, read, true, true);
        const float* L = buf.getReadPointer (0);
        const float* R = nch >= 2 ? buf.getReadPointer (1) : L;
        engine.processBlock (L, R, want);
        const auto md = engine.getMeterData();

        // The POWER accumulator is cumulative inside the engine, so the
        // per-block power is its increment. Read it as a delta rather than
        // re-deriving it, so both columns come from the shipped accumulation.
        const auto acc = engine.getAccumulatedBands();
        double curPow[6] = {};
        for (int i = 0; i < 6; ++i)
            curPow[i] = acc.valid ? std::pow (10.0, (double) acc.db[(size_t) i] / 10.0)
                                    * (double) acc.blocks : 0.0;

        for (int i = 0; i < 64; ++i) sumAll[(size_t) i] += (double) md.spectrum[(size_t) i];
        for (int i = 0; i < 6; ++i) powAll[(size_t) i] = curPow[i];
        ++nAll;
        if (! md.isSilent)
        {
            for (int i = 0; i < 64; ++i) sumAud[(size_t) i] += (double) md.spectrum[(size_t) i];
            for (int i = 0; i < 6; ++i) powAud[(size_t) i] += (curPow[i] - prevPow[i]);
            ++nAud;
        }
        for (int i = 0; i < 6; ++i) prevPow[i] = curPow[i];
        read += want;
    }
    if (nAll == 0 || nAud == 0) return false;

    out.name = f.getFileName();
    out.frames = nAll; out.audible = nAud;
    out.silentFraction = 1.0f - (float) nAud / (float) nAll;

    auto relsOfSpec = [] (const std::array<double, 64>& sum, int n)
    {
        std::array<float, 64> avg {};
        for (int i = 0; i < 64; ++i) avg[(size_t) i] = (float) (sum[(size_t) i] / (double) n);
        const auto b = echojay::computeBands (avg);
        const float v[6] { b.sub, b.low, b.lowMid, b.mid, b.highMid, b.high };
        float m = 0.0f; for (int i = 0; i < 6; ++i) m += v[i]; m /= 6.0f;
        std::array<float, 6> r {};
        for (int i = 0; i < 6; ++i) r[(size_t) i] = v[i] - m;
        return r;
    };
    out.relAll     = relsOfSpec (sumAll, nAll);
    out.relAudible = relsOfSpec (sumAud, nAud);

    auto relsOfPow = [] (const std::array<double, 6>& p, int n)
    {
        std::array<float, 6> db {};
        for (int i = 0; i < 6; ++i)
            db[(size_t) i] = echojay::bandMeanFromSum (p[(size_t) i], n).db;
        return relsOf (db);
    };
    out.powRelAll     = relsOfPow (powAll, nAll);
    out.powRelAudible = relsOfPow (powAud, nAud);

    for (int i = 0; i < 6; ++i)
    {
        out.relDiff[(size_t) i]    = out.relAudible[(size_t) i] - out.relAll[(size_t) i];
        out.powRelDiff[(size_t) i] = out.powRelAudible[(size_t) i] - out.powRelAll[(size_t) i];
        out.worstAbs    = juce::jmax (out.worstAbs,    std::abs (out.relDiff[(size_t) i]));
        out.powWorstAbs = juce::jmax (out.powWorstAbs, std::abs (out.powRelDiff[(size_t) i]));
        if ((out.relAll[(size_t) i] > 0.0f) != (out.relAudible[(size_t) i] > 0.0f))
            out.anySignChange = true;
        if ((out.powRelAll[(size_t) i] > 0.0f) != (out.powRelAudible[(size_t) i] > 0.0f))
            out.powSignChange = true;
    }
    out.ok = true;
    return true;
}

static int runSilenceReal (const juce::File& folder)
{
    std::cout << "=== THE SILENCE DISTORTION, ON REAL MATERIAL ===\n\n";
    std::cout << "  folder: " << folder.getFullPathName() << "  (READ ONLY)\n";
    std::cout << "  No synthetic silence anywhere. Audible = the engine's own\n"
                 "  MeterData::isSilent, peak below threshold for 500 ms.\n\n";

    juce::Array<juce::File> files;
    folder.findChildFiles (files, juce::File::findFiles, false);
    files.sort();

    std::vector<SilenceRow> rows;
    std::cout << "  " << std::left << std::setw (46) << "FILE" << std::right
              << std::setw (9) << "silent%" << std::setw (11) << "eqC worst"
              << std::setw (8) << "sign" << std::setw (11) << "pow worst"
              << std::setw (8) << "sign" << "\n";
    for (auto& f : files)
    {
        const auto ext = f.getFileExtension().toLowerCase();
        if (ext != ".wav" && ext != ".mp3" && ext != ".flac" && ext != ".aiff"
            && ext != ".aif" && ext != ".ogg" && ext != ".m4a") continue;
        SilenceRow r;
        if (! silencePass (f, r)) continue;
        rows.push_back (r);
        std::cout << "  " << std::left << std::setw (46)
                  << r.name.substring (0, 45).toStdString() << std::right
                  << std::setw (8) << std::fixed << std::setprecision (1)
                  << (r.silentFraction * 100.0f) << "%"
                  << std::setw (11) << std::setprecision (2) << r.worstAbs
                  << std::setw (8) << (r.anySignChange ? "YES" : "-")
                  << std::setw (11) << r.powWorstAbs
                  << std::setw (8) << (r.powSignChange ? "YES" : "-") << "\n";
        std::cout.flush();
    }
    if (rows.empty()) { std::cout << "  no files\n"; return 1; }

    std::vector<float> w, pw; int signs = 0, powSigns = 0;
    for (auto& r : rows)
    { w.push_back (r.worstAbs); pw.push_back (r.powWorstAbs);
      if (r.anySignChange) ++signs; if (r.powSignChange) ++powSigns; }
    std::sort (w.begin(), w.end()); std::sort (pw.begin(), pw.end());
    auto pct = [] (std::vector<float>& v, double p)
    { return v[(size_t) juce::jlimit (0, (int) v.size() - 1,
                                      (int) std::llround (p * (double) (v.size() - 1)))]; };

    std::cout << "\n  " << rows.size() << " files measured\n\n";
    std::cout << "  DISTRIBUTION of the largest per-file relative change, dB:\n";
    std::cout << "               min    p25    med    p75    p90    max\n";
    std::cout << "  eqCurve  " << std::setw (7) << std::setprecision (2) << pct (w, 0.0)
              << std::setw (7) << pct (w, 0.25) << std::setw (7) << pct (w, 0.5)
              << std::setw (7) << pct (w, 0.75) << std::setw (7) << pct (w, 0.90)
              << std::setw (7) << pct (w, 1.0) << "\n";
    std::cout << "  power    " << std::setw (7) << pct (pw, 0.0)
              << std::setw (7) << pct (pw, 0.25) << std::setw (7) << pct (pw, 0.5)
              << std::setw (7) << pct (pw, 0.75) << std::setw (7) << pct (pw, 0.90)
              << std::setw (7) << pct (pw, 1.0) << "\n";
    std::cout << "\n  files with ANY band changing sign:  eqCurve " << signs
              << " of " << rows.size() << ",  power " << powSigns
              << " of " << rows.size() << "\n";

    // Does the size of the change track the silent fraction? Pearson r, and the
    // ratio test: the clean model says rel_audible = rel_all / (1 - f), so the
    // change should scale with f/(1-f) times the size of the relative.
    double mx = 0, my = 0;
    for (auto& r : rows) { mx += r.silentFraction; my += r.worstAbs; }
    mx /= (double) rows.size(); my /= (double) rows.size();
    double sxy = 0, sxx = 0, syy = 0;
    for (auto& r : rows)
    { const double dx = r.silentFraction - mx, dy = r.worstAbs - my;
      sxy += dx*dy; sxx += dx*dx; syy += dy*dy; }
    const double rr = (sxx > 0 && syy > 0) ? sxy / std::sqrt (sxx * syy) : 0.0;
    std::cout << "\n  correlation between silent fraction and eqCurve change: r = "
              << juce::String (rr, 3) << "   (mean silent fraction "
              << juce::String (mx * 100.0, 1) << "%, mean change "
              << juce::String (my, 2) << " dB)\n";
    return 0;
}

static int runControlA()
{
    const double sr = 44100.0;
    const int blocks = 1292;                 // 60.0 s, an exact multiple of 2048
    const float TOL = 0.05f;

    std::cout << "=== CONTROL A: DETERMINISTIC MULTI-TONE. THIS ONE GATES. ===\n\n";
    std::cout << "  14 tones, all on exact FFT bin centres (multiples of "
              << juce::String (sr / 2048.0, 4) << " Hz), none near a band edge:\n"
                 "    sub     k=2                 43.07 Hz\n"
                 "    low     k=5,9              107.67, 193.80 Hz\n"
                 "    lowMid  k=15,20            323.00, 430.66 Hz\n"
                 "    mid     k=30,50,80         646.00, 1076.66, 1722.66 Hz\n"
                 "    highMid k=110,180,250     2368.65, 3875.98, 5383.30 Hz\n"
                 "    air     k=320,500,800     6890.62, 10766.60, 17226.56 Hz\n";
    std::cout << "  " << blocks << " blocks = " << juce::String (blocks * 2048 / sr, 3)
              << " s, an exact multiple of 2048 samples.\n\n";
    std::cout << "  TOLERANCE " << juce::String (TOL, 3) << " dB, STATED BEFORE THE RUN.\n"
                 "  Derived: on a perfectly periodic signal the only tail-vs-whole\n"
                 "  difference is the initial settling transient. From a -120 start to a\n"
                 "  -30 level the attack pole (coeff 0.990381 at 2048/44.1k) contributes a\n"
                 "  total deviation of 0.874 dB spread over " << blocks
              << " blocks = 0.0007 dB in the\n"
                 "  mean. float32 quantisation at -30 dB adds ~4e-6 dB per sample and the\n"
                 "  accumulators are double. The modelled bound is ~0.001 dB; the gate is\n"
                 "  set at 50x that to cover what is NOT modelled, not to pass.\n";

    MacroRow m;
    const auto buf = makeMultiTone (sr, blocks);
    if (! macroPassBuffer (buf, sr, "control-A-multitone", m))
    { std::cout << "  CONTROL A PASS FAILED TO RUN\n"; return 1; }

    reportMacro (m, true);

    float worstAbs = 0.0f, worstRel = 0.0f; int wa = -1, wr = -1;
    const auto rt = relsOf (m.tail), rw = relsOf (m.meanDb);
    for (int i = 0; i < 6; ++i)
    {
        const float a = std::abs (m.meanDb[(size_t) i] - m.tail[(size_t) i]);
        const float r = std::abs (rw[(size_t) i] - rt[(size_t) i]);
        if (a > worstAbs) { worstAbs = a; wa = i; }
        if (r > worstRel) { worstRel = r; wr = i; }
    }
    std::cout << "\n  worst ABSOLUTE disagreement: " << juce::String (worstAbs, 5)
              << " dB (" << echojay::macroBandName (wa) << ")\n";
    std::cout << "  worst RELATIVE disagreement: " << juce::String (worstRel, 5)
              << " dB (" << echojay::macroBandName (wr) << ")\n";
    const bool pass = (worstAbs <= TOL) && (worstRel <= TOL);
    std::cout << "  CONTROL A " << (pass ? "PASSES" : "FAILS") << " at "
              << juce::String (TOL, 3) << " dB\n";
    std::cout << "  (modelled bound was ~0.001 dB; measured/modelled = "
              << juce::String (worstAbs / 0.001f, 1) << "x)\n";
    if (! pass)
        std::cout << "\n  STOP. On a signal with ZERO frame-to-frame variance the tail and\n"
                     "  the whole file must agree. They do not, so the harness has a real\n"
                     "  defect and nothing measured after it is evidence.\n";
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// CONTROL B: STOCHASTIC, ACROSS SEEDS. THIS ONE MEASURES.
// ---------------------------------------------------------------------------
// The spread here is the number that should have been the threshold all along.
static juce::AudioBuffer<float> makePinkBuffer (juce::uint32 seed, double sr, int blocks);

static int runControlB (int seeds)
{
    const double sr = 44100.0;
    const int blocks = 1292;

    std::cout << "\n=== CONTROL B: PINK NOISE ACROSS " << seeds
              << " SEEDS. THIS ONE MEASURES, IT DOES NOT GATE. ===\n\n";
    std::cout << "  THE PREDICTION, WRITTEN BEFORE THE TABLE EXISTS. The hypothesis is\n"
                 "  estimator variance: a band averages over however many FFT bins fall\n"
                 "  inside it, so spread should fall as 1/sqrt(bins). Bins per band at\n"
                 "  44.1 kHz / 2048: sub 2, low 9, lowMid 12, mid 69, highMid 186, air 650.\n"
                 "  Normalised to air = 1.0 the predicted spreads are sub 18.0x, low 8.5x,\n"
                 "  lowMid 7.4x, mid 3.1x, highMid 1.9x, air 1.0x.\n"
                 "  SUB MUST BE THE LARGEST. In the single pink run it was the second\n"
                 "  SMALLEST at 0.19 dB. If the seeds put sub at the top, that run was\n"
                 "  lucky. If they do not, the hypothesis is WRONG and says so.\n\n";

    std::vector<std::array<double, 6>> absD, relD, tailV, wholeV, accumV, boundedV;
    int framesSeen = 0, boundedBlocksSeen = 0;
    float boundedSecondsSeen = 0.0f;
    for (int s = 0; s < seeds; ++s)
    {
        const auto buf = makePinkBuffer (0x1000001u + (juce::uint32) s * 2654435761u, sr, blocks);
        MacroRow m;
        if (! macroPassBuffer (buf, sr, "pink", m)) { std::cout << "  seed failed\n"; continue; }
        const auto rt = relsOf (m.tail), rw = relsOf (m.meanDb);
        std::array<double, 6> a {}, r {};
        for (int i = 0; i < 6; ++i)
        {
            a[(size_t) i] = (double) (m.meanDb[(size_t) i] - m.tail[(size_t) i]);
            r[(size_t) i] = (double) (rw[(size_t) i] - rt[(size_t) i]);
        }
        absD.push_back (a); relD.push_back (r);
        {
            std::array<double, 6> t {}, w {};
            for (int i = 0; i < 6; ++i)
            { t[(size_t) i] = m.tail[(size_t) i]; w[(size_t) i] = m.meanDb[(size_t) i]; }
            tailV.push_back (t); wholeV.push_back (w);
            // THE VALUE THAT NOW SHIPS. The pre-registered AFTER test is about
            // the spread of the REPORTED band figure, and since commit 4 that
            // is the power accumulation, not the tail and not the dB mean.
            std::array<double, 6> ac {};
            for (int i = 0; i < 6; ++i) ac[(size_t) i] = m.accumDb[(size_t) i];
            accumV.push_back (ac);
            std::array<double, 6> bd {};
            for (int i = 0; i < 6; ++i) bd[(size_t) i] = m.boundedDb[(size_t) i];
            boundedV.push_back (bd);
            boundedSecondsSeen = m.boundedSeconds;
            boundedBlocksSeen  = m.boundedBlocks;
            framesSeen = m.frames;
        }
        std::cout << "  seed " << std::setw (2) << s << "  abs";
        for (int i = 0; i < 6; ++i)
            std::cout << std::setw (8) << std::showpos << std::fixed << std::setprecision (2)
                      << a[(size_t) i] << std::noshowpos;
        std::cout << "\n"; std::cout.flush();
    }
    if (absD.empty()) return 1;

    const int bins[6] { 2, 9, 12, 69, 186, 650 };
    std::cout << "\n  PER BAND, over " << absD.size() << " seeds:\n";
    std::cout << "  " << std::left << std::setw (10) << "band" << std::right
              << std::setw (7) << "bins"
              << std::setw (11) << "mean ABS" << std::setw (11) << "sd ABS"
              << std::setw (11) << "max|ABS|"
              << std::setw (11) << "sd REL" << std::setw (11) << "max|REL|"
              << std::setw (11) << "sd/air\n";
    // TWO PASSES, because the ratio column divides by air's sd and air is the
    // LAST band: computing it inside the filling loop divided by a zero that
    // had not been written yet, and printed 0.00 for every band but air.
    std::array<double, 6> sdAbs {}, sdRel {}, meanA {}, meanR {}, maxA {}, maxR {};
    for (int i = 0; i < 6; ++i)
    {
        double mA = 0, mR = 0;
        for (auto& v : absD) mA += v[(size_t) i];
        for (auto& v : relD) mR += v[(size_t) i];
        mA /= (double) absD.size(); mR /= (double) relD.size();
        double vA = 0, vR = 0, xA = 0, xR = 0;
        for (auto& v : absD) { vA += (v[(size_t) i]-mA)*(v[(size_t) i]-mA); xA = std::max (xA, std::abs (v[(size_t) i])); }
        for (auto& v : relD) { vR += (v[(size_t) i]-mR)*(v[(size_t) i]-mR); xR = std::max (xR, std::abs (v[(size_t) i])); }
        sdAbs[(size_t) i] = std::sqrt (vA / (double) (absD.size() - 1));
        sdRel[(size_t) i] = std::sqrt (vR / (double) (relD.size() - 1));
        meanA[(size_t) i] = mA; meanR[(size_t) i] = mR;
        maxA[(size_t) i]  = xA; maxR[(size_t) i]  = xR;
    }
    for (int i = 0; i < 6; ++i)
    {
        std::cout << "  " << std::left << std::setw (10) << echojay::macroBandName (i)
                  << std::right << std::setw (7) << bins[i]
                  << std::setw (11) << std::fixed << std::setprecision (3) << meanA[(size_t) i]
                  << std::setw (11) << sdAbs[(size_t) i]
                  << std::setw (11) << maxA[(size_t) i]
                  << std::setw (11) << sdRel[(size_t) i]
                  << std::setw (11) << maxR[(size_t) i]
                  << std::setw (11) << std::setprecision (2)
                  << (sdAbs[5] > 0 ? sdAbs[(size_t) i] / sdAbs[5] : 0.0) << "\n";
    }
    std::cout << "\n  PREDICTED sd/air (1/sqrt(bins), air=1):  sub 18.0, low 8.5, lowMid 7.4,"
                 " mid 3.1, highMid 1.9, air 1.0\n";
    int biggest = 0;
    for (int i = 1; i < 6; ++i) if (sdAbs[(size_t) i] > sdAbs[(size_t) biggest]) biggest = i;
    // =====================================================================
    // THE AFTER TEST, DESIGNED AND WRITTEN DOWN BEFORE THE ACCUMULATION EXISTS
    // =====================================================================
    // Recorded here so it cannot be chosen to suit the result later.
    //
    // THE PRIMARY AFTER TEST. Once the accumulation lands, the figure the
    // plugin reports for a band stops being one ballistic reading and becomes a
    // mean over every frame of the window. If the frames were independent the
    // spread of that figure across seeds falls by sqrt(frames). Control B is
    // re-run with these same twelve seeds and the per-band spread of the
    // REPORTED value must fall by roughly that factor.
    //
    // THE DIRECTION OF CHANGE ON THE PAIR IS SECONDARY. It is one audio file
    // and one story about it; this is twelve independent draws and a number.
    {
        auto sdOf = [] (const std::vector<std::array<double, 6>>& v, int i)
        {
            double m = 0; for (auto& x : v) m += x[(size_t) i]; m /= (double) v.size();
            double q = 0; for (auto& x : v) q += (x[(size_t) i]-m)*(x[(size_t) i]-m);
            return std::sqrt (q / (double) (v.size() - 1));
        };
        const double f = std::sqrt ((double) framesSeen);
        std::cout << "\n  ===== THE AFTER TEST, RECORDED BEFORE THE ACCUMULATION EXISTS ====="
                  << "\n  Re-run control B with these same " << absD.size()
                  << " seeds after the accumulation lands."
                     "\n  The spread of the REPORTED band value must fall by about sqrt("
                  << framesSeen << ") = " << juce::String (f, 1) << "x.\n\n";
        std::cout << "  " << std::left << std::setw (10) << "band" << std::right
                  << std::setw (14) << "BEFORE sd" << std::setw (16) << "PREDICTED sd"
                  << std::setw (18) << "harness whole sd" << "\n";
        std::cout << "  " << std::left << std::setw (10) << " " << std::right
                  << std::setw (14) << "(the tail)" << std::setw (16) << "(tail/sqrt f)"
                  << std::setw (18) << "(achievable now)" << "\n";
        for (int i = 0; i < 6; ++i)
            std::cout << "  " << std::left << std::setw (10) << echojay::macroBandName (i)
                      << std::right << std::setw (14) << std::fixed << std::setprecision (3)
                      << sdOf (tailV, i)
                      << std::setw (16) << (sdOf (tailV, i) / f)
                      << std::setw (18) << sdOf (wholeV, i) << "\n";

        // ===== THE AFTER RESULT: the spread of the SHIPPED figure =====
        std::cout << "\n  AFTER: the spread of the value the comparisons now read\n";
        std::cout << "  " << std::left << std::setw (10) << "band" << std::right
                  << std::setw (14) << "BEFORE sd" << std::setw (14) << "AFTER sd"
                  << std::setw (12) << "fall" << std::setw (26) << "against 17x to 36x" << "\n";
        for (int i = 0; i < 6; ++i)
        {
            const double b = sdOf (tailV, i), a = sdOf (accumV, i);
            const double fall = (a > 0.0) ? b / a : 0.0;
            const bool inRange = (fall >= 17.0 && fall <= 36.0);
            std::cout << "  " << std::left << std::setw (10) << echojay::macroBandName (i)
                      << std::right << std::setw (14) << std::fixed << std::setprecision (3) << b
                      << std::setw (14) << a
                      << std::setw (11) << std::setprecision (1) << fall << "x"
                      << std::setw (26) << (inRange ? "IN RANGE"
                                          : (fall > 36.0 ? "ABOVE (better than asked)"
                                                         : "BELOW (short of the test)")) << "\n";
        }
        // ===== PHASE 1c: THE BOUNDED WINDOW, against its own pre-registered range
        std::cout << "\n  ===== PHASE 1c: THE LIVE SIDE'S BOUNDED WINDOW =====\n"
                  << "  Pre-registered before measuring: a fall of 8x to 17x per band,\n"
                  << "  from sqrt(300) = 17.3 discounted by the same 1.0x to 2.1x frame\n"
                  << "  correlation factor that made the whole-file expectation a range.\n"
                  << "  Window measured: " << boundedBlocksSeen << " blocks, "
                  << juce::String (boundedSecondsSeen, 2) << " s\n\n";
        std::cout << "  " << std::left << std::setw (10) << "band" << std::right
                  << std::setw (14) << "BALLISTIC sd" << std::setw (14) << "BOUNDED sd"
                  << std::setw (10) << "fall" << std::setw (22) << "against 8x to 17x" << "\n";
        for (int i = 0; i < 6; ++i)
        {
            const double b = sdOf (tailV, i), w = sdOf (boundedV, i);
            const double fall = (w > 0.0) ? b / w : 0.0;
            const bool inRange = (fall >= 8.0 && fall <= 17.0);
            std::cout << "  " << std::left << std::setw (10) << echojay::macroBandName (i)
                      << std::right << std::setw (14) << std::fixed << std::setprecision (3) << b
                      << std::setw (14) << w
                      << std::setw (9) << std::setprecision (1) << fall << "x"
                      << std::setw (22) << (inRange ? "IN RANGE"
                                          : (fall > 17.0 ? "ABOVE" : "BELOW")) << "\n";
        }

        std::cout << "\n  The third column is this harness computing the accumulation NOW.\n"
                     "  It is not the test: the test is whether the SHIPPED accumulation\n"
                     "  lands on the same figures. It is printed so the prediction and the\n"
                     "  achievable answer are both on the record before the code is written.\n";
    }

    std::cout << "  LARGEST MEASURED SPREAD: " << echojay::macroBandName (biggest)
              << ".  The hypothesis requires sub.  "
              << (biggest == 0 ? "IT IS SUB: the hypothesis survives and the 0.19 dB\n"
                                 "  single run was a lucky draw."
                               : "IT IS NOT SUB: the hypothesis is WRONG as stated,\n"
                                 "  and the single run was not the anomaly.") << "\n";
    return 0;
}

static int runControl()
{
    std::cout << "=== STEP 2. THE CONTROL, WHICH COMES FIRST ===\n\n";
    std::cout << "A steady-state file has no fade, so the tail and the whole-file figures\n"
                 "must agree. If they do not, the harness is measuring something other than\n"
                 "what it says and nothing after it means anything.\n\n";

    const auto f = writePinkControl (75.0);
    if (! f.existsAsFile()) { std::cout << "  COULD NOT WRITE THE CONTROL FILE\n"; return 1; }
    std::cout << "  synthesised: " << f.getFullPathName() << "  ("
              << juce::String (f.getSize() / 1048576.0, 1) << " MB, deterministic pink, 75 s)\n";
    std::cout << "  NOT in the library. System temp directory only.\n";

    MacroRow m;
    if (! macroPass (f, m)) { std::cout << "  CONTROL PASS FAILED\n"; return 1; }
    reportMacro (m, true);

    float worst = 0.0f; int worstBand = -1;
    for (int i = 0; i < 6; ++i)
    {
        const float d = std::abs (m.meanDb[(size_t) i] - m.tail[(size_t) i]);
        if (d > worst) { worst = d; worstBand = i; }
    }
    std::cout << "\n  largest tail-vs-whole disagreement: " << juce::String (worst, 3)
              << " dB, band " << echojay::macroBandName (worstBand) << "\n";

    const bool pass = worst <= 1.0f;
    std::cout << "  CONTROL " << (pass ? "PASSES" : "FAILS")
              << " (threshold ~1 dB per band)\n";
    if (! pass)
        std::cout << "\n  STOP. The harness disagrees with itself on material that has no\n"
                     "  fade, so it is not measuring the tail-versus-window difference it\n"
                     "  claims to. Nothing measured after this is evidence.\n";
    return pass ? 0 : 1;
}

static int runCensus (const juce::File& folder)
{
    std::cout << "=== STEP 1. THE CENSUS: EVERY REFERENCE, MEASURED ===\n\n";
    std::cout << "  folder: " << folder.getFullPathName() << "  (READ ONLY)\n";

    juce::Array<juce::File> files;
    folder.findChildFiles (files, juce::File::findFiles, false);
    files.sort();

    int live = 0, dead = 0, failed = 0;
    std::cout << "\n" << std::left << std::setw (52) << "  FILE"
              << std::right << std::setw (8) << "SECS"
              << std::setw (10) << "TAIL sub" << std::setw (10) << "WHOLE sub"
              << std::setw (10) << "TAIL air" << std::setw (10) << "WHOLE air"
              << "   TAIL\n";

    std::vector<MacroRow> rows;
    for (auto& f : files)
    {
        const auto ext = f.getFileExtension().toLowerCase();
        if (ext != ".wav" && ext != ".mp3" && ext != ".flac" && ext != ".aiff"
            && ext != ".aif" && ext != ".ogg" && ext != ".m4a") continue;

        MacroRow m;
        if (! macroPass (f, m))
        {
            std::cout << "  " << std::left << std::setw (50)
                      << f.getFileName().substring (0, 50).toStdString()
                      << "   COULD NOT READ\n";
            ++failed; continue;
        }
        const bool dead_ = tailIsDead (m.tail);
        dead_ ? ++dead : ++live;
        rows.push_back (m);

        std::cout << "  " << std::left << std::setw (50)
                  << m.name.substring (0, 50).toStdString()
                  << std::right << std::setw (8) << std::fixed << std::setprecision (1) << m.seconds
                  << std::setw (10) << std::setprecision (2) << m.tail[0]
                  << std::setw (10) << m.meanDb[0]
                  << std::setw (10) << m.tail[5]
                  << std::setw (10) << m.meanDb[5]
                  << "   " << (dead_ ? "DEAD" : "LIVE") << "\n";
        std::cout.flush();
    }

    std::cout << "\n  " << rows.size() << " readable, " << live << " LIVE tail, "
              << dead << " DEAD tail, " << failed << " unreadable\n";

    std::cout << "\n  FULL SIX-BAND DETAIL, every readable file:\n";
    for (auto& m : rows) reportMacro (m, false);
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc > 1 && juce::String (argv[1]) == "--control")
        return runControl();

    // CONTROL A GATES, CONTROL B ONLY MEASURES, so B runs only if A passed and
    // its numbers are never allowed to decide anything.
    if (argc > 1 && juce::String (argv[1]) == "--controls")
    {
        const int rc = runControlA();
        if (rc != 0) return rc;
        const int seeds = argc > 2 ? juce::String (argv[2]).getIntValue() : 12;
        return runControlB (juce::jmax (2, seeds));
    }
    if (argc > 1 && juce::String (argv[1]) == "--controlA") return runControlA();
    if (argc > 2 && juce::String (argv[1]) == "--silence-real")
    {
        const juce::File folder { juce::String (argv[2]) };
        if (! folder.isDirectory()) { std::cout << "not a folder\n"; return 2; }
        return runSilenceReal (folder);
    }
    if (argc > 2 && juce::String (argv[1]) == "--silence")
    {
        const juce::File one { juce::String (argv[2]) };
        if (! one.existsAsFile()) { std::cout << "no such file\n"; return 2; }
        return runSilence (one);
    }

    if (argc > 2 && juce::String (argv[1]) == "--census")
    {
        const juce::File folder { juce::String (argv[2]) };
        if (! folder.isDirectory()) { std::cout << "not a folder\n"; return 2; }
        return runCensus (folder);
    }

    if (argc > 2 && juce::String (argv[1]) == "--macro")
    {
        const juce::File one { juce::String (argv[2]) };
        MacroRow m;
        if (! one.existsAsFile() || ! macroPass (one, m)) { std::cout << "could not read\n"; return 2; }
        std::cout << kBandHdr << "\n";
        reportMacro (m, true);
        return 0;
    }

    const juce::File refFile (argc > 1 ? juce::String (argv[1]) : juce::String());
    const juce::File capFile (argc > 2 ? juce::String (argv[2]) : juce::String());
    if (! refFile.existsAsFile() || ! capFile.existsAsFile())
    {
        std::cout << "usage: measure <reference audio> <capture audio>\n"
                     "       measure --control\n"
                     "       measure --census <folder>\n"
                     "       measure --macro <file>\n";
        return 2;
    }

    std::cout << "SPECTRAL EVIDENCE MEASUREMENT (plan section 1.6)\n\n";
    std::cout << "  reference : " << refFile.getFileName() << "\n";
    std::cout << "  capture   : " << capFile.getFileName() << "\n\n";

    ReferenceResult ref, cap;
    std::cout << "analysing (whole file, every block):\n";
    if (! analyseOne (refFile, ref)) return 1;
    std::cout << "  reference " << juce::String (ref.durationSeconds, 1) << " s\n";
    if (! analyseOne (capFile, cap)) return 1;
    std::cout << "  capture   " << juce::String (cap.durationSeconds, 1) << " s\n\n";

    // The capture side, held constant across both rows.
    const auto capAvg = cap.eqCurve;

    const char* hdr = "                                          sub      low   lowMid      mid  highMid     high";
    std::cout << "BAND LEVELS, dB (each side on its own terms)\n" << hdr << "\n";
    printBands ("capture, whole-file average",  echojay::computeBands (capAvg));
    printBands ("reference, whole-file average", echojay::computeBands (ref.eqCurve));
    printBands ("reference, ballistic tail",     echojay::computeBands (ref.data.spectrum));

    const auto before = echojay::bandDeltas (capAvg, ref.data.spectrum);
    const auto after  = echojay::bandDeltas (capAvg, ref.eqCurve);

    std::cout << "\nSIX BAND DELTAS, dB relative (positive = the capture has more)\n" << hdr << "\n";
    printDeltas ("BEFORE  vs reference tail",    before);
    printDeltas ("AFTER   vs reference average", after);

    if (before.valid && after.valid)
    {
        echojay::BandDeltaSet change; change.valid = true;
        for (int i = 0; i < 6; ++i)
            change.delta[(size_t) i] = after.delta[(size_t) i] - before.delta[(size_t) i];
        printDeltas ("CHANGE  (after minus before)", change);

        int moved = 0; float worst = 0.0f;
        for (int i = 0; i < 6; ++i)
        {
            const float c = std::abs (change.delta[(size_t) i]);
            if (c >= 0.05f) ++moved;
            worst = juce::jmax (worst, c);
        }
        std::cout << "\n  bands that moved by >= 0.05 dB: " << moved << " of 6"
                  << ", largest move " << juce::String (worst, 2) << " dB\n";
        if (moved == 0)
            std::cout << "  THE NUMBERS DID NOT MOVE. The change did not reach this path.\n";

        // The direction argument, checked rather than asserted: a fade out is
        // quieter and duller than the whole-file average, so the old comparison
        // overstated how much more of everything the capture had.
        float sumBefore = 0.0f, sumAfter = 0.0f;
        for (int i = 0; i < 6; ++i) { sumBefore += before.delta[(size_t) i];
                                      sumAfter  += after.delta[(size_t) i]; }
        std::cout << "  sum of deltas: before " << juce::String (sumBefore, 2)
                  << ", after " << juce::String (sumAfter, 2) << "\n";
    }

    // =======================================================================
    // PHASE 1b: THE MACRO BANDS FOR THIS REFERENCE, AND THE HARNESS'S OWN
    // FIDELITY CHECK.
    // =======================================================================
    MacroRow m;
    if (! macroPass (refFile, m)) { std::cout << "\nmacro pass failed\n"; return 1; }

    // THE LOOP IS THE LOOP IT CLAIMS TO BE, checked rather than asserted. This
    // harness runs its own block loop; if that loop differs from
    // ReferenceAnalyser's in block size, ordering or what it samples, its
    // whole-file figures are not the ones the accumulation will produce and
    // BEFORE and AFTER stop being comparable. The eqCurve both produce is the
    // observable that would move.
    {
        // THE THRESHOLD IS DERIVED, NOT PICKED. The first version used a flat
        // 0.0001 dB and fired on both references, which was the check being
        // wrong rather than the loop: ReferenceAnalyser.cpp:137 accumulates into
        // std::array<float, 64>, this loop accumulates into double, so the two
        // are NOT bit-identical and never can be.
        //
        // Bound it. float32 half-ulp rounding is eps = 2^-24 = 5.96e-8 relative.
        // Each addition rounds the running sum, whose magnitude is bounded by
        // frames * 120 because the spectrum floor is -120 dB. Worst case over
        // `frames` additions, then divided by `frames`:
        //
        //     error <= frames * (eps * frames * 120) / frames = eps * 120 * frames
        //
        // With signs random rather than adversarial the expectation is
        // eps * |v| * sqrt(frames^3 / 3) / frames, which for a typical |v| of 50
        // dB and 3279 frames is 9.9e-5 dB, and the two runs measured 1.1e-4 and
        // 1.3e-4. The MEASUREMENT sits where the random-walk estimate says, and
        // the gate is set at the rigorous worst case, because a check must not
        // fire on arithmetic that is provably correct.
        const double eps = std::pow (2.0, -24);
        const float  tol = (float) (eps * 120.0 * (double) m.frames);
        int differ = 0; float worst = 0.0f;
        for (int i = 0; i < 64; ++i)
        {
            const float d = std::abs (m.eqCurve[(size_t) i] - ref.eqCurve[(size_t) i]);
            if (d > tol) ++differ;
            worst = juce::jmax (worst, d);
        }
        std::cout << "\nHARNESS FIDELITY: this loop's eqCurve vs the shipped "
                     "ReferenceAnalyser's\n"
                  << "  threshold " << juce::String (tol, 6)
                  << " dB = eps(2^-24) x 120 dB x " << m.frames << " frames, the worst-case\n"
                     "  float32 accumulation error for THIS file's frame count\n"
                  << "  bins differing by more than that: " << differ << " of 64, worst "
                  << juce::String (worst, 6) << " dB\n"
                  << (differ == 0
                        ? "  The loop matches to within float32's own precision. Its\n"
                          "  macro-band figures are the ones the accumulation will produce.\n"
                          "  NOT bit-identical: the shipped accumulator is float, this one\n"
                          "  is double, and that difference is what the bound describes.\n"
                        : "  THE LOOP DOES NOT MATCH, by more than float32 rounding can\n"
                          "  explain. Its whole-file figures are not ReferenceAnalyser's,\n"
                          "  so BEFORE and AFTER are not comparable.\n");
    }

    std::cout << "\nMACRO BANDS, the BEFORE numbers\n";
    reportMacro (m, true);

    // =======================================================================
    // STEP 5. THE FALSIFIERS, each reported as held or not held.
    // =======================================================================
    {
        const auto relTail  = relsOf (m.tail);
        const auto relWhole = relsOf (m.meanDb);
        const auto dAbs     = diffOf (m.meanDb, m.tail);

        std::cout << "\nFALSIFIERS\n";

        bool allSmall = true;
        for (int i = 0; i < 6; ++i) if (std::abs (dAbs[(size_t) i]) >= 0.5f) allSmall = false;
        std::cout << "  1. all six absolute differences within 0.5 dB : "
                  << (allSmall ? "HOLDS  <-- falsifier fired" : "does not hold") << "\n";

        // FALSIFIER 2, REWRITTEN. The first version tested the ABSOLUTE
        // differences while the prediction it guards is about RELATIVES, so it
        // fired on both references for a reason that had nothing to do with the
        // prediction being wrong. A falsifier that measures a different quantity
        // from the claim it is meant to falsify cannot falsify it. It now tests
        // the same quantity the prediction does.
        const auto dRel = diffOf (relWhole, relTail);
        const float rSub = std::abs (dRel[0]), rAir = std::abs (dRel[5]), rMid = std::abs (dRel[3]);
        const bool subOrAirSmaller = (rSub < rMid) || (rAir < rMid);
        std::cout << "  2. sub or air moving LESS than mid, IN RELATIVES : "
                  << (subOrAirSmaller ? "HOLDS  <-- falsifier fired" : "does not hold")
                  << "\n     (rel moves: sub " << juce::String (rSub, 2)
                  << ", mid " << juce::String (rMid, 2)
                  << ", air " << juce::String (rAir, 2) << " dB)"
                  << "\n     [rewritten: was testing absolutes against a prediction about "
                     "relatives]\n";

        // FALSIFIER 3, REWRITTEN. The first version asked whether a relative
        // changed sign between the tail and the whole file. On a dead tail every
        // relative is exactly zero by construction, so the test reduced to "is
        // the whole-file relative positive" and could not fail for any reason
        // connected to the audio. It is replaced with a test that CAN fail and
        // that measures something decision 8 will actually be judged on: do the
        // two band schemes tell the same story about the same file?
        const auto cb0 = echojay::computeBands (m.eqCurve);
        const float cbv0[6] { cb0.sub, cb0.low, cb0.lowMid, cb0.mid, cb0.highMid, cb0.high };
        float cbMean0 = 0.0f; for (int i = 0; i < 6; ++i) cbMean0 += cbv0[i];
        cbMean0 /= 6.0f;
        int schemeDisagree = 0; juce::String which;
        for (int i = 0; i < 6; ++i)
        {
            const float binRel = cbv0[i] - cbMean0;
            if ((relWhole[(size_t) i] > 0.0f) != (binRel > 0.0f))
            { ++schemeDisagree; which += juce::String (which.isEmpty() ? "" : ", ")
                                       + echojay::macroBandName (i); }
        }
        std::cout << "  3. the two SCHEMES disagree in SIGN on this file : "
                  << schemeDisagree << " of 6"
                  << (schemeDisagree > 0 ? "  <-- falsifier fired (" + which + ")" : "")
                  << "\n     [rewritten: the old test was undefined when every tail relative "
                     "is zero]\n";

        // 4. The two schemes, against each other. computeBands works on the
        //    64-bin display spectrum with its own edges AND its display tilt;
        //    the macro bands are untilted and per-octave normalised, so they
        //    are NOT expected to agree. The check is that the disagreement is
        //    the known structural one and not something new.
        const auto cb = echojay::computeBands (m.eqCurve);
        const float cbv[6] { cb.sub, cb.low, cb.lowMid, cb.mid, cb.highMid, cb.high };
        float cbMean = 0.0f; for (int i = 0; i < 6; ++i) cbMean += cbv[i];
        cbMean /= 6.0f;
        std::cout << "  4. whole-file macro RELATIVES vs computeBands(eqCurve) RELATIVES:\n"
                  << kBandHdr << "\n";
        std::array<float, 6> cbRel {}, gap {};
        for (int i = 0; i < 6; ++i) cbRel[(size_t) i] = cbv[i] - cbMean;
        for (int i = 0; i < 6; ++i) gap[(size_t) i] = relWhole[(size_t) i] - cbRel[(size_t) i];
        printRowSigned ("  macro whole REL", relWhole);
        printRowSigned ("  computeBands(eqCurve) REL", cbRel);
        printRowSigned ("  GAP (macro - bins)", gap);
        std::cout << "     The two schemes differ by construction: different edges at three\n"
                     "     of six boundaries, and the bin scheme carries the display tilt\n"
                     "     while the macro scheme does not. This row is the size of that\n"
                     "     known difference, recorded so the unification can be judged\n"
                     "     against it rather than against nothing.\n";
    }
    return 0;
}
