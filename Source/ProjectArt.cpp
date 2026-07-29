/*
    ProjectArt.cpp

    MIRRORS lib/dash/art.ts. Keep the two in step, especially the ORDER of
    nextFloat() calls in derive().
*/

#include "ProjectArt.h"

namespace echojay
{

//==============================================================================
// mulberry32. Exact uint32 arithmetic, matching the TypeScript bit for bit.

namespace
{
    struct Mulberry32
    {
        juce::uint32 a;

        explicit Mulberry32 (juce::uint32 seed) : a (seed) {}

        float next()
        {
            a += 0x6d2b79f5u;
            juce::uint32 t = a;
            t = (t ^ (t >> 15)) * (t | 1u);
            t ^= t + (t ^ (t >> 7)) * (t | 61u);
            return (float) (double) ((t ^ (t >> 14))) / 4294967296.0;
        }
    };
}

//==============================================================================

juce::Colour ProjectArt::hslToColour (float h, float s, float l)
{
    h = std::fmod (std::fmod (h, 360.0f) + 360.0f, 360.0f);

    const float c  = (1.0f - std::abs (2.0f * l - 1.0f)) * s;
    const float hp = h / 60.0f;
    const float x  = c * (1.0f - std::abs (std::fmod (hp, 2.0f) - 1.0f));

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if      (hp < 1.0f) { r = c; g = x; b = 0.0f; }
    else if (hp < 2.0f) { r = x; g = c; b = 0.0f; }
    else if (hp < 3.0f) { r = 0.0f; g = c; b = x; }
    else if (hp < 4.0f) { r = 0.0f; g = x; b = c; }
    else if (hp < 5.0f) { r = x; g = 0.0f; b = c; }
    else                { r = c; g = 0.0f; b = x; }

    const float m = l - c * 0.5f;

    auto to8 = [m] (float v) -> juce::uint8
    {
        return (juce::uint8) juce::jlimit (0, 255, (int) std::round ((v + m) * 255.0f));
    };

    return juce::Colour (to8 (r), to8 (g), to8 (b));
}

//==============================================================================

ProjectArtParams ProjectArt::derive (const juce::String& seedHex)
{
    ProjectArtParams p;

    if (seedHex.length() != 16 || ! seedHex.containsOnly ("0123456789abcdef"))
    {
        jassertfalse;
        p.hueAnchor = 200;
        p.baseH = 200.0f;
        ArtBlob b; b.h = 200.0f; b.s = 0.9f; b.l = 0.6f;
        p.blobs.add (b);
        return p;
    }

    const juce::uint32 seed32 = (juce::uint32) seedHex.substring (0, 8).getHexValue64();
    Mulberry32 rng (seed32);

    // ORDER IS THE ALGORITHM. Do not reorder or insert calls.
    p.hueAnchor = (int) std::floor (rng.next() * 360.0f);
    p.spread    = 12.0f + rng.next() * 34.0f;
    const int blobCount = 4 + (int) std::floor (rng.next() * 3.0f);

    p.baseH = (float) p.hueAnchor;
    p.baseS = 0.90f;
    p.baseL = 0.60f;

    for (int i = 0; i < blobCount; ++i)
    {
        ArtBlob b;
        b.h  = (float) p.hueAnchor + (rng.next() * 2.0f - 1.0f) * p.spread;
        b.s  = 0.84f + rng.next() * 0.15f;
        b.l  = 0.54f + rng.next() * 0.14f;
        b.cx = 0.08f + rng.next() * 0.84f;
        b.cy = 0.08f + rng.next() * 0.84f;
        b.r  = 0.42f + rng.next() * 0.46f;
        p.blobs.add (b);
    }

    return p;
}

//==============================================================================

void ProjectArt::draw (juce::Graphics& g,
                       const juce::String& seedHex,
                       juce::Rectangle<int> bounds,
                       float cornerRadius)
{
    if (bounds.isEmpty())
        return;

    const int edge = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto square = bounds.withSizeKeepingCentre (edge, edge).toFloat();
    const auto p = derive (seedHex);

    juce::Path clip;
    clip.addRoundedRectangle (square, cornerRadius);

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (clip);

    g.setColour (hslToColour (p.baseH, p.baseS, p.baseL));
    g.fillRect (square);

    const float S = (float) edge;
    const float ox = square.getX(), oy = square.getY();

    for (const auto& b : p.blobs)
    {
        const auto col = hslToColour (b.h, b.s, b.l);
        const float cx = ox + b.cx * S;
        const float cy = oy + b.cy * S;
        const float r  = b.r * S;

        // Radial, centre opaque to edge transparent. Mid stop at 0.45 matches the
        // SVG stop list, without it the falloff reads harder than the web tile.
        juce::ColourGradient grad (col.withAlpha (1.0f), cx, cy,
                                   col.withAlpha (0.0f), cx + r, cy, true);
        grad.addColour (0.45, col.withAlpha (0.72f));

        g.setGradientFill (grad);
        g.fillRect (square);
    }
}

//==============================================================================

namespace
{
    struct TileCache
    {
        juce::HashMap<juce::String, juce::Image> images;
        juce::StringArray order;   // insertion order, oldest first
    };

    TileCache& cache()
    {
        static TileCache c;
        return c;
    }
}

juce::Image ProjectArt::getCached (const juce::String& seedHex, int edgePx)
{
    // Message thread only. Deliberately unlocked: a lock would hide an accidental
    // audio-thread call rather than surface it.
    JUCE_ASSERT_MESSAGE_THREAD

    if (edgePx <= 0)
        return {};

    const auto key = seedHex + "@" + juce::String (edgePx);
    auto& c = cache();

    if (c.images.contains (key))
        return c.images[key];

    juce::Image img (juce::Image::ARGB, edgePx, edgePx, true);
    {
        juce::Graphics g (img);
        draw (g, seedHex, juce::Rectangle<int> (0, 0, edgePx, edgePx));
    }

    while (c.order.size() >= kMaxCacheEntries)
    {
        c.images.remove (c.order[0]);
        c.order.remove (0);
    }

    c.images.set (key, img);
    c.order.add (key);
    return img;
}

void ProjectArt::clearCache()
{
    JUCE_ASSERT_MESSAGE_THREAD
    cache().images.clear();
    cache().order.clear();
}

} // namespace echojay
