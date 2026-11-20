# 09 — Shared BLAS cache in the FScene primitive snapshot

Status: proposed integration diff; not applied to a full UE4.27.2 checkout or compiled.

## Behavior

- Extends each Software RT primitive snapshot entry with a BLAS cache key.
- Uses the borrowed `FStaticMeshLODResources*` returned by the provider as the shared geometry identity.
- Builds a BLAS only when that LOD key is absent from the cache.
- Multiple conventional static-mesh instances using the same LOD resource reuse the same BLAS.
- Transform-only scene changes update snapshot transforms but leave cached local-space BLAS data intact.
- Removes cached BLAS entries once no current primitive references their LOD resource.
- Unsupported proxies and failed geometry exports receive a null BLAS key; they are not silently treated as supported.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h b/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
--- a/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
+++ b/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
@@
+#include "SoftwareRT/SoftwareRTBVH.h"
@@
 	struct FSoftwareRTPrimitiveInstance
 	{
 		FPrimitiveSceneProxy* Proxy;
 		FPrimitiveComponentId ComponentId;
 		FMatrix LocalToWorld;
 		FBoxSphereBounds WorldBounds;
+		/** Key into SoftwareRTBLASCache; null when this proxy is unsupported. */
+		const void* BLASKey;
 	};
@@
 	TArray<FSoftwareRTPrimitiveInstance> SoftwareRTPrimitiveInstances;
+	/** Shared local-space BLAS cache keyed by the source LOD resource identity. */
+	TMap<const void*, FSoftwareRTBLAS> SoftwareRTBLASCache;
 	/** Set by render-thread scene mutations; cleared after the Software RT snapshot is rebuilt. */
 	bool bSoftwareRTSceneDirty = false;
diff --git a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
--- a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
@@
 	if (bSoftwareRTSceneDirty)
 	{
 		check(Primitives.Num() == PrimitiveSceneProxies.Num());
 		check(Primitives.Num() == PrimitiveTransforms.Num());
 		check(Primitives.Num() == PrimitiveBounds.Num());
 		check(Primitives.Num() == PrimitiveComponentIds.Num());
 
+		TSet<const void*> ActiveSoftwareRTBLASKeys;
 		SoftwareRTPrimitiveInstances.Reset(Primitives.Num());
 		for (int32 PrimitiveIndex = 0; PrimitiveIndex < Primitives.Num(); ++PrimitiveIndex)
 		{
+			FPrimitiveSceneProxy* Proxy = PrimitiveSceneProxies[PrimitiveIndex];
+			const void* BLASKey = nullptr;
+			const FStaticMeshLODResources* LODResources = nullptr;
+
+			// The LOD-resource address identifies shared geometry across ordinary
+			// static-mesh instances. Transform changes do not invalidate this key.
+			if (Proxy && Proxy->GetSoftwareRTStaticMeshLOD(0, LODResources) && LODResources)
+			{
+				BLASKey = LODResources;
+				ActiveSoftwareRTBLASKeys.Add(BLASKey);
+
+				if (!SoftwareRTBLASCache.Contains(BLASKey))
+				{
+					FSoftwareRTStaticMeshGeometry Geometry;
+					FSoftwareRTBLAS NewBLAS;
+					if (Proxy->GetSoftwareRTStaticMeshGeometry(0, Geometry) &&
+						FSoftwareRTBVHBuilder::BuildStaticMeshBLAS(Geometry, NewBLAS))
+					{
+						SoftwareRTBLASCache.Add(BLASKey, MoveTemp(NewBLAS));
+					}
+					else
+					{
+						// No cache entry is retained for a failed export/build. The
+						// next scene sync may retry after streaming/resource changes.
+						BLASKey = nullptr;
+					}
+				}
+			}
+			else
+			{
+				BLASKey = nullptr;
+			}
+
 			FSoftwareRTPrimitiveInstance& Instance = SoftwareRTPrimitiveInstances.AddDefaulted_GetRef();
-			Instance.Proxy = PrimitiveSceneProxies[PrimitiveIndex];
+			Instance.Proxy = Proxy;
 			Instance.ComponentId = PrimitiveComponentIds[PrimitiveIndex];
 			Instance.LocalToWorld = PrimitiveTransforms[PrimitiveIndex];
 			Instance.WorldBounds = PrimitiveBounds[PrimitiveIndex].BoxSphereBounds;
+			Instance.BLASKey = BLASKey;
 		}
+
+		// Release BLAS data once no current scene primitive references its source
+		// LOD. This also prevents removed meshes from accumulating indefinitely.
+		for (auto It = SoftwareRTBLASCache.CreateIterator(); It; ++It)
+		{
+			if (!ActiveSoftwareRTBLASKeys.Contains(It.Key()))
+			{
+				It.RemoveCurrent();
+			}
+		}
 		bSoftwareRTSceneDirty = false;
 	}

```

## Important caveats / required corrections before applying

1. **LOD selection:** this first integration explicitly requests LOD 0. It must be changed to the same LOD selection policy used by rendering (including per-view LOD and streaming) before claiming render-exact geometry. For the first controlled test, LOD 0 is deliberate and predictable.
2. **Cache identity:** the raw LOD-resource address is a practical shared key for the first static-mesh provider, but a robust production key needs a resource-generation/revision token to guard against in-place resource rebuilds or address reuse.
3. **Scene update cost:** the current snapshot rebuild happens only when the existing dirty flag is set. It scans primitives, but only builds missing BLAS entries. A moving primitive therefore does not rebuild its BLAS.
4. **Failed export retry:** a failed export is retried on the next dirty scene sync. Add explicit failure counters/diagnostics before production.
5. **Build scheduling:** BLAS construction currently runs synchronously in the render-thread scene sync. This is suitable only as an initial integration test; production should budget or asynchronously build large BLASes and publish completed results safely.
6. **Include placement:** add `#include "SoftwareRT/SoftwareRTBVH.h"` with the private Renderer includes. This header depends on the canonical geometry types from the prior Engine proxy patch.
7. **Patch 04 typo:** its proposed declaration `bool bSoftwareRTSceneDirty = false` must end with a semicolon before applying.

## Dependencies

Requires patches 04, 06, 07 and 08. The scene snapshot patch (04) must be corrected for the missing semicolon. This patch does not create a TLAS, perform ray traversal, or update material data.
