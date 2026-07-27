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
};

struct WsChat {
    juce::String id;
    juce::String title;
    std::vector<WsMessage> messages;
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
                             const juce::String& editTargetName = juce::String());
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
