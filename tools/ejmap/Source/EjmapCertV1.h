/*
  EjmapCertV1.h - v1 CERTIFICATION FOR THE MEASURABLE PARTS OF THE OTHER CATEGORIES (Kathy, 16 Oct 2026, "SORT ALL THREE" item 3).
  DRAFT RULES for Sean to agree (docs/TRUST_LEVELS_v0.md section 5); they judge the drafts Phase B already wrote, nothing is loaded.

  Each rule tests what a measurement can prove - the label the user would ask for against what the audio did - and leaves the "feel"
  bands to the listening session:
    EQ          gain: measured band gain vs its dB label within 1.0 dB (labels within +-12 dB, 3+ positions); freq: measured centre vs
                its Hz label within 1/6 octave (3+ positions; Hz / k displays only); Q: measured -3 dB bandwidth vs the Q label's bandwidth within 25% (only
                where the display is a Q number; else "not tested", not a failure). PASS: certified for every band whose gain AND freq pass (a fixed-
                frequency band: gain alone), at least one; a band with a failed part is left out, never offered. Feel (warmer / air / less boxy) waits.
    LIMITER     ceiling: holds BS.1770 true peak with the worst overshoot <= 0.1 dB at every driven position (PASS on this alone;
                the acceptance requests are reported - they fail where no measured label is within 0.25 dB of the request). threshold: the compressor tone check where the draft carries one (reported, not required).
    DELAY       time: measured repeat time vs the ms / s label within max(1 ms, 2%) (3+ positions; a display with no time unit - "100%",
                a bare number, a note value - is not a time label); feedback: the fall per repeat shrinks as the control rises (3+ points); mix: the law measured and its worst
                miss <= 1.0 dB. PASS: time passes and no acceptance step that ran failed; feedback / mix are offered only where each passes.
    REVERB      decay: RT60 rises with the control (3+ points, max(0.1 s, 10%) allowance); pre-delay: measured from the first position vs the label's distance from the first label, within max(2 ms, 10%), ms / s labels
                only;
                mix as delay; PASS: decay passes, at least one acceptance step ran and none failed; pre-delay / mix are offered only where each
                passes.
                Feel (room size words, "darker") waits.
    SATURATION  drive: THD rises with drive (3+ points at one level, 1.5 dB allowance for a plateau) over at least 6 dB; PASS: drive passes, at least
                one acceptance step ran and none failed. Character words (warm / crunch / fuzz) wait.
    GATE        threshold: the measured open level vs the dB label within 1.5 dB (3+ positions), or - where the positions carry no display -
                the acceptance steps (asked open level vs measured); PASS: threshold passes, at least one
                acceptance step ran and none failed. Feel (tight / natural) waits.
    TRANSIENT   (27 Oct) attack / sustain as AMOUNTS against their labels: the change rises with the control over 3+ dB, every label's sign agrees
                within 1.0 dB; a multiband band's control is not judged; PASS: one amount passes, at least one acceptance step ran and none failed
    DE-ESSER    (27 Oct) the threshold or the depth (by name) against its labels on the noise ladder, frequency vs its Hz label within 1/6
                octave (offered only where it passes); PASS: the amount passes and the acceptance step ran and passed (judgeDeesser)
*/

#pragma once

#include "EjmapTrust.h"

namespace ejmap::certv1
{

struct Part { juce::String name, state, why, control; bool readBack = false, notTheRole = false; };   // notTheRole: the control is not that part at all (a level control, a band of a multiband) - never listed   // state: pass | fail | not_tested; control: the control the part judged; readBack: the trust run found that control read back (27 Oct: per-part trust)
struct Result { juce::String category, product, pluginId, format; bool pass = false; std::vector<Part> parts;   // format: what the draft was measured in (17 Oct, rulings 2: certification counts only for that format)
    // the parts a certified product offers: every part that passed (an EQ band's gain / freq, a delay's mix ...); never a failed one
    juce::StringArray certifiedParts() const { juce::StringArray c; if (pass) for (const auto& p : parts) if (p.state == "pass" && p.name != "acceptance" && ! p.name.containsChar (':')) c.add (p.name); return c; }   // "band 2: gain" is a sub-check, "band 2" the offered band
    juce::String why() const
    { juce::StringArray s; for (const auto& p : parts) s.add (p.name + " " + p.state + (p.why.isNotEmpty() ? " (" + p.why + ")" : "")); return s.joinIntoString ("; "); } };

// the display shows a time / frequency unit (a "100%" or a bare "39" on a time or frequency control is not a label in that unit)
inline bool showsTime (const juce::String& d) { const auto l = d.toLowerCase().trim(); return l.contains ("ms") || l.contains ("msec") || l.contains ("sec") || l.endsWith (" s") || l.endsWith ("s"); }
inline bool showsHz (const juce::String& d) { const auto l = d.toLowerCase(); return l.contains ("hz") || l.trim().endsWith ("k"); }
inline double timeMsOf (const juce::String& d, double v) { const auto l = d.toLowerCase(); return (l.contains ("ms") || l.contains ("msec")) ? v : v * 1000.0; }
inline const juce::Array<juce::var>* listOf (const juce::var& v) { return v.getArray(); }
inline bool num (const juce::var& x, const char* k, double& out) { const auto v = x.getProperty (k, {}); if (v.isVoid() || v.isUndefined() || v.toString().isEmpty()) return false; out = (double) v; return std::isfinite (out); }

// every acceptance step that ran: (ran, failed, the first failure's why)
struct Acc { int ran = 0, failed = 0; juce::String why; };
inline Acc acceptanceOf (const juce::var& list)
{
    Acc a; if (const auto* l = listOf (list)) for (const auto& x : *l) if ((bool) x.getProperty ("ran", false)) { ++a.ran; if (! (bool) x.getProperty ("pass", false)) { ++a.failed; if (a.why.isEmpty()) a.why = x.getProperty ("why", "").toString(); } }
    return a;
}
inline Part accPart (const Acc& a, bool needOne)
{
    if (a.failed > 0) return { "acceptance", "fail", juce::String (a.failed) + " of " + juce::String (a.ran) + " failed: " + a.why };
    if (a.ran == 0) return { "acceptance", "not_tested", needOne ? "no acceptance step ran (needed)" : "no acceptance step ran" };
    return { "acceptance", "pass", juce::String (a.ran) + " step(s)" };
}
// label vs measured over a list: |measured - label(display)| within tol(label); n points needed; key = the measured field
template <typename Tol>
inline Part labelled (const char* name, const juce::var& list, const char* key, Tol tol, int need = 3, double labelScale = 1.0, double labelMax = 1e9)
{
    int n = 0; juce::String worst; double worstD = -1.0;
    if (const auto* l = listOf (list))
        for (const auto& x : *l)
        {
            double m = 0.0, lab = 0.0; if (! num (x, key, m) || ! trust::numberOf (x.getProperty ("display", "").toString(), lab)) continue;
            lab *= labelScale; if (std::abs (lab) > labelMax) continue;
            ++n; const double d = std::abs (m - lab), t = tol (lab);
            if (d > t && d - t > worstD) { worstD = d - t; worst = "'" + x.getProperty ("display", "").toString().trim() + "' measured " + juce::String (m, 2); }
        }
    if (n < need) return { name, "not_tested", juce::String (n) + " labelled position(s)" };
    if (worstD >= 0.0) return { name, "fail", worst };
    return { name, "pass", juce::String (n) + " positions" };
}
// a measured field that must rise (or fall) with the control's norm, allowance in its own units
inline Part monotonic (const char* name, const juce::var& list, const char* key, bool rising, double allowance, double minSpan = 0.0, double relAllowance = 0.0)
{
    std::vector<std::pair<double, double>> pts;
    if (const auto* l = listOf (list)) for (const auto& x : *l) { double m = 0.0; if (num (x, key, m)) pts.push_back ({ (double) x.getProperty ("norm", 0.0), m }); }
    if (pts.size() < 3) return { name, "not_tested", juce::String ((int) pts.size()) + " point(s)" };
    std::sort (pts.begin(), pts.end());
    double lo = pts[0].second, hi = pts[0].second;
    for (size_t i = 1; i < pts.size(); ++i)
    {
        lo = std::min (lo, pts[i].second); hi = std::max (hi, pts[i].second);
        const double step = (pts[i].second - pts[i - 1].second) * (rising ? 1.0 : -1.0);
        if (step < -std::max (allowance, relAllowance * std::abs (pts[i - 1].second))) return { name, "fail", "reverses by " + juce::String (-step, 2) + " at norm " + juce::String (pts[i].first, 3) };
    }
    if (hi - lo < minSpan) return { name, "fail", "moves only " + juce::String (hi - lo, 2) };
    return { name, "pass", juce::String ((int) pts.size()) + " points" };
}
inline bool ok (const Part& p) { return p.state == "pass"; }
inline bool notFail (const Part& p) { return p.state != "fail"; }

// 20 Oct (Kathy): a bare-number frequency label counts as Hz ONLY when the band's frequency control declares Hz; the measured centre must
// then match it within the same tolerance (1/6 octave) as a label that shows Hz
inline bool labelIsHz (const juce::String& display, const juce::String& declaredUnit)
{
    if (showsHz (display)) return true;
    const auto u = declaredUnit.trim().toLowerCase(); const auto d = display.trim();
    return u == "hz" && d.isNotEmpty() && d.containsOnly ("0123456789.");
}
// 20 Oct (Kathy): a shelf mapped at several gains is certified PER GAIN - the frequency part at a gain passes when every acceptance write
// at that gain ran and passed (at least two figures), fails when one ran and failed, else not_tested
inline Part shelfFreqAtGain (const juce::var& acceptance, double gdb)
{
    int ran = 0, passed = 0;
    if (const auto* a = acceptance.getArray()) for (const auto& x : *a)
        if (std::abs ((double) x.getProperty ("target_db", 99.0) - gdb) < 1e-6 && (bool) x.getProperty ("ran", false)) { ++ran; if ((bool) x.getProperty ("pass", false)) ++passed; }
    const auto name = juce::String ("freq at ") + (gdb > 0 ? "+" : "") + juce::String (gdb, 0) + " dB";
    if (ran >= 2 && passed == ran) return { name, "pass", juce::String (ran) + " acceptance write(s) at this gain" };
    if (ran > passed) return { name, "fail", juce::String (ran - passed) + " of " + juce::String (ran) + " write(s) at this gain missed" };
    return { name, "not_tested", juce::String (ran) + " write(s) at this gain" };
}
inline Result judgeEq (const juce::var& d)
{
    Result r; bool anyGood = false, anyFail = false;
    if (const auto* bs = listOf (d.getProperty ("bands", {})))
        for (const auto& b : *bs)
        {
            if (! eq::measuredBand (b.getProperty ("verdict", "").toString())) continue;   // 19 Oct: an engaged band is judged (REQ); its switch is in the draft
            const auto band = b.getProperty ("name", "").toString();
            auto g = labelled ("gain", b.getProperty ("gain_map", {}), "gain_db", [] (double) { return 1.0; }, 3, 1.0, 12.0);
            Part f { "freq", "not_tested", "" };
            {
                int n = 0; double worst = -1.0; juce::String w;
                if (const auto* l = listOf (b.getProperty ("freq_map", {})))
                    for (const auto& x : *l) { double c = 0.0, lab = 0.0; if (! num (x, "centre_hz", c) || ! labelIsHz (x.getProperty ("display", "").toString(), b.getProperty ("freq_unit", "").toString()) || ! trust::numberOf (x.getProperty ("display", "").toString(), lab) || lab <= 0.0 || c <= 0.0) continue;
                        ++n; const double oct = std::abs (std::log2 (c / lab)); if (oct > 1.0 / 6.0 && oct > worst) { worst = oct; w = "'" + x.getProperty ("display", "").toString().trim() + "' measured " + juce::String (c, 1) + " Hz"; } }
                const auto fc = b.getProperty ("freq_control", "").toString();
                const bool fixed = (fc.isEmpty() || fc == "(none)") && ! b.getProperty ("freq_map", {}).isArray();
                f = fixed ? Part { "freq", "not_applicable", "a fixed-frequency band" }
                  : n < 3 ? Part { "freq", "not_tested", juce::String (n) + " labelled position(s)" } : worst >= 0.0 ? Part { "freq", "fail", w } : Part { "freq", "pass", juce::String (n) + " positions" };
            }
            Part q { "q", "not_tested", "the display is not a Q number" };
            {
                int n = 0; double worst = -1.0; juce::String w;
                if (const auto* l = listOf (b.getProperty ("q_map", {})))
                    for (const auto& x : *l) { const auto disp = x.getProperty ("display", "").toString(); double bw = 0.0, qv = 0.0;
                        if (disp.contains ("%") || ! num (x, "bandwidth_oct_at_3db", bw) || ! trust::numberOf (disp, qv) || qv <= 0.05 || qv > 50.0) continue;
                        ++n; const double expect = 2.0 * std::asinh (1.0 / (2.0 * qv)) / std::log (2.0), rel = std::abs (bw - expect) / expect;
                        if (rel > 0.25 && rel > worst) { worst = rel; w = "Q '" + disp.trim() + "' measured " + juce::String (bw, 2) + " oct (expected " + juce::String (expect, 2) + ")"; } }
                if (n >= 3) q = worst >= 0.0 ? Part { "q", "fail", w } : Part { "q", "pass", juce::String (n) + " positions" };
            }
            for (auto* p : { &g, &f, &q }) { p->name = band + " " + p->name; r.parts.push_back (*p); }
            bool shelfGainOk = false;   // 20 Oct: a shelf mapped per gain is certified at the gains whose own writes all pass
            if (const auto* fg = b.getProperty ("freq_by_gain", {}).getArray(); fg != nullptr && ! fg->isEmpty())
                for (double gdb : { -3.0, 3.0 }) { auto pg = shelfFreqAtGain (b.getProperty ("acceptance", {}), gdb); pg.name = band + " " + pg.name; r.parts.push_back (pg); shelfGainOk = shelfGainOk || ok (pg); if (pg.state == "fail") anyFail = true; }
            if (ok (g) && (ok (f) || f.state == "not_applicable" || shelfGainOk)) anyGood = true;
            if (! notFail (g) || ! notFail (f) || ! notFail (q)) anyFail = true;
        }
    r.pass = anyGood; (void) anyFail;   // certified for the bands that pass; a failing band is left out (certifiedParts says which)
    if (r.parts.empty()) r.parts.push_back ({ "bands", "not_tested", "no measured band" });
    return r;
}
inline Result judgeLimiter (const juce::var& d)
{
    Result r; const auto c = d.getProperty ("ceiling", {});
    double over = 0.0; const bool holds = (bool) c.getProperty ("holds_true_peak", false); const int driven = (int) c.getProperty ("driven_positions", 0);
    Part ceil { "ceiling", "fail", "" };
    if (! c.isObject()) ceil = { "ceiling", "not_tested", "no ceiling block" };
    else if (driven < 1) ceil = { "ceiling", "not_tested", "no position driven over the ceiling" };
    else if (! holds) ceil = { "ceiling", "fail", "does not hold true peak" };
    else if (num (c, "worst_true_overshoot_db", over) && over > 0.1) ceil = { "ceiling", "fail", "true-peak overshoot " + juce::String (over, 2) + " dB" };
    else ceil = { "ceiling", "pass", juce::String (driven) + " driven position(s), overshoot " + juce::String (over, 2) + " dB" };
    r.parts.push_back (ceil);
    const auto acc = accPart (acceptanceOf (c.getProperty ("acceptance", {})), false); r.parts.push_back (acc);
    const auto tc = d.getProperty ("tone_check", {});
    r.parts.push_back (tc.isObject() ? Part { "threshold", (bool) tc.getProperty ("pass_within_0_5_db", false) ? "pass" : "fail", "the compressor tone check" } : Part { "threshold", "not_tested", "no tone check" });
    r.pass = ok (ceil);   // the acceptance requests fail where no measured label sits within 0.25 dB of the request: coverage, reported
    return r;
}
inline Result judgeSpace (const juce::var& d, bool reverb)
{
    Result r;
    const auto mixB = d.getProperty ("mix", {});
    Part mix { "mix", "not_tested", "no mix control" };
    if (mixB.isObject()) { double w = 0.0; mix = mixB.getProperty ("verdict", "").toString() != "measured" ? Part { "mix", "not_tested", mixB.getProperty ("verdict", "").toString() }
                                                : (num (mixB, "worst_db", w) && w > 1.0) ? Part { "mix", "fail", "law miss " + juce::String (w, 2) + " dB" } : Part { "mix", "pass", "" }; }
    const auto acc = accPart (acceptanceOf (d.getProperty ("acceptance", {})), reverb);
    if (reverb)
    {
        const auto dec = d.getProperty ("decay", {}), pre = d.getProperty ("predelay", {});
        const auto decayAllow = dec.isObject() ? monotonic ("decay", dec.getProperty ("map", {}), "rt60_s", true, 0.1, 0.0, 0.10) : Part { "decay", "not_tested", "no decay control" };   // max(0.1 s, 10%): a long tail's noise, not a reversal
        Part pd { "predelay", "not_tested", "no pre-delay control" };
        if (pre.isObject())
        {   // relative_ms is measured from the unit's own onset at its first position: compare it with the label's distance from the first label
            int n = 0; double worst = -1.0, first = 0.0; bool haveFirst = false; juce::String w;
            if (const auto* l = listOf (pre.getProperty ("map", {})))
                for (const auto& x : *l) { const auto disp = x.getProperty ("display", "").toString(); double m = 0.0, lab = 0.0;
                    if (! num (x, "relative_ms", m) || ! showsTime (disp) || ! trust::numberOf (disp, lab)) continue;
                    lab = timeMsOf (disp, lab); if (! haveFirst) { first = lab; haveFirst = true; }
                    ++n; const double want = lab - first, d0 = std::abs (m - want), tol = std::max (2.0, 0.1 * std::abs (want));
                    if (d0 > tol && d0 - tol > worst) { worst = d0 - tol; w = "'" + disp.trim() + "' measured +" + juce::String (m, 1) + " ms from the first position (label +" + juce::String (want, 1) + ")"; } }
            pd = n < 3 ? Part { "predelay", "not_tested", juce::String (n) + " time-labelled position(s)" } : worst >= 0.0 ? Part { "predelay", "fail", w } : Part { "predelay", "pass", juce::String (n) + " positions" };
        }
        r.parts = { decayAllow, pd, mix, acc };
        r.pass = ok (decayAllow) && ok (acc);   // pre-delay / mix offered only where each passes (certifiedParts)
    }
    else
    {
        const auto t = d.getProperty ("time", {}), fb = d.getProperty ("feedback", {});
        Part time { "time", "not_tested", "no time control" };
        if (t.isObject())
        {
            // a "1.2 s" label is 1200 ms: scale by the display's unit
            int n = 0; double worst = -1.0; juce::String w;
            if (const auto* l = listOf (t.getProperty ("map", {})))
                for (const auto& x : *l) { const auto disp = x.getProperty ("display", "").toString(); double m = 0.0, lab = 0.0;
                    if (! num (x, "time_ms", m) || disp.containsChar ('/') || ! showsTime (disp) || ! trust::numberOf (disp, lab)) continue;
                    lab = timeMsOf (disp, lab);
                    ++n; const double d0 = std::abs (m - lab), tol = std::max (1.0, 0.02 * std::abs (lab)); if (d0 > tol && d0 - tol > worst) { worst = d0 - tol; w = "'" + disp.trim() + "' measured " + juce::String (m, 1) + " ms"; } }
            time = n < 3 ? Part { "time", "not_tested", juce::String (n) + " labelled position(s)" } : worst >= 0.0 ? Part { "time", "fail", w } : Part { "time", "pass", juce::String (n) + " positions" };
        }
        // fall_per_repeat_db is negative (dB lost per repeat): more feedback = a smaller loss = the value RISES towards 0
        const auto fbRise = fb.isObject() ? monotonic ("feedback", fb.getProperty ("map", {}), "fall_per_repeat_db", true, 0.5) : Part { "feedback", "not_tested", "no feedback control" };
        r.parts = { time, fbRise, mix, acc };
        r.pass = ok (time) && notFail (acc);   // feedback / mix offered only where each passes (certifiedParts)
    }
    return r;
}
inline Result judgeSaturation (const juce::var& d)
{
    Result r; const auto drv = d.getProperty ("drive", {});
    Part drive { "drive", "not_tested", "no drive control" };
    if (drv.isObject())
        if (const auto* lv = drv.getProperty ("levels", {}).getDynamicObject())
        {
            Part best { "drive", "not_tested", "" };
            for (const auto& p : lv->getProperties())
            {
                auto part = monotonic ("drive", p.value, "thd_db", true, 1.5, 6.0); part.why = "at " + p.name.toString() + " dBFS: " + part.why;
                if (ok (part)) { best = part; break; }
                if (best.state == "not_tested" || part.state == "fail") best = part;
            }
            drive = best;
        }
    const auto acc = accPart (acceptanceOf (d.getProperty ("acceptance", {})), true);
    r.parts = { drive, acc }; r.pass = ok (drive) && ok (acc);
    return r;
}
inline Result judgeGate (const juce::var& d)
{
    Result r; const auto th = d.getProperty ("threshold", {});
    auto t = th.isObject() ? labelled ("threshold", th.getProperty ("map", {}), "open_level_dbfs_peak", [] (double) { return 1.5; }) : Part { "threshold", "not_tested", "no threshold" };
    const auto acc = accPart (acceptanceOf (d.getProperty ("acceptance", {})), true);
    // the acceptance steps ask the gate to open at a level and measure where it opens: that IS the threshold's test (within 1.5 dB)
    if (t.state == "not_tested" && ok (acc)) t = { "threshold", "pass", "by the acceptance steps (asked open level vs measured)" };
    r.parts = { t, acc }; r.pass = ok (t) && ok (acc);
    return r;
}
// TRANSIENT SHAPER v1 (Kathy, 26 Oct; FOR_SEAN_DRAFT_TYPES): attack and sustain are AMOUNTS, never times. Each is judged against its own
// labels: the measured change rises with the control (3+ points, 0.5 dB allowance) over at least 3 dB (the acceptance steps ask +-3 dB), and
// every labelled position agrees in sign within 1.0 dB (the transient bar): a label of 0 changes nothing, a positive label adds, a negative
// one takes away. A control on one band of a multiband ("Band 4 Range") or a crossover is not a transient amount: not judged
inline bool multibandControl (const juce::String& control)
{
    const auto l = control.toLowerCase();
    if (l.contains ("crossover") || l.contains ("xover")) return true;
    const int b = l.indexOf ("band"); if (b < 0) return false;
    const auto rest = l.substring (b + 4).trimStart(); return rest.isNotEmpty() && juce::CharacterFunctions::isDigit (rest[0]);
}
// a multiband transient designer (FOR_SEAN_DRAFT_TYPES Table C, approved 26 Oct: "exclude multiband transient designers (MTransientMB)"):
// its amounts act per band, so a whole-plugin attack / sustain is not what it offers. By the product's name: "multiband", a word "multi",
// or a name ending in "MB"
inline bool multibandProduct (const juce::String& name)
{
    const auto l = name.toLowerCase(); const auto words = juce::StringArray::fromTokens (l, " ()-_", "");
    return l.contains ("multiband") || l.contains ("multi-band") || words.contains ("multi") || name.trim().endsWith ("MB");
}
inline constexpr double kTransientBarDb = 1.0;
// A LEVEL CONTROL IS NOT A TRANSIENT AMOUNT (27 Oct: Quantum's "Output" and KNOCK's "Input Gain" were drafted as sustain): where the control
// takes 3 dB or more away, a plain gain takes the hits and the sustain away together - the other measure within 1.0 dB (the transient bar)
// at every such position, one at least. A shaper's control always moves them apart there (Envolution's Sus Level: -27.3 against -2.5)
inline bool levelControl (const juce::var& map, const char* key, const char* otherKey)
{
    int cuts = 0;
    if (const auto* l = listOf (map))
        for (const auto& x : *l)
        {
            double own = 0.0, other = 0.0; if (! num (x, key, own) || own > -3.0) continue;
            if (! num (x, otherKey, other) || std::abs (other - own) > kTransientBarDb) return false;
            ++cuts;
        }
    return cuts > 0;
}
inline Part transientAmount (const char* name, const juce::var& block, const char* key)
{
    if (! block.isObject()) return { name, "not_tested", juce::String ("no ") + name + " control" };
    const auto ctl = block.getProperty ("control", "").toString();
    if (block.getProperty ("verdict", "").toString() != "measured")   // never measured as this part (TransX's Release: no_effect) - not listed
    { Part x { name, "not_tested", ctl + ": " + block.getProperty ("verdict", "").toString() }; x.notTheRole = true; return x; }
    if (multibandControl (ctl)) { Part x { name, "not_tested", "'" + ctl + "' is one band of a multiband: not a transient amount (v1)" }; x.notTheRole = true; return x; }
    auto p = monotonic (name, block.getProperty ("map", {}), key, true, 0.5, 3.0);
    if (! ok (p)) return p;
    if (levelControl (block.getProperty ("map", {}), key, juce::String (key) == "transient_db" ? "sustain_db" : "transient_db"))
    { Part x { name, "fail", "'" + ctl + "' takes the hits and the sustain away together: a level control, not a transient amount" }; x.notTheRole = true; return x; }
    int n = 0; juce::String worst;
    if (const auto* l = listOf (block.getProperty ("map", {})))
        for (const auto& x : *l)
        {
            double m = 0.0, lab = 0.0; if (! num (x, key, m) || ! trust::numberOf (x.getProperty ("display", "").toString(), lab)) continue;
            ++n; const bool agrees = lab > 0.0 ? m >= -kTransientBarDb : lab < 0.0 ? m <= kTransientBarDb : std::abs (m) <= kTransientBarDb;
            if (! agrees && worst.isEmpty()) worst = "'" + x.getProperty ("display", "").toString().trim() + "' measured " + juce::String (m, 2) + " dB";
        }
    if (n < 2) return { name, "not_tested", juce::String (n) + " labelled position(s)" };
    if (worst.isNotEmpty()) return { name, "fail", "the label's sign disagrees: " + worst };
    return { name, "pass", p.why + ", " + juce::String (n) + " labels agree" };
}
inline Result judgeTransient (const juce::var& d)
{
    Result r;
    const auto acc = accPart (acceptanceOf (d.getProperty ("acceptance", {})), true);
    if (const auto product = d.getProperty ("plugin", {}).getProperty ("name", "").toString(); multibandProduct (product))
    { r.parts = { { "attack", "not_tested", "'" + product + "' is a multiband transient designer: per band, not v1" }, acc }; return r; }
    auto atk = transientAmount ("attack", d.getProperty ("attack", {}), "transient_db");
    auto sus = transientAmount ("sustain", d.getProperty ("sustain", {}), "sustain_db");
    atk.control = d.getProperty ("attack", {}).getProperty ("control", "").toString(); sus.control = d.getProperty ("sustain", {}).getProperty ("control", "").toString();
    r.parts = { atk, sus, acc }; r.pass = (ok (atk) || ok (sus)) && ok (acc);   // each amount offered only where it passes (certifiedParts)
    return r;
}
// DE-ESSER v1 (Kathy, 26 Oct; FOR_SEAN_DRAFT_TYPES): the amount control, its noise ladder (every label at every level) and the frequency
// positions with their Hz labels - from the draft (drafts from 27 Oct carry them, so the server can run this rule) or, for an older draft,
// from its measuring record beside it. The amount is a THRESHOLD or a DEPTH by its name (trust::depthNamed: range / depth / reduction),
// and the part is named so:
//   threshold  the GR rises as the labelled threshold falls, at every level (3+ labels, 0.5 dB allowance), over 3 dB at the pick's level;
//              a label 12 dB or more above the signal barely acts (GR <= 1.0 dB), 1+ such cell. Labels above 0 (an amount scale, "40.0")
//              are not a dB threshold: not judged
//   depth      the GR rises as the labelled depth grows, at every level (same allowance and span), and never takes more than its label
//              (GR <= |label| + 0.5 dB, the de-esser bar)
//   frequency  each position's measured corner (a shelf) or centre (a notch) vs its Hz label within 1/6 octave (the EQ rule), 2+ positions;
//              offered only where it passes
// PASS: the amount passes and the acceptance step ran and passed
inline constexpr double kDeesserBarDb = 0.5;
inline Part deesserAmount (const juce::String& ctl, const juce::var& nl, double pickLevel)
{
    const bool depth = trust::depthNamed (ctl); const char* name = depth ? "depth" : "threshold";
    if (ctl.isEmpty()) return { "threshold", "not_tested", "no amount control named (no measuring record beside an older draft)" };
    if (! (bool) nl.getProperty ("ok", false)) return { name, "not_tested", "the noise ladder did not complete" };
    std::map<double, std::vector<std::pair<double, double>>> byLevel;   // level -> (label, GR)
    if (const auto* cs = listOf (nl.getProperty ("cells", {})))
        for (const auto& c : *cs) { double lab = 0.0, gr = 0.0, lv = 0.0; if (trust::numberOf (c.getProperty ("display", "").toString(), lab) && num (c, "gr_db", gr) && num (c, "level_dbfs", lv)) byLevel[lv].push_back ({ lab, gr }); }
    if (byLevel.empty()) return { name, "not_tested", "no labelled cell" };
    int judgedFar = 0;
    for (auto& [lv, pts] : byLevel)
    {
        if (pts.size() < 3) return { name, "not_tested", juce::String ((int) pts.size()) + " labels at " + juce::String (lv, 0) + " dBFS" };
        for (const auto& p : pts) if (! depth && p.first > 0.0) return { name, "not_tested", "'" + ctl + "' labels run above 0 (" + juce::String (p.first, 1) + "): an amount scale, not a dB threshold" };
        for (auto& p : pts) p.first = depth ? std::abs (p.first) : -p.first;   // both read as "more": a deeper depth, a lower threshold
        std::sort (pts.begin(), pts.end());
        double lo = pts[0].second, hi = pts[0].second;
        for (size_t i = 1; i < pts.size(); ++i)
        {
            lo = std::min (lo, pts[i].second); hi = std::max (hi, pts[i].second);
            if (pts[i].second < pts[i - 1].second - kDeesserBarDb)
                return { name, "fail", "GR falls by " + juce::String (pts[i - 1].second - pts[i].second, 2) + " dB as the " + name + " goes to " + juce::String (depth ? pts[i].first : -pts[i].first, 1) + " at " + juce::String (lv, 0) + " dBFS" };
        }
        if (std::abs (lv - pickLevel) < 0.01 && hi - lo < 3.0) return { name, "fail", "GR moves only " + juce::String (hi - lo, 2) + " dB at " + juce::String (lv, 0) + " dBFS" };
        for (const auto& p : pts)
        {
            if (depth && p.second > p.first + kDeesserBarDb) return { name, "fail", "takes " + juce::String (p.second, 2) + " dB at a depth of " + juce::String (p.first, 1) + " (" + juce::String (lv, 0) + " dBFS)" };
            if (! depth && -p.first >= lv + 12.0) { ++judgedFar; if (p.second > 1.0) return { name, "fail", "a threshold of " + juce::String (-p.first, 1) + " takes " + juce::String (p.second, 2) + " dB of a " + juce::String (lv, 0) + " dBFS signal" }; }
        }
    }
    if (! depth && judgedFar == 0) return { name, "not_tested", "no label 12 dB above the signal to judge the threshold by" };
    return { name, "pass", "'" + ctl + "', " + juce::String ((int) byLevel.size()) + " levels" };
}
// a position's measured figure: a notch's centre, a shelf's corner (the draft names it centre_hz / corner_hz; the record carries both, with its shape)
inline bool deesserHz (const juce::var& c, double& hz)
{
    if (c.hasProperty ("centre_hz")) return num (c, "centre_hz", hz);
    if (c.getProperty ("shape", "shelf").toString() != "shelf" && c.hasProperty ("deepest_hz")) return num (c, "deepest_hz", hz);
    return num (c, "corner_hz", hz);
}
inline Part deesserFrequency (const juce::String& ctl, const juce::var& positions)
{
    if (ctl.isEmpty()) return { "frequency", "not_tested", "no frequency control" };
    int n = 0; double worst = -1.0; juce::String w;
    if (const auto* cs = listOf (positions))
        for (const auto& c : *cs)
        {
            double lab = 0.0, hz = 0.0; if (! num (c, "label_hz", lab) || ! deesserHz (c, hz) || lab <= 0.0 || hz <= 0.0) continue;
            ++n; const double oct = std::abs (std::log2 (hz / lab));
            if (oct > 1.0 / 6.0 && oct > worst) { worst = oct; w = "'" + c.getProperty ("display", "").toString() + "' measured " + juce::String (hz, 0) + " Hz (" + juce::String (oct, 2) + " octave)"; }
        }
    if (n < 2) return { "frequency", "not_tested", juce::String (n) + " Hz-labelled position(s)" };
    if (worst >= 0.0) return { "frequency", "fail", w };
    return { "frequency", "pass", juce::String (n) + " positions" };
}
inline Result judgeDeesser (const juce::var& d, const juce::var& record)
{
    Result r; const auto blk = d.getProperty ("deesser", {}), a = blk.getProperty ("acceptance", {});
    const bool own = blk.hasProperty ("amount_control");   // a draft from 27 Oct carries everything; an older one reads its record
    const auto ctl = (own ? blk.getProperty ("amount_control", "") : record.getProperty ("threshold_control", "")).toString();
    const auto nl = own ? blk.getProperty ("noise_ladder", {}) : record.getProperty ("noise_ladder", {});
    double pickLevel = -12.0; num (blk.getProperty ("pick", {}), "level_dbfs", pickLevel);
    auto amount = deesserAmount (ctl, nl, pickLevel); amount.control = ctl;
    const bool ownHz = own && blk.getProperty ("band", {}).hasProperty ("positions");
    const auto fctl = (ownHz ? blk.getProperty ("band", {}).getProperty ("control", "") : record.getProperty ("frequency_control", "")).toString();
    // 31 Oct (weekend item 7): the detector read directly, where it was, in place of the cut curve's corner (which moves with the depth)
    const auto det = ownHz && blk.getProperty ("band", {}).getProperty ("detector", {}).isArray() ? blk.getProperty ("band", {}).getProperty ("detector", {}) : record.getProperty ("detector", {});
    auto freq = deesserFrequency (fctl, det.isArray() && det.size() > 0 ? det : ownHz ? blk.getProperty ("band", {}).getProperty ("positions", {}) : record.getProperty ("centre", {})); freq.control = fctl;
    if (det.isArray() && det.size() > 0) freq.why = "the detector: " + freq.why;
    Part acc { "acceptance", "not_tested", "no acceptance step ran (needed)" };
    if ((bool) a.getProperty ("ran", false)) acc = (bool) a.getProperty ("pass", false) ? Part { "acceptance", "pass", "1 step(s)" } : Part { "acceptance", "fail", a.getProperty ("why", "").toString() };
    r.parts = { amount, freq, acc }; r.pass = ok (amount) && ok (acc);   // frequency offered only where it passes (certifiedParts)
    return r;
}
// the drafts' folder names (phaseb/<folder>/drafts) and the map categories they certify
// 27 Oct (Kathy: "certify PART BY PART"): the categories whose certified trust row lists each part, verified or not - a part that did not
// pass is dialled as mapped, "not verified yet"
inline bool partByPart (const juce::String& mapCategory) { return mapCategory == "de-esser" || mapCategory == "transient_shaper"; }
// the parts a certified row lists (acceptance is the rule's own step, not a part); several passing drafts: a part is verified when any verifies it
inline std::vector<Part> mergeParts (std::vector<Part> into, const std::vector<Part>& from)
{
    for (const auto& p : from)
    {
        if (p.name == "acceptance" || p.control.isEmpty() || p.notTheRole) continue;   // no control (Sibilance has no frequency control), or not that part at all (Quantum's Output as "sustain"): not listed
        auto it = std::find_if (into.begin(), into.end(), [&] (const Part& q) { return q.name == p.name; });
        if (it == into.end()) into.push_back (p); else if (! ok (*it) && ok (p)) *it = p;
    }
    return into;
}
inline juce::String partsWhy (const std::vector<Part>& parts)
{
    juce::StringArray v, m, n;   // verified; mapped (read back) but not verified; neither
    for (const auto& p : parts) (ok (p) ? v : p.readBack ? m : n).add (p.name + (p.control.isNotEmpty() ? " (" + p.control + ")" : juce::String()));
    return (v.isEmpty() ? juce::String() : "; verified: " + v.joinIntoString (", ")) + (m.isEmpty() ? juce::String() : "; mapped, not verified yet: " + m.joinIntoString (", "))
         + (n.isEmpty() ? juce::String() : "; not verified, not mapped: " + n.joinIntoString (", "));
}
inline juce::String mapCategoryOf (const juce::String& folder)
{ return folder == "transient" ? juce::String ("transient_shaper") : folder == "deesser" ? juce::String ("de-esser") : folder; }
// record: the measuring record beside the draft, where the rule reads one (an older de-esser draft)
inline Result judge (const juce::String& category, const juce::var& draft, const juce::var& record = {})
{
    Result r;
    if (category == "eq") r = judgeEq (draft);
    else if (category == "limiter") r = judgeLimiter (draft);
    else if (category == "delay") r = judgeSpace (draft, false);
    else if (category == "reverb") r = judgeSpace (draft, true);
    else if (category == "saturation") r = judgeSaturation (draft);
    else if (category == "gate") r = judgeGate (draft);
    else if (category == "transient") r = judgeTransient (draft);   // 27 Oct
    else if (category == "deesser") r = judgeDeesser (draft, record);   // 27 Oct
    else { r.parts.push_back ({ "rule", "not_tested", "no v1 rule for " + category }); }
    r.category = category;
    r.product = draft.getProperty ("plugin", {}).getProperty ("name", "").toString();
    r.pluginId = draft.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString();
    return r;
}
inline const juce::StringArray& v1Categories() { static const juce::StringArray c { "eq", "limiter", "delay", "reverb", "saturation", "gate", "transient", "deesser" }; return c; }
// the parts that wait for the listening session (said beside every count)
inline juce::String feelWaits (const juce::String& category)
{
    if (category == "eq") return "feel words (warmer, air, less boxy) and the default band per region";
    if (category == "reverb") return "room-size / brightness words and the feel bands";
    if (category == "delay") return "the feel bands (slap, echo, ambience)";
    if (category == "saturation") return "character words (subtle / warm / crunch / fuzz) as THD bands";
    if (category == "gate") return "the feel (tight / natural) and the bleed / hit levels A must send";
    if (category == "limiter") return "the loudness feel (nothing: the LUFS target is EchoJay's)";
    if (category == "transient") return "the feel (punchier / softer) and the release / mix, which v1 does not judge";
    if (category == "deesser") return "how natural the result sounds (lisp, dullness) and the mode / selectivity choice";
    return "all of it";
}

} // namespace ejmap::certv1
