# The dial test rig: the configuration that actually works

Written 9 Aug 2026, minutes after the first end-to-end Tier 2 dial
(bx_townhouse Buss Compressor, Ratio 4:1 + Attack 10 ms, turn of 12:53:37).
Two days went into discovering that THE STACK MATTERS MORE THAN ANY SINGLE
FIX — five server fixes were graded "not working" while a frozen preview
served every test. Do not rediscover this.

## The working configuration

| piece | value |
|---|---|
| client | EchoJay V2 **2.99.99** gate-test build, `echojay-vst-v200` @ feat/plugin-dashboard, `build-dev` (CMakeLists version bump deliberately uncommitted) |
| dev transport | **compiled in**: `ECHOJAY_DEV_TRANSPORT:BOOL=ON` in build-dev/CMakeCache.txt — ALL API traffic goes to `~/.echojay/dev.json` baseUrl, NOT auth.json's endpoint |
| dev.json | `{"baseUrl":"https://echojay-saas-<current-preview>.vercel.app"}` |
| world | **preview scope**. Account `preview:user:wonderwithsienna@gmail.com` (tier pro, the V2-gate signup). Production 404s this account. |
| server | `pricing-v2` @ 803a0bb, deployed as a PREVIEW (`vercel`, no --prod) |
| gates | BANDS/CONTROLS/SET_OP `*_MIN_PLUGIN_VERSION = 2.99.0` — open for the 2.99.99 client, closed for every shipped client |
| registry | `index:mapped-controls` fp-keyed (re-keyed 9 Aug; 809 legacy name fields retired) |

## THE REPOINT RULE (the two-day lesson)

**Deployment URLs are immutable builds.** After EVERY `vercel` deploy:
1. copy the printed URL into `dev.json` `baseUrl`;
2. restart Logic (dev.json is read once per process).

A dev.json left pointing at yesterday's deploy serves yesterday's code
forever, while every `vercel --prod` you run lands somewhere the plugin
never looks. That is exactly what happened 8–9 Aug (pinned preview built
8 Aug 18:09; five production deploys never served the plugin).

## Verify the serving generation BEFORE interpreting any test

The request's own side effects, not timestamps: a row in `ej:events:probe`
(shared redis, unscoped) for your turn proves the serving code is
>= e1c6fb2, which contains the whole 9 Aug batch. The probe row also
carries `emitted`/`dropped`, so "what did the model emit and what did
refine do" is readable even if the event write dies.

Ten-second stack check any time: curl the client's ACTUAL endpoint
(dev.json, not auth.json) `/api/me` with the session's token; a 404 for
your own account means you are not in the world you think you are.

## Scope facts that make preview testing valid

Only `user:{email}` and `data:{email}` are environment-scoped. The maps
corpus (`plugin:*`), `index:mapped-controls`, events/replies/probe, usage,
devices, and the pricing overlay are UNSCOPED — shared between preview and
production. A dial test on preview exercises the same data a production
user would hit.

## The reference pass (evidence chain, 9 Aug 12:53:37)

- probe: `v2|chat|pro emitted=["controls.Ratio","controls.Attack"] dropped=null`
- event: `model=claude-sonnet-4-6`, same emitted, loss not reproducing
- wire: `{"op":"set","slot":1,"settings":"Ratio 4, Attack 10 ms","settings_structured":{"controls":{"Ratio":4,"Attack":10}}}`
- client: `EJParamApply: Ratio: APPLIED 0.500 landed "4:1"`,
  `Attack: APPLIED 0.800 landed "10 ms"` (readback from the plugin)
- `EJDialable ... dialable=false` on this and every controls-only map is
  LOG-ONLY (the bar counts semantic families; known-inverted signal).

## Attribution caveat — record kept on purpose

The 9 Aug batch (fp-keying+sibling merge 10c4c09, telemetry
6579d6f/f5f8d30/e1c6fb2, per-fp mapFps+plumbing 91f602e, carrier+worked
example+rounding 43042fa, core-enumeration demonstration 1b4e1f2, routing
803a0bb) landed as ONE untested batch: none was ever served individually,
because the frozen preview predated them all. **The pass validates the
chain, not any single change. Which fix mattered is unknown and cannot be
known without ablation.** Do not cite any single one of them as "the fix".
