/*
    ProjectArt.h

    Procedural project artwork for the EchoJay Dashboard tab.

    Soft mesh gradient: a saturated base plus 4 to 6 radial colour blobs in a
    tight hue family.

    MIRRORS lib/dash/art.ts EXACTLY. The PRNG below is the same mulberry32 and
    MUST be advanced in the same order, because the order of rng() calls IS the
    algorithm. Change one side, change the other in the same commit.

    Rules carried from the spec:
      - Never fetch a rendered image. The payload sends art_seed, we draw it.
      - art_seed is immutable. Renaming a project must not change its artwork.
*/

#pragma once

#include <JuceHeader.h>

namespace echojay
{

struct ArtBlob
{
    float h = 0.0f, s = 0.0f, l = 0.0f;   // hue degrees, sat 0..1, light 0..1
    float cx = 0.5f, cy = 0.5f, r = 0.5f; // fractions of the tile
};

struct ProjectArtParams
{
    int   hueAnchor = 0;
    float spread    = 0.0f;
    float baseH = 0.0f, baseS = 0.90f, baseL = 0.60f;
    juce::Array<ArtBlob> blobs;           // 4 to 6
};

class ProjectArt
{
public:
    /** Explicit HSL to RGB. Do NOT swap for juce::Colour::fromHSL, it rounds
        differently from the TypeScript and the renderers would diverge.
        h in degrees (wrapped), s and l in [0,1]. */
    static juce::Colour hslToColour (float h, float s, float l);

    /** Derives drawing parameters from a 16 lowercase hex char seed.
        Returns a stable fallback and asserts if the seed is malformed, because a
        bad seed must never take down a paint pass. */
    static ProjectArtParams derive (const juce::String& seedHex);

    /** Draws the tile. Square bounds expected, non-square is centre-cropped. */
    static void draw (juce::Graphics& g,
                      const juce::String& seedHex,
                      juce::Rectangle<int> bounds,
                      float cornerRadius = 10.0f);

    /** Cached tile for the seed at the given edge length. Message thread only. */
    static juce::Image getCached (const juce::String& seedHex, int edgePx);

    /** Drops every cached tile. Call on look-and-feel or display scale change. */
    static void clearCache();

private:
    static constexpr int kMaxCacheEntries = 64;

    // The D0 drop wrote JUCE_DECLARE_NON_CONSTRUCTABLE_WITH_STATIC_MEMBERS
    // here. THAT MACRO DOES NOT EXIST, in JUCE 8.0.12 or anywhere: a grep over
    // modules/ finds nothing. The file failed to compile the first time it was
    // ever added to a target, which is proof that this port, described in the
    // specs as ported byte-for-byte and validated, had never been through a
    // compiler.
    //
    // The intent is clear and is preserved directly: this class is a namespace
    // of static functions and must never be instantiated.
    ProjectArt() = delete;
    ~ProjectArt() = delete;
    JUCE_DECLARE_NON_COPYABLE (ProjectArt)
};

} // namespace echojay
