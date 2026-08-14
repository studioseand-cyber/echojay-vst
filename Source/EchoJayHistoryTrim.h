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

    // The Anthropic API requires messages[] to open with a user turn: skip
    // forward off any leading assistant turns. Degenerate case (nothing
    // qualifies): send the newest alone.
    {
        const int before = firstIdx;
        while (firstIdx < n && roleIsUser[(size_t) firstIdx] == 0)
            ++firstIdx;
        if (firstIdx >= n)
            firstIdx = n - 1;
        r.droppedByRole = firstIdx - before;
    }

    r.firstIdx = firstIdx;
    r.kept     = (n - 1) - firstIdx;
    return r;
}

} // namespace echojay
