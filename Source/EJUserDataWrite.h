#pragma once

#include <JuceHeader.h>

// ===========================================================================
// MAY A USER-DATA WRITE PROCEED? (23 Sep 2026, open list 225)
// ===========================================================================
//
// THE DEFECT THIS CLOSES. EchoJayAPI::saveUserSettings is a read-modify-write:
// it GETs /api/data, copies the user's collections forward, adds the profile,
// and POSTs the result. When the GET FAILED it did not stop. It fell to an
// else branch that wrote `chats`, `albums`, `reviews` and `refTracks` as EMPTY
// ARRAYS and POSTed them, so a failed read became a record asserting the user
// has none of any of them.
//
// WHY A PREDICATE AND NOT AN `if` AT THE CALL SITE. Open list 217: the gate
// links harnesses and never the editor or the processor, so anything asserted
// about EchoJayAPI.cpp is presence-only, a string a pin can grep for and not
// behaviour it can run. The only remedy that has ever worked here is to move
// the decision into a pure predicate in a header the test TU compiles
// directly. EJCaptureGuard.h did exactly this for cmpSyncMayStart and
// cmpMixTargetGain, and cmpMixTargetGain is the proof it matters: it was an
// expression inside processBlock, which is why an A/B regression survived
// 3,858 green checks.
//
// IT IS NOT IN EJCaptureGuard.h, AND THAT IS A CHOICE. That header scopes
// itself to output substitution in its own opening comment. An API write
// policy is a different subject, and filing it there would make the next
// person searching for either one find the wrong file.
//
// Header-inline on purpose, the EJDialMissRows.h discipline: the gate links
// the PREVIOUS build's SharedCode lib, so anything a pin exercises must live
// in a header the test TU compiles directly, or the pin measures the last
// build instead of this one.
// ===========================================================================

namespace echojay
{

/** True when the read that precedes a user-data write came back usable, so
    the write may proceed.

    THREE INPUTS, ALL LOAD-BEARING, and each is a way the old code shipped an
    empty record:
      statusCode == 200   a non-200 is a failed read, not an empty account
      bodyIsObject        a 200 carrying a non-object body says nothing about
                          what the user has
      rootNonNull         json.getDynamicObject() can still return null on a
                          var that reports isObject(), and the old code's
                          `if (root)` guard simply fell through to no payload
                          at all rather than refusing

    WHAT IT DELIBERATELY DOES NOT DECIDE: whether a key MISSING from an
    otherwise good body may be omitted from the write. That is a question about
    the SERVER's merge-or-replace semantics, it is UNANSWERED, and it is
    recorded in HANDOVER/ECHOJAY_API_CONTRACT.md section 6. This predicate is
    about whether the READ succeeded, nothing more.

    THE FIX DOES NOT DEPEND ON THAT ANSWER, which is why it could land first.
    If the server REPLACES, writing after a failed read destroys data. If it
    MERGES, the same write is a no-op for those keys and the round trip was
    pointless. Aborting is correct either way. */
inline bool userDataWriteMayProceed (int statusCode, bool bodyIsObject,
                                     bool rootNonNull) noexcept
{
    return statusCode == 200 && bodyIsObject && rootNonNull;
}

} // namespace echojay
