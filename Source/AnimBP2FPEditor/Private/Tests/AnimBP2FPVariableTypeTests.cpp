// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "AnimLangParser.h"
#include "AnimLangDiffer.h"
#include "AnimLangPatcher.h"
#include "AnimLangParser.h"
#include "AnimLangVariableCodec.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS && ANIMBP2FP_HAS_ANIM_AUTHORING

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableExporterPreservesReadableTypes,
	"AnimBP2FP.VariableTypes.ExporterPreservesReadableTypes",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableExporterPreservesReadableTypes::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Blueprint = NewObject<UAnimBlueprint>(GetTransientPackage());

	auto AddVariable = [Blueprint](const TCHAR* Name, const FEdGraphPinType& PinType)
	{
		FBPVariableDescription& Variable = Blueprint->NewVariables.AddDefaulted_GetRef();
		Variable.VarName = Name;
		Variable.VarType = PinType;
	};

	FEdGraphPinType TransformType;
	TransformType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	TransformType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	AddVariable(TEXT("WorldTransform"), TransformType);

	FEdGraphPinType VectorType;
	VectorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	VectorType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	AddVariable(TEXT("Velocity"), VectorType);

	FEdGraphPinType ObjectType;
	ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object;
	ObjectType.PinSubCategoryObject = UObject::StaticClass();
	AddVariable(TEXT("Target"), ObjectType);

	FEdGraphPinType ArrayType = TransformType;
	ArrayType.ContainerType = EPinContainerType::Array;
	AddVariable(TEXT("Transforms"), ArrayType);

	FEdGraphPinType GenericStructType;
	GenericStructType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	GenericStructType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
	AddVariable(TEXT("CharacterProperties"), GenericStructType);

	const TSharedPtr<FAnimGraphAST> AST = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("AST exported"), AST.IsValid());
	if (!AST.IsValid() || AST->Variables.Num() != 5)
	{
		return false;
	}

	TestEqual(TEXT("transform remains transform"), AST->Variables[0].Type, EPinType::Transform);
	TestEqual(TEXT("vector remains vector"), AST->Variables[1].Type, EPinType::Vector);
	TestEqual(TEXT("object remains object"), AST->Variables[2].Type, EPinType::Object);
	TestEqual(TEXT("array element remains transform"), AST->Variables[3].Type, EPinType::Transform);
	TestEqual(TEXT("generic struct remains struct"), AST->Variables[4].Type, EPinType::Struct);
	TestEqual(TEXT("object category preserved"), AST->Variables[2].PinCategory, UEdGraphSchema_K2::PC_Object.ToString());
	TestEqual(TEXT("object class path preserved"), AST->Variables[2].TypeObjectPath, UObject::StaticClass()->GetPathName());
	TestEqual(TEXT("array container preserved"), AST->Variables[3].ContainerType, FString(TEXT("array")));

	const FString DSL = AST->ToString();
	TestFalse(TEXT("DSL omits redundant object pin category"), DSL.Contains(TEXT(":pin-category \"object\"")));
	TestTrue(TEXT("DSL emits array container"), DSL.Contains(TEXT(":container array")));
	TestTrue(TEXT("generic struct keeps required category"),
		DSL.Contains(TEXT("(struct :name \"CharacterProperties\" :pin-category \"struct\"")));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("exact pin metadata parses"), Parsed.IsValid() && Errors.Num() == 0);
	if (Parsed.IsValid() && Parsed->Variables.Num() == 5)
	{
		TestEqual(TEXT("object category round-trips"), Parsed->Variables[2].PinCategory, UEdGraphSchema_K2::PC_Object.ToString());
		TestEqual(TEXT("object path round-trips"), Parsed->Variables[2].TypeObjectPath, UObject::StaticClass()->GetPathName());
		TestEqual(TEXT("array container round-trips"), Parsed->Variables[3].ContainerType, FString(TEXT("array")));
		TestEqual(TEXT("generic struct category round-trips"),
			Parsed->Variables[4].PinCategory, UEdGraphSchema_K2::PC_Struct.ToString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariablePinTypeSurvivesDSLRoundTrip,
	"AnimBP2FP.VariableTypes.PinTypeSurvivesDSLRoundTrip",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariablePinTypeSurvivesDSLRoundTrip::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Blueprint = NewObject<UAnimBlueprint>(GetTransientPackage());
	FBPVariableDescription& Variable = Blueprint->NewVariables.AddDefaulted_GetRef();
	Variable.VarName = TEXT("Transforms");
	Variable.VarType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	Variable.VarType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	Variable.VarType.ContainerType = EPinContainerType::Array;
	Variable.VarType.bIsConst = true;

	const FString ExportedDSL = FAnimBPExporter::Export(Blueprint);
	TestTrue(TEXT("container is human-readable"), ExportedDSL.Contains(TEXT(":container array")));
	TestTrue(TEXT("exact category is serialized"), ExportedDSL.Contains(TEXT(":pin-category \"struct\"")));
	TestTrue(TEXT("exact type object is serialized"), ExportedDSL.Contains(TEXT(":type-object (asset \"/Script/CoreUObject.Transform\")")));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(ExportedDSL, Errors);
	TestTrue(TEXT("exported DSL parses without errors"), Parsed.IsValid() && Errors.Num() == 0);
	if (!Parsed.IsValid())
	{
		return false;
	}

	const FString ReserializedDSL = Parsed->ToString();
	TestTrue(TEXT("container survives parse/serialize"), ReserializedDSL.Contains(TEXT(":container array")));
	TestTrue(TEXT("pin category survives parse/serialize"), ReserializedDSL.Contains(TEXT(":pin-category \"struct\"")));
	TestTrue(TEXT("type object survives parse/serialize"), ReserializedDSL.Contains(TEXT(":type-object (asset \"/Script/CoreUObject.Transform\")")));
	TestTrue(TEXT("const flag survives parse/serialize"), ReserializedDSL.Contains(TEXT(":const true")));
	const TSharedPtr<FAnimGraphAST> OriginalAST = FAnimBPExporter::ExportToAST(Blueprint);
	TestFalse(TEXT("export-parse diff is empty"), FAnimLangDiffer::Diff(OriginalAST, Parsed).HasChanges());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableNameWithSpacesRoundTrips,
	"AnimBP2FP.VariableTypes.NameWithSpacesRoundTrips",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableNameWithSpacesRoundTrips::RunTest(const FString& Parameters)
{
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_SpacedVariableName");
	FVariableDef& Variable = AST->Variables.AddDefaulted_GetRef();
	Variable.Name = TEXT("MM Search Cost");
	Variable.Type = EPinType::Float;
#if ENGINE_MAJOR_VERSION >= 5
	Variable.PinCategory = UEdGraphSchema_K2::PC_Real.ToString();
#else
	Variable.PinCategory = UEdGraphSchema_K2::PC_Float.ToString();
#endif

	const FString DSL = AST->ToString();
	const bool bUsesExplicitQuotedName = DSL.Contains(TEXT(":name \"MM Search Cost\""));
	TestTrue(TEXT("spaced variable name uses explicit quoted syntax"), bUsesExplicitQuotedName);
	if (!bUsesExplicitQuotedName)
	{
		return false;
	}

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("spaced variable DSL parses"), Parsed.IsValid() && Errors.Num() == 0);
	TestEqual(TEXT("one variable survives"), Parsed.IsValid() ? Parsed->Variables.Num() : 0, 1);
	if (Parsed.IsValid() && Parsed->Variables.Num() == 1)
	{
		TestEqual(TEXT("full variable name survives"), Parsed->Variables[0].Name, Variable.Name);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableExactTypeDiff,
	"AnimBP2FP.VariableTypes.ExactTypeDiff",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableExactTypeDiff::RunTest(const FString& Parameters)
{
	TSharedPtr<FAnimGraphAST> OldAST = MakeShared<FAnimGraphAST>();
	TSharedPtr<FAnimGraphAST> NewAST = MakeShared<FAnimGraphAST>();
	FVariableDef OldVar;
	OldVar.Name = TEXT("Items");
	OldVar.Type = EPinType::Object;
	OldVar.PinCategory = UEdGraphSchema_K2::PC_Object.ToString();
	OldVar.TypeObjectPath = UObject::StaticClass()->GetPathName();
	OldVar.ContainerType = TEXT("none");
	FVariableDef NewVar = OldVar;
	NewVar.ContainerType = TEXT("array");
	OldAST->Variables.Add(OldVar);
	NewAST->Variables.Add(NewVar);
	const FAnimLangDiffResult Diff = FAnimLangDiffer::Diff(OldAST, NewAST);
	TestTrue(TEXT("exact container change is detected"), Diff.HasChanges());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableCompiledMapExport,
	"AnimBP2FP.VariableTypes.CompiledMapExport",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableCompiledMapExport::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), GetTransientPackage(), TEXT("ABP_MapExportFixture"),
		BPTYPE_Normal, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPMapExportTest"))));
	TestNotNull(TEXT("map export fixture exists"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	FEdGraphPinType MapType;
	MapType.PinCategory = UEdGraphSchema_K2::PC_Name;
	MapType.ContainerType = EPinContainerType::Map;
	MapType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
	TestTrue(TEXT("map variable is added"),
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Values"), MapType));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	FMapProperty* MapProperty = Blueprint->GeneratedClass
		? FindFProperty<FMapProperty>(Blueprint->GeneratedClass, TEXT("Values"))
		: nullptr;
	TestNotNull(TEXT("compiled map property exists"), MapProperty);
	UObject* Defaults = Blueprint->GeneratedClass
		? Blueprint->GeneratedClass->GetDefaultObject(false)
		: nullptr;
	TestNotNull(TEXT("compiled class defaults exist"), Defaults);
	if (!MapProperty || !Defaults)
	{
		return false;
	}

	void* MapAddress = MapProperty->ContainerPtrToValuePtr<void>(Defaults);
	FScriptMapHelper MapHelper(MapProperty, MapAddress);
	auto AddEntry = [&MapHelper, MapProperty](const FName Key, const int32 Value)
	{
		const int32 Index = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
		MapProperty->KeyProp->CopyCompleteValue(MapHelper.GetKeyPtr(Index), &Key);
		MapProperty->ValueProp->CopyCompleteValue(MapHelper.GetValuePtr(Index), &Value);
	};
	AddEntry(TEXT("Run"), 2);
	AddEntry(TEXT("Idle"), 1);
	MapHelper.Rehash();

	const TSharedPtr<FAnimGraphAST> AST = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("compiled map exports"), AST.IsValid());
	if (!AST.IsValid() || AST->Variables.Num() != 1)
	{
		return false;
	}

	const FVariableDef& Variable = AST->Variables[0];
	TestEqual(TEXT("map key category exports"), Variable.PinCategory, UEdGraphSchema_K2::PC_Name.ToString());
	TestEqual(TEXT("map value category exports"), Variable.ValuePinCategory, UEdGraphSchema_K2::PC_Int.ToString());
	TestEqual(TEXT("both map entries export"), Variable.MapEntries.Num(), 2);
	if (Variable.MapEntries.Num() == 2)
	{
		TestEqual(TEXT("entries sort by key"), Variable.MapEntries[0].KeyExpression, FString(TEXT("\"Idle\"")));
		TestEqual(TEXT("first value exports"), Variable.MapEntries[0].ValueExpression, FString(TEXT("1")));
		TestEqual(TEXT("second key exports"), Variable.MapEntries[1].KeyExpression, FString(TEXT("\"Run\"")));
	}

	const FString FirstExport = AST->ToString();
	const FString SecondExport = FAnimBPExporter::ExportToAST(Blueprint)->ToString();
	TestEqual(TEXT("map export is deterministic"), SecondExport, FirstExport);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableMapDefaultImport,
	"AnimBP2FP.VariableTypes.MapDefaultImport",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableMapDefaultImport::RunTest(const FString& Parameters)
{
	auto ImportAndCheck = [this](const FString& Source, const FString& ExpectedValueCategory, const FString& ExpectedValue)
	{
		TArray<FAnimLangParseError> ParseErrors;
		const TSharedPtr<FAnimGraphAST> AST = FAnimLangParser::Parse(Source, ParseErrors);
		TestTrue(TEXT("map import fixture parses"), AST.IsValid() && ParseErrors.Num() == 0);
		if (!AST.IsValid())
		{
			return false;
		}

		FAnimBPImportContext Context;
		Context.bTransient = true;
		FString ImportError;
		UAnimBlueprint* Imported = FAnimBPImporter::ImportFromAST(
			AST, TEXT("/Engine/Transient/AnimBP2FPMapImport"), Context, &ImportError);
		TestNotNull(TEXT("map fixture imports"), Imported);
		if (!Imported)
		{
			AddError(ImportError);
			return false;
		}

		const TSharedPtr<FAnimGraphAST> Exported = FAnimBPExporter::ExportToAST(Imported);
		TestTrue(TEXT("imported map re-exports"), Exported.IsValid());
		if (!Exported.IsValid() || Exported->Variables.Num() != 1)
		{
			return false;
		}
		TestEqual(TEXT("value category round-trips"), Exported->Variables[0].ValuePinCategory, ExpectedValueCategory);
		TestEqual(TEXT("one map entry round-trips"), Exported->Variables[0].MapEntries.Num(), 1);
		if (Exported->Variables[0].MapEntries.Num() == 1)
		{
			TestEqual(TEXT("map value round-trips"), Exported->Variables[0].MapEntries[0].ValueExpression, ExpectedValue);
		}
		return true;
	};

	const FString IntSource = TEXT(R"ANIM(
(anim-blueprint "ABP_MapIntImport"
  :variables [
    (name :name "Values" :container map :value-pin-category "int"
      :default [(entry :key "Idle" :value 7)])
  ])
)ANIM");
	ImportAndCheck(IntSource, UEdGraphSchema_K2::PC_Int.ToString(), TEXT("7"));

	const FString ObjectSource = TEXT(R"ANIM(
(anim-blueprint "ABP_MapObjectImport"
  :variables [
    (name :name "Values" :container map :value-pin-category "object"
      :value-type-object (asset "/Script/CoreUObject.Object")
      :default [(entry :key "Class" :value (asset "/Script/CoreUObject.Object"))])
  ])
)ANIM");
	ImportAndCheck(
		ObjectSource,
		UEdGraphSchema_K2::PC_Object.ToString(),
		TEXT("(asset \"/Script/CoreUObject.Object\")"));

	const FString DuplicateSource = TEXT(R"ANIM(
(anim-blueprint "ABP_MapDuplicateImport"
  :variables [
    (name :name "Values" :container map :value-pin-category "int"
      :default [
        (entry :key "Idle" :value 1)
        (entry :key (ue-value "Idle") :value 2)
      ])
  ])
)ANIM");
	TArray<FAnimLangParseError> DuplicateParseErrors;
	const TSharedPtr<FAnimGraphAST> DuplicateAST = FAnimLangParser::Parse(DuplicateSource, DuplicateParseErrors);
	TestTrue(TEXT("typed duplicate fixture parses"), DuplicateAST.IsValid() && DuplicateParseErrors.Num() == 0);
	FAnimBPImportContext Context;
	Context.bTransient = true;
	FString DuplicateError;
	AddExpectedError(TEXT("duplicate typed key"), EAutomationExpectedErrorFlags::Contains, 1);
	UAnimBlueprint* DuplicateResult = FAnimBPImporter::ImportFromAST(
		DuplicateAST, TEXT("/Engine/Transient/AnimBP2FPMapDuplicate"), Context, &DuplicateError);
	TestNull(TEXT("typed duplicate map import fails atomically"), DuplicateResult);
	TestTrue(TEXT("duplicate error is explicit"), DuplicateError.Contains(TEXT("duplicate typed key")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableMapDiffAndPatch,
	"AnimBP2FP.VariableTypes.MapDiffAndPatch",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableMapDiffAndPatch::RunTest(const FString& Parameters)
{
	FVariableDef BaseVariable;
	BaseVariable.Name = TEXT("Values");
	BaseVariable.Type = EPinType::Name;
	BaseVariable.PinCategory = UEdGraphSchema_K2::PC_Name.ToString();
	BaseVariable.ContainerType = TEXT("map");
	BaseVariable.ValuePinCategory = UEdGraphSchema_K2::PC_Int.ToString();
	BaseVariable.MapEntries.Add({TEXT("\"Idle\""), TEXT("1")});

	auto DetectsVariableChange = [&BaseVariable](TFunctionRef<void(FVariableDef&)> Mutate)
	{
		TSharedPtr<FAnimGraphAST> OldAST = MakeShared<FAnimGraphAST>();
		TSharedPtr<FAnimGraphAST> NewAST = MakeShared<FAnimGraphAST>();
		OldAST->Variables.Add(BaseVariable);
		FVariableDef Changed = BaseVariable;
		Mutate(Changed);
		NewAST->Variables.Add(MoveTemp(Changed));
		const FAnimLangDiffResult Diff = FAnimLangDiffer::Diff(OldAST, NewAST);
		return Diff.Entries.ContainsByPredicate([](const FAnimLangDiffEntry& Entry)
		{
			return Entry.Op == EAnimLangDiffOp::VariableChanged;
		});
	};

	TestTrue(TEXT("value category change is detected"), DetectsVariableChange([](FVariableDef& Variable)
	{
		Variable.ValuePinCategory = UEdGraphSchema_K2::PC_Object.ToString();
	}));
	TestTrue(TEXT("value type object change is detected"), DetectsVariableChange([](FVariableDef& Variable)
	{
		Variable.ValueTypeObjectPath = UObject::StaticClass()->GetPathName();
	}));
	TestTrue(TEXT("map entry addition is detected"), DetectsVariableChange([](FVariableDef& Variable)
	{
		Variable.MapEntries.Add({TEXT("\"Run\""), TEXT("2")});
	}));
	TestTrue(TEXT("map entry removal is detected"), DetectsVariableChange([](FVariableDef& Variable)
	{
		Variable.MapEntries.Reset();
	}));
	TestTrue(TEXT("map entry value change is detected"), DetectsVariableChange([](FVariableDef& Variable)
	{
		Variable.MapEntries[0].ValueExpression = TEXT("9");
	}));

	const FString Source = TEXT(R"ANIM(
(anim-blueprint "ABP_MapPatch"
  :variables [
    (name :name "Values" :container map :value-pin-category "int"
      :default [(entry :key "Idle" :value 1)])
  ])
)ANIM");
	TArray<FAnimLangParseError> ParseErrors;
	const TSharedPtr<FAnimGraphAST> SourceAST = FAnimLangParser::Parse(Source, ParseErrors);
	FAnimBPImportContext Context;
	Context.bTransient = true;
	FString ImportError;
	UAnimBlueprint* Blueprint = FAnimBPImporter::ImportFromAST(
		SourceAST, TEXT("/Engine/Transient/AnimBP2FPMapPatch"), Context, &ImportError);
	TestNotNull(TEXT("map patch fixture imports"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	TSharedPtr<FAnimGraphAST> EditedAST = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("map patch fixture exports"), EditedAST.IsValid());
	if (!EditedAST.IsValid())
	{
		return false;
	}
	EditedAST->Variables[0].MapEntries[0].ValueExpression = TEXT("9");
	const FAnimLangPatchResult PatchResult = FAnimLangPatcher::IncrementalUpdate(Blueprint, EditedAST->ToString());
	TestTrue(TEXT("map entry patch succeeds"), PatchResult.bSuccess);

	const TSharedPtr<FAnimGraphAST> PatchedAST = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("patched map re-exports"), PatchedAST.IsValid());
	if (PatchedAST.IsValid() && PatchedAST->Variables.Num() == 1
		&& PatchedAST->Variables[0].MapEntries.Num() == 1)
	{
		TestEqual(TEXT("patched map value persists"),
			PatchedAST->Variables[0].MapEntries[0].ValueExpression,
			FString(TEXT("9")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableMapCanonicalSyntax,
	"AnimBP2FP.VariableTypes.MapCanonicalSyntax",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableMapCanonicalSyntax::RunTest(const FString& Parameters)
{
	const FString Source = TEXT(R"ANIM(
(anim-blueprint "ABP_MapSyntax"
  :variables [
    (name :name "Values" :container map
      :value-pin-category "int"
      :default [
        (entry :key "Run" :value 2)
        (entry :key "Idle" :value 1)
      ])
  ])
)ANIM");

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(Source, Errors);
	TestTrue(TEXT("typed map syntax parses"), Parsed.IsValid() && Errors.Num() == 0);
	TestEqual(TEXT("one map variable parses"), Parsed.IsValid() ? Parsed->Variables.Num() : 0, 1);
	if (!Parsed.IsValid() || Parsed->Variables.Num() != 1)
	{
		return false;
	}

	const FVariableDef& Variable = Parsed->Variables[0];
	TestEqual(TEXT("map container parses"), Variable.ContainerType, FString(TEXT("map")));
	TestEqual(TEXT("map value category parses"), Variable.ValuePinCategory, FString(TEXT("int")));
	TestEqual(TEXT("two map entries parse"), Variable.MapEntries.Num(), 2);
	if (Variable.MapEntries.Num() == 2)
	{
		TestEqual(TEXT("entries canonicalize by key"), Variable.MapEntries[0].KeyExpression, FString(TEXT("\"Idle\"")));
		TestEqual(TEXT("entry value remains typed"), Variable.MapEntries[0].ValueExpression, FString(TEXT("1")));
	}

	const FString Canonical = Parsed->ToString();
	TestFalse(TEXT("redundant key category is omitted"), Canonical.Contains(TEXT(":pin-category")));
	TestFalse(TEXT("None subcategories are omitted"), Canonical.Contains(TEXT("pin-subcategory")));
	TestTrue(TEXT("map container remains explicit"), Canonical.Contains(TEXT(":container map")));
	TestTrue(TEXT("value category remains explicit"), Canonical.Contains(TEXT(":value-pin-category \"int\"")));
	TestTrue(TEXT("canonical key order is stable"),
		Canonical.Find(TEXT(":key \"Idle\"")) < Canonical.Find(TEXT(":key \"Run\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableMapSyntaxValidation,
	"AnimBP2FP.VariableTypes.MapSyntaxValidation",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableMapSyntaxValidation::RunTest(const FString& Parameters)
{
	auto ParseHasErrors = [](const FString& VariableSource)
	{
		const FString Source = FString::Printf(
			TEXT("(anim-blueprint \"ABP_InvalidMap\" :variables [%s])"), *VariableSource);
		TArray<FAnimLangParseError> Errors;
		FAnimLangParser::Parse(Source, Errors);
		return Errors.ContainsByPredicate([](const FAnimLangParseError& Error) { return !Error.bWarning; });
	};

	TestTrue(TEXT("map requires value category"),
		ParseHasErrors(TEXT("(name :name \"Values\" :container map)")));
	TestTrue(TEXT("scalar rejects value fields"),
		ParseHasErrors(TEXT("(name :name \"Value\" :value-pin-category \"int\")")));
	TestTrue(TEXT("entry requires key"),
		ParseHasErrors(TEXT("(name :name \"Values\" :container map :value-pin-category \"int\" :default [(entry :value 1)])")));
	TestTrue(TEXT("entry requires value"),
		ParseHasErrors(TEXT("(name :name \"Values\" :container map :value-pin-category \"int\" :default [(entry :key \"Idle\")])")));
	TestTrue(TEXT("duplicate raw keys are rejected"),
		ParseHasErrors(TEXT("(name :name \"Values\" :container map :value-pin-category \"int\" :default [(entry :key \"Idle\" :value 1) (entry :key \"Idle\" :value 2)])")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableMapPinTypeCodec,
	"AnimBP2FP.VariableTypes.MapPinTypeCodec",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableMapPinTypeCodec::RunTest(const FString& Parameters)
{
	FVariableDef Variable;
	Variable.Name = TEXT("ObjectByName");
	Variable.Type = EPinType::Name;
	Variable.PinCategory = UEdGraphSchema_K2::PC_Name.ToString();
	Variable.PinSubCategory = TEXT("None");
	Variable.ContainerType = TEXT("map");
	Variable.ValuePinCategory = UEdGraphSchema_K2::PC_Object.ToString();
	Variable.ValuePinSubCategory = TEXT("None");
	Variable.ValueTypeObjectPath = UObject::StaticClass()->GetPathName();

	FEdGraphPinType PinType;
	FString Error;
	TestTrue(TEXT("typed map pin builds"),
		FAnimLangVariableCodec::BuildPinType(Variable, PinType, Error));
	TestEqual(TEXT("map container builds"), PinType.ContainerType, EPinContainerType::Map);
	TestEqual(TEXT("map key category builds"), PinType.PinCategory, UEdGraphSchema_K2::PC_Name);
	TestEqual(TEXT("map value category builds"),
		PinType.PinValueType.TerminalCategory, UEdGraphSchema_K2::PC_Object);
	TestEqual(TEXT("map value object builds"),
		PinType.PinValueType.TerminalSubCategoryObject.Get(), static_cast<UObject*>(UObject::StaticClass()));

	FVariableDef MissingValue = Variable;
	MissingValue.ValuePinCategory.Reset();
	TestFalse(TEXT("map without value category fails"),
		FAnimLangVariableCodec::BuildPinType(MissingValue, PinType, Error));

	FVariableDef MissingObject = Variable;
	MissingObject.ValueTypeObjectPath = TEXT("/Script/DoesNotExist.MissingClass");
	TestFalse(TEXT("missing value type object fails"),
		FAnimLangVariableCodec::BuildPinType(MissingObject, PinType, Error));
	return true;
}

#endif
