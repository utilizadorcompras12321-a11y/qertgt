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

static_assert(ColoredCascadeShadows::MaxCascades == 8, "Shader parameter arrays below are sized with the literal 8");

static TAutoConsoleVariable<int32> CVarColoredCascadeShadowsEnable(
	TEXT("r.ColoredCascadeShadows.Enable"),
	1,
	TEXT("Global switch for colored translucent cascaded shadows. The per light toggle (Colored Translucent Cascade Shadows) is still required.\n")
	TEXT(" 0: off, 1: on (default)"),
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
		SHADER_PARAMETER_ARRAY(FMatrix, ScreenToShadow, [8])
		// Cell rectangle of the cascade inside the atlas, in atlas pixels: x, y, width, height.
		SHADER_PARAMETER_ARRAY(FVector4, CellRect, [8])
		// SplitNear, SplitFar, FadePlaneOffset, InvFadePlaneLength (0 = no fade plane). All in view space distances.
		SHADER_PARAMETER_ARRAY(FVector4, SplitParams, [8])
		// Normalized depth bias, inverse normalized transition length.
		SHADER_PARAMETER_ARRAY(FVector4, DepthParams, [8])
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
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture0, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture1, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture2, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture3, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture4, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture5, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture6, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture7, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture8, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture9, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture10, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture11, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture12, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture13, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture14, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture15, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture16, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture17, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture18, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture19, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture20, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture21, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture22, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture23, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture24, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture25, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture26, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture27, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture28, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture29, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture30, ERHIAccess::SRVGraphics)
	RDG_TEXTURE_ACCESS(OpaqueDepthTexture31, ERHIAccess::SRVGraphics)
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

	// Reuse allocated directional CSM cascades. Shader arrays hold four regular splits
	// plus one optional far split. The per-view counter is sized from the actual renderer
	// views; auxiliary views must not index a fixed split-screen-sized array.
	TArray<int32, TInlineAllocator<8>> ViewCascadeCounts;
	ViewCascadeCounts.Init(0, SceneRenderer.Views.Num());
	TArray<FProjectedShadowInfo*, TInlineAllocator<8>> SortedShadows;
	SortedShadows.Append(VisibleLightInfo.ShadowsToProject);
	SortedShadows.Sort([](const FProjectedShadowInfo& A, const FProjectedShadowInfo& B)
	{
		// Far shadow cascades must follow all regular splits even if their split index
		// uses a sentinel or a value outside the regular cascade sequence.
		if (A.CascadeSettings.bFarShadowCascade != B.CascadeSettings.bFarShadowCascade)
		{
			return !A.CascadeSettings.bFarShadowCascade;
		}
		return A.CascadeSettings.ShadowSplitIndex < B.CascadeSettings.ShadowSplitIndex;
	});

	for (FProjectedShadowInfo* Shadow : SortedShadows)
	{
		if (Shadow == nullptr)
		{
			continue;
		}

		if (bLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ColoredCSM]   shadow split=%d allocated=%d wholeSceneDir=%d rtdf=%d far=%d dependentView=%d depthView=%d"),
				Shadow->CascadeSettings.ShadowSplitIndex, (int32)Shadow->bAllocated, Shadow->IsWholeSceneDirectionalShadow() ? 1 : 0, (int32)Shadow->bRayTracedDistanceField,
				Shadow->CascadeSettings.bFarShadowCascade ? 1 : 0, Shadow->DependentView != nullptr ? 1 : 0, Shadow->ShadowDepthView != nullptr ? 1 : 0);
		}

		if (!Shadow->bAllocated
			|| !Shadow->IsWholeSceneDirectionalShadow()
			|| Shadow->bRayTracedDistanceField
			|| Shadow->bReflectiveShadowmap
			|| Shadow->DependentView == nullptr
			|| Shadow->ShadowDepthView == nullptr)
		{
			continue;
		}

		const int32 ViewIndex = SceneRenderer.Views.IndexOfByPredicate([Shadow](const FViewInfo& InView) { return &InView == Shadow->DependentView; });
		if (ViewIndex == INDEX_NONE)
		{
			continue;
		}

		// Runtime guards are required here: check/checkf may be compiled out in Shipping.
		if (!ViewCascadeCounts.IsValidIndex(ViewIndex)
			|| ViewCascadeCounts[ViewIndex] >= ColoredCascadeShadows::MaxCascades
			|| VisibleLightInfo.ColoredCascadeShadows.Num() >= ColoredCascadeShadows::MaxCells
			|| Shadow->RenderTargets.DepthTarget == nullptr)
		{
			continue;
		}
		++ViewCascadeCounts[ViewIndex];

		FColoredCascadeShadowEntry& Entry = VisibleLightInfo.ColoredCascadeShadows.AddDefaulted_GetRef();
		Entry.Shadow = Shadow;
		Entry.CellIndex = VisibleLightInfo.ColoredCascadeShadows.Num() - 1;
		Entry.CellRect = FIntRect(0, 0, Shadow->ResolutionX, Shadow->ResolutionY);
	}

	int32 NumCells = VisibleLightInfo.ColoredCascadeShadows.Num();
	if (NumCells == 0)
	{
		return nullptr;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "ColoredCascadeShadows");

	// Pack variable-sized cells without rescaling them. The RHI limit is platform-specific;
	// a fixed 4x5 grid can exceed it (for example, 16384x20480 with 4096px cascades).
	uint32 MaxCellWidth = 1;
	uint32 MaxCellHeight = 1;
	for (const FColoredCascadeShadowEntry& Entry : VisibleLightInfo.ColoredCascadeShadows)
	{
		MaxCellWidth = FMath::Max(MaxCellWidth, Entry.Shadow->ResolutionX);
		MaxCellHeight = FMath::Max(MaxCellHeight, Entry.Shadow->ResolutionY);
	}
	const int32 MaxTextureDimension = FMath::Max(static_cast<int32>(GMaxTextureDimensions), 1);
	const int32 ColumnsThatFit = MaxCellWidth <= static_cast<uint32>(MaxTextureDimension)
		? MaxTextureDimension / static_cast<int32>(MaxCellWidth) : 0;
	const int32 RowsThatFit = MaxCellHeight <= static_cast<uint32>(MaxTextureDimension)
		? MaxTextureDimension / static_cast<int32>(MaxCellHeight) : 0;
	const int32 AtlasColumns = FMath::Min(NumCells, ColumnsThatFit);
	if (AtlasColumns <= 0 || RowsThatFit <= 0)
	{
		VisibleLightInfo.ColoredCascadeShadows.Reset();
		return nullptr;
	}
	const int32 MaxCellsThatFit = FMath::Min(ColoredCascadeShadows::MaxCells, AtlasColumns * RowsThatFit);
	if (NumCells > MaxCellsThatFit)
	{
		if (bLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ColoredCSM] Atlas limit: keeping %d of %d cascades at native resolution (RHI max %d, cell max %ux%u)"),
				MaxCellsThatFit, NumCells, MaxTextureDimension, MaxCellWidth, MaxCellHeight);
		}
		VisibleLightInfo.ColoredCascadeShadows.SetNum(MaxCellsThatFit, false);
		NumCells = MaxCellsThatFit;
	}
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

	// Color atlas: RGB colored transmittance (cleared to 1). Depth atlas: farthest translucent depth in 32 bit float (cleared to 0 = no casters).
	// The depth must not be 16 bit: it is normalized by the cascade depth range (thousands of units), so half floats cannot resolve small distances.
	const FRDGTextureDesc AtlasDesc = FRDGTextureDesc::Create2D(AtlasSize, PF_FloatRGBA, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource);
	const FRDGTextureDesc DepthAtlasDesc = FRDGTextureDesc::Create2D(AtlasSize, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTextureRef AtlasTexture = GraphBuilder.CreateTexture(AtlasDesc, TEXT("ColoredCascadeShadowAtlas"));
	FRDGTextureRef DepthAtlasTexture = GraphBuilder.CreateTexture(DepthAtlasDesc, TEXT("ColoredCascadeShadowDepthAtlas"));

	// Caster pass. The registered opaque CSM depth inputs below declare its dependency on the regular shadow-depth pass.
	{
		TArray<FColoredCascadeShadowEntry> Entries;
		Entries.Append(VisibleLightInfo.ColoredCascadeShadows);
		FSceneRenderer* Renderer = &SceneRenderer;

		FColoredCascadeCasterPassParameters* CasterParameters = GraphBuilder.AllocParameters<FColoredCascadeCasterPassParameters>();
		CasterParameters->RenderTargets[0] = FRenderTargetBinding(AtlasTexture, ERenderTargetLoadAction::EClear);
		CasterParameters->RenderTargets[1] = FRenderTargetBinding(DepthAtlasTexture, ERenderTargetLoadAction::EClear);

		// Import every original opaque CSM depth target into RDG. This declares the caster
		// pass's SRV reads and lets RDG manage the required resource access transitions.
		TArray<FRDGTextureRef> OpaqueDepthTextures;
		OpaqueDepthTextures.Reserve(Entries.Num());
		for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
		{
			const FProjectedShadowInfo* Shadow = Entries[EntryIndex].Shadow;
			if (Shadow == nullptr || Shadow->RenderTargets.DepthTarget == nullptr
				|| EntryIndex >= ColoredCascadeShadows::MaxCells)
			{
				continue;
			}
			FRDGTextureRef OpaqueDepthTexture = GraphBuilder.RegisterExternalTexture(Shadow->RenderTargets.DepthTarget, TEXT("ColoredCascadeOpaqueShadowDepth"));
			if (OpaqueDepthTexture == nullptr)
			{
				continue;
			}
			OpaqueDepthTextures.Add(OpaqueDepthTexture);

			switch (EntryIndex)
			{
			case 0: CasterParameters->OpaqueDepthTexture0 = OpaqueDepthTexture; break;
			case 1: CasterParameters->OpaqueDepthTexture1 = OpaqueDepthTexture; break;
			case 2: CasterParameters->OpaqueDepthTexture2 = OpaqueDepthTexture; break;
			case 3: CasterParameters->OpaqueDepthTexture3 = OpaqueDepthTexture; break;
			case 4: CasterParameters->OpaqueDepthTexture4 = OpaqueDepthTexture; break;
			case 5: CasterParameters->OpaqueDepthTexture5 = OpaqueDepthTexture; break;
			case 6: CasterParameters->OpaqueDepthTexture6 = OpaqueDepthTexture; break;
			case 7: CasterParameters->OpaqueDepthTexture7 = OpaqueDepthTexture; break;
			case 8: CasterParameters->OpaqueDepthTexture8 = OpaqueDepthTexture; break;
			case 9: CasterParameters->OpaqueDepthTexture9 = OpaqueDepthTexture; break;
			case 10: CasterParameters->OpaqueDepthTexture10 = OpaqueDepthTexture; break;
			case 11: CasterParameters->OpaqueDepthTexture11 = OpaqueDepthTexture; break;
			case 12: CasterParameters->OpaqueDepthTexture12 = OpaqueDepthTexture; break;
			case 13: CasterParameters->OpaqueDepthTexture13 = OpaqueDepthTexture; break;
			case 14: CasterParameters->OpaqueDepthTexture14 = OpaqueDepthTexture; break;
			case 15: CasterParameters->OpaqueDepthTexture15 = OpaqueDepthTexture; break;
			case 16: CasterParameters->OpaqueDepthTexture16 = OpaqueDepthTexture; break;
			case 17: CasterParameters->OpaqueDepthTexture17 = OpaqueDepthTexture; break;
			case 18: CasterParameters->OpaqueDepthTexture18 = OpaqueDepthTexture; break;
			case 19: CasterParameters->OpaqueDepthTexture19 = OpaqueDepthTexture; break;
			case 20: CasterParameters->OpaqueDepthTexture20 = OpaqueDepthTexture; break;
			case 21: CasterParameters->OpaqueDepthTexture21 = OpaqueDepthTexture; break;
			case 22: CasterParameters->OpaqueDepthTexture22 = OpaqueDepthTexture; break;
			case 23: CasterParameters->OpaqueDepthTexture23 = OpaqueDepthTexture; break;
			case 24: CasterParameters->OpaqueDepthTexture24 = OpaqueDepthTexture; break;
			case 25: CasterParameters->OpaqueDepthTexture25 = OpaqueDepthTexture; break;
			case 26: CasterParameters->OpaqueDepthTexture26 = OpaqueDepthTexture; break;
			case 27: CasterParameters->OpaqueDepthTexture27 = OpaqueDepthTexture; break;
			case 28: CasterParameters->OpaqueDepthTexture28 = OpaqueDepthTexture; break;
			case 29: CasterParameters->OpaqueDepthTexture29 = OpaqueDepthTexture; break;
			case 30: CasterParameters->OpaqueDepthTexture30 = OpaqueDepthTexture; break;
			case 31: CasterParameters->OpaqueDepthTexture31 = OpaqueDepthTexture; break;
			default: break;
			}
		}

		if (OpaqueDepthTextures.Num() != Entries.Num())
		{
			if (bLog)
			{
				UE_LOG(LogTemp, Warning, TEXT("[ColoredCSM] Skipping caster pass: one or more opaque cascade depth textures could not be registered"));
			}
			return nullptr;
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ColoredCascadeShadowMaps %d cascades", NumCells),
			CasterParameters,
			ERDGPassFlags::Raster,
			[Renderer, Entries, OpaqueDepthTextures, bLog, AtlasSize](FRHICommandList& RHICmdList)
		{
			for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
			{
				const FColoredCascadeShadowEntry& Entry = Entries[EntryIndex];
				const FIntRect& CellRect = Entry.CellRect;
				if (!OpaqueDepthTextures.IsValidIndex(EntryIndex) || OpaqueDepthTextures[EntryIndex] == nullptr)
				{
					continue;
				}

				// Const cast: ShadowsToProject stores non const pointers, the entry only keeps them const for the projection pass.
				const_cast<FProjectedShadowInfo*>(Entry.Shadow)->RenderColoredTranslucencyDepths(
					RHICmdList, Renderer, CellRect, OpaqueDepthTextures[EntryIndex]->GetRHI());
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
			// Match the native CSM projection: the caster depth already includes the native constant/slope bias,
			// and ComputeTransitionSize() supplies the same soft comparison width used by shadow projection.
			PassParameters->DepthParams[NumCascades] = FVector4(0.0f, 1.0f / FMath::Max(Shadow->ComputeTransitionSize(), 1e-6f), 0.0f, 0.0f);

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
