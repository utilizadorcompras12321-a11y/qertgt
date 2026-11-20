# 07 — Canonical Static Mesh triangle-data export

Status: proposed unified diff; grounded against UE 4.27 source mirror, not applied to a full engine checkout or compiled.

## What this adds

- A CPU-owned `FSoftwareRTStaticMeshGeometry` containing positions, 32-bit indices and per-section index ranges.
- A generic virtual export function on `FPrimitiveSceneProxy`, unsupported by default.
- A conventional `FStaticMeshSceneProxy` implementation that:
  - requires the LOD to be resident and CPU-readable;
  - copies vertex positions;
  - reads 16-bit or 32-bit indices through `FIndexArrayView`;
  - validates every index against the vertex count;
  - exports section ranges, material-slot indices and `bVisibleInRayTracing`;
  - resets output and fails if any validation fails.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h b/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
--- a/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
+++ b/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
@@
 class FPrimitiveSceneInfo;
 class FStaticPrimitiveDrawInterface;
 struct FStaticMeshLODResources;
 class UPrimitiveComponent;
+
+/** CPU-owned section range exported for software ray tracing. */
+struct FSoftwareRTMeshSection
+{
+	uint32 FirstIndex = 0;
+	uint32 NumTriangles = 0;
+	int32 MaterialIndex = INDEX_NONE;
+	bool bVisibleInRayTracing = true;
+};
+
+/**
+ * Canonical CPU copy of a conventional static-mesh LOD.
+ * Indices address Positions directly; section ranges address Indices.
+ */
+struct FSoftwareRTStaticMeshGeometry
+{
+	TArray<FVector> Positions;
+	TArray<uint32> Indices;
+	TArray<FSoftwareRTMeshSection> Sections;
+
+	void Reset()
+	{
+		Positions.Reset();
+		Indices.Reset();
+		Sections.Reset();
+	}
+};
@@
 	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const
 	{
 		OutLODResources = nullptr;
 		return false;
 	}
+
+	/**
+	 * Copies conventional static-mesh geometry into caller-owned arrays.
+	 * Implementations must return false rather than expose incomplete or
+	 * non-CPU-readable geometry. The default implementation is unsupported.
+	 */
+	virtual bool GetSoftwareRTStaticMeshGeometry(int32 LODIndex, FSoftwareRTStaticMeshGeometry& OutGeometry) const
+	{
+		OutGeometry.Reset();
+		return false;
+	}
 
 	/** Gathers shadow shapes from this proxy. */
 	virtual void GetShadowShapes(TArray<FCapsuleShape>& CapsuleShapes) const {}
diff --git a/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h b/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h
--- a/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h
+++ b/Engine/Source/Runtime/Engine/Public/StaticMeshResources.h
@@
 	/** FPrimitiveSceneProxy interface: expose only resident, CPU-readable static-mesh LOD data. */
 	virtual bool GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const override;
+	virtual bool GetSoftwareRTStaticMeshGeometry(int32 LODIndex, FSoftwareRTStaticMeshGeometry& OutGeometry) const override;
 
 	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
diff --git a/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp b/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
--- a/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
+++ b/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
@@
 bool FStaticMeshSceneProxy::GetSoftwareRTStaticMeshLOD(int32 LODIndex, const FStaticMeshLODResources*& OutLODResources) const
 {
@@
 	OutLODResources = &LOD;
 	return true;
 }
+
+bool FStaticMeshSceneProxy::GetSoftwareRTStaticMeshGeometry(int32 LODIndex, FSoftwareRTStaticMeshGeometry& OutGeometry) const
+{
+	OutGeometry.Reset();
+
+	const FStaticMeshLODResources* LOD = nullptr;
+	if (!GetSoftwareRTStaticMeshLOD(LODIndex, LOD) || !LOD)
+	{
+		return false;
+	}
+
+	const FPositionVertexBuffer& PositionBuffer = LOD->VertexBuffers.PositionVertexBuffer;
+	const int32 NumVertices = (int32)PositionBuffer.GetNumVertices();
+	const int32 NumIndices = LOD->IndexBuffer.GetNumIndices();
+
+	if (NumVertices <= 0 || NumIndices <= 0 || (NumIndices % 3) != 0 ||
+		!PositionBuffer.GetVertexData() || !LOD->IndexBuffer.HasCPUData())
+	{
+		return false;
+	}
+
+	const FIndexArrayView IndexView = LOD->IndexBuffer.GetArrayView();
+	if (IndexView.Num() != NumIndices)
+	{
+		return false;
+	}
+
+	OutGeometry.Positions.Reserve(NumVertices);
+	for (int32 VertexIndex = 0; VertexIndex < NumVertices; ++VertexIndex)
+	{
+		OutGeometry.Positions.Add(PositionBuffer.VertexPosition((uint32)VertexIndex));
+	}
+
+	OutGeometry.Indices.Reserve(NumIndices);
+	for (int32 Index = 0; Index < NumIndices; ++Index)
+	{
+		const uint32 VertexIndex = IndexView[Index];
+		if (VertexIndex >= (uint32)NumVertices)
+		{
+			OutGeometry.Reset();
+			return false;
+		}
+		OutGeometry.Indices.Add(VertexIndex);
+	}
+
+	OutGeometry.Sections.Reserve(LOD->Sections.Num());
+	for (const FStaticMeshSection& SourceSection : LOD->Sections)
+	{
+		const uint64 SectionIndexEnd = (uint64)SourceSection.FirstIndex + (uint64)SourceSection.NumTriangles * 3ull;
+		if (SectionIndexEnd > (uint64)NumIndices)
+		{
+			OutGeometry.Reset();
+			return false;
+		}
+
+		FSoftwareRTMeshSection& Section = OutGeometry.Sections.AddDefaulted_GetRef();
+		Section.FirstIndex = SourceSection.FirstIndex;
+		Section.NumTriangles = SourceSection.NumTriangles;
+		Section.MaterialIndex = SourceSection.MaterialIndex;
+		Section.bVisibleInRayTracing = SourceSection.bVisibleInRayTracing;
+	}
+
+	return true;
+}
 
 void FStaticMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
 {

```

## Deliberate scope limits

- No UVs, tangents, vertex colors, material evaluation or transforms yet.
- No support for Spline Mesh, Instanced Static Mesh, Landscape, Nanite, WPO or custom vertex factories.
- The current implementation exports the full LOD index buffer and its section ranges. The next Core stage must iterate sections and respect `bVisibleInRayTracing`; it must not blindly treat all triangles as visible.
- Caller owns the copied arrays. The source proxy/resources are only read during the call on the render thread.
- The previous proposed patch 06 adds `HasCPUData()` to `FRawStaticIndexBuffer` and the narrow LOD accessor used here. This patch depends on that patch being applied first.
- This patch has not been compile-tested. Check exact hunk context and API compatibility in the user's UE4.27.2 tree before applying.

## Mirror source

`AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.
