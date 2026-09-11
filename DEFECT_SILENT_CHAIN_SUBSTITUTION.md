# DEFECT: a chain the user approved was silently replaced (filed 6 Sep 2026, shoot day)

## The observation (Sean, Pro Tools, 6 Sep 2026, logged in as studio.seand@gmail.com)
The assistant proposed a mix-bus chain - SSL G Bus / Studer A800 / Neve 33609 C /
Precision Limiter - and asked "Ready to build it?". Sean answered "Build it".
The rack that was built was EchoJay EQ / AMEK / ATR-102 / bx_limiter: no
overlap with the proposal and no mention of the change. Screenshots: Sean's;
to be filed under a connected folder (echojay-vst/HANDOVER is never committed;
use tools/merge_gate_tests/results_2026-09-06/ or Documents) when he sends them.

## Why it is a defect on ANY account and ANY tier
A user's explicit approval of a named chain is a contract; building a different
chain without saying so is a silent substitution. Whether the substitution came
from resolution (a proposed name that this machine does not have resolving to
something else), from the server's second pass (the build turn re-planned
against the catalogue), or from account routing, the user must be TOLD what
changed and why before or as it lands. Nothing about tier makes that
acceptable.

## The account facts established so far (plugin side, code quoted; server side is where the rest lives)
- The plugin reads tierLevel ONLY for cosmetics: the badge text
  (PluginEditor.cpp:2959-2961 STUDIO MAX / STUDIO / PRO), look-and-feel colours
  (EchoJayLookAndFeel.h:587-607), the account card layout (:3919-3920) and the
  default message limit (EchoJayAPI.cpp:693/826/1059/3793
  defaultLimitForTier). NOTHING in the plugin gates the assistant question
  flow, the chain builder's catalogue, name resolution or a composer version on
  tier. The name-resolution ladder (EJNameLadder.h, EJWavesAlias.h,
  EJWavesRegistryFeed.h) runs locally against THIS machine's scan, tier-blind.
- The string "Oracle" does not occur anywhere in Source/: the "Oracle 2.7"
  shown in the composer comes from the server (the remote prompt/config the
  plugin fetches: "prompt in force = REMOTE v19" in today's logs), not from
  the plugin. Which prompt/version an account is served is a SERVER decision.
- The dashboard surface answers 404 not_enabled for an account without the
  server's DASHBOARD_ENABLED flag (see MERGE_2026-09-06.md, the dashboard
  gate) - a per-account server flag, not tier.
- Today's runs against production answered /api/me 404 "User not found" for
  the session this Mac held; a V1 account and the v2 test account are served
  by different backend state, which the plugin cannot see.
So: of the observed behaviours, NONE is tier-gated in the plugin; the question
flow, the catalogue offered, the model/prompt version and the dashboard flag
are all server-side per-account decisions. The discriminator is the same
request logged in as wonderwithsienna@gmail.com - if the flow and the chain are
right there, this is account routing; the silent substitution is a defect
either way.

## Status
Filed. Not chased further today (shoot). Owed: Sean's screenshots; the
wonderwithsienna re-run; then the server-side trace of the build turn (Kathy
has the saas clone) to see where the proposed names became the built ones.

---

# DEFECT 2 (same family): "only suggest plugins EchoJay can auto-dial" promised, hand-dialling delivered
Filed 6 Sep 2026, evening, by the reviewer's ruling. DIAGNOSE TOMORROW; NOT FIXED TONIGHT; no code was read
for this filing - everything below is observation, and the questions are the questions.

## What Sean saw (Pro Tools session, this Mac)
Setting ON: "Only suggest plugins EchoJay can auto-dial (fewer options)". The built chain contained three
plugins that needed hand-dialling - Solid Bus Comp, SSL Fusion Vintage Drive and UAD Pultec EQP-1A - against
EchoJay EQ and Newfangled Elevate, which were dialled. The plugin said so itself:
    "Solid Bus Comp needs hand-dialing - use the values on its card."
Screenshot evidence: Sean's, in a connected folder (not yet located by name from this side; ask for the path
when the diagnosis starts - none dated today was found under echojay-vst, echojay, echojay-vst-pitch, Documents).

## THE MECHANISM, CONFIRMED BY OBSERVATION one turn later
Sean's next turn dialled BOTH plugins the build had declared un-dialable: SSL Fusion Vintage Drive (Density
3.5) and UAD Pultec EQP-1A (High Freq, HF Boost 0.5); "Changes applied"; the Drive's card now reads "Applied
automatically". Same plugins, same session, one turn later.
So DIALABILITY IS NOT A PROPERTY OF THE PLUGIN. It is a property of whether the parameter map has arrived.
The build asks before the lazy fetch completes and takes "no" as final. One question, three answers, decided
by timing:
    suggestion time -> catalogue says dialable        -> passes the "only dialable" filter
    build time      -> map has not landed             -> "needs hand-dialing"
    next turn       -> map has landed                 -> dials it silently
Consistent with Kathy's own commit "Ask for the fallback map when the dial needs it, not before the rack
exists": suggestion asks a catalogue, build asks the maps, and the two answers can disagree.

## Why it is the same family as DEFECT 1
Plan from one source, build against another. The user is told one thing and given another, and the software
says so in passing rather than refusing or waiting. Probably the same cause; diagnose them together.

## THE PATTERN (the third instance today - recorded in its own right, not as three unrelated bugs)
A decision made on a signal whose timing the code does not control, read at whatever moment it happens to be
read:
    1. the uid adoption gate   - "frozen" decided by counting observations (C4; fixed by a time floor)
    2. name provenance         - a seeded name taken as authoritative because it was read before the host's
                                 own delivery (v6/v7; fixed by provenance + the own-chunk rule)
    3. dialability             - "cannot be dialled" decided before the lazy map fetch lands (this defect)
Memory: [[decisions-on-uncontrolled-timing]]. The test for the family: if the answer would change by asking
a few seconds later, the code must either wait with a bound or say that it has not waited.

## DIAGNOSE, tomorrow (the reviewer's four questions, verbatim in substance)
1. WHERE IS THE SETTING APPLIED? Locally before the request, in the prompt sent to the server, in a filter on
   the response, or nowhere? Quote it. If it only reaches the model as a request rather than a constraint,
   that is the defect in one line.
2. WHERE IS DIALABILITY DECIDED, and is it decided the SAME WAY at suggestion time and at build time? Prime
   suspect: it depends on a parameter map, and maps are fetched lazily. If suggestion asks a catalogue and
   build asks the maps, the two answers can disagree and the user gets exactly this.
3. DOES ROSETTA CHANGE THE ANSWER? The loadable set differs under Rosetta, and so might the map set. (Pro
   Tools runs the x86_64 slice here.)
4. WHAT SHOULD HAPPEN when a suggested plugin turns out not to be dialable at build time? Right now it builds
   it anyway and mentions it in passing.

## THE FIX SHAPE, for tomorrow's ruling (not built tonight)
  - the build WAITS for the map, with a bound, before concluding a plugin cannot be dialled; or
  - if it will not wait, it must not say "needs hand-dialing": it says the settings will apply when the map
    arrives, and then applies them.
Telling the user to hand-dial something the software will dial itself sixty seconds later is the defect, and
it is worse than either honest answer. Whichever is chosen, the "only suggest dialable plugins" setting must
ask the SAME question the build will ask, or it is promising something it cannot know.

## Status
Filed. Diagnosis tomorrow, after the signing question is settled. Owed first: the screenshot path; the quoted
setting-application site (question 1); the two dialability sites (question 2).

## DEFECT 2 - THE TWO MEASUREMENTS AND WHERE IT LIVES (22:30, 6 Sep; from the live log of Sean's build turn)
Instrument: unified log, Pro Tools pid 83157, 20:56-21:44, every EJDial/EJParamMaps/EJFallback/EJStaleMap line
(results_2026-09-06/protools_dial_log_20-57_build.log, 62 lines). Read before writing anything, as ruled.
### The build turn, 20:57:18: "extracted block -- 5 slot(s), 3 with settings_structured"
    slot 0 EchoJay EQ            structured=y  applied (built-in)
    slot 1 Solid Bus Comp        structured=y  requested=3 applied=0 manual=3 readbackMiss=1 status=writesRejected map=y
    slot 2 SSL Fusion Vintage Drive   "carried NO settings_structured in the chain block ... SERVER/model-side gap"
    slot 3 UAD Pultec EQP-1A          "carried NO settings_structured in the chain block ... SERVER/model-side gap"
    slot 4 Newfangled Elevate    structured=y  requested=1 applied=1  (its fallback map arrived inside the wait)
So the three "hand-dial" plugins are TWO different mechanisms, and neither is "the build asked before the map
landed and took no as final":
  Drive and Pultec: the server sent NO structured settings for them at all. Nothing was offered to the dial
    path, so no map question was ever asked. Their maps arrived 20 s later (prefetch, 481 ms) and at 21:39:32 both
    read "dialable=true"; the follow-up turn's EDIT carried structured settings and dialled them. The setting's
    promise was broken on the SERVER: a plugin passed the "only dialable" filter and came back without settings.
  Solid Bus Comp: its map WAS there when the dial ran (map=y; the fallback answered in 2,983 ms, inside the
    settle). The dial ran and the WRITES WERE REJECTED: 3 requested, 0 applied, readback mismatch 1. That is a
    parameter-write failure on this host (Pro Tools, x86_64 slice under Rosetta) - question 3's territory, not
    timing. "dialable=ABSENT" on its map line: the map carries no dialable flag.
### Measurement 1 - how long a map fetch takes, request to usable (three samples, this session)
    fallback lookup, Solid Bus Comp     20:57:25.505 -> 28.487    2,982 ms
    fallback lookup, Newfangled Elevate 20:57:32.648 -> 33.077      429 ms
    prefetch, 2 maps (Drive, Pultec)    20:57:38.444 -> 38.925      481 ms
    The bubble's wait: finishChainBubbleWhenDialSettled(chainJson, 8) x 250 ms = 2,000 ms (PluginEditor.cpp:30555,
    :22131). The slowest measured fetch is 1.5x the bound. A watchdog logs at 6 s.
### Measurement 2 - is the map requested at build time, or only when the dial is attempted?
    Prefetch (all scanned identities) runs at editor open / scan / login (PluginEditor.cpp:11307, :20016, :20292;
    ChainHost.cpp:2723, :4248) - but ONLY for identities already in identityToFp_. All five slots here read
    "rung=first-index": their fps were not indexed, so nothing was requested before the build. The exact-fp
    request and the fallback lookup fire at settings-attach (ChainHost.cpp:2797-2836), i.e. when the dial is
    attempted. The request is late, exactly as the ruling suspected; the wait then runs from that late start.
### Where the "needs hand-dialing" wording comes from (client) - the C2 defect line
    PluginEditor.cpp:22198   case ChainHost::DialStatus::pending: zeroParts.add(di.name);   -> "needs hand-dialing"
    A slot still PENDING when the 2 s wait expires is worded exactly like a slot with no map. And a fetch that
    fails (status != 200, PluginEditor.cpp:820) never clears pendingMapFps_, so that slot is pending forever.
### Where the "only dialable" filter asks (C3)
    Suggestion: ChainHost::getDialableRecommendableNames - local map by exact fp, OR the server's existence
    index (/api/params/lookup mode=exists, mapped_versions non-empty, version-INSENSITIVE key). Build: the local
    map by exact fp, then the version-tolerant fallback. Two sources; the fallback bridges the version gap, and
    the log shows it bridged it here. What the filter cannot see is whether the SERVER will emit settings for
    the plugin it let through - which is what failed for Drive and Pultec.
### WHERE IT LIVES
    C1 (bound) and C2 (honest pending wording, failed-fetch pending) are PLUGIN-SIDE: another AAX round trip.
    The Drive/Pultec omission is SERVER-SIDE (settings_structured not emitted for plugins the existence index
    calls dialable): a deploy, Kathy. Solid Bus Comp's writesRejected under Rosetta is a THIRD item, undiagnosed.
NOT BUILT: the ruled mechanism does not match this log for two of the three plugins; reported back for the ruling.
