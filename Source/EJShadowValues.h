#pragma once
// =============================================================================================
//  EJShadowValues.h - FOLLOW layer 2 (24 Oct 2026): the live copy FOLLOWS the real plugin's
//                     parameters, thirty times a second
// =============================================================================================
//  Sean, after layer 1 passed at 20:40: *"build layer 2 (the 30 Hz parameter mirror: Link
//  publishes the tapped slot's parameter values, the V2 writes only the changed ones into the
//  copy) ... change a knob on the Link's own window with the V2's view open, and the view should
//  follow within a moment."*
//
//  ---- WHY A FILE OF ITS OWN, AND NOT A FIELD ON SOMETHING THAT EXISTS -------------------------
//
//  LinkMeterFrame is 128 bytes and its size IS the stride of the array it lives in - its own
//  header says "SHORT90 must not have grown the frame - the size IS the stride" - and the audio
//  ring's header is frozen field by field against every installed binary. Neither can carry 256
//  floats. A small file of its own costs one mmap, moves no shipped layout, and its ABSENCE is
//  exactly the right answer for an older Link: the V2 finds no file, says the mirror is not
//  available from that Link, and keeps the layer-1 picture. That is the downgrade rule this house
//  already follows for every additive field.
//
//  ONE NAME, FROM THE UID, used by BOTH sides (shadowValuesFileNameFor). The audio ring is named
//  by the Link from its uid and derived by the V2 from the registry's audioFile string - two
//  derivations of one fact, which works and which nobody should copy. This has one.
//
//  ---- THE SHAPE OF THE PROMISE ----------------------------------------------------------------
//
//  The LINK IS THE AUTHORITY. The copy is a picture; when the two disagree, the copy is corrected.
//  That is the same rule layer 1 follows and the opposite of what today's build does (which has no
//  authority at all, because nothing travels).
//
//  WRITTEN BY THE LINK'S MESSAGE THREAD, READ BY THE V2'S MESSAGE THREAD. No audio thread touches
//  this file in either process: a parameter write is a plugin call and can never be made
//  real-time safe (LinkProcessor's own ruling on kSetParam), and the Link's timer already runs at
//  30 Hz, so the publish rides a tick that exists.
//
//  CHANGED-ONLY, so steady state costs the plugin NOTHING. The V2 keeps what it last applied and
//  writes only what moved. A mirror that wrote every value every tick would be 256 plugin calls
//  per tick into an instance whose interface is open - which is the shape of trouble, not a
//  mirror.
//
//  ---- THE DIGEST, BECAUSE AN INDEX IS NOT AN IDENTITY ----------------------------------------
//
//  The values travel by the parameter's own index, which is only meaningful if both processes are
//  holding the same plugin, the same version, with the same parameter list. A bare index would be
//  the proxy-for-an-identity family this house has paid for more than once. So the Link publishes
//  a digest of its parameter list (the count, and a hash of the names) and the V2 computes the
//  same digest over its copy; if they disagree the mirror REFUSES WITH A LINE rather than applying
//  somebody else's values to the wrong knobs.
//
//  ---- WHAT IT DOES NOT CARRY -----------------------------------------------------------------
//
//  A change a plugin makes without reporting a parameter - a graph drag, an IR load, a preset
//  recall - moves nothing here. That is layer 3 (the state digest and the re-pull), and until it
//  exists the honest statement is that this mirror follows PARAMETERS. It is not a claim to follow
//  everything.
#include <JuceHeader.h>
#include <functional>
#include "LinkShm.h"
#include <cstring>
#include <cmath>

namespace echojay
{

// ---- THE LAYOUT, FROZEN THE WAY EVERY OTHER SHARED STRUCTURE HERE IS ------------------------

static constexpr juce::uint32 kShadowValMagic   = 0xEC4A5001u;
static constexpr juce::uint32 kShadowValVersion = 1u;
/** How many parameters one frame carries. 256 covers every plugin in his rack with room to spare;
    a plugin with more publishes its first 256 and says so in `paramCount`, which the V2 reads and
    reports rather than silently mirroring a prefix as if it were the whole plugin. */
static constexpr int kShadowValMax = 256;

struct alignas(64) ShadowValueHeader
{
    juce::uint32 magic;             //  0
    juce::uint32 version;           //  4
    juce::uint32 seq;               //  8  seqlock; ODD = a write is in progress
    juce::int32  slot;              // 12  0-based slot these values belong to, -1 = none
    juce::uint32 paramCount;        // 16  what the plugin HAS
    juce::uint32 published;         // 20  how many are in `value` (<= kShadowValMax)
    juce::uint32 schemaDigest;      // 24  count + a hash of the names; a mismatch refuses
    juce::uint32 structureRevision; // 28  the rack's shape when this was taken
    juce::uint32 stateGeneration;   // 32  LAYER 3 moves this; 0 = never published
    juce::uint32 sweepMicros;       // 36  what the last full getValue() sweep cost the Link
    juce::uint8  _pad[24];          // 40..63
};
static_assert (sizeof (ShadowValueHeader) == 64, "ShadowValueHeader must be 64 bytes");
static_assert (offsetof (ShadowValueHeader, seq)          ==  8, "seq moved");
static_assert (offsetof (ShadowValueHeader, slot)         == 12, "slot moved");
static_assert (offsetof (ShadowValueHeader, paramCount)   == 16, "paramCount moved");
static_assert (offsetof (ShadowValueHeader, published)    == 20, "published moved");
static_assert (offsetof (ShadowValueHeader, schemaDigest) == 24, "schemaDigest moved");
static_assert (offsetof (ShadowValueHeader, sweepMicros)  == 36, "sweepMicros moved");

struct ShadowValueFrame
{
    ShadowValueHeader hdr;
    float             value[kShadowValMax];   // the parameter's OWN normalised 0..1
};
static_assert (sizeof (ShadowValueFrame) == 64 + kShadowValMax * 4,
               "ShadowValueFrame grew - the file's size is its contract with every installed binary");

static constexpr size_t kShadowValFileSize = sizeof (ShadowValueFrame);

/** ONE name, from the uid, for both processes. */
inline juce::String shadowValuesFileNameFor (const juce::String& uid)
{
    juce::String safe;
    for (auto c : uid)
        safe << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c)
                                                               : juce::String ("_"));
    return "shadowval_" + safe + ".bin";
}

// ---- THE DIGEST ------------------------------------------------------------------------------

/** The count and the names, in one number. FNV-1a over each parameter's name with a separator, so
    two plugins that merely happen to have the same number of parameters do not agree, and a vendor
    update that renames or reorders one disagrees - which is the whole point.

    THE SAME FUNCTION ON BOTH SIDES. If the Link hashed names and the V2 hashed something else, a
    mismatch would mean nothing and the guard would be measuring our own inconsistency. */
inline juce::uint32 shadowSchemaDigest (const juce::AudioProcessor& p)
{
    juce::uint32 h = 2166136261u;
    auto eat = [&h] (juce::uint8 b) { h ^= b; h *= 16777619u; };
    const auto& params = p.getParameters();
    const juce::uint32 n = (juce::uint32) params.size();
    for (int i = 0; i < 4; ++i) eat ((juce::uint8) ((n >> (i * 8)) & 0xffu));
    for (auto* prm : params)
    {
        const auto name = prm->getName (64);
        for (auto c : name) eat ((juce::uint8) (((int) c) & 0xff));
        eat ((juce::uint8) 0x1f);                    // a separator, so "ab"+"c" != "a"+"bc"
    }
    return h;
}

// ---- PUBLISH AND READ, SEQLOCKED EXACTLY AS THE METER FRAME IS -------------------------------

inline void publishShadowValues (void* map, const ShadowValueHeader& h,
                                 const float* values, int count)
{
    if (map == nullptr) return;
    auto* dst = reinterpret_cast<ShadowValueFrame*> (map);
    const juce::uint32 s = LinkShm::loadRelaxed (&dst->hdr.seq) & ~1u;
    LinkShm::storeRelease (&dst->hdr.seq, s + 1);                  // odd: writing
    // The header after `seq`, then the values. `seq` itself is never memcpy'd over, exactly as
    // publishMeterFrame leaves it alone - it is the lock, not payload.
    std::memcpy (reinterpret_cast<juce::uint8*> (dst) + 12,
                 reinterpret_cast<const juce::uint8*> (&h) + 12,
                 sizeof (ShadowValueHeader) - 12);
    dst->hdr.magic   = kShadowValMagic;
    dst->hdr.version = kShadowValVersion;
    const int n = juce::jlimit (0, kShadowValMax, count);
    if (n > 0 && values != nullptr)
        std::memcpy (dst->value, values, (size_t) n * sizeof (float));
    LinkShm::storeRelease (&dst->hdr.seq, s + 2);                  // even: stable
}

/** Returns false on a torn read that would not settle, on a file that is not one of ours, or on a
    frame that has never been published - and a caller that gets false KEEPS WHAT IT HAD. `out.seq`
    is the staleness signal: an unchanged seq means no new frame, never "nothing changed". */
inline bool readShadowValues (const void* map, ShadowValueHeader& out,
                              float* values, int maxValues, int& countOut)
{
    countOut = 0;
    if (map == nullptr) return false;
    const auto* src = reinterpret_cast<const ShadowValueFrame*> (map);
    for (int tries = 0; tries < 4; ++tries)
    {
        const juce::uint32 s1 = LinkShm::loadAcquire (&src->hdr.seq);
        if ((s1 & 1u) != 0u || s1 == 0u) continue;                 // mid-write, or never published
        ShadowValueHeader h = src->hdr;
        const int n = juce::jlimit (0, juce::jmin (kShadowValMax, maxValues),
                                    (int) h.published);
        if (n > 0 && values != nullptr)
            std::memcpy (values, src->value, (size_t) n * sizeof (float));
        if (LinkShm::loadAcquire (&src->hdr.seq) != s1) continue;   // it moved under us: retry
        if (h.magic != kShadowValMagic || h.version != kShadowValVersion) return false;
        out = h; countOut = n;
        return true;
    }
    return false;
}

// ---- THE DECISION: DO WE MIRROR, AND IF NOT, WHAT DOES HE READ? ------------------------------

enum class ShadowMirror { Apply, Refuse };

struct ShadowMirrorDecision
{
    ShadowMirror what = ShadowMirror::Refuse;
    juce::String line;              // empty only when `what` is Apply
};

/** Pure, so the gate holds every road without a Link, a plugin or a window. `frameRead` is what
    readShadowValues returned. */
inline ShadowMirrorDecision shadowMirrorDecision (bool frameRead,
                                                  juce::uint32 linkDigest,
                                                  juce::uint32 copyDigest,
                                                  int publishedCount,
                                                  int copyParamCount)
{
    if (! frameRead)
        return { ShadowMirror::Refuse, "the view is not following the Link's knobs - this Link is "
                                       "an older build and does not publish them yet" };

    if (linkDigest != copyDigest)
        return { ShadowMirror::Refuse, "the view is not following the Link's knobs - the plugin "
                                       "there and the copy here do not have the same parameter "
                                       "list, so a value could land on the wrong control" };

    if (publishedCount <= 0 || copyParamCount <= 0)
        return { ShadowMirror::Refuse, "the view is not following the Link's knobs - no values "
                                       "arrived for that slot" };

    return { ShadowMirror::Apply, {} };
}

/** Write the CHANGED values into the copy and return how many moved. `lastApplied` is grown and
    kept by the caller, one entry per parameter, seeded to a value no normalised parameter can hold
    so the FIRST frame applies everything.

    `setValueNotifyingHost`, not `setValue`: we are this copy's host, and the notifying road is the
    one a guest AU's own interface listens on - which is the whole point of a mirror he can SEE. */
inline constexpr float kShadowValNever = -1.0f;      // "nothing has been applied here yet"
inline constexpr float kShadowValEps   = 1.0e-4f;    // below a knob's own resolution

/** ---- THE MIRROR, AFTER HIS 26 OCT RULING: ONLY WHAT DID NOT COME FROM HIM ------------------

    `mayWrite(i, frameValue)` is `echojay::mirrorMayWriteParam` with this V2's own facts bound in:
    his hand, and what this V2 last sent. This function obeys it and decides nothing itself.

    WHAT WENT AND WHY. The old rule was "write every value that differs from what we last applied",
    with a gesture hold bolted on. That made the mirror a second author of every knob, and the copy
    and the mirror then argued: the backward write under his hand, and - once that was stopped - a
    knob that did not move at all, because the mirror's write had been the only thing moving it.
    The copy leads now. The mirror writes automation, the agent, and a hand on the Link's own
    window, and nothing else.

    A SKIPPED PARAMETER'S `lastApplied` IS LEFT ALONE, exactly as before and for the same reason: if
    a skip wrote the frame's value into lastApplied without applying it, the next frame would see
    "no change" and the copy would stay wrong for ever. */
inline int shadowApplyExternalValues (juce::AudioProcessor& copy, const float* values, int count,
                                      std::vector<float>& lastApplied,
                                      const std::function<bool(int, float)>& mayWrite)
{
    const auto& params = copy.getParameters();
    const int n = juce::jmin (count, params.size());
    if (n <= 0) return 0;
    if ((int) lastApplied.size() < n) lastApplied.resize ((size_t) n, kShadowValNever);

    int moved = 0;
    for (int i = 0; i < n; ++i)
    {
        const float want = juce::jlimit (0.0f, 1.0f, values[i]);
        // AGAINST WHAT WE LAST APPLIED, never against the copy's current value: a plugin that
        // quantises or ignores a write would otherwise be written to on every tick for ever, which
        // is the busy-loop version of a mirror.
        const float had  = lastApplied[(size_t) i];
        if (had != kShadowValNever && std::abs (want - had) <= kShadowValEps) continue;
        if (mayWrite && ! mayWrite (i, want)) continue;          // his, or ours: not the mirror's
        lastApplied[(size_t) i] = want;
        if (auto* prm = params[i]) { prm->setValueNotifyingHost (want); ++moved; }
    }
    return moved;
}

} // namespace echojay
