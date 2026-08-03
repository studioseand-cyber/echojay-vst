/*
  ==============================================================================

    EjmapSend.h — the send, Option A: the artefact's bytes on a TLS stream.

    M11's transport constraint, signed: ONE builder for artefact and wire. Under
    Option A that is literal — the TLS plaintext IS the artefact, byte for byte,
    so byte-truth holds BEFORE anything leaves and there is no diff step to run
    afterwards and no window in which a nonconforming request has already
    reached the server.

    Everything the convenience layers would do for us is what we are avoiding:
    juce::URL::withPOSTData composes headers out of sight, NSURLSession reframes
    the request and may negotiate h2. Neither can be diffed. This writes the
    bytes writeRequestArtifact already produced and reads the raw reply.

    WHAT IT REFUSES, and why each refusal is a refusal rather than a retry:

      - a non-2xx status          the server said no; retrying repeats it
      - ANY 3xx                   a redirect is not followed. Following one
                                  would send the body to an address the
                                  artefact does not name, which is exactly the
                                  byte-truth the option exists to keep
      - a timeout                 the outcome is UNKNOWN, and unknown is the
                                  one state a retry can double-submit from, so
                                  it is recorded as refused-with-cause and left
                                  for a human

    SCOPE: one mapper, this machine. No queue, no per-tester tokens, no
    sign-in, no retry policy. Those are M11 and none of them blocks mapping.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

namespace ejmap
{

struct SendResult
{
    bool sent = false;              // true ONLY on a 2xx with a body read
    int  status = 0;                // 0 when no status line was ever read
    juce::String body;
    juce::String refusedReason;     // empty iff sent
    double elapsedMs = 0;

    /** The queue state this outcome should be recorded as. Never "unknown":
        an ambiguous network write is the one state a retry can double-submit
        from, so a timeout is recorded as refused WITH ITS CAUSE and a human
        decides. */
    juce::String queueState() const { return sent ? "sent" : "refused"; }
};

/** KNOWN GAP, 3 August 2026: THE REDIRECT REFUSAL HAS NEVER FIRED LIVE.

    The 401 and the timeout are proven against the real endpoint
    (--gate-m9 sendtest). The redirect is proven only here, on classifyReply --
    which IS the function the send path calls, so the logic that runs is the
    logic proven, but the wire path has never carried a 3xx end to end.

    Why: a genuine 307 (POST /forgot, confirmed 307 by curl) returns NO BYTES
    through this connection, and the cause was not found. So today a real
    redirect exhausts the deadline and is reported as a TIMEOUT.

    IT FAILS SAFE IN THE RIGHT DIRECTION WITH THE WRONG REASON. Both outcomes
    refuse, both queue as "refused", and neither follows the redirect or
    reports success -- so nothing is sent to an address the artefact does not
    name. But the operator is told "may or may not have arrived" when the truth
    is "the server answered and told us to go elsewhere", and those call for
    different next steps: one means check the server before retrying, the other
    means the endpoint moved.

    What would close it: find why the 3xx reply never arrives at
    nw_connection_receive. Until then, treat a timeout against an endpoint that
    is otherwise healthy as a possible redirect.

    Turn a raw HTTP reply into a verdict. PURE: no network, no state, so every
    branch is provable in a test rather than only against a server that happens
    to produce that status today.

    The send path calls exactly this, so a refusal proven here is the refusal
    that runs -- a second copy of the decision for testing would be testing the
    copy. */
inline SendResult classifyReply (const juce::String& text, size_t bytesRead)
{
    SendResult r;
    const auto statusLine = text.upToFirstOccurrenceOf ("\r\n", false, false);
    r.status = statusLine.fromFirstOccurrenceOf (" ", false, false)
                         .upToFirstOccurrenceOf (" ", false, false).getIntValue();
    r.body = text.fromFirstOccurrenceOf ("\r\n\r\n", false, false);

    if (r.status == 0)
    { r.refusedReason = "no status line in the reply (" + juce::String ((int) bytesRead)
                      + " bytes read)"; return r; }
    if (r.status >= 300 && r.status < 400)
    {
        const auto loc = text.fromFirstOccurrenceOf ("\nLocation:", false, true)
                             .upToFirstOccurrenceOf ("\r", false, false).trim();
        r.refusedReason = "HTTP " + juce::String (r.status) + " redirect, NOT followed"
                        + (loc.isNotEmpty() ? " (Location: " + loc + ")" : "")
                        + " -- following it would send the body to an address the artefact does "
                          "not name";
        return r;
    }
    if (r.status < 200 || r.status >= 300)
    { r.refusedReason = "HTTP " + juce::String (r.status) + ": " + r.body.substring (0, 300);
      return r; }
    r.sent = true;
    return r;
}

/** Send raw bytes to host:port over TLS and read the reply.

    `hostPort` is the artefact's own Host header value, so the connection goes
    where the bytes say it goes and nowhere else. */
SendResult sendBytesOverTls (const juce::MemoryBlock& bytes,
                             const juce::String& hostPort,
                             int timeoutMs = 20000);

} // namespace ejmap
