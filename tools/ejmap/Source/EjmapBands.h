/*
  EjmapBands.h

  M5 model: band inference, stride verification, negative evidence, group
  assembly. Shaped by four findings measured this week, each stated where it
  binds:

  - NAME PATTERNS ARE HYPOTHESES. Inference varies EXACTLY ONE token of a
    captured member's name -- a digit run (Band 1 Freq -> Band 2 Freq) or a
    known ordered prefix (LF -> LMF -> MF -> HMF -> HF) -- holding every other
    token literal. Holding the rest literal is what keeps AMEK's TMT channel
    suffix (LF Freq 1 vs LF Freq 2) from conflating channel banks into
    phantom bands. Nothing inferred enters a map unverified: every member is
    swept, and the stride can flag but never generate.

  - THE STRIDE IS A VERIFIER, NEVER A GENERALISER (85.1% across 571 maps,
    failing on EQuality and REQ 4). It is derived from the two indices the
    human actually captured, at whatever band positions those turn out to be
    -- never assumed to bracket the set. If the two captured bands are
    ADJACENT, the stride is flagged unverified across the range (amendment 3):
    a stride from positions 1 and 2 says nothing about position 7.

  - co_moved SEEDS, NEVER DEFINES (Pro-Q 3 drag co-moves Used/Freq/Gain but
    not Q). Co-moved indices from the band captures are offered as candidate
    members needing verification; their absence excludes nothing.

  - THE NEGATIVE DISCRIMINATOR is per-arm only: "these indices were watched,
    this gesture moved three, Mono Maker was not among them". Arms are never
    accumulated into a "never moves" claim; each arm's watched set stands
    alone, and the exclusion list it feeds is EVIDENCE for group membership,
    not a mechanism. The mechanism remains membership itself.
*/

#pragma once

#include <juce_core/juce_core.h>

#include "EjmapSchema.h"
#include "EjmapSweeper.h"

namespace ejmap
{

struct BandInference
{
    //==========================================================================
    struct Member
    {
        juce::String semantic;         // freq_hz | gain_db | q
        int index = -1;
        juce::String name;
        bool captured = false;         // human touched it
        SweepOutcome sweep;
    };

    struct Band
    {
        int ordinal = 0;               // 1-based band number, the GroupSpec n
        juce::String label;            // "1" or "LF"
        juce::Array<Member> members;
        bool strideAgrees   = false;
        bool strideUnverified = false; // captured pair adjacent: says nothing here
        juce::String flag;             // non-empty = needs a wiggle, with the why

        const Member* member (const juce::String& sem) const
        {
            for (auto& m : members) if (m.semantic == sem) return &m;
            return nullptr;
        }
    };

    /** One per-arm negative record, exactly as scoped: valid within its own
        arm, never accumulated.
    */
    struct ArmRecord
    {
        int watched = 0;               // param_count minus noise mask, that arm
        juce::Array<int> moved;
    };

    juce::String axis;                 // "digit" | "prefix"
    juce::String family;               // "" for plain bands; "sband"/"vband" class
    juce::Array<Band> bands;
    juce::Array<ArmRecord> arms;
    juce::StringArray otherFamilies;   // disjoint band-shaped families not touched
    juce::String strideNote;           // human-readable stride verdict

    //==========================================================================
    static juce::StringArray tokenize (const juce::String& name)
    {
        juce::StringArray t;
        t.addTokens (name, " ", "");
        t.removeEmptyStrings();
        return t;
    }

    /** The ordered prefix axis. Order IS the claim (LF below LMF below MF...):
        it decides band ordinals and what "the highest band" means.
    */
    static juce::StringArray prefixOrder()
    {
        return { "LF", "LMF", "MF", "HMF", "HF" };
    }

    /** Splits a token into base + trailing digits ("sband2" -> sband, 2).
        digits == -1 when none.
    */
    static void splitDigits (const juce::String& tok, juce::String& base, int& digits)
    {
        int i = tok.length();
        while (i > 0 && juce::CharacterFunctions::isDigit (tok[i - 1])) --i;
        base   = tok.substring (0, i);
        digits = i < tok.length() ? tok.substring (i).getIntValue() : -1;
    }

    /** Finds the morph slot in a captured member's name: the token position
        carrying a digit run, or a token on the prefix axis. Returns the slot
        index, or -1 with a reason.
    */
    static int morphSlot (const juce::StringArray& toks, juce::String& axisOut,
                          juce::String& valueOut)
    {
        // Digit axis first: a digit token or digit-suffixed token.
        for (int i = 0; i < toks.size(); ++i)
        {
            juce::String base; int d = -1;
            splitDigits (toks[i], base, d);
            if (d >= 0)
            {
                // AMEK's trailing channel digit is ALSO a digit token; prefer
                // a digit that is not the LAST token when a prefix-axis token
                // exists, since "LF Freq 1" morphs on LF, not on the channel.
                bool prefixElsewhere = false;
                for (int j = 0; j < toks.size(); ++j)
                    if (j != i && prefixOrder().contains (toks[j], true))
                        prefixElsewhere = true;
                if (prefixElsewhere) continue;
                axisOut = "digit"; valueOut = juce::String (d);
                return i;
            }
        }
        for (int i = 0; i < toks.size(); ++i)
            if (prefixOrder().contains (toks[i], true))
            { axisOut = "prefix"; valueOut = toks[i]; return i; }
        return -1;
    }

    static juce::String withSlot (juce::StringArray toks, int slot,
                                  const juce::String& replacement)
    {
        // Preserve a digit-suffix shape: "sband1" -> "sband2".
        juce::String base; int d = -1;
        splitDigits (toks[slot], base, d);
        if (d >= 0 && base.isNotEmpty() && ! prefixOrder().contains (toks[slot], true))
            toks.set (slot, base + replacement);
        else
            toks.set (slot, replacement);
        return toks.joinIntoString (" ");
    }

    static int indexOfName (const juce::StringArray& names, const juce::String& want)
    {
        for (int i = 0; i < names.size(); ++i)
            if (names[i].equalsIgnoreCase (want)) return i;
        return -1;
    }

    //==========================================================================
    /** Infers the band table from the captured band-1 members plus the second
        captured freq (whatever band it landed on), against the instance's
        parameter names.
    */
    static BandInference infer (const juce::Array<Member>& capturedBand1,
                                int secondFreqIndex,
                                const juce::StringArray& paramNames)
    {
        BandInference out;
        const Member* freq1 = nullptr;
        for (auto& m : capturedBand1) if (m.semantic == "freq_hz") freq1 = &m;
        if (freq1 == nullptr) { out.strideNote = "no captured freq"; return out; }

        auto toks = tokenize (freq1->name);
        juce::String axis, value;
        const int slot = morphSlot (toks, axis, value);
        if (slot < 0)
        {
            out.strideNote = "no digit or band-prefix token in '" + freq1->name
                           + "': cannot infer siblings, map bands by hand";
            return out;
        }
        out.axis = axis;

        // Family label: the digit token's base when it is a word of its own
        // ("sband2" -> sband). A bare digit or a prefix token has no family.
        if (axis == "digit")
        {
            juce::String base; int d = -1;
            splitDigits (toks[slot], base, d);
            if (base.isNotEmpty() && ! base.equalsIgnoreCase ("band"))
                out.family = base.toLowerCase();
        }

        // Enumerate the band value sequence.
        juce::StringArray seq;
        if (axis == "prefix")
            seq = prefixOrder();
        else
            for (int k = 1; k <= 64; ++k)
            {
                if (indexOfName (paramNames, withSlot (toks, slot, juce::String (k))) < 0)
                    break;
                seq.add (juce::String (k));
            }

        // Build bands: morph EVERY captured member's name per band value.
        for (int b = 0; b < seq.size(); ++b)
        {
            Band band;
            band.ordinal = b + 1;
            band.label = seq[b];
            bool anyMember = false;
            for (auto& cm : capturedBand1)
            {
                auto mtoks = tokenize (cm.name);
                juce::String ax2, v2;
                const int ms = morphSlot (mtoks, ax2, v2);
                if (ms < 0) continue;
                const auto wanted = withSlot (mtoks, ms, seq[b]);
                const int idx = indexOfName (paramNames, wanted);
                if (idx < 0) continue;
                Member m;
                m.semantic = cm.semantic; m.index = idx; m.name = paramNames[idx];
                m.captured = (idx == cm.index);
                band.members.add (m);
                anyMember = true;
            }
            if (anyMember) out.bands.add (band);
        }

        // Mark the second captured freq.
        int secondPos = -1;
        for (int b = 0; b < out.bands.size(); ++b)
            for (auto& m : out.bands.getReference (b).members)
                if (m.index == secondFreqIndex && m.semantic == "freq_hz")
                { m.captured = true; secondPos = b; }

        // Stride: derived from the two CAPTURED freq indices at the positions
        // they actually occupy, never assumed to bracket the set.
        int firstPos = -1;
        for (int b = 0; b < out.bands.size(); ++b)
            if (auto* m = out.bands.getReference (b).member ("freq_hz"))
                if (m->index == freq1->index) firstPos = b;

        if (firstPos < 0 || secondPos < 0 || firstPos == secondPos)
        {
            out.strideNote = "stride unavailable: need two captured bands";
            for (auto& b : out.bands) { b.strideUnverified = true; }
        }
        else
        {
            const int span = secondPos - firstPos;
            const int di   = secondFreqIndex - freq1->index;
            const bool integral = span != 0 && di % span == 0;
            const int stride = integral ? di / span : 0;
            const bool adjacent = std::abs (span) == 1;

            for (int pos = 0; pos < out.bands.size(); ++pos)
            {
                auto& b = out.bands.getReference (pos);
                auto* fm = b.member ("freq_hz");
                if (fm == nullptr) { b.flag = "no freq member"; continue; }
                if (fm->captured) { b.strideAgrees = true; continue; }
                if (! integral)
                { b.strideUnverified = true; continue; }
                const int expect = freq1->index + (pos - firstPos) * stride;
                if (adjacent)
                    b.strideUnverified = true;   // two neighbours prove nothing afar
                else if (fm->index == expect)
                    b.strideAgrees = true;
                else
                    b.flag = "stride disagrees: name says [" + juce::String (fm->index)
                           + "], stride says [" + juce::String (expect) + "] - wiggle it";
            }
            out.strideNote = ! integral
                ? "captured indices give a non-integral stride: names only, every band unverified"
                : adjacent
                ? "captured bands are ADJACENT: stride " + juce::String (stride)
                    + " is unverified across the range (amendment 3) - consider wiggling a far band"
                : "stride " + juce::String (stride) + " from captured bands "
                    + juce::String (firstPos + 1) + " and " + juce::String (secondPos + 1)
                    + ", verifying the rest";
        }

        // Other band-shaped families: a DISJOINT digit-base among param names
        // with at least freq-and-gain-looking members (E2Deesser's second
        // family). Reported for the family prompt, never auto-mapped.
        if (out.family.isNotEmpty())
        {
            juce::StringArray seen;
            for (const auto& n : paramNames)
                for (const auto& tok : tokenize (n))
                {
                    juce::String base; int d = -1;
                    splitDigits (tok, base, d);
                    if (d >= 1 && base.length() >= 2 && ! base.equalsIgnoreCase ("band")
                         && ! base.equalsIgnoreCase (out.family)
                         && ! seen.contains (base.toLowerCase()))
                        seen.add (base.toLowerCase());
                }
            for (const auto& f : seen)
            {
                int hits = 0;
                for (const auto& n : paramNames)
                    if (n.toLowerCase().contains (f)) ++hits;
                if (hits >= 4)                        // freq+gain over >=2 bands
                    out.otherFamilies.add (f);
            }
        }
        return out;
    }

    //==========================================================================
    /** The exclusion list: band-LOOK-ALIKE parameters outside the group that
        were watched and unmoved during the band arms. The AMEK evidence line.
        Look-alike is name-based BY DESIGN here -- it selects which exclusions
        are worth SHOWING, not what is excluded (membership does that), so a
        false match costs a line of text, never a wrong map.
    */
    juce::StringArray exclusionList (const juce::StringArray& paramNames) const
    {
        juce::SortedSet<int> memberSet;
        for (const auto& b : bands)
            for (const auto& m : b.members)
                memberSet.add (m.index);

        juce::SortedSet<int> movedEver;
        for (const auto& a : arms)
            for (int i : a.moved)
                movedEver.add (i);

        juce::StringArray out;
        for (int i = 0; i < paramNames.size(); ++i)
        {
            if (memberSet.contains (i) || movedEver.contains (i)) continue;
            const auto n = paramNames[i].toLowerCase();
            if (n.contains ("freq") || n.contains ("gain") || n.contains ("maker")
                 || n.contains ("hz") || n == "q" || n.endsWith (" q"))
                out.add ("[" + juce::String (i) + "] " + paramNames[i]);
        }
        return out;
    }

    /** GroupSpec assembly, against the consumer contract pinned in the drift
        gate: one group per band, n = band number, freq_range from that band's
        own freq sweep, primary on the touched family.
    */
    juce::Array<GroupSpec> toGroups() const
    {
        juce::Array<GroupSpec> out;
        for (const auto& b : bands)
        {
            if (b.flag.isNotEmpty()) continue;     // flagged bands never ship silently
            GroupSpec g;
            g.family  = family;
            g.n       = b.ordinal;
            g.primary = (b.ordinal == 1);
            for (const auto& m : b.members)
            {
                if (! m.sweep.ok) continue;
                ParamMapping pm;
                pm.semantic  = m.semantic;
                pm.kind      = m.semantic;
                pm.paramName = m.name;
                pm.indices.add (m.index);
                for (const auto& a : m.sweep.anchors)
                    pm.anchors.add ({ (double) a[1], (double) a[0] });
                pm.anchorsReversed = m.sweep.anchorsReversed;
                pm.trust  = m.captured ? Trust::humanVerified : Trust::setread;
                pm.method = m.sweep.method == "setread" ? AnchorMethod::setread
                                                        : AnchorMethod::gettext;
                if (m.semantic == "freq_hz" && ! m.sweep.anchors.isEmpty())
                {
                    float lo = m.sweep.anchors.getFirst()[0], hi = lo;
                    for (const auto& a : m.sweep.anchors)
                    { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
                    g.freqLo = lo; g.freqHi = hi;
                }
                g.params.add (pm);
            }
            if (! g.params.isEmpty())
                out.add (g);
        }
        return out;
    }
};

} // namespace ejmap
