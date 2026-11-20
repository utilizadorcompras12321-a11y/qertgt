# Patch 43 — Standalone Capture VF: C++ Skeleton and First Draw Proof

**Target:** UE 4.27.2  
**Branch:** plugin/software-rt-material-evaluation  
**Mirror:** AlexMercer-MA/UnrealEngine-4.27  
**Pinned mirror commit:** 306a7e9f7fb53f14c2cb3bfd3854ad906710a17e  
**Status:** API audit and implementation plan. Not applied to the user's Engine checkout; not compiled.

## 1. Decision

Implement the capture vertex factory as a standalone subclass of FVertexFactory, not as a subclass of FLocalVertexFactory.

The first test should use:
- one persistent, interleaved vertex buffer;
- three expanded vertices for one source triangle;
- a dedicated vertex declaration;
- a dedicated VF shader include;
- a minimal mesh-material shader pass that writes a debug value;
- an uncached dynamic mesh draw path.

Do not implement chart capture, Material Graph evaluation, atlas allocation, or material feature support in this test. The first milestone is only: the custom VF fetches the intended per-corner data and the submitted draw reaches the pixel shader correctly.

A GlobalShader fullscreen/indirect draw is not an adequate substitute: it would not exercise the custom VF through the mesh draw-command path.

## 2. UE4.27 API facts verified in the pinned mirror

### 2.1 Base class and ownership

FVertexFactory is declared in Engine/Source/Runtime/RenderCore/Public/VertexFactory.h. It derives from FRenderResource and is not abstract in the inspected declaration. It provides GetType(), GetStreams(), SetDeclaration(), protected Streams, protected InitDeclaration(), protected AccessStreamComponent(), and virtual ReleaseRHI().

The custom VF implements InitRHI() to populate its streams and create/set its vertex declaration. Its vertex buffer must outlive the VF and every draw command that can reference it.

### 2.2 Stream and declaration constraints

FVertexStream stores VertexBuffer, Offset, Stride, and VertexStreamUsage. FVertexFactory::GetStreams() emits RHI stream bindings from the protected Streams array. For the first test, use one stream at index 0 with offset 0.

FVertexStreamComponent stores Offset and Stride as uint8 and its constructors check both are <= 255. Keep the interleaved vertex struct below 255 bytes if using this helper. The proposed first prototype is much smaller.

### 2.3 Type registration

The exact macro overload in this mirror is:

IMPLEMENT_VERTEX_FACTORY_TYPE_EX(FactoryClass, ShaderFilename, bUsedWithMaterials, bSupportsStaticLighting, bSupportsDynamicLighting, bPrecisePrevWorldPos, bSupportsPositionOnly, bSupportsCachingMeshDrawCommands, bSupportsPrimitiveIdStream)

The non-EX macro hardcodes both mesh-command caching and primitive-ID-stream support to false. Use the explicit EX form so the limitations are visible.

The type constructor registers the VF type globally and asserts it is loaded before shader type initialization. The implementation must live in a module loaded early enough; for Engine integration, use the Renderer module's established startup/load phase rather than a late-loaded plugin module.

### 2.4 Shader parameter dispatch

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FactoryClass, SF_Vertex, ParameterClass) registers the vertex-frequency parameter class. The registered class must expose GetElementShaderBindings(...).

For this first test, the VF parameter class can be a no-op if no VF-specific uniform parameters are needed. Vertex streams themselves are emitted by the VF's stream list, not by inventing a VertexFactoryUserData pointer.

### 2.5 Shader compilation

FVertexFactoryType::ModifyCompilationEnvironment() automatically maps the VF's shader filename to /Engine/Generated/VertexFactory.ush and defines HAS_PRIMITIVE_UNIFORM_BUFFER.

The custom VF must provide a valid ShouldCompilePermutation(). Its compilation environment callback can be minimal initially. Do not advertise static lighting, dynamic lighting, precise previous positions, position-only support, mesh draw command caching, or primitive-ID stream support until each is implemented and tested.

## 3. Minimal C++ shape

The following is a structural skeleton, not a drop-in patch. Exact includes, module export decoration, buffer class, vertex element formats, shader class, and pass integration must be reconciled with the target checkout before compilation.

    struct FSoftwareRTDebugVertex
    {
        FVector Position;
        FVector2D DebugData;
    };

    class FSoftwareRTDebugVertexFactory : public FVertexFactory
    {
        DECLARE_VERTEX_FACTORY_TYPE(FSoftwareRTDebugVertexFactory);

    public:
        explicit FSoftwareRTDebugVertexFactory(ERHIFeatureLevel::Type FeatureLevel)
            : FVertexFactory(FeatureLevel)
        {}

        static bool ShouldCompilePermutation(
            const FVertexFactoryShaderPermutationParameters& Parameters);

        static void ModifyCompilationEnvironment(
            const FVertexFactoryShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& OutEnvironment);

        virtual void InitRHI() override
        {
            check(Streams.Num() == 0);

            FVertexStream* Stream = new (Streams) FVertexStream;
            Stream->VertexBuffer = VertexBuffer;
            Stream->Offset = 0;
            Stream->Stride = sizeof(FSoftwareRTDebugVertex);

            FVertexDeclarationElementList Elements;
            Elements.Add(FVertexElement(
                0, STRUCT_OFFSET(FSoftwareRTDebugVertex, Position),
                VET_Float3, 0, sizeof(FSoftwareRTDebugVertex), false));
            Elements.Add(FVertexElement(
                0, STRUCT_OFFSET(FSoftwareRTDebugVertex, DebugData),
                VET_Float2, 1, sizeof(FSoftwareRTDebugVertex), false));

            InitDeclaration(Elements);
        }

        void SetVertexBuffer(FVertexBuffer* InBuffer)
        {
            // Assign before InitResource; do not replace while initialized.
            VertexBuffer = InBuffer;
        }

    private:
        FVertexBuffer* VertexBuffer = nullptr; // Borrowed; owner retains it.
    };

The skeleton deliberately omits the buffer upload implementation. That must use the exact RHI creation API in the target branch and handle empty input safely.

UE4.27 uses FVector/FVector2D float-based types; do not copy UE5 FVector3f/FVector2f code into this branch. Ensure the struct layout matches the declaration exactly.

Do not mutate VertexBuffer after InitResource(). Do not call InitDeclaration() and then overwrite the declaration independently. The stream's buffer pointer is borrowed; the owner retains the buffer until the VF has been released and all referencing draw commands have passed the retirement fence.

FVertexFactory::ReleaseRHI() releases declarations and clears streams. If the subclass adds owning RHI references, release those in its override and call the base implementation.

## 4. Debug material shader and draw path

Use a minimal FMeshMaterialShader vertex/pixel shader pair registered against the custom VF type. The debug pixel shader should write a simple value derived from the interpolated per-vertex debug attribute (or a constant color for the first smoke test).

The pass should:
1. Build one FMeshBatch with the custom VF.
2. Use one FMeshBatchElement with a valid index buffer, FirstIndex, NumPrimitives = 1, and a valid primitive/material binding path.
3. Preserve a valid scene primitive identity / primitive uniform data if the selected mesh-material shader path requires it.
4. Use the custom VF type when retrieving the shader from the material shader map.
5. Build and submit through the uncached dynamic mesh draw route.
6. Keep VF, vertex buffer, index buffer, shader resources, and any uniform buffers alive until the draw is retired.

Do not set bSupportsPrimitiveIdStream=true as a shortcut. The first test should use the ordinary primitive binding mode supported by the chosen pass and shader. If a minimal mesh-material shader still requires scene/view/primitive bindings that cannot be supplied without a real scene primitive, use a controlled test primitive in the scene; do not fake a primitive ID.

## 5. First test geometry

Use one triangle with three expanded vertices. Each vertex carries source/local position and a small debug attribute, for example corner ID encoded as float. Use a simple index buffer {0, 1, 2}.

Expected result:
- the triangle appears in the capture/debug target;
- the debug attribute interpolates across the triangle;
- changing the camera does not alter the triangle's local positions beyond the expected primitive transform;
- no original mesh UVs, tangents, vertex colors, WPO, PDO, or Material Graph are involved yet.

After this succeeds, add original UV and tangent attributes, then prove their interpolation. Only after that should the VF feed the dedicated material-capture pixel shader described in Patch 39.

## 6. Stop conditions

Do not proceed to Surface Cache material evaluation if:
- the custom VF type is not registered before shader initialization;
- shader compilation fails for the custom VF;
- the vertex declaration does not match the HLSL input ABI;
- the draw path cannot bind required primitive/view data without invasive changes;
- resource retirement cannot guarantee the VF and buffers survive command execution;
- camera movement changes source attributes because chart raster coordinates leaked into material/world-position inputs.

## 7. Required checkout verification before an executable patch

The pinned mirror confirms the base-class and registration contracts, but it does not prove the user's local source tree is identical. Before writing a compile-ready .patch, compare these exact files against C:\Users\Utilizador~\Desktop\UnrealEngine:

- Engine/Source/Runtime/RenderCore/Public/VertexFactory.h
- Engine/Source/Runtime/RenderCore/Private/VertexFactory.cpp
- Engine/Source/Runtime/Renderer/Private/VectorFieldVisualization.cpp (locate the actual path if moved)
- Engine/Source/Runtime/Engine/Private/Particles/ParticleVertexFactory.cpp
- Engine/Source/Runtime/Engine/Public/ParticleVertexFactory.h
- the selected Renderer mesh-pass processor and dynamic draw submission call sites.

Then implement only the debug VF and one-triangle draw. Do not combine this first compile test with the Surface Cache atlas or material shader.

## 8. References in the pinned mirror

- Engine/Source/Runtime/RenderCore/Public/VertexFactory.h: FVertexStreamComponent, FVertexFactoryType, registration macros, FVertexFactory, FVertexStream, InitDeclaration, GetStreams.
- Engine/Source/Runtime/RenderCore/Private/VertexFactory.cpp: FVertexFactoryType registration and FVertexFactory::GetStreams/ReleaseRHI.
- Engine/Source/Runtime/Engine/Public/LocalVertexFactory.h: Local VF lifecycle and shader parameter binding pattern; not the chosen inheritance model.
- Engine/Source/Runtime/Engine/Private/Particles/ParticleVertexFactory.cpp: separate VF parameter classes, vertex declaration construction, stream setup, and parameter registration precedent.
