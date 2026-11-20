# 09 — Shared BLAS cache with revision-aware identity

Status: proposed integration diff; not applied to a full UE4.27.2 checkout or compiled.

## Behavior

- Shares local-space BLAS data between ordinary static-mesh instances using the same geometry revision.
- Uses FSoftwareRTGeometryKey, not a bare LOD-resource pointer.
- The key includes LOD identity, position/index CPU-storage addresses, vertex/index counts and a section signature.
- A static-mesh update explicitly invalidates the complete BLAS cache. This prevents stale BLAS data when a resource is rebuilt in place and retains the same descriptor address.
- Failed geometry exports do not create cache entries. They are retried after the next scene/resource invalidation; they do not trigger a busy retry loop.
- Removed resources are pruned after the current primitive set is scanned.

## Diff

\`\`\`diff
diff --git a/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h b/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
--- a/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
+++ b/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
@@
+#include "SoftwareRT/SoftwareRTBVH.h"
@@
 \tstruct FSoftwareRTPrimitiveInstance
 \t{
@@
 \t\tFBoxSphereBounds WorldBounds;
+\t\tFSoftwareRTGeometryKey BLASKey;
+\t\tbool bHasBLAS = false;
 \t};
@@
-\tTMap<const void*, FSoftwareRTBLAS> SoftwareRTBLASCache;
+\tTMap<FSoftwareRTGeometryKey, FSoftwareRTBLAS> SoftwareRTBLASCache;
+\t/** Set when static-mesh resources/elements are rebuilt, even if their addresses are reused. */
+\tbool bSoftwareRTBLASCacheNeedsFullRebuild = false;
diff --git a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
--- a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
@@
 \tif (bSoftwareRTSceneDirty)
 \t{
+\t\tif (bSoftwareRTBLASCacheNeedsFullRebuild)
+\t\t{
+\t\t\tSoftwareRTBLASCache.Reset();
+\t\t\tbSoftwareRTBLASCacheNeedsFullRebuild = false;
+\t\t}
+
 \t\tcheck(Primitives.Num() == PrimitiveSceneProxies.Num());
@@
-\t\tTSet<const void*> ActiveSoftwareRTBLASKeys;
+\t\tTSet<FSoftwareRTGeometryKey> ActiveSoftwareRTBLASKeys;
 \t\tSoftwareRTPrimitiveInstances.Reset(Primitives.Num());
 \t\tfor (int32 PrimitiveIndex = 0; PrimitiveIndex < Primitives.Num(); ++PrimitiveIndex)
 \t\t{
 \t\t\tFPrimitiveSceneProxy* Proxy = PrimitiveSceneProxies[PrimitiveIndex];
-\t\t\tconst void* BLASKey = nullptr;
-\t\t\tconst FStaticMeshLODResources* LODResources = nullptr;
+\t\t\tFSoftwareRTGeometryKey BLASKey;
 
-\t\t\tif (Proxy && Proxy->GetSoftwareRTStaticMeshLOD(0, LODResources) && LODResources)
+\t\t\tif (Proxy && Proxy->GetSoftwareRTGeometryKey(0, BLASKey) && BLASKey.IsValid())
 \t\t\t{
-\t\t\t\tBLASKey = LODResources;
 \t\t\t\tActiveSoftwareRTBLASKeys.Add(BLASKey);
 
 \t\t\t\tif (!SoftwareRTBLASCache.Contains(BLASKey))
@@
 \t\t\t\t\tif (Proxy->GetSoftwareRTStaticMeshGeometry(0, Geometry) &&
 \t\t\t\t\t\tFSoftwareRTBVHBuilder::BuildStaticMeshBLAS(Geometry, NewBLAS))
 \t\t\t\t\t{
 \t\t\t\t\t\tSoftwareRTBLASCache.Add(BLASKey, MoveTemp(NewBLAS));
 \t\t\t\t\t}
 \t\t\t\t\telse
 \t\t\t\t\t{
-\t\t\t\t\t\tBLASKey = nullptr;
+\t\t\t\t\t\tBLASKey = FSoftwareRTGeometryKey();
 \t\t\t\t\t}
 \t\t\t\t}
 \t\t\t}
 \t\t\telse
 \t\t\t{
-\t\t\t\tBLASKey = nullptr;
+\t\t\t\tBLASKey = FSoftwareRTGeometryKey();
 \t\t\t}
@@
-\t\t\tInstance.BLASKey = BLASKey;
+\t\t\tInstance.BLASKey = BLASKey;
+\t\t\tInstance.bHasBLAS = BLASKey.IsValid() && SoftwareRTBLASCache.Contains(BLASKey);
@@
 \t\tfor (auto It = SoftwareRTBLASCache.CreateIterator(); It; ++It)
@@
 \t\t\t\tIt.RemoveCurrent();
 \t\t\t}
 \t\t}
diff --git a/Engine/Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp b/Engine/Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp
--- a/Engine/Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp
@@
 void FPrimitiveSceneInfo::UpdateStaticMeshes(FRHICommandListImmediate& RHICmdList, FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos, bool bReAddToDrawLists)
 {
 \tQUICK_SCOPE_CYCLE_COUNTER(STAT_FPrimitiveSceneInfo_UpdateStaticMeshes);
+\tScene->bSoftwareRTBLASCacheNeedsFullRebuild = true;
\`\`\`

## Important details

- Full cache invalidation on static-mesh update is deliberately conservative. It may rebuild unrelated BLASes after a mesh update, but avoids stale geometry from in-place resource rebuilds.
- Transform-only updates do not invalidate the BLAS cache; they rebuild only the TLAS.
- This still uses LOD 0 for the initial controlled test. It is not render-exact for views selecting other LODs.
- Geometry-key pointer fields are never dereferenced by the cache. They are identity tokens only.
- The cache is currently built synchronously during scene synchronization. This is correctness-first prototype behavior, not production scheduling. Failed exports remain unsupported until a subsequent scene/resource invalidation triggers another build attempt.
- This patch requires 04, 06, 07 and 08. It is not compile-tested.
