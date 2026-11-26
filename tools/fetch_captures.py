"""Downloads MicroProfiler captures that Tracy.Capture's DataStoreSink stored in a live game.

    python tools/fetch_captures.py --universe UNIVERSE_ID [--datastore TracyCaptures]
                                   [--out captures] [--convert] [--delete]

The Open Cloud API key comes from the ROBLOX_API_KEY environment variable (or the one
--key-env names), never from the command line. Create it in Creator Hub > Open Cloud >
API Keys with the "universe-datastores.objects" read permission for your experience
(add delete if you use --delete), and set it in your own shell:

    $env:ROBLOX_API_KEY = "<your key>"      # PowerShell
    export ROBLOX_API_KEY="<your key>"      # bash / zsh

Each capture becomes OUT/<id>.gprx with its info in OUT/<id>.json. --convert then runs
`tracy-roblox-bridge --convert` on each new one, for OUT/<id>.tracy (that needs Lute,
LibMP and tracy-capture; see the README). --delete removes a capture from the DataStore
once it is safely on disk. Captures already in OUT are skipped.

Standard library only.
"""
import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

API = "https://apis.roblox.com"


class OpenCloud:
    def __init__(self, base, universe, datastore, key):
        self.root = "%s/datastores/v1/universes/%s/standard-datastores/datastore" % (base.rstrip("/"), universe)
        self.datastore = datastore
        self.key = key

    def _request(self, method, path, params):
        query = urllib.parse.urlencode(dict(params, datastoreName=self.datastore))
        req = urllib.request.Request("%s%s?%s" % (self.root, path, query), method=method,
                                     headers={"x-api-key": self.key})
        for attempt in range(6):
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    return r.read()
            except urllib.error.HTTPError as e:
                if e.code == 429 or e.code >= 500:
                    time.sleep(min(30, 2 ** attempt))  # rate limited or a hiccup: back off and retry
                    continue
                body = e.read().decode("utf-8", "replace")[:300]
                if e.code in (401, 403):
                    raise SystemExit("Open Cloud refused the API key (%d): %s\nCheck that the key has DataStore "
                                     "read access to universe %s (and delete access for --delete)."
                                     % (e.code, body, self.root.split("/universes/")[1].split("/")[0]))
                raise RuntimeError("%s %s: HTTP %d %s" % (method, path, e.code, body))
        raise RuntimeError("%s %s: still rate limited after several retries" % (method, path))

    def keys(self):
        cursor = ""
        while True:
            params = {"limit": "100"}
            if cursor:
                params["cursor"] = cursor
            page = json.loads(self._request("GET", "/entries", params))
            for entry in page.get("keys", []):
                yield entry["key"]
            cursor = page.get("nextPageCursor") or ""
            if not cursor:
                return

    def get(self, key):
        # A stored Luau string comes back as a JSON string.
        return json.loads(self._request("GET", "/entries/entry", {"entryKey": key}))

    def delete(self, key):
        self._request("DELETE", "/entries/entry", {"entryKey": key})


def find_bridge(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    exe = "tracy-roblox-bridge" + (".exe" if os.name == "nt" else "")
    for candidate in (os.path.join(here, "..", "build", exe), os.path.join(here, exe)):
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)
    return shutil.which("tracy-roblox-bridge")


def main():
    ap = argparse.ArgumentParser(description="Download Tracy.Capture captures from a DataStore (Open Cloud).")
    ap.add_argument("--universe", required=True, help="universe (experience) id")
    ap.add_argument("--datastore", default="TracyCaptures", help="DataStore name given to DataStoreSink")
    ap.add_argument("--out", default="captures", help="folder to save captures in")
    ap.add_argument("--convert", action="store_true", help="convert each new capture to .tracy")
    ap.add_argument("--delete", action="store_true", help="delete captures from the DataStore once saved")
    ap.add_argument("--bridge", help="tracy-roblox-bridge, for --convert (default: build/ or PATH)")
    ap.add_argument("--key-env", default="ROBLOX_API_KEY", help="environment variable holding the API key")
    ap.add_argument("--base-url", default=API, help=argparse.SUPPRESS)  # tests point this at a mock
    args = ap.parse_args()

    key = os.environ.get(args.key_env)
    if not key:
        raise SystemExit("Set %s to an Open Cloud API key first (see --help)." % args.key_env)
    bridge = find_bridge(args.bridge) if args.convert else None
    if args.convert and not bridge:
        raise SystemExit("--convert needs tracy-roblox-bridge: pass --bridge PATH")

    cloud = OpenCloud(args.base_url, args.universe, args.datastore, key)
    os.makedirs(args.out, exist_ok=True)
    keys = list(cloud.keys())
    ids = sorted(k for k in keys if "/" not in k)
    have_parts = set(k for k in keys if "/" in k)
    print("%d capture(s) in DataStore %s" % (len(ids), args.datastore))
    saved = failed = 0
    for cid in ids:
        gprx = os.path.join(args.out, cid + ".gprx")
        if os.path.exists(gprx):
            print("  %s: already downloaded" % cid)
            continue
        try:
            info = json.loads(cloud.get(cid))
            parts = int(info.get("Parts", 0))
            missing = [n for n in range(1, parts + 1) if "%s/%d" % (cid, n) not in have_parts]
            if parts < 1 or missing:
                raise RuntimeError("incomplete: parts %s missing" % missing)
            data = b"".join(base64.b64decode(cloud.get("%s/%d" % (cid, n))) for n in range(1, parts + 1))
            if len(data) != int(info.get("Bytes", -1)):
                raise RuntimeError("size mismatch: got %d bytes, expected %s" % (len(data), info.get("Bytes")))
        except Exception as e:  # keep going with the other captures
            print("  %s: FAILED (%s)" % (cid, e))
            failed += 1
            continue
        with open(gprx + ".part", "wb") as f:
            f.write(data)
        os.replace(gprx + ".part", gprx)
        with open(os.path.join(args.out, cid + ".json"), "w", encoding="utf-8") as f:
            json.dump(info, f, indent=2)
        saved += 1
        print("  %s: %.1f MB, %s, %s frames, place version %s" % (
            cid, len(data) / 1048576, info.get("Reason"), info.get("Frames"), info.get("PlaceVersion")))
        if args.delete:
            for n in range(1, parts + 1):
                cloud.delete("%s/%d" % (cid, n))
            cloud.delete(cid)
        if args.convert:
            out = os.path.join(args.out, cid + ".tracy")
            r = subprocess.run([bridge, "--convert", gprx, out])
            if r.returncode != 0:
                print("  %s: conversion failed (the .gprx is kept)" % cid)
                failed += 1
    print("saved %d, failed %d" % (saved, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
