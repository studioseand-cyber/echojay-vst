# The agent in the chat: the mockup Sean approved (14 Oct 2026)

Sean clicked through a mockup built from docs/DESIGN_AGENT_IN_CHAT.md and approved it: "all those tests passed, works and looks great". Build to this. Where this file and the design doc agree, nothing changes; the additions are Auto mode and the details below.

## The composer row
- Chips, left to right: **This channel**, the **Agent chip**, then the model name on the right and **Send**.
- The Agent chip shows a coloured dot and its mode: **Agent: Off** (grey), **Agent: Ask** (amber), **Agent: Auto** (the accent cyan). Clicking cycles Off -> Ask -> Auto -> Off. It is disabled while a turn is running.
- One plain line under the composer says what the current mode does:
  - Off: "Off: normal chat. EchoJay suggests, you make the changes."
  - Ask: "Ask: small moves just happen; adding, removing or touching another track waits for your Apply."
  - Auto: "Auto: does everything without waiting, and only stops to ask you a question. Undo all is on the message."
- Send is disabled while a turn is running (one turn at a time per chat).
- The box empties on Send.

## One agent run = one message
- Header row: "EchoJay", a small mode badge (ASK or AUTO), and the status on the right: Working (accent), Waiting for your OK (amber), Waiting for your answer (amber), Done (green), Stopped (grey), Undone.
- The opening sentence, then the step list. Each step is one row: status glyph, the step in plain words, and on the right either a short status ("applied", "auto-applied", "skipped", "not applied", "undone", "stopped", "your answer?", "working") or the line's buttons.
- Glyphs: tick (done), spinner (running), dim dot (pending), amber ring (awaiting approval), amber question mark (question), dash (skipped), red cross (failed), back arrow (undone), grey square (stopped). Skipped, undone and stopped rows are dimmed; undone rows are struck through.
- **Ask mode:** an ask-first step shows **[Apply] [Skip]** on its own line, and **Apply all / Skip all** appear under the list while anything is waiting. A line's Apply runs that line.
- **Auto mode:** ask-first steps run without stopping and read "auto-applied". The run only stops for a question, or for a step that cannot be undone (that step still asks, with the reason on the line).
- **A question** from the agent is a step row with answer chips under it (the mockup's "Gentle" / "More upfront"). Tapping a chip records the answer on the row ("You said: ...") and the run carries on, in both modes.
- **Stop** sits on the running message only, under the steps. Stop marks everything not yet done as stopped and the closing line says how many changes had landed.
- **A failed step** keeps its own plain sentence under the line, and a **Details** disclosure (chevron) that opens the technical message in place. No dialog.

## When it finishes
- The message collapses to its closing sentence, which says what actually landed (and what was skipped or failed).
- Under it: a **"N changes"** button with a chevron that expands the steps again ("Hide steps" to collapse), and **Undo all** on the right while there are changes to undo. Undo all marks those steps undone and the closing line becomes "Undone. Everything this run changed is back as it was."

## Closing the window mid-run
- The mockup's "Close window" showed the run carrying on while closed, and on reopen the same message is there at whatever point the run reached. That is item 0b, as the design doc specifies.

## Look
- Matches the current plugin: dark navy ground, cyan accent, the AI ASSISTANT header with AI / CHAINS, the "Working on Link: ..." strip above the chat, user messages right-aligned in a bubble, EchoJay's messages left with the "E" avatar.

## Sean's rulings that go with it
- All eight agent-in-chat recommendations in DESIGN_AGENT_IN_CHAT.md §6 and all four in DESIGN_LINK_METERS.md §6, agreed.
- The analyser follows GR + calibration in the same push, before beta.
- Auto mode as above; default for a new user is Ask; the choice persists per user.
