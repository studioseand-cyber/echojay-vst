#pragma once

// =============================================================================
//  ROUND 7a's PINS: MA4, STORED FIGURES SURVIVE A MOVED FILE (kt).
// =============================================================================
//
// A's R4-1 (82e46d99, shape 1) keeps a reference's hop series in the library
// index and reads it back into ReferenceResult::hopFrames on restore. The page
// already windows selectedReference()->hopFrames, so these rows hold the whole
// road, file to window:
//
// kt PIN1   through the index FILE: the series is committed, the audio file is
//           moved away, and the entry read back by its id still carries every
//           hop; relinked to the new place with no re-analysis, it still does;
// kt PIN2   through restore into the page: a reference rebuilt from the moved
//           entry, as A's restore rebuilds it, gives the loudest-30 window the
//           analysed reference gave (same start, same six bands);
// kt PIN3   an entry written before the series restores with none, and the
//           page still refuses the reference-side window rather than falling
//           back to the whole file.
//
// The index calls are A's real ones (commitReferenceIndex, loadReferenceIndex)
// against a temporary directory, never the user's library.
// =============================================================================

#include "EJMatchHandOff.h"
#include "EJReferenceIndex.h"
#include "EJRefHopSeries.h"
#include "k5_fakehost.h"

namespace k7apins
{
using namespace echojay;
using k5pins::FakeMatchHost;

/** A reference rebuilt from an index entry the way A's restore rebuilds it
    (PluginProcessor.cpp, ensureReferenceLibraryLoaded): no re-analysis, and the
    hop series through refHopSeriesFromFloats. Only what the page reads. */
inline ReferenceResult restoredReference (const RefEntry& e)
{
    ReferenceResult r;
    r.name = e.name;
    r.path = e.path;
    r.durationSeconds = e.source.durationSeconds;
    r.data.integrated = e.measurements.integrated;
    r.hopFrames = refHopSeriesFromFloats (e.measurements.hopSeries);
    return r;
}

template <typename Check>
inline void runK7aPins (Check&& check)
{
    // n hops at -20 LUFS with every band at power 1, and a loud section
    // [loudAt, loudAt + 300) at -10 LUFS whose low-mids power is 8.
    auto series = [] (std::size_t n, std::size_t loudAt)
    {
        std::vector<MeterEngine::HopFrame> v (n);
        for (std::size_t i = 0; i < n; ++i)
        {
            auto& h = v[i];
            const bool loud = i >= loudAt && i < loudAt + 300;
            h.shortTermLufs = i < 29 ? -100.0f : (loud ? -10.0f : -20.0f);
            h.shortTermTruePeak = h.shortTermLufs + 9.0f;
            h.bandPower.fill (1.0f);
            if (loud) h.bandPower[2] = 8.0f;
            h.bandPeak = h.bandPower;
            h.bandFrames = 9;
        }
        return v;
    };
    auto capture = [&]
    {
        CaptureSnapshot c;
        c.id = "1730000000007"; c.name = "Capture 1";
        c.hopFrames = series (600, 200); c.hopFramesHops = 600;
        return c;
    };
    auto sides = []
    {
        MatchSide m, r;
        m.macro.fill (0.0f); m.hasMacro = true; m.macroReduction = SpectralReduction::WholeWindowAverage;
        m.durationSeconds = 60.0f; m.integrated = -14.0f; m.truePeak = -1.0f;
        r.macro.fill (0.0f); r.hasMacro = true; r.macroReduction = SpectralReduction::WholeFileAverage;
        r.durationSeconds = 180.0f; r.integrated = -12.0f; r.truePeak = -0.5f;
        return MatchSides { m, r };
    };
    // The page's view over the loudest 30 s with this reference selected.
    auto viewOf = [&] (const ReferenceResult& ref)
    {
        FakeMatchHost host;
        host.snaps = { capture() };
        host.ref = ref;
        host.mixSlot = k5pins::snapshotSlot (0);
        host.refSlot.kind = CompareSlotState::Kind::Reference; host.refSlot.index = 0;
        matchWindowMemory().set (ref.path, MatchWindowMode::Loudest30);
        auto v = matchPageViewOf (host, sides(), host.matchSlots());
        matchWindowMemory().set (ref.path, MatchWindowMode::Whole);
        return v;
    };

    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("k7a_ma4_" + juce::String (juce::Time::currentTimeMillis()));
    const auto lib = root.getChildFile ("lib");
    const auto wav = root.getChildFile ("music").getChildFile ("song-x.wav");
    const auto moved = root.getChildFile ("elsewhere").getChildFile ("song-x.wav");
    wav.getParentDirectory().createDirectory();
    moved.getParentDirectory().createDirectory();
    wav.replaceWithText ("only its place matters");

    ReferenceResult analysed;
    analysed.name = "Song X"; analysed.path = wav.getFullPathName();
    analysed.durationSeconds = 180.0f; analysed.data.integrated = -12.0f;
    analysed.hopFrames = series (1800, 900); analysed.hops = 1800;

    RefEntry e;
    e.id = "r_k7a0000000001"; e.name = analysed.name; e.path = analysed.path;
    e.measurementEpoch = kRefMeasurementEpoch;
    e.availability = RefAvailability::Present;
    e.source.durationSeconds = 180.0f;
    e.measurements.valid = true;
    e.measurements.reduction = "wholeFileAverage";
    e.measurements.windowSeconds = 180.0f;
    e.measurements.integrated = -12.0f;
    e.measurements.eqCurve.fill (-40.0f);
    e.measurements.hasEqCurve = true;
    e.measurements.hopSeries = refHopSeriesToFloats (analysed.hopFrames);   // as commitReferenceLibrary writes it

    // kt PIN1 -- THROUGH THE INDEX FILE, THE AUDIO MOVED AWAY.
    RefEntry afterMove, afterRelink;
    {
        ReferenceIndex mine;
        mine.entries.push_back (e);
        const auto c1 = commitReferenceIndex (lib, mine, "2026-10-30T00:00:00Z", "2.31.0");
        const bool wasMoved = wav.moveFileTo (moved) && ! wav.existsAsFile() && moved.existsAsFile();

        const auto l1 = loadReferenceIndex (lib);
        const int at1 = refFindById (l1.index, e.id);
        if (at1 >= 0) afterMove = l1.index.entries[(size_t) at1];

        // The user points the entry at the new place: same id, no re-analysis.
        RefEntry relinked = afterMove;
        relinked.path = moved.getFullPathName();
        ReferenceIndex mine2;
        mine2.entries.push_back (relinked);
        const auto c2 = commitReferenceIndex (lib, mine2, "2026-10-30T00:01:00Z", "2.31.0");
        const auto l2 = loadReferenceIndex (lib);
        const int at2 = refFindById (l2.index, e.id);
        if (at2 >= 0) afterRelink = l2.index.entries[(size_t) at2];

        auto sameHops = [&] (const RefEntry& x)
        {
            const auto back = refHopSeriesFromFloats (x.measurements.hopSeries);
            if (back.size() != analysed.hopFrames.size()) return false;
            for (std::size_t i = 0; i < back.size(); ++i)
            {
                const auto& a = analysed.hopFrames[i];
                const auto& b = back[i];
                if (b.bandFrames != a.bandFrames || b.flags != a.flags
                    || std::abs (b.shortTermLufs - a.shortTermLufs) > 1.0e-4f
                    || std::abs (b.shortTermTruePeak - a.shortTermTruePeak) > 1.0e-4f) return false;
                for (std::size_t k = 0; k < 6; ++k)
                    if (std::abs (b.bandPower[k] - a.bandPower[k]) > 1.0e-5f * a.bandPower[k]) return false;
            }
            return true;
        };
        const bool ok = c1.ok && c2.ok && wasMoved && at1 >= 0 && at2 >= 0
                     && afterMove.path == wav.getFullPathName() && sameHops (afterMove)
                     && afterRelink.path == moved.getFullPathName() && sameHops (afterRelink)
                     && l2.index.entries.size() == 1;
        check (ok, "kt PIN1 (MA4): the reference's stored series survives the file moving, read back from the index "
                   "by its id, and again after the entry is relinked to the new place with no re-analysis",
               juce::String ("commit ") + (c1.ok ? "ok" : c1.message) + ", moved " + (wasMoved ? "yes" : "no")
                   + ", entry " + juce::String (at1) + "/" + juce::String (at2) + ", hops "
                   + juce::String ((int) refHopSeriesFromFloats (afterMove.measurements.hopSeries).size()) + "/"
                   + juce::String ((int) refHopSeriesFromFloats (afterRelink.measurements.hopSeries).size()));
    }

    // kt PIN2 -- THROUGH RESTORE INTO THE PAGE'S WINDOW.
    {
        const auto fresh = viewOf (analysed);
        const auto atOld = viewOf (restoredReference (afterMove));      // the file is gone from here
        const auto atNew = viewOf (restoredReference (afterRelink));
        auto sameWindow = [&] (const MatchPageView& v)
        {
            if (! v.w.windowed || ! v.w.refWindow || ! fresh.w.refWindow || v.w.why.isNotEmpty()) return false;
            if (v.w.refWindow->begin != fresh.w.refWindow->begin || v.w.refWindow->end != fresh.w.refWindow->end) return false;
            for (std::size_t b = 0; b < 6; ++b)
                if (std::abs (v.w.ref.macro[b] - fresh.w.ref.macro[b]) > 0.01f) return false;
            return v.w.ref.hasMacro;
        };
        const bool planted = fresh.w.windowed && fresh.w.refWindow && fresh.w.refWindow->begin == 900;
        check (planted && sameWindow (atOld) && sameWindow (atNew),
               "kt PIN2 (MA4): a reference restored from the moved entry gives the page the loudest 30 s the analysed "
               "one gave, the same hops and the same six bands, at the old place and the new",
               juce::String ("analysed at ") + (fresh.w.refWindow ? juce::String ((int) fresh.w.refWindow->begin) : "none")
                   + ", restored at " + (atOld.w.refWindow ? juce::String ((int) atOld.w.refWindow->begin) : "none")
                   + " / " + (atNew.w.refWindow ? juce::String ((int) atNew.w.refWindow->begin) : "none")
                   + "; " + atOld.w.why + atNew.w.why);
    }

    // kt PIN3 -- AN ENTRY WITH NO SERIES KEEPS THE REFUSAL.
    {
        RefEntry old = afterRelink;
        old.measurements.hopSeries.clear();
        const auto v = viewOf (restoredReference (old));
        check (! v.w.windowed && ! v.w.ref.hasMacro && v.w.mixWindow.has_value()
                   && v.w.why.contains ("the reference's hop series"),
               "kt PIN3 (MA4): an entry written before the series restores with none, and the page refuses the "
               "reference's window rather than showing the whole file's bands",
               v.w.why);
    }

    root.deleteRecursively();
}

} // namespace k7apins
