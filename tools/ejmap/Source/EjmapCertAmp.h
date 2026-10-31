/*
  EjmapCertAmp.h - AMP SIMS BEYOND THE DRIVE (weekend item 4, 31 Oct 2026, Kathy). The saturation pass already measures an amp's drive
  (THD, onsets, level); this pass reads what it does not: the TONE STACK, the OUTPUT, and the fixed context the reading was taken in.
  Included at the end of EjmapCertDriver.h. Nothing exported, nothing published: a record under <out>/amp/ and a draft under <out>/drafts/.

  --cert-amp <product>
    THE TONE STACK: every control named bass / low / mid / middle / treble / high / presence / contour / tone (never a frequency), at 7
      positions, the EQ's multitone (10 Hz-23.5 kHz, 1/12 octave) at -30 dBFS - low, so the amp stays near its linear region - against
      the plugin as instantiated. Per position: where it acts (the band's centre or corner) and how much (dB there). A dB label judged
      within 0.5 dB; an unlabelled 0-10 knob gets its law recorded, and both get the held-out ACCEPTANCE: two gains between measured
      positions (1/3 and 2/3 of the measured range), written at the interpolated norm, read at the same frequency within 0.5 dB.
    THE OUTPUT: every control named output / master / volume / level / out, at 7 positions, the median change over the tones (the cabinet
      shapes the spectrum; a level control moves all of it). A dB label judged within 0.5 dB, the same held-out acceptance.
    THE FIXED CONTEXT: every switch named model / amp / pedal / type / cab / cabinet / mic / channel / preset, its display as instantiated -
      the tone stack and output were read in that context and are recorded with it (a pedal board records its pedal).
*/

#pragma once

namespace ejmap::cert
{
namespace amp
{
inline bool hasWord (const juce::String& name, std::initializer_list<const char*> words)
{
    const auto l = " " + name.toLowerCase().replaceCharacters ("_-/()[].:", "         ") + " ";
    for (const char* w : words) if (l.contains (" " + juce::String (w) + " ")) return true;
    return false;
}
inline bool toneNamed (const juce::String& n)    { return hasWord (n, { "bass", "low", "lows", "mid", "mids", "middle", "treble", "high", "highs", "presence", "contour", "tone" }) && ! hasWord (n, { "freq", "frequency", "hz", "q", "cut", "filter", "gate", "level", "volume", "gain" }); }
inline bool outputNamed (const juce::String& n)  { return hasWord (n, { "output", "master", "volume", "out", "level" }) && ! hasWord (n, { "input", "in", "drive", "gate", "noise", "reverb", "delay", "mix" }); }
inline bool contextNamed (const juce::String& n) { return hasWord (n, { "model", "amp", "pedal", "type", "cab", "cabinet", "mic", "channel", "preset", "stomp", "head", "speaker" }); }
inline constexpr double kLevelDbfs = -30.0, kTolDb = 0.5;
// one position read: the figure (Hz where it acts most) and the gain there; a broadband move (the median over the tones)
struct ToneRead { bool ok = false; float norm = 0.0f; juce::String display; double hz = 0.0, gainDb = 0.0, broadbandDb = 0.0; juce::String shape; };
inline double median (std::vector<double> v) { if (v.empty()) return 0.0; std::sort (v.begin(), v.end()); return v[v.size() / 2]; }
// the gain at a frequency, from a deviation (log-f interpolation between the tones around it)
inline std::optional<double> gainAt (const std::vector<std::pair<double, double>>& dev, double hz)
{
    for (size_t i = 1; i < dev.size(); ++i)
        if (dev[i - 1].first <= hz && dev[i].first >= hz)
        {   const double t = std::log (hz / dev[i - 1].first) / std::log (dev[i].first / dev[i - 1].first); return dev[i - 1].second + t * (dev[i].second - dev[i - 1].second); }
    return std::nullopt;
}
// THE FREQUENCY A TONE CONTROL ACTS AT: across its positions, the tone where the deviation spreads most (max minus min); every position's
// gain is then read there - one frequency per control, so its law is one curve
// 31 Oct (the amp runs: ENGL's "Presence at 12.7 Hz", Friedman's "Treble at 11.3 Hz"): only where a guitar or bass amp's tone stack acts -
// the grid's ends below 40 Hz and above 16 kHz carry the cabinet's roll-off and noise, never a tone control's band
inline constexpr double kActsLoHz = 40.0, kActsHiHz = 16000.0;
// a control whose positions move the response by less than this anywhere is INACTIVE here (another channel's, another model's): recorded
inline constexpr double kInactiveDb = 3.0;
inline double spreadAt (const std::vector<std::vector<std::pair<double, double>>>& devs, double hz)
{   double lo = 1e9, hi = -1e9; for (const auto& d : devs) for (const auto& [f, v] : d) if (std::abs (f - hz) < 1e-6) { lo = std::min (lo, v); hi = std::max (hi, v); } return hi > lo ? hi - lo : 0.0; }
inline double actsAt (const std::vector<std::vector<std::pair<double, double>>>& devs)
{
    if (devs.empty() || devs.front().empty()) return 0.0;
    double bestHz = 0.0, bestSpread = -1.0;
    for (size_t k = 0; k < devs.front().size(); ++k)
    {   if (devs.front()[k].first < kActsLoHz || devs.front()[k].first > kActsHiHz) continue;
        double lo = 1e9, hi = -1e9; for (const auto& d : devs) if (k < d.size()) { lo = std::min (lo, d[k].second); hi = std::max (hi, d[k].second); }
        if (hi - lo > bestSpread) { bestSpread = hi - lo; bestHz = devs.front()[k].first; } }
    return bestHz;
}
struct Target { double db = 0.0; float norm = 0.0f; };
inline std::vector<Target> targets (std::vector<std::pair<float, double>> v)   // (norm, dB) -> 1/3 and 2/3 of the range, the norm interpolated
{
    std::sort (v.begin(), v.end()); std::vector<Target> out; if (v.size() < 2) return out;
    double lo = v.front().second, hi = lo; for (const auto& [n, x] : v) { lo = std::min (lo, x); hi = std::max (hi, x); }
    if (hi - lo < 1.0) return out;   // under 1 dB of range: nothing to aim at
    for (double f : { 1.0 / 3.0, 2.0 / 3.0 })
    {   const double want = lo + f * (hi - lo);
        for (size_t i = 1; i < v.size(); ++i) { const double a = v[i - 1].second, b = v[i].second;
            if ((want - a) * (want - b) <= 0.0 && std::abs (b - a) > 1e-9) { out.push_back ({ want, (float) (v[i - 1].first + (want - a) / (b - a) * (v[i].first - v[i - 1].first)) }); break; } } }
    return out;
}
inline std::optional<double> dbLabel (const juce::String& display)
{
    const auto t = display.trim().toLowerCase(); if (! t.contains ("db")) return std::nullopt;
    const auto num = t.upToFirstOccurrenceOf ("db", false, false).trim().retainCharacters ("0123456789.-+");
    if (num.isEmpty() || num == "-" || num == "+") return std::nullopt;
    return num.getDoubleValue();
}
} // namespace amp

inline int runAmp (const SweepOptions& opt)
{
    using namespace amp;
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("AMP: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedPlugins()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("AMP: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("AMP: " + opt.product + " - " + gate.stop); return kToneLicenceKnownExit; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("amp"); outDir.createDirectory();
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false);
    const auto stem = fmtTag (desc) + "_" + uidHex + "_" + desc.version;
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "amp", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash,
                                    { "bass", "low", "mid", "middle", "treble", "high", "presence", "contour", "tone", "output", "master", "volume", "level", "model", "amp", "pedal", "type", "cab", "cabinet", "mic", "channel", "drive", "gain" });
    if (! fx.ok) { say ("AMP: " + fx.why); return 1; }
    say ("AMP: " + fx.note);
    std::vector<std::pair<int, juce::String>> controls;
    if (const auto* cs = fx.base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) controls.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", "").toString() });
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra)
    {
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runProbeCall (args, opt.timeoutMs); raw.getChildFile (stem + ".amp." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r;
    };
    auto response = [&] (const juce::String& tag, int ctl, const juce::StringArray& norms)
    {
        const auto r = run (tag, { "--response", "ctl=" + juce::String (ctl), "norms=" + norms.joinIntoString (","), "tones=" + juce::String (eq::kGridTones), "lo=" + juce::String (eq::kGridLoHz, 0),
                                   "hi=" + juce::String (eq::kGridHiHz, 0), "db=" + juce::String (kLevelDbfs, 0), "hold=" + juce::String (eq::kHoldS, 1), "discard=" + juce::String (eq::kDiscardS, 1) });
        return std::make_pair (eq::parseResponse (r.cleanExit() ? r.out : juce::String ("refused " + r.describe())), r);
    };
    juce::StringArray seven; for (int k = 0; k < 7; ++k) seven.add (juce::String ((float) k / 6.0f, 6));
    std::vector<std::pair<int, juce::String>> toneC, outC, ctxC;
    for (const auto& c : controls) { if (toneNamed (c.second)) toneC.push_back (c); else if (outputNamed (c.second)) outC.push_back (c); else if (contextNamed (c.second)) ctxC.push_back (c); }
    say ("AMP: " + opt.product + " " + desc.version + ": tone " + juce::String ((int) toneC.size()) + ", output " + juce::String ((int) outC.size()) + ", context " + juce::String ((int) ctxC.size()));
    if (toneC.empty() && outC.empty()) { say ("AMP: no tone-stack or output control by name"); return 4; }
    // THE BASELINE: the plugin as instantiated (one call; the first named control read at its current value)
    const int baseCtl = ! toneC.empty() ? toneC.front().first : outC.front().first;
    auto [b0, br] = response ("base", baseCtl, { "current" });
    if (br.kind == ChildResult::Kind::uiShown) { say ("AMP: a window appeared; stopping"); return 5; }
    if (b0.positions.empty()) { say ("AMP: the baseline read nothing (" + (b0.refused.isNotEmpty() ? b0.refused : br.describe()) + ")"); return 1; }
    const auto baseline = b0.positions.front();
    int judged = 0, passed = 0, accRan = 0, accPassed = 0;
    auto readControl = [&] (int idx, const juce::String& name, bool tone) -> juce::var
    {
        auto [r, rr] = response ("c" + juce::String (idx), idx, seven);
        if (rr.kind == ChildResult::Kind::uiShown) return juce::var ("window");
        std::vector<std::vector<std::pair<double, double>>> devs; std::vector<ToneRead> reads;
        for (const auto& p : r.positions) { const auto d = eq::deviation (p, baseline); devs.push_back (d); ToneRead t; t.ok = ! d.empty(); t.norm = p.norm; t.display = p.text;
            std::vector<double> g; for (const auto& [hz, db] : d) g.push_back (db); t.broadbandDb = median (g); reads.push_back (t); }
        const double hz = tone ? actsAt (devs) : 0.0;
        double moveDb = 0.0; if (tone) moveDb = spreadAt (devs, hz); else { double lo = 1e9, hi = -1e9; for (const auto& t : reads) if (t.ok) { lo = std::min (lo, t.broadbandDb); hi = std::max (hi, t.broadbandDb); } moveDb = hi > lo ? hi - lo : 0.0; }
        const bool inactive = moveDb < kInactiveDb;
        auto* o = new juce::DynamicObject(); o->setProperty ("index", idx); o->setProperty ("control", name); o->setProperty ("role", inactive ? juce::String ("inactive") : juce::String (tone ? "tone" : "output"));
        if (inactive) o->setProperty ("why", "moves the response by " + juce::String (moveDb, 1) + " dB at most: inactive here (another channel's or model's?) - recorded, not accepted");
        if (tone) o->setProperty ("acts_at_hz", std::round (hz * 10.0) / 10.0);
        juce::Array<juce::var> rows; std::vector<std::pair<float, double>> law;
        for (size_t i = 0; i < reads.size(); ++i)
        {   auto& t = reads[i]; if (! t.ok) continue;
            const double g = tone ? gainAt (devs[i], hz).value_or (0.0) : t.broadbandDb;
            if (tone) { const auto b = eq::deriveBand (devs[i]); t.shape = b.result == "measured" || b.result == "shelf" ? b.shape : b.result; }
            auto* q = new juce::DynamicObject(); q->setProperty ("norm", t.norm); q->setProperty ("display", t.display); q->setProperty ("gain_db", std::round (g * 100.0) / 100.0);
            if (tone) q->setProperty ("shape", t.shape);
            if (const auto lab = dbLabel (t.display)) { const bool ok = std::abs (g - *lab) <= kTolDb; q->setProperty ("label_db", *lab); q->setProperty ("within_tolerance", ok); ++judged; if (ok) ++passed; }
            rows.add (juce::var (q)); law.push_back ({ t.norm, g }); }
        o->setProperty ("law", rows);
        // THE ACCEPTANCE, held out: two gains between measured positions
        juce::Array<juce::var> acc; const auto ts = inactive ? std::vector<Target>() : targets (law);
        if (! ts.empty() && ! sweep::isSteppedControl (sweep::findControl (fx.base, idx)))
        {
            juce::StringArray ns; for (const auto& t : ts) ns.add (juce::String (t.norm, 6));
            auto [ar, arr] = response ("c" + juce::String (idx) + ".acc", idx, ns);
            if (arr.kind == ChildResult::Kind::uiShown) return juce::var ("window");
            for (size_t i = 0; i < ts.size() && i < ar.positions.size(); ++i)
            {   const auto d = eq::deviation (ar.positions[i], baseline); std::vector<double> gg; for (const auto& [h, db] : d) gg.push_back (db);
                const double got = tone ? gainAt (d, hz).value_or (-999.0) : median (gg), off = got - ts[i].db; const bool ok = std::abs (off) <= kTolDb;
                auto* a = new juce::DynamicObject(); a->setProperty ("written_norm", ts[i].norm); a->setProperty ("target_db", std::round (ts[i].db * 100.0) / 100.0); a->setProperty ("measured_db", std::round (got * 100.0) / 100.0);
                a->setProperty ("ran", true); a->setProperty ("pass", ok); acc.add (juce::var (a)); ++accRan; if (ok) ++accPassed;
                say ("    acceptance " + name + ": asked " + juce::String (ts[i].db, 2) + " dB" + (tone ? " at " + juce::String (hz, 0) + " Hz" : juce::String()) + ", measured " + juce::String (got, 2) + (ok ? " PASS" : " FAIL")); }
        }
        o->setProperty ("acceptance", acc);
        juce::StringArray g; for (const auto& [n, x] : law) g.add (juce::String (x, 1));
        say ("  [" + juce::String (idx) + "] " + name + (tone ? " (tone, at " + juce::String (hz, 0) + " Hz)" : juce::String (" (output)")) + ": " + g.joinIntoString (" / ") + " dB");
        return juce::var (o);
    };
    juce::Array<juce::var> toneRows, outRows;
    for (const auto& [idx, name] : toneC) { const auto v = readControl (idx, name, true); if (v.toString() == "window") { say ("AMP: a window appeared; stopping"); return 5; } toneRows.add (v); }
    for (const auto& [idx, name] : outC) { const auto v = readControl (idx, name, false); if (v.toString() == "window") { say ("AMP: a window appeared; stopping"); return 5; } outRows.add (v); }
    juce::Array<juce::var> ctx;
    for (const auto& [idx, name] : ctxC) { auto* c = new juce::DynamicObject(); c->setProperty ("index", idx); c->setProperty ("control", name);
        const auto cv = sweep::findControl (fx.base, idx); c->setProperty ("display", cv.getProperty ("text", cv.getProperty ("display", "")));
        c->setProperty ("why", "fixed as instantiated: the tone stack and output were read in this context"); ctx.add (juce::var (c)); }
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_amp_record/0"); o->setProperty ("status", "PROTOTYPE (Kathy, weekend item 4, 31 Oct): not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", fmtTag (desc) + "|" + uidHex + "|" + desc.version);
    o->setProperty ("method", "the EQ's multitone (" + eq::gridDescription() + ") at " + juce::String (kLevelDbfs, 0) + " dBFS against the plugin as instantiated; a tone control read where it moves the response most, an output as the median over the tones");
    o->setProperty ("tone_stack", toneRows); o->setProperty ("output", outRows); o->setProperty ("fixed_context", ctx);
    o->setProperty ("labels_judged", judged); o->setProperty ("labels_within", passed); o->setProperty ("acceptance_ran", accRan); o->setProperty ("acceptance_passed", accPassed);
    o->setProperty ("measuredAt", nowStamp());
    const juce::var rec (o);
    outDir.getChildFile (stem + ".amp.json").replaceWithText (juce::JSON::toString (rec) + "\n", false, false, "\n");
    say ("AMP: tone " + juce::String (toneRows.size()) + ", output " + juce::String (outRows.size()) + "; labels " + juce::String (passed) + " of " + juce::String (judged) + "; acceptance " + juce::String (accPassed) + " of " + juce::String (accRan)
         + " -> " + outDir.getChildFile (stem + ".amp.json").getFullPathName());
    auto* d = new juce::DynamicObject(); d->setProperty ("schema", "ej_amp_profile/0"); d->setProperty ("category", "amp_sim");
    d->setProperty ("spec", drafts::specTag ("AMP_PROFILE_SPEC")); d->setProperty ("status", drafts::statusLine ("AMP_PROFILE_SPEC"));
    d->setProperty ("plugin", drafts::pluginBlock (opt.product, desc.manufacturerName, uidHex, desc.version, fx.base.getProperty ("map_fp", juce::var())));
    d->setProperty ("measured", drafts::measuredBlock ("EJ Map (feat/ejmap-wizard), probe " + id.cdhash.substring (0, 12), runDateIso(), 48000, "multitone, -30 dBFS"));
    d->setProperty ("tone_stack", toneRows); d->setProperty ("output", outRows); d->setProperty ("fixed_context", ctx);
    const auto f = drafts::draftFile (opt.out, stem, "amp_profile"); const auto problem = drafts::writeDraft (f, juce::var (d));
    say (problem.isEmpty() ? "AMP: draft ej_amp_profile/0 -> " + f.getFullPathName() : "AMP: " + problem);
    return 0;
}
} // namespace ejmap::cert
