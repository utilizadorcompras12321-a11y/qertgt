# 12 — World-space hit reconstruction and primitive identity

Status: proposed follow-up to patch 11. Renderer-private; not applied to a complete UE4.27.2 checkout or compiled.

## Purpose

Patch 11 returns the nearest triangle in instance-local space. That is not yet a usable shared hit for RT Shadows, GI, Reflections, or a future Surface Cache lookup.

This patch completes the geometric hit contract with:

- world-space hit position;
- world-space, winding-correct geometric normal;
- the scene-snapshot primitive index that owns the TLAS instance;
- explicit rejection of non-finite/singular instance transforms and invalid transformed rays.

The primitive index is valid only against the same synchronized `SoftwareRTPrimitiveInstances` snapshot from which the TLAS was built. It is not a persistent component ID and must not be cached across scene rebuilds.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
@@
 struct FSoftwareRTTLASInstance
 {
 	FSoftwareRTGeometryKey BLASKey;
 	FMatrix LocalToWorld = FMatrix::Identity;
 	FBox WorldBounds = FBox(ForceInit);
+	// Index into FScene::SoftwareRTPrimitiveInstances for this TLAS build only.
+	uint32 ScenePrimitiveIndex = MAX_uint32;
 };
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRTRayTracer.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRTRayTracer.h
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRTRayTracer.h
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRTRayTracer.h
@@
 struct FSoftwareRTHit
 {
 	bool bHit = false;
 	float T = BIG_NUMBER;
 	uint32 InstanceIndex = MAX_uint32;
+	uint32 ScenePrimitiveIndex = MAX_uint32;
 	uint32 TriangleIndex = MAX_uint32;
 	int32 MaterialIndex = INDEX_NONE;
 	// Barycentrics are (1-U-V, U, V), corresponding to triangle indices 0,1,2.
 	FVector Barycentrics = FVector::ZeroVector;
 	FVector LocalGeometricNormal = FVector::ZeroVector;
+	FVector WorldPosition = FVector::ZeroVector;
+	FVector WorldGeometricNormal = FVector::ZeroVector;
 };
diff --git a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
--- a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
@@
-		for (const FSoftwareRTPrimitiveInstance& PrimitiveInstance : SoftwareRTPrimitiveInstances)
+		for (int32 PrimitiveIndex = 0; PrimitiveIndex < SoftwareRTPrimitiveInstances.Num(); ++PrimitiveIndex)
 		{
+			const FSoftwareRTPrimitiveInstance& PrimitiveInstance = SoftwareRTPrimitiveInstances[PrimitiveIndex];
 			if (PrimitiveInstance.bHasBLAS && SoftwareRTBLASCache.Contains(PrimitiveInstance.BLASKey))
 			{
 				FSoftwareRTTLASInstance& TLASInstance = TLASInstances.AddDefaulted_GetRef();
 				TLASInstance.BLASKey = PrimitiveInstance.BLASKey;
 				TLASInstance.LocalToWorld = PrimitiveInstance.LocalToWorld;
 				TLASInstance.WorldBounds = PrimitiveInstance.WorldBounds.GetBox();
+				TLASInstance.ScenePrimitiveIndex = (uint32)PrimitiveIndex;
 			}
 		}
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
@@
 				// Invert once per candidate instance. Reject singular/near-singular transforms.
-				if (FMath::Abs(Instance.LocalToWorld.Determinant()) <= SMALL_NUMBER)
+				const float Determinant = Instance.LocalToWorld.Determinant();
+				if (!FMath::IsFinite(Determinant) || FMath::Abs(Determinant) <= SMALL_NUMBER)
 				{
 					continue;
 				}
 				const FMatrix WorldToLocal = Instance.LocalToWorld.Inverse();
 				FSoftwareRTRay LocalRay;
 				LocalRay.Origin = WorldToLocal.TransformPosition(Ray.Origin);
 				LocalRay.Direction = WorldToLocal.TransformVector(Ray.Direction);
 				LocalRay.TMin = Ray.TMin;
 				LocalRay.TMax = ClosestT;
+				if (LocalRay.Origin.ContainsNaN() || LocalRay.Direction.ContainsNaN() || LocalRay.Direction.IsNearlyZero())
+				{
+					continue;
+				}
@@
 					OutHit.bHit = true;
 					OutHit.T = ClosestT;
 					OutHit.InstanceIndex = I;
+					OutHit.ScenePrimitiveIndex = Instance.ScenePrimitiveIndex;
 					OutHit.TriangleIndex = TriangleIndex;
 					OutHit.MaterialIndex = MaterialIndex;
 					OutHit.Barycentrics = Barycentrics;
 					OutHit.LocalGeometricNormal = LocalNormal;
+					OutHit.WorldPosition = Ray.Origin + Ray.Direction * ClosestT;
+					// A geometric normal is the cross product of transformed triangle
+					// edges. Under a mirrored transform its orientation includes sign(det).
+					FVector WorldNormal = WorldToLocal.GetTransposed().TransformVector(LocalNormal);
+					if (Determinant < 0.0f)
+					{
+						WorldNormal *= -1.0f;
+					}
+					OutHit.WorldGeometricNormal = WorldNormal.GetSafeNormal();
 				}
@@
 	}
-	return OutHit.bHit;
+	return OutHit.bHit && !OutHit.WorldPosition.ContainsNaN() &&
+		!OutHit.WorldGeometricNormal.ContainsNaN() && !OutHit.WorldGeometricNormal.IsNearlyZero();
 }
```

## Correctness notes

1. The world hit position uses the original world ray and the unchanged parametric `T`. This remains correct because patch 11 deliberately does not normalize the transformed local direction.
2. The normal uses the inverse-transpose of the instance transform. Multiplying by the determinant sign preserves the orientation of the transformed triangle winding for mirrored instances.
3. `ScenePrimitiveIndex` follows the TLAS instance through the TLAS builder's in-place swaps. Resolve it only against the exact scene snapshot used for this TLAS.
4. The primitive index is a frame/snapshot-local lookup key, not a durable identity. A later Surface Cache must use a stable primitive/geometry identity and revision, not retain this array index across scene changes.
5. This patch does not yet interpolate vertex normals/UVs, apply per-instance material overrides, reject masked hits, handle WPO, or query the Surface Cache.
6. The determinant threshold is a conservative singular-transform rejection. If very small but valid scales must be supported, replace the absolute threshold with a scale-aware conditioning test rather than removing validation.
7. Validate the exact UE4.27.2 matrix API and row/column convention in the target checkout. In particular, test the inverse-transpose normal transform against rotated, non-uniformly scaled, and mirrored triangles.

## Required tests

- Identity, rotation, translation and non-uniform scale preserve hit position and T.
- Negative scale produces the same oriented world geometric normal as explicitly transforming the triangle vertices and taking their cross product.
- Singular and non-finite transforms are skipped without propagating NaNs.
- TLAS partition swaps do not break `ScenePrimitiveIndex` resolution.
- A miss leaves every `FSoftwareRTHit` field at its reset default.
- The returned world normal is finite and normalized within tolerance.

## Dependencies

Requires patches 08–11. Patch 04's scene snapshot must remain synchronized with the TLAS build. This is still a CPU geometric traversal prototype, not a GPU dispatch or a completed material-evaluation system.
