# 46 — Material Shader-Map Inclusion and VS/PS Capture Processor Audit

**Status:** source audit and implementation contract; not an applied engine patch.  
**Engine reference:** `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.  
**Target:** UE 4.27.2, Software RT Surface Cache, dedicated capture VF + dedicated `FMeshMaterialShader` VS/PS.

## 1. Critical finding: registering a VF does not retrofit existing material shader maps

The relevant code is in:

- `Engine/Source/Runtime/Engine/Private/Materials/MaterialShader.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp`
- `Engine/Source/Runtime/Engine/Public/MaterialShared.h`

During material shader-map layout construction, `FMaterialShaderMapLayoutCache::CreateLayout()` iterates:

```cpp
for (FVertexFactoryType* VertexFactoryType : FVertexFactoryType::GetSortedMaterialTypes())
```

For each material-compatible VF type it evaluates `FMeshMaterialShaderType::ShouldCompileVertexFactoryPermutation()`, then iterates registered mesh-material shader types and their permutations. The resulting mesh shader-map layout is part of the material shader-map layout/hash.

Consequences:

1. The capture VF type must be registered before the relevant material shader maps are compiled.
2. A material shader map compiled before the new VF was registered cannot be assumed to contain the capture VF's mesh shader map.
3. Registering the VF later does not mutate already-finalized material shader maps. Existing materials need a supported recompile/rebuild path so their shader-map layout is regenerated.
4. The material shader type must also be registered and its `ShouldCompilePermutation` must accept the capture VF and intended material/platform restrictions.
5. The VF must report `IsUsedWithMaterials()` through its type registration; `FMaterialShaderMap::ProcessCompilationResultsForSingleJob()` asserts this for mesh-material jobs.

This is a startup/cook integration requirement, not something to patch around in the capture processor.

## 2. Runtime lookup is not a reliable shader-compilation strategy

`FMaterial::TryGetShaders()` in `MaterialShared.cpp`:

- chooses the Game Thread or Rendering Thread shader map according to the calling thread;
- looks up the mesh shader map for the supplied VF type;
- returns the shader refs if all requested stages are present;
- if a stage is missing, can submit compile jobs only when the shader map is not complete and a non-zero compiling shader-map ID is available;
- returns false when the requested shaders are missing.

Therefore:

- Use `TryGetShaders()` for a non-fatal availability check in the capture processor.
- Do not use `GetShader()` as a probe: its default missing-shader behavior is fatal.
- Do not expect a finalized editor shader map to compile arbitrary missing capture permutations merely because the processor asks for them.
- In cooked/runtime builds, do not depend on runtime material shader compilation. The required permutations must be present in cooked shader maps. ODSC requests are a separate on-demand shader-compilation path and are not a substitute for ensuring the feature's supported permutations are cooked.

## 3. Smallest verified VS/PS mesh-material processor precedent

The closest minimal precedent is the Runtime Virtual Texture mesh processor:

`Engine/Source/Runtime/Renderer/Private/VT/RuntimeVirtualTextureRender.cpp`

Its material draw uses:

```cpp
TMeshProcessorShaders<
    FShader_VirtualTextureMaterialDraw_VS<MaterialPolicy>,
    FBaseHS,
    FBaseDS,
    FShader_VirtualTextureMaterialDraw_PS<MaterialPolicy>
> Shaders;
```

It retrieves the VS and PS from the material shader map using the batch VF type, initializes `FMeshMaterialShaderElementData`, and calls `BuildMeshDrawCommands()` with the regular material proxy/resource, pass render state, fill/cull modes, sort key, and shader element data.

This is a direct proof that a custom material mesh pass can use a dedicated VS/PS pair while supplying the standard base HS/DS placeholder types. The placeholder stages are not populated for the non-tessellated path. The capture material shader's `ShouldCompilePermutation` must reject tessellation for the first baseline, and the capture processor must reject materials with tessellation enabled.

The RVT processor is a stronger precedent than copying the Base Pass: it is a dedicated `FMeshPassProcessor`, uses a material shader map and material bindings, and writes a pass-specific output.

## 4. Verified draw-command contract

The public declaration in `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` confirms:

```cpp
template<typename VertexType, typename HullType, typename DomainType,
         typename PixelType, typename GeometryType = FMeshMaterialShader,
         typename RayHitGroupType = FMeshMaterialShader,
         typename ComputeType = FMeshMaterialShader>
struct TMeshProcessorShaders;
```

The same header declares `BuildMeshDrawCommands()` with these inputs:

- `FMeshBatch` and element mask;
- optional `FPrimitiveSceneProxy`;
- material render proxy and resolved `FMaterial`;
- `FMeshPassProcessorRenderState`;
- typed pass shader bundle;
- fill and cull modes;
- `FMeshDrawCommandSortKey`;
- `EMeshPassFeatures`;
- shader element data.

The RVT implementation confirms the practical call pattern. `FMeshPassProcessor` also exposes `ComputeMeshOverrideSettings()`, `ComputeMeshFillMode()`, and `ComputeMeshCullMode()`; use these rather than inventing raster-state derivation.

For the first chart-capture processor:

- Use `TMeshProcessorShaders<CaptureVS, FBaseHS, FBaseDS, CapturePS>`.
- Leave HS/DS refs unset and reject tessellated materials.
- Use `TryGetShaders()`; if unavailable, skip the draw and emit a diagnostic rather than calling fatal `GetShader()`.
- Initialize `FMeshMaterialShaderElementData` with the existing view/batch/proxy contract established in Patch 44.
- Build the command with explicit opaque capture blend/depth state and the capture pass's private MRTs already bound by the enclosing RDG raster pass.
- Keep the capture pass uncached initially. The custom expanded-geometry resources and chart mapping must have stable lifetime, and Patch 26–33 retirement rules still apply.

## 5. Recommended compile/cook integration

Do not add a per-frame or per-draw shader compilation mechanism.

Required sequence:

1. Register the capture VF type and capture material VS/PS shader types during module startup, before material shader-map layout generation.
2. Ensure the VF is a material VF type and that the material shader classes' permutation predicates accept only the intended capture VF, shader platform and baseline material restrictions.
3. Trigger a normal material shader-map rebuild/recompile for materials that need Surface Cache capture, so the layout includes the newly registered VF permutations.
4. Ensure cook/build shader compilation includes those permutations for every target RHI/platform the feature supports.
5. At runtime, query with `TryGetShaders()`. If unavailable, skip capture for that material and report the missing permutation; do not crash or compile synchronously in the render pass.

A blanket forced recompile of every project material is not yet justified. The next implementation audit should identify the narrowest supported material recompilation/invalidation route for UE4.27.2 and whether the project can mark eligible materials for capture permutation generation without destabilizing normal material caching.

## 6. Baseline restrictions remain

The first working capture draw should continue to reject:

- tessellation;
- WPO/PDO;
- masked and translucent blend modes;
- view/screen-dependent expressions and SceneTexture/PixelDepth;
- custom interpolators and unsupported vertex-color/override paths;
- virtual-texture feedback and other features not explicitly supported by the capture ABI.

Do not silently substitute a fallback material for a missing capture permutation. A fallback would produce incorrect cached surface attributes while appearing to succeed.

## 7. Evidence and files inspected

Pinned source files:

- `Engine/Source/Runtime/Engine/Private/Materials/MaterialShader.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp`
- `Engine/Source/Runtime/Engine/Public/MaterialShared.h`
- `Engine/Source/Runtime/Renderer/Private/VT/RuntimeVirtualTextureRender.cpp`
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`
- `Engine/Source/Runtime/Renderer/Private/DistortionRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/AnisotropyRendering.cpp`

This document is an API/source audit only. It has not been applied to the user's UE checkout and has not been compiled.
