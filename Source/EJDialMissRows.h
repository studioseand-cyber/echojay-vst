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
    juce::String      reason;       // the A1 reason code
    juce::StringArray names;        // the declined labels this row carries ("manual")
    juce::StringArray alsoReasons;  // A9 §2; EMPTY means no key on the wire at all
};

// ----------------------------------------------------------------------------
// THERE IS NO RANK TABLE HERE, AND THAT IS THE DESIGN.
//
// A9 §2 states a rank order — a cause outranks a verdict, a failure outranks a
// success — and if you came looking for it as a `dialReasonRank` lookup, it was
// written, wired, measured and deleted on 23 Aug. See the contract, §2 "No code
// consults a rank table, and none should".
//
// The precedence §2 describes is "which reason CLAIMS a control's row". It is
// implemented below by the `claimed` set and the `isDecline` flag. Every pair a
// rank could decide is already settled by one of them, or deliberately not
// decided at all:
//
//   readback_mismatch vs out_of_range  nobody decides; BOTH rows are kept,
//                                      because one label in two cause arrays is
//                                      two controls and both must survive
//   causes vs verdicts                 the `claimed` set
//   verdicts against each other        nothing — status is one value, so
//                                      partial/no_map/unusable_map/
//                                      map_fetch_timeout/stale_unmapped never
//                                      compete
//   stale_display_kept vs anything     `isDecline == false`, so it never claims
//                                      and therefore never competes
//
// A sort by rank binds the table to ROW ORDER IN THE VECTOR, which is a
// different question from which reason claims a control. Measured: wiring it
// cost the "A9: stale_display_kept is in the shared set" pin (136 of 137) and
// STILL left a renumber green, because no fixture carries both a readbackMiss
// and an unconfirmed label, so those two reasons' relative order is
// unobservable. A table that looks authoritative and governs nothing is the
// shape this project keeps catching.
//
// THE GUARANTEE IS PINNED. Three mutations in tools/mapfps_test/mapfps_test.cpp
// guard it; if you change `claimed` or `isDecline`, these are what should go red:
//
//   S4  drop the `claimed` guard on the verdict rows (the overcount returns)
//       -> 7 red, including "A9.2 PIN1 Solid EQ: exactly one row for the one
//          control", "A9.2 PIN2 partial: manual + readbackMiss yields one row",
//          "A9: two controls, two rows, neither suppressing the other"
//   S5  stop the verdict riding as also_reasons
//       -> "A9.2 PIN1 Solid EQ: one row, readback_mismatch, also unusable_map",
//          "A9.2 PIN2 partial: manual + readbackMiss yields one row"
//   S6  key the partition on the LABEL alone (cause rows skip a claimed label)
//       -> "A9.2 PIN3 two causes on one label yield TWO rows",
//          "A9.2 PIN3 both causes survive under one name"
//
// If you are here for step 3: splitting unusable_map into map_no_coverage and
// writes_rejected is a change to `verdict` below and to §2's table in the
// contract. It needs no rank lookup either.
// ----------------------------------------------------------------------------

/** The complete, ordered row set for one slot's SETTLED dial state.

    Order is the emit order and is deliberate: readback_mismatch, then
    out_of_range, then stale_display_kept, then the verdict rows for whatever
    `manual` names no cause has claimed. The causes come first BECAUSE they
    claim, and a verdict row exists only for what is left over; the order and
    the partition are therefore the same mechanism, not two.

    (The step-1 docstring here said "the two status-independent findings first,
    then the slot's verdict, then the readback finding". That was step 1's
    order and went stale the moment the partition landed.)

    It is NOT a sort by rank and must not become one — see the note above.
    Nothing here reads anything but `di`, so two callers handed the same
    SlotDialInfo cannot produce different rows.
*/
inline std::vector<DialMissRow> dialMissRowsFor (const ChainHost::SlotDialInfo& di)
{
    // The slot's verdict — one value, so ranks 3-8 never compete.
    // No `default:` on purpose: a new DialStatus must fail to compile here
    // rather than silently emit nothing.
    juce::String verdict;
    switch (di.status)
    {
        case ChainHost::DialStatus::partial:     verdict = "partial";           break;
        // Stale-map ladder, unmapped rung: the plugin loaded at a version the
        // corpus has no mapping for. A different fact from "no map exists",
        // and only that shape earns the alternatives pill.
        case ChainHost::DialStatus::noMap:
            verdict = di.staleIndexedFp.isNotEmpty() ? "stale_unmapped" : "no_map";
            break;
        case ChainHost::DialStatus::unusableMap: verdict = "unusable_map";      break;
        case ChainHost::DialStatus::pending:     verdict = "map_fetch_timeout"; break;
        case ChainHost::DialStatus::applied:
        case ChainHost::DialStatus::none:                                       break;
    }

    // §4a, carried from step 1: readback_mismatch is emitted for `partial` and
    // `unusableMap` only. dialReadbackMiss is cleared solely inside the apply
    // loop, so a slot re-marked noMap/pending by a later sweep can still be
    // carrying the PREVIOUS apply's labels; a row there would assert a
    // readback this turn never performed. That is a presence-of-evidence
    // condition, NOT the outOfRange coupling A9 removed. Do not ungate it.
    const bool readbackAllowed = (di.status == ChainHost::DialStatus::partial
                               || di.status == ChainHost::DialStatus::unusableMap);

    // ---- 1. One row per (label, cause), read straight off the three arrays --
    // The partition key is (label, cause), and no per-control carrier is
    // needed to compute it. A carrier keyed on the parameter index was built
    // and then stripped, because the only state where the two differ is ONE
    // control carrying TWO causes — and that state is unreachable. Per result
    // the causes are mutually exclusive (out-of-range returns before any
    // write, EchoJayParamApply.h:568-575; readback only on applied==false;
    // stale-display only on applied==true), and reaching one parameter twice
    // under one semanticLabel has no path: the band matcher consumes each
    // band, flat and controls entries that hit one parameter carry different
    // keys, and JSON keys are unique.
    //
    // So a label in two cause arrays is always TWO controls, and two rows
    // there is correct rather than a double-count (§2). What this function
    // stops is one control producing several rows: its cause row claims the
    // label, so the rollup does not also report it.
    //
    // The reverse case — several controls sharing a label under ONE cause — is
    // already collapsed to a single array entry upstream by
    // addIfNotAlreadyThere. That undercount is pre-existing and PARKED (§2,
    // "Two defects, and only one of them is A9's"); nothing here can widen or
    // narrow it, because nothing here can see past the collapse.
    std::vector<DialMissRow> rows;
    juce::StringArray claimed;   // labels a DECLINE row has taken (§2: a rollup
                                 // earns a row only for names no specific
                                 // reason has claimed)
    auto causeRow = [&] (const juce::StringArray& names, const juce::String& reason,
                         bool isDecline)
    {
        for (const auto& n : names)
        {
            DialMissRow row;
            row.reason = reason;
            row.names.add (n);
            // Only the verdict rides as a further reason, and only on a
            // decline. A stale_display_kept control was APPLIED and is not in
            // `manual`, so the slot's verdict is not a reason for IT.
            if (isDecline && verdict.isNotEmpty()) row.alsoReasons.add (verdict);
            if (isDecline) claimed.addIfNotAlreadyThere (n);
            rows.push_back (row);
        }
    };
    // Rank order (§2): cause before verdict, failure before success.
    if (readbackAllowed) causeRow (di.readbackMiss, "readback_mismatch",  true);
    causeRow (di.outOfRange,  "out_of_range",       true);
    causeRow (di.unconfirmed, "stale_display_kept", false);

    // ---- 2. The verdict rows: names no cause claimed ------------------------
    // One per label in `manual`, exactly as before — this is where the parked
    // undercount continues to live, untouched.
    if (verdict.isNotEmpty())
        for (const auto& n : di.manual)
            if (! claimed.contains (n))
                rows.push_back ({ verdict, juce::StringArray (n), {} });

    return rows;
}

} // namespace echojay
