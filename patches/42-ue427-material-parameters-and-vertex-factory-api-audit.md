# 42 — UE4.27 material-parameter and Vertex Factory API audit

Status: source audit against the pinned public mirror. No Engine source was changed, no shader was compiled, and no runtime test was performed.

Mirror: `AlexMercer-MA/UnrealEngine-4.27`
Commit: `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`

## 1. Important correction to Patch 39

Patch 39's illustrative PS sequence is internally inconsistent: it explicitly calls `CalcPixelMaterialInputs()` and then calls `CalcMaterialParametersEx()`.

In the audited UE4.27 `MaterialTemplate.ush`, `CalcMaterialParametersEx()` itself ends by calling `CalcPixelMaterialInputs(Parameters, PixelMaterialInputs)`.

Therefore the capture PS must call `CalcMaterialParametersEx()` exactly once and must NOT call `CalcPixelMaterialInputs()` separately. This is a real double-evaluation bug in the Patch 39 example, now explicitly superseded by this correction.

Exact audited signature:

```hlsl
void CalcMaterialParametersEx(
    in out FMaterialPixelParameters Parameters,
    in out FPixelMaterialInputs PixelMaterialInputs,
    float4 SvPosition,
    float4 ScreenPosition,
    FIsFrontFace bIsFrontFace,
    float3 TranslatedWorldPosition,
    float3 TranslatedWorldPositionExcludingShaderOffsets)
```

The function sets:
- `WorldPosition_CamRelative` from the supplied translated world position;
- `AbsoluteWorldPosition` by subtracting `ResolvedView.PreViewTranslation`;
- optional no-offset world positions from the second supplied position;
- `SvPosition` and `ScreenPosition`;
- `CameraVector` from the translated world position (outside ray-hit-group shaders);
- two-sided sign and virtual texture feedback as configured;
- and finally evaluates the Material Graph through `CalcPixelMaterialInputs()`.

Consequences:
- Supplying the original interpolated surface translated-world position is sufficient for the world-position fields; do not reconstruct it from chart `SV_POSITION`.
- `SvPosition` and `ScreenPosition` still describe the synthetic atlas raster domain. Therefore ScreenPosition, SceneTexture, PixelDepth, viewport/pixel-position and view-dependent expressions remain unsupported in the first capture baseline.
- CameraVector is still derived from the active view origin and supplied surface position. It is not a chart-independent material attribute. Exclude CameraVector/view-dependent expressions in the first baseline.

## 2. Pixel material parameter initialization

Audited `MaterialTemplate.ush` defines:

```hlsl
FMaterialPixelParameters MakeInitializedMaterialPixelParameters()
{
    FMaterialPixelParameters MPP;
    MPP = (FMaterialPixelParameters)0;
    MPP.TangentToWorld = float3x3(1,0,0,0,1,0,0,0,1);
    return MPP;
}
```

The audited Geometry Cache and Point Cloud vertex factory shader examples implement:

```hlsl
FMaterialPixelParameters GetMaterialPixelParameters(
    FVertexFactoryInterpolantsVSToPS Interpolants,
    float4 SvPosition)
```

They begin with `MakeInitializedMaterialPixelParameters()`, then explicitly populate the fields their VF supports.

For the capture VF, the minimum required fields for the selected baseline are:
- `TexCoords[]` from the original material UV interpolants;
- `VertexColor` when enabled;
- `TangentToWorld` reconstructed from the original interpolated tangent basis;
- `UnMirrored` where the material shader path expects it;
- `TwoSidedSign` initialized consistently with the supported one-sided baseline.

The original translated-world position is supplied later to `CalcMaterialParametersEx()`, which sets `AbsoluteWorldPosition` and `WorldPosition_CamRelative`. Do not try to initialize these from chart `SV_POSITION`.

Do not assume a partial custom `FMaterialPixelParameters` initializer is valid for every material permutation. The generated material shader may reference additional fields when feature defines are enabled. Keep the initial compile permutation narrow and compare the custom function against the exact target VF implementations.

## 3. Tangent frame and handedness precedent

The audited `GeometryCacheVertexFactory.ush` provides a useful UE4.27 precedent:
- `TangentZ.w` carries the tangent basis determinant sign;
- tangent Y is reconstructed as `cross(TangentZ.xyz, TangentX) * TangentZ.w`;
- `AssembleTangentToWorld()` reconstructs the third basis vector using the handedness sign;
- `CalcTangentToWorld()` incorporates `Primitive.InvNonUniformScaleAndDeterminantSign`.

The capture VF must follow the same convention rather than inventing a new tangent sign rule. Exact packed vertex decoding must match the C++ vertex declaration's normalized/packed element types. Mirrored and non-uniformly scaled primitives remain an explicit acceptance test.

## 4. Vertex Factory C++ API facts

Audited header: `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`.

Confirmed:
- `FVertexStreamComponent` stores buffer pointer, stream offset, attribute offset, stride, element type and usage.
- Its constructor checks that stride and attribute offset fit in 8 bits. This limits the maximum interleaved struct stride to 255 bytes when represented by this helper.
- `FVertexInputStreamArray` is the stream list returned by VF shader parameter bindings.
- `FVertexFactoryShaderParameters::GetElementShaderBindings()` is the type-layout dispatched binding interface.
- `IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FactoryClass, Frequency, ParameterClass)` registers the per-frequency parameter type and binding callback.
- `FVertexFactoryType` declares whether a VF supports materials, static/dynamic lighting, precise previous position, position-only rendering, mesh draw command caching and primitive-ID streams.
- `FVertexFactoryType::ModifyCompilationEnvironment()` installs the VF shader source as `/Engine/Generated/VertexFactory.ush` and defines `HAS_PRIMITIVE_UNIFORM_BUFFER`.
- The custom VF must provide its own declaration and stream bindings; a shader include alone does not bind vertex buffers.

Audited examples:
- `FVectorFieldVisualizationVertexFactory` creates an explicit declaration with `FVertexElement`, sets a stream in `InitRHI()`, calls `SetDeclaration()`, and registers a custom VF shader parameter class.
- `FParticleSpriteVertexFactory` demonstrates multiple vertex streams and explicit per-frequency VF parameter registration.
- `GeometryCacheVertexFactory.ush` demonstrates a material-capable custom VF shader ABI and the expected material-facing helper functions.

The generic `FVertexFactory` implementation and the exact `IMPLEMENT_VERTEX_FACTORY_TYPE` macro overload still need to be checked in the user's actual 4.27.2 checkout before final C++ is authored. Do not copy a UE5 macro signature.

## 5. Corrected capture PS sequence

The conceptual sequence is now:

```hlsl
ResolvedView = ResolveView();

FMaterialPixelParameters MaterialParameters =
    GetMaterialPixelParameters(Interpolants, In.SvPosition);

FPixelMaterialInputs PixelMaterialInputs = (FPixelMaterialInputs)0;

float4 ScreenPosition = /* explicit atlas-domain value; not scene-camera reconstruction */;

float3 SurfaceTranslatedWorldPosition =
    Interpolants.SurfaceTranslatedWorldPosition;

CalcMaterialParametersEx(
    MaterialParameters,
    PixelMaterialInputs,
    In.SvPosition,
    ScreenPosition,
    In.bIsFrontFace,
    SurfaceTranslatedWorldPosition,
    SurfaceTranslatedWorldPosition);
```

This is still conceptual, not compile-ready: the interpolant type and the atlas-domain ScreenPosition policy must be implemented and validated. Critically, there is no explicit `CalcPixelMaterialInputs()` call.

The initial baseline should reject material permutations that use ScreenPosition, CameraVector, SceneTexture, PixelDepth, view-dependent nodes, VT feedback, WPO/PDO or other features that assume the scene camera raster domain.

## 6. Scope correction: this is not yet a general Material Graph evaluator

Even after the chart VF compiles, the first working version will only correctly evaluate a restricted material subset:
- Opaque;
- Default Lit;
- no WPO/PDO;
- no view-dependent/screen-space expressions;
- no custom interpolators;
- no virtual texture feedback;
- no masked/translucent blend modes;
- no unsupported vertex color overrides or missing CPU source buffers.

This is deliberate. A general Material Graph can depend on the active view, screen coordinates, pixel depth, derivatives, virtual texture feedback and other raster context. A chart atlas raster pass does not reproduce the player's camera context. Do not advertise the prototype as supporting arbitrary materials.

## 7. Acceptance gate before code

Before writing the debug VF:
1. inspect the user's exact UE4.27.2 `VertexFactory.h`, `MeshMaterialShader.h`, and target VF shader files;
2. confirm `IMPLEMENT_VERTEX_FACTORY_TYPE` argument list and registration flags;
3. confirm the VF base-class stream/declaration lifecycle and draw-command stream binding requirements;
4. implement one interleaved vertex stream with a stride below 256 bytes, or deliberately use multiple streams;
5. compile a debug VS/PS permutation and render one triangle;
6. verify chart raster independence from camera movement and preservation of original translated-world position.

Do not integrate the material shader processor until this gate passes.

## 8. Audited source files

- `Engine/Shaders/Private/MaterialTemplate.ush`
- `Engine/Shaders/Private/GeometryCacheVertexFactory.ush`
- `Engine/Shaders/Private/PointCloudVertexFactory.ush`
- `Engine/Shaders/Private/VirtualTextureMaterial.usf`
- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`
- `Engine/Source/Runtime/Engine/Public/LocalVertexFactory.h`
- `Engine/Source/Runtime/Engine/Private/VectorFieldVisualization.cpp`
- `Engine/Source/Runtime/Engine/Private/Particles/ParticleVertexFactory.cpp`
