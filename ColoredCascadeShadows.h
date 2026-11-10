// Colored translucent cascaded shadow maps (custom addition for UE 4.27.2).
//
// Overview
//  RenderColoredCascadeShadowTint() is called from FDeferredShadingSceneRenderer::RenderLights() (render graph BUILD time) for every
//  shadowed light, right before the light is applied. For lights with UDirectionalLightComponent::bCastColoredTranslucentCascadeShadows it adds:
//   1. a raster pass that renders the translucent casters of all active directional CSM cascades into ONE small atlas
//      (RGB = colored transmittance, auxiliary depth = farthest translucent caster). Its opaque CSM depth inputs are imported into RDG and declared as shader-resource reads.
//   2. a full screen pass that reads the atlas and writes a screen space RGB tint (white = no tint) for opaque receivers.
//  DeferredLightPixelShaders.usf then multiplies the light radiance by that tint (USE_COLORED_SHADOW_TINT permutation).
//
// When the toggle is off nothing in here runs, no target is allocated and the light pass uses the original shader permutation.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"

class FSceneRenderer;
class FLightSceneInfo;
class FRHICommandListImmediate;
class FRDGBuilder;
class FSceneTextureUniformParameters;

namespace ColoredCascadeShadows
{
	/** Hard limits (shader arrays are sized with these). */
	constexpr int32 MaxCascades = 5; // Four regular splits plus one optional far cascade
	constexpr int32 AtlasColumns = 4;
	constexpr int32 MaxCells = 20; // 5 cascades x 4 split-screen views

	/** Global kill switch (r.ColoredCascadeShadows.Enable). The per light toggle is still required. */
	bool IsGloballyEnabled();
	/** r.ColoredCascadeShadows.Debug (0 = off). */
	int32 GetDebugLevel();
	/** True roughly every 2 seconds when Debug > 0, to throttle logging. */
	bool ShouldLogThisCall(int32 Site = 0);
}

/**
 * Builds the screen space RGB tint for one light. Returns nullptr when the light has no colored cascades this frame,
 * in which case the caller must keep using the regular light permutation.
 */
FRDGTextureRef RenderColoredCascadeShadowTint(
	FRDGBuilder& GraphBuilder,
	FSceneRenderer& SceneRenderer,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer,
	FIntPoint SceneTextureExtent,
	const FLightSceneInfo& LightSceneInfo);
