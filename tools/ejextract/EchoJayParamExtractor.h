/*
  EchoJayParamExtractor.h

  Phase 0 discovery extractor for EchoJay auto-parameter-mapping.

  What it does:
    Given a loaded plugin instance, it enumerates every parameter, reads the
    plugin's own display string across a sweep of normalized values, and builds
    a JSON sample object (fingerprint, identity, per-param sweep tables).

  Key safety property (amended 27 Jul 2026):
    Discovery is READ-ONLY by default: getText(normalisedValue) asks the
    plugin what it WOULD display without moving the control. But some vendors
    (Valhalla, elysia, Vertigo, SPL class) ignore the argument and format
    their CURRENT state, which makes every sweep point read identically
    (a "flat" sweep) and yields no usable map. For exactly those params a
    conditional SET-THEN-READ retry runs: setValue(n), read
    getCurrentValueAsText(), restore. Mutation is bracketed by a full
    getStateInformation snapshot/restore, and a readback-verify step (two
    distinct set points must produce distinct texts) stops us from mutating
    plugins that ignore both paths. Only flat params ever pay this cost;
    every param records method: "gettext" | "setread" for provenance.

  Integration seams (marked SEAM below):
    1. Identity: align makeIdentity() with your canonical plugin DB scheme so
       fingerprints match what the scanner and DB already produce.
    2. Output: extractToFile() writes a local JSON file (Phase 0). In Phase 4
       you post extractSample() straight to /api/params/contribute instead.

  House style: no em-dashes in comments.

  Requires JUCE modules: juce_audio_processors, juce_core, juce_cryptography.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

namespace echojay
{

struct ExtractorConfig
{
    int   extractorVersion   = 5;      // 5 = machine_id stamped in samples, unextractable surfaced in run logs (4 = set-then-read retry)
    int   coarsePoints       = 11;     // initial uniform sweep resolution
    int   maxRefineDepth     = 4;      // adaptive subdivision depth for curves
    int   maxSamplesPerParam = 64;     // cap upload size
    float curvatureTol       = 0.02f;  // relative deviation before we subdivide
    int   maxStringLen       = 256;    // for getName / getText / getLabel
    bool  retryFlatWithSetRead = true; // flat params only; never the whole sweep
    int   setReadPoints      = 21;     // uniform points for the set-read sweep

    /** Settle before each set-then-read display read. DEFAULT 0: the corpus
        was built without it and the extractor's behaviour must stay
        byte-identical. Demonstrated needed 31 Jul 2026 on a BRIDGED AU
        (API-2500 via AUHostingServiceXPC): a read immediately after a set
        serves the stale value across the XPC boundary, collapsing every
        label to the pre-set text. ejmap sets 15 ms; the extractor leaves 0.
    */
    int   settleMs           = 0;

    /** When set, called INSTEAD of sleeping for settleMs. Needed because a
        bridged AU's read-after-set depends on an XPC reply delivered by the
        MESSAGE LOOP: Thread::sleep on the message thread blocks the very
        runloop the reply needs, so the read stays stale no matter how long
        the sleep (measured: 15 ms sleep changed nothing on API-2500). The
        caller supplies a pump; the extractor leaves it null and keeps its
        corpus behaviour exactly.
    */
    std::function<void()> settle;

    juce::String machineId;            // stamped into samples when set (provenance)
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Parse the first signed float found in a display string.
// Handles "1.4:1" -> 1.4, "-18.0 dB" -> -18.0, "3.00 : 1" -> 3.0.
// Returns false for fully non-numeric readings like "Bypass". NOTE: a digit
// anywhere counts, so "Inf:1" parses as 1.0 -- this comment used to claim it
// was rejected, which the drift gate disproved (M3 lift). The behaviour is
// what the 4,233-map corpus was built with, so the behaviour is what is
// pinned; only the comment was wrong.
inline bool parseLeadingFloat (const juce::String& text, double& out)
{
    auto p = text.getCharPointer();
    juce::String num;
    bool seenDigit = false, seenDot = false, started = false;

    while (! p.isEmpty())
    {
        auto c = *p;
        if (! started && (c == '-' || c == '+'))
        {
            num << c; started = true;
        }
        else if (c >= '0' && c <= '9')
        {
            num << c; seenDigit = true; started = true;
        }
        else if (c == '.' && ! seenDot && started)
        {
            num << c; seenDot = true;
        }
        else if (seenDigit)
        {
            break; // reached the unit or separator after a valid number
        }
        else if (started && ! seenDigit)
        {
            num.clear(); started = false; seenDot = false; // false start like a lone '-'
        }
        ++p;
    }

    if (! seenDigit)
        return false;

    out = num.getDoubleValue();
    return true;
}

// One sweep point.
struct SweepPoint { float n; juce::String t; };

// ---------------------------------------------------------------------------
// Adaptive sweep for a single continuous parameter (read-only).
// ---------------------------------------------------------------------------
inline juce::Array<SweepPoint> sweepContinuous (juce::AudioProcessorParameter& param,
                                                const ExtractorConfig& cfg)
{
    struct Node { float n; juce::String t; bool numeric; double v; };

    auto sampleAt = [&] (float n) -> Node
    {
        auto t = param.getText (n, cfg.maxStringLen);
        double v = 0.0;
        bool numeric = parseLeadingFloat (t, v);
        return { n, t, numeric, v };
    };

    juce::Array<Node> nodes;
    const int coarse = juce::jmax (2, cfg.coarsePoints);
    for (int i = 0; i < coarse; ++i)
        nodes.add (sampleAt ((float) i / (float) (coarse - 1)));

    // Estimate the value span from numeric coarse points, for a relative tolerance.
    double vmin = 1.0e12, vmax = -1.0e12; bool haveSpan = false;
    for (auto& nd : nodes)
        if (nd.numeric) { vmin = juce::jmin (vmin, nd.v); vmax = juce::jmax (vmax, nd.v); haveSpan = true; }
    const double span = (haveSpan && vmax > vmin) ? (vmax - vmin) : 1.0;
    const double tol  = cfg.curvatureTol * span;

    // Recursively subdivide intervals where the midpoint deviates from linear.
    std::function<void (Node, Node, int)> refine =
        [&] (Node a, Node b, int depth)
    {
        if (depth >= cfg.maxRefineDepth) return;
        if (nodes.size() >= cfg.maxSamplesPerParam) return;
        if (! (a.numeric && b.numeric)) return; // cannot judge curvature without numbers

        float mn = 0.5f * (a.n + b.n);
        if (mn <= a.n || mn >= b.n) return;

        Node m = sampleAt (mn);
        nodes.add (m);

        if (m.numeric)
        {
            double expected = 0.5 * (a.v + b.v);
            if (std::abs (m.v - expected) > tol)
            {
                refine (a, m, depth + 1);
                refine (m, b, depth + 1);
            }
        }
    };

    // Snapshot the coarse pairs before refining (nodes grows during refinement).
    juce::Array<Node> coarseSnapshot (nodes);
    for (int i = 0; i < coarseSnapshot.size() - 1; ++i)
        refine (coarseSnapshot.getReference (i), coarseSnapshot.getReference (i + 1), 0);

    // Dedupe by normalized value and sort.
    juce::Array<SweepPoint> out;
    std::sort (nodes.begin(), nodes.end(),
               [] (const Node& x, const Node& y) { return x.n < y.n; });
    float last = -1.0f;
    for (auto& nd : nodes)
    {
        if (nd.n - last > 1.0e-6f)
        {
            out.add ({ nd.n, nd.t });
            last = nd.n;
        }
    }
    return out;
}

// Enumerate the display string at each discrete step (read-only).
inline juce::Array<SweepPoint> sweepDiscrete (juce::AudioProcessorParameter& param,
                                              const ExtractorConfig& cfg)
{
    juce::Array<SweepPoint> out;
    int steps = juce::jmax (1, param.getNumSteps());
    steps = juce::jmin (steps, 256); // guard pathological step counts
    for (int i = 0; i < steps; ++i)
    {
        float n = (steps == 1) ? 0.0f : (float) i / (float) (steps - 1);
        out.add ({ n, param.getText (n, cfg.maxStringLen) });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Flat-sweep predicate + set-then-read retry (27 Jul 2026).
// A flat sweep (>=2 points, one distinct text) carries no value information:
// it is the signature of a plugin whose text path ignores the queried value.
// ---------------------------------------------------------------------------
inline bool sweepIsFlat (const juce::Array<SweepPoint>& sweep)
{
    if (sweep.size() < 2) return false;
    for (int i = 1; i < sweep.size(); ++i)
        if (sweep.getReference (i).t != sweep.getReference (0).t) return false;
    return true;
}

// Set-then-read sweep for ONE parameter. Readback-verify first: two distinct
// set points must yield distinct texts, else verified=false and the plugin
// is not mutated further (some ignore both text paths; retrying those is
// wasted risk). Restores the parameter's own value before returning; the
// caller additionally restores full plugin state.
inline juce::Array<SweepPoint> sweepSetRead (juce::AudioProcessorParameter& param,
                                             const ExtractorConfig& cfg,
                                             bool& verified)
{
    auto readAt = [&] (float n) -> juce::String
    {
        param.setValue (n);
        if (cfg.settle != nullptr)
            cfg.settle();
        else if (cfg.settleMs > 0)
            juce::Thread::sleep (cfg.settleMs);
        return param.getCurrentValueAsText().substring (0, cfg.maxStringLen);
    };

    const float original = param.getValue();
    const auto t0 = readAt (0.0f);
    const auto t1 = readAt (1.0f);
    verified = (t0 != t1);
    juce::Array<SweepPoint> out;
    if (! verified)
    {
        param.setValue (original);
        return out;
    }

    int pts = juce::jmax (2, cfg.setReadPoints);
    if (param.isDiscrete())
        pts = juce::jlimit (2, 256, param.getNumSteps());
    for (int i = 0; i < pts; ++i)
    {
        const float n = (float) i / (float) (pts - 1);
        out.add ({ n, readAt (n) });
    }
    param.setValue (original);
    return out;
}

// ---------------------------------------------------------------------------
// SEAM 1: identity. Align this with your canonical plugin DB.
// ---------------------------------------------------------------------------
inline juce::DynamicObject::Ptr makeIdentity (juce::AudioPluginInstance& plugin,
                                              int paramCount)
{
    auto desc = plugin.getPluginDescription();

    auto* id = new juce::DynamicObject();
    id->setProperty ("format",      desc.pluginFormatName);
    // NOTE: uniqueId is JUCE's stable id. If your canonical DB keys on the raw
    // VST3 FUID hex or an AU type:subtype:manu triple, substitute that here so
    // fingerprints match the rest of the system exactly.
    id->setProperty ("uid",         juce::String::toHexString (desc.uniqueId));
    id->setProperty ("name",        desc.name);
    id->setProperty ("vendor",      desc.manufacturerName);
    id->setProperty ("version",     desc.version);
    id->setProperty ("param_count", paramCount);
    return id;
}

inline juce::String makeFingerprint (const juce::DynamicObject::Ptr& identity)
{
    // Order matters and must be stable. Keep this in sync with the server.
    juce::String basis;
    basis << identity->getProperty ("format").toString()  << "|"
          << identity->getProperty ("uid").toString()     << "|"
          << identity->getProperty ("version").toString() << "|"
          << identity->getProperty ("param_count").toString();

    juce::SHA256 sha (basis.toRawUTF8(), basis.getNumBytesAsUTF8());
    return sha.toHexString();
}

// ---------------------------------------------------------------------------
// Build the full sample object for one plugin.
// This is exactly the per-plugin shape /api/params/contribute expects.
// ---------------------------------------------------------------------------
inline juce::var extractSample (juce::AudioPluginInstance& plugin,
                                const ExtractorConfig& cfg = {})
{
    auto& params = plugin.getParameters();

    auto identity = makeIdentity (plugin, params.size());
    auto fp = makeFingerprint (identity);

    // Full-state snapshot BEFORE anything: the set-then-read retry moves
    // parameters, and although each one restores its own value, a full
    // restore guarantees the instance leaves exactly as it arrived even if
    // moving one control side-effected another (linked params, macros).
    juce::MemoryBlock stateBefore;
    if (cfg.retryFlatWithSetRead)
        plugin.getStateInformation (stateBefore);
    bool mutated = false;
    bool anyUsable = false;

    juce::Array<juce::var> paramArray;

    for (auto* p : params)
    {
        if (p == nullptr) continue;

        auto* obj = new juce::DynamicObject();
        obj->setProperty ("index",        p->getParameterIndex());
        obj->setProperty ("name",         p->getName (cfg.maxStringLen));
        obj->setProperty ("label",        p->getLabel());
        obj->setProperty ("discrete",     p->isDiscrete());
        obj->setProperty ("num_steps",    p->isDiscrete() ? p->getNumSteps() : 0);
        obj->setProperty ("default_norm", p->getDefaultValue());

        auto sweep = p->isDiscrete() ? sweepDiscrete   (*p, cfg)
                                     : sweepContinuous (*p, cfg);

        // Conditional set-then-read: ONLY when the read-only sweep came
        // back flat. method records provenance for map-building.
        juce::String method = "gettext";
        if (cfg.retryFlatWithSetRead && sweepIsFlat (sweep))
        {
            bool verified = false;
            auto retried = sweepSetRead (*p, cfg, verified);
            mutated = true;
            if (verified && ! sweepIsFlat (retried))
            {
                sweep = retried;
                method = "setread";
            }
            else
                obj->setProperty ("unextractable", true); // flat via BOTH paths
        }
        if (sweep.size() >= 2 && ! sweepIsFlat (sweep))
            anyUsable = true;

        juce::Array<juce::var> sweepArr;
        for (auto& sp : sweep)
        {
            auto* pt = new juce::DynamicObject();
            pt->setProperty ("n", sp.n);
            pt->setProperty ("t", sp.t);
            sweepArr.add (juce::var (pt));
        }
        obj->setProperty ("sweep", sweepArr);
        obj->setProperty ("method", method);

        paramArray.add (juce::var (obj));
    }

    if (mutated && stateBefore.getSize() > 0)
        plugin.setStateInformation (stateBefore.getData(), (int) stateBefore.getSize());

    auto* sample = new juce::DynamicObject();
    sample->setProperty ("fp",       fp);
    sample->setProperty ("identity", juce::var (identity.get()));
    sample->setProperty ("params",   paramArray);
    sample->setProperty ("extractor_version", cfg.extractorVersion);
    if (cfg.machineId.isNotEmpty())
        sample->setProperty ("machine_id", cfg.machineId);
    // all_flat: nothing on this plugin produced a usable (value-bearing)
    // sweep in either method. Downstream gates skip these entirely.
    sample->setProperty ("all_flat", ! anyUsable && paramArray.size() > 0);
    return juce::var (sample);
}

// ---------------------------------------------------------------------------
// SEAM 2 (Phase 0): dump to a local file. One file per plugin, named by fp.
// In Phase 4 you post extractSample() to the server instead of writing here.
// ---------------------------------------------------------------------------
inline juce::File extractToFile (juce::AudioPluginInstance& plugin,
                                 const juce::File& outDir,
                                 const ExtractorConfig& cfg = {})
{
    auto sample = extractSample (plugin, cfg);
    auto fp = sample.getProperty ("fp", juce::var()).toString();

    outDir.createDirectory();
    auto file = outDir.getChildFile (fp + ".json");
    file.replaceWithText (juce::JSON::toString (sample, true));
    return file;
}

} // namespace echojay
