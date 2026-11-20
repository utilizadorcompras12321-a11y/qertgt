# 06 — Static Mesh CPU-LOD provider and geometry identity

Status: proposed unified diff, grounded against UE4.27 mirror. Not applied to a full engine checkout or compiled.

## Purpose

Expose conventional static-mesh LOD data only when resident and CPU-readable. Provide a cache key that changes when the underlying CPU storage or section layout changes. Resource rebuilds are also explicitly invalidated by patches 04/09.

## Diff

\`\`\`diff
diff --git a/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h b/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
--- a/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
+++ b/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
@@
 class FPrimitiveSceneInfo;
 class FStaticPrimitiveDrawInterface;
+struct FStaticMeshLODResources;
 class UPrimitiveComponent;
+
+/** Identity token for one immutable CPU geometry revision in the Software RT BLAS cache. */
+struct FSoftwareRTGeometryKey
+{
+	const void* LODResource = nullptr;
+	const void* PositionData = nullptr;
+	const void* IndexData = nullptr;
+	uint32 NumVertices = 0;
+	uint32 NumIndices = 0;
+	uint64 SectionSignature = 0;
+
+	bool IsValid() const
+	{
+		return LODResource && PositionData && IndexData && NumVertices > 0 && NumIndices >= 3;
+	}
+	bool operator==(const FSoftwareRTGeometryKey& Other) const
+	{
+		return LODResource == Other.LODResource && PositionData == Other.PositionData &&
+			IndexData == Other.IndexData && NumVertices == Other.NumVertices &&
+			NumIndices == Other.NumIndices && SectionSignature == Other.SectionSignature;
+	}
+};
+
+FORCEINLINE uint32 GetTypeHash(const FSoftwareRTGeometryKey& Key)
+{
+	uint32 Hash = HashCombine(GetTypeHash(Key.LODResource), GetTypeHash(Key.PositionData));
+	Hash = HashCombine(Hash, GetTypeHash(Key.IndexData));
+	Hash = HashCombine(Hash, GetTypeHash(Key.NumVertices));
+	Hash = HashCombine(Hash, GetTypeHash(Key.NumIndices));
+	return HashCombine(Hash, GetTypeHash(Key.SectionSignature));
+}
@@
 	virtual void GetMeshDescription(int32 LODIndex, TArray<FMeshBatch>& OutMeshElements) const {}
+
+	/** Borrowed resident CPU-readable LOD; unsupported by default. */
+	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const
+	{
+		OutLODResources = nullptr;
+		return false;
+	}
+
+	/** Identity of the exact geometry revision exposed by this proxy. */
+	virtual bool GetSoftwareRTGeometryKey(int32 LODIndex, FSoftwareRTGeometryKey& OutKey) const
+	{
+		OutKey = FSoftwareRTGeometryKey();
+		return false;
+	}
diff --git a/Engine/Source/Runtime/Engine/Public/RawIndexBuffer.h b/Engine/Source/Runtime/Engine/Public/RawIndexBuffer.h
--- a/Engine/Source/Runtime/Engine/Public/RawIndexBuffer.h
+++ b/Engine/Source/Runtime/Engine/Public/RawIndexBuffer.h
@@
 	int32 GetIndexDataSize() const { return IndexStorage.Num(); }
+
+	FORCEINLINE bool HasCPUData() const
+	{
+		return GetNumIndices() > 0 && IndexStorage.GetData() != nullptr;
+	}
+
+	/** Identity token only. Callers must not retain or dereference this pointer. */
+	FORCEINLINE const void* GetCPUDataAddress() const
+	{
+		return IndexStorage.GetData();
+	}
diff --git a/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h b/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h
--- a/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h
+++ b/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h
@@
 	virtual void GetMeshDescription(int32 LODIndex, TArray<FMeshBatch>& OutMeshElements) const override;
+	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const override;
+	virtual bool GetSoftwareRTGeometryKey(int32 LODIndex, FSoftwareRTGeometryKey& OutKey) const override;
diff --git a/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp b/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
--- a/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
+++ b/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
@@
 void FStaticMeshSceneProxy::GetMeshDescription(int32 LODIndex, TArray<FMeshBatch>& OutMeshElements) const
 {
@@
 }
+
+bool FStaticMeshSceneProxy::GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const
+{
+	OutLODResources = nullptr;
+	if (!RenderData || LODIndex < GetCurrentFirstLODIdx_Internal() || LODIndex >= RenderData->LODResources.Num())
+	{
+		return false;
+	}
+	const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
+	if (!LOD.VertexBuffers.PositionVertexBuffer.GetVertexData() || !LOD.IndexBuffer.HasCPUData())
+	{
+		return false;
+	}
+	OutLODResources = &LOD;
+	return true;
+}
+
+bool FStaticMeshSceneProxy::GetSoftwareRTGeometryKey(int32 LODIndex, FSoftwareRTGeometryKey& OutKey) const
+{
+	OutKey = FSoftwareRTGeometryKey();
+	const FStaticMeshLODResources* LOD = nullptr;
+	if (!GetSoftwareRTStaticMeshLOD(LODIndex, LOD) || !LOD)
+	{
+		return false;
+	}
+
+	const FPositionVertexBuffer& Positions = LOD->VertexBuffers.PositionVertexBuffer;
+	const FRawStaticIndexBuffer& Indices = LOD->IndexBuffer;
+	uint64 SectionSignature = 1469598103934665603ull;
+	auto Mix = [&SectionSignature](uint64 Value)
+	{
+		SectionSignature ^= Value;
+		SectionSignature *= 1099511628211ull;
+	};
+	for (const FStaticMeshSection& Section : LOD->Sections)
+	{
+		Mix((uint32)Section.FirstIndex);
+		Mix((uint32)Section.NumTriangles);
+		Mix((uint32)Section.MaterialIndex);
+		Mix(Section.bVisibleInRayTracing ? 1u : 0u);
+	}
+
+	OutKey.LODResource = LOD;
+	OutKey.PositionData = Positions.GetVertexData();
+	OutKey.IndexData = Indices.GetCPUDataAddress();
+	OutKey.NumVertices = Positions.GetNumVertices();
+	OutKey.NumIndices = Indices.GetNumIndices();
+	OutKey.SectionSignature = SectionSignature;
+	return OutKey.IsValid();
+}
diff --git a/Engine/Source/Runtime/Engine/Private/SplineMeshSceneProxy.h b/Engine/Source/Runtime/Engine/Private/SplineMeshSceneProxy.h
--- a/Engine/Source/Runtime/Engine/Private/SplineMeshSceneProxy.h
+++ b/Engine/Source/Runtime/Engine/Private/SplineMeshSceneProxy.h
@@
 	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
 	{
 		return FStaticMeshSceneProxy::GetViewRelevance(View);
 	}
+	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const override
+	{
+		OutLODResources = nullptr;
+		return false;
+	}
+	virtual bool GetSoftwareRTGeometryKey(int32 LODIndex, FSoftwareRTGeometryKey& OutKey) const override
+	{
+		OutKey = FSoftwareRTGeometryKey();
+		return false;
+	}
diff --git a/Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h b/Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h
--- a/Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h
+++ b/Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h
@@
 	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
+	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const override
+	{
+		OutLODResources = nullptr;
+		return false;
+	}
+	virtual bool GetSoftwareRTGeometryKey(int32 LODIndex, FSoftwareRTGeometryKey& OutKey) const override
+	{
+		OutKey = FSoftwareRTGeometryKey();
+		return false;
+	}
\`\`\`

## Semantics and limits

- The key combines LOD identity, CPU position/index storage addresses, vertex/index counts and a signature of section ranges, material slots and ray-tracing visibility.
- The addresses are identity tokens only. No borrowed pointer is dereferenced after the provider call.
- Resource rebuilds explicitly clear the BLAS cache through the static-mesh update path added in patch 04/09. This covers normal proxy/resource recreation; arbitrary in-place buffer mutation without a renderer update notification remains unsupported.
- Streamed-out/non-CPU-readable LODs are rejected, not silently replaced.
- Spline and ISM proxies reject this conventional path. Landscape, Nanite, WPO and custom vertex factories remain unsupported.
- Not compile-tested. Verify the exact method names/access and TResourceArray behavior in the target UE4.27.2 tree.
