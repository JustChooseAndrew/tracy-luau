--[[
	Tracy-Luau overhead benchmark.

	Put this Script in ServerScriptService and Tracy.luau in ReplicatedStorage as
	"Tracy". Start tracy-roblox-bridge (a viewer is optional), press Play, and wait
	for the report in the Output window (~12 s).

	Every frame it times five loops of ZONES iterations, in a rotating order:
	  loop      the bare loop (name lookup only), subtracted from the others
	  clock     two os.clock() calls: the timer floor every zone pays
	  noop      two calls to empty functions: what markers cost with Tracy off
	  tracy     Tracy.ZoneBegin(name) + Tracy.ZoneEnd()
	  tracyLocal  the same through locals (local ZoneBegin = Tracy.ZoneBegin)
	Flushing is measured from Tracy.GetStats() over the run and reported per frame.

	Attributes on this Script: Zones (default 500), Frames (default 600).
	The report is also written to this Script's "Report" attribute as JSON.
]]

local HttpService = game:GetService("HttpService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")

local Tracy = require(ReplicatedStorage:WaitForChild("Tracy"))

local ZONES: number = script:GetAttribute("Zones") or 500
local FRAMES: number = script:GetAttribute("Frames") or 600
local WARMUP = 60

-- Optional Tracy.Configure overrides, for stress runs.
local overrides = {}
for _, key in { "FlushInterval", "FlushBytes", "MaxBufferSize" } do
	local value = script:GetAttribute(key)
	if value ~= nil then
		overrides[key] = value
	end
end
if next(overrides) then
	Tracy.Configure(overrides)
end

local NAME_COUNT = 50
local zoneNames = table.create(ZONES)
for i = 1, ZONES do
	zoneNames[i] = "Bench::Zone" .. ((i - 1) % NAME_COUNT + 1)
end

local function noop(_: string?) end
local Off = { ZoneBegin = noop, ZoneEnd = noop }
local ZoneBegin, ZoneEnd = Tracy.ZoneBegin, Tracy.ZoneEnd

local variants: { { name: string, run: () -> () } } = {
	{
		name = "loop",
		run = function()
			for i = 1, ZONES do
				local _ = zoneNames[i]
			end
		end,
	},
	{
		name = "clock",
		-- selene: allow(must_use)
		run = function()
			for i = 1, ZONES do
				local _ = zoneNames[i]
				os.clock()
				os.clock()
			end
		end,
	},
	{
		name = "noop",
		run = function()
			for i = 1, ZONES do
				Off.ZoneBegin(zoneNames[i])
				Off.ZoneEnd()
			end
		end,
	},
	{
		name = "tracy",
		run = function()
			for i = 1, ZONES do
				Tracy.ZoneBegin(zoneNames[i])
				Tracy.ZoneEnd()
			end
		end,
	},
	{
		name = "tracyLocal",
		run = function()
			for i = 1, ZONES do
				ZoneBegin(zoneNames[i])
				ZoneEnd()
			end
		end,
	},
}

local samples: { [string]: { number } } = {}
for _, v in variants do
	samples[v.name] = table.create(FRAMES)
end

local function percentile(sorted: { number }, p: number): number
	local idx = math.clamp(math.ceil(#sorted * p), 1, #sorted)
	return sorted[idx]
end

local function summarize(list: { number })
	local sorted = table.clone(list)
	table.sort(sorted)
	return { median = percentile(sorted, 0.5), p90 = percentile(sorted, 0.9), p99 = percentile(sorted, 0.99) }
end

local frame = 0
local windowRttMax = 0
local statsStart = Tracy.GetStats()
local clockStart = 0
local connection: RBXScriptConnection

local function report()
	connection:Disconnect()
	local elapsed = os.clock() - clockStart
	local s1 = Tracy.GetStats()
	local base = summarize(samples.loop)

	local result = {
		zones = ZONES,
		frames = FRAMES,
		active = Tracy.IsActive(),
		connected = Tracy.IsConnected(),
		variants = {},
	}
	local lines = {
		string.format(
			"[TracyBench] %d zone pairs per frame, %d frames (Tracy active: %s, connected: %s)",
			ZONES,
			FRAMES,
			tostring(result.active),
			tostring(result.connected)
		),
		"[TracyBench] net ms per frame after the bare loop  (median / p90 / p99)   ns per call",
	}
	for _, v in variants do
		local s = summarize(samples[v.name])
		local net = { median = s.median - base.median, p90 = s.p90 - base.median, p99 = s.p99 - base.median }
		result.variants[v.name] = { median = s.median, p90 = s.p90, p99 = s.p99, netMedian = net.median }
		if v.name ~= "loop" then
			table.insert(
				lines,
				string.format(
					"[TracyBench]   %-11s %.4f / %.4f / %.4f       %.1f",
					v.name,
					net.median * 1000,
					net.p90 * 1000,
					net.p99 * 1000,
					net.median / (2 * ZONES) * 1e9
				)
			)
		else
			table.insert(lines, string.format("[TracyBench]   %-11s %.4f ms raw median", v.name, s.median * 1000))
		end
	end

	local flushes = s1.Flushes - statsStart.Flushes
	local flushSeconds = s1.FlushSeconds - statsStart.FlushSeconds
	local flushPerFrame = flushSeconds / FRAMES
	local trips = (s1.BatchesSent - statsStart.BatchesSent) + (s1.BatchesFailed - statsStart.BatchesFailed)
	result.flush = {
		flushes = flushes,
		meanMs = if flushes > 0 then flushSeconds / flushes * 1000 else 0,
		perFrameMs = flushPerFrame * 1000,
		rttMeanMs = if trips > 0 then (s1.RoundTripSeconds - statsStart.RoundTripSeconds) / trips * 1000 else 0,
		rttMaxMs = s1.MaxRoundTripSeconds * 1000,
		rttWindowMaxMs = windowRttMax * 1000,
		bytesPerSecond = (s1.BytesSent - statsStart.BytesSent) / elapsed,
		sent = s1.BatchesSent - statsStart.BatchesSent,
		failed = s1.BatchesFailed - statsStart.BatchesFailed,
		dropped = s1.BatchesDropped - statsStart.BatchesDropped,
		bufferBytes = s1.BufferBytes,
	}
	local tracyNet = result.variants.tracy.netMedian
	local localNet = result.variants.tracyLocal.netMedian
	result.totalPerFrameMs = (tracyNet + flushPerFrame) * 1000
	result.totalLocalPerFrameMs = (localNet + flushPerFrame) * 1000
	table.insert(
		lines,
		string.format(
			"[TracyBench] flush: %d flushes, %.3f ms each, %.4f ms per frame amortized; %.0f KB/s sent; batches sent %d, failed %d, dropped %d; buffer %d KB",
			flushes,
			result.flush.meanMs,
			result.flush.perFrameMs,
			result.flush.bytesPerSecond / 1024,
			result.flush.sent,
			result.flush.failed,
			result.flush.dropped,
			math.floor(s1.BufferBytes / 1024)
		)
	)
	table.insert(
		lines,
		string.format(
			"[TracyBench] HTTP round trip (off the game thread): %.1f ms mean, %.1f ms worst while measuring, %.1f ms worst incl. first request",
			result.flush.rttMeanMs,
			result.flush.rttWindowMaxMs,
			result.flush.rttMaxMs
		)
	)
	table.insert(
		lines,
		string.format(
			"[TracyBench] TOTAL per frame (zones + flush): %.4f ms via Tracy.X, %.4f ms via locals  (target < 0.05 ms)",
			result.totalPerFrameMs,
			result.totalLocalPerFrameMs
		)
	)
	print(table.concat(lines, "\n"))
	script:SetAttribute("Report", HttpService:JSONEncode(result))
end

connection = RunService.Heartbeat:Connect(function()
	frame += 1
	if frame == WARMUP then
		statsStart = Tracy.GetStats()
		clockStart = os.clock()
	end
	local measuring = frame > WARMUP
	local n = #variants
	for k = 0, n - 1 do
		local v = variants[(frame + k) % n + 1]
		-- One Tracy variant per frame, so each frame records exactly ZONES zones and
		-- the flush cost matches a game with that many.
		local skip = (v.name == "tracy" and frame % 2 == 1) or (v.name == "tracyLocal" and frame % 2 == 0)
		if not skip then
			local t0 = os.clock()
			v.run()
			local dt = os.clock() - t0
			if measuring then
				table.insert(samples[v.name], dt)
			end
		end
	end
	Tracy.FrameMark()
	if measuring then
		-- Worst round trip inside the measured window. The overall max also counts
		-- the playtest's first request.
		local last = Tracy.GetStats().LastRoundTripSeconds
		if last > windowRttMax then
			windowRttMax = last
		end
	end
	if frame == WARMUP + FRAMES then
		report()
	end
end)
