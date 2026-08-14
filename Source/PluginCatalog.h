#pragma once
#include <JuceHeader.h>

// ============================================================================
// PluginCatalog
// ============================================================================
// Curated, compiled-in catalogs for plugins that the filesystem scan in
// PluginScanner cannot resolve on its own:
//
//   1. WAVES (WaveShell). Waves ships every plugin it sells inside ONE shell
//      bundle — e.g. "WaveShell1-VST3 14.0.vst3". The shell filename tells us
//      only the Waves version, never which plugins are licensed/installed
//      inside it. The bare filesystem scan therefore records a single useless
//      entry literally named "WaveShell1-VST3 14.0". We can't enumerate the
//      real contents without hosting the shell (out of scope, and would mean
//      instantiating Waves' licensing layer). Instead, when a WaveShell is
//      detected we expand it into a curated list of the most common Waves
//      mixing/mastering plugins so the AI can actually reference them by name.
//
//   2. STOCK DAW PLUGINS. A DAW's bundled stock plugins (Logic's Channel EQ,
//      Ableton's Glue Compressor, FL's Fruity Limiter, Pro Tools' stock, etc.)
//      do NOT live in the shared VST3/AU/VST folders. They're embedded inside
//      the DAW application bundle or in DAW-private locations the scan never
//      visits. So even though the producer uses them daily, the AI never sees
//      them. When we detect that a given DAW is installed, we inject that
//      DAW's stock plugin catalog.
//
// DESIGN NOTES
// ------------
//  * These are NOT a substitute for the real scan — they're a supplement.
//    A real scanned Waves/stock entry (rare, but possible via custom folders)
//    is deduplicated against these by PluginScanner::addPlugin's name+manu
//    check, so we never double-count.
//  * Catalogs are intentionally curated to MIXING/MASTERING relevant tools
//    (EQ, comp, saturation, reverb, delay, limiter, channel strip, etc.) and
//    deliberately exclude instruments/synths, matching the "Effect" category
//    that getPluginNamesString() filters to. A few notable instruments are
//    tagged "Instrument" so the JSON view is still accurate.
//  * Lists are conservative on purpose. Better to under-list a few obscure
//    plugins than to tell the AI a user owns something they don't. The Waves
//    list covers the staples virtually every Waves user has; the stock lists
//    are exhaustive for the shipping version since those ARE guaranteed
//    present once the DAW is installed.
//  * Versions drift. Keep these updated as DAWs/Waves ship new bundles. The
//    detection is version-tolerant (substring/prefix matching) so a new
//    WaveShell version still triggers expansion.

namespace echojay
{

// ----------------------------------------------------------------------------
// Effect type classification
// ----------------------------------------------------------------------------
// Buckets an effect plugin into a processing TYPE from its name, so the AI
// feed can be capped per-type (a few EQs, a few comps, a few reverbs...) rather
// than dumping 40 EQs and no delays. Keyword-based, case-insensitive, ordered
// most-specific first. Returns a stable short tag. "Other" catches anything
// unrecognised (channel strips, multi-fx, exotic tools) — those are still
// useful to the AI so they aren't discarded, just bucketed together.
//
// This is intentionally heuristic. It runs once per plugin at scan time and
// the result is cached on the ScannedPlugin, so misclassifications are cheap
// and never block anything. The goal is a good SPREAD in the capped list, not
// perfect taxonomy.
enum class FxType { EQ, Dynamics, Limiter, Reverb, Delay, Saturation,
                    Modulation, Stereo, Pitch, Utility, Other };

inline const char* fxTypeTag(FxType t)
{
    switch (t)
    {
        case FxType::EQ:         return "EQ";
        case FxType::Dynamics:   return "Dynamics";
        case FxType::Limiter:    return "Limiter";
        case FxType::Reverb:     return "Reverb";
        case FxType::Delay:      return "Delay";
        case FxType::Saturation: return "Saturation";
        case FxType::Modulation: return "Modulation";
        case FxType::Stereo:     return "Stereo";
        case FxType::Pitch:      return "Pitch";
        case FxType::Utility:    return "Utility";
        default:                 return "Other";
    }
}

inline FxType classifyEffect(const juce::String& nameIn)
{
    auto n = nameIn.toLowerCase();

    // FabFilter Pro series: the model letter is the type, and the names share
    // the "fabfilter" token that collides with "filter". Handle explicitly
    // before any stripping. (Pro-Q EQ, Pro-C comp, Pro-L limiter, Pro-R
    // reverb, Pro-DS de-esser, Pro-MB multiband, Pro-G gate, Saturn satur,
    // Timeless delay, Volcano filter.)
    if (n.contains("fabfilter") || n.contains("pro-q") || n.contains("pro-c") ||
        n.contains("pro-l") || n.contains("pro-r") || n.contains("pro-ds") ||
        n.contains("pro-mb") || n.contains("pro-g") || n.contains("saturn") ||
        n.contains("timeless") || n.contains("volcano"))
    {
        if (n.contains("pro-q")) return FxType::EQ;
        if (n.contains("pro-c")) return FxType::Dynamics;
        if (n.contains("pro-l")) return FxType::Limiter;
        if (n.contains("pro-r")) return FxType::Reverb;
        if (n.contains("pro-ds")) return FxType::Dynamics;
        if (n.contains("pro-mb")) return FxType::Dynamics;
        if (n.contains("pro-g")) return FxType::Dynamics;
        if (n.contains("saturn")) return FxType::Saturation;
        if (n.contains("timeless")) return FxType::Delay;
        if (n.contains("volcano")) return FxType::EQ;
        // Bare "fabfilter ..." with no known suffix: fall through after
        // neutralising the colliding token.
    }

    // Strip the FabFilter token so it can't trigger the "filter" EQ rule for
    // anything that fell through above.
    n = n.replace("fabfilter", " ");

    auto has = [&n](const char* k) { return n.contains(k); };

    // Known model-number / shorthand dynamics names that lack a generic
    // keyword. Checked first so they don't fall through to Other.
    if (has("1176") || has("cla-76") || has("cla-2a") || has("cla-3a") ||
        has("la-2a") || has("la2a") || has(" 76") || has("-76") ||
        has("mc77") || has("bf-76") || has("distressor"))
        return FxType::Dynamics;

    // Limiter / mastering ceiling — check before generic dynamics so
    // "Ultramaximizer" / "maximizer" / "limiter" land here, not Dynamics.
    if (has("limiter") || has("maximizer") || has("maximis") ||
        has("ultramax") || has("brickwall") || has("clipper") || has("loudness"))
        return FxType::Limiter;

    // EQ — note "filter" still matches genuine filters (Auto Filter, Love
    // Philter) now that the FabFilter collision is stripped above.
    if (has("eq") || has("equali") || has("filter") || has("pultec") ||
        has("frequency") || has(" band") || has("philter"))
        return FxType::EQ;

    // Dynamics (comp / gate / expander / de-esser / transient)
    if (has("comp") || has("gate") || has("expander") || has("dynamic") ||
        has("de-ess") || has("deess") || has("transient") || has("1176") ||
        has("la-2a") || has("la2a") || has("opto") || has("vca") || has("fet") ||
        has("smack") || has("trans-x") || has("rider") || has("squeeze"))
        return FxType::Dynamics;

    // Reverb
    if (has("reverb") || has("verb") || has("room") || has("hall") ||
        has("plate") || has("chamber") || has("space") || has("convol") ||
        has("ir-") || has("ambien"))
        return FxType::Reverb;

    // Delay / echo
    if (has("delay") || has("echo") || has("tape delay") || has("ping"))
        return FxType::Delay;

    // Saturation / distortion / tape / drive
    if (has("satur") || has("distort") || has("tape") || has("overdrive") ||
        has("drive") || has("fuzz") || has("crush") || has("lo-fi") ||
        has("lofi") || has("exciter") || has("warmth") || has("console") ||
        has("preamp") || has("amp") || has("magneto") || has("decapit") ||
        has("rectif") || has("bitcrush"))
        return FxType::Saturation;

    // Modulation
    if (has("chorus") || has("flang") || has("phaser") || has("phase") ||
        has("tremolo") || has("trem") || has("vibrato") || has("ensemble") ||
        has("rotary") || has("uni-v") || has("modul"))
        return FxType::Modulation;

    // Stereo / imaging / pan
    if (has("stereo") || has("imag") || has("width") || has("mid-side") ||
        has("midside") || has("m/s") || has("pan") || has("spread") ||
        has("center") || has("centre") || has("motion") || has("haas"))
        return FxType::Stereo;

    // Pitch / tuning / vocal pitch
    if (has("tune") || has("pitch") || has("auto-tune") || has("autotune") ||
        has("melodyne") || has("doubler") || has("harmon") || has("formant") ||
        has("vocal transformer"))
        return FxType::Pitch;

    // Utility / metering / analysis / noise
    if (has("meter") || has("analyz") || has("analys") || has("spectr") ||
        has("scope") || has("tuner") || has("gain") || has("trim") ||
        has("vu ") || has("noise") || has("hum") || has("denois") ||
        has("restor") || has("clean"))
        return FxType::Utility;

    return FxType::Other;
}

struct CatalogEntry
{
    const char* name;
    const char* category; // "Effect" or "Instrument"
};

// ----------------------------------------------------------------------------
// WAVES
// ----------------------------------------------------------------------------
// Staple Waves mixing/mastering plugins. Manufacturer is reported as "Waves".
// This is not the entire Waves catalog (that's 200+ titles) — it's the subset
// that shows up in real mix sessions and that the AI should be able to name.
inline const std::vector<CatalogEntry>& wavesCatalog()
{
    static const std::vector<CatalogEntry> c = {
        // Compressors / dynamics
        { "CLA-2A", "Effect" },
        { "CLA-3A", "Effect" },
        { "CLA-76", "Effect" },
        { "API 2500", "Effect" },
        { "SSL G-Master Buss Compressor", "Effect" },
        { "H-Comp Hybrid Compressor", "Effect" },
        { "Renaissance Compressor", "Effect" },
        { "Renaissance Vox", "Effect" },
        { "C1 Compressor", "Effect" },
        { "C4 Multiband Compressor", "Effect" },
        { "C6 Multiband Compressor", "Effect" },
        { "MV2", "Effect" },
        { "Smack Attack", "Effect" },
        { "Trans-X", "Effect" },
        { "L1 Ultramaximizer", "Effect" },
        { "L2 Ultramaximizer", "Effect" },
        { "L3 Multimaximizer", "Effect" },
        { "L3-LL Multimaximizer", "Effect" },
        { "L3-16 Multimaximizer", "Effect" },
        // EQs / channel strips
        { "SSL E-Channel", "Effect" },
        { "SSL G-Channel", "Effect" },
        { "SSL G-Equalizer", "Effect" },
        { "API 550", "Effect" },
        { "API 560", "Effect" },
        { "PuigTec EQP-1A", "Effect" },
        { "PuigTec MEQ-5", "Effect" },
        { "Renaissance Equalizer", "Effect" },
        { "Q10 Equalizer", "Effect" },
        { "H-EQ Hybrid Equalizer", "Effect" },
        { "F6 Floating-Band Dynamic EQ", "Effect" },
        { "Scheps 73", "Effect" },
        { "Scheps Omni Channel", "Effect" },
        { "CLA MixHub", "Effect" },
        // Saturation / character
        { "J37 Tape", "Effect" },
        { "Kramer Master Tape", "Effect" },
        { "Abbey Road Saturator", "Effect" },
        { "NLS Non-Linear Summer", "Effect" },
        { "Aphex Vintage Aural Exciter", "Effect" },
        // Reverb / delay / modulation
        { "H-Reverb Hybrid Reverb", "Effect" },
        { "H-Delay Hybrid Delay", "Effect" },
        { "Renaissance Reverb", "Effect" },
        { "TrueVerb", "Effect" },
        { "IR-L Convolution Reverb", "Effect" },
        { "Abbey Road Chambers", "Effect" },
        { "SuperTap", "Effect" },
        { "MetaFlanger", "Effect" },
        // Pitch / tuning / vocal
        { "Waves Tune Real-Time", "Effect" },
        { "Waves Tune", "Effect" },
        { "Doubler", "Effect" },
        { "Vocal Rider", "Effect" },
        { "DeEsser", "Effect" },
        { "Sibilance", "Effect" },
        // Utility / metering / restoration
        { "WLM Plus Loudness Meter", "Effect" },
        { "PAZ Analyzer", "Effect" },
        { "X-Noise", "Effect" },
        { "X-Hum", "Effect" },
        { "Z-Noise", "Effect" },
        { "S1 Stereo Imager", "Effect" },
        { "Center", "Effect" },
        { "Brauer Motion", "Effect" },
        // Bus / mastering chains
        { "Abbey Road TG Mastering Chain", "Effect" },
        { "Waves Tune LT", "Effect" },
        { "StudioRack", "Effect" },
        { "CLA Vocals", "Effect" },
        { "CLA Bass", "Effect" },
        { "CLA Drums", "Effect" },
        { "CLA Guitars", "Effect" },
        { "CLA Effects", "Effect" },
        { "CLA Unplugged", "Effect" },
    };
    return c;
}

// ----------------------------------------------------------------------------
// LOGIC PRO (Apple) — macOS only
// ----------------------------------------------------------------------------
inline const std::vector<CatalogEntry>& logicStockCatalog()
{
    static const std::vector<CatalogEntry> c = {
        { "Channel EQ", "Effect" },
        { "Vintage EQ Collection", "Effect" },
        { "Linear Phase EQ", "Effect" },
        { "Single Band EQ", "Effect" },
        { "Match EQ", "Effect" },
        { "Compressor", "Effect" },
        { "Vintage Opto", "Effect" },
        { "Vintage VCA", "Effect" },
        { "Vintage FET", "Effect" },
        { "Limiter", "Effect" },
        { "Adaptive Limiter", "Effect" },
        { "Multipressor", "Effect" },
        { "Multiband Dynamics", "Effect" },
        { "DeEsser 2", "Effect" },
        { "Noise Gate", "Effect" },
        { "Enveloper", "Effect" },
        { "ChromaVerb", "Effect" },
        { "Space Designer", "Effect" },
        { "Quantec Room Simulator", "Effect" },
        { "Stereo Delay", "Effect" },
        { "Tape Delay", "Effect" },
        { "Echo", "Effect" },
        { "Tremolo", "Effect" },
        { "Chorus", "Effect" },
        { "Ensemble", "Effect" },
        { "Phaser", "Effect" },
        { "Modulation Delay", "Effect" },
        { "Overdrive", "Effect" },
        { "Bitcrusher", "Effect" },
        { "Clip Distortion", "Effect" },
        { "Distortion", "Effect" },
        { "Phat FX", "Effect" },
        { "Vintage Console EQ", "Effect" },
        { "Vintage Graphic EQ", "Effect" },
        { "Vintage Tube EQ", "Effect" },
        { "Tape", "Effect" },
        { "Exciter", "Effect" },
        { "Stereo Spread", "Effect" },
        { "Direction Mixer", "Effect" },
        { "Gain", "Effect" },
        { "Loudness Meter", "Effect" },
        { "Pitch Correction", "Effect" },
        { "Pitch Shifter", "Effect" },
        { "Vocal Transformer", "Effect" },
    };
    return c;
}

// ----------------------------------------------------------------------------
// ABLETON LIVE
// ----------------------------------------------------------------------------
inline const std::vector<CatalogEntry>& abletonStockCatalog()
{
    static const std::vector<CatalogEntry> c = {
        { "EQ Eight", "Effect" },
        { "EQ Three", "Effect" },
        { "Channel EQ", "Effect" },
        { "Glue Compressor", "Effect" },
        { "Compressor", "Effect" },
        { "Multiband Dynamics", "Effect" },
        { "Limiter", "Effect" },
        { "Gate", "Effect" },
        { "Drum Buss", "Effect" },
        { "Saturator", "Effect" },
        { "Overdrive", "Effect" },
        { "Dynamic Tube", "Effect" },
        { "Amp", "Effect" },
        { "Cabinet", "Effect" },
        { "Pedal", "Effect" },
        { "Roar", "Effect" },
        { "Reverb", "Effect" },
        { "Hybrid Reverb", "Effect" },
        { "Echo", "Effect" },
        { "Delay", "Effect" },
        { "Filter Delay", "Effect" },
        { "Chorus-Ensemble", "Effect" },
        { "Phaser-Flanger", "Effect" },
        { "Auto Filter", "Effect" },
        { "Auto Pan", "Effect" },
        { "Utility", "Effect" },
        { "Spectrum", "Effect" },
        { "Tuner", "Effect" },
        { "Redux", "Effect" },
        { "Erosion", "Effect" },
        { "Vinyl Distortion", "Effect" },
        { "Corpus", "Effect" },
        { "Resonators", "Effect" },
        { "Frequency Shifter", "Effect" },
        { "Grain Delay", "Effect" },
        { "Beat Repeat", "Effect" },
        { "Gated Delay", "Effect" },
        { "Spectral Resonator", "Effect" },
        { "Spectral Time", "Effect" },
        { "Shifter", "Effect" },
    };
    return c;
}

// ----------------------------------------------------------------------------
// FL STUDIO (Image-Line)
// ----------------------------------------------------------------------------
inline const std::vector<CatalogEntry>& flStudioStockCatalog()
{
    static const std::vector<CatalogEntry> c = {
        { "Fruity Parametric EQ 2", "Effect" },
        { "Fruity Parametric EQ", "Effect" },
        { "Fruity 7 Band EQ", "Effect" },
        { "Fruity Compressor", "Effect" },
        { "Fruity Limiter", "Effect" },
        { "Maximus", "Effect" },
        { "Fruity Multiband Compressor", "Effect" },
        { "Fruity Soft Clipper", "Effect" },
        { "Fruity Fast Dist", "Effect" },
        { "Fruity Blood Overdrive", "Effect" },
        { "Fruity WaveShaper", "Effect" },
        { "Fruity Reeverb 2", "Effect" },
        { "Fruity Convolver", "Effect" },
        { "Fruity Delay 3", "Effect" },
        { "Fruity Delay Bank", "Effect" },
        { "Fruity Flanger", "Effect" },
        { "Fruity Chorus", "Effect" },
        { "Fruity Phaser", "Effect" },
        { "Fruity Flangus", "Effect" },
        { "Fruity Stereo Enhancer", "Effect" },
        { "Fruity Stereo Shaper", "Effect" },
        { "Fruity Balance", "Effect" },
        { "Fruity Panomatic", "Effect" },
        { "Fruity Love Philter", "Effect" },
        { "Fruity Filter", "Effect" },
        { "Fruity Fast LP", "Effect" },
        { "Fruity Vocoder", "Effect" },
        { "Fruity Spectroman", "Effect" },
        { "Wave Candy", "Effect" },
        { "Fruity HP / LP", "Effect" },
        { "Gross Beat", "Effect" },
        { "Fruity Squeeze", "Effect" },
        { "Soundgoodizer", "Effect" },
        { "Transient Processor", "Effect" },
        { "Pitch Shifter", "Effect" },
        { "Fruity Pitcher", "Effect" },
        { "Vintage Chorus", "Effect" },
        { "Vintage Phaser", "Effect" },
    };
    return c;
}

// ----------------------------------------------------------------------------
// PRO TOOLS (Avid) — stock AAX. Note: EchoJay's scanner is VST3/AU/VST, so
// these are never seen by a folder scan even on a Pro Tools machine.
// ----------------------------------------------------------------------------
inline const std::vector<CatalogEntry>& proToolsStockCatalog()
{
    static const std::vector<CatalogEntry> c = {
        { "EQ3 1-Band", "Effect" },
        { "EQ3 7-Band", "Effect" },
        { "Dyn3 Compressor/Limiter", "Effect" },
        { "Dyn3 Expander/Gate", "Effect" },
        { "Dyn3 De-Esser", "Effect" },
        { "Channel Strip", "Effect" },
        { "Pro Compressor", "Effect" },
        { "Pro Limiter", "Effect" },
        { "Pro Multiband Dynamics", "Effect" },
        { "Pro Expander", "Effect" },
        { "Maxim", "Effect" },
        { "BF-76", "Effect" },
        { "BF-3A", "Effect" },
        { "Purple Audio MC77", "Effect" },
        { "D-Verb", "Effect" },
        { "Space (Reverb)", "Effect" },
        { "Mod Delay III", "Effect" },
        { "Air Reverb", "Effect" },
        { "Air Dynamic Delay", "Effect" },
        { "Air Chorus", "Effect" },
        { "Air Flanger", "Effect" },
        { "Air Phaser", "Effect" },
        { "Air Multi-Chorus", "Effect" },
        { "Air Stereo Width", "Effect" },
        { "Air Enhancer", "Effect" },
        { "Lo-Fi", "Effect" },
        { "Recti-Fi", "Effect" },
        { "Sci-Fi", "Effect" },
        { "Vari-Fi", "Effect" },
        { "Trim", "Effect" },
        { "Signal Generator", "Effect" },
    };
    return c;
}

// ----------------------------------------------------------------------------
// STUDIO ONE (PreSonus)
// ----------------------------------------------------------------------------
inline const std::vector<CatalogEntry>& studioOneStockCatalog()
{
    static const std::vector<CatalogEntry> c = {
        { "Pro EQ3", "Effect" },
        { "Channel Strip", "Effect" },
        { "Fat Channel", "Effect" },
        { "Compressor", "Effect" },
        { "Tricomp", "Effect" },
        { "Multiband Dynamics", "Effect" },
        { "Limiter2", "Effect" },
        { "Expander", "Effect" },
        { "Gate", "Effect" },
        { "De-Esser", "Effect" },
        { "Ampire", "Effect" },
        { "RedlightDist", "Effect" },
        { "Console Shaper", "Effect" },
        { "Open AIR", "Effect" },
        { "Room Reverb", "Effect" },
        { "Mixverb", "Effect" },
        { "Analog Delay", "Effect" },
        { "Beat Delay", "Effect" },
        { "Groove Delay", "Effect" },
        { "Chorus", "Effect" },
        { "Flanger", "Effect" },
        { "Phaser", "Effect" },
        { "X-Trem", "Effect" },
        { "Autofilter", "Effect" },
        { "Binaural Pan", "Effect" },
        { "Dual Pan", "Effect" },
        { "Tuner", "Effect" },
        { "Level Meter", "Effect" },
        { "Spectrum Meter", "Effect" },
    };
    return c;
}

// ----------------------------------------------------------------------------
// CUBASE / NUENDO (Steinberg)
// ----------------------------------------------------------------------------
inline const std::vector<CatalogEntry>& cubaseStockCatalog()
{
    static const std::vector<CatalogEntry> c = {
        { "Frequency", "Effect" },
        { "StudioEQ", "Effect" },
        { "GEQ-30", "Effect" },
        { "Compressor", "Effect" },
        { "Tube Compressor", "Effect" },
        { "Vintage Compressor", "Effect" },
        { "VintageCompressor", "Effect" },
        { "Multiband Compressor", "Effect" },
        { "Maximizer", "Effect" },
        { "Limiter", "Effect" },
        { "Brickwall Limiter", "Effect" },
        { "Expander", "Effect" },
        { "Gate", "Effect" },
        { "DeEsser", "Effect" },
        { "Envelope Shaper", "Effect" },
        { "MultibandEnvelopeShaper", "Effect" },
        { "Magneto II", "Effect" },
        { "Quadrafuzz v2", "Effect" },
        { "Distortion", "Effect" },
        { "REVerence", "Effect" },
        { "RoomWorks", "Effect" },
        { "RoomWorks SE", "Effect" },
        { "ModMachine", "Effect" },
        { "MonoDelay", "Effect" },
        { "StereoDelay", "Effect" },
        { "PingPongDelay", "Effect" },
        { "Studio Chorus", "Effect" },
        { "Flanger", "Effect" },
        { "Phaser", "Effect" },
        { "Chopper", "Effect" },
        { "StereoEnhancer", "Effect" },
        { "MorphFilter", "Effect" },
        { "SuperVision", "Effect" },
    };
    return c;
}

// ----------------------------------------------------------------------------
// Manufacturer resolution
// ----------------------------------------------------------------------------
// Some vendors organise their plugins into CATEGORY subfolders on disk (UAD is
// the big one: .../Powered Plug-Ins/Guitar and Bass/UAD Ampeg B15N.component).
// The scanner's "parent folder = manufacturer" guess then wrongly yields
// "Guitar and Bass", "Equalizers", "Filter" as the manufacturer. These helpers
// recover the real vendor:
//   1. If the plugin NAME starts with a known brand token, use that brand.
//   2. Else if the parent-folder guess is actually a category word, reject it.
// Returns empty if nothing better than the (rejected) guess is found, so the
// caller can decide a fallback.

// Known brand prefixes that appear at the START of plugin display names.
inline const std::vector<std::pair<const char*, const char*>>& brandNamePrefixes()
{
    // { lowercase prefix to match at start of name, canonical manufacturer }
    static const std::vector<std::pair<const char*, const char*>> p = {
        { "uad ",        "UAD" },
        { "uadx ",       "UAD" },
        { "fabfilter",   "FabFilter" },
        { "soundtoys",   "Soundtoys" },
        { "valhalla",    "Valhalla DSP" },
        { "izotope",     "iZotope" },
        { "oeksound",    "oeksound" },
        { "soothe",      "oeksound" },
        { "slate ",      "Slate Digital" },
        { "ssl ",        "Solid State Logic" },
        { "waves ",      "Waves" },
        { "arturia",     "Arturia" },
        { "native instruments", "Native Instruments" },
        { "kontakt",     "Native Instruments" },
        { "plugin alliance", "Plugin Alliance" },
        { "brainworx",   "Brainworx" },
        { "bx_",         "Brainworx" },
        { "eventide",    "Eventide" },
        { "lexicon",     "Lexicon" },
        { "celemony",    "Celemony" },
        { "melodyne",    "Celemony" },
        { "antares",     "Antares" },
        { "auto-tune",   "Antares" },
        { "sonnox",      "Sonnox" },
        { "tokyo dawn",  "Tokyo Dawn Labs" },
        { "tdr ",        "Tokyo Dawn Labs" },
        { "kilohearts",  "Kilohearts" },
        { "xfer",        "Xfer Records" },
        { "serum",       "Xfer Records" },
    };
    return p;
}

// Category-ish folder names that must NOT be treated as a manufacturer. These
// are the subfolder names vendors (esp. UAD) use to organise plugins by type.
inline bool looksLikeCategoryFolder(const juce::String& folder)
{
    static const char* cats[] = {
        "equalizers", "equalizer", "eq", "dynamics", "compressors", "compressor",
        "limiters", "reverbs", "reverb", "delays", "delay", "modulation",
        "guitar and bass", "guitar", "bass", "channel strips", "channel strip",
        "preamps", "preamp", "saturation", "distortion", "tape", "mastering",
        "metering", "analyzers", "utility", "filter", "filters", "pitch",
        "vocal", "vocals", "drums", "effects", "fx", "instruments", "synths",
        "mixing", "mix", "tools", "special", "creative", "stereo", "imaging",
        "powered plug-ins", "uad plug-ins", "native"
    };
    auto f = folder.toLowerCase().trim();
    for (auto* c : cats)
        if (f == c)
            return true;
    return false;
}

// Resolve a manufacturer given the plugin display name and the parent-folder
// guess. Returns a corrected manufacturer, or empty if no improvement (caller
// keeps its own fallback).
inline juce::String resolveManufacturer(const juce::String& pluginName,
                                        const juce::String& folderGuess)
{
    auto lname = pluginName.toLowerCase();

    // 1. Brand prefix in the name is the strongest signal.
    for (const auto& bp : brandNamePrefixes())
        if (lname.startsWith(bp.first))
            return juce::String(bp.second);

    // 2. If the folder guess is a category, it's not a manufacturer. Try the
    //    first word of the plugin name as a last resort (e.g. "Acustica ...").
    if (looksLikeCategoryFolder(folderGuess))
    {
        auto firstWord = pluginName.upToFirstOccurrenceOf(" ", false, false).trim();
        // Only use it if it looks like a brand (starts uppercase, >1 char) and
        // isn't itself a category word.
        if (firstWord.length() > 1 && ! looksLikeCategoryFolder(firstWord))
            return firstWord;
        return "Unknown";
    }

    // 3. Folder guess is fine as-is.
    return {};
}

// Canonicalise a manufacturer string so the SAME vendor always keys to the
// SAME label regardless of source (AU registry, VST3 moduleinfo Vendor, folder
// name) or casing. This is what merges the "Soundtoys" / "SoundToys" split and
// unifies "UAD" / "UADx" / "Universal Audio". Unknown vendors are returned
// trimmed with their own casing preserved (the registry and moduleinfo already
// give good casing for those, e.g. "Acon Digital", "AnalogObsession").
inline juce::String canonicaliseManufacturer(const juce::String& rawIn)
{
    auto raw = rawIn.trim();
    if (raw.isEmpty()) return "Unknown";

    // Fold for matching only: lowercase, drop commas/periods, collapse spaces.
    auto f = raw.toLowerCase().removeCharacters(",.").trim();
    while (f.contains("  ")) f = f.replace("  ", " ");

    struct Alias { const char* key; const char* canon; };
    static const Alias map[] = {
        { "soundtoys",            "Soundtoys" },
        { "universal audio",      "Universal Audio" },
        { "universal audio inc",  "Universal Audio" },
        { "uad",                  "Universal Audio" },
        { "uadx",                 "Universal Audio" },
        { "uad powered plug-ins", "Universal Audio" },
        { "valhalla",             "Valhalla DSP" },
        { "valhalla dsp",         "Valhalla DSP" },
        { "valhalla dsp llc",     "Valhalla DSP" },
        { "izotope",              "iZotope" },
        { "izotope inc",          "iZotope" },
        { "fabfilter",            "FabFilter" },
        { "solid state logic",    "Solid State Logic" },
        { "ssl",                  "Solid State Logic" },
        { "melda",                "MeldaProduction" },
        { "meldaproduction",      "MeldaProduction" },
        { "melda production",     "MeldaProduction" },
        { "tokyo dawn labs",      "Tokyo Dawn Labs" },
        { "tokyo dawn records",   "Tokyo Dawn Labs" },
        { "tdr",                  "Tokyo Dawn Labs" },
        { "xfer",                 "Xfer Records" },
        { "xfer records",         "Xfer Records" },
        { "native instruments",   "Native Instruments" },
        { "u-he",                 "u-he" },
        { "u he",                 "u-he" },
        { "tc electronic",        "TC Electronic" },
        { "plugin alliance",      "Plugin Alliance" },
        { "brainworx",            "Brainworx" },
        { "oeksound",             "oeksound" },
        { "antares",              "Antares" },
        { "celemony",             "Celemony" },
        { "arturia",              "Arturia" },
        { "slate digital",        "Slate Digital" },
        { "eventide",             "Eventide" },
        { "kilohearts",           "Kilohearts" },
        { "sonnox",               "Sonnox" },
        // Domain-token aliases (from CFBundleIdentifier reverse-DNS), for
        // vendors whose bundle-id token doesn't title-case to their real name.
        { "outputinc",            "Output" },
        { "output",               "Output" },
        { "acustica audio",       "Acustica Audio" },
        { "acusticaaudio",        "Acustica Audio" },
        { "masteringthemix",      "Mastering The Mix" },
        { "mastering the mix",    "Mastering The Mix" },
        { "adsrsounds",           "ADSR" },
        { "adsr",                 "ADSR" },
        { "nativeinstruments",    "Native Instruments" },
        { "xferrecords",          "Xfer Records" },
    };
    for (auto& m : map)
        if (f == m.key)
            return juce::String(m.canon);

    return raw; // unknown vendor: keep its own (already decent) casing
}

// Collapse mono/stereo (and similar) channel variants to a single base name so
// "Pultec EQP-1A (Mono)" and "Pultec EQP-1A (Stereo)" dedupe to one entry. Only
// trailing PARENTHETICAL channel tokens are stripped, so real names that merely
// contain the words (e.g. "Precision K-Stereo", "Mono Maker") are never
// touched. Returns the trimmed base name. NOTE: only the parenthetical form is
// handled here; if a vendor uses a bare trailing word or an m/s suffix instead,
// extend the token list once that exact pattern is confirmed.
// ---------------------------------------------------------------------------
// THE one uid construction point (13 Aug 2026). The tick-state identity was
// built inline at three sites with DIFFERENT normalization depths, and the
// catalog applies 84 rewrite rules (29 brand prefixes, 46 manufacturer
// aliases, 9 channel-variant tokens, plus the category-folder heuristic), so
// any site skipping part of the pipeline mints a second identity for the
// same plugin. Measured live as the Brainworx 56: scan-era uids under
// "plugin_alliance", load-era under "brainworx", one disabled set holding
// both, tick semantics flipping per session era. Every uid goes through
// here, with the FULL pipeline, and the pipeline is idempotent so raw and
// already-normalized inputs produce the same uid (pinned in mapfps_test).
// ---------------------------------------------------------------------------
inline juce::String makeUid(const juce::String& rawName,
                            const juce::String& rawManufacturer);

inline juce::String stripChannelVariant(const juce::String& nameIn)
{
    auto n = nameIn.trim();
    static const char* toks[] = {
        " (mono)", " (stereo)", " (m)", " (s)",
        " (dual mono)", " (stereo to mono)", " (mono to stereo)",
        " (st)", " (mo)"
    };
    bool changed = true;
    while (changed)
    {
        changed = false;
        auto low = n.toLowerCase();
        for (auto* t : toks)
        {
            juce::String tok(t);
            if (low.endsWith(tok))
            {
                n = n.dropLastCharacters(tok.length()).trim();
                changed = true;
                break;
            }
        }
    }
    return n;
}

inline juce::String makeUid(const juce::String& rawName,
                            const juce::String& rawManufacturer)
{
    const auto name = stripChannelVariant(rawName);
    auto manu = rawManufacturer;
    if (auto corrected = resolveManufacturer(name, manu); corrected.isNotEmpty())
        manu = corrected;
    manu = canonicaliseManufacturer(manu);
    return name.toLowerCase().replaceCharacter(' ', '_') + "_"
         + manu.toLowerCase().replaceCharacter(' ', '_');
}

// Derive a manufacturer from a reverse-DNS CFBundleIdentifier (e.g.
// "com.meldaproduction.MUtility" -> "MeldaProduction") for plugins that have no
// vendor from any other source. The vendor is the token after the leading TLD.
// Generic/default identifiers and any token that just repeats the plugin name
// are rejected (returns empty so the caller keeps "Unknown").
inline juce::String vendorFromBundleId(const juce::String& bundleId,
                                       const juce::String& pluginName)
{
    auto parts = juce::StringArray::fromTokens(bundleId, ".", "");
    if (parts.size() < 2) return {};

    static const char* tlds[] = { "com","net","org","io","co","de","uk","fr",
                                  "us","app","audio","music","eu","ltd","inc" };
    int idx = 0;
    auto first = parts[0].toLowerCase().trim();
    for (auto* t : tlds) if (first == t) { idx = 1; break; }
    if (idx >= parts.size()) return {};

    auto token = parts[idx].trim();
    if (token.isEmpty()) return {};

    auto tl = token.toLowerCase();
    static const char* generic[] = { "yourcompany","mycompany","company","example",
                                     "test","audio","plugin","plugins","vst","vst3",
                                     "audiounit","juce","unknown","app" };
    for (auto* g : generic) if (tl == g) return {};

    // hyphens/underscores -> spaces, then title-case each word.
    auto disp = token.replaceCharacter('-', ' ').replaceCharacter('_', ' ');
    juce::String titled;
    bool startOfWord = true;
    for (int i = 0; i < disp.length(); ++i)
    {
        auto c = disp[i];
        if (c == ' ') { titled << ' '; startOfWord = true; }
        else { titled << (startOfWord ? juce::CharacterFunctions::toUpperCase(c) : c);
               startOfWord = false; }
    }

    auto result = canonicaliseManufacturer(titled);

    // Never let the derived vendor just echo the plugin name.
    auto na = [] (const juce::String& s)
    {
        auto low = s.toLowerCase();
        juce::String o;
        for (int i = 0; i < low.length(); ++i)
            if (juce::CharacterFunctions::isLetterOrDigit(low[i]))
                o << low[i];
        return o;
    };
    if (na(result) == na(pluginName)) return {};
    return result;
}

} // namespace echojay
