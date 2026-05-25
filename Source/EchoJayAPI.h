#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <atomic>
#include <map>

struct UserInfo {
    juce::String email;
    juce::String tier = "free";     // "free", "pro", "studio", "its_platinum"
    int tierLevel = 0;              // 0=free, 1=pro, 2=studio (its_platinum maps to 0 here)
    int messagesUsedToday = 0;      // field name is legacy — period is now monthly
    int messageLimit = 15;          // 15 free, 200 pro, 400 studio (per month)
    int credits = 0;                // bonus credits beyond monthly limit
    juce::String displayName;

    bool isPro() const { return tierLevel >= 1; }
    bool isStudio() const { return tierLevel >= 2; }

    static int tierStringToLevel(const juce::String& t)
    {
        if (t == "studio") return 2;
        if (t == "pro")    return 1;
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
    
    // Check if logged in
    bool isLoggedIn() const { return authToken.isNotEmpty(); }
    
    // Get current user info
    UserInfo getUserInfo() const { return userInfo; }
    
    // Check if user can send a message (within monthly limit + credits)
    bool canSendMessage() const;
    
    // Get remaining messages this period
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
    
    void sendChat(const juce::StringArray& roles,
                  const juce::StringArray& contents,
                  const juce::String& systemPrompt,
                  std::function<void(const juce::String& reply, bool success)> onComplete);
    
    // ============ User Settings (synced with web app) ============
    
    // Fetch settings from server
    void fetchSettings(std::function<void(bool success)> onComplete = nullptr);
    
    // Save settings to server
    void saveUserSettings(const UserSettings& settings,
                          std::function<void(bool success)> onComplete = nullptr);
    
    // Get current cached settings
    UserSettings getUserSettings() const { return userSettings; }
    
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
    
    // ============ Local Settings ============
    
    void setEndpoint(const juce::String& url) { apiEndpoint = url; }
    juce::String getEndpoint() const { return apiEndpoint; }
    
    void loadSettings();
    void saveSettings() const;
    static juce::File getSettingsFile();

private:
    juce::String apiEndpoint;
    juce::String authToken;
    juce::String deviceId;
    UserInfo userInfo;
    UserSettings userSettings;
    
    // Shared flag: set to false in destructor so in-flight callbacks
    // know the object is gone and skip any member access.
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>>(true) };
    
    // Helper: make a POST request with auth header
    void postJSON(const juce::String& endpoint, const juce::String& body,
                  std::function<void(const juce::var& json, int statusCode)> onComplete);
    
    // Helper: make a GET request with auth header
    void getJSON(const juce::String& endpoint,
                 std::function<void(const juce::var& json, int statusCode)> onComplete);
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayAPI)
};
