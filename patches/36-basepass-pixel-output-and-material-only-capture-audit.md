# 36 — Base Pass pixel output audit: GBuffer is not a material-only Surface Cache

Status: shader-output audit against UE4.27 source. This patch corrects the scope of Patch 35. No Engine source was changed; no shader compile or GPU test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related:
- [Patch 18 — Static Mesh chart capture prototype design](18-static-mesh-chart-capture-prototype-design.md)
- [Patch 35 — Base Pass material-only capture audit](35-basepass-processor-material-only-capture-audit.md)

## 1. Correction to Patch 35

Forcing `LMP_NO_LIGHTMAP` is necessary to prevent lightmap policy selection, but it does **not** turn the stock Base Pass pixel shader into a material-property capture shader.

The stock opaque Base Pass still:
- evaluates the normal Base Pass lighting path;
- can enable skylight through `Scene->ShouldRenderSkylightInBasePass(...)`;
- calculates the Base Pass scene color output;
- applies fog/pre-exposure paths where relevant;
- encodes deferred material data through `EncodeGBuffer()`;
- writes the ordinary Base Pass MRT layout.

Therefore, Patch 35's proposed policy override alone is insufficient. A capture-specific pixel output path is required.

## 2. Confirmed stock shader output

Source: `Engine/Shaders/Private/BasePassPixelShader.usf`.

The opaque deferred path writes:

- `Out.MRT[0]`: Base Pass scene color, produced through the light accumulator and the Base Pass shading path.
- `Out.MRT[1]`, `Out.MRT[2]`, `Out.MRT[3]`: encoded GBuffer material data through `EncodeGBuffer(GBuffer, ...)`.
- Additional MRT slots: velocity, custom data and pre-shadow factor, conditional on platform/permutation macros.

The output declarations are controlled by `PIXELSHADEROUTPUT_MRT0...` macros at the end of the file. The macros are explicitly required to match `FSceneRenderTargets::GetGBufferRenderTargets()`.

This is not a small, generic chart MRT contract. It is the renderer's screen-space GBuffer contract, including platform-dependent output slots and formats.

## 3. Why binding a private chart MRT to the stock PS is unsafe

The stock PS output declarations are compiled from the Base Pass permutation and its `PIXELSHADEROUTPUT_MRT*` macros. The C++ render-target binding does not redefine what the compiled PS writes.

Consequences:
- A private chart target set with fewer/different attachments can mismatch the shader's declared outputs.
- Reusing scene GBuffer attachments would write chart-space fragments into screen-space buffers and corrupt the frame.
- Binding private targets with the same count is still insufficient unless every slot's format and semantic matches the compiled PS.
- Selective Base Pass outputs and velocity/custom-data/pre-shadow permutations make a fixed guessed MRT layout invalid.

Do not try to solve this by copying the scene GBuffer target array and replacing only selected textures. The shader's output permutation and target layout must be designed together.

## 4. GBuffer data is useful, but it is not the preferred cache payload

The GBuffer contains many material properties needed by downstream lighting, so it is a possible intermediate representation.

However:
- it is encoded/compressed for deferred shading rather than designed as a stable Surface Cache storage format;
- `EncodeGBuffer()` receives a per-pixel pseudo-random quantization bias;
- some channels are conditional on shading model and platform;
- some fields represent lighting-related state, such as `IndirectIrradiance`, rather than only raw material inputs;
- decoding the GBuffer later couples the cache to UE4's GBuffer layout and encoding rules.

For the first Surface Cache proof, prefer writing the raw supported material properties directly into a private, explicitly defined capture payload.

Candidate baseline payload:
- Base Color;
- World Normal (or a documented tangent-space normal convention);
- Roughness;
- Metallic;
- Specular;
- Emissive;
- Opacity/coverage only if the capture policy later supports masked materials.

Do not include precomputed lighting, skylight, scene color, velocity, pre-shadow factor or fog in this payload.

## 5. Recommended shader architecture

Keep the existing Base Pass untouched for normal scene rendering.

Create a dedicated Software RT material-capture pixel-shader permutation/type that:
1. uses the same material shader map and material graph compilation as the source material;
2. evaluates the material's pixel inputs;
3. writes only the explicitly declared Surface Cache payload to private chart MRTs;
4. performs no Base Pass light accumulation, skylight, lightmap, volumetric-lightmap, indirect-lighting-cache, fog or pre-exposure work;
5. uses a capture-specific output declaration whose C++ attachment formats and shader output slots are kept in lockstep.

A narrow capture branch inside `BasePassPixelShader.usf` may be considered if the material-input evaluation can be cleanly separated from the lighting/output code. Do not add a late branch after the full lighting path: that would retain the cost and side effects this capture is meant to avoid.

A separate `.usf` entry point is also possible in principle, but it must be proven to compile against UE4.27's generated material code and the existing `FMeshMaterialShader` material shader-map system. Do not assume that including `Material.ush` alone supplies all Base Pass interpolants, material parameter setup, pixel input evaluation and shader bindings.

## 6. Processor policy remains useful, but is secondary

The capture processor still needs to:
- reject unsupported blend modes/domains;
- force a no-lightmap policy where that policy is part of the shader type;
- disable skylight and other scene-lighting paths in the capture permutation;
- preserve source material proxy, source primitive identity and chart VF bindings;
- avoid simple-forward permutations for this deferred Surface Cache baseline.

But the dedicated capture PS is the actual boundary that guarantees material-only output. A C++ lightmap-policy override cannot by itself guarantee that.

## 7. First implementation gate

Before writing the implementation patch, answer these source-level questions:

1. Where in `BasePassPixelShader.usf` are all required material inputs fully evaluated, before Base Pass lighting and fog are accumulated?
2. Can that material-input stage be factored into a shared helper without changing the normal Base Pass output or generated material shader ABI?
3. Which UE4.27 shader registration mechanism allows the capture PS to retrieve the same material shader map while using a different entry point/output contract?
4. What exact `FPixelShaderOutputCommon.ush` declarations and C++ RDG attachment formats will be used for the capture payload?
5. Can the capture permutation compile for the existing chart VF without pulling in unsupported Base Pass resources?

Do not implement until these are answered. The next patch should audit the material-input evaluation boundary and shader registration/compilation path, not yet add a broad custom material renderer.

## Source paths

- `Engine/Shaders/Private/BasePassPixelShader.usf`
- `Engine/Shaders/Private/PixelShaderOutputCommon.ush`
- `Engine/Shaders/Private/BasePassCommon.ush`
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.h`
- `Engine/Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp`
