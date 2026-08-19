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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DashboardWeb)
};

} // namespace echojay
