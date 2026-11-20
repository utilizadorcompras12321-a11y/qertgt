# 31 — Capture RHI resource retention across command submission

Status: UE4.27 RHI lifetime audit. This patch documents verified pointer/reference behavior and tightens the capture-owner contract. No engine source was changed; no compile or GPU test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related: [Patch 29 — Capture resource owner](29-capture-resource-owner-and-deferred-retirement-design.md) and [Patch 30 — Local Vertex Factory resource lifecycle](30-local-vertex-factory-resource-lifecycle-audit.md).

## 1. Critical finding: queued RHI binding commands store raw pointers

In `RHI/Public/RHICommandList.h`:

- `FRHICommandSetShaderTexture` stores `FRHITexture*`.
- `FRHICommandSetShaderResourceViewParameter` stores `FRHIShaderResourceView*`.
- `FRHICommandSetUAVParameter` stores `FRHIUnorderedAccessView*`.
- `FRHICommandSetShaderUniformBuffer` stores `FRHIUniformBuffer*`.
- `FRHICommandSetShaderSampler` stores `FRHISamplerState*`.

Their constructors copy those raw pointers. The corresponding `FRHICommandList::SetShaderTexture`, `SetShaderResourceViewParameter`, `SetUAVParameter`, `SetShaderUniformBuffer` and sampler methods enqueue these command objects; the command objects do not acquire a `TRefCountPtr` or call `AddRef()` for the bound resource.

The command execute implementations in `RHICommandListCommandExecutes.inl` pass the stored pointers to the RHI context. They do not add an owning reference either.

**Consequence:** a capture owner must retain the owning RHI references until every queued RHI command that can dereference those pointers has executed. Keeping the CPU draw-command object alive is not enough. Enqueueing a binding is not an ownership transfer.

This applies to texture, SRV, UAV, sampler and uniform-buffer bindings. Do not rely on the RHI command list to rescue a resource whose final owner has already released it.

## 2. Render-pass descriptors also borrow raw texture pointers

In `RHI/Public/RHIResources.h`:

- `FRHIRenderPassInfo::FColorEntry` stores raw `FRHITexture*` for render target and resolve target.
- `FDepthStencilEntry` stores raw `FRHITexture*` for depth/stencil and resolve target.
- `FRHIRenderPassInfo::ShadingRateTexture` is an owning `FTextureRHIRef`, but the ordinary color/depth/resolve entries are raw pointers.
- `FRHIRenderTargetView` and `FRHIDepthRenderTargetView` also store raw `FRHITexture*`.

`FRHICommandBeginRenderPass` copies `FRHIRenderPassInfo` by value. That copy does not turn the color/depth/resolve raw pointers into owning references.

**Consequence:** retain all color, depth, resolve and other pass attachments through the execution of the begin/end render-pass commands and the pass work that references them. Do not create a local `FTextureRHIRef`, build a pass descriptor from its raw pointer, then release the ref while the command list is still pending.

## 3. UE4.27 provides owning RHI reference types

`RHI/Public/RHIResources.h` defines these aliases as `TRefCountPtr`:

- `FTextureRHIRef`, `FTexture2DRHIRef`, `FTexture3DRHIRef`, `FTextureCubeRHIRef`;
- `FShaderResourceViewRHIRef`;
- `FUnorderedAccessViewRHIRef`;
- `FUniformBufferRHIRef`;
- `FSamplerStateRHIRef`.

`FRHIResource` implements intrusive `AddRef()` / `Release()` reference counting. The RHI resource destructor checks that no references remain, except for the platform-specific deletion-latency path.

These refs are the correct C++ ownership mechanism for custom capture RHI objects. A raw pointer extracted with `.GetReference()` is borrowed and must not outlive the owning ref.

## 4. Render-target resource example: targetable and shader-resource textures are distinct

The UE4.27 `FTextureRenderTarget2DResource` implementation is instructive:

- `InitDynamicRHI()` creates both `RenderTargetTextureRHI` and `Texture2DRHI` through `RHICreateTargetableShaderResource2D()`.
- The resource can also create an `UnorderedAccessViewRHI`.
- `TextureRHI` is assigned from `Texture2DRHI`.
- `ReleaseDynamicRHI()` releases the base texture resources, clears the texture reference, then releases `Texture2DRHI`, `RenderTargetTextureRHI` and mip-generation resources.

The render-target texture used for raster writes and the shader-resource texture used for sampling may therefore be distinct RHI objects. A capture implementation must retain whichever object each pass actually uses; retaining only the sampled texture is not enough if a queued render pass still targets the targetable surface, and vice versa.

For a capture-owned target, record the exact resource pair returned by the chosen creation API. Do not assume one texture pointer represents both target and resolve/sample storage.

## 5. What RHI deferred deletion does and does not guarantee

`FRHIResource::Release()` defers deletion when the RHI is threaded or the platform requires extra deletion latency. The source comments explain that some APIs require waiting a few frames before deleting resources so the GPU has finished, avoiding expensive fences.

That is an engine-managed resource deletion policy after the final reference is released. It does **not** make a raw pointer stored in a not-yet-executed RHI command safe after the final reference is released.

Keep these two cases separate:

1. **Pending command consumption:** the capture owner must keep a strong `TRefCountPtr` reference while a queued command may still dereference its raw pointer.
2. **GPU use after command execution:** once the command has executed and the owning reference is released, the RHI's own deferred-deletion mechanism applies according to the backend's threading/deletion-latency policy.

Do not describe either mechanism as a universal explicit GPU-completion fence. This audit does not establish that a particular backend's command-list submission point is equivalent to GPU completion.

## 6. Revised capture-owner inventory

The capture generation should own or strongly retain:

| Resource | Owning UE4.27 type / owner | Why |
|---|---|---|
| Chart VF object | Capture owner; release via render-resource lifecycle from Patch 30 | Mesh draw bindings borrow its UB pointer and streams |
| LocalVF uniform buffer | `FLocalVertexFactory`'s `TUniformBufferRef` | `GetUniformBuffer()` returns a borrowed raw pointer |
| Chart mapping UB | `FUniformBufferRHIRef` or the exact owning `TUniformBufferRef` used by its creation API | RHI binding command stores `FRHIUniformBuffer*` |
| Targetable color/depth textures | `FTextureRHIRef` / matching typed RHI ref | Render-pass descriptors borrow raw texture pointers |
| Resolve/sample textures | `FTextureRHIRef` / matching typed RHI ref | Resolve descriptors and texture bindings borrow raw pointers |
| SRVs | `FShaderResourceViewRHIRef` | Binding command stores raw SRV pointer |
| UAVs | `FUnorderedAccessViewRHIRef` | Binding command stores raw UAV pointer |
| Samplers | `FSamplerStateRHIRef` | Binding command stores raw sampler pointer |
| Static Mesh LOD buffers | Existing Static Mesh render-data owner | Do not duplicate ownership; keep source mesh/render data alive through command construction and submission |

The owner must retain resources across the full interval in which queued Render Thread work can enqueue RHI commands and until those RHI commands have been consumed. The exact handoff/retirement signal for the selected capture submission path remains to be audited before implementation.

## 7. Required submission/retirement ordering

The capture system must establish an explicit generation state machine:

1. **Active:** new capture work may be built and submitted.
2. **Closing:** reject new submissions; existing callbacks and render commands may still reference the generation.
3. **Render-thread drained:** no queued callback can still build or enqueue work using the generation's resources.
4. **RHI-command consumption guaranteed:** all queued RHI binding and render-pass commands that contain borrowed pointers into the generation have been consumed by the RHI execution path.
5. **Release:** queue `BeginReleaseResource(&ChartVF)` after the last VF use, and release the generation's owning RHI refs only when the above conditions hold.
6. **Deferred C++ destruction:** use `BeginCleanup()` for the owner object only after its render resource has been released and the resource retirement conditions are met.

Do not equate the end of `DrawDynamicMeshPass()`'s CPU callback with RHI command consumption. The callback can build and submit renderer commands; the RHI command list has its own command storage and execution stage.

Do not use `BeginCleanup()` alone as proof of stage 4. Patch 29's frame-end deferred cleanup is a C++ destruction mechanism, not a documented RHI-command completion signal.

## 8. Implementation gate

This audit proves the ownership requirement and the UE4.27 reference types. It does **not** yet select the exact retirement signal for the project's chosen capture pass. Before implementing:

1. trace the exact `DrawDynamicMeshPass` → mesh draw command → `FRHICommandList` execution path used by the capture pass;
2. identify when its RHI commands are consumed in both RHI-threaded and non-threaded configurations;
3. confirm whether the owner can be retained through that point without a per-capture Game Thread flush;
4. inspect the capture render-target creation path and retain both targetable and resolve/sample textures when distinct;
5. then define the owner handoff and release ordering.

Until that signal is proven, keep the owner alive conservatively. Do not optimize retirement by guessing that a frame boundary or callback return is sufficient.

## Source paths

- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandListCommandExecutes.inl`
- `Engine/Source/Runtime/Engine/Public/TextureResource.h`
- `Engine/Source/Runtime/Engine/Private/TextureRenderTarget2D.cpp`
- `Engine/Source/Runtime/Engine/Public/UnrealClient.h`
- `Engine/Source/Runtime/Renderer/Private/RenderTargetTemp.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
