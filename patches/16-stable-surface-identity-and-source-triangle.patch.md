# 16 — Stable surface identity and source-triangle preservation

Status: proposed unified diff. Depends on patches 04, 08–12 and 14. Not applied to a complete UE4.27.2 checkout or compiled.

## Problem this closes

Two identities in the current pipeline are unsuitable as persistent Surface Cache keys:

- `Hit.ScenePrimitiveIndex` is an index into one synchronized scene snapshot. It changes when the scene's packed arrays change.
- `Hit.TriangleIndex` addresses the BLAS triangle array. Patch 08 reorders that array in-place while building the BVH, so this index is an acceleration-structure address, not the source mesh triangle identity.

This patch preserves the original source triangle ordinal and carries UE's component ID through the TLAS. It then defines a per-instance surface key from:

- `FPrimitiveComponentId` — stable for the component lifetime, including re-registers;
- `FSoftwareRTGeometryKey` — the geometry revision token already used by the BLAS cache;
- source triangle ordinal in the original LOD index buffer;
- material slot index.

This key identifies a geometric/material-slot surface region. It is **not** a material-content revision or a cache atlas coordinate; those require separate tracking and mapping.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
@@
 struct FSoftwareRTTriangle
 {
 	uint32 Index0 = 0, Index1 = 0, Index2 = 0;
+	// Triangle ordinal in the original LOD index buffer; unlike the BLAS array
+	// index, this value is not changed by BVH partitioning.
+	uint32 SourceTriangleIndex = MAX_uint32;
 	int32 MaterialIndex = INDEX_NONE;
@@
 struct FSoftwareRTTLASInstance
 {
 	FSoftwareRTGeometryKey BLASKey;
+	// Stable component identity; ScenePrimitiveIndex remains snapshot-local.
+	FPrimitiveComponentId ComponentId;
 	FMatrix LocalToWorld = FMatrix::Identity;
 diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
@@
 			FSoftwareRTTriangle& T = Out.Triangles.AddDefaulted_GetRef();
 			T.Index0 = A; T.Index1 = C; T.Index2 = D;
+			T.SourceTriangleIndex = (uint32)(I / 3ull);
 			T.MaterialIndex = S.MaterialIndex;
 diff --git a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
--- a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
@@
 				FSoftwareRTTLASInstance& TLASInstance = TLASInstances.AddDefaulted_GetRef();
 				TLASInstance.BLASKey = PrimitiveInstance.BLASKey;
+				TLASInstance.ComponentId = PrimitiveInstance.ComponentId;
 				TLASInstance.LocalToWorld = PrimitiveInstance.LocalToWorld;
 diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.h
@@
 struct FSoftwareRTHit
 {
@@
 	uint32 TriangleIndex = MAX_uint32;
+	// Original LOD triangle ordinal, independent of BLAS BVH reordering.
+	uint32 SourceTriangleIndex = MAX_uint32;
@@
 };
+
+/**
+ * Runtime identity of one component-owned source triangle and material slot.
+ * This is a lookup key, not a persistent serialized asset identifier.
+ */
+struct FSoftwareRTSurfaceKey
+{
+	FPrimitiveComponentId ComponentId;
+	FSoftwareRTGeometryKey GeometryKey;
+	uint32 SourceTriangleIndex = MAX_uint32;
+	int32 MaterialIndex = INDEX_NONE;
+
+	bool IsValid() const
+	{
+		return GeometryKey.IsValid() &&
+			SourceTriangleIndex != MAX_uint32 &&
+			MaterialIndex != INDEX_NONE;
+	}
+
+	bool operator==(const FSoftwareRTSurfaceKey& Other) const
+	{
+		return ComponentId == Other.ComponentId &&
+			GeometryKey == Other.GeometryKey &&
+			SourceTriangleIndex == Other.SourceTriangleIndex &&
+			MaterialIndex == Other.MaterialIndex;
+	}
+};
+
+FORCEINLINE uint32 GetTypeHash(const FSoftwareRTSurfaceKey& Key)
+{
+	uint32 Hash = HashCombine(GetTypeHash(Key.ComponentId), GetTypeHash(Key.GeometryKey));
+	Hash = HashCombine(Hash, GetTypeHash(Key.SourceTriangleIndex));
+	return HashCombine(Hash, GetTypeHash(Key.MaterialIndex));
+}
@@
 	static bool ReconstructWorldShadingFrame(
@@
 		FSoftwareRTWorldShadingFrame& OutFrame);
+
+	static bool BuildSurfaceKey(
+		const FSoftwareRTTLAS& TLAS,
+		const FSoftwareRTHit& Hit,
+		const FSoftwareRTHitSurfaceData& Surface,
+		FSoftwareRTSurfaceKey& OutKey);
 };
 diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTRayTracer.cpp
@@
 				OutTriangleIndex = I;
@@
 				OutMaterialIndex = BLAS.Triangles[(int32)I].MaterialIndex;
+				OutSourceTriangleIndex = BLAS.Triangles[(int32)I].SourceTriangleIndex;
@@
 				uint32 TriangleIndex = MAX_uint32;
+				uint32 SourceTriangleIndex = MAX_uint32;
@@
-				if (TraceBLASClosest(LocalRay, *BLAS, ClosestT, TriangleIndex, MaterialIndex, Barycentrics, LocalNormal))
+				if (TraceBLASClosest(LocalRay, *BLAS, ClosestT, TriangleIndex, SourceTriangleIndex, MaterialIndex, Barycentrics, LocalNormal))
@@
 					OutHit.TriangleIndex = TriangleIndex;
+					OutHit.SourceTriangleIndex = SourceTriangleIndex;
@@
 }
+
+bool FSoftwareRTRayTracer::BuildSurfaceKey(
+	const FSoftwareRTTLAS& TLAS,
+	const FSoftwareRTHit& Hit,
+	const FSoftwareRTHitSurfaceData& Surface,
+	FSoftwareRTSurfaceKey& OutKey)
+{
+	OutKey = FSoftwareRTSurfaceKey();
+	if (!Hit.bHit ||
+		!TLAS.Instances.IsValidIndex((int32)Hit.InstanceIndex) ||
+		Hit.SourceTriangleIndex == MAX_uint32 ||
+		Hit.MaterialIndex == INDEX_NONE ||
+		Surface.MaterialIndex != Hit.MaterialIndex)
+	{
+		return false;
+	}
+
+	const FSoftwareRTTLASInstance& Instance = TLAS.Instances[(int32)Hit.InstanceIndex];
+	if (Instance.ScenePrimitiveIndex != Hit.ScenePrimitiveIndex || !Instance.BLASKey.IsValid())
+	{
+		return false;
+	}
+
+	OutKey.ComponentId = Instance.ComponentId;
+	OutKey.GeometryKey = Instance.BLASKey;
+	OutKey.SourceTriangleIndex = Hit.SourceTriangleIndex;
+	OutKey.MaterialIndex = Hit.MaterialIndex;
+	if (!OutKey.IsValid())
+	{
+		OutKey = FSoftwareRTSurfaceKey();
+		return false;
+	}
+	return true;
+}
```

## Traversal helper signature change

The corresponding Patch 11 helper declaration/definition change is:

```diff
@@
 static bool TraceBLASClosest(
 	const FSoftwareRTRay& LocalRay,
 	const FSoftwareRTBLAS& BLAS,
 	float& InOutClosestT,
 	uint32& OutTriangleIndex,
+	uint32& OutSourceTriangleIndex,
 	int32& OutMaterialIndex,
@@
 					bFound = true;
 					InOutClosestT = HitT;
 					OutTriangleIndex = I;
+					OutSourceTriangleIndex = BLAS.Triangles[(int32)I].SourceTriangleIndex;
@@
 				uint32 TriangleIndex = MAX_uint32;
+				uint32 SourceTriangleIndex = MAX_uint32;
@@
-				if (TraceBLASClosest(LocalRay, *BLAS, ClosestT, TriangleIndex, MaterialIndex, Barycentrics, LocalNormal))
+				if (TraceBLASClosest(LocalRay, *BLAS, ClosestT, TriangleIndex, SourceTriangleIndex, MaterialIndex, Barycentrics, LocalNormal))
@@
 					OutHit.TriangleIndex = TriangleIndex;
+					OutHit.SourceTriangleIndex = SourceTriangleIndex;
```

Initialize the output to `MAX_uint32` at the call site, as shown. The hit's `TriangleIndex` remains the BLAS array index because Patch 14 needs it to access the reordered BLAS triangle and its vertex indices. Do not replace it with `SourceTriangleIndex`.

## Identity and invalidation rules

1. `ComponentId` distinguishes two components sharing one BLAS. It is copied from the same scene snapshot that supplies the TLAS instance.
2. `SourceTriangleIndex` is calculated from the original LOD index-buffer offset before any BVH partitioning. It remains associated with the triangle as the BLAS triangle array is reordered.
3. `GeometryKey` is the existing runtime revision token. Its pointer members are identity tokens only; never dereference them from the surface key.
4. Static-mesh resource updates must continue to invalidate the BLAS cache as Patch 09 specifies. Any Surface Cache entries using the old geometry key must be invalidated at the same time.
5. Component removal must evict that component's per-instance surface entries. Component re-register preserves the ID according to the UE4.27 `FPrimitiveSceneProxy` contract, but cache lifetime still must be bounded and explicit.
6. The key deliberately includes the material slot, not a material-proxy pointer. Material parameter changes can occur without changing the component or geometry key; a later material-capture revision mechanism must invalidate/re-capture affected entries.
7. This key does not solve UV overlap or assign atlas coordinates. The cache still needs a deterministic raster-generated mapping from this identity to one or more capture records.

## UE4.27 source check

Mirror: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

- `FPrimitiveSceneProxy::GetPrimitiveComponentId()` exposes the component ID.
- The `PrimitiveComponentId` member comment states it remains the same for the component lifetime and can identify the component across re-registers.
- `FScene` already stores `PrimitiveComponentIds` parallel to its packed primitive arrays.
- Existing Renderer maps use `FPrimitiveComponentId` as a `TMap` key, confirming UE's hash/equality support is available.
- The TLAS instance receives the ID from Patch 04's `FSoftwareRTPrimitiveInstance.ComponentId`; it does not query UObjects or dereference game-thread component state.

This is a source-grounded proposal, not a compile-tested integration. Verify exact diff context and the `TraceBLASClosest` signature when applying the patch series to the target checkout.
