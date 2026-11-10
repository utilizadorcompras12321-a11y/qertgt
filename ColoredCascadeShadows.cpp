// Colored translucent cascaded shadow maps (custom addition for UE 4.27.2). See ColoredCascadeShadows.h for the overview.

#include "ColoredCascadeShadows.h"
#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "RHIStaticStates.h"
#include "RendererInterface.h"
#include "SceneManagement.h"
#include "ScenePrivateBase.h"
#include "PostProcess/SceneRenderTargets.h"
#include "LightSceneInfo.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "PixelShaderUtils.h"
#include "ShadowRendering.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"

static TAutoConsoleVariable<int32> CVarColoredCascadeShadowsEnable(
	TEXT("r.ColoredShadowMaps"),
	1,
	TEXT("Global switch for colored translucent cascade shadows. The per-light Colored Shadow Maps option is still required.\n")
	TEXT(" 0: off, 1: on (default)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarColoredCascadeShadowsStrength(
	TEXT("r.ColoredShadowMaps.Strength"),
	1.0f,
	TEXT("Blend between no tint (0) and full colored translucent shadow (1)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarColoredCascadeShadowsDebug(
	TEXT("r.ColoredShadowMaps.Debug"),
	0,
	TEXT("Diagnostics. 1: log the state of the feature every ~2 seconds (Output Log, search ColoredCSM).\n")
	TEXT(" 2: light pass shows magenta wherever a colored cascade covers the pixel (tests the screen pass).\n")
	TEXT(" 3: light pass shows the raw tint, 1-tint amplified to red (tests the caster map/depth test).\n")
	TEXT(" 4: cascade used per pixel (0 red, 1 green, 2 blue, 3 white; darker = lookup outside the cell).\n")
	TEXT(" 5: red = caster in the atlas at this pixel, green = receiver behind it, blue = lookup outside the cell.\n")
	TEXT(" 6: raw depths, red = receiver, green = caster."),
	ECVF_RenderThreadSafe);

namespace ColoredCascadeShadows
{
	bool IsGloballyEnabled()
	{
		return CVarColoredCascadeShadowsEnable.GetValueOnRenderThread() != 0;
	}

	int32 GetDebugLevel()
	{
		return CVarColoredCascadeShadowsDebug.GetValueOnRenderThread();
	}

	bool ShouldLogThisCall(int32 Site)
	{
		static uint32 Counters[8] = {};
		return GetDebugLevel() > 0 && ((Counters[Site & 7]++ % 120) == 0);
	}

}

// ------------------------------------------------------------------------------------------------------------------------------------
// Pass 2: atlas -> screen space tint
// ------------------------------------------------------------------------------------------------------------------------------------

class FColoredCascadeTintPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FColoredCascadeTintPS);
	SHADER_USE_PARAMETER_STRUCT(FColoredCascadeTintPS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PreviousTintTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CascadeColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CascadeDepthTexture)
		SHADER_PARAMETER(FMatrix, ScreenToShadow)
		SHADER_PARAMETER(FVector4, SplitParams)
		SHADER_PARAMETER(float, TransitionScale)
		SHADER_PARAMETER(float, Strength)
		SHADER_PARAMETER(int32, DebugMode)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FColoredCascadeTintPS, "/Engine/Private/ColoredCascadeShadows.usf", "TintMainPS", SF_Pixel);

/**
 * Uses the engine's exact screen-to-shadow transform, then removes the native atlas
 * tile offset/scale because each colored cascade is stored in a standalone texture.
 */
static FMatrix GetScreenToColoredShadowMatrix(const FProjectedShadowInfo* Shadow, const FSceneView& View)
{
	const FIntPoint ShadowBufferResolution = Shadow->GetShadowBufferResolution();
	check(ShadowBufferResolution.X > 0 && ShadowBufferResolution.Y > 0);

	const float InvBufferResolutionX = 1.0f / float(ShadowBufferResolution.X);
	const float InvBufferResolutionY = 1.0f / float(ShadowBufferResolution.Y);
	const float TileScaleX = float(Shadow->ResolutionX) * InvBufferResolutionX;
	const float TileScaleY = float(Shadow->ResolutionY) * InvBufferResolutionY;
	const float TileOffsetX = float(Shadow->X + Shadow->BorderSize) * InvBufferResolutionX;
	const float TileOffsetY = float(Shadow->Y + Shadow->BorderSize) * InvBufferResolutionY;

	check(TileScaleX > 0.0f && TileScaleY > 0.0f);

	const FMatrix AtlasToLocalCascade(
		FPlane(1.0f / TileScaleX, 0, 0, 0),
		FPlane(0, 1.0f / TileScaleY, 0, 0),
		FPlane(0, 0, 1, 0),
		FPlane(-TileOffsetX / TileScaleX, -TileOffsetY / TileScaleY, 0, 1));

	return Shadow->GetScreenToShadowMatrix(View) * AtlasToLocalCascade;
}

BEGIN_SHADER_PARAMETER_STRUCT(FColoredCascadeCasterPassParameters, )
	RENDER_TARGET_BINDING_SLOTS()
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture, ERHIAccess::SRVGraphics)
END_SHADER_PARAMETER_STRUCT()

FRDGTextureRef RenderColoredCascadeShadowTint(
	FRDGBuilder& GraphBuilder,
	FSceneRenderer& SceneRenderer,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer,
	FIntPoint SceneTextureExtent,
	const FLightSceneInfo& LightSceneInfo)
{
	if (!ColoredCascadeShadows::IsGloballyEnabled() || SceneRenderer.FeatureLevel < ERHIFeatureLevel::SM5)
	{
		return nullptr;
	}
	const FLightSceneProxy* LightProxy = LightSceneInfo.Proxy;
	if (!LightProxy || LightProxy->GetLightType() != LightType_Directional || !LightProxy->CastsColoredTranslucentCascadeShadows())
	{
		return nullptr;
	}

	FVisibleLightInfo& VisibleLightInfo = SceneRenderer.VisibleLightInfos[LightSceneInfo.Id];
	VisibleLightInfo.ColoredCascadeShadows.Reset();
	const bool bLog = ColoredCascadeShadows::ShouldLogThisCall(0);
	const int32 NumViews = SceneRenderer.Views.Num();

	// Keep the exact projected cascades created by the Directional Light shadow setup.
	// Remove nulls before sorting because UE's pointer-array sort dereferences elements
	// in its comparator. Sort near-to-far using the actual split distances from the engine.
	TArray<FProjectedShadowInfo*, TInlineAllocator<16>> NativeCascades;
	for (FProjectedShadowInfo* Shadow : VisibleLightInfo.ShadowsToProject)
	{
		if (Shadow && Shadow->bAllocated && Shadow->IsWholeSceneDirectionalShadow()
			&& !Shadow->bRayTracedDistanceField && !Shadow->bReflectiveShadowmap
			&& Shadow->DependentView && Shadow->ShadowDepthView && Shadow->RenderTargets.DepthTarget)
		{
			NativeCascades.Add(Shadow);
		}
	}
	// Use the exact comparator used by native shadow projection: descending split
	// index, i.e. far-to-near. Do not reconstruct order from split distances; far
	// cascades and overlapping fade planes are ordered by ShadowSplitIndex natively.
	NativeCascades.Sort(FCompareFProjectedShadowInfoBySplitIndex());

	for (FProjectedShadowInfo* Shadow : NativeCascades)
	{
		const int32 ViewIndex = SceneRenderer.Views.IndexOfByPredicate(
			[Shadow](const FViewInfo& View) { return &View == Shadow->DependentView; });
		if (ViewIndex == INDEX_NONE || !Shadow->FadeAlphas.IsValidIndex(ViewIndex)
			|| Shadow->FadeAlphas[ViewIndex] <= (1.0f / 256.0f))
		{
			continue;
		}
		FColoredCascadeShadowEntry& Entry = VisibleLightInfo.ColoredCascadeShadows.AddDefaulted_GetRef();
		Entry.Shadow = Shadow;
		Entry.CellIndex = ViewIndex;
		Entry.CellRect = FIntRect(0, 0, Shadow->ResolutionX, Shadow->ResolutionY);
	}
	if (VisibleLightInfo.ColoredCascadeShadows.Num() == 0)
	{
		return nullptr;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "ColoredCascadeShadows");
	const FRDGTextureDesc TintDesc = FRDGTextureDesc::Create2D(
		SceneTextureExtent, PF_FloatR11G11B10, FClearValueBinding::White,
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTextureRef TintTextures[2] =
	{
		GraphBuilder.CreateTexture(TintDesc, TEXT("ColoredCascadeShadowTintA")),
		GraphBuilder.CreateTexture(TintDesc, TEXT("ColoredCascadeShadowTintB"))
	};
	AddClearRenderTargetPass(GraphBuilder, TintTextures[0], FLinearColor::White);
	FRDGTextureRef CurrentTintTexture = TintTextures[0];
	int32 CurrentTintIndex = 0;
	const float Strength = FMath::Clamp(CVarColoredCascadeShadowsStrength.GetValueOnRenderThread(), 0.0f, 1.0f);
	const int32 DebugMode = CVarColoredCascadeShadowsDebug.GetValueOnRenderThread();

	// Native projection order is far-to-near. Preserve it so each cascade blends
	// against the next farther cascade already accumulated in the tint texture.
	for (int32 ViewIndex = 0; ViewIndex < NumViews; ++ViewIndex)
	{
		const FViewInfo& View = SceneRenderer.Views[ViewIndex];
		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
		for (int32 EntryIndex = 0; EntryIndex < VisibleLightInfo.ColoredCascadeShadows.Num(); ++EntryIndex)
		{
			const FColoredCascadeShadowEntry& Entry = VisibleLightInfo.ColoredCascadeShadows[EntryIndex];
			if (Entry.CellIndex != ViewIndex || !Entry.Shadow) { continue; }
			FProjectedShadowInfo* Shadow = const_cast<FProjectedShadowInfo*>(Entry.Shadow);
			const FIntPoint CascadeExtent(
				FMath::Max(static_cast<int32>(Shadow->ResolutionX), 1),
				FMath::Max(static_cast<int32>(Shadow->ResolutionY), 1));
			const FIntRect CascadeRect(FIntPoint::ZeroValue, CascadeExtent);

			const FRDGTextureDesc ColorDesc = FRDGTextureDesc::Create2D(
				CascadeExtent, PF_FloatRGBA, FClearValueBinding::White,
				TexCreate_RenderTargetable | TexCreate_ShaderResource);
			const FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
				CascadeExtent, PF_R32_FLOAT, FClearValueBinding::Black,
				TexCreate_RenderTargetable | TexCreate_ShaderResource);
			FRDGTextureRef CascadeColorTexture = GraphBuilder.CreateTexture(ColorDesc, TEXT("ColoredCascadeTransmittance"));
			FRDGTextureRef CascadeDepthTexture = GraphBuilder.CreateTexture(DepthDesc, TEXT("ColoredCascadeTranslucentDepth"));
			FRDGTextureRef OpaqueDepthTexture = GraphBuilder.RegisterExternalTexture(
				Shadow->RenderTargets.DepthTarget, TEXT("ColoredCascadeOpaqueDepth"));
			if (!OpaqueDepthTexture) { continue; }

			FColoredCascadeCasterPassParameters* CasterParameters =
				GraphBuilder.AllocParameters<FColoredCascadeCasterPassParameters>();
			CasterParameters->RenderTargets[0] = FRenderTargetBinding(CascadeColorTexture, ERenderTargetLoadAction::EClear);
			CasterParameters->RenderTargets[1] = FRenderTargetBinding(CascadeDepthTexture, ERenderTargetLoadAction::EClear);
			CasterParameters->OpaqueDepthTexture = OpaqueDepthTexture;
			FSceneRenderer* Renderer = &SceneRenderer;
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ColoredCascadeCaster Split%d", Shadow->CascadeSettings.ShadowSplitIndex),
				CasterParameters, ERDGPassFlags::Raster,
				[Shadow, Renderer, CascadeRect, OpaqueDepthTexture](FRHICommandList& RHICmdList)
				{
					Shadow->RenderColoredTranslucencyDepths(RHICmdList, Renderer, CascadeRect, OpaqueDepthTexture->GetRHI());
				});

			const int32 NextTintIndex = 1 - CurrentTintIndex;
			FRDGTextureRef NextTintTexture = TintTextures[NextTintIndex];
			AddCopyTexturePass(GraphBuilder, CurrentTintTexture, NextTintTexture);

			FColoredCascadeTintPS::FParameters* TintParameters =
				GraphBuilder.AllocParameters<FColoredCascadeTintPS::FParameters>();
			TintParameters->View = View.ViewUniformBuffer;
			TintParameters->SceneTextures = SceneTexturesUniformBuffer;
			TintParameters->PreviousTintTexture = CurrentTintTexture;
			TintParameters->CascadeColorTexture = CascadeColorTexture;
			TintParameters->CascadeDepthTexture = CascadeDepthTexture;
			TintParameters->ScreenToShadow = GetScreenToColoredShadowMatrix(Shadow, View);
			const FShadowCascadeSettings& Cascade = Shadow->CascadeSettings;
			const float InvFadePlaneLength = Cascade.FadePlaneLength > 0.0f ? 1.0f / Cascade.FadePlaneLength : 0.0f;
			TintParameters->SplitParams = FVector4(Cascade.SplitNear, Cascade.SplitFar, Cascade.FadePlaneOffset, InvFadePlaneLength);
			// The native CSM transition is deliberately wide for opaque self-shadow acne; using it here erases transmission when a pane is close to its receiver.
			TintParameters->TransitionScale = 100000.0f;
			TintParameters->Strength = Strength * FMath::Clamp(Shadow->FadeAlphas[ViewIndex], 0.0f, 1.0f);
			TintParameters->DebugMode = DebugMode;
			TintParameters->RenderTargets[0] = FRenderTargetBinding(NextTintTexture, ERenderTargetLoadAction::ELoad);
			TShaderMapRef<FColoredCascadeTintPS> PixelShader(View.ShaderMap);
			FPixelShaderUtils::AddFullscreenPass(
				GraphBuilder, View.ShaderMap,
				RDG_EVENT_NAME("ColoredCascadeTint View%d Split%d", ViewIndex, Shadow->CascadeSettings.ShadowSplitIndex),
				PixelShader, TintParameters, View.ViewRect);
			CurrentTintTexture = NextTintTexture;
			CurrentTintIndex = NextTintIndex;
		}
	}
	if (bLog)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ColoredCSM] scheduled %d native cascade passes across %d views"),
			VisibleLightInfo.ColoredCascadeShadows.Num(), NumViews);
	}
	return CurrentTintTexture;
}
