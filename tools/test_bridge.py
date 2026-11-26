"""Protocol tests for tracy-roblox-bridge. Runs an isolated bridge on test ports.

    python tools/test_bridge.py [--bridge build/tracy-roblox-bridge.exe] [--tracy-dir DIR]

--tracy-dir points at a Tracy 0.15 release folder (tracy-capture, tracy-csvexport).
With it, the data checks also confirm what Tracy itself recorded; without it they
are skipped and only the HTTP and /status checks run.
"""
import argparse
import json
import os
import random
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_client as fc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTTP_PORT = 18080
TRACY_PORT = 18086
URL = "http://127.0.0.1:%d/ingest" % HTTP_PORT
EXE = ".exe" if os.name == "nt" else ""

failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def post(body):
    return fc.post(URL, body)


def status():
    with urllib.request.urlopen("http://127.0.0.1:%d/status" % HTTP_PORT, timeout=5) as r:
        return json.loads(r.read())


def totals():
    keys = ("batches", "events", "gaps", "droppedEnds", "malformed", "unknownIds")
    out = dict.fromkeys(keys, 0)
    for t in status()["tracks"]:
        for k in keys:
            out[k] += t[k]
    return out


def delta(before):
    now = totals()
    return {k: now[k] - before[k] for k in now}


def new_session():
    return random.getrandbits(32)


def past(span):
    """A start time `span` seconds ago, so a fabricated timeline ends before it is sent.

    Waits `span` first: sessions reuse tracks, and a track never goes back in time, so
    a test's timeline must start after the previous test's ended (as real playtests do).
    """
    time.sleep(span + 0.02)
    return time.perf_counter() - span


def header(session, seq, clock, length, flags=0, source=0, clock_offset=0.0, version=fc.VERSION):
    return struct.pack(fc.HEADER, fc.MAGIC, version, source, flags, session, seq, clock, clock_offset, length)


class Bridge:
    def __init__(self, exe, extra=()):
        env = dict(os.environ, TRACY_PORT=str(TRACY_PORT))
        self.log = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        # its own process group, so stop_gracefully() can send it Ctrl+Break alone
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        self.proc = subprocess.Popen([exe, "--port", str(HTTP_PORT)] + list(extra), stdout=self.log,
                                     stderr=subprocess.STDOUT, env=env, creationflags=flags)
        for _ in range(50):
            try:
                status()
                return
            except Exception:
                time.sleep(0.1)
        raise SystemExit("bridge did not start")

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        self.proc.kill()
        self.proc.wait(5)

    def stop_gracefully(self, timeout=90):
        """Like pressing Ctrl+C: the bridge shuts down (and converts a --record session)."""
        import signal
        self.proc.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return None

    def output(self):
        self.log.flush()
        with open(self.log.name, encoding="utf-8", errors="replace") as f:
            return f.read()


def start_capture(tracy_dir, path, seconds):
    p = subprocess.Popen([os.path.join(tracy_dir, "tracy-capture" + EXE), "-o", path, "-a", "127.0.0.1", "-p",
                          str(TRACY_PORT), "-f", "-s", str(seconds)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    return p


def csvexport(tracy_dir, *args):
    # fuzzed messages are arbitrary bytes, so decode leniently
    out = subprocess.run([os.path.join(tracy_dir, "tracy-csvexport" + EXE)] + list(args), capture_output=True).stdout
    return out.decode("utf-8", "replace")


def zone_stats(tracy_dir, path):
    # One row per source location. The same zone name on several tracks is several
    # rows, so merge rows by name and remember which tracks it appeared on.
    # Zone names can contain commas (MicroProfiler's do), so read each row from the
    # right: name, src_file, then 8 numeric columns.
    stats = {}
    for row in csvexport(tracy_dir, path).splitlines()[1:]:
        cols = row.split(",")
        if len(cols) >= 10:
            name, track = ",".join(cols[:-9]), cols[-9]
            total, count, low, high = int(cols[-7]), int(cols[-5]), int(cols[-3]), int(cols[-2])
            s = stats.setdefault(name, {"count": 0, "total": 0, "min": low, "max": high, "files": set()})
            s["count"] += count
            s["total"] += total
            s["min"] = min(s["min"], low)
            s["max"] = max(s["max"], high)
            s["files"].add(track)
    for s in stats.values():
        s["mean"] = s["total"] / s["count"] if s["count"] else 0
    frame_sets = {}
    for row in csvexport(tracy_dir, "-F", path).splitlines()[1:]:
        cols = row.split(",")
        if len(cols) >= 3:
            frame_sets[cols[0]] = int(cols[2])
    return stats, frame_sets


def zone_starts(tracy_dir, path, name):
    starts = []
    for row in csvexport(tracy_dir, "-u", "-f", name, path).splitlines()[1:]:
        cols = row.split(",")
        if len(cols) >= 5 and cols[0] == name:
            starts.append(int(cols[3]))
    return sorted(starts)


# ---------------------------------------------------------------- data tests (captured)

def test_basic():
    """Nested zones, frames, a named frame set and a plot keep their exact timings."""
    s = new_session()
    t = past(1.01)
    b = fc.Batch()
    b.register(1, "T1::Outer")
    b.register(2, "T1::Inner")
    b.register(3, "T1::Value")
    b.register(4, "T1::Named")
    for i in range(100):
        base = t + i * 0.01
        b.zone_begin(1, base)
        b.zone_begin(2, base + 0.001)
        b.zone_end(base + 0.003)
        b.zone_end(base + 0.004)
        b.plot(3, base + 0.004, float(i))
        b.frame_mark_named(4, base + 0.005)
    code, text = post(b.encode(s, 0, time.perf_counter(), fc.FLAG_FINAL))
    check(code == 204, "basic batch accepted (HTTP %d %s)" % (code, text))


def test_zero_length():
    """Zones that begin and end on the same tick still count."""
    s = new_session()
    t = past(0.001)
    b = fc.Batch()
    b.register(1, "T2::Zero")
    for i in range(50):
        b.zone_begin(1, t + i * 1e-5)
        b.zone_end(t + i * 1e-5)
    code, _ = post(b.encode(s, 0, time.perf_counter(), fc.FLAG_FINAL))
    check(code == 204, "zero-length batch accepted")


def test_colors_messages_plots():
    """Colored zones, messages (plain and colored) and a configured plot."""
    s = new_session()
    t = past(0.05)
    b = fc.Batch()
    b.register(1, "T5::Colored")
    b.register(2, "T5::Heap")
    b.plot_config(2, fc.PLOT_MEMORY, step=True, fill=False, color=0x33AAFF)
    for i in range(20):
        base = t + i * 0.002
        b.zone_begin_color(1, base, 0xFF5500)
        b.zone_end(base + 0.001)
        b.plot(2, base, 1024.0 * (i + 1))
    b.message(t + 0.0005, "T5 plain message")
    b.message(t + 0.0015, "T5 orange message", 0xFF8800)
    code, text = post(b.encode(s, 0, time.perf_counter(), fc.FLAG_FINAL))
    check(code == 204, "colors/messages/plot batch accepted (HTTP %d %s)" % (code, text))


def test_relay_alignment():
    """A server batch and a relayed client batch in one request: the client gets its own
    track, and clockOffset lines its events up with the server's."""
    server, client = new_session(), new_session()
    t = past(0.11)
    clock_offset = 12.5  # client clock runs 12.5 s behind the server's
    sb = fc.Batch()
    sb.register(1, "T6::ServerTick")
    cb = fc.Batch()
    cb.register(1, "T6::ClientTick")
    cb.register(2, "T6::Ping")
    for i in range(10):
        moment = t + i * 0.01  # server clock
        sb.zone_begin(1, moment)
        sb.zone_end(moment + 0.002)
        cb.zone_begin(1, moment - clock_offset)  # the same instant, on the client's clock
        cb.zone_end(moment - clock_offset + 0.002)
        cb.plot(2, moment - clock_offset, 4.0)
        cb.frame_mark(moment - clock_offset + 0.005)
    now = time.perf_counter()
    body = (sb.encode(server, 0, now) +
            cb.encode(client, 0, now - clock_offset, source=1, clock_offset=clock_offset))
    code, text = post(body)
    check(code == 204, "server + client batch in one request accepted (HTTP %d %r)" % (code, text))
    # end both sessions
    now = time.perf_counter()
    post(fc.Batch().encode(server, 1, now, fc.FLAG_FINAL) +
         fc.Batch().encode(client, 1, now - clock_offset, fc.FLAG_FINAL, source=1, clock_offset=clock_offset))
    labels = {tr["label"]: tr["source"] for tr in status()["tracks"]}
    check(labels.get("Roblox Client 1") == 1, "client batch got the 'Roblox Client 1' track")


def test_named_tracks():
    """SourceNamed sessions (imports) get the track their TRACK_NAME says, and zone text."""
    t = past(0.02)
    body = b""
    for k, name in enumerate(("Imported Main", "Imported Worker")):
        b = fc.Batch()
        raw = name.encode()
        b.raw(struct.pack("<BH", 0x0A, len(raw)) + raw)  # TRACK_NAME first
        b.register(1, "N1::Scope, " + name)
        for i in range(5):
            b.zone_begin(1, t + i * 0.002)
            text = b"label %d" % i
            b.raw(struct.pack("<BH", 0x0B, len(text)) + text)  # ZONE_TEXT
            b.zone_end(t + i * 0.002 + 0.001)
        body += b.encode(0x4E000000 + k, 0, time.perf_counter(), fc.FLAG_FINAL, source=255)
    code, text = post(body)
    check(code == 204, "named-track batches accepted (HTTP %d %r)" % (code, text))
    labels = {tr["label"] for tr in status()["tracks"] if tr["source"] == 255}
    check(labels >= {"Imported Main", "Imported Worker"},
          "TRACK_NAME picks the track name (%s)" % sorted(labels))


def test_concurrent_sessions():
    """Several server VMs at once each get their own track and lose nothing."""
    results = []

    def run():
        s = new_session()
        t = past(0.06)
        ok = True
        for seq in range(20):
            b = fc.Batch()
            if seq == 0:
                b.register(7, "T8::Worker")
            for i in range(25):
                base = t + (seq * 25 + i) * 1e-4
                b.zone_begin(7, base)
                b.zone_end(base + 5e-5)
            final = fc.FLAG_FINAL if seq == 19 else 0
            code, _ = post(b.encode(s, seq, time.perf_counter(), final))
            ok = ok and code == 204
        results.append(ok)

    threads = [threading.Thread(target=run) for _ in range(4)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    check(all(results) and len(results) == 4, "4 concurrent sessions accepted")
    labels = {t["label"] for t in status()["tracks"] if t["source"] == 0}
    check(len(labels) >= 4, "concurrent sessions got separate tracks (%s)" % ", ".join(sorted(labels)))


# ---------------------------------------------------------------- protocol tests

def test_gap():
    """A skipped sequence number closes open zones; a stray ZoneEnd after it is dropped."""
    before = totals()
    s = new_session()
    t = time.perf_counter()
    b = fc.Batch()
    b.register(1, "G::Open")
    b.zone_begin(1, t)
    post(b.encode(s, 0, t))
    b = fc.Batch()
    b.zone_end(t + 0.01)  # its zone was closed by the gap
    code, _ = post(b.encode(s, 2, t + 0.01, fc.FLAG_FINAL))
    d = delta(before)
    check(code == 204, "batch after a gap accepted")
    check(d["gaps"] == 1, "gap counted (%d)" % d["gaps"])
    check(d["droppedEnds"] == 1, "stray ZoneEnd dropped (%d)" % d["droppedEnds"])


def test_resync():
    """A session joined mid-stream with unknown ids asks Luau to resend its names."""
    before = totals()
    s = new_session()
    t = time.perf_counter()
    b = fc.Batch()
    b.zone_begin(42, t)
    b.zone_end(t + 0.001)
    code, text = post(b.encode(s, 7, t))
    check(code == 200 and text.strip() == "resync %08x" % s,
          "unknown id answered with 'resync <session>' (HTTP %d %r)" % (code, text))
    b = fc.Batch()
    b.register(42, "R::Known")
    b.zone_begin(42, t + 0.002)
    b.zone_end(t + 0.003)
    code, text = post(b.encode(s, 8, t + 0.003, fc.FLAG_FINAL))
    check(code == 204, "after registering, the next batch is clean (HTTP %d %r)" % (code, text))
    d = delta(before)
    check(d["unknownIds"] == 1 and d["gaps"] == 0, "mid-stream join is not a gap, one unknown-id batch")


def test_per_batch_notes():
    """In a combined request, each batch's problem is reported on its own line."""
    server, client_bad, client_unknown = new_session(), new_session(), new_session()
    t = time.perf_counter()
    ok = fc.Batch()
    ok.register(1, "N::Fine")
    ok.zone_begin(1, t)
    ok.zone_end(t + 0.001)
    bad = fc.Batch()
    bad.raw(b"\x7f\x00\x00")  # unknown opcode
    unknown = fc.Batch()
    unknown.zone_begin(9, t)
    unknown.zone_end(t + 0.001)
    body = (ok.encode(server, 0, t) + bad.encode(client_bad, 0, t, source=1) +
            unknown.encode(client_unknown, 3, t, source=2))
    code, text = post(body)
    lines = set(text.strip().splitlines()) if code == 200 else set()
    check(code == 200, "combined request with problems answers 200 (got %d)" % code)
    check(any(ln.startswith("error %08x " % client_bad) for ln in lines), "bad client batch -> 'error <session> ...'")
    check("resync %08x" % client_unknown in lines, "unknown-id client batch -> 'resync <session>'")
    check(not any("%08x" % server in ln for ln in lines), "the healthy server batch reports nothing")
    # framing broken after a good batch: the good one still counts, the rest is reported
    good = fc.Batch()
    good.zone_begin(1, t + 0.01)
    good.zone_end(t + 0.011)
    code, text = post(good.encode(server, 1, t + 0.01) + b"TLUA\x02garbage")
    check(code == 200 and text.startswith("error - "), "broken framing after a batch -> 'error - ...' (%d %r)" % (
        code, text))
    for sess, src in ((server, 0), (client_bad, 1), (client_unknown, 2)):
        post(fc.Batch().encode(sess, 9, t + 1, fc.FLAG_FINAL, source=src))


def test_malformed():
    """Broken input is rejected with 400 and never takes the bridge down."""
    s = new_session()
    t = time.perf_counter()

    def batch(seq, events, **kw):
        return header(s, seq, t, len(events), **kw) + events

    cases = {
        "empty body": b"",
        "short header": header(s, 0, t, 0)[:10],
        "bad magic": b"XXXX" + header(s, 0, t, 0)[4:],
        "bad version": header(s, 0, t, 0, version=1),
        "length past the end": header(s, 0, t, 100) + b"short",
        "truncated ZONE_BEGIN": batch(0, bytes([fc.OP_ZONE_BEGIN, 1, 0, 0])),
        "truncated REGISTER_STRING": batch(1, struct.pack("<BHH", fc.OP_REGISTER_STRING, 1, 50) + b"short"),
        "truncated MESSAGE": batch(2, struct.pack("<BdIH", fc.OP_MESSAGE, t, 0, 40) + b"short"),
        "bad plot format": batch(3, struct.pack("<BHBBI", fc.OP_PLOT_CONFIG, 1, 7, 0, 0)),
        "unknown opcode": batch(4, b"\x7f\x00\x00"),
        "NaN timestamp": batch(5, struct.pack("<Bd", fc.OP_ZONE_END, float("nan"))),
        "infinite timestamp": batch(6, struct.pack("<BHd", fc.OP_ZONE_BEGIN, 1, float("inf"))),
        "absurd timestamp": batch(7, struct.pack("<Bd", fc.OP_FRAME_MARK, 1e300)),
        "NaN flushClock": header(s, 8, float("nan"), 0),
        "NaN clockOffset": header(s, 9, t, 0, clock_offset=float("nan")),
    }
    for name, body in cases.items():
        code, text = post(body)
        # a single bad batch is either refused outright (400) or reported per batch (200 + "error")
        rejected = code == 400 or (code == 200 and text.startswith("error"))
        check(rejected, "%s rejected (HTTP %d %r)" % (name, code, text[:40]))
    post(header(s, 10, t, 0, flags=fc.FLAG_FINAL))


def test_content_type():
    """Browsers can POST text/plain cross-site; the bridge only takes octet-stream."""
    s = new_session()
    body = fc.Batch().encode(s, 0, time.perf_counter(), fc.FLAG_FINAL)
    req = urllib.request.Request(URL, data=body, method="POST", headers={"Content-Type": "text/plain"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            code = r.status
    except urllib.error.HTTPError as e:
        code = e.code
    check(code == 415, "text/plain POST refused with 415 (got %d)" % code)


def test_late_batch():
    """Batches a session sends after its FINAL batch are ignored."""
    s = new_session()
    t = time.perf_counter()
    b = fc.Batch()
    b.register(1, "L::Late")
    post(b.encode(s, 0, t, fc.FLAG_FINAL))
    before = totals()
    b = fc.Batch()
    b.zone_begin(1, t + 0.01)
    code, _ = post(b.encode(s, 1, t + 0.01))
    d = delta(before)
    check(code == 204 and d["batches"] == 0, "late batch ignored")


def test_fuzz(seed=1234, rounds=3000):
    """Random event streams behind valid headers: only 200/204/400, bridge stays up."""
    rng = random.Random(seed)
    codes = set()
    s = new_session()
    t = time.perf_counter()
    for i in range(rounds):
        kind = rng.random()
        if kind < 0.3:
            events = bytes(rng.randrange(256) for _ in range(rng.randrange(64)))
        elif kind < 0.85:
            b = fc.Batch()
            for _ in range(rng.randrange(20)):
                op = rng.randrange(1, 10)
                tm = rng.choice([t, -1e300, 1e300, float("nan"), float("inf"), t + rng.random()])
                if op == 1:
                    text = bytes(rng.randrange(256) for _ in range(rng.randrange(40)))
                    b.raw(struct.pack("<BHH", 1, rng.randrange(65536), len(text)) + text)
                elif op in (2, 6):
                    b.raw(struct.pack("<BHd", op, rng.randrange(65536), tm))
                elif op in (3, 4):
                    b.raw(struct.pack("<Bd", op, tm))
                elif op == 5:
                    b.raw(struct.pack("<BHdd", 5, rng.randrange(65536), tm, rng.choice([1.0, float("nan")])))
                elif op == 7:
                    b.raw(struct.pack("<BHdI", 7, rng.randrange(65536), tm, rng.getrandbits(32)))
                elif op == 8:
                    text = bytes(rng.randrange(256) for _ in range(rng.randrange(40)))
                    b.raw(struct.pack("<BdIH", 8, tm, rng.getrandbits(32), len(text)) + text)
                else:
                    b.raw(struct.pack("<BHBBI", 9, rng.randrange(65536), rng.randrange(5), rng.randrange(4),
                                      rng.getrandbits(32)))
            events = b"".join(b.parts)
            if rng.random() < 0.3 and events:
                events = events[: rng.randrange(len(events))]
        else:
            code, _ = post(bytes(rng.randrange(256) for _ in range(rng.randrange(60))))
            codes.add(code)
            continue
        length = len(events) if rng.random() < 0.9 else rng.randrange(200)
        body = header(s, i, t, length, source=rng.randrange(3), clock_offset=rng.choice([0.0, 3.5])) + events
        if rng.random() < 0.2:
            body += body  # two batches in one request
        code, _ = post(body)
        codes.add(code)
    check(codes <= {200, 204, 400}, "fuzz responses limited to 200/204/400 (%s)" % sorted(codes))


def send_recorded_session():
    """A server session relaying one client, over several requests, then FINAL."""
    server, client = new_session(), new_session()
    t = past(0.3)
    clock_offset = 7.25
    for seq in range(3):
        sb, cb = fc.Batch(), fc.Batch()
        if seq == 0:
            sb.register(1, "R1::Server")
            sb.register(2, "R1::Value")
            cb.register(1, "R1::Client")
        for i in range(20):
            moment = t + (seq * 20 + i) * 0.004
            sb.zone_begin_color(1, moment, 0x3399FF)
            sb.zone_end(moment + 0.001)
            sb.plot(2, moment, float(i))
            cb.zone_begin(1, moment - clock_offset)
            cb.zone_end(moment - clock_offset + 0.001)
        sb.message(t + seq * 0.08, "R1 message %d" % seq)
        flags = fc.FLAG_FINAL if seq == 2 else 0
        now = time.perf_counter()
        code, _ = post(sb.encode(server, seq, now, flags) +
                       cb.encode(client, seq, now - clock_offset, flags, source=1, clock_offset=clock_offset))
        if code != 204:
            return False
        time.sleep(0.1)
    return True


def recorded_stats(tracy_dir, path):
    stats, _ = zone_stats(tracy_dir, path)
    s, c = stats.get("R1::Server"), stats.get("R1::Client")
    skew = max((abs(a - b) for a, b in zip(zone_starts(tracy_dir, path, "R1::Server"),
                                         zone_starts(tracy_dir, path, "R1::Client"))), default=None)
    return {
        "server": s and s["count"], "client": c and c["count"],
        "server_mean": s and round(s["mean"]), "client_track": c and sorted(c["files"]),
        "skew": skew,
        "messages": csvexport(tracy_dir, "-m", path).count("R1 message"),
        "plot": "Server: R1::Value" in csvexport(tracy_dir, "-u", "-p", "-f", "R1", path),
    }


def recording_tests(args):
    tool = os.path.join(args.tracy_dir, "tracy-capture" + EXE)
    tmp = tempfile.mkdtemp(prefix="tracy_luau_rec_")
    rec, raw = os.path.join(tmp, "session.tracy"), os.path.join(tmp, "session.tlrec")
    live_trace = os.path.join(tmp, "live.tracy")

    # --record while a live viewer is connected: both must end up with the same data
    bridge = Bridge(args.bridge, ["--record", rec, "--capture-tool", tool, "--keep-raw"])
    live = start_capture(args.tracy_dir, live_trace, 6)
    check(send_recorded_session(), "recorded session accepted")
    live.wait(30)
    code = bridge.stop_gracefully()
    check(code == 0, "bridge with --record exits cleanly after Ctrl+Break (exit %s)" % code)
    check(os.path.exists(rec), "--record wrote the .tracy file")
    check(os.path.exists(raw), "--keep-raw kept the .tlrec")
    if os.path.exists(rec):
        got = recorded_stats(args.tracy_dir, rec)
        want = {"server": 60, "client": 60, "server_mean": 1000000, "client_track": ["Roblox Client 1"], "skew": 0,
                "messages": 3, "plot": True}
        check(got == want, "recording has every zone, message and plot, client aligned (%s)" % got)
        live_got = recorded_stats(args.tracy_dir, live_trace)
        check(live_got == got, "the live viewer saw the same thing meanwhile (%s)" % live_got)

    # standalone --convert of the kept raw recording reproduces it
    if os.path.exists(raw):
        again = os.path.join(tmp, "again.tracy")
        r = subprocess.run([args.bridge, "--convert", raw, again, "--capture-tool", tool], capture_output=True,
                           text=True, timeout=120)
        check(r.returncode == 0 and os.path.exists(again), "--convert works on its own (%s)" % r.stdout.strip())
        if os.path.exists(again) and os.path.exists(rec):
            check(recorded_stats(args.tracy_dir, again) == recorded_stats(args.tracy_dir, rec),
                  "--convert output matches the --record output")
        r = subprocess.run([args.bridge, "--convert", rec, os.path.join(tmp, "x.tracy"), "--capture-tool", tool],
                           capture_output=True, text=True, timeout=60)
        check(r.returncode != 0 and "not a Tracy-Luau recording" in r.stderr, "--convert refuses a non-recording")

    # without --keep-raw the .tlrec goes away once the .tracy is written
    rec2, raw2 = os.path.join(tmp, "plain.tracy"), os.path.join(tmp, "plain.tlrec")
    bridge = Bridge(args.bridge, ["--record", rec2, "--capture-tool", tool])
    send_recorded_session()
    code = bridge.stop_gracefully()
    check(code == 0 and os.path.exists(rec2) and not os.path.exists(raw2), "raw recording removed after conversion")

    # stopping with nothing recorded writes nothing
    rec3 = os.path.join(tmp, "empty.tracy")
    bridge = Bridge(args.bridge, ["--record", rec3, "--capture-tool", tool])
    code = bridge.stop_gracefully()
    check(code == 0 and not os.path.exists(rec3) and not os.path.exists(os.path.join(tmp, "empty.tlrec")),
          "an empty recording leaves no files")

    # a missing tracy-capture is reported before recording starts
    r = subprocess.run([args.bridge, "--port", str(HTTP_PORT), "--record", os.path.join(tmp, "n.tracy"),
                        "--capture-tool", os.path.join(tmp, "missing.exe")], capture_output=True, text=True,
                       timeout=30)
    check(r.returncode != 0 and "tracy-capture" in r.stderr, "missing tracy-capture is reported up front")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bridge", default=os.path.join(ROOT, "build", "tracy-roblox-bridge" + EXE))
    ap.add_argument("--tracy-dir", default=os.path.join(ROOT, ".tools", "tracy-0.15.0"))
    args = ap.parse_args()
    have_tracy = os.path.isdir(args.tracy_dir)

    print("data tests" + ("" if have_tracy else " (no Tracy tools: Tracy-side checks skipped)"))
    bridge = Bridge(args.bridge)
    trace = os.path.join(tempfile.gettempdir(), "tracy_luau_test.tracy")
    cap = start_capture(args.tracy_dir, trace, 10) if have_tracy else None
    try:
        test_basic()
        test_zero_length()
        test_colors_messages_plots()
        test_relay_alignment()
        test_named_tracks()
        test_concurrent_sessions()
        # garbage while a viewer is connected must not corrupt what Tracy recorded
        test_fuzz(seed=99, rounds=600)
    finally:
        if cap:
            cap.wait(30)
        bridge.stop()
    if have_tracy:
        stats, frames = zone_stats(args.tracy_dir, trace)
        o, n = stats.get("T1::Outer"), stats.get("T1::Inner")
        check(o is not None and o["count"] == 100 and abs(o["mean"] - 4e6) < 2, "T1::Outer: 100 zones of 4.000 ms")
        check(n is not None and n["count"] == 100 and abs(n["mean"] - 2e6) < 2, "T1::Inner: 100 zones of 2.000 ms")
        check(frames.get("Server: T1::Named", 0) >= 99,
              "named frame set recorded as 'Server: T1::Named' (%s)" % frames.get("Server: T1::Named"))
        z = stats.get("T2::Zero")
        check(z is not None and z["count"] == 50, "zero-length zones counted (%s)" % (z and z["count"]))
        c = stats.get("T5::Colored")
        check(c is not None and c["count"] == 20, "colored zones recorded (%s)" % (c and c["count"]))
        messages = csvexport(args.tracy_dir, "-m", trace)
        check("T5 plain message" in messages and "T5 orange message" in messages, "messages recorded")
        plots = csvexport(args.tracy_dir, "-u", "-p", "-f", "T5", trace)
        check("Server: T5::Heap" in plots, "plot recorded under its track prefix 'Server: T5::Heap'")
        st, ct = stats.get("T6::ServerTick"), stats.get("T6::ClientTick")
        check(ct is not None and ct["files"] == {"Roblox Client 1"}, "client zones on the 'Roblox Client 1' track")
        check(st is not None and st["count"] == 10 and ct is not None and ct["count"] == 10, "relayed zones all arrived")
        s_starts = zone_starts(args.tracy_dir, trace, "T6::ServerTick")
        c_starts = zone_starts(args.tracy_dir, trace, "T6::ClientTick")
        skew = max((abs(a - b) for a, b in zip(s_starts, c_starts)), default=None)
        check(skew is not None and skew <= 1 and len(s_starts) == 10,
              "clockOffset aligns client with server (max skew %s ns)" % skew)
        check(frames.get("Client 1", 0) >= 9, "client frames in the 'Client 1' frame set (%s)" % frames.get("Client 1"))
        check("Client 1: T6::Ping" in csvexport(args.tracy_dir, "-u", "-p", "-f", "T6", trace),
              "client plot recorded as 'Client 1: T6::Ping'")
        for name in ("Imported Main", "Imported Worker"):
            z = stats.get("N1::Scope, " + name)
            check(z is not None and z["count"] == 5 and z["files"] == {name},
                  "named track '%s' has its 5 zones (%s)" % (name, z and (z["count"], sorted(z["files"]))))
        w = stats.get("T8::Worker")
        check(w is not None and w["count"] == 4 * 20 * 25, "concurrent sessions: all %d zones (%s)" % (
            4 * 20 * 25, w and w["count"]))

    print("protocol tests")
    bridge = Bridge(args.bridge)
    try:
        test_gap()
        test_resync()
        test_per_batch_notes()
        test_malformed()
        test_content_type()
        test_late_batch()
        test_fuzz()
        check(bridge.alive(), "bridge still running after all of the above")
        s = totals()
        check(s["malformed"] > 0, "malformed batches were counted (%d)" % s["malformed"])
    finally:
        bridge.stop()

    if have_tracy:
        print("recording tests")
        recording_tests(args)

    print("\n%s" % ("ALL PASSED" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
