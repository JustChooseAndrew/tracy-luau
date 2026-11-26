# Benchmarks

**Setup.** Studio's server VM running `bench/Overhead.server.luau`, against the real
bridge and Tracy 0.15, on a Windows 10 desktop. Expect ±10% between runs.

**To run it:** put the script in ServerScriptService, start the bridge, and press Play.
The report prints in Output after about 12 seconds.

## 500 zones per frame

| Caller | Total per frame |
|---|---|
| Ordinary script, `Tracy.ZoneBegin(...)` | 0.10 ms |
| Ordinary script, cached locals | 0.095 ms |
| `--!native` script, cached locals | 0.09 ms |
| Tracy off | ~0 |

That's about 0.6% of a 60 FPS frame.

Every zone arrives: 330,000 of 330,000, and 1.8 million at 5,000 zones per frame.

## Where the time goes

| Cost | ns | Per zone |
|---|---|---|
| `os.clock()` | 22–25 | 2× |
| Calling a Luau function | ~20 | 2× |
| Buffer writes for one event | 5–11 | 2× |
| Name → id lookup | 4 | 1× |

- **The floor.** Two timestamps and two calls put the minimum at about 0.05 ms per 500
  zones. Luau has no cheaper timer or call.
- **How close Tracy gets.** The module runs within about 10 ns per call of that floor.
- **Native callers.** A `--!native` caller saves about 8 ns per call.

## Sending

- **Server requests.** Each HttpService request costs the game thread about 0.25 ms,
  plus about 1 µs per KB. At the default flush (every 0.25 s) that's about 0.03 ms per
  frame and 240 requests a minute.
- **Client flushes.** A client flush (a RemoteEvent) costs about 0.02 ms.
- **Round trip.** It's about 15 ms, off the game thread.
- **First request.** A playtest's first request takes 1–2 s. Events buffer in the
  meantime.
- **`localhost`.** It adds about 1.8 s per request on Windows, so use `127.0.0.1`.

## Failures

| Test | Result |
|---|---|
| 5,000 zones per frame (~6 MB/s) | Per-call cost unchanged. All 1.8 million zones arrived |
| Buffer cap hit (64 KB cap, 100 KB of events per frame) | Batches dropped cleanly, with one warning; the track stayed balanced |
| Bridge killed for 3 s | Reconnected with no events lost |
| Bridge restarted instantly | One `resync`, then streaming continued |

## Other measurements

| | |
|---|---|
| A `debug.profilebegin` + `profileend` pair (the "Live" mirror) | 166 ns |
| Leak-safe zone tracking | 2–6 ns per call |
| `--record` | The saved file matches what the live viewer captured |
| A MicroProfiler dump (11.8 MB, 68 threads) | Converts in about 7 s, with all 187,719 zones |
| A `Tracy.Capture` of 48 busy frames | About 10–12 MB; blocks one frame 44–95 ms |

## MicroProfiler captures

- **Bound the history, then capture it all.** Asking LibMP for a frame count returns
  fewer frames than asked, so `Tracy.Capture` limits the history with `SetFrameLimit`
  and captures everything in it.
- **Give it enough memory.** Busy frames overflow LibMP's default memory budget, and the
  captures come back with no frames. 256 MB holds 64 busy frames.
- **Wait out the warm-up.** Captures in the first ~10 s after enabling the profiler come
  back hollow.
