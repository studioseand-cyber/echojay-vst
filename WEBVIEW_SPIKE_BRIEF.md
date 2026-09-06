# Spike brief: measure the webview before building on it

**Throwaway.** This produces numbers, not code. Nothing here merges.

For a Claude Code session in `~/echojay-vst` on the second Mac. Current tip of
`feat/dashboard-tab` is `9d327e4`.

---

## Why this exists

`PLUGIN_DASHBOARD_PARITY_SPEC.md` §1d items 2 and 3 are unanswered, and §1b
names them as two of the seven things that "need a designed answer, not a hope".
The product owner chose 1b without them. That is a legitimate call, but it means
the two facts most likely to force a reversal are still unknown, and they get
more expensive to discover with every hour spent on stage 2.

An hour here either de-risks the whole 1b build or saves it entirely.

---

## Set the thresholds BEFORE you measure

Write these down first and report against them. A number measured after the fact
gets rationalised; a number measured against a threshold decides something.

Proposed, to be confirmed or changed before the first measurement:

| Question | 1b survives if | 1b is in trouble if |
|---|---|---|
| Memory, 1 instance | under ~80 MB added | over ~150 MB |
| Memory, 8 instances | under ~400 MB added total | over ~600 MB, or non-linear |
| Time to interactive after a Logic Link-window switch | under ~400 ms | over ~1 s |
| Visual | brief flash | white rectangle held long enough to read as broken |
| Stability | no crash over 20 switches | any crash, or a leaked webview process |

If a result lands between the columns, say so plainly rather than rounding it
toward the answer that keeps the plan.

---

## The build

```bash
git switch -c spike/webview-probe   # NOT feat/dashboard-tab. Nothing here merges.
```

1. `CMakeLists.txt:345` — `JUCE_WEB_BROWSER=0` → `1`.
2. macOS needs WebKit linked. Add it for this spike only; JUCE 8.0.12 uses
   `WKWebView` behind `WebBrowserComponent`.
3. In `PluginEditor.cpp`, inside the existing Dashboard block
   (`resized()`, ~16739-16761), put a `juce::WebBrowserComponent` in place of
   `dashViewport_` and point it at:

   ```
   https://www.echojay.ai/dashboard
   ```

   **Signed out is fine.** We are measuring the engine, not the page. Do not
   build the bridge, do not inject a token, do not add `?embed=plugin` — none of
   that changes the numbers and all of it costs time on a branch that gets
   deleted.

Keep the diff under ~50 lines. If it grows past that you are building stage 2 by
accident.

---

## The measurements

### M1 — memory, one instance

1. Open Logic, one EchoJay on a track, Dashboard tab **not** open. Record RSS of
   the plugin host process (`AUHostingService` / `Logic Pro`, whichever holds it
   — name which one you read, it matters for reproducing this).
2. Open the Dashboard tab. Let the page settle. Record RSS again.
3. Delta is the number.

### M2 — memory, eight instances

Same, with eight EchoJay instances, Dashboard open on all of them. **Report
whether the delta is eight times M1 or less** — if WebKit shares a process
across the instances the total may be far below linear, and that materially
changes §6d's "eight instances must not mean eight dashboards".

### M3 — the Logic Link-window switch

This is the one §1b calls out as the same class of problem as the poll timer.

1. Dashboard tab open, page loaded, **scrolled part way down**.
2. Switch to the Link window and back. Twenty times.
3. Report: time from switch-back to the page being readable again; whether the
   scroll position survives; whether it flashes white and for how long; whether
   anything leaks (watch the process count and RSS across the twenty).

### M4 — does anything die

Any crash, hang, or zombie webview process across all of the above. One crash in
twenty switches is a finding, not noise.

---

## Report, then delete

Report the five numbers against the thresholds table, plus M2's linearity answer
and anything surprising.

Then:

```bash
git switch feat/dashboard-tab
git branch -D spike/webview-probe
```

**Do not push this branch.** If a result is worth keeping, keep it as text in
the report, not as a commit. A spike branch that survives becomes a spike branch
somebody merges.

---

## What the answers change

- **M1/M2 bad** → 1b is unaffordable on a loaded session, and §1a or a
  native-lite tab comes back on the table. Better to know now than after the
  bridge is built.
- **M3 bad** → §6c's "persist route, scroll and any half-typed message on the
  processor" stops being a mitigation and becomes a hard requirement with its
  own design, before stage 2 rather than during it.
- **M2 sub-linear** → §6d's focus-gating may be unnecessary, which removes work.
- **All good** → stage 2 starts with the two riskiest unknowns closed, and
  nobody has to relitigate the decision in three months.
