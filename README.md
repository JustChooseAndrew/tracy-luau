# Tracy-Luau

Profile Roblox games in [Tracy](https://github.com/wolfpld/tracy). Put zone markers in
your Luau, press Play in Studio, and watch the server and each client land on Tracy's
timeline on aligned tracks. Zones keep the durations Luau measured, and plots and
messages come along with them.

> **Status: Phase 4 (offline traces).** Zones with colors, frames, plots, messages,
> built-in plots (Lua heap, instance count, ping), and client scripts on their own
> tracks. Sessions can be saved as `.tracy` files, and Roblox MicroProfiler dumps
> (`.gprx`) can be converted to `.tracy`. 500 zones per frame cost about 0.1 ms. The
> Studio plugin comes after the core phases. See [Roadmap](#roadmap).

```
Studio client VM ─RemoteEvent─┐
                              ▼
Studio server VM ────────── HTTP POST ──► tracy-roblox-bridge ──TCP──► Tracy 0.15
 Tracy.luau: buffer,         127.0.0.1     replays the events with       viewer
 interning, batches          :8080         the Luau timestamps
 every ~250 ms                             through Tracy's own client
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

## Quick start

1. **Get the viewer.** Download `windows-0.15.0.zip` from
   [Tracy v0.15.0](https://github.com/wolfpld/tracy/releases/tag/v0.15.0). It must be
   0.15.x, the same version the bridge is built with. Other versions refuse the
   connection.
2. **Build the bridge.** Needs Visual Studio 2022 Build Tools (C++ workload) and CMake.
   ```powershell
   powershell -ExecutionPolicy Bypass -File bridge\build.ps1
   ```
3. **Add the module.** Put `src/Tracy.luau` in `ReplicatedStorage` as a ModuleScript
   named `Tracy`. You can also sync it with Rojo through `default.project.json`.
4. **Allow HTTP.** Game Settings → Security → Allow HTTP Requests.
5. **Run it.** Start `build\tracy-roblox-bridge.exe`. Open `tracy-profiler.exe` and
   connect to `127.0.0.1`. Press Play. To save the session as a file, add
   `--record session.tracy`; see [Saving a session](#saving-a-session---record).

Try the demos first: `examples/ServerDemo.server.luau` (ServerScriptService) and
`examples/ClientDemo.client.luau` (StarterPlayerScripts). Their zones busy-wait for
known times, so you can check Tracy's numbers against the code. They also send a ping
each second that shows the two tracks lining up.

For something closer to a real game, try `examples/Fountain.server.luau` and
`examples/Fountain.client.luau`. They run a part fountain with no busy-waits. The server
launches parts, scans a region for the ones that landed and removes old ones. The
client outlines the highest part. The spawn rate changes in waves, so you can watch the
zones and plots follow the part count.

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

Tracy.IsActive()     -- recording? (Studio, running, ENABLED)
Tracy.IsConnected()  -- reaching the bridge? (on a client: as reported by the server)
Tracy.GetStats()     -- batches, bytes, flush time, round trips, buffer, clients, clock offset
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

**Fastest form.** Cache the functions in locals. Calling through `Tracy.X` costs about
4 ns more per call.

```lua
local ZoneBegin, ZoneEnd = Tracy.ZoneBegin, Tracy.ZoneEnd
```

**Safe to ship.** Outside Studio every function is an empty no-op, so markers can stay
in a published place. To switch them off in Studio too, set `ENABLED = false` at the
top of the module.

**Zone rules.**
- Close every zone in the function that opened it, on every path, early returns
  included.
- Don't yield (`task.wait`, `WaitForChild`, remote calls, HTTP) inside a zone. Other
  code runs during the yield and its zones nest inside yours.
- Use fixed names. A name built per call, like `"Enemy " .. i`, registers a new string
  every time.

## Saving a session (`--record`)

```powershell
tracy-roblox-bridge --record session.tracy
```

- **What it does.** The bridge streams as usual, and the live viewer keeps working.
  When you stop it with Ctrl+C, it writes `session.tracy` containing everything it
  received: every track, zone, frame, plot and message. Open the file in the viewer
  any time later.
- **How it saves.** While running, it keeps a raw `session.tlrec` next to the output.
  It turns that into the `.tracy` file on exit, then deletes it. Pass `--keep-raw` to
  keep it.
- **If it doesn't finish.** If the bridge is killed, or the console window is closed,
  the `.tlrec` stays. Convert it yourself:

  ```powershell
  tracy-roblox-bridge --convert session.tlrec session.tracy
  ```

- **What it needs.** Writing `.tracy` files uses `tracy-capture`, which ships in the
  same Tracy 0.15 zip as the viewer. Put it next to `tracy-roblox-bridge`, on PATH, or
  pass `--capture-tool path\to\tracy-capture.exe`. The bridge checks for it before it
  starts recording.

## Importing MicroProfiler dumps

Roblox's own MicroProfiler shows engine work: physics, rendering, worker threads, the
GPU, and your scripts as labels. To see a MicroProfiler capture in Tracy:

1. **Take a dump.** In the MicroProfiler, choose Dump → "Dump in binary format". That
   saves a `.gprx` file. The HTML dumps work too.
2. **Convert it with Lute and LibMP:**

   ```powershell
   lute run tools/microprofiler/gprx2tlrec.luau capture.gprx capture.tlrec
   ```

   ```powershell
   tracy-roblox-bridge --convert capture.tlrec capture.tracy
   ```

The importer needs [Lute](https://github.com/luau-lang/lute/releases) and LibMP.luau,
Roblox's MicroProfiler reader. Download LibMP.luau from
[Roblox/libmp releases](https://github.com/Roblox/libmp/releases) and put it next to
`gprx2tlrec.luau`.

| In the dump | In Tracy |
|---|---|
| each MicroProfiler thread (`RBX Main`, `RBX Worker A`, `GPU`, ...) | its own track |
| scopes | zones, in their timer's color |
| scope labels (script names, place names, ...) | zone text |
| frames | frame marks |
| GPU scopes | placed on the CPU timeline, using each frame's start time on both clocks |
| core frame stats (`frame_stats/basic/*`: frame time, memory, allocations, GPU time) | plots |

Importer options:

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

| 500 zones per frame | Zone calls | Sending, per frame | Total |
|---|---|---|---|
| Ordinary game script | 0.062–0.067 ms | 0.029 ms | **~0.1 ms** (0.6% of a 60 FPS frame) |
| `--!native` game script | 0.057 ms | 0.033 ms | **~0.09 ms** |
| Tracy disabled or outside Studio | ~0 | none | **~0** |

**Clients pay less to send.** A client's flush is a RemoteEvent, about 0.02 ms, against
the server's ~0.25 ms HTTP request.

**The floor.** Each zone needs two `os.clock()` calls (about 22 ns each) and two
function calls (about 20 ns each). Luau has no cheaper timer or call, so about 0.05 ms
per 500 zones is the minimum before anything is sent.

**What's verified.**
- Every zone arrives: 330,000 of 330,000 in a capture, and 1.8 million at 5,000 zones
  per frame.
- Durations match what Luau measured. A busy-wait coded at 1.0 ms reads 1.001 ms.
- Each repeated zone costs 20 bytes on the wire.

The flush also appears as its own `Tracy::Flush` zone. That way the profiler's cost
is visible on the timeline next to your code.

## Limits in this version

- **Studio only.** Live servers can't reach your machine.
- **Clients need the server.** Client data only flows while a server Script has
  required Tracy.
- **Tested in Play Solo.** Studio's "Start Server + N Clients" mode should work, since
  clients are numbered in join order. So far it has only been exercised by the bridge's
  tests, not in Studio.
- **Pinned Tracy version.** The viewer must be Tracy 0.15.x. The bridge writes Tracy
  queue events directly, to keep the Luau timestamps, so it is tied to that version's
  internals.
- **Shared HTTP budget.** HttpService allows 500 requests a minute per server, and the
  game shares that budget. The default flush uses 240. Early flushes for busy games
  stay under 300. Client data rides in the same requests. If the game hits the limit,
  Tracy pauses for 5 s and keeps buffering.
- **Use `127.0.0.1`, not `localhost`.** On Windows, `localhost` added close to two
  seconds per request in testing.
- **Timer resolution.** `os.clock()` ticks every 100 ns, so zones shorter than that
  read as 0–100 ns.
- **MicroProfiler import, what's tested.** It was tested on a capture taken with
  LibMP's own `CaptureToBufferSync` from a Studio playtest. Not yet tested on a `.gprx`
  saved from the MicroProfiler menu, an HTML dump, or a live-server dump. LibMP reads
  all of these, and the importer uses only LibMP's API.
- **Lute 1.0.** It lacks the `fs.readIntoBuffer` call that LibMP's `OpenFromFile`
  needs, so the importer loads the dump into memory and uses `OpenFromMem`. A
  100 MB dump needs roughly that much RAM.

## Repository layout

| Path | What |
|---|---|
| `src/Tracy.luau` | the Luau module (server and client) |
| `bridge/` | the bridge (C++17, CMake). Tracy 0.15.0 client and cpp-httplib 0.67.0 are vendored unmodified in `bridge/third_party/`, with their licenses |
| `docs/PROTOCOL.md` | wire format, relay messages, and the rules both sides rely on |
| `docs/BENCHMARKS.md` | measurements and method |
| `examples/` | demo scripts with known timings, and the part-fountain example |
| `bench/Overhead.server.luau` | overhead benchmark; run it in Play |
| `tools/microprofiler/gprx2tlrec.luau` | MicroProfiler dump → `.tlrec` importer (Lute + LibMP) |
| `tools/test_bridge.py` | bridge tests: timings, colors, messages, plots, client tracks and clock alignment, named tracks, gaps, resync, malformed input, fuzzing, `--record` / `--convert` |
| `tools/fake_client.py` | sends synthetic batches, to test the bridge without Roblox |

## Testing

```powershell
python tools/test_bridge.py --tracy-dir path\to\tracy-0.15.0
```

This starts its own bridge on test ports (18080 and 18086), so it doesn't disturb a
bridge you already have running. With the Tracy tools folder, it also checks what Tracy
recorded and runs the recording tests: `--record` with a live viewer connected,
`--convert`, and raw-file cleanup. Without the folder, only the HTTP and `/status`
checks run.

## Roadmap

| Phase | Scope | State |
|---|---|---|
| 1 | End-to-end proof: zones and frames from Studio into Tracy | **done** |
| 2 | Binary buffer, interning, batching, benchmarks, hardening | **done**: ~0.1 ms per 500 zones. The < 0.05 ms target is below Luau's floor ([why](docs/BENCHMARKS.md#where-the-time-goes)) |
| 3 | `Tracy.Plot`, `Tracy.Message`, zone colors, client VMs on their own tracks | **done** |
| 4 | `--record` to a `.tracy` file; `.gprx` MicroProfiler import | **done** |
| 5 | Release: CI-built Windows/macOS binaries, Wally and Pesde packages | next |
| Plugin | Studio toolbar button, connection light, auto-injecting the module | after the core is done |

## Licenses

- **Vendored code** keeps its own license: Tracy (BSD-3-Clause) and cpp-httplib (MIT),
  in `bridge/third_party/`.
- **LibMP** (MIT) isn't vendored. Each user downloads it.
- **This project's own license** is still to be chosen.
