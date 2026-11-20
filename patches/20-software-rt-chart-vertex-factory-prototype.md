# 20 — Software RT chart vertex factory: first C++/HLSL prototype

Status: proposed engine-source patch specification, audited against the UE4.27 mirror. Not applied to the user's checkout and not compiled.

## Decision

Put the new vertex-factory type and its shader-parameter implementation in the Engine module, not Renderer.

Reason: UE4.27 declares `FLocalVertexFactoryShaderParametersBase` in the public LocalVertexFactory header, but the class and its helper methods are not exported with `ENGINE_API`. A Renderer-module implementation that calls `GetElementShaderBindingsBase` risks a Windows linker failure. The smallest safe route is to implement the derived parameter class in Engine and export only the new VF API needed by Renderer.

Do not change the stock `FLocalVertexFactory` behavior.

## Files to change

1. `Engine/Source/Runtime/Engine/Public/LocalVertexFactory.h`
2. New `Engine/Source/Runtime/Engine/Public/SoftwareRTChartVertexFactory.h`
3. New `Engine/Source/Runtime/Engine/Private/SoftwareRTChartVertexFactory.cpp`
4. `Engine/Shaders/Private/LocalVertexFactory.ush`

No Base Pass shader modification is needed: UE4.27 already calls `VertexFactoryGetRasterizedWorldPosition` from `BasePassVertexShader.usf`.

## 1. Export the shared local-VF parameter helper

In `LocalVertexFactory.h`, change:

```cpp
class FLocalVertexFactoryShaderParametersBase : public FVertexFactoryShaderParameters
```

to:

```cpp
class ENGINE_API FLocalVertexFactoryShaderParametersBase : public FVertexFactoryShaderParameters
```

This is an export annotation only. Do not change its behavior or layout.

## 2. New public VF declaration

Create `SoftwareRTChartVertexFactory.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "LocalVertexFactory.h"

struct FSoftwareRTChartDrawParameters
{
    // Three affine rows. Input is the translated-world position received
    // by VertexFactoryGetRasterizedWorldPosition.
    FVector4 Row0;
    FVector4 Row1;
    FVector4 Row2;
};

class ENGINE_API FSoftwareRTChartVertexFactory : public FLocalVertexFactory
{
    DECLARE_VERTEX_FACTORY_TYPE(FSoftwareRTChartVertexFactory);

public:
    explicit FSoftwareRTChartVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
        : FLocalVertexFactory(InFeatureLevel, "SoftwareRTChartVertexFactory")
    {
    }

    static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
    static void ModifyCompilationEnvironment(
        const FVertexFactoryShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment);
    static void ValidateCompiledResult(
        const FVertexFactoryType* Type,
        EShaderPlatform Platform,
        const FShaderParameterMap& ParameterMap,
        TArray<FString>& OutErrors);
};

class FSoftwareRTChartVertexFactoryShaderParameters
    : public FLocalVertexFactoryShaderParametersBase
{
    DECLARE_TYPE_LAYOUT(FSoftwareRTChartVertexFactoryShaderParameters, NonVirtual);

public:
    void Bind(const FShaderParameterMap& ParameterMap);

    void GetElementShaderBindings(
        const FSceneInterface* Scene,
        const FSceneView* View,
        const FMeshMaterialShader* Shader,
        EVertexInputStreamType InputStreamType,
        ERHIFeatureLevel::Type FeatureLevel,
        const FVertexFactory* VertexFactory,
        const FMeshBatchElement& BatchElement,
        FMeshDrawSingleShaderBindings& ShaderBindings,
        FVertexInputStreamArray& VertexStreams) const;

private:
    LAYOUT_FIELD(FShaderParameter, ChartRasterRow0);
    LAYOUT_FIELD(FShaderParameter, ChartRasterRow1);
    LAYOUT_FIELD(FShaderParameter, ChartRasterRow2);
};
```

The new VF inherits the complete static-mesh vertex declaration and data setup. It does not add a chart vertex stream.

## 3. New parameter binding implementation

Create `SoftwareRTChartVertexFactory.cpp`:

```cpp
#include "SoftwareRTChartVertexFactory.h"
#include "MeshBatch.h"
#include "MeshMaterialShader.h"

IMPLEMENT_TYPE_LAYOUT(FSoftwareRTChartVertexFactoryShaderParameters);

void FSoftwareRTChartVertexFactoryShaderParameters::Bind(
    const FShaderParameterMap& ParameterMap)
{
    FLocalVertexFactoryShaderParametersBase::Bind(ParameterMap);

    ChartRasterRow0.Bind(ParameterMap, TEXT("SoftwareRTChartRow0"), SPF_MANDATORY);
    ChartRasterRow1.Bind(ParameterMap, TEXT("SoftwareRTChartRow1"), SPF_MANDATORY);
    ChartRasterRow2.Bind(ParameterMap, TEXT("SoftwareRTChartRow2"), SPF_MANDATORY);
}

void FSoftwareRTChartVertexFactoryShaderParameters::GetElementShaderBindings(
    const FSceneInterface* Scene,
    const FSceneView* View,
    const FMeshMaterialShader* Shader,
    EVertexInputStreamType InputStreamType,
    ERHIFeatureLevel::Type FeatureLevel,
    const FVertexFactory* VertexFactory,
    const FMeshBatchElement& BatchElement,
    FMeshDrawSingleShaderBindings& ShaderBindings,
    FVertexInputStreamArray& VertexStreams) const
{
    // Preserve the standard Local VF uniform-buffer and stream bindings.
    const FRHIUniformBuffer* LocalVFUniformBuffer =
        static_cast<const FRHIUniformBuffer*>(BatchElement.VertexFactoryUserData);

    FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase(
        Scene,
        View,
        Shader,
        InputStreamType,
        FeatureLevel,
        VertexFactory,
        BatchElement,
        const_cast<FRHIUniformBuffer*>(LocalVFUniformBuffer),
        ShaderBindings,
        VertexStreams);

    // UserData is a separate, capture-owned per-draw payload.
    const FSoftwareRTChartDrawParameters* DrawParameters =
        static_cast<const FSoftwareRTChartDrawParameters*>(BatchElement.UserData);

    check(DrawParameters);

    ShaderBindings.Add(ChartRasterRow0, DrawParameters->Row0);
    ShaderBindings.Add(ChartRasterRow1, DrawParameters->Row1);
    ShaderBindings.Add(ChartRasterRow2, DrawParameters->Row2);
}

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
    OutEnvironment.SetDefine(TEXT("SOFTWARE_RT_CHART_CAPTURE"), TEXT("1"));
}

void FSoftwareRTChartVertexFactory::ValidateCompiledResult(
    const FVertexFactoryType* Type,
    EShaderPlatform Platform,
    const FShaderParameterMap& ParameterMap,
    TArray<FString>& OutErrors)
{
    FLocalVertexFactory::ValidateCompiledResult(Type, Platform, ParameterMap, OutErrors);
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(
    FSoftwareRTChartVertexFactory,
    SF_Vertex,
    FSoftwareRTChartVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE_EX(
    FSoftwareRTChartVertexFactory,
    "/Engine/Private/LocalVertexFactory.ush",
    true, true, true, true, true, true, true);
```

Important implementation notes:

- The cast from `VertexFactoryUserData` deliberately preserves the stock Local VF contract. It is not the chart payload.
- The chart payload is read from `BatchElement.UserData`, following the same broad per-draw pattern used by UE4.27's spline mesh VF.
- The capture draw path must keep this payload alive until all corresponding mesh draw commands have executed.
- Do not cache mesh draw commands that retain this transient pointer. For the prototype, use a non-cached/dynamic capture draw path and render-thread-owned per-frame storage.
- If the eventual renderer cannot guarantee those lifetime rules, replace the pointer with a renderer-owned uniform-buffer resource before enabling command caching.
- Verify that `FMeshDrawSingleShaderBindings::Add(FShaderParameter, FVector4)` is accepted by the exact 4.27 headers in the target checkout. The mirror's existing spline VF demonstrates the same scalar-array binding mechanism with `FVector4` arrays.

## 4. HLSL raster-position override

In `LocalVertexFactory.ush`, declare the three parameters near the existing VF-specific global parameters:

```hlsl
#if SOFTWARE_RT_CHART_CAPTURE
float4 SoftwareRTChartRow0;
float4 SoftwareRTChartRow1;
float4 SoftwareRTChartRow2;
#endif
```

Replace the existing default raster-position function:

```hlsl
float4 VertexFactoryGetRasterizedWorldPosition(
    FVertexFactoryInput Input,
    FVertexFactoryIntermediates Intermediates,
    float4 InWorldPosition)
{
#if SOFTWARE_RT_CHART_CAPTURE
    return float4(
        dot(SoftwareRTChartRow0, InWorldPosition),
        dot(SoftwareRTChartRow1, InWorldPosition),
        dot(SoftwareRTChartRow2, InWorldPosition),
        1.0f);
#else
    return InWorldPosition;
#endif
}
```

Do not alter `VertexFactoryGetWorldPosition`, `VertexFactoryGetInterpolantsVSToPS`, tangent reconstruction, UV decoding, or material parameter generation.

The chart rows must be constructed for translated-world input and must output coordinates that, after the capture view's `TranslatedWorldToClip`, land on the desired chart triangle. Do not feed normalized atlas UVs directly as if they were world positions.

## 5. Draw-data contract

For the prototype, each draw contains exactly one source triangle.

- `BatchElement.UserData` points to one immutable `FSoftwareRTChartDrawParameters`.
- `BatchElement.VertexFactoryUserData` remains reserved for the Local VF uniform-buffer override.
- The capture path uses the original mesh vertex/index buffers and a draw range selecting exactly one source triangle.
- Do not mutate shared static-mesh index buffers.
- The draw-data allocation must outlive command recording and execution. A stack-local struct is invalid.
- Disable mesh draw-command caching for these draws until a stable resource-binding mechanism is implemented.

This patch does not yet provide the chart transform builder, capture view setup, or triangle draw submission. Those are the next independent pieces and must not be hidden inside the VF.

## 6. Constraints and stop conditions

- This is a capture-only VF. Do not register it as the default VF for static meshes.
- The normal Local VF must compile with the original default raster-position behavior.
- First test opaque, deferred, non-tessellated static mesh material only.
- WPO remains disabled because the Software RT BVH does not yet share displaced geometry.
- Reject masked, translucent, PDO, tessellation, view-dependent expressions, and unsupported material permutations for this prototype.
- Do not claim the new VF works until the target checkout compiles shader permutations and a one-triangle render test passes.

## 7. Exact next validation

Before applying this patch to the engine, check in the target checkout:

1. Whether `FMeshDrawSingleShaderBindings::Add` accepts the FVector4 values in this overload.
2. Whether adding `ENGINE_API` to `FLocalVertexFactoryShaderParametersBase` exports its methods as expected under the project's Windows build.
3. Whether the chart VF can be instantiated and initialized with the original static-mesh `FDataType`.
4. Whether the material shader compiler sees `SOFTWARE_RT_CHART_CAPTURE` for this VF type.
5. Whether dynamic mesh commands preserve `BatchElement.UserData` through execution without command caching.

If any of 1–5 fails, stop and correct that issue before implementing the capture pass.
