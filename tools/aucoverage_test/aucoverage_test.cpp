/*
  AU coverage self-test (29 Aug 2026).

  --bootstrap now partitions: the file walk keeps VST3, the registry pass takes
  AU. That is only safe while the census provably reaches every component a
  .component bundle declares, so EchoJayAuCoverage.h computes that every run and
  anything unprovable falls back to the file walk. These pins are that rule and
  the wiring that consumes it.

  The header is header-inline, so including it IS the shipped implementation:
  no lib copy to drift from, and no rebuild needed for a pin to be honest.
*/
#include <juce_core/juce_core.h>
#include "EchoJayAuCoverage.h"
#include <fstream>
#include <sstream>
#include <iostream>

static int passN = 0, failN = 0;
static void check (bool ok, const juce::String& name, const juce::String& detail = {})
{
    if (ok) { ++passN; std::cout << "  ok    " << name << "\n"; }
    else    { ++failN; std::cout << "  FAIL  " << name
                                 << (detail.isNotEmpty() ? ("\n        " + detail) : juce::String()) << "\n"; }
}

// A .component bundle carrying exactly the AudioComponents entries given.
static juce::File makeComponent (const juce::File& root, const juce::String& name,
                                 const std::vector<std::array<juce::String,3>>& comps,
                                 bool writePlist = true)
{
    auto b = root.getChildFile (name + ".component");
    b.getChildFile ("Contents").createDirectory();
    if (! writePlist) return b;                       // the unreadable case
    juce::String x = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
                     "<key>AudioComponents</key><array>";
    for (const auto& c : comps)
        x << "<dict><key>type</key><string>" << c[0]
          << "</string><key>subtype</key><string>" << c[1]
          << "</string><key>manufacturer</key><string>" << c[2] << "</string></dict>";
    x << "</array></dict></plist>";
    b.getChildFile ("Contents/Info.plist").replaceWithText (x);
    return b;
}

static juce::String slurp (const juce::String& path)
{
    std::ifstream f (path.toRawUTF8()); std::stringstream s; s << f.rdbuf();
    return juce::String (s.str());
}

int main()
{
    std::cout << "AU coverage / bootstrap partition self-test\n";
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("ejcov_" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    tmp.createDirectory();

    // A census holding two of the three products below.
    echojay::auregistry::AuCensus census;
    census.targets.push_back ({ "AudioUnit:Effects/aufx,CVR1,Vend", "aufx", "Vend", "Vend" });
    census.targets.push_back ({ "AudioUnit:Effects/aufx,CVR2,Vend", "aufx", "Vend", "Vend" });
    census.targets.push_back ({ "AudioUnit:Synths/aumu,SYN1,Vend",  "aumu", "Vend", "Vend" });

    // ---- PIN 1: the identifier form matches the census, per component type ----
    // A wrong prefix silently "misses" every instrument and MIDI effect, which
    // would carve them all back into the file walk and undo the partition.
    check (echojay::aucoverage::identifierFor ("aufx","CVR1","Vend") == "AudioUnit:Effects/aufx,CVR1,Vend",
           "cov PIN1: aufx -> Effects");
    check (echojay::aucoverage::identifierFor ("aumu","SYN1","Vend") == "AudioUnit:Synths/aumu,SYN1,Vend",
           "cov PIN1: aumu -> Synths (NOT Effects)");
    check (echojay::aucoverage::identifierFor ("aumi","MID1","Vend") == "AudioUnit:MidiEffects/aumi,MID1,Vend",
           "cov PIN1: aumi -> MidiEffects");
    check (echojay::aucoverage::identifierFor ("aumf","MFX1","Vend") == "AudioUnit:Effects/aumf,MFX1,Vend",
           "cov PIN1: aumf -> Effects");

    // ---- PIN 2: a covered bundle is SKIPPED, so it is never run twice --------
    auto covered   = makeComponent (tmp, "Covered",   {{{"aufx","CVR1","Vend"}}});
    auto shell     = makeComponent (tmp, "Shelly",    {{{"aufx","CVR1","Vend"}},{{"aufx","CVR2","Vend"}}});
    auto uncovered = makeComponent (tmp, "Uncovered", {{{"aufx","NOPE","Vend"}}});
    auto unreadable= makeComponent (tmp, "Unreadable",{}, /*writePlist*/ false);
    juce::Array<juce::File> bundles { covered, shell, uncovered, unreadable };
    auto cov = echojay::aucoverage::assess (bundles, census);

    check (cov.componentsSeen == 4, "cov PIN2: every component bundle is assessed",
           juce::String (cov.componentsSeen));
    check (cov.covers (covered),
           "cov PIN2: a bundle whose identifier IS in the census is covered -> file walk skips it");
    check (cov.covers (shell),
           "cov PIN2: a SHELL declaring several covered components is covered too (the Waves case)");
    check (cov.componentsCovered == 2, "cov PIN2: and exactly those two count as covered",
           juce::String (cov.componentsCovered));

    // ---- PIN 3: anything unprovable is CARVED, and says why ------------------
    check (! cov.covers (uncovered),
           "cov PIN3: an identifier absent from the census is NOT skipped");
    check (! cov.covers (unreadable),
           "cov PIN3: an unreadable Info.plist is NOT skipped (fails safe)");
    check (cov.carved.size() == 2, "cov PIN3: the carve-out holds exactly those two",
           juce::String ((int) cov.carved.size()));
    bool reasons = false;
    for (const auto& c : cov.carved)
    {
        if (c.bundle == uncovered)  reasons |= c.why.startsWith ("not-in-census:");
        if (c.bundle == unreadable) reasons |= (c.why == "no-identifiers");
    }
    check (reasons, "cov PIN3: and every carve entry carries a REASON, not just a name");
    // An empty census must carve everything rather than skip everything: the
    // failure mode that would silently drop all AU coverage.
    {
        echojay::auregistry::AuCensus empty;
        auto none = echojay::aucoverage::assess (bundles, empty);
        check (none.componentsCovered == 0 && none.carved.size() == 4,
               "cov PIN3: an EMPTY census covers nothing and carves everything",
               juce::String (none.componentsCovered) + "/" + juce::String ((int) none.carved.size()));
    }

    // ---- PIN 4: THE WIRING in runBootstrap ----------------------------------
    const auto src = slurp ("tools/ejextract/main.cpp");
    check (src.isNotEmpty(), "cov PIN4: main.cpp readable from the repo root");
    check (src.contains ("if (ext == \".component\" && coverage.covers (b)) { ++registryCovered; continue; }"),
           "cov PIN4: the file walk emits no .component target the registry covers");
    check (src.contains ("runIsolatedWorkerOn (t.identifier, juce::String(), workDir)"),
           "cov PIN4: the registry path dispatches BY IDENTIFIER");
    check (src.contains ("echojay::aucoverage::assess (bundles, census)"),
           "cov PIN4: coverage is computed from the LIVE census each run");
    check (src.contains ("logLine (\"  carve-out: \" + c.bundle.getFileName()"),
           "cov PIN4: and every carve-out entry is logged, never silent");
    // The two ledgers must stay separate: a merge would need a key that is
    // neither a bundle path nor an identifier.
    check (src.contains ("dir.getChildFile (\"au_registry_ledger.json\")")
           && src.contains ("dir.getChildFile (\"bootstrap_ledger.json\")"),
           "cov PIN4: both ledgers are still written, separately");

    tmp.deleteRecursively();
    std::cout << passN << " passed, " << failN << " failed\n";
    return failN == 0 ? 0 : 1;
}
