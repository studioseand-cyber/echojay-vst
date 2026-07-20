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
};

// ---------------------------------------------------------------------------
// Interpolate a target value against an anchor table [[value, normalized], ...].
// The table's values are assumed monotonic (the map-builder truncated any
// non-monotonic tail). Clamps to the ends. Returns normalized 0..1.
// ---------------------------------------------------------------------------
inline float interpolateAnchors (const juce::Array<juce::Array<float>>& anchors,
                                 float target)
{
    if (anchors.isEmpty()) return 0.0f;
    if (anchors.size() == 1) return anchors[0][1];

    const float loV = anchors.getFirst()[0];
    const float hiV = anchors.getLast()[0];

    // Clamp below/above the table.
    if (target <= loV) return anchors.getFirst()[1];
    if (target >= hiV) return anchors.getLast()[1];

    for (int i = 0; i < anchors.size() - 1; ++i)
    {
        const float v0 = anchors[i][0],     v1 = anchors[i + 1][0];
        const float n0 = anchors[i][1],     n1 = anchors[i + 1][1];
        if (target >= v0 && target <= v1)
        {
            if (v1 == v0) return n0;
            const float frac = (target - v0) / (v1 - v0);
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

    float norm = 0.0f;

    if (kind == "position")
    {
        // value is a 1-based position; convert to 0..1 across steps.
        float pos;
        if (! semanticToFloat (value, pos)) { r.note = "bad position value"; return r; }
        const int steps = juce::jmax (2, (int) mapEntry.getProperty ("steps", 2));
        const int p = juce::jlimit (1, steps, (int) std::round (pos));
        norm = (float) (p - 1) / (float) (steps - 1);
    }
    else // anchored or linear, both use the anchor table
    {
        float target;
        if (! semanticToFloat (value, target)) { r.note = "bad value"; return r; }
        auto anchors = anchorsFromVar (mapEntry);
        if (anchors.isEmpty()) { r.note = "no anchors in map"; return r; }
        norm = interpolateAnchors (anchors, target);
    }

    norm = juce::jlimit (0.0f, 1.0f, norm);
    // Wrap in a change gesture: the correct pattern for driving a hosted
    // plugin's parameter, so the plugin and any automation see a clean
    // begin/change/end rather than a bare value poke.
    param->beginChangeGesture();
    param->setValueNotifyingHost (norm);
    param->endChangeGesture();

    r.applied = true;
    r.normalized = norm;
    r.note = "applied";
    return r;
}

// ---------------------------------------------------------------------------
// Apply all semantic settings for one plugin slot.
//   map      : the plugin's map object (from maps/<fp>.json), the whole thing
//   settings : EchoJay's structured settings, e.g. { "ratio":"4:1", "attack_ms":30 }
// Returns one ApplyResult per requested setting.
// ---------------------------------------------------------------------------
inline juce::Array<ApplyResult> applySettings (juce::AudioPluginInstance& plugin,
                                               const juce::var& map,
                                               const juce::var& settings)
{
    juce::Array<ApplyResult> results;
    auto mapParams = map.getProperty ("params", juce::var());
    auto* settingsObj = settings.getDynamicObject();
    if (settingsObj == nullptr) return results;

    for (auto& kv : settingsObj->getProperties())
    {
        const juce::String semantic = kv.name.toString();
        auto mapEntry = mapParams.getProperty (semantic, juce::var());
        if (! mapEntry.isObject())
        {
            ApplyResult r; r.semantic = semantic;
            r.note = "no mapping for this control on this plugin";
            results.add (r);
            continue;
        }
        results.add (applyOne (plugin, semantic, mapEntry, kv.value));
    }
    return results;
}

} // namespace echojay
