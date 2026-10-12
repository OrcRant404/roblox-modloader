# Compiled workspace mod

The implementation exposes `modloader["workspace"]` through the API registry built into the RML loader. It is restricted to the marked, guarded Studio test copy. The source Studio installation is unchanged. The loader source addition and Workspace mod now live in this RML repository. The source includes optional ordinary A/B regression consumers and no VFX plugin.

Live acceptance passed in the separate Studio process 66258 on 2026-10-11 (2026-10-12 UTC), using Studio `0.741.19.7411056`, Qt `5.15.9`, and the installed binary matching the build. Ordinary plugin loading, plugin panel controls, real Command Bar controls, both A/B stack paths, and actual plugin toolbar clicks passed. No new VFX plugin was built or loaded by this native test profile.

That test process is now closed. This evidence describes the earlier standalone Core prototype. The global and export registry have since moved into RML Core, and Workspace links to the rebuilt loader. Diagnostic providers were removed. Integrated-loader validation is recorded separately; the historical results below do not establish a live run of the new Core binary.

## Integrated-loader acceptance

The rebuilt RML Core and Workspace passed a separate live run in test process 63443 on 2026-10-12, on the same verified Studio build. The [Core-only run](evidence/core-integration/runtime-summary.json) first proved `modloader.Version` in the real Command Bar and an ordinary local plugin with zero external native mods. The [integrated Workspace run](evidence/core-integration/workspace-runtime-summary.json) then loaded only Workspace and the optional ordinary A/B consumers.

Both plugins opened and closed their panels, registered their views, and connected both lifecycle signals. Command Bar requests entered A, covered it with B, selected A to remove B, then exited A to restore normal Studio. Stack sizes were 1, 2, 1, and 0. All six plugin callbacks arrived in the expected order with matching request IDs. The real viewport moved between the 40% A and 65% B layouts. Output closed and reopened; closing Viewport failed safely and left the place open.

This rerun found and fixed a startup race: completing one registration had briefly marked another queued view as unregistered, which removed its listeners. Pending registrations now remain `registering` until their own GUI job resolves. A deterministic host regression failed before the fix and passed afterward; all six native test groups passed after the fix. Shared Roblox settings and Preferences remained write-denied in the live test process.

## Historical standalone acceptance

| Check | Observed result |
| --- | --- |
| Ordinary A/B consumers | Each creates its widget with Roblox's Async constructor, registers its panel and view, and opens/closes its panel through the native namespace. |
| A layout | Blue A panel on the left: 510 px; real viewport on the right: 767 px; native splitter: 3 px. Requested split: 40% of 1280 px. |
| B layout | Real viewport on the left: 829 px; purple B panel on the right: 448 px; splitter: 3 px. Requested split: 65% of 1280 px. |
| Command Bar cycle 1 | Enter A → Enter B → Exit B → A resumes → Exit A → normal. |
| Command Bar cycle 2 | Enter A → Enter B → Enter A removes B → Exit A → normal. |
| Ordinary toolbar cycle | Clicking Enter A → Enter B → Enter A → Exit A completes the same unwind path. |
| Events | All 18 events across the three cycles match the expected order. Outgoing Exited precedes incoming Entered. Each event has the native request ID. |
| Repeat active Enter | Returns `noop`; no duplicate Entered event or stack frame. |
| Built-in controls | Command Bar closes/reopens Output. Closing Viewport returns a failed request and preserves the place document. |
| Restoration | All seven originally visible panel XML records match after the first cycle; visible panel positions/sizes match after subsequent cycles. A's panel reopens at its original 560 × 541 floating extent. |

Screenshots: [normal baseline](evidence/workspace-mod/normal-baseline-final.jpg), [A](evidence/workspace-mod/view-A-final.jpg), [B](evidence/workspace-mod/view-B-final.jpg), [resumed A](evidence/workspace-mod/resumed-A-final.jpg), [A selected beneath B](evidence/workspace-mod/returned-A-final.jpg), [toolbar B](evidence/workspace-mod/toolbar-view-B-final.jpg), and [normal restored](evidence/workspace-mod/normal-restored-final.jpg). The [runtime summary](evidence/workspace-mod/runtime-summary.json) contains request results, native geometry reports, panel inventories, and ordered event records. Files without the `final` suffix document earlier attempts and are not acceptance evidence.

Qtitan insertion did not honor the requested proportions alone. The implementation captures its new layout, changes the validated branch size hints, reloads it, then invalidates and activates the real Qt layout. Verification measures visible leaves and the document's actual tab strip; Qtitan's logical containers can have unused placeholder rectangles. All native parent/order, minimum size, containment, coverage, and split proportion checks remain active. The earlier rejected attempts restored normal Studio and sent no success event.

## Consumer API

Create widgets through Roblox's normal asynchronous method, then register them:

```luau
local W = modloader["workspace"]
local widgetId = "my-plugin.Graph"
local widget = plugin:CreateDockWidgetPluginGuiAsync(widgetId,
    DockWidgetPluginGuiInfo.new(Enum.InitialDockState.Float, true, true, 560, 540, 260, 220))
local panelId, err = W.RegisterPanel(plugin, widgetId, widget)
assert(panelId, err)

local view, reason = W.RegisterView(plugin, "my-plugin.Editing", {
    Direction = "Horizontal", Size = UDim.new(0.4, 0),
    First = panelId, Second = W.GetBuiltinPanel("Viewport"),
})
assert(view, reason)
view.Entered:Connect(function(event)
    print("entered", event.reason, event.requestId)
end)
view.Exited:Connect(function(event)
    print("exited", event.reason, event.requestId)
end)
local request, errorMessage = W.EnterView(view)
assert(request, errorMessage)
-- GetRequest(request.id) reports completion. This call queues native GUI work.
```

Use namespaced widget creation IDs. The current association uses the exact creation ID supplied by the caller because Roblox exposes no public getter; a missing or ambiguous native match fails safely. `CreatePanel` returns a controlled unsupported error before invoking either yielding constructor. The existing Roblox constructor keeps its normal return value.

`OpenPanel` and `ClosePanel` accept registered panel IDs or supported built-in handles: Viewport, Output, Explorer, Properties, and Toolbox. Closing the Viewport is rejected because it would close the place document. Layouts include the real Viewport once, place the registering plugin's own panels and built-ins, and temporarily close omitted dock panels.

`GetView`, `EnterView`, `ExitView`, `UnregisterView`, `GetState`, `GetViewStack`, `GetRequest`, `InspectPanels`, and `RecoverLayout` provide view control and diagnostics. Layout requests return frozen records with `id`, `status`, `error`, and `report`. Status is queued, applied, noop, failed, cancelled, or recovery-required. `InspectPanels().panels` is the latest completed inventory; wait for its request before using a subsequent inventory read.

View handles contain `Id`, `Name`, `Entered`, and `Exited`. Signals support `:Connect()` and connections support `:Disconnect()`. The outgoing Exited event precedes the incoming Entered event after the layout and stack commit. Callbacks are nonyielding; use `task.defer` for asynchronous work. Listener errors do not reverse a completed transition or block later notifications. Calls made by a listener join the queue after current event delivery.

A → B → Exit B resumes A. A → B → Enter A removes B and resumes A. Exit A then restores normal Studio. Entering the already active view is a noop with no duplicate events. Each cover captures the current layout so manual changes can resume. Plugin unloading cancels its pending panel actions and queues removal of its views. If restoration fails, cleanup remains pending; successful `RecoverLayout` retries removal. Released owners receive no later events.

## Build and launch

Run from `examples/workspace/` after building RML with its native API addition. The loader build artifact must be supplied explicitly:

```sh
STUDIO_APP="/path/to/the/matching/RML/RobloxStudio.app"
RML_LOADER="/path/to/rebuilt/libroblox_modloader.dylib"
python3 scripts/prepare_test_studio.py --studio "$STUDIO_APP"
python3 scripts/build_workspace.py --loader "$RML_LOADER"
python3 scripts/test_workspace.py
python3 scripts/prepare_native_api_test.py --workspace --studio "$STUDIO_APP" --loader "$RML_LOADER"
python3 scripts/test_profile_guard.py
python3 scripts/launch_test_studio.py --workspace-api --place build/workspace-test/WorkspaceApiTest.rbxlx
```

Close the test copy before preparing it. Base preparation creates a marked copy with zero mods. Deployment replaces only the copied loader and installs only `workspace`. For a Core-only run, omit `--workspace` during deployment and launch with `--native-api --place build/global-test/NativeApiTest.rbxlx`; no external native mods are installed in that profile. The profile guard must pass before launching. These commands require macOS, the matching source Studio, and permission for process inspection and the standalone sandbox probe.

To reproduce ordinary-plugin checks, add `--test-plugins` to the workspace build. It exports A/B packages into `build/workspace-test/plugins`; a default build generates none. There is no ModloaderApi provider, API bootstrap, or HTTP bridge.

In the fenced test run, set Studio Settings → Plugins Dir to this project's `build/workspace-test/plugins` and choose Restart Later. Close and reopen the disposable place in that same process to load the exported A/B packages normally. The path is session-only because the sandbox denies writes to shared Roblox settings and Preferences. Do not restart Studio outside the fenced launcher. Alternatively, save a selected enabled consumer Script through Plugins → Save as Local Plugin into that exact folder. Putting an absolute path in the filename field can create an invalid filename. The shared plugins directory is not modified.

The ordinary consumer source is `scripts/test-fixtures/workspace-consumer.luau`; the opt-in build generates both `.rbxmx` packages. It checks its panel open/close requests on startup and prints WORKSPACE_PLUGIN_READY. Its Workspace Test A/B toolbar actions call the native API. The real Command Bar can also call `W.EnterView(W.GetView("workspace-test.A"))`, the equivalent B call, and `W.ExitView(...)`.

After loading those optional plugins in the test Studio, choose the Plugins ribbon tab and use **Enter A**, **Enter B**, and **Exit A/B**. The toolbar scripts report request IDs and await completion. For a Command Bar example:

```luau
local W = modloader["workspace"]
local request, err = W.EnterView(W.GetView("workspace-test.A"))
assert(request, err)
print(request.id) -- Then W.GetRequest(request.id) reports the completed result.
```

Run all six native regression groups after configuring the RML Core build:

```sh
python3 scripts/test_workspace.py
# For a different CMake build folder: add --core-build /path/to/configured/build.
```

Core injector/binder, registry, stack, Lua adapter, and host tests run under ASan/UBSan with strict compiler warnings. Docking tests validate the preserved native XML fixture, nested UDim allocation, real leaf geometry, malformed input, and the observed default-size failure cases. These tests do not launch Studio; live validation is a separate check.

## Limits

This is a pinned ARM64/macOS prototype with image and symbol guards. It is not deployed to default RML. Core loads automatically; Workspace changes require restart. Namespaces are read-only snapshots and do not hot refresh. There are 64 process-lifetime export slots, 32 live plugin owners, 128 listeners, 1024 live Lua references, 128 queued normal requests, and bounded request-result retention.

The workspace mod does not own camera state or VFX preview content. Settled CFrame, Focus, FieldOfView, and CameraType matched through the second cycle and the toolbar cycle; this is observation, not a general camera restoration API. Studio normalized Focus after the initial deliberate camera reset, so that immediate pre-frame value is excluded from the comparison. Closed panels may retain stale cached rectangles; visible normal panels restored exactly and reopening A restored its floating size.

Per-place persistence, seven-day expiry, injected Studio Settings, timeline, node editing, resizing and splitter interaction for this new API, other Studio builds, and crash recovery remain later work. Plugin unloading and recovery retry are covered by native lifecycle tests; unloading an active ordinary consumer in the live GUI is not part of this acceptance. If the mod unloads and restoration fails, it retains in-process diagnostic state and snapshots, but revoked exports cannot supply a public retry; restart/crash recovery is not established.

Compiler checks and ASan/UBSan tests cover the native registry, view stack, adapter, and host lifecycle. The adapter uses a provider-owned Lua registry table: Studio's private reference API crashed on the first attempt because its internal table was uninitialized. That API is now disabled. Both legacy and current widget creation methods yield, so the adapter no longer invokes either inside its native protected call.
