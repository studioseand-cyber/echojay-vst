#include "EchoJayWorkspace.h"

extern void ejTeardownLog(const juce::String& msg);

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
        if (c.id == chatId) { c.albumId = albumId; break; }

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
        if (c.id == chatId) { c.albumId = ""; break; }

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

bool EchoJayWorkspace::appendMessageToChat(const juce::String& chatId,
                                            const juce::String& role,
                                            const juce::String& content,
                                            const juce::String& reviewId)
{
    for (auto& c : chats)
    {
        if (c.id == chatId)
        {
            bool first = c.messages.empty();
            WsMessage m;
            m.role     = role;
            m.content  = content;
            m.reviewId = reviewId;
            c.messages.push_back(std::move(m));
            return first;
        }
    }
    // Chat not found — create a minimal entry so the message is persisted
    WsChat c;
    c.id      = chatId;
    c.title   = "New chat";
    c.created = juce::Time::getCurrentTime().toISO8601(true);
    WsMessage m;
    m.role     = role;
    m.content  = content;
    m.reviewId = reviewId;
    c.messages.push_back(std::move(m));
    chats.insert(chats.begin(), std::move(c));
    return true; // first message
}

void EchoJayWorkspace::setChatTitle(const juce::String& chatId,
                                     const juce::String& title)
{
    for (auto& c : chats)
        if (c.id == chatId) { c.title = title; return; }
}

void EchoJayWorkspace::setChatTrackName(const juce::String& chatId,
                                         const juce::String& trackName)
{
    for (auto& c : chats)
        if (c.id == chatId) { c.trackName = trackName; return; }
}

void EchoJayWorkspace::incrementChatRevisionCount(const juce::String& chatId)
{
    for (auto& c : chats)
        if (c.id == chatId) { c.revisionCount++; return; }
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

        // Parse albums
        albums.clear();
        if (auto* arr = root->getProperty("albums").getArray())
            for (auto& v : *arr)
                albums.push_back(parseAlbum(v));

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

            reviews.clear();
            if (auto* arr = root->getProperty("reviews").getArray())
                for (auto& v : *arr)
                    reviews.push_back(parseReview(v));

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

        albums.clear();
        if (auto* arr = root->getProperty("albums").getArray())
            for (auto& v : *arr)
                albums.push_back(parseAlbum(v));

        // Merge reviews — same as doLoad: preserve local-only entries
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

            reviews.clear();
            if (auto* arr = root->getProperty("reviews").getArray())
                for (auto& v : *arr)
                    reviews.push_back(parseReview(v));

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
                    c.messages.push_back(std::move(msg));
                }
            }
        }
    }
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

juce::var EchoJayWorkspace::chatToVar(const WsChat& c)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("id",            c.id);
    obj->setProperty("title",         c.title);
    obj->setProperty("created",       c.created);
    obj->setProperty("trackName",     c.trackName);
    obj->setProperty("albumId",       c.albumId);
    obj->setProperty("revisionCount", c.revisionCount);

    juce::Array<juce::var> msgs;
    for (auto& m : c.messages)
    {
        auto* mObj = new juce::DynamicObject();
        mObj->setProperty("role",    m.role);
        mObj->setProperty("content", m.content);
        if (m.reviewId.isNotEmpty())
            mObj->setProperty("_reviewId",  m.reviewId);
        if (m.meterCtx.isNotEmpty())
            mObj->setProperty("_meterCtx",  m.meterCtx);
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

    juce::Array<juce::var> cids, rids;
    for (auto& x : a.chatIds)   cids.add(x);
    for (auto& x : a.reviewIds) rids.add(x);
    obj->setProperty("chatIds",   juce::var(cids));
    obj->setProperty("reviewIds", juce::var(rids));
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
    obj->setProperty("data", juce::var(d));
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

    // --- profile ---
    auto pv = profileToVar(profile);
    if (auto* obj = pv.getDynamicObject()) stripUnderscoreKeys(*obj);
    root->setProperty("profile", pv);

    return juce::JSON::toString(juce::var(root), true);
}
