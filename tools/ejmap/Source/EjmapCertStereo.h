/*
  EjmapCertStereo.h - THE STEREO / IMAGING PASS (weekend item 2, 31 Oct 2026, Kathy). Included at the end of EjmapCertDriver.h. Nothing
  exported, nothing published: a record under <out>/stereo/ and a draft under <out>/drafts/. The derivations are EjmapStereo.h's.

  --cert-stereo <product>
    every control a width / balance / mid / side name proposes, at 7 positions (a stepped control: up to 12 of its steps), a 997 Hz sine
    at -12 dBFS, 2 s per position:
      width-named   fed `left` (side / mid = 0 dB in): side / mid out;     balance-named fed `same`: L / R out;
      mid-named     fed `same`: the mid's gain;                            side-named    fed `side`: the side's gain.
    The role is the figure that moves monotonically over 6 dB (names propose, measurement decides). Width labels in % judged within
    0.5 dB; mid / side gain labels in dB within 0.5 dB; balance recorded (pan is held, 30 Oct), its centre judged equal within 0.2 dB.
    THE WIDTH ACCEPTANCE (held out): two side / mid figures between measured positions, written and read back within 0.5 dB.
  A plugin with one main input (mono, mono-to-stereo) is fed its one input: the side it MAKES is recorded, never judged against a %.
*/

#pragma once

#include "EjmapStereo.h"

namespace ejmap::cert
{
inline int runStereo (const SweepOptions& opt)
{
    using namespace stereo;
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("STEREO: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedPlugins()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("STEREO: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("STEREO: " + opt.product + " - " + gate.stop); return kToneLicenceKnownExit; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("stereo"); outDir.createDirectory();
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false);
    const auto stem = fmtTag (desc) + "_" + uidHex + "_" + desc.version;
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "stereo", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash,
                                    { "width", "stereo", "spread", "image", "wide", "balance", "pan", "mid", "side", "ms", "center", "centre", "rotation", "shuffle", "space", "position" });
    if (! fx.ok) { say ("STEREO: " + fx.why); return 1; }
    say ("STEREO: " + fx.note);
    std::vector<std::pair<int, juce::String>> controls;
    if (const auto* cs = fx.base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) controls.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", "").toString() });
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra)
    {
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runProbeCall (args, opt.timeoutMs); raw.getChildFile (stem + ".stereo." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r;
    };
    auto normsFor = [&] (int idx)
    {
        const auto ctl = sweep::findControl (fx.base, idx); juce::StringArray out;
        if (sweep::isSteppedControl (ctl)) { const int st = (int) ctl.getProperty ("numSteps", 0), n = juce::jmin (st, 12);
            for (int k = 0; k < n; ++k) out.add (juce::String ((float) std::lround ((double) k * (st - 1) / juce::jmax (1, n - 1)) / (float) juce::jmax (1, st - 1), 6)); }
        else for (int k = 0; k < 7; ++k) out.add (juce::String ((float) k / 6.0f, 6));
        return out;
    };
    int mainIn = -1;
    // one sweep of a control fed one way: per position the output's mid / side / L / R, and the input's RMS (the gains' reference)
    struct Read { std::vector<MsRead> rs; double inRmsDb = 0.0; bool window = false; juce::String refused; };
    auto readAt = [&] (int idx, const juce::StringArray& norms, const juce::String& feed, const juce::String& tag) -> Read
    {
        Read rd;
        const auto r = run (tag, { "--sweep", "thr=" + juce::String (idx), "norms=" + norms.joinIntoString (","), "levels=" + juce::String (kLevelDbfs, 1), "hz=" + juce::String (kToneHz, 1),
                                   "hold=" + juce::String (kHoldS, 2), "discard=" + juce::String (kDiscardS, 2), "ref=0", "moving_db=999", "stereo=" + feed });
        if (r.kind == ChildResult::Kind::uiShown) { rd.window = true; return rd; }
        std::map<int, size_t> at;
        for (const auto& line : juce::StringArray::fromLines (r.cleanExit() ? r.out : juce::String()))
        {
            const auto f = juce::StringArray::fromTokens (line, "\t", ""); if (f.isEmpty()) continue;
            auto kv = [&] (int from, const juce::String& key) -> juce::String { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return {}; };
            if (line.startsWith ("refused")) rd.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim();
            else if (f[0] == "config" && mainIn < 0) mainIn = kv (1, "main_in").getIntValue();
            else if (f[0] == "pos" && f.size() > 3) { MsRead m; m.norm = (float) kv (2, "norm").getDoubleValue(); m.display = kv (2, "text"); at[f[1].getIntValue()] = rd.rs.size(); rd.rs.push_back (m); }
            else if (f[0] == "hold" && f.size() > 3 && at.count (f[1].getIntValue())) rd.inRmsDb = kv (3, "in_rms_db").getDoubleValue();
            else if (f[0] == "stereo" && f.size() > 3 && at.count (f[1].getIntValue()))   // the probe's stereo line (31 Oct), beside its hold line
            {   auto& m = rd.rs[at[f[1].getIntValue()]]; const auto h = readHold (f, 3); const auto norm = m.norm; const auto disp = m.display; m = h; m.norm = norm; m.display = disp; }
        }
        return rd;
    };
    auto rowsOf = [&] (const Read& rd, const juce::String& role, int* judged, int* passed)
    {
        juce::Array<juce::var> rows;
        for (const auto& r : rd.rs)
        {
            auto* o = new juce::DynamicObject(); o->setProperty ("norm", r.norm); o->setProperty ("display", r.display);
            if (! r.ok) { o->setProperty ("why", "not read"); rows.add (juce::var (o)); continue; }
            auto rnd = [] (double x) { return std::round (x * 100.0) / 100.0; };
            o->setProperty ("side_over_mid_db", rnd (juce::jlimit (-99.0, 99.0, widthDb (r)))); o->setProperty ("l_over_r_db", rnd (juce::jlimit (-99.0, 99.0, balanceDb (r))));
            o->setProperty ("mid_gain_db", rnd (r.midDb - rd.inRmsDb)); o->setProperty ("side_gain_db", rnd (r.sideDb - rd.inRmsDb)); o->setProperty ("correlation", rnd (r.corr));
            Judged j;
            if (role == "width" && mainIn >= 2) j = judgeWidth (r, widthLabel (r.display));
            else if (role == "mid") { if (const auto d = dbLabel (r.display)) j = judgeGain (r.midDb - rd.inRmsDb, *d); }
            else if (role == "side") { if (const auto d = dbLabel (r.display)) j = judgeGain (r.sideDb - rd.inRmsDb, *d); }
            else if (role == "balance") { const auto t = r.display.trim().toLowerCase(); if (t == "c" || t == "0" || t == "<0>" || t == "center" || t == "centre" || t == "0.0")
                                          { j.judged = true; j.offDb = balanceDb (r); j.pass = std::abs (j.offDb) <= kCentreTolDb; j.why = "centre: L / R " + juce::String (j.offDb, 2) + " dB"; } }
            if (j.judged) { o->setProperty ("within_tolerance", j.pass); o->setProperty ("judged", j.why); if (judged) ++*judged; if (passed && j.pass) ++*passed; }
            rows.add (juce::var (o));
        }
        return rows;
    };
    juce::Array<juce::var> ctlRows; int judgedAll = 0, passedAll = 0, accRan = 0, accPassed = 0; int roles = 0;
    for (const auto& [idx, name] : controls)
    {
        juce::String role, feed;
        if (widthNamed (name)) { role = "width"; feed = "left"; } else if (balanceNamed (name)) { role = "balance"; feed = "same"; }
        else if (sideNamed (name)) { role = "side"; feed = "side"; } else if (midNamed (name)) { role = "mid"; feed = "same"; } else continue;
        auto rd = readAt (idx, normsFor (idx), feed, "c" + juce::String (idx) + "." + feed);
        if (rd.window) { say ("STEREO: a window appeared; stopping"); return 5; }
        if (rd.refused.contains ("two main inputs")) { feed = "same"; rd = readAt (idx, normsFor (idx), feed, "c" + juce::String (idx) + ".same"); if (rd.window) { say ("STEREO: a window appeared; stopping"); return 5; } }
        juce::String why; bool decided = false;
        if (role == "width") decided = moves (rd.rs, [] (const MsRead& r) { return widthDb (r); }, why);
        else if (role == "balance") decided = moves (rd.rs, [] (const MsRead& r) { return balanceDb (r); }, why);
        else if (role == "mid") decided = moves (rd.rs, [] (const MsRead& r) { return r.midDb; }, why);
        else decided = moves (rd.rs, [] (const MsRead& r) { return r.sideDb; }, why);
        if (decided && role == "width" && isRotation (rd.rs)) { role = "rotation"; why += "; it drives the mid away (side over mid beyond " + juce::String (kRotationDb, 0) + " dB): a rotation, recorded, never judged as a width"; }
        int judged = 0, passed = 0;
        auto* o = new juce::DynamicObject(); o->setProperty ("index", idx); o->setProperty ("control", name); o->setProperty ("proposed", role); o->setProperty ("fed", feed);
        o->setProperty ("main_inputs", mainIn); o->setProperty ("moves", decided); o->setProperty ("why", why);
        o->setProperty ("positions", rowsOf (rd, role, &judged, &passed)); o->setProperty ("labels_judged", judged); o->setProperty ("labels_within", passed);
        if (decided) { ++roles; judgedAll += judged; passedAll += passed; }
        say ("  [" + juce::String (idx) + "] " + name + " (" + role + ", fed " + feed + "): " + why + (decided ? " -> measured" : " -> not this role") + (judged > 0 ? "; labels " + juce::String (passed) + " of " + juce::String (judged) : juce::String()));
        // THE WIDTH ACCEPTANCE, held out (a continuous width control fed two inputs)
        if (decided && role == "width" && mainIn >= 2 && ! sweep::isSteppedControl (sweep::findControl (fx.base, idx)))
        {
            const auto ts = widthTargets (rd.rs); juce::StringArray ns; for (const auto& t : ts) ns.add (juce::String (t.norm, 6));
            juce::Array<juce::var> acc;
            if (! ns.isEmpty())
            {   const auto ar = readAt (idx, ns, "left", "c" + juce::String (idx) + ".acc");
                if (ar.window) { say ("STEREO: a window appeared; stopping"); return 5; }
                for (size_t i = 0; i < ts.size() && i < ar.rs.size(); ++i)
                {   const auto& r = ar.rs[i]; auto* a = new juce::DynamicObject(); a->setProperty ("written_norm", ts[i].norm); a->setProperty ("target_side_over_mid_db", std::round (ts[i].db * 100.0) / 100.0);
                    const bool ran = r.ok; a->setProperty ("ran", ran);
                    if (ran) { const double off = widthDb (r) - ts[i].db; a->setProperty ("measured_db", std::round (widthDb (r) * 100.0) / 100.0); a->setProperty ("off_db", std::round (off * 100.0) / 100.0); a->setProperty ("pass", std::abs (off) <= kWidthTolDb); ++accRan; if (std::abs (off) <= kWidthTolDb) ++accPassed;
                               say ("    acceptance: asked " + juce::String (ts[i].db, 2) + " dB side / mid, measured " + juce::String (widthDb (r), 2) + (std::abs (off) <= kWidthTolDb ? " PASS" : " FAIL")); }
                    acc.add (juce::var (a)); }
            }
            o->setProperty ("acceptance", acc);
        }
        ctlRows.add (juce::var (o));
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_stereo_record/0"); o->setProperty ("status", "PROTOTYPE (Kathy, weekend item 2, 31 Oct): not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", fmtTag (desc) + "|" + uidHex + "|" + desc.version);
    o->setProperty ("method", "a 997 Hz sine at -12 dBFS, 2 s per position; width fed to the left input only (side / mid 0 dB in), balance and mid fed to both, side fed antiphase; the output's mid ((L+R)/2), side ((L-R)/2), L and R over the measured span");
    o->setProperty ("main_inputs", mainIn); o->setProperty ("controls", ctlRows); o->setProperty ("measuredAt", nowStamp());
    const juce::var rec (o);
    outDir.getChildFile (stem + ".stereo.json").replaceWithText (juce::JSON::toString (rec) + "\n", false, false, "\n");
    say ("STEREO: " + juce::String (roles) + " control(s) measured in their role; labels " + juce::String (passedAll) + " of " + juce::String (judgedAll) + " within; acceptance " + juce::String (accPassed) + " of " + juce::String (accRan)
         + " -> " + outDir.getChildFile (stem + ".stereo.json").getFullPathName());
    if (roles == 0) return 4;
    auto* d = new juce::DynamicObject(); d->setProperty ("schema", "ej_stereo_profile/0"); d->setProperty ("category", "stereo");
    d->setProperty ("spec", drafts::specTag ("STEREO_PROFILE_SPEC")); d->setProperty ("status", drafts::statusLine ("STEREO_PROFILE_SPEC"));
    d->setProperty ("plugin", drafts::pluginBlock (opt.product, desc.manufacturerName, uidHex, desc.version, fx.base.getProperty ("map_fp", juce::var())));
    d->setProperty ("measured", drafts::measuredBlock ("EJ Map (feat/ejmap-wizard), probe " + id.cdhash.substring (0, 12), runDateIso(), 48000, "997 Hz sine, -12 dBFS, per-input feeds"));
    juce::Array<juce::var> measured; for (const auto& c : ctlRows) if ((bool) c.getProperty ("moves", false)) measured.add (c);
    d->setProperty ("controls", measured); d->setProperty ("main_inputs", mainIn);
    const auto f = drafts::draftFile (opt.out, stem, "stereo_profile"); const auto problem = drafts::writeDraft (f, juce::var (d));
    say (problem.isEmpty() ? "STEREO: draft ej_stereo_profile/0 -> " + f.getFullPathName() : "STEREO: " + problem);
    return 0;
}
} // namespace ejmap::cert
