# Task: Fix Reflection plugin crash + batch-fix graph-less cooked `UMaterial` imports

## Part 0 — FIX FIRST: `EXCEPTION_ACCESS_VIOLATION` in `DeserializeTexturePlatformData`

Reproduced twice, same crash signature both times, different Unreal projects
(`HostProject`, `CustomRig55`), different memory addresses — consistent and
reproducible, not one-off corrupt-data noise. Treat this as a real bug and fix
before doing any of the graph-reconstruction work below (a crash blocks batch
processing entirely; the graph-fallback work is pointless if imports keep
crashing on textures first).

**Crash:** `EXCEPTION_ACCESS_VIOLATION writing address 0x...` (round-looking
addresses both times — `0x000001f97d200000`, `0x000001c502200000` — worth noting,
may indicate a write through an uninitialized/garbage pointer rather than a
legitimate-but-overrun buffer).

**KEY FINDING from full editor log (not just the crash dump) — this narrows the
bug significantly:**

The full `LogReflection`/`LogJson`/`LogStreaming` output leading up to the crash
shows this crash is NOT purely a texture-format issue — it's specifically about
how the importer handles a texture reference whose target package is missing
on disk. Four `MaterialInstanceConstant` imports immediately before the crash
hit the *exact same* "package doesn't exist on disk" condition and succeed
cleanly:

```
LogStreaming: SkipPackage: <texture> - package does not exist on disk or in the loader
LogUObjectGlobals: Failed to find object '<texture>'
LogJson: Warning: Field Properties was not found
LogJson: Warning: Json Value of type 'Null' used as a 'Object'
LogReflection: Successfully reflected "MI_WhiteFox_Ear" as "MaterialInstanceConstant"
```

That `Field Properties was not found` → `Null used as 'Object'` pair is a
**null-guard catching a fully-absent `Properties` object in the JSON** and
bailing out gracefully — the texture slot is left empty, the parent
`MaterialInstanceConstant` still imports successfully. This exact pattern
repeats 4 times with 4 different missing textures, all fine.

The crash happens on try #5, for `Scp-1471_face_color_3`. Same
`SkipPackage`/`Failed to find object` warnings fire (package genuinely missing
on disk) — **but the `Field Properties was not found` / `Null used as Object`
pair does NOT appear this time.** That means this texture's JSON export DOES
have a `Properties` object (unlike the 4 prior cases where it was fully
`null`) — so the existing null-guard doesn't trigger, and the importer proceeds
into actually constructing the texture from whatever is in that `Properties`
object. It then crashes in `DeserializeTexturePlatformData`.

**Revised hypothesis:** the bug isn't "wrong pixel format" or "threading" in
general — it's specifically that the null-guard only handles the
*fully-absent-`Properties`* case. When `Properties` exists but is
incomplete/malformed (e.g. missing `PlatformData`, missing mip array, or a mip
entry with a null/empty `BulkData` payload — likely because the exporter
itself couldn't fully serialize this particular texture, possibly for the same
reason its package doesn't resolve on disk), the code has no equivalent guard
and proceeds to write mip data that was never actually populated.

**CONFIRMED root cause (from the actual crashing texture's JSON export,
`Scp-1471_face_color_3`):**

This texture's export has full metadata — `Properties` block populated,
`PixelFormat: PF_BC6H`, correct `SizeX`/`SizeY`, and a complete 11-entry `Mips`
array with correct per-mip dimensions and `BulkData` descriptors
(`ElementCount`, `SizeOnDisk`, `OffsetInFile`, `BulkDataFlags`) — all
plausible, correctly-sized numbers (e.g. 4,194,304 bytes is exactly right for
a 2048×2048 BC6H top mip). **But not a single mip has an actual pixel-byte
payload field.** No `"Data": "<base64>"` anywhere, for any of the 11 mips.

- Top 5 mips are flagged `BULKDATA_PayloadInSeperateFile` — bytes are meant to
  live in a companion `.ubulk` file that this export apparently doesn't
  include/resolve.
- Bottom 6 mips are flagged `BULKDATA_ForceInlinePayload`/`SingleUse` — meant
  to have bytes inline in this same JSON — but they don't either.

So the null-guard correctly passes this texture through (Properties genuinely
isn't null/absent), and `DeserializeTexturePlatformData` then does the
reasonable-looking thing: computes buffer sizes from the — real, correct —
`ElementCount`/`SizeOnDisk` values, allocates accordingly, and copies from a
payload that was never actually populated by the exporter. That's the write
violation.

**This is an export-completeness bug, not a texture-format or threading bug.**
Confirms the fix target precisely — no further guessing needed before writing
code.

**Full call path (real file paths, from the plugin source tree —
`Plugins/Reflection/Source/Reflection/`):**

```
Private/Importers/Constructor/ImportJob.cpp:227   TickJob()
Private/Importers/Constructor/ImportJob.cpp:179   StepJob()
Private/Importers/Constructor/ImportReader.cpp:137            IImportReader::ReadExportAndImport()
Private/Importers/Types/Materials/MaterialInstanceConstantImporter.cpp:33   IMaterialInstanceConstantImporter::Import()
Private/Serializers/ObjectSerializer.cpp:337       UObjectSerializer::DeserializeObjectProperties()
Private/Serializers/PropertySerializer.cpp:123      UPropertySerializer::DeserializePropertyValue()
Private/Serializers/PropertySerializer.cpp:448      UPropertySerializer::DeserializePropertyValue()  (nested)
Private/Serializers/PropertySerializer.cpp:630      UPropertySerializer::DeserializeStruct()
Private/Serializers/Structs/FallbackStructSerializer.cpp:80  FFallbackStructSerializer::Deserialize()
Private/Serializers/PropertySerializer.cpp:180      UPropertySerializer::DeserializePropertyValue()
Public/Importers/Constructor/Importer.h:212         IImporter::LoadExport<UObject>()
Public/Importers/Constructor/Importer.h:89          IImporter::DownloadWrapper<UObject>()
Private/Importers/Constructor/Asset.cpp:203         FAssetUtilities::ConstructAsset<UObject>()
Private/Importers/Constructor/Asset.cpp:319         FAssetUtilities::Construct_TypeTexture()
Private/Importers/Constructor/Asset.cpp:362         FAssetUtilities::Fast_Construct_TypeTexture()
Private/Importers/Types/Texture/TextureCreator.cpp:84    FTextureCreatorUtilities::CreateTexture<UTexture2D>()
Private/Importers/Types/Texture/TextureCreator.cpp:305   FTextureCreatorUtilities::DeserializeTexturePlatformData()  ← CRASH
```

Trigger path: importing a `MaterialInstanceConstant` whose properties reference
a texture (via `IImporter::LoadExport` pulling in a dependent `UTexture2D`
export) crashes while deserializing that texture's platform data (mip data).

**Tasks:**
- [ ] In `Private/Importers/Types/Texture/TextureCreator.cpp`, find where
      `DeserializeTexturePlatformData` reads each mip's `BulkData` object —
      specifically wherever it looks for the actual payload bytes (likely a
      `"Data"` field lookup, base64-decoded, or similar). Confirm this is indeed
      the gap: the code reads `ElementCount`/`SizeOnDisk` to size the
      allocation/copy, but does NOT check whether a payload field is actually
      present before copying from it.
- [ ] Add a presence check: before doing the size-based allocation/copy for a
      given mip, verify the payload data field actually exists and its length
      matches the expected `ElementCount`/`SizeOnDisk`. If missing or
      size-mismatched, skip that mip (and log a warning using the same style as
      the existing `Field Properties was not found` message) rather than
      proceeding to write.
- [ ] Decide the right granularity for the skip: per-mip (import the texture
      with fewer mips / a lower max resolution, using whichever mips DO have
      valid payload data) vs whole-texture (skip constructing this `UTexture2D`
      entirely and log a warning, leaving the reference unresolved — same
      graceful-skip behavior as the fully-null-Properties case). Recommend
      whole-texture skip for v1 (simpler, matches existing precedent, and a
      texture missing ALL mip payloads like this one isn't partially
      salvageable anyway) — revisit per-mip partial-recovery only if this turns
      out to be common with textures that have SOME valid mips.
- [ ] Double check: is this a systemic FModel export issue (i.e., "Save
      Properties" doesn't include bulk texture data for `PayloadInSeperateFile`
      mips by design, and needs a different export action to include `.ubulk`
      companions) rather than a one-off broken file? If so, this fix isn't just
      defensive — it's the *normal* case for however these JSONs are being
      produced, meaning most/all textures exported this way will hit this same
      gap. Worth confirming your FModel export method/settings before assuming
      this is rare.
- [ ] Re-run against both original crash reproductions (and ideally the batch
      of textures from the fox/wolf/SCP characters in the log) to confirm the
      fix resolves them without regressing the 4 already-working
      missing-package-with-null-Properties cases.
- [ ] If the fix is generally applicable (not game-specific), consider upstreaming
      to `JsonAsAsset/Reflection` — check their issue tracker first in case this is
      already a known/reported issue (`TextureReferenceIndex != INDEX_NONE` is a
      known one; this may be adjacent or the same underlying cause).

---

# Task: Batch-fix graph-less cooked `UMaterial` imports in Reflection

## Background

Reflection (fork of JsonAsAsset) imports Unreal assets from FModel-exported JSON.
Cooked `UMaterial` packages (flagged `PKG_FilterEditorOnly`) never serialize their
`Expressions` node graph — only compiled shader output survives
(`LoadedMaterialResources[].Content.MaterialCompilationOutput.UniformExpressionSet`).
`MaterialInstanceConstant` assets are unaffected (they never had a graph; parameter
overrides + `Parent` reference import fine as-is).

Today, importing a graph-less `UMaterial` silently produces a blank material shell
(no nodes), so any `MaterialInstanceConstant` built on top renders with no logic
even though its own parameter data is intact.

Goal: detect this case automatically and generate a usable fallback graph instead
of an empty one, across an arbitrary batch of exported JSON files.

## Scope

Two deliverables:
1. **Standalone batch scanner/preprocessor** (Python or Node — dev tooling, not
   shipped in the plugin) that walks a directory of FModel JSON exports, flags
   every `UMaterial` with missing/empty `Expressions`, and pre-generates a
   synthesized `Expressions` block for each one so Reflection's existing importer
   can consume it unmodified.
2. **C++ patch to Reflection's material importer** (proper long-term fix) that
   performs the same fallback generation in-engine at import time, so no
   preprocessing step is needed.

Do both — the scanner is useful immediately and for auditing; the C++ patch is
the durable fix. If forced to choose one first, do the scanner first (faster
feedback loop, no engine recompile needed to iterate).

## Part 1 — Batch scanner / preprocessor

- [ ] Input: a root directory of FModel JSON export files (recursive walk).
- [ ] For each file, parse top-level array; find entries where `"Type": "Material"`.
- [ ] Flag as "graph-less" if `Expressions` key is absent, `null`, or `[]`.
- [ ] For each flagged material, extract from
      `LoadedMaterialResources[0].Content.MaterialCompilationOutput.UniformExpressionSet`:
  - `UniformNumericParameters` → name, type (`Scalar`/`Vector`), default value
  - `UniformTextureParameters` → texture parameter slots (may be empty; cross-reference
    against any `MaterialInstanceConstant` in the batch whose `Parent` points at this
    material, using its `TextureParameterValues`/`ScalarParameterValues`/
    `VectorParameterValues` as the authoritative parameter list + names, since the
    instance often has better-named parameters than the raw uniform table)
  - `FunctionInfos` → referenced `MaterialFunction` paths (e.g. `MF_PhongToMetalRoughness`)
  - `UniformBufferLayoutInitializer` / `PropertyConnectedMask` → rough signal of which
    material output pins are actually driven (useful later for smarter wiring; skip
    for v1, just note it in output)
- [ ] Cross-reference pass: scan all `MaterialInstanceConstant` entries in the same
      batch for `Parent.ObjectPath` matching each graph-less material's path. Merge
      their parameter names/types/associations into the parameter list — this is the
      most reliable source of human-readable parameter names.
- [ ] Generate a synthesized `Expressions` array per flagged material:
  - One `MaterialExpressionTextureSampleParameter2D` per texture parameter
  - One `MaterialExpressionVectorParameter` per vector parameter
  - One `MaterialExpressionScalarParameter` per scalar parameter
  - If a `FunctionInfos` entry references a known function
    (start with `MF_PhongToMetalRoughness`, keep this a lookup table so more can be
    added later), emit a `MaterialExpressionMaterialFunctionCall` node pointed at it
  - Default wiring heuristic (v1, keep simple):
    - texture named/associated with "color"/"albedo"/"diffuse" → BaseColor
    - texture named/associated with "alpha"/"mask"/"opacity" → OpacityMask
      (only if `BasePropertyOverrides.BlendMode == BLEND_Masked` on any instance)
    - function call output → Metallic/Roughness if function name matches
      `*ToMetalRoughness*`
    - anything unmatched → left as an unconnected expression node in the graph
      (still visible/editable in the Material Editor, just not auto-wired)
  - Write each generated node with a stable `ExpressionGUID` derived from the
    parameter's existing `ExpressionGUID` in the instance data where available,
    so instance parameter matching still works after import
- [ ] Write output: either patch the `Expressions` key directly into a copy of the
      original JSON (`*.fixed.json`), or write a sidecar report — decide based on
      whether Reflection's importer reads `Expressions` in-place (likely wants
      patched JSON directly).
- [ ] CLI: `--input <dir> --output <dir> [--dry-run] [--report report.csv]`
      Report should list: material path, # params recovered, source
      (uniform-table-only vs instance-cross-referenced), functions referenced,
      confidence flag (e.g. "wired" vs "unwired-fallback").
- [ ] Unit test against the two sample files already in hand (`Material_018`,
      `M_Clothes_Dress`) to confirm expected output shape before running at scale.

## Part 2 — C++ patch to Reflection's importer (durable fix)

- [ ] Locate the material import path. Confirmed real files in the plugin tree
      (`Plugins/Reflection/Source/Reflection/`):
      - `Private/Importers/Types/Materials/MaterialInstanceConstantImporter.cpp`
        — importer for `MaterialInstanceConstant` (confirmed from crash trace)
      - Look for a sibling `Private/Importers/Types/Materials/MaterialImporter.cpp`
        or similarly named file handling plain `UMaterial` — this is where
        `Expressions` should be read and where the empty-graph case currently
        no-ops. Use the same `Private/Importers/Types/Materials/` directory as
        the starting point; grep for `"Expressions"` string literal to find the
        exact read site.
      - Shared construction helpers live in `Private/Importers/Constructor/Asset.cpp`
        (`FAssetUtilities::Construct_Type*` functions) — follow the same pattern
        used there for consistency with how textures are constructed.
- [ ] Add detection: after checking for `Expressions`, if absent/empty, branch into
      `BuildFallbackGraphFromCompiledOutput(...)` instead of returning early.
- [ ] Port the same parameter-extraction + cross-reference logic from Part 1 into
      C++ (reuse logic/tests from the scanner as the spec — don't reinvent the
      wiring heuristic, just reimplement it against Reflection's existing
      `UMaterialExpression*` construction helpers).
- [ ] Reflection almost certainly already has helper functions for creating
      `MaterialExpressionTextureSampleParameter2D` / `VectorParameter` /
      `ScalarParameter` nodes (used on the happy path when `Expressions` IS present)
      — reuse those rather than writing new node-construction code.
- [ ] Cross-reference against sibling `MaterialInstanceConstant` JSON files: needs
      a way to look these up at import time. Options:
      (a) accept a folder of related JSON as import context (batch import mode), or
      (b) fall back to raw `UniformNumericParameters` names only if no instance
      data is available in the current import batch — degrade gracefully rather
      than failing.
- [ ] Add a visible warning/log line on import (not a silent fallback) —
      e.g. "Material '%s' had no editor Expressions; generated approximate
      fallback graph from N parameters — review manually", matching the project's
      existing log conventions.
- [ ] Add an editor notification/toast (Reflection already does this for other
      import issues, e.g. `TextureReferenceIndex != INDEX_NONE`) so it's not
      buried in the log.
- [ ] Gate behind a plugin setting, e.g. `bGenerateFallbackGraphForGraphlessMaterials`
      (default true), so users who'd rather get the current blank-material behavior
      can opt out.

## Out of scope / explicitly not doing (v1)

- Shader bytecode decompilation (SPIR-V/DXBC → HLSL → node graph). Way more effort
  than the parameter-approximation approach above; revisit only if the approximate
  wiring proves insufficient in practice.
- Perfect visual parity with the original material. This is a "get something
  editable and roughly correct" fallback, not a lossless reconstruction — the
  original graph data is gone; can't get it back exactly.
- Function library beyond a small hardcoded lookup table (`MF_PhongToMetalRoughness`
  and whatever else shows up frequently in your batch) — expand the table
  iteratively as new function names are seen in scan reports rather than trying
  to handle every possible function up front.

## Suggested order of work

1. Scanner Part 1, tested against the two known sample files.
2. Run scanner across full export batch, review the CSV report — this tells you
   how common graph-less materials actually are and which functions/parameter
   patterns dominate, before investing in the C++ patch.
3. C++ patch (Part 2), informed by what the batch report shows.
4. Consider upstreaming Part 2 as a PR to JsonAsAsset/Reflection if the fallback
   proves useful — check their contribution guidelines first.
