# EJ Map certification driver: design requirements

Branch `feat/ejmap-cert`. The driver is step 5 of the certification plan and is
not written yet. These are requirements established by measurement before it
exists, recorded where whoever writes it will find them.

The split is decision D2 (28 Sep): the signed probe (`tools/au_instantiate_probe`)
MEASURES, and EJ Map DERIVES and ORCHESTRATES. The test for where a piece of logic
goes is whether changing it needs re-measuring or only re-computing. A formula
change must never cost a rebuild and a re-sign.

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

## 4. The fixture unit rule is EJ Map's

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
