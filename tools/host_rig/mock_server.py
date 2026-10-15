#!/usr/bin/env python3
"""The host rig's local server (Sean's ask A, 13 Oct 2026).

WHY A MOCK AT ALL. The rig has to run the same path a hundred times a day, in a
gate, with no network and no cost, and it has to be able to hold a reply OPEN for
a known number of seconds - because "close the window mid-reply" is only testable
if the rig decides when mid-reply is. A live server cannot be asked for that.
Sean's ask also allows a live mode against a preview deployment; that is the
plugin pointing at a real apiEndpoint instead of this, and needs nothing here.

WHAT IT SERVES
  POST /api/chat-stream   SSE, `data: {json}\\n\\n` per EJStreamFraming.h:
                            {"type":"delta","block":"text","text":"..."}  xN
                            {"type":"done","reply":"<the whole text>",...}
                          The delay between deltas comes from the SCRIPT, so a
                          turn can be made to last exactly as long as a test needs.
  POST /api/chat          the non-streaming fallback, one JSON reply.
  POST /api/agent/start   SSE agent frames from the script (tool_call/plan/await/done).
  POST /api/agent/step    the next scripted agent round.
  POST /api/agent/stop    204.
  GET  /api/*             200 {} - every other call the plugin makes on startup
                          (feeds, polls, usage) answers empty rather than 404, so
                          a log full of 404s cannot be mistaken for the fault
                          under test.

THE SCRIPT. --script <file.json>:
  { "chat":  [ {"text": "...", "deltaMs": 40, "holdMs": 0} , ... ],
    "agent": [ [ <frame>, <frame>, ... ],   # round 1, in order
               [ ... ] ] }                  # round 2
Each chat entry is used once, in order; the last entry repeats if the rig sends
more turns than the script has. holdMs delays the FIRST delta, which is how a
reply is still "thinking" when the rig closes the window.

Isolation: this binds 127.0.0.1 only and writes nothing outside its own stdout.
"""
import argparse, json, sys, threading, time
import socket
import struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SCRIPT = {"chat": [], "agent": []}
STATE = {"chatIdx": 0, "agentRound": 0, "lock": threading.Lock(), "requests": []}


def log(*a):
    print("[mock]", *a, file=sys.stderr, flush=True)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        # EVERY request, named. The first four rig passes could not tell "the plugin never sent" from "the
        # plugin sent and the reply was dropped", because only chat holds were logged and only POSTs were
        # recorded. A rig that cannot say what the server received cannot diagnose anything.
        log("<-", self.command, self.path)

    # ---- helpers ---------------------------------------------------------
    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw.decode("utf-8") or "{}")
        except Exception:
            return {}

    def _json(self, obj, code=200):
        payload = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _sse_open(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

    def _sse(self, obj, event=None):
        """One SSE event. `event` writes the `event:` line, which the AGENT endpoints require.

        THE AGENT PATH OF THIS MOCK HAS NEVER WORKED (found 14 Oct 2026). EJAgentProtocol::parseFrame takes
        the frame's kind from the SSE EVENT NAME only - "An event with no name ... is Unknown and the client
        skips it" - and this mock wrote nothing but `data:` lines. So every agent frame it ever sent was
        discarded, which is why the scripted agent rounds were an empty list and nothing here had been
        exercised. The chat endpoints are unaffected: /api/chat-stream frames carry their type INSIDE the
        JSON ({"type":"delta"}) and EJStreamFraming skips event lines by contract."""
        head = ("event: " + event + "\n") if event else ""
        self.wfile.write((head + "data: " + json.dumps(obj) + "\n\n").encode("utf-8"))
        self.wfile.flush()

    def _record(self, path, body):
        with STATE["lock"]:
            STATE["requests"].append({"path": path, "atMs": int(time.time() * 1000),
                                      "chatId": body.get("chatId", ""),
                                      "message": (body.get("message") or body.get("goal") or "")[:200]})

    # ---- the routes ------------------------------------------------------
    def do_GET(self):
        if self.path.startswith("/api/rig/requests"):      # the rig asks what it was sent
            with STATE["lock"]:
                return self._json({"requests": STATE["requests"]})
        self._record(self.path, {})                        # GETs are requests too (/api/vst-config, feeds)
        return self._json({})

    # ---- A CUT THAT THE CLIENT ACTUALLY FEELS (15 Oct 2026) ---------------------------------------------
    # `self.connection.close()` DOES NOT DROP THE CONNECTION. Python's socket object is reference-counted by
    # the makefile() wrappers rfile/wfile, so close() only marks it: the fd stays open, no FIN is sent, and
    # the client sits there until ITS OWN timeout. Measured, with curl against a server doing exactly that:
    # 20.01 s (curl's -m 20), not 1 s. That is why the C11 row saw silence for its whole 30 s bound - the
    # plugin was not failing to record a dropped turn, it was still waiting on a socket the mock had only
    # pretended to close. With fix 2's 330 s client bound it would have waited five and a half minutes.
    #
    # A real drop needs the file wrappers released first, then SO_LINGER{on,0} so close() sends an RST
    # rather than a graceful FIN - a server crash, not a goodbye. Measured the same way: 1.01 s.
    def _hard_cut(self):
        self.close_connection = True
        for f in ("wfile", "rfile"):
            try:
                getattr(self, f).close()
            except Exception:
                pass
        try:
            self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        except Exception:
            pass
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
        try:
            self.connection.close()
        except Exception:
            pass

    def do_POST(self):
        body = self._body()
        self._record(self.path, body)

        if self.path.startswith("/api/chat-stream"):
            return self.chat_stream(body)
        if self.path.startswith("/api/chat"):
            # NON-STREAMING, AND IT HOLDS TOO. The rig's first run held only /api/chat-stream, and the plugin
            # classified the turn as a chain build and used this path - so "closing mid-reply" closed the
            # window after a reply that had already arrived. Whichever path the plugin picks, the hold is the
            # rig's instrument for deciding when mid-reply is, so it belongs on both.
            entry = self.next_chat(body)
            hold = int(entry.get("holdMs", 0))
            if hold:
                log(f"chat (non-streaming): holding {hold}ms before answering")
                time.sleep(hold / 1000.0)
            # AND IT CUTS ON THIS PATH TOO (ship 1, fix 6). The streaming path learned to cut first and the
            # row still passed, because the plugin had classified the turn as a chain edit and used THIS
            # endpoint - so the whole reply arrived in one JSON and nothing failed. Exactly the lesson the
            # `holdMs` note above records: whichever path the plugin picks, the rig's instrument has to work
            # on it, or the row proves nothing. A dropped socket here is a close with no body at all.
            if int(entry.get("cutAfterDeltas", 0)):
                log("chat (non-streaming): CUTTING the connection with no body (fix 6 row)")
                self._hard_cut()
                return
            try:
                return self._json({"reply": entry["text"], "modelName": "mock-1"})
            except (BrokenPipeError, ConnectionResetError):
                log("chat: the client went away mid-reply (expected in the close test)")
                return
        if self.path.startswith("/api/agent/start") or self.path.startswith("/api/agent/step"):
            return self.agent_stream(body)
        if self.path.startswith("/api/agent/stop"):
            return self._json({}, 200)
        return self._json({})

    def next_chat(self, body=None):
        """The next scripted reply - or, when an entry carries `holdIfContains`, the entry whose marker is in
        THIS request's message.

        Why by content: the rig's "close the window mid-reply" leg needs the reply to be held, and keying the
        hold to the entry ORDER meant the hold landed on whichever turn happened to be third. It was on the
        wrong turn in the first run, so the window closed after a reply that had already arrived and the leg
        tested nothing. A marker in the message cannot drift when a leg is added above it."""
        msg = ""
        if isinstance(body, dict):
            msg = str(body.get("message") or body.get("goal") or "")
            # the composer sends the whole prompt; the marker is in there somewhere
            if not msg:
                msg = json.dumps(body)[:200000]
        with STATE["lock"]:
            script = SCRIPT.get("chat") or [{"text": "mock reply", "deltaMs": 10, "holdMs": 0}]
            for e in script:
                mark = e.get("holdIfContains")
                if mark and mark in msg:
                    log(f"chat: the marker {mark!r} is in this message -> holding {e.get('holdMs', 0)}ms")
                    return e
            plain = [e for e in script if not e.get("holdIfContains")]
            if not plain:
                plain = script
            i = min(STATE["chatIdx"], len(plain) - 1)
            STATE["chatIdx"] += 1
            return plain[i]

    def chat_stream(self, body):
        entry = self.next_chat(body)
        text = entry.get("text", "mock reply")
        delta_ms = int(entry.get("deltaMs", 20))
        hold_ms = int(entry.get("holdMs", 0))
        log(f"chat-stream: {len(text)}b, hold {hold_ms}ms, {delta_ms}ms/word")
        self._sse_open()
        if hold_ms:
            time.sleep(hold_ms / 1000.0)
        words = text.split(" ")
        # CUT THE STREAM DELIBERATELY (ship 1, fix 6). The mock could hold a reply or finish it, and had no
        # way to FAIL one - which is why the close-mid-reply row only ever proved the success handover. Sean's
        # turn failed after his window closed and notified nobody: no reply, no error, no note, for ever.
        # `cutAfterDeltas` sends that many words and then closes with no done frame, which is what a dropped
        # socket looks like to the client.
        cut_after = int(entry.get("cutAfterDeltas", 0))
        try:
            for i, w in enumerate(words):
                if cut_after and i >= cut_after:
                    log(f"chat-stream: CUTTING the stream after {i} delta(s), no done frame (fix 6 row)")
                    try:
                        self.wfile.flush()
                    except Exception:
                        pass
                    self._hard_cut()
                    return
                self._sse({"type": "delta", "block": "text",
                           "text": (w if i == 0 else " " + w)})
                time.sleep(delta_ms / 1000.0)
            self._sse({"type": "done", "reply": text, "modelName": "mock-1",
                       "turnType": "chat", "chatId": body.get("chatId", "")})
        except (BrokenPipeError, ConnectionResetError):
            # The rig closed the window mid-reply: that IS the test, not a failure.
            log("chat-stream: the client went away mid-reply (expected in the close test)")

    def agent_stream(self, body):
        """The scripted agent rounds.

        KEYED BY THE GOAL, not by arrival order, for the reason the chat holds are: a round list indexed by
        "how many agent POSTs have happened" lands on whichever turn happens to be third, and drifts the
        moment a leg is added above it. An entry may be either a bare list of rounds (the old shape) or
        {"goalContains": "...", "rounds": [[frame, ...], ...]}. A session started from a keyed entry keeps
        reading THAT entry's rounds on each /step, so a two-round proof cannot be interleaved with another.

        Each frame is {"event": "<sse event name>", ...the data object}. The event name is what carries the
        frame's kind to the client; see _sse.
        """
        script = SCRIPT.get("agent") or []
        goal = str((body or {}).get("goal") or "")
        sid = str((body or {}).get("sessionId") or "")
        with STATE["lock"]:
            sessions = STATE.setdefault("agentSessions", {})
            entry, idx = None, 0
            if sid and sid in sessions:
                entry, idx = sessions[sid]["entry"], sessions[sid]["round"]
                sessions[sid]["round"] = idx + 1
            else:
                for e in script:
                    if isinstance(e, dict) and e.get("goalContains") and e["goalContains"] in goal:
                        entry = e
                        log(f"agent: the marker {e['goalContains']!r} is in this goal -> its scripted rounds")
                        break
                if entry is None:
                    idx = STATE["agentRound"]; STATE["agentRound"] += 1
                else:
                    idx = 0
                newSid = "ag_rig_" + str(int(time.time() * 1000))
                sessions[newSid] = {"entry": entry, "round": 1}
                STATE["lastSid"] = newSid
            rounds = (entry or {}).get("rounds") if isinstance(entry, dict) else None
            if rounds is None:
                rounds = [e for e in script if not (isinstance(e, dict) and e.get("goalContains"))]
            sidOut = sid or STATE.get("lastSid", "ag_rig")
        frames = rounds[idx] if idx < len(rounds) else [{"event": "done", "reason": "complete",
                                                         "summary": "nothing further"}]
        log(f"agent round {idx} (session {sidOut}): {len(frames)} frame(s)")
        self._sse_open()
        try:
            for f in frames:
                f = dict(f) if isinstance(f, dict) else f
                hold = int(f.pop("_holdMs", 0)) if isinstance(f, dict) else 0
                if hold:
                    time.sleep(hold / 1000.0)
                ev = f.pop("event", None) if isinstance(f, dict) else None
                # every round frame carries the session, so the client's own round/session tracking works
                if ev == "round":
                    f.setdefault("sessionId", sidOut)
                self._sse(f, ev)
                time.sleep(0.02)
        except (BrokenPipeError, ConnectionResetError):
            log("agent: the client went away mid-run (expected in the close test)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--script", default="")
    a = ap.parse_args()
    if a.script:
        with open(a.script) as f:
            SCRIPT.update(json.load(f))
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    log(f"listening on 127.0.0.1:{a.port}  chat entries={len(SCRIPT.get('chat', []))} "
        f"agent rounds={len(SCRIPT.get('agent', []))}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
