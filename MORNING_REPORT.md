# Morning report — 8 October 2026, overnight run

**Read this first: the Mac hibernated for six hours.** `pmset -g log` records
`IOPMrootDomain:hibernate user wake` at 07:10. Everything in flight froze at about 00:55 and resumed when it woke.
A guard run that should have taken four minutes shows 6 h 19 m of wall clock, and a `cmake --build` that takes three
minutes shows the same. That is why the queue stops where it does: the working night was about three hours, not nine.
I have not guessed at anything to fill the gap.

---

## What is done, committed, and green

| # | Item | Commit | Guard |
|---|------|--------|-------|
| 1 | Full gate finished and diagnosed | `81d5be9` | 60 of 61; see below |
| — | 06c's five items + the 06d item-1 teardown-race fix | `ad7f131` | `net_quiet_guard` GREEN, proven both directions |
| — | Test 5: the staged proposal (never printed, kept on the wire, applied on a yes) | `397b380` | `proposal_guard` GREEN (16), `history_guard` GREEN |
| 8 | `place_ship.sh`'s identical-UUID blind spot | `aae53d6` | proven both directions on a fixture tree |
| 2 | **Level — item 6/10 FINAL** | `a8defea` | `loudness_loop_guard` GREEN, including your acceptance case |
| 3 | Apply button on the proposal offer card | `5ed4c17` | `ui_guard` GREEN, four new legs |
| — | Guards link with a 512 MB main-thread stack | `0d837ff` | see "the ui_guard crash" below |

### Item 2 is the big one, and your own figures are its acceptance
- **The target is integrated, not the loudest 3 s.** That single change is the 4.5 dB you measured: the loop used to
  make the loudest three seconds equal the target, so on your bus it proposed +3.4 where your ear said +8.
- **The peak-headroom cap is gone**, with the −6.0 floor that existed only to bound it. On your bus
  `max(−6, ceiling + 3 − truePeak)` computed −0.1 + 3.0 − 2.9 = **0.0** and turned a +4.4 dB Level into zero.
  In its place the opening gain is a closed-loop write, `Level + (target − output integrated)`, which is immune to
  the pre-chain gain, the slot gains and the bus trim alike because it measures past all of them.
- **The GR model is measurement-first**: the measured typical reduction over the hits; else the EchoJay Limiter's own
  GR; else `0.25 × excess` **clamped at 3 dB**. `grCapDb` commercial **6 → 10**, pushed 8 → 12. Every window logs
  estimate vs measured, so your next calibration point lands for free.
- **Listen always resolves** within 20 s of playback, with one of four named reasons, and a leaked knob gesture can
  no longer block a measurement for the session.
- **Acceptance leg Z, green:** input integrated −14.5, the chain 0.7 dB down so the output at Level 0 is −15.2 as
  yours is, target −8 → the Level lands **within 1 dB of +8** and an *independent* meter reads the output
  **within 1 LU of −8**.
- Thirty-three other assertions went red first and every one was the ruling rather than a bug. The handoff records
  each class: legs about the window now arm with no reading; K3 and L1 are **inverted** to assert the cap and the
  floor are *not* applied; K1/K2/O2 are re-aimed at the 10 dB cap.

### The 23:40 full gate: 60 of 61
Stages 1–3 clean (archives, 51 guards, 4 bundles, 0 errors each), `link_state_guard` GREEN both sides.
**`dialinfo_keep_guard` passed — the teardown race is closed by `ad7f131`,** which is what queue item 1 asked me to
confirm. The one red was `lease_id_guard`, which then passed alone (210 s) and passed again with another heavy guard
in parallel (214 s): one red in three runs, under the gate's own `ctest -j 2`. Rather than shrug at an intermittent I
changed the leg to check its precondition **at** the write instead of five seconds earlier, print `borrowActive` and
the uid, and exit 2 as a RUNNER REFUSED if the borrow lapsed — so the next occurrence diagnoses itself. I did not
reproduce the red, and two green runs prove only that it is not deterministically broken.

---

## What is NOT done

| # | Item | State |
|---|------|-------|
| 4 | Item 12: substitute ceiling from `substitute.settings_structured` | Not started |
| 5 | Heard-counter reset on rack change | Not started |
| 6 | "Don't suggest this plugin again" on the substitution card | Not started (the read-only work is done: the sentence is composed in `composeBuildBubble`, and `BuildBubble` needs an `excludeNames` field carrying `sb.from` for a licence substitution) |
| 7 | The editor-view half of the 18:32 ruling | Not started |
| 9 | EQ edit within ⅓ octave of an existing band | Not started |

**The ui_guard crash, and the correction I owe my own first account of it.** After item 2 went in, `ui_guard`
started segfaulting — and it had passed the 23:40 gate, so something that night caused it. My first reading was that
my own new leg did it, and I reverted the leg and the Apply chip with it. That was wrong, and the crash report says
so in three frames: `start -> main -> ___chkstk_darwin`, SIGSEGV with `KERN_PROTECTION_FAILURE` at a stack address.
That is a **stack overflow**, not a product fault. `ui_guard` holds several `EchoJayProcessor`s as locals in one
frame, and item 2 added two clocks and a `std::function` to `LoudnessLoop`, which is a processor member — the frame
had been just inside the 8 MB default and went just outside it.

I first moved the work to a pthread with room, the pattern `loudness_loop_guard` already uses for its sanitized
builds. That fixes the overflow and **breaks four GUI legs**, because a JUCE GUI guard's message thread has to be
the thread it was initialised on. So the stack itself is bigger at link time (`-Wl,-stack_size`, applied to every
guard) and every assertion stays exactly where it was. `ui_guard` is green, the Apply chip is restored with its four
legs, and item 3 is done.

This is worth knowing for its own sake: it is a class of failure that looks like a product crash, fires on a change
with nothing to do with the guard, and would have read as "the loop broke the UI" to anyone who did not pull the
backtrace.

---

## The gate and the build

**`ship_2026-10-08a` is placed, labelled UNGATED, and NOT installed.** 234 MB, four bundles plus `UNGATED.txt`
which carries the whole record. UUIDs, arm64 first:

| bundle | arm64 | x86_64 |
|---|---|---|
| EchoJay V2.component | `4E5A7D82-CB06-3AA6-80A2-E8C77A2953ED` | `D2745F25-7341-397E-8CCA-EC5C9736B284` |
| EchoJay V2.vst3 | `3FC919E2-9027-313A-BA8E-CA9862D37DC7` | `2AE1D6BE-222B-300E-A493-09E3FA8F8EF8` |
| EchoJay Link.component | `2EBEB279-384B-32EC-8F46-1FF7EF996492` | `E097175A-4D99-367A-ABED-3AC0A7749A75` |
| EchoJay Link.vst3 | `732A699A-239A-3631-91E1-C7BB5667EAB6` | `F19DA546-4FEF-3495-8F72-49577ABF0C91` |

The Link is byte-identical to 06b/06c, which is correct — its archive compiles none of the files these rounds
touched. **`place_ship.sh` said that itself this time instead of refusing**, which is the item-8 fix working on a
real round: 06c had to be placed by hand for exactly this.

**Why UNGATED:** the full gate has not completed on this tree. The 23:40 gate predates item 2. I started a fresh
full gate at **07:44**; it takes about two hours, so expect it around 09:45. Everything that covers a changed
source is already green — `loudness_loop_guard` (with your acceptance case), `ui_guard` (with the Apply legs),
`proposal_guard`, `history_guard`, `net_quiet_guard`, `level_slot_guard`, `level_loop_guard`, `limiter_wall_guard`,
`pregain_readback`, `track_level_guard` — and both archives and all four bundles compile with zero errors.

**To install when you want it:**
```
cp -R "ship_2026-10-08a/EchoJay V2.component"   ~/Library/Audio/Plug-Ins/Components/
cp -R "ship_2026-10-08a/EchoJay V2.vst3"        ~/Library/Audio/Plug-Ins/VST3/
cp -R "ship_2026-10-08a/EchoJay Link.component" ~/Library/Audio/Plug-Ins/Components/
cp -R "ship_2026-10-08a/EchoJay Link.vst3"      ~/Library/Audio/Plug-Ins/VST3/
killall -9 AUHostingService 2>/dev/null; true
```
The `killall` matters — a stale AU hosting service has served an old binary here before. Verify by the V2 arm64
UUID above, not by the file dates.

## Your Logic test steps, when a build is placed

These are the four things from last night that changed under the hood, in the order worth testing.

1. **The mix bus is no longer quiet.** Build a chain on the mix bus with a `-8 commercial` target. The Level should
   open to roughly `target − (the output integrated you can read on the V2 meters at Level 0)` — on the chain you
   tested that is about **+7 to +8 dB**, not 0. The log line to look for is
   `EJLoudness: opening gain (closed loop): output integrated … -> …`, and it now says
   `No peak-headroom cap (7 Oct ruling)` instead of `opening gain capped:`.
2. **Listen always answers.** Arm a loop and *do not* play anything. Within about 20 seconds you should get
   "No signal is reaching EchoJay — the chain output reads …" with the figure, and a Listen pill. Then play the
   chorus and tap Listen: the proposal should name an **integrated** figure, not "loudest 3 s".
3. **Tap Listen twice on purpose.** The second tap used to say "Already listening" and then hang. It should now
   re-show the open proposal card instead.
4. **Quit with the plugin just opened.** Open EchoJay, then quit the host within a few seconds — the teardown race
   that turned 06c's gate red is the one this closes. The log should carry
   `network quiet in N ms` (or, if something was genuinely stuck, `NETWORK NOT QUIET after …` naming it).

5. **The Apply button.** This one needs B's server change deployed to see end-to-end, but when a reply offers a
   rack change and carries the staged ops, the card should show an **Apply** chip; tapping it dials the ops without
   another model call, and the chip disappears once applied. Typing "yes do it" does the same thing through the
   same code path.

One thing I still need from you, carried over: whether the **Integrated meter was reset** before your 20:59
reading, and what the **rack/output trim** was set to. Neither changes the ruling; both pin the last 0.4 dB.
