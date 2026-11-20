# Patch 01 — Material usage flag for software RT

Target: UE 4.27.2 source. Apply from the engine repository root.

This patch adds only the eligibility flag and its propagation to FMaterialResource. It does not yet register or compile a compute shader.

Material instances inherit the flag from their base UMaterial because FMaterialResource reads the base material property, matching the existing usage-flag pattern.

Changing the UPROPERTY uses the normal UMaterial::PostEditChangePropertyInternal path: it regenerates the material ID and calls CacheResourceShadersForRendering, invalidating the old shader map. This avoids introducing a separate cache key for the flag.

The patch uses line-numbered unified hunks and the exact source contexts inspected in the UE 4.27.2 dump. It has not been applied to a local engine or compiled.

```diff
diff --git a/Engine/Source/Runtime/Engine/Classes/Materials/Material.h b/Engine/Source/Runtime/Engine/Classes/Materials/Material.h
--- a/Engine/Source/Runtime/Engine/Classes/Materials/Material.h
+++ b/Engine/Source/Runtime/Engine/Classes/Materials/Material.h
@@ -726,4 +726,11 @@
 	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Usage)
 	uint32 bUsedWithVirtualHeightfieldMesh : 1;
 
+	/**
+	 * Indicates that this material and its instances can be evaluated by software ray tracing.
+	 * Enables material-linked compute shader variants and increases shader compile time and memory usage.
+	 */
+	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Usage)
+	uint32 bUsedWithSoftwareRayTracing : 1;
+
 	/** 
 	 * Indicates that the material and its instances can be used with Slate UI and UMG
diff --git a/Engine/Source/Runtime/Engine/Private/Materials/Material.cpp b/Engine/Source/Runtime/Engine/Private/Materials/Material.cpp
--- a/Engine/Source/Runtime/Engine/Private/Materials/Material.cpp
+++ b/Engine/Source/Runtime/Engine/Private/Materials/Material.cpp
@@ -953,4 +953,5 @@
 	bApplyCloudFogging = false;
 	bIsSky = false;
 	bUsedWithWater = false;
+	bUsedWithSoftwareRayTracing = false;
 	BlendableLocation = BL_AfterTonemapping;
diff --git a/Engine/Source/Runtime/Engine/Public/MaterialShared.h b/Engine/Source/Runtime/Engine/Public/MaterialShared.h
--- a/Engine/Source/Runtime/Engine/Public/MaterialShared.h
+++ b/Engine/Source/Runtime/Engine/Public/MaterialShared.h
@@ -1685,7 +1685,8 @@
 	virtual bool IsUsedWithWater() const { return false; }
 	virtual bool IsUsedWithHairStrands() const { return false; }
 	virtual bool IsUsedWithLidarPointCloud() const { return false; }
 	virtual bool IsUsedWithVirtualHeightfieldMesh() const { return false; }
+	virtual bool IsUsedWithSoftwareRayTracing() const { return false; }
 	ENGINE_API virtual enum EMaterialTessellationMode GetTessellationMode() const;
@@ -2549,2 +2549,3 @@
 	ENGINE_API virtual bool IsUsedWithVirtualHeightfieldMesh() const override;
+	ENGINE_API virtual bool IsUsedWithSoftwareRayTracing() const override;
 	ENGINE_API virtual enum EMaterialTessellationMode GetTessellationMode() const override;
diff --git a/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp b/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp
--- a/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp
+++ b/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp
@@ -1245,6 +1245,11 @@
 bool FMaterialResource::IsUsedWithVirtualHeightfieldMesh() const
 {
 	return Material->bUsedWithVirtualHeightfieldMesh;
 }
+
+bool FMaterialResource::IsUsedWithSoftwareRayTracing() const
+{
+	return Material->bUsedWithSoftwareRayTracing;
+}
 
 bool FMaterialResource::IsUsedWithLandscape() const
 {
```

Next: add a material shader type whose ShouldCompilePermutation checks IsUsedWithSoftwareRayTracing(), then extend the translator to emit the required compute properties. Do not yet change TextureSample's global compute mip behavior: ray-footprint gradients need a separate explicit path, otherwise existing compute users may regress.
