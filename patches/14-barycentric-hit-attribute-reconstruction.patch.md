# 14 — Barycentric hit-attribute reconstruction

Status: proposed unified diff. Depends on patches 07–13. Not applied to a complete UE4.27.2 checkout or compiled.

## Purpose

Patch 13 exports UVs, tangent basis and vertex colors into `FSoftwareRTStaticMeshGeometry`, but Patch 08 currently copies only positions into the BLAS. This patch closes that missing link and reconstructs the attributes for the exact hit triangle using Patch 11's barycentrics.

The result is local-space surface data. It deliberately does not evaluate a material, sample textures, resolve component overrides, or transform the shading frame to world space. Those belong to later stages.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
@@
 struct FSoftwareRTBLAS
 {
 	TArray<FVector> Positions;
+	uint32 NumTexCoords = 0;
+	TArray<FVector2D> UVs;
+	TArray<FVector4> TangentX;
+	TArray<FVector4> TangentZ;
+	TArray<FLinearColor> VertexColors;
+	bool bHasVertexColors = false;
 	TArray<FSoftwareRTTriangle> Triangles;
 	TArray<FSoftwareRTBVHNode> Nodes;
 	void Reset();
 	bool IsValid() const { return Nodes.Num() > 0 && Triangles.Num() > 0 && Positions.Num() > 0; }
 };
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
@@
 void FSoftwareRTBLAS::Reset()
 {
 	Positions.Reset();
+	NumTexCoords = 0;
+	UVs.Reset();
+	TangentX.Reset();
+	TangentZ.Reset();
+	VertexColors.Reset();
+	bHasVertexColors = false;
 	Triangles.Reset();
 	Nodes.Reset();
 }
@@
 	if (G.Positions.Num() == 0 || G.Indices.Num() == 0 || (G.Indices.Num() % 3) != 0 ||
-		G.Sections.Num() == 0 || LeafLimit == 0)
+		G.Sections.Num() == 0 || LeafLimit == 0 || G.NumTexCoords == 0 ||
+		(uint64)G.Positions.Num() * (uint64)G.NumTexCoords > (uint64)MAX_int32 ||
+		G.UVs.Num() != (int32)((uint64)G.Positions.Num() * (uint64)G.NumTexCoords) ||
+		G.TangentX.Num() != G.Positions.Num() || G.TangentZ.Num() != G.Positions.Num() ||
+		G.VertexColors.Num() != G.Positions.Num())
 	{
 		return false;
 	}
 	Out.Positions = G.Positions;
+	Out.NumTexCoords = G.NumTexCoords;
+	Out.UVs = G.UVs;
+	Out.TangentX = G.TangentX;
+	Out.TangentZ = G.TangentZ;
+	Out.VertexColors = G.VertexColors;
+	Out.bHasVertexColors = G.bHasVertexColors;
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
@@
 struct FSoftwareRTHit
 {
@@
 	FVector WorldGeometricNormal = FVector::ZeroVector;
 };
+
+/** Interpolated attributes at one geometric hit. Tangent frame is BLAS-local. */
+struct FSoftwareRTHitSurfaceData
+{
+	TArray<FVector2D> UVs;
+	FVector TangentX = FVector::ZeroVector;
+	FVector TangentZ = FVector::ZeroVector;
+	float TangentBasisSign = 1.0f;
+	FLinearColor VertexColor = FLinearColor::White;
+	bool bHasVertexColors = false;
+	int32 MaterialIndex = INDEX_NONE;
+
+	void Reset()
+	{
+		UVs.Reset();
+		TangentX = FVector::ZeroVector;
+		TangentZ = FVector::ZeroVector;
+		TangentBasisSign = 1.0f;
+		VertexColor = FLinearColor::White;
+		bHasVertexColors = false;
+		MaterialIndex = INDEX_NONE;
+	}
+};
@@
 class FSoftwareRTRayTracer
 {
 public:
@@
 		const TMap<FSoftwareRTGeometryKey, FSoftwareRTBLAS>& BLASCache,
 		FSoftwareRTHit& OutHit);
+
+	static bool ReconstructHitSurface(
+		const FSoftwareRTBLAS& BLAS,
+		const FSoftwareRTHit& Hit,
+		FSoftwareRTHitSurfaceData& OutSurface);
 };
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
@@
 #include "SoftwareRTRayTracer.h"
+
+bool FSoftwareRTRayTracer::ReconstructHitSurface(
+	const FSoftwareRTBLAS& BLAS,
+	const FSoftwareRTHit& Hit,
+	FSoftwareRTHitSurfaceData& OutSurface)
+{
+	OutSurface.Reset();
+	if (!Hit.bHit || !BLAS.IsValid() ||
+		!BLAS.Triangles.IsValidIndex((int32)Hit.TriangleIndex) ||
+		BLAS.NumTexCoords == 0 ||
+		(uint64)BLAS.Positions.Num() * (uint64)BLAS.NumTexCoords > (uint64)MAX_int32 ||
+		BLAS.UVs.Num() != (int32)((uint64)BLAS.Positions.Num() * (uint64)BLAS.NumTexCoords) ||
+		BLAS.TangentX.Num() != BLAS.Positions.Num() ||
+		BLAS.TangentZ.Num() != BLAS.Positions.Num() ||
+		BLAS.VertexColors.Num() != BLAS.Positions.Num())
+	{
+		return false;
+	}
+
+	const FSoftwareRTTriangle& Triangle = BLAS.Triangles[(int32)Hit.TriangleIndex];
+	if (Triangle.Index0 >= (uint32)BLAS.Positions.Num() ||
+		Triangle.Index1 >= (uint32)BLAS.Positions.Num() ||
+		Triangle.Index2 >= (uint32)BLAS.Positions.Num())
+	{
+		return false;
+	}
+
+	const float B0 = Hit.Barycentrics.X;
+	const float B1 = Hit.Barycentrics.Y;
+	const float B2 = Hit.Barycentrics.Z;
+	const float BarycentricSum = B0 + B1 + B2;
+	const float BarycentricTolerance = 1.0e-3f;
+	if (!FMath::IsFinite(B0) || !FMath::IsFinite(B1) || !FMath::IsFinite(B2) ||
+		B0 < -BarycentricTolerance || B1 < -BarycentricTolerance || B2 < -BarycentricTolerance ||
+		FMath::Abs(BarycentricSum - 1.0f) > BarycentricTolerance)
+	{
+		return false;
+	}
+
+	// Clamp only tiny numerical excursions, then renormalize to preserve affine interpolation.
+	const float C0 = FMath::Max(0.0f, B0);
+	const float C1 = FMath::Max(0.0f, B1);
+	const float C2 = FMath::Max(0.0f, B2);
+	const float ClampedSum = C0 + C1 + C2;
+	if (!FMath::IsFinite(ClampedSum) || ClampedSum <= SMALL_NUMBER)
+	{
+		return false;
+	}
+	const float W0 = C0 / ClampedSum;
+	const float W1 = C1 / ClampedSum;
+	const float W2 = C2 / ClampedSum;
+
+	const uint32 Indices[3] = { Triangle.Index0, Triangle.Index1, Triangle.Index2 };
+	const float Weights[3] = { W0, W1, W2 };
+	OutSurface.UVs.SetNumUninitialized((int32)BLAS.NumTexCoords);
+	for (uint32 UVChannel = 0; UVChannel < BLAS.NumTexCoords; ++UVChannel)
+	{
+		const FVector2D& UV0 = BLAS.UVs[(int32)(Indices[0] * BLAS.NumTexCoords + UVChannel)];
+		const FVector2D& UV1 = BLAS.UVs[(int32)(Indices[1] * BLAS.NumTexCoords + UVChannel)];
+		const FVector2D& UV2 = BLAS.UVs[(int32)(Indices[2] * BLAS.NumTexCoords + UVChannel)];
+		OutSurface.UVs[(int32)UVChannel] = UV0 * W0 + UV1 * W1 + UV2 * W2;
+	}
+
+	const FVector4& TX0 = BLAS.TangentX[(int32)Indices[0]];
+	const FVector4& TX1 = BLAS.TangentX[(int32)Indices[1]];
+	const FVector4& TX2 = BLAS.TangentX[(int32)Indices[2]];
+	const FVector4& TZ0 = BLAS.TangentZ[(int32)Indices[0]];
+	const FVector4& TZ1 = BLAS.TangentZ[(int32)Indices[1]];
+	const FVector4& TZ2 = BLAS.TangentZ[(int32)Indices[2]];
+
+	const FVector InterpolatedTangentX =
+		FVector(TX0.X, TX0.Y, TX0.Z) * W0 +
+		FVector(TX1.X, TX1.Y, TX1.Z) * W1 +
+		FVector(TX2.X, TX2.Y, TX2.Z) * W2;
+	const FVector InterpolatedTangentZ =
+		FVector(TZ0.X, TZ0.Y, TZ0.Z) * W0 +
+		FVector(TZ1.X, TZ1.Y, TZ1.Z) * W1 +
+		FVector(TZ2.X, TZ2.Y, TZ2.Z) * W2;
+	const float InterpolatedBasisSign = TZ0.W * W0 + TZ1.W * W1 + TZ2.W * W2;
+
+	if (InterpolatedTangentX.ContainsNaN() || InterpolatedTangentZ.ContainsNaN() ||
+		!FMath::IsFinite(InterpolatedBasisSign) || FMath::Abs(InterpolatedBasisSign) <= SMALL_NUMBER)
+	{
+		OutSurface.Reset();
+		return false;
+	}
+
+	// Gram-Schmidt: keep tangent X orthogonal to the interpolated tangent Z.
+	OutSurface.TangentZ = InterpolatedTangentZ.GetSafeNormal();
+	const FVector OrthogonalTangentX =
+		InterpolatedTangentX - OutSurface.TangentZ * FVector::DotProduct(InterpolatedTangentX, OutSurface.TangentZ);
+	if (OutSurface.TangentZ.IsNearlyZero() || OrthogonalTangentX.IsNearlyZero())
+	{
+		OutSurface.Reset();
+		return false;
+	}
+	OutSurface.TangentX = OrthogonalTangentX.GetSafeNormal();
+	OutSurface.TangentBasisSign = InterpolatedBasisSign < 0.0f ? -1.0f : 1.0f;
+
+	const FLinearColor& Color0 = BLAS.VertexColors[(int32)Indices[0]];
+	const FLinearColor& Color1 = BLAS.VertexColors[(int32)Indices[1]];
+	const FLinearColor& Color2 = BLAS.VertexColors[(int32)Indices[2]];
+	OutSurface.VertexColor = Color0 * W0 + Color1 * W1 + Color2 * W2;
+	OutSurface.bHasVertexColors = BLAS.bHasVertexColors;
+	OutSurface.MaterialIndex = Triangle.MaterialIndex;
+
+	for (const FVector2D& UV : OutSurface.UVs)
+	{
+		if (!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y))
+		{
+			OutSurface.Reset();
+			return false;
+		}
+	}
+	if (!FMath::IsFinite(OutSurface.VertexColor.R) ||
+		!FMath::IsFinite(OutSurface.VertexColor.G) ||
+		!FMath::IsFinite(OutSurface.VertexColor.B) ||
+		!FMath::IsFinite(OutSurface.VertexColor.A))
+	{
+		OutSurface.Reset();
+		return false;
+	}
+	return true;
+}
```

## Important details

1. **Patch 08 must copy attributes into the BLAS.** Patch 13 only extends the exported geometry object; without this patch's BLAS copy, the hit tracer has no access to UV/tangent/color arrays after the source proxy snapshot is gone.
2. `TriangleIndex` addresses the final BLAS triangle array, including the in-place reordering performed by the BVH builder. The three indices on that triangle are the authoritative interpolation order for barycentrics from Patch 11.
3. UVs use the same vertex-major layout as Patch 13: `vertexIndex * NumTexCoords + channel`.
4. Tangent Z's W stores the basis determinant sign in UE4.27. It is interpolated separately and reduced to a sign; it is not a fourth normal component.
5. The tangent frame is orthonormalized in local space. Transforming it to world space is deliberately deferred: tangent X must use the instance's linear transform, tangent Z uses inverse-transpose, and mirrored transforms require a consistent handedness correction.
6. Vertex colors are already stored as `FLinearColor` by Patch 13. No second sRGB conversion is applied here.
7. The section/material identity comes from the hit triangle's `MaterialIndex`, not from a second section lookup.

## Required tests

- Barycentrics at each vertex and at the centroid reproduce exact vertex/average attributes.
- UV interpolation remains correct on duplicated seam vertices and on multiple UV channels.
- Tangent Z W preserves positive and negative basis signs; interpolated tangent X is orthogonal to tangent Z.
- White fallback vertex colors remain white when the source LOD has no color buffer.
- Degenerate/invalid barycentrics and malformed attribute array sizes fail cleanly and reset output.
- The triangle index still resolves correctly after BVH partition swaps.
- Material index equals the hit triangle's material slot for sections with mixed visibility.

## Deliberate limits

No material graph evaluation, texture sampling, opacity-mask rejection, per-component override colors, per-instance material overrides/custom data, WPO, skinning, Nanite, spline meshes, or world-space shading-frame conversion. The returned attributes are the geometric inputs to a later Surface Cache capture/lookup stage, not final material properties.

## Source/API notes

The UE4.27 mirror `AlexMercer-MA/UnrealEngine-4.27` at commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e` confirms that `TStaticMeshVertexTangentDatum::GetTangentZ()` returns `FVector4`, `SetTangents()` stores `GetBasisDeterminantSign(X,Y,Z)` in W, and the static mesh vertex buffer exposes `VertexTangentX/Z()` and `GetVertexUV()`. This patch has not been compiled in the target checkout.
