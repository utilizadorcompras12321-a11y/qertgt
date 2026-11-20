# 15 — World-space shading-frame reconstruction

Status: proposed unified diff. Depends on patches 10–14. Not applied to a complete UE4.27.2 checkout or compiled.

## Purpose

Patch 14 reconstructs UVs, vertex color and a tangent frame in BLAS-local space. Consumers such as RT material evaluation and the Surface Cache need a consistent world-space frame.

This patch adds a separate conversion step. It does not alter traversal, the hit record, or material evaluation.

- Tangent X is transformed as a direction by `LocalToWorld`, then orthogonalized against the world shading normal.
- Tangent Z is transformed by the inverse-transpose matrix, then normalized.
- The geometric normal from Patch 12 is used only to keep the shading normal in the same hemisphere.
- Tangent handedness accounts for mirrored instance transforms and for the optional shading-normal hemisphere flip.
- The world bitangent is reconstructed from the final orthonormal frame.

The instance transform is resolved from `TLAS.Instances[Hit.InstanceIndex]`. Do not store or infer it from the BLAS: BLAS data is shared in local space.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
@@
 struct FSoftwareRTHitSurfaceData
 {
@@
 };
+
+/** Orthonormal world-space tangent frame for one reconstructed surface hit. */
+struct FSoftwareRTWorldShadingFrame
+{
+	FVector TangentX = FVector::ZeroVector;
+	FVector TangentY = FVector::ZeroVector;
+	FVector TangentZ = FVector::ZeroVector;
+	float TangentBasisSign = 1.0f;
+
+	void Reset()
+	{
+		TangentX = FVector::ZeroVector;
+		TangentY = FVector::ZeroVector;
+		TangentZ = FVector::ZeroVector;
+		TangentBasisSign = 1.0f;
+	}
+};
@@
 	static bool ReconstructHitSurface(
 		const FSoftwareRTBLAS& BLAS,
 		const FSoftwareRTHit& Hit,
 		FSoftwareRTHitSurfaceData& OutSurface);
+
+	static bool ReconstructWorldShadingFrame(
+		const FSoftwareRTTLAS& TLAS,
+		const FSoftwareRTHit& Hit,
+		const FSoftwareRTHitSurfaceData& LocalSurface,
+		FSoftwareRTWorldShadingFrame& OutFrame);
 };
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
@@
 bool FSoftwareRTRayTracer::ReconstructHitSurface(
@@
 	return true;
 }
+
+bool FSoftwareRTRayTracer::ReconstructWorldShadingFrame(
+	const FSoftwareRTTLAS& TLAS,
+	const FSoftwareRTHit& Hit,
+	const FSoftwareRTHitSurfaceData& LocalSurface,
+	FSoftwareRTWorldShadingFrame& OutFrame)
+{
+	OutFrame.Reset();
+
+	// InstanceIndex addresses the final TLAS instance array after BVH partitioning.
+	// ScenePrimitiveIndex is checked as a guard against pairing a hit with another snapshot.
+	if (!Hit.bHit ||
+		!TLAS.Instances.IsValidIndex((int32)Hit.InstanceIndex) ||
+		LocalSurface.MaterialIndex == INDEX_NONE ||
+		!FMath::IsFinite(LocalSurface.TangentBasisSign) ||
+		FMath::Abs(LocalSurface.TangentBasisSign) <= SMALL_NUMBER ||
+		Hit.WorldGeometricNormal.ContainsNaN() ||
+		Hit.WorldGeometricNormal.IsNearlyZero())
+	{
+		return false;
+	}
+
+	const FSoftwareRTTLASInstance& Instance = TLAS.Instances[(int32)Hit.InstanceIndex];
+	if (Instance.ScenePrimitiveIndex != Hit.ScenePrimitiveIndex ||
+		Instance.LocalToWorld.ContainsNaN())
+	{
+		return false;
+	}
+
+	const float Determinant = Instance.LocalToWorld.Determinant();
+	if (!FMath::IsFinite(Determinant) || FMath::Abs(Determinant) <= SMALL_NUMBER)
+	{
+		return false;
+	}
+
+	const FMatrix WorldToLocal = Instance.LocalToWorld.Inverse();
+	if (WorldToLocal.ContainsNaN())
+	{
+		return false;
+	}
+
+	// Tangents are directions (not normals): use the forward linear transform.
+	FVector WorldTangentX = Instance.LocalToWorld.TransformVector(LocalSurface.TangentX);
+	// The normal is a covector: use inverse-transpose for non-uniform scale.
+	FVector WorldTangentZ = WorldToLocal.GetTransposed().TransformVector(LocalSurface.TangentZ);
+	if (WorldTangentX.ContainsNaN() || WorldTangentZ.ContainsNaN() ||
+		WorldTangentX.IsNearlyZero() || WorldTangentZ.IsNearlyZero())
+	{
+		return false;
+	}
+
+	WorldTangentZ = WorldTangentZ.GetSafeNormal();
+	const FVector WorldGeometricNormal = Hit.WorldGeometricNormal.GetSafeNormal();
+	if (WorldTangentZ.IsNearlyZero() || WorldGeometricNormal.IsNearlyZero())
+	{
+		return false;
+	}
+
+	// Transforming a tangent frame through a mirrored matrix reverses its handedness.
+	float WorldBasisSign = (LocalSurface.TangentBasisSign < 0.0f ? -1.0f : 1.0f) *
+		(Determinant < 0.0f ? -1.0f : 1.0f);
+
+	// Keep interpolated vertex normals in the geometric hemisphere. If the normal
+	// is flipped, flip handedness too so the reconstructed bitangent stays consistent.
+	if (FVector::DotProduct(WorldTangentZ, WorldGeometricNormal) < 0.0f)
+	{
+		WorldTangentZ *= -1.0f;
+		WorldBasisSign *= -1.0f;
+	}
+
+	// Gram-Schmidt after the forward transform: non-uniform scale destroys
+	// orthogonality between the transformed tangent and inverse-transpose normal.
+	WorldTangentX -= WorldTangentZ * FVector::DotProduct(WorldTangentX, WorldTangentZ);
+	if (WorldTangentX.ContainsNaN() || WorldTangentX.IsNearlyZero())
+	{
+		return false;
+	}
+	WorldTangentX = WorldTangentX.GetSafeNormal();
+
+	FVector WorldTangentY = FVector::CrossProduct(WorldTangentZ, WorldTangentX) * WorldBasisSign;
+	if (WorldTangentY.ContainsNaN() || WorldTangentY.IsNearlyZero())
+	{
+		return false;
+	}
+	WorldTangentY = WorldTangentY.GetSafeNormal();
+
+	// Final finite/orthogonality checks prevent malformed source attributes from
+	// leaking NaNs or a degenerate basis into material evaluation.
+	if (WorldTangentX.ContainsNaN() || WorldTangentY.ContainsNaN() || WorldTangentZ.ContainsNaN() ||
+		FMath::Abs(FVector::DotProduct(WorldTangentX, WorldTangentY)) > 1.0e-3f ||
+		FMath::Abs(FVector::DotProduct(WorldTangentX, WorldTangentZ)) > 1.0e-3f ||
+		FMath::Abs(FVector::DotProduct(WorldTangentY, WorldTangentZ)) > 1.0e-3f)
+	{
+		return false;
+	}
+
+	OutFrame.TangentX = WorldTangentX;
+	OutFrame.TangentY = WorldTangentY;
+	OutFrame.TangentZ = WorldTangentZ;
+	OutFrame.TangentBasisSign = WorldBasisSign;
+	return true;
+}
```

## Transform and handedness rules

1. `LocalToWorld.TransformVector(TangentX)` is correct for a tangent direction. It must not be replaced by inverse-transpose.
2. `WorldToLocal.GetTransposed().TransformVector(TangentZ)` is the inverse-transpose normal transform. UE4.27's `FMatrix::TransformVector` explicitly excludes translation; `GetTransposed()`, `Inverse()`, `Determinant()` and `ContainsNaN()` are declared on `FMatrix`.
3. The mirrored-transform correction multiplies the local basis sign by `sign(det(LocalToWorld))`. If the transformed shading normal must then be flipped to the geometric hemisphere, the basis sign is flipped once more. This preserves the bitangent orientation implied by the corrected frame.
4. `TangentY = cross(TangentZ, TangentX) * TangentBasisSign`. This matches the determinant-sign convention used by UE's tangent basis: the sign describes the orientation of X/Y relative to Z.
5. Do not use `WorldGeometricNormal` as the shading normal. It is a triangle/winding normal; the interpolated tangent-Z is the smoothed vertex shading normal. The geometric normal is only a hemisphere guard here.

## Important integration constraints

- Call `ReconstructHitSurface()` first, then `ReconstructWorldShadingFrame()`, using the same hit, TLAS and scene snapshot.
- Keep UVs, vertex color and material slot in `FSoftwareRTHitSurfaceData`; this patch outputs only the world frame. No duplicate attribute arrays are needed.
- The function checks `ScenePrimitiveIndex` against the TLAS instance to catch stale/mismatched hit data. It is not a persistent primitive identity.
- This is still a static-mesh LOD0 frame. Per-component override colors, per-instance material overrides, WPO, skinning, Nanite, spline meshes and custom vertex factories remain unsupported.
- The determinant threshold remains the same conservative absolute threshold as Patch 12. Very small but valid scales may be rejected; production code should eventually use a scale-aware conditioning test.

## Required tests

- Identity, rotation and translation preserve an orthonormal frame.
- Non-uniform scale produces a tangent orthogonal to the inverse-transpose normal.
- Positive and negative determinant transforms produce the expected bitangent orientation.
- A mirrored transform plus hemisphere correction flips handedness exactly once per correction.
- Opposed interpolated shading/geometric normals are corrected without replacing the shading normal with the geometric normal.
- Zero/NaN tangent inputs, singular transforms, stale instance indices and mismatched primitive indices fail with a reset output.
- Verify `cross(TangentZ, TangentX) * TangentBasisSign` against UE4.27's tangent basis convention and an explicit transformed local bitangent.

## UE4.27 API verification

Checked against `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`, `Engine/Source/Runtime/Core/Public/Math/Matrix.h`:

- `FMatrix::TransformVector(const FVector&)` (direction transform; excludes translation).
- `FMatrix::GetTransposed()`.
- `FMatrix::Inverse()`.
- `FMatrix::Determinant()`.
- `FMatrix::ContainsNaN()`.

The header explicitly recommends inverse-transpose for normals under non-uniform scale. This confirms declarations and intended semantics, not compilation of this patch or its integration hunks in the target UE4.27.2 checkout.
