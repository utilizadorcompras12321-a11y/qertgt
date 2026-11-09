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

static_assert(ColoredCascadeShadows::MaxCascades == 4, "Shader parameter arrays below are sized with the literal 4");

static TAutoConsoleVariable<int32> CVarColoredCascadeShadowsEnable(
	TEXT("r.ColoredCascadeShadows.Enable"),
	1,
	TEXT("Global switch for colored translucent cascaded shadows. The per light toggle (Colored Translucent Cascade Shadows) is still required.\n")
	TEXT(" 0: off, 1: on (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarColoredCascadeShadowsMaxCascades(
	TEXT("r.ColoredCascadeShadows.MaxCascades"),
	3,
	TEXT("Number of cascades (counted from the camera) that render colored translucent shadows. Clamped to 1..4."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarColoredCascadeShadowsDepthBias(
	TEXT("r.ColoredCascadeShadows.DepthBias"),
	5.0f,
	TEXT("World space depth bias (in Unreal units) used when deciding whether an opaque receiver is behind a translucent caster."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarColoredCascadeShadowsTransition(
	TEXT("r.ColoredCascadeShadows.Transition"),
	30.0f,
	TEXT("World space distance (in Unreal units) over which the tint fades in behind a translucent caster. Larger = softer."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarColoredCascadeShadowsStrength(
	TEXT("r.ColoredCascadeShadows.Strength"),
	1.0f,
	TEXT("Blend between no tint (0) and full colored translucent shadow (1)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarColoredCascadeShadowsDebug(
	TEXT("r.ColoredCascadeShadows.Debug"),
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

	int32 GetMaxCascades()
	{
		return FMath::Clamp(CVarColoredCascadeShadowsMaxCascades.GetValueOnRenderThread(), 1, MaxCascades);
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
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AtlasTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthAtlasTexture)
		SHADER_PARAMETER(int32, NumCascades)
		SHADER_PARAMETER(float, Strength)
		SHADER_PARAMETER(int32, DebugMode)
		// Screen space (W = view depth) -> shadow space (xy = 0..1 inside the cascade cell, z = normalized light depth).
		SHADER_PARAMETER_ARRAY(FMatrix, ScreenToShadow, [4])
		// Cell rectangle of the cascade inside the atlas, in atlas pixels: x, y, width, height.
		SHADER_PARAMETER_ARRAY(FVector4, CellRect, [4])
		// SplitNear, SplitFar, FadePlaneOffset, InvFadePlaneLength (0 = no fade plane). All in view space distances.
		SHADER_PARAMETER_ARRAY(FVector4, SplitParams, [4])
		// Normalized depth bias, inverse normalized transition length.
		SHADER_PARAMETER_ARRAY(FVector4, DepthParams, [4])
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FColoredCascadeTintPS, "/Engine/Private/ColoredCascadeShadows.usf", "TintMainPS", SF_Pixel);

/** Same transform as FProjectedShadowInfo::GetScreenToShadowMatrix, but producing 0..1 UVs relative to ONE cascade cell (no atlas offset, no border). */
static FMatrix GetScreenToColoredShadowMatrix(const FProjectedShadowInfo* Shadow, const FViewInfo& View)
{
	const FMatrix ViewDependentTransform =
		// Z of the position being transformed is the view space Z; turn it into post projection space.
		FMatrix(
			FPlane(1, 0, 0, 0),
			FPlane(0, 1, 0, 0),
			FPlane(0, 0, View.ViewMatrices.GetProjectionMatrix().M[2][2], 1),
			FPlane(0, 0, View.ViewMatrices.GetProjectionMatrix().M[3][2], 0)) *
		View.ViewMatrices.GetInvTranslatedViewProjectionMatrix() *
		FTranslationMatrix(-View.ViewMatrices.GetPreViewTranslation());

	const FMatrix ShadowMapDependentTransform =
		FTranslationMatrix(Shadow->PreShadowTranslation) *
		Shadow->SubjectAndReceiverMatrix *
		// xy: -1..1 -> 0..1 (y flipped like the regular shadow lookup), z normalized by the subject depth range.
		FMatrix(
			FPlane(0.5f, 0, 0, 0),
			FPlane(0, -0.5f, 0, 0),
			FPlane(0, 0, Shadow->InvMaxSubjectDepth, 0),
			FPlane(0.5f, 0.5f, 0, 1));

	return ViewDependentTransform * ShadowMapDependentTransform;
}

BEGIN_SHADER_PARAMETER_STRUCT(FColoredCascadeCasterPassParameters, )
	RENDER_TARGET_BINDING_SLOTS()
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
	if (LightProxy->GetLightType() != LightType_Directional || !LightProxy->CastsColoredTranslucentCascadeShadows())
	{
		return nullptr;
	}

	FVisibleLightInfo& VisibleLightInfo = SceneRenderer.VisibleLightInfos[LightSceneInfo.Id];
	VisibleLightInfo.ColoredCascadeShadows.Reset();

	const bool bLog = ColoredCascadeShadows::ShouldLogThisCall(0);
	if (bLog)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ColoredCSM] Light %d (%s): toggle on, ShadowsToProject=%d"), LightSceneInfo.Id, *LightProxy->GetComponentName().ToString(), VisibleLightInfo.ShadowsToProject.Num());
	}

	// Pick the cascades. ShadowsToProject is already known here (it is filled in InitViews); it is sorted by split index.
	const int32 MaxCascadesPerView = ColoredCascadeShadows::GetMaxCascades();

	TArray<int32, TInlineAllocator<2>> CascadesPerView;
	CascadesPerView.SetNumZeroed(SceneRenderer.Views.Num());

	for (FProjectedShadowInfo* Shadow : VisibleLightInfo.ShadowsToProject)
	{
		if (bLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ColoredCSM]   shadow split=%d allocated=%d wholeSceneDir=%d rtdf=%d far=%d dependentView=%d depthView=%d"),
				Shadow->CascadeSettings.ShadowSplitIndex, (int32)Shadow->bAllocated, Shadow->IsWholeSceneDirectionalShadow() ? 1 : 0, (int32)Shadow->bRayTracedDistanceField,
				Shadow->CascadeSettings.bFarShadowCascade ? 1 : 0, Shadow->DependentView != nullptr ? 1 : 0, Shadow->ShadowDepthView != nullptr ? 1 : 0);
		}

		if (VisibleLightInfo.ColoredCascadeShadows.Num() >= ColoredCascadeShadows::MaxCells)
		{
			break;
		}

		if (!Shadow->bAllocated
			|| !Shadow->IsWholeSceneDirectionalShadow()
			|| Shadow->bRayTracedDistanceField
			|| Shadow->bReflectiveShadowmap
			|| Shadow->CascadeSettings.bFarShadowCascade
			|| Shadow->DependentView == nullptr
			|| Shadow->ShadowDepthView == nullptr)
		{
			continue;
		}

		const int32 ViewIndex = SceneRenderer.Views.IndexOfByPredicate([Shadow](const FViewInfo& InView) { return &InView == Shadow->DependentView; });
		if (ViewIndex == INDEX_NONE || CascadesPerView[ViewIndex] >= MaxCascadesPerView)
		{
			continue;
		}

		CascadesPerView[ViewIndex]++;

		FColoredCascadeShadowEntry& Entry = VisibleLightInfo.ColoredCascadeShadows.AddDefaulted_GetRef();
		Entry.Shadow = Shadow;
		Entry.CellIndex = VisibleLightInfo.ColoredCascadeShadows.Num() - 1;
		Entry.CellRect = FIntRect(0, 0, Shadow->ResolutionX, Shadow->ResolutionY);
	}

	const int32 NumCells = VisibleLightInfo.ColoredCascadeShadows.Num();
	if (NumCells == 0)
	{
		return nullptr;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "ColoredCascadeShadows");

	// Pack variable-sized cells in two columns. Each cell keeps the exact resolution allocated to its source CSM cascade.
	uint32 MaxCellWidth = 1;
	uint32 MaxCellHeight = 1;
	for (const FColoredCascadeShadowEntry& Entry : VisibleLightInfo.ColoredCascadeShadows)
	{
		MaxCellWidth = FMath::Max(MaxCellWidth, Entry.Shadow->ResolutionX);
		MaxCellHeight = FMath::Max(MaxCellHeight, Entry.Shadow->ResolutionY);
	}
	const int32 AtlasColumns = FMath::Min(2, NumCells);
	const int32 AtlasRows = FMath::DivideAndRoundUp(NumCells, AtlasColumns);
	for (FColoredCascadeShadowEntry& Entry : VisibleLightInfo.ColoredCascadeShadows)
	{
		const int32 Column = Entry.CellIndex % AtlasColumns;
		const int32 Row = Entry.CellIndex / AtlasColumns;
		const int32 CellWidth = FMath::Max(static_cast<int32>(Entry.Shadow->ResolutionX), 1);
		const int32 CellHeight = FMath::Max(static_cast<int32>(Entry.Shadow->ResolutionY), 1);
		Entry.CellRect = FIntRect(Column * MaxCellWidth, Row * MaxCellHeight,
			Column * MaxCellWidth + CellWidth, Row * MaxCellHeight + CellHeight);
	}
	const FIntPoint AtlasSize(AtlasColumns * MaxCellWidth, AtlasRows * MaxCellHeight);

	// Color atlas: RGB colored transmittance (cleared to 1). Depth atlas: nearest translucent depth in 32 bit float (cleared to 1 = far plane).
	// The depth must not be 16 bit: it is normalized by the cascade depth range (thousands of units), so half floats cannot resolve small distances.
	const FRDGTextureDesc AtlasDesc = FRDGTextureDesc::Create2D(AtlasSize, PF_FloatRGBA, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource);
	const FRDGTextureDesc DepthAtlasDesc = FRDGTextureDesc::Create2D(AtlasSize, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTextureRef AtlasTexture = GraphBuilder.CreateTexture(AtlasDesc, TEXT("ColoredCascadeShadowAtlas"));
	FRDGTextureRef DepthAtlasTexture = GraphBuilder.CreateTexture(DepthAtlasDesc, TEXT("ColoredCascadeShadowDepthAtlas"));

	// Caster pass. Added to the graph here, so it executes after the regular shadow depth pass (which is also a graph pass).
	{
		TArray<FColoredCascadeShadowEntry> Entries;
		Entries.Append(VisibleLightInfo.ColoredCascadeShadows);
		FSceneRenderer* Renderer = &SceneRenderer;

		FColoredCascadeCasterPassParameters* CasterParameters = GraphBuilder.AllocParameters<FColoredCascadeCasterPassParameters>();
		CasterParameters->RenderTargets[0] = FRenderTargetBinding(AtlasTexture, ERenderTargetLoadAction::EClear);
		CasterParameters->RenderTargets[1] = FRenderTargetBinding(DepthAtlasTexture, ERenderTargetLoadAction::EClear);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ColoredCascadeShadowMaps %d cascades", NumCells),
			CasterParameters,
			ERDGPassFlags::Raster,
			[Renderer, Entries, bLog, AtlasSize](FRHICommandList& RHICmdList)
		{
			// The caster pixel shader samples the original opaque CSM depth. These shadow maps are
			// produced by the legacy shadow renderer, outside RDG, so explicitly transition each
			// unique pooled depth target to graphics SRV before any cascade samples it.
			TSet<IPooledRenderTarget*, DefaultKeyFuncs<IPooledRenderTarget*>, SceneRenderingSetAllocator> OpaqueDepthTargets;
			TArray<FRHITransitionInfo, SceneRenderingAllocator> OpaqueDepthTransitions;
			for (const FColoredCascadeShadowEntry& Entry : Entries)
			{
				IPooledRenderTarget* OpaqueDepthTarget = Entry.Shadow->RenderTargets.DepthTarget;
				if (OpaqueDepthTarget)
				{
					bool bAlreadyAdded = false;
					OpaqueDepthTargets.Emplace(OpaqueDepthTarget, &bAlreadyAdded);
					if (!bAlreadyAdded)
					{
						OpaqueDepthTransitions.Add(FRHITransitionInfo(
							OpaqueDepthTarget->GetShaderResourceRHI(),
							ERHIAccess::Unknown,
							ERHIAccess::SRVMask));
					}
				}
			}
			if (OpaqueDepthTransitions.Num() > 0)
			{
				RHICmdList.Transition(OpaqueDepthTransitions);
			}

			for (const FColoredCascadeShadowEntry& Entry : Entries)
			{
				const FIntRect& CellRect = Entry.CellRect;

				// Const cast: ShadowsToProject stores non const pointers, the entry only keeps them const for the projection pass.
				const_cast<FProjectedShadowInfo*>(Entry.Shadow)->RenderColoredTranslucencyDepths(RHICmdList, Renderer, CellRect);
			}

			if (bLog)
			{
				UE_LOG(LogTemp, Warning, TEXT("[ColoredCSM] caster pass executed: %d cascade cells into a %dx%d atlas"), Entries.Num(), AtlasSize.X, AtlasSize.Y);
			}
		});
	}

	RDG_EVENT_SCOPE(GraphBuilder, "ColoredCascadeShadowTint");

	const FRDGTextureDesc TintDesc = FRDGTextureDesc::Create2D(SceneTextureExtent, PF_FloatR11G11B10, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTextureRef TintTexture = GraphBuilder.CreateTexture(TintDesc, TEXT("ColoredCascadeShadowTint"));
	AddClearRenderTargetPass(GraphBuilder, TintTexture, FLinearColor::White);

	const float DepthBiasWorld = FMath::Max(CVarColoredCascadeShadowsDepthBias.GetValueOnRenderThread(), 0.0f);
	const float TransitionWorld = FMath::Max(CVarColoredCascadeShadowsTransition.GetValueOnRenderThread(), 1.0f);
	const float Strength = FMath::Clamp(CVarColoredCascadeShadowsStrength.GetValueOnRenderThread(), 0.0f, 1.0f);

	for (int32 ViewIndex = 0; ViewIndex < SceneRenderer.Views.Num(); ++ViewIndex)
	{
		const FViewInfo& View = SceneRenderer.Views[ViewIndex];

		FColoredCascadeTintPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FColoredCascadeTintPS::FParameters>();
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTextures = SceneTexturesUniformBuffer;
		PassParameters->AtlasTexture = AtlasTexture;
		PassParameters->DepthAtlasTexture = DepthAtlasTexture;
		PassParameters->Strength = Strength;
		PassParameters->DebugMode = CVarColoredCascadeShadowsDebug.GetValueOnRenderThread();
		PassParameters->RenderTargets[0] = FRenderTargetBinding(TintTexture, ERenderTargetLoadAction::ELoad);

		int32 NumCascades = 0;

		for (const FColoredCascadeShadowEntry& Entry : VisibleLightInfo.ColoredCascadeShadows)
		{
			const FProjectedShadowInfo* Shadow = Entry.Shadow;
			if (Shadow->DependentView != &View || NumCascades >= ColoredCascadeShadows::MaxCascades)
			{
				continue;
			}


			const FShadowCascadeSettings& Cascade = Shadow->CascadeSettings;
			const float InvFadePlaneLength = Cascade.FadePlaneLength > 0.0f ? 1.0f / Cascade.FadePlaneLength : 0.0f;
			const float InvDepthRange = Shadow->InvMaxSubjectDepth;

			PassParameters->ScreenToShadow[NumCascades] = GetScreenToColoredShadowMatrix(Shadow, View);
			PassParameters->CellRect[NumCascades] = FVector4(Entry.CellRect.Min.X, Entry.CellRect.Min.Y, Entry.CellRect.Width(), Entry.CellRect.Height());
			PassParameters->SplitParams[NumCascades] = FVector4(Cascade.SplitNear, Cascade.SplitFar, Cascade.FadePlaneOffset, InvFadePlaneLength);
			PassParameters->DepthParams[NumCascades] = FVector4(DepthBiasWorld * InvDepthRange, 1.0f / FMath::Max(TransitionWorld * InvDepthRange, 1e-6f), 0.0f, 0.0f);

			++NumCascades;
		}

		if (NumCascades == 0)
		{
			continue;
		}

		PassParameters->NumCascades = NumCascades;

		// Unused array entries keep a harmless value so the shader never reads garbage.
		for (int32 Index = NumCascades; Index < ColoredCascadeShadows::MaxCascades; ++Index)
		{
			PassParameters->ScreenToShadow[Index] = FMatrix::Identity;
			PassParameters->CellRect[Index] = FVector4(0, 0, 1, 1);
			PassParameters->SplitParams[Index] = FVector4(0, 0, 0, 0);
			PassParameters->DepthParams[Index] = FVector4(0, 1, 0, 0);
		}

		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

		TShaderMapRef<FColoredCascadeTintPS> PixelShader(View.ShaderMap);
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			View.ShaderMap,
			RDG_EVENT_NAME("ColoredCascadeTint View%d", ViewIndex),
			PixelShader,
			PassParameters,
			View.ViewRect);
	}

	return TintTexture;
}
