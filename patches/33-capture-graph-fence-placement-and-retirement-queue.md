# 33 — Capture graph fence placement and non-blocking retirement queue

Status: concrete integration audit based on UE4.27 source. This patch refines Patch 32 into an implementable sequencing contract. No engine source was changed; no compile or GPU test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related:
- [Patch 29 — Capture resource owner](29-capture-resource-owner-and-deferred-retirement-design.md)
- [Patch 30 — Local Vertex Factory resource lifecycle](30-local-vertex-factory-resource-lifecycle-audit.md)
- [Patch 31 — Capture RHI resource retention](31-capture-rhi-resource-retention-audit.md)
- [Patch 32 — Capture RHI-thread retirement fence](32-capture-rhi-thread-retirement-fence.md)

## 1. Confirmed RDG execution order

In `RenderCore/Public/RenderGraphBuilder.h`, `FRDGBuilder` is constructed with an `FRHICommandListImmediate&`.

In `RenderCore/Private/RenderGraphBuilder.cpp`, `FRDGBuilder::Execute()` iterates the compiled graph and calls `ExecutePass()` for each live pass. `ExecutePass()` chooses the builder's graphics command list (or the async-compute list for async passes), then calls prologue, pass body, and epilogue in that order.

For raster passes:
- `ExecutePassPrologue()` emits `BeginRenderPass()` before the pass lambda.
- `Pass->Execute()` invokes the pass lambda.
- `ExecutePassEpilogue()` emits `EndRenderPass()` after the pass lambda, then submits epilogue barriers.

**A fence inside a raster pass lambda is too early:** it is inserted before `EndRenderPass()`. For a standalone capture graph, append the fence only after `GraphBuilder.Execute()` returns. If capture is integrated into a renderer-owned graph, the outer graph owner must append the fence after its `Execute()`; an individual capture pass must not do so.

## 2. Dynamic mesh pass placement

`DrawDynamicMeshPass()` in `Renderer/Public/MeshPassProcessor.inl` builds one-frame mesh command storage, invokes its build lambda, then calls `DrawDynamicMeshPassPrivate()`.

`DrawDynamicMeshPassPrivate()` in `Renderer/Private/MeshPassProcessor.cpp` calls `SubmitMeshDrawCommandsRange()`, which calls `FMeshDrawCommand::SubmitDraw()` for each visible command using the supplied `FRHICommandList&`.

The capture draw therefore records its RHI operations into the list supplied by the caller. In an RDG pass, the graph still emits the raster epilogue after the callback returns.

For a non-RDG one-shot capture, a fence may be appended after `DrawDynamicMeshPass()` returns, provided no later command uses that generation.

## 3. Correct fence and dispatch placement

For a standalone capture graph:

```cpp
FRDGBuilder GraphBuilder(RHICmdList);

// Add capture raster pass.
// Add resolve / copy passes.
// The raster pass may call DrawDynamicMeshPass().

GraphBuilder.Execute();

// After all graph pass bodies, epilogues, barriers and recorded copies.
FGraphEventRef CaptureRHIConsumedFence = RHICmdList.RHIThreadFence();
RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);

// Transfer owner + fence to render-thread retirement tracking.
```

This is a sequencing sketch, not complete compile-ready code. The graph owner must ensure no later graph or immediate-list operation uses the generation before the fence.

The immediate flush is required to dispatch the fence command and all preceding commands. Do not store the event and assume it will progress if the immediate list has not been dispatched.

## 4. No-separate-RHI-thread path

In `RHI/Public/RHICommandList.inl`, `ImmediateFlush(DispatchToRHIThread)` calls `GRHICommandList.ExecuteList(*this)` when commands exist.

In `RHI/Private/RHICommandList.cpp`, when `IsRunningRHIInSeparateThread()` is false, `FRHICommandListExecutor::ExecuteInner()` falls through to `ExecuteInner_DoExecute()`, which executes the commands on the calling thread.

Thus:
- `RHIThreadFence()` returns null when there is no separate RHI thread.
- `ImmediateFlush(DispatchToRHIThread)` still consumes the immediate command list synchronously in that mode.
- A null fence is eligible for retirement only after that dispatch flush returns.

Do not treat a null fence as completed before dispatching the immediate command list.

## 5. Non-blocking retirement queue

Do not call `WaitOnRHIThreadFence()` for every generation immediately after submission. Its implementation waits on the Render Thread if the event is incomplete.

UE4.27 has non-blocking precedents:
- `FShaderPipelineCache::ReadyForPrecompile()` checks `LastPrecompileRHIFence->IsComplete()` and clears it only after completion.
- `FRHILockTracker::FlushCompleteUnlocks()` scans outstanding fence events and removes only completed entries.
- `FRenderTargetPool::WaitForTransitionFence()` is an explicit blocking path, not the routine polling model.

Use a renderer-owned queue accessed only on the rendering thread. Each entry retains a strong owner reference and its nullable `FGraphEventRef`.

At a later render-thread maintenance point:
1. Iterate pending entries.
2. If the fence exists and is incomplete, leave the entry queued.
3. If the fence is null, this entry is eligible only because the dispatch flush already returned synchronously.
4. Once eligible, call `ChartVF.ReleaseResource()` on the rendering thread.
5. Assert the VF is no longer initialized.
6. Transfer the owner to deferred C++ cleanup and remove the queue entry.

Do not poll from the Game Thread. Do not destroy the owner while the VF is initialized. Do not use `WaitOnRHIThreadFence()` in this normal polling loop.

The queue must outlive every generation it tracks. Its shutdown path must explicitly drain pending entries before renderer/module teardown.

## 6. RHI consumption is not GPU completion

The event is dispatched by `FRHICommandRHIThreadFence::Execute()` after the RHI thread reaches that command. It proves earlier commands in the ordered RHI stream have been consumed by the RHI thread. It does not prove that the GPU has completed execution.

After command consumption, the owner can release strong RHI references that were required to keep raw pointers valid during command execution. UE4.27's normal RHI deferred-deletion policy then governs backend destruction. Do not add a GPU-idle wait or per-capture flush as a shortcut.

## 7. Remaining implementation gate

Two project-specific details remain:
1. Identify the exact renderer hook that owns the outer `FRDGBuilder` for Software RT capture. If capture is inserted into an existing graph, the fence belongs after that graph's `Execute()`, not inside the capture lambda.
2. Define the renderer-owned retirement queue's creation, polling, and shutdown location. It must poll on the render thread and outlive pending generations.

Do not implement the queue as a global static with unspecified module shutdown ordering. Prefer an owner tied to the renderer/scene extension that creates the capture generations.

## Source paths

- `Engine/Source/Runtime/RenderCore/Public/RenderGraphBuilder.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraphBuilder.cpp`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`
- `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.inl`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RenderCore/Private/ShaderPipelineCache.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIUtilities.h`
- `Engine/Source/Runtime/Renderer/Private/SceneOcclusion.cpp`
