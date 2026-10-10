/*
  EjmapTunerProfile.h - THE ej_tuner_profile/1 DRAFT EXPORTER (docs/TUNER_PROFILE_SPEC_v0_1.md section 5).

  PROPOSAL v0.1, 4 Oct 2026 - NOT FOR PUBLICATION. Behind `--tuner-profile-draft <record> <out.json>` only; nothing in the
  batch, the follow-up or the send path calls it. It turns a tuner record (pitchCandidates + pitchExtras) into the shape
  the spec proposes, so Sean can see real numbers against the schema before anything is agreed. Every field the record
  cannot fill is null with a reason in `notes` (the compressor spec's rule), and the whole file says what it is.

  speed:      the strength candidate whose transition time MOVES across positions (the spec's "speed control"); its
              direction read from the measurement (lower_is_harder when transition_ms falls as norm rises), never from
              the name; `transition_ms` null + `faster_than_ms` for a bound; `strength` beside each point.
  strength:   filled only when a candidate's static strength moves across positions (crispytuner's Amount); null for
              Auto-Tune's Retune Speed (1.0 throughout).
  flex / humanize / key / scale: from pitchExtras (A5), null with a note when absent.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <optional>
#include <vector>

namespace ejmap::tunerprofile
{

inline constexpr int kDefaultTuneStep = 1;   // v0.2 (Sean, 8 Oct): tuning asked for without a degree is step 1, Natural (v0.1 suggested 2)


inline constexpr const char* kSchema = "ej_tuner_profile/1";
inline constexpr const char* kStatus = "PROPOSAL v0.2 - not for publication";
inline constexpr double kStrengthMovesBy = 0.2;      // a candidate whose static strength spans this much is a strength control
inline constexpr double kSpeedMovesBy = 1.5;         // a candidate whose transition times span this RATIO is a speed control

struct Export { bool ok = false; juce::String refused; juce::var profile; juce::StringArray notes; };

inline std::optional<double> numberOf (const juce::var& v) { if (v.isDouble() || v.isInt() || v.isInt64()) return (double) v; return {}; }

// Does this candidate's speed move across positions (max/min transition ratio over kSpeedMovesBy)? Bounds count as the
// fastest measured value (they are "faster than"); refusals do not count.
inline bool speedMoves (const juce::var& cand)
{
    double lo = 1e9, hi = 0.0; int n = 0;
    if (const auto* ps = cand.getProperty ("positions", {}).getArray())
        for (const auto& p : *ps)
        {
            const auto sp = p.getProperty ("speed", {});
            std::optional<double> t;
            if (sp.getProperty ("result", "").toString() == "measured") t = numberOf (sp.getProperty ("duration_ms", {}));
            else if (sp.getProperty ("result", "").toString() == "bound") t = numberOf (sp.getProperty ("faster_than_ms", {}));
            if (t) { lo = juce::jmin (lo, *t); hi = juce::jmax (hi, *t); ++n; }
        }
    return n >= 2 && lo > 0.0 && hi / lo >= kSpeedMovesBy;
}
inline bool strengthMoves (const juce::var& cand)
{
    double lo = 1e9, hi = -1e9; int n = 0;
    if (const auto* ps = cand.getProperty ("positions", {}).getArray())
        for (const auto& p : *ps)
            if (const auto st = numberOf (p.getProperty ("strength", {}).getProperty ("strength", {}))) { lo = juce::jmin (lo, *st); hi = juce::jmax (hi, *st); ++n; }
    return n >= 2 && hi - lo >= kStrengthMovesBy;
}
// THE DIRECTION IN THE SPEC'S WORDS (section 5 field rules): lower_is_harder / higher_is_harder of the DISPLAY value, read
// from the measurement - the end with the shorter transition is the harder end (Auto-Tune: 400 is 1099 ms, 0 is a bound
// under 21.4 ms -> lower_is_harder). When the display is not a number (Slow / Medium / Fast) the words are about the norm:
// higher_norm_is_harder / lower_norm_is_harder, and the curve's `display` carries the words.
inline juce::String directionOf (const juce::var& cand)
{
    struct Pt { double norm; std::optional<double> display; double ms; };
    std::vector<Pt> pts;
    if (const auto* ps = cand.getProperty ("positions", {}).getArray())
        for (const auto& p : *ps)
        {
            const auto sp = p.getProperty ("speed", {});
            const auto t = sp.getProperty ("result", "").toString() == "measured" ? numberOf (sp.getProperty ("duration_ms", {})) : sp.getProperty ("result", "").toString() == "bound" ? numberOf (sp.getProperty ("faster_than_ms", {})) : std::nullopt;
            const auto n = numberOf (p.getProperty ("norm", {}));
            if (! n || ! t) continue;
            const auto d = p.getProperty ("display", "").toString().trim();
            std::optional<double> dv; if (d.isNotEmpty() && d.containsAnyOf ("0123456789") && d.retainCharacters ("0123456789.-+").length() == d.length()) dv = d.getDoubleValue();
            pts.push_back ({ *n, dv, *t });
        }
    if (pts.size() < 2) return "unknown";
    std::sort (pts.begin(), pts.end(), [] (const Pt& a, const Pt& b) { return a.norm < b.norm; });
    const bool harderAtHighNorm = pts.back().ms < pts.front().ms;
    if (pts.front().display && pts.back().display)
    {
        const bool displayRisesWithNorm = *pts.back().display > *pts.front().display;
        return (harderAtHighNorm == displayRisesWithNorm) ? "higher_is_harder" : "lower_is_harder";
    }
    return harderAtHighNorm ? "higher_norm_is_harder" : "lower_norm_is_harder";
}

inline Export exportTunerProfileDraft (const juce::var& record)
{
    Export e;
    if (record.getProperty ("schema", "").toString() != "ej_cert_tuner/1") { e.refused = "not a tuner record (schema " + record.getProperty ("schema", "").toString() + ")"; return e; }
    const auto* cands = record.getProperty ("pitchCandidates", {}).getArray();
    if (cands == nullptr || cands->isEmpty()) { e.refused = "no pitch candidate was measured"; return e; }
    auto* prof = new juce::DynamicObject();
    prof->setProperty ("schema", kSchema);
    prof->setProperty ("status", kStatus);
    {
        auto* pl = new juce::DynamicObject();
        pl->setProperty ("name", record.getProperty ("product", {})); pl->setProperty ("manufacturer", record.getProperty ("manufacturer", {}));
        pl->setProperty ("format", record.getProperty ("format", "AudioUnit")); pl->setProperty ("plugin_id", record.getProperty ("identity", {}));
        pl->setProperty ("version", record.getProperty ("version", {})); pl->setProperty ("map_fp", record.getProperty ("map_fp", juce::var()));
        prof->setProperty ("plugin", juce::var (pl));
    }
    const juce::var& first = cands->getReference (0);
    {
        auto* m = new juce::DynamicObject();
        m->setProperty ("tool", "EJ Map (ej_cert_tuner/1 record, plan v" + juce::String ((int) record.getProperty ("pitchPlan", {}).getProperty ("version", 1)) + ")");
        m->setProperty ("date", first.getProperty ("measuredAt", {}).toString().substring (0, 8));
        m->setProperty ("sample_rate", 48000);
        m->setProperty ("signal", "220 Hz sine; 30-cent square vibrato (half period 1/2/4 s adaptive); static detunes 5/10/20/30/50 cents; 200 ms notes with 100 ms gaps");
        m->setProperty ("latency_samples", first.getProperty ("generator", {}).getProperty ("latency_samples", juce::var()));
        prof->setProperty ("measured", juce::var (m));
    }
    // speed: the candidate whose transition moves; strength: the one whose static strength moves (may be the same control)
    const juce::var* speedCand = nullptr; const juce::var* strengthCand = nullptr;
    for (const auto& c : *cands) { if (speedCand == nullptr && speedMoves (c)) speedCand = &c; if (strengthCand == nullptr && strengthMoves (c)) strengthCand = &c; }
    auto curveOf = [&] (const juce::var& c, bool withSpeed, bool withStrength)
    {
        juce::Array<juce::var> curve;
        if (const auto* ps = c.getProperty ("positions", {}).getArray())
            for (const auto& p : *ps)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("norm", p.getProperty ("norm", {})); o->setProperty ("display", p.getProperty ("display", {}));
                if (withSpeed)
                {
                    const auto sp = p.getProperty ("speed", {}); const auto res = sp.getProperty ("result", "").toString();
                    if (res == "measured") o->setProperty ("transition_ms", sp.getProperty ("duration_ms", {}));
                    else { o->setProperty ("transition_ms", juce::var()); if (res == "bound") o->setProperty ("faster_than_ms", sp.getProperty ("faster_than_ms", {}));
                           else e.notes.add ("speed null at norm " + p.getProperty ("norm", {}).toString() + " (" + p.getProperty ("display", {}).toString() + "): " + sp.getProperty ("reason", "not measured").toString()); }
                    if (sp.hasProperty ("half_period_s")) o->setProperty ("measured_half_period_s", sp.getProperty ("half_period_s", {}));
                }
                if (withStrength)
                {
                    const auto st = p.getProperty ("strength", {});
                    if (st.getProperty ("result", "").toString() == "measured") o->setProperty ("strength", st.getProperty ("strength", {}));
                    else { o->setProperty ("strength", juce::var()); e.notes.add ("strength null at norm " + p.getProperty ("norm", {}).toString() + ": " + st.getProperty ("reason", "not measured").toString()); }
                }
                curve.add (juce::var (o));
            }
        return curve;
    };
    if (speedCand != nullptr)
    {
        auto* sp = new juce::DynamicObject();
        sp->setProperty ("control", speedCand->getProperty ("name", {})); sp->setProperty ("control_index", speedCand->getProperty ("index", {}));
        sp->setProperty ("direction", directionOf (*speedCand));
        sp->setProperty ("curve", curveOf (*speedCand, true, true));
        sp->setProperty ("stepped", speedCand->getProperty ("detentsBy", "").toString() == "text" || speedCand->getProperty ("detentsBy", "").toString() == "declared");
        if (speedCand->hasProperty ("detentTexts")) sp->setProperty ("detents", speedCand->getProperty ("detentTexts", {}));
        prof->setProperty ("speed", juce::var (sp));
    }
    else { prof->setProperty ("speed", juce::var()); e.notes.add ("speed null: no candidate's transition time moves across its positions"); }
    if (strengthCand != nullptr && strengthCand != speedCand)
    {
        auto* st = new juce::DynamicObject();
        st->setProperty ("control", strengthCand->getProperty ("name", {})); st->setProperty ("control_index", strengthCand->getProperty ("index", {}));
        st->setProperty ("curve", curveOf (*strengthCand, false, true));
        prof->setProperty ("strength", juce::var (st));
    }
    else if (strengthCand != nullptr) { prof->setProperty ("strength", juce::var()); e.notes.add ("strength: the speed control '" + strengthCand->getProperty ("name", {}).toString() + "' is also the strength control (its curve carries strength per point)"); }
    else { prof->setProperty ("strength", juce::var()); e.notes.add ("strength null: no candidate's static strength moves across its positions (1.0 throughout is Auto-Tune's shape)"); }
    // the v0.1 extras
    const auto extras = record.getProperty ("pitchExtras", {});
    auto firstOf = [] (const juce::var& arr) -> juce::var { if (const auto* a = arr.getArray(); a != nullptr && ! a->isEmpty()) return a->getReference (0); return {}; };
    if (const auto fx = firstOf (extras.getProperty ("flex", {})); fx.isObject())
    {
        auto* f = new juce::DynamicObject(); f->setProperty ("control", fx.getProperty ("name", {})); f->setProperty ("control_index", fx.getProperty ("index", {}));
        juce::Array<juce::var> curve;
        if (const auto* ps = fx.getProperty ("positions", {}).getArray())
            for (const auto& p : *ps) { auto* o = new juce::DynamicObject(); o->setProperty ("norm", p.getProperty ("norm", {})); o->setProperty ("display", p.getProperty ("display", {}));
                                        o->setProperty ("window_cents", p.getProperty ("window_cents", juce::var())); o->setProperty ("strength_by_detune", p.getProperty ("strength_by_detune", {})); curve.add (juce::var (o)); }
        f->setProperty ("curve", curve); prof->setProperty ("flex", juce::var (f));
    }
    else { prof->setProperty ("flex", juce::var()); e.notes.add (extras.isObject() ? "flex null: no flex/tolerance control" : "flex null: not measured (record predates the v0.1 measurements)"); }
    if (const auto hz = firstOf (extras.getProperty ("humanize", {})); hz.isObject())
    {
        auto* h = new juce::DynamicObject(); h->setProperty ("control", hz.getProperty ("name", {})); h->setProperty ("control_index", hz.getProperty ("index", {}));
        juce::Array<juce::var> curve;
        if (const auto* ps = hz.getProperty ("positions", {}).getArray())
            for (const auto& p : *ps) { auto* o = new juce::DynamicObject(); o->setProperty ("norm", p.getProperty ("norm", {})); o->setProperty ("display", p.getProperty ("display", {}));
                                        o->setProperty ("held_note_correction", p.getProperty ("held_note_correction", juce::var())); o->setProperty ("short_note_correction", p.getProperty ("short_note_correction", juce::var()));
                                        o->setProperty ("held_over_short", p.getProperty ("held_over_short", juce::var()));
                                        if (p.hasProperty ("held_vibrato_retained")) o->setProperty ("held_vibrato_retained", p.getProperty ("held_vibrato_retained", {}));
                                        curve.add (juce::var (o)); }
        h->setProperty ("curve", curve); prof->setProperty ("humanize", juce::var (h));
        // the 5 Oct records read held-vs-short only; whether a held note KEEPS ITS VIBRATO is the 6 Oct measurement (--redo tuners)
        bool vib = false; if (const auto* ps = hz.getProperty ("positions", {}).getArray()) for (const auto& p : *ps) if (p.hasProperty ("held_vibrato_retained")) vib = true;
        if (! vib) e.notes.add ("humanize: held-vs-short correction from this record; held-note vibrato retention not measured on it (--redo tuners measures it)");
    }
    else { prof->setProperty ("humanize", juce::var()); e.notes.add (extras.isObject() ? "humanize null: no humanize control" : "humanize null: not measured (record predates the v0.1 measurements)"); }
    bool key = false, scale = false;
    if (const auto* rb = extras.getProperty ("readbacks", {}).getDynamicObject())
        for (const auto& kv : rb->getProperties())
        {
            const auto& r = kv.value; const auto kind = r.getProperty ("kind", "").toString();
            auto* o = new juce::DynamicObject(); o->setProperty ("control", r.getProperty ("name", {})); o->setProperty ("control_index", r.getProperty ("index", {})); o->setProperty ("values", r.getProperty ("values", {}));
            if (kind == "key" && ! key) { prof->setProperty ("key", juce::var (o)); key = true; } else if (kind == "scale" && ! scale) { prof->setProperty ("scale", juce::var (o)); scale = true; } else delete o;
        }
    if (! key) { prof->setProperty ("key", juce::var()); e.notes.add ("key null: no key control read back - the spec says such a tuner is not profiled"); }
    if (! scale) { prof->setProperty ("scale", juce::var()); e.notes.add ("scale null: no scale control read back"); }
    // neutral and never_touch: every other control as instantiated, bypass/power named
    juce::Array<juce::var> neutral, never;
    if (const auto* cs = record.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            const int idx = (int) c.getProperty ("index", -1); const auto name = c.getProperty ("name", "").toString();
            bool used = false; for (const auto& cand : *cands) if ((int) cand.getProperty ("index", -2) == idx) used = true;
            if (used) continue;
            const auto lower = name.toLowerCase();
            if (lower.contains ("bypass") || lower == "power" || lower.contains ("standby")) { never.add (name); continue; }
            auto* o = new juce::DynamicObject(); o->setProperty ("control", name); o->setProperty ("set", c.getProperty ("defaultOnInstantiate", {}).getProperty ("display", {})); o->setProperty ("norm", c.getProperty ("defaultOnInstantiate", {}).getProperty ("normalised", {}));
            neutral.add (juce::var (o));
        }
    prof->setProperty ("neutral", neutral); prof->setProperty ("never_touch", never);
    {
        auto* q = new juce::DynamicObject();
        double worst = 0.0;
        if (speedCand != nullptr) if (const auto* ps = speedCand->getProperty ("positions", {}).getArray())
            for (const auto& p : *ps) if (const auto* ed = p.getProperty ("speed", {}).getProperty ("edge_durations_ms", {}).getArray(); ed != nullptr && ed->size() >= 2)
            { double lo = 1e9, hi = 0.0; for (const auto& d : *ed) { lo = juce::jmin (lo, (double) d); hi = juce::jmax (hi, (double) d); } worst = juce::jmax (worst, hi - lo); }
        q->setProperty ("transition_spread_ms", worst); q->setProperty ("method", "edges per position from the square vibrato; spread = max - min of the edge durations");
        prof->setProperty ("quality", juce::var (q));
    }
    // THE FEEL LADDER (v0.2, Sean's rulings 8 Oct): five steps; tuning asked for without a degree -> step 1 Natural (v0.1 suggested 2);
    // not asked -> 0; the step rides as tune_step on the slot's [CURRENT CHAIN] line; the key comes from the plugin's own detection
    // (key + confidence per channel), chromatic when unknown or low-confidence. The step's settings are the server's (section 6).
    {
        auto* f = new juce::DynamicObject();
        juce::Array<juce::var> steps; int k = 0; for (const char* w : { "Off", "Natural", "Polished", "Noticeable", "Hard" }) { auto* st = new juce::DynamicObject(); st->setProperty ("step", k++); st->setProperty ("name", w); steps.add (juce::var (st)); }
        f->setProperty ("steps", steps); f->setProperty ("default_step", kDefaultTuneStep); f->setProperty ("default_step_not_asked", 0); f->setProperty ("field", "tune_step");
        f->setProperty ("key_source", "the plugin's detected key (key + confidence per channel); chromatic when unknown or low-confidence");
        prof->setProperty ("feel", juce::var (f));
    }
    juce::Array<juce::var> notes; for (const auto& n : e.notes) notes.add (n); prof->setProperty ("notes", notes);
    e.ok = true; e.profile = juce::var (prof);
    return e;
}

} // namespace ejmap::tunerprofile
