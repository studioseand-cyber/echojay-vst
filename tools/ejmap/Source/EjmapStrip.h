/*
  EjmapStrip.h - CHANNEL STRIPS (Kathy's NEXT BUILD item E, 7 Oct 2026; docs/CHANNEL_STRIP_RULE_DECISION.md read first).

  A strip is several processors behind one parameter list. Each control is put in a SECTION by its name (the section word,
  whole-token: comp / compressor / dyn / dynamics -> compressor; gate / expander / exp / expand -> gate; eq / band / lf / lmf /
  mf / hmf / hf / low / mid / high / hpf / lpf / filter / shelf / bell / q / freq -> eq; sat / saturation / drive / harmonics /
  color / colour / tape / tube / preamp / pre -> saturation; the rest global). A section's ENGAGE switch is its two-step
  control whose name carries on / in / enable / active / engage / power / bypass (Rule 1: the section's own switch rides with
  it; the ON norm by its text - on / in / active -> that position, bypass -> the other; no text: on = 1, bypass = 0). Every
  other section is left at its instantiate value (Rule 1 as ruled 2 Oct), so a section's measurement is the strip with that
  section engaged, never the strip with the others bypassed - said on the record.

  Each section then runs through its category's mode as a child process with `--only-controls` (the mode's lexicon sees the
  section's controls alone) and the engage write as the probe's preset (EJ_PROBE_PRESET). The compressor section: the
  threshold plan over its controls (Rule 1's pick with the section's engage) is recorded; the full certification sweep and
  the tone check need the worklist to admit a strip section - NOT built here (said on the record as `compressor_sweep`).

  PURE, pinned in RoundTripTest.cpp (ST1-ST4).
*/

#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <optional>
#include <vector>

namespace ejmap::strip
{

inline juce::StringArray tokens (const juce::String& name) { auto t = juce::StringArray::fromTokens (name.toLowerCase(), " -_/()[]:>", "\"'"); t.removeEmptyStrings(); return t; }

inline juce::String sectionOf (const juce::String& name)
{
    const auto tk = tokens (name);
    auto has = [&] (std::initializer_list<const char*> ws) { for (const auto& t : tk) for (const char* w : ws) if (t == w) return true; return false; };
    if (has ({ "gate", "expander", "exp", "expand", "gating", "ge" })) return "gate";          // "GE" = bx_console SSL's gate / expander
    if (has ({ "comp", "compressor", "compression", "dyn", "dynamics", "limiter", "lim", "lc" })) return "compressor";   // "LC" = its limiter / compressor
    if (has ({ "eq", "band", "lf", "lmf", "mf", "hmf", "hf", "low", "mid", "high", "hpf", "lpf", "filter", "filters", "shelf", "bell", "q", "freq", "frequency", "hz" })) return "eq";
    if (has ({ "sat", "saturation", "saturate", "drive", "harmonics", "color", "colour", "tape", "tube", "preamp", "pre", "thd" })) return "saturation";
    // an unprefixed compressor control (bx_console SSL 4000 E: "Threshold", "Ratio", "Attack", "Release" beside "Dyn On/Off"): the role word alone
    if (has ({ "threshold", "thresh", "ratio", "knee", "makeup", "make-up", "attack", "release" })) return "compressor";
    return "global";
}

struct Control { int index = -1; juce::String name; int numSteps = 0; std::map<juce::String, float> texts; };   // texts: display -> norm (two-step switches)
struct Section { juce::String name; std::vector<Control> controls; std::optional<Control> engage; float engageNorm = 1.0f; juce::String engageText, note; int numeric = 0; using engage_type = Control; };

inline bool switchWord (const juce::String& t) { for (const char* w : { "on", "in", "enable", "enabled", "active", "engage", "power", "bypass", "mute", "off", "out" }) if (t == w) return true; return false; }

inline std::vector<Section> sectionsOf (const std::vector<Control>& controls)
{
    std::map<juce::String, Section> by;
    for (const auto& c : controls) { auto& s = by[sectionOf (c.name)]; s.name = sectionOf (c.name); s.controls.push_back (c); if (c.numSteps != 2) ++s.numeric; }
    std::vector<Section> out;
    for (const char* n : { "compressor", "eq", "gate", "saturation", "global" }) if (by.count (n)) out.push_back (by[n]);
    for (auto& s : out)
    {
        if (s.name == "global") continue;
        // the engage switch: a two-step control of the section with a switch word in its name; "bypass" inverts the ON reading
        for (const auto& c : s.controls)
        {
            if (c.numSteps != 2) continue;
            const auto tk = tokens (c.name); bool sw = false, bypass = false; for (const auto& t : tk) { if (switchWord (t)) sw = true; if (t == "bypass" || t == "mute") bypass = true; }
            if (! sw) continue;
            Section::engage_type e = c; float onNorm = bypass ? 0.0f : 1.0f; juce::String onText; bool found = false;
            for (const auto& [text, norm] : c.texts)
            {
                const auto tl = text.toLowerCase().trim();
                const bool onLike = tl == "on" || tl == "in" || tl == "active" || tl == "enabled" || tl == "enable" || tl == "engaged" || tl == "yes";
                const bool offLike = tl == "off" || tl == "out" || tl == "bypass" || tl == "bypassed" || tl == "no" || tl == "inactive";
                if ((! bypass && onLike) || (bypass && offLike)) { onNorm = norm; onText = text; found = true; }
            }
            if (! found) for (const auto& [text, norm] : c.texts) if (std::abs (norm - onNorm) < 1e-6f) onText = text;
            s.engage = e; s.engageNorm = onNorm; s.engageText = onText; break;
        }
        if (! s.engage) s.note = "no engage switch named in the section: measured as instantiated";
    }
    return out;
}

inline juce::String indicesOf (const Section& s) { juce::StringArray a; for (const auto& c : s.controls) a.add (juce::String (c.index)); return a.joinIntoString (","); }
inline juce::String presetOf (const Section& s) { return s.engage ? juce::String (s.engage->index) + ":" + juce::String (s.engageNorm, 6) : juce::String(); }
inline juce::String modeFor (const juce::String& section) { return section == "eq" ? "--cert-eq" : section == "gate" ? "--cert-dynamics" : section == "saturation" ? "--cert-saturation" : section == "compressor" ? "--cert-gain-cal" : juce::String(); }

} // namespace ejmap::strip
