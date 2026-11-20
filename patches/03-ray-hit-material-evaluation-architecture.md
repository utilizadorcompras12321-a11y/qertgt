# Patch 03 — Shared Software RT Core and Surface Cache

Target: UE 4.27.2 source. This is the selected shared architecture for software RT shadows, GI, and reflections.

## Decision

Build one Engine/Renderer-owned **Software RT Core** shared by all software RT effects. Do not build separate acceleration structures or material systems for DDGI, shadows, and reflections.

The core owns reusable scene representation and hit data. Each effect owns its ray generation, acceptance rules, shading/transport, denoising, and temporal history.

The visual target is DXR-like scene coverage, not bit-identical DXR behavior or hardware-DXR performance. SSR is not a required stage or primary source.

## Shared architecture

```
UE4 Renderer scene
  ├─ Static mesh geometry ── BLAS
  ├─ Instanced geometry ──── shared BLAS + instance records
  ├─ Dynamic geometry ────── dynamic/refitted BLAS
  └─ Primitive transforms ── TLAS
                              |
                    Software RT Core
                    ├─ scene membership/culling
                    ├─ BLAS/TLAS lifecycle
                    ├─ triangle intersection
                    ├─ hit record (primitive, instance, triangle,
                    |             barycentrics, distance, UV data)
                    └─ material surface lookup
                              |
              ┌───────────────┼────────────────┐
              |               |                |
          RT Shadows        RT GI        RT Reflections
          visibility       diffuse        specular
          rays             rays            rays
          own resolve      own resolve     own resolve
          own history      own history     own history
```

## Geometry representation

Use a GPU-traversable triangle BVH as the primary representation. SDF/voxel structures may be added later as optional accelerators or fallbacks, not as the authoritative geometry for general reflections.

- **BLAS:** geometry-local acceleration structure, shareable by multiple instances of the same mesh.
- **TLAS:** instance transforms and references to BLAS; update/refit when instances move.
- **Static geometry:** build once and retain.
- **Movable instances:** update instance records/TLAS without rebuilding unchanged BLAS.
- **Deforming/skinned geometry:** explicit dynamic geometry path; initially refit/rebuild only affected BLAS within a budget.
- **WPO:** must be represented in traced geometry or explicitly unsupported for that primitive. Updating only material captures is insufficient.
- **Unsupported or excluded primitives:** expose coverage/stat counters; never silently pretend the scene is complete.

The core must not assume a specific consumer, DDGI cascade, voxel layout, or plugin.

## Shared hit contract

A successful intersection must provide enough information for consumers and material lookup:

- primitive and instance identity;
- geometry/triangle identity;
- hit distance and world-space position;
- barycentric coordinates;
- geometric normal and interpolated shading basis;
- UV channels needed by the material capture mapping.

Do not reduce a triangle hit to only a world position: that loses stable surface identity and makes cache lookup unreliable.

## Material representation

Do not execute the complete Material Graph at every ray hit.

Use an Engine-owned **Software RT Surface Cache**, populated through the existing raster material pipeline. All ordinary Materials and Material Instances participate automatically; no user-facing checkbox, per-material opt-in, special material asset, or manual authoring step.

Initial cached properties:

- Base Color
- Shading Normal
- Roughness
- Metallic
- Specular
- Emissive
- Ambient Occlusion
- Opacity Mask / coverage for masked materials

Material Instances use their resolved render proxy and normal uniform-expression/parameter path. The cache is shared across all RT consumers.

The cache mapping must be based on stable surface identity/coordinates. Do not assume Lightmap UVs exist or are unique. A card/atlas implementation needs a deterministic mapping from primitive + hit position/normal to a capture and atlas UV. Coverage misses must be measurable and visualizable.

The capture is an approximation of the original material at finite resolution and limited directions. Raster-only expressions (SceneTexture, PixelDepth, screen coordinates, pixel derivatives) do not have universal view-independent ray-hit semantics. Document these limits rather than inventing values. Translucency needs a separate transmission representation.

## Consumer responsibilities

### RT Shadows
- Generate visibility rays from the receiving surface toward the light.
- Opaque accepted hits terminate visibility; do not fetch full material properties for opaque occlusion.
- Masked hits consult cached opacity/coverage.
- Translucent transmission is a separate later feature.

### RT GI
- Generate diffuse rays using the receiving normal and sampling policy.
- Fetch cached surface properties at accepted hits.
- Accumulate indirect radiance with its own temporal/spatial reconstruction.

### RT Reflections
- Generate specular rays from view direction, normal, and roughness.
- Trace the same scene BVH, including off-screen geometry.
- Fetch cached surface properties and evaluate the reflection lighting/transport.
- Use its own roughness filtering and temporal/spatial reconstruction.
- SSR is not a required fallback. Sky/Reflection Captures may be used for misses or unsupported cache coverage, but must not be confused with a successful scene hit.

## Performance policy

- One shared geometry representation and one shared material cache.
- Build static BLAS once; update only dirty dynamic geometry and instance data.
- Bound per-frame build/refit and cache-capture work.
- Start with configurable internal resolution and ray budgets per effect; do not hard-code a universal half-resolution assumption.
- Keep shadow visibility cheap; only GI/reflections need full surface shading data.
- Batch/cohere rays where practical, compact active rays, and avoid unnecessary material fetches.
- Reuse hit results only when ray, scene version, and required hit semantics are genuinely equivalent. Do not force unsafe cross-effect reuse.
- Track BVH memory/build/refit time, rays, traversal steps, hit/miss rates, surface-cache coverage/misses, and denoising cost.

## UE4.27 integration sequence

1. Audit and integrate with Renderer scene/primitive lifecycle. The core must receive primitive add, remove, transform, geometry-change, and material-change notifications.
2. Add the shared Software RT Core scene data and GPU buffer/resource lifecycle.
3. Implement and validate triangle BVH build, refit, and traversal independently of any effect.
4. Add automatic surface capture and stable hit-to-cache mapping.
5. Add a minimal debug consumer that visualizes hit distance, instance/triangle identity, normals, and cache coverage.
6. Connect RT Shadows, GI/DDGI, and Reflections as separate consumers.
7. Add temporal reconstruction and per-effect scalability only after correctness is established.

## First acceptance tests

- Static triangle scene: exact expected hit/miss and distance.
- Two instances sharing one BLAS: distinct transforms, shared geometry.
- Moving instance: TLAS update without BLAS rebuild.
- Dynamic geometry: only affected BLAS updates.
- Material Instance: scalar/vector/texture overrides appear in the capture.
- Masked foliage: transparent texels reject hits.
- Off-screen object: reflection ray can hit it.
- One scene and one surface cache can be queried by all three consumers without duplicate scene/capture ownership.

## Explicit non-goals

- Reimplementing DXR or claiming DXR-equivalent performance.
- Executing the complete Material Graph per ray.
- Making SSR a required component.
- Using SDF/voxels as the sole general-purpose geometry representation.
- Three independent BVHs or material caches.
- Requiring artists to opt materials or meshes into software RT.

This is an architecture and integration contract. It is not a claim that the runtime core, BVH, cache, or effects have already been implemented or compiled.
