# 11 — Software RT closest-hit traversal (TLAS → BLAS)

Status: proposed Renderer-private implementation. Not applied to a complete engine checkout and not compiled.

## Contract

- A ray is `Origin + T * Direction`; `Direction` is **not required to be normalized**.
- `TMin` and `TMax` use that same parametric T.
- Transform origin and direction by the instance inverse transform, but **do not normalize the local direction**. This preserves T under rotation, translation and non-uniform scale.
- The result is the nearest geometric hit: TLAS instance index, BLAS triangle index, material-slot index, T and barycentric coordinates.
- The hit includes a local-space geometric normal. World-space normal transformation and material/surface-cache lookup are deliberately separate follow-up work.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
new file mode 100644
--- /dev/null
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
@@
+#pragma once
+
+#include "CoreMinimal.h"
+#include "SoftwareRTBVH.h"
+
+struct FSoftwareRTRay
+{
+	FVector Origin = FVector::ZeroVector;
+	FVector Direction = FVector::ForwardVector;
+	float TMin = 0.0f;
+	float TMax = BIG_NUMBER;
+};
+
+struct FSoftwareRTHit
+{
+	bool bHit = false;
+	float T = BIG_NUMBER;
+	uint32 InstanceIndex = MAX_uint32;
+	uint32 TriangleIndex = MAX_uint32;
+	int32 MaterialIndex = INDEX_NONE;
+	// Barycentrics are (1-U-V, U, V), corresponding to triangle indices 0,1,2.
+	FVector Barycentrics = FVector::ZeroVector;
+	FVector LocalGeometricNormal = FVector::ZeroVector;
+};
+
+class FSoftwareRTRayTracer
+{
+public:
+	static bool TraceClosest(
+		const FSoftwareRTRay& Ray,
+		const FSoftwareRTTLAS& TLAS,
+		const TMap<const void*, FSoftwareRTBLAS>& BLASCache,
+		FSoftwareRTHit& OutHit);
+};
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
new file mode 100644
--- /dev/null
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
@@
+#include "SoftwareRTRayTracer.h"
+
+namespace
+{
+	static bool IntersectBounds(
+		const FBox& Bounds,
+		const FVector& Origin,
+		const FVector& Direction,
+		float TMin,
+		float TMax)
+	{
+		if (!Bounds.IsValid)
+		{
+			return false;
+		}
+
+		for (int32 Axis = 0; Axis < 3; ++Axis)
+		{
+			const float O = Axis == 0 ? Origin.X : (Axis == 1 ? Origin.Y : Origin.Z);
+			const float D = Axis == 0 ? Direction.X : (Axis == 1 ? Direction.Y : Direction.Z);
+			const float BMin = Axis == 0 ? Bounds.Min.X : (Axis == 1 ? Bounds.Min.Y : Bounds.Min.Z);
+			const float BMax = Axis == 0 ? Bounds.Max.X : (Axis == 1 ? Bounds.Max.Y : Bounds.Max.Z);
+
+			// Parallel to this slab: avoid division by zero and NaNs.
+			if (FMath::Abs(D) <= SMALL_NUMBER)
+			{
+				if (O < BMin || O > BMax)
+				{
+					return false;
+				}
+				continue;
+			}
+
+			const float InvD = 1.0f / D;
+			float T0 = (BMin - O) * InvD;
+			float T1 = (BMax - O) * InvD;
+			if (T0 > T1)
+			{
+				Swap(T0, T1);
+			}
+			TMin = FMath::Max(TMin, T0);
+			TMax = FMath::Min(TMax, T1);
+			if (TMax < TMin)
+			{
+				return false;
+			}
+		}
+		return true;
+	}
+
+	static bool IntersectTriangle(
+		const FSoftwareRTBLAS& BLAS,
+		const FSoftwareRTTriangle& Triangle,
+		const FVector& Origin,
+		const FVector& Direction,
+		float TMin,
+		float TMax,
+		float& OutT,
+		float& OutU,
+		float& OutV,
+		FVector& OutNormal)
+	{
+		const FVector& P0 = BLAS.Positions[(int32)Triangle.Index0];
+		const FVector& P1 = BLAS.Positions[(int32)Triangle.Index1];
+		const FVector& P2 = BLAS.Positions[(int32)Triangle.Index2];
+		const FVector Edge1 = P1 - P0;
+		const FVector Edge2 = P2 - P0;
+		const FVector P = FVector::CrossProduct(Direction, Edge2);
+		const float Det = FVector::DotProduct(Edge1, P);
+
+		// Double-sided intersection. A culling policy can be added per ray type later.
+		if (FMath::Abs(Det) <= SMALL_NUMBER)
+		{
+			return false;
+		}
+
+		const float InvDet = 1.0f / Det;
+		const FVector T = Origin - P0;
+		const float U = FVector::DotProduct(T, P) * InvDet;
+		if (U < 0.0f || U > 1.0f)
+		{
+			return false;
+		}
+
+		const FVector Q = FVector::CrossProduct(T, Edge1);
+		const float V = FVector::DotProduct(Direction, Q) * InvDet;
+		if (V < 0.0f || U + V > 1.0f)
+		{
+			return false;
+		}
+
+		const float HitT = FVector::DotProduct(Edge2, Q) * InvDet;
+		if (HitT < TMin || HitT > TMax)
+		{
+			return false;
+		}
+
+		OutT = HitT;
+		OutU = U;
+		OutV = V;
+		OutNormal = FVector::CrossProduct(Edge1, Edge2).GetSafeNormal();
+		return !OutNormal.IsNearlyZero();
+	}
+
+	static bool TraceBLASClosest(
+		const FSoftwareRTRay& LocalRay,
+		const FSoftwareRTBLAS& BLAS,
+		float& InOutClosestT,
+		uint32& OutTriangleIndex,
+		int32& OutMaterialIndex,
+		FVector& OutBarycentrics,
+		FVector& OutLocalNormal)
+	{
+		if (!BLAS.IsValid())
+		{
+			return false;
+		}
+
+		TArray<int32, TInlineAllocator<64>> Stack;
+		Stack.Add(0);
+		bool bFound = false;
+
+		while (Stack.Num() > 0)
+		{
+			const int32 NodeIndex = Stack.Pop(false);
+			if (!BLAS.Nodes.IsValidIndex(NodeIndex))
+			{
+				continue;
+			}
+			const FSoftwareRTBVHNode& Node = BLAS.Nodes[NodeIndex];
+			if (!IntersectBounds(Node.Bounds, LocalRay.Origin, LocalRay.Direction, LocalRay.TMin, InOutClosestT))
+			{
+				continue;
+			}
+
+			if (Node.NumTriangles > 0)
+			{
+				const uint64 End = (uint64)Node.FirstTriangle + Node.NumTriangles;
+				if (End > (uint64)BLAS.Triangles.Num())
+				{
+					continue;
+				}
+				for (uint32 I = Node.FirstTriangle; I < End; ++I)
+				{
+					float HitT, U, V;
+					FVector Normal;
+					if (IntersectTriangle(BLAS, BLAS.Triangles[(int32)I], LocalRay.Origin, LocalRay.Direction,
+						LocalRay.TMin, InOutClosestT, HitT, U, V, Normal))
+					{
+						bFound = true;
+						InOutClosestT = HitT;
+						OutTriangleIndex = I;
+						OutMaterialIndex = BLAS.Triangles[(int32)I].MaterialIndex;
+						OutBarycentrics = FVector(1.0f - U - V, U, V);
+						OutLocalNormal = Normal;
+					}
+				}
+			}
+			else
+			{
+				if (Node.LeftChild != INDEX_NONE) Stack.Add(Node.LeftChild);
+				if (Node.RightChild != INDEX_NONE) Stack.Add(Node.RightChild);
+			}
+		}
+		return bFound;
+	}
+}
+
+bool FSoftwareRTRayTracer::TraceClosest(
+	const FSoftwareRTRay& Ray,
+	const FSoftwareRTTLAS& TLAS,
+	const TMap<const void*, FSoftwareRTBLAS>& BLASCache,
+	FSoftwareRTHit& OutHit)
+{
+	OutHit = FSoftwareRTHit();
+	if (!TLAS.IsValid() || Ray.TMin < 0.0f || Ray.TMax < Ray.TMin ||
+		Ray.Direction.IsNearlyZero() || Ray.Origin.ContainsNaN() || Ray.Direction.ContainsNaN())
+	{
+		return false;
+	}
+
+	TArray<int32, TInlineAllocator<64>> Stack;
+	Stack.Add(0);
+	float ClosestT = Ray.TMax;
+
+	while (Stack.Num() > 0)
+	{
+		const int32 NodeIndex = Stack.Pop(false);
+		if (!TLAS.Nodes.IsValidIndex(NodeIndex))
+		{
+			continue;
+		}
+		const FSoftwareRTTLASNode& Node = TLAS.Nodes[NodeIndex];
+		if (!IntersectBounds(Node.Bounds, Ray.Origin, Ray.Direction, Ray.TMin, ClosestT))
+		{
+			continue;
+		}
+
+		if (Node.NumInstances > 0)
+		{
+			const uint64 End = (uint64)Node.FirstInstance + Node.NumInstances;
+			if (End > (uint64)TLAS.Instances.Num())
+			{
+				continue;
+			}
+			for (uint32 I = Node.FirstInstance; I < End; ++I)
+			{
+				const FSoftwareRTTLASInstance& Instance = TLAS.Instances[(int32)I];
+				const FSoftwareRTBLAS* BLAS = BLASCache.Find(Instance.BLASKey);
+				if (!BLAS || !BLAS->IsValid())
+				{
+					continue;
+				}
+
+				// Invert once per candidate instance. Reject singular/near-singular transforms.
+				if (FMath::Abs(Instance.LocalToWorld.Determinant()) <= SMALL_NUMBER)
+				{
+					continue;
+				}
+				const FMatrix WorldToLocal = Instance.LocalToWorld.Inverse();
+				FSoftwareRTRay LocalRay;
+				LocalRay.Origin = WorldToLocal.TransformPosition(Ray.Origin);
+				LocalRay.Direction = WorldToLocal.TransformVector(Ray.Direction);
+				LocalRay.TMin = Ray.TMin;
+				LocalRay.TMax = ClosestT;
+
+				uint32 TriangleIndex = MAX_uint32;
+				int32 MaterialIndex = INDEX_NONE;
+				FVector Barycentrics, LocalNormal;
+				if (TraceBLASClosest(LocalRay, *BLAS, ClosestT, TriangleIndex, MaterialIndex, Barycentrics, LocalNormal))
+				{
+					OutHit.bHit = true;
+					OutHit.T = ClosestT;
+					OutHit.InstanceIndex = I;
+					OutHit.TriangleIndex = TriangleIndex;
+					OutHit.MaterialIndex = MaterialIndex;
+					OutHit.Barycentrics = Barycentrics;
+					OutHit.LocalGeometricNormal = LocalNormal;
+				}
+			}
+		}
+		else
+		{
+			if (Node.LeftChild != INDEX_NONE) Stack.Add(Node.LeftChild);
+			if (Node.RightChild != INDEX_NONE) Stack.Add(Node.RightChild);
+		}
+	}
+	return OutHit.bHit;
+}
```

## Important correctness notes

1. **Do not normalize the local ray direction.** With non-uniform scale, normalizing it would change the parameterization and corrupt the comparison between hits from different instances. The unchanged T is valid because an affine inverse maps `O + T D` to `O' + T D'`.
2. The current AABB test handles near-zero direction components explicitly, avoiding division-by-zero and NaN propagation.
3. Triangle tests are double-sided. Shadow rays may later request backface culling; GI/reflection rays may need a different policy. Do not bake one global policy into the BVH.
4. `LocalGeometricNormal` is not yet a world normal. The correct world normal uses the inverse-transpose of the instance transform's linear 3×3 part, followed by normalization. Negative determinant/mirrored instances also need a deliberate winding convention.
5. This first version is correctness-oriented: it traverses both children without near-first ordering and inverts each candidate instance matrix. Later optimize by ordering child visits by entry distance and caching inverse transforms in TLAS instances.
6. It returns geometric hits only. Opacity-mask rejection, per-instance material overrides, WPO, translucency, material Surface Cache lookup, ray differentials, and effect-specific acceptance are not implemented here.
7. Validate `TArray::Pop(EAllowShrinking::No)` against the exact UE4.27.2 branch; if that overload is absent, use `Pop(false)`. Also validate `FBox::IsValid`, `FMatrix::Determinant/Inverse/TransformPosition/TransformVector`, and the existing TLAS/BLAS node definitions in the target checkout.
8. This is not a claim of compilation or working engine integration. Unit-test ray/AABB parallel cases, rays starting inside bounds, edge/vertex triangle hits, non-uniform and negative scale, singular transforms, overlapping instances, and nearest-hit ordering before exposing it to renderer effects.

## Dependencies

Requires patches 06–10. The code assumes patch 08's flat BLAS nodes and patch 10's TLAS node/instance definitions. Add the new `.cpp` to the Renderer module's normal source discovery; no Build.cs module dependency is expected.
