#include "EchoJayWorkspace.h"
#include "EJAgentRunView.h"   // normalisedProse: the record compares answers the way the card does
extern "C" void EchoJay_NSLog (const char* msg);   // NativeClip.mm, as every other unit declares it
#include <algorithm>

extern void ejTeardownLog(const juce::String& msg);

// ============================================================================
// B.0 — chat last-activity stamps
// ============================================================================
// EXACTLY JavaScript's new Date().toISOString(), which is what the web client
// writes (api/data.js and the D1.1 chat mutations): UTC, milliseconds,
// trailing Z. NOT juce::Time::toISO8601, which renders LOCAL time with a
// numeric offset ("+0100"); the dashboard sorts these strings with
// localeCompare (lib/dash/adapters.js activityKey), so a local-offset stamp
// would order wrongly against a UTC one for the same instant.
static juce::String isoUtcNow()
{
    const auto now = juce::Time::getCurrentTime();
    // Time's component accessors are local, so shift by the local offset and
    // read the components back as UTC. No CRT date calls, so this behaves the
    // same on Windows.
    const juce::Time utc (now.toMilliseconds()
                          - (juce::int64) now.getUTCOffsetSeconds() * 1000);
    return juce::String::formatted("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                                   utc.getYear(), utc.getMonth() + 1, utc.getDayOfMonth(),
                                   utc.getHours(), utc.getMinutes(), utc.getSeconds(),
                                   (int) (now.toMilliseconds() % 1000));
}

// ============================================================================
// Construction / destruction
// ============================================================================

EchoJayWorkspace::EchoJayWorkspace(EchoJayAPI& apiRef)
    : api(apiRef)
{
}

EchoJayWorkspace::~EchoJayWorkspace()
{
    alive->store(false);
    stopTimer();
}

// ============================================================================
// Public API
// ============================================================================

void EchoJayWorkspace::requestLoad()
{
    if (!api.isLoggedIn() || loadInFlight) return;
    loadState = LoadState::Loading;
    doLoad();
}

void EchoJayWorkspace::requestSync()
{
    if (!api.isLoggedIn()) return;
    syncPending = true;
    startTimer(1000); // restart the debounce window
}

// ============================================================================
// Mutation helpers
// ============================================================================

void EchoJayWorkspace::addChat(WsChat chat)
{
    chats.insert(chats.begin(), std::move(chat));
}

void EchoJayWorkspace::addAlbum(WsAlbum album)
{
    albums.push_back(std::move(album));
}

void EchoJayWorkspace::assignProjectToAlbum(const juce::String& projectName,
                                            const juce::String& albumId)
{
    if (projectName.isEmpty()) return;
    for (auto& a : albums)
        a.projectNames.removeString(projectName);   // remove from all first
    if (albumId.isNotEmpty())
        for (auto& a : albums)
            if (a.id == albumId)
            {
                if (!a.projectNames.contains(projectName))
                    a.projectNames.add(projectName);
                ejTeardownLog("[Workspace] assignProjectToAlbum \"" + projectName
                    + "\" -> album " + a.id + " projectNames=["
                    + a.projectNames.joinIntoString(", ") + "]");
                break;
            }
}

juce::String EchoJayWorkspace::createAlbumWithName(const juce::String& name)
{
    WsAlbum a;
    a.id      = "album-" + juce::String(juce::Time::currentTimeMillis());
    a.name    = name;
    a.created = juce::Time::getCurrentTime().toISO8601(true);
    albums.push_back(a);
    return a.id;
}

bool EchoJayWorkspace::updateReviewAudio(const juce::String& reviewId,
                                        const juce::String& fileName,
                                        const juce::var& waveform)
{
    if (reviewId.isEmpty() || fileName.isEmpty()) return false;
    for (auto& r : reviews)
        if (r.id == reviewId)
        {
            if (r.fileName == fileName) return false;   // already set
            r.fileName = fileName;
            if (waveform.isArray()) r.waveform = waveform;
            requestMutationSync();
            return true;
        }
    return false;
}

void EchoJayWorkspace::deleteReviews(const juce::StringArray& ids)
{
    if (ids.isEmpty()) return;
    reviews.erase(std::remove_if(reviews.begin(), reviews.end(),
        [&](const WsReview& r) { return ids.contains(r.id); }), reviews.end());
    requestMutationSync();
}

void EchoJayWorkspace::addReview(WsReview review)
{
    reviews.insert(reviews.begin(), std::move(review));
    if ((int)reviews.size() > 50)
        reviews.resize(50);
}

void EchoJayWorkspace::moveChatToAlbum(const juce::String& chatId,
                                        const juce::String& albumId)
{
    // Remove from current album's chatIds
    for (auto& a : albums)
        a.chatIds.removeString(chatId);

    // Update the chat's albumId
    for (auto& c : chats)
        if (c.id == chatId) { c.albumId = albumId; c.updatedAt = isoUtcNow(); break; }

    // Add to new album's chatIds (if albumId non-empty)
    if (albumId.isNotEmpty())
    {
        for (auto& a : albums)
        {
            if (a.id == albumId)
            {
                if (!a.chatIds.contains(chatId))
                    a.chatIds.add(chatId);
                break;
            }
        }
    }
}

void EchoJayWorkspace::removeChatFromAlbum(const juce::String& chatId)
{
    for (auto& c : chats)
        if (c.id == chatId) { c.albumId = ""; c.updatedAt = isoUtcNow(); break; }

    for (auto& a : albums)
        a.chatIds.removeString(chatId);
}

void EchoJayWorkspace::removeChat(const juce::String& chatId)
{
    // Unlink from every album
    for (auto& a : albums)
        a.chatIds.removeString(chatId);
    // Erase from chats list
    chats.erase(std::remove_if(chats.begin(), chats.end(),
        [&](const WsChat& c) { return c.id == chatId; }), chats.end());
}

void EchoJayWorkspace::setAlbumName(const juce::String& albumId, const juce::String& name)
{
    for (auto& a : albums)
        if (a.id == albumId) { a.name = name; return; }
}

void EchoJayWorkspace::removeAlbum(const juce::String& albumId)
{
    // Ungroup all chats that belonged to this album
    for (auto& c : chats)
        if (c.albumId == albumId) c.albumId = "";
    // Remove the album itself
    albums.erase(std::remove_if(albums.begin(), albums.end(),
        [&](const WsAlbum& a) { return a.id == albumId; }), albums.end());
}

// ---- STAGE A OF THE CHAT LOG (his ruling of 21 Oct 2026) -------------------------------------
//
// *"One stored log per chat, where every entry ... gets its chat id, a monotonic order number and a
// stable id at the moment it's created. The view only renders that log in order; a live run updates
// its own entry by id (steps, chips, closing) and never inserts by position; no merge on reopen."*
//
// These two functions are the whole of stage A's writing half. Nothing reads `seq` or `id` yet - by
// his approval of the staging, A+B write the new log beside the old one so the switch in C can be
// tested on his real chats instead of a fixture.
//
// WHY THE ID OF AN AGENT CARD IS THE RUN'S KEY, and not something minted here: the key already
// exists before the server has answered (there is no sessionId yet), it is in the snapshot, and it
// survives a replay and a reopen. That is exactly what the per-window integer could not do - the
// chat replay reissues those, which is how a waiting card lost its chips at 14:15.
bool EchoJayWorkspace::lastStampOf (const juce::String& chatId, juce::int64& seqOut,
                                    juce::String& idOut) const
{
    for (const auto& c : chats)
        if (c.id == chatId && ! c.messages.empty())
        {
            const auto& m = c.messages.back();
            if (m.seq <= 0) return false;
            seqOut = m.seq; idOut = m.entryId;
            return true;
        }
    return false;
}

juce::int64 EchoJayWorkspace::takeNextSeq (const juce::String& chatId)
{
    for (auto& c : chats)
        if (c.id == chatId)
        {
            if (c.nextSeq <= 0)
            {
                juce::int64 highest = 0;
                for (const auto& e : c.messages) highest = juce::jmax (highest, e.seq);
                c.nextSeq = highest + 1;
            }
            return c.nextSeq++;
        }
    return 0;   // a chat this store does not know yet: NOT stamped, never guessed
}

void EchoJayWorkspace::stampLogEntry (WsChat& chat, WsMessage& m)
{
    // THE ALLOCATOR IS THE CHAT'S. A row never takes its number from the size of the list: a list
    // can be filtered, merged or half-loaded, and all three have happened in this file's history.
    if (chat.nextSeq <= 0)
    {
        // A chat whose rows predate stage A: start past whatever is already there, so a migration
        // that has not run yet cannot collide with a live append.
        juce::int64 highest = 0;
        for (const auto& e : chat.messages) highest = juce::jmax (highest, e.seq);
        chat.nextSeq = highest + 1;
    }
    if (m.seq <= 0) m.seq = chat.nextSeq++;
    if (m.entryId.isEmpty())
    {
        // An agent card's identity is the RUN's. Read out of the snapshot rather than passed in, so
        // no call site has to remember - and a card with no key yet still gets a minted one.
        // ---- THE PARSED VAR IS HELD, NOT POINTED INTO (the 18 Sep rule, caught again today) ----
        // `if (auto* o = juce::JSON::parse (x).getDynamicObject())` dangles: in C++17 the temporary
        // in an if-statement's init is destroyed BEFORE the body runs, so `o` points at freed
        // memory and read as an empty object - which is exactly what legs CL4 and CL9 reported
        // ("parsed=object, keys=[]"). The same pattern caused the 18 Sep Pro Tools crash; it is a
        // named local here, which is the whole of the fix.
        juce::String fromRun;
        if (m.agentJson.isNotEmpty())
        {
            const auto snap = juce::JSON::parse (m.agentJson);
            if (auto* o = snap.getDynamicObject())
                fromRun = o->getProperty ("runKey").toString().trim();
        }
        m.entryId = fromRun.isNotEmpty()
                      ? fromRun
                      : (juce::String (m.seq) + "-" + juce::Uuid().toDashedString().substring (0, 8));
    }
}

int EchoJayWorkspace::migrateChatLog (WsChat& chat)
{
    // THE FILE ORDER IS THE TRUTH. Not the timestamps (rows carry none), not a re-sort by role: the
    // order the rows are in is the order they happened in, and stage A's whole claim is that an
    // entry's place is recorded rather than recomputed - so recording the order already on disk is
    // the only honest migration.
    int stamped = 0;
    juce::int64 next = 1;
    for (auto& m : chat.messages)
    {
        if (m.seq > 0) { next = juce::jmax (next, m.seq + 1); continue; }   // already stamped
        m.seq = next++;
        if (m.entryId.isEmpty())
        {
            juce::String fromRun;
            if (m.agentJson.isNotEmpty())
            {
                const auto snap = juce::JSON::parse (m.agentJson);   // held, never pointed into
                if (auto* o = snap.getDynamicObject())
                    fromRun = o->getProperty ("runKey").toString().trim();
            }
            // A RUN THAT WAS LIVE ACROSS THE UPGRADE IS STILL FOUND, because its key is its id.
            m.entryId = fromRun.isNotEmpty()
                          ? fromRun
                          : (juce::String (m.seq) + "-" + juce::Uuid().toDashedString().substring (0, 8));
        }
        ++stamped;
    }
    if (chat.nextSeq < next) chat.nextSeq = next;
    return stamped;
}

bool EchoJayWorkspace::appendMessageToChat(const juce::String& chatId,
                                            const juce::String& role,
                                            const juce::String& content,
                                            const juce::String& reviewId,
                                            const juce::String& chainJson,
                                            const juce::String& gainJson,
                                            const juce::String& askJson,
                                            const juce::String& editJson,
                                            const juce::String& altPrompt,
                                            const juce::String& altLabel,
                                            const juce::String& displayText,
                                            const juce::String& editTargetUid,
                                            const juce::String& editTargetName,
                                            const juce::String& figuresJson,
                             const juce::String& agentJson)
{
    for (auto& c : chats)
    {
        if (c.id == chatId)
        {
            bool first = c.messages.empty();
            WsMessage m;
            m.role      = role;
            m.content   = content;
            m.reviewId  = reviewId;
            m.figuresJson = figuresJson;
            m.chainJson = chainJson;
            m.gainJson  = gainJson;
            m.askJson   = askJson;
            m.editJson  = editJson;
            m.editAltPrompt = altPrompt;
            m.editAltLabel  = altLabel;
            m.displayText   = displayText;
            m.editTargetUid  = editTargetUid;
            m.editTargetName = editTargetName;
            m.agentJson      = agentJson;      // the run's card (18 Oct 2026)
            stampLogEntry (c, m);              // stage A: its place and its identity, at creation
            c.messages.push_back(std::move(m));
            c.updatedAt = isoUtcNow();   // B.0: real activity on this chat
            return first;
        }
    }
    // Chat not found — create a minimal entry so the message is persisted
    WsChat c;
    c.id      = chatId;
    c.title   = "New chat";
    c.created = juce::Time::getCurrentTime().toISO8601(true);
    // ...AND IT IS FILED, like any other chat (18 Oct 2026, Sean's rule 1). Without this it was
    // born with no project and no channel, which the sidebar draws as UNGROUPED and no chip can
    // point at. The editor owns the answer; this is the one place that asks for it.
    if (fileNewChat) fileNewChat (c);
    WsMessage m;
    m.role      = role;
    m.content   = content;
    m.reviewId  = reviewId;
    m.figuresJson = figuresJson;
    m.chainJson = chainJson;
    m.gainJson  = gainJson;
    m.askJson   = askJson;
    m.editJson  = editJson;
    m.editAltPrompt = altPrompt;
    m.editAltLabel  = altLabel;
    m.displayText   = displayText;
    m.editTargetUid  = editTargetUid;
    m.editTargetName = editTargetName;
    m.agentJson      = agentJson;
    stampLogEntry (c, m);   // stage A
    c.messages.push_back(std::move(m));
    c.updatedAt = isoUtcNow();
    chats.insert(chats.begin(), std::move(c));
    return true; // first message
}

EchoJayWorkspace::Repair EchoJayWorkspace::repairChatFiling (const juce::String& openProject,
                                                             const juce::String& albumId)
{
    Repair r;
    // (a) a chat with no project takes the open one. Dated chats keep their dates; only the FOLDER
    //     is repaired, because that is the only thing that was wrong.
    if (openProject.trim().isNotEmpty())
        for (auto& c : chats)
            if (c.trackName.trim().isEmpty())
            {
                c.trackName = openProject.trim();
                if (c.albumId.isEmpty()) c.albumId = albumId;
                ++r.refiled;
            }
    // (b) the em-dash twins: an assistant message whose normalised text repeats an earlier one in
    //     the SAME chat. Normalised exactly as the record's own write compares them.
    //
    // ---- AND THE RUN CARD'S OWN WORDS (20 Oct 2026, his 17:11 and 18:07 reports) ---------------
    //
    // *"The closing showed twice again"* - three times in one day, and the last two after the
    // key-minting fix of 16 Oct had closed the cause everyone knew about.
    //
    // THE ROAD IN is the SYNC, not the run. `requestMutationSync` POSTs the workspace and then calls
    // `requestSync()` - "pull any concurrent web-app edits" - so a line the SERVER persisted beside
    // the card comes straight back down as its own assistant message. The plugin never drew it
    // twice; it was handed a second copy and drew what it was given.
    //
    // (b) above could not catch it. It compares a message against EARLIER MESSAGES' content, and a
    // run card's prose does not live only in `content`: a MID-ROUND sentence lives on a step inside
    // `agentJson`, and the card's own `content` has by then been overwritten by a later round's
    // closing. His 15:29 case was exactly that - "Added EchoJay EQ. Settings applied." was round
    // 8's, and the card had moved on to "I cut the low-mids...".
    //
    // So the seen-set is seeded with EVERY prose string a run card in this chat carries: its
    // opening, its closing, and each step's label, detail and recorded answer. A PLAIN bubble (one
    // with no agentJson of its own) whose prose the card already says is then dropped as the
    // duplicate it is.
    //
    // TWO GUARDS AGAINST OVER-REACH, because dropping a real message is worse than showing one
    // twice: a message that carries its OWN agentJson is never dropped (it is a card, not an echo),
    // and a match under kMinFoldChars is ignored - "Done." or "OK" can legitimately be said by a
    // step and by a later turn, and folding on those would lose a real answer.
    static constexpr int kMinFoldChars = 20;
    for (auto& c : chats)
    {
        juce::StringArray seen;
        // EVERY card's words first, so a pulled echo is folded wherever it lands in the order -
        // the server's copy can arrive before or after the card it duplicates.
        for (const auto& m : c.messages)
        {
            if (m.agentJson.isEmpty()) continue;
            const auto v = echojay::agentui::fromJson (juce::JSON::parse (m.agentJson));
            auto note = [&seen] (const juce::String& t)
            {
                if (t.trim().length() < kMinFoldChars) return;
                seen.addIfNotAlreadyThere (echojay::agentui::normalisedProse (t));
            };
            note (v.opening);
            note (v.closing);
            for (const auto& st : v.steps) { note (st.label); note (st.detail); note (st.answered); }
            // AND THE CARD'S OWN `content`, which is what (b) compared against before this change.
            // Keeping it means a plain bubble duplicating a card's visible prose is still folded -
            // that half already worked and must not be lost to the widening.
            note (m.content);
        }
        std::vector<WsMessage> kept;
        kept.reserve (c.messages.size());
        for (auto& m : c.messages)
        {
            if (m.role == "assistant" && m.content.isNotEmpty() && m.agentJson.isEmpty())
            {
                const auto key = echojay::agentui::normalisedProse (m.content);
                if (m.content.trim().length() >= kMinFoldChars && seen.contains (key))
                { ++r.duplicatesDropped; continue; }
                seen.addIfNotAlreadyThere (key);
            }
            kept.push_back (m);
        }
        c.messages = std::move (kept);
    }
    // (c) and what CANNOT be repaired honestly: the same assistant text in two different chats.
    //     Counted and named, never deleted - a copy and a real answer read identically here.
    for (size_t i = 0; i < chats.size(); ++i)
        for (const auto& m : chats[i].messages)
        {
            if (m.role != "assistant" || m.content.length() < 40) continue;
            const auto key = echojay::agentui::normalisedProse (m.content);
            for (size_t j = i + 1; j < chats.size(); ++j)
                for (const auto& n : chats[j].messages)
                    if (n.role == "assistant"
                        && echojay::agentui::normalisedProse (n.content) == key)
                    {
                        ++r.crossChatSuspects;
                        EchoJay_NSLog (("EJStores: the same answer sits in chats " + chats[i].id
                                        + " and " + chats[j].id + " - left alone, because a copy and "
                                        "a real answer read identically in the record: \""
                                        + m.content.substring (0, 60) + "...\"").toRawUTF8());
                    }
        }
    if (r.refiled > 0 || r.duplicatesDropped > 0)
        EchoJay_NSLog (("EJStores: chat store repaired - " + juce::String (r.refiled)
                        + " chat(s) refiled under \"" + openProject + "\", "
                        + juce::String (r.duplicatesDropped) + " duplicate answer(s) dropped, "
                        + juce::String (r.crossChatSuspects)
                        + " cross-chat repeat(s) left for a human").toRawUTF8());
    return r;
}

bool EchoJayWorkspace::updateAgentCardById (const juce::String& chatId, const juce::String& entryId,
                                            const juce::String& agentJson, const juce::String& content)
{
    if (chatId.isEmpty() || entryId.isEmpty()) return false;
    auto* chat = findChatById (chatId);
    if (chat == nullptr) return false;
    for (auto& m : chat->messages)
    {
        if (m.entryId != entryId) continue;
        bool changed = false;
        if (m.agentJson != agentJson) { m.agentJson = agentJson; changed = true; }
        if (content.isNotEmpty() && m.content != content) { m.content = content; changed = true; }
        if (changed) requestMutationSync();
        return true;            // found, whether or not anything needed changing
    }
    return false;
}

void EchoJayWorkspace::updateAssistantAgentJson (const juce::String& chatId,
                                                 const juce::String& matchContent,
                                                 const juce::String& agentJson)
{
    for (auto& c : chats)
    {
        if (c.id != chatId) continue;
        for (auto& m : c.messages)
            if (m.role == "assistant" && m.content == matchContent)
                m.agentJson = agentJson;
        c.updatedAt = isoUtcNow();
        return;
    }
}

void EchoJayWorkspace::clearEditAltPrompt(const juce::String& chatId,
                                          const juce::String& matchContent)
{
    for (auto& c : chats)
    {
        if (c.id != chatId) continue;
        for (auto& m : c.messages)
            if (m.role == "assistant" && m.content == matchContent)
                m.editAltPrompt.clear();
        c.updatedAt = isoUtcNow();
        return;
    }
}

void EchoJayWorkspace::markEditApplied(const juce::String& chatId,
                                       const juce::String& matchContent,
                                       const juce::String& resultSummary,
                                       const juce::String& altPrompt,
                                       const juce::String& altLabel)
{
    for (auto& c : chats)
    {
        if (c.id != chatId) continue;
        for (auto& m : c.messages)
            if (m.role == "assistant" && m.editJson.isNotEmpty()
                && m.content == matchContent)
            {
                m.editApplied   = true;
                m.editResult    = resultSummary;
                m.editAltPrompt = altPrompt;
                m.editAltLabel  = altLabel;
            }
        c.updatedAt = isoUtcNow();
        return;
    }
}

void EchoJayWorkspace::markAskAnswered(const juce::String& chatId,
                                       const juce::String& matchContent)
{
    for (auto& c : chats)
    {
        if (c.id != chatId) continue;
        for (auto& m : c.messages)
            if (m.role == "assistant" && m.askJson.isNotEmpty()
                && m.content == matchContent)
                m.askAnswered = true;
        c.updatedAt = isoUtcNow();
        return;
    }
}

// 21t-j (a): the settle's line, completed in place. The LAST matching assistant message wins, as with the gain
// json above: a reply's text can repeat across a session and the most recent one is the one on screen.
void EchoJayWorkspace::updateAssistantText(const juce::String& chatId,
                                           const juce::String& matchContent,
                                           const juce::String& newContent)
{
    if (matchContent.isEmpty() || newContent.isEmpty()) return;
    for (auto& c : chats)
        if (c.id == chatId)
        {
            for (auto it = c.messages.rbegin(); it != c.messages.rend(); ++it)
                if (it->role == "assistant" && it->content == matchContent)
                { it->content = newContent; break; }
            c.updatedAt = isoUtcNow();
            requestMutationSync();
            return;
        }
}

void EchoJayWorkspace::updateAssistantGainJson(const juce::String& chatId,
                                               const juce::String& matchContent,
                                               const juce::String& gainJson)
{
    for (auto& c : chats)
        if (c.id == chatId)
        {
            // Prefer the LAST matching assistant message (most recent reply)
            for (auto it = c.messages.rbegin(); it != c.messages.rend(); ++it)
                if (it->role == "assistant" && it->content == matchContent)
                {
                    it->gainJson = gainJson;
                    c.updatedAt  = isoUtcNow();
                    return;
                }
            return;
        }
}

void EchoJayWorkspace::setChatTitle(const juce::String& chatId,
                                     const juce::String& title)
{
    for (auto& c : chats)
        if (c.id == chatId) { c.title = title; c.updatedAt = isoUtcNow(); return; }
}

void EchoJayWorkspace::setChatTrackName(const juce::String& chatId,
                                         const juce::String& trackName)
{
    for (auto& c : chats)
        if (c.id == chatId) { c.trackName = trackName; c.updatedAt = isoUtcNow(); return; }
}

void EchoJayWorkspace::renameProject(const juce::String& oldName,
                                     const juce::String& newName)
{
    const juce::String a = oldName.trim(), b = newName.trim();
    if (a.isEmpty() || b.isEmpty() || a == b) return;
    for (auto& c : chats)
        if (c.trackName == a) { c.trackName = b; c.updatedAt = isoUtcNow(); }
    for (auto& al : albums)
        for (auto& pn : al.projectNames)
            if (pn == a) pn = b;
}

void EchoJayWorkspace::incrementChatRevisionCount(const juce::String& chatId)
{
    for (auto& c : chats)
        if (c.id == chatId) { c.revisionCount++; c.updatedAt = isoUtcNow(); return; }
}

// NOTE: pinning deliberately does NOT stamp updatedAt. It already has its
// own timestamp (pinnedAt) which orders the pinned group, and it is an
// organisational act rather than activity: bumping a chat to the top of the
// dashboard's recents because someone pinned it would be a lie about when
// they last worked on it.
void EchoJayWorkspace::setChatPinned(const juce::String& chatId, bool pinned)
{
    for (auto& c : chats)
        if (c.id == chatId)
        {
            c.pinned   = pinned;
            c.pinnedAt = pinned ? juce::Time::getCurrentTime().toISO8601(true)
                                : juce::String();
            return;
        }
}

void EchoJayWorkspace::setAlbumPinned(const juce::String& albumId, bool pinned)
{
    for (auto& a : albums)
        if (a.id == albumId)
        {
            a.pinned   = pinned;
            a.pinnedAt = pinned ? juce::Time::getCurrentTime().toISO8601(true)
                                : juce::String();
            return;
        }
}

void EchoJayWorkspace::setProjectPinned(const juce::String& projectName, bool pinned)
{
    if (projectName.isEmpty()) return;
    // Remove any existing entry first (unpin, or re-pin to refresh pinnedAt).
    pinnedProjects.erase(std::remove_if(pinnedProjects.begin(), pinnedProjects.end(),
        [&](const WsPinnedProject& p) { return p.name == projectName; }),
        pinnedProjects.end());
    if (pinned)
        pinnedProjects.push_back({ projectName,
                                   juce::Time::getCurrentTime().toISO8601(true) });
}

bool EchoJayWorkspace::isAlbumPinned(const juce::String& albumId) const
{
    for (auto& a : albums) if (a.id == albumId) return a.pinned;
    return false;
}

bool EchoJayWorkspace::isProjectPinned(const juce::String& projectName) const
{
    for (auto& p : pinnedProjects) if (p.name == projectName) return true;
    return false;
}

juce::String EchoJayWorkspace::albumPinnedAt(const juce::String& albumId) const
{
    for (auto& a : albums) if (a.id == albumId) return a.pinnedAt;
    return {};
}

juce::String EchoJayWorkspace::projectPinnedAt(const juce::String& projectName) const
{
    for (auto& p : pinnedProjects) if (p.name == projectName) return p.pinnedAt;
    return {};
}

void EchoJayWorkspace::requestMutationSync()
{
    if (!api.isLoggedIn()) return;

    auto aliveFlag = alive;
    juce::String body = buildPostBody();

    api.postWorkspaceData(body, [this, aliveFlag](const juce::var&, int sc)
    {
        ejTeardownLog("[callAsync] EchoJayWorkspace::requestMutationSync POST callback");
        if (!aliveFlag->load()) return;
        if (sc == 200)
        {
            DBG("[EchoJayWorkspace] mutation POST ok — scheduling refresh");
            requestSync(); // pull any concurrent web-app edits
        }
        else
            DBG("[EchoJayWorkspace] mutation POST failed sc=" << sc);
    });
}

// ============================================================================
// Timer — fires after debounce delay
// ============================================================================

void EchoJayWorkspace::timerCallback()
{
    stopTimer();
    if (syncPending)
    {
        syncPending = false;
        doSync();
    }
}

// ============================================================================
// Load
// ============================================================================

void EchoJayWorkspace::doLoad()
{
    loadInFlight = true;
    auto aliveFlag = alive;

    api.getWorkspaceData([this, aliveFlag](const juce::var& json, int statusCode)
    {
        ejTeardownLog("[callAsync] EchoJayWorkspace::doLoad callback firing");
        if (!aliveFlag->load()) { ejTeardownLog("[callAsync] EchoJayWorkspace::doLoad: alive=false, bailing"); return; }

        loadInFlight = false;
        lastStatus = statusCode;

        if (statusCode != 200 || !json.isObject())
        {
            DBG("[EchoJayWorkspace] load failed — statusCode=" << statusCode);
            loadState = LoadState::Error;
            if (onLoaded) onLoaded();
            return;
        }

        auto* root = json.getDynamicObject();
        if (!root) { loadState = LoadState::Error; if (onLoaded) onLoaded(); return; }

        // Merge chats: prefer the local copy when it has >= messages than the
        // server copy (local may have pending messages not yet persisted).
        // Prepend any local-only chats that are not on the server yet.
        {
            std::vector<WsChat> serverChats;
            if (auto* arr = root->getProperty("chats").getArray())
                for (auto& v : *arr)
                    serverChats.push_back(parseChat(v));

            std::vector<WsChat> merged;
            merged.reserve(serverChats.size());
            for (auto& sc : serverChats)
            {
                bool usedLocal = false;
                for (auto& lc : chats)
                    if (lc.id == sc.id) {
                        merged.push_back(lc.messages.size() >= sc.messages.size() ? lc : sc);
                        usedLocal = true; break;
                    }
                if (!usedLocal) merged.push_back(sc);
            }
            for (auto& lc : chats) {
                bool onServer = false;
                for (auto& sc : serverChats) if (sc.id == lc.id) { onServer = true; break; }
                if (!onServer) merged.insert(merged.begin(), lc);
            }
            chats = std::move(merged);
        }

        // Parse albums — preserve local project→album membership by id if the
        // server omits it, so a move made before this load isn't wiped.
        {
            std::map<juce::String, juce::StringArray> localProjs;
            for (auto& la : albums)
                if (!la.projectNames.isEmpty()) localProjs[la.id] = la.projectNames;
            albums.clear();
            if (auto* arr = root->getProperty("albums").getArray())
                for (auto& v : *arr)
                {
                    auto a = parseAlbum(v);
                    if (a.projectNames.isEmpty())
                    {
                        auto it = localProjs.find(a.id);
                        if (it != localProjs.end()) a.projectNames = it->second;
                    }
                    albums.push_back(a);
                }
        }

        // Pinned projects (songs) — synced list; preserve local if server omits.
        {
            std::vector<WsPinnedProject> serverPins;
            if (auto* arr = root->getProperty("pinnedProjects").getArray())
                for (auto& v : *arr)
                    if (auto* o = v.getDynamicObject())
                        serverPins.push_back({ o->getProperty("name").toString(),
                                               o->getProperty("pinnedAt").toString() });
            if (!serverPins.empty() || root->hasProperty("pinnedProjects"))
                pinnedProjects = std::move(serverPins);
        }

        // Parse reviews — merge: keep locally-added reviews that the server
        // hasn't confirmed yet (avoids race with requestMutationSync).
        {
            // Collect IDs that the server returned
            juce::StringArray serverIds;
            if (auto* arr = root->getProperty("reviews").getArray())
                for (auto& v : *arr)
                    if (auto* o = v.getDynamicObject())
                        serverIds.add(o->hasProperty("id") ? o->getProperty("id").toString()
                                                           : o->getProperty("_id").toString());

            // Save locally-added reviews not on the server yet
            std::vector<WsReview> localOnly;
            for (auto& lr : reviews)
                if (!serverIds.contains(lr.id))
                    localOnly.push_back(lr);

            // Snapshot in-memory spectrum data before overwriting (server doesn't store it)
            // Maps review id → (spectrumBands, hasSpectrum)
            struct SpecSnap { std::array<float, 64> bands; bool has;
                              std::array<float, 6> macro; bool hasMacro; };
            std::vector<std::pair<juce::String, SpecSnap>> specCache;
            for (auto& lr : reviews)
                if (lr.hasSpectrum || lr.hasMacroBands)
                    specCache.push_back({ lr.id, { lr.spectrumBands, lr.hasSpectrum,
                                                   lr.macroBandDb, lr.hasMacroBands } });

            reviews.clear();
            if (auto* arr = root->getProperty("reviews").getArray())
            {
                for (auto& v : *arr)
                {
                    auto r = parseReview(v);
                    // Restore in-session spectrum that the server doesn't persist
                    for (auto& sc : specCache)
                        if (sc.first == r.id)
                        {
                            if (sc.second.has)      { r.spectrumBands = sc.second.bands; r.hasSpectrum = true; }
                            if (sc.second.hasMacro) { r.macroBandDb   = sc.second.macro; r.hasMacroBands = true; }
                            break;
                        }
                    reviews.push_back(std::move(r));
                }
            }

            // Prepend surviving local reviews (newest-first order maintained)
            for (int i = (int)localOnly.size() - 1; i >= 0; --i)
                reviews.insert(reviews.begin(), localOnly[(size_t)i]);
            if ((int)reviews.size() > 50)
                reviews.resize(50);
        }

        // Parse profile
        profile = parseProfile(root->getProperty("profile"));

        loadState = LoadState::Loaded;

        DBG("[EchoJayWorkspace] loaded — chats:" << (int)chats.size()
            << "  albums:" << (int)albums.size()
            << "  reviews:" << (int)reviews.size());

        if (onLoaded) onLoaded();
    });
}

// ============================================================================
// Sync (GET → merge → clean → POST)
// ============================================================================

void EchoJayWorkspace::doSync()
{
    auto aliveFlag = alive;

    // Step 1: GET latest from server so we don't overwrite concurrent web-app
    // edits. In phase 1 there are no local-only mutations so "merge by id"
    // is trivially "trust the server"; we just apply clean rules before
    // re-posting.
    api.getWorkspaceData([this, aliveFlag](const juce::var& json, int statusCode)
    {
        ejTeardownLog("[callAsync] EchoJayWorkspace::doSync GET callback firing");
        if (!aliveFlag->load()) { ejTeardownLog("[callAsync] EchoJayWorkspace::doSync: alive=false"); return; }

        lastStatus = statusCode;
        if (statusCode != 200 || !json.isObject())
        {
            DBG("[EchoJayWorkspace] sync GET failed — statusCode=" << statusCode);
            loadState = LoadState::Error;
            if (onLoaded) onLoaded();
            return;
        }

        auto* root = json.getDynamicObject();
        if (!root) { loadState = LoadState::Error; if (onLoaded) onLoaded(); return; }

        // Merge chats — same strategy as doLoad: prefer local when it has
        // >= messages so in-flight captures survive the background sync GET.
        {
            std::vector<WsChat> serverChats;
            if (auto* arr = root->getProperty("chats").getArray())
                for (auto& v : *arr)
                    serverChats.push_back(parseChat(v));

            std::vector<WsChat> merged;
            merged.reserve(serverChats.size());
            for (auto& sc : serverChats)
            {
                bool usedLocal = false;
                for (auto& lc : chats)
                    if (lc.id == sc.id) {
                        merged.push_back(lc.messages.size() >= sc.messages.size() ? lc : sc);
                        usedLocal = true; break;
                    }
                if (!usedLocal) merged.push_back(sc);
            }
            for (auto& lc : chats) {
                bool onServer = false;
                for (auto& sc : serverChats) if (sc.id == lc.id) { onServer = true; break; }
                if (!onServer) merged.insert(merged.begin(), lc);
            }
            chats = std::move(merged);
        }

        // Preserve local project→album membership by album id if the server
        // omits it (e.g. a client that strips projectNames), so an in-flight
        // "Move to album" is never wiped by the sync GET that follows its POST.
        {
            std::map<juce::String, juce::StringArray> localProjs;
            for (auto& la : albums)
                if (!la.projectNames.isEmpty()) localProjs[la.id] = la.projectNames;
            albums.clear();
            if (auto* arr = root->getProperty("albums").getArray())
                for (auto& v : *arr)
                {
                    auto a = parseAlbum(v);
                    if (a.projectNames.isEmpty())
                    {
                        auto it = localProjs.find(a.id);
                        if (it != localProjs.end()) a.projectNames = it->second;
                    }
                    if (!a.projectNames.isEmpty())
                        ejTeardownLog("[Workspace] sync album " + a.id
                            + " projectNames=[" + a.projectNames.joinIntoString(", ") + "]");
                    albums.push_back(a);
                }
        }

        // Pinned projects (songs) — synced list; preserve local if server omits.
        {
            std::vector<WsPinnedProject> serverPins;
            if (auto* arr = root->getProperty("pinnedProjects").getArray())
                for (auto& v : *arr)
                    if (auto* o = v.getDynamicObject())
                        serverPins.push_back({ o->getProperty("name").toString(),
                                               o->getProperty("pinnedAt").toString() });
            if (!serverPins.empty() || root->hasProperty("pinnedProjects"))
                pinnedProjects = std::move(serverPins);
            // else: server didn't send the field at all — keep local.
        }

        // Merge reviews — same as doLoad: preserve local-only entries + in-session spectrum
        {
            juce::StringArray serverIds;
            if (auto* arr = root->getProperty("reviews").getArray())
                for (auto& v : *arr)
                    if (auto* o = v.getDynamicObject())
                        serverIds.add(o->hasProperty("id") ? o->getProperty("id").toString()
                                                           : o->getProperty("_id").toString());

            std::vector<WsReview> localOnly;
            for (auto& lr : reviews)
                if (!serverIds.contains(lr.id))
                    localOnly.push_back(lr);

            // Snapshot in-memory spectrum data before overwriting (server doesn't store it)
            struct SpecSnap { std::array<float, 64> bands; bool has;
                              std::array<float, 6> macro; bool hasMacro; };
            std::vector<std::pair<juce::String, SpecSnap>> specCache;
            for (auto& lr : reviews)
                if (lr.hasSpectrum || lr.hasMacroBands)
                    specCache.push_back({ lr.id, { lr.spectrumBands, lr.hasSpectrum,
                                                   lr.macroBandDb, lr.hasMacroBands } });

            reviews.clear();
            if (auto* arr = root->getProperty("reviews").getArray())
            {
                for (auto& v : *arr)
                {
                    auto r = parseReview(v);
                    // Restore in-session spectrum that the server doesn't persist
                    for (auto& sc : specCache)
                        if (sc.first == r.id)
                        {
                            if (sc.second.has)      { r.spectrumBands = sc.second.bands; r.hasSpectrum = true; }
                            if (sc.second.hasMacro) { r.macroBandDb   = sc.second.macro; r.hasMacroBands = true; }
                            break;
                        }
                    reviews.push_back(std::move(r));
                }
            }

            for (int i = (int)localOnly.size() - 1; i >= 0; --i)
                reviews.insert(reviews.begin(), localOnly[(size_t)i]);
            if ((int)reviews.size() > 50)
                reviews.resize(50);
        }

        profile = parseProfile(root->getProperty("profile"));
        loadState = LoadState::Loaded;

        // Step 2: Build clean POST body and send.
        juce::String body = buildPostBody();

        api.postWorkspaceData(body, [aliveFlag](const juce::var&, int sc)
        {
            ejTeardownLog("[callAsync] EchoJayWorkspace::doSync POST callback firing");
            if (!aliveFlag->load()) return;
            if (sc == 200)
                DBG("[EchoJayWorkspace] sync POST ok");
            else
                DBG("[EchoJayWorkspace] sync POST failed — statusCode=" << sc);
        });
    });
}

// ============================================================================
// Parse helpers
// ============================================================================

WsChat EchoJayWorkspace::parseChat(const juce::var& v)
{
    // ---- STAGE A: EVERY CHAT IS MIGRATED AS IT IS PARSED (his plan's fifth case) --------------
    // One place, and it is the only road a chat takes into memory from either store - so there is
    // no "migrated on some paths" state to reason about. The stamping is idempotent, so a chat that
    // has been through it is untouched and costs one integer compare per row.
    // Said once per chat that actually needed it, because a silent migration of his whole history
    // is the kind of thing that must be in the log if it ever has to be explained.

    WsChat c;
    if (auto* obj = v.getDynamicObject())
    {
        // Accept both "_id" (MongoDB) and "id"
        c.id            = obj->hasProperty("id")    ? obj->getProperty("id").toString()
                                                     : obj->getProperty("_id").toString();
        c.title         = obj->getProperty("title").toString();
        c.created       = obj->getProperty("created").toString();
        c.trackName     = obj->getProperty("trackName").toString();
        c.albumId       = obj->getProperty("albumId").toString();
        c.revisionCount = (int)obj->getProperty("revisionCount");
        c.pinned        = (bool)obj->getProperty("pinned");
        c.linkUid       = obj->getProperty("linkUid").toString();      // absent -> "" = main chat
        c.linkNameSnap  = obj->getProperty("linkNameSnap").toString();
        // 21t-a H2c: absent -> "" = not a group chat (every pre-21t record)
        c.groupId       = obj->getProperty("groupId").toString();
        c.groupName     = obj->getProperty("groupName").toString();
        c.pinnedAt      = obj->getProperty("pinnedAt").toString();
        // B.0: whatever the web stamped. Absent on chats neither client has
        // touched since D1.1, which is why it is only written back when set.
        c.updatedAt     = obj->getProperty("updatedAt").toString();
        c.nextSeq       = (juce::int64) (double) obj->getProperty("_nextSeq");   // stage A

        if (auto* msgs = obj->getProperty("messages").getArray())
        {
            for (auto& m : *msgs)
            {
                if (auto* mObj = m.getDynamicObject())
                {
                    WsMessage msg;
                    msg.role      = mObj->getProperty("role").toString();
                    msg.content   = mObj->getProperty("content").toString();
                    msg.reviewId  = mObj->getProperty("_reviewId").toString();
                    msg.meterCtx  = mObj->getProperty("_meterCtx").toString();
                    msg.chainJson = mObj->getProperty("_chain").toString();
                    msg.figuresJson = mObj->getProperty("_figures").toString();
                    msg.gainJson  = mObj->getProperty("_gain").toString();
                    msg.agentJson = mObj->getProperty("_agent").toString();
                    msg.askJson   = mObj->getProperty("_ask").toString();
                    msg.askAnswered = (bool)mObj->getProperty("_askDone");
                    msg.editJson  = mObj->getProperty("_edit").toString();
                    msg.editApplied = (bool)mObj->getProperty("_editDone");
                    msg.editResult  = mObj->getProperty("_editResult").toString();
                    msg.editAltPrompt = mObj->getProperty("_editAlt").toString();
                    msg.editAltLabel  = mObj->getProperty("_editAltLbl").toString();
                    msg.displayText   = mObj->getProperty("_display").toString();
                    msg.editTargetUid  = mObj->getProperty("_editTgtUid").toString();
                    msg.editTargetName = mObj->getProperty("_editTgtName").toString();
                    // stage A: the entry's place and identity, 0 / empty on every row written before
                    // today - which is what migrateChatLog stamps.
                    msg.seq     = (juce::int64) (double) mObj->getProperty("_seq");
                    msg.entryId = mObj->getProperty("_id").toString();
                    c.messages.push_back(std::move(msg));
                }
            }
        }
    }
    if (const int stamped = migrateChatLog (c); stamped > 0)
        EchoJay_NSLog (("EJWs: chat \"" + (c.title.isNotEmpty() ? c.title : c.id) + "\" migrated to the "
                        "stored log - " + juce::String (stamped) + " row(s) stamped with their place "
                        "and identity, in the order the file already had (stage A of his chat-log "
                        "ruling). Nothing reads them yet.").toRawUTF8());
    return c;
}

WsAlbum EchoJayWorkspace::parseAlbum(const juce::var& v)
{
    WsAlbum a;
    if (auto* obj = v.getDynamicObject())
    {
        a.id      = obj->hasProperty("id")  ? obj->getProperty("id").toString()
                                             : obj->getProperty("_id").toString();
        a.name    = obj->getProperty("name").toString();
        a.created = obj->getProperty("created").toString();

        if (auto* arr = obj->getProperty("chatIds").getArray())
            for (auto& x : *arr)
                a.chatIds.add(x.toString());

        if (auto* arr = obj->getProperty("reviewIds").getArray())
            for (auto& x : *arr)
                a.reviewIds.add(x.toString());

        // Synced key first; fall back to the legacy underscore key so pre-fix
        // data still reads. (Underscore data only exists in old local state,
        // since it was always stripped before the server ever saw it.)
        if (auto* arr = obj->getProperty("projectNames").getArray())
            for (auto& x : *arr)
                a.projectNames.add(x.toString());
        else if (auto* arr = obj->getProperty("_projects").getArray())
            for (auto& x : *arr)
                a.projectNames.add(x.toString());

        a.pinned   = (bool)obj->getProperty("pinned");
        a.pinnedAt = obj->getProperty("pinnedAt").toString();
    }
    return a;
}

WsReview EchoJayWorkspace::parseReview(const juce::var& v)
{
    WsReview r;
    if (auto* obj = v.getDynamicObject())
    {
        r.id          = obj->hasProperty("id")  ? obj->getProperty("id").toString()
                                                 : obj->getProperty("_id").toString();
        r.label       = obj->getProperty("label").toString();
        r.fileName    = obj->getProperty("fileName").toString();
        r.genre       = obj->getProperty("genre").toString();
        r.stemType    = obj->getProperty("stemType").toString();
        r.channelType = obj->getProperty("channelType").toString();
        r.date        = obj->getProperty("date").toString();
        r.audioUrl    = obj->getProperty("audioUrl").toString();
        r.origin      = obj->getProperty("origin").toString();
        r.linkUid     = obj->getProperty("linkUid").toString();   // absent -> "" = main capture
        r.linkNameSnap = obj->getProperty("linkNameSnap").toString();
        r.channelDataScoped = (bool)obj->getProperty("channelDataScoped");
        r.waveform    = obj->getProperty("waveform");  // keep raw

        auto dataVar = obj->getProperty("data");
        if (auto* d = dataVar.getDynamicObject())
        {
            r.data.integ    = (float)d->getProperty("integ");
            r.data.range    = (float)d->getProperty("range");
            r.data.rmsL     = (float)d->getProperty("rmsL");
            r.data.rmsR     = (float)d->getProperty("rmsR");
            r.data.peakL    = (float)d->getProperty("peakL");
            r.data.peakR    = (float)d->getProperty("peakR");
            r.data.tpL      = (float)d->getProperty("tpL");
            r.data.tpR      = (float)d->getProperty("tpR");
            r.data.width    = (float)d->getProperty("width");
            r.data.corr     = (float)d->getProperty("corr");
            r.data.crest    = (float)d->getProperty("crest");
            r.data.dc       = (float)d->getProperty("dc");
            r.data.duration = (float)d->getProperty("duration");
            // Absent key = unavailable — keep the sentinel defaults then
            if (d->hasProperty("psr"))   r.data.psr   = (float)d->getProperty("psr");
            if (d->hasProperty("plr"))   r.data.plr   = (float)d->getProperty("plr");
            if (d->hasProperty("overs")) r.data.overs = (int)d->getProperty("overs");
            if (d->hasProperty("crestSub")) r.data.crestSub = (float)d->getProperty("crestSub");
            if (d->hasProperty("crestMid")) r.data.crestMid = (float)d->getProperty("crestMid");
            if (d->hasProperty("crestTop")) r.data.crestTop = (float)d->getProperty("crestTop");
        }
        if (auto* chArr = obj->getProperty("channels").getArray())
        {
            for (auto& cVar : *chArr)
            {
                if (auto* cObj = cVar.getDynamicObject())
                {
                    WsChannelMeasurements ch;
                    ch.name    = cObj->getProperty("name").toString();
                    ch.wavFile = cObj->getProperty("wavFile").toString();
                    auto cdVar = cObj->getProperty("data");
                    if (auto* cd = cdVar.getDynamicObject())
                    {
                        ch.data.integ    = (float)cd->getProperty("integ");
                        ch.data.range    = (float)cd->getProperty("range");
                        ch.data.rmsL     = (float)cd->getProperty("rmsL");
                        ch.data.rmsR     = (float)cd->getProperty("rmsR");
                        ch.data.peakL    = (float)cd->getProperty("peakL");
                        ch.data.peakR    = (float)cd->getProperty("peakR");
                        ch.data.tpL      = (float)cd->getProperty("tpL");
                        ch.data.tpR      = (float)cd->getProperty("tpR");
                        ch.data.width    = (float)cd->getProperty("width");
                        ch.data.corr     = (float)cd->getProperty("corr");
                        ch.data.crest    = (float)cd->getProperty("crest");
                        ch.data.dc       = (float)cd->getProperty("dc");
                        ch.data.duration = (float)cd->getProperty("duration");
                    }
                    r.channels.push_back(ch);
                }
            }
        }
    }
    return r;
}

WsProfile EchoJayWorkspace::parseProfile(const juce::var& v)
{
    WsProfile p;
    if (auto* obj = v.getDynamicObject())
    {
        p.name       = obj->getProperty("name").toString();
        p.experience = obj->getProperty("experience").toString();
        p.monitors   = obj->getProperty("monitors").toString();
        p.headphones = obj->getProperty("headphones").toString();
        p.plugins    = obj->getProperty("plugins").toString();
        p.genres     = obj->getProperty("genres").toString();

        // "daw" may be an array or a single string
        auto dawVar = obj->getProperty("daw");
        if (auto* arr = dawVar.getArray())
            for (auto& x : *arr)
                p.daw.add(x.toString());
        else if (dawVar.isString() && dawVar.toString().isNotEmpty())
            p.daw.add(dawVar.toString());
    }
    return p;
}

// ============================================================================
// Serialise helpers
// ============================================================================

// Remove top-level keys whose names start with '_' (e.g. _id, __v).
static void stripUnderscoreKeys(juce::DynamicObject& obj)
{
    juce::StringArray toRemove;
    for (auto& prop : obj.getProperties())
        if (prop.name.toString().startsWith("_"))
            toRemove.add(prop.name.toString());
    for (auto& k : toRemove)
        obj.removeProperty(k);
}

WsChat* EchoJayWorkspace::findChatById(const juce::String& id)
{
    if (id.isEmpty()) return nullptr;
    for (auto& c : chats)
        if (c.id == id) return &c;
    return nullptr;
}

bool EchoJayWorkspace::runRoundTripSelfTest()
{
#if ! (JUCE_DEBUG || defined(EJ_SELFTEST))
    // Release: test code must not ship — no work, no stderr. EJ_SELFTEST is
    // defined ONLY by tools/workspace_roundtrip_test/build_and_run.sh (the
    // console harness); no CMake target defines it, so release plugin
    // builds always take this stub.
    //
    // ODR GUARD RAIL: the harness compiles THIS translation unit with
    // EJ_SELFTEST while the rest of the static library was built without
    // it. That is benign ONLY while the define gates a function BODY in a
    // .cpp. EJ_SELFTEST must NEVER appear in a header, and must never gate
    // anything that changes struct layout, class size, or vtable shape —
    // that mismatch would present as random memory corruption at runtime,
    // not a link error. Body-only gating in a .cpp is the sole sanctioned
    // use.
    return true;
#else
    static bool done = false, lastResult = false;
    if (done) return lastResult;
    done = true;

    auto ser = [](const WsChat& c) { return juce::JSON::toString(chatToVar(c), true); };

    WsChat pre;                       // shaped like every pre-C1 record
    pre.id = "t1"; pre.title = "pre-C1"; pre.created = "2026-01-01T00:00:00Z";
    pre.trackName = "Proj"; pre.albumId = "alb"; pre.revisionCount = 2; pre.pinned = true;
    WsMessage m; m.role = "user"; m.content = "hello"; pre.messages.push_back(m);

    WsChat chan = pre;                // identical but a channel chat
    chan.id = "t2"; chan.linkUid = "abc123def0"; chan.linkNameSnap = "Drums";

    const auto s1  = ser(pre);
    const auto s1b = ser(parseChat(juce::JSON::parse(s1)));
    const auto s2  = ser(chan);
    const auto rt2 = parseChat(juce::JSON::parse(s2));
    const auto s2b = ser(rt2);

    const bool preStable  = s1 == s1b;
    const bool preNoKeys  = !s1.contains("linkUid") && !s1.contains("linkNameSnap");
    const bool chanStable = s2 == s2b;
    const bool chanFields = rt2.linkUid == "abc123def0" && rt2.linkNameSnap == "Drums";

    // B.0: a chat neither client has stamped must serialise with NO
    // updatedAt key at all, and a chat the WEB stamped must come back out
    // carrying exactly what the web wrote. The second one is the whole point
    // of the slice: both clients send chats whole, so a field this client
    // does not own has to survive the round trip untouched.
    const bool updNoKey = !s1.contains("updatedAt");
    WsChat webStamped = pre;
    webStamped.id = "t3";
    webStamped.updatedAt = "2026-07-28T09:15:00.123Z";   // as new Date().toISOString()
    const auto s3   = ser(webStamped);
    const auto rt3  = parseChat(juce::JSON::parse(s3));
    const bool updPreserved = rt3.updatedAt == "2026-07-28T09:15:00.123Z"
                           && ser(rt3) == s3;
    // And our own stamp must be in the web's exact shape, not JUCE's
    // local-offset ISO: 24 chars, T at 10, dot at 19, trailing Z.
    const auto stamp = isoUtcNow();
    const bool updFormat = stamp.length() == 24 && stamp[10] == 'T'
                        && stamp[19] == '.' && stamp.endsWith("Z")
                        && !stamp.contains("+");

    // WsReview round-trip (Phase C item 3)
    auto serR = [](const WsReview& r) { return juce::JSON::toString(reviewToVar(r), true); };
    WsReview mainRev;   // pre-C shape: no linkUid
    mainRev.id = "r1"; mainRev.label = "mix v1"; mainRev.fileName = "mix_v1.wav";
    mainRev.channelType = "FullMix"; mainRev.date = "2026-01-01T00:00:00Z";
    WsReview chanRev = mainRev;   // per-channel capture
    chanRev.id = "r2"; chanRev.linkUid = "abc123def0"; chanRev.linkNameSnap = "Vocals";
    chanRev.channelDataScoped = true;
    const auto sr1  = serR(mainRev);
    const auto sr1b = serR(parseReview(juce::JSON::parse(sr1)));
    const auto sr2  = serR(chanRev);
    const auto rrt2 = parseReview(juce::JSON::parse(sr2));
    const bool revMainStable = sr1 == sr1b;
    const bool revMainNoKey  = !sr1.contains("linkUid");
    const bool revChanStable = sr2 == serR(rrt2);
    const bool revChanField  = rrt2.linkUid == "abc123def0" && rrt2.linkNameSnap == "Vocals"
                            && rrt2.channelDataScoped == true;
    const bool revMainNoSnap = !sr1.contains("linkNameSnap")
                            && !sr1.contains("channelDataScoped");

    // Figure-card round-trip: an AI Compare assistant message carries
    // figuresJson, which must survive on the message record (serialised as
    // _figures) so a reloaded chat redraws the card; a message WITHOUT it must
    // serialise with NO _figures key (absent parses as absent, silent migration).
    WsChat figChat = pre; figChat.id = "t4";
    WsMessage fm; fm.role = "assistant"; fm.content = "different sources";
    fm.figuresJson = "{\"a\":{\"label\":\"X\",\"int\":-14.2},\"b\":{\"label\":\"Y\"},\"cross\":true}";
    figChat.messages.push_back(fm);
    const auto sf  = ser(figChat);
    const auto rtf = parseChat(juce::JSON::parse(sf));
    const bool figRoundTrip = rtf.messages.size() == 2
                           && rtf.messages[1].figuresJson == fm.figuresJson
                           && ser(rtf) == sf;
    const bool figNoKey = !s1.contains("_figures");   // plain pre-C chat carries none


    lastResult = preStable && preNoKeys && chanStable && chanFields
              && revMainStable && revMainNoKey && revChanStable && revChanField && revMainNoSnap
              && updNoKey && updPreserved && updFormat
              && figRoundTrip && figNoKey;
    std::fprintf(stderr,
        "EJWorkspace selftest: %s (preStable=%d preNoKeys=%d chanStable=%d chanFields=%d "
        "revMainStable=%d revMainNoKey=%d revChanStable=%d revChanField=%d revMainNoSnap=%d "
        "updNoKey=%d updPreserved=%d updFormat=%d figRoundTrip=%d figNoKey=%d)\n",
        lastResult ? "PASS" : "FAIL",
        (int)preStable, (int)preNoKeys, (int)chanStable, (int)chanFields,
        (int)revMainStable, (int)revMainNoKey, (int)revChanStable, (int)revChanField,
        (int)revMainNoSnap,
        (int)updNoKey, (int)updPreserved, (int)updFormat,
        (int)figRoundTrip, (int)figNoKey);
    return lastResult;
#endif
}

juce::var EchoJayWorkspace::chatToVar(const WsChat& c)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("id",            c.id);
    obj->setProperty("title",         c.title);
    obj->setProperty("created",       c.created);
    obj->setProperty("trackName",     c.trackName);
    obj->setProperty("albumId",       c.albumId);
    obj->setProperty("revisionCount", c.revisionCount);
    obj->setProperty("pinned",        c.pinned);   // must survive the sync round-trip
    if (c.pinnedAt.isNotEmpty())
        obj->setProperty("pinnedAt",  c.pinnedAt);
    // B.0: written back whether WE stamped it or the web did. Only when
    // non-empty, so a never-stamped chat serialises exactly as before.
    if (c.updatedAt.isNotEmpty())
        obj->setProperty("updatedAt", c.updatedAt);
    // Phase C1: written only when non-empty — a main chat's serialised
    // form is byte-identical to pre-C1 (silent migration guarantee).
    if (c.linkUid.isNotEmpty())
        obj->setProperty("linkUid",      c.linkUid);
    if (c.linkNameSnap.isNotEmpty())
        obj->setProperty("linkNameSnap", c.linkNameSnap);
    // 21t-a H2c: written only when set, so a non-group chat is byte-identical
    if (c.groupId.isNotEmpty())
        obj->setProperty("groupId",   c.groupId);
    if (c.groupName.isNotEmpty())
        obj->setProperty("groupName", c.groupName);
    if (c.nextSeq > 0) obj->setProperty("_nextSeq", (double) c.nextSeq);   // stage A: the allocator

    juce::Array<juce::var> msgs;
    for (auto& m : c.messages)
    {
        auto* mObj = new juce::DynamicObject();
        mObj->setProperty("role",    m.role);
        mObj->setProperty("content", m.content);
        // ---- STAGE A OF THE CHAT LOG (his ruling of 21 Oct) -----------------------------------
        // The entry's own place and its own identity, written only when stamped, so a reader that
        // does not know them sees exactly today's chat. Nothing READS them yet: stage C does.
        if (m.seq > 0)             mObj->setProperty("_seq", (double) m.seq);
        if (m.entryId.isNotEmpty()) mObj->setProperty("_id",  m.entryId);
        if (m.reviewId.isNotEmpty())
            mObj->setProperty("_reviewId",  m.reviewId);
        if (m.meterCtx.isNotEmpty())
            mObj->setProperty("_meterCtx",  m.meterCtx);
        if (m.chainJson.isNotEmpty())
            mObj->setProperty("_chain",     m.chainJson);
        if (m.figuresJson.isNotEmpty())
            mObj->setProperty("_figures",   m.figuresJson);
        if (m.gainJson.isNotEmpty())
            mObj->setProperty("_gain",      m.gainJson);
        // THE RUN'S CARD (18 Oct 2026). Additive: a reader that does not know `_agent` sees the
        // prose in `content` and draws the bubble it always drew.
        if (m.agentJson.isNotEmpty())
            mObj->setProperty("_agent",     m.agentJson);
        if (m.askJson.isNotEmpty())
        {
            mObj->setProperty("_ask",       m.askJson);
            if (m.askAnswered)
                mObj->setProperty("_askDone", true);
        }
        if (m.editJson.isNotEmpty())
        {
            mObj->setProperty("_edit",      m.editJson);
            if (m.editApplied)
                mObj->setProperty("_editDone", true);
            if (m.editResult.isNotEmpty())
                mObj->setProperty("_editResult", m.editResult);
        }
        // _editAlt is independent of _edit: build RESULT BUBBLES (plain
        // messages) carry the Suggest-an-alternative prompt too
        if (m.editAltPrompt.isNotEmpty())
            mObj->setProperty("_editAlt", m.editAltPrompt);
        if (m.editAltLabel.isNotEmpty())
            mObj->setProperty("_editAltLbl", m.editAltLabel);
        if (m.displayText.isNotEmpty())
            mObj->setProperty("_display", m.displayText);
        if (m.editTargetUid.isNotEmpty())
            mObj->setProperty("_editTgtUid", m.editTargetUid);
        if (m.editTargetName.isNotEmpty())
            mObj->setProperty("_editTgtName", m.editTargetName);
        msgs.add(juce::var(mObj));
    }
    obj->setProperty("messages", juce::var(msgs));
    return juce::var(obj);
}

juce::var EchoJayWorkspace::albumToVar(const WsAlbum& a)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("id",      a.id);
    obj->setProperty("name",    a.name);
    obj->setProperty("created", a.created);

    juce::Array<juce::var> cids, rids, projs;
    for (auto& x : a.chatIds)      cids.add(x);
    for (auto& x : a.reviewIds)    rids.add(x);
    for (auto& x : a.projectNames) projs.add(x);
    obj->setProperty("chatIds",   juce::var(cids));
    obj->setProperty("reviewIds", juce::var(rids));
    // Project→album membership. MUST be a plain (non-underscore) key so the
    // POST's stripUnderscoreKeys does NOT drop it — otherwise the server never
    // stores it, the next sync GET rebuilds albums without it, and the move is
    // lost (collapse/tab/reopen). Same discipline as chat "pinned".
    if (!projs.isEmpty())
        obj->setProperty("projectNames", juce::var(projs));
    // Pin state — plain keys so the POST keeps them and they reach the web app.
    obj->setProperty("pinned",   a.pinned);
    if (a.pinnedAt.isNotEmpty())
        obj->setProperty("pinnedAt", a.pinnedAt);
    return juce::var(obj);
}

juce::var EchoJayWorkspace::reviewToVar(const WsReview& r)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("id",          r.id);
    obj->setProperty("label",       r.label);
    obj->setProperty("fileName",    r.fileName);
    obj->setProperty("genre",       r.genre);
    obj->setProperty("stemType",    r.stemType);
    obj->setProperty("channelType", r.channelType);
    obj->setProperty("date",        r.date);
    obj->setProperty("audioUrl",    r.audioUrl);
    obj->setProperty("origin",      r.origin);
    if (r.linkUid.isNotEmpty())
        obj->setProperty("linkUid", r.linkUid);   // main captures stay byte-identical
    if (r.linkNameSnap.isNotEmpty())
        obj->setProperty("linkNameSnap", r.linkNameSnap);
    if (r.channelDataScoped)
        obj->setProperty("channelDataScoped", true);
    obj->setProperty("waveform",    r.waveform);

    auto* d = new juce::DynamicObject();
    d->setProperty("integ",    r.data.integ);
    d->setProperty("range",    r.data.range);
    d->setProperty("rmsL",     r.data.rmsL);
    d->setProperty("rmsR",     r.data.rmsR);
    d->setProperty("peakL",    r.data.peakL);
    d->setProperty("peakR",    r.data.peakR);
    d->setProperty("tpL",      r.data.tpL);
    d->setProperty("tpR",      r.data.tpR);
    d->setProperty("width",    r.data.width);
    d->setProperty("corr",     r.data.corr);
    d->setProperty("crest",    r.data.crest);
    d->setProperty("dc",       r.data.dc);
    d->setProperty("duration", r.data.duration);
    // Phase-1/2a metering additions — written only when available (sentinel
    // defaults are omitted; absent key = unavailable, shared v1 convention)
    if (r.data.psr   > -900.0f) d->setProperty("psr",   r.data.psr);
    if (r.data.plr   > -900.0f) d->setProperty("plr",   r.data.plr);
    if (r.data.overs >= 0)      d->setProperty("overs", r.data.overs);
    if (r.data.crestSub >= 0.0f) d->setProperty("crestSub", r.data.crestSub);
    if (r.data.crestMid >= 0.0f) d->setProperty("crestMid", r.data.crestMid);
    if (r.data.crestTop >= 0.0f) d->setProperty("crestTop", r.data.crestTop);
    obj->setProperty("data", juce::var(d));
    if (!r.channels.empty())
    {
        juce::Array<juce::var> chArr;
        for (auto& ch : r.channels)
        {
            auto* cObj  = new juce::DynamicObject();
            cObj->setProperty("name", ch.name);
            if (ch.wavFile.isNotEmpty())
                cObj->setProperty("wavFile", ch.wavFile);
            auto* cd = new juce::DynamicObject();
            cd->setProperty("integ",    ch.data.integ);
            cd->setProperty("range",    ch.data.range);
            cd->setProperty("rmsL",     ch.data.rmsL);
            cd->setProperty("rmsR",     ch.data.rmsR);
            cd->setProperty("peakL",    ch.data.peakL);
            cd->setProperty("peakR",    ch.data.peakR);
            cd->setProperty("tpL",      ch.data.tpL);
            cd->setProperty("tpR",      ch.data.tpR);
            cd->setProperty("width",    ch.data.width);
            cd->setProperty("corr",     ch.data.corr);
            cd->setProperty("crest",    ch.data.crest);
            cd->setProperty("dc",       ch.data.dc);
            cd->setProperty("duration", ch.data.duration);
            cObj->setProperty("data", juce::var(cd));
            chArr.add(juce::var(cObj));
        }
        obj->setProperty("channels", juce::var(chArr));
    }
    return juce::var(obj);
}

juce::var EchoJayWorkspace::profileToVar(const WsProfile& p)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("name",       p.name);
    obj->setProperty("experience", p.experience);
    obj->setProperty("monitors",   p.monitors);
    obj->setProperty("headphones", p.headphones);
    obj->setProperty("plugins",    p.plugins);
    obj->setProperty("genres",     p.genres);

    juce::Array<juce::var> dawArr;
    for (auto& d : p.daw) dawArr.add(d);
    obj->setProperty("daw", juce::var(dawArr));
    return juce::var(obj);
}

// ============================================================================
// Build clean POST body
// ============================================================================

juce::String EchoJayWorkspace::buildPostBody() const
{
    auto* root = new juce::DynamicObject();

    // --- chats: drop any with no messages ---
    juce::Array<juce::var> chatArr;
    for (auto& c : chats)
    {
        if (c.messages.empty()) continue;
        auto v = chatToVar(c);
        if (auto* obj = v.getDynamicObject()) stripUnderscoreKeys(*obj);
        chatArr.add(v);
    }
    root->setProperty("chats", juce::var(chatArr));

    // --- albums ---
    juce::Array<juce::var> albumArr;
    for (auto& a : albums)
    {
        auto v = albumToVar(a);
        if (auto* obj = v.getDynamicObject()) stripUnderscoreKeys(*obj);
        albumArr.add(v);
    }
    root->setProperty("albums", juce::var(albumArr));

    // --- reviews: sort newest-first by date string, cap at 50 ---
    // ISO-8601 dates sort correctly as strings. We sort a copy of the index.
    std::vector<size_t> idx(reviews.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [this](size_t a, size_t b) {
        return reviews[a].date > reviews[b].date; // descending
    });

    juce::Array<juce::var> revArr;
    int limit = juce::jmin((int)idx.size(), 50);
    for (int i = 0; i < limit; ++i)
    {
        auto v = reviewToVar(reviews[idx[(size_t)i]]);
        if (auto* obj = v.getDynamicObject()) stripUnderscoreKeys(*obj);
        revArr.add(v);
    }
    root->setProperty("reviews", juce::var(revArr));

    // --- pinned projects (songs) — synced list (plain keys) ---
    juce::Array<juce::var> pinArr;
    for (auto& p : pinnedProjects)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("name",     p.name);
        o->setProperty("pinnedAt", p.pinnedAt);
        pinArr.add(juce::var(o));
    }
    root->setProperty("pinnedProjects", juce::var(pinArr));

    // --- profile ---
    auto pv = profileToVar(profile);
    if (auto* obj = pv.getDynamicObject()) stripUnderscoreKeys(*obj);
    root->setProperty("profile", pv);

    return juce::JSON::toString(juce::var(root), true);
}
