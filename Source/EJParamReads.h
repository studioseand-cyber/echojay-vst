#pragma once

#include <JuceHeader.h>
#include <functional>

// 6c section 8: one racked slot's CURRENT parameter READS, for the chat body.
//
// Header-inline, the EJDialMissRows.h discipline: the gate links the PREVIOUS
// build's SharedCode lib, so anything a pin exercises must live in a header the
// test TU compiles directly or the pin measures the last build instead of this
// one.
//
// "READS", NOT "VALUES" (8a). The name has to say where the text came from.
// This entire contract exists because a field named for what it looked like was
// an echo of a request, so the one thing the name must not do is describe the
// content without its provenance.
//
// THE CLIENT DOES NOT SELECT. Every parameter, keyed by index; the server joins
// at its single print site (fmtControl) onto the controls it has already decided
// to expose. A client-side selection rule was specified, built as an experiment
// and measured wrong: a port of the server's exposure rule agreed 128/128 with
// the rule this checkout can see and disagreed with the LIVE rule on 17 of 128
// maps, missing exactly the governing switches (Inf EQ Band1State, Pro-Q 3
// "Band 1 Used") the server had just added. Selecting here would have re-shipped
// the original bug inside the fix for it.
//
// DISPLAY TEXT, NOT NORMALISED FLOATS (3a). Converting a float to a display
// value needs the map's range and curve, and 218 of 4,693 anchored controls in
// the cache report a ceiling below their real one. A conversion built on a bad
// range is a confident wrong current value, the same harm in a new place.
namespace echojay
{

// 8e: a backstop against something pathological, never the primary bound.
// Corpus parameter counts: max 2085 (Heatwave), then 1088, 951, 519, 358, 338,
// 296, 245; median 25. 1024 carries 126 of the 128 cached maps whole and is
// four times the largest plugin in this investigation.
inline constexpr int kMaxParamReadsPerSlot = 1024;

/** One slot's entry, in the shape 8a settles. `readDisplay(i)` returns the
    parameter's display text.

    A JSON OBJECT, NOT A PACKED STRING (8a): display text can contain '=', ','
    and spaces -- "1 = Off" and "-3.0 dB, L" are both plausible -- so a packed
    encoding would need a delimiter and an escaping rule, and an escaping rule
    is a thing to get wrong.

    THE FOUR STATES 8d PRINTS FROM:
      read, non-empty        "3": "7.2 dB"
      read, empty text       "3": ""          -- not a sentinel: the empty
                                                 string IS what the plugin
                                                 returned, so it cannot collide
                                                 with a real display value
      index absent           no key           -- stale client / old build, the
                                                 common case, prints as today
      cut by the backstop    truncated: true  -- lets the server tell a cut from
                                                 a miss, which 8d requires and
                                                 which is impossible without it

    readFailed says the plugin did not respond AT ALL. The slot still appears,
    carrying an empty `reads`: the server needs to know the slot exists and
    could not be read, which an absent entry could not say.
*/
inline juce::var slotParamReadsVar (int slot1Based,
                                    const juce::String& pluginName,
                                    int numParams,
                                    const std::function<juce::String (int)>& readDisplay,
                                    bool readFailed = false)
{
    juce::DynamicObject::Ptr entry = new juce::DynamicObject();
    entry->setProperty ("slot", slot1Based);
    entry->setProperty ("name", pluginName);

    juce::DynamicObject::Ptr reads = new juce::DynamicObject();
    const int n = readFailed ? 0 : juce::jmin (numParams, kMaxParamReadsPerSlot);
    for (int i = 0; i < n; ++i)
        reads->setProperty (juce::String (i), readDisplay (i));

    entry->setProperty ("reads", juce::var (reads.get()));
    entry->setProperty ("truncated", (! readFailed) && numParams > n);
    entry->setProperty ("readFailed", readFailed);
    return juce::var (entry.get());
}

/** The whole per-slot decision, including "no instance means readFailed".

    IT LIVES HERE RATHER THAN AT THE CALL SITE so a pin can drive it with a
    null processor. The first version put the null check in ChainHost.cpp and
    pinned it by searching the source for the readFailed call; a mutation that
    dead-coded the branch left that call in place and the pin stayed green.
    A pin that asserts a line EXISTS cannot tell you it RUNS.
*/
inline juce::var slotParamReadsFor (int slot1Based,
                                    const juce::String& pluginName,
                                    juce::AudioProcessor* proc)
{
    if (proc == nullptr)
        return slotParamReadsVar (slot1Based, pluginName, 0,
                                  [] (int) { return juce::String(); }, true);

    auto& params = proc->getParameters();
    return slotParamReadsVar (slot1Based, pluginName, params.size(),
        [&params] (int p) -> juce::String
        {
            // The same call the apply path already makes on this loaded
            // instance (EchoJayParamApply.h), on the same thread. Measured
            // 0.4 us per read; 245 params cost 0.089 ms.
            return params[p] != nullptr ? params[p]->getCurrentValueAsText()
                                        : juce::String();
        });
}

} // namespace echojay
