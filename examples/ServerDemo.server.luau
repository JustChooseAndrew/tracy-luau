--[[
	Tracy-Luau server demo. Put Tracy.luau in ReplicatedStorage as "Tracy", this
	Script in ServerScriptService, start tracy-roblox-bridge, connect the Tracy
	viewer, press Play. Pair it with ClientDemo.client.luau for a client track.

	The zones busy-wait for known times, so the numbers in Tracy can be checked
	against the code: Demo::Physics ~1.0 ms, three Demo::AI::Agent ~0.2 ms each,
	Demo::Update ~1.6 ms per frame. Once a second the server marks "ping sent"
	and fires a RemoteEvent; the client marks "ping received". On Tracy's timeline
	each "received" lands a few ms after its "sent", which shows the server and
	client tracks are aligned.
]]

local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")

local Tracy = require(ReplicatedStorage:WaitForChild("Tracy"))

local ping = Instance.new("RemoteEvent")
ping.Name = "TracyDemoPing"
ping.Parent = ReplicatedStorage

local function spin(ms: number)
	local stop = os.clock() + ms / 1000
	while os.clock() < stop do
	end
end

Tracy.ConfigurePlot("Demo::Agents busy", { Format = "Percentage", Color = 0xFF5500 })

local frame = 0
local lastPing = os.clock()

RunService.Heartbeat:Connect(function()
	frame += 1
	Tracy.ZoneBegin("Demo::Update")

	Tracy.ZoneBegin("Demo::Physics", 0x3399FF)
	spin(1.0)
	Tracy.ZoneEnd()

	Tracy.ZoneBegin("Demo::AI", 0xFF5500)
	for _ = 1, 3 do
		Tracy.ZoneBegin("Demo::AI::Agent")
		spin(0.2)
		Tracy.ZoneEnd()
	end
	Tracy.ZoneEnd()

	Tracy.ZoneEnd()
	Tracy.Plot("Demo::Agents busy", 50 + 50 * math.sin(frame / 30))
	Tracy.FrameMark()

	local now = os.clock()
	if now - lastPing >= 1 then
		lastPing = now
		Tracy.Message("Demo: ping sent", 0xFFAA00)
		ping:FireAllClients()
	end
end)

Tracy.Message("Demo: server started")
