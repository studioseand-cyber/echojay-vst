# Contract: the plugin ⇄ dashboard webview bridge (stage 3, `loadChain`)

**For the `echojay-saas-dash` session building the page half.** The plugin half
is built and shipped on `feat/dashboard-tab`; this document is what the page is
built against, so neither side guesses at the other. Everything about the JUCE
shape below is quoted from the JUCE 8.0.12 source in the plugin
(`modules/juce_gui_extra`), not recalled.

The plugin loads `/dashboard?embed=plugin` in a `juce::WebBrowserComponent`
(WKWebView on macOS) with native integration enabled and **two** native
functions registered: `loadChain` and `openChat`. No other §8 function exists
yet — an unregistered function is cleanly feature-detectable (see below); do not
assume `openProject`, `openBrowser`, `setBadge`, etc. They arrive in later stages.

---

## 1. Detecting the bridge — feature-detect, never UA-sniff

JUCE injects, at document start, a `window.__JUCE__` object. The registered
native function names are placed in
`window.__JUCE__.initialisationData.__juce__functions`
(JUCE C++ `WebBrowserComponent.cpp`: `getNativeFunctions()` → for each,
`withInitialisationData("__juce__functions", name)`). In the plugin webview that
array is:

```js
window.__JUCE__.initialisationData.__juce__functions // => ["loadChain", "openChat"]
```

Feature-detect **each** function you call by name; do not assume that
`loadChain` being present means everything is.

In an ordinary browser `window.__JUCE__` is **undefined** (JUCE's
`check_native_interop.js` only defines a placeholder with EMPTY arrays once the
`juce-framework-frontend` module is imported). So the one correct detection is:

```js
function bridgeHasLoadChain() {
  const fns = window.__JUCE__?.initialisationData?.__juce__functions;
  return Array.isArray(fns) && fns.includes("loadChain");
}
```

- **True** only inside the plugin webview with `loadChain` registered.
- **False** in any ordinary browser, and false in a future plugin build that has
  not registered it. That is the whole point of registering one function and not
  stubbing the rest: absence is a clean boolean, not a lie you special-case.
- **Never** branch on `?embed=plugin` alone or on the user agent. `embed=plugin`
  controls *layout*; the bridge is what controls *behaviour*, and only this test
  proves the bridge is live.

## 2. Calling `loadChain`

Use the JUCE frontend helper (npm `juce-framework-frontend`, the module version
bundled with JUCE 8.0.12 reports `7.0.7`; the plugin injects the matching
runtime). `getNativeFunction(name)` returns a function that forwards its
arguments and returns a `Promise` that resolves with the C++ side's result:

```js
import { getNativeFunction } from "juce-framework-frontend";

const loadChain = getNativeFunction("loadChain");
const result = await loadChain({ chainId });   // or { slug }
// result === { accepted: true }                       on success
// result === { accepted: false, reason: "<reason>" }  otherwise
```

Under the hood (JUCE `index.js`), the call emits a `__juce__invoke` event with
`{ name, params, resultId }` and the native completion resolves the promise via
`__juce__complete`. You do not need to touch that; call the function and await
the object. If you cannot use the frontend module, replicate that protocol
exactly — but the module is the supported path.

**Wire-protocol pin — re-verify on any JUCE upgrade.** The above (`__juce__invoke`
with `{ name, params, resultId }` out, `__juce__complete` with `{ promiseId,
result }` back, resolving the `getNativeFunction` promise) is read from JUCE
**8.0.12** source (`modules/juce_gui_extra/native/javascript/index.js`,
`misc/juce_WebBrowserComponent.cpp`). It is a private JUCE detail, not a stable
API. **If the plugin's JUCE version changes, re-verify these event names and
shapes against the new source before trusting this document** — a silent rename
there breaks every bridge call with no compile error on either side.

### Payload — EXACTLY ONE of `chainId` / `slug`

The first (and only meaningful) argument is a plain object. The plugin validates
it **hard, natively, before anything else** (`validateLoadChain` in
`Source/DashboardWeb.cpp`). Rules:

| Field | Type | Rules |
|---|---|---|
| `chainId` | string | present **iff** `slug` absent; non-empty; ≤ 64 chars; `[A-Za-z0-9_-]` only |
| `slug` | string | present **iff** `chainId` absent; non-empty; ≤ 32 chars; `[A-Za-z0-9_-]` only |

- **Exactly one key present.** Both present → rejected. Neither → rejected. This
  is checked by key *presence*, so do not send `{ chainId, slug: "" }` thinking
  the empty one is ignored — send only the key you mean.
- Anything else — wrong type, empty string, over-length, an illegal character,
  a non-object argument — is rejected.

Use `slug` for someone else's shared chain (Trending / Recently shared /
Featured / feed rows / message attachments); use `chainId` for the user's own
chains. The plugin does the share-import for a `slug` itself.

### `openChat` — `{ chatId: string }`

Registered alongside `loadChain`; call it the same way:

```js
const openChat = getNativeFunction("openChat");
const result = await openChat({ chatId });   // { accepted:true } | { accepted:false, reason }
```

Payload: an object with a single `chatId` string — non-empty, ≤ 64 chars,
`[A-Za-z0-9_-]` only, validated natively (`validateOpenChat`). The plugin
switches to its native Chat tab and selects that chat. Like `loadChain` this is
an **acknowledgement** (§4) — the tab switch destroys the webview, and there is
nothing to report back. Same `busy` / `bad_payload` semantics as `loadChain`
(§5), and the **same shared in-flight guard**: a `loadChain` in flight busies an
`openChat` and vice versa (both navigate away and tear the webview down).

## 3. Required page behaviour

- **Embed mode + bridge present:** a chain-row click calls
  `loadChain({ slug })` (or `{ chainId }` for an own-chain row) **instead of** the
  SPA's normal navigation. Prevent the default navigation; do not also route.
- **Bridge absent (ordinary browser, or embed mode without the bridge):**
  behaviour is **unchanged** — the row navigates as it always did. Same page,
  two behaviours, chosen only by `bridgeHasLoadChain()`.
- One call per click. The plugin has its own idempotency guard (§5), but do not
  fire `loadChain` repeatedly for one intent.

## 4. The answer is an ACKNOWLEDGEMENT, not the dial report

**This corrects the §8 table**, which predates the plugin's lazy webview
lifecycle and still says `loadChain` "answers per-slot dial results." It does
not, and cannot:

On `{ accepted: true }` the plugin switches to its native **Chain** tab, which —
by the lazy lifecycle measured in stage 2 — **destroys this webview**. There is
no page left to deliver per-slot results to, and the Chain tab already shows them
natively (which slot dialled, which came back at defaults and why). So:

- `{ accepted: true }` means **validated and handed off** — the load is now the
  plugin's, and the webview is about to go away. Do not wait for or expect a
  second message, a progress stream, or per-slot results. Treat `accepted:true`
  as "done, on my side."
- The promise resolving with `accepted:true` is the last thing the page hears.

## 5. Handling `{ accepted: false, reason }`

| `reason` | Meaning | What the page should do |
|---|---|---|
| `"busy"` | A load is already in flight (a confirm dialog is up, or one was accepted within the last ~8 s). | **Nothing.** Do not retry, do not error-toast. It self-heals; the next deliberate click works. |
| `"bad_payload"` | The payload failed native validation. | **Your bug.** The page sent a malformed `{chainId}`/`{slug}`. Fix the call; never show this to the user. Log it in dev. |

There are no other reasons this stage. Treat any unknown `reason` as
"don't retry, log it" — a forward-compatible default.

## 6. Not in this contract (later stages)

`openProject`, `openBrowser` (with the allowlist re-check), `setBadge`,
`focusChanged`, and messaging. Do not feature-detect or call them; they are not
registered, so `__juce__functions.includes(...)` for them returns false by
design.
