# 21 — Register the experimental chart vertex factory (UE4.27)

Status: proposed source patch. Not applied to an Unreal Engine checkout and not compiled.

Mirror audited: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

This patch is deliberately only a registration/compilation proof. It does **not** yet alter raster position, bind per-draw data, create chart buffers, or submit capture draws.

## Why this shape

UE4.27 already contains two useful precedents:
- `FGeometryCollectionVertexFactory : public FLocalVertexFactory`, with its own `DECLARE_VERTEX_FACTORY_TYPE`, parameter class and type registration.
- `FGeometryCollectionVertexFactoryShaderParameters`, which calls `FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase` and supplies the normal Local VF uniform buffer.

The experimental VF follows that supported pattern. It does not subclass or modify the global Local VF shader parameter implementation.

## Files to add

### `Engine/Source/Runtime/Engine/Public/SoftwareRTChartVertexFactory.h`

```cpp
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "LocalVertexFactory.h"

/**
 * Experimental Local VF derivative for Software RT chart capture.
 * This first version intentionally has no chart-specific vertex attributes
 * or draw parameters. It exists only to prove VF registration and Base Pass
 * material shader compilation.
 */
class FSoftwareRTChartVertexFactoryShaderParameters
    : public FLocalVertexFactoryShaderParameters
{
    DECLARE_TYPE_LAYOUT(FSoftwareRTChartVertexFactoryShaderParameters, NonVirtual);

public:
    void Bind(const FShaderParameterMap& ParameterMap)
    {
        FLocalVertexFactoryShaderParameters::Bind(ParameterMap);
    }

    void GetElementShaderBindings(
        const FSceneInterface* Scene,
        const FSceneView* View,
        const FMeshMaterialShader* Shader,
        const EVertexInputStreamType InputStreamType,
        ERHIFeatureLevel::Type FeatureLevel,
        const FVertexFactory* VertexFactory,
        const FMeshBatchElement& BatchElement,
        FMeshDrawSingleShaderBindings& ShaderBindings,
        FVertexInputStreamArray& VertexStreams) const;
};

class ENGINE_API FSoftwareRTChartVertexFactory : public FLocalVertexFactory
{
    DECLARE_VERTEX_FACTORY_TYPE(FSoftwareRTChartVertexFactory);

public:
    FSoftwareRTChartVertexFactory(
        ERHIFeatureLevel::Type InFeatureLevel,
        const char* InDebugName = "FSoftwareRTChartVertexFactory")
        : FLocalVertexFactory(InFeatureLevel, InDebugName)
    {
    }

    static bool ShouldCompilePermutation(
        const FVertexFactoryShaderPermutationParameters& Parameters);

    static void ModifyCompilationEnvironment(
        const FVertexFactoryShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment);

    static void ValidateCompiledResult(
        const FVertexFactoryType* Type,
        EShaderPlatform Platform,
        const FShaderParameterMap& ParameterMap,
        TArray<FString>& OutErrors);
};
```

### `Engine/Source/Runtime/Engine/Private/SoftwareRTChartVertexFactory.cpp`

```cpp
// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftwareRTChartVertexFactory.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"

IMPLEMENT_TYPE_LAYOUT(FSoftwareRTChartVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(
    FSoftwareRTChartVertexFactory,
    SF_Vertex,
    FSoftwareRTChartVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE_EX(
    FSoftwareRTChartVertexFactory,
    "/Engine/Private/SoftwareRTChartVertexFactory.ush",
    true,   // bUsedWithMaterials
    true,   // bSupportsStaticLighting
    true,   // bSupportsDynamicLighting
    true,   // bPrecisePrevWorldPos
    true,   // bSupportsPositionOnly
    false,  // bSupportsCachingMeshDrawCommands: disabled for this proof
    false); // bSupportsPrimitiveIdStream

bool FSoftwareRTChartVertexFactory::ShouldCompilePermutation(
    const FVertexFactoryShaderPermutationParameters& Parameters)
{
    return FLocalVertexFactory::ShouldCompilePermutation(Parameters);
}

void FSoftwareRTChartVertexFactory::ModifyCompilationEnvironment(
    const FVertexFactoryShaderPermutationParameters& Parameters,
    FShaderCompilerEnvironment& OutEnvironment)
{
    FLocalVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    OutEnvironment.SetDefine(TEXT("SOFTWARE_RT_CHART_VF"), TEXT("1"));
}

void FSoftwareRTChartVertexFactory::ValidateCompiledResult(
    const FVertexFactoryType* Type,
    EShaderPlatform Platform,
    const FShaderParameterMap& ParameterMap,
    TArray<FString>& OutErrors)
{
    FLocalVertexFactory::ValidateCompiledResult(Type, Platform, ParameterMap, OutErrors);
}

void FSoftwareRTChartVertexFactoryShaderParameters::GetElementShaderBindings(
    const FSceneInterface* Scene,
    const FSceneView* View,
    const FMeshMaterialShader* Shader,
    const EVertexInputStreamType InputStreamType,
    ERHIFeatureLevel::Type FeatureLevel,
    const FVertexFactory* VertexFactory,
    const FMeshBatchElement& BatchElement,
    FMeshDrawSingleShaderBindings& ShaderBindings,
    FVertexInputStreamArray& VertexStreams) const
{
    check(VertexFactory);
    check(VertexFactory->GetType() == &FSoftwareRTChartVertexFactory::StaticType);

    const FSoftwareRTChartVertexFactory* ChartVF =
        static_cast<const FSoftwareRTChartVertexFactory*>(VertexFactory);

    // This VF has no per-draw uniform yet. Supply the normal Local VF UB
    // explicitly; do not decode VertexFactoryUserData as chart data.
    FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase(
        Scene,
        View,
        Shader,
        InputStreamType,
        FeatureLevel,
        VertexFactory,
        BatchElement,
        ChartVF->GetUniformBuffer(),
        ShaderBindings,
        VertexStreams);
}
```

### `Engine/Shaders/Private/SoftwareRTChartVertexFactory.ush`

```hlsl
// Copyright Epic Games, Inc. All Rights Reserved.

// Separate shader source identity for the experimental chart VF.
// Keep this wrapper behavior-identical to LocalVertexFactory until the
// chart raster-position permutation is implemented and tested.
#define SOFTWARE_RT_CHART_VF 1
#include "/Engine/Private/LocalVertexFactory.ush"
```

## Required integration

Add `SoftwareRTChartVertexFactory.cpp` to the existing Engine module source tree; UE's normal module source discovery should include it. No Build.cs dependency is expected because this is inside Runtime/Engine and uses existing Engine/RenderCore dependencies.

Do not change `LocalVertexFactory.ush` in this patch. The wrapper defines a distinct VF shader source identity but currently includes the stock implementation unchanged.

## Deliberate constraints

- The VF is not instantiated or used by any scene proxy in this patch.
- No capture mesh processor is added.
- No per-draw transform or chart ID exists yet.
- Mesh draw command caching is disabled in the VF type registration for this experimental path. This does not by itself prove that every caller will submit uncached commands; the eventual capture path must explicitly enforce that.
- The initial registration advertises the same broad material/static/dynamic lighting capabilities as the stock Local VF. This can cause additional shader permutations. If shader compile cost is excessive, the next step is a dedicated material usage gate, not weakening the VF contract silently.
- Position-only support is retained only to match the base VF capability. The capture proof must use the regular material Base Pass vertex shader, not a depth-only permutation.

## Acceptance test

1. Build Engine/Renderer target with no existing Local VF source edits.
2. Confirm shader compiler registers `FSoftwareRTChartVertexFactory` and resolves `/Engine/Private/SoftwareRTChartVertexFactory.ush`.
3. Force-compile one opaque material Base Pass permutation against the chart VF in a controlled test harness.
4. Confirm the same material continues compiling against `FLocalVertexFactory`.
5. Confirm the chart VF uses the inherited Local VF vertex declaration/streams and its parameter binding supplies the Local VF uniform buffer.
6. Confirm no `VertexFactoryUserData` reinterpretation occurs in the chart parameter path.

## Stop condition

If UE4.27 rejects the wrapper include, type registration, or Base Pass material permutation, stop here and fix registration. Do not add chart transforms or capture scheduling until this proof passes.

No compile or runtime success is claimed by this patch.

## UE4.27 API audit update

- `IMPLEMENT_VERTEX_FACTORY_TYPE_EX` takes the seven boolean capability arguments used above, in the documented order.
- `FLocalVertexFactory::ValidateCompiledResult` is declared and defined as a public static function with the signature used above.
- `FLocalVertexFactoryShaderParameters::Bind` is inherited publicly from `FLocalVertexFactoryShaderParametersBase`.
- `FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase` accepts the Local VF uniform buffer used above.
- `FLocalVertexFactory::GetUniformBuffer()` returns `FRHIUniformBuffer*`.
- UE4.27's Geometry Collection VF uses the same parameter-class derivation and registration pattern.

One declaration was tightened after this audit: `FSoftwareRTChartVertexFactoryShaderParameters` is intentionally **not** marked `ENGINE_API`. It is an Engine-module implementation detail and derives from the non-exported `FLocalVertexFactoryShaderParameters`; exporting the derived parameter class is unnecessary and can trigger Windows DLL-interface warnings.

Still unproven: shader compilation of the wrapper include and actual Base Pass selection/draw submission. This audit is source-signature validation only, not a compile result.
