--[[
	Tracy-Luau example: a part fountain (server half).

	Setup: Tracy.luau in ReplicatedStorage as "Tracy", this Script in
	ServerScriptService, Fountain.client.luau in StarterPlayerScripts. Start
	tracy-roblox-bridge, connect the Tracy viewer to 127.0.0.1, press Play.

	Unlike the demos, nothing here busy-waits. Every Heartbeat the server launches
	parts, counts the ones in the basin, and removes old ones, each step in its own
	zone. The spawn rate changes every few seconds, with a message at each wave:
	watch the zones and the "Fountain parts" plot follow it.
]]

local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")

local Tracy = require(ReplicatedStorage:WaitForChild("Tracy"))

local WAVES = { 30, 90, 160, 60 } -- parts per second
local WAVE_LENGTH = 4 -- seconds
local LIFETIME = 5 -- seconds before a part is removed
local MAX_PARTS = 600

-- The fountain stands on the ground 35 studs in front of the spawn.
local spawnLocation = workspace:FindFirstChildOfClass("SpawnLocation")
local base = if spawnLocation
	then spawnLocation.Position - Vector3.new(0, spawnLocation.Size.Y / 2, 35)
	else Vector3.new(0, 0, -35)

local folder = Instance.new("Folder")
folder.Name = "Fountain"
folder.Parent = workspace

local nozzle = Instance.new("Part")
nozzle.Name = "Nozzle"
nozzle.Shape = Enum.PartType.Cylinder
nozzle.Size = Vector3.new(4, 3, 3)
nozzle.CFrame = CFrame.new(base + Vector3.new(0, 2, 0)) * CFrame.Angles(0, 0, math.pi / 2)
nozzle.Anchored = true
nozzle.CanQuery = false
nozzle.Color = Color3.fromRGB(90, 90, 100)
nozzle.Parent = folder

local basinCFrame = CFrame.new(base)
local basinSize = Vector3.new(40, 8, 40)
local overlap = OverlapParams.new()
overlap.FilterType = Enum.RaycastFilterType.Include
overlap.FilterDescendantsInstances = { folder }

Tracy.ConfigurePlot("Fountain parts", { Step = true, Fill = true, Color = 0x3399FF })
Tracy.ConfigurePlot("Parts in basin", { Color = 0x66CC66 })

local born: { [BasePart]: number } = {}
local count = 0
local debt = 0
local wave = 0
local waveStart = -math.huge

local function launch(now: number)
	local part = Instance.new("Part")
	part.Size = Vector3.one * (0.6 + math.random() * 0.8)
	part.Color = Color3.fromHSV(math.random(), 0.7, 1)
	part.Position = base + Vector3.new(0, 5, 0)
	part.Parent = folder
	part.AssemblyLinearVelocity = Vector3.new(math.random(-12, 12), 95 + math.random() * 25, math.random(-12, 12))
	born[part] = now
	count += 1
end

RunService.Heartbeat:Connect(function(dt)
	local now = os.clock()
	if now - waveStart >= WAVE_LENGTH then
		waveStart = now
		wave += 1
		local rate = WAVES[(wave - 1) % #WAVES + 1]
		Tracy.Message(`Fountain: wave {wave}, {rate} parts/s`, 0x3399FF)
	end

	Tracy.ZoneBegin("Fountain::Step")

	Tracy.ZoneBegin("Fountain::Launch", 0x3399FF)
	debt = math.min(debt + WAVES[(wave - 1) % #WAVES + 1] * dt, 10)
	while debt >= 1 and count < MAX_PARTS do
		launch(now)
		debt -= 1
	end
	Tracy.ZoneEnd()

	Tracy.ZoneBegin("Fountain::Scan", 0x66CC66)
	local landed = #workspace:GetPartBoundsInBox(basinCFrame, basinSize, overlap)
	Tracy.ZoneEnd()

	Tracy.ZoneBegin("Fountain::Cleanup", 0xFF8800)
	for part, t in born do
		if now - t > LIFETIME or part.Position.Y < base.Y - 50 then
			born[part] = nil
			part:Destroy()
			count -= 1
		end
	end
	Tracy.ZoneEnd()

	Tracy.ZoneEnd()

	Tracy.Plot("Fountain parts", count)
	Tracy.Plot("Parts in basin", landed)
	Tracy.FrameMark()
end)
