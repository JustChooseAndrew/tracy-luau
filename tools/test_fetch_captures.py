"""Tests tools/fetch_captures.py against a mock of Roblox Open Cloud's DataStore API.

    python tools/test_fetch_captures.py              self-test (no Roblox needed)
    python tools/test_fetch_captures.py --serve 8091 run the mock for a Studio test

With --serve, the mock also accepts POST /seed?key=K (body = the stored string), so a
Studio playtest can stand in for a live game: give Tracy.Capture.DataStoreSink a fake
DataStore whose SetAsync posts there, then run fetch_captures.py with
--base-url http://127.0.0.1:8091 --universe 1 against it.
"""
import argparse
import base64
import json
import os
import subprocess
import sys
import tempfile
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
API_KEY = "test-key-not-a-secret"
PAGE = 3  # small pages, so the client has to follow cursors

failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


class Store:
    def __init__(self):
        self.entries = {}  # key -> stored Luau string
        self.lock = threading.Lock()
        self.deleted = []


def make_handler(store, universe, datastore):
    root = "/datastores/v1/universes/%s/standard-datastores/datastore" % universe

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body=b"", ctype="application/json"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _parse(self):
            u = urllib.parse.urlparse(self.path)
            return u.path, dict(urllib.parse.parse_qsl(u.query))

        def _authorized(self, q):
            if self.headers.get("x-api-key") != API_KEY:
                self._send(401, b'{"error":"bad key"}')
                return False
            if q.get("datastoreName") != datastore:
                self._send(404, b'{"error":"no such datastore"}')
                return False
            return True

        def do_POST(self):
            path, q = self._parse()
            if path != "/seed" or "key" not in q:
                return self._send(404)
            body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode("utf-8")
            with store.lock:
                store.entries[q["key"]] = body
            self._send(200, b"{}")

        def do_GET(self):
            path, q = self._parse()
            if path == root + "/entries":
                if not self._authorized(q):
                    return
                with store.lock:
                    keys = sorted(store.entries)
                start = int(q.get("cursor") or 0)
                page = keys[start:start + PAGE]
                nxt = str(start + PAGE) if start + PAGE < len(keys) else ""
                body = {"keys": [{"scope": "global", "key": k} for k in page], "nextPageCursor": nxt}
                return self._send(200, json.dumps(body).encode())
            if path == root + "/entries/entry":
                if not self._authorized(q):
                    return
                with store.lock:
                    value = store.entries.get(q.get("entryKey", ""))
                if value is None:
                    return self._send(404, b'{"error":"NOT_FOUND"}')
                return self._send(200, json.dumps(value).encode())
            self._send(404)

        def do_DELETE(self):
            path, q = self._parse()
            if path != root + "/entries/entry" or not self._authorized(q):
                return self._send(404) if path != root + "/entries/entry" else None
            with store.lock:
                store.entries.pop(q.get("entryKey", ""), None)
                store.deleted.append(q.get("entryKey"))
            self._send(204)

    return Handler


def serve(port, universe="1", datastore="TracyCaptures"):
    store = Store()
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(store, universe, datastore))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, store


def seed_capture(store, cid, data, parts, **meta_extra):
    """Stores a capture the way DataStoreSink does: base64 parts, then the info."""
    size = len(data)
    step = -(-size // parts)
    for n in range(parts):
        store.entries["%s/%d" % (cid, n + 1)] = base64.b64encode(data[n * step:(n + 1) * step]).decode()
    meta = {"Id": cid, "Reason": "test", "Bytes": size, "Parts": parts, "Frames": 64, "PlaceVersion": 7,
            "Format": "libmp-compressed", "Version": 1}
    meta.update(meta_extra)
    store.entries[cid] = json.dumps(meta)


def run_fetch(port, out, *extra):
    env = dict(os.environ, ROBLOX_API_KEY=API_KEY)
    return subprocess.run([sys.executable, os.path.join(HERE, "fetch_captures.py"), "--universe", "1",
                           "--base-url", "http://127.0.0.1:%d" % port, "--out", out] + list(extra),
                          capture_output=True, text=True, env=env, timeout=120)


def self_test():
    port = 18091
    server, store = serve(port)
    try:
        good = os.urandom(1000003)
        seed_capture(store, "20261125T100000Z-aaaa0001", good, 3)
        single = os.urandom(10)
        seed_capture(store, "20261125T100500Z-aaaa0002", single, 1)
        seed_capture(store, "20261125T101000Z-bbbb0003", os.urandom(5000), 2)
        del store.entries["20261125T101000Z-bbbb0003/2"]  # a part that never got written
        seed_capture(store, "20261125T102000Z-cccc0004", os.urandom(300), 1, Bytes=999)  # wrong size
        store.entries["20261125T103000Z-dddd0005/1"] = "QUJD"  # parts whose info never arrived

        out = tempfile.mkdtemp(prefix="tracy_fetch_")
        r = run_fetch(port, out, "--delete")
        print("\n".join("        " + ln for ln in r.stdout.strip().splitlines()))
        read = lambda name: open(os.path.join(out, name), "rb").read() if os.path.exists(os.path.join(out, name)) else None
        check(read("20261125T100000Z-aaaa0001.gprx") == good, "a 3-part capture is reassembled byte-exact")
        check(read("20261125T100500Z-aaaa0002.gprx") == single, "a 1-part capture is saved")
        info = json.loads(read("20261125T100000Z-aaaa0001.json") or b"{}")
        check(info.get("Reason") == "test" and info.get("PlaceVersion") == 7, "the capture info is saved beside it")
        check(read("20261125T101000Z-bbbb0003.gprx") is None and "incomplete" in r.stdout,
              "a capture with a missing part is reported, not saved")
        check(read("20261125T102000Z-cccc0004.gprx") is None and "size mismatch" in r.stdout,
              "a capture with the wrong size is reported, not saved")
        check(r.returncode == 1, "exit code 1 when some captures failed (%d)" % r.returncode)
        gone = [k for k in store.entries if k.startswith("20261125T100000Z-aaaa0001")]
        check(not gone and "20261125T101000Z-bbbb0003" in store.entries,
              "--delete removes only what was saved")
        r2 = run_fetch(port, out)
        check("20261125T100000Z" not in r2.stdout or "already" in r2.stdout,
              "a second run skips what's already downloaded")

        env = dict(os.environ, ROBLOX_API_KEY="wrong")
        r3 = subprocess.run([sys.executable, os.path.join(HERE, "fetch_captures.py"), "--universe", "1",
                             "--base-url", "http://127.0.0.1:%d" % port, "--out", out],
                            capture_output=True, text=True, env=env, timeout=60)
        check(r3.returncode != 0 and "refused the API key" in r3.stderr, "a bad key gets a clear message")
        env.pop("ROBLOX_API_KEY")
        r4 = subprocess.run([sys.executable, os.path.join(HERE, "fetch_captures.py"), "--universe", "1"],
                            capture_output=True, text=True, env=env, timeout=60)
        check(r4.returncode != 0 and "ROBLOX_API_KEY" in r4.stderr, "a missing key gets a clear message")
    finally:
        server.shutdown()
    print("\n%s" % ("ALL PASSED" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serve", type=int, help="run the mock on this port until Ctrl+C")
    args = ap.parse_args()
    if args.serve:
        server, store = serve(args.serve)
        print("mock Open Cloud on http://127.0.0.1:%d (universe 1, DataStore TracyCaptures, key %s)"
              % (args.serve, API_KEY), flush=True)
        try:
            threading.Event().wait()
        except KeyboardInterrupt:
            server.shutdown()
        return 0
    return self_test()


if __name__ == "__main__":
    sys.exit(main())
