#pragma once
// =============================================================================================
//  EJChatHome.h - WHICH ROWS GET A HOME BEFORE THE REPLAY LIST IS CLEARED
//  (his item 3, 26 Oct 2026: the disappearing message)
// =============================================================================================
//  Sean, twice now: a message is drawn in a chat and then gone. The second sighting, from the rig
//  on 25 Oct: *"his message WAS drawn in the new chat and was gone again by the snapshot these
//  checks read (onScreen rows now: 0, user rows 0, cards 0)."*
//
//  ---- WHAT READING THE CODE FOUND -----------------------------------------------------------
//
//  A chat switch clears the processor's replay list, and Sean's rule 2 of 18 Oct is that it must:
//  *"A message is saved only into the chat it was sent in. Opening or switching chats never moves,
//  copies or appends anything."*  His 09:24 fault of 22 Oct then added the other half - a row that
//  belongs elsewhere must not be DELETED either - and the fix's own sentence is the rule:
//
//      *"A row that names its own chat has a home; it goes there, and only then is the list
//       cleared."*
//
//  That was implemented for a row carrying `agentJson` - a run's card - and for nothing else. A
//  row without one was counted in "dropped N unsynced row(s)" and deleted: his own typed words, or
//  an assistant line, in a chat the store had not caught up with yet. The asymmetry had no reason
//  behind it, and it is exactly the shape he keeps seeing.
//
//  THIS IS A HOLE FOUND BY READING, NOT A PROVEN CAUSE of either sighting. The 22 Oct log has
//  rotated away and the 25 Oct rig run shows no clear-site line at all, so the two may be different
//  faults. It is shipped because a row that can be deleted with no home is wrong whatever else is
//  true, and the rig now names which window published each snapshot so the next run settles the rest.
// =============================================================================================

namespace echojay
{

/** Where a row on the replay list goes when the list is about to be cleared for another chat. */
enum class RowHome
{
    keep,       // it belongs to the chat being opened: it stays on the list
    card,       // another chat's run card -> that chat's stored log, by its entry id
    message,    // another chat's plain row -> that chat's stored log, as prose
    nowhere     // nothing can be done with it: no chat of its own, or nothing in it
};

/** `ownChatId`  - the row names a chat of its own (empty means it never got stamped).
    `isThisChat` - that chat is the one being opened.
    `hasCard`    - the row carries a run's agentJson.
    `hasContent` - the row has words in it. */
inline RowHome rowHomeFor (bool ownChatId, bool isThisChat, bool hasCard, bool hasContent)
{
    if (! ownChatId)            return RowHome::nowhere;   // never invent a chat for an unstamped row
    if (isThisChat)             return RowHome::keep;
    if (hasCard)                return RowHome::card;
    if (hasContent)             return RowHome::message;
    return RowHome::nowhere;                               // an empty row is not a loss
}

} // namespace echojay
