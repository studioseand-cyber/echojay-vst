/*
  EjmapRoleEvidence.h - NAMES PROPOSE, MEASUREMENT DECIDES (Kathy's ruling, 5 Oct 2026 evening). Every Phase B prototype
  picks its controls by name; run 2 found a case in each of R3-R7 where the name was wrong (Saphira's band gains as drives,
  bx_delay2500's "Modulation Mix", H-Reverb's "Predelay Free", TransX's amount hiding as "Range", Quantum's "Attack -
  Vibrato - Mix", MannyM's "DeBoxy Thresh", Melda's 22 "thresholds"). From now a name only NOMINATES a control; it gets
  the role only if its measured effect matches the role's signature, read on two positions (the control's ends, or the
  mode's own full sweep). A nominee that fails is dropped with the reason; an unnamed numeric control that shows the
  signature is reported as "measured role, unnamed" - reported, never swept as the role without a ruling.

  PURE: a Figure is what one position measured (every field optional: a mode fills what it reads); signatureHolds says
  whether two Figures show a role's movement, and why; verdicts are words on the record.

  The signatures (kept here, in one place, so every mode reads them the same way):
    gain       the level moves >= 1 dB, and by the same amount at two levels (within 1 dB): a plain gain, not a path
    threshold  where the effect starts moves: GR at a fixed level moves >= 3 dB
    mix        the dry/wet ratio moves >= 6 dB, dry falling while wet rises
    decay      RT60 moves by a factor >= 1.5
    time       the FIRST REPEAT'S ONSET (the rising-edge lag from the burst) moves >= 10 ms and >= 20 % (ruling 2: never the spacing)
    tone       the onset stays while the repeats' shape or level moves (first repeat >= 3 dB, or the fall per repeat >= 3 dB): the feedback path's tone
    feedback   the fall per repeat moves >= 3 dB with the first repeat put (within 6 dB), or the repeat count by >= 3 with the first repeat put
    drive      THD moves >= 3 dB and reaches at least -50 dB (0.3 %: J37's slap level and Formula move THD 5-8 dB under -56 - not a drive)
    frequency  the centre (or corner) moves >= 1/3 octave while the band stays (gain within 3 dB)
    q          the bandwidth moves >= 30 % while the centre stays within 1/3 octave
    attack / release / hold   the timing figure moves >= 50 % (both ends measured)
    transient / sustain       the figure moves >= 2 dB
    range      the closed level moves >= 6 dB
    ceiling    the output peak under drive moves >= 1 dB
    global     the whole-unit GR moves >= 1 dB
*/

#pragma once

#include <juce_core/juce_core.h>
#include <optional>
#include <vector>
#include <map>

namespace ejmap::roleevidence
{

struct Figure
{
    bool ok = false;                                 // the position was measured at all
    std::optional<double> levelDb, levelDbQuiet;     // out - in at the reference level, and at a quieter one (gain's level independence)
    std::optional<double> grDb;                      // gain reduction at a fixed level (threshold / band threshold / global)
    std::optional<double> dryDb, wetDb;              // mix
    std::optional<double> rt60s;                     // decay
    std::optional<double> onsetMs;                   // time (pre-delay / delay time / repeat spacing)
    std::optional<double> fallPerRepeatDb; std::optional<int> repeats; std::optional<double> firstRepeatDb;   // feedback (the first repeat's level: a count that grew because every repeat got louder is a gain)
    std::optional<double> thdDb, sidebandDb;         // drive: THD at the harmonic bins; sidebandDb = energy beside the fundamental and the harmonics, relative to the fundamental (modulation)
    std::optional<double> centreHz, bandwidthOct, bandGainDb;   // frequency / q / eq gain
    std::optional<double> attackMs, releaseMs, holdMs;          // timing
    std::optional<double> transientDb, sustainDb;    // shapers
    std::optional<double> openLevelDb, rangeDb, openGainDb;   // gates (openGainDb: the gain when open - a range control leaves it; an output gain moves it)
    std::optional<double> peakDb;                    // ceiling
    std::optional<double> peakDriveDeltaDb;          // ceiling: how far the output peak moves when the DRIVE moves 6 dB at this position (a ceiling holds it, a gain passes it)
    std::optional<double> outputDb;                  // the output's absolute level at this position: a silent end (below kSilentOutputDb) reads nothing (THD on noise, a "band" on nothing)
    std::optional<double> levelShiftDb;              // eq: the median deviation over the whole grid at this position (a level shift, not a band)
};
inline constexpr double kSilentOutputDb = -60.0, kCeilingHoldsDb = 2.0;
inline constexpr int kRepeatCountMove = 3;   // a repeat count that moves by fewer than this (14 -> 16 on bx_delay2500's Wah Amount) is a floor effect

inline constexpr double kGainMoveDb = 1.0, kGainSameDb = 1.0, kThresholdMoveDb = 3.0, kMixMoveDb = 6.0, kDecayRatio = 1.5, kTimeMoveMs = 10.0, kTimeMoveFrac = 0.2,
                        kFeedbackMoveDb = 3.0, kDriveMoveDb = 3.0, kDriveFloorDb = -50.0, kFreqMoveOct = 1.0 / 3.0, kBandStayDb = 3.0, kQMoveFrac = 0.3,
                        kTimingMoveFrac = 0.5, kShaperMoveDb = 2.0, kRangeMoveDb = 6.0, kCeilingMoveDb = 1.0, kGlobalMoveDb = 1.0;

struct Signature { bool holds = false; juce::String why; };

inline juce::String f1 (double v) { return juce::String (v, 1); }
inline juce::String f2 (double v) { return juce::String (v, 2); }

// MODULATION (ruling 1): at the end with the more energy beside the bins, the sideband power exceeds the harmonic power and sits
// above kSidebandFloorDb relative to the fundamental - energy smeared beside the tone (J37's WOW Depth), not harmonics on it.
// The control must ADD the sidebands: they rise by kSidebandRiseDb between its ends (J37's instantiate state already carries
// -32.8 dB of tape hiss beside the tone at every control - a constant floor is the unit's, not a modulation control's).
inline constexpr double kSidebandFloorDb = -40.0, kSidebandRiseDb = 12.0;
inline Signature modulationOf (const Figure& a, const Figure& b)
{
    Signature s;
    if (! a.ok || ! b.ok) { s.why = "not measured at both positions"; return s; }
    if (! a.sidebandDb || ! b.sidebandDb) { s.why = "no sideband reading at both ends (the probe printed no total)"; return s; }
    const double lo = juce::jmin (*a.sidebandDb, *b.sidebandDb), hi = juce::jmax (*a.sidebandDb, *b.sidebandDb);
    const Figure& e = *b.sidebandDb >= *a.sidebandDb ? b : a; const double harm = e.thdDb ? *e.thdDb : -200.0;
    if (hi > kSidebandFloorDb && hi - juce::jmax (-200.0, lo) >= kSidebandRiseDb && hi > harm) { s.holds = true; s.why = "energy beside the fundamental rises " + f1 (lo) + " -> " + f1 (hi) + " dB (relative) over the harmonics' " + f1 (harm) + ": modulation, not drive"; return s; }
    s.why = "sidebands " + f1 (lo) + " -> " + f1 (hi) + " dB" + (hi > harm && hi > kSidebandFloorDb ? " (the unit's own floor: no rise with the control)" : " under the harmonics' " + f1 (harm)); return s;
}
// Does the pair (a at one end, b at the other) show the role's movement?
inline Signature signatureHolds (const juce::String& role, const Figure& a, const Figure& b)
{
    Signature s;
    if (! a.ok || ! b.ok) { s.why = "not measured at both positions"; return s; }
    auto both = [] (const std::optional<double>& x, const std::optional<double>& y) { return x && y; };
    if (role == "gain" || role == "output" || role == "makeup" || role == "input")
    {
        if (! both (a.levelDb, b.levelDb)) { s.why = "no level reading"; return s; }
        const double d = *b.levelDb - *a.levelDb;
        if (std::abs (d) < kGainMoveDb) { s.why = "the level moves " + f2 (std::abs (d)) + " dB between the ends (needs " + f1 (kGainMoveDb) + ")"; return s; }
        if (both (a.levelDbQuiet, b.levelDbQuiet)) { const double dq = *b.levelDbQuiet - *a.levelDbQuiet; if (std::abs (d - dq) > kGainSameDb) { s.why = "the level moves " + f2 (d) + " dB at one level and " + f2 (dq) + " at the quieter: a path, not a plain gain"; return s; } }
        s.holds = true; s.why = "the level moves " + f2 (d) + " dB" + (both (a.levelDbQuiet, b.levelDbQuiet) ? ", the same at the quieter level" : juce::String()); return s;
    }
    if (role == "threshold" || role == "band_threshold")
    {
        if (! both (a.grDb, b.grDb)) { s.why = "no GR reading"; return s; }
        const double d = std::abs (*b.grDb - *a.grDb);
        s.holds = d >= kThresholdMoveDb; s.why = "GR moves " + f2 (d) + " dB between the ends" + (s.holds ? "" : " (needs " + f1 (kThresholdMoveDb) + ")"); return s;
    }
    if (role == "global")
    {
        if (! both (a.grDb, b.grDb)) { s.why = "no whole-unit GR reading"; return s; }
        const double d = std::abs (*b.grDb - *a.grDb);
        s.holds = d >= kGlobalMoveDb; s.why = "the whole-unit GR moves " + f2 (d) + " dB" + (s.holds ? "" : " (needs " + f1 (kGlobalMoveDb) + ")"); return s;
    }
    if (role == "mix")
    {
        // a mix's dry end has no wet and its wet end no dry: an absent reading on the side the law removes is the floor, not a gap
        if (! a.dryDb && ! b.dryDb) { s.why = "no dry reading at either end"; return s; }
        if (! a.wetDb && ! b.wetDb) { s.why = "no wet reading at either end"; return s; }
        const double aDry = a.dryDb ? *a.dryDb : -120.0, bDry = b.dryDb ? *b.dryDb : -120.0, aWet = a.wetDb ? *a.wetDb : -120.0, bWet = b.wetDb ? *b.wetDb : -120.0;
        const double ra = aWet - aDry, rb = bWet - bDry, d = rb - ra;
        const bool opposite = (bDry - aDry) * (bWet - aWet) <= 0.0;
        if (std::abs (d) < kMixMoveDb) { s.why = "the wet/dry ratio moves " + f2 (std::abs (d)) + " dB (needs " + f1 (kMixMoveDb) + ")"; return s; }
        if (! opposite) { s.why = "dry and wet move the same way (" + f2 (bDry - aDry) + " / " + f2 (bWet - aWet) + " dB): a level, not a mix"; return s; }
        s.holds = true; s.why = "the wet/dry ratio moves " + f2 (d) + " dB, dry and wet opposite"; return s;
    }
    if (role == "decay")
    {
        if (! both (a.rt60s, b.rt60s)) { s.why = "RT60 not read at both ends"; return s; }
        const double r = juce::jmax (*a.rt60s, *b.rt60s) / juce::jmax (1e-6, juce::jmin (*a.rt60s, *b.rt60s));
        s.holds = r >= kDecayRatio; s.why = "RT60 " + f2 (*a.rt60s) + " -> " + f2 (*b.rt60s) + " s (x" + f2 (r) + (s.holds ? ")" : ", needs x" + f1 (kDecayRatio) + ")"); return s;
    }
    if (role == "time" || role == "predelay")
    {
        if (! both (a.onsetMs, b.onsetMs)) { s.why = "no onset at both ends"; return s; }
        const double d = std::abs (*b.onsetMs - *a.onsetMs), base = juce::jmax (1.0, juce::jmin (*a.onsetMs, *b.onsetMs));
        s.holds = d >= kTimeMoveMs && d / base >= kTimeMoveFrac; s.why = "the onset moves " + f1 (*a.onsetMs) + " -> " + f1 (*b.onsetMs) + " ms" + (s.holds ? "" : " (needs >= 10 ms and 20 %)"); return s;
    }
    if (role == "tone")
    {
        // ruling 2: the onset holds, the repeats change - a filter or a level in the feedback path (bx_delay2500's Feedback Hi Pass / Low Pass)
        if (! both (a.onsetMs, b.onsetMs)) { s.why = "no onset at both ends"; return s; }
        const double d = std::abs (*b.onsetMs - *a.onsetMs), base = juce::jmax (1.0, juce::jmin (*a.onsetMs, *b.onsetMs));
        if (d >= kTimeMoveMs && d / base >= kTimeMoveFrac) { s.why = "the onset moves (" + f1 (*a.onsetMs) + " -> " + f1 (*b.onsetMs) + " ms): a time, not a tone"; return s; }
        if (both (a.firstRepeatDb, b.firstRepeatDb) && std::abs (*b.firstRepeatDb - *a.firstRepeatDb) >= kFeedbackMoveDb) { s.holds = true; s.why = "the onset holds at " + f1 (*a.onsetMs) + " ms while the first repeat moves " + f2 (*a.firstRepeatDb) + " -> " + f2 (*b.firstRepeatDb) + " dB: tone in the feedback path, not time"; return s; }
        if (both (a.fallPerRepeatDb, b.fallPerRepeatDb) && std::abs (*b.fallPerRepeatDb - *a.fallPerRepeatDb) >= kFeedbackMoveDb) { s.holds = true; s.why = "the onset holds at " + f1 (*a.onsetMs) + " ms while the fall per repeat moves " + f2 (*a.fallPerRepeatDb) + " -> " + f2 (*b.fallPerRepeatDb) + " dB: tone in the feedback path, not time"; return s; }
        s.why = "the onset holds and the repeats hold too"; return s;
    }
    if (role == "feedback")
    {
        // ruling 2: a feedback control acts from the second repeat on - the first repeat stays put (a filter in the path moves it: tone)
        const bool firstPut = both (a.firstRepeatDb, b.firstRepeatDb) && std::abs (*b.firstRepeatDb - *a.firstRepeatDb) <= kMixMoveDb;
        if (both (a.fallPerRepeatDb, b.fallPerRepeatDb) && std::abs (*b.fallPerRepeatDb - *a.fallPerRepeatDb) >= kFeedbackMoveDb)
        {
            if (both (a.firstRepeatDb, b.firstRepeatDb) && ! firstPut) { s.why = "the fall per repeat moves " + f2 (*a.fallPerRepeatDb) + " -> " + f2 (*b.fallPerRepeatDb) + " dB but the first repeat moves " + f2 (*b.firstRepeatDb - *a.firstRepeatDb) + " too: the feedback path's tone or level, not feedback"; return s; }
            s.holds = true; s.why = "the fall per repeat moves " + f2 (*a.fallPerRepeatDb) + " -> " + f2 (*b.fallPerRepeatDb) + " dB" + (firstPut ? " with the first repeat put" : ""); return s;
        }
        // the count alone counts only when the FIRST repeat stayed put (within 6 dB): a gain lifts every repeat over the floor and the count grows too (bx_delay2500's Gain In 0 -> 16)
        if (both (a.repeats, b.repeats) && std::abs (*b.repeats - *a.repeats) >= kRepeatCountMove)
        {
            if (! both (a.firstRepeatDb, b.firstRepeatDb)) { s.why = "the repeat count moves " + juce::String (*a.repeats) + " -> " + juce::String (*b.repeats) + " but the first repeat is not read at both ends: a level or a mute, not decided as feedback"; return s; }
            if (std::abs (*b.firstRepeatDb - *a.firstRepeatDb) > kMixMoveDb) { s.why = "the repeat count moves " + juce::String (*a.repeats) + " -> " + juce::String (*b.repeats) + " with the first repeat moving " + f2 (*b.firstRepeatDb - *a.firstRepeatDb) + " dB: a level, not feedback"; return s; }
            s.holds = true; s.why = "the repeat count moves " + juce::String (*a.repeats) + " -> " + juce::String (*b.repeats) + " with the first repeat put (" + f2 (*b.firstRepeatDb - *a.firstRepeatDb) + " dB)"; return s;
        }
        s.why = both (a.repeats, b.repeats) ? "repeats " + juce::String (*a.repeats) + " -> " + juce::String (*b.repeats) + (both (a.fallPerRepeatDb, b.fallPerRepeatDb) ? ", fall " + f2 (*a.fallPerRepeatDb) + " -> " + f2 (*b.fallPerRepeatDb) + " dB" : juce::String()) + ": no feedback movement" : "no repeat reading at both ends"; return s;
    }
    if (role == "drive")
    {
        if (! both (a.thdDb, b.thdDb)) { s.why = "no THD reading at both ends"; return s; }
        if ((a.outputDb && *a.outputDb < kSilentOutputDb) || (b.outputDb && *b.outputDb < kSilentOutputDb)) { s.why = "the output is below " + f1 (kSilentOutputDb) + " dBFS at one end (J37's Output Level at minimum): THD there is noise, not a reading"; return s; }
        // DRIVE NEEDS A STEADY TONE (ruling 1, 5 Oct evening): harmonics rise at exact multiples while the fundamental holds; energy
        // smeared BESIDE the fundamental (modulation sidebands) is not drive - the driven end's sideband power must stay under its harmonic power
        if (const auto m = modulationOf (a, b); m.holds) { s.why = m.why; return s; }
        const double d = std::abs (*b.thdDb - *a.thdDb), top = juce::jmax (*a.thdDb, *b.thdDb);
        if (top < kDriveFloorDb) { s.why = "THD never above " + f1 (kDriveFloorDb) + " dB (top " + f1 (top) + ")"; return s; }
        s.holds = d >= kDriveMoveDb; s.why = "THD moves " + f1 (*a.thdDb) + " -> " + f1 (*b.thdDb) + " dB" + (s.holds ? "" : " (needs " + f1 (kDriveMoveDb) + ")"); return s;
    }
    if (role == "frequency")
    {
        if (! both (a.centreHz, b.centreHz)) { s.why = "no centre at both ends (the band must be audible at both)"; return s; }
        const double oct = std::abs (std::log2 (*b.centreHz / juce::jmax (1.0, *a.centreHz)));
        if (both (a.bandGainDb, b.bandGainDb) && std::abs (*b.bandGainDb - *a.bandGainDb) > kBandStayDb) { s.why = "the band's gain moves " + f2 (*b.bandGainDb - *a.bandGainDb) + " dB with it: not a frequency alone"; return s; }
        s.holds = oct >= kFreqMoveOct; s.why = "the centre moves " + juce::String (*a.centreHz, 0) + " -> " + juce::String (*b.centreHz, 0) + " Hz (" + f2 (oct) + " oct" + (s.holds ? ")" : ", needs 1/3)"); return s;
    }
    if (role == "q")
    {
        if (! both (a.bandwidthOct, b.bandwidthOct)) { s.why = "no bandwidth at both ends"; return s; }
        const double frac = std::abs (*b.bandwidthOct - *a.bandwidthOct) / juce::jmax (1e-6, juce::jmin (*a.bandwidthOct, *b.bandwidthOct));
        if (both (a.centreHz, b.centreHz) && std::abs (std::log2 (*b.centreHz / juce::jmax (1.0, *a.centreHz))) > kFreqMoveOct) { s.why = "the centre moves with it: a frequency, not a Q"; return s; }
        s.holds = frac >= kQMoveFrac; s.why = "the bandwidth moves " + f2 (*a.bandwidthOct) + " -> " + f2 (*b.bandwidthOct) + " oct" + (s.holds ? "" : " (needs 30 %)"); return s;
    }
    if (role == "eq_gain")
    {
        if (! both (a.bandGainDb, b.bandGainDb)) { s.why = "no band gain at both ends"; return s; }
        const double d = std::abs (*b.bandGainDb - *a.bandGainDb);
        if (d < kGainMoveDb) { s.why = "the band's gain moves " + f2 (d) + " dB (needs 1)"; return s; }
        // a band stands out from the grid's overall level; an input / output gain moves every tone alike (bx_digital's Input Gain read as a 12 dB "band")
        if (both (a.levelShiftDb, b.levelShiftDb)) { const double dl = std::abs (*b.levelShiftDb - *a.levelShiftDb); if (std::abs (d - dl) < kGainMoveDb) { s.why = "every tone moves alike (" + f2 (dl) + " dB over the grid, the 'band' " + f2 (d) + "): a level, not a band"; return s; } }
        s.holds = true; s.why = "the band's gain moves " + f2 (d) + " dB"; return s;
    }
    if (role == "attack" || role == "release" || role == "hold")
    {
        const auto& xa = role == "attack" ? a.attackMs : role == "release" ? a.releaseMs : a.holdMs;
        const auto& xb = role == "attack" ? b.attackMs : role == "release" ? b.releaseMs : b.holdMs;
        if (! both (xa, xb)) { s.why = "the " + role + " figure was not measured at both ends"; return s; }
        const double frac = std::abs (*xb - *xa) / juce::jmax (1e-6, juce::jmin (*xa, *xb));
        s.holds = frac >= kTimingMoveFrac; s.why = "the " + role + " moves " + f1 (*xa) + " -> " + f1 (*xb) + " ms" + (s.holds ? "" : " (needs 50 %)"); return s;
    }
    if (role == "transient" || role == "sustain")
    {
        const auto& xa = role == "transient" ? a.transientDb : a.sustainDb; const auto& xb = role == "transient" ? b.transientDb : b.sustainDb;
        if (! both (xa, xb)) { s.why = "no " + role + " figure at both ends"; return s; }
        const double d = std::abs (*xb - *xa);
        if (d < kShaperMoveDb) { s.why = "the " + role + " moves " + f2 (d) + " dB (needs 2)"; return s; }
        // a shaper moves the transient and the sustain DIFFERENTLY; an output level moves both alike (Smack Attack's Output: 48 / 48 dB)
        const auto& oa = role == "transient" ? a.sustainDb : a.transientDb; const auto& ob = role == "transient" ? b.sustainDb : b.transientDb;
        if (both (oa, ob)) { const double other = std::abs (*ob - *oa); if (std::abs (d - other) < kShaperMoveDb) { s.why = "the " + role + " moves " + f2 (d) + " dB and the " + (role == "transient" ? "sustain" : "transient") + " " + f2 (other) + ": a level, not a shaper"; return s; } }
        s.holds = true; s.why = "the " + role + " moves " + f2 (d) + " dB" + (both (oa, ob) ? " (the " + juce::String (role == "transient" ? "sustain" : "transient") + " " + f2 (std::abs (*ob - *oa)) + ")" : juce::String()); return s;
    }
    if (role == "gate_threshold")
    {
        if (! both (a.openLevelDb, b.openLevelDb)) { s.why = "the gate did not open at both ends"; return s; }
        const double d = std::abs (*b.openLevelDb - *a.openLevelDb);
        s.holds = d >= kRangeMoveDb; s.why = "the open level moves " + f1 (d) + " dB" + (s.holds ? "" : " (needs 6)"); return s;
    }
    if (role == "range")
    {
        if (! both (a.rangeDb, b.rangeDb)) { s.why = "no closed level at both ends"; return s; }
        const double d = std::abs (*b.rangeDb - *a.rangeDb);
        if (d < kRangeMoveDb) { s.why = "the closed level moves " + f1 (d) + " dB (needs 6)"; return s; }
        // a range control moves the CLOSED level and leaves the open one; an output gain moves both (G8's Output Gain read as a 147 dB "range")
        if (both (a.openGainDb, b.openGainDb) && std::abs (*b.openGainDb - *a.openGainDb) >= kRangeMoveDb) { s.why = "the closed level moves " + f1 (d) + " dB but so does the open level (" + f1 (*b.openGainDb - *a.openGainDb) + "): a level, not a range"; return s; }
        s.holds = true; s.why = "the closed level moves " + f1 (d) + " dB" + (both (a.openGainDb, b.openGainDb) ? ", the open level put (" + f1 (*b.openGainDb - *a.openGainDb) + ")" : juce::String()); return s;
    }
    if (role == "ceiling")
    {
        if (! both (a.peakDb, b.peakDb)) { s.why = "no output peak at both ends"; return s; }
        const double d = std::abs (*b.peakDb - *a.peakDb);
        if (d < kCeilingMoveDb) { s.why = "the output peak moves " + f2 (d) + " dB (needs 1)"; return s; }
        // a ceiling HOLDS the peak when the drive moves; a post gain passes the drive through (bx_limiter's Gain and Output Dim moved the peak like a ceiling)
        if (b.peakDriveDeltaDb && std::abs (*b.peakDriveDeltaDb) > kCeilingHoldsDb) { s.why = "the peak moves " + f2 (d) + " dB with the control but follows the drive by " + f2 (*b.peakDriveDeltaDb) + " dB: a gain, not a ceiling"; return s; }
        s.holds = true; s.why = "the output peak moves " + f2 (d) + " dB" + (b.peakDriveDeltaDb ? " and holds against a 6 dB drive change (" + f2 (*b.peakDriveDeltaDb) + ")" : juce::String()); return s;
    }
    s.why = "no signature defined for role '" + role + "'"; return s;
}

// THE VERDICT ON A CONTROL: a nominee confirmed or dropped, an unnamed control that showed the signature.
struct RoleVerdict { int index = -1; juce::String name, role, verdict, reason; };   // verdict: confirmed | dropped | measured_unnamed | not_probed
inline RoleVerdict nominee (int index, const juce::String& name, const juce::String& role, const Signature& s)
{ RoleVerdict v; v.index = index; v.name = name; v.role = role; v.verdict = s.holds ? "confirmed" : "dropped"; v.reason = s.why; return v; }
inline RoleVerdict unnamed (int index, const juce::String& name, const juce::String& role, const Signature& s)
{ RoleVerdict v; v.index = index; v.name = name; v.role = role; v.verdict = s.holds ? "measured_unnamed" : "not_" + role; v.reason = s.why; return v; }

inline juce::var toVar (const std::vector<RoleVerdict>& vs)
{
    juce::Array<juce::var> a;
    for (const auto& v : vs) { auto* o = new juce::DynamicObject(); o->setProperty ("index", v.index); o->setProperty ("control", v.name); o->setProperty ("role", v.role); o->setProperty ("verdict", v.verdict); o->setProperty ("reason", v.reason); a.add (juce::var (o)); }
    return a;
}

// MEASUREMENT NOMINATES (Kathy's 6 Oct ruling, the Phase B fallback): when a mode's lexicon finds nothing, every sampled
// numeric control is read at its two ends with the mode's own probe, and one whose ends show the role's signature IS the
// nominee - recorded "unnamed". Energy beside the tone is modulation, never a drive. Nothing holds, nothing is nominated.
inline std::optional<RoleVerdict> measurementNominates (int index, const juce::String& name, const juce::String& role, const Figure& a, const Figure& b)
{
    if (role == "drive" && modulationOf (a, b).holds) return std::nullopt;
    const auto sig = signatureHolds (role, a, b);
    if (! sig.holds) return std::nullopt;
    auto v = unnamed (index, name, role, sig); v.reason = "nominated by measurement (the lexicon found nothing): " + sig.why;
    return v;
}
inline juce::String line (const RoleVerdict& v)
{
    return (v.verdict == "confirmed" ? "role " + v.role + " CONFIRMED  " : v.verdict == "dropped" ? "role " + v.role + " DROPPED    " : v.verdict == "measured_unnamed" ? "measured role " + v.role + ", UNNAMED  " : "not " + v.role + "  ")
         + "[" + juce::String (v.index) + "] " + v.name + " - " + v.reason;
}
// what the record keeps: only the verdicts that say something (confirmed, dropped, measured_unnamed); the "not_" ones are counted
inline std::vector<RoleVerdict> keep (const std::vector<RoleVerdict>& all, int& notShown)
{
    std::vector<RoleVerdict> out; notShown = 0;
    for (const auto& v : all) { if (v.verdict == "confirmed" || v.verdict == "dropped" || v.verdict == "measured_unnamed") out.push_back (v); else ++notShown; }
    return out;
}

} // namespace ejmap::roleevidence
