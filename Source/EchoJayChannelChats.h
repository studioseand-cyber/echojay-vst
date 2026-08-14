#pragma once

#include <cstddef>

// The channel-chat selection decision, header-inline so the shipped logic and
// its test compile the SAME bytes (the EchoJayHistoryTrim.h discipline: the
// gate links the previous build's SharedCode lib, so anything under test must
// live in a header the test TU includes directly).
//
// Until 14 Aug 2026 a channel had exactly one chat per (linkUid, trackName)
// and the lookup returned the FIRST match, which is why "New chat" into a
// channel silently appended to the existing conversation. A channel now owns
// many chats and the lookup means something: THE MOST RECENT BY ACTIVITY.

namespace echojay
{

struct LatestChatPick
{
    int index   = -1;   // index into the chats container; -1 = no match
    int matches = 0;    // how many chats matched (linkUid, trackName)
};

// Most recent by ACTIVITY: updatedAt when non-empty, else created. Both are
// ISO-8601 UTC strings, so lexical comparison IS chronological comparison
// (the same activityKey rule the dashboard documents at EchoJayWorkspace.h's
// updatedAt field). Container order is deliberately not consulted: addChat
// prepends by CREATION, and creation order is not activity order.
//
// Templated on the chat type (needs .linkUid, .trackName, .updatedAt,
// .created) so the unit test exercises this exact code over a lightweight
// stub instead of dragging the full workspace header into the test TU.
template <typename ChatVec, typename StringType>
inline LatestChatPick latestChatForLink (const ChatVec& chats,
                                         const StringType& linkUid,
                                         const StringType& trackName)
{
    LatestChatPick r;
    StringType bestKey;
    for (std::size_t i = 0; i < chats.size(); ++i)
    {
        const auto& c = chats[i];
        if (! (c.linkUid == linkUid) || ! (c.trackName == trackName))
            continue;
        ++r.matches;
        const StringType key = c.updatedAt.isNotEmpty() ? c.updatedAt : c.created;
        if (r.index < 0 || bestKey < key)
        {
            bestKey = key;
            r.index = (int) i;
        }
    }
    return r;
}

} // namespace echojay
