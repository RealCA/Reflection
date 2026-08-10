# Discovery: why animation import requires manual Content Browser selection

## Symptom

Batch-importing a folder of JSON exports works automatically for textures and
materials (creates assets directly, no manual steps). For animations
(`AnimSequence`/similar), the plugin instead pops up:

> "Select a Animation inside of the Content Browser to reflect data."

...meaning it appears to require a pre-existing animation asset to already be
selected in the Content Browser before it will apply/"reflect" imported data
onto it, rather than creating the asset fresh like it does for other types.
This breaks unattended batch folder-import for any JSON that includes
animations.

## Why this might be expected (not a bug) — needs confirming, not assuming

`UAnimSequence` (and related animation asset types) are bound to a `USkeleton`
at creation time — bone-indexed keyframe/curve data only makes sense relative
to a specific skeleton's bone hierarchy, unlike a `UTexture2D` or
`UMaterialInstanceConstant`, which can be constructed standalone. So it's
plausible the current importer just doesn't have an auto-create path that
resolves the skeleton reference itself, and instead offloads that step to the
user via manual selection.

BUT: `MaterialInstanceConstant` already auto-resolves its own external
reference (`Parent` → the base `Material`) during import without user
interaction, using whatever asset resolution Reflection already has for
cross-references. If the animation JSON also contains a resolvable skeleton
reference (need to check — see below), there's no obvious reason the same
resolution mechanism couldn't apply here too. This needs investigating in the
actual source, not assuming either way.

## Questions for opencode to answer by reading the actual source

**1. Does the animation JSON export contain a skeleton reference?**
- Pull a sample animation export (ask user for one if not already available)
  and check: is there a `Skeleton` property (analogous to `Parent` on
  `MaterialInstanceConstant`) pointing at an object path, e.g.
  `/Game/.../SomeSkeleton.SomeSkeleton`?
- If yes — confirm it's resolvable the same way `Parent` is for materials
  (i.e. points at an asset that can/should already exist in the target
  project, same resolution mechanism as other cross-references).

**2. Find the animation importer code path**
- Locate the equivalent of `MaterialInstanceConstantImporter.cpp` for
  animations — expected somewhere like
  `Private/Importers/Types/Animation*/...` or similar under
  `Private/Importers/Types/`. Confirm exact file/class name.
- Find where the "Select a Animation inside of the Content Browser to reflect
  data" message is triggered (search for that string literal). What condition
  causes it to fire — is it unconditional for this asset type (always requires
  manual selection), or only in some fallback case (e.g. skeleton reference in
  JSON not found/resolvable)?

**3. Compare against the working `MaterialInstanceConstant` auto-resolve path**
- In `MaterialInstanceConstantImporter.cpp`, find where `Parent` gets resolved
  to an actual `UMaterial*` reference automatically. What
  utility/function does this call (expected something in
  `FAssetUtilities` or similar, given the pattern from `Asset.cpp` in the
  texture/material work)?
- Is that same utility reusable for resolving a `Skeleton` reference on an
  animation import, or is it material-type-specific?

**4. Check whether asset *creation* (not just reference resolution) is the
   actual blocker**
- Does the current animation importer only support "reflect onto existing
  asset" (update/populate an asset that already exists), with no code path
  for creating a new `AnimSequence` from scratch at all? Or does asset
  creation exist but just isn't wired up to auto-run for this type yet?
- If creation-from-scratch genuinely doesn't exist in the importer at all,
  that's a bigger lift (need to call whatever factory Unreal uses to create a
  new `UAnimSequence` bound to a resolved `USkeleton`, akin to
  `UAnimSequenceFactory` in editor code) than if it exists but just isn't
  triggered automatically.

## Output format requested from opencode

Short answer to each question with file/line references, plus a clear verdict:
**(a)** this is a straightforward wiring gap — skeleton reference is
resolvable, asset-creation code already exists, just needs auto-triggering
like materials do — low effort; or
**(b)** asset creation from scratch doesn't exist for this type yet and needs
building — higher effort, needs its own spec once confirmed.

Once we know which, we'll write the actual implementation task list.

---

## Findings (from source, verified 2026-08-03)

**Verdict: (a) — straightforward wiring gap, low effort.** Fix implemented (see
below). The animation importer is the *only* registered type importer with no
`CreateAsset` override; the skeleton reference is resolvable through the exact
same generic machinery materials use for `Parent`.

**1. Skeleton reference in the JSON — yes, resolvable.**
- `ReadAnimationData` (Public/Modules/Cloud/Tools/AnimationData.h:55-64)
  deserializes properties via
  `DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), {...}), ...)`.
  The removal list (`NumFrames`, `TrackToSkeletonMapTable`, `SequenceLength`,
  `SkeletonGuid`, `CompressedTrackToSkeletonMapTable`, `CompressedDataStructure`,
  `CompressedRawDataSize`, `RawCurveData`) does NOT include `Skeleton`, so the
  `Skeleton` object property is deserialized through the generic object
  property resolver.
- The resolver: `UPropertySerializer::DeserializePropertyValue` →
  `FObjectPropertyBase` branch (Private/Serializers/PropertySerializer.cpp:164-311)
  → `Importer->LoadExport(&JsonValueAsObject, Object)` → generic
  `LoadObjectByPath<T>` → in-container export fallback → Cloud download.
  Identical path to how `Parent` is resolved on a `UMaterialInstanceConstant`.
- `ISkeletonImporter` exists and creates skeletons, so the referenced skeleton
  asset is present in the project when animations are batch-imported after
  skeletons.
- Logical proof it works today: with a manual selection, `GetSkeleton()` at
  AnimationData.h:66 only succeeds if the JSON's `Skeleton` reference resolves
  during `DeserializeObjectProperties` (the selected asset isn't the source of
  the skeleton — it's deserialized from JSON). Since manual-selection imports
  succeed, the JSON carries a resolvable `Skeleton`. Caveat: no sample export
  JSON is checked into the repo (fixtures only cover materials), and an
  unresolvable skeleton fails gracefully at AnimationData.h:68-71 with
  "Could not get valid Skeleton".

**2. The importer code path — condition is UNCONDITIONAL.**
- `IAnimationBaseImporter::Import()` (Private/Importers/Types/Animation/
  AnimationBaseImporter.cpp:18-19, originally) called
  `ReadAnimationData(this, true, this)` — hardcoded `UseSelectedAsset=true`.
- With `UseSelectedAsset=true`, `ReadAnimationData` (AnimationData.h:16) calls
  `GetSelectedAsset<UAnimSequenceBase>(true, Container->GetAssetName())`
  (Public/Utilities/ContentBrowser.h:27), which reads the Content Browser
  selection. When nothing is selected, the dialog
  `"Select a Animation inside of the Content Browser to reflect data."`
  fires at AnimationData.h:31.
- This is not a fallback path — it is the only wired mode for the batch
  importer. The `UseSelectedAsset=false` branch (AnimationData.h:18-21, uses
  `Container->GetAsset()`) was unreachable because no code path ever set
  `Container->GetAsset()` for animations.

**3. MIC comparison — utility is generic and reusable.**
- `IMaterialInstanceConstantImporter::Import()`
  (Private/Importers/Types/Materials/MaterialInstanceConstantImporter.cpp:14-36):
  `Create<UMaterialInstanceConstant>()` → `CreateAsset` override
  (lines 10-12, `NewObject<UMaterialInstanceConstant>`) → sets
  `AssetExport->Object` via base `IImporter::CreateAsset`
  (Private/Importers/Constructor/Importer.cpp:14-22) → then
  `DeserializeObjectProperties`, which resolves `Parent` through the generic
  object-property resolver above.
- `GetAsset()` returns `AssetExport->Object`
  (Private/Serializers/SerializerContainer.cpp:99-105), so the
  `UseSelectedAsset=false` branch in `ReadAnimationData` picks up whatever
  `CreateAsset` created.
- Resolution is fully generic (`LoadExport<T>`/`LoadObjectByPath<T>`/
  `ConstructAsset<T>` are templated in Public/Importers/Constructor/Importer.h:121-228).
  A `Skeleton` (USkeleton) reference resolves identically to `Parent`. Only
  caveat: `ConstructAsset<USkeleton>` has no explicit template instantiation
  (Private/Importers/Constructor/Asset.cpp:152) — only matters for the
  Cloud-download fallback when the skeleton asset is missing locally; local
  path resolution works fine.

**4. Asset creation is the blocker, but it's a wiring gap, not missing plumbing.**
- The montage inline creation at AnimationData.h:24-26
  (`NewObject<UAnimMontage>`) exists but was unreachable from the importer
  path (only runs when `!AnimSequenceBase`, i.e. only under
  `UseSelectedAsset=false`, which the importer never used).
- `IAnimationBaseImporter` is the ONLY registered type importer without a
  `CreateAsset` override. Every other type (MIC, DataTable, Skeleton, PoseAsset,
  SoundCue, ParticleSystem, etc.) creates its asset via `NewObject` in
  `CreateAsset`. `IAnimationBaseImporter` neither overrode it nor called
  `Create`/`CreateAsset` in `Import()`, so `Container->GetAsset()` was never set.
- The skeleton-bound `UAnimSequence` creation pattern already exists in the
  codebase: `IPoseAssetImporter::CreateAnimSequenceFromPose`
  (Private/Importers/Types/Animation/PoseAssetImporter.cpp:184-185),
  `NewObject<UAnimSequence>(...)` + `SetSkeleton(Skeleton)`.

## Fix implemented (2026-08-03)

`IAnimationBaseImporter` now mirrors the MIC pattern:

- **Public/Importers/Types/Animation/AnimationBaseImporter.h** — added
  `virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;`
- **Private/Importers/Types/Animation/AnimationBaseImporter.cpp** — added
  `CreateAsset` override (`NewObject<UAnimMontage>` when the asset class is a
  montage, else `NewObject<UAnimSequence>`, `RF_Public | RF_Standalone` in
  `GetPackage()`), and changed `Import()` to `CreateAsset(nullptr);` then
  `ReadAnimationData(this, false, this);`.

Result: batch animation imports create the `AnimSequence`/`AnimMontage` fresh
(like materials), the skeleton resolves via the generic object-property
machinery, and the "Select a Animation..." dialog no longer fires during batch
import. An unresolvable skeleton still fails gracefully (log + per-asset
failure) instead of blocking the whole batch with a modal dialog.

### Verification status
- Compile: PASS (no errors; only pre-existing deprecation warnings).
- Link/editor test: BLOCKED — editor running (PID 30704) holds
  `Plugins\Reflection\Binaries\Win64\UnrealEditor-Reflection.dll`. Relink + hot
  reload after closing the editor (or via Live Coding), then re-test batch
  animation import.
