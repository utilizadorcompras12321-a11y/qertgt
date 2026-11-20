# 32 — Capture retirement signal: RHI-thread command fence

Status: retirement-path audit and proposed handoff. Based on UE4.27 source. No engine source was changed; no compile or GPU test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related:
- [Patch 29 — Capture resource owner](29-capture-resource-owner-and-deferred-retirement-design.md)
- [Patch 30 — Local Vertex Factory resource lifecycle](30-local-vertex-factory-resource-lifecycle-audit.md)
- [Patch 31 — Capture RHI resource retention](31-capture-rhi-resource-retention-audit.md)

## 1. Draw submission path

The relevant UE4.27 helper path is:

1. `DrawDynamicMeshPass()` builds the one-frame mesh command storage and visible command array.
2. It calls `DrawDynamicMeshPassPrivate()`.
3. `DrawDynamicMeshPassPrivate()` calls `SubmitMeshDrawCommandsRange()`.
4. `SubmitMeshDrawCommandsRange()` calls `FMeshDrawCommand::SubmitDraw()`.
5. `SubmitDraw()` calls `FMeshDrawShaderBindings::SetOnCommandList()`, sets streams and issues draw calls through the supplied `FRHICommandList&`.

Those RHI methods record commands when command-list bypass is disabled. Returning from `DrawDynamicMeshPass()` therefore proves that the helper finished building/submitting renderer commands to its supplied list; it does not, by itself, prove that the RHI thread has consumed the recorded commands.

This confirms Patch 31's rule: do not release capture-owned RHI refs merely because the dynamic-pass callback returned.

## 2. UE4.27 has an explicit RHI-thread command fence

In `RHI/Public/RHICommandList.h`, `FRHICommandListImmediate::RHIThreadFence(bool)` is available.

In `RHI/Private/RHICommandList.cpp`:

- When `IsRunningRHIInSeparateThread()` is true, `RHIThreadFence()` allocates an `FRHICommandRHIThreadFence` command and returns its `FGraphEventRef`.
- The fence command executes on the RHI thread and dispatches its graph event only when that command is reached.
- `WaitOnRHIThreadFence()` dispatches pending immediate-list commands if necessary, then waits for the returned event.
- When there is no separate RHI thread, `RHIThreadFence()` returns a null event; there is no separate RHI-thread queue to wait for.

This is a more precise signal for the ownership problem in Patch 31 than `FFrameEndSync` or a plain `FRenderCommandFence`.

## 3. What the RHI-thread fence guarantees

If inserted on the rendering thread after the final capture commands, then dispatched in order, the fence event signals that the RHI thread has executed the fence command. Therefore all earlier commands in that ordered RHI stream—including raw texture/SRV/UAV/sampler/UB bindings and render-pass commands—have been consumed by the RHI thread.

It does **not** mean the GPU has finished executing the submitted work. It is a CPU-side RHI command-consumption fence, not a GPU-completion fence.

That distinction is acceptable for the immediate raw-pointer hazard if the owner retains all RHI refs until this event completes, because the RHI commands have then finished dereferencing the borrowed pointers. After releasing the final owning refs, UE4.27's `FRHIResource::Release()` uses the engine's deferred-deletion policy when running threaded rendering or when the platform requests extra deletion latency. That policy is the engine's normal resource-destruction mechanism; it must not be misrepresented as a caller-controlled GPU fence.

If a future capture resource requires a stronger explicit GPU-completion guarantee than the normal RHI deferred-deletion contract, it needs a separate backend-verified GPU fence/readback retirement design. Do not infer that guarantee from `RHIThreadFence()`.

## 4. Why FFrameEndSync is not the generation's retirement contract

`FFrameEndSync::Sync()` calls `FRenderCommandFence::BeginFence(true)`, but `FRenderCommandFence::BeginFence(true)` can downgrade to render-thread-only synchronization when `GTSyncType == 0` or `r.VSync == 0`.

When it does take the RHI/GPU path, it uses `FRHISyncFrameCommand` with the configured GT sync type. That is frame pacing/synchronization policy, not a deterministic per-capture-generation completion contract.

Do not make resource safety depend on the current VSync or GT sync settings. Also do not assume that the pending-cleanup frame boundary always implies that the RHI thread has consumed every command referencing a particular generation.

## 5. Proposed non-blocking retirement handoff

The preferred shape is an asynchronous two-stage handoff, not a per-generation Game Thread flush:

### Stage A — close and drain render-thread producers

- Mark the capture generation as closing; reject new work.
- Ensure no queued render callback can still build a chart draw or enqueue a command referencing the generation.
- After the final capture draw and all associated render-target resolve/copy commands have been recorded on the immediate RHI command list, append `RHIThreadFence()`.
- Dispatch the immediate command list to the RHI thread in the normal ordered way.

The fence must be appended after the final command that uses the generation. A fence inserted earlier is insufficient.

### Stage B — retire after fence completion

- Retain the generation owner and all its RHI refs while the fence event is pending.
- When the event completes, schedule retirement back onto the appropriate owning thread.
- Queue `BeginReleaseResource(&ChartVF)` after its final render-thread use; keep the VF C++ object alive until that release command has executed.
- Release capture-owned RHI refs only after the RHI command-consumption fence has completed.
- Use `BeginCleanup()` for deferred C++ owner destruction only after the VF is uninitialized and all owner references have been retired.

When no separate RHI thread is running, the fence event is null. In that configuration, verify that the chosen immediate-list submission path has synchronously executed its commands before advancing to retirement; do not treat a null event as an asynchronous completion event.

## 6. Important integration constraint

`DrawDynamicMeshPass()` accepts `FRHICommandList&`, not specifically `FRHICommandListImmediate&`. `RHIThreadFence()` belongs to `FRHICommandListImmediate`.

Therefore do not call `RHIThreadFence()` from inside a generic dynamic-pass lambda unless the concrete command-list type and ordering are proven. A practical integration is to append the fence in the enclosing render-thread command/capture orchestration after `DrawDynamicMeshPass()` returns, using the same immediate command list that received the draw commands.

If the capture work is recorded into a non-immediate command list and later submitted/merged, the fence must be placed after that list's submission in the parent immediate list—not on an unrelated command list.

## 7. FRenderResource release remains a separate prerequisite

The RHI-thread fence solves the raw-pointer command-consumption interval. It does not remove the requirement from Patch 30:

- `FLocalVertexFactory` must be released on the rendering thread using `BeginReleaseResource()` or `ReleaseResource()` in the correct thread context.
- Its object must remain alive until the release command executes.
- The VF must be uninitialized before its C++ destructor runs.

The release command must be ordered after the last render-thread use. Do not queue release while another render callback can still submit a draw using that VF.

## 8. Decision

Use `RHIThreadFence()` as the candidate explicit signal for **RHI command consumption**, retain generation-owned RHI refs until its event completes, and rely on UE4.27's standard RHI deferred-deletion behavior after the final refs are released.

This closes the main raw-pointer lifetime gap identified in Patch 31 at the design level. It does not yet constitute an implementation proof: the actual capture call site must establish that all draw, resolve and copy commands use the same ordered immediate-list stream before the fence is inserted.

## Source paths

- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`
- `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.inl`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/Engine/Public/UnrealEngine.h`
- `Engine/Source/Runtime/Engine/Private/UnrealEngine.cpp`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
