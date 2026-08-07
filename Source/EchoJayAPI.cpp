#include "EchoJayAPI.h"
#include "ChainHost.h"    // buildCurrentChainInjection reads the live rack
#include "LinkShm.h"      // RackSidecar — targeted [CURRENT CHAIN] (Phase R)
#include "NativeClip.h"   // EchoJay_NSLog — unified-log diagnostics
#include "EqPresets.h"    // the EQ teaching block lists presets from the table

// Defined later in this file (used by both /api/me parse sites)
static void parseUsagePool(juce::DynamicObject* root, UserInfo& info);
static void parseTierModels(juce::DynamicObject* root, UserInfo& info);

// Forward-declared from PluginProcessor.cpp so we can tag callAsync entry
// points from the API thread. The diagnostic helps identify which async
// path (if any) fires AFTER the plugin has been removed — those are the
// ones that hang Cubase on the user's next message-loop pump (next click).
extern void ejTeardownLog(const juce::String& msg);

// ===========================================================================
// Development transport (Session B): COMPILED OUT unless ECHOJAY_DEV_TRANSPORT
// ===========================================================================
// Every /api/v2/* route is reachable only on a Vercel preview deployment,
// which needs two things a release build must never contain: a base URL that
// changes on every deploy, and the project's protection-bypass secret.
//
// Both live in ~/.echojay/dev.json, OUTSIDE the repo so they cannot be
// committed, and the whole mechanism sits behind the compile guard so the
// strings are absent from a release binary rather than merely unused. See
// the option block in CMakeLists.txt.
//
// Diagnostics this shape gives you, from session-b-plugin-kickoff.md section 6:
//   302 -> the bypass header is missing or wrong
//   401 -> the bypass worked, the bearer token did not
//   404 not_enabled -> you are hitting production, not a preview
#if ECHOJAY_DEV_TRANSPORT
namespace
{
    struct DevTransport
    {
        juce::String baseUrl;
        juce::String bypass;
        bool loaded = false;
    };

    // Read once per process. A dev config that appears mid-session is not
    // worth a stat on every request.
    const DevTransport& devTransport()
    {
        static DevTransport dt = []
        {
            DevTransport out;
            auto f = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                         .getChildFile(".echojay").getChildFile("dev.json");
            if (! f.existsAsFile()) return out;
            auto parsed = juce::JSON::parse(f.loadFileAsString());
            if (auto* obj = parsed.getDynamicObject())
            {
                out.baseUrl = obj->getProperty("baseUrl").toString().trim();
                out.bypass  = obj->getProperty("protectionBypass").toString().trim();
                out.loaded  = out.baseUrl.isNotEmpty() || out.bypass.isNotEmpty();
            }
            return out;
        }();
        return dt;
    }
}
#endif

// Base URL for a request: the dev override when one is configured, otherwise
// whatever the plugin normally talks to. In a release build this is the
// identity function and the compiler removes it.
juce::String EchoJayAPI::transportEndpoint(const juce::String& configured)
{
   #if ECHOJAY_DEV_TRANSPORT
    const auto& dt = devTransport();
    if (dt.baseUrl.isNotEmpty()) return dt.baseUrl;
   #endif
    return configured;
}

// Extra request headers the dev transport needs. Empty in a release build.
juce::String EchoJayAPI::transportHeaders()
{
   #if ECHOJAY_DEV_TRANSPORT
    const auto& dt = devTransport();
    if (dt.bypass.isNotEmpty())
        return "x-vercel-protection-bypass: " + dt.bypass + "\r\n";
   #endif
    return {};
}

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
            // Bail immediately if the plugin has been removed. Without this the
            // detached worker keeps running inside the host process for up to
            // a minute on a dead socket, holding OS resources that outlive the
            // plugin module. On Windows that surfaces as Cubase freezing a few
            // seconds after removal, with a perfectly clean teardown log
            // (this thread is not part of teardown and leaves no trace).
            if (!aliveFlag->load()) return;

            juce::URL url(transportEndpoint(endpoint) + path);
            url = url.withPOSTData(body);

            juce::String headers = "Content-Type: application/json\r\n";
            if (token.isNotEmpty())
                headers += "Authorization: Bearer " + token + "\r\n";
            headers += transportHeaders();   // empty in a release build

            statusCode = 0;
            auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                               .withExtraHeaders(headers)
                               .withConnectionTimeoutMs(60000)
                               .withStatusCode(&statusCode);

            auto stream = url.createInputStream(options);

            if (stream != nullptr)
            {
                juce::MemoryBlock mb;
                stream->readIntoMemoryBlock(mb);
                auto responseText = juce::String::fromUTF8((const char*)mb.getData(), (int)mb.getSize());
                json = juce::JSON::parse(responseText);
                break; // got a response (any status) — stop retrying
            }

            DBG("[EchoJay] postJSON connection failed (attempt " << attempt
                << "/" << maxAttempts << ") path=" << path << " statusCode=" << statusCode);

            // Bail out of the backoff sleep too if torn down mid-retry.
            if (!aliveFlag->load()) return;
            if (attempt < maxAttempts)
                juce::Thread::sleep(attempt * 1000); // 1s, then 2s backoff
        }

        auto callback = cb;
        auto sc = statusCode;
        auto j = json;
        // Do NOT post back into the host message queue if we have already been
        // torn down. A request can be in flight for up to ~3 minutes (60s
        // timeout x 3 attempts), long after the user removed the plugin. If we
        // post a callAsync after teardown, it sits in the host's message queue
        // and fires on the host's NEXT message-loop pump — i.e. the user's next
        // mouse click — invoking a callback that captures now-destroyed objects.
        // That is the "Cubase freezes on my first click after removing the
        // plugin" report. Checking alive BEFORE posting (not just inside the
        // lambda) means a request that finishes after removal quietly does
        // nothing instead of queueing a time-bomb into the host loop.
        if (!aliveFlag->load()) return;
        juce::MessageManager::callAsync([callback, j, sc, aliveFlag]() {
            ejTeardownLog("[callAsync] postJSON completion firing");
            if (!aliveFlag->load()) { ejTeardownLog("[callAsync] postJSON: alive=false, bailing"); return; }
            (*callback)(j, sc);
            ejTeardownLog("[callAsync] postJSON completion done");
        });
    });
}

// ============ Generic PATCH helper ============
//
// Same teardown discipline as postJSON, and the same reason for it: a request
// can be in flight for minutes after the plugin is removed, and a callAsync
// posted after teardown fires on the host's next message pump.
//
// PATCH is NOT retried on a connection failure. postJSON retries because its
// failures never reached the server; that reasoning does not carry to a verb
// that overwrites an existing chain. A dropped connection after the server
// committed the write is indistinguishable here from one before it, so a
// retry could silently apply the same overwrite twice. One attempt, and an
// honest error the user can act on.
void EchoJayAPI::patchJSON(const juce::String& path, const juce::String& body,
                           std::function<void(const juce::var& json, int statusCode)> onComplete)
{
    auto endpoint = apiEndpoint;
    auto token = authToken;
    auto cb = std::make_shared<std::function<void(const juce::var&, int)>>(onComplete);
    auto aliveFlag = alive;

    juce::Thread::launch([=]()
    {
        if (!aliveFlag->load()) return;

        juce::URL url(transportEndpoint(endpoint) + path);
        url = url.withPOSTData(body);

        juce::String headers = "Content-Type: application/json\r\n";
        if (token.isNotEmpty())
            headers += "Authorization: Bearer " + token + "\r\n";
        headers += transportHeaders();   // empty in a release build

        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                           .withExtraHeaders(headers)
                           .withConnectionTimeoutMs(60000)
                           .withStatusCode(&statusCode)
                           .withHttpRequestCmd("PATCH");

        auto stream = url.createInputStream(options);

        juce::var json;
        if (stream != nullptr)
        {
            juce::MemoryBlock mb;
            stream->readIntoMemoryBlock(mb);
            auto responseText = juce::String::fromUTF8((const char*)mb.getData(), (int)mb.getSize());
            json = juce::JSON::parse(responseText);
        }

        auto callback = cb;
        auto sc = statusCode;
        auto j = json;
        if (!aliveFlag->load()) return;
        juce::MessageManager::callAsync([callback, j, sc, aliveFlag]() {
            ejTeardownLog("[callAsync] patchJSON completion firing");
            if (!aliveFlag->load()) { ejTeardownLog("[callAsync] patchJSON: alive=false, bailing"); return; }
            (*callback)(j, sc);
            ejTeardownLog("[callAsync] patchJSON completion done");
        });
    });
}

// ============ Generic DELETE helper ============
//
// No body, no retry. Same teardown discipline as the others: a request can be
// in flight for a long time after the plugin is removed, and a callAsync
// posted after teardown fires on the host's next message pump.
void EchoJayAPI::deleteJSON(const juce::String& path,
                            std::function<void(const juce::var& json, int statusCode)> onComplete)
{
    auto endpoint = apiEndpoint;
    auto token = authToken;
    auto cb = std::make_shared<std::function<void(const juce::var&, int)>>(onComplete);
    auto aliveFlag = alive;

    juce::Thread::launch([=]()
    {
        if (!aliveFlag->load()) return;

        juce::URL url(transportEndpoint(endpoint) + path);

        juce::String headers;
        if (token.isNotEmpty())
            headers += "Authorization: Bearer " + token + "\r\n";
        headers += transportHeaders();   // empty in a release build

        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withExtraHeaders(headers)
                           .withConnectionTimeoutMs(kDefaultGetTimeoutMs)
                           .withStatusCode(&statusCode)
                           .withHttpRequestCmd("DELETE");

        auto stream = url.createInputStream(options);

        juce::var json;
        if (stream != nullptr)
        {
            juce::MemoryBlock mb;
            stream->readIntoMemoryBlock(mb);
            auto responseText = juce::String::fromUTF8((const char*)mb.getData(), (int)mb.getSize());
            json = juce::JSON::parse(responseText);
        }

        auto callback = cb;
        auto sc = statusCode;
        auto j = json;
        if (!aliveFlag->load()) return;
        juce::MessageManager::callAsync([callback, j, sc, aliveFlag]() {
            ejTeardownLog("[callAsync] deleteJSON completion firing");
            if (!aliveFlag->load()) { ejTeardownLog("[callAsync] deleteJSON: alive=false, bailing"); return; }
            (*callback)(j, sc);
            ejTeardownLog("[callAsync] deleteJSON completion done");
        });
    });
}

// ============ Saved chains: one place that turns a failure into a sentence ==
//
// Codes are from session-b-plugin-kickoff.md section 6 and the backend's
// lib/dash/chains.js. Two rules the wording has to hold to:
//  - a 413 names the plugin problem in terms of what the user should do, and
//    never claims the save failed outright, because the chain still saves
//    with that slot nulled;
//  - "not enabled yet" is never reported as "not found". They are different
//    facts and only one of them is the user's problem.
juce::String EchoJayAPI::chainErrorMessage(const juce::var& json, int statusCode,
                                           ChainOp op)
{
    if (statusCode >= 200 && statusCode < 300) return {};

    juce::String code, message;
    if (auto* obj = json.getDynamicObject())
    {
        code    = obj->getProperty("error").toString();
        message = obj->getProperty("message").toString();
    }

    // NO CONNECTION. Worth retrying, and the wording must say so. Saying
    // "your chain has not been saved" when the user was only LOOKING at a
    // list sends them hunting a bug that does not exist.
    if (statusCode == 0)
    {
        if (op == ChainOp::List)
            return "Could not reach EchoJay, so this list may be out of date.";
        if (op == ChainOp::Open)
            return "Could not reach EchoJay, so that chain could not be opened."
                   " It is still saved; try again when you are back online.";
        return "Could not reach EchoJay. Your chain has not been saved yet.";
    }
    if (statusCode == 302)
        return "The development transport is not configured for this deployment.";
    if (statusCode == 401)
        return op == ChainOp::Save ? "Your session has expired. Sign in again to save chains."
                                   : "Your session has expired. Sign in again to see your chains.";
    if (statusCode == 404 && code == "not_enabled")
        return "Saved chains are not switched on for this build yet.";
    // GONE, not unreachable. Retrying will never help, and a cached row that
    // outlived its chain lands here: the distinction is the whole point.
    if (statusCode == 404)
        return op == ChainOp::Open
                 ? "That chain no longer exists. It may have been deleted somewhere else."
                 : "That chain no longer exists.";
    if (statusCode == 402 || code == "chain_limit_reached")
        return "You have reached the free limit of saved chains.";
    if (code == "state_slot_too_large" || code == "state_total_too_large")
        return "Some plugin settings were too large to save with this chain.";
    if (statusCode == 503)
        return op == ChainOp::Save
                 ? "EchoJay is unavailable right now. Your chain has not been saved."
                 : "EchoJay is unavailable right now.";

    // Anything else: prefer the server's own sentence, which names the
    // problem, over a generic one that does not.
    if (message.isNotEmpty()) return message;
    if (op == ChainOp::List) return "Your chains could not be loaded (error "
                                  + juce::String(statusCode) + ").";
    if (op == ChainOp::Open) return "That chain could not be opened (error "
                                  + juce::String(statusCode) + ").";
    return "That chain could not be saved (error " + juce::String(statusCode) + ").";
}

// ============ Generic GET helper ============

void EchoJayAPI::getJSON(const juce::String& path,
                          std::function<void(const juce::var& json, int statusCode)> onComplete,
                          int timeoutMs)
{
    auto endpoint = apiEndpoint;
    auto token = authToken;
    auto devId = deviceId;
    auto cb = std::make_shared<std::function<void(const juce::var&, int)>>(onComplete);
    auto aliveFlag = alive; // capture shared_ptr by value — prevent use-after-free
    
    juce::Thread::launch([=]()
    {
        // Bail immediately if the plugin was removed before this thread ran.
        // See postJSON for why: a detached worker on a dead socket outliving
        // the plugin module is what freezes the host seconds after removal.
        if (!aliveFlag->load()) return;

        juce::URL url(transportEndpoint(endpoint) + path);
        
        juce::String headers;
        if (token.isNotEmpty())
            headers += "Authorization: Bearer " + token + "\r\n";
        if (devId.isNotEmpty())
            headers += "x-device-id: " + devId + "\r\n";
        headers += transportHeaders();   // empty in a release build
        
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withExtraHeaders(headers)
                           .withConnectionTimeoutMs(timeoutMs)
                           .withStatusCode(&statusCode);
        
        auto stream = url.createInputStream(options);
        
        juce::var json;
        if (stream != nullptr)
        {
            juce::MemoryBlock mb;
            stream->readIntoMemoryBlock(mb);
            auto responseText = juce::String::fromUTF8((const char*)mb.getData(), (int)mb.getSize());
            json = juce::JSON::parse(responseText);
        }
        
        auto callback = cb;
        auto sc = statusCode;
        auto j = json;
        // See postJSON: do not queue a callback into the host message loop
        // after teardown, or it fires on the user's next click and touches
        // destroyed objects (Cubase freeze on first click after removal).
        if (!aliveFlag->load()) return;
        juce::MessageManager::callAsync([callback, j, sc, aliveFlag]() {
            ejTeardownLog("[callAsync] getJSON completion firing");
            if (!aliveFlag->load()) { ejTeardownLog("[callAsync] getJSON: alive=false, bailing"); return; }
            (*callback)(j, sc);
            ejTeardownLog("[callAsync] getJSON completion done");
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
                
                parseUsagePool(obj, userInfo);   // usage-v2 (additive)
                parseTierModels(obj, userInfo);  // tier bar labels (additive)

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

// ============ Device pairing (Sign in with browser) ============
// See the header comment. Deliberately does NOT share code with login():
// the password path stays byte-identical; this is a parallel way to end up
// holding the same token.
void EchoJayAPI::startDeviceLogin(std::function<void(const juce::String&)> onCode,
                                  std::function<void(bool, const juce::String&)> onComplete)
{
    cancelDeviceLogin();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    devicePairCancelled_ = cancelled;

    juce::String body = "{\"deviceId\":" + juce::JSON::toString(deviceId) + "}";
    postJSON("/api/device/start", body, [this, cancelled, onCode, onComplete](const juce::var& json, int statusCode)
    {
        if (cancelled->load()) return;
        auto* obj = json.getDynamicObject();
        if (statusCode != 200 || obj == nullptr)
        {
            if (onComplete) onComplete(false, "Could not reach the sign-in service. Check your connection and try again.");
            return;
        }
        const auto userCode   = obj->getProperty("userCode").toString();
        const auto deviceCode = obj->getProperty("deviceCode").toString();
        const auto verifyUrl  = obj->getProperty("verifyUrl").toString();
        int interval  = (int) obj->getProperty("interval");
        int expiresIn = (int) obj->getProperty("expiresIn");
        if (interval < 3)   interval = 5;
        if (expiresIn <= 0) expiresIn = 600;
        if (userCode.isEmpty() || deviceCode.isEmpty() || verifyUrl.isEmpty())
        {
            if (onComplete) onComplete(false, "Sign-in service returned an unexpected response. Try again.");
            return;
        }
        if (onCode) onCode(userCode);
        juce::URL(verifyUrl).launchInDefaultBrowser();
        pollDeviceCode(deviceCode, interval * 1000, expiresIn / interval + 4, cancelled, onComplete);
    });
}

void EchoJayAPI::cancelDeviceLogin()
{
    if (devicePairCancelled_) devicePairCancelled_->store(true);
    devicePairCancelled_.reset();
}

void EchoJayAPI::pollDeviceCode(const juce::String& deviceCode, int intervalMs, int attemptsLeft,
                                std::shared_ptr<std::atomic<bool>> cancelled,
                                std::function<void(bool, const juce::String&)> onComplete)
{
    auto aliveFlag = alive;
    juce::Timer::callAfterDelay(intervalMs, [this, aliveFlag, cancelled, deviceCode, intervalMs, attemptsLeft, onComplete]()
    {
        if (!aliveFlag->load() || cancelled->load()) return;
        if (attemptsLeft <= 0)
        {
            if (onComplete) onComplete(false, "The sign-in code expired. Try again.");
            return;
        }
        juce::String body = "{\"deviceCode\":" + juce::JSON::toString(deviceCode) + "}";
        postJSON("/api/device/poll", body, [this, aliveFlag, cancelled, deviceCode, intervalMs, attemptsLeft, onComplete](const juce::var& json, int statusCode)
        {
            if (!aliveFlag->load() || cancelled->load()) return;
            auto* obj = json.getDynamicObject();
            const auto st = obj != nullptr ? obj->getProperty("status").toString() : juce::String();

            if (statusCode == 429)
            {
                // slow_down: back off by half an interval and keep waiting
                pollDeviceCode(deviceCode, intervalMs + intervalMs / 2, attemptsLeft - 1, cancelled, onComplete);
                return;
            }
            if (statusCode == 403)
            {
                // device-limit: surface the server's message, same as login
                juce::String err = "Device limit reached.";
                if (obj != nullptr && obj->hasProperty("error")) err = obj->getProperty("error").toString();
                if (onComplete) onComplete(false, err);
                return;
            }
            if (statusCode == 200 && st == "authorised" && obj != nullptr && obj->hasProperty("token"))
            {
                authToken = obj->getProperty("token").toString();

                // Same payload shape as /api/login: parse identically so the
                // post-pairing state matches a password login exactly
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
                parseUsagePool(obj, userInfo);   // usage-v2 (additive)
                auto usageVar = obj->getProperty("usage");
                if (auto* usageObj = usageVar.getDynamicObject())
                {
                    userInfo.messagesUsedToday = (int) usageObj->getProperty("messagesUsedToday");
                    userInfo.messageLimit = (int) usageObj->getProperty("messagesPerDay");
                    if (usageObj->hasProperty("credits"))
                        userInfo.credits = (int) usageObj->getProperty("credits");
                }
                if (userInfo.messageLimit <= 0)
                    userInfo.messageLimit = UserInfo::defaultLimitForTier(userInfo.tierLevel);
                if (userInfo.displayName.isEmpty())
                    userInfo.displayName = userInfo.email.upToFirstOccurrenceOf("@", false, false);

                saveSettings();
                refreshUserInfo(nullptr);
                if (onComplete) onComplete(true, "");
                return;
            }
            if (statusCode == 200 && st == "pending")
            {
                pollDeviceCode(deviceCode, intervalMs, attemptsLeft - 1, cancelled, onComplete);
                return;
            }
            if (statusCode == 200 && st == "expired")
            {
                if (onComplete) onComplete(false, "The sign-in code expired. Try again.");
                return;
            }
            // Transient failure (connection blip / 5xx): keep the cadence
            pollDeviceCode(deviceCode, intervalMs, attemptsLeft - 1, cancelled, onComplete);
        });
    });
}

void EchoJayAPI::logout()
{
    authToken = "";
    userInfo = UserInfo();
    saveSettings();
}

// Dev credit-state simulation, no real credits burned. Checked at most
// every 2s so timer-rate callers stay cheap; delete the file(s) to restore
// live behaviour within 2s.
//   ~/Library/EchoJay/simulate_no_credits  (any content) -> 0 usable left
//   ~/Library/EchoJay/simulate_credits     (integer N)   -> N usable left
//     (e.g. echo 2 > simulate_credits to see the amber state)
// Returns -1 when no simulation is active.
static int simulatedCreditsOverride()
{
    static int value = -1;
    static juce::int64 lastCheck = 0;
    auto now = juce::Time::currentTimeMillis();
    if (now - lastCheck > 2000)
    {
        lastCheck = now;
        auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("EchoJay");
        if (dir.getChildFile("simulate_no_credits").existsAsFile())
            value = 0;
        else
        {
            auto f = dir.getChildFile("simulate_credits");
            value = f.existsAsFile() ? juce::jmax(0, f.loadFileAsString().trim().getIntValue())
                                     : -1;
        }
    }
    return value;
}

bool EchoJayAPI::canSendMessage() const
{
    if (int ov = simulatedCreditsOverride(); ov >= 0) return ov > 0;
    // FREE V2 two-lane: this is the CHAT lane — daily pool, never
    // credit-extended (mirrors the server gate exactly)
    if (userInfo.usagePool.twoLane())
        return userInfo.usagePool.chats.used < userInfo.usagePool.chats.pool;
    // usage-v2 legacy single pool: at 100 percent with zero credits the
    // server refuses the turn
    if (userInfo.usagePool.present)
        return userInfo.usagePool.percent < 100.0f || userInfo.usagePool.credits > 0;
    return userInfo.messagesUsedToday < (userInfo.messageLimit + userInfo.credits);
}

bool EchoJayAPI::isPremiumExhausted() const
{
    if (int ov = simulatedCreditsOverride(); ov >= 0) return ov == 0;
    // Only the two-lane free contract has a distinct premium pool; every
    // other state answers via the general gate so locks behave consistently.
    if (userInfo.usagePool.twoLane())
        return userInfo.usagePool.premium.used >= userInfo.usagePool.premium.pool
            && userInfo.usagePool.credits <= 0;
    return !canSendMessage();
}

static bool isPremiumTurnType(const juce::String& t)
{
    return t == "capture_analysis" || t == "link_analysis"
        || t == "chain_generate"  || t == "version_compare";
}

bool EchoJayAPI::canSendTurn(const juce::String& turnType) const
{
    if (isPremiumTurnType(turnType)) return !isPremiumExhausted();
    return canSendMessage();
}

int EchoJayAPI::getRemainingMessages() const
{
    if (int ov = simulatedCreditsOverride(); ov >= 0)
        return juce::jmin(ov, userInfo.messageLimit);
    return juce::jmax(0, userInfo.messageLimit - userInfo.messagesUsedToday);
}


juce::String EchoJayAPI::getLimitReachedMessage() const
{
    // FREE two-lane: the no-turnType call is the CHAT (daily) gate. Name the
    // pool + its reset; note premium is still available. No "credits".
    if (userInfo.usagePool.twoLane())
        return "Your daily chats are used up. They reset tomorrow. "
               "Premium actions are still available.";
    // Legacy/paid single pool: name the reset, no counts, no "credits".
    if (getUsagePeriod() == "monthly")
        return "You've used your monthly usage. It resets on the 1st.";
    return "You've used today's usage. It resets tomorrow.";
}

juce::String EchoJayAPI::getLimitReachedMessage(const juce::String& turnType) const
{
    // FREE two-lane premium (monthly) gate — name the pool + its reset.
    if (userInfo.usagePool.twoLane() && isPremiumTurnType(turnType))
        return "You've used your monthly premium actions. They reset on the 1st.";
    return getLimitReachedMessage();
}

// Always-present tier-model names ("tierModels" root object, additive):
// FIXED per tier, they drive the Settings usage bar labels for every tier.
// Absent or partial object leaves the names empty (labels fall back).
static void parseTierModels(juce::DynamicObject* root, UserInfo& info)
{
    info.tierModels = {};
    if (root == nullptr) return;
    if (auto* tm = root->getProperty("tierModels").getDynamicObject())
    {
        info.tierModels.chat    = tm->getProperty("chat").toString().trim();
        info.tierModels.premium = tm->getProperty("premium").toString().trim();
    }
}

// usage-v2: parse the additive usagePool object from an /api/me (or login)
// response root. Absent -> present=false and the legacy fields drive a
// locally-computed percent. Unknown sub-fields (nudge etc.) parse loosely —
// never crash on new server fields.
static void parseUsagePool(juce::DynamicObject* root, UserInfo& info)
{
    info.usagePool = {};
    if (root == nullptr || !root->hasProperty("usagePool")) return;
    auto* up = root->getProperty("usagePool").getDynamicObject();
    if (up == nullptr) return;
    auto& u = info.usagePool;
    u.present       = true;
    // FREE V2 two-lane sub-objects (premium monthly / chats daily)
    auto parseLane = [](juce::DynamicObject* pool, const char* key,
                        UserInfo::UsagePool::Lane& lane)
    {
        if (auto* l = pool->getProperty(key).getDynamicObject())
        {
            lane.present = true;
            lane.used    = (int) l->getProperty("used");
            lane.pool    = (int) l->getProperty("pool");
            lane.percent = (float)(double) l->getProperty("percent");
            lane.resetAt = l->getProperty("resetAt").toString();
        }
    };
    parseLane(up, "premium", u.premium);
    parseLane(up, "chats",   u.chats);
    u.used          = (int) up->getProperty("used");
    u.pool          = (int) up->getProperty("pool");
    u.percent       = (float)(double) up->getProperty("percent");
    u.period        = up->getProperty("period").toString();
    u.resetAt       = up->getProperty("resetAt").toString();
    u.credits       = (int) up->getProperty("credits");
    u.tierLabel     = up->getProperty("tierLabel").toString();
    u.capacityLabel = up->getProperty("capacityLabel").toString();
    if (auto* model = up->getProperty("model").getDynamicObject())
    {
        u.modelFast      = (bool) model->getProperty("fast");
        u.modelName      = model->getProperty("name").toString();
        u.tasteRemaining = (int)  model->getProperty("tasteRemaining");
    }
    if (up->hasProperty("nudge"))
        u.nudge = up->getProperty("nudge").toString();   // parsed, unused
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
    getJSON(juce::String("/api/me?appVersion=") + JucePlugin_VersionString,
            [this, onComplete](const juce::var& json, int statusCode)
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
                parseUsagePool(root, userInfo);   // usage-v2 (additive)
                parseTierModels(root, userInfo);  // tier bar labels (additive)
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
                           std::function<void(const juce::String& reply, bool success)> onComplete,
                           const juce::String& meterJsonBlob)
{
    // FREE V2: the gate is LANE-AWARE — the staged turnType decides whether
    // this send draws the daily chat pool or the monthly premium pool, so a
    // spent premium pool never blocks plain chat and vice versa.
    const juce::String gateTurnType = nextChatTurnType_;
    if (!canSendTurn(gateTurnType))
    {
        // AUTH CACHE REFRESH — local cache says we're over limit, but the user may have
        // upgraded tier or credits since the last server sync. Refresh from /api/me first,
        // and only show "limit reached" if the server ALSO agrees we're at the limit.
        // This is the self-healing path for users who upgrade mid-session.
        auto aliveFlag = alive;
        refreshUserInfo([this, roles, contents, systemPrompt, onComplete, aliveFlag, meterJsonBlob, gateTurnType](bool refreshSuccess)
        {
            if (!aliveFlag->load()) return;

            if (refreshSuccess && canSendTurn(gateTurnType))
            {
                // Refresh revealed we actually CAN send (tier upgraded, credits added, new day, etc).
                // Retry the send as if the limit error never happened.
                sendChat(roles, contents, systemPrompt, onComplete, meterJsonBlob);
                return;
            }

            // Server confirms we're at the limit (or refresh failed — fall back to cached state).
            // Drop ALL staged per-turn state: a blocked capture's payload
            // must not leak onto the next plain chat send.
            if (nextChatMeters_.isNotEmpty() || nextChatIsExplicitCapture_)
                EchoJay_NSLog("EJChat: staged capture payload dropped (limit reached)");
            nextChatMeters_.clear();
            nextChatTurnType_.clear();
            nextChatBusCount_ = 0;
            nextChatIsExplicitCapture_ = false;
            onComplete(getLimitReachedMessage(gateTurnType), false);
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

    // Byte-budget the payload on top of the message-count cap. Individual
    // turns can be huge (capture turns embed meter context, Compare turns a
    // full compareCtx, user turns the AVAILABLE PLUGINS injection), so twelve
    // messages could still be hundreds of KB. Walk backwards from the newest
    // accumulating stripped-content size; older messages fall off first. The
    // newest message is always sent regardless of size.
    constexpr int maxPayloadBytes = 60000;

    // Per-turn injection blocks (plugin lists + chain rules) only matter on
    // the CURRENT message — strip them from history turns before sizing.
    // The marker list lives in historyStripMarkers() (ONE list, shared with
    // the chain-guidance self-test's coverage check).
    auto strippedContent = [&contents](int i) -> juce::String
    {
        auto c = contents[i];
        for (const auto& marker : historyStripMarkers())
        {
            int cut = c.indexOf(marker);
            if (cut >= 0) c = c.substring(0, cut);
        }
        // Pre-usage-v2 history persisted [LIVE METER] blocks inside message
        // text — band data must not ride out on ANY turn, including history
        int lm = c.indexOf("\n\n[LIVE METER");
        if (lm >= 0) c = c.substring(0, lm);
        return c;
    };

    {
        int budget = maxPayloadBytes - (int)contents[roles.size() - 1].getNumBytesAsUTF8();
        int cutIdx = roles.size() - 1;
        while (cutIdx > firstIdx)
        {
            int sz = (int)strippedContent(cutIdx - 1).getNumBytesAsUTF8();
            if (budget - sz < 0) break;
            budget -= sz;
            --cutIdx;
        }
        firstIdx = juce::jmax(firstIdx, cutIdx);
    }

    // Anthropic API requires the first message in messages[] to be 'user'.
    // If trimming landed us on 'assistant', skip forward by one to land on user.
    while (firstIdx < roles.size() && roles[firstIdx] != "user")
        firstIdx++;
    if (firstIdx >= roles.size())          // degenerate: always send the newest
        firstIdx = roles.size() - 1;

    // Newest message: keep its live injection, but any meter/band TEXT is
    // gated on the same explicit-capture flag as the meters blob — a
    // [LIVE METER] section on a plain chat turn gets scrubbed and logged.
    juce::String newestContent = contents[roles.size() - 1];
    if (!nextChatIsExplicitCapture_)
    {
        int lm = newestContent.indexOf("\n\n[LIVE METER");
        if (lm >= 0)
        {
            newestContent = newestContent.substring(0, lm);
            EchoJay_NSLog("EJChat: SCRUBBED [LIVE METER] text from outgoing message (no explicit-capture flag)");
        }
    }
    const int newestMsgBytes = (int) newestContent.getNumBytesAsUTF8();

    juce::String messagesJson = "[";
    messagesJson += "{\"role\":\"system\",\"content\":" + juce::JSON::toString(systemPrompt) + "}";
    for (int i = firstIdx; i < roles.size(); ++i)
    {
        // History messages go out with injections + meter text stripped;
        // only the newest keeps its (scrubbed) full content
        juce::String c = (i == roles.size() - 1) ? newestContent : strippedContent(i);
        messagesJson += ",{\"role\":" + juce::JSON::toString(roles[i]) +
                        ",\"content\":" + juce::JSON::toString(c) + "}";
    }
    messagesJson += "]";
    // Field-by-field breakdown of the messages array so nothing hides:
    // system prompt vs history vs the newest message, each in bytes
    {
        int histBytes = 0;
        for (int i = firstIdx; i < roles.size() - 1; ++i)
            histBytes += (int) strippedContent(i).getNumBytesAsUTF8();
        EchoJay_NSLog(("EJChat: body breakdown -- system="
                       + juce::String((int) systemPrompt.getNumBytesAsUTF8()) + "b"
                       + " history=" + juce::String(histBytes) + "b("
                       + juce::String(roles.size() - 1 - firstIdx) + " msgs)"
                       + " newest=" + juce::String(newestMsgBytes) + "b"
                       + " messagesTotal=" + juce::String((int) messagesJson.getNumBytesAsUTF8()) + "b").toRawUTF8());
    }
    
    juce::String body = "{\"messages\":" + messagesJson + ",\"max_tokens\":4096";
    // usage-v2 client contract: version identifier + turnType on EVERY turn.
    // turnType is staged per send ("" = plain "chat"); capture payloads only
    // ride on explicit capture turns (the callers enforce that pairing).
    body += ",\"appVersion\":\"" + juce::String(JucePlugin_VersionString) + "\"";
    // Auto-dial mode rides EVERY chat turn when on; the server only acts on
    // it for chain turns with a live plugin feed and ignores it elsewhere.
    if (autoDialMode)
        body += ",\"autoDial\":true";
    if (! nextDialFlags_.isEmpty())
    {
        juce::Array<juce::var> arr;
        for (auto& n : nextDialFlags_) arr.add(n);
        body += ",\"dialFlags\":" + juce::JSON::toString(juce::var(arr), true);
        nextDialFlags_.clear();
    }

    // Meter/band payload: EXPLICIT CAPTURE ONLY. Anything staged (or passed
    // via the legacy parameter) without the flag is discarded loudly — a
    // stray blob on a plain chat turn made the server classify it as
    // capture_analysis / flow live.
    juce::String metersBlob;
    if (nextChatIsExplicitCapture_)
        metersBlob = nextChatMeters_.isNotEmpty() ? nextChatMeters_ : meterJsonBlob;
    else if (nextChatMeters_.isNotEmpty() || meterJsonBlob.isNotEmpty())
        EchoJay_NSLog("EJChat: DISCARDED stray meter blob (no explicit-capture flag)");
    nextChatMeters_.clear();

    {
        juce::String tt = nextChatTurnType_.isNotEmpty() ? nextChatTurnType_ : "chat";
        if (!nextChatIsExplicitCapture_
            && (tt == "capture_analysis" || tt == "link_analysis"))
        {
            EchoJay_NSLog(("EJChat: turnType " + tt
                           + " downgraded to chat (no explicit-capture flag)").toRawUTF8());
            tt = "chat";
            nextChatBusCount_ = 0;
        }
        if (tt == "capture_analysis" && metersBlob.isEmpty())
        {
            // NEVER capture_analysis without a payload (server 400s it)
            EchoJay_NSLog("EJChat: capture_analysis without payload downgraded to chat");
            tt = "chat";
        }
        body += ",\"turnType\":" + juce::JSON::toString(tt);
        if (nextChatBusCount_ > 0)
            body += ",\"busCount\":" + juce::String(nextChatBusCount_);
        // Per-send verification line: turn class + whether a payload rode
        EchoJay_NSLog(("EJChat: send turnType=" + tt
                       + " autoDial=" + (autoDialMode ? juce::String("on") : juce::String("off"))
                       + (nextChatBusCount_ > 0 ? " busCount=" + juce::String(nextChatBusCount_)
                                                : juce::String())
                       + " payload=" + (metersBlob.isNotEmpty()
                            ? "YES (" + juce::String((int) metersBlob.getNumBytesAsUTF8()) + "b)"
                            : juce::String("NO"))
                       + " msg=" + juce::String(newestMsgBytes) + "b").toRawUTF8());
        nextChatTurnType_.clear();
        nextChatBusCount_ = 0;
        nextChatIsExplicitCapture_ = false;   // cleared after EVERY send
    }
    if (metersBlob.isNotEmpty())
    {
        // Raw passthrough — the blob is already a JSON object; splicing it in
        // unchanged preserves the absent-key convention field for field
        // (backend parseExtendedMeter reads psr/plr/oversCount/macroBands).
        body += ",\"meters\":" + metersBlob;
        // Diagnostic: outgoing meter portion, visible in Console.app on the
        // same stream as the NativeClip2 logs
        EchoJay_NSLog(("EJChat: meters " + metersBlob.substring(0, 400)).toRawUTF8());
    }
    body += "}";

    // dev_mode: dump the EXACT outgoing body for diffing against server
    // logs (same switch as the Dump meters button)
    {
        static const bool devMode = juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
            .getChildFile("EchoJay").getChildFile("dev_mode").existsAsFile();
        if (devMode)
        {
            auto f = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                         .getChildFile("EchoJay").getChildFile("chat-body-debug.json");
            f.getParentDirectory().createDirectory();
            f.replaceWithText(body);
            EchoJay_NSLog(("EJChat: dev_mode body dump -> " + f.getFullPathName()
                           + " (" + juce::String((int) body.getNumBytesAsUTF8()) + "b total)").toRawUTF8());
        }
    }

    postJSON("/api/chat", body, [this, onComplete](const juce::var& json, int statusCode)
    {
        if (statusCode == 200 && json.isObject())
        {
            auto* obj = json.getDynamicObject();
            if (obj && obj->hasProperty("reply"))
            {
                juce::String reply = obj->getProperty("reply").toString();

                // Model that handled THIS turn, for the input-bar indicator.
                // Absent field keeps the previous value (no blanking).
                if (obj->hasProperty("modelName"))
                {
                    auto mn = obj->getProperty("modelName").toString().trim();
                    if (mn.isNotEmpty()) lastChatModelName_ = mn;
                }

                // Server-side turn classification for THIS reply (the client
                // turnType is a staged label the server reclassifies; this is
                // the truth). Gates the prose name-scan chain fallback and
                // lands in the log for live turn-class verification.
                lastResolvedTurnType_ = obj->getProperty("resolvedTurnType").toString().trim();
                EchoJay_NSLog(("EJChat: resolvedTurnType="
                               + (lastResolvedTurnType_.isNotEmpty()
                                    ? lastResolvedTurnType_ : juce::String("(absent)"))).toRawUTF8());

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
            // Display the server's error message directly (the server sends
            // the lane-correct copy). The 429 body also carries a FRESH
            // usagePool — ingest it so Settings bars and the premium locks
            // reflect the blocked state immediately, without waiting for
            // the next /api/me poll.
            juce::String serverMsg;
            if (json.isObject())
            {
                auto* obj = json.getDynamicObject();
                if (obj)
                {
                    if (obj->hasProperty("error"))
                        serverMsg = obj->getProperty("error").toString();
                    if (obj->hasProperty("usagePool"))
                    {
                        parseUsagePool(obj, userInfo);
                        EchoJay_NSLog("EJChat: 429 carried usagePool -- state refreshed");
                    }
                    if ((bool) obj->getProperty("upgradeRequired"))
                        EchoJay_NSLog("EJChat: 429 upgradeRequired=true");
                }
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
        if (!aliveFlag->load()) return; // plugin removed before thread ran

        juce::URL url(endpoint + "/api/vst-config");
        
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs(60000)
                           .withStatusCode(&statusCode);
        
        auto stream = url.createInputStream(options);
        
        if (stream != nullptr && statusCode == 200)
        {
            juce::MemoryBlock mb;
            stream->readIntoMemoryBlock(mb);
            auto responseText = juce::String::fromUTF8((const char*)mb.getData(), (int)mb.getSize());
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
                                             const juce::String& pluginSummary)
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

    // usage-v2 (2.9.98): capture-era data availability. Appended AFTER
    // whichever base prompt is in use — including the REMOTE config prompt,
    // which still carries live-meter-era instructions and a LIVE METER
    // field glossary the model imitated on payload-less chats (fabricated
    // blocks with invented values). Explicit later-override wording
    // neutralises those sections until the SaaS prompt is updated.
    prompt += "DATA AVAILABILITY (THIS OVERRIDES ANY EARLIER INSTRUCTION ABOUT LIVE METERS):\n";
    prompt += "Meter data reaches you ONLY through explicit captures (a [CAPTURE ...] data block or attached capture payload) and Compare data. There is NO live meter feed on chat turns. Any earlier instruction about a LIVE METER block is obsolete: such a block will never be present, and you must NEVER produce one, imitate its format, or invent meter values.\n";
    prompt += "If the conversation contains NO capture data: do not describe or characterise the mix's loudness, dynamics, stereo image, tonal balance, or any metric in any way - you have no data at all. Do not guess, estimate, or speak in general terms about how it probably measures. ANSWER THE QUESTION THE USER ACTUALLY ASKED FIRST; do NOT open with a capture-status report. Only mention that a Capture would give you real numbers if the user asks about the sound, the mix, or the measurements - capture availability is context for your own reasoning, not a status line to lead with.\n\n";

    
    // Channel type context — tells AI what kind of audio this is and what to
    // focus on. Item 2 fix: the Mix/Master Bus case is no longer SILENT — it
    // used to skip this block entirely, so the model was told nothing about
    // the material and would ask "is this a vocal, a bus, a synth?" even
    // though the user had set Mix Bus. It now states the full-mix material;
    // only the element-specific FOCUS stays gated to non-bus material.
    if (channelType == "Mix Bus" || channelType == "Master Bus")
    {
        prompt += "MATERIAL: the FULL MIX (" + channelType + "). Review it as a "
                  "complete mix, not a single element.\n\n";
    }
    else
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
    
    if (pluginSummary.isNotEmpty())
    {
        prompt += "USER'S PLUGIN LIBRARY (REFERENCE ONLY): " + pluginSummary + "\n";
        prompt += "This is a summary of what the user owns, for context only. Do NOT proactively suggest plugins. When the user asks for a plugin suggestion or a chain, or you're solving a clear flagged problem where a specific tool is the answer, their full relevant plugin list will be provided in that message — use the actual names from it and rotate your picks. If no list is attached to a given message, you can still refer to the library in general terms but don't invent specific product names you haven't been given.\n\n";
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
    
    // === V2 PLUGIN-SIDE CHAIN BLOCK RULE ===
    // This section is appended by the EchoJay V2 desktop plugin only.
    // The SaaS backend (echojay-saas) does not include this — do not add it there.
    // It is a standing instruction so the model always knows the output contract,
    // even before the per-turn AVAILABLE PLUGINS list arrives in the user message.
    prompt += "\n[ECHOJAY V2 — CHAIN BLOCK OUTPUT CONTRACT]\n";
    prompt += "When this conversation contains an AVAILABLE PLUGINS list (provided in the user message), ";
    prompt += "and your reply names two or more of those plugins to be used in sequence ";
    prompt += "(i.e. you are describing or recommending a processing chain), you MUST: ";
    prompt += "(1) Write the human-readable explanation first. ";
    prompt += "(2) Then append the following machine block as the VERY LAST thing in your response — "
              "nothing at all after <<<END_CHAIN>>>:\n";
    prompt += "<<<ECHOJAY_CHAIN>>>\n";
    prompt += "{\"chain\":[{\"name\":\"<exact name from AVAILABLE PLUGINS>\",\"role\":\"<2-4 words>\","
              "\"settings\":\"<REQUIRED: 3-6 short values, e.g. '4:1, -18dB thr, 30ms att' or '-3dB@200Hz, +2dB shelf'>\"},...],\"explanation\":\"<one sentence>\"}\n";
    prompt += "<<<END_CHAIN>>>\n";
    prompt += "The prose and the block are NOT alternatives — when there is a chain, include both. "
              "Use only names that appear verbatim in the AVAILABLE PLUGINS list. "
              "If your preferred plugin is not listed, substitute ONLY a plugin that serves the same "
              "function as the slot's role; if no same-role plugin is available, omit that slot and "
              "briefly note in prose which plugin you would have used and that it is not available. "
              "Omit the block ONLY when you are not proposing an ordered set of plugins "
              "(e.g. single-plugin answers, general mixing advice, or chat with no plugin chain).\n";
    prompt += "CRITICAL: \"settings\" is MANDATORY for every chain entry. It must contain specific, "
              "concrete starting values in real units (dB, ratio, ms, Hz, dBTP) — matching the values "
              "you mention in your prose. NEVER leave settings empty, null, or vague ('adjust to taste', "
              "'as needed'). If you give a number in the prose for this plugin, put it in settings.\n\n";

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

// ============ Per-turn plugin injection ============
// Plugins live in two places now: a tiny summary in the (cached) system
// prompt, and the FULL list injected into the user turn only when the message
// actually needs it. This keeps the everyday prompt cheap and cache-stable
// while giving the AI the user's entire library exactly on the turns where
// plugin specifics matter — no cap, nothing hidden.

const juce::StringArray& EchoJayAPI::historyStripMarkers()
{
    // "[TARGET CHANNEL" also covers "[TARGET CHANNEL OFFLINE" (prefix match).
    // "[THIS CHANNEL" is the main plugin's conduct declaration (7 Aug 2026)
    // — it always rides AFTER a plugin marker today, but it is listed in its
    // own right so a future reordering cannot leak it into history.
    static const juce::StringArray markers {
        "\n\n[AVAILABLE PLUGINS", "\n\n[USER'S FULL PLUGIN LIST",
        "\n\n[AVAILABLE BUILTINS",
        "\n\n[CURRENT CHAIN", "\n\n[TARGET CHANNEL", "\n\n[THIS CHANNEL"
    };
    return markers;
}

bool EchoJayAPI::messageNeedsPlugins(const juce::String& userMessage)
{
    auto m = userMessage.toLowerCase();

    // Direct asks and processing-type mentions. Deliberately broad: a false
    // positive just attaches a list the model may not use (cheap on a single
    // turn), whereas a false negative means the AI can't name the user's
    // tools when they wanted it to (the thing we're fixing). So we lean in.
    static const char* kCues[] = {
        // explicit asks
        "plugin", "plug-in", "vst", "chain", "what should i use", "which ",
        "recommend", "suggestion", "suggest", "what would you", "any tips on",
        "how do i", "how would you", "what can i use", "reach for", "tool for",
        // processing types
        "eq", "equali", "compress", "limit", "reverb", "delay", "saturat",
        "distort", "tape", "exciter", "transient", "de-ess", "deess", "gate",
        "widen", "stereo", "imag", "chorus", "flang", "phaser", "pitch",
        "tune", "clip", "multiband", "sidechain", "side-chain", "bus comp",
        "glue", "parallel", "harmonic", "warmth", "color", "colour",
        // mix-move verbs that usually imply a tool
        "brighten", "darken", "tame", "control the", "smooth out", "thicken",
        "tighten", "add air", "more punch", "more presence",
        // chain-edit verbs (CHAIN_AI_BUILD_SPEC Phase 1a): editing an
        // existing rack needs the plugin feed + current-chain injection too
        "remove", "swap", "instead of", "take out", "take off", "get rid of",
        "reorder", "re-order", "rearrange", "move the"
    };

    for (auto* cue : kCues)
        if (m.contains(cue))
            return true;

    return false;
}

// The [AVAILABLE BUILTINS] advertisement - EchoJay's own devices and their full
// ParamSchemas. Shared by BOTH feed builders on purpose: the built-ins are always
// available, so they must be advertised whether or not the user owns recommendable
// third-party plugins. Attaching it to only one path left the whole built-in suite
// invisible to every user WITH plugins installed - i.e. exactly the users it is for.
static juce::String echojayBuiltinsBlock()
{
    juce::String block;
    const auto& registry = BuiltinDeviceRegistry::instance();
    if (! registry.all().empty())
    {
        block << "\n\n[AVAILABLE BUILTINS — EchoJay's own built-in devices, always "
                 "available regardless of installed plugins; use in a chain by exact "
                 "name. Dial one by putting real-world values under "
                 "settings_structured.params using the ids listed; any param you omit "
                 "is left as it is. The EchoJay EQ additionally takes: "
                 "eq_bands — an array of band objects {type: bell|lowshelf|highshelf|"
                 "notch|highpass|lowpass, freq_hz OR note (\"G5\",\"C#3\", A4=440), "
                 "gain_db, q, slope_db_oct (HP/LP), band (1-24 to target, omit to "
                 "auto-place), channel: stereo|mid|side|left|right (default stereo), "
                 "dynamic:{threshold_db,range_db,attack_ms,release_ms}, disable:true "
                 "to clear a band}; "
                 "eq_settings — {output_db, auto_gain, phase_mode: zero|linear, "
                 "ms_mode} (same ids as params); "
                 "eq_action — {type:\"tame_resonances\", sensitivity: low|medium|high, "
                 "range_hz:[lo,hi], max_bands:1-8, dynamic:true|false} finds resonant "
                 "peaks in the LIVE signal and places taming bands itself — use it "
                 "when the user says tame the ringing/harshness/resonances without "
                 "naming frequencies (audio must be playing); "
                 "eq_preset — a named base loaded BEFORE eq_bands merge, one of: ";

        // The preset menu comes from the same table the apply path resolves
        // against, so this list cannot drift from what actually loads.
        for (int pi = 0; pi < echojay::kNumEqPresets; ++pi)
        {
            const auto& p = echojay::kEqPresets[pi];
            block << (pi > 0 ? "; " : "") << p.name << " (" << p.blurb << ")";
        }
        block << "]:";

        juce::String currentCategory;
        for (const auto& d : registry.all())
        {
            if (d.category != currentCategory)
            {
                currentCategory = d.category;
                block << "\n\n-- " << currentCategory << " --";
            }

            block << "\n" << d.name;
            if (d.summary.isNotEmpty())
                block << "\n  " << d.summary;

            for (const auto& p : d.schema.params())
                block << "\n    " << juce::String (echojay::ParamSchema::describeLine (p));
        }
    }
    return block;
}

juce::String EchoJayAPI::buildPluginInjection(const juce::String& fullList)
{
    if (fullList.isEmpty()) return {};

    // Appended to the user's message (NOT the system prompt), so it never
    // touches the cached prefix. Framed so the model treats it as the
    // authoritative set to pick from for this answer.
    juce::String block;
    block << "\n\n[USER'S FULL PLUGIN LIST — for this question, recommend only from these, using their exact names; rotate your picks rather than defaulting to the same few]:\n"
          << fullList;

    // Built-in devices can never appear in the scanned feed above — they are
    // compiled into the plugin, not installed on the machine — so the server
    // would otherwise have no way to know this build has them. Advertising
    // them explicitly is what lets the backend gate on a CAPABILITY the client
    // states rather than guessing from a version number (version guessing is
    // unreliable here: an unrelated branch can carry a higher version and no
    // EQ at all). A client that does not send this block simply never gets
    // built-ins offered.
    //
    // GENERATED FROM THE REGISTRY, grouped by category (BUILTIN_SUITE_PLAN.md
    // §1/§3). A device registers itself in its own .cpp and appears here with no
    // edit to this function — which is the property that lets several Wave 1
    // sessions add devices in parallel without touching a shared line.
    //
    // Each entry carries its ParamSchema, so this one block teaches the model
    // BOTH that the device exists and exactly how to dial it. The server
    // validates against the same numbers it reads here, so the two cannot drift.
    block << echojayBuiltinsBlock();

    return block;
}

juce::String EchoJayAPI::buildChainInjection(const juce::StringArray& availablePlugins)
{
    if (availablePlugins.isEmpty()) return {};

    juce::String list;
    for (int i = 0; i < availablePlugins.size(); ++i)
    {
        if (i > 0) list += ", ";
        list += "\"" + availablePlugins[i] + "\"";
    }

    juce::String block;
    block << "\n\n[AVAILABLE PLUGINS — the ONLY plugins that can be loaded into a chain "
          << "on this machine right now: " << list << "]\n\n"
          << "[CHAIN BLOCK RULE — read carefully and follow exactly]\n"
          << "WHENEVER your reply names two or more plugins to use in order "
          << "(i.e. you are recommending or describing a processing chain), "
          << "you MUST append the machine block below at the very end of your reply — "
          << "after all prose, as the absolute last thing, with nothing after <<<END_CHAIN>>>.\n"
          << "The prose explanation and the block are NOT alternatives: when there is a chain, give BOTH.\n"
          << "Only omit the block when you are genuinely not proposing a chain "
          << "(pure advice, a single-plugin suggestion, or general discussion with no ordered plugin set).\n\n"
          << "BLOCK FORMAT (copy exactly, no extra text before or after):\n"
          << "<<<ECHOJAY_CHAIN>>>\n"
          << "{\"chain\":[{\"name\":\"<exact name from AVAILABLE PLUGINS>\","
          << "\"role\":\"<2-4 words>\","
          << "\"settings\":\"<SHORT: 3-6 key values only, e.g. '4:1, -18dB thr, 30ms att'>\"},...],\"explanation\":\"<one sentence>\"}\n"
          << "<<<END_CHAIN>>>\n\n"
          << "RULES FOR THE BLOCK:\n"
          << "- Use ONLY exact names from the AVAILABLE PLUGINS list above. That list is the "
          << "  SOLE source of truth for chain blocks: the user's profile plugin library, the "
          << "  conversation history, and your own knowledge of popular plugins are NOT valid "
          << "  sources — plugins named there may be uninstalled or in a format (e.g. VST2) "
          << "  this host cannot load. NEVER put a name in the block that is not in the list, "
          << "  even if the user appears to own it.\n"
          << "- Order slots for logical audio signal flow (e.g. EQ → Comp → Saturation → Limiter).\n"
          << "- If your ideal plugin is absent from the list, substitute ONLY a plugin that serves "
          << "  the same function as the slot's role. If no same-role plugin is available, omit that "
          << "  slot and briefly note in prose which plugin you would have used and that it is not "
          << "  available. Never fill a slot with a different-function plugin just because its name "
          << "  is similar.\n"
          << "- Do not pad with unnecessary plugins; include only slots that genuinely serve the request.\n"
          << "- settings is MANDATORY for every slot — never leave it empty or vague ('adjust to taste' is not acceptable). "
          << "  Give 3-6 concrete values in real units (dB, ratio, ms, Hz, dBTP). Full advice stays in the prose; this is the compact tile version.\n"
          << "- Keep the ENTIRE block compact — short names, 2-4 word roles, 3-6 value settings. "
          << "  This is machine data written after the prose; it must fit in the remaining response budget.\n"
          << "- Write prose first (full technical detail), then append the compact block as the final output.";

    // Built-ins ride the chain feed too - see echojayBuiltinsBlock().
    block << echojayBuiltinsBlock();

    return block;
}

juce::String EchoJayAPI::buildCurrentChainInjection(const ChainHost& chainHost)
{
    // Adapter: fill a RackSidecar from the live local rack and let the ONE
    // formatter below author the block (Phase R anti-drift discipline).
    LinkShm::RackSidecar rack;
    rack.valid     = true;
    rack.revision  = chainHost.getChainRevision();
    rack.masterWet = chainHost.getMasterWet();
    for (const auto& s : chainHost.getAllSlotInfos())
        rack.slots.push_back({ s.name, s.format, s.settings, s.bypassed, s.wet });
    return buildCurrentChainInjection(rack, juce::String());
}

juce::String EchoJayAPI::buildCurrentChainInjection(const LinkShm::RackSidecar& rack,
                                                    const juce::String& channelLabel)
{
    if (!rack.valid || rack.slots.empty()) return {};

    // Numbered 1-BASED \xe2\x80\x94 the numbering users naturally use ("the first
    // slot is slot 1") and therefore the numbering the model must echo in
    // prose and edit ops. parseChainEditOps is the single point converting
    // back to internal 0-based. Kept compact: this rides every
    // plugin-relevant turn while a chain exists, so no prose padding. wet
    // reported only when it departs from fully wet; settings clipped so one
    // verbose slot cannot bloat the turn.
    const juce::String owner = channelLabel.isEmpty()
        ? juce::String("the user's rack right now")
        : "the rack on the user's \"" + channelLabel + "\" Link channel right now";
    juce::String block;
    block << juce::String::fromUTF8("\n\n[CURRENT CHAIN \xe2\x80\x94 ") << owner
          << ", in signal-flow "
          << "order; slot numbers are 1-based (the first slot is slot 1). "
          << "When the user asks to change THIS chain (add/remove/swap/"
          << "reorder), treat it as an edit of these slots, not a new chain "
          << "from scratch. Every slot listed here is loaded and running and "
          << "may ALWAYS be referenced and edited (remove/move/bypass/"
          << "replace), regardless of AVAILABLE PLUGINS membership or any "
          << "auto-dial restriction " << juce::String::fromUTF8("\xe2\x80\x94")
          << " those only govern which plugins may "
          << "fill NEW slots.]\n";
    for (int i = 0; i < (int)rack.slots.size(); ++i)
    {
        const auto& s = rack.slots[(size_t)i];
        block << (i + 1) << ": \"" << s.name << "\" (" << s.format;
        if (s.bypassed) block << ", BYPASSED";
        if (s.wet < 0.995f)
            block << ", wet " << juce::roundToInt(s.wet * 100.0f) << "%";
        block << ")";
        auto settings = s.settings.trim();
        if (settings.isNotEmpty())
            block << juce::String::fromUTF8(" \xe2\x80\x94 settings: ")
                  << (settings.length() > 120
                        ? settings.substring(0, 120) + juce::String::fromUTF8("\xe2\x80\xa6")
                        : settings);
        block << "\n";
    }
    if (rack.masterWet < 0.995f)
        block << "Master chain wet/dry: " << juce::roundToInt(rack.masterWet * 100.0f) << "%\n";
    return block;
}

bool EchoJayAPI::extractChainBlock(juce::String& replyInOut, juce::String& chainJsonOut)
{
    const juce::String kOpen  = "<<<ECHOJAY_CHAIN>>>";
    const juce::String kClose = "<<<END_CHAIN>>>";

    int start = replyInOut.indexOf(kOpen);
    if (start < 0) return false;

    int jsonStart = start + (int)kOpen.length();
    int end = replyInOut.indexOf(start, kClose);

    if (end >= 0)
    {
        // Complete block — extract JSON and strip entire block including delimiters
        chainJsonOut = replyInOut.substring(jsonStart, end).trim();
        replyInOut   = replyInOut.substring(0, start).trimEnd()
                     + replyInOut.substring(end + (int)kClose.length());
    }
    else
    {
        // Truncated: opening delimiter found but no closing tag.
        // ALWAYS strip everything from <<<ECHOJAY_CHAIN>>> to end of reply so
        // raw JSON never leaks into the visible chat message.
        chainJsonOut = replyInOut.substring(jsonStart).trim();
        replyInOut   = replyInOut.substring(0, start).trimEnd();
    }
    return true;
}

bool EchoJayAPI::extractGainBlock(juce::String& replyInOut, juce::String& gainJsonOut)
{
    const juce::String kOpen  = "<<<ECHOJAY_GAIN>>>";
    const juce::String kClose = "<<<END_GAIN>>>";

    int start = replyInOut.indexOf(kOpen);
    if (start < 0) return false;

    int jsonStart = start + (int)kOpen.length();
    int end = replyInOut.indexOf(start, kClose);

    if (end >= 0)
    {
        gainJsonOut = replyInOut.substring(jsonStart, end).trim();
        replyInOut  = replyInOut.substring(0, start).trimEnd()
                    + replyInOut.substring(end + (int)kClose.length());
    }
    else
    {
        gainJsonOut = replyInOut.substring(jsonStart).trim();
        replyInOut  = replyInOut.substring(0, start).trimEnd();
    }
    return true;
}

// CHAIN_EDIT ops block (CHAIN_AI_BUILD_SPEC Phase 1c). Same tolerant
// truncation semantics as the other extractors. Delimiters: keep in sync
// with api/_blocks.js BLOCK_TYPES.chain_edit (canonical) and
// extractChainEditBlockWeb in public/app.html.
bool EchoJayAPI::extractChainEditBlock(juce::String& replyInOut, juce::String& editJsonOut)
{
    const juce::String kOpen  = "<<<ECHOJAY_CHAIN_EDIT>>>";
    const juce::String kClose = "<<<END_CHAIN_EDIT>>>";

    int start = replyInOut.indexOf(kOpen);
    if (start < 0) return false;

    int jsonStart = start + (int)kOpen.length();
    int end = replyInOut.indexOf(start, kClose);

    if (end >= 0)
    {
        editJsonOut = replyInOut.substring(jsonStart, end).trim();
        replyInOut  = replyInOut.substring(0, start).trimEnd()
                    + replyInOut.substring(end + (int)kClose.length());
    }
    else
    {
        editJsonOut = replyInOut.substring(jsonStart).trim();
        replyInOut  = replyInOut.substring(0, start).trimEnd();
    }
    return true;
}

// ASK question/choices block (CHAIN_AI_BUILD_SPEC Phase 1b). Same tolerant
// truncation semantics as the chain/gain extractors. Delimiters: keep in
// sync with api/_blocks.js BLOCK_TYPES.ask (the canonical registry) and
// extractAskBlockWeb in public/app.html.
bool EchoJayAPI::extractAskBlock(juce::String& replyInOut, juce::String& askJsonOut)
{
    const juce::String kOpen  = "<<<ECHOJAY_ASK>>>";
    const juce::String kClose = "<<<END_ASK>>>";

    int start = replyInOut.indexOf(kOpen);
    if (start < 0) return false;

    int jsonStart = start + (int)kOpen.length();
    int end = replyInOut.indexOf(start, kClose);

    if (end >= 0)
    {
        askJsonOut = replyInOut.substring(jsonStart, end).trim();
        replyInOut = replyInOut.substring(0, start).trimEnd()
                   + replyInOut.substring(end + (int)kClose.length());
    }
    else
    {
        // Truncated: strip from the opening delimiter so raw JSON never
        // shows in chat; the (partial) payload may still parse.
        askJsonOut = replyInOut.substring(jsonStart).trim();
        replyInOut = replyInOut.substring(0, start).trimEnd();
    }
    return true;
}

juce::String EchoJayAPI::salvagePartialChain(const juce::String& partial)
{
    // Find the chain array opening
    int arrayPos = partial.indexOf("\"chain\":[");
    if (arrayPos < 0) arrayPos = partial.indexOf("\"chain\": [");
    if (arrayPos < 0) return {};

    int bracketPos = partial.indexOf(arrayPos, "[");
    if (bracketPos < 0) return {};
    int pos = bracketPos + 1; // step past '['

    // Bracket-depth scan: collect every complete top-level {...} object
    juce::String entries;
    int depth = 0;
    int entryStart = -1;
    int n = partial.length();

    for (int i = pos; i < n; ++i)
    {
        juce::juce_wchar c = partial[i];
        if (c == '{')
        {
            if (depth == 0) entryStart = i;
            depth++;
        }
        else if (c == '}')
        {
            if (depth > 0)
            {
                depth--;
                if (depth == 0 && entryStart >= 0)
                {
                    if (entries.isNotEmpty()) entries += ",";
                    entries += partial.substring(entryStart, i + 1);
                    entryStart = -1;
                }
            }
        }
        else if (c == ']' && depth == 0)
        {
            break; // end of array (shouldn't reach here if truncated, but be safe)
        }
    }

    if (entries.isEmpty()) return {};
    return "{\"chain\":[" + entries + "],\"explanation\":\"Chain recovered from truncated response\"}";
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

        // Absent property -> false: auto-dial mode defaults OFF.
        autoDialMode = (bool) obj->getProperty("autoDialMode");
        
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
    obj->setProperty("autoDialMode", autoDialMode);
    
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

void EchoJayAPI::fetchWhatsNew(std::function<void(const juce::var&)> onComplete)
{
    auto cacheFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("EchoJay").getChildFile("whats_new.json");
    auto fallback = [onComplete, cacheFile]
    {
        // Silent fallback: last cached copy, else empty — the card shows
        // "You're up to date", NEVER an error state
        juce::var cached;
        if (cacheFile.existsAsFile())
            cached = juce::JSON::parse(cacheFile.loadFileAsString());
        if (onComplete) onComplete(cached);
    };
    if (!isLoggedIn()) { fallback(); return; }

    getJSON("/api/whats-new", [onComplete, cacheFile, fallback](const juce::var& json, int statusCode)
    {
        if (statusCode == 200 && json.isArray())
        {
            cacheFile.getParentDirectory().createDirectory();
            cacheFile.replaceWithText(juce::JSON::toString(json, true));
            if (onComplete) onComplete(json);
            return;
        }
        fallback();
    });
}

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
    // The scanner is the source of truth for what is installed NOW — REPLACE
    // the profile list outright. The old merge-only behaviour accumulated
    // names forever (web-app manual entries + every past scan), so plugins
    // uninstalled long ago kept riding the profile into the AI's server-side
    // context and could surface in recommendations.
    juce::StringArray scanned;
    scanned.addTokens(scannedPlugins, ",", "");
    scanned.trim();
    scanned.removeEmptyStrings();
    scanned.sort(true);
    userSettings.plugins = scanned.joinIntoString(", ");
}
