--[[
	Tracy-Luau example: a part fountain (client half). Put this LocalScript in
	StarterPlayerScripts, next to Fountain.server.luau. It shows up in Tracy as the
	"Roblox Client 1" track.

	Every rendered frame it finds the highest part in the air and outlines it. The
	zone's cost grows with the number of parts, so it follows the server's waves.
]]

local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")

local Tracy = require(ReplicatedStorage:WaitForChild("Tracy"))

local folder = workspace:WaitForChild("Fountain")
local nozzle = folder:WaitForChild("Nozzle")

local highlight = Instance.new("Highlight")
highlight.FillTransparency = 1
highlight.OutlineColor = Color3.new(1, 1, 1)
highlight.Parent = workspace

RunService.RenderStepped:Connect(function()
	Tracy.ZoneBegin("Fountain::FindHighest", 0xCC66FF)
	local best, bestY = nil, -math.huge
	for _, part in folder:GetChildren() do
		if part ~= nozzle and part:IsA("BasePart") then
			local y = part.Position.Y
			if y > bestY then
				best, bestY = part, y
			end
		end
	end
	highlight.Adornee = best
	Tracy.ZoneEnd()

	if best then
		Tracy.Plot("Highest part (studs)", bestY - nozzle.Position.Y)
	end
	Tracy.FrameMark()
end)

Tracy.Message("Fountain client started", 0xCC66FF)
