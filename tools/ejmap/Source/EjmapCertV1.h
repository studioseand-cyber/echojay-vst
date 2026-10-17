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
*/

#pragma once

#include "EjmapTrust.h"

namespace ejmap::certv1
{

struct Part { juce::String name, state, why; };   // state: pass | fail | not_tested
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

inline Result judgeEq (const juce::var& d)
{
    Result r; bool anyGood = false, anyFail = false;
    if (const auto* bs = listOf (d.getProperty ("bands", {})))
        for (const auto& b : *bs)
        {
            if (b.getProperty ("verdict", "").toString() != "measured") continue;
            const auto band = b.getProperty ("name", "").toString();
            auto g = labelled ("gain", b.getProperty ("gain_map", {}), "gain_db", [] (double) { return 1.0; }, 3, 1.0, 12.0);
            Part f { "freq", "not_tested", "" };
            {
                int n = 0; double worst = -1.0; juce::String w;
                if (const auto* l = listOf (b.getProperty ("freq_map", {})))
                    for (const auto& x : *l) { double c = 0.0, lab = 0.0; if (! num (x, "centre_hz", c) || ! showsHz (x.getProperty ("display", "").toString()) || ! trust::numberOf (x.getProperty ("display", "").toString(), lab) || lab <= 0.0 || c <= 0.0) continue;
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
            if (ok (g) && (ok (f) || f.state == "not_applicable")) anyGood = true;
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
inline Result judge (const juce::String& category, const juce::var& draft)
{
    Result r;
    if (category == "eq") r = judgeEq (draft);
    else if (category == "limiter") r = judgeLimiter (draft);
    else if (category == "delay") r = judgeSpace (draft, false);
    else if (category == "reverb") r = judgeSpace (draft, true);
    else if (category == "saturation") r = judgeSaturation (draft);
    else if (category == "gate") r = judgeGate (draft);
    else { r.parts.push_back ({ "rule", "not_tested", "no v1 rule for " + category }); }
    r.category = category;
    r.product = draft.getProperty ("plugin", {}).getProperty ("name", "").toString();
    r.pluginId = draft.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString();
    return r;
}
inline const juce::StringArray& v1Categories() { static const juce::StringArray c { "eq", "limiter", "delay", "reverb", "saturation", "gate" }; return c; }
// the parts that wait for the listening session (said beside every count)
inline juce::String feelWaits (const juce::String& category)
{
    if (category == "eq") return "feel words (warmer, air, less boxy) and the default band per region";
    if (category == "reverb") return "room-size / brightness words and the feel bands";
    if (category == "delay") return "the feel bands (slap, echo, ambience)";
    if (category == "saturation") return "character words (subtle / warm / crunch / fuzz) as THD bands";
    if (category == "gate") return "the feel (tight / natural) and the bleed / hit levels A must send";
    if (category == "limiter") return "the loudness feel (nothing: the LUFS target is EchoJay's)";
    return "all of it";
}

} // namespace ejmap::certv1
