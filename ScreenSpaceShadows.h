// Copyright Epic Games, Inc. All Rights Reserved.
// Based on Bend Studio's Screen Space Shadows CPU dispatch builder.
// Copyright 2023 Sony Interactive Entertainment.
// Licensed under the Apache License, Version 2.0.
// https://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "CoreMinimal.h"

/**
 * CPU-side dispatch layout for the Bend Studio screen-space shadow algorithm.
 *
 * The bounds passed to BuildDispatchList are inclusive and are expressed in
 * viewport-local pixel coordinates. The returned offsets are in pixels,
 * while WaveCount[0..2] are dispatch group counts (X/Y/Z).
 *
 * The implementation intentionally keeps the original Bend layout algorithm.
 * The shader port must use a 64-thread wave and consume these values unchanged.
 */
namespace Bend
{
	struct DispatchData
	{
		int32 WaveCount[3];
		int32 WaveOffset_Shader[2];
	};

	struct DispatchList
	{
		float LightCoordinate_Shader[4];
		DispatchData Dispatch[8];
		int32 DispatchCount;
	};

	FORCEINLINE int32 bend_min(const int32 A, const int32 B)
	{
		return A > B ? B : A;
	}

	FORCEINLINE int32 bend_max(const int32 A, const int32 B)
	{
		return A > B ? A : B;
	}

	/**
	 * Builds the compute dispatches for one light.
	 *
	 * InLightProjection is the homogeneous clip-space result of transforming
	 * float4(light direction, 0) by the view-projection matrix for a directional
	 * light. InViewportSize is the target viewport size. Min/MaxRenderBounds
	 * are inclusive viewport-local pixel coordinates.
	 */
	FORCEINLINE DispatchList BuildDispatchList(
		const float InLightProjection[4],
		const int32 InViewportSize[2],
		const int32 InMinRenderBounds[2],
		const int32 InMaxRenderBounds[2],
		const bool bInExpandedZRange = false,
		const int32 InWaveSize = 64)
	{
		DispatchList Result = {};

		// The algorithm divides by WaveSize and assumes a positive viewport.
		if (InWaveSize <= 0 || InViewportSize[0] <= 0 || InViewportSize[1] <= 0)
		{
			return Result;
		}

		const float XYLightWOriginal = InLightProjection[3];
		float XYLightW = XYLightWOriginal;
		const float FPLimit = 0.000002f * static_cast<float>(InWaveSize);

		if (XYLightW >= 0.0f && XYLightW < FPLimit)
		{
			XYLightW = FPLimit;
		}
		else if (XYLightW < 0.0f && XYLightW > -FPLimit)
		{
			XYLightW = -FPLimit;
		}

		Result.LightCoordinate_Shader[0] =
			((InLightProjection[0] / XYLightW) * 0.5f + 0.5f) * static_cast<float>(InViewportSize[0]);
		Result.LightCoordinate_Shader[1] =
			((InLightProjection[1] / XYLightW) * -0.5f + 0.5f) * static_cast<float>(InViewportSize[1]);
		Result.LightCoordinate_Shader[2] =
			XYLightWOriginal == 0.0f ? 0.0f : (InLightProjection[2] / XYLightWOriginal);
		Result.LightCoordinate_Shader[3] = XYLightWOriginal > 0.0f ? 1.0f : -1.0f;

		if (bInExpandedZRange)
		{
			Result.LightCoordinate_Shader[2] = Result.LightCoordinate_Shader[2] * 0.5f + 0.5f;
		}

		const int32 LightXY[2] =
		{
			static_cast<int32>(Result.LightCoordinate_Shader[0] + 0.5f),
			static_cast<int32>(Result.LightCoordinate_Shader[1] + 0.5f)
		};

		const int32 BiasedBounds[4] =
		{
			InMinRenderBounds[0] - LightXY[0],
			-(InMaxRenderBounds[1] - LightXY[1]),
			InMaxRenderBounds[0] - LightXY[0],
			-(InMinRenderBounds[1] - LightXY[1])
		};

		for (int32 Q = 0; Q < 4; ++Q)
		{
			const bool bVertical = (Q == 0 || Q == 3);

			const int32 Bounds[4] =
			{
				bend_max(0, ((Q & 1) ? BiasedBounds[0] : -BiasedBounds[2])) / InWaveSize,
				bend_max(0, ((Q & 2) ? BiasedBounds[1] : -BiasedBounds[3])) / InWaveSize,
				bend_max(0, (((Q & 1) ? BiasedBounds[2] : -BiasedBounds[0]) + InWaveSize * (bVertical ? 1 : 2) - 1)) / InWaveSize,
				bend_max(0, (((Q & 2) ? BiasedBounds[3] : -BiasedBounds[1]) + InWaveSize * (bVertical ? 2 : 1) - 1)) / InWaveSize
			};

			if ((Bounds[2] - Bounds[0]) <= 0 || (Bounds[3] - Bounds[1]) <= 0)
			{
				continue;
			}

			const int32 BiasX = (Q == 2 || Q == 3) ? 1 : 0;
			const int32 BiasY = (Q == 1 || Q == 3) ? 1 : 0;

			// At most two entries are emitted per quadrant: one base dispatch
			// and one split dispatch. Four quadrants therefore fit Dispatch[8].
			if (Result.DispatchCount >= UE_ARRAY_COUNT(Result.Dispatch))
			{
				break;
			}

			DispatchData& Disp = Result.Dispatch[Result.DispatchCount++];
			Disp.WaveCount[0] = InWaveSize;
			Disp.WaveCount[1] = Bounds[2] - Bounds[0];
			Disp.WaveCount[2] = Bounds[3] - Bounds[1];
			Disp.WaveOffset_Shader[0] = ((Q & 1) ? Bounds[0] : -Bounds[2]) + BiasX;
			Disp.WaveOffset_Shader[1] = ((Q & 2) ? -Bounds[3] : Bounds[1]) + BiasY;

			int32 AxisDelta = +BiasedBounds[0] - BiasedBounds[1];
			if (Q == 1) AxisDelta = +BiasedBounds[2] + BiasedBounds[1];
			if (Q == 2) AxisDelta = -BiasedBounds[0] - BiasedBounds[3];
			if (Q == 3) AxisDelta = -BiasedBounds[2] + BiasedBounds[3];
			AxisDelta = (AxisDelta + InWaveSize - 1) / InWaveSize;

			if (AxisDelta > 0 && Result.DispatchCount < UE_ARRAY_COUNT(Result.Dispatch))
			{
				DispatchData& Disp2 = Result.Dispatch[Result.DispatchCount++];
				Disp2 = Disp;

				if (Q == 0)
				{
					Disp2.WaveCount[2] = bend_min(Disp.WaveCount[2], AxisDelta);
					Disp.WaveCount[2] -= Disp2.WaveCount[2];
					Disp2.WaveOffset_Shader[1] = Disp.WaveOffset_Shader[1] + Disp.WaveCount[2];
					Disp2.WaveOffset_Shader[0]--;
					Disp2.WaveCount[1]++;
				}
				else if (Q == 1)
				{
					Disp2.WaveCount[1] = bend_min(Disp.WaveCount[1], AxisDelta);
					Disp.WaveCount[1] -= Disp2.WaveCount[1];
					Disp2.WaveOffset_Shader[0] = Disp.WaveOffset_Shader[0] + Disp.WaveCount[1];
					Disp2.WaveCount[2]++;
				}
				else if (Q == 2)
				{
					Disp2.WaveCount[1] = bend_min(Disp.WaveCount[1], AxisDelta);
					Disp.WaveCount[1] -= Disp2.WaveCount[1];
					Disp2.WaveOffset_Shader[0] += Disp2.WaveCount[1];
					Disp2.WaveCount[2]++;
					Disp2.WaveOffset_Shader[1]--;
				}
				else // Q == 3
				{
					Disp2.WaveCount[2] = bend_min(Disp.WaveCount[2], AxisDelta);
					Disp.WaveCount[2] -= Disp2.WaveCount[2];
					Disp2.WaveOffset_Shader[1] += Disp2.WaveCount[2];
					Disp2.WaveCount[1]++;
				}

				if (Disp2.WaveCount[1] <= 0 || Disp2.WaveCount[2] <= 0)
				{
					Disp2 = Result.Dispatch[--Result.DispatchCount];
				}
				if (Disp.WaveCount[1] <= 0 || Disp.WaveCount[2] <= 0)
				{
					Disp = Result.Dispatch[--Result.DispatchCount];
				}
			}
		}

		for (int32 Index = 0; Index < Result.DispatchCount; ++Index)
		{
			Result.Dispatch[Index].WaveOffset_Shader[0] *= InWaveSize;
			Result.Dispatch[Index].WaveOffset_Shader[1] *= InWaveSize;
		}

		return Result;
	}
}
