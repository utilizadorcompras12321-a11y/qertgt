# Software RT patch application

The patch documents in `patches/` are not all executable. Only a real unified-diff file ending in `.patch` is accepted by the helper. Files ending in `.md` are design notes or instructions and must not be passed to `git apply`.

## Requirements

- Windows PowerShell 5.1 or newer.
- Git for Windows available as `git` on PATH.
- A source-built Unreal Engine 4.27.x directory.
- The patch file downloaded from this repository.

## 1. Dry run (default)

Example using the user's currently documented Engine location:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Apply-SoftwareRTPatch.ps1 `
  -EngineRoot 'C:\Users\Utilizador~\Desktop\UnrealEngine' `
  -PatchFile 'C:\Temp\04-software-rt-scene-snapshot.patch'
```

The script verifies the Engine source-tree layout, reads `Engine\Build\Build.version`, checks major/minor version 4.27, validates the unified diff with `git apply --check`, rejects unsafe paths, and prints every target as MODIFY or CREATE. The default run changes nothing.

## 2. Apply

Only after reviewing the dry-run target list:

```powershell
.\Apply-SoftwareRTPatch.ps1 `
  -EngineRoot 'C:\Users\Utilizador~\Desktop\UnrealEngine' `
  -PatchFile 'C:\Temp\04-software-rt-scene-snapshot.patch' `
  -Apply
```

Before applying, existing target files are copied under:

```
<EngineRoot>\.software-rt-patch-backups\<timestamp>\
```

A `manifest.txt` records which files were backed up and which were new. The script then applies the patch with Git.

## Experimental Surface Cache capture patch order

Apply these executable patches one at a time, in this exact order:

`50 → 53 → 54 → 55 → 56 → 57 → 58 → 59 → 60 → 61 → 62`

The capture path is disabled by default. After applying and compiling the Engine, enable it with:

```text
r.SoftwareRT.MaterialCapture 1
```

Initial limitations: only opted-in opaque, one-sided materials; CPU-readable Static Mesh LOD data; no Instanced/HISM meshes; one view-selected LOD; 1024×1024 atlas with a 16-pixel triangle tile (maximum 4096 charts). This is an experimental capture proof, not a production cache: chart dilation, material/view-expression filtering, revision invalidation, and GI/shadow/reflection consumers are still outstanding.


## Important limitations

- This is a guarded patch applier, not an automatic patch downloader or patch-sequencing system.
- It deliberately accepts only executable `.patch` unified diffs. It will reject the current Markdown patch/design documents.
- It checks UE 4.27 major/minor, not the exact Epic 4.27.2 source changelist. A patch still needs source/API review against the exact Engine checkout.
- `git apply --check` verifies patch context, not C++ correctness. It does not compile or test the Engine.
- Apply one patch at a time, in the documented dependency order. Do not batch-apply the whole series.
- Do not use `-Apply` until the displayed Engine root and every target path have been checked.

The script does not assume that the GitHub repository is the Engine checkout. It operates only on the explicit `-EngineRoot` supplied on the command line.
