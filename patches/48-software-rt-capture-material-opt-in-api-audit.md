# 48 — Software RT Capture Material Opt-In: UE4.27 API Integration Audit

**Status:** source audit and implementation contract; not applied or compiled.  
**Engine reference:** `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.  
**Target:** UE 4.27.2.

## 1. Decision: do not extend EMaterialUsage

The initial proposal used a new `EMaterialUsage` entry. Source inspection shows this is the wrong abstraction.

`EMaterialUsage` is used by the engine's primitive/material compatibility path:

- `UMaterial::SetUsageByFlag()`
- `UMaterial::GetUsageByFlag()`
- `UMaterial::SetMaterialUsage()`
- `UMaterial::NeedsSetMaterialUsage_Concurrent()`
- `IsPrimitiveTypeUsageFlag()`
- `FMaterialsWithDirtyUsageFlags`

The enum is not a generic feature-permutation registry. Its values participate in bit masks and automatic usage warnings, and primitive-type usages are specifically classified for mesh compatibility.

Software RT capture is a renderer-side material feature, not a new primitive type. Adding it to this enum would couple capture eligibility to automatic primitive usage behavior and dirty-usage annotation machinery for no benefit.

**Use an independent material property instead.**

## 2. Proposed material property

In `Engine/Source/Runtime/Engine/Classes/Materials/Material.h`, add a serialized UPROPERTY alongside the existing Usage flags:

```cpp
/**
 * Enables material shader permutations used by the Software RT Surface Cache capture pass.
 * This does not change compatibility with any scene primitive or regular rendering pass.
 */
UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Usage, AdvancedDisplay)
uint8 bUsedWithSoftwareRTCapture : 1;
```

Initialize it to `false` in `UMaterial::UMaterial(const FObjectInitializer&)`.

Reasons:

- Existing material usage settings are already exposed as UPROPERTY fields in the `Usage` category.
- The normal Details panel can expose this property; no custom Material Editor Slate checkbox is required for the first implementation.
- UPROPERTY serialization handles saving/loading the value; no custom archive serialization should be added.
- Default false prevents all existing materials from acquiring capture permutations by default.
- Blueprint read-only is consistent with the existing material usage flags.

Do not add a per-material-instance override in the first version. The feature flag belongs to the base material and is naturally shared by all its instances through `FMaterialResource::Material`.

## 3. Propagate the flag through FMaterial

The shader compiler does not inspect `UMaterial` directly. It receives an `FMaterial` abstraction.

Required API additions:

### `Engine/Source/Runtime/Engine/Public/MaterialShared.h`

Add a virtual getter to `FMaterial`, near the other `IsUsedWith...` methods:

```cpp
virtual bool IsUsedWithSoftwareRTCapture() const { return false; }
```

Default false preserves behavior for other FMaterial implementations.

Add the override to `FMaterialResource`:

```cpp
ENGINE_API virtual bool IsUsedWithSoftwareRTCapture() const override;
```

### `Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp`

Implement the resource getter using the base UMaterial:

```cpp
bool FMaterialResource::IsUsedWithSoftwareRTCapture() const
{
    return Material->bUsedWithSoftwareRTCapture;
}
```

This mirrors the existing implementations for Hair Strands, LiDAR Point Clouds and Virtual Heightfield Mesh. Material instances use the same base material resource property; no instance override plumbing is needed.

## 4. Add the flag to FMaterialShaderParameters

In `Engine/Source/Runtime/Engine/Public/MaterialShared.h`, add a bit to the existing `FMaterialShaderParameters::PackedFlags` union:

```cpp
uint64 bIsUsedWithSoftwareRTCapture : 1;
```

Append it after the existing packed flag fields; do not reorder existing fields.

In `FMaterialShaderParameters(const FMaterial* InMaterial)`, assign:

```cpp
bIsUsedWithSoftwareRTCapture = InMaterial->IsUsedWithSoftwareRTCapture();
```

The constructor zeroes the complete structure before setting fields. The new bit must be part of this structure so the material shader-map layout/compatibility identity distinguishes opt-in and non-opt-in materials. This is the key that prevents the capture shader permutation from being treated as globally applicable to all otherwise compatible materials.

Before implementation, confirm the exact layout-cache hash/equality path in the user's 4.27.2 source; the pinned mirror's material shader-map layout construction already consumes `FMaterialShaderParameters`.

## 5. Gate the capture shader permutation

In the capture VS/PS shader classes' `ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)`, require all of:

- `Parameters.MaterialParameters.bIsUsedWithSoftwareRTCapture` is true;
- `Parameters.VertexFactoryType` is exactly the Software RT capture VF type;
- supported shader platform;
- Surface material domain;
- Opaque blend mode for the first prototype;
- no tessellation;
- the other baseline ABI restrictions from Patches 39–45.

Do not use `bIsSpecialEngineMaterial` as a bypass. The capture pass must not compile for every special engine material.

The new flag should not be added to the VF's primitive compatibility logic, nor to `IsPrimitiveTypeUsageFlag()`.

## 6. Recompile and editor behavior

The pinned UE4.27 implementation of `UMaterial::PostEditChangePropertyInternal()` treats ordinary material property edits as shader-relevant by default:

- cancels outstanding compilation;
- updates cached expression data;
- calls `CacheResourceShadersForRendering(bRegenerateId, ...)`;
- recreates affected component render states;
- propagates data to the material proxy.

Interactive edits defer compilation until the final property change. Therefore the new UPROPERTY participates in the existing material recompilation lifecycle without a custom Slate callback or bespoke serialization.

For scripted/batch opt-in, use the public Editor material update route established in Patch 47, with `FMaterialUpdateContext` and `UpdateMaterialShaderCacheAndTextureReferences()`. Do not directly call the private `CacheResourceShadersForRendering()`.

## 7. Files to change in the eventual C++ patch

1. `Engine/Source/Runtime/Engine/Classes/Materials/Material.h`
   - Add UPROPERTY.
2. `Engine/Source/Runtime/Engine/Private/Materials/Material.cpp`
   - Initialize default false.
3. `Engine/Source/Runtime/Engine/Public/MaterialShared.h`
   - Add FMaterial virtual getter.
   - Add FMaterialResource override declaration.
   - Add packed shader parameter bit and constructor assignment.
4. `Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp`
   - Implement FMaterialResource getter.
5. Capture shader implementation files (not yet created)
   - Gate VS/PS permutations on the new shader parameter.

No change is required to `EMaterialUsage`, `SetUsageByFlag()`, `GetUsageByFlag()`, `SetMaterialUsage()`, `IsPrimitiveTypeUsageFlag()`, or `FMaterialsWithDirtyUsageFlags`.

## 8. Validation checklist before coding

- Confirm `FMaterialShaderParameters` layout hash/equality in the exact UE4.27.2 checkout.
- Confirm the capture shader permutation predicate receives the updated `FMaterialShaderParameters`.
- Confirm UHT accepts the new UPROPERTY in the existing Usage category.
- Confirm the material Details panel displays the property and saves it.
- Toggle the property on one test material and confirm a new shader-map compile occurs.
- Confirm a non-opt-in material does not request or contain the capture VS/PS permutation.
- Confirm a material instance inherits eligibility from its parent base material.
- Confirm cooking includes the opt-in permutation for the target RHI.

This is an API integration audit and implementation contract only. It has not been applied to the user's engine checkout and has not been compiled.
