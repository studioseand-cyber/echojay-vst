/*
    DashboardWebFlow self-test (stage 2, Part D).

    Drives the PURE gate-fallback state machine and URL builder over the SHIPPING
    struct — no webview, no live page. echojay::DashboardWebFlow is public, so
    this constructs and drives it directly (no friend needed); the .cpp under
    test compiles its advance()/currentUrl() into the SharedCode lib this links.

    What it holds to:
      - URL building from a fake base: /dashboard?embed=plugin
      - the SEED url NEVER carries embed=plugin (the gate 307 strips the query,
        which would land the seed on the full page — the whole trap this design
        exists to avoid)
      - the gate-fallback machine: 404 -> seed -> embed -> Done, and a SECOND
        404 = fail; no token / gate-off never seeds; a network error fails
      - a negative control that MUST fail (a suite passing with zero failures
        has proven nothing)
*/

#include <JuceHeader.h>
#include "DashboardWeb.h"

#include <cstdio>

namespace
{
using Flow = echojay::DashboardWebFlow;
using Step = Flow::Step;
using Out  = Flow::Outcome;

int failures = 0;

void check (bool ok, const char* what)
{
    if (ok) return;
    ++failures;
    std::printf ("  FAIL  %s\n", what);
}

Flow make (const juce::String& base, const juce::String& token, bool gate)
{
    Flow f;
    f.base        = base;
    f.token       = token;
    f.gateEnabled = gate;
    return f;
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("DashboardWebFlow self-test\n");
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::String base = "https://echojay-dash-preview.vercel.app";
    const juce::String tok  = "TOK EN/&?=x";   // spaces + reserved chars, to prove escaping

    // ---- URL building + the never-seed-with-embed rule ----------------------
    {
        auto f = make (base, tok, true);
        check (f.embedUrl() == base + "/dashboard?embed=plugin",
               "embed URL = base + /dashboard?embed=plugin");
        check (f.seedUrl().startsWith (base + "/dashboard?v2preview="),
               "seed URL = base + /dashboard?v2preview=");
        check (! f.seedUrl().contains ("embed=plugin"),
               "seed URL NEVER carries embed=plugin");
        check (! f.seedUrl().contains (" "),
               "seed token is URL-escaped (no raw spaces)");
        std::printf ("  ok    URL building + never-seed-with-embed\n");
    }

    // ---- embed loads first try -> Done --------------------------------------
    {
        auto f = make (base, tok, true);
        check (f.step == Step::LoadEmbed,      "starts at LoadEmbed");
        check (f.currentUrl() == f.embedUrl(), "LoadEmbed loads the embed URL");
        const auto next = f.advance (Out::Dashboard);
        check (f.step == Step::Done && f.succeeded() && next.isEmpty(),
               "embed loads -> Done");
        std::printf ("  ok    embed loads first try -> Done\n");
    }

    // ---- 404 -> seed -> embed -> Done ---------------------------------------
    {
        auto f = make (base, tok, true);
        const auto n1 = f.advance (Out::NotDashboard);     // gate 404
        check (f.step == Step::Seed && n1 == f.seedUrl(),
               "gate 404 -> Seed (loads the seed URL)");
        const auto n2 = f.advance (Out::Dashboard);        // seed page (content irrelevant)
        check (f.step == Step::LoadEmbedAfterSeed && n2 == f.embedUrl(),
               "seed done -> load embed (cookie now set)");
        const auto n3 = f.advance (Out::Dashboard);        // embed loads
        check (f.step == Step::Done && f.succeeded() && n3.isEmpty(),
               "embed after seed -> Done");
        std::printf ("  ok    404 -> seed -> embed -> Done\n");
    }

    // ---- 404 -> seed -> SECOND 404 = fail -----------------------------------
    {
        auto f = make (base, tok, true);
        f.advance (Out::NotDashboard);                     // -> Seed
        f.advance (Out::NotDashboard);                     // seed page -> LoadEmbedAfterSeed
        const auto n = f.advance (Out::NotDashboard);       // embed STILL 404
        check (f.step == Step::Failed && ! f.succeeded() && n.isEmpty(),
               "second 404 -> Failed");
        std::printf ("  ok    404 -> seed -> second 404 = Failed\n");
    }

    // ---- cannot seed: no token, or gate disabled (production) -> Failed ------
    {
        auto noTok = make (base, {}, true);                // gate on, no token
        noTok.advance (Out::NotDashboard);
        check (noTok.step == Step::Failed,
               "gate 404 + no token -> Failed (never seeds)");

        auto prod = make (base, tok, false);               // gate compiled out
        prod.advance (Out::NotDashboard);
        check (prod.step == Step::Failed,
               "gate 404 + gate disabled -> Failed (no seed path)");
        std::printf ("  ok    no token / gate off -> Failed, never seeds\n");
    }

    // ---- network error at any step -> Failed --------------------------------
    {
        auto f = make (base, tok, true);
        f.advance (Out::NetworkError);
        check (f.step == Step::Failed, "network error at LoadEmbed -> Failed");

        auto g = make (base, tok, true);
        g.advance (Out::NotDashboard);                     // -> Seed
        g.advance (Out::NetworkError);                     // error during seed
        check (g.step == Step::Failed, "network error at Seed -> Failed");
        std::printf ("  ok    network error -> Failed at any step\n");
    }

    // ---- negative control ---------------------------------------------------
    {
        const int before = failures;
        check (1 == 2, "NEGATIVE CONTROL - this line is SUPPOSED to fail");
        const bool caught = (failures == before + 1);
        failures = before;   // the control is not a real defect
        check (caught, "the harness reports a false assertion");
        std::printf (caught ? "  ok    negative control caught\n"
                            : "  FAIL  negative control NOT caught\n");
    }

    if (failures) { std::printf ("dashweb_test: %d FAILED\n", failures); return 1; }
    std::printf ("dashweb_test: PASS\n");
    return 0;
}
