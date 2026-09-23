// sync_live_probe (23 Sep 2026) — 21q item 1, the LIVE check against B's /api/params/sync.
//
// Two legs on the SAME body, printed side by side:
//   RAW    — the identical POST (same endpoint, same bearer, same JSON) read back as TEXT, so the server's own
//            field names, tiers and fps are visible exactly as they arrive.
//   CLIENT — EchoJayAPI::syncParamIdentities, the shipping code path, printing what IT parsed into the store.
// A difference between the two is a contract mismatch and is the whole point of running this.
//
// ISOLATION: a private ECHOJAY_STATE_HOME holding a COPY of the real state, so the real scan and the real session
// are used and nothing live is written (the smoke_body pattern). The bearer token is used, never printed.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "EchoJayParamMaps.h"
#include <cstdio>
using namespace juce;

namespace {
const char* kWanted[] = { "Auto-Tune Pro", "UAD Pultec HLF-3C", "Bass Rider (m)" };

void pumpFor (double ms)
{
    const double t0 = Time::getMillisecondCounterHiRes();
    while (Time::getMillisecondCounterHiRes() - t0 < ms)
    {
        Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    }
}
}

int main()
{
    ScopedJuceInitialiser_GUI gui;
    File tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("ej_synclive_" + String (Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    File ("/Users/SeanD/Library/Application Support/EchoJay").copyDirectoryTo (tmp.getChildFile ("Library/Application Support/EchoJay"));
    File ("/Users/SeanD/Library/EchoJay").copyDirectoryTo (tmp.getChildFile ("Library/EchoJay"));
    std::printf ("isolated state root (a copy of the real one): %s\n", tmp.getFullPathName().toRawUTF8());

    EchoJayProcessor proc;
    auto& ch = proc.getChainHost();
    ch.loadFromDisk();
    pumpFor (2000);

    // ---- the three real identities, off this Mac's own scan ------------------------------------------------
    std::vector<echojay::SyncRef> refs;
    std::vector<PluginDescription>    picked;
    // Straight off this Mac's own scan cache (the COPY in the isolated root), read exactly as ChainHost reads it.
    // A row with uniqueId 0 is a thin VST3 row - no identity, and the client never sends one.
    {
        auto doc = XmlDocument::parse (tmp.getChildFile ("Library/EchoJay/chain_entries.xml"));
        if (doc == nullptr) { std::printf ("no chain_entries.xml in the copied root\n"); return 2; }
        for (auto* want : kWanted)
        {
            bool found = false;
            for (auto* c : doc->getChildIterator())
            {
                PluginDescription d;
                if (! d.loadFromXml (*c)) continue;
                if (d.uniqueId == 0) continue;
                if (! d.name.equalsIgnoreCase (want)) continue;
                refs.push_back (echojay::syncRefForDescription (d));
                picked.push_back (d);
                found = true;
                break;
            }
            if (! found) std::printf ("  NOT IN THE SCAN: %s\n", want);
        }
    }
    if (refs.empty()) { std::printf ("no identities resolved - nothing to ask\n"); return 2; }

    std::printf ("\n== the three identities, as the client would send them ==\n");
    for (size_t i = 0; i < refs.size(); ++i)
        std::printf ("  %-22s ik=%-30s name=%-20s manu=%-17s format=%-10s version=%s\n",
                     picked[i].name.toRawUTF8(), refs[i].ik.toRawUTF8(), refs[i].name.toRawUTF8(),
                     refs[i].manufacturer.toRawUTF8(), refs[i].format.toRawUTF8(), refs[i].version.toRawUTF8());

    // ---- the body, built exactly as syncParamIdentities builds it ------------------------------------------
    Array<var> arr;
    for (auto& r : refs)
    {
        auto* o = new DynamicObject();
        o->setProperty ("name", r.name);
        o->setProperty ("manufacturer", r.manufacturer);
        o->setProperty ("format", r.format);
        o->setProperty ("version", r.version);
        arr.add (var (o));
    }
    auto* root = new DynamicObject();
    root->setProperty ("plugins", arr);
    const auto body = JSON::toString (var (root), true);
    std::printf ("\n== REQUEST BODY ==\n%s\n", body.toRawUTF8());

    // ---- RAW LEG ------------------------------------------------------------------------------------------
    {
        const auto auth = JSON::parse (File (tmp.getFullPathName() + "/Library/Application Support/EchoJay/auth.json").loadFileAsString());
        String endpoint = auth.getProperty ("endpoint", var()).toString();
        if (endpoint.isEmpty()) endpoint = "https://www.echojay.ai";
        const String token = auth.getProperty ("token", var()).toString();   // used, never printed
        std::printf ("\n== RAW LEG: POST %s/api/params/sync (bearer present: %s) ==\n",
                     endpoint.toRawUTF8(), token.isNotEmpty() ? "yes" : "NO - the call will 401");

        URL url (endpoint + "/api/params/sync");
        url = url.withPOSTData (body);
        String headers = "Content-Type: application/json\r\n";
        if (token.isNotEmpty()) headers += "Authorization: Bearer " + token + "\r\n";
        int sc = 0;
        auto stream = url.createInputStream (URL::InputStreamOptions (URL::ParameterHandling::inPostData)
                                                 .withExtraHeaders (headers)
                                                 .withConnectionTimeoutMs (60000)
                                                 .withStatusCode (&sc));
        const String raw = stream != nullptr ? stream->readEntireStreamAsString() : String ("<no stream>");
        std::printf ("status %d\nraw body:\n%s\n", sc, raw.toRawUTF8());
    }

    // ---- CLIENT LEG ---------------------------------------------------------------------------------------
    std::printf ("\n== CLIENT LEG: EchoJayAPI::syncParamIdentities (the shipping path) ==\n");
    std::printf ("logged in: %s\n", proc.getApi().isLoggedIn() ? "yes" : "no");
    bool done = false, okOut = false;
    std::map<String, echojay::SyncedIdentity> rowsOut;
    proc.getApi().syncParamIdentities (refs, [&] (bool ok, std::map<String, echojay::SyncedIdentity> rows)
    {
        okOut = ok; rowsOut = std::move (rows); done = true;
    });
    const double t0 = Time::getMillisecondCounterHiRes();
    while (! done && Time::getMillisecondCounterHiRes() - t0 < 60000.0) pumpFor (100);
    std::printf ("ok=%s, %d row(s) parsed after %.0f ms\n", okOut ? "true" : "false",
                 (int) rowsOut.size(), Time::getMillisecondCounterHiRes() - t0);
    for (auto& kv : rowsOut)
        std::printf ("  ik=%-34s fp=%-40s version=%-10s tier=%s\n",
                     kv.first.toRawUTF8(), kv.second.fp.toRawUTF8(),
                     kv.second.version.toRawUTF8(), kv.second.tier.toRawUTF8());

    // ---- what it does to mapFps, on the real feed -----------------------------------------------------------
    if (okOut && ! rowsOut.empty())
    {
        ch.buildRecommendable ({}, {});   // no rebuild: just report what the store now holds
        ch.applySyncedIdentities (rowsOut);
        std::printf ("\nstore after apply: %d identit(ies)\n", ch.syncedIdentityCount());
    }
    std::printf ("\n==== sync_live_probe done ====\n");
    return 0;
}
