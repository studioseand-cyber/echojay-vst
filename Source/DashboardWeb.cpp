#include "DashboardWeb.h"
#include "EJStateRoot.h"
#include "EchoJayAPI.h"   // EchoJayAPI::transportEndpoint — the SAME base the API uses
#include "DashPoll.h"     // ejDashLog

namespace echojay
{

// ===========================================================================
// DashboardWebFlow — pure
// ===========================================================================
const char* DashboardWebFlow::stepName (Step s)
{
    switch (s)
    {
        case Step::LoadEmbed:          return "LoadEmbed";
        case Step::Seed:               return "Seed";
        case Step::LoadEmbedAfterSeed: return "LoadEmbedAfterSeed";
        case Step::LoadHandoff:        return "LoadHandoff";
        case Step::Done:               return "Done";
        case Step::Failed:             return "Failed";
    }
    return "?";
}

juce::String DashboardWebFlow::currentUrl() const
{
    switch (step)
    {
        case Step::LoadEmbed:
        case Step::LoadEmbedAfterSeed: return embedUrl();
        case Step::Seed:               return seedUrl();
        // The HOP, not the /go#... URL: the fragment must never pass through
        // goToURL, which percent-encodes it into the path (the mm:64 comment
        // in DashboardWeb.h). handleFinish issues the real navigation as JS.
        case Step::LoadHandoff:        return base + handoffHopPath;
        case Step::Done:
        case Step::Failed:             return {};
    }
    return {};
}

juce::String DashboardWebFlow::advance (Outcome o)
{
    if (o == Outcome::NetworkError) { step = Step::Failed; return {}; }

    switch (step)
    {
        case Step::LoadEmbed:
            step = (o == Outcome::Dashboard)
                       ? Step::Done
                       : ((gateEnabled && token.isNotEmpty()) ? Step::Seed : Step::Failed);
            break;

        case Step::Seed:
            // The seed page's own content is irrelevant — it exists only to set
            // the ej_v2_preview cookie. Either way, load the embed URL now.
            step = Step::LoadEmbedAfterSeed;
            break;

        case Step::LoadEmbedAfterSeed:
            step = (o == Outcome::Dashboard) ? Step::Done : Step::Failed;  // 2nd 404 = fail
            break;

        case Step::LoadHandoff:
            // Handoff path (contract §2): go.html either landed the bound
            // page or it did not. NO seed fallthrough from here - the seed
            // gates VISIBILITY, the handoff gates IDENTITY (§4), and the
            // editor owns the one re-mint (§5) by calling startWithHandoff
            // again with a fresh token.
            step = (o == Outcome::Dashboard) ? Step::Done : Step::Failed;
            break;

        case Step::Done:
        case Step::Failed:
            break;
    }
    return currentUrl();
}

// ===========================================================================
// loadChain payload validation — pure, HARD, before anything else touches it
// ===========================================================================
static bool isCleanId (const juce::String& s, int maxLen)
{
    if (s.isEmpty() || s.length() > maxLen) return false;
    for (int i = 0; i < s.length(); ++i)
    {
        const juce::juce_wchar c = s[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                     || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (! ok) return false;
    }
    return true;
}

juce::String validateLoadChain (const juce::var& payload, LoadChainRequest& req)
{
    auto* obj = payload.getDynamicObject();
    if (obj == nullptr) return "bad_payload";           // not an object

    const bool hasC = obj->hasProperty ("chainId");
    const bool hasS = obj->hasProperty ("slug");
    if (hasC == hasS) return "bad_payload";             // both keys present, or neither

    if (hasC)
    {
        const juce::var v = obj->getProperty ("chainId");
        if (! v.isString() || ! isCleanId (v.toString(), 64)) return "bad_payload";
        req.chainId = v.toString();
    }
    else
    {
        const juce::var v = obj->getProperty ("slug");
        if (! v.isString() || ! isCleanId (v.toString(), 32)) return "bad_payload";
        req.slug = v.toString();
    }
    return {};
}

juce::String validateOpenChat (const juce::var& payload, juce::String& chatId)
{
    chatId = {};
    auto* obj = payload.getDynamicObject();
    if (obj == nullptr) return "bad_payload";      // not an object

    // EMPTY payload -> open the Chat tab with NOTHING selected: {} (no chatId)
    // or {chatId:""}. Both yield an empty chatId, which the followDashLink chat
    // branch already treats as "switch to Chat, no selection" (recent-chats
    // "See all", and project tiles whose latestChatId is null — §5c fallback).
    if (! obj->hasProperty ("chatId")) return {};

    const juce::var v = obj->getProperty ("chatId");
    if (! v.isString()) return "bad_payload";       // present but wrong type (e.g. null, number)
    const juce::String id = v.toString();
    if (id.isEmpty()) return {};                    // {chatId:""} -> no selection

    if (! isCleanId (id, 64)) return "bad_payload"; // non-empty: same validation as before
    chatId = id;
    return {};
}

bool loadChainBusy (int numModalComponents, juce::int64 elapsedMs, juce::int64 windowMs)
{
    return numModalComponents > 0 || (elapsedMs >= 0 && elapsedMs < windowMs);
}

juce::String importedChainId (const juce::var& importResponse)
{
    auto* o = importResponse.getDynamicObject();
    return o != nullptr ? o->getProperty ("chainId").toString() : juce::String();
}

// ===========================================================================
// Inner — the JUCE webview, forwarding its callbacks out
// ===========================================================================
struct DashboardWeb::Inner : public juce::WebBrowserComponent
{
    explicit Inner (const Options& options) : juce::WebBrowserComponent (options) {}

    std::function<void (const juce::String&)> onFinished;
    std::function<void()>                     onNetError;

    // SUPPRESS RELOAD-ON-RESHOW. JUCE's WebBrowserComponent reloads lastURL on
    // EVERY visibility/parent-hierarchy change (parentHierarchyChanged /
    // visibilityChanged -> Impl::checkWindowAssociation -> reloadLastURL). That
    // reloads /dashboard?embed=plugin and resets our SPA to its initial route —
    // discarding the deep community/messages state, its composer and its
    // keyboard focus — and it flashes on every Link-window reparent. Allow the
    // FIRST association WHILE SHOWING (that renders the already-loaded page), and
    // suppress every one after: the loaded page stays resident across reshows.
    // The NSView peer attach is the child NSViewComponent's job, NOT this
    // method, so attachment is untouched. Each DashboardWeb construction gets a
    // fresh flag, so the lazy destroy-on-tab-away / rebuild-on-return is intact.
    bool firstShowAssociationDone_ = false;
    bool allowAssociation()
    {
        if (firstShowAssociationDone_) return false;      // a reshow — suppress the reload
        if (isShowing()) firstShowAssociationDone_ = true; // the one association that renders
        return true;                                       // (non-showing calls are no-ops we pass through)
    }
    void visibilityChanged() override
    {
        if (allowAssociation()) juce::WebBrowserComponent::visibilityChanged();
    }
    void parentHierarchyChanged() override
    {
        if (allowAssociation()) juce::WebBrowserComponent::parentHierarchyChanged();
    }

    void pageFinishedLoading (const juce::String& url) override
    {
        if (onFinished) onFinished (url);
    }

    bool pageLoadHadNetworkError (const juce::String&) override
    {
        // Return false: do NOT show JUCE's internal error page. The editor
        // handles the failure by staying on the native view.
        if (onNetError) onNetError();
        return false;
    }
};

// ===========================================================================
// DashboardWeb
// ===========================================================================
DashboardWeb::DashboardWeb()
{
    // THE SAME base the plugin's API talks to: dev.json's baseUrl under the dev
    // transport, production otherwise. Never a second hardcoded host — the
    // webview and the API pointing at different servers is the mismatch that
    // nearly happened once already.
    flow_.base = EchoJayAPI::pluginBaseUrl().trimCharactersAtEnd ("/");

   #if ECHOJAY_DEV_TRANSPORT
    flow_.gateEnabled = true;
    // Preview-gate scaffolding, dev-transport builds only (compiled out of a
    // release binary). The token is read at runtime, never a literal, and its
    // value is never logged.
    const auto tokFile = echojay::userStateHome()
                             .getChildFile (".echojay/v2preview.token");
    if (tokFile.existsAsFile())
        flow_.token = tokFile.loadFileAsString().trim();
   #endif

    auto safe = juce::Component::SafePointer<DashboardWeb> (this);

    // Stage 3 bridge: register EXACTLY ONE native function, loadChain (JUCE 8
    // bindings). Not the rest of the §8 table — an unregistered call is cleanly
    // feature-detectable from the page, while a stub answering "not implemented"
    // is a liar the page must special-case. Native integration must be enabled
    // for any binding to exist.
    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled (true)
        .withNativeFunction ("loadChain",
            [safe] (const juce::Array<juce::var>& args,
                    juce::WebBrowserComponent::NativeFunctionCompletion complete)
            {
                if (safe == nullptr) { complete (juce::var()); return; }
                safe->handleLoadChain (args, std::move (complete));
            })
        .withNativeFunction ("openChat",
            [safe] (const juce::Array<juce::var>& args,
                    juce::WebBrowserComponent::NativeFunctionCompletion complete)
            {
                if (safe == nullptr) { complete (juce::var()); return; }
                safe->handleOpenChat (args, std::move (complete));
            });

    web_ = std::make_unique<Inner> (options);
    web_->onFinished = [safe] (const juce::String& url) { if (safe) safe->handleFinish (url); };
    web_->onNetError = [safe] { if (safe) safe->advanceWith (DashboardWebFlow::Outcome::NetworkError); };
    addAndMakeVisible (*web_);
}

DashboardWeb::~DashboardWeb() = default;   // Inner complete here

void DashboardWeb::resized()
{
    if (web_ != nullptr) web_->setBounds (getLocalBounds());
}

void DashboardWeb::start()
{
    finished_ = false;
    flow_.step = DashboardWebFlow::Step::LoadEmbed;
    loadCurrent();
}

// The redeem budget: how long the WHOLE handoff — hop, JS replace, /go's
// redeem, target landing — gets before "no_landing". The step-1 harness
// measured a successful redeem + landing at ~2 s wall clock; a cold
// serverless redeem can add several more; the hop is one static 404 and
// adds noise. 15 s is that measurement with cold-start headroom, and still
// short enough that a genuinely dead redeem does not strand the tab on
// "Loading" — NOT the embed probe's 0.8 s, which was sized for a
// one-navigation flow and mislabeled a slow redeem as a failure on 21 Aug.
static constexpr int kRedeemBudgetMs = 15000;
// Marker polling once the target landed: the harness's cadence (~0.7 s), up
// to ~10 s of SPA hydration. Past that the redeem WORKED and the page did
// not render — "no_marker", a different failure that no re-mint can fix.
static constexpr int kMarkerPollMs = 700;
static constexpr int kMarkerPollMax = 14;

void DashboardWeb::startWithHandoff (const juce::String& goPath)
{
    finished_ = false;
    flow_.handoffPath = goPath;
    flow_.step = DashboardWebFlow::Step::LoadHandoff;
    // The bound target rides the fragment as &to=<path>; the verdict is
    // anchored to a load-finish on THAT path, never on /go's. Compare
    // path-only (an allowlisted target may carry a query).
    handoffTargetPath_ = "/dashboard";
    const int toAt = goPath.indexOf ("&to=");
    if (toAt >= 0)
    {
        auto t = goPath.substring (toAt + 4);
        const int q = t.indexOfChar ('?');
        if (q >= 0) t = t.substring (0, q);
        if (t.startsWithChar ('/')) handoffTargetPath_ = t;
    }
    handoffTargetReached_ = false;
    handoffHopDone_ = false;
    markerPolls_ = 0;
    ++handoffGen_;   // a pending budget/marker timer from a previous attempt is now void

    // The budget is armed HERE, at flow entry — never on observing a
    // particular load-finish. A verdict clock that only starts when a
    // specific path shows up cannot report the case where that path never
    // shows up, which is the case it exists for: on 21 Aug the mangled
    // /go%23t=... path matched nothing, no timer armed, and the tab sat on
    // "Loading" for ten minutes instead of failing in fifteen seconds.
    // Terminal outcomes cancel it via finished_; a re-mint voids it via
    // handoffGen_.
    {
        auto safe = juce::Component::SafePointer<DashboardWeb> (this);
        const int gen = handoffGen_;
        juce::Timer::callAfterDelay (kRedeemBudgetMs, [safe, gen]
        {
            if (safe == nullptr || safe->finished_ || gen != safe->handoffGen_) return;
            if (! safe->handoffTargetReached_)
                safe->finishDirect (false, "no_landing");
        });
    }
    loadCurrent();
}

// Path of a full URL, no query, no fragment. "/" when it cannot tell.
static juce::String urlPathOf (const juce::String& url)
{
    auto p = url;
    const int ss = p.indexOf ("://");
    if (ss >= 0)
    {
        const int sl = p.indexOfChar (ss + 3, '/');
        p = sl >= 0 ? p.substring (sl) : juce::String ("/");
    }
    const int h = p.indexOfChar ('#'); if (h >= 0) p = p.substring (0, h);
    const int q = p.indexOfChar ('?'); if (q >= 0) p = p.substring (0, q);
    return p.isEmpty() ? juce::String ("/") : p;
}

void DashboardWeb::handleFinish (const juce::String& url)
{
    // EVERY load-finish is recorded with its path; the fragment is never
    // logged but its presence and length are — raw '#' AND the %23-encoded
    // form (redactedFinishUrl in the header says why both). Until this line
    // existed, "redeem failed" and "redeem was slow" were the same
    // observation.
    ejDashLog ("[dashweb] finish " + redactedFinishUrl (url));
    if (finished_ || web_ == nullptr) return;

    if (flow_.step != DashboardWebFlow::Step::LoadHandoff) { probe (1); return; }

    const auto path = urlPathOf (url);

    if (path == handoffTargetPath_)
    {
        // The final navigation arrived: the redeem happened. The verdict is
        // now only about the page rendering its marker.
        handoffTargetReached_ = true;
        markerPolls_ = 0;
        pollMarker();
        return;
    }
    if (! handoffHopDone_)
    {
        // The hop page finished (whatever it served — see handoffHopPath).
        // Issue the REAL navigation as JS: location.replace carries the
        // fragment intact and client-side, which goToURL cannot (the mm:64
        // comment in DashboardWeb.h). No timer starts here — the budget was
        // armed at startWithHandoff and already covers this leg.
        handoffHopDone_ = true;
        ejDashLog ("[dashweb] hop replace "
                   + redactedFinishUrl (flow_.base + flow_.handoffPath));
        web_->evaluateJavascript (handoffHopScript (flow_.base, flow_.handoffPath));
        return;
    }
    // Any other finish after the hop (/go itself, an intermediate redirect):
    // recorded above, not judged. The entry-armed budget owns the case where
    // the target never lands.
}

void DashboardWeb::pollMarker()
{
    if (finished_ || web_ == nullptr) return;
    static const char* kMarker =
        "(function(){try{return document.querySelector('.dash-main')?'dash':'no';}"
        "catch(e){return 'no';}})()";
    auto safe = juce::Component::SafePointer<DashboardWeb> (this);
    const int gen = handoffGen_;
    web_->evaluateJavascript (kMarker,
        [safe, gen] (juce::WebBrowserComponent::EvaluationResult r)
        {
            if (safe == nullptr || safe->finished_ || gen != safe->handoffGen_) return;
            const bool dash = r.getResult() != nullptr && r.getResult()->toString() == "dash";
            if (dash) { safe->advanceWith (DashboardWebFlow::Outcome::Dashboard); return; }
            if (++safe->markerPolls_ >= kMarkerPollMax)
            {
                // The target landed (redeem WORKED) and the marker never
                // rendered. A re-mint re-runs the same race and burns a
                // token — it is the wrong lever for this failure, so this
                // reason is terminal at the editor.
                safe->finishDirect (false, "no_marker");
                return;
            }
            juce::Timer::callAfterDelay (kMarkerPollMs,
                [safe, gen] { if (safe != nullptr && gen == safe->handoffGen_) safe->pollMarker(); });
        });
}

void DashboardWeb::finishDirect (bool ok, const juce::String& reason)
{
    if (finished_) return;
    flow_.step = ok ? DashboardWebFlow::Step::Done : DashboardWebFlow::Step::Failed;
    finished_ = true;
    ejDashLog (juce::String ("[dashweb] result=") + (ok ? "loaded" : "failed")
               + " reason=" + reason);
    if (onLoadResult) onLoadResult (ok, reason);
}

void DashboardWeb::loadCurrent()
{
    const auto url = flow_.currentUrl();
    if (url.isEmpty() || web_ == nullptr) return;

    // Log the step; the v2preview token value is redacted, never logged —
    // and so is the handoff token in the /go fragment (single-use, but a
    // log line outlives its 120 s TTL and the rule is no tokens in logs).
    juce::String safeUrl = url;
    const int vp = safeUrl.indexOf ("v2preview=");
    if (vp >= 0) safeUrl = safeUrl.substring (0, vp) + "v2preview=<redacted>";
    const int ht = safeUrl.indexOf ("#t=");
    if (ht >= 0) safeUrl = safeUrl.substring (0, ht) + "#t=<redacted>";
    ejDashLog (juce::String ("[dashweb] load ")
               + DashboardWebFlow::stepName (flow_.step) + " " + safeUrl);

    web_->goToURL (url);
}

void DashboardWeb::probe (int attemptsLeft)
{
    if (finished_ || web_ == nullptr) return;

    // Detect the embed dashboard vs the gate 404 by OUR OWN marker check (not a
    // page-supplied string, so §8's no-eval-of-page-strings rule is intact): the
    // dashboard shell always carries .dash-main (present in embed mode too), the
    // gate 404 is a 9-byte "Not Found" with nothing. One delayed re-check
    // absorbs SPA hydration before declaring NotDashboard.
    static const char* kProbe =
        "(function(){try{return document.querySelector('.dash-main')?'dash':'no';}"
        "catch(e){return 'no';}})()";

    auto safe = juce::Component::SafePointer<DashboardWeb> (this);
    web_->evaluateJavascript (kProbe,
        [safe, attemptsLeft] (juce::WebBrowserComponent::EvaluationResult r)
        {
            if (safe == nullptr) return;
            const bool dash = r.getResult() != nullptr && r.getResult()->toString() == "dash";
            if (dash) { safe->advanceWith (DashboardWebFlow::Outcome::Dashboard); return; }
            if (attemptsLeft > 0)
            {
                juce::Timer::callAfterDelay (400,
                    [safe, attemptsLeft] { if (safe) safe->probe (attemptsLeft - 1); });
                return;
            }
            safe->advanceWith (DashboardWebFlow::Outcome::NotDashboard);
        });
}

void DashboardWeb::advanceWith (DashboardWebFlow::Outcome o)
{
    if (finished_) return;
    lastOutcome_ = o;
    flow_.advance (o);
    if (flow_.isTerminal()) { finish (flow_.succeeded()); return; }
    loadCurrent();
}

void DashboardWeb::finish (bool ok)
{
    if (finished_) return;
    finished_ = true;
    // The reason the editor branches on (contract §5): "net" never re-mints,
    // "not_dashboard" on the handoff path is the redeem-failed class and
    // earns exactly one re-mint.
    const juce::String reason = ok ? "dashboard"
        : (lastOutcome_ == DashboardWebFlow::Outcome::NetworkError ? "net" : "not_dashboard");
    ejDashLog (juce::String ("[dashweb] result=") + (ok ? "loaded" : "failed")
               + " reason=" + reason);
    if (onLoadResult) onLoadResult (ok, reason);

    // TODO (handoff auto-signin, a later slice): if the page shows its login
    // form, mint a one-time handoff and hand the token to the page so the user
    // is signed in automatically — see EchoJayAPI.h:890 (mintHandoff). This
    // stage accepts the page's own session: the persistent WKWebsiteDataStore
    // holds it (measured to survive editor recreation), so the user signs in
    // once per machine.
}

void DashboardWeb::handleLoadChain (const juce::Array<juce::var>& args,
                                    std::function<void (juce::var)> completion)
{
    // {accepted, reason?} — the acknowledgement shape the page awaits.
    auto reply = [] (bool accepted, const juce::String& reason)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("accepted", accepted);
        if (! accepted) o->setProperty ("reason", reason);
        return juce::var (o.release());
    };

    // VALIDATE HARD, natively, before anything else. The first JS argument is
    // the payload object; extra arguments are ignored.
    LoadChainRequest req;
    const juce::var payload = args.isEmpty() ? juce::var() : args.getReference (0);
    const auto reason = validateLoadChain (payload, req);
    if (reason.isNotEmpty()) { completion (reply (false, reason)); return; }

    if (onLoadChain == nullptr) { completion (reply (false, "bad_payload")); return; }

    // Forward the clean request; the editor routes it through openSavedChain and
    // answers. The answer is the acknowledgement, not the dial report (see the
    // onLoadChain doc). completion may be called from any thread per JUCE; the
    // editor answers synchronously on the message thread.
    onLoadChain (req.chainId, req.slug,
        [completion, reply] (bool accepted, juce::String r)
        {
            completion (reply (accepted, r));
        });
}

void DashboardWeb::handleOpenChat (const juce::Array<juce::var>& args,
                                   std::function<void (juce::var)> completion)
{
    auto reply = [] (bool accepted, const juce::String& reason)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("accepted", accepted);
        if (! accepted) o->setProperty ("reason", reason);
        return juce::var (o.release());
    };

    juce::String chatId;
    const juce::var payload = args.isEmpty() ? juce::var() : args.getReference (0);
    const auto reason = validateOpenChat (payload, chatId);
    if (reason.isNotEmpty()) { completion (reply (false, reason)); return; }

    if (onOpenChat == nullptr) { completion (reply (false, "bad_payload")); return; }

    onOpenChat (chatId,
        [completion, reply] (bool accepted, juce::String r)
        {
            completion (reply (accepted, r));
        });
}

} // namespace echojay
