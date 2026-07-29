/*
  tools/ejextract/main.cpp

  EchoJay auto-parameter-mapping harness, two modes in one universal binary:

  WORKER (default):  ejextract <plugin-path> <output-dir>
    Loads ONE plugin, runs the read-only parameter sweep, writes <fp>.json
    to the output dir. One plugin per process: a hang or crash kills only
    this process. Byte-compatible with the original standalone extractor.

  DRIVER:            ejextract --bootstrap
    The opt-in post-install background mapper (launchd LaunchAgent, see
    build-installer.sh). INCREMENTAL across runs and product updates: the
    ledger maps each bundle path to a file-level signature
    (CFBundleShortVersionString | newest Contents/MacOS mtime) plus the
    fingerprints extracted from it. Unchanged signature = reuse the cached
    fingerprints and skip instantiation entirely; only new or changed
    bundles spawn a worker (a plugin update changes the signature, and its
    new fingerprint follows from re-extraction). Each run then reconciles
    fingerprints against the server: fetches maps only for fps not already
    in the local bootstrap cache, contributes samples only for fps that
    are unmapped AND not previously contributed. Net effect: first run is
    the full sweep, every later run (including after an EchoJay update
    re-registers the agent) touches only new or updated plugins.

    Results land in ~/Library/EchoJay/param_maps_bootstrap.json, which
    ChainHost merges read-only; the driver NEVER writes the plugin's own
    param_maps.json, so there is no write race with a running DAW.
    Crash-isolated: a started-marker written BEFORE each spawn makes a
    plugin that kills the driver get skipped (at that signature) on the
    next run instead of crash-looping. Runs at nice(19).

  House style: no em-dashes.
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <sys/resource.h>
#include <sys/stat.h>   // ::chmod for machine_id (0600)

#include "EchoJayParamExtractor.h"
#include "EchoJayAuRegistry.h"   // the one AU walk, shared with ejmap

// Same default the plugin compiles in (Source/EchoJayAPI.cpp); the real
// endpoint is read from auth.json when the user has ever logged in.
static const char* kDefaultEndpoint = "https://www.echojay.ai";
static constexpr int kPerPluginTimeoutMs = 120 * 1000;
// The endpoint accepts 500 fps, but they ride in a GET URL: 500 x 65 chars
// is a ~32KB request line, which fails at the transport before the server
// ever sees it (observed live: status 0 on a 351-fp batch). 100 fps is a
// ~6.5KB URL, safely inside every hop's limit.
static constexpr int kMapsBatch = 100;
// Contribute batches are BYTE-limited: Vercel 413s request bodies past
// ~4.5MB, and one sample (a dense sweep) can run to ~3MB alone (observed
// live). Count is capped too, far under the endpoint's 200.
static constexpr juce::int64 kContributeBatchBytes = 3 * 1024 * 1024;
static constexpr juce::int64 kContributeMaxSampleBytes = 4 * 1024 * 1024;
static constexpr int kContributeBatchCount = 100;

static juce::File echojayDir();
static juce::String machineIdString();

// An instantiation failure that is a LICENSE / AUTH wall, not an enumeration
// or code problem. Kept deliberately broad: the point is to separate "this
// machine is not authorised for this plugin" (SSL Native auth, McDSP APB
// hardware box, an unlicensed Waves variant) from "no plugin types found",
// which the failure triage kept re-deriving because both landed on "failed".
static bool looksLikeLicenseError (const juce::String& err)
{
    const auto s = err.toLowerCase();
    return s.contains ("licen")     || s.contains ("authoriz") || s.contains ("authoris")
        || s.contains ("activat")   || s.contains ("ilok")     || s.contains ("demo")
        || s.contains ("trial")     || s.contains ("not permitted")
        || s.contains ("challenge") || s.contains ("entitle");
}

// ---------------------------------------------------------------------------
// Worker: the original one-plugin-per-process extractor.
//
// Exit codes (read by runIsolatedWorker's classifier - keep in sync):
//   0  ok             a sample was written
//   2  no-types       findAllTypesForFile returned nothing
//   3  no-data        instantiated, but nothing extractable was written
//   4  license-refused every instantiation failed and at least one looked
//                     like an auth/license wall
//   5  load-failed    every instantiation failed for a non-license reason
// argv target may be a file PATH (file-walk) or an AU component IDENTIFIER
// ("AudioUnit:Effects/aufx,STEM,ksWV" from --au-registry): findAllTypesForFile
// resolves both, so this worker needs no change to serve registry entries.
// ---------------------------------------------------------------------------
static int runWorker (const juce::String& pluginPath, const juce::File& outDir)
{
    // Provenance config: machine_id rides INSIDE each sample (v5) so a
    // sample file is attributable on its own, not only via the run ledger.
    echojay::ExtractorConfig cfg;
    cfg.machineId = machineIdString();

    juce::AudioPluginFormatManager formatManager;
#if JUCE_PLUGINHOST_VST3
    formatManager.addFormat (new juce::VST3PluginFormat());
#endif
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    formatManager.addFormat (new juce::AudioUnitPluginFormat());
#endif

    juce::OwnedArray<juce::PluginDescription> descriptions;
    for (int i = 0; i < formatManager.getNumFormats(); ++i)
    {
        juce::OwnedArray<juce::PluginDescription> found;
        formatManager.getFormat (i)->findAllTypesForFile (found, pluginPath);
        descriptions.addCopiesOf (found);
    }
    if (descriptions.isEmpty())
    {
        std::cerr << "No plugin types found at: " << pluginPath << "\n";
        return 2;
    }

    int okCount = 0, instFail = 0, licenseFail = 0;
    for (auto* desc : descriptions)
    {
        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> instance (
            formatManager.createPluginInstance (*desc, 48000.0, 512, error));
        if (instance == nullptr)
        {
            ++instFail;
            if (looksLikeLicenseError (error)) ++licenseFail;
            std::cerr << "Failed to instantiate: " << desc->name << " (" << error << ")\n";
            continue;
        }
        auto file = echojay::extractToFile (*instance, outDir, cfg);
        std::cout << "OK  " << desc->name << " -> " << file.getFileName() << "\n";
        ++okCount;
    }
    if (okCount > 0) return 0;
    // Nothing written. Separate the reasons so the ledger does not conflate a
    // license wall with an opaque plugin (the SSL/McDSP triage confusion).
    if (instFail > 0) return licenseFail > 0 ? 4 : 5;   // license-refused vs load-failed
    return 3;                                            // all instantiated, nothing extractable
}

// ---------------------------------------------------------------------------
// Driver helpers
// ---------------------------------------------------------------------------
static juce::File echojayDir()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
             .getChildFile ("Library/EchoJay");
}

static juce::var readJsonFile (const juce::File& f)
{
    return f.existsAsFile() ? juce::JSON::parse (f.loadFileAsString()) : juce::var();
}

static void writeJsonFileAtomic (const juce::File& f, const juce::var& v)
{
    auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    tmp.replaceWithText (juce::JSON::toString (v));
    tmp.moveFileTo (f);
}

// endpoint from auth.json (same file/field EchoJayAPI persists), else default
static juce::String resolveEndpoint()
{
    auto auth = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                  .getChildFile ("Application Support/EchoJay/auth.json");
    auto j = readJsonFile (auth);
    auto ep = j.getProperty ("endpoint", juce::var()).toString();
    return ep.isNotEmpty() ? ep : juce::String (kDefaultEndpoint);
}

static juce::String httpGet (const juce::String& fullUrl, int& statusCode)
{
    juce::URL url (fullUrl);
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs (20000)
                    .withStatusCode (&statusCode);
    if (auto stream = url.createInputStream (opts))
        return stream->readEntireStreamAsString();
    return {};
}

static juce::String httpPostJson (const juce::String& fullUrl, const juce::String& body, int& statusCode)
{
    juce::URL url = juce::URL (fullUrl).withPOSTData (body);
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                    .withConnectionTimeoutMs (30000)
                    .withExtraHeaders ("Content-Type: application/json\r\n")
                    .withStatusCode (&statusCode);
    if (auto stream = url.createInputStream (opts))
        return stream->readEntireStreamAsString();
    return {};
}

// Which arch slice to run the worker under for this bundle. Prefer the host
// arch; fall back to x86_64 under Rosetta for Intel-only plugins (mirrors
// the original run_extractor.sh pick_arch).
static juce::String pickArch (const juce::File& bundle)
{
    auto macosDir = bundle.getChildFile ("Contents/MacOS");
    juce::String archs;
    for (const auto& f : macosDir.findChildFiles (juce::File::findFiles, false))
    {
        juce::ChildProcess lipo;
        if (lipo.start (juce::StringArray { "/usr/bin/lipo", "-archs", f.getFullPathName() }))
            archs += " " + lipo.readAllProcessOutput().trim();
    }
#if JUCE_ARM
    const juce::String hostArch = "arm64";
#else
    const juce::String hostArch = "x86_64";
#endif
    if (archs.contains (hostArch))  return {};          // native, no wrapper
    if (archs.contains ("x86_64")) return "x86_64";     // Rosetta
    if (archs.contains ("arm64"))  return "arm64";
    return {};                                          // unknown: try native
}

// File-level identity for incremental scanning:
// CFBundleShortVersionString | newest mtime across Contents/MacOS binaries.
// A plugin update changes the binary mtime (and usually the version), so a
// changed signature is exactly "new or updated"; an unchanged signature
// means the cached fingerprints are still valid and instantiation can be
// skipped. Bundle-root mtime alone is NOT enough: an in-place update can
// replace files deep in the bundle without touching the root directory.
static juce::String bundleSignature (const juce::File& bundle)
{
    juce::int64 newest = 0;
    for (const auto& f : bundle.getChildFile ("Contents/MacOS")
                               .findChildFiles (juce::File::findFiles, false))
        newest = juce::jmax (newest, f.getLastModificationTime().toMilliseconds());
    if (newest == 0)
        newest = bundle.getLastModificationTime().toMilliseconds();

    // Version via in-process plist parse: spawning `defaults read` per
    // bundle cost minutes per run at ~1200 bundles (measured), far too slow
    // for a login-time agent. XML plists (the overwhelming majority of
    // plugin bundles) parse directly; binary plists fall back to an empty
    // version, where the binary mtime alone still catches every update.
    juce::String version;
    auto plist = bundle.getChildFile ("Contents/Info.plist");
    if (plist.existsAsFile())
    {
        auto text = plist.loadFileAsString();
        if (! text.startsWith ("bplist"))
            if (auto xml = juce::parseXML (text))
                if (auto* dict = xml->getChildByName ("dict"))
                {
                    // plist dict = alternating <key>/<value> elements
                    for (auto* key = dict->getFirstChildElement(); key != nullptr;
                         key = key->getNextElement())
                        if (key->hasTagName ("key")
                            && key->getAllSubText().trim() == "CFBundleShortVersionString")
                        {
                            if (auto* val = key->getNextElement())
                                version = val->getAllSubText().trim();
                            break;
                        }
                }
    }
    return version + "|" + juce::String (newest);
}

// Random per-machine UUID shared with the plugin's events.jsonl telemetry
// (~/Library/EchoJay/machine_id, minted by whichever side touches it first).
// NEVER hostname/serial derived. 0600 on create.
static juce::String machineIdString()
{
    auto f = echojayDir().getChildFile ("machine_id");
    auto id = f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();
    if (id.length() != 36)
    {
        id = juce::Uuid().toDashedString();
        echojayDir().createDirectory();
        f.replaceWithText (id + "\n");
        ::chmod (f.getFullPathName().toRawUTF8(), 0600);
    }
    return id;
}

// One isolated worker run: spawn, wait with timeout, classify the outcome.
// A crash or hang costs ONE ledger entry, never the driver: setValue on an
// offline instance can trigger heavy DSP reconfiguration and take the
// process down, which is exactly why extraction stays one-plugin-per-process.
struct WorkerResult
{
    juce::String status;   // ok | failed | crashed | timeout
    juce::String reason;
    int exitCode = -1;
    juce::int64 elapsedMs = 0;
};

// Core: spawn one isolated worker on an arbitrary TARGET (a bundle path OR an
// AU component identifier), optionally under a specific arch. The exit-code
// classifier is the ONE place worker outcomes become ledger statuses.
static WorkerResult runIsolatedWorkerOn (const juce::String& target, const juce::String& arch,
                                         const juce::File& workDir)
{
    const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    juce::StringArray args;
    if (arch.isNotEmpty()) { args.add ("/usr/bin/arch"); args.add ("-" + arch); }
    args.add (self.getFullPathName());
    args.add (target);
    args.add (workDir.getFullPathName());

    WorkerResult r;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    juce::ChildProcess worker;
    if (! worker.start (args))
    {
        r.status = "failed"; r.reason = "spawn failed";
        return r;
    }
    if (! worker.waitForProcessToFinish (kPerPluginTimeoutMs))
    {
        worker.kill();
        r.status = "timeout"; r.reason = "killed after " + juce::String (kPerPluginTimeoutMs / 1000) + "s";
        r.elapsedMs = (juce::int64) (juce::Time::getMillisecondCounterHiRes() - t0);
        return r;
    }
    r.exitCode  = (int) worker.getExitCode();
    r.elapsedMs = (juce::int64) (juce::Time::getMillisecondCounterHiRes() - t0);
    // Fine-grained outcome vocabulary (runWorker exit codes). Distinguishing
    // license-refused from no-types is the whole point of the SSL/McDSP fix;
    // an unrecognised code is a signal/abort, i.e. a real crash.
    switch (r.exitCode)
    {
        case 0:  r.status = "ok";                                                            break;
        case 2:  r.status = "no-types";        r.reason = "no plugin types found";           break;
        case 3:  r.status = "no-data";         r.reason = "instantiated, nothing extractable";break;
        case 4:  r.status = "license-refused"; r.reason = "instantiation refused (auth/license)"; break;
        case 5:  r.status = "load-failed";     r.reason = "instantiation failed (non-license)";   break;
        default: r.status = "crashed";         r.reason = "abnormal exit " + juce::String (r.exitCode); break;
    }
    return r;
}

static WorkerResult runIsolatedWorker (const juce::File& bundle, const juce::File& workDir)
{
    return runIsolatedWorkerOn (bundle.getFullPathName(), pickArch (bundle), workDir);
}

static void stampLedgerEntry (juce::DynamicObject::Ptr& e, const juce::String& sig,
                              const WorkerResult& r)
{
    e->setProperty ("sig", sig);
    e->setProperty ("status", r.status);
    if (r.reason.isNotEmpty()) e->setProperty ("reason", r.reason);
    e->setProperty ("exitCode", r.exitCode);
    e->setProperty ("elapsedMs", (double) r.elapsedMs);
    e->setProperty ("extractorVersion", echojay::ExtractorConfig{}.extractorVersion);
    e->setProperty ("machineId", machineIdString());
}

static int runBootstrap()
{
    setpriority (PRIO_PROCESS, 0, 19);   // politeness floor; plist adds IO throttle

    // CONSENT GATE. The INSTALLER writes mapping_consent.json (the "Set up
    // plugin auto-mapping" component + the Share / Don't share choice);
    // this process only reads it. Two-level consent:
    //   "fetch-only"           scan + fetch maps (read-only); NEVER contribute
    //   "fetch-and-contribute" scan + fetch + contribute samples upstream
    //   "granted"              legacy in-app Yes: full behaviour
    //   "declined"             legacy in-app Not-now: exit entirely
    //   absent                 exit entirely (no consent basis)
    // The contribute stage below is additionally gated on contributeAllowed;
    // the ONLY upstream write in this program is that one POST, so
    // "fetch-only" can never share anything.
    bool contributeAllowed = false;
    {
        auto consent = readJsonFile (echojayDir().getChildFile ("mapping_consent.json"))
                         .getProperty ("consent", juce::var()).toString();
        const bool runAllowed = (consent == "fetch-only"
                                 || consent == "fetch-and-contribute"
                                 || consent == "granted");
        contributeAllowed = (consent == "fetch-and-contribute" || consent == "granted");
        if (! runAllowed)
        {
            std::cout << "bootstrap: consent " << (consent.isEmpty() ? "unset" : consent)
                      << ", exiting without scanning\n";
            return 0;
        }
        std::cout << "bootstrap: consent " << consent
                  << (contributeAllowed ? " (fetch + contribute)\n" : " (fetch only, no sharing)\n");
    }

    auto dir = echojayDir();
    dir.createDirectory();
    auto ledgerFile  = dir.getChildFile ("bootstrap_ledger.json");
    auto outFile     = dir.getChildFile ("param_maps_bootstrap.json");
    auto samplesDir  = dir.getChildFile ("bootstrap_samples");
    auto logLine = [] (const juce::String& s)
    {
        std::cout << s << "\n";
        std::cout.flush();
    };

    auto ledger = readJsonFile (ledgerFile);
    juce::DynamicObject::Ptr led = ledger.getDynamicObject() != nullptr
        ? juce::DynamicObject::Ptr (ledger.getDynamicObject())
        : juce::DynamicObject::Ptr (new juce::DynamicObject());
    juce::DynamicObject::Ptr plugins = led->getProperty ("plugins").getDynamicObject();
    if (plugins == nullptr) { plugins = new juce::DynamicObject(); led->setProperty ("plugins", juce::var (plugins.get())); }

    // fps already POSTed to /api/params/contribute (per this contributor):
    // still-unmapped plugins are not re-contributed on every run.
    juce::StringArray contributedFps;
    if (auto* arr = led->getProperty ("contributedFps").getArray())
        for (auto& v : *arr) contributedFps.addIfNotAlreadyThere (v.toString());

    // Stable ANONYMOUS contributor id: a random per-machine UUID, persisted
    // in the ledger. NEVER an email or account id: the installer pane says
    // "anonymized" and the payload honors it. The server only needs a
    // stable id per machine for MIN_CONTRIBUTORS counting, and a random
    // UUID is strictly more anonymous than any hash of an identifier. The
    // "@" guard migrates ledgers written before this fix, which could have
    // persisted a raw email as the contributor.
    juce::String contributor = led->getProperty ("contributor").toString();
    if (contributor.isEmpty() || contributor.contains ("@"))
    {
        contributor = "anon-" + juce::Uuid().toDashedString();
        led->setProperty ("contributor", contributor);
    }
    auto saveLedger = [&] { writeJsonFileAtomic (ledgerFile, juce::var (led.get())); };
    saveLedger();

    // Walk the plugin folders. EchoJay's own bundles are skipped: the point
    // is third-party maps, and headless-instantiating ourselves would spin
    // up timers and network from inside the mapper.
    juce::Array<juce::File> bundles;
    for (const char* d : { "/Library/Audio/Plug-Ins/VST3", "/Library/Audio/Plug-Ins/Components" })
        for (const auto& f : juce::File (d).findChildFiles (juce::File::findDirectories | juce::File::findFiles, false))
            bundles.add (f);
    auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    for (const char* d : { "Library/Audio/Plug-Ins/VST3", "Library/Audio/Plug-Ins/Components" })
        for (const auto& f : home.getChildFile (d).findChildFiles (juce::File::findDirectories | juce::File::findFiles, false))
            bundles.add (f);

    int fresh = 0, failed = 0, reused = 0, skippedMarked = 0;

    for (const auto& b : bundles)
    {
        const auto ext = b.getFileExtension().toLowerCase();
        if (ext != ".vst3" && ext != ".component") continue;
        if (b.getFileName().startsWithIgnoreCase ("EchoJay")) continue;

        const auto key = b.getFullPathName();
        const auto sig = bundleSignature (b);
        auto entry = plugins->getProperty (juce::Identifier (key));
        const auto priorSig    = entry.getProperty ("sig", juce::var()).toString();
        const auto priorStatus = entry.getProperty ("status", juce::var()).toString();

        // INCREMENTAL SKIP, keyed to bundle signature AND extractor version:
        // an ok entry from an older extractor re-extracts (its samples lack
        // the newer method/flat provenance), and a failure mark from a BAD
        // BUILD cannot poison the ledger permanently: bumping the extractor
        // version retries everything once. "started" is the crash marker.
        const int priorVer = (int) entry.getProperty ("extractorVersion", 0);
        const int curVer   = echojay::ExtractorConfig{}.extractorVersion;
        const bool sameKey = (priorSig == sig && priorVer == curVer);
        if (sameKey && priorStatus == "ok")               { ++reused; continue; }
        if (sameKey && priorStatus != "ok" && priorStatus.isNotEmpty())
                                                          { ++skippedMarked; continue; }

        // New or changed bundle: extract in an isolated worker process.
        // Started-marker BEFORE the spawn: if this plugin takes the driver
        // down, the next run skips it at this signature.
        {
            juce::DynamicObject::Ptr e = new juce::DynamicObject();
            e->setProperty ("status", "started");
            e->setProperty ("sig", sig);
            e->setProperty ("extractorVersion", echojay::ExtractorConfig{}.extractorVersion);
            plugins->setProperty (juce::Identifier (key), juce::var (e.get()));
            saveLedger();
        }

        // Per-plugin worker output dir so this bundle's fps are attributable
        // (one bundle can host several plugin types).
        auto workDir = samplesDir.getChildFile (juce::MD5 (key.toUTF8()).toHexString());
        workDir.deleteRecursively();

        auto res = runIsolatedWorker (b, workDir);

        // Collect this bundle's fps, flatten samples into the shared dir.
        juce::Array<juce::var> fpList;
        for (const auto& f : workDir.findChildFiles (juce::File::findFiles, false, "*.json"))
        {
            fpList.add (f.getFileNameWithoutExtension());
            f.moveFileTo (samplesDir.getChildFile (f.getFileName()));
        }
        workDir.deleteRecursively();
        if (res.status == "ok" && fpList.isEmpty()) { res.status = "failed"; res.reason = "no samples written"; }

        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        stampLedgerEntry (e, sig, res);
        e->setProperty ("fps", fpList);
        plugins->setProperty (juce::Identifier (key), juce::var (e.get()));
        saveLedger();
        (res.status == "ok") ? ++fresh : ++failed;
        logLine (res.status.paddedRight (' ', 8) + b.getFileName()
                 + (res.reason.isNotEmpty() ? "  (" + res.reason + ")" : ""));
    }

    // Union of fingerprints across every ok ledger entry (cached + fresh).
    juce::StringArray allFps;
    for (auto& p : plugins->getProperties())
        if (p.value.getProperty ("status", juce::var()).toString() == "ok")
            if (auto* arr = p.value.getProperty ("fps", juce::var()).getArray())
                for (auto& fp : *arr) allFps.addIfNotAlreadyThere (fp.toString());
    logLine ("plugins: " + juce::String (fresh) + " extracted, " + juce::String (reused)
             + " reused (unchanged), " + juce::String (failed) + " failed, "
             + juce::String (skippedMarked) + " skip-marked; "
             + juce::String (allFps.size()) + " fingerprints total");

    // Start from the existing bootstrap cache: on an incremental run the
    // samples for unchanged plugins are long gone, but their identity index
    // and fetched maps persist here.
    auto existing = readJsonFile (outFile);
    juce::DynamicObject::Ptr outIdentity = new juce::DynamicObject();
    juce::DynamicObject::Ptr outMaps     = new juce::DynamicObject();
    if (auto* o = existing.getProperty ("identityToFp", juce::var()).getDynamicObject())
        for (auto& p : o->getProperties()) outIdentity->setProperty (p.name, p.value);
    if (auto* o = existing.getProperty ("maps", juce::var()).getDynamicObject())
        for (auto& p : o->getProperties()) outMaps->setProperty (p.name, p.value);

    // Identity entries from any sample files on disk (fresh this run, or
    // left over awaiting contribution).
    for (const auto& f : samplesDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        auto s = readJsonFile (f);
        auto ident = s.getProperty ("identity", juce::var());
        const auto fp = s.getProperty ("fp", juce::var()).toString();
        if (fp.isEmpty()) continue;
        const auto ik = ident.getProperty ("format", juce::var()).toString() + "|"
                      + ident.getProperty ("uid", juce::var()).toString() + "|"
                      + ident.getProperty ("version", juce::var()).toString();
        outIdentity->setProperty (juce::Identifier (ik), fp);
    }

    // RECONCILE + REVALIDATE: every known fp is re-requested each run (not
    // just uncached ones). The server stamps maps with a content rev, so a
    // corrected map overwrites the stale local copy and a RETRACTED map
    // (explicit null for an fp we have cached) is dropped. This heals
    // machines whose bootstrap cache holds since-fixed maps.
    juce::StringArray needFetch = allFps;

    const auto endpoint = resolveEndpoint();
    juce::StringArray unmappedFps;
    int retracted = 0;
    for (int i = 0; i < needFetch.size(); i += kMapsBatch)
    {
        juce::StringArray batch;
        for (int j = i; j < juce::jmin (needFetch.size(), i + kMapsBatch); ++j) batch.add (needFetch[j]);
        int status = 0;
        auto body = httpGet (endpoint + "/api/params/maps?fps=" + batch.joinIntoString (","), status);
        if (status != 200) { logLine ("maps fetch failed, status " + juce::String (status)); continue; }
        auto maps = juce::JSON::parse (body).getProperty ("maps", juce::var());
        if (auto* o = maps.getDynamicObject())
            for (auto& p : o->getProperties())
            {
                if (p.value.getDynamicObject() != nullptr)
                    outMaps->setProperty (p.name, p.value);
                else
                {
                    if (outMaps->hasProperty (p.name)) { outMaps->removeProperty (p.name); ++retracted; }
                    unmappedFps.addIfNotAlreadyThere (p.name.toString());
                }
            }
    }
    if (retracted > 0) logLine ("retracted " + juce::String (retracted) + " stale map(s)");

    juce::DynamicObject::Ptr out = new juce::DynamicObject();
    out->setProperty ("identityToFp", juce::var (outIdentity.get()));
    out->setProperty ("maps", juce::var (outMaps.get()));
    out->setProperty ("updated", juce::Time::getCurrentTime().toISO8601 (true));
    writeJsonFileAtomic (outFile, juce::var (out.get()));
    logLine ("wrote " + outFile.getFileName() + ": "
             + juce::String (outMaps->getProperties().size()) + " map(s), "
             + juce::String (outIdentity->getProperties().size()) + " identities, "
             + juce::String (needFetch.size()) + " fetched this run");

    // Contribute samples for unmapped fps not contributed before. A sample
    // is deleted once contributed (or once its map exists): the server has
    // it, and keeping it would only grow the dir forever.
    // SHARE GATE: with "fetch-only" consent this whole stage is skipped;
    // nothing is ever sent upstream, and samples stay local.
    if (! contributeAllowed)
    {
        logLine ("share not consented: contribute stage skipped (fetch-only)");
        led->setProperty ("lastRun", juce::Time::getCurrentTime().toISO8601 (true));
        saveLedger();
        logLine ("bootstrap: run complete");
        return 0;
    }
    juce::Array<juce::var> toContribute;
    for (const auto& f : samplesDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto fp = f.getFileNameWithoutExtension();
        if (outMaps->hasProperty (juce::Identifier (fp))) { f.deleteFile(); continue; }
        if (! unmappedFps.contains (fp) || contributedFps.contains (fp)) continue;
        auto s = readJsonFile (f);
        if (s.getProperty ("fp", juce::var()).toString().isNotEmpty()) toContribute.add (s);
    }
    int contributed = 0;
    juce::Array<juce::var> batch;
    juce::int64 batchBytes = 0;
    auto flushBatch = [&]()
    {
        if (batch.isEmpty()) return;
        juce::DynamicObject::Ptr req = new juce::DynamicObject();
        req->setProperty ("contributor", contributor);
        req->setProperty ("samples", batch);
        int status = 0;
        httpPostJson (endpoint + "/api/params/contribute", juce::JSON::toString (juce::var (req.get())), status);
        if (status == 202)
        {
            for (auto& s : batch)
            {
                const auto fp = s.getProperty ("fp", juce::var()).toString();
                contributedFps.addIfNotAlreadyThere (fp);
                samplesDir.getChildFile (fp + ".json").deleteFile();
            }
            contributed += batch.size();
        }
        else logLine ("contribute failed, status " + juce::String (status)
                      + " (" + juce::String (batch.size()) + " sample(s), "
                      + juce::String (batchBytes / 1024) + " KB)");
        batch.clear();
        batchBytes = 0;
    };
    for (const auto& s : toContribute)
    {
        const auto bytes = (juce::int64) juce::JSON::toString (s).getNumBytesAsUTF8();
        if (bytes > kContributeMaxSampleBytes)
        {
            logLine ("sample too large to contribute ("
                     + juce::String (bytes / 1024) + " KB), skipping "
                     + s.getProperty ("fp", juce::var()).toString().substring (0, 12));
            continue;
        }
        if (! batch.isEmpty()
            && (batchBytes + bytes > kContributeBatchBytes
                || batch.size() >= kContributeBatchCount))
            flushBatch();
        batch.add (s);
        batchBytes += bytes;
    }
    flushBatch();
    logLine ("contributed " + juce::String (contributed) + " sample(s) for unmapped plugins");

    juce::Array<juce::var> contribVar;
    for (const auto& fp : contributedFps) contribVar.add (fp);
    led->setProperty ("contributedFps", contribVar);
    led->setProperty ("lastRun", juce::Time::getCurrentTime().toISO8601 (true));
    saveLedger();
    logLine ("bootstrap: run complete");
    return 0;
}

// ---------------------------------------------------------------------------
// Targeted re-extraction driver (27 Jul 2026): the studio set-then-read run.
//   ejextract --extract-list <listfile> [outDir]
// <listfile>: one case-insensitive substring per line (# = comment); every
// installed VST3/Component bundle whose FILE NAME contains a listed
// substring is re-extracted, BOTH formats, ignoring the incremental
// skip-lock. Samples land in outDir (default ~/Library/EchoJay/
// retry_samples) and STAY there: this mode never fetches, never
// contributes, and never touches param_maps_bootstrap.json, so it is safe
// to run without the consent gate (nothing leaves the machine). The
// bootstrap ledger IS updated (stamped entries, distinct statuses) so runs
// are auditable and the purge/re-contribute step can key off it later.
// ---------------------------------------------------------------------------
static int runExtractList (const juce::File& listFile, const juce::File& outDir)
{
    setpriority (PRIO_PROCESS, 0, 10);

    if (! listFile.existsAsFile())
    {
        std::cerr << "extract-list: no such file: " << listFile.getFullPathName() << "\n";
        return 1;
    }
    juce::StringArray needles;
    for (auto& line : juce::StringArray::fromLines (listFile.loadFileAsString()))
    {
        auto t = line.upToFirstOccurrenceOf ("#", false, false).trim();
        if (t.isNotEmpty()) needles.add (t.toLowerCase());
    }
    if (needles.isEmpty()) { std::cerr << "extract-list: empty list\n"; return 1; }

    auto dir = echojayDir();
    dir.createDirectory();
    outDir.createDirectory();
    auto ledgerFile = dir.getChildFile ("bootstrap_ledger.json");
    auto ledger = readJsonFile (ledgerFile);
    juce::DynamicObject::Ptr led = ledger.getDynamicObject() != nullptr
        ? juce::DynamicObject::Ptr (ledger.getDynamicObject())
        : juce::DynamicObject::Ptr (new juce::DynamicObject());
    juce::DynamicObject::Ptr plugins = led->getProperty ("plugins").getDynamicObject();
    if (plugins == nullptr) { plugins = new juce::DynamicObject(); led->setProperty ("plugins", juce::var (plugins.get())); }
    auto saveLedger = [&] { writeJsonFileAtomic (ledgerFile, juce::var (led.get())); };

    juce::Array<juce::File> bundles;
    auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    for (const auto& root : { juce::File ("/Library/Audio/Plug-Ins/VST3"),
                              juce::File ("/Library/Audio/Plug-Ins/Components"),
                              home.getChildFile ("Library/Audio/Plug-Ins/VST3"),
                              home.getChildFile ("Library/Audio/Plug-Ins/Components") })
        for (const auto& f : root.findChildFiles (juce::File::findDirectories | juce::File::findFiles, false))
        {
            const auto ext = f.getFileExtension().toLowerCase();
            if (ext != ".vst3" && ext != ".component") continue;
            if (f.getFileName().startsWithIgnoreCase ("EchoJay")) continue;
            const auto lower = f.getFileName().toLowerCase();
            for (const auto& n : needles)
                if (lower.contains (n)) { bundles.add (f); break; }
        }

    std::cout << "extract-list: " << bundles.size() << " bundle(s) matched, extractor v"
              << echojay::ExtractorConfig{}.extractorVersion << ", machine "
              << machineIdString().substring (0, 8) << "...\n";
    for (const auto& b : bundles) std::cout << "  " << b.getFullPathName() << "\n";
    std::cout.flush();

    int okN = 0, failedN = 0, crashedN = 0, timeoutN = 0, allFlatN = 0;
    int setreadTotal = 0, unextractableTotal = 0, unexBundles = 0;
    for (const auto& b : bundles)
    {
        const auto key = b.getFullPathName();
        const auto sig = bundleSignature (b);

        // Started-marker before spawn, same crash discipline as bootstrap.
        {
            juce::DynamicObject::Ptr e = new juce::DynamicObject();
            e->setProperty ("status", "started");
            e->setProperty ("sig", sig);
            e->setProperty ("extractorVersion", echojay::ExtractorConfig{}.extractorVersion);
            plugins->setProperty (juce::Identifier (key), juce::var (e.get()));
            saveLedger();
        }

        auto workDir = outDir.getChildFile (juce::MD5 (key.toUTF8()).toHexString());
        workDir.deleteRecursively();
        auto res = runIsolatedWorker (b, workDir);

        juce::Array<juce::var> fpList;
        int setreadParams = 0, unextractable = 0, allFlatSamples = 0, samples = 0;
        for (const auto& f : workDir.findChildFiles (juce::File::findFiles, false, "*.json"))
        {
            ++samples;
            fpList.add (f.getFileNameWithoutExtension());
            auto sample = readJsonFile (f);
            if ((bool) sample.getProperty ("all_flat", false)) ++allFlatSamples;
            if (auto* arr = sample.getProperty ("params", juce::var()).getArray())
                for (auto& pv : *arr)
                {
                    if (pv.getProperty ("method", juce::var()).toString() == "setread") ++setreadParams;
                    if ((bool) pv.getProperty ("unextractable", false)) ++unextractable;
                }
            f.moveFileTo (outDir.getChildFile (f.getFileName()));
        }
        workDir.deleteRecursively();
        if (res.status == "ok" && samples == 0) { res.status = "failed"; res.reason = "no samples written"; }
        if (res.status == "ok" && samples > 0 && allFlatSamples == samples)
        {
            res.status = "all_flat";
            res.reason = "no value-bearing sweep via either method";
        }
        if (res.status == "ok" && (setreadParams > 0 || unextractable > 0))
        {
            juce::StringArray parts;
            if (setreadParams > 0)  parts.add ("setread recovered " + juce::String (setreadParams) + " param(s)");
            if (unextractable > 0)  parts.add (juce::String (unextractable) + " unextractable");
            res.reason = parts.joinIntoString (", ");
        }

        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        stampLedgerEntry (e, sig, res);
        e->setProperty ("fps", fpList);
        plugins->setProperty (juce::Identifier (key), juce::var (e.get()));
        saveLedger();

        setreadTotal += setreadParams;
        unextractableTotal += unextractable;
        if (unextractable > 0) ++unexBundles;
        if (res.status == "ok") ++okN;
        else if (res.status == "crashed") ++crashedN;
        else if (res.status == "timeout") ++timeoutN;
        else if (res.status == "all_flat") ++allFlatN;
        else ++failedN;   // failed / no-types / no-data / license-refused / load-failed
        std::cout << res.status.paddedRight (' ', 9) << b.getFileName()
                  << "  [" << (res.elapsedMs / 1000.0) << "s]"
                  << (res.reason.isNotEmpty() ? "  (" + res.reason + ")" : juce::String()) << "\n";
        std::cout.flush();
    }
    led->setProperty ("lastExtractList", juce::Time::getCurrentTime().toISO8601 (true));
    saveLedger();
    std::cout << "extract-list done: ok " << okN << ", all_flat " << allFlatN
              << ", failed " << failedN << ", crashed " << crashedN
              << ", timeout " << timeoutN
              << "; setread recovered " << setreadTotal << " param(s), "
              << unextractableTotal << " unextractable across " << unexBundles
              << " bundle(s); samples in "
              << outDir.getFullPathName() << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// AU registry enumeration (27 Jul 2026).
//
// The file walk (findAllTypesForFile on a bundle) returns only a shell's
// DEFAULT component, so WaveShell AU gave 5 of ~614 Waves variants. The OS
// AudioComponent registry lists every variant as its own component; enumerate
// THAT and dispatch each identifier to the same isolated worker. No shell
// file is opened by this driver, and the legacy VST3 shells (7.0/8.0/9.6) are
// a different FORMAT and never touched. Vendor-agnostic: the same blindspot
// hides SSL Native, McDSP, and others.
//
// Delta policy mirrors the file walk's au-done: identifiers already terminal
// in the ledger (ok / no-types / license-refused / load-failed / no-data) are
// skipped; only timeout / crashed are retried on a rerun. Apple system AUs and
// EchoJay's own plugins are excluded entirely.
//
// This is the STANDALONE corpus driver only; wiring the registry into
// --bootstrap (so every user machine stops under-contributing Waves AU) is a
// shipped-binary change that rides the next plugin release, not this run.
// ---------------------------------------------------------------------------
static int runAuRegistry (const juce::File& outDir, bool enumerateOnly)
{
#if ! (JUCE_PLUGINHOST_AU && JUCE_MAC)
    std::cerr << "AU host not built in; --au-registry is macOS/AU only\n";
    return 1;
#else
    // The walk, the vendor parse and the Apple/EchoJay exclusions all moved into
    // EchoJayAuRegistry.h so ejmap enumerates through the same code. The body
    // that used to be here is unchanged in behaviour, only relocated: it still
    // never resolves an identifier in-driver, because findAllTypesForFile on an
    // AU identifier instantiates the plugin and that would load ~1400 plugins
    // in-process. All instantiation stays in the isolated worker.
    const auto census = echojay::auregistry::buildCensus();

    struct Target { juce::String id, vendor; };
    std::vector<Target> targets;
    for (const auto& t : census.targets)
        targets.push_back ({ t.identifier, t.vendorName });

    const auto& byVendor = census.byVendor;

    std::cout << "AU registry: " << census.enumerated << " component(s) enumerated, "
              << targets.size() << " candidate(s) after excluding Apple ("
              << census.excludedApple << ") and EchoJay (" << census.excludedEcho << ")"
              << (census.unparsed ? "; " + juce::String (census.unparsed) + " identifier(s) unparsed (kept)" : "")
              << "\n";
    std::cout << "by vendor:\n";
    std::vector<std::pair<juce::String,int>> rows (byVendor.begin(), byVendor.end());
    std::sort (rows.begin(), rows.end(), [] (auto& a, auto& b) { return a.second > b.second; });
    for (auto& [v, n] : rows)
        if (n >= 3) std::cout << "  " << v.paddedRight (' ', 22) << n << "\n";

    if (enumerateOnly)
    {
        std::cout << "enumerate-only: no plugin instantiated, no extraction performed\n";
        return 0;
    }

    outDir.createDirectory();
    auto dir = echojayDir();
    auto ledgerFile = dir.getChildFile ("au_registry_ledger.json");
    auto ledger = readJsonFile (ledgerFile);
    juce::DynamicObject::Ptr led = ledger.getDynamicObject() != nullptr
        ? juce::DynamicObject::Ptr (ledger.getDynamicObject())
        : juce::DynamicObject::Ptr (new juce::DynamicObject());
    juce::DynamicObject::Ptr done = led->getProperty ("done").getDynamicObject();
    if (done == nullptr) { done = new juce::DynamicObject(); led->setProperty ("done", juce::var (done.get())); }
    auto saveLedger = [&] { writeJsonFileAtomic (ledgerFile, juce::var (led.get())); };

    // Terminal statuses are NOT retried; timeout/crashed ARE (a hang may be a
    // one-off licensing dialog that will not reappear).
    auto isTerminal = [] (const juce::String& s) {
        return s == "ok" || s == "no-types" || s == "no-data"
            || s == "license-refused" || s == "load-failed";
    };

    int okN = 0, licN = 0, notypesN = 0, loadN = 0, nodataN = 0, timeoutN = 0, crashedN = 0, skipN = 0;
    int idx = 0;
    for (const auto& t : targets)
    {
        ++idx;
        const auto keyId = juce::String (juce::MD5 (t.id.toUTF8()).toHexString());
        const auto prior = done->getProperty (keyId).getProperty ("status", juce::var()).toString();
        // Rerun delta by ledger only (we never resolve the uid in-driver):
        // terminal identifiers are skipped, timeout/crashed are retried.
        if (prior.isNotEmpty() && isTerminal (prior))
        { ++skipN; continue; }

        auto workDir = dir.getChildFile ("au_registry_work_" + juce::String (idx));
        workDir.deleteRecursively(); workDir.createDirectory();
        // Native arch: modern Waves/SSL AU components are universal; an
        // x86_64-only legacy component simply records load-failed.
        auto res = runIsolatedWorkerOn (t.id, juce::String(), workDir);

        juce::StringArray fpList;
        if (res.status == "ok")
        {
            int moved = 0;
            for (const auto& f : workDir.findChildFiles (juce::File::findFiles, false, "*.json"))
            {
                fpList.add (f.getFileNameWithoutExtension());
                f.moveFileTo (outDir.getChildFile (f.getFileName()));
                ++moved;
            }
            if (moved == 0) { res.status = "no-data"; res.reason = "worker ok but no sample file"; }
        }
        workDir.deleteRecursively();

        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        stampLedgerEntry (e, t.id, res);
        e->setProperty ("identifier", t.id);
        e->setProperty ("vendor", t.vendor);
        e->setProperty ("fps", fpList);
        done->setProperty (juce::Identifier (keyId), juce::var (e.get()));
        saveLedger();

        if      (res.status == "ok")              ++okN;
        else if (res.status == "license-refused") ++licN;
        else if (res.status == "no-types")        ++notypesN;
        else if (res.status == "load-failed")     ++loadN;
        else if (res.status == "no-data")         ++nodataN;
        else if (res.status == "timeout")         ++timeoutN;
        else if (res.status == "crashed")         ++crashedN;
        std::cout << "[" << idx << "/" << targets.size() << "] "
                  << res.status.paddedRight (' ', 15) << t.vendor << " / "
                  << t.id.fromLastOccurrenceOf ("/", false, false)
                  << "  [" << (res.elapsedMs / 1000.0) << "s]"
                  << (res.reason.isNotEmpty() ? "  (" + res.reason + ")" : juce::String()) << "\n";
        std::cout.flush();
    }

    led->setProperty ("lastRun", juce::Time::getCurrentTime().toISO8601 (true));
    saveLedger();
    std::cout << "au-registry done: ok " << okN << ", license-refused " << licN
              << ", no-types " << notypesN << ", load-failed " << loadN
              << ", no-data " << nodataN << ", timeout " << timeoutN
              << ", crashed " << crashedN << ", skipped " << skipN
              << "; samples in " << outDir.getFullPathName() << "\n";
    return 0;
#endif
}

// ---------------------------------------------------------------------------
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc >= 2 && juce::String (argv[1]) == "--bootstrap")
        return runBootstrap();

    if (argc >= 3 && juce::String (argv[1]) == "--extract-list")
        return runExtractList (juce::File (juce::String (juce::CharPointer_UTF8 (argv[2]))),
                               argc >= 4 ? juce::File (juce::String (juce::CharPointer_UTF8 (argv[3])))
                                         : echojayDir().getChildFile ("retry_samples"));

    if (argc >= 2 && juce::String (argv[1]) == "--au-registry")
    {
        // ejextract --au-registry [outDir] [--enumerate]
        bool enumerateOnly = false;
        juce::File outDir = echojayDir().getChildFile ("au_registry_samples");
        for (int i = 2; i < argc; ++i)
        {
            const juce::String a = juce::String (juce::CharPointer_UTF8 (argv[i]));
            if (a == "--enumerate") enumerateOnly = true;
            else outDir = juce::File (a);
        }
        return runAuRegistry (outDir, enumerateOnly);
    }

    if (argc < 3)
    {
        std::cerr << "Usage: ejextract <plugin-path> <output-dir> | ejextract --bootstrap"
                     " | ejextract --extract-list <listfile> [outDir]"
                     " | ejextract --au-registry [outDir] [--enumerate]\n";
        return 1;
    }
    return runWorker (juce::String (juce::CharPointer_UTF8 (argv[1])),
                      juce::File (juce::String (juce::CharPointer_UTF8 (argv[2]))));
}
