# Property Access: copies and binds

A short guide to the UE5 "property access" data inside an exported anim
blueprint: where it lives, how a copy is read, what a bind is, and how the two
relate. Written for the `AB_CharacterUnit` export but the layout is the same
for any UE5.7 anim blueprint.

## Where the data lives

In the `AnimBlueprintGeneratedClass` export:

```
export[0].SerializedSparseClassData.AnimBlueprintExtension_PropertyAccess
  .Library
    .PathSegments          [flat list of path segment names, indexed by paths]
    .SrcPaths              [57 entries - source property paths]
    .DestPaths             [57 entries - destination property paths]
    .CopyBatchArray[0]
      .Copies              [57 entries - the compiled copies]
```

Each `SrcPaths[i]` / `DestPaths[i]` is an object with
`PathSegmentStartIndex` + `PathSegmentCount` that slices a contiguous range out
of `PathSegments`. Joining the sliced names with `>` gives a readable path.

Each `Copies[i]` has:

| field                  | meaning                                              |
| ---------------------- | ---------------------------------------------------- |
| `AccessIndex`          | index into `SrcPaths` (the source path)              |
| `DestAccessStartIndex` | index into `DestPaths` (the destination path)        |
| `Type`                 | copy kind (`Object`, `Plain`, `Bool`, `DemoteDoubleToFloat`, ...) |

In this export `AccessIndex == i` and `DestAccessStartIndex == i`, so `Copies[i]`
is exactly the pair `SrcPaths[i] -> DestPaths[i]` (1:1, no reordering).

## What a copy is

A **copy** is the runtime data flow the engine compiles from a node pin binding:
at evaluation time the value of the source (an anim instance variable) is copied
into the destination (a property on an anim node instance).

Example (`AB_CharacterUnit`):

```
Copy 13: Facial_Angry  ->  AnimGraphNode_SequencePlayer_49>Sequence
Copy 29: Height        ->  AnimGraphNode_TwoWayBlend_6>Alpha
Copy 37: Height        ->  AnimGraphNode_ModifyBone_13>Alpha
```

## What a bind is

A **bind** is the editor-side pin wiring that produces those copies: a node's
pin (e.g. `Sequence`) is bound to a variable (e.g. `Facial_Angry`). In the
editor this is the small icon on the pin; the blueprint compiler turns every
bind into a copy in the property access `Library`.

So the same `SrcPaths[i]/DestPaths[i]` pair describes both:

- editor view -> "pin `Sequence` on node `_49` is bound to variable `Facial_Angry`"
- compiled view -> "copy `Facial_Angry` into `AnimGraphNode_SequencePlayer_49>Sequence`"

## How to find which copy feeds which node

Two equivalent ways, both land on the same copies:

1. **Destination matching (simplest).** Resolve every `DestPaths[i]`, keep the
   ones shaped `AnimGraphNode_SequencePlayer[_N]>Sequence` (or any
   `AnimGraphNode...>Pin`). That path already contains the node name and pin.

2. **Per-node CopyRecords.** Each sparse node carries its own compiled records:
   `SerializedSparseClassData.AnimGraphNode_SequencePlayer_57.CopyRecords[0].CopyIndex`
   = 6, i.e. that node consumes `Copies[6]`. Node `_53` has `CopyRecords: []` and
   a non-null `Sequence` - it is not bound.

The source of a copy is the **leaf** of its source path: `Facial_Angry` for
`Facial_Angry`, `Height` for `SomeStruct>Height`. The leaf is the anim instance
variable name.

## The 57 copies in AB_CharacterUnit

| group                            | count | destination shape                              |
| -------------------------------- | ----- | ---------------------------------------------- |
| Facial / idle sequence bindings  | 27    | `AnimGraphNode_SequencePlayer[_N]>Sequence`    |
| Height (TwoWayBlend alpha)       | 3     | `AnimGraphNode_TwoWayBlend[_N]>Alpha`          |
| Height / WombFillRate (ModifyBone)| 13   | `AnimGraphNode_ModifyBone[_N]>Alpha`           |
| Internal plumbing                 | 14    | `__AnimBlueprintMutables>...`, `__CustomProperty...` |

The 27 `>Sequence` copies match exactly the 27 SequencePlayer nodes whose CDO
`Sequence` is serialized as `null` (the bindings are the runtime source of those
sequences, so the stored default is empty).

## Sequence-player default recovery (removed)

At one point the importer walked these `>Sequence` copies and, for every node
whose CDO `Sequence` was null, filled it from the source leaf's object-reference
default (e.g. `Facial_Angry -> AnimSequence'Facial_WolfMorph_Angry'`), with two
synthesized defaults for variables that have no CDO default in the export:

- `Facial Chew`       -> `/Game/SexScene/Facial_Animation/WolfMorph/Facial_Wolf_Chew.0`
- `Facial Open Mouth` -> `/Game/SexScene/Facial_Animation/WolfMorph/Facial_Wolf_OpenMouth.0`

Both systems (the value copy into the CDO and the node-pin binding writer) were
fully removed to isolate an import crash. This file documents how to re-add or
reproduce them from the JSON alone.
