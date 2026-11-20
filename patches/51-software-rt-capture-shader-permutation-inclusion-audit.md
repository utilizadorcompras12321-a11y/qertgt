# 51 — Software RT Capture Shader Permutation Inclusion Audit

Status: source audit / implementation contract. No Engine source changed, applied, or compiled.

## Scope

Determine how the Patch 50 material opt-in becomes an actual material shader-map dependency, and establish the registration and permutation gates for the future Software RT capture VS/PS and capture Vertex Factory.

Mirror audited: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

Relevant source:
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialShader.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp`
- `Engine/Source/Runtime/Engine/Public/MaterialShared.h`
- `Engine/Source/Runtime/RenderCore/Private/VertexFactory.cpp`
- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`
- `Engine/Source/Runtime/Renderer/Private/AnisotropyRendering.cpp`

## Findings

### 1. The opt-in bit is only an input to layout selection

`FMaterial::GetDependentShaderAndVFTypes()` constructs `FMaterialShaderParameters`, calls `AcquireMaterialShaderMapLayout()`, and exports the shader and VF dependencies selected by that layout.

`FMaterialShaderMapLayoutCache::CreateLayout()`:
1. Iterates registered material shader types and evaluates each `ShouldCompilePermutation(Platform, MaterialParameters, ...)`.
2. Iterates `FVertexFactoryType::GetSortedMaterialTypes()`.
3. Calls `FMeshMaterialShaderType::ShouldCompileVertexFactoryPermutation(...)` for each VF.
4. For an accepted VF, evaluates each mesh-material shader type's `ShouldCompilePermutation(..., VertexFactoryType, ...)`.
5. Adds the VF to a mesh layout only when at least one shader permutation is selected.

Therefore Patch 50's `bIsUsedWithSoftwareRTCapture` bit does not independently register a capture permutation. A capture mesh shader type must exist and its predicate must select the opt-in materials for the capture VF.

### 2. Required capture shader predicate

The future capture VS and PS should each use a predicate equivalent in intent to:

- capture material opt-in is true;
- material domain is `MD_Surface`;
- blend mode is `BLEND_Opaque`;
- tessellation is disabled;
- the requested Vertex Factory is exactly the Software RT capture VF;
- platform/feature constraints required by the capture shader are met.

The exact C++ member access must be checked against the actual `FMaterialShaderPermutationParameters` / `FMeshMaterialShaderPermutationParameters` definitions when implementing the shader classes. Do not guess a field name or cast to `FMaterial`.

The two shader predicates must agree. If only one stage is selected, the processor cannot retrieve a complete VS/PS pair.

### 3. Capture VF registration is an engine-startup requirement

`FVertexFactoryType` constructor:
- checks the virtual shader filename is a valid `.ush` path;
- checks that shader-type serialization history has not already been initialized;
- links the type into the global VF list;
- adds material-capable VFs to the sorted material VF list.

The source comment explicitly states that VF types must be implemented in modules loaded before `InitializeShaderTypes`; otherwise registration fails. It recommends loading the module at `ELoadingPhase::PostConfigInit`.

Consequences:
- The capture VF type must be statically registered in an appropriately early-loaded module.
- Registering it lazily when the renderer first requests capture is invalid.
- The capture shader classes and VF registration must be available before material shader-map layouts are generated.
- Adding the type after the material layout cache has been populated is not a supported way to retrofit existing maps.

### 4. VF must be material-capable

The capture VF registration must set `bUsedWithMaterials=true`, so it is present in `FVertexFactoryType::GetSortedMaterialTypes()`.

Its `ShouldCache` callback must reject unsupported platform/feature combinations but must not reject the opted-in capture material itself. The material-specific opt-in belongs in the mesh-material capture shader predicates, not in a global VF callback that cannot distinguish a material asset.

Do not advertise capabilities that the first implementation does not implement:
- no mesh draw command caching for the initial dynamic capture path;
- no primitive-ID stream unless a real stream is supplied;
- no static/dynamic lighting capability unless required and implemented;
- no tessellation support.

The exact macro arguments and shader-parameter registration must be reconciled with the final standalone VF implementation before code is written.

### 5. Shader-map and DDC behavior

The material layout is selected from the packed `FMaterialShaderParameters` and the registered shader/VF types. The selected shader and VF dependencies are then included in the material shader map ID and its derived-data key.

The Patch 50 bit therefore changes layout selection for opt-in materials only when the capture shader types and capture VF are registered and their predicates select them. Do not add a second custom DDC key field or manually append a duplicate dependency string.

The capture VF type must be registered before the layout cache first evaluates material permutations. After integrating the new registration, restart the Editor and rebuild the affected material shader maps; do not expect an already-running process or finalized shader map to discover a newly registered VF.

### 6. Runtime lookup contract

The renderer must treat missing capture permutations as a normal unsupported/not-yet-built case:
- resolve the material resource and capture VF type;
- request the capture VS and PS from that material's shader map;
- if either stage is unavailable, skip capture for that material and emit a useful diagnostic in development builds;
- never use an unchecked `GetShader()` assumption for an opt-in material.

Do not attempt to compile arbitrary missing permutations from the render path. Editor rebuild/cook must generate the permutations ahead of use.

## Decision

Patch 51 remains an audit/contract document. It does not add shader classes, VF registration, processor code, or shader files.

Implementation order:
1. Verify the Patch 50 opt-in getter and packed bit in the actual UE4.27.2 checkout.
2. Add the standalone capture VF and register it in an early-loaded module.
3. Add dedicated capture VS/PS material shader classes with matching opt-in predicates.
4. Verify shader-map layout inclusion for an opted-in material and exclusion for a non-opted-in material.
5. Only then implement processor lookup and capture draw submission.

## Required validation before calling this compile-ready

- UBT confirms the module containing the VF is loaded before shader type initialization.
- Startup has no late-VF registration assertion.
- An opted-in opaque Surface material's shader map contains both capture stages for the capture VF.
- The same material with opt-in disabled does not contain those permutations.
- Masked, translucent, non-Surface, and tessellated materials are excluded.
- A non-opted material and a missing permutation are skipped safely at runtime.
- DDC/shader map rebuild after toggling the opt-in produces the expected dependency set.
- Compile and runtime validation are performed in the user's UE4.27.2 checkout, not inferred from the mirror.

## Important limitation

All source conclusions above are from the pinned public UE4.27 mirror. The user's `C:\Users\Utilizador~\Desktop\UnrealEngine` checkout may contain local changes; reconcile the exact declarations and module loading phase there before applying implementation code.
