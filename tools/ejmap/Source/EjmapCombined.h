/*
  EjmapCombined.h - COMBINED SETTINGS (Kathy's NEXT BUILD item A1, 6 Oct 2026; accuracy, data only, nothing exported).

  For a certified compressor that also has a time draft (timing spec v0.1) and a gain draft (gain spec v0.1), compose the
  full setting a server would write - the section 6 pick at g, the profile's engage and neutral writes (the tone check's
  own list), the ratio the profile was measured at, an ATTACK and a RELEASE position from the time draft, and a MAKE-UP from
  the gain draft's curve that puts the gain reduction back - then predict what the unit will read and let the probe say
  what it does. The miss is the record.

  THE PREDICTION, stated so the miss means something:
    GR_pred  = g + gr_shift(attack position) + gr_shift(release position)
               (the time draft measured each position's GR step against the instantiate position; the two are assumed
                additive - the miss tests exactly that)
    out_pred = L_rms + static_gain_db - GR_pred + (makeup gives - makeup at its neutral)
               (the profile's static gain is at the neutral make-up; the draft's curve says what the written position adds)
  The positions chosen: the attack and the release whose |gr_shift_db| is the LARGEST (a setting that moves the amount is
  the one worth testing; a position that shifts nothing would only re-run the tone check); a position whose attack is a
  bound ("faster than") is still a setting. The make-up control: the draft's first writable control of role makeup, else
  output; the target +GR_pred, clamped to the curve's span (said on the record).

  PURE here, pinned in RoundTripTest.cpp (CB1-CB6); the driver (runCombined) runs the probe once per product.
*/

#pragma once

#include "EjmapGainCal.h"
#include "EjmapProfileExport.h"

namespace ejmap::combined
{

inline constexpr double kTargetGDb = 4.0;      // the setting's ask: 4 dB of gain reduction on the tone at L
inline constexpr double kPassDb    = 0.5;      // the tone check's bar, applied to GR; the output level reported, not gated

struct Write { int index = -1; juce::String control, display, from; double norm = 0.0; };
struct Setting
{
    bool ok = false; juce::String refused; juce::StringArray notes;
    std::vector<Write> writes;                           // every write the probe makes, in order; the amount last
    double g = kTargetGDb, Lrms = 0.0, Lpeak = 0.0;      // the tone
    int amountIndex = -1; juce::String amountControl; double amountNorm = 0.0;
    juce::String attackDisplay, releaseDisplay; double attackShiftDb = 0.0, releaseShiftDb = 0.0;
    int attackIndex = -1, releaseIndex = -1; std::optional<Write> attackBefore, releaseBefore;   // the time controls written, and what the tone check wrote there before (none = at instantiate)
    double attackMs = 0.0, releaseMs = 0.0, holdS = 2.5;   // the chosen positions' times from the draft, and THE HOLD: at least 10x the slower of them, never under the tone check's 2.5 s (Kathy, 7 Oct)
    juce::String makeupControl; double makeupTargetDb = 0.0, makeupGivesDb = 0.0, makeupNeutralDb = 0.0; bool makeupClamped = false;
    double staticGainDb = 0.0, predictedGrDb = 0.0; std::optional<double> predictedOutDb;
};

// THE HOLD (Kathy, 7 Oct): the combined read and the attack-only / release-only reads all settle at least 10x the slower time constant
// written, never under the tone check's 2.5 s, capped at 20 s (a 4 s release = 20 s)
inline constexpr double kMinHoldS = 2.5, kMaxHoldS = 20.0, kHoldFactor = 10.0;
inline double holdFor (double attackMs, double releaseMs) { return juce::jlimit (kMinHoldS, kMaxHoldS, kHoldFactor * juce::jmax (attackMs, releaseMs) / 1000.0); }
// the time draft's position of a role with the largest |gr_shift_db| (ties: the first); none when the role has no positions
struct TimePos { bool ok = false; double norm = 0.0, shiftDb = 0.0, ms = 0.0; juce::String display; };
inline TimePos largestShift (const juce::var& timeDraft, const juce::String& role)
{
    TimePos best;
    const auto blk = timeDraft.getProperty (role, {});
    if (const auto* ps = blk.getProperty ("positions", {}).getArray())
        for (const auto& p : *ps)
        {
            if (! p.hasProperty ("gr_shift_db")) continue;
            const double s = (double) p.getProperty ("gr_shift_db", 0.0);
            if (! best.ok || std::abs (s) > std::abs (best.shiftDb)) { best.ok = true; best.norm = (double) p.getProperty ("norm", 0.0); best.shiftDb = s; best.display = p.getProperty ("display", "").toString();
                const auto ms = p.getProperty (role == "attack" ? "attack_ms" : "release_ms", juce::var()); best.ms = ms.isDouble() || ms.isInt() ? (double) ms : p.hasProperty ("faster_than_ms") ? (double) p.getProperty ("faster_than_ms", 0.0) : (double) p.getProperty (role == "attack" ? "attack_faster_than_ms" : "release_faster_than_ms", 0.0); }
        }
    return best;
}

// the gain draft's curve as gaincal readings at the reference level (measured_db), for normForDb
inline std::vector<gaincal::Reading> readingsOf (const juce::var& control, double refLevel)
{
    std::vector<gaincal::Reading> rows;
    if (const auto* cs = control.getProperty ("curve", {}).getArray())
        for (const auto& c : *cs)
        {
            gaincal::Reading r; r.norm = (double) c.getProperty ("norm", 0.0); r.display = c.getProperty ("display", "").toString(); r.landed = true;
            if (c.getProperty ("measured_db", juce::var()).isDouble() || c.getProperty ("measured_db", juce::var()).isInt()) r.measuredDb[refLevel] = (double) c.getProperty ("measured_db", 0.0);
            rows.push_back (r);
        }
    return rows;
}
// the draft's first writable make-up control (role makeup, else output)
inline juce::var makeupControlOf (const juce::var& gainDraft)
{
    for (const char* role : { "makeup", "output" })
        if (const auto* cs = gainDraft.getProperty ("controls", {}).getArray())
            for (const auto& c : *cs)
                if (c.getProperty ("role", "").toString() == role && (bool) c.getProperty ("writable", false)) return c;
    return {};
}
// the curve's measured value nearest a norm (the neutral's position), for the make-up's own reference
inline std::optional<double> curveDbNearest (const std::vector<gaincal::Reading>& rows, double norm, double refLevel)
{
    std::optional<double> best; double bd = 1e9;
    for (const auto& r : rows) if (r.measuredDb.count (refLevel) && std::abs (r.norm - norm) < bd) { bd = std::abs (r.norm - norm); best = r.measuredDb.at (refLevel); }
    return best;
}

// THE SETTING. toneWrites = the tone check's `writes` (control, index, norm, set): the server-style list, which already holds
// engage, neutral and the ratio; its amount entry is replaced by the pick at g. controlIndex names -> indices for the timing
// and make-up controls (the record's control list).
inline Setting compose (const juce::var& profile, const juce::var& toneWrites, const juce::var& timeDraft, const juce::var& gainDraft,
                        const std::map<juce::String, int>& controlIndex, double g = kTargetGDb)
{
    Setting s; s.g = g;
    s.amountControl = profile.getProperty ("amount", {}).getProperty ("control", "").toString();
    if (s.amountControl.isEmpty()) { s.refused = "the profile names no amount control"; return s; }
    if (! controlIndex.count (s.amountControl)) { s.refused = "the amount control '" + s.amountControl + "' is not on the record's control list"; return s; }
    const auto tl = profile::toneLevelFor (profile, g);
    if (! tl.ok || ! tl.pick.ok) { s.refused = "no tone level / pick at g " + juce::String (g, 1) + ": " + (tl.ok ? tl.pick.refused : tl.reason); return s; }
    s.Lrms = tl.L; s.Lpeak = tl.L + profile::kPeakToSineRmsDb; s.amountIndex = controlIndex.at (s.amountControl); s.amountNorm = tl.pick.norm;
    s.staticGainDb = (double) profile.getProperty ("static_gain_db", 0.0);
    // the tone check's writes, minus the amount (re-picked at g)
    std::set<int> taken;
    if (const auto* ws = toneWrites.getArray())
        for (const auto& w : *ws)
        {
            const int idx = (int) w.getProperty ("index", -1); if (idx < 0 || idx == s.amountIndex) continue;
            s.writes.push_back ({ idx, w.getProperty ("control", "").toString(), w.getProperty ("set", "").toString(), "tone check: " + w.getProperty ("why", "").toString(), (double) w.getProperty ("norm", 0.0) }); taken.insert (idx);
        }
    if (s.writes.empty()) s.notes.add ("the tone check carried no writes (no engage, no neutral): the setting is the pick, the timing and the make-up alone");
    // attack and release from the time draft: the largest |gr_shift_db| positions; a write replaces the tone check's entry for that control
    auto place = [&] (const juce::String& role, juce::String& display, double& shift)
    {
        const auto blk = timeDraft.getProperty (role, {}); const auto name = blk.getProperty ("control", "").toString();
        const auto tp = largestShift (timeDraft, role);
        if (name.isEmpty() || ! tp.ok) { s.notes.add ("no " + role + " position in the time draft: left as the tone check has it"); return; }
        if (! controlIndex.count (name)) { s.notes.add ("the " + role + " control '" + name + "' is not on the record's control list: left as is"); return; }
        const int idx = controlIndex.at (name); display = tp.display; shift = tp.shiftDb;
        if (role == "attack") { s.attackIndex = idx; s.attackMs = tp.ms; } else { s.releaseIndex = idx; s.releaseMs = tp.ms; }
        for (auto& w : s.writes) if (w.index == idx) { (role == "attack" ? s.attackBefore : s.releaseBefore) = w; w.display = tp.display; w.norm = tp.norm; w.from = "time draft " + role + " (gr_shift " + juce::String (tp.shiftDb, 2) + " dB)"; return; }
        s.writes.push_back ({ idx, name, tp.display, "time draft " + role + " (gr_shift " + juce::String (tp.shiftDb, 2) + " dB)", tp.norm }); taken.insert (idx);
    };
    place ("attack", s.attackDisplay, s.attackShiftDb); place ("release", s.releaseDisplay, s.releaseShiftDb);
    s.holdS = holdFor (s.attackMs, s.releaseMs);
    s.predictedGrDb = std::round ((g + s.attackShiftDb + s.releaseShiftDb) * 100.0) / 100.0;
    // the make-up: the draft's curve inverted for +GR_pred
    const auto mk = makeupControlOf (gainDraft);
    if (mk.isObject())
    {
        const auto name = mk.getProperty ("control", "").toString(); const double ref = -40.0;
        const auto rows = readingsOf (mk, ref); const bool stepped = (bool) mk.getProperty ("stepped", false);
        if (! controlIndex.count (name)) s.notes.add ("the make-up control '" + name + "' is not on the record's control list: no make-up written");
        else
        {
            const int idx = controlIndex.at (name);
            double neutralNorm = 0.0; bool haveNeutral = false; for (const auto& w : s.writes) if (w.index == idx) { neutralNorm = w.norm; haveNeutral = true; }
            if (! haveNeutral) if (const auto* ns = gainDraft.getProperty ("neutral", {}).getArray()) for (const auto& n : *ns) if (n.getProperty ("control", "").toString() == name) { neutralNorm = (double) n.getProperty ("norm", 0.0); haveNeutral = true; }
            const auto nd = curveDbNearest (rows, neutralNorm, ref);
            if (! haveNeutral || ! nd) s.notes.add ("the make-up's neutral position is not on the draft's curve: the output prediction is relative to the curve's zero");
            s.makeupNeutralDb = nd ? *nd : 0.0; s.makeupTargetDb = s.makeupNeutralDb + s.predictedGrDb;
            const auto inv = gaincal::normForDb (rows, s.makeupTargetDb, ref, stepped);
            if (! inv.ok) s.notes.add ("the make-up curve cannot be inverted for +" + juce::String (s.predictedGrDb, 2) + " dB: " + inv.why + "; no make-up written");
            else
            {
                s.makeupControl = name; s.makeupGivesDb = inv.givesDb; s.makeupClamped = inv.clamped;
                if (inv.clamped) s.notes.add ("the make-up of +" + juce::String (s.predictedGrDb, 2) + " dB is past the curve's span: written at its end (" + juce::String (inv.givesDb, 2) + " dB)");
                juce::String disp; for (const auto& r : rows) if (std::abs (r.norm - inv.norm) < 1e-6) disp = r.display;
                bool replaced = false; for (auto& w : s.writes) if (w.index == idx) { w.norm = inv.norm; w.display = disp.isNotEmpty() ? disp : juce::String (inv.norm, 4); w.from = "gain draft make-up (+" + juce::String (s.predictedGrDb, 2) + " dB)"; replaced = true; }
                if (! replaced) s.writes.push_back ({ idx, name, disp.isNotEmpty() ? disp : juce::String (inv.norm, 4), "gain draft make-up (+" + juce::String (s.predictedGrDb, 2) + " dB)", inv.norm });
                s.predictedOutDb = std::round ((s.Lrms + s.staticGainDb - s.predictedGrDb + (s.makeupGivesDb - s.makeupNeutralDb)) * 100.0) / 100.0;
            }
        }
    }
    else s.notes.add ("the gain draft has no writable make-up or output control: no make-up written, no output prediction");
    s.ok = true; return s;
}

// THE VARIANTS: the setting with only the attack written (the release as the tone check had it, else at instantiate), and only the release
inline std::vector<Write> writesWithOnly (const Setting& s, const juce::String& role)
{
    std::vector<Write> out;
    const int drop = role == "attack" ? s.releaseIndex : s.attackIndex; const auto& before = role == "attack" ? s.releaseBefore : s.attackBefore;
    for (const auto& w : s.writes) { if (w.index == drop) { if (before) out.push_back (*before); continue; } out.push_back (w); }
    return out;
}
// THE ADDITIVITY CHECK, at one hold: the attack-only and release-only shifts from g, their sum, against the combined shift; within kPassDb = additive
struct Additivity { bool ok = false; double attackOnlyShiftDb = 0.0, releaseOnlyShiftDb = 0.0, sumDb = 0.0, combinedShiftDb = 0.0, missDb = 0.0; bool additive = false; juce::String why; };
inline Additivity additivity (double g, std::optional<double> grAttackOnly, std::optional<double> grReleaseOnly, std::optional<double> grCombined)
{
    Additivity a;
    if (! grAttackOnly || ! grReleaseOnly || ! grCombined) { a.why = "not all three reads landed"; return a; }
    a.ok = true; a.attackOnlyShiftDb = std::round ((*grAttackOnly - g) * 100.0) / 100.0; a.releaseOnlyShiftDb = std::round ((*grReleaseOnly - g) * 100.0) / 100.0;
    a.sumDb = std::round ((a.attackOnlyShiftDb + a.releaseOnlyShiftDb) * 100.0) / 100.0; a.combinedShiftDb = std::round ((*grCombined - g) * 100.0) / 100.0;
    a.missDb = std::round ((a.combinedShiftDb - a.sumDb) * 100.0) / 100.0; a.additive = std::abs (a.missDb) <= kPassDb;
    a.why = (a.additive ? "the shifts add at this hold (" : "the shifts do NOT add at this hold (") + juce::String (a.attackOnlyShiftDb, 2) + " + " + juce::String (a.releaseOnlyShiftDb, 2) + " = " + juce::String (a.sumDb, 2) + " against the combined " + juce::String (a.combinedShiftDb, 2) + ", miss " + juce::String (a.missDb, 2) + ")";
    return a;
}
inline juce::var additivityVar (const Additivity& a, double holdS)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("hold_s", holdS); o->setProperty ("note", "all three reads at this hold: the time draft's shifts came from short bursts; a miss here that the draft did not show means the draft was under-settled, a miss at this hold too means the shifts truly do not add");
    if (! a.ok) { o->setProperty ("why", a.why); return juce::var (o); }
    o->setProperty ("attack_only_shift_db", a.attackOnlyShiftDb); o->setProperty ("release_only_shift_db", a.releaseOnlyShiftDb); o->setProperty ("sum_db", a.sumDb); o->setProperty ("combined_shift_db", a.combinedShiftDb); o->setProperty ("miss_db", a.missDb); o->setProperty ("additive_within_0_5_db", a.additive); o->setProperty ("why", a.why);
    return juce::var (o);
}

// THE MISS: GR and output level, measured against the prediction
struct Miss { bool grRead = false, outRead = false, outPredicted = false, grPass = false; double grMeasuredDb = 0.0, grMissDb = 0.0, outMeasuredDb = 0.0, outMissDb = 0.0; };
inline Miss judge (const Setting& s, std::optional<double> grDb, std::optional<double> outRmsDb)
{
    Miss m;
    if (grDb) { m.grRead = true; m.grMeasuredDb = *grDb; m.grMissDb = std::round ((*grDb - s.predictedGrDb) * 100.0) / 100.0; m.grPass = std::abs (m.grMissDb) <= kPassDb; }
    if (outRmsDb) { m.outRead = true; m.outMeasuredDb = *outRmsDb; if (s.predictedOutDb) { m.outPredicted = true; m.outMissDb = std::round ((*outRmsDb - *s.predictedOutDb) * 100.0) / 100.0; } }
    return m;
}

inline juce::var settingVar (const Setting& s, const Miss& m)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("g_db", s.g); o->setProperty ("L_rms_dbfs", std::round (s.Lrms * 100.0) / 100.0); o->setProperty ("L_peak_dbfs", std::round (s.Lpeak * 100.0) / 100.0);
    o->setProperty ("amount_control", s.amountControl); o->setProperty ("amount_norm", s.amountNorm);
    o->setProperty ("attack", s.attackDisplay); o->setProperty ("attack_gr_shift_db", s.attackShiftDb); o->setProperty ("release", s.releaseDisplay); o->setProperty ("release_gr_shift_db", s.releaseShiftDb);
    o->setProperty ("attack_ms", s.attackMs); o->setProperty ("release_ms", s.releaseMs); o->setProperty ("hold_s", s.holdS);
    o->setProperty ("gr_shift_sign", "gr_shift_db = GR step at the position minus the GR step at the instantiate position: positive = MORE gain reduction at that position");
    o->setProperty ("makeup_control", s.makeupControl.isNotEmpty() ? juce::var (s.makeupControl) : juce::var()); o->setProperty ("makeup_target_db", std::round (s.makeupTargetDb * 100.0) / 100.0); o->setProperty ("makeup_gives_db", std::round (s.makeupGivesDb * 100.0) / 100.0); if (s.makeupClamped) o->setProperty ("makeup_clamped", true);
    o->setProperty ("static_gain_db", s.staticGainDb);
    o->setProperty ("predicted_gr_db", s.predictedGrDb); o->setProperty ("predicted_out_rms_dbfs", s.predictedOutDb ? juce::var (*s.predictedOutDb) : juce::var());
    o->setProperty ("prediction", "GR = g + attack gr_shift + release gr_shift; out = L + static_gain - GR + (make-up written - make-up at neutral)");
    juce::Array<juce::var> ws; for (const auto& w : s.writes) { auto* wo = new juce::DynamicObject(); wo->setProperty ("index", w.index); wo->setProperty ("control", w.control); wo->setProperty ("norm", w.norm); wo->setProperty ("set", w.display); wo->setProperty ("from", w.from); ws.add (juce::var (wo)); }
    o->setProperty ("writes", ws);
    if (m.grRead) { o->setProperty ("gr_measured_db", std::round (m.grMeasuredDb * 100.0) / 100.0); o->setProperty ("gr_miss_db", m.grMissDb); o->setProperty ("gr_within_0_5_db", m.grPass); }
    if (m.outRead) { o->setProperty ("out_measured_rms_dbfs", std::round (m.outMeasuredDb * 100.0) / 100.0); if (m.outPredicted) o->setProperty ("out_miss_db", m.outMissDb); }
    juce::Array<juce::var> ns; for (const auto& n : s.notes) ns.add (n); o->setProperty ("notes", ns);
    return juce::var (o);
}

} // namespace ejmap::combined
