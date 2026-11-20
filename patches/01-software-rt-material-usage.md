# Patch 01 — Material usage flag for software RT

Target: UE 4.27.2 source. Apply from the engine repository root.

This is deliberately only the material eligibility plumbing. It does not yet register or compile a compute shader.

The flag is inherited by material instances through FMaterialResource::Material, matching the existing usage-flag pattern. UMaterial::PostEditChangePropertyInternal calls CacheResourceShadersForRendering with ID regeneration for ordinary property edits, so changing this UPROPERTY invalidates the existing material shader map.

Before applying, verify the four context anchors against the exact engine checkout. This patch has not been compiled.

```diff
diff --git a/Engine/Source/Runtime/Engine/Classes/Materials/Material.h b/Engine/Source/Runtime/Engine/Classes/Materials/Material.h
--- a/Engine/Source/Runtime/Engine/Classes/Materials/Material.h
+++ b/Engine/Source/Runtime/Engine/Classes/Materials/Material.h
@@
 	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Usage)
 	uint32 bUsedWithVirtualHeightfieldMesh : 1;
+
+	/**
+	 * Indicates that this material and its instances can be evaluated by software ray tracing.
+	 * Enables material-linked compute shader variants and increases shader compile time and memory usage.
+	 */
+	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Usage)
+	uint32 bUsedWithSoftwareRayTracing : 1;
 
 	/** 
 	 * Indicates that the material and its instances can be used with Slate UI and UMG
diff --git a/Engine/Source/Runtime/Engine/Private/Materials/Material.cpp b/Engine/Source/Runtime/Engine/Private/Materials/Material.cpp
--- a/Engine/Source/Runtime/Engine/Private/Materials/Material.cpp
+++ b/Engine/Source/Runtime/Engine/Private/Materials/Material.cpp
@@
 	bIsSky = false;
 	bUsedWithWater = false;
+	bUsedWithSoftwareRayTracing = false;
 	BlendableLocation = BL_AfterTonemapping;
diff --git a/Engine/Source/Runtime/Engine/Public/MaterialShared.h b/Engine/Source/Runtime/Engine/Public/MaterialShared.h
--- a/Engine/Source/Runtime/Engine/Public/MaterialShared.h
+++ b/Engine/Source/Runtime/Engine/Public/MaterialShared.h
@@
 	virtual bool IsUsedWithVirtualHeightfieldMesh() const { return false; }
+	virtual bool IsUsedWithSoftwareRayTracing() const { return false; }
 	ENGINE_API virtual enum EMaterialTessellationMode GetTessellationMode() const;
@@
 	ENGINE_API virtual bool IsUsedWithVirtualHeightfieldMesh() const override;
+	ENGINE_API virtual bool IsUsedWithSoftwareRayTracing() const override;
 	ENGINE_API virtual enum EMaterialTessellationMode GetTessellationMode() const override;
diff --git a/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp b/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp
--- a/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp
+++ b/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp
@@
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

Next patch must add the compute material shader type and gate its ShouldCompilePermutation on IsUsedWithSoftwareRayTracing(). The translator change alone is insufficient.
