# 51 — Software RT capture shader permutation gate audit

Status: API audit and implementation contract only. No Engine source is changed by this document.

Reference engine:
- Repository: `AlexMercer-MA/UnrealEngine-4.27`
- Pinned commit: `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`

## Objective

Define the exact compile-time gate for the future Software RT Surface Cache material capture VS/PS. The gate must make the new `UMaterial::bUsedWithSoftwareRTCapture` opt-in meaningful without compiling capture permutations for unrelated materials or vertex factories.

This patch does not create shader classes because the capture VF and shader source/registration are not yet implemented. Adding a predicate to a non-existent class would be fabricated code.

## Verified UE4.27 API path

### 1. Material opt-in reaches the permutation parameters

`Engine/Source/Runtime/Engine/Public/MaterialShared.h` defines `FMaterialShaderParameters`. Its constructor:
- zero-initializes the complete structure with `FMemory::Memzero(*this)`;
- copies material domain, blend mode and tessellation mode from `FMaterial`;
- copies all usage flags through material getters.

Patch 50 adds `bIsUsedWithSoftwareRTCapture` to this structure and initializes it using `InMaterial->IsUsedWithSoftwareRTCapture()`.

`Engine/Source/Runtime/Renderer/Public/MaterialShader.h` defines:

```cpp
struct FMaterialShaderPermutationParameters : public FShaderPermutationParameters
{
    FMaterialShaderParameters MaterialParameters;
};
```

`Engine/Source/Runtime/Renderer/Public/MeshMaterialShader.h` defines `FMeshMaterialShaderPermutationParameters`, which inherits that structure and adds:

```cpp
const FVertexFactoryType* VertexFactoryType;
```

Therefore a future mesh-material capture shader's `ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)` can read both the material opt-in and the exact VF type. No extra global lookup or UMaterial asset lookup is required.

### 2. The layout builder actually evaluates this predicate

`Engine/Source/Runtime/Engine/Private/Materials/MaterialShader.cpp`, `FMaterialShaderMapLayoutCache::CreateLayout()`:
- iterates registered material shader types;
- iterates registered material mesh shader types;
- iterates `FVertexFactoryType::GetSortedMaterialTypes()`;
- calls `FMeshMaterialShaderType::ShouldCompileVertexFactoryPermutation(...)`;
- calls each mesh shader type's `ShouldCompilePermutation(Platform, MaterialParameters, VertexFactoryType, ...)`;
- includes the shader/VF combination only when the predicates return true.

`FMeshMaterialShaderType::ShouldCompilePermutation()` in `Engine/Source/Runtime/Engine/Private/Materials/MeshMaterialShader.cpp` constructs `FMeshMaterialShaderPermutationParameters` and dispatches to the registered shader type's predicate.

This confirms the intended gate is evaluated during material shader-map layout construction, not at draw time.

### 3. Precedent for the predicate shape

`Engine/Source/Runtime/Renderer/Private/AnisotropyRendering.cpp` uses a mesh material shader predicate that checks material/platform compatibility and then delegates to `FMeshMaterialShader::ShouldCompilePermutation(Parameters)`.

`Engine/Source/Runtime/Renderer/Private/VT/RuntimeVirtualTextureRender.cpp` uses `Parameters.MaterialParameters` to gate material permutations. Its RVT-specific conditions must not be copied.

## Required capture shader gate

For each future capture VS/PS class, the shared eligibility rule must require:

1. `Parameters.MaterialParameters.bIsUsedWithSoftwareRTCapture` is true.
2. `Parameters.VertexFactoryType` is exactly the registered Software RT capture VF type, not merely any Local VF or any VF supporting materials.
3. `Parameters.MaterialParameters.MaterialDomain == MD_Surface`.
4. `Parameters.MaterialParameters.BlendMode == BLEND_Opaque`.
5. `Parameters.MaterialParameters.TessellationMode == MTM_NoTessellation`.
6. The base `FMeshMaterialShader::ShouldCompilePermutation(Parameters)` returns true.

The eventual implementation should centralize these checks in one shared helper or make the PS delegate to the VS eligibility predicate, following UE4.27's existing mesh-shader patterns. Avoid subtly divergent VS and PS predicates.

Do not add an SM5-only restriction solely by copying Anisotropy's hardware gate. The project target is broad software-RT compatibility; platform restrictions should be based on the capture VF's actual vertex declaration, shader model and MRT format requirements, audited separately.

## Material/VF registration implications

- The capture VF must be registered as a material-capable VF before material shader-map layouts are created.
- The capture VS/PS must be registered as mesh material shader types.
- Both shader classes must reject every VF except the dedicated capture VF.
- The capture VF's own `ShouldCache` must also permit the opted-in material only if the VF contract requires that restriction. Do not rely on the VF gate alone: shader predicates must enforce the opt-in too.
- Registering the VF or shader types after material shader-map layout caches have been created is not a supported hot-add path. Integrate registrations before initial material shader-map layout creation; restart the editor after engine integration.
- Existing material shader maps are not retroactively populated by merely adding a shader class. Rebuild/cook opt-in materials after registration.

## What this does not solve

- No capture VS/PS class or shader source exists yet.
- No dedicated capture VF registration or VF `ShouldCache` implementation exists yet.
- No capture shader source hash / DDC / cook validation has been run in the user's checkout.
- No shader compile or engine build has been run.
- This does not implement selective material recompilation. That remains a separate step after the registration path exists.

## Decision

Patch 50's opt-in field is exposed through the correct UE4.27 material permutation parameter path. The future shader gate can consume it directly as `Parameters.MaterialParameters.bIsUsedWithSoftwareRTCapture`.

Do not add another property, enum, material lookup, global registry, or custom DDC key for this purpose. The next implementation step should create the dedicated capture VF and shader type registrations, then apply the gate above to those real shader classes.
