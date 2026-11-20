# 34 — Deferred renderer capture graph integration point audit

Status: renderer integration-point audit against UE4.27. The project repository branch currently contains patch documentation and tools, not renderer source files, so this is a source-grounded integration decision rather than an edit to an existing Software RT implementation. No engine source was changed; no compile or render test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related:
- [Patch 19 — Chart rasterization through Base Pass VF hook](19-chart-rasterization-through-basepass-vertex-factory-hook.md)
- [Patch 22 — Base Pass selection and mesh clone audit](22-basepass-chart-vf-selection-and-mesh-clone-audit.md)
- [Patch 27 — Base Pass primitive and Chart-VF binding audit](27-basepass-primitive-and-chart-vf-binding-audit.md)
- [Patch 33 — Capture graph fence placement and retirement queue](33-capture-graph-fence-placement-and-retirement-queue.md)

## 1. Repository state

The root of `qertgt` on `plugin/software-rt-material-evaluation` currently contains `patches/` and `tools/`. There is no checked-in Engine source tree or Software RT renderer implementation in this branch to identify an existing graph owner or modify directly.

The actual integration target must therefore be selected from UE4.27 source and later applied to the user's local Engine checkout as a separate engine patch.

## 2. Main deferred renderer graph owner

In `Renderer/Private/DeferredShadingRenderer.cpp`, the relevant owner is:

```cpp
void FDeferredShadingSceneRenderer::Render(FRHICommandListImmediate& RHICmdList)
```

The function:
1. performs scene updates and view preparation;
2. runs the Z-prepass outside the main RDG graph;
3. dispatches ray-tracing world updates when enabled;
4. creates the main `FRDGBuilder GraphBuilder(RHICmdList, ...)`;
5. records deferred-rendering passes, including the base pass, lighting, reflections and frame-finish work;
6. calls `GraphBuilder.Execute()` near the end of the function.

This is the strongest existing owner for a capture pass that needs the normal deferred renderer's scene/view context and Base Pass material shader path.

## 3. Recommended initial capture insertion point

The conservative first integration point is in that main graph, immediately before the existing `RenderBasePass(GraphBuilder, ...)` call.

At this point:
- the Z-prepass has already populated scene depth;
- the main `FRDGBuilder` is alive;
- view and scene-texture setup for the deferred renderer has begun;
- the standard Base Pass is about to be recorded;
- the capture can use a dedicated chart atlas target without replacing the normal scene Base Pass.

The capture pass should be added to the graph as its own pass and should submit chart-VF batches through the existing Base Pass mesh processor path, as established by Patches 19–27. It must not mutate the normal scene Base Pass batches or global Static Mesh vertex factories.

Initial supported material scope should remain narrow: opaque, deferred, non-tessellated, non-masked, non-translucent, no view-dependent expressions, no WPO/PDO, and no unsupported custom depth/stencil side effects. These restrictions avoid turning this integration step into a second general-purpose material renderer.

Do not assume the capture pass can consume every scene texture merely because it is placed in the main graph. Declare graph resources it actually reads/writes and retain custom RHI resources through the Patch 33 retirement path.

## 4. Fence location for this integration

The main graph contains many passes after Base Pass. The capture pass's callback is not a valid fence insertion point because RDG emits raster `EndRenderPass()` and epilogue barriers after the callback.

For the first correctness implementation:

1. Add the capture pass before `RenderBasePass()`.
2. Allow the full main `GraphBuilder` to execute.
3. Immediately after the main `GraphBuilder.Execute()` returns, append `RHICmdList.RHIThreadFence()`.
4. Call `RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread)`.
5. Enqueue the capture generation in the render-thread retirement queue.
6. Poll `Fence->IsComplete()` in later render-thread maintenance and retire only when complete.

This intentionally retains the capture generation until the end of the deferred renderer graph, not merely until the capture pass ends. It is conservative and may retain resources longer than necessary, but it avoids an early fence and avoids blocking the Render Thread.

The graph also records later lighting/reflection/frame-finish work, so the fence is after those commands too. This does not make those later passes depend on the capture resources; it only extends the generation's owner lifetime to a safe graph boundary.

## 5. Why not fence immediately after the capture pass?

A fence appended from inside the capture pass lambda is too early for a raster pass: RDG calls `ExecutePassEpilogue()` only after the lambda returns, and that epilogue ends the render pass and submits barriers.

A fence appended from the outer renderer after the graph executes is ordered after all graph commands, including the capture pass epilogue. It is therefore the correct initial correctness boundary.

A later optimization may split capture into a dedicated standalone RDG graph and fence immediately after that graph's `Execute()`, but only if material shader dependencies, scene/view uniforms, graph resource registration and pass ordering have been proven. Do not split the graph merely to shorten owner lifetime.

## 6. Ownership and shutdown implications

The retirement queue should be owned by a renderer/scene-level object whose lifetime is guaranteed to exceed the capture generations it tracks. Avoid a global static with unclear module shutdown ordering.

At renderer teardown:
- stop creating capture generations;
- ensure no later render callback can enqueue a capture draw;
- dispatch any final immediate-list work;
- wait/drain pending RHI-thread fence events in a controlled shutdown path;
- release initialized chart VFs on the rendering thread;
- then allow deferred C++ owner destruction.

A blocking wait may be acceptable during controlled shutdown if required to prevent use-after-free, but it must not occur once per generation during normal rendering.

## 7. Remaining implementation gate

This identifies the engine integration point, not the final capture implementation. Before writing the engine patch, inspect the exact Base Pass call site and the existing `FBasePassMeshProcessor` construction/submission APIs at that location. The capture path must be able to construct a separate capture processor/batch list without borrowing transient graph allocator memory or altering the normal Base Pass command list.

The next implementation unit should be a small renderer hook that:
- is disabled by default behind a Software RT capture CVar;
- records one dedicated chart capture pass before the normal Base Pass;
- uses the already audited chart VF and mesh clone contracts;
- stores the capture generation in a persistent renderer-owned owner;
- fences after the outer graph executes;
- retires asynchronously through the render-thread polling queue.

Do not combine this with the actual material evaluation consumer yet. First prove the chart capture draw and resource lifetime with a simple opaque test material.

## Source paths

- `Engine/Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`
- `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraphBuilder.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraphBuilder.cpp`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.inl`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
