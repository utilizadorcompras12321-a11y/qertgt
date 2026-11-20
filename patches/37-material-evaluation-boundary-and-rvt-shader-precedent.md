# 37 — Material graph evaluation boundary and Runtime Virtual Texture shader precedent

Status: source audit against UE4.27. This identifies the existing material-evaluation boundary and a native shader-registration pattern suitable for Software RT Surface Cache capture. No Engine source was changed; no shader compile or render test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related:
- [Patch 18 — Static Mesh chart capture prototype design](18-static-mesh-chart-capture-prototype-design.md)
- [Patch 35 — Base Pass processor material-only capture audit](35-basepass-processor-material-only-capture-audit.md)
- [Patch 36 — Base Pass pixel output audit](36-basepass-pixel-output-and-material-only-capture-audit.md)

## 1. Exact material evaluation boundary

Source: `Engine/Shaders/Private/MaterialTemplate.ush`.

The generated material shader exposes:

```hlsl
void CalcPixelMaterialInputs(
    in out FMaterialPixelParameters Parameters,
    in out FPixelMaterialInputs PixelMaterialInputs)
```

The body contains translator-generated sections (the `%s` placeholders in the engine template) that evaluate the material graph's pixel inputs.

The function:
1. evaluates the generated initial expressions needed for the normal;
2. evaluates the material normal expression;
3. transforms the normal into the expected world/tangent representation;
4. evaluates the remaining generated material inputs;
5. calculates anisotropy tangent data where enabled.

The resulting `FPixelMaterialInputs` carries the generated material values, including BaseColor, Metallic, Specular, Roughness, EmissiveColor, Opacity, OpacityMask, Tangent and ShadingModel. Geometry-derived values such as final WorldNormal and AbsoluteWorldPosition are held in `FMaterialPixelParameters`.

Accessors in the same template include:
- `GetMaterialBaseColor()` / `GetMaterialBaseColorRaw()`;
- `GetMaterialMetallic()`;
- `GetMaterialSpecular()`;
- `GetMaterialRoughness()`;
- `GetMaterialEmissive()`;
- `GetMaterialOpacity()`;
- `GetMaterialMask()`;
- `GetMaterialShadingModel()`.

This is the correct boundary for material-only capture: call the generated material-input evaluation and read its outputs before any Base Pass light accumulation, fog, pre-exposure or scene-color composition.

Important distinction: `CalcMaterialParametersEx()` initializes world/screen/camera/facing parameters and calls `CalcPixelMaterialInputs()` internally. A capture shader should follow the established call sequence for its chosen position path and must not accidentally evaluate the generated graph twice. The capture entry point should call the appropriate `CalcMaterialParameters()` or `CalcMaterialParametersEx()` path once; it should not manually call both without a specific reason.

## 2. Native UE4.27 precedent: Runtime Virtual Texture material shader

Sources:
- `Engine/Shaders/Private/VirtualTextureMaterial.usf`
- `Engine/Source/Runtime/Renderer/Private/VT/RuntimeVirtualTextureRender.cpp`

The Runtime Virtual Texture pixel shader:
1. includes `Common.ush`;
2. includes `/Engine/Generated/Material.ush`;
3. includes `/Engine/Generated/VertexFactory.ush`;
4. obtains `FMaterialPixelParameters` through `GetMaterialPixelParameters()`;
5. obtains the material's pixel inputs through `CalcPixelMaterialInputs()` / `CalcMaterialParametersEx()`;
6. reads BaseColor, Specular, Roughness and WorldNormal;
7. writes a private output layout to `Out.MRT[]`.

Its output is not the screen-space Base Pass GBuffer. The shader declares its own `PIXELSHADEROUTPUT_MRT*` macros and includes `PixelShaderOutputCommon.ush` to generate the wrapper.

This proves UE4.27 already supports a material shader that evaluates the original material graph and writes a separate render-target payload without running the ordinary Base Pass lighting path.

## 3. Native C++ registration precedent

In `VT/RuntimeVirtualTextureRender.cpp`, Epic defines:

```cpp
class FShader_VirtualTextureMaterialDraw : public FMeshMaterialShader
```

It provides:
- `ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters&)`;
- `ModifyCompilationEnvironment(...)`;
- constructors using `FMeshMaterialShader`;
- material shader parameter setup through the `FMeshMaterialShader` base;
- templated VS/PS shader types;
- `IMPLEMENT_MATERIAL_SHADER_TYPE(..., SF_Pixel)` registration for `VirtualTextureMaterial.usf`.

At draw time the processor gets the shader from the actual material resource:

```cpp
MaterialResource.GetShader<FShader_VirtualTextureMaterialDraw_PS<MaterialPolicy>>(
    VertexFactory->GetType());
```

The Runtime Virtual Texture processor is a dedicated `FMeshPassProcessor`, not a modified Base Pass processor. It builds mesh draw commands using the material resource, vertex factory, primitive proxy and its own render state.

This is the architecture to reuse for Software RT capture.

## 4. Recommended Software RT adaptation

Create a dedicated material shader pair, for example:

- `FSoftwareRTMaterialCaptureVS : FMeshMaterialShader`
- `FSoftwareRTMaterialCapturePS : FMeshMaterialShader`
- shader source `/Engine/Private/SoftwareRTMaterialCapture.usf`
- a dedicated `FSoftwareRTMaterialCaptureMeshProcessor : FMeshPassProcessor`

The shader source should:
- include `Common.ush`, `/Engine/Generated/Material.ush` and `/Engine/Generated/VertexFactory.ush`;
- use the chart VF raster-position hook in its VS;
- preserve the original material vertex interpolants and material coordinate domain;
- initialize material pixel parameters using the proper UE4.27 helper path;
- evaluate the material graph exactly once;
- write an explicit private Surface Cache payload;
- declare only the MRT outputs actually written, then include `PixelShaderOutputCommon.ush`.

The initial payload should be small and explicit:
- MRT0: BaseColor.rgb + Roughness;
- MRT1: WorldNormal.xyz + Metallic;
- MRT2: Emissive.rgb + Specular;
- optional separate channels only when a later feature requires them.

These are a suggested first layout, not final texture-format commitments. Select formats based on the required precision and target-platform support. If a target platform cannot support the chosen MRT count/formats, reduce or split the payload rather than assuming desktop SM5 layout is universal.

The processor should:
- reject unsupported material domains/blend modes;
- retrieve the capture VS/PS from the actual `FMaterial` and chart VF type;
- preserve source material proxy and primitive identity;
- use a private capture render state and private MRT attachments;
- avoid the stock Base Pass lightmap policy and scene GBuffer bindings entirely.

## 5. Important details not to copy blindly from Runtime Virtual Texture

Runtime Virtual Texture is a precedent, not a drop-in implementation.

1. Its VS maps geometry into the virtual-texture projection. Software RT chart capture must instead use the chart raster transform while preserving source material UVs/interpolants.
2. Its pixel shader supports Runtime Virtual Texture output nodes and standard material outputs. Surface Cache capture should initially use standard material attributes and define a clear policy for RVT-specific output nodes.
3. Its `FMeshMaterialShader::SetParameters()` helper requests `ESceneTextureSetupMode::All`. Do not copy that indiscriminately: capture should bind only the scene/view/material resources actually required. Avoid pulling in scene textures just because the RVT pass does.
4. Its render state, blend state, depth policy and target formats are tailored to VT pages. Chart capture needs its own explicit state.
5. Its processor uses a specific cached/dynamic draw setup. Software RT must retain the audited uncached one-shot path until chart VF and per-draw mapping lifetimes are persistent and proven.
6. The exact generated material shader permutations still need to be restricted. Do not compile every material feature for the first proof.

## 6. Revised architecture decision

Do not modify `FBasePassMeshProcessor` to become the Surface Cache material evaluator.

Keep the normal Base Pass untouched. Build a separate, narrow `FMeshPassProcessor` plus `FMeshMaterialShader` pair following the Runtime Virtual Texture precedent. Reuse the UE material shader-map system and the generated `Material.ush` functions, but provide a dedicated chart VS, material-only PS output and private render targets.

This is not a second Material Graph implementation: the material graph is still compiled by Unreal's normal material translator. The new shader only supplies a different raster entry point and output contract.

## 7. Implementation gates before writing source

1. Verify the chart VF compiles with the new material VS for the intended feature level and material permutations.
2. Verify the material PS gets the correct material interpolants and calls the generated pixel-input path once.
3. Compile a simple opaque material with constant BaseColor/Roughness and verify the expected MRT values.
4. Confirm no Base Pass light accumulation, skylight, lightmap, fog, pre-exposure or scene-color dependency is compiled into the capture PS.
5. Confirm chart UVs affect only raster placement and do not replace the material's source UVs.
6. Confirm capture draw command, chart VF, material proxy, primitive data, pass UBs and target resources survive until the Patch 33 retirement boundary.

Only after this proof should the capture path add normal maps, texture sampling complexity, masked materials, WPO/PDO or view-dependent expressions.

## Source paths

- `Engine/Shaders/Private/MaterialTemplate.ush`
- `Engine/Shaders/Private/VirtualTextureMaterial.usf`
- `Engine/Shaders/Private/PixelShaderOutputCommon.ush`
- `Engine/Source/Runtime/Renderer/Private/VT/RuntimeVirtualTextureRender.cpp`
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.h`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`
- `Engine/Source/Runtime/RenderCore/Public/MaterialShader.h`
