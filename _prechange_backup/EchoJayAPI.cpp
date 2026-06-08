#include "EchoJayAPI.h"

// Static members for remote config — shared across all plugin instances
juce::String EchoJayAPI::remoteSystemPrompt;
int EchoJayAPI::remotePromptVersion = 0;
bool EchoJayAPI::remoteConfigLoaded = false;
juce::String EchoJayAPI::latestVersion;
juce::String EchoJayAPI::updateUrl;
juce::String EchoJayAPI::downloadUrlMac;
juce::String EchoJayAPI::downloadUrlWin;
juce::String EchoJayAPI::announcement;
std::map<juce::String, juce::String> EchoJayAPI::remoteChannelPrompts;
int EchoJayAPI::remoteChannelPromptsVersion = 0;
juce::String EchoJayAPI::remoteIndividualChannelRules;
juce::String EchoJayAPI::remoteIndividualChannelStyle;

EchoJayAPI::EchoJayAPI()
{
    loadSettings();
    if (apiEndpoint.isEmpty())
        apiEndpoint = "https://www.echojay.ai";
    
    // Generate unique device ID from machine identifiers
    auto computerName = juce::SystemStats::getComputerName();
    auto userName = juce::SystemStats::getLogonName();
    auto osName = juce::SystemStats::getOperatingSystemName();
    deviceId = juce::SHA256((computerName + "|" + userName + "|" + osName).toUTF8())
                   .toHexString().substring(0, 16);
    
    // Fetch remote config once per session (shared across instances)
    if (!remoteConfigLoaded)
        fetchRemoteConfig();
    
    // AUTH CACHE REFRESH — if user was already logged in from a previous session,
    // the cached tier/limit/usage on disk may be stale (e.g. they upgraded via the website
    // since last plugin launch). Trigger an immediate server refresh so UI and send-gating
    // reflect current server state instead of stale disk cache.
    if (authToken.isNotEmpty())
        refreshUserInfo(nullptr);
}

EchoJayAPI::~EchoJayAPI()
{
    alive->store(false);
    saveSettings();
}

// ============ Generic POST helper ============

void EchoJayAPI::postJSON(const juce::String& path, const juce::String& body,
                           std::function<void(const juce::var& json, int statusCode)> onComplete)
{
    auto endpoint = apiEndpoint;
    auto token = authToken;
    auto cb = std::make_shared<std::function<void(const juce::var&, int)>>(onComplete);
    auto aliveFlag = alive; // capture shared_ptr by value — prevent use-after-free

    juce::Thread::launch([=]()
    {
        juce::var json;
        int statusCode = 0;

        // Retry only on connection-level failures (createInputStream returns
        // nullptr — timeout, dropped connection, TLS/DNS blip, cold-start
        // stall before first byte). These never reach the server, so retrying
        // is safe. Real HTTP responses (200/4xx/5xx) are NOT retried here —
        // the caller's status-code logic handles those.
        constexpr int maxAttempts = 3;
        for (int attempt = 1; attempt <= maxAttempts; ++attempt)
        {
            juce::URL url(endpoint + path);
            url = url.withPOSTData(body);

            juce::String headers = "Content-Type: application/json\r\n";
            if (token.isNotEmpty())
                headers += "Authorization: Bearer " + token + "\r\n";

            statusCode = 0;
            auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                               .withExtraHeaders(headers)
                               .withConnectionTimeoutMs(60000)
                               .withStatusCode(&statusCode);

            auto stream = url.createInputStream(options);

            if (stream != nullptr)
            {
                auto responseText = stream->readEntireStreamAsString();
                json = juce::JSON::parse(responseText);
                break; // got a response (any status) — stop retrying
            }

            DBG("[EchoJay] postJSON connection failed (attempt " << attempt
                << "/" << maxAttempts << ") path=" << path << " statusCode=" << statusCode);

            if (attempt < maxAttempts)
                juce::Thread::sleep(attempt * 1000); // 1s, then 2s backoff
        }

        auto callback = cb;
        auto sc = statusCode;
        auto j = json;
        juce::MessageManager::callAsync([callback, j, sc, aliveFlag]() {
            if (!aliveFlag->load()) return; // object destroyed — bail
            (*callback)(j, sc);
        });
    });
}

// ============ Generic GET helper ============

void EchoJayAPI::getJSON(const juce::String& path,
                          std::function<void(const juce::var& json, int statusCode)> onComplete)
{
    auto endpoint = apiEndpoint;
    auto token = authToken;
    auto devId = deviceId;
    auto cb = std::make_shared<std::function<void(const juce::var&, int)>>(onComplete);
    auto aliveFlag = alive; // capture shared_ptr by value — prevent use-after-free
    
    juce::Thread::launch([=]()
    {
        juce::URL url(endpoint + path);
        
        juce::String headers;
        if (token.isNotEmpty())
            headers += "Authorization: Bearer " + token + "\r\n";
        if (devId.isNotEmpty())
            headers += "x-device-id: " + devId + "\r\n";
        
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withExtraHeaders(headers)
                           .withConnectionTimeoutMs(60000)
                           .withStatusCode(&statusCode);
        
        auto stream = url.createInputStream(options);
        
        juce::var json;
        if (stream != nullptr)
        {
            auto responseText = stream->readEntireStreamAsString();
            json = juce::JSON::parse(responseText);
        }
        
        auto callback = cb;
        auto sc = statusCode;
        auto j = json;
        juce::MessageManager::callAsync([callback, j, sc, aliveFlag]() {
            if (!aliveFlag->load()) return; // object destroyed — bail
            (*callback)(j, sc);
        });
    });
}

// ============ Auth ============

void EchoJayAPI::login(const juce::String& email, const juce::String& password,
                        std::function<void(bool success, const juce::String& error)> onComplete)
{
    juce::String body = "{\"email\":" + juce::JSON::toString(email) + 
                        ",\"password\":" + juce::JSON::toString(password) +
                        ",\"deviceId\":" + juce::JSON::toString(deviceId) + "}";
    
    postJSON("/api/login", body, [this, onComplete](const juce::var& json, int statusCode)
    {
        if (statusCode == 200 && json.isObject())
        {
            auto* obj = json.getDynamicObject();
            if (obj && obj->hasProperty("token"))
            {
                authToken = obj->getProperty("token").toString();
                
                // Try to parse user info from login response (may or may not have it)
                userInfo.email = obj->getProperty("email").toString();
                
                // Check for nested user object
                auto userVar = obj->getProperty("user");
                if (auto* userObj = userVar.getDynamicObject())
                {
                    userInfo.email = userObj->getProperty("email").toString();
                    userInfo.displayName = userObj->getProperty("name").toString();
                    juce::String tierStr = userObj->getProperty("tier").toString();
                    if (tierStr.isNotEmpty())
                    {
                        userInfo.tier = tierStr;
                        userInfo.tierLevel = UserInfo::tierStringToLevel(tierStr);
                    }
                }
                
                // Check for nested usage object
                auto usageVar = obj->getProperty("usage");
                if (auto* usageObj = usageVar.getDynamicObject())
                {
                    userInfo.messagesUsedToday = (int)usageObj->getProperty("messagesUsedToday");
                    userInfo.messageLimit = (int)usageObj->getProperty("messagesPerDay");
                    if (usageObj->hasProperty("credits"))
                        userInfo.credits = (int)usageObj->getProperty("credits");
                }
                
                // Also check flat fields (in case server sends those)
                if (obj->hasProperty("plan"))
                {
                    juce::String plan = obj->getProperty("plan").toString();
                    userInfo.tier = plan;
                    userInfo.tierLevel = UserInfo::tierStringToLevel(plan);
                }
                if (obj->hasProperty("name") && userInfo.displayName.isEmpty())
                    userInfo.displayName = obj->getProperty("name").toString();
                
                // Set defaults if not populated
                if (userInfo.messageLimit <= 0)
                    userInfo.messageLimit = UserInfo::defaultLimitForTier(userInfo.tierLevel);
                if (userInfo.displayName.isEmpty())
                    userInfo.displayName = userInfo.email.upToFirstOccurrenceOf("@", false, false);
                
                saveSettings();
                
                // Immediately refresh from /api/me to get full data
                refreshUserInfo(nullptr);
                
                onComplete(true, "");
                return;
            }
        }
        
        juce::String error = "Login failed";
        if (json.isObject())
        {
            auto* obj = json.getDynamicObject();
            if (obj && obj->hasProperty("error"))
                error = obj->getProperty("error").toString();
        }
        onComplete(false, error);
    });
}

void EchoJayAPI::logout()
{
    authToken = "";
    userInfo = UserInfo();
    saveSettings();
}

bool EchoJayAPI::canSendMessage() const
{
    return userInfo.messagesUsedToday < (userInfo.messageLimit + userInfo.credits);
}

int EchoJayAPI::getRemainingMessages() const
{
    return juce::jmax(0, userInfo.messageLimit - userInfo.messagesUsedToday);
}

juce::String EchoJayAPI::getLimitReachedMessage() const
{
    // Strings MUST stay in sync with api/chat.js in the SaaS — see the
    // `if (user.tier === 'free')` / `else if (user.tier === 'pro')` block.
    // The SaaS sends these in the 429 response body; we mirror them here
    // for the client-side pre-check path so the user sees identical
    // wording regardless of which side caught the limit.
    if (userInfo.tier == "free")
        return "You've hit your free monthly limit. Upgrade to Pro to keep going.";
    if (userInfo.tier == "pro")
        return "You've hit your monthly limit. Upgrade to Studio or top up with credits to keep going.";
    // studio, its_platinum, and any future tiers fall through to the
    // generic "top up" message — they can't upgrade further.
    return "You've hit your monthly limit. Top up with credits to keep going.";
}

void EchoJayAPI::refreshUserInfo(std::function<void(bool success)> onComplete)
{
    if (!isLoggedIn())
    {
        if (onComplete) onComplete(false);
        return;
    }
    
    // GET /api/me returns:
    // { "user": { "email": "...", "name": "...", "tier": "pro" | "studio" | "free" },
    //   "usage": { "messagesUsedToday": 12, "messagesPerDay": 50, "remaining": 38, "credits": 15 },
    //   "tierInfo": { "name": "Pro", "model": "claude-sonnet-4-20250514" } }
    getJSON("/api/me", [this, onComplete](const juce::var& json, int statusCode)
    {
        if (statusCode == 200 && json.isObject())
        {
            auto* root = json.getDynamicObject();
            if (root)
            {
                // Parse user object
                auto userVar = root->getProperty("user");
                if (auto* userObj = userVar.getDynamicObject())
                {
                    userInfo.email = userObj->getProperty("email").toString();
                    userInfo.displayName = userObj->getProperty("name").toString();
                    if (userInfo.displayName.isEmpty())
                        userInfo.displayName = userInfo.email.upToFirstOccurrenceOf("@", false, false);
                    
                    juce::String tierStr = userObj->getProperty("tier").toString();
                    if (tierStr.isNotEmpty())
                    {
                        userInfo.tier = tierStr;
                        userInfo.tierLevel = UserInfo::tierStringToLevel(tierStr);
                    }
                }
                
                // Parse usage object
                auto usageVar = root->getProperty("usage");
                if (auto* usageObj = usageVar.getDynamicObject())
                {
                    userInfo.messagesUsedToday = (int)usageObj->getProperty("messagesUsedToday");
                    userInfo.messageLimit = (int)usageObj->getProperty("messagesPerDay");
                    if (userInfo.messageLimit <= 0)
                        userInfo.messageLimit = UserInfo::defaultLimitForTier(userInfo.tierLevel);
                    if (usageObj->hasProperty("credits"))
                        userInfo.credits = (int)usageObj->getProperty("credits");
                }
                
                saveSettings();
                if (onComplete) onComplete(true);
                return;
            }
        }
        
        // Token might be expired — but don't wipe on refresh failures
        // as it could be a transient network issue. Only login and chat
        // endpoints should clear auth on 401.
        
        if (onComplete) onComplete(false);
    });
}

// ============ Chat ============

void EchoJayAPI::sendChat(const juce::StringArray& roles,
                           const juce::StringArray& contents,
                           const juce::String& systemPrompt,
                           std::function<void(const juce::String& reply, bool success)> onComplete)
{
    if (!canSendMessage())
    {
        // AUTH CACHE REFRESH — local cache says we're over limit, but the user may have
        // upgraded tier or credits since the last server sync. Refresh from /api/me first,
        // and only show "limit reached" if the server ALSO agrees we're at the limit.
        // This is the self-healing path for users who upgrade mid-session.
        auto aliveFlag = alive;
        refreshUserInfo([this, roles, contents, systemPrompt, onComplete, aliveFlag](bool refreshSuccess)
        {
            if (!aliveFlag->load()) return;
            
            if (refreshSuccess && canSendMessage())
            {
                // Refresh revealed we actually CAN send (tier upgraded, credits added, new day, etc).
                // Retry the send as if the limit error never happened.
                sendChat(roles, contents, systemPrompt, onComplete);
                return;
            }
            
            // Server confirms we're at the limit (or refresh failed — fall back to cached state).
            onComplete(getLimitReachedMessage(), false);
        });
        return;
    }
    
    // Build messages JSON
    // Limit history to the last N messages to prevent stale captures from old
    // sessions confusing the AI. Captures persist in plugin state across DAW
    // sessions, so without this limit a fresh "first capture" on plugin reload
    // could be read against months of old chat history — which caused the AI
    // to occasionally reference stale numbers (e.g. saying -8 LUFS when the
    // current capture is -27, because some old capture in history was -8).
    // Keeping last 12 messages = roughly last 6 capture+reply pairs, plenty
    // for in-session continuity without dragging in unrelated old captures.
    constexpr int maxHistoryMessages = 12;
    int firstIdx = juce::jmax(0, roles.size() - maxHistoryMessages);
    // Anthropic API requires the first message in messages[] to be 'user'.
    // If trimming landed us on 'assistant', skip forward by one to land on user.
    while (firstIdx < roles.size() && roles[firstIdx] != "user")
        firstIdx++;
    
    juce::String messagesJson = "[";
    messagesJson += "{\"role\":\"system\",\"content\":" + juce::JSON::toString(systemPrompt) + "}";
    for (int i = firstIdx; i < roles.size(); ++i)
    {
        messagesJson += ",{\"role\":" + juce::JSON::toString(roles[i]) + 
                        ",\"content\":" + juce::JSON::toString(contents[i]) + "}";
    }
    messagesJson += "]";
    
    juce::String body = "{\"messages\":" + messagesJson + "}";
    
    postJSON("/api/chat", body, [this, onComplete](const juce::var& json, int statusCode)
    {
        if (statusCode == 200 && json.isObject())
        {
            auto* obj = json.getDynamicObject();
            if (obj && obj->hasProperty("reply"))
            {
                juce::String reply = obj->getProperty("reply").toString();
                
                // Check if this message used a credit (don't increment daily counter)
                bool usedCredit = false;
                
                // Try to read server's usage count from response
                // Could be flat: {"usage": 42} or nested: {"usage": {"messagesUsedToday": 42}}
                if (obj->hasProperty("usage"))
                {
                    auto usageVal = obj->getProperty("usage");
                    if (usageVal.isObject())
                    {
                        if (auto* usageObj = usageVal.getDynamicObject())
                        {
                            if (usageObj->hasProperty("usedCredit"))
                                usedCredit = (bool)usageObj->getProperty("usedCredit");
                            if (usageObj->hasProperty("credits"))
                                userInfo.credits = (int)usageObj->getProperty("credits");
                            if (usageObj->hasProperty("messagesUsedToday"))
                                userInfo.messagesUsedToday = (int)usageObj->getProperty("messagesUsedToday");
                            if (usageObj->hasProperty("messagesPerDay"))
                                userInfo.messageLimit = (int)usageObj->getProperty("messagesPerDay");
                            if (usageObj->hasProperty("remaining"))
                            {
                                int rem = (int)usageObj->getProperty("remaining");
                                userInfo.messagesUsedToday = userInfo.messageLimit - rem;
                            }
                        }
                    }
                    else
                    {
                        // Flat integer
                        userInfo.messagesUsedToday = (int)usageVal;
                    }
                }
                else
                {
                    // No usage in response — increment locally only if not a credit use
                    if (!usedCredit)
                        userInfo.messagesUsedToday++;
                }
                
                saveSettings(); // persist usage count to disk
                onComplete(reply, true);
                return;
            }
        }
        
        if (statusCode == 401)
        {
            authToken = "";
            userInfo = UserInfo();
            saveSettings();
            onComplete("Session expired. Please log in again.", false);
            return;
        }
        
        if (statusCode == 429)
        {
            // Display the server's error message directly
            juce::String serverMsg;
            if (json.isObject())
            {
                auto* obj = json.getDynamicObject();
                if (obj && obj->hasProperty("error"))
                    serverMsg = obj->getProperty("error").toString();
            }
            if (serverMsg.isEmpty())
                serverMsg = getLimitReachedMessage();
            onComplete(serverMsg, false);
            return;
        }
        
        juce::String error = (statusCode == 0)
            ? "Could not reach EchoJay. Check your connection and try again."
            : "Failed to get AI response";
        if (json.isObject())
        {
            auto* obj = json.getDynamicObject();
            if (obj && obj->hasProperty("error"))
                error = obj->getProperty("error").toString();
        }
        onComplete(error, false);
    });
}

// ============ Remote Config ============

void EchoJayAPI::fetchRemoteConfig()
{
    auto endpoint = apiEndpoint;
    auto aliveFlag = alive;
    
    // Helper lambda to parse channel prompts from a config JSON object
    auto parseChannelConfig = [](juce::DynamicObject* obj)
    {
        // Parse channel-specific prompts
        if (obj->hasProperty("channelPrompts"))
        {
            int cpVersion = (int)obj->getProperty("channelPromptsVersion");
            if (cpVersion > remoteChannelPromptsVersion)
            {
                auto cpVar = obj->getProperty("channelPrompts");
                if (auto* cpObj = cpVar.getDynamicObject())
                {
                    remoteChannelPrompts.clear();
                    for (auto& prop : cpObj->getProperties())
                        remoteChannelPrompts[prop.name.toString()] = prop.value.toString();
                    remoteChannelPromptsVersion = cpVersion;
                }
            }
        }
        
        // Parse individual channel rules and style
        if (obj->hasProperty("individualChannelRules"))
        {
            auto rules = obj->getProperty("individualChannelRules").toString();
            if (rules.isNotEmpty())
                remoteIndividualChannelRules = rules;
        }
        if (obj->hasProperty("individualChannelStyle"))
        {
            auto style = obj->getProperty("individualChannelStyle").toString();
            if (style.isNotEmpty())
                remoteIndividualChannelStyle = style;
        }
    };
    
    juce::Thread::launch([=]()
    {
        juce::URL url(endpoint + "/api/vst-config");
        
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs(60000)
                           .withStatusCode(&statusCode);
        
        auto stream = url.createInputStream(options);
        
        if (stream != nullptr && statusCode == 200)
        {
            auto responseText = stream->readEntireStreamAsString();
            auto json = juce::JSON::parse(responseText);
            
            if (auto* obj = json.getDynamicObject())
            {
                int version = (int)obj->getProperty("systemPromptVersion");
                auto prompt = obj->getProperty("systemPrompt").toString();
                
                if (prompt.isNotEmpty() && version > remotePromptVersion)
                {
                    remoteSystemPrompt = prompt;
                    remotePromptVersion = version;
                }
                
                // Parse channel prompts
                parseChannelConfig(obj);
                
                if (obj->hasProperty("latestVersion"))
                    latestVersion = obj->getProperty("latestVersion").toString();
                if (obj->hasProperty("updateUrl"))
                    updateUrl = obj->getProperty("updateUrl").toString();
                if (obj->hasProperty("downloadUrlMac"))
                    downloadUrlMac = obj->getProperty("downloadUrlMac").toString();
                if (obj->hasProperty("downloadUrlWin"))
                    downloadUrlWin = obj->getProperty("downloadUrlWin").toString();
                if (obj->hasProperty("announcement"))
                    announcement = obj->getProperty("announcement").toString();
                
                remoteConfigLoaded = true;
                
                // Cache to disk so it works offline next time
                auto cacheFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                     .getChildFile("EchoJay").getChildFile("remote_config.json");
                cacheFile.getParentDirectory().createDirectory();
                cacheFile.replaceWithText(responseText);
            }
        }
        else if (!remoteConfigLoaded)
        {
            // Offline fallback — try loading cached config from disk
            auto cacheFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                 .getChildFile("EchoJay").getChildFile("remote_config.json");
            if (cacheFile.existsAsFile())
            {
                auto json = juce::JSON::parse(cacheFile.loadFileAsString());
                if (auto* obj = json.getDynamicObject())
                {
                    auto prompt = obj->getProperty("systemPrompt").toString();
                    int version = (int)obj->getProperty("systemPromptVersion");
                    if (prompt.isNotEmpty())
                    {
                        remoteSystemPrompt = prompt;
                        remotePromptVersion = version;
                    }
                    
                    // Also parse channel prompts from cache
                    parseChannelConfig(obj);
                    
                    // Pull update-related fields too so the offline path still
                    // knows the latest version and where to download from. The
                    // user will likely be online by the time they click
                    // "Download Update" — caching these lets the overlay show
                    // the right state even before the next config refresh.
                    if (obj->hasProperty("latestVersion"))
                        latestVersion = obj->getProperty("latestVersion").toString();
                    if (obj->hasProperty("updateUrl"))
                        updateUrl = obj->getProperty("updateUrl").toString();
                    if (obj->hasProperty("downloadUrlMac"))
                        downloadUrlMac = obj->getProperty("downloadUrlMac").toString();
                    if (obj->hasProperty("downloadUrlWin"))
                        downloadUrlWin = obj->getProperty("downloadUrlWin").toString();
                }
            }
            remoteConfigLoaded = true;
        }
    });
}

// ============ Chat Language ============
// Stored as a short code in ~/Documents/EchoJay/chat_language.txt. The file
// is human-editable plain text so users can override via the filesystem if
// they want to add a language we haven't exposed in the UI yet (anything
// Claude recognises will work — it's just a string injected into the
// system prompt).

const juce::Array<std::pair<juce::String, juce::String>>& EchoJayAPI::chatLanguageList()
{
    // Static-init the list once. Order is the order they'll appear in the
    // Settings dropdown. "Auto" first so it's the default sensible pick;
    // English second; rest sorted by approximate user share for audio
    // production globally (rough order — feel free to reshuffle).
    static const auto list = [] {
        juce::Array<std::pair<juce::String, juce::String>> l;
        l.add({ "auto",  "Auto (match input)" });
        l.add({ "en",    "English" });
        l.add({ "es",    "Spanish" });
        l.add({ "pt-br", "Portuguese (Brazil)" });
        l.add({ "fr",    "French" });
        l.add({ "de",    "German" });
        l.add({ "it",    "Italian" });
        l.add({ "nl",    "Dutch" });
        l.add({ "ja",    "Japanese" });
        l.add({ "ko",    "Korean" });
        l.add({ "zh",    "Chinese (Simplified)" });
        return l;
    }();
    return list;
}

juce::String EchoJayAPI::chatLanguageDisplayName(const juce::String& code)
{
    for (auto& p : chatLanguageList())
        if (p.first == code) return p.second;
    return "Auto (match input)";
}

static juce::File getChatLanguageFile()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
               .getChildFile("EchoJay").getChildFile("chat_language.txt");
}

juce::String EchoJayAPI::getChatLanguage()
{
    auto file = getChatLanguageFile();
    if (! file.existsAsFile()) return "auto";
    auto code = file.loadFileAsString().trim().toLowerCase();
    // Validate against the known list — reject anything else to avoid
    // injecting arbitrary user-edited text into the system prompt.
    for (auto& p : chatLanguageList())
        if (p.first == code) return code;
    return "auto";
}

void EchoJayAPI::setChatLanguage(const juce::String& code)
{
    // Round-trip through the list so we only ever persist known codes.
    juce::String validated = "auto";
    for (auto& p : chatLanguageList())
        if (p.first == code) { validated = code; break; }
    
    auto file = getChatLanguageFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText(validated);
}

// ============ System Prompt ============

juce::String EchoJayAPI::buildSystemPrompt(const juce::String& channelType,
                                             const juce::String& genre,
                                             const juce::String& pluginList)
{
    juce::String prompt;
    
    // Use remote system prompt if available, otherwise use hardcoded fallback
    if (remoteSystemPrompt.isNotEmpty())
    {
        prompt = remoteSystemPrompt + "\n\n";
    }
    else
    {
    // === HARDCODED FALLBACK (used when offline or before remote config loads) ===
    prompt += "You're EchoJay, a mix engineer. You talk like a mate in the studio — direct, honest, conversational. No corporate language, no dramatic descriptions, no lists.\n\n";
    
    prompt += "Write your reviews exactly like these examples:\n\n";
    prompt += "EXAMPLE 1 — when nothing's flagged (version A):\n";
    prompt += "\"Nothing flagging on the meters. What are you trying to do with this — push it harder, give it more space, sit it against something specific? Or you can drop a reference into Compare Mixes if you want a side-by-side.\"\n\n";
    prompt += "EXAMPLE 2 — when nothing's flagged (version B):\n";
    prompt += "\"Nothing's jumping out as a problem. Anything specific you're wrestling with on this, or want me to focus on a particular part of it?\"\n\n";
    prompt += "EXAMPLE 3 — when nothing's flagged (version C):\n";
    prompt += "\"Nothing flagging on the meters. Tell me what you want to dig into and we can take it from there — or drop a reference into Compare Mixes.\"\n\n";
    prompt += "EXAMPLE 4 — when there are issues:\n";
    prompt += "\"The limiter's eating into your transients pretty hard at 4.8dB crest. If that's the vibe you're going for, fair enough; if you're finding it lacks punch that could be why. Backing off the limiter a touch usually gets dynamics back without losing much perceived loudness.\n\n";
    prompt += "What are you working on with this?\"\n\n";
    
    prompt += "IMPORTANT: Each review must read differently. Don't compare the crest to \"what most people/tracks would have at this level\" — that's a crutch. Just state what the number means for the track. Vary your opening sentence every time. Never start two reviews the same way.\n\n";
    
    prompt += "MATCH THAT TONE AND STYLE EXACTLY. Notice:\n";
    prompt += "- Short, conversational paragraphs — 1-2 paragraphs is fine\n";
    prompt += "- The meters can be commented on freely — saying 'the loudness is sitting in a good spot' or 'the crest factor is healthy' or 'the meters are looking good' is fine because those are statements about the meter readings, which you can see. What is NOT fine is commenting on how the audio sounds — 'the transients are punching through', 'there's life in the mix', 'things are punchy', 'sounds clean' — those are claims about audio you cannot hear. Same word can be fine or not depending on what it's describing: 'healthy crest factor' = fine (the meter), 'healthy mix' = not fine (the audio).\n";
    prompt += "- Frames issues as choices (\"if that's the vibe\" / \"if you're finding it lacks punch\")\n";
    prompt += "- Does NOT give unsolicited technique suggestions — just reviews the numbers and asks what the user wants to work on\n";
    prompt += "- Only suggests a specific technique if the user asks, or if there's a clear problem worth flagging\n";
    prompt += "- Never uses these words/phrases: fascinating, striking, sparkle, sparkly, sizzle, shimmer, notably, perceived, sonic landscape, breathing room, on the table, intriguing, presence, harsh, harshness, warmth, warm, body, clarity, depth, character, openness, air, airy, mud, muddy, smooth, smoothness, sheen, polish (these are listening claims about audio character — you cannot hear the audio, only meters)\n";
    prompt += "- Never guesses instruments — you cannot hear what's in the track\n";
    prompt += "- Suggests Compare Mixes or asks what the user wants to work on\n";
    prompt += "- Doesn't force criticism — if nothing's flagging, say so neutrally and ask what the user is working on\n\n";
    
    prompt += "CRITICAL — YOU CANNOT HEAR THE TRACK. You only have meter data. So:\n";
    prompt += "- NEVER guess what instruments or elements are in the mix\n";
    prompt += "- NEVER make up specific mix scenarios — you're guessing\n";
    prompt += "- NEVER criticise a metric that's in the normal range. If LUFS is in range, crest is healthy, width is 10-50%, and correlation is above 0 — those are all FINE. Do not suggest improvements to things that aren't broken.\n";
    prompt += "- NEVER mention a metric that isn't in the data. If width or correlation aren't shown, don't mention them. Don't invent or guess values.\n";
    prompt += "- It is OK to write a short review. If nothing is wrong, 1-2 paragraphs is enough. Do NOT pad with invented problems to fill space.\n\n";
    
    prompt += "METER READING:\n";
    prompt += "- LUFS: Use the genre (provided below, never mention it in your response) to judge loudness. -8 to -11 is normal for urban/pop. -5 to -8 for dance/club genres (DnB, house, techno, EDM, trance, garage, bassline). -6 to -9 for other electronic. -8 to -12 for rock. -14 to -22 for jazz/classical/ambient. Only flag if clearly outside the expected range.\n";
    prompt += "- Crest factor: Below 3dB = crushed. 3-5dB = compressed but can be intentional for loud/punchy genres. 5-8dB = solid. 8-14dB = dynamic. Do NOT assume compressed = bad. For club music, hip-hop, and pop a crest of 4-6dB is often exactly right. Only flag crest if it seems wrong FOR THE GENRE.\n";
    prompt += "- True peak: Only shown in the data if it's clipping above +2.0 dBTP. If true peak isn't in the data, don't mention it.\n";
    prompt += "- Width: Only shown in the data if it's notably narrow or wide. If width isn't in the data, don't mention it.\n";
    prompt += "- Correlation: Only shown if there are phase issues. If not in the data, don't mention it.\n\n";
    
    prompt += "NEVER mention streaming platform loudness penalties, normalisation, or 'platforms will turn it down'. This advice is outdated and unhelpful. Judge loudness purely on whether it suits the genre and creative intent.\n\n";
    
    prompt += "CHANNEL SHAPE CHECK: If the data block contains a line starting with 'CHANNEL SHAPE UNUSUAL:' open your response by raising it as a question — NOT a verdict. The fingerprint check is just looking at energy distribution, and unusual readings can be a genuine creative choice (heavy distortion, layered samples, sidechaining, unconventional sound design). So phrase it as 'these readings are a bit unusual for an 808 — is this definitely a bass?' or 'the shape here is more like a hi-hat than an 808, did you mean to capture this on the 808 channel or is there something distinctive going on with the sound?'. Use the reason and shape-guess from the flag itself but soften the language — never say 'this is wrong' or 'you've selected the wrong channel'. After raising the question, you can still give a brief read of what's there based on the band profile if one's included. Do NOT quote dB values or band names at the user. If a BAND PROFILE line is present, treat the numbers as your reference, not the user's.\n\n";
    prompt += "Even without an explicit shape flag, sanity-check the data against the channel: a vocal channel with very wide stereo, very low crest, or strong sub-bass content probably isn't a vocal; a drum/percussion channel with no transient character isn't drums; a bass channel with most energy above 500Hz isn't bass; a Mix Bus with energy only in one frequency range probably isn't a full mix. If something looks off, raise it as a question once — 'these numbers look more like a full mix than a lead vocal, is that the right channel selected or is this an unusual processing setup?' — then move on.\n\n";
    
    prompt += "SPECTRUM READING:\n";
    prompt += "- Lines labelled SPECTRUM ISSUE, SPECTRUM WARNING, or SPECTRUM NOTE are internal flags — NEVER say those words. Talk like an engineer.\n";
    prompt += "- FOR FULL MIXES / MASTER BUS / MUSIC BUS: ONLY mention the spectrum on your initial review if the data contains a SPECTRUM ISSUE flag. If there is no spectrum data or no flag, do NOT talk about frequency balance, tonal balance, spectrum shape, or anything frequency-related on the initial review — a normal spectrum is not worth mentioning. HOWEVER, if the user asks about frequencies, tonal balance, or the spectrum directly in a follow-up message, you MUST describe what the band data actually shows. Never claim there is no spectrum data when it is in your context — describe what's there in plain language (e.g. 'the lows are sitting heavier than the mids, highs are quite controlled'). Never quote dB values or band names.\n";
    prompt += "- FOR INDIVIDUAL CHANNELS: If the data contains a spectrum warning, flag it conversationally. If no spectrum warning is present, don't mention frequencies on the initial review. But if the user asks about frequencies in a follow-up, describe what the band data shows in plain language — never claim no spectrum data exists when it's in your context.\n";
    prompt += "- When you DO mention spectrum issues, use plain language — 'there's nothing below 100Hz' or 'the highs are way louder than everything else'. Never list dB values or band names.\n";
    prompt += "- If the spectrum looks wrong for the channel type, ask about upstream processing or the right channel selected.\n\n";
    
    prompt += "WHEN NOTHING IS FLAGGING: Say the meters aren't flagging anything (or that they look good — you can comment on the meters themselves), then ask what the user wants to focus on, or suggest dropping a reference into Compare Mixes. You can be conversational and write a few sentences. The line you must not cross is making claims about how the audio SOUNDS — never say the mix sounds clean/punchy/tight/balanced, never say transients are coming through, never say there's 'life' or 'energy' or 'punch' in the audio. You can hear nothing. You can only see meters. Comment on meters, ask the user about intent, suggest Compare Mixes — but don't tell the user how their audio sounds. Do NOT invent problems. Do NOT talk about the spectrum if it wasn't flagged.\n\n";
    prompt += "DATA IS ALWAYS PRESENT: Every capture you receive contains valid LUFS, crest factor, true peak, width, and correlation values. NEVER claim 'not getting readings', 'meters aren't picking up', 'no loudness data', 'still no readings', 'not seeing the data', or anything implying the data is missing or incomplete. If you can see the capture data block at all, you have all the values. If a number looks unusual (very low, very high, zero), report it as the unusual value it is — do NOT claim the meters are broken or missing.\n\n";
    prompt += "EACH CAPTURE IS FRESH: Treat every new capture as a fresh review based on its own data. Do NOT carry over concerns, narratives, or framing from previous captures unless the data on the new capture independently supports it. If you flagged a problem on capture 1 (drift, channel mismatch, missing sub etc), do NOT assume the same problem exists on capture 2 — re-evaluate from scratch. Each capture's data stands on its own.\n\n";
    prompt += "In follow-up conversation: answer what they ask. Be conversational. Don't repeat the review.\n\n";
    
    } // end hardcoded fallback
    
    // === DYNAMIC SECTIONS (always appended, whether remote or hardcoded) ===
    
    // Channel type context — tells AI what kind of audio this is and what to focus on
    if (channelType != "Mix Bus" && channelType != "Master Bus")
    {
        prompt += "CHANNEL TYPE: \"" + channelType + "\"\n";
        prompt += "This is NOT a full mix. Focus your feedback on what matters for this specific element.\n";
        
        // Channel-specific focus prompt — use remote config if available, hardcoded fallback otherwise
        juce::String channelFocus;
        
        // Try remote config first
        auto it = remoteChannelPrompts.find(channelType);
        if (it != remoteChannelPrompts.end())
        {
            channelFocus = it->second;
        }
        else
        {
            // Hardcoded fallback — used when offline or before remote config loads
            // Vocals
            if (channelType == "Lead Vocal")
                channelFocus = "Focus on: clarity and presence (2-5kHz), sibilance (6-10kHz), compression and dynamic control, proximity effect (low-mid buildup around 200-400Hz), de-essing needs, whether it would cut through a mix. Loudness norms don't apply — judge relative to typical vocal levels.";
            else if (channelType == "Backing Vocal")
                channelFocus = "Focus on: how it would sit behind a lead (presence vs lead conflict), stereo placement, whether EQ could help it tuck in without disappearing, compression for evenness, any harshness that would compete with the lead.";
            else if (channelType == "Adlibs")
                channelFocus = "Focus on: character and vibe, whether effects (delay/reverb) are working, stereo placement, whether they cut through without being distracting, dynamic range.";
            else if (channelType == "Vocal Bus")
                channelFocus = "Focus on: overall vocal balance, bus compression glue, tonal consistency across layers, stereo spread of the vocal stack, how it would sit in a full mix.";
            else if (channelType == "Kick")
                channelFocus = "Focus on: sub weight (40-80Hz), punch/attack (2-5kHz), whether the click/beater cuts through, mono compatibility, any boxiness (200-400Hz), transient shape — is the attack defined or mushy?";
            else if (channelType == "Snare")
                channelFocus = "Focus on: crack and snap (2-4kHz), body (150-300Hz), ring or resonance, transient definition, whether it would cut through a busy mix, any unwanted bleed if it sounds like a live snare.";
            else if (channelType == "Hi-Hat")
                channelFocus = "Focus on: harshness or brittleness (8-12kHz), stereo placement, dynamic consistency, whether it's too loud/quiet relative to typical hat levels, any resonance or ringing.";
            else if (channelType == "Overheads")
                channelFocus = "Focus on: stereo image and width, cymbal clarity vs harshness, low-end bleed from kick/snare, phase coherence (check correlation), whether the overheads give a natural room sound.";
            else if (channelType == "Drum Bus")
                channelFocus = "Focus on: overall drum balance, bus compression character (is it pumping?), transient punch vs glue, tonal balance of the full kit, stereo width of the kit.";
            else if (channelType == "Percussion")
                channelFocus = "Focus on: stereo placement, transient clarity, whether it adds movement or clutters the mix, frequency range — is it competing with other elements?";
            else if (channelType == "Bass / 808")
                channelFocus = "Focus on: sub weight and extension (30-60Hz), mid-range presence for smaller speakers (100-300Hz), mono compatibility (should be nearly 100% mono below 120Hz), distortion/saturation character, whether the 808 tail sustains cleanly or gets muddy, compression and level consistency.";
            else if (channelType == "Bass Guitar")
                channelFocus = "Focus on: low-end body (60-120Hz), finger/pick definition (700Hz-2kHz), string noise, dynamic consistency, whether it would lock with a kick drum, any mud in the 200-400Hz range.";
            else if (channelType == "Sub Bass")
                channelFocus = "Focus on: purity of the sub frequencies (20-60Hz), whether it's truly mono, any harmonic content above 100Hz, level consistency, whether it would cause issues on different playback systems.";
            else if (channelType == "Synth Bass")
                channelFocus = "Focus on: low-end weight, mid-range character and growl, mono compatibility of the lows, whether the sound design is working for the genre, dynamic control.";
            else if (channelType == "Piano")
                channelFocus = "Focus on: natural tone and resonance, dynamic range (should it be more controlled?), low-end buildup vs clarity, stereo image (real piano vs mono synth piano), any harshness in the upper register.";
            else if (channelType == "Keys")
                channelFocus = "Focus on: tonal character, stereo placement, frequency range — is it taking up too much space? Does it need to be EQ'd to fit around vocals? Dynamic control.";
            else if (channelType == "Acoustic Guitar")
                channelFocus = "Focus on: body (100-300Hz) vs string brightness (3-8kHz), pick/strum clarity, room sound, stereo image if double-tracked, any boominess or boxiness, dynamic range.";
            else if (channelType == "Electric Guitar")
                channelFocus = "Focus on: amp tone and saturation, mid-range presence (1-4kHz), low-end mud, high-end fizz from distortion, stereo image (double tracking?), how it would sit in a dense mix.";
            else if (channelType == "Guitar Bus")
                channelFocus = "Focus on: overall guitar balance, stereo spread, tonal consistency across layers, whether it's taking up too much frequency space, bus compression glue.";
            else if (channelType == "Synth Lead")
                channelFocus = "Focus on: presence and cut-through (1-5kHz), stereo placement (leads often work best fairly centred), dynamic control, whether the sound design fits the genre, any harshness or resonance.";
            else if (channelType == "Synth Pad")
                channelFocus = "Focus on: stereo width and spread, frequency range — is it taking up too much space? Low-end content that might conflict with bass, movement and modulation, how it would sit behind vocals and lead elements.";
            else if (channelType == "Synth Pluck")
                channelFocus = "Focus on: transient snap and definition, stereo placement, reverb/delay character, frequency range, whether it cuts through without being harsh.";
            else if (channelType == "Synth Bus")
                channelFocus = "Focus on: overall synth balance, stereo spread, frequency distribution — are the synths fighting each other? Bus processing character.";
            else if (channelType == "Strings")
                channelFocus = "Focus on: naturalness and realism (if sampled), stereo spread, bow noise and articulation, low-mid buildup, whether they add warmth or muddiness, dynamic expression.";
            else if (channelType == "Brass")
                channelFocus = "Focus on: bite and presence (1-4kHz), dynamic punch, low-mid body, whether it cuts through or gets buried, stereo placement.";
            else if (channelType == "Woodwind")
                channelFocus = "Focus on: breathiness and air, presence range, dynamic control, stereo placement, naturalness.";
            else if (channelType == "Orchestral")
                channelFocus = "Focus on: stereo imaging and depth, dynamic range (classical norms are much wider), frequency balance across the full ensemble, room/reverb character.";
            else if (channelType == "FX")
                channelFocus = "Focus on: character and purpose of the effect, whether it adds or clutters, stereo spread, frequency content — is it conflicting with main elements? Level relative to the dry signal.";
            else if (channelType == "Reverb")
                channelFocus = "Focus on: tail length and character, pre-delay, frequency content of the reverb (is the low end too thick?), stereo spread, whether it's adding depth or just mud.";
            else if (channelType == "Delay")
                channelFocus = "Focus on: timing and rhythm, feedback amount, frequency content of the repeats, stereo ping-pong vs mono, whether it's adding space or clutter.";
            else if (channelType == "Foley")
                channelFocus = "Focus on: realism and presence, dynamic range, stereo placement, whether it sits naturally in the soundscape, any unwanted noise floor.";
            else if (channelType == "Ambient")
                channelFocus = "Focus on: texture and atmosphere, stereo field, frequency range, dynamic movement, whether it creates the intended mood.";
            else if (channelType == "Instrument Bus")
                channelFocus = "Focus on: overall instrumental balance, frequency distribution, stereo spread, bus compression character, how it would sit with vocals on top.";
            else if (channelType == "Music Bus")
                channelFocus = "Focus on: overall balance of all musical elements, stereo image, dynamic range, tonal balance — this is everything minus vocals, so think about how it would work as a bed for the vocal.";
        }
        
        if (channelFocus.isNotEmpty())
            prompt += channelFocus + "\n";
        
        prompt += "\n";
        
        // Individual channel rules — remote first, hardcoded fallback
        if (remoteIndividualChannelRules.isNotEmpty())
        {
            prompt += remoteIndividualChannelRules + "\n\n";
        }
        else
        {
            // Hardcoded fallback
            prompt += "INDIVIDUAL CHANNEL RULES:\n";
            prompt += "- Do NOT mention LUFS. Individual channels have no loudness target.\n";
            prompt += "- Do NOT mention correlation. It's not useful for individual instruments.\n";
            prompt += "- Do NOT mention crest factor UNLESS the data says it's extremely squashed (below 3dB). Normal crest values are not worth discussing for individual elements.\n";
            prompt += "- Do NOT mention stereo width on bass — wide bass is a creative choice nowadays.\n";
            prompt += "- Do NOT say 'meters are clean', 'meters are reading clean', 'nothing jumping out', 'SPECTRUM WARNING', or ANY internal label names. Never reference meters/flags/warnings being clean/healthy/fine.\n";
            prompt += "- If the spectrum data shows something wrong for this channel type (e.g. a drum bus with nothing below 10kHz, a kick with no sub, a hi-hat with bass bleed) — tell the user directly in plain language. These are not subtle problems.\n";
            prompt += "- Use the spectrum to inform your feedback even when nothing is obviously wrong. If the balance looks normal, you don't need to mention it. But if something looks unusual (e.g. a vocal with no presence, a snare with no crack), mention it naturally.\n";
            prompt += "- If true peak is clipping, mention it briefly.\n";
            prompt += "- For reverb and delay returns: low-end buildup is common. Mention it and suggest HPF on the return.\n";
            prompt += "- Only mention width if it contradicts expectations.\n\n";
        }
        
        // Response style — remote first, hardcoded fallback
        if (remoteIndividualChannelStyle.isNotEmpty())
        {
            prompt += remoteIndividualChannelStyle + "\n\n";
        }
        else
        {
            // Hardcoded fallback
            prompt += "RESPONSE STYLE FOR INDIVIDUAL CHANNELS:\n";
            prompt += "- NEVER say anything about meters, flags, or internal labels. Don't say 'meters look good', 'nothing flagged', 'clean capture', 'readings are healthy', 'SPECTRUM WARNING', 'APPROACH', or ANY internal label. Pretend the data system doesn't exist — you're just an engineer listening.\n";
            prompt += "- If the data shows a spectrum problem: lead with it conversationally. Example: 'There's nothing going on below 10k here — have you got a filter on this channel somewhere?' Talk like you're standing next to them.\n";
            prompt += "- If the spectrum is FINE: do NOT mention it on the initial review. Don't say 'the frequency balance looks good' or 'spectrum is healthy' or anything about frequencies. Just move on to whatever else you want to talk about. BUT if the user asks about frequencies in a follow-up, describe what the band data shows in plain language — never claim no spectrum data exists when it's in your context.\n";
            prompt += "- The [APPROACH] hint tells you what angle to take THIS time. Follow it — it's how we keep responses varied. But never mention the hint itself or say 'as suggested' or anything that reveals the prompt.\n";
            prompt += "- VARIETY IS EVERYTHING. Each response should feel different. Vary the angle — sometimes ask about the user's vision, sometimes ask what they're trying to fix, sometimes ask what they've tried already, sometimes suggest dropping a reference into Compare Mixes. Do NOT suggest specific plugins, processing chains, or techniques unless (a) the user has explicitly asked for advice, or (b) there's a clear problem flagged in the data and the suggestion directly addresses it. The hint guides which angle to take — follow it.\n";
            prompt += "- If the user asks for plugin suggestions, or you're addressing a flagged problem and a plugin is genuinely the answer, use their ACTUAL plugin names. Reach for specific tools — channel strips, tape sims, console EQs, transient shapers, saturators — and rotate which ones you suggest. Don't default to Pro-Q and Pro-C every time. But the default is NOT to suggest plugins.\n";
            prompt += "- Keep it to 2-3 sentences max unless there's a real spectrum problem.\n\n";
        }
    }
    
    if (pluginList.isNotEmpty())
    {
        prompt += "USER'S PLUGINS (REFERENCE ONLY): " + pluginList + "\n";
        prompt += "This list is for reference only. Do NOT proactively suggest plugins from this list. ONLY mention specific plugins by name if (a) the user explicitly asks for plugin suggestions or for a chain, or (b) you're solving a clear problem flagged in the data and a specific tool is the right answer. When you DO suggest plugins, use their actual names from this list and rotate which ones you suggest — don't default to the same ones every time.\n\n";
    }
    
    prompt += "Audio topics only. Internal genre reference (NEVER say this in your response): " + genre + ". ";
    prompt += "NEVER say the genre name — not \"hip-hop\", \"drill\", \"pop\", \"R&B\", \"rap\", \"trap\", \"grime\" or any other genre name. ";
    prompt += "Don't say \"for hip-hop\" or \"that makes hip-hop work\" or \"in this genre\". Just talk about the mix without naming the genre.\n\n";
    
    prompt += "QUESTION ROUTING — read this carefully before every response:\n";
    prompt += "Figure out what the user is actually asking. There are two kinds of messages:\n";
    prompt += "1. MIX ANALYSIS — the user is asking about the capture/meter data in front of them. Words like 'my mix', 'this track', 'how does it sound', 'what's wrong with it', 'feedback on my capture'. Answer with meter analysis in the style above.\n";
    prompt += "2. GENERAL QUESTION — the user is asking about production, other artists, gear, references, techniques, workflow, or anything not tied to the current capture. Examples: 'how do I get drums to hit like Hitboy', 'what's a good way to mix 808s', 'should I use parallel compression', 'tell me about bus compression', 'how would you EQ vocals'. Answer these directly like an engineer would in the studio. DO NOT pivot to analysing the current capture's meter data. DO NOT open with 'your vocal is sitting in a good spot' or similar. The meter data in earlier context is NOT the subject of the question — ignore it for general questions.\n";
    prompt += "If you're unsure which kind, lean toward GENERAL QUESTION and answer what was asked. You can briefly tie advice back to their mix at the end IF relevant, but never force it.\n";
    prompt += "NEVER reframe a general question as a mix analysis. If the user asks 'how do X hit like Y', answer that question — do not analyse their current capture instead.\n";
    
    // Language preference — appended last so it sits closest to the user
    // turn in the model's attention. "auto" means the AI should match
    // whatever language the user typed in (Claude does this naturally
    // when nothing else is specified, so we omit any instruction). For
    // any specific language, we force it explicitly. Technical audio
    // terms (LUFS, dB, RMS, plugin names) stay in English regardless —
    // translating "Pro-Q 3" into French would just be confusing.
    auto langCode = getChatLanguage();
    if (langCode != "auto" && langCode.isNotEmpty())
    {
        auto langName = chatLanguageDisplayName(langCode);
        // chatLanguageDisplayName returns "Auto (match input)" when the
        // code isn't recognised — guard against that slipping through.
        if (! langName.startsWith("Auto"))
        {
            prompt += "\nIMPORTANT — RESPONSE LANGUAGE: Reply in " + langName
                   + ". Keep technical audio terms, units (LUFS, dB, Hz, ms), "
                     "and plugin or product names in their original English form. "
                     "Everything else — explanations, examples, conversational "
                     "phrasing — should be in " + langName + ".\n";
        }
    }
    
    return prompt;
}

// ============ Settings persistence ============

juce::File EchoJayAPI::getSettingsFile()
{
    auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    return appData.getChildFile("Application Support/EchoJay/auth.json");
#elif JUCE_WINDOWS
    return appData.getChildFile("EchoJay/auth.json");
#else
    return appData.getChildFile(".echojay/auth.json");
#endif
}

void EchoJayAPI::loadSettings()
{
    auto file = getSettingsFile();
    if (!file.existsAsFile()) return;
    
    auto json = juce::JSON::parse(file.loadFileAsString());
    if (auto* obj = json.getDynamicObject())
    {
        apiEndpoint = obj->getProperty("endpoint").toString();
        authToken = obj->getProperty("token").toString();
        userInfo.email = obj->getProperty("email").toString();
        
        // Load tier (new format) with fallback to isPro (old format)
        juce::String tierStr = obj->getProperty("tier").toString();
        if (tierStr.isNotEmpty())
        {
            userInfo.tier = tierStr;
            userInfo.tierLevel = UserInfo::tierStringToLevel(tierStr);
        }
        else
        {
            // Migration from old isPro bool
            bool wasPro = (bool)obj->getProperty("isPro");
            userInfo.tier = wasPro ? "pro" : "free";
            userInfo.tierLevel = wasPro ? 1 : 0;
        }
        
        // Load messageLimit from saved value, or fall back to tier default
        int savedLimit = (int)obj->getProperty("messageLimit");
        userInfo.messageLimit = savedLimit > 0 ? savedLimit : UserInfo::defaultLimitForTier(userInfo.tierLevel);
        
        userInfo.credits = (int)obj->getProperty("credits");
        userInfo.messagesUsedToday = (int)obj->getProperty("messagesUsedToday");
        userInfo.displayName = obj->getProperty("displayName").toString();
        if (userInfo.displayName.isEmpty())
            userInfo.displayName = userInfo.email.upToFirstOccurrenceOf("@", false, false);
        
        // Check if the saved usage is from this period — reset if we've
        // rolled into a new month. Period is monthly across all tiers in
        // the current SaaS. (Field name `messagesUsedToday` is legacy and
        // kept for compat with old saved settings on disk; semantically
        // it now means "messages used this period".)
        // The local cache is just an offline-friendly stale view — the
        // SaaS is the source of truth and the next refreshUserInfo() call
        // will correct any drift. We use YYYY-MM as the comparison key.
        auto savedDate = obj->getProperty("usageDate").toString();
        auto thisMonth = juce::Time::getCurrentTime().formatted("%Y-%m");
        if (savedDate.substring(0, 7) != thisMonth)
            userInfo.messagesUsedToday = 0;
    }
}

void EchoJayAPI::saveSettings() const
{
    auto file = getSettingsFile();
    file.getParentDirectory().createDirectory();
    
    auto obj = new juce::DynamicObject();
    obj->setProperty("endpoint", apiEndpoint);
    obj->setProperty("token", authToken);
    obj->setProperty("email", userInfo.email);
    obj->setProperty("tier", userInfo.tier);
    obj->setProperty("tierLevel", userInfo.tierLevel);
    obj->setProperty("messageLimit", userInfo.messageLimit);
    obj->setProperty("credits", userInfo.credits);
    obj->setProperty("displayName", userInfo.displayName);
    obj->setProperty("messagesUsedToday", userInfo.messagesUsedToday);
    obj->setProperty("usageDate", juce::Time::getCurrentTime().formatted("%Y-%m-%d"));
    
    file.replaceWithText(juce::JSON::toString(juce::var(obj)));
}

// ============ UserSettings JSON ============

juce::String UserSettings::toJSON() const
{
    juce::String json = "{";
    json += "\"name\":" + juce::JSON::toString(name) + ",";
    
    // DAW as JSON array (web app uses "daw" not "daws")
    json += "\"daw\":[";
    for (int i = 0; i < daws.size(); ++i)
    {
        json += juce::JSON::toString(daws[i]);
        if (i < daws.size() - 1) json += ",";
    }
    json += "],";
    
    // Experience as lowercase to match web app (web app uses "beginner", "advanced" etc.)
    juce::String expLower = experienceLevel.toLowerCase();
    json += "\"experience\":" + juce::JSON::toString(expLower) + ",";
    json += "\"monitors\":" + juce::JSON::toString(monitors) + ",";
    json += "\"headphones\":" + juce::JSON::toString(headphones) + ",";
    json += "\"genres\":" + juce::JSON::toString(genres) + ",";
    json += "\"plugins\":" + juce::JSON::toString(plugins);
    json += "}";
    return json;
}

UserSettings UserSettings::fromJSON(const juce::var& json)
{
    UserSettings s;
    if (auto* obj = json.getDynamicObject())
    {
        s.name = obj->getProperty("name").toString();
        
        // Experience: web app uses "experience" (lowercase), VST used "experienceLevel" (capitalized)
        juce::String exp = obj->getProperty("experience").toString();
        if (exp.isEmpty()) exp = obj->getProperty("experienceLevel").toString();
        // Capitalize first letter for internal use
        if (exp.isNotEmpty())
            exp = exp.substring(0, 1).toUpperCase() + exp.substring(1).toLowerCase();
        s.experienceLevel = exp;
        
        s.monitors = obj->getProperty("monitors").toString();
        s.headphones = obj->getProperty("headphones").toString();
        s.genres = obj->getProperty("genres").toString();
        s.plugins = obj->getProperty("plugins").toString();
        
        // DAW: web app uses "daw" (array), VST used "daws" (array)
        auto dawVar = obj->getProperty("daw");
        if (dawVar.isVoid() || dawVar.isUndefined())
            dawVar = obj->getProperty("daws");
        
        if (auto* dawArr = dawVar.getArray())
            for (auto& d : *dawArr)
                s.daws.add(d.toString());
        else if (dawVar.isString() && dawVar.toString().isNotEmpty())
            s.daws.add(dawVar.toString()); // single string from old web app version
    }
    return s;
}

// ============ Settings Sync (via /api/data — profile field) ============

void EchoJayAPI::fetchSettings(std::function<void(bool success)> onComplete)
{
    if (!isLoggedIn())
    {
        if (onComplete) onComplete(false);
        return;
    }
    
    // GET /api/data returns: { chats: [], albums: [], reviews: [], refTracks: [], profile: { name, daws, ... } }
    getJSON("/api/data", [this, onComplete](const juce::var& json, int statusCode)
    {
        if (statusCode == 200 && json.isObject())
        {
            auto* root = json.getDynamicObject();
            if (root)
            {
                // Parse the profile sub-object
                auto profileVar = root->getProperty("profile");
                if (profileVar.isObject())
                {
                    userSettings = UserSettings::fromJSON(profileVar);
                    if (onComplete) onComplete(true);
                    return;
                }
            }
        }
        
        // Fallback: at least get name from /api/me
        getJSON("/api/me", [this, onComplete](const juce::var& json2, int sc2)
        {
            if (sc2 == 200 && json2.isObject())
            {
                auto* root = json2.getDynamicObject();
                if (root)
                {
                    auto userVar = root->getProperty("user");
                    if (auto* userObj = userVar.getDynamicObject())
                    {
                        juce::String name = userObj->getProperty("name").toString();
                        if (name.isNotEmpty() && userSettings.name.isEmpty())
                            userSettings.name = name;
                    }
                }
            }
            if (onComplete) onComplete(true);
        });
    });
}

void EchoJayAPI::saveUserSettings(const UserSettings& settings,
                                    std::function<void(bool success)> onComplete)
{
    if (!isLoggedIn())
    {
        if (onComplete) onComplete(false);
        return;
    }
    
    userSettings = settings;
    
    // First GET the existing data so we don't overwrite chats/albums/reviews
    getJSON("/api/data", [this, settings, onComplete](const juce::var& json, int statusCode)
    {
        // Build the payload using DynamicObject for proper JSON
        auto payload = std::make_unique<juce::DynamicObject>();
        
        // Preserve existing data fields from the GET response
        if (statusCode == 200 && json.isObject())
        {
            auto* root = json.getDynamicObject();
            if (root)
            {
                if (root->hasProperty("chats"))
                    payload->setProperty("chats", root->getProperty("chats"));
                else
                    payload->setProperty("chats", juce::var(juce::Array<juce::var>()));
                    
                if (root->hasProperty("albums"))
                    payload->setProperty("albums", root->getProperty("albums"));
                else
                    payload->setProperty("albums", juce::var(juce::Array<juce::var>()));
                    
                if (root->hasProperty("reviews"))
                    payload->setProperty("reviews", root->getProperty("reviews"));
                else
                    payload->setProperty("reviews", juce::var(juce::Array<juce::var>()));
                    
                if (root->hasProperty("refTracks"))
                    payload->setProperty("refTracks", root->getProperty("refTracks"));
                else
                    payload->setProperty("refTracks", juce::var(juce::Array<juce::var>()));
            }
        }
        else
        {
            payload->setProperty("chats", juce::var(juce::Array<juce::var>()));
            payload->setProperty("albums", juce::var(juce::Array<juce::var>()));
            payload->setProperty("reviews", juce::var(juce::Array<juce::var>()));
            payload->setProperty("refTracks", juce::var(juce::Array<juce::var>()));
        }
        
        // Build profile object
        auto profile = std::make_unique<juce::DynamicObject>();
        profile->setProperty("name", settings.name);
        profile->setProperty("monitors", settings.monitors);
        profile->setProperty("headphones", settings.headphones);
        profile->setProperty("genres", settings.genres);
        profile->setProperty("plugins", settings.plugins);
        profile->setProperty("experience", settings.experienceLevel.toLowerCase());
        
        // DAW array
        juce::Array<juce::var> dawArr;
        for (auto& d : settings.daws)
            dawArr.add(d);
        profile->setProperty("daw", juce::var(dawArr));
        
        payload->setProperty("profile", juce::var(profile.release()));
        
        juce::String body = juce::JSON::toString(juce::var(payload.release()), true);
        
        postJSON("/api/data", body, [onComplete](const juce::var& resp, int sc)
        {
            if (onComplete) onComplete(sc == 200);
        });
    });
}

void EchoJayAPI::updatePluginsFromScanner(const juce::String& scannedPlugins)
{
    // Merge scanned plugins with any manually entered ones from the web app
    juce::StringArray existing;
    existing.addTokens(userSettings.plugins, ",\n", "");
    existing.trim();
    existing.removeEmptyStrings();
    
    juce::StringArray scanned;
    scanned.addTokens(scannedPlugins, ",", "");
    scanned.trim();
    scanned.removeEmptyStrings();
    
    // Add scanned plugins that aren't already in the list
    for (auto& p : scanned)
    {
        bool found = false;
        for (auto& e : existing)
            if (e.containsIgnoreCase(p.upToFirstOccurrenceOf(" (", false, false)))
            { found = true; break; }
        
        if (!found)
            existing.add(p);
    }
    
    existing.sort(true);
    userSettings.plugins = existing.joinIntoString(", ");
}
