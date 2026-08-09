# BACKLOG: the sweep must ship per-param decline reasons

Filed 9 Aug 2026, from the XTComp ratio investigation.

**LANDED 9 Aug 2026** — schema 2.4, tool commit f685bc9 (`declines` on the
payload, reasons authored at each decision site, tri-state by key presence,
session cargo + fp stamp) and server commit 8b61d2a in echojay-saas-dialable
(`plugin:<fp>:declines` beside evidence, outside the rev hash, never served;
counts.declines null/0/N in the ingest response). What follows is the filing
as written; deviations from it are noted in the commits.

The question "why is X unmapped on this plugin?" is UNANSWERABLE from the
server today. XTComp: identity.param_count 27, mapped 17 — ten parameters
absent from a fast-mode sweep, and the plugin's real per-channel ratio
buttons are (probably) among them. The map carries no per-parameter
decline record: `flatSkipped` is null on campaign maps, and the skips
field speaks in SEMANTICS ("ratio: deferred"), not parameters. So the
diagnosis stopped at "probably declined as stepped/mode, or UI-only —
cannot tell", and the next such question will stop at the same wall.

**What to ship:** for every parameter the sweep sees and does NOT map,
one row: { index, name, reason } — reason from the sweep's own decision
points (switch/choice declined, anchors non-monotonic, display lied,
lockstep twin, read-only, sweep budget). Rides the submission next to
`controls`; the server stores it beside evidence (NOT in the rev hash).

**Why it matters beyond diagnosis:** the no-such-control note now has
three provenance tiers (deferred / unmapped / complete). With decline
reasons, tier 2 sharpens from "may exist unmapped" to "exists at index N,
declined because <reason>" — and the completeness arithmetic
(CONTROLS_NEGATIVE_KNOWLEDGE_SCOPE.md) becomes fully explainable instead
of a bare count. 611 of 1,108 maps are incomplete by arithmetic today
(39,724 unmapped params, Snapin-host padding included); every one of
those gaps is currently a question the records cannot answer.
