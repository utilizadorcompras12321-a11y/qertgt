# Patch 45 — Custom Material Shader Registration and Mesh Processor Contract

**Target:** UE 4.27.2  
**Branch:** plugin/software-rt-material-evaluation  
**Mirror:** AlexMercer-MA/UnrealEngine-4.27  
**Pinned mirror commit:** 306a7e9f7fb53f14c2cb3bfd3854ad906710a17e  
**Status:** API audit / processor design. Not applied to the user's Engine checkout; not compiled.

## 1. Purpose

Patch 44 closed the primitive/material/view binding requirements. This patch closes the next layer: how the custom debug VS/PS are registered against a VF type, retrieved from a material shader map, and passed into BuildMeshDrawCommands().

Do not write a fake direct RHI draw around the VF. The test must go through FMeshPassProcessor and BuildMeshDrawCommands() so that the engine performs the normal VF shader-parameter dispatch, primitive binding, stream setup, and draw-command finalization.

## 2. Verified shader registration pattern

The pinned mirror uses the following pattern for material shaders:

- Declare the shader class with DECLARE_SHADER_TYPE(ShaderClass, MeshMaterial).
- Derive from FMeshMaterialShader.
- Implement the compiled-shader constructor taking FMeshMaterialShaderType::CompiledShaderInitializerType (or the compatible ShaderMetaType alias).
- Implement static ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters&).
- Register with IMPLEMENT_MATERIAL_SHADER_TYPE(, ShaderClass, TEXT("/Engine/Private/YourShader.usf"), TEXT("EntryPoint"), SF_Vertex/SF_Pixel).

Examples in the mirror:
- Renderer/Private/DebugViewModeRendering.cpp
- Renderer/Private/SceneHitProxyRendering.cpp
- Renderer/Private/DistortionRendering.cpp

The custom VS and PS should each gate compilation to the intended custom VF type:
- Parameters.VertexFactoryType == FSoftwareRTDebugVertexFactory::StaticType (or the exact type accessor supported by the declaration).
- Add platform/material restrictions explicitly.
- Do not inherit assumptions from Base Pass or Debug View Mode shader classes.

Keep the first shaders minimal. The vertex shader reads the custom VF's declared vertex inputs and emits clip-space position plus the debug varying. The pixel shader writes that varying to the currently bound debug target. Do not include MaterialTemplate.ush or call CalcMaterialParametersEx in this smoke test.

## 3. Shader retrieval from the material shader map

The mirror's DebugViewMode mesh processor demonstrates the required retrieval sequence:

1. Resolve the effective FMaterial from MeshBatch.MaterialRenderProxy for FeatureLevel.
2. Obtain MeshBatch.VertexFactory->GetType().
3. Build an FMaterialShaderTypes collection and add the custom VS and PS shader types.
4. Call Material->ShouldCacheShaders(ShaderTypes, VertexFactoryType).
5. Call Material->TryGetShaders(ShaderTypes, VertexFactoryType, Shaders).
6. Extract the concrete shaders with TryGetVertexShader() and TryGetPixelShader() into TMeshProcessorShaders<CustomVS, FNoHull, FNoDomain, CustomPS> (or the exact valid UE4.27 template form).

If ShouldCacheShaders() is false, skip the batch. If TryGetShaders() is false, the required permutations are not currently available in that material's rendering-thread shader map. Do not dereference invalid TShaderRefs or assume that requesting a shader compiles it synchronously.

The test must arrange for the custom VF type to be registered before shader type initialization and for the chosen material's custom-VF shader permutations to exist. A valid material UniformExpressionCache is also required at draw time (Patch 44).

Use a known valid opaque engine material as the first material proxy only after confirming that the shader permutations were compiled for the custom VF. The custom shader source need not evaluate its Material Graph, but FMeshMaterialShader still uses the material shader-map and material UB infrastructure.

## 4. Processor structure

The processor should derive from FMeshPassProcessor and implement AddMeshBatch(). Its constructor should accept:
- const FScene* (may be null for the isolated test only if the selected material has no unresolved Parameter Collection dependencies);
- ERHIFeatureLevel::Type;
- const FSceneView* for the dynamic command path;
- FMeshPassDrawListContext*;
- the pass render state / view uniform buffer.

The base constructor is exactly:
FMeshPassProcessor(InScene, InFeatureLevel, InViewIfDynamicMeshCommand, InDrawListContext).

For this isolated test, call DrawDynamicMeshPass(View, RHICmdList, Lambda) from an enclosing pass with the debug target and viewport already bound. Inside the lambda:
- instantiate the custom processor using the supplied FDynamicPassMeshDrawListContext;
- submit the single custom FMeshBatch via AddMeshBatch();
- let DrawDynamicMeshPass() build, sort, and submit the one-frame commands.

The custom processor should:
- validate MeshBatch.VertexFactory and its type;
- resolve the effective material from the proxy;
- construct FMaterialShaderTypes for the custom VS/PS;
- retrieve the shader refs;
- calculate fill/cull state with FMeshPassProcessor helpers;
- create FMeshMaterialShaderElementData and call InitializeMeshMaterialData(ViewIfDynamicMeshCommand, nullptr, MeshBatch, -1, false);
- create FMeshPassProcessorRenderState from the active FSceneView (or explicitly set its view UB);
- set explicit blend and depth-stencil states;
- call BuildMeshDrawCommands() with PrimitiveSceneProxy=nullptr and EMeshPassFeatures::Default.

The exact render-target binding and viewport setup belong to the enclosing renderer/RDG pass, not DrawDynamicMeshPass().

## 5. Render state details

FMeshPassProcessorRenderState(const FSceneView& SceneView, FRHIUniformBuffer* InPassUniformBuffer = nullptr) initializes its ViewUniformBuffer from SceneView.ViewUniformBuffer.

FMeshMaterialShader::GetShaderBindings() unconditionally binds the view UB and also binds the fade/dither and instanced-view UBs. The instanced-view getter falls back to the regular view UB when no instanced-view UB is set.

For this test:
- use FMeshPassProcessorRenderState(*ViewIfDynamicMeshCommand) where a real view is available;
- set a valid opaque blend state and depth state appropriate to the capture target;
- do not declare a pass uniform buffer in the debug shaders;
- do not rely on default-constructed FMeshPassProcessorRenderState, because its view UB is null.

## 6. Primitive and draw command invariants

Carry forward Patch 44 exactly:
- BatchElement.PrimitiveUniformBufferResource = &GIdentityPrimitiveUniformBuffer;
- BatchElement.PrimitiveIdMode = PrimID_ForceZero when PrimitiveSceneProxy is null;
- no primitive-ID stream on the initial custom VF;
- valid index buffer and vertex range;
- no arbitrary VertexFactoryUserData.

FMeshMaterialShader::GetElementShaderBindings() invokes the custom VF type's registered element-binding callback when one exists, then enforces primitive UB binding for the no-primitive-stream path.

## 7. Remaining compile blockers before a real .patch

This audit does not yet prove:
- the custom VS input ABI and generated VertexFactory.ush compile together;
- the exact TMeshProcessorShaders template arguments for a minimal VS/PS-only processor in the target checkout;
- the shader map compile/cook lifecycle for a newly registered VF type;
- the RHI target format and PS output signature;
- the custom vertex/index buffer upload code;
- the exact lifetime of all RHI resources through the renderer's RHI-thread retirement path.

Do not write a source patch for the engine until those points are resolved against the user's actual checkout. The next narrow audit should inspect the shader type templates, shader map compile path, and existing minimal VS/PS-only mesh processor.

## 8. Source files audited

Pinned mirror paths:
- Engine/Source/Runtime/Renderer/Private/DebugViewModeRendering.cpp
- Engine/Source/Runtime/Renderer/Private/SceneHitProxyRendering.cpp
- Engine/Source/Runtime/Renderer/Private/ShaderBaseClasses.cpp
- Engine/Source/Runtime/Renderer/Public/MeshMaterialShader.h
- Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h
- Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl
- Engine/Source/Runtime/Engine/Public/MaterialShared.h
- Engine/Source/Runtime/Engine/Public/MeshBatch.h

Patch 45 extends Patches 43–44. It is an audit/design document, not an executable C++ patch.
