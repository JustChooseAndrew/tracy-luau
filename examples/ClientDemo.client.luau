--[[
	Tracy-Luau client demo. Put this LocalScript in StarterPlayerScripts and run
	ServerDemo.server.luau too: client data reaches the bridge through the server.
	It shows up in Tracy as the "Roblox Client 1" track.

	ClientDemo::Render busy-waits 0.5 ms per rendered frame, so the number in Tracy
	can be checked against the code.
]]

local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")

local Tracy = require(ReplicatedStorage:WaitForChild("Tracy"))

local function spin(ms: number)
	local stop = os.clock() + ms / 1000
	while os.clock() < stop do
	end
end

RunService.RenderStepped:Connect(function(dt)
	Tracy.ZoneBegin("ClientDemo::Render", 0x66CC66)
	spin(0.5)
	Tracy.ZoneEnd()
	Tracy.Plot("ClientDemo::Frame time (ms)", dt * 1000)
	Tracy.FrameMark()
end)

ReplicatedStorage:WaitForChild("TracyDemoPing").OnClientEvent:Connect(function()
	Tracy.Message("ClientDemo: ping received", 0x66CC66)
end)

Tracy.Message("ClientDemo: client started")
