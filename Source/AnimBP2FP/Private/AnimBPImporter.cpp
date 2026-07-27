// AnimBPImporter.cpp - DSL to Animation Blueprint Importer
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBPImporter.h"
#include "AnimLangVariableCodec.h"

#if WITH_EDITOR

#include "AnimLangParser.h"
#include "AnimBPExporter.h"
#include "AnimLangDiffer.h"
#include "AnimLangPatcher.h"
#include "AnimBP2FPModule.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"

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
#include "AnimGraphNode_StateResult.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateNode.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimGraphNode_ModifyCurve.h"
#include "AnimGraphNode_BlendListBase.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "K2Node_AnimGetter.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_IdentityPose.h"
#include "AnimGraphNode_ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "AnimationGraph.h"
#include "AnimLangTokenizer.h"
#include "RigLangExporter.h"
#include "RigLangImporter.h"
#include "AnimLispWorkspace.h"

#include "Animation/AnimClassInterface.h"
#include "Animation/AnimLayerInterface.h"
#include "Factories/AnimBlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintLispAST.h"
#include "BlueprintLispConverter.h"
#include "EdGraphUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "PackageTools.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "Engine/MemberReference.h"
#include "UObject/UObjectIterator.h"
#include "UObject/StructOnScope.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Framework/Application/SlateApplication.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnimBPImporter, Log, All);

static FAnimLispTypeRef IMP_RigInputTypeFromPin(const FEdGraphPinType& PinType)
{
	FAnimLispTypeRef Result;
	const UObject* TypeObject = PinType.PinSubCategoryObject.Get();
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean) Result.CPPType = TEXT("bool");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int) Result.CPPType = TEXT("int32");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64) Result.CPPType = TEXT("int64");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
		Result.CPPType = PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double ? TEXT("double") : TEXT("float");
	else if (const UScriptStruct* Struct = Cast<UScriptStruct>(TypeObject)) Result.CPPType = Struct->GetStructCPPName();
	else Result.CPPType = PinType.PinCategory.ToString();
	Result.CPPTypeObject = TypeObject ? TypeObject->GetPathName() : FString();
	switch (PinType.ContainerType)
	{
	case EPinContainerType::Array: Result.ContainerType = TEXT("array"); break;
	case EPinContainerType::Set: Result.ContainerType = TEXT("set"); break;
	case EPinContainerType::Map: Result.ContainerType = TEXT("map"); break;
	default: break;
	}
	Result.Canonicalize();
	return Result;
}

namespace
{
	struct FImporterRigValidationContext
	{
		TMap<FString, FRigLangExportResult> ModulesByAsset;
	};

	thread_local FImporterRigValidationContext* GActiveImporterRigValidationContext = nullptr;
	thread_local const FAnimBPImportContext* GActiveAnimImportContext = nullptr;
}

// Helper: strip surrounding double quotes from a string
static FString StripQuotes(const FString& Input)
{
	FString Result = Input;
	if (Result.StartsWith(TEXT("\"")) && Result.EndsWith(TEXT("\"")))
	{
		Result = Result.Mid(1, Result.Len() - 2);
		FString Decoded;
		Decoded.Reserve(Result.Len());
		for (int32 Index = 0; Index < Result.Len(); ++Index)
		{
			const TCHAR Character = Result[Index];
			if (Character != '\\' || Index + 1 >= Result.Len())
			{
				Decoded += Character;
				continue;
			}

			const TCHAR Escaped = Result[++Index];
			switch (Escaped)
			{
			case '"': Decoded += '"'; break;
			case '\\': Decoded += '\\'; break;
			case 'n': Decoded += '\n'; break;
			case 'r': Decoded += '\r'; break;
			case 't': Decoded += '\t'; break;
			default:
				Decoded += '\\';
				Decoded += Escaped;
				break;
			}
		}
		Result = MoveTemp(Decoded);
	}
	return Result;
}

static void ResetStructToExportTextBaseline(const UScriptStruct* Struct, void* StructMemory)
{
	if (!Struct || !StructMemory) return;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
		{
			Property->ClearValue_InContainer(StructMemory, ArrayIndex);
			if (FStructProperty* NestedStructProperty = CastField<FStructProperty>(Property))
			{
				void* NestedMemory = NestedStructProperty->ContainerPtrToValuePtr<void>(StructMemory, ArrayIndex);
				ResetStructToExportTextBaseline(NestedStructProperty->Struct, NestedMemory);
			}
		}
	}
}

static bool ValidateExternalDependencies(const TArray<FAnimDependency>& Dependencies, FString& OutError)
{
	for (const FAnimDependency& Dependency : Dependencies)
	{
		if (!Dependency.Mode.Equals(TEXT("external"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Dependency '%s' has unsupported mode '%s'; only mode=external is valid"),
				*Dependency.ObjectPath, *Dependency.Mode);
			return false;
		}
		if (Dependency.ObjectPath.IsEmpty() || Dependency.ClassPath.IsEmpty() || Dependency.Role.IsEmpty())
		{
			OutError = TEXT("Dependency manifest entry requires object-path, class-path, and role");
			return false;
		}

		UClass* ExpectedClass = LoadObject<UClass>(nullptr, *Dependency.ClassPath);
		if (!ExpectedClass)
		{
			OutError = FString::Printf(TEXT("Dependency '%s' declares unloadable class '%s'"),
				*Dependency.ObjectPath, *Dependency.ClassPath);
			return false;
		}

		UObject* Object = nullptr;
		if (Dependency.Role.Equals(TEXT("control-rig"), ESearchCase::IgnoreCase)
			&& GActiveAnimImportContext)
		{
			FString RigAssetPath = Dependency.ObjectPath;
			int32 DotIndex = INDEX_NONE;
			if (RigAssetPath.FindChar(TEXT('.'), DotIndex)) RigAssetPath.LeftInline(DotIndex);
			if (const FAnimBPResolvedRig* ResolvedRig = GActiveAnimImportContext->ResolvedRigs.Find(RigAssetPath))
			{
				Object = ResolvedRig->Blueprint.Get();
			}
			else if (GActiveAnimImportContext->bAllowLegacyRigFallback
				&& GActiveAnimImportContext->LegacyExternalRigPaths.Contains(RigAssetPath))
			{
				Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Dependency.ObjectPath);
			}
		}
		else
		{
			Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Dependency.ObjectPath);
		}
		if (!Object)
		{
			OutError = FString::Printf(TEXT("Missing dependency '%s' (expected class '%s')"),
				*Dependency.ObjectPath, *Dependency.ClassPath);
			return false;
		}
		if (!Object->IsA(ExpectedClass))
		{
			OutError = FString::Printf(TEXT("Dependency '%s' class mismatch: expected '%s', loaded '%s'"),
				*Dependency.ObjectPath, *Dependency.ClassPath, *Object->GetClass()->GetPathName());
			return false;
		}
	}
	return true;
}

static FString NormalizeComparableAnimEscapes(const FString& Value)
{
	FString Result;
	Result.Reserve(Value.Len());
	for (int32 Index = 0; Index < Value.Len();)
	{
		if (Value[Index] != TEXT('\\'))
		{
			Result.AppendChar(Value[Index++]);
			continue;
		}
		const int32 SlashStart = Index;
		while (Index < Value.Len() && Value[Index] == TEXT('\\')) ++Index;
		if (Index < Value.Len() && Value[Index] == TEXT('"'))
		{
			Result.AppendChar(TEXT('\\'));
			Result.AppendChar(TEXT('"'));
			++Index;
		}
		else
		{
			Result.Append(Value.Mid(SlashStart, Index - SlashStart));
		}
	}
	return Result;
}

static FString BuildComparableAnimCanonical(
	const TSharedPtr<FAnimGraphAST>& Candidate,
	const FAnimGraphAST& Source)
{
	if (!Candidate.IsValid()) return FString();
	Candidate->Name = Source.Name;

	TMap<FString, TSharedPtr<FAnimNodeAST>> SourceNodesById;
	Source.VisitNodes([&](const TSharedPtr<FAnimNodeAST>& Node)
	{
		if (Node.IsValid() && !Node->NodeId.IsEmpty()) SourceNodesById.Add(Node->NodeId, Node);
	});
	TMap<FString, FAnimLispModuleId> SourceModuleByStagedPath;
	TMap<FString, FString> SourceAliasByStagedPath;
	Candidate->VisitNodes([&](const TSharedPtr<FAnimNodeAST>& Node)
	{
		if (!Node.IsValid() || !Node->RigBinding.IsSet()) return;
		const TSharedPtr<FAnimNodeAST>* SourceNode = SourceNodesById.Find(Node->NodeId);
		if (!SourceNode || !SourceNode->IsValid() || !(*SourceNode)->RigBinding.IsSet()) return;
		FAnimRigNodeBinding& Binding = Node->RigBinding.GetValue();
		const FAnimRigNodeBinding& SourceBinding = (*SourceNode)->RigBinding.GetValue();
		SourceModuleByStagedPath.Add(Binding.RigModule.AssetPath, SourceBinding.RigModule);
		SourceAliasByStagedPath.Add(Binding.RigModule.AssetPath, SourceBinding.ImportAlias);
		Binding.RigModule = SourceBinding.RigModule;
		Binding.ImportAlias = SourceBinding.ImportAlias;
	});

	for (FAnimLispImport& Import : Candidate->RigImports)
	{
		if (const FAnimLispImport* SourceImport = Source.RigImports.FindByPredicate(
			[&Import](const FAnimLispImport& Value) { return Value.Alias == Import.Alias; }))
		{
			Import = *SourceImport;
			continue;
		}
		const FString StagedPath = Import.Target.AssetPath;
		if (const FAnimLispModuleId* SourceModule = SourceModuleByStagedPath.Find(StagedPath))
		{
			Import.Target = *SourceModule;
			Import.Alias = SourceAliasByStagedPath.FindRef(StagedPath);
			if (const FAnimLispImport* SourceImport = Source.RigImports.FindByPredicate(
				[&Import](const FAnimLispImport& Value)
				{
					return Value.Target == Import.Target && Value.Alias == Import.Alias;
				}))
			{
				Import.ExpectedHash = SourceImport->ExpectedHash;
			}
		}
	}
	// Transient packages have no AssetRegistry dependency records. The manifest was
	// already preflighted before staging, so restore it as representation-only identity.
	Candidate->Dependencies = Source.Dependencies;
	return NormalizeComparableAnimEscapes(Candidate->ToString());
}

static bool ApplyAnimBlueprintMetadata(UAnimBlueprint* Blueprint, const FAnimBlueprintMetadata& Metadata, FString& OutError)
{
	if (Metadata.RootMotionMode.IsEmpty()) return true;
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		OutError = TEXT("Cannot restore RootMotionMode: AnimBlueprint generated class is unavailable");
		return false;
	}
	UObject* ClassDefaults = Blueprint->GeneratedClass->GetDefaultObject(false);
	FProperty* RootMotionMode = ClassDefaults
		? FindFProperty<FProperty>(ClassDefaults->GetClass(), TEXT("RootMotionMode"))
		: nullptr;
	if (!RootMotionMode)
	{
		OutError = TEXT("Cannot restore RootMotionMode: property is unavailable on the AnimInstance class defaults");
		return false;
	}
	void* ValuePtr = RootMotionMode->ContainerPtrToValuePtr<void>(ClassDefaults);
	if (!RootMotionMode->ImportText_Direct(*Metadata.RootMotionMode, ValuePtr, ClassDefaults, PPF_None))
	{
		OutError = FString::Printf(TEXT("Cannot restore RootMotionMode value '%s'"), *Metadata.RootMotionMode);
		return false;
	}
	Blueprint->Modify();
	Blueprint->MarkPackageDirty();
	return true;
}

namespace
{
	const FName AutoLayoutBehaviorName(TEXT("AutoLayout"));

	static AnimBP2FPImportLifecycle::FImportLifecycleContext MakeAnimLifecycleContext(
		UAnimBlueprint* Blueprint,
		UEdGraph* Graph,
		bool bIsFullRebuild,
		bool bIsIncremental,
		bool bWillCompile,
		bool bAutoLayout)
	{
		AnimBP2FPImportLifecycle::FImportLifecycleContext Context;
		Context.ImportSessionId = FGuid::NewGuid();
		Context.TargetAsset = Blueprint;
		Context.TargetGraph = Graph;
		Context.ScopeName = Graph ? FName(*Graph->GetName()) : NAME_None;
		Context.bIsFullRebuild = bIsFullRebuild;
		Context.bIsIncremental = bIsIncremental;
		Context.bIsHeadless = IsRunningCommandlet() || !FSlateApplication::IsInitialized();
		Context.bWillCompile = bWillCompile;
		if (bAutoLayout)
		{
			Context.RequestedBehaviors.Add(AutoLayoutBehaviorName);
		}
		return Context;
	}

	static void CollectAnimNodeChanges(
		const TSet<UEdGraphNode*>& PreExistingNodes,
		UEdGraph* Graph,
		TArray<AnimBP2FPImportLifecycle::FImportNodeChange>& OutChanges)
	{
		if (!Graph)
		{
			return;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !PreExistingNodes.Contains(Node))
			{
				AnimBP2FPImportLifecycle::FImportNodeChange Change;
				Change.Node = Node;
				Change.ChangeType = AnimBP2FPImportLifecycle::EImportNodeChangeType::Added;
				OutChanges.Add(Change);
			}
		}
	}

	static FString SanitizeHelperIdentifier(const FString& Input)
	{
		FString Result = Input;
		for (int32 Index = 0; Index < Result.Len(); ++Index)
		{
			const TCHAR Ch = Result[Index];
			if (!(FChar::IsAlnum(Ch) || Ch == TEXT('_')))
			{
				Result[Index] = TEXT('_');
			}
		}
		return Result;
	}

	static FString GetResolvedHelperGraphName(const FHelperGraphDef& Helper)
	{
		if (!Helper.GraphName.TrimStartAndEnd().IsEmpty())
		{
			return Helper.GraphName.TrimStartAndEnd();
		}
		return FString::Printf(TEXT("__ABP2FP_HG_%s"), *SanitizeHelperIdentifier(Helper.Id));
	}

	static FString GetResolvedGeneratedVarName(const FHelperGraphDef& Helper)
	{
		if (!Helper.GeneratedVar.TrimStartAndEnd().IsEmpty())
		{
			return Helper.GeneratedVar.TrimStartAndEnd();
		}
		return FString::Printf(TEXT("__abp2fp_gv_%s"), *SanitizeHelperIdentifier(Helper.Id));
	}

	static bool IMP_IsManagedHelperGraphName(const FString& GraphName)
	{
		return GraphName.StartsWith(TEXT("__ABP2FP_HG_"));
	}

	static bool IMP_IsManagedGeneratedVarName(const FString& VariableName)
	{
		return VariableName.StartsWith(TEXT("__abp2fp_gv_"));
	}

	static FString FindManagedGeneratedVarName(const UEdGraph* Graph)
	{
		if (!Graph)
		{
			return FString();
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(Node))
			{
				const FString VariableName = VarSetNode->VariableReference.GetMemberName().ToString();
				if (IMP_IsManagedGeneratedVarName(VariableName))
				{
					return VariableName;
				}
			}
		}

		return FString();
	}

	static FLispNodePtr CloneBlueprintLispNodeWithoutStableIds(const FLispNodePtr& Node)
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

				Copy->Children.Add(CloneBlueprintLispNodeWithoutStableIds(Child));
			}
		}

		return Copy;
	}

	static FString CanonicalizeBlueprintLispForComparison(const FString& LispCode)
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
			const FLispNodePtr CanonNode = CloneBlueprintLispNodeWithoutStableIds(Node);
			if (CanonNode.IsValid())
			{
				CanonicalNodes.Add(CanonNode->ToString(false, 0));
			}
		}
		return FString::Join(CanonicalNodes, TEXT("\n"));
	}

	static FString MakeComparableHelperSignature(const FHelperGraphDef& Helper)
	{
		return FString::Printf(
			TEXT("%s|%s|%s|%d|%s|%s"),
			*Helper.Id.TrimStartAndEnd(),
			*GetResolvedHelperGraphName(Helper),
			*GetResolvedGeneratedVarName(Helper),
			static_cast<int32>(Helper.GeneratedType),
			*Helper.UpdateGroup.TrimStartAndEnd(),
			*CanonicalizeBlueprintLispForComparison(Helper.DSL));
	}

	static bool AreEquivalentHelperSets(const TArray<FHelperGraphDef>& A, const TArray<FHelperGraphDef>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}

		TArray<FString> SignaturesA;
		TArray<FString> SignaturesB;
		SignaturesA.Reserve(A.Num());
		SignaturesB.Reserve(B.Num());

		for (const FHelperGraphDef& Helper : A)
		{
			SignaturesA.Add(MakeComparableHelperSignature(Helper));
		}
		for (const FHelperGraphDef& Helper : B)
		{
			SignaturesB.Add(MakeComparableHelperSignature(Helper));
		}

		SignaturesA.Sort();
		SignaturesB.Sort();
		return SignaturesA == SignaturesB;
	}

	static bool AreEquivalentLogicGraphSets(const TArray<FLogicGraphDef>& A, const TArray<FLogicGraphDef>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		TArray<FString> SignaturesA;
		TArray<FString> SignaturesB;
		for (const FLogicGraphDef& Graph : A)
		{
			SignaturesA.Add(Graph.Role + TEXT("|") + Graph.Kind + TEXT("|") + Graph.GraphName + TEXT("|")
				+ Graph.SchemaClassPath + TEXT("|") + CanonicalizeBlueprintLispForComparison(Graph.DSL));
		}
		for (const FLogicGraphDef& Graph : B)
		{
			SignaturesB.Add(Graph.Role + TEXT("|") + Graph.Kind + TEXT("|") + Graph.GraphName + TEXT("|")
				+ Graph.SchemaClassPath + TEXT("|") + CanonicalizeBlueprintLispForComparison(Graph.DSL));
		}
		SignaturesA.Sort();
		SignaturesB.Sort();
		return SignaturesA == SignaturesB;
	}

	static void RemoveStaleManagedHelperGraphs(UAnimBlueprint* Blueprint, const TArray<FHelperGraphDef>& DesiredHelpers)
	{
		if (!Blueprint)
		{
			return;
		}

		TSet<FName> DesiredGraphNames;
		for (const FHelperGraphDef& Helper : DesiredHelpers)
		{
			DesiredGraphNames.Add(FName(*GetResolvedHelperGraphName(Helper)));
		}

		TArray<UEdGraph*> GraphsToRemove;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			const FString GraphName = Graph->GetName();
			const FString ManagedGeneratedVar = FindManagedGeneratedVarName(Graph);
			const bool bManagedGraph = IMP_IsManagedHelperGraphName(GraphName) || IMP_IsManagedGeneratedVarName(ManagedGeneratedVar);
			if (!bManagedGraph)
			{
				continue;
			}

			if (!DesiredGraphNames.Contains(Graph->GetFName()))
			{
				GraphsToRemove.Add(Graph);
			}
		}

		for (UEdGraph* GraphToRemove : GraphsToRemove)
		{
			UE_LOG(LogAnimBPImporter, Log, TEXT("Removing stale managed helper graph: %s"), *GraphToRemove->GetName());
			FBlueprintEditorUtils::RemoveGraph(Blueprint, GraphToRemove, EGraphRemoveFlags::MarkTransient);
			GraphToRemove->MarkAsGarbage();
		}

		if (GraphsToRemove.Num() > 0)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
	}

	static void RemoveStaleManagedGeneratedVars(UAnimBlueprint* Blueprint, const TArray<FHelperGraphDef>& DesiredHelpers)
	{
		if (!Blueprint)
		{
			return;
		}

		TSet<FName> DesiredVariableNames;
		for (const FHelperGraphDef& Helper : DesiredHelpers)
		{
			DesiredVariableNames.Add(FName(*GetResolvedGeneratedVarName(Helper)));
		}

		TArray<FName> VariablesToRemove;
		for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
		{
			const FString VariableName = ExistingVar.VarName.ToString();
			if (IMP_IsManagedGeneratedVarName(VariableName) && !DesiredVariableNames.Contains(ExistingVar.VarName))
			{
				VariablesToRemove.Add(ExistingVar.VarName);
			}
		}

		for (const FName& VariableToRemove : VariablesToRemove)
		{
			UE_LOG(LogAnimBPImporter, Log, TEXT("Removing stale managed generated var: %s"), *VariableToRemove.ToString());
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VariableToRemove);
		}
	}

	static FString IMP_NormalizeBindingToken(const FString& In)
	{
		FString Out = In.ToLower();
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	}

	static FString KebabToCamelBindingName(const FString& Input)
	{
		FString Result;
		bool bCapNext = true;
		for (int32 i = 0; i < Input.Len(); i++)
		{
			const TCHAR Ch = Input[i];
			if (Ch == '-' || Ch == '_')
			{
				bCapNext = true;
				continue;
			}
			if (bCapNext)
			{
				Result += FChar::ToUpper(Ch);
				bCapNext = false;
			}
			else
			{
				Result += Ch;
			}
		}
		return Result;
	}

	static UEdGraphPin* FindInputValuePin(UAnimGraphNode_Base* Node, const FString& KebabKey)
	{
		if (!Node) return nullptr;

		const FString CamelKey = KebabToCamelBindingName(KebabKey);
		const FString NormalizedKey = IMP_NormalizeBindingToken(KebabKey);
		UEdGraphPin* BestMatch = nullptr;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input)
			{
				continue;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
				&& (Pin->PinType.PinSubCategoryObject == FPoseLink::StaticStruct()
					|| Pin->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct()))
			{
				continue;
			}

			const FString PinNameStr = Pin->PinName.ToString();
			if (PinNameStr == CamelKey || PinNameStr.Equals(CamelKey, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
			if (IMP_NormalizeBindingToken(PinNameStr) == NormalizedKey)
			{
				BestMatch = Pin;
			}
		}

		return BestMatch;
	}

	static bool TryParseBindingForm(const FString& Value, FString& OutFormName, FString& OutArgument)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("(")) || !Trimmed.EndsWith(TEXT(")")))
		{
			return false;
		}

		const FString Inner = Trimmed.Mid(1, Trimmed.Len() - 2).TrimStartAndEnd();
		if (Inner.IsEmpty())
		{
			return false;
		}

		int32 SplitIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Inner.Len(); ++Index)
		{
			if (FChar::IsWhitespace(Inner[Index]))
			{
				SplitIndex = Index;
				break;
			}
		}

		if (SplitIndex == INDEX_NONE)
		{
			OutFormName = Inner;
			OutArgument.Reset();
			return true;
		}

		OutFormName = Inner.Left(SplitIndex);
		OutArgument = Inner.Mid(SplitIndex + 1).TrimStartAndEnd();
		return true;
	}

	static UEdGraphPin* FindFirstDataOutputPin(UEdGraphNode* Node)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UK2Node_VariableGet* CreateMemberVariableGetNode(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FString& VariableName, UEdGraphNode* TargetNode)
	{
		if (!Blueprint || !Graph || VariableName.IsEmpty())
		{
			return nullptr;
		}

		UK2Node_VariableGet* VarNode = NewObject<UK2Node_VariableGet>(Graph);
		if (!VarNode)
		{
			return nullptr;
		}

		VarNode->VariableReference.SetSelfMember(FName(*VariableName));
		if (TargetNode)
		{
			VarNode->NodePosX = TargetNode->NodePosX - 240;
			VarNode->NodePosY = TargetNode->NodePosY;
		}
		VarNode->CreateNewGuid();
		VarNode->PostPlacedNewNode();
		Graph->AddNode(VarNode, false, false);
		VarNode->AllocateDefaultPins();
		return VarNode;
	}

	static TMap<FName, FAnimGraphNodePropertyBinding>* GetMutablePropertyBindingMap(UAnimGraphNode_Base* Node)
	{
		if (!Node || !Node->GetMutableBinding())
		{
			return nullptr;
		}

		UObject* BindingObject = reinterpret_cast<UObject*>(Node->GetMutableBinding());
		if (FMapProperty* MapProperty = FindFProperty<FMapProperty>(BindingObject->GetClass(), TEXT("PropertyBindings")))
		{
			void* MapPtr = MapProperty->ContainerPtrToValuePtr<void>(BindingObject);
			return reinterpret_cast<TMap<FName, FAnimGraphNodePropertyBinding>*>(MapPtr);
		}

		return nullptr;
	}

	static FString JoinBindingPath(const TArray<FString>& PropertyPath)
	{
		return FString::Join(PropertyPath, TEXT("."));
	}

	static bool ParseBindPathArgument(const FString& Argument, TArray<FString>& OutPropertyPath)
	{
		OutPropertyPath.Reset();
		FString Trimmed = Argument.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return false;
		}

		if (Trimmed.StartsWith(TEXT("[")) && Trimmed.EndsWith(TEXT("]")))
		{
			const FString Inner = Trimmed.Mid(1, Trimmed.Len() - 2);
			int32 Index = 0;
			while (Index < Inner.Len())
			{
				while (Index < Inner.Len() && FChar::IsWhitespace(Inner[Index]))
				{
					++Index;
				}
				if (Index >= Inner.Len())
				{
					break;
				}

				FString Token;
				if (Inner[Index] == TEXT('"'))
				{
					++Index;
					while (Index < Inner.Len())
					{
						if (Inner[Index] == TEXT('"') && (Index == 0 || Inner[Index - 1] != TEXT('\\')))
						{
							++Index;
							break;
						}
						Token.AppendChar(Inner[Index]);
						++Index;
					}
				}
				else
				{
					const int32 Start = Index;
					while (Index < Inner.Len() && !FChar::IsWhitespace(Inner[Index]))
					{
						++Index;
					}
					Token = Inner.Mid(Start, Index - Start);
				}

				Token = StripQuotes(Token).TrimStartAndEnd();
				if (!Token.IsEmpty())
				{
					OutPropertyPath.Add(Token);
				}
			}
		}
		else
		{
			const FString CleanPath = StripQuotes(Trimmed);
			CleanPath.ParseIntoArray(OutPropertyPath, TEXT("."), true);
			for (FString& Segment : OutPropertyPath)
			{
				Segment = Segment.TrimStartAndEnd();
			}
		}

		OutPropertyPath.RemoveAll([](const FString& Segment)
		{
			return Segment.TrimStartAndEnd().IsEmpty();
		});
		return OutPropertyPath.Num() > 0;
	}

	static bool ParseBindPathForm(const FString& Value, TArray<FString>& OutPropertyPath,
		EAnimGraphNodePropertyBindingType& OutType, FName& OutContextId, int32& OutArrayIndex,
		bool& bOutOnlyUpdateWhenActive, bool& bOutHasExplicitType)
	{
		OutPropertyPath.Reset();
		OutType = EAnimGraphNodePropertyBindingType::Property;
		OutContextId = NAME_None;
		OutArrayIndex = INDEX_NONE;
		bOutOnlyUpdateWhenActive = false;
		bOutHasExplicitType = false;

		const FLispParseResult Parsed = FLispParser::Parse(Value);
		if (!Parsed.bSuccess || Parsed.Nodes.Num() != 1 || !Parsed.Nodes[0].IsValid()
			|| !Parsed.Nodes[0]->IsForm(TEXT("bind-path")))
		{
			return false;
		}

		const FLispNodePtr Form = Parsed.Nodes[0];
		const FLispNodePtr PathNode = Form->Get(1);
		if (!PathNode.IsValid())
		{
			return false;
		}
		if (PathNode->IsString() || PathNode->IsSymbol())
		{
			PathNode->StringValue.ParseIntoArray(OutPropertyPath, TEXT("."), true);
		}
		else if (PathNode->IsList())
		{
			for (const FLispNodePtr& Segment : PathNode->Children)
			{
				if (Segment.IsValid() && (Segment->IsString() || Segment->IsSymbol()))
				{
					OutPropertyPath.Add(Segment->StringValue);
				}
			}
		}
		OutPropertyPath.RemoveAll([](const FString& Segment) { return Segment.TrimStartAndEnd().IsEmpty(); });
		if (OutPropertyPath.IsEmpty())
		{
			return false;
		}

		if (const FLispNodePtr TypeNode = Form->GetKeywordArg(TEXT(":type"));
			TypeNode.IsValid() && (TypeNode->IsString() || TypeNode->IsSymbol()))
		{
			bOutHasExplicitType = true;
			if (TypeNode->StringValue.Equals(TEXT("function"), ESearchCase::IgnoreCase))
			{
				OutType = EAnimGraphNodePropertyBindingType::Function;
			}
			else if (!TypeNode->StringValue.Equals(TEXT("property"), ESearchCase::IgnoreCase))
			{
				return false;
			}
		}
		if (const FLispNodePtr ContextNode = Form->GetKeywordArg(TEXT(":context"));
			ContextNode.IsValid() && (ContextNode->IsString() || ContextNode->IsSymbol()))
		{
			OutContextId = FName(*ContextNode->StringValue);
		}
		if (const FLispNodePtr ArrayNode = Form->GetKeywordArg(TEXT(":array-index"));
			ArrayNode.IsValid() && ArrayNode->IsNumber())
		{
			OutArrayIndex = static_cast<int32>(ArrayNode->NumberValue);
		}
		if (const FLispNodePtr ActiveNode = Form->GetKeywordArg(TEXT(":only-update-when-active"));
			ActiveNode.IsValid() && (ActiveNode->IsString() || ActiveNode->IsSymbol()))
		{
			bOutOnlyUpdateWhenActive = ActiveNode->StringValue.Equals(TEXT("true"), ESearchCase::IgnoreCase);
		}
		return true;
	}

	static FString ResolveBindingPropertyName(UAnimGraphNode_Base* Node, const FString& KebabKey)
	{
		if (!Node)
		{
			return FString();
		}

		const FString CamelKey = KebabToCamelBindingName(KebabKey);
		const FString NormalizedKey = IMP_NormalizeBindingToken(KebabKey);
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			const FString PinNameStr = Pin->PinName.ToString();
			if (PinNameStr == CamelKey || PinNameStr.Equals(CamelKey, ESearchCase::IgnoreCase) || IMP_NormalizeBindingToken(PinNameStr) == NormalizedKey)
			{
				return PinNameStr;
			}
		}

		for (TFieldIterator<FStructProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
		{
			FStructProperty* StructProp = *PropIt;
			if (!StructProp->Struct || !StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
			{
				continue;
			}

			for (TFieldIterator<FProperty> InnerIt(StructProp->Struct); InnerIt; ++InnerIt)
			{
				FProperty* InnerProp = *InnerIt;
				const FString PropName = InnerProp->GetName();
				if (PropName == CamelKey || PropName.Equals(CamelKey, ESearchCase::IgnoreCase) || IMP_NormalizeBindingToken(PropName) == NormalizedKey)
				{
					return PropName;
				}
			}
			break;
		}

		return FString();
	}

	static bool ParseExposedPinNames(const FString& Value, TSet<FString>& OutNormalizedNames)
	{
		OutNormalizedNames.Reset();
		const FLispParseResult Parsed = FLispParser::Parse(Value);
		if (!Parsed.bSuccess || Parsed.Nodes.Num() != 1 || !Parsed.Nodes[0].IsValid()
			|| !Parsed.Nodes[0]->IsForm(TEXT("pin-names")))
		{
			return false;
		}
		for (int32 Index = 1; Index < Parsed.Nodes[0]->Num(); ++Index)
		{
			const FLispNodePtr NameNode = Parsed.Nodes[0]->Get(Index);
			if (NameNode.IsValid() && (NameNode->IsString() || NameNode->IsSymbol()))
			{
				OutNormalizedNames.Add(IMP_NormalizeBindingToken(NameNode->StringValue));
			}
		}
		return true;
	}

	static bool RestoreCustomPinVisibility(UAnimGraphNode_Base* Node, const TSet<FString>& NormalizedNames)
	{
		if (!Node || NormalizedNames.IsEmpty()) return false;
		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Node->GetClass(), TEXT("CustomPinProperties"));
		FStructProperty* ElementProperty = ArrayProperty ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		if (!ElementProperty || !ElementProperty->Struct) return false;

		FNameProperty* NameProperty = FindFProperty<FNameProperty>(ElementProperty->Struct, TEXT("PropertyName"));
		FBoolProperty* ShowProperty = FindFProperty<FBoolProperty>(ElementProperty->Struct, TEXT("bShowPin"));
		if (!NameProperty || !ShowProperty) return false;

		bool bChanged = false;
		FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Node));
		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			void* Element = ArrayHelper.GetRawPtr(Index);
			const FString Name = NameProperty->GetPropertyValue_InContainer(Element).ToString();
			if (NormalizedNames.Contains(IMP_NormalizeBindingToken(Name))
				&& !ShowProperty->GetPropertyValue_InContainer(Element))
			{
				ShowProperty->SetPropertyValue_InContainer(Element, true);
				bChanged = true;
			}
		}
		return bChanged;
	}

	static bool ConfigureTypedCustomPinVisibility(
		UAnimGraphNode_ControlRig* Node,
		const TSet<FName>& PublicInputProperties,
		const TSet<FName>& BoundInputProperties,
		FString& OutError)
	{
		FArrayProperty* ArrayProperty = Node
			? FindFProperty<FArrayProperty>(Node->GetClass(), TEXT("CustomPinProperties"))
			: nullptr;
		FStructProperty* ElementProperty = ArrayProperty ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		FNameProperty* NameProperty = ElementProperty && ElementProperty->Struct
			? FindFProperty<FNameProperty>(ElementProperty->Struct, TEXT("PropertyName"))
			: nullptr;
		FBoolProperty* ShowProperty = ElementProperty && ElementProperty->Struct
			? FindFProperty<FBoolProperty>(ElementProperty->Struct, TEXT("bShowPin"))
			: nullptr;
		if (!ArrayProperty || !NameProperty || !ShowProperty)
		{
			OutError = TEXT("Control Rig node has no readable CustomPinProperties schema");
			return false;
		}

		TMap<FName, int32> PropertyCounts;
		FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Node));
		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			void* Element = ArrayHelper.GetRawPtr(Index);
			const FName PropertyName = NameProperty->GetPropertyValue_InContainer(Element);
			if (!PublicInputProperties.Contains(PropertyName)) continue;
			if (++PropertyCounts.FindOrAdd(PropertyName) != 1)
			{
				OutError = FString::Printf(TEXT("Control Rig CustomPinProperties contains duplicate exact input '%s'"), *PropertyName.ToString());
				return false;
			}
			ShowProperty->SetPropertyValue_InContainer(Element, BoundInputProperties.Contains(PropertyName));
		}
		for (const FName BoundProperty : BoundInputProperties)
		{
			if (PropertyCounts.FindRef(BoundProperty) != 1)
			{
				OutError = FString::Printf(TEXT("Control Rig binding has no unique exact CustomPinProperty '%s'"), *BoundProperty.ToString());
				return false;
			}
		}

		Node->ReconstructNode();

		TSet<FName> VisibleInputProperties;
		PropertyCounts.Reset();
		FScriptArrayHelper ReconstructedArray(
			ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Node));
		for (int32 Index = 0; Index < ReconstructedArray.Num(); ++Index)
		{
			void* Element = ReconstructedArray.GetRawPtr(Index);
			const FName PropertyName = NameProperty->GetPropertyValue_InContainer(Element);
			if (!PublicInputProperties.Contains(PropertyName)) continue;
			if (++PropertyCounts.FindOrAdd(PropertyName) != 1)
			{
				OutError = FString::Printf(TEXT("Control Rig CustomPinProperties reconstruct produced duplicate exact input '%s'"), *PropertyName.ToString());
				return false;
			}
			if (ShowProperty->GetPropertyValue_InContainer(Element))
			{
				VisibleInputProperties.Add(PropertyName);
			}
		}
		if (VisibleInputProperties.Num() != BoundInputProperties.Num()
			|| !VisibleInputProperties.Includes(BoundInputProperties)
			|| !BoundInputProperties.Includes(VisibleInputProperties))
		{
			OutError = TEXT("Control Rig visible typed inputs do not exactly match the binding set");
			return false;
		}
		return true;
	}
}

// ========== Name Conversion Helpers ==========

FString FAnimBPImporter::KebabToCamel(const FString& Input)
{
	FString Result;
	bool bCapNext = true;
	for (int32 i = 0; i < Input.Len(); i++)
	{
		TCHAR Ch = Input[i];
		if (Ch == '-' || Ch == '_')
		{
			bCapNext = true;
			continue;
		}
		if (bCapNext)
		{
			Result += FChar::ToUpper(Ch);
			bCapNext = false;
		}
		else
		{
			Result += Ch;
		}
	}
	return Result;
}

FString FAnimBPImporter::KebabToCamelClassName(const FString& KebabName)
{
	// Special mappings for known node types
	static TMap<FString, FString> SpecialMappings;
	if (SpecialMappings.Num() == 0)
	{
		SpecialMappings.Add(TEXT("sequence-player"), TEXT("AnimGraphNode_SequencePlayer"));
		SpecialMappings.Add(TEXT("blendspace-player"), TEXT("AnimGraphNode_BlendSpacePlayer"));
		SpecialMappings.Add(TEXT("blend"), TEXT("AnimGraphNode_TwoWayBlend"));
		SpecialMappings.Add(TEXT("apply-additive"), TEXT("AnimGraphNode_ApplyAdditive"));
		SpecialMappings.Add(TEXT("layered-bone-blend"), TEXT("AnimGraphNode_LayeredBoneBlend"));
		SpecialMappings.Add(TEXT("state-machine"), TEXT("AnimGraphNode_StateMachine"));
		SpecialMappings.Add(TEXT("identity-pose"), TEXT("AnimGraphNode_IdentityPose"));
		SpecialMappings.Add(TEXT("root"), TEXT("AnimGraphNode_Root"));
		// Note: blend-list is NOT here — it uses :class property for the actual type
	}
	
	const FString* Found = SpecialMappings.Find(KebabName);
	if (Found)
	{
		return *Found;
	}
	
	// Generic conversion: kebab-case -> AnimGraphNode_CamelCase
	return TEXT("AnimGraphNode_") + KebabToCamel(KebabName);
}

// ========== Node Class Discovery ==========

UClass* FAnimBPImporter::FindAnimNodeClass(const FString& DSLNodeType)
{
	FString ClassName;
	
	// If the input already looks like a UE class name, use it directly
	if (DSLNodeType.StartsWith(TEXT("AnimGraphNode_")))
	{
		ClassName = DSLNodeType;
	}
	else
	{
		ClassName = KebabToCamelClassName(DSLNodeType);
	}
	
	if (ClassName.IsEmpty())
	{
		return nullptr;
	}
	
	// Try to find the UClass by name — iterate all classes
	// (FindObject with ANY_PACKAGE is deprecated in UE5)
	
	// Fallback: iterate all classes to find by name suffix
	FString ShortName = ClassName;
	ShortName.RemoveFromStart(TEXT("AnimGraphNode_"));
	
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* TestClass = *It;
		if (TestClass->IsChildOf(UAnimGraphNode_Base::StaticClass()) && !TestClass->HasAnyClassFlags(CLASS_Abstract))
		{
			FString TestName = TestClass->GetName();
			if (TestName == ClassName || TestName.EndsWith(ShortName))
			{
				return TestClass;
			}
		}
	}
	
	UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not find UClass for node type '%s' (tried '%s')"), *DSLNodeType, *ClassName);
	return nullptr;
}

// ========== Pin Utilities ==========

UEdGraphPin* FAnimBPImporter::FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
	if (!Node) return nullptr;
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinName.ToString() == PinName)
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraphPin* FAnimBPImporter::FindOutputPosePin(UAnimGraphNode_Base* Node)
{
	if (!Node) return nullptr;
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && 
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraphPin* FAnimBPImporter::FindInputPosePin(UAnimGraphNode_Base* Node, const FString& KebabPinName)
{
	if (!Node) return nullptr;
	
	// Convert kebab-case pin name to the UE pin name (CamelCase)
	FString CamelPinName = KebabToCamel(KebabPinName);
	
	// Build a normalized version for fuzzy matching (lowercase, no spaces/hyphens/underscores)
	auto Normalize = [](const FString& In) -> FString
	{
		FString Out = In.ToLower();
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	};
	FString NormalizedTarget = Normalize(KebabPinName);
	
	UEdGraphPin* BestMatch = nullptr;
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && 
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			FString PinNameStr = Pin->PinName.ToString();

			// Try exact CamelCase match
			if (PinNameStr == CamelPinName)
			{
				return Pin;
			}
			// Try case-insensitive match
			if (PinNameStr.Equals(CamelPinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
			// Try normalized fuzzy match
			if (Normalize(PinNameStr) == NormalizedTarget)
			{
				BestMatch = Pin;
			}
		}
	}
	
	if (BestMatch) return BestMatch;
	
	// Fallback: if only one input pose pin exists and no specific name requested, use it
	UEdGraphPin* SingleInput = nullptr;
	int32 PoseInputCount = 0;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && 
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			SingleInput = Pin;
			PoseInputCount++;
		}
	}
	if (PoseInputCount == 1)
	{
		return SingleInput;
	}
	
	UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not find input pose pin '%s' (CamelCase: '%s') on node '%s'"), 
		*KebabPinName, *CamelPinName, *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	return nullptr;
}

void FAnimBPImporter::ConnectPins(UEdGraphPin* OutputPin, UEdGraphPin* InputPin)
{
	if (!OutputPin || !InputPin)
	{
		UE_LOG(LogAnimBPImporter, Warning, TEXT("ConnectPins: null pin(s)"));
		return;
	}
	
	// Break existing connections
	OutputPin->BreakAllPinLinks();
	InputPin->BreakAllPinLinks();
	
	// Make the connection
	OutputPin->MakeLinkTo(InputPin);
}

// ========== Node Property Setting ==========

bool FAnimBPImporter::SetNodeProperty(UAnimGraphNode_Base* Node, const FString& KebabKey, const FString& Value)
{
	if (!Node) return false;
	
	FString CamelKey = KebabToCamel(KebabKey);
	
	// Normalize helper: lowercase, strip all spaces/hyphens/underscores
	auto Normalize = [](const FString& In) -> FString
	{
		FString Out = In.ToLower();
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	};
	
	FString NormalizedKey = Normalize(KebabKey);
	
	// Strip quotes from value
	FString CleanValue = StripQuotes(Value);
	
	// Pass 1: Try exact CamelCase match, then case-insensitive, then normalized fuzzy
	UEdGraphPin* BestMatch = nullptr;
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && 
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
		{
			FString PinNameStr = Pin->PinName.ToString();

			// Exact CamelCase match
			if (PinNameStr == CamelKey)
			{
				BestMatch = Pin;
				break;
			}
			// Case-insensitive match
			if (PinNameStr.Equals(CamelKey, ESearchCase::IgnoreCase))
			{
				BestMatch = Pin;
				break;
			}
			// Normalized fuzzy match (handles "CurveValues 0" vs "CurveValues0",
			// "BlendTime 0" vs "BlendTime0", etc.)
			if (Normalize(PinNameStr) == NormalizedKey)
			{
				BestMatch = Pin;
			}
		}
	}
	
	const bool bPinWasSet = BestMatch != nullptr;
	if (BestMatch)
	{
		BestMatch->DefaultValue = CleanValue;
	}

	// Some animation editor nodes own semantic settings directly rather than in
	// their internal FAnimNode_* struct. FMemberReference values use a stable
	// "-ref" DSL suffix to distinguish them from ordinary scalar properties.
	for (TFieldIterator<FProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
	{
		FProperty* DirectProp = *PropIt;
		const FString DirectName = DirectProp->GetName();
		bool bNameMatches = Normalize(DirectName) == NormalizedKey
			|| DirectName.Equals(CamelKey, ESearchCase::IgnoreCase);
		if (!bNameMatches)
		{
			const FStructProperty* StructProperty = CastField<FStructProperty>(DirectProp);
			bNameMatches = StructProperty && StructProperty->Struct == FMemberReference::StaticStruct()
				&& NormalizedKey == Normalize(DirectName) + TEXT("ref");
		}
		if (!bNameMatches)
		{
			continue;
		}

		void* ValuePtr = DirectProp->ContainerPtrToValuePtr<void>(Node);
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(DirectProp);
			StructProperty && StructProperty->Struct == FMemberReference::StaticStruct()
			&& Value.StartsWith(TEXT("(member-ref ")))
		{
			const FLispParseResult ParsedReference = FLispParser::Parse(Value);
			if (!ParsedReference.bSuccess || ParsedReference.Nodes.Num() != 1
				|| !ParsedReference.Nodes[0].IsValid() || !ParsedReference.Nodes[0]->IsForm(TEXT("member-ref")))
			{
				return false;
			}

			const FLispNodePtr ReferenceForm = ParsedReference.Nodes[0];
			const FLispNodePtr NameNode = ReferenceForm->GetKeywordArg(TEXT(":name"));
			const FLispNodePtr SelfNode = ReferenceForm->GetKeywordArg(TEXT(":self"));
			if (!NameNode.IsValid() || (!NameNode->IsString() && !NameNode->IsSymbol()))
			{
				return false;
			}

			FMemberReference* MemberReference = static_cast<FMemberReference*>(ValuePtr);
			const FName MemberName(*NameNode->StringValue);
			const bool bSelfContext = SelfNode.IsValid()
				&& SelfNode->StringValue.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			if (bSelfContext)
			{
				MemberReference->SetSelfMember(MemberName, FGuid());
				return true;
			}

			const FLispNodePtr ParentNode = ReferenceForm->GetKeywordArg(TEXT(":parent"));
			if (!ParentNode.IsValid() || (!ParentNode->IsString() && !ParentNode->IsSymbol()))
			{
				return false;
			}
			if (UClass* ParentClass = LoadObject<UClass>(nullptr, *ParentNode->StringValue))
			{
				MemberReference->SetExternalMember(MemberName, ParentClass);
				return true;
			}
			return false;
		}

		// Runtime FAnimNodeFunctionRef values are exported as
		// (FunctionName="..."). The editor-side property with the same name is an
		// FMemberReference, so import the runtime struct through reflection and
		// translate its function name to a self member reference.
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(DirectProp);
			StructProperty && StructProperty->Struct == FMemberReference::StaticStruct())
		{
			for (TFieldIterator<FStructProperty> AnimPropIt(Node->GetClass()); AnimPropIt; ++AnimPropIt)
			{
				FStructProperty* AnimStructProperty = *AnimPropIt;
				if (!AnimStructProperty->Struct || !AnimStructProperty->Struct->IsChildOf(FAnimNode_Base::StaticStruct())) continue;
				for (TFieldIterator<FProperty> InnerIt(AnimStructProperty->Struct); InnerIt; ++InnerIt)
				{
					FStructProperty* RuntimeFunctionProperty = CastField<FStructProperty>(*InnerIt);
					if (!RuntimeFunctionProperty || Normalize(RuntimeFunctionProperty->GetName()) != NormalizedKey) continue;
					FNameProperty* FunctionNameProperty = FindFProperty<FNameProperty>(RuntimeFunctionProperty->Struct, TEXT("FunctionName"));
					if (!FunctionNameProperty) continue;

					FStructOnScope RuntimeFunctionValue(RuntimeFunctionProperty->Struct);
					ResetStructToExportTextBaseline(RuntimeFunctionProperty->Struct, RuntimeFunctionValue.GetStructMemory());
					if (!RuntimeFunctionProperty->ImportText_Direct(*CleanValue, RuntimeFunctionValue.GetStructMemory(), nullptr, PPF_None)) continue;
					const FName FunctionName = FunctionNameProperty->GetPropertyValue(
						FunctionNameProperty->ContainerPtrToValuePtr<void>(RuntimeFunctionValue.GetStructMemory()));
					if (!FunctionName.IsNone())
					{
						static_cast<FMemberReference*>(ValuePtr)->SetSelfMember(FunctionName, FGuid());
						return true;
					}
				}
				break;
			}
		}

		if (DirectProp->ImportText_Direct(*CleanValue, ValuePtr, Node, PPF_None))
		{
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(DirectProp);
				StructProperty && StructProperty->Struct == FMemberReference::StaticStruct())
			{
				FMemberReference* MemberReference = static_cast<FMemberReference*>(ValuePtr);
				if (MemberReference->IsSelfContext())
				{
					MemberReference->SetSelfMember(MemberReference->GetMemberName(), FGuid());
				}
			}
			return true;
		}
	}
	
	// Pass 2: Try FProperty reflection on the internal anim node struct
	// This handles properties that are not exposed as pins but are FProperty members
	// The internal FAnimNode_xxx struct is stored as the first struct property
	// of the UAnimGraphNode_xxx.
	for (TFieldIterator<FStructProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
	{
		FStructProperty* StructProp = *PropIt;
		if (StructProp->Struct && StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
		{
			void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(Node);
			
			// Search for the property by name in the internal struct
			for (TFieldIterator<FProperty> InnerIt(StructProp->Struct); InnerIt; ++InnerIt)
			{
				FProperty* InnerProp = *InnerIt;
				FString PropName = InnerProp->GetName();
				
				if (Normalize(PropName) == NormalizedKey || PropName.Equals(CamelKey, ESearchCase::IgnoreCase))
				{
					void* ValuePtr = InnerProp->ContainerPtrToValuePtr<void>(StructPtr);
					if (const FStructProperty* InnerStructProperty = CastField<FStructProperty>(InnerProp))
					{
						FStructOnScope ImportedValue(InnerStructProperty->Struct);
						ResetStructToExportTextBaseline(InnerStructProperty->Struct, ImportedValue.GetStructMemory());
						if (InnerProp->ImportText_Direct(*CleanValue, ImportedValue.GetStructMemory(), nullptr, PPF_None))
						{
							InnerProp->CopyCompleteValue(ValuePtr, ImportedValue.GetStructMemory());
							return true;
						}
						return false;
					}
					const TCHAR* ImportResult = InnerProp->ImportText_Direct(*CleanValue, ValuePtr, nullptr, PPF_None);
					if (ImportResult != nullptr)
					{
						return true;
					}
				}
			}
			break; // Only check the first FAnimNode struct
		}
	}
	
	UE_LOG(LogAnimBPImporter, Verbose, TEXT("SetNodeProperty: Could not find pin or property for '%s' on '%s'"), 
		*KebabKey, *Node->GetClass()->GetName());
	return bPinWasSet;
}

// ========== Blueprint Creation ==========

UAnimBlueprint* FAnimBPImporter::CreateEmptyBlueprint(
	const FString& PackagePath,
	const FString& BlueprintName,
	const FString& SkeletonPath,
	const bool bTransient)
{
	// Find or load the skeleton
	USkeleton* Skeleton = nullptr;
	if (!SkeletonPath.IsEmpty())
	{
		FString CleanPath = SkeletonPath;
		if (CleanPath.StartsWith(TEXT("\"")) && CleanPath.EndsWith(TEXT("\"")))
		{
			CleanPath = CleanPath.Mid(1, CleanPath.Len() - 2);
		}
		Skeleton = LoadObject<USkeleton>(nullptr, *CleanPath);
		if (!Skeleton)
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not load skeleton: %s"), *CleanPath);
		}
	}
	
	// Create the package
	FString FullPackagePath = PackagePath / BlueprintName;
	UPackage* Package = bTransient ? GetTransientPackage() : CreatePackage(*FullPackagePath);
	if (!Package)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("Failed to create package: %s"), *FullPackagePath);
		return nullptr;
	}
	
	// Use the factory to create the Animation Blueprint
	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->TargetSkeleton = Skeleton;
	
	UObject* CreatedAsset = Factory->FactoryCreateNew(
		UAnimBlueprint::StaticClass(),
		Package,
		bTransient
			? MakeUniqueObjectName(Package, UAnimBlueprint::StaticClass(), FName(*BlueprintName))
			: FName(*BlueprintName),
		bTransient ? RF_Transient : RF_Public | RF_Standalone,
		nullptr,
		GWarn
	);
	
	UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(CreatedAsset);
	if (!AnimBlueprint)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("Factory failed to create AnimBlueprint"));
		return nullptr;
	}
	
	// Notify asset registry
	if (!bTransient)
	{
		FAssetRegistryModule::AssetCreated(AnimBlueprint);
		Package->MarkPackageDirty();
	}
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("Created AnimBlueprint: %s (Skeleton: %s)"), 
		*BlueprintName, Skeleton ? *Skeleton->GetName() : TEXT("none"));
	
	return AnimBlueprint;
}

UEdGraph* FAnimBPImporter::FindAnimGraph(UAnimBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("[INTERNAL] FindAnimGraph: Blueprint is null"));
		return nullptr;
	}
	
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetSchema() && Graph->GetSchema()->IsA<UAnimationGraphSchema>()
			&& Graph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return IsValid(Node) && Node->IsA<UAnimGraphNode_Root>(); }))
		{
			return Graph;
		}
	}
	
	UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not find AnimGraph in blueprint"));
	return nullptr;
}

// ========== Variable Building ==========

bool FAnimBPImporter::BuildVariables(UAnimBlueprint* Blueprint, const TArray<FVariableDef>& Variables)
{
	if (!Blueprint || Variables.Num() == 0) return true;
	
	for (const FVariableDef& Var : Variables)
	{
		bool bExists = false;
		for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
		{
			if (ExistingVar.VarName == FName(*Var.Name))
			{
				bExists = true;
				break;
			}
		}
		if (bExists)
		{
			continue;
		}

		FEdGraphPinType PinType;
		FString TypeError;
		if (!FAnimLangVariableCodec::BuildPinType(Var, PinType, TypeError))
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:VariableType] Variable '%s': %s"), *Var.Name, *TypeError);
			return false;
		}

		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*Var.Name), PinType, Var.DefaultValue))
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[FAILED:VariableCreate] Could not add variable '%s'"), *Var.Name);
			return false;
		}
		const int32 AddedIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Var.Name));
		if (AddedIndex == INDEX_NONE)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[FAILED:VariableCreate] Variable '%s' was reported added but cannot be found"), *Var.Name);
			return false;
		}
		Blueprint->NewVariables[AddedIndex].VarType = PinType;
		Blueprint->NewVariables[AddedIndex].DefaultValue = Var.DefaultValue;
		UE_LOG(LogAnimBPImporter, Verbose, TEXT("Added variable: %s"), *Var.Name);
	}
	
	return true;
}

bool FAnimBPImporter::ApplyMapVariableDefaults(
	UAnimBlueprint* Blueprint,
	const TArray<FVariableDef>& Variables,
	FString& OutError)
{
	const bool bHasMapEntries = Variables.ContainsByPredicate([](const FVariableDef& Variable)
	{
		return Variable.ContainerType.Equals(TEXT("map"), ESearchCase::IgnoreCase)
			&& !Variable.MapEntries.IsEmpty();
	});
	if (!bHasMapEntries)
	{
		return true;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint,
		EBlueprintCompileOptions::RegenerateSkeletonOnly
		| EBlueprintCompileOptions::SkipGarbageCollection
		| EBlueprintCompileOptions::SkipSave);

	for (const FVariableDef& Variable : Variables)
	{
		if (!Variable.ContainerType.Equals(TEXT("map"), ESearchCase::IgnoreCase)
			|| Variable.MapEntries.IsEmpty())
		{
			continue;
		}

		FString DefaultText;
		if (!FAnimLangVariableCodec::BuildMapDefaultText(*Blueprint, Variable, DefaultText, OutError))
		{
			return false;
		}
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(
			Blueprint, FName(*Variable.Name));
		if (VariableIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("map variable '%s' could not be found after skeleton compile"), *Variable.Name);
			return false;
		}
		Blueprint->NewVariables[VariableIndex].DefaultValue = MoveTemp(DefaultText);
	}
	return true;
}

bool FAnimBPImporter::BuildGeneratedVars(UAnimBlueprint* Blueprint, const TArray<FHelperGraphDef>& Helpers)
{
	if (!Blueprint || Helpers.Num() == 0) return true;

	for (const FHelperGraphDef& Helper : Helpers)
	{
		const FString GeneratedVarName = GetResolvedGeneratedVarName(Helper);
		if (GeneratedVarName.IsEmpty())
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:GeneratedVar] Helper '%s' has no resolvable generated variable name"), *Helper.Id);
			continue;
		}

		bool bExists = false;
		for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
		{
			if (ExistingVar.VarName == FName(*GeneratedVarName))
			{
				bExists = true;
				break;
			}
		}
		if (bExists)
		{
			continue;
		}

		FVariableDef GeneratedVar;
		GeneratedVar.Name = GeneratedVarName;
		GeneratedVar.Type = Helper.GeneratedType;
		GeneratedVar.DefaultValue = (Helper.GeneratedType == EPinType::Bool) ? TEXT("false") : TEXT("0.0");
		TArray<FVariableDef> SingleVar;
		SingleVar.Add(GeneratedVar);
		if (!BuildVariables(Blueprint, SingleVar))
		{
			return false;
		}
	}

	return true;
}

bool FAnimBPImporter::BuildHelperGraphs(UAnimBlueprint* Blueprint, const TArray<FHelperGraphDef>& Helpers)
{
	if (!Blueprint || Helpers.Num() == 0) return true;

	bool bAllSucceeded = true;
	for (const FHelperGraphDef& Helper : Helpers)
	{
		const FString GraphName = GetResolvedHelperGraphName(Helper);
		const FString HelperDSL = Helper.DSL.TrimStartAndEnd();
		if (GraphName.IsEmpty() || HelperDSL.IsEmpty())
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:HelperGraph] Helper '%s' is missing graph-name or dsl; graph creation skipped"), *Helper.Id);
			continue;
		}

		UEdGraph* FunctionGraph = nullptr;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == FName(*GraphName))
			{
				FunctionGraph = Graph;
				break;
			}
		}

		if (!FunctionGraph)
		{
			FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint,
				FName(*GraphName),
				UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			if (!FunctionGraph)
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:HelperGraphCreate] Failed to create helper graph '%s'"), *GraphName);
				bAllSucceeded = false;
				continue;
			}
			FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, FunctionGraph, true, nullptr);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}

		FBlueprintLispConverter::FImportOptions LispOpts;
		LispOpts.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
		LispOpts.bAutoLayout = false;
		LispOpts.bCompile = false;
		FBlueprintLispResult LispResult = FBlueprintLispConverter::ImportGraph(FunctionGraph, HelperDSL, LispOpts);
		if (!LispResult.bSuccess)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:HelperGraphImport] Helper '%s' graph '%s' import failed: %s"),
				*Helper.Id, *GraphName, *LispResult.Error);
			bAllSucceeded = false;
		}
	}

	return bAllSucceeded;
}

bool FAnimBPImporter::BuildAnimationLayers(UAnimBlueprint* Blueprint, const TArray<FAnimationLayerDef>& Layers,
	const TMap<FString, FHelperGraphDef>* HelperGraphs)
{
	if (!Blueprint)
	{
		return false;
	}

	for (const FAnimationLayerDef& Layer : Layers)
	{
		if (Layer.GraphName.TrimStartAndEnd().IsEmpty())
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:AnimationLayer] Layer is missing graph-name"));
			return false;
		}

		UEdGraph* LayerGraph = nullptr;
		for (FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			const bool bMatchingInterface = Layer.InterfaceClassPath.IsEmpty()
				|| (InterfaceDesc.Interface && InterfaceDesc.Interface->GetPathName() == Layer.InterfaceClassPath);
			if (!bMatchingInterface) continue;
			for (UEdGraph* Graph : InterfaceDesc.Graphs)
			{
				if (Graph && Graph->GetName() == Layer.GraphName)
				{
					LayerGraph = Graph;
					break;
				}
			}
			if (LayerGraph) break;
		}

		if (!LayerGraph)
		{
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetName() == Layer.GraphName
					&& Graph->GetSchema() && Graph->GetSchema()->IsA<UAnimationGraphSchema>())
				{
					LayerGraph = Graph;
					break;
				}
			}
		}

		if (!LayerGraph)
		{
			UClass* SchemaClass = UAnimationGraphSchema::StaticClass();
			if (!Layer.SchemaClassPath.IsEmpty())
			{
				if (UClass* RequestedSchema = LoadObject<UClass>(nullptr, *Layer.SchemaClassPath);
					RequestedSchema && RequestedSchema->IsChildOf(UAnimationGraphSchema::StaticClass()))
				{
					SchemaClass = RequestedSchema;
				}
			}
			LayerGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(*Layer.GraphName), UAnimationGraph::StaticClass(), SchemaClass);
			if (!LayerGraph)
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:AnimationLayer] Failed to create graph '%s'"), *Layer.GraphName);
				return false;
			}
			if (Layer.InterfaceClassPath.IsEmpty())
			{
				FBlueprintEditorUtils::AddDomainSpecificGraph(Blueprint, LayerGraph);
			}
			else
			{
				UClass* InterfaceClass = LoadObject<UClass>(nullptr, *Layer.InterfaceClassPath);
				if (!InterfaceClass)
				{
					UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:AnimationLayer] Interface class not found for '%s': %s"),
						*Layer.GraphName, *Layer.InterfaceClassPath);
					return false;
				}
				FBlueprintEditorUtils::AddInterfaceGraph(Blueprint, LayerGraph, InterfaceClass);
			}
		}

		if (!Layer.GraphGuid.IsEmpty())
		{
			FGuid ParsedGuid;
			if (!FGuid::Parse(Layer.GraphGuid, ParsedGuid) || !ParsedGuid.IsValid())
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:AnimationLayer] Invalid graph-guid '%s' for '%s'"),
					*Layer.GraphGuid, *Layer.GraphName);
				return false;
			}
			LayerGraph->GraphGuid = ParsedGuid;
		}

		UAnimGraphNode_Root* RootNode = nullptr;
		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* Node : LayerGraph->Nodes)
		{
			if (!RootNode) RootNode = Cast<UAnimGraphNode_Root>(Node);
			if (!Node || Node->IsA<UAnimGraphNode_Root>()) continue;
			NodesToRemove.Add(Node);
		}
		for (UEdGraphNode* Node : NodesToRemove)
		{
			LayerGraph->RemoveNode(Node);
		}
		if (!RootNode)
		{
			RootNode = NewObject<UAnimGraphNode_Root>(LayerGraph);
			RootNode->CreateNewGuid();
			RootNode->PostPlacedNewNode();
			RootNode->AllocateDefaultPins();
			LayerGraph->AddNode(RootNode, false, false);
		}
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin) Pin->BreakAllPinLinks();
		}

		TMap<FString, UAnimGraphNode_SaveCachedPose*> DefineNodes;
		for (const FCachedPoseDef& Def : Layer.Defines)
		{
			UAnimGraphNode_SaveCachedPose* SaveNode = NewObject<UAnimGraphNode_SaveCachedPose>(LayerGraph);
			SaveNode->CreateNewGuid();
			SaveNode->PostPlacedNewNode();
			SaveNode->AllocateDefaultPins();
			SaveNode->CacheName = Def.Name;
			LayerGraph->AddNode(SaveNode, false, false);
			DefineNodes.Add(Def.GetIdentifier(), SaveNode);
		}
		for (const FCachedPoseDef& Def : Layer.Defines)
		{
			UAnimGraphNode_SaveCachedPose* SaveNode = DefineNodes.FindRef(Def.GetIdentifier());
			if (!SaveNode || !Def.Body.IsValid()) continue;
			UAnimGraphNode_Base* BodyNode = BuildAnimNode(Def.Body, LayerGraph, &DefineNodes, HelperGraphs);
			if (!BodyNode && Def.Body->NodeType != TEXT("identity-pose")) return false;
			if (!BodyNode) continue;
			UEdGraphPin* BodyOutput = FindOutputPosePin(BodyNode);
			for (UEdGraphPin* Pin : SaveNode->Pins)
			{
				if (BodyOutput && Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					ConnectPins(BodyOutput, Pin);
					break;
				}
			}
		}

		if (Layer.RootNode.IsValid())
		{
			UAnimGraphNode_Base* RootTree = BuildAnimNode(Layer.RootNode, LayerGraph, &DefineNodes, HelperGraphs);
			if (!RootTree && Layer.RootNode->NodeType != TEXT("identity-pose")) return false;
			if (RootTree)
			{
				UEdGraphPin* TreeOutput = FindOutputPosePin(RootTree);
				for (UEdGraphPin* Pin : RootNode->Pins)
				{
					if (TreeOutput && Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						ConnectPins(TreeOutput, Pin);
						break;
					}
				}
			}
		}
		UE_LOG(LogAnimBPImporter, Log, TEXT("Restored animation layer: %s (%s)"), *Layer.GraphName, *Layer.InterfaceClassPath);
	}
	return true;
}

bool FAnimBPImporter::BuildLogicGraphs(UAnimBlueprint* Blueprint, const TArray<FLogicGraphDef>& LogicGraphs, bool bReplaceExistingSet)
{
	if (!Blueprint)
	{
		return false;
	}

	TSet<FName> DesiredEventGraphs;
	TSet<FName> DesiredFunctionGraphs;
	for (const FLogicGraphDef& LogicGraph : LogicGraphs)
	{
		const bool bEvent = LogicGraph.Role.Equals(TEXT("event"), ESearchCase::IgnoreCase)
			&& LogicGraph.Kind.Equals(TEXT("ubergraph"), ESearchCase::IgnoreCase);
		const bool bFunction = LogicGraph.Role.Equals(TEXT("function"), ESearchCase::IgnoreCase)
			&& LogicGraph.Kind.Equals(TEXT("function"), ESearchCase::IgnoreCase);
		if (!bEvent && !bFunction)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:LogicGraph] Graph '%s' has inconsistent role '%s' and kind '%s'"),
				*LogicGraph.GraphName, *LogicGraph.Role, *LogicGraph.Kind);
			return false;
		}
		(bEvent ? DesiredEventGraphs : DesiredFunctionGraphs).Add(FName(*LogicGraph.GraphName));
	}

	for (UEdGraph* Ubergraph : Blueprint->UbergraphPages)
	{
		if (!Ubergraph) continue;
		TArray<UEdGraphNode*> DuplicateEvents;
		for (UEdGraphNode* Node : Ubergraph->Nodes)
		{
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
				EventNode && DesiredFunctionGraphs.Contains(EventNode->EventReference.GetMemberName()))
			{
				DuplicateEvents.Add(EventNode);
			}
		}
		for (UEdGraphNode* DuplicateEvent : DuplicateEvents) Ubergraph->RemoveNode(DuplicateEvent);
	}

	if (bReplaceExistingSet)
	{
		TArray<UEdGraph*> StaleGraphs;
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph && !DesiredEventGraphs.Contains(Graph->GetFName())) StaleGraphs.Add(Graph);
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (!Graph || (Graph->GetSchema() && Graph->GetSchema()->IsA<UAnimationGraphSchema>())
				|| Graph->GetName().StartsWith(TEXT("__ABP2FP_HG_"))) continue;
			if (!DesiredFunctionGraphs.Contains(Graph->GetFName())) StaleGraphs.Add(Graph);
		}
		for (UEdGraph* Graph : StaleGraphs)
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::MarkTransient);
		}
	}

	auto FindOrCreateLogicGraph = [Blueprint](const FLogicGraphDef& LogicGraph) -> UEdGraph*
	{
		const FString GraphName = LogicGraph.GraphName.TrimStartAndEnd();
		const bool bEventGraph = LogicGraph.Role.Equals(TEXT("event"), ESearchCase::IgnoreCase);
		const TArray<TObjectPtr<UEdGraph>>& ExistingGraphs = bEventGraph ? Blueprint->UbergraphPages : Blueprint->FunctionGraphs;
		for (UEdGraph* Graph : ExistingGraphs)
		{
			if (Graph && Graph->GetFName() == FName(*GraphName)) return Graph;
		}

		UClass* SchemaClass = UEdGraphSchema_K2::StaticClass();
		if (!LogicGraph.SchemaClassPath.IsEmpty())
		{
			if (UClass* RequestedSchema = LoadObject<UClass>(nullptr, *LogicGraph.SchemaClassPath);
				RequestedSchema && RequestedSchema->IsChildOf(UEdGraphSchema::StaticClass()))
			{
				SchemaClass = RequestedSchema;
			}
		}
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(*GraphName), UEdGraph::StaticClass(), SchemaClass);
		if (!Graph) return nullptr;
		if (bEventGraph) FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
		else FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, nullptr);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return Graph;
	};

	// All function graph identities must exist before any body is imported. Then import
	// signatures in a separate pass so cross-function calls resolve against the target BP.
	for (const FLogicGraphDef& LogicGraph : LogicGraphs)
	{
		if (!FindOrCreateLogicGraph(LogicGraph))
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:LogicGraph] Failed to create graph '%s'"), *LogicGraph.GraphName);
			return false;
		}
	}
	for (const FLogicGraphDef& LogicGraph : LogicGraphs)
	{
		if (LogicGraph.Role.Equals(TEXT("event"), ESearchCase::IgnoreCase)) continue;
		UEdGraph* FunctionGraph = FindOrCreateLogicGraph(LogicGraph);
		FBlueprintLispConverter::FImportOptions SignatureOptions;
		SignatureOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
		SignatureOptions.bAutoLayout = false;
		SignatureOptions.bCompile = false;
		SignatureOptions.bFailOnUnsupportedForm = true;
		SignatureOptions.bSignatureOnly = true;
		const FBlueprintLispResult SignatureResult = FBlueprintLispConverter::ImportGraph(
			FunctionGraph, LogicGraph.DSL.TrimStartAndEnd(), SignatureOptions);
		if (!SignatureResult.bSuccess)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:LogicGraph] Failed to import signature for '%s': %s"),
				*LogicGraph.GraphName, *SignatureResult.Error);
			return false;
		}
	}

	for (const FLogicGraphDef& LogicGraph : LogicGraphs)
	{
		const FString GraphName = LogicGraph.GraphName.TrimStartAndEnd();
		const FString LispCode = LogicGraph.DSL.TrimStartAndEnd();
		if (GraphName.IsEmpty() || LispCode.IsEmpty())
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:LogicGraph] Logic graph is missing graph-name or dsl"));
			return false;
		}
		if (GraphName.Contains(TEXT("AnimGraph")) || GraphName.StartsWith(TEXT("__ABP2FP_HG_")))
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:LogicGraph] Refusing reserved graph '%s' in logic-graphs"), *GraphName);
			return false;
		}

		const bool bEventGraph = LogicGraph.Role.Equals(TEXT("event"), ESearchCase::IgnoreCase);
		UEdGraph* TargetGraph = nullptr;
		const TArray<TObjectPtr<UEdGraph>>& ExistingGraphs = bEventGraph ? Blueprint->UbergraphPages : Blueprint->FunctionGraphs;
		for (UEdGraph* Graph : ExistingGraphs)
		{
			if (Graph && Graph->GetFName() == FName(*GraphName))
			{
				TargetGraph = Graph;
				break;
			}
		}

		if (!TargetGraph)
		{
			UClass* SchemaClass = UEdGraphSchema_K2::StaticClass();
			if (!LogicGraph.SchemaClassPath.IsEmpty())
			{
				if (UClass* RequestedSchema = LoadObject<UClass>(nullptr, *LogicGraph.SchemaClassPath);
					RequestedSchema && RequestedSchema->IsChildOf(UEdGraphSchema::StaticClass()))
				{
					SchemaClass = RequestedSchema;
				}
			}
			TargetGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(*GraphName), UEdGraph::StaticClass(), SchemaClass);
			if (!TargetGraph)
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:LogicGraph] Failed to create graph '%s'"), *GraphName);
				return false;
			}
			if (bEventGraph)
			{
				FBlueprintEditorUtils::AddUbergraphPage(Blueprint, TargetGraph);
			}
			else
			{
				FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, TargetGraph, true, nullptr);
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}

		FBlueprintLispConverter::FImportOptions Options;
		Options.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
		Options.bAutoLayout = false;
		Options.bCompile = false;
		Options.bFailOnUnsupportedForm = true;
		const FBlueprintLispResult ImportResult = bEventGraph
			? FBlueprintLispConverter::Import(Blueprint, GraphName, LispCode, Options)
			: FBlueprintLispConverter::ImportGraph(TargetGraph, LispCode, Options);
		if (!ImportResult.bSuccess)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:LogicGraph] Failed to import '%s': %s"),
				*GraphName, *ImportResult.Error);
			return false;
		}
	}

	// call-macro stores a direct graph reference. BlueprintLisp may resolve a private macro
	// from the source Blueprint because AnimLang intentionally serializes only event/function
	// logic graphs. Such a cross-package private reference compiles transiently but cannot be
	// saved, so internalize it in the rebuilt Blueprint and bind every instance to the clone.
	TMap<TObjectPtr<UEdGraph>, TObjectPtr<UEdGraph>> InternalizedMacros;
	bool bFoundNewPrivateMacro = false;
	do
	{
		bFoundNewPrivateMacro = false;
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(GraphNode);
				UEdGraph* SourceMacro = MacroNode ? MacroNode->GetMacroGraph() : nullptr;
				if (!SourceMacro || SourceMacro->GetTypedOuter<UBlueprint>() == Blueprint
					|| SourceMacro->HasAnyFlags(RF_Public))
				{
					continue;
				}

				UEdGraph* TargetMacro = InternalizedMacros.FindRef(SourceMacro);
				if (!TargetMacro)
				{
					if (Blueprint->MacroGraphs.ContainsByPredicate([SourceMacro](const UEdGraph* Candidate)
					{
						return Candidate && Candidate->GetFName() == SourceMacro->GetFName();
					}))
					{
						UE_LOG(LogAnimBPImporter, Error,
							TEXT("[UNSUPPORTED:LogicGraph] Private macro name '%s' resolves to multiple source graphs"),
							*SourceMacro->GetName());
						return false;
					}
					TargetMacro = FEdGraphUtilities::CloneGraph(SourceMacro, Blueprint);
					if (!TargetMacro)
					{
						UE_LOG(LogAnimBPImporter, Error,
							TEXT("[UNSUPPORTED:LogicGraph] Failed to internalize private macro '%s'"),
							*SourceMacro->GetPathName());
						return false;
					}
					TargetMacro->ClearFlags(RF_Transient);
					FBlueprintEditorUtils::RenameGraph(TargetMacro, SourceMacro->GetName());
					if (TargetMacro->GetFName() != SourceMacro->GetFName())
					{
						UE_LOG(LogAnimBPImporter, Error,
							TEXT("[UNSUPPORTED:LogicGraph] Internalized macro '%s' was renamed to '%s'"),
							*SourceMacro->GetName(), *TargetMacro->GetName());
						return false;
					}
					Blueprint->MacroGraphs.Add(TargetMacro);
					bFoundNewPrivateMacro = true;
				}
				InternalizedMacros.Add(SourceMacro, TargetMacro);
				MacroNode->SetMacroGraph(TargetMacro);
			}
		}
	}
	while (bFoundNewPrivateMacro);
	if (!InternalizedMacros.IsEmpty())
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	return true;
}

bool FAnimBPImporter::ConnectPropertyBinding(UAnimBlueprint* Blueprint, UEdGraph* Graph, UAnimGraphNode_Base* Node,
	const FString& KebabKey, const FString& Value, const TMap<FString, FHelperGraphDef>* HelperGraphs)
{
	if (!Blueprint || !Graph || !Node)
	{
		return false;
	}

	FString FormName;
	FString Argument;
	if (!TryParseBindingForm(Value, FormName, Argument))
	{
		return false;
	}

	const FString CleanArgument = StripQuotes(Argument);
	FString BoundVariableName;
	if (FormName == TEXT("bind-var"))
	{
		BoundVariableName = CleanArgument;
	}
	else if (FormName == TEXT("subgraph-ref"))
	{
		if (!HelperGraphs)
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:HelperLookup] Node '%s' property ':%s' references helper '%s' but no helper lookup map is available"),
				*Node->GetName(), *KebabKey, *CleanArgument);
			return true;
		}

		const FHelperGraphDef* Helper = HelperGraphs->Find(CleanArgument);
		if (!Helper)
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:HelperLookup] Node '%s' property ':%s' references unknown helper '%s'"),
				*Node->GetName(), *KebabKey, *CleanArgument);
			return true;
		}

		BoundVariableName = GetResolvedGeneratedVarName(*Helper);
	}
	else if (FormName == TEXT("bind-path"))
	{
		TArray<FString> PropertyPath;
		EAnimGraphNodePropertyBindingType BindingType = EAnimGraphNodePropertyBindingType::Property;
		FName ContextId = NAME_None;
		int32 ArrayIndex = INDEX_NONE;
		bool bOnlyUpdateWhenActive = false;
		bool bHasExplicitType = false;
		if (!ParseBindPathForm(Value, PropertyPath, BindingType, ContextId, ArrayIndex,
			bOnlyUpdateWhenActive, bHasExplicitType))
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:BindPath] Node '%s' property ':%s' = %s — could not parse bind-path argument"),
				*Node->GetName(), *KebabKey, *Value);
			return true;
		}
		if (!bHasExplicitType && Blueprint->SkeletonGeneratedClass
			&& Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*PropertyPath.Last())))
		{
			BindingType = EAnimGraphNodePropertyBindingType::Function;
		}

		const FString PropertyName = ResolveBindingPropertyName(Node, KebabKey);
		if (PropertyName.IsEmpty())
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:BindPath] Node '%s' property ':%s' = %s — could not resolve target property name"),
				*Node->GetName(), *KebabKey, *Value);
			return true;
		}

		Node->Modify();
		if (UObject* BindingObject = reinterpret_cast<UObject*>(Node->GetMutableBinding()))
		{
			BindingObject->Modify();
		}
		const int32 OptionalPinIndex = Node->ShowPinForProperties.IndexOfByPredicate(
			[&PropertyName](const FOptionalPinFromProperty& OptionalPin)
			{
				return OptionalPin.PropertyName == FName(*PropertyName);
			});
		if (OptionalPinIndex != INDEX_NONE)
		{
			// UE's binding UI exposes the destination pin before adding the binding. Without
			// this, the compiler folds the bound field into an anonymous constant property.
			Node->SetPinVisibility(true, OptionalPinIndex);
		}

		TMap<FName, FAnimGraphNodePropertyBinding>* PropertyBindings = GetMutablePropertyBindingMap(Node);
		if (!PropertyBindings)
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:BindPath] Node '%s' property ':%s' = %s — property binding storage is unavailable"),
				*Node->GetName(), *KebabKey, *Value);
			return true;
		}

		FAnimGraphNodePropertyBinding PropertyBinding;
		PropertyBinding.PropertyName = FName(*PropertyName);
		PropertyBinding.PropertyPath = PropertyPath;
		PropertyBinding.PathAsText = FText::FromString(JoinBindingPath(PropertyPath));
		PropertyBinding.Type = BindingType;
		PropertyBinding.bIsBound = true;
		PropertyBinding.ArrayIndex = ArrayIndex;
		PropertyBinding.ContextId = ContextId;
		PropertyBinding.bOnlyUpdateWhenActive = bOnlyUpdateWhenActive;
		PropertyBindings->Add(FName(*PropertyName), PropertyBinding);

		Node->ReconstructNode();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		return true;
	}
	else
	{
		return false;
	}

	if (BoundVariableName.IsEmpty())
	{
		UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:PropertyBinding] Node '%s' property ':%s' = %s — resolved variable name is empty"),
			*Node->GetName(), *KebabKey, *Value);
		return true;
	}

	bool bVariableExists = false;
	for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
	{
		if (ExistingVar.VarName == FName(*BoundVariableName))
		{
			bVariableExists = true;
			break;
		}
	}
	if (!bVariableExists)
	{
		UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:PropertyBinding] Node '%s' property ':%s' = %s — blueprint variable '%s' does not exist"),
			*Node->GetName(), *KebabKey, *Value, *BoundVariableName);
		return true;
	}

	UEdGraphPin* TargetPin = FindInputValuePin(Node, KebabKey);
	if (!TargetPin)
	{
		const FString PropertyName = ResolveBindingPropertyName(Node, KebabKey);
		TMap<FName, FAnimGraphNodePropertyBinding>* PropertyBindings = GetMutablePropertyBindingMap(Node);
		if (!PropertyName.IsEmpty() && PropertyBindings)
		{
			Node->Modify();
			if (UObject* BindingObject = reinterpret_cast<UObject*>(Node->GetMutableBinding()))
			{
				BindingObject->Modify();
			}

			FAnimGraphNodePropertyBinding PropertyBinding;
			PropertyBinding.PropertyName = FName(*PropertyName);
			PropertyBinding.PropertyPath.Reset();
			PropertyBinding.PropertyPath.Add(BoundVariableName);
			PropertyBinding.PathAsText = FText::FromString(BoundVariableName);
			PropertyBinding.Type = EAnimGraphNodePropertyBindingType::Property;
			PropertyBinding.bIsBound = true;
			PropertyBinding.ArrayIndex = INDEX_NONE;
			PropertyBinding.ContextId = NAME_None;
			PropertyBindings->Add(FName(*PropertyName), PropertyBinding);
			Node->ReconstructNode();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			UE_LOG(LogAnimBPImporter, Log, TEXT("Restored property binding via binding map: %s.%s <- %s"),
				*Node->GetName(), *PropertyName, *BoundVariableName);
			return true;
		}

		UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:PropertyBinding] Node '%s' property ':%s' = %s — input pin not found for binding restore"),
			*Node->GetName(), *KebabKey, *Value);
		return true;
	}

	UK2Node_VariableGet* VarGetNode = CreateMemberVariableGetNode(Blueprint, Graph, BoundVariableName, Node);
	if (!VarGetNode)
	{
		UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:PropertyBinding] Node '%s' property ':%s' = %s — failed to create VariableGet node for '%s'"),
			*Node->GetName(), *KebabKey, *Value, *BoundVariableName);
		return true;
	}

	UEdGraphPin* OutputPin = FindFirstDataOutputPin(VarGetNode);
	if (!OutputPin)
	{
		UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:PropertyBinding] VariableGet '%s' has no data output pin"), *BoundVariableName);
		return true;
	}

	ConnectPins(OutputPin, TargetPin);
	return true;
}

// ========== Node Building ==========

UAnimGraphNode_Base* FAnimBPImporter::BuildAnimNode(const TSharedPtr<FAnimNodeAST>& NodeAST, UEdGraph* Graph,
	const TMap<FString, UAnimGraphNode_SaveCachedPose*>* DefineNodes,
	const TMap<FString, FHelperGraphDef>* HelperGraphs)
{
	if (!NodeAST.IsValid() || !Graph)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("[INTERNAL] BuildAnimNode called with null NodeAST or null Graph"));
		return nullptr;
	}

	const FString& NodeType = NodeAST->NodeType;

	auto RestoreNodeGuid = [&NodeAST](UEdGraphNode* Node) -> bool
	{
		if (NodeAST->NodeId.IsEmpty())
		{
			Node->CreateNewGuid();
			return true;
		}

		FGuid ParsedGuid;
		if (!FGuid::Parse(NodeAST->NodeId, ParsedGuid) || !ParsedGuid.IsValid())
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[INVALID:NodeId] '%s' is not a complete valid Unreal GUID"), *NodeAST->NodeId);
			return false;
		}
		Node->NodeGuid = ParsedGuid;
		return true;
	};

	if (NodeType == TEXT("cached-pose-ref"))
	{
		const FString* NameProperty = NodeAST->Properties.Find(TEXT("name"));
		if (!NameProperty)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:CachedPoseRef] cached-pose-ref requires a :name property"));
			return nullptr;
		}

		const FString CacheName = StripQuotes(*NameProperty);
		FString NormalizedName = CacheName;
		NormalizedName.ReplaceInline(TEXT(" "), TEXT("-"));
		UAnimGraphNode_SaveCachedPose* SaveNode = DefineNodes ? DefineNodes->FindRef(NormalizedName) : nullptr;
		if (!SaveNode && DefineNodes)
		{
			SaveNode = DefineNodes->FindRef(CacheName);
		}
		if (!SaveNode)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:CachedPoseRef] No define named '%s' exists"), *CacheName);
			return nullptr;
		}

		UAnimGraphNode_UseCachedPose* UseNode = NewObject<UAnimGraphNode_UseCachedPose>(Graph);
		if (!RestoreNodeGuid(UseNode))
		{
			return nullptr;
		}
		UseNode->PostPlacedNewNode();
		UseNode->AllocateDefaultPins();
		UseNode->SaveCachedPoseNode = SaveNode;
		UseNode->Node.CachePoseName = FName(*SaveNode->CacheName);
		Graph->AddNode(UseNode, false, false);
		return UseNode;
	}

	const bool bAllowedLegacyRigCoverage = NodeAST->Coverage == EAnimNodeCoverage::Lossy
		&& NodeType == TEXT("control-rig")
		&& NodeAST->RigBinding.IsSet()
		&& GActiveAnimImportContext
		&& GActiveAnimImportContext->bAllowLegacyRigFallback
		&& GActiveAnimImportContext->LegacyExternalRigPaths.Contains(
			NodeAST->RigBinding->RigModule.AssetPath);
	if ((NodeAST->Coverage == EAnimNodeCoverage::Lossy && !bAllowedLegacyRigCoverage)
		|| NodeAST->Coverage == EAnimNodeCoverage::Unsupported)
	{
		UE_LOG(LogAnimBPImporter, Warning, TEXT("[UNSUPPORTED:NodeCoverage] Node '%s' has non-importable coverage '%s'"),
			*NodeType, NodeAST->Coverage == EAnimNodeCoverage::Lossy ? TEXT("lossy") : TEXT("unsupported"));
		return nullptr;
	}

	// Handle blend-list: the :class property contains the actual UE class name
	FString EffectiveNodeType = NodeType;
	if (NodeType == TEXT("blend-list"))
	{
		const FString* ClassProp = NodeAST->Properties.Find(TEXT("class"));
		if (ClassProp)
		{
			EffectiveNodeType = StripQuotes(*ClassProp);
			// ClassProp might be "AnimGraphNode_BlendListByEnum" — use directly
		}
	}

	UClass* NodeClass = nullptr;
	if (!NodeAST->NodeClassPath.IsEmpty())
	{
		NodeClass = LoadClass<UAnimGraphNode_Base>(nullptr, *NodeAST->NodeClassPath);
		if (!NodeClass || !NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:NodeClass] Could not load concrete animation node class '%s' for '%s'"),
				*NodeAST->NodeClassPath, *NodeType);
			return nullptr;
		}
	}
	else
	{
		NodeClass = FindAnimNodeClass(EffectiveNodeType);
	}
	if (!NodeClass)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:NodeClass] Could not resolve node type '%s'; cached poses require explicit cached-pose-ref syntax"), *NodeType);
		return nullptr;
	}
	
	// Create the node
	UAnimGraphNode_Base* NewNode = NewObject<UAnimGraphNode_Base>(Graph, NodeClass);
	if (!NewNode)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("Failed to create node of class '%s'"), *NodeClass->GetName());
		return nullptr;
	}
	
	if (!RestoreNodeGuid(NewNode))
	{
		return nullptr;
	}
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	
	Graph->AddNode(NewNode, false, false);
	
	// ---- Handle special node types ----
	
	// LinkedAnimLayer: set layer name and reconstruct to generate pins
	if (UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(NewNode))
	{
		const FString* LayerNameStr = NodeAST->Properties.Find(TEXT("layer"));
		if (LayerNameStr)
		{
			FName LayerFName(*StripQuotes(*LayerNameStr));
			
			// If an interface path is specified, set it on the runtime node
			const FString* InterfacePath = NodeAST->Properties.Find(TEXT("interface"));
			if (InterfacePath)
			{
				FString CleanPath = StripQuotes(*InterfacePath);
				UClass* InterfaceClass = LoadObject<UClass>(nullptr, *CleanPath);
				if (InterfaceClass)
				{
					LayerNode->Node.Interface = InterfaceClass;
				}
				else
				{
					UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not load AnimLayerInterface: %s"), *CleanPath);
				}
			}

			// Validate: resolve the authored layer against the blueprint's implemented interface graphs first.
			// For linked anim layers, matching UEdGraph names / InterfaceGuid is more reliable than probing UFunction names.
			bool bLayerValid = true;
			UAnimBlueprint* BP = Cast<UAnimBlueprint>(FBlueprintEditorUtils::FindBlueprintForGraph(Graph));
			if (BP)
			{
				for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
				{
					UClass* CandidateInterface = Desc.Interface;
					if (!CandidateInterface)
					{
						continue;
					}
					if (LayerNode->Node.Interface && LayerNode->Node.Interface != CandidateInterface)
					{
						continue;
					}
					for (UEdGraph* InterfaceGraph : Desc.Graphs)
					{
						if (InterfaceGraph && InterfaceGraph->GetFName() == LayerFName)
						{
							LayerNode->Node.Interface = CandidateInterface;
							LayerNode->InterfaceGuid = InterfaceGraph->InterfaceGuid;
							UE_LOG(LogAnimBPImporter, Log, TEXT("LinkedAnimLayer: resolved interface '%s' (guid=%s) for layer '%s'"),
								*CandidateInterface->GetPathName(), *LayerNode->InterfaceGuid.ToString(), *LayerFName.ToString());
							break;
						}
					}
					if (LayerNode->Node.Interface == CandidateInterface && LayerNode->InterfaceGuid.IsValid())
					{
						break;
					}
				}
			}
			if (!LayerNode->Node.Interface)
			{
				// Self-layer: look it up in SkeletonGeneratedClass
				if (BP && BP->SkeletonGeneratedClass)
				{
					IAnimClassInterface* AnimClass = IAnimClassInterface::GetFromClass(BP->SkeletonGeneratedClass);
					if (AnimClass)
					{
						bool bFound = false;
						for (const FAnimBlueprintFunction& Fn : AnimClass->GetAnimBlueprintFunctions())
						{
							if (Fn.Name == LayerFName)
							{
								bFound = true;
								break;
							}
						}
						if (!bFound)
						{
							UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:LinkedAnimLayer] Layer '%s' not found in SkeletonGeneratedClass at import time — keep node and fall back to manual pin reconstruction"),
								*LayerFName.ToString());
						}
						else if (!LayerNode->InterfaceGuid.IsValid())
						{
							LayerNode->InterfaceGuid.Invalidate();
						}
					}
				}
			}

			if (bLayerValid)
			{
				// Keep Node.Layer and FunctionReference consistent (can't call SetLayerName due MinimalAPI linking)
				LayerNode->Node.Layer = LayerFName;
				if (FStructProperty* FunctionRefProp = FindFProperty<FStructProperty>(LayerNode->GetClass(), TEXT("FunctionReference")))
				{
					if (FMemberReference* FunctionRef = FunctionRefProp->ContainerPtrToValuePtr<FMemberReference>(LayerNode))
					{
						if (UClass* TargetClass = LayerNode->GetTargetClass())
						{
							FGuid FunctionGuid;
							FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName(FBlueprintEditorUtils::GetMostUpToDateClass(TargetClass), LayerFName, FunctionGuid);
							FunctionRef->SetExternalMember(LayerFName, TargetClass, FunctionGuid);
						}
						else
						{
							FunctionRef->SetSelfMember(LayerFName);
						}
					}
				}
				
				// Reconstruct node to regenerate pins from the layer definition.
				static_cast<UEdGraphNode*>(LayerNode)->ReconstructNode();
				
				UE_LOG(LogAnimBPImporter, Log, TEXT("LinkedAnimLayer: set layer='%s', pins=%d"),
					*LayerFName.ToString(), LayerNode->Pins.Num());
			}
			else
			{
				// Remove the invalid node to keep the graph clean
				Graph->RemoveNode(LayerNode);
				return nullptr;
			}
		}

		// Ensure pose input pins exist for all DSL children.
		// If ReconstructNode did not produce them (e.g., new blueprint without SkeletonGeneratedClass),
		// manually create the necessary pose pins from the DSL child node names.
		for (const auto& Child : NodeAST->Children)
		{
			const FString& ChildPinName = Child.PinName;
			FString CamelPinName = KebabToCamel(ChildPinName);

			// Check if this pose pin already exists
			bool bPinExists = false;
			for (UEdGraphPin* Pin : LayerNode->Pins)
			{
				if (Pin && Pin->PinName.ToString() == CamelPinName && 
					Pin->Direction == EGPD_Input &&
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					bPinExists = true;
					break;
				}
			}
			
			if (!bPinExists)
			{
				// Create a pose input pin manually
				FEdGraphPinType PoseType;
				PoseType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PoseType.PinSubCategoryObject = FPoseLink::StaticStruct();
				
				UEdGraphPin* NewPin = LayerNode->CreatePin(EGPD_Input, PoseType, FName(*CamelPinName));
				if (NewPin)
				{
					UE_LOG(LogAnimBPImporter, Log, TEXT("LinkedAnimLayer: manually created pose pin '%s'"), *CamelPinName);
				}
			}
		}
	}

	// SequencePlayer / SequenceEvaluator: set animation sequence
	// Supports both :name "ShortName" (legacy) and :sequence (asset "/Game/Path") formats
	if (UAnimGraphNode_SequencePlayer* SeqPlayer = Cast<UAnimGraphNode_SequencePlayer>(NewNode))
	{
		UAnimSequence* Seq = nullptr;
		FString SearchPath;

		// Priority 1: :sequence (asset "...") — full path
		const FString* SeqAsset = NodeAST->Properties.Find(TEXT("sequence"));
		if (SeqAsset && SeqAsset->StartsWith(TEXT("(asset ")))
		{
			SearchPath = *SeqAsset;
			SearchPath.RemoveFromStart(TEXT("(asset "));
			SearchPath.RemoveFromEnd(TEXT(")"));
			SearchPath = StripQuotes(SearchPath);
			Seq = LoadObject<UAnimSequence>(nullptr, *SearchPath);
		}
		
		// Priority 2: :name "ShortName" — search by name
		if (!Seq)
		{
			const FString* SeqName = NodeAST->Properties.Find(TEXT("name"));
			if (SeqName)
			{
				SearchPath = StripQuotes(*SeqName);
				Seq = LoadObject<UAnimSequence>(nullptr, *SearchPath);
				if (!Seq)
				{
					for (TObjectIterator<UAnimSequence> It; It; ++It)
					{
						if (It->GetName() == SearchPath)
						{
							Seq = *It;
							break;
						}
					}
				}
				if (!Seq)
				{
					FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
					IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
					
					TArray<FAssetData> AssetList;
					AssetRegistry.GetAssetsByClass(UAnimSequence::StaticClass()->GetClassPathName(), AssetList, false);
					
					for (const FAssetData& AssetData : AssetList)
					{
						if (AssetData.AssetName.ToString() == SearchPath)
						{
							Seq = Cast<UAnimSequence>(AssetData.GetAsset());
							if (Seq) break;
						}
					}
				}
			}
		}
		
		if (Seq)
		{
			SeqPlayer->Node.SetSequence(Seq);
		}
		else if (!SearchPath.IsEmpty() && SearchPath != TEXT("None"))
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:AssetLoad] Node '%s': could not find AnimSequence asset '%s'"),
				*NodeType, *SearchPath);
		}
		
		// Loop — explicitly handle both true and false
		const FString* LoopVal = NodeAST->Properties.Find(TEXT("loop"));
		if (LoopVal)
		{
			bool bLoop = LoopVal->ToBool();
			SeqPlayer->Node.SetLoopAnimation(bLoop);
		}
	}
	
	// BlendSpacePlayer: set blend space
	// Supports both :name "ShortName" (legacy) and :blend-space (asset "/Path") formats
	if (UAnimGraphNode_BlendSpacePlayer* BSPlayer = Cast<UAnimGraphNode_BlendSpacePlayer>(NewNode))
	{
		UBlendSpace* BS = nullptr;
		FString SearchPath;
		
		// Priority 1: :blend-space (asset "...") — full path
		const FString* BSAsset = NodeAST->Properties.Find(TEXT("blend-space"));
		if (BSAsset && BSAsset->StartsWith(TEXT("(asset ")))
		{
			SearchPath = *BSAsset;
			SearchPath.RemoveFromStart(TEXT("(asset "));
			SearchPath.RemoveFromEnd(TEXT(")"));
			SearchPath = StripQuotes(SearchPath);
			BS = LoadObject<UBlendSpace>(nullptr, *SearchPath);
		}
		
		// Priority 2: :name "ShortName" — search by name
		if (!BS)
		{
			const FString* BSName = NodeAST->Properties.Find(TEXT("name"));
			if (BSName)
			{
				SearchPath = StripQuotes(*BSName);
				BS = LoadObject<UBlendSpace>(nullptr, *SearchPath);
				if (!BS)
				{
					for (TObjectIterator<UBlendSpace> It; It; ++It)
					{
						if (It->GetName() == SearchPath)
						{
							BS = *It;
							break;
						}
					}
				}
				if (!BS)
				{
					FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
					IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
					
					// Force scan Engine/Content paths (Tutorial assets may not be indexed yet)
					TArray<FString> ScanPaths;
					ScanPaths.Add(TEXT("/Engine/"));
					AssetRegistry.ScanPathsSynchronous(ScanPaths, true);
					
					TArray<FAssetData> AssetList;
					// Search for UBlendSpace and all subclasses (including BlendSpace1D)
					AssetRegistry.GetAssetsByClass(UBlendSpace::StaticClass()->GetClassPathName(), AssetList, true);
					
					for (const FAssetData& AssetData : AssetList)
					{
						if (AssetData.AssetName.ToString() == SearchPath)
						{
							BS = Cast<UBlendSpace>(AssetData.GetAsset());
							if (BS) break;
						}
					}
				}
			}
		}
		
		if (BS)
		{
			BSPlayer->Node.SetBlendSpace(BS);
		}
		else if (!SearchPath.IsEmpty())
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:AssetLoad] Node '%s': could not find BlendSpace asset '%s'"),
				*NodeType, *SearchPath);
		}
		
		const FString* LoopVal2 = NodeAST->Properties.Find(TEXT("loop"));
		if (LoopVal2)
		{
			bool bLoop = LoopVal2->ToBool();
			BSPlayer->Node.SetLoop(bLoop);
		}
	}
	
	// ---- Pre-process: Expand dynamic array pins before setting properties ----
	// UE creates dynamic pins based on internal array sizes (e.g. CurveValues, BlendTime, BlendWeights).
	// We need to pre-size these arrays and ReconstructNode() so pins exist when SetNodeProperty is called.
	{
		// Collect array-indexed properties: "curve-values-0" → BaseName="curve-values", Indices={0: value}
		TMap<FString, TMap<int32, FString>> ArrayProperties;
		for (const auto& Pair : NodeAST->Properties)
		{
			// Check for trailing "-N" pattern (e.g. "curve-values-0", "blend-time-1")
			int32 LastDash = INDEX_NONE;
			Pair.Key.FindLastChar('-', LastDash);
			if (LastDash != INDEX_NONE && LastDash < Pair.Key.Len() - 1)
			{
				FString Suffix = Pair.Key.Mid(LastDash + 1);
				if (FCString::IsNumeric(*Suffix))
				{
					int32 Idx = FCString::Atoi(*Suffix);
					FString BaseName = Pair.Key.Left(LastDash);
					ArrayProperties.FindOrAdd(BaseName).Add(Idx, Pair.Value);
				}
			}
		}
		if (const FString* CanonicalBlendTime = NodeAST->Properties.Find(TEXT("blend-time")))
		{
			FString BlendTimeText = StripQuotes(*CanonicalBlendTime);
			BlendTimeText.RemoveFromStart(TEXT("("));
			BlendTimeText.RemoveFromEnd(TEXT(")"));
			TArray<FString> BlendTimes;
			BlendTimeText.ParseIntoArray(BlendTimes, TEXT(","), false);
			for (int32 Index = 0; Index < BlendTimes.Num(); ++Index)
			{
				ArrayProperties.FindOrAdd(TEXT("blend-time")).Add(Index, BlendTimes[Index].TrimStartAndEnd());
			}
		}
		
		bool bNeedReconstruct = false;
		
		// ModifyCurve: populate CurveNames and CurveValues arrays via AddCurve API
		if (UAnimGraphNode_ModifyCurve* MCNode = Cast<UAnimGraphNode_ModifyCurve>(NewNode))
		{
			const TMap<int32, FString>* CurveVals = ArrayProperties.Find(TEXT("curve-values"));
			if (CurveVals && CurveVals->Num() > 0)
			{
				// Find max index to determine array size
				int32 MaxIdx = 0;
				for (const auto& KV : *CurveVals)
				{
					MaxIdx = FMath::Max(MaxIdx, KV.Key);
				}
				int32 ArraySize = MaxIdx + 1;
				
				// Access FAnimNode_ModifyCurve via FProperty reflection and use AddCurve()
				for (TFieldIterator<FStructProperty> PropIt(MCNode->GetClass()); PropIt; ++PropIt)
				{
					FStructProperty* StructProp = *PropIt;
					if (StructProp->Struct && StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
					{
						FAnimNode_ModifyCurve* InternalNode = StructProp->ContainerPtrToValuePtr<FAnimNode_ModifyCurve>(MCNode);
						if (InternalNode)
						{
							for (int32 i = 0; i < ArraySize; i++)
							{
								float Val = 0.f;
								const FString* FoundVal = CurveVals->Find(i);
								if (FoundVal)
								{
									Val = FCString::Atof(*StripQuotes(*FoundVal));
								}
								FName CurveName = *FString::Printf(TEXT("Curve_%d"), i);
								InternalNode->AddCurve(CurveName, Val);
							}
						}
						break;
					}
				}
				bNeedReconstruct = true;
			}
		}
		
		// BlendListByEnum: restore BoundEnum asset
		if (UAnimGraphNode_BlendListByEnum* BLEnumNode = Cast<UAnimGraphNode_BlendListByEnum>(NewNode))
		{
			if (const FString* BoundEnumVal = NodeAST->Properties.Find(TEXT("bound-enum")))
			{
				FString EnumPath = *BoundEnumVal;
				if (EnumPath.StartsWith(TEXT("(asset ")))
				{
					EnumPath.RemoveFromStart(TEXT("(asset "));
					EnumPath.RemoveFromEnd(TEXT(")"));
					EnumPath = StripQuotes(EnumPath);
				}
				else
				{
					EnumPath = StripQuotes(EnumPath);
				}

				if (!EnumPath.IsEmpty())
				{
					if (UEnum* BoundEnumObj = LoadObject<UEnum>(nullptr, *EnumPath))
					{
						BLEnumNode->ReloadEnum(BoundEnumObj);
						bNeedReconstruct = true;
					}
					else
					{
						UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:BoundEnum] Could not load enum '%s' for BlendListByEnum"), *EnumPath);
					}
				}
			}
		}

		// BlendListBase (includes BlendListByEnum, BlendListByBool, etc.): expand BlendTime array
		if (UAnimGraphNode_BlendListBase* BLNode = Cast<UAnimGraphNode_BlendListBase>(NewNode))
		{
			const TMap<int32, FString>* TimeVals = ArrayProperties.Find(TEXT("blend-time"));
			if (TimeVals && TimeVals->Num() > 0)
			{
				int32 MaxIdx = 0;
				for (const auto& KV : *TimeVals) MaxIdx = FMath::Max(MaxIdx, KV.Key);
				int32 RequiredSize = MaxIdx + 1;
				
				// Access the internal FAnimNode_BlendListBase via the first FAnimNode struct property
				for (TFieldIterator<FStructProperty> PropIt(BLNode->GetClass()); PropIt; ++PropIt)
				{
					FStructProperty* StructProp = *PropIt;
					if (StructProp->Struct && StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
					{
						void* NodePtr = StructProp->ContainerPtrToValuePtr<void>(BLNode);
						
						// Find and resize the BlendTime TArray
						FArrayProperty* BlendTimeArrayProp = nullptr;
						for (TFieldIterator<FArrayProperty> InnerIt(StructProp->Struct); InnerIt; ++InnerIt)
						{
							if (InnerIt->GetName() == TEXT("BlendTime"))
							{
								BlendTimeArrayProp = *InnerIt;
								break;
							}
						}
						
						if (BlendTimeArrayProp)
						{
							FScriptArrayHelper ArrayHelper(BlendTimeArrayProp, BlendTimeArrayProp->ContainerPtrToValuePtr<void>(NodePtr));
							// Ensure array has enough elements (add poses as needed)
							while (ArrayHelper.Num() < RequiredSize)
							{
								ArrayHelper.AddValue();
							}
							// Set values
							FFloatProperty* InnerFloat = CastField<FFloatProperty>(BlendTimeArrayProp->Inner);
							if (InnerFloat)
							{
								for (const auto& KV : *TimeVals)
								{
									if (KV.Key < ArrayHelper.Num())
									{
										float Val = FCString::Atof(*StripQuotes(KV.Value));
										InnerFloat->SetPropertyValue(ArrayHelper.GetRawPtr(KV.Key), Val);
									}
								}
							}
						}
						
						// Also resize BlendPose TArray to match
						FArrayProperty* BlendPoseArrayProp = nullptr;
						for (TFieldIterator<FArrayProperty> InnerIt(StructProp->Struct); InnerIt; ++InnerIt)
						{
							if (InnerIt->GetName() == TEXT("BlendPose"))
							{
								BlendPoseArrayProp = *InnerIt;
								break;
							}
						}
						if (BlendPoseArrayProp)
						{
							FScriptArrayHelper PoseHelper(BlendPoseArrayProp, BlendPoseArrayProp->ContainerPtrToValuePtr<void>(NodePtr));
							while (PoseHelper.Num() < RequiredSize)
							{
								PoseHelper.AddValue();
							}
						}
						break;
					}
				}
				bNeedReconstruct = true;
			}
		}
		
		// LayeredBoneBlend: expand BlendWeights array
		if (UAnimGraphNode_LayeredBoneBlend* LBBNode = Cast<UAnimGraphNode_LayeredBoneBlend>(NewNode))
		{
			const TMap<int32, FString>* WeightVals = ArrayProperties.Find(TEXT("blend-weights"));
			if (WeightVals && WeightVals->Num() > 0)
			{
				int32 MaxIdx = 0;
				for (const auto& KV : *WeightVals) MaxIdx = FMath::Max(MaxIdx, KV.Key);
				int32 RequiredSize = MaxIdx + 1;
				
				// Directly resize the internal arrays
				while (LBBNode->Node.BlendWeights.Num() < RequiredSize)
				{
					LBBNode->Node.AddPose();
				}
				
				for (const auto& KV : *WeightVals)
				{
					if (KV.Key < LBBNode->Node.BlendWeights.Num())
					{
						LBBNode->Node.BlendWeights[KV.Key] = FCString::Atof(*StripQuotes(KV.Value));
					}
				}
				bNeedReconstruct = true;
			}
		}

		if (bNeedReconstruct)
		{
			NewNode->ReconstructNode();

		}
	}

	// Custom-property nodes such as Control Rig derive their input pins from a target asset.
	// Restore typed module identity first; the reflected string remains a legacy fallback.
	bool bRestoredTypedRig = false;
	TMap<FString, FString> TypedRigInputPropertyNames;
	if (NodeAST->RigBinding.IsSet())
	{
		const FAnimRigNodeBinding& Binding = NodeAST->RigBinding.GetValue();
		const FString AssetPath = Binding.RigModule.AssetPath;
		const FString ObjectPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
		if (UAnimGraphNode_ControlRig* ControlRigNode = Cast<UAnimGraphNode_ControlRig>(NewNode))
		{
			const FAnimBPResolvedRig* ResolvedRig = GActiveAnimImportContext
				? GActiveAnimImportContext->ResolvedRigs.Find(AssetPath) : nullptr;
			UControlRigBlueprint* RigBlueprint = ResolvedRig
				? ResolvedRig->Blueprint.Get()
				: (!GActiveAnimImportContext || GActiveAnimImportContext->bAllowLegacyRigFallback
					? LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath) : nullptr);
			if (RigBlueprint)
			{
				UClass* RigClass = RigBlueprint->GeneratedClass.Get();
				if (RigClass && RigClass->IsChildOf(UControlRig::StaticClass()))
				{
					const FControlRigAssetStrongReference RigReference(RigBlueprint);
					const bool bLegacyMigratedBinding = GActiveAnimImportContext
						&& GActiveAnimImportContext->bAllowLegacyRigFallback
						&& GActiveAnimImportContext->LegacyExternalRigPaths.Contains(AssetPath)
						&& !ResolvedRig
						&& NodeAST->Coverage == EAnimNodeCoverage::Lossy
						&& Binding.EntryName.IsEmpty()
						&& Binding.Inputs.IsEmpty();
					if (bLegacyMigratedBinding)
					{
						FString SerializedReference;
						FControlRigAssetStrongReference::StaticStruct()->ExportText(
							SerializedReference, &RigReference, nullptr, nullptr, PPF_None, nullptr);
						if (SetNodeProperty(NewNode, TEXT("control-rig-asset-reference"), SerializedReference))
						{
							NewNode->ReconstructNode();
							bRestoredTypedRig = true;
						}
					}
					if (!bLegacyMigratedBinding)
					{
					const FRigModuleAST* RigModule = ResolvedRig && ResolvedRig->Module.IsValid()
						? ResolvedRig->Module.Get() : nullptr;
					FRigLangExportResult UncachedRigExport;
					if (!RigModule)
					{
						FRigLangExportResult* CachedRigExport = GActiveImporterRigValidationContext
							? GActiveImporterRigValidationContext->ModulesByAsset.Find(AssetPath) : nullptr;
						if (!CachedRigExport)
						{
							UncachedRigExport = FRigLangExporter::Export(RigBlueprint);
							CachedRigExport = GActiveImporterRigValidationContext
								? &GActiveImporterRigValidationContext->ModulesByAsset.Add(AssetPath, MoveTemp(UncachedRigExport))
								: &UncachedRigExport;
						}
						if (CachedRigExport->bSuccess && CachedRigExport->Module.IsValid())
						{
							RigModule = CachedRigExport->Module.Get();
						}
					}
					if (!RigModule)
					{
						UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ControlRigModule] Failed to inspect Rig module '%s'"), *AssetPath);
						Graph->RemoveNode(NewNode);
						return nullptr;
					}
					const TArray<FName>& SupportedEvents = RigReference.GetSupportedEvents();
					const int32 MatchingEntries = RigModule->Entries.FilterByPredicate(
						[&Binding, &SupportedEvents](const FRigEntryAST& Entry)
						{
							return Entry.Name == Binding.EntryName
								&& !Entry.EventName.Contains(TEXT("Construction"), ESearchCase::IgnoreCase)
								&& SupportedEvents.Contains(FName(*Entry.EventName));
						}).Num();
					if (MatchingEntries != 1)
					{
						UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ControlRigEntry] Entry '%s' is not one unique supported public Rig entry"), *Binding.EntryName);
						Graph->RemoveNode(NewNode);
						return nullptr;
					}
					FString SerializedReference;
					FControlRigAssetStrongReference::StaticStruct()->ExportText(
						SerializedReference, &RigReference, nullptr, nullptr, PPF_None, nullptr);
					TSet<FName> PublicInputProperties;
					for (const FRigVariableAST& Variable : RigModule->Variables)
					{
						if (Variable.Access != ERigVariableAccess::PublicInput) continue;
						const FName PropertyName(*Variable.Name);
						if (PublicInputProperties.Contains(PropertyName))
						{
							UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ControlRigInput] Public Rig inputs collide on exact property '%s'"), *Variable.Name);
							Graph->RemoveNode(NewNode);
							return nullptr;
						}
						PublicInputProperties.Add(PropertyName);
					}
					TSet<FName> BoundInputProperties;
					for (const FAnimRigInputBinding& Input : Binding.Inputs)
					{
						TArray<const FRigVariableAST*> Variables;
						for (const FRigVariableAST& Variable : RigModule->Variables)
						{
							if (Variable.Access == ERigVariableAccess::PublicInput
								&& AnimLispStableRuntimeSymbol(Variable.Name) == Input.RigInputName)
							{
								Variables.Add(&Variable);
							}
						}
						if (Variables.Num() != 1 || Variables[0]->Type != Input.ResolvedType)
						{
							UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ControlRigInput] Input '%s' has no unique exact public Rig variable/type"), *Input.RigInputName);
							Graph->RemoveNode(NewNode);
							return nullptr;
						}
						const FName PropertyName(*Variables[0]->Name);
						if (TypedRigInputPropertyNames.Contains(Input.RigInputName)
							|| BoundInputProperties.Contains(PropertyName))
						{
							UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ControlRigInput] Input '%s' collides on exact binding property '%s'"),
								*Input.RigInputName, *PropertyName.ToString());
							Graph->RemoveNode(NewNode);
							return nullptr;
						}
						TypedRigInputPropertyNames.Add(Input.RigInputName, Variables[0]->Name);
						BoundInputProperties.Add(PropertyName);
					}
					if (SetNodeProperty(NewNode, TEXT("control-rig-asset-reference"), SerializedReference))
					{
						NewNode->ReconstructNode();
						FString VisibilityError;
						if (!ConfigureTypedCustomPinVisibility(
							ControlRigNode, PublicInputProperties, BoundInputProperties, VisibilityError))
						{
							UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ControlRigInputVisibility] %s"), *VisibilityError);
							Graph->RemoveNode(NewNode);
							return nullptr;
						}
						for (const FAnimRigInputBinding& Input : Binding.Inputs)
						{
							const FString& PropertyName = TypedRigInputPropertyNames.FindChecked(Input.RigInputName);
							UEdGraphPin* ExactPin = nullptr;
							int32 ExactPinCount = 0;
							for (UEdGraphPin* CandidatePin : ControlRigNode->Pins)
							{
								if (CandidatePin && CandidatePin->Direction == EGPD_Input
									&& CandidatePin->PinName == FName(*PropertyName))
								{
									ExactPin = CandidatePin;
									++ExactPinCount;
								}
							}
							FProperty* PinProperty = ExactPin ? ControlRigNode->GetPinProperty(ExactPin->GetFName()) : nullptr;
							if (ExactPinCount != 1 || !PinProperty || PinProperty->GetFName() != ExactPin->PinName
								|| IMP_RigInputTypeFromPin(ExactPin->PinType) != Input.ResolvedType)
							{
								UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ControlRigInput] Input '%s' has no exact target pin/property/type"), *Input.RigInputName);
								Graph->RemoveNode(NewNode);
								return nullptr;
							}
						}
						bRestoredTypedRig = true;
					}
					}
				}
			}
		}
		if (!bRestoredTypedRig)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ControlRigModule] Failed to restore typed Rig module '%s' for node '%s'"),
				*Binding.RigModule.AssetPath, *NodeType);
			Graph->RemoveNode(NewNode);
			return nullptr;
		}
	}
	if (!NodeAST->RigBinding.IsSet())
	{
		if (const FString* RigReference = NodeAST->Properties.Find(TEXT("control-rig-asset-reference"));
			RigReference && SetNodeProperty(NewNode, TEXT("control-rig-asset-reference"), *RigReference))
		{
			NewNode->ReconstructNode();

			TSet<FString> ExposedNames;
			if (const FString* ExposedPins = NodeAST->Properties.Find(TEXT("exposed-input-pins")))
			{
				ParseExposedPinNames(*ExposedPins, ExposedNames);
			}
			else
			{
				// Compatibility with DSL exported before the explicit exposure manifest.
				for (const TPair<FString, FString>& Pair : NodeAST->Properties)
				{
					ExposedNames.Add(IMP_NormalizeBindingToken(Pair.Key));
				}
			}
			if (RestoreCustomPinVisibility(NewNode, ExposedNames))
			{
				NewNode->ReconstructNode();
			}
		}
	}

	if (UAnimGraphNode_LinkedInputPose* LinkedInputPose = Cast<UAnimGraphNode_LinkedInputPose>(NewNode))
	{
		if (const FString* NameProperty = NodeAST->Properties.Find(TEXT("name")))
		{
			LinkedInputPose->Node.Name = FName(*StripQuotes(*NameProperty));
			LinkedInputPose->ReconstructNode();
		}
	}

	// Some nodes, notably Orientation Warping, create different input pins for
	// each mode. Apply the shape-controlling enum before importing bindings.
	if (const FString* ModeProperty = NodeAST->Properties.Find(TEXT("mode")))
	{
		if (SetNodeProperty(NewNode, TEXT("mode"), *ModeProperty))
		{
			NewNode->ReconstructNode();
		}
	}

	const bool bUsesCurveAlpha = NodeAST->Properties.Contains(TEXT("alpha-curve-name"));
	const bool bUsesBoolAlpha = NodeAST->Properties.Contains(TEXT("b-alpha-bool-enabled"));
	if (bUsesCurveAlpha || bUsesBoolAlpha)
	{
		const EAnimAlphaInputType AlphaInputTypeValue = bUsesCurveAlpha
			? EAnimAlphaInputType::Curve
			: EAnimAlphaInputType::Bool;
		for (TFieldIterator<FStructProperty> PropIt(NewNode->GetClass()); PropIt; ++PropIt)
		{
			FStructProperty* StructProperty = *PropIt;
			if (!StructProperty->Struct || !StructProperty->Struct->IsChildOf(FAnimNode_Base::StaticStruct())) continue;
			if (FProperty* AlphaInputType = FindFProperty<FProperty>(StructProperty->Struct, TEXT("AlphaInputType")))
			{
				void* StructMemory = StructProperty->ContainerPtrToValuePtr<void>(NewNode);
				void* ValueMemory = AlphaInputType->ContainerPtrToValuePtr<void>(StructMemory);
				if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(AlphaInputType))
				{
					EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValueMemory, static_cast<int64>(AlphaInputTypeValue));
				}
				else if (FByteProperty* ByteProperty = CastField<FByteProperty>(AlphaInputType))
				{
					ByteProperty->SetPropertyValue(ValueMemory, static_cast<uint8>(AlphaInputTypeValue));
				}
			}
			break;
		}
		NewNode->ReconstructNode();
	}

	auto ApplyInputValue = [&](const FString& PropertyName, const FString& PropertyValue,
		const FAnimLispTypeRef* ResolvedType) -> bool
	{
		auto FindExactTargetPin = [&]() -> UEdGraphPin*
		{
			UEdGraphPin* Match = nullptr;
			for (UEdGraphPin* Pin : NewNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == FName(*PropertyName))
				{
					if (Match) return nullptr;
					Match = Pin;
				}
			}
			return Match;
		};
		if (ConnectPropertyBinding(Cast<UAnimBlueprint>(FBlueprintEditorUtils::FindBlueprintForGraph(Graph)),
			Graph, NewNode, PropertyName, PropertyValue, HelperGraphs))
		{
			if (ResolvedType && PropertyValue.StartsWith(TEXT("(bind-path ")))
			{
				const TMap<FName, FAnimGraphNodePropertyBinding>* PropertyBindings = GetMutablePropertyBindingMap(NewNode);
				const FAnimGraphNodePropertyBinding* Binding = PropertyBindings
					? PropertyBindings->Find(FName(*PropertyName)) : nullptr;
				return Binding && Binding->bIsBound && !Binding->PropertyPath.IsEmpty();
			}
			return true;
		}

		if (PropertyValue.StartsWith(TEXT("(pin-default ")))
		{
			const FLispParseResult ParsedDefault = FLispParser::Parse(PropertyValue);
			UEdGraphPin* TargetPin = ResolvedType ? FindExactTargetPin() : FindInputValuePin(NewNode, PropertyName);
			if (!ParsedDefault.bSuccess || ParsedDefault.Nodes.Num() != 1 || !ParsedDefault.Nodes[0].IsValid()
				|| ParsedDefault.Nodes[0]->GetFormName() != TEXT("pin-default")
				|| ParsedDefault.Nodes[0]->Num() != 2 || !TargetPin)
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:RigPinDefault] Node '%s' property ':%s' has invalid pin-default metadata or no target pin"),
					*NodeType, *PropertyName);
				return false;
			}
			FString DefaultValue = StripQuotes(ParsedDefault.Nodes[0]->Get(1)->ToString(false, 0));
			const bool bBoolType = (ResolvedType && ResolvedType->CPPType == TEXT("bool"))
				|| TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean;
			if (bBoolType)
			{
				if (!DefaultValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)
					&& !DefaultValue.Equals(TEXT("false"), ESearchCase::IgnoreCase))
				{
					UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:RigPinDefault] Node '%s' bool property ':%s' has invalid default '%s'"),
						*NodeType, *PropertyName, *DefaultValue);
					return false;
				}
				DefaultValue = DefaultValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)
					? TEXT("True") : TEXT("False");
			}
			if (TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int
				|| TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
			{
				int64 ParsedInteger = 0;
				if (!LexTryParseString(ParsedInteger, *DefaultValue))
				{
					UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:RigPinDefault] Node '%s' numeric property ':%s' has invalid default '%s'"),
						*NodeType, *PropertyName, *DefaultValue);
					return false;
				}
			}
			else if (TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
			{
				double ParsedReal = 0.0;
				if (!LexTryParseString(ParsedReal, *DefaultValue))
				{
					UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:RigPinDefault] Node '%s' numeric property ':%s' has invalid default '%s'"),
						*NodeType, *PropertyName, *DefaultValue);
					return false;
				}
			}
			const UEdGraphSchema* Schema = Graph->GetSchema();
			if (!Schema || !Schema->IsPinDefaultValid(TargetPin, DefaultValue, nullptr, FText::GetEmpty()).IsEmpty())
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:RigPinDefault] Node '%s' property ':%s' default is not valid for the exact target pin"),
					*NodeType, *PropertyName);
				return false;
			}
			Schema->TrySetDefaultValue(*TargetPin, DefaultValue);
			if (TargetPin->DefaultValue != DefaultValue)
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:RigPinDefault] Node '%s' property ':%s' default failed read-back verification"),
					*NodeType, *PropertyName);
				return false;
			}
			return true;
		}

		if (PropertyValue.StartsWith(TEXT("(value-expr ")))
		{
			const FLispParseResult ParsedValue = FLispParser::Parse(PropertyValue);
			UEdGraphPin* TargetPin = ResolvedType ? FindExactTargetPin() : FindInputValuePin(NewNode, PropertyName);
			if (!ParsedValue.bSuccess || ParsedValue.Nodes.Num() != 1 || !ParsedValue.Nodes[0].IsValid()
				|| ParsedValue.Nodes[0]->GetFormName() != TEXT("value-expr") || ParsedValue.Nodes[0]->Num() != 2 || !TargetPin)
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:LinkedPureExpression] Node '%s' property ':%s' has invalid value-expr metadata or no target pin"),
					*NodeType, *PropertyName);
				return false;
			}
			const FString ExpressionCode = ParsedValue.Nodes[0]->Get(1)->ToString(false, 0);
			const FBlueprintLispResult ImportedExpression = FBlueprintLispConverter::ImportPureExpression(Graph, TargetPin, ExpressionCode);
			if (!ImportedExpression.bSuccess)
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:LinkedPureExpression] Node '%s' property ':%s' import failed: %s"),
					*NodeType, *PropertyName, *ImportedExpression.Error);
				return false;
			}
			if (TargetPin->LinkedTo.IsEmpty())
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:LinkedPureExpression] Node '%s' property ':%s' import produced no target connection"),
					*NodeType, *PropertyName);
				return false;
			}
			return true;
		}

		if (PropertyValue.StartsWith(TEXT("(var ")) || PropertyValue.StartsWith(TEXT("(ref ")))
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:EventGraphConnection] Node '%s' property ':%s' = %s — this pin is driven by an EventGraph node, connection cannot be auto-restored; pin will use default value"),
				*NodeType, *PropertyName, *PropertyValue);
			return false;
		}

		if (PropertyValue.StartsWith(TEXT("(asset ")))
		{
			FString AssetPath = PropertyValue;
			AssetPath.RemoveFromStart(TEXT("(asset "));
			AssetPath.RemoveFromEnd(TEXT(")"));
			AssetPath = StripQuotes(AssetPath);
			if (!SetNodeProperty(NewNode, PropertyName, FString::Printf(TEXT("\"%s\""), *AssetPath)))
			{
				UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:AssetRef] Node '%s' property ':%s' = %s — asset reference could not be set via property reflection"),
					*NodeType, *PropertyName, *PropertyValue);
				return false;
			}
			return true;
		}

		if (!SetNodeProperty(NewNode, PropertyName, PropertyValue))
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:PropertySet] Node '%s' property ':%s' = %s — could not find matching pin or FProperty"),
				*NodeType, *PropertyName, *PropertyValue);
			return false;
		}
		return true;
	};

	// Set non-pose properties via the same binding/default restoration path used by typed Rig inputs.
	for (const auto& Pair : NodeAST->Properties)
	{
		// Skip special properties already handled above
		if (Pair.Key == TEXT("name") || Pair.Key == TEXT("loop") ||
			Pair.Key == TEXT("class") || Pair.Key == TEXT("initial") ||
			Pair.Key == TEXT("transitions") || Pair.Key == TEXT("state-nodes") || Pair.Key == TEXT("layer") ||
			Pair.Key == TEXT("interface") || Pair.Key == TEXT("bound-enum") ||
			Pair.Key == TEXT("exposed-input-pins") ||
			Pair.Key == TEXT("bound-graph-class") || Pair.Key == TEXT("bound-graph-schema") ||
			Pair.Key == TEXT("bound-graph-guid"))
		{
			continue;
		}
		if (NodeAST->RigBinding.IsSet()
			&& Pair.Key == TEXT("control-rig-asset-reference"))
		{
			continue;
		}

		// Skip sequence/blend-space only for nodes that handle them in special branches above
		if (Pair.Key == TEXT("sequence") && Cast<UAnimGraphNode_SequencePlayer>(NewNode))
		{
			continue;
		}
		if (Pair.Key == TEXT("blend-space") && Cast<UAnimGraphNode_BlendSpacePlayer>(NewNode))
		{
			continue;
		}
		
		ApplyInputValue(Pair.Key, Pair.Value, nullptr);
	}
	if (NodeAST->RigBinding.IsSet())
	{
		for (const FAnimRigInputBinding& Input : NodeAST->RigBinding.GetValue().Inputs)
		{
			const FString* PropertyName = TypedRigInputPropertyNames.Find(Input.RigInputName);
			if (!PropertyName || !ApplyInputValue(*PropertyName, Input.ValueExpression, &Input.ResolvedType))
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ControlRigInputValue] Failed to restore typed Rig input '%s'"),
					*Input.RigInputName);
				Graph->RemoveNode(NewNode);
				return nullptr;
			}
		}
	}

	auto ReapplyBlendTime = [&]()
	{
		if (UAnimGraphNode_BlendListBase* BlendListNode = Cast<UAnimGraphNode_BlendListBase>(NewNode))
		{
			if (const FString* BlendTimeProperty = NodeAST->Properties.Find(TEXT("blend-time")))
			{
				BlendListNode->ReconstructNode();
				SetNodeProperty(BlendListNode, TEXT("blend-time"), *BlendTimeProperty);

				FString BlendTimeText = StripQuotes(*BlendTimeProperty);
				BlendTimeText.RemoveFromStart(TEXT("("));
				BlendTimeText.RemoveFromEnd(TEXT(")"));
				TArray<FString> BlendTimes;
				BlendTimeText.ParseIntoArray(BlendTimes, TEXT(","), false);
				for (UEdGraphPin* Pin : BlendListNode->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Input) continue;
					FString NormalizedPinName = Pin->PinName.ToString().ToLower();
					NormalizedPinName.ReplaceInline(TEXT(" "), TEXT(""));
					NormalizedPinName.ReplaceInline(TEXT("_"), TEXT(""));
					NormalizedPinName.ReplaceInline(TEXT("-"), TEXT(""));
					if (!NormalizedPinName.StartsWith(TEXT("blendtime"))) continue;
					const FString IndexText = NormalizedPinName.Mid(9);
					if (!IndexText.IsNumeric()) continue;
					const int32 Index = FCString::Atoi(*IndexText);
					if (BlendTimes.IsValidIndex(Index))
					{
						Pin->DefaultValue = BlendTimes[Index].TrimStartAndEnd();
					}
				}
			}
		}
	};
	
	// ---- Handle state machine ----
	if (NodeType == TEXT("state-machine"))
	{
		UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(NewNode);
		if (!SMNode || !BuildStateMachine(SMNode, NodeAST, DefineNodes, HelperGraphs))
		{
			Graph->RemoveNode(NewNode);
			return nullptr;
		}
		// State machine children are managed internally by BuildStateMachine (as UAnimStateNode),
		// not via pose input pins on the state machine node itself. Skip the generic child-connect loop.
		UE_LOG(LogAnimBPImporter, Log, TEXT("Created node: %s (%s)"), *NodeType, *NodeClass->GetName());
		return NewNode;
	}

	bool bIsBlendStackNode = false;
	for (const UClass* TestClass = NewNode->GetClass(); TestClass; TestClass = TestClass->GetSuperClass())
	{
		if (TestClass->GetName() == TEXT("AnimGraphNode_BlendStack_Base"))
		{
			bIsBlendStackNode = true;
			break;
		}
	}
	if (bIsBlendStackNode)
	{
		const FNamedChild* SampleChild = NodeAST->Children.FindByPredicate(
			[](const FNamedChild& Child) { return Child.PinName == TEXT("sample-graph"); });
		if (!SampleChild || !SampleChild->Node.IsValid() || NewNode->GetSubGraphs().IsEmpty())
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:BlendStackBoundGraph] BlendStack '%s' is missing :sample-graph or bound graph"), *NodeType);
			Graph->RemoveNode(NewNode);
			return nullptr;
		}

		UEdGraph* BoundGraph = NewNode->GetSubGraphs()[0];
		if (const FString* RequestedClass = NodeAST->Properties.Find(TEXT("bound-graph-class"));
			RequestedClass && StripQuotes(*RequestedClass) != BoundGraph->GetClass()->GetPathName())
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:BlendStackBoundGraph] Bound graph class mismatch: requested %s, created %s"),
				**RequestedClass, *BoundGraph->GetClass()->GetPathName());
			Graph->RemoveNode(NewNode);
			return nullptr;
		}
		if (const FString* RequestedSchema = NodeAST->Properties.Find(TEXT("bound-graph-schema"));
			RequestedSchema && BoundGraph->GetSchema()
			&& StripQuotes(*RequestedSchema) != BoundGraph->GetSchema()->GetClass()->GetPathName())
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:BlendStackBoundGraph] Bound graph schema mismatch: requested %s, created %s"),
				**RequestedSchema, *BoundGraph->GetSchema()->GetClass()->GetPathName());
			Graph->RemoveNode(NewNode);
			return nullptr;
		}
		if (const FString* RequestedGuid = NodeAST->Properties.Find(TEXT("bound-graph-guid")))
		{
			FGuid ParsedGuid;
			if (!FGuid::Parse(StripQuotes(*RequestedGuid), ParsedGuid) || !ParsedGuid.IsValid())
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:BlendStackBoundGraph] Invalid bound graph GUID: %s"), **RequestedGuid);
				Graph->RemoveNode(NewNode);
				return nullptr;
			}
			BoundGraph->GraphGuid = ParsedGuid;
		}

		UAnimGraphNode_Root* SampleResult = nullptr;
		TArray<UEdGraphNode*> SampleNodesToRemove;
		for (UEdGraphNode* BoundNode : BoundGraph->Nodes)
		{
			if (!SampleResult) SampleResult = Cast<UAnimGraphNode_Root>(BoundNode);
			if (BoundNode && !BoundNode->IsA<UAnimGraphNode_Root>()) SampleNodesToRemove.Add(BoundNode);
		}
		for (UEdGraphNode* BoundNode : SampleNodesToRemove) BoundGraph->RemoveNode(BoundNode);
		if (!SampleResult)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:BlendStackBoundGraph] Created bound graph has no result node"));
			Graph->RemoveNode(NewNode);
			return nullptr;
		}
		for (UEdGraphPin* Pin : SampleResult->Pins) if (Pin) Pin->BreakAllPinLinks();

		UAnimGraphNode_Base* SampleRoot = BuildAnimNode(SampleChild->Node, BoundGraph, DefineNodes, HelperGraphs);
		if (!SampleRoot)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:BlendStackBoundGraph] Failed to rebuild sample graph root '%s'"),
				*SampleChild->Node->NodeType);
			Graph->RemoveNode(NewNode);
			return nullptr;
		}
		UEdGraphPin* SampleOutput = FindOutputPosePin(SampleRoot);
		UEdGraphPin* ResultInput = nullptr;
		for (UEdGraphPin* Pin : SampleResult->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				ResultInput = Pin;
				break;
			}
		}
		if (!SampleOutput || !ResultInput)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:BlendStackBoundGraph] Sample graph root/result pose pins are missing"));
			Graph->RemoveNode(NewNode);
			return nullptr;
		}
		ConnectPins(SampleOutput, ResultInput);
	}
	
	// ---- Recursively build children and connect ----
	for (const FNamedChild& Child : NodeAST->Children)
	{
		if (bIsBlendStackNode && Child.PinName == TEXT("sample-graph"))
		{
			continue;
		}
		if (!Child.Node.IsValid())
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:InvalidChild] Node '%s' has a null child node in DSL (pin '%s') — skipping"),
				*NodeType, *Child.PinName);
			continue;
		}
		
		UAnimGraphNode_Base* ChildNode = BuildAnimNode(Child.Node, Graph, DefineNodes, HelperGraphs);
		if (!ChildNode)
		{
			// identity-pose is intentionally null (no node created); others should log
			if (Child.Node->NodeType != TEXT("identity-pose"))
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:ChildBuildFailed] Node '%s' child '%s' (type '%s') could not be built"),
					*NodeType, *Child.PinName, *Child.Node->NodeType);
				Graph->RemoveNode(NewNode);
				return nullptr;
			}
			continue;
		}
		
		// Find the output pin on the child
		UEdGraphPin* ChildOutput = FindOutputPosePin(ChildNode);
		if (!ChildOutput)
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:NoOutputPin] Child '%s' (type '%s') has no output pose pin — cannot connect to parent '%s'"),
				*Child.PinName, *Child.Node->NodeType, *NodeType);
			continue;
		}
		
		// Find the input pin on the parent by the named pin
		UEdGraphPin* ParentInput = nullptr;
		if (!Child.PinName.IsEmpty())
		{
			ParentInput = FindInputPosePin(NewNode, Child.PinName);
		}
		else
		{
			// Try the default/first pose input
			ParentInput = FindInputPosePin(NewNode, TEXT(""));
		}
		
		if (ParentInput)
		{
			ConnectPins(ChildOutput, ParentInput);
		}
		else
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not connect child '%s' to parent '%s' pin '%s'"),
				*Child.Node->NodeType, *NodeType, *Child.PinName);
		}
	}

	ReapplyBlendTime();
	static const FName AlphaPropertiesToReapply[] = {
		TEXT("alpha-bool-blend"),
		TEXT("alpha-scale-bias-clamp")
	};
	for (const FName& PropertyName : AlphaPropertiesToReapply)
	{
		if (const FString* PropertyValue = NodeAST->Properties.Find(PropertyName.ToString()))
		{
			SetNodeProperty(NewNode, PropertyName.ToString(), *PropertyValue);
		}
	}
	UE_LOG(LogAnimBPImporter, Log, TEXT("Created node: %s (%s)"), *NodeType, *NodeClass->GetName());
	return NewNode;
}

// ========== State Machine Building ==========

namespace
{
	struct FImportedStateMachineNode
	{
		FString Kind;
		FString Name;
		FString ChildId;
		bool bEmpty = false;
		bool bGlobal = false;
		TArray<FString> Targets;
		FString RuleGraph;
	};

	static bool ParseStateMachineNodes(const FString& Source, TArray<FImportedStateMachineNode>& OutNodes)
	{
		TArray<FAnimLangToken> Tokens;
		TArray<FAnimLangLexError> Errors;
		if (!FAnimLangTokenizer::Tokenize(Source, Tokens, Errors) || Errors.Num() > 0)
		{
			return false;
		}

		int32 Index = 0;
		auto Is = [&Tokens, &Index](const EAnimLangTokenType Type)
		{
			return Tokens.IsValidIndex(Index) && Tokens[Index].Type == Type;
		};
		if (!Is(EAnimLangTokenType::LBracket))
		{
			return false;
		}
		++Index;

		while (Tokens.IsValidIndex(Index) && !Is(EAnimLangTokenType::RBracket) && !Is(EAnimLangTokenType::EndOfFile))
		{
			if (!Is(EAnimLangTokenType::LParen))
			{
				++Index;
				continue;
			}
			++Index;
			if (!Is(EAnimLangTokenType::Identifier))
			{
				return false;
			}

			FImportedStateMachineNode& Node = OutNodes.AddDefaulted_GetRef();
			Node.Kind = Tokens[Index++].Value;
			while (Tokens.IsValidIndex(Index) && !Is(EAnimLangTokenType::RParen) && !Is(EAnimLangTokenType::EndOfFile))
			{
				if (!Is(EAnimLangTokenType::Keyword))
				{
					++Index;
					continue;
				}
				const FString Key = Tokens[Index++].Value;
				if (!Tokens.IsValidIndex(Index))
				{
					return false;
				}
				const FAnimLangToken& Value = Tokens[Index++];
				if (Key == TEXT("name") && Value.Type == EAnimLangTokenType::String) Node.Name = Value.Value;
				else if (Key == TEXT("child") && Value.Type == EAnimLangTokenType::String) Node.ChildId = Value.Value;
				else if (Key == TEXT("empty") && Value.Type == EAnimLangTokenType::Bool) Node.bEmpty = Value.Value == TEXT("true");
				else if (Key == TEXT("global") && Value.Type == EAnimLangTokenType::Bool) Node.bGlobal = Value.Value == TEXT("true");
				else if (Key == TEXT("target") && Value.Type == EAnimLangTokenType::String) Node.Targets.Add(Value.Value);
				else if (Key == TEXT("rule-graph") && Value.Type == EAnimLangTokenType::String) Node.RuleGraph = Value.Value;
			}
			if (!Is(EAnimLangTokenType::RParen) || Node.Name.IsEmpty())
			{
				return false;
			}
			++Index;
		}
		return Is(EAnimLangTokenType::RBracket);
	}
}

bool FAnimBPImporter::BuildStateMachine(UAnimGraphNode_StateMachine* SMNode, const TSharedPtr<FAnimNodeAST>& NodeAST,
	const TMap<FString, UAnimGraphNode_SaveCachedPose*>* DefineNodes,
	const TMap<FString, FHelperGraphDef>* HelperGraphs)
{
	if (!SMNode || !NodeAST.IsValid()) return false;
	
	// Get the state machine graph
	UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph;
	if (!SMGraph)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("StateMachine node has no EditorStateMachineGraph"));
		return false;
	}
	
	// Parse state machine properties
	const FString* NameProp = NodeAST->Properties.Find(TEXT("name"));
	if (NameProp)
	{
		FString SMName = StripQuotes(*NameProp);
		// Rename the state machine node (this renames the internal graph too)
		SMNode->OnRenameNode(SMName);
	}
	
	const FString* InitialProp = NodeAST->Properties.Find(TEXT("initial"));
	FString InitialStateName;
	if (InitialProp)
	{
		InitialStateName = StripQuotes(*InitialProp);
	}
	
	TArray<FImportedStateMachineNode> NodeSpecs;
	if (const FString* StateNodesProp = NodeAST->Properties.Find(TEXT("state-nodes")))
	{
		if (!ParseStateMachineNodes(*StateNodesProp, NodeSpecs))
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("State machine has malformed :state-nodes metadata"));
			return false;
		}
	}
	else
	{
		// Backward compatibility for DSL written before explicit topology metadata.
		for (const FNamedChild& Child : NodeAST->Children)
		{
			FImportedStateMachineNode& Spec = NodeSpecs.AddDefaulted_GetRef();
			Spec.Kind = TEXT("state");
			Spec.ChildId = Child.PinName;
			Spec.Name = Child.PinName;
			Spec.Name.ReplaceInline(TEXT("-"), TEXT(" "));
			for (int32 CharIndex = 0; CharIndex < Spec.Name.Len(); ++CharIndex)
			{
				if (CharIndex == 0 || Spec.Name[CharIndex - 1] == ' ')
				{
					Spec.Name[CharIndex] = FChar::ToUpper(Spec.Name[CharIndex]);
				}
			}
		}
	}

	auto FindStateChild = [&NodeAST](const FString& ChildId) -> TSharedPtr<FAnimNodeAST>
	{
		for (const FNamedChild& Child : NodeAST->Children)
		{
			if (Child.PinName == ChildId) return Child.Node;
		}
		return nullptr;
	};

	TMap<FString, UAnimStateNodeBase*> StateNodes;
	TMap<FString, UAnimStateAliasNode*> AliasNodes;
	for (const FImportedStateMachineNode& Spec : NodeSpecs)
	{
		if (Spec.Kind == TEXT("state"))
		{
			UAnimStateNode* StateNode = NewObject<UAnimStateNode>(SMGraph);
			StateNode->CreateNewGuid();
			if (Spec.ChildId.StartsWith(TEXT("state-")))
			{
				FGuid RestoredGuid;
				if (FGuid::ParseExact(Spec.ChildId.Mid(6), EGuidFormats::Digits, RestoredGuid))
				{
					StateNode->NodeGuid = RestoredGuid;
				}
			}
			StateNode->PostPlacedNewNode();
			StateNode->AllocateDefaultPins();
			SMGraph->AddNode(StateNode, false, false);
			StateNode->OnRenameNode(Spec.Name);

			const TSharedPtr<FAnimNodeAST> StateAnimation = FindStateChild(Spec.ChildId);
			if (StateAnimation.IsValid())
			{
				if (!StateNode->BoundGraph)
				{
					return false;
				}
				UAnimGraphNode_StateResult* ResultNode = StateNode->GetResultNodeInsideState();
				UAnimGraphNode_Base* AnimTree = BuildAnimNode(StateAnimation, StateNode->BoundGraph, DefineNodes, HelperGraphs);
				if (!AnimTree || !ResultNode)
				{
					return false;
				}
				ConnectPins(FindOutputPosePin(AnimTree), StateNode->GetPoseSinkPinInsideState());
			}
			StateNodes.Add(Spec.Name, StateNode);
			UE_LOG(LogAnimBPImporter, Log, TEXT("Created state: %s"), *Spec.Name);
		}
		else if (Spec.Kind == TEXT("alias"))
		{
			UAnimStateAliasNode* AliasNode = NewObject<UAnimStateAliasNode>(SMGraph);
			AliasNode->CreateNewGuid();
			AliasNode->PostPlacedNewNode();
			AliasNode->AllocateDefaultPins();
			SMGraph->AddNode(AliasNode, false, false);
			AliasNode->OnRenameNode(Spec.Name);
			AliasNode->bGlobalAlias = Spec.bGlobal;
			StateNodes.Add(Spec.Name, AliasNode);
			AliasNodes.Add(Spec.Name, AliasNode);
			UE_LOG(LogAnimBPImporter, Log, TEXT("Created alias: %s"), *Spec.Name);
		}
		else if (Spec.Kind == TEXT("conduit"))
		{
			UAnimStateConduitNode* ConduitNode = NewObject<UAnimStateConduitNode>(SMGraph);
			ConduitNode->CreateNewGuid();
			ConduitNode->PostPlacedNewNode();
			ConduitNode->AllocateDefaultPins();
			SMGraph->AddNode(ConduitNode, false, false);
			ConduitNode->OnRenameNode(Spec.Name);
			if (!Spec.RuleGraph.IsEmpty() && ConduitNode->GetBoundGraph())
			{
				FBlueprintLispConverter::FImportOptions LispOpts;
				LispOpts.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
				LispOpts.bAutoLayout = false;
				LispOpts.bCompile = false;
				const FBlueprintLispResult LispResult = FBlueprintLispConverter::ImportGraph(
					ConduitNode->GetBoundGraph(), Spec.RuleGraph, LispOpts);
				if (!LispResult.bSuccess)
				{
					UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:ConduitRuleGraphImport] Conduit '%s': %s"),
						*Spec.Name, *LispResult.Error);
				}
			}
			StateNodes.Add(Spec.Name, ConduitNode);
			UE_LOG(LogAnimBPImporter, Log, TEXT("Created conduit: %s"), *Spec.Name);
		}
	}

	for (const FImportedStateMachineNode& Spec : NodeSpecs)
	{
		UAnimStateAliasNode* const* AliasPtr = AliasNodes.Find(Spec.Name);
		if (!AliasPtr || !*AliasPtr) continue;
		for (const FString& TargetName : Spec.Targets)
		{
			if (UAnimStateNodeBase* const* Target = StateNodes.Find(TargetName))
			{
				(*AliasPtr)->GetAliasedStates().Add(*Target);
			}
			else
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("Alias '%s' target '%s' was not found"), *Spec.Name, *TargetName);
			}
		}
	}
	
	// Connect entry node to initial state
	if (!InitialStateName.IsEmpty())
	{
		UAnimStateNodeBase** InitState = StateNodes.Find(InitialStateName);
		if (InitState && *InitState)
		{
			// Find the entry node
			UAnimStateEntryNode* EntryNode = SMGraph->EntryNode;
			if (EntryNode)
			{
				UEdGraphPin* EntryOutput = nullptr;
				for (UEdGraphPin* Pin : EntryNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output)
					{
						EntryOutput = Pin;
						break;
					}
				}
				UEdGraphPin* StateInput = nullptr;
				for (UEdGraphPin* Pin : (*InitState)->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input)
					{
						StateInput = Pin;
						break;
					}
				}
				if (EntryOutput && StateInput)
				{
					ConnectPins(EntryOutput, StateInput);
				}
			}
		}
	}
	
	// ---- Build transitions from :transitions property ----
	const FString* TransProp = NodeAST->Properties.Find(TEXT("transitions"));
	if (TransProp && !TransProp->IsEmpty())
	{
		// Parse the transitions string: [(FromState -> ToState :key val ...) ...]
		// State names can contain spaces, so we parse carefully using "->" as the delimiter
		struct FParsedTransition
		{
			FString FromState;
			FString ToState;
			float Duration = 0.2f;
			int32 Priority = 0;
			bool bBidirectional = false;
			bool bAutoRule = false;
			float AutoRuleTriggerTime = -1.0f;
			FString RuleRef;    // (var/ref "...") summary string, for logging
			FString RuleGraph;  // BlueprintLisp DSL of the full transition condition graph
		};
		TArray<FParsedTransition> ParsedTransitions;
		
		FString TransStr = *TransProp;
		TransStr.TrimStartAndEndInline();
		
		// Strip outer brackets [ ... ]
		if (TransStr.StartsWith(TEXT("[")) && TransStr.EndsWith(TEXT("]")))
		{
			TransStr = TransStr.Mid(1, TransStr.Len() - 2);
			TransStr.TrimStartAndEndInline();
		}
		
		// Split into individual transition entries by matching balanced parentheses
		// Each entry is (FromState -> ToState :key val ...)
		int32 Pos = 0;
		while (Pos < TransStr.Len())
		{
			// Skip whitespace
			while (Pos < TransStr.Len() && FChar::IsWhitespace(TransStr[Pos])) Pos++;
			if (Pos >= TransStr.Len()) break;
			
			if (TransStr[Pos] != '(')
			{
				Pos++;
				continue;
			}
			
			// Find matching closing paren, respecting nested parens and quoted strings
			int32 Start = Pos + 1; // skip opening '('
			int32 Depth = 1;
			Pos++;
			bool bInString = false;
			while (Pos < TransStr.Len() && Depth > 0)
			{
				TCHAR Ch = TransStr[Pos];
				if (bInString)
				{
					if (Ch == '"' && (Pos == 0 || TransStr[Pos-1] != '\\'))
					{
						bInString = false;
					}
				}
				else
				{
					if (Ch == '"') bInString = true;
					else if (Ch == '(') Depth++;
					else if (Ch == ')') Depth--;
				}
				if (Depth > 0) Pos++;
			}
			
			if (Depth != 0) break; // malformed
			
			FString EntryStr = TransStr.Mid(Start, Pos - Start);
			EntryStr.TrimStartAndEndInline();
			Pos++; // skip closing ')'
			
			// Parse the entry: "FromState -> ToState :duration X :priority Y :bidirectional true :rule (...)"
			// Find " -> " to split From and the rest
			int32 ArrowIdx = INDEX_NONE;
			// Search for the delimiter outside quoted endpoint names. Alias names may contain "->".
			int32 SearchPos = 0;
			bool bFoundArrow = false;
			bool bInEndpointString = false;
			while (SearchPos < EntryStr.Len() - 2)
			{
				if (EntryStr[SearchPos] == '"' && (SearchPos == 0 || EntryStr[SearchPos - 1] != '\\'))
				{
					bInEndpointString = !bInEndpointString;
				}
				else if (!bInEndpointString && EntryStr[SearchPos] == '-' && EntryStr[SearchPos+1] == '>')
				{
					ArrowIdx = SearchPos;
					bFoundArrow = true;
					break;
				}
				SearchPos++;
			}
			
			if (!bFoundArrow)
			{
				UE_LOG(LogAnimBPImporter, Warning, TEXT("Transition entry missing '->': %s"), *EntryStr);
				continue;
			}
			
			FParsedTransition Trans;
			Trans.FromState = StripQuotes(EntryStr.Left(ArrowIdx).TrimStartAndEnd());
			
			// After "->", the rest is "ToState :key val ..."
			// ToState ends at the first " :" (space + colon = keyword start)
			FString Rest = EntryStr.Mid(ArrowIdx + 2).TrimStartAndEnd();
			
			// Find the first keyword marker " :" to separate ToState from properties
			int32 FirstKeyword = INDEX_NONE;
			for (int32 i = 0; i < Rest.Len() - 1; i++)
			{
				if (Rest[i] == ' ' && Rest[i+1] == ':')
				{
					FirstKeyword = i;
					break;
				}
			}
			
			if (FirstKeyword != INDEX_NONE)
			{
				Trans.ToState = StripQuotes(Rest.Left(FirstKeyword).TrimStartAndEnd());
				FString Props = Rest.Mid(FirstKeyword).TrimStartAndEnd();
				
				// Parse keyword properties from the Props string
				// Keywords: :duration, :priority, :bidirectional, :rule
				// Tokenize by splitting on " :" boundaries
				TArray<TPair<FString, FString>> KeyVals;
				int32 KPos = 0;
				while (KPos < Props.Len())
				{
					// Skip whitespace
					while (KPos < Props.Len() && FChar::IsWhitespace(Props[KPos])) KPos++;
					if (KPos >= Props.Len()) break;
					
					if (Props[KPos] != ':')
					{
						KPos++;
						continue;
					}
					KPos++; // skip ':'
					
					// Read keyword name (until space)
					int32 KeyStart = KPos;
					while (KPos < Props.Len() && !FChar::IsWhitespace(Props[KPos])) KPos++;
					FString Key = Props.Mid(KeyStart, KPos - KeyStart);
					
					// Skip space
					while (KPos < Props.Len() && FChar::IsWhitespace(Props[KPos])) KPos++;
					
				// Read value — could be a simple token, a quoted string, or a nested (...)
				FString Value;
				if (KPos < Props.Len() && Props[KPos] == '"')
				{
					// Quoted string value — read until closing unescaped quote
					KPos++; // skip opening quote
					int32 VStart = KPos;
					FString Unescaped;
					while (KPos < Props.Len())
					{
						TCHAR Ch = Props[KPos];
						if (Ch == '\\' && KPos + 1 < Props.Len())
						{
							TCHAR Next = Props[KPos + 1];
							if (Next == '"')       { Unescaped += '"';  KPos += 2; continue; }
							if (Next == '\\')      { Unescaped += '\\'; KPos += 2; continue; }
							if (Next == 'n')       { Unescaped += '\n'; KPos += 2; continue; }
							if (Next == 'r')       { Unescaped += '\r'; KPos += 2; continue; }
						}
						if (Ch == '"') { KPos++; break; } // closing quote
						Unescaped += Ch;
						KPos++;
					}
					Value = Unescaped;
				}
				else if (KPos < Props.Len() && Props[KPos] == '(')
				{
						// Nested expression — find matching paren
						int32 VStart = KPos;
						int32 VDepth = 0;
						bool bVInStr = false;
						while (KPos < Props.Len())
						{
							TCHAR VCh = Props[KPos];
							if (bVInStr)
							{
								if (VCh == '"' && (KPos == 0 || Props[KPos-1] != '\\'))
									bVInStr = false;
							}
							else
							{
								if (VCh == '"') bVInStr = true;
								else if (VCh == '(') VDepth++;
								else if (VCh == ')') { VDepth--; if (VDepth == 0) { KPos++; break; } }
							}
							KPos++;
						}
						Value = Props.Mid(VStart, KPos - VStart);
					}
					else
					{
						// Simple token (until next space+colon or end)
						int32 VStart = KPos;
						while (KPos < Props.Len())
						{
							// Check if this is the start of next keyword
							if (Props[KPos] == ' ' && KPos + 1 < Props.Len() && Props[KPos+1] == ':')
								break;
							KPos++;
						}
						Value = Props.Mid(VStart, KPos - VStart).TrimStartAndEnd();
					}
					
					KeyVals.Add(TPair<FString, FString>(Key, Value));
				}
				
				// Apply parsed key-value pairs
				for (const auto& KV : KeyVals)
				{
					if (KV.Key == TEXT("duration"))
					{
						Trans.Duration = FCString::Atof(*KV.Value);
					}
					else if (KV.Key == TEXT("priority"))
					{
						Trans.Priority = FCString::Atoi(*KV.Value);
					}
					else if (KV.Key == TEXT("bidirectional"))
					{
						Trans.bBidirectional = (KV.Value == TEXT("true"));
					}
					else if (KV.Key == TEXT("rule"))
					{
						// Check if it's an auto-rule or a ref
						if (KV.Value.StartsWith(TEXT("(auto-rule")))
						{
							Trans.bAutoRule = true;
							// Parse :time-remaining value
							if (KV.Value.Contains(TEXT("crossfade-duration")))
							{
								Trans.AutoRuleTriggerTime = -1.0f;
							}
							else
							{
								// Extract the float value after :time-remaining
								int32 TRIdx = KV.Value.Find(TEXT(":time-remaining"));
								if (TRIdx != INDEX_NONE)
								{
									FString TimeStr = KV.Value.Mid(TRIdx + 15).TrimStartAndEnd();
									TimeStr.RemoveFromEnd(TEXT(")"));
									TimeStr.TrimStartAndEndInline();
									Trans.AutoRuleTriggerTime = FCString::Atof(*TimeStr);
								}
							}
						}
						else if (KV.Value.StartsWith(TEXT("(var")) || KV.Value.StartsWith(TEXT("(ref")))
						{
							Trans.RuleRef = KV.Value;
							// (var/ref "...") conditions require EventGraph nodes — cannot be auto-reconstructed
						}
					}
					else if (KV.Key == TEXT("rule-graph"))
					{
						// BlueprintLisp DSL of the full transition condition graph
						// Value has already been unescaped by the quoted-string reader above
						Trans.RuleGraph = KV.Value;
					}
				}
			}
			else
			{
				// No keywords — just "ToState"
				Trans.ToState = StripQuotes(Rest.TrimStartAndEnd());
			}
			
			if (!Trans.FromState.IsEmpty() && !Trans.ToState.IsEmpty())
			{
				ParsedTransitions.Add(Trans);
			}
		}
		
		// Create UAnimStateTransitionNode for each parsed transition
		// Track bidirectional pairs to avoid creating duplicates
		// (Exporter emits A->B and B->A separately when Bidirectional=true)
		TSet<FString> CreatedBidirectionalPairs;
		int32 CreatedCount = 0;
		
		for (const FParsedTransition& Trans : ParsedTransitions)
		{
			// Skip reverse of an already-created bidirectional transition
			if (Trans.bBidirectional)
			{
				FString ReverseKey = FString::Printf(TEXT("%s|%s"), *Trans.ToState, *Trans.FromState);
				if (CreatedBidirectionalPairs.Contains(ReverseKey))
				{
					UE_LOG(LogAnimBPImporter, Log, TEXT("Skipping reverse bidirectional transition: %s -> %s (already created as %s -> %s)"),
						*Trans.FromState, *Trans.ToState, *Trans.ToState, *Trans.FromState);
					continue;
				}
			}
			
			UAnimStateNodeBase** FromStatePtr = StateNodes.Find(Trans.FromState);
			UAnimStateNodeBase** ToStatePtr = StateNodes.Find(Trans.ToState);
			
			if (!FromStatePtr || !*FromStatePtr)
			{
				UE_LOG(LogAnimBPImporter, Warning, TEXT("Transition: FromState '%s' not found in state machine"), *Trans.FromState);
				continue;
			}
			if (!ToStatePtr || !*ToStatePtr)
			{
				UE_LOG(LogAnimBPImporter, Warning, TEXT("Transition: ToState '%s' not found in state machine"), *Trans.ToState);
				continue;
			}
			
			UAnimStateTransitionNode* TransNode = NewObject<UAnimStateTransitionNode>(SMGraph);
			if (!TransNode) continue;
			
			TransNode->CreateNewGuid();
			TransNode->PostPlacedNewNode();
			TransNode->AllocateDefaultPins();
			SMGraph->AddNode(TransNode, false, false);
			
			// Connect the transition between From and To state nodes
			TransNode->CreateConnections(*FromStatePtr, *ToStatePtr);
			
			// Set transition properties
			TransNode->CrossfadeDuration = Trans.Duration;
			TransNode->PriorityOrder = Trans.Priority;
			TransNode->Bidirectional = Trans.bBidirectional;
			
			// Handle auto-rule
			if (Trans.bAutoRule)
			{
				TransNode->bAutomaticRuleBasedOnSequencePlayerInState = true;
				TransNode->AutomaticRuleTriggerTime = Trans.AutoRuleTriggerTime;
			}
			// Restore full transition condition graph from BlueprintLisp DSL
			else if (!Trans.RuleGraph.IsEmpty())
			{
				UEdGraph* BoundGraph = TransNode->GetBoundGraph();
				if (BoundGraph)
				{
					FBlueprintLispConverter::FImportOptions LispOpts;
					LispOpts.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
					LispOpts.bAutoLayout = false;
					LispOpts.bCompile = false;
					FBlueprintLispResult LispResult = FBlueprintLispConverter::ImportGraph(
						BoundGraph, Trans.RuleGraph, LispOpts);
					if (LispResult.bSuccess)
					{
						for (UEdGraphNode* RuleNode : BoundGraph->Nodes)
						{
							if (UK2Node_AnimGetter* AnimGetter = Cast<UK2Node_AnimGetter>(RuleNode))
							{
								AnimGetter->SourceStateNode = *FromStatePtr;
								AnimGetter->SourceNode = SMNode;
								AnimGetter->SourceAnimBlueprint = Cast<UAnimBlueprint>(FBlueprintEditorUtils::FindBlueprintForGraph(BoundGraph));
								if (UFunction* GetterFunction = AnimGetter->GetTargetFunction())
								{
									AnimGetter->GetterClass = GetterFunction->GetOwnerClass();
								}
							}
						}
						UE_LOG(LogAnimBPImporter, Log, TEXT("Restored rule-graph for transition %s -> %s"),
							*Trans.FromState, *Trans.ToState);
					}
					else
					{
						UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:RuleGraphImport] Transition %s -> %s: BlueprintLisp ImportGraph failed: %s — transition will never fire"),
							*Trans.FromState, *Trans.ToState, *LispResult.Error);
					}
				}
				else
				{
					UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:RuleGraphImport] Transition %s -> %s: GetBoundGraph() returned null"),
						*Trans.FromState, *Trans.ToState);
				}
			}
			// Log ref-based rules with no rule-graph — cannot restore
			else if (!Trans.RuleRef.IsEmpty())
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:EventGraphConnection] Transition %s -> %s: rule %s is driven by EventGraph nodes and no :rule-graph was exported — transition will never fire"),
					*Trans.FromState, *Trans.ToState, *Trans.RuleRef);
			}
			
			// Track bidirectional pairs
			if (Trans.bBidirectional)
			{
				FString ForwardKey = FString::Printf(TEXT("%s|%s"), *Trans.FromState, *Trans.ToState);
				CreatedBidirectionalPairs.Add(ForwardKey);
			}
			
			CreatedCount++;
			UE_LOG(LogAnimBPImporter, Log, TEXT("Created transition: %s -> %s (duration=%.2f, priority=%d, bidirectional=%s)"),
				*Trans.FromState, *Trans.ToState, Trans.Duration, Trans.Priority, Trans.bBidirectional ? TEXT("true") : TEXT("false"));
		}
		
		UE_LOG(LogAnimBPImporter, Log, TEXT("State machine: %d/%d transitions created"), CreatedCount, ParsedTransitions.Num());
	}
	
	return true;
}

// ========== Graph Building ==========

bool FAnimBPImporter::BuildAnimGraph(
	UAnimBlueprint* Blueprint,
	const TSharedPtr<FAnimGraphAST>& AST,
	FString* OutError)
{
	if (!Blueprint || !AST.IsValid()) return false;
	FImporterRigValidationContext RigValidationContext;
	TGuardValue<FImporterRigValidationContext*> RigValidationGuard(
		GActiveImporterRigValidationContext, &RigValidationContext);
	
	UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
	const AnimBP2FPImportLifecycle::FImportLifecycleContext LifecycleContext =
		MakeAnimLifecycleContext(Blueprint, AnimGraph, true, false, false, true);
	TArray<AnimBP2FPImportLifecycle::FImportPropertyChange> PropertyChanges;
	TSet<UEdGraphNode*> PreExistingNodes;
	if (AnimGraph)
	{
		for (UEdGraphNode* ExistingNode : AnimGraph->Nodes)
		{
			if (ExistingNode)
			{
				PreExistingNodes.Add(ExistingNode);
			}
		}
	}
	BroadcastNodeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PreNodeChanges, LifecycleContext, {});
	BroadcastPropertyLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PrePropertyChanges, LifecycleContext, PropertyChanges);
	if (!AnimGraph)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("Could not find AnimGraph"));
		return false;
	}

	// Implement AnimLayerInterfaces declared in :implements [...]
	// This must happen before building nodes so SkeletonGeneratedClass has the layer functions
	for (const FString& InterfacePath : AST->ImplementedInterfaces)
	{
		// Check if already implemented
		bool bAlreadyImplemented = false;
		for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
		{
			if (Desc.Interface && Desc.Interface->GetPathName() == InterfacePath)
			{
				bAlreadyImplemented = true;
				break;
			}
		}
		if (bAlreadyImplemented)
		{
			UE_LOG(LogAnimBPImporter, Log, TEXT("Interface already implemented: %s"), *InterfacePath);
			continue;
		}

		// Load the interface class
		UClass* InterfaceClass = LoadObject<UClass>(nullptr, *InterfacePath);
		if (!InterfaceClass)
		{
			// Try stripping _C suffix and loading the generated class
			FString TrimPath = InterfacePath;
			if (TrimPath.EndsWith(TEXT("_C")))
			{
				TrimPath.RemoveFromEnd(TEXT("_C"));
				InterfaceClass = LoadObject<UClass>(nullptr, *(TrimPath + TEXT("_C")));
			}
		}
		if (!InterfaceClass)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:ImplementInterface] Could not load interface class '%s'"),
				*InterfacePath);
			continue;
		}

		// Use FTopLevelAssetPath from the class's path
		FTopLevelAssetPath InterfaceAssetPath(InterfaceClass->GetPathName());
		bool bOk = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceAssetPath);
		if (bOk)
		{
			UE_LOG(LogAnimBPImporter, Log, TEXT("Implemented interface: %s"), *InterfacePath);
		}
		else
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:ImplementInterface] Failed to implement interface '%s' — LinkedAnimLayer nodes for this interface will report 'invalid layer'"),
				*InterfacePath);
		}
	}
	
	// Recompile skeleton so SkeletonGeneratedClass is up to date before building nodes
	if (AST->ImplementedInterfaces.Num() > 0)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	}
	
	// Build variables and helper bridges
	if (!BuildVariables(Blueprint, AST->Variables))
	{
		return false;
	}
	FString MapDefaultError;
	if (!ApplyMapVariableDefaults(Blueprint, AST->Variables, MapDefaultError))
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:VariableDefault] %s"), *MapDefaultError);
		if (OutError) *OutError = MapDefaultError;
		return false;
	}
	if (!BuildGeneratedVars(Blueprint, AST->HelperGraphs)
		|| !BuildHelperGraphs(Blueprint, AST->HelperGraphs))
	{
		return false;
	}
	if (!BuildLogicGraphs(Blueprint, AST->LogicGraphs, AST->bHasLogicGraphsBlock))
	{
		return false;
	}

	TMap<FString, FHelperGraphDef> HelperGraphLookup;
	for (const FHelperGraphDef& Helper : AST->HelperGraphs)
	{
		if (!Helper.Id.IsEmpty())
		{
			HelperGraphLookup.Add(Helper.Id, Helper);
		}
	}
	if (!BuildAnimationLayers(Blueprint, AST->AnimationLayers, &HelperGraphLookup))
	{
		return false;
	}
	if (AST->AnimationLayers.Num() > 0)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint,
			EBlueprintCompileOptions::RegenerateSkeletonOnly
			| EBlueprintCompileOptions::SkipGarbageCollection
			| EBlueprintCompileOptions::SkipSave);
	}
	
	// Build defines (SaveCachedPose nodes)
	TMap<FString, UAnimGraphNode_SaveCachedPose*> DefineNodesRaw;

	// Pass 1: create and register all defines first so forward refs can resolve
	for (const FCachedPoseDef& Def : AST->Defines)
	{
		UAnimGraphNode_SaveCachedPose* SaveNode = NewObject<UAnimGraphNode_SaveCachedPose>(AnimGraph);
		if (SaveNode)
		{
			SaveNode->CreateNewGuid();
			SaveNode->PostPlacedNewNode();
			SaveNode->AllocateDefaultPins();
			AnimGraph->AddNode(SaveNode, false, false);

			// Set the cache name and register before building any body subtrees
			SaveNode->CacheName = Def.Name;
			DefineNodesRaw.Add(Def.GetIdentifier(), SaveNode);
			UE_LOG(LogAnimBPImporter, Log, TEXT("Created define: %s"), *Def.Name);
		}
	}

	// Pass 2: build each define body with the full define map available
	for (const FCachedPoseDef& Def : AST->Defines)
	{
		UAnimGraphNode_SaveCachedPose* SaveNode = DefineNodesRaw.FindRef(Def.GetIdentifier());
		if (!SaveNode)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[INTERNAL] BuildAnimGraph: define '%s' not found in DefineNodesRaw after Pass-1 registration — skipping body build"),
				*Def.GetIdentifier());
			continue;
		}
		if (!Def.Body.IsValid())
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:EmptyDefineBody] define '%s' has null body — SaveCachedPose node will have no input"),
				*Def.Name);
			continue;
		}

		UAnimGraphNode_Base* BodyNode = BuildAnimNode(Def.Body, AnimGraph, &DefineNodesRaw, &HelperGraphLookup);
		if (!BodyNode && Def.Body->NodeType != TEXT("identity-pose"))
		{
			return false;
		}
		if (BodyNode)
		{
			UEdGraphPin* BodyOutput = FindOutputPosePin(BodyNode);
			// Find SaveCachedPose's input pose pin
			UEdGraphPin* SaveInput = nullptr;
			for (UEdGraphPin* Pin : SaveNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && 
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					SaveInput = Pin;
					break;
				}
			}
			if (BodyOutput && SaveInput)
			{
				ConnectPins(BodyOutput, SaveInput);
			}
		}
	}
	
	// Build the root animation tree
	if (AST->RootNode.IsValid())
	{
		UAnimGraphNode_Base* RootTree = BuildAnimNode(AST->RootNode, AnimGraph, &DefineNodesRaw, &HelperGraphLookup);
		if (!RootTree && AST->RootNode->NodeType != TEXT("identity-pose"))
		{
			return false;
		}
		
		if (RootTree)
		{
			// Find the AnimGraph's root node (already created by the factory)
			UAnimGraphNode_Root* RootNode = nullptr;
			for (UEdGraphNode* Node : AnimGraph->Nodes)
			{
				RootNode = Cast<UAnimGraphNode_Root>(Node);
				if (RootNode) break;
			}
			
			if (RootNode)
			{
				UEdGraphPin* TreeOutput = FindOutputPosePin(RootTree);
				UEdGraphPin* RootInput = nullptr;
				for (UEdGraphPin* Pin : RootNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input && 
						Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						RootInput = Pin;
						break;
					}
				}
				if (TreeOutput && RootInput)
				{
					ConnectPins(TreeOutput, RootInput);
				}
			}
		}
	}
	
	TArray<AnimBP2FPImportLifecycle::FImportNodeChange> NodeChanges;
	CollectAnimNodeChanges(PreExistingNodes, AnimGraph, NodeChanges);
	BroadcastNodeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PostNodeChanges, LifecycleContext, NodeChanges);
	BroadcastPropertyLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PostPropertyChanges, LifecycleContext, PropertyChanges);
	return true;
}

// ========== Compilation ==========

void FAnimBPImporter::BroadcastNodeLifecycle(
	AnimBP2FPImportLifecycle::EImportLifecyclePhase Phase,
	const AnimBP2FPImportLifecycle::FImportLifecycleContext& Context,
	const TArray<AnimBP2FPImportLifecycle::FImportNodeChange>& Changes)
{
	if (!FAnimBP2FPModule::IsAvailable())
	{
		return;
	}

	AnimBP2FPImportLifecycle::FImportNodePhaseEvent Event;
	Event.Phase = Phase;
	Event.Context = Context;
	Event.Changes = Changes;
	FAnimBP2FPModule::Get().BroadcastNodePhase(Event);
}

void FAnimBPImporter::BroadcastPropertyLifecycle(
	AnimBP2FPImportLifecycle::EImportLifecyclePhase Phase,
	const AnimBP2FPImportLifecycle::FImportLifecycleContext& Context,
	const TArray<AnimBP2FPImportLifecycle::FImportPropertyChange>& Changes)
{
	if (!FAnimBP2FPModule::IsAvailable())
	{
		return;
	}

	AnimBP2FPImportLifecycle::FImportPropertyPhaseEvent Event;
	Event.Phase = Phase;
	Event.Context = Context;
	Event.Changes = Changes;
	FAnimBP2FPModule::Get().BroadcastPropertyPhase(Event);
}

void FAnimBPImporter::BroadcastFinalizeLifecycle(
	AnimBP2FPImportLifecycle::EImportLifecyclePhase Phase,
	const AnimBP2FPImportLifecycle::FImportLifecycleContext& Context)
{
	if (!FAnimBP2FPModule::IsAvailable())
	{
		return;
	}

	AnimBP2FPImportLifecycle::FImportFinalizePhaseEvent Event;
	Event.Phase = Phase;
	Event.Context = Context;
	FAnimBP2FPModule::Get().BroadcastFinalizePhase(Event);
}

bool FAnimBPImporter::CompileBlueprint(UAnimBlueprint* Blueprint, FString* OutError)
{
	if (!Blueprint)
	{
		if (OutError) *OutError = TEXT("Null blueprint");
		return false;
	}
	
	Blueprint->Modify();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	
	if (Blueprint->Status == BS_Error)
	{
		if (OutError) *OutError = TEXT("Blueprint has compilation errors after import");
		return false;
	}
	
	return true;
}

// ========== Top-Level API ==========

FAnimLispBundleImportResult FAnimBPImporter::ImportBundle(
	const TArray<FAnimLispBundleSource>& Sources,
	const FAnimLispBundleImportOptions& Options)
{
	FAnimLispBundleImportResult Result;
	if (Sources.IsEmpty())
	{
		Result.Diagnostics.Add(
			EAnimLangDiagSeverity::Error,
			EAnimLangDiagCategory::Module,
			TEXT("Bundle import requires at least one source module"));
		return Result;
	}
	if (Options.TargetRoot.IsEmpty())
	{
		Result.Diagnostics.Add(
			EAnimLangDiagSeverity::Error,
			EAnimLangDiagCategory::Import,
			TEXT("Bundle import requires an explicit target root"));
		return Result;
	}

	FAnimLispWorkspace Workspace;
	for (const FAnimLispBundleSource& Source : Sources)
	{
		Workspace.AddSource(Source.SourceFile, Source.Source);
	}
	if (!Workspace.Build(Result.Diagnostics, Options.Mode == EAnimLispBundleImportMode::Legacy)
		|| !Workspace.BuildImportPlan(Result.Plan, Result.Diagnostics))
	{
		Result.Plan.Reset();
		return Result;
	}
	TMap<FString, FString> PersistentTargetByModule;
	if (Options.bCommitPersistent)
	{
		if (!Options.TargetRoot.StartsWith(TEXT("/Game/")) && Options.TargetRoot != TEXT("/Game"))
		{
			Result.Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Import,
				TEXT("Persistent bundle target root must be under /Game"));
			return Result;
		}
		TSet<FString> UniqueTargets;
		for (const FAnimLispImportPlanEntry& Entry : Result.Plan)
		{
			const FString AssetName = Entry.ModuleId.Kind == EAnimLispModuleKind::Anim && Entry.AnimAST.IsValid()
				? Entry.AnimAST->Name : FPaths::GetBaseFilename(Entry.ModuleId.AssetPath);
			const FString TargetPackage = Options.TargetRoot / AssetName;
			if (UniqueTargets.Contains(TargetPackage)
				|| FindPackage(nullptr, *TargetPackage)
				|| FPackageName::DoesPackageExist(TargetPackage))
			{
				Result.Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Import,
					FString::Printf(TEXT("Persistent bundle target already exists or collides: '%s'"), *TargetPackage));
				return Result;
			}
			UniqueTargets.Add(TargetPackage);
			PersistentTargetByModule.Add(Entry.ModuleId.ToString(), TargetPackage);
		}
	}

	auto IsLegacyAnimEntry = [](const FAnimLispImportPlanEntry& Entry)
	{
		if (!Entry.AnimAST.IsValid()) return false;
		if (Entry.AnimAST->RigImports.ContainsByPredicate(
			[](const FAnimLispImport& Import) { return Import.bLegacyExternal; })) return true;
		bool bLegacyControlRig = false;
		Entry.AnimAST->VisitNodes([&bLegacyControlRig](const TSharedPtr<FAnimNodeAST>& Node)
		{
			bLegacyControlRig |= Node.IsValid()
				&& Node->NodeType == TEXT("control-rig")
				&& (!Node->RigBinding.IsSet()
					|| (Node->Coverage == EAnimNodeCoverage::Lossy
						&& Node->RigBinding->EntryName.IsEmpty()));
		});
		return bLegacyControlRig;
	};

	TSet<FString> VerifiedLegacyExternalRigPaths;
	for (const FAnimLispImportPlanEntry& Entry : Result.Plan)
	{
		if (Entry.ModuleId.Kind != EAnimLispModuleKind::Anim || !Entry.AnimAST.IsValid()) continue;
		if (!IsLegacyAnimEntry(Entry)) continue;
		if (Options.Mode == EAnimLispBundleImportMode::Strict)
		{
			Result.Diagnostics.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Import,
				FString::Printf(
					TEXT("Strict bundle rejects legacy Control Rig fallback in '%s'; import-rig and typed binding are required"),
					*Entry.SourceFile));
			return Result;
		}
		Result.Diagnostics.Add(
			EAnimLangDiagSeverity::Warning,
			EAnimLangDiagCategory::RoundTrip,
			FString::Printf(
				TEXT("Legacy Control Rig fallback in '%s' has non-exact bundle coverage"),
				*Entry.SourceFile));

		for (const FAnimLispImport& Import : Entry.AnimAST->RigImports)
		{
			if (!Import.bLegacyExternal) continue;
			const FString ObjectPath = Import.Target.AssetPath + TEXT(".")
				+ FPaths::GetBaseFilename(Import.Target.AssetPath);
			if (!LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath))
			{
				Result.Diagnostics.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Import,
					FString::Printf(
						TEXT("Legacy Control Rig fallback target '%s' does not exist or is not a Control Rig Blueprint"),
						*Import.Target.AssetPath),
					Import.Location);
				return Result;
			}
			VerifiedLegacyExternalRigPaths.Add(Import.Target.AssetPath);
		}
	}

	auto DiscardStagedAssets = [&Result]()
	{
		for (UObject* Asset : Result.StagedAssets)
		{
			if (!Asset) continue;
			Asset->ClearFlags(RF_Public | RF_Standalone);
		}
		Result.StagedAssets.Reset();
	};

	FAnimBPImportContext AnimContext;
	AnimContext.bStrictBundle = Options.Mode == EAnimLispBundleImportMode::Strict;
	AnimContext.bAllowLegacyRigFallback = Options.Mode == EAnimLispBundleImportMode::Legacy;
	AnimContext.LegacyExternalRigPaths = MoveTemp(VerifiedLegacyExternalRigPaths);
	AnimContext.bTransient = true;
	TArray<FAnimLispModuleId> StagedModuleIds;

	// Every Rig must compile and pass its immediate semantic re-export gate before Anim staging starts.
	for (const FAnimLispImportPlanEntry& Entry : Result.Plan)
	{
		if (Entry.ModuleId.Kind != EAnimLispModuleKind::Rig) continue;
		if (!Entry.RigAST.IsValid())
		{
			Result.Diagnostics.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Import,
				FString::Printf(TEXT("Rig compile gate has no parsed AST for '%s'"), *Entry.ModuleId.ToString()));
			DiscardStagedAssets();
			return Result;
		}
		FRigLangImportOptions RigOptions;
		RigOptions.TargetPackage = FString::Printf(
			TEXT("/Engine/Transient/AnimLispBundle_%s_%s"),
			*FPaths::GetBaseFilename(Entry.ModuleId.AssetPath),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		RigOptions.bTransient = true;
		RigOptions.bStrict = Options.Mode == EAnimLispBundleImportMode::Strict;
		FRigLangImportResult RigResult = FRigLangImporter::Import(*Entry.RigAST, RigOptions);
		Result.Diagnostics.Items.Append(RigResult.Diagnostics.Items);
		if (!RigResult.Blueprint || !RigResult.bCompiled || RigResult.Diagnostics.HasErrors())
		{
			if (RigResult.Blueprint)
			{
				RigResult.Blueprint->ClearFlags(RF_Public | RF_Standalone);
			}
			Result.Diagnostics.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Import,
				FString::Printf(
					TEXT("Rig compile/diff gate failed for '%s'"),
					*Entry.ModuleId.ToString()));
			DiscardStagedAssets();
			return Result;
		}
		Result.StagedAssets.Add(RigResult.Blueprint.Get());
		StagedModuleIds.Add(Entry.ModuleId);
		FAnimBPResolvedRig& ResolvedRig = AnimContext.ResolvedRigs.Add(Entry.ModuleId.AssetPath);
		ResolvedRig.Blueprint = RigResult.Blueprint.Get();
		ResolvedRig.Module = Entry.RigAST;
	}

	for (const FAnimLispImportPlanEntry& Entry : Result.Plan)
	{
		if (Entry.ModuleId.Kind != EAnimLispModuleKind::Anim) continue;
		if (!Entry.AnimAST.IsValid())
		{
			Result.Diagnostics.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Import,
				FString::Printf(TEXT("Anim staging has no parsed AST for '%s'"), *Entry.ModuleId.ToString()));
			DiscardStagedAssets();
			return Result;
		}
		FString AnimError;
		UAnimBlueprint* AnimBlueprint = ImportFromAST(
			ConstCastSharedPtr<FAnimGraphAST>(Entry.AnimAST),
			Options.TargetRoot,
			AnimContext,
			&AnimError);
		if (!AnimBlueprint)
		{
			Result.Diagnostics.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Import,
				FString::Printf(TEXT("Anim compile gate failed for '%s': %s"),
					*Entry.ModuleId.ToString(), *AnimError));
			DiscardStagedAssets();
			return Result;
		}
		TSharedPtr<FAnimGraphAST> ReexportedAnim = FAnimBPExporter::ExportToAST(AnimBlueprint);
		const FString ExpectedCanonical = NormalizeComparableAnimEscapes(Entry.AnimAST->ToString());
		const FString ActualCanonical = BuildComparableAnimCanonical(ReexportedAnim, *Entry.AnimAST);
		const bool bRequireExactCanonical = Options.Mode == EAnimLispBundleImportMode::Strict
			|| !IsLegacyAnimEntry(Entry);
		if (!ReexportedAnim.IsValid() || (bRequireExactCanonical && ActualCanonical != ExpectedCanonical))
		{
			int32 Mismatch = 0;
			const int32 CommonLength = FMath::Min(ActualCanonical.Len(), ExpectedCanonical.Len());
			while (Mismatch < CommonLength && ActualCanonical[Mismatch] == ExpectedCanonical[Mismatch]) ++Mismatch;
			Result.Diagnostics.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::RoundTrip,
				FString::Printf(
					TEXT("Anim canonical re-export gate failed for '%s' at %d; expected '%s', actual '%s'"),
					*Entry.ModuleId.ToString(), Mismatch,
					*ExpectedCanonical.Mid(FMath::Max(0, Mismatch - 100), 200),
					*ActualCanonical.Mid(FMath::Max(0, Mismatch - 100), 200)));
			AnimBlueprint->ClearFlags(RF_Public | RF_Standalone);
			DiscardStagedAssets();
			return Result;
		}
		Result.StagedAssets.Add(AnimBlueprint);
		StagedModuleIds.Add(Entry.ModuleId);
	}

	if (Options.bCommitPersistent)
	{
		TArray<TObjectPtr<UObject>> CommittedAssets;
		FAnimBPImportContext CommittedContext;
		CommittedContext.bStrictBundle = Options.Mode == EAnimLispBundleImportMode::Strict;
		CommittedContext.bAllowLegacyRigFallback = Options.Mode == EAnimLispBundleImportMode::Legacy;
		CommittedContext.LegacyExternalRigPaths = AnimContext.LegacyExternalRigPaths;
		CommittedContext.bTransient = false;
		TArray<FString> PersistentPackageNames;
		PersistentTargetByModule.GenerateValueArray(PersistentPackageNames);
		PersistentPackageNames.Sort();
		TArray<FString> PersistentFilenames;
		for (const FString& PackageName : PersistentPackageNames)
		{
			PersistentFilenames.Add(FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension()));
		}
		auto RollbackPersistentAssets = [&]()
		{
			TArray<UPackage*> PackagesToUnload;
			for (const FString& PackageName : PersistentPackageNames)
			{
				UPackage* Package = FindPackage(nullptr, *PackageName);
				if (!Package) continue;
				Package->SetDirtyFlag(false);
				PackagesToUnload.Add(Package);
			}
			CommittedContext.ResolvedRigs.Reset();
			CommittedAssets.Reset();
			FText UnloadError;
			if (!PackagesToUnload.IsEmpty()
				&& !UPackageTools::UnloadPackages(PackagesToUnload, UnloadError, true))
			{
				for (UPackage* Package : PackagesToUnload)
				{
					if (!Package || Package->HasAnyInternalFlags(EInternalObjectFlags::Garbage)) continue;
					const FString OriginalName = Package->GetName();
					const FString DiscardedName = FString::Printf(
						TEXT("/Engine/Transient/AnimLispRollback_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits));
					if (!Package->Rename(*DiscardedName, nullptr,
						REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty))
					{
						Result.Diagnostics.Add(EAnimLangDiagSeverity::Warning, EAnimLangDiagCategory::Import,
							FString::Printf(TEXT("Rollback could not release in-memory package identity '%s': %s"),
								*OriginalName, *UnloadError.ToString()));
					}
				}
			}
			for (const FString& Filename : PersistentFilenames)
			{
				IFileManager::Get().Delete(*Filename, false, true);
			}
		};
		bool bPersistentBuildSucceeded = true;
		for (const FAnimLispImportPlanEntry& Entry : Result.Plan)
		{
			if (Entry.ModuleId.Kind != EAnimLispModuleKind::Rig) continue;
			const FString* TargetPackage = PersistentTargetByModule.Find(Entry.ModuleId.ToString());
			if (!TargetPackage || !Entry.RigAST.IsValid()) { bPersistentBuildSucceeded = false; break; }
			Result.bMutationStarted = true;
			FRigLangImportOptions RigOptions;
			RigOptions.TargetPackage = *TargetPackage;
			RigOptions.bTransient = false;
			RigOptions.bStrict = Options.Mode == EAnimLispBundleImportMode::Strict;
			FRigLangImportResult RigResult = FRigLangImporter::Import(*Entry.RigAST, RigOptions);
			Result.Diagnostics.Items.Append(RigResult.Diagnostics.Items);
			if (!RigResult.Blueprint || !RigResult.bCompiled || RigResult.Diagnostics.HasErrors())
			{
				bPersistentBuildSucceeded = false;
				break;
			}
			CommittedAssets.Add(RigResult.Blueprint.Get());
			FAnimBPResolvedRig& Resolved = CommittedContext.ResolvedRigs.Add(Entry.ModuleId.AssetPath);
			Resolved.Blueprint = RigResult.Blueprint.Get();
			Resolved.Module = Entry.RigAST;
		}
		for (const FAnimLispImportPlanEntry& Entry : Result.Plan)
		{
			if (!bPersistentBuildSucceeded || Entry.ModuleId.Kind != EAnimLispModuleKind::Anim) continue;
			Result.bMutationStarted = true;
			FString AnimError;
			UAnimBlueprint* AnimBlueprint = ImportFromAST(
				ConstCastSharedPtr<FAnimGraphAST>(Entry.AnimAST), Options.TargetRoot, CommittedContext, &AnimError);
			if (!AnimBlueprint)
			{
				Result.Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Import,
					FString::Printf(TEXT("Persistent Anim build failed for '%s': %s"),
						*Entry.ModuleId.ToString(), *AnimError));
				bPersistentBuildSucceeded = false;
				break;
			}
			TSharedPtr<FAnimGraphAST> ReexportedAnim = FAnimBPExporter::ExportToAST(AnimBlueprint);
			const FString ExpectedCanonical = NormalizeComparableAnimEscapes(Entry.AnimAST->ToString());
			const FString ActualCanonical = BuildComparableAnimCanonical(ReexportedAnim, *Entry.AnimAST);
			const bool bRequireExactCanonical = Options.Mode == EAnimLispBundleImportMode::Strict
				|| !IsLegacyAnimEntry(Entry);
			if (!ReexportedAnim.IsValid() || (bRequireExactCanonical && ActualCanonical != ExpectedCanonical))
			{
				Result.Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::RoundTrip,
					FString::Printf(TEXT("Persistent Anim canonical re-export gate failed for '%s'"),
						*Entry.ModuleId.ToString()));
				AnimBlueprint->ClearFlags(RF_Public | RF_Standalone);
				bPersistentBuildSucceeded = false;
				break;
			}
			CommittedAssets.Add(AnimBlueprint);
		}
		if (!bPersistentBuildSucceeded || CommittedAssets.Num() != Result.StagedAssets.Num())
		{
			RollbackPersistentAssets();
			Result.Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Import,
				TEXT("Persistent bundle rebuild failed before package save"));
			DiscardStagedAssets();
			return Result;
		}
		for (int32 SaveIndex = 0; SaveIndex < CommittedAssets.Num(); ++SaveIndex)
		{
			UObject* Committed = CommittedAssets[SaveIndex];
			UPackage* Package = Committed ? Committed->GetOutermost() : nullptr;
			const FString PackageName = Package ? Package->GetName() : FString();
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			bool bInjectSaveFailure = false;
#if WITH_DEV_AUTOMATION_TESTS
			bInjectSaveFailure = Options.TestFailSaveIndex == SaveIndex;
#endif
			if (bInjectSaveFailure || !Package || !UPackage::SavePackage(Package, Committed, *Filename, SaveArgs))
			{
				RollbackPersistentAssets();
				Result.Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Import,
					TEXT("Persistent bundle save failed; newly created package files were rolled back"));
				DiscardStagedAssets();
				return Result;
			}
		}
		DiscardStagedAssets();
		Result.StagedAssets = MoveTemp(CommittedAssets);
	}

	Result.bSuccess = true;
	return Result;
}

UAnimBlueprint* FAnimBPImporter::Import(const FString& DSLCode, const FString& PackagePath, FString* OutError)
{
	// Parse the DSL
	TArray<FAnimLangParseError> ParseErrors;
	TSharedPtr<FAnimGraphAST> AST = FAnimLangParser::Parse(DSLCode, ParseErrors);
	
	if (!AST.IsValid() || ParseErrors.ContainsByPredicate(
		[](const FAnimLangParseError& Error) { return !Error.bWarning; }))
	{
		if (OutError)
		{
			*OutError = TEXT("Failed to parse DSL code");
			for (const FAnimLangParseError& Err : ParseErrors)
			{
				*OutError += TEXT("\n  ") + Err.ToString();
			}
		}
		return nullptr;
	}
	
	return ImportFromAST(AST, PackagePath, OutError);
}

UAnimBlueprint* FAnimBPImporter::ImportFromAST(
	const TSharedPtr<FAnimGraphAST>& AST,
	const FString& PackagePath,
	const FAnimBPImportContext& Context,
	FString* OutError)
{
	TGuardValue<const FAnimBPImportContext*> ContextGuard(GActiveAnimImportContext, &Context);
	return ImportFromAST(AST, PackagePath, OutError);
}

UAnimBlueprint* FAnimBPImporter::ImportFromAST(const TSharedPtr<FAnimGraphAST>& AST, const FString& PackagePath, FString* OutError)
{
	if (!AST.IsValid())
	{
		if (OutError) *OutError = TEXT("Null AST");
		return nullptr;
	}

	FString DependencyError;
	if (!ValidateExternalDependencies(AST->Dependencies, DependencyError))
	{
		if (OutError) *OutError = DependencyError;
		UE_LOG(LogAnimBPImporter, Error, TEXT("%s"), *DependencyError);
		return nullptr;
	}
	
	// Create the blueprint
	UAnimBlueprint* Blueprint = CreateEmptyBlueprint(
		PackagePath,
		AST->Name,
		AST->SkeletonPath,
		GActiveAnimImportContext && GActiveAnimImportContext->bTransient);
	if (!Blueprint)
	{
		if (OutError) *OutError = TEXT("Failed to create empty blueprint");
		return nullptr;
	}
	
	// Build the animation graph
	FString BuildError;
	if (!BuildAnimGraph(Blueprint, AST, &BuildError))
	{
		if (OutError) *OutError = BuildError.IsEmpty() ? TEXT("Failed to build animation graph") : BuildError;
		return nullptr;
	}

	UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
	const AnimBP2FPImportLifecycle::FImportLifecycleContext LifecycleContext =
		MakeAnimLifecycleContext(Blueprint, AnimGraph, true, false, true, true);
	
	// Compile
	FString CompileError;
	BroadcastFinalizeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PreFinalize, LifecycleContext);
	if (!CompileBlueprint(Blueprint, &CompileError))
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("Compilation failed: %s"), *CompileError);
		if (OutError) *OutError = CompileError;
		return nullptr;
	}
	FString MetadataError;
	if (!ApplyAnimBlueprintMetadata(Blueprint, AST->Metadata, MetadataError))
	{
		if (OutError) *OutError = MetadataError;
		return nullptr;
	}
	BroadcastFinalizeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PostFinalize, LifecycleContext);
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("Successfully imported '%s' from DSL (%d variables, %d defines)"),
		*AST->Name, AST->Variables.Num(), AST->Defines.Num());
	
	return Blueprint;
}

// ========== AnimGraph Clearing ==========

void FAnimBPImporter::ClearAnimGraph(UEdGraph* AnimGraph)
{
	if (!AnimGraph) return;
	
	// Collect all nodes except the Root node (which is persistent)
	TArray<UEdGraphNode*> NodesToRemove;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (!Cast<UAnimGraphNode_Root>(Node))
		{
			NodesToRemove.Add(Node);
		}
	}
	
	// Break all connections on root node's input pins first
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node))
		{
			for (UEdGraphPin* Pin : Root->Pins)
			{
				if (Pin) Pin->BreakAllPinLinks();
			}
			break;
		}
	}
	
	// Remove non-root nodes
	for (UEdGraphNode* Node : NodesToRemove)
	{
		// Break all pin links
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin) Pin->BreakAllPinLinks();
		}
		AnimGraph->RemoveNode(Node);
	}
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("Cleared AnimGraph: removed %d nodes"), NodesToRemove.Num());
}

void FAnimBPImporter::ClearDefines(UAnimBlueprint* Blueprint)
{
	if (!Blueprint) return;
	
	UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
	if (!AnimGraph) return;
	
	// SaveCachedPose nodes are in the AnimGraph — they were cleared by ClearAnimGraph
	// This method is here for future use if defines get stored elsewhere
}

bool FAnimBPImporter::RebuildAnimGraph(UAnimBlueprint* Blueprint, const TSharedPtr<FAnimGraphAST>& NewAST)
{
	if (!Blueprint || !NewAST.IsValid()) return false;
	FImporterRigValidationContext RigValidationContext;
	TGuardValue<FImporterRigValidationContext*> RigValidationGuard(
		GActiveImporterRigValidationContext, &RigValidationContext);
	
	UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
	const AnimBP2FPImportLifecycle::FImportLifecycleContext LifecycleContext =
		MakeAnimLifecycleContext(Blueprint, AnimGraph, true, false, false, true);
	TArray<AnimBP2FPImportLifecycle::FImportPropertyChange> PropertyChanges;
	TSet<UEdGraphNode*> PreExistingNodes;
	if (AnimGraph)
	{
		for (UEdGraphNode* ExistingNode : AnimGraph->Nodes)
		{
			if (ExistingNode)
			{
				PreExistingNodes.Add(ExistingNode);
			}
		}
	}
	BroadcastNodeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PreNodeChanges, LifecycleContext, {});
	BroadcastPropertyLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PrePropertyChanges, LifecycleContext, PropertyChanges);
	if (!AnimGraph)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("RebuildAnimGraph: Could not find AnimGraph"));
		return false;
	}

	// Implement new AnimLayerInterfaces declared in :implements [...]
	bool bAddedInterfaces = false;
	for (const FString& InterfacePath : NewAST->ImplementedInterfaces)
	{
		bool bAlreadyImplemented = false;
		for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
		{
			if (Desc.Interface && Desc.Interface->GetPathName() == InterfacePath)
			{
				bAlreadyImplemented = true;
				break;
			}
		}
		if (bAlreadyImplemented) continue;

		UClass* InterfaceClass = LoadObject<UClass>(nullptr, *InterfacePath);
		if (!InterfaceClass)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:ImplementInterface] RebuildAnimGraph: Could not load interface class '%s'"),
				*InterfacePath);
			continue;
		}

		FTopLevelAssetPath InterfaceAssetPath(InterfaceClass->GetPathName());
		bool bOk = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceAssetPath);
		if (bOk)
		{
			UE_LOG(LogAnimBPImporter, Log, TEXT("RebuildAnimGraph: Implemented interface: %s"), *InterfacePath);
			bAddedInterfaces = true;
		}
		else
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[SKIP:ImplementInterface] RebuildAnimGraph: Failed to implement interface '%s'"),
				*InterfacePath);
		}
	}
	if (bAddedInterfaces)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	}
	
	// Step 1: Clear all existing nodes (except Root)
	ClearAnimGraph(AnimGraph);
	ClearDefines(Blueprint);
	RemoveStaleManagedHelperGraphs(Blueprint, NewAST->HelperGraphs);
	
	// Step 2: Rebuild using the standard BuildAnimGraph logic
	// Note: Variables already exist in the blueprint — we may need to add new ones
	// but we don't remove existing user variables to avoid breaking EventGraph references
	
	// Add any new variables that don't already exist
	for (const FVariableDef& Var : NewAST->Variables)
	{
		int32 ExistingIndex = INDEX_NONE;
		for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
		{
			if (ExistingVar.VarName == FName(*Var.Name))
			{
				ExistingIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, ExistingVar.VarName);
				break;
			}
		}
		if (ExistingIndex == INDEX_NONE)
		{
			TArray<FVariableDef> SingleVar;
			SingleVar.Add(Var);
			if (!BuildVariables(Blueprint, SingleVar))
			{
				return false;
			}
		}
		else
		{
			FEdGraphPinType PinType;
			FString TypeError;
			if (!FAnimLangVariableCodec::BuildPinType(Var, PinType, TypeError))
			{
				UE_LOG(LogAnimBPImporter, Error, TEXT("[UNSUPPORTED:VariableType] Variable '%s': %s"), *Var.Name, *TypeError);
				return false;
			}
			Blueprint->NewVariables[ExistingIndex].VarType = PinType;
			Blueprint->NewVariables[ExistingIndex].DefaultValue = Var.DefaultValue;
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
	}
	if (!BuildGeneratedVars(Blueprint, NewAST->HelperGraphs)
		|| !BuildHelperGraphs(Blueprint, NewAST->HelperGraphs))
	{
		return false;
	}
	if (!BuildLogicGraphs(Blueprint, NewAST->LogicGraphs, NewAST->bHasLogicGraphsBlock))
	{
		return false;
	}
	RemoveStaleManagedGeneratedVars(Blueprint, NewAST->HelperGraphs);

	TMap<FString, FHelperGraphDef> HelperGraphLookup;
	for (const FHelperGraphDef& Helper : NewAST->HelperGraphs)
	{
		if (!Helper.Id.IsEmpty())
		{
			HelperGraphLookup.Add(Helper.Id, Helper);
		}
	}
	if (!BuildAnimationLayers(Blueprint, NewAST->AnimationLayers, &HelperGraphLookup))
	{
		return false;
	}
	if (NewAST->AnimationLayers.Num() > 0)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint,
			EBlueprintCompileOptions::RegenerateSkeletonOnly
			| EBlueprintCompileOptions::SkipGarbageCollection
			| EBlueprintCompileOptions::SkipSave);
	}
	
	// Step 3: Build defines (SaveCachedPose nodes)
	TMap<FString, UAnimGraphNode_SaveCachedPose*> DefineNodesRaw;

	// Pass 1: create and register all define nodes first
	for (const FCachedPoseDef& Def : NewAST->Defines)
	{
		UAnimGraphNode_SaveCachedPose* SaveNode = NewObject<UAnimGraphNode_SaveCachedPose>(AnimGraph);
		if (SaveNode)
		{
			SaveNode->CreateNewGuid();
			SaveNode->PostPlacedNewNode();
			SaveNode->AllocateDefaultPins();
			AnimGraph->AddNode(SaveNode, false, false);

			SaveNode->CacheName = Def.Name;
			DefineNodesRaw.Add(Def.GetIdentifier(), SaveNode);
		}
	}

	// Pass 2: build define bodies with full map available
	for (const FCachedPoseDef& Def : NewAST->Defines)
	{
		UAnimGraphNode_SaveCachedPose* SaveNode = DefineNodesRaw.FindRef(Def.GetIdentifier());
		if (!SaveNode)
		{
			UE_LOG(LogAnimBPImporter, Error, TEXT("[INTERNAL] RebuildAnimGraph: define '%s' not found in DefineNodesRaw after Pass-1 registration — skipping body build"),
				*Def.GetIdentifier());
			continue;
		}
		if (!Def.Body.IsValid())
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:EmptyDefineBody] define '%s' has null body — SaveCachedPose node will have no input"),
				*Def.Name);
			continue;
		}

		UAnimGraphNode_Base* BodyNode = BuildAnimNode(Def.Body, AnimGraph, &DefineNodesRaw, &HelperGraphLookup);
		if (!BodyNode && Def.Body->NodeType != TEXT("identity-pose"))
		{
			return false;
		}
		if (BodyNode)
		{
			UEdGraphPin* BodyOutput = FindOutputPosePin(BodyNode);
			UEdGraphPin* SaveInput = nullptr;
			for (UEdGraphPin* Pin : SaveNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && 
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					SaveInput = Pin;
					break;
				}
			}
			if (BodyOutput && SaveInput)
			{
				ConnectPins(BodyOutput, SaveInput);
			}
		}
	}
	
	// Step 4: Build the root animation tree
	if (NewAST->RootNode.IsValid())
	{
		UAnimGraphNode_Base* RootTree = BuildAnimNode(NewAST->RootNode, AnimGraph, &DefineNodesRaw, &HelperGraphLookup);
		if (!RootTree && NewAST->RootNode->NodeType != TEXT("identity-pose"))
		{
			return false;
		}
		
		if (RootTree)
		{
			UAnimGraphNode_Root* RootNode = nullptr;
			for (UEdGraphNode* Node : AnimGraph->Nodes)
			{
				RootNode = Cast<UAnimGraphNode_Root>(Node);
				if (RootNode) break;
			}
			
			if (RootNode)
			{
				UEdGraphPin* TreeOutput = FindOutputPosePin(RootTree);
				UEdGraphPin* RootInput = nullptr;
				for (UEdGraphPin* Pin : RootNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input && 
						Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						RootInput = Pin;
						break;
					}
				}
				if (TreeOutput && RootInput)
				{
					ConnectPins(TreeOutput, RootInput);
				}
			}
		}
	}
	
	TArray<AnimBP2FPImportLifecycle::FImportNodeChange> NodeChanges;
	CollectAnimNodeChanges(PreExistingNodes, AnimGraph, NodeChanges);
	BroadcastNodeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PostNodeChanges, LifecycleContext, NodeChanges);
	BroadcastPropertyLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PostPropertyChanges, LifecycleContext, PropertyChanges);
	return true;
}

// ========== UpdateBlueprint Implementation ==========

FAnimBPImporter::FUpdateResult FAnimBPImporter::UpdateBlueprintDetailed(UAnimBlueprint* ExistingBlueprint, const FString& NewDSLCode)
{
	FUpdateResult Result;
	
	if (!ExistingBlueprint)
	{
		Result.Warnings.Add(TEXT("Null blueprint"));
		return Result;
	}
	
	// Step 1: Export current blueprint to AST (old state)
	TSharedPtr<FAnimGraphAST> OldAST = FAnimBPExporter::ExportToAST(ExistingBlueprint);
	if (!OldAST.IsValid())
	{
		Result.Warnings.Add(TEXT("Failed to export current blueprint to AST — falling back to full rebuild"));
		
		// Parse new DSL
		TArray<FAnimLangParseError> ParseErrors;
		TSharedPtr<FAnimGraphAST> NewAST = FAnimLangParser::Parse(NewDSLCode, ParseErrors);
		if (!NewAST.IsValid() || ParseErrors.ContainsByPredicate(
			[](const FAnimLangParseError& Error) { return !Error.bWarning; }))
		{
			Result.Warnings.Add(TEXT("Failed to parse new DSL code"));
			return Result;
		}
		FString DependencyError;
		if (!ValidateExternalDependencies(NewAST->Dependencies, DependencyError))
		{
			Result.Warnings.Add(DependencyError);
			return Result;
		}
		
		// Full rebuild
		Result.bUsedIncrementalPatch = false;
		Result.bSuccess = RebuildAnimGraph(ExistingBlueprint, NewAST);
		
		if (Result.bSuccess)
		{
			FString CompileError;
			const AnimBP2FPImportLifecycle::FImportLifecycleContext LifecycleContext =
				MakeAnimLifecycleContext(ExistingBlueprint, FindAnimGraph(ExistingBlueprint), true, false, true, true);
			BroadcastFinalizeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PreFinalize, LifecycleContext);
			if (!CompileBlueprint(ExistingBlueprint, &CompileError))
			{
				Result.bSuccess = false;
			}
			else
			{
				FString MetadataError;
				if (!ApplyAnimBlueprintMetadata(ExistingBlueprint, NewAST->Metadata, MetadataError))
				{
					Result.bSuccess = false;
					Result.Warnings.Add(MetadataError);
				}
			}
			BroadcastFinalizeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PostFinalize, LifecycleContext);
			if (!CompileError.IsEmpty())
			{
				Result.Warnings.Add(TEXT("Compile: ") + CompileError);
			}
		}
		
		return Result;
	}
	
	// Step 2: Parse new DSL to AST
	TArray<FAnimLangParseError> ParseErrors;
	TSharedPtr<FAnimGraphAST> NewAST = FAnimLangParser::Parse(NewDSLCode, ParseErrors);
	
	for (const FAnimLangParseError& Err : ParseErrors)
	{
		Result.Warnings.Add(FString::Printf(TEXT("Parse: %s"), *Err.ToString()));
	}
	
	if (!NewAST.IsValid() || ParseErrors.ContainsByPredicate(
		[](const FAnimLangParseError& Error) { return !Error.bWarning; }))
	{
		Result.Warnings.Add(TEXT("Failed to parse new DSL code"));
		return Result;
	}
	FString DependencyError;
	if (!ValidateExternalDependencies(NewAST->Dependencies, DependencyError))
	{
		Result.Warnings.Add(DependencyError);
		return Result;
	}

	const bool bHelperGraphsChanged = !AreEquivalentHelperSets(OldAST->HelperGraphs, NewAST->HelperGraphs);
	const bool bLogicGraphsChanged = !AreEquivalentLogicGraphSets(OldAST->LogicGraphs, NewAST->LogicGraphs);
	const bool bMetadataChanged = OldAST->Metadata.RootMotionMode != NewAST->Metadata.RootMotionMode;
	
	// Step 3: Compute diff
	FAnimLangDiffResult Diff = FAnimLangDiffer::Diff(OldAST, NewAST);
	
	Result.NumChanges = Diff.Entries.Num();
	Result.NumPropertyChanges = Diff.NumPropertyChanges();
	Result.NumStructuralChanges = Diff.NumStructuralChanges();
	Result.DiffSummary = Diff.ToSummary();

	if (bHelperGraphsChanged)
	{
		Result.NumChanges += 1;
		Result.NumStructuralChanges += 1;
		Result.DiffSummary = Diff.HasChanges()
			? Result.DiffSummary + TEXT("; helper graphs changed")
			: TEXT("helper graphs changed");
	}
	if (bLogicGraphsChanged)
	{
		Result.NumChanges += 1;
		Result.NumStructuralChanges += 1;
		Result.DiffSummary = Result.DiffSummary.IsEmpty()
			? TEXT("logic graphs changed")
			: Result.DiffSummary + TEXT("; logic graphs changed");
	}
	if (bMetadataChanged)
	{
		Result.NumChanges += 1;
		Result.NumStructuralChanges += 1;
		Result.DiffSummary = Result.DiffSummary.IsEmpty()
			? TEXT("AnimBlueprint metadata changed")
			: Result.DiffSummary + TEXT("; AnimBlueprint metadata changed");
	}
	
	if (!Diff.HasChanges() && !bHelperGraphsChanged && !bLogicGraphsChanged && !bMetadataChanged)
	{
		Result.bSuccess = true;
		Result.Warnings.Add(TEXT("No changes detected between current blueprint and new DSL"));
		return Result;
	}
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("UpdateBlueprint diff: %s"), *Result.DiffSummary);
	
	// Step 4: Decide strategy
	//
	// Use incremental patch when:
	//   - Only property changes (no structural modifications)
	//   - No node additions/removals
	//   - No define changes
	//   - No variable changes
	//
	// Use full rebuild when:
	//   - Any structural change exists
	//   - Root node type changed
	
	bool bCanUseIncremental = (Result.NumStructuralChanges == 0);
	
	if (bCanUseIncremental)
	{
		// ===== INCREMENTAL PATH =====
		UE_LOG(LogAnimBPImporter, Log, TEXT("Using incremental patch (%d property changes)"), Result.NumPropertyChanges);
		Result.bUsedIncrementalPatch = true;
		
		// Use the Patcher module
		FAnimLangPatchResult PatchResult = FAnimLangPatcher::Apply(ExistingBlueprint, Diff, NewAST);
		
		Result.AppliedOps = PatchResult.AppliedOps;
		Result.Warnings.Append(PatchResult.Warnings);
		
		if (PatchResult.FailedOps.Num() > 0)
		{
			// Some incremental operations failed — fall back to full rebuild
			UE_LOG(LogAnimBPImporter, Warning, TEXT("Incremental patch had %d failures — falling back to full rebuild"), PatchResult.FailedOps.Num());
			
			for (const FString& FailedOp : PatchResult.FailedOps)
			{
				Result.Warnings.Add(TEXT("Incremental failed: ") + FailedOp);
			}
			
			// Fall back to full rebuild
			Result.bUsedIncrementalPatch = false;
			Result.bSuccess = RebuildAnimGraph(ExistingBlueprint, NewAST);
			Result.AppliedOps.Add(TEXT("Full AnimGraph rebuild (incremental fallback)"));
		}
		else
		{
			Result.bSuccess = true;
		}
	}
	else
	{
		// ===== FULL REBUILD PATH =====
		UE_LOG(LogAnimBPImporter, Log, TEXT("Using full rebuild (%d structural changes)"), Result.NumStructuralChanges);
		Result.bUsedIncrementalPatch = false;
		
		Result.bSuccess = RebuildAnimGraph(ExistingBlueprint, NewAST);
		
		if (Result.bSuccess)
		{
			Result.AppliedOps.Add(FString::Printf(TEXT("Full AnimGraph rebuild (%d changes applied)"), Result.NumChanges));
		}
	}
	
	// Step 5: Compile
	if (Result.bSuccess)
	{
		struct FBlendListPinCompileState
		{
			FName Name;
			FString DefaultValue;
			FString AutogeneratedDefaultValue;
		};
		struct FBlendListCompileState
		{
			TWeakObjectPtr<UAnimGraphNode_BlendListBase> Node;
			TArray<FString> BlendTimes;
			TArray<FBlendListPinCompileState> Pins;
		};
		TArray<FBlendListCompileState> BlendListCompileStates;
		TArray<UEdGraph*> BlueprintGraphs;
		ExistingBlueprint->GetAllGraphs(BlueprintGraphs);
		for (UEdGraph* Graph : BlueprintGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				UAnimGraphNode_BlendListBase* BlendListNode = Cast<UAnimGraphNode_BlendListBase>(GraphNode);
				if (!BlendListNode) continue;
				FBlendListCompileState& State = BlendListCompileStates.AddDefaulted_GetRef();
				State.Node = BlendListNode;
				for (UEdGraphPin* Pin : BlendListNode->Pins)
				{
					if (Pin && Pin->PinName.ToString().StartsWith(TEXT("BlendTime_")))
					{
						State.Pins.Add({ Pin->PinName, Pin->DefaultValue, Pin->AutogeneratedDefaultValue });
					}
				}
				for (TFieldIterator<FStructProperty> PropIt(BlendListNode->GetClass()); PropIt; ++PropIt)
				{
					FStructProperty* StructProperty = *PropIt;
					if (!StructProperty->Struct || !StructProperty->Struct->IsChildOf(FAnimNode_Base::StaticStruct())) continue;
					FArrayProperty* BlendTimeProperty = FindFProperty<FArrayProperty>(StructProperty->Struct, TEXT("BlendTime"));
					if (!BlendTimeProperty) break;
					void* StructMemory = StructProperty->ContainerPtrToValuePtr<void>(BlendListNode);
					FScriptArrayHelper ArrayHelper(BlendTimeProperty, BlendTimeProperty->ContainerPtrToValuePtr<void>(StructMemory));
					for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
					{
						FString Value;
						BlendTimeProperty->Inner->ExportText_Direct(Value, ArrayHelper.GetRawPtr(Index), nullptr, nullptr, PPF_None);
						State.BlendTimes.Add(MoveTemp(Value));
					}
					break;
				}
			}
		}
		auto RestoreBlendListCompileState = [&BlendListCompileStates]()
		{
			for (const FBlendListCompileState& State : BlendListCompileStates)
			{
				UAnimGraphNode_BlendListBase* BlendListNode = State.Node.Get();
				if (!BlendListNode) continue;
				for (const FBlendListPinCompileState& PinState : State.Pins)
				{
					if (UEdGraphPin* Pin = BlendListNode->FindPin(PinState.Name))
					{
						Pin->DefaultValue = PinState.DefaultValue;
						Pin->AutogeneratedDefaultValue = PinState.AutogeneratedDefaultValue;
					}
				}
				for (TFieldIterator<FStructProperty> PropIt(BlendListNode->GetClass()); PropIt; ++PropIt)
				{
					FStructProperty* StructProperty = *PropIt;
					if (!StructProperty->Struct || !StructProperty->Struct->IsChildOf(FAnimNode_Base::StaticStruct())) continue;
					FArrayProperty* BlendTimeProperty = FindFProperty<FArrayProperty>(StructProperty->Struct, TEXT("BlendTime"));
					if (!BlendTimeProperty) break;
					void* StructMemory = StructProperty->ContainerPtrToValuePtr<void>(BlendListNode);
					FScriptArrayHelper ArrayHelper(BlendTimeProperty, BlendTimeProperty->ContainerPtrToValuePtr<void>(StructMemory));
					ArrayHelper.EmptyAndAddValues(State.BlendTimes.Num());
					for (int32 Index = 0; Index < State.BlendTimes.Num(); ++Index)
					{
						BlendTimeProperty->Inner->ImportText_Direct(*State.BlendTimes[Index], ArrayHelper.GetRawPtr(Index), nullptr, PPF_None);
					}
					break;
				}
			}
		};

		FString CompileError;
		const AnimBP2FPImportLifecycle::FImportLifecycleContext LifecycleContext =
			MakeAnimLifecycleContext(ExistingBlueprint, FindAnimGraph(ExistingBlueprint), !Result.bUsedIncrementalPatch, Result.bUsedIncrementalPatch, true, true);
		BroadcastFinalizeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PreFinalize, LifecycleContext);
		if (!CompileBlueprint(ExistingBlueprint, &CompileError))
		{
			Result.Warnings.Add(TEXT("Compile: ") + CompileError);
			Result.bSuccess = false;
		}
		else
		{
			RestoreBlendListCompileState();
			FString MetadataError;
			if (!ApplyAnimBlueprintMetadata(ExistingBlueprint, NewAST->Metadata, MetadataError))
			{
				Result.Warnings.Add(MetadataError);
				Result.bSuccess = false;
			}
			else
			{
				Result.AppliedOps.Add(TEXT("Blueprint compiled successfully"));
			}
		}
		BroadcastFinalizeLifecycle(AnimBP2FPImportLifecycle::EImportLifecyclePhase::PostFinalize, LifecycleContext);
	}
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("UpdateBlueprint %s: %s"),
		Result.bSuccess ? TEXT("succeeded") : TEXT("failed"),
		*Result.DiffSummary);
	
	return Result;
}

bool FAnimBPImporter::UpdateBlueprint(UAnimBlueprint* ExistingBlueprint, const FString& NewDSLCode, FString* OutError)
{
	FUpdateResult Result = UpdateBlueprintDetailed(ExistingBlueprint, NewDSLCode);
	
	if (!Result.bSuccess && OutError)
	{
		*OutError = TEXT("UpdateBlueprint failed");
		for (const FString& W : Result.Warnings)
		{
			*OutError += TEXT("\n  ") + W;
		}
	}
	
	return Result.bSuccess;
}

#endif // WITH_EDITOR
