# EchoJay v2 — Conversational Chain AI: Build Spec

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

### Phase 1d — Staged messages (do before 1c; low risk, completes the feel)
- Intro bubble (carries the confirm button — it IS the preview) -> shimmer working
  state (real per-slot build events) -> result bubble. Result = model text on all-ok,
  factual composition from results array on any failure (never desync).
- Web gets identical shimmer + structure.
- After 1b+1d: the ENTIRE conversational feel is done, before any mutation code.

### Phase 1c — Structural edits (the risky one; lands after the shell is proven)
- extractChainEditBlock; ops-preview card ("+ add / - remove / move") with Apply-
  changes confirm; applyChainEdits sequencer (see Apply logic); Link v:2/editOps/ack.

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

## Gotchas learned (1a/1b live verification — read before 1c)
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

## Standing engineering rules (from prior work)
- Build via ~/reinstall-v2.sh (kills AU host, bumps version, rebuilds, installs
  atomically). Version on screen = proof of fresh binary.
- Two Claude Code sessions must NOT build the shared repo simultaneously.
- Confirm binary timestamp > newest source before installing.
- Deploy the live backend via clean git flow; test before moving on.
