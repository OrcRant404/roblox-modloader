# Modloader and workspace API specification

Compiled native RML mods expose APIs through an injected `modloader` global. Ordinary Studio plugins and the Command Bar call those APIs. A compiled workspace mod provides panel and layout control through `modloader["workspace"]`.

The loader global and workspace API passed ordinary-plugin and Command Bar checks in the separate Studio, including panel controls, A/B view transitions, and event ordering. See [the current results](workspace-mod-results.md). Earlier VFX docking and API-probe reports remain historical evidence; their diagnostic providers have been removed. The bounded API below is implemented in `native-workspace/`. The VFX example remains a design example, and no VFX plugin is included.

## Mods provide APIs and Luau consumes them

RML loads external compiled mods, such as an HTTP unlocker or the workspace mod. Each mod registers its namespace and exported functions through the loader's built-in native registration mechanism. The loader makes those exports available to Luau:

```luau
local Workspace = modloader["workspace"]
local HttpUnlocker = modloader["http-unlocker"]
-- Each table contains the functions exported by that compiled mod.
```

Registration belongs to the compiled mod and loader. Plugins do not register mods or publish their own functions into `modloader`. There is no intervening `modloader.mods` table.

```text
Compiled mod loads and registers its namespace and functions
    -> Loader exposes that namespace through modloader
        -> Plugin or Command Bar calls an exported function
            -> Workspace mod performs the requested Studio UI operation
```

The loader owns injection and export dispatch as Core functionality, rather than a separately installed mod. The workspace mod owns panel discovery, registered views, the active view stack, docking, and restoration. A future VFX plugin would own graph contents, properties controls, camera state, and real ParticleEmitter previews.

The global must be available automatically in ordinary plugin and Command Bar environments. Defining a local variable in a test bootstrap does not establish this capability. Core exposes native C exports; Workspace uses native event connections. There is no Luau API bootstrap or HTTP bridge.

## Workspace API

All calls below are exports of `modloader["workspace"]`. Calls that change the native layout return a request record; use `GetRequest(request.id)` to read completion or failure.

| Function | Result and purpose |
| --- | --- |
| `RegisterPanel(plugin, pluginGuiId, widget)` | Associate an existing real `DockWidgetPluginGui` with its creation ID and return an opaque `PanelId`. |
| `GetBuiltinPanel(name)` | Resolve a supported built-in panel, such as `"Viewport"` or `"Output"`, to a `PanelId`. |
| `OpenPanel(panelId)` | Request that an existing panel become visible. |
| `ClosePanel(panelId)` | Request that an existing panel close. |
| `RegisterView(plugin, name, description)` | Register a complete workspace description and return a view handle with lifecycle events. |
| `GetView(name)` | Resolve a named registration to its view handle in the current Studio session. |
| `EnterView(view)` | Enter a new view, or return to its existing position in the active stack. |
| `ExitView(view)` | Leave the active view and resume the previous view, or restore normal Studio. |
| `UnregisterView(view)` | Release a registration and remove it from the active stack. |
| `GetState(view)` | Read registering, inactive, active, suspended, or unregistered state, plus the last request result and recovery flag. |
| `GetViewStack()` | Inspect a copy of the active stack ordered from the earliest view to the current view. |
| `GetRequest(requestId)` | Read queued, applied, noop, failed, cancelled, or recovery-required status. |
| `InspectPanels()` | Queue a fresh native inventory; the returned panel list is the last completed inventory. |
| `RecoverLayout()` | Retry the retained in-process recovery snapshot after a failed rollback. |

Creating a panel or registering a view associates UI resources with a plugin. It does not register a mod. Plugin unloading releases its views and panel associations; it does not remove the workspace mod's API from other consumers. A view can remain registered while absent from the active stack.

The returned view handle has a stable registration name, an opaque session ID, and `Entered` and `Exited` signals. Signals support `:Connect(callback)` and return a connection with `:Disconnect()`. They report changes in which view is active; registering a view alone fires neither signal. Resuming a suspended view uses the same handle and subscriptions.

`RegisterPanel` takes the widget returned by Studio and the exact ID used to create it. Studio's existing creation method keeps its normal return value. Both current and deprecated creation methods can yield on this build, so the nonyielding native `CreatePanel` export returns a controlled unsupported error before calling either constructor. Use normal `plugin:CreateDockWidgetPluginGuiAsync(...)`, then register the result. Native pointers stay inside the mod.

Plugin view descriptions can explicitly place their own panels and supported built-in panels. The `"Viewport"` handle refers to the existing place document area, which hosts the real Studio viewport.

## Plugin example

This describes the VFX arrangement: graph on the left, real viewport above properties on the right. Particle creation and camera handling remain in the VFX plugin.

```luau
local Workspace = modloader["workspace"]

local function panelInfo(width, height, minWidth, minHeight)
    return DockWidgetPluginGuiInfo.new(
        Enum.InitialDockState.Float,
        false, true,
        width, height,
        minWidth, minHeight
    )
end

local graph = plugin:CreateDockWidgetPluginGuiAsync("vfx.Graph", panelInfo(760, 640, 320, 240))
local graphId = assert(Workspace.RegisterPanel(plugin, "vfx.Graph", graph))
graph.Title = "VFX Graph"

local properties = plugin:CreateDockWidgetPluginGuiAsync("vfx.Properties", panelInfo(400, 260, 200, 160))
local propertiesId = assert(Workspace.RegisterPanel(plugin, "vfx.Properties", properties))
properties.Title = "VFX Properties"

local viewportId = Workspace.GetBuiltinPanel("Viewport")

local view = Workspace.RegisterView(plugin, "vfx.Editing", {
    Direction = "Horizontal",
    Size = UDim.new(0.70, 0),
    First = graphId,
    Second = {
        Direction = "Vertical",
        Size = UDim.new(0.65, 0),
        First = viewportId,
        Second = propertiesId,
    },
})

-- These callbacks use the VFX plugin's own preview and camera helpers.
view.Entered:Connect(function(event)
    if event.reason == "enter" then
        captureReturnCamera()
    elseif event.reason == "resume" then
        restoreSuspendedCamera()
    end
    enablePreviewControls()
end)

view.Exited:Connect(function(event)
    clearPreviewParticles()
    disablePreviewControls()

    if event.reason == "covered" then
        captureSuspendedCamera()
    else
        restoreReturnCamera()
    end
end)

-- Separate toolbar actions:
-- Enter button: Workspace.EnterView(view)
-- Exit button:  Workspace.ExitView(view)

-- Request progress and errors remain available to the plugin.
local state = Workspace.GetState(view)
```

The callback helpers stand for the VFX plugin's existing camera and preview logic. Entry and exit return request acknowledgements; lifecycle events report successful changes in the active view. `GetState` also exposes progress and errors. Accepting a request is not proof that panels have finished moving.

Each split gives its first child the requested size along the split axis. The second child receives the remaining space. `UDim` scale refers to the available span of that split; offset adds logical pixels. Native minimum sizes still apply. Proportional sizing and splitter interaction require live verification.

## Command Bar examples

The Command Bar uses the same namespace without loading a helper or registering a mod:

```luau
local Workspace = modloader["workspace"]
local outputId = Workspace.GetBuiltinPanel("Output")

Workspace.OpenPanel(outputId)
```

A later command can close it:

```luau
local Workspace = modloader["workspace"]
Workspace.ClosePanel(Workspace.GetBuiltinPanel("Output"))
```

It can also request a view that the VFX plugin has already registered:

```luau
local Workspace = modloader["workspace"]
Workspace.EnterView(Workspace.GetView("vfx.Editing"))
```

## View order and lifecycle events

The workspace mod keeps one active stack per Studio window. Normal Studio is the return baseline beneath that stack. Only the top view is active; views below it are suspended. Exiting a view removes it from the stack but leaves its registration available for a later entry.

| Action | Stack after success | Visible arrangement |
| --- | --- | --- |
| Enter A | A | A |
| Enter B | A, B | B |
| Exit B | A | A resumes |
| Exit A | Empty | Original normal Studio layout |

Before B covers A, the mod saves A's current layout. Exiting B restores that saved layout, including manual panel changes made while A was active. Normal Studio's original return snapshot remains unchanged until the stack becomes empty.

Selecting a view already in the stack returns to that position and removes every view above it. For example, entering A from the stack `A, B` leaves only A; exiting A then returns to normal Studio. The mod does not add another A frame or move B below A. Requesting the already active view leaves the stack unchanged and fires no duplicate lifecycle events.

Proposed event reasons:

| Event | Reason | Meaning |
| --- | --- | --- |
| `Entered` | `"enter"` | A registered view becomes active from outside the stack. |
| `Entered` | `"resume"` | A suspended view becomes active again. |
| `Exited` | `"covered"` | Another view becomes active above it; it remains in the stack. |
| `Exited` | `"exit"` | It leaves the top of the stack through exit or returning to an earlier view. |
| `Exited` | `"unregister"` | An active registration is explicitly released. |

For `Normal -> A -> B -> A -> Normal`, notifications are:

```text
A.Entered("enter")
A.Exited("covered"), B.Entered("enter")
B.Exited("exit"), A.Entered("resume")
A.Exited("exit")
```

The argument above abbreviates the event record's `reason` field. A transition commits its layout and stack before notifying listeners. It dispatches the outgoing view's `Exited` event before the incoming view's `Entered` event. A failed or cancelled entry emits no successful entry or exit notifications. Listener errors are reported without reversing an already completed transition or preventing other listeners from receiving it. Calls made by listeners join the transition queue instead of changing the stack inside the current notification.

Implemented lifecycle defaults: `ExitView` acts on the top view and rejects requests to exit a suspended view. `UnregisterView` can remove a suspended registration without disturbing the visible top view or firing a second `Exited` event for an already inactive view. Releasing the active registration resumes the nearest remaining view, or normal Studio. Plugin unloading removes all of that plugin's registrations; none can resume later. Native cleanup must succeed without depending on callbacks in an unloading plugin's Luau environment.

Plugins request entry from their own toolbar or activation handlers. A plugin covered by another view remains in the stack; its ordinary loss of tool focus must not automatically remove that view. View events provide the notifications for pausing and resuming its editor controls.

## Complete views and restoration

A view description defines the complete visible docking arrangement when applied. Dock panels omitted from it close temporarily, including other plugins' panels. Closing a panel changes its visibility; it does not destroy the widget or unload its plugin. Returning to an earlier view restores its saved arrangement; leaving the last view restores the original normal Studio arrangement.

Before mutation, the workspace mod validates the description and resolves its panel IDs. Native docking calls run on Studio's GUI thread. If a transition fails after moving panels, it restores the previously active arrangement and preserves the prior stack. If restoration also fails, it retains the stack and recovery snapshots, reports recovery as required, and emits no events claiming that the requested transition completed.

Camera state remains a plugin responsibility. On first successful entry, the VFX plugin keeps the camera it must return to. When covered, it stops its preview and remembers its active camera without restoring normal Studio or discarding its return camera. On resume, it restores the suspended camera. On final exit, it restores its return camera before the previous view receives its resume event. The workspace mod's layout operations do not themselves change the camera.

The example covers normal entry, covering, resume, and exit. Camera restoration when suspended frames are removed or owners unload needs an additional cleanup policy; callbacks in a plugin that has already unloaded cannot supply that restoration.

The loader tracks exports by compiled mod lifetime. Unloading a mod removes its namespace and invalidates cached native calls. Unloading the workspace mod must attempt to restore the normal Studio baseline and clear the stack before releasing its layout controller. Duplicate namespaces must be rejected rather than overwrite another mod's exports.

## First proof and remaining decisions

The first proof loads a compiled test mod that exports a small native function under its own namespace. Both an ordinary Studio plugin and the Command Bar must call it through the automatically injected global. Test duplicate namespaces, mod unloading, cached calls after unload, and reload. Plugins must remain consumers throughout this test.

The local RML source has global-provider and bridge machinery, but its Luau subsystem is disabled by default. The installed macOS loader lacks the bridge exports needed by that source interface. The project-owned native loader API component now establishes automatic injection on the pinned test build by hooking the verified engine loader. Both ordinary plugin and Command Bar calls passed. Native registry tests cover collisions, revocation, cached slots and replacement; live mod hot reload and table refresh remain unimplemented.

The user authorized the compiled workspace mod and live A/B panel and event tests next. This milestone implements panel association, built-in controls, registered split views, and the native view stack. It does not build or run a new VFX plugin. Camera handling and particles stay outside this mod.

View-stack acceptance must cover A -> B -> exit B -> A -> exit A -> normal; A -> B -> enter A -> exit A -> normal; three views; an already active view request; restoration of manual changes; failed or cancelled entry; failure while returning to an earlier view; unregistering an active or suspended view; and plugin or workspace-mod unloading. Check event reasons and order, persistent subscriptions across resume, callback errors, and requests made from callbacks.

Direct panel operations change the current layout without changing the view stack. Covering a view captures its current native layout, including these manual changes. Plugins and the Command Bar can operate session panel IDs. Event callbacks are nonyielding; defer asynchronous work with `task.defer`. Camera cleanup on removal or unloading remains an editor-level design decision.

Per-place persistence, the seven-day expiry setting, a node editor, and production installation remain later work.
