#pragma once
// =============================================================================================
//  EJReadBackWait.h - "NOT YET" IS NOT "NEVER" (his 17:25 fault, 25 Oct 2026)
// =============================================================================================
//  Sean:
//
//      *"The change to slot 1: Left Reduction 3, Right Reduction 3 was sent but not confirmed by
//      the plugin's read-back (left reduction, right reduction) - check it on the plugin."*
//
//  His log says the write was exactly right, and two seconds too late to be read where the
//  sentence was composed:
//
//      17:23:59  EJLink: applied set@1 ... applied=1/1
//      17:23:59  EJParamMaps(Link): asked the V2 for 1 map(s), 1 outstanding
//      17:24:00  step ... DONE "... I could not read these values back to confirm them"
//      17:24:02  EJParamMaps(Link): the V2 answered with 1 map(s)
//      17:24:02  EJParamApply: Left Reduction: APPLIED 0.300 ... landed "3.0"
//      17:24:02  EJParamApply: Right Reduction: APPLIED 0.300 ... landed "3.0"
//
//  The Link had never hosted that plugin, so it had no parameter map. A `set` op's ack comes back
//  when the payload is STORED; the map-gated apply runs when the map arrives. ChainHost's own
//  comment on the op has said so since 5 September: *"a first-encounter plugin with no cached map
//  has always reported dialled having written nothing ... it needs the op's result deferred until
//  the dial settles."*
//
//  ---- WHAT TELLS "PENDING" APART FROM "UNREADABLE": TIME, AND NOTHING ELSE -------------------
//
//  From the reading side both look identical - no value for those ids - so the one honest move is
//  to read again on a BOUND and let the clock answer. A pending apply becomes readable in a second
//  or two; a plugin that will not report stays unreadable however long we wait, and then the
//  sentence he gets is the one he got before, which is correct for that case.
//
//  The wait only applies when there is nothing WRONG to report: a value that read back at the
//  wrong figure is a real miss, and it has its own road (one more write, then the honest failure).
//  Waiting on a miss would delay bad news, which is the opposite of the rule.
// =============================================================================================

namespace echojay
{

/** Should the read-back be taken again instead of reporting?

    `anyMatched`     - at least one value read back as asked: there is something to report.
    `anyMissed`      - at least one value read back WRONG: a real miss, reported now, never waited on.
    `anyUnreadable`  - at least one value has no reading at all, which is what pending looks like.
    `readAttempt`    - how many times the read has already been taken again (0 on the first pass).
    `maxReadRetries` - the bound. */
inline bool readBackShouldWaitAgain (bool anyMatched, bool anyMissed, bool anyUnreadable,
                                     int readAttempt, int maxReadRetries)
{
    if (anyMatched || anyMissed) return false;      // something to say, say it
    if (! anyUnreadable)         return false;      // nothing was asked for that could be read
    return readAttempt < maxReadRetries;
}

} // namespace echojay
