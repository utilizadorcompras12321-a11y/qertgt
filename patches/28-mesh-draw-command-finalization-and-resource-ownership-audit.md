# 28 — Mesh Draw Command finalization and resource ownership audit

Status: source audit and lifetime contract. No engine source was changed; no compile or GPU test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## 1. The engine states the ownership rule explicitly

The class comment for `FMeshDrawCommand` in `Renderer/Public/MeshPassProcessor.h` states:

- a draw command contains only data needed to draw;
- cached commands are stored by the renderer;
- any resources referenced by a command must be kept alive for the command lifetime;
- `FMeshDrawCommand` is not responsible for resource lifetime management.

This is the governing rule for the chart capture path. A command container is not a resource owner.

## 2. What Finalize actually does

The inline `FMeshDrawCommand::Finalize` implementation only:
1. assigns `CachedPipelineId`;
2. calls `ShaderBindings.Finalize(ShadersForDebugging)`.

It does not copy or retain the vertex factory, vertex buffers, index buffer, uniform buffers, textures, SRVs, or samplers. It does not transfer ownership from the source batch or capture owner.

`FMeshDrawCommand::SetDrawParametersAndFinalize` copies scalar draw parameters and stores raw RHI pointers:
- `IndexBufferRHI.GetReference()`;
- indirect argument buffer pointer and offset, when used;
- first index, primitive count, instance count, base vertex and vertex count.

The vertex streams are separately stored as stream entries containing RHI vertex-buffer pointers. These are references, not owning references.

## 3. Shader binding storage: copied bytes, not owned resources

`FMeshDrawShaderBindings::Initialize` builds shader layouts and allocates zeroed byte storage sized for the shader parameter layouts.

The element-binding code writes uniform-buffer, sampler, texture/SRV pointers and loose parameter bytes into that storage.

`CopyFrom`:
- copies the shader layouts;
- allocates command-owned storage;
- copies the binding bytes (inline or heap).

This gives the destination command its own storage for the binding values. It does **not** acquire ownership of the RHI resources whose addresses are present in those bytes.

`SetOnCommandList` later reads those stored pointers and submits them to RHI binding calls. The pointers must still identify valid resources at that point.

The optional `VALIDATE_UNIFORM_BUFFER_LIFETIME` instrumentation increments/decrements a debugging counter for uniform buffers. It is diagnostic instrumentation, not a reference-counting or ownership mechanism. It does not cover every resource category.

## 4. Dynamic instancing makes pointer identity significant

`MatchesForDynamicInstancing` and `GetDynamicInstancingHash` compare/hash shader binding values, vertex stream buffers, index buffers and draw parameters.

Consequences:
- chart-specific uniform-buffer bindings participate in command equivalence;
- chart resources must not be recycled or mutated while commands can still refer to them;
- per-chart bindings naturally prevent merging when their resource pointers or loose parameter bytes differ;
- this is a correctness constraint, not a reason to encode chart identity through temporary pointers.

The first prototype should prefer correctness over dynamic-instancing efficiency.

## 5. Ownership contract for the capture implementation

The capture subsystem must own, or retain strong references to, every resource referenced by its commands:

- chart vertex-factory object and its uniform buffer;
- chart mapping uniform buffer;
- vertex/index buffers used by the cloned batch;
- capture render targets/textures and any SRVs;
- samplers used by capture shaders;
- any other RHI resource stored in shader binding bytes or vertex streams.

The owner must outlive both:
1. CPU command construction/submission;
2. the GPU's use of the submitted commands and resources.

Do not release resources merely because `DrawDynamicMeshPass` or the CPU-side capture callback returned. Use the renderer/RHI's established deferred-release or fence-safe retirement mechanism for the exact resource type and lifetime. The correct retirement mechanism still needs to be selected and verified in the implementation.

## 6. What the one-shot dynamic pass does and does not solve

`FDynamicMeshDrawCommandStorage` uses `TChunkedArray<FMeshDrawCommand>` so command addresses remain stable while the array grows. Its lifetime covers the dynamic pass's command-building and draw execution scope.

That solves command-object storage stability for the one-shot pass. It does not:
- retain RHI resources;
- extend the lifetime of the chart VF or its owner;
- make a temporary uniform buffer safe;
- guarantee GPU completion when the CPU helper returns.

Therefore the dynamic pass is acceptable as a controlled first correctness test only if a capture owner retains all referenced resources beyond command submission and retires them safely.

## 7. Revised implementation gate

Before implementing the capture processor, specify a concrete owner object and its retirement path. The design must identify, for each resource above:
- who creates it;
- who holds the strong reference;
- which render-thread command owns/updates it;
- when it becomes safe to release it.

Do not proceed with raw pointers to stack objects, temporary arrays, or one-frame allocations whose lifetime ends before GPU use is complete.

The typed chart uniform-buffer binding and shader-wrapper compile proof remain separate gates. This audit does not claim either has been implemented or compiled.

## Source paths

- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`
- `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl`
