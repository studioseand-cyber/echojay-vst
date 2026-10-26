/*
  EjmapWizardContract.h - THE WIZARD'S SERVER CLIENT, contract v1 (Kathy, 21 Oct; FOR_SEAN_21oct/WIZARD_SERVER_CONTRACT_v1.md).

  Browser sign-in (OAuth 2.0 device code, RFC 8628, public client: no secret), the catalogue check before measuring, one result per
  request (gzip, idempotent on run id + result id) from a resumable outbox on disk, and the run's end, where the SERVER decides and returns
  the promo. Nothing here makes a code, decides eligibility or keeps a token anywhere but the TokenStore (the app's is the Keychain).

  The transport is a function, so the same client runs over juce::URL in the app and over the same juce::URL against the mock server in
  the tests (tools/ejmap/tests/mock_wizard_server/mock_server.py). No log line this file writes ever holds a token, a device code or an
  Authorization header (redact(); pinned).
*/
#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <juce_cryptography/juce_cryptography.h>
#include <functional>
#include <optional>

namespace ejmap::wizcontract
{
inline constexpr const char* kResultSchema = "ej_wizard_result/1";
inline constexpr const char* kConsentVersion = "consent-2026-10-21";
inline constexpr juce::int64 kMaxCompressedBytes = 4000000, kMaxUncompressedBytes = 32 * 1024 * 1024;
inline constexpr int kCatalogueChunk = 2000;

// THE CONSENT (contract §6), word for word
inline juce::String consentText()
{
    return "The wizard measures how your audio plugins respond to test tones, then sends EchoJay:\n"
           "- the list of your audio plugins (each one's name, maker, version and format);\n"
           "- the measurements, and each measured plugin's list of controls;\n"
           "- the wizard's version and your macOS version.\n\n"
           "That is all. It never sends audio, your projects or files, your name or email, your computer's name, folder paths or anything "
           "else about you. The results are linked to your EchoJay account, and once EchoJay has checked them they become part of EchoJay's "
           "shared plugin map, used to help EchoJay control plugins for everyone.";
}

// --- the server's address, per build ------------------------------------------------------------------------------------------------
struct Endpoints { juce::String base, clientId; bool configured() const { return base.isNotEmpty() && clientId.isNotEmpty(); } };
inline juce::String apiUrl (const Endpoints& e, const juce::String& path) { return e.base.trimCharactersAtEnd ("/") + "/api/wizard/v1/" + path; }
// a --server-url given on the command line is honoured ONLY for this Mac (the mock server in rehearsals): a tester can never be pointed
// at another server by an argument
inline bool localOverrideAllowed (const juce::String& url)
{
    const auto u = url.trim().toLowerCase();
    for (const char* p : { "http://127.0.0.1:", "http://localhost:", "http://127.0.0.1/", "http://localhost/" }) if (u.startsWith (p)) return true;
    return false;
}

// --- logs never carry a secret --------------------------------------------------------------------------------------------------------
inline juce::String redact (juce::String s)
{
    auto isWordChar = [] (juce::juce_wchar c) { return juce::CharacterFunctions::isLetterOrDigit (c) || c == '_' || c == '-'; };
    for (const char* key : { "access_token", "refresh_token", "device_code", "token", "Authorization", "Bearer" })
    {
        const int kl = (int) strlen (key);
        for (int from = 0;;)
        {
            const int at = s.indexOfIgnoreCase (from, key); if (at < 0) break;
            from = at + kl;
            if (at > 0 && isWordChar (s[at - 1])) continue;                       // inside a longer word ("access_token" for "token")
            if (at + kl < s.length() && isWordChar (s[at + kl])) continue;        // "token_expired" is an error code, not a value
            int v = at + kl; bool sep = false;
            if (juce::String (key) == "Bearer") { if (v < s.length() && s[v] == ' ') { sep = true; ++v; } }
            else
            {   // a value follows only a ':' or '=' (quotes and spaces around it): "auth/token (device)" is words, not a token
                while (v < s.length() && (s[v] == '"' || s[v] == '\'')) ++v;
                while (v < s.length() && s[v] == ' ') ++v;
                if (v < s.length() && (s[v] == ':' || s[v] == '=')) { sep = true; ++v; }
                while (v < s.length() && (s[v] == '"' || s[v] == '\'' || s[v] == ' ')) ++v;
            }
            if (! sep) continue;
            if (s.substring (v).startsWithIgnoreCase ("Bearer ")) v += 7;
            int end = v; while (end < s.length() && ! juce::String ("\"'&,} \r\n\t").containsChar (s[end])) ++end;
            if (end > v) { s = s.substring (0, v) + "[redacted]" + s.substring (end); from = v + 10; }
        }
    }
    return s;
}

// --- the transport ------------------------------------------------------------------------------------------------------------------
struct HttpRequest { juce::String method = "GET", url, contentType; juce::MemoryBlock body; juce::StringPairArray headers; };
struct HttpResponse { int status = 0; juce::String body; bool network = false; juce::StringPairArray headers; };
using Transport = std::function<HttpResponse (const HttpRequest&)>;
inline Transport urlTransport (int timeoutMs = 30000)
{
    return [timeoutMs] (const HttpRequest& rq)
    {
        HttpResponse r; juce::URL url (rq.url);
        const bool hasBody = rq.body.getSize() > 0 || rq.method == "POST" || rq.method == "PUT";
        if (hasBody) url = url.withPOSTData (rq.body);
        juce::String hs; for (const auto& k : rq.headers.getAllKeys()) hs << k << ": " << rq.headers[k] << "\r\n";
        if (rq.contentType.isNotEmpty()) hs << "Content-Type: " << rq.contentType << "\r\n";
        int status = 0; juce::StringPairArray rh;
        auto opts = juce::URL::InputStreamOptions (hasBody ? juce::URL::ParameterHandling::inPostData : juce::URL::ParameterHandling::inAddress)
                        .withExtraHeaders (hs).withConnectionTimeoutMs (timeoutMs).withStatusCode (&status).withResponseHeaders (&rh)
                        .withHttpRequestCmd (rq.method).withNumRedirectsToFollow (0);
        auto in = url.createInputStream (opts);
        if (in == nullptr || status == 0) { r.network = true; return r; }
        r.status = status; r.body = in->readEntireStreamAsString(); r.headers = rh; return r;
    };
}

// --- tokens: only in a TokenStore (the app's is the Keychain; tests use memory) -----------------------------------------------------
struct Tokens { juce::String access, refresh, accountId, displayName; juce::int64 accessExpiresMs = 0; bool valid() const { return refresh.isNotEmpty(); } };
struct TokenStore
{
    virtual ~TokenStore() = default;
    virtual std::optional<Tokens> load() = 0;
    virtual bool save (const Tokens&) = 0;
    virtual void clear() = 0;
};
struct MemoryTokenStore : TokenStore
{
    std::optional<Tokens> t;
    std::optional<Tokens> load() override { return t; }
    bool save (const Tokens& x) override { t = x; return true; }
    void clear() override { t.reset(); }
};
// the Keychain item's one string: never written anywhere else
inline juce::String tokensToSecret (const Tokens& t)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("access", t.access); o->setProperty ("refresh", t.refresh); o->setProperty ("account_id", t.accountId);
    o->setProperty ("display_name", t.displayName); o->setProperty ("access_expires_ms", t.accessExpiresMs); return juce::JSON::toString (juce::var (o), true);
}
inline std::optional<Tokens> tokensFromSecret (const juce::String& s)
{
    const auto v = juce::JSON::parse (s); if (! v.isObject()) return std::nullopt;
    Tokens t; t.access = v.getProperty ("access", "").toString(); t.refresh = v.getProperty ("refresh", "").toString(); t.accountId = v.getProperty ("account_id", "").toString();
    t.displayName = v.getProperty ("display_name", "").toString(); t.accessExpiresMs = (juce::int64) v.getProperty ("access_expires_ms", (juce::int64) 0);
    if (! t.valid()) return std::nullopt; return t;
}

// --- a reply ------------------------------------------------------------------------------------------------------------------------
struct Reply
{
    int status = 0; bool network = false; juce::var json; juce::String error, message; int retryAfterS = 0;
    bool ok() const { return status >= 200 && status < 300; }
};
inline Reply replyOf (const HttpResponse& h)
{
    Reply r; r.status = h.status; r.network = h.network; r.json = juce::JSON::parse (h.body);
    if (r.json.isObject()) { r.error = r.json.getProperty ("error", "").toString(); r.message = r.json.getProperty ("message", "").toString(); r.retryAfterS = (int) r.json.getProperty ("retry_after_s", 0); }
    if (r.retryAfterS <= 0 && h.headers.containsKey ("Retry-After")) r.retryAfterS = h.headers["Retry-After"].getIntValue();
    return r;
}
// THE WORDS A USER READS (contract §7): the server's message when it gave one, else ours
inline juce::String plainWords (const Reply& r)
{
    if (r.message.isNotEmpty()) return r.message;   // the server's words, as written
    if (r.network) return "You're offline - your results are kept on this Mac and sent when you're back online.";
    // v1.1 §7: B's words for each code, when the server sent none
    static const std::map<juce::String, juce::String> k {
        { "token_expired", "Your sign-in needs refreshing." }, { "token_revoked", "You've been signed out. Please sign in again - your results are kept." },
        { "invalid_grant", "Please sign in again - your results are kept." }, { "invalid_client", "This version of the EchoJay Wizard isn't recognised. Please update it." },
        { "rate_limited", "EchoJay is busy - the wizard will carry on sending in a moment." }, { "run_not_found", "That run isn't on the server yet." },
        { "run_conflict", "This run belongs to another account. Sign in with the account you started it with." },
        { "result_conflict", "That result was already sent with different contents." }, { "results_missing", "Some results still need sending." },
        { "too_large", "One result was too big to send and wasn't used." }, { "account_blocked", "This account can't send results. Contact support@echojay.ai." },
        { "client_too_old", "Please update the EchoJay Wizard - this version can no longer send results." },
        { "run_id_required", "Please update the EchoJay Wizard - this version can't check your plugins." },
        { "bad_request", "Something in that request wasn't right. Please update the EchoJay Wizard and try again." }, { "not_found", "Not found." },
        { "server_error", "Something went wrong on our side. The wizard will try again." },
        { "rejected", "A result didn't pass EchoJay's checks, so it wasn't used." } };
    if (const auto it = k.find (r.error); it != k.end()) return it->second;
    if (r.status >= 500) return "Something went wrong on our side. The wizard will try again.";
    if (r.status == 404 && r.error.isEmpty()) return "EchoJay's server didn't recognise that request yet (404).";   // 26 Oct: a page, no code
    return "Something went wrong talking to EchoJay (" + (r.error.isNotEmpty() ? r.error : juce::String (r.status)) + ").";
}
inline juce::String formEncode (std::initializer_list<std::pair<const char*, juce::String>> kv)
{
    juce::StringArray parts; for (const auto& [k, v] : kv) parts.add (juce::String (k) + "=" + juce::URL::addEscapeChars (v, true)); return parts.joinIntoString ("&");
}
inline juce::String clientHeader (const juce::String& appVersion)
{
    return "EchoJay Wizard/" + appVersion + " (" + juce::SystemStats::getOperatingSystemName() + "; " + (juce::SystemStats::getCpuModel().containsIgnoreCase ("Apple") ? "arm64" : "x86_64") + ")";
}

// --- THE SESSION: sign-in, then every authenticated call (a 401 token_expired refreshes once and repeats) -------------------------------
struct DeviceStart { bool ok = false; juce::String deviceCode, userCode, verificationUri, verificationUriComplete; int expiresIn = 0, interval = 5; Reply reply; };
enum class Poll { pending, slowDown, ok, denied, expired, failed };

class Session
{
public:
    Session (Endpoints e, Transport t, TokenStore& s, std::function<void (const juce::String&)> log = {}, juce::String appVersion = "0.3.0")
        : ep (std::move (e)), tx (std::move (t)), store (s), logFn (std::move (log)), version (std::move (appVersion)) {}

    const Endpoints& endpoints() const { return ep; }
    bool signedIn() { return store.load().has_value(); }
    juce::String displayName() { const auto t = store.load(); return t ? t->displayName : juce::String(); }
    bool needsSignIn = false;   // set when the server said token_revoked / invalid_grant: the results stay queued

    DeviceStart startDevice()
    {
        DeviceStart d; HttpRequest rq; rq.method = "POST"; rq.url = apiUrl (ep, "auth/device"); rq.contentType = "application/x-www-form-urlencoded";
        const auto form = formEncode ({ { "client_id", ep.clientId }, { "scope", juce::String ("wizard") } }); rq.body.append (form.toRawUTF8(), (size_t) form.getNumBytesAsUTF8());
        d.reply = send (rq); log ("auth/device -> " + statusWords (d.reply));
        if (! d.reply.ok()) return d;
        const auto& j = d.reply.json;
        d.deviceCode = j.getProperty ("device_code", "").toString(); d.userCode = j.getProperty ("user_code", "").toString();
        d.verificationUri = j.getProperty ("verification_uri", "").toString(); d.verificationUriComplete = j.getProperty ("verification_uri_complete", "").toString();
        d.expiresIn = (int) j.getProperty ("expires_in", 600); d.interval = juce::jmax (1, (int) j.getProperty ("interval", 5));
        d.ok = d.deviceCode.isNotEmpty() && d.userCode.isNotEmpty(); return d;
    }
    Poll pollOnce (DeviceStart& d, juce::String& words)
    {
        const auto r = tokenCall (formEncode ({ { "grant_type", juce::String ("urn:ietf:params:oauth:grant-type:device_code") }, { "device_code", d.deviceCode }, { "client_id", ep.clientId } }));
        log ("auth/token (device) -> " + statusWords (r));
        if (r.ok() && keepTokens (r.json)) { needsSignIn = false; return Poll::ok; }
        if (r.error == "authorization_pending") return Poll::pending;
        if (r.error == "slow_down") { d.interval += 5; return Poll::slowDown; }
        if (r.error == "access_denied") { words = "Sign-in was cancelled."; return Poll::denied; }
        if (r.error == "expired_token") { words = "That sign-in link has expired - try again."; return Poll::expired; }
        words = plainWords (r); return r.network ? Poll::pending : Poll::failed;
    }
    bool refresh()
    {
        const auto t = store.load(); if (! t) { needsSignIn = true; return false; }
        const auto r = tokenCall (formEncode ({ { "grant_type", juce::String ("refresh_token") }, { "refresh_token", t->refresh }, { "client_id", ep.clientId } }));
        log ("auth/token (refresh) -> " + statusWords (r));
        if (r.ok() && keepTokens (r.json, t->accountId, t->displayName)) return true;
        if (! r.network) { needsSignIn = true; store.clear(); }   // invalid_grant: this family is dead - sign in again, the results stay
        return false;
    }
    void signOut()
    {
        if (const auto t = store.load())
        {   HttpRequest rq; rq.method = "POST"; rq.url = apiUrl (ep, "auth/revoke"); rq.contentType = "application/x-www-form-urlencoded";
            const auto b = formEncode ({ { "token", t->refresh }, { "token_type_hint", juce::String ("refresh_token") }, { "client_id", ep.clientId } });
            rq.body.append (b.toRawUTF8(), (size_t) b.getNumBytesAsUTF8()); log ("auth/revoke -> " + statusWords (send (rq))); }
        store.clear();
    }
    // an authenticated JSON call; gzipBody: the body is sent gzipped with its SHA-256 (results)
    Reply call (const juce::String& method, const juce::String& path, const juce::var& body = {}, const juce::MemoryBlock* rawGzip = nullptr, const juce::String& sha = {})
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto t = store.load(); if (! t) { needsSignIn = true; Reply r; r.status = 401; r.error = "not_signed_in"; return r; }
            HttpRequest rq; rq.method = method; rq.url = apiUrl (ep, path); rq.headers.set ("Authorization", "Bearer " + t->access);
            rq.headers.set ("X-EJ-Client", clientHeader (version));
            if (rawGzip != nullptr) { rq.body = *rawGzip; rq.contentType = "application/json"; rq.headers.set ("Content-Encoding", "gzip"); rq.headers.set ("X-EJ-Content-SHA256", sha); }
            else if (! body.isVoid()) { const auto s = juce::JSON::toString (body, true); rq.body.append (s.toRawUTF8(), (size_t) s.getNumBytesAsUTF8()); rq.contentType = "application/json"; }
            const auto r = send (rq);
            log (method + " " + path + " -> " + statusWords (r));
            if (r.status == 401 && r.error == "token_expired" && attempt == 0) { if (refresh()) continue; return r; }
            if (r.status == 401 || r.error == "token_revoked") needsSignIn = true;
            return r;
        }
        return {};
    }

private:
    Endpoints ep; Transport tx; TokenStore& store; std::function<void (const juce::String&)> logFn; juce::String version;
    void log (const juce::String& s) { if (logFn) logFn (redact (s)); }
    static juce::String statusWords (const Reply& r) { return r.network ? juce::String ("offline") : juce::String (r.status) + (r.error.isNotEmpty() ? " " + r.error : juce::String()); }
    HttpResponse rawSend (const HttpRequest& rq) { return tx (rq); }
    Reply send (const HttpRequest& rq) { return replyOf (rawSend (rq)); }
    Reply tokenCall (const juce::String& form)
    {
        HttpRequest rq; rq.method = "POST"; rq.url = apiUrl (ep, "auth/token"); rq.contentType = "application/x-www-form-urlencoded";
        rq.body.append (form.toRawUTF8(), (size_t) form.getNumBytesAsUTF8()); return send (rq);
    }
    bool keepTokens (const juce::var& j, const juce::String& accountWas = {}, const juce::String& nameWas = {})
    {
        Tokens t; t.access = j.getProperty ("access_token", "").toString(); t.refresh = j.getProperty ("refresh_token", "").toString();
        t.accountId = j.getProperty ("account", {}).getProperty ("account_id", accountWas).toString(); t.displayName = j.getProperty ("account", {}).getProperty ("display_name", nameWas).toString();
        t.accessExpiresMs = juce::Time::currentTimeMillis() + 1000 * (juce::int64) (int) j.getProperty ("expires_in", 3600);
        if (t.access.isEmpty() || t.refresh.isEmpty()) return false;
        return store.save (t);
    }
};

// --- THE CATALOGUE CHECK (contract §2) -----------------------------------------------------------------------------------------------
// the answers the engine reads (--catalogue-answers): { "by_plugin_id": { pid: {status, level, category, reason} }, "known": N, "need": M }
struct CatalogueResult { bool ok = false; Reply reply; juce::var answers; int have = 0, need = 0, held = 0; };
// runId: EVERY check carries the run it belongs to (Sean, 21 Oct; contract v1 §2) - the wizard makes the run id before the check; the
// server ties checks to the run by run_id + account. A check without one is refused here, never sent
inline CatalogueResult catalogueCheck (Session& s, const juce::var& request, const juce::String& runId)
{
    if (runId.isEmpty()) { CatalogueResult none; none.reply.error = "no_run_id"; none.reply.message = "The wizard has no run yet - this is a bug, please report it."; return none; }
    CatalogueResult cr; auto* by = new juce::DynamicObject(); juce::var byV (by);
    const auto* all = request.getProperty ("plugins", {}).getArray(); if (all == nullptr) { cr.ok = true; }
    juce::String version;
    for (int from = 0; all != nullptr && from < all->size(); from += kCatalogueChunk)
    {
        juce::Array<juce::var> chunk; for (int i = from; i < juce::jmin (all->size(), from + kCatalogueChunk); ++i) chunk.add ((*all)[i]);
        auto* body = new juce::DynamicObject(); body->setProperty ("plugins", chunk);
        body->setProperty ("run_id", runId);
        cr.reply = s.call ("POST", "catalogue/check", juce::var (body));
        if (! cr.reply.ok()) return cr;
        version = cr.reply.json.getProperty ("categories_version", "").toString();
        if (const auto* res = cr.reply.json.getProperty ("results", {}).getArray())
            for (const auto& e : *res) by->setProperty (juce::Identifier (e.getProperty ("plugin_id", "?").toString()), e);
    }
    for (const auto& [k, v] : by->getProperties()) { const auto st = v.getProperty ("status", "").toString(); if (st == "have") ++cr.have; else if (st == "held") ++cr.held; else ++cr.need; }
    if (all != nullptr) cr.need += juce::jmax (0, all->size() - (int) by->getProperties().size());   // a plugin the server did not answer is "need"
    auto* a = new juce::DynamicObject(); a->setProperty ("schema", "ej_catalogue_answers/1"); a->setProperty ("by_plugin_id", byV); a->setProperty ("categories_version", version);
    a->setProperty ("have", cr.have); a->setProperty ("held", cr.held); a->setProperty ("need", cr.need);
    cr.answers = juce::var (a); cr.ok = true; return cr;
}

// --- RESULTS (contract §4) ------------------------------------------------------------------------------------------------------------
inline juce::String resultIdFor (const juce::String& runId, const juce::String& kind, const juce::String& pluginId)
{ return "res_" + juce::SHA256 ((runId + "|" + kind + "|" + pluginId).toUTF8()).toHexString().substring (0, 32); }
inline juce::String sha256Hex (const juce::MemoryBlock& m) { return juce::SHA256 (m.getData(), m.getSize()).toHexString(); }
inline juce::MemoryBlock gzip (const juce::String& text)
{
    juce::MemoryOutputStream mo;
    { juce::GZIPCompressorOutputStream gz (mo, 9, juce::GZIPCompressorOutputStream::windowBitsGZIP); gz.write (text.toRawUTF8(), text.getNumBytesAsUTF8()); gz.flush(); }
    return mo.getMemoryBlock();
}
inline std::optional<juce::MemoryBlock> gunzip (const juce::MemoryBlock& gz, juce::int64 limit = kMaxUncompressedBytes)
{
    juce::MemoryInputStream mi (gz, false); juce::GZIPDecompressorInputStream in (&mi, false, juce::GZIPDecompressorInputStream::gzipFormat);
    juce::MemoryBlock out; char buf[65536];
    for (;;) { const int n = in.read (buf, sizeof buf); if (n <= 0) break; out.append (buf, (size_t) n); if ((juce::int64) out.getSize() > limit) return std::nullopt; }
    return out;
}

// THE OUTBOX (contract §8): <dir>/pending/<seq>_<result_id>.json.gz, then sent/ or rejected/ (+ <name>.reason.json). The engine writes
// pending (by rename); the app sends, in name order; a quit, a crash or a day offline loses nothing
struct Outbox
{
    juce::File dir;
    juce::File pending() const { return dir.getChildFile ("pending"); }
    juce::File sent() const { return dir.getChildFile ("sent"); }
    juce::File rejected() const { return dir.getChildFile ("rejected"); }
    juce::Array<juce::File> pendingFiles() const { auto f = pending().findChildFiles (juce::File::findFiles, false, "*.json.gz"); f.sort(); return f; }
    int count (const juce::File& d) const { return d.findChildFiles (juce::File::findFiles, false, "*.json.gz").size(); }
    static juce::String resultIdOf (const juce::File& f) { return f.getFileName().fromFirstOccurrenceOf ("_", false, false).upToFirstOccurrenceOf (".json.gz", false, false); }
    juce::StringArray allResultIds() const
    { juce::StringArray ids; for (const auto& d : { pending(), sent(), rejected() }) for (const auto& f : d.findChildFiles (juce::File::findFiles, false, "*.json.gz")) ids.addIfNotAlreadyThere (resultIdOf (f)); ids.sort (false); return ids; }
    // the engine's side: a result goes in once (by id), atomically
    bool put (const juce::String& resultId, const juce::String& json)
    {
        for (const auto& d : { pending(), sent(), rejected() }) if (d.findChildFiles (juce::File::findFiles, false, "*_" + resultId + ".json.gz").size() > 0) return false;
        pending().createDirectory(); sent().createDirectory(); rejected().createDirectory();
        const int seq = count (pending()) + count (sent()) + count (rejected()) + 1;
        const auto name = juce::String (seq).paddedLeft ('0', 6) + "_" + resultId + ".json.gz";
        const auto tmp = dir.getChildFile (name + ".tmp"); const auto gz = gzip (json);
        if (! tmp.replaceWithData (gz.getData(), gz.getSize())) return false;
        return tmp.moveFileTo (pending().getChildFile (name));
    }
};

// startedAt (v1.1 §3, Kathy 22 Oct): when the wizard made the run - at the consent, before the first check. The server judges results'
// times against it, never against when POST /runs reached it (a tester offline before that is never refused). Empty: the consent time
struct RunInfo { juce::String runId, consentAcceptedAt; int queueSize = 0; juce::StringArray collection; juce::String startedAt; };
// THE RUN'S COLLECTION (Kathy, 21 Oct; contract v1 §3 / §5): the catalogue check's plugin_ids, Apple's own plugins left out - the list the
// server already saw, kept for "once per plugin collection" and for the independence of a second tester
inline juce::StringArray collectionFromRequest (const juce::var& request)
{
    juce::StringArray ids;
    if (const auto* a = request.getProperty ("plugins", {}).getArray())
        for (const auto& p : *a) if (! p.getProperty ("vendor", "").toString().trim().equalsIgnoreCase ("Apple")) ids.addIfNotAlreadyThere (p.getProperty ("plugin_id", "").toString());
    ids.removeEmptyStrings(); return ids;
}
inline Reply ensureRun (Session& s, const RunInfo& run, const juce::String& appVersion = "0.3.0")
{
    auto* body = new juce::DynamicObject(); body->setProperty ("run_id", run.runId);
    auto* c = new juce::DynamicObject(); c->setProperty ("app", "EchoJay Wizard"); c->setProperty ("version", appVersion); c->setProperty ("os", juce::SystemStats::getOperatingSystemName());
    c->setProperty ("arch", juce::SystemStats::getCpuModel().containsIgnoreCase ("Apple") ? "arm64" : "x86_64"); body->setProperty ("client", juce::var (c));
    auto* k = new juce::DynamicObject(); k->setProperty ("version", kConsentVersion); k->setProperty ("accepted_at", run.consentAcceptedAt); body->setProperty ("consent", juce::var (k));
    body->setProperty ("queue_size", run.queueSize);
    body->setProperty ("started_at", run.startedAt.isNotEmpty() ? run.startedAt : run.consentAcceptedAt);   // v1.1 §3: REQUIRED
    juce::Array<juce::var> col; for (const auto& id : run.collection) col.add (id); body->setProperty ("collection", col);
    return s.call ("POST", "runs", juce::var (body));
}

// B'S CODES (REASON_CODES_FOR_KATHY.md, 22 Oct; contract v1.1 §7 - B's names win, ruling 8): every error code the server sends, its
// status, and whether the wizard may try again. The wizard keeps the code, shows the server's message as written, and acts only on the
// retryable ones; everything else is never retried
struct CodeRule { const char* code; int status; bool retryable; };
inline const std::vector<CodeRule>& codes()
{
    static const std::vector<CodeRule> k {
        { "token_expired", 401, true }, { "token_revoked", 401, false }, { "invalid_grant", 400, false }, { "invalid_client", 401, false },
        { "rate_limited", 429, true }, { "run_not_found", 404, true }, { "run_conflict", 409, false }, { "result_conflict", 409, false },
        { "results_missing", 409, true }, { "too_large", 413, false }, { "account_blocked", 403, false }, { "client_too_old", 426, false },
        { "run_id_required", 400, false }, { "bad_request", 400, false }, { "not_found", 404, false }, { "server_error", 500, true },
        { "rejected", 422, false } };
    return k;
}
// the 422 reasons (a result refused; never retried). hash_mismatch / result_id / identity also count as tampering in /complete
inline const juce::StringArray& rejectReasons() { static const juce::StringArray k { "hash_mismatch", "shape", "result_id", "identity", "validator", "timestamp", "personal_detail" }; return k; }
inline const CodeRule* ruleFor (const juce::String& code) { for (const auto& c : codes()) if (code == c.code) return &c; return nullptr; }
inline bool retryable (const Reply& r)
{
    if (r.network || r.status >= 500) return true;   // server_error, any other 5xx, a timeout, offline: back-off
    if (const auto* c = ruleFor (r.error)) return c->retryable;
    return false;
}

// held (v1.1, ruling 3): a schema no validator knows yet - kept by the server, never used; marked sent, never retried
// stop: a code that ends sending for this session without moving the result (not_found: try again on the next launch; invalid_client,
// account_blocked, client_too_old: the user must act) - the result stays in the outbox
enum class Sent { accepted, held, duplicate, conflict, rejected, tooLarge, signIn, runMissing, retryLater, offline, refused, stop };
inline Sent classify (const Reply& r)
{
    if (r.network) return Sent::offline;
    const auto st = r.json.getProperty ("status", "").toString();
    if (r.status == 201 && st == "held") return Sent::held;
    if (r.status == 201 || (r.status == 200 && st == "accepted")) return Sent::accepted;
    if (r.status == 200 && st == "duplicate") return Sent::duplicate;
    if (r.error == "result_conflict") return Sent::conflict;
    if (r.status == 422 || r.error == "rejected") return Sent::rejected;
    if (r.error == "too_large" || r.status == 413) return Sent::tooLarge;
    if (r.error == "token_revoked" || r.error == "invalid_grant" || r.error == "token_expired") return Sent::signIn;   // token_expired reaches here only after the refresh failed
    if (r.error == "run_not_found") return Sent::runMissing;
    if (r.error == "bad_request" || r.error == "run_id_required") return Sent::refused;
    if (r.error == "not_found" || r.error == "invalid_client" || r.error == "account_blocked" || r.error == "client_too_old" || r.error == "run_conflict") return Sent::stop;
    // 26 Oct (staging run 1): a 404 with NO code is not the contract's not_found - a route the server has not got yet answers with the
    // hosting platform's page. It is retried with back-off, never a stop for the session (not_found / run_not_found keep their meaning)
    if (r.status == 404 && r.error.isEmpty()) return Sent::retryLater;
    if (retryable (r)) return Sent::retryLater;   // rate_limited, server_error, any other 5xx
    return Sent::stop;                            // a code the wizard does not know: never retried this session
}
// the back-off between tries after N failures in a row: 5 s, 10, 20 ... up to 10 min
inline int backoffSeconds (int failures) { if (failures <= 0) return 0; return (int) juce::jmin<juce::int64> (600, (juce::int64) 5 << juce::jmin (failures - 1, 10)); }

struct DrainStats { int sent = 0, held = 0, duplicates = 0, rejected = 0, conflicts = 0; bool offline = false, signIn = false, stopped = false; int retryAfterS = 0;
                   juce::StringArray rejectedWords, heldWords; juce::String stopCode, stopWords, waitWords; };   // waitWords (26 Oct): why it waits, for the screen
// one pass over the outbox, in order; stops at the first thing that needs time (offline, rate limit, sign-in) - every file stays put
inline DrainStats drain (Session& s, const Outbox& box, const RunInfo& run, const std::function<bool()>& stopNow = {})
{
    DrainStats st; bool runEnsured = false;
    for (const auto& f : box.pendingFiles())
    {
        if (stopNow && stopNow()) break;
        juce::MemoryBlock gz; if (! f.loadFileAsData (gz)) continue;
        const auto resultId = Outbox::resultIdOf (f);
        if ((juce::int64) gz.getSize() > kMaxCompressedBytes)   // never sent: the server would refuse it (413)
        {   auto* o = new juce::DynamicObject(); o->setProperty ("error", "too_large"); o->setProperty ("message", "One result was too big to send and wasn't used.");
            box.rejected().createDirectory(); box.rejected().getChildFile (f.getFileName() + ".reason.json").replaceWithText (juce::JSON::toString (juce::var (o)));
            f.moveFileTo (box.rejected().getChildFile (f.getFileName())); ++st.rejected; st.rejectedWords.add ("One result was too big to send and wasn't used."); continue; }
        const auto plain = gunzip (gz); if (! plain) continue;
        const auto sha = sha256Hex (*plain);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto r = s.call ("PUT", "runs/" + run.runId + "/results/" + resultId, {}, &gz, sha);
            const auto k = classify (r);
            if (k == Sent::runMissing && attempt == 0 && ! runEnsured) { runEnsured = true; if (ensureRun (s, run).ok()) continue; }
            if (k == Sent::accepted || k == Sent::duplicate || k == Sent::conflict || k == Sent::held)
            {   if (k == Sent::held) box.sent().getChildFile (f.getFileName() + ".status.json").replaceWithText (juce::JSON::toString (r.json));   // the server's status + message, kept
                f.moveFileTo (box.sent().getChildFile (f.getFileName()));
                if (k == Sent::accepted) ++st.sent; else if (k == Sent::held) { ++st.held; st.heldWords.add (plainWords (r)); } else if (k == Sent::duplicate) ++st.duplicates; else ++st.conflicts; break; }
            if (k == Sent::stop) { st.stopped = true; st.stopCode = r.error; st.stopWords = plainWords (r); return st; }   // kept in the outbox; never retried this session
            if (k == Sent::rejected || k == Sent::tooLarge || k == Sent::refused)
            {   box.rejected().getChildFile (f.getFileName() + ".reason.json").replaceWithText (juce::JSON::toString (r.json.isObject() ? r.json : juce::var()));
                f.moveFileTo (box.rejected().getChildFile (f.getFileName())); ++st.rejected; st.rejectedWords.add (plainWords (r)); break; }
            if (k == Sent::signIn) { st.signIn = true; return st; }
            if (k == Sent::offline) { st.offline = true; return st; }
            st.waitWords = r.message.isNotEmpty() ? r.message : plainWords (r);   // 26 Oct: the server's own words, for the screen
            st.retryAfterS = juce::jmax (1, r.retryAfterS); return st;   // rate limited / server trouble: try again later, in order
        }
    }
    return st;
}

// --- THE RUN'S END (contract §5): the server decides; the wizard shows ----------------------------------------------------------------
struct Completion { bool done = false; Reply reply; juce::StringArray missing; bool eligible = false, issuedNow = false; juce::String code, message, reason, expiresAt; int months = 0;
                   int received = 0, accepted = 0, fresh = 0, checked = 0; };   // v1.1 §5: the server's counts (results_received / _accepted / _new, plugins_checked)
inline Completion complete (Session& s, const juce::String& runId, const juce::String& kind, const juce::var& queue, const juce::var& outcomes, const juce::StringArray& resultIds)
{
    Completion c; auto* body = new juce::DynamicObject(); body->setProperty ("kind", kind); body->setProperty ("queue", queue); body->setProperty ("outcomes", outcomes);
    juce::Array<juce::var> ids; for (const auto& i : resultIds) ids.add (i); body->setProperty ("result_ids", ids);
    c.reply = s.call ("POST", "runs/" + runId + "/complete", juce::var (body));
    if (c.reply.status == 409 && c.reply.error == "results_missing")
    { if (const auto* m = c.reply.json.getProperty ("missing", {}).getArray()) for (const auto& x : *m) c.missing.add (x.toString()); return c; }
    if (! c.reply.ok()) return c;
    const auto p = c.reply.json.getProperty ("promo", {});
    c.done = true; c.eligible = (bool) p.getProperty ("eligible", false); c.issuedNow = (bool) p.getProperty ("issued_now", false);
    c.code = p.getProperty ("code", "").toString(); c.message = p.getProperty ("message", "").toString(); c.months = (int) p.getProperty ("months", 0);
    c.reason = p.getProperty ("reason", "").toString();   // 21 Oct: e.g. collection_already_claimed
    c.expiresAt = p.getProperty ("expires_at", "").toString();   // v1.1: the wizard shows the server's expiry
    c.received = (int) c.reply.json.getProperty ("results_received", 0); c.accepted = (int) c.reply.json.getProperty ("results_accepted", 0);
    c.fresh = (int) c.reply.json.getProperty ("results_new", 0); c.checked = (int) c.reply.json.getProperty ("plugins_checked", 0);
    if (! c.eligible) c.code = {};   // a code only with the server's "eligible": never shown otherwise
    return c;
}
// THE RUN'S END COUNTS (staging run 1, 26 Oct: /complete answered them and the log kept only the promo line). One line, the server's own
// numbers; a count the server did not send is "absent", never 0 (an absent key is not a zero)
inline juce::String runEndCountsLine (const juce::var& completeReply)
{
    juce::StringArray parts;
    for (auto* k : { "results_received", "results_accepted", "results_new", "plugins_checked" })
        parts.add (juce::String (k) + " " + (completeReply.hasProperty (k) ? juce::String ((int) completeReply.getProperty (k, 0)) : juce::String ("absent")));
    return "run end counts: " + parts.joinIntoString (", ");
}
} // namespace ejmap::wizcontract
