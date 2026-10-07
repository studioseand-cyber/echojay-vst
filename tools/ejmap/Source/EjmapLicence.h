/*
  EjmapLicence.h - THE LICENCE FILE (Sean's rulings, 6 Oct 2026): cert/licences.csv, the alias table, the verdict per plugin.

  THE FILE (Sean's format, ~/Desktop/licence_states_UAD_2026-10-06.csv copied to cert/licences.csv for runs; other vendors in
  the same format): vendor,product,state,demo_end,note with state = owned | unowned | expired | demo. The products are the
  vendor's PRODUCT / BUNDLE names, not plugin names ("UA 1176 Limiter Collection" covers every 1176 plugin).

  THE ALIAS TABLE (plugin -> licence line), in this order and never a silent fuzzy guess:
    1. exact: the plugin's name (its vendor prefix dropped: "UAD Precision Limiter" -> "Precision Limiter") equals a line's
       product, or the line's product is the plugin's name followed by a descriptor ("Precision Limiter" -> "Precision
       Limiter" / "Cambridge" -> "Cambridge EQ"). Two lines fitting = AMBIGUOUS (both listed).
    2. the note's "covers ..." / "includes ..." list, names separated by commas or "and".
    3. the explicit vendor-model table below (kExplicit): a bundle's plugins by model - every such match is listed on the
       review sheet as "explicit", so a human sees it.
    A plugin no rule matches is UNMATCHED: on licence_review.txt, NOT loaded until reviewed.
  "bundled with UAD-2 hardware" (Sean, 6 Oct): the CS-1 channel strip set is free with the hardware - an owned entry the
  table supplies itself (kBundledWithHardware), so those plugins never land on the review sheet.

  THE VERDICT on a run date: owned -> load; demo whose demo_end is on or after the date -> load AND every record, trace
  header and draft carries {"licence": {"state": "demo", "expires": "<date>"}}; demo past its end -> expired; expired /
  unowned / unmatched -> never loaded, filed needs_licence with the reason.

  Which plugins a vendor's lines govern: a line with vendor "UAD-2" governs every plugin named "UAD ..."; any other vendor
  governs the plugins whose manufacturer is that vendor (case-insensitive). A plugin no line's vendor governs is outside
  the file: the file says nothing about it.

  PURE, pinned in RoundTripTest.cpp (LC1-LC9).
*/

#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <optional>
#include <vector>

namespace ejmap::licence
{

struct Line { juce::String vendor, product, state, demoEnd, note; int row = 0; };

inline juce::StringArray splitCsvRow (const juce::String& row)
{
    juce::StringArray out; juce::String cur; bool q = false;
    for (int i = 0; i < row.length(); ++i)
    {
        const auto c = row[i];
        if (c == '"') { if (q && i + 1 < row.length() && row[i + 1] == '"') { cur << '"'; ++i; } else q = ! q; }
        else if (c == ',' && ! q) { out.add (cur.trim()); cur.clear(); }
        else cur << c;
    }
    out.add (cur.trim()); return out;
}
inline std::vector<Line> parseCsv (const juce::String& text)
{
    std::vector<Line> lines; int n = 0;
    for (const auto& row : juce::StringArray::fromLines (text))
    {
        ++n; if (row.trim().isEmpty()) continue;
        const auto f = splitCsvRow (row); if (f.size() < 3) continue;
        if (n == 1 && f[0].equalsIgnoreCase ("vendor")) continue;
        Line l; l.vendor = f[0]; l.product = f[1]; l.state = f[2].toLowerCase(); l.demoEnd = f.size() > 3 ? f[3] : juce::String(); l.note = f.size() > 4 ? f[4] : juce::String(); l.row = n;
        lines.push_back (l);
    }
    return lines;
}

// the plugin's name without its vendor prefix ("UAD "), lower-cased, punctuation squeezed
inline juce::String normalise (const juce::String& s) { auto t = s.trim().toLowerCase().replaceCharacters ("_-", "  "); juce::String o; bool sp = false; for (auto c : t) { if (juce::CharacterFunctions::isLetterOrDigit (c)) { o << c; sp = false; } else if (! sp && o.isNotEmpty()) { o << ' '; sp = true; } } return o.trim(); }
inline juce::String pluginShortName (const juce::String& pluginName, const juce::String& vendor)
{
    if (vendor.equalsIgnoreCase ("UAD-2") && pluginName.startsWithIgnoreCase ("UAD ")) return pluginName.substring (4).trim();
    return pluginName.trim();
}
inline bool governs (const Line& l, const juce::String& pluginName, const juce::String& manufacturer)
{
    if (l.vendor.equalsIgnoreCase ("UAD-2")) return pluginName.startsWithIgnoreCase ("UAD ");
    return manufacturer.equalsIgnoreCase (l.vendor);
}

// THE EXPLICIT VENDOR-MODEL TABLE (UAD, 6 Oct): bundle product -> the plugin short names it carries. Every match from here is
// "explicit" on the review sheet. Kept small and literal; a model missing here lands on the review sheet, never guessed.
inline const std::map<juce::String, juce::StringArray>& kExplicit()
{
    static const std::map<juce::String, juce::StringArray> k {
        { "UA 1176 Limiter Collection",            { "UA 1176 Rev A", "UA 1176AE", "UA 1176LN Rev E" } },
        { "Tube-Tech EQ Collection",               { "Tube-Tech PE 1C", "Tube-Tech ME 1B" } },
        { "API 500 EQ Collection",                 { "API 550A", "API 560" } },
        { "MDWEQ5",                                { "MDWEQ5-3B", "MDWEQ5-5B" } },
        { "Fairchild Tube Limiter Collection",     { "Fairchild 660", "Fairchild 670" } },
        { "Teletronix LA-2A Leveler Collection",   { "Teletronix LA-2A Gray", "Teletronix LA-2A Silver", "Teletronix LA-2" } },
        { "Neve 1073 Preamp and EQ Collection",    { "Neve 1073", "Neve 1073SE" } },
        { "Neve Dynamics Collection",              { "Neve 2254 E", "Neve 2254 E Dual", "Neve 33609 C", "Neve 33609SE" } },
        { "Pultec Passive EQ Collection",          { "Pultec EQP-1A", "Pultec MEQ-5", "Pultec HLF-3C" } },
        { "Manley Massive Passive EQ Collection",  { "Manley Massive Passive", "Manley Massive Passive MST" } },
        { "SSL 4000 G Bus Compressor Collection",  { "SSL G Bus Compressor" } },
        { "SSL 4000 G Legacy Bus Compressor",      { "SSL G Bus Compressor Legacy" } },
        { "Helios Type 69 Preamp and EQ Collection", { "Helios Type 69" } },
        { "Shadow Hills Mastering Compressor",     { "Shadow Hills Mastering Compressor", "Shadow Hills Class A Mastering Comp" } },
        { "bx_digital V3 EQ Collection",           { "bx_digital V3", "bx_digital V3 mix" } },
        { "bx_digital V2 EQ",                      { "bx_digital V2", "bx_digital V2 Mono" } },
        { "Precision Multiband Compressor",        { "Precision Multiband" } },
        { "Oxford Limiter V2",                     { "Oxford Limiter" } } };
    return k;
}
// bundled with the UAD-2 hardware (Sean, 6 Oct): the CS-1 channel strip set, owned by owning the device
inline const Line& kBundledWithHardware() { static const Line l { "UAD-2", "bundled with UAD-2 hardware", "owned", "", "the CS-1 channel strip set is free with the hardware (Sean, 6 Oct)", 0 }; return l; }
inline const juce::StringArray& kCs1Set() { static const juce::StringArray k { "CS-1", "Precision Delay Mod", "Precision Delay Mod L", "Precision Reflection Engine", "EX-1", "DM-1", "DM-1L", "DM-1 L", "RS-1" }; return k; }

struct Match { bool matched = false, ambiguous = false, explicitTable = false, bundled = false, siblingResolved = false; std::optional<Line> line; juce::String how; juce::StringArray candidates; };

inline juce::StringArray coveredBy (const Line& l)
{
    juce::StringArray out; const auto n = l.note;
    for (const char* key : { "covers", "includes" })
    {
        int at = n.indexOfIgnoreCase (key); if (at < 0) continue;
        auto rest = n.substring (at + (int) juce::String (key).length()).trim();
        if (rest.startsWithIgnoreCase (":")) rest = rest.substring (1);
        const auto stop = rest.indexOfAnyOf (";.("); if (stop > 0) rest = rest.substring (0, stop);
        rest = rest.replace (" and ", ",");
        for (auto& t : juce::StringArray::fromTokens (rest, ",", "")) if (t.trim().isNotEmpty()) out.add (t.trim());
    }
    return out;
}

// siblings: every other governed plugin name (installed, or named in the folder's rows). A descriptor line that is really a LONGER
// sibling's ("AMS RMX16 Expanded Digital Reverb" when "UAD AMS RMX16 Expanded" exists beside "UAD AMS RMX16") is the sibling's, not
// ours: dropped, said on the review sheet as "sibling-resolved". A rule, not a guess.
inline Match matchPlugin (const juce::String& pluginName, const juce::String& manufacturer, const std::vector<Line>& lines, const juce::StringArray& siblings = {})
{
    Match m;
    std::vector<const Line*> gov; for (const auto& l : lines) if (governs (l, pluginName, manufacturer)) gov.push_back (&l);
    if (gov.empty()) { m.how = "no licence line governs this vendor"; return m; }
    const auto vendor = gov.front()->vendor; const auto shortName = pluginShortName (pluginName, vendor); const auto key = normalise (shortName);
    // 1. exact, or the line's product = the plugin + a descriptor
    std::vector<const Line*> exact, desc;
    for (const auto* l : gov) { const auto p = normalise (l->product); if (p == key) exact.push_back (l); else if (p.startsWith (key + " ")) desc.push_back (l); }
    if (exact.size() == 1) { m.matched = true; m.line = *exact.front(); m.how = "exact name"; return m; }
    if (exact.empty() && desc.size() > 1)
    {
        std::vector<const Line*> own;
        for (const auto* l : desc)
        {
            bool sib = false; const auto p = normalise (l->product);
            for (const auto& sn : siblings) { const auto sk = normalise (pluginShortName (sn, vendor)); if (sk.length() > key.length() && sk.startsWith (key + " ") && (p == sk || p.startsWith (sk + " "))) { sib = true; break; } }
            if (! sib) own.push_back (l);
        }
        if (own.size() == 1) { m.matched = true; m.siblingResolved = true; m.line = *own.front(); m.how = "name + descriptor (" + own.front()->product + "), the other " + juce::String ((int) (desc.size() - 1)) + " line(s) belong to a longer sibling plugin: sibling-resolved, listed for review"; return m; }
    }
    if (exact.size() > 1 || (exact.empty() && desc.size() > 1)) { m.ambiguous = true; m.how = "ambiguous: " + juce::String ((int) (exact.size() + desc.size())) + " lines fit the name"; for (const auto* l : exact) m.candidates.add (l->product); for (const auto* l : desc) m.candidates.add (l->product); return m; }
    if (desc.size() == 1) { m.matched = true; m.line = *desc.front(); m.how = "name + descriptor (" + desc.front()->product + ")"; return m; }
    // 2. the note's covers / includes
    std::vector<const Line*> covered;
    for (const auto* l : gov) for (const auto& c : coveredBy (*l)) { const auto cn = normalise (c); if (cn.isNotEmpty() && (cn == key || key.endsWith (" " + cn))) { covered.push_back (l); break; } }   // a note names the model ("PE 1C"), the plugin carries the brand too
    if (covered.size() == 1) { m.matched = true; m.line = *covered.front(); m.how = "the note's covers/includes (" + covered.front()->product + ")"; return m; }
    if (covered.size() > 1) { m.ambiguous = true; m.how = "ambiguous: named in the notes of " + juce::String ((int) covered.size()) + " lines"; for (const auto* l : covered) m.candidates.add (l->product); return m; }
    // 3. the explicit vendor-model table (UAD-2 only), then the hardware bundle
    if (vendor.equalsIgnoreCase ("UAD-2"))
    {
        std::vector<const Line*> ex;
        for (const auto* l : gov) { const auto it = kExplicit().find (l->product); if (it == kExplicit().end()) continue; for (const auto& n : it->second) if (normalise (n) == key) { ex.push_back (l); break; } }
        if (ex.size() == 1) { m.matched = true; m.explicitTable = true; m.line = *ex.front(); m.how = "explicit vendor-model table (" + ex.front()->product + "): listed for review"; return m; }
        if (ex.size() > 1) { m.ambiguous = true; m.how = "ambiguous in the explicit table"; for (const auto* l : ex) m.candidates.add (l->product); return m; }
        for (const auto& n : kCs1Set()) if (normalise (n) == key) { m.matched = true; m.bundled = true; m.line = kBundledWithHardware(); m.how = "bundled with UAD-2 hardware (the CS-1 set)"; return m; }
    }
    m.how = "unmatched: no line's product, note or the explicit table names it"; return m;
}

// THE VERDICT on a date (YYYY-MM-DD)
struct Verdict { bool load = false, demo = false, outsideFile = false; juce::String state, reason, expires; };
inline Verdict verdictFor (const Match& m, const juce::String& runDate)
{
    Verdict v;
    // THE FILE'S SCOPE (Kathy, 7 Oct): "unmatched -> not loaded" applies ONLY to plugins of a vendor that appears in licences.csv. A plugin
    // of a vendor not in the file (Waves, Plugin Alliance ... when only UAD-2 lines exist) is outside the file and runs as before.
    if (! m.matched && ! m.ambiguous && m.how.startsWith ("no licence line governs")) { v.outsideFile = true; v.load = true; v.state = "outside_file"; v.reason = m.how; return v; }
    if (m.ambiguous) { v.state = "unmatched"; v.reason = "licence file: " + m.how + " (" + m.candidates.joinIntoString (" / ") + "): on licence_review.txt, not loaded until reviewed"; return v; }
    if (! m.matched) { v.state = "unmatched"; v.reason = "licence file: " + m.how + ": on licence_review.txt, not loaded until reviewed"; return v; }
    const auto& l = *m.line; v.state = l.state;
    if (l.state == "owned") { v.load = true; v.reason = "licence file: owned (" + l.product + ")"; return v; }
    if (l.state == "demo")
    {
        v.expires = l.demoEnd;
        if (l.demoEnd.isEmpty()) { v.state = "unmatched"; v.reason = "licence file: demo with no demo_end (" + l.product + "): on licence_review.txt, not loaded until reviewed"; return v; }
        if (l.demoEnd.compare (runDate) < 0) { v.state = "expired"; v.reason = "licence file: demo of " + l.product + " ended " + l.demoEnd + " (run date " + runDate + "): needs_licence, not loaded"; return v; }
        v.load = true; v.demo = true; v.reason = "licence file: demo of " + l.product + " until " + l.demoEnd + ": measured and stamped"; return v;
    }
    if (l.state == "expired" || l.state == "unowned") { v.reason = "licence file: " + l.state + " (" + l.product + (l.note.isNotEmpty() ? "; " + l.note : juce::String()) + "): needs_licence, not loaded"; return v; }
    v.state = "unmatched"; v.reason = "licence file: unknown state '" + l.state + "' on row " + juce::String (l.row) + ": on licence_review.txt, not loaded until reviewed"; return v;
}
inline juce::var stampVar (const Verdict& v) { auto* o = new juce::DynamicObject(); o->setProperty ("state", "demo"); o->setProperty ("expires", v.expires); return juce::var (o); }

// THE REVIEW SHEET: every governed plugin with its match, the unmatched and ambiguous first
struct Reviewed { juce::String plugin; Match match; Verdict verdict; };
inline juce::String reviewSheet (const std::vector<Reviewed>& rows, const juce::String& runDate, const juce::String& fileName)
{
    juce::String s; int unmatched = 0, amb = 0, expl = 0, bundled = 0, owned = 0, demo = 0, stopped = 0;
    int sibl = 0;
    for (const auto& r : rows) { if (r.match.ambiguous) ++amb; else if (! r.match.matched) ++unmatched; if (r.match.explicitTable) ++expl; if (r.match.siblingResolved) ++sibl; if (r.match.bundled) ++bundled; if (r.verdict.load && ! r.verdict.demo) ++owned; if (r.verdict.demo) ++demo; if (! r.verdict.load) ++stopped; }
    s << "LICENCE REVIEW (" << fileName << ", run date " << runDate << "): " << (int) rows.size() << " governed plugin(s): " << owned << " owned, " << demo << " demo (measured, stamped), " << stopped << " not loaded (" << unmatched << " unmatched, " << amb << " ambiguous, the rest expired / unowned); " << expl << " matched by the explicit table, " << sibl << " sibling-resolved, " << bundled << " bundled with the hardware\n\n";
    s << "UNMATCHED / AMBIGUOUS - not loaded until reviewed (add a line, a 'covers' note, or an explicit entry):\n";
    for (const auto& r : rows) if (! r.match.matched) s << "  " << r.plugin << ": " << r.match.how << (r.match.candidates.isEmpty() ? juce::String() : " -> " + r.match.candidates.joinIntoString (" / ")) << "\n";
    s << "\nEXPLICIT TABLE / SIBLING-RESOLVED (matched by rule, please confirm):\n";
    for (const auto& r : rows) if (r.match.explicitTable || r.match.siblingResolved) s << "  " << r.plugin << " -> " << r.match.line->product << " (" << r.match.line->state << (r.match.line->demoEnd.isNotEmpty() ? ", until " + r.match.line->demoEnd : juce::String()) << "): " << (r.match.explicitTable ? "explicit table" : "sibling-resolved") << "\n";
    s << "\nEVERY OTHER MATCH:\n";
    for (const auto& r : rows) if (r.match.matched && ! r.match.explicitTable && ! r.match.siblingResolved) s << "  " << r.plugin << " -> " << r.match.line->product << " (" << r.match.line->state << (r.match.line->demoEnd.isNotEmpty() ? ", until " + r.match.line->demoEnd : juce::String()) << "): " << r.match.how << " -> " << (r.verdict.load ? (r.verdict.demo ? "demo, measured and stamped" : "load") : "NOT loaded") << "\n";
    return s;
}

} // namespace ejmap::licence
