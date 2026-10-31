/*
  EjmapCertModulation.h - THE MODULATION PASS (weekend item 1, 31 Oct 2026, Kathy). Included at the end of EjmapCertDriver.h (its probe
  calls, fixture, licence gate and drafts). Nothing exported, nothing published: a record under <out>/modulation/ and a draft under
  <out>/drafts/, as every other Phase B mode. The derivations are EjmapModulation.h's.

  --cert-modulation <product>
    1. every control a rate name proposes, at 7 positions (a stepped control: up to 12 of its steps), a steady tone at 300 Hz and at
       2 kHz (one probe call each), 12 s per position, the level read in 5 ms windows; a position with no period re-read at 42 s;
    2. no level wobble anywhere: the pitch read instead (440 Hz, --sweep-pitch, 10.7 ms hop) - a vibrato;
    3. the rate control is the candidate whose positions move the rate monotonically over 3x or more (names propose, measurement decides);
    4. each position judged against its label at 120 bpm (a note value) within 5 %;
    5. THE ACCEPTANCE (held out): a continuous control written at two rates between measured positions; a note-valued one read at 90 bpm
       at two of its positions; each within 5 %;
    6. the depth control (named), at 5 positions at the rate control's own setting: the wobble's spread in dB (or cents), recorded;
    7. the mix and feedback controls named: recorded, not measured (v1).
  The probe's instance gets the depth at 0.75 for the rate reading (a wobble large enough to time); every write is said in the record.
*/

#pragma once

#include "EjmapModulation.h"

namespace ejmap::cert
{
namespace modrun
{
// the --sweep output's positions: per position k its norm, text and the level windows of its hold
struct Pos { int k = -1; float norm = 0.0f; juce::String text; bool landed = true; std::vector<double> win; };
inline std::vector<Pos> parseWindows (const juce::String& out)
{
    std::vector<Pos> ps; std::map<int, size_t> at;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", ""); if (f.size() < 3) continue;
        if (f[0] == "pos") { Pos p; p.k = f[1].getIntValue(); p.norm = (float) kv (f, 2, "norm").getDoubleValue(); p.text = kv (f, 2, "text"); p.landed = ! f.contains ("write_unlanded"); at[p.k] = ps.size(); ps.push_back (p); }
        else if ((f[0] == "hold" || f[0] == "rerender") && at.count (f[1].getIntValue())) ps[at[f[1].getIntValue()]].win = modulation::parseList (kv (f, 3, "win"));
    }
    return ps;
}
} // namespace modrun

inline int runModulation (const SweepOptions& opt)
{
    using namespace modulation;
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("MOD: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedPlugins()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("MOD: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("MOD: " + opt.product + " - " + gate.stop); return kToneLicenceKnownExit; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("modulation"); outDir.createDirectory();
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false);
    const auto stem = fmtTag (desc) + "_" + uidHex + "_" + desc.version;
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "modulation", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash,
                                    { "rate", "speed", "lfo", "freq", "frequency", "depth", "intensity", "amount", "width", "mod", "mix", "wet", "dry", "blend", "feedback", "regen", "resonance", "sync" });
    if (! fx.ok) { say ("MOD: " + fx.why); return 1; }
    say ("MOD: " + fx.note);
    std::vector<std::pair<int, juce::String>> controls;
    if (const auto* cs = fx.base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) controls.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", "").toString() });
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra)
    {
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runProbeCall (args, juce::jmax (opt.timeoutMs, 600000)); raw.getChildFile (stem + ".modulation." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r;
    };
    std::vector<std::pair<int, juce::String>> rateC, depthC, mixC, fbC, syncC;
    for (const auto& c : controls) { if (syncNamed (c.second)) syncC.push_back (c); else if (rateNamed (c.second)) rateC.push_back (c); else if (depthNamed (c.second)) depthC.push_back (c); else if (mixNamed (c.second)) mixC.push_back (c); else if (feedbackNamed (c.second)) fbC.push_back (c); }
    juce::StringArray sets; juce::Array<juce::var> writes;
    if (! depthC.empty()) { sets.add (juce::String (depthC.front().first) + ":0.75"); auto* w = new juce::DynamicObject(); w->setProperty ("index", depthC.front().first); w->setProperty ("control", depthC.front().second); w->setProperty ("norm", 0.75); w->setProperty ("why", "the depth at 0.75 for the rate reading: a wobble large enough to time (the probe's instance only)"); writes.add (juce::var (w)); }
    // 31 Oct: ONE LFO AT A TIME - a rate control's paired depth (lfoKey) up, every other paired depth at 0 (replaces the one write above)
    auto lfoSets = [&] (int rateIdx, const juce::String& rateName) -> juce::StringArray
    {
        const auto key = lfoKey (rateName, true); bool paired = false; juce::StringArray out;
        for (const auto& [di, dn] : depthC) if (key.isNotEmpty() && lfoKey (dn, false) == key) paired = true;
        if (! paired) return sets;
        for (const auto& [di, dn] : depthC) if (lfoKey (dn, false).isNotEmpty()) out.add (juce::String (di) + (lfoKey (dn, false) == key ? ":0.75" : ":0.0"));
        (void) rateIdx; return out;
    };
    auto normsFor = [&] (int idx)
    {
        const auto ctl = sweep::findControl (fx.base, idx); juce::StringArray out;
        if (sweep::isSteppedControl (ctl)) { const int st = (int) ctl.getProperty ("numSteps", 0), n = juce::jmin (st, 12);
            for (int k = 0; k < n; ++k) out.add (juce::String ((float) std::lround ((double) k * (st - 1) / juce::jmax (1, n - 1)) / (float) juce::jmax (1, st - 1), 6)); }
        else for (int k = 0; k < 7; ++k) out.add (juce::String ((float) k / 6.0f, 6));
        return out;
    };
    // ONE READING of a control at some norms: both tones, the level windows; positions with no period re-read with the long hold; then,
    // if nothing wobbled in level, the pitch. tempo: the playhead's bpm
    struct Read { std::vector<RatePoint> pts; std::vector<double> spread; juce::String via = "level"; bool window = false, silent = false; };
    auto readRates = [&] (int idx, const juce::StringArray& norms, double bpm, const juce::String& tagBase, const juce::StringArray& extraSets) -> Read
    {
        Read rd; juce::StringArray allSets = sets; allSets.addArray (extraSets); allSets.removeEmptyStrings();
        std::map<float, std::vector<std::vector<double>>> series; std::map<float, juce::String> texts;
        auto sweepAt = [&] (const juce::StringArray& ns, double hold, const juce::String& tag)
        {
            for (double hz : kTonesHz)
            {
                juce::StringArray a { "--sweep", "thr=" + juce::String (idx), "norms=" + ns.joinIntoString (","), "levels=" + juce::String (kLevelDbfs, 1), "hz=" + juce::String (hz, 1),
                                      "hold=" + juce::String (hold, 1), "discard=" + juce::String (kDiscardS, 1), "win=" + juce::String (kWinS, 4), "ref=0", "moving_db=999", "tempo=" + juce::String (bpm, 1) };
                if (! allSets.isEmpty()) a.add ("set=" + allSets.joinIntoString (","));
                const auto r = run (tag + ".t" + juce::String ((int) hz), a);
                if (r.kind == ChildResult::Kind::uiShown) { rd.window = true; return; }
                for (const auto& p : modrun::parseWindows (r.cleanExit() ? r.out : juce::String()))
                    if (p.landed && ! p.win.empty()) { auto& v = series[p.norm]; v.push_back (afterDiscard (p.win, kWinS, kDiscardS)); if (p.text.isNotEmpty()) texts[p.norm] = p.text; }
            }
        };
        sweepAt (norms, kHoldS, tagBase);
        if (rd.window) return rd;
        {   bool any = series.empty() ? false : true, sound = false;   // 31 Oct (bx_bluechorus2): a silent output is said as silent, never "no rate"
            for (const auto& [n, vs] : series) for (const auto& v : vs) for (double x : v) if (x > -200.0) { sound = true; break; }
            if (any && ! sound) { rd.silent = true; return rd; } }
        auto periodAt = [&] (float n) { return series.count (n) ? periodOf (series[n], kWinS) : Period {}; };
        juce::StringArray again; for (const auto& ns : norms) { const float n = ns.getFloatValue(); const auto p = periodAt (n); double sp = 0.0; for (const auto& v : series[n]) sp = std::max (sp, spreadOf (v)); if (! p.ok && sp >= kMinWobbleDb) again.add (ns); }
        if (! again.isEmpty()) { for (const auto& ns : again) series.erase (ns.getFloatValue()); sweepAt (again, kLongHoldS, tagBase + ".long"); if (rd.window) return rd; }
        bool anyWobble = false;
        for (const auto& ns : norms)
        {   const float n = ns.getFloatValue(); double sp = 0.0; for (const auto& v : series[n]) sp = std::max (sp, spreadOf (v));
            RatePoint pt; pt.norm = n; pt.display = texts[n]; pt.period = sp >= kMinWobbleDb ? periodAt (n) : Period { false, 0, 0, 0, "the level does not wobble (" + juce::String (sp, 2) + " dB)" };
            rd.pts.push_back (pt); rd.spread.push_back (sp); if (sp >= kMinWobbleDb) anyWobble = true; }
        if (anyWobble) return rd;
        // THE PITCH (a vibrato: all wet, no dry - the level never moves)
        rd.via = "pitch"; rd.pts.clear(); rd.spread.clear();
        juce::StringArray a { "--sweep-pitch", "ctl=" + juce::String (idx), "norms=" + norms.joinIntoString (","), "gen=static", "note=" + juce::String (kPitchNoteHz, 1), "cents=0", "hold=" + juce::String (kHoldS, 1), "db=" + juce::String (kLevelDbfs, 1) };
        if (! allSets.isEmpty()) a.add ("sets=" + allSets.joinIntoString (","));
        const auto r = run (tagBase + ".pitch", a);
        if (r.kind == ChildResult::Kind::uiShown) { rd.window = true; return rd; }
        const auto m = pitch::parsePitch (r.cleanExit() ? r.out : juce::String());
        for (const auto& p : m.positions)
        {
            std::vector<double> c; double dt = 0.0, lastT = -1.0;
            for (const auto& w : p.windows) if (w.tMs >= kDiscardS * 1000.0 && w.outConf >= 0.5) { c.push_back (w.outC); if (lastT >= 0.0 && dt == 0.0) dt = (w.tMs - lastT) / 1000.0; lastT = w.tMs; }
            const double sp = spreadOf (c);
            RatePoint pt; pt.norm = p.norm; pt.display = p.text;
            pt.period = sp >= kMinWobbleCents && dt > 0.0 ? periodOf ({ c }, dt) : Period { false, 0, 0, 0, "the pitch does not wobble (" + juce::String (sp, 1) + " cents)" };
            rd.pts.push_back (pt); rd.spread.push_back (sp);
        }
        return rd;
    };
    auto pointsVar = [&] (const Read& rd, double bpm, int* judged = nullptr, int* passed = nullptr, const juce::String& unit = {})
    {
        juce::Array<juce::var> rows;
        for (size_t i = 0; i < rd.pts.size(); ++i)
        {
            const auto& p = rd.pts[i]; auto* o = new juce::DynamicObject(); o->setProperty ("norm", p.norm); o->setProperty ("display", p.display);
            o->setProperty (rd.via == "pitch" ? "wobble_cents" : "wobble_db", std::round (rd.spread[i] * 100.0) / 100.0);
            if (p.period.ok) { o->setProperty ("rate_hz", std::round (p.period.rateHz * 10000.0) / 10000.0); o->setProperty ("correlation", std::round (p.period.strength * 100.0) / 100.0); }
            else o->setProperty ("why", p.period.why);
            const auto lab = rateLabel (p.display, unit, bpm);
            if (lab.ok) { o->setProperty ("label_hz", std::round (lab.hz * 10000.0) / 10000.0); o->setProperty ("label_kind", lab.kind);
                          if (p.period.ok) { const auto j = judgeRate (p.period.rateHz, lab); o->setProperty ("off_pct", std::round (j.offPct * 10.0) / 10.0); o->setProperty ("within_tolerance", j.pass);
                                             if (judged) ++*judged; if (passed && j.pass) ++*passed; } }
            rows.add (juce::var (o));
        }
        return rows;
    };

    say ("MOD: " + opt.product + " " + desc.version + ": rate candidate(s) " + juce::String ((int) rateC.size()) + ", depth " + juce::String ((int) depthC.size()) + ", mix " + juce::String ((int) mixC.size())
         + ", feedback " + juce::String ((int) fbC.size()) + ", sync " + juce::String ((int) syncC.size()) + (sets.isEmpty() ? juce::String() : "; writes " + sets.joinIntoString (",")));
    juce::Array<juce::var> candRows; juce::var rateBlock; int rateIdx = -1; Read rateRead; juce::String rateUnit; juce::StringArray rateSets;
    for (const auto& [idx, name] : rateC)
    {
        const auto unit = sweep::findControl (fx.base, idx).getProperty ("unit", "").toString();
        const auto own = lfoSets (idx, name); const auto savedSets = sets; sets = own;
        const auto rd = readRates (idx, normsFor (idx), kTempoBpm, "c" + juce::String (idx), {});
        sets = savedSets;
        if (rd.window) { say ("MOD: a window appeared; stopping"); return 5; }
        if (rd.silent) { say ("MOD: " + opt.product + ": the output is silent at every position (a demo or licence state?) - nothing to read"); return 4; }
        juce::String why; const bool moves = movesRate (rd.pts, why);
        say ("  [" + juce::String (idx) + "] " + name + " (" + rd.via + "): " + why + (moves ? " -> the rate control" : ""));
        auto* c = new juce::DynamicObject(); c->setProperty ("index", idx); c->setProperty ("control", name); c->setProperty ("read_via", rd.via); c->setProperty ("moves_rate", moves); c->setProperty ("why", why);
        c->setProperty ("positions", pointsVar (rd, kTempoBpm, nullptr, nullptr, unit)); candRows.add (juce::var (c));
        if (moves && rateIdx < 0) { rateIdx = idx; rateRead = rd; rateUnit = unit; rateSets = own; }
    }
    int judged = 0, passed = 0, accRan = 0, accPassed = 0;
    if (rateIdx >= 0)
    {
        auto* rb = new juce::DynamicObject(); juce::String rateName; for (const auto& c : controls) if (c.first == rateIdx) rateName = c.second;
        rb->setProperty ("index", rateIdx); rb->setProperty ("control", rateName); rb->setProperty ("read_via", rateRead.via); rb->setProperty ("tempo_bpm", kTempoBpm);
        rb->setProperty ("map", pointsVar (rateRead, kTempoBpm, &judged, &passed, rateUnit));
        bool noteValued = false; for (const auto& p : rateRead.pts) if (rateLabel (p.display, rateUnit, kTempoBpm).kind == "note") noteValued = true;
        rb->setProperty ("unit", noteValued ? "note" : "hz");
        rb->setProperty ("labels_judged", judged); rb->setProperty ("labels_within", passed); rb->setProperty ("tolerance_pct", kRateTolPct);
        // THE ACCEPTANCE, held out
        juce::Array<juce::var> acc;
        if (noteValued)
        {   // two of its own note values, read at 90 bpm: the label at 90 says what the rate must be
            std::vector<const RatePoint*> pick; for (const auto& p : rateRead.pts) if (p.period.ok && rateLabel (p.display, rateUnit, kAcceptTempoBpm).ok) pick.push_back (&p);
            if (pick.size() > 2) pick = { pick[pick.size() / 3], pick[(2 * pick.size()) / 3] };
            juce::StringArray ns; for (const auto* p : pick) ns.add (juce::String (p->norm, 6));
            if (! ns.isEmpty())
            {   const auto keepS = sets; sets = rateSets; const auto rd = readRates (rateIdx, ns, kAcceptTempoBpm, "acc90", {}); sets = keepS;
                if (rd.window) { say ("MOD: a window appeared; stopping"); return 5; }
                for (const auto& p : rd.pts) { const auto lab = rateLabel (p.display, rateUnit, kAcceptTempoBpm); auto j = judgeAcceptance (lab.hz, p.period.ok ? p.period.rateHz : 0.0);
                    auto* a = new juce::DynamicObject(); a->setProperty ("written_norm", p.norm); a->setProperty ("display", p.display); a->setProperty ("tempo_bpm", kAcceptTempoBpm); a->setProperty ("target_hz", lab.hz);
                    a->setProperty ("ran", j.judged); a->setProperty ("pass", j.pass); a->setProperty ("why", j.why); if (j.judged) { a->setProperty ("measured_hz", p.period.rateHz); ++accRan; if (j.pass) ++accPassed; } acc.add (juce::var (a));
                    say ("    acceptance at 90 bpm, '" + p.display + "': " + j.why + (j.judged ? (j.pass ? " PASS" : " FAIL") : juce::String())); } }
        }
        else if (! sweep::isSteppedControl (sweep::findControl (fx.base, rateIdx)))
        {
            const auto ts = acceptanceTargets (rateRead.pts);
            juce::StringArray ns; for (const auto& t : ts) ns.add (juce::String (t.norm, 6));
            if (! ns.isEmpty())
            {   const auto keepS = sets; sets = rateSets; const auto rd = readRates (rateIdx, ns, kTempoBpm, "acc", {}); sets = keepS;
                if (rd.window) { say ("MOD: a window appeared; stopping"); return 5; }
                for (size_t i = 0; i < ts.size() && i < rd.pts.size(); ++i)
                {   const auto& p = rd.pts[i]; const auto j = judgeAcceptance (ts[i].rateHz, p.period.ok ? p.period.rateHz : 0.0);
                    auto* a = new juce::DynamicObject(); a->setProperty ("written_norm", ts[i].norm); a->setProperty ("display", p.display); a->setProperty ("target_hz", ts[i].rateHz);
                    a->setProperty ("ran", j.judged); a->setProperty ("pass", j.pass); a->setProperty ("why", j.why); if (j.judged) { a->setProperty ("measured_hz", p.period.rateHz); ++accRan; if (j.pass) ++accPassed; } acc.add (juce::var (a));
                    say ("    acceptance: " + j.why + (j.judged ? (j.pass ? " PASS" : " FAIL") : juce::String())); }
            }
        }
        else rb->setProperty ("acceptance_note", "a stepped rate control: its own positions are what it can be set to (the map is the check)");
        rb->setProperty ("acceptance", acc);
        say ("  rate: " + juce::String (passed) + " of " + juce::String (judged) + " labelled position(s) within " + juce::String (kRateTolPct, 0) + " %; acceptance " + juce::String (accPassed) + " of " + juce::String (accRan));
        rateBlock = juce::var (rb);
    }
    // THE DEPTH: 5 positions at the rate control as instantiated; the wobble's spread, recorded
    juce::var depthBlock;
    if (! depthC.empty() && rateIdx >= 0)
    {
        const auto [dIdx, dName] = depthC.front();
        auto keep = sets; keep.removeString (juce::String (dIdx) + ":0.75"); const auto savedSets = sets; sets = keep;
        const auto rd = readRates (dIdx, { "0.000000", "0.250000", "0.500000", "0.750000", "1.000000" }, kTempoBpm, "depth" + juce::String (dIdx), {});
        sets = savedSets;
        if (rd.window) { say ("MOD: a window appeared; stopping"); return 5; }
        auto* db = new juce::DynamicObject(); db->setProperty ("index", dIdx); db->setProperty ("control", dName); db->setProperty ("read_via", rd.via);
        db->setProperty ("map", pointsVar (rd, kTempoBpm)); db->setProperty ("note", "the wobble's spread (5th to 95th percentile) at each depth position, the rate control as instantiated: recorded, not judged (v1)");
        depthBlock = juce::var (db);
        juce::StringArray sp; for (size_t i = 0; i < rd.spread.size(); ++i) sp.add (juce::String (rd.spread[i], 2));
        say ("  depth [" + juce::String (dIdx) + "] " + dName + ": spread " + sp.joinIntoString (" / ") + (rd.via == "pitch" ? " cents" : " dB"));
    }
    auto namesVar = [] (const std::vector<std::pair<int, juce::String>>& v) { juce::Array<juce::var> a; for (const auto& [i, n] : v) { auto* o = new juce::DynamicObject(); o->setProperty ("index", i); o->setProperty ("control", n); a.add (juce::var (o)); } return a; };
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_modulation_record/0"); o->setProperty ("status", "PROTOTYPE (Kathy, weekend item 1, 31 Oct): not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", fmtTag (desc) + "|" + uidHex + "|" + desc.version);
    o->setProperty ("method", "a steady tone at 300 Hz and at 2 kHz, " + juce::String (kLevelDbfs, 0) + " dBFS, " + juce::String (kHoldS, 0) + " s per position (" + juce::String (kLongHoldS, 0) + " s where no period showed), the first "
                              + juce::String (kDiscardS, 0) + " s discarded, the level in " + juce::String (kWinS * 1000.0, 0) + " ms windows; the rate is the period both tones agree on (EjmapModulation.h periodOf); a playing host at " + juce::String (kTempoBpm, 0) + " bpm");
    o->setProperty ("measuring_writes", writes); o->setProperty ("rate_candidates", candRows);
    if (rateBlock.isObject()) o->setProperty ("rate", rateBlock);
    if (depthBlock.isObject()) o->setProperty ("depth", depthBlock);
    o->setProperty ("mix_controls", namesVar (mixC)); o->setProperty ("feedback_controls", namesVar (fbC)); o->setProperty ("sync_controls", namesVar (syncC));
    o->setProperty ("measuredAt", nowStamp());
    const juce::var rec (o);
    outDir.getChildFile (stem + ".modulation.json").replaceWithText (juce::JSON::toString (rec) + "\n", false, false, "\n");
    say ("MOD: " + juce::String (rateIdx >= 0 ? "rate measured" : "no rate control found") + " -> " + outDir.getChildFile (stem + ".modulation.json").getFullPathName());
    if (rateIdx < 0) return 4;
    auto* d = new juce::DynamicObject(); d->setProperty ("schema", "ej_modulation_profile/0"); d->setProperty ("category", "modulation");
    d->setProperty ("spec", drafts::specTag ("MODULATION_PROFILE_SPEC")); d->setProperty ("status", drafts::statusLine ("MODULATION_PROFILE_SPEC"));
    d->setProperty ("plugin", drafts::pluginBlock (opt.product, desc.manufacturerName, uidHex, desc.version, fx.base.getProperty ("map_fp", juce::var())));
    d->setProperty ("measured", drafts::measuredBlock ("EJ Map (feat/ejmap-wizard), probe " + id.cdhash.substring (0, 12), runDateIso(), 48000, "300 Hz and 2 kHz sines, -12 dBFS, 5 ms windows"));
    d->setProperty ("rate", rateBlock); if (depthBlock.isObject()) d->setProperty ("depth", depthBlock);
    d->setProperty ("mix_controls", namesVar (mixC)); d->setProperty ("feedback_controls", namesVar (fbC)); d->setProperty ("sync_controls", namesVar (syncC));
    const auto f = drafts::draftFile (opt.out, stem, "modulation_profile"); const auto problem = drafts::writeDraft (f, juce::var (d));
    say (problem.isEmpty() ? "MOD: draft ej_modulation_profile/0 -> " + f.getFullPathName() : "MOD: " + problem);
    return 0;
}
} // namespace ejmap::cert
