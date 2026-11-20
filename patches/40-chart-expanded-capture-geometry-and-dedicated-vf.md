# 40 — Chart-expanded capture geometry and dedicated Vertex Factory decision

Status: source audit and implementation decision. No Engine source was changed. No C++ was compiled and no runtime test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Related:
- [Patch 13 — Static Mesh hit attributes export](13-static-mesh-hit-attributes-export.patch.md)
- [Patch 18 — Static Mesh chart capture prototype design](18-static-mesh-chart-capture-prototype-design.md)
- [Patch 21 — Register experimental chart Vertex Factory](21-register-experimental-chart-vertex-factory.patch.md)
- [Patch 23 — Static Mesh section batch and UserData audit](23-static-mesh-section-batch-and-userdata-audit.md)
- [Patch 30 — Local Vertex Factory resource lifecycle audit](30-local-vertex-factory-resource-lifecycle-audit.md)
- [Patch 39 — Surface Cache material-capture HLSL contract](39-surface-cache-material-capture-hlsl-contract.md)

## 1. Decision

Do not try to make the current cloned `FLocalVertexFactory` represent chart-expanded vertices by adding a chart UV to its existing material UV array.

Use a dedicated capture vertex factory with an explicit, interleaved capture vertex layout. Keep the original material's vertex-factory/material interface, but make the capture VF own the mapping from the expanded capture stream to the material's original attributes.

This is the smallest design that makes the per-corner chart UV and original surface position unambiguous. It is a new VF implementation, but it avoids changing the global Static Mesh VF or generated material ABI.

The existing Patch 21 Local-VF-derived approach is not sufficient as written. It may remain useful as a reference for shader registration and material shader lookup, but it is no longer the proposed capture geometry implementation.

## 2. UE4.27 source facts

Mirror paths inspected:

- `Engine/Source/Runtime/Engine/Public/Components.h`
- `Engine/Source/Runtime/Engine/Public/LocalVertexFactory.h`
- `Engine/Source/Runtime/Engine/Private/LocalVertexFactory.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Public/Rendering/PositionVertexBuffer.h`
- `Engine/Source/Runtime/Engine/Public/Rendering/StaticMeshVertexBuffer.h`
- `Engine/Source/Runtime/Engine/Public/Rendering/ColorVertexBuffer.h`
- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`

Confirmed details:

1. `FStaticMeshDataType` is a set of `FVertexStreamComponent` values for position, two tangent-basis streams, texture coordinates, lightmap coordinate and color, plus optional SRVs and counts. It is not a generic arbitrary per-corner attribute container.
2. `FLocalVertexFactory::FDataType` only extends this with `PreSkinPositionComponentSRV`.
3. `FLocalVertexFactory::SetData()` copies the supplied data and calls `UpdateRHI()`; the VF owns its declaration construction and its stock shader ABI.
4. `FLocalVertexFactory::Copy()` enqueues a command that copies the source `Data` through a raw source pointer, then calls `BeginUpdateResourceRHI()`. The source and destination must remain alive until that command has consumed them.
5. Static Mesh resource buffers expose CPU-side accessors:
   - `FPositionVertexBuffer::VertexPosition()`
   - `FStaticMeshVertexBuffer::VertexTangentX()`, `VertexTangentY()`, `VertexTangentZ()`, `GetVertexUV()`
   - `FColorVertexBuffer::VertexColor()`
6. `FPositionVertexBuffer`, `FStaticMeshVertexBuffer` and `FColorVertexBuffer` expose CPU-access policy/state. The existence of an accessor does not prove the CPU backing data is retained in every cooked/runtime asset.
7. `FVertexStreamComponent` describes a stream buffer, offset, stride, element type and usage. It can describe a custom interleaved capture vertex struct, provided the VF builds a matching vertex declaration.
8. The normal Local VF shader path consumes its standard position/tangent/UV/color stream semantics. Appending a chart stream as an extra material UV is not acceptable: it can change material UV counts and expose chart coordinates to Material Graph UV nodes.

## 3. Capture vertex layout

Build one capture vertex for each source triangle corner. A triangle therefore produces exactly three capture vertices, even when its source vertices are shared by adjacent triangles.

Proposed CPU-side structure (layout is illustrative; exact alignment and vertex element declarations must be fixed in the implementation):

```cpp
struct FSoftwareRTCaptureVertex
{
    FVector3f Position;          // source local-space position
    FPackedNormal TangentX;
    FPackedNormal TangentZ;      // preserve tangent-Z sign/handedness
    FVector2f ChartUV;           // unique to this source triangle corner
    FVector2f MaterialUVs[MAX_STATIC_TEXCOORDS];
    FColor Color;
};
```

Important constraints:
- Do not put world-space positions into the vertex buffer. Preserve local-space source positions and use the original primitive transform/uniform data for the material VF path.
- Preserve all source material UV channels in their original indices and values.
- Preserve tangent basis and handedness using the same convention as the source Static Mesh buffers. Verify the packed normal representation and tangent-Y reconstruction against UE4.27 before finalizing the struct.
- Preserve vertex color when present. If the source section uses an override color buffer or unsupported color override path, reject that section in the initial prototype.
- Do not duplicate material UVs into a new semantic and then change `NUM_MATERIAL_TEXCOORDS` to include ChartUV.
- ChartUV is a capture-only attribute and must never be visible to material UV accessors.

The expanded vertex buffer can be accompanied by an index buffer containing sequential indices, or rendered non-indexed if the chosen RHI draw path supports the required submission cleanly. Prefer an index buffer for the first implementation because the existing mesh draw machinery is indexed-mesh oriented. Every source triangle emits three indices referring to its three expanded vertices.

## 4. Dedicated capture VF responsibilities

The dedicated VF must define:
- an explicit vertex declaration for Position, TangentX, TangentZ, ChartUV, material UV channels and optional Color;
- an input/intermediate structure matching that declaration;
- a shader parameter binding implementation;
- a chart raster-position function that uses ChartUV;
- the standard material-facing interpolants, populated from the original attributes;
- a separate original translated-world-position interpolant for the material PS;
- a compile permutation policy limited to the first supported feature level/platform set.

Do not derive from `FLocalVertexFactory` and assume that the extra stream automatically appears in `FVertexFactoryInput`. A subclass still inherits the Local VF shader implementation unless the shader path and input structure are explicitly changed.

The preferred implementation is a dedicated `FVertexFactory` subclass with its own `FVertexFactoryType` shader source and a purpose-built `.ush` file. It may reuse low-level helper functions from Local VF only after confirming their required structures and uniform declarations are compatible. Avoid copying the entire Local VF shader unless the custom layout genuinely requires it.

The capture VF must still satisfy the material shader's expectations for:
- `FVertexFactoryInput`;
- `FVertexFactoryIntermediates`;
- `VertexFactoryGetWorldPosition`;
- `VertexFactoryGetTangentToLocal`;
- `VertexFactoryGetInterpolantsVSToPS`;
- `GetMaterialPixelParameters` and the material-generated interpolants.

The exact generated structure definitions and helper dependencies need a dedicated shader-source audit before writing the VF shader. This patch intentionally does not invent their fields.

## 5. Chart rasterization and original surface data

For each expanded vertex:

1. Read the original local position and transform it using the same primitive transform path used by the material VF.
2. Save the resulting original translated-world position in a dedicated VS-to-PS varying.
3. Compute chart raster position from ChartUV and the capture chart transform.
4. Write chart raster position to `SV_POSITION`.
5. Generate material interpolants from the original vertex attributes and original world/tangent frame, not from ChartUV.

The chart transform must explicitly define atlas viewport, tile origin, tile dimensions, clip-space depth/W and the PreViewTranslation convention. A screen-space chart pass must not accidentally use scene camera projection.

At the PS:
- use the separate original translated-world-position varying when calling `CalcMaterialParametersEx()`;
- do not reconstruct material world position from chart `SV_Position`;
- do not expose ChartUV through the material's regular UV interpolants;
- keep the original material UV interpolants unchanged.

The rasterized chart position and original surface position are different coordinate domains. Treating them as one is a correctness bug, not a tolerable approximation.

## 6. CPU source-data availability and fallback

The Static Mesh vertex buffer accessors are only usable when their CPU backing data is retained. A GPU buffer or SRV being valid does not imply that the CPU data pointer remains available.

Before building expanded capture geometry:
- verify the source LOD's position, tangent/UV and color buffers retain CPU data;
- verify the index buffer can be read on the CPU;
- validate all vertex indices and section ranges;
- validate all required material UV channels exist;
- validate the effective material and supported section policy.

If any required CPU data is unavailable, skip capture for that LOD/section and report a diagnostic. Do not dereference absent CPU data, silently substitute zeros, or force all Static Mesh assets to retain CPU access as an undocumented global side effect.

A later GPU expansion path may be considered, but it is not part of this prototype.

## 7. Section and primitive semantics

Build capture geometry per source LOD and material section, or retain an explicit source-triangle-to-section table. Each capture draw must preserve:
- source primitive proxy and primitive uniform data;
- effective material render proxy for that section;
- source section's triangle range and source triangle identity;
- reverse-culling / determinant semantics;
- material UV and vertex color values;
- the stable source-triangle identity already defined in Patch 16.

Do not collapse sections with different materials into one draw. Do not use a temporary chart buffer as `FMeshBatchElement::UserData` or `VertexFactoryUserData` without following the exact UE4.27 binding contracts established in Patch 23 and Patch 38.

The chart-expanded geometry is capture-only. It must not replace the source Static Mesh's render buffers or affect the normal scene Base Pass.

## 8. Ownership and update lifecycle

The capture geometry owner must retain:
- CPU staging data for as long as any build task reads it;
- RHI vertex buffer;
- RHI index buffer;
- dedicated capture VF;
- any VF uniform buffers/SRVs;
- chart mapping resources;
- material draw command resources until the established RHI-thread retirement point.

Initialization/release must follow the Render Resource lifecycle. Do not destroy an initialized VF or vertex buffer by dropping a C++ pointer. Release on the render thread after all submitted commands that reference it have been consumed, using the retirement model from Patches 29–34.

For the initial prototype, prefer immutable capture geometry per LOD/material-revision generation. Rebuild and retire a generation rather than mutate buffers while cached or in-flight commands may still reference them.

## 9. First implementation milestone

Implement only:
1. CPU expansion for one Static Mesh LOD and one opaque section;
2. dedicated capture vertex/index buffers;
3. a minimal dedicated VF that outputs chart raster position and original translated-world position;
4. a debug PS that writes a constant color to one private target;
5. one triangle or one section draw with no material shader evaluation.

Only after this passes should the VF be connected to `FMeshMaterialShader` and the three-MRT material-only PS from Patch 39.

### Acceptance tests

- A shared source vertex used by triangles in different charts produces different capture vertices and correct chart UVs.
- Material UV0..UVn are byte/value-equivalent to the source attributes.
- World position in the capture PS matches the original transformed surface position.
- Tangent frame and mirrored-transform handedness match the source convention.
- Source Static Mesh buffers and normal rendering are unchanged.
- Missing CPU access produces a clean skip/diagnostic, not a crash.
- Section material and primitive identity are preserved.
- Resource retirement does not free buffers/VF while draw commands can still reference them.

## 10. Stop conditions

Stop and redesign if:
- the custom VF cannot provide the generated material shader's required interpolants without broad modifications to the global material/VF ABI;
- the only way to preserve chart UVs is to expose them as material UV channels;
- the runtime path assumes CPU vertex data exists for every cooked asset;
- the chart raster mapping requires rewriting the normal scene projection/view path;
- resource lifetime cannot be tied to the capture generation and draw submission.

A small dedicated capture VF is preferable to invasive changes to `FLocalVertexFactory` or `FStaticMeshSceneProxy`.

## 11. Audited source paths

- `Engine/Source/Runtime/Engine/Public/Components.h`
- `Engine/Source/Runtime/Engine/Public/LocalVertexFactory.h`
- `Engine/Source/Runtime/Engine/Private/LocalVertexFactory.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Public/Rendering/PositionVertexBuffer.h`
- `Engine/Source/Runtime/Engine/Public/Rendering/StaticMeshVertexBuffer.h`
- `Engine/Source/Runtime/Engine/Public/Rendering/ColorVertexBuffer.h`
- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`
