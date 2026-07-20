// AnimBPExporter.cpp - Animation Blueprint to DSL Exporter
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBPExporter.h"

#if WITH_EDITOR

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Modules/ModuleManager.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"

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
#include "AnimationGraphSchema.h"

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

namespace
{
	struct FHelperExportContext
	{
		TMap<FString, FHelperGraphDef> HelperByGeneratedVar;
	};

	static const FHelperExportContext* GActiveHelperExportContext = nullptr;

	static FString GetDependencyRole(const UObject* Object)
	{
		if (!Object) return FString();
		if (Object->IsA<UAnimMontage>()) return TEXT("montage");
		if (Object->IsA<UAnimSequence>()) return TEXT("anim-sequence");
		if (Object->IsA<UBlendSpace>()) return TEXT("blend-space");
		const FString ClassPath = Object->GetClass()->GetPathName();
		if (ClassPath.Contains(TEXT("PoseSearchDatabase"))) return TEXT("pose-search-database");
		if (ClassPath.Contains(TEXT("PoseSearchSchema"))) return TEXT("pose-search-schema");
		if (ClassPath.Contains(TEXT("Chooser"))) return TEXT("chooser");
		if (ClassPath.Contains(TEXT("ControlRig"))) return TEXT("control-rig");
		if (ClassPath.Contains(TEXT("MirrorDataTable"))) return TEXT("mirror-data-table");
		if (ClassPath.Contains(TEXT("BlendProfile"))) return TEXT("blend-profile");
		return FString();
	}

	static bool IsDependencyCandidateClassPath(const FString& ClassPath)
	{
		return ClassPath.Contains(TEXT("AnimSequence"))
			|| ClassPath.Contains(TEXT("AnimMontage"))
			|| ClassPath.Contains(TEXT("BlendSpace"))
			|| ClassPath.Contains(TEXT("PoseSearchDatabase"))
			|| ClassPath.Contains(TEXT("PoseSearchSchema"))
			|| ClassPath.Contains(TEXT("Chooser"))
			|| ClassPath.Contains(TEXT("ControlRig"))
			|| ClassPath.Contains(TEXT("MirrorDataTable"))
			|| ClassPath.Contains(TEXT("BlendProfile"));
	}

	static FAnimationAssetMetadataSnapshot SnapshotAnimationAsset(const UAnimSequenceBase* Asset)
	{
		FAnimationAssetMetadataSnapshot Snapshot;
		if (!Asset) return Snapshot;
		Snapshot.bHasSnapshot = true;
		Snapshot.bHasRootMotion = Asset->HasRootMotion();
		for (const FAnimNotifyEvent& Event : Asset->Notifies)
		{
			FAnimNotifySnapshot& Notify = Snapshot.Notifies.AddDefaulted_GetRef();
			Notify.Name = Event.NotifyName.ToString();
			Notify.Time = Event.GetTriggerTime();
			Notify.Duration = Event.GetDuration();
			Notify.bIsState = Event.NotifyStateClass != nullptr;
			const UObject* NotifyObject = Event.NotifyStateClass ? static_cast<const UObject*>(Event.NotifyStateClass.Get()) : static_cast<const UObject*>(Event.Notify.Get());
			Notify.ClassPath = NotifyObject ? NotifyObject->GetClass()->GetPathName() : TEXT("name-only");
		}
		Snapshot.Notifies.Sort([](const FAnimNotifySnapshot& A, const FAnimNotifySnapshot& B)
		{
			if (A.Time != B.Time) return A.Time < B.Time;
			if (A.Name != B.Name) return A.Name < B.Name;
			return A.ClassPath < B.ClassPath;
		});

		if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Asset))
		{
			Snapshot.bEnableRootMotion = Sequence->bEnableRootMotion;
			Snapshot.bForceRootLock = Sequence->bForceRootLock;
			if (const UEnum* RootLockEnum = StaticEnum<ERootMotionRootLock::Type>())
			{
				Snapshot.RootMotionRootLock = RootLockEnum->GetNameStringByValue(Sequence->RootMotionRootLock.GetValue());
			}
			for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
			{
				Snapshot.SyncMarkers.Add({Marker.MarkerName.ToString(), Marker.Time});
			}
			Snapshot.SyncMarkers.Sort([](const FAnimSyncMarkerSnapshot& A, const FAnimSyncMarkerSnapshot& B)
			{
				return A.Time == B.Time ? A.Name < B.Name : A.Time < B.Time;
			});
		}
		else
		{
			Snapshot.UnsupportedFields.Add(TEXT("sequence-root-motion-flags"));
			Snapshot.UnsupportedFields.Add(TEXT("sync-markers"));
		}

		if (const UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
		{
			for (const FCompositeSection& Source : Montage->CompositeSections)
			{
				Snapshot.MontageSections.Add({Source.SectionName.ToString(), Source.GetTime(), Source.NextSectionName.ToString()});
			}
			for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
			{
				Snapshot.SlotTrackNames.AddUnique(Slot.SlotName.ToString());
			}
			Snapshot.SlotTrackNames.Sort();
		}
		return Snapshot;
	}

	static void AddExternalDependency(UObject* Object, TSharedPtr<FAnimGraphAST> AST, TSet<FString>& SeenPaths)
	{
		if (!Object || !AST.IsValid()) return;
		const FString Role = GetDependencyRole(Object);
		const FString ObjectPath = Object->GetPathName();
		if (Role.IsEmpty() || SeenPaths.Contains(ObjectPath)) return;
		SeenPaths.Add(ObjectPath);
		FAnimDependency& Dependency = AST->Dependencies.AddDefaulted_GetRef();
		Dependency.ObjectPath = ObjectPath;
		Dependency.ClassPath = Object->GetClass()->GetPathName();
		Dependency.Role = Role;
		Dependency.Mode = TEXT("external");
		if (const UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Object))
		{
			Dependency.AssetMetadata = SnapshotAnimationAsset(Animation);
		}
		if (const UAnimMontage* Montage = Cast<UAnimMontage>(Object))
		{
			AddExternalDependency(Montage->BlendProfileIn, AST, SeenPaths);
			AddExternalDependency(Montage->BlendProfileOut, AST, SeenPaths);
		}
	}

	static void CollectExternalDependencies(UAnimBlueprint* Blueprint, TSharedPtr<FAnimGraphAST> AST)
	{
		if (!Blueprint || !AST.IsValid() || Blueprint->GetOutermost() == GetTransientPackage()) return;
		FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FName> PackageNames;
		Module.Get().GetDependencies(Blueprint->GetOutermost()->GetFName(), PackageNames,
			UE::AssetRegistry::EDependencyCategory::Package);
		PackageNames.Sort(FNameLexicalLess());
		TSet<FString> SeenPaths;
		for (const FName PackageName : PackageNames)
		{
			TArray<FAssetData> Assets;
			Module.Get().GetAssetsByPackageName(PackageName, Assets, true);
			for (const FAssetData& AssetData : Assets)
			{
				if (IsDependencyCandidateClassPath(AssetData.AssetClassPath.ToString()))
				{
					AddExternalDependency(AssetData.GetAsset(), AST, SeenPaths);
				}
			}
		}
	}

	static FString QuoteDSLString(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	static FString NormalizeBindingToken(const FString& In)
	{
		FString Out = In.ToLower();
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	}

	static const TMap<FName, FAnimGraphNodePropertyBinding>* GetPropertyBindingMap(const UAnimGraphNode_Base* Node)
	{
		if (!Node || !Node->GetBinding())
		{
			return nullptr;
		}

		const UObject* BindingObject = reinterpret_cast<const UObject*>(Node->GetBinding());
		if (FMapProperty* MapProperty = FindFProperty<FMapProperty>(BindingObject->GetClass(), TEXT("PropertyBindings")))
		{
			const void* MapPtr = MapProperty->ContainerPtrToValuePtr<void>(BindingObject);
			return reinterpret_cast<const TMap<FName, FAnimGraphNodePropertyBinding>*>(MapPtr);
		}

		return nullptr;
	}

	static bool TryGetPropertyBinding(const UAnimGraphNode_Base* Node, const FName& BindingName, FAnimGraphNodePropertyBinding& OutBinding)
	{
		const TMap<FName, FAnimGraphNodePropertyBinding>* PropertyBindings = GetPropertyBindingMap(Node);
		if (!PropertyBindings)
		{
			return false;
		}

		if (const FAnimGraphNodePropertyBinding* Exact = PropertyBindings->Find(BindingName))
		{
			OutBinding = *Exact;
			return Exact->bIsBound && Exact->PropertyPath.Num() > 0;
		}

		const FName ComparisonName(BindingName, 0);
		for (const TPair<FName, FAnimGraphNodePropertyBinding>& Pair : *PropertyBindings)
		{
			if (FName(Pair.Key, 0) == ComparisonName)
			{
				OutBinding = Pair.Value;
				return Pair.Value.bIsBound && Pair.Value.PropertyPath.Num() > 0;
			}
		}

		return false;
	}

	static FString FormatBindPathValue(const TArray<FString>& PropertyPath)
	{
		if (PropertyPath.Num() == 0)
		{
			return FString();
		}

		const FString JoinedPath = FString::Join(PropertyPath, TEXT("."));
		return FString::Printf(TEXT("(bind-path %s)"), *QuoteDSLString(JoinedPath));
	}

	static bool IsManagedHelperGraphName(const FString& GraphName)
	{
		return GraphName.StartsWith(TEXT("__ABP2FP_HG_"));
	}

	static bool IsManagedGeneratedVarName(const FString& VariableName)
	{
		return VariableName.StartsWith(TEXT("__abp2fp_gv_"));
	}

	static FString DecodeManagedHelperId(const FString& Encoded)
	{
		FString Result = Encoded;
		Result.ReplaceInline(TEXT("_"), TEXT("-"));
		return Result;
	}

	static FLispNodePtr EXP_CloneBlueprintLispNodeWithoutStableIds(const FLispNodePtr& Node)
	{
		if (!Node.IsValid())
		{
			return Node;
		}

		FLispNodePtr Copy = MakeShared<FLispNode>();
		Copy->Type = Node->Type;
		Copy->StringValue = Node->StringValue;
		Copy->NumberValue = Node->NumberValue;
		Copy->Line = Node->Line;
		Copy->Column = Node->Column;

		if (Node->IsList())
		{
			for (int32 Index = 0; Index < Node->Children.Num(); ++Index)
			{
				const FLispNodePtr& Child = Node->Children[Index];
				if (Child.IsValid() && Child->IsKeyword()
					&& (Child->StringValue == TEXT(":id") || Child->StringValue == TEXT(":event-id")))
				{
					++Index;
					continue;
				}

				Copy->Children.Add(EXP_CloneBlueprintLispNodeWithoutStableIds(Child));
			}
		}

		return Copy;
	}

	static FString EXP_CanonicalizeHelperGraphDSLForExport(const FString& LispCode)
	{
		const FString Trimmed = LispCode.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return FString();
		}

		const FLispParseResult ParseResult = FLispParser::Parse(Trimmed);
		if (!ParseResult.bSuccess)
		{
			FString Fallback = Trimmed;
			Fallback.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
			return Fallback;
		}

		TArray<FString> CanonicalNodes;
		for (const FLispNodePtr& Node : ParseResult.Nodes)
		{
			const FLispNodePtr CanonNode = EXP_CloneBlueprintLispNodeWithoutStableIds(Node);
			if (CanonNode.IsValid())
			{
				CanonicalNodes.Add(CanonNode->ToString(false, 0));
			}
		}

		return FString::Join(CanonicalNodes, TEXT("\n"));
	}

	static EPinType PinTypeToAnimLangType(const FEdGraphPinType& PinType)
	{
		const FString Category = PinType.PinCategory.ToString();
		if (Category == TEXT("float") || Category == TEXT("real") || Category == TEXT("double"))
		{
			return EPinType::Float;
		}
		if (Category == TEXT("int") || Category == TEXT("int64"))
		{
			return EPinType::Int;
		}
		if (Category == TEXT("bool") || Category == TEXT("boolean"))
		{
			return EPinType::Bool;
		}
		if (Category == TEXT("name"))
		{
			return EPinType::Name;
		}
		if ((Category == TEXT("byte") || Category == UEdGraphSchema_K2::PC_Byte.ToString()) && Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
		{
			return EPinType::Enum;
		}
		if (Category == TEXT("object") || Category == TEXT("softobject"))
		{
			return EPinType::Object;
		}
		if (Category == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get())
		{
			return EPinType::Vector;
		}
		if (Category == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject == TBaseStructure<FRotator>::Get())
		{
			return EPinType::Rotator;
		}
		if (Category == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject == TBaseStructure<FTransform>::Get())
		{
			return EPinType::Transform;
		}
		if (Category == UEdGraphSchema_K2::PC_Struct.ToString())
		{
			return EPinType::Struct;
		}
		return EPinType::Unknown;
	}

	static bool TryGetVariableNameFromLinkedPin(UEdGraphPin* Pin, FString& OutVariableName)
	{
		if (!Pin || Pin->LinkedTo.Num() == 0)
		{
			return false;
		}

		UEdGraphNode* LinkedNode = Pin->LinkedTo[0]->GetOwningNode();
		if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(LinkedNode))
		{
			OutVariableName = VarGetNode->VariableReference.GetMemberName().ToString();
			return !OutVariableName.IsEmpty();
		}

		return false;
	}

	static bool IsPoseLinkPin(const UEdGraphPin* Pin)
	{
		if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
		{
			return false;
		}

		const UObject* TypeObject = Pin->PinType.PinSubCategoryObject.Get();
		return TypeObject == FPoseLink::StaticStruct() || TypeObject == FComponentSpacePoseLink::StaticStruct();
	}

	static FString GetLinkedNodeFallbackValue(UEdGraphPin* Pin)
	{
		if (!Pin || Pin->LinkedTo.Num() == 0)
		{
			return FString();
		}

		UEdGraphNode* LinkedNode = Pin->LinkedTo[0]->GetOwningNode();
		if (!LinkedNode)
		{
			return FString();
		}

		return FString::Printf(TEXT("(var %s)"), *QuoteDSLString(LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
	}

	static FString ExportLinkedPinValue(UEdGraphPin* Pin)
	{
		FString VariableName;
		if (TryGetVariableNameFromLinkedPin(Pin, VariableName))
		{
			if (GActiveHelperExportContext)
			{
				if (const FHelperGraphDef* Helper = GActiveHelperExportContext->HelperByGeneratedVar.Find(VariableName))
				{
					return FString::Printf(TEXT("(subgraph-ref %s)"), *QuoteDSLString(Helper->Id));
				}
			}
			return FString::Printf(TEXT("(bind-var %s)"), *QuoteDSLString(VariableName));
		}

		return GetLinkedNodeFallbackValue(Pin);
	}

	static FString ResolveHelperGeneratedVarName(const UEdGraph* Graph)
	{
		if (!Graph)
		{
			return FString();
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(Node))
			{
				const FString VariableName = VarSetNode->VariableReference.GetMemberName().ToString();
				if (IsManagedGeneratedVarName(VariableName))
				{
					return VariableName;
				}
			}
		}

		return FString();
	}

	static void CollectHelperGraphs(UAnimBlueprint* AnimBlueprint, const TSharedPtr<FAnimGraphAST>& ResultAST, FHelperExportContext& OutContext)
	{
		if (!AnimBlueprint || !ResultAST.IsValid())
		{
			return;
		}

		for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			const FString GraphName = Graph->GetName();
			const FString GeneratedVarName = ResolveHelperGeneratedVarName(Graph);
			if (GeneratedVarName.IsEmpty())
			{
				continue;
			}
			if (!IsManagedHelperGraphName(GraphName) && !IsManagedGeneratedVarName(GeneratedVarName))
			{
				continue;
			}

			FBlueprintLispConverter::FExportOptions LispOptions;
			LispOptions.bPrettyPrint = false;
			LispOptions.bIncludeComments = false;
			LispOptions.bIncludePositions = false;
			LispOptions.bStableIds = true;
			FBlueprintLispResult LispResult = FBlueprintLispConverter::ExportGraph(Graph, LispOptions);
			if (!LispResult.bSuccess || LispResult.LispCode.IsEmpty())
			{
				UE_LOG(LogAnimBP2FP, Warning, TEXT("[DEGRADATION:HelperGraphExport] Failed to export helper graph '%s': %s"),
					*GraphName, *LispResult.Error);
				continue;
			}

			FHelperGraphDef Helper;
			const FString HelperSuffix = GeneratedVarName.Mid(FCString::Strlen(TEXT("__abp2fp_gv_")));
			Helper.Id = DecodeManagedHelperId(HelperSuffix);
			Helper.GraphName = GraphName;
			Helper.GeneratedVar = GeneratedVarName;
			Helper.DSL = EXP_CanonicalizeHelperGraphDSLForExport(LispResult.LispCode);
			Helper.UpdateGroup = TEXT("__ABP2FP_UpdateBindings");
			Helper.GeneratedType = EPinType::Float;

			for (const FBPVariableDescription& Var : AnimBlueprint->NewVariables)
			{
				if (Var.VarName == FName(*GeneratedVarName))
				{
					Helper.GeneratedType = PinTypeToAnimLangType(Var.VarType);
					break;
				}
			}

			ResultAST->HelperGraphs.Add(Helper);
			OutContext.HelperByGeneratedVar.Add(GeneratedVarName, Helper);
		}
	}
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
			FAnimGraphNodePropertyBinding PropertyBinding;
			if (TryGetPropertyBinding(Node, Pin->GetFName(), PropertyBinding))
			{
				const FString BindingValue = FormatBindPathValue(PropertyBinding.PropertyPath);
				if (!BindingValue.IsEmpty())
				{
					return BindingValue;
				}
			}

			// If the pin has a linked node, prefer structured binding forms before falling back to (var ...)
			if (Pin->LinkedTo.Num() > 0)
			{
				const FString LinkedValue = ExportLinkedPinValue(Pin);
				if (!LinkedValue.IsEmpty())
				{
					return LinkedValue;
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
		
		FString ParamName = CamelToKebab(Pin->PinName.ToString());
		FAnimGraphNodePropertyBinding PropertyBinding;
		if (TryGetPropertyBinding(Node, Pin->GetFName(), PropertyBinding))
		{
			FString BindingValue = FormatBindPathValue(PropertyBinding.PropertyPath);
			if (!BindingValue.IsEmpty())
			{
				OutProperties.Add(ParamName, BindingValue);
			}
			continue;
		}

		// Pose-link pins are restored via child pose connections, not scalar property forms.
		if (IsPoseLinkPin(Pin))
		{
			continue;
		}

		// Non-pose struct pins connected to another node use a structured binding.
		// Unconnected struct pins continue through the default-value path below.
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			if (Pin->LinkedTo.Num() > 0)
			{
				FString Value = ExportLinkedPinValue(Pin);
				if (!Value.IsEmpty())
				{
					OutProperties.Add(ParamName, Value);
				}
			}
			if (Pin->LinkedTo.Num() > 0)
			{
				continue;
			}
		}
		
		FString Value;
		
		if (Pin->LinkedTo.Num() > 0)
		{
			// Prefer structured binding export for VariableGet / generated-var bridges before falling back
			Value = ExportLinkedPinValue(Pin);
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

			const FString KebabName = CamelToKebab(PropName);
			FAnimGraphNodePropertyBinding PropertyBinding;
			if (TryGetPropertyBinding(Node, FName(*PropName), PropertyBinding))
			{
				FString BindingValue = FormatBindPathValue(PropertyBinding.PropertyPath);
				if (!BindingValue.IsEmpty())
				{
					OutProperties.Add(KebabName, BindingValue);
				}
				continue;
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
	if (AnimBlueprint->GeneratedClass)
	{
		if (UObject* ClassDefaults = AnimBlueprint->GeneratedClass->GetDefaultObject(false))
		{
			if (const FProperty* RootMotionMode = FindFProperty<FProperty>(ClassDefaults->GetClass(), TEXT("RootMotionMode")))
			{
				const void* ValuePtr = RootMotionMode->ContainerPtrToValuePtr<void>(ClassDefaults);
				RootMotionMode->ExportText_Direct(ResultAST->Metadata.RootMotionMode, ValuePtr, nullptr, ClassDefaults, PPF_None);
			}
		}
	}

	// Extract variables from AnimBlueprint
	for (const FBPVariableDescription& Var : AnimBlueprint->NewVariables)
	{
		if (Var.VarType.ContainerType == EPinContainerType::Map)
		{
			UE_LOG(LogAnimBP2FP, Error, TEXT("[UNSUPPORTED:VariableType] Variable '%s' is a map; map value terminal types are not represented by this DSL version"),
				*Var.VarName.ToString());
			return nullptr;
		}
		FVariableDef VarDef;
		VarDef.Name = Var.VarName.ToString();
		VarDef.Type = PinTypeToAnimLangType(Var.VarType);
		VarDef.PinCategory = Var.VarType.PinCategory.ToString();
		VarDef.PinSubCategory = Var.VarType.PinSubCategory.ToString();
		switch (Var.VarType.ContainerType)
		{
		case EPinContainerType::Array: VarDef.ContainerType = TEXT("array"); break;
		case EPinContainerType::Set: VarDef.ContainerType = TEXT("set"); break;
		case EPinContainerType::Map: VarDef.ContainerType = TEXT("map"); break;
		default: VarDef.ContainerType.Reset(); break;
		}
		VarDef.bIsReference = Var.VarType.bIsReference;
		VarDef.bIsConst = Var.VarType.bIsConst;
		VarDef.bIsWeakPointer = Var.VarType.bIsWeakPointer;
		VarDef.bIsUObjectWrapper = Var.VarType.bIsUObjectWrapper;

		if (const UObject* TypeObject = Var.VarType.PinSubCategoryObject.Get())
		{
			VarDef.TypeObjectPath = TypeObject->GetPathName();
		}
		
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

	FHelperExportContext HelperExportContext;
	CollectHelperGraphs(AnimBlueprint, ResultAST, HelperExportContext);

	auto ExportLogicGraph = [AnimBlueprint, &ResultAST](UEdGraph* Graph, const TCHAR* Role, const TCHAR* Kind) -> bool
	{
		if (!Graph || Graph->Nodes.IsEmpty())
		{
			return true;
		}

		FBlueprintLispConverter::FExportOptions LispOptions;
		LispOptions.bPrettyPrint = false;
		LispOptions.bStableIds = true;
		const FBlueprintLispResult LispResult = FBlueprintLispConverter::ExportGraph(Graph, LispOptions);
		const FString LispCode = LispResult.LispCode.TrimStartAndEnd();
		if (!LispResult.bSuccess || LispCode.IsEmpty() || LispCode.StartsWith(TEXT("; skip:"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogAnimBP2FP, Error, TEXT("[UNSUPPORTED:LogicGraph] Graph '%s' contains %d nodes but BlueprintLisp did not export it: %s"),
				*Graph->GetName(), Graph->Nodes.Num(), LispResult.Error.IsEmpty() ? *LispCode : *LispResult.Error);
			return false;
		}

		FLogicGraphDef& LogicGraph = ResultAST->LogicGraphs.AddDefaulted_GetRef();
		LogicGraph.Role = Role;
		LogicGraph.Kind = Kind;
		LogicGraph.GraphName = Graph->GetName();
		LogicGraph.DSL = LispCode;
		if (const UEdGraphSchema* Schema = Graph->GetSchema())
		{
			LogicGraph.SchemaClassPath = Schema->GetClass()->GetPathName();
		}
		return true;
	};

	ResultAST->bHasLogicGraphsBlock = true;
	for (UEdGraph* Graph : AnimBlueprint->UbergraphPages)
	{
		if (!ExportLogicGraph(Graph, TEXT("event"), TEXT("ubergraph")))
		{
			return nullptr;
		}
	}
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}
		const FString GraphName = Graph->GetName();
		if ((Graph->GetSchema() && Graph->GetSchema()->IsA<UAnimationGraphSchema>())
			|| GraphName.Contains(TEXT("AnimGraph")) || GraphName.StartsWith(TEXT("__ABP2FP_HG_")))
		{
			continue;
		}
		if (!ExportLogicGraph(Graph, TEXT("function"), TEXT("function")))
		{
			return nullptr;
		}
	}

	const FHelperExportContext* PreviousHelperContext = GActiveHelperExportContext;
	GActiveHelperExportContext = &HelperExportContext;

	// Find the AnimGraph
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
	{
		if (!Graph || !(Graph->GetSchema() && Graph->GetSchema()->IsA<UAnimationGraphSchema>()))
		{
			continue;
		}
		if (Graph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return IsValid(Node) && Node->IsA<UAnimGraphNode_Root>(); }))
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
			if (Graph && Graph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return IsValid(Node) && Node->IsA<UAnimGraphNode_Root>(); }))
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

	GActiveHelperExportContext = PreviousHelperContext;
	CollectExternalDependencies(AnimBlueprint, ResultAST);
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

	// Preserve the complete GUID so import can restore the real editor-node identity.
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

	// ---- UseCachedPose -> explicit cached-pose reference ----
	if (UAnimGraphNode_UseCachedPose* UseNode = Cast<UAnimGraphNode_UseCachedPose>(Node))
	{
		FString CacheName;
		if (UseNode->SaveCachedPoseNode.IsValid())
		{
			CacheName = UseNode->SaveCachedPoseNode->CacheName;
		}
		else
		{
			CacheName = TEXT("Unknown");
		}
		
		Result->NodeType = TEXT("cached-pose-ref");
		Result->Properties.Add(TEXT("name"), FString::Printf(TEXT("\"%s\""), *CacheName));
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
	for (const UClass* TestClass = Node->GetClass(); TestClass; TestClass = TestClass->GetSuperClass())
	{
		if (TestClass->GetName() != TEXT("AnimGraphNode_BlendStack_Base"))
		{
			continue;
		}

		FString TypeName = ClassName;
		TypeName.RemoveFromStart(TEXT("AnimGraphNode_"));
		Result->NodeType = CamelToKebab(TypeName);
		Result->NodeClassPath = Node->GetClass()->GetPathName();
		Result->Coverage = EAnimNodeCoverage::Lossy;
		CollectNonPoseParams(Node, Result->Properties);
		CollectInternalProperties(Node, Result->Properties);

		bool bExportedBoundGraph = false;
		TArray<FString> UnsupportedGraphClasses;
		TArray<FString> UnsupportedGraphSchemas;
		TArray<FString> UnsupportedGraphNodeClasses;
		for (UEdGraph* BoundGraph : Node->GetSubGraphs())
		{
			if (!BoundGraph)
			{
				continue;
			}
			FBlueprintLispConverter::FExportOptions LispOptions;
			LispOptions.bPrettyPrint = false;
			LispOptions.bStableIds = true;
			const FBlueprintLispResult LispResult = FBlueprintLispConverter::ExportGraph(BoundGraph, LispOptions);
			const FString LispCode = LispResult.LispCode.TrimStartAndEnd();
			if (LispResult.bSuccess && !LispCode.IsEmpty()
				&& !LispCode.StartsWith(TEXT("; skip:"), ESearchCase::IgnoreCase))
			{
				Result->Properties.Add(TEXT("bound-graph"), QuoteDSLString(LispCode));
				bExportedBoundGraph = true;
				break;
			}
			UE_LOG(LogAnimBP2FP, Warning, TEXT("[UNSUPPORTED:BlendStackBoundGraph] Node '%s' bound graph '%s' did not export: %s"),
				*Node->GetName(), *BoundGraph->GetName(), LispResult.Error.IsEmpty() ? *LispCode : *LispResult.Error);
			UnsupportedGraphClasses.AddUnique(BoundGraph->GetClass()->GetPathName());
			if (const UEdGraphSchema* Schema = BoundGraph->GetSchema())
			{
				UnsupportedGraphSchemas.AddUnique(Schema->GetClass()->GetPathName());
			}
			for (const UEdGraphNode* BoundNode : BoundGraph->Nodes)
			{
				if (BoundNode)
				{
					UnsupportedGraphNodeClasses.AddUnique(BoundNode->GetClass()->GetPathName());
				}
			}
		}
		if (!bExportedBoundGraph)
		{
			Result->Coverage = EAnimNodeCoverage::Unsupported;
			UnsupportedGraphClasses.Sort();
			UnsupportedGraphSchemas.Sort();
			UnsupportedGraphNodeClasses.Sort();
			Result->Properties.Add(TEXT("unsupported-bound-graph-class"), QuoteDSLString(FString::Join(UnsupportedGraphClasses, TEXT(","))));
			Result->Properties.Add(TEXT("unsupported-bound-graph-schema"), QuoteDSLString(FString::Join(UnsupportedGraphSchemas, TEXT(","))));
			Result->Properties.Add(TEXT("unsupported-bound-graph-node-classes"), QuoteDSLString(FString::Join(UnsupportedGraphNodeClasses, TEXT(","))));
		}

		if (FProperty* FunctionReference = Node->GetClass()->FindPropertyByName(TEXT("OnMotionMatchingStateUpdatedFunction")))
		{
			void* ValuePtr = FunctionReference->ContainerPtrToValuePtr<void>(Node);
			FString ExportedReference;
			FunctionReference->ExportText_Direct(ExportedReference, ValuePtr, nullptr, Node, PPF_None);
			if (!ExportedReference.IsEmpty())
			{
				Result->Properties.Add(TEXT("on-motion-matching-state-updated-function-ref"), QuoteDSLString(ExportedReference));
			}
		}

		for (const FPoseInput& Input : CollectPoseInputs(Node))
		{
			if (TSharedPtr<FAnimNodeAST> ChildAST = ConvertAnimNode(Input.Node))
			{
				Result->AddChild(Input.PinName, ChildAST);
			}
		}
		return Result;
	}

	{
		// Convert "AnimGraphNode_XYZ" to "xyz" in kebab-case
		FString TypeName = ClassName;
		TypeName.RemoveFromStart(TEXT("AnimGraphNode_"));
		Result->NodeType = CamelToKebab(TypeName);
		Result->NodeClassPath = Node->GetClass()->GetPathName();
		Result->Coverage = EAnimNodeCoverage::Reflected;
		
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
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			// UE5.5+: GetResultNodeInsideState() was removed, use GetPoseSinkPinInsideState() instead
			UEdGraphPin* PoseSinkPin = StateNode->GetPoseSinkPinInsideState();
			if (PoseSinkPin && PoseSinkPin->LinkedTo.Num() > 0)
			{
				UAnimGraphNode_Base* AnimRoot = Cast<UAnimGraphNode_Base>(PoseSinkPin->LinkedTo[0]->GetOwningNode());
				if (AnimRoot)
				{
					State.Animation = ConvertAnimNode(AnimRoot);
				}
			}
#else
			// UE < 5.5: Use legacy GetResultNodeInsideState()
			UAnimGraphNode_StateResult* ResultNode = StateNode->GetResultNodeInsideState();
			if (ResultNode)
			{
				UAnimGraphNode_Base* AnimRoot = GetFirstConnectedPoseNode(ResultNode);
				if (AnimRoot)
				{
					State.Animation = ConvertAnimNode(AnimRoot);
				}
			}
#endif
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
					ReverseTrans.RuleGraph = Trans.RuleGraph;
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
