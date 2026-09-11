# WITHDRAWN 10 Sep 2026 — do not action

This escalation is withdrawn. On Build H, the client request from Sean's Mac was found to be CORRECT: the param maps for
the affected plugins (SSL DeEss, Purple Audio MC 77, Acme Opticom XLA-3, ValhallaVintageVerb) are cached and dialable,
and the request's mapFps carried their exact fingerprints. The "server omitted settings for N of M" count was a symptom
with several possible causes, and Sean's observation that the SAME chains dial on his OTHER Mac (same account, same
server) refutes a blanket server omission. The open question is now a difference BETWEEN the two Macs (appVersion /
request shape), not a server bug — nothing for you to chase until that comparison is done. The original text is kept
below for history only.

---

# For Kathy: the chain block that omitted settings_structured for four of five plugins (9 Sep 2026, 15:26)

This is the server-side item from Sean's session today. READ THIS FIRST: the four plugins that did not dial would not
have dialled even if your response had carried their settings - on the build path Sean used (a borrowed host), the
plugin side has no parameter maps and no map fetch, a defect of ours filed and being fixed (MERGE_2026-09-06.md).
Two defects produce this one symptom. (Plugin side FIXED 10 Sep: a borrowed host now loads maps and fetches its own; a third-party slot can dial on the path Sean uses. Your half - the omission - is still open and still yours.) You are asked about YOUR half only: why the settings were omitted. Nothing on
the plugin side is asked of you.

The one slot your response DID carry settings for was EchoJay EQ, and it dialled (15:27:00.622 EJParamApply: slot 0
("EchoJay EQ") EXACT built-in apply, 2 band(s)). So the response shape was right for one of five.

## What was sent (the request)
- Endpoint: the chat turn (EchoJayAPI.cpp, postJSON to the chat route), turnType = chain_generate, appVersion 2.26.4.
- Body flags: "autoDial": true (Sean's "auto dial plugins only" was ON). dialWritesBlocked absent (writes allowed).
- The full request body is on disk from the plugin's dev-mode dump, written at the moment of sending:
      /Users/SeanD/Documents/EchoJay/chat-body-debug.json   (141,997 bytes, 15:26:21)
  Top-level keys: _dumpSource, appVersion, autoDial, mapFps, max_tokens, messages, turnType.
  messages: system (39,263 b), user (53 b), assistant (88 b), user (101,262 b: the AVAILABLE PLUGINS feed plus the
  instruction). The instruction in the user message reads, in part: "Dial one by putting real-world values under
  settings_structured.params using the ids listed; any param you omit is left as it is."
- Log line at send: 15:26:21.182 EJChat: send turnType=chain_generate autoDial=on dialWrites=on payload=NO msg=98417b

## What came back (the response)
- 15:26:54.139 EJChain: extracted block -- 1980 ch, 5 slot(s), 1 with settings_structured
- 15:26:54.146 EJChat: chain block feed check -- 4/5 names in recommendable feed  (EchoJay EQ is out of the feed by
  design: it is the built-in)
- The five slots, in chain order: EchoJay EQ, Eiosis E2Deesser, Purple Audio MC 77, Millennia TCL-2, UAD SPL TwinTube.
- Exactly ONE of the five carried settings_structured: EchoJay EQ (established from its apply line; the plugin does
  not log the presence per slot and does not persist the block - the plugin-side observability defect, filed). If you
  need the block itself, the server's own record of the assistant turn for chat 1788963956081 at 15:26:54 is the copy
  to read.

## What is asked
1. For that turn, which slot carried settings_structured and why the other four did not. The plugin-side hypothesis on
   record (KATHY_REPLY_2026-09-06.md) is the filter-passed omission: plugins that pass the "dialable" filter come back
   without settings_structured. Four of five here are third-party plugins with maps (mapFps was sent).
2. With autoDial: true, every slot whose plugin has a map should carry settings_structured, or the response should say
   per slot why it does not (a "settings_omitted": "<reason>" field would let the plugin print it instead of counting).
3. Confirmation that "autoDial": true is the flag your side keys on, and what the server does with mapFps.

## Two more blocks from the same afternoon (same plugin, same flag)
- 16:20:38 extracted block -- 2144 ch, 6 slot(s), 4 with settings_structured (chain: EchoJay EQ, Lindell 902 De-esser,
  UAD Tube-Tech CL 1B, Acme Opticom XLA-3, UAD SPL TwinTube, Maag EQ4) - two omitted, unnamed in the log.
- 16:22:42 extracted block -- 2097 ch, 7 slot(s), 2 with settings_structured (Auto-Tune Pro, EchoJay EQ, SSL DeEss,
  Purple Audio MC 77, AVOX WARM, UAD Maag EQ4, ValhallaVintageVerb) - the Link's own summary for this build names them:
  EchoJay EQ and ValhallaVintageVerb carried settings; Auto-Tune Pro, SSL DeEss, MC 77, AVOX WARM, UAD Maag EQ4 did not.
The rack-read failure Sean also saw today ("no answer from the Link") is a different request on the plugin side and is
not related to these omissions.

## Plugins affected today
Eiosis E2Deesser, Purple Audio MC 77, Millennia TCL-2, UAD SPL TwinTube - the four third-party slots, none delivered
with settings. Separately, and not yours: none of those four could have dialled on this build path even with settings,
because a borrowed host has no parameter maps (plugin defect, filed, being fixed in the same trip as the reporting).
