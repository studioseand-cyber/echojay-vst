#pragma once
// BuiltinIntentTranslate (18 Sep 2026, item 4): a SUBSTITUTED built-in must always be dialled.
//
// The model wrote settings for the third-party plugin it named (AMEK Mastering Compressor: "Ratio 2:1",
// "Threshold -18 dB", "Attack 30 ms", "Release auto", "3-4 dB GR"). When the slot becomes a built-in, those
// arrive as {"controls": {...}} - the third-party shape - and the built-in's applyStructured only understands
// {"params": {<schema id>: number}}. Before this file the payload was dropped whole: "EchoJay Compressor needs
// hand-dialing (0/3 applied)". This translates BY SEMANTIC into the built-in's schema: the control NAME picks
// the id (ratio / threshold / attack / release / makeup / knee / mix / range / ceiling / input / freq / drive /
// decay / predelay / size / damping ...), the VALUE is parsed by unit ("2:1", "-18 dB", "30 ms", "2.5 s",
// "auto", "fast", "3-4 dB"), and a gain-reduction target with no threshold DERIVES the threshold from the
// measured input level: threshold = p90 - GR * ratio / (ratio - 1). Only ids the device's schema knows are
// emitted, so nothing is guessed at a knob that does not exist; what could not translate is listed.
#include <JuceHeader.h>
#include "EedParamSchema.h"
#include <cmath>
#include <limits>

namespace echojay {

struct IntentTranslation
{
    juce::var        payload;            // {"params": {...}} or {"eq_bands": [...]}; void when nothing translated
    juce::StringArray translated;        // "Ratio 2:1 -> ratio 2"
    juce::StringArray dropped;           // "Timing 1 (no built-in control for it)"
    bool derivedThreshold = false;
    juce::String note;                   // one line for the log
    int count() const noexcept { return translated.size(); }
};

namespace detail {
inline juce::String lower (const juce::String& s) { return s.trim().toLowerCase(); }
inline bool has (const juce::String& hay, const char* needle) { return hay.contains (needle); }

// The first number in a string, with its unit hint. "2:1" -> 2 (ratio); "-18 dB" -> -18; "30 ms" -> 30;
// "2.5 s" -> 2.5 (unit s); "3-4 dB" -> 3.5 (a range reads as its midpoint); "auto" / "fast" -> NaN (words).
struct Num { double v = std::numeric_limits<double>::quiet_NaN(); juce::String unit; bool ok = false; bool range = false; };
inline Num parseNum (const juce::var& value)
{
    Num out;
    if (value.isDouble() || value.isInt() || value.isInt64()) { out.v = (double) value; out.ok = true; return out; }
    if (value.isBool()) { out.v = (bool) value ? 1.0 : 0.0; out.ok = true; return out; }
    const auto s = lower (value.toString());
    if (s.isEmpty()) return out;
    // ratio "2:1" / "4 : 1"
    if (s.containsChar (':'))
    {
        const auto a = s.upToFirstOccurrenceOf (":", false, false).retainCharacters ("0123456789.");
        if (a.isNotEmpty()) { out.v = a.getDoubleValue(); out.ok = true; out.unit = "ratio"; return out; }
    }
    // a range "3-4", "3 to 4", "3–4" -> midpoint (only when the first char is not a minus sign of a negative)
    {
        juce::String t = s.replace ("\xe2\x80\x93", "-").replace (" to ", "-");
        const int dash = t.indexOfChar (1, '-');
        if (dash > 0 && juce::CharacterFunctions::isDigit (t[dash - 1]) && dash + 1 < t.length() && (juce::CharacterFunctions::isDigit (t[dash + 1]) || t[dash + 1] == '.'))
        {
            const auto a = t.substring (0, dash).retainCharacters ("0123456789.-");
            auto b = t.substring (dash + 1); int e = 0; while (e < b.length() && (juce::CharacterFunctions::isDigit (b[e]) || b[e] == '.')) ++e;
            b = b.substring (0, e);
            if (a.isNotEmpty() && b.isNotEmpty()) { out.v = 0.5 * (a.getDoubleValue() + b.getDoubleValue()); out.ok = true; out.range = true; }
        }
    }
    if (! out.ok)
    {
        int i = 0; while (i < s.length() && ! (juce::CharacterFunctions::isDigit (s[i]) || ((s[i] == '-' || s[i] == '+' || s[i] == '.') && i + 1 < s.length() && juce::CharacterFunctions::isDigit (s[i + 1])))) ++i;
        if (i >= s.length()) return out;   // no digits: a word ("auto", "fast")
        int e = i + 1; while (e < s.length() && (juce::CharacterFunctions::isDigit (s[e]) || s[e] == '.')) ++e;
        out.v = s.substring (i, e).getDoubleValue(); out.ok = true;
    }
    if (has (s, "khz")) out.unit = "khz"; else if (has (s, "hz")) out.unit = "hz";
    else if (has (s, "ms")) out.unit = "ms"; else if (s.matchesWildcard ("*[0-9] s", true) || s.endsWith ("s") || has (s, " sec")) out.unit = "s";
    else if (has (s, "db")) out.unit = "db"; else if (has (s, "%")) out.unit = "pct";
    return out;
}
inline double toMs (const Num& n) { return n.unit == "s" ? n.v * 1000.0 : n.v; }
inline double toS  (const Num& n) { return n.unit == "ms" ? n.v / 1000.0 : n.v; }
inline double toHz (const Num& n) { return n.unit == "khz" ? n.v * 1000.0 : n.v; }
} // namespace detail

// deviceName: "EchoJay Compressor" etc. controls: the {"controls": {...}} object (or its inner map).
// inputP90Db: the slot's measured input p90 (dBFS RMS or LUFS), NaN when unknown - the GR derivation needs it.
inline IntentTranslation translateControlsForBuiltin (const juce::String& deviceName, const ParamSchema& schema,
                                                       const juce::var& controlsIn, float inputP90Db)
{
    using namespace detail;
    IntentTranslation out;
    juce::var controls = controlsIn;
    if (auto* o = controlsIn.getDynamicObject()) if (o->hasProperty ("controls")) controls = o->getProperty ("controls");
    auto* co = controls.getDynamicObject();
    if (co == nullptr) { out.note = "no controls object"; return out; }
    const auto dev = lower (deviceName);
    const bool isEq = dev.contains ("eq");
    auto* params = new juce::DynamicObject();
    juce::var paramsVar (params);
    juce::Array<juce::var> bands;
    auto known = [&] (const char* id) { return schema.find (id) != nullptr; };
    auto put = [&] (const char* id, double v, const juce::String& from)
    {
        if (! known (id)) { out.dropped.add (from + " (no \"" + id + "\" on " + deviceName + ")"); return false; }
        params->setProperty (id, v); out.translated.add (from + " -> " + id + " " + juce::String (v, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")); return true;
    };
    double ratio = std::numeric_limits<double>::quiet_NaN(), grDb = std::numeric_limits<double>::quiet_NaN();
    bool thresholdGiven = false; juce::String grFrom;

    for (const auto& p : co->getProperties())
    {
        const auto name = lower (p.name.toString());
        const auto from = p.name.toString() + " " + p.value.toString();
        const Num n = parseNum (p.value);
        const auto word = lower (p.value.toString());
        // ---- dynamics ------------------------------------------------------------------------------------
        if (has (name, "gain reduction") || name == "gr" || has (name, " gr") || name.startsWith ("gr ") || has (name, "reduction"))
        { if (n.ok) { grDb = std::abs (n.v); grFrom = from; } else out.dropped.add (from + " (no figure)"); continue; }
        if (has (name, "ratio")) { if (n.ok) { ratio = n.v; put ("ratio", n.v, from); } else out.dropped.add (from + " (no figure)"); continue; }
        if (has (name, "thresh") || name == "thr" || name.startsWith ("thr ")) { if (n.ok) { thresholdGiven = put ("threshold_db", n.v, from); } else out.dropped.add (from + " (no figure)"); continue; }
        if (has (name, "attack"))
        {
            if (n.ok) put ("attack_ms", toMs (n), from);
            else if (has (word, "fast")) put ("attack_ms", 1.0, from + " (fast = 1 ms)"); else if (has (word, "slow")) put ("attack_ms", 30.0, from + " (slow = 30 ms)"); else if (has (word, "med")) put ("attack_ms", 10.0, from + " (medium = 10 ms)");
            else out.dropped.add (from + " (not a time)");
            continue;
        }
        if (has (name, "release"))
        {
            if (n.ok) put ("release_ms", toMs (n), from);
            else if (has (word, "auto")) { if (! put ("auto_release", 1.0, from)) {} }
            else if (has (word, "fast")) put ("release_ms", 50.0, from + " (fast = 50 ms)"); else if (has (word, "slow")) put ("release_ms", 400.0, from + " (slow = 400 ms)"); else if (has (word, "med")) put ("release_ms", 120.0, from + " (medium = 120 ms)");
            else out.dropped.add (from + " (not a time)");
            continue;
        }
        if (has (name, "timing"))   // AMEK-style one-knob attack/release
        {
            if (has (word, "fast")) { put ("attack_ms", 1.0, from + " (fast)"); put ("release_ms", 50.0, from + " (fast)"); }
            else if (has (word, "slow")) { put ("attack_ms", 30.0, from + " (slow)"); put ("release_ms", 400.0, from + " (slow)"); }
            else if (has (word, "auto")) { put ("auto_release", 1.0, from); }
            else out.dropped.add (from + " (no built-in control for it)");
            continue;
        }
        if (has (name, "knee")) { if (n.ok) put ("knee_db", n.v, from); else if (has (word, "soft")) put ("knee_db", 12.0, from + " (soft = 12 dB)"); else if (has (word, "hard")) put ("knee_db", 0.0, from + " (hard = 0 dB)"); else out.dropped.add (from); continue; }
        if (has (name, "ceiling") || has (name, "out ceil")) { if (n.ok) put ("ceiling_db", n.v, from); else out.dropped.add (from); continue; }
        if (has (name, "lookahead")) { if (n.ok) put ("lookahead_ms", toMs (n), from); else out.dropped.add (from); continue; }
        if (has (name, "sidechain") || has (name, "sc hpf") || has (name, "sc filter")) { if (n.ok) put ("sc_hpf_hz", toHz (n), from); else out.dropped.add (from); continue; }
        if (has (name, "range") || has (name, "depth") || (has (name, "amount") && (dev.contains ("de-ess") || dev.contains ("gate") || dev.contains ("expand"))))
        { if (n.ok) put ("range_db", std::abs (n.v), from); else out.dropped.add (from); continue; }
        // ---- gain-ish ------------------------------------------------------------------------------------
        if (has (name, "makeup") || has (name, "make-up") || has (name, "make up"))
        { if (n.ok) put ("makeup_db", n.v, from); else if (has (word, "auto")) out.dropped.add (from + " (auto makeup has no built-in control)"); else out.dropped.add (from); continue; }
        if (has (name, "input") || has (name, "in gain") || (has (name, "drive") && ! dev.contains ("satur") && ! dev.contains ("tape")))
        { if (n.ok) { if (! put ("input_db", n.v, from)) put ("drive_db", n.v, from); } else out.dropped.add (from); continue; }
        if (has (name, "output") || has (name, "trim") || has (name, "out gain") || (name == "gain" || has (name, "gain")))
        {
            if (! n.ok) { out.dropped.add (from); continue; }
            if (known ("makeup_db")) put ("makeup_db", n.v, from); else if (! put ("output_db", n.v, from)) put ("gain_db", n.v, from);
            continue;
        }
        if (has (name, "mix") || has (name, "blend") || has (name, "wet") || has (name, "dry"))
        { if (n.ok) { double v = n.v; if (v <= 1.0 && n.unit != "pct" && ! (word.contains ("%"))) v *= 100.0; put ("mix", v, from); } else out.dropped.add (from); continue; }
        // ---- saturation / tape --------------------------------------------------------------------------
        if (has (name, "drive") || has (name, "saturation") || has (name, "amount"))
        { if (n.ok) put ("drive_db", n.v, from); else if (has (word, "low")) put ("drive_db", 3.0, from + " (low = 3 dB)"); else if (has (word, "med")) put ("drive_db", 6.0, from + " (medium = 6 dB)"); else if (has (word, "high") || has (word, "heavy")) put ("drive_db", 12.0, from + " (heavy = 12 dB)"); else out.dropped.add (from); continue; }
        if (has (name, "tone")) { if (n.ok) put ("tone_db", n.v, from); else out.dropped.add (from); continue; }
        if (name == "type" || has (name, "character") || has (name, "mode"))
        {
            if (const auto* spec = schema.find ("type")) { int ix = 0; bool hit = false; for (const auto& c : spec->choices) { if (word.contains (lower (c))) { hit = true; break; } ++ix; } if (hit) { params->setProperty ("type", ix); out.translated.add (from + " -> type " + juce::String (ix)); continue; } }
            if (const auto* spec = schema.find ("mode")) { int ix = 0; bool hit = false; for (const auto& c : spec->choices) { if (word.contains (lower (c))) { hit = true; break; } ++ix; } if (hit) { params->setProperty ("mode", ix); out.translated.add (from + " -> mode " + juce::String (ix)); continue; } }
            out.dropped.add (from + " (no matching mode)"); continue;
        }
        // ---- de-esser / reverb / filters ----------------------------------------------------------------
        if (has (name, "freq") || has (name, "frequency") || has (name, "centre") || has (name, "center"))
        { if (n.ok) put ("freq_hz", toHz (n), from); else out.dropped.add (from); continue; }
        if (has (name, "decay") || has (name, "rt60") || has (name, "reverb time") || (has (name, "time") && dev.contains ("reverb")))
        { if (n.ok) put ("decay_s", toS (n), from); else out.dropped.add (from); continue; }
        if (has (name, "pre-delay") || has (name, "predelay") || has (name, "pre delay")) { if (n.ok) put ("predelay_ms", toMs (n), from); else out.dropped.add (from); continue; }
        if (has (name, "size") || has (name, "room")) { if (n.ok) put ("size", n.v, from); else if (has (word, "small")) put ("size", 25.0, from + " (small = 25)"); else if (has (word, "large") || has (word, "big")) put ("size", 80.0, from + " (large = 80)"); else out.dropped.add (from); continue; }
        if (has (name, "damp")) { if (n.ok) put ("damping", n.v, from); else out.dropped.add (from); continue; }
        if (has (name, "width")) { if (n.ok) put ("width", n.v, from); else out.dropped.add (from); continue; }
        if (has (name, "low cut") || has (name, "lowcut") || has (name, "hpf") || has (name, "high pass") || has (name, "high-pass") || has (name, "highpass"))
        {
            if (! n.ok) { out.dropped.add (from); continue; }
            if (isEq) { auto* b = new juce::DynamicObject(); b->setProperty ("type", "highpass"); b->setProperty ("freq_hz", toHz (n)); bands.add (juce::var (b)); out.translated.add (from + " -> highpass " + juce::String (toHz (n)) + " Hz"); }
            else if (! put ("low_cut_hz", toHz (n), from)) put ("hpf_hz", toHz (n), from);
            continue;
        }
        if (isEq && (has (name, "low pass") || has (name, "lpf") || has (name, "lowpass") || has (name, "high cut")))
        { if (n.ok) { auto* b = new juce::DynamicObject(); b->setProperty ("type", "lowpass"); b->setProperty ("freq_hz", toHz (n)); bands.add (juce::var (b)); out.translated.add (from + " -> lowpass"); } else out.dropped.add (from); continue; }
        if (isEq && (has (name, "shelf") || has (name, "bell") || has (name, "band") || has (name, "peak") || has (name, "hz")))
        {   // "Bell 250 Hz": gain from the value, frequency from the name or value
            const Num fn = parseNum (juce::var (p.name.toString()));
            const double f = fn.ok ? toHz (fn) : (n.unit == "hz" || n.unit == "khz" ? toHz (n) : 0.0);
            const double g = (n.ok && n.unit == "db") ? n.v : 0.0;
            if (f > 0.0) { auto* b = new juce::DynamicObject(); b->setProperty ("type", has (name, "low shelf") || has (name, "lowshelf") ? "lowshelf" : has (name, "high shelf") || has (name, "highshelf") ? "highshelf" : "bell"); b->setProperty ("freq_hz", f); b->setProperty ("gain_db", g); bands.add (juce::var (b)); out.translated.add (from + " -> band " + juce::String (f) + " Hz " + juce::String (g, 1) + " dB"); }
            else out.dropped.add (from + " (no frequency)");
            continue;
        }
        out.dropped.add (from + " (no built-in control for it)");
    }
    // A GR target with no threshold DERIVES it from the measured input level.
    if (! thresholdGiven && std::isfinite (grDb) && known ("threshold_db"))
    {
        if (std::isfinite (inputP90Db))
        {
            const double r = std::isfinite (ratio) && ratio > 1.0 ? ratio : (schema.find ("ratio") ? schema.find ("ratio")->def : 4.0);
            const double thr = (double) inputP90Db - grDb * r / (r - 1.0);
            params->setProperty ("threshold_db", thr); out.derivedThreshold = true;
            out.translated.add (grFrom + " -> threshold_db " + juce::String (thr, 1) + " (derived: input p90 " + juce::String (inputP90Db, 1) + " dB, ratio " + juce::String (r, 1) + ")");
        }
        else out.dropped.add (grFrom + " (needs the measured input level to derive a threshold; none yet)");
    }
    else if (std::isfinite (grDb) && thresholdGiven) out.translated.add (grFrom + " (threshold given, GR target noted)");

    if (isEq && bands.size() > 0)
    {
        auto* w = new juce::DynamicObject(); w->setProperty ("eq_bands", juce::var (bands));
        if (params->getProperties().size() > 0) w->setProperty ("eq_settings", paramsVar);
        out.payload = juce::var (w);
    }
    else if (params->getProperties().size() > 0)
    {
        auto* w = new juce::DynamicObject(); w->setProperty ("params", paramsVar); out.payload = juce::var (w);
    }
    out.note = juce::String (out.translated.size()) + " translated, " + juce::String (out.dropped.size()) + " dropped";
    return out;
}

} // namespace echojay
