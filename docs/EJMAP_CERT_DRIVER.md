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

So a licence problem can present as an empty instance, a hang behind an
activation window, or an exit. The driver cannot tell these from real defects by
the probe's output alone. It therefore needs an independent "is this licensed
here" signal, or it must treat every such shape from a known licence-bound
vendor as `unlicensed_on_host` and say so in the run report.

An activation window is a UI event on the user's desktop. An unattended run must
count it and name it in the report, not just time it out silently.

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

## 6. The fixture unit rule is EJ Map's

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
