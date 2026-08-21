# Finding: two real items for the backend — a gated-preview handoff 404, and `?embed=plugin` unimplemented

**For the backend session (`echojay-saas-dash`).** Corrected 2026-08-18 after the
first Mac measured the preview host directly. An earlier draft of this finding
overstated the problem as a missing `/dashboard` route; that was wrong. `/dashboard`
exists. What follows is what actually holds.

## Context (verified on the first Mac)

`echojay-dash-preview.vercel.app` serves the current deployment, Vercel protection
is **off**, and `/dashboard` **is** the route. Production `/dashboard` returns 200.
On the preview host, `/dashboard` is behind a **feature gate**: an `ej_v2_preview`
cookie, seeded by loading `/dashboard?v2preview=<token>` (307 + `Set-Cookie`,
30-day). Without that cookie the preview host returns 404; with it, the real
dashboard renders. Same `preview:` namespace, so login works.

## Item 1 — a gated-preview handoff lands on a 404 (narrow)

`/dashboard` is one of the four handoff allowlist literals (`PLUGIN_DASHBOARD_PARITY_SPEC.md`
§5b; `EchoJayAPI.h:882`). A handoff mints a **single-use, 120-second** token and
`/go#t=<token>` **burns** it on redemption.

So **on a preview host in a session that has not seeded the `ej_v2_preview`
cookie**, a handoff to `/dashboard` spends a one-time credential, redirects, and
lands on the gate's 404 — token already burned, no retry without minting again.

Scope, stated honestly: this does **not** exist in production (`/dashboard` is
200 there), so there is no "View all" trap in the shipped product. It is a
preview-only sharp edge: an ungated preview session that hands off to `/dashboard`
burns a token onto a 404. Whether that is worth guarding (e.g. the gate applying
to the handoff path too) is a backend call; flagging it so it is a decision, not
a surprise.

## Item 2 — `?embed=plugin` is unimplemented (real, belongs in the queue)

§1b, §9a and §9c option 2 are all written against `/dashboard?embed=plugin` — the
mode that hides the hero and sidebar and leaves the bands. It is **spec only; not
implemented** (confirmed on the first Mac). Any stage-2 work that assumes the
webview can load an embed-mode dashboard — the §1b WebView option, the §9c
handoff-into-webview — has nothing to load until §9a ships. This is queue work,
not a conflict.

## Evidence

- Preview gate mechanics (deployment, cookie, 307/Set-Cookie/30-day, protection
  off): measured on the first Mac.
- Allowlist + handoff single-use/120 s/JWT-in-body: §5b and `EchoJayAPI.h:874-910`.
- Plugin's only live handoff target today: `DashboardTab.cpp:515`
  (`/app?overlay=community`) — so item 1 has no caller in the plugin now.
- `?embed=plugin` unimplemented: first Mac; spec references at §1b, §9a, §9c.
