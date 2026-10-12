-- Ordinary local plugin: a consumer of the compiled workspace mod.
local label = "__VIEW_LABEL__"
local Workspace = assert(modloader["workspace"], "Compiled workspace API missing")
local name = "workspace-test." .. label
local color = label == "A" and Color3.fromRGB(39, 88, 150) or Color3.fromRGB(120, 62, 152)
local localPanelId = name .. ".Panel"
local panel = plugin:CreateDockWidgetPluginGuiAsync(localPanelId, DockWidgetPluginGuiInfo.new(Enum.InitialDockState.Float, true, true, 560, 540, 260, 220))
local panelId, panelError = Workspace.RegisterPanel(plugin, localPanelId, panel)
assert(panelId, panelError)
panel.Title = "Workspace Test " .. label
local text = Instance.new("TextLabel")
text.Size = UDim2.fromScale(1, 1)
text.BackgroundColor3 = color
text.TextColor3 = Color3.new(1, 1, 1)
text.TextSize = 32
text.Text = "View " .. label .. "\nNative workspace API test"
text.Parent = panel

task.wait(0.3) -- Give Studio time to create the ordinary native widget host.
local function completed(request, reason)
    assert(request, reason)
    local deadline = os.clock() + 5
    repeat
        local state, err = Workspace.GetRequest(request.id)
        assert(state, err)
        if state.status ~= "queued" then
            assert(state.status == "applied" or state.status == "noop", state.error)
            return state.id
        end
        task.wait()
    until os.clock() >= deadline
    error("Workspace panel request did not complete: " .. request.id)
end
local opened = completed(Workspace.OpenPanel(panelId))
local closed = completed(Workspace.ClosePanel(panelId))
print("WORKSPACE_PLUGIN_PANEL_CONTROLS", label, opened, closed)
panel.Enabled = false
local viewport = Workspace.GetBuiltinPanel("Viewport")
local layout = {
    Direction = "Horizontal", Size = UDim.new(label == "A" and 0.40 or 0.65, 0),
    First = label == "A" and panelId or viewport,
    Second = label == "A" and viewport or panelId,
}
local view, err = Workspace.RegisterView(plugin, name, layout)
assert(view, err)
local enteredConnection, enteredError = view.Entered:Connect(function(event)
    print("WORKSPACE_EVENT", label, "Entered", event.reason, event.requestId)
end)
assert(enteredConnection, enteredError)
local exitedConnection, exitedError = view.Exited:Connect(function(event)
    print("WORKSPACE_EVENT", label, "Exited", event.reason, event.requestId)
end)
assert(exitedConnection, exitedError)

local toolbar = plugin:CreateToolbar("Workspace Test " .. label)
local enter = toolbar:CreateButton("WorkspaceEnter" .. label, "Enter registered view " .. label, "", "Enter " .. label)
local exit = toolbar:CreateButton("WorkspaceExit" .. label, "Exit registered view " .. label, "", "Exit " .. label)
enter.ClickableWhenViewportHidden = true
exit.ClickableWhenViewportHidden = true
enter.Click:Connect(function()
    local request, reason = Workspace.EnterView(view)
    assert(request, reason)
    print("WORKSPACE_REQUEST", "Enter", label, request.id)
    completed(request)
end)
exit.Click:Connect(function()
    local request, reason = Workspace.ExitView(view)
    assert(request, reason)
    print("WORKSPACE_REQUEST", "Exit", label, request.id)
    completed(request)
end)
print("WORKSPACE_PLUGIN_READY", label, view.Id, panelId)
