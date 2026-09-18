#pragma once
// ChainJsonView (18 Sep 2026, the Pro Tools crash at 13:39:51): ONE parse of a chain JSON block that OWNS the
// parsed root for the view's lifetime and exposes the chain array and the role-by-name map from it.
//
// The crash: three sites wrote
//     if (auto* co = juce::JSON::parse(chainJson).getProperty("chain", juce::var()).getArray()) for (auto& sv : *co) ...
// The parsed root and the property lookup are TEMPORARIES that die at the end of that init-statement; the root's
// DynamicObject was the array's only owner, so `co` pointed into freed memory before the loop ran. Whether it
// crashed depended on what malloc wrote into the freed block: on 18 Sep it was a scribbled free-list word
// (0xff76007affffffff read as a var's type pointer -> EXC_BAD_ACCESS in EchoJayEditor::roleByNameFor +440, on the
// main thread, in the SESSION build's dial-settled completion). MallocScribble=1 makes it deterministic.
//
// Rule (pinned by tools/temp_var_grep_guard): no raw pointer into a temporary var anywhere. Hold the owner.
#include <JuceHeader.h>
#include <map>

class ChainJsonView
{
public:
    explicit ChainJsonView (const juce::String& chainJson)
        : root_ (juce::JSON::parse (chainJson)),
          chain_ (root_.getProperty ("chain", juce::var()))   // a second reference on the array object, held for the view's life
    {}

    // The chain array, or nullptr when the block has none. Valid EXACTLY as long as this view lives: never store
    // the pointer beyond the view, never call this on a temporary view and keep the result.
    const juce::Array<juce::var>* chain() const noexcept { return chain_.getArray(); }
    bool hasChain() const noexcept { return chain() != nullptr; }
    int  size() const noexcept { auto* a = chain(); return a != nullptr ? a->size() : 0; }

    // Slot names in chain order, as written.
    juce::StringArray names() const
    {
        juce::StringArray out;
        if (auto* a = chain())
            for (auto& ev : *a) out.add (ev.getProperty ("name", juce::var()).toString());
        return out;
    }

    // Trimmed, lower-cased name -> role. Returned BY VALUE: it outlives the view safely.
    std::map<juce::String, juce::String> rolesByName() const
    {
        std::map<juce::String, juce::String> roles;
        if (auto* a = chain())
            for (auto& sv : *a)
                roles[sv.getProperty ("name", juce::var()).toString().trim().toLowerCase()] = sv.getProperty ("role", juce::var()).toString();
        return roles;
    }

private:
    juce::var root_;    // owns the parsed tree
    juce::var chain_;   // owns the array object independently of root_
};
