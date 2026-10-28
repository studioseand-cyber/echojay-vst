#pragma once

/** ---- A COPY-ROAD REQUEST THE CACHE COULD NOT ANSWER YET (28 Oct 2026, B9's skip) -------------

    `shadowFollowSelection` has three callers - his press, the switch going off, and a stale copy
    being rebuilt - and none of them is periodic. So a selection made while this process's rack
    cache for that Link is not valid yet asked for nothing and was never asked again: the live view
    simply never appeared, and B9 reported it as `shadowRunning=0` for days.

    The refusal is now remembered and retried from the view's own refresh. This is that decision,
    pulled out as a pure function for the reason `inlineNeedsReopen` and `mayTakeLentEditor` were:
    so a guard can drive it in both directions instead of a UI test trying to.

    `forget` matters as much as `retry`. A memo for a selection nobody is looking at any more is a
    way to open a copy behind the wrong cards later, which is the fault class the rack-and-slot work
    spent a day on. */
namespace echojay
{

struct ShadowRetry
{
    bool retry  = false;   ///< ask shadowFollowSelection again, now
    bool forget = false;   ///< drop the memo (either because it was used, or because he moved on)
};

/** @param havePending         a refusal is remembered
    @param stillSameSelection  the rack in view and the selected slot are still the ones refused
    @param cacheCanAnswer      the cache is valid, long enough, and names a plugin in that slot */
inline ShadowRetry shadowRetryDecision (bool havePending, bool stillSameSelection,
                                        bool cacheCanAnswer) noexcept
{
    if (! havePending)        return {};                      // nothing is waiting
    if (! stillSameSelection) return { false, true };          // he moved on: forget it
    if (cacheCanAnswer)       return { true,  true };          // ask once, and drop the memo first
    return {};                                                 // still waiting, keep the memo
}

/** The ask's own poll is 25 x 120 ms, so three seconds covers it; five is the bound, which leaves
    room for a slow message thread without letting a lost ask latch for ever. */
inline constexpr int kShadowAskBudgetMs = 5000;

/** Is a tap request for this very slot still legitimately in flight?

    "One at a time" is right, and unbounded it is a latch: the ask's poll clears the flag when it
    gives up, but a copy creation that never calls back leaves it set - and then every later
    selection of that slot returns silently and the live view never comes. Bounded in TIME, like
    every other wait in this project. A clock that has gone backwards (a negative age) is treated as
    NOT in flight: the safe direction is to ask again. */
inline bool shadowAskInFlight (bool asking, bool sameTarget, int msSinceAsk,
                               int budgetMs = kShadowAskBudgetMs) noexcept
{
    return asking && sameTarget && msSinceAsk >= 0 && msSinceAsk < budgetMs;
}

} // namespace echojay
