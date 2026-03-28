#include "EchoJayAPI.h"

// Static members for remote config — shared across all plugin instances
juce::String EchoJayAPI::remoteSystemPrompt;
int EchoJayAPI::remotePromptVersion = 0;
bool EchoJayAPI::remoteConfigLoaded = false;
juce::String EchoJayAPI::latestVersion;
juce::String EchoJayAPI::updateUrl;
juce::String EchoJayAPI::announcement;

EchoJayAPI::EchoJayAPI()
{
    loadSettings();
    if (apiEndpoint.isEmpty())
        apiEndpoint = "https://www.echojay.ai";
    
    // Fetch remote config once per session (shared across instances)
    if (!remoteConfigLoaded)
        fetchRemoteConfig();
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
        juce::URL url(endpoint + path);
        url = url.withPOSTData(body);
        
        juce::String headers = "Content-Type: application/json\r\n";
        if (token.isNotEmpty())
            headers += "Authorization: Bearer " + token + "\r\n";
        
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                           .withExtraHeaders(headers)
                           .withConnectionTimeoutMs(15000)
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

// ============ Generic GET helper ============

void EchoJayAPI::getJSON(const juce::String& path,
                          std::function<void(const juce::var& json, int statusCode)> onComplete)
{
    auto endpoint = apiEndpoint;
    auto token = authToken;
    auto cb = std::make_shared<std::function<void(const juce::var&, int)>>(onComplete);
    auto aliveFlag = alive; // capture shared_ptr by value — prevent use-after-free
    
    juce::Thread::launch([=]()
    {
        juce::URL url(endpoint + path);
        
        juce::String headers;
        if (token.isNotEmpty())
            headers += "Authorization: Bearer " + token + "\r\n";
        
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withExtraHeaders(headers)
                           .withConnectionTimeoutMs(15000)
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
                        ",\"password\":" + juce::JSON::toString(password) + "}";
    
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
        juce::String limitStr = juce::String(userInfo.messageLimit);
        juce::String msg = "You've hit your daily limit of " + limitStr + " AI messages. ";
        if (userInfo.tierLevel >= 2)
            msg += "Limit resets at midnight.";
        else if (userInfo.tierLevel >= 1)
            msg += "Upgrade to Studio for 150 messages per day.";
        else
            msg += "Upgrade to Pro for 50 messages per day.";
        onComplete(msg, false);
        return;
    }
    
    // Build messages JSON
    juce::String messagesJson = "[";
    messagesJson += "{\"role\":\"system\",\"content\":" + juce::JSON::toString(systemPrompt) + "}";
    for (int i = 0; i < roles.size(); ++i)
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
                serverMsg = "Daily message limit reached.";
            onComplete(serverMsg, false);
            return;
        }
        
        juce::String error = "Failed to get AI response";
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
    
    juce::Thread::launch([=]()
    {
        juce::URL url(endpoint + "/api/vst-config");
        
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs(5000)
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
                
                if (obj->hasProperty("latestVersion"))
                    latestVersion = obj->getProperty("latestVersion").toString();
                if (obj->hasProperty("updateUrl"))
                    updateUrl = obj->getProperty("updateUrl").toString();
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
                }
            }
            remoteConfigLoaded = true;
        }
    });
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
    prompt += "EXAMPLE 1 — when the mix sounds good (version A):\n";
    prompt += "\"The loudness is sitting right at -9 LUFS — no issues there. Dynamics are in a decent spot at 7.5dB crest, your transients are punching through without being squashed.\n\n";
    prompt += "Everything's looking healthy from what the meters show. Is there anything specific you want to work on, or would you like to drop a reference track in Compare Mixes and see how it stacks up?\"\n\n";
    prompt += "EXAMPLE 2 — when the mix sounds good (version B):\n";
    prompt += "\"Meters are reading clean — -7 LUFS, 9dB crest. Nothing jumping out as a problem here. The dynamics are well controlled and you've got a good balance between loudness and headroom.\n\n";
    prompt += "If you want to push this further, throw a reference into Compare Mixes and see where you sit against it. Otherwise, what part of the mix are you least sure about?\"\n\n";
    prompt += "EXAMPLE 3 — when there are issues:\n";
    prompt += "\"You're pushing this hard — the crest at 4.8dB means the limiter is eating into your transients pretty heavily. If that's the vibe you're going for then fair enough, but if you're finding it lacks punch that could be why.\n\n";
    prompt += "The loudness at -5.5 LUFS is loud for streaming — most platforms will turn it down anyway, so you might be able to back off the limiter a bit and get some dynamics back without losing perceived loudness.\n\n";
    prompt += "What are you hearing that you'd like to improve? I can help work through specifics if you tell me what you're going for.\"\n\n";
    
    prompt += "IMPORTANT: Each review must read differently. Don't compare the crest to \"what most people/tracks would have at this level\" — that's a crutch. Just state what the number means for the track. Vary your opening sentence every time. Never start two reviews the same way.\n\n";
    
    prompt += "MATCH THAT TONE AND STYLE EXACTLY. Notice:\n";
    prompt += "- Short, conversational paragraphs — 1-2 paragraphs is fine\n";
    prompt += "- Says what's good without being sycophantic\n";
    prompt += "- Frames issues as choices (\"if that's the vibe\" / \"if you're finding it lacks punch\")\n";
    prompt += "- Does NOT give unsolicited technique suggestions — just reviews the numbers and asks what the user wants to work on\n";
    prompt += "- Only suggests a specific technique if the user asks, or if there's a clear problem worth flagging\n";
    prompt += "- Never uses these words/phrases: fascinating, striking, sparkle, sparkly, sizzle, shimmer, notably, perceived, sonic landscape, breathing room, on the table, intriguing\n";
    prompt += "- Never guesses instruments — you cannot hear what's in the track\n";
    prompt += "- Suggests Compare Mixes or asks what the user wants to work on\n";
    prompt += "- Doesn't force criticism — if it's good, says so\n\n";
    
    prompt += "CRITICAL — YOU CANNOT HEAR THE TRACK. You only have meter data. So:\n";
    prompt += "- NEVER guess what instruments or elements are in the mix\n";
    prompt += "- NEVER make up specific mix scenarios — you're guessing\n";
    prompt += "- NEVER criticise a metric that's in the normal range. If LUFS is in range, crest is healthy, width is 10-50%, and correlation is above 0 — those are all FINE. Do not suggest improvements to things that aren't broken.\n";
    prompt += "- NEVER mention a metric that isn't in the data. If width or correlation aren't shown, don't mention them. Don't invent or guess values.\n";
    prompt += "- It is OK to write a short review. If nothing is wrong, 1-2 paragraphs is enough. Do NOT pad with invented problems to fill space.\n\n";
    
    prompt += "METER READING:\n";
    prompt += "- LUFS: Use the genre (provided below, never mention it in your response) to judge loudness. -8 to -11 is normal for urban/pop. -6 to -9 for electronic. -8 to -12 for rock. -14 to -22 for jazz/classical/ambient. Only flag if clearly outside the expected range.\n";
    prompt += "- Crest factor: Below 4dB = crushed. 4-6dB = compressed. 6-10dB = healthy. 10-14dB = dynamic.\n";
    prompt += "- True peak: Only shown in the data if it's clipping above +1.5 dBTP. If true peak isn't in the data, don't mention it.\n";
    prompt += "- Width: Only shown in the data if it's notably narrow or wide. If width isn't in the data, don't mention it.\n";
    prompt += "- Correlation: Only shown if there are phase issues. If not in the data, don't mention it.\n\n";
    
    prompt += "WHEN EVERYTHING LOOKS GOOD: Say the mix is in good shape, mention what the numbers tell you is working well, then suggest they drop a reference in Compare Mixes or offer ONE creative technique to experiment with. Don't invent problems.\n\n";
    prompt += "In follow-up conversation: answer what they ask. Be conversational. Don't repeat the review.\n\n";
    
    } // end hardcoded fallback
    
    // === DYNAMIC SECTIONS (always appended, whether remote or hardcoded) ===
    
    // Channel type context — tells AI what kind of audio this is and what to focus on
    if (channelType != "Mix Bus" && channelType != "Master Bus")
    {
        prompt += "CHANNEL TYPE: \"" + channelType + "\"\n";
        prompt += "This is NOT a full mix. Focus your feedback on what matters for this specific element.\n";
        
        // Vocals
        if (channelType == "Lead Vocal")
            prompt += "Focus on: clarity and presence (2-5kHz), sibilance (6-10kHz), compression and dynamic control, proximity effect (low-mid buildup around 200-400Hz), de-essing needs, whether it would cut through a mix. Loudness norms don't apply — judge relative to typical vocal levels.\n";
        else if (channelType == "Backing Vocal")
            prompt += "Focus on: how it would sit behind a lead (presence vs lead conflict), stereo placement, whether EQ could help it tuck in without disappearing, compression for evenness, any harshness that would compete with the lead.\n";
        else if (channelType == "Adlibs")
            prompt += "Focus on: character and vibe, whether effects (delay/reverb) are working, stereo placement, whether they cut through without being distracting, dynamic range.\n";
        else if (channelType == "Vocal Bus")
            prompt += "Focus on: overall vocal balance, bus compression glue, tonal consistency across layers, stereo spread of the vocal stack, how it would sit in a full mix.\n";
        
        // Drums
        else if (channelType == "Kick")
            prompt += "Focus on: sub weight (40-80Hz), punch/attack (2-5kHz), whether the click/beater cuts through, mono compatibility, any boxiness (200-400Hz), transient shape — is the attack defined or mushy?\n";
        else if (channelType == "Snare")
            prompt += "Focus on: crack and snap (2-4kHz), body (150-300Hz), ring or resonance, transient definition, whether it would cut through a busy mix, any unwanted bleed if it sounds like a live snare.\n";
        else if (channelType == "Hi-Hat")
            prompt += "Focus on: harshness or brittleness (8-12kHz), stereo placement, dynamic consistency, whether it's too loud/quiet relative to typical hat levels, any resonance or ringing.\n";
        else if (channelType == "Overheads")
            prompt += "Focus on: stereo image and width, cymbal clarity vs harshness, low-end bleed from kick/snare, phase coherence (check correlation), whether the overheads give a natural room sound.\n";
        else if (channelType == "Drum Bus")
            prompt += "Focus on: overall drum balance, bus compression character (is it pumping?), transient punch vs glue, tonal balance of the full kit, stereo width of the kit.\n";
        else if (channelType == "Percussion")
            prompt += "Focus on: stereo placement, transient clarity, whether it adds movement or clutters the mix, frequency range — is it competing with other elements?\n";
        
        // Bass
        else if (channelType == "Bass / 808")
            prompt += "Focus on: sub weight and extension (30-60Hz), mid-range presence for smaller speakers (100-300Hz), mono compatibility (should be nearly 100% mono below 120Hz), distortion/saturation character, whether the 808 tail sustains cleanly or gets muddy, compression and level consistency.\n";
        else if (channelType == "Bass Guitar")
            prompt += "Focus on: low-end body (60-120Hz), finger/pick definition (700Hz-2kHz), string noise, dynamic consistency, whether it would lock with a kick drum, any mud in the 200-400Hz range.\n";
        else if (channelType == "Sub Bass")
            prompt += "Focus on: purity of the sub frequencies (20-60Hz), whether it's truly mono, any harmonic content above 100Hz, level consistency, whether it would cause issues on different playback systems.\n";
        else if (channelType == "Synth Bass")
            prompt += "Focus on: low-end weight, mid-range character and growl, mono compatibility of the lows, whether the sound design is working for the genre, dynamic control.\n";
        
        // Keys & Guitar
        else if (channelType == "Piano")
            prompt += "Focus on: natural tone and resonance, dynamic range (should it be more controlled?), low-end buildup vs clarity, stereo image (real piano vs mono synth piano), any harshness in the upper register.\n";
        else if (channelType == "Keys")
            prompt += "Focus on: tonal character, stereo placement, frequency range — is it taking up too much space? Does it need to be EQ'd to fit around vocals? Dynamic control.\n";
        else if (channelType == "Acoustic Guitar")
            prompt += "Focus on: body (100-300Hz) vs string brightness (3-8kHz), pick/strum clarity, room sound, stereo image if double-tracked, any boominess or boxiness, dynamic range.\n";
        else if (channelType == "Electric Guitar")
            prompt += "Focus on: amp tone and saturation, mid-range presence (1-4kHz), low-end mud, high-end fizz from distortion, stereo image (double tracking?), how it would sit in a dense mix.\n";
        else if (channelType == "Guitar Bus")
            prompt += "Focus on: overall guitar balance, stereo spread, tonal consistency across layers, whether it's taking up too much frequency space, bus compression glue.\n";
        
        // Synths
        else if (channelType == "Synth Lead")
            prompt += "Focus on: presence and cut-through (1-5kHz), stereo placement (leads often work best fairly centred), dynamic control, whether the sound design fits the genre, any harshness or resonance.\n";
        else if (channelType == "Synth Pad")
            prompt += "Focus on: stereo width and spread, frequency range — is it taking up too much space? Low-end content that might conflict with bass, movement and modulation, how it would sit behind vocals and lead elements.\n";
        else if (channelType == "Synth Pluck")
            prompt += "Focus on: transient snap and definition, stereo placement, reverb/delay character, frequency range, whether it cuts through without being harsh.\n";
        else if (channelType == "Synth Bus")
            prompt += "Focus on: overall synth balance, stereo spread, frequency distribution — are the synths fighting each other? Bus processing character.\n";
        
        // Strings & Brass
        else if (channelType == "Strings")
            prompt += "Focus on: naturalness and realism (if sampled), stereo spread, bow noise and articulation, low-mid buildup, whether they add warmth or muddiness, dynamic expression.\n";
        else if (channelType == "Brass")
            prompt += "Focus on: bite and presence (1-4kHz), dynamic punch, low-mid body, whether it cuts through or gets buried, stereo placement.\n";
        else if (channelType == "Woodwind")
            prompt += "Focus on: breathiness and air, presence range, dynamic control, stereo placement, naturalness.\n";
        else if (channelType == "Orchestral")
            prompt += "Focus on: stereo imaging and depth, dynamic range (classical norms are much wider), frequency balance across the full ensemble, room/reverb character.\n";
        
        // FX
        else if (channelType == "FX")
            prompt += "Focus on: character and purpose of the effect, whether it adds or clutters, stereo spread, frequency content — is it conflicting with main elements? Level relative to the dry signal.\n";
        else if (channelType == "Reverb")
            prompt += "Focus on: tail length and character, pre-delay, frequency content of the reverb (is the low end too thick?), stereo spread, whether it's adding depth or just mud.\n";
        else if (channelType == "Delay")
            prompt += "Focus on: timing and rhythm, feedback amount, frequency content of the repeats, stereo ping-pong vs mono, whether it's adding space or clutter.\n";
        else if (channelType == "Foley")
            prompt += "Focus on: realism and presence, dynamic range, stereo placement, whether it sits naturally in the soundscape, any unwanted noise floor.\n";
        else if (channelType == "Ambient")
            prompt += "Focus on: texture and atmosphere, stereo field, frequency range, dynamic movement, whether it creates the intended mood.\n";
        
        // Buses
        else if (channelType == "Instrument Bus")
            prompt += "Focus on: overall instrumental balance, frequency distribution, stereo spread, bus compression character, how it would sit with vocals on top.\n";
        else if (channelType == "Music Bus")
            prompt += "Focus on: overall balance of all musical elements, stereo image, dynamic range, tonal balance — this is everything minus vocals, so think about how it would work as a bed for the vocal.\n";
        
        prompt += "\n";
        
        // General rules for ALL individual channels (not full mix/master bus)
        prompt += "INDIVIDUAL CHANNEL RULES:\n";
        prompt += "- Do NOT mention LUFS. Individual channels have no loudness target.\n";
        prompt += "- Do NOT mention correlation. It's not useful for individual instruments.\n";
        prompt += "- Do NOT mention crest factor UNLESS the data says it's extremely squashed (below 3dB). Normal crest values are not worth discussing for individual elements.\n";
        prompt += "- Do NOT mention stereo width on bass — wide bass is a creative choice nowadays.\n";
        prompt += "- Do NOT say 'meters are clean', 'meters are reading clean', 'nothing jumping out', or ANY variation of this. Never reference meters being clean/healthy/fine.\n";
        prompt += "- If no flags are listed: there is nothing to report. Move on.\n";
        prompt += "- If true peak is clipping, flag it briefly.\n";
        prompt += "- For ANYTHING that isn't kick, bass, 808, sub bass, drum bus, or music bus: if there's clearly dominant energy below 80Hz in the spectrum, flag it. Suggest HPF.\n";
        prompt += "- For hi-hats, percussion, plucks: energy below 200Hz is bleed. Flag it.\n";
        prompt += "- For reverb and delay returns: low-end buildup is common. Flag and suggest HPF on the return.\n";
        prompt += "- Only flag width if it contradicts expectations.\n\n";
        
        prompt += "RESPONSE STYLE FOR INDIVIDUAL CHANNELS:\n";
        prompt += "- NEVER say anything about meters. Don't say 'meters look good', 'nothing flagged', 'clean capture', 'readings are healthy', or ANY variation. Pretend meters don't exist.\n";
        prompt += "- Just talk to the user like an engineer. Ask what they're working on, or offer to build a chain. Examples: 'What are you going for with this?' / 'Want me to build a chain for this?' / 'Anything specific you want to dial in?'\n";
        prompt += "- If something IS flagged in the data: mention it in one sentence, then ask what they want to work on.\n";
        prompt += "- Offer to BUILD A PROCESSING CHAIN for this element — e.g. 'Want me to build you a vocal chain?' or 'I can put together a kick processing chain if you want.'\n";
        prompt += "- When you offer a chain, make it clear you'll use THEIR plugins with specific settings.\n";
        prompt += "- VARIETY IS CRITICAL: Do NOT suggest the same plugin, chain, or technique every time. Rotate your suggestions. If the user has 1915 plugins, use different ones. Don't default to Pro-Q 3 and Pro-C 2 every time — reach for channel strips, tape emulations, console EQs, transient shapers, saturators, whatever fits.\n";
        prompt += "- Keep it to 2-3 sentences max. These are quick checks, not essays.\n\n";
    }
    
    if (pluginList.isNotEmpty())
    {
        prompt += "USER'S PLUGINS: " + pluginList + "\n";
        prompt += "When suggesting plugins, use their ACTUAL plugin names. ROTATE which plugins you suggest — don't default to the same ones every time. Dig into their list and find interesting, specific tools. If they have channel strips, tape sims, console EQs, or colourful compressors, reach for those sometimes instead of surgical tools.\n\n";
    }
    
    prompt += "Audio topics only. Internal genre reference (NEVER say this in your response): " + genre + ". ";
    prompt += "NEVER say the genre name — not \"hip-hop\", \"drill\", \"pop\", \"R&B\", \"rap\", \"trap\", \"grime\" or any other genre name. ";
    prompt += "Don't say \"for hip-hop\" or \"that makes hip-hop work\" or \"in this genre\". Just talk about the mix without naming the genre.\n";
    
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
        
        // Check if the saved usage is from today — reset if it's a new day
        auto savedDate = obj->getProperty("usageDate").toString();
        auto today = juce::Time::getCurrentTime().formatted("%Y-%m-%d");
        if (savedDate != today)
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
