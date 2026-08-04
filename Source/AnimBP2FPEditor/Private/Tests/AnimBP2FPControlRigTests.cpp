// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"

#if ENGINE_MAJOR_VERSION >= 5
#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "AnimLangParser.h"
#include "AnimLispWorkspace.h"
#include "RigLangExporter.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_ControlRig.h"
#if ENGINE_MINOR_VERSION >= 7
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString NormalizePinName(const FString& Value)
	{
		FString Result = Value.ToLower();
		Result.ReplaceInline(TEXT("_"), TEXT(""));
		Result.ReplaceInline(TEXT("-"), TEXT(""));
		Result.ReplaceInline(TEXT(" "), TEXT(""));
		return Result;
	}

	UAnimGraphNode_Base* FindControlRigNode(UAnimBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(GraphNode);
				if (Node && Node->GetClass()->GetName() == TEXT("AnimGraphNode_ControlRig")) return Node;
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindInputPin(UAnimGraphNode_Base* Node, const FString& Name)
	{
		const FString NormalizedName = NormalizePinName(Name);
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input
				&& NormalizePinName(Pin->PinName.ToString()) == NormalizedName)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	FString ExportControlRigReference(UAnimGraphNode_Base* Node)
	{
		for (TFieldIterator<FStructProperty> StructIt(Node->GetClass()); StructIt; ++StructIt)
		{
			FStructProperty* NodeProperty = *StructIt;
			if (!NodeProperty->Struct || !NodeProperty->Struct->IsChildOf(FAnimNode_Base::StaticStruct())) continue;
			if (FProperty* ReferenceProperty = NodeProperty->Struct->FindPropertyByName(TEXT("ControlRigAssetReference")))
			{
				FString Value;
				void* NodeMemory = NodeProperty->ContainerPtrToValuePtr<void>(Node);
				ReferenceProperty->ExportText_Direct(Value,
					ReferenceProperty->ContainerPtrToValuePtr<void>(NodeMemory), nullptr, Node, PPF_None);
				return Value;
			}
		}
		return FString();
	}

	TArray<TSharedPtr<FAnimNodeAST>> CollectControlRigASTs(const FAnimGraphAST& AST)
	{
		TArray<TSharedPtr<FAnimNodeAST>> Nodes;
		AST.VisitNodes([&Nodes](const TSharedPtr<FAnimNodeAST>& Node)
		{
			if (Node->NodeType == TEXT("control-rig")) Nodes.Add(Node);
		});
		return Nodes;
	}

	TSet<FName> GetVisibleCustomPinPropertyNames(UAnimGraphNode_Base* Node)
	{
		TSet<FName> Names;
		FArrayProperty* ArrayProperty = Node
			? FindFProperty<FArrayProperty>(Node->GetClass(), TEXT("CustomPinProperties")) : nullptr;
		FStructProperty* ElementProperty = ArrayProperty
			? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		if (!ElementProperty || !ElementProperty->Struct) return Names;
		FNameProperty* NameProperty = FindFProperty<FNameProperty>(ElementProperty->Struct, TEXT("PropertyName"));
		FBoolProperty* ShowProperty = FindFProperty<FBoolProperty>(ElementProperty->Struct, TEXT("bShowPin"));
		if (!NameProperty || !ShowProperty) return Names;
		FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Node));
		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			const void* Element = ArrayHelper.GetRawPtr(Index);
			if (ShowProperty->GetPropertyValue_InContainer(Element))
			{
				Names.Add(NameProperty->GetPropertyValue_InContainer(Element));
			}
		}
		return Names;
	}

	bool ShowOneExtraCustomPinProperty(UAnimGraphNode_Base* Node, const TSet<FName>& BoundNames)
	{
		FArrayProperty* ArrayProperty = Node
			? FindFProperty<FArrayProperty>(Node->GetClass(), TEXT("CustomPinProperties")) : nullptr;
		FStructProperty* ElementProperty = ArrayProperty
			? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		if (!ElementProperty || !ElementProperty->Struct) return false;
		FNameProperty* NameProperty = FindFProperty<FNameProperty>(ElementProperty->Struct, TEXT("PropertyName"));
		FBoolProperty* ShowProperty = FindFProperty<FBoolProperty>(ElementProperty->Struct, TEXT("bShowPin"));
		if (!NameProperty || !ShowProperty) return false;
		FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Node));
		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			void* Element = ArrayHelper.GetRawPtr(Index);
			const FName Name = NameProperty->GetPropertyValue_InContainer(Element);
			if (!BoundNames.Contains(Name))
			{
				ShowProperty->SetPropertyValue_InContainer(Element, true);
				Node->ReconstructNode();
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPControlRigInputsRoundTrip,
	"AnimBP2FP.ControlRig.InputsRoundTrip",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPControlRigInputsRoundTrip::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(
		nullptr, TEXT("/Game/Blueprints/SandboxCharacter_Mover_ABP.SandboxCharacter_Mover_ABP"));
	if (!Source)
	{
		AddInfo(TEXT("SKIPPED: real Mover AnimBlueprint fixture is not installed"));
		return true;
	}

	UAnimGraphNode_Base* SourceNode = FindControlRigNode(Source);
	TestNotNull(TEXT("source Control Rig node exists"), SourceNode);
	if (!SourceNode) return false;
	const FString SourceReference = ExportControlRigReference(SourceNode);
	TestTrue(TEXT("source Control Rig asset reference is populated"),
		SourceReference.Contains(TEXT("CR_Biped_FootPlacement_C")));

	const TSharedPtr<FAnimGraphAST> SourceAST = FAnimBPExporter::ExportToAST(Source);
	TestTrue(TEXT("real Mover AnimBlueprint exports"), SourceAST.IsValid());
	if (!SourceAST.IsValid()) return false;
	const FString SourceDSL = SourceAST->ToString();
	UAnimBlueprint* CorruptedIdentityBlueprint = DuplicateObject<UAnimBlueprint>(
		Source, GetTransientPackage(), TEXT("ABP_ControlRigCorruptedPinIdentity"));
	TestNotNull(TEXT("transient source duplicate is created for exact pin identity validation"),
		CorruptedIdentityBlueprint);
	if (CorruptedIdentityBlueprint)
	{
		UAnimGraphNode_Base* CorruptedNode = FindControlRigNode(CorruptedIdentityBlueprint);
		UEdGraphPin* CorruptedPin = FindInputPin(CorruptedNode, TEXT("GroundNormal"));
		TestNotNull(TEXT("duplicate has the GroundNormal input pin"), CorruptedPin);
		if (CorruptedPin)
		{
			const FString OriginalName = CorruptedPin->PinName.ToString();
			const FString FuzzyEquivalentName = OriginalName + TEXT("_");
			TestEqual(TEXT("corrupted name remains equal under the removed fuzzy identity rule"),
				NormalizePinName(FuzzyEquivalentName), NormalizePinName(OriginalName));
			CorruptedPin->PinName = FName(*FuzzyEquivalentName);
			AddExpectedError(TEXT("[UNSUPPORTED:ControlRigInputIdentity] Exposed property"),
				EAutomationExpectedErrorFlags::Contains, 1);
			TestFalse(TEXT("export rejects a pin without an exact authoritative property identity"),
				FAnimBPExporter::ExportToAST(CorruptedIdentityBlueprint).IsValid());
		}
	}
	TestEqual(TEXT("real Control Rig exports one typed Rig import"), SourceAST->RigImports.Num(), 1);
	if (SourceAST->RigImports.Num() == 1)
	{
		TestEqual(TEXT("typed Rig import uses the source blueprint package"),
			SourceAST->RigImports[0].Target.AssetPath,
			FString(TEXT("/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement")));
		TestEqual(TEXT("typed Rig import alias is stable semantic identity"),
			SourceAST->RigImports[0].Alias, FString(TEXT("FootPlacement")));
		TestTrue(TEXT("typed Rig import carries the exported module hash"),
			SourceAST->RigImports[0].ExpectedHash.StartsWith(TEXT("sha256:")));
	}
	const TArray<TSharedPtr<FAnimNodeAST>> ControlRigNodes = CollectControlRigASTs(*SourceAST);
	TestEqual(TEXT("real Anim export contains exactly one Control Rig node"), ControlRigNodes.Num(), 1);
	const TSharedPtr<FAnimNodeAST> ControlRigAST = ControlRigNodes.Num() == 1 ? ControlRigNodes[0] : nullptr;
	TestTrue(TEXT("real Control Rig exports a typed node binding"),
		ControlRigAST.IsValid() && ControlRigAST->RigBinding.IsSet());
	if (!ControlRigAST.IsValid() || !ControlRigAST->RigBinding.IsSet()) return false;
	const FAnimRigNodeBinding& RigBinding = ControlRigAST->RigBinding.GetValue();
	TestEqual(TEXT("typed Control Rig entry resolves uniquely"),
		RigBinding.EntryName, FString(TEXT("ForwardsSolve")));
	TestEqual(TEXT("all eight exposed Rig inputs have typed bindings"), RigBinding.Inputs.Num(), 8);
	const TMap<FString, FString> ExpectedTypes = {
		{ TEXT("GroundNormal"), TEXT("FVector") },
		{ TEXT("DebugDraw"), TEXT("bool") },
		{ TEXT("EnableFootPinning"), TEXT("bool") },
		{ TEXT("EnableSlopeWarping"), TEXT("bool") },
		{ TEXT("HasTeleported"), TEXT("bool") },
		{ TEXT("WorldZDamperEnabled"), TEXT("bool") },
		{ TEXT("ForceReset"), TEXT("bool") },
		{ TEXT("DoRaycast"), TEXT("bool") } };
	for (const TPair<FString, FString>& Expected : ExpectedTypes)
	{
		const FAnimRigInputBinding* Input = RigBinding.Inputs.FindByPredicate(
			[&Expected](const FAnimRigInputBinding& Candidate)
			{ return Candidate.RigInputName == Expected.Key; });
		TestNotNull(TEXT("typed Rig input exists: ") + Expected.Key, Input);
		if (!Input) continue;
		TestEqual(TEXT("typed Rig input CPP type is exact: ") + Expected.Key,
			Input->ResolvedType.CPPType, Expected.Value);
		TestTrue(TEXT("typed Rig input has a structured value expression: ") + Expected.Key,
			Input->ValueExpression.StartsWith(TEXT("(")) && Input->ValueExpression.EndsWith(TEXT(")")));
	}
	TArray<FAnimLangParseError> ReparseErrors;
	const TSharedPtr<FAnimGraphAST> ReparsedAST = FAnimLangParser::Parse(SourceDSL, ReparseErrors);
	TestTrue(TEXT("real typed Control Rig canonical DSL reparses without diagnostics"),
		ReparsedAST.IsValid() && ReparseErrors.Num() == 0);
	if (ReparsedAST.IsValid())
	{
		TestEqual(TEXT("canonical reparse retains the typed Rig asset path"),
			ReparsedAST->RigImports.Num() == 1
				? ReparsedAST->RigImports[0].Target.AssetPath : FString(),
			FString(TEXT("/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement")));
		const TArray<TSharedPtr<FAnimNodeAST>> ReparsedNodes = CollectControlRigASTs(*ReparsedAST);
		TestEqual(TEXT("canonical reparse retains exactly one Control Rig node"), ReparsedNodes.Num(), 1);
		if (ReparsedNodes.Num() == 1 && ReparsedNodes[0]->RigBinding.IsSet())
		{
			TestEqual(TEXT("canonical reparse retains all eight typed Rig inputs"),
				ReparsedNodes[0]->RigBinding.GetValue().Inputs.Num(), 8);
		}
		if (ReparsedNodes.Num() == 1)
		{
			TestEqual(TEXT("real typed Control Rig node reaches a canonical fixed point"),
				ReparsedNodes[0]->ToString(), ControlRigAST->ToString());
		}
	}
	TestFalse(TEXT("typed Control Rig DSL suppresses the legacy exposed input manifest"),
		SourceDSL.Contains(TEXT(":exposed-input-pins")));

	UAnimGraphNode_ControlRig* TypedSourceNode = Cast<UAnimGraphNode_ControlRig>(SourceNode);
	UControlRigBlueprint* RigBlueprint = nullptr;
#if ENGINE_MINOR_VERSION >= 8
	RigBlueprint = TypedSourceNode
		? Cast<UControlRigBlueprint>(TypedSourceNode->Node.GetControlRigAssetReference().GetEditorAsset())
		: nullptr;
#else
	UClass* RigClass = TypedSourceNode ? TypedSourceNode->Node.GetControlRigClass().Get() : nullptr;
	RigBlueprint = RigClass ? Cast<UControlRigBlueprint>(RigClass->ClassGeneratedBy) : nullptr;
#endif
	TestNotNull(TEXT("source Control Rig blueprint resolves for workspace lint"), RigBlueprint);
	if (!RigBlueprint) return false;
	const FRigLangExportResult RigExport = FRigLangExporter::Export(RigBlueprint);
	TestTrue(TEXT("source Control Rig exports for workspace lint"), RigExport.bSuccess && RigExport.Module.IsValid());
	if (!RigExport.bSuccess || !RigExport.Module.IsValid()) return false;
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(TEXT("CR_Biped_FootPlacement.riglang"), RigExport.Module->ToCanonicalString());
	Workspace.AddSource(TEXT("SandboxCharacter_Mover_ABP.animlang"), SourceDSL);
	FAnimLangDiagnostics WorkspaceDiagnostics;
	const bool bWorkspaceBuilt = Workspace.Build(WorkspaceDiagnostics);
	if (!bWorkspaceBuilt)
	{
		for (const FAnimLangDiagnostic& Diagnostic : WorkspaceDiagnostics.Items)
		{
			AddInfo(TEXT("WORKSPACE-DIAGNOSTIC ") + Diagnostic.ToString());
		}
	}
	TestTrue(TEXT("real generated RigLang and AnimLang pair passes workspace lint"), bWorkspaceBuilt);
	TestFalse(TEXT("real generated module pair has zero workspace errors"), WorkspaceDiagnostics.HasErrors());
	const FAnimLispDefinition* EntryDefinition = Workspace.FindDefinition(
		TEXT("SandboxCharacter_Mover_ABP.animlang"), TEXT("FootPlacement/ForwardsSolve"));
	TestNotNull(TEXT("FootPlacement/ForwardsSolve resolves through the real generated pair"), EntryDefinition);
	if (EntryDefinition)
	{
		TestEqual(TEXT("resolved runtime symbol is a Rig entry"), EntryDefinition->Id.Kind,
			EAnimLispSymbolKind::RigEntry);
		TestEqual(TEXT("typed Control Rig entry contributes one Anim reference"),
			Workspace.FindReferences(EntryDefinition->Id).Num(), 1);
	}
	for (const TPair<FString, FString>& Expected : ExpectedTypes)
	{
		const FAnimLispDefinition* InputDefinition = Workspace.FindDefinition(
			TEXT("SandboxCharacter_Mover_ABP.animlang"), TEXT("FootPlacement/") + Expected.Key);
		TestNotNull(TEXT("workspace resolves public Rig input: ") + Expected.Key, InputDefinition);
		if (!InputDefinition) continue;
		TestEqual(TEXT("workspace resolves exact public Rig input type: ") + Expected.Key,
			InputDefinition->TypeSignature.CPPType, Expected.Value);
		TestEqual(TEXT("typed Rig input contributes one Anim reference: ") + Expected.Key,
			Workspace.FindReferences(InputDefinition->Id).Num(), 1);
	}
	const TArray<FAnimLispCompletion> RuntimeCompletions = Workspace.Complete(
		TEXT("SandboxCharacter_Mover_ABP.animlang"), EAnimLispCapability::AnimRuntimeReference);
	for (const FRigFunctionAST& Function : RigExport.Module->Functions)
	{
		if (Function.Visibility != TEXT("internal")) continue;
		TestFalse(TEXT("internal Rig function is excluded from Anim runtime completion: ") + Function.Name,
			RuntimeCompletions.ContainsByPredicate([&Function](const FAnimLispCompletion& Completion)
			{
				return Completion.Id.Kind == EAnimLispSymbolKind::RigFunction
					&& Completion.Id.QualifiedName == Function.Name;
			}));
	}

	UClass* ParentClass = Source->ParentClass ? Source->ParentClass.Get() : UAnimInstance::StaticClass();
	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		ParentClass, GetTransientPackage(), TEXT("ABP_ControlRigInputsRoundTrip"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPControlRigTest"))));
	TestNotNull(TEXT("transient destination AnimBlueprint is created"), Destination);
	if (!Destination) return false;
	Destination->TargetSkeleton = Source->TargetSkeleton;

	const FAnimBPImporter::FUpdateResult ImportResult =
		FAnimBPImporter::UpdateBlueprintDetailed(Destination, SourceDSL);
	TestTrue(TEXT("real Mover DSL import succeeds"), ImportResult.bSuccess);

	UAnimGraphNode_Base* DestinationNode = FindControlRigNode(Destination);
	TestNotNull(TEXT("destination Control Rig node exists"), DestinationNode);
	if (!DestinationNode) return false;
	TestEqual(TEXT("Control Rig asset reference round-trips"),
		ExportControlRigReference(DestinationNode), SourceReference);
	TSet<FName> ExpectedVisibleProperties;
	for (const FAnimRigInputBinding& Input : RigBinding.Inputs)
	{
		const FRigVariableAST* Variable = RigExport.Module->Variables.FindByPredicate(
			[&Input](const FRigVariableAST& Candidate)
			{
				return Candidate.Access == ERigVariableAccess::PublicInput
					&& AnimLispStableRuntimeSymbol(Candidate.Name) == Input.RigInputName;
			});
		if (Variable) ExpectedVisibleProperties.Add(FName(*Variable->Name));
	}
	TestTrue(TEXT("typed import exposes exactly the bound authoritative CustomPinProperties"),
		GetVisibleCustomPinPropertyNames(DestinationNode).Includes(ExpectedVisibleProperties)
		&& ExpectedVisibleProperties.Includes(GetVisibleCustomPinPropertyNames(DestinationNode)));
	TSharedPtr<FAnimGraphAST> VisibilityAST = MakeShared<FAnimGraphAST>();
	VisibilityAST->Name = TEXT("ABP_ControlRigVisibilityCorrection");
	VisibilityAST->SkeletonPath = SourceAST->SkeletonPath;
	VisibilityAST->RigImports = SourceAST->RigImports;
	TSharedPtr<FAnimNodeAST> VisibilityRigNode = MakeShared<FAnimNodeAST>(*ControlRigAST);
	VisibilityRigNode->Children.Reset();
	VisibilityRigNode->Properties.Reset();
	if (VisibilityRigNode->RigBinding.IsSet())
	{
		TArray<FAnimRigInputBinding>& VisibilityInputs = VisibilityRigNode->RigBinding.GetValue().Inputs;
		if (VisibilityInputs.Num() > 1)
		{
			VisibilityInputs.RemoveAt(VisibilityInputs.Num() - 1);
		}
		for (FAnimRigInputBinding& Input : VisibilityInputs)
		{
			Input.ValueExpression = Input.ResolvedType.CPPType == TEXT("bool")
				? TEXT("(pin-default false)") : TEXT("(pin-default \"(X=0,Y=0,Z=1)\")");
		}
	}
	TSet<FName> VisibilityExpectedVisibleProperties;
	for (const FAnimRigInputBinding& Input : VisibilityRigNode->RigBinding.GetValue().Inputs)
	{
		if (const FRigVariableAST* Variable = RigExport.Module->Variables.FindByPredicate(
			[&Input](const FRigVariableAST& Candidate)
			{
				return Candidate.Access == ERigVariableAccess::PublicInput
					&& AnimLispStableRuntimeSymbol(Candidate.Name) == Input.RigInputName;
			}))
		{
			VisibilityExpectedVisibleProperties.Add(FName(*Variable->Name));
		}
	}
	VisibilityAST->RootNode = VisibilityRigNode;
	UAnimBlueprint* VisibilityDestination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), GetTransientPackage(), TEXT("ABP_ControlRigVisibilityCorrection"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPControlRigVisibilityTest"))));
	TestNotNull(TEXT("visibility-only destination is created"), VisibilityDestination);
	if (!VisibilityDestination) return false;
	VisibilityDestination->TargetSkeleton = Source->TargetSkeleton;
	const FString VisibilityDSL = VisibilityAST->ToString();
	TestTrue(TEXT("visibility-only initial typed import succeeds"),
		FAnimBPImporter::UpdateBlueprintDetailed(VisibilityDestination, VisibilityDSL).bSuccess);
	UAnimGraphNode_Base* VisibilityNode = FindControlRigNode(VisibilityDestination);
	TestNotNull(TEXT("visibility-only destination contains Control Rig"), VisibilityNode);
	if (!VisibilityNode) return false;
	TestTrue(TEXT("typed import exposes only its bound authoritative CustomPinProperties"),
		GetVisibleCustomPinPropertyNames(VisibilityNode).Includes(VisibilityExpectedVisibleProperties)
		&& VisibilityExpectedVisibleProperties.Includes(GetVisibleCustomPinPropertyNames(VisibilityNode)));
	TestTrue(TEXT("fixture has an unbound CustomPinProperty that can be made visible"),
		ShowOneExtraCustomPinProperty(VisibilityNode, VisibilityExpectedVisibleProperties));
	TestTrue(TEXT("extra CustomPinProperty is visible before the typed update"),
		GetVisibleCustomPinPropertyNames(VisibilityNode).Num() > VisibilityExpectedVisibleProperties.Num());
	const FAnimBPImporter::FUpdateResult VisibilityUpdate =
		FAnimBPImporter::UpdateBlueprintDetailed(VisibilityDestination, VisibilityDSL);
	TestTrue(TEXT("typed visibility correction update succeeds"), VisibilityUpdate.bSuccess);
	VisibilityNode = FindControlRigNode(VisibilityDestination);
	TestTrue(TEXT("typed update closes extras and exposes exactly the binding set"),
		VisibilityNode
		&& GetVisibleCustomPinPropertyNames(VisibilityNode).Includes(VisibilityExpectedVisibleProperties)
		&& VisibilityExpectedVisibleProperties.Includes(GetVisibleCustomPinPropertyNames(VisibilityNode)));

	const TArray<FString> LinkedInputs = {
		TEXT("GroundNormal"), TEXT("DebugDraw"), TEXT("EnableFootPinning"),
		TEXT("EnableSlopeWarping"), TEXT("HasTeleported"),
		TEXT("WorldZDamperEnabled"), TEXT("ForceReset") };
	for (const FString& InputName : LinkedInputs)
	{
		UEdGraphPin* SourcePin = FindInputPin(SourceNode, InputName);
		UEdGraphPin* DestinationPin = FindInputPin(DestinationNode, InputName);
		TestNotNull(TEXT("source Control Rig input exists: ") + InputName, SourcePin);
		TestNotNull(TEXT("destination Control Rig input exists: ") + InputName, DestinationPin);
		if (SourcePin) TestTrue(TEXT("source Control Rig input is linked: ") + InputName, SourcePin->LinkedTo.Num() > 0);
		if (DestinationPin) TestTrue(TEXT("destination Control Rig input is linked: ") + InputName, DestinationPin->LinkedTo.Num() > 0);
	}
	UEdGraphPin* SourceRaycast = FindInputPin(SourceNode, TEXT("DoRaycast"));
	UEdGraphPin* DestinationRaycast = FindInputPin(DestinationNode, TEXT("DoRaycast"));
	TestNotNull(TEXT("source DoRaycast input exists"), SourceRaycast);
	TestNotNull(TEXT("destination DoRaycast input exists"), DestinationRaycast);
	if (SourceRaycast && DestinationRaycast)
	{
		TestEqual(TEXT("DoRaycast default round-trips"), DestinationRaycast->DefaultValue, SourceRaycast->DefaultValue);
	}

	auto ExpectTypedImportFailure = [this, Source](const FString& Label, const TFunctionRef<void(FAnimRigNodeBinding&)>& Mutate)
	{
		const TSharedPtr<FAnimGraphAST> InvalidAST = FAnimBPExporter::ExportToAST(Source);
		if (!TestTrue(Label + TEXT(": source re-export succeeds"), InvalidAST.IsValid())) return;
		const TArray<TSharedPtr<FAnimNodeAST>> Nodes = CollectControlRigASTs(*InvalidAST);
		if (!TestTrue(Label + TEXT(": has one typed Control Rig node"),
			Nodes.Num() == 1 && Nodes[0]->RigBinding.IsSet())) return;
		Mutate(Nodes[0]->RigBinding.GetValue());
		const FName DestinationName(*(FString(TEXT("ABP_")) + Label.Replace(TEXT(" "), TEXT(""))));
		UAnimBlueprint* InvalidDestination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			Source->ParentClass ? Source->ParentClass.Get() : UAnimInstance::StaticClass(),
			GetTransientPackage(), DestinationName,
			BPTYPE_Normal, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("AnimBP2FPControlRigNegativeTest"))));
		if (!TestNotNull(Label + TEXT(": transient destination is created"), InvalidDestination)) return;
		InvalidDestination->TargetSkeleton = Source->TargetSkeleton;
		const FAnimBPImporter::FUpdateResult InvalidResult =
			FAnimBPImporter::UpdateBlueprintDetailed(InvalidDestination, InvalidAST->ToString());
		TestFalse(Label + TEXT(": typed validation failure propagates to UpdateResult"), InvalidResult.bSuccess);
	};
	AddExpectedError(TEXT("[UNSUPPORTED:ControlRigEntry] Entry 'MissingEntry'"),
		EAutomationExpectedErrorFlags::Contains, 1);
	ExpectTypedImportFailure(TEXT("MissingRigEntry"), [](FAnimRigNodeBinding& Binding)
	{
		Binding.EntryName = TEXT("MissingEntry");
	});
	AddExpectedError(TEXT("[UNSUPPORTED:ControlRigInput] Input 'MissingInput'"),
		EAutomationExpectedErrorFlags::Contains, 1);
	ExpectTypedImportFailure(TEXT("MissingRigInput"), [](FAnimRigNodeBinding& Binding)
	{
		if (Binding.Inputs.Num() != 0) Binding.Inputs[0].RigInputName = TEXT("MissingInput");
	});
	AddExpectedError(TEXT("[UNSUPPORTED:ControlRigInput] Input 'GroundNormal' has no unique exact public Rig variable/type"),
		EAutomationExpectedErrorFlags::Contains, 1);
	ExpectTypedImportFailure(TEXT("MismatchedRigInputType"), [](FAnimRigNodeBinding& Binding)
	{
		if (FAnimRigInputBinding* Input = Binding.Inputs.FindByPredicate(
			[](const FAnimRigInputBinding& Candidate) { return Candidate.RigInputName == TEXT("GroundNormal"); }))
		{
			Input->ResolvedType.CPPType = TEXT("bool");
			Input->ResolvedType.CPPTypeObject.Reset();
		}
	});
	AddExpectedError(TEXT("[SKIP:RigPinDefault] Node 'control-rig' bool property ':DoRaycast' has invalid default 'maybe'"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("[UNSUPPORTED:ControlRigInputValue] Failed to restore typed Rig input 'DoRaycast'"),
		EAutomationExpectedErrorFlags::Contains, 1);
	ExpectTypedImportFailure(TEXT("InvalidRigInputValue"), [](FAnimRigNodeBinding& Binding)
	{
		if (FAnimRigInputBinding* Input = Binding.Inputs.FindByPredicate(
			[](const FAnimRigInputBinding& Candidate) { return Candidate.RigInputName == TEXT("DoRaycast"); }))
		{
			Input->ValueExpression = TEXT("(pin-default maybe)");
		}
	});
	return true;
}

#endif

#endif
