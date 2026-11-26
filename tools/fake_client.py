"""Sends synthetic Tracy-Luau batches to the bridge, so the bridge can be tested without Roblox.

    python tools/fake_client.py [--url http://127.0.0.1:8080/ingest] [--seconds 3]

Each fake 60 Hz frame holds "Fake::Update" (2.0 ms) with "Fake::Inner" (1.0 ms) inside it,
plus a "Fake::Value" plot. Timestamps are synthetic, so Tracy should show those exact
durations. Exits non-zero if the bridge rejects a batch.
"""
import argparse
import random
import struct
import sys
import time
import urllib.request

MAGIC = 0x41554C54
VERSION = 2
HEADER = "<IBBHIIddI"  # magic, version, source, flags, session, sequence, flushClock, clockOffset, length
HEADER_SIZE = 36
SOURCE_SERVER = 0
FLAG_FINAL = 1

OP_REGISTER_STRING = 0x01
OP_ZONE_BEGIN = 0x02
OP_ZONE_END = 0x03
OP_FRAME_MARK = 0x04
OP_PLOT_VALUE = 0x05
OP_FRAME_MARK_NAMED = 0x06
OP_ZONE_BEGIN_COLOR = 0x07
OP_MESSAGE = 0x08
OP_PLOT_CONFIG = 0x09

PLOT_NUMBER, PLOT_MEMORY, PLOT_PERCENTAGE = 0, 1, 2


class Batch:
    def __init__(self):
        self.parts = []

    def register(self, sid, text):
        raw = text.encode("utf-8")
        self.parts.append(struct.pack("<BHH", OP_REGISTER_STRING, sid, len(raw)) + raw)

    def zone_begin(self, sid, t):
        self.parts.append(struct.pack("<BHd", OP_ZONE_BEGIN, sid, t))

    def zone_end(self, t):
        self.parts.append(struct.pack("<Bd", OP_ZONE_END, t))

    def frame_mark(self, t):
        self.parts.append(struct.pack("<Bd", OP_FRAME_MARK, t))

    def plot(self, sid, t, value):
        self.parts.append(struct.pack("<BHdd", OP_PLOT_VALUE, sid, t, value))

    def frame_mark_named(self, sid, t):
        self.parts.append(struct.pack("<BHd", OP_FRAME_MARK_NAMED, sid, t))

    def zone_begin_color(self, sid, t, color):
        self.parts.append(struct.pack("<BHdI", OP_ZONE_BEGIN_COLOR, sid, t, color))

    def message(self, t, text, color=0):
        raw = text.encode("utf-8")
        self.parts.append(struct.pack("<BdIH", OP_MESSAGE, t, color, len(raw)) + raw)

    def plot_config(self, sid, fmt=PLOT_NUMBER, step=False, fill=True, color=0):
        flags = (1 if step else 0) | (2 if fill else 0)
        self.parts.append(struct.pack("<BHBBI", OP_PLOT_CONFIG, sid, fmt, flags, color))

    def raw(self, data):
        self.parts.append(data)

    def encode(self, session, seq, flush_clock, flags=0, source=SOURCE_SERVER, clock_offset=0.0):
        events = b"".join(self.parts)
        header = struct.pack(HEADER, MAGIC, VERSION, source, flags, session, seq, flush_clock, clock_offset,
                             len(events))
        assert len(header) == HEADER_SIZE
        return header + events


def post(url, body):
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080/ingest")
    ap.add_argument("--seconds", type=float, default=3.0)
    args = ap.parse_args()

    session = random.getrandbits(32)
    clock0 = time.perf_counter()
    frame = 1 / 60
    frames = int(args.seconds / frame)
    seq = 0
    batch = Batch()
    batch.register(1, "Fake::Update")
    batch.register(2, "Fake::Inner")
    batch.register(3, "Fake::Value")
    sent_frames = 0
    for i in range(frames):
        t = clock0 + i * frame
        batch.zone_begin(1, t)
        batch.zone_begin(2, t + 0.0005)
        batch.zone_end(t + 0.0015)
        batch.zone_end(t + 0.0020)
        batch.plot(3, t + 0.0020, float(i % 30))
        batch.frame_mark(t + frame)
        if (i + 1) % 9 == 0 or i == frames - 1:
            # pace like a real game: never send a frame before it "happened"
            due = clock0 + (i + 1) * frame
            while time.perf_counter() < due:
                time.sleep(0.001)
            final = i == frames - 1
            status, text = post(args.url, batch.encode(session, seq, time.perf_counter(), FLAG_FINAL if final else 0))
            if status not in (200, 204):
                print("bridge rejected batch %d: HTTP %d %s" % (seq, status, text))
                return 1
            if status == 200 and text == "resync":
                print("bridge asked for a resync after batch %d" % seq)
                return 1
            seq += 1
            sent_frames = i + 1
            batch = Batch()
    print("sent %d frames in %d batches, session %08x" % (sent_frames, seq, session))
    return 0


if __name__ == "__main__":
    sys.exit(main())
