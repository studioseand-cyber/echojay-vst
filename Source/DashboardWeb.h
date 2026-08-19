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
    enum class Step    { LoadEmbed, Seed, LoadEmbedAfterSeed, Done, Failed };
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

    /** Fired once per start(): true when the embed dashboard loaded, false on a
        gate 404 (after any seed attempt) or a network error. The editor keeps
        the native view up until true, and on false stays native and does not
        retry until the next tab selection. */
    std::function<void (bool ok)> onLoadResult;

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

    /** Begin the load flow. Call once, after the component is added and shown. */
    void start();

    void resized() override;

private:
    struct Inner;                       // the WebBrowserComponent, defined in .cpp
    std::unique_ptr<Inner> web_;
    DashboardWebFlow       flow_;
    bool                   finished_ = false;

    void loadCurrent();
    void probe (int attemptsLeft);      // detect embed dashboard vs gate 404
    void advanceWith (DashboardWebFlow::Outcome o);
    void finish (bool ok);

    // The registered `loadChain` native function: validates args, forwards to
    // onLoadChain, and resolves the JS promise with {accepted, reason}. The
    // completion is juce::WebBrowserComponent::NativeFunctionCompletion, kept as
    // the raw std::function type so this header need not name WebBrowserComponent.
    void handleLoadChain (const juce::Array<juce::var>& args,
                          std::function<void (juce::var)> completion);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DashboardWeb)
};

} // namespace echojay
