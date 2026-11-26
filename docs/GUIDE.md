# Tracy-Luau guide

- [Installing](#installing)
- [API](#api)
- [Zone rules](#zone-rules)
- [Switches](#switches)
- [In live games](#in-live-games)
- [Saving a session](#saving-a-session)
- [MicroProfiler dumps to Tracy](#microprofiler-dumps-to-tracy)
- [Limits](#limits)

## Installing

The module is a ModuleScript `Tracy` with a child `Capture`. Install it one of these
ways:

| How | |
|---|---|
| Wally | `Tracy = "justchooseandrew/tracy-luau@0.5.0"` |
| Pesde | `pesde add justchooseandrew/tracy_luau` |
| Model file | `Tracy.rbxm` from a release, into `ReplicatedStorage` |
| Rojo | sync `default.project.json` |

The bridge comes from a release:

| Platform | Download |
|---|---|
| Windows | `tracy-roblox-bridge-windows-x64.zip` (includes the Tracy viewer and `tracy-capture`) |
| macOS | `tracy-roblox-bridge-macos-universal.tar.gz` |
| Linux | `tracy-roblox-bridge-linux-x64.tar.gz` |

The viewer must be Tracy 0.15.x. On macOS and Linux, build it from the
[Tracy v0.15.0 source](https://github.com/wolfpld/tracy/releases/tag/v0.15.0).

## API

```lua
local Tracy = require(ReplicatedStorage.Tracy)

Tracy.ZoneBegin("Enemies")              -- open a zone (use a fixed name)
Tracy.ZoneBegin("AI", 0xFF5500)         -- with a color (0xRRGGBB)
Tracy.ZoneEnd()                         -- close the most recent zone
Tracy.FrameMark()                       -- end of frame
Tracy.FrameMark("Physics")              -- a separate, named frame set

local update = Tracy.Wrap("Enemies", updateEnemies)  -- a function, inside a zone
Tracy.Instrument(EnemyService, "EnemyService")       -- every function in a table

Tracy.Plot("Enemies alive", #enemies)   -- a sample on a named plot
Tracy.ConfigurePlot("Cache", {          -- optional, once per plot
	Format = "Memory",                  -- "Number" (default), "Memory" or "Percentage"
	Step = true, Fill = false, Color = 0x33AAFF,
})
Tracy.Message("Wave 3 started")         -- text marker on this VM's track
Tracy.Message("Boss spawned", 0xFF0000) -- with a color

Tracy.Configure({
	Url = "http://127.0.0.1:8080/ingest", -- must match the bridge's --port
	FlushInterval = 0.25,                  -- seconds between batches
	FlushBytes = 512 * 1024,               -- send early past this much
	RetryInterval = 2,                     -- seconds between reconnect attempts
	MaxBufferSize = 4 * 1024 * 1024,       -- unsent bytes kept before dropping a batch
	BuiltinPlots = true,                   -- Lua heap, instances and ping, 10 times a second
})

Tracy.IsActive()     -- streaming?
Tracy.IsConnected()  -- reaching the bridge?
Tracy.GetStats()     -- batches, bytes, flush time, round trips, leaked zones, ...
```

- **Tracks and names.** Each VM gets its own track, such as `Roblox Server` and
  `Roblox Client 1`. Plots and named frame sets carry the track's name, like
  `Server: Lua heap`.
- **Client scripts.** Also require Tracy from a server Script. Client data travels
  through the server.
- **Speed.** Cached locals are a little faster:
  `local ZoneBegin, ZoneEnd = Tracy.ZoneBegin, Tracy.ZoneEnd`.

## Zone rules

- **Close every zone** in the function that opened it, on every path.
- **Don't yield** (`task.wait`, `WaitForChild`, remote calls) inside a zone.
- **Use fixed names**, not names built per call like `"Enemy " .. i`.

If a zone is still open at the end of a frame (it yielded or errored), Tracy closes it,
labels it in the viewer, warns once, and ignores its late `ZoneEnd`. `Wrap` and
`Instrument` are covered by this too.

## Switches

Each switch is a constant at the top of the module or an attribute on the `Tracy`
ModuleScript. The attribute wins, and the Studio plugin sets these for you.

| Constant | Attribute | Effect |
|---|---|---|
| `ENABLED` | `Enabled` | `false` turns streaming off, in Studio too |
| `MICROPROFILER` | `MicroProfiler` | `"Live"`, `"Always"` or `"Off"`; see below |
| | `Url` | the bridge address `Configure` defaults to |
| | `BuiltinPlots` | `Configure`'s `BuiltinPlots` default |

## In live games

Live servers can't reach the bridge on your machine, so Tracy-Luau goes through
Roblox's MicroProfiler instead.

### Your zones in the MicroProfiler

Outside Studio, `ZoneBegin` and `ZoneEnd` call `debug.profilebegin` and
`debug.profileend`, so your zones show up in MicroProfiler views and captures.

| `MICROPROFILER` | Zones go to the MicroProfiler... | Cost |
|---|---|---|
| `"Live"` (default) | whenever Tracy isn't streaming | ~0.17 µs per zone |
| `"Always"` | in Studio playtests too | the same, on top of streaming |
| `"Off"` | never | nothing |

### Captures from live servers

`Tracy.Capture` saves a server's recent MicroProfiler frames when the server hitches, or
when you ask. A capture holds every engine thread and your zones. It needs
[LibMP](https://github.com/Roblox/libmp) in your game.

```lua
local Capture = require(ReplicatedStorage.Tracy.Capture)

Capture.Start({
	LibMP = require(ServerStorage.LibMP),
	Sink = Capture.DataStoreSink("TracyCaptures"),
	HitchMs = 150,       -- capture when a frame takes this long (0 = only Now())
	Frames = 64,         -- frames per capture
	Cooldown = 600,      -- seconds between automatic captures
	MaxCaptures = 3,     -- per server
	SampleRate = 0.05,   -- share of servers that record at all
	Warmup = 15,         -- seconds before the first capture
	MemoryMB = 256,      -- the MicroProfiler's history budget
})

Capture.Now("boss fight started")
```

| Sink | Where captures go |
|---|---|
| `DataStoreSink(name)` | a DataStore (nothing to host) |
| `HttpSink(url, { Headers, Secret })` | a server you run |
| `BridgeSink()` | the bridge on this machine, in Studio |
| `function(info, data)` | your own |

**Getting DataStore captures:**
1. Create an Open Cloud API key with DataStore list and read access (plus delete for
   `--delete`).
2. Set it in your shell: `$env:ROBLOX_API_KEY = "<your key>"`.
3. Run `python tools/fetch_captures.py --universe <id> --convert --delete`.

Captures land in `captures/` as `.gprx` files with their info, and as `.tracy` files
with `--convert`.

**Costs.**
- **Memory.** The MicroProfiler holds up to `MemoryMB` while it records, which is why
  `SampleRate` exists.
- **One stalled frame.** Taking a capture blocks that frame for 45–95 ms in a busy
  Studio scene.
- **Few or no frames?** Raise `MemoryMB` or lower `Frames`.

## Saving a session

```powershell
tracy-roblox-bridge --record session.tracy
```

- **What it does.** The bridge streams as usual, and writes `session.tracy` when you
  stop it with Ctrl+C.
- **If the bridge is killed,** convert the leftover `session.tlrec` with
  `tracy-roblox-bridge --convert session.tlrec session.tracy`.
- **It needs** `tracy-capture` from the same Tracy release, next to the bridge or on
  PATH.

## MicroProfiler dumps to Tracy

```powershell
tracy-roblox-bridge --convert capture.gprx capture.tracy
```

This works for `.gprx` dumps, HTML dumps and `Tracy.Capture` captures. It needs:
- [Lute](https://github.com/luau-lang/lute/releases) on PATH;
- LibMP.luau next to `gprx2tlrec.luau`.

Each MicroProfiler thread becomes a track, scopes become zones in their colors, labels
become zone text, and frame stats become plots.

To run the importer yourself:
`lute run gprx2tlrec.luau capture.gprx capture.tlrec`. Its options:

| Option | Effect |
|---|---|
| `--counters TEXT` | also plot counters whose path contains TEXT |
| `--all-counters` | plot every counter |
| `--no-counters` | no plots |
| `--no-gpu` | leave out the GPU track |
| `--no-labels` | leave out labels |

## Limits

- **Streaming is Studio only.** Live games use the MicroProfiler route.
- **Pinned viewer version.** The viewer must be Tracy 0.15.x.
- **Shared HTTP budget.** HttpService allows 500 requests a minute per server, and Tracy
  uses about 240 of them.
- **Use `127.0.0.1`, not `localhost`.** `localhost` is slow on Windows.
- **Timer resolution.** `os.clock()` ticks every 100 ns.
- **Not yet tested:** "Start Server + N Clients" in Studio, and dumps from live servers
  or the MicroProfiler's own menu.
