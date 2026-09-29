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

