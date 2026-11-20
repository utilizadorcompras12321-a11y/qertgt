#include "DDGIRadianceCascadeRHIViewExtension.h"

#include "DDGIMasterSubsystem.h"
#include "DDGIRadianceCascadeResources.h"

#include "GlobalShader.h"
#include "ShaderParameterUtils.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "SceneInterface.h"
#include "ScenePrivate.h"
#include "DeferredShadingRenderer.h"
#include "PostProcess/SceneRenderTargets.h"
#include "SceneView.h"
#include "HAL/IConsoleManager.h"
#include "ScreenPass.h"

#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"

static TAutoConsoleVariable<int32> CVarDDGIUsePrecomputedProbeNeighbors(
    TEXT("r.DDGI.UsePrecomputedProbeNeighbors"),
    1,
    TEXT("Use precomputed probe neighbors in DDGI spatial gather (0=spatial hash, 1=precomputed list; default 1)."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarDDGIDebugApply(
    TEXT("r.DDGI.DebugApply"),
    0,
    TEXT("DDGI apply debug view (0=off, 1=probe coverage, 2=raw irradiance, 3=solid magenta, 4-7=spatial-level irradiance, 8=clipmap blend weights, 9=clipmap support)."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarDDGITraceProbesPerFrame(
    TEXT("r.DDGI.TraceProbesPerFrame"),
    128,
    TEXT("Number of DDGI probes retraced per frame for dynamic lighting updates (default 128)."),
    ECVF_RenderThreadSafe);


static void DDGITransitionTexture(
	FRHICommandList& RHICmdList,
	FRHITexture* Texture,
	ERHIAccess AccessBefore,
	ERHIAccess AccessAfter)
{
	if (!Texture) return;
	FRHITransitionInfo TransitionInfo(Texture, AccessBefore, AccessAfter);
	RHICmdList.Transition(MakeArrayView(&TransitionInfo, 1));
}

class FDDGIRadianceCascadeInitializeRHI : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDDGIRadianceCascadeInitializeRHI, Global);

public:
	FDDGIRadianceCascadeInitializeRHI() {}

	explicit FDDGIRadianceCascadeInitializeRHI(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		TotalRaysPerProbe.Bind(Initializer.ParameterMap, TEXT("TotalRaysPerProbe"));
		CascadeCount.Bind(Initializer.ParameterMap, TEXT("CascadeCount"));
		BaseRayCount.Bind(Initializer.ParameterMap, TEXT("BaseRayCount"));
		TextureWidth.Bind(Initializer.ParameterMap, TEXT("TextureWidth"));
		TotalTexels.Bind(Initializer.ParameterMap, TEXT("TotalTexels"));
		DispatchGroupsX.Bind(Initializer.ParameterMap, TEXT("DispatchGroupsX"));
		ProbeCount.Bind(Initializer.ParameterMap, TEXT("ProbeCount"));
		DirectionsUAV.Bind(Initializer.ParameterMap, TEXT("DirectionsUAV"));
		RadianceUAV.Bind(Initializer.ParameterMap, TEXT("RadianceUAV"));
		HitDistanceUAV.Bind(Initializer.ParameterMap, TEXT("HitDistanceUAV"));
		RadianceScratchUAV.Bind(Initializer.ParameterMap, TEXT("RadianceScratchUAV"));
		RadianceIntegrationScratchUAV.Bind(Initializer.ParameterMap, TEXT("RadianceIntegrationScratchUAV"));
		HitTriangleIndexUAV.Bind(Initializer.ParameterMap, TEXT("HitTriangleIndexUAV"));
		NewPhysicalSlots.Bind(Initializer.ParameterMap, TEXT("NewPhysicalSlots"));
		bInitializeDirections.Bind(Initializer.ParameterMap, TEXT("bInitializeDirections"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	template<typename TShaderRHIParamRef>
	void SetParameters(
		FRHICommandList& RHICmdList,
		const TShaderRHIParamRef ShaderRHI,
		FUnorderedAccessViewRHIRef DirectionsView,
		FUnorderedAccessViewRHIRef RadianceView,
		FUnorderedAccessViewRHIRef HitDistanceView,
		FUnorderedAccessViewRHIRef RadianceScratchView,
		FUnorderedAccessViewRHIRef RadianceIntegrationScratchView,
		FUnorderedAccessViewRHIRef HitTriangleIndexView,
		FShaderResourceViewRHIRef InNewPhysicalSlotsSRV,
		uint32 bInInitializeDirections,
		uint32 InTotalRaysPerProbe,
		uint32 InCascadeCount,
		uint32 InBaseRayCount,
		uint32 InProbeCount,
		uint32 InTotalTexels,
		uint32 InDispatchGroupsX)
	{
		const uint32 InTextureWidth = 4096;

		SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeCount, InCascadeCount);
		SetShaderValue(RHICmdList, ShaderRHI, BaseRayCount, InBaseRayCount);
		SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
		SetShaderValue(RHICmdList, ShaderRHI, TotalTexels, InTotalTexels);
		SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeCount, InProbeCount);
		SetShaderValue(RHICmdList, ShaderRHI, bInitializeDirections, bInInitializeDirections);

		if (DirectionsUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, DirectionsUAV.GetBaseIndex(), DirectionsView);
		}
		if (RadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), RadianceView);
		}
		if (HitDistanceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), HitDistanceView);
		}
		if (RadianceScratchUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceScratchUAV.GetBaseIndex(), RadianceScratchView);
		if (RadianceIntegrationScratchUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceIntegrationScratchUAV.GetBaseIndex(), RadianceIntegrationScratchView);
		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), HitTriangleIndexView);
		if (NewPhysicalSlots.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, NewPhysicalSlots.GetBaseIndex(), InNewPhysicalSlotsSRV);
	}

	template<typename TShaderRHIParamRef>
	void UnbindResources(FRHICommandList& RHICmdList, const TShaderRHIParamRef ShaderRHI)
	{
		if (DirectionsUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, DirectionsUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
		if (RadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
		if (HitDistanceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
		if (RadianceScratchUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceScratchUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (RadianceIntegrationScratchUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceIntegrationScratchUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (NewPhysicalSlots.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, NewPhysicalSlots.GetBaseIndex(), FShaderResourceViewRHIRef());
	}

private:
	LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
	LAYOUT_FIELD(FShaderParameter, CascadeCount);
	LAYOUT_FIELD(FShaderParameter, BaseRayCount);
	LAYOUT_FIELD(FShaderParameter, TextureWidth);
	LAYOUT_FIELD(FShaderParameter, TotalTexels);
	LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
	LAYOUT_FIELD(FShaderParameter, ProbeCount);
	LAYOUT_FIELD(FShaderResourceParameter, DirectionsUAV);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceUAV);
	LAYOUT_FIELD(FShaderResourceParameter, HitDistanceUAV);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceScratchUAV);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceIntegrationScratchUAV);
	LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexUAV);
	LAYOUT_FIELD(FShaderResourceParameter, NewPhysicalSlots);
	LAYOUT_FIELD(FShaderParameter, bInitializeDirections);
};

IMPLEMENT_SHADER_TYPE(
	,
	FDDGIRadianceCascadeInitializeRHI,
	TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeInitializeCS.usf"),
	TEXT("RadianceCascadeInitializeCS"),
	SF_Compute);


class FDDGIRadianceCascadeTraceRHI : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDDGIRadianceCascadeTraceRHI, Global);

public:
	FDDGIRadianceCascadeTraceRHI() {}

	explicit FDDGIRadianceCascadeTraceRHI(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		TextureWidth.Bind(Initializer.ParameterMap, TEXT("TextureWidth"));
		TotalTexels.Bind(Initializer.ParameterMap, TEXT("TotalTexels"));
		DispatchGroupsX.Bind(Initializer.ParameterMap, TEXT("DispatchGroupsX"));
		ProbeCount.Bind(Initializer.ParameterMap, TEXT("ProbeCount"));
		TraceStartIndex.Bind(Initializer.ParameterMap, TEXT("TraceStartIndex"));
		TotalRaysPerProbe.Bind(Initializer.ParameterMap, TEXT("TotalRaysPerProbe"));
		NodeCount.Bind(Initializer.ParameterMap, TEXT("NodeCount"));
		TriangleCount.Bind(Initializer.ParameterMap, TEXT("TriangleCount"));
		CascadeCount.Bind(Initializer.ParameterMap, TEXT("CascadeCount"));
		TraceCascadeIndex.Bind(Initializer.ParameterMap, TEXT("TraceCascadeIndex"));
		BaseRayCount.Bind(Initializer.ParameterMap, TEXT("BaseRayCount"));
		BaseSegmentLength.Bind(Initializer.ParameterMap, TEXT("BaseSegmentLength"));
		CascadeScale.Bind(Initializer.ParameterMap, TEXT("CascadeScale"));
		MaxTraceDistance.Bind(Initializer.ParameterMap, TEXT("MaxTraceDistance"));
		StaticBackgroundColor.Bind(Initializer.ParameterMap, TEXT("StaticBackgroundColor"));
		StaticBackgroundIntensity.Bind(Initializer.ParameterMap, TEXT("StaticBackgroundIntensity"));
		GIIntensity.Bind(Initializer.ParameterMap, TEXT("GIIntensity"));
		ActiveWindowCenter.Bind(Initializer.ParameterMap, TEXT("ActiveWindowCenter"));
		BackgroundTransitionStart.Bind(Initializer.ParameterMap, TEXT("BackgroundTransitionStart"));
		BackgroundTransitionEnd.Bind(Initializer.ParameterMap, TEXT("BackgroundTransitionEnd"));
		bEnableStaticBackground.Bind(Initializer.ParameterMap, TEXT("bEnableStaticBackground"));

		DirectionsTexture.Bind(Initializer.ParameterMap, TEXT("DirectionsTexture"));
		ProbePositionsTexture.Bind(Initializer.ParameterMap, TEXT("ProbePositionsTexture"));
		LogicalToPhysicalSlots.Bind(Initializer.ParameterMap, TEXT("LogicalToPhysicalSlots"));
		BVHNodes.Bind(Initializer.ParameterMap, TEXT("BVHNodes"));
		BVHTriangles.Bind(Initializer.ParameterMap, TEXT("BVHTriangles"));
		RadianceUAV.Bind(Initializer.ParameterMap, TEXT("RadianceScratchUAV"));
		HitDistanceUAV.Bind(Initializer.ParameterMap, TEXT("HitDistanceUAV"));
 		HitTriangleIndexUAV.Bind(Initializer.ParameterMap, TEXT("HitTriangleIndexUAV"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	template<typename TShaderRHIParamRef>
	void SetParameters(
		FRHICommandList& RHICmdList,
		const TShaderRHIParamRef ShaderRHI,
		FShaderResourceViewRHIRef DirectionsSRV,
		FShaderResourceViewRHIRef ProbePositionsSRV,
		FShaderResourceViewRHIRef LogicalToPhysicalSRV,
		FShaderResourceViewRHIRef NodesSRV,
		FShaderResourceViewRHIRef TrianglesSRV,
		FUnorderedAccessViewRHIRef RadianceView,
		FUnorderedAccessViewRHIRef HitDistanceView,
		FUnorderedAccessViewRHIRef HitTriangleIndexView,
		uint32 InTraceCascadeIndex,
		uint32 InTotalTexels,
		uint32 InDispatchGroupsX,
		uint32 InProbeCount,
		uint32 InTraceStartIndex,
		uint32 InTotalRaysPerProbe,
		uint32 InNodeCount,
		uint32 InTriangleCount,
		uint32 InCascadeCount,
		uint32 InBaseRayCount,
		float InBaseSegmentLength,
		float InCascadeScale,
		float InMaxTraceDistance,
		const FVector4& InStaticBackgroundColor,
		float InStaticBackgroundIntensity,
		float InGIIntensity,
		const FVector4& InActiveWindowCenter,
		float InBackgroundTransitionStart,
		float InBackgroundTransitionEnd,
		uint32 bInEnableStaticBackground)
	{
		const uint32 InTextureWidth = 4096;
		SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
		SetShaderValue(RHICmdList, ShaderRHI, TotalTexels, InTotalTexels);
		SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeCount, InProbeCount);
		SetShaderValue(RHICmdList, ShaderRHI, TraceStartIndex, InTraceStartIndex);
		SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
		SetShaderValue(RHICmdList, ShaderRHI, NodeCount, InNodeCount);
		SetShaderValue(RHICmdList, ShaderRHI, TriangleCount, InTriangleCount);
 		SetShaderValue(RHICmdList, ShaderRHI, CascadeCount, InCascadeCount);
		SetShaderValue(RHICmdList, ShaderRHI, TraceCascadeIndex, InTraceCascadeIndex);
		SetShaderValue(RHICmdList, ShaderRHI, BaseRayCount, InBaseRayCount);
		SetShaderValue(RHICmdList, ShaderRHI, BaseSegmentLength, InBaseSegmentLength);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeScale, InCascadeScale);
		SetShaderValue(RHICmdList, ShaderRHI, MaxTraceDistance, InMaxTraceDistance);
		SetShaderValue(RHICmdList, ShaderRHI, StaticBackgroundColor, InStaticBackgroundColor);
		SetShaderValue(RHICmdList, ShaderRHI, StaticBackgroundIntensity, InStaticBackgroundIntensity);
		SetShaderValue(RHICmdList, ShaderRHI, GIIntensity, InGIIntensity);
		SetShaderValue(RHICmdList, ShaderRHI, ActiveWindowCenter, InActiveWindowCenter);
		SetShaderValue(RHICmdList, ShaderRHI, BackgroundTransitionStart, InBackgroundTransitionStart);
		SetShaderValue(RHICmdList, ShaderRHI, BackgroundTransitionEnd, InBackgroundTransitionEnd);
		SetShaderValue(RHICmdList, ShaderRHI, bEnableStaticBackground, bInEnableStaticBackground);

		if (DirectionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, DirectionsTexture.GetBaseIndex(), DirectionsSRV);
		if (ProbePositionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, ProbePositionsTexture.GetBaseIndex(), ProbePositionsSRV);
		if (LogicalToPhysicalSlots.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), LogicalToPhysicalSRV);
		if (BVHNodes.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHNodes.GetBaseIndex(), NodesSRV);
		if (BVHTriangles.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHTriangles.GetBaseIndex(), TrianglesSRV);
		if (RadianceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), RadianceView);
		if (HitDistanceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), HitDistanceView);
 		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), HitTriangleIndexView);
	}

	template<typename TShaderRHIParamRef>
	void UnbindResources(FRHICommandList& RHICmdList, const TShaderRHIParamRef ShaderRHI)
	{
		if (DirectionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, DirectionsTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (ProbePositionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, ProbePositionsTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (LogicalToPhysicalSlots.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (BVHNodes.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHNodes.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (BVHTriangles.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHTriangles.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (RadianceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (HitDistanceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
 		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
	}

private:
	LAYOUT_FIELD(FShaderParameter, TextureWidth);
	LAYOUT_FIELD(FShaderParameter, TotalTexels);
	LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
	LAYOUT_FIELD(FShaderParameter, ProbeCount);
	LAYOUT_FIELD(FShaderParameter, TraceStartIndex);
	LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
	LAYOUT_FIELD(FShaderParameter, NodeCount);
	LAYOUT_FIELD(FShaderParameter, TriangleCount);
 	LAYOUT_FIELD(FShaderParameter, CascadeCount);
	LAYOUT_FIELD(FShaderParameter, TraceCascadeIndex);
	LAYOUT_FIELD(FShaderParameter, BaseRayCount);
	LAYOUT_FIELD(FShaderParameter, BaseSegmentLength);
	LAYOUT_FIELD(FShaderParameter, CascadeScale);
	LAYOUT_FIELD(FShaderParameter, MaxTraceDistance);
	LAYOUT_FIELD(FShaderParameter, StaticBackgroundColor);
	LAYOUT_FIELD(FShaderParameter, StaticBackgroundIntensity);
	LAYOUT_FIELD(FShaderParameter, GIIntensity);
	LAYOUT_FIELD(FShaderParameter, ActiveWindowCenter);
	LAYOUT_FIELD(FShaderParameter, BackgroundTransitionStart);
	LAYOUT_FIELD(FShaderParameter, BackgroundTransitionEnd);
	LAYOUT_FIELD(FShaderParameter, bEnableStaticBackground);

	LAYOUT_FIELD(FShaderResourceParameter, DirectionsTexture);
	LAYOUT_FIELD(FShaderResourceParameter, ProbePositionsTexture);
	LAYOUT_FIELD(FShaderResourceParameter, LogicalToPhysicalSlots);
	LAYOUT_FIELD(FShaderResourceParameter, BVHNodes);
	LAYOUT_FIELD(FShaderResourceParameter, BVHTriangles);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceUAV);
	LAYOUT_FIELD(FShaderResourceParameter, HitDistanceUAV);
 	LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexUAV);
};

IMPLEMENT_SHADER_TYPE(
	,
	FDDGIRadianceCascadeTraceRHI,
	TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeTraceCS.usf"),
	TEXT("RadianceCascadeTraceCS"),
	SF_Compute);


class FDDGIRadianceCascadeShadeRHI : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FDDGIRadianceCascadeShadeRHI, Global);
public:
    FDDGIRadianceCascadeShadeRHI() {}
    explicit FDDGIRadianceCascadeShadeRHI(const ShaderMetaType::CompiledShaderInitializerType& I)
        : FGlobalShader(I)
    {
        TextureWidth.Bind(I.ParameterMap, TEXT("TextureWidth"));
        TotalTexels.Bind(I.ParameterMap, TEXT("TotalTexels"));
        DispatchGroupsX.Bind(I.ParameterMap, TEXT("DispatchGroupsX"));
        ProbeCount.Bind(I.ParameterMap, TEXT("ProbeCount"));
        TraceStartIndex.Bind(I.ParameterMap, TEXT("TraceStartIndex"));
        TotalRaysPerProbe.Bind(I.ParameterMap, TEXT("TotalRaysPerProbe"));
        NodeCount.Bind(I.ParameterMap, TEXT("NodeCount"));
        TriangleCount.Bind(I.ParameterMap, TEXT("TriangleCount"));
        GIIntensity.Bind(I.ParameterMap, TEXT("GIIntensity"));
        DirectionalLightDirection.Bind(I.ParameterMap, TEXT("DirectionalLightDirection"));
        DirectionalLightColor.Bind(I.ParameterMap, TEXT("DirectionalLightColor"));
        DirectionalLightIntensity.Bind(I.ParameterMap, TEXT("DirectionalLightIntensity"));
        bHasDirectionalLight.Bind(I.ParameterMap, TEXT("bHasDirectionalLight"));
        bDirectionalLightCastsShadows.Bind(I.ParameterMap, TEXT("bDirectionalLightCastsShadows"));
        LocalLightCount.Bind(I.ParameterMap, TEXT("LocalLightCount"));
        LocalLightPositionAndRadius.Bind(I.ParameterMap, TEXT("LocalLightPositionAndRadius"));
        LocalLightDirectionAndCosOuter.Bind(I.ParameterMap, TEXT("LocalLightDirectionAndCosOuter"));
        LocalLightColorAndIntensity.Bind(I.ParameterMap, TEXT("LocalLightColorAndIntensity"));
        LocalLightParameters.Bind(I.ParameterMap, TEXT("LocalLightParameters"));
        DirectionsTexture.Bind(I.ParameterMap, TEXT("DirectionsTexture"));
        ProbePositionsTexture.Bind(I.ParameterMap, TEXT("ProbePositionsTexture"));
        LogicalToPhysicalSlots.Bind(I.ParameterMap, TEXT("LogicalToPhysicalSlots"));
        BVHNodes.Bind(I.ParameterMap, TEXT("BVHNodes"));
        BVHTriangles.Bind(I.ParameterMap, TEXT("BVHTriangles"));
        HitDistanceTexture.Bind(I.ParameterMap, TEXT("HitDistanceTexture"));
        HitTriangleIndexTexture.Bind(I.ParameterMap, TEXT("HitTriangleIndexTexture"));
        RadianceUAV.Bind(I.ParameterMap, TEXT("RadianceScratchUAV"));
    }
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
    {
        return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
    }
    template<typename TShaderRHIParamRef>
    void SetParameters(
        FRHICommandList& C, const TShaderRHIParamRef Sh,
        FShaderResourceViewRHIRef Directions, FShaderResourceViewRHIRef Positions,
        FShaderResourceViewRHIRef LogicalToPhysical, FShaderResourceViewRHIRef Nodes,
        FShaderResourceViewRHIRef Triangles, FShaderResourceViewRHIRef HitDistances,
        FShaderResourceViewRHIRef HitIndices, FUnorderedAccessViewRHIRef Radiance,
        uint32 InTotalTexels, uint32 InGroupsX, uint32 InProbeCount,
        uint32 InTraceStartIndex, uint32 InTotalRaysPerProbe,
        uint32 InNodeCount, uint32 InTriangleCount, float InGIIntensity,
        const FVector4& InDirectionalDirection, const FVector4& InDirectionalColor,
        float InDirectionalIntensity, uint32 bInHasDirectionalLight,
        uint32 bInDirectionalCastsShadows,
        const TArray<FVector4>& InLocalPositions,
        const TArray<FVector4>& InLocalDirections,
        const TArray<FVector4>& InLocalColors,
        const TArray<FVector4>& InLocalParameters, uint32 InLocalLightCount)
    {
        const uint32 Width = 4096u;
        SetShaderValue(C, Sh, TextureWidth, Width);
        SetShaderValue(C, Sh, TotalTexels, InTotalTexels);
        SetShaderValue(C, Sh, DispatchGroupsX, InGroupsX);
        SetShaderValue(C, Sh, ProbeCount, InProbeCount);
        SetShaderValue(C, Sh, TraceStartIndex, InTraceStartIndex);
        SetShaderValue(C, Sh, TotalRaysPerProbe, InTotalRaysPerProbe);
        SetShaderValue(C, Sh, NodeCount, InNodeCount);
        SetShaderValue(C, Sh, TriangleCount, InTriangleCount);
        SetShaderValue(C, Sh, GIIntensity, InGIIntensity);
        SetShaderValue(C, Sh, DirectionalLightDirection, InDirectionalDirection);
        SetShaderValue(C, Sh, DirectionalLightColor, InDirectionalColor);
        SetShaderValue(C, Sh, DirectionalLightIntensity, InDirectionalIntensity);
        SetShaderValue(C, Sh, bHasDirectionalLight, bInHasDirectionalLight);
        SetShaderValue(C, Sh, bDirectionalLightCastsShadows, bInDirectionalCastsShadows);
        SetShaderValue(C, Sh, LocalLightCount, FMath::Min(InLocalLightCount, 32u));
        check(InLocalPositions.Num() >= 32 && InLocalDirections.Num() >= 32 &&
            InLocalColors.Num() >= 32 && InLocalParameters.Num() >= 32);
        SetShaderValueArray(C, Sh, LocalLightPositionAndRadius, InLocalPositions.GetData(), 32u);
        SetShaderValueArray(C, Sh, LocalLightDirectionAndCosOuter, InLocalDirections.GetData(), 32u);
        SetShaderValueArray(C, Sh, LocalLightColorAndIntensity, InLocalColors.GetData(), 32u);
        SetShaderValueArray(C, Sh, LocalLightParameters, InLocalParameters.GetData(), 32u);
        if (DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, DirectionsTexture.GetBaseIndex(), Directions);
        if (ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, ProbePositionsTexture.GetBaseIndex(), Positions);
        if (LogicalToPhysicalSlots.IsBound()) C.SetShaderResourceViewParameter(Sh, LogicalToPhysicalSlots.GetBaseIndex(), LogicalToPhysical);
        if (BVHNodes.IsBound()) C.SetShaderResourceViewParameter(Sh, BVHNodes.GetBaseIndex(), Nodes);
        if (BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh, BVHTriangles.GetBaseIndex(), Triangles);
        if (HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, HitDistanceTexture.GetBaseIndex(), HitDistances);
        if (HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, HitTriangleIndexTexture.GetBaseIndex(), HitIndices);
        if (RadianceUAV.IsBound()) C.SetUAVParameter(Sh, RadianceUAV.GetBaseIndex(), Radiance);
    }
    template<typename TShaderRHIParamRef>
    void UnbindResources(FRHICommandList& C, const TShaderRHIParamRef Sh)
    {
        if (DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, DirectionsTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, ProbePositionsTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (LogicalToPhysicalSlots.IsBound()) C.SetShaderResourceViewParameter(Sh, LogicalToPhysicalSlots.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (BVHNodes.IsBound()) C.SetShaderResourceViewParameter(Sh, BVHNodes.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh, BVHTriangles.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, HitDistanceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, HitTriangleIndexTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (RadianceUAV.IsBound()) C.SetUAVParameter(Sh, RadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
    }
private:
    LAYOUT_FIELD(FShaderParameter, TextureWidth);
    LAYOUT_FIELD(FShaderParameter, TotalTexels);
    LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
    LAYOUT_FIELD(FShaderParameter, ProbeCount);
    LAYOUT_FIELD(FShaderParameter, TraceStartIndex);
    LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
    LAYOUT_FIELD(FShaderParameter, NodeCount);
    LAYOUT_FIELD(FShaderParameter, TriangleCount);
    LAYOUT_FIELD(FShaderParameter, GIIntensity);
    LAYOUT_FIELD(FShaderParameter, DirectionalLightDirection);
    LAYOUT_FIELD(FShaderParameter, DirectionalLightColor);
    LAYOUT_FIELD(FShaderParameter, DirectionalLightIntensity);
    LAYOUT_FIELD(FShaderParameter, bHasDirectionalLight);
    LAYOUT_FIELD(FShaderParameter, bDirectionalLightCastsShadows);
    LAYOUT_FIELD(FShaderParameter, LocalLightCount);
    LAYOUT_FIELD(FShaderParameter, LocalLightPositionAndRadius);
    LAYOUT_FIELD(FShaderParameter, LocalLightDirectionAndCosOuter);
    LAYOUT_FIELD(FShaderParameter, LocalLightColorAndIntensity);
    LAYOUT_FIELD(FShaderParameter, LocalLightParameters);
    LAYOUT_FIELD(FShaderResourceParameter, DirectionsTexture);
    LAYOUT_FIELD(FShaderResourceParameter, ProbePositionsTexture);
    LAYOUT_FIELD(FShaderResourceParameter, LogicalToPhysicalSlots);
    LAYOUT_FIELD(FShaderResourceParameter, BVHNodes);
    LAYOUT_FIELD(FShaderResourceParameter, BVHTriangles);
    LAYOUT_FIELD(FShaderResourceParameter, HitDistanceTexture);
    LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexTexture);
    LAYOUT_FIELD(FShaderResourceParameter, RadianceUAV);
};
IMPLEMENT_SHADER_TYPE(
    ,
    FDDGIRadianceCascadeShadeRHI,
    TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeShadeCS.usf"),
    TEXT("RadianceCascadeShadeCS"),
    SF_Compute);


class FDDGIRadianceCascadeResolveRHI : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FDDGIRadianceCascadeResolveRHI, Global);
public:
    FDDGIRadianceCascadeResolveRHI() {}
    explicit FDDGIRadianceCascadeResolveRHI(const ShaderMetaType::CompiledShaderInitializerType& I)
        : FGlobalShader(I)
    {
        TextureWidth.Bind(I.ParameterMap, TEXT("TextureWidth"));
        TotalTexels.Bind(I.ParameterMap, TEXT("TotalTexels"));
        DispatchGroupsX.Bind(I.ParameterMap, TEXT("DispatchGroupsX"));
        ProbeCount.Bind(I.ParameterMap, TEXT("ProbeCount"));
        TraceStartIndex.Bind(I.ParameterMap, TEXT("TraceStartIndex"));
        TotalRaysPerProbe.Bind(I.ParameterMap, TEXT("TotalRaysPerProbe"));
        NodeCount.Bind(I.ParameterMap, TEXT("NodeCount"));
        TriangleCount.Bind(I.ParameterMap, TEXT("TriangleCount"));
        MaxTraceDistance.Bind(I.ParameterMap, TEXT("MaxTraceDistance"));
        StaticBackgroundColor.Bind(I.ParameterMap, TEXT("StaticBackgroundColor"));
        StaticBackgroundIntensity.Bind(I.ParameterMap, TEXT("StaticBackgroundIntensity"));
        bEnableStaticBackground.Bind(I.ParameterMap, TEXT("bEnableStaticBackground"));
        GIIntensity.Bind(I.ParameterMap, TEXT("GIIntensity"));
        DirectionalLightDirection.Bind(I.ParameterMap, TEXT("DirectionalLightDirection"));
        DirectionalLightColor.Bind(I.ParameterMap, TEXT("DirectionalLightColor"));
        DirectionalLightIntensity.Bind(I.ParameterMap, TEXT("DirectionalLightIntensity"));
        bHasDirectionalLight.Bind(I.ParameterMap, TEXT("bHasDirectionalLight"));
        LocalLightCount.Bind(I.ParameterMap, TEXT("LocalLightCount"));
        LocalLightPositionAndRadius.Bind(I.ParameterMap, TEXT("LocalLightPositionAndRadius"));
        LocalLightDirectionAndCosOuter.Bind(I.ParameterMap, TEXT("LocalLightDirectionAndCosOuter"));
        LocalLightColorAndIntensity.Bind(I.ParameterMap, TEXT("LocalLightColorAndIntensity"));
        LocalLightParameters.Bind(I.ParameterMap, TEXT("LocalLightParameters"));
        DirectionsTexture.Bind(I.ParameterMap, TEXT("DirectionsTexture"));
        ProbePositionsTexture.Bind(I.ParameterMap, TEXT("ProbePositionsTexture"));
        LogicalToPhysicalSlots.Bind(I.ParameterMap, TEXT("LogicalToPhysicalSlots"));
        BVHNodes.Bind(I.ParameterMap, TEXT("BVHNodes"));
        BVHTriangles.Bind(I.ParameterMap, TEXT("BVHTriangles"));
        HitDistanceTexture.Bind(I.ParameterMap, TEXT("HitDistanceTexture"));
        HitTriangleIndexTexture.Bind(I.ParameterMap, TEXT("HitTriangleIndexTexture"));
        SurfaceRadianceTexture.Bind(I.ParameterMap, TEXT("SurfaceRadianceTexture"));
        RadianceUAV.Bind(I.ParameterMap, TEXT("RadianceUAV"));
    }
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
    {
        return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
    }
    template<typename TShaderRHIParamRef>
    void SetParameters(
        FRHICommandList& C, const TShaderRHIParamRef Sh,
        FShaderResourceViewRHIRef Directions, FShaderResourceViewRHIRef Positions,
        FShaderResourceViewRHIRef LogicalToPhysical, FShaderResourceViewRHIRef Nodes,
        FShaderResourceViewRHIRef Triangles, FShaderResourceViewRHIRef HitDistances,
        FShaderResourceViewRHIRef HitIndices, FShaderResourceViewRHIRef SurfaceRadiance, FUnorderedAccessViewRHIRef Radiance,
        uint32 InTotalTexels, uint32 InGroupsX, uint32 InProbeCount,
        uint32 InTraceStartIndex, uint32 InTotalRaysPerProbe,
        uint32 InNodeCount, uint32 InTriangleCount, float InMaxTraceDistance,
        const FVector4& InBackgroundColor, float InBackgroundIntensity,
        uint32 bInEnableStaticBackground, float InGIIntensity,
        const FVector4& InDirectionalDirection, const FVector4& InDirectionalColor,
        float InDirectionalIntensity, uint32 bInHasDirectionalLight,
        const TArray<FVector4>& InLocalPositions,
        const TArray<FVector4>& InLocalDirections,
        const TArray<FVector4>& InLocalColors,
        const TArray<FVector4>& InLocalParameters, uint32 InLocalLightCount)
    {
        const uint32 Width = 4096u;
        SetShaderValue(C, Sh, TextureWidth, Width);
        SetShaderValue(C, Sh, TotalTexels, InTotalTexels);
        SetShaderValue(C, Sh, DispatchGroupsX, InGroupsX);
        SetShaderValue(C, Sh, ProbeCount, InProbeCount);
        SetShaderValue(C, Sh, TraceStartIndex, InTraceStartIndex);
        SetShaderValue(C, Sh, TotalRaysPerProbe, InTotalRaysPerProbe);
        SetShaderValue(C, Sh, NodeCount, InNodeCount);
        SetShaderValue(C, Sh, TriangleCount, InTriangleCount);
        SetShaderValue(C, Sh, MaxTraceDistance, InMaxTraceDistance);
        SetShaderValue(C, Sh, StaticBackgroundColor, InBackgroundColor);
        SetShaderValue(C, Sh, StaticBackgroundIntensity, InBackgroundIntensity);
        SetShaderValue(C, Sh, bEnableStaticBackground, bInEnableStaticBackground);
        SetShaderValue(C, Sh, GIIntensity, InGIIntensity);
        SetShaderValue(C, Sh, DirectionalLightDirection, InDirectionalDirection);
        SetShaderValue(C, Sh, DirectionalLightColor, InDirectionalColor);
        SetShaderValue(C, Sh, DirectionalLightIntensity, InDirectionalIntensity);
        SetShaderValue(C, Sh, bHasDirectionalLight, bInHasDirectionalLight);
        SetShaderValue(C, Sh, LocalLightCount, FMath::Min(InLocalLightCount, 32u));
        check(InLocalPositions.Num() >= 32 && InLocalDirections.Num() >= 32 &&
            InLocalColors.Num() >= 32 && InLocalParameters.Num() >= 32);
        SetShaderValueArray(C, Sh, LocalLightPositionAndRadius, InLocalPositions.GetData(), 32u);
        SetShaderValueArray(C, Sh, LocalLightDirectionAndCosOuter, InLocalDirections.GetData(), 32u);
        SetShaderValueArray(C, Sh, LocalLightColorAndIntensity, InLocalColors.GetData(), 32u);
        SetShaderValueArray(C, Sh, LocalLightParameters, InLocalParameters.GetData(), 32u);
        if (DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, DirectionsTexture.GetBaseIndex(), Directions);
        if (ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, ProbePositionsTexture.GetBaseIndex(), Positions);
        if (LogicalToPhysicalSlots.IsBound()) C.SetShaderResourceViewParameter(Sh, LogicalToPhysicalSlots.GetBaseIndex(), LogicalToPhysical);
        if (BVHNodes.IsBound()) C.SetShaderResourceViewParameter(Sh, BVHNodes.GetBaseIndex(), Nodes);
        if (BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh, BVHTriangles.GetBaseIndex(), Triangles);
        if (HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, HitDistanceTexture.GetBaseIndex(), HitDistances);
        if (HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, HitTriangleIndexTexture.GetBaseIndex(), HitIndices);
        if (SurfaceRadianceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, SurfaceRadianceTexture.GetBaseIndex(), SurfaceRadiance);
        if (RadianceUAV.IsBound()) C.SetUAVParameter(Sh, RadianceUAV.GetBaseIndex(), Radiance);
    }
    template<typename TShaderRHIParamRef>
    void UnbindResources(FRHICommandList& C, const TShaderRHIParamRef Sh)
    {
        if (DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, DirectionsTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, ProbePositionsTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (LogicalToPhysicalSlots.IsBound()) C.SetShaderResourceViewParameter(Sh, LogicalToPhysicalSlots.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (BVHNodes.IsBound()) C.SetShaderResourceViewParameter(Sh, BVHNodes.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh, BVHTriangles.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, HitDistanceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, HitTriangleIndexTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (SurfaceRadianceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh, SurfaceRadianceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        if (RadianceUAV.IsBound()) C.SetUAVParameter(Sh, RadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
    }
private:
    LAYOUT_FIELD(FShaderParameter, TextureWidth);
    LAYOUT_FIELD(FShaderParameter, TotalTexels);
    LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
    LAYOUT_FIELD(FShaderParameter, ProbeCount);
    LAYOUT_FIELD(FShaderParameter, TraceStartIndex);
    LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
    LAYOUT_FIELD(FShaderParameter, NodeCount);
    LAYOUT_FIELD(FShaderParameter, TriangleCount);
    LAYOUT_FIELD(FShaderParameter, MaxTraceDistance);
    LAYOUT_FIELD(FShaderParameter, StaticBackgroundColor);
    LAYOUT_FIELD(FShaderParameter, StaticBackgroundIntensity);
    LAYOUT_FIELD(FShaderParameter, bEnableStaticBackground);
    LAYOUT_FIELD(FShaderParameter, GIIntensity);
    LAYOUT_FIELD(FShaderParameter, DirectionalLightDirection);
    LAYOUT_FIELD(FShaderParameter, DirectionalLightColor);
    LAYOUT_FIELD(FShaderParameter, DirectionalLightIntensity);
    LAYOUT_FIELD(FShaderParameter, bHasDirectionalLight);
    LAYOUT_FIELD(FShaderParameter, LocalLightCount);
    LAYOUT_FIELD(FShaderParameter, LocalLightPositionAndRadius);
    LAYOUT_FIELD(FShaderParameter, LocalLightDirectionAndCosOuter);
    LAYOUT_FIELD(FShaderParameter, LocalLightColorAndIntensity);
    LAYOUT_FIELD(FShaderParameter, LocalLightParameters);
    LAYOUT_FIELD(FShaderResourceParameter, DirectionsTexture);
    LAYOUT_FIELD(FShaderResourceParameter, ProbePositionsTexture);
    LAYOUT_FIELD(FShaderResourceParameter, LogicalToPhysicalSlots);
    LAYOUT_FIELD(FShaderResourceParameter, BVHNodes);
    LAYOUT_FIELD(FShaderResourceParameter, BVHTriangles);
    LAYOUT_FIELD(FShaderResourceParameter, HitDistanceTexture);
    LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexTexture);
    LAYOUT_FIELD(FShaderResourceParameter, SurfaceRadianceTexture);
    LAYOUT_FIELD(FShaderResourceParameter, RadianceUAV);
};
IMPLEMENT_SHADER_TYPE(
    ,
    FDDGIRadianceCascadeResolveRHI,
    TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeResolveCS.usf"),
    TEXT("RadianceCascadeResolveCS"),
    SF_Compute);


class FDDGIRadianceCascadeSpatialGatherRHI : public FGlobalShader
{
 DECLARE_SHADER_TYPE(FDDGIRadianceCascadeSpatialGatherRHI, Global);
public:
 FDDGIRadianceCascadeSpatialGatherRHI() {}
 explicit FDDGIRadianceCascadeSpatialGatherRHI(const ShaderMetaType::CompiledShaderInitializerType& I) : FGlobalShader(I)
 {
  TextureWidth.Bind(I.ParameterMap,TEXT("TextureWidth")); TotalTexels.Bind(I.ParameterMap,TEXT("TotalTexels"));
  DispatchGroupsX.Bind(I.ParameterMap,TEXT("DispatchGroupsX")); ProbeCount.Bind(I.ParameterMap,TEXT("ProbeCount"));
  TraceStartIndex.Bind(I.ParameterMap,TEXT("TraceStartIndex"));
  TotalRaysPerProbe.Bind(I.ParameterMap,TEXT("TotalRaysPerProbe")); BaseRayCount.Bind(I.ParameterMap,TEXT("BaseRayCount")); HashCapacity.Bind(I.ParameterMap,TEXT("HashCapacity"));
  TriangleCount.Bind(I.ParameterMap,TEXT("TriangleCount"));
  ProbeSpacing.Bind(I.ParameterMap,TEXT("ProbeSpacing")); ProbeGridOrigin.Bind(I.ParameterMap,TEXT("ProbeGridOrigin"));
  RadianceTexture.Bind(I.ParameterMap,TEXT("RadianceTexture")); HitDistanceTexture.Bind(I.ParameterMap,TEXT("HitDistanceTexture")); HitTriangleIndexTexture.Bind(I.ParameterMap,TEXT("HitTriangleIndexTexture"));
  DirectionsTexture.Bind(I.ParameterMap,TEXT("DirectionsTexture"));
  ProbePositionsTexture.Bind(I.ParameterMap,TEXT("ProbePositionsTexture")); BVHTriangles.Bind(I.ParameterMap,TEXT("BVHTriangles"));
  ProbeSpatialHash.Bind(I.ParameterMap,TEXT("ProbeSpatialHash"));
  ProbeNeighborIndices.Bind(I.ParameterMap,TEXT("ProbeNeighborIndices"));
  UsePrecomputedProbeNeighbors.Bind(I.ParameterMap,TEXT("UsePrecomputedProbeNeighbors"));
  OutputRadianceUAV.Bind(I.ParameterMap,TEXT("OutputRadianceUAV"));
  LogicalToPhysicalSlots.Bind(I.ParameterMap,TEXT("LogicalToPhysicalSlots"));
 }
 static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
 template<typename TShaderRHIParamRef>
 void SetParameters(FRHICommandList& C,const TShaderRHIParamRef Sh,FShaderResourceViewRHIRef R,FShaderResourceViewRHIRef H,FShaderResourceViewRHIRef HitIndices,
  FShaderResourceViewRHIRef D,FShaderResourceViewRHIRef P,FShaderResourceViewRHIRef LogicalToPhysicalSRV,FShaderResourceViewRHIRef Triangles,FShaderResourceViewRHIRef S,
  FShaderResourceViewRHIRef NeighborList,FUnorderedAccessViewRHIRef O,
  uint32 T,uint32 G,uint32 PC,uint32 StartIndex,uint32 RPP,uint32 BRC,uint32 HC,uint32 TC,float PS,const FVector4& Origin,uint32 bUseNeighborList)
 {
  uint32 W=4096u;
  SetShaderValue(C,Sh,TextureWidth,W); SetShaderValue(C,Sh,TotalTexels,T); SetShaderValue(C,Sh,DispatchGroupsX,G);
  SetShaderValue(C,Sh,ProbeCount,PC); SetShaderValue(C,Sh,TraceStartIndex,StartIndex);
  SetShaderValue(C,Sh,TotalRaysPerProbe,RPP); SetShaderValue(C,Sh,BaseRayCount,BRC); SetShaderValue(C,Sh,HashCapacity,HC);
  SetShaderValue(C,Sh,TriangleCount,TC);
  SetShaderValue(C,Sh,ProbeSpacing,PS); SetShaderValue(C,Sh,ProbeGridOrigin,Origin);
  SetShaderValue(C,Sh,UsePrecomputedProbeNeighbors,bUseNeighborList);
  if(RadianceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,RadianceTexture.GetBaseIndex(),R);
  if(HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitDistanceTexture.GetBaseIndex(),H);
  if(HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitTriangleIndexTexture.GetBaseIndex(),HitIndices);
  if(DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,DirectionsTexture.GetBaseIndex(),D);
  
  if(ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbePositionsTexture.GetBaseIndex(),P);
  if(LogicalToPhysicalSlots.IsBound()) C.SetShaderResourceViewParameter(Sh,LogicalToPhysicalSlots.GetBaseIndex(),LogicalToPhysicalSRV);
  if(BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh,BVHTriangles.GetBaseIndex(),Triangles);
  if(ProbeSpatialHash.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeSpatialHash.GetBaseIndex(),S);
  if(ProbeNeighborIndices.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeNeighborIndices.GetBaseIndex(),NeighborList);
  if(OutputRadianceUAV.IsBound()) C.SetUAVParameter(Sh,OutputRadianceUAV.GetBaseIndex(),O);
 }
 template<typename TShaderRHIParamRef>
 void UnbindResources(FRHICommandList& C,const TShaderRHIParamRef Sh)
 {
  if(RadianceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,RadianceTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitDistanceTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitTriangleIndexTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,DirectionsTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  
  if(ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbePositionsTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(LogicalToPhysicalSlots.IsBound()) C.SetShaderResourceViewParameter(Sh,LogicalToPhysicalSlots.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh,BVHTriangles.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(ProbeSpatialHash.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeSpatialHash.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(ProbeNeighborIndices.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeNeighborIndices.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(OutputRadianceUAV.IsBound()) C.SetUAVParameter(Sh,OutputRadianceUAV.GetBaseIndex(),FUnorderedAccessViewRHIRef());
 }
private:
 LAYOUT_FIELD(FShaderParameter, TextureWidth);
 LAYOUT_FIELD(FShaderParameter, TotalTexels);
 LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
 LAYOUT_FIELD(FShaderParameter, ProbeCount);
 LAYOUT_FIELD(FShaderParameter, TraceStartIndex);
 LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
 LAYOUT_FIELD(FShaderParameter, BaseRayCount);
 LAYOUT_FIELD(FShaderParameter, HashCapacity);
 LAYOUT_FIELD(FShaderParameter, TriangleCount);
 LAYOUT_FIELD(FShaderParameter, ProbeSpacing);
 LAYOUT_FIELD(FShaderParameter, ProbeGridOrigin);
 LAYOUT_FIELD(FShaderResourceParameter, RadianceTexture);
 LAYOUT_FIELD(FShaderResourceParameter, HitDistanceTexture);
 LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexTexture);
 LAYOUT_FIELD(FShaderResourceParameter, DirectionsTexture);
 LAYOUT_FIELD(FShaderResourceParameter, ProbePositionsTexture);
 LAYOUT_FIELD(FShaderResourceParameter, LogicalToPhysicalSlots);
 LAYOUT_FIELD(FShaderResourceParameter, BVHTriangles);
 LAYOUT_FIELD(FShaderResourceParameter, ProbeSpatialHash);
 LAYOUT_FIELD(FShaderResourceParameter, ProbeNeighborIndices);
 LAYOUT_FIELD(FShaderResourceParameter, OutputRadianceUAV);
 LAYOUT_FIELD(FShaderParameter, UsePrecomputedProbeNeighbors);
};
IMPLEMENT_SHADER_TYPE(,FDDGIRadianceCascadeSpatialGatherRHI,
 TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeSpatialGatherCS.usf"),
 TEXT("RadianceCascadeSpatialGatherCS"),SF_Compute);

class FDDGIRadianceCascadeIntegrateRHI : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDDGIRadianceCascadeIntegrateRHI, Global);

public:
	FDDGIRadianceCascadeIntegrateRHI() {}

	explicit FDDGIRadianceCascadeIntegrateRHI(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		TextureWidth.Bind(Initializer.ParameterMap, TEXT("TextureWidth"));
		TotalIntegrationTexels.Bind(Initializer.ParameterMap, TEXT("TotalIntegrationTexels"));
		DispatchGroupsX.Bind(Initializer.ParameterMap, TEXT("DispatchGroupsX"));
		TotalRaysPerProbe.Bind(Initializer.ParameterMap, TEXT("TotalRaysPerProbe"));
		ProbeCount.Bind(Initializer.ParameterMap, TEXT("ProbeCount"));
		ProbeStartIndex.Bind(Initializer.ParameterMap, TEXT("ProbeStartIndex"));
		CascadeStorageOffset.Bind(Initializer.ParameterMap, TEXT("CascadeStorageOffset"));
		CascadeRayCount.Bind(Initializer.ParameterMap, TEXT("CascadeRayCount"));
		NextCascadeStorageOffset.Bind(Initializer.ParameterMap, TEXT("NextCascadeStorageOffset"));
		bCopyGatherToFinal.Bind(Initializer.ParameterMap, TEXT("bCopyGatherToFinal"));
		RadianceUAV.Bind(Initializer.ParameterMap, TEXT("RadianceUAV"));
		GatheredRadianceTexture.Bind(Initializer.ParameterMap, TEXT("GatheredRadianceTexture"));
		HitDistanceTexture.Bind(Initializer.ParameterMap, TEXT("HitDistanceTexture"));
		HitTriangleIndexTexture.Bind(Initializer.ParameterMap, TEXT("HitTriangleIndexTexture"));
		LogicalToPhysicalSlots.Bind(Initializer.ParameterMap, TEXT("LogicalToPhysicalSlots"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	template<typename TShaderRHIParamRef>
	void SetParameters(
		FRHICommandList& RHICmdList,
		const TShaderRHIParamRef ShaderRHI,
		FUnorderedAccessViewRHIRef InRadianceUAV,
		FShaderResourceViewRHIRef GatheredRadianceSRV,
		FShaderResourceViewRHIRef HitDistanceSRV,
		FShaderResourceViewRHIRef HitTriangleIndexSRV,
		FShaderResourceViewRHIRef LogicalToPhysicalSRV,
		uint32 InTotalIntegrationTexels,
		uint32 InDispatchGroupsX,
		uint32 InTotalRaysPerProbe,
		uint32 InProbeCount,
		uint32 InProbeStartIndex,
		uint32 InCascadeStorageOffset,
		uint32 InCascadeRayCount,
		uint32 InNextCascadeStorageOffset,
		uint32 bInCopyGatherToFinal)
	{
		const uint32 InTextureWidth = 4096u;
		SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
		SetShaderValue(RHICmdList, ShaderRHI, TotalIntegrationTexels, InTotalIntegrationTexels);
		SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
		SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeCount, InProbeCount);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeStartIndex, InProbeStartIndex);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeStorageOffset, InCascadeStorageOffset);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeRayCount, InCascadeRayCount);
		SetShaderValue(RHICmdList, ShaderRHI, NextCascadeStorageOffset, InNextCascadeStorageOffset);
		SetShaderValue(RHICmdList, ShaderRHI, bCopyGatherToFinal, bInCopyGatherToFinal);
		if (RadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), InRadianceUAV);
		}
		if (GatheredRadianceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, GatheredRadianceTexture.GetBaseIndex(), GatheredRadianceSRV);
		}
		if (HitDistanceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, HitDistanceTexture.GetBaseIndex(), HitDistanceSRV);
		}
		if (HitTriangleIndexTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, HitTriangleIndexTexture.GetBaseIndex(), HitTriangleIndexSRV);
		}
		if (LogicalToPhysicalSlots.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), LogicalToPhysicalSRV);
		}
	}

	template<typename TShaderRHIParamRef>
	void UnbindResources(FRHICommandList& RHICmdList, const TShaderRHIParamRef ShaderRHI)
	{
		if (RadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
		if (GatheredRadianceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, GatheredRadianceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
		if (HitDistanceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, HitDistanceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
		if (HitTriangleIndexTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, HitTriangleIndexTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
		if (LogicalToPhysicalSlots.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
	}

private:
	LAYOUT_FIELD(FShaderParameter, TextureWidth);
	LAYOUT_FIELD(FShaderParameter, TotalIntegrationTexels);
	LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
	LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
	LAYOUT_FIELD(FShaderParameter, ProbeCount);
	LAYOUT_FIELD(FShaderParameter, ProbeStartIndex);
	LAYOUT_FIELD(FShaderParameter, CascadeStorageOffset);
	LAYOUT_FIELD(FShaderParameter, CascadeRayCount);
	LAYOUT_FIELD(FShaderParameter, NextCascadeStorageOffset);
	LAYOUT_FIELD(FShaderParameter, bCopyGatherToFinal);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceUAV);
	LAYOUT_FIELD(FShaderResourceParameter, GatheredRadianceTexture);
	LAYOUT_FIELD(FShaderResourceParameter, HitDistanceTexture);
	LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexTexture);
	LAYOUT_FIELD(FShaderResourceParameter, LogicalToPhysicalSlots);
};

IMPLEMENT_SHADER_TYPE(
	,
	FDDGIRadianceCascadeIntegrateRHI,
	TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeIntegrateCS.usf"),
	TEXT("RadianceCascadeIntegrateCS"),
	SF_Compute);



class FDDGIRadianceCascadeVisibilityMomentsRHI : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FDDGIRadianceCascadeVisibilityMomentsRHI, Global);

public:
    FDDGIRadianceCascadeVisibilityMomentsRHI() {}

    explicit FDDGIRadianceCascadeVisibilityMomentsRHI(
        const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        TextureWidth.Bind(Initializer.ParameterMap, TEXT("TextureWidth"));
        TotalTexels.Bind(Initializer.ParameterMap, TEXT("TotalTexels"));
        DispatchGroupsX.Bind(Initializer.ParameterMap, TEXT("DispatchGroupsX"));
        ProbeCount.Bind(Initializer.ParameterMap, TEXT("ProbeCount"));
        TraceStartIndex.Bind(Initializer.ParameterMap, TEXT("TraceStartIndex"));
        TotalRaysPerProbe.Bind(Initializer.ParameterMap, TEXT("TotalRaysPerProbe"));
        BaseRayCount.Bind(Initializer.ParameterMap, TEXT("BaseRayCount"));
        CascadeCount.Bind(Initializer.ParameterMap, TEXT("CascadeCount"));
        MaxTraceDistance.Bind(Initializer.ParameterMap, TEXT("MaxTraceDistance"));
        HitDistanceTexture.Bind(Initializer.ParameterMap, TEXT("HitDistanceTexture"));
        HitTriangleIndexTexture.Bind(Initializer.ParameterMap, TEXT("HitTriangleIndexTexture"));
        LogicalToPhysicalSlots.Bind(Initializer.ParameterMap, TEXT("LogicalToPhysicalSlots"));
        VisibilityMomentsUAV.Bind(Initializer.ParameterMap, TEXT("VisibilityMomentsUAV"));
    }

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    template<typename TShaderRHIParamRef>
    void SetParameters(
        FRHICommandList& RHICmdList,
        const TShaderRHIParamRef ShaderRHI,
        FShaderResourceViewRHIRef HitDistances,
        FShaderResourceViewRHIRef HitTriangleIndices,
        FShaderResourceViewRHIRef LogicalToPhysical,
        FUnorderedAccessViewRHIRef VisibilityMoments,
        uint32 InTotalTexels,
        uint32 InDispatchGroupsX,
        uint32 InProbeCount,
        uint32 InTraceStartIndex,
        uint32 InTotalRaysPerProbe,
        uint32 InBaseRayCount,
        uint32 InCascadeCount,
        float InMaxTraceDistance)
    {
        const uint32 InTextureWidth = 4096u;
        SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
        SetShaderValue(RHICmdList, ShaderRHI, TotalTexels, InTotalTexels);
        SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
        SetShaderValue(RHICmdList, ShaderRHI, ProbeCount, InProbeCount);
        SetShaderValue(RHICmdList, ShaderRHI, TraceStartIndex, InTraceStartIndex);
        SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
        SetShaderValue(RHICmdList, ShaderRHI, BaseRayCount, InBaseRayCount);
        SetShaderValue(RHICmdList, ShaderRHI, CascadeCount, InCascadeCount);
        SetShaderValue(RHICmdList, ShaderRHI, MaxTraceDistance, InMaxTraceDistance);
        if (HitDistanceTexture.IsBound())
        {
            RHICmdList.SetShaderResourceViewParameter(
                ShaderRHI, HitDistanceTexture.GetBaseIndex(), HitDistances);
        }
        if (HitTriangleIndexTexture.IsBound())
        {
            RHICmdList.SetShaderResourceViewParameter(
                ShaderRHI, HitTriangleIndexTexture.GetBaseIndex(), HitTriangleIndices);
        }
        if (LogicalToPhysicalSlots.IsBound())
        {
            RHICmdList.SetShaderResourceViewParameter(
                ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), LogicalToPhysical);
        }
        if (VisibilityMomentsUAV.IsBound())
        {
            RHICmdList.SetUAVParameter(
                ShaderRHI, VisibilityMomentsUAV.GetBaseIndex(), VisibilityMoments);
        }
    }

    template<typename TShaderRHIParamRef>
    void UnbindResources(FRHICommandList& RHICmdList, const TShaderRHIParamRef ShaderRHI)
    {
        if (HitDistanceTexture.IsBound())
        {
            RHICmdList.SetShaderResourceViewParameter(
                ShaderRHI, HitDistanceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        }
        if (HitTriangleIndexTexture.IsBound())
        {
            RHICmdList.SetShaderResourceViewParameter(
                ShaderRHI, HitTriangleIndexTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
        }
        if (LogicalToPhysicalSlots.IsBound())
        {
            RHICmdList.SetShaderResourceViewParameter(
                ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), FShaderResourceViewRHIRef());
        }
        if (VisibilityMomentsUAV.IsBound())
        {
            RHICmdList.SetUAVParameter(
                ShaderRHI, VisibilityMomentsUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
        }
    }

private:
    LAYOUT_FIELD(FShaderParameter, TextureWidth);
    LAYOUT_FIELD(FShaderParameter, TotalTexels);
    LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
    LAYOUT_FIELD(FShaderParameter, ProbeCount);
    LAYOUT_FIELD(FShaderParameter, TraceStartIndex);
    LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
    LAYOUT_FIELD(FShaderParameter, BaseRayCount);
    LAYOUT_FIELD(FShaderParameter, CascadeCount);
    LAYOUT_FIELD(FShaderParameter, MaxTraceDistance);
    LAYOUT_FIELD(FShaderResourceParameter, HitDistanceTexture);
    LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexTexture);
    LAYOUT_FIELD(FShaderResourceParameter, LogicalToPhysicalSlots);
    LAYOUT_FIELD(FShaderResourceParameter, VisibilityMomentsUAV);
};

IMPLEMENT_SHADER_TYPE(
    ,
    FDDGIRadianceCascadeVisibilityMomentsRHI,
    TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeVisibilityMomentsCS.usf"),
    TEXT("RadianceCascadeVisibilityMomentsCS"),
    SF_Compute);


class FDDGIApplyRadianceCascadesLightingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDDGIApplyRadianceCascadesLightingCS);
	SHADER_USE_PARAMETER_STRUCT(FDDGIApplyRadianceCascadesLightingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MetallicTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ScreenSpaceAOTexture)
		SHADER_PARAMETER(uint32, bHasScreenSpaceAO)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DirectionsTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RadianceTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HitTriangleIndexTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VisibilityMomentsTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ProbePositionsTexture)
		SHADER_PARAMETER_SRV(StructuredBuffer<int4>, ProbeSpatialHash)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, ProbeNeighborIndices)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, LogicalToPhysicalSlots)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SceneColorOutputUAV)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
		SHADER_PARAMETER(FVector4, ScaledViewSizeAndInvSize)
		SHADER_PARAMETER(FVector2D, ViewRectMin)
		SHADER_PARAMETER(uint32, ProbeCount)
		SHADER_PARAMETER(uint32, TotalRaysPerProbe)
		SHADER_PARAMETER(uint32, BaseRayCount)
		SHADER_PARAMETER(uint32, HashCapacity)
		SHADER_PARAMETER(uint32, UsePrecomputedProbeNeighbors)
		SHADER_PARAMETER(float, ProbeSpacing)
		SHADER_PARAMETER(float, BaseSegmentLength)
		SHADER_PARAMETER(float, CascadeScale)
		SHADER_PARAMETER(float, MaxTraceDistance)
		SHADER_PARAMETER(uint32, CascadeCount)
		SHADER_PARAMETER(float, ProbeGridDistance)
		SHADER_PARAMETER(FVector4, ProbeGridOrigin)
		SHADER_PARAMETER(FVector4, ActiveWindowCenterCell)
		SHADER_PARAMETER(FVector4, ActiveWindowCenterLocal)
		SHADER_PARAMETER(FVector4, BVHWorldOrigin)
		SHADER_PARAMETER(uint32, ShouldUsePreExposure)
		SHADER_PARAMETER(uint32, DebugMode)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};


IMPLEMENT_GLOBAL_SHADER(
	FDDGIApplyRadianceCascadesLightingCS,
	"/Plugin/DDGIRadianceCascades/Private/ApplyRadianceCascadesLighting.usf",
	"MainCS",
	SF_Compute);


void FDDGIRadianceCascadeRHIViewExtension::RenderDiffuseIndirect(
	const FScene& InScene,
	const FViewInfo& View,
	FRDGBuilder& GraphBuilder,
	FGlobalIlluminationExperimentalPluginResources& Resources)
{

	const FSceneInterface* Scene = static_cast<const FSceneInterface*>(&InScene);
	if (!Scene ||
		!Resources.SceneColor.IsValid() ||
		!Resources.GBufferA.IsValid() ||
		!Resources.GBufferB.IsValid() ||
		!Resources.GBufferC.IsValid() ||
		!Resources.SceneDepthZ.IsValid())
	{
		return;
	}

	const FDDGIMasterConfig* Config = DDGIGetMasterConfig_RenderThread(Scene);
	if (!Config || !Config->bEnableDynamicGI || !Config->SoftwareBVH.IsValid())
	{
		return;
	}

	FRDGTextureRef SceneColorTexture = GraphBuilder.RegisterExternalTexture(
		Resources.SceneColor, ERenderTargetTexture::ShaderResource);
	FRDGTextureRef SceneColorTarget = GraphBuilder.RegisterExternalTexture(
		Resources.SceneColor, ERenderTargetTexture::Targetable);
	FRDGTextureRef NormalTexture = GraphBuilder.RegisterExternalTexture(Resources.GBufferA);
	FRDGTextureRef MetallicTexture = GraphBuilder.RegisterExternalTexture(Resources.GBufferB);
	FRDGTextureRef BaseColorTexture = GraphBuilder.RegisterExternalTexture(Resources.GBufferC);
	FRDGTextureRef DepthTexture = GraphBuilder.RegisterExternalTexture(Resources.SceneDepthZ);

	if (!SceneColorTexture || !SceneColorTarget || !NormalTexture ||
		!MetallicTexture || !BaseColorTexture || !DepthTexture)
	{
		return;
	}

	FRDGTextureRef DirectionsTexture = nullptr;
	FRDGTextureRef RadianceTexture = nullptr;
	FRDGTextureRef HitTriangleIndexTexture = nullptr;
	FRDGTextureRef VisibilityMomentsTexture = nullptr;
	FRDGTextureRef ProbePositionsTexture = nullptr;
	uint32 ProbeCount = 0;
	uint32 TotalRaysPerProbe = 0;
	if (!DDGIGetRadianceCascadeLightingTextures_RenderThread(
		GraphBuilder, Scene, DirectionsTexture, RadianceTexture,
		HitTriangleIndexTexture, VisibilityMomentsTexture, ProbePositionsTexture,
		ProbeCount, TotalRaysPerProbe))
	{
		return;
	}

	FShaderResourceViewRHIRef ProbeSpatialHashSRV;
	FShaderResourceViewRHIRef LogicalToPhysicalSRV, NewPhysicalSlotsSRV;
	uint32 NewPhysicalSlotCount = 0, PhysicalSlotCapacity = 0;
	uint32 HashCapacity = 0;
	float ProbeSpacing = 0.0f;
	if (!DDGIGetRadianceCascadePhysicalSlotResources_RenderThread(
		Scene, LogicalToPhysicalSRV, NewPhysicalSlotsSRV, NewPhysicalSlotCount, PhysicalSlotCapacity) ||
		!LogicalToPhysicalSRV.IsValid() || PhysicalSlotCapacity == 0u)
	{
		return;
	}

	if (!DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(
		Scene, ProbeSpatialHashSRV, HashCapacity, ProbeSpacing) ||
		!ProbeSpatialHashSRV.IsValid() || HashCapacity == 0)
	{
		return;
	}

	FShaderResourceViewRHIRef ProbeNeighborIndicesSRV;
	uint32 NeighborListProbeCount = 0u;
	const bool bHasPrecomputedNeighbors =
		DDGIGetRadianceCascadeProbeNeighborList_RenderThread(
			Scene, ProbeNeighborIndicesSRV, NeighborListProbeCount) &&
		ProbeNeighborIndicesSRV.IsValid() &&
		NeighborListProbeCount == ProbeCount;

	const FIntPoint ViewSize = View.ViewRect.Size();
	if (ViewSize.X <= 0 || ViewSize.Y <= 0)
	{
		return;
	}

	FRDGTextureDesc OutputDesc = SceneColorTexture->Desc;
	OutputDesc.Flags |= TexCreate_UAV | TexCreate_ShaderResource;
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
		OutputDesc, TEXT("DDGI.RadianceCascades.SceneColor"));

	FDDGIApplyRadianceCascadesLightingCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FDDGIApplyRadianceCascadesLightingCS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneColorTexture = SceneColorTexture;
	PassParameters->NormalTexture = NormalTexture;
	PassParameters->DepthTexture = DepthTexture;
	PassParameters->BaseColorTexture = BaseColorTexture;
	PassParameters->MetallicTexture = MetallicTexture;

	// The native diffuse-indirect composite uses StaticFraction to decide how
	// much AO affects SceneColor. At zero, it does not apply AO at all, so
	// apply the generated SSAO mask directly to the DDGI contribution.
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
	FRDGTextureRef ScreenSpaceAOTexture = nullptr;
	if (SceneContext.bScreenSpaceAOIsValid &&
		SceneContext.ScreenSpaceAO.IsValid() &&
		View.FinalPostProcessSettings.AmbientOcclusionStaticFraction <= SMALL_NUMBER)
	{
		ScreenSpaceAOTexture = GraphBuilder.RegisterExternalTexture(
			SceneContext.ScreenSpaceAO, TEXT("DDGI.ScreenSpaceAO"));
	}
	PassParameters->bHasScreenSpaceAO = ScreenSpaceAOTexture != nullptr ? 1u : 0u;
	PassParameters->ScreenSpaceAOTexture = ScreenSpaceAOTexture != nullptr
		? ScreenSpaceAOTexture
		: DepthTexture;

	// The GI delegate runs before the renderer's native diffuse-indirect/AO
	// composite. Leave AO application to that pass so SSAO/DFAO is applied once.
	PassParameters->DirectionsTexture = DirectionsTexture;
	PassParameters->RadianceTexture = RadianceTexture;
	PassParameters->HitTriangleIndexTexture = HitTriangleIndexTexture;
	PassParameters->VisibilityMomentsTexture = VisibilityMomentsTexture;
	PassParameters->ProbePositionsTexture = ProbePositionsTexture;
	PassParameters->ProbeSpatialHash = ProbeSpatialHashSRV;
	PassParameters->ProbeNeighborIndices = bHasPrecomputedNeighbors
		? ProbeNeighborIndicesSRV
		: LogicalToPhysicalSRV;
	PassParameters->LogicalToPhysicalSlots = LogicalToPhysicalSRV;
	PassParameters->UsePrecomputedProbeNeighbors = bHasPrecomputedNeighbors ? 1u : 0u;
	PassParameters->SceneColorOutputUAV = GraphBuilder.CreateUAV(OutputTexture);
	PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->ScaledViewSizeAndInvSize = FVector4(
		static_cast<float>(ViewSize.X), static_cast<float>(ViewSize.Y),
		1.0f / static_cast<float>(ViewSize.X), 1.0f / static_cast<float>(ViewSize.Y));
	PassParameters->ViewRectMin = FVector2D(
		static_cast<float>(View.ViewRect.Min.X),
		static_cast<float>(View.ViewRect.Min.Y));
	PassParameters->ProbeCount = ProbeCount;
	PassParameters->TotalRaysPerProbe = TotalRaysPerProbe;
	PassParameters->BaseRayCount = static_cast<uint32>(FMath::Clamp(
		Config->RadianceCascades.BaseRayCount, 1, 64));
	PassParameters->HashCapacity = HashCapacity;
	PassParameters->ProbeSpacing = ProbeSpacing;
	PassParameters->BaseSegmentLength = Config->RadianceCascades.BaseSegmentLength;
	PassParameters->CascadeScale = Config->RadianceCascades.CascadeScale;
	PassParameters->MaxTraceDistance = Config->RadianceCascades.MaxTraceDistance;
	PassParameters->CascadeCount = static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount());
	PassParameters->ProbeGridDistance = Config->ProbeGridDistance;
	PassParameters->ProbeGridOrigin = FVector4(
		Config->ProbeGridOrigin.X, Config->ProbeGridOrigin.Y, Config->ProbeGridOrigin.Z, 0.0f);
	// Preserve the snapped lattice center for support calculations; ownership uses the continuous center below.
	PassParameters->ActiveWindowCenterCell = FVector4(
		static_cast<float>(Config->ActiveProbeWindowCenterCell.X),
		static_cast<float>(Config->ActiveProbeWindowCenterCell.Y),
		static_cast<float>(Config->ActiveProbeWindowCenterCell.Z), 0.0f);
	// The lattice remains snapped, but clipmap ownership follows the continuous
	// camera center so the transition does not jump when the probe window recenters.
	const FVector ActiveWindowCenterLocal =
		Config->ActiveProbeWindowCenter - Config->SoftwareBVH->WorldOrigin;
	PassParameters->ActiveWindowCenterLocal = FVector4(
		ActiveWindowCenterLocal.X, ActiveWindowCenterLocal.Y,
		ActiveWindowCenterLocal.Z, 0.0f);
	PassParameters->BVHWorldOrigin = FVector4(
		Config->SoftwareBVH->WorldOrigin.X,
		Config->SoftwareBVH->WorldOrigin.Y,
		Config->SoftwareBVH->WorldOrigin.Z, 0.0f);
	PassParameters->ShouldUsePreExposure = 1u;
	PassParameters->DebugMode = static_cast<uint32>(FMath::Clamp(CVarDDGIDebugApply.GetValueOnRenderThread(), 0, 9));

	TShaderMapRef<FDDGIApplyRadianceCascadesLightingCS> ComputeShader(
		GetGlobalShaderMap(View.GetFeatureLevel()));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("DDGI Apply Radiance Cascades Lighting"),
		ComputeShader,
		PassParameters,
		FIntVector(
			FMath::DivideAndRoundUp(ViewSize.X, 8),
			FMath::DivideAndRoundUp(ViewSize.Y, 8),
			1));

	// Write the UAV result back into the renderer-owned scene-color target. This
	// keeps subsequent fog, atmosphere, water and translucency passes on the
	// standard renderer path instead of applying GI in post processing.
	AddDrawTexturePass(
		GraphBuilder,
		View,
		OutputTexture,
		SceneColorTarget,
		View.ViewRect.Min,
		View.ViewRect.Min,
		View.ViewRect.Size());

}

FDDGIRadianceCascadeRHIViewExtension::FDDGIRadianceCascadeRHIViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

bool FDDGIRadianceCascadeRHIViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return true;
}

void FDDGIRadianceCascadeRHIViewExtension::PreRenderView_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FSceneView& InView)
{
	check(IsInRenderingThread());

	if (!InView.Family || !InView.Family->Scene)
	{
		return;
	}

	const FSceneInterface* Scene = InView.Family->Scene;
	const FDDGIMasterConfig* Config = DDGIGetMasterConfig_RenderThread(Scene);
	if (!Config || !Config->bEnableDynamicGI)
	{
		return;
	}

	// Probe positions are anchored to world origin. The active view controls
	// when this scene-wide update runs, but never translates the probe lattice.
	if (!Config->SoftwareBVH.IsValid())
	{
		return;
	}

	FShaderResourceViewRHIRef NodesSRV;
	FShaderResourceViewRHIRef TrianglesSRV;
	uint32 NodeCount = 0;
	uint32 TriangleCount = 0;
	if (!DDGIGetSoftwareBVHResources_RenderThread(Scene, NodesSRV, TrianglesSRV, NodeCount, TriangleCount))
	{
		// The resource manager supplies valid zero-count buffers for an empty BVH.
		return;
	}

	FTexture2DRHIRef DirectionsTexture;
	FTexture2DRHIRef RadianceTexture;
	FTexture2DRHIRef HitDistanceTexture;
	FTexture2DRHIRef HitTriangleIndexTexture;
	FTexture2DRHIRef ProbePositionsTexture;
	uint32 ProbeCount = 0;
	uint32 TotalRaysPerProbe = 0;
	if (!DDGIGetRadianceCascadeTracingRHIResources_RenderThread(
		Scene, DirectionsTexture, RadianceTexture, HitDistanceTexture, HitTriangleIndexTexture, ProbePositionsTexture,
		ProbeCount, TotalRaysPerProbe))
	{
		return;
	}


	const uint32 TotalTexels = ProbeCount * TotalRaysPerProbe;
	if (TotalTexels == 0)
	{
		return;
	}

	// Prepare every resource required by Trace, Gather and Integrate before
	// dispatching anything. A late allocation failure must not leave Radiance
	// partially overwritten by Trace while Apply still reads the previous frame.
	FTexture2DRHIRef RadianceScratchTexture;
	FTexture2DRHIRef RadianceIntegrationScratchTexture;
	FShaderResourceViewRHIRef SpatialHashSRV, NeighborListSRV;
	uint32 HashCapacity = 0, SpatialProbeCount = 0, NeighborListProbeCount = 0;
	float ProbeSpacing = 0.0f;
	const bool bRequestNeighborList = CVarDDGIUsePrecomputedProbeNeighbors.GetValueOnRenderThread() != 0;
	uint32 bUseNeighborList = 0u;
	if (bRequestNeighborList)
	{
		bUseNeighborList =
			DDGIGetRadianceCascadeProbeNeighborList_RenderThread(Scene, NeighborListSRV, NeighborListProbeCount) &&
			NeighborListSRV.IsValid() && NeighborListProbeCount == ProbeCount
			? 1u : 0u;
	}
	if (!DDGIGetRadianceCascadeScratchTexture_RenderThread(Scene, RadianceScratchTexture) ||
		!DDGIGetRadianceCascadeIntegrationScratchTexture_RenderThread(Scene, RadianceIntegrationScratchTexture) ||
		!DDGIGetRadianceCascadeProbePositionsRHI_RenderThread(Scene, ProbePositionsTexture, SpatialProbeCount) ||
		SpatialProbeCount != ProbeCount)
	{
		return;
	}
	if (bUseNeighborList)
	{
		ProbeSpacing = FMath::Max(1.0f, Config->ProbeSpacing);
	}
	else
	{
		const bool bHasSpatialHash = DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(
			Scene, SpatialHashSRV, HashCapacity, ProbeSpacing);
		if (!bHasSpatialHash || !SpatialHashSRV.IsValid() || HashCapacity == 0)
		{
			return;
		}
	}
	FShaderResourceViewRHIRef GatherRadianceSRV = RHICreateShaderResourceView(RadianceTexture, 0);
	FShaderResourceViewRHIRef GatherHitSRV = RHICreateShaderResourceView(HitDistanceTexture, 0);
	FShaderResourceViewRHIRef GatherHitIndicesSRV = RHICreateShaderResourceView(HitTriangleIndexTexture, 0);
	FShaderResourceViewRHIRef GatherDirectionsSRV = RHICreateShaderResourceView(DirectionsTexture, 0);
	FShaderResourceViewRHIRef GatherPositionsSRV = RHICreateShaderResourceView(ProbePositionsTexture, 0);
	FUnorderedAccessViewRHIRef GatherOutputUAV = RHICreateUnorderedAccessView(RadianceScratchTexture);
	FShaderResourceViewRHIRef GatherOutputSRV = RHICreateShaderResourceView(RadianceScratchTexture, 0);
	FUnorderedAccessViewRHIRef IntegrationScratchUAV = RHICreateUnorderedAccessView(RadianceIntegrationScratchTexture);
	FShaderResourceViewRHIRef IntegrationScratchSRV = RHICreateShaderResourceView(RadianceIntegrationScratchTexture, 0);
	FShaderResourceViewRHIRef IntegrationHitDistanceSRV = RHICreateShaderResourceView(HitDistanceTexture, 0);
	FShaderResourceViewRHIRef IntegrationHitTriangleIndexSRV = RHICreateShaderResourceView(HitTriangleIndexTexture, 0);
	if (!GatherRadianceSRV.IsValid() || !GatherHitSRV.IsValid() || !GatherHitIndicesSRV.IsValid() ||
		!GatherDirectionsSRV.IsValid() || !GatherPositionsSRV.IsValid() || !TrianglesSRV.IsValid() ||
		!GatherOutputUAV.IsValid() || !GatherOutputSRV.IsValid() ||
		!IntegrationScratchUAV.IsValid() || !IntegrationScratchSRV.IsValid() ||
		!IntegrationHitDistanceSRV.IsValid() || !IntegrationHitTriangleIndexSRV.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to prepare Gather/Integrate SRV/UAV views."));
		return;
	}

	FShaderResourceViewRHIRef DirectionsSRV = RHICreateShaderResourceView(DirectionsTexture, 0);
	FShaderResourceViewRHIRef LogicalToPhysicalSRV, NewPhysicalSlotsSRV;
	uint32 NewPhysicalSlotCount = 0, PhysicalSlotCapacity = 0;
	if (!DDGIGetRadianceCascadePhysicalSlotResources_RenderThread(
		Scene, LogicalToPhysicalSRV, NewPhysicalSlotsSRV, NewPhysicalSlotCount, PhysicalSlotCapacity) ||
		!LogicalToPhysicalSRV.IsValid() || PhysicalSlotCapacity == 0u)
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to acquire logical-to-physical probe mapping."));
		return;
	}
	TArray<uint32> NewLogicalProbeIndices;
	DDGIGetRadianceCascadeNewLogicalProbeIndices_RenderThread(Scene, NewLogicalProbeIndices);
	FShaderResourceViewRHIRef ProbePositionsSRV = RHICreateShaderResourceView(ProbePositionsTexture, 0);
	FUnorderedAccessViewRHIRef RadianceUAV = RHICreateUnorderedAccessView(RadianceTexture);
	FUnorderedAccessViewRHIRef HitDistanceUAV = RHICreateUnorderedAccessView(HitDistanceTexture);
	FUnorderedAccessViewRHIRef HitTriangleIndexUAV = RHICreateUnorderedAccessView(HitTriangleIndexTexture);
	if (!DirectionsSRV.IsValid() || !ProbePositionsSRV.IsValid() || !RadianceUAV.IsValid() || !HitDistanceUAV.IsValid() || !HitTriangleIndexUAV.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create tracing SRV/UAV views."));
		return;
	}

	// Reserve the frame immediately before the first GPU dispatch. If initialization
	// is pending, its UAVs must be valid first; otherwise tracing views are checked
	// before the frame is claimed.
	bool bClaimedUpdateFrame = false;

	// Initialize shared ray directions when their texture is created, and clear
	// history only for newly allocated or explicitly invalidated physical slots.
	// Keep initialization outputs separate from the tracing resources.
	// The initialization getter SafeReleases its output references when no
	// initialization is pending; sharing the tracing references would null them.
	FTexture2DRHIRef InitDirectionsTexture;
	FTexture2DRHIRef InitRadianceTexture;
	FTexture2DRHIRef InitHitDistanceTexture;
	FTexture2DRHIRef InitRadianceScratchTexture;
	FTexture2DRHIRef InitRadianceIntegrationScratchTexture;
	FTexture2DRHIRef InitHitTriangleIndexTexture;
	uint32 InitPendingSlotCount = 0;
	bool bInitializeDirections = false;
	if (DDGIGetRadianceCascadeInitializationRHIResources_RenderThread(
		Scene, InitDirectionsTexture, InitRadianceTexture, InitHitDistanceTexture,
		InitRadianceScratchTexture, InitRadianceIntegrationScratchTexture,
		InitHitTriangleIndexTexture, InitPendingSlotCount, bInitializeDirections))
	{
		if (!InitDirectionsTexture.IsValid() ||
			!InitRadianceTexture.IsValid() ||
			!InitHitDistanceTexture.IsValid() ||
			!InitRadianceScratchTexture.IsValid() ||
			!InitRadianceIntegrationScratchTexture.IsValid() ||
			!InitHitTriangleIndexTexture.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: initialization returned invalid textures."));
			return;
		}

		FUnorderedAccessViewRHIRef InitDirectionsUAV = RHICreateUnorderedAccessView(InitDirectionsTexture);
		FUnorderedAccessViewRHIRef InitRadianceUAV = RHICreateUnorderedAccessView(InitRadianceTexture);
		FUnorderedAccessViewRHIRef InitHitDistanceUAV = RHICreateUnorderedAccessView(InitHitDistanceTexture);
		FUnorderedAccessViewRHIRef InitRadianceScratchUAV = RHICreateUnorderedAccessView(InitRadianceScratchTexture);
		FUnorderedAccessViewRHIRef InitRadianceIntegrationScratchUAV = RHICreateUnorderedAccessView(InitRadianceIntegrationScratchTexture);
		FUnorderedAccessViewRHIRef InitHitTriangleIndexUAV = RHICreateUnorderedAccessView(InitHitTriangleIndexTexture);

		if (InitDirectionsUAV.IsValid() && InitRadianceUAV.IsValid() && InitHitDistanceUAV.IsValid() &&
			InitRadianceScratchUAV.IsValid() && InitRadianceIntegrationScratchUAV.IsValid() &&
			InitHitTriangleIndexUAV.IsValid())
		{
			if (!DDGIBeginRadianceCascadeUpdate_RenderThread(Scene, InView.Family->FrameNumber))
			{
				return;
			}
			bClaimedUpdateFrame = true;

			DDGITransitionTexture(RHICmdList, InitDirectionsTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitHitDistanceTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceScratchTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceIntegrationScratchTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitHitTriangleIndexTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);

			TShaderMapRef<FDDGIRadianceCascadeInitializeRHI> InitializeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			RHICmdList.SetComputeShader(InitializeShader.GetComputeShader());
			const uint32 InitTotalTexels = InitPendingSlotCount * TotalRaysPerProbe;
			const uint32 InitTotalGroups = FMath::DivideAndRoundUp(InitTotalTexels, 64u);
			const uint32 InitGroupsX = FMath::Max(1u, FMath::Min(InitTotalGroups, 65535u));
			const uint32 InitGroupsY = FMath::DivideAndRoundUp(InitTotalGroups, InitGroupsX);
			InitializeShader->SetParameters(
				RHICmdList, InitializeShader.GetComputeShader(), InitDirectionsUAV, InitRadianceUAV,
				InitHitDistanceUAV, InitRadianceScratchUAV, InitRadianceIntegrationScratchUAV,
				InitHitTriangleIndexUAV, NewPhysicalSlotsSRV, bInitializeDirections ? 1u : 0u,
				TotalRaysPerProbe,
				static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount()),
				static_cast<uint32>(FMath::Clamp(Config->RadianceCascades.BaseRayCount, 1, 64)),
				InitPendingSlotCount, InitTotalTexels, InitGroupsX);
			DispatchComputeShader(RHICmdList, InitializeShader.GetShader(), InitGroupsX, InitGroupsY, 1);
			InitializeShader->UnbindResources(RHICmdList, InitializeShader.GetComputeShader());

			DDGITransitionTexture(RHICmdList, InitDirectionsTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitHitDistanceTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceScratchTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceIntegrationScratchTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitHitTriangleIndexTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(Scene);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create initialization UAVs."));
			return;
		}
	}


	// With no initialization dispatch this is the first GPU operation, so only
	// claim after all tracing SRV/UAV views have been created successfully.
	if (!bClaimedUpdateFrame &&
		!DDGIBeginRadianceCascadeUpdate_RenderThread(Scene, InView.Family->FrameNumber))
	{
		return;
	}

	// A new physical probe must be fully traced and integrated before it is
	// exposed to Apply. The regular round-robin budget is used only when there
	// are no newly allocated probes, preventing duplicate work in this frame.
	auto DispatchProbeBatch = [&](uint32 TraceStartIndex, uint32 TraceProbeCount)
	{
		if (TraceProbeCount == 0u)
		{
			return;
		}
	const uint32 ActiveCascadeCount = static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount());
	const uint32 BaseRayCount = static_cast<uint32>(FMath::Clamp(Config->RadianceCascades.BaseRayCount, 1, 64));
	// Each cascade traces independent probe-origin rays; no ancestor mask or
	// scratch-texture ping-pong is required during tracing.
	DDGITransitionTexture(RHICmdList, RadianceScratchTexture, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);
	DDGITransitionTexture(RHICmdList, HitDistanceTexture, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);
	DDGITransitionTexture(RHICmdList, HitTriangleIndexTexture, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);

	TShaderMapRef<FDDGIRadianceCascadeTraceRHI> TraceShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	for (uint32 CascadeIndex = 0u; CascadeIndex < ActiveCascadeCount; ++CascadeIndex)
	{
		const uint32 CascadeRayCount = BaseRayCount << CascadeIndex;
		const uint32 TraceTotalTexels = TraceProbeCount * CascadeRayCount;
		const uint32 TraceTotalGroups = FMath::DivideAndRoundUp(TraceTotalTexels, 64u);
		const uint32 TraceGroupsX = FMath::Max(1u, FMath::Min(TraceTotalGroups, 65535u));
		const uint32 TraceGroupsY = FMath::DivideAndRoundUp(TraceTotalGroups, TraceGroupsX);

		RHICmdList.SetComputeShader(TraceShader.GetComputeShader());
		TraceShader->SetParameters(
			RHICmdList, TraceShader.GetComputeShader(), DirectionsSRV, ProbePositionsSRV, LogicalToPhysicalSRV, NodesSRV, TrianglesSRV,
			GatherOutputUAV, HitDistanceUAV, HitTriangleIndexUAV,
			CascadeIndex,
			TraceTotalTexels, TraceGroupsX, ProbeCount, TraceStartIndex, TotalRaysPerProbe,
			NodeCount, TriangleCount, ActiveCascadeCount, BaseRayCount,
			Config->RadianceCascades.BaseSegmentLength, Config->RadianceCascades.CascadeScale,
			Config->RadianceCascades.MaxTraceDistance,
			FVector4(Config->StaticBackgroundColor.R, Config->StaticBackgroundColor.G, Config->StaticBackgroundColor.B, Config->StaticBackgroundColor.A),
			Config->StaticBackgroundIntensity, Config->GIIntensity,
			FVector4(
				InView.ViewMatrices.GetViewOrigin().X - Config->SoftwareBVH->WorldOrigin.X,
				InView.ViewMatrices.GetViewOrigin().Y - Config->SoftwareBVH->WorldOrigin.Y,
				InView.ViewMatrices.GetViewOrigin().Z - Config->SoftwareBVH->WorldOrigin.Z, 0.0f),
			Config->BackgroundTransitionStart, Config->BackgroundTransitionEnd,
			Config->bEnableStaticBackground ? 1u : 0u);
		DispatchComputeShader(RHICmdList, TraceShader.GetShader(), TraceGroupsX, TraceGroupsY, 1);
		TraceShader->UnbindResources(RHICmdList, TraceShader.GetComputeShader());
	}

	// Gather and integration still process the complete packed ray range for this probe batch.
	const uint32 TraceTotalTexels = TraceProbeCount * TotalRaysPerProbe;
	const uint32 TraceTotalGroups = FMath::DivideAndRoundUp(TraceTotalTexels, 64u);
	const uint32 TraceGroupsX = FMath::Max(1u, FMath::Min(TraceTotalGroups, 65535u));
	const uint32 TraceGroupsY = FMath::DivideAndRoundUp(TraceTotalGroups, TraceGroupsX);

	// Primary BVH traversal and surface lighting are separate compute programs.
	// The trace pass writes hit metadata/background; the shade pass consumes it
	// and performs primary direct lighting and visibility. The resolve pass adds the diffuse bounce.
	DDGITransitionTexture(RHICmdList, HitDistanceTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
	DDGITransitionTexture(RHICmdList, HitTriangleIndexTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);

	TShaderMapRef<FDDGIRadianceCascadeShadeRHI> ShadeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(ShadeShader.GetComputeShader());
	ShadeShader->SetParameters(
		RHICmdList, ShadeShader.GetComputeShader(),
		DirectionsSRV, ProbePositionsSRV, LogicalToPhysicalSRV, NodesSRV, TrianglesSRV,
		IntegrationHitDistanceSRV, IntegrationHitTriangleIndexSRV, GatherOutputUAV,
		TraceTotalTexels, TraceGroupsX, ProbeCount, TraceStartIndex, TotalRaysPerProbe,
		NodeCount, TriangleCount, Config->GIIntensity,
		FVector4(Config->DirectionalLightDirection.X, Config->DirectionalLightDirection.Y, Config->DirectionalLightDirection.Z, 0.0f),
		FVector4(Config->DirectionalLightColor.R, Config->DirectionalLightColor.G, Config->DirectionalLightColor.B, 0.0f),
		Config->DirectionalLightIntensity,
		Config->bHasDirectionalLight ? 1u : 0u,
		Config->bDirectionalLightCastsShadows ? 1u : 0u,
		Config->LocalLightPositionAndRadius,
		Config->LocalLightDirectionAndCosOuter,
		Config->LocalLightColorAndIntensity,
		Config->LocalLightParameters,
		Config->LocalLightCount);
	DispatchComputeShader(RHICmdList, ShadeShader.GetShader(), TraceGroupsX, TraceGroupsY, 1);
	ShadeShader->UnbindResources(RHICmdList, ShadeShader.GetComputeShader());

	// Surface shading writes the scratch radiance. Resolve reads it as an SRV
	// and writes the final texture, avoiding typed UAV reads on PF_FloatRGBA.
	DDGITransitionTexture(RHICmdList, RadianceScratchTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
	DDGITransitionTexture(RHICmdList, RadianceTexture, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);

	TShaderMapRef<FDDGIRadianceCascadeResolveRHI> ResolveShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(ResolveShader.GetComputeShader());
	ResolveShader->SetParameters(
		RHICmdList, ResolveShader.GetComputeShader(),
		DirectionsSRV, ProbePositionsSRV, LogicalToPhysicalSRV, NodesSRV, TrianglesSRV,
		IntegrationHitDistanceSRV, IntegrationHitTriangleIndexSRV, GatherOutputSRV, RadianceUAV,
		TraceTotalTexels, TraceGroupsX, ProbeCount, TraceStartIndex, TotalRaysPerProbe,
		NodeCount, TriangleCount, Config->RadianceCascades.MaxTraceDistance,
		FVector4(Config->StaticBackgroundColor.R, Config->StaticBackgroundColor.G, Config->StaticBackgroundColor.B, Config->StaticBackgroundColor.A),
		Config->StaticBackgroundIntensity, Config->bEnableStaticBackground ? 1u : 0u,
		Config->GIIntensity,
		FVector4(Config->DirectionalLightDirection.X, Config->DirectionalLightDirection.Y, Config->DirectionalLightDirection.Z, 0.0f),
		FVector4(Config->DirectionalLightColor.R, Config->DirectionalLightColor.G, Config->DirectionalLightColor.B, 0.0f),
		Config->DirectionalLightIntensity,
		Config->bHasDirectionalLight ? 1u : 0u,
				Config->LocalLightPositionAndRadius,
		Config->LocalLightDirectionAndCosOuter,
		Config->LocalLightColorAndIntensity,
		Config->LocalLightParameters,
		Config->LocalLightCount);
	DispatchComputeShader(RHICmdList, ResolveShader.GetShader(), TraceGroupsX, TraceGroupsY, 1);
	ResolveShader->UnbindResources(RHICmdList, ResolveShader.GetComputeShader());

	DDGITransitionTexture(RHICmdList, RadianceTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);

	// Spatial gather writes only probes traced in this batch, but may read
	// persistent raw samples from neighboring probes updated in earlier batches.
	// This preserves a full spatial neighborhood under incremental scheduling;
	// the integrated output remains a separate persistent texture.
	DDGITransitionTexture(RHICmdList, RadianceScratchTexture, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);
	TShaderMapRef<FDDGIRadianceCascadeSpatialGatherRHI> GatherShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(GatherShader.GetComputeShader());
	GatherShader->SetParameters(
		RHICmdList, GatherShader.GetComputeShader(),
		GatherRadianceSRV, GatherHitSRV, GatherHitIndicesSRV, GatherDirectionsSRV,
		GatherPositionsSRV, LogicalToPhysicalSRV, TrianglesSRV, SpatialHashSRV,
		NeighborListSRV, GatherOutputUAV,
		TraceTotalTexels, TraceGroupsX, ProbeCount, TraceStartIndex,
		TotalRaysPerProbe, static_cast<uint32>(Config->RadianceCascades.BaseRayCount),
		HashCapacity, TriangleCount, ProbeSpacing,
 		FVector4(Config->ProbeGridOrigin.X, Config->ProbeGridOrigin.Y, Config->ProbeGridOrigin.Z, 0.0f),
		bUseNeighborList);
	DispatchComputeShader(RHICmdList, GatherShader.GetShader(), TraceGroupsX, TraceGroupsY, 1);
	GatherShader->UnbindResources(RHICmdList, GatherShader.GetComputeShader());
	DDGITransitionTexture(RHICmdList, RadianceScratchTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);

	// Copy this batch's gathered rays into persistent final storage. Only the
	// scheduled physical probe slots are overwritten; all other probes retain
	// their previous integrated result.
	DDGITransitionTexture(RHICmdList, RadianceIntegrationScratchTexture,
		ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);
	TShaderMapRef<FDDGIRadianceCascadeIntegrateRHI> IntegrateShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(IntegrateShader.GetComputeShader());
	IntegrateShader->SetParameters(
		RHICmdList, IntegrateShader.GetComputeShader(),
		IntegrationScratchUAV, GatherOutputSRV, IntegrationHitDistanceSRV,
		IntegrationHitTriangleIndexSRV,
		LogicalToPhysicalSRV,
		TraceTotalTexels, TraceGroupsX, TotalRaysPerProbe,
		ProbeCount, TraceStartIndex, 0u, TotalRaysPerProbe, 0u, 1u);
	DispatchComputeShader(RHICmdList, IntegrateShader.GetShader(), TraceGroupsX, TraceGroupsY, 1);
	IntegrateShader->UnbindResources(RHICmdList, IntegrateShader.GetComputeShader());
	DDGITransitionTexture(RHICmdList, RadianceIntegrationScratchTexture,
		ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);

	// Integrate from far to near in the same persistent output texture. Each
	// pass reads child rays from a disjoint range and writes only its parent
	// range, so untouched probes retain their prior final radiance.
	const uint32 CascadeCount = static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount());
	for (int32 CascadeIndex = static_cast<int32>(CascadeCount) - 2; CascadeIndex >= 0; --CascadeIndex)
	{
		const uint32 CurrentCascadeRayCount = static_cast<uint32>(
			Config->RadianceCascades.GetRayCount(CascadeIndex));
		const uint32 IntegrationTexels = TraceProbeCount * CurrentCascadeRayCount;
		if (IntegrationTexels == 0u)
		{
			continue;
		}

		const uint32 IntegrationGroups = FMath::Max(
			1u, FMath::Min(FMath::DivideAndRoundUp(IntegrationTexels, 64u), 65535u));
		const uint32 IntegrationGroupsY = FMath::DivideAndRoundUp(
			IntegrationTexels, IntegrationGroups * 64u);

		DDGITransitionTexture(RHICmdList, RadianceIntegrationScratchTexture,
			ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);
		RHICmdList.SetComputeShader(IntegrateShader.GetComputeShader());
		IntegrateShader->SetParameters(
			RHICmdList, IntegrateShader.GetComputeShader(),
			IntegrationScratchUAV, GatherOutputSRV, IntegrationHitDistanceSRV,
			IntegrationHitTriangleIndexSRV, LogicalToPhysicalSRV,
			IntegrationTexels, IntegrationGroups, TotalRaysPerProbe,
			ProbeCount, TraceStartIndex,
			static_cast<uint32>(Config->RadianceCascades.GetRayStorageOffset(CascadeIndex)),
			CurrentCascadeRayCount,
			static_cast<uint32>(Config->RadianceCascades.GetRayStorageOffset(CascadeIndex + 1)),
			0u);
		DispatchComputeShader(RHICmdList, IntegrateShader.GetShader(), IntegrationGroups, IntegrationGroupsY, 1);
		IntegrateShader->UnbindResources(RHICmdList, IntegrateShader.GetComputeShader());
		DDGITransitionTexture(RHICmdList, RadianceIntegrationScratchTexture,
			ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
	}

    // Precompute one hit-distance distribution per base angular cell for this
    // probe batch. RadianceScratch is no longer needed by integration after
    // this point; its base-ray texels become persistent visibility moments.
    const uint32 VisibilityMomentTexels = TraceProbeCount * BaseRayCount;
    if (ActiveCascadeCount > 1u && VisibilityMomentTexels > 0u)
    {
        const uint32 VisibilityMomentGroups =
            FMath::DivideAndRoundUp(VisibilityMomentTexels, 64u);
        const uint32 VisibilityMomentGroupsX =
            FMath::Max(1u, FMath::Min(VisibilityMomentGroups, 65535u));
        const uint32 VisibilityMomentGroupsY =
            FMath::DivideAndRoundUp(VisibilityMomentGroups, VisibilityMomentGroupsX);

        DDGITransitionTexture(
            RHICmdList, RadianceScratchTexture,
            ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);
        TShaderMapRef<FDDGIRadianceCascadeVisibilityMomentsRHI> VisibilityMomentsShader(
            GetGlobalShaderMap(ERHIFeatureLevel::SM5));
        RHICmdList.SetComputeShader(VisibilityMomentsShader.GetComputeShader());
        VisibilityMomentsShader->SetParameters(
            RHICmdList, VisibilityMomentsShader.GetComputeShader(),
            IntegrationHitDistanceSRV, IntegrationHitTriangleIndexSRV,
            LogicalToPhysicalSRV, GatherOutputUAV,
            VisibilityMomentTexels, VisibilityMomentGroupsX,
            ProbeCount, TraceStartIndex, TotalRaysPerProbe,
            BaseRayCount, ActiveCascadeCount,
            Config->RadianceCascades.MaxTraceDistance);
        DispatchComputeShader(
            RHICmdList, VisibilityMomentsShader.GetShader(),
            VisibilityMomentGroupsX, VisibilityMomentGroupsY, 1);
        VisibilityMomentsShader->UnbindResources(
            RHICmdList, VisibilityMomentsShader.GetComputeShader());
        DDGITransitionTexture(
            RHICmdList, RadianceScratchTexture,
            ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
    }
	};

	auto DispatchProbeIndexRanges = [&](const TArray<uint32>& ProbeIndices)
	{
		if (ProbeIndices.Num() == 0)
		{
			return;
		}

		uint32 RangeStart = ProbeIndices[0];
		uint32 PreviousIndex = RangeStart;
		for (int32 Index = 1; Index <= ProbeIndices.Num(); ++Index)
		{
			const bool bEndOfRange = Index == ProbeIndices.Num();
			const uint32 CurrentIndex = bEndOfRange ? MAX_uint32 : ProbeIndices[Index];
			if (!bEndOfRange && CurrentIndex == PreviousIndex + 1u)
			{
				PreviousIndex = CurrentIndex;
				continue;
			}

			DispatchProbeBatch(RangeStart, PreviousIndex - RangeStart + 1u);
			if (!bEndOfRange)
			{
				RangeStart = CurrentIndex;
				PreviousIndex = CurrentIndex;
			}
		}
	};

	// New probes bypass the recurring budget and are completed immediately.
	DispatchProbeIndexRanges(NewLogicalProbeIndices);

	// Keep the normal dynamic-lighting refresh budget active as well, excluding
	// probes already processed above so no logical probe is dispatched twice.
	const uint32 TraceProbeBudget = static_cast<uint32>(FMath::Clamp(
		CVarDDGITraceProbesPerFrame.GetValueOnRenderThread(), 1, 32768));
	const uint32 TraceProbeCount = FMath::Min(ProbeCount, TraceProbeBudget);
	const uint32 TraceStartIndex = DDGIGetNextRadianceCascadeTraceStartIndex_RenderThread(
		Scene, ProbeCount, TraceProbeCount);
	if (TraceProbeCount > 0u && TraceStartIndex != MAX_uint32)
	{
		TSet<uint32> NewProbeIndexSet;
		NewProbeIndexSet.Reserve(NewLogicalProbeIndices.Num());
		for (const uint32 NewProbeIndex : NewLogicalProbeIndices)
		{
			NewProbeIndexSet.Add(NewProbeIndex);
		}

		TArray<uint32> RegularProbeIndices;
		RegularProbeIndices.Reserve(TraceProbeCount);
		for (uint32 BatchIndex = 0u; BatchIndex < TraceProbeCount; ++BatchIndex)
		{
			const uint32 ProbeIndex = (TraceStartIndex + BatchIndex) % ProbeCount;
			if (!NewProbeIndexSet.Contains(ProbeIndex))
			{
				RegularProbeIndices.Add(ProbeIndex);
			}
		}
		RegularProbeIndices.Sort();
		DispatchProbeIndexRanges(RegularProbeIndices);
	}


}