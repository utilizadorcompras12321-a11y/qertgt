# 08 — Software RT BLAS and median-split BVH builder

Status: proposed Renderer-private implementation; not applied to a full engine checkout or compiled.

## Purpose

Build one local-space BLAS from patch 07's canonical geometry. The BLAS owns positions and triangle records, retains each triangle's material-slot index, and excludes sections marked invisible to ray tracing.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
new file mode 100644
--- /dev/null
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.h
@@
+#pragma once
+#include "CoreMinimal.h"
+#include "PrimitiveSceneProxy.h"
+
+struct FSoftwareRTTriangle
+{
+	uint32 Index0 = 0, Index1 = 0, Index2 = 0;
+	int32 MaterialIndex = INDEX_NONE;
+	FBox Bounds = FBox(ForceInit);
+	FVector Centroid = FVector::ZeroVector;
+};
+
+/** Leaf iff NumTriangles > 0; internal nodes reference two child nodes. */
+struct FSoftwareRTBVHNode
+{
+	FBox Bounds = FBox(ForceInit);
+	int32 LeftChild = INDEX_NONE;
+	int32 RightChild = INDEX_NONE;
+	uint32 FirstTriangle = 0;
+	uint32 NumTriangles = 0;
+};
+
+/** Local-space, CPU-owned BLAS. It retains no proxy or LOD-resource pointers. */
+struct FSoftwareRTBLAS
+{
+	TArray<FVector> Positions;
+	TArray<FSoftwareRTTriangle> Triangles;
+	TArray<FSoftwareRTBVHNode> Nodes;
+	void Reset();
+	bool IsValid() const { return Nodes.Num() > 0 && Triangles.Num() > 0 && Positions.Num() > 0; }
+};
+
+class FSoftwareRTBVHBuilder
+{
+public:
+	static bool BuildStaticMeshBLAS(const FSoftwareRTStaticMeshGeometry& Geometry, FSoftwareRTBLAS& OutBLAS, uint32 MaxLeafTriangles = 4);
+};
diff --git a/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
new file mode 100644
--- /dev/null
+++ b/Engine/Source/Runtime/Renderer/Private/SoftwareRT/SoftwareRTBVH.cpp
@@
+#include "SoftwareRTBVH.h"
+
+namespace
+{
+	static int32 LongestAxis(const FVector& E)
+	{
+		return (E.X >= E.Y && E.X >= E.Z) ? 0 : (E.Y >= E.Z ? 1 : 2);
+	}
+	static float AxisValue(const FVector& V, int32 A)
+	{
+		return A == 0 ? V.X : (A == 1 ? V.Y : V.Z);
+	}
+	static int32 BuildNode(FSoftwareRTBLAS& B, uint32 First, uint32 Count, uint32 LeafLimit, uint32 Depth)
+	{
+		const int32 NodeIndex = B.Nodes.AddDefaulted();
+		FBox Bounds(ForceInit), Centroids(ForceInit);
+		const uint32 End = First + Count;
+		for (uint32 I = First; I < End; ++I)
+		{
+			Bounds += B.Triangles[I].Bounds;
+			Centroids += B.Triangles[I].Centroid;
+		}
+		B.Nodes[NodeIndex].Bounds = Bounds;
+		if (Count <= LeafLimit || Depth >= 64)
+		{
+			B.Nodes[NodeIndex].FirstTriangle = First;
+			B.Nodes[NodeIndex].NumTriangles = Count;
+			return NodeIndex;
+		}
+
+		const int32 Axis = LongestAxis(Centroids.GetSize());
+		const float Split = AxisValue(Centroids.GetCenter(), Axis);
+		uint32 Middle = First;
+		for (uint32 I = First; I < End; ++I)
+		{
+			if (AxisValue(B.Triangles[I].Centroid, Axis) < Split)
+			{
+				B.Triangles.Swap((int32)I, (int32)Middle);
+				++Middle;
+			}
+		}
+		// Avoid empty children when all triangle centroids coincide or cluster
+		// entirely on one side of the split plane.
+		if (Middle == First || Middle == End)
+		{
+			Middle = First + Count / 2;
+		}
+		const int32 Left = BuildNode(B, First, Middle - First, LeafLimit, Depth + 1);
+		const int32 Right = BuildNode(B, Middle, End - Middle, LeafLimit, Depth + 1);
+		FSoftwareRTBVHNode& Node = B.Nodes[NodeIndex];
+		Node.LeftChild = Left;
+		Node.RightChild = Right;
+		return NodeIndex;
+	}
+}
+
+void FSoftwareRTBLAS::Reset()
+{
+	Positions.Reset();
+	Triangles.Reset();
+	Nodes.Reset();
+}
+
+bool FSoftwareRTBVHBuilder::BuildStaticMeshBLAS(const FSoftwareRTStaticMeshGeometry& G, FSoftwareRTBLAS& Out, uint32 LeafLimit)
+{
+	Out.Reset();
+	if (G.Positions.Num() == 0 || G.Indices.Num() == 0 || (G.Indices.Num() % 3) != 0 ||
+		G.Sections.Num() == 0 || LeafLimit == 0)
+	{
+		return false;
+	}
+	Out.Positions = G.Positions;
+	for (const FSoftwareRTMeshSection& S : G.Sections)
+	{
+		if (!S.bVisibleInRayTracing || S.NumTriangles == 0)
+		{
+			continue;
+		}
+		const uint64 Begin = S.FirstIndex;
+		const uint64 End = Begin + (uint64)S.NumTriangles * 3ull;
+		if (End > (uint64)G.Indices.Num())
+		{
+			Out.Reset();
+			return false;
+		}
+		for (uint64 I = Begin; I < End; I += 3)
+		{
+			const uint32 A = G.Indices[(int32)I];
+			const uint32 C = G.Indices[(int32)I + 1];
+			const uint32 D = G.Indices[(int32)I + 2];
+			if (A >= (uint32)Out.Positions.Num() || C >= (uint32)Out.Positions.Num() || D >= (uint32)Out.Positions.Num())
+			{
+				Out.Reset();
+				return false;
+			}
+			const FVector& P0 = Out.Positions[A];
+			const FVector& P1 = Out.Positions[C];
+			const FVector& P2 = Out.Positions[D];
+			if (FVector::CrossProduct(P1 - P0, P2 - P0).SizeSquared() <= SMALL_NUMBER)
+			{
+				continue;
+			}
+			FSoftwareRTTriangle& T = Out.Triangles.AddDefaulted_GetRef();
+			T.Index0 = A; T.Index1 = C; T.Index2 = D;
+			T.MaterialIndex = S.MaterialIndex;
+			T.Bounds = FBox(ForceInit);
+			T.Bounds += P0; T.Bounds += P1; T.Bounds += P2;
+			T.Centroid = (P0 + P1 + P2) / 3.0f;
+		}
+	}
+	if (Out.Triangles.Num() == 0)
+	{
+		Out.Reset();
+		return false;
+	}
+	BuildNode(Out, 0, (uint32)Out.Triangles.Num(), LeafLimit, 0);
+	return Out.IsValid();
+}

```

## Design

- Flat node array; internal nodes have two child indices; leaves own contiguous triangle ranges.
- Split on the longest centroid-bounds axis, using an in-place midpoint partition.
- If centroids do not divide the set, fall back to a balanced split.
- Default leaf capacity is four triangles; recursion depth is capped at 64.
- Degenerate zero-area triangles are skipped.
- Geometry stays in mesh-local space. Instance transforms belong in a later TLAS.
- No traversal, TLAS, material cache, GPU upload or refit/update policy yet.

## Dependencies and validation

Requires patches 06 and 07. Files belong under Renderer/Private/SoftwareRT; no public API or Build.cs dependency is introduced.

Not compile-tested. Validate exact UE4.27.2 API compatibility and test empty input, single triangle, degenerate triangles, multiple/invisible sections, 16/32-bit source indices and clustered centroids. Also instrument triangle/node counts, build time and memory before connecting this builder to scene updates.

This is a BLAS builder, not the complete shared Software RT Core.
