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

// ===========================================================================
// WHICH COLLECTION KEYS A SETTINGS WRITE MAY CARRY (10 Oct 2026, from B)
// ===========================================================================
//
// THE QUESTION ABOVE IS NOW ANSWERED. The comment left open whether
// POST /api/data MERGES or REPLACES, and said the failed-read fix did not
// depend on the answer. B has answered it:
//
//   A MISSING KEY KEEPS THE STORED VALUE. AN EXPLICIT EMPTY ARRAY CLEARS IT.
//
// So the remaining half of the defect is live. saveUserSettings copies the
// user's collections forward from the GET, and for a key the body did NOT
// carry it wrote `[]`. Under merge semantics that is not a harmless echo: it
// is the one shape that DESTROYS the stored value. A body that omits `albums`
// - because the server trimmed it, or the user has none yet, or the shape
// changed - came back as a write asserting the user has no albums.
//
// The rule is therefore: ECHO WHAT THE READ CARRIED, OMIT WHAT IT DID NOT.
// Never synthesise a value for a key this client does not originate. The
// plugin is not the author of chats, albums, reviews, reference tracks or
// pinned projects; it is a settings editor that must hand them back untouched.
//
// pinnedProjects JOINS THE LIST (B's second item). It was not echoed at all,
// so under REPLACE semantics a settings save silently unpinned every song, and
// under MERGE it survived only by luck - the key's absence is what saved it.
// Luck is not a mechanism, and the workspace sync already round-trips this key
// (EchoJayWorkspace.cpp writes "pinnedProjects"), so the two writers now agree.
//
// WHY A PREDICATE, AGAIN: open list 217, the reason recorded above. The gate
// never links EchoJayAPI.cpp, so a rule expressed as an `if` there is a string
// a pin can grep for and not behaviour a suite can run.

/** The collection keys a settings write forwards from the read, in the order
    the payload carries them. Not a list of things the plugin owns - the exact
    opposite: these are the keys it must hand back exactly as it found them. */
inline juce::StringArray userDataForwardedKeys()
{
    return { "chats", "albums", "reviews", "refTracks", "pinnedProjects" };
}

/** May this write carry `key`, given whether the 200 body carried it?

    TRUE  -> copy the read's value across verbatim.
    FALSE -> OMIT the key. Do not write [], {} or null: under the merge
             semantics B confirmed, an explicit empty value is a DELETE, and
             this client has nothing to restore it from.

    Deliberately total rather than clever: a key the read did not carry is
    never written, whatever it is. */
inline bool userDataMayForwardKey (bool readCarriedKey) noexcept
{
    return readCarriedKey;
}

/** May the write carry `baseUpdatedAt` for the server to check against?

    Only when the read actually supplied a non-empty stamp. A write that
    invents one, or sends an empty string, is claiming to have read a version
    it did not - which is worse than sending no stamp at all, because the
    server would take it as a conflict check that passed. */
inline bool userDataMaySendBaseUpdatedAt (const juce::String& stampFromRead) noexcept
{
    return stampFromRead.isNotEmpty();
}

} // namespace echojay
