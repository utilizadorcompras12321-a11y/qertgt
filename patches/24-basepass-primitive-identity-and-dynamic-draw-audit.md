# 24 — Base Pass primitive identity and dynamic draw-path audit

Status: source-path audit and implementation gate. Not applied to an Unreal Engine checkout and not compiled.

Mirror audited: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## 1. Base Pass does not treat the primitive proxy as optional metadata

In `FBasePassMeshProcessor::Process` (`Renderer/Private/BasePassRendering.cpp`), the supplied `PrimitiveSceneProxy` is forwarded to:
- `SetDepthStencilStateForBasePass`;
- `TBasePassShaderElementData::InitializeMeshMaterialData(..., PrimitiveSceneProxy, ...)`;
- `BuildMeshDrawCommands(..., PrimitiveSceneProxy, ...)`.

It is also used to calculate translucent sort keys and to select lighting policy paths in `AddMeshBatch`.

Some simple opaque paths may not dereference it in every branch, but that does not make a null proxy a valid general Base Pass contract. The capture implementation must retain and pass the source proxy for the initial opaque/deferred test.

## 2. GPU Scene makes a null proxy unsafe with the default primitive ID mode

In `Renderer/Public/MeshPassProcessor.h`, the mesh processor receives the proxy as part of `AddMeshBatch` and `BuildMeshDrawCommands`.

In `Renderer/Private/MeshPassProcessor.cpp`, `FMeshPassProcessor::GetDrawCommandPrimitiveId` checks:
- if GPU Scene is enabled and `PrimitiveIdMode == PrimID_FromPrimitiveSceneInfo`, it asserts that `PrimitiveUniformBufferResource` is null and executes `check(PrimitiveSceneInfo)`, then reads `PrimitiveSceneInfo->GetIndex()`;
- `PrimID_DynamicPrimitiveShaderData` instead derives an ID from the scene primitive count plus `DynamicPrimitiveShaderDataIndex`;
- `PrimID_ForceZero` uses primitive ID zero and requires the view's primitive-scene-data override SRV to define the data at that index.

Therefore a cloned static-mesh element retaining the normal `PrimID_FromPrimitiveSceneInfo` mode cannot be submitted with a null proxy on the GPU Scene path. Forcing another primitive ID mode is not a shortcut: it requires explicit, correct primitive shader data setup and must be audited as a separate design.

## 3. Keep source primitive identity; change only chart rasterization

The safest first capture draw is:
- preserve the source `FPrimitiveSceneProxy`;
- preserve its scene primitive identity and the source element's primitive ID mode;
- preserve the original primitive transform/uniform data;
- replace only the chart VF and its VF-owned bindings;
- let the chart VF's vertex shader hook map the already transformed vertex position into chart clip space.

This avoids creating a second primitive-data system merely to rasterize into chart tiles. It also means the chart mapping must be correctly defined in the coordinate space received by the existing VF hook; do not feed chart UVs as if they were world positions.

## 4. Dynamic mesh draw commands are the relevant non-cached route

The audited API surface exposes:
- `FMeshPassProcessor` constructed with an optional `FSceneView* InViewIfDynamicMeshCommand`;
- `FMeshPassDrawListContext` as the destination for generated draw commands;
- `DrawDynamicMeshPassPrivate(...)` as an exported helper that dispatches a supplied dynamic mesh command array.

The pass registration flags distinguish `EMeshPassFlags::CachedMeshCommands` from non-cached passes. The chart capture must use a dynamic/non-cached context and must not insert its temporary chart batches into the scene's cached static mesh draw lists.

The helper signature alone does not establish ownership of the view, draw-list context, dynamic command storage, or chart payload. Those owners must remain alive until the helper has completed command setup and dispatch. A stack-local batch is acceptable only if the processor consumes it synchronously into fully copied command state; do not assume that without verifying the exact command setup implementation in the chosen call path.

## 5. Consequences for the implementation

- Keep the source `FPrimitiveSceneProxy`; null proxy is rejected for the baseline.
- Keep `PrimitiveIdMode` and primitive scene data unchanged for the baseline.
- Use a capture-owned dynamic mesh pass/context, not the main view's cached static draw lists.
- Keep chart-specific state out of `FMeshBatchElement::UserData`; use typed chart-VF bindings.
- Do not yet add a capture processor until command storage, context, and payload lifetimes are traced through the exact dynamic draw helper call site.

## Source paths checked

- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`
- `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`
- `Engine/Source/Runtime/Engine/Public/MeshBatch.h`
- `Engine/Source/Runtime/Engine/Public/StaticMeshResources.h`
