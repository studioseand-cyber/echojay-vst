// track_level_guard - COMP_PROFILE_SPEC_v1 section 5, the level the plugin sends with a build.
//
//   "track_level": { "loud_rms_dbfs": -18.4, "loud_peak_dbfs": -6.2, "window": "400ms_rms_p95", "heard_s": 90 }
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

    std::printf ("\n-- (1) under 20 s heard there is NO answer --\n");
    {
        echojay::TrackLevel tl; tl.prepare (kSr);
        feed (tl, take (10.0, -12.0f, -30.0f, 0.25));
        const auto r = tl.read();
        check (! r.valid,
               "(1) 10 s of audio gives no reading - the spec's null  (RED as it stood: there was no track_level "
               "at all, so the server had no level to compute a threshold from)",
               "heard " + f1 (r.heardSeconds) + " s, valid=" + (r.valid ? "y" : "n"));
        check (tl.toVar().isVoid(),
               "(1) ...and the wire field is null, not a number and not an object",
               tl.toVar().isVoid() ? juce::String ("void") : juce::JSON::toString (tl.toVar()));
        check (std::abs (r.heardSeconds - 10.0f) < 0.6f,
               "(1) ...while the heard time it reports is the audio it actually got",
               f1 (r.heardSeconds) + " s of 10.0");
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
            check (o->getProperty ("window").toString() == "400ms_rms_p95",
                   "(5) window says how it was measured", o->getProperty ("window").toString());
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
