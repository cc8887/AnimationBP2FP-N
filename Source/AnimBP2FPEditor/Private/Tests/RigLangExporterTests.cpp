// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RigLangExporter.h"
#include "RigLangParser.h"
#include "AnimLispWorkspace.h"
#include "ControlRigBlueprintLegacy.h"
#include "ControlRigBlueprintFactory.h"
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyMetadata.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMCore/RigVMExternalVariable.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReturnNode.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMDispatchNode.h"
#include "RigVMModel/Nodes/RigVMAggregateNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMFunctions/Math/RigVMFunction_MathFloat.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RigLangExporterTests
{
const EAutomationTestFlags TestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

FString QuoteValue(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return TEXT("\"") + Escaped + TEXT("\"");
}

FString ExportStruct(const UScriptStruct* Struct, const void* Value);

FString ExportTemplateTypeMap(const FRigVMTemplateTypeMap& TypeMap)
{
	TArray<FString> Values;
	for (const TPair<FName, TRigVMTypeIndex>& Pair : TypeMap)
	{
		const FRigVMTemplateArgumentType& Type = FRigVMRegistry::Get().GetType(Pair.Value);
		Values.Add(TEXT("(") + QuoteValue(Pair.Key.ToString()) + TEXT(" ")
			+ QuoteValue(Type.CPPType.ToString()) + TEXT(")"));
	}
	Values.Sort();
	return TEXT("(") + FString::Join(Values, TEXT(" ")) + TEXT(")");
}

FString ExportVariableMap(const TMap<FName, FName>& VariableMap)
{
	TArray<FString> Values;
	for (const TPair<FName, FName>& Pair : VariableMap)
	{
		Values.Add(TEXT("(") + QuoteValue(Pair.Key.ToString()) + TEXT(" ")
			+ QuoteValue(Pair.Value.ToString()) + TEXT(")"));
	}
	Values.Sort();
	return TEXT("(") + FString::Join(Values, TEXT(" ")) + TEXT(")");
}

FString ExportTypedControlValue(const FRigControlValue& Value, ERigControlType Type)
{
	auto F = [](float Number) { return LexToString(Number); };
	const FString TypeName = StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(Type));
	TArray<FString> Fields;
	switch (Type)
	{
	case ERigControlType::Bool: Fields.Add(Value.Get<bool>() ? TEXT("true") : TEXT("false")); break;
	case ERigControlType::Float:
	case ERigControlType::ScaleFloat: Fields.Add(F(Value.Get<float>())); break;
	case ERigControlType::Integer: Fields.Add(FString::FromInt(Value.Get<int32>())); break;
	case ERigControlType::Position:
	case ERigControlType::Scale:
	case ERigControlType::Rotator:
	case ERigControlType::Vector2D:
	{
		const FVector3f& V = Value.GetRef<FVector3f>();
		Fields = {F(V.X), F(V.Y), F(V.Z)};
		break;
	}
	case ERigControlType::Transform:
	{
		const FRigControlValue::FTransform_Float& T = Value.GetRef<FRigControlValue::FTransform_Float>();
		Fields = {F(T.TranslationX), F(T.TranslationY), F(T.TranslationZ), F(T.RotationX), F(T.RotationY),
			F(T.RotationZ), F(T.RotationW), F(T.ScaleX), F(T.ScaleY), F(T.ScaleZ)};
		break;
	}
	case ERigControlType::TransformNoScale:
	{
		const FRigControlValue::FTransformNoScale_Float& T = Value.GetRef<FRigControlValue::FTransformNoScale_Float>();
		Fields = {F(T.TranslationX), F(T.TranslationY), F(T.TranslationZ), F(T.RotationX), F(T.RotationY),
			F(T.RotationZ), F(T.RotationW)};
		break;
	}
	case ERigControlType::EulerTransform:
	{
		const FRigControlValue::FEulerTransform_Float& T = Value.GetRef<FRigControlValue::FEulerTransform_Float>();
		Fields = {F(T.TranslationX), F(T.TranslationY), F(T.TranslationZ), F(T.RotationPitch), F(T.RotationYaw),
			F(T.RotationRoll), F(T.ScaleX), F(T.ScaleY), F(T.ScaleZ)};
		break;
	}
	default: break;
	}
	return TEXT("(") + TypeName + (Fields.IsEmpty() ? FString() : TEXT(" ") + FString::Join(Fields, TEXT(" "))) + TEXT(")");
}

FString ExportMetadata(const URigHierarchy* Hierarchy, const FRigElementKey& Key)
{
	TArray<FString> Values;
	if (const FRigBaseElement* Element = Hierarchy->Find(Key))
	{
		TArray<FName> Names = Hierarchy->GetMetadataNames(Key);
		Names.Sort(FNameLexicalLess());
		for (const FName Name : Names)
		{
			const ERigMetadataType Type = Hierarchy->GetMetadataType(Key, Name);
			if (const FRigBaseMetadata* Metadata = Hierarchy->FindMetadataForElement(Element, Name, Type))
			{
				Values.Add(TEXT("(") + QuoteValue(Name.ToString()) + TEXT(" ")
					+ QuoteValue(StaticEnum<ERigMetadataType>()->GetNameStringByValue(static_cast<int64>(Type)))
					+ TEXT(" ") + QuoteValue(ExportStruct(Metadata->GetMetadataStruct(), Metadata)) + TEXT(")"));
			}
		}
	}
	return TEXT("(") + FString::Join(Values, TEXT(" ")) + TEXT(")");
}

void TestTypedMetadataExact(FAutomationTestBase& Test, const FRigHierarchyMetadataAST& Typed, const FRigBaseMetadata* Source)
{
	Test.TestEqual(TEXT("Typed metadata name is exact"), Typed.Name, Source->GetName().ToString());
	switch (Source->GetType())
	{
	case ERigMetadataType::Bool: Test.TestEqual(TEXT("Typed bool metadata is exact"), Typed.BoolValues[0], CastChecked<FRigBoolMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::BoolArray: Test.TestTrue(TEXT("Typed bool-array metadata is exact"), Typed.BoolValues == CastChecked<FRigBoolArrayMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::Float: Test.TestEqual(TEXT("Typed float metadata is exact"), Typed.NumberValues[0], static_cast<double>(CastChecked<FRigFloatMetadata>(Source)->GetValue())); break;
	case ERigMetadataType::FloatArray:
	{
		const TArray<float>& Values=CastChecked<FRigFloatArrayMetadata>(Source)->GetValue(); Test.TestEqual(TEXT("Typed float-array count"),Typed.NumberValues.Num(),Values.Num());
		for(int32 I=0;I<Values.Num()&&Typed.NumberValues.IsValidIndex(I);++I)Test.TestEqual(TEXT("Typed float-array value"),Typed.NumberValues[I],static_cast<double>(Values[I])); break;
	}
	case ERigMetadataType::Int32: Test.TestEqual(TEXT("Typed int metadata is exact"), Typed.IntegerValues[0], static_cast<int64>(CastChecked<FRigInt32Metadata>(Source)->GetValue())); break;
	case ERigMetadataType::Int32Array:
	{
		const TArray<int32>& Values=CastChecked<FRigInt32ArrayMetadata>(Source)->GetValue(); Test.TestEqual(TEXT("Typed int-array count"),Typed.IntegerValues.Num(),Values.Num());
		for(int32 I=0;I<Values.Num()&&Typed.IntegerValues.IsValidIndex(I);++I)Test.TestEqual(TEXT("Typed int-array value"),Typed.IntegerValues[I],static_cast<int64>(Values[I])); break;
	}
	case ERigMetadataType::Name: Test.TestEqual(TEXT("Typed name metadata is exact"),Typed.StringValues[0],CastChecked<FRigNameMetadata>(Source)->GetValue().ToString()); break;
	case ERigMetadataType::NameArray:
	{
		const TArray<FName>& Values=CastChecked<FRigNameArrayMetadata>(Source)->GetValue(); Test.TestEqual(TEXT("Typed name-array count"),Typed.StringValues.Num(),Values.Num());
		for(int32 I=0;I<Values.Num()&&Typed.StringValues.IsValidIndex(I);++I)Test.TestEqual(TEXT("Typed name-array value"),Typed.StringValues[I],Values[I].ToString()); break;
	}
	case ERigMetadataType::Vector: Test.TestEqual(TEXT("Typed vector metadata is exact"),Typed.VectorValues[0],CastChecked<FRigVectorMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::VectorArray: Test.TestTrue(TEXT("Typed vector-array metadata is exact"),Typed.VectorValues==CastChecked<FRigVectorArrayMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::Rotator: Test.TestEqual(TEXT("Typed rotator metadata is exact"),Typed.RotatorValues[0],CastChecked<FRigRotatorMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::RotatorArray: Test.TestTrue(TEXT("Typed rotator-array metadata is exact"),Typed.RotatorValues==CastChecked<FRigRotatorArrayMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::Quat: Test.TestEqual(TEXT("Typed quat metadata is exact"),Typed.QuatValues[0],CastChecked<FRigQuatMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::QuatArray: Test.TestTrue(TEXT("Typed quat-array metadata is exact"),Typed.QuatValues==CastChecked<FRigQuatArrayMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::Transform: Test.TestEqual(TEXT("Typed transform metadata is exact"),Typed.TransformValues[0],CastChecked<FRigTransformMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::TransformArray:
	{
		const TArray<FTransform>& Values=CastChecked<FRigTransformArrayMetadata>(Source)->GetValue(); Test.TestEqual(TEXT("Typed transform-array count"),Typed.TransformValues.Num(),Values.Num());
		for(int32 I=0;I<Values.Num()&&Typed.TransformValues.IsValidIndex(I);++I)Test.TestTrue(TEXT("Typed transform-array value"),Typed.TransformValues[I].Equals(Values[I],0.0)); break;
	}
	case ERigMetadataType::LinearColor: Test.TestEqual(TEXT("Typed color metadata is exact"),Typed.ColorValues[0],CastChecked<FRigLinearColorMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::LinearColorArray: Test.TestTrue(TEXT("Typed color-array metadata is exact"),Typed.ColorValues==CastChecked<FRigLinearColorArrayMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::RigElementKey: Test.TestEqual(TEXT("Typed element-key metadata is exact"),Typed.StringValues[0],CastChecked<FRigElementKeyMetadata>(Source)->GetValue().ToString()); break;
	case ERigMetadataType::RigElementKeyArray:
	{
		const TArray<FRigElementKey>& Values=CastChecked<FRigElementKeyArrayMetadata>(Source)->GetValue(); Test.TestEqual(TEXT("Typed element-key-array count"),Typed.StringValues.Num(),Values.Num());
		for(int32 I=0;I<Values.Num()&&Typed.StringValues.IsValidIndex(I);++I)Test.TestEqual(TEXT("Typed element-key-array value"),Typed.StringValues[I],Values[I].ToString()); break;
	}
	default: break;
	}
}

FString StableRuntimeSymbol(const FString& Value)
{
	FString Symbol;
	for (const TCHAR Character : Value)
	{
		if (FChar::IsAlnum(Character) || Character == TEXT('_')) Symbol.AppendChar(Character);
	}
	if (Symbol.IsEmpty()) Symbol = TEXT("Unnamed");
	if (FChar::IsDigit(Symbol[0])) Symbol = TEXT("_") + Symbol;
	return Symbol;
}

FString ExportStruct(const UScriptStruct* Struct, const void* Value)
{
	FString Text;
	Struct->ExportText(Text, Value, Value, nullptr, PPF_None, nullptr);
	return Text;
}

FString ExportTransform(const FTransform& Transform)
{
	return ExportStruct(TBaseStructure<FTransform>::Get(), &Transform);
}

UControlRigBlueprint* MakeSyntheticRig()
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	const FName BlueprintName = MakeUniqueObjectName(
		GetTransientPackage(), UControlRigBlueprint::StaticClass(), TEXT("CR_RigLangExporterSynthetic"));
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), GetTransientPackage(),
		BlueprintName, RF_Transient, nullptr, GWarn));
	if (!Blueprint)
	{
		return nullptr;
	}
	URigHierarchyController* HierarchyController = Blueprint->GetHierarchyController();
	FRigControlSettings Settings;
	Settings.ControlType = ERigControlType::Float;
	const FRigElementKey FootControl = HierarchyController->AddControl(
		TEXT("FootControl"), FRigElementKey(), Settings, FRigControlValue::Make(0.25f),
		FTransform::Identity, FTransform::Identity, false);
	const FRigElementKey Root = HierarchyController->AddBone(
		TEXT("Root"), FRigElementKey(),
		FTransform(FVector(10.123456789, 20.987654321, 30.111111111)),
		true, ERigBoneType::User, false);
	Blueprint->GetHierarchy()->SetBoolMetadata(Root, TEXT("ExportTag"), true);
	const FRigElementKey ParentB = HierarchyController->AddBone(
		TEXT("ParentB"), FRigElementKey(), FTransform::Identity,
		true, ERigBoneType::User, false);
	HierarchyController->AddParent(FootControl, Root, 0.75f, false, TEXT("RootSpace"), false);
	HierarchyController->AddParent(FootControl, ParentB, 0.25f, false, TEXT("ParentBSpace"), false);
	URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
	Hierarchy->SetParentWeight(FootControl, Root, FRigElementWeight(0.7f, 0.6f, 0.5f), false, false);
	Hierarchy->SetParentWeight(FootControl, Root, FRigElementWeight(0.4f, 0.3f, 0.2f), true, false);
	Hierarchy->SetControlValue(FootControl, FRigControlValue::Make(0.75f), ERigControlValueType::Current);
	Hierarchy->SetControlValue(FootControl, FRigControlValue::Make(0.25f), ERigControlValueType::Initial);
	Hierarchy->SetControlValue(FootControl, FRigControlValue::Make(-1.0f), ERigControlValueType::Minimum);
	Hierarchy->SetControlValue(FootControl, FRigControlValue::Make(2.0f), ERigControlValueType::Maximum);
	Hierarchy->SetControlOffsetTransform(FootControl, FTransform(FVector(1.0, 2.0, 3.0)), false, false);
	Hierarchy->SetControlOffsetTransform(FootControl, FTransform(FVector(4.0, 5.0, 6.0)), true, false);
	Hierarchy->SetControlShapeTransform(FootControl, FTransform(FVector(7.0, 8.0, 9.0)), false);
	Hierarchy->SetControlShapeTransform(FootControl, FTransform(FVector(10.0, 11.0, 12.0)), true);
	if (FRigControlElement* MutableControl = Hierarchy->Find<FRigControlElement>(FootControl))
	{
		MutableControl->PreferredEulerAngles.RotationOrder = EEulerRotationOrder::ZYX;
		MutableControl->PreferredEulerAngles.Current = FVector(13.0, 14.0, 15.0);
		MutableControl->PreferredEulerAngles.Initial = FVector(16.0, 17.0, 18.0);
	}
	HierarchyController->AddCurve(TEXT("TestCurve"), 0.625f, false);

	Blueprint->AddMemberVariable(TEXT("bEnabled"), TEXT("bool"), true, false, TEXT("True"));

	FRigVMClient* RigVMClient = Blueprint->URigVMBlueprint::GetRigVMClient();
	URigVMGraph* ForwardsGraph = RigVMClient->GetDefaultModel();
	if (!ForwardsGraph)
	{
		ForwardsGraph = RigVMClient->AddModel(TEXT("RigVMModel"), false);
	}
	URigVMController* ForwardsController = Blueprint->GetOrCreateController(ForwardsGraph);
	ForwardsController->AddLocalVariable(TEXT("UnusedLocal"), TEXT("float"), nullptr, TEXT("3.5"), false, false);
	ForwardsController->AddLocalVariable(TEXT("SecondLocal"), TEXT("int32"), nullptr, TEXT("7"), false, false);
	ForwardsController->AddUnitNode(
		FRigUnit_BeginExecution::StaticStruct(), FRigUnit::GetMethodName(),
		FVector2D::ZeroVector, TEXT("ForwardsSolve"), false);

	URigVMGraph* ConstructionGraph = RigVMClient->AddModel(TEXT("ConstructionGraph"), false);
	URigVMController* ConstructionController = Blueprint->GetOrCreateController(ConstructionGraph);
	ConstructionController->AddUnitNode(
		FRigUnit_PrepareForExecution::StaticStruct(), FRigUnit::GetMethodName(),
		FVector2D::ZeroVector, TEXT("Construction"), false);
	return Blueprint;
}

ERigPinDirection ExpectedPinDirection(const ERigVMPinDirection Direction)
{
	switch (Direction)
	{
	case ERigVMPinDirection::Output: return ERigPinDirection::Output;
	case ERigVMPinDirection::IO: return ERigPinDirection::IO;
	case ERigVMPinDirection::Visible: return ERigPinDirection::Visible;
	case ERigVMPinDirection::Hidden: return ERigPinDirection::Hidden;
	case ERigVMPinDirection::Invalid: return ERigPinDirection::Invalid;
	default: return ERigPinDirection::Input;
	}
}

FString RelativePinPath(const URigVMPin* Pin)
{
	FString Path = Pin->GetPinPath(false);
	if (const URigVMNode* Node = Pin->GetNode())
	{
		Path.RemoveFromStart(Node->GetNodePath(false) + TEXT("."));
	}
	return Path;
}

void ComparePinTree(FAutomationTestBase& Test, const URigVMPin* Source, const FRigPinAST& Exported)
{
	const FString Context = Source->GetPinPath(true);
	Test.TestEqual(Context + TEXT(" path"), Exported.Path, RelativePinPath(Source));
	Test.TestEqual(Context + TEXT(" direction"), Exported.Direction, ExpectedPinDirection(Source->GetDirection()));
	Test.TestEqual(Context + TEXT(" CPP type"), Exported.Type.CPPType, Source->GetCPPType());
	Test.TestEqual(Context + TEXT(" CPP type object"), Exported.Type.CPPTypeObject,
		Source->GetCPPTypeObject() ? Source->GetCPPTypeObject()->GetPathName() : FString());
	Test.TestEqual(Context + TEXT(" container"), Exported.Type.ContainerType,
		Source->IsArray() ? FString(TEXT("array")) : FString());
	Test.TestEqual(Context + TEXT(" default"), Exported.DefaultValue, Source->GetDefaultValue());
	Test.TestEqual(Context + TEXT(" execute context"), Exported.bExecuteContext, Source->IsExecuteContext());
	Test.TestEqual(Context + TEXT(" subpin count"), Exported.SubPins.Num(), Source->GetSubPins().Num());
	for (const URigVMPin* SourceSubPin : Source->GetSubPins())
	{
		if (!SourceSubPin) continue;
		const FRigPinAST* ExportedSubPin = Exported.SubPins.FindByPredicate(
			[SourceSubPin](const FRigPinAST& Pin) { return Pin.Path == RelativePinPath(SourceSubPin); });
		if (Test.TestNotNull(Context + TEXT(" exported subpin"), ExportedSubPin))
		{
			ComparePinTree(Test, SourceSubPin, *ExportedSubPin);
		}
	}
}

const FRigGraphAST* FindGraph(const FRigModuleAST& Module, const FString& ModelId)
{
	if (const FRigGraphAST* Graph = Module.Graphs.FindByPredicate(
		[&ModelId](const FRigGraphAST& Item) { return Item.StableId == ModelId; }))
	{
		return Graph;
	}
	if (const FRigEntryAST* Entry = Module.Entries.FindByPredicate(
		[&ModelId](const FRigEntryAST& Item) { return Item.StableId == ModelId; }))
	{
		return &Entry->Graph;
	}
	if (const FRigFunctionAST* Function = Module.Functions.FindByPredicate(
		[&ModelId](const FRigFunctionAST& Item) { return Item.StableId == ModelId; }))
	{
		return &Function->Graph;
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangExporterSHA256Test,
	"AnimBP2FP.RigLang.Exporter.SHA256KnownVector",
	RigLangExporterTests::TestFlags)

bool FRigLangExporterSHA256Test::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("SHA-256 abc known vector"),
		FRigLangExporter::ComputeContentHash(TEXT("abc")),
		TEXT("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	TestEqual(
		TEXT("Deterministic editor GUID known vector"),
		FRigLangExporter::ComputeDeterministicEditorGuid(TEXT("/Game/Rigs/Vector"), TEXT("RigVMModel")),
		TEXT("ea377a0f-8a78-c92e-95ee-f935a644701c"));
	const FString FirstCollisionInput = FRigLangExporter::ComputeDeterministicEditorGuid(TEXT("ab"), TEXT("c"));
	const FString SecondCollisionInput = FRigLangExporter::ComputeDeterministicEditorGuid(TEXT("a"), TEXT("bc"));
	TestNotEqual(TEXT("Length-delimited identities prevent concatenation collisions"),
		FirstCollisionInput, SecondCollisionInput);
	FGuid ParsedGuid;
	TestTrue(TEXT("Deterministic editor GUID is a valid nonzero canonical GUID"),
		FGuid::ParseExact(FirstCollisionInput, EGuidFormats::DigitsWithHyphens, ParsedGuid)
			&& ParsedGuid.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangExporterSyntheticTest,
	"AnimBP2FP.RigLang.Exporter.Synthetic",
	RigLangExporterTests::TestFlags)

bool FRigLangExporterSyntheticTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* Blueprint = RigLangExporterTests::MakeSyntheticRig();
	TestNotNull(TEXT("Synthetic Control Rig is created"), Blueprint);

	FRigLangExportOptions Options;
	Options.bStrict = true;
	const FRigLangExportResult Result = FRigLangExporter::Export(Blueprint, Options);
	TestTrue(TEXT("Strict synthetic export succeeds"), Result.bSuccess);
	if (!TestNotNull(TEXT("Synthetic export produces a module"), Result.Module.Get()))
	{
		return false;
	}

	TestEqual(TEXT("All hierarchy elements export"), Result.Module->Hierarchy.Num(), 4);
	TestEqual(TEXT("Public variable exports"), Result.Module->Variables.Num(), 1);
	const FRigVariableAST& Variable = Result.Module->Variables[0];
	TestEqual(TEXT("Public variable access exports"), Variable.Access, ERigVariableAccess::PublicInput);
	TestEqual(TEXT("Public variable exact CPP type exports"), Variable.Type.CPPType, TEXT("bool"));
	TestEqual(TEXT("Public variable default exports"), Variable.DefaultValue, TEXT("True"));

	const FRigHierarchyElementAST* Root = Result.Module->Hierarchy.FindByPredicate(
		[](const FRigHierarchyElementAST& Element) { return Element.Name == TEXT("Root"); });
	const FRigHierarchyElementAST* Control = Result.Module->Hierarchy.FindByPredicate(
		[](const FRigHierarchyElementAST& Element) { return Element.Name == TEXT("FootControl"); });
	const FRigHierarchyElementAST* Curve = Result.Module->Hierarchy.FindByPredicate(
		[](const FRigHierarchyElementAST& Element) { return Element.Name == TEXT("TestCurve"); });
	TMap<FString, int32> HierarchyOrder;
	for (int32 Index = 0; Index < Result.Module->Hierarchy.Num(); ++Index)
		HierarchyOrder.Add(Result.Module->Hierarchy[Index].StableId, Index);
	for (const FRigHierarchyElementAST& Element : Result.Module->Hierarchy)
	{
		const FRigElementKey Key(FName(*Element.Name), static_cast<ERigElementType>(Element.Kind == ERigHierarchyElementKind::Bone
			? ERigElementType::Bone : Element.Kind == ERigHierarchyElementKind::Control
			? ERigElementType::Control : Element.Kind == ERigHierarchyElementKind::Null
			? ERigElementType::Null : ERigElementType::Curve));
		for (const FRigElementKey& Parent : Blueprint->GetHierarchy()->GetParents(Key, false))
		{
			TestTrue(TEXT("Hierarchy is exported parent-before-child"),
				HierarchyOrder.FindRef(Parent.ToString()) < HierarchyOrder.FindRef(Element.StableId));
		}
	}
	TestEqual(TEXT("Ready root bones use stable lexical order"), Result.Module->Hierarchy[0].Name, TEXT("ParentB"));
	TestEqual(TEXT("Second ready root bone is stable"), Result.Module->Hierarchy[1].Name, TEXT("Root"));
	if (TestNotNull(TEXT("Exported root exists"), Root))
	{
		const FRigElementKey RootKey(TEXT("Root"), ERigElementType::Bone);
		const FRigBaseElement* SourceRoot = Blueprint->GetHierarchy()->Find(RootKey);
		TestTrue(TEXT("Root has no typed parents"), Root->Parents.IsEmpty());
		const FRigHierarchyStateAST* BoneState = Root->States.FindByPredicate([](const FRigHierarchyStateAST& State)
		{
			return State.Kind == ERigHierarchyStateKind::BoneType;
		});
		if (TestNotNull(TEXT("Typed bone state exists"), BoneState))
			TestEqual(TEXT("Typed bone type reconstructs exactly"), BoneState->Type, TEXT("User"));
		const FRigBaseMetadata* SourceMetadata = SourceRoot
			? Blueprint->GetHierarchy()->FindMetadataForElement(
				SourceRoot, TEXT("ExportTag"), ERigMetadataType::Bool)
			: nullptr;
		if (TestNotNull(TEXT("Synthetic source metadata exists"), SourceMetadata))
		{
			const FRigHierarchyMetadataAST* TypedMetadata = Root->Metadata.FindByPredicate([](const FRigHierarchyMetadataAST& Metadata)
			{
				return Metadata.Name == TEXT("ExportTag");
			});
			if (TestNotNull(TEXT("Typed synthetic metadata exists"), TypedMetadata))
				RigLangExporterTests::TestTypedMetadataExact(*this, *TypedMetadata, SourceMetadata);
		}
		const FTransform SourceTransform = Blueprint->GetHierarchy()->GetInitialLocalTransform(
			RootKey);
		const FRigHierarchyTransformAST* InitialLocal = Root->Transforms.FindByPredicate([](const FRigHierarchyTransformAST& Transform)
		{
			return Transform.Role == ERigHierarchyTransformRole::InitialLocal;
		});
		if (TestNotNull(TEXT("Typed initial local transform exists"), InitialLocal))
			TestTrue(TEXT("Typed initial local transform is exact"), InitialLocal->Value.Equals(SourceTransform, 0.0));
	}
	if (TestNotNull(TEXT("Exported control exists"), Control))
	{
		const FRigControlElement* SourceControl = Blueprint->GetHierarchy()->Find<FRigControlElement>(
			FRigElementKey(TEXT("FootControl"), ERigElementType::Control));
		if (TestNotNull(TEXT("Synthetic source control exists"), SourceControl))
		{
			const FRigHierarchyStateAST* SettingsState = Control->States.FindByPredicate([](const FRigHierarchyStateAST& State)
			{
				return State.Kind == ERigHierarchyStateKind::ControlSettings;
			});
			if (TestNotNull(TEXT("Typed control settings state exists"), SettingsState))
			{
				TestEqual(TEXT("Typed control type is exact"), SettingsState->Type, TEXT("Float"));
				TestEqual(TEXT("Complete typed control settings export"), SettingsState->SerializedValue,
					RigLangExporterTests::ExportStruct(FRigControlSettings::StaticStruct(), &SourceControl->Settings));
			}
		}
		TestEqual(TEXT("Typed parent constraint count"), Control->Parents.Num(), 2);
		if (Control->Parents.Num() == 2)
		{
			TestEqual(TEXT("Typed current location weight is non-uniform and exact"), Control->Parents[0].CurrentWeight.Location, 0.7);
			TestEqual(TEXT("Typed current rotation weight is non-uniform and exact"), Control->Parents[0].CurrentWeight.Rotation, 0.6);
			TestEqual(TEXT("Typed current scale weight is non-uniform and exact"), Control->Parents[0].CurrentWeight.Scale, 0.5);
			TestEqual(TEXT("Typed initial location weight is exact"), Control->Parents[0].InitialWeight.Location, 0.4);
			TestEqual(TEXT("Typed initial rotation weight is exact"), Control->Parents[0].InitialWeight.Rotation, 0.3);
			TestEqual(TEXT("Typed initial scale weight is exact"), Control->Parents[0].InitialWeight.Scale, 0.2);
			TestEqual(TEXT("Typed parent label is exact"), Control->Parents[0].Label, TEXT("RootSpace"));
		}
		const FRigElementKey ControlKey(TEXT("FootControl"), ERigElementType::Control);
		const FRigControlValue CurrentValue = Blueprint->GetHierarchy()->GetControlValue(ControlKey, ERigControlValueType::Current);
		const FRigControlValue InitialValue = Blueprint->GetHierarchy()->GetControlValue(ControlKey, ERigControlValueType::Initial);
		const FRigControlValue MinimumValue = Blueprint->GetHierarchy()->GetControlValue(ControlKey, ERigControlValueType::Minimum);
		const FRigControlValue MaximumValue = Blueprint->GetHierarchy()->GetControlValue(ControlKey, ERigControlValueType::Maximum);
		auto TestFloatControlValue = [this, Control](const TCHAR* Label, const FString& Role, const FRigControlValue& Expected)
		{
			const FRigHierarchyStateAST* State = Control->States.FindByPredicate([&Role](const FRigHierarchyStateAST& Candidate)
			{
				return Candidate.Kind == ERigHierarchyStateKind::ControlValue && Candidate.Role == Role;
			});
			if (TestNotNull(Label, State)) TestEqual(Label, State->NumberValue, static_cast<double>(Expected.Get<float>()));
		};
		TestFloatControlValue(TEXT("Current typed control value reconstructs exactly"), TEXT("current"), CurrentValue);
		TestFloatControlValue(TEXT("Initial typed control value reconstructs exactly"), TEXT("initial"), InitialValue);
		TestFloatControlValue(TEXT("Minimum typed control value reconstructs exactly"), TEXT("minimum"), MinimumValue);
		TestFloatControlValue(TEXT("Maximum typed control value reconstructs exactly"), TEXT("maximum"), MaximumValue);
		auto TestTypedTransform = [this, Control](const TCHAR* Label, ERigHierarchyTransformRole Role, const FTransform& Expected)
		{
			const FRigHierarchyTransformAST* Transform = Control->Transforms.FindByPredicate([Role](const FRigHierarchyTransformAST& Candidate)
			{
				return Candidate.Role == Role;
			});
			if (TestNotNull(Label, Transform)) TestTrue(Label, Transform->Value.Equals(Expected, 0.0));
		};
		TestTypedTransform(TEXT("Current local control offset reconstructs exactly"), ERigHierarchyTransformRole::OffsetCurrentLocal,
			SourceControl->GetOffsetTransform()[ERigTransformType::CurrentLocal].Get());
		TestTypedTransform(TEXT("Initial global control shape reconstructs exactly"), ERigHierarchyTransformRole::ShapeInitialGlobal,
			Blueprint->GetHierarchy()->GetGlobalControlShapeTransform(ControlKey, true));
		for (const TPair<FString, FVector> Expected : { TPair<FString, FVector>(TEXT("current"), SourceControl->PreferredEulerAngles.Current), TPair<FString, FVector>(TEXT("initial"), SourceControl->PreferredEulerAngles.Initial) })
		{
			const FRigHierarchyStateAST* Euler = Control->States.FindByPredicate([&Expected](const FRigHierarchyStateAST& State)
			{
				return State.Kind == ERigHierarchyStateKind::PreferredEuler && State.Role == Expected.Key;
			});
			if (TestNotNull(TEXT("Preferred Euler vector state exists"), Euler))
			{
				TestEqual(TEXT("Preferred Euler order reconstructs exactly"), Euler->Type, TEXT("ZYX"));
				TestTrue(TEXT("Preferred Euler vector reconstructs exactly"), Euler->Components == TArray<double>{Expected.Value.X, Expected.Value.Y, Expected.Value.Z});
			}
		}
	}
	TArray<FRigLangParseError> TypedRoundTripErrors;
	const TSharedPtr<FRigModuleAST> TypedRoundTrip = FRigLangParser::Parse(
		Result.Module->ToCanonicalString(), TEXT("SyntheticTypedHierarchy.riglang"), TypedRoundTripErrors);
	TestTrue(TEXT("Synthetic typed hierarchy canonical parses"), TypedRoundTripErrors.IsEmpty());
	if (TestNotNull(TEXT("Synthetic typed hierarchy round-trip module"), TypedRoundTrip.Get()))
	{
		const FRigHierarchyElementAST* ParsedControl = TypedRoundTrip->Hierarchy.FindByPredicate(
			[](const FRigHierarchyElementAST& Element) { return Element.Name == TEXT("FootControl"); });
		if (TestNotNull(TEXT("Parsed typed control exists"), ParsedControl))
		{
			TestEqual(TEXT("Parsed non-uniform location weight"), ParsedControl->Parents[0].CurrentWeight.Location, 0.7);
			TestEqual(TEXT("Parsed non-uniform rotation weight"), ParsedControl->Parents[0].CurrentWeight.Rotation, 0.6);
			TestEqual(TEXT("Parsed non-uniform scale weight"), ParsedControl->Parents[0].CurrentWeight.Scale, 0.5);
		}
	}
	if (TestNotNull(TEXT("Exported curve exists"), Curve))
	{
		const FRigElementKey CurveKey(TEXT("TestCurve"), ERigElementType::Curve);
		const FRigHierarchyStateAST* CurveState = Curve->States.FindByPredicate([](const FRigHierarchyStateAST& State)
		{
			return State.Kind == ERigHierarchyStateKind::Curve;
		});
		if (TestNotNull(TEXT("Typed curve state exists"), CurveState))
		{
			TestEqual(TEXT("Curve value reconstructs exactly"), CurveState->NumberValue,
				static_cast<double>(Blueprint->GetHierarchy()->GetCurveValue(CurveKey)));
			TestEqual(TEXT("Curve set state reconstructs exactly"), CurveState->bBoolValue,
				Blueprint->GetHierarchy()->IsCurveValueSet(CurveKey));
		}
		TestTrue(TEXT("Empty metadata collection is represented by an empty typed array"), Curve->Metadata.IsEmpty());
	}
	TestEqual(TEXT("Construction and Forwards Solve export as entries"), Result.Module->Entries.Num(), 2);
	int32 ModelOnlyUnitNodes = 0;
	for (const FRigGraphAST& Graph : Result.Module->Graphs)
	{
		for (const FRigNodeAST& Node : Graph.Nodes)
		{
			if (!Node.MethodName.IsEmpty())
			{
				TestTrue(TEXT("Model-only unit node uses an explicit non-GUID fallback"),
					Node.Guid.StartsWith(TEXT("model:")));
				if (Node.Guid.StartsWith(TEXT("model:"))) ++ModelOnlyUnitNodes;
				TestEqual(TEXT("Unit node preserves its UObject class"),
					Node.ClassPath, URigVMUnitNode::StaticClass()->GetPathName());
				TestTrue(TEXT("Unit node exports its script struct separately"),
					Node.Properties.Contains(TEXT("script-struct")));
			}
		}
	}
	TestEqual(TEXT("Both transient synthetic units are classified as model-only"), ModelOnlyUnitNodes, 2);
	TestEqual(TEXT("Every synthetic model is accounted for"), Result.Coverage.ModelTotal, Blueprint->GetAllModels().Num());
	TestEqual(TEXT("Every synthetic node is accounted for"), Result.Coverage.NodeTotal, 2);
	TestEqual(TEXT("Synthetic models export as authoritative shared graphs"),
		Result.Module->Graphs.Num(), Blueprint->GetAllModels().Num());
	for (const FRigEntryAST& Entry : Result.Module->Entries)
	{
		TestFalse(TEXT("Entry references an authoritative graph"), Entry.GraphStableId.IsEmpty());
	}
	int32 RecursivePinCount = 0;
	int32 ExecuteContextPinCount = 0;
	TFunction<void(const FRigPinAST&)> InspectPin = [&RecursivePinCount, &ExecuteContextPinCount, &InspectPin](
		const FRigPinAST& Pin)
	{
		++RecursivePinCount;
		if (Pin.bExecuteContext) ++ExecuteContextPinCount;
		for (const FRigPinAST& SubPin : Pin.SubPins) InspectPin(SubPin);
	};
	for (const FRigGraphAST& Graph : Result.Module->Graphs)
	{
		for (const FRigNodeAST& Node : Graph.Nodes)
		{
			for (const FRigPinAST& Pin : Node.Pins) InspectPin(Pin);
		}
	}
	TestEqual(TEXT("Recursive pin and subpin inventory is exact"), RecursivePinCount, Result.Coverage.PinTotal);
	TestTrue(TEXT("Execute-context pins preserve their semantic type"), ExecuteContextPinCount >= 2);

	const FString Canonical = Result.Module->ToCanonicalString();
	TArray<FRigLangParseError> RoundTripErrors;
	const TSharedPtr<FRigModuleAST> RoundTripped = FRigLangParser::Parse(
		Canonical, TEXT("CR_RigLangExporterSynthetic.riglang"), RoundTripErrors);
	TestTrue(TEXT("Synthetic canonical properties reparse"), RoundTripErrors.IsEmpty());
	if (TestNotNull(TEXT("Synthetic canonical properties produce an AST"), RoundTripped.Get()))
	{
		TestEqual(TEXT("Hierarchy raw properties round-trip byte-for-byte"),
			RoundTripped->ToCanonicalString(), Canonical);
		const FRigHierarchyElementAST* ParsedRoot = RoundTripped->Hierarchy.FindByPredicate(
			[](const FRigHierarchyElementAST& Element) { return Element.Name == TEXT("Root"); });
		if (TestNotNull(TEXT("Parsed high-precision root exists"), ParsedRoot))
		{
			const FTransform SourceTransform = Blueprint->GetHierarchy()->GetInitialLocalTransform(
				FRigElementKey(TEXT("Root"), ERigElementType::Bone));
			const FRigHierarchyTransformAST* ParsedTransform = ParsedRoot->Transforms.FindByPredicate([](const FRigHierarchyTransformAST& Transform)
			{
				return Transform.Role == ERigHierarchyTransformRole::InitialLocal;
			});
			if (TestNotNull(TEXT("Parsed typed transform exists"), ParsedTransform))
				TestTrue(TEXT("Parsed transform equals exact source value"), ParsedTransform->Value.Equals(SourceTransform, 0.0));
		}
		const FRigHierarchyElementAST* ParsedControl = RoundTripped->Hierarchy.FindByPredicate(
			[](const FRigHierarchyElementAST& Element) { return Element.Name == TEXT("FootControl"); });
		if (TestNotNull(TEXT("Parsed multi-parent control exists"), ParsedControl))
		{
			TestEqual(TEXT("Parsed control preserves both direct parents exactly"), ParsedControl->Parents.Num(), Control->Parents.Num());
			for (int32 ParentIndex = 0; ParentIndex < ParsedControl->Parents.Num() && Control->Parents.IsValidIndex(ParentIndex); ++ParentIndex)
				TestEqual(TEXT("Parsed typed parent ID is exact"), ParsedControl->Parents[ParentIndex].StableId, Control->Parents[ParentIndex].StableId);
			const FRigHierarchyStateAST* ParsedSettings = ParsedControl->States.FindByPredicate([](const FRigHierarchyStateAST& State)
			{
				return State.Kind == ERigHierarchyStateKind::ControlSettings;
			});
			if (TestNotNull(TEXT("Parsed typed control settings exist"), ParsedSettings))
			{
				FRigControlSettings ReconstructedSettings;
				const UScriptStruct* SettingsStruct = FRigControlSettings::StaticStruct();
				const TCHAR* Remainder = SettingsStruct->ImportText(*ParsedSettings->SerializedValue,
					&ReconstructedSettings, nullptr, PPF_None, nullptr, TEXT("FRigControlSettings"));
				TestNotNull(TEXT("Parsed typed control settings payload reconstructs"), Remainder);
				const FRigControlElement* SourceRoundTripControl = Blueprint->GetHierarchy()->Find<FRigControlElement>(
					FRigElementKey(TEXT("FootControl"), ERigElementType::Control));
				if (SourceRoundTripControl)
					TestTrue(TEXT("Parsed typed control settings are structurally exact"),
						SettingsStruct->CompareScriptStruct(&ReconstructedSettings, &SourceRoundTripControl->Settings, PPF_None));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangExporterCommentHashNormalizationTest,
	"AnimBP2FP.RigLang.Exporter.CommentHashNormalization",
	RigLangExporterTests::TestFlags)

bool FRigLangExporterCommentHashNormalizationTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* Blueprint = RigLangExporterTests::MakeSyntheticRig();
	if (!TestNotNull(TEXT("Comment hash synthetic rig is created"), Blueprint)) return false;
	const FRigLangExportResult Before = FRigLangExporter::Export(Blueprint);
	if (!TestTrue(TEXT("Baseline comment hash export succeeds"), Before.bSuccess)) return false;
	URigVMGraph* Graph = Blueprint->URigVMBlueprint::GetRigVMClient()->GetDefaultModel();
	URigVMController* Controller = Blueprint->GetOrCreateController(Graph);
	URigVMCommentNode* Comment = Controller->AddCommentNode(
		TEXT("Editor-only note"), FVector2D(100.0, 200.0), FVector2D(300.0, 150.0),
		FLinearColor::Red, TEXT("HashExcludedComment"), false);
	if (!TestNotNull(TEXT("Editor-only comment is created"), Comment)) return false;
	const FRigLangExportResult After = FRigLangExporter::Export(Blueprint);
	TestTrue(TEXT("Strict export visits a normalized comment"), After.bSuccess);
	TestEqual(TEXT("Comment-only edit does not change semantic content hash"),
		After.Module->Header.ContentHash, Before.Module->Header.ContentHash);
	TestTrue(TEXT("Canonical display output retains the comment"),
		After.Module->ToCanonicalString().Contains(TEXT("rig-comment")));
	const FString DisplayBeforeMutation = After.Module->ToCanonicalString();
	const FString HashInputBeforeMutation = After.Module->ToCanonicalHashInput();
	FRigNodeAST* ExportedComment = nullptr;
	for (FRigGraphAST& ExportedGraph : After.Module->Graphs)
	{
		ExportedComment = ExportedGraph.Nodes.FindByPredicate(
			[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Comment; });
		if (ExportedComment) break;
	}
	if (TestNotNull(TEXT("Exported comment AST is available for hash normalization"), ExportedComment))
	{
		ExportedComment->Properties.Add(TEXT("comment-text"), RigLangExporterTests::QuoteValue(TEXT("Changed note")));
		TestNotEqual(TEXT("Changing only comment content changes display canonical text"),
			After.Module->ToCanonicalString(), DisplayBeforeMutation);
		TestEqual(TEXT("Changing only comment content leaves canonical hash input stable"),
			After.Module->ToCanonicalHashInput(), HashInputBeforeMutation);
	}
	TestTrue(TEXT("Comment coverage records its normalization reason"),
		After.Coverage.Reasons.FindRef(Comment->GetPathName()).Contains(TEXT("excluded from semantic hash")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangExporterCurrentPoseHashNormalizationTest,
	"AnimBP2FP.RigLang.Exporter.CurrentPoseHashNormalization",
	RigLangExporterTests::TestFlags)

bool FRigLangExporterCurrentPoseHashNormalizationTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* Blueprint = RigLangExporterTests::MakeSyntheticRig();
	if (!TestNotNull(TEXT("Current-pose synthetic rig is created"), Blueprint)) return false;
	const FRigLangExportResult Result = FRigLangExporter::Export(Blueprint);
	if (!TestTrue(TEXT("Current-pose baseline export succeeds"), Result.bSuccess)
		|| !TestNotNull(TEXT("Current-pose module exists"), Result.Module.Get())) return false;
	FRigHierarchyElementAST* Control = Result.Module->Hierarchy.FindByPredicate(
		[](const FRigHierarchyElementAST& Element) { return Element.Name == TEXT("FootControl"); });
	if (!TestNotNull(TEXT("Current-pose control exists"), Control)) return false;
	const FString DisplayBefore = Result.Module->ToCanonicalString();
	const FString HashBefore = Result.Module->ToCanonicalHashInput();
	Control->Parents[0].CurrentWeight.Location += 0.123;
	Control->Properties.Add(TEXT("current-local-transform"), TEXT("\"legacy-current-mutation\""));
	Control->Properties.Add(TEXT("control-value-current"), TEXT("(Float 999)"));
	FRigHierarchyTransformAST* CurrentTransform = Control->Transforms.FindByPredicate([](const FRigHierarchyTransformAST& Transform)
	{
		return Transform.Role == ERigHierarchyTransformRole::CurrentLocal;
	});
	if (TestNotNull(TEXT("Current local typed transform exists"), CurrentTransform))
		CurrentTransform->Value.AddToTranslation(FVector(31, 32, 33));
	for (FRigHierarchyStateAST& State : Control->States)
	{
		if (State.Kind == ERigHierarchyStateKind::ControlValue && State.Role == TEXT("current")) State.NumberValue += 7.0;
		if (State.Kind == ERigHierarchyStateKind::PreferredEuler && State.Role == TEXT("current")) State.Components[0] += 9.0;
	}
	TestNotEqual(TEXT("Current/editor pose mutation changes display canonical"), Result.Module->ToCanonicalString(), DisplayBefore);
	TestFalse(TEXT("Promoted legacy current properties do not leak into canonical output"),
		Result.Module->ToCanonicalString().Contains(TEXT("legacy-current-mutation")));
	TestEqual(TEXT("Current/editor pose mutation does not change semantic hash input"), Result.Module->ToCanonicalHashInput(), HashBefore);
	FRigHierarchyTransformAST* InitialTransform = Control->Transforms.FindByPredicate([](const FRigHierarchyTransformAST& Transform)
	{
		return Transform.Role == ERigHierarchyTransformRole::InitialLocal;
	});
	if (TestNotNull(TEXT("Initial local typed transform exists"), InitialTransform))
		InitialTransform->Value.AddToTranslation(FVector(1, 0, 0));
	TestNotEqual(TEXT("Initial pose mutation changes semantic hash input"), Result.Module->ToCanonicalHashInput(), HashBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangExporterInjectedNodeTest,
	"AnimBP2FP.RigLang.Exporter.InjectedNode",
	RigLangExporterTests::TestFlags)

bool FRigLangExporterInjectedNodeTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* Blueprint = RigLangExporterTests::MakeSyntheticRig();
	if (!TestNotNull(TEXT("Injected synthetic rig is created"), Blueprint)) return false;
	URigVMGraph* Graph = Blueprint->URigVMBlueprint::GetRigVMClient()->GetDefaultModel();
	URigVMController* Controller = Blueprint->GetOrCreateController(Graph);
	URigVMUnitNode* AddNode = Controller->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"), FVector2D::ZeroVector, TEXT("AddForInjection"), false);
	if (!TestNotNull(TEXT("Injection owner unit is created"), AddNode)) return false;
	URigVMInjectionInfo* Injection = Controller->AddInjectedNode(
		AddNode->GetName() + TEXT(".A"), true,
		FRigVMFunction_MathFloatNegate::StaticStruct(), TEXT("Execute"),
		TEXT("Value"), TEXT("Result"), TEXT("InjectedNegate"), false);
	if (!TestNotNull(TEXT("Injected unit is created"), Injection)) return false;

	const FRigLangExportResult Result = FRigLangExporter::Export(Blueprint);
	TestTrue(TEXT("Strict export with a supported injected unit succeeds"), Result.bSuccess);
	if (!TestNotNull(TEXT("Injected export produces a module"), Result.Module.Get())) return false;
	const FRigGraphAST* ExportedGraph = RigLangExporterTests::FindGraph(*Result.Module, Graph->GetPathName());
	if (!TestNotNull(TEXT("Injected owner graph exports"), ExportedGraph)) return false;
	const FRigNodeAST* Injected = ExportedGraph->Nodes.FindByPredicate(
		[](const FRigNodeAST& Node) { return Node.bInjected; });
	if (!TestNotNull(TEXT("Injected node is enumerated outside Graph.GetNodes"), Injected)) return false;
	TestEqual(TEXT("Injected node exports owner pin"), Injected->InjectionOwnerPin,
		AddNode->FindPin(TEXT("A"))->GetPinPath(true));
	TestEqual(TEXT("Injected node exports pin-local order"), Injected->InjectionOrder, 0);
	TestTrue(TEXT("Injected node exports input direction"), Injected->bInjectedAsInput);
	TestEqual(TEXT("Injected node exports exposed input pin"), Injected->InjectionInputPin, FString(TEXT("Value")));
	TestEqual(TEXT("Injected node exports exposed output pin"), Injected->InjectionOutputPin, FString(TEXT("Result")));
	int32 InjectedCount = 0;
	for (const FRigNodeAST& Node : ExportedGraph->Nodes) if (Node.bInjected) ++InjectedCount;
	TestEqual(TEXT("Injected node appears exactly once"), InjectedCount, 1);
	TestEqual(TEXT("Coverage includes injected nodes"), Result.Coverage.NodeTotal, 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangExporterRuntimeSymbolCollisionTest,
	"AnimBP2FP.RigLang.Exporter.RuntimeSymbolCollision",
	RigLangExporterTests::TestFlags)

bool FRigLangExporterRuntimeSymbolCollisionTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* Blueprint = RigLangExporterTests::MakeSyntheticRig();
	if (!TestNotNull(TEXT("Collision synthetic Control Rig is created"), Blueprint)) return false;
	URigVMController* LibraryController = Blueprint->GetOrCreateController(Blueprint->GetLocalFunctionLibrary());
	if (!TestNotNull(TEXT("Local function library controller exists"), LibraryController)) return false;
	TestNotNull(TEXT("Function whose readable symbol collides with Forwards Solve is created"),
		LibraryController->AddFunctionToLibrary(TEXT("Forwards Solve"), false, FVector2D::ZeroVector, false));

	FRigLangExportOptions StrictOptions;
	StrictOptions.bStrict = true;
	const FRigLangExportResult Strict = FRigLangExporter::Export(Blueprint, StrictOptions);
	TestFalse(TEXT("Strict export rejects a normalized runtime symbol collision"), Strict.bSuccess);
	TestTrue(TEXT("Strict collision is a concrete hard error"), Strict.Errors.ContainsByPredicate(
		[](const FString& Error) { return Error.Contains(TEXT("Duplicate exported rig symbol: ForwardsSolve")); }));

	FRigLangExportOptions NonStrictOptions;
	NonStrictOptions.bStrict = false;
	const FRigLangExportResult NonStrict = FRigLangExporter::Export(Blueprint, NonStrictOptions);
	TestFalse(TEXT("Non-strict export cannot swallow a runtime symbol collision"), NonStrict.bSuccess);
	if (TestNotNull(TEXT("Failed non-strict export still produces an inspectable module"), NonStrict.Module.Get()))
	{
		int32 SymbolCount = 0;
		for (const FRigEntryAST& Entry : NonStrict.Module->Entries) if (Entry.Name == TEXT("ForwardsSolve")) ++SymbolCount;
		for (const FRigFunctionAST& Function : NonStrict.Module->Functions) if (Function.Name == TEXT("ForwardsSolve")) ++SymbolCount;
		TestEqual(TEXT("Collision output never contains parser-invalid duplicate declarations"), SymbolCount, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangExporterStrictCoverageTest,
	"AnimBP2FP.RigLang.Exporter.StrictConnectedPinCoverage",
	RigLangExporterTests::TestFlags)

bool FRigLangExporterStrictCoverageTest::RunTest(const FString& Parameters)
{
	FRigLangExportCoverage Coverage;
	Coverage.PinTotal = 1;
	Coverage.VisitedPins.Add(TEXT("Model|Visited.Pin"));
	Coverage.ConnectedPins.Add(TEXT("Model|Missing.Pin"));
	TArray<FString> Errors;
	TestFalse(TEXT("Strict coverage rejects an unvisited connected pin"),
		FRigLangExporter::ValidateStrictCoverage(Coverage, Errors));
	TestTrue(TEXT("Strict coverage explains the missing connected pin"),
		Errors.ContainsByPredicate([](const FString& Error)
		{
			return Error.Contains(TEXT("Model|Missing.Pin"));
		}));
	FRigLangExportCoverage CompleteReflected;
	CompleteReflected.NodeTotal = 1;
	CompleteReflected.VisitedNodes.Add(TEXT("Model|Comment"));
	CompleteReflected.Reasons.Add(TEXT("Model|Comment"), TEXT("normalized: editor-only comment excluded from semantic hash"));
	TArray<FString> ReflectedErrors;
	TestTrue(TEXT("Strict coverage accepts a complete normalized reflected node"),
		FRigLangExporter::ValidateStrictCoverage(CompleteReflected, ReflectedErrors));
	CompleteReflected.LossyOrUnsupportedNodes.Add(TEXT("Model|Unsupported"));
	TestFalse(TEXT("Strict coverage rejects an explicitly unsupported node"),
		FRigLangExporter::ValidateStrictCoverage(CompleteReflected, ReflectedErrors));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangExporterExternalImportPolicyTest,
	"AnimBP2FP.RigLang.Exporter.ExternalImportPolicy",
	RigLangExporterTests::TestFlags)

bool FRigLangExporterExternalImportPolicyTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Function symbol strips hyphens through the shared sanitizer"),
		RigFunctionStableRuntimeSymbol(TEXT("Solve-Leg")), FString(TEXT("SolveLeg")));
	TestEqual(TEXT("Function symbol strips spaces through the shared sanitizer"),
		RigFunctionStableRuntimeSymbol(TEXT("Solve Leg")), FString(TEXT("SolveLeg")));
	TestEqual(TEXT("Function symbol prefixes a leading digit"),
		RigFunctionStableRuntimeSymbol(TEXT("12-Step")), FString(TEXT("_12Step")));
	TestEqual(TEXT("Function symbol strips unsupported ASCII punctuation"),
		RigFunctionStableRuntimeSymbol(TEXT("A!B")), FString(TEXT("AB")));
	TestEqual(TEXT("Sanitizer collisions are deterministic and visible to duplicate-symbol validation"),
		RigFunctionStableRuntimeSymbol(TEXT("A-B")), RigFunctionStableRuntimeSymbol(TEXT("A B")));
	TestEqual(TEXT("Library path symbol derivation calls the same sanitizer"),
		RigFunctionSymbolFromLibraryNodePath(TEXT("/Game/R.R:RigVMFunctionLibrary.12-Step")),
		RigFunctionStableRuntimeSymbol(TEXT("12-Step")));
	auto MakeCall = [](const FString& Id, const FString& Host, const FString& Path)
	{
		FRigNodeAST Node;
		Node.Kind = ERigNodeKind::Call;
		Node.StableId = Id;
		Node.Guid = Id + TEXT("-guid");
		Node.FunctionName = TEXT("LegacyShort");
		Node.FunctionIdentifier.HostObject = Host;
		Node.FunctionIdentifier.LibraryNodePath = Path;
		return Node;
	};
	auto MakeModule = [&MakeCall](const bool bReverse)
	{
		FRigModuleAST Module;
		Module.Header.ModuleId = FAnimLispModuleId::FromAssetPath(TEXT("/Game/Rigs/Caller"), EAnimLispModuleKind::Rig);
		Module.Header.AssetClassPath = TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");
		Module.Header.Version = 1;
		FRigImportAST& Existing = Module.Imports.AddDefaulted_GetRef();
		Existing.Import.Target = FAnimLispModuleId::FromAssetPath(TEXT("/Game/Existing"), EAnimLispModuleKind::Rig);
		Existing.Import.Alias = TEXT("Common");
		Existing.Import.ExpectedHash = TEXT("known-hash");
		FRigGraphAST& Graph = Module.Graphs.AddDefaulted_GetRef();
		Graph.StableId = TEXT("graph");
		Graph.EditorGuid = TEXT("11111111-1111-1111-1111-111111111111");
		TArray<FRigNodeAST> Calls = {
			MakeCall(TEXT("a1"), TEXT("/Game/A/Common.Common_C"), TEXT("/Game/A/Common.Common:RigVMFunctionLibrary.SolveA")),
			MakeCall(TEXT("a2"), TEXT("/Game/A/Common.Common_C"), TEXT("/Game/A/Common.Common:RigVMFunctionLibrary.SolveA")),
			MakeCall(TEXT("b"), TEXT("/Plugin/B/Common.Common_C"), TEXT("/Plugin/B/Common.Common:RigVMFunctionLibrary.SolveB")),
			MakeCall(TEXT("empty"), TEXT(".Generated_C"), TEXT(":RigVMFunctionLibrary.Empty"))
		};
		if (bReverse) Algo::Reverse(Calls);
		Graph.Nodes = MoveTemp(Calls);
		FRigLinkAST& FirstLink = Graph.Links.AddDefaulted_GetRef();
		FirstLink.SourceNodeId = TEXT("a1");
		FirstLink.SourcePinPath = TEXT("Out");
		FirstLink.TargetNodeId = TEXT("b");
		FirstLink.TargetPinPath = TEXT("In");
		FRigLinkAST& SecondLink = Graph.Links.AddDefaulted_GetRef();
		SecondLink.SourceNodeId = TEXT("a2");
		SecondLink.SourcePinPath = TEXT("Out");
		SecondLink.TargetNodeId = TEXT("empty");
		SecondLink.TargetPinPath = TEXT("In");
		if (bReverse) Algo::Reverse(Graph.Links);
		FRigLangExporter::NormalizeFunctionCallsAndImports(Module);
		Module.Header.ContentHash = FRigLangExporter::ComputeContentHash(Module.ToCanonicalHashInput());
		return Module;
	};

	FRigModuleAST Forward = MakeModule(false);
	FRigModuleAST Reverse = MakeModule(true);
	TestEqual(TEXT("Node/model order does not change canonical imports, symbols, or hash"),
		Forward.ToCanonicalString(), Reverse.ToCanonicalString());
	TestEqual(TEXT("One existing import plus three unique external targets"), Forward.Imports.Num(), 4);
	TestEqual(TEXT("Existing explicit import alias and known hash are preserved"),
		Forward.Imports[0].Import.Alias + TEXT("|") + Forward.Imports[0].Import.ExpectedHash,
		FString(TEXT("Common|known-hash")));
	TSet<FString> Targets;
	TSet<FString> Aliases;
	for (const FRigImportAST& Import : Forward.Imports)
	{
		Targets.Add(Import.Import.Target.AssetPath);
		if (Import.Import.Target.AssetPath != TEXT("/Game/Existing"))
		{
			TestTrue(TEXT("Synthesized imports do not invent an expected content hash"),
				Import.Import.ExpectedHash.IsEmpty());
		}
		bool bLegalAlias = !Import.Import.Alias.IsEmpty()
			&& !FChar::IsDigit(Import.Import.Alias[0]);
		for (const TCHAR Character : Import.Import.Alias)
		{
			bLegalAlias &= (Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('_');
		}
		TestTrue(TEXT("Every import alias is a legal identifier"),
			bLegalAlias);
		TestFalse(TEXT("Aliases are collision-free"), Aliases.Contains(Import.Import.Alias));
		Aliases.Add(Import.Import.Alias);
	}
	TestEqual(TEXT("Same target referenced by multiple calls is imported once"), Targets.Num(), 4);
	TestTrue(TEXT("Empty basename uses the Rig fallback alias"), Aliases.Contains(TEXT("Rig")));
	TestFalse(TEXT("Existing Common alias is not reused for another target"),
		Forward.Graphs[0].Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
		{
			return Node.FunctionIdentifier.HostObject.Contains(TEXT("/Common."))
				&& Node.FunctionName.StartsWith(TEXT("Common/"));
		}));
	const FString FixedPoint = Forward.ToCanonicalString();
	FRigLangExporter::NormalizeFunctionCallsAndImports(Forward);
	Forward.Header.ContentHash = FRigLangExporter::ComputeContentHash(Forward.ToCanonicalHashInput());
	TestEqual(TEXT("Import normalization is a canonical fixed point"), Forward.ToCanonicalString(), FixedPoint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangExporterRealAssetTest,
	"AnimBP2FP.RigLang.Exporter.RealAsset",
	RigLangExporterTests::TestFlags)

bool FRigLangExporterRealAssetTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* Blueprint = LoadObject<UControlRigBlueprint>(
		nullptr,
		TEXT("/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement.CR_Biped_FootPlacement"));
	if (!TestNotNull(TEXT("Real foot placement Control Rig loads"), Blueprint))
	{
		return false;
	}

	FRigLangExportOptions Options;
	Options.bStrict = true;
	const FRigLangExportResult First = FRigLangExporter::Export(Blueprint, Options);
	TestTrue(TEXT("Strict real asset export succeeds"), First.bSuccess);
	if (!TestNotNull(TEXT("Real asset export produces a module"), First.Module.Get()))
	{
		return false;
	}

	TestEqual(TEXT("Real model contract"), First.Coverage.ModelTotal, 39);
	TestEqual(TEXT("Real node contract"), First.Coverage.NodeTotal, 1165);
	TestEqual(TEXT("Real link contract"), First.Coverage.LinkTotal, 1190);
	TestEqual(TEXT("Real hierarchy contract"), First.Module->Hierarchy.Num(), 124);
	const URigHierarchy* SourceHierarchy = Blueprint->GetHierarchy();
	TMap<FString, int32> HierarchyIndices;
	for (int32 Index = 0; Index < First.Module->Hierarchy.Num(); ++Index)
		HierarchyIndices.Add(First.Module->Hierarchy[Index].StableId, Index);
	for (const FRigElementKey& Key : SourceHierarchy->GetAllKeys(true))
	{
		const FRigHierarchyElementAST* Exported = First.Module->Hierarchy.FindByPredicate(
			[&Key](const FRigHierarchyElementAST& Candidate) { return Candidate.StableId == Key.ToString(); });
		if (!TestNotNull(TEXT("Every real hierarchy element exports"), Exported)) continue;
		TArray<FString> Parents;
		for (const FRigElementKey& Parent : SourceHierarchy->GetParents(Key, false))
		{
			Parents.Add(RigLangExporterTests::QuoteValue(Parent.ToString()));
			TestTrue(TEXT("Every real hierarchy parent precedes its child"),
				HierarchyIndices.FindRef(Parent.ToString()) < HierarchyIndices.FindRef(Key.ToString()));
		}
	}
	TMap<ERigNodeKind, int32> KindCounts;
	for (const FRigGraphAST& Graph : First.Module->Graphs)
	{
		for (const FRigNodeAST& Node : Graph.Nodes) ++KindCounts.FindOrAdd(Node.Kind);
	}
	TestEqual(TEXT("Real unit node inventory"), KindCounts.FindRef(ERigNodeKind::Unit), 446);
	TestEqual(TEXT("Real variable node inventory"), KindCounts.FindRef(ERigNodeKind::Variable), 337);
	TestEqual(TEXT("Real comment node inventory"), KindCounts.FindRef(ERigNodeKind::Comment), 126);
	TestEqual(TEXT("Real reroute node inventory"), KindCounts.FindRef(ERigNodeKind::Reroute), 85);
	TestEqual(TEXT("Real function reference inventory"), KindCounts.FindRef(ERigNodeKind::Call), 41);
	TestEqual(TEXT("Real function entry inventory"), KindCounts.FindRef(ERigNodeKind::Entry), 37);
	TestEqual(TEXT("Real function return inventory"), KindCounts.FindRef(ERigNodeKind::Return), 37);
	TestEqual(TEXT("Real collapse inventory"), KindCounts.FindRef(ERigNodeKind::Collapse), 26);
	TestEqual(TEXT("Real dispatch inventory"), KindCounts.FindRef(ERigNodeKind::Dispatch), 19);
	TestEqual(TEXT("Real aggregate inventory"), KindCounts.FindRef(ERigNodeKind::Aggregate), 11);
	TestEqual(TEXT("Every real node has reconstruction coverage"), KindCounts.FindRef(ERigNodeKind::InvokeEntry), 0);
	TestEqual(TEXT("Every real model exports once as an authoritative graph"),
		First.Module->Graphs.Num(), First.Coverage.ModelTotal);
	int32 SourceEventCount = 0;
	TMap<FString, int32> GraphRoleCounts;
	TMap<ERigNodeKind, int32> ContainedOwnerKindCounts;
	int32 AccountedOwnedGraphs = 0;
	for (const URigVMGraph* Model : Blueprint->GetAllModels())
	{
		if (Model) SourceEventCount += Model->GetEventNames().Num();
	}
	TestEqual(TEXT("Every source event gets a typed entry declaration"),
		First.Module->Entries.Num(), SourceEventCount);
	TestEqual(TEXT("Only local library functions get callable declarations"),
		First.Module->Functions.Num(), Blueprint->GetLocalFunctionLibrary()->GetFunctions().Num());
	for (const URigVMLibraryNode* LibraryNode : Blueprint->GetLocalFunctionLibrary()->GetFunctions())
	{
		if (!LibraryNode || !LibraryNode->GetContainedGraph()) continue;
		const FRigVMGraphFunctionHeader Header = LibraryNode->GetFunctionHeader();
		FRigFunctionIdentifierAST ExpectedIdentifier;
		ExpectedIdentifier.HostObject = Header.LibraryPointer.HostObject.ToString();
		ExpectedIdentifier.LibraryNodePath = Header.LibraryPointer.GetLibraryNodePath();
		const FString StableId = ExpectedIdentifier.ToStableId();
		const FRigFunctionAST* Function = First.Module->Functions.FindByPredicate(
			[&StableId](const FRigFunctionAST& Candidate) { return Candidate.StableId == StableId; });
		if (!TestNotNull(TEXT("Local function resolves by full source identifier"), Function)) continue;
		TestTrue(TEXT("Function declaration preserves the typed source identifier"),
			Function->FunctionIdentifier == ExpectedIdentifier);
		TestEqual(TEXT("Function exposes a readable declaration symbol"), Function->Name,
			RigLangExporterTests::StableRuntimeSymbol(Header.Name.ToString()));
		TestEqual(TEXT("Function declaration references its authoritative contained graph"),
			Function->GraphStableId, LibraryNode->GetContainedGraph()->GetPathName());
		TestEqual(TEXT("Function ordered argument count is exact"), Function->Arguments.Num(), Header.Arguments.Num());
		for (int32 ArgumentIndex = 0; ArgumentIndex < Header.Arguments.Num(); ++ArgumentIndex)
		{
			if (!Function->Arguments.IsValidIndex(ArgumentIndex)) continue;
			const FRigVMGraphFunctionArgument& SourceArgument = Header.Arguments[ArgumentIndex];
			const FRigCallableArgumentAST& Argument = Function->Arguments[ArgumentIndex];
			TestEqual(TEXT("Function argument order and name are exact"), Argument.Name, SourceArgument.Name.ToString());
			TestEqual(TEXT("Function argument direction is exact"), Argument.Direction,
				RigLangExporterTests::ExpectedPinDirection(SourceArgument.Direction));
			TestEqual(TEXT("Function argument CPP type is exact"), Argument.Type.CPPType,
				SourceArgument.CPPType.ToString());
			TestEqual(TEXT("Function argument type object is exact"), Argument.Type.CPPTypeObject,
				SourceArgument.CPPTypeObject.ToSoftObjectPath().ToString());
			TestEqual(TEXT("Function argument array shape is exact"), Argument.Type.ContainerType,
				SourceArgument.bIsArray ? FString(TEXT("array")) : FString());
			TestEqual(TEXT("Function argument default is exact"), Argument.DefaultValue,
				SourceArgument.DefaultValue);
			TestEqual(TEXT("Function argument execute-context is exact"), Argument.bExecuteContext, SourceArgument.IsExecuteContext());
			TestEqual(TEXT("Function argument const is exact"), Argument.bConstant, SourceArgument.bIsConst);
			TestEqual(TEXT("Function argument input-variable is exact"), Argument.bInputVariable, SourceArgument.bIsInputVariable);
		}
		const TArray<FRigVMExternalVariable> SourceExternalVariables = LibraryNode->GetExternalVariables();
		TestEqual(TEXT("Function external variable count is exact"), Function->ExternalVariables.Num(), SourceExternalVariables.Num());
		for (int32 VariableIndex = 0; VariableIndex < SourceExternalVariables.Num(); ++VariableIndex)
		{
			if (!Function->ExternalVariables.IsValidIndex(VariableIndex)) continue;
			const FRigVMExternalVariable& SourceVariable = SourceExternalVariables[VariableIndex];
			const FRigExternalVariableAST& Variable = Function->ExternalVariables[VariableIndex];
			TestEqual(TEXT("External variable GUID is exact"), Variable.Guid, SourceVariable.GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
			TestEqual(TEXT("External variable name is exact"), Variable.Name, SourceVariable.GetName().ToString());
			TestEqual(TEXT("External variable CPP type is exact"), Variable.Type.CPPType, SourceVariable.GetBaseCPPType().ToString());
			TestEqual(TEXT("External variable object type is exact"), Variable.Type.CPPTypeObject,
				SourceVariable.GetCPPTypeObject() ? SourceVariable.GetCPPTypeObject()->GetPathName() : FString());
			TestEqual(TEXT("External variable array shape is exact"), Variable.Type.ContainerType,
				SourceVariable.IsArray() ? FString(TEXT("array")) : FString());
			TestEqual(TEXT("External variable public state is exact"), Variable.bPublic, SourceVariable.IsPublic());
			TestEqual(TEXT("External variable read-only state is exact"), Variable.bReadOnly, SourceVariable.IsReadOnly());
		}
		TArray<FRigFunctionDependencyAST> ExpectedDependencies;
		for (const TPair<FRigVMGraphFunctionIdentifier, uint32>& Pair : LibraryNode->GetDependencies())
		{
			FRigFunctionDependencyAST& Dependency = ExpectedDependencies.AddDefaulted_GetRef();
			Dependency.HostObject = Pair.Key.HostObject.ToString();
			Dependency.LibraryNodePath = Pair.Key.GetLibraryNodePath();
			Dependency.Hash = Pair.Value;
		}
		ExpectedDependencies.Sort([](const FRigFunctionDependencyAST& A, const FRigFunctionDependencyAST& B)
		{
			return A.HostObject == B.HostObject ? A.LibraryNodePath < B.LibraryNodePath : A.HostObject < B.HostObject;
		});
		TestEqual(TEXT("Function dependency count is exact"), Function->Dependencies.Num(), ExpectedDependencies.Num());
		for (int32 DependencyIndex = 0; DependencyIndex < ExpectedDependencies.Num(); ++DependencyIndex)
		{
			if (!Function->Dependencies.IsValidIndex(DependencyIndex)) continue;
			TestEqual(TEXT("Dependency host is exact"), Function->Dependencies[DependencyIndex].HostObject, ExpectedDependencies[DependencyIndex].HostObject);
			TestEqual(TEXT("Dependency path is exact"), Function->Dependencies[DependencyIndex].LibraryNodePath, ExpectedDependencies[DependencyIndex].LibraryNodePath);
			TestEqual(TEXT("Dependency hash is exact"), Function->Dependencies[DependencyIndex].Hash, ExpectedDependencies[DependencyIndex].Hash);
		}
	}
	for (const FRigGraphAST& Graph : First.Module->Graphs)
	{
		TestFalse(TEXT("Graph stable identity exports"), Graph.StableId.IsEmpty());
		TestFalse(TEXT("Graph editor GUID or explicit fallback exports"), Graph.EditorGuid.IsEmpty());
		TestFalse(TEXT("Graph role exports as a typed field"), Graph.Role.IsEmpty());
	}
	const TArray<FRigVMGraphVariableDescription> SourceVariables = Blueprint->GetMemberVariables();
	TestEqual(TEXT("Every member variable exports"), First.Module->Variables.Num(), SourceVariables.Num());
	for (const FRigVMGraphVariableDescription& SourceVariable : SourceVariables)
	{
		const FRigVariableAST* ExportedVariable = First.Module->Variables.FindByPredicate(
			[&SourceVariable](const FRigVariableAST& Variable) { return Variable.Name == SourceVariable.Name.ToString(); });
		if (!TestNotNull(TEXT("Source member variable exists in RigLang"), ExportedVariable)) continue;
		TestEqual(TEXT("Variable stable GUID exports"), ExportedVariable->StableId,
			SourceVariable.Guid.IsValid()
				? SourceVariable.Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
				: SourceVariable.Name.ToString());
		TestEqual(TEXT("Variable visibility exports"), ExportedVariable->Access,
			SourceVariable.bPublic ? ERigVariableAccess::PublicInput : ERigVariableAccess::Internal);
		TestEqual(TEXT("Variable CPP type exports"), ExportedVariable->Type.CPPType, SourceVariable.CPPType);
		FAnimLispTypeRef ExpectedVariableType;
		ExpectedVariableType.CPPType = SourceVariable.CPPType;
		ExpectedVariableType.CPPTypeObject = SourceVariable.CPPTypeObject
			? SourceVariable.CPPTypeObject->GetPathName()
			: SourceVariable.CPPTypeObjectPath.ToString();
		ExpectedVariableType.ContainerType = SourceVariable.ToExternalVariable().IsArray() ? TEXT("array") : TEXT("");
		ExpectedVariableType.Canonicalize();
		TestEqual(TEXT("Variable CPP type object exports"), ExportedVariable->Type.CPPTypeObject,
			ExpectedVariableType.CPPTypeObject);
		TestEqual(TEXT("Variable array container exports"), ExportedVariable->Type.ContainerType,
			ExpectedVariableType.ContainerType);
		TestEqual(TEXT("Variable default exports"), ExportedVariable->DefaultValue, SourceVariable.DefaultValue);
	}
	int32 BoneCount = 0, ControlCount = 0, NullCount = 0, CurveCount = 0;
	for (const FRigHierarchyElementAST& Element : First.Module->Hierarchy)
	{
		switch (Element.Kind)
		{
		case ERigHierarchyElementKind::Bone: ++BoneCount; break;
		case ERigHierarchyElementKind::Control: ++ControlCount; break;
		case ERigHierarchyElementKind::Null: ++NullCount; break;
		case ERigHierarchyElementKind::Curve: ++CurveCount; break;
		}
	}
	TestEqual(TEXT("Real bone contract"), BoneCount, 88);
	TestEqual(TEXT("Real control contract"), ControlCount, 18);
	TestEqual(TEXT("Real null contract"), NullCount, 16);
	TestEqual(TEXT("Real curve contract"), CurveCount, 2);
	for (const FRigElementKey& SourceKey : Blueprint->GetHierarchy()->GetAllKeys(true))
	{
		const FRigHierarchyElementAST* ExportedElement = First.Module->Hierarchy.FindByPredicate(
			[&SourceKey](const FRigHierarchyElementAST& Element) { return Element.StableId == SourceKey.ToString(); });
		if (!TestNotNull(TEXT("Every hierarchy key exports"), ExportedElement)) continue;
		const TArray<FRigElementKey> SourceParents = Blueprint->GetHierarchy()->GetParents(SourceKey, false);
		TestEqual(TEXT("Typed parent inventory is exact"), ExportedElement->Parents.Num(), SourceParents.Num());
		const FRigMultiParentElement* SourceMultiParent = Blueprint->GetHierarchy()->Find<FRigMultiParentElement>(SourceKey);
		for (int32 ParentIndex = 0; ParentIndex < SourceParents.Num() && ExportedElement->Parents.IsValidIndex(ParentIndex); ++ParentIndex)
		{
			const FRigHierarchyParentAST& TypedParent = ExportedElement->Parents[ParentIndex];
			const FRigElementWeight SourceCurrentWeight = Blueprint->GetHierarchy()->GetParentWeight(SourceKey, SourceParents[ParentIndex], false);
			const FRigElementWeight SourceInitialWeight = Blueprint->GetHierarchy()->GetParentWeight(SourceKey, SourceParents[ParentIndex], true);
			TestEqual(TEXT("Typed parent stable ID is exact"), TypedParent.StableId, SourceParents[ParentIndex].ToString());
			TestEqual(TEXT("Typed current location weight is exact"), TypedParent.CurrentWeight.Location, static_cast<double>(SourceCurrentWeight.Location));
			TestEqual(TEXT("Typed current rotation weight is exact"), TypedParent.CurrentWeight.Rotation, static_cast<double>(SourceCurrentWeight.Rotation));
			TestEqual(TEXT("Typed current scale weight is exact"), TypedParent.CurrentWeight.Scale, static_cast<double>(SourceCurrentWeight.Scale));
			TestEqual(TEXT("Typed initial location weight is exact"), TypedParent.InitialWeight.Location, static_cast<double>(SourceInitialWeight.Location));
			TestEqual(TEXT("Typed initial rotation weight is exact"), TypedParent.InitialWeight.Rotation, static_cast<double>(SourceInitialWeight.Rotation));
			TestEqual(TEXT("Typed initial scale weight is exact"), TypedParent.InitialWeight.Scale, static_cast<double>(SourceInitialWeight.Scale));
			if (SourceMultiParent && SourceMultiParent->ParentConstraints.IsValidIndex(ParentIndex))
				TestEqual(TEXT("Typed parent label is exact"), TypedParent.Label, SourceMultiParent->ParentConstraints[ParentIndex].DisplayLabel.ToString());
		}
		auto TestTypedTransform = [this, ExportedElement](const TCHAR* Label, ERigHierarchyTransformRole Role, const FTransform& Expected)
		{
			const FRigHierarchyTransformAST* Typed = ExportedElement->Transforms.FindByPredicate(
				[Role](const FRigHierarchyTransformAST& Transform) { return Transform.Role == Role; });
			if (TestNotNull(Label, Typed)) TestTrue(Label, Typed->Value.Equals(Expected, 0.0));
		};
		TestTypedTransform(TEXT("Typed initial local transform is exact"), ERigHierarchyTransformRole::InitialLocal, Blueprint->GetHierarchy()->GetInitialLocalTransform(SourceKey));
		TestTypedTransform(TEXT("Typed initial global transform is exact"), ERigHierarchyTransformRole::InitialGlobal, Blueprint->GetHierarchy()->GetInitialGlobalTransform(SourceKey));
		TestTypedTransform(TEXT("Typed current local transform is exact"), ERigHierarchyTransformRole::CurrentLocal, Blueprint->GetHierarchy()->GetLocalTransform(SourceKey, false));
		TestTypedTransform(TEXT("Typed current global transform is exact"), ERigHierarchyTransformRole::CurrentGlobal, Blueprint->GetHierarchy()->GetGlobalTransform(SourceKey, false));
		const TArray<FName> SourceMetadataNames = Blueprint->GetHierarchy()->GetMetadataNames(SourceKey);
		TestEqual(TEXT("Typed metadata inventory is exact"), ExportedElement->Metadata.Num(), SourceMetadataNames.Num());
		for (const FName MetadataName : SourceMetadataNames)
		{
			const FRigHierarchyMetadataAST* TypedMetadata = ExportedElement->Metadata.FindByPredicate(
				[MetadataName](const FRigHierarchyMetadataAST& Metadata) { return Metadata.Name == MetadataName.ToString(); });
			if (TestNotNull(TEXT("Every source metadata entry has a typed record"), TypedMetadata))
			{
				const FRigBaseElement* SourceElement = Blueprint->GetHierarchy()->Find(SourceKey);
				const ERigMetadataType MetadataType = Blueprint->GetHierarchy()->GetMetadataType(SourceKey, MetadataName);
				const FRigBaseMetadata* SourceMetadata = SourceElement ? Blueprint->GetHierarchy()->FindMetadataForElement(SourceElement, MetadataName, MetadataType) : nullptr;
				if (TestNotNull(TEXT("Typed metadata source exists"), SourceMetadata)) RigLangExporterTests::TestTypedMetadataExact(*this, *TypedMetadata, SourceMetadata);
			}
		}
		const FRigElementKey FirstParent = Blueprint->GetHierarchy()->GetFirstParent(SourceKey);
		TestEqual(TEXT("Typed compatibility parent is empty for roots and exact otherwise"),
			ExportedElement->ParentName,
			FirstParent.IsValid() ? FirstParent.Name.ToString() : FString());
		if (const FRigBoneElement* SourceBone = Blueprint->GetHierarchy()->Find<FRigBoneElement>(SourceKey))
		{
			const FRigHierarchyStateAST* BoneState = ExportedElement->States.FindByPredicate([](const FRigHierarchyStateAST& State){return State.Kind==ERigHierarchyStateKind::BoneType;});
			if (TestNotNull(TEXT("Typed bone state exists"), BoneState)) TestEqual(TEXT("Typed bone type is exact"), BoneState->Type,
				StaticEnum<ERigBoneType>()->GetNameStringByValue(static_cast<int64>(SourceBone->BoneType)));
		}
		if (SourceKey.Type == ERigElementType::Curve)
		{
			const FRigHierarchyStateAST* CurveState = ExportedElement->States.FindByPredicate([](const FRigHierarchyStateAST& State){return State.Kind==ERigHierarchyStateKind::Curve;});
			if (TestNotNull(TEXT("Typed curve state exists"), CurveState))
			{
				TestEqual(TEXT("Typed curve value is exact"), CurveState->NumberValue,
					static_cast<double>(Blueprint->GetHierarchy()->GetCurveValue(SourceKey)));
				TestEqual(TEXT("Typed curve set state is exact"), CurveState->bBoolValue,
					Blueprint->GetHierarchy()->IsCurveValueSet(SourceKey));
			}
		}
		if (const FRigControlElement* SourceControl = Blueprint->GetHierarchy()->Find<FRigControlElement>(SourceKey))
		{
			const FRigHierarchyStateAST* Settings = ExportedElement->States.FindByPredicate([](const FRigHierarchyStateAST& State){return State.Kind==ERigHierarchyStateKind::ControlSettings;});
			if (TestNotNull(TEXT("Typed control settings state exists"), Settings))
			{
				TestEqual(TEXT("Typed control type is exact"), Settings->Type,
					StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(SourceControl->Settings.ControlType)));
				TestEqual(TEXT("Typed complete control settings are exact"), Settings->SerializedValue,
					RigLangExporterTests::ExportStruct(FRigControlSettings::StaticStruct(), &SourceControl->Settings));
			}
			TestTypedTransform(TEXT("Typed offset initial local is exact"), ERigHierarchyTransformRole::OffsetInitialLocal, SourceControl->GetOffsetTransform()[ERigTransformType::InitialLocal].Get());
			TestTypedTransform(TEXT("Typed offset initial global is exact"), ERigHierarchyTransformRole::OffsetInitialGlobal, Blueprint->GetHierarchy()->GetGlobalControlOffsetTransform(SourceKey, true));
			TestTypedTransform(TEXT("Typed offset current local is exact"), ERigHierarchyTransformRole::OffsetCurrentLocal, SourceControl->GetOffsetTransform()[ERigTransformType::CurrentLocal].Get());
			TestTypedTransform(TEXT("Typed offset current global is exact"), ERigHierarchyTransformRole::OffsetCurrentGlobal, Blueprint->GetHierarchy()->GetGlobalControlOffsetTransform(SourceKey, false));
			TestTypedTransform(TEXT("Typed shape initial local is exact"), ERigHierarchyTransformRole::ShapeInitialLocal, Blueprint->GetHierarchy()->GetLocalControlShapeTransform(SourceKey, true));
			TestTypedTransform(TEXT("Typed shape initial global is exact"), ERigHierarchyTransformRole::ShapeInitialGlobal, Blueprint->GetHierarchy()->GetGlobalControlShapeTransform(SourceKey, true));
			TestTypedTransform(TEXT("Typed shape current local is exact"), ERigHierarchyTransformRole::ShapeCurrentLocal, Blueprint->GetHierarchy()->GetLocalControlShapeTransform(SourceKey, false));
			TestTypedTransform(TEXT("Typed shape current global is exact"), ERigHierarchyTransformRole::ShapeCurrentGlobal, Blueprint->GetHierarchy()->GetGlobalControlShapeTransform(SourceKey, false));
			TestTypedTransform(TEXT("Typed pose initial local is exact"), ERigHierarchyTransformRole::PoseInitialLocal, Blueprint->GetHierarchy()->GetLocalTransform(SourceKey, true));
			TestTypedTransform(TEXT("Typed pose initial global is exact"), ERigHierarchyTransformRole::PoseInitialGlobal, Blueprint->GetHierarchy()->GetGlobalTransform(SourceKey, true));
			TestTypedTransform(TEXT("Typed pose current local is exact"), ERigHierarchyTransformRole::PoseCurrentLocal, Blueprint->GetHierarchy()->GetLocalTransform(SourceKey, false));
			TestTypedTransform(TEXT("Typed pose current global is exact"), ERigHierarchyTransformRole::PoseCurrentGlobal, Blueprint->GetHierarchy()->GetGlobalTransform(SourceKey, false));
			for (const TPair<FString, FVector> ExpectedEuler : { TPair<FString, FVector>(TEXT("current"), SourceControl->PreferredEulerAngles.Current), TPair<FString, FVector>(TEXT("initial"), SourceControl->PreferredEulerAngles.Initial) })
			{
				const FRigHierarchyStateAST* EulerState = ExportedElement->States.FindByPredicate([&ExpectedEuler](const FRigHierarchyStateAST& State)
				{
					return State.Kind == ERigHierarchyStateKind::PreferredEuler && State.Role == ExpectedEuler.Key;
				});
				if (TestNotNull(TEXT("Typed preferred Euler state exists"), EulerState))
				{
					TestEqual(TEXT("Typed preferred Euler order is exact"), EulerState->Type,
						StaticEnum<EEulerRotationOrder>()->GetNameStringByValue(static_cast<int64>(SourceControl->PreferredEulerAngles.RotationOrder)));
					TestTrue(TEXT("Typed preferred Euler components are exact"), EulerState->Components == TArray<double>{ExpectedEuler.Value.X, ExpectedEuler.Value.Y, ExpectedEuler.Value.Z});
				}
			}
			auto StateValueText = [](const FRigHierarchyStateAST& State)
			{
				TArray<FString> Fields;
				if (State.Type == TEXT("Bool")) Fields.Add(State.bBoolValue ? TEXT("true") : TEXT("false"));
				else if (State.Type == TEXT("Integer")) Fields.Add(LexToString(State.IntegerValue));
				else if (!State.Components.IsEmpty()) for (double Value : State.Components) Fields.Add(LexToString(Value));
				else Fields.Add(LexToString(State.NumberValue));
				return TEXT("(") + State.Type + (Fields.IsEmpty() ? FString() : TEXT(" ") + FString::Join(Fields, TEXT(" "))) + TEXT(")");
			};
			auto TestControlValue = [this, ExportedElement, SourceControl, &StateValueText, Blueprint, SourceKey](const TCHAR* Label, const FString& Role, ERigControlValueType ValueType)
			{
				const FRigHierarchyStateAST* State = ExportedElement->States.FindByPredicate([&Role](const FRigHierarchyStateAST& Candidate)
				{
					return Candidate.Kind == ERigHierarchyStateKind::ControlValue && Candidate.Role == Role;
				});
				if (TestNotNull(Label, State)) TestEqual(Label, StateValueText(*State), RigLangExporterTests::ExportTypedControlValue(
					Blueprint->GetHierarchy()->GetControlValue(SourceKey, ValueType), SourceControl->Settings.ControlType));
			};
			TestControlValue(TEXT("Typed current control value is exact"), TEXT("current"), ERigControlValueType::Current);
			TestControlValue(TEXT("Typed initial control value is exact"), TEXT("initial"), ERigControlValueType::Initial);
			TestControlValue(TEXT("Typed minimum control value is exact"), TEXT("minimum"), ERigControlValueType::Minimum);
			TestControlValue(TEXT("Typed maximum control value is exact"), TEXT("maximum"), ERigControlValueType::Maximum);
		}
	}
	TestEqual(TEXT("All models are visited"), First.Coverage.VisitedModels.Num(), First.Coverage.ModelTotal);
	TestEqual(TEXT("All nodes are visited"), First.Coverage.VisitedNodes.Num(), First.Coverage.NodeTotal);
	TestEqual(TEXT("All pins are visited"), First.Coverage.VisitedPins.Num(), First.Coverage.PinTotal);
	TestEqual(TEXT("All links are visited"), First.Coverage.VisitedLinks.Num(), First.Coverage.LinkTotal);
	const FRigCoverageTotals CoverageTotals = First.Module->GetCoverageTotals();
	TestEqual(TEXT("Module node coverage totals match the source node inventory"),
		CoverageTotals.Total(), First.Coverage.NodeTotal);
	auto AssertReasons = [this, &First](const TSet<FString>& Visited, const TCHAR* Label)
	{
		for (const FString& Id : Visited)
		{
			TestTrue(FString::Printf(TEXT("%s has a coverage reason: %s"), Label, *Id),
				First.Coverage.Reasons.Contains(Id));
		}
	};
	AssertReasons(First.Coverage.VisitedModels, TEXT("Model"));
	AssertReasons(First.Coverage.VisitedNodes, TEXT("Node"));
	AssertReasons(First.Coverage.VisitedPins, TEXT("Pin"));
	AssertReasons(First.Coverage.VisitedLinks, TEXT("Link"));
	for (const URigVMGraph* Model : Blueprint->GetAllModels())
	{
		if (!Model) continue;
		const FRigGraphAST* ExportedGraph = RigLangExporterTests::FindGraph(*First.Module, Model->GetPathName());
		if (!TestNotNull(TEXT("Every source model has one exported owner graph"), ExportedGraph)) continue;
		FString ExpectedRole = TEXT("root");
		if (Model == Blueprint->GetLocalFunctionLibrary()) ExpectedRole = TEXT("function-library");
		else if (Model->GetParentGraph()) ExpectedRole = Model->GetParentGraph() == Blueprint->GetLocalFunctionLibrary()
			? TEXT("function") : TEXT("node-contained");
		TestEqual(TEXT("Model graph role exports as a typed field"), ExportedGraph->Role, ExpectedRole);
		++GraphRoleCounts.FindOrAdd(ExportedGraph->Role);
		TestEqual(TEXT("Model parent graph exports as a typed field"), ExportedGraph->ParentStableId,
			Model->GetParentGraph() ? Model->GetParentGraph()->GetPathName() : FString());
		for (const FRigNodeAST& Node : ExportedGraph->Nodes)
		{
			if (Node.ContainedGraphStableId.IsEmpty()) continue;
			++ContainedOwnerKindCounts.FindOrAdd(Node.Kind);
			const FRigGraphAST* ContainedGraph = RigLangExporterTests::FindGraph(
				*First.Module, Node.ContainedGraphStableId);
			if (TestNotNull(TEXT("Every typed contained-graph owner resolves"), ContainedGraph))
			{
				TestEqual(TEXT("Every typed contained graph points back to its owner graph"),
					ContainedGraph->ParentStableId, ExportedGraph->StableId);
			}
		}
		if (ExportedGraph->Role == TEXT("function-library"))
		{
			++AccountedOwnedGraphs;
		}
		else if (ExportedGraph->Role == TEXT("root"))
		{
			int32 Owners = 0;
			for (const FRigEntryAST& Entry : First.Module->Entries)
			{
				if (Entry.GraphStableId == ExportedGraph->StableId) ++Owners;
			}
			TestTrue(TEXT("Root graph has at least one event entry owner"), Owners >= 1);
			if (Owners >= 1) ++AccountedOwnedGraphs;
		}
		else if (ExportedGraph->Role == TEXT("function"))
		{
			int32 Owners = 0;
			for (const FRigFunctionAST& Function : First.Module->Functions)
			{
				if (Function.GraphStableId == ExportedGraph->StableId) ++Owners;
			}
			TestEqual(TEXT("Function graph has exactly one callable owner"), Owners, 1);
			if (Owners == 1) ++AccountedOwnedGraphs;
		}
		else if (ExportedGraph->Role == TEXT("node-contained"))
		{
			const FRigGraphAST* ParentGraph = RigLangExporterTests::FindGraph(*First.Module, ExportedGraph->ParentStableId);
			int32 Owners = 0;
			if (TestNotNull(TEXT("Node-contained graph parent exists"), ParentGraph))
			{
				for (const FRigNodeAST& Node : ParentGraph->Nodes)
				{
					if (Node.ContainedGraphStableId != ExportedGraph->StableId) continue;
					++Owners;
				}
			}
			TestEqual(TEXT("Node-contained graph has exactly one typed parent-node owner"), Owners, 1);
			if (Owners == 1) ++AccountedOwnedGraphs;
		}
		const TArray<FRigVMGraphVariableDescription> SourceLocals = Model->GetLocalVariables(false);
		TestEqual(TEXT("Graph local variable count preserves unused locals"), ExportedGraph->LocalVariables.Num(), SourceLocals.Num());
		for (int32 LocalIndex = 0; LocalIndex < SourceLocals.Num(); ++LocalIndex)
		{
			if (!ExportedGraph->LocalVariables.IsValidIndex(LocalIndex)) continue;
			const FRigVMGraphVariableDescription& SourceLocal = SourceLocals[LocalIndex];
			const FRigGraphVariableAST& Local = ExportedGraph->LocalVariables[LocalIndex];
			TestEqual(TEXT("Graph local order and GUID are exact"), Local.Guid,
				SourceLocal.Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
			TestEqual(TEXT("Graph local name is exact"), Local.Name, SourceLocal.Name.ToString());
			TestEqual(TEXT("Graph local CPP type is exact"), Local.Type.CPPType, SourceLocal.CPPType);
			FAnimLispTypeRef ExpectedLocalType;
			ExpectedLocalType.CPPType = SourceLocal.CPPType;
			ExpectedLocalType.CPPTypeObject = SourceLocal.CPPTypeObject
				? SourceLocal.CPPTypeObject->GetPathName() : SourceLocal.CPPTypeObjectPath.ToString();
			ExpectedLocalType.ContainerType = SourceLocal.ToExternalVariable().IsArray() ? TEXT("array") : TEXT("");
			ExpectedLocalType.Canonicalize();
			TestEqual(TEXT("Graph local object type is exact"), Local.Type.CPPTypeObject,
				ExpectedLocalType.CPPTypeObject);
			TestEqual(TEXT("Graph local array shape is exact"), Local.Type.ContainerType,
				ExpectedLocalType.ContainerType);
			TestEqual(TEXT("Graph local default is exact"), Local.DefaultValue, SourceLocal.DefaultValue);
			TestEqual(TEXT("Graph local CPP type object path field is exact"),
				Local.CPPTypeObjectPath, SourceLocal.CPPTypeObjectPath.ToString());
			FString ExpectedCategory;
			FTextStringHelper::WriteToBuffer(ExpectedCategory, SourceLocal.Category);
			FString ExpectedTooltip;
			FTextStringHelper::WriteToBuffer(ExpectedTooltip, SourceLocal.Tooltip);
			TestEqual(TEXT("Graph local category FText serialization is exact"), Local.Category, ExpectedCategory);
			TestEqual(TEXT("Graph local tooltip FText serialization is exact"), Local.Tooltip, ExpectedTooltip);
			TestEqual(TEXT("Graph local exposed-on-spawn flag is exact"), Local.bExposedOnSpawn, SourceLocal.bExposedOnSpawn);
			TestEqual(TEXT("Graph local cinematics flag is exact"), Local.bExposeToCinematics, SourceLocal.bExposeToCinematics);
			TestEqual(TEXT("Graph local public flag is exact"), Local.bPublic, SourceLocal.bPublic);
			TestEqual(TEXT("Graph local private flag is exact"), Local.bPrivate, SourceLocal.bPrivate);
		}
		FString ExpectedGraphGuid = FRigLangExporter::ComputeDeterministicEditorGuid(
			Blueprint->GetPathName(), Model->GetPathName());
		TArray<UEdGraph*> ModelEditorGraphs;
		Blueprint->GetAllGraphs(ModelEditorGraphs);
		for (const UEdGraph* EditorGraph : ModelEditorGraphs)
		{
			if (const URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(EditorGraph);
				RigGraph && RigGraph->GetModel() == Model && RigGraph->GraphGuid.IsValid())
			{
				ExpectedGraphGuid = RigGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				break;
			}
		}
		TestEqual(TEXT("Model graph GUID is exact or uses the explicit model fallback"),
			ExportedGraph->EditorGuid, ExpectedGraphGuid);
		TArray<FString> ExpectedEvents;
		for (const FName EventName : Model->GetEventNames())
		{
			ExpectedEvents.Add(RigLangExporterTests::QuoteValue(EventName.ToString()));
			const FRigEntryAST* Entry = First.Module->Entries.FindByPredicate(
				[Model, EventName](const FRigEntryAST& Item)
			{
				return Item.GraphStableId == Model->GetPathName() && Item.EventName == EventName.ToString();
			});
			if (TestNotNull(TEXT("Every source event has an entry declaration"), Entry))
			{
				TestEqual(TEXT("Entry name is a stable runtime symbol"), Entry->Name,
					RigLangExporterTests::StableRuntimeSymbol(EventName.ToString()));
			}
		}
		TestEqual(TEXT("All model event names export without truncation"),
			ExportedGraph->Properties.FindRef(TEXT("event-names")),
			TEXT("(") + FString::Join(ExpectedEvents, TEXT(" ")) + TEXT(")"));
		for (int32 NodeIndex = 1; NodeIndex < ExportedGraph->Nodes.Num(); ++NodeIndex)
		{
			const FRigNodeAST& Previous = ExportedGraph->Nodes[NodeIndex - 1];
			const FRigNodeAST& Current = ExportedGraph->Nodes[NodeIndex];
			TestTrue(TEXT("Canonical node order is exported GUID then stable ID"),
				Previous.Guid < Current.Guid
				|| (Previous.Guid == Current.Guid && Previous.StableId <= Current.StableId));
		}
		TSet<FString> ExpectedLinks;
		for (const URigVMLink* SourceLink : Model->GetLinks())
		{
			if (SourceLink && SourceLink->GetSourceNode() && SourceLink->GetTargetNode()
				&& SourceLink->GetSourcePin() && SourceLink->GetTargetPin())
			{
				ExpectedLinks.Add(SourceLink->GetSourceNode()->GetName() + TEXT(".")
					+ RigLangExporterTests::RelativePinPath(SourceLink->GetSourcePin()) + TEXT("->")
					+ SourceLink->GetTargetNode()->GetName() + TEXT(".")
					+ RigLangExporterTests::RelativePinPath(SourceLink->GetTargetPin()));
			}
		}
		TSet<FString> ExportedLinks;
		for (const FRigLinkAST& Link : ExportedGraph->Links)
		{
			ExportedLinks.Add(Link.SourceNodeId + TEXT(".") + Link.SourcePinPath + TEXT("->")
				+ Link.TargetNodeId + TEXT(".") + Link.TargetPinPath);
		}
		TestTrue(TEXT("Every graph preserves its exact link endpoint set"),
			ExpectedLinks.Includes(ExportedLinks) && ExportedLinks.Includes(ExpectedLinks));
		for (const URigVMNode* SourceNode : Model->GetNodes())
		{
			if (!SourceNode) continue;
			const FRigNodeAST* ExportedNode = ExportedGraph->Nodes.FindByPredicate(
				[SourceNode](const FRigNodeAST& Node) { return Node.StableId == SourceNode->GetName(); });
			if (!TestNotNull(TEXT("Every source node exists in its owner graph"), ExportedNode)) continue;
			const FString NodeReason = First.Coverage.Reasons.FindRef(SourceNode->GetPathName());
			FString ExpectedReasonPrefix;
			if (ExportedNode->Kind == ERigNodeKind::Comment)
				ExpectedReasonPrefix = TEXT("normalized:");
			else switch (ExportedNode->Coverage)
			{
			case ERigNodeCoverage::Exact: ExpectedReasonPrefix = TEXT("exact:"); break;
			case ERigNodeCoverage::Reflected: ExpectedReasonPrefix = TEXT("reflected:"); break;
			case ERigNodeCoverage::Lossy: ExpectedReasonPrefix = TEXT("lossy:"); break;
			case ERigNodeCoverage::Unsupported: ExpectedReasonPrefix = TEXT("unsupported:"); break;
			}
			TestTrue(TEXT("Node coverage enum matches its reason classification"),
				NodeReason.StartsWith(ExpectedReasonPrefix));
			if (ExportedNode->Guid.StartsWith(TEXT("model:")))
			{
				TestEqual(TEXT("Model-only node fallback key is its full model path"),
					ExportedNode->Guid, TEXT("model:") + SourceNode->GetNodePath(true));
				TestFalse(TEXT("Model-only identity does not downgrade semantic coverage"),
					ExportedNode->Coverage == ERigNodeCoverage::Lossy
					|| ExportedNode->Coverage == ERigNodeCoverage::Unsupported);
			}
			else
			{
				FGuid EditorGuid;
				TestTrue(TEXT("Editor-backed node GUID parses as an FGuid"),
					FGuid::Parse(ExportedNode->Guid, EditorGuid));
				TestNotEqual(TEXT("Editor-backed node GUID is not a node-name fallback"),
					ExportedNode->Guid, SourceNode->GetName());
			}
			if (ExportedNode->Coverage == ERigNodeCoverage::Lossy
				|| ExportedNode->Coverage == ERigNodeCoverage::Unsupported)
			{
				TestFalse(TEXT("Lossy/unsupported node records a concrete reason"), NodeReason.IsEmpty());
			}
			TestEqual(TEXT("Node UObject class exports"), ExportedNode->ClassPath, SourceNode->GetClass()->GetPathName());
			TestEqual(TEXT("Node injected state exports"), ExportedNode->bInjected, SourceNode->IsInjected());
			TestTrue(TEXT("Strict real export has no lossy or unsupported nodes"),
				ExportedNode->Coverage != ERigNodeCoverage::Lossy
				&& ExportedNode->Coverage != ERigNodeCoverage::Unsupported);
			if (const URigVMVariableNode* VariableNode = Cast<URigVMVariableNode>(SourceNode))
			{
				TestEqual(TEXT("Variable node name reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("variable-name")),
					RigLangExporterTests::QuoteValue(VariableNode->GetVariableName().ToString()));
				TestEqual(TEXT("Variable GUID reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("variable-guid")),
					RigLangExporterTests::QuoteValue(VariableNode->GetVariableGuid().ToString(EGuidFormats::DigitsWithHyphensLower)));
				TestEqual(TEXT("Variable getter state reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("getter")),
					VariableNode->IsGetter() ? FString(TEXT("true")) : FString(TEXT("false")));
				TestEqual(TEXT("Variable external state reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("external")),
					VariableNode->IsExternalVariable() ? FString(TEXT("true")) : FString(TEXT("false")));
				TestEqual(TEXT("Variable local state reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("local")),
					VariableNode->IsLocalVariable() ? FString(TEXT("true")) : FString(TEXT("false")));
				TestEqual(TEXT("Variable CPP type reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("variable-cpp-type")),
					RigLangExporterTests::QuoteValue(VariableNode->GetCPPType()));
				TestEqual(TEXT("Variable CPP type object reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("variable-cpp-type-object")),
					RigLangExporterTests::QuoteValue(VariableNode->GetCPPTypeObject()
						? VariableNode->GetCPPTypeObject()->GetPathName() : FString()));
				TestEqual(TEXT("Variable default reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("variable-default")),
					RigLangExporterTests::QuoteValue(VariableNode->GetDefaultValue()));
			}
			if (const URigVMTemplateNode* TemplateNode = Cast<URigVMTemplateNode>(SourceNode))
			{
				TestEqual(TEXT("Template notation reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("template-notation")),
					RigLangExporterTests::QuoteValue(TemplateNode->GetNotation().ToString()));
				TestEqual(TEXT("Template resolved state reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("template-resolved")),
					TemplateNode->IsResolved() ? FString(TEXT("true")) : FString(TEXT("false")));
				TestEqual(TEXT("Template resolved function reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("resolved-function")),
					RigLangExporterTests::QuoteValue(TemplateNode->GetResolvedFunction()
						? TemplateNode->GetResolvedFunction()->Name : FString()));
				TestEqual(TEXT("Template type map reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("template-types")),
					RigLangExporterTests::ExportTemplateTypeMap(TemplateNode->GetTemplatePinTypeMap(true, true)));
			}
			if (const URigVMDispatchNode* DispatchNode = Cast<URigVMDispatchNode>(SourceNode))
			{
				const FRigVMDispatchFactory* Factory = DispatchNode->GetFactory();
				TestEqual(TEXT("Dispatch factory reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("dispatch-factory")),
					RigLangExporterTests::QuoteValue(Factory ? Factory->GetFactoryName().ToString() : FString()));
				TestEqual(TEXT("Dispatch script struct reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("dispatch-script-struct")),
					RigLangExporterTests::QuoteValue(Factory && Factory->GetScriptStruct()
						? Factory->GetScriptStruct()->GetPathName() : FString()));
			}
			if (const URigVMAggregateNode* AggregateNode = Cast<URigVMAggregateNode>(SourceNode))
			{
				TestEqual(TEXT("Aggregate method reconstructs exactly"), ExportedNode->MethodName,
					AggregateNode->GetMethodName().ToString());
				TestEqual(TEXT("Aggregate direction reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("input-aggregate")),
					AggregateNode->IsInputAggregate() ? FString(TEXT("true")) : FString(TEXT("false")));
				TestEqual(TEXT("Aggregate first inner endpoint reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("first-inner-node")),
					RigLangExporterTests::QuoteValue(AggregateNode->GetFirstInnerNode()
						? AggregateNode->GetFirstInnerNode()->GetNodePath(true) : FString()));
				TestEqual(TEXT("Aggregate last inner endpoint reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("last-inner-node")),
					RigLangExporterTests::QuoteValue(AggregateNode->GetLastInnerNode()
						? AggregateNode->GetLastInnerNode()->GetNodePath(true) : FString()));
			}
			if (const URigVMLibraryNode* LibraryNode = Cast<URigVMLibraryNode>(SourceNode))
			{
				TestEqual(TEXT("Typed library-node contained graph reconstructs exactly"),
					ExportedNode->ContainedGraphStableId,
					LibraryNode->GetContainedGraph() ? LibraryNode->GetContainedGraph()->GetPathName() : FString());
			}
			if (const URigVMCollapseNode* CollapseNode = ExportedNode->Kind == ERigNodeKind::Collapse
				? Cast<URigVMCollapseNode>(SourceNode) : nullptr)
			{
				TArray<FString> ExpectedPinGuids;
				for (const URigVMPin* Pin : CollapseNode->GetPins()) if (Pin)
					ExpectedPinGuids.Add(TEXT("(") + RigLangExporterTests::QuoteValue(Pin->GetName()) + TEXT(" ")
						+ RigLangExporterTests::QuoteValue(CollapseNode->FindPinGuid(Pin).ToString(EGuidFormats::DigitsWithHyphensLower)) + TEXT(")"));
				TestEqual(TEXT("Collapse interface pin GUIDs reconstruct exactly"),
					ExportedNode->Properties.FindRef(TEXT("interface-pin-guids")),
					TEXT("(") + FString::Join(ExpectedPinGuids, TEXT(" ")) + TEXT(")"));
				TArray<FString> ExpectedExternalVariables;
				for (const FRigVMExternalVariable& Variable : CollapseNode->GetExternalVariables())
					ExpectedExternalVariables.Add(TEXT("(") + RigLangExporterTests::QuoteValue(Variable.GetName().ToString())
						+ TEXT(" ") + RigLangExporterTests::QuoteValue(Variable.GetExtendedCPPType().ToString()) + TEXT(")"));
				TestEqual(TEXT("Collapse external variables reconstruct exactly"),
					ExportedNode->Properties.FindRef(TEXT("external-variables")),
					TEXT("(") + FString::Join(ExpectedExternalVariables, TEXT(" ")) + TEXT(")"));
			}
			if (const URigVMFunctionReferenceNode* FunctionNode = Cast<URigVMFunctionReferenceNode>(SourceNode))
			{
				const FRigVMGraphFunctionHeader Header = FunctionNode->GetReferencedFunctionHeader();
				TestEqual(TEXT("Function reference host reconstructs exactly"),
					ExportedNode->FunctionIdentifier.HostObject,
					Header.LibraryPointer.HostObject.ToString());
				TestEqual(TEXT("Function reference path reconstructs exactly"),
					ExportedNode->FunctionIdentifier.LibraryNodePath,
					Header.LibraryPointer.GetLibraryNodePath());
				TestEqual(TEXT("Function reference remap requirement reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("requires-variable-remapping")),
					FunctionNode->RequiresVariableRemapping() ? FString(TEXT("true")) : FString(TEXT("false")));
				TestEqual(TEXT("Function reference remap completeness reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("fully-remapped")),
					FunctionNode->IsFullyRemapped() ? FString(TEXT("true")) : FString(TEXT("false")));
				TestEqual(TEXT("Function reference variable map reconstructs exactly"),
					ExportedNode->Properties.FindRef(TEXT("variable-remapping")),
					RigLangExporterTests::ExportVariableMap(FunctionNode->GetVariableMap()));
			}
			TestEqual(TEXT("Node event name exports"), ExportedNode->EventName, SourceNode->GetEventName().ToString());
			if (const URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(SourceNode))
			{
				TestEqual(TEXT("Unit method exports"), ExportedNode->MethodName, UnitNode->GetMethodName().ToString());
			}
			TestEqual(TEXT("Root pin count preserves the source tree"),
				ExportedNode->Pins.Num(), SourceNode->GetPins().Num());
			for (const URigVMPin* SourcePin : SourceNode->GetPins())
			{
				if (!SourcePin) continue;
				const FRigPinAST* ExportedPin = ExportedNode->Pins.FindByPredicate(
					[SourcePin](const FRigPinAST& Pin)
					{
						return Pin.Path == RigLangExporterTests::RelativePinPath(SourcePin);
					});
				if (TestNotNull(TEXT("Every source root pin exports"), ExportedPin))
				{
					RigLangExporterTests::ComparePinTree(*this, SourcePin, *ExportedPin);
				}
			}
		}
	}
	TestEqual(TEXT("Every real graph has a valid ownership category"),
		AccountedOwnedGraphs, First.Module->Graphs.Num());
	AddInfo(FString::Printf(TEXT("Real graph roles: root=%d function-library=%d function=%d node-contained=%d total=%d"),
		GraphRoleCounts.FindRef(TEXT("root")), GraphRoleCounts.FindRef(TEXT("function-library")),
		GraphRoleCounts.FindRef(TEXT("function")), GraphRoleCounts.FindRef(TEXT("node-contained")),
		First.Module->Graphs.Num()));
	AddInfo(FString::Printf(TEXT("Real typed contained owners: collapse=%d aggregate=%d call=%d total=%d"),
		ContainedOwnerKindCounts.FindRef(ERigNodeKind::Collapse),
		ContainedOwnerKindCounts.FindRef(ERigNodeKind::Aggregate),
		ContainedOwnerKindCounts.FindRef(ERigNodeKind::Call),
		ContainedOwnerKindCounts.FindRef(ERigNodeKind::Collapse)
			+ ContainedOwnerKindCounts.FindRef(ERigNodeKind::Aggregate)
			+ ContainedOwnerKindCounts.FindRef(ERigNodeKind::Call)));

	TSet<FString> ExpectedEditorGuids;
	TArray<UEdGraph*> EditorGraphs;
	Blueprint->GetAllGraphs(EditorGraphs);
	for (const UEdGraph* EditorGraph : EditorGraphs)
	{
		if (!EditorGraph) continue;
		for (const UEdGraphNode* EditorNode : EditorGraph->Nodes)
		{
			if (const URigVMEdGraphNode* RigVMNode = Cast<URigVMEdGraphNode>(EditorNode);
				RigVMNode && RigVMNode->NodeGuid.IsValid())
			{
				ExpectedEditorGuids.Add(RigVMNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			}
		}
	}
	TSet<FString> ExportedGuids;
	TMap<FString, const FRigNodeAST*> ExportedFunctionReferences;
	auto InspectGraph = [this, &ExportedGuids, &ExportedFunctionReferences](const FRigGraphAST& Graph)
	{
		for (const FRigNodeAST& Node : Graph.Nodes)
		{
			ExportedGuids.Add(Node.Guid);
			if (Node.Kind == ERigNodeKind::Call)
			{
				const FString Identifier = Node.FunctionIdentifier.ToStableId();
				ExportedFunctionReferences.Add(Identifier, &Node);
				TestTrue(TEXT("Function reference exports a complete typed identifier"),
					Node.FunctionIdentifier.IsComplete());
				TestFalse(TEXT("Function reference exports a parser-safe readable symbol"),
					Node.FunctionName.IsEmpty());
			}
		}
	};
	for (const FRigGraphAST& Graph : First.Module->Graphs) InspectGraph(Graph);
	for (const FString& ExpectedGuid : ExpectedEditorGuids)
	{
		TestTrue(TEXT("Every real editor NodeGuid exports"), ExportedGuids.Contains(ExpectedGuid));
	}
	TSet<FString> ExpectedFunctionReferences;
	TMap<FString, int32> SourceFunctionReferenceCounts;
	for (const URigVMGraph* Model : Blueprint->GetAllModels())
	{
		if (!Model) continue;
		for (const URigVMNode* ModelNode : Model->GetNodes())
		{
			if (const URigVMFunctionReferenceNode* FunctionNode = Cast<URigVMFunctionReferenceNode>(ModelNode))
			{
				const FRigVMGraphFunctionHeader Header = FunctionNode->GetReferencedFunctionHeader();
				FRigFunctionIdentifierAST IdentifierAST;
				IdentifierAST.HostObject = Header.LibraryPointer.HostObject.ToString();
				IdentifierAST.LibraryNodePath = Header.LibraryPointer.GetLibraryNodePath();
				const FString Identifier = IdentifierAST.ToStableId();
				ExpectedFunctionReferences.Add(Identifier);
				++SourceFunctionReferenceCounts.FindOrAdd(
					Header.LibraryPointer.HostObject.ToString() + TEXT("|")
					+ Header.LibraryPointer.GetLibraryNodePath());
				if (const FRigNodeAST* const* ExportedNode = ExportedFunctionReferences.Find(Identifier))
				{
					TestEqual(TEXT("Function reference uses the declaration-compatible readable symbol"),
						(*ExportedNode)->FunctionName,
						RigLangExporterTests::StableRuntimeSymbol(Header.Name.ToString()));
				}
			}
		}
	}
	for (const TPair<FString, int32>& Inventory : SourceFunctionReferenceCounts)
	{
		AddInfo(FString::Printf(TEXT("Real function reference inventory: count=%d identity=%s"),
			Inventory.Value, *Inventory.Key));
	}
	TestTrue(TEXT("Real asset contains exported function references"), !ExpectedFunctionReferences.IsEmpty());
	TestEqual(TEXT("Function reference identity inventory is exact"),
		ExportedFunctionReferences.Num(), ExpectedFunctionReferences.Num());
	for (const FString& ExpectedIdentifier : ExpectedFunctionReferences)
	{
		TestTrue(TEXT("Every function reference resolves to the exact source header target"),
			ExportedFunctionReferences.Contains(ExpectedIdentifier));
	}

	const FString FirstText = First.Module->ToCanonicalString();
	TArray<FRigLangParseError> ParseErrors;
	const TSharedPtr<FRigModuleAST> Parsed = FRigLangParser::Parse(
		FirstText, TEXT("CR_Biped_FootPlacement.riglang"), ParseErrors);
	for (const FRigLangParseError& Error : ParseErrors)
	{
		AddError(TEXT("Exported canonical parse diagnostic: ") + Error.ToString());
	}
	TestTrue(TEXT("Exported RigLang reparses without errors"), ParseErrors.IsEmpty());
	TestNotNull(TEXT("Exported RigLang reparses to a module"), Parsed.Get());
	if (Parsed.IsValid())
	{
		TestEqual(TEXT("Real export parse-print is a canonical fixed point"),
			Parsed->ToCanonicalString(), FirstText);
		TestEqual(TEXT("Real export parse-print hash input is a fixed point"),
			Parsed->ToCanonicalHashInput(), First.Module->ToCanonicalHashInput());
		TestEqual(TEXT("Real export header hash matches canonical hash input"),
			First.Module->Header.ContentHash,
			FRigLangExporter::ComputeContentHash(Parsed->ToCanonicalHashInput()));
	}

	FAnimLispWorkspace Workspace;
	Workspace.AddSource(TEXT("CR_Biped_FootPlacement.riglang"), FirstText);
	const FString AnimSource = FString::Printf(
		TEXT("(anim-module :asset \"/Game/Test/ABP_RigConsumer\" :class \"/Script/Engine.AnimBlueprint\" :version 1 :content-hash \"anim-consumer\")\n")
		TEXT("(import-rig :asset \"%s\" :alias FootPlacement :content-hash \"%s\")\n")
		TEXT("(rig-entry :target \"FootPlacement/ForwardsSolve\")\n"),
		*First.Module->Header.ModuleId.AssetPath,
		*First.Module->Header.ContentHash);
	Workspace.AddSource(TEXT("ABP_RigConsumer.animlang"), AnimSource);
	FAnimLangDiagnostics WorkspaceDiagnostics;
	const bool bWorkspaceBuilt = Workspace.Build(WorkspaceDiagnostics);
	if (!bWorkspaceBuilt)
	{
		for (const FAnimLangDiagnostic& Diagnostic : WorkspaceDiagnostics.Items)
		{
			AddError(TEXT("Real exported rig workspace diagnostic: ") + Diagnostic.ToCompactString());
		}
	}
	TestTrue(TEXT("Real exported rig resolves through an Anim workspace import"), bWorkspaceBuilt);
	TestFalse(TEXT("Real exported rig workspace has no diagnostics"), WorkspaceDiagnostics.HasErrors());
	TestNotNull(TEXT("FootPlacement/ForwardsSolve resolves to the real exported declaration"),
		Workspace.FindDefinition(TEXT("ABP_RigConsumer.animlang"), TEXT("FootPlacement/ForwardsSolve")));
	int32 ExportedCallCount = 0;
	int32 NonInjectedCallCount = 0;
	for (const FRigGraphAST& Graph : First.Module->Graphs)
	{
		for (const FRigNodeAST& Node : Graph.Nodes)
		{
			if (Node.Kind != ERigNodeKind::Call) continue;
			++ExportedCallCount;
			if (!Node.bInjected) ++NonInjectedCallCount;
		}
	}
	int32 SourceDirectCallCount = 0;
	for (const URigVMGraph* Model : Blueprint->GetAllModels())
	{
		if (!Model) continue;
		for (const URigVMNode* Node : Model->GetNodes())
		{
			if (Cast<URigVMFunctionReferenceNode>(Node)) ++SourceDirectCallCount;
		}
	}
	TestEqual(TEXT("Every direct source Rig call exports exactly once"),
		NonInjectedCallCount, SourceDirectCallCount);
	int32 IndexedCallReferences = 0;
	for (const FRigFunctionAST& Function : First.Module->Functions)
	{
		const FAnimLispDefinition* Definition = Workspace.FindDefinition(
			TEXT("CR_Biped_FootPlacement.riglang"), Function.Name);
		if (Definition) IndexedCallReferences += Workspace.FindReferences(Definition->Id).Num();
	}
	TestEqual(TEXT("Every exported local Rig call resolves by exact typed identity"),
		IndexedCallReferences, ExportedCallCount);

	const FRigLangExportResult Second = FRigLangExporter::Export(Blueprint, Options);
	TestTrue(TEXT("Second strict export succeeds"), Second.bSuccess);
	if (Second.Module.IsValid())
	{
		TestEqual(TEXT("Repeated export is byte-for-byte deterministic"),
			Second.Module->ToCanonicalString(), FirstText);
		TestEqual(TEXT("Repeated canonical hash input is byte-for-byte deterministic"),
			Second.Module->ToCanonicalHashInput(), First.Module->ToCanonicalHashInput());
	}
	return true;
}

#endif
