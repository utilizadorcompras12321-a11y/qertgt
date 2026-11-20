# 52 — Software RT Capture Vertex Factory Registration and Module Placement

Status: registration placement audit and implementation contract. No Engine source changed, applied, or compiled.

Mirror audited: `AlexMercer-MA/UnrealEngine-4.27`, commit `306a7e9f7fb53f14c2cb3bfd3854ad906710a17e`.

## Decision

Place the standalone Software RT capture Vertex Factory implementation in the existing **Engine runtime module**, under:

- Header: `Engine/Source/Runtime/Engine/Public/SoftwareRTCaptureVertexFactory.h`
- Implementation: `Engine/Source/Runtime/Engine/Private/SoftwareRTCaptureVertexFactory.cpp`
- Shader include: `Engine/Shaders/Private/SoftwareRTCaptureVertexFactory.ush`

This is a proposed layout, not a claim that these files already exist.

The registration must be a static translation-unit registration in the Engine module. Do not register the VF lazily from a renderer callback, plugin startup after shader initialization, or first capture request.

## Evidence

### 1. Engine already contains the Local VF registration precedent

`Engine/Source/Runtime/Engine/Private/LocalVertexFactory.cpp` contains:

- `IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLocalVertexFactory, SF_Vertex, FLocalVertexFactoryShaderParameters)`
- `IMPLEMENT_VERTEX_FACTORY_TYPE_EX(FLocalVertexFactory, "/Engine/Private/LocalVertexFactory.ush", ...)`

This is the closest placement precedent for a material-capable VF that is part of the Engine module.

### 2. Engine has the required module dependency

`Engine/Source/Runtime/Engine/Engine.Build.cs` lists `RenderCore` in `PublicDependencyModuleNames`. The Engine module can therefore use the RenderCore Vertex Factory registration and base APIs without introducing a new module dependency.

Do not add an Engine -> Renderer dependency. Renderer already depends on Engine; reversing that dependency would create an architectural cycle.

### 3. VF registration timing is strict

`FVertexFactoryType::FVertexFactoryType()` in `RenderCore/Private/VertexFactory.cpp` checks:

`!bInitializedSerializationHistory`

The source comment explicitly says VF types must be implemented in modules loaded before `InitializeShaderTypes`, and points to `ELoadingPhase::PostConfigInit` for modules that need earlier loading.

`LaunchEngineLoop.cpp` calls `GetRendererModule()` and then `InitializeShaderTypes()` in the shader initialization sequence. Engine's core module load occurs earlier in startup. Keeping the registration in Engine's already-loaded module avoids creating a separate module whose descriptor would need an explicit early loading phase.

Important: the exact Engine module load ordering must still be checked in the user's local source tree if that tree differs from the pinned mirror.

### 4. Renderer module is the wrong owner for this VF

The Renderer module depends on Engine. Placing a material VF in Renderer would make the VF registration dependent on Renderer loading before shader type initialization, and would put a geometry/material input type in the higher-level consumer module.

The capture VF is a reusable Engine rendering primitive, not a renderer pass. The renderer should consume it, not own its type registration.

## Class and registration contract

The class should derive directly from `FVertexFactory`, not `FLocalVertexFactory`.

Reasons:
- capture geometry is expanded into dedicated vertices;
- chart/raster coordinates are a separate attribute domain;
- original material attributes must remain independent of atlas raster position;
- Local VF's data layout and uniform-buffer assumptions are not the capture VF ABI.

The new class must provide its own:
- vertex declaration and stream setup in `InitRHI()`;
- resource release in `ReleaseRHI()`;
- shader parameter binding implementation;
- vertex shader input/interpolant ABI;
- static type registration and parameter-type registration.

The initial VF registration should advertise only implemented capabilities:
- `bUsedWithMaterials = true`;
- static lighting = false;
- dynamic lighting = false;
- precise previous world position = false;
- position-only = false;
- cached mesh draw commands = false;
- primitive ID stream = false.

The corresponding `IMPLEMENT_VERTEX_FACTORY_TYPE_EX` flags are therefore intended to be:

`IMPLEMENT_VERTEX_FACTORY_TYPE_EX(FSoftwareRTCaptureVertexFactory, "/Engine/Private/SoftwareRTCaptureVertexFactory.ush", true, false, false, false, false, false, false);`

This line is a registration target only. It must not be committed as a standalone code change until the class and required static callbacks exist.

Register the vertex-frequency parameter type with the concrete capture VF shader-parameter class. Do not copy Local VF's parameter class unless its implementation actually matches the capture VF's own streams and bindings.

## Build.cs impact

No new module is required and no Build.cs dependency change is expected:
- Engine already depends publicly on RenderCore.
- Engine already owns the Local VF implementation.
- Renderer already depends on Engine.

If a later implementation adds a new module, that module must be loaded before shader type initialization; merely adding a Build.cs dependency is not enough to establish registration order.

## Files that must not be changed for this step

- `Renderer.Build.cs`: no new dependency or module ownership.
- Engine module descriptor: no new module is being introduced.
- `LocalVertexFactory.h/.cpp`: do not modify or overload the stock Local VF to carry chart-specific data.
- `MaterialShared.*`: Patch 50's opt-in remains a separate concern.

## Next implementation slice

Patch 53 should add only the VF declaration and static registration skeleton:
1. Declare `FSoftwareRTCaptureVertexFactory` in the Engine public header.
2. Declare a dedicated `FVertexFactoryShaderParameters` subclass and register its type layout.
3. Add the `SF_Vertex` parameter-type registration.
4. Add the VF type registration with the restricted capability flags above.
5. Leave vertex buffer upload, declaration construction, material shader classes, and draw submission out of this slice unless their required ABI is already defined.

A registration-only skeleton must not pretend to be a usable VF. It should be treated as incomplete until `InitRHI`, streams, bindings, and the matching shader include are implemented and compile.

## Validation checklist

- Confirm the Engine module is loaded before `InitializeShaderTypes()` in the exact local UE4.27.2 checkout.
- Confirm the new translation unit is compiled into the Engine module.
- Confirm the shader virtual path maps to the actual file under `Engine/Shaders/Private`.
- Confirm the VF type is present in `FVertexFactoryType::GetSortedMaterialTypes()`.
- Confirm startup does not trigger the late-registration assertion.
- Confirm the VF does not claim unsupported static/dynamic lighting, primitive ID stream, tessellation, or draw-command caching.
- Build the Engine and start the Editor; no source-mirror audit is a substitute for this validation.

## Explicit limitations

This document is an audit/placement decision. It does not create the header, C++ implementation, shader include, or any working Vertex Factory. It has not been applied or compiled in the user's Engine checkout.
