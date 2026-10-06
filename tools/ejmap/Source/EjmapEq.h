/*
  EjmapEq.h - EQ RESPONSE (roadmap 2.2). PROTOTYPE, 5 Oct 2026 overnight (Phase B4). Nothing exported, nothing published;
  `--cert-eq <product>` writes cert/eq/<identity>.eq.json and docs/EQ_PROPOSAL.md reads it.

  THE MEASUREMENT (probe_response.h): a 61-tone log multitone 20 Hz-20 kHz at -12 dBFS peak; per position of one control the
  output's magnitude at every tone against the input's. THE DERIVATION, pure (this file): deviation(f) = (out-in)(f) at the
  position minus (out-in)(f) at the band's baseline (everything as instantiated); the band's CENTRE is the tone of largest
  |deviation| (parabolic refinement in log f); its GAIN the deviation there; its BANDWIDTH the span between the two frequencies
  where the deviation falls 3 dB below the peak (interpolated on the log grid; a peak at the grid's end, or a deviation that
  never falls 3 dB on one side, is a SHELF or a cut-off and is said so, with no bandwidth). Labels: the gain control's text
  against the measured gain; the frequency control's text against the measured centre (a tone grid is a comb - 61 tones over
  three decades is ~ 1/20 octave - so the centre is known to that resolution, said in the record).
  THE BANDS, plan-time: a control is a band's gain / freq / q by its name's token (gain boost cut level; freq frequency hz khz;
  q width bandwidth bw shape slope), and the band's key is the rest of the name ("LF Gain" -> "LF", "Band 1 Freq" -> "Band 1");
  controls sharing a key form a band when it has at least a gain and a frequency. Pultec-style boost/atten pairs each count as a
  gain control of the same band.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <map>
#include <optional>
#include <vector>
#include "EjmapNameTokens.h"

namespace ejmap::eq
{

struct Tone { double hz = 0.0, inDb = -999.0, outDb = -999.0; };
struct Position { int k = -1; float norm = 0.0f; juce::String text; bool landed = true; std::vector<Tone> tones; };
struct Response { bool ok = false; juce::String refused; int ctl = -1; juce::String ctlName; int tones = 0; std::vector<Position> positions; };

inline Response parseResponse (const juce::String& out)
{
    Response r; Position* cur = nullptr;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        if (line.startsWith ("refused")) { r.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); return r; }
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 3) continue;
        if (f[0] == "response") { r.ok = true; r.ctl = kv (f, 1, "ctl").getIntValue(); r.ctlName = kv (f, 1, "name"); r.tones = kv (f, 1, "tones").getIntValue(); }
        else if (f[0] == "rpos") { Position p; p.k = f[1].getIntValue(); p.norm = (float) kv (f, 2, "norm").getDoubleValue(); p.text = kv (f, 2, "text"); p.landed = kv (f, 2, "confirm_ms").getDoubleValue() >= 0.0; r.positions.push_back (p); cur = &r.positions.back(); }
        else if (f[0] == "rtone" && cur != nullptr) { Tone t; t.hz = kv (f, 2, "hz").getDoubleValue(); t.inDb = kv (f, 2, "in_db").getDoubleValue(); t.outDb = kv (f, 2, "out_db").getDoubleValue(); cur->tones.push_back (t); }
    }
    return r;
}

// deviation per tone: (out - in) at the position minus (out - in) at the baseline, by matching tone frequency
inline std::vector<std::pair<double, double>> deviation (const Position& p, const Position& baseline)
{
    std::vector<std::pair<double, double>> d;
    for (const auto& t : p.tones)
        for (const auto& b : baseline.tones)
            if (std::abs (b.hz - t.hz) < 1e-6 && t.outDb > -200.0 && b.outDb > -200.0) { d.push_back ({ t.hz, (t.outDb - t.inDb) - (b.outDb - b.inDb) }); break; }
    return d;
}

struct Band
{
    juce::String result = "refused", reason;    // measured | shelf | flat | refused
    double centreHz = 0.0, gainDb = 0.0, bandwidthOct = 0.0, lowHz = 0.0, highHz = 0.0, worstOffGridDb = 0.0;
    double cornerHz = 0.0;                        // a shelf's corner: where the deviation is half the plateau (the frequency label's meaning for a shelf)
    int tonesUsed = 0; juce::String shape;        // peak | low_shelf | high_shelf | lowcut | highcut
};
inline constexpr double kFlatDb = 0.5;           // a deviation that never exceeds this is "flat" (the control did nothing at this position)

inline Band deriveBand (const std::vector<std::pair<double, double>>& dev)
{
    Band b;
    if (dev.size() < 8) { b.reason = "fewer than 8 tones compared"; return b; }
    b.tonesUsed = (int) dev.size();
    size_t ip = 0; for (size_t i = 1; i < dev.size(); ++i) if (std::abs (dev[i].second) > std::abs (dev[ip].second)) ip = i;
    const double peak = dev[ip].second;
    if (std::abs (peak) < kFlatDb) { b.result = "flat"; b.reason = "the largest deviation is " + juce::String (peak, 2) + " dB"; return b; }
    // parabolic refinement of the centre in log f (not at the grid's ends)
    double lc = std::log2 (dev[ip].first);
    if (ip > 0 && ip + 1 < dev.size())
    {
        const double y0 = std::abs (dev[ip - 1].second), y1 = std::abs (dev[ip].second), y2 = std::abs (dev[ip + 1].second);
        const double den = y0 - 2.0 * y1 + y2;
        if (std::abs (den) > 1e-9) { const double off = juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / den); lc += off * (std::log2 (dev[ip + 1].first) - std::log2 (dev[ip].first)); }
    }
    b.centreHz = std::pow (2.0, lc); b.gainDb = peak;
    // the -3 dB points relative to the peak, on each side
    const double target = std::abs (peak) - 3.0;
    auto cross = [&] (int dir) -> std::optional<double>
    {
        for (long i = (long) ip; i + dir >= 0 && i + dir < (long) dev.size(); i += dir)
        {
            const double a = std::abs (dev[(size_t) i].second), c = std::abs (dev[(size_t) (i + dir)].second);
            if (a >= target && c < target) { const double f = (a - target) / (a - c); return std::pow (2.0, std::log2 (dev[(size_t) i].first) + f * (std::log2 (dev[(size_t) (i + dir)].first) - std::log2 (dev[(size_t) i].first))); }
        }
        return std::nullopt;
    };
    const auto lo = cross (-1), hi = cross (+1);
    const bool atLowEnd = ip == 0, atHighEnd = ip + 1 == dev.size();
    if (lo && hi) { b.lowHz = *lo; b.highHz = *hi; b.bandwidthOct = std::log2 (*hi / *lo); b.result = "measured"; b.shape = "peak"; return b; }
    // a shelf: the plateau is the peak; its CORNER is where the deviation crosses half the plateau, walking from the plateau toward the grid's interior
    auto half = [&] (int dir) -> std::optional<double>
    {
        const double t = std::abs (peak) * 0.5;
        for (long i = (long) ip; i + dir >= 0 && i + dir < (long) dev.size(); i += dir)
        {
            const double a = std::abs (dev[(size_t) i].second), c = std::abs (dev[(size_t) (i + dir)].second);
            if (a >= t && c < t) { const double f = (a - t) / (a - c); return std::pow (2.0, std::log2 (dev[(size_t) i].first) + f * (std::log2 (dev[(size_t) (i + dir)].first) - std::log2 (dev[(size_t) i].first))); }
        }
        return std::nullopt;
    };
    if (! lo && hi) { b.highHz = *hi; b.result = "shelf"; b.shape = "low_shelf"; if (const auto c = half (+1)) b.cornerHz = *c; b.reason = "no -3 dB point below the centre: a low shelf (or the band's low side lies under 20 Hz)"; return b; }
    if (lo && ! hi) { b.lowHz = *lo; b.result = "shelf"; b.shape = "high_shelf"; if (const auto c = half (-1)) b.cornerHz = *c; b.reason = "no -3 dB point above the centre: a high shelf (or the band's high side lies over 20 kHz)"; return b; }
    b.result = "shelf"; b.shape = atLowEnd ? "low_shelf" : atHighEnd ? "high_shelf" : "broad"; b.reason = "no -3 dB point on either side: broader than the grid, or a shelf spanning it";
    if (atLowEnd) { if (const auto c = half (+1)) b.cornerHz = *c; } else if (atHighEnd) { if (const auto c = half (-1)) b.cornerHz = *c; }
    return b;
}

inline std::optional<double> labelNumber (const juce::String& display)
{
    auto t = display.trim().removeCharacters ("+");
    const bool k = t.containsIgnoreCase ("k");
    t = t.upToFirstOccurrenceOf (" ", false, false).replace ("dB", "").replace ("Hz", "").replace ("hz", "").replace ("kHz", "").replace ("k", "").replace ("K", "");
    if (t.isEmpty() || ! t.containsAnyOf ("0123456789") || t.retainCharacters ("0123456789.-").length() != t.length()) return std::nullopt;
    const double v = t.getDoubleValue();
    return k && ! display.containsIgnoreCase ("db") ? v * 1000.0 : v;
}

// THE BANDS from the control names (plan-time lexicon, here and not in EjmapRoles)
enum class BandRole { none, gain, freq, q };
inline BandRole bandRole (const juce::String& name)
{
    for (const char* t : { "freq", "frequency", "frq", "hz", "khz" }) if (nametokens::controlAnswersTerm (name, t)) return BandRole::freq;   // "frq": Waves Q10 / REQ (6 Oct)
    for (const char* t : { "q", "width", "bandwidth", "bw", "shape", "slope" }) if (nametokens::controlAnswersTerm (name, t)) return BandRole::q;
    for (const char* t : { "gain", "boost", "cut", "atten", "attenuation", "level", "db" }) if (nametokens::controlAnswersTerm (name, t)) return BandRole::gain;
    return BandRole::none;
}
inline juce::String bandKey (const juce::String& name)
{
    juce::StringArray toks = juce::StringArray::fromTokens (name, " -_/", "\"'"); juce::StringArray keep;
    for (const auto& tk : toks)
    {
        const auto l = tk.toLowerCase();
        bool roleWord = false;
        for (const char* t : { "freq", "frequency", "frq", "hz", "khz", "q", "width", "bandwidth", "bw", "shape", "slope", "gain", "boost", "cut", "atten", "attenuation", "level", "db" }) if (l == t) roleWord = true;
        if (! roleWord && l.isNotEmpty()) keep.add (tk);
    }
    return keep.joinIntoString (" ").trim();
}
// ENGAGE CANDIDATES (5 Oct R8a): a band that reads flat at every gain position may be switched off (bx_digital's second
// channel: "EQ Band HF 2 On"). The switch-like controls (two steps or word-valued) that share the band key's tokens, the
// closest first (most shared tokens), then global on / in / enable / bypass switches that share the key's first token (the
// channel). The ON norm by text: on / in / active / enable / engage -> that position; bypass / off -> the other.
struct EngageCandidate { int index = -1; juce::String name; float onNorm = 1.0f; juce::String onText; int shared = 0; };
inline std::vector<EngageCandidate> engageCandidates (const juce::String& bandKey, const std::vector<std::tuple<int, juce::String, bool, std::map<juce::String, float>>>& switches, const std::vector<int>& exclude)
{
    // switches: (index, name, isSwitch, texts -> norm)
    std::vector<EngageCandidate> out;
    juce::StringArray keyToks = juce::StringArray::fromTokens (bandKey.toLowerCase(), " -_/", "\"'"); keyToks.removeEmptyStrings();
    for (const auto& [idx, name, isSwitch, texts] : switches)
    {
        if (! isSwitch || std::find (exclude.begin(), exclude.end(), idx) != exclude.end()) continue;
        juce::StringArray nameToks = juce::StringArray::fromTokens (name.toLowerCase(), " -_/", "\"'"); nameToks.removeEmptyStrings();
        int shared = 0; for (const auto& t : keyToks) if (nameToks.contains (t)) ++shared;
        // every token of the switch's name must be the band's or a switch word: "EQ Band LF 2 On" is another band's, "Master Bypass" is global
        int foreign = 0; for (const auto& t : nameToks) { const bool sw = t == "on" || t == "in" || t == "enable" || t == "enabled" || t == "active" || t == "bypass" || t == "engage" || t == "power" || t == "mute" || t == "off" || t == "out"; if (! sw && ! keyToks.contains (t)) ++foreign; }
        if (shared == 0 || foreign > 0) continue;
        EngageCandidate c; c.index = idx; c.name = name; c.shared = shared;
        bool bypassName = nameToks.contains ("bypass") || nameToks.contains ("mute");
        bool found = false;
        for (const auto& [text, norm] : texts)
        {
            const auto t = text.toLowerCase().trim();
            const bool onLike = t == "on" || t == "in" || t == "active" || t == "enabled" || t == "enable" || t == "engaged" || t == "yes";
            const bool offLike = t == "off" || t == "out" || t == "bypass" || t == "bypassed" || t == "no" || t == "inactive";
            if ((! bypassName && onLike) || (bypassName && offLike)) { c.onNorm = norm; c.onText = text; found = true; }
        }
        if (! found) { c.onNorm = bypassName ? 0.0f : 1.0f; for (const auto& [text, norm] : texts) if (std::abs (norm - c.onNorm) < 1e-6) c.onText = text; }
        out.push_back (c);
    }
    std::sort (out.begin(), out.end(), [] (const EngageCandidate& a, const EngageCandidate& b) { return a.shared != b.shared ? a.shared > b.shared : a.index < b.index; });
    return out;
}
// A pool control the MEASURED fallback may read as a band's gain: not one whose name already says frequency or Q (REQ 2's
// "Band1 Frq" moves the tone's level by 92 dB as a low-cut's corner crosses it - a frequency, never a gain; 6 Oct)
inline bool measuredGainCandidate (const juce::String& name) { const auto r = bandRole (name); return r != BandRole::freq && r != BandRole::q; }
struct BandControls { juce::String key; std::vector<int> gains, freqs, qs; std::vector<juce::String> gainNames, freqNames, qNames; };
inline std::vector<BandControls> bandsFrom (const std::vector<std::pair<int, juce::String>>& controls)
{
    std::map<juce::String, BandControls> by;
    for (const auto& [idx, name] : controls)
    {
        const auto role = bandRole (name); if (role == BandRole::none) continue;
        auto& b = by[bandKey (name)]; b.key = bandKey (name);
        if (role == BandRole::gain) { b.gains.push_back (idx); b.gainNames.push_back (name); }
        else if (role == BandRole::freq) { b.freqs.push_back (idx); b.freqNames.push_back (name); }
        else { b.qs.push_back (idx); b.qNames.push_back (name); }
    }
    std::vector<BandControls> out;
    for (auto& [k, b] : by) if (! b.gains.empty() && ! b.freqs.empty()) out.push_back (b);
    return out;
}

} // namespace ejmap::eq
