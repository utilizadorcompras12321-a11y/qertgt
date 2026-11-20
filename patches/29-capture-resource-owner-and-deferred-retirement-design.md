# 29 — Capture resource owner and deferred retirement design

Status: ownership design based on UE4.27 source conventions. No engine source was changed; no compile or GPU test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## 1. Decision

Use a capture-owned resource bundle and UE4.27's existing deferred cleanup mechanism as the first retirement candidate:

- the capture owner retains the chart VF and all capture resources;
- draw commands may contain non-owning pointers into those resources;
- when the capture generation is no longer referenced by any future command submission, transfer the owner to `BeginCleanup()`;
- the owner is destroyed by the engine's pending-cleanup flow, not immediately at the end of the capture callback.

This follows an existing engine pattern. `FRetainerWidgetRenderingResources` in `UMG/Private/Slate/SRetainerWidget.cpp` derives from `FDeferredCleanupInterface`; `SRetainerWidget::~SRetainerWidget` calls `BeginCleanup(RenderingResources)` and explicitly says not to delete the object there.

## 2. What UE4.27's deferred cleanup guarantees

`FDeferredCleanupInterface` is a polymorphic cleanup base. `BeginCleanup` enqueues the object in the pending-cleanup list.

The normal engine tick in `LaunchEngineLoop.cpp`:
1. obtains the current pending-cleanup set;
2. synchronizes frame-end rendering through `FFrameEndSync`;
3. deletes the previous frame's pending-cleanup set.

The cleanup object's destructor therefore runs after that engine frame synchronization point. This is an established render-thread-safe deferred object-destruction pattern.

Important limit: this is not, by itself, a documented explicit GPU-completion fence for arbitrary submitted work. Do not describe it as a GPU fence or use it to justify releasing resources outside the RHI's normal reference/deferred-destruction rules.

## 3. Proposed owner shape

Conceptual only; exact UE4.27 member types and release calls must be verified when implementing:

```cpp
class FSoftwareRTCaptureResourceOwner final : public FDeferredCleanupInterface
{
public:
    // Owns/retains the chart VF and its initialized render resources.
    // Retains chart mapping uniform buffers and capture textures/SRVs.
    // Retains any additional RHI resources referenced by draw commands.

    virtual ~FSoftwareRTCaptureResourceOwner()
    {
        // Release chart VF resources using the verified UE4.27
        // render-resource release path before destroying the VF object.
        // RHI references are released only through their owning UE4.27 types.
    }
};
```

Do not copy this sketch into the engine as-is. The exact ownership types and VF release order remain implementation work.

## 4. Resource inventory

| Resource | Required owner | Retirement condition |
|---|---|---|
| Chart VF object | Capture owner | No command can refer to it; release its render resource on the correct thread before object destruction |
| Chart VF uniform buffer | Chart VF / capture owner through the VF's verified ownership | Must remain valid while draw bindings can be submitted |
| Chart mapping uniform buffer | Capture owner via owning RHI reference | No pending command or GPU/RHI use can reference it |
| Vertex/index buffers | Existing Static Mesh LOD owner; capture owner only if it creates replacement buffers | Preserve original LOD lifetime, or retain replacement buffers through their owning RHI refs |
| Capture render targets/textures | Capture owner / texture resource owner | No pending draw or RHI operation can reference them |
| SRVs and samplers | Capture owner via owning RHI refs, or underlying resource owner where UE4.27 guarantees it | Same lifetime as every command that binds them |
| Draw command storage | Dynamic pass storage / renderer command owner | Until command execution/processing is complete |

Do not duplicate ownership of Static Mesh LOD buffers unnecessarily. Their existing mesh render data is already the owner; the capture system must keep the mesh/render data alive for the command lifetime.

## 5. Lifecycle

### Create
- Create the owner on the appropriate thread.
- Initialize chart VF and its resources through the established render-resource lifecycle.
- Create mapping buffers and capture targets.
- Keep the owner strongly reachable from the capture generation.

### Build and submit
- Clone the source mesh batch while source proxy, mesh render data, and source buffers are valid.
- Build commands using the chart VF.
- Bind only resources owned by the capture generation or guaranteed alive by existing engine owners.
- Do not put raw pointers to stack storage or transient allocator memory in shader bindings.

### Retire
- Stop producing new commands for the generation.
- Ensure no queued CPU-side pass can still build or submit commands referring to it.
- Transfer the owner to `BeginCleanup` instead of deleting it directly.
- In the owner's destruction path, release chart VF render resources using the verified UE4.27 resource-release mechanism, then release RHI references through their owning types.

## 6. FRenderCommandFence is not the default solution

UE4.27's `FRenderCommandFence::BeginFence(bool bSyncToRHIAndGPU)` can optionally request RHI/GPU synchronization, but its implementation has policy conditions: when RHI/GPU sync is requested, it may disable that path if `r.VSync` is off or GT sync is disabled. It is therefore not a universal unconditional GPU-completion primitive.

Do not block the Game Thread with a fence for every capture generation. That would be expensive and is unnecessary if the normal renderer ownership/deferred cleanup model is followed.

If a future resource type requires explicit GPU completion beyond the engine's normal RHI reference semantics, select and verify a resource-specific fence/retirement mechanism at that point.

## 7. Remaining verification gates

Before implementing the owner:
1. inspect `FLocalVertexFactory` / `FVertexFactory` initialization and release semantics in UE4.27;
2. confirm the chart VF's uniform buffer ownership and whether it is recreated by `BeginUpdateResourceRHI`;
3. identify exact UE4.27 RHI ref types for the chosen chart uniform buffer, texture and SRV creation APIs;
4. verify capture render-target lifetime through the pass and any queued resolve/copy;
5. confirm that the capture generation cannot be retired while a dynamic pass callback or queued render command still references it.

Only after those checks should the sketch become implementation code.

## Source paths

- `Engine/Source/Runtime/RenderCore/Public/RenderingThread.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Runtime/UMG/Private/Slate/SRetainerWidget.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`
