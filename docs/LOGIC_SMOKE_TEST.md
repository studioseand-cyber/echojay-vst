# Logic smoke test — about 10 minutes, after the install

**Drawn from `docs/AGENT_SCENARIO_MATRIX.md`, not from memory.** Everything the runner can judge has
already been judged; this list is only the things it genuinely cannot be: Logic's own AU hosting, its editor
teardown, a licence-bound plugin, and whether something reads and sounds right to you.

Each step says what PASS looks like. Anything else, stop and tell me the step number - I have the matrix
scenario and its evidence beside it.

**Setup (1 min).** Open the project with the Mix Bus V2 and the Kick's Link. Agent mode **Ask**. Play
something with transients through it.

---

### 1. The AU road's window close (2 min) — the rig cannot do this at all
*The AU wrapper will not let a test host destroy the plugin's editor; only Logic does. Matrix F1/F3.*

1. Mix Bus, Agent **Off**. Ask **"how does this kick sound"**.
2. **Close the plugin window while it is still answering.** Count to three. Reopen it.
   - **PASS:** either the answer is there, or exactly ONE row saying **"You closed the window before the
     answer came back."** with a **Retry** pill. Not two rows. Not a bare sentence with no pill.
3. Press **Retry**.
   - **PASS:** it answers the question you actually asked, and the new answer is at the BOTTOM of the chat,
     under your question - not above the previous exchange.

### 2. A run while Logic recreates the editor (1 min) — AUHostingService's own behaviour
*Logic rebuilds an AU editor on a window switch; nothing else does. Matrix F3.*

1. Agent **Ask**, Mix Bus: **"add an EchoJay Compressor with threshold at -20 dB"**. Approve it.
2. While it is running, click onto the Kick's Link window and back.
   - **PASS:** one run row, its steps in order, no duplicated lines, and the closing appears once.

### 3. MTransient — the licence-bound case the rig refuses to load (2 min)
*MTransient carries PACE, so nothing of mine may load it (your 2 Oct rule). Only you can check it. Matrix A5.*

1. Kick's Link, Agent **Ask**: **"add MTransient with attack at +6 dB"**.
   - **PASS (until B rebuilds the map):** **"Added MTransient. Settings not confirmed."** - and NO figure
     anywhere in the run. The plugin is added.
   - **FAIL:** any sentence that names a figure for the attack, or "Not applied: add MTransient".
2. Open MTransient's own window and look at Attack.
   - **Expected today:** still 0.00 dB. Its map names controls that do not exist on this plugin
     ("Basic - Attack"); that is B's to fix and it is written up in FOR_B §9.

### 4. The third-party read-back, in the host (2 min) — matrix A3/A4
1. Mix Bus: **"add Renaissance Axx with threshold at -20 dB"**.
   - **PASS:** the run says it read the threshold back and names **-20**; the closing does NOT say "settings
     not confirmed".
2. Open Renaissance Axx and look at Thresh.
   - **PASS:** -20.0, and the slot's **Suggested Settings** says the same number.
3. Same on the **Kick's Link**.
   - **PASS:** same answer, and the Link's Suggested Settings is populated (not "No suggested settings for
     this plugin").

### 5. The rack strip and the GR tile (1 min) — matrix G1/E2/E5
1. With the Kick's Link selected in the rack view, ask the agent to **add an EchoJay Compressor** to it.
   **Do not touch the rack selector.**
   - **PASS:** the new slot appears in the main plugin's strip by itself, within a few seconds.
2. Look at the Link's slots in that strip.
   - **PASS:** **no GR figure on any of them** (Link meters are item 9, still queued).
3. Now select the Mix Bus's own rack, with the compressor working.
   - **PASS:** the tile reads **"GR n.n"** and the figure MOVES with the meter; the two agree; the caption is
     not cut off ("WORK...").

### 6. Gain reduction, in words (1 min) — matrix E1/E3
1. Mix Bus, with audio playing: **"how much gain reduction is the compressor doing?"**
   - **PASS:** it listens FIRST, then answers with **loudest hits** and a **typical** figure, both in the
     range you can see on the meter. No single instant figure, no "the instant hitting up to 6 dB".
2. Ask the same about **Renaissance Axx**.
   - **PASS:** it says at once that it cannot measure that one (not an EchoJay device, not certified), with
     no figure - and it does NOT say "no audio has been heard".

### 7. An ask you can answer (1 min) — matrix B1/B3/B4
1. Kick's Link: **"add a transient shaper to the kick with attack at +6 dB"**.
   - **PASS:** if it asks which one, there are **chips to tap**; if there are no chips, there is a line
     telling you to type the answer - never a question with no road out.
2. Type the plugin's name and press Enter.
   - **PASS:** it is taken as the answer and the run carries on with the same job.
3. Start another run, and while it is waiting on a question, type something unrelated
   (**"set the pre-gain to 2 dB"**).
   - **PASS:** the waiting run is marked **stopped** and a NEW run starts for what you just typed.

### 8. Your eyes, not a check (1 min)
Nothing to pass or fail - tell me what you think.

1. Does the run read as ONE message rather than a list?
2. Can you read **"GR 4.4"** on the tile at a glance?
3. Does the amber line under a choice-less question read as help rather than as an error?

---

**If a step fails:** give me the step number and what you saw. Each one maps to a matrix scenario with its
own PNG under `docs/review/<date>/`, so I can compare what the runner drew with what Logic drew - which is
the whole point of having both.
