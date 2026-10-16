#!/usr/bin/env python3
"""The local plugin role review (Kathy, 16 Oct 2026) - today's version of the admin's /admin/plugins/review.

    python3 tools/ejmap/review/review_server.py --items <review_items.json> --answers <human_checks.json> [--port 8765]
    then open http://127.0.0.1:8765

Standard library only; listens on 127.0.0.1 alone; nothing leaves this Mac. Every answer is written to --answers at once
(atomic replace), in the format the admin's import reads (ejmap_human_checks/1), and EJ Map's --trust-levels --human reads.
    GET  /api/review                        -> { schema, generated_at, items, answers: { id: answer } }
    POST /api/review { id, verdict }        -> { ok, answer, counts }      verdict: yes | no | clear
"""
import argparse, json, os, tempfile, time, http.server, threading

HERE = os.path.dirname(os.path.abspath(__file__))
LOCK = threading.Lock()

def load(path, empty):
    try:
        with open(path) as f: return json.load(f)
    except (OSError, ValueError): return empty

def save_atomic(path, obj):
    d = os.path.dirname(os.path.abspath(path)); os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix='.human_checks.', suffix='.json')
    with os.fdopen(fd, 'w') as f: json.dump(obj, f, indent=1)
    os.replace(tmp, path)

def counts(items, answers):
    now_maps, mapped, answered = set(), set(), 0
    for it in items:
        a = answers.get(it['id'])
        if a and a.get('verdict') in ('yes', 'no'): answered += 1
        if it.get('yes_makes_mapped'):
            now_maps.add(it['fp'])
            if a and a.get('verdict') == 'yes': mapped.add(it['fp'])
    return {'items': len(items), 'answered': answered, 'countsNowMaps': len(now_maps), 'mapsMappedByAnswers': len(mapped)}

def make_handler(items_path, answers_path):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def send(self, code, body, ctype='application/json'):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store'); self.end_headers(); self.wfile.write(data)
        def state(self):
            rv = load(items_path, {'items': []}); hc = load(answers_path, {'schema': 'ejmap_human_checks/1', 'answers': []})
            return rv, hc, {a['id']: a for a in hc.get('answers', []) if 'id' in a}
        def do_GET(self):
            if self.path.split('?')[0] == '/api/review':
                rv, hc, by = self.state()
                return self.send(200, {'schema': rv.get('schema'), 'generated_at': rv.get('generated_at'), 'items': rv.get('items', []), 'answers': by})
            files = {'/': ('index.html', 'text/html; charset=utf-8'), '/admin-plugin-review.js': ('admin-plugin-review.js', 'text/javascript'),
                     '/review.css': ('review.css', 'text/css')}
            f = files.get(self.path.split('?')[0])
            if not f: return self.send(404, {'error': 'not_found'})
            with open(os.path.join(HERE, f[0]), 'rb') as fh: return self.send(200, fh.read(), f[1])
        def do_POST(self):
            if self.path != '/api/review': return self.send(404, {'error': 'not_found'})
            try: body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0') or 0)) or b'{}')
            except ValueError: return self.send(400, {'error': 'bad_json'})
            verdict = body.get('verdict'); iid = body.get('id')
            if verdict not in ('yes', 'no', 'clear'): return self.send(400, {'error': 'bad_verdict'})
            with LOCK:
                rv, hc, by = self.state()
                item = next((i for i in rv.get('items', []) if i['id'] == iid), None)
                if item is None: return self.send(404, {'error': 'no_such_item'})
                if verdict == 'clear': by.pop(iid, None); ans = None
                else:
                    ans = {'id': iid, 'fp': item['fp'], 'plugin': item['plugin'], 'version': item.get('version'), 'control': item['control'],
                           'role': item['suggested_role'], 'verdict': verdict, 'at': time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'by': 'kathy (local review)'}
                    by[iid] = ans
                save_atomic(answers_path, {'schema': 'ejmap_human_checks/1', 'items_generated_at': rv.get('generated_at'), 'answers': list(by.values())})
                return self.send(200, {'ok': True, 'answer': ans, 'counts': counts(rv.get('items', []), by)})
    return H

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--items', required=True); ap.add_argument('--answers', required=True); ap.add_argument('--port', type=int, default=8765)
    a = ap.parse_args()
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', a.port), make_handler(os.path.abspath(a.items), os.path.abspath(a.answers)))
    print(f'REVIEW: http://127.0.0.1:{a.port}  (answers -> {a.answers}; Ctrl-C to stop)', flush=True)
    try: srv.serve_forever()
    except KeyboardInterrupt: pass

if __name__ == '__main__': main()
