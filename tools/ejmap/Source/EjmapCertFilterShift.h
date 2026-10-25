/*
  EjmapCertFilterShift.h - THE FILTER PASS and THE PITCH-SHIFT PASS (Kathy, 22 Oct, calls 3 and 4). Included at the end of
  EjmapCertDriver.h (they use its probe calls, fixture, licence gate and drafts). Nothing exported, nothing published: a record under
  <out>/filter/ or <out>/pitchshift/ and a draft under <out>/drafts/, as every other Phase B mode.

  --cert-filter <product>      every cutoff control (EjmapFilter.h cutoffControls) at 7 positions (a stepped control: up to 12 of its
                               steps) on the EQ's grid; per position the -3 dB cutoff, the slope, the resonance; the cutoff judged
                               against the label (5 %, the EQ acceptance's frequency tolerance); a control certified on >= 3 judged
  --cert-pitchshift <product>  every shift control (EjmapShift.h shiftControls) at up to 7 labelled positions from a 25-point text grid;
                               per position a 220 Hz sine through it, the output's frequency read against the label; the mix set wet
                               for the reading (the probe's instance only)
*/

#pragma once

#include "EjmapFilter.h"
#include "EjmapShift.h"

namespace ejmap::cert
{

// THE FILTER READ of one control: its positions' shapes and label judgements, its verdict. `response` is the caller's --response call
// (ResponseCall: declared in EjmapCertDriver.h, before runEq)
struct FilterControlRead { juce::var row; filter::Verdict verdict; bool window = false; };
inline juce::StringArray filterNorms (const juce::var& base, int idx)
{
    const auto ctl = sweep::findControl (base, idx); juce::StringArray out;
    if (sweep::isSteppedControl (ctl))
    {   const int st = (int) ctl.getProperty ("numSteps", 0), n = juce::jmin (st, 12);
        for (int k = 0; k < n; ++k) out.add (juce::String ((float) std::lround ((double) k * (st - 1) / juce::jmax (1, n - 1)) / (float) juce::jmax (1, st - 1), 6)); }
    else for (int k = 0; k < 7; ++k) out.add (juce::String ((float) k / 6.0f, 6));
    return out;
}
inline FilterControlRead readFilterControl (const ResponseCall& response, const juce::var& base, int idx, const juce::String& name, const juce::String& tag,
                                            const std::function<void (const juce::String&)>& say)
{
    FilterControlRead fr; auto* o = new juce::DynamicObject(); o->setProperty ("index", idx); o->setProperty ("control", name);
    const auto ctl = sweep::findControl (base, idx); if (const auto u = ctl.getProperty ("unit", "").toString(); u.isNotEmpty()) o->setProperty ("unit", u);
    o->setProperty ("stepped", sweep::isSteppedControl (ctl));
    auto [r, cr] = response (tag, idx, filterNorms (base, idx), {});
    if (cr.kind == ChildResult::Kind::uiShown) { fr.window = true; fr.row = juce::var (o); return fr; }
    juce::Array<juce::var> ps; juce::StringArray types;
    if (! r.ok) o->setProperty ("refused", r.refused.isNotEmpty() ? r.refused : cr.describe());
    std::vector<std::vector<std::pair<double, double>>> tr; for (const auto& p : r.positions) tr.push_back (filter::transferOf (p));
    const auto unit = ctl.getProperty ("unit", "").toString();
    // A SHELF IS READ AS A SHELF (Kathy, 22 Oct, decision 8: LOVEND is a +5.7 dB low shelf, not a filter): when most positions are shelves
    // on the plain transfer, the control goes to the EQ's own shelf measurement (eq::deriveBand: the corner where the shelf is half its
    // plateau - filter::shelfCorner, the EQ's own rule - and its gain); the corner judged against the label at the same 5 %
    { int shelves = 0; for (const auto& t : tr) if (filter::deriveShape (t).result == "shelf") ++shelves;
      if (shelves >= 2 && shelves * 2 >= (int) tr.size())
      {
          o->setProperty ("measured_as", "shelf (the EQ shelf measurement): " + juce::String (shelves) + " of " + juce::String ((int) tr.size()) + " positions are shelves");
          std::vector<std::pair<filter::Shape, filter::Judged>> rows; juce::Array<juce::var> ps;
          for (size_t k = 0; k < r.positions.size(); ++k)
          {
              const auto& p = r.positions[k]; const auto sh = filter::deriveShape (tr[k]); const auto corner = filter::shelfCorner (tr[k], sh);
              double lo = 1e9; for (const auto& [hz, d] : tr[k]) lo = juce::jmin (lo, d);
              filter::Shape s0; s0.tones = (int) tr[k].size(); filter::Judged j;
              auto* pv = new juce::DynamicObject(); pv->setProperty ("norm", p.norm); pv->setProperty ("display", p.text); pv->setProperty ("result", corner ? juce::String ("shelf") : sh.result);
              if (corner)
              {   s0.result = "measured"; s0.cutoffHz = *corner; const auto shape = sh.type == "highpass" ? juce::String ("high_shelf") : juce::String ("low_shelf");
                  pv->setProperty ("shape", shape); pv->setProperty ("corner_hz", std::round (*corner * 10.0) / 10.0); pv->setProperty ("gain_db", std::round ((sh.passbandDb - lo) * 100.0) / 100.0);
                  if (const auto lab = filter::labelHz (p.text, unit, name))
                  { j.labelHz = *lab; j.judged = true; j.offPct = 100.0 * (*corner - *lab) / *lab; j.pass = std::abs (j.offPct) <= filter::kCutoffTolPct;
                    pv->setProperty ("label_hz", *lab); pv->setProperty ("off_pct", std::round (j.offPct * 10.0) / 10.0); pv->setProperty ("within_tolerance", j.pass); }
                  say ("    [" + juce::String (idx) + "] " + name + " '" + p.text + "': " + shape + " corner " + juce::String (*corner, 1) + " Hz, " + juce::String (sh.passbandDb - lo, 2) + " dB"
                       + (j.judged ? "; " + juce::String (j.offPct, 1) + " % off the label" : juce::String())); }
              else { s0.result = sh.result; pv->setProperty ("reason", sh.reason); say ("    [" + juce::String (idx) + "] " + name + " '" + p.text + "': " + sh.result + " (" + sh.reason + ")"); }
              rows.push_back ({ s0, j }); ps.add (juce::var (pv));
          }
          fr.verdict = filter::controlVerdict (rows); if (fr.verdict.verdict == "certified") fr.verdict.convention = "shelf corner";
          o->setProperty ("type", "shelf"); o->setProperty ("positions", ps); o->setProperty ("verdict", fr.verdict.verdict); o->setProperty ("why", fr.verdict.why);
          if (fr.verdict.convention.isNotEmpty()) o->setProperty ("convention", fr.verdict.convention);
          o->setProperty ("tolerance_pct", filter::kCutoffTolPct);
          say ("    [" + juce::String (idx) + "] " + name + ": " + fr.verdict.verdict + " (as a shelf) - " + fr.verdict.why);
          fr.row = juce::var (o); return fr;
      } }
    const int open = filter::openPosition (tr);   // 22 Oct: each position against the control's most open one
    if (open >= 0) { auto* b = new juce::DynamicObject(); b->setProperty ("norm", r.positions[(size_t) open].norm); b->setProperty ("display", r.positions[(size_t) open].text);
                     b->setProperty ("why", "the most open position: every other is read against it, so what this control does not move cancels"); o->setProperty ("open_position", juce::var (b)); }
    std::vector<filter::Positioned> pos;
    for (size_t k = 0; k < r.positions.size(); ++k)
    {
        const auto& p = r.positions[k];
        if ((int) k == open) { filter::Shape s0; s0.result = "open"; s0.reason = "the most open position (the reference)"; s0.tones = (int) tr[k].size(); pos.push_back ({ s0, p.text }); ps.add (filter::shapeVar (s0, {}, p.norm, p.text)); continue; }
        const auto s = filter::deriveShape (open >= 0 ? filter::lessOpen (tr[k], tr[(size_t) open]) : tr[k]); const auto j = filter::judgeCutoff (s, p.text, unit, name);
        pos.push_back ({ s, p.text }); ps.add (filter::shapeVar (s, j, p.norm, p.text)); if (s.type.isNotEmpty()) types.add (s.type);
        say ("    [" + juce::String (idx) + "] " + name + " '" + p.text + "': " + s.result + (s.result == "measured" ? " " + s.type + " -3 dB " + juce::String (s.cutoffHz, 1) + " Hz"
             + (s.slopeBasis.isNotEmpty() ? ", " + juce::String (s.slopeDbOct, 1) + " dB/oct" : juce::String()) + (s.resonanceDb > 0.0 ? ", resonance +" + juce::String (s.resonanceDb, 1) + " dB at " + juce::String (s.resonanceHz, 0) + " Hz, natural ~" + juce::String (s.naturalHz, 0) + " Hz" : juce::String()) : " (" + s.reason + ")")
             + (j.judged ? "; " + j.why : juce::String()));
    }
    fr.verdict = filter::conventionVerdict (pos, unit, name);   // 22 Oct (decision 3): the one convention the label follows, if one holds everywhere
    juce::String type; int best = 0; for (const auto& t : { juce::String ("highpass"), juce::String ("lowpass") }) { int c = 0; for (const auto& x : types) if (x == t) ++c; if (c > best) { best = c; type = t; } }
    if (type.isNotEmpty()) o->setProperty ("type", type);
    o->setProperty ("positions", ps); o->setProperty ("verdict", fr.verdict.verdict); o->setProperty ("why", fr.verdict.why); o->setProperty ("tolerance_pct", filter::kCutoffTolPct);
    if (fr.verdict.convention.isNotEmpty()) o->setProperty ("convention", fr.verdict.convention);   // which point the label names, recorded in the draft
    say ("    [" + juce::String (idx) + "] " + name + ": " + fr.verdict.verdict + " - " + fr.verdict.why);
    fr.row = juce::var (o); return fr;
}

// THE EQ'S HPF / LPF BANDS (call 3): each read as a filter control; `measured` counts those that read a cutoff
inline juce::Array<juce::var> readEqPassBands (const ResponseCall& response, const juce::var& base, const std::vector<std::pair<int, juce::String>>& controls,
                                               const std::function<void (const juce::String&)>& say, bool& window, int& measured)
{
    juce::Array<juce::var> rows; window = false; measured = 0;
    const auto pb = filter::eqPassBands (controls);
    if (! pb.empty()) say ("EQ: " + juce::String ((int) pb.size()) + " high / low pass band(s) with no gain control: read by the filter measurement");
    for (const auto& [idx, name] : pb)
    {
        const auto fr = readFilterControl (response, base, idx, name, "f" + juce::String (idx), say);
        if (fr.window) { window = true; break; }
        rows.add (fr.row); if (fr.verdict.verdict != "not_measured") ++measured;
    }
    return rows;
}

inline int runFilter (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("FILTER: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedPlugins()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("FILTER: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("FILTER: " + opt.product + " - " + gate.stop); return kToneLicenceKnownExit; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("filter"); outDir.createDirectory();
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false);
    const auto stem = fmtTag (desc) + "_" + uidHex + "_" + desc.version;
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "filter", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash,
                                    { "cutoff", "freq", "frequency", "filter", "hp", "lp", "hpf", "lpf", "highpass", "lowpass", "sweep", "hz", "khz" });
    if (! fx.ok) { say ("FILTER: " + fx.why); return 1; }
    say ("FILTER: " + fx.note);
    std::vector<std::pair<int, juce::String>> controls;
    if (const auto* cs = fx.base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) controls.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", "").toString() });
    const ResponseCall response = [&] (const juce::String& tag, int ctl, const juce::StringArray& norms, const juce::StringArray& sets)
    {
        juce::StringArray a { "--response", "ctl=" + juce::String (ctl), "norms=" + norms.joinIntoString (","), "tones=" + juce::String (eq::kGridTones), "lo=" + juce::String (eq::kGridLoHz, 0),
                              "hi=" + juce::String (eq::kGridHiHz, 0), "db=" + juce::String (eq::kLevelDbfs, 0), "hold=" + juce::String (eq::kHoldS, 1), "discard=" + juce::String (eq::kDiscardS, 1) };
        if (! sets.isEmpty()) a.add ("set=" + sets.joinIntoString (","));
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (a);
        const auto r = runProbeCall (args, opt.timeoutMs); raw.getChildFile (stem + ".filter." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n");
        return std::make_pair (eq::parseResponse (r.cleanExit() ? r.out : juce::String ("refused " + r.describe())), r);
    };
    const auto cut = filter::cutoffControls (controls);
    say ("FILTER: " + opt.product + " " + desc.version + ": " + juce::String ((int) cut.size()) + " cutoff control(s) by name");
    juce::Array<juce::var> rows; int measured = 0, certified = 0;
    for (const auto& [idx, name] : cut)
    {
        const auto fr = readFilterControl (response, fx.base, idx, name, "c" + juce::String (idx), say);
        if (fr.window) { say ("FILTER: a window appeared; stopping"); return 5; }
        rows.add (fr.row); if (fr.verdict.verdict != "not_measured") ++measured; if (fr.verdict.verdict == "certified") ++certified;
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_filter_record/0"); o->setProperty ("status", "PROTOTYPE (Kathy, 22 Oct): not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", fmtTag (desc) + "|" + uidHex + "|" + desc.version);
    o->setProperty ("grid", eq::gridDescription()); o->setProperty ("method", "the transfer (out - in) at each tone; cutoff = the -3 dB point from the passband; slope = dB/oct between the -6 and -18 dB points (-12 when -18 is not reached); resonance = the peak over the passband");
    o->setProperty ("controls", rows); o->setProperty ("measuredAt", nowStamp());
    if (cut.empty()) { juce::StringArray names; for (const auto& [i, n] : controls) names.add (n); o->setProperty ("note", "no control named for a cutoff: " + names.joinIntoString (", ")); }
    const juce::var rec (o);
    outDir.getChildFile (stem + ".filter.json").replaceWithText (juce::JSON::toString (rec) + "\n", false, false, "\n");
    say ("FILTER: " + juce::String (measured) + " of " + juce::String ((int) cut.size()) + " control(s) measured, " + juce::String (certified) + " certified -> " + outDir.getChildFile (stem + ".filter.json").getFullPathName());
    if (measured > 0)
    {
        auto* d = new juce::DynamicObject(); d->setProperty ("schema", "ej_filter_profile/1"); d->setProperty ("category", "filter"); d->setProperty ("spec", drafts::specTag ("FILTER_PROFILE_SPEC")); d->setProperty ("status", drafts::statusLine ("FILTER_PROFILE_SPEC"));   // v1.1: the filter category's draft
        d->setProperty ("plugin", drafts::pluginBlock (opt.product, desc.manufacturerName, uidHex, desc.version, fx.base.getProperty ("map_fp", juce::var())));
        d->setProperty ("measured", drafts::measuredBlock ("EJ Map (feat/ejmap-wizard), probe " + id.cdhash.substring (0, 12), runDateIso(), 48000, eq::gridDescription()));
        juce::Array<juce::var> fs; for (const auto& r : rows) if (r.getProperty ("verdict", "").toString() != "not_measured") fs.add (r);
        d->setProperty ("filters", fs);
        const auto f = drafts::draftFile (opt.out, stem, "filter_profile"); const auto problem = drafts::writeDraft (f, juce::var (d));
        say (problem.isEmpty() ? "FILTER: draft ej_filter_profile/1 -> " + f.getFullPathName() : "FILTER: " + problem);
    }
    return measured > 0 ? 0 : 4;
}

inline int runPitchShift (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("SHIFT: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedPlugins()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("SHIFT: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("SHIFT: " + opt.product + " - " + gate.stop); return kToneLicenceKnownExit; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("pitchshift"); outDir.createDirectory();
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false);
    const auto stem = fmtTag (desc) + "_" + uidHex + "_" + desc.version;
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "pitchshift", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash,
                                    { "pitch", "shift", "semi", "semitone", "semitones", "transpose", "coarse", "fine", "cents", "cent", "interval", "octave", "freq", "frequency", "mix", "wet", "dry", "blend" });
    if (! fx.ok) { say ("SHIFT: " + fx.why); return 1; }
    say ("SHIFT: " + fx.note);
    std::vector<std::pair<int, juce::String>> controls;
    if (const auto* cs = fx.base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) controls.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", "").toString() });
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra)
    {
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runProbeCall (args, opt.timeoutMs); raw.getChildFile (stem + ".pitchshift." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r;
    };
    const auto shiftCtls = shift::shiftControls (controls);
    const auto wet = shift::wetWrite (controls);
    say ("SHIFT: " + opt.product + " " + desc.version + ": " + juce::String ((int) shiftCtls.size()) + " shift control(s) by name"
         + (wet ? "; the mix [" + juce::String (wet->first) + "] set to " + juce::String (wet->second, 1) + " for the reading (the probe's instance only)" : juce::String ("; no mix control")));
    juce::Array<juce::var> rows; int measured = 0, agreed = 0;
    for (const auto& [idx, name] : shiftCtls)
    {
        auto* o = new juce::DynamicObject(); o->setProperty ("index", idx); o->setProperty ("control", name);
        const auto unit = sweep::findControl (fx.base, idx).getProperty ("unit", "").toString(); if (unit.isNotEmpty()) o->setProperty ("unit", unit);
        juce::StringArray grid; for (int k = 0; k <= 24; ++k) grid.add (juce::String ((float) k / 24.0f, 6));
        const auto tg = run ("c" + juce::String (idx) + ".textgrid", { "--text-at-norms", juce::String (idx), grid.joinIntoString (",") });
        if (tg.kind == ChildResult::Kind::uiShown) { say ("SHIFT: a window appeared; stopping"); return 5; }
        std::vector<std::pair<float, juce::String>> labelled; for (const auto& r : pitch::parseTextGrid (tg.cleanExit() ? tg.out : juce::String())) if (r.landed) labelled.push_back ({ (float) r.norm, r.text });
        juce::StringArray texts; for (const auto& l : labelled) texts.add (l.second);
        const auto kind = shift::labelKind (texts, unit, name); o->setProperty ("labels", kind);
        // 22 Oct: the measuring writes for THIS control - the mix wet, the other voices' and the direct level at their minimum
        juce::StringArray sets; juce::Array<juce::var> mw;
        auto addSet = [&] (int i, float n, const juce::String& why) { if (i == idx) return; sets.add (juce::String (i) + ":" + juce::String (n, 6));
                                                                   auto* w = new juce::DynamicObject(); w->setProperty ("index", i); w->setProperty ("norm", n); w->setProperty ("why", why); mw.add (juce::var (w)); };
        if (wet) addSet (wet->first, wet->second, "the mix fully wet");
        const auto iso = shift::isolation (controls, name);
        for (const auto& [i, n] : iso.writes) addSet (i, n, n > 0.5f ? "its own voice switched on for the read" : "another voice / the direct signal off or at its minimum: one pitch at a time");
        if (! mw.isEmpty()) o->setProperty ("measuring_writes", mw);
        if (! iso.dependsOn.isEmpty())   // 22 Oct (decision 5): what the intervals depend on - the switch written on for the read
        { auto* d = new juce::DynamicObject(); d->setProperty ("control", iso.dependsOn[0]); d->setProperty ("display", "On"); d->setProperty ("why", "the voice sounds only with its own switch on (off as instantiated)"); o->setProperty ("depends_on", juce::var (d)); }
        auto pick = shift::pickPositions (labelled, unit, name, kind);
        const bool unlabelled = pick.empty();
        if (unlabelled) for (int k = 0; k < 7; ++k) pick.push_back ({ (float) k / 6.0f, {} });   // 22 Oct: no shift in its labels - read where it goes, judge nothing
        say ("  [" + juce::String (idx) + "] " + name + ": " + juce::String ((int) labelled.size()) + " label(s) read (" + kind + "), "
             + (unlabelled ? juce::String ("no shift in them: 7 unlabelled positions") : juce::String ((int) pick.size()) + " position(s) with a shift to read") + (sets.isEmpty() ? juce::String() : "; writes " + sets.joinIntoString (",")));
        juce::Array<juce::var> ps; std::vector<shift::Reading> rs;
        for (const auto& [norm, text] : pick)
        {
            shift::Expect e; if (unlabelled) { e.ok = true; e.outHz = shift::kInHz; e.unit = "none"; } else e = shift::expected (text, unit, name, kind);
            const auto [note, detune] = shift::noteAndDetune (e);
            juce::StringArray a { "--sweep-pitch", "ctl=" + juce::String (idx), "norms=" + juce::String (norm, 6), "gen=static", "note=" + juce::String (note, 4), "cents=" + juce::String (detune, 2), "hold=2", "db=-18" };
            if (! sets.isEmpty()) a.add ("sets=" + sets.joinIntoString (","));
            const auto pr = run ("c" + juce::String (idx) + ".n" + juce::String (norm, 3), a);
            if (pr.kind == ChildResult::Kind::uiShown) { say ("SHIFT: a window appeared; stopping"); return 5; }
            const auto m = pitch::parsePitch (pr.cleanExit() ? pr.out : juce::String ("refused " + pr.describe()));
            shift::Reading r; if (! m.ok || m.positions.empty()) r.why = "the probe read nothing (" + (m.refused.isNotEmpty() ? m.refused : pr.describe()) + ")"; else r = shift::readPosition (m.positions.front(), e);
            const auto shown = text.isNotEmpty() ? text : (m.positions.empty() ? juce::String() : m.positions.front().text);
            if (unlabelled && r.read) r.why = "out " + juce::String (r.outHz, 2) + " Hz: a shift of " + juce::String (r.shiftCents / 100.0, 2) + " st (unlabelled)";
            rs.push_back (r); auto rv = shift::readingVar (r, e, norm, shown); if (unlabelled) if (auto* ro = rv.getDynamicObject()) { ro->removeProperty ("expected_out_hz"); ro->removeProperty ("expected_cents"); ro->removeProperty ("agrees"); ro->removeProperty ("error_cents"); }
            ps.add (rv);
            say ("    '" + shown + "'" + (unlabelled ? juce::String() : " (expects " + juce::String (e.outHz, 2) + " Hz)") + ": " + r.why);
        }
        shift::Verdict v;
        if (unlabelled) { const auto u = shift::unlabelledVerdict (rs); v.verdict = u.verdict; v.why = u.why; }
        else v = shift::controlVerdict (rs);
        o->setProperty ("positions", ps); o->setProperty ("verdict", v.verdict); o->setProperty ("why", v.why); o->setProperty ("tolerance_cents", shift::kAgreeCents);
        say ("  [" + juce::String (idx) + "] " + name + ": " + v.verdict + " - " + v.why);
        rows.add (juce::var (o)); if (v.verdict != "not_measured") ++measured; if (v.verdict == "agrees") ++agreed;
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_pitchshift_record/0"); o->setProperty ("status", "PROTOTYPE (Kathy, 22 Oct): not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", fmtTag (desc) + "|" + uidHex + "|" + desc.version);
    o->setProperty ("method", "a 220 Hz sine at -18 dBFS, 2 s per position; the probe's detector centred on the frequency the label expects (input detuned back to 220 Hz); the median of the readable windows after 0.5 s is the error against the label");
    if (wet) { auto* w = new juce::DynamicObject(); w->setProperty ("index", wet->first); w->setProperty ("norm", wet->second); w->setProperty ("why", "the mix fully wet for the reading (the probe's instance only)"); o->setProperty ("measuring_writes", juce::Array<juce::var> { juce::var (w) }); }
    o->setProperty ("controls", rows); o->setProperty ("measuredAt", nowStamp());
    if (shiftCtls.empty()) { juce::StringArray names; for (const auto& [i, n] : controls) names.add (n); o->setProperty ("note", "no control named for a shift: " + names.joinIntoString (", ")); }
    const juce::var rec (o);
    outDir.getChildFile (stem + ".pitchshift.json").replaceWithText (juce::JSON::toString (rec) + "\n", false, false, "\n");
    say ("SHIFT: " + juce::String (measured) + " of " + juce::String ((int) shiftCtls.size()) + " control(s) measured, " + juce::String (agreed) + " agree with their labels -> " + outDir.getChildFile (stem + ".pitchshift.json").getFullPathName());
    if (measured > 0)
    {
        auto* d = new juce::DynamicObject(); d->setProperty ("schema", "ej_pitch_profile/1"); d->setProperty ("category", "pitch"); d->setProperty ("pitch_measurement", "shift");   /* 27 Oct (B): was "measurement", which the result's measurement identity block shares */ d->setProperty ("spec", drafts::specTag ("PITCHSHIFT_PROFILE_SPEC")); d->setProperty ("status", drafts::statusLine ("PITCHSHIFT_PROFILE_SPEC"));   // v1.1: a measurement within pitch
        d->setProperty ("plugin", drafts::pluginBlock (opt.product, desc.manufacturerName, uidHex, desc.version, fx.base.getProperty ("map_fp", juce::var())));
        d->setProperty ("measured", drafts::measuredBlock ("EJ Map (feat/ejmap-wizard), probe " + id.cdhash.substring (0, 12), runDateIso(), 48000, "220 Hz sine, -18 dBFS"));
        juce::Array<juce::var> cs; for (const auto& r : rows) if (r.getProperty ("verdict", "").toString() != "not_measured") cs.add (r);
        d->setProperty ("shifts", cs);
        const auto f = drafts::draftFile (opt.out, stem, "pitch_profile"); const auto problem = drafts::writeDraft (f, juce::var (d));
        say (problem.isEmpty() ? "SHIFT: draft ej_pitch_profile/1 (measurement: shift) -> " + f.getFullPathName() : "SHIFT: " + problem);
    }
    return measured > 0 ? 0 : 4;
}

} // namespace ejmap::cert
