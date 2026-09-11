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

  READ ONLY. It opens the two audio files for reading and writes nothing
  anywhere. The files under ~/Documents/EchoJay/References and
  ~/Library/EchoJay/codec_cache belong to the user and the cache is live.
*/

#include <JuceHeader.h>
#include "ReferenceAnalyser.h"
#include "EJSpectralEvidence.h"
#include <iostream>
#include <iomanip>

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

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File refFile (argc > 1 ? juce::String (argv[1]) : juce::String());
    const juce::File capFile (argc > 2 ? juce::String (argv[2]) : juce::String());
    if (! refFile.existsAsFile() || ! capFile.existsAsFile())
    {
        std::cout << "usage: measure <reference audio> <capture audio>\n";
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
    return 0;
}
