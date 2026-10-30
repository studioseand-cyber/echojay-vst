#pragma once

// =============================================================================
//  ROUND 7b's PINS: THE PAYLOAD CARRIES THE BAND ID (ku).
// =============================================================================
//
// Sean's ruling of 29 Oct: the match_report payload's bands[].name is B1's id
// ("lowMid"), not a label. The card shows a label, and it comes from the one
// table (EJMatchProposal.h) through matchReportPayloadBandLabel, which is what
// A's paint branch reads.
//
// ku PIN1   bands[].name is B1's six ids, in band order;
// ku PIN2   the label for a payload band is the table's; a name the table does
//           not hold (C2's old "low-mid", another case, none at all) gives "",
//           never the name itself;
// ku PIN3   the label the paint branch reads back from the payload is the label
//           on the report's own line for that band.
// =============================================================================

#include "EJMatchReport.h"

namespace k7bpins
{
using namespace echojay;

template <typename Check>
inline void runK7bPins (Check&& check)
{
    MatchReportIn in;
    in.referenceName = "Song X";
    in.mix.integrated = -11.8f; in.ref.integrated = -9.7f;
    for (int b = 0; b < 6; ++b)
    {
        MatchReportBandIn bi;
        bi.mixMinusRefDb = (b % 2 == 0 ? 3.0f : -3.0f);
        in.bands[(size_t) b] = bi;
    }
    const auto rep = buildMatchReport (in);
    const juce::var payload = juce::JSON::parse (matchReportPayload (in, rep));
    const juce::var bandsVar = payload.getProperty ("bands", juce::var());
    const auto* bands = bandsVar.getArray();

    // ku PIN1 -- THE IDS, IN ORDER.
    {
        const char* ids[6] = { "sub", "low", "lowMid", "mid", "highMid", "air" };
        bool ok = bands != nullptr && bands->size() == 6;
        juce::StringArray got;
        for (int b = 0; ok && b < 6; ++b)
        {
            const juce::var band = bands->getReference (b);
            got.add (band.getProperty ("name", juce::var()).toString());
            ok = got[b] == ids[b];
        }
        check (ok, "ku PIN1: the match_report payload's bands[].name is B1's id, the six in band order "
                   "(\"lowMid\", not \"Low-mids\")",
               got.joinIntoString (", "));
    }

    // ku PIN2 -- THE LABEL FROM THE TABLE, AND NOTHING FOR A NAME IT DOES NOT HOLD.
    {
        bool ok = bands != nullptr && bands->size() == 6;
        juce::StringArray got;
        for (int b = 0; ok && b < 6; ++b)
        {
            const juce::var band = bands->getReference (b);
            got.add (matchReportPayloadBandLabel (band));
            ok = got[b] == matchBandLabel (b) && got[b].isNotEmpty();
        }
        auto labelFor = [] (const juce::var& name)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            if (! name.isVoid()) o->setProperty ("name", name);
            return matchReportPayloadBandLabel (juce::var (o.get()));
        };
        const bool strangers = labelFor ("low-mid").isEmpty() && labelFor ("LowMid").isEmpty()
                            && labelFor ("Low-mids").isEmpty() && labelFor ("").isEmpty()
                            && labelFor (juce::var()).isEmpty() && labelFor (2).isEmpty()
                            && matchReportPayloadBandLabel (juce::var()).isEmpty()
                            && matchReportPayloadBandLabel (juce::var ("lowMid")).isEmpty();
        check (ok && strangers,
               "ku PIN2: the card's label for a payload band comes from the one table (\"lowMid\" reads "
               "\"Low-mids\"), and a name the table does not hold reads as nothing, never as itself",
               got.joinIntoString (", ") + (strangers ? "" : "; a stranger got a label"));
    }

    // ku PIN3 -- THE PAINT BRANCH AND THE REPORT AGREE.
    {
        int agreed = 0, lines = 0;
        juce::StringArray bad;
        for (const auto& l : rep.lines)
        {
            if (l.band < 0) continue;
            ++lines;
            const auto fromPayload = bands != nullptr && l.band < bands->size()
                                   ? matchReportPayloadBandLabel (bands->getReference (l.band)) : juce::String();
            if (fromPayload == l.label) ++agreed;
            else bad.add (l.label + " / " + fromPayload);
        }
        check (lines == 6 && agreed == lines,
               "ku PIN3: the label read back from the payload is the label on the report's own line for that band",
               juce::String (agreed) + " of " + juce::String (lines) + "; " + bad.joinIntoString (" | "));
    }
}

} // namespace k7bpins
