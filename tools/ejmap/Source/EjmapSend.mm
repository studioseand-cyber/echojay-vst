// EjmapSend.mm — Option A. See EjmapSend.h for why this is not NSURLSession.

#include "EjmapSend.h"

#import <Network/Network.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <functional>
#include <mutex>
#include <condition_variable>

namespace ejmap
{

/** Name the cause instead of listing the possibilities. */
static juce::String describeNwError (nw_error_t error, bool waiting)
{
    const juce::String prefix = waiting ? "connection could not be established: "
                                        : "connection failed: ";
    if (error == nullptr) return prefix + "no error detail from the network stack";

    const nw_error_domain_t domain = nw_error_get_error_domain (error);
    const int code = nw_error_get_error_code (error);

    if (domain == nw_error_domain_dns)
        return prefix + "DNS could not resolve the host (dns error " + juce::String (code)
             + "). The Host header names somewhere that does not exist -- check the URL the "
               "artefact was built with, not the network.";
    if (domain == nw_error_domain_tls)
        return prefix + "TLS handshake failed (tls error " + juce::String (code)
             + "). The host resolved and answered, but the certificate or protocol was refused.";
    if (domain == nw_error_domain_posix)
    {
        switch (code)
        {
            case ECONNREFUSED: return prefix + "connection REFUSED (ECONNREFUSED). The host "
                                                "exists and is reachable; nothing is listening on "
                                                "that port.";
            case EHOSTUNREACH: return prefix + "host UNREACHABLE (EHOSTUNREACH). Routing, not DNS "
                                                "and not the server.";
            case ENETUNREACH:  return prefix + "network UNREACHABLE (ENETUNREACH). This machine "
                                                "has no route out.";
            case ETIMEDOUT:    return prefix + "connection timed out (ETIMEDOUT) before the server "
                                                "answered.";
            default:           return prefix + "posix error " + juce::String (code) + " ("
                                      + juce::String (strerror (code)) + ")";
        }
    }
    return prefix + "error domain " + juce::String ((int) domain) + ", code " + juce::String (code);
}

SendResult sendBytesOverTls (const juce::MemoryBlock& bytes,
                             const juce::String& hostPort,
                             int timeoutMs)
{
    SendResult r;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();

    // The artefact's own Host header decides the destination. Port is taken
    // from it when present, so a typed port reaches the connection exactly as
    // it reaches the Host header -- the two must never disagree.
    juce::String host = hostPort;
    juce::String port = "443";
    if (hostPort.containsChar (':'))
    {
        host = hostPort.upToFirstOccurrenceOf (":", false, false);
        port = hostPort.fromLastOccurrenceOf (":", false, false);
    }
    if (host.isEmpty()) { r.refusedReason = "no host in the artefact's Host header"; return r; }

    // ONE state object, captured by pointer. __block variables cannot be
    // captured by a C++ lambda and C++ lambdas cannot be captured by an ObjC
    // block, so the two worlds share a plain struct instead. It lives on this
    // stack frame and the function does not return until every block that
    // touches it has stopped -- the wait below is what makes that true.
    struct Shared
    {
        std::mutex m;
        std::condition_variable cv;
        bool finished = false;
        juce::String failure;
        juce::MemoryBlock reply;
        nw_connection_t conn = nullptr;

        /** Have we read a whole reply? Headers plus either the Content-Length
            the server declared or, when it declared none, whatever arrives
            before it closes. Reading past this would wait on a connection the
            server is entitled to keep open. */
        bool replyIsComplete() const
        {
            const juce::String t (juce::CharPointer_UTF8 ((const char*) reply.getData()),
                                  (size_t) reply.getSize());
            const int hdrEnd = t.indexOf ("\r\n\r\n");
            if (hdrEnd < 0) return false;                       // headers still arriving
            const auto headers = t.substring (0, hdrEnd).toLowerCase();
            const int clAt = headers.indexOf ("content-length:");
            if (clAt >= 0)
            {
                const int declared = headers.substring (clAt + 15).trim()
                                            .upToFirstOccurrenceOf ("\r", false, false).getIntValue();
                const int bodyBytes = (int) reply.getSize() - (hdrEnd + 4);
                return bodyBytes >= declared;
            }
            if (headers.contains ("transfer-encoding:")) return false;   // chunked: read on

            // NO LENGTH AND NO CHUNKING. HTTP/1.1 says such a body ends at
            // connection close -- but a 3xx/204/304 has no body by definition,
            // and a keep-alive connection will not close, so waiting for one
            // spends the whole deadline. Measured: a real 307 from /forgot sat
            // for 20 s before this case existed, and reported as a timeout,
            // which is the one outcome a redirect must NOT be confused with.
            const int code = headers.fromFirstOccurrenceOf (" ", false, false)
                                    .upToFirstOccurrenceOf (" ", false, false).getIntValue();
            return (code >= 300 && code < 400) || code == 204 || code == 304;
        }

        void finish (const juce::String& why)
        {
            std::lock_guard<std::mutex> lock (m);
            if (! finished) { failure = why; finished = true; }
            cv.notify_all();
        }
    };
    Shared shared;

    nw_endpoint_t endpoint = nw_endpoint_create_host (host.toRawUTF8(), port.toRawUTF8());
    nw_parameters_t params = nw_parameters_create_secure_tcp (
        NW_PARAMETERS_DEFAULT_CONFIGURATION, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_connection_t conn = nw_connection_create (endpoint, params);
    if (conn == nullptr) { r.refusedReason = "could not create the connection"; return r; }
    shared.conn = conn;

    dispatch_queue_t q = dispatch_queue_create ("ai.echojay.ejmap.send", DISPATCH_QUEUE_SERIAL);
    nw_connection_set_queue (conn, q);

    Shared* sp = &shared;

    // FINISH ON CONTENT-LENGTH, NOT ONLY ON CLOSE. HTTP/1.1 defaults to
    // keep-alive, so the server answers and holds the connection open: waiting
    // for isComplete waits for the deadline instead of for the reply, and a
    // correct 401 arrives as a timeout. Measured: the token-unset case sat for
    // the full 20 s before this was fixed.
    //
    // The block is COPIED to the heap. A stack block that escapes into an
    // async callback is a dangling pointer the moment this frame moves on, and
    // the receive here is asynchronous by construction.
    __block void (^pump)(void) = nil;
    pump = [^{
        nw_connection_receive (sp->conn, 1, 65536,
            ^(dispatch_data_t content, nw_content_context_t, bool isComplete, nw_error_t error)
            {
                if (content != nullptr)
                    dispatch_data_apply (content,
                        ^bool (dispatch_data_t, size_t, const void* buf, size_t sz)
                        { sp->reply.append (buf, sz); return true; });
                if (error != nullptr) { sp->finish ("receive failed"); return; }
                if (sp->replyIsComplete()) { sp->finish ({}); return; }
                if (isComplete) { sp->finish ({}); return; }
                pump();
            });
    } copy];

    auto* payloadBytes = &bytes;
    nw_connection_set_state_changed_handler (conn,
        ^(nw_connection_state_t state, nw_error_t error)
        {
            // THREE CAUSES, NOT ONE. "unreachable, DNS or refused" collapsed
            // failures that need different fixes: a name that does not resolve
            // is a configuration error, a refused connection is a server that
            // is up and saying no, and an unreachable host is the network.
            // NWConnection knows which; it was simply not being asked.
            if (state == nw_connection_state_waiting || state == nw_connection_state_failed)
                sp->finish (describeNwError (error, state == nw_connection_state_waiting));
            else if (state == nw_connection_state_ready)
            {
                dispatch_data_t payload = dispatch_data_create (
                    payloadBytes->getData(), payloadBytes->getSize(), q,
                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                nw_connection_send (sp->conn, payload, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true,
                    ^(nw_error_t sendError)
                    {
                        if (sendError != nullptr) { sp->finish ("send failed"); return; }
                        pump();
                    });
            }
        });

    nw_connection_start (conn);

    {
        std::unique_lock<std::mutex> lock (shared.m);
        if (! shared.cv.wait_for (lock, std::chrono::milliseconds (timeoutMs),
                                  [&] { return shared.finished; }))
        {
            // THE TIMEOUT. The write may or may not have been received, which
            // is precisely why this is refused-with-cause and not unknown: a
            // retry from an unknown state is how one submission becomes two.
            shared.finished = true;
            shared.failure = "timeout after " + juce::String (timeoutMs) + " ms -- the server may or may "
                      "not have received it, so this is REFUSED rather than unknown and must not "
                      "be retried without checking the server first";
        }
    }

    nw_connection_cancel (conn);
    r.elapsedMs = juce::Time::getMillisecondCounterHiRes() - t0;

    if (shared.failure.isNotEmpty()) { r.refusedReason = shared.failure; return r; }

    // Parse the reply ourselves: no client between us and the bytes. The
    // decision lives in the header as a pure function so it can be proven
    // without a server -- this call IS that function, not a copy of it.
    const juce::String text (juce::CharPointer_UTF8 ((const char*) shared.reply.getData()),
                             (size_t) shared.reply.getSize());
    auto verdict = classifyReply (text, shared.reply.getSize());
    verdict.elapsedMs = r.elapsedMs;
    return verdict;
}


} // namespace ejmap
