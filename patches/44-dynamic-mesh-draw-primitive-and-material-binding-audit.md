# Patch 44 — Dynamic Mesh Draw: Primitive and Material Binding Requirements

**Target:** UE 4.27.2  
**Branch:** plugin/software-rt-material-evaluation  
**Mirror:** AlexMercer-MA/UnrealEngine-4.27  
**Pinned mirror commit:** 306a7e9f7fb53f14c2cb3bfd3854ad906710a17e  
**Status:** source audit and correction to Patch 43. Not applied to the user's Engine checkout; not compiled.

## 1. Important correction to Patch 43

A standalone capture VF and a custom FMeshMaterialShader can be drawn without a real FPrimitiveSceneProxy for the one-triangle debug test, but only if the mesh batch supplies valid primitive uniform data and the batch element uses the correct primitive-ID mode.

The simple route is the engine's existing identity primitive uniform buffer:

- BatchElement.PrimitiveUniformBufferResource = &GIdentityPrimitiveUniformBuffer
- BatchElement.PrimitiveIdMode = PrimID_ForceZero

This is not a fabricated scene primitive. It is an explicit identity transform / identity primitive-data binding already used by UE4.27's TriangleRendering.cpp and TileRendering.cpp.

Do not leave the default PrimitiveIdMode (PrimID_FromPrimitiveSceneInfo) when passing a null PrimitiveSceneProxy on a GPU Scene platform. FMeshPassProcessor::GetDrawCommandPrimitiveId() checks that PrimitiveSceneInfo is valid for PrimID_FromPrimitiveSceneInfo.

## 2. Verified primitive binding behavior

### 2.1 FMeshMaterialShader::GetElementShaderBindings

In Renderer/Private/ShaderBaseClasses.cpp, FMeshMaterialShader::GetElementShaderBindings() behaves as follows:

1. It asks the VF type for its registered per-element shader parameter bindings, if present.
2. If GPU Scene is enabled and the VF has a valid primitive-ID stream, it expects primitive data to be fetched through that stream / Scene data and disallows a direct PrimitiveUniformBuffer binding.
3. Otherwise it requires either:
   - BatchElement.PrimitiveUniformBuffer, or
   - BatchElement.PrimitiveUniformBufferResource.

If neither exists, it executes a checkf. This requirement applies even if the custom debug shader does not read primitive members, because the base binding routine performs the check.

For the initial custom VF, keep bSupportsPrimitiveIdStream=false and do not provide a primitive-ID stream. Bind GIdentityPrimitiveUniformBuffer through PrimitiveUniformBufferResource.

### 2.2 Primitive ID mode

FMeshBatchElement defaults PrimitiveIdMode to PrimID_FromPrimitiveSceneInfo. FMeshPassProcessor::GetDrawCommandPrimitiveId() requires a non-null FPrimitiveSceneInfo for this mode when GPU Scene is active.

For the null-proxy one-triangle test, explicitly set PrimID_ForceZero. This avoids claiming a scene primitive exists. Do not set PrimID_DynamicPrimitiveShaderData unless the matching dynamic primitive shader data has actually been allocated and populated.

### 2.3 Existing engine precedent

Engine/Source/Runtime/Engine/Private/TriangleRendering.cpp sets:
- IndexBuffer, FirstIndex, NumPrimitives, MinVertexIndex, MaxVertexIndex;
- PrimitiveUniformBufferResource = &GIdentityPrimitiveUniformBuffer.

Engine/Source/Runtime/Engine/Private/TileRendering.cpp uses the same identity primitive uniform buffer pattern.

These are strong precedents for a standalone triangle batch, although their VertexFactory is FLocalVertexFactory and their draw submission path is not identical to the proposed capture processor.

## 3. Material and view bindings are still required

FMeshMaterialShader::GetShaderBindings() calls FMaterialShader::GetShaderBindings(), which:
- requires a valid rendering-thread material shader map;
- requires a valid MaterialRenderProxy UniformExpressionCache for the selected feature level;
- binds the Material uniform buffer;
- binds any referenced Material Parameter Collection uniform buffers.

FMeshMaterialShader then binds:
- view uniform buffer;
- fade uniform buffer;
- dither uniform buffer;
- instanced-view uniform buffer;
- pass uniform buffer if the shader declares one.

Therefore:
- use a valid material proxy/material pair whose uniform expression cache is current;
- set the view uniform buffer on FMeshPassProcessorRenderState;
- if the debug shader declares a pass UB, set it explicitly; preferably declare no pass UB for this smoke test;
- do not use a null/uninitialized material proxy just because the custom pixel shader does not sample material parameters;
- if the material references Parameter Collections, a valid Scene or default collection instance must resolve them.

The debug shader can avoid evaluating Material Graph inputs in its own shader source, but FMeshMaterialShader still uses the normal material shader-map and material uniform-buffer binding infrastructure.

## 4. FMeshMaterialShaderElementData can use a null primitive proxy

FMeshMaterialShaderElementData::InitializeMeshMaterialData() initializes fade/dither uniform buffers and only dereferences PrimitiveSceneProxy inside a guarded branch. A null proxy is therefore valid for this test when StaticMeshId is -1 and the explicit identity primitive UB is supplied on the batch element.

Use:
- PrimitiveSceneProxy = nullptr;
- StaticMeshId = -1;
- bAllowStencilDither = false.

## 5. Dynamic draw path contract

The pinned mirror's MeshPassProcessor.inl implements DrawDynamicMeshPass():

1. Creates FDynamicMeshDrawCommandStorage.
2. Creates FMeshCommandOneFrameArray.
3. Creates FGraphicsMinimalPipelineStateSet.
4. Creates FDynamicPassMeshDrawListContext.
5. Calls the supplied build lambda.
6. Calls DrawDynamicMeshPassPrivate() immediately.

DrawDynamicMeshPassPrivate() applies view overrides, sorts/merges commands, and calls SubmitMeshDrawCommandsRange() before returning.

A minimal processor can therefore be instantiated inside the build lambda, provided all objects referenced by the processor and built commands remain valid through command construction and submission. Do not store references to lambda-local objects beyond the call.

Important: the dynamic helper's command storage lifetime does not prove that the VF's RHI resources can be destroyed immediately after it returns. Keep the capture owner alive and retire it using the already-established RHI-thread fence / resource-release sequence from Patches 29–34.

## 6. Minimal debug batch recipe

For the first triangle:
- MeshBatch.VertexFactory = custom initialized VF;
- MeshBatch.MaterialRenderProxy = valid proxy for a material whose shader map contains the custom mesh-material shader for this VF type;
- MeshBatch.Type = PT_TriangleList;
- one FMeshBatchElement;
- IndexBuffer = persistent index buffer containing {0, 1, 2};
- FirstIndex = 0;
- NumPrimitives = 1;
- NumInstances = 1;
- MinVertexIndex = 0;
- MaxVertexIndex = 2;
- PrimitiveUniformBufferResource = &GIdentityPrimitiveUniformBuffer;
- PrimitiveIdMode = PrimID_ForceZero;
- no arbitrary VertexFactoryUserData;
- no primitive-ID stream.

The processor must:
- derive from FMeshPassProcessor;
- call the base constructor with the correct Scene (null is acceptable only if the material has no unresolved collection dependencies), FeatureLevel, ViewIfDynamicMeshCommand, and draw-list context;
- retrieve the custom material shaders using the custom VF type;
- initialize FMeshMaterialShaderElementData with null proxy and StaticMeshId -1;
- set the view UB in FMeshPassProcessorRenderState;
- set explicit blend/depth states and target the debug render target;
- call BuildMeshDrawCommands() with the batch and the identity primitive data.

The test should run inside an existing renderer pass / RDG raster pass where the target is already bound. Do not assume DrawDynamicMeshPass() itself sets render targets or viewport.

## 7. New stop conditions

Do not call the smoke test complete until:
- the material shader map returns both custom shaders for the custom VF type;
- material UniformExpressionCache is valid for the active feature level;
- view UB is bound;
- identity primitive UB is bound;
- PrimitiveIdMode is PrimID_ForceZero when PrimitiveSceneProxy is null;
- the render target and viewport are explicitly set by the enclosing pass;
- the draw reaches the pixel shader and the debug attribute interpolates;
- VF and buffers survive the RHI retirement fence before ReleaseResource.

## 8. Source files audited

Pinned mirror paths:
- Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl
- Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h
- Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp
- Engine/Source/Runtime/Renderer/Private/ShaderBaseClasses.cpp
- Engine/Source/Runtime/Engine/Public/MeshBatch.h
- Engine/Source/Runtime/Engine/Private/TriangleRendering.cpp
- Engine/Source/Runtime/Engine/Private/TileRendering.cpp

This patch refines Patch 43. It does not establish that the user's local checkout exactly matches the pinned mirror.
