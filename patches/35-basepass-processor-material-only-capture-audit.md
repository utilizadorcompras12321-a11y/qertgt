# 35 — Base Pass processor reuse: material-only capture constraints

Status: audit of the UE4.27 Base Pass mesh processor and its uniform-buffer setup. This patch records what can be reused and what must be isolated for Surface Cache capture. No engine source was changed; no compile or render test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related:
- [Patch 18 — Static Mesh chart capture prototype design](18-static-mesh-chart-capture-prototype-design.md)
- [Patch 22 — Base Pass selection and mesh clone audit](22-basepass-chart-vf-selection-and-mesh-clone-audit.md)
- [Patch 27 — Base Pass primitive and Chart-VF binding audit](27-basepass-primitive-and-chart-vf-binding-audit.md)
- [Patch 34 — Deferred renderer capture graph integration](34-deferred-renderer-capture-graph-integration-audit.md)

## 1. Confirmed reusable pieces

### 1.1 Base Pass processor construction

`BasePassRendering.h` declares `FBasePassMeshProcessor` with constructor inputs:

- `const FScene*`;
- feature level;
- dynamic view;
- `FMeshPassProcessorRenderState`;
- `FMeshPassDrawListContext*`;
- `FBasePassMeshProcessor::EFlags`;
- optional translucency pass type.

The established dynamic path in `Renderer/Private/Renderer.cpp` constructs this processor inside `DrawDynamicMeshPass()`, calls `AddMeshBatch()`, and submits through the dynamic pass context. This confirms the Base Pass processor can be used with a capture-owned cloned `FMeshBatch`; it does not require replacing the Static Mesh's registered VF.

### 1.2 Base Pass uniform buffer

`BasePassRendering.h/.cpp` provides an RDG overload:

```cpp
TRDGUniformBufferRef<FOpaqueBasePassUniformParameters>
CreateOpaqueBasePassUniformBuffer(
    FRDGBuilder& GraphBuilder,
    const FViewInfo& View,
    FRDGTextureRef ForwardScreenSpaceShadowMask,
    const FSceneWithoutWaterTextures* SceneWithoutWaterTextures,
    int32 ViewIndex);
```

The normal deferred Base Pass creates `FOpaqueBasePassParameters`, assigns this RDG uniform buffer to `PassParameters->BasePass`, and assigns its render-target binding slots before adding the raster pass.

This is the correct starting point for Base Pass global material parameters in the capture graph. Prefer the RDG overload so resource references participate in graph lifetime tracking. Do not create a transient uniform buffer in a stack scope and bind it globally across an unrelated queued pass.

### 1.3 Mesh processor render state

The normal path constructs `FMeshPassProcessorRenderState DrawRenderState(View)`, then calls `SetupBasePassState(...)`. The processor uses that state when building its draw commands.

A capture pass needs its own render state. Do not blindly copy `Scene->DefaultBasePassDepthStencilAccess` or the scene Base Pass render-target slots: chart capture has a different target and depth policy.

## 2. Critical mismatch: FBasePassMeshProcessor is not a material-only evaluator

`FBasePassMeshProcessor::AddMeshBatch()` resolves the material proxy/fallback and calls `TryAddMeshBatch()`.

For opaque mode, `TryAddMeshBatch()`:
- accepts non-translucent materials only;
- applies `ShouldIncludeDomainInMeshPass()` and `ShouldIncludeMaterialInDefaultOpaquePass()`;
- inspects `MeshBatch.LCI` for lightmap interaction;
- reads `r.AllowStaticLighting` and high/low-quality lightmap settings;
- checks scene volumetric lightmap and precomputed light volume state;
- selects among lightmap, distance-field-shadow, volumetric-lightmap, cached indirect-lighting and no-lightmap policies.

Therefore, directly reusing the stock processor can select a lighting policy that is not appropriate for a material-only Surface Cache. A capture intended to store intrinsic material properties must not accidentally bake lightmap or cached indirect-lighting contributions into its cached result.

The processor's templated `Process<LightMapPolicyType>()` is private. A caller cannot simply force `LMP_NO_LIGHTMAP` through its public API.

## 3. Recommended narrow change: explicit capture mode

Do not fork the whole Base Pass shader system and do not use an undocumented `LCI = nullptr` trick as the long-term contract.

Add a narrow, explicit capture-mode setting to the Base Pass processor (or a tightly scoped capture processor in the same Renderer module) with these rules:

1. Require opaque blend mode and the supported material domain.
2. Force the `LMP_NO_LIGHTMAP` material shader policy for capture.
3. Disable skylight/atmospheric-fog contributions that are not part of the material property cache.
4. Do not select lightmap, distance-field-shadow, volumetric-lightmap or cached indirect-lighting policies.
5. Preserve the original material proxy and material shader map selection.
6. Preserve the chart VF and primitive identity bindings audited in Patches 20–28.
7. Keep the ordinary scene Base Pass path unchanged when capture mode is disabled.

The exact implementation should be limited to the policy-selection branch in `TryAddMeshBatch()` and the parameters used by `Process()`. Do not duplicate the entire `FBasePassMeshProcessor` class.

Whether this requires a dedicated capture PS permutation or can reuse the existing `LMP_NO_LIGHTMAP` Base Pass PS must be confirmed by a shader-output proof before implementation.

## 4. Render targets are not interchangeable

The ordinary opaque Base Pass uses `FOpaqueBasePassParameters` and the scene's GBuffer render-target slots. The Base Pass pixel shader writes the GBuffer outputs expected by its shader permutation.

A chart capture target cannot be an arbitrary single RGBA texture while still using the normal Base Pass pixel shader. The capture pass must either:

- provide a chart MRT set with formats and slot semantics compatible with the selected Base Pass PS outputs; or
- use a dedicated capture pixel shader/permutation that writes the smaller Surface Cache payload explicitly.

Do not bind scene GBuffer textures as chart targets; that would overwrite the screen-space GBuffer with chart-space rasterization.

The first prototype should use a private chart MRT set, with formats chosen to match the required material properties. If the stock Base Pass PS writes extra outputs that are not useful, use a dedicated capture PS variant rather than silently dropping or aliasing MRT slots.

## 5. Depth/stencil flag is meaningful

`FBasePassMeshProcessor::EFlags::CanUseDepthStencil` sets `bEnableReceiveDecalOutput`. It is not a generic "allow this pass" flag.

For a chart atlas:
- do not set `CanUseDepthStencil` unless the capture pass actually binds a compatible depth/stencil target and intends to preserve the normal decal/stencil output behavior;
- choose a capture-specific depth state deliberately;
- chart triangles should not depend on the scene depth buffer because chart-space raster coordinates are not scene-space depth;
- if chart tiles are non-overlapping, depth may be unnecessary for the first proof, but the PSO and render-pass attachment contract must be verified.

Do not reuse the scene Base Pass depth/stencil attachment for chart-space rendering.

## 6. First proof should not capture all materials

The first working capture test should use:
- deferred shading path;
- opaque, non-masked material;
- no tessellation;
- no WPO/PDO;
- no view-dependent expressions;
- no static lightmap/indirect-lighting contribution in the capture policy;
- private chart MRTs;
- one chart triangle with a simple constant/base-color material;
- a persistent chart VF and a single one-shot capture generation.

Only after the shader permutation and output attachments are proven should more material features be admitted.

## 7. Decision

Reuse these UE4.27 pieces:
- `FBasePassMeshProcessor` command-building structure, with an explicit capture policy;
- `CreateOpaqueBasePassUniformBuffer(GraphBuilder, ...)` for RDG-tracked Base Pass globals;
- existing `FMeshBatch` cloning, chart VF and primitive identity contracts.

Do **not** directly reuse the stock opaque processor unchanged. Its lighting-policy selection can inject lightmap or cached lighting behavior, and its pixel shader expects Base Pass GBuffer output attachments. The capture path needs an explicit material-only policy and a private, compatible MRT layout or a capture-specific pixel shader.

## Source paths

- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.h`
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`
- `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`
