// track_level_guard - COMP_PROFILE_SPEC_v1 section 5, the level the plugin sends with a build.
//
//   "track_level": { "loud_rms_dbfs": -18.4, "loud_peak_dbfs": -6.2, "window": "400ms_p95", "heard_s": 90 }
//
// The 95th percentile of 400 ms RMS over what has been HEARD, on the track PRE-CHAIN, plus the loud-phrase peak;
// null under 20 s heard. Driven on SYNTHETIC signals whose levels this file chose, so every expected number is
// arithmetic rather than a reading taken from the thing under test.
//
// WHY THIS IS THE ONE NUMBER THAT MATTERS: the server subtracts it from the profile's eff_threshold_dbfs to pick
// the amount position (spec section 6, T = L - g/(1 - 1/R) + knee/4). An error of 1 dB here is an error of about
// 1 dB in the threshold, which at 4:1 is 0.75 dB of gain reduction - most of the 1.0 dB the acceptance test in
// section 8 allows. So it is asserted to 0.5 dB, not to "about right".

#include <JuceHeader.h>
#include "EJTrackLevel.h"
#include "ChainHost.h"   // 2 Oct 2026: the parked-rack leg drives the real tap through ChainHost::process
#include <cstdio>
#include <cmath>
#include <vector>

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::String f1 (float v) { return juce::String (v, 2); }

constexpr double kSr = 48000.0;

/** A sine at an exact RMS dBFS. A sine of amplitude a has RMS a/sqrt(2), so the amplitude for a wanted RMS in
    dBFS is 10^(db/20) * sqrt(2). Stated here because the whole guard rests on it. */
struct Phrase { double seconds; float rmsDbfs; };

/** Feed a list of phrases through the accumulator, 512 samples at a time, as the audio thread would. */
void feed (echojay::TrackLevel& tl, const std::vector<Phrase>& phrases, float* peakOfLoudestOut = nullptr,
           float loudestDb = -1000.0f)
{
    double phase = 0.0;
    const double inc = 2.0 * juce::MathConstants<double>::pi * 1000.0 / kSr;
    std::vector<float> buf ((size_t) 512);
    float peakOfLoudest = 0.0f;
    for (const auto& p : phrases)
    {
        const float amp = p.rmsDbfs <= -200.0f ? 0.0f
                        : juce::Decibels::decibelsToGain (p.rmsDbfs) * std::sqrt (2.0f);
        const int total = (int) std::lround (p.seconds * kSr);
        for (int done = 0; done < total; )
        {
            const int n = juce::jmin (512, total - done);
            for (int i = 0; i < n; ++i) { buf[(size_t) i] = amp * (float) std::sin (phase); phase += inc; }
            tl.push (buf.data(), nullptr, n);
            done += n;
        }
        if (p.rmsDbfs >= loudestDb) peakOfLoudest = juce::jmax (peakOfLoudest, amp);
    }
    if (peakOfLoudestOut != nullptr) *peakOfLoudestOut = peakOfLoudest;
}

/** A take: `loudFraction` of its windows at `loudDb`, the rest at `quietDb`, in 1 s alternating phrases. */
std::vector<Phrase> take (double seconds, float loudDb, float quietDb, double loudFraction)
{
    std::vector<Phrase> out;
    const int bars = (int) std::lround (seconds);
    const int loudBars = juce::jmax (1, (int) std::lround ((double) bars * loudFraction));
    const int every = juce::jmax (1, bars / loudBars);
    for (int b = 0; b < bars; ++b)
        out.push_back ({ 1.0, (b % every == 0) ? loudDb : quietDb });
    return out;
}

void guardMain()
{
    std::printf ("== track_level_guard: COMP_PROFILE_SPEC_v1 section 5 ==\n");

    std::printf ("\n-- (1) THREE seconds is an answer, and silence never is (2 Oct 2026 ruling) --\n");
    {
        // SUPERSEDED EXPECTATION, RE-RULED 2 Oct 2026. This leg asserted that 10 s of audio gives NO reading -
        // the spec's old "under 20 s heard: send null". Sean withdrew the 20 s floor: it meant a build on a track
        // that had played a few seconds sent no level at all, so the server computed no threshold and fell back,
        // on exactly the turns someone is auditioning a short phrase. The floor is 3 s of audio ABOVE THE GATE,
        // heard_s still rides the payload so the server can weigh it, and null is reserved for the one case that
        // really has no answer: nothing above the gate was ever heard.
        echojay::TrackLevel tl; tl.prepare (kSr);
        feed (tl, take (10.0, -12.0f, -30.0f, 0.25));
        const auto r = tl.read();
        check (r.valid,
               "(1) 10 s of audio DOES give a reading  (RED as it stood: the 20 s floor returned null here, and "
               "the server fell back to no threshold at all)",
               "heard " + f1 (r.heardSeconds) + " s, valid=" + (r.valid ? "y" : "n"));
        check (! tl.toVar().isVoid(),
               "(1) ...and the wire field is an object, not null",
               tl.toVar().isVoid() ? juce::String ("void") : juce::JSON::toString (tl.toVar()));
        check (std::abs (r.heardSeconds - 10.0f) < 0.6f,
               "(1) ...and heard_s still reports the audio it actually got, so the server can say how much it was "
               "set from", f1 (r.heardSeconds) + " s of 10.0");
    }

    std::printf ("\n-- (1b) 6 s of vocal produces a reading; SILENCE ALONE produces null --\n");
    {
        // Sean's two RED tests, together, because they are the same rule from both sides.
        echojay::TrackLevel six; six.prepare (kSr);
        feed (six, take (6.0, -14.0f, -32.0f, 0.3));
        const auto r6 = six.read();
        check (r6.valid && std::abs (r6.loudRmsDbfs + 14.0f) < 1.0f,
               "(1b) 6 s of vocal gives a reading, at the level it was fed",
               "valid=" + juce::String (r6.valid ? "y" : "n") + " rms " + f1 (r6.loudRmsDbfs)
                   + " heard " + f1 (r6.heardSeconds) + " s");
        // SILENCE: forty seconds of it, far past both the 3 s floor and the settle window, so this cannot pass by
        // simply not having waited long enough. Below the gate is not programme: it never enters the histogram and
        // never advances the heard clock, so there is nothing to report and the field is null.
        echojay::TrackLevel quiet; quiet.prepare (kSr);
        feed (quiet, { Phrase { 40.0, -200.0f } });
        const auto rq = quiet.read();
        check (! rq.valid && rq.heardSeconds <= 0.0f,
               "(1b) ...while 40 s of SILENCE gives null - below the gate is not programme, and no amount of it "
               "becomes one",
               "valid=" + juce::String (rq.valid ? "y" : "n") + " heard " + f1 (rq.heardSeconds) + " s");
        check (quiet.toVar().isVoid(),
               "(1b) ...and that one really is null on the wire",
               quiet.toVar().isVoid() ? juce::String ("void") : juce::JSON::toString (quiet.toVar()));
    }

    std::printf ("\n-- (1c) a clip SHORTER than 3 s, once it has played through --\n");
    {
        // "If the clip is shorter, send what was heard once it has played through." Measured on the SAMPLE clock -
        // audio that stopped arriving - so a guard can drive it and it cannot drift with machine load.
        echojay::TrackLevel tl; tl.prepare (kSr);
        feed (tl, take (1.6, -11.0f, -40.0f, 0.1));          // under the floor
        const auto mid = tl.read();
        check (! mid.valid,
               "(1c) 1.6 s and still arriving: no answer yet - the floor is not abandoned, only bounded",
               "valid=" + juce::String (mid.valid ? "y" : "n") + " heard " + f1 (mid.heardSeconds) + " s");
        feed (tl, { Phrase { 2.5, -200.0f } });              // the clip ended: silence, past kSettledAfterS
        const auto done = tl.read();
        check (done.valid && std::abs (done.loudRmsDbfs + 11.0f) < 1.5f,
               "(1c) ...and once it has played through, what was heard IS the answer  (RED as it stood: a clip "
               "shorter than the floor waited for audio that was never coming)",
               "valid=" + juce::String (done.valid ? "y" : "n") + " rms " + f1 (done.loudRmsDbfs)
                   + " heard " + f1 (done.heardSeconds) + " s");
        check (std::abs (done.heardSeconds - 1.6f) < 0.6f,
               "(1c) ...and heard_s says how little it was: the server is told, not misled",
               f1 (done.heardSeconds) + " s");
    }

    std::printf ("\n-- (2) the 95th percentile IS the loud phrases --\n");
    {
        // 40 s, a quarter of it at -12 dBFS RMS and the rest at -30. The top 5% of windows are loud, so the
        // percentile must land in the loud band - that is the whole point of using one.
        echojay::TrackLevel tl; tl.prepare (kSr);
        float loudAmp = 0.0f;
        feed (tl, take (40.0, -12.0f, -30.0f, 0.25), &loudAmp, -12.0f);
        const auto r = tl.read();
        check (r.valid, "(2) 40 s of audio gives a reading", "heard " + f1 (r.heardSeconds) + " s");
        check (std::abs (r.loudRmsDbfs - -12.0f) <= 0.5f,
               "(2) loud_rms_dbfs is the LOUD level, -12.0 dBFS, within half a dB  (RED as it stood: nothing "
               "measured it; a mean over the take would have read about -22)",
               f1 (r.loudRmsDbfs) + " dBFS, wanted -12.0");
        const float wantPeak = juce::Decibels::gainToDecibels (loudAmp);
        check (std::abs (r.loudPeakDbfs - wantPeak) <= 0.5f,
               "(2) ...and loud_peak_dbfs is the peak OF THOSE PHRASES, a sine's 3.01 dB above its RMS",
               f1 (r.loudPeakDbfs) + " dBFS, wanted " + f1 (wantPeak));
        check (std::abs ((r.loudPeakDbfs - r.loudRmsDbfs) - 3.01f) < 0.6f,
               "(2) ...so the crest of a sine comes out at 3.0 dB, which is the arithmetic and not a coincidence",
               f1 (r.loudPeakDbfs - r.loudRmsDbfs) + " dB of crest");
    }

    std::printf ("\n-- (3) a take that is mostly silence is not dragged down --\n");
    {
        // 10% loud. A MEAN would read far below the loud phrases; the percentile must not.
        echojay::TrackLevel tl; tl.prepare (kSr);
        feed (tl, take (60.0, -10.0f, -34.0f, 0.10));
        const auto r = tl.read();
        check (r.valid && std::abs (r.loudRmsDbfs - -10.0f) <= 0.6f,
               "(3) with only a tenth of the take loud, the reading is still the loud level",
               f1 (r.loudRmsDbfs) + " dBFS, wanted -10.0");
    }

    std::printf ("\n-- (4) silence is not programme --\n");
    {
        echojay::TrackLevel tl; tl.prepare (kSr);
        feed (tl, { { 60.0, -1000.0f } });          // digital black, transport rolling
        const auto r = tl.read();
        check (! r.valid && r.heardSeconds < 0.5f,
               "(4) sixty seconds of digital black is heard for 0 s and gives no reading  (a floor is not a "
               "measurement - the same rule the Listen gate uses)",
               "heard " + f1 (r.heardSeconds) + " s, windows " + juce::String (r.windows));
        echojay::TrackLevel t2; t2.prepare (kSr);
        feed (t2, { { 30.0, -80.0f }, { 30.0, -12.0f } });
        const auto r2 = t2.read();
        check (r2.valid && std::abs (r2.loudRmsDbfs - -12.0f) <= 0.5f && r2.heardSeconds < 35.0f,
               "(4) ...and material under the gate is excluded from the heard time as well as the statistic, so a "
               "long quiet head does not buy the 20 s",
               f1 (r2.loudRmsDbfs) + " dBFS over " + f1 (r2.heardSeconds) + " s heard");
    }

    std::printf ("\n-- (5) the wire object is exactly the spec's shape --\n");
    {
        echojay::TrackLevel tl; tl.prepare (kSr);
        feed (tl, take (90.0, -18.4f, -32.0f, 0.3));
        const auto v = tl.toVar();
        auto* o = v.getDynamicObject();
        check (o != nullptr, "(5) it is an object");
        if (o != nullptr)
        {
            check (o->hasProperty ("loud_rms_dbfs") && o->hasProperty ("loud_peak_dbfs")
                       && o->hasProperty ("window") && o->hasProperty ("heard_s"),
                   "(5) with the four fields the spec names, and no others",
                   juce::JSON::toString (v));
            // THE SPEC'S STRING, VERBATIM (COMP_PROFILE_SPEC_v1 section 5): "400ms_p95". This leg pinned
            // "400ms_rms_p95" - the v1 label, which named the window after the RMS figure and stopped being true
            // at v1.3, when loud_peak_dbfs was defined over THE SAME windows. Both the product and this leg
            // carried the stale string, so the leg agreed with the bug and could never catch it; the server's own
            // generated request is what disagreed. A leg is not evidence when it was written from the same
            // misreading as the code it checks.
            check (o->getProperty ("window").toString() == "400ms_p95",
                   "(5) window is the spec's label verbatim, and names the window both figures share",
                   o->getProperty ("window").toString());
            check (std::abs ((float) (double) o->getProperty ("loud_rms_dbfs") - -18.4f) <= 0.5f,
                   "(5) ...and the number is the spec's own example value for a take at that level",
                   juce::String ((double) o->getProperty ("loud_rms_dbfs")));
            check ((int) o->getProperty ("heard_s") >= 20,
                   "(5) ...and heard_s is whole seconds",
                   juce::String ((int) o->getProperty ("heard_s")));
        }
    }

    std::printf ("\n-- (6) 400 ms is the window, and the percentile is over WINDOWS --\n");
    {
        echojay::TrackLevel tl; tl.prepare (kSr);
        feed (tl, { { 4.0, -20.0f } });
        const auto r = tl.read();
        check (r.windows == 10,
               "(6) four seconds is ten 400 ms windows, so the window length is what the spec says",
               juce::String (r.windows) + " window(s)");
    }

    std::printf ("\n-- (7) THE CONVENTION, pinned by a test (spec v1.2 section 5) --\n");
    {
        // "as plain RMS: 20*log10(rms of the samples), where a full-scale sine reads -3.01 dBFS. NOT the AES17
        // convention (full-scale sine = 0 dBFS) ... A 3 dB slip here is larger than the whole GR target, so both
        // sides carry a test: a full-scale 997 Hz sine must read -3.01."
        echojay::TrackLevel tl; tl.prepare (kSr);
        // A FULL-SCALE 997 Hz SINE: amplitude 1.0, so its RMS is 1/sqrt(2) and 20*log10 of that is -3.0103.
        std::vector<float> buf ((size_t) 512);
        double phase = 0.0;
        const double inc = 2.0 * juce::MathConstants<double>::pi * 997.0 / kSr;
        const int total = (int) std::lround (40.0 * kSr);
        for (int done = 0; done < total; )
        {
            const int n = juce::jmin (512, total - done);
            for (int i = 0; i < n; ++i) { buf[(size_t) i] = (float) std::sin (phase); phase += inc; }
            tl.push (buf.data(), nullptr, n);
            done += n;
        }
        const auto r = tl.read();
        check (r.valid, "(7) precondition: 40 s of tone gives a reading", "heard " + f1 (r.heardSeconds) + " s");
        check (std::abs (r.loudRmsDbfs - -3.01f) <= 0.05f,
               "(7) a FULL-SCALE 997 Hz sine reads -3.01 dBFS - plain RMS, not AES17  (RED as it stood: the "
               "percentile reported its histogram BIN's lower edge, 0.25 dB wide, which answered -3.00; a 3 dB "
               "slip here would be larger than the whole gain-reduction target)",
               juce::String (r.loudRmsDbfs, 3) + " dBFS, wanted -3.010");
        check (std::abs (r.loudPeakDbfs - 0.0f) <= 0.1f,
               "(7) ...and its PEAK reads 0.0 dBFS, so the pair differ by exactly the sine's 3.01",
               juce::String (r.loudPeakDbfs, 2) + " dBFS peak");
        const auto v = tl.toVar();
        check (v.isObject()
                   && std::abs ((float) (double) v.getProperty ("loud_rms_dbfs", juce::var()) - -3.01f) <= 0.05f,
               "(7) ...and the WIRE carries it to two decimals, because one cannot express -3.01",
               juce::JSON::toString (v.getProperty ("loud_rms_dbfs", juce::var())));
    }

    std::printf ("\n-- (8) loud_peak_dbfs, DEFINED (spec v1.3 section 5) --\n");
    {
        // "over the same 400 ms windows, the maximum absolute sample value in each window (no oversampling), then
        // the 95th percentile of those across what was heard."
        //
        // THE OLD AND NEW DEFINITIONS GIVE DIFFERENT ANSWERS, and this leg is built on that difference. A take of
        // ordinary phrases at a known peak, plus ONE window carrying a single full-scale sample - a click. The old
        // reading was the largest peak among the loud windows, so the click set it and it read 0.0 dBFS. A
        // percentile discards the top 5% of windows, so one click cannot set it: the answer is the ordinary
        // phrases' peak. That is the whole point of defining it as a percentile.
        echojay::TrackLevel tl; tl.prepare (kSr);
        const float phraseRms = -12.0f;                    // a sine, so its peak is 3.01 dB above this
        const float wantPeak = phraseRms + 3.01f;
        // 60 one-second phrases at a known level...
        std::vector<float> buf ((size_t) 512);
        double phase = 0.0;
        const double inc = 2.0 * juce::MathConstants<double>::pi * 1000.0 / kSr;
        const float amp = juce::Decibels::decibelsToGain (phraseRms) * std::sqrt (2.0f);
        const int oneSec = (int) std::lround (kSr);
        for (int sec = 0; sec < 60; ++sec)
        {
            for (int done = 0; done < oneSec; )
            {
                const int n = juce::jmin (512, oneSec - done);
                for (int i = 0; i < n; ++i) { buf[(size_t) i] = amp * (float) std::sin (phase); phase += inc; }
                // ...and in ONE second, a single full-scale sample: a click, in one 400 ms window of 150.
                if (sec == 30 && done == 0) buf[0] = 1.0f;
                tl.push (buf.data(), nullptr, n);
                done += n;
            }
        }
        const auto r = tl.read();
        check (r.valid && r.windows >= 140,
               "(8) precondition: 60 s is ~150 windows, one of which carries a full-scale click",
               juce::String (r.windows) + " window(s)");
        check (std::abs (r.loudPeakDbfs - wantPeak) <= 0.2f,
               "(8) loud_peak_dbfs is the 95th percentile of the PER-WINDOW peaks, so a single click in one window "
               "cannot set it  (RED as it stood: it was the largest peak among the loud windows, so the click set "
               "it and it read 0.0 dBFS)",
               juce::String (r.loudPeakDbfs, 2) + " dBFS, wanted " + juce::String (wantPeak, 2));
        check (r.loudPeakDbfs < -5.0f,
               "(8) ...and it is nowhere near the click's 0.0 dBFS", juce::String (r.loudPeakDbfs, 2) + " dBFS");
        check (std::abs ((r.loudPeakDbfs - r.loudRmsDbfs) - 3.01f) <= 0.25f,
               "(8) ...while on a sine the pair still differ by the sine's 3.01, because both are percentiles over "
               "the SAME windows",
               juce::String (r.loudPeakDbfs - r.loudRmsDbfs, 2) + " dB apart");
        // ...and with NO click the two definitions agree, so the change is about the outlier and nothing else.
        echojay::TrackLevel clean; clean.prepare (kSr);
        feed (clean, take (60.0, phraseRms, phraseRms, 1.0));
        const auto rc = clean.read();
        check (std::abs (rc.loudPeakDbfs - wantPeak) <= 0.2f,
               "(8) ...and on a take with no outlier it is simply the peak of the material",
               juce::String (rc.loudPeakDbfs, 2) + " dBFS, wanted " + juce::String (wantPeak, 2));
    }

    // ---- THE READING EXISTS WITH NO SLOTS IN THE RACK (2 Oct 2026, Sean's finding 2) ----------------------
    // The Link publishes this reading for V2 to send on a Link-rack build, and while V2 HOLDS the rack the Link's
    // own chain is PARKED at 0 slots (RACK_BORROW_IMPLEMENTATION_SPEC §1/§2 - V2 owns the instances). So the
    // measurement has to be independent of what the rack contains, or the publish is dead code on exactly the
    // turn that needs it. ChainHost::process feeds the tap on BOTH its paths - the normal one and the dry
    // pass-through it takes when the graph lock is busy - before anything about slot count, and that is what this
    // pins: an empty rack still measures its input.
    {
        std::printf ("\n-- an EMPTY rack still measures its input (the parked-while-borrowed case) --\n");
        ChainHost h;
        h.prepare (48000.0, 512);
        check (h.getNumSlots() == 0, "precondition: no slots, as a parked Link's chain has none",
               juce::String (h.getNumSlots()));
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        const double inc = 2.0 * juce::MathConstants<double>::pi * 997.0 / 48000.0;
        double phase = 0.0;
        const float amp = juce::Decibels::decibelsToGain (-12.0f) * std::sqrt (2.0f);
        const int blocks = (int) std::lround (25.0 * 48000.0 / 512.0);   // 25 s: past the spec's 20 s floor
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 512; ++i) { const float v = amp * (float) std::sin (phase); phase += inc;
                                            buf.setSample (0, i, v); buf.setSample (1, i, v); }
            h.process (buf, midi);
        }
        const auto r = h.trackLevelReading();
        check (r.valid, "an empty rack's input is still measured, so a parked Link can publish  (RED as it stood "
                        "if the tap had sat behind the slot graph: the reading would never become valid)",
               juce::String ("valid=") + (r.valid ? "y" : "n") + " heard=" + juce::String ((int) r.heardSeconds) + "s");
        check (std::abs (r.loudRmsDbfs + 12.0f) < 0.5f,
               "...and it reads the level that was fed, not a dry-path artefact",
               juce::String (r.loudRmsDbfs, 2) + " dBFS for a -12 dBFS tone");
    }

    std::printf ("\n==== track_level_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    guardMain();
    return failures == 0 ? 0 : 1;
}
