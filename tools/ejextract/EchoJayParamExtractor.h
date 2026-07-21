/*
  EchoJayParamExtractor.h

  Phase 0 discovery extractor for EchoJay auto-parameter-mapping.

  What it does:
    Given a loaded plugin instance, it enumerates every parameter, reads the
    plugin's own display string across a sweep of normalized values, and builds
    a JSON sample object (fingerprint, identity, per-param sweep tables).

  Key safety property:
    Discovery is READ-ONLY. It never calls setValue on any parameter. It only
    calls getText(normalisedValue), which asks the plugin what it WOULD display
    at a given normalized value without moving the control. Per the VST3/AU
    value-to-string contract this is stateless, so a sweep cannot disturb the
    plugin's state or audio. The only physically risky moment (instantiation)
    happens in your scanner, not here.

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
    int   extractorVersion   = 3;      // bump to trigger server re-harvest later
    int   coarsePoints       = 11;     // initial uniform sweep resolution
    int   maxRefineDepth     = 4;      // adaptive subdivision depth for curves
    int   maxSamplesPerParam = 64;     // cap upload size
    float curvatureTol       = 0.02f;  // relative deviation before we subdivide
    int   maxStringLen       = 256;    // for getName / getText / getLabel
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Parse the first signed float found in a display string.
// Handles "1.4:1" -> 1.4, "-18.0 dB" -> -18.0, "3.00 : 1" -> 3.0.
// Returns false for non-numeric readings like "Inf:1" or "Bypass".
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

        juce::Array<juce::var> sweepArr;
        for (auto& sp : sweep)
        {
            auto* pt = new juce::DynamicObject();
            pt->setProperty ("n", sp.n);
            pt->setProperty ("t", sp.t);
            sweepArr.add (juce::var (pt));
        }
        obj->setProperty ("sweep", sweepArr);

        paramArray.add (juce::var (obj));
    }

    auto* sample = new juce::DynamicObject();
    sample->setProperty ("fp",       fp);
    sample->setProperty ("identity", juce::var (identity.get()));
    sample->setProperty ("params",   paramArray);
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
