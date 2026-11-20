# 20 — Chart VF registration and draw-binding audit (UE4.27)

Status: source-audited implementation contract. No engine source changed or compiled.

## Decision

The raster-position hook is real and correctly ordered, but Patch 19 was too optimistic about how to bind the per-draw transform. The safe next step is **not** to write the full VF immediately. First establish a distinct VF type, distinct shader source wrapper, and explicit parameter-binding class. Do not put a raw transform pointer in `FMeshBatchElement::UserData` or repurpose `VertexFactoryUserData`.

## Verified engine facts

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

### 1. VF types select their own shader source

`FVertexFactoryType::ModifyCompilationEnvironment` installs the VF's shader filename as `/Engine/Generated/VertexFactory.ush`. The VF type registration macro stores:
- the concrete VF class name;
- its shader source filename;
- the class-specific parameter construction/layout/binding callbacks;
- class-specific compile-environment and validation callbacks.

Therefore a chart VF can have its own shader wrapper and registration. It does not require changing the normal `FLocalVertexFactory` registration.

### 2. Shader parameter dispatch is static/type-layout based

`FVertexFactoryShaderParameters` uses non-virtual type-layout dispatch. `IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE` registers a concrete parameter class for a concrete VF class/frequency. A chart VF must register its own parameter class; inheriting the stock parameter class without registering the chart type is insufficient.

The stock `FLocalVertexFactoryShaderParameters::GetElementShaderBindings` explicitly decodes `BatchElement.VertexFactoryUserData` as `FRHIUniformBuffer*`. Do not place chart data there unless it is genuinely an FRHI uniform buffer with the expected local-VF uniform layout. The stock implementation also delegates common local-VF bindings to `FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase`.

The base class is public and has a non-virtual layout. A chart-specific parameter class can be designed to:
1. bind a chart-transform shader parameter;
2. call `GetElementShaderBindingsBase` to retain normal local-VF bindings;
3. add the chart uniform binding from an explicitly owned draw-data resource.

The exact storage/lifetime mechanism still needs a concrete implementation and cache-policy proof.

### 3. Shader hook ordering

`BasePassVertexShader.usf`:
- obtains normal world position;
- builds material vertex parameters;
- applies WPO;
- calls `VertexFactoryGetRasterizedWorldPosition`;
- uses the returned position for clip-space output;
- builds material interpolants from the original `VertexParameters`.

`LocalVertexFactory.ush` default hook returns its input unchanged. This confirms the chart override can change raster position while retaining the existing material-facing attributes.

### 4. A wrapper is the least invasive shader route

The registered chart VF shader source should be a small wrapper that defines a chart-only compile symbol and includes the existing local VF shader implementation. The chart-only branch should override only `VertexFactoryGetRasterizedWorldPosition`; all other local VF functions and material-facing attribute decoding remain the same.

Do not globally change `LocalVertexFactory.ush` behavior for the ordinary local VF.

This wrapper approach must be verified against UE4.27's shader preprocessor/include rules during the first shader compile. If include ordering or duplicate function definitions prevent it, use a narrowly factored shared include rather than copying the entire local VF shader.

## Proposed C++ shape (contract, not copy-paste code)

- `FSoftwareRTChartVertexFactory : public FLocalVertexFactory`
  - own `DECLARE_VERTEX_FACTORY_TYPE`;
  - own static type registration using `IMPLEMENT_VERTEX_FACTORY_TYPE_EX`;
  - own compile-environment callback, calling the local VF implementation and defining the chart permutation;
  - initially preserve the inherited local vertex streams and declaration; no extra vertex attribute yet.
- `FSoftwareRTChartVertexFactoryShaderParameters`
  - own `DECLARE_TYPE_LAYOUT(..., NonVirtual)`;
  - own `Bind` for the chart transform parameter;
  - own `GetElementShaderBindings`;
  - call local VF base binding helper for the standard local-VF uniform buffer and streams;
  - bind the chart transform from a draw-data object whose lifetime covers mesh-command execution and whose identity participates correctly in mesh draw command caching.
- Register the chart parameter class for every shader frequency actually used by the chosen Base Pass permutation. Start with vertex frequency only if the chart uniform is consumed exclusively in the VS; do not register irrelevant frequencies speculatively.
- The chart shader wrapper must compile only for the chart VF type. Normal local VF shader permutations remain unchanged.

## Important unresolved constraint: cached mesh draw commands

UE4.27's VF type registration has a `bSupportsCachingMeshDrawCommands` capability flag. A per-draw transform cannot be read from transient mesh-batch memory if the resulting draw command may outlive that memory.

For the first proof, choose one of these explicit policies:
- **No cached draw commands for the experimental chart VF**, if the capture path can guarantee uncached submission; or
- persistent, renderer-owned chart draw records/uniform buffers with stable lifetime and correct rebinding/update semantics.

Do not enable mesh draw command caching until the chosen binding is shown to survive command caching and updates. A persistent uniform buffer alone is not sufficient if the cached command retains the wrong buffer for a later triangle.

## Transform space contract

The raster hook receives the post-WPO translated-world position used by the Base Pass. Its return value is multiplied by `ResolvedView.TranslatedWorldToClip`.

Consequently, the chart transform must map from the exact input coordinate space (translated world) into a position that the capture view projection maps to the intended atlas tile. The implementation must explicitly account for `PreViewTranslation`, clip-space W, viewport, scissor and atlas tile origin. Returning chart UVs as a world position is invalid.

For the initial prototype, prefer a capture view with a deliberately constructed projection that maps a simple chart plane to the target tile. Validate with non-zero `PreViewTranslation`; do not assume the main camera's view constants are suitable.

## One-triangle draw is still not solved

A per-triangle raster transform does not itself restrict a section draw to one triangle. The draw must use an index range containing exactly the three indices for the intended source triangle, without mutating the shared static-mesh index buffer.

The eventual draw path must prove:
- source triangle index maps to the correct original index-buffer triplet;
- base vertex and index-buffer offsets are applied exactly once;
- mesh batch element min/max vertex and first-index fields are correct;
- the draw command is uncached or has correct stable per-triangle binding identity.

Until that is implemented, do not claim the one-triangle prototype is executable.

## Minimal implementation order

1. Add the chart VF C++ type and shader wrapper only; no chart transform yet. Register only the necessary material shader frequency and compile.
2. Verify the chart VF compiles a simple opaque Base Pass material while normal local VF permutations remain unchanged.
3. Add the chart parameter class with a constant test transform supplied through a safe, non-cached test draw path.
4. Prove material UV, tangent basis and world-position inputs match normal rendering.
5. Add a true single-triangle index range and verify the chart lands in the expected tile.
6. Only then add dynamic per-triangle transform data and atlas scheduling.

## Stop conditions

Stop and revise if:
- the wrapper cannot include the local VF implementation cleanly;
- derived VF registration does not retain the local vertex declaration/streams as expected;
- Base Pass material shader selection rejects the new VF;
- chart parameter binding cannot be made safe under the selected draw-command cache policy;
- the capture draw cannot select exactly one source triangle without modifying shared mesh buffers.

No claims of compilation, runtime success, complete chart capture, or RT integration are made by this document.
