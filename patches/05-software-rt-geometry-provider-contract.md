# 05 — Software RT geometry provider contract (UE 4.27.2)

Status: design contract grounded against UE 4.27 source; not an engine implementation or compile-tested patch.

## Findings

- `FPrimitiveSceneProxy` already exposes virtual `GetMeshDescription(int32 LODIndex, TArray<FMeshBatch>&)`. This is a view-independent mesh-batch description, not a canonical triangle export API.
- `FStaticMeshSceneProxy::GetMeshDescription` walks LOD sections and batches and calls `GetMeshElement`.
- `FStaticMeshSceneProxy::RenderData` is protected. Renderer code holding only `FPrimitiveSceneProxy*` cannot legally read it.
- `FStaticMeshBatch` derives from `FMeshBatch`; it records the owning `FPrimitiveSceneInfo*` and static-mesh batch ID. It does not make every batch a conventional static mesh.
- `FSplineMeshSceneProxy` derives from `FStaticMeshSceneProxy`, but uses `FSplineMeshVertexFactory` and spline deformation parameters. Exporting the source LOD positions as-is would not match rendered geometry.
- `FInstancedStaticMeshSceneProxy` derives from `FStaticMeshSceneProxy`, but sets up batches with a dedicated instanced vertex factory and per-instance buffer. A single primitive transform is insufficient.
- Landscape and other specialized proxies require their own explicit treatment.

## Required design

Add a small virtual provider interface to `FPrimitiveSceneProxy`, with a default implementation that reports unsupported. The interface must return a render-thread-owned, frame-valid geometry description; it must not expose UObjects or require Renderer code to cast a generic proxy to a concrete Engine proxy type.

The description must identify:
- canonical triangle/index data or a supported GPU-resident source;
- vertex positions in the documented local space;
- per-vertex attributes needed by the shared Surface Cache (UV channels, normals/tangents, vertex color where available);
- submesh/section ranges and material-slot identity;
- instance transforms (or an explicit one-instance declaration);
- geometry revision / update policy and whether the source is static, refittable, or rebuild-required;
- unsupported features such as WPO or unsupported vertex-factory deformation.

The consumer applies the primitive transform exactly once. Geometry ownership and lifetime must remain valid through upload/build; no pointer into temporary mesh-collector memory may escape its frame.

## Provider rollout

1. Conventional `FStaticMeshSceneProxy`: first provider. Use current LOD/render-data resources and preserve section/index ranges. Respect streamed LOD availability; do not assume CPU vertex data exists for every LOD.
2. `FInstancedStaticMeshSceneProxy`: explicit instance transforms and per-instance material/visibility semantics. Do not flatten instances into one primitive transform.
3. `FSplineMeshSceneProxy`: export spline-deformed positions (and compatible shading basis), or mark unsupported until deformation is reproduced. Never silently use undeformed source positions.
4. Landscape, foliage variants, procedural meshes, Nanite and other proxies: explicit provider implementations or explicit unsupported status. No unsafe casts.

## Batch handling

When consuming `FMeshBatch` / `FMeshBatchElement`, preserve `FirstIndex`, `NumPrimitives`, `BaseVertexIndex`, index-buffer identity, material proxy and instance count. Do not rebuild a whole LOD by assuming each batch covers every section or by ignoring batch-specific index ranges.

`GetMeshDescription` can be used to discover the proxy's view-independent batches, but it is not itself proof that the underlying vertex factory is CPU-readable or that its deformation/instancing is represented by a simple static vertex buffer.

## Acceptance criteria

- No unchecked cast from `FPrimitiveSceneProxy*` to `FStaticMeshSceneProxy*`.
- No false-positive support: unsupported geometry is counted and visible in diagnostics.
- Static mesh triangles match the selected render LOD and section ranges.
- Instanced meshes preserve all instance transforms.
- Spline meshes never trace undeformed source geometry as if it were the rendered mesh.
- WPO and other material-driven geometry changes are either represented in the exported geometry or explicitly flagged unsupported.
- All three consumers (software shadows, GI and reflections) consume the same canonical geometry/provider contract.

## Source references checked

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

- `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Public/Components.h`
- `Engine/Source/Runtime/Engine/Private/SplineMeshSceneProxy.h/.cpp`
- `Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h`
- `Engine/Source/Runtime/Engine/Private/InstancedStaticMesh.cpp`
- `Engine/Source/Runtime/Renderer/Private/SceneCore.h`

This document is a contract for the next code change. It does not claim that a provider, triangle extraction, BVH, or ray traversal has been implemented.
