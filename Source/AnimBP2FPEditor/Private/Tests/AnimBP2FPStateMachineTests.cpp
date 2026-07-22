// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "BlueprintLispConverter.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AnimBP2FPStateMachineTests
{
	struct FTopology
	{
		int32 States = 0;
		int32 Aliases = 0;
		int32 Conduits = 0;
		int32 Transitions = 0;
		int32 ConnectedStatePoses = 0;
		TArray<FString> NodeSignatures;
		TArray<FString> TransitionEndpoints;
		TArray<FString> TransitionObjectInputs;
	};

	static UAnimGraphNode_StateMachine* FindStateMachine(UAnimBlueprint* Blueprint, const FString& GraphName)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UAnimGraphNode_StateMachine* StateMachine = Cast<UAnimGraphNode_StateMachine>(Node);
				if (StateMachine && StateMachine->EditorStateMachineGraph
					&& StateMachine->EditorStateMachineGraph->GetName().Equals(GraphName, ESearchCase::CaseSensitive))
				{
					return StateMachine;
				}
			}
		}
		return nullptr;
	}

	static TSharedPtr<FAnimNodeAST> FindStateMachineAST(const TSharedPtr<FAnimNodeAST>& Node, const FString& Name)
	{
		if (!Node.IsValid()) return nullptr;
		FString NodeName = Node->GetStringProperty(TEXT("name"));
		if (NodeName.StartsWith(TEXT("\"")) && NodeName.EndsWith(TEXT("\"")))
		{
			NodeName = NodeName.Mid(1, NodeName.Len() - 2);
		}
		if (Node->NodeType == TEXT("state-machine") && NodeName == Name)
		{
			return Node;
		}
		for (const FNamedChild& Child : Node->Children)
		{
			if (TSharedPtr<FAnimNodeAST> Match = FindStateMachineAST(Child.Node, Name)) return Match;
		}
		return nullptr;
	}

	static FTopology ReadTopology(FAutomationTestBase& Test, UAnimGraphNode_StateMachine* StateMachine, const TCHAR* Label)
	{
		FTopology Result;
		if (!StateMachine || !StateMachine->EditorStateMachineGraph)
		{
			return Result;
		}
		UAnimBlueprint* OwnerBlueprint = StateMachine->GetTypedOuter<UAnimBlueprint>();

		for (UEdGraphNode* Node : StateMachine->EditorStateMachineGraph->Nodes)
		{
			if (UAnimStateNode* State = Cast<UAnimStateNode>(Node))
			{
				++Result.States;
				UEdGraphPin* PoseSink = State->GetPoseSinkPinInsideState();
				const int32 LinkCount = PoseSink ? PoseSink->LinkedTo.Num() : 0;
				Result.ConnectedStatePoses += LinkCount > 0 ? 1 : 0;
				Result.NodeSignatures.Add(FString::Printf(TEXT("state|%s|pose=%s"),
					*State->GetStateName(), LinkCount > 0 ? TEXT("true") : TEXT("false")));
				const FString SourceClass = LinkCount > 0 && PoseSink->LinkedTo[0]
					? PoseSink->LinkedTo[0]->GetOwningNode()->GetClass()->GetPathName()
					: TEXT("none");
				Test.AddInfo(FString::Printf(TEXT("%s state '%s': graph=%s pose-links=%d source=%s"),
					Label, *State->GetStateName(), State->GetBoundGraph() ? *State->GetBoundGraph()->GetClass()->GetPathName() : TEXT("none"),
					LinkCount, *SourceClass));
				if (UEdGraph* BoundGraph = State->GetBoundGraph())
				{
					for (UEdGraphNode* InnerNode : BoundGraph->Nodes)
					{
						FString PinSummary;
						for (UEdGraphPin* Pin : InnerNode->Pins)
						{
							if (!Pin)
							{
								continue;
							}
							PinSummary += FString::Printf(TEXT(" %s:%s=%d"),
								Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"), *Pin->PinName.ToString(), Pin->LinkedTo.Num());
						}
						Test.AddInfo(FString::Printf(TEXT("%s state '%s' inner %s '%s':%s"), Label, *State->GetStateName(),
							*InnerNode->GetClass()->GetPathName(), *InnerNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *PinSummary));
					}
				}
			}
			else if (UAnimStateAliasNode* Alias = Cast<UAnimStateAliasNode>(Node))
			{
				++Result.Aliases;
				TArray<FString> Targets;
				for (const TWeakObjectPtr<UAnimStateNodeBase>& Target : Alias->GetAliasedStates())
				{
					if (Target.IsValid())
					{
						Targets.Add(Target->GetStateName());
					}
				}
				Targets.Sort();
				Result.NodeSignatures.Add(FString::Printf(TEXT("alias|%s|global=%s|targets=%s"),
					*Alias->GetStateName(), Alias->bGlobalAlias ? TEXT("true") : TEXT("false"), *FString::Join(Targets, TEXT(","))));
				Test.AddInfo(FString::Printf(TEXT("%s alias '%s': global=%s targets=%d"), Label,
					*Alias->GetStateName(), Alias->bGlobalAlias ? TEXT("true") : TEXT("false"), Alias->GetAliasedStates().Num()));
			}
			else if (UAnimStateConduitNode* Conduit = Cast<UAnimStateConduitNode>(Node))
			{
				++Result.Conduits;
				Result.NodeSignatures.Add(FString::Printf(TEXT("conduit|%s|graph=%s"), *Conduit->GetStateName(),
					Conduit->GetBoundGraph() ? TEXT("true") : TEXT("false")));
				Test.AddInfo(FString::Printf(TEXT("%s conduit '%s': graph=%s"), Label, *Conduit->GetStateName(),
					Conduit->GetBoundGraph() ? *Conduit->GetBoundGraph()->GetClass()->GetPathName() : TEXT("none")));
			}
			else if (UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node))
			{
				++Result.Transitions;
				UAnimStateNodeBase* From = Transition->GetPreviousState();
				UAnimStateNodeBase* To = Transition->GetNextState();
				Result.TransitionEndpoints.Add(FString::Printf(TEXT("%s|%s"),
					From ? *From->GetStateName() : TEXT("none"), To ? *To->GetStateName() : TEXT("none")));
				if (UEdGraph* RuleGraph = Transition->GetBoundGraph())
				{
					for (UEdGraphNode* RuleNode : RuleGraph->Nodes)
					{
						UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(RuleNode);
						if (!Call)
						{
							continue;
						}
						const UFunction* Function = Call->GetTargetFunction();
						for (UEdGraphPin* Pin : Call->Pins)
						{
							if (!Pin || Pin->Direction != EGPD_Input
								|| (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Object
									&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Interface))
							{
								continue;
							}
							FString Source = TEXT("unlinked");
							if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
							{
								UEdGraphNode* SourceNode = Pin->LinkedTo[0]->GetOwningNode();
								Source = FString::Printf(TEXT("%s|%s"), *SourceNode->GetClass()->GetPathName(),
									*SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
							}
							else if (Pin->DefaultObject)
							{
								Source = TEXT("default-object|") + Pin->DefaultObject->GetPathName();
							}
							else if (!Pin->DefaultValue.IsEmpty())
							{
								Source = TEXT("default-value|") + Pin->DefaultValue;
							}
							FString FunctionIdentity = Function ? Function->GetPathName() : Call->FunctionReference.GetMemberName().ToString();
							if (Function && OwnerBlueprint)
							{
								UClass* FunctionOwner = Function->GetOuterUClass();
								if (FunctionOwner == OwnerBlueprint->GeneratedClass
									|| FunctionOwner == OwnerBlueprint->SkeletonGeneratedClass
									|| FunctionOwner->ClassGeneratedBy == OwnerBlueprint)
								{
									FunctionIdentity = TEXT("self:") + Function->GetName();
								}
							}
							else if (!Function && Call->FunctionReference.IsSelfContext())
							{
								FunctionIdentity = TEXT("self:") + Call->FunctionReference.GetMemberName().ToString();
							}
							const FString Signature = FString::Printf(TEXT("%s->%s|%s|%s|%s"),
								From ? *From->GetStateName() : TEXT("none"), To ? *To->GetStateName() : TEXT("none"),
								*FunctionIdentity,
								*Pin->PinName.ToString(), *Source);
							if (FunctionIdentity.StartsWith(TEXT("self:")))
							{
								Result.TransitionObjectInputs.Add(Signature);
								Test.AddInfo(FString::Printf(TEXT("%s transition self call: %s"), Label, *Signature));
							}
						}
					}
				}
			}
		}
		Result.NodeSignatures.Sort();
		Result.TransitionEndpoints.Sort();
		Result.TransitionObjectInputs.Sort();
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPStateMachineTopologyAndPosesRoundTrip,
	"AnimBP2FP.StateMachine.TopologyAndPosesRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPStateMachineTopologyAndPosesRoundTrip::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPStateMachineTests;

	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/Blueprints/SandboxCharacter_CMC_ABP.SandboxCharacter_CMC_ABP"));
	TestNotNull(TEXT("CMC source AnimBlueprint loads"), Source);
	if (!Source)
	{
		return false;
	}

	UAnimGraphNode_StateMachine* SourceMachine = FindStateMachine(Source, TEXT("State Controller"));
	TestNotNull(TEXT("source State Controller exists"), SourceMachine);
	if (!SourceMachine)
	{
		return false;
	}
	const FTopology SourceTopology = ReadTopology(*this, SourceMachine, TEXT("source"));
	TestTrue(TEXT("fixture contains aliases"), SourceTopology.Aliases > 0);
	TestTrue(TEXT("fixture contains a conduit"), SourceTopology.Conduits > 0);
	bool bFoundSelfOwner = false;
	for (UEdGraphNode* Node : SourceMachine->EditorStateMachineGraph->Nodes)
	{
		if (UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node))
		{
			FBlueprintLispConverter::FExportOptions LispOptions;
			LispOptions.bPrettyPrint = false;
			const FBlueprintLispResult ExportedRule = FBlueprintLispConverter::ExportGraph(Transition->GetBoundGraph(), LispOptions);
			bFoundSelfOwner |= ExportedRule.bSuccess && ExportedRule.LispCode.Contains(TEXT(":owner \"self\""));
		}
	}
	TestTrue(TEXT("transition DSL normalizes local Blueprint function owner to self"), bFoundSelfOwner);

	const TSharedPtr<FAnimGraphAST> SourceAST = FAnimBPExporter::ExportToAST(Source);
	TestTrue(TEXT("source exports"), SourceAST.IsValid());
	if (!SourceAST.IsValid())
	{
		return false;
	}

	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		Source->ParentClass, GetTransientPackage(), TEXT("ABP_StateMachineRoundTrip"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPStateMachineTest"))));
	TestNotNull(TEXT("destination AnimBlueprint is created"), Destination);
	if (!Destination)
	{
		return false;
	}
	Destination->TargetSkeleton = Source->TargetSkeleton;

	TSharedPtr<FAnimGraphAST> MinimalAST = MakeShared<FAnimGraphAST>();
	MinimalAST->Name = TEXT("ABP_StateMachineRoundTrip");
	MinimalAST->SkeletonPath = SourceAST->SkeletonPath;
	MinimalAST->Variables = SourceAST->Variables;
	MinimalAST->LogicGraphs = SourceAST->LogicGraphs;
	MinimalAST->bHasLogicGraphsBlock = SourceAST->bHasLogicGraphsBlock;
	MinimalAST->RootNode = FindStateMachineAST(SourceAST->RootNode, TEXT("State Controller"));
	TestTrue(TEXT("export contains State Controller AST"), MinimalAST->RootNode.IsValid());
	if (!MinimalAST->RootNode.IsValid()) return false;
	FAnimBPImporter::UpdateBlueprintDetailed(Destination, MinimalAST->ToString());
	UAnimGraphNode_StateMachine* DestinationMachine = FindStateMachine(Destination, TEXT("State Controller"));
	TestNotNull(TEXT("destination State Controller exists"), DestinationMachine);
	if (!DestinationMachine)
	{
		return false;
	}
	const FTopology DestinationTopology = ReadTopology(*this, DestinationMachine, TEXT("destination"));

	TestEqual(TEXT("state count round-trips"), DestinationTopology.States, SourceTopology.States);
	TestEqual(TEXT("alias count round-trips"), DestinationTopology.Aliases, SourceTopology.Aliases);
	TestEqual(TEXT("conduit count round-trips"), DestinationTopology.Conduits, SourceTopology.Conduits);
	TestEqual(TEXT("transition node count round-trips"), DestinationTopology.Transitions, SourceTopology.Transitions);
	TestEqual(TEXT("connected state pose count round-trips"), DestinationTopology.ConnectedStatePoses, SourceTopology.ConnectedStatePoses);
	TestTrue(TEXT("state-machine node signatures round-trip"), DestinationTopology.NodeSignatures == SourceTopology.NodeSignatures);
	TestTrue(TEXT("transition endpoints round-trip"), DestinationTopology.TransitionEndpoints == SourceTopology.TransitionEndpoints);
	TestTrue(TEXT("transition rule object inputs round-trip"), DestinationTopology.TransitionObjectInputs == SourceTopology.TransitionObjectInputs);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPStateMachineNestedControlRigFailurePropagates,
	"AnimBP2FP.StateMachine.NestedControlRigFailurePropagates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPStateMachineNestedControlRigFailurePropagates::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPStateMachineTests;
	UAnimBlueprint* StateSource = LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/Blueprints/SandboxCharacter_CMC_ABP.SandboxCharacter_CMC_ABP"));
	UAnimBlueprint* RigSource = LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/Blueprints/SandboxCharacter_Mover_ABP.SandboxCharacter_Mover_ABP"));
	TestNotNull(TEXT("state-machine fixture loads"), StateSource);
	TestNotNull(TEXT("typed Control Rig fixture loads"), RigSource);
	if (!StateSource || !RigSource) return false;

	const TSharedPtr<FAnimGraphAST> StateAST = FAnimBPExporter::ExportToAST(StateSource);
	const TSharedPtr<FAnimGraphAST> RigAST = FAnimBPExporter::ExportToAST(RigSource);
	TestTrue(TEXT("both fixtures export"), StateAST.IsValid() && RigAST.IsValid());
	if (!StateAST.IsValid() || !RigAST.IsValid()) return false;
	TSharedPtr<FAnimNodeAST> StateMachine = FindStateMachineAST(StateAST->RootNode, TEXT("State Controller"));
	TSharedPtr<FAnimNodeAST> InvalidRigNode;
	RigAST->VisitNodes([&InvalidRigNode](const TSharedPtr<FAnimNodeAST>& Node)
	{
		if (!InvalidRigNode.IsValid() && Node->NodeType == TEXT("control-rig") && Node->RigBinding.IsSet())
		{
			InvalidRigNode = Node;
		}
	});
	TestTrue(TEXT("fixtures provide a state machine and typed Control Rig"),
		StateMachine.IsValid() && InvalidRigNode.IsValid());
	if (!StateMachine.IsValid() || !InvalidRigNode.IsValid()) return false;
	InvalidRigNode->RigBinding.GetValue().EntryName = TEXT("MissingNestedEntry");
	const FString ChildId = TEXT("state-nested-rig");
	StateMachine->Children.Reset();
	FNamedChild& NestedStateChild = StateMachine->Children.AddDefaulted_GetRef();
	NestedStateChild.PinName = ChildId;
	NestedStateChild.Node = InvalidRigNode;
	StateMachine->Properties.Add(TEXT("name"), TEXT("\"Nested Rig State Machine\""));
	StateMachine->Properties.Add(TEXT("initial"), TEXT("\"Nested Rig State\""));
	StateMachine->Properties.Add(TEXT("state-nodes"), FString::Printf(
		TEXT("[(state :name \"Nested Rig State\" :child \"%s\")]"), *ChildId));
	StateMachine->Properties.Add(TEXT("transitions"), TEXT("[]"));
	TSharedPtr<FAnimGraphAST> MinimalAST = MakeShared<FAnimGraphAST>();
	MinimalAST->Name = TEXT("ABP_StateMachineNestedRigFailure");
	MinimalAST->SkeletonPath = StateAST->SkeletonPath;
	MinimalAST->Variables = StateAST->Variables;
	MinimalAST->RigImports = RigAST->RigImports;
	MinimalAST->RootNode = StateMachine;
	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		StateSource->ParentClass, GetTransientPackage(), TEXT("ABP_StateMachineNestedRigFailure"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPStateMachineNestedRigFailureTest"))));
	TestNotNull(TEXT("nested failure destination is created"), Destination);
	if (!Destination) return false;
	Destination->TargetSkeleton = StateSource->TargetSkeleton;
	AddExpectedErrorPlain(TEXT("[UNSUPPORTED:ControlRigEntry] Entry 'MissingNestedEntry'"),
		EAutomationExpectedErrorFlags::Contains, 1);
	const FAnimBPImporter::FUpdateResult Result =
		FAnimBPImporter::UpdateBlueprintDetailed(Destination, MinimalAST->ToString());
	for (const FString& Warning : Result.Warnings) AddInfo(TEXT("nested import: ") + Warning);
	TestFalse(TEXT("state-machine nested typed Control Rig failure reaches UpdateResult"), Result.bSuccess);
	return true;
}

#endif
