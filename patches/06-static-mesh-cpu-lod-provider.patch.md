# 06 — First Static Mesh CPU-LOD provider (proposed unified diff)

This patch adds a narrow, opt-in source interface for conventional static-mesh LOD resources. It does not extract triangles, build a BVH, upload geometry, or implement ray traversal.

## Intended behavior

- Base `FPrimitiveSceneProxy` reports unsupported by default.
- `FStaticMeshSceneProxy` exposes a borrowed pointer to an LOD only when that LOD is resident and its position/index CPU data are still available.
- The pointer is render-thread/lifetime scoped. Consumers must copy or upload before retaining data.
- Spline Mesh and Instanced Static Mesh explicitly report unsupported so they cannot accidentally inherit the conventional static-mesh path.
- The index-buffer helper makes CPU-data availability testable without calling `GetArrayView()` on discarded storage.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h b/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
--- a/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
+++ b/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
@@
 class FPrimitiveSceneInfo;
 class FStaticPrimitiveDrawInterface;
+struct FStaticMeshLODResources;
 class UPrimitiveComponent;
@@
 	/** Gathers a description of the mesh elements to be rendered for the given LOD index, without consideration for views. */
 	virtual void GetMeshDescription(int32 LODIndex, TArray<FMeshBatch>& OutMeshElements) const {}
+
+	/**
+	 * Returns CPU-readable static-mesh LOD resources for software ray tracing.
+	 * The returned pointer is borrowed: it is valid only while this proxy and its
+	 * current render resources remain alive on the rendering thread. Callers must
+	 * copy/upload the data before retaining it beyond that lifetime.
+	 *
+	 * The base implementation deliberately reports unsupported. Specialized
+	 * proxies must opt in only when their rendered geometry is represented by
+	 * these unmodified static-mesh buffers.
+	 */
+	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const
+	{
+		OutLODResources = nullptr;
+		return false;
+	}
 
 	/** Gathers shadow shapes from this proxy. */
 	virtual void GetShadowShapes(TArray<FCapsuleShape>& CapsuleShapes) const {}
diff --git a/Engine/Source/Runtime/Engine/Public/RawIndexBuffer.h b/Engine/Source/Runtime/Engine/Public/RawIndexBuffer.h
--- a/Engine/Source/Runtime/Engine/Public/RawIndexBuffer.h
+++ b/Engine/Source/Runtime/Engine/Public/RawIndexBuffer.h
@@
 	/** == GetNumIndices() * (b32Bit ? 4 : 2) */
 	int32 GetIndexDataSize() const { return IndexStorage.Num(); }
+
+	/** True when the index buffer still has CPU-readable backing storage. */
+	FORCEINLINE bool HasCPUData() const
+	{
+		return GetNumIndices() > 0 && IndexStorage.GetData() != nullptr;
+	}
 
 	/** Create an RHI index buffer with CPU data. CPU data may be discarded after creation (see TResourceArray::Discard) */
 	FIndexBufferRHIRef CreateRHIBuffer_RenderThread();
diff --git a/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h b/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h
--- a/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h
+++ b/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h
@@
 	virtual void GetMeshDescription(int32 LODIndex, TArray<FMeshBatch>& OutMeshElements) const override;
+
+	/** FPrimitiveSceneProxy interface: expose only resident, CPU-readable static-mesh LOD data. */
+	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const override;
 
 	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
diff --git a/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp b/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
--- a/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
+++ b/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
@@
 void FStaticMeshSceneProxy::GetMeshDescription(int32 LODIndex, TArray<FMeshBatch>& OutMeshElements) const
 {
 	const FStaticMeshLODResources& LODModel = RenderData->LODResources[LODIndex];
@@
 	}
 }
+
+bool FStaticMeshSceneProxy::GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const
+{
+	OutLODResources = nullptr;
+
+	// Software RT currently consumes the LOD selected/resident on the render thread.
+	// Do not access streamed-out LOD CPU buffers or silently substitute another LOD.
+	if (!RenderData || LODIndex < GetCurrentFirstLODIdx_Internal() || LODIndex >= RenderData->LODResources.Num())
+	{
+		return false;
+	}
+
+	const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
+	if (!LOD.VertexBuffers.PositionVertexBuffer.GetVertexData() || !LOD.IndexBuffer.HasCPUData())
+	{
+		return false;
+	}
+
+	OutLODResources = &LOD;
+	return true;
+}
 
 void FStaticMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
 {
diff --git a/Engine/Source/Runtime/Engine/Private/SplineMeshSceneProxy.h b/Engine/Source/Runtime/Engine/Private/SplineMeshSceneProxy.h
--- a/Engine/Source/Runtime/Engine/Private/SplineMeshSceneProxy.h
+++ b/Engine/Source/Runtime/Engine/Private/SplineMeshSceneProxy.h
@@
 	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
 	{
 		return FStaticMeshSceneProxy::GetViewRelevance(View);
 	}
+
+	// Spline deformation is performed by FSplineMeshVertexFactory; the source LOD
+	// buffers are not the geometry rendered by this proxy.
+	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const override
+	{
+		OutLODResources = nullptr;
+		return false;
+	}
 
 	// 	  virtual uint32 GetMemoryFootprint( void ) const { return 0; }
diff --git a/Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h b/Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h
--- a/Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h
+++ b/Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h
@@
 	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
+
+	// This proxy's batches are backed by a per-instance transform buffer. A single
+	// static-mesh LOD pointer does not describe the full rendered instance set.
+	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const override
+	{
+		OutLODResources = nullptr;
+		return false;
+	}
 
 	virtual int32 GetNumMeshBatches() const override
 	{

```

## Important limitations

1. This exposes the raw LOD resource descriptor, not a canonical triangle array.
2. It does not yet export UVs/tangents/material sections or instance transforms.
3. It assumes callers select a resident LOD index; the API rejects streamed-out LODs.
4. It does not yet cover Landscape, foliage-specific proxies, procedural meshes, WPO, Nanite, or custom vertex factories.
5. The diff is grounded against the UE 4.27 mirror commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`, but it has not been applied to the user's full engine tree or compiled. Validate hunk context before applying.
