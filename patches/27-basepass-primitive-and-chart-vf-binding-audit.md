# 27 — Base Pass primitive and chart-VF binding audit

Status: source audit and binding contract. No engine source was changed; no compile or GPU test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## 1. Base Pass delegates element binding to the material shader path

The Base Pass vertex shader policy's `GetElementShaderBindings` in `BasePassRendering.inl` delegates to `FMeshMaterialShader::GetElementShaderBindings`.

That function in `ShaderBaseClasses.cpp` performs two distinct jobs:
1. asks the registered Vertex Factory type's shader-parameter implementation to bind VF resources and vertex streams;
2. binds primitive data according to GPU Scene and the VF's primitive-ID stream support.

This is the correct extension point for chart-VF data. Do not replace the Base Pass primitive binding path.

## 2. Chart VF uniform buffer binding

The existing Local VF shader-parameter implementation decodes `BatchElement.VertexFactoryUserData` as an `FRHIUniformBuffer*`, then calls `FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase`.

That base helper:
- binds `FLocalVertexFactoryUniformShaderParameters` when manual vertex fetch or GPU Scene requires it;
- uses the explicit VF uniform buffer when supplied, otherwise the VF's own uniform buffer;
- binds the override color stream only when `bUserDataIsColorVertexBuffer` is set.

Consequences:
- preserve `VertexFactoryUserData` as the chart VF's uniform buffer pointer;
- never put chart mapping data in `UserData` or overwrite the override-color convention;
- if the chart VF needs extra chart parameters, its custom `FVertexFactoryShaderParameters::GetElementShaderBindings` must add a separately typed chart uniform buffer parameter;
- keep the Local VF base helper call for ordinary Local VF bindings and vertex streams.

The exact typed parameter declaration/binding code still needs to be written and compiled against the engine headers. This audit does not invent a type name or assume the shader wrapper already compiles.

## 3. Primitive binding: GPU Scene path

In `FMeshPassProcessor::GetDrawCommandPrimitiveId`:
- when GPU Scene is active and `PrimitiveIdMode == PrimID_FromPrimitiveSceneInfo`, a valid `FPrimitiveSceneInfo` is required and its index becomes the draw primitive ID;
- `PrimitiveUniformBufferResource` is expected to be null for this mode;
- the scene primitive ID is also taken from the source scene info.

In `FMeshMaterialShader::GetElementShaderBindings`, when GPU Scene is active and the VF exposes a primitive-ID stream, the normal primitive uniform buffer is not bound. The shader obtains primitive data through the primitive ID / Scene data path. The code explicitly warns that an assigned `PrimitiveUniformBuffer` cannot be respected in this mode.

Decision: keep the source `FPrimitiveSceneProxy` and its `FPrimitiveSceneInfo` through mesh processing, preserve the source element's primitive ID mode, and do not attach a separate primitive UB to this GPU Scene path.

## 4. Primitive binding: non-GPU Scene path

When the GPU Scene primitive-ID condition is false, `FMeshMaterialShader::GetElementShaderBindings` binds:
- `BatchElement.PrimitiveUniformBuffer`, if supplied; otherwise
- `BatchElement.PrimitiveUniformBufferResource->GetUniformBufferRHI()`.

It asserts that one of these sources exists.

Decision: clone the source mesh batch element without dropping its primitive uniform buffer fields. Keep the original primitive proxy available to the processor. Do not fabricate a primitive UB or force `PrimID_ForceZero` as a shortcut.

## 5. Shader binding memory versus RHI resource ownership

`FMeshDrawShaderBindings::Initialize` allocates storage for all shader parameter layouts. Parameter writes populate this command-owned byte storage. `CopyFrom` copies the bytes (inline or heap) into another binding object.

But uniform buffers, SRVs and samplers in those bytes are raw RHI resource pointers. The binding object does not turn them into owning smart references. The optional `VALIDATE_UNIFORM_BUFFER_LIFETIME` instrumentation tracks mesh-command references for debugging; it is not ownership.

Therefore the capture owner must retain the chart uniform buffer, source VF uniform buffer where needed, capture SRVs/textures and samplers for the complete command/GPU-use lifetime.

## 6. Approved first-prototype binding contract

For an opaque deferred Static Mesh section:
1. Obtain the original section batch using `FStaticMeshSceneProxy::GetMeshElement`.
2. Clone the batch/element and preserve source primitive fields, section/material, index ranges, culling and vertex streams.
3. Replace only the VF with the chart VF and set `VertexFactoryUserData` to the chart VF uniform buffer expected by the Local VF binding base.
4. Add chart mapping through a separate typed chart uniform buffer in the chart VF shader-parameter implementation.
5. Call the Base Pass mesh processor with the original source primitive proxy; do not null it.
6. Preserve the original primitive ID mode and primitive uniform buffer/resource fields. GPU Scene and non-GPU Scene must both pass their respective binding path.
7. Keep all chart RHI resources alive beyond CPU command construction and until safe GPU retirement.

## 7. Remaining gates before capture implementation

- Implement and compile the typed chart uniform-buffer parameter declaration and binding.
- Verify chart VF shader wrapper and Base Pass shader permutations compile.
- Verify `GetMeshElement` source elements actually provide valid primitive data for the selected dynamic pass on both GPU Scene configurations.
- Inspect the exact `FMeshDrawCommand::Finalize` path and command storage ownership in the checked engine revision.
- Then implement a one-shot opaque/deferred test with a single known triangle and verify viewport/scissor, chart transform, and index range.

Do not extend this first test to masked, translucent, WPO/PDO, tessellated, GPU-skinned, or override-color sections until the baseline works.

## Source paths

- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.inl`
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.h`
- `Engine/Source/Runtime/Renderer/Private/ShaderBaseClasses.cpp`
- `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`
- `Engine/Source/Runtime/Engine/Private/LocalVertexFactory.cpp`
- `Engine/Source/Runtime/Engine/Public/LocalVertexFactory.h`
