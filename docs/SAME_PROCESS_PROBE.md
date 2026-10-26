# The one-minute same-process probe (Ableton, Cubase, or any host)
### His 26 Oct ruling, §1 of `LIVE_VIEW_REAL_EDITOR_2026-10-26.md`. Nothing here loads a PACE plugin or changes any state.

**What it answers.** Whether that host puts the EchoJay V2 and the EchoJay Link in **one process**. That
one fact decides which live view the plugin uses: the real plugin's own editor where they share a
process, the copy-leads view where they do not. The product asks the same question at runtime
(`echojay::embedAllowed`), so this just tells us what to expect.

**Already answered, measured on this Mac:** Logic Pro (AU) — yes, one process, one thread.
Pro Tools (AAX) — yes, one process. These two need no re-running.

---

## The steps (about a minute per host)

1. **Close every other plugin host**, so the log can only be about this one.
2. Open the host and make an **empty project**.
3. On track 1, add **EchoJay V2** (VST3). Open its window and leave it open.
4. On track 2, add **EchoJay Link** (VST3). Open its window too, and give it a moment — the Link
   publishes a heartbeat every second, which is what the probe counts.
5. Click once in the V2's chat box and type one character (any) — that is enough to make the V2
   write a line of its own.
6. In this session's terminal, paste:

   ```
   ! bash tools/same_process_probe.sh 5
   ```

## What it prints, and what each answer means

```
pid       V2-only lines   Link-only lines   verdict
23964     827             891               BOTH in this process - the real-editor road is available
VERDICT: this host puts the V2 and the Link in ONE process.
```

* **One pid with both columns non-zero** → same process. The real-editor road is available in that host.
* **Two pids, one with V2 lines and one with Link lines** → separate processes. That host gets the
  copy-leads view, and the plugin will say so rather than trying to embed.
* **"no line in the window carries a pid"** → that build predates 12 Oct 2026. The probe prints the
  one-line `log show` alternative.
* **Only one column anywhere** → one of the two plugins never actually loaded or never opened its
  window. Do steps 3–5 again and re-run.

## If a host turns out to be split
Nothing breaks and nothing needs deciding: that host takes §3 of the design (the copy leads), which
is the road being shipped first and is what every host uses today.
