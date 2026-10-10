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

struct Control { int index = -1; juce::String name; int numSteps = 0; std::map<juce::String, float> texts; float instNorm = 0.0f; juce::String instText; };   // texts: display -> norm (two-step switches); inst*: as instantiated
struct Section { juce::String name; std::vector<Control> controls; std::optional<Control> engage; float engageNorm = 1.0f, engageOffNorm = 0.0f; juce::String engageText, engageOffText, note; int numeric = 0; bool engagedAtInstantiate = false; using engage_type = Control; };
inline bool isDynamics (const juce::String& section) { return section == "compressor" || section == "gate"; }

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
            s.engage = e; s.engageNorm = onNorm; s.engageText = onText; s.engageOffNorm = onNorm > 0.5f ? 0.0f : 1.0f;
            for (const auto& [text, norm] : c.texts) if (std::abs (norm - s.engageOffNorm) < 1e-6f) s.engageOffText = text;
            s.engagedAtInstantiate = std::abs (c.instNorm - onNorm) < 1e-6f; break;
        }
        if (! s.engage) s.note = "no engage switch named in the section: measured as instantiated";
    }
    return out;
}

// THE OTHER DYNAMICS SECTIONS (Kathy's ruling, 7 Oct): when a dynamics section is swept, every OTHER dynamics section (gate / expander /
// a second compressor or limiter) engaged at instantiate is switched OFF by its own engage control for the whole sweep, detector and tone
// check, carried as a neutral write on the record and the profile (the server writes it too). EQ and saturation stay at instantiate,
// recorded. An other dynamics section WITHOUT an engage control -> refused: needs_review with that reason.
struct StripWrite { int index = -1; juce::String control, set, role; float norm = 0.0f; };   // role: "engage" (this section on) | "other_off" (another dynamics section off by its engage) | "other_neutral" (neutralised by its own controls)
struct OtherDynamics { bool ok = true; juce::String refused; std::vector<StripWrite> offWrites; juce::StringArray atInstantiate, alreadyOff, neutralised; };
// NEUTRALISING BY ITS OWN CONTROLS (Kathy's refined ruling, 7 Oct): an other dynamics section with no engage control is not held - a gate /
// expander's threshold goes to its fully-open end (the LOWEST threshold) and its range / depth to 0 where it has one; a second compressor /
// limiter's threshold to its no-compression end (the HIGHEST). The ends are read from the control's own texts at the ends (a dB number);
// a control whose ends do not read as numbers cannot be neutralised and the section refuses. Confirmed by measurement afterwards: the swept
// section's quiet reference rungs must read, else needs_review with that reason.
inline std::optional<double> dbOf (const juce::String& text) { const auto t = text.trim().removeCharacters ("+"); const auto num = t.upToFirstOccurrenceOf (" ", false, false).replace ("dB", ""); if (num.isEmpty() || ! num.containsAnyOf ("0123456789") || num.retainCharacters ("0123456789.-").length() != num.length()) return std::nullopt; return num.getDoubleValue(); }
inline std::optional<std::pair<float, juce::String>> endOf (const Control& c, bool lowest)
{
    std::optional<std::pair<float, juce::String>> best; std::optional<double> bestDb;
    for (const auto& [text, norm] : c.texts) if (const auto v = dbOf (text)) { if (! bestDb || (lowest ? *v < *bestDb : *v > *bestDb)) { bestDb = v; best = { norm, text }; } }
    return best;
}
inline std::optional<std::pair<float, juce::String>> zeroOf (const Control& c)
{
    std::optional<std::pair<float, juce::String>> best; double bestAbs = 1e9;
    for (const auto& [text, norm] : c.texts) if (const auto v = dbOf (text)) if (std::abs (*v) < bestAbs) { bestAbs = std::abs (*v); best = { norm, text }; }
    return best;
}
inline bool thresholdWord (const juce::String& name) { for (const auto& t : tokens (name)) if (t == "threshold" || t == "thresh" || t == "thr") return true; return false; }
inline bool rangeWord (const juce::String& name) { for (const auto& t : tokens (name)) if (t == "range" || t == "depth" || t == "floor") return true; return false; }
inline bool neutraliseByControls (const Section& s, std::vector<StripWrite>& out, juce::String& why)
{
    int n = 0;
    for (const auto& c : s.controls)
    {
        if (c.numSteps == 2) continue;
        if (thresholdWord (c.name) && ! rangeWord (c.name))
        {
            const auto e = endOf (c, s.name == "gate");   // a gate opens at its lowest threshold; a compressor stops at its highest
            if (! e) { why = "the strip's " + s.name + " section has no engage control and its '" + c.name + "' ends do not read as dB: it cannot be neutralised"; return false; }
            out.push_back ({ c.index, c.name, e->second, "other_neutral", e->first }); ++n;
        }
        else if (s.name == "gate" && rangeWord (c.name))
        {
            const auto z = zeroOf (c); if (! z) { why = "the strip's gate section has no engage control and its '" + c.name + "' has no 0 position readable: it cannot be neutralised"; return false; }
            out.push_back ({ c.index, c.name, z->second, "other_neutral", z->first }); ++n;
        }
    }
    if (n == 0) { why = "the strip's " + s.name + " section has no engage control and no threshold or range control to neutralise it by"; return false; }
    return true;
}
inline OtherDynamics otherDynamicsFor (const std::vector<Section>& sections, const juce::String& thisSection)
{
    OtherDynamics o;
    for (const auto& s : sections)
    {
        if (s.name == thisSection || s.name == "global") continue;
        if (! isDynamics (s.name)) { o.atInstantiate.add (s.name + (s.engage ? " (" + s.engage->name + " = '" + s.engage->instText + "')" : juce::String (" (no engage control)"))); continue; }
        if (! s.engage)
        {   // no engage control: neutralised by its own controls (the refined ruling), confirmed by measurement afterwards
            std::vector<StripWrite> ws; juce::String why;
            if (! neutraliseByControls (s, ws, why)) { o.ok = false; o.refused = why; return o; }
            for (const auto& w : ws) o.offWrites.push_back (w);
            juce::StringArray what; for (const auto& w : ws) what.add (w.control + " -> '" + w.set + "'"); o.neutralised.add (s.name + " (" + what.joinIntoString (", ") + ")");
            continue;
        }
        if (s.engagedAtInstantiate) o.offWrites.push_back ({ s.engage->index, s.engage->name, s.engageOffText, "other_off", s.engageOffNorm });
        else o.alreadyOff.add (s.name + " (" + s.engage->name + " = '" + s.engage->instText + "')");
    }
    return o;
}
// MODELLING NOISE OFF (Kathy, 10 Oct: bx_console N did not repeat serial vs serial - 440 record figures; its "Virtual Gain", the
// modelled analogue noise floor at -85 dB by default, made every quiet reading random: run-to-run |A-B| 0.03-0.05 dB median under
// -80 dB out, 0.000 above -40). A control NAMED as a noise source (noise / hiss / drift / wow / flutter / crackle, or Virtual Gain)
// that reads an OFF end ("-oo dB", "-inf", "Off", "0 %") is written to that end for every section, carried as a neutral write
// (role noise_off) on the record and the draft; the server writes it too. Not a noise SOURCE: a noise gate / reduction / filter /
// threshold / key. THD and console-channel / tolerance models are deterministic (the same every run): left as instantiated.
inline bool noiseSourceName (const juce::String& name)
{
    const auto t = tokens (name);
    // not a noise SOURCE (10 Oct census: UAD Oxide Tape "Noise Reduct", Retro Fi "Noise Bypass" - Off = the noise plays -, bx_dynEQ
    // "Noise Solo", Waves / FabFilter "Noise Shaping" - dither shaping)
    for (const auto& x : t) if (x == "gate" || x.startsWith ("reduc") || x == "filter" || x == "threshold" || x == "thresh" || x == "key" || x.startsWith ("suppress") || x == "shaper"
                                || x == "shaping" || x == "bypass" || x == "solo") return false;
    for (const auto& x : t) if (x == "noise" || x == "hiss" || x == "drift" || x == "wow" || x == "flutter" || x == "crackle") return true;
    const auto j = t.joinIntoString (" ");
    return j.contains ("virtual gain") || j == "vgain" || j.contains ("v gain");
}
inline bool offText (const juce::String& text)
{
    const auto s = text.trim().toLowerCase().removeCharacters (" ");
    return s.startsWith ("-oo") || s.startsWith ("-inf") || s.startsWith (juce::CharPointer_UTF8 ("-\xe2\x88\x9e")) || s == "off" || s == "0%" || s == "0.0%" || s == "0.00%";
}
inline std::vector<StripWrite> noiseOffWrites (const std::vector<Control>& controls)
{
    std::vector<StripWrite> out;
    for (const auto& c : controls)
    {
        if (! noiseSourceName (c.name)) continue;
        for (const auto& [text, norm] : c.texts)
            if (offText (text)) { if (! offText (c.instText)) out.push_back ({ c.index, c.name, text, "noise_off", norm }); break; }
    }
    return out;
}
inline std::vector<StripWrite> stripWrites (const Section& sec, const OtherDynamics& od, const std::vector<StripWrite>& noiseOff = {})
{
    std::vector<StripWrite> ws;
    if (sec.engage) ws.push_back ({ sec.engage->index, sec.engage->name, sec.engageText, "engage", sec.engageNorm });
    for (const auto& w : od.offWrites) ws.push_back (w);
    for (const auto& w : noiseOff) { bool dup = false; for (const auto& x : ws) dup = dup || x.index == w.index; if (! dup) ws.push_back (w); }
    return ws;
}
inline juce::String presetOf (const std::vector<StripWrite>& ws) { juce::StringArray a; for (const auto& w : ws) a.add (juce::String (w.index) + ":" + juce::String (w.norm, 6)); return a.joinIntoString (","); }
inline juce::var stripWritesVar (const std::vector<StripWrite>& ws) { juce::Array<juce::var> a; for (const auto& w : ws) { auto* o = new juce::DynamicObject(); o->setProperty ("index", w.index); o->setProperty ("control", w.control); o->setProperty ("set", w.set); o->setProperty ("norm", w.norm); o->setProperty ("role", w.role); a.add (juce::var (o)); } return a; }
inline juce::String indicesOf (const Section& s) { juce::StringArray a; for (const auto& c : s.controls) a.add (juce::String (c.index)); return a.joinIntoString (","); }
inline juce::String presetOf (const Section& s) { return s.engage ? juce::String (s.engage->index) + ":" + juce::String (s.engageNorm, 6) : juce::String(); }
inline juce::String modeFor (const juce::String& section) { return section == "eq" ? "--cert-eq" : section == "gate" ? "--cert-dynamics" : section == "saturation" ? "--cert-saturation" : section == "compressor" ? "--cert-gain-cal" : juce::String(); }

} // namespace ejmap::strip
