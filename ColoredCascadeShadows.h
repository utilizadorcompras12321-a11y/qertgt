// Colored translucent cascaded shadow maps (custom addition for UE 4.27.2).
//
// Overview
//  RenderColoredCascadeShadowTint() is called from FDeferredShadingSceneRenderer::RenderLights() (render graph BUILD time) for every
//  shadowed light, right before the light is applied. For lights with UDirectionalLightComponent::bCastColoredTranslucentCascadeShadows it adds:
//   1. one raster pass per existing directional CSM cascade, using a native-resolution RGB target and that cascade's original opaque depth target.
//   2. one screen pass per cascade, accumulated far-to-near using the native fade-plane settings.
//  DeferredLightPixelShaders.usf multiplies the light radiance by the resulting tint (USE_COLORED_SHADOW_TINT permutation).
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
