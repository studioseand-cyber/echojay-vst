# EchoJay plugin to server API contract

## PROVENANCE, READ THIS BEFORE USING ANY FIGURE BELOW

**Read from `Source/` at `integration/reasoning-plus-pitch`, HEAD `332c6a3`, on
23 September 2026.** It describes THAT LINE OF DEVELOPMENT and nothing else.

**IT HAS NOT BEEN CHECKED AGAINST `origin/release/v2-agent`.** That branch's
`Source/EchoJayAPI.cpp` is **+702 / -173 across 29 commits** since the merge
base `25861e6c` (verified by reading git, 10 October 2026). A document about
the API surface is exactly the kind of thing 875 changed lines can invalidate
silently, so every route, field name and line number below should be treated as
unverified against that branch until someone re-reads it there.

**Section 6's question remains UNANSWERED.** Whether `POST /api/data` merges or
replaces has still not been read in `echojay-saas-dash`. Nothing in this
document answers it and nothing here should be read as answering it.

THIS DOCUMENT SPANS TWO REPOSITORIES. It records what the PLUGIN sends and
reads. It does not record what the server does with any of it. Where the two
cannot be separated by reading plugin source, the question is marked
UNANSWERED rather than filled in.

STATEMENTS ARE MARKED **read** OR **inferred**. "read" means it was taken from
the source named beside it. "inferred" means it follows from something read but
was not itself observed. **47 statements carry `read` and 5 carry `inferred`.**

**TWO ITEMS CARRY NO MARK, AND THAT IS NOT AN OVERSIGHT WORTH READING AROUND:**
the route table in section 2.2, whose rows give routes and line numbers without
a per-row mark, and the UNANSWERED directive at the end of section 6. An
earlier version of this preamble claimed EVERY line was marked, which was not
literally true; it is corrected rather than defended.

---

## 1. Transport

**read** Base URL `https://www.echojay.ai`, resolved through
`EchoJayAPI::transportEndpoint()` (`EchoJayAPI.cpp:77`), which lets
`~/.echojay/dev.json`'s `baseUrl` override it.

**read** Four helpers share one transport: `postJSON` (`:221`), `patchJSON`
(`:320`, `withHttpRequestCmd("PATCH")`), `deleteJSON` (`:377`, `"DELETE"`),
`getJSON` (`:495`).

**read** Headers on every helper call (`:252-256`): `Content-Type:
application/json`, and `Authorization: Bearer <authToken>` whenever a token is
held.

**read** `/api/vst-config` is the one route that does NOT go through a helper
and carries NO `Authorization` header (`:2355-2360`).

**read** The token lives in `EchoJayAPI::authToken` (`EchoJayAPI.h:1203`) and
is persisted to the file returned by `getSettingsFile()` (`EchoJayAPI.cpp:3808`),
which is `Application Support/EchoJay/auth.json` on macOS.

---

## 2. Routes

### 2.1 Authentication

| route | method | site | sends | reads |
|---|---|---|---|---|
| `/api/login` | POST | `:638` | `email`, `password`, `deviceId` | `token`, `email`, `user.email`, `user.name`, `user.tier`, `usage.messagesUsedToday`, `usage.messagesPerDay`, `usage.credits` |
| `/api/device/start` | POST | `:730` | `deviceId` | `userCode`, `deviceCode`, `verifyUrl`, `interval`, `expiresIn` |
| `/api/device/poll` | POST | `:777` | `deviceCode` | `status`, `token`, `error`, plus the same `user`/`usage` set as `/api/login` |

**read** All field names above are literals in the source at the sites named.

**read** `/api/login` sends the password in the JSON request body.

**read** Both paths end by assigning the response's `token` to `authToken`
(`:642`, `:799`) and both reset `classifierOff_` to false, with a comment
stating the classifier gate is per account and keyed on an allowlisted uid.

**read** No route sends a uid. `deviceId` is the only device identifier on the
wire.

**read** `verifyUrl` is opened with `juce::URL(verifyUrl).launchInDefaultBrowser()`
at `:752`, guarded only by an emptiness check at `:747`.

### 2.2 Profile and settings

| route | method | site |
|---|---|---|
| `/api/me` | GET | `:4050` (bare) and `:1025` (`?appVersion=` + `JucePlugin_VersionString`) |
| `/api/data` | GET | `:4031`, `:4083`, `EchoJayAPI.h:983` |
| `/api/data` | POST | `:4142`, `EchoJayAPI.h:991` |

**read** `/api/me` reads `user.name` only, and assigns it only when
`userSettings.name` is already empty (`:4059-4061`).

### 2.3 Everything else

**read** `/api/chat` POST `:1499`; `/api/chat-stream` POST `:1837` (SSE, via
`juce::WebInputStream`); `/api/classify` POST `:2138`; `/api/classify-question`
POST `:2270`; `/api/params/lookup` POST `:593` and `:3974`;
`/api/params/maps?fps=` GET `PluginEditor.cpp:837`; `/api/report-misdial` POST
`EchoJayAPI.h:724`; `/api/whats-new` GET `:4009`; `/api/vst-config` GET `:2355`;
`/api/v2/handoff` POST `:139` and `EchoJayAPI.h:1169`; `/api/v2/chains` POST
`.h:1010` and GET `.h:1032`; `/api/v2/chains/<id>` GET `.h:1042`, PATCH
`.h:1018`, DELETE `.h:1065`; `/api/v2/chains/<id>/share` POST `.h:1091`;
`/api/v2/shares/<slug>/import` POST `.h:1054`; `/api/v2/dashboard?surface=plugin`
GET `.h:1122`; `/api/v2/community/poll` GET `.h:1139`.

**read** Every `/api/v2/*` wrapper is gated on `isLoggedIn()` and is a no-op
when signed out.

**read** `/api/v2/handoff` sends `{"to":"/dashboard"}` and reads `url`, a
`/go#t=...` path carrying a single-use 120 second token. Its comment states the
token is never logged and nothing stores it.

**inferred** The chat, classify and params bodies are composed elsewhere and
their field sets were not enumerated for this document.

---

## 3. The three status literals, and the absence of a denied branch

**read** `status` is read into `st` at `:781` and compared against exactly
THREE literals:

```
:797   statusCode == 200 && st == "authorised" && obj->hasProperty("token")
:835   statusCode == 200 && st == "pending"
:840   statusCode == 200 && st == "expired"
```

**read** The spelling is the British `"authorised"`. A server sending
`"authorized"` matches nothing.

**read** THERE IS NO `"denied"` BRANCH. A 200 carrying `"denied"`, or any
status string other than the three above, reaches the fallthrough at
`:844-845`, whose comment reads "Transient failure (connection blip / 5xx):
keep the cadence", and re-polls with `attemptsLeft - 1`.

**read** Two separate sites produce the sentence "The sign-in code expired. Try
again.": `:773` when `attemptsLeft <= 0`, and `:842` when `st == "expired"`.

**inferred** A user who denies the pairing in the browser therefore sees no
change until attempts run out, and is then told the code expired. This follows
from the two reads above; the behaviour was not observed running.

**read** HTTP status, not `status`, decides two outcomes: 429 backs off by half
an interval (`:787-790`), 403 surfaces the response's `error` field or
"Device limit reached." (`:792-797`).

---

## 4. Field-name drift between the two repos

### 4.1 The read fallbacks

**read** `UserSettings::fromJSON` (`EchoJayAPI.cpp:3934`) accepts BOTH spellings
for two fields:

```
:3941-3943   experience   tried first, falls back to   experienceLevel
:3955-3958   daw          tried first, falls back to   daws
```

**read** The comment at `:3940` states the reason: "web app uses "experience"
(lowercase), VST used "experienceLevel" (capitalized)". The comment at `:3954`
says the same for `daw` / `daws`.

**read** `daw` is accepted as an array (`:3960-3962`) or as a single string
(`:3963-3964`), the latter commented "single string from old web app version".

### 4.2 The writer emits only the web spellings

**read** The POST body is built inline at `:4122-4136`, not by a named
serialiser. It writes `experience` and `daw`. It NEVER writes `experienceLevel`
or `daws`.

**inferred** A plugin save therefore NORMALISES a record to the web spellings:
a profile stored with `experienceLevel` or `daws` is read successfully, and the
next save writes it back under `experience` and `daw`. The old keys are not
deleted by the plugin; what happens to them depends on section 6.

### 4.3 The case mangling on experience

**read** `:3944-3946`:

```cpp
if (exp.isNotEmpty())
    exp = exp.substring(0, 1).toUpperCase() + exp.substring(1).toLowerCase();
```

**read** On write, `:4128` emits `settings.experienceLevel.toLowerCase()`.

**inferred** So a value round-trips as: whatever the server holds, upper-cased
in its first character and lower-cased in every other for internal use, then
written back entirely lower-cased. A value with internal capitals or an
acronym does not survive the round trip unchanged.

### 4.4 Full profile field list

**read** Read by `fromJSON`: `name`, `experience` / `experienceLevel`,
`monitors`, `headphones`, `genres`, `plugins`, `daw` / `daws`. All optional;
nothing is required and an absent key yields an empty string.

**read** Written by the POST builder: `name`, `monitors`, `headphones`,
`genres`, `plugins`, `experience`, `daw`.

---

## 5. Usage and tier

**read** `parseUsagePool` (`EchoJayAPI.cpp`, defined above `parseTierModels`)
is gated on `root->hasProperty("usagePool")`. The whole object is OPTIONAL; its
absence resets `info.usagePool` to `{}` and leaves `present` false.

**read** Inside `usagePool`, every field is read with `getProperty` and no
presence test, so all are optional and default to zero or empty: `used`,
`pool`, `percent`, `period`, `resetAt`, `credits`, `tierLabel`,
`capacityLabel`.

**read** Two optional lane sub-objects, `premium` and `chats`, each with
`used`, `pool`, `percent`, `resetAt`. Each sets its own `present` flag only if
the sub-object exists.

**read** One optional `model` sub-object with `fast`, `name`, `tasteRemaining`.

**read** `nudge` IS PARSED AND NEVER USED. It is guarded by `hasProperty`,
assigned to `u.nudge`, and the source comment beside it says "parsed, unused".

**read** `parseTierModels` reads an optional `tierModels` object with `chat`
and `premium`, both trimmed.

**read** Tier arrives only as `user.tier` on `/api/login` and
`/api/device/poll`, mapped through `UserInfo::tierStringToLevel`. THERE IS NO
SEPARATE ENTITLEMENT OR LICENCE ROUTE.

---

## 6. The four collections, and the open question

**read** `EchoJayAPI::saveUserSettings` (`:4071`) is a READ-MODIFY-WRITE. Its
comment at `:4082` states the intent: "First GET the existing data so we don't
overwrite chats/albums/reviews".

**read** It issues `GET /api/data` (`:4083`), then builds the POST payload:

- If `statusCode == 200 && json.isObject()` and `root` is non-null, each of
  `chats`, `albums`, `reviews`, `refTracks` is copied forward when
  `root->hasProperty(...)`, and written as an EMPTY ARRAY when it is not
  (`:4092-4113`).
- Otherwise, the `else` at `:4114-4120` writes ALL FOUR AS EMPTY ARRAYS.

**read** The `else` is taken when the GET returns a non-200, when the body is
not an object, or when `root` is null.

**read** The plugin never originates content for these four keys. Every write
of them is either a copy-forward of what the GET returned or an empty array.

**read** Three callers, none of which can choose a branch, because the branch
is decided by the GET at runtime:

```
PluginEditor.cpp:669     onComplete = nullptr, result discarded
PluginEditor.cpp:16566   checks success
PluginEditor.cpp:25930   onComplete = nullptr, result discarded
```

**read** The POST response is read only as `sc == 200` (`:4144`). Its shape is
not inspected.

### DOES THE SERVER MERGE OR REPLACE? **UNANSWERED.**

**read** Nothing in the plugin states it. The only evidence is the `:4082`
comment, which says the GET exists so the POST does not overwrite those keys.

**inferred** That comment indicates the AUTHOR BELIEVED the POST replaces. It
is a statement of intent, not of server behaviour.

**THIS MUST BE READ IN `echojay-saas-dash`, NOT GUESSED HERE.** The question is
whether `POST /api/data` merges the submitted object into the stored record or
replaces it. Until that is read, the consequence of a failed GET followed by a
successful POST is unknown, and this document does not assert it in either
direction.

**read** A nearby comment at `:4151-4152` does describe REPLACE semantics, but
it is about the scanner's plugin list and is a different field. It is recorded
here only so a later reader does not mistake it for an answer to the above.

---

## 7. Traffic that is not an API call

**read** Two `createInputStream` GETs bypass the API layer entirely and carry
no `Authorization` header:

- `PluginEditor.cpp:4732`, the installer download, URL from
  `EchoJayAPI::downloadUrlMac` / `downloadUrlWin`.
- `PluginEditor.cpp:16516`, `fetchProjectArt`, URL supplied by the dashboard
  webview through `dashView_.onNeedArt` (`:501`).

**read** TEN `launchInDefaultBrowser` sites. An earlier version of this line said "Eight" above a list of ten; the WORD was wrong and the LIST was right, and all ten were re-read from source to settle it: `EchoJayAPI.cpp:752`
(`verifyUrl`), `PluginEditor.cpp:499`, `:520` (after a successful
`mintHandoff`), `:785`, `:795`, `:1108`, `:1722`, `:1900`, `:38460`, `:38957`.

**read** EchoJay Link makes NO server calls. `LinkProcessor.cpp`,
`LinkEditor.cpp` and `LinkShm.h` contain no URL, http, `/api/` or
`createInputStream`.

---

## 8. Counts

**read** **25 routed route-and-method pairs across 21 distinct paths**, which
is what sections 2.1 to 2.3 enumerate, **plus 2 unrouted GETs** to
server-supplied URLs (`PluginEditor.cpp:4732`, the installer download, and
`:16516`, the artwork fetch). **25 + 2 = 27 calls in total.**

**read** AN EARLIER VERSION OF THIS LINE SAID "27 distinct route-and-method
pairs across 21 distinct paths". THE NUMBER 27 WAS RIGHT AND THE LABEL WAS
WRONG: 27 is the total including the two unrouted GETs, which sections 2.1 to
2.3 do not list at all, so calling all 27 route-and-method pairs double-counted
the two as routes and left the enumeration short by two against its own total.
The 21 is correct: the sections name 22 `/api` strings, of which `/api/v2/*` is
a glob quoted from a source comment and not a path.

**read** 102 lines in `Source/` match the network primitives
(`createInputStream`, `WebInputStream`, `withHttpRequestCmd`, `postJSON`,
`patchJSON`, `deleteJSON`, `getJSON`, `launchInDefaultBrowser`), partitioning
as 5 definitions, 7 declarations, 42 doc comments and other non-code, and 48
call sites.
