# Tracy-Luau guide

- [Installing](#installing)
- [API](#api)
- [Zone rules](#zone-rules)
- [Switches](#switches)
- [In live games](#in-live-games)
- [Saving a session](#saving-a-session)
- [MicroProfiler dumps to Tracy](#microprofiler-dumps-to-tracy)
- [What it costs](#what-it-costs)
- [Limits](#limits)

## Installing

**The Luau module** is a ModuleScript `Tracy` with a child `Capture`.

| How | |
|---|---|
| Wally | `Tracy = "justchooseandrew/tracy-luau@0.5.0"` under `[dependencies]` |
| Pesde | `pesde add justchooseandrew/tracy_luau` (Pesde names can't contain hyphens) |
| Model file | drag `Tracy.rbxm` from a release into `ReplicatedStorage` |
| Rojo / by hand | sync `default.project.json`, or copy `src/Tracy/init.luau` into a ModuleScript `Tracy` and `src/Tracy/Capture.luau` into a child ModuleScript `Capture` |

**The bridge**, from a release:

| Platform | Download | Contains |
|---|---|---|
| Windows | `tracy-roblox-bridge-windows-x64.zip` | the bridge, the Tracy 0.15.0 viewer and `tracy-capture` |
| macOS (Apple silicon and Intel) | `tracy-roblox-bridge-macos-universal.tar.gz` | the bridge |
| Linux | `tracy-roblox-bridge-linux-x64.tar.gz` | the bridge |

**The viewer** must be Tracy 0.15.x, the version the bridge is built with; other
versions refuse the connection. Tracy publishes Windows binaries only. On macOS and
Linux, build the viewer (`profiler/`) and `tracy-capture` (`capture/`) from the
[Tracy v0.15.0 source](https://github.com/wolfpld/tracy/releases/tag/v0.15.0) with
CMake.

To build the bridge yourself, see [DEVELOPING.md](DEVELOPING.md).

## API

```lua
local Tracy = require(ReplicatedStorage.Tracy)

Tracy.ZoneBegin("Enemies")              -- open a zone (use a fixed name)
Tracy.ZoneBegin("AI", 0xFF5500)         -- with a color (0xRRGGBB)
Tracy.ZoneEnd()                         -- close the most recent zone
Tracy.FrameMark()                       -- end of frame (once per Heartbeat / RenderStepped)
Tracy.FrameMark("Physics")              -- a separate, named frame set

local update = Tracy.Wrap("Enemies", updateEnemies)  -- a function, inside a zone
Tracy.Instrument(EnemyService, "EnemyService")       -- every function in a table,
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

`Wrap` and `Instrument` pass arguments and results through unchanged.

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

**Client scripts.** Require Tracy from at least one server Script as well. The server
creates the relay that carries client data. Without it, the client warns after 30 s and
drops its data.

**Fastest form.** Cache the functions in locals:
`local ZoneBegin, ZoneEnd = Tracy.ZoneBegin, Tracy.ZoneEnd`.

## Zone rules

- **Close every zone** in the function that opened it, on every path, early returns
  included.
- **Don't yield** (`task.wait`, `WaitForChild`, remote calls, HTTP) inside a zone. Other
  code runs during the yield and its zones nest inside yours.
- **Use fixed names.** A name built per call, like `"Enemy " .. i`, registers a new
  string every time.

When a zone is still open at the end of a frame, because it yielded or errored before
`ZoneEnd`:
- Tracy closes it, notes in the viewer that its duration runs to that point, and warns
  once in Output with the zone's name.
- Its late `ZoneEnd` is ignored, so it can't close someone else's zone.
- `GetStats().ZonesLeaked` and `StrayEnds` count both.

`Wrap` and `Instrument` get this for free, including when the wrapped function errors.

## Switches

Each switch is a constant at the top of the module, or an attribute on the `Tracy`
ModuleScript. The attribute wins. The Tracy-Luau Studio plugin sets these for you.

| Constant | Attribute | Effect |
|---|---|---|
| `ENABLED = false` | `Enabled` (bool) | nothing streams, in Studio too |
| `MICROPROFILER` | `MicroProfiler` (string) | `"Live"`, `"Always"` or `"Off"`; see below |
| | `Url` (string) | default bridge address for `Configure`, e.g. `http://127.0.0.1:8081/ingest` |
| | `BuiltinPlots` (bool) | default for `Configure`'s `BuiltinPlots` |

## In live games

Live servers can't reach the bridge on your machine, so Tracy-Luau works through
Roblox's MicroProfiler there instead.

### Your zones in the MicroProfiler

Outside Studio, `ZoneBegin` and `ZoneEnd` *are* `debug.profilebegin` and
`debug.profileend`, and `Wrap` and `Instrument` use them too. Your zones appear in every
MicroProfiler view and capture of a live server or client.

| `MICROPROFILER` | Zones go to the MicroProfiler | Cost outside Studio |
|---|---|---|
| `"Live"` (default) | whenever Tracy isn't streaming: published games, and Studio with Tracy off | one `debug.profilebegin`/`profileend` pair per zone, ~0.17 µs measured in Studio |
| `"Always"` | in Studio playtests too, next to streaming | the same, on top of Tracy's own cost |
| `"Off"` | never | nothing: every call is an empty function, and `Wrap` returns your function unchanged |

`FrameMark`, `Plot` and `Message` do nothing outside Studio.

### Captures from live servers (`Tracy.Capture`)

When a server frame takes too long, or when you ask, `Tracy.Capture` grabs the last few
dozen frames of the MicroProfiler and sends them out of the game. A capture holds every
engine thread (physics, replication, scripts, GC, workers), your zones, and the frame
stats. It needs [LibMP](https://github.com/Roblox/libmp) (MIT), Roblox's MicroProfiler
API, as a ModuleScript in your game.

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
	Warmup = 15,         -- seconds before the first capture, while the history fills
	MemoryMB = 256,      -- the MicroProfiler's history budget
})

Capture.Now("boss fight started") -- on demand
Capture.GetStats()                -- Captures, Sent, Failed, LastId, LastError
```

| Sink | Where captures go |
|---|---|
| `Capture.DataStoreSink(name or DataStore)` | a DataStore. Nothing to host. Each capture is stored as base64 parts plus an info entry, and counts against the experience's DataStore storage and request budget |
| `Capture.HttpSink(url, { Headers, Secret })` | POSTed to a server you run. `Secret` names a Roblox secret sent as `Authorization: Bearer ...` |
| `Capture.BridgeSink()` | the bridge on this machine (Studio): saved under `captures/` and converted to `.tracy` |
| any `function(info, data)` | your own; error to report failure |

**Getting DataStore captures.**
1. Create an Open Cloud API key with DataStore list and read access to the
   experience (and delete access for `--delete`).
2. Set it in your shell; the tool reads it from the environment:

   ```powershell
   $env:ROBLOX_API_KEY = "<your key>"
   ```

3. Fetch the captures:

   ```powershell
   python tools/fetch_captures.py --universe <universe id> --convert --delete
   ```

Each capture lands in `captures/` as `<id>.gprx`, with its info (reason, time, place
version, frame time, frames) in `<id>.json`. `--convert` makes `<id>.tracy` too, and
`--delete` removes each capture from the DataStore once it's on disk.

**Costs and caveats.**
- **Memory.** The MicroProfiler keeps up to `MemoryMB` of history while it records,
  which is why `SampleRate` exists.
- **A one-frame stall.** Taking a capture stalls one frame while it's compressed and
  checked. In Studio, with a busy scene, that was 45–95 ms for 48–64 frames of about
  10–12 MB.
- **Hollow captures.** If the history is too small for how busy the frames are, a
  capture comes back with few or no frames. `Tracy.Capture` checks for that and retries,
  and the converter says so if it still happens. Raise `MemoryMB` or lower `Frames`.
- **Not yet tried on a live server.** The capture path, the DataStore encoding and the
  fetch tool were tested in Studio and against a mock of Open Cloud's DataStore API.

## Saving a session

```powershell
tracy-roblox-bridge --record session.tracy
```

- **What you get.** The bridge streams as usual, and the live viewer keeps working.
  When you stop it with Ctrl+C, it writes `session.tracy` with every track, zone,
  frame, plot and message it received.
- **The raw file.** While running, it keeps a raw `session.tlrec` next to the output,
  and deletes it after writing the `.tracy`. Pass `--keep-raw` to keep it.
- **If the bridge is killed,** the `.tlrec` stays. Convert it with
  `tracy-roblox-bridge --convert session.tlrec session.tracy`.
- **It needs** `tracy-capture` from the same Tracy release: next to the bridge (the
  Windows zip includes it), on PATH, or passed with `--capture-tool`.

## MicroProfiler dumps to Tracy

The bridge converts any MicroProfiler capture in one step: a `.gprx` from the
MicroProfiler's Dump menu, an HTML dump, or a `Tracy.Capture` capture.

```powershell
tracy-roblox-bridge --convert capture.gprx capture.tracy
```

It runs the importer, `gprx2tlrec.luau`, which ships next to the bridge in the release
zips. That needs:
- [Lute](https://github.com/luau-lang/lute/releases), next to the bridge or on PATH, or
  passed with `--lute`;
- LibMP.luau from [Roblox/libmp releases](https://github.com/Roblox/libmp/releases),
  placed next to `gprx2tlrec.luau`.

| In the dump | In Tracy |
|---|---|
| each MicroProfiler thread (`RBX Main`, `RBX Worker A`, `GPU`, ...) | its own track |
| scopes, your zones included | zones, in their timer's color |
| scope labels (script names, place names, ...) | zone text |
| frames | frame marks |
| GPU scopes | placed on the CPU timeline, using each frame's start time on both clocks |
| core frame stats (`frame_stats/basic/*`) | plots |

You can also run the importer yourself:
`lute run gprx2tlrec.luau capture.gprx capture.tlrec`, then `--convert` the `.tlrec`.
Its options:

| Option | Effect |
|---|---|
| `--counters TEXT` | also plot every counter whose path contains TEXT |
| `--all-counters` | plot every counter, often several hundred |
| `--no-counters` | no plots |
| `--no-gpu` | leave out the GPU track |
| `--no-labels` | leave out labels |
| `--verbose` | also print zones per thread |

## What it costs

Measured on Studio's server with `bench/Overhead.server.luau`. The method and data are
in [BENCHMARKS.md](BENCHMARKS.md).

| 500 zones per frame | Total per frame |
|---|---|
| Studio, streaming to Tracy | **~0.1 ms** (0.09–0.13 ms across runs) |
| Published game, `MICROPROFILER = "Live"` | ~0.08 ms, from the ~0.17 µs a `debug.profilebegin`/`profileend` pair cost in Studio |
| Published game, `MICROPROFILER = "Off"` | **~0** |

- **Clients pay less to send.** A client's flush is a RemoteEvent, about 0.02 ms,
  against the server's ~0.25 ms HTTP request.
- **The floor.** Each zone needs two `os.clock()` calls (22–25 ns each) and two
  function calls (about 20 ns each). Luau has no cheaper timer or call, so about
  0.05 ms per 500 zones is the minimum before anything is sent.
- **Nothing is lost.** 330,000 of 330,000 zones arrived in a capture, and 1.8 million at
  5,000 zones per frame. A busy-wait coded at 1.0 ms reads 1.001 ms.
- **The flush is visible.** It appears as its own `Tracy::Flush` zone, so the
  profiler's cost sits on the timeline next to your code.

## Limits

- **Streaming is Studio only.** Live games use the MicroProfiler route above.
- **Clients need the server.** Client data only flows while a server Script has
  required Tracy.
- **Tested in Play Solo.** "Start Server + N Clients" should work, since clients are
  numbered in join order, but it has only been exercised by the bridge's tests.
- **Pinned Tracy version.** The viewer must be Tracy 0.15.x. The bridge writes Tracy
  queue events directly, to keep the Luau timestamps, so it is tied to that version's
  internals.
- **Shared HTTP budget.** HttpService allows 500 requests a minute per server, shared
  with the game. The default flush uses 240, and early flushes for busy games stay under
  300. If the game hits the limit, Tracy pauses for 5 s and keeps buffering.
- **Use `127.0.0.1`, not `localhost`.** On Windows, `localhost` can add about two
  seconds per request.
- **Timer resolution.** `os.clock()` ticks every 100 ns, so zones shorter than that
  read as 0–100 ns.
- **Untested dump sources.** Dumps saved from the MicroProfiler's own menu, and dumps
  from live servers, aren't tested yet. Every format LibMP writes (raw, compressed,
  ASCII, HTML) converts, and the importer uses only LibMP's API.
- **Big dumps use RAM.** Lute 1.0 lacks the `fs.readIntoBuffer` call that LibMP's
  `OpenFromFile` needs, so the importer loads the whole dump into memory.
