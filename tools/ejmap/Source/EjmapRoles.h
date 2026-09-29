/*
  EjmapRoles.h

  WHICH CONTROL IS THE THRESHOLD. Spec section 3's roles (threshold, ratio, attack, release,
  input, output, makeup, mix; key, reference, strength on tuners), decided from a control's
  recorded NAME with the server's own matcher (EjmapNameTokens.h). Section 4 sweeps the
  threshold, and until this existed none of the 1,783 fixture controls had a role, so
  nothing could tell the sweep which control to turn.

  THE RULE, per control, in this order:
    1. VETO. A name answering a sidechain, meter, filter or preset term gets NO role, and
       the reason names every family that vetoed it ("veto:sidechain+filter"). Sidechain
       does not veto on a tuner, where "Key" is the musical key.
    2. ROLE. The roles whose terms the name answers. More than one REFUSES: no role, and
       the reason names each ("ambiguous:attack|release" - CL 1B "Select Attack Release"
       is a mode selector, not either). This is the shipped resolver's rule (EJNameLadder.h,
       6a4550b: a name that means several things is refused, and says which). Its
       functions tiedAtBestRank / tieSpansProducts are NOT called here: they take a pool of
       juce::PluginDescription, rank by channel-variant suffix and compare
       ChainHost::stripParenthetical bases, none of which a control name has, and neither
       exists on this branch. Every lexicon hit ranks equally, so a role tie is any tie.
    0. READOUT. A control the probe measured as a readout gets no role (reason "readout"),
       before any name is read.
    3. FLAG "dc". A name answering "dc" keeps its role and is flagged. "L DC Thr" on a
       Fairchild may be DC bias, but that is inferred from the name and unverified, and a
       veto would silently drop a threshold candidate; the sweep's curve decides.

  THE LEXICON carries section 3's names plus what the 74 fixtures showed they need: UAD's
  abbreviations (Thr, Rat, Rcv, Att; Neve 2254's Rcvr) and LA-2A's truncated "Peak Reduct". Tuner roles
  are scoped to tuners: in a compressor "Key" is the sidechain key, and unscoped they gave
  20 compressor controls tuner roles.

  THE PRODUCT CLASS, from how many controls hold the threshold role (spec section 4.7):
    single_threshold       exactly one: the sweep proceeds on it
    comp_over_expander     several, and exactly one does not answer exp/expander/strap:
                           that one keeps the role (flagged), the others lose it
    surround               several, one named with an "lfe" word
    channels_lr            several, every one L- or R-ended
    bands_or_stages        several, otherwise
    input_as_threshold     none, but an input control: it TAKES the threshold role (flagged).
                           A 1176's Input is its threshold. This is why the role is stored
                           per control and not derived from the semantic (EjmapRoleSemantics.h)
    amount_only            none and no input, but a density/amount/drive-style control
    none

  PINNED to tools/ejmap/tests/fixtures/role-classification-74.json, produced by a reference
  classifier running the server's own matcher in node: every product's class and every
  control's role, flags and reason. A lexicon change that moves a product between classes
  fails RoundTripTest. 29 Sep: 38 / 9 / 4 / 4 / 3 / 14 / 2 in the order
  single / input_as_threshold / comp_over_expander / amount_only / channels_lr /
  bands_or_stages / surround.
*/

#pragma once

#include "EjmapNameTokens.h"

namespace ejmap::roles
{

enum class Category { compressor, tuner };

using TermTable = std::vector<std::pair<juce::String, juce::StringArray>>;

inline const TermTable& compressorLexicon()
{
    static const TermTable t {
        { "threshold", { "threshold", "thresh", "thr", "peak reduction", "peak reduct" } },
        { "ratio",     { "ratio", "rat" } },
        { "attack",    { "attack", "atk", "att" } },
        { "release",   { "release", "recovery", "recover", "rel", "rec", "rcvr", "rcv" } },
        { "input",     { "input", "input gain", "in gain" } },
        { "output",    { "output", "out", "output gain", "out gain" } },
        { "makeup",    { "makeup", "make up" } },
        { "mix",       { "mix", "dry wet", "wet", "blend", "parallel mix" } },
    };
    return t;
}

inline const TermTable& tunerLexicon()
{
    static const TermTable t {
        { "key",       { "key", "scale" } },
        { "reference", { "reference", "ref" } },
        { "strength",  { "strength", "retune", "speed", "amount" } },
    };
    return t;
}

inline const TermTable& vetoes()
{
    static const TermTable t {
        { "sidechain", { "sc", "sidechain", "side chain", "key", "ext" } },
        { "meter",     { "meter", "vu", "gr" } },
        { "filter",    { "filter", "hp", "lp", "hpf", "lpf", "freq", "frequency" } },
        { "preset",    { "preset", "trigger" } },
    };
    return t;
}

inline const juce::StringArray& expanderTerms()  { static const juce::StringArray t { "exp", "expander", "strap" }; return t; }
inline const juce::StringArray& amountTerms()
{
    static const juce::StringArray t { "density", "reduction", "amount", "compress", "compression", "drive", "tension" };
    return t;
}

inline bool answersAny (const juce::String& name, const juce::StringArray& terms)
{
    for (const auto& t : terms)
        if (nametokens::controlAnswersTerm (name, t)) return true;
    return false;
}

struct ControlRole
{
    int index = -1;
    juce::String name;
    juce::String role;          // empty = no role
    juce::StringArray flags;    // "dc", "input_as_threshold", "comp_over_expander"
    juce::String reason;        // why no role: "veto:...", "ambiguous:...", "expander_or_strap_threshold"
};

struct Classification
{
    juce::String cls;
    std::vector<ControlRole> controls;
};

// Steps 1-3 for one name.
inline ControlRole roleOfName (const juce::String& name, Category category)
{
    ControlRole r;
    r.name = name;

    juce::StringArray vetoedBy;
    for (const auto& [family, terms] : vetoes())
    {
        if (category == Category::tuner && family == "sidechain") continue;
        if (answersAny (name, terms)) vetoedBy.add (family);
    }
    if (! vetoedBy.isEmpty())
    {
        r.reason = "veto:" + vetoedBy.joinIntoString ("+");
        return r;
    }

    juce::StringArray roles;
    for (const auto& [role, terms] : compressorLexicon())
        if (answersAny (name, terms)) roles.add (role);
    if (category == Category::tuner)
        for (const auto& [role, terms] : tunerLexicon())
            if (answersAny (name, terms)) roles.add (role);
    if (roles.size() > 1)
    {
        r.reason = "ambiguous:" + roles.joinIntoString ("|");
        return r;
    }

    if (nametokens::controlAnswersTerm (name, "dc")) r.flags.add ("dc");
    if (roles.size() == 1) r.role = roles[0];
    return r;
}

// JavaScript /^(l|r)\b/i || /\b(l|r)$/i: an L or R standing alone at either end.
inline bool isChannelEnded (const juce::String& name)
{
    using nametokens::isJsWordChar;
    const auto c = nametokens::toChars (name);
    if (c.empty()) return false;
    auto isLR = [] (juce::juce_wchar ch) { return ch == 'l' || ch == 'L' || ch == 'r' || ch == 'R'; };
    const bool atStart = isLR (c.front()) && (c.size() == 1 || ! isJsWordChar (c[1]));
    const bool atEnd   = isLR (c.back())  && (c.size() == 1 || ! isJsWordChar (c[c.size() - 2]));
    return atStart || atEnd;
}

// readout: the control is a meter by BEHAVIOUR (item 12, EjmapFixtureReadout.h). A readout
// never gets a role, whatever its name says - a roled meter would be swept, and the sweep
// would measure the meter wandering (EJMAP_CERT_DRIVER.md section 7).
struct NamedControl { int index; juce::String name; bool readout = false; };

// The whole rule: per-control roles, the product class, and the two class-level
// reassignments (input_as_threshold, comp_over_expander) applied to the roles.
inline Classification classify (const std::vector<NamedControl>& controls, Category category)
{
    Classification out;
    for (const auto& c : controls)
    {
        ControlRole r;
        if (c.readout) { r.name = c.name; r.reason = "readout"; }
        else           r = roleOfName (c.name, category);
        r.index = c.index;
        out.controls.push_back (r);
    }

    std::vector<ControlRole*> thr;
    for (auto& r : out.controls)
        if (r.role == "threshold") thr.push_back (&r);

    if (thr.size() == 1)
    {
        out.cls = "single_threshold";
    }
    else if (thr.size() > 1)
    {
        int expanders = 0;
        for (auto* t : thr) if (answersAny (t->name, expanderTerms())) ++expanders;

        juce::StringArray lowered;
        for (auto* t : thr) lowered.add (t->name);
        const auto joined = nametokens::jsLower (nametokens::toChars (lowered.joinIntoString (" ")));

        bool allChannels = true;
        for (auto* t : thr) allChannels = allChannels && isChannelEnded (t->name);

        if (expanders > 0 && (int) thr.size() - expanders == 1)
        {
            out.cls = "comp_over_expander";
            for (auto* t : thr)
            {
                if (answersAny (t->name, expanderTerms())) { t->role.clear(); t->reason = "expander_or_strap_threshold"; }
                else t->flags.add ("comp_over_expander");
            }
        }
        else if (nametokens::jsWordBoundedContains (joined, "lfe")) out.cls = "surround";
        else if (allChannels)                                        out.cls = "channels_lr";
        else                                                         out.cls = "bands_or_stages";
    }
    else
    {
        bool hasInput = false;
        for (const auto& r : out.controls) hasInput = hasInput || r.role == "input";
        if (hasInput)
        {
            out.cls = "input_as_threshold";
            for (auto& r : out.controls)
                if (r.role == "input") { r.role = "threshold"; r.flags.add ("input_as_threshold"); }
        }
        else
        {
            bool amount = false;
            for (const auto& c : controls) amount = amount || answersAny (c.name, amountTerms());
            out.cls = amount ? "amount_only" : "none";
        }
    }
    return out;
}

} // namespace ejmap::roles
