// stream_reroute_guard (17 Sep 2026): a streamed turn the server resolves as a normal chat is RENDERED, never
// discarded, and the user is never asked to resend. Drives the REAL EchoJayAPI decision seams the stream parser
// uses: (1) the 403 chat_turn_not_streamed answer -> re-send as chat (RED today: the message "Please send it
// again"); (2) a done frame resolvedTurnType=general with text -> rendered; (3) the quiet line appended to the
// re-sent reply. Limits: the HTTP transport itself is not driven here (no server); the seams are what the
// parser's callback calls, pinned in source.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayAPI.h"
#include <cstdio>
namespace { int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; } }
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("stream_reroute_guard: a chat-resolved streamed turn renders; never 'please send it again'\n");
    const juce::String body403 = "{\"error\":\"Not a chain build. Send this turn to /api/chat.\",\"code\":\"chat_turn_not_streamed\",\"resolvedTurnType\":\"chat\",\"turnId\":\"e2e618d3\"}";
#ifndef EJ_GUARD_TODAY
    // 10 OCT 2026: THE 403 IS RETIRED SERVER-SIDE (Sean), so this fixture now pins a RETIRED shape. It stays,
    // for two reasons worth stating rather than deleting:
    //   (a) a plugin in the field still meets servers that have not rolled, and the reroute must keep working
    //       for them - this is the downgrade direction of the same compatibility rule the state fields follow;
    //   (b) the predicate is now UNREACHABLE on a current server, and a test is the only thing that can tell
    //       "retired" from "quietly broken". If the server ever answers a chat-classified turn with anything
    //       other than a 200 stream again, (1) is what proves the client still reroutes instead of showing
    //       "Something went wrong. Please try again."
    // WHAT IS NOT ASSERTED, said plainly: that a CURRENT server's shape is handled. That needs the shape, and
    // docs/api-contract does not state it (see the FIXME below). Until it does, this guard covers the old
    // server and the done-frame path, and nothing claims to cover the new refusal - because nobody has named it.
    const auto rj = EchoJayAPI::streamRejectionFor (403, body403);
    check (rj.rerouteToChat && rj.code == "chat_turn_not_streamed" && rj.turnId == "e2e618d3", "(1) a 403 chat_turn_not_streamed from an OLDER server still re-sends to /api/chat (retired shape, kept for the field)", rj.code + " reroute=" + (rj.rerouteToChat ? "y" : "n"));
    check (! rj.message.containsIgnoreCase ("send it again"), "(1) no resend prompt", rj.message);
    // AND THE GENERIC NON-2XX PATH IS NOT A REROUTE - the counter-direction, so "reroute" cannot quietly become
    // "reroute on anything". A 500 is a failure and is reported as one.
    const auto rj500 = EchoJayAPI::streamRejectionFor (500, "{\"error\":\"upstream timeout\"}");
    check (! rj500.rerouteToChat && rj500.message.isNotEmpty(),
           "(1b) a 500 is NOT rerouted - it is reported as the failure it is", rj500.message);
    const auto other = EchoJayAPI::streamRejectionFor (500, "{\"error\":\"boom\"}");
    check (! other.rerouteToChat && other.message.isNotEmpty(), "(1) control: a real failure still reports an error", other.message);
    juce::var done = juce::JSON::parse ("{\"resolvedTurnType\":\"general\",\"stopReason\":\"end_turn\"}");
    check (EchoJayAPI::shouldRenderStreamedReply (done, "A drum bus usually wants glue compression..."), "(2) a resolvedTurnType=general reply WITH text is rendered");
    check (! EchoJayAPI::shouldRenderStreamedReply (done, "   "), "(2) control: an empty reply has nothing to render");
    const auto r = EchoJayAPI::renderRerouteReply ("Here is what I would do.");
    // 7 Oct 2026 (Sean's ruling): the quiet line is no longer appended by the client at all. rerouteQuietLine() is
    // kept deliberately so this assertion has the exact string to prove ABSENT - deleting it would assert nothing.
    check (r == juce::String ("Here is what I would do.")
           && ! r.contains (EchoJayAPI::rerouteQuietLine())
           && ! r.containsIgnoreCase ("sent as a chat"),
           "(3) a re-sent reply is rendered UNCHANGED - no quiet line  (RED as it stood: the client appended it)", r);
    check (! r.containsIgnoreCase ("send it again"), "(3) and never a resend prompt");
    // source pin: the parser's 403 branch calls the seam and the editor wires onRerouteToChat
    const auto api = juce::File ("/Users/SeanD/echojay-vst/Source/EchoJayAPI.cpp").loadFileAsString();
    const auto ed  = juce::File ("/Users/SeanD/echojay-vst/Source/PluginEditor.cpp").loadFileAsString();
    check (api.contains ("streamRejectionFor (statusCode, bodyText)") && api.contains ("ev->onRerouteToChat (se, tid)"), "(pin) the stream parser's non-2xx branch takes the seam and fires onRerouteToChat");
    check (ed.contains ("ev.onRerouteToChat = ") && ed.contains ("rerouteChatTurn (sysPrompt") && ! ed.contains ("Please send it again"), "(pin) the editor re-sends via rerouteChatTurn; no 'Please send it again' anywhere in the editor");
    check (! api.contains ("Please send it again"), "(pin) no 'Please send it again' anywhere in the API layer");
#else
    check (false, "(1) 403 chat_turn_not_streamed -> RE-SENT to /api/chat as a chat turn", "TODAY: EchoJayAPI has no streamRejectionFor; the parser shows 'Please send it again' and discards the turn");
#endif
    std::printf ("\n==== stream_reroute_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
