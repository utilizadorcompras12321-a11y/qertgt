# 39 — Surface Cache material-capture HLSL contract and chart-position correctness

Status: HLSL/source audit and implementation contract. No Engine source was changed. No shader was compiled and no runtime test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related:
- [Patch 18 — Static Mesh chart capture prototype design](18-static-mesh-chart-capture-prototype-design.md)
- [Patch 19 — Chart rasterization through Base Pass VF hook](19-chart-rasterization-through-basepass-vertex-factory-hook.md)
- [Patch 21 — Experimental chart vertex factory registration](21-register-experimental-chart-vertex-factory.patch.md)
- [Patch 37 — Material evaluation boundary and RVT shader precedent](37-material-evaluation-boundary-and-rvt-shader-precedent.md)
- [Patch 38 — Material capture processor and binding audit](38-software-rt-material-capture-processor-and-binding-audit.md)

## 1. Confirmed HLSL entry-point pattern

Source: `Engine/Shaders/Private/VirtualTextureMaterial.usf`.

The UE4.27 material shader pattern is:

```hlsl
#include "Common.ush"
#include "/Engine/Generated/Material.ush"
#include "/Engine/Generated/VertexFactory.ush"
```

The vertex entry point:
1. calls `ResolveView()`;
2. creates `FVertexFactoryIntermediates`;
3. gets translated world position through `VertexFactoryGetWorldPosition()`;
4. gets tangent basis through `VertexFactoryGetTangentToLocal()`;
5. builds `FMaterialVertexParameters`;
6. gets material interpolants through `VertexFactoryGetInterpolantsVSToPS()`;
7. obtains raster position separately through `VertexFactoryGetRasterizedWorldPosition()`;
8. transforms raster position with `ResolvedView.TranslatedWorldToClip`.

This confirms that raster position and the vertex parameters/interpolants can be kept as separate concepts. It does **not** by itself solve how the chart coordinate is supplied to each vertex.

The pixel wrapper pattern is:
- define `FPixelShaderInOut_MainPS(...)`;
- define `PIXELSHADEROUTPUT_INTERPOLANTS`;
- define the exact `PIXELSHADEROUTPUT_MRTn` macros;
- include `PixelShaderOutputCommon.ush`, which generates the actual `MainPS` entry point.

The C++ shader registration must therefore point to `MainVS` and `MainPS`, not to `FPixelShaderInOut_MainPS`.

## 2. Correct material-input evaluation sequence

Source: `Engine/Shaders/Private/MaterialTemplate.ush`.

`CalcMaterialParametersEx()` performs all of the following:
- initializes world/camera/screen-related pixel parameters;
- sets the two-sided sign;
- initializes virtual-texture feedback when compiled in;
- calls `CalcPixelMaterialInputs()` at the end.

Therefore the capture PS must **not** call `CalcPixelMaterialInputs()` separately before `CalcMaterialParametersEx()`. That would evaluate the generated material graph twice.

The correct conceptual sequence is:

```hlsl
ResolvedView = ResolveView();

FMaterialPixelParameters MaterialParameters =
    GetMaterialPixelParameters(Interpolants, In.SvPosition);

FPixelMaterialInputs PixelMaterialInputs = (FPixelMaterialInputs)0;

float4 ScreenPosition =
    SvPositionToResolvedScreenPosition(In.SvPosition);

// IMPORTANT: this must be the interpolated ORIGINAL surface position,
// not a position reconstructed from the chart-rasterized SV_Position.
float3 SurfaceTranslatedWorldPosition = Interpolants.SurfaceTranslatedWorldPosition;

CalcMaterialParametersEx(
    MaterialParameters,
    PixelMaterialInputs,
    In.SvPosition,
    ScreenPosition,
    In.bIsFrontFace,
    SurfaceTranslatedWorldPosition,
    SurfaceTranslatedWorldPosition);
```

This is a contract illustration, not compile-ready code: `SurfaceTranslatedWorldPosition` does not exist in stock `FVertexFactoryInterpolantsVSToPS`. A chart-specific varying must be added and wired through the VS output and PS input before this can compile.

For the first baseline, exclude expressions that depend on the capture view or chart screen coordinates, including CameraVector, ScreenPosition, SceneTexture lookups, view-dependent effects and any other view-dependent material functions. The chart pass has a synthetic raster projection, not the player's camera.

## 3. Critical correction: chart raster position cannot be used as surface world position

The chart VF hook changes the position used to calculate `SV_Position`. The stock `CalcMaterialParameters()` convenience function reconstructs translated world position from `SV_Position` using the active view matrices.

If the chart projection is encoded by returning a synthetic position from `VertexFactoryGetRasterizedWorldPosition()`, reconstructing world position from the resulting `SV_Position` returns the chart projection's synthetic position, **not the original surface position**.

That corrupts:
- AbsoluteWorldPosition and WorldPosition material expressions;
- world-position texture/object mapping;
- world-space normal/tangent calculations if they depend on position;
- CameraVector (already excluded in the initial baseline);
- any material expression using screen/world reconstruction.

The capture PS must receive the original translated world position as a separate perspective-correct interpolant, generated from the original geometry position before chart remapping. The same principle applies to any other original geometric attribute that cannot be reconstructed from chart `SV_Position`.

Do not call `SvPositionToResolvedTranslatedWorld()` for the material's world position after chart remapping.

## 4. Critical correction: chart coordinates must be available per vertex

A chart mapping stores three chart UVs per source triangle. A normal indexed Static Mesh vertex can be shared by multiple triangles and therefore can have different chart UVs for each triangle.

A per-draw uniform containing only `ChartUV[3]` is insufficient unless the vertex shader can reliably identify the triangle corner (0, 1 or 2). Stock `FVertexFactoryInput` for the normal Local Vertex Factory path does not expose a general draw-relative corner index. `SV_VertexID` is only declared in specific Local VF permutations such as manual vertex fetch / GPU-skin pass-through; it is not a general safe assumption for the normal Local VF path.

Consequently:
- Do not index `ChartUV[3]` using an assumed `SV_VertexID`.
- Do not assume the original shared Static Mesh vertex can carry one unique chart UV.
- Do not overwrite source material UV channels with chart UVs.
- Do not assume a per-triangle uniform alone solves the corner lookup.

### Required implementation decision

For the first correct prototype, use chart-expanded capture geometry: each captured source triangle has three capture vertices, each carrying:
- the original position;
- original tangent basis;
- original material UV channels;
- original vertex color if supported;
- the chart UV for that triangle corner;
- any other source attributes needed by the selected material permutation.

The capture VS uses the chart UV only to generate raster position and separately forwards the original translated world position and original material interpolants.

This requires a dedicated capture vertex stream / vertex-factory data layout or an equivalent explicit per-corner data source. It is **not** implemented by simply cloning `FLocalVertexFactory` and changing its uniform buffer. The current Patch 21/22 chart VF proposal must be extended to describe and bind this per-corner stream.

For correctness, a temporary CPU-generated expanded triangle buffer is acceptable for the first test. It is not the final performance path. A later optimization can pack chart-expanded vertices persistently and reuse them across capture generations.

## 5. Proposed material-only PS payload

The first test should use three private MRTs with a simple float payload:

| Target | Channels | Value |
|---|---|---|
| MRT0 | RGB, A | BaseColor.rgb, Roughness |
| MRT1 | RGB, A | WorldNormal.xyz, Metallic |
| MRT2 | RGB, A | Emissive.rgb, Specular |

Conceptual output:

```hlsl
half3 BaseColor = GetMaterialBaseColor(PixelMaterialInputs);
half Metallic = GetMaterialMetallic(PixelMaterialInputs);
half Specular = GetMaterialSpecular(PixelMaterialInputs);
half Roughness = GetMaterialRoughness(PixelMaterialInputs);
half3 Emissive = GetMaterialEmissive(PixelMaterialInputs);
half3 WorldNormal = MaterialParameters.WorldNormal;

Out.MRT[0] = float4(BaseColor, Roughness);
Out.MRT[1] = float4(normalize(WorldNormal) * 0.5f + 0.5f, Metallic);
Out.MRT[2] = float4(Emissive, Specular);
```

This payload is a diagnostic baseline, not a final cache encoding. The normal is encoded into [0,1] for storage; the consumer must decode it. The exact target formats and precision must be selected with the RDG attachment declarations and RHI capabilities together. Three `PF_FloatRGBA` targets are an uncomplicated initial precision choice on the intended desktop path, but are not yet approved as the universal format for every UE4.27 RHI.

Do not multiply material values by Opacity for opaque materials. Masked materials are excluded from the first proof; their coverage/clip semantics require a separate policy. Translucency is excluded.

## 6. Shader output declarations

The first three-target permutation must declare:

```hlsl
#define PIXELSHADEROUTPUT_INTERPOLANTS 1
#define PIXELSHADEROUTPUT_MRT0 1
#define PIXELSHADEROUTPUT_MRT1 1
#define PIXELSHADEROUTPUT_MRT2 1

#include "PixelShaderOutputCommon.ush"
```

No Base Pass output macro should be enabled. Do not include `PIXELSHADEROUTPUT_BASEPASS`; do not declare scene color, GBuffer, velocity, fog, pre-shadow or lighting outputs.

The C++ RDG pass must bind exactly three color attachments in the same order and with formats matching the chosen output encoding. Depth can be omitted for a first one-triangle isolated test if the pass guarantees no overlap; for real chart capture, use a private depth target or another explicit overlap policy. Do not use the scene depth buffer.

## 7. Chart vertex shader contract

The chart VS must preserve two independent positions:

1. `SurfaceTranslatedWorldPosition`: original geometry position after the supported vertex deformation policy, before chart remapping. For the initial baseline, WPO is disabled, so this is the original transformed position.
2. `ChartRasterPosition`: the chart-space raster position converted into the coordinate expected by `ResolvedView.TranslatedWorldToClip`.

The chart transform must account for:
- chart tile origin and dimensions;
- atlas viewport/scissor;
- PreViewTranslation convention;
- clip-space W and depth;
- viewport orientation and pixel-center convention.

The chart UV must be interpolated from the chart-expanded per-corner stream. Do not return chart UV directly as a world position. The raster hook's return value must be a position in the exact translated-world space expected by the view matrix, or the shader must use a dedicated clip-position path with a separately defined contract.

The current Local VF shader wrapper includes `LocalVertexFactory.ush`, whose stock `VertexFactoryGetRasterizedWorldPosition()` simply returns its input world position. A compile-time chart define alone does not change that function. The implementation must add an explicit chart-VF branch to the relevant VF shader code (or use a genuinely separate VF shader implementation). Merely defining `SOFTWARE_RT_CHART_VF` in the wrapper is insufficient.

## 8. First compile proof and stop conditions

Before integrating the full mesh processor, compile and render one chart-expanded triangle with:
- a constant opaque material;
- BaseColor, Roughness, Metallic, Specular and Emissive constants;
- no WPO/PDO;
- no masked/translucent blend;
- no custom interpolators;
- no scene textures or view-dependent expressions.

Validate:
1. chart triangle lands in the intended atlas tile;
2. original UVs remain unchanged;
3. the original world position reaches the PS correctly despite chart rasterization;
4. WorldNormal is correct under the supported transform;
5. all three MRT values match the material constants;
6. no stock Base Pass outputs or lighting are executed.

Stop if the per-corner chart stream or original-world-position varying requires a broad rewrite of Local VF or generated material ABI. In that case, prefer a dedicated minimal capture VF with explicit inputs over invasive global changes.

## 9. Source paths inspected

- `Engine/Shaders/Private/VirtualTextureMaterial.usf`
- `Engine/Shaders/Private/PixelShaderOutputCommon.ush`
- `Engine/Shaders/Private/MaterialTemplate.ush`
- `Engine/Shaders/Private/LocalVertexFactory.ush`
- `Engine/Shaders/Private/BasePassVertexShader.usf`
- `Engine/Source/Runtime/Renderer/Private/VT/RuntimeVirtualTextureRender.cpp`
