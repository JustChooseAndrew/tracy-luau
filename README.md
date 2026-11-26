# Tracy-Luau

Profile Roblox games in [Tracy](https://github.com/wolfpld/tracy). Put zone markers in
your Luau, press Play in Studio, and watch the server and each client land on Tracy's
timeline on aligned tracks. Zones keep the durations Luau measured, and plots and
messages come along with them.

In published games the same markers go to Roblox's MicroProfiler. Captures from live
servers, holding your zones and the engine's own work, come back to your machine as
Tracy traces.

> **Status: 0.5.0 (module and bridge).**
> - **Studio:** streaming, recording to `.tracy`, and leak-safe zones.
> - **Wrap and Instrument:** add zones without writing ZoneBegin/ZoneEnd pairs.
> - **Live games:** zones are mirrored into the MicroProfiler, and live-game captures
>   are turned into Tracy traces.
> - **Release tooling:** Wally and Pesde manifests and CI.
>
> 500 zones per frame cost about 0.1 ms. The Studio plugin comes next. See
> [Roadmap](#roadmap).

```
Studio client VM ─RemoteEvent─┐
                              ▼
Studio server VM ────────── HTTP POST ──► tracy-roblox-bridge ──TCP──► Tracy 0.15
 Tracy.luau: buffer,         127.0.0.1     replays the events with       viewer
 interning, batches          :8080         the Luau timestamps
 every ~250 ms                             through Tracy's own client

Live server ── Tracy.Capture ── DataStore ──► fetch_captures.py ──► .gprx ──► bridge --convert ──► .tracy
 (zones mirrored into the MicroProfiler; a capture on each hitch, via LibMP)
```

- **Why the bridge.** Roblox can't open raw sockets, so the bridge receives batches
  over HTTP. It feeds them to Tracy's own client library, which handles the connection
  to the viewer.
- **Why clients go through the server.** Client scripts can't make HTTP requests, so
  clients send their batches to the server over a RemoteEvent. The server forwards them
  inside its own requests.
- **Timing.** Every event carries the `os.clock()` time it happened, not the time it
  reached the bridge. Clients sync their clocks to the server, so all tracks share one
  timeline.

## Install

**The Luau module** is a ModuleScript `Tracy` with a child `Capture`. Pick one way to
add it:

| How | |
|---|---|
| Wally | `Tracy = "justchooseandrew/tracy-lua@0.5.0"` under `[dependencies]` |
| Pesde | `pesde add justchooseandrew/tracy_lua` (Pesde names can't contain hyphens) |
| Model file | drag `Tracy.rbxm` from a release into `ReplicatedStorage` |
| Rojo / by hand | sync `default.project.json`, or copy `src/Tracy/init.luau` into a ModuleScript `Tracy` and `src/Tracy/Capture.luau` into a child ModuleScript `Capture` |

**The bridge**, from a release:

| Platform | Download | Contains |
|---|---|---|
| Windows | `tracy-roblox-bridge-windows-x64.zip` | the bridge, plus the matching Tracy 0.15.0 viewer and `tracy-capture` |
| macOS (Apple silicon and Intel) | `tracy-roblox-bridge-macos-universal.tar.gz` | the bridge |
| Linux | `tracy-roblox-bridge-linux-x64.tar.gz` | the bridge |

**The viewer.** Tracy publishes Windows binaries only. On macOS and Linux, build the
0.15.0 viewer (`profiler/`) and `tracy-capture` (`capture/`) from the
[Tracy v0.15.0 source](https://github.com/wolfpld/tracy/releases/tag/v0.15.0) with
CMake. The viewer must be 0.15.x, the version the bridge is built with; other versions
refuse the connection.

**Building the bridge yourself.**
- Windows: Visual Studio 2022 Build Tools (C++) and CMake, then
  `powershell -ExecutionPolicy Bypass -File bridge\build.ps1`.
- macOS and Linux: `cmake -S bridge -B build -DCMAKE_BUILD_TYPE=Release` and
  `cmake --build build`.

## Quick start (Studio)

1. **Add the module** to `ReplicatedStorage` (see [Install](#install)).
2. **Allow HTTP.** Game Settings → Security → Allow HTTP Requests.
3. **Start** `tracy-roblox-bridge`, open the Tracy viewer and connect to `127.0.0.1`.
4. **Press Play.** To save the session as a file too, start the bridge with
   `--record session.tracy`; see [Saving a session](#saving-a-session---record).

**Examples.**
- **Timed demos:** `examples/ServerDemo.server.luau` (ServerScriptService) and
  `examples/ClientDemo.client.luau` (StarterPlayerScripts). Their zones busy-wait for
  known times, so you can check Tracy's numbers against the code. They also send a
  ping each second that shows the two tracks lining up.
- **A game-like workload:** `examples/Fountain.server.luau` and
  `examples/Fountain.client.luau` run a part fountain with no busy-waits, so the zones
  and plots follow real costs.
- **Live captures:** `examples/LiveCaptures.server.luau` (see
  [In live games](#in-live-games)).

**Profiling client scripts.** Require Tracy from at least one server Script as well.
The server creates the relay that carries client data. Without it, the client warns
after 30 s and drops its data.

## API

```lua
local Tracy = require(ReplicatedStorage.Tracy)

Tracy.ZoneBegin("Enemies")              -- open a zone (use a fixed name)
Tracy.ZoneBegin("AI", 0xFF5500)         -- with a color (0xRRGGBB)
Tracy.ZoneEnd()                         -- close the most recent zone
Tracy.FrameMark()                       -- end of frame (once per Heartbeat / RenderStepped)
Tracy.FrameMark("Physics")              -- a separate, named frame set

-- Zones without Begin/End pairs. Arguments and results pass through.
local update = Tracy.Wrap("Enemies", updateEnemies)          -- a function, wrapped
Tracy.Instrument(EnemyService, "EnemyService")               -- every function in a table,
                                                             -- as "EnemyService.Update", ...

Tracy.Plot("Enemies alive", #enemies)   -- a sample on a named plot
Tracy.ConfigurePlot("Cache", {          -- optional, once per plot
	Format = "Memory",                  -- "Number" (default), "Memory" (bytes), "Percentage"
	Step = true, Fill = false, Color = 0x33AAFF,
})
Tracy.Message("Wave 3 started")         -- text marker on this VM's track
Tracy.Message("Boss spawned", 0xFF0000) -- with a color

Tracy.Configure({
	Url = "http://127.0.0.1:8080/ingest", -- the bridge's --port must match (server only)
	FlushInterval = 0.25,                  -- seconds between batches (min 0.125)
	FlushBytes = 512 * 1024,               -- send early past this much (at most every 0.2 s)
	RetryInterval = 2,                     -- seconds between reconnect attempts
	MaxBufferSize = 4 * 1024 * 1024,       -- unsent bytes held before dropping a batch
	BuiltinPlots = true,                   -- Lua heap, instances, ping at 10 Hz
})

Tracy.IsActive()     -- streaming? (Studio, running, enabled)
Tracy.IsConnected()  -- reaching the bridge? (on a client: as reported by the server)
Tracy.GetStats()     -- batches, bytes, flush time, round trips, clients, clock offset,
                     -- ZonesLeaked, StrayEnds
```

**Built-in plots.** Each VM plots these 10 times a second. Turn them off with
`BuiltinPlots = false`.

| Plot | Source |
|---|---|
| `Lua heap` | `gcinfo()`, shown as memory |
| `Instances` | `Stats.InstanceCount` |
| `Ping (ms)` on a client | `GetNetworkPing()` |
| `Ping Client N (ms)` on the server, one per player | `GetNetworkPing()` |

**Names in Tracy.** Each VM gets its own track: `Roblox Server`, `Roblox Client 1`, and
so on. Plots and named frame sets are global in Tracy, so they carry the track's name,
like `Server: Lua heap` or `Client 1: Enemies alive`.

**Fastest form.** Cache the functions in locals.

```lua
local ZoneBegin, ZoneEnd = Tracy.ZoneBegin, Tracy.ZoneEnd
```

**Zone rules, and what happens when they're broken.**
- **Close every zone** in the function that opened it, on every path, early returns
  included.
- **Don't yield** (`task.wait`, `WaitForChild`, remote calls, HTTP) inside a zone.
  Other code runs during the yield and its zones nest inside yours.
- **Use fixed names.** A name built per call, like `"Enemy " .. i`, registers a new
  string every time.

When a zone is still open at the end of a frame, because it yielded or errored before
`ZoneEnd`:
- Tracy closes it, notes in the viewer that its duration runs to that point, and warns
  once in Output with the zone's name.
- The zone's late `ZoneEnd` is ignored, so it can't close someone else's zone.
- `GetStats().ZonesLeaked` and `StrayEnds` count both.

`Wrap` and `Instrument` get this for free, including when the wrapped function errors.

**Switches.** Each one is a constant at the top of the module, or an attribute on the
`Tracy` ModuleScript. The attribute wins, which suits Rojo and Wally users.

| Constant | Attribute | Effect |
|---|---|---|
| `ENABLED = false` | `Enabled` (bool) | nothing streams, in Studio too |
| `MICROPROFILER` | `MicroProfiler` (string) | where zones go besides Tracy: `"Live"`, `"Always"` or `"Off"`, below |

## In live games

Live servers can't reach the bridge on your machine, so Tracy-Luau works with Roblox's
MicroProfiler there instead.

### Your zones in the MicroProfiler

Outside Studio, `ZoneBegin`/`ZoneEnd` *are* `debug.profilebegin`/`debug.profileend`,
and `Wrap`/`Instrument` use them too. So your zones appear in every MicroProfiler view
and capture of a live server or client: the in-game MicroProfiler, dumps, and the
captures below.

| `MICROPROFILER` | Zones go to the MicroProfiler... | Cost outside Studio |
|---|---|---|
| `"Live"` (default) | whenever Tracy isn't streaming: published games, and Studio with Tracy off | one `debug.profilebegin`/`profileend` pair per zone, ~0.17 µs measured in Studio |
| `"Always"` | in Studio playtests too, next to streaming | the same, added to Tracy's own cost in Studio |
| `"Off"` | never | nothing: every call is an empty function, and `Wrap` returns your function unchanged |

`FrameMark`, `Plot` and `Message` do nothing outside Studio.

### Captures from live servers (`Tracy.Capture`)

When a server frame takes too long, or when you ask, `Tracy.Capture` grabs the last few
dozen frames of the MicroProfiler and sends them out of the game. A capture holds:
- every engine thread (physics, replication, scripts, GC, workers);
- your mirrored zones;
- the frame stats.

It needs [LibMP](https://github.com/Roblox/libmp) (MIT), Roblox's MicroProfiler API,
as a ModuleScript in your game.

```lua
local Capture = require(ReplicatedStorage.Tracy.Capture)

Capture.Start({
	LibMP = require(ServerStorage.LibMP),
	Sink = Capture.DataStoreSink("TracyCaptures"), -- where captures go
	HitchMs = 150,       -- capture when a frame takes 150 ms or more (0 = only Now())
	Frames = 64,         -- frames of history, and so per capture
	Cooldown = 600,      -- seconds between automatic captures
	MaxCaptures = 3,     -- per server
	SampleRate = 0.05,   -- share of servers that turn the MicroProfiler on at all
	Warmup = 15,         -- seconds before the first capture (the history is filling)
	MemoryMB = 256,      -- the MicroProfiler's history budget
})

Capture.Now("boss fight started") -- on demand
Capture.GetStats()                -- Captures, Sent, Failed, LastId, LastError
```

**Sinks.**

| Sink | Where captures go |
|---|---|
| `Capture.DataStoreSink(name or DataStore)` | a DataStore. Nothing to host. Each capture is stored as base64 parts plus an info entry, and counts against the experience's DataStore storage and request budget |
| `Capture.HttpSink(url, { Headers, Secret })` | POSTed to a server you run. `Secret` names a Roblox secret sent as `Authorization: Bearer ...` |
| `Capture.BridgeSink()` | the bridge on this machine (Studio): saved under `captures/` and converted to `.tracy` automatically |
| any `function(info, data)` | your own; error to report failure |

**Getting DataStore captures.**
1. Create an Open Cloud API key with DataStore read access to the experience (and
   delete access for `--delete`).
2. Set it in your shell; the tool reads it from the environment, never from the
   command line:

   ```powershell
   $env:ROBLOX_API_KEY = "<your key>"
   ```

3. Fetch the captures:

   ```powershell
   python tools/fetch_captures.py --universe <universe id> --convert --delete
   ```

Each capture lands in `captures/` as `<id>.gprx` with its info in `<id>.json`.
- The info holds the reason, time, place version, frame time, and frames.
- `--convert` makes `<id>.tracy` too.
- `--delete` removes each capture from the DataStore once it's on disk.

**Costs and caveats.**
- **Memory while recording.** The MicroProfiler keeps up to `MemoryMB` of history,
  which is why `SampleRate` exists.
- **A capture stalls one frame** while it's compressed and checked. In Studio, with the
  whole Play Solo process and a busy scene, that was 45–95 ms for 48–64 frames of about
  10–12 MB.
- **Hollow captures.** If the history is too small for how busy the frames are, a
  capture comes back with few or no frames. `Tracy.Capture` checks for that and retries,
  and the converter says so if it still happens. Raise `MemoryMB` or lower `Frames`.
- **Tested in Studio only.** The capture path, the DataStore encoding, and the fetch
  tool were tested in Studio and against a local copy of Open Cloud's DataStore API.
  A real live server isn't tested yet. LibMP has been enabled in all live games since
  its October 2026 update.

## Saving a session (`--record`)

```powershell
tracy-roblox-bridge --record session.tracy
```

- **What it does.** The bridge streams as usual, and the live viewer keeps working.
  When you stop it with Ctrl+C, it writes `session.tracy` containing everything it
  received: every track, zone, frame, plot and message.
- **How it saves.** While running, it keeps a raw `session.tlrec` next to the output.
  It turns that into the `.tracy` file on exit, then deletes it. Pass `--keep-raw` to
  keep it.
- **If it doesn't finish.** If the bridge is killed, the `.tlrec` stays. Convert it
  with `tracy-roblox-bridge --convert session.tlrec session.tracy`.
- **What it needs.** `tracy-capture` from the same Tracy 0.15 release, next to the
  bridge (the Windows zip includes it), on PATH, or passed with `--capture-tool`.

## MicroProfiler dumps to Tracy

The bridge converts any MicroProfiler capture in one step:
- a `.gprx` from the MicroProfiler's Dump menu;
- an HTML dump;
- a `Tracy.Capture` capture.

```powershell
tracy-roblox-bridge --convert capture.gprx capture.tracy
```

It runs the importer, `tools/microprofiler/gprx2tlrec.luau` (in the release zips, next
to the bridge), which needs:
- [Lute](https://github.com/luau-lang/lute/releases), next to the bridge or on PATH,
  or passed with `--lute`;
- LibMP.luau from [Roblox/libmp releases](https://github.com/Roblox/libmp/releases),
  placed next to `gprx2tlrec.luau`.

| In the dump | In Tracy |
|---|---|
| each MicroProfiler thread (`RBX Main`, `RBX Worker A`, `GPU`, ...) | its own track |
| scopes, your mirrored zones included | zones, in their timer's color |
| scope labels (script names, place names, ...) | zone text |
| frames | frame marks |
| GPU scopes | placed on the CPU timeline, using each frame's start time on both clocks |
| core frame stats (`frame_stats/basic/*`) | plots |

You can also run the importer yourself:
`lute run tools/microprofiler/gprx2tlrec.luau capture.gprx capture.tlrec`, then
`--convert` the `.tlrec`. Its options:

| Option | Effect |
|---|---|
| `--counters TEXT` | also plot every counter whose path contains TEXT |
| `--all-counters` | plot every counter, often several hundred |
| `--no-counters` | no plots |
| `--no-gpu` | leave out the GPU track |
| `--no-labels` | leave out labels |
| `--verbose` | also print zones per thread |

## What it costs

Measured on Studio's server with `bench/Overhead.server.luau`. Full method and data are
in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

| 500 zones per frame | Total per frame |
|---|---|
| Studio, streaming to Tracy | **~0.1 ms** (0.11–0.13 ms in the 0.5 runs on a busier machine; 0.09–0.1 ms in Phase 2–3) |
| Published game, `MICROPROFILER = "Live"` | ~0.08 ms, estimated from the ~0.17 µs a `debug.profilebegin`/`profileend` pair cost in Studio |
| Published game, `MICROPROFILER = "Off"` | **~0** |

- **Clients pay less to send.** A client's flush is a RemoteEvent, about 0.02 ms,
  against the server's ~0.25 ms HTTP request.
- **The floor.** Each zone needs two `os.clock()` calls (22–25 ns each) and two
  function calls (about 20 ns each). Luau has no cheaper timer or call, so about
  0.05 ms per 500 zones is the minimum before anything is sent.
- **Leak safety** costs a few nanoseconds per call (2–6 ns in the quietest A/B run).

**What's verified.**
- Every zone arrives: 330,000 of 330,000 in a capture, and 1.8 million at 5,000 zones
  per frame.
- Durations match what Luau measured. A busy-wait coded at 1.0 ms reads 1.001 ms.
- MicroProfiler captures convert with every zone, in every container format LibMP
  writes. The same capture came out as exactly 675,402 zones from raw, compressed,
  ASCII and HTML forms.

The flush also appears as its own `Tracy::Flush` zone. That way the profiler's cost
is visible on the timeline next to your code.

## Limits in this version

- **Streaming is Studio only.** Live games use the MicroProfiler route above.
- **Clients need the server.** Client data only flows while a server Script has
  required Tracy.
- **Tested in Play Solo.** Studio's "Start Server + N Clients" mode should work, since
  clients are numbered in join order. So far it has only been exercised by the bridge's
  tests.
- **Pinned Tracy version.** The viewer must be Tracy 0.15.x. The bridge writes Tracy
  queue events directly, to keep the Luau timestamps, so it is tied to that version's
  internals.
- **Shared HTTP budget.** HttpService allows 500 requests a minute per server, and the
  game shares that budget. The default flush uses 240, and early flushes for busy games
  stay under 300. If the game hits the limit, Tracy pauses for 5 s and keeps buffering.
- **Use `127.0.0.1`, not `localhost`.** On Windows, `localhost` added close to two
  seconds per request in testing.
- **Timer resolution.** `os.clock()` ticks every 100 ns, so zones shorter than that
  read as 0–100 ns.
- **Untested dump sources.** Dumps saved from the MicroProfiler's own menu, and dumps
  from live servers, aren't tested yet. The formats LibMP writes (raw, compressed,
  ASCII, HTML) all convert, and the importer uses only LibMP's API.
- **Lute 1.0.** It lacks the `fs.readIntoBuffer` call that LibMP's `OpenFromFile`
  needs, so the importer loads the dump into memory. A 100 MB dump needs roughly that
  much RAM.

## Repository layout

| Path | What |
|---|---|
| `src/Tracy/init.luau` | the Luau module (server and client) |
| `src/Tracy/Capture.luau` | live-game MicroProfiler captures (needs LibMP) |
| `bridge/` | the bridge (C++17, CMake). Tracy 0.15.0 client and cpp-httplib 0.67.0 are vendored unmodified in `bridge/third_party/`, with their licenses |
| `docs/PROTOCOL.md` | wire format, relay messages, `/capture`, and the rules both sides rely on |
| `docs/BENCHMARKS.md` | measurements and method |
| `examples/` | timed demos, the part fountain, and live captures |
| `bench/Overhead.server.luau` | overhead benchmark; run it in Play |
| `tools/microprofiler/gprx2tlrec.luau` | MicroProfiler dump → `.tlrec` importer (Lute + LibMP) |
| `tools/fetch_captures.py` | downloads `Tracy.Capture` captures from a DataStore (Open Cloud) |
| `tools/test_bridge.py` | bridge tests: timings, colors, messages, plots, client tracks and clock alignment, named tracks, gaps, resync, malformed input, fuzzing, `/capture`, `--record` / `--convert` |
| `tools/test_fetch_captures.py` | `fetch_captures.py` against a mock of Open Cloud's DataStore API |
| `tools/fake_client.py` | sends synthetic batches, to test the bridge without Roblox |
| `.github/workflows/ci.yml` | lint, package check, bridge builds and tests on Windows, macOS and Linux; releases on `v*` tags |

## Testing

```powershell
python tools/test_bridge.py --tracy-dir path\to\tracy-0.15.0
```

```powershell
python tools/test_fetch_captures.py
```

`test_bridge.py` starts its own bridge on test ports (18080 and 18086), so it doesn't
disturb a bridge you already have running.
- **With the Tracy tools folder,** it also checks what Tracy recorded, and runs the
  recording tests.
- **With `--lute` and `--gprx CAPTURE`** (and LibMP next to the importer), it also
  checks MicroProfiler conversion, directly and through `/capture`.
- **Without them,** the HTTP, `/status` and `/capture` checks run. That's what CI does.

Luau style: `stylua --check src examples bench` and `selene src examples bench`. The
tool versions are in `aftman.toml`.

## Roadmap

| Phase | Scope | State |
|---|---|---|
| 1 | End-to-end proof: zones and frames from Studio into Tracy | **done** |
| 2 | Binary buffer, interning, batching, benchmarks, hardening | **done**: ~0.1 ms per 500 zones. The < 0.05 ms target is below Luau's floor ([why](docs/BENCHMARKS.md#where-the-time-goes)) |
| 3 | `Tracy.Plot`, `Tracy.Message`, zone colors, client VMs on their own tracks | **done** |
| 4 | `--record` to a `.tracy` file; MicroProfiler import | **done** |
| 4.5 | MicroProfiler mirroring, `Tracy.Capture` live captures, `Wrap`/`Instrument`, leak-safe zones | **done** |
| 5 | Release: CI-built Windows/macOS/Linux binaries, Wally and Pesde packages | tooling **done**; first release once the repository is on GitHub |
| Plugin | Studio toolbar button, connection light, auto-injecting the module | next |

## Trademarks and affiliation

- Luau is a trademark of Roblox Corporation. Roblox is a trademark of Roblox
  Corporation.
- **Independent project.** Tracy-Luau is not affiliated with or endorsed by Roblox, the
  Luau team, or the Tracy project.
- **Tracy** is developed by Bartosz Taudul and contributors
  ([wolfpld/tracy](https://github.com/wolfpld/tracy)).

## Licenses

- **Tracy-Luau** is MIT licensed ([LICENSE](LICENSE)).
- **Vendored code** keeps its own license: Tracy (BSD-3-Clause) and cpp-httplib (MIT),
  in `bridge/third_party/`. The Windows release also bundles Tracy's viewer and
  `tracy-capture` under Tracy's license.
- **LibMP** (MIT) isn't vendored. Each user downloads it.
