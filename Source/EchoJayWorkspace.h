#pragma once
#include <JuceHeader.h>
#include "EchoJayAPI.h"
#include <functional>
#include <memory>
#include <atomic>
#include <vector>

// ---------------------------------------------------------------------------
// Workspace data structs — parsed from GET /api/data
// ---------------------------------------------------------------------------

struct WsMessage {
    juce::String role;
    juce::String content;
    juce::String reviewId;   // non-empty on capture-review user messages; serialised as _reviewId
    juce::String meterCtx;   // optional meter snapshot text; serialised as _meterCtx
    juce::String chainJson;  // non-empty on assistant chain replies; serialised as _chain
    juce::String gainJson;   // non-empty on assistant gain-proposal replies; serialised
                             // as _gain. Carries the proposals AND their applied/prevGain
                             // state (mutated on Apply/Undo) so cards reload correctly.
    /** ---- THE RUN'S CARD, SO A REOPENED CHAT SHOWS WHAT WAS ON SCREEN (18 Oct 2026) --------
        Sean: *"when I reopen a chat, an agent run comes back as just its last message in a plain
        bubble. It should come back exactly as it looked live: the ASK card with every step, the
        'You said' lines, the read-back lines, the red failures, Details, Hide steps, and the
        closing."*

        This is echojay::agentui::toJson of the finished run - the same snapshot the row has always
        kept on ChatMsg::agentData and the processor's replay list, which until now died with the
        window. Serialised as `_agent`, written only when non-empty, so a build that does not know
        the key reads the message exactly as it does today: the prose, in a bubble. */
    juce::String agentJson;
    juce::String askJson;    // non-empty on assistant ASK replies (question + tappable
                             // choices, Phase 1b); serialised as _ask
    bool askAnswered = false; // chip tapped (or question superseded); serialised as _askDone
    juce::String editJson;   // non-empty on assistant CHAIN_EDIT replies (Phase 1c);
                             // serialised as _edit
    bool editApplied = false;   // Apply pressed; serialised as _editDone
    juce::String editResult; // outcome summary after apply; serialised as _editResult
    juce::String editAltPrompt; // "Suggest an alternative" follow-up text, set only
                                // when an op failed as a plugin LOAD failure;
                                // serialised as _editAlt, cleared once tapped
    juce::String editAltLabel;  // display label for that tap; serialised as _editAltLbl
    juce::String displayText;   // tap-generated user turns: rendered text
                                // (content = sent text); serialised as _display
    juce::String editTargetUid;  // Phase R: non-empty when the edit card targets a
                                 // LINK's rack (v:2 send, sidecar-revision guard) —
                                 // persisted so a reloaded card can never mis-route
                                 // to the local rack; serialised as _editTgtUid
    juce::String editTargetName; // display label at propose time; _editTgtName
    juce::String figuresJson;    // AI Compare figure card data (both sources'
                                 // figures + labels + cross flag); serialised as
                                 // _figures. Absent on every non-compare message
                                 // and every pre-card message (silent migration).

    /** ---- STAGE A OF THE CHAT LOG (his ruling of 21 Oct; docs/PLAN_CHAT_LOG_2026-10-21.md) ----

        *"One stored log per chat, where every entry (user message, agent card, notice, Stopped line)
        gets its chat id, a monotonic order number and a stable id at the moment it's created. The
        view only renders that log in order."*

        TWO FIELDS, AND THEY ARE WRITTEN AND NOT YET READ. Stage A+B are a SECOND WRITER by his
        approval of the staging: both logs exist, the new one is filled, and nothing renders from it
        until C switches the reader over and deletes the merge. That is what makes the switch
        testable on his own chats rather than on a fixture.

          seq   the entry's place in its chat, allocated by the store (WsChat::nextSeq) when the row
                is created and NEVER renumbered. The view will sort by it; "insert into its own
                place" stops existing, which is the order fault (his 14:15 and 17:3x) at the root.
          id    stable, created with the row. For an agent card it is the RUN'S KEY, which already
                exists before any sessionId does and survives a reopen - so the live run updates its
                own entry by id instead of being re-identified by a per-window integer the replay
                reissues (his 14:15 chips).

        Serialised as `_seq` and `_id`, written only when set, so a build that does not know them
        reads the chat exactly as it does today. 0 / empty means "not stamped yet", which is what
        every row written before today holds until the migration stamps it. */
    juce::int64  seq = 0;
    juce::String entryId;
};

/** ---- THE LOG'S ORDER, IN ONE PLACE (his 21:2x, 21 Oct 2026) ------------------------------
    A log is rendered by this and nothing else. TWO RULES:

      1. a placed entry sorts by its `seq`;
      2. an UNPLACED entry (seq 0) sorts LAST.

    Rule 2 is his 21:22 fault. I had written that an unplaceable row "is given its place at the end",
    and the render line in his log proved that path never ran: the row went in with seq 0, and 0
    sorts FIRST, so his own message ended up underneath the card it had started. An unplaced row is
    the newest thing that happened, so last is both true and the benign failure mode - above the
    whole conversation is the inverted one.

    Shared by the editor's reader and chat_log_guard, so the rule cannot be stated twice and drift. */
inline bool chatLogBefore (juce::int64 aSeq, juce::int64 bSeq)
{
    if (aSeq <= 0 && bSeq <= 0) return false;          // both unplaced: arrival order (stable sort)
    if (aSeq <= 0) return false;                        // a is unplaced: it goes after b
    if (bSeq <= 0) return true;                         // b is unplaced: a goes first
    return aSeq < bSeq;
}

struct WsChat {
    juce::String id;
    juce::String title;
    std::vector<WsMessage> messages;
    /** THE SEQ ALLOCATOR for this chat (stage A, his chat-log ruling). One per chat, held by the
        store, so two windows appending at once cannot mint the same number - which is the "two
        windows open" case his plan asks for. Serialised as `_nextSeq`; 0 means "not migrated yet"
        and the loader stamps it past the last row it finds. */
    juce::int64 nextSeq = 0;
    juce::String created;
    juce::String trackName;
    juce::String albumId;
    int revisionCount = 0;
    bool pinned = false;       // shown in the PINNED sidebar group
    juce::String pinnedAt;     // ISO timestamp — orders the group (newest first)
    // ---- Phase C1: channel chats ----
    // linkUid: "" = main chat (every pre-C1 record parses to this and
    // behaves exactly as before — silent migration, no version bump).
    // Non-empty = this chat IS a Link channel's conversation; the uid is
    // THE key (persisted in the Link's plugin state, survives project
    // reopen) and is never displayed. Serialised only when non-empty, so
    // a linkUid-less chat round-trips byte-identically.
    // linkNameSnap: channel display name at last message time (Phase N
    // precedence result). DISPLAY ONLY — offline rows, web fallback, the
    // future adopt-by-name repair. Refreshed on each send while live.
    juce::String linkUid;
    juce::String linkNameSnap;
    // ---- 21t-a H2c (25 Sep 2026): group chats ----
    // A chat started while a GROUP is the chat target belongs to that group,
    // not to any one Link and not to Main. A group is NOT a channel, so it
    // does not borrow linkUid: a group id would then be read as a rack
    // identity by every uid consumer (borrow, banner, orphan repair). Two
    // own fields instead, with linkUid's exact migration guarantee -
    // serialised only when non-empty, so every chat that has never been on a
    // group round-trips byte-identically.
    // groupId: the LinkGroup id (stable, never displayed).
    // groupName: the group's display name at the time of writing (display
    // only, same role linkNameSnap plays for a channel).
    juce::String groupId;
    juce::String groupName;
    // ---- Session B.0: last activity ----
    // The dashboard orders recentChats by `updatedAt || created`
    // (lib/dash/adapters.js activityKey), and the web client has stamped it on
    // every chat mutation since D1.1. The plugin sends chats WHOLE, so until
    // now a plugin save silently dropped whatever the web wrote and those
    // chats fell back to creation order.
    //
    // Two rules, both load-bearing:
    //  - PRESERVE: parsed from the server and written back untouched for any
    //    chat the plugin did not modify.
    //  - SERIALISE ONLY WHEN NON-EMPTY, so a chat that has never been stamped
    //    by either client round-trips byte-identically (the same silent
    //    migration guarantee linkUid has).
    // Format is exactly JavaScript's new Date().toISOString(): UTC,
    // milliseconds, trailing Z. See isoUtcNow() in the .cpp.
    juce::String updatedAt;
};

struct WsAlbum {
    juce::String id;
    juce::String name;
    juce::String created;
    juce::StringArray chatIds;
    juce::StringArray reviewIds;
    // Projects (songs, = chat.trackName) assigned to this album. Serialised as
    // the synced "projectNames" key. Additive: legacy albums have none; the
    // sidebar also derives membership from chatIds' trackNames so existing
    // albums stay populated.
    juce::StringArray projectNames;
    bool pinned = false;         // shown in the flat PINNED sidebar group
    juce::String pinnedAt;       // ISO timestamp — orders the group (newest first)
};

// A pinned song (project). Songs are not first-class objects (a song is just a
// chat.trackName), so pinned songs live in a workspace-level synced list.
struct WsPinnedProject {
    juce::String name;
    juce::String pinnedAt;
};

struct WsMeasurements {
    float integ    = 0.f, range    = 0.f;
    float rmsL     = 0.f, rmsR     = 0.f;
    float peakL    = 0.f, peakR    = 0.f;
    float tpL      = 0.f, tpR      = 0.f;
    float width    = 0.f, corr     = 0.f;
    float crest    = 0.f, dc       = 0.f;
    float duration = 0.f;
    // Phase-1/2a metering additions. Sentinel defaults mean "unavailable"
    // (review stored by an older build, or inputs never became valid) —
    // serialisation OMITS the keys in that case, per the shared v1
    // convention that an absent key = unavailable. A real overs count of
    // 0 is a valid measurement and IS written.
    float psr   = -999.0f;
    float plr   = -999.0f;
    int   overs = -1;
    float crestSub = -1.0f, crestMid = -1.0f, crestTop = -1.0f;
};

struct WsChannelMeasurements {
    juce::String   name;
    WsMeasurements data;
    juce::String   wavFile;  // filename only; empty until saved
};

struct WsReview {
    juce::String id;
    juce::String label;      // display name set at capture time (e.g. "test v1")
    juce::String fileName;
    juce::String genre;
    juce::String stemType;
    juce::String channelType;
    juce::String date;
    juce::String audioUrl;
    juce::String origin;
    juce::var    waveform;      // kept as raw var (may be array or string)
    // Phase C: the Link channel this capture reviewed. "" = a MAIN capture
    // (mix bus + all Links, or a host-only review) — every pre-C review
    // parses to this. Non-empty = a per-channel capture of that Link, so a
    // reloaded channel chat re-associates its own captures. Serialised only
    // when non-empty; absent parses as "" (silent migration, no bump).
    juce::String linkUid;
    // Phase C item 3: channel display name at capture time (Phase N
    // precedence result). DISPLAY ONLY - a capture of a channel you closed
    // an hour ago still labels correctly once resolveLinkDisplayName()
    // returns empty. Serialised only when non-empty; the uid stays the key.
    juce::String linkNameSnap;
    // Item 2 marker: TRUE only when `data` is genuinely this channel's own
    // measurements (routed from snap.channels), never the host full-mix.
    // A review with linkUid but NO marker is a PRE-FIX record whose numbers
    // are the full capture -> the Compare list labels it "Full capture",
    // which is true of its numbers. Serialised only when true; absent parses
    // false (silent migration). Existing records are never rewritten.
    bool channelDataScoped = false;
    WsMeasurements data;
    std::vector<WsChannelMeasurements> channels;  // empty for single-channel (host-only) reviews

    // In-memory only — NOT persisted to the server.
    // Populated in createReviewFromCapture() from the source CaptureSnapshot's avgSpectrum
    // so that the Compare-page spectrum panel can show stored data for WsCapture slots.
    std::array<float, 64> spectrumBands = {};
    bool hasSpectrum = false;
    // In-memory only (same lifetime as spectrumBands, session-cached across a
    // merge). The pink-referenced per-octave macro bands, so the Compare
    // context can report per-source band relatives (band vs the band average).
    // -120 = no data; sub, low, lowMid, mid, highMid, air.
    std::array<float, 6> macroBandDb = { -120, -120, -120, -120, -120, -120 };
    bool hasMacroBands = false;
};

struct WsProfile {
    juce::String name;
    juce::StringArray daw;
    juce::String experience;
    juce::String monitors;
    juce::String headphones;
    juce::String plugins;
    juce::String genres;
};

// ---------------------------------------------------------------------------
// EchoJayWorkspace
//
// Owns the parsed workspace state pulled from GET /api/data.
// All public methods must be called from the message thread.
//
// Phase-1 behaviour:
//   requestLoad() — GET /api/data, parse into structs, log counts, fire onLoaded.
//   requestSync() — debounced 1 s: GET latest, merge by id (phase-1: trivial,
//                   no local writes yet), apply clean rules, POST.
// ---------------------------------------------------------------------------
class EchoJayWorkspace : private juce::Timer
{
public:
    explicit EchoJayWorkspace(EchoJayAPI& api);
    ~EchoJayWorkspace() override;

    // Trigger a load now. No-op if not logged in or a load is already in
    // flight. Safe to call redundantly (on plugin open, tab-focus events).
    void requestLoad();

    // Schedule a debounced sync (~1 s). The sync does GET → merge → POST.
    void requestSync();

    // Mutation helpers — update in-memory state then call requestMutationSync()
    // to persist (POST first so the change survives the following GET).
    void addChat(WsChat chat);         // prepend to chats list

    /** ---- EVERY CHAT IS FILED, WHOEVER MINTED IT (18 Oct 2026, Sean's rule 1) --------------
        *"Every chat belongs to ONE project and ONE track: Main (the main rack) or a Link (by
        uid). A new chat is filed under the open project, in that track's folder, dated today.
        Never Ungrouped while a project is open. Just now, a new chat on the Mix Bus in 'project
        nafe aitch' went to Ungrouped."*

        The store cannot know the open project or the viewed channel, so the editor hands it this.
        appendMessageToChat used to mint a chat with NO trackName, albumId or linkUid when it was
        handed an id it did not know - a chat with no project (Ungrouped) and no target, which is
        exactly what he read. Now there is ONE filing rule and both creators go through it. */
    std::function<void (WsChat&)> fileNewChat;

    /** ---- THE ONE-OFF CLEAN-UP (18 Oct 2026, Sean's rule 4) --------------------------------
        Repairs what the 14:53 build wrote: a chat with no project adopts the one that is open,
        and an assistant message that repeats another one in the SAME chat (the em-dash twin) is
        dropped. Returns how many chats were refiled and how many duplicates went, so the log can
        say it rather than imply it. It does NOT guess at messages copied BETWEEN chats: nothing in
        the record distinguishes a copy from a legitimate answer, and deleting on a guess is worse
        than the mess. Those are named in the log for a human decision. */
    struct Repair { int refiled = 0, duplicatesDropped = 0, crossChatSuspects = 0; };
    Repair repairChatFiling (const juce::String& openProject, const juce::String& albumId);
    /** ---- STAGE A: THE ONE PLACE AN ENTRY IS GIVEN ITS PLACE AND ITS NAME ------------------
        Called by every append before the row goes into the chat. `seq` comes from the chat's own
        allocator (so two windows cannot collide) and `id` is the RUN'S KEY for an agent card - which
        exists before any sessionId and survives a reopen - or a minted one for anything else.

        Public and static-shaped so chat_log_guard can drive it without a store on disk. */
    static void stampLogEntry (WsChat& chat, WsMessage& m);

    /** ---- STAGE A: MIGRATION (his plan's fifth case) --------------------------------------
        Every chat written before today has rows with no seq and no id. THE FILE ORDER IS THE
        HISTORICAL TRUTH, so `seq` is assigned in the order the file already has, an id is minted per
        row (the run key where a card carries one), and the allocator is set past the last. Returns
        how many rows it stamped, so the caller can log one line per chat and write it back once.

        Idempotent: a chat whose rows are already stamped is left alone and returns 0. */
    static int migrateChatLog (WsChat& chat);

    void addAlbum(WsAlbum album);      // append to albums list
    // Project (song) → album assignment. Removes the project from every
    // album's projectNames first, then adds it to albumId (empty = orphan).
    void assignProjectToAlbum(const juce::String& projectName, const juce::String& albumId);
    // Create a named album and return its id (message thread).
    juce::String createAlbumWithName(const juce::String& name);
    // Rename a project (song): set trackName on all its chats and move any
    // album membership (projectNames) to the new name.
    void renameProject(const juce::String& oldName, const juce::String& newName);
    void addReview(WsReview review);   // prepend to reviews list, caps at 50
    // Phase C handshake: set a review's WAV filename once the background save
    // finished writing it (the filename carries a save-time timestamp, so it
    // is only known then). Optional waveform var. No-op if the review is gone
    // or already has the filename. Returns true if it changed something.
    bool updateReviewAudio(const juce::String& reviewId,
                           const juce::String& fileName,
                           const juce::var& waveform = juce::var());
    // Item 4: delete reviews by id (orphan purge). User-initiated, confirmed.
    void deleteReviews(const juce::StringArray& ids);
    void moveChatToAlbum(const juce::String& chatId, const juce::String& albumId);
    void removeChatFromAlbum(const juce::String& chatId);
    void removeChat(const juce::String& chatId);   // remove chat + unlink from any album
    void setAlbumName(const juce::String& albumId, const juce::String& name);
    void removeAlbum(const juce::String& albumId); // remove album; its chats become ungrouped

    // Message-level mutations — used by the send path.
    // Returns true if this is the first message in the chat (for auto-title).
    // If no chat with chatId exists, a new one is created on the fly.
    // reviewId is optional — pass it on capture-review user messages to tag them.
    bool appendMessageToChat(const juce::String& chatId,
                             const juce::String& role,
                             const juce::String& content,
                             const juce::String& reviewId  = juce::String(),
                             const juce::String& chainJson = juce::String(),
                             const juce::String& gainJson  = juce::String(),
                             const juce::String& askJson   = juce::String(),
                             const juce::String& editJson  = juce::String(),
                             const juce::String& altPrompt = juce::String(),
                             const juce::String& altLabel  = juce::String(),
                             const juce::String& displayText = juce::String(),
                             const juce::String& editTargetUid  = juce::String(),
                             const juce::String& editTargetName = juce::String(),
                             const juce::String& figuresJson    = juce::String(),
                             const juce::String& agentJson      = juce::String());
    /** The run's card, rewritten in place when a later round adds to it - matched on the visible
        text, the same rule updateAssistantText follows. */
    void updateAssistantAgentJson (const juce::String& chatId,
                                   const juce::String& matchContent,
                                   const juce::String& agentJson);
    /** ---- THE SAME CARD, FOUND BY ITS OWN ID (22 Oct 2026, his 09:24 fault) ----------------
        A card's identity is the RUN'S KEY (stage A), and that is the only handle that works while
        the run is still going: its prose arrives at the end, so a content match cannot find a row
        that was appended when the run began. Returns false when this chat has no row with that id,
        which is how the caller knows to append instead.

        `content` is written only when it is not empty, so a round that has not produced the closing
        yet cannot blank a sentence that is already stored. */
    bool updateAgentCardById (const juce::String& chatId, const juce::String& entryId,
                              const juce::String& agentJson, const juce::String& content);
    /** Does that chat's stored log already hold this exact row? (his item 3, 26 Oct 2026) The
        homing road for a plain message asks this before appending, because the send already syncs
        the user's words as they are taken and a second copy of his sentence is its own fault. */
    bool chatHoldsMessage (const juce::String& chatId, const juce::String& role,
                           const juce::String& content) const;
    // Update the persisted gain-proposal block for the assistant message
    // whose visible content matches (the display list can hold transient
    // messages that never reached the store, so content match beats index).
    // Used when a card's applied/undone state changes so it survives reload.
    void updateAssistantGainJson(const juce::String& chatId,
                                 const juce::String& matchContent,
                                 const juce::String& gainJson);
    // Mark an assistant ASK message answered (chip tapped) — same
    // content-match rule as updateAssistantGainJson, so the disabled state
    // survives reload.
    void markAskAnswered(const juce::String& chatId,
                         const juce::String& matchContent);
    // 21t-j (a): REWRITE an assistant message's text in place - the settle's one line, completed. Same
    // content-match rule as the calls above, so the stored copy follows the display and a reload shows the
    // finished line rather than the promise.
    void updateAssistantText(const juce::String& chatId,
                             const juce::String& matchContent,
                             const juce::String& newContent);
    // Mark an assistant CHAIN_EDIT message applied and store the outcome
    // summary (Phase 1c) — same content-match rule.
    // Clear a persisted Suggest-an-alternative prompt on ANY assistant
    // message (edit cards and plain result bubbles alike) - one-shot pill.
    void clearEditAltPrompt(const juce::String& chatId,
                            const juce::String& matchContent);
    void markEditApplied(const juce::String& chatId,
                         const juce::String& matchContent,
                         const juce::String& resultSummary,
                         const juce::String& altPrompt = juce::String(),
                         const juce::String& altLabel  = juce::String());
    void setChatTitle(const juce::String& chatId, const juce::String& title);
    void setChatTrackName(const juce::String& chatId, const juce::String& trackName);
    void setChatPinned(const juce::String& chatId, bool pinned); // stamps pinnedAt
    void setAlbumPinned(const juce::String& albumId, bool pinned);      // stamps pinnedAt
    void setProjectPinned(const juce::String& projectName, bool pinned); // stamps pinnedAt
    bool isAlbumPinned(const juce::String& albumId) const;
    bool isProjectPinned(const juce::String& projectName) const;
    juce::String albumPinnedAt(const juce::String& albumId) const;
    juce::String projectPinnedAt(const juce::String& projectName) const;
    const std::vector<WsPinnedProject>& getPinnedProjects() const { return pinnedProjects; }
    void incrementChatRevisionCount(const juce::String& chatId);

    // Persist a local mutation: POST current state immediately, then schedule
    // a background sync so we pull any concurrent web-app edits.
    void requestMutationSync();

    // Parsed data accessors — message thread only.
    const std::vector<WsChat>&   getChats()   const { return chats;   }
    WsChat* findChatById(const juce::String& id);   // nullptr when absent
    /** ---- STAGE B: THE ONE ALLOCATOR, ASKED BY BOTH ROADS (his chat-log ruling) -------------
        Hands out the next place in that chat and advances it. The processor's own stamper calls
        this through a function it is given, so the store remains the only allocator - two of them
        would be two sources of truth for one order, which is the fault this round removes.

        A chat this store does not know yet returns 0, which the caller reads as "not stamped":
        a guessed place is worse than none, and the row is stamped when its chat exists. */
    juce::int64 takeNextSeq (const juce::String& chatId);
    /** ---- THE STAMP THE STORE JUST GAVE A ROW (the single stamp, his 21:2x) ----------------
        The editor appends to the STORE first - which creates the chat when it is new, so the
        allocator has something to allocate from - and then hands the SAME place and id to the
        processor's entry. This is how it reads them back. FALSE when the chat or the row is not
        there, and then the caller leaves its copies unstamped rather than inventing a pair. */
    bool lastStampOf (const juce::String& chatId, juce::int64& seqOut, juce::String& idOut) const;
    // Round-trip self-assert (Phase C1/C3 verification): serialise->parse->
    // serialise must be byte-stable for a pre-C1 chat (and emit NO linkUid
    // keys for it) and for a channel chat (fields intact). Runs once per
    // process from the processor ctor; result to stderr + return value.
    static bool runRoundTripSelfTest();
    const std::vector<WsAlbum>&  getAlbums()  const { return albums;  }
    const std::vector<WsReview>& getReviews() const { return reviews; }
    const WsProfile&             getProfile() const { return profile; }

    enum class LoadState { Idle, Loading, Loaded, Error };
    LoadState getLoadState() const { return loadState; }
    int       getLastStatus() const { return lastStatus; }

    // Fired on the message thread after each load attempt (success or fail).
    std::function<void()> onLoaded;

private:
    void timerCallback() override;
    void doLoad();
    void doSync();

    // Parse helpers
    static WsChat       parseChat   (const juce::var& v);
    static WsAlbum      parseAlbum  (const juce::var& v);
    static WsReview     parseReview (const juce::var& v);
    static WsProfile    parseProfile(const juce::var& v);

    // Serialise helpers (for POST body)
    static juce::var chatToVar   (const WsChat&    c);
    static juce::var albumToVar  (const WsAlbum&   a);
    static juce::var reviewToVar (const WsReview&  r);
    static juce::var profileToVar(const WsProfile& p);

    // Build a clean, stripped POST payload from current in-memory state:
    //   - chats with no messages dropped
    //   - underscore-prefixed top-level keys stripped from every object
    //   - reviews sorted newest-first, capped at 50
    juce::String buildPostBody() const;

    EchoJayAPI& api;
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>>(true) };

    std::vector<WsChat>   chats;
    std::vector<WsAlbum>  albums;
    std::vector<WsReview> reviews;
    std::vector<WsPinnedProject> pinnedProjects;   // synced "pinnedProjects" list
    WsProfile             profile;

    LoadState loadState  = LoadState::Idle;
    int       lastStatus = 0;
    bool loadInFlight    = false;
    bool syncPending     = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayWorkspace)
};
