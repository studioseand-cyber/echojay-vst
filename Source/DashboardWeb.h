#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>

namespace echojay
{

// ---------------------------------------------------------------------------
// DashboardWebFlow — the URL builder and gate-fallback state machine.
//
// PURE and testable: no webview, no I/O. tools/dashweb_test drives it directly.
// The whole point of it being a separate struct is that the two rules that
// matter — "the webview host is the SAME base the API uses" and "the seed URL
// NEVER carries embed=plugin" — are assertable without a live page.
// ---------------------------------------------------------------------------
struct DashboardWebFlow
{
    // LoadHandoff is LAST on purpose: dashweb_test compiles this header and
    // links the previously built lib's advance(), and inserting an
    // enumerator mid-list shifts Done/Failed under that pairing (the
    // stale-lib trap, third sighting). Appending keeps every existing value.
    enum class Step    { LoadEmbed, Seed, LoadEmbedAfterSeed, Done, Failed, LoadHandoff };
    enum class Outcome { Dashboard, NotDashboard, NetworkError };

    juce::String base;                  // scheme+host, NO trailing slash, NO path
    juce::String token;                 // v2preview token; empty => no seed possible
    bool         gateEnabled = false;   // dev-transport builds only

    // The embed URL is the target. The seed URL NEVER carries embed=plugin: the
    // preview gate's 307 strips the whole query, so a seed with embed on it
    // would land on the FULL page (hero+sidebar+animated visualiser, the
    // 326 MB cost the whole design avoids). Seed sets the cookie; then we load
    // the embed URL directly.
    juce::String embedUrl() const { return base + "/dashboard?embed=plugin"; }
    juce::String seedUrl()  const { return base + "/dashboard?v2preview="
                                         + juce::URL::addEscapeChars (token, true); }

    Step step = Step::LoadEmbed;
    // Auth handoff (CONTRACT_dashboard_auth_handoff.md §2): the minted
    // /go#t=...&to=... path. The fragment carries a single-use 120 s token;
    // it is navigated once (via the hop script below, NEVER via goToURL) and
    // never stored anywhere else.
    juce::String handoffPath;

    // First navigation of the handoff — a fragment-free path on the same
    // host (see the hop comment below this struct for why a hop exists).
    // Deliberately UNROUTED: vercel.json has no catch-all rewrite, so this
    // serves the platform's small 404 page. That is fine ON PURPOSE — the
    // hop page's content is irrelevant; it is only a browsing context to
    // run location.replace from, and a 404 is the cheapest document the
    // host can produce.
    static constexpr const char* handoffHopPath = "/go-hop";

    /** URL to load to ENTER the current step ("" at terminal steps). */
    juce::String currentUrl() const;

    /** Advance given the outcome of the load that just finished. Returns the
        next URL to load ("" => terminal; inspect step for Done/Failed).
        Transitions:
          LoadEmbed          + Dashboard    -> Done
          LoadEmbed          + NotDashboard -> Seed (if gate+token) else Failed
          Seed               + any load     -> LoadEmbedAfterSeed (cookie is set)
          LoadEmbedAfterSeed + Dashboard    -> Done
          LoadEmbedAfterSeed + NotDashboard -> Failed   (second 404 = fail)
          any                + NetworkError -> Failed */
    juce::String advance (Outcome o);

    bool isTerminal() const { return step == Step::Done || step == Step::Failed; }
    bool succeeded()  const { return step == Step::Done; }

    static const char* stepName (Step s);
};

// ---------------------------------------------------------------------------
// Handoff hop + finish-log redaction — pure, HEADER-INLINE so that
// tools/dashweb_test compiles them directly and never pairs a new lib symbol
// with the previous build's lib (the stale-lib trap, third sighting).
//
// WHY A HOP EXISTS AT ALL: juce_WebBrowserComponent_mac.mm:64 percent-encodes
// EVERY goToURL string with URLQueryAllowedCharacterSet, a set that excludes
// '#' and '%'. A /go#t=... URL therefore reaches WebKit as /go%23t=... — the
// fragment destroyed, the token moved into the PATH, and go.html's script
// running with an empty location.hash (measured live 21 Aug 2026,
// dash-poll.log 14:49:39). So the fragment must never pass through goToURL:
// navigate first to a cheap fragment-free page on the same host, then issue
// the real navigation as JS — location.replace keeps the fragment intact and
// client-side (a fragment is never transmitted to a server, which is the
// contract's whole reason for using one).
//
// The hop must NOT be /go itself: replacing /go with /go#t=... is a
// same-document fragment change — nothing reloads, and go.html reads
// location.hash exactly once, at parse time, with no hashchange listener
// (verified in public/go.html, 21 Aug 2026). Any OTHER same-host document
// works, whatever its content.
//
// THE REAL FIX IS IN JUCE — escape only when URLWithString fails, instead of
// unconditionally. That is a fork of a module we do not own: a decision, not
// a repair. Until it is made, this workaround exists BECAUSE of mm:64, not
// because anybody liked two navigations.
// ---------------------------------------------------------------------------

/** The JS for the handoff's second navigation: an absolute location.replace
    to base+goPath with the fragment intact. Single quotes and backslashes in
    the URL are escaped so composed URL content cannot break out of the
    script literal. PURE — tools/dashweb_test drives it. */
inline juce::String handoffHopScript (const juce::String& base, const juce::String& goPath)
{
    const auto url = (base + goPath).replace ("\\", "\\\\").replace ("'", "\\'");
    return "location.replace('" + url + "');";
}

/** Redact a load-finish URL for logging. Cuts at the FIRST of a raw '#' or a
    percent-encoded '%23': the first redactor keyed on a literal '#' alone,
    and when JUCE's goToURL escaping moved the fragment into the path as
    %23t=..., two raw tokens reached dash-poll.log (21 Aug 2026 — single-use
    and expired, the design's own backstop, but the rule is no tokens in
    logs). Everything from the cut onward is replaced by its length. PURE —
    tools/dashweb_test drives it, with the live leak's exact shape as a
    fixture. */
inline juce::String redactedFinishUrl (const juce::String& url)
{
    int cut = url.indexOfChar ('#');
    const int enc = url.indexOfIgnoreCase ("%23");
    if (enc >= 0 && (cut < 0 || enc < cut)) cut = enc;
    if (cut < 0) return url + " no-fragment";
    return url.substring (0, cut) + " frag=<redacted,"
         + juce::String (url.length() - cut) + "ch>";
}

// ---------------------------------------------------------------------------
// Stage 3 bridge: the loadChain payload validator.
// ---------------------------------------------------------------------------
/** A validated loadChain request: exactly ONE of chainId / slug is non-empty. */
struct LoadChainRequest { juce::String chainId, slug; };

/** Validate a loadChain payload (the JS call's first argument). Returns an empty
    string on success (and fills `req`), or the wire `reason` on failure. PURE —
    no webview, no I/O; tools/dashweb_test drives it directly. The page is our own
    page and it is still a page (§8), so this runs HARD before anything else.
    Rules: an object with EXACTLY one of chainId / slug present; that one a
    non-empty string; chainId <= 64 chars, slug <= 32; [A-Za-z0-9_-] only. Any
    violation returns "bad_payload". */
juce::String validateLoadChain (const juce::var& payload, LoadChainRequest& req);

/** Validate an openChat payload. Returns an empty string on success (and fills
    `chatId`), or "bad_payload". An EMPTY payload — {} or {chatId:""} — is valid
    and means "open the Chat tab with nothing selected" (fills an empty chatId).
    A PRESENT, non-empty chatId must be a string, <= 64 chars, [A-Za-z0-9_-]
    only (same as loadChain's ids); a non-string chatId (null, number) is
    rejected. Pure; tools/dashweb_test drives it. */
juce::String validateOpenChat (const juce::var& payload, juce::String& chatId);

/** The loadChain busy test — PURE, so tools/dashweb_test drives it. A request is
    busy if a modal is up (a confirm is showing) OR the last accepted request was
    within `windowMs`. Two tests, deliberately, because a single boolean leaks:
    a user cancelling openSavedChain's confirm, and openSavedChain's own internal
    fetch failing, both leave nothing to clear a flag. The modal test covers "a
    confirm is showing" exactly and indefinitely — and it matters here because
    the webview is a native NSView that JUCE modality may NOT block, so a click
    can reach the bridge past the dialog. The timestamp covers the async
    import/fetch window before the confirm and self-heals both leak paths.
    `elapsedMs` is millis since the last accepted request (a value >= windowMs
    means "none / cleared"). */
bool loadChainBusy (int numModalComponents, juce::int64 elapsedMs,
                    juce::int64 windowMs = 8000);

/** The chainId an importShare response carries — PURE, so tools/dashweb_test
    drives it. Works for a real import AND for importing YOUR OWN share, which
    answers { imported:false, reason:'own_share', chainId } and is SUCCESS: the
    id is exactly what the caller wants next (§5a). Empty if the response has no
    chainId (a genuine error). A non-empty result is the success signal. */
juce::String importedChainId (const juce::var& importResponse);

// ---------------------------------------------------------------------------
// DashboardWeb — owns ONE juce::WebBrowserComponent and its load lifecycle.
//
// The editor talks to this class only; JUCE webview quirks stay inside it. The
// LIFECYCLE — construct on Dashboard-tab select, DESTROY on tab-away — is the
// editor's job (see the Dashboard blocks in PluginEditor.cpp), because destroy
// (not setVisible(false)) is what frees the ~103 MB resident WebKit processes.
// The webview is held by PIMPL so this header never names WebBrowserComponent,
// which keeps it includable from a JUCE_WEB_BROWSER=0 test TU.
// ---------------------------------------------------------------------------
class DashboardWeb : public juce::Component
{
public:
    DashboardWeb();
    ~DashboardWeb() override;

    /** Fired once per start()/startWithHandoff(): ok=true when the dashboard
        loaded. On failure `reason` distinguishes the two classes the editor
        must treat differently (contract §5): "net" (network error - offline
        line, no retry) vs "not_dashboard" (landed but the probe found no
        dashboard - on the handoff path this is the redeem-failed class and
        the editor re-mints ONCE). ok=true carries reason "dashboard". */
    std::function<void (bool ok, const juce::String& reason)> onLoadResult;

    /** Stage 3 bridge: the editor's loadChain handler. DashboardWeb validates the
        page's payload natively (validateLoadChain) and forwards a CLEAN request
        (exactly one of chainId / slug non-empty); the editor routes it (share
        import / direct) through the ONE openSavedChain loader and calls `answer`.
        `answer` is an ACKNOWLEDGEMENT — {accepted, reason} — NOT per-slot dial
        results: on success the plugin switches to the Chain tab, which destroys
        this webview (lazy lifecycle), and the Chain tab shows the per-slot notes
        natively. Answer true the moment the request is validated and handed off. */
    using Answer = std::function<void (bool accepted, juce::String reason)>;
    std::function<void (const juce::String& chainId, const juce::String& slug, Answer)> onLoadChain;

    /** Stage 3 item 3: the editor's openChat handler. DashboardWeb validates the
        payload natively (validateOpenChat) and forwards a clean chatId; the editor
        routes it to the Chat tab (the followDashLink chat path). Same
        acknowledgement + busy semantics as loadChain. */
    std::function<void (const juce::String& chatId, Answer)> onOpenChat;

    /** Begin the load flow. Call once, after the component is added and shown. */
    void start();

    /** Auth handoff entry (contract §2, the ONLY path that signs the webview
        in): two navigations — the fragment-free hop page via goToURL, then
        the minted /go#... path via handoffHopScript (JS location.replace),
        because goToURL destroys fragments (see the hop comment above);
        go.html redeems, writes ej-token to localStorage, and lands on the
        bound path. Callable again on the SAME live webview for the editor's
        one re-mint (§5, redeem fails). The token in the fragment is never
        logged (redactedFinishUrl covers raw AND %23-encoded forms) and never
        persisted plugin-side. The redeem budget is armed HERE, at entry —
        never on observing a particular path. */
    void startWithHandoff (const juce::String& goPath);

    void resized() override;

private:
    struct Inner;                       // the WebBrowserComponent, defined in .cpp
    std::unique_ptr<Inner> web_;
    DashboardWebFlow       flow_;
    bool                   finished_ = false;
    DashboardWebFlow::Outcome lastOutcome_ = DashboardWebFlow::Outcome::NetworkError; // feeds finish()'s reason

    void loadCurrent();
    void probe (int attemptsLeft);      // detect embed dashboard vs gate 404
    void advanceWith (DashboardWebFlow::Outcome o);
    void finish (bool ok);
    // Handoff verdict machinery (21 Aug 2026, after the 14:03 mislabel):
    // the embed probe judged the handoff from /go's own load-finish — a
    // two-navigation sequence scored on its first page, so "redeem was
    // slow" and "redeem failed" were one observation, four tokens burned
    // proving nothing. handleFinish records EVERY load-finish with its
    // path, anchors the verdict to the TARGET's load-finish, and splits
    // the two failures: no target landing inside the redeem budget
    // (reason "no_landing", the only one worth a re-mint) vs target
    // reached but the marker never renders ("no_marker", never re-mint).
    void handleFinish (const juce::String& url);
    void pollMarker();
    void finishDirect (bool ok, const juce::String& reason);
    juce::String handoffTargetPath_;      // path of the bound target ("/dashboard")
    bool         handoffTargetReached_ = false;
    bool         handoffHopDone_       = false;  // hop finished; replace issued
    int          markerPolls_ = 0;
    int          handoffGen_  = 0;        // invalidates timers across re-mints

    // The registered `loadChain` native function: validates args, forwards to
    // onLoadChain, and resolves the JS promise with {accepted, reason}. The
    // completion is juce::WebBrowserComponent::NativeFunctionCompletion, kept as
    // the raw std::function type so this header need not name WebBrowserComponent.
    void handleLoadChain (const juce::Array<juce::var>& args,
                          std::function<void (juce::var)> completion);
    void handleOpenChat  (const juce::Array<juce::var>& args,
                          std::function<void (juce::var)> completion);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DashboardWeb)
};

} // namespace echojay
