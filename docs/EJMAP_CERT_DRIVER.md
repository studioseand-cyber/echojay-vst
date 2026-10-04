# EJ Map certification driver: design requirements

Branch `feat/ejmap-cert`. The driver is step 5 of the certification plan and is
not written yet. These are requirements established by measurement before it
exists, recorded where whoever writes it will find them.

The split is decision D2 (28 Sep): the signed probe (`tools/au_instantiate_probe`)
MEASURES, and EJ Map DERIVES and ORCHESTRATES. The test for where a piece of logic
goes is whether changing it needs re-measuring or only re-computing. A formula
change must never cost a rebuild and a re-sign.

## 0. Acceptance criterion for step 5, stated before any driver code

The driver's first job is regenerating the 74 pushed compressor-profile fixtures
(echojay-saas `origin/main` `scripts/fixtures/compressor-profiles/`) from live probe
runs. It CANNOT reach most of them on the dev Mac, and its proof must say so rather
than borrow the size of the whole set. Measured 28 Sep:

| Class on the dev Mac | Products |
|---|---|
| installed at the fixture's version, not PACE-wrapped | 14 |
| installed and present, but the fixture records no version (AUMultibandCompressor: its `version` field holds the component code) | 1 |
| HELD until step 2 closes: 10 PACE-wrapped at the fixture's version, plus the 2 McDSP APB fixtures (also no version recorded), which are PACE-wrapped and register without an `AudioComponents` key, so the driver cannot find their bundle and holds them rather than assume them clear | 12 |
| installed at a DIFFERENT version (all 42 are Waves 12 vs 15 or UAD 11.2 vs 11.8) | 42 |
| not installed | 5 |
| **total** | **74** |

So 15 are reachable today. (Before the first driver run this table said the two APB
fixtures were "not installed". A plist scan cannot see them; the AU registry can, and
the first run found them. See below.) kHs Compressor hung the probe past its own bound
once on 28 Sep, then answered normally in both driver runs, so its hang is intermittent.

**THE FIRST RUN (28 Sep) BROKE THE HOLD, AND THIS IS RECORDED RATHER THAN HIDDEN.** The
first version treated "no bundle found" as "not PACE", so it ran `--list-params` and
`--text-at all` on the two APB compressors, which are PACE-wrapped, while step 2 was open.
Both answered in full and matched their fixtures. The driver watched no windows, so
whether a dialog appeared is UNKNOWN, and this is NOT step 2 evidence. The hold now fails
safe: a component whose PACE state cannot be checked is held and says why. Apple's own
built-ins are the only exemption.

**MEASURED OUTCOME (28 Sep, second run): reproduced 14 of 15 reachable, of 74.** The
fifteenth, Shadow Hills Class A Mastering Comp, differs in exactly ONE field:
`controls[41].defaultOnInstantiate.normalised`. Control 41 is "VU Meter R", a meter the
plugin exposes as an automatable parameter. Its value on instantiate differs in every
fresh process: 0.153, 0.162, 0.170, 0.165 and 0.173 in five runs, against 0.139 in the
pushed fixture. That fixture field recorded one sample of a moving meter, so NO run can
reproduce it. By the criterion above this is a miss, and it is counted as one. Making it
reproducible needs a MEASUREMENT change (instantiate twice and flag a control whose value
differs as a readout), which is a probe and schema decision, not a driver fix.

**THE CRITERION:** the driver reproduces every fixture it can reach on this machine.
Concretely:
- Every reachable product whose probe runs answer regenerates with ZERO field
  differences from its pushed fixture. Only the provenance fields are not compared:
  `sampledAt`, `probe`, `defaultsSampledAt`, `defaultsProbe`.
- Every reachable product that does not answer is recorded with a NAMED reason
  (timeout, refused and its text, or crash and its signal). It counts as NOT
  reproduced. It is never a pass and never silently dropped.
- The run report's FIRST lines are the coverage above, followed by the reproduced
  count as "N of 15 reachable, of 74". It never reads as "74 reproduced", and the
  out-of-reach products are listed BY NAME with their class.
- Separately, `--cert-rederive` re-derives `range`, `direction` and `unit` from each
  fixture's own `displayAt` for ALL 74 (1,783 controls). That needs no probe and no
  installed plugin, so it is the one check that covers the whole set. It proves the
  derivation code, not the measurements.

**RE-DERIVED WITH THE DONGLE IN (29 Sep, after step 2 closed; `--cert-defaults
--include-pace`, 388 s, 73 probe attempts, no retries).** This supersedes the no-dongle
table above for THIS Mac as it now is:

| Class of the 74, dongle in | Products |
|---|---|
| answered, REPRODUCED exactly | 21 |
| answered, differs ONLY on a detected readout (Shadow Hills Class A, VU Meter R) | 1 |
| answered, DIFFERS (MCompressor 6 fields, MModernCompressor 27; both Melda, see below) | 2 |
| showed a PACE activation window, killed by the watch (kHs Compressor, see below) | 1 |
| not measured: requires external hardware (McDSP APB C673-A, C-18) | 2 |
| installed at another version (Waves 12 vs 15, UAD 11.2 vs 11.8) | 42 |
| not installed | 5 |
| **total** | **74** |

**Result: reproduced 21 of 25 reachable, of 74.** Separately: emitted the readout fields
correctly on 24 of 25 (kHs was never checked, because it never answered).
`unlicensed_on_host` was **0**. All 8 PACE-wrapped products from the vendors expected not
to load here (Softube: Drawmer 1973, Tube-Tech CL 1B, Mike-E Comp; SSL: Native X-Comp v6,
G3 MultiBusComp, Native Bus Compressor 2; Audified U73b; Antares Auto-Tune Vocal
Compressor) answered and REPRODUCED their fixtures exactly with the dongle in.

Two findings, recorded as observed and NOT diagnosed:
- **kHs Compressor showed PACE's activation UI** (`PACEEdenExperience`, in the probe's own
  process tree, 3.4 s in). Yet by the driver's PACE rule, which reads only the plugin's
  bundle, kHs is NOT licence-bound: it has no Eden bundle and no PACE bytes. So the
  bundle-only rule misses at least one product that brings up PACE. The same product
  answered normally in three earlier runs and hung once on 28 Sep, before any window was
  watched. Its classification here is "not reproduced (named)", not `unlicensed_on_host`,
  because nothing marked it licence-bound. That rule gap is logged, not fixed.
- **Both Melda compressors differ at the fixture's own version (14.16.0)**, and the
  differences are FORMATTING, not measurements:
  - control names are ordered differently ("Preset trigger - previous" here, against
    "previous (Preset trigger)" in the fixture);
  - some middle texts read "63 ms" against "62 ms";
  - one control's top end reads "+96.00 dB" against "Off".

  The pushed fixtures were sampled on another Mac, so a machine-level Melda display setting
  is one possible explanation. It is not measured. Until it is, a Melda fixture is not
  portable between machines on names alone.

The ~40 dongle-bound products outside the fixture set (the 2 Sep list: SSL, Softube,
Harrison, oeksound, Audified, Eventide) have NO pushed fixtures, so nothing here measures
them. Only the 7 of them inside the 74 were run, and all 7 reproduced.

**WHAT "REPRODUCED N OF 15" COMPARES, AND WHAT IT DOES NOT.** Read this before
quoting the number.
- **It compares:** every top-level field (`identity`, `product`, `format`, `uid`,
  `version`, `certified`, `note`, `listParamsRc`, `textAtRc`) and, on every control,
  `index`, `name`, `unit`, `displayAt`, `numSteps`, `discrete`, `range`, `direction` and
  `defaultOnInstantiate`, taken from the MEASURED form: the fixture composed exactly as
  it was before item 12.
- **It excludes provenance:** `sampledAt`, `probe`, `defaultsSampledAt` and
  `defaultsProbe`. These record when and with what a fixture was made, so they differ by
  construction.
- **It excludes the item-12 readout fields:** `readoutCheck`, `readout`, and the `null`
  that replaces a readout's `normalised` and `display`. The score is computed on the
  measured form, so these fields never enter it. This is deliberate. The pushed fixtures
  predate item 12 and carry none of them, so comparing the emitted form would score the
  schema change, not the measurement. Excluding them keeps the number comparable to the
  14 of 15 measured before the change, and it is the ONLY evidence the driver reproduces
  measurements.
- **So it says nothing about whether the readout fields are right.** That is a separate
  line, "emitted the readout fields correctly on N of 15". It is asserted by checks C1-C5
  (`EjmapFixtureReadout.h` checkEmission) against the measured form, because no pushed
  fixture exists to compare it to. The two lines answer different questions and are never
  merged.

**LOGGED 28 Sep, NOT FIXED: the driver's exit code is permanently 1 on this machine.**
`--cert-defaults` exits 1 whenever any reachable product differs, and Shadow Hills Class
A always does. Its pushed fixture holds one sample of "VU Meter R" (0.139141). No run
reproduces that number, so `differ only on detected readouts` is 1 on every run here and
cannot reach 0. By the criterion that is HONEST. But a gate that is always red invites
someone to soften it later.

- **THE CORRECT RESOLUTION is a ONE-PRODUCT re-sample of the Shadow Hills pushed
  fixture.** It is possible only now that the readout encoding exists: its control 41
  becomes a `readout` with a `null` `defaultOnInstantiate`, and it reproduces from then
  on. That is a write to the SERVER tree, so it is the FIRST item for the server half
  (section 7). It is not a Mac-side change, and it is not a reason to touch the other 73.
- **THE WRONG FIX is excluding readouts from the pass condition.** Do not make
  `differ only on detected readouts` count as a pass, and do not drop readout controls from
  the reproduction comparison. The difference is REAL: the pushed fixture carries a number
  that is not a measurement of anything settable. The pass condition's job is to say so
  until the fixture is corrected, not to learn to stop saying it. Any future readout found
  in another pushed fixture gets the same treatment: a one-product re-sample, never a
  softer rule.

The 10 PACE-wrapped products join when step 2 closes. The 49 at other versions or not
installed are out of reach on this machine. A run on a machine that has them is what
reaches them.

## 1. Sign once, at the start of a run, and fail loudly if signing blocks

PACE refuses an unsigned probe ("fatal wrapper bootstrap error", 21 Sep, df407ab),
so every run starts from a signed binary.

Measured 28 Sep on the dev Mac: `codesign` with the Developer ID, `--timestamp`,
`--options runtime` and the entitlements took 1.13 s, then 0.49 s and 0.48 s. No
keychain prompt appeared (SecurityAgent never ran), because the key already
allows codesign.

That holds only while the login keychain is unlocked. A locked keychain (after
logout, or with lock-on-sleep), or a session with no logged-in GUI, can put up
a prompt. `codesign` then blocks and does not fail. A driver that signs lazily,
or per product, would hang in the middle of an unattended batch.

Therefore:
- Sign exactly ONCE, before the first product, and verify the result
  (`codesign --verify --strict`).
- Put a deadline on the signing step (a few seconds against a 1 s measurement).
  If it has not returned, abort the whole run before touching any plugin, and say
  that signing blocked and a keychain prompt is the likely cause.
- Never sign again mid-run. The binary does not change during a run.
- Record the probe's identity in the run report: its code-directory hash, or its
  UUID from `dwarfdump --uuid` / `otool -l`. That records which probe measured
  what; the version string does not.

## 2. The driver owns every timeout

The probe has its own 30 s bound in list mode, but it cannot interrupt a call
that blocks inside plugin code. Measured 28 Sep: `--list-params` on kHs
Compressor (AU manufacturer code `" kHs"`, with a leading space) printed its
header line and then nothing for more than 60 s, past its own bound, until it was
killed from outside. Nothing was recorded about why. The leading space is not the
explanation: JUCE's stringToOSType keeps a padded code intact (it trims only when
4 characters would remain), and Decapitator's padded "DEC " instantiated normally.

Therefore the driver runs every probe with its own deadline and kills the process
on expiry. It records a timeout as a timeout, not a refusal: the retry rule is
"-15 / killed = re-run alone once, and two timeouts mark a hang". The probe's own
bound is a convenience, never the guarantee.

## 3. Unlicensed on this host is not broken

Every run happens on one machine with that machine's licences. A licence-bound
plugin without its licence is **UNLICENSED ON THIS HOST**. That says nothing
about the plugin. Spec section 6 already has the behaviour: leave the product at
`mapped` with reason `unlicensed_on_host`, and let a machine that has the licence
pick it up. The driver must never read a stub, an empty instance or a hang as
evidence about the plugin: not `uncertifiable`, not `error`, and never a count
toward quarantine.

Shapes seen on the dev Mac (no iLok dongle present), 28 Sep, all through the
signed standalone probe with `--list-params`:
- **Empty instance.** Decapitator AU 5.4.3 (PACE-wrapped): exit 0 in 7.4 s, an
  instance, 0 parameters, no dialog. EJ Map's extractor also recorded 0 for this
  AU on 27 Jul; its VST3 has 12. It cannot be scored: "no licence" and "this AU
  shows nothing outside a DAW" look identical.
- **Activation UI, then a hang.** Eiosis E2Deesser AU 1.1.6 (PACE-wrapped;
  expected 67 parameters from ejextract 27 Jul and the EJ Map map 7 Aug): about
  1.1 s into instantiation the wrapper launched PACE's "Software Activation"
  (`PACEEdenExperience`) as a child with a window. The probe never got an
  instance, printed 0 rows, and was killed at 90 s; the activation process exited
  with it. It was NOT the "fatal wrapper bootstrap error" (100001) dialog.
- **Fatal wrapper bootstrap error.** Seen 21 Sep with an UNSIGNED harness (df407ab).
  Not seen with the signed probe.

**WINDOW WATCHING IS NOW IN THE DRIVER (28 Sep), and it is the precondition for any
PACE subject joining.** Every probe run is polled every 250 ms for ON-SCREEN windows
owned by the probe or any descendant, found by walking parent pids. PACE's activation
UI is a child of the probe. On the first such window the driver kills the whole tree
and records `SHOWED A WINDOW (<owner>)`. That result is not retried: a retry would only
put the dialog up again. `--cert-watch-selftest <helper>` proves it with a helper that
orders in a 10x10 window 20,000 px off every display. The user never sees it, but
CoreGraphics counts it as on screen, so the PRODUCTION setting is what gets tested. It
was caught and killed in 0.3 s as a direct child and as a grandchild, and a child with no
window was left alone. GREEN.

So a licence problem can present as an empty instance, a hang behind an
activation window, or an exit. The driver cannot tell these from real defects by
the probe's output alone. It therefore needs an independent "is this licensed
here" signal, or it must treat every such shape from a known licence-bound
vendor as `unlicensed_on_host` and say so in the run report.

An activation window is a UI event on the user's desktop. An unattended run must
count it and name it in the report, not just time it out silently.

**STEP 2 CLOSED 29 Sep: PASSED, on the evidence of 28 Sep.** The question was whether
PACE refuses a standalone signed probe, which would mean the harness can only live inside
a shipped plugin bundle. On 28 Sep the signed standalone EchoJayProbe (cdhash
`f7ffe8eb…`) ran `--list-params` and `--text-at all` on both McDSP APB compressors. Both
are PACE-wrapped (an Eden bundle and PACE bytes in the binary). Both returned COMPLETE
parameter lists that matched their pushed fixtures field for field (4/4 and 8/8
controls). A refused process does not return complete, matching data, so PACE did not
refuse the standalone signed probe, and the architecture stands.

What this evidence does NOT cover, stated so it is not over-read:
- **No window watch was running on 28 Sep** (it did not exist yet). So it cannot say that
  no dialog appeared. It says only that instantiation and enumeration both succeeded,
  which a BLOCKING dialog prevents.
- **It is two plugins from ONE vendor.** It shows PACE does not categorically refuse a
  standalone signed host. It does not show that every PACE-wrapped product will load.
- The same two plugins did not answer on 29 Sep with the dongle in (below), and McDSP is
  now out of the pool as hardware-bound. That is recorded, not re-litigated. Step 2 is
  closed deliberately on the result already in hand, without spending runs to find a
  cleaner version of it.

The closure does NOT settle E2Deesser's activation window, which was dropped as a subject
(it is not a compressor and not in the fixture set). It also does not settle why any
particular PACE product fails on this Mac: that is licensing, recorded as
`unlicensed_on_host`.

**A SECOND CATEGORY THAT IS NOT MEASURED: plugins that require external hardware
(logged 29 Sep).** A plugin that processes audio only on attached hardware cannot be
certified by rendering. It may enumerate, but it renders nothing without the hardware.
Record it as such, with the reason `requires_external_hardware`, the same way
`unlicensed_on_host` is recorded. Never measure it and never score it. The first known
members are the McDSP APB plugins, which need McDSP's Analog Processing Box (per Kathy).
On the dev Mac, with the dongle in, 29 Sep, through the same signed probe binary (cdhash
`f7ffe8eb…`) that listed both in full on 28 Sep without the dongle:
- **APB C673-A:** `--list-params` refused after 87 s with "An OS error occurred during
  initialisation of the plug-in (-10847)", which is `kAudioUnitErr_Unauthorized`.
- **APB C-18:** timed out at 90 s with no output past its header.

Neither showed a window. These are recorded, not diagnosed. McDSP is out of the step-2
subject pool.

**LICENCE-BOUND BY BEHAVIOUR (29 Sep): PACE's UI outranks the bundle scan.** kHs Compressor
brought up `PACEEdenExperience` in the probe's tree, although its bundle carries no PACE
markers. A product that shows PACE's activation window IS licence-bound, whatever its bundle
says. The driver now records such a failure as `unlicensed_on_host` ("licence-bound BY
BEHAVIOUR"), not as a defect (`EjmapCertOutcome.h`; pins K1-K4). A window from something
other than PACE is still recorded as a window, but is not called a licensing fact. A hang
with no window stays "not reproduced". Dropping the behavioural clause reddens exactly one
pin (K1).

**THE 2 SEP VENDOR-GAP TABLE IS SUSPECT (29 Sep).** The 2 Sep table of dongle-bound vendors
that did not load (SSL 28, Softube 4, Harrison 4, oeksound, Audified, Eventide: about 40
products) was measured WITHOUT the dongle. With the dongle in, every PACE-wrapped product
from those vendors that is in the fixture set answered and reproduced its fixture exactly:
7 of 7 (Softube 3, SSL 3, Audified 1; the eighth PACE success that day, Antares, is not a 2
Sep vendor). That table needs RE-DERIVING with the dongle in before anyone plans work from
it. The ~33 of its products outside the fixture set have not been run at all.

**Dongle-bound subjects on the dev Mac** (per Sean, 28 Sep; not measured here).
A full certification run on this Mac will SILENTLY UNDER-COVER these, and the run
report must say so: SSL (28 products), Softube (4), Harrison (4), oeksound,
Audified, Eventide.

## 4. Sidechains are enabled and fed silence, and the fixture says so

Ruled 28 Sep: every certification render keeps every bus the plugin declares
enabled at its declared layout, and feeds silence to every input outside the
main bus. The reasoning is in the header of
`tools/au_instantiate_probe/probe_render.h`. In short: disabling a declared bus
measures a configuration the plugin never ships in.

The probe prints a `policy sidechain enabled_silent` line and one `sidechain`
line per non-main input bus, as measured after configure. The fixture writer
must carry that state on every certification record.

The reason is a known risk. A compressor that defaults to EXTERNAL sidechain
keying keys off silence and never compresses, so its sweep reads flat at every
position and level. That lands in the existing `flat` result, which is correct,
but a `flat` with an enabled-and-silent sidechain has to be readable as "maybe
keyed externally", not blamed on the threshold. Never report one without the
other.

## 5. Every write is confirmed; no delay is ever assumed

Spec section 4.3 sets a position and then renders. On a bridged plugin a write lands
**34-69 ms after it is made** (measured 28 Sep on API-2500, whether the host pumps,
renders or sleeps), and a read taken straight after the write returns the OLD value.
Offline rendering runs at about 200x realtime, so "a few blocks later" can still be
before the write lands. The full measurement is in the header of
`tools/au_instantiate_probe/probe_write.h`.

So the sweep uses the probe's three-step verify for EVERY position, and never an
assumed delay or block count:
1. Confirm the property: pump until `getValue()` matches, bounded at 500 ms (the
   longest seen was 70 ms). On timeout, record `write_unlanded` and skip that position.
   The property never lagged the audio, so a confirmed read means the next block
   reflects the write.
2. Only where display text is the evidence (the §4.6 dB-threshold row): wait for a
   stable read, two reads 20 ms apart, bounded at 300 ms (measured 21-22 ms).
3. After the 1.5 s hold, re-read `getValue()` and the text. If either moved, discard
   and re-render that position.

Measured cost on the bridge: about 45-70 ms per write to confirm, plus about 21 ms
where the text is needed. Position-outer (16 writes), that is about 1-1.5 s per plugin.

## 6. Readouts: item 12 on the mismatch list, AGREED and EMITTED 28 Sep

**AGREED as written below, with `null` chosen over the first sample.** The reason:
`defaultOnInstantiate` means "the value the plugin holds on instantiate", and a meter has
no such value. Null is the truthful encoding. The first sample is a number that looks like
a measurement and is not.

**EMITTED since 28 Sep.** `applySchema` (`EjmapFixtureReadout.h`) turns the measured form
into this shape, and that is what the driver writes. It is checked by `checkEmission`,
which never calls `applySchema`. There are five checks, and each reports at most one line,
so one defect reddens one check:
- **C1:** `readoutCheck` is present with the agreed content exactly when the check ran.
- **C2:** `readout` is on exactly the controls that moved, carrying their samples.
- **C3:** `normalised` and `display` are null on exactly those controls.
- **C4:** `declaredDefault` survives on them.
- **C5:** every other control's `defaultOnInstantiate` is untouched.

Pinned in `testFixtureReadoutEmission` (10 checks). Mutating `applySchema` to drop
`readoutCheck` reddens exactly one pin (P1). Mutating it to keep the first sample instead
of null reddens exactly one pin (P3). The checker's own pins run on a hand-built fixture,
never on the transform's output, so they cannot share a failure with it. How the emission
line relates to the reproduction score is set out in section 0.

A control whose value differs between two FRESH instances is a READOUT, and its
`defaultOnInstantiate` is meaningless. Ruled 28 Sep:
- The Shadow Hills Class A fixture records one sample of a moving meter, and no run can
  reproduce it.
- It is not one control. Nobody knows how many of the 1,783 are readouts, and every one
  carries the same defect silently.
- It protects the sweep. A meter that gets a role would be swept, and the measurement
  would be of the meter wandering, not of anything that was set.

It is a SCHEMA change, because the server will read it.

**Measured (28 Sep, the driver's third run):** checked on 15 of 15 reachable products,
two fresh instances each. 1 control moved: Shadow Hills Class A "VU Meter R", 0.189068
then 0.186410, which is also the one field that stopped that fixture reproducing. The
driver writes readouts only to `readouts.json` beside its report, never into a fixture,
until the shape below is agreed. The 74 pushed fixtures are NOT re-sampled. Any meter
samples they contain are a known, recorded condition, corrected when a machine that has
the plugin re-runs them.

**THE RULE'S LIMIT, stated with it:** it is sufficient, not necessary. A meter that
reads the same in both instances is NOT caught. Shadow Hills' "GR Meter" and "Eye
Meter" both read 0.000 twice. So "no readout flag" means "did not move between two
instances", never "is a control".

**THE SHAPE** (agreed; not yet emitted by the composer):

```json
"readoutCheck": { "method": "instantiate_twice", "instances": 2 },
"controls": [
  {
    "index": 41, "name": "VU Meter R", "...": "...",
    "readout": { "samples": [0.189068, 0.186410], "displays": ["", ""] },
    "defaultOnInstantiate": {
      "normalised": null, "display": null, "declaredDefault": 1.0,
      "note": "a readout: its value moved between two fresh instances, so it has no instantiate value"
    }
  }
]
```

- `readoutCheck` sits at fixture level, and its PRESENCE says the check ran. The 74
  existing fixtures lack it, which reads "never checked", not "no readouts". (An
  absence needs its presence condition.)
- `readout` appears only on a control that moved, carrying both samples as evidence.
- On a readout, `defaultOnInstantiate.normalised` and `.display` become `null`, and
  `declaredDefault` stays: what the plugin CALLS its default is still a fact. The
  alternative is to keep the first sample, which is backward-compatible but silently
  wrong for any reader that ignores `readout`. **Recommended: null.** A reader that has
  not learned the new field then cannot mistake a meter sample for a default, but the
  server must handle a null `normalised`. That is the server-side change this item needs.
- Consumer rules the server half would adopt: never give a role to a control with
  `readout`, and never apply the threshold working-position rule to one.

**LOGGED, NOT BUILT (28 Sep): a stronger readout detector.** Render mode makes it
cheap: feed a tone and read the control afterwards. A meter moves with signal; a
parameter does not. That would catch the meters the two-instance rule cannot, such as
Shadow Hills' GR Meter and Eye Meter sitting at 0.000 in silence. It is deferred
because an uncaught meter degrades gracefully: the sweep writes to it, the audio does
not respond, and it lands in `flat` and then `uncertifiable`. Worth having; not worth
the scope yet.

## 6b. OPEN DESIGN QUESTION: "wrong" and "machine-dependent" look the same (logged 29 Sep)

Certification runs on ANY machine EJ Map runs on (spec section 6). Fixtures accumulate
from wherever the plugins happen to be. Spec section 2 promises that a product certified
on one Mac is certified for everyone at that version.

**The gap:** the driver cannot tell "this measurement is WRONG" apart from "this
measurement is MACHINE-DEPENDENT". Both score `differs`, and they need different
handling:
- A wrong measurement is a defect to fix.
- A machine-dependent one means two machines produce conflicting fixtures for the same
  product at the same version. Under the distributed model the second overwrites the
  first, and nothing detects the conflict.

For such a vendor, section 2's promise is FALSE, silently.

**It is not Melda-specific.** Any vendor with machine-level display preferences, or other
per-machine plugin state, has it, and there is currently NO way to know which vendors
those are.

**The first known case, Melda (29 Sep), and what is established:**
- MCompressor and MModernCompressor differ from their pushed fixtures (sampled on another
  Mac) at the SAME version, 14.16.0. The differences are in name ORDER ("Preset trigger -
  previous" against "previous (Preset trigger)") and in value text ("63 ms" against
  "62 ms"; "+96.00 dB" against "Off").
- On this Mac, Melda's text is STABLE: no control moved between two fresh instances in
  the readout check.
- JUCE 8.0.12, the version on both Macs by the records, passes an AU parameter's own
  name string through unchanged (`juce_AudioUnitPluginFormatImpl.h:2175-2178`;
  parameter groups become JUCE groups and are never joined into the name). So the name
  order is most likely Melda's own string. That holds only if the studio's JUCE commit
  (29396c22c9) behaves the same; this Mac's `../JUCE` is not a git checkout, so its
  commit is unknown.
- Melda keeps machine-level state that is not readable: a per-plugin encrypted
  `systemconfig.tmf` (~90 bytes), written on instantiation (MModernCompressor's at 12:48
  on 29 Sep), and `LIB/system*.dat`. No plain-text naming or unit preference was found.
  So machine-dependence is plausible, but it is NOT shown.

**THE SETTLING TEST (not yet run):** run the SAME signed probe binary, at the SAME Melda
version, on the studio Mac against the two Melda compressors:
- **It reproduces the pushed fixtures there (and not here):** the binary is ruled out;
  the same binary gives different text on two Macs, so Melda's display text depends on
  the MACHINE. The test alone cannot say whether that is one findable oddity of this Mac
  or an inherent property of the vendor. That needs the setting to be found, or a third
  machine. Until then it is treated as machine-dependent, and section 2 needs a rule for
  it.
- **It gives this Mac's text there too:** the machine is ruled out. The pushed fixtures
  reflect something else: the other Mac's state when they were sampled, or the different
  probe build that sampled them (the installed bundle's probe, from the kathy line).
  That is findable.

**Candidate rules for a machine-dependent vendor (undecided):**
- NORMALISE the text before it enters a fixture.
- RECORD the relevant machine STATE alongside the measurement. This cannot record the
  machine's IDENTITY, because section 6 says nothing in a fixture identifies the machine
  or the person.
- REQUIRE AGREEMENT across machines before a fixture is trusted. A first fixture would
  then be provisional until a second machine reproduces it, and a disagreement is flagged
  rather than overwritten.

Whichever is chosen, the server half must stop letting a second fixture silently
overwrite a first that disagrees with it.

## 7. What the SERVER half must handle (so it is not discovered later)

The fixture schema is the contract between EJ Map and the server. These are the
server-side obligations the Mac-side work has created so far:
- **FIRST: re-sample ONE pushed fixture, Shadow Hills Class A Mastering Comp**
  (`AudioUnit_704f4855_1.4.1.json`), in the item-12 shape. Its control 41 "VU Meter R"
  becomes a `readout` with a null `defaultOnInstantiate`. This is the only thing that
  turns the driver's permanent exit 1 on the dev Mac to 0 honestly (section 0). Touch
  only this one file. The other 73 are corrected per product when a machine that has the
  plugin re-runs them, never in a bulk pass.
- **A null `defaultOnInstantiate.normalised` (and `.display`).** That is how a readout is
  encoded (item 12). A reader that assumes a number must not crash on null, and must not
  substitute a default for it.
- **Never give a role to a control carrying `readout`.** A roled meter would be swept, and
  the sweep would measure the meter wandering.
- **Never apply the threshold working-position rule to a control carrying `readout`.** The
  rule reads `defaultOnInstantiate`, which a readout does not have.
- **Read `readoutCheck`'s ABSENCE as "never checked"**, not "no readouts". All 74 existing
  fixtures lack it and may hold meter samples. That is a known condition, corrected per
  product when a machine that has the plugin re-runs it, not in a bulk pass.
- Earlier items on the same list, for completeness: `defaultOnInstantiate` and `range`
  are OBJECTS (items 8 and 11; the spec is amended, not the fixtures); roles are absent on
  every control (item 9, a prerequisite for the sweep); identity keyed on the XOR uid is its
  own migration task (item 10).
- **Read `thresholdSweep`'s map and display separately** (section 10). `result` says whether
  the dB-equivalent map is good; dial by `thresholdDbEquivalent`. The display is recorded as
  numbers only (`displayEngage.drift_db`, `displayOffsetSpread.iqr_db`, `displayOffsetDb`).
  Whether it is linear in dBFS is NOT decided yet; do not infer it from any one number.
- **Fields beyond spec 4.5**, added 29 Sep and flagged: `level_convention`, `procedure`,
  `bridged`, `positionNorms`, `linearReference` (with `spread_db` or the per-position
  `check_db`), `displayEngage`, `displayOffsetDb`, `displayOffsetSpread`, `defaultGain_db`
  (information only), `positionLandedBy` and `writeLanding`. When they occur:
  `holdDoubled`, `stillMovingAfterDoubling`, `skipped`, `diagnosticArm`, `thresholdPick`,
  and `ratioDuring.raisedFrom` / `readBack`. There is NO `displayLinear` yet: its bound comes
  from the population. A fixture with
  `diagnosticArm` is a measurement with a non-swept control moved on purpose: never fold it
  into a product's certification.
- **Pin the server's matcher to `tools/ejmap/tests/fixtures/name-token-vectors.json`**
  (added 29 Sep, section 9). EJ Map's port of `controlNameTokens` / `controlAnswersTerm`
  is asserted against that file; the file was generated from the server's copy, but
  nothing on the server asserts it yet, so a server-side change to the matcher would
  drift from EJ Map silently. Copy the file into the server tree and test the server's
  functions against every vector. Change the matcher only on the server first, then
  regenerate the file and re-run both ends.

## 8. The fixture unit rule is EJ Map's

The 74 compressor-profile fixtures derived `unit` in an uncommitted sampling
pass. The rule is now written down and pinned:

- the code: `tools/ejmap/Source/EjmapFixtureUnit.h`, which carries the full
  evidence and the four exceptions
- the pins: `testFixtureUnitRule` in `tools/ejmap/tests/RoundTripTest.cpp`, run
  by the pre-commit gate

In short: take the label when present; otherwise the text after a leading number
at the 0.0 point, falling back to the 1.0 point; the 0.5 point is never read. A
bare leading dot is not a number. It was measured against real labels on 13
products (435 controls, all labels empty, 88 units, all reproduced), and it gives
a unit to none of the 1,096 controls whose fixture has none.

When the driver regenerates the 74 fixtures, it applies this rule to the probe's
`--text-at` output. A fixture that disagrees afterwards is a finding to report,
not something to silently re-derive.

## 9. Roles: decided from the name by the server's matcher, stored per control (29 Sep)

Section 4 sweeps the threshold, and until this landed none of the 1,783 fixture controls
carried a role. The rule is in `tools/ejmap/Source/EjmapRoles.h`, the matcher in
`EjmapNameTokens.h`, and the role-to-semantic table in `EjmapRoleSemantics.h`.

**The matcher is the server's, ported, not re-derived.** `name-token-vectors.json` holds
1,006 token vectors and 1,506 answer vectors, generated by running
`lib/controls-note.js` (origin/main a86dba8) unmodified in node. 466 of the answer
vectors are refusals: the term's letters sit inside a token ("hold" in "Threshold"), or a
band abbreviation has no word boundary ("lf" in "LF_Gain", because JavaScript's `\b`
treats `_` and digits as word characters). JavaScript lowercases exactly two non-ASCII
code points to ASCII word characters, U+0130 and the Kelvin sign U+212A (enumerated over
all of Unicode). The port reproduces both, so the Kelvin sign plus "ey" answers "key" at
both ends.

**The rule, per control:**
1. A readout gets no role.
2. A sidechain, meter, filter or preset name is vetoed. Sidechain does not veto on a tuner.
3. A name answering two roles refuses and names both, e.g. `ambiguous:attack|release` for
   the CL 1B's "Select Attack Release". This is the shipped resolver's rule from 6a4550b
   ("a name that means several things is refused, and says which"), but not its code.
   `tiedAtBestRank` and `tieSpansProducts` take a pool of `juce::PluginDescription`, rank
   by channel-variant suffix and compare `ChainHost::stripParenthetical` bases. A control
   name has none of those things, and neither function exists on this branch. Every
   lexicon hit ranks equally, so a role tie is any tie.
4. "DC" is a flag, never a veto.

Tuner roles (key, reference, strength) apply to tuners only.

**Product classes over the 74 fixtures**, pinned in `role-classification-74.json` with
every control's role, flags and reason:

| Class | Products | Next |
|---|---|---|
| single_threshold | 38 | the sweep |
| input_as_threshold | 9 | flagged |
| comp_over_expander | 4 | flagged |
| amount_only | 4 | review after the sweep |
| channels_lr | 3 | review after the sweep |
| bands_or_stages | 14 | review after the sweep |
| surround | 2 | review after the sweep |

Two of the 9 input-as-threshold products give TWO thresholds, not one: Acme Opticom
XLA-3 (Input Gain and Input Pad) and Purple Audio MC 77 (Input L and Input R). The
fallback flags both controls, and a sweep that needs exactly one control cannot proceed on
those two without a pick.

**The role is stored per control, and the semantic table only supplies defaults.** A
1176's Input carries role `threshold` and semantic `input_db`. The table is asymmetric:
- Role to semantic is unit-gated: threshold becomes `threshold_db` only in dB. A 0-10
  threshold has a role and no semantic, which is why the sweep exists.
- dBu is not dB, by the shipped `displayUnitFamily`.
- Semantic to role is unconditional for the eight semantics, and gives nothing for knee,
  range, hold, tone and slope.

**Pins and mutations:**
- RoundTripTest checks T0-T7, R1-R10, S1-S4 and C0-C6.
- Ten mutations were run, and each reddens at least one pin.
- An eleventh ("a split consumes one character, not two") is an EQUIVALENT mutant. In
  these three split patterns the second character of a match can never start another, so
  it cannot be observed. It was dropped rather than pinned.

## 10. The threshold sweep (spec section 4), as ruled 29 Sep after the first real subject

The probe measures (`--sweep`, `tools/au_instantiate_probe/probe_sweep.h`); EJ Map plans,
derives and writes the fixture (`--cert-sweep`, `tools/ejmap/Source/EjmapSweep.h`).
`--cert-sweep-census` lists what can run here, read-only.

**The first subject was bx_townhouse** (native, 2:1, threshold −20 to +10 dB). Three arms:
- A: positions walked ascending in one process. It read "certified" on bad data.
- B: the same, walked descending. It read nonmonotonic.
- C: one fresh process per position and level, 6 s hold. Clean, and the ground truth.

Any loud-to-quiet transition left townhouse's auto release running for more than 3 s. That
covers the next position's quiet level, and the default reference's −6 hold before the first
position. `AudioUnitReset` is a no-op on townhouse (outputs bit-identical), so it is no cure.

**The ruled procedure:**
- **One process per position**, levels quiet to loud inside it (−24, −12, −6).
- Plus one reference-only process at the default threshold, used ONLY for spec 4.7's
  unlicensed test.
- Otherwise 16 positions × 3 levels, 997 Hz, the peak convention, and 1.5 s holds with 0.75 s
  discarded.
- The still-moving detector doubles a hold once. It fires when the last 0.25 s window moved
  more than 0.1 dB.
- The three-step write verify, where `write_unlanded` skips the position.
- The retry rule per process. A window aborts the product as a licence event.
- For an unseen version, defaults are sampled first (a new identity), then the sweep, in one
  pass.
- Measured cost: 17 processes, 78 s of audio, 11 s wall for native townhouse.

**SPEC 4.3 AMENDED: the reference is the soft end's LINEAR gain.** Reduction is measured
against the soft end's out-minus-in, not against the output at the default threshold. The
soft end's gains must agree across the three levels within 0.5 dB, or the sweep refuses.
Why: the default-threshold reference is wrong on any plugin whose default compresses.
Townhouse's default (0.0 dB) takes 3.07 dB at −6, while its soft end is linear
(+2.43 / +2.42 / +2.35 dB).

**Guards: a map certifies only if neither fires.**
- More than two positions still moving after the doubling.
- Any reduction more than 0.5 dB below the linear reference.

The arm A trace is committed, and it must not certify (RoundTripTest D9).

**The display is recorded as NUMBERS; `displayLinear` is left unset** (ruled 29 Sep, a
second time). These are never merged with the map's `result`, the same rule as reproduction
score vs schema emission.
1. `result`: the dB-equivalent map (certified, flat, nonmonotonic or unreadable).
2. `displayEngage` (**primary**): at each level, the display where reduction crosses 0.5 dB,
   less the level for lower_is_harder or plus it for higher_is_harder. Its `drift_db` is the
   spread across the three levels. It never touches the ratio.
   - The crossing is extrapolated along the line through the first TWO ENGAGED positions.
   - Interpolating across the knee (last unengaged position → first engaged) treats the zero
     region as linear and invents drift: 1.33 dB on a perfectly dB-linear input at 4 dB
     spacing (pin Q6).
   - With the knee artefact removed: townhouse 4.46, MCompressor Peak 0.24 (it was 2.02 under
     bracket interpolation), MCompressor 100 ms RMS 0.28.
3. `displayOffsetSpread.iqr_db` (secondary): the IQR of (derived T − display). It inherits
   R's error at depth. Townhouse 4.50, MCompressor 2.53 / 2.75.
4. `displayOffsetDb`: the median of (derived T − display), recorded as a number whatever its
   value. Spec 7's 2 dB bar is a test on it.

No bound is set: n = 2 is no population. The population decides at about 20 fixtures, the way
the acceptance test settled peak vs RMS. The first two subjects' premise ("MCompressor's cells
are consistent") was wrong, and measuring it is what moved the engage check to primary.

**Input-as-threshold: each position carries its own quiet reference** (ruled 29 Sep). An
input control changes gain as well as compression, so no end of its travel is a linear
reference (MC 77's soft end is −inf). Each position's process renders −54 and −48 first,
quiet to loud.
- The two quiet readings must differ by 6 dB within 0.1 dB. If they don't, the quiet tone is
  compressed or on a noise floor, and that position is refused rather than calibrated off it.
- Reduction is the position's own −48 gain minus its gain at the level.
- The per-position check is recorded (`linearReference.check_db`).
- The first batch's traces carry no quiet levels, so their input-as-threshold products
  re-derive as unreadable, as they should.

**Not licensed is not inferable from audio** (ruled 29 Sep, the second misfiring proxy after
the PACE bundle markers). At the default settings a product is flagged only for:
- silence at every level
- non-finite output
- output that is not the input's tone (`tone_frac` < 0.5, the Goertzel share of output power
  at 997 Hz)

The window watch is the other signal. Default gain is recorded (`defaultGain_db`) as
information and never judged. Spec 4.7's "more than 3 dB from the input" withheld six working
Waves plugins on 29 Sep; CLA-2A's default gain is +8.5 dB.

**Pass-through has its own reason:** `flat` with reason "passthrough: output equals input
within 0.01 dB at every reading". C1 comp and RCompressor at 12.0.0 read that way. Their
defaults hold no bypass or on/off control at an off default, only modes (Low/Peak Ref,
ARC/Manual, Warm/Smooth, Electro/Opto), so they go to review with the cause unmeasured.

**The soft end's disagreement is a number on every fixture** (`linearReference.spread_db`).
Only beyond 2 dB is the data unusable and the sweep refused. Useful reductions are 3 to 20 dB,
so 0.63 is noise and 4.91 is the size of the signal. The first ruling's 0.5 dB had nothing
behind it.

**Writes escalate, and each records its mechanism** (ruled 29 Sep). The probe pumps first for
500 ms; on timeout it confirms while rendering silence for up to 500 ms. Silence builds no
compression history, and the post-render re-read still guards. Each write prints `landed_by`
(instack, pump, render or unlanded). Fixtures carry `positionLandedBy` and `writeLanding`
counts, so the 2 Aug / 10 Aug disagreement is settled write by write. Traces from before the
field existed are read by the same rule: `instack_match` means instack, otherwise pump, since
pumping was then the only mechanism. Solid Bus Comp is the case in point: its writes land only
while rendering.

**UAD-2 is conditional hardware** (ruled 29 Sep), in the same category as the McDSP APBs:
"needs UAD hardware present", one re-run once attached.
- On this Mac, which has no UAD hardware, all 19 opened their own dialog at the first render.
  Defaults sampling, which never renders, was clean.
- That is consistent with needing the hardware, not proven: the watch cannot read titles.
- All 215 installed UAD components are `com.uaudio.effects` with manufacturer `!UAD`. None is
  a UADx (native) build, so attaching the hardware should recover all 19.
- The census names the hardware, and the batch skips them without launching anything.

**The first real fixture, townhouse:** map certified, `displayLinear` false,
`displayOffsetDb` −15.00. The file is
`tools/ejmap/cert-fixtures/profiles/AudioUnit_417f6e76_1.8.1.json`. Its 17 raw
traces are committed and re-derive it exactly (RoundTripTest V5).
`--cert-sweep-rederive <fixture> <processes.json> <rawDir> <out>` applies a changed rule to
any past sweep without measuring.

**The convention: KEEP PEAK** (ruled 29 Sep). These were diagnostic arms, with MCompressor's
RMS length moved on purpose; their traces are committed (RoundTripTest C1/C2).
- With RMS length at Peak, `displayOffsetDb` is −0.15 against a prediction of 0. This is the
  positive control, and it passes.
- With RMS length at 100 ms, it is +0.80, not +3.01. That is a fact about Melda's detector,
  not about the convention. Noted, not chased.

**The ratio raise (spec 4.2), built:**
1. A ratio instantiating at 1:1 is read on a grid by the probe's `--text-at-norms`: every step
   if the ratio is stepped, otherwise 65 points.
2. The position with the **smallest READ value at or above 4:1** is chosen. This is not the
   first norm: C1's ratio runs 0.5:1 → ∞ → −5:1, and RCompressor's is inverted.
3. That position is written in every process.
4. **R is derived from the text read back after the write**, never from what was asked for;
   a stepped ratio asked for 4:1 may land on 3.5:1 or 5:1.
5. Processes that read it back differently refuse the sweep. The fixture records
   `ratioDuring.raisedFrom`.

**The threshold picks, built (never by name):**
- **Several input-as-threshold candidates:** the only continuous one wins over stepped ones.
  XLA-3 sweeps Input Gain, not its two-state Input Pad.
- **An input-as-threshold L/R pair:** channel A (spec 4.7). MC 77 sweeps Input L. Link
  controls are recorded as they instantiated and never written; MC 77's Link is "Std".
- **The channels_lr class** (Fairchild) stays deferred to review.

**The defaults refactor, proved 29 Sep.** Readout detection is now one function, shared with
the unseen-version path. `--cert-defaults` was re-run on Shadow Hills Class A (the known
[41] VU Meter R readout) and on townhouse (none). Against the pre-refactor run, every emitted
field is identical except the meter's own two samples, and the readout sidecars match.

**Measured bias, recorded, not corrected.** Against arm C's 6 s truth, the 1.5 s holds
under-read townhouse's reduction by at most 0.35 dB, with a median of 0.14 dB over 30 engaged
cells. It never over-reads. The cause is a slow detector creep of about 0.02 to 0.09 dB per
0.25 s, which stays under the 0.1 dB still-moving threshold.

**Sleep never becomes a hang** (ruled 29 Sep; section 6 runs on laptops).
- **Timeouts count awake time only.** `runChild` takes an explicit `Clock`, and every timeout is
  awake elapsed (`timeoutPassed`). This was already true by accident: JUCE's clock is
  `mach_absolute_time`, which on this Mac read 518,794 s against 1,031,532 s of wall time since
  boot. It is now pinned (S1–S3, on a real child process with a simulated 15-minute sleep).
- **Every process records `slept_ms`** (wall minus awake), and the report counts processes that
  slept mid-run.
- **Every run holds `PreventSystemSleep` and `PreventUserIdleSystemSleep`.** Neither stops a
  lid-close on battery. Batch 2's sleep (29 Sep, 18:36) was exactly that: "Clamshell Sleep" on
  9% battery, with idle sleep already prevented system-wide by other processes.
- **The hazard sleep actually produced was not a timeout.** It was a bridged plugin across a
  wake: RCompressor (s) read its ratio as 0, and 2.1 M blocks rendered as no-ops within 500 ms.
  The write verify caught it. Whether a process that slept should be re-run, not just recorded,
  is open.

**A process that slept is re-run once, and refused if it sleeps again** (ruled 29 Sep).
- A measurement across a wake can be corrupt and still plausible. RCompressor (s) read its
  ratio as 0 through a bridge that died across a dark wake.
- So a clean run during which the Mac slept more than 1 s is re-run once. If it sleeps again
  it is refused (`ChildResult::sleptTwice`), never looped. This is the same shape as the
  SIGTERM rule, and one helper (`runWithRetry`) serves `--cert-defaults` and `--cert-sweep`.
- Sleep is measured on `mach_continuous_time` minus awake time, which wall-clock changes
  cannot move. A slept attempt is recorded as not clean, so a re-derivation distrusts it too.
- Nobody on someone else's Mac reads `slept_ms`. The rule acts on it instead.

**Engage drift has (at least) two mechanisms, and both show as drift** (logged 29 Sep; still
no bound):
1. **A display nonlinear in dBFS.** The crossing moves differently from the level across the
   travel: townhouse's display models a console.
2. **Positions bunched in display terms.** Tube-Tech CL 1B: every level engages at one knob
   position near Off, where the display reads 1.4, 0.8, 0.2, then −4.2. The crossing is pinned
   to that knob step whatever the level, so its display value barely moves while the level
   moves 18 dB. Engage drift is 15.70 against an offset IQR of 0.45, and deep in its travel
   the offsets hold at −14.3. This is a property of where the positions fall, not of whether
   the display is linear.

There are three drift/IQR disagreements so far: C1 comp-sc/comp-gate (4.17 vs 0.70), CL 1B,
and townhouse, where the two numbers agree at 4.46 / 4.50. The population decides the bound.

**Categorisation is a PREREQUISITE, not a gap** (ruled 29 Sep). A product's category comes
only from the server's categorisation (`/api/params/categories`, cached as the ledger's
`categories.json`). The param map has never worked offline, and EJ Map already fetches map
state from the same server. A fresh Mac needs one connected EJ Map run, and it brings both the
categories and the map state. Local categorisation is not to be built: reimplementing the
server's model locally is the two-vocabularies hazard behind the Waves untick bug.

**Discovery: the worklist is keyed on MAPS** (ruled 29 Sep). The driver used to read its
worklist from its own output: every mode took its subjects from a fixtures directory, so a
product nobody had certified could never enter it. A hand-written identity stub unblocked
everything in the 29 Sep fresh-system test, which proved the point. Fixtures are the RECORD of
what is certified, not the list of what to certify. Now a candidate is:
- installed, and
- MAPPED at its installed build, meaning a local map, or the server's map state 1–3 for that
  identity. On a fresh Mac most products are mapped by someone else and have no local map.
- in the compressor category (the local map's own, else `categories.json` by `format|uid`),
  and
- with no fixture at its installed identity and version.

The worklist is the store's unswept fixtures plus the discovered candidates, in coverage
order: a record whose sweep alone is missing, then those needing defaults first. A fixture
that already records a sweep leaves the list. `--ejmap-ledger` names the ledger (default
`~/Library/ejmap`); discovery only reads it. "pitch" products are listed, not swept, because
tuner certification is not built. Pinned: a ledger with maps and no fixtures produces a
non-empty worklist (F1), plus F2–F5.

**Mapping and defaults each make their own probe pass, and that stays.** They measure
different things (the control surface by set-then-read, and the instantiate state twice), and
merging them would couple two stages that fail differently.

**Level dependence: the axis that DEFINES a threshold** (ruled 30 Sep). Every other guard tests
the curve across positions. On 29 Sep API-2500 and H-Comp certified with reduction identical at
−24, −12 and −6 dB: a make-up gain law, not compression. Three levels were collected as inputs
to a derivation and their disagreement was never used as evidence.
- Textbook: dg/dL = 1 − 1/R above threshold, so 18 dB of level moves reduction by
  18 (1 − 1/R) dB. That's 0.86 dB even at 1.05:1. API-2500 moved 0.00 with a ratio of 4.0 read
  back (13.5 dB predicted), so the guard cross-checks the ratio too.
- **Hard refuse:** any position inside the readable band at every level whose reduction spans
  less than 0.25 dB across the levels. The two false cases span 0.00–0.02 dB; the nearest honest
  product spans 2.52. Saturated positions (above the band at every level: C1 at −100 dB,
  MCompressor at −80 dB) are flat for an honest reason and are excluded by the band condition.
  The fixture then carries `roleFlag: "not_a_threshold"`.
- **Record:** `levelDependence` on every fixture: dg/dL per position, the median over adjacent
  in-band level pairs, the implied ratio 1/(1 − median), and the textbook prediction from the
  ratio read back. No tight bound: real compressors depart the textbook at depth (MCompressor's
  implied ratio is 2.04 against 1.8 read back; townhouse's 2.22 against 2).
- Pinned on the committed API-2500 and H-Comp traces as negative cases (G1, G2), with
  MCompressor and a saturated synthetic as positive controls (G3, G5).
- **The grid does not decide the conclusion** (ruled 30 Sep; the 0.25 dB bound is approved because
  it rests on a derivation, not on a gap between clusters). A gain law is identical at every
  level, so a position is in band at all three levels or at none, and a grid placed entirely
  above the band would have escaped form A. Two fallback forms use data already computed:
  B, when no position is in band at all three levels, refuses if every adjacent in-band level
  pair is flat (at least two); C refuses if every engaged position, whatever the ceiling, is
  flat (at least two). A real compressor at its ceiling is flat there and has slope at its
  in-band positions; a gain law has slope nowhere. `levelDependence.refused_by` names the form.
  Pinned: G6 (a gain law entirely above the band), G7 (pairs straddling the band edge), and G8,
  a real compressor on a grid too coarse for form A, which must certify.
- **Multiband candidates:** 997 Hz excites one band, so a candidate whose band does not cover
  it shows nothing. A flat candidate is labelled as evidence of that ("uncertified for want of
  an in-band tone"), not as pass-through or a defect, and `thresholdReview` says the map comes
  after the human's pick, when that band's own ratio can be read.

**`displayLinear` is not a field** (ruled 30 Sep, the fourth application of the split-verdict
rule). Ten independent products carry both display numbers, and they disagree in both
directions on 6 of 10: engage drift catches what the offset IQR misses, and the reverse. A single
boolean was the wrong shape. Both numbers are recorded, and the consumer combines them: the
server half knows whether it needs a trustworthy offset or only a usable map. An unexplained
third cause of drift (C1's display steps are evenly spaced, yet it drifts 4.17 dB) means any bound
set now would encode ignorance.

**Several thresholds: sweep every candidate, labelled** (ruled 30 Sep, correcting the earlier
plan). "Review with curves in hand" was incoherent for a product refused at the roles stage: it
was never swept, so there were no curves. Now a product with two or more threshold candidates
and no pick (bands or stages, an L/R pair outside the input-as-threshold rule, several input
controls) is not refused. Each candidate is swept in turn with every other control at its
instantiate default, and the fixture carries `thresholdCandidates` (one labelled sweep each,
with its own three results and level dependence) and `thresholdReview` (class, count), and NO
`thresholdSweep` until a human picks from curves. Traces are tagged `c<index>.ref` /
`c<index>.posNN`, and `--cert-sweep-rederive` re-derives every candidate. Validated on
MDrumLeveler (2 candidates, 37 processes, re-derivation exact). Products with no candidate at all
(amount-style, no threshold) are still refused at the roles stage.

Cost, measured 30 Sep from batch 4's own position processes (a position process runs at about
1.05× the product's instantiation): 42 products, 202 candidate sweeps, about 1.9 h. The largest
is DynOne3 (15 candidates, about 11 min).

**Noted for later, NOT built:** a silence-settle optimisation (render silence between
positions instead of starting a fresh process). It is untested, and its failure mode is a
quiet bias rather than an error.


## 11. Where the work lands, what counts as a record, and the jam (ruled 30 Sep)

**The store is the output directory.** `--cert-sweep` wrote everything under a mandatory
`--out` with no default, and pruned its worklist against a separate `--fixtures`. Nothing was
ever written to `~/Library/ejmap`, so on another Mac the handover (`zip -rq ~/Library/ejmap`,
runbook §4) carried none of it; and "a second run skips what the first certified" was only
true when the operator pointed `--fixtures` at `<out>/fixtures` by hand. Now `--out` defaults
to `~/Library/ejmap/cert/`, `--fixtures` to `<out>/fixtures/`, and `--probe` to `EchoJayProbe`
beside the running `ejmap` executable (`resolveCertPaths`). Explicit flags keep working for
repo-store runs. Pins R4–R6.

**A record is a sweep, a candidates fixture, or a refusal** (`sweepRecorded`). Only a
`thresholdSweep` counted before, so a candidates fixture (which has none by design) went
straight back on the worklist, the defaults sidecar (`<stem>.defaults.json`) was loaded as a
second subject with no sweep, and a product stopped before any fixture existed was rediscovered
on every batch. Pins R1–R3, R8–R9.

**A refusal is written, at the stage it happened.** Every early exit — defaults would not
sample, plan found no threshold, ratio search failed or found nothing at 4:1, a window
appeared, the reference was not the input's tone — writes the fixture it has (the store's, or
the discovered identity with no controls yet) with `thresholdRefusal {stage, reason,
recordedAt, host, probe, processes, uncleanProcesses}` and no `thresholdSweep`. The worklist
skips it; a new installed version re-runs on its own, deleting the file works too, and
`--retry-refused` puts back the TRANSIENT ones — stages `defaults`, `ratio_search`, `window`,
`budget`, `reference`, facts about the run (iLok back in, hardware attached). Stages `plan`
and `ratio_none` are PERMANENT, facts about the product, and `--retry-refused` skips them
(`refusalIsPermanent`; pins R3b–R3d): retrying them indiscriminately would re-run the
uncertifiable on every batch, the jam the record exists to escape. `--retry-refused-all`
overrides, for the operator after a rule change. The record carries `retry` saying which. The defaults-phase case is
the one that mattered: a plugin that hangs on load has a map (it mapped, on some machine) and no
fixture, so it was offered again on every batch — four minutes of timeouts each time, for ever.
Pin R7 (a discovered identity, stage `defaults`), and a live run with `--timeout-s 1` on
Millennia TCL-2: refused at `defaults`, absent from the next run's worklist, back with
`--retry-refused`.

**The per-product budget** (`kUncleanBudget` = 2). The mapper's sweep bounds a plugin at one
90 s process; a certification is ~17 processes per candidate, each with its once-retry, so a
plugin that hung on every load cost about 68 minutes before its fixture existed. After two
processes end unclean (after their retry) the product stops and the refusal is written at stage
`budget`, naming both. This also changes one tolerated case: a product with two flaky positions
out of sixteen used to derive from the fourteen; it is now refused and says so. The defaults
phase still stops on its first unclean process, which is within the budget.

**The mapper's escape hatch reaches cert.** `categories.json` `disposition` other than `sweep`
(`operator_excluded`, runbook §3 and §B) keeps a product out of cert discovery, with its `why`
in the worklist report. Pin R10. Before this, a plugin excluded from mapping for hanging was
still offered to the certification batch.

What it does NOT do: there is no cross-run try counter (the mapper's 3-try quarantine). One
refusal is permanent until acted on. A transient failure — iLok out for one run — therefore
needs `--retry-refused`, which is why the runbook says to confirm the iLok before starting.

**`thresholdReview` names the band** (ruled 30 Sep, after batch 5b). "A consumer can derive it from
the candidates array" is two places deriving the same thing, the shape of the Waves untick bug.
`thresholdReview.responding` is now `[{index, name}]` for every candidate that certified;
`verdicts` is `[{index, name, result}]` for all of them (`pass_through_at_defaults` and
`licence_suspect` named as results there); `passThroughAtDefaults` is `{count, candidates}`.
`flats` now counts band-coverage evidence only — pass-through flats sit in their own list, because
a product that does nothing as instantiated says nothing about its bands. `thresholdSweep`
gained `passThroughAtDefaults` (boolean, always present) for the same reason on single-threshold
fixtures. Pins V1–V5, D13; all 41 candidate fixtures re-derived from their traces, candidate
sweeps identical. Census: 34 of 199 candidates on 5 products (DynOne3, MaxxVolume ×2, EMO-D5
×2) are pass-through, every one a whole-product case; C1 comp and RCompressor are the
single-threshold members. Category named; what enables each one is NOT being hunted.

Recorded, not built: **dedupe by identical read-back.** Ten products carry one control under two
names with point-identical curves (six L/R or A/B channel pairs; four aliases inside one
parameter list, e.g. Melda's "Band 2 -> Threshold" and "Band 2 - Processor 1 - Threshold").
Sweeping both cost ~14 of 199 sweeps, concentrated on the heaviest products.

Recorded, not diagnosed: **MDynamicsMB and MDynamicsMBLarge respond at 997 Hz on bands 2 AND 3**
with near-identical curves (7.50 vs 7.42 dB at -6, position for position) — each band alone
gives the full reduction, so the tone is not being split across a crossover. 
**997 Hz sits ON MDynamicsMB's default crossover.** The defaults sidecar reads `Crossover ->
Cross 2 = 1000 Hz` (Cross 1 200 Hz, Cross 3 4634 Hz, analog 24 dB/oct) on both MDynamicsMB and
MDynamicsMBLarge. Bands 2 and 3 both responding with near-identical curves is therefore benign
and expected — and the finding is about OUR TONE CHOICE for multibands, not about Melda: a
fixed 997 Hz will land on any product whose crossover defaults to 1 kHz. The per-band tone
(already logged as a later feature) is the fix; until then a multiband whose `responding` names
two adjacent bands should be read against its crossover defaults before anything else.

**A licence is a property of the PRODUCT, not of a candidate** (ruled 30 Sep; the third misfire
of a licence inference, after the PACE bundle markers and the 3 dB reference test). The
silent-at-default rule had flagged 9 Melda candidates as licence-suspect — every one a gate or a
Processor-2 stage, silent because the stage closes on the tone — while other candidates of the
same fixture produced it. `resolveLicenceAtProductLevel`: if ANY candidate produced the input's
tone the plugin is licensed, and a silent candidate keeps the result its data gave it, with
`silentOrOffToneAtDefault` recorded beside it; only a product silent on every candidate stays
licence-suspect. Pins V6–V8, two mutants red. Re-derived: the 9 became 7 `flat` and 2
`nonmonotonic` (MSpectralDynamics Processor 1, MDynamicsMBLarge Band 3), none pass-through; no
`licence_suspect` remains in the 41.

**A retried refusal samples defaults first** (found 30 Sep on SSL G3's first `--retry-refused`). A
record written in the defaults phase is the discovered identity and nothing else; planned from as
a fixture it read "0 controls hold the threshold role" and wrote a PERMANENT refusal without
running a process — a permanent verdict manufactured from an empty record. `needsDefaultsFirst`:
defaults are sampled for an unseen version, a discovered product, and any subject whose record
carries no `controls`. Pin R9b. The 1-second live check earlier in the day showed the same "0
threshold roles" on Millennia TCL-2 for the same reason.

## 12. Reading before acceptance (ruled 30 Sep, after the zero-curve classification)

Three of seven "responded but unreadable" verdicts were lost by 0.10, 0.36 and 0.67 dB against
two absolute bars applied to a relative quantity. Order: the reading rules first, then the
acceptance rules, each re-derived across the whole store from its committed traces.

**Reading.** (1) A hold whose output is not the tone (`tone_frac` under 0.5) is not a reading:
refused like a non-finite one and listed on the fixture as `notToneReadings`. It produced every
`nonmonotonic` verdict in the candidate pile and hid inside "flat" ones. (2) The flat test reads
every position, not the two ends: `flatSpan_db` is its number; ends that agree at every level
while the middle differs is `unreadable: the response is not across the sweep`; the sense is
read at whichever level the ends differ most (AMEK: 7 dB at -24, 0 at -6). (3) Pass-through is
output = input plus a CONSTANT (`passThroughOffset_db`): dbx-160 at +0.31 dB, SSLComp at +3.00,
Maag's K/limiter stages at +0.24, Shadow Hills' second stages at +1.13/+0.33. Pins T1-T3, F1-F3,
P1-P2. Whole-store re-derivation (98 fixtures, 258 verdicts) changed 38: 14 flats became
pass-through by a constant (9 more only gained the field), 8 nonmonotonic became certified, 4
flats became unreadable (a 1.78 dB mid-sweep difference on four Melda candidates, identical to
the hundredth - recorded, not chased), 2 unreadable became flat, 1 nonmonotonic became
pass-through (MDrumLeveler: the spikes were its only readings). Auto-Tune's "three-way identity"
in the earlier report was an artefact of the silent holds dominating the statistics: with them
refused, Mod Comp 1 certifies and Mod Comp 2 / Opt B Comp 1 are pass-through.

**Acceptance.** (4) Both reference guards are RELATIVE: the reference error as a fraction of the
largest measured reduction, bar `kRefErrorFrac` = kSenseDb/kSaturateDb = 1/12 - the proportion
the sense test already accepts, not a constant fitted to this sample. Recorded as
`linearReference.{response_db, error_db, error_fraction, bar_fraction}`. Pins D4, D4b, D4c.
Second re-derivation: 19 verdicts changed, all certified -> unreadable, and NONE of the three
recovered (Lindell 354E Mid 17.0%, MTurboCompMB Band 2 16.6%, Solid Dynamics 9.3%). The
distribution across all 83 soft-end references is the finding: 48 at <= 2% (single-band
products) and a band at 8-19% that is EVERY MULTIBAND (C4, C6, C6-SC, LinMB, MDynamicsMB/Large,
MTurboComp) plus SPL IRON, SSL Native Bus Compressor 2 and the Lindells - soft ends contaminated
by the other stages compressing at their defaults. That is item 5's case, not a bar to loosen.
The 1/12 bar also flips test D4's 1.2 dB-against-10 dB from accepted to refused (12%); D4 now
pins both sides of the bar.

**Why the 19 refusals were correct, recorded (ruled 30 Sep).** The distribution is the result,
not the three non-recoveries. Forty-eight references at or under 2% against a band at 8-19% that
is every multiband, with a MECHANISM - the other stages compressing at their defaults - is the
first bimodal split with a mechanism this work has found. Lindell 354E Mid at 17% was a
contaminated reference, not a tight bar; loosening the bar to recover it would have shipped
confident wrong maps for the whole 8-19% band. A bar is loosened when the population under it is
continuous and the mechanism is noise; it is kept when the population is bimodal and the
mechanism is systematic. This one is kept.

**The discriminating prediction, stated before the fallback batch ran.** The multibands should
recover: their contamination is other bands compressing, and a -48 dBFS tone is below all of
them. SPL IRON and SSL Native Bus Compressor 2 are NOT multiband, so their contamination has a
different cause and they may not. If they recover too, the mechanism is broader than "other
stages"; if they do not, the diagnosis is confirmed specifically. Outcome below, once measured.

**Fallback.** (5) `needsQuietFallback`: a sweep refused because the soft end is not linear, or
because a reading sits above it, is re-swept at once with the per-position quiet reference
(-54/-48 added), tagged `q.` + prefix; `linearReference.fallback` says why; re-derivation
prefers the `q.` traces. Pins Q1-Q3. Live: AMEK certifies through it (both thresholds, every
position linear at -48). OTT's quiet check fails (-48 minus -54 = -4.5 dB: it compresses upward
at the quiet levels too) - no level is linear at any setting; unreadable and recorded so.

**LEAD, not a curiosity (30 Sep):** 1.78 dB appears IDENTICALLY, to the hundredth, as the
mid-sweep difference on four Melda candidates across three products (MDynamicsMB Band 3
Processor 2, MDynamicsMBLarge Band 1 and Band 6 Processor 1, MDrumLeveler Threshold Max). Four
independent measurements do not agree to 0.01 dB; this points at a shared code path or a fixed
internal step in the plugin family, or in our hold window against it. Logged for a look when a
Melda question is next open; not chased now.

**The tone_frac guard retroactively invalidated two "findings" (30 Sep).** MDrumLeveler was
classed "our tone does not suit the architecture" and Auto-Tune Vocal Compressor carried a
"three-way identity and an 18 dB clamp"; both were built on non-tone readings. With those
readings refused, MDrumLeveler is pass-through at defaults and Auto-Tune's Mod Comp 1 certifies
while the other two are pass-through - no identity, no clamp. Any conclusion drawn from these
sweeps before the guard existed is suspect for the same reason, and should be re-read from the
re-derived store rather than from the earlier report.

**The fallback batch, measured (22 fixtures, 16 product names; 30 Sep evening + 1 Oct 13:44-14:31):
24 candidates on 18 products RECOVERED through the quiet-level reference, 0 failed the quiet check
like OTT, 3 still refused (LinMB (m/s) Band 3 and MDynamicsMBLarge Band 3 Processor 1: nonmonotonic
on the positions the quiet check allows), 2 not run (SSLGChannel (m/s): no map, not discoverable
once their fixture is lifted - queued separately). THE PREDICTION: the multibands recovered as
predicted - AND SO DID SPL IRON (41 of 41 quiet checks) AND SSL NATIVE BUS COMPRESSOR 2 (8 of 16).
The mechanism is therefore broader than "other stages": a single-band compressor whose soft end
still compresses at -24..-6 dBFS contaminates its own reference, and the same quiet reference
repairs it. The 8-19% band was two causes with one cure; the diagnosis "other stages" was right
for the multibands and incomplete as a mechanism. Full table in the batch-7 README.

## 13. Engage detection (spec section 3 `engage`, built 1 Oct)

A candidate whose first sweep reads `pass_through_at_defaults` is tried with each ENGAGE CANDIDATE
in turn: a quick probe of three positions (soft, middle, hard) at the three levels with that one
write as a precondition. The first candidate whose probe shows gain reduction (`showsResponse`:
not flat, a span above the sense resolution) is VERIFIED - gain reduction with the write,
pass-through without it (the sweep just taken) - and the full 16-position sweep is re-run with
the write, tagged `e<idx>.` + prefix (quick probes `eq<idx>.`). The fixture records
`thresholdSweep.engageWrites {writes[{index, control, norm, from, verified, verifiedBy}], tried[],
found}`. A product that shows nothing with every candidate stays pass-through and records
`found: false` with the tried list: that is an answer, not a failure.

Candidates (`engageCandidates`, pure): from the NAME (on / enable / engage / active / in) and from
the SHAPE (two steps, instantiated at one extreme); never bypass, power, standby, monitor, listen,
solo, mute (`neverTouchName`), never the threshold itself. Written to the OTHER extreme from the
instantiate value. Ordered name-and-shape, then shape, then name; within a rank the control
sharing the threshold's first word first ("Comp On" for "Comp Thresh"). Pins E1-E10, six
mutants red. Re-derivation picks the engaged run and its quiet fallback by tag (`resolveTraceRun`)
and restores the writes from the fixture (`restoreEngage`).

Not built: combinations (two switches needed at once). If no single candidate shows GR the
product stays pass-through; the tried list says so. One defect found by the pin: `JSON::parse(...).
getArray()` on a temporary dangles - the set read freed memory and E9 caught it.

**Live, 1 Oct 14:3x-15:39:** 4 of the 6 products became measurable - MaxxVolume (m/s) Low/High Level
Thresh with their "...Thresh On" switches, EMO-D5 (m/s) Gate/Comp/Limiter Thresh with their "...On"
switches (Leveller: switch verified, sweep unreadable; DeEsser: not found after 14 - a de-esser
band does not cover 997 Hz). dbx-160 (s) (2 tried) and DynOne3 (1 tried per threshold) stay
pass-through with the tried list recorded: nothing switch-shaped engages them. Batch-8 README.

## 14. The ratio-free amount curve, beside ours (1 Oct)

Sean's `eff_threshold_dbfs` - the input level at which gain reduction reaches 1.0 dB - is now
derived at every position as `thresholdSweep.thresholdEffective1dB[]`, ALONGSIDE
`thresholdDbEquivalent[]` (T = L − g·R/(R−1)), not replacing it. It is interpolated between the
two test levels that bracket the crossing; `{below: -24}` when GR is already past 1 dB at the
quietest level, `{above: -6}` when it never reaches it, `null` when a reading around the crossing
is missing (a guard: no bracket, no number). Needs no ratio. Pins R0–R5, three mutants red. Level
convention is the fixture's (peak dBFS); his is sine RMS, 3.01 dB apart for a sine.

**Where the two disagree, from the re-derived store (1 Oct, 98 fixtures, 1,114 certified
positions):**

- Both numeric on only **117** positions. The ratio-free curve gives a number on **245** positions
  where the R-based map has none (no ratio control, or the ratio did not parse); the R-based map
  gives one on 107 where the crossing is not bracketed; 645 are outside the measurable band for
  both.
- On the 117: eff − T has **median −0.40 dB, IQR −2.85…+0.75, range −6.4…+6.5**. Textbook says
  the crossing sits **+R/(R−1)** above T (+1.2…+3.0 at the ratios in use). Measured against that
  offset, **88 of 117 positions disagree by more than 1 dB**, and the disagreement is almost all
  the SAME SIGN: the 1 dB crossing sits BELOW the R-based threshold by 2–6 dB (Lindell SBC −5.8,
  Solid Bus Comp −4.5, DPR-402 −4.5, CLA-76 −3.5, SSL X-ValveComp −4.0, Tube-Tech CL 1B −5.2),
  while the C1 family lands within 0.1 dB of T. Consistent with a soft knee (GR starts well below
  the hard-knee-equivalent T the R-based extrapolation assumes) — recorded as the reading, NOT
  measured as a knee: knee width is not derived anywhere. The disagreement list is the knee
  detector Sean's `knee_db` would need; it is a lead, not a field.
- The store was re-derived with the field; no verdict changed (258/258).

## 15. Tuners (spec section 5), built 1 Oct - the harness, not yet a measurement

**Installed and reachable on this Mac (read-only census):** real-time, native: Auto-Tune Access
10.5.0, Auto-Tune EFX 9.0.1, Auto-Tune EFX+ 10.5.0, Auto-Tune Pro, Auto-Tune Artist (Antares, the
five `pitch` products in categories.json), plus MetaTune 1.1.8 (Slate, categorised `Fx|Pitch
Shift`) and bx_crispytuner 1.1.0 (categorised elsewhere). Hardware-conditional: four UAD Auto-Tune
Realtime variants (`!UAD`). **Waves Tune Real-Time is NOT installed** - only "Waves Tune LT", the
offline editor. **Melodyne** is ARA/offline-only: no real-time pitch path, uncertifiable by any
harness, refused by name before any process (`araOnlyByName`, pin A1). First subject when mains
returns: Auto-Tune Access.

**Probe (`probe_pitch.h`, `--sweep-pitch`):** autocorrelation detector (window 2048, hop 512 at
48 kHz; lag searched half an octave either side of the note, smallest near-best peak preferred -
the first self-test read +30 cents as -1170, an octave error, and the range IS the guard), two
generators (static detuned note; square vibrato alternating ±cents at `rate`, sine optional),
cents per window for input and output, confidence beside each. `--pitch-selftest` runs without a
plugin: six detunes within 0.5 cents, silence refused, the generator's flips in place.

**EJ Map (`EjmapPitch.h`):** STRENGTH = 1 − residual/detune from the second half of a static hold;
refused when the detector does not read the input's own detune (within 2 cents - a detector or
routing fault refuses everything), when the output is silent or unconfident, when the
steady-state IQR exceeds 3 cents. SPEED = the DURATION of each output transition after a flip,
from 50% of its excursion to 10% and staying; read off the output trace alone, so a 150 ms plugin
latency leaves it unchanged (pin V6; the mutant that anchors on the input flip is red). Refused
when the late plateau is not a plateau (IQR over 3 cents - a 2 s time constant read as "settled in
736 ms" before this guard), when fewer than 3 edges time, when edges disagree by more than 2×; a
transition shorter than one window is reported as a BOUND ("faster than 21 ms"), never a number.
Pins S1-S5, V1-V6, P1-P2, R1, A1; seven mutants red.

**Plan:** every control with the tuner-lexicon "strength" role (strength / retune / speed /
amount fold into one role on purpose) is swept with both generators, 8 positions (or every step),
one process per generator; the two curves say which is which. Key/scale is a precondition like a
ratio: the note is A3 (220 Hz), in every major scale and chromatic, so its instantiate text is
recorded and nothing is written. `--cert-tuner --product NAME` writes `<out>/tuners/<identity>.json`
with `pitchCandidates[]` + `pitchReview`; the defaults pass runs first, as for any unseen version.

**Live, 1 Oct 15:39-15:42 (batch-9 README has the table):** Auto-Tune Pro and EFX+ give a clean
monotonic retune-speed curve - 565 / 416 / 256 / 139 / 85 / 48 ms at displays 226 / 126 / 69 / 36 /
17 / 6, a bound under 21 ms at 0, and an honest refusal at 400 (not settled within the 1 s half
period); strength 1.0 at every position. bx_crispytuner's Amount gives a strength CURVE (0 to
1.07 - over-correction past the note at 86-100) and ~140 ms transitions. Two leads: Auto-Tune
Artist 9.5.0 refuses speed at every position where Pro/EFX+ measure (same displays); Auto-Tune
Access's Slow/Medium/Fast control reports continuous and 6 of 8 writes did not land - the norms
list needs the text-step scan. Melodyne refused by name; MetaTune showed PACE's window; EFX 9.0.1's
list-params was refused. Decision for Kathy in the handover: where tuner records live.

## 16. One store (ruled 1 Oct)

The store is where records live; the schema is what a record says. Compressor and tuner records
share `cert-fixtures/profiles/` (and `~/Library/ejmap/cert/fixtures/` live), discriminated by
`schema` (`ej_cert_compressor/1` / `ej_cert_tuner/1`, stamped on every record kind incl.
refusals). `sweepRecorded` counts `pitchCandidates`; discovery offers category `pitch` as a
candidate with the tuner certification; `--cert-sweep-all` dispatches by `Subject.category`;
one census, one zip, one runbook loop. Tuner refusals (ARA-only: `ara_only`, permanent; window
or defaults: transient) are records too. The deciding argument was the mapper: two stores is
two loops, two censuses and a second chance at the "work never comes home" defect. Pins R1b,
R1c, F2/F3 re-ruled; three mutants red. Store re-derived to stamp the schema: 0 verdicts changed.
Sean's contract: untouched; told in the reply doc, section 5a.

## 17. The exporter to ej_comp_profile/1 (COMP_PROFILE_SPEC v1.1), 1 Oct

One exporter, one place: `EjmapProfileExport.h`, `--export-profile <record> <out>` /
`--export-profiles <store> <dir>`. A pure function of one store record; a record that cannot
honestly fill a required field is refused with the reason, never padded. The level reference -
ours peak, his sine RMS - is the likeliest silent error and is pinned twice: against the constant
(3.0103 dB, X2; the subtraction's mutant is red) and MEASURED from MCompressor's committed
Peak-arm trace, where the probe printed -27.0103 dB RMS beside the -24 dBFS peak hold (X0). Field
rules as his section 3 (topology from the ROLE, never the reference mode - a pin caught that;
engage only from the with/without test and never a never_touch name; neutral = the preconditions
with their read-back; reference_ratio = the read-back ratio; static_gain_db from the two-quiet-level
reference or NO PROFILE; level_coupling from the per-position quiet gain; fit = his model
grid-fitted, max error and error against his 2 dB target, nothing tuned; stepped on stepped
amounts; time omitted). X0-X13, five mutants red.

**map_fp against reality:** EchoJay's own persisted identity->fp index
(`~/Library/EchoJay/chain_fp_scan.json`, written at slot load, the source of EJDialSummary's
`fp=` which prints only 12 characters) holds 91 of our records: 91 of 91 match (M2). So we ARE
computing his map_fp; plugin_id + version is the fallback for records that predate the field.

**Dry run on the store, 1 Oct:** 0 of 103 export - and every refusal is right: 42 are topology
`other` (several candidates, no human pick), 25 have no two-quiet-level reference (soft-end
sweeps), the rest are flat / too few 1 dB crossings inside a 3-level sweep. That is the case for
the profile sweep (section 18): 31 levels, the quiet reference on every position.

## 18. The profile sweep (--profile), built 1 Oct

`--cert-sweep --profile --product NAME` runs his section-4 method on our harness: −60..0 dBFS
peak in 2 dB steps (31 levels), ASCENDING inside every fresh per-position process (loud-to-quiet
contaminates through release, arm B), 2.5 s hold, last 300 ms read, the per-position quiet
reference on every product by design (−54 and −48 are steps of the grid), 997 Hz kept and said in
`measured.signal`. The NEUTRAL set is written as preconditions where a role names it - mix 100%
wet, make-up 0, auto make-up off, drive at its cleanest (by name: drive / saturation / sat /
color / harmonics / warmth) - each chosen on the control's own text grid by the ratio raise's
mechanism and carried on the record with the text it READ BACK (`preconditions[]` with `role`);
a control whose texts never parse is left alone and said. Engage detection and the quiet
fallback run as before; the quick engage probe stays three levels. Re-derivation recognises a
profile sweep by its 31-level grid. Pins N1–N9, four mutants red. Records from a profile sweep
are what the exporter accepts.

**Checked, not assumed (1 Oct):** Pro-C 2 is NOT installed on this Mac; no component named
Logic Compressor exists outside Logic (Apple's AUDynamicsProcessor is the only Apple dynamics AU);
the four Waves units here are all **V12.0.0** (his example keys 15.0.70 - a V15 map_fp will not
match a V12 record); UAD 1176 / LA-2A are hardware-conditional. His first ten reachable here:
Tube-Tech CL 1B, EMO-D5 (s), NEOLD U2A, NEOLD V76U73, Mike-E Comp, CLA-76, CLA-2A, VComp (8).

## 19. v1.2 (1 Oct, late): measured points replace the formula

`docs/COMP_PROFILE_SPEC_v1_2.md` is the contract. Section 6 now picks the amount position on
measured `in_at_gr_dbfs`; the v1 threshold formula is withdrawn (it matched a start-of-compression
threshold against a 1 dB point and landed about 1 dB too much GR - what our section-6 numbers
showed from the other side: the curves climb slower than the formula assumes).

- `in_at_gr` {1, 2, 3} per position in the record (section 2 of the request): linear interpolation
  between the two steps that straddle each target, only where GR rises across it; `not_reached`
  (never by the loudest level) and `below_range` (already past at the quietest) kept APART in the
  record; `null` where a readable rising straddle does not exist (a gap); never extrapolated.
  `eff_threshold` IS `in_at_gr[1]`, the same value. A quality figure beside: non-monotonic
  straddles (a fall on either side of the one interpolated) and the widest straddle. Pins G1-G6.
- Exporter v1.2: `in_at_gr_dbfs` on every point (both words become null, as his spec says),
  `eff_threshold_dbfs == in_at_gr_dbfs["1"]` written identically (X14), `stepped` boolean with
  every detent or the export is refused, `steps_dbfs` [start, end, step] in level_ref units
  (−63.01, −3.01, 2) and a refusal for a non-uniform grid, `detector` "unknown" until measured,
  the full-scale 997 Hz sine exporting as −3.01 (X2b, his section 5 pin), and `fit` computed
  exactly as the contract says but NEVER a gate (X18) - Kathy is asking Sean to redefine it - with
  the measured-point quality beside it. Five mutants red.
- Open: the level sweep itself (section 18) has not run - battery since 18:0x. CL 1B and EMO-D5 (s)
  are armed behind the mains watcher; the section-6 pick and his section-8 tone check (0.5 dB) run
  on this Mac after that, then the Desktop folder.

## 20. v1.3 / v1.4 (1 Oct, night), folded into the armed run

v1.3 and v1.4 arrived while the CL 1B profile run waited on mains, so they went into the binary
the run starts with. Record: `inAtGrRepeat` (the hold-doubled pass, 5 s hold, same 300 ms read)
and `quality {repeats, method "hold 2.5 s vs 5 s", point_error_db, pointsCompared,
shapeDisagreements, withinPositionsMonotonic, acrossPositionsMonotonic, violations}`; nothing is
averaged - the 2.5 s sweep is the curve, the 5 s one sits beside it, the worst disagreement on any
in_at_gr point is the number. Export (v1.4): `detector_f` and `quality.point_error_db` REQUIRED
(a record without either is refused), `quality.method`, the monotonic self-check computed from
the exported points (strict within, one direction across with nulls skipped and equal neighbours
allowed), `notes` naming the guards that passed. Pins N2b, X16, X19-X21, D3-D5, M1-M3, Q1-Q6;
eight mutants red. Live traces of the repeat are tagged `r2.` + the run's prefix; re-derivation
loads them (`loadRepeatPositions`).

**Run order now:** CL 1B only on this Mac (EMO-D5 waits for Waves 15 - a V12 profile will never
match a 15.0.70 map and V12 runs bridged where V15 is probably native). After the sweep:
export -> tone check (L = -18 dBFS RMS, g = 2, pass within 0.5 dB) -> detector_f -> re-export, then
the Desktop folder. **When Waves 15 lands (Sean replaces, not stacks):** quit EJ Map, `killall
AudioComponentRegistrar`, reopen, Scan (the scan cache is from 4 Aug); census; confirm EMO-D5 (s)
at 15.0.70 with map_fp 32b7e1d9a0c3...; note native vs bridged (write landing differs); every
Waves record in the store is V12, so the whole Waves set becomes new identities - cost it from the
census before running beyond EMO-D5; any V15 product the census cannot see is unmapped and needs
the runbook's section 3 sweep first; then EMO-D5 (s): engage detection, the v1.4 profile run, tone
check, export.

## 21. The reference ladder, and three things the first live profile run exposed (2 Oct, morning)

Mains came back at 07:46 and the CL 1B profile run went first. The sweep certified, the hold-doubled
repeat agreed to 0.1 dB, and the export refused: "only 3 curve points reach 1 dB inside the measured
levels (his rule: at least 9)". The record said why: the per-position quiet reference (-54 and -48,
6 dB self-check) **failed on positions 6-15**. CL 1B's threshold runs to -41 on the display, about
-57 dBFS peak, and with its soft knee the plugin is already compressing at -48 on ten of sixteen
positions. The guard refused those positions rather than calibrate off a compressed reading - right -
and the curve had three points.

**The ladder.** `kQuietLadder` = (-54,-48), (-66,-60), (-78,-72), (-90,-84). A profile sweep renders
the five levels below the grid (-90..-66) ahead of it, quiet to loud (`probeLevels()`, 36 levels; the
derivation still reads the 31 of the grid). `derive` walks the rungs the trace holds, loudest first,
and the first whose pair differs by 6 dB within 0.1 is that position's reference (loudest passing =
farthest from the noise floor). A position that passes at no measured rung is refused, and the reason
names the last rung walked. A trace that holds only -54/-48 - every record before today - walks one
rung and reads exactly as before. The record carries `linearReference.rung_dbfs` per position, a
`descended` count and the `ladder_dbfs`; `levels_dbfs` still names the first rung. The tone check
renders the whole ladder below L so the picked position gets the same rule. Pins L1-L7 (loudest
passing wins over a deeper pass; the reason; the gain read at the rung's upper level; the one-rung
trace unchanged; the record fields), N1 updated; four mutants red (never descends: 6 pins; quietest
passing: 3; gain read at -48 regardless: 1; rung unrecorded: 1).

**Live, CL 1B 2.5.62, 08:17:** 16 of 16 positions referenced, **10 below the first rung** (positions
6-8 at -66/-60, 9-10 at -78/-72, 11-15 at -90/-84), tone_frac 1.00 down to -90, every quiet pair
within 0.02 dB of 6. Certified, higher_is_harder, T(peak) -13.4 .. -54.4, repeat agreement
point_error_db 0.1 over 36 points. The second run (08:31, the shipped binary) is the record.

**Two defects the chain then exposed, both older than today:**

1. **A one-position derive returned nothing.** The detector's and the tone check's processes hold one
   position; `derive` returned "fewer than two positions" before computing any reduction, so
   `inAtGr` was empty, the detector returned `nullopt` silently and said "a 2 dB point was not reached
   on one signal" - a message about the signal for a defect in the reader. The live detector had
   never recorded a fraction. Now the quiet-reference reduction and the in_at_gr straddle are
   lambdas shared by both paths, and a one-position quiet-reference derive stays `unreadable` as a
   map but carries its reduction and in_at_gr (D6, D6b). The detector names the derive's result and
   reason when it has no curve.
2. **A merge with no reference process was never ok.** `Measured::ok` came only from the reference's
   own `sweep` line, and the detector, the tone check and the repeat path's fallback merge with a
   `{"", clean, "none"}` reference - every one derived as "no sweep output". A reference-less merge
   is now ok when a position's process was (D7).

Neither pin could have been written from the message: both said something true about the signal.
The one that found them was the live chain, and only because the export refused first and the
chain was run by hand.

**Detector, live:** sine 2 dB at -10.9, two-tone (997 + 1201 Hz, same peak) at -12.1: shift 1.20 dB,
**f = 0.40**, word "unknown" by his rule (neither end). Recorded raw and clamped.

**Tone check, live (section 6 at L = -18 dBFS RMS, g = 2): FAIL by the rule's own pick.** The 8 dB
clamp removed position 4 (1 dB point -29.9, more than 8 dB below L = -15 peak), leaving position 3
alone (2 dB at -13.9) with nothing to interpolate toward, and the GR there at L is **1.33 dB**
(target 2.0, bar 0.5). Measured beside it, not in the rule: the clamp-free interpolation between
positions 3 and 4 (norm 0.2077) gives **1.58 dB**; the midpoint norm 0.2333 gives 2.37. On a 6:1
soft-knee opto the 1 -> 2 dB span is 7 dB and the next position's 1 dB point is 9 dB lower, so the
clamp and the bracket cannot both hold. This is his rule's number on his device; it is reported, not
tuned.

**A third defect:** the discovery path never stamped `manufacturer` (the 103 store records got it by
hand on 1 Oct), so the export's `plugin.manufacturer` was empty. `composeFixtureImpl` now writes the
host's `manufacturerName` (M0b).

## 22. Grid refinement, the ratio and neutral fields, the tone check's writes (2 Oct, 10:00-10:40)

Kathy's six items on the 08:35 export, built in order, each committed and pushed:

1. **Grid refinement** (`sweep::refineNorms`, driver `refineGrid`): wherever adjacent positions' 2 dB
   points differ by more than 3 dB, ceil(gap/3) − 1 positions are added between them and measured
   with the same procedure (preconditions, engage writes, quiet ladder, hold-doubled repeat) under
   the same tag prefix with the next indices, until no gap is over the bar (4 rounds / 64 positions
   cap). A null neighbour is an absence, not a gap. `gridRefinement {rule, gap_db, rounds,
   added_norms}` on the record, restored on re-derive. Pins G1-G5b, four mutants red. CL 1B: 11 + 1
   positions added; the 9.3 dB jump between norm 0.200 and 0.267 is now six steps of ≤ 3 dB.
2. **Ratio**: an adjustable ratio never exports as fixed (X22-X22c): one curve point at the norm the
   sweep ran at (a ratio_raise precondition's, else the instantiate value), its display, the value
   read back, `measured_ratio` implied from level dependence and labelled so; `fixed` null;
   `knee_db` null everywhere (none measured).
3. **Neutral** = the measurement conditions (X23-X24): every control except the amount, the ratio,
   readouts/meters (`isReadoutOrMeter`: the readout flag or a name answering meter/vu/readout/display),
   the engage writes and never_touch, at a precondition's set value where written else the
   instantiate value from the defaults sample, with set text, norm and source; a control with no
   instantiate value refuses by name. Four mutants red across 2 and 3.
4. **Tone check writes** (`profile::toneWrites`, T1-T3): engage[], neutral[] and ratio.curve[0] from
   the exported profile, resolved by name through the record's controls, then the section 6 pick -
   the record's own sweep is no longer consulted. Live: six writes, 1.85 dB, PASS.
5. `~/Desktop/ej_profiles_CL1B_v1.4.zip`: `ej_comp_profile/` + `SEAN_MAC_RUNBOOK.md`, nothing else.
6. Reply doc: the clamp is checked on positions not the pick; the 1 → 2 / 1 → 3 spacings (6.7 / 10.3
   dB) cap the reachable GR at about 2.3 dB under an 8 dB clamp.

Known, not fixed: `repeatFor` looks up the repeat by "" or "q." and never by an engaged prefix
("e<idx>."), so an engaged profile sweep would get no repeat quality - no engaged product has been
profile-swept yet; the refinement uses `lastSweepPrefix`, which is the key sweepFor wrote.

## 23. After the iLok: licence-free work (2 Oct, from 10:39)

The iLok watcher (`ilok_watch.sh`, every 20 s) logs each change of the dongle's presence; its log is
committed with the day's traces so a later reader knows why no PACE product ran after it left.
The census's licence classification (bundle PACE wrapping, `isPaceWrapped`) decides what is runnable:
of the 98 compressor records, 74 are licence-free, 20 PACE, 4 unresolved by uid (Solid Dynamics,
Solid Bus Comp, Drawmer 1973, TBTECH Cenozoix - no bundle matched; treated as not runnable until
classified). Waves V12 records are also left alone: Sean replaces 12 with 15, and a V12 profile
never matches a V15 map (the EMO-D5 ruling).

**NEOLD U2A** (licence-free): flat with the neutral set, and the engage signature once that was
widened (section 22): the search ran, tried its one candidate (Mode -> Limit) and the sweep stayed
flat - Peak Reduction and Mode both land (read back) and change nothing in the audio; the GR Meter
parameter reads 1.0 throughout. Nothing else on the product is switch-shaped. LEAD, not chased: a
plugin whose parameter writes are accepted but not processed (a bank/preset layer?). The record in
the store carries `engageWrites.tried`.

**NEOLD V76U73** (licence-free) - refused at plan, class none. PROPOSAL, not built:
- The known trap is on the record: `Mode` instantiates at **'Bypass'** (norm 0.5, between 'Compress' 0.0
  and 'Limit' 1.0). The engage write would be Mode -> 0.0 'Compress'. Today's engage candidates never
  consider a control whose texts are Compress/Bypass/Limit (not on/off-shaped, and "bypass" is a
  never-touch word), so the search would not find it; the plan needs the write as a named rule
  (a three-text mode control whose instantiate text is 'Bypass' and whose other texts name a process).
- No threshold exists: the U73 is a vari-mu whose amount is the level driven into it. Amount control:
  `Send` (-24..+24 dB, index 16, the level from the V76 stage into the U73) - **topology input_drive**,
  quiet reference per position. The alternative `Gain` (43-76 dB, the V76 preamp) also drives it but
  carries the preamp's saturation; `Trim` and `Makeup Gain` are output trims (neutral). The role
  lexicon does not know "Send" as drive; it would need the word.
- Ratio: none (vari-mu); the profile's ratio block would be `fixed {measured_ratio implied}`.

**Empirical Labs Mike-E Comp** (PACE: waits for the iLok's return) - refused at plan, class
amount_only. PROPOSAL, not built:
- Amount control `Drive` (0-10, index 3): **topology input_drive** (a Distressor-family input drive).
- `Ratio` (index 5) is stepped with a **'Bypass'** detent at norm 0 and 'NUKE' at 1; the instantiate value
  '4:1' is a working ratio, so no ratio raise, but the ratio_raise rule must never choose 'Bypass'.
- `Comp Mode` instantiates at 'Off' (Off / Low Freq Emph / High Freq Emph): on the hardware this is the
  detector emphasis, not an engage - unverified here; the engage search would try it only if the
  sweep read the signature, which is the right test.
- `Preamp Gain` 'CLEAN' and `Mix` 10.00 are the neutral conditions as instantiated; `Out` is a trim.

**Item 4 (the rest, licence-free, already certified in the store, non-Waves), highest quality first by
numeric 1 dB points on the certification sweep:** Lindell SBC (9/16), Lindell 254E (8), Lindell 7X-500
(8), bx_townhouse Buss Compressor (7), elysia alpha mix (7), Bettermaker Bus Compressor DSP (6), elysia
mpressor (6), Acme Opticom XLA-3 (4), bx_opto (4). Cost, from CL 1B (200 s with two refinement rounds)
and U2A (169 s with the engage search): about 3-4 minutes each, 30-40 minutes for the nine, plus the
after-chain (detector, export, tone check) under a minute each. Started 10:51.

## 24. PROPOSAL (not built): the channel-strip rule, measured on the store (2 Oct, 11:30)

EMO-D5 needed `--candidate "Comp Thresh"`; on a stranger's Mac nobody picks. Until a rule lands, every
record with several threshold candidates ends as `needs_review` (a pass state, EjmapLoop.h L3). The
rule as proposed, applied to the 42 multi-candidate records in the store today:

**Rule 1 (as asked): exactly one candidate whose name carries the compressor stage word (a token
starting "comp"), and it certifies; the other stages stay at their instantiate defaults; that
candidate is the amount control.** Decides **4 of 42**:

| product | pick | the others |
|---|---|---|
| EMO-D5 (m) 12.0.0 | Comp Thresh | Gate Thresh certified, Limiter Thresh certified, Leveller unreadable, DeEsser flat |
| EMO-D5 (s) 12.0.0 | Comp Thresh | same |
| Solid Dynamics 1.4.5 | Threshold Comp | Threshold G/E flat |
| MDynamics 14.16.0 (PACE) | Compressor -> Threshold | Processor 1 - Threshold ALSO certified (not comp-worded), Gate flat |

Refused by Rule 1 and why, the other 38: no candidate carries the word (34: multibands' "Band N",
L/R and 1/2 twins, Low/Mid/High, Processor N, Optical/Discrete); several carry it (MTurboCompMB 6
bands "Band N Compressor", Maag MAGNUM-K "K Comp Threshold 1/2" both flat, Auto-Tune Vocal
Compressor 4 "Comp" candidates, 1 certifies). MDynamics is the one to look at: Rule 1 picks the
compressor while a second, differently named stage also certifies - the rule says "other stages at
their defaults", which is what the sweep did, so the pick is still the right one for the server;
flagging it so you see the shape.

**Two extensions, measured, for you to accept or refuse (each a separate rule with its own pins):**

- **Rule 2 - exactly one candidate certifies and no candidate is band-named** (a "Band N", "Low/Mid/
  High", "L/M/H" name means a crossover, and the one certifying band is the one the 997 Hz tone
  landed in, not the product's amount control). Decides **+2**: Unfiltered Audio Zip (Threshold;
  Auto Threshold flat), PuigChild 670 (s) (Left Threshold; Right flat - the probe's tone is on the
  left). Would NOT decide C4/C6 (band), Lindell 354E/MBC (Low/Mid/High), SSL G3 (Low/Mid/High),
  MTurboCompMB (bands), MSpectralDynamics/Mini (Processor 1 of 2 - not band-named, but a two-processor
  chain; I would hold these as needs_review by a "Processor N" exclusion - your call).
- **Rule 3 - channel twins: exactly two candidates whose names differ only by a channel suffix
  (L/R, 1/2, A/B, L/M vs R/S, "" vs " R") and BOTH certify; pick the first and record the twin so the
  server writes both.** Decides **+9**: DPR-402 (s) (L/M + R/S), SPL IRON (L + R), elysia alpha
  master (1 + 2), AMEK Mastering Compressor (1 + 2), Millennia TCL-2 (1 + 2), Vertigo VSC-2 (A + B),
  Abbey Road RS124 (s) (Input Control + R), MaxxVolume (m/s) are NOT twins (Low Level / High Level
  are two stages, both certifying - needs_review stays), Shadow Hills ×2 are NOT twins (Optical /
  Discrete are two circuits - needs_review stays).

**Stays needs_review under all three (27):** every multiband (LinMB ×2, C4 ×2, C6 ×4, MDynamicsMB ×2,
MTurboCompMB, OTT, DynOne3, Lindell 354E/MBC, SSL G3, Drawmer 1973), dbx-160 (s) (nothing certifies),
MDrumLeveler, Pro Audio DSP DSM V3 (3 certify), Kiive XTComp (3 input controls certify), Maag
MAGNUM-K, Auto-Tune Vocal Compressor, MSpectralDynamics ×2, MaxxVolume ×2, Shadow Hills ×2. That is
the honest shape: a multiband or a two-stage device is not a single amount control, and nobody
should pick for it.

The rehearsal (item 5) runs with NO rule: channel strips end as needs_review.

## 25. AUDIT (not a fix): every place certification reads a MAPPING artefact or verdict (2 Oct, 14:15)

Three mapping rules gated certification silently today, each found by the fresh-ledger test: the
categorise step asked only about the mapping worklist; discovery obeyed mapping verdicts; the mapping
sweep skips `no_dial_set`, which is what the server says about every tuner. Kathy asked for the whole
list before anything else is built. Read from `EjmapCertDriver.h` (`loadDiscoveryInputs`,
`discoverCandidates`, `buildWorklist`, `quarantinedBundles`, `measurable`), `EjmapLoop.h`,
`MainComponent.h` (`decideSweep`, `loadCategories`, `uncategorisedWorklistProducts`, `collectWorklist`),
`EjmapLedger.h`, `PluginScanner.cpp`.

| # | artefact / verdict | what MAPPING uses it for | what CERTIFICATION uses it for | right for certification? | what breaks on a fresh Mac |
|---|---|---|---|---|---|
| 1 | `categories.json` → `category` (by `mark_keys` = `Format\|uid`) | which products the mapping sweep opens (`sweepable` = disposition sweep ∧ ¬refused_by_both ∧ category non-empty) | THE worklist: category `compressor` → compressor certification, `pitch` → tuner certification; nothing else is a subject | yes — this is the one mapping artefact certification should depend on | (a) the endpoint reply carried no `mark_keys` → 0 categorised identities [FIXED today: stamped on merge]; (b) only the mapping worklist was asked → mapped products had no category [FIXED today: every scanned product]; (c) the catalogue's content: every real-time tuner is `category null` (5 Aug verdicts) [BLOCKED on Sean, section 24 of COMP_PROFILE_REPLY] |
| 2 | `categories.json` → `disposition` (+ `why`) | the sweep/skip verdict: `sweep`, `no_dial_set`, `review`, `not_a_processor`, `operator_excluded` | until today: any non-`sweep` disposition excluded the product (30 Sep ruling); now only `operator_excluded` and the hang/crash family (re-ruled 2 Oct) | partly — the operator's exclusion is right; a mapping verdict says nothing about measurability | 202 of 1073 products were held, every tuner among them [FIXED today] |
| 3 | `categories.json` → `refused_by_both`, `kind`, `hedged` | `sweepable` (a product both arms refused differently is still refused); `kind` is the arms' free text | not read | right not to read them; note `kind` already says "pitch correction" for exactly the tuners the catalogue files as null — it is the evidence Sean's fix can use | nothing |
| 4 | `map-state.json` (server: 0 unmapped, 1 local only, 2 submitted here, 3 submitted elsewhere, 4 different build) | the mapping worklist (unmapped / different build / unknown are offerable) | **the discovery gate: a product is a subject only if MAPPED (local or server) at its installed build**; also `mappedBy` on the row | **questionable** — see the tuner proposal below: certification samples its own defaults, resolves the plugin in the AU registry and computes `map_fp` itself; the map is used for nothing but its category (and only when local) | a product with no map anywhere is never a subject (now a named `unmapped` row); the file is re-fetched at every GUI/`--sweep` launch, so a scratch edit before the mapping sweep is overwritten (the test's edits go after it) |
| 5 | local maps `maps/*.json` → `identity`, `category` | the mapping sweep's product; the send queue | category (wins over `categories.json`) and "mapped (local map)" | the category is fine; the precedence is a choice (a local map's category is the mapper's own categorisation at sweep time, from the same `categories.json`) | a fresh Mac has none until the mapper sweeps; nothing breaks, nothing is gained |
| 6 | the mapping sweep's SKIP rules (`decideSweep`): `skipped_uncategorised`, `skipped_<disposition>` via `sweepable`, `skipped_quarantined`, `marks.isUnmappable`, `marks.hasIssue` (flagged) | which plugins get a map | **not read directly — inherited through #4**: whatever the mapping sweep refuses to map can never be discovered | **no** — this is the third gate: an UNMAPPED tuner (`no_dial_set` on the server) is never mapped, so even after the catalogue gives it `category pitch` it has no map and fails #4 | every unmapped tuner, forever, and any unmapped compressor the catalogue marks `review`/`no_dial_set`; the test measures it today with bx_crispytuner (category pitch set as a stand-in, disposition left as served) |
| 7 | `quarantine.json` (the ledger's retry rule: hang at scan on the first timeout, deaths at the threshold) | the scan skips the bundle; the sweep skips the plugin | the row `quarantined_at_scan` (named today); the certification PROBE never consults it — it runs the plugin out of process under its own budget (2 unclean, once-retried) | mostly — a scan-stage quarantine of a VST3 never touches the AU; a LOAD-stage quarantine (the plugin crashed the mapper) is not read by the batch, which will try the AU and spend its budget (`refused budget`) | nothing silent now; a crasher costs ~2 × timeout per product |
| 8 | `licence-stops.json` (new today: the scan's window watch) | the scan skips the bundle until `--retry-licence` | the row `needs_licence`; the census section | yes | nothing silent; a licence stop costs ~3 s |
| 9 | the batch's own `held (licence)` — `isPaceWrapped` bundle marker + `--include-pace` | — | `measurable()`: a PACE-wrapped product is held unless `--include-pace` | **inconsistent with today's ruling at the scan** ("the window is the evidence; PACE-wrapped is not unlicensed"): on a Mac WITH the iLok every PACE product is held unless the mapper passes a flag — a hand step. The probe already has the window watch (`uiShown`), so the batch could try every product and let the window decide, writing `needs_licence` | a licensed PACE product on a licensed Mac is skipped; an operator must know to pass `--include-pace` |
| 10 | `scan-cache.xml` (the scan rows) | everything the mapper does | NOT read by certification (`installedAudioUnits()` reads the AU registry) — but the categorise request and the map-state fetch are built from the rows, so a product missing from the scan has no category and no map state | fine for AUs (the AU scan reads the registry, loads nothing); a VST3 stopped or quarantined at scan does not remove the AU | nothing, given #1 and #4 |
| 11 | `marks` (unmappable / issue flags) | the mapper's hand decisions | not read | right | nothing |
| 12 | `categories.json` → `operator_excluded` written by the runbook's exclusion script (§3, §B) | the sweep leaves the plugin alone | honoured (#2) | right: the operator's escape hatch reaches cert | nothing |
| 13 | `config.json` (the sign-in token) | categorise, map-state fetch, send | the categorise step of the runbook needs it; the batch itself never touches the network | right; the zip must exclude it (it does: `cert/` only) | a Mac that never signed in gets no categories and no map state: the runbook's one-time step |

**For tuners specifically — does certification NEED a map?** No. `runCertTuner` resolves the product by
name in the AU registry, samples its defaults with the probe (the controls come from the plugin, not from
a map), roles them with the tuner lexicon, runs the pitch sweeps, and the record's `map_fp` is computed
from the probe's own parameter count (`fingerprintForDescription`), not read from a map. The compressor
path is the same: the defaults pass supplies the controls; the only thing a map contributes is a
category, and only when it is local. The MAPPED gate in discovery (#4) is a proxy for "someone has deemed
this a dialable processor", inherited from the mapping worklist, and it drags the mapping sweep's skip
rules (#6) into certification.

**Proposal (not built; your ruling):** discovery keyed on INSTALLED + CATEGORY (`compressor` or `pitch`
from `categories.json`, by product key where `mark_keys` are absent), with the map state recorded on the
row as information (`mappedBy: local | server | none`) and never as a gate; `unmapped` stops being a
state and becomes a field. Consequences: a product the mapper never mapped is probed under the probe's
own budget (2 unclean, once-retried) and ends `refused` or measured; the `map_fp` on the record is still
ours; the server's map for the identity, if any, is unaffected. With the catalogue fix (section 24 of
the reply doc) this makes the eleven tuners subjects on any Mac; without it, nothing changes for them.
If you would rather keep the gate, the smaller change is #6 alone: let `--sweep` map `category pitch`
products whatever their disposition — but that is a mapping-sweep rule, and it is yours.

Also proposed from #9: the batch stops pre-holding PACE products by the bundle marker and lets the
probe's window watch decide, the same rule as the scan; `--include-pace` goes away.

## 26. The ruling on the audit, applied (2 Oct, 14:30)

**1. Certification does not need a map** (reverses 29 Sep). The map was a proxy for "somewhere to
attach"; the join is by `map_fp`, which the record computes exactly as EchoJay does (ab70ea5337fe…
matched Sean's log), and it can happen whenever the map arrives. Discovery = **installed + category
(compressor | pitch) + not excluded**. The map state goes on the record and the row as INFORMATION:
`mapState` / `map` = `local map` | `server map state N` | `server map at a different build` | `none`.
"Unmapped" is a field, not a state. **This changes the coverage state machine: certification no longer
waits on mapping.** The census's "NO MAP YET" line is information. Pins: F5 (an installed, categorised,
UNMAPPED compressor is discovered, map `none`; a different-build map is named), F4 (the no-map list is
information), L16 (`unmapped` is not a state); mutant: the old MAPPED gate back in → F4, F5 red.

**2. Licence, from the scan's evidence** (accepted with the change). The PACE-marker hold and
`--include-pace` are gone (`measurable` no longer reads `licenceBound`; the flag prints "ignored").
Three paths, pinned:
- recorded `needs_licence` at the scan → carried forward as that state, **not loaded again**
  (`loop::carriedLicenceStop`, matched by product name with (m)/(s) suffixes ignored; L17; mutant red);
  a hang quarantine is not a licence stop (L18; mutant red);
- loaded fine at the scan → runs, no flag (L18);
- a window at the probe's load in the batch → `needs_licence` too, not `refused` (L19; mutant red) —
  the probe's window watch still guards every batch load, a licence can vanish between scan and batch.
`--retry-licence` on the batch re-checks just the needs-licence set: the carried-forward products are
loaded, and only the refusals a window caused come back (R12; mutant red). M5 (both reaches): a
PACE-wrapped subject is measurable; mutant red.

**3. The principle, re-checked row by row** (section 25): *certification may be gated ONLY by facts
about the plugin — installed, category, operator exclusion — and by evidence observed at load — licence
window, hang, crash — NEVER by a verdict the mapping workflow produced for its own purposes.*

| row | verdict against the principle | changed |
|---|---|---|
| 1 category | a fact (the catalogue's classification) | kept; the catalogue's category is now read FIRST, a local map's only when the catalogue has none |
| 2 disposition | `operator_excluded` is a fact; the "hang/crash family" of disposition WORDS was a verdict dressed as evidence — load evidence lives in the ledger, not in categories.json | **changed**: only `operator_excluded` excludes (R10c; mutant red) |
| 3 refused_by_both / kind / hedged | verdicts; not read | unchanged |
| 4 map-state gate | a mapping-workflow fact used as a certification gate | **removed** (point 1) |
| 5 local map category | a fact, same source as 1 | precedence swapped (see 1) |
| 6 the mapping sweep's skip rules | verdicts, inherited through 4 | **no longer inherited** (4 removed): an unmapped tuner is a subject |
| 7 quarantine.json | load evidence (hang / crash) — admissible | unchanged: the batch's probe gathers its own load evidence out of process; a scan-stage VST3 quarantine is a row, a load-stage one is not consulted (allowed, not required) |
| 8 licence-stops.json | load evidence (a window) | **now carried forward** (point 2) |
| 9 PACE marker + `--include-pace` | a fact about the bundle's wrapper, not about the licence — a pre-judgement | **removed** (point 2) |
| 10 scan rows | facts | unchanged |
| 11 marks | the mapper's hand decisions, not read | unchanged |
| 12 operator_excluded | the operator's fact | unchanged, honoured |
| 13 config.json | the sign-in, for categorise / map-state only | unchanged |

Tuners stay BLOCKED on the catalogue (section 24 of the reply doc): with category null they are not
subjects; with category pitch they now are, map or no map — bx_crispytuner with its labelled stand-in
category is the test of that.

## 27. The test L per level, the known-licence skip, the section 11 guard (3 Oct, morning)

Three rulings after the v1.7 build, all measured on the tc17 folder (the four rehearsal products) and
on CL 1B with the iLok away.

**1. A level the clamp refuses at OUR L is not a level that failed.** `profile::toneLevelFor (profile, g)`
chooses the test L per level: the median of `in_at_gr[g]` over the positions carrying g, then those values
by distance from the median, each within `measured.steps_dbfs[0..1]`; the first whose §6.4 pick passes the
12 dB clamp is the L. The same rule applies to g = 2 (`--L` on the hand command overrides it). Per level the
tone check records `L_rms_dbfs`, `L_rule` and, when nulled, `null_reason` ∈ {`failed_check_at_L`,
`no_valid_L_clamp_geometry`, `no_valid_L`, `could_not_run`} plus `spacing_1_to_g_min_db`. The profile's
notes carry the reason and the spacing. Re-run of the four (18.5 s): every level that has a valid L PASSES
within 0.12 dB — SBC 4 dB at −15.21 (4.01), 5 dB at −13.66 (4.99), where the fixed −18 had nulled both;
bx_opto / 7X-500 / mpressor 2, 4, 5, 6 all PASS at their medians (−29.7…−15.9).

**The finding for Sean (§6.4 of the spec, not ours to change):** with the clamp on the pick's own 1 dB
point, a level whose 1→g spacing is ≥ 12 dB at every position has NO valid L — not at −18, not anywhere
in the range. Measured 1→g spacing (min / median, dB):

| product | 1→2 | 1→4 | 1→5 | 1→6 |
|---|---|---|---|---|
| Lindell SBC | 3.5 / 3.6 | 9.1 / 9.2 | 11.6 / 11.7 | **14.1 at every position → no L** |
| Tube-Tech CL 1B | 6.3 / 6.7 | 11.7 / 12.3 (some positions inside 12: an L may exist) | **13.5 / 14.1 → no L** | **15.3 / 15.9 → no L** |
| bx_opto | 1.9 | 5.1 | 6.6 | 8.0 |
| Lindell 7X-500 | 1.3 | 3.7 | 4.8 | 5.9 |
| elysia mpressor | 1.5 | 4.5 | 5.9 | 7.4 |

So "Lindell's 6 dB level must come back tested" cannot be met under the 12 dB clamp as written: the
driver now says so by name (`no_valid_L_clamp_geometry`, 14.1 dB) instead of nulling it silently, and
pins the rule (T7–T11: the fixed −18 refused while the median passes; g = 2 under the same rule; the
range honoured; geometry named with the SMALLEST spacing; tries recorded). Ten mutants red (fixed −18
instead of the rule, median not first, range ignored, geometry never named, max for min, both version
guard mutants, both licence sources ignored, retry not honoured). For v1.8 Sean has a choice: a clamp
relative to the level (e.g. 12 + (g − 1) dB, or on the pick's own (g−1) dB point), or a per-level clamp
table; until then the deep levels of a soft-knee unit above ~10 dB spacing stay null with the reason.

**2. Do not load a product the session knows needs a licence that is not present.** `loop::knownLicenceStop
(scanStops, outcomes, product, retryLicence)` — the scan's licence stop in the ledger, or a `needs_licence`
row in the cert folder — is asked by `runToneCheck`, `runDetector` and the tone-check-only mode before any
load; exit 6 (`kToneLicenceKnownExit`), row `needs_licence` *known from the scan / this folder's outcomes*.
CL 1B measured: known from the scan 1.0 s, no load (was: loaded, window, killed at 2.9 s); known from the
folder 0.4 s; `--retry-licence` loads and the window watch kills it at 2.3 s (the only way past). The hand
commands `--cert-tone-check` and `--cert-detector` now parse `--ejmap-ledger` and `--retry-licence` (they
silently used the real ledger before — the first measurement here loaded CL 1B for exactly that reason).

**3. Section 11.** `profile::versionMismatch (record, installed)` refuses a differing version AND an unknown
one (a guard never guesses); exit 7 (`kToneVersionExit`), row `needs_review` with the §11 reason; in the
detector too. Rehearsed on the CL 1B import with the record's version edited to 2.5.70: re-derived, refused
before load, row written, the four finished products skipped by the resume (1.3 s).

**The CL 1B import** for Sean's Mac: `tools/ejmap/packaging/export_traces.sh <stem> <traces dir> <zip>`
packs the store's record (detector, pick carried over by the mode), the processes list and the raw
captures into a `cert/`-shaped zip (refuses `config.json` or an unredacted home path); `unzip -n` over
`~/Library/ejmap` on his side; the mode walks `fixtures/` by file, not by row paths (the earlier mode read
the rows' absolute paths, which are another Mac's in a zipped-back folder — fixed), so an imported record
is found. Section in `SEAN_MAC_TONECHECK.md`.

Suite 3213 checks green; traces `cert-traces/2026-10-03-tonelevel/{tc18,cl1b-licence,cl1b-import}`.

## 28. v1.8 (Sean's spec, copied to docs/COMP_PROFILE_SPEC_v1_8.md, 3 Oct): the reverse read

**§6.4 step 2, the reverse read.** `profile::grAtLevel (point, L)`: the GR a position gives AT a level, interpolated
across all of its numeric points 1..6 (a level at its 5 dB point reports 5); past its deepest point the figure is that
deepest level, flagged `extrapolated` (below its shallowest point the same, the other way). A STEPPED pick now expects what
its chosen detent gives at L (§6.4 step 6) — `Pick::expectedGrDb` from the reverse read on that detent, `expectedExtrapolated`
beside it, the note says between which points it read — where before it expected g, which the detent only approximates.
A continuous pick sits at L by construction, so g stands (pinned: R4, and the mutant that reverse-reads a continuous pick
goes red on P6/T8). Pins P9 (re-derived: the nearest detent at the asked 4.5 is position 10, and at L it gives 3.76), R1
(5 at the 5 dB point; 4.5 halfway; past the end flagged; no points → none), R1b (1–3 only: 3, extrapolated), R2 (a detent
giving 2.6 at L expects 2.6 — passes, not failed by 0.6 against 2.0), R3 (between the 4 and 5 points: 4.5, the deep points
read). Mutants red: the read over 1–3 only (Sean's bug) → P9/R1/R3/R4; stepped expects g → P9/R2/R3/R4; past-the-end not
flagged → R1/R1b/R4; continuous reverse-read → P6/R4/T8.

**Would anything already exported have failed under the old expectation?** No, and the reason is that nothing exported is
stepped: the 33 exported profiles we hold (11 products: XLA-3, Bettermaker, EMO-D5, 254E, 7X-500, SBC, CL 1B, bx_opto,
townhouse, alpha mix, mpressor, across repo exports, tc18, the unbroken rehearsal and the batch traces) all carry
`amount.stepped: false`; tc17's four are continuous. The store's only certified record with a stepped threshold-like
control is PuigChild 660 (m) (`Input`, 21 detents) and it was swept on its continuous `Threshold`, never on `Input`; the
other "stepped" hits are switches (Comp Off / Comp In / Comp/Limiter / Comp Bypass…), word-valued, never threshold
candidates since Rule 1. So the old expectation never produced a verdict here; the new one is pinned for the first stepped
unit that does export.

## 29. The clamp for deep asks (v1.9 §6.4 step 4, Sean's ruling 3 Oct — wording CHECKED against v1.9 on arrival: it matches)

`profile::pickClampDb (g)`: 12 dB up to g = 3, then 12 + 2 × (g − 3), linear — 13 at 3.5, 14 at 4, 16 at 5, 18 at 6. The
comparison is unchanged (the pick's own interpolated 1 dB point against L); only the limit widens, and only above 3, so a
shallow pick is provably unchanged (mutant C-b, a widening that starts at g = 2, goes red on C1–C4). Used by the pick replica
(`Pick::clampDb`, the refusal names the limit and the ruling) and by the tone-check L rule (`ToneLevel::clampDb`; clamp
geometry is judged against the limit AT g — C5). Pins C1 (12/12/13/14/16/18), C2 (14–16 dB spacing: geometry at g = 2,
TESTED at g = 6), C3 (at g = 4, 13.5 below passes and 14.5 is refused), C4 (3.5 → 13), C5. Mutants red: the constant 12
(C1–C4), widening from g = 2 (C1–C4), the pick ignoring g (C2–C4), geometry still judged at 12 (C5).

**tc17's four re-run (18.1 s, `cert-traces/2026-10-03-tonelevel/tc19-v19-clamp/`): 16 of 16 levels PASS**, nothing nulled.
Lindell SBC's 6 dB level, clamp geometry under 12, is now **TESTED: GR 5.98 dB at L −13.16 dBFS RMS** (pick norm 0.6333,
its 1 dB point 14.1 below L, inside 18); its 2/4/5 unchanged at −18.11/−15.21/−13.66 (1.99/4.01/4.99). bx_opto, 7X-500 and
mpressor identical to §27 (their spacings were always inside 12).

**CL 1B, the L the rule now picks** (`--cert-tone-levels`, a dry read of the rule, nothing loaded; the checks wait for the
iLok on Sean's Mac): g 2 → −37.11 (clamp 12, 1 dB point 6.5 below); g 4 → −31.61 (clamp 14, 12.0 below); g 5 → −29.81
(clamp 16, 13.8 below); g 6 → −28.01 (clamp 18, 15.6 below) — all at the median, first try, pick norm 0.5000 (Threshold
−18.5 at ratio 6:1). Under the 12 dB clamp its 5 and 6 dB had no valid L (§27).

`--cert-tone-levels <profile>` is the new dry command: the per-level L, limit, pick and expectation without a load.

## 30. v1.8 notes: every deep null accounted for, one line per (level, reason); the notes shape behind one constant (3 Oct)

**The account.** The exporter files every deep null (4/5/6) in the curve under exactly one line `deep null <g> dB - <reason>:
positions <norms>` (details in parentheses, never commas), from the record's own words, never a guessed cause:
`not reached by -3.01 dBFS` (the derivation's `not_reached`); `past at the quietest level` (`below_range`); `no rising
straddle (gap or fall)` (a plain null with no other explanation); `hold test failed` with BOTH values (the record's
`quality.deepPointsNulled` now reads `i@t: first / hold-doubled second / delta`); and the all-null position, filed under
its own word at 1 dB — `not reached … (all-null position)` (bx_opto's bottom three: the knob does nothing there) or `past at
the quietest level (all-null position)`, with any deep point measured there shown as withheld. The tone check appends its
own two: `tone check failed (GR x against y expected at L z)` and `tone level untestable (no valid L in the measured range,
1→g spacing … clamp geometry)`, positions = those that carried the level before the null. Pins X28 (set equality: the
(level, norm) pairs on the lines == the deep nulls in the curve, each once), X29/X29b (one of every reason, named), X30
(the control: a dropped line is caught), X31 (the all-null word), Q8 (both hold values). Mutants red: gap nulls not listed,
hold failures filed as gaps, the all-null position unlisted, the lines never written, the hold test recording one value.
Live: CL 1B (9 nulls), 7X-500 (5), SBC (13), bx_opto (16), mpressor (7) — 50 of 50 accounted, every one `not reached` (no
hold failure in these five; the hold path is exercised by X29). Traces `cert-traces/2026-10-03-tonelevel/tc20-null-account/`.

**The shape.** Notes are built as lines (`noteLines`) and written through `profile::notesVar`; the driver appends through
`profile::notesAppend`; readers use `profile::notesText`. `kNotesAsList = false` keeps today's one `"; "`-joined string
(Sean's example still shows `""`); flipping that constant writes v1.8 §3's list of plain strings and nothing else changes.
Not switched: Kathy is asking Sean which shape his validator accepts.

**Item 4, report only — a deep point that breaks monotonic order.** The exporter does NOT refuse: it exports the profile with
`quality.monotonic_within_positions` / `monotonic_across_positions` false and the violation named in `quality.violations`
("point i: 5 dB not strictly above 4 dB"; "4 dB values rise n and fall m times across positions"); the batch row is still
`exported`, and the tone check still runs. The spec says the server "rejects non-monotonic points", so today that is an
export the server will reject, flagged by us first. It has not happened on any record we hold (every exported profile has
both flags true). Two ways to close it if Kathy wants: null the offending deep point (and account for it — a sixth reason
line, "breaks monotonic order") so the shallow profile stands, or refuse the export as `needs_review`. The first matches
v1.8's "accepted with nulls" spirit for deep points; a shallow (1/2/3) break should stay a refusal.

## 31. v1.9 (copied to docs/COMP_PROFILE_SPEC_v1_9.md, 3 Oct): notes as a list; out-of-order deep points nulled before export; the borrow-the-point rule

**pickClampDb against v1.9 §6.4 step 4:** "12 dB for an ask up to 3 dB, and then 12 + 2 × (g − 3): 14 dB at 4, 16 at 5,
18 at 6, interpolated for a fractional ask … the refusal names the depth that applied" — matches `pickClampDb` and the
refusal text exactly; no change, v1.9 cited in the code.

**Notes as a list (v1.9 §3, Sean: his validator accepts either).** `kNotesAsList = true`: `notes` is a list of plain strings,
one line each (the levels line split from "tone 997 Hz" so line 0 carries no "; "); the driver appends through `notesAppend`
in the same shape. Pin X32; mutant (back to one string) red. Live: the five profiles carry 11–14 lines.

**A deep point out of order is nulled before export (v1.9 §3 "nulled at load, not rejected"; Kathy's ruling: within its
position, or across positions at its level).** Within a position, a 4/5/6 point that does not rise above the point below
it is null. Across positions at a deep level, walking in the 1 dB direction, a point that does not continue from the last
kept point is null — the point that broke, deterministic (an LIS was tried first and chose by tie on Sean-shaped data; the
walk is the server's own within-position reading applied across). Each nulled point is on the sixth account line, `deep
null <g> dB - breaks monotonic order: positions <norm> (measured <value>)`; the account stays exact (X35) and the exported
monotonic flags come out true, since the server would null the same points. **A 1/2/3 point is never touched here** (X36):
unchanged — the derivation's `nonmonotonic` refusal is the shallow gate; a shallow in_at_gr order break that survives it
(never seen) is still exported with the quality flag false, as before. Pins X33–X36; mutants red: within not nulled,
across not nulled, a shallow break nulled too, the sixth line missing. Across the five real profiles: 0 breaks (flags
true/true before and after). v1.9 also nulls a point *above the sweep ceiling* at load; our derivation cannot produce one
(the sweep tops at −3.01), so there is no exporter rule for it.

**The borrow-the-point rule (v1.9 §6.3 step 1, "v1.8, clarified").** What the replica did before: `valuesAt(g)` read the
fractional value per position, so a position with a null deep bound had no 3.5 value, and the fill pass then interpolated
**the 3.5 value itself** across the norm axis from the neighbours — the wholesale re-read v1.9 forbids, discarding the
position's own 3 dB measurement. Now the fill borrows the missing POINT (the bound level) from the positions either side
and interpolates between the position's own measured lower point and the borrowed one; a whole-number ask is unchanged
(the point is the level); a SHALLOW null bound is never borrowed (the position cannot serve the ask). Pins P7b (the own-3
answer lands exactly on the position; the wholesale answer sits 0.75 dB away — Sean's figure, reproduced on the test
curve), P7c (the wholesale value picks a different setting), P7d (a shallow null bound is skipped); mutants red: the
wholesale re-read, a shallow bound borrowed. **Tone checks use whole-number g, so no result changes — confirmed live:**
tc17's four re-run under this build against the v1.9-clamp run (tc19): identical L and pass on all 16 levels, GR identical
to 0.00 dB. Traces `cert-traces/2026-10-03-tonelevel/tc21-v19-list-notes/`.

## 32. Before the follow-up build (3 Oct, afternoon): L anchored on the typical vocal; the across rule nulls the level; a shallow break refuses

**The tone-check L (item 1 of the earlier list, now built).** `profile::toneLevelRef (f) = -18.4 + f × (-6.2 + 18.4 - 3.01)` —
the server's own §6.4 step 1 for the spec's example track through the unit's `detector_f` (RMS unit −18.4; peak unit −9.21;
CL 1B at f 0.43: −14.45). The test L is the first VALID level (a pick that passes the clamp at g) among L_ref itself and then
the positions' `in_at_gr[g]` values by distance from L_ref, within the sweep's range. Per level the tone check records
`L_ref_dbfs`, `L_rms_dbfs` and `L_gap_db`; no `detector_f` → the level cannot be anchored and the rule says so (never RMS
assumed). Pins T7 (L_ref refused, the nearest valid candidate answers, NOT the median which is valid but farther — the
median-first control), T8/T8b/T8c, T9–T11 and C2/C5 re-derived; mutants red: median first, fixed −18, detector_f ignored.
**Live, tc17's four (18.1 s): every one of the 16 levels tested AT its L_ref (gap 0.00), all PASS** — 7X-500 f 0.13 → −17.21
(2.0/4.04/5.03/6.01); SBC f 0.00 → −18.40 (1.99/4.0/4.98/5.99); bx_opto f 0.63 → −12.61 (1.98/3.99/5.0/5.97); mpressor f 0.40 →
−14.72 (1.99/4.0/5.0/5.99). **CL 1B dry run: 2 dB at −14.45** (not −37), and 4/5/6 at −14.45 too, pick norms 0.20–0.27, the 1 dB
point 6.8/12.4/14.1/15.8 below — inside each clamp, first try. The resume marker is now `L_ref_dbfs`, so a tone check made
under the median rule is redone by the follow-up. Traces `cert-traces/2026-10-03-tonelevel/tc22-lref/`.

**CL 1B's deep nulls reconciled (item 2).** The record: 28 positions, 84 deep points, 75 numeric, `deepPointsCompared` 75,
`deepPointsNulled` [] (empty), `deep_point_error_db` 0.10. The 9 nulls are the three all-null positions at norm 0.0000 /
0.0667 / 0.1333 — `not_reached` at 1 dB (the knob at its quietest three positions never compresses by −3.01), so their 4/5/6
are null too — every one on the `not reached by -3.01 dBFS (all-null position)` line. **Hold-test nulls: zero.** So the
b6d2dc04 message ("deep points on 25 of 28 positions, nothing nulled") was right; my later "CL 1B has three hold-nulled deep
points" (this session, before the account existed) was wrong — it conflated the three all-null positions with hold failures.
The account is what settles it, and it was built after the wrong statement.

**Across-position deep break: the whole level (item 2 of the list).** The forward walk is gone: an early outlier made it null
every good point after it, and the server's repair is within-position only, so this is our rule — on an across-position
break at a deep level the WHOLE level is null, one line `deep null <g> dB - breaks monotonic order across positions at
<norms>` naming every position that carried it. Within-position stays point-level (the server's rule). Pins X34 (an early
outlier nulls the level, not the good points — a forward-walk mutant red), X35 (account exact, both line shapes); mutants
red: the walk, no across nulling.

**A shallow order break is a refusal (item 3).** A 1/2/3 `in_at_gr` order break that survived the derivation refuses the
export — `needs_review`, "shallow in_at_gr order break (the server rejects it): <violations>" — judged on the 1/2/3 levels
only (every deep break was nulled above, so a deep flag never refuses). M2/M3 re-pinned as refusals, X36; the flag-only
mutant red. Never seen on a real record (the derivation's `nonmonotonic` result catches these first).

Suite 3238 checks green.

## 33. v1.10 (Sean's next ask, 3 Oct): targets 1..12, the clamp to 30, the saturation note — folded into the follow-up build

**One list.** `sweep::kGrTargetMax = 12`; `kGrTargets` is generated 1..12 from it. Every rule keyed on the levels walks that
list or reads the constant: the straddle derivation and its words, the hold test (0.5 dB) and `deep_point_error_db` over the
survivors, within-position and whole-level across-position nulling, deep-only withholding, the null account, the tone
checks at each carried level, the reverse read, the fractional borrow, `inAtGr`'s top clamp. 1/2/3 stays the trust gate
(`kTrustTargets`, unchanged). **The grep for a second list** (every `6`, `"6"`, `1..6`, `{1..6}`, `{4,5,6}` in the four
sources), each one reported: `kGrTargets {1..6}` (the list itself → generated); `inAtGr` `jlimit(1, 6)` and `g >= 6.0`
(→ `kGrTargetMax`); `grAtLevel` `k <= 6` (→ `kGrTargets`); the borrow's `jlimit(1, 6)` / `gEff >= 6.0` (→ `kGrTargetMax`);
the notes line "deep points 4/5/6" (→ built from `kDeepFrom..kGrTargetMax`); six comments saying 4/5/6 or 1..6 (reworded).
Every other `6` is formatting precision (`juce::String (x, 6)`), the stepped-control bound (`steps <= 64`), the grid
(`k <= 64`) or a readout index — not a level. Mutants that re-introduce a 6 at the list, the reverse read and `inAtGr` are
red (Q7, X37, R1c).

**The clamp** is unchanged in form — 12 to 3, then 12 + 2 × (g − 3) — pinned at 7/9/12 → 20/24/30 (C1b; a cap at 18 red).
Wording to be checked against v1.10 when it lands.

**The saturation note.** Per deep level, the positions whose exported point is above −9.01 dBFS RMS (the ceiling −3.01 less
6) are listed: `deep <g> dB read in the top 6 dB of the sweep, where saturation also lowers level: positions <norms>`.
Information only — pinned that the points stay numeric (X39; mutants: the note nulling the point, the threshold at 3 dB).

**Live, re-derived from the traces (tc25; CL 1B from its refined traces as the import would):**

| product | positions | deepest level carried | positions carrying 7 / 8 / 9 / 10 / 11 / 12 | hold nulls | dpe (over) | nulls accounted | tone checks |
|---|---|---|---|---|---|---|---|
| Lindell 7X-500 | 22 | 12 | 19 / 19 / 18 / 17 / 17 / 16 | 0 | 0.10 (167) | 31 / 31 | 2, 4..12 all PASS at L_ref −17.21 |
| Lindell SBC | 16 | 12 | 9 / 8 / 7 / 6 / 5 / 4 (before the checks) | 0 | 0.10 (70) | 92 / 92 | 2, 4..7 PASS at −18.40; 8..12 PASS at −17.81 / −15.51 / −13.21 / −10.91 / −8.61 (gaps +0.6 … +9.8) |
| bx_opto | 27 | 12 | 20 / 20 / 19 / 18 / 18 / 17 | 0 | 0.10 (177) | 66 / 66 | all PASS at −12.61 |
| elysia mpressor | 20 | 12 | 16 / 16 / 15 / 14 / 14 / 13 | 0 | 0.10 (141) | 39 / 39 | all PASS at −14.72 |
| Tube-Tech CL 1B | 28 | 12 | 24 / 24 / 23 / 22 / 21 / 21 | 0 | 0.10 (210) | 42 / 42 | dry run: every level 2, 4..12 at L_ref −14.45, first try (1 dB point 6.8 … 25.0 below, inside each clamp) |

Every product carries 9 saturation lines (its deepest points are read near the top of the sweep, as expected). **Time:
42.1 s for the four = 10.5 s per product** (was 18 s / 4.5 s at 1..6: ten probe processes per product instead of four).

**Found by the live run and fixed before the build (T8d).** The first 1..12 run FAILED SBC's 9..12 at L_ref −18.4 — GR 7.77
at every one — and nulled all four levels. SBC's 9..12 dB points sit at −9…−3 dBFS (the top of the sweep); at −18.4 no
position gives 9 dB, so the pick at L_ref was merely the *nearest* position (unbracketed, the clamp passing) and the check
compared 9 against what that position gives at a level 9 dB below its 9 dB point. A test level must sit on the level's
measured curve: the L rule now counts a continuous pick only when bracketed (or exactly on a point); the next candidate
is a position's own point. SBC's 8..12 then PASS at their nearest points (gaps +0.6 … +9.8, recorded). The server's own
rung-4 pick at such a level is the nearest position — an approximation the tone check must not rehearse as a
measurement. Mutant (unbracketed accepted) red. `tonecheck-all-BEFORE-on-curve-rule.log` keeps the failing run.

## 34. The hold test on raw crossings; the export at 0.1 dB is what every check judges (3 Oct, evening)

**The bug (found by Kathy's read-only question).** `curveInAtGr` rounded every interpolated crossing to 0.1 dB when it wrote
the record, and the hold test compared those rounded values: every hold-doubling delta was a multiple of 0.1 (so
`deep_point_error_db` read 0.10 on all five products, a floor, not a measurement), and the 0.5 dB gate floated by ±0.05
(raw 0.54 could pass, raw 0.46 could fail). The probe reads to 0.0001 dB; the resolution was lost in the derivation.

**Fixed.** The record keeps the crossing unrounded; the hold test (shallow gate and deep nulling alike) compares raw values
and `point_error_db` / `deep_point_error_db` are the raw worsts, reported to 0.01. **The export rounds the raw peak crossing
to 0.1 dB, then converts** (so the exported shape and resolution are exactly what they were), and every order check (within,
across, the shallow refusal, deep nulling), the pick replica and the tone-check L run on those exported values — we judge
what Sean's server reads. The record's own informational monotonic flags compare at the export's 0.1 dB too, so sub-0.1
noise is never an order break anywhere.

**The grep for other rounding before a gate** (every `std::round` and `jlimit` in the derivation, and String precision fed
back into numbers): the crossing rounding at the straddle (THE bug, fixed); `passThroughOffsetDb` rounded to 0.01 — after
its gate (the gate compares `gmax − gmin > 0.02` raw); `tEquivalent` rounded to 0.1 — feeds `thresholdDbEquivalent` and the
display-offset report, which is informational (spec 7's bar is "a TEST … never the definition"), not a gate; the record's
output rounding of quality figures, reference gains, norms (1e-6), level dependence — all after their compares;
`levelKey(L)` = `String(L, 2)` keys the per-level maps — the levels are whole dB, so exact; `point_error_db` to 0.01 — after
the compare. Nothing else sits in front of a gate.

**Pins.** Q8a (a derived record carries crossings off the 0.1 grid: 23 of 23 in the fixture), Q8b (raw 0.46 passes and is
the figure; raw 0.54 fails — a rounding to 0.1 before the compare would pass it), Q8c (the shallow figure is the raw worst,
0.54 not 0.5), M1b (two neighbours 0.03 dB apart in reverse order raw export equal and are NOT an order break); X2/X14's
fixture tolerances re-stated for the export rule. Mutants red: the crossing rounded at the straddle (the old bug), the
delta rounded before the gate, the order check on the raw record.

**Live, the five re-derived (tc27 vs tc25):** every exported `in_at_gr` value identical (230 / 116 / 247 / 200 / 282), null
patterns identical, accounts exact, flags true, tone checks unchanged (g = 2 and 9/9 deep PASS each; CL 1B dry). The new
figures — `point_error_db` / `deep_point_error_db`: 7X-500 0.01 / 0.00, SBC 0.01 / 0.01, bx_opto **0.11** / 0.01, mpressor
0.01 / 0.01, CL 1B **0.03** / 0.01 — the raw worsts, as predicted in §33's measurement. No verdict changed. 43.7 s for four.

## 35. v2.1 (copied to docs/COMP_PROFILE_SPEC_v2_1.md): the clamp is the pick's own spacing + 3 dB past 3 dB (3 Oct, late)

`profile::pickAllowanceDb (g, pickSpacingDb)`: at 3 dB or under the flat 12 (unchanged); past 3 dB the PICKED position's own
measured spacing from its 1 dB point to g, plus 3 dB — the pick's g point and 1 dB point both interpolated with the pick's
own t, so an interior continuous pick agrees by construction (L − pickOne = spacing < spacing + 3). Same comparison; the
refusal names the allowance it used ("this pick's own 1->9.0 dB spacing 24.5 + 3 dB"). `12 + 2 × (g − 3)` is gone from the
sources and the tests; clamp geometry (a unit spaced wider than a fixed clamp) cannot occur and its reason class is gone
too. The on-curve rule for the tone-check L stays (it is what stopped SBC's 9 dB false failure and is independent of the
clamp). **The estimated branch is not built** — said in the code: the tone check never picks an estimated point (§6.3 never
extrapolates, the L rule only tests on the curve).

Pins A1 (a 3.06 dB/dB unit's interior picks at 9 and 12: allowed, spacing 24.48 / 33.66 → allowance 27.48 / 36.66, where
the old line's 24 / 30 refused both), A2 (an interior continuous pick at any g > 3 passes, 24 cases), A3 (a stepped detent
3.5 dB louder than its own 9 dB point refused naming the allowance; 2.5 above, or below, passes), A4 (flat 12 at g = 2 and 3,
the soft unit's refused pick at −14 unchanged), A5 (the fallback level's own spacing), A6 (the L rule records the allowance);
T10 re-stated (flat 12 at 2 dB still bites). Mutants red: the old line past 3 dB (5 pins), spacing + 3 applied at g ≤ 3
(7), margin 5 dB (4), spacing never computed (13).

**Live (tc29 vs tc27):** L moved on 0 of 40 levels, verdicts changed on 0; every allowance now the pick's own spacing + 3
(7X-500 6.7 … 15.7 at 4 … 12; SBC 12.2 … 31.3; bx_opto 8.1 … 19.3; mpressor 7.7 … 20.1). CL 1B dry run: every level at
L_ref −14.45, allowances 15.4 … 28.0 against 12.4 … 25.0 below — its 12 dB pick, which the v1.9 line (30) only just
allowed, now has 3 dB of margin by construction. 41.7 s for four.

## 36. Range gaps (4 Oct): the census, the fold, range_partial

**The census (`--cert-range-gaps <dir>`, read-only; `cert-traces/2026-10-04-sean-zip/range-gaps-*.txt`).** Two classes per
control — (a) an END prints a word (`range.endsNotNumeric`), (b) an END SAMPLE is missing (`displayAt` lacks 0.000 or
1.000, or the text is empty) — with the control's role from `roles::classify`:

| population | records | (a) word end | (b) missing end sample |
|---|---|---|---|
| our cert store | 103 | 267 controls, 58 products, **105 in a role** | 135 controls, 8 products, 20 in a role |
| Sean's 3 Oct zip | 132 | 317 controls, 70 products, **117 in a role** | 136 controls, 9 products, 20 in a role |
| B (server map data) | 74 | 42 controls, 26 products | 76 controls, 37 in dial-written roles |

B's figures are far below ours on both classes and the populations differ (B: 74 products with server map data; ours 103 /
132 with every sampled control). Class (b) in our data is dominated by TBTECH Cenozoix (98 controls whose text-at returned
nothing) and meters; the 20 in a role are the ones that matter. **A name-level diff against B's list needs B's list** — our
two lists are committed so it can be made the moment it is shared; without it "on one list but not the other" cannot be
answered. Our role-bearing word-end controls include the whole Melda ratio family ("Infinity"), every "Auto"/"auto"/"Dual"
release, CLA-76 / MC 77 Input "-Inf" (instantiate OUTSIDE the parsed range), SSLComp Attack ".1ms" (a parser miss: no bare
leading dot), DPR-402 Threshold "Out", CL 1B Threshold "Off".

**The fold (built).** `fixturerange::derive` takes the instantiate text and norm: every parsed sample folds into min/max,
`at_instantiate {norm, value}` is recorded, and `range_partial` names the gap — "an end prints a word", "an end sample is
missing", "the instantiate point lies outside the sampled ends" — so no consumer resolves display to norm across it by
interpolation. Without an instantiate text the rule is exactly what it was (R7d: the 1,783-control reproduction stands).
Pins R7 (CL 1B Gain: min 0, max 31, at_instantiate {0.33, 0.0}, both gaps named), R7b, R7c, R7d; mutants red (not folded;
never flagged). The re-sample (21 norms in the tone-check session) is the next section.

**The re-sample (built, §36 continued).** In the tone-check session every control of a loaded product whose range is partial
(`range_partial` or `endsNotNumeric`) is read by the probe at 21 evenly spaced norms (0, 0.05 … 1) plus its own
instantiate norm (`--text-at-norms`, one process per control); `profile::foldResample` folds every parsed sample into the
corrected range and keeps the words as named positions with their norms; the result is written to
`cert/controls/<identity>.controls.json` (identity, product, version, map_fp, per control `range_before`, `samples[]`,
`range_resampled {min, max, numeric_samples, of, named_positions[]}`) for Kathy to pass to Sean — nothing published. Pin R8
(CL 1B Gain's shape: 0.33 reads "0.0", the range includes it, "Off" a named position). Live on the four rehearsal units
(`cert-traces/2026-10-03-tonelevel/tc31-range-resample/`): Lindell SBC SC HPF 90..400 → **20.2..400** (Off at 0), Link
70..100 → **50..100**, Ratio 1.5..4 → **1.5..10** with Inf named at 0.95/1.0 (the old range had borrowed the middle "4:1"
as its top!); Lindell 7X-500 SC High Pass Filter 100..300 confirmed, OFF over the bottom half named. The session took
101 s for the four (was 42): about 15 s per re-sampled control on these units. CL 1B's own Gain re-sample waits for a Mac
with the iLok.

## 37. The false licence flag (4 Oct, H-Comp from Sean's run): judged only where the ladder accepts; what was seen is named

H-Comp (m)/(s) were refused "not licensed suspected … output at -84.00 is 36.8% the input's tone; -90.00 12.4%": the unit's
modelled noise floor (about −82 dBFS) swamps the −90..−78 tones; from −72 up tone_frac is 0.90 → 1.00. The default
reference is now judged only at levels at or above the quietest ladder rung whose 6 dB pair passes on the reference itself
(0.1 dB, the positions' own bar) — H-Comp's −90/−84 (1.3 dB), −84/−78 (3.2) and −78/−72 (4.9) rungs fail, −66/−60 (5.9)
passes, so the judgement starts at −66 and the floor readings are noted as "noise floor above the test level at −90, −84 dB
… not judged". With no rung passing anywhere, every level is judged (a dead unit still flags). **What was seen is named**
(`referenceSeen`, in the reason): "silent at every judged level", "non-finite output", "fixed-level non-tone bursts at
<dBFS> (inputs …)" — off-tone readings whose output sits at one level whatever the input: MDynamics at −44.8 dBFS on
inputs −56/−54/−44 — "intermittent dropouts" (isolated off-tone readings at scattered levels, clean between), else "not the
input's tone at …". Pins A6 (H-Comp's own numbers: no flag, floor noted, judged from −66), A7 (MDynamics c8: bursts named),
A8/A8b/A9; mutants red (judged everywhere; nothing named). H-Comp (m)/(s) are among the re-runs the projected outcome
(§40) lists: their refusal was recorded, so the batch skips them until `--retry-refused` or a new version.

## 38. The measured candidate rules (4 Oct; EjmapCandidateRules.h): linked pair, leader/follower, master over trims, Main over Aux

Rule 1 decides at plan time; these decide a multi-candidate record from what was MEASURED, after the sweeps, in both the
batch (`decideByMeasurement` beside Rule 1 in `composeCandidatesAndReport`) and the tone-check-only re-derive from traces.
The record then carries `pickedCandidate` + `ruleDecided {rule, pick, twin|trims, ruleText}` exactly like Rule 1, so the
export, the tone check and the row use the same single view; `notes` names the rule, the pick, the twin/trims (at
instantiate, in neutral) and the measurements.

- **linked_pair (item 1):** two candidates whose names differ only by a literal channel token — `""/" R"`, `L/R`,
  `Left/Right`, `1/2`, `A/B`, `L/M`–`R/S`, `M/S`, as suffix OR prefix (UnFairchild's "L Threshold / R Threshold" is a prefix
  pair: Kathy, veto if "suffix" was meant literally) — both certify; their 2 dB curves agree within 0.5 dB at every common
  position; and in the FIRST candidate's own sweep both output channels (the probe's per-hold `ch` levels) agree within
  0.5 dB at every reading above silence. The first is the amount; the twin and every link/mode control stay at instantiate.
- **leader_follower (item 10):** the same pair, the first certifies with its channels together, the second does not certify.
- **master_over_trims (item 11):** one candidate's name is every other's minus a channel or band suffix (L/R/Left/Right/A/B/
  L/M/R/S/M/S/Low/Mid/High/Lo/Hi or digits) and it certifies. A name match alone never decides.
- **main_over_aux (item 12):** the literal words "Stereo/Main" against "Aux", Main certifies.

**Sean's zip, re-derived from the traces:** all **10 channel pairs pass** linked_pair — AMEK, Abbey Road RS124 (s),
DPR-402 (s), Millennia TCL-2, VT-7, SPL IRON (channels 0.27 dB apart, inside 0.5), UnFairchild (prefix tokens, 0.05),
VBC FG-MU (0.02), Vertigo VSC-2, elysia alpha master — every curve pair identical to the digit, every first candidate's
channels within 0.27 dB. **PuigChild 670 (s)** → leader_follower (Left certified, channels 0.00 over 1330 readings, Right
flat). **Ozone 12 Vintage** → main_over_aux. **Kiive XTComp** (INPUT over Input Left/Right) and **DSM V3** (Threshold over
1/2/3) → master_over_trims. **Shadow Hills** (both) → no rule: four candidates (Optical/Discrete × 1/2) — and note the
premise "channels independent" is NOT what its traces show: in each of its four candidate sweeps both output channels agree
to 0.00 dB over 840 readings; it stays in review because two STAGES are not a channel pair, not because its channels part.
Lindell 354E / MBC (3 candidates, no master shape) stay.

**The tone check for any decided pick** writes in the server's order (engage, neutral including the twin, ratio, then the
amount as the sweep position — already the order `toneWrites` builds, pinned T1) and now requires BOTH output channels within
0.5 dB of g (per-channel GR = GR + (level − channel) from the test hold's `ch` levels; recorded as `gr_per_channel_db`),
which catches a twin write mirroring back onto the amount. Live on the four rehearsal units: every level's two channels
read identical to 0.01 dB (e.g. SBC g = 2 → 1.99 / 1.99), 40 of 40 PASS.

Pins P1–P6, M1–M3, X1/X1b; mutants red (pair by name alone, curves not compared, master need not certify, a certified second
counted as leader, Low/High as a channel pair).

## 39. A failed tone check is needs_review (4 Oct); Lindell 254E and VBC FG-Grey re-tested

`outcomeAfterExport` now reads the tone-check result: a check that fails or cannot read is `needs_review` "tone check failed:
<GR> vs <g>" (the quiet-check failure named when it is the cause); the profile file stays on disk with its `tone_check`
block but the row never says exported. Pins L19g/L19g2/L19g3; STRANGER_MAC_TEST.md §B re-stated.

**Re-test under the current pick.** Sean's checks ran at the fixed −18: 254E (f 0.83) GR unreadable, quiet check failed, at
norm 0.767; FG-Grey (f 0.76) GR 1.02 at norm 0.291. Dry run now: 254E L_ref −10.77, the g = 2 pick at L_ref is unbracketed
so the nearest on-curve point −11.01 (norm 1.0) is the test level; FG-Grey L_ref −11.42, pick norm 0.152, bracketed. **254E
is installed here at Sean's version, so it was run live: g = 2 PASSES — 1.98 dB at −11.01.** Its deep levels then showed a
new finding: 8 of 9 did not read at all because **the amount write did not land** — 254E's Threshold is declared continuous
but snaps to 1/15 steps (the capture says `write_unlanded getValue 0.866667` for the pick 0.8546), so any interpolated pick
between its real detents renders nothing; only the 9 dB pick (0.5346 ≈ 8/15) landed and passed (8.97). The sweep never saw
this because its 16 evenly spaced positions ARE the detents. The tone check now says so by name ("the amount write did not
land: norm 0.8546 snapped to 0.866667 … a control declared continuous that steps") instead of "quiet check FAILED / GR
unreadable". **Lead, not chased:** a control whose sweep writes consistently snap to a grid should be exported `stepped` with
that grid — its `positionLandedBy` / landing evidence is in the record; the fix is a derivation rule with its own pins.
FG-Grey is not installed here; its live re-test waits for Sean's follow-up run (expected to pass: the old 1.02 was read at
−18, 6.6 dB below where its 2 dB points sit).

## 40. Out-of-scope states, ON (4 Oct): multiband and surround

`outcomeForRecord` files, before the candidates rule: **surround** — Logic's `(N->N)` in the product name with N > 2 →
"surround: not profiled (N channels; the profile is a stereo contract)"; **multiband** — threshold candidates that are
band-numbered ("Band N", "(Band N") or carry Low AND Mid AND High by literal word → "multiband: profiling not built yet
(<bands>; N threshold candidates)". A licence row comes first (nothing measured), and a decided record (Rule 1, a measured
rule, a review pick) is never filed by its names. Neither state is needs_review; both are counted in the batch summary and
in `--cert-states <dir>` (a read-only census of what every record would be filed as now). **Sean's zip:** 15 multiband —
C4 (m)/(s), C6 (m)/(s), C6-SideChain (m)/(s), Drawmer 1973, Lindell 354E, Lindell MBC, LinMB (m)/(s), MO-TT, Ozone 12
Dynamics, Pro-MB, SSL G3 MultiBusComp — plus the three Melda multibands (MDynamicsMB, MDynamicsMBLarge, MTurboCompMB) that
are licence rows first (every candidate silent), which makes Kathy's 18; 2 surround (the Spherix units). Not multiband by
the literal rule and still in review: kHs Dynamics and MaxxVolume (Low/High without Mid — level stages, not bands) and
DynOne3 (C LF / MF / HMF… names, no Band number and no literal Low/Mid/High). Pins L19h–L19k3; mutants red (never filed;
Low + High enough; two channels as surround).

## 41. The one-time review pick (4 Oct): cert/review_picks.json and the review sheet

`cert/review_picks.json` is a list of `{"product", "candidate", "by", "date", "note"}`. `loop::applyReviewPick` runs on a
candidates record before anything reads it — in the tone-check mode (then the re-derive carries the pick) and in the batch's
finish pass: an entry naming a candidate whose sweep certified writes `pickedCandidate` + `ruleDecided {rule: review_pick,
by, date, pick, trims, ruleText "picked by KD on 2026-10-04 from the candidates' 2 dB curves…"}`, and the record goes on
like a Rule-1 pick (re-derived, exported, tone-checked, notes naming the pick and who made it). **Nothing is ever picked
without an entry**; an entry naming an uncertified candidate, an unknown candidate, or lacking initials/date picks nothing
and the log says why. Pins L19m–L19p; mutants red (a missing entry picking the first certified candidate; an uncertified
pick accepted). **The review sheet** — `--cert-review-sheet <cert dir>`, and written by the tone-check mode as
`cert/review_sheet.txt` after the rules have run — lists every remaining needs_review record's candidates with their
verdicts and 2 dB curves (norm:dBFS RMS), so picks are made from data. Sean's zip before the rules: 28 products
(`cert-traces/2026-10-04-sean-zip/review-sheet-before-rules.txt`).

## 42. Sean's zip, read-only answers (item 5): API-2500's missing repeat; the 12 flat rows

**API-2500 (m)/(s): the repeat WAS taken and the batch lost it.** The traces hold 37 `r2.e6.pos*` captures: the engage
search found control 6, the full sweep ran under the prefix `e6.`, the hold-doubled repeat was stored under `e6.` — and the
single-candidate composition asked `repeatRuns` for `""` (or `"q."`), found nothing and wrote `repeats 1, no repeat`, so the
export refused on `point_error_db`. Fixed: the repeat is keyed on `lastSweepPrefix`. The re-derive always paired it
(`resolveTraceRun`), so both API-2500 records re-derive to repeats 2, point_error 0.03 / deep 0.01 over 54 / 115 points and
export in the projection. Any engage-search product in Sean's batch had the same loss; the follow-up's re-derive repairs
every one from the traces (no re-run).

**The 12 flat rows.** Not licence anywhere: every one of them outputs clean tone (the Waves ones share WaveShell licences
with CLA-76 / C1 comp-sc / H-Comp, which measured). Two classes:
- *Pass-through at every reading* — SSLComp (m)/(s) (IN = In; Thresh swept −15…+15; 0.00 GR), RCompressor (m)/(s)
  (Threshold −60…0, ratio raised to 4.34; 0.00), dbx-160 (m) (+0.31 constant), NEOLD U17 (Input 0…14.4; −0.01). Every
  threshold write landed (`pump` / `instack`). A working compressor cannot read 0.00 GR with its threshold at −60 and 0 dBFS
  in: the AU is not processing. Not engage (no engage control; U17's Power = On) — **a host-side lead**: four classic
  Waves AUs pass audio untouched in the probe on BOTH Macs (our store says the same for RCompressor, SSLComp, VComp, C1
  comp) while their siblings measure; needs a hand test in Logic before any rule.
- *Something, but the same at every position* — **C1 comp (m)/(s)**: compared with C1 comp-sc / comp-gate from the same
  family and the same run: identical preconditions (Ratio 4.13:1, Makeup 0), identical Threshold texts and landing, yet
  comp-sc/gate read up to 31 dB GR and comp reads −0.0 at every level. No engage control (the search tried [0] Low/Peak
  Ref, a detector mode). Same on our Mac. Lead, same shape as above. **VComp (m)/(s)**: Input swept, engage [4] tried,
  0.0 GR — same group. **NEOLD U2A**: Peak Reduction 0…26.7, 0.01 GR — known since 2 Oct, neutral set applied; a
  precondition we have not found. **SSL Fusion HF Compressor**: Threshold 10…4.7 with 0.13–0.75 dB GR — an HF-only
  compressor: the 997 Hz tone sits below its band, so flat is band evidence (neutral/signal), not a defect.

## 43. The projection for Sean's zip (item 16) — `--derive-only`, nothing loaded

`--cert-tonecheck-all --derive-only` re-derives every exported and every undecided-candidate record from the traces, applies
the measured rules and the review picks, re-exports, and re-files every other row under the current rules; it loads nothing
(no probe, no detector, no tone check). 7.5 s for the whole zip. Found and fixed on the way: **a crash** on the first
linked-pair export — `plan = plan.forCandidate (c)` with `c` referencing the vector the assignment destroyed (three sites;
Rule 1's single-candidate records never tripped it).

| Sean's run (3 Oct) | → under 1–15 | count | names |
|---|---|---|---|
| exported 43 | would export (tone check in the follow-up) | 43 | as before |
| needs_review 65 | **would export** — pair/master pick: detector + tone check in the follow-up | 13 | AMEK, RS124 (s), DPR-402 (s), TCL-2, VT-7, SPL IRON, VBC FG-MU, VSC-2, alpha master (linked pairs); PuigChild 670 (s) (leader); Ozone 12 Vintage (Main); XTComp, DSM V3 (master) |
| | would export — repeat repaired from the traces | 2 | API-2500 (m)/(s) |
| | multiband: profiling not built yet | 15 | §40 |
| | needs_licence "licence suspected" | 8 | 7 Melda + Pro-C 3 |
| | surround: not profiled | 2 | Spherix 10->10, 12->12 |
| | **still in review** | 25 | 11 with candidates and no rule (MaxxVolume ×2, Auto-Tune Vocal Comp, DynOne3, Shadow Hills ×2, VBC Rack, dbx-160 (s), kHs Dynamics, OTT, MAGNUM-K) — the review sheet; 12 flat (§42); Purple Audio MC 77 (nonmonotonic sweep); UnFairchild (linked pair decided, export refused: 5 curve points) |
| refused 18 | re-run needed | 18 | H-Comp (m)/(s), Low Control, MModernCompressor, MTurboComp (the licence false flag, §37: `--retry-refused` after the follow-up build); 13 "0 controls hold the threshold role" (Mike-E, MV2, Vac Attack, V76U73, OneKnob ×4, RVox ×2, Rubber Band, bx_opto Pedal) — roles, not a re-run |
| held 45, recorded 6, quarantined 2, needs_licence 2 | unchanged | 55 | |

So the follow-up on Sean's Mac: 58 exports to tone-check (13 of them needing one detector load each), review 65 → 25, and
the five refused-for-licence re-run with `--retry-refused`. `cert-traces/2026-10-04-sean-zip/projection-*`,
`review-sheet-after-rules.txt` (the 11).

## 44. Input-drive and one-knob compressors (4 Oct): the amount control is swept like a threshold

**Read-only first — the 13 "no threshold role" refusals in Sean's zip** (none has a sweep in the traces: only the defaults
list-params / text-at captures, so every one needs a re-run):

| product | the amount control | range / default | note |
|---|---|---|---|
| RVox (m)/(s) | [0] Compression | −36..0 dB, default 0.0 | the OneKnob-style amount; Gate at −Inf, Gain 0 stay |
| OneKnob Pressure (m)/(s) | [1] Pressure | 0..10, default 0.0 | Input (Unity/Boost/Pad) is a 3-step word control, not swept |
| OneKnob Pumper (m)/(s) | — | Pump 0..10, Rate 1/4 | **refused by name**: a rhythmic ducker, not a level-dependent compressor |
| Empirical Labs Mike-E Comp | [3] Drive | 0..10, default 5.0 | Ratio Bypass / 4:1 / NUKE is at 4:1; Preamp Gain (CLEAN / 8 / 18 dB) is not the amount |
| NEOLD V76U73 | [3] Gain | 43..76 dB, default 58 | the tube input gain; **Mode instantiates at Bypass** — the engage search must find Mode → Compress or the sweep reads flat |
| Rubber Band Compressor V2 | [5] Tension | 1..3, default 2.0 | by the lexicon ("tension" is an amount term); Snap / Bias / Crunch are not — to verify on the first sweep |
| Mixland Vac Attack | [6] Left Reduction + [13] Right Reduction | 0..10, default 0.0 | two amount controls ending in a channel token → candidates, the pair rule decides after the sweeps; **Power instantiates Off** |
| bx_opto Pedal | [2] Density | 0..100, default 0 | Speed stays; Power On |
| MV2 (m)/(s) | [1] High Level | −48..0 dB, default 0.0 | the downward (compression) stage; Low Level (upward, 0..48) stays at 0 |

**Built.** In `planFromFixture`, when no control holds the threshold role, the lexicon at plan time (EjmapRoles.h's pinned
classification untouched) finds the amount control — the roles' amount terms, plus "Pressure", plus an exact "Gain" in dB on
a product with no input role, plus "High Level" in dB with a non-positive range; stepped and word-valued controls excluded;
"Pump" refuses by name. One → the amount, flagged `amount_as_threshold`, swept with the quiet reference; two ending in a
channel token → candidates. The sweep records the flag (`roleFlag`) and the export reads it: `topology: input_drive` with
`level_coupling` (the gain below threshold per position, from the same quiet reference), exactly as input_as_threshold.
Pins A1 (RVox), A2 (Mike-E), A3 (Pumper refused), A4 (Vac Attack pair), X12b (export); mutants red. `--cert-plan <record>`
is the dry command. **What the zip would export without re-measuring: nothing from these 13** — they were refused before
any sweep, so all 11 that now plan need a re-run (`--retry-refused` after the follow-up build; the two Pumpers stay refused,
with the new reason).

## 45. Stepped by evidence (4 Oct, Lindell 254E)

A control declared continuous whose writes land only on N values is stepped with those N detents. **Evidence, not
declaration:** before any level, the tone check writes the amount control at 41 norms (k/40) in one probe process and reads
where each write landed (`at … getValue`); `profile::detentsFromLanding` says stepped when at least one write landed
somewhere other than where it was written (beyond 1e-4) and every landed value sits on one uniform grid k/(N−1), 2 ≤ N ≤ 64.
On-grid writes alone are not evidence — 254E's own 16-position sweep sat exactly on its 1/15 detents and never showed it
(R9b). The evidence goes on the record (`amountLanding {control, detents, samples[], note}`), the export marks `amount.stepped:
true` with `stepped_by_evidence` when the swept positions are exactly those detents (else `stepped_by_evidence_unresolved`:
continuous, "a re-sweep on the detents would make it stepped"), and the levels then pick detents with the reverse-read
expectation. **Live on 254E here:** 16 detents (35 of 41 writes moved, e.g. 0.5346 → 0.5333, 0.8546 → 0.8667), profile
re-exported stepped, **g = 2 and all nine deep levels PASS** (2.14 vs 2.1 … 11.68 vs 11.7) where before eight levels could
not land. Pins R9–R9e, X12c/X12d; mutants red (on-grid writes as evidence; a loose grid fit; the export ignoring the
evidence). Traces `cert-traces/2026-10-04-sean-zip/254e-stepped-by-evidence/`. Cost: one probe process per product (~15 s)
when the profile is not already stepped.
