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
