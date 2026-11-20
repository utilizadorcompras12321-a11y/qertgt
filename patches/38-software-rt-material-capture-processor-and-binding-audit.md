# 38 — Software RT material-capture mesh processor and shader binding audit

Status: API/source audit only. No Engine source was changed, no shaders were compiled and no runtime test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related:
- [Patch 20 — Chart VF registration and draw binding audit](20-chart-vf-registration-and-draw-binding-audit.md)
- [Patch 22 — Base Pass chart VF selection and mesh clone audit](22-basepass-chart-vf-selection-and-mesh-clone-audit.md)
- [Patch 24 — Base Pass primitive identity and dynamic draw audit](24-basepass-primitive-and-dynamic-draw-audit.md)
- [Patch 27 — Base Pass primitive and chart VF binding audit](27-basepass-primitive-and-chart-vf-binding-audit.md)
- [Patch 35 — Base Pass processor material-only capture audit](35-basepass-processor-material-only-capture-audit.md)
- [Patch 36 — Base Pass pixel output audit](36-basepass-pixel-output-and-material-only-capture-audit.md)
- [Patch 37 — Material evaluation boundary and RVT shader precedent](37-material-evaluation-boundary-and-rvt-shader-precedent.md)

## 1. Confirmed shader class and registration pattern

Relevant source:
- `Engine/Source/Runtime/Renderer/Public/MeshMaterialShader.h`
- `Engine/Source/Runtime/Renderer/Private/VT/RuntimeVirtualTextureRender.cpp`

`FMeshMaterialShader` derives from `FMaterialShader` and is the UE4.27 base for shaders that require both material and vertex-factory parameters.

The Runtime Virtual Texture implementation demonstrates the full pattern:
- A common material shader class derives from `FMeshMaterialShader`.
- Vertex and pixel shader classes derive from that common class.
- Each shader type uses `DECLARE_SHADER_TYPE(..., MeshMaterial)`.
- Shader types are registered with `IMPLEMENT_MATERIAL_SHADER_TYPE`.
- The shader source includes `/Engine/Generated/Material.ush` and `/Engine/Generated/VertexFactory.ush`.
- The processor obtains shader instances from the actual material resource using `MaterialResource.GetShader<ShaderType>(VertexFactory->GetType())`.

This is the correct registration family for Software RT capture. Do not register the material-capture PS as a global shader: it needs the material's generated shader map and the chart VF permutation.

The RVT `ShouldCompilePermutation` condition includes `bHasRuntimeVirtualTextureOutput || bIsDefaultMaterial`. Do **not** copy that condition. It would exclude ordinary materials which have no RVT output node. The Software RT permutation must be eligible for the supported material domain/blend modes and the chart VF, subject to the engine's normal material/VF shader-map compilation rules.

## 2. Material and vertex-factory binding is already handled by the mesh draw command path

Source: `Engine/Source/Runtime/Renderer/Private/ShaderBaseClasses.cpp`.

The engine's normal mesh draw command setup calls:

### `FMeshMaterialShader::GetShaderBindings()`

It delegates to `FMaterialShader::GetShaderBindings()`, which binds:
- the material uniform buffer from the material proxy's current `UniformExpressionCache`;
- referenced Material Parameter Collection uniform buffers.

Then `FMeshMaterialShader::GetShaderBindings()` binds:
- pass uniform buffer, but only if the shader declared/bound `PassUniformBuffer`;
- view uniform buffer;
- distance-cull fade uniform buffer;
- dither uniform buffer;
- instanced-view uniform buffer.

### `FMeshMaterialShader::GetElementShaderBindings()`

It binds:
- the selected vertex factory's shader parameters;
- primitive data according to the active GPU Scene / primitive-ID path;
- the primitive uniform buffer when the active path requires one.

This confirms the Patch 24/27 decision: keep the source primitive proxy, scene primitive identity, primitive ID mode and primitive uniform data intact. The chart VF supplies its own VF parameters; it must not replace or spoof the primitive binding path.

It also means the capture processor should not manually rebuild material texture/scalar/vector bindings. Let the normal material shader-map and uniform-expression machinery provide them.

## 3. Do not copy the RVT legacy SetParameters helper blindly

The RVT shader class contains a helper which calls `FMeshMaterialShader::SetParameters(..., View.ViewUniformBuffer, ESceneTextureSetupMode::All)`. However, the RVT `DrawMeshes()` path builds and submits mesh draw commands; it does not explicitly call that helper for each mesh.

In the mesh draw command path, material and VF bindings are assembled through `GetShaderBindings()` and `GetElementShaderBindings()`. The helper is not the binding mechanism to reproduce in the Software RT processor.

For the new shader:
- Do not request `ESceneTextureSetupMode::All` by default.
- Do not bind scene textures unless the capture shader actually references them.
- Do not bind a pass uniform buffer merely because the RVT class binds one.
- If `PassUniformBuffer` is bound in the shader initializer, the processor's `FMeshPassProcessorRenderState` must provide the matching pass uniform buffer; the base implementation checks this.
- Keep the capture PS free of scene-color, fog, pre-exposure and lighting dependencies.

## 4. Processor construction and draw-command sequence

The RVT processor is a direct, concrete example:

1. Derive `FRuntimeVirtualTextureMeshProcessor` from `FMeshPassProcessor`.
2. Initialize the base processor with Scene, feature level, optional dynamic view and draw-list context.
3. Configure `FMeshPassProcessorRenderState` with view/instanced-view UBs and explicit depth state.
4. In `AddMeshBatch()`, resolve the actual material resource and fallback proxy.
5. In the typed `Process<MaterialPolicy>()`, obtain VS and PS from `MaterialResource.GetShader<...>(VertexFactory->GetType())`.
6. Compute fill/cull state from the mesh batch and material.
7. Initialize `FMeshMaterialShaderElementData` with `InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false)`.
8. Call `BuildMeshDrawCommands()` with the original mesh batch, primitive proxy, material proxy/resource, render state, shader set and element data.

For Software RT, replace only the material selection and chart-capture state:
- Accept only batches with a valid Software RT capture mapping and supported chart VF.
- Resolve the effective material proxy/resource for the section.
- Retrieve the capture VS/PS for the chart VF type.
- Preserve the original `FPrimitiveSceneProxy`, `FMeshBatchElement` primitive data and source material proxy.
- Use the capture-specific MRT/depth/blend state.
- Keep the source material's original vertex interpolants and UV domain; chart coordinates affect raster placement only.

Do not reuse the RVT-specific `RuntimeVirtualTextureMaterialType`, RVT output attribute mask, blend policies or page projection.

## 5. Initial uncached submission path

RVT's `DrawMeshes()` shows the concrete uncached mesh draw setup:
- `FDynamicMeshDrawCommandStorage`;
- `FMeshCommandOneFrameArray`;
- `FGraphicsMinimalPipelineStateSet`;
- `FDynamicPassMeshDrawListContext`;
- a processor instance using that context;
- calls to `AddMeshBatch()`;
- `SortAndMergeDynamicPassMeshDrawCommands()`;
- `SubmitMeshDrawCommands()`.

RVT also merges cached pass commands. Software RT chart batches must **not** enter the stock cached Base Pass lists. The first capture implementation should use the uncached dynamic path and the chart-owned batch clones already specified in Patches 22–28.

The one-frame storage is appropriate for a correctness prototype, not yet a high-frequency persistent capture architecture. Patch 25 already records that limitation. A later optimization can introduce a persistent capture draw-command owner after the resource-retirement protocol is proven.

## 6. Render Graph attachment contract

RVT creates its raster pass with RDG render-target bindings in a pass-parameter struct and executes mesh draws inside the raster pass lambda. This is a valid UE4.27 pattern for the private Surface Cache MRTs.

Software RT should use its own pass parameter struct and explicit attachment formats. The shader's `PIXELSHADEROUTPUT_MRT*` declarations must exactly match the C++ RDG render-target slots. Do not reuse the stock scene GBuffer or RVT output formats by assumption.

The chart capture pass must also ensure its viewport/scissor and chart raster transform agree. A valid shader binding alone does not prove chart UV-to-clip mapping is correct.

## 7. Revised implementation plan

The first source implementation should now be limited to these pieces:

1. `FSoftwareRTMaterialCaptureVS` and `FSoftwareRTMaterialCapturePS`, both registered as `MeshMaterial` shader types.
2. A dedicated `SoftwareRTMaterialCapture.usf` with a chart-VF-aware VS and a material-only PS.
3. A dedicated `FSoftwareRTMaterialCaptureMeshProcessor` deriving from `FMeshPassProcessor`.
4. A private RDG raster pass with a minimal Surface Cache MRT layout.
5. One test material and one static-mesh section, using the original primitive identity and material proxy.
6. A diagnostic capture target/readback proving BaseColor, Roughness, Metallic, Specular, Emissive and WorldNormal are written as expected.

Do not add masked/translucent materials, WPO/PDO, view-dependent expressions, material blending, dynamic mesh support or multi-chart batching in the first compile proof.

## 8. Remaining compile/runtime gates

Still not proven by source inspection:
- The chart VF's `ShouldCompilePermutation` and the material shader type will produce the intended chart-VF permutations for the selected materials.
- The wrapper shader path and generated `VertexFactory.ush` compile together.
- The capture VS's chart raster position does not corrupt the original material vertex parameters/interpolants.
- The capture PS evaluates the generated material inputs exactly once.
- The pass supplies all required view/pass/primitive/VF bindings without binding unused scene textures.
- The chosen MRT count and formats are supported by the target RHI.
- The first draw command survives with the chart VF, material proxy, primitive data, mapping UB and RDG attachments alive through submission and retirement.

## Source paths inspected

- `Engine/Source/Runtime/Renderer/Public/MeshMaterialShader.h`
- `Engine/Source/Runtime/Renderer/Public/MaterialShader.h`
- `Engine/Source/Runtime/Renderer/Private/ShaderBaseClasses.cpp`
- `Engine/Source/Runtime/Renderer/Private/VT/RuntimeVirtualTextureRender.cpp`
- `Engine/Shaders/Private/VirtualTextureMaterial.usf`
- `Engine/Shaders/Private/MaterialTemplate.ush`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`
