# 23 — Static-mesh section extraction, batch payload and capture ownership audit

Status: source-path audit and implementation gate. Not applied to an Unreal Engine checkout and not compiled.

Mirror audited: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## 1. The source proxy already exposes section-batch construction

In `Engine/Source/Runtime/Engine/Public/StaticMeshResources.h`, `FStaticMeshSceneProxy::GetMeshElement(...)` is a public virtual method. It accepts LOD index, batch index, section/element index, depth-priority group, selection-outline flag, pre-culled-index flag and an output `FMeshBatch`.

The implementation in `Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp`:
- selects the material from `LODs[LODIndex].Sections[SectionIndex]`;
- selects the source LOD Local VF, or the override-color VF;
- fills the source element's geometry/index information through `SetMeshElementGeometrySource`;
- assigns the material proxy, LCI, section index, culling and vertex bounds.

This is the correct UE4.27 entry point for obtaining the exact material section batch. Do not reconstruct section index ranges independently from render data.

Proposed extraction sequence on the render thread:
1. Keep a valid source `FStaticMeshSceneProxy*` and call its public `GetMeshElement` for each supported LOD/section.
2. Copy the resulting `FMeshBatch` into capture-owned storage.
3. Clone only the selected `FMeshBatchElement` into the capture batch; preserve the returned geometry/index data.
4. Replace the cloned batch's VF and the cloned element's VF uniform-buffer payload only after the chart VF is initialized and ready.
5. Keep the source proxy, LOD resources, source VF, capture VF and all referenced buffers alive until the capture draw commands have completed.

The source proxy's `GetMeshDescription` also builds batches by calling `GetMeshElement`, but its output is an array of ordinary source batches; it does not solve capture scheduling or lifetime.

## 2. `FMeshBatchElement::UserData` is not a free chart-payload slot

The audited `FMeshBatchElement` declaration in `Engine/Source/Runtime/Engine/Public/MeshBatch.h` distinguishes:
- `VertexFactoryUserData`: VF-defined data;
- `UserData`: separate, VF/path-defined data;
- `bUserDataIsColorVertexBuffer`: an explicit interpretation flag.

The standard Static Mesh path sets `UserData` only for component-overridden vertex colors:
- `UserData = ProxyLODInfo.OverrideColorVertexBuffer`;
- `bUserDataIsColorVertexBuffer = true`;
- `VertexFactoryUserData` remains the override VF's uniform buffer.

In `LocalVertexFactory.cpp`, `FLocalVertexFactoryShaderParameters::GetElementShaderBindings` decodes `VertexFactoryUserData` as an `FRHIUniformBuffer*`. Separately, when `bUserDataIsColorVertexBuffer` is true, it casts `UserData` to `FColorVertexBuffer*` and binds the override color stream.

**Consequence:** do not store chart parameters in `UserData` on a source batch. For the first implementation, leave `UserData` and `bUserDataIsColorVertexBuffer` unchanged. Put chart-specific parameters in an explicit chart-VF parameter binding path, with owned resources and a documented lifetime. If the chart VF needs per-draw payload beyond its VF uniform buffer, add a dedicated typed mechanism; do not overload either legacy field.

## 3. The source VF and override-color path are distinct cases

A section may use `FStaticMeshVertexFactories::VertexFactory` or `VertexFactoryOverrideColorVertexBuffer`. A chart capture cannot blindly copy the source VF's `VertexFactoryUserData` and assume it belongs to the chart VF.

For the baseline, choose one of these policies explicitly:
- **Recommended first proof:** reject/skip sections with `OverrideColorVertexBuffer`, and test standard Local VF sections only.
- Later support: copy the effective override-color VF data and its color-buffer stream deliberately, preserving the override color buffer lifetime and the `bUserDataIsColorVertexBuffer` contract.

Do not silently render component-overridden vertex colors incorrectly.

## 4. What this audit resolves — and what it does not

Resolved from the mirror:
- public source-proxy API to produce exact section batches;
- exact ownership meaning of `UserData` and `VertexFactoryUserData` in the standard Local VF path;
- why source element user data must not be reused as chart parameters;
- a safe first-test exclusion for overridden vertex colors.

Not resolved:
- the complete `FBasePassMeshProcessor` primitive/view uniform-buffer behavior for a capture-owned pass;
- whether the intended capture pass can preserve the source primitive data while using the capture chart transform;
- exact lifetime integration with the renderer's mesh-pass setup tasks and draw-command execution;
- whether disabling mesh draw command caching in the chart VF type is sufficient for all capture submissions;
- shader-wrapper compile proof and a real chart raster draw.

## 5. Implementation decision

Do **not** write the capture mesh processor yet.

The next audit must follow `FBasePassMeshProcessor::Process` through the primitive uniform-buffer selection and the mesh-pass setup path, including the `FPrimitiveSceneProxy` argument and the GPU Scene / non-GPU Scene branches. It must also identify the narrowest existing capture-owned mesh pass pattern that retains a source proxy and its primitive data without registering the chart draw as a normal scene primitive.

Only after that can the capture processor be specified without guessing at primitive bindings or command lifetimes.

## Source paths checked

- `Engine/Source/Runtime/Engine/Public/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp`
- `Engine/Source/Runtime/Engine/Public/MeshBatch.h`
- `Engine/Source/Runtime/Engine/Private/LocalVertexFactory.cpp`
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp`
