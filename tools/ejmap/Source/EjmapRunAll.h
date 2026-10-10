/*
  EjmapRunAll.h - THE ONE-COMMAND RUN (Kathy, 8 Oct stretch S1): "$BIN" --run-all [--until 07:00] [--only <product>]... [--steps a,b]
  [--skip a,b] [--dry-run]. Sean's whole sequence in order - preflight, licence check + stamp, the Satellite check, the compressor
  follow-up, gain / timing, every --redo set (the 7 Oct nights, then the next build's), --phaseb-drafts - each step a child ejmap process
  in its own process group, its output streamed to cert/run_all/<step>.log.

  RESUMABLE: cert/run_all.json records every step's state (pending / started / done / failed). A done step is never run again. A step
  that was STARTED and interrupted (Ctrl-C, --until, a sleep) is resumed WITHOUT --redo: `--phaseb-all --redo X` deletes X's finished rows
  every time it starts, so a resume runs `--phaseb-all --category X` and only the missing rows run (a redo selector - uad, no_pool,
  nothing_nominated - resumes as a plain --phaseb-all over the default categories: exactly the rows its first start deleted, plus any never
  run). CTRL-C SAFE: SIGINT is passed to the step's process group (the Phase B batch already leaves no half row), the step stays "started".
  --until HH:MM: no step starts after the hour; a running step is stopped with SIGINT at the hour (SIGKILL 60 s later) and resumes next time.
  One progress line per step and every minute: done / total, elapsed, the ETA from each step's stated estimate, the hour it stops.

  This header holds what is pinned without a plugin: the step table, the arguments a step runs with (first start vs resume), the state's
  read / write, the deadline arithmetic, the ETA. The driver (runRunAll in EjmapCertDriver.h) does the processes. It is also the engine the
  wizard will drive.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <map>
#include <optional>

namespace ejmap::runall
{

// kind: "plain" (fixed args, always the same), "redo" (a Phase B redo: --redo on the first start, --category on a resume),
// "selector" (a redo selector: --redo on the first start, a plain --phaseb-all on a resume), "licence" (needs cert/licences.csv)
// everyRun (8 Oct, the switch-over): planned on every run even when done - preflight must check THIS app (a state file written by an
// earlier build marks it done), and the drafts must be re-derived by THIS build's rules (v0.2) over every record on disk
struct Step { juce::String name, kind; juce::StringArray args; double estimateS; juce::String why; bool stopOnFail = false; bool everyRun = false; };
inline const std::vector<Step>& steps()
{
    static const std::vector<Step> k {
        { "preflight",        "plain",    { "--cert-preflight" },                                   5.0,     "the app, the probe beside it and its signature", true, true },
        { "licence_check",    "licence",  { "--licence-check", "{cert}" },                         30.0,    "the alias table and the review sheet (cert/licences.csv)" },
        { "licence_stamp",    "licence",  { "--licence-stamp", "{cert}" },                         30.0,    "demo rows stamped, expired / unowned / unmatched filed needs_licence" },
        { "uad_preflight",    "plain",    { "--uad-preflight" },                                    5.0,     "the Satellite: PRESENT or ABSENT (recorded, the run continues)" },
        { "followup",         "plain",    { "--cert-tonecheck-all" },                               2400.0,  "the compressor follow-up" },
        { "gain_timing",      "redo",     { "gaincal,timing" },                                     3000.0,  "gain and timing for every certified compressor" },
        { "uad",              "selector", { "uad" },                                                600.0,   "UAD rows the Satellite admits" },
        // 10 Oct (Sean's night): 9 rows took 2 h+ (EchoBoy 42 min, EchoBoy Jr 35, Little PrimalTap 37 - delays under the 7 Oct reverb / delay
        // mode, role tests twice, tails to 20 s): slow, not stuck; the estimate follows
        { "no_pool",          "selector", { "no_pool" },                                            10800.0, "the Soundtoys / 2C empty-list rows (~40 min per delay or reverb)" },
        { "multiband",        "redo",     { "multiband" },                                          1200.0,  "multiband (the enable step)" },
        // THE FIX-UP (10 Oct, folded into the run): every multiband row and the gain-all rows that timed out or failed, re-run under this
        // build - planned ONLY for a state whose multiband or gain_all step was finished by a build from before the steps carried a build
        // stamp (Sean's 8-9 Oct run); a fresh run never needs it, and once done it is done
        { "fixups",           "fixup",    { "--phaseb-all", "--redo", "multiband,unfinished", "--category", "multiband", "--category", "gainall" }, 4200.0, "multiband again (9 Oct fixes) + the gain-all rows that timed out or failed" },
        { "categorise",       "plain",    { "--categorise-propose", "--include-pace" },             5400.0,  "the review sheet for the uncategorised (never categories.json)" },
        { "combined",         "redo",     { "combined" },                                           2400.0,  "combined settings" },
        { "material",         "redo",     { "material" },                                           4200.0,  "real material" },
        { "frequency",        "redo",     { "frequency" },                                          4200.0,  "three frequencies" },
        { "samplerate",       "redo",     { "samplerate" },                                         480.0,   "three sample rates on a spread of ten" },
        { "tuners",           "redo",     { "tuners" },                                             1800.0,  "tuners (humanize, flex)" },
        { "limiter",          "redo",     { "limiter" },                                            1500.0,  "limiters (ceiling + 6 dB, BS.1770 true peak)" },
        { "deesser",          "redo",     { "deesser" },                                            600.0,   "de-essers (the noise ladder)" },
        // gain_all BEFORE eq and saturation (Kathy, 8 Oct): saturation's level match uses the measured output control from the gain-all
        // drafts (phaseb/gainall/drafts, SATURATION spec section 6.3); the named / level-only fallback stays for products without one
        { "gain_all",         "redo",     { "gainall" },                                            25200.0, "the gain spec on every other product (before saturation: its output control)" },
        { "eq",               "redo",     { "eq" },                                                 9000.0,  "EQ (adaptive frequency points)" },
        { "saturation",       "redo",     { "saturation" },                                         7200.0,  "saturation + amp sims" },
        { "reverb_delay",     "redo",     { "reverb,delay" },                                       11500.0, "reverb and delay (role tests twice: x1.5-1.65 rehearsed, 8 Oct)" },
        { "transient_gate",   "redo",     { "transient,gate" },                                     1900.0,  "transient shapers and gates (role tests twice: x1.3-1.65 rehearsed, 8 Oct)" },
        { "nothing_nominated","selector", { "nothing_nominated" },                                  16200.0, "rows the lexicon nominated nothing for" },
        { "strips",           "redo",     { "strips" },                                             50400.0, "channel strips, every section drafted" },
        { "drafts",           "plain",    { "--phaseb-drafts" },                                    5.0,     "every draft from the records on disk", false, true } };
    return k;
}
inline const Step* stepNamed (const juce::String& n) { for (const auto& s : steps()) if (s.name == n) return &s; return nullptr; }

// THE ARGUMENTS a step runs with. `resume` = the step was started before and not finished. {cert} -> the cert folder.
inline juce::StringArray argsFor (const Step& s, bool resume, const juce::String& certPath, const juce::StringArray& only)
{
    juce::StringArray a;
    if (s.kind == "redo")
    {
        a.add ("--phaseb-all");
        if (resume) for (const auto& c : juce::StringArray::fromTokens (s.args[0], ",", "")) { a.add ("--category"); a.add (c); }
        else { a.add ("--redo"); a.add (s.args[0]); }
    }
    else if (s.kind == "selector") { a.add ("--phaseb-all"); if (! resume) { a.add ("--redo"); a.add (s.args[0]); } }
    else if (s.kind == "fixup" && resume) { for (int i = 0; i < s.args.size(); ++i) { if (s.args[i] == "--redo") { ++i; continue; } a.add (s.args[i]); } }   // a resume never re-deletes what the first start redid
    else for (const auto& x : s.args) a.add (x == "{cert}" ? certPath : x);
    const bool phaseb = a.contains ("--phaseb-all"), categorise = a.contains ("--categorise-propose");
    if (phaseb || categorise) for (const auto& p : only) { a.add ("--only"); a.add (p); }
    if (phaseb || categorise || a.contains ("--cert-tonecheck-all") || a.contains ("--phaseb-drafts")) { a.add ("--out"); a.add (certPath); }
    return a;
}

// THE STATE (cert/run_all.json): step -> { state, started_at, finished_at, exit, runs }
struct StepState { juce::String state = "pending"; juce::String startedAt, finishedAt, build; int exitCode = 0, runs = 0; };   // build: the build that finished it (10 Oct)
using State = std::map<juce::String, StepState>;
inline State stateFromVar (const juce::var& v)
{
    State st; if (auto* o = v.getProperty ("steps", {}).getDynamicObject()) for (const auto& kv : o->getProperties())
    { StepState s; s.state = kv.value.getProperty ("state", "pending").toString(); s.startedAt = kv.value.getProperty ("started_at", "").toString(); s.finishedAt = kv.value.getProperty ("finished_at", "").toString(); s.exitCode = (int) kv.value.getProperty ("exit", 0); s.runs = (int) kv.value.getProperty ("runs", 0); s.build = kv.value.getProperty ("build", "").toString(); st[kv.name.toString()] = s; }
    return st;
}
inline juce::var stateVar (const State& st)
{
    auto* o = new juce::DynamicObject(); auto* ss = new juce::DynamicObject();
    for (const auto& [n, s] : st) { auto* x = new juce::DynamicObject(); x->setProperty ("state", s.state); x->setProperty ("started_at", s.startedAt); x->setProperty ("finished_at", s.finishedAt); x->setProperty ("exit", s.exitCode); x->setProperty ("runs", s.runs); if (s.build.isNotEmpty()) x->setProperty ("build", s.build); ss->setProperty (n, juce::var (x)); }
    o->setProperty ("schema", "ej_run_all/1"); o->setProperty ("steps", juce::var (ss)); return juce::var (o);
}
// the plan: the steps to run now, in order (done ones skipped; --steps narrows, --skip removes)
// forced (10 Oct): steps planned even when done - the follow-up when stale tone checks exist (the driver decides from the folder)
inline bool fixupOwed (const State& st)
{
    for (const char* dep : { "multiband", "gain_all" }) if (st.count (dep) && st.at (dep).state == "done" && st.at (dep).build.isEmpty()) return true;
    return false;
}
inline std::vector<const Step*> plan (const State& st, const juce::StringArray& onlySteps, const juce::StringArray& skip, const juce::StringArray& forced = {})
{
    std::vector<const Step*> p;
    for (const auto& s : steps())
    {
        if (! onlySteps.isEmpty() && ! onlySteps.contains (s.name)) continue;
        if (skip.contains (s.name)) continue;
        if (s.kind == "fixup" && ! (st.count (s.name) && st.at (s.name).state != "done" && st.at (s.name).state != "pending") && ! fixupOwed (st)) continue;
        if (st.count (s.name) && st.at (s.name).state == "done" && ! s.everyRun && ! forced.contains (s.name)) continue;
        p.push_back (&s);
    }
    return p;
}
// the notes the run prints above its plan (10 Oct, the night lines): each one names what THIS plan does - a fixups note only when the
// fixups step is planned (once done, a pre-stamp multiband / gain_all no longer means anything runs), and stale checks the plan does
// not re-check (a --steps without followup) said as left stale, never as "runs again"
inline juce::StringArray planNotes (const State& st, const std::vector<const Step*>& p, int staleN, const juce::String& build)
{
    auto planned = [&] (const char* n) { for (const auto* x : p) if (x->name == n) return true; return false; };
    juce::StringArray notes;
    if (staleN > 0) notes.add ("RUN-ALL: " + juce::String (staleN) + " tone check(s) made by another build than " + build
                               + (planned ("followup") ? ": the follow-up runs again (stale checks first)" : ": the follow-up is not in this plan (--steps / --skip) - they stay stale"));
    if (planned ("fixups") && fixupOwed (st)) notes.add ("RUN-ALL: multiband / gain-all finished by a build before the 9-10 Oct fixes: the fixups step re-runs multiband and the gain-all rows that timed out or failed");
    return notes;
}
inline bool isResume (const State& st, const juce::String& step) { return st.count (step) && (st.at (step).state == "started" || st.at (step).state == "failed"); }

// THE DEADLINE: "07:00" -> the next 07:00 after `now` (today's if still ahead, else tomorrow's); empty -> none
inline std::optional<juce::Time> deadlineFor (const juce::String& hhmm, juce::Time now)
{
    if (hhmm.isEmpty()) return std::nullopt;
    const int h = hhmm.upToFirstOccurrenceOf (":", false, false).getIntValue(), m = hhmm.fromFirstOccurrenceOf (":", false, false).getIntValue();
    if (h < 0 || h > 23 || m < 0 || m > 59 || ! hhmm.contains (":")) return std::nullopt;
    juce::Time t (now.getYear(), now.getMonth(), now.getDayOfMonth(), h, m, 0, 0, true);
    if (t <= now) t = t + juce::RelativeTime::days (1);
    return t;
}
// THE ETA: the stated estimates of the steps not done (a started step counts half)
inline double etaSeconds (const std::vector<const Step*>& p, const State& st)
{
    double e = 0.0; for (const auto* s : p) e += isResume (st, s->name) ? 0.5 * s->estimateS : s->estimateS; return e;
}
inline juce::String hms (double s) { const int t = (int) std::lround (juce::jmax (0.0, s)); return juce::String (t / 3600) + ":" + juce::String ((t / 60) % 60).paddedLeft ('0', 2) + ":" + juce::String (t % 60).paddedLeft ('0', 2); }
inline juce::String progressLine (int doneN, int totalN, const juce::String& step, double elapsedS, double etaS, const std::optional<juce::Time>& until)
{
    return "[run-all " + juce::String (doneN) + "/" + juce::String (totalN) + (step.isNotEmpty() ? " " + step : juce::String()) + " | elapsed " + hms (elapsedS) + " | ETA " + hms (etaS) + (until ? " | stops at " + until->formatted ("%H:%M") : juce::String()) + "]";
}

} // namespace ejmap::runall
