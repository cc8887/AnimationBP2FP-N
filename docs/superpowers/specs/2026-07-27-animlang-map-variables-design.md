# AnimLang Map Variable Design

## Goal

Add strict, deterministic round-trip support for Unreal Blueprint Map member variables without making canonical AnimLang unnecessarily verbose.

The feature covers both parts of a Map variable:

1. The complete Unreal key and value pin types.
2. The authored default entries, when the Map is non-empty.

## Canonical Syntax

Map variables retain the existing variable form. The form head describes the key type, and `:container map` explicitly distinguishes the variable from a scalar, array, or set.

```lisp
(name
  :name "IKRetargeter_Map"
  :container map
  :value-pin-category "object"
  :value-type-object (asset "/Script/IKRig.IKRetargeter")
  :default [
    (entry
      :key "Manny"
      :value (asset "/Game/Retargeters/RTG_Manny.RTG_Manny"))
    (entry
      :key "Quinn"
      :value (asset "/Game/Retargeters/RTG_Quinn.RTG_Quinn"))
  ])
```

Canonical omission rules are:

- The form head implies `:pin-category`; the exporter omits the redundant field when they agree.
- `:pin-subcategory` is omitted when it is empty or `None`.
- `:container map` is always present.
- `:value-pin-category` is always present for a Map.
- `:value-pin-subcategory` is omitted when it is empty or `None`.
- `:type-object` and `:value-type-object` are emitted only when required to identify the key or value type exactly.
- `:default` is omitted for an empty Map and emitted for a non-empty Map.

The parser continues to accept explicit redundant key fields and `None` subcategories. Re-export canonicalizes them using the omission rules above.

## AST Model

`FVariableDef` gains the value-terminal half of `FEdGraphPinType`:

- `ValuePinCategory`
- `ValuePinSubCategory`
- `ValueTypeObjectPath`

It also gains `TArray<FMapEntryDef> MapEntries`. Each `FMapEntryDef` stores `KeyExpression` and `ValueExpression` in the DSL value representation rather than concatenating an opaque Map string. The array order is canonical output order, not Unreal `TMap` iteration order.

Non-Map variables must not carry value-terminal fields or Map entries. An empty Map has complete type metadata and an empty entry array.

## Parsing And Validation

The parser recognizes the new Map-only keywords and the structured default list:

```lisp
:default [(entry :key <value> :value <value>) ...]
```

Strict validation rejects:

- `:container map` without `:value-pin-category`.
- Value-terminal fields on a non-Map variable.
- `:default` entries on a non-Map variable.
- An `entry` missing `:key` or `:value`.
- Duplicate keys after canonical typed conversion.
- A key or value that cannot be imported by its declared Unreal property type.
- A required key or value type object that cannot be loaded.

Legacy scalar defaults retain the existing positional representation. The new keyword `:default` is reserved for structured Map entries in this feature.

## Export

For each Blueprint member variable, the exporter copies the primary pin type as it does today. For a Map it additionally copies `PinValueType.TerminalCategory`, `TerminalSubCategory`, and `TerminalSubCategoryObject` into the AST.

Default entries are read from the compiled generated-class default object through the reflected `FMapProperty`. Keys and values are converted with their owning `FProperty`, then represented as typed DSL values:

- Booleans and numeric values use Lisp atoms.
- String-like and name-like values use quoted strings.
- Object/class references use `(asset "...")`.
- Null object/class references use `nil`.
- Enum values use quoted Unreal enum export text.
- Struct and any otherwise unsupported-but-losslessly-exportable property value use `(ue-value "<escaped Unreal property text>")`.

`ue-value` is deliberately an escape hatch, not a type declaration. Its text is interpreted only through the already-declared key or value `FProperty`; it is never accepted where the declared property cannot import the complete text. The exporter escapes quotes, backslashes, and control characters with the existing DSL string escaping rules.

The exporter sorts entries by canonical serialized key and then canonical serialized value. This makes repeated exports byte-stable even though `TMap` iteration order is not stable.

If the Blueprint has no compiled generated class or the reflected Map property cannot be resolved, strict export reports `[UNSUPPORTED:VariableDefault]` rather than silently emitting an empty default.

## Import

`TryBuildVariablePinType` reconstructs both halves of `FEdGraphPinType`. It sets `ContainerType` to `Map` only from explicit `:container map`; it does not infer the container from value fields.

Map variables use a two-stage build because their reflected `FMapProperty` does not exist before Blueprint compilation:

1. Create all variables with complete `FEdGraphPinType` data and no Map default text.
2. Run a skeleton-only compile to materialize the generated properties.
3. Resolve each `FMapProperty`, validate every structured entry with its key/value `FProperty`, and build Unreal's canonical Map default text.
4. Assign that text to the matching `FBPVariableDescription::DefaultValue`.
5. Continue through the existing full compile and strict canonical re-export gate.

The extra skeleton compile is required only when at least one Map has non-empty `MapEntries`. Empty typed Maps remain on the existing compile path.

Import fails atomically if any entry cannot be represented. It must not create the variable with an empty Map as a fallback.

## Diff And Patch

Variable equality includes the three value-terminal type fields and the canonical Map entries. A key type change, value type change, entry addition/removal, or value change produces a variable modification.

The patcher uses the same pin-type builder and Map-default converter as full import. Shared helpers must live in the runtime module so full import and patch update cannot diverge.

## Compatibility

- Existing scalar, array, and set syntax remains valid.
- Existing explicit `:pin-category` and `None` subcategories remain parseable.
- A legacy DSL Map without value-terminal type data remains invalid because it cannot be reconstructed exactly.
- There is no inference from a source asset during import; AnimLang remains self-contained.
- Map-of-container nesting is outside scope because Blueprint pin containers do not provide a general nested-container contract.

## Tests

Tests follow red-green development and cover:

1. Parser acceptance and canonical omission for an empty typed Map.
2. Parser rejection of missing value type, misplaced value fields, malformed entries, and duplicate keys.
3. AST serialization and parse round-trip for primitive, object, enum, and struct key/value metadata.
4. Export of a transient compiled Anim Blueprint with non-empty Map defaults.
5. Import and strict re-export equality, including exact key/value type objects and entries.
6. Deterministic output when reflected Map iteration order differs.
7. Differ and patcher detection/application of Map type and entry changes.
8. Regression coverage for existing scalar, array, and set variables.

The completion gate is the focused Map-variable automation suite, the full AnimBP2FP automation suite, a complete project Editor-target build, strict export of `Face_AnimBP` and `ABP_GenericRetarget`, strict transient round-trip, and a cold editor/commandlet load without related warnings or errors.

## Out Of Scope

- Runtime mutation semantics for Maps after Blueprint initialization.
- A general-purpose Lisp hash-table runtime.
- Nested Map/Array/Set containers.
- Recovering omitted non-empty defaults from the original source asset.
- Changing the established variable form to a new `(map ...)` head.
