// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#if ANIMBP2FP_HAS_MODERN_RIGVM_AUTHORING
#include "Misc/AutomationTest.h"

#include "RigLangExporter.h"
#include "RigLangImporter.h"
#if ENGINE_MINOR_VERSION >= 7
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#include "ControlRigBlueprintFactory.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "RigVMCore/RigVMExternalVariable.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RigLangHierarchyImportTests
{
const ANIMBP2FP_AUTOMATION_TEST_FLAGS_TYPE Flags =
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter;

UControlRigBlueprint* MakeSourceRig()
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UControlRigBlueprint::StaticClass(), TEXT("CR_HierarchyImportSource")),
		RF_Transient, nullptr, GWarn));
	if (!Blueprint) return nullptr;

	URigHierarchyController* Controller = Blueprint->GetHierarchyController();
	const FRigElementKey Bone = Controller->AddBone(
		TEXT("Root"), FRigElementKey(), FTransform(FVector(10.0, 20.0, 30.0)),
		true, ERigBoneType::User, false);
	const FRigElementKey Space = Controller->AddNull(
		TEXT("Space"), Bone, FTransform(FVector(1.0, 2.0, 3.0)), false, false);
	FRigControlSettings Settings;
	Settings.AnimationType = ERigControlAnimationType::AnimationControl;
	Settings.ControlType = ERigControlType::Float;
	Settings.DisplayName = TEXT("Foot Amount");
	Settings.PrimaryAxis = ERigControlAxis::Z;
	Settings.LimitEnabled.SetNum(1);
	Settings.LimitEnabled[0].bMinimum = true;
	Settings.LimitEnabled[0].bMaximum = true;
	Settings.bDrawLimits = true;
	Settings.bShapeVisible = true;
	Settings.ShapeName = TEXT("Circle_Thick");
	Settings.ShapeColor = FLinearColor(0.1f, 0.3f, 0.7f, 1.0f);
	const FRigElementKey Control = Controller->AddControl(
		TEXT("FootControl"), Space, Settings, FRigControlValue::Make(0.25f),
		FTransform(FVector(4.0, 5.0, 6.0)), FTransform(FVector(7.0, 8.0, 9.0)), false);
	const FRigElementKey Curve = Controller->AddCurve(TEXT("StrideCurve"), 0.625f, false);
	FRigControlSettings NoScaleSettings;
	NoScaleSettings.ControlType = ERigControlType::TransformNoScale;
	FRigControlValue::FTransformNoScale_Float NoScaleValue;
	NoScaleValue.TranslationX = 31.0f; NoScaleValue.TranslationY = 32.0f; NoScaleValue.TranslationZ = 33.0f;
	NoScaleValue.RotationX = 0.0f; NoScaleValue.RotationY = 0.0f; NoScaleValue.RotationZ = 0.0f; NoScaleValue.RotationW = 1.0f;
	const FRigElementKey NoScaleControl = Controller->AddControl(
		TEXT("NoScaleControl"), Space, NoScaleSettings, FRigControlValue::Make(NoScaleValue),
		FTransform::Identity, FTransform::Identity, false);
	FRigControlSettings EulerSettings;
	EulerSettings.ControlType = ERigControlType::EulerTransform;
	FRigControlValue::FEulerTransform_Float EulerValue;
	EulerValue.TranslationX = 41.0f; EulerValue.TranslationY = 42.0f; EulerValue.TranslationZ = 43.0f;
	EulerValue.RotationPitch = 10.0f; EulerValue.RotationYaw = 20.0f; EulerValue.RotationRoll = 30.0f;
	EulerValue.ScaleX = 1.1f; EulerValue.ScaleY = 1.2f; EulerValue.ScaleZ = 1.3f;
	const FRigElementKey EulerControl = Controller->AddControl(
		TEXT("EulerControl"), Space, EulerSettings, FRigControlValue::Make(EulerValue),
		FTransform::Identity, FTransform::Identity, false);

	URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
	Hierarchy->SetCurveValue(Hierarchy->Find<FRigCurveElement>(Curve), 0.625f, false, true);
	const FRigElementKey ParentB = Controller->AddNull(
		TEXT("ParentB"), Bone, FTransform(FVector(-1.0, -2.0, -3.0)), false, false);
	Controller->AddParent(Control, ParentB, 0.25f, false, TEXT("ParentBSpace"), false);
	Hierarchy->SetParentWeight(Control, Space, FRigElementWeight(0.7f, 0.6f, 0.5f), false, false);
	Hierarchy->SetParentWeight(Control, Space, FRigElementWeight(0.4f, 0.3f, 0.2f), true, false);
	Hierarchy->SetParentWeight(Control, ParentB, FRigElementWeight(0.3f, 0.4f, 0.5f), false, false);
	Hierarchy->SetParentWeight(Control, ParentB, FRigElementWeight(0.6f, 0.7f, 0.8f), true, false);
	Hierarchy->SetLocalTransform(Control, FTransform(FVector(11.0, 12.0, 13.0)), true);
	Hierarchy->SetLocalTransform(Control, FTransform(FVector(14.0, 15.0, 16.0)), false);
	Hierarchy->SetControlValue(Control, FRigControlValue::Make(0.25f), ERigControlValueType::Initial);
	Hierarchy->SetControlValue(Control, FRigControlValue::Make(0.75f), ERigControlValueType::Current);
	Hierarchy->SetControlValue(Control, FRigControlValue::Make(-1.0f), ERigControlValueType::Minimum);
	Hierarchy->SetControlValue(Control, FRigControlValue::Make(2.0f), ERigControlValueType::Maximum);
	Hierarchy->SetControlOffsetTransform(Control, FTransform(FVector(4.0, 5.0, 6.0)), true);
	Hierarchy->SetControlOffsetTransform(Control, FTransform(FVector(17.0, 18.0, 19.0)), false);
	Hierarchy->SetControlShapeTransform(Control, FTransform(FVector(7.0, 8.0, 9.0)), true);
	Hierarchy->SetControlShapeTransform(Control, FTransform(FVector(20.0, 21.0, 22.0)), false);
	Hierarchy->SetBoolMetadata(Bone, TEXT("ExportTag"), true);
	Hierarchy->SetNameMetadata(Space, TEXT("SpaceKind"), TEXT("Fixture"));
	Hierarchy->SetVectorMetadata(Control, TEXT("AimAxis"), FVector(0.0, 1.0, 0.0));
	Hierarchy->SetInt32ArrayMetadata(Control, TEXT("Channels"), {2, 4, 8});
	Hierarchy->SetRigElementKeyMetadata(Control, TEXT("DrivenBone"), Bone);
	Hierarchy->SetRigElementKeyArrayMetadata(Control, TEXT("Spaces"), {Space, ParentB});
	Hierarchy->SetControlValue(NoScaleControl, FRigControlValue::Make(NoScaleValue), ERigControlValueType::Initial);
	Hierarchy->SetControlValue(EulerControl, FRigControlValue::Make(EulerValue), ERigControlValueType::Initial);
	if (FRigControlElement* MutableControl = Hierarchy->Find<FRigControlElement>(Control))
	{
		MutableControl->PreferredEulerAngles.RotationOrder = EEulerRotationOrder::ZYX;
		MutableControl->PreferredEulerAngles.Initial = FVector(1.0, 2.0, 3.0);
		MutableControl->PreferredEulerAngles.Current = FVector(4.0, 5.0, 6.0);
	}
	Blueprint->AddMemberVariable(TEXT("bEnabled"), TEXT("bool"), true, false, TEXT("True"));
	const FGuid PointsGuid(0x12345678, 0x90abcdef, 0x11223344, 0x55667788);
	static_cast<IRigVMEditorAssetInterface*>(Blueprint)->AddHostMemberVariableFromExternal(FRigVMExternalVariable::Make(
		PointsGuid, TEXT("Points"), TEXT("TArray<FVector>"), TBaseStructure<FVector>::Get(), true, false),
		TEXT("((X=1.000000,Y=2.000000,Z=3.000000))"));
	return Blueprint;
}

FString HierarchyVariableSemanticInput(const FRigModuleAST& Source)
{
	FRigModuleAST Copy = Source;
	Copy.Imports.Reset();
	Copy.Graphs.Reset();
	Copy.Functions.Reset();
	Copy.Entries.Reset();
	Copy.Header.ContentHash.Reset();
	for (FRigVariableAST& Variable : Copy.Variables) Variable.StableId = Variable.Name;
	return Copy.ToCanonicalHashInput();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangHierarchyImportRoundTripTest,
	"AnimBP2FP.RigLang.Importer.HierarchyVariablesRoundTrip",
	RigLangHierarchyImportTests::Flags)

bool FRigLangHierarchyImportRoundTripTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* SourceBlueprint = RigLangHierarchyImportTests::MakeSourceRig();
	if (!TestNotNull(TEXT("source hierarchy fixture is created"), SourceBlueprint)) return false;
	const FRigLangExportResult SourceExport = FRigLangExporter::Export(SourceBlueprint);
	if (!TestTrue(TEXT("source hierarchy fixture exports"), SourceExport.bSuccess)
		|| !TestNotNull(TEXT("source module exists"), SourceExport.Module.Get())) return false;

	FRigLangImportOptions Options;
	Options.TargetPackage = TEXT("/Engine/Transient/CR_HierarchyImportStaging");
	Options.bTransient = true;
	Options.bStrict = true;
	const FRigLangImportResult Imported = FRigLangImporter::Import(*SourceExport.Module, Options);
	if (!TestNotNull(TEXT("staging import creates a Control Rig blueprint"), Imported.Blueprint.Get()))
	{
		AddError(Imported.Diagnostics.ToReport());
		return false;
	}
	TestFalse(TEXT("Task 8 does not compile graph content"), Imported.bCompiled);
	TestFalse(TEXT("staging import has no fatal diagnostics"), Imported.Diagnostics.HasErrors());

	URigHierarchy* Hierarchy = Imported.Blueprint->GetHierarchy();
	const FRigElementKey Bone(TEXT("Root"), ERigElementType::Bone);
	const FRigElementKey Space(TEXT("Space"), ERigElementType::Null);
	const FRigElementKey Control(TEXT("FootControl"), ERigElementType::Control);
	const FRigElementKey Curve(TEXT("StrideCurve"), ERigElementType::Curve);
	TestTrue(TEXT("bone key is exact"), Hierarchy->Contains(Bone));
	TestTrue(TEXT("null key is exact"), Hierarchy->Contains(Space));
	TestTrue(TEXT("control key is exact"), Hierarchy->Contains(Control));
	TestTrue(TEXT("curve key is exact"), Hierarchy->Contains(Curve));
	TestEqual(TEXT("null parent is the bone"), Hierarchy->GetFirstParent(Space), Bone);
	TestEqual(TEXT("control parent is the null"), Hierarchy->GetFirstParent(Control), Space);
	TestTrue(TEXT("initial control transform is exact"),
		Hierarchy->GetInitialLocalTransform(Control).Equals(
			SourceBlueprint->GetHierarchy()->GetInitialLocalTransform(Control), 0.0));
	TestEqual(TEXT("curve default is exact"), Hierarchy->GetCurveValue(Curve), 0.625f);
	const TArray<FRigElementKey> ImportedParents = Hierarchy->GetParents(Control, false);
	const TArray<FRigElementKey> SourceParents = SourceBlueprint->GetHierarchy()->GetParents(Control, false);
	TestEqual(TEXT("all typed parents are reconstructed"), ImportedParents, SourceParents);
	for (const FRigElementKey& Parent : SourceParents)
	{
		const FRigElementWeight ImportedCurrent = Hierarchy->GetParentWeight(Control, Parent, false);
		const FRigElementWeight SourceCurrent = SourceBlueprint->GetHierarchy()->GetParentWeight(Control, Parent, false);
		const FRigElementWeight ImportedInitial = Hierarchy->GetParentWeight(Control, Parent, true);
		const FRigElementWeight SourceInitial = SourceBlueprint->GetHierarchy()->GetParentWeight(Control, Parent, true);
		TestEqual(TEXT("current typed parent location weight is exact"), ImportedCurrent.Location, SourceCurrent.Location);
		TestEqual(TEXT("current typed parent rotation weight is exact"), ImportedCurrent.Rotation, SourceCurrent.Rotation);
		TestEqual(TEXT("current typed parent scale weight is exact"), ImportedCurrent.Scale, SourceCurrent.Scale);
		TestEqual(TEXT("initial typed parent location weight is exact"), ImportedInitial.Location, SourceInitial.Location);
		TestEqual(TEXT("initial typed parent rotation weight is exact"), ImportedInitial.Rotation, SourceInitial.Rotation);
		TestEqual(TEXT("initial typed parent scale weight is exact"), ImportedInitial.Scale, SourceInitial.Scale);
	}
	TestTrue(TEXT("bool metadata is exact"), Hierarchy->GetBoolMetadata(Bone, TEXT("ExportTag"), false));
	TestEqual(TEXT("name metadata is exact"),
		Hierarchy->GetNameMetadata(Space, TEXT("SpaceKind"), NAME_None), FName(TEXT("Fixture")));
	TestEqual(TEXT("vector metadata is exact"),
		Hierarchy->GetVectorMetadata(Control, TEXT("AimAxis"), FVector::ZeroVector), FVector(0.0, 1.0, 0.0));
	TestEqual(TEXT("metadata array is exact"),
		Hierarchy->GetInt32ArrayMetadata(Control, TEXT("Channels")), TArray<int32>({2, 4, 8}));
	TestEqual(TEXT("element-key metadata is exact"),
		Hierarchy->GetRigElementKeyMetadata(Control, TEXT("DrivenBone"), FRigElementKey()), Bone);
	TestEqual(TEXT("element-key array metadata is exact"),
		Hierarchy->GetRigElementKeyArrayMetadata(Control, TEXT("Spaces")),
		TArray<FRigElementKey>({Space, FRigElementKey(TEXT("ParentB"), ERigElementType::Null)}));
	const FRigElementKey NoScaleControl(TEXT("NoScaleControl"), ERigElementType::Control);
	const FRigElementKey EulerControl(TEXT("EulerControl"), ERigElementType::Control);
	const auto ImportedNoScale = Hierarchy->GetControlValue(
		NoScaleControl, ERigControlValueType::Initial).Get<FRigControlValue::FTransformNoScale_Float>();
	const auto ImportedEuler = Hierarchy->GetControlValue(
		EulerControl, ERigControlValueType::Initial).Get<FRigControlValue::FEulerTransform_Float>();
	TestEqual(TEXT("TransformNoScale translation is exact"), ImportedNoScale.TranslationX, 31.0f);
	TestEqual(TEXT("TransformNoScale quaternion is exact"), ImportedNoScale.RotationW, 1.0f);
	TestEqual(TEXT("EulerTransform translation is exact"), ImportedEuler.TranslationY, 42.0f);
	TestEqual(TEXT("EulerTransform rotation is exact"), ImportedEuler.RotationYaw, 20.0f);
	TestEqual(TEXT("EulerTransform scale is exact"), ImportedEuler.ScaleZ, 1.3f);
	const FRigControlElement* ImportedControl = Hierarchy->Find<FRigControlElement>(Control);
	const FRigControlElement* SourceControl = SourceBlueprint->GetHierarchy()->Find<FRigControlElement>(Control);
	if (TestNotNull(TEXT("imported control element exists"), ImportedControl)
		&& TestNotNull(TEXT("source control element exists"), SourceControl))
	{
		TestTrue(TEXT("full reflected control settings are exact"),
			FRigControlSettings::StaticStruct()->CompareScriptStruct(
				&ImportedControl->Settings, &SourceControl->Settings, PPF_None));
	}
	const TArray<FRigVMGraphVariableDescription> Variables = Imported.Blueprint->GetMemberVariables();
	const FRigVMGraphVariableDescription* Enabled = Variables.FindByPredicate(
		[](const FRigVMGraphVariableDescription& Variable) { return Variable.Name == TEXT("bEnabled"); });
	if (TestNotNull(TEXT("member variable is reconstructed"), Enabled))
	{
		TestEqual(TEXT("member variable type is exact"), Enabled->CPPType, FString(TEXT("bool")));
		TestEqual(TEXT("member variable default is exact"), Enabled->DefaultValue, FString(TEXT("True")));
		TestTrue(TEXT("member variable public input is exact"), Enabled->bPublic);
	}
	const FRigVMGraphVariableDescription* Points = Variables.FindByPredicate(
		[](const FRigVMGraphVariableDescription& Variable) { return Variable.Name == TEXT("Points"); });
	if (TestNotNull(TEXT("array type-object variable is reconstructed"), Points))
	{
		TestEqual(TEXT("member variable Guid is exact"), Points->Guid,
			FGuid(0x12345678, 0x90abcdef, 0x11223344, 0x55667788));
		TestEqual(TEXT("array member extended CPP type is exact"), Points->CPPType, FString(TEXT("TArray<FVector>")));
		TestTrue(TEXT("array member container is exact"), Points->ToExternalVariable().IsArray());
		TestTrue(TEXT("array member type object is exact"),
			Points->CPPTypeObject.Get() == TBaseStructure<FVector>::Get());
		TestEqual(TEXT("array member default is exact"), Points->DefaultValue,
			FString(TEXT("((X=1.000000,Y=2.000000,Z=3.000000))")));
		TestTrue(TEXT("array member access is exact"), Points->bPublic);
	}

	const FRigLangExportResult ReExport = FRigLangExporter::Export(Imported.Blueprint);
	if (!TestTrue(TEXT("imported hierarchy immediately re-exports"), ReExport.bSuccess)
		|| !TestNotNull(TEXT("re-exported module exists"), ReExport.Module.Get())) return false;
	FRigModuleAST NormalizedReExport = *ReExport.Module;
	NormalizedReExport.Header = SourceExport.Module->Header;
	TestEqual(TEXT("hierarchy and variable semantic diff is zero"),
		RigLangHierarchyImportTests::HierarchyVariableSemanticInput(NormalizedReExport),
		RigLangHierarchyImportTests::HierarchyVariableSemanticInput(*SourceExport.Module));

	FRigModuleAST NameFallbackModule = *SourceExport.Module;
	FRigVariableAST* NameFallbackVariable = NameFallbackModule.Variables.FindByPredicate(
		[](const FRigVariableAST& Variable) { return Variable.Name == TEXT("bEnabled"); });
	if (TestNotNull(TEXT("name-fallback variable fixture exists"), NameFallbackVariable))
	{
		NameFallbackVariable->StableId = NameFallbackVariable->Name;
		const FRigLangImportResult NameFallbackImport = FRigLangImporter::Import(NameFallbackModule, Options);
		if (!TestNotNull(TEXT("name-fallback variable round-trip imports"), NameFallbackImport.Blueprint.Get()))
		{
			AddError(NameFallbackImport.Diagnostics.ToReport());
			return false;
		}
		const TArray<FRigVMGraphVariableDescription> FallbackVariables =
			NameFallbackImport.Blueprint->GetMemberVariables();
		const FRigVMGraphVariableDescription* ReflectedFallback =
			FallbackVariables.FindByPredicate(
				[](const FRigVMGraphVariableDescription& Variable) { return Variable.Name == TEXT("bEnabled"); });
		if (TestNotNull(TEXT("name-fallback variable is reflected"), ReflectedFallback))
		{
			TestTrue(TEXT("name-fallback variable receives an engine identity"), ReflectedFallback->Guid.IsValid());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangHierarchyImportPreflightTest,
	"AnimBP2FP.RigLang.Importer.PreflightRejectsInvalidInput",
	RigLangHierarchyImportTests::Flags)

bool FRigLangHierarchyImportPreflightTest::RunTest(const FString& Parameters)
{
	auto ExpectRejectedBeforePackage = [this](FRigModuleAST Module, const FString& PackageName, const FString& Context)
	{
		FRigLangImportOptions Options;
		Options.TargetPackage = PackageName;
		Options.bTransient = false;
		Options.bStrict = true;
		TestNull(Context + TEXT(" package absent before import"), FindPackage(nullptr, *PackageName));
		const FRigLangImportResult Result = FRigLangImporter::Import(Module, Options);
		TestNull(Context + TEXT(" returns no blueprint"), Result.Blueprint.Get());
		TestTrue(Context + TEXT(" reports fatal diagnostics"), Result.Diagnostics.HasErrors());
		TestNull(Context + TEXT(" does not create a package"), FindPackage(nullptr, *PackageName));
	};

	FRigModuleAST MissingParent;
	FRigHierarchyElementAST& Child = MissingParent.Hierarchy.AddDefaulted_GetRef();
	Child.Kind = ERigHierarchyElementKind::Null;
	Child.Name = TEXT("Child");
	Child.ParentName = TEXT("Missing");
	ExpectRejectedBeforePackage(MissingParent, TEXT("/Game/Tests/CR_MissingParentImport"), TEXT("missing parent"));

	FRigModuleAST Cycle;
	FRigHierarchyElementAST& A = Cycle.Hierarchy.AddDefaulted_GetRef();
	A.Kind = ERigHierarchyElementKind::Null; A.Name = TEXT("A"); A.ParentName = TEXT("B");
	FRigHierarchyElementAST& B = Cycle.Hierarchy.AddDefaulted_GetRef();
	B.Kind = ERigHierarchyElementKind::Null; B.Name = TEXT("B"); B.ParentName = TEXT("A");
	ExpectRejectedBeforePackage(Cycle, TEXT("/Game/Tests/CR_CycleImport"), TEXT("cycle"));

	FRigModuleAST MissingTypedParent;
	FRigHierarchyElementAST& TypedChild = MissingTypedParent.Hierarchy.AddDefaulted_GetRef();
	TypedChild.Kind = ERigHierarchyElementKind::Null;
	TypedChild.StableId = TEXT("Null(TypedChild)");
	TypedChild.Name = TEXT("TypedChild");
	TypedChild.Parents.AddDefaulted_GetRef().StableId = TEXT("Null(MissingTypedParent)");
	ExpectRejectedBeforePackage(
		MissingTypedParent, TEXT("/Game/Tests/CR_MissingTypedParentImport"), TEXT("missing typed parent"));

	FRigModuleAST TypedCycle;
	FRigHierarchyElementAST& TypedA = TypedCycle.Hierarchy.AddDefaulted_GetRef();
	TypedA.Kind = ERigHierarchyElementKind::Null;
	TypedA.StableId = TEXT("Null(TypedA)");
	TypedA.Name = TEXT("TypedA");
	TypedA.Parents.AddDefaulted_GetRef().StableId = TEXT("Null(TypedB)");
	FRigHierarchyElementAST& TypedB = TypedCycle.Hierarchy.AddDefaulted_GetRef();
	TypedB.Kind = ERigHierarchyElementKind::Null;
	TypedB.StableId = TEXT("Null(TypedB)");
	TypedB.Name = TEXT("TypedB");
	TypedB.Parents.AddDefaulted_GetRef().StableId = TEXT("Null(TypedA)");
	ExpectRejectedBeforePackage(
		TypedCycle, TEXT("/Game/Tests/CR_TypedParentCycleImport"), TEXT("typed parent cycle"));

	FRigModuleAST DuplicateTypedParent;
	FRigHierarchyElementAST& Parent = DuplicateTypedParent.Hierarchy.AddDefaulted_GetRef();
	Parent.Kind = ERigHierarchyElementKind::Null;
	Parent.StableId = TEXT("Null(Parent)");
	Parent.Name = TEXT("Parent");
	FRigHierarchyElementAST& DuplicateChild = DuplicateTypedParent.Hierarchy.AddDefaulted_GetRef();
	DuplicateChild.Kind = ERigHierarchyElementKind::Null;
	DuplicateChild.StableId = TEXT("Null(DuplicateChild)");
	DuplicateChild.Name = TEXT("DuplicateChild");
	DuplicateChild.ParentName = TEXT("Parent");
	DuplicateChild.Parents.AddDefaulted_GetRef().StableId = Parent.StableId;
	DuplicateChild.Parents.AddDefaulted_GetRef().StableId = Parent.StableId;
	ExpectRejectedBeforePackage(
		DuplicateTypedParent, TEXT("/Game/Tests/CR_DuplicateTypedParentImport"), TEXT("duplicate typed parent"));

	FRigModuleAST InvalidBoneType;
	FRigHierarchyElementAST& InvalidBone = InvalidBoneType.Hierarchy.AddDefaulted_GetRef();
	InvalidBone.Kind = ERigHierarchyElementKind::Bone;
	InvalidBone.StableId = TEXT("Bone(InvalidBone)");
	InvalidBone.Name = TEXT("InvalidBone");
	FRigHierarchyStateAST& InvalidBoneState = InvalidBone.States.AddDefaulted_GetRef();
	InvalidBoneState.Kind = ERigHierarchyStateKind::BoneType;
	InvalidBoneState.Type = TEXT("NotABoneType");
	ExpectRejectedBeforePackage(
		InvalidBoneType, TEXT("/Game/Tests/CR_InvalidBoneTypeImport"), TEXT("invalid bone enum"));

	FRigModuleAST InvalidControlValue;
	FRigHierarchyElementAST& InvalidControl = InvalidControlValue.Hierarchy.AddDefaulted_GetRef();
	InvalidControl.Kind = ERigHierarchyElementKind::Control;
	InvalidControl.StableId = TEXT("Control(InvalidControl)");
	InvalidControl.Name = TEXT("InvalidControl");
	FRigControlSettings InvalidControlSettings;
	InvalidControlSettings.ControlType = ERigControlType::TransformNoScale;
	FRigHierarchyStateAST& InvalidSettingsState = InvalidControl.States.AddDefaulted_GetRef();
	InvalidSettingsState.Kind = ERigHierarchyStateKind::ControlSettings;
	InvalidSettingsState.Type = TEXT("TransformNoScale");
	FRigControlSettings::StaticStruct()->ExportText(
		InvalidSettingsState.SerializedValue, &InvalidControlSettings, nullptr, nullptr, PPF_None, nullptr);
	FRigHierarchyStateAST& InvalidValueState = InvalidControl.States.AddDefaulted_GetRef();
	InvalidValueState.Kind = ERigHierarchyStateKind::ControlValue;
	InvalidValueState.Role = TEXT("initial");
	InvalidValueState.Type = TEXT("TransformNoScale");
	InvalidValueState.Components = {1.0, 2.0, 3.0, 0.0, 0.0, 1.0};
	ExpectRejectedBeforePackage(
		InvalidControlValue, TEXT("/Game/Tests/CR_InvalidControlValueImport"), TEXT("invalid control components"));

	FRigModuleAST InvalidEulerEnum = InvalidControlValue;
	InvalidEulerEnum.Hierarchy[0].States[1].Components = {1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 1.0};
	FRigHierarchyStateAST& InvalidEuler = InvalidEulerEnum.Hierarchy[0].States.AddDefaulted_GetRef();
	InvalidEuler.Kind = ERigHierarchyStateKind::PreferredEuler;
	InvalidEuler.Role = TEXT("initial");
	InvalidEuler.Type = TEXT("NotAnEulerOrder");
	InvalidEuler.Components = {1.0, 2.0, 3.0};
	ExpectRejectedBeforePackage(
		InvalidEulerEnum, TEXT("/Game/Tests/CR_InvalidEulerEnumImport"), TEXT("invalid Euler enum"));

	FRigModuleAST InvalidMetadata;
	FRigHierarchyElementAST& MetadataBone = InvalidMetadata.Hierarchy.AddDefaulted_GetRef();
	MetadataBone.Kind = ERigHierarchyElementKind::Bone;
	MetadataBone.StableId = TEXT("Bone(MetadataBone)");
	MetadataBone.Name = TEXT("MetadataBone");
	FRigHierarchyMetadataAST& FirstTag = MetadataBone.Metadata.AddDefaulted_GetRef();
	FirstTag.Name = TEXT("DuplicateTag"); FirstTag.Kind = ERigHierarchyMetadataValueKind::Bool; FirstTag.BoolValues = {true};
	FRigHierarchyMetadataAST& SecondTag = MetadataBone.Metadata.AddDefaulted_GetRef();
	SecondTag.Name = TEXT("DuplicateTag"); SecondTag.Kind = ERigHierarchyMetadataValueKind::Bool; SecondTag.BoolValues = {false};
	ExpectRejectedBeforePackage(
		InvalidMetadata, TEXT("/Game/Tests/CR_DuplicateMetadataImport"), TEXT("duplicate metadata"));

	FRigModuleAST InvalidElementKeyMetadata;
	FRigHierarchyElementAST& KeyBone = InvalidElementKeyMetadata.Hierarchy.AddDefaulted_GetRef();
	KeyBone.Kind = ERigHierarchyElementKind::Bone;
	KeyBone.StableId = TEXT("Bone(KeyBone)");
	KeyBone.Name = TEXT("KeyBone");
	FRigHierarchyMetadataAST& MissingKeys = KeyBone.Metadata.AddDefaulted_GetRef();
	MissingKeys.Name = TEXT("MissingKeys");
	MissingKeys.Kind = ERigHierarchyMetadataValueKind::ElementKeyArray;
	MissingKeys.StringValues = {TEXT("not-a-rig-element-key")};
	ExpectRejectedBeforePackage(
		InvalidElementKeyMetadata, TEXT("/Game/Tests/CR_InvalidElementKeyMetadataImport"), TEXT("invalid element-key metadata"));

	FRigModuleAST UnsupportedContainer;
	FRigVariableAST& MapVariable = UnsupportedContainer.Variables.AddDefaulted_GetRef();
	MapVariable.Name = TEXT("BadMap"); MapVariable.Type.CPPType = TEXT("float");
	MapVariable.Type.ContainerType = TEXT("map");
	ExpectRejectedBeforePackage(
		UnsupportedContainer, TEXT("/Game/Tests/CR_UnsupportedContainerImport"), TEXT("unsupported container"));

	FRigModuleAST MissingTypeObject;
	FRigVariableAST& ObjectVariable = MissingTypeObject.Variables.AddDefaulted_GetRef();
	ObjectVariable.Name = TEXT("BadTypeObject"); ObjectVariable.Type.CPPType = TEXT("FMissingType");
	ObjectVariable.Type.CPPTypeObject = TEXT("/Script/DefinitelyMissing.MissingType");
	ExpectRejectedBeforePackage(
		MissingTypeObject, TEXT("/Game/Tests/CR_MissingTypeObjectImport"), TEXT("missing type object"));

	FRigModuleAST PublicOutput;
	FRigVariableAST& OutputVariable = PublicOutput.Variables.AddDefaulted_GetRef();
	OutputVariable.Name = TEXT("UnsupportedOutput");
	OutputVariable.StableId = TEXT("UnsupportedOutput");
	OutputVariable.Access = ERigVariableAccess::PublicOutput;
	OutputVariable.Type.CPPType = TEXT("float");
	ExpectRejectedBeforePackage(
		PublicOutput, TEXT("/Game/Tests/CR_PublicOutputImport"), TEXT("public output variable"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangHierarchyImportRollbackTest,
	"AnimBP2FP.RigLang.Importer.PostCreationFailureRollsBack",
	RigLangHierarchyImportTests::Flags)

bool FRigLangHierarchyImportRollbackTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* SourceBlueprint = RigLangHierarchyImportTests::MakeSourceRig();
	if (!TestNotNull(TEXT("rollback source fixture is created"), SourceBlueprint)) return false;
	const FRigLangExportResult Exported = FRigLangExporter::Export(SourceBlueprint);
	if (!TestTrue(TEXT("rollback source fixture exports"), Exported.bSuccess)
		|| !TestNotNull(TEXT("rollback source module exists"), Exported.Module.Get())) return false;
	FRigModuleAST Mismatched = *Exported.Module;
	Mismatched.Hierarchy[0].Properties.Add(TEXT("UnrestorableReviewField"), TEXT("true"));
	const FString PackageName = TEXT("/Game/Tests/CR_PostCreationRollbackImport");
	TestNull(TEXT("rollback package absent before import"), FindPackage(nullptr, *PackageName));
	FRigLangImportOptions Options;
	Options.TargetPackage = PackageName;
	Options.bTransient = false;
	Options.bStrict = true;
	const FRigLangImportResult Result = FRigLangImporter::Import(Mismatched, Options);
	TestNull(TEXT("post-creation semantic failure returns no blueprint"), Result.Blueprint.Get());
	TestTrue(TEXT("post-creation semantic failure reports diagnostics"), Result.Diagnostics.HasErrors());
	TestNull(TEXT("post-creation semantic failure removes package"), FindPackage(nullptr, *PackageName));
	return true;
}

#endif

#endif // UE 5.4+
