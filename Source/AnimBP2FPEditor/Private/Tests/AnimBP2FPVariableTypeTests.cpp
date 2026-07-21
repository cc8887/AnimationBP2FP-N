// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Animation/AnimBlueprint.h"
#include "AnimBPExporter.h"
#include "AnimLangParser.h"
#include "AnimLangDiffer.h"
#include "AnimLangParser.h"
#include "EdGraphSchema_K2.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableExporterPreservesReadableTypes,
	"AnimBP2FP.VariableTypes.ExporterPreservesReadableTypes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

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

	const TSharedPtr<FAnimGraphAST> AST = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("AST exported"), AST.IsValid());
	if (!AST.IsValid() || AST->Variables.Num() != 4)
	{
		return false;
	}

	TestEqual(TEXT("transform remains transform"), AST->Variables[0].Type, EPinType::Transform);
	TestEqual(TEXT("vector remains vector"), AST->Variables[1].Type, EPinType::Vector);
	TestEqual(TEXT("object remains object"), AST->Variables[2].Type, EPinType::Object);
	TestEqual(TEXT("array element remains transform"), AST->Variables[3].Type, EPinType::Transform);
	TestEqual(TEXT("object category preserved"), AST->Variables[2].PinCategory, UEdGraphSchema_K2::PC_Object.ToString());
	TestEqual(TEXT("object class path preserved"), AST->Variables[2].TypeObjectPath, UObject::StaticClass()->GetPathName());
	TestEqual(TEXT("array container preserved"), AST->Variables[3].ContainerType, FString(TEXT("array")));

	const FString DSL = AST->ToString();
	TestTrue(TEXT("DSL emits exact pin category"), DSL.Contains(TEXT(":pin-category \"object\"")));
	TestTrue(TEXT("DSL emits array container"), DSL.Contains(TEXT(":container array")));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("exact pin metadata parses"), Parsed.IsValid() && Errors.IsEmpty());
	if (Parsed.IsValid() && Parsed->Variables.Num() == 4)
	{
		TestEqual(TEXT("object category round-trips"), Parsed->Variables[2].PinCategory, UEdGraphSchema_K2::PC_Object.ToString());
		TestEqual(TEXT("object path round-trips"), Parsed->Variables[2].TypeObjectPath, UObject::StaticClass()->GetPathName());
		TestEqual(TEXT("array container round-trips"), Parsed->Variables[3].ContainerType, FString(TEXT("array")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariablePinTypeSurvivesDSLRoundTrip,
	"AnimBP2FP.VariableTypes.PinTypeSurvivesDSLRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

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
	TestTrue(TEXT("exported DSL parses without errors"), Parsed.IsValid() && Errors.IsEmpty());
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
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableNameWithSpacesRoundTrips::RunTest(const FString& Parameters)
{
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_SpacedVariableName");
	FVariableDef& Variable = AST->Variables.AddDefaulted_GetRef();
	Variable.Name = TEXT("MM Search Cost");
	Variable.Type = EPinType::Float;
	Variable.PinCategory = UEdGraphSchema_K2::PC_Real.ToString();

	const FString DSL = AST->ToString();
	const bool bUsesExplicitQuotedName = DSL.Contains(TEXT(":name \"MM Search Cost\""));
	TestTrue(TEXT("spaced variable name uses explicit quoted syntax"), bUsesExplicitQuotedName);
	if (!bUsesExplicitQuotedName)
	{
		return false;
	}

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("spaced variable DSL parses"), Parsed.IsValid() && Errors.IsEmpty());
	TestEqual(TEXT("one variable survives"), Parsed.IsValid() ? Parsed->Variables.Num() : 0, 1);
	if (Parsed.IsValid() && Parsed->Variables.Num() == 1)
	{
		TestEqual(TEXT("full variable name survives"), Parsed->Variables[0].Name, Variable.Name);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPVariableExactTypeDiffAndMapFailure,
	"AnimBP2FP.VariableTypes.ExactTypeDiffAndMapFailure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPVariableExactTypeDiffAndMapFailure::RunTest(const FString& Parameters)
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

	UAnimBlueprint* Blueprint = NewObject<UAnimBlueprint>(GetTransientPackage());
	FBPVariableDescription& MapVariable = Blueprint->NewVariables.AddDefaulted_GetRef();
	MapVariable.VarName = TEXT("MapVariable");
	MapVariable.VarType.PinCategory = UEdGraphSchema_K2::PC_Name;
	MapVariable.VarType.ContainerType = EPinContainerType::Map;
	MapVariable.VarType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
	AddExpectedError(TEXT("[UNSUPPORTED:VariableType]"), EAutomationExpectedErrorFlags::Contains, 1);
	const FString ExportedDSL = FAnimBPExporter::Export(Blueprint);
	TestTrue(TEXT("map export fails instead of emitting incomplete DSL"), ExportedDSL.StartsWith(TEXT("; Error:")));
	return true;
}

#endif
