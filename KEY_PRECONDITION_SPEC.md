# Key: the Meters panel, and the "I need the key first" precondition

Two connected features:
1. **Meters gets a KEY panel** — the detected key becomes a visible, first-class fact, not
   something buried inside one device's editor.
2. **A two-tier precondition** — when a request genuinely needs the key: add a detector to a
   bus Link and analyse it automatically if one exists; ask the user to place a Link only when
   none does. Never build blind, never ask for something EchoJay could do itself.

The capability that shapes both: EchoJay **can** build and edit a chain on a targeted **Link**
(the command path exists — `LinkProcessor` handles chain ops and acks them). So when a Link is
already on the music bus, EchoJay can add a Key Detector to it ITSELF rather than asking.

What it genuinely cannot do is **instantiate a Link plugin in the DAW** — that is the user's
action, in their session. So the ask, when there is one, is only ever "put a Link on your
instrumental bus", never "add this device for me".

## 1. METERS — the KEY panel

Meters already shows measured facts about the signal. Key belongs there.

**When a key IS being reported** (from a bus Link, or a Key Detector in this chain — see
KEY_DETECTOR_SPEC.md §9 for source preference):

```
KEY        F# minor          conf 0.85
TUNING     441.3 Hz  (+5 c)
SOURCE     "Music Bus" (bus)      age 4 s
ALT        A major (0.72)
```

Plus the note wheel (same `DwellGlow` treatment as the device, smaller) so it reads as one
family, and a RE-ANALYSE button that triggers a fresh committed pass on the source.

**When NO key is being reported** — this is the important state, and it must be actionable
rather than a blank readout:

```
KEY        not detected
           Put an EchoJay Link on your instrumental or mix bus —
           EchoJay will add a Key Detector to it and analyse
           the track's key for you.
           [ How? ]
```

**When a key is reported but LOW confidence** (below the ~0.5 rule): show it greyed with the
value visible and labelled `low confidence — treat as unknown`. Never hide it, never present
it as fact. A user can see 0.31 and decide for themselves; the AI is told to discount it.

### 1.1 WHERE it goes — a fourth panel in the middle row

Meters is laid out as: a full-width LOUDNESS strip; a middle row of LEVELS | STEREO IMAGE |
TONAL BALANCE; then a full-width SPECTRUM/SPECTROGRAM.

**KEY becomes a fourth panel in that middle row.** Its content is shaped exactly like STEREO
IMAGE — a square visualisation (the note wheel, where the goniometer sits) plus a short stack of
numbers beside it — so it reads as part of the family rather than bolted on. Four columns at
typical window width is ~360 px each, which the goniometer panel already demonstrates works.

Fallback if four columns prove too tight at small sizes: a compact KEY block in the top LOUDNESS
strip (key, confidence, tuning as three more stats). This loses the wheel, so prefer the middle
row and only degrade to the strip when width genuinely forces it.

### 1.2 THE SOURCE MUST BE UNMISSABLE — this panel is not about this channel

Every other panel in Meters measures the audio on the channel EchoJay is loaded on. **The KEY
panel usually will not.** In the common case it shows a reading taken from a Link on the MIX BUS
while the user is sitting on a vocal channel.

If that is not immediately obvious, the panel is actively misleading: a user sees "F# minor" on
their vocal's meters and reasonably concludes it was measured from the vocal — which is the one
source the spec says never to trust for key (5.3).

So the source is NOT small print. Requirements:
- The source line sits directly under the key, at readable weight — e.g. **"from Music Bus
  (bus)"** — not tucked in a corner.
- It is visually distinguished from the other panels' implicit "this channel" framing, so the
  difference is legible at a glance rather than on inspection.
- When the reading DID come from this channel's own chain (a Key Detector in this rack), say that
  too — "from this chain" — so the two cases are never ambiguous.
- When the only available reading came from a channel rather than a bus, name the stem and treat
  it as weaker (5.3).

**Why Meters and not just the device:** the key is a property of the session, not of one slot.
Meters is where a user looks to find out what is true about the audio, and it is where they
will look when the AI says it needs the key.

## 2. THE PRECONDITION — do it if you can, ask only if you cannot

### 2.1 Two tiers — do it, or ask for the one thing only the user can do

**TIER 1 — a Link exists on a bus: DO IT, do not ask.**
EchoJay adds an `EchoJay Key Detector` to that Link's chain via the existing Link chain command
path, triggers a committed ANALYSE, waits for the reading, then builds the vocal chain using it.
Say what happened, do not ask permission for a reversible, inaudible, zero-latency addition:

> Added a Key Detector to your **Music Bus** and analysed — the track is **F# minor**
> (confidence 0.85, tuning 441.3 Hz). Building the chain in key.

Preference when several Links exist: `placement == bus` over `channel`; if only channel Links
exist, use the one most likely to be the music and SAY which it used, since a single stem is a
weaker read than the mix.

**TIER 2 — no Link anywhere on a bus: ASK, because only the user can place a plugin.**

> I can build this. For the pitch correction I need the track's key, and I can only hear this
> channel — which is the vocal.
>
> Put an **EchoJay Link** on your instrumental or mix bus. I will add a Key Detector to it and
> analyse it myself — you just need to place the Link. You will see the result in
> **Meters → Key**.
>
> Want me to build the rest of the chain now and add the pitch stage after?

### 2.2 When the key matters at all (narrow, deliberately)

Neither tier fires for most chains. EQ, compression, de-essing, saturation, reverb do not care
about the key. Doing unnecessary work is only slightly better than asking an unnecessary
question — both are noise.

**Act only when the key materially changes the result:**
- pitch correction / autotune / harmony / doubler,
- explicitly musical asks ("in key", "tuned to the track", "melodic"),
- key-locked effects (tuned delay, resonator, harmoniser).

If the intent is ambiguous, do nothing and build the chain. Fail safe toward not interrupting.

### 2.3 How it resolves

Tier 1 resolves within the turn — the detector is added, analysed and read before the chain is
built. Tier 2 resolves by the user placing a Link; the next turn's feed then carries
`[DETECTED KEY]` (Tier 1 having fired automatically once a Link exists), so **the presence of
the fact in the feed IS the resolution**. No wizard state, no flag to clear.

If a Tier 2 ask goes unanswered, do not repeat it. Build without the key-dependent stage and
say plainly what was left out and why.

### 2.4 Implementation

- Tier 1 is a **client capability**, not a prompt trick: the decision to add a detector to a bus
  Link and analyse before answering should be a deliberate, logged step, and it must be
  idempotent — never add a second Key Detector to a Link that already has one.
- Tier 2 reuses the classifier's existing precondition/short-circuit path (the mechanism that
  returns a question and fires no main call, as `channel_mismatch` does). It asserts only when
  §2.2 applies AND no bus Link exists.
- Both must **fail safe**: ambiguous intent asserts nothing.
- The added detector is inaudible (zero latency, bit-identical passthrough), so Tier 1 is safe
  to do unprompted. Say it happened; do not ask first.

## 3. What the key is actually FOR, once known

Be honest about this in the teaching, because it sets expectations:

- **Third-party pitch correction** (Auto-Tune, Waves Tune, Melodyne…) — EchoJay does not ship
  a pitch corrector. The key's job is to set that plugin's key/scale parameter, via the normal
  third-party path, and — critically — to **tell the user the key** so they can verify it.
  A wrong key in an autotune is audible immediately; being told "F# minor, confidence 0.85"
  lets the user sanity-check before committing.
- **EchoJay's own devices** — musical placement rather than generic moves: keep a corrective
  notch off the root, put a dynamic bell on a degree that is fighting rather than one carrying
  the melody, and interpret a hunted resonance correctly (on the root = the instrument;
  between degrees = more likely the room).
- **Tuning reference** — `detected_tuning_hz` matters independently of key: a track at 441.3 Hz
  will fight a pitch corrector set to 440. Surface it, and set it where the target supports it.

## 4. Acceptance

- No Link anywhere: Meters → Key shows the actionable empty state; a plain vocal chain builds
  with NO key question; an autotune request produces the Tier 2 ask naming Link, not the device.
- A Link on the mix bus with no Key Detector: an autotune request adds the detector to that
  Link itself, analyses, reports the key in the reply, and builds the chain in key — with no
  question asked and no user action.
- Run it twice: the second request does NOT add a second Key Detector (idempotent).
- User places a Link after a Tier 2 ask: the next turn resolves via Tier 1 automatically.
- Low-confidence reading: Meters shows it greyed and labelled; the AI treats it as unknown and
  says so rather than silently using it.
- Ask twice in a row with still no key: it builds without the stage and explains — it does not
  repeat the question.

---

# 5. PASSIVE DETECTION — know the key before anyone asks

The best version of this feature is one nobody ever triggers. If the key is already known by
the time a chain is requested, Tier 1 has nothing to do and Tier 2 almost never fires.

Two places the key should be derived automatically, without a Key Detector device being added
by anyone:

## 5.1 Link metering (the common path)

A Link already runs an FFT every analysis cycle for its band/loudness work. Chroma
accumulation on top of that is incremental, so **Link should detect key passively as part of
metering** and publish it in `LinkMeterFrame` exactly as specced. Consequences:

- A Link on the mix bus means the key is **always already known**. No device to add, no
  ANALYSE to press, no question to ask.
- The Key Detector device stops being the mechanism and becomes the **visible, explicit** one:
  the full UI, the note wheel, the on-demand committed pass, and the option to analyse a
  specific chain's signal. Useful, but no longer the only way to know.
- Tier 1 of §2.1 collapses in the common case to "read the fact that is already there".

**Keep it cheap — this is the design constraint.** Gate on: transport rolling, signal present
above a floor, and a low duty cycle (an 8 s committed pass every ~30 s is ample; nobody needs
the key re-derived four times a second). Skip entirely when the input is silent. The added CPU
should be invisible next to what metering already costs.

## 5.2 Captures (the BEST input)

A capture is stored audio with no realtime constraint, which makes it the highest-quality key
source in the product: more material, no dropout risk, and the full HPSS + Viterbi treatment
can run over the whole thing rather than a rolling window. **Run a key pass over a capture when
it is made** and store the result with it.

- A capture of the mix or the instrumental gives the most reliable key reading available.
- Store `key`, `confidence`, `detected_tuning_hz` and the source alongside the capture, so it
  travels with the material rather than being re-derived.
- The offline pass may be slower and better than the live one — that is the point of doing it
  offline. Use a longer window and finer resolution than realtime allows.

## 5.3 Which reading wins

More sources means a precedence rule. In order:

1. A **capture of the music/mix** — offline, highest quality, but check its age and whether it
   is still the material being worked on.
2. A **bus Link's** passive reading — live and current.
3. A **channel Link's** reading — a single stem; usable, but say which stem it came from.
4. A **Key Detector device** in the current chain — only trustworthy if this chain IS the music.

Never prefer a reading taken from the channel EchoJay is on when that channel is the vocal.
A vocal is monophonic, sliding and often pitch-corrected: the worst possible key source, and
worse than having no key at all, because it is confidently wrong.

## 5.4 Staleness and re-detection

- Carry `age` on every reading; the feed already does.
- Do not silently re-detect mid-request. If a stored reading is old, use it and say so —
  "F# minor, measured 12 minutes ago" — rather than stalling a chain build to re-analyse.
- Re-detect on the events that actually invalidate a key: a new capture, transport jumping to
  a different section after a long gap, or the user pressing RE-ANALYSE. Not on a timer.
- The §2.2 narrowness still applies: none of this fires for chains that do not need the key.

## 5.5 What this does to the precondition

With passive detection in place, the flow for "build me a melodic vocal chain with autotune"
becomes, in the overwhelmingly common case: the key is already known from the bus Link, the
chain is built in key, and the reply mentions the key as a fact rather than a step. The Tier 2
ask survives only for the genuine cold case — no Link anywhere, no capture — which is exactly
when asking is warranted, because there is genuinely nothing else EchoJay could have done.


---

# 6. TWO FIXES FROM LIVE TESTING

## 6.1 The main plugin must self-detect when IT is on a music bus

**Bug found live:** EchoJay was loaded on the mix bus, with its channel-role selector reading
"Mix Bus" — and it still reported no key until a Link was added to another channel. It should
have detected its own channel.

The precedence rule in 5.3 ("never prefer a reading taken from the channel EchoJay is on") was
written for the EchoJay-on-a-vocal case and over-generalised. When EchoJay IS on the mix bus,
that channel is precisely the right source.

**The plugin already knows its own role** — the channel-role selector in the header (Mix Bus /
Master / channel types). Key off it:

- Role is **Mix Bus / Master / a music bus** → the main plugin detects its OWN channel passively,
  on the same scheduler and gating as a Link (5.1), and that reading ranks as a **bus reading**
  in the precedence order. No Link required, nothing for the user to add.
- Role is a **vocal / instrument / unknown channel** → do not self-detect as a primary source
  (the 5.3 poisoning rule still applies); fall back to Link or capture sources.

Restate 5.3's rule accordingly: the disqualifier is not "the channel EchoJay is on", it is
"a channel that is not the music" — determined by declared role, not by proximity.

Acceptance: EchoJay alone on the mix bus, no Link anywhere, music playing → Meters shows the key
within one duty cycle, sourced as "this channel (bus)".

## 6.2 Compact the KEY panel when the AI panel is open

**Problem found live:** with the AI panel hidden, KEY sits well as a fourth column in the middle
row. With the AI panel open the layout is narrower, KEY degrades to a FULL-WIDTH strip roughly
150 px tall whose content occupies the left third — a large band of empty space that pushes the
spectrum/spectrogram down and makes it too small.

**Fix: make the narrow form genuinely compact — one line, ~56 px.** Everything fits:

```
KEY   C minor   conf 0.09   from "music bus" (channel - one stem)   age 3s   441.0 Hz (+4.1c)   [RE-ANALYSE]
```

That returns ~90 px to the spectrum. Rules:
- Lay the values out horizontally on a single line; do not stack them.
- The note wheel is the only thing that does not fit. Drop it in the compact form, or reduce it
  to a small inline glyph at the right — it is a nice-to-have here and the full wheel lives in
  the device editor regardless.
- Everything else is preserved, including the prominent source attribution (1.2) and the
  low-confidence label — those are correctness, not decoration, and must survive the compaction.
- The empty and low-confidence states also collapse to one line.

**Rejected alternative** (recorded so it is not re-proposed): wrapping the middle row into a 2x2
grid keeps the wheel and gives every panel comfortable width, but doubles the middle section's
height — the opposite of the goal. Only revisit if the wheel is judged essential at all widths.

---

# 7. SOURCE SELECTOR — let the user aim the analyser

Precedence (5.3) picks a source automatically and is right most of the time. But it cannot
always be right: several Links may exist and it picked a stem rather than the mix; a channel's
role may be mis-declared; the user may simply know better. Give them the wheel.

## 7.1 The control

A **source dropdown in the KEY panel**, in both the wide and compact forms (compact: a short
label that opens the same menu — it is one control, not a second feature).

```
SOURCE   [ Auto — "Music Bus" (bus) ▾ ]
```

Menu contents, built from the SAME collector that feeds precedence — never a second enumeration
that could disagree:

- **Auto** (default, first) — shows which source it currently resolves to, so Auto is never
  opaque: `Auto — "Music Bus" (bus)`.
- Then every available source, each named with what it is and how fresh: capture / this channel /
  each Link by display name, with placement (bus | channel) and age.
- Sources that exist but are unusable still appear, **greyed with the reason** ("vocal role —
  not the music"), rather than being hidden. A user hunting for a channel that is not in the list
  has no way to know why.

## 7.2 Behaviour

- **Auto is the default** and keeps today's precedence exactly. Nothing changes for anyone who
  never opens the menu.
- **Pinning overrides precedence**, including the 5.3 poisoning rule — an explicit choice beats
  an inferred one, because the most likely reason to pin a "vocal" channel is that the role is
  mis-declared and the user knows it. Keep the confidence labelling and the source attribution
  exactly as strict; do not suppress warnings just because it was chosen.
- **A pinned source that disappears is stated, not silently replaced.** If the pinned Link is
  removed or goes stale, the panel says so ("pinned source \"Gtr Bus\" is gone — showing Auto")
  rather than quietly swapping underneath the user. Silent fallback is how someone ends up
  trusting a reading from somewhere they did not choose.
- **Persist the pin in state**, per plugin instance, so it survives a session reload.

## 7.3 The feed

`[DETECTED KEY]` must say when the source was **user-selected** rather than auto-resolved, e.g.
`source: "Gtr Bus" (channel - one stem, USER-SELECTED)`. Two reasons: the model should weight a
deliberate choice differently from an inference, and if a user pins something odd, the reason the
AI behaved oddly is then visible in the feed rather than mysterious.

## 7.4 Acceptance

- Default Auto behaves exactly as before; the menu shows what Auto resolved to.
- With two Links, pinning the non-default one changes both the panel and the feed.
- An unusable source is listed greyed with its reason, and can still be pinned deliberately.
- Removing a pinned source produces an explicit message, never a silent swap.
- The pin survives closing and reopening the session.
