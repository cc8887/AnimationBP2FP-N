# AnimLang Map Variables Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add exact, deterministic AnimLang export/import/diff/patch support for Blueprint Map variables, including key/value pin types and non-empty default entries.

**Architecture:** Extend `FVariableDef` with value-terminal metadata and structured Map entries. Centralize Unreal pin-type and reflected Map-value conversion in `FAnimLangVariableCodec`, then use it from exporter, full importer, and patcher. Non-empty Map defaults use a skeleton-compile phase to materialize `FMapProperty`, followed by canonical default generation and the existing full compile/re-export gate.

**Tech Stack:** Unreal Engine 5.9 C++, `FEdGraphPinType`, `FMapProperty`, `FScriptMapHelper`, AnimLang tokenizer/parser/AST, Unreal Automation Tests, UnrealBuildTool.

---

## File Map

- Modify `Source/AnimBP2FP/Public/AnimLangAST.h`: add `FMapEntryDef` and Map value-terminal fields.
- Modify `Source/AnimBP2FP/Private/AnimLangAST.cpp`: canonical Map variable serialization and omission rules.
- Modify `Source/AnimBP2FP/Public/AnimLangParser.h`: declare Map-default parsing/validation helpers.
- Modify `Source/AnimBP2FP/Private/AnimLangParser.cpp`: parse Map metadata and structured entries.
- Create `Source/AnimBP2FP/Public/AnimLangVariableCodec.h`: shared pin-type/default conversion API.
- Create `Source/AnimBP2FP/Private/AnimLangVariableCodec.cpp`: reflected Map encoding/decoding and canonical ordering.
- Modify `Source/AnimBP2FP/Private/AnimBPExporter.cpp`: export Map type and CDO entries.
- Modify `Source/AnimBP2FP/Private/AnimBPImporter.cpp`: shared pin builder and two-stage Map defaults.
- Modify `Source/AnimBP2FP/Private/AnimLangDiffer.cpp`: compare value-terminal metadata and entries.
- Modify `Source/AnimBP2FP/Private/AnimLangPatcher.cpp`: use shared codec and compile/apply Map defaults.
- Modify `Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp`: parser, codec, export, import, diff, and deterministic regression tests.
- Modify `README.md`: document canonical Map syntax.

## Common Commands

Set these PowerShell variables once per shell:

```powershell
$Project = 'F:\GASP\GASP.uproject'
$EditorCmd = 'D:\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$TaskCodexHome = if ($env:CODEX_HOME) {
  $env:CODEX_HOME
} else {
  Join-Path $env:USERPROFILE '.codex'
}
$SkillRoot = Join-Path $TaskCodexHome 'skills\ue-diagnosing-plugin-build-load'
$BuildScript = Join-Path $SkillRoot 'scripts\build-ue-editor-with-plugins.ps1'
```

Focused test command:

```powershell
& $EditorCmd $Project -unattended -nop4 -nosplash -nullrhi `
  '-ExecCmds=Automation RunTests AnimBP2FP.VariableTypes; Quit' `
  '-TestExit=Automation Test Queue Empty' `
  '-log=F:\GASP\Saved\Logs\AnimBP2FP-MapVariables.log'
```

Build command for this workspace's explicitly accepted shared-plugin layout:

```powershell
& $BuildScript -Project $Project -EngineRoot 'D:\UnrealEngine' -AllowSharedPluginOutputs
```

### Task 1: AST And Canonical Parser Syntax

**Files:**
- Modify: `Source/AnimBP2FP/Public/AnimLangAST.h`
- Modify: `Source/AnimBP2FP/Private/AnimLangAST.cpp`
- Modify: `Source/AnimBP2FP/Public/AnimLangParser.h`
- Modify: `Source/AnimBP2FP/Private/AnimLangParser.cpp`
- Test: `Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp`

- [ ] **Step 1: Replace the old Map-failure test with failing canonical syntax tests**

Add tests that parse and reserialize:

```cpp
const FString Source = TEXT(R"ANIM(
(anim-blueprint "ABP_MapSyntax"
  :variables [
    (name :name "Values" :container map
      :value-pin-category "int"
      :default [
        (entry :key "Run" :value 2)
        (entry :key "Idle" :value 1)
      ])
  ]
  :anim-graph (identity-pose))
)ANIM");
```

Assert that parsing yields one Map with `ValuePinCategory == "int"`, two entries, and canonical output which omits `:pin-category`, both `None` subcategories, and emits entries sorted by key. Add negative fixtures for missing `:value-pin-category`, value fields on a scalar, missing entry key/value, and duplicate keys.

- [ ] **Step 2: Run the focused suite and verify RED**

Run the focused command from Common Commands. Expected: compile failure because `FVariableDef` has no value-terminal fields/Map entries, or test failures because the parser does not recognize the syntax.

- [ ] **Step 3: Add the AST model**

Add before `FVariableDef`:

```cpp
struct ANIMBP2FP_API FMapEntryDef
{
    FString KeyExpression;
    FString ValueExpression;

    bool operator==(const FMapEntryDef& Other) const
    {
        return KeyExpression == Other.KeyExpression
            && ValueExpression == Other.ValueExpression;
    }
};
```

Add to `FVariableDef`:

```cpp
FString ValuePinCategory;
FString ValuePinSubCategory;
FString ValueTypeObjectPath;
TArray<FMapEntryDef> MapEntries;
```

- [ ] **Step 4: Serialize the canonical Map form**

Update `FVariableDef::ToString()` so the form head implies the key category, `None` subcategories are omitted, `:container map` and `:value-pin-category` are explicit, optional type objects use `(asset ...)`, and non-empty entries serialize as:

```cpp
Result += TEXT(" :default [");
for (const FMapEntryDef& Entry : MapEntries)
{
    Result += FString::Printf(TEXT(" (entry :key %s :value %s)"),
        *Entry.KeyExpression, *Entry.ValueExpression);
}
Result += TEXT(" ]");
```

Keep positional `DefaultValue` unchanged for non-Map variables.

- [ ] **Step 5: Parse and validate Map fields**

Add parser helpers:

```cpp
bool ParseMapDefault(FVariableDef& Var);
bool ParseMapEntry(FMapEntryDef& OutEntry);
void ValidateVariableDefinition(const FVariableDef& Var);
```

Recognize `value-pin-category`, `value-pin-subcategory`, `value-type-object`, and `default`. `ParseMapDefault` must require `[`...`]`, require `(entry :key Value :value Value)`, preserve each raw value expression through `ParseRawExpressionText`, and reject duplicate raw canonical keys. Call `ValidateVariableDefinition` before returning `ParseVarDef`.

- [ ] **Step 6: Run focused tests and verify GREEN**

Run the focused command. Expected: all syntax/validation tests pass. Keep the pre-existing test which expects Map export rejection unchanged until Task 3, so the suite remains green between tasks.

- [ ] **Step 7: Commit the syntax slice**

```powershell
git add Source/AnimBP2FP/Public/AnimLangAST.h `
  Source/AnimBP2FP/Private/AnimLangAST.cpp `
  Source/AnimBP2FP/Public/AnimLangParser.h `
  Source/AnimBP2FP/Private/AnimLangParser.cpp `
  Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp
git commit -m "Add AnimLang map variable syntax"
```

### Task 2: Shared Pin-Type And Map-Value Codec

**Files:**
- Create: `Source/AnimBP2FP/Public/AnimLangVariableCodec.h`
- Create: `Source/AnimBP2FP/Private/AnimLangVariableCodec.cpp`
- Modify: `Source/AnimBP2FP/Private/AnimBPImporter.cpp`
- Modify: `Source/AnimBP2FP/Private/AnimLangPatcher.cpp`
- Test: `Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp`

- [ ] **Step 1: Write failing codec tests**

Test the wished-for API with a `name -> object` Map and a `name -> int` Map:

```cpp
FEdGraphPinType PinType;
FString Error;
TestTrue(TEXT("typed map pin builds"),
    FAnimLangVariableCodec::BuildPinType(Variable, PinType, Error));
TestEqual(TEXT("map container"), PinType.ContainerType, EPinContainerType::Map);
TestEqual(TEXT("value category"),
    PinType.PinValueType.TerminalCategory, UEdGraphSchema_K2::PC_Object);
TestEqual(TEXT("value object"),
    PinType.PinValueType.TerminalSubCategoryObject.Get(), UObject::StaticClass());
```

Add failures for missing value category and an unloadable required value type object.

- [ ] **Step 2: Run focused tests and verify RED**

Expected: compile failure because `FAnimLangVariableCodec` does not exist.

- [ ] **Step 3: Add the shared codec API**

Create:

```cpp
class ANIMBP2FP_API FAnimLangVariableCodec
{
public:
    static bool BuildPinType(const FVariableDef& Variable,
        FEdGraphPinType& OutPinType, FString& OutError);

    static bool ExportMapEntries(const UAnimBlueprint& Blueprint,
        FVariableDef& InOutVariable, FString& OutError);

    static bool BuildMapDefaultText(const UAnimBlueprint& Blueprint,
        const FVariableDef& Variable, FString& OutDefaultText, FString& OutError);

private:
    static bool ImportPropertyExpression(const FProperty& Property,
        const FString& Expression, void* Value, FString& OutError);
    static FString ExportPropertyExpression(const FProperty& Property,
        const void* Value);
};
```

Guard editor-only Blueprint/reflection operations with `#if WITH_EDITOR`, while leaving `BuildPinType` available to existing editor consumers in the runtime module.

- [ ] **Step 4: Implement exact pin reconstruction**

Move the existing `TryBuildVariablePinType` logic into `BuildPinType`. Set the key/main type first, map `array`, `set`, and `map`, then for Map resolve:

```cpp
OutPinType.PinValueType.TerminalCategory = FName(*Variable.ValuePinCategory);
OutPinType.PinValueType.TerminalSubCategory =
    IsNoneText(Variable.ValuePinSubCategory)
        ? NAME_None : FName(*Variable.ValuePinSubCategory);
OutPinType.PinValueType.TerminalSubCategoryObject = ResolvedValueTypeObject;
```

Reject Map without a value category and reject value-terminal fields on non-Map variables.

- [ ] **Step 5: Implement reflected property expression conversion**

Use property-specific handling:

```cpp
if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
{
    if (Expression == TEXT("nil"))
    {
        ObjectProperty->SetObjectPropertyValue(Value, nullptr);
        return true;
    }
    const FString Path = UnwrapAssetExpression(Expression);
    UObject* Object = StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *Path);
    if (!Object)
    {
        OutError = FString::Printf(
            TEXT("Object '%s' could not be loaded as '%s'"),
            *Path, *ObjectProperty->PropertyClass->GetPathName());
        return false;
    }
    ObjectProperty->SetObjectPropertyValue(Value, Object);
    return true;
}
```

For primitive/name/string/enum values, unwrap DSL syntax and call `ImportText_Direct`. For `(ue-value "...")`, unescape the payload and require `ImportText_Direct` to consume the entire string. Export objects as `(asset ...)`, null as `nil`, primitives as atoms/quoted strings, and other lossless types as `(ue-value ...)`.

- [ ] **Step 6: Replace duplicate pin builders**

Make full importer and patcher call `FAnimLangVariableCodec::BuildPinType`; remove their local duplicate builders. Do not yet apply Map entries.

- [ ] **Step 7: Run focused tests and verify GREEN**

Expected: codec tests and all pre-existing scalar/array/set tests pass.

- [ ] **Step 8: Commit the codec slice**

```powershell
git add Source/AnimBP2FP/Public/AnimLangVariableCodec.h `
  Source/AnimBP2FP/Private/AnimLangVariableCodec.cpp `
  Source/AnimBP2FP/Private/AnimBPImporter.cpp `
  Source/AnimBP2FP/Private/AnimLangPatcher.cpp `
  Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp
git commit -m "Centralize AnimLang variable type conversion"
```

### Task 3: Reflected Map Default Export

**Files:**
- Modify: `Source/AnimBP2FP/Private/AnimLangVariableCodec.cpp`
- Modify: `Source/AnimBP2FP/Private/AnimBPExporter.cpp`
- Test: `Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp`

- [ ] **Step 1: Write a failing compiled Blueprint export test**

Create a transient Anim Blueprint with `FKismetEditorUtilities::CreateBlueprint(UAnimInstance::StaticClass(), GetTransientPackage(), TEXT("ABP_MapExportFixture"), BPTYPE_Normal, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(), FName(TEXT("AnimBP2FPMapExportTest")))`, add `TMap<FName, int32>` through `FBlueprintEditorUtils::AddMemberVariable`, and compile it. Resolve its generated `FMapProperty`, populate the CDO with `FScriptMapHelper::AddDefaultValue_Invalid_NeedsRehash`, assign `FName` keys and `int32` values through the helper pointers, call `Rehash`, and export. Assert exact value-terminal metadata, both entries, and sorted canonical output independent of insertion order.

- [ ] **Step 2: Run focused tests and verify RED**

Expected: `[UNSUPPORTED:VariableType]` from the old unconditional Map rejection or no exported entries.

- [ ] **Step 3: Export Map metadata and defaults**

Remove the unconditional Map rejection in `FAnimBPExporter::ExportToAST`. Copy:

```cpp
VarDef.ValuePinCategory = Var.VarType.PinValueType.TerminalCategory.ToString();
VarDef.ValuePinSubCategory = Var.VarType.PinValueType.TerminalSubCategory.ToString();
if (const UObject* ValueTypeObject =
    Var.VarType.PinValueType.TerminalSubCategoryObject.Get())
{
    VarDef.ValueTypeObjectPath = ValueTypeObject->GetPathName();
}
```

Then call `ExportMapEntries`. Resolve `FMapProperty` by member name on `GeneratedClass`, iterate valid indices through `FScriptMapHelper`, convert key/value expressions, and sort `MapEntries` by key then value.

For transient uncompiled fixtures, allow an empty Map only when `FBPVariableDescription::DefaultValue` is empty; reject a claimed non-empty default without a generated property.

- [ ] **Step 4: Run focused tests and verify GREEN**

Expected: compiled Map export succeeds, entries are sorted, and repeated export strings are identical.

- [ ] **Step 5: Commit the exporter slice**

```powershell
git add Source/AnimBP2FP/Private/AnimLangVariableCodec.cpp `
  Source/AnimBP2FP/Private/AnimBPExporter.cpp `
  Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp
git commit -m "Export typed map variable defaults"
```

### Task 4: Two-Stage Map Default Import

**Files:**
- Modify: `Source/AnimBP2FP/Private/AnimLangVariableCodec.cpp`
- Modify: `Source/AnimBP2FP/Private/AnimBPImporter.cpp`
- Modify: `Source/AnimBP2FP/Public/AnimBPImporter.h`
- Test: `Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp`

- [ ] **Step 1: Write failing strict import/re-export tests**

Build AST fixtures for `name -> int` and `name -> object` Maps with non-empty entries, call `ImportFromAST`, then export the result and assert canonical equality. Add a duplicate typed-key test where two syntactically different inputs import to the same key and must fail atomically.

- [ ] **Step 2: Run focused tests and verify RED**

Expected: imported Map is empty or strict canonical re-export differs.

- [ ] **Step 3: Add the two-stage default application helper**

Declare and implement:

```cpp
static bool ApplyMapVariableDefaults(UAnimBlueprint* Blueprint,
    const TArray<FVariableDef>& Variables, FString& OutError);
```

After `BuildVariables`, if any Map has entries:

```cpp
FKismetEditorUtilities::CompileBlueprint(Blueprint,
    EBlueprintCompileOptions::RegenerateSkeletonOnly
    | EBlueprintCompileOptions::SkipGarbageCollection
    | EBlueprintCompileOptions::SkipSave);

if (!ApplyMapVariableDefaults(Blueprint, AST->Variables, MapDefaultError))
{
    UE_LOG(LogAnimBPImporter, Error,
        TEXT("[UNSUPPORTED:VariableDefault] %s"), *MapDefaultError);
    return false;
}
```

- [ ] **Step 4: Build canonical Unreal Map defaults**

In the codec, allocate temporary Map property storage, import every key/value expression with the reflected key/value properties, detect duplicates after `Rehash`, export the complete Map with `FMapProperty::ExportTextItem_Direct`, destroy temporary values, and return the canonical text. Assign it to the matching `FBPVariableDescription::DefaultValue` before the existing full compile.

- [ ] **Step 5: Run focused tests and verify GREEN**

Expected: both primitive and object Map fixtures import, compile, and re-export exactly; invalid defaults fail without returning a usable Blueprint.

- [ ] **Step 6: Commit the importer slice**

```powershell
git add Source/AnimBP2FP/Private/AnimLangVariableCodec.cpp `
  Source/AnimBP2FP/Private/AnimBPImporter.cpp `
  Source/AnimBP2FP/Public/AnimBPImporter.h `
  Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp
git commit -m "Import typed map variable defaults"
```

### Task 5: Map Diff And Incremental Patch

**Files:**
- Modify: `Source/AnimBP2FP/Private/AnimLangDiffer.cpp`
- Modify: `Source/AnimBP2FP/Private/AnimLangPatcher.cpp`
- Modify: `Source/AnimBP2FP/Public/AnimLangPatcher.h`
- Test: `Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp`

- [ ] **Step 1: Write failing diff and patch tests**

Verify that changing only `ValuePinCategory`, `ValueTypeObjectPath`, adding/removing an entry, and changing one value each produce `VariableChanged`. Create a compiled Blueprint, patch its Map entries, and assert strict re-export equals the edited AST.

- [ ] **Step 2: Run focused tests and verify RED**

Expected: value-terminal/entry-only changes are missed or patch leaves an empty/old default.

- [ ] **Step 3: Extend exact variable comparison**

Add comparisons for:

```cpp
|| OldVar.ValuePinCategory != NewVar.ValuePinCategory
|| OldVar.ValuePinSubCategory != NewVar.ValuePinSubCategory
|| OldVar.ValueTypeObjectPath != NewVar.ValueTypeObjectPath
|| OldVar.MapEntries != NewVar.MapEntries
```

- [ ] **Step 4: Apply Map defaults during patch compilation**

Track whether a variable operation adds/changes a non-empty Map. After structural changes and before the patcher's final full compile, run the same skeleton-compile/default-build sequence as full import. Expose no second conversion implementation; call `FAnimLangVariableCodec`.

- [ ] **Step 5: Run focused tests and verify GREEN**

Expected: all Map diff cases are detected and incremental update re-exports canonically.

- [ ] **Step 6: Commit the diff/patch slice**

```powershell
git add Source/AnimBP2FP/Private/AnimLangDiffer.cpp `
  Source/AnimBP2FP/Private/AnimLangPatcher.cpp `
  Source/AnimBP2FP/Public/AnimLangPatcher.h `
  Source/AnimBP2FPEditor/Private/Tests/AnimBP2FPVariableTypeTests.cpp
git commit -m "Patch AnimLang map variable changes"
```

### Task 6: Documentation And Full Verification

**Files:**
- Modify: `README.md`
- Verify: `F:\GASP\Saved\Logs\AnimBP2FP-MapVariables.log`
- Verify: strict outputs for `Face_AnimBP` and `ABP_GenericRetarget`

- [ ] **Step 1: Document canonical Map syntax**

Add the approved example and omission rules: explicit `:container map`, required `:value-pin-category`, omitted `None` subcategories, omitted empty default, structured non-empty entries.

- [ ] **Step 2: Run formatting and source checks**

```powershell
git diff --check
Select-String -Path Source\AnimBP2FP\Private\AnimBPExporter.cpp `
  -Pattern 'map value terminal types are not represented'
```

Expected: `git diff --check` exits zero and the removed rejection string has no matches.

- [ ] **Step 3: Build the complete Editor target with plugin audit**

Run the build command from Common Commands. Expected: exit code 0, no missing plugin dependency warning, receipts/manifests/DLLs agree with `D:\UnrealEngine` UE 5.9.

- [ ] **Step 4: Run the focused Map suite**

Run the focused command. Inspect the log for `Success`, zero failed tests, and no unexpected `LogAnimBP2FP`/`LogAnimBPImporter` errors.

- [ ] **Step 5: Run the full AnimBP2FP suite**

```powershell
& $EditorCmd $Project -unattended -nop4 -nosplash -nullrhi `
  '-ExecCmds=Automation RunTests AnimBP2FP; Quit' `
  '-TestExit=Automation Test Queue Empty' `
  '-log=F:\GASP\Saved\Logs\AnimBP2FP-MapVariables-Full.log'
```

Expected: zero failed tests.

- [ ] **Step 6: Verify the two previously blocked real assets**

```powershell
& $EditorCmd $Project -unattended -nop4 -nosplash -nullrhi `
  '-run=AnimBP2FPExport' `
  '-AssetPath=/Game/MetaHumans/Common/Face/Face_AnimBP' `
  '-log=F:\GASP\Saved\Logs\AnimBP2FP-Map-Face.log'

& $EditorCmd $Project -unattended -nop4 -nosplash -nullrhi `
  '-run=AnimBP2FPExport' `
  '-AssetPath=/Game/Blueprints/RetargetedCharacters/ABP_GenericRetarget' `
  '-log=F:\GASP\Saved\Logs\AnimBP2FP-Map-Retarget.log'
```

Expected: no `[UNSUPPORTED:VariableType]`/`[UNSUPPORTED:VariableDefault]`, and both outputs contain canonical typed Map declarations.

- [ ] **Step 7: Run strict transient round-trip on both outputs**

Run strict round-trip over the generated bundle root:

```powershell
& $EditorCmd $Project -unattended -nop4 -nosplash -nullrhi `
  '-run=AnimBP2FPRoundTrip' `
  '-Bundle=F:\GASP\Saved\BP2DSL' `
  '-OutDir=/Engine/Transient/AnimBP2FPMapRoundTrip' `
  '-log=F:\GASP\Saved\Logs\AnimBP2FP-Map-RoundTrip.log'
```

Expected: strict canonical re-export equality and exit code 0 for the exported bundle.

- [ ] **Step 8: Cold-load verification**

Start a fresh `UnrealEditor-Cmd.exe` with `-nullrhi -unattended -nop4 -nosplash -stdout -FullStdOutLogOutput`, load the project, and quit. Expected: AnimBP2FP modules load without related warnings/errors and no stale-build or `/Script/AnimBP2FP` failure.

- [ ] **Step 9: Final commit**

```powershell
git add README.md
git commit -m "Document AnimLang map variables"
git status --short
```

Expected: clean plugin worktree after all implementation commits.
