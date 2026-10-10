#pragma once
#include <JuceHeader.h>

// The one refusal a stream request may be resent from (M6, open list 150).
//
// Since 14 Sep 2026 /api/chat-stream answers 403 with
//   { error: 'Not a chain build. Send this turn to /api/chat.',
//     code: 'chat_turn_not_streamed', resolvedTurnType: 'chat', turnId }
// for any turn the SERVER resolves to type chat, which the client cannot
// know before it sends. The refusal is issued after the turn context and
// BEFORE the charge opens (api/chat-stream.js, the block headed NOT A CHAIN
// BUILD, return at :904; turnCharge.open at :919), so nothing was charged
// and one resend of the same body to /api/chat is one charge.
//
// This is the WHOLE of the decision, header-inline so the gate can run it:
// mapfps_test links the previous build's SharedCode lib, so anything a pin
// exercises has to be compiled by the test TU itself (see EJRefusalLine.h).
//
// The rule is deliberately narrow. Every other non-200 keeps showing its
// sentence: a 429 with limitReached is the meter's deny and a resend would
// be a second charge attempt (CONTRACT_stream_charge.md section 4); a
// 502 and an error frame are refundable only by a server change that is
// still [planned]; an unparseable body says nothing about the charge. Only
// the exact machine-readable code below is known to have cost nothing.
namespace echojay
{

inline const char* const kChatTurnNotStreamedCode = "chat_turn_not_streamed";

// True iff `body` parsed to a JSON object whose "code" is exactly
// chat_turn_not_streamed. Case-sensitive, equality not containment, and a
// body that is not an object (an empty body, HTML from a proxy, a bare
// string) is never resendable. The HTTP status is deliberately not an
// input: the server pairs this code with 403 today, and the code is the
// contract, the status is not.
inline bool streamRefusalIsResendable (const juce::var& body)
{
    auto* o = body.getDynamicObject();
    if (o == nullptr) return false;
    if (! o->hasProperty ("code")) return false;
    const auto code = o->getProperty ("code");
    if (! code.isString()) return false;
    return code.toString() == juce::String (kChatTurnNotStreamedCode);
}

} // namespace echojay
