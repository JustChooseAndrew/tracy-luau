--[[
	Tracy-Luau example: MicroProfiler captures from a live game.

	Put this Script in ServerScriptService, with Tracy in ReplicatedStorage and
	LibMP (https://github.com/Roblox/libmp) in ServerStorage. In a published game,
	each sampled server captures its last 64 frames when a frame takes 150 ms or
	more, at most every 10 minutes, and stores the capture in the "TracyCaptures"
	DataStore. Download and convert them on your machine:

	    python tools/fetch_captures.py --universe <id> --convert --delete

	In Studio the same script sends captures to tracy-roblox-bridge instead (start
	it before pressing Play), which saves them under captures/ and converts them.

	Your Tracy zones show up inside the captures: outside Studio, Tracy mirrors
	them into the MicroProfiler (see MICROPROFILER at the top of the Tracy module).
]]

local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")
local ServerStorage = game:GetService("ServerStorage")

local Capture = require(ReplicatedStorage:WaitForChild("Tracy"):WaitForChild("Capture"))

local started, why = Capture.Start({
	LibMP = require(ServerStorage:WaitForChild("LibMP")),
	Sink = if RunService:IsStudio() then Capture.BridgeSink() else Capture.DataStoreSink("TracyCaptures"),
	HitchMs = 150,
	Cooldown = 600,
	MaxCaptures = 3,
	-- The MicroProfiler keeps MemoryMB of history while it records, and taking a
	-- capture stalls one frame; start with a small share of servers.
	SampleRate = if RunService:IsStudio() then 1 else 0.05,
})
if not started then
	print("[LiveCaptures] not capturing on this server: " .. tostring(why))
end

-- Capture on demand too, e.g. when something unusual starts:
-- Capture.Now("boss fight")
