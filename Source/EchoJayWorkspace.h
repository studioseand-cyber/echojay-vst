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
};

struct WsChat {
    juce::String id;
    juce::String title;
    std::vector<WsMessage> messages;
    juce::String created;
    juce::String trackName;
    juce::String albumId;
    int revisionCount = 0;
};

struct WsAlbum {
    juce::String id;
    juce::String name;
    juce::String created;
    juce::StringArray chatIds;
    juce::StringArray reviewIds;
};

struct WsMeasurements {
    float integ    = 0.f, range    = 0.f;
    float rmsL     = 0.f, rmsR     = 0.f;
    float peakL    = 0.f, peakR    = 0.f;
    float tpL      = 0.f, tpR      = 0.f;
    float width    = 0.f, corr     = 0.f;
    float crest    = 0.f, dc       = 0.f;
    float duration = 0.f;
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
                             const juce::String& chainJson = juce::String());
    void setChatTitle(const juce::String& chatId, const juce::String& title);
    void setChatTrackName(const juce::String& chatId, const juce::String& trackName);
    void incrementChatRevisionCount(const juce::String& chatId);

    // Persist a local mutation: POST current state immediately, then schedule
    // a background sync so we pull any concurrent web-app edits.
    void requestMutationSync();

    // Parsed data accessors — message thread only.
    const std::vector<WsChat>&   getChats()   const { return chats;   }
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
    WsProfile             profile;

    LoadState loadState  = LoadState::Idle;
    int       lastStatus = 0;
    bool loadInFlight    = false;
    bool syncPending     = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayWorkspace)
};
