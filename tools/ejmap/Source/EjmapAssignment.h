/*
  EjmapAssignment.h

  M4 model: rows, proposals, corroboration, session persistence, payload.

  THE CORROBORATION RULE (signed 2026-07-31). SPACE in Fast mode accepts a
  proposal only when something ALREADY ON DISK corroborates it: a capture of
  that index, membership in a co-moved set, or a stride prediction derived
  from observed captures. Uncorroborated proposals require W (wiggle-verify).
  The reasoning is the project's own history: the fastest path through the
  loop must not be the one that produces the least evidence, because that is
  how 2,107 maps got built on a classifier that reads Mono Maker as a
  frequency.

  STRIDE IS A VERIFIER, NEVER A GENERALISER (M2 finding, 15% failure rate
  measured). The stride here is derived from pairs of CAPTURED indices whose
  names are equal after digit-stripping -- two bands actually touched -- and
  never from the name pattern's own index assignments. It corroborates a
  proposal; it never invents one.

  PROPOSALS ARE A LOCAL FILE DROP: <ledger-root>/proposals/<fp>.json, the
  classified-v2 shape, fp-exact only. Absence is a first-class state (74% of
  machine-relevant maps have no verdict, measured): the rows become the
  category's dial set and every row records proposal_source so the finished
  map says which rows were assisted.

  MODE IS PER ROW, not per session. A session that starts Deep and drifts to
  SPACE-accepting is exactly what per-key trust exists to describe, so each
  row records the lane it was resolved in.

  MISMATCHES ARE A DATASET, not a burial. When W captures a different index
  than the proposal named, the row is re-pointed and the mismatch is written
  to misclassified-<run>.jsonl -- fp, semantic, proposed index and name,
  captured index and name, the classifier's reason. That is the labelled
  ground truth the plan lists as open decision 4, arriving free.
*/

#pragma once

#include <juce_core/juce_core.h>

#include "EjmapSchema.h"
#include "EjmapSweeper.h"

namespace ejmap
{

//==============================================================================
struct DialSets
{
    static juce::StringArray forCategory (const juce::String& cat)
    {
        auto c = cat.trim().toLowerCase();
        if (c == "compressor" || c == "comp")
            return { "threshold_db", "ratio", "attack_ms", "release_ms",
                     "makeup_db", "knee_db", "mix_pct", "input_db", "output_db" };
        if (c == "limiter")
            return { "threshold_db", "ceiling_db", "release_ms", "input_db",
                     "output_db", "mix_pct" };
        if (c == "eq")
            return { "freq_hz", "gain_db", "q", "low_cut_freq_hz",
                     "high_cut_freq_hz", "output_db" };
        if (c == "de-esser" || c == "deesser")
            return { "threshold_db", "sensitivity", "freq_hz", "mix_pct", "output_db" };
        if (c == "delay")
            return { "delay_time_ms", "feedback_pct", "mix_pct",
                     "low_cut_freq_hz", "high_cut_freq_hz", "output_db" };
        if (c == "reverb")
            return { "reverb_decay_s", "predelay_ms", "mix_pct", "wet_pct",
                     "low_cut_freq_hz", "high_cut_freq_hz", "output_db" };
        if (c == "saturation" || c == "sat")
            return { "drive", "mix_pct", "input_db", "output_db" };
        if (c == "gate")
            return { "threshold_db", "attack_ms", "release_ms", "ratio", "output_db" };
        if (c == "transient_shaper" || c == "transient")
            return { "sensitivity", "attack_ms", "release_ms", "output_db", "mix_pct" };
        if (c == "channel_strip")
            return { "input_db", "output_db", "threshold_db", "ratio", "freq_hz", "gain_db" };
        if (c == "amp_sim")
            return { "drive", "input_db", "output_db", "mix_pct" };
        return { "mix_pct", "output_db" };          // unknown category: minimal set
    }

    static juce::StringArray categories()
    {
        return { "compressor", "limiter", "eq", "de-esser", "delay", "reverb",
                 "saturation", "gate", "transient_shaper", "channel_strip", "amp_sim" };
    }

    /** The I-bulk floor's third leg: does this parameter NAME look like
        anything in any dial set? Name-based, and deliberately so -- this is a
        WITHHOLDING filter (it keeps things OUT of a bulk skip), so the
        conservative direction is to match too much, never too little.
    */
    static bool nameSuggestsDialSet (const juce::String& paramName)
    {
        static const char* tokens[] = { "thresh", "ratio", "attack", "release",
                                        "makeup", "knee", "mix", "gain", "freq",
                                        "output", "input", "drive", "ceiling",
                                        "sens", "decay", "predelay", "feedback",
                                        "cut", "wet", "dry", "time" };
        const auto n = paramName.toLowerCase();
        for (auto* t : tokens)
            if (n.contains (t))
                return true;
        // 'q' as a whole word only: 'Q', 'Q Factor', 'Band 1 Q'.
        return n == "q" || n.startsWith ("q ") || n.endsWith (" q") || n.contains (" q ");
    }
};

//==============================================================================
struct ProposalEntry
{
    int index = -1;
    juce::String kind, confidence, reason, channel;
};

struct ProposalSet
{
    bool present = false;
    juce::String category;
    juce::Array<ProposalEntry> entries;

    static ProposalSet load (const juce::File& root, const juce::String& fp)
    {
        ProposalSet out;
        auto f = root.getChildFile ("proposals").getChildFile (fp + ".json");
        if (! f.existsAsFile())
            return out;

        auto v = juce::JSON::parse (f.loadFileAsString());
        if (! v.isObject())
            return out;

        out.present  = true;
        out.category = v.getProperty ("category", "").toString();
        if (auto* arr = v.getProperty ("params", juce::var()).getArray())
            for (auto& pv : *arr)
            {
                ProposalEntry e;
                e.index      = (int) pv.getProperty ("index", -1);
                e.kind       = pv.getProperty ("kind", "").toString();
                e.confidence = pv.getProperty ("confidence", "").toString();
                e.reason     = pv.getProperty ("reason", "").toString();
                e.channel    = pv.getProperty ("channel", "").toString();
                if (e.index >= 0)
                    out.entries.add (e);
            }
        return out;
    }
};

//==============================================================================
/** Everything already on disk about this plugin's parameters, built from the
    captures-*.jsonl files in the ledger root. This is what corroborates.
*/
struct EvidenceIndex
{
    juce::SortedSet<int> captured;          // rows kind captured/captured_from_gesture
    juce::SortedSet<int> coMoved;           // members of any co_moved set
    struct Cap { int index = -1; juce::String name, capturedBy, at; };
    juce::Array<Cap> captures;

    static EvidenceIndex build (const juce::File& root, const juce::String& pluginId)
    {
        EvidenceIndex out;
        for (const auto& entry : juce::RangedDirectoryIterator (root, false, "captures-*.jsonl"))
        {
            juce::StringArray lines;
            lines.addLines (entry.getFile().loadFileAsString());
            for (const auto& line : lines)
            {
                if (line.trim().isEmpty()) continue;
                auto v = juce::JSON::parse (line);
                if (v.getProperty ("plugin_id", "").toString() != pluginId) continue;

                const auto kind = v.getProperty ("kind", "").toString();
                if (kind == "captured" || kind == "captured_from_gesture")
                {
                    Cap c;
                    c.index      = (int) v.getProperty ("index", -1);
                    c.name       = v.getProperty ("param_name", "").toString();
                    c.capturedBy = v.getProperty ("captured_by", "").toString();
                    c.at         = v.getProperty ("at", "").toString();
                    if (c.index >= 0) { out.captured.add (c.index); out.captures.add (c); }
                    if (auto* cm = v.getProperty ("co_moved", juce::var()).getArray())
                        for (auto& m : *cm)
                            out.coMoved.add ((int) m.getProperty ("index", -1));
                }
            }
        }
        return out;
    }

    static juce::String stripDigits (const juce::String& s)
    {
        juce::String out;
        for (auto c : s)
            if (! juce::CharacterFunctions::isDigit (c))
                out << juce::String::charToString (c);
        return out.trim();
    }

    /** Stride corroboration, derived from observed captures only: two captured
        indices whose names are equal after digit-stripping define a stride,
        and a proposal in the same name family sitting on that stride is
        corroborated. 85.1% predictive across the corpus, 15% failure measured,
        which is exactly why it corroborates and never generalises.
    */
    bool strideCorroborates (int proposalIndex, const juce::String& proposalName) const
    {
        const auto family = stripDigits (proposalName);
        if (family.isEmpty())
            return false;

        for (int i = 0; i < captures.size(); ++i)
            for (int j = i + 1; j < captures.size(); ++j)
            {
                const auto& a = captures.getReference (i);
                const auto& b = captures.getReference (j);
                if (a.index == b.index) continue;
                if (stripDigits (a.name) != family || stripDigits (b.name) != family) continue;

                const int lo = juce::jmin (a.index, b.index);
                const int s  = std::abs (a.index - b.index);
                if (s == 0) continue;
                if (proposalIndex >= lo && (proposalIndex - lo) % s == 0)
                    return true;
            }
        return false;
    }

    /** Empty string means uncorroborated. Otherwise names the source, which
        the row records: SPACE's legitimacy must be auditable per row.
    */
    juce::String corroborationFor (int index, const juce::String& paramName) const
    {
        if (captured.contains (index))  return "capture";
        if (coMoved.contains (index))   return "co-moved";
        if (strideCorroborates (index, paramName)) return "stride";
        return {};
    }
};

//==============================================================================
struct AssignRow
{
    enum class State { proposed, armed, captured, swept, confirmed,
                       skipNotPresent, skipNotAutomatable, skipDeferred };

    juce::String semantic;                 // dial-set key, or "ignore" rows use kind
    juce::String kind;                     // classifier kind ("ignore" included)
    int  proposedIndex = -1;
    juce::String proposedName;             // resolved from the instance at build
    juce::String proposalSource;           // "classifier" | "none"
    juce::String proposalConfidence, proposalReason, proposalChannel;

    State state = State::proposed;
    int  resolvedIndex = -1;               // what the map will use
    bool proposalMismatch = false;         // W captured something else
    juce::String corroboration;            // what SPACE relied on, if SPACE
    juce::String mode;                     // "fast" | "deep", recorded at resolution
    juce::String trust;                    // "human-verified" | "llm-classified"
    juce::String skipReason;
    juce::Array<int> coMoved;
    SweepOutcome sweep;
    juce::String resolvedAt;               // ISO 8601

    bool isSkipped() const
    {
        return state == State::skipNotPresent || state == State::skipNotAutomatable
            || state == State::skipDeferred;
    }
    bool isResolved() const { return state == State::confirmed || isSkipped(); }

    juce::String stateString() const
    {
        switch (state)
        {
            case State::proposed:           return "proposed";
            case State::armed:              return "armed";
            case State::captured:           return "captured";
            case State::swept:              return "swept";
            case State::confirmed:          return "confirmed";
            case State::skipNotPresent:     return "not_present";
            case State::skipNotAutomatable: return "not_automatable";
            case State::skipDeferred:       return "deferred";
        }
        return "proposed";
    }

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("semantic", semantic);
        o->setProperty ("kind", kind);
        o->setProperty ("proposed_index", proposedIndex);
        o->setProperty ("proposed_name", proposedName);
        o->setProperty ("proposal_source", proposalSource);
        o->setProperty ("proposal_confidence", proposalConfidence);
        o->setProperty ("proposal_reason", proposalReason);
        o->setProperty ("state", stateString());
        o->setProperty ("resolved_index", resolvedIndex);
        o->setProperty ("proposal_mismatch", proposalMismatch);
        o->setProperty ("corroboration", corroboration);
        o->setProperty ("mode", mode);
        o->setProperty ("trust", trust);
        o->setProperty ("skip_reason", skipReason);
        o->setProperty ("resolved_at", resolvedAt);
        juce::Array<juce::var> anch;
        for (const auto& a : sweep.anchors)
        { juce::Array<juce::var> p; p.add (a[0]); p.add (a[1]); anch.add (juce::var (p)); }
        o->setProperty ("anchors", juce::var (anch));
        o->setProperty ("anchors_reversed", sweep.anchorsReversed);
        o->setProperty ("sweep_method", sweep.method);
        o->setProperty ("identity_display", sweep.identityDisplay);
        return juce::var (o);
    }
};

} // namespace ejmap
