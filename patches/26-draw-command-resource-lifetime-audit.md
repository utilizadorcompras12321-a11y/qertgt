# 26 — Draw-command finalization: copied geometry versus referenced RHI resources

Status: source-path audit. Not applied to an Unreal Engine checkout and not compiled.

Mirror audited: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## 1. What `SetDrawParametersAndFinalize` actually copies

In `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`, `FMeshDrawCommand::SetDrawParametersAndFinalize` reads the selected `FMeshBatchElement` and copies the draw's scalar geometry parameters:
- index-buffer RHI pointer (or null);
- first index;
- primitive count;
- instance count;
- base vertex index;
- vertex count derived from min/max vertex index;
- indirect-argument buffer pointer and offset, when applicable.

It then calls `Finalize`.

It does **not** copy the `FMeshBatch`, `FMeshBatchElement`, source VF, CPU vertex buffers, source proxy or arbitrary user-data pointers into a self-contained owner.

This means the temporary source batch can go out of scope after command construction only if every value needed later has already been converted into command-owned data or into a resource whose lifetime is independently guaranteed.

## 2. Shader bindings are copied into command storage, but RHI resource pointers are not ownership

The mesh processor builds shader bindings before finalization. `FMeshDrawShaderBindings` stores the bound parameter data in the draw command's binding storage; the dynamic context stores the resulting `FMeshDrawCommand` in its one-frame `TChunkedArray`.

However, uniform-buffer, SRV and sampler bindings in that parameter storage are RHI resource pointers. Copying the binding bytes does not mean the draw command becomes the owner of the referenced resource.

Therefore:
- chart VF uniform buffers must be persistent RHI resources whose owner outlives submission;
- chart textures/SRVs and samplers must likewise be held alive through submission;
- do not bind pointers to stack structs, transient CPU allocations, or RHI resources released before `DrawDynamicMeshPass` returns;
- use UE4.27's normal uniform-buffer/resource reference and render-command lifetime mechanisms.

## 3. Vertex streams and index buffers are referenced by the draw command

The draw command's vertex streams are populated during shader element binding. `SubmitDraw` later iterates those streams and binds their RHI vertex buffers. The index buffer is stored as a raw RHI pointer during finalization.

The underlying Static Mesh LOD buffers are owned by the render resources, not by the temporary `FMeshBatch`. The source proxy and render resources must remain valid while the command is built; the RHI resources themselves must remain alive until the queued/submitted GPU work can no longer reference them, using the engine's existing resource lifetime rules.

For chart VF, the copied `FLocalVertexFactory::FDataType` must continue to reference valid source vertex buffers. The chart VF resource and its uniform buffer must also remain alive through the draw. A copied CPU `FDataType` is not a deep copy of the GPU buffers.

## 4. Do not use per-triangle CPU batch lifetime as a substitute for GPU resource lifetime

The safe one-shot pattern is:
- build the cloned batch and selected element inside the `DrawDynamicMeshPass` callback;
- build commands synchronously using the source proxy and chart VF;
- ensure the chart VF, its uniform buffer, and all referenced RHI resources are owned by a capture object that survives command submission;
- let the helper's local draw-command storage live through its internal submission;
- release or recycle capture resources only through the renderer's correct RHI/GPU lifetime mechanism, not immediately because the CPU helper returned.

The helper's synchronous CPU submission does not imply the GPU has finished executing the draw when the function returns.

## 5. Primitive bindings remain a separate mandatory path

The primitive ID is resolved during command construction. With GPU Scene and `PrimID_FromPrimitiveSceneInfo`, a valid `FPrimitiveSceneInfo` is required. Preserve the original source proxy/scene primitive during processor execution.

Without GPU Scene, the primitive uniform buffer is bound through the mesh material shader element binding path. This path must be checked for the exact capture Base Pass processor constructor and selected shader permutation; do not assume the GPU Scene result covers it.

## 6. Decision and remaining gate

The dynamic helper solves the lifetime of its **CPU command containers**, not ownership of chart GPU resources. The correct prototype owner is therefore a persistent render-thread capture object holding:
- chart VF;
- chart VF uniform buffer;
- chart mapping uniform buffer or equivalent typed per-draw resource;
- any capture textures/SRVs/samplers;
- references/ownership guarantees for the source LOD resources while the source batch is constructed and submitted.

Before implementing the capture processor, finish two focused checks:
1. Trace the actual Base Pass shader binding path for both GPU Scene and non-GPU Scene, confirming primitive UB and chart VF UB binding.
2. Trace `FMeshDrawCommand::Finalize` and `FMeshDrawShaderBindings` allocation/copy behavior to confirm no shader binding payload points into temporary memory.

Then the one-shot opaque/deferred prototype can be specified. A real build and GPU validation remain mandatory.

## Source paths checked

- `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp`
- `Engine/Source/Runtime/Engine/Public/LocalVertexFactory.h`
- `Engine/Source/Runtime/Engine/Private/LocalVertexFactory.cpp`
