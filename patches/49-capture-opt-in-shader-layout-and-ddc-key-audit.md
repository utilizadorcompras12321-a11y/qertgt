# 49 — Capture Opt-In Shader Layout, Shader-Map ID and DDC Key Audit

**Status:** source audit; no engine code changed or compiled.  
**Reference mirror:** `AlexMercer-MA/UnrealEngine-4.27` at `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.  
**Target:** UE 4.27.2.

## 1. Confirmed: FMaterialShaderParameters participates in layout identity

File: `Engine/Source/Runtime/Engine/Private/Materials/MaterialShader.cpp`.

`FMaterialShaderMapLayoutCache::AcquireLayout()` computes:

```cpp
const uint64 ParameterHash = CityHash64WithSeed(
    (char*)&MaterialParameters,
    sizeof(MaterialParameters),
    (uint64)Platform | ((uint64)Flags << 32));
```

The full `FMaterialShaderParameters` byte range is hashed. Its constructor in `Engine/Source/Runtime/Engine/Public/MaterialShared.h` first calls:

```cpp
FMemory::Memzero(*this);
```

and then assigns all fields. This ensures deterministic padding and unused bits. Adding a new one-bit field to the existing `PackedFlags` union and assigning it in this constructor changes the parameter hash when the opt-in changes.

The layout cache's `FindLayout()` uses the resulting 64-bit hash as the lookup identity. The stored `MaterialShaderParameters` array is retained alongside layouts, but this lookup path does not perform a full structure comparison. As with the existing implementation, correctness relies on the 64-bit hash; do not add custom equality logic for this feature.

The current packed flags occupy 42 of the 64 bits in the inspected mirror, so one additional bit fits without changing the union's size.

## 2. Confirmed: the bit changes the shader layout, not merely a runtime boolean

`FMaterial::GetDependentShaderAndVFTypes()` constructs `FMaterialShaderParameters MaterialParameters(this)`, then calls `AcquireMaterialShaderMapLayout()`.

`FMaterialShaderMapLayoutCache::CreateLayout()` iterates registered material shader types and mesh material shader types, evaluating their `ShouldCompilePermutation()` predicates against the supplied `MaterialParameters`. For mesh shaders it also evaluates the selected Vertex Factory's permutations.

Therefore the capture shader predicate can inspect the new bit and include the capture VS/PS only for opted-in materials. A non-opted material will not have those capture shader types in its dependency layout.

The capture predicate must still gate the exact capture VF type and the intended material domain/blend/tessellation restrictions. The material flag alone is not sufficient.

## 3. Confirmed: the difference propagates into FMaterialShaderMapId and DDC

The relevant chain is:

1. `FMaterial::GetShaderMapId()` calls `GetDependentShaderAndVFTypes()`.
2. That function acquires the material layout using `FMaterialShaderParameters`.
3. It collects the shader types, pipelines and VF types present in that layout.
4. `FMaterial::GetShaderMapId()` passes those arrays to `FMaterialShaderMapId::SetShaderDependencies()`.
5. `SetShaderDependencies()` records shader type names and source hashes, pipeline dependencies and VF type names/source hashes.
6. `FMaterialShaderMapId::AppendKeyString()` appends those dependencies to the key string.
7. `GetMaterialShaderMapKeyString()` uses `AppendKeyString()` to construct the material shader-map DDC key.

Thus, when the capture opt-in adds the capture shader types and capture VF dependency to the layout, the shader-map ID and DDC key include those additional dependencies. The opt-in does not need to be separately appended to `FMaterialShaderMapId`; its effect is represented by the resulting dependency set.

Important distinction: the DDC key does not directly append the raw `FMaterialShaderParameters` bitfield. It receives the effect indirectly through the shader/VF dependency list. Do not claim otherwise.

## 4. Material edit invalidation

The inspected `UMaterial::PostEditChangePropertyInternal()` path calls `CacheResourceShadersForRendering(bRegenerateId, ...)` for ordinary shader-relevant property changes, then recreates affected component render states and propagates material proxy data.

The new UPROPERTY should therefore be changed through the normal Editor property system. Its change must not be hidden from the material's post-edit path.

For explicit scripted updates, retain Patch 47's rule: use the public material update route and `FMaterialUpdateContext`; do not call private `CacheResourceShadersForRendering()` directly.

## 5. Required corrections/constraints for Patch 48 implementation

- Append the new flag at the end of the existing `FMaterialShaderParameters` bitfield list; do not reorder existing bits.
- Add its assignment in the zero-initialized `FMaterialShaderParameters` constructor.
- Add the virtual getter to `FMaterial` with a default false implementation, and override it in `FMaterialResource`.
- Ensure the base UMaterial property is initialized false.
- Ensure the capture shader predicate checks both the new flag and the exact capture VF.
- Keep the new flag out of `EMaterialUsage` and all primitive usage-warning logic.
- Do not add a separate manual DDC-key string or custom shader-map ID field for this flag.
- Do not assume a runtime call to `TryGetShaders()` can create a missing permutation in an already-finalized shader map. The material must have been compiled/cooked with the capture VF and shaders registered.

## 6. One remaining implementation verification

The source audit establishes the propagation chain, but a real build/test is still required to verify the integration in the user's exact UE4.27.2 checkout:

1. Build Engine/Editor after adding the property and getter.
2. Register the capture VF and material shaders before material shader-map layouts are first acquired.
3. Enable the flag on one opaque test material and force the normal material update/recompile path.
4. Confirm the capture shader types appear in its material shader map and are absent from a non-opted material.
5. Confirm the two materials produce distinct shader dependency sets/DDC keys.
6. Repeat through a clean cook for the intended target RHI.

This patch is an audit and integration contract only. It is not a source patch, and nothing has been applied to or compiled in the user's local Engine.
