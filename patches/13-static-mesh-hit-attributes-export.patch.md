# 13 — Static-mesh hit attributes export

Status: proposed unified diff, API checked against the UE4.27 mirror below. Not applied to a complete UE4.27.2 checkout or compiled.

## Purpose

Extend Patch 07's CPU-owned canonical geometry with the per-vertex attributes required to reconstruct a useful surface hit:

- all static-mesh UV channels;
- tangent X and tangent Z (including the tangent-Z W component);
- vertex color;
- an explicit UV channel count and vertex-color availability flag.

Patch 11 already supplies barycentrics in triangle-index order. A later hit-reconstruction stage can use those barycentrics to interpolate these attributes. This patch only exports data; it does not add interpolation to the traversal loop or evaluate materials there.

## Diff

```diff
diff --git a/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h b/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
--- a/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
+++ b/Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
@@
 struct FSoftwareRTStaticMeshGeometry
 {
 	TArray<FVector> Positions;
 	TArray<uint32> Indices;
 	TArray<FSoftwareRTMeshSection> Sections;
+
+	// Per-vertex data. UVs are vertex-major:
+	// UVs[VertexIndex * NumTexCoords + UVChannel].
+	uint32 NumTexCoords = 0;
+	TArray<FVector2D> UVs;
+	TArray<FVector4> TangentX;
+	TArray<FVector4> TangentZ;
+	TArray<FLinearColor> VertexColors;
+	bool bHasVertexColors = false;
 
 	void Reset()
 	{
 		Positions.Reset();
 		Indices.Reset();
 		Sections.Reset();
+		NumTexCoords = 0;
+		UVs.Reset();
+		TangentX.Reset();
+		TangentZ.Reset();
+		VertexColors.Reset();
+		bHasVertexColors = false;
 	}
 };
diff --git a/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp b/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
--- a/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
+++ b/Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp
@@
 	const FPositionVertexBuffer& PositionBuffer = LOD->VertexBuffers.PositionVertexBuffer;
+	const FStaticMeshVertexBuffer& StaticVertexBuffer = LOD->VertexBuffers.StaticMeshVertexBuffer;
+	const FColorVertexBuffer& ColorBuffer = LOD->VertexBuffers.ColorVertexBuffer;
 	const int32 NumVertices = (int32)PositionBuffer.GetNumVertices();
 	const int32 NumIndices = LOD->IndexBuffer.GetNumIndices();
+	const uint32 NumTexCoords = StaticVertexBuffer.GetNumTexCoords();
 
 	if (NumVertices <= 0 || NumIndices <= 0 || (NumIndices % 3) != 0 ||
-		!PositionBuffer.GetVertexData() || !LOD->IndexBuffer.HasCPUData())
+		!PositionBuffer.GetVertexData() || !LOD->IndexBuffer.HasCPUData() ||
+		StaticVertexBuffer.GetNumVertices() != (uint32)NumVertices ||
+		NumTexCoords == 0 ||
+		(uint64)NumVertices * (uint64)NumTexCoords > (uint64)MAX_int32 ||
+		!StaticVertexBuffer.GetTangentData() ||
+		!StaticVertexBuffer.GetTexcoordData())
 	{
 		return false;
 	}
+
+	const uint32 NumColorVertices = ColorBuffer.GetNumVertices();
+	if (NumColorVertices != 0 &&
+		(NumColorVertices != (uint32)NumVertices || !ColorBuffer.GetVertexData()))
+	{
+		return false;
+	}
@@
 	OutGeometry.Positions.Reserve(NumVertices);
+	OutGeometry.NumTexCoords = NumTexCoords;
+	OutGeometry.UVs.Reserve((int32)((uint64)NumVertices * (uint64)NumTexCoords));
+	OutGeometry.TangentX.Reserve(NumVertices);
+	OutGeometry.TangentZ.Reserve(NumVertices);
+	OutGeometry.VertexColors.Reserve(NumVertices);
+	OutGeometry.bHasVertexColors = NumColorVertices == (uint32)NumVertices;
 	for (int32 VertexIndex = 0; VertexIndex < NumVertices; ++VertexIndex)
 	{
 		OutGeometry.Positions.Add(PositionBuffer.VertexPosition((uint32)VertexIndex));
+
+		OutGeometry.TangentX.Add(StaticVertexBuffer.VertexTangentX((uint32)VertexIndex));
+		OutGeometry.TangentZ.Add(StaticVertexBuffer.VertexTangentZ((uint32)VertexIndex));
+
+		for (uint32 UVChannel = 0; UVChannel < NumTexCoords; ++UVChannel)
+		{
+			OutGeometry.UVs.Add(StaticVertexBuffer.GetVertexUV((uint32)VertexIndex, UVChannel));
+		}
+
+		// A missing color stream has the same neutral value as the default
+		// vertex-color input: opaque white.
+		const FColor VertexColor = OutGeometry.bHasVertexColors
+			? ColorBuffer.VertexColor((uint32)VertexIndex)
+			: FColor::White;
+		OutGeometry.VertexColors.Add(FLinearColor(VertexColor));
 	}
@@
 	return true;
 }
```

## Attribute and interpolation contract

For a hit triangle with vertex indices I0/I1/I2 and barycentrics B0/B1/B2:

- UV(channel) = UV[I0, channel] * B0 + UV[I1, channel] * B1 + UV[I2, channel] * B2.
- Tangent X/Z are interpolated as vectors, then normalized/orthogonalized when building the shading frame. Preserve tangent-Z W as the handedness signal; do not treat it as a vector component to normalize.
- Vertex color is exported as FLinearColor before interpolation.
- Position remains the existing canonical position array; the hit's world position and geometric normal remain the Patch 12 outputs.
- The material slot remains the section's MaterialIndex already carried by the triangle.

Do not interpolate across section boundaries: the triangle's section/material identity is authoritative.

## Deliberate limits

- This exports asset LOD vertex colors only. Per-component override color buffers and per-instance custom data are not represented.
- No per-instance material override resolution yet. The section material slot is only the mesh slot index.
- No WPO, spline deformation, skinning, Nanite, Landscape or custom vertex-factory support.
- This does not solve UV seams: the canonical triangle indices refer to the correct duplicated mesh vertices, so interpolation remains within the triangle.
- UV channel count is taken from the LOD's static vertex buffer. No assumption is made that Lightmap UVs are present or unique.
- CPU readability is required. If tangent or UV data is unavailable, export fails rather than returning incomplete hit attributes.
- The next stage should add barycentric interpolation to a separate hit-surface record and validate UV channels, tangent handedness, vertex colors, and seam behavior. Keep traversal itself free of material evaluation.

## UE4.27 API verification

Checked against `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`:

- `FStaticMeshVertexBuffer::GetNumTexCoords()`
- `FStaticMeshVertexBuffer::GetNumVertices()`
- `FStaticMeshVertexBuffer::GetTangentData()`
- `FStaticMeshVertexBuffer::GetTexcoordData()`
- `FStaticMeshVertexBuffer::VertexTangentX()`
- `FStaticMeshVertexBuffer::VertexTangentZ()`
- `FStaticMeshVertexBuffer::GetVertexUV()`
- `FColorVertexBuffer::GetNumVertices()`
- `FColorVertexBuffer::GetVertexData()`
- `FColorVertexBuffer::VertexColor()`

These declarations were verified in the mirror; the combined patch hunk and exact target-tree context have not been compiled in the user's UE4.27.2 checkout.
