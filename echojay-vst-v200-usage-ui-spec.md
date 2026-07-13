# EchoJay VST v2 Task Spec: Usage UI (usagePool), Counter Removal, Settings Percentage Bar

**Repo:** ~/Documents/ECHOJAY FILES/ECHOJAY VST/echojay-vst-v200
**Companion:** echojay-usage-v2-spec.md (SaaS side). This spec implements the client half of its /api/me and /api/chat contracts (SaaS spec sections 4.4, 7, 11.1).
**Build loop:** cmake Release build, reinstall script, test in Logic Pro. No em-dashes in any user-facing copy.

---

## 0. Goals

1. Consume the new `usagePool` object from /api/me instead of raw message counts.
2. Remove every numeric usage counter from the UI (top bar, chat header, anywhere else). No used/limit numbers visible anywhere.
3. Settings tab becomes the only place usage is shown: a percentage figure plus the existing usage bar, filled to `usagePool.percent`. Nothing else changes in Settings layout.
4. Send the v2 client contract on every API call: version identifier and `turnType`, capture payload only on explicit capture.
5. Free tier banner support using the exact server-provided copy.

Non-goals: any weights logic client-side (server-only), pricing page copy, nudge UI treatment (server flag ships, UI decided later; just don't crash on unknown fields).

---

## 1. Counter removal

There were three numeric counter call sites in v1 (top bar next to version string, Settings panel, chat header next to the Aa control). v2 inherited from that codebase, so:

- Grep `PluginEditor.cpp` (and any split-out components) for `getRemainingMessages`, `messageLimit`, and `"/"` usage-string construction. Remove every draw call that renders used/limit or remaining/limit numbers.
- Top bar: version string stays, usage portion goes.
- Chat header: mini counter goes entirely.
- Do not replace them with percentages. Chat and top bar show no usage at all. The product should feel unlimited until it isn't (limit handling in section 4).

## 2. /api/me consumption

- Parse the additive `usagePool` object: `used, pool, percent, period, resetAt, credits, tierLabel, capacityLabel, model.fast, model.tasteRemaining`, plus optional `nudge`.
- Store on the existing UserInfo struct alongside legacy fields. If `usagePool` is absent (server not yet deployed, or rollback), fall back to legacy `usage` fields and compute percent locally from used/limit. The plugin must work against both server states.
- Keep the existing refresh triggers unchanged (60s periodic, window focus, Settings open, chat send).

## 3. Settings display

- Reuse the existing usage bar component. Fill fraction = `percent / 100`, clamped 0 to 1.
- Text: `percent` plus a reset line, e.g. `42% used` and `Resets daily` / `Resets monthly` from `usagePool.period` (or `Resets [date]` from resetAt if we want the specific date; pick one, keep it short). Tier line shows `tierLabel` only. Never show pool size, used units, weights, or unit names.
- Credits: only when `credits > 0`, show a small `+N credits` line under the bar (credits are top-ups the user bought, hiding them entirely would look like theft). No credits line otherwise.
- Bar color: existing style; at >= 90 percent switch fill to the warning/coral treatment already used for refs.

## 4. Limit handling

- At 100 percent with zero credits the server will refuse the turn. On refusal, show the existing blocking notice with period-aware copy: daily pool: "You've used today's free analyses. Resets at midnight UTC." Monthly: "You've hit this month's limit. Top up or upgrade to continue." No numbers.
- Optional soft warning: one-time toast per session at >= 80 percent. If it adds risk, skip for launch.

## 5. v2 client contract (must match SaaS spec sections 4.4 and 11.1)

- Confirm what EchoJayAPI.cpp already sends as a version identifier (header or body field). Ensure v2 builds send a value the server can use for clientGen detection; if nothing exists, add `appVersion` to the request body of /api/chat and /api/me.
- Every /api/chat request includes `turnType`: `chat`, `capture_analysis`, `chain_generate`, `version_compare`, or `link_analysis`. Link requests include `busCount`.
- Capture payload attaches only on explicit capture button press or a send that includes a fresh capture. Plain chat turns send no band/meter data. Live meters keep running locally; they just never auto-attach.
- Never send `turnType: capture_analysis` without a payload (server 400s it).

## 6. Free tier banner

- When logged-in tier is free and `usagePool.model.fast` is true, show the banner in the chat area, exact string: "You're on EchoJay's fast model. Pro unlocks our most advanced feedback." Dismissible per session, reappears next session. Not shown while `tasteRemaining > 0`.
- Style: unobtrusive, existing dark teal treatment, no warning colors. It is an upsell, not an error.

## 7. Acceptance

1. Grep gate: no remaining draw calls rendering used/limit numbers anywhere in the editor.
2. Settings shows percentage text and filled bar matching /api/me `percent`; updates within 60s of server-side change.
3. Plugin behaves correctly against a server without `usagePool` (legacy fallback, no crash, percent computed locally).
4. Chat turn request body contains `turnType: chat` and no capture payload; capture press sends `capture_analysis` with payload.
5. Link analysis sends busCount; 4-bus session visible in server logs as weight 8.
6. Free account past taste shows the banner; Pro account never does.
7. Blocked state shows daily copy for free, monthly copy for paid.
8. Credits > 0 shows the +N credits line; zero credits shows nothing.

## 8. Open items (flag to Sean during build)

- resetAt display: relative ("Resets daily") vs absolute date. Suggest relative for daily, absolute date for monthly.
- Whether the 80 percent soft toast ships at launch or waits.
- Banner placement: above the chat input vs pinned top of Chat tab.
