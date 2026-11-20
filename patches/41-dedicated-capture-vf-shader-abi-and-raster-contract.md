# 41 — Dedicated capture Vertex Factory shader ABI and raster-position contract

Status: design/audit decision. This is not Engine source, has not been compiled, and has not been runtime-tested.

Target: UE 4.27.2.
Reference mirror intended by this work: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.
Important limitation: the GitHub file connector did not return the mirror source during this turn. Therefore this document does not claim a fresh line-by-line verification of the mirror. The implementation must be checked against the user's exact 4.27.2 checkout before code is written.

Related:
- [Patch 39 — Surface Cache material-capture HLSL contract](39-surface-cache-material-capture-hlsl-contract.md)
- [Patch 40 — Chart-expanded capture geometry and dedicated VF](40-chart-expanded-capture-geometry-and-dedicated-vf.md)

## 1. Decisions

The capture VF is a standalone `FVertexFactory`, not a subclass of `FLocalVertexFactory`.

Its shader file must own the complete capture vertex input and interpolant contract. Do not include the stock Local VF shader and assume a preprocessor define changes its input layout or raster behavior.

The capture VS must write atlas clip-space position directly to `SV_POSITION`. Do not encode chart coordinates as a fabricated translated-world position and then multiply them by `ResolvedView.TranslatedWorldToClip`. That couples atlas rasterization to the scene view and makes the transform dependent on camera matrices.

The capture VS must separately forward the original surface data needed by the material PS:
- original translated-world position;
- original tangent frame (or the minimum basis needed to reconstruct it);
- original material UV channels;
- vertex color when supported;
- primitive identity required by material shader bindings.

Chart UV is a capture-only input and must never be inserted into the material UV array.

## 2. Vertex input and C++ declaration contract

The expanded CPU vertex is a per-corner record. The C++ VF declaration and HLSL `FVertexFactoryInput` must agree exactly on:
- attribute index;
- component count;
- element type/precision;
- stream index;
- byte offset and stride;
- normalization behavior.

Suggested semantic allocation (provisional; finalize against UE4.27's RHI vertex element types):
- ATTRIBUTE0: local Position;
- ATTRIBUTE1: packed TangentX;
- ATTRIBUTE2: packed TangentZ including sign;
- ATTRIBUTE3: ChartUV;
- ATTRIBUTE4 onward: original material UV channels;
- a separate declared attribute for Color, if enabled.

Do not copy this table into code until the exact 4.27.2 `EVertexElementType` choices and platform limits have been checked. In particular, the maximum vertex attribute count and half/normalized formats vary by RHI backend.

The VF must construct its vertex declaration in `InitRHI` from the same stream layout used by the CPU builder. It must expose only the normal draw stream needed for this capture pass; depth-only and position-only declarations can be omitted if the capture pass never requests them, but the mesh draw path must be configured not to request unsupported stream types.

## 3. HLSL responsibilities

The capture shader source owns:
1. `FVertexFactoryInput`;
2. `FVertexFactoryIntermediates`;
3. `GetVertexFactoryIntermediates`;
4. `VertexFactoryGetWorldPosition`;
5. `VertexFactoryGetTangentToLocal`;
6. `VertexFactoryGetInterpolantsVSToPS`;
7. `GetMaterialPixelParameters` (capture-specific implementation);
8. the capture vertex entry point and capture pixel wrapper.

The exact signatures and common helper dependencies must be copied from the UE4.27.2 shader sources, not reconstructed from UE5 documentation or a different engine fork.

The material shader path must still include the generated material shader code and call the material graph through `CalcMaterialParametersEx` exactly once. The capture VF's `GetMaterialPixelParameters` must initialize the full `FMaterialPixelParameters` structure in the same way the engine's stock VF does, then populate the original surface position and material-facing attributes.

Do not add a field to the engine-global material ABI. Keep the extra capture data inside this VF's own `FVertexFactoryInterpolantsVSToPS` and its matching `GetMaterialPixelParameters` implementation. This is the intended extension seam, but it still needs a shader compile proof against 4.27.2.

## 4. Raster and material coordinate separation

The VS computes:
- `SurfaceTranslatedWorldPosition` from original local Position and the source primitive transform;
- `ChartClipPosition` from ChartUV and the chart-to-atlas transform;
- original material interpolants from the source attributes.

The output position is `ChartClipPosition`, not a transformed pseudo-world coordinate.

The chart-to-atlas transform must include explicit atlas dimensions, tile origin, tile extent, and the RHI viewport Y convention. The pass must set a matching viewport/scissor and must not use the scene camera projection.

The PS must consume the interpolated original surface position when initializing material parameters. It must not reconstruct world position from chart `SV_Position`.

For the first prototype, reject WPO, PDO, tessellation, spline deformation, instancing, masked blend mode, translucency, custom interpolators and view-dependent material expressions. WPO can be revisited only after a deliberate policy is defined for whether the cache represents pre- or post-WPO geometry.

## 5. Material shader binding and primitive identity

The capture VF must not replace the source primitive identity with an arbitrary capture primitive.

The draw command must preserve the source `FPrimitiveSceneProxy`, primitive uniform/GPU Scene data and `PrimitiveIdMode`. The custom VF only supplies capture vertex attributes and its own VF parameters.

The capture shader should be retrieved as a `FMeshMaterialShader` for the capture VF type, using the effective material resource for the source section. Material uniform-expression and parameter-collection bindings remain on the engine's material shader binding path.

No scene textures, scene color, GBuffer, precomputed lighting or Base Pass outputs are required.

## 6. Minimal compile/render proof — required before processor integration

Do not implement the full capture processor yet.

First create a dedicated debug shader pair and render one expanded triangle into one private `PF_FloatRGBA` target:
- VS reads the expanded vertex stream;
- VS writes atlas clip position;
- VS forwards original translated-world position;
- PS writes a debug color derived from the interpolated original position.

Acceptance checks:
1. the triangle lands at the expected atlas pixels regardless of the camera's view/projection;
2. moving the camera does not move the chart triangle;
3. changing chart UV changes only atlas raster placement;
4. original translated-world position matches the source primitive transform;
5. material UV0..UVn are unchanged;
6. tangent sign survives packing and mirrored transforms;
7. the capture draw does not alter the source Static Mesh or its normal Base Pass.

Only after this proof passes should the shader be changed to `FMeshMaterialShader` and evaluate the Material Graph.

## 7. Stop conditions and risks

Stop if the capture material shader cannot initialize `FMaterialPixelParameters` with the original surface data without changing engine-global material shader ABI.

Stop if the chosen RHI cannot represent the required vertex layout within its attribute/format limits. In that case, split the layout across multiple streams or use a compact structured-buffer/manual-fetch design; do not silently drop UV channels or tangent precision.

CPU expansion is only a prototype path. Cooked assets without retained CPU source data must be skipped cleanly. This is a functional coverage limitation, not merely a performance concern.

The exact `CalcMaterialParametersEx` call signature, VF shader helper signatures, vertex declaration API and material shader registration must be verified in the exact 4.27.2 checkout before implementation. No compile-ready HLSL or C++ is claimed by this patch.

## 8. Next step

Patch 42 should audit the exact 4.27.2 definitions for:
- `FVertexFactoryInterpolantsVSToPS` and `GetMaterialPixelParameters`;
- `FMaterialPixelParameters` initialization and `CalcMaterialParametersEx`;
- `FVertexFactory::InitRHI`, vertex declaration creation and stream binding;
- the `FMeshMaterialShader` VS/PS registration and permutation interface.

Then produce the smallest compile-ready debug VF, without Material Graph evaluation.