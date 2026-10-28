# `tools/mapfps_test`: 26 red, 21 of them nobody's work today, 5 of them mine (28 Oct 2026)

Round 35 added `mapfps_test` to its touched set, because A0's own condition for the Match carve was
*"the Match rows of `tools/mapfps_test` and `ui_guard` stay green as they are"*. **Those rows pass**
(`mr PIN1` through `PIN27`). What is red is 26 other assertions, and this is the attribution, because
a red nobody has attributed is a red that will be blamed on the next change that runs into it.

**A correction to my own first reading.** I wrote the attribution below after 20 of the 26 had
printed and said "none is mine". Six more came in after it, and **five of the 26 are mine** — three
pins whose property I changed and did not re-pin, because this guard was in nobody's touched set when
I changed them. They are fixed in this commit, not excluded:

| pin | what moved | now pinned on |
|---|---|---|
| `rs PIN15` ×2 | A0 carved `MatchPanel::keyPressed` out of `PluginEditor.cpp` into `EJMatchTab.cpp`, then A0b made Escape ask `host->goBackToCompare()` instead of naming `setRefSubTab` | the **chain**, in both files: the page asks the host, and the host answers with the sub-tab move. Two controls, one per half — strictly stronger than what it pinned before |
| `ej PIN3` ×2 | the Settings toggles are placed **from one list** since 26 Oct (`chainSuggestionToggles()`), and a hidden one takes no row, so `echoJayOnlyToggle.setBounds(...)` and `y += 2 * (fh + 8)` are both gone | the property: one list on both sides, full-width rows, the hidden ones excluded in layout AND paint. Controls for the literal coming back and for a count taken from the list's size rather than what is shown — the same defect in a new spelling |
| `ap PIN1` | his 09:52 ruling of 20 Oct ("one press, one card") turned the Apply press into read-the-index, check-the-identity, then apply | the whole road, in order, with a control that reports the old one-line form — which would now apply a press that had been re-aimed onto another card |

The remaining 21 are below, and they are not this session's.

## The 20, by cause

**Eleven read files this working tree has not touched.** `git diff --stat HEAD --` prints nothing for
any of them, so the pin and the code it reads are both exactly as committed:

| pin | reads |
|---|---|
| `6cS PIN C: ChainHost.cpp takes no read of its own` (found 1) | `Source/ChainHost.cpp` |
| `6cF PIN C: the adapter fills it and passes it index-parallel` | `Source/ChainHost.cpp` |
| `na PIN A: the live name is read only inside the tagged branch` | `Source/ChainHost.cpp` |
| `fd PIN3: and clears the in-flight marker it set` | `ChainHost::storeFallbackMaps` |
| `of PIN D premise: op.name has exactly FOUR writers` | `Source/ChainHost.cpp` |
| `of PIN D premise: a created identity has exactly TWO authors` | `Source/ChainHost.cpp` |
| `buildChatRequestBody decides through trimChatHistory` | `Source/EchoJayAPI.cpp` |
| `tr PIN1: the trigger is the trim's three drop counts` | `Source/EchoJayAPI.h` |
| `refusal PIN7: the client holds no copy of the server's sentences` | `Source/EJRefusalLine.h` |
| `ml PIN5: the two arms that bypass completeLoad record for themselves` | `Source/EchoJayParamApply.h` |
| `sv PIN6: the refusal branch is the verdict, unmodified` | `Source/EchoJayParamApply.h` |

These are **source-text pins that have drifted**: the code they assert about moved after the pin was
written, in changes that are not mine and mostly not this session's. Each one is a real question
("does ChainHost still take a read of its own?") and each needs its own answer; what none of them is,
is evidence about the Match re-point.

**Two are unfalsifiable here.** `ef PIN5` reads `HANDOVER/commit-features-block.txt`:

```
std::ifstream fc ("HANDOVER/commit-features-block.txt");
check (msg.isNotEmpty(), "ef PIN5: the commit message is present to cite from");
```

That path exists in **no** worktree of this repo (`git worktree list` is 10 trees; none has it) and
is untracked. So the check reads an empty string and fails, and has failed since it was written
anywhere but the machine it was written on. A test whose subject is a file the repo does not carry
cannot go green, which is the "proven both directions" rule failing in the unpassable direction.

**Seven depend on this Mac's AU catalog**: `wa PIN3`/`wa PIN5` turn on *"Abbey Road TG ... which is
not installed"* and *"68 of the 69 curated names get an answer"*, and `wr PIN1`/`PIN2`/`PIN4` on the
AU manufacturer codes the registry hands back. The installed set here is not the set they were
written against — and a full AU scan on this machine is itself off limits (iLok/PACE prompts and
crashes), so these cannot be made true by rescanning.

## What round 35b does with it

`mapfps_test` is **excluded from the round's verdict, in writing, here** — not skipped silently and
not deleted. The round's subject is carried by the five guards that are green against the same
archive (`ui_guard`, `chains_view_guard`, `wet_rebuild_guard`, `settings_toggles_guard`,
`slotwet_knobpath`, plus `chat_store_guard`) and by the full rig pass.

## What it needs, as a job of its own

1. **The eleven drifted pins**: answer each one — is the pin stale, or has the product lost the
   property the pin was protecting? That second case is the one that matters, and it cannot be
   answered by reading the pin alone.
2. **`ef PIN5`**: either commit the citation table it reads, or make the pin read something the repo
   actually carries. As it stands it is a test that can only fail.
3. **The seven catalog pins**: pin them against a FIXTURE catalog rather than the machine's, which is
   the only way they can mean anything on two Macs.

Raised rather than fixed today because the subject is other sessions' code and the day's order is
Sean's list; it is the kind of debt that silently costs a round whenever someone adds this guard to a
touched set, which is exactly what happened at 10:40 today.
