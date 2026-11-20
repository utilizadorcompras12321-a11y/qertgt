# 47 — Selective Surface Cache Material Permutation Rebuild Audit

**Status:** source audit and design decision; not applied or compiled.  
**Engine reference:** `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.  
**Target:** UE 4.27.2.

## 1. Corrected conclusion: selective recompilation is possible, but selective permutation inclusion needs an opt-in flag

The previous patch established that the new VF and material shader types must be registered before material shader-map layouts are created.

This audit adds an important distinction:

- **Which materials are rebuilt** can be selected.
- **Which material permutations are included in a shader-map layout** is controlled by material shader parameters and shader/VF predicates, not by an arbitrary asset list in the capture processor.

The layout cache in `MaterialShader.cpp` builds its key from `FMaterialShaderParameters` and enumerates all registered material VFs and mesh-material shader types. The structure in `MaterialShared.h` contains domain, blend mode, shading models, tessellation, feature level, quality level, and packed usage/property flags. It does not contain a generic per-asset Surface Cache opt-in.

Therefore, simply restricting the capture processor to selected assets does **not** stop compatible non-selected materials from receiving the capture shader permutation during normal shader-map compilation.

## 2. Targeted rebuild API found

Files:

- `Engine/Source/Runtime/Engine/Classes/Materials/Material.h`
- `Engine/Source/Runtime/Engine/Private/Materials/Material.cpp`
- `Engine/Source/Runtime/Engine/Public/MaterialShared.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInstance.cpp`

The public Editor-only API is:

```cpp
UMaterial::UpdateMaterialShaderCacheAndTextureReferences()
```

Its implementation:

1. Cancels outstanding compilation for the material.
2. Calls the private `CacheResourceShadersForRendering(true)`.
3. This regenerates the material state/shader-map identity through `FlushResourceShaderMaps()`.
4. Rebuilds the material's resources for active feature levels/quality.
5. Recaches uniform expressions and propagates material data.
6. Recreates component render state for affected primitives.

The underlying `CacheResourceShadersForRendering(bool, ...)` is private. External code must not call it directly. `UMaterial::ForceRecompileForRendering()` is public but calls `CacheResourceShadersForRendering(false)`; it does not force the state ID regeneration needed for this use case.

## 3. Dependent material instances

`FMaterialUpdateContext` is the engine-supported mechanism for tracking material changes and dependent instances.

Its public API provides:

```cpp
FMaterialUpdateContext UpdateContext(
    FMaterialUpdateContext::EOptions::SyncWithRenderingThread);

UpdateContext.AddMaterial(BaseMaterial);
```

At context destruction, the engine:

- finds loaded material instances dependent on the updated base material;
- updates their cached layer parameters;
- recaches uniform expressions;
- calls `InitStaticPermutation(EMaterialShaderPrecompileMode::None)`;
- updates static draw lists for affected resources when the component recreation/reregistration options were not used.

`InitStaticPermutation()` rebuilds static-permutation material resources, so dependent static material instances are not silently left with the old shader map.

### Candidate Editor-only targeted flow

For each selected base material, on the Game Thread:

1. Construct an `FMaterialUpdateContext` with `SyncWithRenderingThread`.
2. Call `AddMaterial(BaseMaterial)`.
3. Call `BaseMaterial->UpdateMaterialShaderCacheAndTextureReferences()`.
4. Let the context destruct, which recaches dependent material instances and updates static draw lists.

This is a source-backed candidate, not yet a compile-ready helper. The nested render-state recreation performed by `UpdateMaterialShaderCacheAndTextureReferences()` and the context's static draw-list update should be measured and reconciled before production use. Do not execute this every frame or during rendering.

## 4. Why UMaterial::UpdateMaterialShaders() is not the selective solution

The static API:

```cpp
UMaterial::UpdateMaterialShaders(
    ShaderTypesToFlush,
    ShaderPipelineTypesToFlush,
    VFTypesToFlush,
    ShaderPlatform);
```

looks relevant because it accepts shader and VF type lists. However, its implementation iterates every loaded `UMaterial`, adds each to one `FMaterialUpdateContext`, and calls `CacheResourceShadersForRendering(false)`. It is a global material shader-map refresh path, not a targeted per-asset rebuild. Do not use it for a small Surface Cache opt-in set.

## 5. Required opt-in for avoiding unnecessary capture permutations

To avoid compiling capture shaders for every material that matches broad properties such as opaque/default-lit/no-tessellation, add a material-level opt-in that participates in `FMaterialShaderParameters`.

Recommended direction:

- Add an explicit `UsedWithSoftwareRTCapture` material usage flag/property to the UMaterial usage model.
- Expose its value through the `FMaterial` interface.
- Copy it into `FMaterialShaderParameters`.
- Include it in the packed flags/hash used by `FMaterialShaderMapLayoutCache`.
- Gate the capture material shader's `ShouldCompilePermutation` on this flag, in addition to VF/platform/domain/blend/tessellation restrictions.
- Ensure material instances inherit the usage eligibility from the base material; do not add a separate per-instance toggle in the first implementation.

This is preferable to using an unrelated existing usage flag. It makes the opt-in visible in the material asset, ensures DDC/shader-map identity reflects the new compile eligibility, and lets ordinary materials avoid the capture permutation.

The exact enum/property propagation points and serialization/editor UI changes need a dedicated API audit before writing the C++ patch.

## 6. Startup and cook requirements

- Register the capture VF and capture material shader types before the first material shader-map layout is generated. Late registration is unsafe: the material shader-map layout cache is static and has no public invalidation API established by this audit.
- Do not hot-register the VF after materials have already built shader-map layouts in the same Editor process. Restart after integrating the static registrations.
- Rebuild/cook the opt-in materials for each supported target shader platform. Cooked platforms cannot rely on runtime material shader compilation.
- The capture processor should continue to use non-fatal shader lookup and skip/log if a required permutation is absent.
- Do not trigger shader compilation from the render thread or from the capture RDG pass.

## 7. Decision

**Proceed with a new material usage opt-in before implementing broad shader-map rebuild tooling.**

The existing public API can rebuild a selected base material and its dependent loaded static material instances. It cannot, by itself, make the capture permutation opt-in per asset. That requires extending the material usage/shader-parameter contract first.

No source code has been changed in the user's UE checkout. This document is an audit/design record only.
