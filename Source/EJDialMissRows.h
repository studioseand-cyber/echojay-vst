#pragma once

#include <JuceHeader.h>
#include "ChainHost.h"     // ChainHost::SlotDialInfo / DialStatus — the REAL types
#include <vector>

// ============================================================================
// A9 step 1: THE dial-miss row set, authored once.
//
// Three walkers used to compose this row set independently —
// finishChainBubbleWhenDialSettled (build, clean-load),
// finishEditBubbleWhenDialSettled (edit) and logDialMissesWhenSettled (build,
// dirty-load). They are per-turn ALTERNATIVES at their call sites, so a given
// slot-turn is reported by exactly one of them — which is precisely why they
// were free to drift, and had:
//
//   - walker 3 emitted NEITHER stale_display_kept NOR out_of_range at all, and
//     reported a stale-ladder noMap as plain "no_map" rather than
//     "stale_unmapped";
//   - walkers 1 and 2 DROPPED the readback_mismatch row whenever outOfRange
//     was non-empty, because the unusableMap branch broke early.
//
// So identical slot state produced three different row populations depending
// on which turn type ran, and every rate cut by turn_type inherited that as
// bias. This function is now the only author: adding a caller cannot omit a
// reason, and adding a reason reaches every caller.
//
// PURE and header-inline on purpose: tools/mapfps_test compiles this directly
// (so the pins exercise the SHIPPED derivation, never a copy of it, and no new
// lib symbol is paired with the previous build's archive — the stale-lib trap,
// third sighting). It takes the real ChainHost::SlotDialInfo, so the pins
// cannot drift from the struct the emitter actually reads.
//
// NOT in scope here (A9 steps 2 and 3, separate commits): the reason
// partition, and splitting unusable_map into its "map covered nothing
// requested" and "map covered it, nothing landed" cases. This step changes
// WHO emits and fixes the outOfRange coupling; it does not renumber the
// reasons.
// ============================================================================
namespace echojay
{

struct DialMissRow
{
    juce::String      reason;   // the A1 reason code
    juce::StringArray names;    // the declined labels this row carries ("manual")
};

/** The complete, ordered row set for one slot's SETTLED dial state.

    Order is the emit order and is deliberate: the two status-independent
    findings first (they are true whatever the verdict), then the slot's
    verdict, then the readback finding. Nothing here reads anything but `di`,
    so two callers handed the same SlotDialInfo cannot produce different rows.
*/
inline std::vector<DialMissRow> dialMissRowsFor (const ChainHost::SlotDialInfo& di)
{
    std::vector<DialMissRow> rows;

    // Recorded whatever the slot status: the write was kept on norm proof and
    // the display disagreement must not vanish into the applied count.
    if (! di.unconfirmed.isEmpty())
        rows.push_back ({ "stale_display_kept", di.unconfirmed });

    // Range refusals are recorded whatever the slot status: the bubble must
    // never say "use the values on its card" about a value the card says will
    // not map onto this version.
    if (! di.outOfRange.isEmpty())
        rows.push_back ({ "out_of_range", di.outOfRange });

    // The slot's verdict. No `default:` on purpose — a new DialStatus must
    // fail to compile here rather than silently emit nothing.
    switch (di.status)
    {
        case ChainHost::DialStatus::partial:
            rows.push_back ({ "partial", di.manual });
            break;

        case ChainHost::DialStatus::noMap:
            // Stale-map ladder, unmapped rung: the plugin loaded at a version
            // the corpus has no mapping for. A different fact from "no map
            // exists", and only this shape earns the alternatives pill.
            rows.push_back ({ di.staleIndexedFp.isNotEmpty() ? "stale_unmapped" : "no_map",
                              di.manual });
            break;

        case ChainHost::DialStatus::unusableMap:
            rows.push_back ({ "unusable_map", di.manual });
            break;

        case ChainHost::DialStatus::pending:
            // Fetch never answered inside the cap.
            rows.push_back ({ "map_fetch_timeout", di.manual });
            break;

        case ChainHost::DialStatus::applied:
        case ChainHost::DialStatus::none:
            break;
    }

    // THE FIX (A9 step 1). This row used to sit INSIDE the unusableMap branch,
    // after an `if (!outOfRange.isEmpty()) { ...; break; }` — so a slot that
    // had both a range refusal and a readback mismatch reported only the range
    // refusal, and the readback row's existence depended on an unrelated field
    // being empty. They are different findings about different controls; one
    // must never suppress the other.
    //
    // The status test that remains is NOT the old coupling: readbackMiss is
    // filled only in the not-applied branch of the apply loop, and it is
    // cleared per apply (ChainHost.cpp, the dialReadbackMiss.clear() before
    // the report loop). A slot re-marked noMap/pending by a LATER sweep never
    // re-enters that loop, so it can still be carrying the previous apply's
    // labels — emitting them under a no_map verdict would be a stale row
    // asserting a readback that this turn never performed. Restricting to the
    // two verdicts that mean "the apply ran and something did not land" is the
    // presence condition for the evidence, not a dependence on another finding.
    if (! di.readbackMiss.isEmpty()
        && (di.status == ChainHost::DialStatus::partial
            || di.status == ChainHost::DialStatus::unusableMap))
        rows.push_back ({ "readback_mismatch", di.readbackMiss });

    return rows;
}

} // namespace echojay
