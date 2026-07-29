#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <atomic>
#include <map>

class ChainHost;   // buildCurrentChainInjection reads the live rack
namespace LinkShm { struct RackSidecar; }   // Phase R: targeted-injection overload

struct UserInfo {
    juce::String email;
    juce::String tier = "free";     // "free", "pro", "studio", "its_platinum"
    int tierLevel = 0;              // 0=free, 1=pro, 2=studio (its_platinum maps to 0 here)
    int messagesUsedToday = 0;      // LEGACY fields — kept for fallback when the
    int messageLimit = 15;          // server has not deployed usagePool yet
    int credits = 0;
    juce::String displayName;

    // usage-v2: additive usagePool object from /api/me. When absent
    // (present=false) the client falls back to the legacy fields above and
    // computes percent locally — the plugin must work against both server
    // states. Weights are SERVER-ONLY; the client never sees units.
    struct UsagePool {
        bool  present = false;
        // FREE V2 two-lane shape (13 Jul 2026 contract; /api/me emits
        // usagePool ONLY for free accounts now): premium actions
        // (capture/link/chain/compare) draw the monthly pool + credits;
        // chats draw a daily pool and NEVER touch credits. Paid accounts
        // get no usagePool and keep the legacy single locally-computed bar.
        struct Lane {
            bool  present = false;
            int   used = 0, pool = 0;
            float percent = 0.0f;      // server-computed, 0..100
            juce::String resetAt;      // ISO8601
        };
        Lane premium, chats;
        bool twoLane() const { return premium.present && chats.present; }
        // Legacy single-pool fields (earlier v2 shape) — still parsed so a
        // server rollback cannot blank the UI.
        int   used = 0, pool = 0;
        float percent = 0.0f;          // 0..100
        juce::String period;           // "daily" / "monthly"
        juce::String resetAt;          // ISO8601
        int   credits = 0;
        juce::String tierLabel, capacityLabel;
        bool  modelFast = false;
        juce::String modelName;    // display name of the free lane's model
        int   tasteRemaining = 0;
        // optional nudge field: parsed loosely, unused (UI decided later)
        juce::String nudge;
    };
    UsagePool usagePool;

    // Always-present tier model names from /api/me ("tierModels": { "chat",
    // "premium" }): FIXED per tier, they label the Settings usage bars for
    // every tier (unlike the per-turn chat modelName). Server-fed display
    // names, never hardcoded; empty until the server sends them and the
    // labels then fall back to their plain copy.
    struct TierModels { juce::String chat, premium; };
    TierModels tierModels;

    bool isPro() const { return tierLevel >= 1; }
    bool isStudio() const { return tierLevel >= 2; }

    static int tierStringToLevel(const juce::String& t)
    {
        if (t == "studio_max") return 3;   // top tier, above studio
        if (t == "studio")     return 2;
        if (t == "pro")        return 1;
        return 0;
    }
    static int defaultLimitForTier(int level)
    {
        if (level >= 2) return 400;
        if (level >= 1) return 200;
        return 15;
    }
};

struct UserSettings {
    juce::String name;
    juce::StringArray daws;             // "Logic Pro", "Pro Tools", etc.
    juce::String experienceLevel;       // "Beginner", "Intermediate", "Advanced", "Expert"
    juce::String monitors;
    juce::String headphones;
    juce::String genres;                // Comma-separated
    juce::String plugins;               // Comma-separated or one per line
    
    // Convert to JSON for API
    juce::String toJSON() const;
    // Parse from API response
    static UserSettings fromJSON(const juce::var& json);
};

class EchoJayAPI
{
public:
    EchoJayAPI();
    ~EchoJayAPI();
    
    // ============ Auth ============
    
    // Login with email/password (async)
    void login(const juce::String& email, const juce::String& password,
               std::function<void(bool success, const juce::String& error)> onComplete);
    
    // Logout (clears token and user info)
    void logout();

    // ============ Device pairing (Sign in with browser) ============
    // ADDITIVE alternative to login(), for accounts created with Google or
    // Apple on the web (no password) and anyone who prefers the browser.
    // Flow: POST /api/device/start with this machine's deviceId, open the
    // returned verifyUrl in the default browser (the user approves on
    // echojay.ai, where email/password, Google and Apple all work), then
    // poll /api/device/poll until the server hands back the SAME 30-day
    // JWT that /api/login issues. Ends in the identical logged-in state
    // (authToken + auth.json + userInfo + refreshUserInfo), so everything
    // downstream is untouched.
    //   onCode(userCode)      message thread, fires once when the code is
    //                         minted, for the pairing UI
    //   onComplete(ok, err)   message thread, fires exactly once
    // cancelDeviceLogin() aborts a pairing in progress (Cancel button).
    void startDeviceLogin(std::function<void(const juce::String& userCode)> onCode,
                          std::function<void(bool success, const juce::String& error)> onComplete);
    void cancelDeviceLogin();
    
    // Check if logged in
    bool isLoggedIn() const { return authToken.isNotEmpty(); }
    
    // Get current user info
    UserInfo getUserInfo() const { return userInfo; }
    
    // Check if user can send a message. Two-lane pool: this is the CHAT
    // lane (daily, never credit-extended) — the chat input's gate. Premium
    // surfaces must use canSendTurn/isPremiumExhausted instead.
    bool canSendMessage() const;

    // FREE V2 lane gates. Premium turns (capture_analysis / link_analysis /
    // chain_generate / version_compare) draw the monthly pool + credits;
    // chat draws the daily pool. Legacy states (no pool / paid) fall back
    // to canSendMessage so every tier gets ONE consistent answer.
    bool isPremiumExhausted() const;   // two-lane free: monthly spent, no credits
    bool canSendTurn(const juce::String& turnType) const;
    // Lane-correct blocked copy, mirroring the server's 429 strings exactly.
    juce::String getLimitReachedMessage(const juce::String& turnType) const;
    
    // Get remaining messages this period. THE one used->remaining
    // conversion: the backend sends USED (messagesUsedToday) + limit;
    // every surface must count REMAINING, and must get it from here.
    int getRemainingMessages() const;

    
    // Returns the canonical "you've hit your limit" message for the current
    // tier. Mirrors the strings the SaaS returns on a 429 response so the
    // VST shows the same wording whether the limit is hit client-side
    // (pre-check) or server-side. Centralised here to avoid drift between
    // multiple call sites.
    juce::String getLimitReachedMessage() const;
    
    // Refresh user info from server (check usage, pro status)
    void refreshUserInfo(std::function<void(bool success)> onComplete = nullptr);
    
    // ============ Chat ============
    
    // meterJsonBlob: raw MeterEngine JSON (getMeterDataJSON / meterDataToJSON),
    // passed through UNCHANGED as the request's "meters" field — the backend's
    // parseExtendedMeter reads psr/plr/oversCount/macroBands from it. Empty =
    // field omitted (absent = unavailable convention).
    void sendChat(const juce::StringArray& roles,
                  const juce::StringArray& contents,
                  const juce::String& systemPrompt,
                  std::function<void(const juce::String& reply, bool success)> onComplete,
                  const juce::String& meterJsonBlob = juce::String());

    // Stage the meter blob for the NEXT sendChat call (consumed when the
    // request body is built, so it also survives the limit-refresh retry).
    // Alternative to passing meterJsonBlob directly.
    void setNextChatMeters(const juce::String& blob) { nextChatMeters_ = blob; }

    // usage-v2 client contract: every /api/chat turn carries a turnType
    // ("chat" default, "capture_analysis", "chain_generate",
    // "version_compare", "link_analysis"); link turns include busCount.
    // Staged like the meters blob, consumed at body build (so the
    // limit-retry keeps it).
    void setNextChatTurnType(const juce::String& t, int busCount = 0)
    { nextChatTurnType_ = t; nextChatBusCount_ = busCount; }

    // THE ONLY WAY meter/band data reaches /api/chat: the explicit-capture
    // flag is set here (Capture button flow) and cleared after EVERY send —
    // including the limit-failure path, so a blocked capture can never leak
    // its payload onto the next plain chat turn. sendChat discards any
    // staged blob that arrives without this flag (logged).
    void stageCapturePayload(const juce::String& metersBlob, int busCount)
    {
        nextChatMeters_   = metersBlob;
        nextChatBusCount_ = busCount > 1 ? busCount : 0;
        nextChatTurnType_ = busCount > 1 ? "link_analysis" : "capture_analysis";
        nextChatIsExplicitCapture_ = true;
    }

    /** Drop anything staged for the next chat turn.
     *
     *  ADDED WHEN EchoJayAPI MOVED TO THE PROCESSOR, to preserve the exact
     *  semantics it had as an editor member rather than change them silently.
     *
     *  The four nextChat* fields are a STAGING AREA for the next send: set
     *  just before sendChat, consumed and cleared inside it. While this class
     *  was owned by the editor, staging that was never sent died when Logic
     *  destroyed the editor on a Link window switch. Owned by the processor
     *  it would SURVIVE that, so a capture staged before the switch could
     *  ride the first unrelated chat message typed after it, silently
     *  attaching meter data the user never asked to send.
     *
     *  The editor calls this on construction, which reproduces the old
     *  lifetime exactly: a fresh editor starts with nothing staged.
     *
     *  Every other member here is account or session scoped (endpoint, token,
     *  deviceId, userInfo, userSettings, last model name) and is BETTER shared
     *  than duplicated, which is the point of the move.
     */
    void clearStagedTurn()
    {
        nextChatMeters_.clear();
        nextChatTurnType_.clear();
        nextChatBusCount_ = 0;
        nextChatIsExplicitCapture_ = false;
    }

    // usage-v2 accessors. Percent works against BOTH server states.
    float getUsagePercent() const
    {
        if (userInfo.usagePool.present)
            return juce::jlimit(0.0f, 100.0f, userInfo.usagePool.percent);
        const int lim = juce::jmax(1, userInfo.messageLimit);
        return juce::jlimit(0.0f, 100.0f, 100.0f * (float) userInfo.messagesUsedToday / (float) lim);
    }
    juce::String getUsagePeriod() const
    { return userInfo.usagePool.present && userInfo.usagePool.period.isNotEmpty()
             ? userInfo.usagePool.period : juce::String(userInfo.tierLevel > 0 ? "monthly" : "daily"); }
    int getUsageCredits() const
    { return userInfo.usagePool.present ? userInfo.usagePool.credits : userInfo.credits; }

    // Display name of the model behind the assistant, for the input-bar
    // indicator. Server-fed only, never hardcoded client-side.
    //
    // ORDERING IS THE ENTIRE SAFETY OF THIS FUNCTION — do not reorder.
    //   1. lastChatModelName_        what actually handled the latest turn
    //                                (top-level "modelName" in /api/chat).
    //   2. usagePool.modelName       FREE TIER resolves HERE: /api/me sends
    //                                usagePool only for free (api/me.js:56),
    //                                and its model.name is the free lane's
    //                                CURRENT fast model (Haiku-class).
    //   3. tierModels                always-present for EVERY tier (server
    //                                contract) — the session-start fallback
    //                                for PAID tiers, whose /api/me has no
    //                                usagePool at all (label was empty until
    //                                the first reply landed).
    // If (3) ever moves above (2), the free tier's label silently becomes
    // the PREMIUM model name instead of its fast-model name — tierModels is
    // never empty, so the bug would not announce itself. Branch order is
    // most-specific-first by design; a reordering is wrong on its face.
    juce::String currentModelDisplayName() const
    {
        if (lastChatModelName_.isNotEmpty())          return lastChatModelName_;          // (1)
        if (userInfo.usagePool.modelName.isNotEmpty())return userInfo.usagePool.modelName;// (2) free tier
        if (userInfo.tierModels.premium.isNotEmpty()) return userInfo.tierModels.premium; // (3) paid
        return userInfo.tierModels.chat;                                                  // (3) fallback
    }

    // Server-resolved turnType of the LAST chat reply ("chat" /
    // "chain_generate" / "chain_edit" / ...). Empty until the server ships
    // the resolvedTurnType response field or before the first reply. The
    // client's staged turnType is a hint the server reclassifies — use THIS
    // for any behaviour that depends on what the turn actually was (e.g.
    // gating the prose name-scan chain fallback).
    juce::String getLastResolvedTurnType() const { return lastResolvedTurnType_; }

    // 2.4 dialFlags (26 Jul 2026, DARK): names from the AVAILABLE PLUGINS
    // feed whose LOCAL map passes the dial-signals threshold. Staged per
    // send by the editor only when kDialSignalsEnabled; rides the body as
    // "dialFlags":[...] and clears after the send.
    void setNextDialFlags(const juce::StringArray& names) { nextDialFlags_ = names; }
    
    // ============ User Settings (synced with web app) ============

    // WHAT'S NEW card: GET /api/whats-new — a static JSON array of releases
    // [{"title","line"}...]. Cached to whats_new.json; on any failure the
    // last cache (then empty) is delivered — never an error state.
    void fetchWhatsNew(std::function<void(const juce::var&)> onComplete);

    // Fetch settings from server
    void fetchSettings(std::function<void(bool success)> onComplete = nullptr);
    
    // Save settings to server
    void saveUserSettings(const UserSettings& settings,
                          std::function<void(bool success)> onComplete = nullptr);
    
    // Get current cached settings
    UserSettings getUserSettings() const { return userSettings; }

    // Auto-dial mode (Settings toggle, default off): when on, every
    // /api/chat body carries "autoDial":true and the server restricts chain
    // suggestions to plugins EchoJay has param maps for (so every suggested
    // slot is one-click dialable). Persisted in the LOCAL settings file, not
    // the server profile: the flag only means anything on this machine, and
    // keeping it out of the shared profile blob means a web-side settings
    // save can never clobber it.
    bool getAutoDialMode() const { return autoDialMode; }
    void setAutoDialMode(bool on) { autoDialMode = on; saveSettings(); }

    // Update plugins list from scanner (merges with existing)
    void updatePluginsFromScanner(const juce::String& scannedPlugins);
    
    // ============ Remote Config ============
    
    // Fetch remote config (system prompt, feature flags, version info)
    // Runs async, updates internal state. Falls back to hardcoded prompt if offline.
    void fetchRemoteConfig();
    
    // Get the current system prompt — uses remote version if available, else hardcoded
    static juce::String buildSystemPrompt(const juce::String& channelType,
                                           const juce::String& genre,
                                           const juce::String& pluginSummary);

    // Returns true if a user message looks like it wants plugin suggestions /
    // a chain / a specific processing tool — i.e. a turn where we should
    // inject the full plugin list. Cheap keyword heuristic; errs toward
    // including, since a false positive just sends a list that goes unused.
    static bool messageNeedsPlugins(const juce::String& userMessage);

    // Builds the per-turn plugin block to append to a user message when
    // messageNeedsPlugins() is true. Given the full list string from the
    // scanner, wraps it with framing the AI understands. Returns empty if the
    // list is empty.
    static juce::String buildPluginInjection(const juce::String& fullList);

    // Builds the chain-request injection appended to the user's message content.
    // availablePlugins: the resolved recommendable names (exact, from ChainHost).
    // Returns empty if availablePlugins is empty.
    // When present, instructs the model to: choose ONLY from these names, return
    // the chain as a <<<ECHOJAY_CHAIN>>>...<<<END_CHAIN>>> JSON block at the end
    // of the reply (in addition to the normal human-readable explanation).
    static juce::String buildChainInjection(const juce::StringArray& availablePlugins);

    // [CURRENT CHAIN] injection (CHAIN_AI_BUILD_SPEC Phase 1a): a numbered
    // snapshot of the live rack (name, format, bypassed, wet, settings) so
    // the model can see what already exists — the prerequisite for edit
    // intent. Empty string when the rack is empty. Appended to the user turn
    // like the other injections; sendChat strips it from history turns.
    // The ChainHost form is a thin adapter over the RackSidecar form — ONE
    // formatter authors the block, so a local and a Link-targeted
    // [CURRENT CHAIN] can never drift (Phase R). channelLabel empty = the
    // local rack ("the user's rack right now"); non-empty = that Link's
    // rack, named in the intro. The "[CURRENT CHAIN" marker prefix is
    // byte-identical either way (server classifier + history strip key on
    // it).
    static juce::String buildCurrentChainInjection(const ChainHost& chainHost);
    static juce::String buildCurrentChainInjection(const LinkShm::RackSidecar& rack,
                                                   const juce::String& channelLabel);

    // Parse the chain block out of an assistant reply.
    // Returns true and fills chainJsonOut if a block (complete or truncated) is present.
    // Always strips everything from <<<ECHOJAY_CHAIN>>> onward from replyInOut so raw
    // JSON/delimiters are never shown to the user, even when the closing tag is missing.
    static bool extractChainBlock(juce::String& replyInOut, juce::String& chainJsonOut);

    // Same contract for the Link gain-proposal block:
    //   <<<ECHOJAY_GAIN>>> {"proposals":[{linkId,currentGain,proposedGain,reason}]} <<<END_GAIN>>>
    // Stripped from the visible reply; the client renders APPLY cards.
    static bool extractGainBlock(juce::String& replyInOut, juce::String& gainJsonOut);

    // Same contract for the CHAIN_EDIT ops block (Phase 1c): payload
    // {"baseSlots":[...],"edit":[{op,...}],"explanation"}. Delimiters: keep
    // in sync with api/_blocks.js BLOCK_TYPES.chain_edit (canonical) and
    // extractChainEditBlockWeb in public/app.html.
    static bool extractChainEditBlock(juce::String& replyInOut, juce::String& editJsonOut);

    // Same contract for the ASK question/choices block (Phase 1b): payload
    // {"question","choices":[{"label","detail"}...],"allowFreeText"}.
    // Delimiters: keep in sync with api/_blocks.js BLOCK_TYPES.ask
    // (canonical) and extractAskBlockWeb in public/app.html.
    static bool extractAskBlock(juce::String& replyInOut, juce::String& askJsonOut);

    // Recover a valid chain JSON string from a partially-written (truncated) block.
    // Scans for complete {...} objects using brace depth and reconstructs the array.
    // Returns an empty string if no complete entry can be found.
    static juce::String salvagePartialChain(const juce::String& partial);
    
    // Chat language preference. Affects the language the AI replies in.
    // Stored as a short code: "auto" (match user input — default),
    // "en", "es", "pt-br", "fr", "de", "it", "nl", "ja", "ko", "zh".
    // Persisted to ~/Documents/EchoJay/chat_language.txt so the choice
    // survives across DAW sessions and applies to every plugin instance.
    // The full display name (e.g. "Spanish") is what gets injected into
    // the system prompt — see chatLanguageDisplayName().
    static juce::String getChatLanguage();
    static void         setChatLanguage(const juce::String& code);
    static juce::String chatLanguageDisplayName(const juce::String& code);
    // Returns the (code, displayName) pairs in the order they should appear
    // in the language picker. Includes "auto" as the first entry.
    static const juce::Array<std::pair<juce::String, juce::String>>& chatLanguageList();
    
    // Remote prompt storage — static so all instances share the same prompt
    static juce::String remoteSystemPrompt;
    static int remotePromptVersion;
    static bool remoteConfigLoaded;
    static juce::String latestVersion;
    static juce::String updateUrl;          // legacy/fallback browser URL
    static juce::String downloadUrlMac;     // direct .pkg URL for macOS in-plugin download
    static juce::String downloadUrlWin;     // direct .exe URL for Windows in-plugin download
    static juce::String announcement;
    
    // Remote channel-specific prompts — keyed by channel type name (e.g. "Kick", "Lead Vocal")
    static std::map<juce::String, juce::String> remoteChannelPrompts;
    static int remoteChannelPromptsVersion;
    static juce::String remoteIndividualChannelRules;
    static juce::String remoteIndividualChannelStyle;
    
    // ============ Workspace data (used by EchoJayWorkspace) ============

    // Raw GET /api/data — calls back on the message thread via callAsync.
    void getWorkspaceData(std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) getJSON("/api/data", std::move(cb));
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // Raw POST /api/data — calls back on the message thread via callAsync.
    void postWorkspaceData(const juce::String& body,
                           std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) postJSON("/api/data", body, std::move(cb));
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // ============ Saved chains (Session B, B.1 / B.2) ============
    // Every one of these is gated on DASHBOARD_ENABLED server side, so on
    // production they answer 404 not_enabled until the flag is flipped. That
    // is the dark state working, not a fault. Develop against a preview (see
    // the dev transport).
    //
    // All four call back on the MESSAGE THREAD. `json` is the parsed body and
    // `statusCode` is the HTTP status; callers map it with chainErrorMessage
    // rather than inventing their own wording.

    // POST /api/v2/chains -> 201 { chain }. Create. Body carries slots and
    // state; the response returns hasState, never the blob itself.
    void createChain(const juce::String& body,
                     std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) postJSON("/api/v2/chains", body, std::move(cb));
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // PATCH /api/v2/chains/:id -> 200 { chain }. Save over an existing chain.
    void patchChain(const juce::String& id, const juce::String& body,
                    std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) patchJSON("/api/v2/chains/" + id, body, std::move(cb));
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // GET /api/v2/chains -> { chains: [...] }. NEVER returns state; each row
    // carries hasState so the UI can say whether loading restores settings
    // without fetching a megabyte to find out.
    // 10s, not the 60s default: the sidebar has a CACHED list to fall back
    // on, so a fast honest failure beats a minute of "Loading" followed by
    // the same failure. fetchChain below deliberately keeps the default,
    // because opening a chain has no fallback and waiting beats failing.
    static constexpr int kChainListTimeoutMs = 10000;
    void listChains(std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) getJSON("/api/v2/chains", std::move(cb), kChainListTimeoutMs);
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // GET /api/v2/chains/:id -> { chain }. The ONLY endpoint that returns
    // state. Ownership is enforced server side; a chain that is not yours is
    // 404 not_found, the same answer as one that does not exist.
    void fetchChain(const juce::String& id,
                    std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) getJSON("/api/v2/chains/" + id, std::move(cb));
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // DELETE /api/v2/chains/:id -> 200 { ok: true }. SOFT delete server side:
    // the row and its state blob survive so a mis-click stays recoverable by
    // hand, but every read filters it out, so the client treats it as gone.
    // A second delete answers 404, exactly like a chain that never existed.
    void deleteChain(const juce::String& id,
                     std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) deleteJSON("/api/v2/chains/" + id, std::move(cb));
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // ---- D3.2: sharing a chain ------------------------------------------
    //
    // POST /api/v2/chains/:id/share -> 200 { share, created }
    //
    // The plugin only ever asks for `visibility: "link"`. A PUBLIC share is
    // indexable and carries your name, so the server requires a claimed
    // handle and answers 409 handle_required without one. The plugin has no
    // text entry and no token path to the settings page, so a public option
    // here could only fail pointing at a browser it cannot sign you into, or
    // succeed at publishing something under a name you never chose. Public
    // sharing is a web act. Say so in the copy; do not offer it.
    //
    // IDEMPOTENT. Pressing this twice returns the SAME link and only the
    // first costs a send against the monthly cap, so a second press is a
    // copy, not a new share. `created` in the response says which happened.
    //
    // `share.url` comes back as a PATH ("/c/<slug>"), never an absolute URL:
    // the server refuses to build one from a caller-controlled Host header.
    // The client prepends the site root.
    void shareChain(const juce::String& id, const juce::String& body,
                    std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) postJSON("/api/v2/chains/" + id + "/share", body, std::move(cb));
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // ---- Session C: the Dashboard tab -----------------------------------
    //
    // TWO ENDPOINTS, TWO CADENCES, AND THEY MUST NOT BE CONFUSED. This pair is
    // the whole reason the dashboard is affordable at 11,285 accounts, so the
    // costs are written here at the call site rather than left in a spec:
    //
    //   fetchDashboard  5 Redis reads + 4 Postgres queries on a cold build.
    //                   Called on TAB OPEN and after its 60s TTL. Nothing else.
    //   pollCommunity   1 Redis round trip, 0 Postgres.
    //                   Called every 20s while the editor is open.
    //
    // The dashboard payload CONTAINS community.unread, which makes it tempting
    // to poll it for the badge and delete the second endpoint. That would be
    // roughly a hundred times the cost per tick. Do not.

    // GET /api/v2/dashboard?surface=plugin -> the full payload.
    //
    // surface=plugin is not decoration: the server returns deliberately
    // shorter lists for this consumer (5 rows against the web's 8), which is
    // why the plugin must never ask for the web surface.
    //
    // 10s like listChains and for the same reason: the tab has a disk-cached
    // payload to fall back on, so a fast honest failure beats a minute of
    // "Loading" ending in the same failure.
    static constexpr int kDashboardTimeoutMs = 10000;
    void fetchDashboard(std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) getJSON("/api/v2/dashboard?surface=plugin", std::move(cb), kDashboardTimeoutMs);
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // GET /api/v2/community/poll -> { total, announcements, team, direct, rev }.
    //
    // rev is MONOTONIC. If it has not moved since the last tick, there is
    // nothing new and the caller must do nothing at all: no repaint, no
    // payload fetch, no state write. That check is the point of the endpoint.
    //
    // 5s, tighter than everything else here. A badge that is 20 seconds late
    // is fine; a poll that hangs for 60s while the editor is open is a leaked
    // connection every tick. There is no fallback to wait for either, since a
    // missed tick is simply retried 20 seconds later.
    static constexpr int kPollTimeoutMs = 5000;
    void pollCommunity(std::function<void(const juce::var&, int)> cb)
    {
        if (isLoggedIn()) getJSON("/api/v2/community/poll", std::move(cb), kPollTimeoutMs);
        else if (cb) juce::MessageManager::callAsync([cb]{ cb(juce::var(), 401); });
    }

    // One place that turns a chain-endpoint failure into a sentence for the
    // user. Keeps the wording consistent and keeps "not enabled yet" from
    // being reported as "not found". Empty return = success.
    // Which operation failed, because the same status means different things
    // to a user depending on what they were doing. A list failure that says
    // "your chain has not been saved" sends someone hunting a bug that does
    // not exist.
    enum class ChainOp { Save, List, Open };
    static juce::String chainErrorMessage(const juce::var& json, int statusCode,
                                          ChainOp op = ChainOp::Save);

    // ============ Local Settings ============

    void setEndpoint(const juce::String& url) { apiEndpoint = url; }
    juce::String getEndpoint() const { return apiEndpoint; }
    
    void loadSettings();
    void saveSettings() const;
    static juce::File getSettingsFile();

private:
    juce::String apiEndpoint;
    juce::String authToken;
    // Model that handled the latest chat turn (server "modelName"). Kept
    // across turns that omit the field so the indicator never blanks mid
    // session; empty until the server has ever sent one.
    juce::String lastChatModelName_;
    juce::String lastResolvedTurnType_;   // see getLastResolvedTurnType()
    // Device pairing: cancellation flag for the in-flight poll chain (a
    // fresh one per startDeviceLogin; cancel just flips the current one)
    std::shared_ptr<std::atomic<bool>> devicePairCancelled_;
    void pollDeviceCode(const juce::String& deviceCode, int intervalMs, int attemptsLeft,
                        std::shared_ptr<std::atomic<bool>> cancelled,
                        std::function<void(bool, const juce::String&)> onComplete);
    juce::String deviceId;
    juce::String nextChatMeters_;   // staged by setNextChatMeters()
    juce::String nextChatTurnType_; // staged by setNextChatTurnType(); "" = "chat"
    juce::StringArray nextDialFlags_; // see setNextDialFlags(); cleared per send
    int          nextChatBusCount_ = 0;
    bool         nextChatIsExplicitCapture_ = false;   // see stageCapturePayload
    UserInfo userInfo;
    UserSettings userSettings;
    bool autoDialMode = false;   // see get/setAutoDialMode()
    
    // Shared flag: set to false in destructor so in-flight callbacks
    // know the object is gone and skip any member access.
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>>(true) };
    
    // Helper: make a POST request with auth header
    void postJSON(const juce::String& endpoint, const juce::String& body,
                  std::function<void(const juce::var& json, int statusCode)> onComplete);

    // Development transport (Session B). Both are the identity/empty case in
    // a release build: the implementations are wrapped in
    // #if ECHOJAY_DEV_TRANSPORT, so the preview URL and the bypass secret are
    // not merely unused but absent from the binary. See CMakeLists.txt.
    static juce::String transportEndpoint(const juce::String& configured);
    static juce::String transportHeaders();

    // Helper: PATCH with a JSON body. Same thread and teardown discipline as
    // postJSON; juce::URL sends POST by default, so the verb is set
    // explicitly with withHttpRequestCmd.
    void patchJSON(const juce::String& endpoint, const juce::String& body,
                   std::function<void(const juce::var& json, int statusCode)> onComplete);
    // DELETE, no body. Like patchJSON it does NOT retry, and here the reason
    // is sharper: the second call answers 404 by design, so a retry after a
    // connection dropped mid-flight would report "no longer exists" for a
    // delete that had in fact succeeded. One attempt, one honest answer.
    void deleteJSON(const juce::String& endpoint,
                    std::function<void(const juce::var& json, int statusCode)> onComplete);

public:
    // Helper: make a GET request with auth header. Public: the editor uses
    // it directly for the auto-parameter-mapping map fetches
    // (GET /api/params/maps?fps=...).
    // timeoutMs defaults to the value every caller has always used. Only
    // pass something shorter where the caller has SOMETHING TO SHOW when it
    // expires. getJSON is shared with the whole workspace blob
    // (GET /api/data), the param-map fetch (up to 500 fingerprints per
    // request), the startup config read and the device-pairing long poll; a
    // blanket cut would turn "slow but working" into "fails" on a poor
    // connection and would break pairing outright.
    static constexpr int kDefaultGetTimeoutMs = 60000;
    void getJSON(const juce::String& endpoint,
                 std::function<void(const juce::var& json, int statusCode)> onComplete,
                 int timeoutMs = kDefaultGetTimeoutMs);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayAPI)
};
