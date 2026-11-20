# 18 — Static-mesh chart capture prototype: renderer integration design

Status: source-audited implementation design. Not engine code, not applied to the UE checkout, and not compiled.

## Decision

Prototype a dedicated **chart-capture vertex factory path** for one opaque static-mesh section, while reusing UE4.27's deferred Base Pass material shaders and GBuffer encoding wherever possible.

Do not attempt to make an ordinary camera Base Pass render directly into an atlas by changing only the viewport. Do not overwrite the source UV channels with chart UVs.

The capture has two independent coordinate domains:

- **Material domain:** original local/world position, source UV channels, vertex color, tangent basis, primitive/material parameters. These feed the existing material shader.
- **Raster domain:** generated chart clip-space position. This determines where the triangle writes into the Surface Cache atlas.

The chart coordinate must never become the material's TexCoord input.

## UE4.27 source audit

Mirror checked: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

### Existing mesh/material path

- `FStaticMeshSceneProxy::GetMeshDescription` gathers mesh batches by LOD section and batch through `GetMeshElement`.
- `FStaticMeshSceneProxy::GetMeshElement` resolves the section's component material, obtains its render proxy, chooses the LOD vertex factory, and fills the mesh batch's index buffer, element ranges, material proxy and vertex factory.
- `FMeshBatch` stores a material render proxy and vertex factory; each `FMeshBatchElement` stores index/vertex ranges and primitive/user data.
- `FBasePassMeshProcessor::AddMeshBatch` resolves the material/fallback and dispatches the material/vertex-factory shader permutation.
- The existing Base Pass pixel shader evaluates material pixel inputs and, on deferred platforms, calls `EncodeGBuffer` and writes the configured MRT outputs.
- `FDeferredShadingSceneRenderer::RenderBasePass` obtains the normal scene GBuffer render-target set through `FSceneRenderTargets::GetGBufferRenderTargets`.

### Existing local vertex factory limitation

`FLocalVertexFactory::FDataType` inherits `FStaticMeshDataType`. Its standard inputs include the original position, tangent, texture-coordinate and color data. `FLocalVertexFactory::SetData` updates this vertex factory data on the rendering thread.

The ordinary local vertex factory's shader path computes material world position and then rasterizes that world position through the current view's translated-world-to-clip transform. There is no built-in independent chart-position stream in this path.

Therefore, simply setting a different viewport or replacing the vertex buffer position would either leave the mesh in camera projection or corrupt the position/UV/tangent values used by the material. A separate chart-raster position is required.

## Proposed narrow prototype

### A. Scope

Support only:
- deferred renderer;
- static mesh;
- one LOD selected by the existing RT geometry provider;
- one opaque, non-masked, non-translucent material section;
- no tessellation;
- no WPO;
- no spline deformation, skinning, Nanite, or per-view material semantics.

The first test material should use Base Color, Roughness, Metallic, Specular, Emissive, and a regular source texture sample. It must not use ScreenPosition, SceneColor, camera-facing expressions, or other view-dependent inputs.

Masked support comes after the opaque capture is proven because opacity clipping and coverage must be tested separately. Translucency is not part of this prototype.

### B. Per-triangle chart geometry

Use the source triangle order preserved by Patch 16. Generate three capture vertices per triangle corner, rather than trying to share vertices across charts.

Each generated capture vertex carries:
- original local position;
- original tangent basis;
- original source UV set(s);
- original vertex color;
- generated chart position (or chart clip coordinate);
- any required primitive/instance data.

The three generated chart positions form a non-overlapping right-triangle region with an atlas gutter. The source UVs and other material attributes are copied unchanged. The generated chart coordinate is a separate attribute, never inserted into the material UV array.

This intentionally duplicates vertices. It is acceptable for the first correctness prototype; later packing/merging is a separate optimization.

### C. Vertex factory and shader boundary

Add a dedicated capture vertex factory (initially a static-mesh-derived implementation if UE4.27's shader registration constraints permit it; otherwise a dedicated vertex factory type). It must expose the same material-facing attributes as the original static-mesh local vertex factory plus one additional chart-raster attribute.

The capture vertex shader must:
1. build the same material vertex parameters from the original attributes;
2. calculate world position and tangent basis through the normal material path;
3. apply the same supported position semantics as the normal Base Pass (WPO is explicitly disabled in this prototype);
4. write the chart-raster position to `SV_Position`, independently of the material world position;
5. preserve all interpolants required by the material pixel shader.

Do not implement this by replacing `VertexFactoryGetWorldPosition` globally. The chart coordinate belongs only to raster positioning. The exact UE4.27 shader hook and permutation must be added as a capture-only path so normal rendering remains unchanged.

### D. Reuse Base Pass material evaluation and GBuffer encoding

Prefer the existing deferred Base Pass pixel shader and GBuffer encoding over a second material evaluator.

The capture pass should use the capture vertex factory's Base Pass shader permutation and render to a dedicated atlas MRT set with formats/layout compatible with the active UE4.27 GBuffer configuration. It must not write into the live scene's GBuffer or SceneColor.

The pass needs its own render-target allocation and binding. It must not call `FSceneRenderTargets::GetGBufferRenderTargets` and then redirect those live scene targets to the atlas. Instead, create capture-owned atlas textures whose formats, MRT order, and optional outputs match the selected GBuffer permutation. Validate the active platform's GBuffer configuration before enabling the pass.

This reuses material shader evaluation and GBuffer packing, but it is not a drop-in invocation of `FBasePassMeshProcessor`: that processor and its draw-state setup live in Renderer private code and are coupled to normal Base Pass assumptions. Add a dedicated capture draw path in Renderer that deliberately reuses the existing Base Pass shader types/processor logic while supplying capture render state and capture atlas targets.

Do not copy the entire Base Pass renderer. Factor the minimum reusable shader selection/mesh-command setup only after a one-section prototype demonstrates the required bindings.

### E. Chart lookup

For a hit, Patch 14's barycentrics interpolate the three chart corner coordinates stored for the original source triangle. The resulting chart UV addresses the atlas.

The lookup must validate:
- SurfaceKey and source triangle mapping;
- capture material revision;
- capture policy revision;
- chart allocation generation;
- valid texel coverage.

A missing or stale mapping is a cache miss. It must not alias another chart.

## Why the ordinary Base Pass cannot simply be reused unchanged

The ordinary Base Pass uses a view projection for raster position and writes into scene render targets selected by the deferred renderer. A chart capture changes both of those assumptions.

Changing only the viewport does not transform arbitrary mesh triangles into atlas charts. Changing the vertex position stream changes the geometry seen by the material shader. Rebinding the live GBuffer is unsafe because the pass would overwrite the frame's actual scene data.

The dedicated capture path is therefore a small but real Renderer extension: separate chart position, capture-only shader permutation, capture-owned MRTs, and explicit mesh draw state.

## Material semantics and hard limits

- Source texture UVs, vertex colors, world position and tangent basis must remain the same as the original mesh's material inputs.
- ScreenPosition and other camera/view-dependent expressions cannot be treated as universally view-independent material values. Reject these materials from the first prototype and record the reason.
- WPO is disabled until the Software RT geometry provider and capture pass share the same displaced geometry.
- Pixel depth offset is disabled for the same reason: it changes raster depth without changing the current software BVH surface.
- Tessellation is disabled.
- Runtime virtual textures and virtual texture page dependencies require explicit invalidation and residency handling; exclude them from the first prototype.
- The GBuffer is a packed representation, not a lossless copy of every possible Material Graph output. Custom data, shading models and feature-specific encodings must be checked against the RT consumer's needs.
- This prototype targets opaque material attributes. It does not implement transmissive shadowing.

## Implementation sequence

1. Add a test-only capture vertex factory and shader permutation; compile it for the target desktop deferred feature level.
2. Build one static-mesh section's three-vertices-per-triangle chart geometry, retaining original material attributes.
3. Render the section with the capture-only vertex shader and existing Base Pass pixel shader into a single capture-owned GBuffer-compatible target set.
4. Read back/debug-view the atlas and verify:
   - triangles occupy separate charts;
   - overlapping source UVs do not overlap in chart space;
   - a source texture sampled by the material remains unchanged;
   - material world position and tangent basis remain correct;
   - GBuffer values match the same section rendered by the normal Base Pass, within expected format/quantization differences.
5. Add barycentric chart lookup and verify the selected texel belongs to the hit's source triangle.
6. Only then add multiple sections, component material overrides, atlas packing, material revision invalidation and masked coverage.

## Acceptance tests

- Two source triangles with identical UV coordinates write to different atlas regions.
- A checker texture sampled through source UVs has the same pattern in normal rendering and chart capture.
- A world-position-driven material produces matching output for corresponding surface points.
- A tangent-space normal map uses the original tangent basis, not the chart axes.
- The capture pass does not alter the live SceneColor or GBuffer.
- The pass compiles and runs with the standard deferred renderer without DXR or RT hardware.
- An unsupported material feature is reported as unsupported; it does not silently produce a misleading cache entry.

## Stop condition

If the capture vertex factory cannot preserve the original material interpolants while supplying independent chart raster positions without a broad rewrite of all local vertex-factory shader paths, stop and reassess.

Do not fall back to changing source UVs or rendering a normal camera Base Pass into a texture. Those shortcuts invalidate material semantics and defeat the Surface Cache contract.

## Explicit status

This is a source-grounded design and implementation sequence, not a compiling patch. The next code patch should be limited to the test-only capture vertex factory and shader permutation after the exact shader registration and vertex-factory input declarations have been validated in the target UE4.27.2 tree.
