# 10 — Software RT TLAS over shared BLAS instances

Status: proposed Renderer-private implementation; not applied to a full UE4.27.2 checkout or compiled.

## What it adds

- A TLAS instance references a typed FSoftwareRTGeometryKey, local-to-world transform and world-space bounds.
- Only snapshot entries with a successfully built BLAS are inserted.
- TLAS is rebuilt after scene synchronization and BLAS pruning.
- Transform-only changes rebuild TLAS but preserve local-space BLAS data.

## Diff

\`\`\`diff
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
@@
 struct FSoftwareRTBLAS
 {
@@
 };
 
+/** One world instance referencing shared local-space geometry. */
+struct FSoftwareRTTLASInstance
+{
+\tFSoftwareRTGeometryKey BLASKey;
+\tFMatrix LocalToWorld = FMatrix::Identity;
+\tFBox WorldBounds = FBox(ForceInit);
+};
+
+struct FSoftwareRTTLASNode
+{
+\tFBox Bounds = FBox(ForceInit);
+\tint32 LeftChild = INDEX_NONE;
+\tint32 RightChild = INDEX_NONE;
+\tuint32 FirstInstance = 0;
+\tuint32 NumInstances = 0;
+};
+
+struct FSoftwareRTTLAS
+{
+\tTArray<FSoftwareRTTLASInstance> Instances;
+\tTArray<FSoftwareRTTLASNode> Nodes;
+\tvoid Reset() { Instances.Reset(); Nodes.Reset(); }
+\tbool IsValid() const { return Instances.Num() > 0 && Nodes.Num() > 0; }
+};
+
 class FSoftwareRTBVHBuilder
 {
 public:
 \tstatic bool BuildStaticMeshBLAS(const FSoftwareRTStaticMeshGeometry& Geometry, FSoftwareRTBLAS& OutBLAS, uint32 MaxLeafTriangles = 4);
+\tstatic bool BuildTLAS(const TArray<FSoftwareRTTLASInstance>& SceneInstances, FSoftwareRTTLAS& OutTLAS, uint32 MaxLeafInstances = 2);
 };
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
--- a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
@@
 namespace
 {
@@
 \tstatic int32 BuildNode(FSoftwareRTBLAS& B, uint32 First, uint32 Count, uint32 LeafLimit, uint32 Depth)
@@
 \t}
+
+\tstatic int32 BuildTLASNode(FSoftwareRTTLAS& T, uint32 First, uint32 Count, uint32 LeafLimit, uint32 Depth)
+\t{
+\t\tconst int32 NodeIndex = T.Nodes.AddDefaulted();
+\t\tFBox Bounds(ForceInit), Centroids(ForceInit);
+\t\tconst uint32 End = First + Count;
+\t\tfor (uint32 I = First; I < End; ++I)
+\t\t{
+\t\t\tBounds += T.Instances[I].WorldBounds;
+\t\t\tCentroids += T.Instances[I].WorldBounds.GetCenter();
+\t\t}
+\t\tT.Nodes[NodeIndex].Bounds = Bounds;
+\t\tif (Count <= LeafLimit || Depth >= 64)
+\t\t{
+\t\t\tT.Nodes[NodeIndex].FirstInstance = First;
+\t\t\tT.Nodes[NodeIndex].NumInstances = Count;
+\t\t\treturn NodeIndex;
+\t\t}
+
+\t\tconst int32 Axis = LongestAxis(Centroids.GetSize());
+\t\tconst float Split = AxisValue(Centroids.GetCenter(), Axis);
+\t\tuint32 Middle = First;
+\t\tfor (uint32 I = First; I < End; ++I)
+\t\t{
+\t\t\tif (AxisValue(T.Instances[I].WorldBounds.GetCenter(), Axis) < Split)
+\t\t\t{
+\t\t\t\tT.Instances.Swap((int32)I, (int32)Middle);
+\t\t\t\t++Middle;
+\t\t\t}
+\t\t}
+\t\tif (Middle == First || Middle == End) Middle = First + Count / 2;
+\t\tconst int32 Left = BuildTLASNode(T, First, Middle - First, LeafLimit, Depth + 1);
+\t\tconst int32 Right = BuildTLASNode(T, Middle, End - Middle, LeafLimit, Depth + 1);
+\t\tFSoftwareRTTLASNode& Node = T.Nodes[NodeIndex];
+\t\tNode.LeftChild = Left;
+\t\tNode.RightChild = Right;
+\t\treturn NodeIndex;
+\t}
 }
@@
 bool FSoftwareRTBVHBuilder::BuildStaticMeshBLAS(const FSoftwareRTStaticMeshGeometry& G, FSoftwareRTBLAS& Out, uint32 LeafLimit)
@@
 }
+
+bool FSoftwareRTBVHBuilder::BuildTLAS(const TArray<FSoftwareRTTLASInstance>& SceneInstances, FSoftwareRTTLAS& Out, uint32 LeafLimit)
+{
+\tOut.Reset();
+\tif (SceneInstances.Num() == 0 || LeafLimit == 0) return false;
+\tOut.Instances.Reserve(SceneInstances.Num());
+\tfor (const FSoftwareRTTLASInstance& Source : SceneInstances)
+\t{
+\t\tif (Source.BLASKey.IsValid() && Source.WorldBounds.IsValid)
+\t\t{
+\t\t\tOut.Instances.Add(Source);
+\t\t}
+\t}
+\tif (Out.Instances.Num() == 0)
+\t{
+\t\tOut.Reset();
+\t\treturn false;
+\t}
+\tBuildTLASNode(Out, 0, (uint32)Out.Instances.Num(), LeafLimit, 0);
+\treturn Out.IsValid();
+}
diff --git a/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h b/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
--- a/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
+++ b/Engine/Source/Runtime/Renderer/Private/ScenePrivate.h
@@
 \tTMap<FSoftwareRTGeometryKey, FSoftwareRTBLAS> SoftwareRTBLASCache;
+\tFSoftwareRTTLAS SoftwareRTTLAS;
diff --git a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
--- a/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
+++ b/Engine/Source/Runtime/Renderer/Private/RendererScene.cpp
@@
-\t\t\tif (PrimitiveInstance.BLASKey && SoftwareRTBLASCache.Contains(PrimitiveInstance.BLASKey))
+\t\t\tif (PrimitiveInstance.bHasBLAS && SoftwareRTBLASCache.Contains(PrimitiveInstance.BLASKey))
 \t\t\t{
 \t\t\t\tFSoftwareRTTLASInstance& TLASInstance = TLASInstances.AddDefaulted_GetRef();
 \t\t\t\tTLASInstance.BLASKey = PrimitiveInstance.BLASKey;
 \t\t\t\tTLASInstance.LocalToWorld = PrimitiveInstance.LocalToWorld;
 \t\t\t\tTLASInstance.WorldBounds = PrimitiveInstance.WorldBounds.GetBox();
 \t\t\t}
 \t\t}
 \t\tFSoftwareRTBVHBuilder::BuildTLAS(TLASInstances, SoftwareRTTLAS);
\`\`\`

## Limits

- Full TLAS rebuild on scene sync; no incremental refit.
- Bounds are proxy world bounds, conservative rather than recomputed from BLAS.
- Initial provider uses LOD 0, not the per-view rendered LOD.
- No WPO, spline deformation, Nanite, HISM/ISM, Landscape or per-instance material overrides.
- The TLAS builder must reject invalid/empty bounds and guard integer overflow for large scenes before production use.
- Requires patches 04, 06, 07, 08 and 09. Not compile-tested.
