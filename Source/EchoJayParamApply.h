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
    juce::String semantic;   // "ratio", "threshold_db", ...
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
// Returns +1 match, 0 unparseable (cannot verify), -1 mismatch.
inline int typedReadbackMatch (const juce::String& semantic, float target,
                               const juce::String& landedText,
                               const juce::Array<juce::Array<float>>& anchors)
{
    const auto unit = semanticUnit (semantic);
    float landed = 0.0f;
    bool negInf = false;
    if (! parseDisplayForUnit (landedText, unit, landed, negInf)) return 0;

    juce::Array<float> vals;
    for (auto& a : anchors) vals.add (a[0]);
    vals.sort();
    const float lo = vals.getFirst(), hi = vals.getLast();
    float gap = 0.0f;
    for (int i = 0; i < vals.size() - 1; ++i)
        if (target >= vals[i] && target <= vals[i + 1]) { gap = vals[i + 1] - vals[i]; break; }
    if (gap <= 0.0f && vals.size() >= 2)
        gap = target < lo ? vals[1] - vals[0] : vals[vals.size() - 1] - vals[vals.size() - 2];
    const float tol = juce::jmax (0.02f * (hi - lo), 0.5f * gap);

    if (negInf) return target <= lo + tol ? +1 : -1;
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
inline ApplyResult applyOne (juce::AudioPluginInstance& plugin,
                             const juce::String& semantic,
                             const juce::var& mapEntry,
                             const juce::var& value)
{
    ApplyResult r; r.semantic = semantic;
    const int index = (int) mapEntry.getProperty ("index", -1);
    r.index = index;

    auto& params = plugin.getParameters();
    if (index < 0 || index >= params.size() || params[index] == nullptr)
    {
        r.note = "param index not present on this instance";
        return r;
    }
    auto* param = params[index];
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

    norm = juce::jlimit (0.0f, 1.0f, interpolateAnchors (eff.table, target));
    writeNorm (norm);
    r.normalized = norm;
    r.landedText = param->getCurrentValueAsText();

    const auto method = mapEntry.getProperty ("method", "gettext").toString();
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
    const int verdict = typedReadbackMatch (semantic, expected, r.landedText, eff.table);
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
    else
    {
        revert();
        r.applied = false;
        r.readbackMismatch = true;
        r.note = "asked " + juce::String (expected, 2) + " " + semanticUnit (semantic)
               + ", plugin shows \"" + r.landedText.trim() + "\", value restored";
    }
    return r;
}

// ---------------------------------------------------------------------------
// Dial predicate (26 Jul 2026) — the ONE definition of "usable" shared by
// apply-time honesty, the (dial) feed markers, and dialFlags. A param entry
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

// Marker/dialFlags threshold: >=2 usable CORE semantics (68 of 297 local
// maps at last count). Apply-time honesty deliberately does NOT use this:
// there the question is only "were the REQUESTED semantics written".
// A role-aware version (match the slot's role to the semantics that role
// needs) is feasible later once roles are normalized; not built now.
inline bool mapIsDialableForSignals (const juce::var& map)
{
    return usableCoreCount (map) >= 2;
}

// Dial signals master switch (feed "(dial)" markers + request dialFlags).
// MUST stay false until the server-side explainer for the marker exists —
// shipping markers the model has never had explained invites it to invent
// meaning for them.
constexpr bool kDialSignalsEnabled = false;

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

// Assign a bands[] request to group bands and write each. One ApplyResult per
// requested per-band control. Always reports (declines when no band is free),
// so the caller never has to synthesise feedback.
inline void applyBands (juce::AudioPluginInstance& plugin, const juce::var& map,
                        const juce::var& bandsVar, juce::Array<ApplyResult>& results)
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
            auto fr = cands[ci].group.getProperty ("freq_range", juce::var());
            auto* a = fr.getArray();
            if (a == nullptr || a->size() < 2) return true; // no range -> assume reachable
            const float lo = (float) (double) (*a)[0], hi = (float) (double) (*a)[1];
            return wantFreq >= lo && wantFreq <= hi;
        };
        juce::Array<int> reachable;
        for (int i : pool) if (reaches (i)) reachable.add (i);
        const bool fellThrough = reachable.isEmpty();
        juce::Array<int>& choosePool = fellThrough ? pool : reachable;

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

        const juce::String tag = "band " + cands[best].family + juce::String (cands[best].n)
                               + (fellThrough ? " (first-free, none reached target)" : "") + ": ";
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
                auto res = applyOne (plugin, bk, e, kv.value);
                res.note = tag + res.note;
                results.add (res);
            }
    }
}

// ---------------------------------------------------------------------------
// Apply all semantic settings for one plugin slot.
//   map      : the plugin's map object (from maps/<fp>.json), the whole thing
//   settings : EchoJay's structured settings, e.g. { "ratio":"4:1", "attack_ms":30 }
// Returns one ApplyResult per requested setting. Band-class semantics that the
// flat map cannot serve (a suppressed freq_hz/gain_db on a multiband plugin)
// are diverted to the band matcher instead of a flat decline; an explicit
// settings.bands array always goes to the matcher.
// ---------------------------------------------------------------------------
inline juce::Array<ApplyResult> applySettings (juce::AudioPluginInstance& plugin,
                                               const juce::var& map,
                                               const juce::var& settings)
{
    juce::Array<ApplyResult> results;
    auto mapParams = map.getProperty ("params", juce::var());
    auto* settingsObj = settings.getDynamicObject();
    if (settingsObj == nullptr) return results;

    juce::DynamicObject::Ptr synthBand;   // flat freq/gain the flat map cannot serve
    for (auto& kv : settingsObj->getProperties())
    {
        const juce::String semantic = kv.name.toString();
        if (semantic == "bands") continue; // handled after the flat pass

        auto mapEntry = mapParams.getProperty (semantic, juce::var());
        const bool flatUsable = mapEntry.isObject() && usableParamEntry (mapEntry);
        const bool bandClass  = (semantic == "freq_hz" || semantic == "gain_db" || semantic == "q");
        if (bandClass && ! flatUsable)
        {
            // Suppressed or absent on the flat map: divert to the matcher as a
            // one-band request rather than declining outright.
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
        results.add (applyOne (plugin, semantic, mapEntry, kv.value));
    }

    auto bandsVar = settings.getProperty ("bands", juce::var());
    if (bandsVar.isArray())
        applyBands (plugin, map, bandsVar, results);
    else if (synthBand != nullptr)
    {
        juce::Array<juce::var> one; one.add (juce::var (synthBand.get()));
        applyBands (plugin, map, juce::var (one), results);
    }
    return results;
}

} // namespace echojay
