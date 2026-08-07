# Halting a wrong semantic

Written 7 Aug 2026, **before** the dashboard session, because this decides what
a report can and cannot do and it existed only in conversation. Nothing here is
built; `SERVER_CONTRACT.md` carries the endpoints, this carries the rules.

**It exists so that shipping model-proposed semantics is safe.** With ~12,850
escalations left unreviewed by choice, the corpus ships with answers no human
checked. That is affordable only if a wrong one gets caught and stopped by the
people using it — otherwise the pile has to be drained first, which it will not
be.

---

## The three verdicts

| what happened | evidence | what it does |
|---|---|---|
| **Automatic mismatch** — the dial landed in a unit the semantic contradicts | a MEASUREMENT the client already made | **halts that semantic on that fp, on one report** |
| **User report on a verified write** — "that changed the wrong thing" | one person's account | **queues. Serves unchanged until corroborated** |
| **Corroborated user report** — a second independent report, or a map-side check that agrees | two sources | **halts that semantic on that fp** |

**Halted means: the server stops serving that ONE semantic for that ONE
fingerprint.** Nothing else changes.

---

## Why a halt is per-semantic and not per-map

A map holds forty controls measured by a sweep that worked. One wrong
`attack_ms` is one bad row in a good map, and pulling the map would cost the
other thirty-nine — controls that dial correctly today, on a plugin the mapper
may never revisit.

**And the control does not stop being addressable.** `applySettings` resolves an
unmapped settings key by EXACT control name against `map.controls`, and has done
since 31 July. A halted semantic falls back to name-addressing: the model can
still ask for `"Sustain": 4` and it still lands. What is lost is the *generic*
request — "set the attack" — not the control.

That asymmetry is the whole argument. Halting a semantic costs one generic
phrasing. Halting a map costs every control on the plugin.

## Why it is not a tombstone

Withdrawal (`SERVER_CONTRACT.md` §2.9) is for a map whose **controls were
measured on another plugin** — SSL Fusion HF Compressor carrying Dangerous BAX
EQ Mix's six controls. Nothing in that map is a measurement of its own plugin,
so there is nothing to keep and a re-map is the only fix.

A wrong semantic is the opposite: the anchors, the ranges, the indices are all
real measurements of the right plugin. Only the LABEL on one row is wrong.
Tombstoning it would destroy good measurement to correct a naming error, and
would make a fixable row look like a poisoned one to every later reader.

**Withdrawal is for evidence that is not this plugin's. A halt is for a name
that is wrong about this plugin's evidence.**

## Why one automatic report is enough

Because it is not a report. It is a measurement the client already acted on.

`typedReadbackMatch` (`EchoJayParamApply.h:273`) compares the semantic's unit
family against the unit the plugin's own display declared after the write:

```cpp
if (! unitFamiliesAgree (unit, displayUnitFamily (landedText))) return -1;
```

A `-1` means the client **already reverted the write**. The plugin was asked for
`attack_ms −1.125`, the display read `-1.13 dB`, and dB is not a time. No
judgement, no user, no ambiguity — and the same rule found 2 of 2 errors in the
hold-out that four separate models missed.

Requiring a second one would mean serving a semantic the client has already
refused to write. The corroboration exists to guard against *opinion*, and there
is no opinion here.

## Why one user report is not enough

A user report is an account of what someone believes they heard. The server
cannot check it, and three things follow:

- **Auto-correcting on one report makes every user's mistake everyone's
  outage.** A single misheard change would pull a working semantic for every
  studio.
- **It makes the corpus writable by anyone with the app.** Same reasoning that
  keeps a mapper's queue decision scoped to their own map.
- **The base rate is against it.** Measured two-arm accept precision is ~98.7%
  (qualified: the hand-answer standard is partly model-sourced). Most reports on
  a correct semantic will be about something else — the wrong slot, a stale
  chain, an expectation the plugin does not meet.

So a lone report **queues and serves unchanged**. What promotes it:

- a **second independent report** on the same `(fp, semantic)` — the only signal
  that scales;
- **map-side corroboration**, none of which needs the user trusted:
  - the unit family contradicts the swept unit → the map was wrong before any
    report;
  - `evidence.readback` holds asked/wrote/read per semantic, and a display that
    never parsed cleanly in that family is latent evidence;
  - **re-run the proposer** over the same control evidence — if two-model
    agreement now disagrees with the map, that is a second opinion the report
    did not supply.

## What a halt records

Enough that a later reader can undo it, and never so little that "why is this
not served" is unanswerable:

```json
{"fp":"…","semantic":"attack_ms","index":7,
 "halted_at":"…","halted_by":"automatic-mismatch|corroborated-report",
 "evidence":{"asked":-1.125,"landed":"-1.13 dB",
             "semantic_unit":"ms","display_unit":"db"},
 "reports":[{"by":"<mapper ref>","at":"…","kind":"wrong-knob"}]}
```

A halt is **reversible and expected to be reversed**: re-derivation corrects the
row, the correction records the report that triggered it, and the semantic is
served again. That is the loop closing, not an error being buried.

## The honest limitation

**Report coverage is proportional to use.** A wrong semantic on a plugin nobody
dials is never reported, and the field will never sample the long tail.

This is why the automatic path matters more than the report path: it needs no
one to notice, and it fires on any dial that lands in a contradicting unit. The
report path catches the residue that no rule can see — a wrong knob in the right
unit — and it should be built second.

Neither reaches a control nobody ever asks for. Those stay name-addressable and
unexercised, which is the correct place for them.
