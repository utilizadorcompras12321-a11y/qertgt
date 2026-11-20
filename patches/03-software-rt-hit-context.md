# Patch 03 — Replace the placeholder with an explicit software-RT hit context

Target: UE 4.27.2 source. Apply after patches 01 and 02, from the engine repository root.

This patch is the engine-side contract for the next integration stage. It does **not** depend on DDGI or its BVH. The caller supplies hit attributes through an SRV; the material-linked compute shader evaluates the opted-in material using the normal `FMaterialShader::SetParameters` path and writes material outputs to an UAV.

## Important correction to Patch 02

Patch 02's test shader creates an empty `FMaterialPixelParameters` and hardcodes UV=(0,0). That is only a shader-compilation smoke test. It is not a usable material evaluator and must not be connected to DDGI.

The hit context below supplies the geometric values needed to initialize the material parameter struct. It intentionally does not claim support for arbitrary material graphs yet: explicit texture gradients, primitive/object data, WPO-consistent BVH geometry, virtual textures, and material expressions that depend on raster-only state still need dedicated work.

## 1. Define a stable hit-input layout

New file: `Engine/Shaders/Private/SoftwareRTMaterialEvaluation.ush`

```hlsl
#ifndef SOFTWARE_RT_MATERIAL_EVALUATION_USH
#define SOFTWARE_RT_MATERIAL_EVALUATION_USH

struct FSoftwareRTMaterialHit
{
    float3 WorldPosition;
    float  HitDistance;

    float3 GeometricNormal;
    float  FrontFaceSign;

    float3 WorldTangent;
    float  TangentHandedness;

    float2 TexCoord0;
    float2 TexCoord0DDX;
    float2 TexCoord0DDY;

    float3 CameraVector;
    uint   PrimitiveId;
};

struct FSoftwareRTMaterialResult
{
    float3 BaseColor;
    float  OpacityMask;

    float3 Emissive;
    float  Metallic;

    float3 WorldNormal;
    float  Roughness;
};

#endif
```

The C++ producer must use the exact same 64-byte-compatible field ordering and packing. Do not upload a native C++ struct until its size and offsets are checked with static assertions. The first integration can use a structured buffer of packed float4 elements if the RHI layout is uncertain.

## 2. Initialize material parameters from the hit

In `Engine/Shaders/Private/SoftwareRTMaterialEvaluation.usf`, replace the empty parameter initialization in Patch 02 with:

```hlsl
#include "/Engine/Private/SoftwareRTMaterialEvaluation.ush"
#include "/Engine/Generated/Material.ush"

StructuredBuffer<FSoftwareRTMaterialHit> HitInput;
RWStructuredBuffer<FSoftwareRTMaterialResult> MaterialOutput;

[numthreads(64, 1, 1)]
void MainCS(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint HitIndex = DispatchThreadId.x;
    FSoftwareRTMaterialHit Hit = HitInput[HitIndex];

    FMaterialPixelParameters Parameters = MakeInitializedMaterialPixelParameters();

    Parameters.AbsoluteWorldPosition = Hit.WorldPosition;
    Parameters.WorldPosition_CamRelative = Hit.WorldPosition + ResolvedView.PreViewTranslation;
    Parameters.WorldPosition_NoOffsets = Hit.WorldPosition;
    Parameters.WorldPosition_NoOffsets_CamRelative = Parameters.WorldPosition_CamRelative;
    Parameters.WorldNormal = normalize(Hit.GeometricNormal);
    Parameters.WorldTangent = normalize(Hit.WorldTangent);
    Parameters.CameraVector = normalize(Hit.CameraVector);
    Parameters.TwoSidedSign = Hit.FrontFaceSign;
    Parameters.PrimitiveId = Hit.PrimitiveId;

#if NUM_TEX_COORD_INTERPOLATORS > 0
    Parameters.TexCoords[0] = Hit.TexCoord0;
#endif

    FSoftwareRTMaterialResult Result;
    Result.BaseColor = GetMaterialBaseColorForCS(Parameters);
    Result.Emissive = GetMaterialEmissiveForCS(Parameters);
    Result.Metallic = 0.0f;
    Result.Roughness = 0.5f;
    Result.OpacityMask = 1.0f;
    Result.WorldNormal = Parameters.WorldNormal;

    MaterialOutput[HitIndex] = Result;
}
```

This is an intermediate contract, not the final property implementation. Patch 02 only generates Base Color and Emissive. The remaining outputs must be added to the translator and material template before this result structure is considered complete. Do not treat the placeholder defaults above as actual graph evaluation.

## 3. Bind the hit SRV as a material shader parameter

In `Engine/Source/Runtime/Renderer/Private/SoftwareRTMaterialEvaluation.h`, add a shader resource parameter for `HitInput` and a UAV parameter for `MaterialOutput`. Keep the shader derived from `FMaterialShader`; do not change it to `FGlobalShader`.

The dispatch owner must:
- obtain the material's rendering-thread shader map;
- retrieve `FSoftwareRTMaterialEvaluationCS` through `FMaterialShaderMap::GetShader<FSoftwareRTMaterialEvaluationCS>()`;
- bind the material proxy with `FMaterialShader::SetParameters`, passing a real `FSceneView`;
- bind `HitInput` and `MaterialOutput`;
- dispatch and unbind resources.

Do not manually bind the Material uniform buffer or parameter-collection buffers. `FMaterialShader::SetParameters` already evaluates and binds the material uniform-expression cache for the supplied `FMaterialRenderProxy`.

## 4. Texture gradients are a separate required change

The two gradient fields in the hit layout are not consumed by Patch 02. UE 4.27.2's current non-pixel `TextureSample` translator path forces `TMVM_MipLevel` and defaults the mip to zero. Merely adding DDX/DDY values to the hit struct does not fix this.

Before calling this production-ready, change the compute material translation path so texture samples use explicit gradients derived from the hit footprint, and pass those gradients to the generated sample code. Keep the existing pixel path unchanged. Do not use `ddx`/`ddy` in compute shaders and do not silently fall back to mip 0 for ordinary texture samples.

## 5. Scope and acceptance criteria

This patch establishes the data contract only. It is not a claim that all UE material graphs are supported.

The next engine patch must implement and test, in this order:
1. Base Color, Emissive, Metallic and Roughness graph outputs.
2. Opacity Mask and Two Sided for visibility.
3. Tangent-space Normal mapped into world space.
4. Explicit texture gradients and correct mip selection.
5. Material Instance parameter updates without shader recompilation.
6. WPO/BVH geometry agreement and primitive/object data.

A valid first runtime test supplies synthetic hit records for one ordinary textured material and one Material Instance, dispatches the material-linked compute shader, and reads back output values. Only after that passes should the DDGI BVH feed real hit records.

## Status

- Patch 01: material usage opt-in flag.
- Patch 02: material-linked compute shader registration and initial Base Color/Emissive compilation path.
- Patch 03: hit-input contract and integration requirements described here.
- None of these patches has been applied to the user's local engine or compiled in this workflow.
