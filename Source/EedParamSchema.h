/*
    EedParamSchema.h  —  the universal dialable contract for EchoJay's built-in
    devices (BUILTIN_SUITE_PLAN.md §3).

    ONE schema per device, and it drives three things that would otherwise drift
    apart:
      (a) the AI feed TEACHES it            (EchoJayAPI advertisement),
      (b) the server VALIDATES/CLAMPS to it (same numbers, shipped in the feed),
      (c) the processor MAPS id->knob       (EedDeviceProcessor::applyParams).

    That is the point: building a device's schema IS building its dialability, so
    nothing can ship clickable-but-not-dialable. A knob with no ParamSpec is not
    advertised, not validated and not settable — it simply doesn't exist to the AI,
    which is a loud failure rather than a silent one.

    Deliberately JUCE-FREE, like EqEngine/EqMove: it is a data contract, not UI,
    so it builds and unit-tests under plain g++ (test/param_schema_test.cpp).
    Values are real-world units throughout — dB, Hz, ms, % — never normalised
    0..1. The model reasons in dB; making it reason in 0..1 is how exactness dies.
*/

#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace echojay
{

// Canonical form used for matching: lowercased, separators dropped, so
// "attack_ms", "Attack-MS" and "attackMs" are the same parameter — and so are
// "Low Shelf" and "lowshelf".
//
// Tolerance here is not sloppiness — it is the difference between a move that
// lands and one that silently does nothing. An id that misses is a knob that
// never turns, with no error anywhere; the cost of accepting a near-miss is
// zero, and the cost of rejecting it is a broken feature.
inline std::string normalizeParamToken (const std::string& raw)
{
    std::string out;
    out.reserve (raw.size());
    for (char c : raw)
    {
        if (c >= 'A' && c <= 'Z')      out += (char) (c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
        // every other character (_ - space .) is a separator: dropped
    }
    return out;
}

// ---------------------------------------------------------------------------
// One dialable parameter.
// ---------------------------------------------------------------------------
struct ParamSpec
{
    std::string id;             // canonical snake_case: "level_db", "pan"
    std::string unit;           // "dB" | "Hz" | "ms" | "%" | "" when dimensionless
    double      min = 0.0;
    double      max = 1.0;
    double      def = 0.0;
    std::string description;    // one line: what turning it actually does
    bool        boolean = false; // 0/1 switch — advertised as on/off, not a range

    // Enumerated values in index order — the param's VALUE is the index into
    // this list, so `min`/`max` stay 0..choices.size()-1 and every existing
    // clamp, state and editor path keeps working unchanged.
    //
    // Why this exists: a selector is a knob, and the house rule is that a knob a
    // human can turn is one the model must be able to set exactly. Without it a
    // curve selector would have to be advertised as a bare 0..3, which the model
    // gets wrong for the obvious reason that "tube" is not a number. With it,
    // `"type": "tube"` and `"type": 0` both land, and the advertisement carries
    // the names rather than an index the model has to memorise.
    std::vector<std::string> choices;

    // Clamp an incoming value into the advertised range.
    //
    // NaN maps to the DEFAULT rather than to a bound. A NaN that survives into a
    // gain coefficient poisons every sample downstream of it and is inaudible as
    // a bug until the whole chain goes silent; JSON is a network-shaped input, so
    // this is a real path, not a theoretical one.
    double clamp (double v) const noexcept
    {
        if (std::isnan (v)) return def;
        if (v < min) return min;
        if (v > max) return max;
        return v;
    }

    // Index of a named choice, or -1 when the label is not one of ours. Matching
    // is as tolerant as id matching: "Low Shelf", "low_shelf" and "lowshelf" are
    // the same choice.
    int indexOfChoice (const std::string& label) const
    {
        const auto key = normalizeParamToken (label);
        if (key.empty()) return -1;
        for (std::size_t i = 0; i < choices.size(); ++i)
            if (normalizeParamToken (choices[i]) == key) return (int) i;
        return -1;
    }

    // The label a value selects. Empty for a param with no choices, so a caller
    // can use emptiness to mean "this is a number, print it as one".
    std::string choiceLabel (double v) const
    {
        if (choices.empty()) return {};
        long idx = std::lround (std::isnan (v) ? def : v);
        if (idx < 0) idx = 0;
        if (idx >= (long) choices.size()) idx = (long) choices.size() - 1;
        return choices[(std::size_t) idx];
    }
};

// ---------------------------------------------------------------------------
// A device's whole dialable surface.
// ---------------------------------------------------------------------------
class ParamSchema
{
public:
    ParamSchema() = default;
    explicit ParamSchema (std::vector<ParamSpec> p) : params_ (std::move (p)) {}

    const std::vector<ParamSpec>& params() const noexcept { return params_; }
    bool        empty() const noexcept { return params_.empty(); }
    std::size_t size()  const noexcept { return params_.size(); }

    // See normalizeParamToken: ids and choice labels fold the same way, so a
    // device cannot end up strict about one and tolerant about the other.
    static std::string normalizeId (const std::string& raw)
    {
        return normalizeParamToken (raw);
    }

    // Nullptr when the id is not part of this device's contract.
    const ParamSpec* find (const std::string& id) const noexcept
    {
        const auto key = normalizeId (id);
        for (const auto& p : params_)
            if (normalizeId (p.id) == key) return &p;
        return nullptr;
    }

    // Human/model-readable number: 3 not 3.000000, -7.5 not -7.500000.
    static std::string number (double v)
    {
        if (std::isnan (v)) return "0";
        char buf[32];
        // %g gives the shortest faithful form and drops trailing zeros, which is
        // what keeps the advertisement compact enough to ship on every turn.
        std::snprintf (buf, sizeof (buf), "%g", v);
        return std::string (buf);
    }

    // One advertisement line for a param. This exact text is what the model reads
    // and what the server validates against, so the two cannot disagree.
    //   "threshold_db (dB, -60..0, default -18) — level where compression starts"
    //   "auto_gain (on/off, default off) — cancel the loudness change"
    //   "type (tube|tape|diode|soft, default tube) — the saturation curve"
    static std::string describeLine (const ParamSpec& p)
    {
        std::string s = p.id + " (";
        if (! p.choices.empty())
        {
            // Names, not the index behind them. The model picks "tube"; that the
            // wire value is 0 is an implementation detail it never has to know.
            for (std::size_t i = 0; i < p.choices.size(); ++i)
            {
                if (i > 0) s += "|";
                s += p.choices[i];
            }
            s += ", default " + p.choiceLabel (p.def);
        }
        else if (p.boolean)
        {
            s += "on/off, default ";
            s += (p.def >= 0.5 ? "on" : "off");
        }
        else
        {
            if (! p.unit.empty()) s += p.unit + ", ";
            s += number (p.min) + ".." + number (p.max)
               + ", default " + number (p.def);
        }
        s += ")";
        if (! p.description.empty()) s += " - " + p.description;
        return s;
    }

    // The whole contract, one param per line. Empty for a device with no params.
    std::string describe() const
    {
        std::string s;
        for (std::size_t i = 0; i < params_.size(); ++i)
        {
            if (i > 0) s += "\n";
            s += describeLine (params_[i]);
        }
        return s;
    }

private:
    std::vector<ParamSpec> params_;
};

} // namespace echojay
