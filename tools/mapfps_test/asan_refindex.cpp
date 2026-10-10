/*
  asan_refindex.cpp — EJReferenceIndex.h under AddressSanitizer and
  UndefinedBehaviorSanitizer, and NOTHING ELSE.

  WHY SEPARATE FROM mapfps_test. The gate harness is one main() of ~7,300 lines.
  Instrumented, it dies deterministically at check ~192 inside the 6c param-read
  pins, which construct real processors; the reference-index pins sit at the end
  of main() and are never reached. Excluding regions one at a time through a
  monolith to get to the last block is the wrong shape of work. This file drives
  the index API directly.

  IT IS NOT THE PINS. mapfps_test owns the assertions; this owns the memory
  behaviour. A clean run here means "no ASan or UBSan finding on these paths",
  which is narrower than "the header is correct".
*/
#include <JuceHeader.h>
#include "EJReferenceIndex.h"
#include <thread>
#include <atomic>
#include <iostream>

using namespace echojay;

static juce::File freshDir (const char* tag)
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile (juce::String ("asanrefidx_") + tag + "_"
                               + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    d.deleteRecursively();
    d.createDirectory();
    return d;
}

static RefEntry entryAt (const char* id, const char* path, const char* added)
{
    RefEntry e;
    e.id = id; e.path = path; e.name = path; e.addedAt = added;
    e.measurementEpoch = kRefMeasurementEpoch;
    e.availability = RefAvailability::Present;
    e.measurements.reduction = "wholeFileAverage";
    e.measurements.windowSeconds = 168.5f;
    e.measurements.hasEqCurve = true;
    for (int i = 0; i < 64; ++i) e.measurements.eqCurve[(size_t) i] = -50.0f - (float) i;
    e.measurements.valid = true;
    e.waveform.points.assign (300, 0.5f);
    return e;
}

int main()
{
    std::cout << "asan_refindex: driving EJReferenceIndex.h\n";

    // 1. parse of every shape, including the ones designed to be refused
    for (const char* j : { "{}", "", "not json", "{\"schema\":99,\"entries\":[]}",
                           "{\"schema\":1,\"entries\":[{\"id\":\"r_a\"}]}",
                           "{\"schema\":1,\"entries\":[{\"id\":\"r_a\",\"measurements\":"
                           "{\"eqCurve\":[1,2,3],\"macroBandDb\":[1,2]}}]}" })
    {
        auto ix = parseReferenceIndex (j);
        (void) refReadOnlyNotice (ix);
        (void) refIndexMayWrite (ix);
        migrateReferenceIndex (ix);
        (void) writeReferenceIndex (ix, "T", "2.26.4");
    }

    // 2. round trip with unknown keys on both levels, twice, so the clone path
    //    that produced the commit-1 use-after-free runs repeatedly
    {
        juce::String src = "{\"schema\":1,\"futureDoc\":\"keep\",\"entries\":[{"
                           "\"id\":\"r_1\",\"path\":\"/m/x.wav\",\"futureEntry\":7,"
                           "\"measurements\":{\"reduction\":\"wholeFileAverage\",\"eqCurve\":[";
        for (int i = 0; i < 64; ++i) src += (i ? "," : "") + juce::String (-40.0 - i);
        src += "],\"macroBandDb\":null}}],\"tombstones\":[{\"id\":\"r_d\",\"at\":\"T\"}]}";
        for (int pass = 0; pass < 5; ++pass)
        {
            auto ix = parseReferenceIndex (src);
            src = writeReferenceIndex (ix, "T", "2.26.4");
        }
        std::cout << "  round trip x5 ok, " << src.length() << " bytes\n";
    }

    // 3. merge: union, path dedupe, tombstone drop
    {
        ReferenceIndex a, b;
        a.entries.push_back (entryAt ("r_a", "/m/same.wav", "2026-01-01T00:00:00Z"));
        a.entries.push_back (entryAt ("r_k", "/m/keep.wav", "2026-01-01T00:00:00Z"));
        b.entries.push_back (entryAt ("r_b", "/m/same.wav", "2026-06-01T00:00:00Z"));
        b.tombstones.push_back ({ "r_k", "2026-02-01T00:00:00Z" });
        mergeReferenceIndex (a, b);
        std::cout << "  merge -> " << a.entries.size() << " entries, "
                  << a.tombstones.size() << " tombstones\n";
    }

    // 4. commit paths: absent, present, unreadable, newer schema
    {
        auto d = freshDir ("commit");
        ReferenceIndex mine;
        mine.entries.push_back (entryAt ("r_c", "/m/c.wav", "2026-01-01T00:00:00Z"));
        (void) commitReferenceIndex (d, mine, "T", "2.26.4");
        (void) commitReferenceIndex (d, mine, "T", "2.26.4");
        refIndexFile (d).replaceWithText ("{ broken");
        (void) commitReferenceIndex (d, mine, "T", "2.26.4");
        refIndexFile (d).replaceWithText ("{\"schema\":99,\"entries\":[]}");
        (void) commitReferenceIndex (d, mine, "T", "2.26.4");
        d.deleteRecursively();
        std::cout << "  commit paths ok\n";
    }

    // 5. the concurrency path, which is where a data race would live
    {
        auto d = freshDir ("race");
        constexpr int kThreads = 8;
        // COUNTERS ONLY, NO SHARED STRING. A juce::String assigned from eight
        // threads is a data race, and ASan does not detect races: writing one
        // here to report a message would have introduced the exact class of
        // defect this run exists to look for, invisibly.
        std::atomic<int> refused { 0 };
        std::atomic<int> lockRefusals { 0 };
        std::vector<std::thread> ts;
        for (int i = 0; i < kThreads; ++i)
            ts.emplace_back ([&d, i, &refused, &lockRefusals]
            {
                for (int r = 0; r < 3; ++r)
                {
                    ReferenceIndex mine;
                    mine.entries.push_back (entryAt (
                        ("r_t" + juce::String (i) + "_" + juce::String (r)).toRawUTF8(),
                        ("/race/t" + juce::String (i) + "_" + juce::String (r) + ".wav").toRawUTF8(),
                        "2026-01-01T00:00:00Z"));
                    const auto c = commitReferenceIndex (d, mine, "T", "2.26.4");
                    if (! c.ok)
                    {
                        ++refused;
                        if (c.message.containsIgnoreCase ("saving")) ++lockRefusals;
                    }
                }
            });
        for (auto& t : ts) t.join();
        auto after = loadReferenceIndex (d);
        std::cout << "  24 concurrent commits -> " << after.index.entries.size()
                  << " entries, " << refused.load() << " refused ("
                  << lockRefusals.load() << " of them a LOCK timeout)\n";
        std::cout << "    landed + refused = "
                  << ((int) after.index.entries.size() + refused.load())
                  << " of 24 (equal means nothing was LOST, only declined)\n";
        d.deleteRecursively();
    }

    // 6. the pure predicates, including the sentinel paths
    {
        RefEntry e; (void) refNeedsReanalysis (e); (void) refIsStale (e);
        (void) refIsUnmeasured (e);
        ReferenceIndex ix; (void) refAddRefusal (ix);
        for (int i = 0; i < kRefIndexMaxEntries; ++i) ix.entries.push_back (RefEntry{});
        (void) refAddRefusal (ix);
        (void) refIndexBackoffMs (99); (void) refIndexShouldEscalate (5, 0);
        (void) refIndexEscalatedMessage (180000);
        (void) refFindByPath (ix, "/nope"); (void) refFindById (ix, "r_nope");
        (void) newReferenceId();
        std::cout << "  predicates ok\n";
    }

    std::cout << "asan_refindex: COMPLETED\n";
    return 0;
}
