# Patch 02 — Register material-linked compute evaluation shader

Target: UE 4.27.2 source. Apply after patch 01, from the engine repository root.

This patch adds a material-linked compute shader type and compiles Base Color + Emissive as compute-frequency material properties for materials with the new usage flag.

It deliberately does **not** add dispatch yet. It proves the intended shader-map route and HLSL generation; the next patch must supply the actual hit context, bind the output UAV, call FMaterialShader::SetParameters with a valid FSceneView, and dispatch. The test entrypoint below initializes an empty material parameter struct and uses UV=(0,0), so it is only a compile smoke test, not a valid ray-hit evaluation.

## 1. Include the opt-in flag in the material shader permutation snapshot

```diff
diff --git a/Engine/Source/Runtime/Engine/Public/MaterialShared.h b/Engine/Source/Runtime/Engine/Public/MaterialShared.h
--- a/Engine/Source/Runtime/Engine/Public/MaterialShared.h
+++ b/Engine/Source/Runtime/Engine/Public/MaterialShared.h
@@ -1354,7 +1354,8 @@ enum ECompiledMaterialProperty
 	CompiledMP_EmissiveColorCS = MP_MAX,
 	CompiledMP_PrevWorldPositionOffset,
+	CompiledMP_BaseColorCS,
 	CompiledMP_MAX
 };
@@ -3182,4 +3182,5 @@ struct FMaterialShaderParameters
 			uint64 bIsUsedWithVirtualHeightfieldMesh : 1;
 			uint64 bIsStencilTestEnabled : 1;
+			uint64 bIsUsedWithSoftwareRayTracing : 1;
 		};
 	};
@@ -3240,4 +3240,5 @@ struct FMaterialShaderParameters
 		bIsUsedWithVirtualHeightfieldMesh = InMaterial->IsUsedWithVirtualHeightfieldMesh();
 		bIsStencilTestEnabled = InMaterial->IsStencilTestEnabled();
+		bIsUsedWithSoftwareRayTracing = InMaterial->IsUsedWithSoftwareRayTracing();
 	}
 };
```

The new bit is appended to the packed flags; existing bit positions remain unchanged. The enum value is appended after the existing entries, so existing compiled-property indices are preserved.

## 2. Compile Base Color and Emissive in the compute frequency

```diff
diff --git a/Engine/Source/Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp b/Engine/Source/Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp
--- a/Engine/Source/Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp
+++ b/Engine/Source/Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp
@@ -543,7 +543,7 @@ bool FHLSLMaterialTranslator::Translate()
-		bCompileForComputeShader = Material->IsLightFunction();
+		bCompileForComputeShader = Material->IsLightFunction() || Material->IsUsedWithSoftwareRayTracing();
@@ -699,6 +699,7 @@
 		if (bCompileForComputeShader)
 		{
 			Chunk[CompiledMP_EmissiveColorCS]	= Material->CompilePropertyAndSetMaterialProperty(MP_EmissiveColor, this, SF_Compute);
+			Chunk[CompiledMP_BaseColorCS]		= Material->CompilePropertyAndSetMaterialProperty(MP_BaseColor, this, SF_Compute);
 		}
@@ -1045,6 +1046,13 @@
 			case CompiledMP_EmissiveColorCS:
 			    if (bCompileForComputeShader)
 				{
 					GetFixedParameterCode(Chunk[PropertyId], SharedPropertyCodeChunks[SF_Compute], TranslatedCodeChunkDefinitions[PropertyId], TranslatedCodeChunks[PropertyId]);
 				}
 				break;
+			case CompiledMP_BaseColorCS:
+				if (bCompileForComputeShader)
+				{
+					GetFixedParameterCode(Chunk[PropertyId], SharedPropertyCodeChunks[SF_Compute], TranslatedCodeChunkDefinitions[PropertyId], TranslatedCodeChunks[PropertyId]);
+				}
+				break;
 			case CompiledMP_PrevWorldPositionOffset:
@@ -1632,6 +1640,7 @@ FString FHLSLMaterialTranslator::GetMaterialShaderCode()
 	if (bCompileForComputeShader)
 	{
 		LazyPrintf.PushParam(*GenerateFunctionCode(CompiledMP_EmissiveColorCS));
+		LazyPrintf.PushParam(*GenerateFunctionCode(CompiledMP_BaseColorCS));
 	}
 	else
 	{
 		LazyPrintf.PushParam(TEXT("return 0"));
+		LazyPrintf.PushParam(TEXT("return 0"));
 	}
```

## 3. Add the generated Base Color function placeholder

```diff
diff --git a/Engine/Shaders/Private/MaterialTemplate.ush b/Engine/Shaders/Private/MaterialTemplate.ush
--- a/Engine/Shaders/Private/MaterialTemplate.ush
+++ b/Engine/Shaders/Private/MaterialTemplate.ush
@@ -2088,5 +2088,10 @@ half3 GetMaterialEmissiveForCS(FMaterialPixelParameters Parameters)
 {
 %s;
 }
+
+half3 GetMaterialBaseColorForCS(FMaterialPixelParameters Parameters)
+{
+%s;
+}
```

The additional LazyPrintf argument in section 2 must remain in exactly the same order as this new `%s` placeholder.

## 4. Register the compute shader type

New file: `Engine/Source/Runtime/Renderer/Private/SoftwareRTMaterialEvaluation.h`

```cpp
#pragma once

#include "MaterialShader.h"

class FSoftwareRTMaterialEvaluationCS : public FMaterialShader
{
	DECLARE_SHADER_TYPE(FSoftwareRTMaterialEvaluationCS, Material);

public:
	FSoftwareRTMaterialEvaluationCS() = default;

	FSoftwareRTMaterialEvaluationCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMaterialShader(Initializer)
	{
		OutputBuffer.Bind(Initializer.ParameterMap, TEXT("OutputBuffer"));
	}

	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return Parameters.MaterialParameters.bIsUsedWithSoftwareRayTracing;
	}

	LAYOUT_FIELD(FShaderResourceParameter, OutputBuffer);
};
```

New file: `Engine/Source/Runtime/Renderer/Private/SoftwareRTMaterialEvaluation.cpp`

```cpp
#include "SoftwareRTMaterialEvaluation.h"

IMPLEMENT_MATERIAL_SHADER_TYPE(
	,
	FSoftwareRTMaterialEvaluationCS,
	TEXT("/Engine/Private/SoftwareRTMaterialEvaluation.usf"),
	TEXT("MainCS"),
	SF_Compute
);
```

New shader file: `Engine/Shaders/Private/SoftwareRTMaterialEvaluation.usf`

```hlsl
#include "/Engine/Generated/Material.ush"

RWStructuredBuffer<float4> OutputBuffer;

[numthreads(64, 1, 1)]
void MainCS(uint3 DispatchThreadId : SV_DispatchThreadID)
{
	FMaterialPixelParameters Parameters = MakeInitializedMaterialPixelParameters();

#if NUM_TEX_COORD_INTERPOLATORS > 0
	Parameters.TexCoords[0] = float2(0.0f, 0.0f);
#endif

	OutputBuffer[DispatchThreadId.x * 2 + 0] = float4(GetMaterialBaseColorForCS(Parameters), 1.0f);
	OutputBuffer[DispatchThreadId.x * 2 + 1] = float4(GetMaterialEmissiveForCS(Parameters), 1.0f);
}
```

## Important limitations / validation

- The compute translation path still forces automatic texture samples to mip 0 in UE 4.27.2. This patch does not solve ray-footprint gradients.
- The test shader initializes no world position, normal, tangent, barycentrics, or ray footprint. Graph nodes depending on those values are not meaningfully evaluated yet.
- This only registers the compute type in the material shader map. It does not dispatch it or bind a live material render proxy.
- Before applying, verify the exact hunk contexts and compile the shader. This patch has not been applied to a local engine or compiled.
- If shader compilation fails because the generated material include or a shader parameter binding differs in this branch, stop and fix that before adding dispatch. Do not work around it by switching to FGlobalShader or manually duplicating material uniform-expression bindings.
