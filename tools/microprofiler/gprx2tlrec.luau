--!strict
--[[
	gprx2tlrec: turns a Roblox MicroProfiler dump into a Tracy-Luau recording, which
	the bridge then converts into a .tracy file:

	    lute run tools/microprofiler/gprx2tlrec.luau capture.gprx capture.tlrec
	    tracy-roblox-bridge --convert capture.tlrec capture.tracy

	Takes the binary dump (MicroProfiler > Dump > "Dump in binary format", a .gprx)
	or the HTML dump, from Studio, the client, or a live server (Developer Console).

	Every MicroProfiler thread becomes a Tracy track, scopes become zones in their
	timer's color, scope labels (script and place names, ...) become zone text,
	frames become frame marks, and the core frame stats become plots.

	Options:
	  --counters TEXT   also plot counters whose path contains TEXT (repeatable)
	  --all-counters    plot every counter (often several hundred)
	  --no-counters     no plots at all
	  --no-gpu          leave out the GPU track
	  --no-labels       leave out scope labels

	Needs Lute (https://github.com/luau-lang/lute) and LibMP.luau, Roblox's
	MicroProfiler reader (https://github.com/Roblox/libmp/releases), placed next
	to this script.
]]

local fs = require("@std/fs")
local process = require("@std/process")
local LibMP = require("./LibMP")

-- Wire protocol v2 (docs/PROTOCOL.md).
local MAGIC = 0x41554C54
local PROTOCOL_VERSION = 2
local SOURCE_NAMED = 255
local FLAG_FINAL = 1
local OP_REGISTER_STRING = 0x01
local OP_ZONE_BEGIN = 0x02
local OP_ZONE_END = 0x03
local OP_FRAME_MARK = 0x04
local OP_PLOT_VALUE = 0x05
local OP_ZONE_BEGIN_COLOR = 0x07
local OP_PLOT_CONFIG = 0x09
local OP_TRACK_NAME = 0x0A
local OP_ZONE_TEXT = 0x0B
local PLOT_MEMORY = 1
local PLOT_FILL = 2

-- The frame stats every capture has: frame time, memory, allocations, GPU time.
local DEFAULT_COUNTERS = { "frame_stats/basic/" }

---------------------------------------------------------------- arguments

local function usage(message: string?): never
	if message then
		print("gprx2tlrec: " .. message)
	end
	print("usage: lute run gprx2tlrec.luau capture.gprx capture.tlrec [--counters TEXT] [--all-counters]")
	print("                                [--no-counters] [--no-gpu] [--no-labels] [--verbose] [--quiet]")
	process.exit(2)
	error("unreachable")
end

local inputPath: string? = nil
local outputPath: string? = nil
local counterFilters = table.clone(DEFAULT_COUNTERS)
local allCounters, withGpu, withLabels, verbose = false, true, true, false
local quiet = false -- --quiet: print only problems (the bridge runs it this way)
do
	local args = process.args
	local i = 2 -- args[1] is this script
	while i <= #args do
		local a = args[i]
		if a == "--counters" then
			i += 1
			if args[i] == nil then
				usage("--counters needs some text to match")
			end
			table.insert(counterFilters, args[i])
		elseif a == "--all-counters" then
			allCounters = true
		elseif a == "--no-counters" then
			counterFilters = {}
		elseif a == "--no-gpu" then
			withGpu = false
		elseif a == "--no-labels" then
			withLabels = false
		elseif a == "--quiet" then
			quiet = true
		elseif a == "--verbose" then
			verbose = true
		elseif string.sub(a, 1, 2) == "--" then
			usage("unknown option " .. a)
		elseif inputPath == nil then
			inputPath = a
		elseif outputPath == nil then
			outputPath = a
		else
			usage("too many arguments")
		end
		i += 1
	end
end
if inputPath == nil or outputPath == nil then
	usage()
end

---------------------------------------------------------------- byte writer

type Bytes = { buf: buffer, len: number }

local function newBytes(size: number): Bytes
	return { buf = buffer.create(size), len = 0 }
end

local function reserve(b: Bytes, n: number)
	local need = b.len + n
	local cap = buffer.len(b.buf)
	if need > cap then
		while cap < need do
			cap *= 2
		end
		local grown = buffer.create(cap)
		buffer.copy(grown, 0, b.buf, 0, b.len)
		b.buf = grown
	end
end

local function u8(b: Bytes, v: number)
	reserve(b, 1)
	buffer.writeu8(b.buf, b.len, v)
	b.len += 1
end

local function u16(b: Bytes, v: number)
	reserve(b, 2)
	buffer.writeu16(b.buf, b.len, v)
	b.len += 2
end

local function u32(b: Bytes, v: number)
	reserve(b, 4)
	buffer.writeu32(b.buf, b.len, v)
	b.len += 4
end

local function f64(b: Bytes, v: number)
	reserve(b, 8)
	buffer.writef64(b.buf, b.len, v)
	b.len += 8
end

local function text16(b: Bytes, s: string) -- [len u16][bytes]
	local n = math.min(#s, 65535)
	u16(b, n)
	reserve(b, n)
	buffer.writestring(b.buf, b.len, s, n)
	b.len += n
end

local function append(dst: Bytes, src: Bytes)
	reserve(dst, src.len)
	buffer.copy(dst.buf, dst.len, src.buf, 0, src.len)
	dst.len += src.len
end

---------------------------------------------------------------- tracks

type Track = {
	name: string,
	session: number,
	seq: number,
	ids: { [string]: number },
	nextId: number,
	events: Bytes,
	depth: number,
	zones: number,
}

local tracks: { Track } = {}

local function newTrack(name: string, session: number): Track
	local track: Track = {
		name = name,
		session = session,
		seq = 0,
		ids = {},
		nextId = 1,
		events = newBytes(64 * 1024),
		depth = 0,
		zones = 0,
	}
	-- A named session opens with its name (it picks the Tracy track).
	u8(track.events, OP_TRACK_NAME)
	text16(track.events, name)
	table.insert(tracks, track)
	return track
end

local function nameId(track: Track, name: string): number
	local id = track.ids[name]
	if id == nil then
		id = track.nextId
		track.nextId += 1
		track.ids[name] = id
		u8(track.events, OP_REGISTER_STRING)
		u16(track.events, id)
		text16(track.events, name)
	end
	return id
end

local function sealBatch(request: Bytes, track: Track, flushClock: number, final: boolean)
	u32(request, MAGIC)
	u8(request, PROTOCOL_VERSION)
	u8(request, SOURCE_NAMED)
	u16(request, if final then FLAG_FINAL else 0)
	u32(request, track.session)
	u32(request, track.seq)
	f64(request, flushClock)
	f64(request, 0) -- clockOffset
	u32(request, track.events.len)
	append(request, track.events)
	track.seq += 1
	track.events.len = 0
end

---------------------------------------------------------------- recording file

local out = newBytes(4 * 1024 * 1024)
reserve(out, 8)
buffer.writestring(out.buf, 0, "TLUAREC1")
out.len = 8
u32(out, PROTOCOL_VERSION)
u32(out, 0)

local requests = 0
local t0 = 0 -- the capture's first frame start: arrival times count from here

local function writeRequest(flushClock: number, final: boolean)
	local request = newBytes(64 * 1024)
	for _, track in tracks do
		if final or track.events.len > 0 then
			sealBatch(request, track, flushClock, final)
		end
	end
	if request.len == 0 then
		return
	end
	local arrival = math.max(0, math.floor((flushClock - t0) * 1e9))
	u32(out, arrival % 0x1_0000_0000)
	u32(out, arrival // 0x1_0000_0000)
	u32(out, request.len)
	append(out, request)
	requests += 1
end

---------------------------------------------------------------- read the dump

local started = os.clock()
local source = fs.readFileToString(inputPath :: string)
-- OpenFromMem rather than OpenFromFile: the latter needs fs.readIntoBuffer, which
-- Lute 1.0 doesn't have.
local session = LibMP.Session.OpenFromMem(source)
if session == nil then
	usage("LibMP could not read " .. (inputPath :: string) .. " (not a MicroProfiler dump?)")
end
source = ""
local mp = session :: any

local global = mp:FetchGlobalDesc()
local cpuSeconds = global.TickToMsCpu / 1000
local gpuSeconds = global.TickToMsGpu / 1000

local function trim(s: string): string
	return (string.gsub(s, "^%s+", ""):gsub("%s+$", ""))
end

-- Threads, with repeated names numbered ("RBX Worker A", "...workerThreadLoop #2").
type ThreadInfo = { name: string, gpu: boolean, track: Track? }
local threads: { [number]: ThreadInfo } = {}
local seenNames: { [string]: number } = {}
local mainThreadId: number? = nil
for _, id in mp:FetchThreadIds() do
	local desc = mp:FetchThreadDesc(id)
	local name = trim(desc.ThreadName)
	if name == "" then
		name = "Thread " .. id
	end
	local n = (seenNames[name] or 0) + 1
	seenNames[name] = n
	if n > 1 then
		name ..= " #" .. n
	end
	threads[id] = { name = name, gpu = desc.IsGpu, track = nil }
	if mainThreadId == nil and string.find(name, "Main", 1, true) and not desc.IsGpu then
		mainThreadId = id
	end
end

local function trackFor(threadId: number): Track?
	local info = threads[threadId]
	if info == nil then
		return nil
	end
	if info.track == nil then
		info.track = newTrack(info.name, 0x4D500000 + threadId)
	end
	return info.track
end

-- The main thread's track comes first, so it owns Tracy's default frame set.
local mainTrack: Track = if mainThreadId then trackFor(mainThreadId) :: Track else newTrack("Frames", 0x4D5000FF)

type TimerInfo = { name: string, color: number }
local timers: { [number]: TimerInfo } = {}
do
	local groupColor: { [number]: number } = {}
	for id = 0, mp:GetGroupIdMax() do
		local ok, desc = pcall(mp.FetchGroupDesc, mp, id)
		if ok and desc then
			groupColor[id] = desc.Color
		end
	end
	for _, id in mp:FetchTimerIds() do
		local desc = mp:FetchTimerDesc(id)
		local color = desc.Color
		if color == 0 then
			color = groupColor[desc.GroupId] or 0
		end
		timers[id] = { name = desc.TimerName, color = bit32.band(color, 0xFFFFFF) }
	end
end

local frameCache: { [number]: any } = {}
local function frameDesc(frameId: number): any
	local desc = frameCache[frameId]
	if desc == nil then
		local ok, fetched = pcall(mp.FetchFrameDesc, mp, frameId)
		desc = if ok and fetched then fetched else false
		frameCache[frameId] = desc
	end
	return desc or nil
end

local firstFrameId, lastFrameId = mp:GetFrameIdMin(), mp:GetFrameIdMax()
local firstFrame = frameDesc(firstFrameId)
t0 = if firstFrame then firstFrame.TickStartCpu * cpuSeconds else 0

-- GPU scopes run on the GPU's clock. Each frame records where it started on both
-- clocks, which pins GPU times onto the CPU timeline.
local function gpuToCpu(timestamp: number, frameId: number): number?
	local frame = frameDesc(frameId)
	if frame == nil or frame.TickStartGpu == 0 then
		return nil
	end
	return frame.TickStartCpu * cpuSeconds + (timestamp - frame.TickStartGpu) * gpuSeconds
end

---------------------------------------------------------------- walk the log

local it = mp:CreateLogIterator()
it:Configure({ SkipEvents = true, SkipGpuThreads = not withGpu })

local maxTime = t0
local frames, zones, labels, gpuSkipped, pending = 0, 0, 0, 0, false
while it:Step() do
	local s = it:GetState()
	if s:FrameChanged() and pending then
		-- one request per frame, like a game flushing as it goes
		writeRequest(maxTime, false)
		pending = false
	end
	if s:IsFrameBoundary() then
		local t = s:Timestamp() * cpuSeconds
		u8(mainTrack.events, OP_FRAME_MARK)
		f64(mainTrack.events, t)
		frames += 1
		pending = true
		if t > maxTime then
			maxTime = t
		end
	elseif s:IsEnter() or s:IsExit() then
		local threadId = s:ThreadId()
		local info = threads[threadId]
		local track = trackFor(threadId)
		if info and track then
			local t: number? = if info.gpu then gpuToCpu(s:Timestamp(), s:FrameId()) else s:Timestamp() * cpuSeconds
			if t == nil then
				gpuSkipped += 1
			else
				local time = t :: number
				if s:IsEnter() then
					local timer = timers[s:TimerId()] or { name = "Timer " .. s:TimerId(), color = 0 }
					local id = nameId(track, timer.name)
					local ev = track.events
					if timer.color ~= 0 then
						u8(ev, OP_ZONE_BEGIN_COLOR)
						u16(ev, id)
						f64(ev, time)
						u32(ev, timer.color)
					else
						u8(ev, OP_ZONE_BEGIN)
						u16(ev, id)
						f64(ev, time)
					end
					track.depth += 1
					track.zones += 1
					zones += 1
				elseif track.depth > 0 then
					u8(track.events, OP_ZONE_END)
					f64(track.events, time)
					track.depth -= 1
				end
				pending = true
				if time > maxTime then
					maxTime = time
				end
			end
		end
	elseif s:IsLabel() and withLabels then
		local track = trackFor(s:ThreadId())
		if track and track.depth > 0 then
			local label = it:FetchCurrentLabel()
			if label and #label > 0 then
				u8(track.events, OP_ZONE_TEXT)
				text16(track.events, label)
				labels += 1
				pending = true
			end
		end
	end
end
it:Dispose()
if pending then
	writeRequest(maxTime, false)
end
if frames > 0 and zones == 0 then
	print(
		"gprx2tlrec: "
			.. (inputPath :: string)
			.. " has frames but no scopes in them. For a Tracy.Capture capture taken right after "
			.. "Capture.Start, the MicroProfiler's history was still filling: capture later (see Warmup)."
	)
	process.exit(3)
end
if frames == 0 then
	print(
		"gprx2tlrec: "
			.. (inputPath :: string)
			.. " holds no complete frames, so there's nothing to put on a timeline. For a "
			.. "Tracy.Capture capture, the MicroProfiler's history overflowed before the capture: "
			.. "raise MemoryMB or lower Frames in Capture.Start."
	)
	process.exit(3)
end

---------------------------------------------------------------- counters -> plots

local counterTrack: Track? = nil
local plotted = 0
if allCounters or #counterFilters > 0 then
	-- full paths ("frame_stats/basic/cpu_frame_time_microsec"); leaves only
	local descs: { [number]: any } = {}
	for _, id in mp:FetchCounterIds() do
		descs[id] = mp:FetchCounterDesc(id)
	end
	local function pathOf(id: number): string
		local parts = {}
		local cur = descs[id]
		local guard = 0
		while cur and guard < 64 do
			table.insert(parts, 1, cur.CounterName)
			if cur.Level == 0 or cur.Parent == cur.CounterId then
				break
			end
			cur = descs[cur.Parent]
			guard += 1
		end
		return table.concat(parts, "/")
	end
	type Series = { name: string, byFrame: { [number]: number } }
	local series: { Series } = {}
	for id, desc in descs do
		if desc.FirstChild == 0 then
			local path = pathOf(id)
			local wanted = allCounters
			for _, filter in counterFilters do
				if string.find(path, filter, 1, true) then
					wanted = true
				end
			end
			if wanted then
				local byFrame = {}
				for _, sample in mp:FetchCounterSamples(id) do
					byFrame[sample.FrameId] = sample.Value
				end
				table.insert(series, { name = path, byFrame = byFrame })
			end
		end
	end
	table.sort(series, function(a, b)
		return a.name < b.name
	end)
	if #series > 0 then
		local track = newTrack("Counters", 0x4D5000FE)
		counterTrack = track
		for _, s in series do
			local id = nameId(track, s.name)
			if string.find(s.name, "_bytes", 1, true) then
				u8(track.events, OP_PLOT_CONFIG)
				u16(track.events, id)
				u8(track.events, PLOT_MEMORY)
				u8(track.events, PLOT_FILL)
				u32(track.events, 0)
			end
		end
		-- Frame by frame, so every plot's samples go forward in time (a track never
		-- goes back in time in the bridge).
		for frameId = firstFrameId, lastFrameId do
			local frame = frameDesc(frameId)
			if frame then
				local t = frame.TickStartCpu * cpuSeconds
				for _, s in series do
					local value = s.byFrame[frameId]
					if value ~= nil then
						u8(track.events, OP_PLOT_VALUE)
						u16(track.events, track.ids[s.name])
						f64(track.events, t)
						f64(track.events, value)
						plotted += 1
					end
				end
			end
		end
		writeRequest(maxTime, false)
	end
end

-- End every session, so the bridge closes what's still open and frees the tracks.
writeRequest(maxTime, true)
mp:Dispose()

fs.writeStringToFile(outputPath :: string, buffer.readstring(out.buf, 0, out.len))

local threadsWithZones = 0
for _, track in tracks do
	if track.zones > 0 then
		threadsWithZones += 1
		if verbose then
			print(string.format("  %-48s %7d zones", track.name, track.zones))
		end
	end
end
if quiet then
	process.exit(0)
end
print(
	string.format(
		"wrote %s: %d frames, %d zones on %d threads, %d labels, %d plot samples%s, %.1f MB in %d requests (%.1f s)",
		outputPath :: string,
		frames,
		zones,
		threadsWithZones,
		labels,
		plotted,
		if gpuSkipped > 0 then string.format(", %d GPU scopes skipped (no GPU clock anchor)", gpuSkipped) else "",
		out.len / (1024 * 1024),
		requests,
		os.clock() - started
	)
)
print("next: tracy-roblox-bridge --convert " .. (outputPath :: string) .. " capture.tracy")
