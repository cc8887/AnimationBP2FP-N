// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"

#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimGraphNode_Base.h"
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS
#if ENGINE_MAJOR_VERSION >= 5
namespace
{
	const TMap<FName, FAnimGraphNodePropertyBinding>* GetPropertyBindings(const UAnimGraphNode_Base* Node)
	{
		if (!Node || !Node->GetBinding()) return nullptr;

		const UObject* BindingObject = reinterpret_cast<const UObject*>(Node->GetBinding());
		const FMapProperty* MapProperty = FindFProperty<FMapProperty>(BindingObject->GetClass(), TEXT("PropertyBindings"));
		return MapProperty
			? reinterpret_cast<const TMap<FName, FAnimGraphNodePropertyBinding>*>(
				MapProperty->ContainerPtrToValuePtr<void>(BindingObject))
			: nullptr;
	}

	TArray<FString> GatherOffsetRootBindingSignatures(const UAnimBlueprint* Blueprint)
	{
		TArray<FString> Signatures;
		if (!Blueprint) return Signatures;

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			for (const UEdGraphNode* GraphNode : Graph->Nodes)
			{
				const UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(GraphNode);
				if (!Node || Node->GetClass()->GetName() != TEXT("AnimGraphNode_OffsetRootBone")) continue;

				const TMap<FName, FAnimGraphNodePropertyBinding>* Bindings = GetPropertyBindings(Node);
				if (!Bindings) continue;
				for (const TPair<FName, FAnimGraphNodePropertyBinding>& Pair : *Bindings)
				{
					const FAnimGraphNodePropertyBinding& Binding = Pair.Value;
					if (!Binding.bIsBound) continue;
					Signatures.Add(FString::Printf(TEXT("%s|property=%s|path=%s|type=%d|pin=%s|array=%d|context=%s"),
						*Pair.Key.ToString(), *Binding.PropertyName.ToString(),
						*FString::Join(Binding.PropertyPath, TEXT(".")), static_cast<int32>(Binding.Type),
						*Binding.PinType.PinCategory.ToString(), Binding.ArrayIndex, *Binding.ContextId.ToString()));
				}
			}
		}
		Signatures.Sort();
		return Signatures;
	}

	bool AreOffsetRootBoundPinsVisible(const UAnimBlueprint* Blueprint, TArray<FString>& OutHiddenBindings)
	{
		OutHiddenBindings.Reset();
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			for (const UEdGraphNode* GraphNode : Graph->Nodes)
			{
				const UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(GraphNode);
				if (!Node || Node->GetClass()->GetName() != TEXT("AnimGraphNode_OffsetRootBone")) continue;
				const TMap<FName, FAnimGraphNodePropertyBinding>* Bindings = GetPropertyBindings(Node);
				if (!Bindings) continue;
				for (const TPair<FName, FAnimGraphNodePropertyBinding>& Pair : *Bindings)
				{
					if (!Pair.Value.bIsBound) continue;
					const FOptionalPinFromProperty* OptionalPin = Node->ShowPinForProperties.FindByPredicate(
						[&Pair](const FOptionalPinFromProperty& Candidate)
						{
							return Candidate.PropertyName == Pair.Value.PropertyName;
						});
					if (!OptionalPin || !OptionalPin->bShowPin)
					{
						OutHiddenBindings.Add(Pair.Value.PropertyName.ToString());
					}
				}
			}
		}
		return OutHiddenBindings.Num() == 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPOffsetRootBindingsRoundTrip,
	"AnimBP2FP.OffsetRoot.BindingsRoundTrip",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPOffsetRootBindingsRoundTrip::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(
		nullptr, TEXT("/Game/Blueprints/SandboxCharacter_CMC_ABP.SandboxCharacter_CMC_ABP"));
	TestNotNull(TEXT("real CMC AnimBlueprint loads"), Source);
	if (!Source) return false;

	const TArray<FString> SourceBindings = GatherOffsetRootBindingSignatures(Source);
	TestTrue(TEXT("source contains Offset Root bindings"), SourceBindings.Num() > 0);
	bool bHasFunctionBinding = false;
	for (const FString& Signature : SourceBindings)
	{
		AddInfo(TEXT("Source Offset Root binding: ") + Signature);
		bHasFunctionBinding |= Signature.Contains(TEXT("|type=2|"));
	}
	TestTrue(TEXT("source Offset Root uses function bindings"), bHasFunctionBinding);
	TArray<FString> SourceHiddenBindings;
	TestTrue(TEXT("source bound Offset Root pins are visible"),
		AreOffsetRootBoundPinsVisible(Source, SourceHiddenBindings));

	const TSharedPtr<FAnimGraphAST> SourceAST = FAnimBPExporter::ExportToAST(Source);
	TestTrue(TEXT("real CMC AnimBlueprint exports"), SourceAST.IsValid());
	if (!SourceAST.IsValid()) return false;
	const FString SourceDSL = SourceAST->ToString();
	TestTrue(TEXT("bind-path DSL preserves function binding type"),
		SourceDSL.Contains(TEXT("(bind-path \"Get_OffsetRootTranslationMode\" :type function")));

	UClass* ParentClass = Source->ParentClass ? Source->ParentClass.Get() : UAnimInstance::StaticClass();
	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		ParentClass, GetTransientPackage(), TEXT("ABP_OffsetRootBindingRoundTrip"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPOffsetRootBindingTest"))));
	TestNotNull(TEXT("transient destination AnimBlueprint is created"), Destination);
	if (!Destination) return false;
	Destination->TargetSkeleton = Source->TargetSkeleton;

	const FAnimBPImporter::FUpdateResult ImportResult =
		FAnimBPImporter::UpdateBlueprintDetailed(Destination, SourceDSL);
	TestTrue(TEXT("real CMC DSL import succeeds"), ImportResult.bSuccess);
	for (const TCHAR* FunctionName : {
		TEXT("Get_OffsetRootTranslationMode"), TEXT("Get_OffsetRootRotationMode"),
		TEXT("Get_OffsetRootTranslationHalfLife"), TEXT("Get_OffsetRootTranslationRadius"), TEXT("IsMoving") })
	{
		const UFunction* Function = Destination->SkeletonGeneratedClass
			? Destination->SkeletonGeneratedClass->FindFunctionByName(FName(FunctionName))
			: nullptr;
		TestNotNull(FString::Printf(TEXT("destination binding function '%s' exists"), FunctionName), Function);
		if (Function)
		{
			const FProperty* ReturnProperty = Function->GetReturnProperty();
			AddInfo(FString::Printf(TEXT("Destination binding function: %s flags=%llu return=%s:%s"),
				FunctionName, static_cast<uint64>(Function->FunctionFlags),
				ReturnProperty ? *ReturnProperty->GetName() : TEXT("<null>"),
				ReturnProperty ? *ReturnProperty->GetCPPType() : TEXT("<null>")));
		}
	}

	const TArray<FString> DestinationBindings = GatherOffsetRootBindingSignatures(Destination);
	for (const FString& Signature : DestinationBindings)
	{
		AddInfo(TEXT("Destination Offset Root binding: ") + Signature);
	}
	TestEqual(TEXT("Offset Root binding count is preserved"), DestinationBindings.Num(), SourceBindings.Num());
	TestTrue(TEXT("Offset Root binding signatures round-trip exactly"), DestinationBindings == SourceBindings);
	TArray<FString> DestinationHiddenBindings;
	const bool bDestinationPinsVisible = AreOffsetRootBoundPinsVisible(Destination, DestinationHiddenBindings);
	if (!bDestinationPinsVisible)
	{
		AddError(TEXT("Imported Offset Root bindings with hidden pins: ") + FString::Join(DestinationHiddenBindings, TEXT(", ")));
	}
	TestTrue(TEXT("destination bound Offset Root pins are visible"), bDestinationPinsVisible);
	return true;
}

#endif
#endif
#endif // UE 5.8+