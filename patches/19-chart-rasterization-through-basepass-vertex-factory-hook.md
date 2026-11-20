# 19 — Chart rasterization through the UE4.27 Base Pass vertex-factory hook

Status: source-audited design correction. No engine source is modified or compiled.

## Critical finding

UE4.27 already has the exact abstraction point needed to separate material position from raster position:

`VertexFactoryGetRasterizedWorldPosition(Input, VFIntermediates, WorldPosition)`

The default implementation in `Engine/Shaders/Private/LocalVertexFactory.ush` returns `InWorldPosition` unchanged.

The UE4.27 Base Pass vertex shader `Engine/Shaders/Private/BasePassVertexShader.usf` does this in order:

1. `VertexFactoryGetWorldPosition(...)` obtains the material world position.
2. `GetMaterialVertexParameters(...)` builds material vertex parameters from that world position and the original tangent basis.
3. `GetMaterialWorldPositionOffset(...)` applies WPO to `WorldPosition`.
4. `VertexFactoryGetRasterizedWorldPosition(..., WorldPosition)` obtains the position used only for rasterization.
5. That rasterized position is multiplied by `ResolvedView.TranslatedWorldToClip` to produce `SV_Position`.
6. `VertexFactoryGetInterpolants(..., VertexParameters)` is built from the original material vertex parameters.

This confirms that a vertex-factory override can change raster placement without replacing the material's original UVs, world position, tangent frame, or other material interpolants.

This is materially better than the previous assumption that a separate chart vertex stream was necessarily required.

## Revised prototype: one source triangle per draw

For the first prototype, do not add a chart vertex stream and do not duplicate or rewrite the source vertex buffer.

Use one draw per source triangle and supply a per-draw affine mapping that transforms its three world-space vertices into a small chart triangle in the capture view's raster plane.

The mapping is evaluated only by the capture vertex-factory implementation of `VertexFactoryGetRasterizedWorldPosition`. The original `WorldPosition` argument is not modified and continues through the existing material path.

For a source triangle with world vertices (P_0,P_1,P_2), construct an affine transform (A) such that:

- (A(P_0)) maps to chart corner (C_0);
- (A(P_1)) maps to chart corner (C_1);
- (A(P_2)) maps to chart corner (C_2).

The output must be expressed in the coordinate space expected by the capture view's `TranslatedWorldToClip`. Do not assume that simply returning normalized atlas UVs as a world position is correct. The capture view projection and chart transform must be derived together and tested.

Because the draw contains exactly one source triangle, the affine mapping is unambiguous for its three vertices. This is intentionally a slow correctness prototype; it is not the final throughput design.

### WPO

The Base Pass computes WPO before calling the raster-position hook. Therefore the hook receives the WPO-adjusted `WorldPosition`, while the material parameters and interpolants are still built through the original path.

For this first test, keep WPO disabled anyway: the current software BVH does not yet share the raster capture's displaced geometry, so WPO would make the capture disagree with ray-hit geometry. The hook's ordering does not remove that cross-system mismatch.

## Vertex-factory registration strategy

Create a distinct `FSoftwareRTChartVertexFactory` type derived from `FLocalVertexFactory`, with its own registered vertex-factory type name and its own shader compilation define (for example `SOFTWARE_RT_CHART_CAPTURE`).

Reuse the existing `/Engine/Private/LocalVertexFactory.ush` shader source for the initial experiment, but gate the alternate raster-position function behind the new vertex-factory define. Normal `FLocalVertexFactory` permutations must keep returning `InWorldPosition`.

Do not globally change the default local vertex factory behavior.

The chart vertex factory needs a dedicated shader-parameter binding implementation for the per-draw chart transform. Do not overload `FMeshBatchElement::VertexFactoryUserData` casually: the stock `FLocalVertexFactoryShaderParameters` interprets that field as an `FRHIUniformBuffer*` override. A custom parameter class must own and validate the chart-transform binding explicitly.

The per-draw transform must be supplied through a lifetime-safe uniform-buffer resource or a renderer-owned draw-data path compatible with the mesh draw command lifetime. A raw pointer in `FMeshBatchElement::UserData` is not acceptable unless its lifetime and mesh-command caching behavior are proven.

## Source facts verified

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

- `Engine/Shaders/Private/LocalVertexFactory.ush`: default `VertexFactoryGetRasterizedWorldPosition` returns the incoming world position.
- `Engine/Shaders/Private/BasePassVertexShader.usf`: material vertex parameters and WPO are evaluated before the raster-position hook; the hook's output is used for clip-space position; material interpolants are generated from the original `VertexParameters`.
- `Engine/Source/Runtime/Engine/Public/LocalVertexFactory.h`: `FLocalVertexFactory` is exported and its `FDataType` extends `FStaticMeshDataType`; shader parameter base class is declared for derived local vertex factories.
- `Engine/Source/Runtime/Engine/Private/LocalVertexFactory.cpp`: local VF registers its parameter type and VF type with `IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE` and `IMPLEMENT_VERTEX_FACTORY_TYPE_EX`; it has explicit compilation-environment and validation methods.
- `Engine/Source/Runtime/Engine/Public/MeshBatch.h`: `FMeshBatchElement` contains `UserData` and `VertexFactoryUserData`; the latter is explicitly interpreted by the stock local VF as a uniform-buffer override.

## Unresolved implementation questions (must be answered before code)

1. Can the new VF subclass reuse the local VF shader parameter base without breaking the stock local VF uniform-buffer binding?
2. Which UE4.27 shader-parameter binding mechanism is safest for a per-draw chart transform when mesh draw commands may be cached?
3. How should the capture view's translated-world origin and clip transform be configured so chart coordinates map exactly to atlas texel coordinates?
4. Can a one-triangle draw be represented without mutating shared static-mesh section index buffers? If a temporary three-index buffer is required, its RHI lifetime and draw-command cache policy must be explicit.
5. How will the capture pass disable or reject unsupported view-dependent material expressions, masked/translucent blend modes, tessellation, and pixel depth offset?

## Prototype acceptance test

Use a single static mesh triangle and a simple opaque material that samples a checker texture from source UV0 and uses a tangent-space normal map.

Render the same triangle once through the normal Base Pass and once through the chart capture path. Verify:

- the chart triangle lands at the requested atlas region;
- checker texture UV appearance is identical;
- tangent-space normal orientation is identical;
- material world-position-driven output is identical;
- capture does not write into the live scene's GBuffer or SceneColor;
- normal meshes rendered by `FLocalVertexFactory` are unchanged;
- the chart transform remains correct with non-zero `PreViewTranslation` and translated-world coordinates.

Only after this test passes should the implementation move to multi-triangle batching, atlas allocation, masked materials, or RT cache lookup.

## Performance warning

One draw per triangle is not a viable production solution for large scenes. It is only a proof that the existing Base Pass can shade a triangle at a generated chart location without changing its material inputs.

If the proof passes, the next optimization is to batch triangles sharing a chart transform/atlas tile, or introduce a dedicated chart-position stream while retaining the same raster-position hook. Do not optimize before correctness is demonstrated.

## Explicit non-goals

- No source UV rewriting.
- No modification of normal `FLocalVertexFactory` raster behavior.
- No material graph evaluation in ray traversal.
- No claim that the capture pass is implemented.
- No claim that WPO is supported by the current Software RT geometry.
- No compile or runtime validation claim.
