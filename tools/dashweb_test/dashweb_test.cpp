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

// Build a one- or two-key JS-payload object (a juce::var DynamicObject).
juce::var one (const char* k, juce::var v)
{
    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty (k, std::move (v));
    return juce::var (o.release());
}
juce::var two (const char* k1, juce::var v1, const char* k2, juce::var v2)
{
    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty (k1, std::move (v1));
    o->setProperty (k2, std::move (v2));
    return juce::var (o.release());
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

    // ---- loadChain payload validation ---------------------------------------
    {
        using echojay::validateLoadChain;
        using echojay::LoadChainRequest;

        LoadChainRequest a;
        check (validateLoadChain (one ("chainId", "abc-1_XY"), a).isEmpty()
               && a.chainId == "abc-1_XY" && a.slug.isEmpty(),
               "accepts { chainId } and fills it");
        LoadChainRequest b;
        check (validateLoadChain (one ("slug", "shareSlug_9"), b).isEmpty()
               && b.slug == "shareSlug_9" && b.chainId.isEmpty(),
               "accepts { slug } and fills it");

        auto bad = [&] (juce::var p, const char* what)
        {
            LoadChainRequest r;
            check (validateLoadChain (p, r) == "bad_payload", what);
        };
        bad (two ("chainId", "a", "slug", "b"),                   "rejects BOTH keys present");
        bad (juce::var (new juce::DynamicObject()),              "rejects NEITHER key");
        bad (one ("chainId", ""),                                "rejects empty chainId");
        bad (one ("slug", ""),                                    "rejects empty slug");
        bad (one ("chainId", juce::String::repeatedString ("a", 65)), "rejects over-length chainId (>64)");
        bad (one ("slug",    juce::String::repeatedString ("a", 33)), "rejects over-length slug (>32)");
        bad (one ("chainId", "has space"),                       "rejects an illegal character");
        bad (one ("chainId", juce::var (123)),                   "rejects wrong type (number)");
        bad (one ("slug",    juce::var (true)),                  "rejects wrong type (bool)");
        bad (juce::var ("a bare string"),                        "rejects a non-object payload");
        bad (juce::var(),                                         "rejects a void payload");
        std::printf ("  ok    payload validation: two legal shapes, garbage rejected\n");
    }

    // ---- the busy guard: modal OR within-window, self-healing ---------------
    {
        using echojay::loadChainBusy;
        check (loadChainBusy (0, 1000, 8000) == true,  "busy within the window");
        check (loadChainBusy (0, 9000, 8000) == false, "not busy after the window expires");
        check (loadChainBusy (1, 9000, 8000) == true,  "busy while a confirm modal is up (window irrelevant)");
        check (loadChainBusy (2, 9000, 8000) == true,  "busy with multiple modals");

        // cancel-then-reclick: while confirm A is up -> busy (modal); just after
        // cancel but within the window -> still busy (self-heals, not permanent);
        // after the window with no modal -> NOT busy, so the reclick resolves to
        // a SECOND confirm rather than the old boolean's permanent silence.
        check (loadChainBusy (1, 500,  8000) == true,  "cancel-reclick: confirm A up -> busy");
        check (loadChainBusy (0, 500,  8000) == true,  "cancel-reclick: just cancelled, within window -> busy (self-heals)");
        check (loadChainBusy (0, 9000, 8000) == false, "cancel-reclick: after window -> a second confirm, not silence");
        std::printf ("  ok    busy = modal OR within-window; never permanent silence\n");
    }

    // ---- importedChainId: own_share resolves to success ---------------------
    {
        using echojay::importedChainId;

        auto ownShare = [] {
            auto o = std::make_unique<juce::DynamicObject>();
            o->setProperty ("imported", false);
            o->setProperty ("reason",   "own_share");
            o->setProperty ("chainId",  "own1");
            return juce::var (o.release());
        }();
        check (importedChainId (ownShare) == "own1",
               "own_share {imported:false, reason:'own_share', chainId} -> chainId (SUCCESS)");
        check (importedChainId (one ("chainId", "imp1")) == "imp1",
               "real import { chainId } -> chainId");
        check (importedChainId (one ("error", "not_found")).isEmpty(),
               "error response (no chainId) -> empty");
        check (importedChainId (juce::var()).isEmpty(),
               "void response -> empty");
        // A non-empty chainId is the bridge's success signal (§5a): both own_share
        // and a real import yield the id to load next.
        check (importedChainId (ownShare).isNotEmpty(),
               "non-empty chainId is the success signal");
        std::printf ("  ok    importedChainId: own_share is success, error is empty\n");
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
