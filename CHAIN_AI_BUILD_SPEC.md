# EchoJay v2 — Conversational Chain AI: Build Spec

> **OWNER: this repo (echojay-vst-v200).** Edit here. Any copy in echojay-saas
> is a mirror and must not be edited there.

**Scope:** v2 web app + v2 VST ONLY. Not v1.
**Repo note:** The V2 backend is echojay-saas. echojay-saas-v2 is the LEGACY app — do NOT edit it for this work.

## Goal
Turn the chain AI from one-shot into an intelligent multi-turn conversation that can:
- Build chains (existing capability)
- EDIT existing chains by prompt (add/remove/reorder/replace/bypass)
- ASK clarifying questions and OFFER choices as tappable buttons
- STAGE its responses (intro -> animated working -> result)
- Eventually edit plugin parameters ("make the EQ brighter")
- Extend the same conversational pattern to capture analysis & comparison (later)

Both surfaces (JUCE plugin + JS web app) must behave identically. Parity is achieved
by the SERVER owning all block schemas and copy; clients are dumb renderers.

## Core architecture (the spine)
The block protocol IS the router. One model call per turn. The reply is prose plus
zero or more delimited machine blocks, each with its own extractor that strips it
from the visible text. The model self-routes by choosing which block to emit.

Existing blocks: ECHOJAY_CHAIN (build), ECHOJAY_GAIN (gain proposals).
New blocks: ECHOJAY_CHAIN_EDIT (edit ops), ECHOJAY_ASK (question/choices).

Decisions locked:
- NO two-stage Haiku router. The block protocol handles routing. Revisit only via a
  Phase 1.5 fallback if telemetry shows the regex missing pronoun-heavy edits.
- ASK chips layout: B2 DOCKED SHELF — chips live in a shelf attached seamlessly to
  the top of the chat input (radius 12px top corners, input bg + 5% cyan wash,
  "or type below" hint), NOT under the assistant message. Pill styling both
  surfaces: fully rounded, cyan 9% fill / 40% border (hover 18%/70%), text #7FE3F2
  @ 12.5px, pad 6/14. Parity by mirrored value-comment blocks (PluginEditor.h
  AskChipLnF <-> app.html .ask-chip CSS). Any user send supersedes pending asks.
- Keep the Sage(Haiku)/Oracle model split, routed by turnType through the existing
  _model-routing.js ROUTE table — NOT a new router.
- Model metering is USAGE (tokens), not turn count.
- Preview-then-confirm before ANY chain mutation. Never silently mutate.
- Server-owned block schemas + copy so plugin and web cannot drift.
- Working-state animation: SHIMMER ONLY (cyan band sweeping through dim status text,
  ~1.6s). No waveform glyph, no chatbot dots.

## Model routing (server, single-source)
- Light/conversational turns (chat, ASK questions) -> Sage/Haiku.
- Heavy turns (build, edit ops, param dial) -> Oracle.
- Because the model self-routes via blocks, assign model via the existing rule layer
  (messageNeedsPlugins / classifyChainIntent) that predicts turn type BEFORE the call.
  Sage-first-then-escalate or rule-inference preferred over a pre-classify call.
- Produced-type charging: a build-classified turn that returns only an ASK question
  gets re-weighted DOWN (~1) post-parse via existing refund machinery. Makes
  clarifying turns cheap by construction — supersedes any "discount answer-taps" logic.

## Ask-vs-act policy (CRITICAL — tune, do not assume)
The AI must ask ONLY when genuinely necessary, not routinely. Bias hard toward acting.
- Prompt frames asking as a LAST RESORT: "only emit an ASK if you genuinely cannot
  make a sensible plugin choice or placement without the answer. Never ask about
  things you can infer from context or sensible defaults. At most one clarifying
  round, then act."
- Preview-then-confirm is the safety net — acting without asking is cheap because
  nothing applies without confirmation. Default to proposing, not asking.
- MUST be tested against real prompts before shipping (see Phase 1b testing).

## Data / state
- Current-chain visibility: inject [CURRENT CHAIN] (from ChainHost::getAllSlotInfos()
  — name/bypassed/settings/format/wet per slot, numbered) so the model sees the rack.
  Biggest gap today (model currently NEVER sees the existing chain).
- Staleness guard: add a monotonic chainRevision to ChainHost, bumped on EVERY
  mutation (add/remove/move/bypass/wet). Edit apply aborts if revision changed between
  propose and confirm. Also baseSlots echo in the edit block as belt-and-braces.
- Conversation state: already client-held, server-stateless (replays last 12 msgs /
  60KB). Machine blocks are stripped before storage — so the model must ALSO ask
  questions in PROSE (block only carries tappable choices), and tapped answers format
  as explicit user turns ("Tape-style (answering: 'What warmth?')") so the Q->A pair
  survives history trimming. No new state store needed.

## Block schemas
ECHOJAY_CHAIN_EDIT payload:
{"baseSlots":["TDR Nova","CREAM2PRE","Valhalla Room"],
 "edit":[
   {"op":"add","name":"TDR De-esser","after":1,"settings":"..."},
   {"op":"remove","slot":2},
   {"op":"replace","slot":0,"name":"Pro-Q 3","settings":"..."},
   {"op":"move","slot":1,"to":2},
   {"op":"bypass","slot":1,"on":true}
 ],"explanation":"..."}

ECHOJAY_ASK payload:
{"question":"What kind of warmth?",
 "choices":[
   {"label":"Tape-style","detail":"soft saturation, rounded top"},
   {"label":"Tube-style","detail":"harmonic thickness, mid glow"}
 ],"allowFreeText":true}

Plugin names come EXCLUSIVELY from the AVAILABLE PLUGINS injection.

## Apply logic (shared, both hosts)
- applyChainEdits(ops) lives in ChainHost — shared by main plugin + Link, like the
  wet/dry work. Main plugin calls directly; Link via versioned cmd transport
  (v:2 + editOps + ack; old Links safely ignore unknown versions).
- The risky component: serial async op sequencer. "add at position" = async
  loadPluginAsync (append) then moveSlot; "replace" = remove + insert. Slot removal
  MUST honor the close-editors-first, 80ms-deferred discipline (the AMEK
  use-after-free). Ops run serially through a small state machine, remapping
  subsequent indices as earlier ops shift the rack. THIS is where bugs live.

## PHASES (each independently shippable, in order)

**PHASE 1 (1a+1b+1c+1d) COMPLETE — all built, deployed and live-verified (26 Jul 2026).**

### Phase 1a — Protocol foundation ✅ BUILT + DEPLOYED (25 Jul 2026)
- Formalize the block union server-side (extensible — adding a block type must be
  trivial, not an if-chain). Keep CHAIN/GAIN behavior byte-identical (additive,
  non-breaking). Reserve CHAIN_EDIT and ASK types now.
- [CURRENT CHAIN] injection + chainRevision guard in ChainHost.
- Rule-layer routing: extend messageNeedsPlugins cues (remove/swap/instead-of) and
  server classifier so the right injections ride the right turns.
- Four server routing items (single-source): CHAIN_EDIT turnType -> Oracle; weight
  rows; classifier extension; produced-type re-weighting.
- Nothing user-visible ships from 1a alone.
- SHIPPED: api/_blocks.js registry; [CURRENT CHAIN] injection + chainRevision
  (all six mutators bump); chain_edit ROUTE/weight/classifier; produced-type
  re-weighting. Verification round added two fixes: the [CURRENT CHAIN CONTEXT]
  ground-truth note (model ignored the unexplained block) and a tightened
  chat-turn CHAIN BLOCK RULE strip (the old to-end-of-string regex destroyed
  [CURRENT CHAIN] on chat turns).

### Phase 1b — ASK / tappable choices ✅ BUILT + DEPLOYED (25 Jul 2026)
- extractAskBlock; ChatMsg.askData; chip renderer + tap-to-send with answer-
  formatting; chips disable after answering; wrap on narrow sidebar; coexist with
  free text. Web: same chips in renderChatDOM.
- Ask-vs-act prompt tuning + TEST against real prompts (see Testing gates).
- SHIPPED: ASK_PROTOCOL_NOTE (with one positive archetype: ambiguous flavour
  words like "warmth"), ANSWER_TAP_RE (taps classify as the build turn),
  extractAskBlock/extractAskBlockWeb + chips, workspace _ask/_askDone
  persistence. Ask-frequency gate PASSED: 8-prompt batch, only "add some
  warmth" asks; live flow verified in Logic. Re-weighting verified firing on
  ask-only chain replies. B2 docked-shelf restyle BUILT both surfaces
  (pending next install/deploy at time of writing).

### Phase 1d — Staged messages ✅ BUILT + DEPLOYED (26 Jul 2026)
- Intro bubble (carries the confirm button — it IS the preview) -> shimmer working
  state (real per-slot build events) -> result bubble. Result = model text on all-ok,
  factual composition from results array on any failure (never desync).
- Web gets identical shimmer + structure.
- After 1b+1d: the ENTIRE conversational feel is done, before any mutation code.
- SHIPPED, with live-test refinements:
  - Working row is the END-OF-LIST row (where "Analysing..." lived), NOT a
    message — stages never touch per-message heights, sidestepping the
    two-tH-sums bug class; stageRowH() is the single height source with
    exactly two audited consumers (paint + measure).
  - Real event labels only where real events exist (per-slot build loads,
    applyChainEdits onProgress per op); the model wait gets generic-safe
    lines. Shimmer: dim text3 base + #7FE3F2 band, 1.6s cycle (JUCE
    clipped-gradient two-pass + 33ms row ticker / web gradient-text mask).
  - STAGED REPLY RULE hardened: the ENTIRE prose is a 1-3 sentence intro;
    per-slot detail lives in the block, never prose. Builds render a
    structured SLOT CARD ("N. Name — settings", ops-card visual language)
    between the intro and Build this chain, both surfaces.
  - Result bubbles fire ONLY when they say something the retired card does
    not (clean apply + model "result" text); failures keep the card summary
    as the single record — no duplication.
  - Build failures get the same Suggest-an-alternative offer as edits, ONE
    follow-up covering ALL failures (see Gotchas), pill pluralised.

### Phase 1c — Structural edits ✅ BUILT + DEPLOYED (26 Jul 2026)
- extractChainEditBlock; ops-preview card ("+ add / - remove / move") with Apply-
  changes confirm; applyChainEdits sequencer (see Apply logic); Link v:2/editOps/ack.
- SHIPPED, with live-test refinements:
  - Slot numbering is 1-BASED everywhere the model or user sees it (the
    [CURRENT CHAIN] injection, prompt rules, preview cards, web card).
    Internal ChainHost indices stay 0-based; the ONLY conversion is in
    parseChainEditOps ("after":0 = insert first -> internal -1). The web
    card renders raw block JSON (no parse boundary): numbers display as-is,
    only the baseSlots array lookup shifts by -1.
  - Failure taxonomy: EXPECTED runtime failures (plugin load, runtime
    resolve-miss) CONTINUE — load-before-destroy makes a failed op a
    provable no-op and op indices are original-anchored, so independent
    later ops stay valid. Map-INVARIANT violations (tombstone lookups that
    pre-flight makes unreachable) hard-stop with "not attempted" padding —
    if the map is wrong, continuing is the unsafe move. Summaries are
    per-op honest ("Applied 1 of 2 — add failed: ...; removed ...").
  - Suggest-an-alternative: retired cards whose failure was a plugin LOAD
    failure grow a one-shot pill (AskChipLnF family, persisted _editAlt)
    that auto-sends a pre-formatted follow-up via the 1b answer-tap
    machinery; the prompt states the failure is authoritative and forbids
    re-proposing the same plugin. Never auto-substitutes. Aborts
    (staleness/invalid) never get the pill.
  - Load-failure feed exclusion is SESSION-SCOPED ONLY
    (ChainHost::sessionLoadFailed_, in-memory, gone on reload; fed by live
    chain-load failures only; consumed only by buildRecommendable; cleared
    per-entry by a successful load). See Gotchas for why persistence was
    rejected.
  - Follow-ups: settings_structured enrichment does not know edit blocks
    (prose settings on add/replace); web edit card is read-only (no rack on
    web); main-plugin -> Link edit SENDING not wired (Link v:2 handler +
    ack exist; sender needs a Link-rack injection first).

### Phase 2 — Parameter editing (GATED on param-mapping coverage)
- 2a: absolute re-dial of mapped slots (set op -> existing applyStructuredSettings).
  Infrastructure shipped; gated only on per-fingerprint map coverage. Preview shows
  dialable slots + per-param apply reports.
- 2b: relative edits ("make the EQ brighter"). Needs NEW parameter read-back (invert
  anchor tables), current values in injection, server semantic vocabulary. Real gate.

### Phase 3 — Extend conversation to EVERYTHING (the "yes everything")
Same block protocol applied to the AI's other v1 capabilities:
- Capture analysis conversational ("true peak is clipping — add a limiter?")
- Comparison conversational ("this ref is brighter/wider — match it?")
- General chat gets the same staged/ask treatment.
Each is "another consumer of the protocol," not new architecture.

### Phase 3.5 (optional) — SSE streaming for a real streamed intro
Only if shimmer does not feel live enough after 1d. Streams the intro from the same
single call (cannot desync). NOT a two-stage router.

## Testing gates
- 1b ask-frequency test: batch of real prompts ("build me a vocal chain", "add a
  compressor", "make it punchier", "add some warmth", "remove the reverb"). Clear
  requests must be ACTED on; only genuinely ambiguous ones ("add some warmth")
  trigger ASK. Review ask/act per prompt; tighten prompt if over-asking.
- 1c sequencer test: add/remove/reorder/replace with latent plugins and the AMEK-
  class deferred-removal case; confirm no use-after-free, indices remap right.
- Staleness test: change the rack between propose and confirm — edit must abort
  cleanly ("chain changed, ask again").
- Parity test: every shipped block behaves identically in plugin and web.
- Live-backend safety: existing chat/build still works after each deploy.

## Ship discipline
Ship at phase boundaries. Phase 1 (conversational build + edit) is a complete,
valuable product and the differentiator vs MixingGPT — get it into v2 and learn from
real use before Phase 2/3. Do not wait for "everything" to ship anything.

## Gotchas learned (live verification)
- CHANNEL SELECTOR PROJECT-SCOPING GAP (C4, known and ACCEPTED — do not
  "fix" the stamping): the banner dropdown (which REPLACED the chip bar;
  the queued two-row "+N" overflow cap is CANCELLED — a menu handles all
  16 registry slots) lists live Links in the SESSION, not the current
  project. Two DAW projects open at once put
  Project B's Link in Project A's bar; tapping it opens a chat stamped
  with Project A's trackName targeting a rack in Project B. Accepted:
  the registry is per-session; Logic runs both projects in ONE process
  (no PID/process-token discriminator to filter on); the stamping is
  correct given the information available — the gap is upstream in what
  the registry can tell us; and the Link monitor behaves identically, so
  it is consistent, not a new inconsistency.
- LOGIC RECREATES THE PLUGIN EDITOR whenever you switch between the Link
  window and EchoJay (single plugin window) — every couple of minutes in
  real Link work. ANY editor-instance state that matters across that
  boundary must live on the PROCESSOR (like chatHistory) or in the
  workspace. The constructor copy-back rebuilds role/content/waveform
  ONLY; full block state (edit cards, Build buttons, ASK chips, retired
  flags, edit targets) returns via the one-shot loadChatFromWorkspace
  re-hydrate in workspace.onLoaded (guards: user has not navigated away,
  no send in flight; one-shot because onLoaded re-fires on later syncs
  and a re-run would clear a live conversation mid-session).
- COMPOSE TARGET (the chat target pill) lives on the processor
  (chatTargetLinkUid/Name) for the same reason — an editor recreate must
  not silently retarget a Link conversation to the local rack. PER-CHAT
  targeting is deliberately DEFERRED to Phase C of the per-Link
  conversations plan, where the channel IS the chat — build no interim
  per-chat machinery.
- ANY TEST THAT CONSTRUCTS AUTH BY HAND CANNOT VERIFY AUTH (28 Jul 2026, M1).
  A Node suite and curl both set the Authorization header themselves, so every
  layer passed while the only path a real user takes was 401 on every request:
  the client sent no token at all. Generalises past auth: whatever a harness
  supplies for convenience is exactly what that harness cannot check. A
  hardcoded uid, a stubbed session, a preset flag, a hand-built payload. The
  plugin equivalent is a state blob assembled in a test rather than captured
  from a real hosted plugin.
- VISIBILITY IS WRITTEN PER COMPONENT IN resized(), NEVER THROUGH A POINTER
  LOOP, and every component needs an OFF path that runs on every tab.
  Two halves, both learned the hard way (28 Jul 2026, chain header Save /
  Save As / Open drawing on top of the Compare tab's own buttons):
  (a) bounds AND visibility authored inside `if (currentTab == Tab::X)` means
      the code never runs on any other tab, so the component keeps
      visible=true at tab-X coordinates. Author it UNCONDITIONALLY with the
      tab test inside the single visibility expression, so there is one
      authority that always evaluates BOTH ways. Hiding it in the tab-switch
      handler instead just moves the bug.
  (b) `for (auto* b : {&a,&b,&c}) b->setVisible(x)` is INVISIBLE to a grep
      for "<name>.setVisible", which is how anyone audits this file. The bug
      above survived its author's own audit for exactly that reason. Write
      each setVisible out per component even when it is repetitive.
  Watch for a SECOND authority too: applyReviewModalState also sets chain
  header chrome, and anything it re-shows must CONSUME the rect resized()
  stored rather than deriving its own.
- NON-ASCII IN JUCE DRAW STRINGS MUST USE EXPLICIT ESCAPES (\xe2\x80\x94
  etc), never literal characters: raw multi-byte bytes written into a
  string literal rendered as mojibake in the build card. The ops card's
  escape style is the pattern; audit any generated code for raw bytes.
- ALT-PROMPTS ARE NAME-ANCHORED, NEVER NUMERIC. The follow-up turn carries
  a fresh [CURRENT CHAIN], so the stored prompt must do NO positioning
  work: anchor by surviving-neighbour NAME ("right after \"X\"") or "at
  the start". Names fail soft (model re-places against the live
  injection); stored indices fail hard (stale by tap time -> pre-flight
  abort). The edit path had the same flaw via describeEditOp numbering.
- ONE FOLLOW-UP COVERING ALL FAILURES, never per-failure offers: applying
  one replacement bumps chainRevision and the next card's staleness guard
  correctly aborts it — per-failure offers serially kill each other.
- PRE-FLIGHT CLAMPS POSITIONAL TARGETS, ABORTS IDENTITY TARGETS.
  add.after / move.to out-of-range clamp to the rack end (an append one
  slot off beats a dead batch; runtime already clamped). slot refs on
  remove/replace/bypass/move-source still abort: clamping those would
  mutate the WRONG plugin.
- APPEND-AT-END NEEDS ITS EXPLICIT PROMPT FORM ("after": <last slot
  number>). Without it the model invented out-of-range positions
  (wrote the destination index, "after": 6 on a 5-slot rack).
- TAP-GENERATED USER TURNS DISPLAY A SHORT LABEL while the full
  instruction rides history unchanged (ChatMsg.displayText / _display /
  web msg.display; displayedText() is the single render source for both
  text-layout passes). Backend prompt text must NEVER render verbatim in
  a chat bubble.
- A LOAD FAILURE MEANS "CAN'T AUTHORISE RIGHT NOW", NOT "NOT OWNED". iLok
  absence makes genuinely-owned plugins fail to load on this machine while
  they work fine on another. NEVER persist load-failure exclusions and
  NEVER auto-untick the checklist from a load failure — a persistent
  load_failed.txt was built and then REVERSED for exactly this reason;
  exclusion is session-scoped in-memory only. (Checklist tick state,
  plugin_disabled.json, is per-machine local and must stay that way; only
  the profile plugin-list string is account-synced.)
- WAVES VARIANT-SUFFIX FEED KEYING: WaveShell AUs register per-variant
  component names ("Abbey Road Plates (s)"/"(m)") while the Settings
  scanner lists curated suffix-less names. buildRecommendable's exact
  normalized-name map missed them, silently dropping Waves plugins from
  the AI feed. Fixed with secondary stripParenthetical keys — keep both
  key forms if the map is ever rebuilt.
- AUTO-DIAL RESTRICTS NEW SLOTS ONLY; RACK PLUGINS ARE ALWAYS
  REFERENCEABLE. With auto-dial ON, the note framed the mapped set as
  "the ONLY valid source of chain slots" and the model told the user a
  plugin RUNNING IN THEIR RACK was "not in your available plugins".
  The note now carves out CURRENT CHAIN plugins (installed and running by
  definition; edits always legitimate; say "I can't auto-dial that one",
  never "unavailable"), and the [CURRENT CHAIN] injection states the same
  from the client side. Any future feed narrowing must keep this rule.

- CLIENT NAME-SCAN FALLBACK SYNTHESIZES CHAIN BLOCKS. PluginEditor has a
  legacy fallback that builds a chain block from ANY successful reply
  mentioning 2+ recommendable plugin names ("Chain extracted from reply
  text") — it manufactured Build buttons on "what's in my chain?" and on
  chain_edit prose (re-opening the destructive full-rebuild trap locally).
  Now GATED: it runs only when the reply carried a <<<ECHOJAY_CHAIN>>>
  opener (truncation salvage) or the server resolved chain_generate. Any
  future "why is there a Build button?" starts here.
- turnType IN PLUGIN LOGS IS THE CLIENT'S STAGED LABEL, NOT THE TRUTH. The
  plugin stages chain_generate on every feed-carrying turn; the server
  reclassifies via classifyChainIntent afterward. The server now returns
  resolvedTurnType in every chat response (plugin logs it as
  "EJChat: resolvedTurnType=...", server logs [turn-class] on reclassify).
  Never diagnose routing from the staged label.
- CHAIN_EDIT_INTERIM_NOTE KEEPS EDIT TURNS PROSE-ONLY UNTIL 1c. Since 1a,
  edit-verb prompts with a rack classify chain_edit; without the note the
  client's CHAIN BLOCK RULE could force a full chain block whose Build
  REBUILDS the whole rack (destroying hosted plugin state). The note (plus
  the same block-rule strip chat turns use) must be REPLACED, not just
  removed, when 1c ships real edit operations.
- CHANNEL SELECTOR DEPENDENCY: STRIP SELECTION IS THE ONLY SELECTOR ON A
  COLLAPSED LINK TAB (30 Jul 2026). The chip bar was deleted, which left the
  chat sidebar's channel banner as the only way to switch which channel a
  conversation targets. The sidebar can now collapse to zero width
  (processorRef.chatSidebarCollapsed), and it takes the banner with it, so a
  collapsed Link tab has NO channel selector at all. The Link mixer closes
  that gap by construction: TAPPING A STRIP SELECTS THAT CHANNEL. Strip
  selection is therefore load-bearing, not a convenience. Do not remove it,
  and do not gate it behind a view mode, without first restoring some other
  selector that survives a collapsed sidebar. ONE selection state, not two:
  strip and banner both read and write effectiveChannelUid() (the active
  chat's linkUid, else processorRef.pendingChannelUid). Both live on the
  PROCESSOR, which is also what makes selection survive the Logic editor
  recreate; a selected-strip index cached on the editor would not.
- ONE AUTHOR FOR BOUNDS, VISIBILITY AND HIT REGIONS, THE GENERAL FORM
  (30 Jul 2026, written after four instances in two days). The visibility note
  above and the chatBoxH height rule are both cases of one rule: any
  component's bounds, visibility or hit region has EXACTLY ONE author, and
  paint AND hit-testing both CONSUME what that author stored rather than
  recomputing it. A second computation of the same geometry is a bug that has
  not fired yet, because the two copies agree on the day they are written and
  diverge on the day one of them is edited. Evidence, all four inside 48
  hours:
    1. THE TAB STRIP: tab rects computed in two places, so the strip painted
       one set of widths and click-tested another.
    2. THE VISUALISATION PRESET HIT TEST: paint drew the preset strip to full
       width while mouseDown recomputed its bounds 280px short, so the right
       end of a visibly-drawn strip was dead to the mouse.
    3. THE TWO HEADER ICONS: position authored twice, so the pair drifted
       apart depending on which path ran last.
    4. A BUTTON WHOSE VISIBILITY WAS SET IN TWO PLACES: one path showed it,
       the other hid it, and which won depended on call order.
  Practical form: resized(), or a measure pass resized() calls, is the sole
  geometry author and STORES the rects; paint() measures nothing; mouseDown
  and hitTest index the stored rects. Where a painter and resized() must
  share a constant, the constant lives in ONE named place, never as a comment
  claiming the other copy "mirrors" it. The Link tab already carries exactly
  that smell today: paintLinkView hardcodes pad / title / host-card heights
  while resized() re-adds them as `topH + 16 + 26 + 64 + 6` under the comment
  "mirrors the panel painter's layout constants". Two authorities,
  self-documented.
  COROLLARY FOR THE LINK MIXER: a horizontally scrolling, variable-count,
  variable-width strip set is the highest-risk shape for this bug. Strip rects
  are measured ONCE per resize into a stored vector; paint indexes it and
  mouseDown indexes the same vector. computeColumns stays the single authority
  on the tab's available width, and no copy of the width formula is added.
- CROSS-VERSION SHM: A NEW FIELD NEEDS A PRESENCE BIT, BECAUSE 0.0f IS A VALID
  READING (30 Jul 2026, established while scoping the Link mixer meter view).
  Users run mismatched Link and main plugin versions, so state both directions
  before touching LinkMeterFrame or RegistrySlot.
  OLDER MAIN + NEWER LINK degrades correctly for free: new fields go in _pad,
  every prior offset is unchanged, sizeof(LinkMeterFrame) stays 128 (there is a
  static_assert), so kRegSize and the meterFrames() offset never move and the
  old main simply never reads the new bytes.
  NEWER MAIN + OLDER LINK IS THE HAZARD. The old Link never writes the field,
  so the new main reads whatever the shared page holds, which is ZERO and NOT a
  sentinel. The absent convention for a dB field is -100.0f; 0.0f is a
  perfectly valid measurement. So an ungated new field renders a FABRICATED
  READING, which is exactly the action-honesty failure the rest of this spec
  forbids. truePeakCur survives only because 0 dBTP is implausible enough that
  the display gates it; lra at 0 LU is plausible-but-wrong and is the existing
  soft spot.
  THE RULE: any future shm field ships with a `uint32_t fieldsMask` in _pad
  where 0 means "old writer, none of the new fields present", and EVERY new
  field's display is gated on its bit. No bit set, no reading, N/A.
  RegistryHeader::version cannot be used for this as things stand: it is
  written once (`version = 1` in openRegistry) and READ NOWHERE. Only `magic`
  is checked, via the init CAS. Whoever changes shm next has to start reading
  the version before they can rely on it.
- BANDS[] CANNOT ARRIVE: MULTI-BAND MOVES ARE INEXPRESSIBLE FLEET-WIDE
  (filed 1 Aug 2026, found chasing the AMEK manual-q telemetry). The
  server's validate-settings.js VOCAB is flat; a model-emitted bands[]
  array drops as "not in vocabulary" before it ever reaches the plugin.
  Consequences: the matcher's explicit settings.bands path in applyBands
  is dead code everywhere; every EQ request collapses to ONE synthBand,
  so a turn can address exactly one band of a five-band map (AMEK). The
  fix is server-side vocabulary (bands with per-band sub-validation
  against the same semantic ranges). LATENT CLIENT BUG TO FIX IN THE
  SAME SLICE: applySettings routes flat band-class keys into synthBand
  and then applies it only in an ELSE IF behind settings.bands - the day
  bands[] starts arriving, flat freq/gain/q riding the same turn are
  silently dropped with NO honesty entry, the exact silent-skip class
  apply-time honesty exists to prevent.
- SERVE-TIME METADATA NEVER REACHES A CACHED MAP (filed 1 Aug 2026).
  storeParamMaps stamps fpFetchedAt BEFORE the rev compare (deliberate:
  an unchanged-rev answer must clear the stale flag) and then `continue`s
  on unchanged rev, keeping the cached object wholesale - so a field the
  server stamps at SERVE time rather than build time (dialable) never
  lands on any map cached before the field existed, and the TTL path
  keeps re-confirming the old object forever. Verified live: the local
  AMEK map at rev b3f287f9537d reads dialable ABSENT while the server
  stamps it on every response. mapIsDialableForSignals is strict
  (absent = not dialable), so when kDialSignalsEnabled flips on, every
  veteran cache reads not-dialable until a content rev bump. Decision
  needed from whoever owns the rev contract: overlay serve-time fields
  onto the cached object on unchanged rev, or fold them into rev.
- TYPEDREADBACKMATCH HAS REVERTED A CORRECT WRITE TWICE IN ONE DAY (risk
  entry, filed 1 Aug 2026; not two unrelated fixes). First the q half-gap:
  a target at the exact bracket midpoint lost to float representation by
  ~3e-8 and a correct snap was reverted on every dial. Then the controls
  unit override: a control name derives no unit from semanticUnit, so an
  hz display like "1.2 kHz" parsed unitless as 1.2 against 1200 and a
  correct write reverted. The SHAPE both share: the verifier held a
  belief about the display (its tolerance, its unit) that was stricter or
  blinder than the plugin's own display contract, and the failure mode is
  VERIFICATION UNDOING REAL SUCCESS, which telemetry then reports as the
  write failing - the diagnosis points away from the verifier every time.
  A third bug will get a third point fix under time pressure; this entry
  exists so the function gets the systematic pass a point fix never
  forces: enumerate every assumption typedReadbackMatch + parseDisplayForUnit
  make about a display (unit source, locale/comma, token order, snap
  direction, negative-infinity forms, k-multiplier scope) and check each
  against the corpus BEFORE the next class fires. Until that pass exists,
  any new "asked X, plugin shows Y, value restored" telemetry cluster
  should be presumed a VERIFIER bug first and a wrong write second.

## Standing engineering rules (from prior work)
- Build via ~/reinstall-v2.sh (kills AU host, bumps version, rebuilds, installs
  atomically). It now derives REPO from `git rev-parse --show-toplevel` and
  configures the build tree if `$BUILD/CMakeCache.txt` is missing, so it builds
  whichever worktree you run it from and survives a `rm -rf build`.
- BINARY VERIFICATION IS A CONTENT CHECK, NOT A VERSION OR TIMESTAMP (28 Jul
  2026, learned the hard way). The old rule "version on screen = proof of fresh
  binary" is WRONG and cost a full afternoon: an installed component read
  v2.23.99 while containing object code older than three sessions' work. While
  more than one session or worktree can install to the SAME destination
  (~/Library/Audio/Plug-Ins/Components), the version counter and the file
  timestamp churn independently of the linked object code — a binary can read
  NEWER while BEING older (a stale .o linked into a freshly-stamped bundle, or a
  competing session's reinstall). The ONLY proof that an installed binary
  contains a given change is a CONTENT check: `strings` the installed component's
  Mach-O for a marker string from the feature under test
  (`strings "$HOME/Library/Audio/Plug-Ins/Components/EchoJay V2.component/Contents/MacOS/EchoJay V2" | grep -F "<marker>"`).
  Pick a marker that is a real string literal the feature emits; a pure code
  reorder has no marker, so verify those behaviourally instead. Neither the
  version number nor the mtime is evidence.
- Two Claude Code sessions must NOT build the shared repo simultaneously. Prefer
  separate worktrees; note the install DEST is still shared (last install wins)
  unless the branches carry distinct PRODUCT_NAME / BUNDLE_ID / PLUGIN_CODE.
- Confirm the installed binary by CONTENT (above), not by timestamp.
- Deploy the live backend via clean git flow; test before moving on.

## QUEUED, Link CPU: the 4096-point visual FFT runs on every Link and nothing reads it
Found 30 Jul 2026 while establishing the metering headroom for the Link mixer.

WHAT IT COSTS. Per active Link, MeterEngine::processBlock runs the FULL
main-plugin analysis suite, not a lightweight tap: a 2048-point FFT on EVERY
block (computeSpectrum, called unconditionally), a SECOND 4096-point "visual"
FFT every kVisHopSamples = 1024 samples (~43 per second), 4x-oversampled true
peak on both channels, K-weighting, three band-crest filter banks, correlation,
a width HPF, and a waveform ring push. At 44.1k/512 that is roughly 86
2048-point FFTs/s plus ~43 4096-point FFTs/s PER LINK. kRegMaxSlots is 16, so
a full session pays that 16 times over.

WHY THE VISUAL FFT IS SAFE TO GATE OFF IN THE LINK BUILD: it has NO consumer
there. LinkEditor.cpp never references meterEngine_ or any spectrum, and
LinkMeterFrame carries no spectrum bins at all. The work is computed and thrown
away on all 16 Links.

THE CAUTION, and it is the whole reason this is queued rather than done: the
frame's bandRel[6] comes from md.macroBandDb, which is integrated from the
ANALYSIS FFT (2048-point), NOT from the visual one. So the 2048-point FFT must
STAY. Gating the wrong FFT silently empties the band-relative readouts on every
Link strip. Verify bandRel is still populated after any such change.

The publish side is not worth optimising by comparison: getMeterData() is one
mutex lock and a struct copy, and publishMeterFrame is a 128-byte memcpy
between two release-stores. That is also why RAISING the publish rate above
10 Hz would be cheap if it is ever wanted; the only real constraint there is
dataMutex, which processBlock also takes, so a faster publish means more
contention with each Link's audio thread.
