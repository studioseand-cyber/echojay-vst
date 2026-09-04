/*
  EchoJayParamApply.h

  Phase 3 apply function for EchoJay auto-parameter-mapping.

  The write-side mirror of EchoJayParamExtractor.h. Given a loaded plugin, its
  map (built by build_maps.py, keyed by the same fingerprint), and a set of
  semantic settings from EchoJay (e.g. { "ratio": "4:1", "attack_ms": 30 }),
  it interpolates each semantic value to a normalized 0..1 and writes it via the
  host's normal parameter-write path.

  How each map entry is applied:
    - "anchored" (ratio, log curves): interpolate the semantic value against the
      [value, normalized] anchor table. Clamp to the table's ends.
    - "linear" (dB, %, and log time stored as anchors): same interpolation; the
      anchor table already captures any curve, so we treat both the same way.
    - "position": set by step index (value is 1-based position; convert to 0..1).

  This does the reverse of discovery: discovery read getText across normalized;
  apply searches those same anchors for the normalized that yields a target.

  Reporting: returns a per-parameter result so the UI can say what was applied
  vs left manual.

  House style: no em-dashes.

  Requires JUCE modules: juce_audio_processors, juce_core.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

namespace echojay
{

struct ApplyResult
{
    juce::String semantic;   // "ratio", "threshold_db", a band key, or a control name
    // The value the request ASKED for, verbatim. The card is the user's only
    // record of what happened, and it cannot re-derive this from the flat
    // settings object: band values live in bands[i], control values in
    // controls["Name"] - a flat lookup by semantic returns void and the card
    // printed bare repeated labels ("freq Hz, gain dB, freq Hz, gain dB").
    juce::var    requestedValue;
    // WHAT THE CONTROL READ BEFORE THIS WRITE (4 Sep 2026). Captured here
    // because here is the only place it exists: applyOne holds the parameter
    // and writes it, so anything asking afterwards gets the new value. The
    // move log's first attempt used the slot's cached display sweep instead,
    // which is taken once per SEND and therefore predates a slot built in the
    // same turn: every entry came out with an empty before. Read once, before
    // any branch writes.
    juce::String beforeText;
    int          index = -1; // plugin parameter index
    bool         applied = false;
    float        normalized = 0.0f;
    juce::String note;       // human-readable outcome
    // Read-back (this build): what the plugin's display showed after the
    // write, whether that display was compared and matched, and whether the
    // write was REVERTED because it landed wrong.
    juce::String landedText;
    bool         displayVerified  = false; // display text compared and matched
    bool         readbackMismatch = false; // landed wrong; value restored

    /** THE MAP'S ANCHORS CAME FROM A DIFFERENT VERSION (26 Aug 2026).

        Set when the served map carried `anchors_unverified`, which the server
        stamps on a product fallback: the identity asked for has no map, so the
        newest mapped identity of the same product (format|uid) is served in its
        place, tagged with `served_from`.

        MAP_CARRY_FORWARD measured what survives a version bump across 539
        two-version products: the control SURFACE held everywhere (the names
        dialling resolves by, plus index/kind/range/unit), and the measured
        ANCHORS drifted on ~19%. So the write is worth making and the number
        is not worth asserting.

        The consequence is a reporting one and it is deliberate: the honesty
        floor's "a successful write shows NOTHING extra" rule (ChainHost.cpp,
        above appliedSummary.add) must NOT apply here. Silence there means
        "this landed as asked", and on a ~19% drift that is a claim we cannot
        make. An unverified-anchor dial says so on the card and to the model. */
    bool         anchorsUnverified = false;

    /** Bridged-AU report-only outcome (10 Aug 2026): verification DISAGREED
        or could not run, but every in-stack read on this instance is
        structurally pre-write (DEFECT_BRIDGED_READBACK; measured 10 Aug:
        display text AND getValue() both stale on API-2500), so the write
        was KEPT instead of reverted -- a revert here can only undo correct
        work. Nothing in-stack can verify OR falsify on the bridge; the card
        carries that caveat and the settle machinery downstream owns the
        truth. Distinct from displayVerified=false alone (the setread class,
        where verification ran and passed by norm).
    */
    bool         staleDisplayKept = false;
    // Range validation (12 Aug 2026): the asked value fell outside the live
    // map's anchor range for this control, nothing was written, the control
    // went manual. Counted per slot on the EJRangeCheck line.
    bool         outOfRange       = false;
};

// ---------------------------------------------------------------------------
// Read-back comparison. Display text is compared TYPED against the requested
// semantic value, in the unit the semantic key implies. A normalised
// round-trip would NOT have caught the mpressor threshold bug: the norm we
// wrote is the norm we would read back. Only the plugin's own display says
// what actually landed.
// ---------------------------------------------------------------------------

// Unit implied by a semantic key: "db", "ms", "s", "hz", "pct", "ratio",
// or "" for unitless semantics (sensitivity, drive).
inline juce::String semanticUnit (const juce::String& key)
{
    if (key == "ratio")        return "ratio";
    if (key.endsWith ("_db"))  return "db";
    if (key.endsWith ("_ms"))  return "ms";
    if (key.endsWith ("_hz"))  return "hz";
    if (key.endsWith ("_pct")) return "pct";
    if (key.endsWith ("_s"))   return "s";
    return {};
}

// Parse a plugin display string into the semantic's unit. Handles the same
// display quirks the server parser does: us/microseconds under a ms target,
// s<->ms flips, bare-k and NkM frequency forms under an hz target, "-oo" /
// "-inf" / infinity glyphs at a dB bottom, "N:1" ratios, comma decimals.
// A "k" NEVER multiplies under a non-hz target (structural guard).
inline bool parseDisplayForUnit (const juce::String& text, const juce::String& unit,
                                 float& out, bool& negInf)
{
    negInf = false;
    auto t = text.trim().toLowerCase();
    if (t.isEmpty()) return false;
    if (unit == "db" && (t.contains ("-oo") || t.contains ("-inf")
                         || t.containsChar ((juce::juce_wchar) 0x221e))) // infinity
    {
        negInf = true;
        return true;
    }
    if (unit == "ratio")
    {
        const int colon = t.indexOfChar (':');
        if (colon > 0) t = t.substring (0, colon);
    }
    int start = -1;
    for (int i = 0; i < t.length(); ++i)
        if (t[i] >= '0' && t[i] <= '9') { start = i; break; }
    if (start < 0) return false;
    if (start > 0 && (t[start - 1] == '-' || t[start - 1] == '+')) --start;

    juce::String num;
    bool dot = false;
    int i = start;
    if (t[i] == '-' || t[i] == '+') { num << t[i]; ++i; }
    for (; i < t.length(); ++i)
    {
        const auto c = t[i];
        if (c >= '0' && c <= '9') num << c;
        else if ((c == '.' || c == ',') && ! dot
                 && i + 1 < t.length() && t[i + 1] >= '0' && t[i + 1] <= '9')
        { num << '.'; dot = true; }
        else break;
    }
    float v = num.getFloatValue();

    // NkM form under hz only: "1k1" means 1100
    if (unit == "hz" && i < t.length() && t[i] == 'k'
        && i + 1 < t.length() && t[i + 1] >= '0' && t[i + 1] <= '9')
    {
        out = v * 1000.0f + (float) (t[i + 1] - '0') * 100.0f;
        return true;
    }

    // unit token: leading run of letters / '%' / micro sign after the number
    juce::String ut;
    for (int j = i; j < t.length(); ++j)
    {
        const auto c = t[j];
        if ((c >= 'a' && c <= 'z') || c == '%' || c == (juce::juce_wchar) 0xb5) ut << c;
        else if (ut.isEmpty() && (c == ' ' || c == '\t')) continue;
        else break;
    }
    const juce::String micro = juce::String::charToString ((juce::juce_wchar) 0xb5);
    if (unit == "ms")
    {
        if (ut == "us" || ut == micro + "s") v /= 1000.0f;
        else if (ut == "s" || ut == "sec")   v *= 1000.0f;
    }
    else if (unit == "s")
    {
        if (ut == "ms")      v /= 1000.0f;
        else if (ut == "us" || ut == micro + "s") v /= 1.0e6f;
    }
    else if (unit == "hz")
    {
        if (ut == "khz" || ut == "k") v *= 1000.0f;
    }
    out = v;
    return true;
}

// Compare a landed display against the requested value. Tolerance comes from
// the map's own resolution, nothing looser: half the anchor gap bracketing
// the target (actual interpolation resolution) with a 2%-of-span floor for
// display rounding and smoothing lag. Deliberately NO percent-of-target
// term: on -18 dB that would permit +/-1.8 dB, wide enough to pass exactly
// the wrong-write class this comparison exists to catch.
//
// NEAREST-STEP ACCEPTANCE (1 Aug 2026). A snapped display lands ON a step,
// and a target at the exact midpoint of its bracket sits exactly at the
// half-gap tolerance, where float representation decides the verdict: AMEK's
// stepped Q ladder, asked 0.7, snapped up to "0.8", |diff| beat tol by ~3e-8
// and a correct best-effort write was reverted, deterministically, on every
// dial (the fleet's partial/manual["q"]). So the two anchors bracketing the
// target are accepted by IDENTITY, ulp-scale epsilon only: landing on either
// step around the target IS the map's own resolution, which is what the
// distance tolerance above was already claiming to measure. The epsilon
// covers text->float round-trip, nothing more; it must stay far below any
// real anchor gap so nearest-step never becomes any-step.
// Returns +1 match, 0 unparseable (cannot verify), -1 mismatch.
inline int typedReadbackMatch (const juce::String& semantic, float target,
                               const juce::String& landedText,
                               const juce::Array<juce::Array<float>>& anchors,
                               const juce::String& unitOverride = {})
{
    // Params encode their unit in the semantic key ("threshold_db"); named
    // controls carry it as an entry field ("Mono Maker", unit "hz"). The
    // override wins when present: a control name derives no unit, and an
    // hz display like "1.2 kHz" parsed unitless reads 1.2 against a target
    // of 1200, reverting a correct write.
    const auto unit = unitOverride.isNotEmpty() ? unitOverride : semanticUnit (semantic);
    float landed = 0.0f;
    bool negInf = false;
    if (! parseDisplayForUnit (landedText, unit, landed, negInf)) return 0;

    juce::Array<float> vals;
    for (auto& a : anchors) vals.add (a[0]);
    vals.sort();
    const float lo = vals.getFirst(), hi = vals.getLast();
    float gap = 0.0f;
    float bracketLo = 0.0f, bracketHi = 0.0f;
    bool haveBracket = false;
    for (int i = 0; i < vals.size() - 1; ++i)
        if (target >= vals[i] && target <= vals[i + 1])
        {
            gap = vals[i + 1] - vals[i];
            bracketLo = vals[i];
            bracketHi = vals[i + 1];
            haveBracket = true;
            break;
        }
    if (gap <= 0.0f && vals.size() >= 2)
        gap = target < lo ? vals[1] - vals[0] : vals[vals.size() - 1] - vals[vals.size() - 2];
    const float tol = juce::jmax (0.02f * (hi - lo), 0.5f * gap);

    if (negInf) return target <= lo + tol ? +1 : -1;

    if (haveBracket)
    {
        auto onStep = [landed] (float a)
        { return std::abs (landed - a) <= juce::jmax (1.0e-5f * std::abs (a), 1.0e-6f); };
        if (onStep (bracketLo) || onStep (bracketHi)) return +1;
    }

    return std::abs (landed - target) <= tol ? +1 : -1;
}

// ---------------------------------------------------------------------------
// Segment handling for non-monotonic anchor tables (the surviving v1 map
// class; v2 maps arrive pre-sorted and pre-truncated by the server
// sanitizer). Client mirror of the server's rule: the longest monotonic
// subsequence of values, either direction, must dominate (>= 60% of the
// table) or the entry is unusable (mirror / garbage shapes are refused, not
// guessed at). Interpolation then runs inside that dominant segment only.
// ---------------------------------------------------------------------------
struct EffectiveAnchors
{
    juce::Array<juce::Array<float>> table;
    bool ok = false;
};

// ---------------------------------------------------------------------------
// STEP LADDERS (4 Sep 2026). An anchor table is a LADDER when its normalised
// axis sits on a uniform grid COARSER than the sampler's own: the map builder
// walked a switch's positions rather than sweeping a continuous control. The
// reachable values are then the anchor values and nothing between them.
//
// This is the server's test one from enumeratedSteps (lib/params-lib.js),
// implemented here because the client's copy of a map carries `anchors` and
// not `steps`: the server derives the list for PRINTING and does not store it,
// so a client that wants to know cannot ask. The two must agree; the constants
// are named after the server's and the rule is the same three lines.
//
// TEST TWO IS DELIBERATELY NOT PORTED. Server-side it asks whether the gaps
// are uneven ENOUGH to be worth printing instead of a span, which is a note
// budget question. A control with an even ladder (a 12-position selector, an
// 8 dB ladder) steps exactly as hard as an uneven one, and refusing to notice
// that would leave 1,428 of the 2,141 ladders in the local registry unguarded.
//
// TESTED AS d * M, NOT 1 / d, for the same reason the server says: stored
// normalised values carry sweep float noise, and the reciprocal multiplies it
// by M squared.
inline juce::Array<float> stepLadderValues (const juce::Array<juce::Array<float>>& table)
{
    juce::Array<float> none;
    const int n = table.size();
    if (n < 2) return none;
    constexpr float kNormEps = 1.0e-5f;
    constexpr int   kSweepPoints = 21;      // the sampler's N for a continuous control

    juce::Array<float> vals, norms;
    for (const auto& pr : table)
    {
        if (pr.size() < 2) return none;
        vals.add (pr[0]);
        norms.add (pr[1]);
    }
    auto sortedNorms = norms;
    std::sort (sortedNorms.begin(), sortedNorms.end());
    float d = 1.0f;
    for (int i = 1; i < n; ++i) d = juce::jmin (d, sortedNorms[i] - sortedNorms[i - 1]);
    if (! (d > kNormEps)) return none;
    for (int i = 1; i < n; ++i)
    {
        const float g = (sortedNorms[i] - sortedNorms[i - 1]) / d;
        if (std::abs (g - std::round (g)) >= kNormEps) return none;
    }
    const int M = (int) std::round (1.0f / d);
    if (std::abs (d * (float) M - 1.0f) >= kNormEps || M < 1 || M > kSweepPoints - 2) return none;

    auto sorted = vals;
    std::sort (sorted.begin(), sorted.end());
    for (int i = 1; i < n; ++i)
        if (! (sorted[i] > sorted[i - 1])) return none;     // a dedupe, not a ladder
    return sorted;
}

// Which rung a request lands on, or -1 when it lands between rungs. EXACT
// ONLY, with a tolerance that exists for float drift and nothing else: the
// map stores 0.100000001490116 where the plugin steps at 0.1, so a bare == on
// a double request would miss every rung it was meant to hit. It is NOT a
// snap: 2 on a ladder of {0.1|0.5|1|5|10|30} is a miss, and is meant to be.
inline int stepLadderIndex (const juce::Array<float>& ladder, float target)
{
    for (int i = 0; i < ladder.size(); ++i)
    {
        const float tol = 1.0e-4f * juce::jmax (1.0f, std::abs (ladder[i]));
        if (std::abs (target - ladder[i]) <= tol) return i;
    }
    return -1;
}

// THE WHOLE DECISION, INCLUDING ITS SENTENCE, in one pure function. applyOne
// needs a loaded plugin and cannot be called from the gate, so a verdict left
// inline there is a branch no test can witness. This is the shipped decision;
// the write path does nothing with a ladder that is not decided here.
struct LadderVerdict
{
    bool         isLadder = false;   // the table is a walked switch
    bool         onRung   = false;   // ... and the request is one of its values
    float        snapped  = 0.0f;    // the STORED rung value, when onRung
    juce::String note;               // the refusal sentence, when not
};

inline LadderVerdict checkStepLadder (const juce::Array<juce::Array<float>>& table, float target)
{
    LadderVerdict v;
    const auto ladder = stepLadderValues (table);
    if (ladder.isEmpty()) return v;                 // continuous: not our business
    v.isLadder = true;
    const int rung = stepLadderIndex (ladder, target);
    if (rung >= 0) { v.onRung = true; v.snapped = ladder[rung]; return v; }
    auto trim = [] (float f) {
        return juce::String (f, 4).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    };
    juce::StringArray rungs;
    for (auto r : ladder) rungs.add (trim (r));
    // Names what was asked AND what was reachable: a refusal the reader cannot
    // act on is barely a refusal.
    v.note = "asked " + trim (target) + ", this control steps {"
           + rungs.joinIntoString ("|") + "}, left manual";
    return v;
}

inline EffectiveAnchors dominantMonotonicTable (const juce::Array<juce::Array<float>>& anchors)
{
    EffectiveAnchors out;
    const int n = anchors.size();
    if (n < 2) return out;

    auto lisIndices = [&anchors, n] (bool descending)
    {
        // O(n^2) longest strictly-monotonic subsequence; tables are tiny.
        std::vector<int> len ((size_t) n, 1), prev ((size_t) n, -1);
        int best = 0;
        for (int i = 1; i < n; ++i)
            for (int j = 0; j < i; ++j)
            {
                const bool okStep = descending ? anchors[i][0] < anchors[j][0]
                                               : anchors[i][0] > anchors[j][0];
                if (okStep && len[(size_t) j] + 1 > len[(size_t) i])
                {
                    len[(size_t) i] = len[(size_t) j] + 1;
                    prev[(size_t) i] = j;
                    if (len[(size_t) i] > len[(size_t) best]) best = i;
                }
            }
        std::vector<int> idx;
        for (int k = best; k >= 0; k = prev[(size_t) k]) idx.push_back (k);
        std::reverse (idx.begin(), idx.end());
        return idx;
    };
    auto asc = lisIndices (false), desc = lisIndices (true);
    const auto& bestIdx = asc.size() >= desc.size() ? asc : desc;
    if ((int) bestIdx.size() < 2) return out;
    if ((float) bestIdx.size() / (float) n < 0.6f) return out;   // mirror/garbage: refuse
    for (int k : bestIdx) out.table.add (anchors[k]);
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// Interpolate a target value against an anchor table [[value, normalized], ...],
// in sweep order (ascending n). Values may run ASCENDING OR DESCENDING: the
// sweep itself tells us the direction (first vs last value), and plenty of
// real controls are inverted (elysia mpressor threshold reads 16.0 dB at
// n=0 and -18.0 dB at n=1; the old ascending-only clamp sent a -18 dB
// request to n=0, the TOP of the range, and the card claimed success).
// Clamps to the ends. Returns normalized 0..1.
// ---------------------------------------------------------------------------
inline float interpolateAnchors (const juce::Array<juce::Array<float>>& anchors,
                                 float target)
{
    if (anchors.isEmpty()) return 0.0f;
    if (anchors.size() == 1) return anchors[0][1];

    const float firstV = anchors.getFirst()[0];
    const float lastV  = anchors.getLast()[0];
    const bool descending = firstV > lastV;

    // Clamp beyond either end, direction-aware.
    if (! descending)
    {
        if (target <= firstV) return anchors.getFirst()[1];
        if (target >= lastV)  return anchors.getLast()[1];
    }
    else
    {
        if (target >= firstV) return anchors.getFirst()[1];
        if (target <= lastV)  return anchors.getLast()[1];
    }

    for (int i = 0; i < anchors.size() - 1; ++i)
    {
        const float v0 = anchors[i][0],     v1 = anchors[i + 1][0];
        const float n0 = anchors[i][1],     n1 = anchors[i + 1][1];
        const bool inSeg = (target >= juce::jmin (v0, v1) && target <= juce::jmax (v0, v1));
        if (inSeg)
        {
            if (v1 == v0) return n0;
            const float frac = (target - v0) / (v1 - v0);   // sign-correct both directions
            return n0 + frac * (n1 - n0);
        }
    }
    return anchors.getLast()[1];
}

// Parse a semantic value from EchoJay's JSON into a float.
// Handles "4:1" -> 4.0, "-18 dB" -> -18.0, 30 -> 30.0, "30" -> 30.0.
inline bool semanticToFloat (const juce::var& value, float& out)
{
    if (value.isDouble() || value.isInt() || value.isInt64())
    {
        out = (float) (double) value;
        return true;
    }
    auto s = value.toString();
    // strip a trailing ":1" ratio form
    auto colon = s.indexOfChar (':');
    if (colon > 0) s = s.substring (0, colon);
    // grab the first number
    juce::String num;
    bool seenDigit = false, seenDot = false, started = false;
    for (auto c : s)
    {
        if (! started && (c == '-' || c == '+')) { num << c; started = true; }
        else if (c >= '0' && c <= '9') { num << c; seenDigit = true; started = true; }
        else if (c == '.' && ! seenDot && started) { num << c; seenDot = true; }
        else if (seenDigit) break;
    }
    if (! seenDigit) return false;
    out = num.getFloatValue();
    return true;
}

// Pull an anchor table out of the map entry var into a JUCE array.
inline juce::Array<juce::Array<float>> anchorsFromVar (const juce::var& entry)
{
    juce::Array<juce::Array<float>> out;
    if (auto* arr = entry.getProperty ("anchors", juce::var()).getArray())
        for (auto& pair : *arr)
            if (auto* p = pair.getArray())
                if (p->size() >= 2)
                {
                    juce::Array<float> a;
                    a.add ((float) (double) (*p)[0]);
                    a.add ((float) (double) (*p)[1]);
                    out.add (a);
                }
    return out;
}

// ---------------------------------------------------------------------------
// Apply one semantic setting to the plugin using its map entry.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Range validation (12 Aug 2026, every turn, not only diverged ones). The
// old behaviour clamped an out-of-range request to the rail and reported it
// APPLIED: with CLA-76's identity planted at bx_townhouse's fingerprint the
// model wrote Release 100, a millisecond value from the other plugin's
// vocabulary, and the clamp moved a seven-position knob to position 7 and
// called it a success. Clamp-to-rail-and-report-applied is wrong whether or
// not a stale fingerprint was involved, so the refusal lives at the VALUE
// level, unconditionally, and not behind a divergence gate.
//
// FOR THE RECORD, why the 100 got this far: refineControls clamps
// server-side against the map it EXPOSED, and the client clamps against the
// LIVE map. Two clamps against two different maps; a value legal in the
// exposed vocabulary sails through the first and hits the second's rail.
//
// What this does NOT cover: a value that is IN range and still semantically
// wrong for this version (Release 3 means something different on both
// plugins). Only the queued server-side exposure echo (pass 2 marking
// fromFp vs fromBulk per slot) can close that, because the client cannot
// see which vocabulary authored the number.
//
// The guard band: STRICT refusal is not safe. Table bounds are float32
// renderings of measured anchors while requests are decimal documentation
// values ("20", "0.7"), a mismatch class of parts-per-million to well under
// 0.1% (the q=0.7 ladder-tolerance defect was decided by ~3e-8). The defect
// class this refuses is MULTIPLES of the span (100 against [1..7]), so a
// 0.1%-of-span band separates them with orders of magnitude to spare.
inline bool valueWithinMappedRange (float target, float loV, float hiV)
{
    const float tol = juce::jmax (1.0e-3f * (hiV - loV),
                                  1.0e-5f * juce::jmax (std::abs (loV), std::abs (hiV)),
                                  1.0e-6f);
    return target >= loV - tol && target <= hiV + tol;
}

// THE LENGTH THE LIVE NAME IS READ AT, one definition (26 Aug 2026).
// buildFallbackLookupJson composes param_names with it and the tagged-path
// assertion below re-reads with it. If these two ever differ, the client would
// compare a string the server never saw, and the assertion could refuse a write
// the server had already judged safe. Same call, same length, same session.
inline constexpr int kParamNameQueryLen = 128;

/** normName from the server's lib/params-lib.js, replicated:

        String(n == null ? "" : n).trim().toLowerCase().replace(/\s+/g, " ")

    Replicated rather than reused, because the client's existing
    normalizeControlName is NOT the same function: it trims and collapses but
    does NOT lowercase, and its call sites make up the difference with
    equalsIgnoreCase. Comparing with that pair happens to give the same answer,
    which is exactly the kind of accidental agreement that stops being true when
    one side is edited. This is the server's rule, whole, in one place.

    Trim, lowercase and collapse are done in a single pass off ONE whitespace
    predicate, so the three can never disagree about what a space is. Residual
    difference, stated rather than hidden: JUCE's isWhitespace and JavaScript's
    \s do not cover byte-identical Unicode sets. Every character they disagree
    about is exotic whitespace inside a plugin parameter name; if one ever
    appears, this errs toward refusing a write, which is the safe direction. */
inline juce::String normNameServerRule (const juce::String& raw)
{
    juce::String out;
    bool lastWasSpace = true;              // true at the start: drops the leading run
    auto cp = raw.getCharPointer();
    for (juce::juce_wchar c; (c = cp.getAndAdvance()) != 0;)
    {
        if (juce::CharacterFunctions::isWhitespace (c))
        {
            if (! lastWasSpace) { out << ' '; lastWasSpace = true; }
        }
        else
        {
            out << juce::CharacterFunctions::toLowerCase (c);
            lastWasSpace = false;
        }
    }
    // Any space this emitted is ASCII, so trimEnd removes the trailing run
    // whatever the source character was.
    return out.trimEnd();
}

// anchorsUnverified rides beside staleDisplayReads and for the same reason:
// both are facts about the SOURCE of this write that every result must carry,
// and threading them as parameters keeps applyOne free of any lookup of its
// own. Set once at the top so every early return carries it.
inline ApplyResult applyOne (juce::AudioPluginInstance& plugin,
                             const juce::String& semantic,
                             const juce::var& mapEntry,
                             const juce::var& value,
                             bool staleDisplayReads = false,
                             bool anchorsUnverified = false)
{
    ApplyResult r; r.semantic = semantic;
    r.anchorsUnverified = anchorsUnverified;
    r.requestedValue = value;
    const int index = (int) mapEntry.getProperty ("index", -1);
    r.index = index;

    auto& params = plugin.getParameters();
    if (index < 0 || index >= params.size() || params[index] == nullptr)
    {
        r.note = "param index not present on this instance";
        return r;
    }
    auto* param = params[index];
    // BEFORE, read once and before every write path below.
    r.beforeText = param->getCurrentValueAsText().trim();

    // SECOND GUARD, TAGGED MAPS ONLY (26 Aug 2026).
    //
    // The fingerprint-integrity check is exempted for a fallback map, because
    // such a map names a different binary by design. That exemption is only
    // safe while something proves the index still means what the map says, and
    // until now the only thing doing so was the server's nameGuard, which this
    // layer cannot see. So it checks for itself: the live parameter's name at
    // the resolved index against the map entry's name, before any write.
    //
    // normNameServerRule is the server's own rule, and the live name is read
    // with kParamNameQueryLen, the same call and length buildFallbackLookupJson
    // used to compose param_names. Identical sourcing plus identical
    // normalization in the same session means this can only fire on a genuine
    // mismatch, never on a write the server already judged safe.
    //
    // The wanted name falls back to the entry's key exactly as the server's
    // `c.name || key` does, so an entry carrying no name is compared rather
    // than waved through.
    //
    // FAILS CLOSED. An unreadable live name on a tagged map is a mismatch, not
    // a pass: the whole point is that we cannot vouch for this index, and an
    // empty string is not evidence that we can.
    //
    // A disagreement is an honest miss in the same decline shape as a name that
    // never resolved, so the value stays on the card for hand dialling. Never a
    // silent skip and never a write.
    //
    // The VERIFIED path adds no such check, because an exact fingerprint
    // already guarantees the index.
    if (anchorsUnverified)
    {
        const auto mapped = mapEntry.getProperty ("name", juce::var()).toString();
        const auto want   = mapped.isNotEmpty() ? mapped : semantic;
        const auto live   = param->getName (kParamNameQueryLen);
        if (live.isEmpty()
            || normNameServerRule (live) != normNameServerRule (want))
        {
            r.note = "carried map names \"" + want + "\" at index " + juce::String (index)
                   + ", this version has \""
                   + (live.isEmpty() ? juce::String ("(unreadable)") : live)
                   + "\" -- not written, dial by hand";
            return r;
        }
    }

    const auto kind = mapEntry.getProperty ("kind", "").toString();

    // Every write is read back before success is claimed. The pre-write
    // value is captured so a write that LANDS WRONG can be reverted: a knob
    // left at +16 dB when -18 dB was asked is worse than an untouched knob.
    const float prevNorm = param->getValue();
    auto writeNorm = [param] (float n)
    {
        // Change gesture: the correct pattern for driving a hosted plugin's
        // parameter, so the plugin and any automation see a clean
        // begin/change/end rather than a bare value poke.
        param->beginChangeGesture();
        param->setValueNotifyingHost (n);
        param->endChangeGesture();
    };
    auto revert = [&writeNorm, prevNorm] () { writeNorm (prevNorm); };

    float norm = 0.0f;

    if (kind == "position")
    {
        // value is a 1-based position; convert to 0..1 across steps.
        float pos;
        if (! semanticToFloat (value, pos)) { r.note = "bad position value"; return r; }
        const int steps = juce::jmax (2, (int) mapEntry.getProperty ("steps", 2));
        const int p = juce::jlimit (1, steps, (int) std::round (pos));
        norm = (float) (p - 1) / (float) (steps - 1);
        writeNorm (norm);
        r.landedText = param->getCurrentValueAsText();
        // Positions carry no display expectation: norm round-trip proves
        // addressability only, and the result says so.
        if (std::abs (param->getValue() - norm) <= 0.5f / (float) (steps - 1))
        {
            r.applied = true;
            r.normalized = norm;
            r.note = "position set (display unverifiable)";
        }
        else if (staleDisplayReads)
        {
            // Same measured fact as the setread path: in-stack value reads
            // are pre-write on the bridge.
            r.applied = true;
            r.normalized = norm;
            r.staleDisplayKept = true;
            r.note = "position written; readback cannot be confirmed in-stack "
                     "on this bridged plugin";
        }
        else
        {
            revert();
            r.readbackMismatch = true;
            r.note = "position write did not stick, value restored";
        }
        return r;
    }

    if (kind == "mode")
    {
        const auto label = value.toString().trim();
        auto labels = mapEntry.getProperty ("labels", juce::var());
        auto* lo = labels.getDynamicObject();
        if (lo == nullptr || label.isEmpty()) { r.note = "no labels in mode entry"; return r; }
        const bool ciOk = (bool) mapEntry.getProperty ("caseInsensitiveOk", false);
        // Provenance: labels borrowed from the sibling binary named here, rather
        // than measured on this one. Written only by the class-1 label join; see
        // the branch below for why this, and not trust, is the gate.
        const bool labelsAreForeign =
            mapEntry.getProperty ("joined_from", juce::var()).toString().isNotEmpty();
        juce::String matched;
        float labelNorm = 0.0f;
        for (auto& kv : lo->getProperties())
        {
            const auto name = kv.name.toString();
            if (name == label || (ciOk && name.equalsIgnoreCase (label)))
            { matched = name; labelNorm = (float) (double) kv.value; break; }
        }
        if (matched.isEmpty()) { r.note = "unknown mode label \"" + label + "\""; return r; }
        writeNorm (labelNorm);
        r.landedText = param->getCurrentValueAsText().trim();
        const bool textOk = r.landedText == matched
                            || (ciOk && r.landedText.equalsIgnoreCase (matched));
        if (textOk)
        {
            r.applied = true;
            r.normalized = labelNorm;
            r.displayVerified = true;
            r.note = "applied, reads \"" + r.landedText + "\"";
        }
        else if (labelsAreForeign && ! staleDisplayReads)
        {
            // BORROWED LABELS (29 Aug 2026). These label strings were measured
            // on a DIFFERENT binary - the sibling named by joined_from - and
            // attached here because both formats step the same parameter at the
            // same normalised positions. That is the whole point of the join:
            // this binary reports its value as an ORDINAL ("6"), which is why it
            // had no labels of its own. So the text comparison above compares a
            // name against a number and can never succeed, and the revert below
            // would undo a write that landed on exactly the right option.
            // MEASURED on the 28 Aug Saturn 2 pilot, from this very path:
            //   asked "Warm Tape", plugin shows "6", value restored
            // and "Warm Tape" IS index 6. The write was correct; the instrument
            // was wrong.
            //
            // GATED ON PROVENANCE, NOT ON TRUST, and that is load-bearing.
            // trust:"setread" looks like the natural gate and is useless as one:
            // measured across 133 maps, ALL 5,772 genuine mode controls carry it,
            // because every label in the corpus was captured set-then-read. A
            // trust gate would relax verification for every mode control there
            // is. joined_from is written by the join and by nothing else.
            // (Corroboration, deliberately NOT a code condition: 0 of those 5,772
            // carry an anchors array, while every joined control does. An
            // incidental absence is not a guard - if a future builder emits
            // anchors on a native mode control the discriminator dies silently,
            // so the explicit field stays the only gate.)
            //
            // NOT a rubber stamp: the write is still verified, by the same norm
            // round-trip the anchored setread path below uses. Swapping the
            // instrument is the fix; skipping verification is not. A write that
            // does not stick still reverts.
            //
            // The ! staleDisplayReads guard keeps the bridged path below intact:
            // on a bridge getValue() is pre-write too, so the round-trip would
            // fail by construction and revert exactly the work the bridged rung
            // exists to keep. A bridged joined control falls through to it.
            if (std::abs (param->getValue() - labelNorm) <= 0.02f)
            {
                r.applied = true;
                r.normalized = labelNorm;
                r.note = "applied (display unverifiable on this plugin)";
            }
            else
            {
                revert();
                r.readbackMismatch = true;
                r.note = "asked \"" + matched + "\", write did not stick, value restored";
            }
        }
        else if (staleDisplayReads)
        {
            // BRIDGED INSTANCE, report-only (10 Aug 2026, option a of
            // DEFECT_BRIDGED_READBACK): every in-stack read is PRE-write on
            // the bridge. The filing assumed the norm cache updates
            // synchronously; MEASURED 10 Aug on API-2500 through this very
            // path, it does not - getValue() right after the write returned
            // the pre-write norm too. So nothing in this stack frame can
            // verify OR falsify the write; reverting on a read that is
            // stale by construction undoes correct work deterministically
            // (the filing, 3 of 3). Keep the write, say what is known, and
            // leave verification to the settle machinery downstream.
            r.applied = true;
            r.normalized = labelNorm;
            r.staleDisplayKept = true;
            r.note = "written \"" + matched + "\"; readback cannot be confirmed "
                     "in-stack on this bridged plugin";
        }
        else
        {
            revert();
            r.readbackMismatch = true;
            r.note = "asked \"" + matched + "\", plugin shows \"" + r.landedText + "\", value restored";
        }
        return r;
    }

    // anchored or linear, both use the anchor table
    float target;
    if (! semanticToFloat (value, target)) { r.note = "bad value"; return r; }
    auto anchors = anchorsFromVar (mapEntry);
    if (anchors.isEmpty()) { r.note = "no anchors in map"; return r; }
    // Non-monotonic tables (surviving v1 maps): interpolate inside the
    // dominant monotonic segment only; mirror/garbage shapes are refused.
    auto eff = dominantMonotonicTable (anchors);
    if (! eff.ok) { r.note = "anchor table unusable (non-monotonic), left manual"; return r; }
    // DEFENSIVE: a near-zero-span anchor table can only pin the knob at
    // one end whatever value is asked (the Solid Bus Comp bug class).
    // The map builder now rejects these at build time; this guard makes
    // sure even a future bad map cannot slam a parameter.
    float loV = eff.table.getFirst()[0], hiV = loV;
    for (auto& a : eff.table) { loV = juce::jmin (loV, a[0]); hiV = juce::jmax (hiV, a[0]); }
    if (hiV - loV < 1.0e-6f) { r.note = "degenerate map range, left manual"; return r; }

    // Range validation: see valueWithinMappedRange above. Refused values
    // write nothing and go manual, named individually on the card.
    if (! valueWithinMappedRange (target, loV, hiV))
    {
        r.outOfRange = true;
        r.note = "asked " + juce::String (target, 2) + ", this control's range is ["
               + juce::String (loV, 2) + " .. " + juce::String (hiV, 2)
               + "], left manual";
        return r;
    }

    // A STEPPED CONTROL'S REQUEST MUST BE ONE OF ITS STEPS (4 Sep 2026, live).
    // A Shadow Hills Discrete Attack steps {0.1|0.5|1|5|10|30 ms}; ask for 2
    // and interpolateAnchors returns a normalised BETWEEN two rungs, which the
    // plugin then quantises to whichever rung is nearer. What happened next
    // depended on an 0.02 tolerance in the setread branch below that has
    // nothing to do with the step spacing: land within it and the write is
    // reported as applied AT THE REQUESTED VALUE while the knob sits on a
    // different rung, land outside it and the write is reverted and blamed on
    // the plugin ("write did not stick"). Both are wrong, and neither tells the
    // user what the reachable values were.
    //
    // REFUSED RATHER THAN SNAPPED, and the rule is already written in this
    // file: a knob left where it was is better than a knob on a value nobody
    // asked for. Snapping 2 ms to 1 ms silently is the defect, not the fix, and
    // a refusal that prints the ladder is something the user can act on. It
    // also fails safe if the ladder detection is ever wrong about a continuous
    // control: the cost is one refused write with the anchor values printed,
    // against a wrong write that reports success.
    const auto ladderVerdict = checkStepLadder (eff.table, target);
    if (ladderVerdict.isLadder && ! ladderVerdict.onRung)
    {
        r.note = ladderVerdict.note;
        return r;
    }
    // Snap to the STORED rung so the interpolation lands exactly on its own
    // anchor: the request is a double (1.0) and the table holds a float32
    // round-trip (0.100000001490116), and the drift between them is enough to
    // put the normalised a hair off the rung.
    if (ladderVerdict.onRung) target = ladderVerdict.snapped;

    norm = juce::jlimit (0.0f, 1.0f, interpolateAnchors (eff.table, target));
    writeNorm (norm);
    r.normalized = norm;
    r.landedText = param->getCurrentValueAsText();

    // The ONE verification switch. Params entries carry method; controls
    // entries carry trust ("setread" = anchors captured set-then-read
    // because the display lies) and NO method field. Absent method with
    // setread trust IS setread: without this translation a setread control
    // would fall into the typed display comparison below and readback
    // would lie about controls the way it lied about q.
    auto method = mapEntry.getProperty ("method", juce::var()).toString();
    if (method.isEmpty())
        method = mapEntry.getProperty ("trust", "").toString() == "setread" ? "setread"
                                                                            : "gettext";
    if (method == "setread")
    {
        // Set-then-read maps exist BECAUSE these plugins' display text lies
        // (getText ignores its argument), so display comparison is blind
        // here. Norm round-trip proves addressability, not correctness, and
        // the caveat travels with the result so the card never presents this
        // as a verified write.
        if (std::abs (param->getValue() - norm) <= 0.02f)
        {
            r.applied = true;
            r.note = "applied (display unverifiable on this plugin)";
        }
        else if (staleDisplayReads)
        {
            // The filing called setread entries immune ("the norm cache
            // updates synchronously"); MEASURED 10 Aug on API-2500: the
            // in-stack getValue() is pre-write on the bridge too. Same
            // report-only rule as the display paths.
            r.applied = true;
            r.staleDisplayKept = true;
            // The captured text is a PRE-WRITE read on this path, by the
            // same measurement that keeps the write: carrying it forward
            // printed landed "4.00" for writes of 0.000 and 1.000 (12 Aug,
            // CLA-76) on a line already saying readback cannot be
            // confirmed. There is no post-write reader in-stack, so the
            // honest value is none.
            r.landedText.clear();
            r.note = "written; readback cannot be confirmed in-stack on this "
                     "bridged plugin";
        }
        else
        {
            revert();
            r.readbackMismatch = true;
            r.note = "write did not stick (norm round-trip failed), value restored";
        }
        return r;
    }

    // Typed display comparison against the CLAMPED request: a value beyond
    // the table's ends aims at the end, and landing there is the honest
    // best effort, not a mismatch.
    const float expected = juce::jlimit (loV, hiV, target);
    const auto unitOverride = mapEntry.getProperty ("unit", "").toString();
    const int verdict = typedReadbackMatch (semantic, expected, r.landedText, eff.table,
                                            unitOverride);
    if (verdict > 0)
    {
        r.applied = true;
        r.displayVerified = true;
        r.note = "applied, reads \"" + r.landedText.trim() + "\"";
    }
    else if (verdict == 0)
    {
        // Unparseable display: cannot verify either way. Applied with the
        // caveat carried, same presentation class as setread.
        r.applied = true;
        r.note = "applied (read-back unparseable: \"" + r.landedText.trim() + "\")";
    }
    else if (staleDisplayReads)
    {
        // Same bridged report-only rule as the mode path above: every
        // in-stack read (display AND norm, measured) is pre-write on the
        // bridge, so a revert here can only undo correct work.
        r.applied = true;
        r.staleDisplayKept = true;
        // Same rule as the setread branch: the text was read pre-write, so
        // it must not travel as a landing.
        r.landedText.clear();
        r.note = "written; readback cannot be confirmed in-stack on this "
                 "bridged plugin";
    }
    else
    {
        revert();
        r.applied = false;
        r.readbackMismatch = true;
        r.note = "asked " + juce::String (expected, 2) + " "
               + (unitOverride.isNotEmpty() ? unitOverride : semanticUnit (semantic))
               + ", plugin shows \"" + r.landedText.trim() + "\", value restored";
    }
    return r;
}

// ---------------------------------------------------------------------------
// Dial predicate (26 Jul 2026) — the ONE definition of "usable" shared by
// apply-time honesty and the feed-split branch. (It also backed the "(dial)"
// markers and dialFlags until those were deleted on 25 Aug; see the
// supersession note further down.) A param entry
// is usable iff applyOne above could actually write it: valid index, and
// either a stepped position control or an anchor table with real span (the
// same 1e-6 degenerate-span threshold applyOne refuses at).
// ---------------------------------------------------------------------------
inline bool usableParamEntry (const juce::var& entry)
{
    if (! entry.isObject()) return false;
    if ((int) entry.getProperty ("index", -1) < 0) return false;
    const auto kind = entry.getProperty ("kind", "").toString();
    if (kind == "position")
        return (int) entry.getProperty ("steps", 0) >= 2;
    if (kind == "mode")
    {
        auto* lo = entry.getProperty ("labels", juce::var()).getDynamicObject();
        return lo != nullptr && lo->getProperties().size() >= 2;
    }
    auto anchors = anchorsFromVar (entry);
    if (anchors.size() < 2) return false;
    // Same segment rule applyOne enforces: no dominant monotonic run means
    // applyOne would refuse it, so it must not count as usable either.
    auto eff = dominantMonotonicTable (anchors);
    if (! eff.ok) return false;
    float lo = eff.table.getFirst()[0], hi = lo;
    for (auto& a : eff.table) { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
    return (hi - lo) > 1.0e-6f;
}

inline int usableParamCount (const juce::var& map)
{
    int n = 0;
    if (auto* params = map.getProperty ("params", juce::var()).getDynamicObject())
        for (auto& kv : params->getProperties())
            if (usableParamEntry (kv.value)) ++n;
    return n;
}

// CORE semantics (mirrors the server map-builder's CORE set): the controls
// that make a plugin meaningfully one-click dialable. spiff's mix_pct +
// position map passes a raw >=2 count yet cannot de-ess; the marker
// threshold therefore counts CORE semantics only.
inline int usableCoreCount (const juce::var& map)
{
    static const char* kCore[] = { "ratio", "threshold_db", "attack_ms",
                                   "release_ms", "makeup_db", "gain_db" };
    int n = 0;
    auto params = map.getProperty ("params", juce::var());
    for (auto* k : kCore)
        if (usableParamEntry (params.getProperty (k, juce::var()))) ++n;
    return n;
}

// Dialability is the SERVER's answer: resolveFpV2 stamps map.dialable from
// mapClearsCategoryBar (usable semantics across flat params AND groups, minus
// suppressed, judged against the plugin's category bar). Read it, do not
// reimplement it - the old flat, compressor-shaped usableCoreCount could not
// see a suppressed key or a grouped band, so bx_digital read NOT dialable.
// Absent flag = unknown = NOT dialable: this gates a strictness feature, so the
// safe default is to withhold rather than guess, and TTL revalidation supplies
// the flag within one window. usableCoreCount is left defined but no longer
// consulted here - it was wrong in both directions.
inline bool mapIsDialableForSignals (const juce::var& map)
{
    return (bool) map.getProperty ("dialable", false);
}

// kDialSignalsEnabled IS DELETED (25 Aug 2026), and it is SUPERSEDED, not
// pending. It gated the feed's "(dial)" name markers and the request's
// dialFlags array, and it stayed false from 26 Jul waiting on a server-side
// explainer for the marker that was never written.
//
// The category tag shipped on 25 Aug does that job at a granularity that
// actually discriminates: 467 of 859 feed names carry one, against the dial
// signal's 1,183 of 1,185 products. A marker that is true of almost everything
// tells the model nothing, which is why no explainer for it was ever worth
// writing.
//
// Recorded here rather than removed silently, because a dark constant with a
// "MUST stay false until..." comment reads as work someone is coming back to,
// and nobody was. mapIsDialableForSignals above SURVIVES: it still backs
// ChainHost::getDialableRecommendableNames, which the P16 feed-split branch
// and the settle walker both use.

// Short human label for a semantic key, for "needs hand-dialing (threshold,
// attack)" copy. Mirrors formatSemanticSetting's suffix handling.
inline juce::String semanticLabel (const juce::String& key)
{
    if (key == "ratio") return "ratio";
    for (auto* suf : { "_db", "_ms", "_hz", "_pct", "_s" })
        if (key.endsWith (suf))
            return key.dropLastCharacters ((int) juce::String (suf).length())
                      .replaceCharacter ('_', ' ');
    return key.replaceCharacter ('_', ' ');
}

// ---------------------------------------------------------------------------
// Band matcher (28 Jul 2026). A multiband EQ's per-band freq/gain controls
// collapse to one flat freq_hz / gain_db key and get SUPPRESSED at build time
// (>=2 candidates, no arbitrary winner), so the flat path declines them. The
// matcher instead consumes map.groups: for a per-band request it picks a band
// in the primary family whose freq control can REACH the target, sets that
// band's freq and gain, and consumes the band so a second request in the same
// block cannot land on it. Range is a tiebreaker allowed to fall through to
// first-free by band number.
//
// EQ-band guard, carried from the corpus measurement into the runtime, not
// left in the harness: a group whose "freq" is really a modulation rate
// (Doubler "Voice1 Rate"), a delay tap (SuperTap "Tap 1 Freq") or a crossover
// split is NOT an EQ band and MUST be declined. Placing a frequency there is
// exactly the wrong-write class this track exists to remove.
// ---------------------------------------------------------------------------

// True when a group is a real EQ band: freq_hz + gain_db both usable, the freq
// param is a band centre (not a rate/tap/crossover/feedback), and the family
// is not a modulation/delay family. Mirrors the corpus guard (NOTFAM/NOTBAND).
inline bool groupIsEqBand (const juce::String& family, const juce::var& group)
{
    const auto fam = family.toLowerCase();
    for (auto* bad : { "voice", "tap", "damp", "crossover", "chorus", "phaser", "delay", "osc" })
        if (fam.contains (bad)) return false;

    auto params = group.getProperty ("params", juce::var());
    auto freq = params.getProperty ("freq_hz", juce::var());
    auto gain = params.getProperty ("gain_db", juce::var());
    if (! freq.isObject() || ! gain.isObject())      return false; // both needed for an EQ move
    if (! usableParamEntry (freq) || ! usableParamEntry (gain)) return false;

    const auto fn = freq.getProperty ("name", "").toString().toLowerCase();
    for (auto* bad : { "rate", "voice", "crossover", "feedback", "detune", "lfo", "min freq", "max freq", "pitch" })
        if (fn.contains (bad)) return false;
    return true;
}

// Read a param's current value in the semantic's unit, for the untouched check.
inline bool readCurrentValue (juce::AudioPluginInstance& plugin, const juce::var& entry,
                              const juce::String& semantic, float& out)
{
    auto& params = plugin.getParameters();
    const int index = (int) entry.getProperty ("index", -1);
    if (index < 0 || index >= params.size() || params[index] == nullptr) return false;
    bool negInf = false;
    if (! parseDisplayForUnit (params[index]->getCurrentValueAsText(), semanticUnit (semantic), out, negInf))
        return false;
    if (negInf) out = -120.0f; // "-oo" at the dB floor reads as fully attenuated, not unity
    return true;
}

// The band's reachable frequency window. freq_range when the map carries it;
// otherwise DERIVED from the band's own freq_hz anchor values - the anchors
// ARE the authoritative reachability, freq_range is only their summary (the
// AMEK 8 kHz incident: ingestGroups dropped freq_range from every stored
// map, "no range" read as "assume reachable", and a 15-780 Hz band took an
// 8 kHz request as a verified-looking clamp to 780). known=false only when
// the band has no freq entry at all: unknown, which is not unreachable.
struct BandReach { float lo = 0.0f, hi = 0.0f; bool known = false; };
inline BandReach bandReachRange (const juce::var& group)
{
    BandReach r;
    auto fr = group.getProperty ("freq_range", juce::var());
    if (auto* a = fr.getArray())
        if (a->size() >= 2)
        {
            r.lo = (float) (double) (*a)[0];
            r.hi = (float) (double) (*a)[1];
            if (r.hi > r.lo) { r.known = true; return r; }
        }
    auto anchors = anchorsFromVar (group.getProperty ("params", juce::var())
                                        .getProperty ("freq_hz", juce::var()));
    if (anchors.size() >= 2)
    {
        r.lo = r.hi = anchors[0][0];
        for (auto& p : anchors)
        {
            r.lo = juce::jmin (r.lo, p[0]);
            r.hi = juce::jmax (r.hi, p[0]);
        }
        r.known = r.hi > r.lo;
    }
    return r;
}

// Assign a bands[] request to group bands and write each. One ApplyResult per
// requested per-band control. Always reports (declines when no band is free),
// so the caller never has to synthesise feedback.
inline void applyBands (juce::AudioPluginInstance& plugin, const juce::var& map,
                        const juce::var& bandsVar, juce::Array<ApplyResult>& results,
                        bool staleDisplayReads = false,
                        bool anchorsUnverified = false)
{
    auto* bands = bandsVar.getArray();
    if (bands == nullptr) return;

    struct Cand { juce::String family; int n; juce::var group; };
    struct ByN { const juce::Array<Cand>& c; int compareElements (int a, int b) const noexcept { return c[a].n - c[b].n; } };
    constexpr int kUntouchedScan = 8; // cap on live gain reads per requested band
    juce::Array<Cand> cands;
    juce::String primaryFam;
    auto groupsVar = map.getProperty ("groups", juce::var());
    if (auto* groups = groupsVar.getArray())
        for (auto& gv : *groups)
        {
            const auto fam = gv.getProperty ("family", "").toString();
            if ((bool) gv.getProperty ("primary", false)) primaryFam = fam;
            if (groupIsEqBand (fam, gv))
                cands.add (Cand { fam, (int) gv.getProperty ("n", 0), gv });
        }

    auto declineBand = [&results] (const juce::var& bv, const juce::String& why)
    {
        if (auto* o = bv.getDynamicObject())
            for (auto& kv : o->getProperties())
            {
                const juce::String bk = kv.name.toString();
                if (bk == "family") continue;
                ApplyResult r; r.semantic = bk; r.note = why;
                results.add (r);
            }
    };

    if (cands.isEmpty()) // no EQ band group on this plugin (e.g. ungrouped map)
    {
        for (auto& bv : *bands) declineBand (bv, "no EQ band group for this control on this plugin");
        return;
    }

    juce::Array<int> consumed;
    for (auto& bv : *bands)
    {
        const juce::String wantFam = bv.getProperty ("family", "").toString();

        // Family preference: requested family, then the primary family, then any
        // unconsumed band. A mislabelled family must not block the match.
        juce::StringArray famOrder;
        if (wantFam.isNotEmpty())    famOrder.add (wantFam);
        if (primaryFam.isNotEmpty() && ! famOrder.contains (primaryFam, true)) famOrder.add (primaryFam);
        juce::Array<int> pool;
        for (auto& f : famOrder)
        {
            for (int i = 0; i < cands.size(); ++i)
                if (! consumed.contains (i) && cands[i].family.equalsIgnoreCase (f)) pool.add (i);
            if (! pool.isEmpty()) break;
        }
        if (pool.isEmpty())
            for (int i = 0; i < cands.size(); ++i)
                if (! consumed.contains (i)) pool.add (i);
        if (pool.isEmpty()) { declineBand (bv, "no free band left in the family for this request"); continue; }

        // Requested frequency, for the reach test.
        float wantFreq = 0.0f; bool haveFreq = false;
        auto fv = bv.getProperty ("freq_hz", juce::var());
        if (! fv.isVoid()) { float t; if (semanticToFloat (fv, t)) { wantFreq = t; haveFreq = true; } }

        auto reaches = [&] (int ci) -> bool
        {
            if (! haveFreq) return true;
            auto r = bandReachRange (cands[ci].group);
            if (! r.known) return true;   // unknown is not unreachable
            return wantFreq >= r.lo && wantFreq <= r.hi;
        };
        juce::Array<int> reachable;
        for (int i : pool) if (reaches (i)) reachable.add (i);
        if (reachable.isEmpty())
        {
            // KNOWING-CLAMP GUARD (1 Aug 2026): every free band verifiably
            // cannot reach the target, so DECLINE - the old first-free
            // fallback produced a plausible-looking wrong setting (8 kHz
            // "landed" at a low band's 780 Hz ceiling) that the clamped
            // best-effort readback then VERIFIED. An honest decline beats a
            // verified lie.
            declineBand (bv, "no free band on this EQ reaches "
                             + juce::String (wantFreq, 0) + " Hz, left manual");
            continue;
        }
        juce::Array<int>& choosePool = reachable;

        // Low band number first, so both the default pick and the untouched
        // scan favour the lowest band.
        ByN byN { cands };
        choosePool.sort (byN);

        // Prefer a band sitting at unity gain so the AI does not overwrite one
        // the user already dialled. This read uses getCurrentValueAsText, the
        // SAME call applyOne's read-back already makes on this loaded instance,
        // so it adds no new stall class; it is still capped at kUntouchedScan
        // reads so a 24-band bank cannot turn one apply into 24 text calls.
        int best = choosePool.getFirst();               // lowest-n fallback
        const int scan = juce::jmin (choosePool.size(), kUntouchedScan);
        for (int idx = 0; idx < scan; ++idx)
        {
            const int i = choosePool[idx];
            float g;
            auto ge = cands[i].group.getProperty ("params", juce::var()).getProperty ("gain_db", juce::var());
            if (readCurrentValue (plugin, ge, "gain_db", g) && std::abs (g) < 0.5f) { best = i; break; }
        }
        consumed.add (best);

        const juce::String tag = "band " + cands[best].family + juce::String (cands[best].n) + ": ";
        auto bandParams = cands[best].group.getProperty ("params", juce::var());
        if (auto* o = bv.getDynamicObject())
            for (auto& kv : o->getProperties())
            {
                const juce::String bk = kv.name.toString();
                if (bk == "family") continue;
                auto e = bandParams.getProperty (bk, juce::var());
                if (! e.isObject())
                {
                    ApplyResult r; r.semantic = bk; r.note = tag + "band has no " + bk + " control";
                    results.add (r); continue;
                }
                auto res = applyOne (plugin, bk, e, kv.value, staleDisplayReads, anchorsUnverified);
                res.note = tag + res.note;
                results.add (res);
            }
    }
}

// ---------------------------------------------------------------------------
// Apply all semantic settings for one plugin slot.
//   map      : the plugin's map object (from maps/<fp>.json), the whole thing
//   settings : EchoJay's structured settings, e.g. { "ratio":"4:1", "attack_ms":30 }
// Control-name normalisation, applied on BOTH the name the model sends and the
// map/live name it is matched against, so the two agree when they differ only
// in whitespace. Trim, then collapse every internal run of whitespace (space,
// tab, newline) to a single space. "Global\nStrength" (a real control name on a
// shipping plugin) and a double-spaced "Input  Gain" both round-trip. It must
// run on both ends or not at all: normalising one side only would turn a match
// into a miss. The plugin sends its live paramNames through this same function.
inline juce::String normalizeControlName (const juce::String& raw)
{
    juce::String out;
    auto cp = raw.trim().getCharPointer();
    bool lastWasSpace = false;
    for (juce::juce_wchar c; (c = cp.getAndAdvance()) != 0;)
    {
        const bool ws = (c == ' ' || c == '\t' || c == '\n'
                      || c == '\r' || c == '\f' || c == 0x0b);
        if (ws) { if (! lastWasSpace) { out << ' '; lastWasSpace = true; } }
        else    { out << c; lastWasSpace = false; }
    }
    return out;
}

// Returns one ApplyResult per requested setting. Band-class semantics that the
// flat map cannot serve (a suppressed freq_hz/gain_db on a multiband plugin)
// are diverted to the band matcher instead of a flat decline; an explicit
// settings.bands array always goes to the matcher.
// ---------------------------------------------------------------------------
inline juce::Array<ApplyResult> applySettings (juce::AudioPluginInstance& plugin,
                                               const juce::var& map,
                                               const juce::var& settings,
                                               bool staleDisplayReads = false)
{
    juce::Array<ApplyResult> results;
    // READ ONCE, HERE, because this is the only function that sees the whole
    // map. A product fallback carries anchors_unverified (and served_from
    // naming the identity it came from); every result of this apply inherits
    // it, so no downstream layer has to look the map up again to find out.
    const bool anchorsUnverified = (bool) map.getProperty ("anchors_unverified", false);
    auto mapParams = map.getProperty ("params", juce::var());
    auto* settingsObj = settings.getDynamicObject();
    if (settingsObj == nullptr) return results;

    // A grouped multiband EQ: any freq/gain/q in the request is a BAND move and
    // the whole pair must go to the band matcher as a unit. Otherwise routing
    // each key by its own flat-usability splits one move across two controls -
    // bx_digital's suppressed gain lands on a band while its flat freq_hz (the
    // stray "Dynamic EQ Frequency") steals the frequency half. A stray flat
    // control must never take half a band move.
    bool mapHasEqBands = false;
    if (auto* groups = map.getProperty ("groups", juce::var()).getArray())
        for (auto& gv : *groups)
            if (groupIsEqBand (gv.getProperty ("family", "").toString(), gv))
            { mapHasEqBands = true; break; }

    juce::DynamicObject::Ptr synthBand;   // band-class keys routed to the matcher
    for (auto& kv : settingsObj->getProperties())
    {
        const juce::String semantic = kv.name.toString();
        if (semantic == "bands")    continue; // handled after the flat pass
        if (semantic == "controls") continue; // handled after the flat pass

        auto mapEntry = mapParams.getProperty (semantic, juce::var());
        const bool flatUsable = mapEntry.isObject() && usableParamEntry (mapEntry);
        const bool bandClass  = (semantic == "freq_hz" || semantic == "gain_db" || semantic == "q");
        if (bandClass && (mapHasEqBands || ! flatUsable))
        {
            // EQ-band map, or a suppressed/absent flat entry: divert to the
            // matcher as one coherent band request. On a grouped EQ this fires
            // even when a flat entry exists, so freq and gain stay together.
            if (synthBand == nullptr) synthBand = new juce::DynamicObject();
            synthBand->setProperty (semantic, kv.value);
            continue;
        }
        if (! mapEntry.isObject())
        {
            ApplyResult r; r.semantic = semantic;
            r.note = "no mapping for this control on this plugin";
            results.add (r);
            continue;
        }
        results.add (applyOne (plugin, semantic, mapEntry, kv.value, staleDisplayReads, anchorsUnverified));
    }

    auto bandsVar = settings.getProperty ("bands", juce::var());
    if (bandsVar.isArray())
    {
        // Flat band-class keys riding the same turn as an explicit bands[]
        // used to be dropped here silently (the else-if below never ran).
        // The synth band is one more band request, appended AFTER the
        // explicit bands so it can never consume a band the model addressed
        // deliberately; applyBands reports every band, so if no band is
        // left free the keys decline with a note instead of vanishing.
        juce::Array<juce::var> all;
        if (auto* bs = bandsVar.getArray())
            for (auto& b : *bs) all.add (b);
        if (synthBand != nullptr) all.add (juce::var (synthBand.get()));
        applyBands (plugin, map, juce::var (all), results, staleDisplayReads, anchorsUnverified);
    }
    else if (synthBand != nullptr)
    {
        juce::Array<juce::var> one; one.add (juce::var (synthBand.get()));
        applyBands (plugin, map, juce::var (one), results, staleDisplayReads, anchorsUnverified);
    }

    // Named Tier 2 controls (1 Aug 2026, first reader): settings.controls is
    // { "<exact name>": value } looked up in map.controls, the exact-name
    // registry section stored and served since map-format v2. The server
    // enforces exact case before the block reaches the plugin, so the
    // case-insensitive rescue below only forgives drift, it is not the
    // contract. Every miss is an honesty entry, never a silent skip. The
    // entries are already in applyOne's shape (index/kind/anchors/labels);
    // trust->method and the unit field are translated inside applyOne.
    auto controlsReq = settings.getProperty ("controls", juce::var());
    if (auto* co = controlsReq.getDynamicObject())
    {
        auto mapControls = map.getProperty ("controls", juce::var());
        for (auto& kv : co->getProperties())
        {
            const juce::String name = kv.name.toString();
            auto entry = mapControls.getProperty (kv.name, juce::var());
            if (! entry.isObject())
                if (auto* mo = mapControls.getDynamicObject())
                {
                    // Whitespace- and case-insensitive rescue: a name that
                    // differs only in run-length whitespace (or case) still
                    // matches. Both sides go through normalizeControlName.
                    const auto want = normalizeControlName (name);
                    for (auto& mk : mo->getProperties())
                        if (normalizeControlName (mk.name.toString()).equalsIgnoreCase (want))
                        { entry = mk.value; break; }
                }
            if (! entry.isObject())
            {
                ApplyResult r; r.semantic = name;
                r.note = "no mapped control of this name on this plugin";
                results.add (r);
                continue;
            }
            if (! usableParamEntry (entry))
            {
                ApplyResult r; r.semantic = name;
                r.note = "control entry unusable (bad index or anchors), left manual";
                results.add (r);
                continue;
            }
            results.add (applyOne (plugin, name, entry, kv.value, staleDisplayReads, anchorsUnverified));
        }
    }
    else if (! controlsReq.isVoid())
    {
        ApplyResult r; r.semantic = "controls";
        r.note = "controls must be an object of name -> value";
        results.add (r);
    }
    return results;
}

} // namespace echojay
