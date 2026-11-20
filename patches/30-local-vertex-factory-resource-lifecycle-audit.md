# 30 — Local Vertex Factory resource lifecycle and retirement correction

Status: source audit completed against the UE4.27 mirror. This document corrects and narrows the owner proposal in Patch 29. No engine source was changed; no compile or GPU test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## 1. Findings

### 1.1 FVertexFactory is an FRenderResource

In `RenderCore/Public/VertexFactory.h`:

- `FVertexFactory` derives from `FRenderResource`.
- It overrides `ReleaseRHI()`.
- Its declaration objects and stream arrays are render-resource state, not ordinary batch-owned data.

In `RenderCore/Private/VertexFactory.cpp`, `FVertexFactory::ReleaseRHI()` releases the default, position-only and position-and-normal vertex declarations, then empties the stream arrays.

### 1.2 FLocalVertexFactory owns its LocalVF uniform buffer

In `Engine/Public/LocalVertexFactory.h`:

- `FLocalVertexFactory` derives from `FVertexFactory`.
- Its `UniformBuffer` member is a `TUniformBufferRef<FLocalVertexFactoryUniformShaderParameters>`.
- `GetUniformBuffer()` returns `UniformBuffer.GetReference()`; this is a borrowed raw pointer, not an ownership transfer.
- `ReleaseRHI()` calls `UniformBuffer.SafeRelease()` and then `FVertexFactory::ReleaseRHI()`.

In `Engine/Private/LocalVertexFactory.cpp`:

- `InitRHI()` creates the LocalVF uniform buffer and stores the returned `TUniformBufferRef` in the VF member.
- `CreateLocalVFUniformBuffer()` uses `CreateUniformBufferImmediate(..., UniformBuffer_MultiFrame)`.
- `SetData()` is render-thread-only and calls `UpdateRHI()`.
- `Copy()` enqueues a command capturing raw pointers to the destination VF and `Other.Data`, then calls `BeginUpdateResourceRHI(this)`.

Consequences:
- The VF owns its LocalVF uniform-buffer reference.
- A draw binding obtained through `GetUniformBuffer()` does not keep the VF or its uniform buffer alive by itself.
- During `Copy()`, both source and destination VFs must remain alive until the queued data-copy command has executed; the destination must also remain alive through the queued RHI update.
- Do not destroy or mutate the source VF while the queued copy still reads `Other.Data`.

### 1.3 FRenderResource has a strict release-before-delete rule

In `RenderCore/Public/RenderResource.h` and `RenderCore/Private/RenderResource.cpp`:

- `InitResource()`, `ReleaseResource()` and `UpdateRHI()` require the rendering thread.
- `BeginInitResource()`, `BeginUpdateResourceRHI()` and `BeginReleaseResource()` enqueue render-thread commands from the Game Thread.
- `ReleaseResource()` calls `ReleaseRHI()` and `ReleaseDynamicRHI()` when initialized, then removes the resource from the resource list and sets it uninitialized.
- `FRenderResource::~FRenderResource()` logs a fatal error if an initialized resource is deleted without first being released.

This is not optional cleanup. Deleting an initialized chart VF is an engine-fatal lifetime error.

## 2. Correction to Patch 29

Patch 29's conceptual owner destructor must **not** directly call `ReleaseResource()` unless that destructor is guaranteed to execute on the rendering thread. The normal deferred-cleanup destruction point is not a substitute for the documented rendering-thread-only release API.

The safe division is:

1. On the Game Thread, stop scheduling new work for the generation.
2. Ensure no CPU-side callback or queued render command can still access the generation or its VFs.
3. Queue `BeginReleaseResource(&ChartVF)` (or perform `ReleaseResource()` from a correctly ordered rendering-thread command).
4. Keep the owner and VF object alive until the release command has executed.
5. Only then allow C++ destruction; the VF must report `IsInitialized() == false`.

The release command must be ordered after the final render-thread use of the VF. Do not enqueue release while any later callback can still build or submit a draw using that VF.

## 3. What deferred cleanup does and does not solve

`FDeferredCleanupInterface` / `BeginCleanup()` remains useful to defer C++ owner destruction, but it does not itself call `FRenderResource::ReleaseResource()`.

The frame-end cleanup sequence is not a documented universal GPU-completion fence. Therefore:

- Use the render-thread release command to perform the VF's required RHI release.
- Use deferred cleanup to keep the C++ owner alive across the engine's deferred destruction window.
- Do not claim that `BeginCleanup()` alone proves arbitrary GPU work has completed.
- Do not assume that a raw `FRHIUniformBuffer*` stored in a draw command owns a reference to the underlying UB.

For every custom RHI resource bound by the capture pass, keep its owning RHI reference alive until the relevant render/RHI usage has passed the engine's supported lifetime boundary. If the capture implementation needs a stronger GPU-completion guarantee than UE4.27's normal RHI resource-reference/deferred-release behavior, that guarantee must be audited separately; this patch does not invent one.

## 4. Recommended owner responsibilities

The owner should retain:

- the chart VF C++ object;
- chart mapping uniform-buffer references;
- capture textures, SRVs, UAVs and samplers through their actual UE4.27 RHI reference types;
- any replacement vertex/index resources created by the capture system;
- the capture-generation state needed to reject submissions after retirement begins.

It should **not** duplicate ownership of Static Mesh LOD buffers when those remain owned by the mesh render data. The source mesh/render data and source proxy must instead remain valid until all capture commands that refer to them have been built and submitted.

The chart VF's LocalVF uniform buffer should remain owned by its `TUniformBufferRef`. Do not add a second raw-pointer owner or manually release that member.

## 5. Revised lifecycle

### Create and initialize

- Allocate the owner and chart VF with a lifetime that spans all queued initialization and update commands.
- Populate VF data on the rendering thread, or use the verified queued resource APIs.
- If using `FLocalVertexFactory::Copy()`, retain both source and destination VFs until the copy and subsequent update commands have executed.
- Initialize the VF through `BeginInitResource()` / `InitResource()` in the correct thread context.

### Submit

- Draw commands may borrow the chart VF's UB pointer and RHI resources.
- Keep the owner alive for every pass/callback that can still submit work referencing the generation.
- Do not store stack pointers or short-lived allocator memory in the VF data or shader bindings.

### Retire

- Mark the generation as retiring and prevent all new capture submissions.
- Drain or invalidate every CPU-side callback and queued command that can still dereference its owner, VF, or source data.
- Queue `BeginReleaseResource(&ChartVF)` after the last render-thread use.
- Retain the owner until the release command has executed and any separately required RHI/GPU lifetime condition is satisfied.
- Destroy the owner only after the VF is uninitialized. Its base destructor will otherwise fail fatally.

## 6. Implementation gate still open

This audit establishes the VF release mechanics, but it does **not** establish a complete safe retirement protocol for the eventual capture pass. Before implementing the owner, still verify:

1. the exact thread and ordering of the capture generation's final draw submission;
2. how the capture pass retains render targets and their RHI references through resolve/copy operations;
3. whether the RHI backend retains resources referenced by submitted command lists as expected for this UE4.27 path;
4. how the owner can know that no asynchronous CPU callback or queued render command still refers to it;
5. whether the normal RHI resource reference semantics are sufficient, or a specific completion primitive is required.

Do not use a per-generation Game Thread flush as a shortcut. It would serialize the renderer and still needs a precise statement of what it guarantees.

## Source paths

- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`
- `Engine/Source/Runtime/RenderCore/Private/VertexFactory.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderResource.cpp`
- `Engine/Source/Runtime/Engine/Public/LocalVertexFactory.h`
- `Engine/Source/Runtime/Engine/Private/LocalVertexFactory.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderingThread.h`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`
