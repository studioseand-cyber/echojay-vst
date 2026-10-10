/*
  EjmapDrafts.h - THE DRAFT PROFILES (Kathy, 7 Oct 2026): every Phase B category's profile shape, drafted against its v0.1
  PROPOSAL spec, DATA ONLY. The rules, pinned here and nowhere else:

    - a draft lands in cert/phaseb/<category>/drafts/<stem>.<kind>.draft.json - the mode writes <out>/drafts/ and the Phase B
      parent moves that folder into place like any other record folder; NEVER cert/profiles (the exporter never reads drafts/)
    - every draft carries "spec": "<SPEC NAME> v0.2 PROPOSAL" (specTag; v0.2 = Sean's rulings of 8 Oct, SPEC_RULINGS_v0_2.md, applied
      to the v0.1 specs until each is re-issued), "status" saying it is a draft, and the plugin /
      measured blocks the specs share
    - a draft is DERIVED FROM THE RECORD, never measured separately: the same function runs at the end of the mode and in the
      derive-only pass (--phaseb-drafts) over an existing folder (Sean's), so a draft can be re-cut without a re-run
    - the `notes` rule from the compressor spec: a reason for every null and every unusable position

  The per-category derivations live beside their records' derivations (EjmapLimiter.h ceilingBlock, EjmapEq.h ..., EjmapGainCal.h ...);
  this file holds the shared shape and the file rule.
*/

#pragma once

#include <juce_core/juce_core.h>

namespace ejmap::drafts
{

inline constexpr const char* kSpecSuffix = " v0.2 PROPOSAL";   // 8 Oct: Sean's rulings (was v0.1)
inline juce::String specTag (const juce::String& specName) { return specName + kSpecSuffix; }
inline constexpr const char* kFolder = "drafts";
inline juce::File draftDir (const juce::File& out) { return out.getChildFile (kFolder); }
inline juce::String draftFileName (const juce::String& stem, const juce::String& kind) { return stem + "." + kind + ".draft.json"; }
inline juce::File draftFile (const juce::File& out, const juce::String& stem, const juce::String& kind) { return draftDir (out).getChildFile (draftFileName (stem, kind)); }

// the path rule: under a drafts/ folder, never under a profiles/ folder (the export's folder), never a .profile.json
inline bool pathAllowed (const juce::File& f)
{
    const auto parent = f.getParentDirectory().getFileName();
    if (parent != kFolder) return false;
    for (auto d = f.getParentDirectory(); d != juce::File() && d.getFileName().isNotEmpty(); d = d.getParentDirectory()) if (d.getFileName() == "profiles") return false;
    return f.getFileName().endsWith (".draft.json");
}
// the content rule: a spec tag ending in kSpecSuffix (v0.2 PROPOSAL), a status, a schema
inline juce::String contentProblem (const juce::var& v)
{
    if (! v.isObject()) return "not an object";
    const auto spec = v.getProperty ("spec", "").toString();
    if (! spec.endsWith (kSpecSuffix)) return "spec tag '" + spec + "' does not end in '" + juce::String (kSpecSuffix) + "'";
    if (v.getProperty ("status", "").toString().isEmpty()) return "no status";
    if (v.getProperty ("schema", "").toString().isEmpty()) return "no schema";
    return {};
}
// the write: refused (returns the problem) when either rule fails, so a mode can never put a draft where a profile goes
inline juce::String writeDraft (const juce::File& f, const juce::var& v)
{
    if (! pathAllowed (f)) return "draft path refused: " + f.getFullPathName() + " is not <out>/drafts/<stem>.<kind>.draft.json outside any profiles/ folder";
    if (const auto p = contentProblem (v); p.isNotEmpty()) return "draft content refused: " + p;
    f.getParentDirectory().createDirectory();
    const auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    tmp.replaceWithText (juce::JSON::toString (v) + "\n", false, false, "\n");
    f.deleteFile(); tmp.moveFileTo (f);
    return {};
}

// the shared blocks
inline juce::var pluginBlock (const juce::String& name, const juce::String& manufacturer, const juce::String& uidHex, const juce::String& version, const juce::var& mapFp)
{
    auto* pl = new juce::DynamicObject(); pl->setProperty ("name", name); pl->setProperty ("manufacturer", manufacturer); pl->setProperty ("format", "AudioUnit");
    pl->setProperty ("plugin_id", "AudioUnit|" + uidHex + "|" + version); pl->setProperty ("version", version); pl->setProperty ("map_fp", mapFp);
    return juce::var (pl);
}
inline juce::var measuredBlock (const juce::String& tool, const juce::String& date, int sampleRate, const juce::String& signal)
{
    auto* m = new juce::DynamicObject(); m->setProperty ("tool", tool); m->setProperty ("date", date); m->setProperty ("sample_rate", sampleRate); m->setProperty ("signal", signal);
    return juce::var (m);
}
// the status line names the same version as the spec tag (Kathy, 10 Oct: it said v0.1 under a v0.2 tag)
inline constexpr const char* kStatusVersion = "v0.2";
inline juce::String statusLine (const juce::String& specName) { return "DRAFT against " + specName + " " + kStatusVersion + " (a proposal): data only, not exported, not published"; }
// identity -> uid hex / version ("AudioUnit|417f6e6e|1.2.1")
inline juce::String uidOfIdentity (const juce::String& identity) { return identity.fromFirstOccurrenceOf ("|", false, false).upToFirstOccurrenceOf ("|", false, false); }
inline juce::String versionOfIdentity (const juce::String& identity) { return identity.fromLastOccurrenceOf ("|", false, false); }
inline juce::String stemOfIdentity (const juce::String& identity) { return "AudioUnit_" + uidOfIdentity (identity) + "_" + versionOfIdentity (identity); }

} // namespace ejmap::drafts
