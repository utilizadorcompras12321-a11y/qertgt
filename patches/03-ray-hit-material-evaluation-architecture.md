# Patch 03 — Generic ray-hit material evaluation architecture (supersedes 01/02)

Target: UE 4.27.2 source. This is the architecture baseline for the Engine implementation.

## Status of patches 01 and 02

Patches 01 and 02 are prototypes and MUST NOT be applied as the implementation baseline.

- Patch 01 introduces a plugin-named usage flag. The final capability must be named and owned by the Engine as a generic material usage, not by DDGI or a plugin.
- Patch 02 compiles only Base Color and Emissive, then evaluates them with a zero-initialized pixel parameter struct and UV=(0,0). That is not ray-hit material evaluation.
- Neither patch provides a ray-hit context, reliable texture footprint, visibility evaluation, nor a reusable Engine-side evaluation contract.

## Goal

Add a native Engine capability that lets any software ray tracing implementation evaluate the original Unreal Material Graph at a ray hit. The Engine owns graph compilation, material shader-map integration, uniform expressions, texture/sampler bindings, and evaluation semantics. DDGI, software shadows, software reflections, and future systems are consumers only.

No dependency on DDGI, a BVH implementation, a plugin module, or a particular ray tracing algorithm is permitted in Runtime/Engine or Runtime/Renderer.

## Runtime contract

The public contract is conceptually:

```cpp
enum class EMaterialRayHitEvaluation : uint8
{
    Visibility,  // Opacity Mask / two-sided visibility decisions
    Surface,     // Base Color, Metallic, Specular, Roughness, Normal, Tangent
    FullSurface  // Surface + Emissive, AO, and other supported surface outputs
};

struct FMaterialRayHitContext
{
    FVector3f WorldPosition;
    FVector3f GeometricNormal;
    FVector3f ShadingNormal;
    FVector3f WorldTangent;
    FVector3f WorldBitangent;
    FVector3f RayDirection;
    FVector4f VertexColor;
    FVector2f TexCoords[/* engine-supported material UV count */];
    FVector2f TexCoordDDX[/* same count */];
    FVector2f TexCoordDDY[/* same count */];
    float TwoSidedSign;
};
```

This is a contract sketch, not a committed C++ ABI. The exact UE4.27 types, UV storage, alignment, serialization, and shader parameter transport must be verified before defining the final public struct.

The hit context must be supplied by the consumer because the Engine cannot infer triangle attributes, barycentrics, UVs, or ray differentials without knowing the consumer's geometry representation. The Engine must not own a BVH or assume one particular acceleration structure.

## Material evaluation semantics

1. Evaluate the original graph, not a duplicate material or a reduced graph authored by the user.
2. Reuse the material's shader map and uniform-expression set. Material Instance scalar/vector/texture parameters must resolve through the existing render proxy and uniform-expression cache.
3. Convert the ray-hit context into the material evaluation parameter structure. Do not call `MakeInitializedMaterialPixelParameters()` and leave its geometry fields at zero.
4. Keep visibility evaluation separate from full surface evaluation. A masked hit must be rejectable without evaluating Base Color, Normal, Roughness, and Emissive.
5. Texture samples must use explicit ray-hit UV gradients/footprints for mip selection. Compute-shader DDX/DDY returning zero is not a valid fallback for ray hits.
6. Keep pixel/view-only expressions explicit. Where a graph node fundamentally requires raster pixel derivatives, scene textures, or unavailable per-view state, compilation must report a clear unsupported-context error or use a documented caller-supplied view context. Never silently substitute fabricated values.
7. WPO is a geometry/acceleration-structure concern: evaluating WPO after an intersection cannot repair a hit against un-deformed geometry. It must be handled by the geometry producer or explicitly marked unsupported for a given consumer.
8. Translucency is not equivalent to Opacity Mask. It requires a separate transmission/accumulation contract and is not part of the first visibility milestone.

## Shader compilation and caching

The compute material variant must be part of the normal material shader-map lifecycle and cache identity. The profile (Visibility, Surface, FullSurface) must participate in the permutation/cache key. Material Instances must reuse the base material's compiled graph while binding their own uniform-expression values.

Do not blindly compile every profile for every material. The Engine needs an explicit, generic eligibility/request mechanism. A generic material usage capability is acceptable if it follows existing UE material usage conventions, but it must not mention software RT consumers by name and must not force all projects to compile every compute variant. The exact mechanism must be settled against the existing shader-map compilation flow before adding a UPROPERTY or a custom shader-map side cache.

Important source facts already verified in the UE4.27.2 dump:

- `FHLSLMaterialTranslator::Translate()` currently sets `bCompileForComputeShader = Material->IsLightFunction()`.
- Compute translation currently compiles only `CompiledMP_EmissiveColorCS`.
- `FHLSLMaterialTranslator::TextureSample()` forces every non-pixel sample to `TMVM_MipLevel`, disables automatic view mip bias, and defaults a missing mip input to zero.
- `DDX()` and `DDY()` return zero for `SF_Compute`.
- `FMaterialPixelParameters` contains geometry, view, and pixel-specific members. A ray-hit adapter must initialize every member that the selected profile can read; using the zero initializer alone is invalid.
- `FMaterialShader::SetParameters()` already binds the material uniform buffer and parameter collection buffers from the render proxy's uniform-expression cache. It currently requires an `FSceneView`; view-independent and view-dependent evaluation need to be distinguished rather than bypassing uniform expressions.
- `FMaterialShaderType` supports `SF_Compute`, and material shader types are registered in the material shader map. The existing `FMaterialShaders::Shaders[SF_NumGraphicsFrequencies]` arrays are graphics-only, so the compute shader must use the generic material shader-type lookup path, not be forced into those arrays.

## Implementation sequence

### 1. Compiler contract
- Introduce a generic ray-hit compute compilation mode, distinct from the existing Light Function compute mode.
- Compile the requested material properties into compute-frequency functions.
- Make the requested evaluation profile part of the material shader permutation/cache identity.
- Preserve existing Light Function behavior and its current generated entrypoint.

### 2. Hit-context adapter
- Define the HLSL ray-hit context and a conversion routine that populates the material parameter structure used by existing generated graph code.
- Populate world position, geometric/shading normal, tangent basis, camera/ray vectors where available, vertex color, UVs, two-sided sign, and explicit texture gradients.
- Do not pretend scene textures, screen coordinates, or pixel derivatives exist in a ray hit.

### 3. Texture footprint
- Add a compute ray-hit texture-sampling path that consumes explicit gradients.
- Do not globally change existing compute material sampling semantics, since Light Functions and other compute users may rely on mip-level behavior.
- Audit 2D, cube, array, volume, virtual, external, and custom sampler paths; initially support only paths whose semantics can be implemented correctly and issue compile errors for the rest.

### 4. Engine evaluation interface
- Provide a generic Engine/Renderer material evaluation entry point taking a material render proxy, material resource, hit context, profile, and destination/output representation.
- Bind material uniform expressions through the existing material shader machinery. Do not reimplement parameter collection or texture parameter binding in a plugin.
- Keep GPU dispatch scheduling and hit-buffer layout owned by the consumer; the Engine API should not assume a BVH or a particular ray batching strategy.

### 5. First validation
A small test must demonstrate all of the following before integrating DDGI:
- A material with a texture sample evaluates the expected texel at supplied UVs and gradients.
- A Material Instance returns its overridden scalar/vector/texture parameter values.
- A masked material correctly rejects/accepts hits through the Visibility profile.
- Surface and FullSurface profiles return the expected outputs.
- A graph using unsupported pixel-only expressions fails clearly rather than returning plausible but incorrect values.

## Non-goals for the first milestone

- Implementing or owning a BVH.
- Building a new material editor or a parallel material asset format.
- Rewriting the Material Graph as a runtime interpreter.
- Supporting translucent multi-hit transport.
- Claiming arbitrary Material Expression compatibility before an expression-by-expression audit.
- Integrating DDGI, shadows, or reflections before the Engine contract is validated.

## Files already inspected

- `MaterialShared.h/.cpp`
- `HLSLMaterialTranslator.cpp`
- `MaterialTemplate.ush`
- `MaterialShader.h`
- `MaterialShaderType.h`
- `MaterialShader.cpp`
- `ShaderBaseClasses.cpp`
- `MaterialInstance.h/.cpp`

This document is the architecture baseline only. It does not claim that the runtime interface or shader implementation has been applied or compiled.
