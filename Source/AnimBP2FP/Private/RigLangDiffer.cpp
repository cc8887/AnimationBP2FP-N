// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "RigLangDiffer.h"
#include "RigLangExporter.h"
#include "RigLangImporter.h"

namespace
{
FString JsonEscape(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Value.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return Value;
}

FString UnquoteProperty(const FString& Value)
{
	return Value.Len() >= 2 && Value[0] == TEXT('"') && Value[Value.Len() - 1] == TEXT('"')
		? Value.Mid(1, Value.Len() - 2) : Value;
}

bool Exact(const FString& A, const FString& B)
{
	return A.Equals(B, ESearchCase::CaseSensitive);
}

bool ExactProperties(const TMap<FString, FString>& A, const TMap<FString, FString>& B)
{
	if (A.Num() != B.Num()) return false;
	for (const TPair<FString, FString>& Left : A)
	{
		const TPair<FString, FString>* Right = nullptr;
		for (const TPair<FString, FString>& Candidate : B)
		{
			if (Exact(Left.Key, Candidate.Key)) { Right = &Candidate; break; }
		}
		if (!Right || !Exact(Left.Value, Right->Value)) return false;
	}
	return true;
}

void AddDifference(FRigLangDiffResult& Result, const FString& Path,
	const FString& OldValue, const FString& NewValue)
{
	FRigLangDifference& Difference = Result.Differences.AddDefaulted_GetRef();
	Difference.Path = Path;
	Difference.OldValue = OldValue;
	Difference.NewValue = NewValue;
}

void DiffScalar(const FString& OldValue, const FString& NewValue,
	const FString& Path, FRigLangDiffResult& Result)
{
	if (!Exact(OldValue, NewValue)) AddDifference(Result, Path, OldValue, NewValue);
}

void DiffBool(const bool bOldValue, const bool bNewValue,
	const FString& Path, FRigLangDiffResult& Result)
{
	DiffScalar(bOldValue ? TEXT("true") : TEXT("false"),
		bNewValue ? TEXT("true") : TEXT("false"), Path, Result);
}

FString DoubleText(const double Value)
{
	return FString::Printf(TEXT("%.17g"), Value == 0.0 ? 0.0 : Value);
}

void DiffProperties(const TMap<FString, FString>& OldProperties,
	const TMap<FString, FString>& NewProperties, const FString& Path,
	FRigLangDiffResult& Result)
{
	for (const TPair<FString, FString>& Property : OldProperties)
	{
		const FString* NewValue = NewProperties.Find(Property.Key);
		if (!NewValue) AddDifference(Result, Path + TEXT("/property:") + Property.Key,
			Property.Value, TEXT("missing"));
		else DiffScalar(Property.Value, *NewValue,
			Path + TEXT("/property:") + Property.Key, Result);
	}
	for (const TPair<FString, FString>& Property : NewProperties)
		if (!OldProperties.Contains(Property.Key))
			AddDifference(Result, Path + TEXT("/property:") + Property.Key,
				TEXT("missing"), Property.Value);
}

void DiffType(const FAnimLispTypeRef& OldType, const FAnimLispTypeRef& NewType,
	const FString& Path, FRigLangDiffResult& Result)
{
	DiffScalar(OldType.CPPType, NewType.CPPType, Path + TEXT("/cpp-type"), Result);
	DiffScalar(OldType.CPPTypeObject, NewType.CPPTypeObject,
		Path + TEXT("/cpp-type-object"), Result);
	DiffScalar(OldType.ContainerType, NewType.ContainerType,
		Path + TEXT("/container-type"), Result);
}

FString HierarchyKindPath(const ERigHierarchyElementKind Kind)
{
	switch (Kind)
	{
	case ERigHierarchyElementKind::Bone: return TEXT("bone");
	case ERigHierarchyElementKind::Control: return TEXT("control");
	case ERigHierarchyElementKind::Null: return TEXT("null");
	case ERigHierarchyElementKind::Curve: return TEXT("curve");
	default: return TEXT("unknown");
	}
}

FString NodeIdentity(const FRigNodeAST& Node)
{
	return Node.Guid.IsEmpty() ? Node.StableId : Node.Guid;
}

TMap<FString, FString> SemanticNodeProperties(const FRigNodeAST& Node)
{
	TMap<FString, FString> Result = Node.Properties;
	Result.Remove(TEXT("editor-position"));
	if (Node.Kind == ERigNodeKind::Comment)
	{
		Result.Remove(TEXT("font-size"));
		Result.Remove(TEXT("bubble-visible"));
		Result.Remove(TEXT("color-bubble"));
	}
	if (Exact(Result.FindRef(TEXT("template-resolved")), TEXT("false")))
		Result.Remove(TEXT("resolved-function"));
	// These are regenerated RigVM caches. GraphSemanticSnapshot below compares
	// their declaration/reference relationships through stable semantic tokens.
	Result.Remove(TEXT("interface-pin-guids"));
	Result.Remove(TEXT("variable-guid"));
	const bool bResolvedUnit = Node.Kind == ERigNodeKind::Unit
		&& Exact(Node.ClassPath, TEXT("/Script/RigVMDeveloper.RigVMUnitNode"))
		&& !Node.MethodName.IsEmpty()
		&& !UnquoteProperty(Result.FindRef(TEXT("script-struct"))).IsEmpty()
		&& !UnquoteProperty(Result.FindRef(TEXT("resolved-function"))).IsEmpty()
		&& Exact(Result.FindRef(TEXT("template-resolved")), TEXT("true"))
		&& !Exact(Result.FindRef(TEXT("template-types")), TEXT("()"));
	if (bResolvedUnit)
		Result.Add(TEXT("template-notation"), TEXT("\"$resolved-unit-notation\""));
	return Result;
}

FString LinkIdentity(const FRigLinkAST& Link)
{
	return Link.SourceNodeId + TEXT(".") + Link.SourcePinPath + TEXT("->")
		+ Link.TargetNodeId + TEXT(".") + Link.TargetPinPath;
}

void DiffPins(const TArray<FRigPinAST>& OldPins, const TArray<FRigPinAST>& NewPins,
	const FString& NodePath, FRigLangDiffResult& Result)
{
	TMap<FString, const FRigPinAST*> NewByPath;
	auto IsTransientCache = [](const FRigPinAST& Pin)
	{
		return Pin.Direction == ERigPinDirection::Hidden
			&& Exact(Pin.Type.CPPType, TEXT("FCachedRigElement"));
	};
	TArray<FString> OldOrder;
	TArray<FString> NewOrder;
	for (const FRigPinAST& Pin : OldPins) if (!IsTransientCache(Pin)) OldOrder.Add(Pin.Path);
	for (const FRigPinAST& Pin : NewPins) if (!IsTransientCache(Pin)) NewOrder.Add(Pin.Path);
	DiffScalar(FString::Join(OldOrder, TEXT("|")), FString::Join(NewOrder, TEXT("|")),
		NodePath + TEXT("/pin-order"), Result);
	for (const FRigPinAST& Pin : NewPins)
		if (!IsTransientCache(Pin)) NewByPath.Add(Pin.Path, &Pin);
	for (const FRigPinAST& OldPin : OldPins)
	{
		if (IsTransientCache(OldPin)) continue;
		const FString PinPath = NodePath + TEXT("/pin:") + OldPin.Path;
		const FRigPinAST* const* NewPinPtr = NewByPath.Find(OldPin.Path);
		if (!NewPinPtr)
		{
			AddDifference(Result, PinPath, TEXT("present"), TEXT("missing"));
			continue;
		}
		const FRigPinAST& NewPin = **NewPinPtr;
		const bool bEquivalentTypedDefaults =
			FRigLangImporter::BuildCanonicalPinDefault(OldPin).Equals(
				FRigLangImporter::BuildCanonicalPinDefault(NewPin), ESearchCase::CaseSensitive);
		if (!OldPin.DefaultValue.Equals(NewPin.DefaultValue, ESearchCase::CaseSensitive)
			&& !bEquivalentTypedDefaults)
			AddDifference(Result, PinPath + TEXT("/default"), OldPin.DefaultValue, NewPin.DefaultValue);
		if (OldPin.Direction != NewPin.Direction)
			AddDifference(Result, PinPath + TEXT("/direction"),
				FString::FromInt(static_cast<int32>(OldPin.Direction)),
				FString::FromInt(static_cast<int32>(NewPin.Direction)));
		DiffType(OldPin.Type, NewPin.Type, PinPath + TEXT("/type"), Result);
		if (OldPin.bExecuteContext != NewPin.bExecuteContext)
			AddDifference(Result, PinPath + TEXT("/execute-context"),
				OldPin.bExecuteContext ? TEXT("true") : TEXT("false"),
				NewPin.bExecuteContext ? TEXT("true") : TEXT("false"));
		DiffProperties(OldPin.Properties, NewPin.Properties, PinPath, Result);
		DiffPins(OldPin.SubPins, NewPin.SubPins, PinPath, Result);
		NewByPath.Remove(OldPin.Path);
	}
	for (const TPair<FString, const FRigPinAST*>& Pair : NewByPath)
		AddDifference(Result, NodePath + TEXT("/pin:") + Pair.Key, TEXT("missing"), TEXT("present"));
}

void DiffLinks(const TArray<FRigLinkAST>& OldLinks, const TArray<FRigLinkAST>& NewLinks,
	const FString& GraphPath, FRigLangDiffResult& Result)
{
	TArray<FString> OldOrder;
	TArray<FString> NewOrder;
	for (const FRigLinkAST& Link : OldLinks) OldOrder.Add(LinkIdentity(Link));
	for (const FRigLinkAST& Link : NewLinks) NewOrder.Add(LinkIdentity(Link));
	DiffScalar(FString::Join(OldOrder, TEXT("|")), FString::Join(NewOrder, TEXT("|")),
		GraphPath + TEXT("/link-order"), Result);
	TArray<bool> Matched;
	Matched.Init(false, NewLinks.Num());
	for (const FRigLinkAST& Link : OldLinks)
	{
		const FString Id = LinkIdentity(Link);
		int32 Match = INDEX_NONE;
		for (int32 Index = 0; Index < NewLinks.Num(); ++Index)
		{
			if (!Matched[Index] && Exact(Id, LinkIdentity(NewLinks[Index]))) { Match = Index; break; }
		}
		if (Match == INDEX_NONE)
			AddDifference(Result, GraphPath + TEXT("/link:") + Id, TEXT("present"), TEXT("missing"));
		else
		{
			Matched[Match] = true;
			DiffProperties(Link.Properties, NewLinks[Match].Properties,
				GraphPath + TEXT("/link:") + Id, Result);
		}
	}
	for (int32 Index = 0; Index < NewLinks.Num(); ++Index)
		if (!Matched[Index]) AddDifference(Result, GraphPath + TEXT("/link:")
			+ LinkIdentity(NewLinks[Index]), TEXT("missing"), TEXT("present"));
}

void DiffGraph(const FRigGraphAST& OldGraph, const FRigGraphAST& NewGraph,
	const FString& GraphPath, FRigLangDiffResult& Result,
	const TMap<FString, FString>* OldGraphTokens = nullptr,
	const TMap<FString, FString>* NewGraphTokens = nullptr)
{
	const FString GraphFieldPath = GraphPath + TEXT("/graph");
	const FString OldGraphIdentity = OldGraphTokens
		? OldGraphTokens->FindRef(OldGraph.StableId) : OldGraph.StableId;
	const FString NewGraphIdentity = NewGraphTokens
		? NewGraphTokens->FindRef(NewGraph.StableId) : NewGraph.StableId;
	DiffScalar(OldGraphIdentity, NewGraphIdentity, GraphFieldPath + TEXT("/stable-id"), Result);
	const bool bSourceFallbackGraph = OldGraph.Nodes.IsEmpty()
		|| !OldGraph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
		{
			FGuid EditorGuid;
			return !Node.bInjected && FGuid::Parse(Node.Guid, EditorGuid) && EditorGuid.IsValid();
		});
	if (!bSourceFallbackGraph)
		DiffScalar(OldGraph.EditorGuid, NewGraph.EditorGuid,
			GraphFieldPath + TEXT("/editor-guid"), Result);
	DiffScalar(OldGraph.Role, NewGraph.Role, GraphFieldPath + TEXT("/role"), Result);
	DiffScalar(OldGraph.ParentStableId, NewGraph.ParentStableId,
		GraphFieldPath + TEXT("/parent-stable-id"), Result);
	DiffProperties(OldGraph.Properties, NewGraph.Properties, GraphFieldPath, Result);
	TArray<FString> OldLocalOrder, NewLocalOrder, OldNodeOrder, NewNodeOrder;
	for (const FRigGraphVariableAST& Variable : OldGraph.LocalVariables) OldLocalOrder.Add(Variable.Name);
	for (const FRigGraphVariableAST& Variable : NewGraph.LocalVariables) NewLocalOrder.Add(Variable.Name);
	for (const FRigNodeAST& Node : OldGraph.Nodes) OldNodeOrder.Add(Node.StableId);
	for (const FRigNodeAST& Node : NewGraph.Nodes) NewNodeOrder.Add(Node.StableId);
	DiffScalar(FString::Join(OldLocalOrder, TEXT("|")), FString::Join(NewLocalOrder, TEXT("|")),
		GraphFieldPath + TEXT("/local-variable-order"), Result);
	DiffScalar(FString::Join(OldNodeOrder, TEXT("|")), FString::Join(NewNodeOrder, TEXT("|")),
		GraphFieldPath + TEXT("/node-order"), Result);
	TMap<FString, const FRigGraphVariableAST*> NewLocals;
	for (const FRigGraphVariableAST& Variable : NewGraph.LocalVariables)
		NewLocals.Add(Variable.Name, &Variable);
	for (const FRigGraphVariableAST& OldVariable : OldGraph.LocalVariables)
	{
		const FString Path = GraphPath + TEXT("/local-variable:") + OldVariable.Name;
		const FRigGraphVariableAST* const* NewVariable = NewLocals.Find(OldVariable.Name);
		if (!NewVariable) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			DiffType(OldVariable.Type, (*NewVariable)->Type, Path + TEXT("/type"), Result);
			DiffScalar(OldVariable.CPPTypeObjectPath, (*NewVariable)->CPPTypeObjectPath,
				Path + TEXT("/cpp-type-object-path"), Result);
			DiffScalar(OldVariable.DefaultValue, (*NewVariable)->DefaultValue,
				Path + TEXT("/default"), Result);
			DiffScalar(OldVariable.Category, (*NewVariable)->Category,
				Path + TEXT("/category"), Result);
			DiffScalar(OldVariable.Tooltip, (*NewVariable)->Tooltip,
				Path + TEXT("/tooltip"), Result);
			DiffBool(OldVariable.bExposedOnSpawn, (*NewVariable)->bExposedOnSpawn,
				Path + TEXT("/exposed-on-spawn"), Result);
			DiffBool(OldVariable.bExposeToCinematics, (*NewVariable)->bExposeToCinematics,
				Path + TEXT("/expose-to-cinematics"), Result);
			DiffBool(OldVariable.bPublic, (*NewVariable)->bPublic, Path + TEXT("/public"), Result);
			DiffBool(OldVariable.bPrivate, (*NewVariable)->bPrivate, Path + TEXT("/private"), Result);
			NewLocals.Remove(OldVariable.Name);
		}
	}
	for (const TPair<FString, const FRigGraphVariableAST*>& Pair : NewLocals)
		AddDifference(Result, GraphPath + TEXT("/local-variable:") + Pair.Key,
			TEXT("missing"), TEXT("present"));
	TArray<bool> MatchedNewNodes;
	MatchedNewNodes.Init(false, NewGraph.Nodes.Num());
	for (const FRigNodeAST& OldNode : OldGraph.Nodes)
	{
		const FString Identity = NodeIdentity(OldNode);
		const FString NodePath = GraphPath + TEXT("/node:") + Identity;
		int32 NewNodeIndex = INDEX_NONE;
		for (int32 Index = 0; Index < NewGraph.Nodes.Num(); ++Index)
		{
			if (MatchedNewNodes[Index]) continue;
			const FRigNodeAST& Candidate = NewGraph.Nodes[Index];
			if (Exact(Identity, NodeIdentity(Candidate))
				|| (OldNode.Guid.StartsWith(TEXT("model:"))
					&& Exact(OldNode.StableId, Candidate.StableId)))
			{
				NewNodeIndex = Index;
				break;
			}
		}
		if (NewNodeIndex == INDEX_NONE)
		{
			AddDifference(Result, NodePath, TEXT("present"), TEXT("missing"));
			continue;
		}
		MatchedNewNodes[NewNodeIndex] = true;
		const FRigNodeAST& NewNode = NewGraph.Nodes[NewNodeIndex];
		if (OldNode.Kind != NewNode.Kind)
			AddDifference(Result, NodePath + TEXT("/kind"),
				FString::FromInt(static_cast<int32>(OldNode.Kind)),
				FString::FromInt(static_cast<int32>(NewNode.Kind)));
		DiffScalar(OldNode.StableId, NewNode.StableId, NodePath + TEXT("/stable-id"), Result);
		DiffScalar(OldNode.ClassPath, NewNode.ClassPath, NodePath + TEXT("/class"), Result);
		DiffScalar(OldNode.MethodName, NewNode.MethodName, NodePath + TEXT("/method"), Result);
		DiffScalar(OldNode.EventName, NewNode.EventName, NodePath + TEXT("/event"), Result);
		DiffScalar(OldNode.FunctionName, NewNode.FunctionName, NodePath + TEXT("/function"), Result);
		DiffScalar(OldNode.FunctionIdentifier.HostObject, NewNode.FunctionIdentifier.HostObject,
			NodePath + TEXT("/function-identifier/host"), Result);
		DiffScalar(OldNode.FunctionIdentifier.LibraryNodePath, NewNode.FunctionIdentifier.LibraryNodePath,
			NodePath + TEXT("/function-identifier/library-node-path"), Result);
		DiffScalar(OldGraphTokens ? OldGraphTokens->FindRef(OldNode.ContainedGraphStableId) : OldNode.ContainedGraphStableId,
			NewGraphTokens ? NewGraphTokens->FindRef(NewNode.ContainedGraphStableId) : NewNode.ContainedGraphStableId,
			NodePath + TEXT("/contained-graph"), Result);
		DiffBool(OldNode.bInjected, NewNode.bInjected, NodePath + TEXT("/injected"), Result);
		DiffScalar(OldNode.InjectionOwnerPin, NewNode.InjectionOwnerPin,
			NodePath + TEXT("/injection-owner-pin"), Result);
		DiffScalar(FString::FromInt(OldNode.InjectionOrder), FString::FromInt(NewNode.InjectionOrder),
			NodePath + TEXT("/injection-order"), Result);
		DiffBool(OldNode.bInjectedAsInput, NewNode.bInjectedAsInput,
			NodePath + TEXT("/injected-as-input"), Result);
		DiffScalar(OldNode.InjectionInputPin, NewNode.InjectionInputPin,
			NodePath + TEXT("/injection-input-pin"), Result);
		DiffScalar(OldNode.InjectionOutputPin, NewNode.InjectionOutputPin,
			NodePath + TEXT("/injection-output-pin"), Result);
		if (OldNode.Coverage != NewNode.Coverage)
			AddDifference(Result, NodePath + TEXT("/coverage"),
				FString::FromInt(static_cast<int32>(OldNode.Coverage)),
				FString::FromInt(static_cast<int32>(NewNode.Coverage)));
		const bool bGeneratedFallbackGuid = OldNode.Guid.StartsWith(TEXT("model:"))
			&& Exact(OldNode.StableId, NewNode.StableId);
		if (!bGeneratedFallbackGuid)
			DiffScalar(OldNode.Guid, NewNode.Guid, NodePath + TEXT("/guid"), Result);
		if (OldNode.Kind != NewNode.Kind
			|| (!bGeneratedFallbackGuid && !Exact(OldNode.Guid, NewNode.Guid))
			|| !Exact(OldNode.ClassPath, NewNode.ClassPath)
			|| !Exact(OldNode.StableId, NewNode.StableId)
			|| !Exact(OldNode.MethodName, NewNode.MethodName)
			|| !Exact(OldNode.EventName, NewNode.EventName)
			|| !Exact(OldNode.FunctionName, NewNode.FunctionName)
			|| !Exact(OldNode.FunctionIdentifier.HostObject, NewNode.FunctionIdentifier.HostObject)
			|| !Exact(OldNode.FunctionIdentifier.LibraryNodePath, NewNode.FunctionIdentifier.LibraryNodePath)
			|| !Exact(OldGraphTokens
					? OldGraphTokens->FindRef(OldNode.ContainedGraphStableId)
					: OldNode.ContainedGraphStableId,
				NewGraphTokens
					? NewGraphTokens->FindRef(NewNode.ContainedGraphStableId)
					: NewNode.ContainedGraphStableId)
			|| OldNode.bInjected != NewNode.bInjected
			|| !Exact(OldNode.InjectionOwnerPin, NewNode.InjectionOwnerPin)
			|| OldNode.InjectionOrder != NewNode.InjectionOrder
			|| OldNode.bInjectedAsInput != NewNode.bInjectedAsInput
			|| !Exact(OldNode.InjectionInputPin, NewNode.InjectionInputPin)
			|| !Exact(OldNode.InjectionOutputPin, NewNode.InjectionOutputPin))
		{
			AddDifference(Result, NodePath + TEXT("/identity"), OldNode.ClassPath, NewNode.ClassPath);
		}
		const TMap<FString, FString> OldProperties = SemanticNodeProperties(OldNode);
		const TMap<FString, FString> NewProperties = SemanticNodeProperties(NewNode);
		DiffProperties(OldProperties, NewProperties, NodePath, Result);
		DiffPins(OldNode.Pins, NewNode.Pins, NodePath, Result);
	}
	for (int32 Index = 0; Index < NewGraph.Nodes.Num(); ++Index)
		if (!MatchedNewNodes[Index])
			AddDifference(Result, GraphPath + TEXT("/node:") + NodeIdentity(NewGraph.Nodes[Index]),
				TEXT("missing"), TEXT("present"));
	DiffLinks(OldGraph.Links, NewGraph.Links, GraphPath, Result);
}

template<typename T>
TMap<FString, const T*> IndexByName(const TArray<T>& Values)
{
	TMap<FString, const T*> Result;
	for (const T& Value : Values) Result.Add(Value.Name, &Value);
	return Result;
}

void DiffArguments(const TArray<FRigCallableArgumentAST>& OldArguments,
	const TArray<FRigCallableArgumentAST>& NewArguments, const FString& OwnerPath,
	FRigLangDiffResult& Result)
{
	TArray<FString> OldOrder, NewOrder;
	for (const FRigCallableArgumentAST& Argument : OldArguments) OldOrder.Add(Argument.Name);
	for (const FRigCallableArgumentAST& Argument : NewArguments) NewOrder.Add(Argument.Name);
	DiffScalar(FString::Join(OldOrder, TEXT("|")), FString::Join(NewOrder, TEXT("|")),
		OwnerPath + TEXT("/argument-order"), Result);
	TMap<FString, const FRigCallableArgumentAST*> NewByName = IndexByName(NewArguments);
	for (const FRigCallableArgumentAST& OldArgument : OldArguments)
	{
		const FString Path = OwnerPath + TEXT("/argument:") + OldArgument.Name;
		const FRigCallableArgumentAST* const* NewArgument = NewByName.Find(OldArgument.Name);
		if (!NewArgument) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			if (OldArgument.Direction != (*NewArgument)->Direction)
				AddDifference(Result, Path + TEXT("/direction"),
					FString::FromInt(static_cast<int32>(OldArgument.Direction)),
					FString::FromInt(static_cast<int32>((*NewArgument)->Direction)));
			DiffType(OldArgument.Type, (*NewArgument)->Type, Path + TEXT("/type"), Result);
			DiffScalar(OldArgument.DefaultValue, (*NewArgument)->DefaultValue,
				Path + TEXT("/default"), Result);
			DiffBool(OldArgument.bExecuteContext, (*NewArgument)->bExecuteContext,
				Path + TEXT("/execute-context"), Result);
			DiffBool(OldArgument.bConstant, (*NewArgument)->bConstant,
				Path + TEXT("/constant"), Result);
			DiffBool(OldArgument.bInputVariable, (*NewArgument)->bInputVariable,
				Path + TEXT("/input-variable"), Result);
			NewByName.Remove(OldArgument.Name);
		}
	}
	for (const TPair<FString, const FRigCallableArgumentAST*>& Pair : NewByName)
		AddDifference(Result, OwnerPath + TEXT("/argument:") + Pair.Key,
			TEXT("missing"), TEXT("present"));
}

void DiffExternalVariables(const TArray<FRigExternalVariableAST>& OldVariables,
	const TArray<FRigExternalVariableAST>& NewVariables, const FString& OwnerPath,
	FRigLangDiffResult& Result)
{
	TMap<FString, const FRigExternalVariableAST*> NewByName = IndexByName(NewVariables);
	for (const FRigExternalVariableAST& OldVariable : OldVariables)
	{
		const FString Path = OwnerPath + TEXT("/external-variable:") + OldVariable.Name;
		const FRigExternalVariableAST* const* NewVariable = NewByName.Find(OldVariable.Name);
		if (!NewVariable) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			DiffScalar(OldVariable.Guid, (*NewVariable)->Guid, Path + TEXT("/guid"), Result);
			DiffType(OldVariable.Type, (*NewVariable)->Type, Path + TEXT("/type"), Result);
			DiffBool(OldVariable.bPublic, (*NewVariable)->bPublic, Path + TEXT("/public"), Result);
			DiffBool(OldVariable.bReadOnly, (*NewVariable)->bReadOnly, Path + TEXT("/read-only"), Result);
			NewByName.Remove(OldVariable.Name);
		}
	}
	for (const TPair<FString, const FRigExternalVariableAST*>& Pair : NewByName)
		AddDifference(Result, OwnerPath + TEXT("/external-variable:") + Pair.Key,
			TEXT("missing"), TEXT("present"));
}

void DiffDependencies(const TArray<FRigFunctionDependencyAST>& OldDependencies,
	const TArray<FRigFunctionDependencyAST>& NewDependencies, const FString& OwnerPath,
	FRigLangDiffResult& Result)
{
	auto Identity = [](const FRigFunctionDependencyAST& Dependency)
	{
		return Dependency.HostObject + TEXT("|") + Dependency.LibraryNodePath;
	};
	TSet<FString> NewIdentities;
	for (const FRigFunctionDependencyAST& Dependency : NewDependencies)
		NewIdentities.Add(Identity(Dependency));
	for (const FRigFunctionDependencyAST& Dependency : OldDependencies)
	{
		const FString Id = Identity(Dependency);
		if (!NewIdentities.Remove(Id))
			AddDifference(Result, OwnerPath + TEXT("/dependency:") + Id,
				TEXT("present"), TEXT("missing"));
	}
	for (const FString& Id : NewIdentities)
		AddDifference(Result, OwnerPath + TEXT("/dependency:") + Id,
			TEXT("missing"), TEXT("present"));
}

FString ImportIdentity(const FRigImportAST& Import)
{
	return Import.Import.Target.AssetPath + TEXT("|") + Import.Import.Alias;
}

void DiffHierarchyCollections(const FRigHierarchyElementAST& OldElement,
	const FRigHierarchyElementAST& NewElement, const FString& ElementPath,
	FRigLangDiffResult& Result)
{
	auto IsCurrentTransform = [](const ERigHierarchyTransformRole Role)
	{
		return Role == ERigHierarchyTransformRole::CurrentLocal || Role == ERigHierarchyTransformRole::CurrentGlobal
			|| Role == ERigHierarchyTransformRole::PoseCurrentLocal || Role == ERigHierarchyTransformRole::PoseCurrentGlobal
			|| Role == ERigHierarchyTransformRole::OffsetCurrentLocal || Role == ERigHierarchyTransformRole::OffsetCurrentGlobal
			|| Role == ERigHierarchyTransformRole::ShapeCurrentLocal || Role == ERigHierarchyTransformRole::ShapeCurrentGlobal;
	};
	auto IsCurrentState = [](const FRigHierarchyStateAST& State)
	{
		return State.Role == TEXT("current")
			&& (State.Kind == ERigHierarchyStateKind::ControlValue
				|| State.Kind == ERigHierarchyStateKind::PreferredEuler);
	};
	TMap<FString, const FRigHierarchyParentAST*> NewParents;
	for (const FRigHierarchyParentAST& Parent : NewElement.Parents) NewParents.Add(Parent.StableId, &Parent);
	for (const FRigHierarchyParentAST& OldParent : OldElement.Parents)
	{
		const FString Path = ElementPath + TEXT("/parent-entry:") + OldParent.StableId;
		const FRigHierarchyParentAST* const* NewParent = NewParents.Find(OldParent.StableId);
		if (!NewParent) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			DiffScalar(OldParent.Label, (*NewParent)->Label, Path + TEXT("/label"), Result);
			DiffScalar(DoubleText(OldParent.CurrentWeight.Location), DoubleText((*NewParent)->CurrentWeight.Location), Path + TEXT("/current/location"), Result);
			DiffScalar(DoubleText(OldParent.CurrentWeight.Rotation), DoubleText((*NewParent)->CurrentWeight.Rotation), Path + TEXT("/current/rotation"), Result);
			DiffScalar(DoubleText(OldParent.CurrentWeight.Scale), DoubleText((*NewParent)->CurrentWeight.Scale), Path + TEXT("/current/scale"), Result);
			DiffScalar(DoubleText(OldParent.InitialWeight.Location), DoubleText((*NewParent)->InitialWeight.Location), Path + TEXT("/initial/location"), Result);
			DiffScalar(DoubleText(OldParent.InitialWeight.Rotation), DoubleText((*NewParent)->InitialWeight.Rotation), Path + TEXT("/initial/rotation"), Result);
			DiffScalar(DoubleText(OldParent.InitialWeight.Scale), DoubleText((*NewParent)->InitialWeight.Scale), Path + TEXT("/initial/scale"), Result);
			NewParents.Remove(OldParent.StableId);
		}
	}
	for (const TPair<FString, const FRigHierarchyParentAST*>& Pair : NewParents)
		AddDifference(Result, ElementPath + TEXT("/parent-entry:") + Pair.Key, TEXT("missing"), TEXT("present"));

	TMap<int32, const FRigHierarchyTransformAST*> NewTransforms;
	for (const FRigHierarchyTransformAST& Transform : NewElement.Transforms)
		if (!IsCurrentTransform(Transform.Role)) NewTransforms.Add(static_cast<int32>(Transform.Role), &Transform);
	for (const FRigHierarchyTransformAST& OldTransform : OldElement.Transforms)
	{
		if (IsCurrentTransform(OldTransform.Role)) continue;
		const int32 Role = static_cast<int32>(OldTransform.Role);
		const FString Path = ElementPath + TEXT("/transform:") + FString::FromInt(Role);
		const FRigHierarchyTransformAST* const* NewTransform = NewTransforms.Find(Role);
		if (!NewTransform) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			FRigModuleAST OldTransformModule, NewTransformModule;
			FRigHierarchyElementAST OldOwner, NewOwner;
			OldOwner.StableId = TEXT("transform-owner"); OldOwner.Transforms.Add(OldTransform);
			NewOwner.StableId = TEXT("transform-owner"); NewOwner.Transforms.Add(**NewTransform);
			OldTransformModule.Hierarchy.Add(OldOwner);
			NewTransformModule.Hierarchy.Add(NewOwner);
			const FString OldCanonical = FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
				OldTransformModule, OldTransformModule);
			const FString NewCanonical = FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
				NewTransformModule, OldTransformModule);
			if (!Exact(OldCanonical, NewCanonical))
				AddDifference(Result, Path, OldCanonical, NewCanonical);
			NewTransforms.Remove(Role);
		}
	}
	for (const TPair<int32, const FRigHierarchyTransformAST*>& Pair : NewTransforms)
		AddDifference(Result, ElementPath + TEXT("/transform:") + FString::FromInt(Pair.Key), TEXT("missing"), TEXT("present"));

	auto StateKey = [](const FRigHierarchyStateAST& State)
	{
		return FString::FromInt(static_cast<int32>(State.Kind)) + TEXT(":") + State.Role;
	};
	TMap<FString, const FRigHierarchyStateAST*> NewStates;
	for (const FRigHierarchyStateAST& State : NewElement.States)
		if (!IsCurrentState(State)) NewStates.Add(StateKey(State), &State);
	for (const FRigHierarchyStateAST& OldState : OldElement.States)
	{
		if (IsCurrentState(OldState)) continue;
		const FString Key = StateKey(OldState);
		const FString Path = ElementPath + TEXT("/state:") + Key;
		const FRigHierarchyStateAST* const* NewState = NewStates.Find(Key);
		if (!NewState) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			DiffScalar(OldState.Type, (*NewState)->Type, Path + TEXT("/type"), Result);
			DiffScalar(OldState.SerializedValue, (*NewState)->SerializedValue, Path + TEXT("/serialized"), Result);
			DiffBool(OldState.bBoolValue, (*NewState)->bBoolValue, Path + TEXT("/bool"), Result);
			DiffScalar(FString::Printf(TEXT("%lld"), OldState.IntegerValue), FString::Printf(TEXT("%lld"), (*NewState)->IntegerValue), Path + TEXT("/integer"), Result);
			DiffScalar(DoubleText(OldState.NumberValue), DoubleText((*NewState)->NumberValue), Path + TEXT("/number"), Result);
			TArray<FString> OldComponents, NewComponents;
			for (double Value : OldState.Components) OldComponents.Add(DoubleText(Value));
			for (double Value : (*NewState)->Components) NewComponents.Add(DoubleText(Value));
			DiffScalar(FString::Join(OldComponents, TEXT("|")), FString::Join(NewComponents, TEXT("|")), Path + TEXT("/components"), Result);
			NewStates.Remove(Key);
		}
	}
	for (const TPair<FString, const FRigHierarchyStateAST*>& Pair : NewStates)
		AddDifference(Result, ElementPath + TEXT("/state:") + Pair.Key, TEXT("missing"), TEXT("present"));

	TMap<FString, const FRigHierarchyMetadataAST*> NewMetadata;
	for (const FRigHierarchyMetadataAST& Metadata : NewElement.Metadata) NewMetadata.Add(Metadata.Name, &Metadata);
	for (const FRigHierarchyMetadataAST& OldMetadata : OldElement.Metadata)
	{
		const FString Path = ElementPath + TEXT("/metadata:") + OldMetadata.Name;
		const FRigHierarchyMetadataAST* const* NewValue = NewMetadata.Find(OldMetadata.Name);
		if (!NewValue) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			if (OldMetadata.Kind != (*NewValue)->Kind)
				AddDifference(Result, Path + TEXT("/kind"), FString::FromInt(static_cast<int32>(OldMetadata.Kind)), FString::FromInt(static_cast<int32>((*NewValue)->Kind)));
			FRigModuleAST OldSlice, NewSlice;
			FRigHierarchyElementAST OldOwner, NewOwner;
			OldOwner.StableId = TEXT("metadata-owner"); OldOwner.Metadata.Add(OldMetadata);
			NewOwner.StableId = TEXT("metadata-owner"); NewOwner.Metadata.Add(**NewValue);
			OldSlice.Hierarchy.Add(OldOwner); NewSlice.Hierarchy.Add(NewOwner);
			DiffScalar(OldSlice.ToCanonicalHashInput(), NewSlice.ToCanonicalHashInput(), Path + TEXT("/value"), Result);
			NewMetadata.Remove(OldMetadata.Name);
		}
	}
	for (const TPair<FString, const FRigHierarchyMetadataAST*>& Pair : NewMetadata)
		AddDifference(Result, ElementPath + TEXT("/metadata:") + Pair.Key, TEXT("missing"), TEXT("present"));
}
}

FRigLangDiffResult FRigLangDiffer::Diff(const FRigModuleAST& OldModule, const FRigModuleAST& NewModule)
{
	FRigLangDiffResult Result;
	if (!Exact(OldModule.Header.ModuleId.AssetPath, NewModule.Header.ModuleId.AssetPath))
		AddDifference(Result, TEXT("module/header/asset-path"),
			OldModule.Header.ModuleId.AssetPath, NewModule.Header.ModuleId.AssetPath);
	if (OldModule.Header.ModuleId.Kind != NewModule.Header.ModuleId.Kind)
		AddDifference(Result, TEXT("module/header/module-kind"),
			FString::FromInt(static_cast<int32>(OldModule.Header.ModuleId.Kind)),
			FString::FromInt(static_cast<int32>(NewModule.Header.ModuleId.Kind)));
	if (!Exact(OldModule.Header.AssetClassPath, NewModule.Header.AssetClassPath))
		AddDifference(Result, TEXT("module/header/asset-class"),
			OldModule.Header.AssetClassPath, NewModule.Header.AssetClassPath);
	if (OldModule.Header.Version != NewModule.Header.Version)
		AddDifference(Result, TEXT("module/header/version"),
			FString::FromInt(OldModule.Header.Version), FString::FromInt(NewModule.Header.Version));
	const FString OldComputedHash = FRigLangExporter::ComputeContentHash(
		OldModule.ToCanonicalHashInput());
	const FString NewComputedHash = FRigLangExporter::ComputeContentHash(
		NewModule.ToCanonicalHashInput());
	const bool bOldHashInvalid = !Exact(OldModule.Header.ContentHash, OldComputedHash);
	const bool bNewHashInvalid = !Exact(NewModule.Header.ContentHash, NewComputedHash);
	if (bOldHashInvalid || bNewHashInvalid)
	{
		const FString OldHash = bOldHashInvalid
			? OldModule.Header.ContentHash + TEXT(" (computed ") + OldComputedHash + TEXT(")")
			: OldModule.Header.ContentHash;
		const FString NewHash = bNewHashInvalid
			? NewModule.Header.ContentHash + TEXT(" (computed ") + NewComputedHash + TEXT(")")
			: NewModule.Header.ContentHash;
		AddDifference(Result, TEXT("module/content-hash"), OldHash, NewHash);
	}
	for (const TPair<FString, FString>& Property : OldModule.Header.Properties)
	{
		const FString* NewValue = NewModule.Header.Properties.Find(Property.Key);
		if (!NewValue)
			AddDifference(Result, TEXT("module/header/property:") + Property.Key,
				Property.Value, TEXT("missing"));
		else if (!Exact(Property.Value, *NewValue))
			AddDifference(Result, TEXT("module/header/property:") + Property.Key,
				Property.Value, *NewValue);
	}
	for (const TPair<FString, FString>& Property : NewModule.Header.Properties)
		if (!OldModule.Header.Properties.Contains(Property.Key))
			AddDifference(Result, TEXT("module/header/property:") + Property.Key,
				TEXT("missing"), Property.Value);
	TMap<FString, const FRigImportAST*> NewImports;
	for (const FRigImportAST& Import : NewModule.Imports) NewImports.Add(ImportIdentity(Import), &Import);
	for (const FRigImportAST& OldImport : OldModule.Imports)
	{
		const FString Identity = ImportIdentity(OldImport);
		const FString Path = TEXT("import:") + OldImport.Import.Target.AssetPath
			+ TEXT(":") + OldImport.Import.Alias;
		const FRigImportAST* const* NewImport = NewImports.Find(Identity);
		if (!NewImport) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			DiffScalar(OldImport.Import.ExpectedHash, (*NewImport)->Import.ExpectedHash,
				Path + TEXT("/expected-hash"), Result);
			DiffProperties(OldImport.Properties, (*NewImport)->Properties, Path, Result);
			NewImports.Remove(Identity);
		}
	}
	for (const TPair<FString, const FRigImportAST*>& Pair : NewImports)
		AddDifference(Result, TEXT("import:") + Pair.Value->Import.Target.AssetPath
			+ TEXT(":") + Pair.Value->Import.Alias, TEXT("missing"), TEXT("present"));

	TMap<FString, const FRigVariableAST*> NewVariables = IndexByName(NewModule.Variables);
	for (const FRigVariableAST& OldVariable : OldModule.Variables)
	{
		const FString Path = TEXT("variable:") + OldVariable.Name;
		const FRigVariableAST* const* NewVariable = NewVariables.Find(OldVariable.Name);
		if (!NewVariable) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			DiffScalar(OldVariable.StableId, (*NewVariable)->StableId, Path + TEXT("/stable-id"), Result);
			if (OldVariable.Access != (*NewVariable)->Access)
				AddDifference(Result, Path + TEXT("/access"),
					FString::FromInt(static_cast<int32>(OldVariable.Access)),
					FString::FromInt(static_cast<int32>((*NewVariable)->Access)));
			DiffType(OldVariable.Type, (*NewVariable)->Type, Path + TEXT("/type"), Result);
			FRigModuleAST OldDefaultModule, NewDefaultModule;
			OldDefaultModule.Variables.Add(OldVariable);
			FRigVariableAST NewDefaultOnly = OldVariable;
			NewDefaultOnly.DefaultValue = (*NewVariable)->DefaultValue;
			NewDefaultModule.Variables.Add(NewDefaultOnly);
			const FString OldDefault = FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
				OldDefaultModule, OldDefaultModule);
			const FString NewDefault = FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
				NewDefaultModule, OldDefaultModule);
			if (!Exact(OldDefault, NewDefault))
				AddDifference(Result, Path + TEXT("/default"), OldDefault, NewDefault);
			DiffBool(OldVariable.bExecuteContext, (*NewVariable)->bExecuteContext,
				Path + TEXT("/execute-context"), Result);
			DiffProperties(OldVariable.Properties, (*NewVariable)->Properties, Path, Result);
			NewVariables.Remove(OldVariable.Name);
		}
	}
	for (const TPair<FString, const FRigVariableAST*>& Pair : NewVariables)
		AddDifference(Result, TEXT("variable:") + Pair.Key, TEXT("missing"), TEXT("present"));
	TSet<FString> OldGraphTokenCollisions;
	TSet<FString> NewGraphTokenCollisions;
	const TMap<FString, FString> OldGraphTokens =
		FRigLangImporter::BuildGraphSemanticTokens(OldModule, &OldGraphTokenCollisions);
	const TMap<FString, FString> NewGraphTokens =
		FRigLangImporter::BuildGraphSemanticTokens(NewModule, &NewGraphTokenCollisions);
	TMap<FString, const FRigHierarchyElementAST*> NewHierarchy;
	for (const FRigHierarchyElementAST& Element : NewModule.Hierarchy)
		NewHierarchy.Add(Element.StableId, &Element);
	for (const FRigHierarchyElementAST& OldElement : OldModule.Hierarchy)
	{
		const FString ElementPath = TEXT("hierarchy/") + HierarchyKindPath(OldElement.Kind)
			+ TEXT(":") + OldElement.Name;
		const FRigHierarchyElementAST* const* NewElementPtr = NewHierarchy.Find(OldElement.StableId);
		if (!NewElementPtr)
		{
			AddDifference(Result, ElementPath, TEXT("present"), TEXT("missing"));
			continue;
		}
		const FRigHierarchyElementAST& NewElement = **NewElementPtr;
		if (!Exact(OldElement.ParentName, NewElement.ParentName))
			AddDifference(Result, ElementPath + TEXT("/parent"), OldElement.ParentName, NewElement.ParentName);
		DiffScalar(OldElement.Name, NewElement.Name, ElementPath + TEXT("/name"), Result);
		if (OldElement.Kind != NewElement.Kind)
			AddDifference(Result, ElementPath + TEXT("/kind"),
				FString::FromInt(static_cast<int32>(OldElement.Kind)),
				FString::FromInt(static_cast<int32>(NewElement.Kind)));
		DiffProperties(OldElement.Properties, NewElement.Properties, ElementPath, Result);
		DiffHierarchyCollections(OldElement, NewElement, ElementPath, Result);
		FRigModuleAST OldElementModule;
		FRigModuleAST NewElementModule;
		OldElementModule.Hierarchy.Add(OldElement);
		NewElementModule.Hierarchy.Add(NewElement);
		const FString OldElementCanonical =
			FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
				OldElementModule, OldElementModule);
		const FString NewElementCanonical =
			FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
				NewElementModule, OldElementModule);
		if (!Exact(OldElementCanonical, NewElementCanonical))
			AddDifference(Result, ElementPath + TEXT("/canonical"),
				OldElementCanonical, NewElementCanonical);
		NewHierarchy.Remove(OldElement.StableId);
	}
	for (const TPair<FString, const FRigHierarchyElementAST*>& Pair : NewHierarchy)
		AddDifference(Result, TEXT("hierarchy/") + HierarchyKindPath(Pair.Value->Kind)
			+ TEXT(":") + Pair.Value->Name, TEXT("missing"), TEXT("present"));

	TMap<FString, const FRigFunctionAST*> NewFunctions = IndexByName(NewModule.Functions);
	for (const FRigFunctionAST& OldFunction : OldModule.Functions)
	{
		const FString Path = TEXT("function:") + OldFunction.Name;
		const FRigFunctionAST* const* NewFunction = NewFunctions.Find(OldFunction.Name);
		if (!NewFunction) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			FString OldFunctionGraphToken = OldGraphTokens.FindRef(OldFunction.GraphStableId);
			FString NewFunctionGraphToken = NewGraphTokens.FindRef((*NewFunction)->GraphStableId);
			if (OldFunctionGraphToken.IsEmpty()) OldFunctionGraphToken = OldFunction.GraphStableId;
			if (NewFunctionGraphToken.IsEmpty()) NewFunctionGraphToken = (*NewFunction)->GraphStableId;
			FString OldFunctionId = OldFunction.StableId;
			FString NewFunctionId = (*NewFunction)->StableId;
			if (!OldFunction.GraphStableId.IsEmpty()) OldFunctionId.ReplaceInline(
				*OldFunction.GraphStableId, *OldFunctionGraphToken, ESearchCase::CaseSensitive);
			if (!(*NewFunction)->GraphStableId.IsEmpty()) NewFunctionId.ReplaceInline(
				*(*NewFunction)->GraphStableId, *NewFunctionGraphToken, ESearchCase::CaseSensitive);
			DiffScalar(OldFunctionId, NewFunctionId,
				Path + TEXT("/stable-id"), Result);
			DiffScalar(OldFunction.FunctionIdentifier.HostObject,
				(*NewFunction)->FunctionIdentifier.HostObject,
				Path + TEXT("/identifier/host"), Result);
			DiffScalar(OldFunction.FunctionIdentifier.LibraryNodePath,
				(*NewFunction)->FunctionIdentifier.LibraryNodePath,
				Path + TEXT("/identifier/library-node-path"), Result);
			DiffScalar(OldFunction.Visibility, (*NewFunction)->Visibility,
				Path + TEXT("/visibility"), Result);
			DiffScalar(OldFunction.ReturnCPPType, (*NewFunction)->ReturnCPPType,
				Path + TEXT("/return-cpp-type"), Result);
			DiffScalar(OldFunctionGraphToken, NewFunctionGraphToken,
				Path + TEXT("/graph-stable-id"), Result);
			DiffArguments(OldFunction.Arguments, (*NewFunction)->Arguments, Path, Result);
			DiffExternalVariables(OldFunction.ExternalVariables,
				(*NewFunction)->ExternalVariables, Path, Result);
			DiffDependencies(OldFunction.Dependencies, (*NewFunction)->Dependencies, Path, Result);
			DiffProperties(OldFunction.Properties, (*NewFunction)->Properties, Path, Result);
			DiffGraph(OldFunction.Graph, (*NewFunction)->Graph, Path, Result,
				&OldGraphTokens, &NewGraphTokens);
			NewFunctions.Remove(OldFunction.Name);
		}
	}
	for (const TPair<FString, const FRigFunctionAST*>& Pair : NewFunctions)
		AddDifference(Result, TEXT("function:") + Pair.Key, TEXT("missing"), TEXT("present"));

	TMap<FString, const FRigEntryAST*> NewEntries = IndexByName(NewModule.Entries);
	for (const FRigEntryAST& OldEntry : OldModule.Entries)
	{
		const FString Path = TEXT("entry:") + OldEntry.Name;
		const FRigEntryAST* const* NewEntry = NewEntries.Find(OldEntry.Name);
		if (!NewEntry) AddDifference(Result, Path, TEXT("present"), TEXT("missing"));
		else
		{
			FString OldEntryGraphToken = OldGraphTokens.FindRef(OldEntry.GraphStableId);
			FString NewEntryGraphToken = NewGraphTokens.FindRef((*NewEntry)->GraphStableId);
			if (OldEntryGraphToken.IsEmpty()) OldEntryGraphToken = OldEntry.GraphStableId;
			if (NewEntryGraphToken.IsEmpty()) NewEntryGraphToken = (*NewEntry)->GraphStableId;
			FString OldEntryId = OldEntry.StableId;
			FString NewEntryId = (*NewEntry)->StableId;
			if (!OldEntry.GraphStableId.IsEmpty()) OldEntryId.ReplaceInline(
				*OldEntry.GraphStableId, *OldEntryGraphToken, ESearchCase::CaseSensitive);
			if (!(*NewEntry)->GraphStableId.IsEmpty()) NewEntryId.ReplaceInline(
				*(*NewEntry)->GraphStableId, *NewEntryGraphToken, ESearchCase::CaseSensitive);
			DiffScalar(OldEntryId, NewEntryId,
				Path + TEXT("/stable-id"), Result);
			DiffScalar(OldEntry.EventName, (*NewEntry)->EventName,
				Path + TEXT("/event-name"), Result);
			DiffScalar(OldEntryGraphToken, NewEntryGraphToken,
				Path + TEXT("/graph-stable-id"), Result);
			DiffArguments(OldEntry.Arguments, (*NewEntry)->Arguments, Path, Result);
			DiffProperties(OldEntry.Properties, (*NewEntry)->Properties, Path, Result);
			DiffGraph(OldEntry.Graph, (*NewEntry)->Graph, Path, Result,
				&OldGraphTokens, &NewGraphTokens);
			NewEntries.Remove(OldEntry.Name);
		}
	}
	for (const TPair<FString, const FRigEntryAST*>& Pair : NewEntries)
		AddDifference(Result, TEXT("entry:") + Pair.Key, TEXT("missing"), TEXT("present"));

	TMap<FString, const FRigGraphAST*> NewGraphs;
	for (const FRigGraphAST& Graph : NewModule.Graphs)
	{
		const FString Token = NewGraphTokens.FindRef(Graph.StableId);
		if (!NewGraphTokenCollisions.Contains(Token)) NewGraphs.Add(Token, &Graph);
	}
	for (const FRigGraphAST& OldGraph : OldModule.Graphs)
	{
		const FString Token = OldGraphTokens.FindRef(OldGraph.StableId);
		if (!OldGraphTokenCollisions.Contains(Token))
		{
			if (const FRigGraphAST* const* NewGraph = NewGraphs.Find(Token))
			{
				DiffGraph(OldGraph, **NewGraph, TEXT("graph:") + OldGraph.StableId, Result,
					&OldGraphTokens, &NewGraphTokens);
				NewGraphs.Remove(Token);
				continue;
			}
		}
		AddDifference(Result, TEXT("graph:") + OldGraph.StableId,
			TEXT("present"), TEXT("missing"));
	}
	for (const TPair<FString, const FRigGraphAST*>& Pair : NewGraphs)
		AddDifference(Result, TEXT("graph:") + Pair.Value->StableId,
			TEXT("missing"), TEXT("present"));

	const FString OldNonGraph = FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
		OldModule, OldModule);
	const FString NewNonGraph = FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
		NewModule, OldModule);
	if (!Exact(OldNonGraph, NewNonGraph))
		AddDifference(Result, TEXT("module/canonical"),
			OldNonGraph, NewNonGraph);
	const FString OldGraphSnapshot = FRigLangImporter::BuildGraphSemanticSnapshot(OldModule, OldModule);
	const FString NewGraphSnapshot = FRigLangImporter::BuildGraphSemanticSnapshot(NewModule, OldModule);
	if (!Exact(OldGraphSnapshot, NewGraphSnapshot))
		AddDifference(Result, TEXT("graph/canonical"), OldGraphSnapshot, NewGraphSnapshot);
	return Result;
}

FString FRigLangDiffResult::ToJson() const
{
	FString Json = TEXT("{\"difference_count\":") + FString::FromInt(Differences.Num())
		+ TEXT(",\"differences\":[");
	for (int32 Index = 0; Index < Differences.Num(); ++Index)
	{
		if (Index) Json += TEXT(",");
		const FRigLangDifference& Difference = Differences[Index];
		Json += TEXT("{\"path\":\"") + JsonEscape(Difference.Path)
			+ TEXT("\",\"old\":\"") + JsonEscape(Difference.OldValue)
			+ TEXT("\",\"new\":\"") + JsonEscape(Difference.NewValue) + TEXT("\"}");
	}
	return Json + TEXT("]}");
}
