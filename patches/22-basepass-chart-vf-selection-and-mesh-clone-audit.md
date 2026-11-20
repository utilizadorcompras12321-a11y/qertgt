# 22 — Base Pass chart-VF selection and static-mesh data reuse audit

Status: source-path audit and integration contract. Not applied to an Unreal Engine checkout and not compiled.

Mirror audited: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## Finding: Base Pass selects the VF from the submitted mesh batch

The UE4.27 `FBasePassMeshProcessor::Process` implementation obtains:

```cpp
const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;
```

It then calls `GetBasePassShaders` with `VertexFactory->GetType()`, and builds the draw command from the same `MeshBatch`.

Therefore the experimental VF does not need to replace or modify the static mesh's normal VF globally. The capture pass can submit a capture-owned `FMeshBatch` whose `VertexFactory` points to an initialized `FSoftwareRTChartVertexFactory`. Base Pass shader lookup will then request the chart VF's shader permutation.

The relevant path is:
- `FBasePassMeshProcessor::AddMeshBatch`
- `FBasePassMeshProcessor::TryAddMeshBatch`
- `FBasePassMeshProcessor::Process`
- `GetBasePassShaders(..., VertexFactory->GetType(), ...)`
- `BuildMeshDrawCommands(..., MeshBatch, ...)`

Source locations in the mirror: `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp`, approximately lines 1240–1328 and 1493–1513.

## Reuse the existing static-mesh vertex streams

UE4.27 exposes these APIs on `FLocalVertexFactory`:
- public `FDataType`
- public `SetData(const FDataType&)`
- public `Copy(const FLocalVertexFactory&)`
- public `GetUniformBuffer()`

The existing Static Mesh render path assigns the LOD's Local VF to `FMeshBatch.VertexFactory` and sets `FMeshBatchElement.VertexFactoryUserData` to that VF's uniform buffer.

For a chart capture draw, the proposed safe reuse route is:

1. Keep the source LOD's static mesh vertex/index buffers alive.
2. Create a persistent capture-owned `FSoftwareRTChartVertexFactory` with the same feature level.
3. Copy the source Local VF data into the chart VF using the inherited `Copy(const FLocalVertexFactory&)` API.
4. Wait for the queued render-resource update to complete before submitting capture draws that use the chart VF.
5. Clone the source `FMeshBatch` and the selected `FMeshBatchElement` rather than mutating the mesh's shared batch or vertex factory.
6. Set the cloned batch's `VertexFactory` to the chart VF.
7. Set the cloned element's `VertexFactoryUserData` to the chart VF's `GetUniformBuffer()`, not the source VF's uniform buffer.
8. Preserve the source material render proxy, index buffer, index range, vertex range, reverse-culling state, and all source section flags unless the capture policy explicitly changes them.

Important: `FLocalVertexFactory::Copy` queues a render command that copies the source `Data` pointer and calls `BeginUpdateResourceRHI(this)`. It is not an immediate CPU copy. The source VF must remain alive until the queued command has consumed its data; the destination VF must remain alive through capture execution.

## Do not patch the global Static Mesh path

Do not replace `FStaticMeshSceneProxy`'s normal LOD VF, and do not modify `StaticMeshRender.cpp` to select the chart VF for ordinary scene rendering.

The capture path should construct its own mesh batches and pass them to a capture-owned `FBasePassMeshProcessor` (or a narrowly derived processor). This keeps chart rasterization isolated from normal Base Pass draws.

## Constraints not solved by this audit

- This does not prove the shader wrapper compiles.
- This does not instantiate or initialize the chart VF.
- This does not define a safe persistent owner for one chart VF per capture source/LOD.
- This does not define how the capture pass obtains the exact source section batches and primitive data.
- This does not solve the lifetime/ownership of chart per-draw parameters. Do not overwrite `BatchElement.UserData` until its existing use is audited for the chosen mesh path.
- This does not guarantee that `FBasePassMeshProcessor` accepts every desired material. The first test remains opaque, deferred, non-tessellated static-mesh material only.
- Passing a null `PrimitiveSceneProxy` is not yet approved. The capture implementation should retain the source proxy until its primitive-uniform-buffer and visibility behavior have been checked.

## Next implementation gate

Before writing a capture mesh processor, audit the exact UE4.27.2 source for:
1. How the capture pass obtains/copies a source section's `FMeshBatch` and `FMeshBatchElement`.
2. How `FMeshBatchElement.UserData` is used by the selected static-mesh VF/material path.
3. How `FBasePassMeshProcessor` builds primitive/view uniform bindings when called from a capture-owned mesh pass.
4. How to keep the source Local VF, destination chart VF, source buffers, and per-draw payload alive until draw execution.
5. How to force a non-cached dynamic mesh draw path for the capture pass.

Only after these are resolved should a source patch add chart capture scheduling or draw submission.
