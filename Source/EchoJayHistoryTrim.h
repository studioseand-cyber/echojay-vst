#pragma once

#include <algorithm>
#include <vector>

// The /api/chat history-trim decision, header-inline so the shipped logic and
// its test compile the SAME bytes (the mapfps_test discipline: the gate links
// the previous build's SharedCode lib, so anything under test must live in a
// header the test TU includes directly).
//
// Born from a live defect (13 Aug 2026): the previous trimmer charged the
// newest message against one shared 60000-byte payload budget, assuming
// history would fall off first and the newest would always fit inside it.
// The newest turn carries every per-turn injection ([AVAILABLE PLUGINS], the
// chain rules, [AVAILABLE BUILTINS] with full ParamSchemas, [CURRENT CHAIN],
// LINK LEVELS) and measured 61-70KB on every live send, so the budget was
// negative before the backward walk started, no history message was ever
// admitted, and the model could not remember its own previous reply.
//
// The contract now: the NEWEST message is always sent whole and its size is
// charged nowhere. HISTORY has its own byte budget, walked backwards
// newest-first over stripped sizes (injections are cut from history turns
// before sizing), on top of the message-count cap.

namespace echojay
{

struct HistoryTrimResult
{
    int firstIdx        = 0;  // first wire-array index admitted into messages[]
    int total           = 0;  // candidate history messages (everything but the newest)
    int kept            = 0;  // history messages actually admitted
    int droppedByCap    = 0;  // lost to the message-count cap
    int droppedByBudget = 0;  // lost to the history byte budget
    int droppedByRole   = 0;  // lost aligning the first message onto a user turn
    int pairedBack      = 0;  // re-admitted so an assistant turn keeps the user turn before it (7 Oct 2026)
};

// strippedSizes: stripped-content byte size of EVERY message, newest last.
// The newest entry is present so callers pass the arrays index-aligned, but
// it is deliberately never read: the newest message is not charged against
// any budget. roleIsUser: nonzero where that message's role is "user".
// maxHistoryMessages counts messages INCLUDING the newest (the historical
// 12 keeps at most 11 history messages plus the newest).
inline HistoryTrimResult trimChatHistory (const std::vector<int>& strippedSizes,
                                          const std::vector<char>& roleIsUser,
                                          int maxHistoryMessages,
                                          int maxHistoryBytes)
{
    HistoryTrimResult r;
    const int n = (int) strippedSizes.size();
    if (n <= 0 || n != (int) roleIsUser.size())
        return r;
    r.total = n - 1;

    // Message-count cap first: bounds how far the byte walk can reach back.
    int firstIdx = std::max (0, n - maxHistoryMessages);
    r.droppedByCap = firstIdx;

    // History byte budget: walk backwards from the message just before the
    // newest, admitting while it fits. Older messages fall off first. The
    // newest message (index n-1) is not part of this walk.
    {
        int budget = maxHistoryBytes;
        int cutIdx = n - 1;
        while (cutIdx > firstIdx)
        {
            const int sz = strippedSizes[(size_t) (cutIdx - 1)];
            if (budget - sz < 0) break;
            budget -= sz;
            --cutIdx;
        }
        r.droppedByBudget = std::max (0, cutIdx - firstIdx);
        firstIdx = std::max (firstIdx, cutIdx);
    }

    // The Anthropic API requires messages[] to open with a user turn. Two ways
    // to get there, and WHICH ONE MATTERS (7 Oct 2026, test 5):
    //
    // Skipping FORWARD off a leading assistant turn throws that turn away. When
    // the window happens to start on the assistant's own last reply, the thing
    // thrown away is the reply the user is answering - and the server's
    // affirmation rule reads exactly that message to find the offer being
    // accepted ("Want me to add that to the EQ in slot 1?" -> "yes do it").
    // Lose it and the turn dead-ends on "Nothing is waiting to be applied".
    // Sean's live log shows this alignment firing with roleAlign 1 and 2 on
    // ordinary turns, so it is not hypothetical; it has simply been discarding
    // OLD assistant turns, where it costs little.
    //
    // So: step BACK to the user turn before it when there is one, re-admitting
    // that pair, and only skip forward when there is no earlier user turn at
    // all. The step back can take history one message past maxHistoryBytes -
    // a named, bounded exception, because an assistant turn without the user
    // turn that prompted it is not history the model can use.
    {
        const int before = firstIdx;
        if (firstIdx < n && roleIsUser[(size_t) firstIdx] == 0)
        {
            int back = firstIdx;
            while (back > 0 && roleIsUser[(size_t) (back - 1)] == 0)
                --back;                                    // walk over consecutive assistant turns
            if (back > 0 && roleIsUser[(size_t) (back - 1)] != 0)
            {
                firstIdx = back - 1;                       // the user turn that prompted them
                r.pairedBack = before - firstIdx;
                if (r.droppedByBudget > 0) r.droppedByBudget = std::max (0, r.droppedByBudget - r.pairedBack);
                else                       r.droppedByCap    = std::max (0, r.droppedByCap    - r.pairedBack);
            }
            else
            {
                while (firstIdx < n && roleIsUser[(size_t) firstIdx] == 0)
                    ++firstIdx;
                if (firstIdx >= n)
                    firstIdx = n - 1;
                r.droppedByRole = firstIdx - before;
            }
        }
    }

    r.firstIdx = firstIdx;
    r.kept     = (n - 1) - firstIdx;
    return r;
}

} // namespace echojay
