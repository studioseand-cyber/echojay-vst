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

    // Canonical form used for matching: lowercased, separators dropped, so
    // "attack_ms", "Attack-MS" and "attackMs" are the same parameter.
    //
    // Tolerance here is not sloppiness — it is the difference between a move
    // that lands and one that silently does nothing. An id that misses is a
    // knob that never turns, with no error anywhere; the cost of accepting a
    // near-miss is zero, and the cost of rejecting it is a broken feature.
    static std::string normalizeId (const std::string& raw)
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
    static std::string describeLine (const ParamSpec& p)
    {
        std::string s = p.id + " (";
        if (p.boolean)
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
