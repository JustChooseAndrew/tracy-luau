# Tracy-Luau

Tracy-Luau is a profiler for Roblox, built on [Tracy](https://github.com/wolfpld/tracy).
Mark zones in your Luau code and press Play in Studio. The server and every client show
up on one timeline in Tracy's viewer, with the durations your code actually measured.
In published games the same zones go to Roblox's MicroProfiler, and captures from live
servers come back to your machine as Tracy traces.

## What is Tracy, and why use it?

Tracy is a frame profiler used across the games industry.
- **Zoom.** Its viewer zooms from a whole session down to single nanoseconds.
- **Statistics.** It gives every zone counts, averages and a histogram.
- **Comparison.** It compares two traces side by side, and saves sessions as files you
  can share.

Roblox's MicroProfiler shows a short window of recent frames. Tracy-Luau keeps the whole
session, so a hitch that happens once every few minutes is still there when you go
looking for it. It's built for the systems you write yourself: AI, custom physics, world
generation, ECS schedulers, and anything else that eats frame time.

## Usage

**The module.** Install it in one of three ways:
- as a [Wally](https://wally.run/) package: `Tracy = "justchooseandrew/tracy-lua@0.5.0"`;
- as `Tracy.rbxm` from the
  [latest release](https://github.com/JustChooseAndrew/tracy-lua/releases/latest),
  dropped into `ReplicatedStorage`;
- from source, with Rojo.

**The bridge.** Download `tracy-roblox-bridge` from the same release. The Windows zip
includes the matching Tracy viewer.

Then:
1. Turn on Game Settings → Security → Allow HTTP Requests.
2. Start `tracy-roblox-bridge`, open Tracy, and connect to `127.0.0.1`.
3. Press Play.

Here's a basic example:

```lua
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")
local Tracy = require(ReplicatedStorage.Tracy)

RunService.Heartbeat:Connect(function()
	Tracy.ZoneBegin("Enemies")
	updateEnemies()
	Tracy.ZoneEnd()
	Tracy.FrameMark()
end)
```

And a more complete one:

```lua
local Tracy = require(ReplicatedStorage.Tracy)

-- zones without writing Begin/End pairs
local updateAI = Tracy.Wrap("AI", updateAI)
Tracy.Instrument(EnemyService, "EnemyService") -- every function in the table

RunService.Heartbeat:Connect(function()
	Tracy.ZoneBegin("Physics", 0x3399FF) -- with a color
	stepPhysics()
	Tracy.ZoneEnd()

	updateAI()
	EnemyService:Update()

	Tracy.Plot("Enemies alive", #enemies)
	Tracy.FrameMark()
end)

Tracy.Message("Wave 3 started")
```

**Client scripts work too.** Require Tracy from at least one server Script as well, and
each client gets its own track.

**Saving and converting.**
- `tracy-roblox-bridge --record session.tracy` saves the session as a file.
- `tracy-roblox-bridge --convert capture.gprx capture.tracy` turns a MicroProfiler dump
  into a Tracy trace.

**Live games.** Leave your markers in. Outside Studio they become MicroProfiler scopes.
To capture a live server's frames whenever it hitches:

```lua
local Capture = require(ReplicatedStorage.Tracy.Capture)

Capture.Start({
	LibMP = require(ServerStorage.LibMP), -- https://github.com/Roblox/libmp
	Sink = Capture.DataStoreSink("TracyCaptures"),
	HitchMs = 150,
	SampleRate = 0.05,
})
```

Then download and convert the captures:

```powershell
python tools/fetch_captures.py --universe <id> --convert
```

## Learning Tracy-Luau

- **Examples.** The [examples](examples) folder has timed demos whose numbers you can
  check against the code, a part fountain with real costs, and a live-capture setup.
- **The guide.** [docs/GUIDE.md](docs/GUIDE.md) covers everything else:
  - the full API and the rules for zones;
  - live captures;
  - recording and dump conversion;
  - costs and limits.
- **Contributing.** [docs/DEVELOPING.md](docs/DEVELOPING.md) covers building the bridge
  and running the tests.

## How it works

```
Studio client ─RemoteEvent─┐
                           ▼
Studio server ───── HTTP POST ─────► tracy-roblox-bridge ───► Tracy viewer
 Tracy module batches         127.0.0.1    replays each event at the
 events every ~250 ms                      time Luau recorded it
```

- **Why the bridge.** Roblox can't open raw sockets, so the server posts batches over
  HTTP to the bridge. The bridge replays them into Tracy's own client library.
- **Why clients go through the server.** Clients can't make HTTP requests, so their
  batches travel to the server over a RemoteEvent, and the server forwards them.
- **One clock.** Every event carries the `os.clock()` time it happened, and clients sync
  their clocks to the server's, so all tracks line up.

500 zones a frame cost about 0.1 ms. See [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## Credits

Created by [JustChooseAndrew](https://github.com/JustChooseAndrew).

Built on the work of others:
- [Tracy](https://github.com/wolfpld/tracy), by Bartosz Taudul and contributors;
- [LibMP](https://github.com/Roblox/libmp), by Roblox;
- [cpp-httplib](https://github.com/yhirose/cpp-httplib), by Yuji Hirose.

Thanks to everyone who has helped with Tracy-Luau.

Tracy-Luau is MIT licensed. It's an independent project, not affiliated with or
endorsed by Roblox, the Luau team, or the Tracy project. Luau is a trademark of Roblox
Corporation.
