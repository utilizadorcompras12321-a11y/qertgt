# 17 — Surface Cache capture mapping and material revision contract

Status: design/contract patch grounded in UE4.27 source inspection. No engine code is changed by this document. Capture-pass implementation and compilation remain outstanding.

## Purpose

Define the missing contract between the stable geometric surface identity from Patch 16 and raster-produced material captures.

This patch deliberately does **not** claim that UV coordinates are a unique cache address, that a material proxy pointer is a revision, or that UE4.27 exposes a universal material-content version.

## Decisions

1. `FSoftwareRTSurfaceKey` remains the identity of a component-owned source triangle/material slot. Do not add UVs, atlas coordinates, or material-proxy addresses to that key.
2. A separate capture mapping translates a surface key and hit barycentrics into one or more Surface Cache atlas locations.
3. The first correctness implementation must not rely on source UV uniqueness. It uses generated, non-overlapping capture charts; source UV channels remain unchanged inputs to the material.
4. A capture is valid only for the exact material proxy revision and capture policy from which it was rendered.
5. Material Graph evaluation remains in a raster pass. Ray traversal and hit lookup only resolve identity, barycentrics, chart coordinates, and cached values.

## Why source UVs are not the cache address

Source UVs are material inputs, not unique surface identifiers:

- different triangles can intentionally overlap or mirror UVs;
- a mesh can have several UV channels, and there is no universal channel suitable for RT;
- some meshes have no useful UV channel;
- material UVs can be tiled or transformed and are not necessarily normalized to a unique atlas region;
- a material slot can be overridden per component while the source mesh UVs remain unchanged.

Therefore, neither source UV0 nor lightmap UVs can be the authoritative Surface Cache key.

## Capture mapping contract

Add a separate mapping table owned by the Surface Cache:

```cpp
struct FSoftwareRTSurfaceCaptureMapping
{
    FSoftwareRTSurfaceKey SurfaceKey;
    uint32 CaptureChartId = MAX_uint32;
    FVector2D ChartUV[3]; // one coordinate for each source-triangle corner
    uint32 MaterialRevision = 0;
    uint32 CapturePolicyRevision = 0;
};
```

The exact storage type may change during implementation, but these semantics must remain:

- Mapping is keyed by `FSoftwareRTSurfaceKey`, not source UV.
- `ChartUV[0..2]` correspond to the source triangle's original corner order, not the BVH-reordered triangle array.
- At a ray hit, interpolate the chart coordinate with the hit barycentrics:
  `ChartUV = B0 * ChartUV[0] + B1 * ChartUV[1] + B2 * ChartUV[2]`.
- A triangle must map to a unique chart region. Two different source triangles must never write the same chart texels, even if their source UVs overlap.
- Atlas packing must reserve gutters to prevent bilinear filtering from sampling adjacent charts.
- A mapping may refer to more than one capture record if the implementation later splits a surface across cards. The lookup contract must not assume a one-to-one mapping forever.
- Missing, stale, or invalid mappings are explicit cache misses. They must not silently return another triangle's material.

### Initial chart policy

For the first correctness baseline, assign each source triangle its own chart. This is memory-inefficient but removes UV-overlap ambiguity and gives deterministic mapping from Patch 16 identity plus barycentrics.

Do not optimize by merging triangles into larger charts until the capture rasterization path can prove that the merged chart preserves material UV inputs, seams, section/material-slot boundaries, and filtering gutters. Chart merging is an optimization, not a prerequisite for correctness.

## Material slot resolution and revision tracking

The capture system must resolve the actual per-instance material used by the render mesh batch, not infer it from the source asset alone.

UE4.27 source facts checked in mirror `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`:

- `FPrimitiveSceneProxy::GetMeshDescription(int32 LODIndex, TArray<FMeshBatch>& OutMeshElements) const` exists and is documented to gather mesh descriptions without view consideration. Its base implementation is empty, so it is not a universal primitive export API.
- `FStaticMeshSceneProxy::GetMeshDescription` emits batches by LOD section and batch, using `GetMeshElement`; each batch carries its actual `MaterialRenderProxy`. The capture provider must also preserve the source section's material-slot index alongside each batch. Do not infer a material slot from a batch's position in the flattened output for arbitrary proxy types.
- `FMeshPassProcessor::AddMeshBatch` is the Renderer mesh-pass entry point for turning `FMeshBatch` descriptions into draw commands.
- `FMaterialRenderProxy::GetMaterialWithFallback` resolves a usable material and an optional fallback proxy. The proxy corresponding to the returned material must be used for subsequent material operations.
- `FMaterialRenderProxy::UpdateUniformExpressionCacheIfNeeded(FeatureLevel)` is the normal render-thread update path.
- `FMaterialRenderProxy::GetExpressionCacheSerialNumber()` is public and returns the proxy's uniform-expression cache serial.
- `FMaterialRenderProxy::InvalidateUniformExpressionCache` increments that serial and invalidates the cached expressions.
- `FMaterialInstanceResource::RenderThread_UpdateParameter` invalidates the uniform-expression cache before changing scalar/vector/texture parameter arrays. Clearing instance parameters does the same.

### Revision fingerprint

For each currently resolved mesh-section material, track:

- the current resolved `FMaterialRenderProxy*` as a **lifetime-local identity token only**;
- `GetExpressionCacheSerialNumber()`, sampled after `UpdateUniformExpressionCacheIfNeeded(FeatureLevel)`;
- the component ID, geometry key, and material slot already represented by the surface key.

The pointer is not a durable identifier and must never be dereferenced after its owning render proxy may have been destroyed. It is compared only while reconciling currently live render-thread mesh batches. Do not serialize it or expose it to ray-hit consumers.

When either the resolved proxy identity or its serial changes, mark the corresponding material-slot captures dirty. The capture queue should coalesce repeated invalidations and recapture only affected surfaces, subject to a per-frame budget.

A changed material proxy with the same component, geometry, triangle, and slot is a new capture revision; it does not require rebuilding the BLAS or changing `FSoftwareRTSurfaceKey`.

### Important limitation of the serial

The expression-cache serial is a useful UE4.27 invalidation signal, **not a complete universal material-content version**. It proves that the uniform-expression cache was invalidated/rebuilt; it does not by itself prove that every possible external dependency of a material capture is unchanged.

The implementation must separately account for:
- material proxy replacement and destruction;
- material shader-map/recompile transitions;
- texture resource/content changes that do not produce a detectable proxy serial change;
- runtime virtual texture producer changes;
- capture-policy changes, including output format, resolution, filtering, and supported material features.

If the engine path cannot reliably observe one of these changes, that dependency must either be hooked explicitly or declared unsupported for cached RT shading. Do not claim full material revision correctness from the serial alone.

## Capture pass boundary

The capture must use Renderer-owned raster mesh descriptions and the existing material render proxy/uniform-expression path. It must not call Material Graph compilation/evaluation from the ray tracer.

However, a custom `FMeshPassProcessor` alone does not solve chart rasterization. The pass still needs a validated way to provide two distinct coordinate sets at once:

1. original mesh attributes and world-space data, so the existing material shaders evaluate the same UVs, world position, vertex color, and tangent frame as the ordinary raster path;
2. generated chart coordinates, so the resulting material outputs land in the non-overlapping Surface Cache chart.

Do not overwrite source UVs with chart UVs: that changes the material being captured. Do not assume a normal camera-projected Base Pass is a valid atlas capture: screen-space expressions and projection-dependent derivatives would change meaning.

The initial implementation scope should be static meshes, because the source proxy implements `GetMeshDescription` and exposes section-based batches. Other primitive types require their own explicit mesh-description/material-slot provider; the base virtual function returns no batches by default. The implementation must prove how the selected vertex factory and material shader path preserve original material inputs while rasterizing into generated chart coordinates. If this requires a dedicated chart vertex stream/factory or a carefully scoped Base Pass extension, document and test that exact path before adding a code diff. The source audit so far confirms the mesh-pass and mesh-description entry points, but does not establish a drop-in UE4.27 chart-capture implementation.

## Cached values and validity

A capture record must contain at least:

- the surface key and chart mapping revision;
- material revision and capture-policy revision;
- captured Base Color, Shading Normal, Roughness, Metallic, Specular, Emissive, Ambient Occlusion;
- masked-material coverage/opacity result;
- capture resolution and valid-texel coverage.

A hit is cache-valid only if all identity/revision checks pass and the sampled chart texel is marked valid. Masked materials must preserve coverage semantics; an invalid/transparent texel rejects the hit rather than returning the material's opaque fallback values.

Opaque shadow visibility should continue to terminate without fetching full material properties. Masked shadow visibility may consult cached coverage. Translucency remains a separate transmission representation and is not implied by this patch.

## Invalidation matrix

| Event | BLAS | TLAS | Surface Cache |
|---|---:|---:|---:|
| Component transform only | Keep | Update | Keep local chart; update instance transform |
| Component removed | Keep shared BLAS if still referenced | Remove instance | Evict that component's mappings/captures |
| Static mesh/LOD geometry revision | Rebuild affected BLAS | Rebind affected instance | Evict entries using old geometry key; rebuild chart mapping |
| Material slot proxy replaced | Keep | Keep | Recapture that slot |
| Material instance parameter update | Keep | Keep | Recapture affected slot after revision change |
| Material shader-map/recompile change | Keep unless geometry also changed | Keep | Invalidate/recapture after new material is renderable |
| Capture policy/resolution changed | Keep | Keep | Invalidate captures and regenerate chart allocations |
| Texture/RVT resource change not reflected by proxy serial | Keep | Keep | Requires explicit resource invalidation hook or declared unsupported dependency |

## Required validation before implementation

1. Two triangles with fully overlapping source UVs produce separate chart regions and independently correct cached values.
2. A mirrored UV island produces no chart collision.
3. Multiple material slots on one component resolve the correct per-section material proxy and revision.
4. Two component instances sharing one BLAS can use different material overrides and therefore different capture revisions.
5. Updating a scalar, vector, or texture parameter on a material instance changes the observed expression-cache serial and schedules recapture without rebuilding geometry.
6. Replacing a material instance proxy invalidates only the affected component/slot captures.
7. Removing a component evicts its mappings without evicting a shared BLAS still used by another component.
8. Invalid or uncovered chart texels report cache misses; they never alias another chart.
9. Material expressions with view-dependent/screen-dependent semantics are either explicitly rejected/approximated or covered by a documented policy. Do not claim they are universally view-independent.

## Explicit non-goals

- No Material Graph evaluation during ray traversal.
- No assumption that UV0 or lightmap UVs are unique.
- No use of material-proxy pointers as persistent IDs or as the SurfaceKey.
- No claim that `GetExpressionCacheSerialNumber()` alone detects every external texture/material dependency.
- No claim that a working chart-atlas rasterizer has already been implemented.
- No change to the UE checkout and no compile claim.

This patch defines the contract and source-backed invalidation signal. The next implementation task is to prototype the chart-capture pass with one static mesh section and prove that the existing material shader inputs remain unchanged while output is routed to generated chart coordinates.
