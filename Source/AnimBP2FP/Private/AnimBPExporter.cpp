// AnimBPExporter.cpp - Animation Blueprint to DSL Exporter
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBPExporter.h"

#if WITH_EDITOR

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

// AnimGraph node headers
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimGraphNode_BlendListBase.h"
#include "AnimGraphNode_BlendListByEnum.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_ApplyAdditive.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimationStateMachineGraph.h"
#include "Animation/AnimLayerInterface.h"
#include "BlueprintLispConverter.h"

// State machine node headers
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateConduitNode.h"
#include "AnimGraphNode_StateResult.h"

// Logging
DEFINE_LOG_CATEGORY_STATIC(LogAnimBP2FP, Log, All);

// ========== Helper Functions ==========

// Convert CamelCase, PascalCase, or "Spaced Words" to kebab-case
static FString CamelToKebab(const FString& Input)
{
	FString Result;
	for (int32 i = 0; i < Input.Len(); i++)
	{
		TCHAR Ch = Input[i];
		
		// Replace spaces and underscores with hyphens
		if (Ch == ' ' || Ch == '_')
		{
			// Avoid double hyphens
			if (Result.Len() > 0 && Result[Result.Len() - 1] != '-')
			{
				Result += TEXT("-");
			}
			continue;
		}
		
		if (FChar::IsUpper(Ch) && i > 0)
		{
			// Only add hyphen if previous char wasn't already a separator
			if (Result.Len() > 0 && Result[Result.Len() - 1] != '-')
			{
				Result += TEXT("-");
			}
		}
		Result += FChar::ToLower(Ch);
	}
	return Result;
}

// Follow a specific named input pose pin to its connected node
static UAnimGraphNode_Base* GetConnectedPoseNode(UAnimGraphNode_Base* Node, const FName& PinName)
{
	if (!Node) return nullptr;
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == EGPD_Input && Pin->PinName == PinName)
		{
			if (Pin->LinkedTo.Num() > 0)
			{
				return Cast<UAnimGraphNode_Base>(Pin->LinkedTo[0]->GetOwningNode());
			}
		}
	}
	return nullptr;
}

// Get the first connected input pose node (follows the first struct-type input)
static UAnimGraphNode_Base* GetFirstConnectedPoseNode(UAnimGraphNode_Base* Node)
{
	if (!Node) return nullptr;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == EGPD_Input &&
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
			Pin->LinkedTo.Num() > 0)
		{
			UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(Pin->LinkedTo[0]->GetOwningNode());
			if (Child)
			{
				return Child;
			}
		}
	}
	return nullptr;
}

// Get a pin's value as string - either default value, connected expression ref, or fallback
// NOTE: EventGraph-driven connections are exported as (var "NodeTitle") — cannot be auto-restored on import
static FString GetPinValueOrDefault(UAnimGraphNode_Base* Node, const FName& PinName, const FString& DefaultVal)
{
	if (!Node) return DefaultVal;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->PinName == PinName && Pin->Direction == EGPD_Input)
		{
			// If the pin has a linked node, return as (var ...) marking EventGraph-driven connection
			if (Pin->LinkedTo.Num() > 0)
			{
				UEdGraphNode* LinkedNode = Pin->LinkedTo[0]->GetOwningNode();
				if (LinkedNode)
				{
					return FString::Printf(TEXT("(var \"%s\")"), *LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
				}
			}
			// Otherwise return the default value
			if (!Pin->DefaultValue.IsEmpty())
			{
				return Pin->DefaultValue;
			}
		}
	}
	return DefaultVal;
}

// Collect ALL non-pose (non-Struct) input pin values as properties
// This extracts float, bool, int, enum, name, etc. parameters from the node
static void CollectNonPoseParams(UAnimGraphNode_Base* Node, TMap<FString, FString>& OutProperties)
{
	if (!Node) return;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction != EGPD_Input) continue;
		
		// Skip hidden or orphaned pins
		if (Pin->bHidden || Pin->bOrphanedPin) continue;
		
		// Struct pins connected to another node: export as (var "NodeTitle") — marks EventGraph-driven connection
		// NOTE: (var ...) is exported for traceability but cannot be automatically restored on import
		// because it requires K2Node_VariableGet/Function nodes in the EventGraph.
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			if (Pin->LinkedTo.Num() > 0)
			{
				UEdGraphNode* LinkedNode = Pin->LinkedTo[0]->GetOwningNode();
				if (LinkedNode)
				{
					FString ParamName = CamelToKebab(Pin->PinName.ToString());
					FString Value = FString::Printf(TEXT("(var \"%s\")"), 
						*LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
					OutProperties.Add(ParamName, Value);
				}
			}
			continue;
		}
		
		// Convert pin name to kebab-case for DSL
		FString ParamName = CamelToKebab(Pin->PinName.ToString());
		
		FString Value;
		
		if (Pin->LinkedTo.Num() > 0)
		{
			// Pin is connected to an EventGraph node — export as (var "NodeTitle")
			// NOTE: (var ...) marks EventGraph-driven connection; cannot be auto-restored on import
			UEdGraphNode* LinkedNode = Pin->LinkedTo[0]->GetOwningNode();
			if (LinkedNode)
			{
				Value = FString::Printf(TEXT("(var \"%s\")"), 
					*LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
		}
		else if (!Pin->DefaultValue.IsEmpty())
		{
			// Use the default value
			FString Category = Pin->PinType.PinCategory.ToString();
			
			if (Category == TEXT("bool"))
			{
				Value = Pin->DefaultValue.ToLower() == TEXT("true") ? TEXT("true") : TEXT("false");
			}
			else if (Category == TEXT("float") || Category == TEXT("real") || Category == TEXT("double"))
			{
				Value = Pin->DefaultValue;
			}
			else if (Category == TEXT("int") || Category == TEXT("int64"))
			{
				Value = Pin->DefaultValue;
			}
			else if (Category == TEXT("byte"))
			{
				// Enum type - output as string
				Value = FString::Printf(TEXT("\"%s\""), *Pin->DefaultValue);
			}
			else if (Category == TEXT("name") || Category == TEXT("string"))
			{
				Value = FString::Printf(TEXT("\"%s\""), *Pin->DefaultValue);
			}
			else
			{
				// Other types: output as string
				Value = FString::Printf(TEXT("\"%s\""), *Pin->DefaultValue);
			}
		}
		else if (Pin->DefaultObject != nullptr)
		{
			// Handle asset references (animation sequences, blend spaces, etc.)
			Value = FString::Printf(TEXT("(asset \"%s\")"), *Pin->DefaultObject->GetPathName());
		}
		else if (!Pin->AutogeneratedDefaultValue.IsEmpty())
		{
			// AutogeneratedDefaultValue with no explicit DefaultValue set
			// This is a pure engine default — skip it
			continue;
		}
		
		if (!Value.IsEmpty())
		{
			OutProperties.Add(ParamName, Value);
		}
	}
}

// Collect properties from the internal FAnimNode struct via reflection.
// This captures properties NOT exposed as pins (e.g. BoneToModify, TranslationMode, RotationSpace, etc.)
// Only exports non-default values, skipping pose links and properties already collected by CollectNonPoseParams.
static void CollectInternalProperties(UAnimGraphNode_Base* Node, TMap<FString, FString>& OutProperties)
{
	if (!Node) return;

	// Build a set of property names already collected from pins (normalized to lowercase-no-separators)
	// so we don't duplicate them
	auto NormalizeName = [](const FString& In) -> FString
	{
		FString Out = In.ToLower();
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	};

	TSet<FString> AlreadyCollected;
	for (const auto& Pair : OutProperties)
	{
		AlreadyCollected.Add(NormalizeName(Pair.Key));
	}

	// Also build a set from pin names so we skip anything that has a pin (even if not collected due to Struct type)
	TSet<FString> PinNames;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && !Pin->bHidden && !Pin->bOrphanedPin)
		{
			PinNames.Add(NormalizeName(Pin->PinName.ToString()));
		}
	}

	// Properties to always skip — internal to FAnimNode_Base or handled specially
	static const TSet<FString> SkipProperties = {
		TEXT("componentpose"), TEXT("baseposecomponentspace"),
		TEXT("alphainputtype"), TEXT("internalblendalpha"), TEXT("balphaisblendalpha"),
		TEXT("balphaisrelevant"), TEXT("actualalpha"),
		// SaveCachedPose internal
		TEXT("cachedposename"), TEXT("globalcachedposename"),
		// State machine internals  
		TEXT("statemachineinitialstatename"),
		// Functions / delegates
		TEXT("initializationfunction"), TEXT("startfunction"), TEXT("updatefunction"),
	};

	// Find the first FAnimNode_Base derived struct property
	for (TFieldIterator<FStructProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
	{
		FStructProperty* StructProp = *PropIt;
		if (!StructProp->Struct || !StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
		{
			continue;
		}

		void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(Node);

		// Get CDO for default value comparison
		UObject* CDO = Node->GetClass()->GetDefaultObject();
		void* CDOStructPtr = CDO ? StructProp->ContainerPtrToValuePtr<void>(CDO) : nullptr;

		for (TFieldIterator<FProperty> InnerIt(StructProp->Struct); InnerIt; ++InnerIt)
		{
			FProperty* InnerProp = *InnerIt;
			FString PropName = InnerProp->GetName();
			FString NormalizedPropName = NormalizeName(PropName);

			// Skip if in skip list
			if (SkipProperties.Contains(NormalizedPropName)) continue;

			// Skip if already collected from pins
			if (AlreadyCollected.Contains(NormalizedPropName)) continue;

			// Skip if there's a pin for this property (even Struct pins — those are pose links)
			if (PinNames.Contains(NormalizedPropName)) continue;

			// Skip pose link types
			if (FStructProperty* InnerStructProp = CastField<FStructProperty>(InnerProp))
			{
				UScriptStruct* InnerStruct = InnerStructProp->Struct;
				if (InnerStruct)
				{
					static UScriptStruct* PoseLinkStruct = FPoseLink::StaticStruct();
					static UScriptStruct* CSPoseLinkStruct = FComponentSpacePoseLink::StaticStruct();
					if (InnerStruct->IsChildOf(PoseLinkStruct) || InnerStruct->IsChildOf(CSPoseLinkStruct))
					{
						continue;
					}
				}
			}

			// Skip arrays of pose links
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(InnerProp))
			{
				if (FStructProperty* InnerStructProp = CastField<FStructProperty>(ArrayProp->Inner))
				{
					UScriptStruct* InnerStruct = InnerStructProp->Struct;
					if (InnerStruct)
					{
						static UScriptStruct* PoseLinkStruct = FPoseLink::StaticStruct();
						static UScriptStruct* CSPoseLinkStruct = FComponentSpacePoseLink::StaticStruct();
						if (InnerStruct->IsChildOf(PoseLinkStruct) || InnerStruct->IsChildOf(CSPoseLinkStruct))
						{
							continue;
						}
					}
				}
			}

			void* ValuePtr = InnerProp->ContainerPtrToValuePtr<void>(StructPtr);

			// Skip if value equals CDO default
			if (CDOStructPtr)
			{
				void* CDOValuePtr = InnerProp->ContainerPtrToValuePtr<void>(CDOStructPtr);
				if (InnerProp->Identical(ValuePtr, CDOValuePtr))
				{
					continue;
				}
			}

		// Export the value to string
		FString ExportedValue;
		InnerProp->ExportText_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);

		if (ExportedValue.IsEmpty()) continue;

			// Format the value for DSL output
			FString KebabName = CamelToKebab(PropName);
			FString FormattedValue;

			// Simple numeric/bool types → raw value
			if (InnerProp->IsA<FBoolProperty>())
			{
				FormattedValue = ExportedValue.ToLower() == TEXT("true") ? TEXT("true") : TEXT("false");
			}
			else if (InnerProp->IsA<FIntProperty>() || InnerProp->IsA<FInt64Property>())
			{
				FormattedValue = ExportedValue;
			}
			else if (InnerProp->IsA<FFloatProperty>() || InnerProp->IsA<FDoubleProperty>())
			{
				FormattedValue = ExportedValue;
			}
			else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(InnerProp))
			{
				// Enum → quoted string
				FormattedValue = FString::Printf(TEXT("\"%s\""), *ExportedValue);
			}
			else if (FByteProperty* ByteProp = CastField<FByteProperty>(InnerProp))
			{
				if (ByteProp->Enum)
				{
					FormattedValue = FString::Printf(TEXT("\"%s\""), *ExportedValue);
				}
				else
				{
					FormattedValue = ExportedValue;
				}
			}
			else if (InnerProp->IsA<FNameProperty>() || InnerProp->IsA<FStrProperty>())
			{
				FormattedValue = FString::Printf(TEXT("\"%s\""), *ExportedValue);
			}
			else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(InnerProp))
			{
				// Asset/Object reference → (asset "path")
				UObject* ObjValue = ObjProp->GetObjectPropertyValue(ValuePtr);
				if (ObjValue)
				{
					FormattedValue = FString::Printf(TEXT("(asset \"%s\")"), *ObjValue->GetPathName());
				}
				else
				{
					continue; // null object, skip
				}
			}
			else
			{
				// Structs, arrays, and other complex types → quoted ExportText string
				FormattedValue = FString::Printf(TEXT("\"%s\""), *ExportedValue);
			}

			if (!FormattedValue.IsEmpty())
			{
				OutProperties.Add(KebabName, FormattedValue);
			}
		}

		break; // Only process the first FAnimNode struct
	}
}

// Collect all pose (Struct) input pins as named children
// Returns array of (PinName, ConnectedNode) pairs
struct FPoseInput
{
	FString PinName;
	UAnimGraphNode_Base* Node;
};

static TArray<FPoseInput> CollectPoseInputs(UAnimGraphNode_Base* Node)
{
	TArray<FPoseInput> Result;
	if (!Node) return Result;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == EGPD_Input && 
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
			Pin->LinkedTo.Num() > 0)
		{
			UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(Pin->LinkedTo[0]->GetOwningNode());
			if (Child)
			{
				FPoseInput Input;
				Input.PinName = CamelToKebab(Pin->PinName.ToString());
				Input.Node = Child;
				Result.Add(Input);
			}
		}
	}
	return Result;
}

// ========== NodeId Utilities ==========

// Collect all NodeIds from an AST subtree into an array
static void CollectNodeIds(const TSharedPtr<FAnimNodeAST>& Node, TArray<FString>& OutIds)
{
	if (!Node.IsValid()) return;
	if (!Node->NodeId.IsEmpty())
		OutIds.Add(Node->NodeId);
	for (const FNamedChild& Child : Node->Children)
		CollectNodeIds(Child.Node, OutIds);
}

// FGuid segment cutpoints (cumulative lengths in the full guid string)
// Full format: "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" (36 chars)
// Segments at:  8       13   18   23   36
static const int32 GGuidPrefixLengths[] = { 8, 13, 18, 23, 36 };

// Compute shortest unique prefix for each full GUID among all GUIDs.
// Returns a map from full GUID string -> short prefix string.
static TMap<FString, FString> ComputeShortNodeIds(const TArray<FString>& AllFullIds)
{
	TMap<FString, FString> Result;
	if (AllFullIds.Num() == 0) return Result;

	for (const FString& FullId : AllFullIds)
	{
		// Try each prefix length until we find one that's unique
		for (int32 PrefixLen : GGuidPrefixLengths)
		{
			if (PrefixLen > FullId.Len()) break;
			FString Prefix = FullId.Left(PrefixLen);

			// Count how many IDs share this prefix
			int32 Collisions = 0;
			for (const FString& OtherId : AllFullIds)
			{
				if (OtherId.StartsWith(Prefix))
					Collisions++;
			}

			if (Collisions == 1)
			{
				Result.Add(FullId, Prefix);
				break;
			}
		}

		// Ensure every ID has an entry (should always be resolved by 36 chars)
		if (!Result.Contains(FullId))
		{
			Result.Add(FullId, FullId);
		}
	}

	return Result;
}

// Apply computed short IDs back to the AST (in-place)
static void ApplyShortNodeIds(const TSharedPtr<FAnimNodeAST>& Node, const TMap<FString, FString>& ShortIds)
{
	if (!Node.IsValid()) return;
	if (!Node->NodeId.IsEmpty())
	{
		const FString* Short = ShortIds.Find(Node->NodeId);
		if (Short) Node->NodeId = *Short;
	}
	for (const FNamedChild& Child : Node->Children)
		ApplyShortNodeIds(Child.Node, ShortIds);
}

// ========== Main Export Functions ==========

FString FAnimBPExporter::Export(UAnimBlueprint* AnimBlueprint)
{
	if (!AnimBlueprint)
	{
		return TEXT("; Error: Null AnimBlueprint");
	}

	TSharedPtr<FAnimGraphAST> AST = ExportToAST(AnimBlueprint);
	if (!AST.IsValid())
	{
		return TEXT("; Error: Failed to export AST");
	}

	return AST->ToString();
}

TSharedPtr<FAnimGraphAST> FAnimBPExporter::ExportToAST(UAnimBlueprint* AnimBlueprint)
{
	if (!AnimBlueprint)
	{
		UE_LOG(LogAnimBP2FP, Error, TEXT("[INTERNAL] ExportToAST: AnimBlueprint is null"));
		return nullptr;
	}

	TSharedPtr<FAnimGraphAST> ResultAST = MakeShared<FAnimGraphAST>();
	ResultAST->Name = AnimBlueprint->GetName();

	// Extract skeleton path
	if (AnimBlueprint->TargetSkeleton)
	{
		ResultAST->SkeletonPath = AnimBlueprint->TargetSkeleton->GetPathName();
	}

	// Extract variables from AnimBlueprint
	for (const FBPVariableDescription& Var : AnimBlueprint->NewVariables)
	{
		FVariableDef VarDef;
		VarDef.Name = Var.VarName.ToString();
		
		// Map UE pin category to our type
		FString CategoryStr = Var.VarType.PinCategory.ToString();
		if (CategoryStr == TEXT("float") || CategoryStr == TEXT("real") || CategoryStr == TEXT("double"))
			VarDef.Type = EPinType::Float;
		else if (CategoryStr == TEXT("int"))
			VarDef.Type = EPinType::Int;
		else if (CategoryStr == TEXT("bool"))
			VarDef.Type = EPinType::Bool;
		else
			VarDef.Type = EPinType::Float; // default
		
		VarDef.DefaultValue = Var.DefaultValue;
		ResultAST->Variables.Add(VarDef);
	}

	// Export implemented interfaces that are AnimLayerInterface subclasses
	// This enables import to call ImplementNewInterface, which is required for self-layer LinkedAnimLayer nodes
	for (const FBPInterfaceDescription& InterfaceDesc : AnimBlueprint->ImplementedInterfaces)
	{
		if (InterfaceDesc.Interface)
		{
			// Export all non-UObject interfaces (skip core engine interfaces that are not layer-related)
			const FString InterfacePath = InterfaceDesc.Interface->GetPathName();
			// Skip built-in engine paths
			if (!InterfacePath.StartsWith(TEXT("/Script/Engine")) && 
				!InterfacePath.StartsWith(TEXT("/Script/CoreUObject")))
			{
				ResultAST->ImplementedInterfaces.Add(InterfacePath);
				UE_LOG(LogAnimBP2FP, Log, TEXT("  implements: %s"), *InterfacePath);
			}
		}
	}

	// Find the AnimGraph
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
	{
		if (Graph->GetFName().ToString().Contains(TEXT("AnimGraph")))
		{
			AnimGraph = Graph;
			break;
		}
	}

	// Also check UbergraphPages
	if (!AnimGraph)
	{
		for (UEdGraph* Graph : AnimBlueprint->UbergraphPages)
		{
			if (Graph->GetFName().ToString().Contains(TEXT("AnimGraph")))
			{
				AnimGraph = Graph;
				break;
			}
		}
	}

	if (AnimGraph)
	{
		TraverseAnimGraph(AnimGraph, ResultAST);
	}
	else
	{
		UE_LOG(LogAnimBP2FP, Warning, TEXT("No AnimGraph found in blueprint: %s"), *AnimBlueprint->GetName());
	}

	// Compute shortest unique NodeId prefixes across the entire AST
	{
		TArray<FString> AllIds;
		CollectNodeIds(ResultAST->RootNode, AllIds);
		for (const FCachedPoseDef& Def : ResultAST->Defines)
			CollectNodeIds(Def.Body, AllIds);

		if (AllIds.Num() > 0)
		{
			TMap<FString, FString> ShortIds = ComputeShortNodeIds(AllIds);
			ApplyShortNodeIds(ResultAST->RootNode, ShortIds);
			for (FCachedPoseDef& Def : ResultAST->Defines)
				ApplyShortNodeIds(Def.Body, ShortIds);
		}
	}

	return ResultAST;
}

FString FAnimBPExporter::ExportWithOptions(UAnimBlueprint* AnimBlueprint, const FExportOptions& Options)
{
	if (!AnimBlueprint)
	{
		return TEXT("; Error: Null AnimBlueprint");
	}

	TSharedPtr<FAnimGraphAST> AST = ExportToAST(AnimBlueprint);
	if (!AST.IsValid())
	{
		return TEXT("; Error: Failed to export AST");
	}

	return ASTToString(AST, Options);
}

// ========== Graph Traversal ==========

void FAnimBPExporter::TraverseAnimGraph(UEdGraph* Graph, TSharedPtr<FAnimGraphAST> OutAST)
{
	if (!Graph || !OutAST.IsValid())
	{
		return;
	}

	// Collect SaveCachedPose nodes - they become top-level (define ...) bindings
	TMap<FString, UAnimGraphNode_SaveCachedPose*> CachedPoseNodes;

	// Step 1: Find the Root node and collect SaveCachedPose nodes
	UAnimGraphNode_Root* RootNode = nullptr;
	
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node))
		{
			RootNode = Root;
		}
		
		if (UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(Node))
		{
			CachedPoseNodes.Add(SaveNode->CacheName, SaveNode);
		}
	}

	// Step 2: Convert SaveCachedPose nodes to (define name body) bindings
	// Do this BEFORE converting the main tree so that UseCachedPose references resolve
	for (const auto& Pair : CachedPoseNodes)
	{
		FCachedPoseDef Def;
		Def.Name = Pair.Key;
		
		// Get the input pose subtree of the SaveCachedPose node
		UAnimGraphNode_Base* Child = GetFirstConnectedPoseNode(Pair.Value);
		if (Child)
		{
			Def.Body = ConvertAnimNode(Child);
		}
		
		OutAST->Defines.Add(Def);
		UE_LOG(LogAnimBP2FP, Log, TEXT("  define: %s"), *Def.Name);
	}

	// Step 3: From the Root node, convert the main animation tree
	if (RootNode)
	{
		UAnimGraphNode_Base* ActualRoot = GetFirstConnectedPoseNode(RootNode);
		if (ActualRoot)
		{
			OutAST->RootNode = ConvertAnimNode(ActualRoot);
		}
		else
		{
			UE_LOG(LogAnimBP2FP, Warning, TEXT("Root node has no connected input in graph: %s"), *Graph->GetName());
		}
	}
	else
	{
		UE_LOG(LogAnimBP2FP, Warning, TEXT("No AnimGraphNode_Root found in graph: %s. Trying fallback."), *Graph->GetName());
		
		// Fallback: find node whose class name contains "Result"
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
			if (AnimNode && AnimNode->GetClass()->GetName().Contains(TEXT("Result")))
			{
				UAnimGraphNode_Base* Connected = GetFirstConnectedPoseNode(AnimNode);
				if (Connected)
				{
					OutAST->RootNode = ConvertAnimNode(Connected);
					break;
				}
			}
		}
	}

	// Step 4: Topological sort of Defines to eliminate forward references
	// A define D1 depends on D2 if D1's body subtree references D2 (UseCachedPose)
	if (OutAST->Defines.Num() > 1)
	{
		// Build a name→index map
		TMap<FString, int32> NameToIdx;
		for (int32 i = 0; i < OutAST->Defines.Num(); i++)
		{
			NameToIdx.Add(OutAST->Defines[i].GetIdentifier(), i);
		}
		
		// Build adjacency: Edges[i] = set of indices that define[i] depends on
		TArray<TSet<int32>> Deps;
		Deps.SetNum(OutAST->Defines.Num());
		
		// Recursive lambda to find all UseCachedPose references in a subtree
		TFunction<void(const TSharedPtr<FAnimNodeAST>&, TSet<int32>&)> CollectDeps;
		CollectDeps = [&](const TSharedPtr<FAnimNodeAST>& Node, TSet<int32>& OutDeps)
		{
			if (!Node.IsValid()) return;
			
			// Check if this node is a variable reference (UseCachedPose)
			const int32* DepIdx = NameToIdx.Find(Node->NodeType);
			if (DepIdx)
			{
				OutDeps.Add(*DepIdx);
			}
			
			// Recurse into children
			for (const FNamedChild& Child : Node->Children)
			{
				CollectDeps(Child.Node, OutDeps);
			}
		};
		
		for (int32 i = 0; i < OutAST->Defines.Num(); i++)
		{
			CollectDeps(OutAST->Defines[i].Body, Deps[i]);
			Deps[i].Remove(i); // Remove self-references
		}
		
		// Kahn's algorithm for topological sort
		TArray<int32> InDegree;
		InDegree.SetNumZeroed(OutAST->Defines.Num());
		for (int32 i = 0; i < Deps.Num(); i++)
		{
			for (int32 Dep : Deps[i])
			{
				InDegree[Dep]++; // Dep is depended upon by i, but we want dep BEFORE i
			}
		}
		
		// Actually: if i depends on j, then j must come before i
		// Reverse the edge direction for topo sort: edges go from dependency to dependent
		TArray<TArray<int32>> RevAdj;
		RevAdj.SetNum(OutAST->Defines.Num());
		TArray<int32> InDeg;
		InDeg.SetNumZeroed(OutAST->Defines.Num());
		for (int32 i = 0; i < Deps.Num(); i++)
		{
			InDeg[i] = Deps[i].Num(); // i has this many dependencies
			for (int32 Dep : Deps[i])
			{
				RevAdj[Dep].Add(i); // Dep → i (Dep must come before i)
			}
		}
		
		TArray<int32> SortedOrder;
		TArray<int32> Queue;
		for (int32 i = 0; i < InDeg.Num(); i++)
		{
			if (InDeg[i] == 0) Queue.Add(i);
		}
		
		while (Queue.Num() > 0)
		{
			int32 Curr = Queue[0];
			Queue.RemoveAt(0);
			SortedOrder.Add(Curr);
			
			for (int32 Next : RevAdj[Curr])
			{
				InDeg[Next]--;
				if (InDeg[Next] == 0)
				{
					Queue.Add(Next);
				}
			}
		}
		
		// If we got a valid ordering, reorder the defines
		if (SortedOrder.Num() == OutAST->Defines.Num())
		{
			TArray<FCachedPoseDef> Sorted;
			for (int32 Idx : SortedOrder)
			{
				Sorted.Add(MoveTemp(OutAST->Defines[Idx]));
			}
			OutAST->Defines = MoveTemp(Sorted);
			UE_LOG(LogAnimBP2FP, Log, TEXT("  Defines topologically sorted (%d items)"), SortedOrder.Num());
		}
		else
		{
			UE_LOG(LogAnimBP2FP, Warning, TEXT("  Cyclic dependency detected in defines, keeping original order"));
		}
	}

	// Log statistics
	int32 TotalNodes = 0;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Cast<UAnimGraphNode_Base>(Node))
		{
			TotalNodes++;
		}
	}
	UE_LOG(LogAnimBP2FP, Log, TEXT("AnimGraph '%s': %d animation nodes, %d defines"), 
		*Graph->GetName(), TotalNodes, OutAST->Defines.Num());
}

// ========== Node Conversion ==========

TSharedPtr<FAnimNodeAST> FAnimBPExporter::ConvertAnimNode(UAnimGraphNode_Base* Node)
{
	if (!Node)
	{
		UE_LOG(LogAnimBP2FP, Error, TEXT("[INTERNAL] ConvertAnimNode called with null Node"));
		return nullptr;
	}

	TSharedPtr<FAnimNodeAST> Result = MakeShared<FAnimNodeAST>();
	FString ClassName = Node->GetClass()->GetName();

	// Store full NodeGuid as initial NodeId; it will be shortened after full AST is built
	Result->NodeId = Node->NodeGuid.ToString();

	// ---- Root node (should be handled by TraverseAnimGraph, but just in case) ----
	if (UAnimGraphNode_Root* RootNode = Cast<UAnimGraphNode_Root>(Node))
	{
		Result->NodeType = TEXT("root");
		UAnimGraphNode_Base* Child = GetFirstConnectedPoseNode(Node);
		if (Child)
		{
			return ConvertAnimNode(Child); // Skip root, return its child directly
		}
		return Result;
	}

	// ---- SequencePlayer ----
	if (UAnimGraphNode_SequencePlayer* SeqPlayer = Cast<UAnimGraphNode_SequencePlayer>(Node))
	{
		Result->NodeType = TEXT("sequence-player");
		
		if (SeqPlayer->Node.GetSequence())
		{
			Result->Properties.Add(TEXT("name"), FString::Printf(TEXT("\"%s\""), *SeqPlayer->Node.GetSequence()->GetName()));
		}
		else
		{
			Result->Properties.Add(TEXT("name"), TEXT("\"None\""));
		}
		
		Result->Properties.Add(TEXT("loop"), SeqPlayer->Node.IsLooping() ? TEXT("true") : TEXT("false"));
		
		// Collect all other non-pose parameters from pins
		CollectNonPoseParams(Node, Result->Properties);
		CollectInternalProperties(Node, Result->Properties);
		return Result;
	}

	// ---- BlendSpacePlayer ----
	if (UAnimGraphNode_BlendSpacePlayer* BSPlayer = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
	{
		Result->NodeType = TEXT("blendspace-player");
		
		if (BSPlayer->Node.GetBlendSpace())
		{
			Result->Properties.Add(TEXT("name"), FString::Printf(TEXT("\"%s\""), *BSPlayer->Node.GetBlendSpace()->GetName()));
		}
		else
		{
			Result->Properties.Add(TEXT("name"), TEXT("\"None\""));
		}
		
		Result->Properties.Add(TEXT("loop"), BSPlayer->Node.IsLooping() ? TEXT("true") : TEXT("false"));
		
		float PlayRate = BSPlayer->Node.GetPlayRate();
		if (!FMath::IsNearlyEqual(PlayRate, 1.0f))
		{
			Result->Properties.Add(TEXT("play-rate"), FString::SanitizeFloat(PlayRate));
		}
		
		// Collect pin-driven parameters (X, Y, PlayRate from pins, etc.)
		CollectNonPoseParams(Node, Result->Properties);
		CollectInternalProperties(Node, Result->Properties);
		return Result;
	}

	// ---- TwoWayBlend ----
	if (UAnimGraphNode_TwoWayBlend* BlendNode = Cast<UAnimGraphNode_TwoWayBlend>(Node))
	{
		Result->NodeType = TEXT("blend");
		
		// Collect all parameters from pins (Alpha, etc.)
		CollectNonPoseParams(Node, Result->Properties);
		
		// Named pose inputs: A and B
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				if (Pin->LinkedTo.Num() > 0)
				{
					UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(Pin->LinkedTo[0]->GetOwningNode());
					if (Child)
					{
						FString PinLabel = CamelToKebab(Pin->PinName.ToString());
						Result->AddChild(PinLabel, ConvertAnimNode(Child));
					}
				}
			}
		}
		return Result;
	}

	// ---- ApplyAdditive ----
	if (UAnimGraphNode_ApplyAdditive* AdditiveNode = Cast<UAnimGraphNode_ApplyAdditive>(Node))
	{
		Result->NodeType = TEXT("apply-additive");
		
		// Collect all non-pose parameters from pins (Alpha, LODThreshold, etc.)
		CollectNonPoseParams(Node, Result->Properties);
		CollectInternalProperties(Node, Result->Properties);
		
		// Named pose inputs: Base and Additive
		UAnimGraphNode_Base* BaseChild = GetConnectedPoseNode(Node, TEXT("Base"));
		if (BaseChild)
		{
			Result->AddChild(TEXT("base"), ConvertAnimNode(BaseChild));
		}
		
		UAnimGraphNode_Base* AdditiveChild = GetConnectedPoseNode(Node, TEXT("Additive"));
		if (AdditiveChild)
		{
			Result->AddChild(TEXT("additive"), ConvertAnimNode(AdditiveChild));
		}
		return Result;
	}

	// ---- LayeredBoneBlend ----
	if (UAnimGraphNode_LayeredBoneBlend* LBBNode = Cast<UAnimGraphNode_LayeredBoneBlend>(Node))
	{
		Result->NodeType = TEXT("layered-bone-blend");
		
		// Collect all non-pose parameters
		CollectNonPoseParams(Node, Result->Properties);
		CollectInternalProperties(Node, Result->Properties);
		
		// Named pose inputs: BasePose + BlendPose 0, BlendPose 1, ...
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && 
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
				Pin->LinkedTo.Num() > 0)
			{
				UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(Pin->LinkedTo[0]->GetOwningNode());
				if (Child)
				{
					FString PinLabel = CamelToKebab(Pin->PinName.ToString());
					Result->AddChild(PinLabel, ConvertAnimNode(Child));
				}
			}
		}
		return Result;
	}

	// ---- SaveCachedPose ----
	// NOTE: SaveCachedPose is handled by TraverseAnimGraph as top-level (define ...).
	// If we encounter it during tree traversal (shouldn't normally happen), just convert its body.
	if (UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(Node))
	{
		// Return the body subtree directly (the define wrapper is emitted at top level)
		UAnimGraphNode_Base* Child = GetFirstConnectedPoseNode(Node);
		if (Child)
		{
			return ConvertAnimNode(Child);
		}
		// No input connected — export as identity-pose but warn
		UE_LOG(LogAnimBP2FP, Warning, TEXT("[DEGRADATION:EmptySaveCachedPose] SaveCachedPose '%s' encountered during tree traversal with no input connected — exporting as identity-pose"),
			*SaveNode->CacheName);
		Result->NodeType = TEXT("identity-pose");
		return Result;
	}

	// ---- UseCachedPose → variable reference ----
	if (UAnimGraphNode_UseCachedPose* UseNode = Cast<UAnimGraphNode_UseCachedPose>(Node))
	{
		// Output as a bare variable reference — matches the (define name ...) binding
		FString CacheName;
		if (UseNode->SaveCachedPoseNode.IsValid())
		{
			CacheName = UseNode->SaveCachedPoseNode->CacheName;
		}
		else
		{
			CacheName = TEXT("Unknown");
		}
		
		// Convert to identifier: "Post Layering" → "Post-Layering"
		CacheName.ReplaceInline(TEXT(" "), TEXT("-"));
		
		Result->NodeType = CacheName;  // bare variable name, no parentheses needed
		// No properties, no children — this is a leaf reference
		return Result;
	}

	// ---- BlendList (Blend by bool/int/enum) ----
	if (UAnimGraphNode_BlendListBase* BlendList = Cast<UAnimGraphNode_BlendListBase>(Node))
	{
		Result->NodeType = TEXT("blend-list");
		Result->Properties.Add(TEXT("class"), FString::Printf(TEXT("\"%s\""), *ClassName));

		// BlendListByEnum needs BoundEnum to reconstruct correctly on import
		if (UAnimGraphNode_BlendListByEnum* BlendListByEnum = Cast<UAnimGraphNode_BlendListByEnum>(Node))
		{
			if (UEnum* BoundEnum = BlendListByEnum->GetEnum())
			{
				Result->Properties.Add(TEXT("bound-enum"), FString::Printf(TEXT("(asset \"%s\")"), *BoundEnum->GetPathName()));
			}
		}
		
		// Collect all non-pose parameters (ActiveChildIndex, etc.)
		CollectNonPoseParams(Node, Result->Properties);
		CollectInternalProperties(Node, Result->Properties);
		
		// Named pose inputs: each BlendPose pin with its name
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && 
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
				Pin->LinkedTo.Num() > 0)
			{
				UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(Pin->LinkedTo[0]->GetOwningNode());
				if (Child)
				{
					FString PinLabel = CamelToKebab(Pin->PinName.ToString());
					Result->AddChild(PinLabel, ConvertAnimNode(Child));
				}
			}
		}
		return Result;
	}

	// ---- State Machine — fully expand states and transitions ----
	if (UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node))
	{
		TSharedPtr<FStateMachineAST> SMAST = ConvertStateMachine(SMNode);
		if (SMAST.IsValid() && SMAST->States.Num() > 0)
		{
			// Emit as an inline state-machine node that contains full structure
			Result->NodeType = TEXT("state-machine");
			Result->Properties.Add(TEXT("name"), FString::Printf(TEXT("\"%s\""), *SMAST->Name));
			if (!SMAST->InitialState.IsEmpty())
			{
				Result->Properties.Add(TEXT("initial"), FString::Printf(TEXT("\"%s\""), *SMAST->InitialState));
			}
			
			// Each state becomes a named child with its animation subtree
			for (const FStateMachineAST::FState& State : SMAST->States)
			{
						if (State.Animation.IsValid())
						{
							FString StateId = CamelToKebab(State.Name);
							Result->AddChild(StateId, State.Animation);
						}
						else
						{
							// State with no animation — emit identity-pose placeholder but warn
							UE_LOG(LogAnimBP2FP, Warning, TEXT("[DEGRADATION:EmptyStatePose] State '%s' in StateMachine '%s' has no animation — exporting as identity-pose"),
								*State.Name, *SMAST->Name);
							TSharedPtr<FAnimNodeAST> Placeholder = MakeShared<FAnimNodeAST>();
							Placeholder->NodeType = TEXT("identity-pose");
							FString StateId = CamelToKebab(State.Name);
							Result->AddChild(StateId, Placeholder);
						}
			}
			
			// Transitions are stored in Properties as a serialized list
			// Format: :transitions "[(from -> to :duration 0.2 :priority 0 :auto true) ...]"
			if (SMAST->Transitions.Num() > 0)
			{
				FString TransStr = TEXT("[");
				for (int32 i = 0; i < SMAST->Transitions.Num(); i++)
				{
					const FStateMachineAST::FTransition& Trans = SMAST->Transitions[i];
					if (i > 0) TransStr += TEXT(" ");
					TransStr += FString::Printf(TEXT("(%s -> %s"), *Trans.FromState, *Trans.ToState);
					if (!FMath::IsNearlyEqual(Trans.BlendDuration, 0.2f))
					{
						TransStr += FString::Printf(TEXT(" :duration %s"), *FString::SanitizeFloat(Trans.BlendDuration));
					}
					if (Trans.Priority != 0)
					{
						TransStr += FString::Printf(TEXT(" :priority %d"), Trans.Priority);
					}
					if (Trans.bInterruptible)
					{
						TransStr += TEXT(" :bidirectional true");
					}
					if (Trans.Condition.IsValid())
					{
						TransStr += FString::Printf(TEXT(" :rule %s"), *Trans.Condition->ToString());
					}
					// Append :rule-graph (BlueprintLisp DSL of the full condition graph) if available
					if (!Trans.RuleGraph.IsEmpty())
					{
						FString EscapedGraph = Trans.RuleGraph;
						EscapedGraph.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
						EscapedGraph.ReplaceInline(TEXT("\""), TEXT("\\\""));
						EscapedGraph.ReplaceInline(TEXT("\n"), TEXT("\\n"));
						EscapedGraph.ReplaceInline(TEXT("\r"), TEXT("\\r"));
						TransStr += FString::Printf(TEXT(" :rule-graph \"%s\""), *EscapedGraph);
					}
					TransStr += TEXT(")");
				}
				TransStr += TEXT("]");
				Result->Properties.Add(TEXT("transitions"), TransStr);
			}
			
			return Result;
		}
		
		// Fallback if state machine graph is empty or unavailable
		UE_LOG(LogAnimBP2FP, Warning, TEXT("[DEGRADATION:EmptyStateMachine] StateMachine '%s' has no states — exporting as empty shell"),
			SMNode->EditorStateMachineGraph ? *SMNode->EditorStateMachineGraph->GetName() : TEXT("(unknown)"));
		Result->NodeType = TEXT("state-machine");
		if (SMNode->EditorStateMachineGraph)
		{
			Result->Properties.Add(TEXT("name"), FString::Printf(TEXT("\"%s\""), *SMNode->EditorStateMachineGraph->GetName()));
		}
		else
		{
			Result->Properties.Add(TEXT("name"), FString::Printf(TEXT("\"%s\""), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString()));
		}
		return Result;
	}

	// ---- LinkedAnimLayer: export layer name and interface ----
	if (UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(Node))
	{
		Result->NodeType = TEXT("linked-anim-layer");
		
		// Export the layer name (e.g. "BaseLayer", "OverlayLayer")
		// Note: GetLayerName() is MinimalAPI (not exported), so access Node.Layer directly
		FName LayerName = LayerNode->Node.Layer;
		if (LayerName != NAME_None)
		{
			Result->Properties.Add(TEXT("layer"), FString::Printf(TEXT("\"%s\""), *LayerName.ToString()));
		}
		
		// Export the interface class path if it's an interface layer (not self layer)
		if (LayerNode->Node.Interface)
		{
			FString InterfacePath = LayerNode->Node.Interface->GetPathName();
			Result->Properties.Add(TEXT("interface"), FString::Printf(TEXT("\"%s\""), *InterfacePath));
		}
		
		// Collect non-pose parameters from pins
		CollectNonPoseParams(Node, Result->Properties);
		CollectInternalProperties(Node, Result->Properties);
		
		// Collect pose inputs as named children
		TArray<FPoseInput> PoseInputs = CollectPoseInputs(Node);
		for (const FPoseInput& Input : PoseInputs)
		{
			TSharedPtr<FAnimNodeAST> ChildAST = ConvertAnimNode(Input.Node);
			if (ChildAST.IsValid())
			{
				Result->AddChild(Input.PinName, ChildAST);
			}
		}
		
		UE_LOG(LogAnimBP2FP, Log, TEXT("LinkedAnimLayer: layer='%s', interface=%s, %d params, %d pose inputs"),
			*LayerName.ToString(),
			LayerNode->Node.Interface ? *LayerNode->Node.Interface->GetName() : TEXT("(self)"),
			Result->Properties.Num(), PoseInputs.Num());
		
		return Result;
	}

	// ---- Generic fallback: auto-extract all pins ----
	{
		// Convert "AnimGraphNode_XYZ" to "xyz" in kebab-case
		FString TypeName = ClassName;
		TypeName.RemoveFromStart(TEXT("AnimGraphNode_"));
		Result->NodeType = CamelToKebab(TypeName);
		
		// Collect ALL non-pose parameters from pins
		CollectNonPoseParams(Node, Result->Properties);
		
		// Collect internal FAnimNode struct properties not exposed as pins
		CollectInternalProperties(Node, Result->Properties);
		
		// Collect ALL pose inputs as named children
		TArray<FPoseInput> PoseInputs = CollectPoseInputs(Node);
		for (const FPoseInput& Input : PoseInputs)
		{
			TSharedPtr<FAnimNodeAST> ChildAST = ConvertAnimNode(Input.Node);
			if (ChildAST.IsValid())
			{
				Result->AddChild(Input.PinName, ChildAST);
			}
		}
		
		UE_LOG(LogAnimBP2FP, Log, TEXT("Generic conversion for node type: %s -> %s (%d params, %d pose inputs)"), 
			*ClassName, *Result->NodeType, Result->Properties.Num(), PoseInputs.Num());
	}

	return Result;
}

// ========== State Machine Conversion — full expansion ==========

TSharedPtr<FStateMachineAST> FAnimBPExporter::ConvertStateMachine(UAnimGraphNode_StateMachine* SMNode)
{
	if (!SMNode)
	{
		UE_LOG(LogAnimBP2FP, Error, TEXT("[INTERNAL] ConvertStateMachine: SMNode is null"));
		return nullptr;
	}

	UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph;
	if (!SMGraph)
	{
		UE_LOG(LogAnimBP2FP, Error, TEXT("[DEGRADATION:NoStateMachineGraph] StateMachine node '%s' has no EditorStateMachineGraph — state machine will export as empty shell"),
			*SMNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
		return nullptr;
	}

	TSharedPtr<FStateMachineAST> Result = MakeShared<FStateMachineAST>();
	Result->Name = SMGraph->GetName();

	// Find the initial (default) state from the entry node
	if (SMGraph->EntryNode)
	{
		UEdGraphNode* DefaultState = SMGraph->EntryNode->GetOutputNode();
		if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(DefaultState))
		{
			Result->InitialState = StateNode->GetStateName();
		}
		else if (UAnimStateConduitNode* Conduit = Cast<UAnimStateConduitNode>(DefaultState))
		{
			Result->InitialState = Conduit->GetStateName();
		}
	}

	// Collect all states
	for (UEdGraphNode* GraphNode : SMGraph->Nodes)
	{
		if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(GraphNode))
		{
			FStateMachineAST::FState State;
			State.Name = StateNode->GetStateName();
			
			// Convert the state's internal animation graph
			UEdGraph* StateGraph = StateNode->GetBoundGraph();
			if (StateGraph)
			{
				// Find the StateResult node inside the state's graph
				UAnimGraphNode_StateResult* ResultNode = StateNode->GetResultNodeInsideState();
				if (ResultNode)
				{
					// Get the node connected to the StateResult's input
					UAnimGraphNode_Base* AnimRoot = GetFirstConnectedPoseNode(ResultNode);
					if (AnimRoot)
					{
						State.Animation = ConvertAnimNode(AnimRoot);
					}
				}
			}
			
			Result->States.Add(State);
			UE_LOG(LogAnimBP2FP, Log, TEXT("    State: %s (has animation: %s)"), 
				*State.Name, State.Animation.IsValid() ? TEXT("yes") : TEXT("no"));
		}
		else if (UAnimStateConduitNode* Conduit = Cast<UAnimStateConduitNode>(GraphNode))
		{
			// Conduit nodes are pass-through logic nodes, not real states
			// We skip them but their transitions are still collected
			UE_LOG(LogAnimBP2FP, Log, TEXT("    Conduit: %s (skipped)"), *Conduit->GetStateName());
		}
	}

	// Collect all transitions
	for (UEdGraphNode* GraphNode : SMGraph->Nodes)
	{
		if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(GraphNode))
		{
			// Skip disabled transitions
			if (TransNode->bDisabled) continue;
			
			UAnimStateNodeBase* FromState = TransNode->GetPreviousState();
			UAnimStateNodeBase* ToState = TransNode->GetNextState();
			
			if (FromState && ToState)
			{
				FStateMachineAST::FTransition Trans;
				Trans.FromState = FromState->GetStateName();
				Trans.ToState = ToState->GetStateName();
				Trans.BlendDuration = TransNode->CrossfadeDuration;
				Trans.Priority = TransNode->PriorityOrder;
				Trans.bInterruptible = TransNode->Bidirectional;
				
				// Try to extract the transition condition
				// The BoundGraph contains the condition logic (returns bool)
				if (TransNode->bAutomaticRuleBasedOnSequencePlayerInState)
				{
					// Auto-rule based on sequence player remaining time
					TSharedPtr<FLiteralExpr> AutoExpr = MakeShared<FLiteralExpr>();
					AutoExpr->Type = FLiteralExpr::EType::String;
					if (TransNode->AutomaticRuleTriggerTime < 0.0f)
					{
						AutoExpr->Value = TEXT("(auto-rule :time-remaining crossfade-duration)");
					}
					else
					{
						AutoExpr->Value = FString::Printf(TEXT("(auto-rule :time-remaining %s)"), 
							*FString::SanitizeFloat(TransNode->AutomaticRuleTriggerTime));
					}
					Trans.Condition = AutoExpr;
				}
				else if (TransNode->GetBoundGraph() == nullptr)
				{
					// BoundGraph is null — may not be loaded in commandlet mode
					UE_LOG(LogAnimBP2FP, Warning, TEXT("    [DEGRADATION:NoBoundGraph] Transition %s -> %s: GetBoundGraph() returned null — condition cannot be exported"),
						*Trans.FromState, *Trans.ToState);
				}
				else if (UEdGraph* CondGraph = TransNode->GetBoundGraph())
				{
					// Export the full transition condition graph as BlueprintLisp DSL
					// This is stored in :rule-graph for import-side restoration
					UAnimBlueprint* OwnerBP = Cast<UAnimBlueprint>(TransNode->GetGraph()->GetOuter()->GetOuter());
					if (!OwnerBP)
					{
						// Try going up further: TransitionNode -> SMGraph -> SM_Node -> AnimGraph -> AnimBP
						OwnerBP = TransNode->GetTypedOuter<UAnimBlueprint>();
					}
					
					bool bExportedRuleGraph = false;
					{
						// Export the transition condition graph directly using ExportGraph
						FBlueprintLispConverter::FExportOptions LispOpts;
						LispOpts.bPrettyPrint = false;
						LispOpts.bStableIds = true;
						FBlueprintLispResult LispResult = FBlueprintLispConverter::ExportGraph(CondGraph, LispOpts);
						if (LispResult.bSuccess && !LispResult.LispCode.IsEmpty())
						{
							Trans.RuleGraph = LispResult.LispCode;
							bExportedRuleGraph = true;
							UE_LOG(LogAnimBP2FP, Log, TEXT("    Transition %s -> %s: exported rule-graph (%d chars)"),
								*Trans.FromState, *Trans.ToState, LispResult.LispCode.Len());
						}
						else
						{
							UE_LOG(LogAnimBP2FP, Warning, TEXT("    [DEGRADATION:RuleGraphExport] Transition %s -> %s: BlueprintLisp export failed: %s"),
								*Trans.FromState, *Trans.ToState, *LispResult.Error);
						}
					}

					// Also extract a human-readable summary as :rule for diagnostics
					for (UEdGraphNode* CondNode : CondGraph->Nodes)
					{
						if (CondNode->GetClass()->GetName().Contains(TEXT("TransitionResult")))
						{
							for (UEdGraphPin* Pin : CondNode->Pins)
							{
								if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
								{
									UEdGraphNode* CondSource = Pin->LinkedTo[0]->GetOwningNode();
									if (CondSource)
									{
										TSharedPtr<FLiteralExpr> CondExpr = MakeShared<FLiteralExpr>();
										CondExpr->Type = FLiteralExpr::EType::String;
										// Export as (var "NodeTitle") — human-readable summary; full restore uses :rule-graph
										CondExpr->Value = FString::Printf(TEXT("(var \"%s\")"), 
											*CondSource->GetNodeTitle(ENodeTitleType::ListView).ToString());
										Trans.Condition = CondExpr;
									}
								}
							}
							break;
						}
					}
					
					if (!bExportedRuleGraph && !Trans.Condition.IsValid())
					{
						UE_LOG(LogAnimBP2FP, Error, TEXT("[SKIP:TransitionRule] Transition %s -> %s: no condition and no rule-graph exported — transition will never fire on import"),
							*Trans.FromState, *Trans.ToState);
					}
				}
				
				Result->Transitions.Add(Trans);
				UE_LOG(LogAnimBP2FP, Log, TEXT("    Transition: %s -> %s (duration: %.2f, priority: %d)"), 
					*Trans.FromState, *Trans.ToState, Trans.BlendDuration, Trans.Priority);
				
				// If bidirectional, also add the reverse transition
				if (TransNode->Bidirectional)
				{
					FStateMachineAST::FTransition ReverseTrans;
					ReverseTrans.FromState = Trans.ToState;
					ReverseTrans.ToState = Trans.FromState;
					ReverseTrans.BlendDuration = Trans.BlendDuration;
					ReverseTrans.Priority = Trans.Priority;
					ReverseTrans.bInterruptible = true;
					ReverseTrans.Condition = Trans.Condition;
					Result->Transitions.Add(ReverseTrans);
				}
			}
		}
	}

	UE_LOG(LogAnimBP2FP, Log, TEXT("  StateMachine '%s': %d states, %d transitions, initial: %s"), 
		*Result->Name, Result->States.Num(), Result->Transitions.Num(), *Result->InitialState);

	return Result;
}

// ========== Expression Conversion ==========

TSharedPtr<FExpressionAST> FAnimBPExporter::ConvertExpression(UEdGraphNode* ExprNode)
{
	if (!ExprNode)
	{
		return nullptr;
	}

	// Basic expression: just output the node title as a literal
	TSharedPtr<FLiteralExpr> Literal = MakeShared<FLiteralExpr>();
	Literal->Type = FLiteralExpr::EType::String;
	Literal->Value = ExprNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	return Literal;
}

// ========== AST to String ==========

FString FAnimBPExporter::ASTToString(const TSharedPtr<FAnimGraphAST>& AST, const FExportOptions& Options)
{
	if (!AST.IsValid())
	{
		return TEXT("; Error: Invalid AST");
	}

	if (Options.bPrettyPrint)
	{
		return AST->ToSExpression(true, Options.IndentSize);
	}
	else
	{
		return AST->ToString();
	}
}

// ============================================================================
// EventGraph export via BlueprintLisp plugin
// ============================================================================

bool FAnimBPExporter::ExportEventGraph(
	UAnimBlueprint*                 AnimBlueprint,
	const FEventGraphExportOptions& Options,
	FString&                        OutLispCode,
	FString&                        OutError)
{
	if (!AnimBlueprint)
	{
		OutError = TEXT("AnimBlueprint is null");
		return false;
	}

	// Delegate to BlueprintLisp plugin
	FBlueprintLispConverter::FExportOptions ExportOpts;
	ExportOpts.bPrettyPrint      = Options.bPrettyPrint;
	ExportOpts.bIncludePositions = Options.bIncludePositions;
	ExportOpts.bStableIds        = Options.bStableIds;

	FBlueprintLispResult Result = FBlueprintLispConverter::Export(
		AnimBlueprint,
		Options.GraphName,
		ExportOpts);

	if (Result.bSuccess)
	{
		OutLispCode = Result.LispCode;
		return true;
	}
	else
	{
		OutError = Result.Error;
		return false;
	}
}

#endif // WITH_EDITOR