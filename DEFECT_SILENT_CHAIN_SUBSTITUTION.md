# DEFECT: a chain the user approved was silently replaced (filed 6 Sep 2026, shoot day)

## The observation (Sean, Pro Tools, 6 Sep 2026, logged in as studio.seand@gmail.com)
The assistant proposed a mix-bus chain - SSL G Bus / Studer A800 / Neve 33609 C /
Precision Limiter - and asked "Ready to build it?". Sean answered "Build it".
The rack that was built was EchoJay EQ / AMEK / ATR-102 / bx_limiter: no
overlap with the proposal and no mention of the change. Screenshots: Sean's;
to be filed under a connected folder (echojay-vst/HANDOVER is never committed;
use tools/merge_gate_tests/results_2026-09-06/ or Documents) when he sends them.

## Why it is a defect on ANY account and ANY tier
A user's explicit approval of a named chain is a contract; building a different
chain without saying so is a silent substitution. Whether the substitution came
from resolution (a proposed name that this machine does not have resolving to
something else), from the server's second pass (the build turn re-planned
against the catalogue), or from account routing, the user must be TOLD what
changed and why before or as it lands. Nothing about tier makes that
acceptable.

## The account facts established so far (plugin side, code quoted; server side is where the rest lives)
- The plugin reads tierLevel ONLY for cosmetics: the badge text
  (PluginEditor.cpp:2959-2961 STUDIO MAX / STUDIO / PRO), look-and-feel colours
  (EchoJayLookAndFeel.h:587-607), the account card layout (:3919-3920) and the
  default message limit (EchoJayAPI.cpp:693/826/1059/3793
  defaultLimitForTier). NOTHING in the plugin gates the assistant question
  flow, the chain builder's catalogue, name resolution or a composer version on
  tier. The name-resolution ladder (EJNameLadder.h, EJWavesAlias.h,
  EJWavesRegistryFeed.h) runs locally against THIS machine's scan, tier-blind.
- The string "Oracle" does not occur anywhere in Source/: the "Oracle 2.7"
  shown in the composer comes from the server (the remote prompt/config the
  plugin fetches: "prompt in force = REMOTE v19" in today's logs), not from
  the plugin. Which prompt/version an account is served is a SERVER decision.
- The dashboard surface answers 404 not_enabled for an account without the
  server's DASHBOARD_ENABLED flag (see MERGE_2026-09-06.md, the dashboard
  gate) - a per-account server flag, not tier.
- Today's runs against production answered /api/me 404 "User not found" for
  the session this Mac held; a V1 account and the v2 test account are served
  by different backend state, which the plugin cannot see.
So: of the observed behaviours, NONE is tier-gated in the plugin; the question
flow, the catalogue offered, the model/prompt version and the dashboard flag
are all server-side per-account decisions. The discriminator is the same
request logged in as wonderwithsienna@gmail.com - if the flow and the chain are
right there, this is account routing; the silent substitution is a defect
either way.

## Status
Filed. Not chased further today (shoot). Owed: Sean's screenshots; the
wonderwithsienna re-run; then the server-side trace of the build turn (Kathy
has the saas clone) to see where the proposed names became the built ones.
