# 25 — Dynamic mesh pass lifetime and one-shot chart capture decision

Status: source-path audit and implementation gate. Not applied to an Unreal Engine checkout and not compiled.

Mirror audited: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## 1. UE4.27 has a synchronous one-shot dynamic mesh pass helper

The template `DrawDynamicMeshPass(...)` is implemented in:

`Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`

Its actual sequence is:

1. Construct local `FDynamicMeshDrawCommandStorage`.
2. Construct local `FMeshCommandOneFrameArray`.
3. Construct local `FGraphicsMinimalPipelineStateSet` and shader-initialization flag.
4. Construct `FDynamicPassMeshDrawListContext` referencing those objects.
5. Invoke the supplied lambda synchronously with that context.
6. Call `DrawDynamicMeshPassPrivate` before the helper returns.

The private function applies view overrides, sorts/merges the one-frame commands, and calls `SubmitMeshDrawCommandsRange`. Submission iterates the visible commands and calls `FMeshDrawCommand::SubmitDraw`.

**This resolves the central command-storage lifetime question:** a capture callback can build its chart draw commands inside the lambda; the helper's local command storage remains alive through command submission. We do not need to invent a persistent command-list owner for a one-shot capture draw.

The lambda must not retain pointers/references to its stack-local batches, processor, context or local capture payload after it returns.

## 2. The dynamic context copies the finalized command into one-frame storage

In `Renderer/Public/MeshPassProcessor.h`:
- `FDynamicMeshDrawCommandStorage::MeshDrawCommands` is a `TChunkedArray<FMeshDrawCommand>`, explicitly chosen so growth does not move commands referenced by `FVisibleMeshDrawCommand`.
- `FDynamicPassMeshDrawListContext::AddCommand` adds the initializer to that storage.
- `FinalizeCommand` calls `MeshDrawCommand.SetDrawParametersAndFinalize(...)`, then stores a visible-command pointer to the finalized command.

The visible list is not the owner of the draw command; `FDynamicMeshDrawCommandStorage` is. Both live until the synchronous helper finishes submission.

This proves the lifetime of the command container and its visible pointers. It does **not** by itself prove that arbitrary raw pointers inserted into custom shader payloads or VF user data are copied or retained safely. Chart resources must be bound as RHI references/uniform buffers with normal renderer lifetime rules, not as pointers to temporary CPU structs.

## 3. Source proxy lifetime is required during command construction

The Base Pass processor reads the `PrimitiveSceneProxy` while building shader element data, primitive IDs and draw commands. The capture lambda must execute while the source proxy and its scene primitive data are valid on the render thread.

After command construction, the dynamic command stores the resolved draw bindings and primitive IDs used by submission. Do not retain a proxy pointer in custom chart shader data.

For the initial implementation:
- obtain the source section batch using `FStaticMeshSceneProxy::GetMeshElement`;
- call `DrawDynamicMeshPass(View, RHICmdList, lambda)`;
- inside the lambda, construct the capture Base Pass processor using the provided dynamic context;
- call `AddMeshBatch` with the cloned chart batch and the original source `FPrimitiveSceneProxy*`;
- submit before returning from the helper.

This keeps source primitive identity and avoids registering chart batches in the scene's cached static mesh draw lists.

## 4. Important performance caveat

The helper's own source comment says it performs many dynamic allocations and should not be used for game rendering. This is a warning against using it as the permanent per-frame rendering path.

For a first correctness prototype, it is acceptable as a one-shot/debug capture route if capture frequency and cost are explicitly limited. It is not approved as the final high-frequency Surface Cache update path.

If profiling later shows excessive allocation/CPU cost, the follow-up is a dedicated persistent capture command context/storage with explicit frame lifetime—not reverting to cached scene static-mesh commands or retaining temporary pointers.

## 5. Current implementation gate

The command-container lifetime is now sufficiently understood to prototype a one-shot capture pass. Remaining hard gates before writing the processor:
- audit the full `SetDrawParametersAndFinalize` path for which mesh batch fields are copied into `FMeshDrawCommand` and which RHI resources are referenced;
- confirm the Base Pass's primitive uniform binding under both GPU Scene and non-GPU Scene;
- define chart VF per-draw transform binding using a typed uniform buffer/resource with a lifetime extending through submission;
- prove the chart VF wrapper compiles and the hook maps translated world position to chart clip space correctly;
- verify chart tile viewport/scissor and one-triangle index range.

Do not claim the capture pass works until those checks and an engine build/draw test succeed.

## Source paths checked

- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`
- `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp`
