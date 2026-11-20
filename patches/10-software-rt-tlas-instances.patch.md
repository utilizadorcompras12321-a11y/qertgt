# 10 — Software RT TLAS over shared BLAS instances

Status: proposed integration diff; not applied to a full UE4.27.2 checkout or compiled.

## What it adds

- A TLAS instance record containing the shared BLAS key, local-to-world transform and world-space bounds.
- A flat binary TLAS with leaves containing up to two instances by default.
- Median partitioning on the longest axis of instance-bound centroids.
- Scene synchronization gathers only instances whose BLAS exists in the shared cache, then rebuilds the TLAS after the BLAS cache has been pruned.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
@@
 struct FSoftwareRTBLAS
 {
@@
 };
 
+/** One scene instance referencing shared local-space BLAS geometry. */
+struct FSoftwareRTTLASInstance
+{
+	const void* BLASKey = nullptr;
+	FMatrix LocalToWorld = FMatrix::Identity;
+	FBox WorldBounds = FBox(ForceInit);
+};
+
+/** Top-level BVH over world-space instance bounds. */
+struct FSoftwareRTTLASNode
+{
+	FBox Bounds = FBox(ForceInit);
+	int32 LeftChild = INDEX_NONE;
+	int32 RightChild = INDEX_NONE;
+	uint32 FirstInstance = 0;
+	uint32 NumInstances = 0;
+};
+
+struct FSoftwareRTTLAS
+{
+	TArray<FSoftwareRTTLASInstance> Instances;
+	TArray<FSoftwareRTTLASNode> Nodes;
+	void Reset()
+	{
+		Instances.Reset();
+		Nodes.Reset();
+	}
+	bool IsValid() const { return Instances.Num() > 0 && Nodes.Num() > 0; }
+};
+
 class FSoftwareRTBVHBuilder
 {
 public:
 	static bool BuildStaticMeshBLAS(const FSoftwareRTStaticMeshGeometry& Geometry, FSoftwareRTBLAS& OutBLAS, uint32 MaxLeafTriangles = 4);
+	static bool BuildTLAS(const TArray<FSoftwareRTTLASInstance>& SceneInstances, FSoftwareRTTLAS& OutTLAS, uint32 MaxLeafInstances = 2);
 };
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
@@
 namespace
 {
@@
 	static int32 BuildNode(FSoftwareRTBLAS& B, uint32 First, uint32 Count, uint32 LeafLimit, uint32 Depth)
@@
 	}
+
+	static int32 BuildTLASNode(FSoftwareRTTLAS& T, uint32 First, uint32 Count, uint32 LeafLimit, uint32 Depth)
+	{
+		const int32 NodeIndex = T.Nodes.AddDefaulted();
+		FBox Bounds(ForceInit), Centroids(ForceInit);
+		const uint32 End = First + Count;
+		for (uint32 I = First; I < End; ++I)
+		{
+			Bounds += T.Instances[I].WorldBounds;
+			Centroids += T.Instances[I].WorldBounds.GetCenter();
+		}
+		T.Nodes[NodeIndex].Bounds = Bounds;
+		if (Count <= LeafLimit || Depth >= 64)
+		{
+			T.Nodes[NodeIndex].FirstInstance = First;
+			T.Nodes[NodeIndex].NumInstances = Count;
+			return NodeIndex;
+		}
+
+		const int32 Axis = LongestAxis(Centroids.GetSize());
+		const float Split = AxisValue(Centroids.GetCenter(), Axis);
+		uint32 Middle = First;
+		for (uint32 I = First; I < End; ++I)
+		{
+			if (AxisValue(T.Instances[I].WorldBounds.GetCenter(), Axis) < Split)
+			{
+				T.Instances.Swap((int32)I, (int32)Middle);
+				++Middle;
+			}
+		}
+		if (Middle == First || Middle == End)
+		{
+			Middle = First + Count / 2;
+		}
+		const int32 Left = BuildTLASNode(T, First, Middle - First, LeafLimit, Depth + 1);
+		const int32 Right = BuildTLASNode(T, Middle, End - Middle, LeafLimit, Depth + 1);
+		FSoftwareRTTLASNode& Node = T.Nodes[NodeIndex];
+		Node.LeftChild = Left;
+		Node.RightChild = Right;
+		return NodeIndex;
+	}
 }
@@
 bool FSoftwareRTBVHBuilder::BuildStaticMeshBLAS(const FSoftwareRTStaticMeshGeometry& G, FSoftwareRTBLAS& Out, uint32 LeafLimit)
@@
 }
+
+bool FSoftwareRTBVHBuilder::BuildTLAS(const TArray<FSoftwareRTTLASInstance>& SceneInstances, FSoftwareRTTLAS& Out, uint32 LeafLimit)
+{
+	Out.Reset();
+	if (SceneInstances.Num() == 0 || LeafLimit == 0)
+	{
+		return false;
+	}
+
+	Out.Instances.Reserve(SceneInstances.Num());
+	for (const FSoftwareRTTLASInstance& Source : SceneInstances)
+	{
+		if (!Source.BLASKey || !Source.WorldBounds.IsValid)
+		{
+			continue;
+		}
+		Out.Instances.Add(Source);
+	}
+	if (Out.Instances.Num() == 0)
+	{
+		Out.Reset();
+		return false;
+	}
+
+	BuildTLASNode(Out, 0, (uint32)Out.Instances.Num(), LeafLimit, 0);
+	return Out.IsValid();
+}
diff --git a/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h b/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
--- a/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
+++ b/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
@@
 	TMap<const void*, FSoftwareRTBLAS> SoftwareRTBLASCache;
+	/** World-space hierarchy over current instances referencing SoftwareRTBLASCache. */
+	FSoftwareRTTLAS SoftwareRTTLAS;
diff --git a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
--- a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
@@
 		for (auto It = SoftwareRTBLASCache.CreateIterator(); It; ++It)
 		{
 			if (!ActiveSoftwareRTBLASKeys.Contains(It.Key()))
 			{
 				It.RemoveCurrent();
 			}
 		}
+
+		TArray<FSoftwareRTTLASInstance> TLASInstances;
+		TLASInstances.Reserve(SoftwareRTPrimitiveInstances.Num());
+		for (const FSoftwareRTPrimitiveInstance& PrimitiveInstance : SoftwareRTPrimitiveInstances)
+		{
+			if (PrimitiveInstance.BLASKey && SoftwareRTBLASCache.Contains(PrimitiveInstance.BLASKey))
+			{
+				FSoftwareRTTLASInstance& TLASInstance = TLASInstances.AddDefaulted_GetRef();
+				TLASInstance.BLASKey = PrimitiveInstance.BLASKey;
+				TLASInstance.LocalToWorld = PrimitiveInstance.LocalToWorld;
+				TLASInstance.WorldBounds = PrimitiveInstance.WorldBounds.GetBox();
+			}
+		}
+		FSoftwareRTBVHBuilder::BuildTLAS(TLASInstances, SoftwareRTTLAS);
 		bSoftwareRTSceneDirty = false;
 	}

```

## Important behavior

The BLAS remains local-space and shared. The TLAS stores a transform per instance. A transform update rebuilds the TLAS but does not rebuild the BLAS. This is the intended separation for static mesh instances.

## Limitations

- This is a full TLAS rebuild at scene sync, not an incremental refit. It is acceptable as a correctness-first prototype, but moving-heavy scenes will need refit/rebuild budgeting.
- No ray traversal or ray-space transform is implemented yet. Traversal must transform rays into BLAS-local space and correctly handle non-uniform scale and negative determinant transforms.
- Bounds are taken from the primitive snapshot. They are conservative proxy bounds, not recomputed from the BLAS.
- The same LOD 0 / resource-pointer identity limitations documented in patch 09 apply.
- No handling for multiple LODs per view, per-instance material overrides, HISM-specific instance data, WPO, spline deformation, Nanite or unsupported proxy types.
- `FBox::IsValid`, `FBoxSphereBounds::GetBox`, and all exact patch contexts must be verified against the user's UE4.27.2 tree before application.

## Dependencies

Requires patches 04, 06, 07, 08 and 09. Patch 04's missing semicolon must be fixed. This patch does not claim working ray queries; it only establishes the two-level acceleration-structure data layout and scene build path.
