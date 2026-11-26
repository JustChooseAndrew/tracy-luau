# Benchmarks

Every measurement here comes from Roblox Studio's server VM, with
`bench/Overhead.server.luau` running against the real bridge and Tracy 0.15. The test
machine was a Windows 10 desktop. Numbers move ±10% between runs. The timer alone
varied from 21.6 to 26.2 ns per call over the session.

## How the benchmark works

- **The timed loops.** Each frame, the benchmark times several loops of 500 iterations
  in a rotating order:

  | Loop | What it runs |
  |---|---|
  | `loop` | the bare loop. Subtracted from all the others |
  | `clock` | two `os.clock()` calls. This is the timer cost every zone pays |
  | `noop` | two calls to empty functions. This is what markers cost when Tracy is off |
  | `tracy` | `Tracy.ZoneBegin(name)` + `Tracy.ZoneEnd()` |
  | `tracyLocal` | the same calls through cached locals (`local ZoneBegin = Tracy.ZoneBegin`) |

- **One recorded set per frame.** Each frame runs only one of the two Tracy loops, so
  it records exactly 500 zones. That keeps the flush cost equal to what a game with
  500 zones per frame would pay.
- **Flush cost.** It comes from `Tracy.GetStats()` over the run and is amortized per
  frame.
- **Run length.** Each run is 60 warmup frames plus 600 measured frames.
- **Completeness.** A `tracy-capture` recording checks that every zone reached Tracy.
  Runs marked "complete" had 330,000 of 330,000.

Run it yourself:
1. Put the script in ServerScriptService.
2. Start the bridge.
3. Press Play.
4. After about 12 s, the report prints in the Output window.

## Result: 500 zones per frame

| Caller | Zone calls | Flush, per frame | **Total per frame** |
|---|---|---|---|
| Ordinary game script, `Tracy.ZoneBegin(...)` | 0.067 ms | 0.031 ms | **0.098 ms** |
| Ordinary game script, cached locals | 0.064 ms | 0.031 ms | **0.095 ms** |
| `--!native` game script, cached locals | 0.057 ms | 0.033 ms | **0.090 ms** |
| Tracy disabled (no-op functions) | ~0 | none | **~0** |

That is about 0.6% of a 16.7 ms frame. **It misses the Phase 2 target of < 0.05 ms**, and
the next section explains why that target isn't reachable in Luau.

Before Phase 2, the same setup measured 0.104–0.108 ms (ordinary caller) and
0.100–0.105 ms (native caller). Nearly all of the gain is in flushing.

## Where the time goes

Each cost below was measured separately in native code (`.dev` cost probe):

| Cost | ns | Per zone |
|---|---|---|
| `os.clock()` | 22.5 | 2× (begin + end) |
| Calling a Luau function, even native to native | ~20 | 2× |
| Buffer writes for one event | ~5–11 | 2× |
| Name → id lookup | 3.7 | 1× |

A zone needs two timestamps and two calls. That puts the floor at roughly 90–100 ns
per zone, or about 0.05 ms per 500 zones, before anything is sent.

- **`os.clock()`** is the only high-resolution timer Luau has. It is an ordinary C call,
  not a fast-path builtin.
- **The call cost** is what the Luau VM charges for any function call.

The implementation now runs within about 10 ns per call of that floor.

**Native vs. interpreted.** This matrix ran before the final tuning, at a 0.15 s flush:

| Module | Caller | ns per call (`Tracy.X` / locals) |
|---|---|---|
| native | interpreted | 66.8 / 62.7 |
| interpreted | interpreted | 83.1 / 79.9 |
| native | native | 59.4 / 54.7 |
| interpreted | native | 89.5 / 86.1 |

The module stays `--!native`. A game script compiled with `--!native` saves about 8 ns
per call on top.

## Flushing

Every batch is one `HttpService:RequestAsync`. Its cost on the game thread:

| Batch | `buffer.readstring` | `RequestAsync` until it yields |
|---|---|---|
| 1 KB | 0.002 ms | 0.25 ms |
| 16 KB | 0.006 ms | 0.25 ms |
| 180 KB | 0.056 ms | 0.42 ms |
| 1 MB | 0.32 ms | 0.96 ms |

Each request costs about 0.25 ms of fixed overhead, plus about 1 µs per KB.

What didn't help:
- **Request style.** A reused request table saved about 0.035 ms on small batches and
  nothing on large ones. Removing headers saved nothing. `PostAsync` was no cheaper.

What did help:
- **Fewer requests.** The default `FlushInterval` went from 0.15 s to 0.25 s. That is 4
  requests a second, matching the original plan's "4–6 times per second". Flush cost
  dropped from 0.041 to 0.031 ms per frame, and HttpService use fell from about 400 to
  240 requests a minute.

## HttpService findings

- **Body size.** There is no limit up to at least 32 MB. Every size arrived byte-exact,
  binary included.
- **Round trip to the bridge.** About 15 ms mean, 17 ms at worst during steady
  streaming. This happens off the game thread.
- **First request of a playtest.** It takes 1.2–2 s, which looks like Windows network
  setup. Events buffer in the meantime, and nothing is lost.
- **`localhost` vs `127.0.0.1`.** `localhost` added about 1.8 s per request, so the
  default URL is `127.0.0.1`.
- **A closed port.** Connecting to a closed localhost port takes about 2 s to fail on
  Windows. That covers short bridge restarts with no data loss (see Recovery).

## Load and failure tests

| Test | Result |
|---|---|
| Stress: 5,000 zones per frame (~6 MB/s) | Per-call cost unchanged (55–60 ns). Flush 1.75 ms per 1.5 MB batch, 0.14 ms per frame. Round trip 14 ms mean, 16 ms at worst. All 1,800,000 zones reached Tracy |
| Overflow: 64 KB buffer cap, 100 KB of events per frame | One warning. 454 batches dropped cleanly, zero malformed batches, zero unknown names. The bridge counted the gaps and kept the track balanced |
| Bridge killed for 3 s mid-run | One failed request, then automatic reconnect. No events dropped: the failing request took about 2 s, the events were kept, and they were sent on reconnect |
| Bridge restarted instantly (it lost the name table) | One `resync` round. Luau re-sent its names and streaming continued |
| `tools/test_bridge.py` | 31 checks. Exact timings, zero-length zones, 4 concurrent sessions on 4 tracks, gaps, resync, 11 malformed-input cases, late batches, 3,600 fuzzed batches (600 of them while a capture was recording) |

## Phase 3 additions

| | Measured |
|---|---|
| 500 zones per frame, ordinary caller, built-in plots on | 0.091–0.096 ms per frame total. The colored-zone branch and plots added no measurable cost |
| Client flush (seal + copy + `RemoteEvent:FireServer`) | ~0.018 ms, against the server's ~0.25 ms HTTP request |
| Client → server RemoteEvent throughput in Studio, buffer payloads | 2 MB/s with 17 ms lag, nothing lost (tested up to 512 KB per send) |
| Client/server clock: Play Solo | Both VMs share one clock (same process). The sync measures 0, so tracks align exactly |
| Bridge alignment test, with a client clock 12.5 s off | 0 ns skew between paired server and client zones |

## Phase 4: recording and import

| | Measured |
|---|---|
| `--record` in Studio (server + client demo, 10 s, live viewer connected the whole time) | The `.tracy` it wrote matched the live viewer's capture exactly: same zones per track, counts and mean durations, same 27 messages. The bridge saved and exited 0.3 s after Ctrl+Break, with the viewer still attached |
| `--convert` | 41 requests, 12.4 s of profiling, converted in about a second |
| MicroProfiler dump from the Studio playtest (11.8 MB, 68 threads, 92 frames) | `gprx2tlrec` took 5.4 s under Lute and wrote a 5.9 MB `.tlrec`. That converted to a 1.3 MB `.tracy` with all 187,719 zones on 27 tracks, 106,617 labels as zone text, 506 GPU scopes placed on the CPU timeline, and 9 frame-stat plots |

## Bugs found by measuring

- **Tracy drops zero-length zones from its statistics.** `os.clock()` ticks every
  100 ns, so a short zone can begin and end on the same tick. In the first capture
  about 46% of zones went missing. The bridge now gives such zones 1 ns, which is well
  inside the timer's error, and the counts are exact.
- **Garbage timestamps.** A NaN, infinite, or absurd value would have dragged a whole
  track to the end of time. The bridge now rejects the batch. Plot samples that are NaN
  or infinite are skipped.
- **Allocations on failure.** Recovering from a dropped batch, or idling while the
  bridge was down, allocated a new buffer every flush. The batch is now rebuilt in
  place.
- **Replayed recordings had no zones.** A batch's events happen before its seal time,
  so starting the replay clock right at the capture's connection put each session's
  first events before the trace began, and Tracy dropped them. A pre-scan now leaves
  room for the earliest event.
- **Commas in zone names.** MicroProfiler names like
  `::PowerSelector, MainManager::step` broke naive parsing of `tracy-csvexport` output,
  which looked like lost zones. The tests now read CSV rows from the right.
