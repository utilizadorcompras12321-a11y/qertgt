# Patch 03 — Generic software-RT material representation

Target: UE 4.27.2 source. This replaces the direct per-ray Material Graph evaluation proposal.

## Decision

**Do not execute the complete Unreal Material Graph at every software ray hit.** That is too expensive and is not the normal fast path used by production game renderers.

Use an Engine-owned **Software RT Surface Cache**: render/capture material properties through the existing raster material pipeline into a reusable surface representation, then let software RT consumers sample that representation at hit points.

This follows the established Surface Cache approach used by Unreal Lumen: software tracing finds surfaces using a cheaper geometric representation, while material properties are looked up from cached surface captures. Epic documents that Lumen captures material properties from multiple mesh-facing cards and amortizes updates over frames. The same principle is appropriate here, but this implementation must be generic UE4.27 Engine infrastructure and must not depend on Lumen, DDGI, a plugin, or a particular BVH.

## Requirements

- Every supported regular Material and Material Instance participates automatically. **No user-facing checkbox, per-material opt-in, special material asset, or manual authoring step.**
- The original Material Graph remains the source of truth. The cache is populated by rendering the original material through the normal raster material pipeline, not by maintaining a second graph or a hand-authored simplified material.
- Material Instances must capture their resolved scalar/vector/texture parameters and overrides.
- The cache is generic Engine/Renderer infrastructure. DDGI, software RT shadows, software RT reflections, and future consumers can read it.
- Consumers own their tracing structures and hit generation. The cache API must not assume a BVH, SDF, voxel grid, or DDGI data structure.
- Do not add a compute shader variant for full Material Graph evaluation as the primary path. Compute graph evaluation may be reconsidered only for a narrowly justified fallback or feature.

## Surface representation

Use a set of surface captures/cards per renderable mesh or primitive, with a GPU-resident atlas or equivalent cache storage. Each capture stores the material outputs needed by RT consumers, initially:

- Base Color
- World/Shading Normal (and/or a reconstructible tangent-space normal representation)
- Roughness
- Metallic
- Specular
- Emissive
- Ambient Occlusion
- Opacity Mask / coverage for masked materials

The exact packing and formats must be selected against UE4.27 RHI capabilities and memory budgets. Avoid storing duplicate channels and avoid assuming FP16 is required for every property.

A hit must resolve to a cache address using stable surface coordinates supplied by the consumer. For a card-based representation, this requires a mapping from the hit primitive/local surface position to the selected card and atlas UV. If the consumer only has a voxel/SDF hit without enough surface coordinates, the cache lookup contract must explicitly define how the surface point is projected to a card; do not invent UVs.

## Capture and update policy

- Populate captures using the existing raster material rendering path so normal material expressions, texture sampling, and Material Instance parameters work without a compute material interpreter.
- Generate card placement and capture resources automatically from renderable geometry; do not require artists to mark materials or meshes.
- Static content should be captured once and retained until geometry/material state changes.
- Dynamic or changing content must update incrementally/amortized across frames, not force all captures to rerender every frame.
- Invalidate or refresh the relevant captures when material assignment, material parameters, textures, geometry, or relevant transforms change.
- Use the existing render proxy and material uniform-expression mechanisms. Do not build a separate parameter binding system.
- Capture resolution/card count must be bounded and scalable. Provide coverage/quality diagnostics so missing coverage is visible rather than silently returning black or stale values.
- Avoid view-dependent material capture assumptions. Expressions such as Camera Position, Pixel Depth, screen-space SceneTexture, and pixel derivatives may not have stable view-independent values. Define capture-view semantics and document limitations. Do not claim exact equivalence for these expressions.

## Consumer behavior

### Software RT shadows

- Opaque geometry should remain opaque in traversal and should not require a material lookup for every hit.
- Masked geometry needs cached opacity/coverage to accept or reject candidate hits. Keep this path as cheap as possible.
- Translucent shadows are a separate transmission problem; do not treat opacity mask as translucency.

### Software RT GI and reflections

- Query cached Base Color, Normal, Roughness, Metallic, Specular, Emissive, and AO only when the consumer needs them.
- Reuse the same cache data across DDGI, reflections, and other consumers.
- A cache lookup is an approximation of the original material at the capture resolution and card coverage. It is not exact per-triangle hit shading.

### WPO and deformation

- WPO changes geometry, not just surface shading. The geometry representation used by the software tracer must account for the deformation or explicitly document that WPO is unsupported. Updating material captures alone cannot fix incorrect intersections.
- Dynamic/skinned/deforming geometry needs a bounded refresh strategy and may require lower capture resolution or fewer cards.

## Memory and performance policy

The primary cost moves from **per-ray full graph execution** to **bounded cache storage and incremental capture updates**.

The implementation must:
- Allocate cache storage only for primitives represented by the software RT scene.
- Share captures across all software RT consumers.
- Update only dirty/visible/relevant captures within a per-frame budget.
- Support streaming/eviction for large scenes.
- Avoid one independent material cache per effect.
- Keep atlas sampling compact and coherent.
- Expose stats for allocated texels, card coverage, update work, misses, and stale entries.

This trades exactness for predictable cost. It is the correct default for a performance-oriented software RT path. A high-quality direct graph evaluation path is not the baseline.

## UE4.27 implementation direction

1. Audit the existing renderer infrastructure for mesh capture, scene capture, render targets, primitive scene info, mesh draw commands, and texture atlas allocation. Reuse existing mechanisms where possible; do not assume UE5 Lumen code exists in UE4.27.
2. Add an Engine/Renderer-owned surface-cache manager and resource lifecycle, independent of any RT consumer.
3. Add automatic card/coverage generation from existing primitive/mesh bounds and surface geometry. The first milestone may use a conservative fixed card layout per primitive, then improve placement/coverage.
4. Render each card through the normal material raster path into cache targets. Ensure Material Instances resolve through their normal render proxies.
5. Add a small, stable lookup API that takes primitive identity plus hit position/normal (or an explicit cache address) and returns the cache UV/texel. The consumer remains responsible for producing valid hit data.
6. Add sampling helpers for consumers and a debug visualization for coverage/misses.
7. Only after the cache works independently, connect DDGI, software shadows, and software reflections.

## First validation

- A textured opaque material is captured and sampled correctly.
- A Material Instance with scalar/vector/texture overrides produces the expected cached values.
- A masked foliage material stores coverage and can reject transparent regions.
- A dynamic material parameter change invalidates and refreshes only affected captures.
- A moving/deforming primitive follows the documented update path.
- Card/atlas coverage visualization exposes uncovered regions.
- Two independent consumers can sample the same cache without duplicate capture work.

## Important limitations

- Surface captures approximate material output at finite resolution and limited directions.
- Camera/view-dependent expressions may not have a stable capture result.
- Thin geometry, complex concave meshes, and poor card coverage can cause misses or leakage.
- Translucency requires a separate representation/transport model.
- WPO/skinning must be reflected in the geometry representation as well as cache updates.

## Superseded proposals

- patches/01-software-rt-material-usage.md must not be applied: no per-material usage flag.
- patches/02-material-compute-shader-registration.md must not be applied: full graph compute evaluation per hit is not the selected fast path.
- The earlier direct ray-hit Material Graph evaluation contract is superseded by this cache-first architecture.

This document is an architecture decision, not a claim that the runtime cache has been implemented or compiled.