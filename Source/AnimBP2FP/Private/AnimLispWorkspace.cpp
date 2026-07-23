// AnimLispWorkspace.cpp - Cross-file AnimLang and RigLang symbol workspace
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLispWorkspace.h"

#include "AnimLangParser.h"
#include "AnimLangTokenizer.h"
#include "RigLangParser.h"
#include "RigVMCore/RigVMRegistry.h"

namespace
{
bool AreRigVMLinkTypesCompatible(const FAnimLispTypeRef& A, const FAnimLispTypeRef& B)
{
	if (A == B) return true;
	const FRigVMRegistry_RWLock& Registry = FRigVMRegistry::Get();
	const TRigVMTypeIndex AIndex = Registry.GetTypeIndexFromCPPType(A.CPPType);
	const TRigVMTypeIndex BIndex = Registry.GetTypeIndexFromCPPType(B.CPPType);
	return AIndex != INDEX_NONE && BIndex != INDEX_NONE
		&& Registry.CanMatchTypes(AIndex, BIndex, true);
}

struct FTokenForm
{
	int32 Start = INDEX_NONE;
	int32 End = INDEX_NONE;
	FString Head;
	FAnimLangSourceLoc Location;
};

struct FWorkspaceUse
{
	FString QualifiedName;
	FRigFunctionIdentifierAST FunctionIdentifier;
	bool bLegacyShortCall = false;
	EAnimLispCapability Required = EAnimLispCapability::DefinitionOnly;
	EAnimLispSymbolKind ExpectedKind = EAnimLispSymbolKind::RigEntry;
	FAnimLangSourceLoc Location;
};

struct FWorkspaceBinding
{
	FString AnimVariable;
	FString RigVariable;
	FAnimLangSourceLoc Location;
};

struct FWorkspaceRigInputUse
{
	FString QualifiedName;
	FAnimLispTypeRef ExpectedType;
	FAnimLangSourceLoc Location;
};

struct FWorkspaceRigNodeUse
{
	FString ImportAlias;
	FString EntryQualifiedName;
	FAnimLangSourceLoc Location;
	TArray<FWorkspaceRigInputUse> Inputs;
};

struct FWorkspaceModule
{
	bool bIndexable = false;
	FString SourceFile;
	FAnimLispModuleId Id;
	FString ContentHash;
	FAnimLangSourceLoc Location;
	TArray<FAnimLispImport> Imports;
	TArray<FWorkspaceUse> Uses;
	TArray<FWorkspaceBinding> Bindings;
	TArray<FWorkspaceRigNodeUse> RigNodes;
	TArray<int32> DefinitionIndices;
	TSharedPtr<FAnimGraphAST> Anim;
	TSharedPtr<FRigModuleAST> Rig;
	TArray<FRigLangParseError> ParseErrors;
};

TArray<FTokenForm> FindForms(
	const TArray<FAnimLangToken>& Tokens,
	const int32 RangeStart,
	const int32 RangeEnd,
	const int32 DesiredOpenDepth)
{
	TArray<FTokenForm> Forms;
	int32 Depth = 0;
	int32 FormStart = INDEX_NONE;
	for (int32 Index = RangeStart; Index < RangeEnd; ++Index)
	{
		if (Tokens[Index].Type == EAnimLangTokenType::LParen)
		{
			if (Depth == DesiredOpenDepth)
			{
				FormStart = Index;
			}
			++Depth;
		}
		else if (Tokens[Index].Type == EAnimLangTokenType::RParen)
		{
			--Depth;
			if (Depth == DesiredOpenDepth && FormStart != INDEX_NONE)
			{
				FTokenForm& Form = Forms.AddDefaulted_GetRef();
				Form.Start = FormStart;
				Form.End = Index;
				if (Tokens.IsValidIndex(FormStart + 1)
					&& Tokens[FormStart + 1].Type == EAnimLangTokenType::Identifier)
				{
					Form.Head = Tokens[FormStart + 1].Value;
					Form.Location = Tokens[FormStart + 1].Span;
				}
				FormStart = INDEX_NONE;
			}
		}
	}
	if (FormStart != INDEX_NONE && Tokens.IsValidIndex(FormStart + 1))
	{
		FTokenForm& Form = Forms.AddDefaulted_GetRef();
		Form.Start = FormStart;
		Form.End = RangeEnd - 1;
		if (Tokens[FormStart + 1].Type == EAnimLangTokenType::Identifier)
		{
			Form.Head = Tokens[FormStart + 1].Value;
			Form.Location = Tokens[FormStart + 1].Span;
		}
	}
	return Forms;
}

TArray<FTokenForm> FindTopLevelForms(const TArray<FAnimLangToken>& Tokens)
{
	return FindForms(Tokens, 0, Tokens.Num(), 0);
}

TArray<FTokenForm> FindChildForms(const TArray<FAnimLangToken>& Tokens, const FTokenForm& Parent)
{
	TArray<FTokenForm> Forms;
	int32 Depth = 0;
	int32 FormStart = INDEX_NONE;
	for (int32 Index = Parent.Start + 2; Index < Parent.End; ++Index)
	{
		if (Tokens[Index].Type == EAnimLangTokenType::LParen)
		{
			if (Depth == 0) FormStart = Index;
			++Depth;
		}
		else if (Tokens[Index].Type == EAnimLangTokenType::RParen)
		{
			--Depth;
			if (Depth == 0 && FormStart != INDEX_NONE)
			{
				FTokenForm& Form = Forms.AddDefaulted_GetRef();
				Form.Start = FormStart;
				Form.End = Index;
				if (Tokens.IsValidIndex(FormStart + 1)
					&& Tokens[FormStart + 1].Type == EAnimLangTokenType::Identifier)
				{
					Form.Head = Tokens[FormStart + 1].Value;
					Form.Location = Tokens[FormStart + 1].Span;
				}
				FormStart = INDEX_NONE;
			}
		}
	}
	return Forms;
}

const FAnimLangToken* FindProperty(
	const TArray<FAnimLangToken>& Tokens,
	const FTokenForm& Form,
	const FString& Key)
{
	int32 Depth = 0;
	for (int32 Index = Form.Start + 2; Index < Form.End; ++Index)
	{
		if (Tokens[Index].Type == EAnimLangTokenType::LParen)
		{
			++Depth;
			continue;
		}
		if (Tokens[Index].Type == EAnimLangTokenType::RParen)
		{
			--Depth;
			continue;
		}
		if (Depth == 0
			&& Tokens[Index].Type == EAnimLangTokenType::Keyword
			&& Tokens[Index].Value == Key
			&& Index + 1 < Form.End)
		{
			return &Tokens[Index + 1];
		}
	}
	return nullptr;
}

int32 CountProperties(
	const TArray<FAnimLangToken>& Tokens,
	const FTokenForm& Form,
	const FString& Key)
{
	int32 Count = 0;
	int32 Depth = 0;
	for (int32 Index = Form.Start + 2; Index < Form.End; ++Index)
	{
		if (Tokens[Index].Type == EAnimLangTokenType::LParen)
		{
			++Depth;
			continue;
		}
		if (Tokens[Index].Type == EAnimLangTokenType::RParen)
		{
			--Depth;
			continue;
		}
		if (Depth == 0
			&& Tokens[Index].Type == EAnimLangTokenType::Keyword
			&& Tokens[Index].Value == Key)
		{
			++Count;
		}
	}
	return Count;
}

FString SymbolKey(const FAnimLispSymbolId& Id)
{
	return Id.Module.ToString() + TEXT("|") + Id.QualifiedName + TEXT("|")
		+ FString::FromInt(static_cast<int32>(Id.Kind));
}

FString DefinitionLookupKey(const FAnimLispModuleId& Module, const FString& Name)
{
	return Module.ToString() + TEXT("|") + Name;
}

bool SplitQualifiedName(const FString& QualifiedName, FString& OutAlias, FString& OutName)
{
	int32 Slash = INDEX_NONE;
	int32 Dot = INDEX_NONE;
	QualifiedName.FindChar(TEXT('/'), Slash);
	QualifiedName.FindChar(TEXT('.'), Dot);
	int32 Separator = Slash;
	if (Separator == INDEX_NONE || (Dot != INDEX_NONE && Dot < Separator)) Separator = Dot;
	if (Separator <= 0 || Separator >= QualifiedName.Len() - 1) return false;
	OutAlias = QualifiedName.Left(Separator);
	OutName = QualifiedName.Mid(Separator + 1);
	return true;
}

bool MatchesCapability(const EAnimLispCapability Available, const EAnimLispCapability Required)
{
	return Available == Required
		|| Required == EAnimLispCapability::DefinitionOnly;
}

void CollectPins(const TArray<FRigPinAST>& Pins, TMap<FString, const FRigPinAST*>& OutPins)
{
	for (const FRigPinAST& Pin : Pins)
	{
		OutPins.FindOrAdd(Pin.Path, &Pin);
		CollectPins(Pin.SubPins, OutPins);
	}
}

void LintRigGraph(const FRigGraphAST& Graph, FAnimLangDiagnostics& OutDiag)
{
	TMap<FString, const FRigNodeAST*> Nodes;
	TMap<FString, TMap<FString, const FRigPinAST*>> PinsByNode;
	TMap<FString, FString> GuidOwners;
	for (const FRigNodeAST& Node : Graph.Nodes)
	{
		Nodes.FindOrAdd(Node.StableId, &Node);
		CollectPins(Node.Pins, PinsByNode.FindOrAdd(Node.StableId));
		if (!Node.Guid.IsEmpty())
		{
			if (const FString* FirstOwner = GuidOwners.Find(Node.Guid))
			{
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Semantic,
					FString::Printf(
						TEXT("Duplicate node GUID '%s' on '%s' and '%s'"),
						*Node.Guid,
						**FirstOwner,
						*Node.StableId),
					Node.Location);
			}
			else
			{
				GuidOwners.Add(Node.Guid, Node.StableId);
			}
		}
	}

	TMap<FString, TArray<FString>> Adjacency;
	TMap<FString, int32> Incoming;
	for (const FRigNodeAST& Node : Graph.Nodes) Incoming.Add(Node.StableId, 0);
	for (const FRigLinkAST& Link : Graph.Links)
	{
		const FString SourceEndpoint = Link.SourceNodeId + TEXT(".") + Link.SourcePinPath;
		const FString TargetEndpoint = Link.TargetNodeId + TEXT(".") + Link.TargetPinPath;
		const FRigNodeAST* const* SourceNode = Nodes.Find(Link.SourceNodeId);
		const FRigNodeAST* const* TargetNode = Nodes.Find(Link.TargetNodeId);
		const FRigPinAST* SourcePin = nullptr;
		const FRigPinAST* TargetPin = nullptr;
		if (SourceNode == nullptr)
		{
			OutDiag.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Semantic,
				TEXT("Link endpoint '") + SourceEndpoint + TEXT("' does not exist"),
				Link.Location);
		}
		else if (const TMap<FString, const FRigPinAST*>* Pins = PinsByNode.Find(Link.SourceNodeId))
		{
			SourcePin = Pins->FindRef(Link.SourcePinPath);
			if (SourcePin == nullptr)
			{
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Semantic,
					TEXT("Link endpoint '") + SourceEndpoint + TEXT("' does not exist"),
					Link.Location);
			}
		}
		if (TargetNode == nullptr)
		{
			OutDiag.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Semantic,
				TEXT("Link endpoint '") + TargetEndpoint + TEXT("' does not exist"),
				Link.Location);
		}
		else if (const TMap<FString, const FRigPinAST*>* Pins = PinsByNode.Find(Link.TargetNodeId))
		{
			TargetPin = Pins->FindRef(Link.TargetPinPath);
			if (TargetPin == nullptr)
			{
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Semantic,
					TEXT("Link endpoint '") + TargetEndpoint + TEXT("' does not exist"),
					Link.Location);
			}
		}
		bool bValidLink = SourcePin != nullptr && TargetPin != nullptr;
		if (SourcePin != nullptr
			&& SourcePin->Direction != ERigPinDirection::Output
			&& SourcePin->Direction != ERigPinDirection::IO)
		{
			FAnimLangDiagnostic Diagnostic(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Semantic,
				TEXT("Pin '") + SourceEndpoint + TEXT("' cannot be used as a link source"),
				SourcePin->Location);
			if (TargetPin != nullptr)
			{
				Diagnostic.AddRelatedLocation(TargetPin->Location, TEXT("Link target declared here"));
			}
			OutDiag.Add(Diagnostic);
			bValidLink = false;
		}
		if (TargetPin != nullptr
			&& TargetPin->Direction != ERigPinDirection::Input
			&& TargetPin->Direction != ERigPinDirection::IO)
		{
			FAnimLangDiagnostic Diagnostic(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Semantic,
				TEXT("Pin '") + TargetEndpoint + TEXT("' cannot be used as a link target"),
				TargetPin->Location);
			if (SourcePin != nullptr)
			{
				Diagnostic.AddRelatedLocation(SourcePin->Location, TEXT("Link source declared here"));
			}
			OutDiag.Add(Diagnostic);
			bValidLink = false;
		}
		if (SourcePin != nullptr && TargetPin != nullptr
			&& SourcePin->bExecuteContext != TargetPin->bExecuteContext)
		{
			FAnimLangDiagnostic Diagnostic(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Semantic,
				TEXT("Connected pins '") + SourceEndpoint + TEXT("' and '") + TargetEndpoint
					+ TEXT("' have an execute-context mismatch"),
				SourcePin->Location);
			Diagnostic.AddRelatedLocation(TargetPin->Location, TEXT("Connected pin declared here"));
			OutDiag.Add(Diagnostic);
			bValidLink = false;
		}
		if (SourcePin != nullptr && TargetPin != nullptr
			&& !AreRigVMLinkTypesCompatible(SourcePin->Type, TargetPin->Type))
		{
			FAnimLangDiagnostic Diagnostic(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Type,
				TEXT("Connected pins '") + SourceEndpoint + TEXT("' and '") + TargetEndpoint
					+ TEXT("' have incompatible exact Unreal types"),
				SourcePin->Location);
			Diagnostic.AddRelatedLocation(TargetPin->Location, TEXT("Connected pin declared here"));
			OutDiag.Add(Diagnostic);
			bValidLink = false;
		}
		if (bValidLink)
		{
			Adjacency.FindOrAdd(Link.SourceNodeId).AddUnique(Link.TargetNodeId);
			Incoming.FindOrAdd(Link.TargetNodeId) += 1;
		}
	}

	TMap<FString, int32> VisitIndex;
	TMap<FString, int32> LowLink;
	TArray<FString> ComponentStack;
	TSet<FString> OnComponentStack;
	TSet<FString> CycleNodes;
	int32 NextVisitIndex = 0;
	TFunction<void(const FString&)> VisitComponent;
	VisitComponent = [&](const FString& NodeId)
	{
		const int32 NodeVisitIndex = NextVisitIndex++;
		VisitIndex.Add(NodeId, NodeVisitIndex);
		LowLink.Add(NodeId, NodeVisitIndex);
		ComponentStack.Add(NodeId);
		OnComponentStack.Add(NodeId);

		TArray<FString> Targets = Adjacency.FindRef(NodeId);
		Targets.Sort();
		for (const FString& Target : Targets)
		{
			if (!VisitIndex.Contains(Target))
			{
				VisitComponent(Target);
				LowLink.FindChecked(NodeId) = FMath::Min(LowLink.FindChecked(NodeId), LowLink.FindChecked(Target));
			}
			else if (OnComponentStack.Contains(Target))
			{
				LowLink.FindChecked(NodeId) = FMath::Min(LowLink.FindChecked(NodeId), VisitIndex.FindChecked(Target));
			}
		}

		if (LowLink.FindChecked(NodeId) != VisitIndex.FindChecked(NodeId)) return;
		TArray<FString> Component;
		FString Member;
		do
		{
			Member = ComponentStack.Pop(EAllowShrinking::No);
			OnComponentStack.Remove(Member);
			Component.Add(Member);
		}
		while (Member != NodeId);
		const bool bSelfCycle = Component.Num() == 1
			&& Adjacency.FindRef(Component[0]).Contains(Component[0]);
		if (Component.Num() <= 1 && !bSelfCycle) return;
		Component.Sort();
		for (const FString& CycleNode : Component) CycleNodes.Add(CycleNode);
		const FRigNodeAST* PrimaryNode = Nodes.FindRef(Component[0]);
		FAnimLangDiagnostic Diagnostic(
			EAnimLangDiagSeverity::Error,
			EAnimLangDiagCategory::Semantic,
			TEXT("Graph cycle: ") + FString::Join(Component, TEXT(" -> ")),
			PrimaryNode != nullptr ? PrimaryNode->Location : FAnimLangSourceLoc());
		for (int32 Index = 1; Index < Component.Num(); ++Index)
		{
			if (const FRigNodeAST* RelatedNode = Nodes.FindRef(Component[Index]))
			{
				Diagnostic.AddRelatedLocation(RelatedNode->Location, TEXT("Cycle member declared here"));
			}
		}
		OutDiag.Add(Diagnostic);
	};
	TArray<FString> SortedNodeIds;
	Nodes.GetKeys(SortedNodeIds);
	SortedNodeIds.Sort();
	for (const FString& NodeId : SortedNodeIds)
	{
		if (!VisitIndex.Contains(NodeId)) VisitComponent(NodeId);
	}

	TArray<FString> Queue;
	for (const FRigNodeAST& Node : Graph.Nodes)
	{
		if (Incoming.FindRef(Node.StableId) == 0) Queue.Add(Node.StableId);
	}
	TArray<FString> CycleSeeds = CycleNodes.Array();
	CycleSeeds.Sort();
	Queue.Append(CycleSeeds);
	Queue.Sort();
	TSet<FString> Reachable;
	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		const FString NodeId = Queue[QueueIndex];
		if (Reachable.Contains(NodeId)) continue;
		Reachable.Add(NodeId);
		if (const TArray<FString>* Targets = Adjacency.Find(NodeId))
		{
			TArray<FString> SortedTargets = *Targets;
			SortedTargets.Sort();
			for (const FString& Target : SortedTargets) Queue.Add(Target);
		}
	}
	for (const FRigNodeAST& Node : Graph.Nodes)
	{
		if (!Reachable.Contains(Node.StableId)) continue;
		if (Node.Coverage == ERigNodeCoverage::Lossy)
		{
			OutDiag.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Semantic,
				FString::Printf(TEXT("Reachable lossy node '%s'"), *Node.StableId),
				Node.Location);
		}
		else if (Node.Coverage == ERigNodeCoverage::Unsupported)
		{
			OutDiag.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Semantic,
				FString::Printf(TEXT("Reachable unsupported node '%s'"), *Node.StableId),
				Node.Location);
		}
	}
}

void LintRigModule(const FRigModuleAST& Rig, FAnimLangDiagnostics& OutDiag)
{
	TMap<FString, const FRigHierarchyElementAST*> ElementsByName;
	TMap<FString, const FRigHierarchyElementAST*> ElementsById;
	for (const FRigHierarchyElementAST& Element : Rig.Hierarchy)
	{
		ElementsByName.FindOrAdd(Element.Name, &Element);
		ElementsById.FindOrAdd(Element.StableId, &Element);
	}
	for (const FRigHierarchyElementAST& Element : Rig.Hierarchy)
	{
		if (!Element.ParentName.IsEmpty() && !ElementsByName.Contains(Element.ParentName))
		{
			OutDiag.Add(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Semantic,
				FString::Printf(
					TEXT("Hierarchy element '%s' has missing parent '%s'"),
					*Element.Name,
					*Element.ParentName),
				Element.Location);
		}
		TSet<FString> SeenParentIds;
		for (const FRigHierarchyParentAST& Parent : Element.Parents)
		{
			if (!ElementsById.Contains(Parent.StableId))
			{
				OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
					FString::Printf(TEXT("Hierarchy element '%s' has missing parent '%s'"), *Element.Name, *Parent.StableId), Parent.Location);
			}
			if (SeenParentIds.Contains(Parent.StableId))
			{
				OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
					FString::Printf(TEXT("Hierarchy element '%s' has duplicate parent '%s'"), *Element.Name, *Parent.StableId), Parent.Location);
			}
			SeenParentIds.Add(Parent.StableId);
			auto FiniteWeight = [](const FRigHierarchyWeightAST& Weight)
			{
				return FMath::IsFinite(Weight.Location) && FMath::IsFinite(Weight.Rotation) && FMath::IsFinite(Weight.Scale);
			};
			if (!FiniteWeight(Parent.CurrentWeight) || !FiniteWeight(Parent.InitialWeight))
				OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic, TEXT("Hierarchy parent weight must be finite"), Parent.Location);
		}
		TSet<uint8> SeenTransformRoles;
		for (const FRigHierarchyTransformAST& Transform : Element.Transforms)
		{
			const uint8 Role = static_cast<uint8>(Transform.Role);
			if (SeenTransformRoles.Contains(Role))
				OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
					FString::Printf(TEXT("Hierarchy element '%s' has duplicate transform role"), *Element.Name), Transform.Location);
			SeenTransformRoles.Add(Role);
			if (Transform.Value.ContainsNaN())
				OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic, TEXT("Hierarchy transform must contain finite values"), Transform.Location);
		}
		for (const FRigHierarchyStateAST& State : Element.States)
		{
			if (State.Kind == ERigHierarchyStateKind::ControlValue)
			{
				int32 RequiredComponents = 0;
				if (State.Type == TEXT("Position") || State.Type == TEXT("Scale") || State.Type == TEXT("Rotator") || State.Type == TEXT("Vector2D")) RequiredComponents = 3;
				else if (State.Type == TEXT("Transform")) RequiredComponents = 10;
				else if (State.Type == TEXT("TransformNoScale")) RequiredComponents = 7;
				else if (State.Type == TEXT("EulerTransform")) RequiredComponents = 9;
				else if (State.Type != TEXT("Bool") && State.Type != TEXT("Float") && State.Type != TEXT("ScaleFloat") && State.Type != TEXT("Integer"))
					OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic, TEXT("Invalid control value type"), State.Location);
				if (RequiredComponents > 0 && State.Components.Num() != RequiredComponents)
					OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
						FString::Printf(TEXT("Control value type '%s' requires %d components"), *State.Type, RequiredComponents), State.Location);
			}
		}
		TSet<FString> SeenMetadataNames;
		for (const FRigHierarchyMetadataAST& Metadata : Element.Metadata)
		{
			if (SeenMetadataNames.Contains(Metadata.Name))
				OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
					FString::Printf(TEXT("Hierarchy element '%s' has duplicate metadata '%s'"), *Element.Name, *Metadata.Name), Metadata.Location);
			SeenMetadataNames.Add(Metadata.Name);
		}
	}

	TSet<FString> Finished;
	bool bCycleReported = false;
	for (const FRigHierarchyElementAST& Start : Rig.Hierarchy)
	{
		if (bCycleReported || Finished.Contains(Start.Name)) continue;
		TArray<FString> Path;
		TMap<FString, int32> Position;
		FString Current = Start.Name;
		while (!Current.IsEmpty() && ElementsByName.Contains(Current) && !Finished.Contains(Current))
		{
			if (const int32* CycleStart = Position.Find(Current))
			{
				TArray<FString> Cycle;
				for (int32 Index = *CycleStart; Index < Path.Num(); ++Index) Cycle.Add(Path[Index]);
				Cycle.Add(Current);
				const FRigHierarchyElementAST* Element = ElementsByName.FindRef(Current);
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Semantic,
					TEXT("Hierarchy cycle: ") + FString::Join(Cycle, TEXT(" -> ")),
					Element != nullptr ? Element->Location : Start.Location);
				bCycleReported = true;
				break;
			}
			Position.Add(Current, Path.Num());
			Path.Add(Current);
			const FRigHierarchyElementAST* Element = ElementsByName.FindRef(Current);
			Current = Element != nullptr ? Element->ParentName : FString();
		}
		for (const FString& Name : Path) Finished.Add(Name);
	}

	TMap<FString, uint8> TypedParentVisit;
	TArray<FString> TypedParentPath;
	bool bTypedCycleReported = false;
	TFunction<void(const FString&)> VisitTypedParents = [&](const FString& ElementId)
	{
		if (bTypedCycleReported || TypedParentVisit.FindRef(ElementId) == 2) return;
		if (TypedParentVisit.FindRef(ElementId) == 1)
		{
			const int32 CycleStart = TypedParentPath.Find(ElementId);
			TArray<FString> Cycle;
			const int32 FirstIndex = CycleStart == INDEX_NONE ? 0 : CycleStart;
			for (int32 Index = FirstIndex; Index < TypedParentPath.Num(); ++Index) Cycle.Add(TypedParentPath[Index]);
			Cycle.Add(ElementId);
			const FRigHierarchyElementAST* Element = ElementsById.FindRef(ElementId);
			OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
				TEXT("Hierarchy multi-parent cycle: ") + FString::Join(Cycle, TEXT(" -> ")),
				Element ? Element->Location : FAnimLangSourceLoc());
			bTypedCycleReported = true;
			return;
		}
		TypedParentVisit.Add(ElementId, 1);
		TypedParentPath.Add(ElementId);
		if (const FRigHierarchyElementAST* Element = ElementsById.FindRef(ElementId))
			for (const FRigHierarchyParentAST& Parent : Element->Parents)
				if (ElementsById.Contains(Parent.StableId)) VisitTypedParents(Parent.StableId);
		TypedParentPath.Pop(EAllowShrinking::No);
		TypedParentVisit.Add(ElementId, 2);
	};
	for (const FRigHierarchyElementAST& Element : Rig.Hierarchy) VisitTypedParents(Element.StableId);

	if (!Rig.Graphs.IsEmpty())
	{
		for (const FRigGraphAST& Graph : Rig.Graphs) LintRigGraph(Graph, OutDiag);
	}
	else
	{
		for (const FRigFunctionAST& Function : Rig.Functions) LintRigGraph(Function.Graph, OutDiag);
		for (const FRigEntryAST& Entry : Rig.Entries) LintRigGraph(Entry.Graph, OutDiag);
	}
}

FAnimLispTypeRef RigFunctionTypeSignature(const FRigFunctionAST& Function)
{
	auto Atom = [](const FString& Value)
	{
		return FString::Printf(TEXT("%d#%s"), Value.Len(), *Value);
	};
	auto DirectionText = [](const ERigPinDirection Direction) -> const TCHAR*
	{
		switch (Direction)
		{
		case ERigPinDirection::Output: return TEXT("output");
		case ERigPinDirection::IO: return TEXT("io");
		case ERigPinDirection::Visible: return TEXT("visible");
		case ERigPinDirection::Hidden: return TEXT("hidden");
		case ERigPinDirection::Invalid: return TEXT("invalid");
		default: return TEXT("input");
		}
	};
	TArray<FString> Arguments;
	const TArray<FRigCallableArgumentAST>* OrderedArguments = &Function.Arguments;
	TArray<FRigCallableArgumentAST> LegacyArguments;
	if (OrderedArguments->IsEmpty() && (!Function.Inputs.IsEmpty() || !Function.Outputs.IsEmpty()))
	{
		LegacyArguments = Function.Inputs;
		LegacyArguments.Append(Function.Outputs);
		OrderedArguments = &LegacyArguments;
	}
	for (const FRigCallableArgumentAST& Argument : *OrderedArguments)
	{
		Arguments.Add(
			FString(DirectionText(Argument.Direction)) + TEXT("|")
			+ Atom(Argument.Name) + TEXT("|")
			+ Atom(Argument.Type.CPPType) + TEXT("|")
			+ Atom(Argument.Type.CPPTypeObject) + TEXT("|")
			+ Atom(Argument.Type.ContainerType) + TEXT("|")
			+ Atom(Argument.DefaultValue) + TEXT("|")
			+ (Argument.bExecuteContext ? TEXT("execute") : TEXT("value")) + TEXT("|")
			+ (Argument.bConstant ? TEXT("const") : TEXT("mutable")) + TEXT("|")
			+ (Argument.bInputVariable ? TEXT("input-variable") : TEXT("value")));
	}
	FAnimLispTypeRef Signature;
	Signature.CPPType = TEXT("rig-fn(") + FString::Join(Arguments, TEXT(";"))
		+ TEXT(")->") + Atom(Function.ReturnCPPType.IsEmpty() ? TEXT("void") : Function.ReturnCPPType);
	return Signature;
}
}

struct FAnimLispWorkspace::FImpl
{
	TMap<FString, FString> Sources;
	TArray<FWorkspaceModule> Modules;
	TArray<FAnimLispDefinition> Definitions;
	TArray<FAnimLispReference> References;
	TMap<FString, int32> ModuleBySource;
	TMap<FString, int32> ModuleByIdentity;
	TMap<FString, int32> FailedModuleByIdentity;
	TMap<FString, int32> DefinitionByModuleAndName;
	TMap<FString, ERigVariableAccess> RigVariableAccessBySymbol;
	TMap<FString, FRigFunctionIdentifierAST> RigFunctionIdentifierBySymbol;
	TSet<FString> QuarantinedModuleIdentities;
	TSet<int32> QuarantinedDefinitionIndices;
	TArray<FAnimLangDiagnostic> EarlyDiagnostics;
	bool bLastBuildSucceeded = false;
	bool bAllowLegacyExternalRigs = false;

	void ResetBuild()
	{
		bLastBuildSucceeded = false;
		Modules.Reset();
		Definitions.Reset();
		References.Reset();
		ModuleBySource.Reset();
		ModuleByIdentity.Reset();
		FailedModuleByIdentity.Reset();
		DefinitionByModuleAndName.Reset();
		RigVariableAccessBySymbol.Reset();
		RigFunctionIdentifierBySymbol.Reset();
		QuarantinedModuleIdentities.Reset();
		QuarantinedDefinitionIndices.Reset();
		EarlyDiagnostics.Reset();
	}

	int32 AddDefinition(
		FWorkspaceModule& Module,
		const FString& Name,
		const EAnimLispSymbolKind Kind,
		const EAnimLispCapability Capability,
		const FAnimLispTypeRef& Type,
		const FAnimLangSourceLoc& Location,
		const FString& Guid)
	{
		FAnimLispDefinition& Definition = Definitions.AddDefaulted_GetRef();
		Definition.Id.Module = Module.Id;
		Definition.Id.QualifiedName = Name;
		Definition.Id.Kind = Kind;
		Definition.Capability = Capability;
		Definition.TypeSignature = Type;
		Definition.Location = Location;
		Definition.Guid = Guid;
		const int32 Index = Definitions.Num() - 1;
		Module.DefinitionIndices.Add(Index);
		DefinitionByModuleAndName.FindOrAdd(DefinitionLookupKey(Module.Id, Name), Index);
		return Index;
	}

	void ParseRigSource(const FString& SourceFile, const FString& Source)
	{
		TArray<FRigLangParseError> Errors;
		const TSharedPtr<FRigModuleAST> Rig = FRigLangParser::Parse(Source, SourceFile, Errors);
		if (!Rig.IsValid())
		{
			TArray<FAnimLangParseError> TokenErrors;
			const TArray<FAnimLangToken> Tokens = FAnimLangTokenizer::Tokenize(Source, SourceFile, &TokenErrors);
			const TArray<FTokenForm> Forms = FindTopLevelForms(Tokens);
			const FTokenForm* Header = Forms.FindByPredicate([](const FTokenForm& Form)
			{
				return Form.Head == TEXT("rig-module");
			});
			const FAnimLangToken* Asset = Header != nullptr
				? FindProperty(Tokens, *Header, TEXT("asset"))
				: nullptr;
			for (const FTokenForm& Form : Forms)
			{
				if (Form.Head == TEXT("import-anim"))
				{
					EarlyDiagnostics.Emplace(
						EAnimLangDiagSeverity::Error,
						EAnimLangDiagCategory::Module,
						TEXT("Rig module cannot import Anim module"),
						Form.Location);
				}
			}
			if (Asset == nullptr)
			{
				if (Errors.IsEmpty())
				{
					FAnimLangSourceLoc Location;
					Location.SourceFile = SourceFile;
					EarlyDiagnostics.Emplace(
						EAnimLangDiagSeverity::Error,
						EAnimLangDiagCategory::Parse,
						TEXT("Invalid Rig module header"),
						Location);
				}
				for (const FRigLangParseError& Error : Errors)
				{
					EarlyDiagnostics.Emplace(
						EAnimLangDiagSeverity::Error,
						EAnimLangDiagCategory::Parse,
						Error.Message,
						Error.Location);
				}
				return;
			}

			FWorkspaceModule& Failed = Modules.AddDefaulted_GetRef();
			Failed.SourceFile = SourceFile;
			Failed.Id = FAnimLispModuleId::FromAssetPath(Asset->Value, EAnimLispModuleKind::Rig);
			Failed.Location = Header->Location;
			Failed.ParseErrors = MoveTemp(Errors);
			if (const FAnimLangToken* Hash = FindProperty(Tokens, *Header, TEXT("content-hash")))
			{
				Failed.ContentHash = Hash->Value;
			}
			FailedModuleByIdentity.FindOrAdd(Failed.Id.ToString(), Modules.Num() - 1);
			return;
		}

		FWorkspaceModule& Module = Modules.AddDefaulted_GetRef();
		Module.bIndexable = true;
		Module.SourceFile = SourceFile;
		Module.Id = Rig->Header.ModuleId;
		Module.ContentHash = Rig->Header.ContentHash;
		Module.Location = Rig->Header.Location;
		Module.Rig = Rig;
		Module.ParseErrors = MoveTemp(Errors);
		for (const FRigImportAST& Import : Rig->Imports) Module.Imports.Add(Import.Import);
		ModuleBySource.Add(SourceFile, Modules.Num() - 1);
		ModuleByIdentity.FindOrAdd(Module.Id.ToString(), Modules.Num() - 1);

		for (const FRigVariableAST& Variable : Rig->Variables)
		{
			const FString RuntimeName = AnimLispStableRuntimeSymbol(Variable.Name);
			const EAnimLispCapability Capability = Variable.Access == ERigVariableAccess::Internal
				? EAnimLispCapability::DefinitionOnly
				: EAnimLispCapability::AnimRuntimeReference;
			const int32 DefinitionIndex = AddDefinition(
				Module,
				RuntimeName,
				EAnimLispSymbolKind::RigVariable,
				Capability,
				Variable.Type,
				Variable.Location,
				Variable.StableId);
			RigVariableAccessBySymbol.Add(SymbolKey(Definitions[DefinitionIndex].Id), Variable.Access);
		}
		for (const FRigFunctionAST& Function : Rig->Functions)
		{
			const EAnimLispCapability Capability = Function.Visibility == TEXT("internal")
				? EAnimLispCapability::DefinitionOnly
				: EAnimLispCapability::RigCall;
			const int32 DefinitionIndex = AddDefinition(Module, Function.Name,
				EAnimLispSymbolKind::RigFunction, Capability,
				RigFunctionTypeSignature(Function), Function.Location, Function.StableId);
			RigFunctionIdentifierBySymbol.Add(
				SymbolKey(Definitions[DefinitionIndex].Id), Function.FunctionIdentifier);
		}
		for (const FRigEntryAST& Entry : Rig->Entries)
		{
			AddDefinition(Module, Entry.Name, EAnimLispSymbolKind::RigEntry,
				EAnimLispCapability::AnimRuntimeReference, FAnimLispTypeRef(), Entry.Location, Entry.StableId);
		}
		auto CollectCalls = [&Module](const FRigGraphAST& Graph)
		{
			for (const FRigNodeAST& Node : Graph.Nodes)
			{
				if (Node.Kind != ERigNodeKind::Call) continue;
				FWorkspaceUse& Use = Module.Uses.AddDefaulted_GetRef();
				Use.QualifiedName = Node.FunctionName;
				Use.FunctionIdentifier = Node.FunctionIdentifier;
				Use.bLegacyShortCall = !Node.FunctionIdentifier.IsComplete();
				Use.Required = EAnimLispCapability::RigCall;
				Use.ExpectedKind = EAnimLispSymbolKind::RigFunction;
				Use.Location = Node.Location;
			}
		};
		if (!Rig->Graphs.IsEmpty())
			for (const FRigGraphAST& Graph : Rig->Graphs) CollectCalls(Graph);
		else
		{
			for (const FRigFunctionAST& Function : Rig->Functions) CollectCalls(Function.Graph);
			for (const FRigEntryAST& Entry : Rig->Entries) CollectCalls(Entry.Graph);
		}
	}

	void ParseAnimSource(const FString& SourceFile, const FString& Source)
	{
		FString CanonicalProbeSource = Source;
		if (!CanonicalProbeSource.IsEmpty() && CanonicalProbeSource[0] == 0xFEFF)
		{
			CanonicalProbeSource.RemoveAt(0, 1, EAllowShrinking::No);
		}
		TArray<FAnimLangParseError> ProbeErrors;
		const TArray<FAnimLangToken> ProbeTokens = FAnimLangTokenizer::Tokenize(
			CanonicalProbeSource, SourceFile, &ProbeErrors);
		const TArray<FTokenForm> ProbeForms = FindTopLevelForms(ProbeTokens);
		if (!ProbeForms.IsEmpty() && ProbeForms[0].Head == TEXT("anim-blueprint"))
		{
			TArray<FAnimLangParseError> ParseErrors;
			const TSharedPtr<FAnimGraphAST> AST = FAnimLangParser::Parse(CanonicalProbeSource, ParseErrors);
			for (const FAnimLangParseError& Error : ParseErrors)
			{
				FAnimLangSourceLoc Location = Error.Location;
				if (Location.SourceFile.IsEmpty()) Location.SourceFile = SourceFile;
				EarlyDiagnostics.Emplace(
					Error.bWarning ? EAnimLangDiagSeverity::Warning : EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Parse, Error.Message, Location);
			}
			if (!AST.IsValid() || ParseErrors.ContainsByPredicate(
				[](const FAnimLangParseError& Error) { return !Error.bWarning; }))
			{
				return;
			}

			FWorkspaceModule& Module = Modules.AddDefaulted_GetRef();
			Module.bIndexable = true;
			Module.SourceFile = SourceFile;
			Module.Id = FAnimLispModuleId::FromAssetPath(
				SourceFile + TEXT("#") + AST->Name, EAnimLispModuleKind::Anim);
			Module.Location.SourceFile = SourceFile;
			Module.Location.Line = 1;
			Module.Location.Column = 1;
			Module.Imports = AST->RigImports;
			Module.Anim = AST;
			for (FAnimLispImport& Import : Module.Imports)
			{
				if (Import.Location.SourceFile.IsEmpty()) Import.Location.SourceFile = SourceFile;
			}
			AST->VisitNodes([&Module, &SourceFile](const TSharedPtr<FAnimNodeAST>& Node)
			{
				if (!Node->RigBinding.IsSet()) return;
				const FAnimRigNodeBinding& Binding = Node->RigBinding.GetValue();
				FWorkspaceRigNodeUse& Use = Module.RigNodes.AddDefaulted_GetRef();
				Use.ImportAlias = Binding.ImportAlias;
				Use.EntryQualifiedName = Binding.EntryName.IsEmpty()
					? FString() : Binding.ImportAlias + TEXT("/") + Binding.EntryName;
				Use.Location = Node->Location;
				if (Use.Location.SourceFile.IsEmpty()) Use.Location.SourceFile = SourceFile;
				for (const FAnimRigInputBinding& Input : Binding.Inputs)
				{
					FWorkspaceRigInputUse& InputUse = Use.Inputs.AddDefaulted_GetRef();
					InputUse.QualifiedName = Binding.ImportAlias + TEXT("/") + Input.RigInputName;
					InputUse.ExpectedType = Input.ResolvedType;
					InputUse.Location = Input.Location;
					if (InputUse.Location.SourceFile.IsEmpty()) InputUse.Location.SourceFile = SourceFile;
				}
			});
			ModuleBySource.Add(SourceFile, Modules.Num() - 1);
			ModuleByIdentity.FindOrAdd(Module.Id.ToString(), Modules.Num() - 1);
			return;
		}

		TArray<FAnimLangParseError> TokenErrors;
		const TArray<FAnimLangToken> Tokens = FAnimLangTokenizer::Tokenize(Source, SourceFile, &TokenErrors);
		const TArray<FTokenForm> Forms = FindTopLevelForms(Tokens);
		TArray<const FTokenForm*> Headers;
		for (const FTokenForm& Form : Forms)
		{
			if (Form.Head == TEXT("anim-module")) Headers.Add(&Form);
		}

		TArray<FRigLangParseError> Errors;
		auto AddError = [&Errors, &SourceFile](const FString& Message, FAnimLangSourceLoc Location)
		{
			if (Location.SourceFile.IsEmpty()) Location.SourceFile = SourceFile;
			FRigLangParseError& Error = Errors.AddDefaulted_GetRef();
			Error.Message = Message;
			Error.Location = Location;
		};
		for (const FAnimLangParseError& TokenError : TokenErrors)
		{
			FAnimLangSourceLoc Location;
			Location.SourceFile = SourceFile;
			Location.Line = TokenError.Line;
			Location.Column = TokenError.Column;
			AddError(TokenError.Message, Location);
		}
		TArray<EAnimLangTokenType> Delimiters;
		for (const FAnimLangToken& Token : Tokens)
		{
			if (Token.Type == EAnimLangTokenType::LParen || Token.Type == EAnimLangTokenType::LBracket)
			{
				Delimiters.Add(Token.Type);
			}
			else if (Token.Type == EAnimLangTokenType::RParen || Token.Type == EAnimLangTokenType::RBracket)
			{
				const EAnimLangTokenType Expected = Token.Type == EAnimLangTokenType::RParen
					? EAnimLangTokenType::LParen
					: EAnimLangTokenType::LBracket;
				if (Delimiters.IsEmpty() || Delimiters.Last() != Expected)
				{
					AddError(TEXT("Mismatched Anim module delimiter"), Token.Span);
				}
				else
				{
					Delimiters.Pop(EAllowShrinking::No);
				}
			}
		}
		if (!Delimiters.IsEmpty())
		{
			FAnimLangSourceLoc Location;
			Location.SourceFile = SourceFile;
			Location.Line = Tokens.Last().Span.Line;
			Location.Column = Tokens.Last().Span.Column;
			AddError(TEXT("Unbalanced Anim module form"), Location);
		}
		if (Headers.IsEmpty())
		{
			FAnimLangSourceLoc Location;
			Location.SourceFile = SourceFile;
			EarlyDiagnostics.Emplace(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Module,
				TEXT("Missing anim-module header"),
				Location);
			for (const FRigLangParseError& Error : Errors)
			{
				EarlyDiagnostics.Emplace(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Parse,
					Error.Message,
					Error.Location);
			}
			return;
		}
		if (Headers.Num() > 1)
		{
			EarlyDiagnostics.Emplace(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Module,
				TEXT("Duplicate anim-module header"),
				Headers[1]->Location);
			AddError(TEXT("Duplicate anim-module header"), Headers[1]->Location);
		}
		const FTokenForm* Header = Headers[0];
		auto RequireProperty = [&Tokens, &AddError](
			const FTokenForm& Form,
			const FString& Key,
			const std::initializer_list<EAnimLangTokenType> AllowedTypes,
			const FString& AlternateKey = FString())
			-> const FAnimLangToken*
		{
			const int32 Count = CountProperties(Tokens, Form, Key)
				+ (AlternateKey.IsEmpty() ? 0 : CountProperties(Tokens, Form, AlternateKey));
			const FString DisplayKey = TEXT(":") + Key;
			if (Count == 0)
			{
				AddError(
					FString::Printf(TEXT("Form '%s' requires %s"), *Form.Head, *DisplayKey),
					Form.Location);
				return nullptr;
			}
			if (Count > 1)
			{
				AddError(
					FString::Printf(TEXT("Form '%s' repeats required key %s"), *Form.Head, *DisplayKey),
					Form.Location);
				return nullptr;
			}
			const FAnimLangToken* Value = FindProperty(Tokens, Form, Key);
			if (Value == nullptr && !AlternateKey.IsEmpty())
			{
				Value = FindProperty(Tokens, Form, AlternateKey);
			}
			bool bAllowedType = false;
			if (Value != nullptr)
			{
				for (const EAnimLangTokenType AllowedType : AllowedTypes)
				{
					if (Value->Type == AllowedType)
					{
						bAllowedType = true;
						break;
					}
				}
			}
			if (!bAllowedType || Value->Value.IsEmpty())
			{
				AddError(
					FString::Printf(TEXT("Form '%s' requires a valid %s value"), *Form.Head, *DisplayKey),
					Form.Location);
				return nullptr;
			}
			return Value;
		};
		auto ValidateOptionalProperty = [&Tokens, &AddError](
			const FTokenForm& Form,
			const FString& Key,
			const EAnimLangTokenType AllowedType,
			const bool bAllowEmpty)
		{
			const int32 Count = CountProperties(Tokens, Form, Key);
			if (Count == 0) return;
			const FString DisplayKey = TEXT(":") + Key;
			if (Count > 1)
			{
				AddError(
					FString::Printf(TEXT("Form '%s' repeats optional key %s"), *Form.Head, *DisplayKey),
					Form.Location);
				return;
			}
			const FAnimLangToken* Value = FindProperty(Tokens, Form, Key);
			if (Value == nullptr || Value->Type != AllowedType || (!bAllowEmpty && Value->Value.IsEmpty()))
			{
				AddError(
					FString::Printf(TEXT("Form '%s' requires a valid %s value when present"), *Form.Head, *DisplayKey),
					Form.Location);
			}
		};
		const FAnimLangToken* Asset = RequireProperty(
			*Header, TEXT("asset"), {EAnimLangTokenType::String});
		RequireProperty(*Header, TEXT("class"), {EAnimLangTokenType::String});
		RequireProperty(*Header, TEXT("version"), {EAnimLangTokenType::Integer});
		RequireProperty(*Header, TEXT("content-hash"), {EAnimLangTokenType::String});
		static const TSet<FString> AllowedForms = {
			TEXT("anim-module"), TEXT("import-rig"), TEXT("anim-variables"),
			TEXT("rig-entry"), TEXT("rig-call"), TEXT("bind-rig-variable"), TEXT("define")};
		TMap<FString, FAnimLangSourceLoc> FirstImportByAlias;
		for (const FTokenForm& Form : Forms)
		{
			if (Form.Head.IsEmpty() || !AllowedForms.Contains(Form.Head))
			{
				AddError(
					FString::Printf(TEXT("Unsupported Anim top-level form '%s'"), *Form.Head),
					Form.Location);
			}
			else if (Form.Head == TEXT("import-rig"))
			{
				RequireProperty(Form, TEXT("asset"), {EAnimLangTokenType::String});
				const FAnimLangToken* Alias = RequireProperty(
					Form,
					TEXT("alias"),
					{EAnimLangTokenType::Identifier, EAnimLangTokenType::String});
				if (Alias != nullptr)
				{
					if (const FAnimLangSourceLoc* FirstLocation = FirstImportByAlias.Find(Alias->Value))
					{
						FAnimLangDiagnostic Diagnostic(
							EAnimLangDiagSeverity::Error,
							EAnimLangDiagCategory::Parse,
							FString::Printf(TEXT("Import alias '%s' is repeated"), *Alias->Value),
							Form.Location);
						Diagnostic.AddRelatedLocation(*FirstLocation, TEXT("Alias first declared here"));
						EarlyDiagnostics.Add(Diagnostic);
						AddError(TEXT("Anim module import aliases must be unique"), Form.Location);
					}
					else
					{
						FirstImportByAlias.Add(Alias->Value, Form.Location);
					}
				}
				RequireProperty(
					Form,
					TEXT("content-hash"),
					{EAnimLangTokenType::String},
					TEXT("expected-hash"));
			}
			else if (Form.Head == TEXT("anim-variables"))
			{
				for (const FTokenForm& Variable : FindChildForms(Tokens, Form))
				{
					if (Variable.Head != TEXT("variable"))
					{
						FAnimLangSourceLoc Location = Variable.Location;
						if (Location.SourceFile.IsEmpty() && Tokens.IsValidIndex(Variable.Start))
						{
							Location = Tokens[Variable.Start].Span;
						}
						AddError(TEXT("anim-variables contains an unrecognized child form"), Location);
						continue;
					}
					RequireProperty(Variable, TEXT("id"), {EAnimLangTokenType::String});
					RequireProperty(Variable, TEXT("name"), {EAnimLangTokenType::String});
					RequireProperty(Variable, TEXT("cpp-type"), {EAnimLangTokenType::String});
					ValidateOptionalProperty(
						Variable, TEXT("cpp-type-object"), EAnimLangTokenType::String, true);
					ValidateOptionalProperty(
						Variable, TEXT("container-type"), EAnimLangTokenType::String, false);
				}
			}
			else if (Form.Head == TEXT("rig-entry") || Form.Head == TEXT("rig-call"))
			{
				RequireProperty(Form, TEXT("target"), {EAnimLangTokenType::String});
			}
			else if (Form.Head == TEXT("bind-rig-variable"))
			{
				RequireProperty(Form, TEXT("anim-variable"), {EAnimLangTokenType::String});
				RequireProperty(Form, TEXT("rig-variable"), {EAnimLangTokenType::String});
			}
		}
		if (!Errors.IsEmpty())
		{
			if (Asset != nullptr)
			{
				FWorkspaceModule& Failed = Modules.AddDefaulted_GetRef();
				Failed.SourceFile = SourceFile;
				Failed.Id = FAnimLispModuleId::FromAssetPath(Asset->Value, EAnimLispModuleKind::Anim);
				Failed.Location = Header->Location;
				Failed.ParseErrors = MoveTemp(Errors);
				if (const FAnimLangToken* Hash = FindProperty(Tokens, *Header, TEXT("content-hash")))
				{
					Failed.ContentHash = Hash->Value;
				}
				FailedModuleByIdentity.FindOrAdd(Failed.Id.ToString(), Modules.Num() - 1);
			}
			else
			{
				for (const FRigLangParseError& Error : Errors)
				{
					EarlyDiagnostics.Emplace(
						EAnimLangDiagSeverity::Error,
						EAnimLangDiagCategory::Parse,
						Error.Message,
						Error.Location);
				}
			}
			return;
		}

		FWorkspaceModule& Module = Modules.AddDefaulted_GetRef();
		Module.bIndexable = true;
		Module.SourceFile = SourceFile;
		Module.Id = FAnimLispModuleId::FromAssetPath(Asset->Value, EAnimLispModuleKind::Anim);
		Module.Location = Header->Location;
		if (const FAnimLangToken* Hash = FindProperty(Tokens, *Header, TEXT("content-hash")))
		{
			Module.ContentHash = Hash->Value;
		}
		ModuleBySource.Add(SourceFile, Modules.Num() - 1);
		ModuleByIdentity.FindOrAdd(Module.Id.ToString(), Modules.Num() - 1);

		for (const FTokenForm& Form : Forms)
		{
			if (Form.Head == TEXT("import-rig"))
			{
				const FAnimLangToken* ImportAsset = FindProperty(Tokens, Form, TEXT("asset"));
				const FAnimLangToken* Alias = FindProperty(Tokens, Form, TEXT("alias"));
				if (ImportAsset != nullptr && Alias != nullptr)
				{
					FAnimLispImport& Import = Module.Imports.AddDefaulted_GetRef();
					Import.Target = FAnimLispModuleId::FromAssetPath(ImportAsset->Value, EAnimLispModuleKind::Rig);
					Import.Alias = Alias->Value;
					Import.Location = Form.Location;
					const FAnimLangToken* Hash = FindProperty(Tokens, Form, TEXT("content-hash"));
					if (Hash == nullptr) Hash = FindProperty(Tokens, Form, TEXT("expected-hash"));
					if (Hash != nullptr)
					{
						Import.ExpectedHash = Hash->Value;
					}
				}
			}
			else if (Form.Head == TEXT("anim-variables"))
			{
				for (const FTokenForm& Variable : FindChildForms(Tokens, Form))
				{
					if (Variable.Head != TEXT("variable")) continue;
					const FAnimLangToken* Name = FindProperty(Tokens, Variable, TEXT("name"));
					if (Name == nullptr) continue;
					FAnimLispTypeRef Type;
					if (const FAnimLangToken* Value = FindProperty(Tokens, Variable, TEXT("cpp-type"))) Type.CPPType = Value->Value;
					if (const FAnimLangToken* Value = FindProperty(Tokens, Variable, TEXT("cpp-type-object"))) Type.CPPTypeObject = Value->Value;
					if (const FAnimLangToken* Value = FindProperty(Tokens, Variable, TEXT("container-type"))) Type.ContainerType = Value->Value;
					const FAnimLangToken* Id = FindProperty(Tokens, Variable, TEXT("id"));
					AddDefinition(Module, Name->Value, EAnimLispSymbolKind::AnimVariable,
						EAnimLispCapability::AnimRuntimeReference, Type, Variable.Location, Id != nullptr ? Id->Value : FString());
				}
			}
			else if (Form.Head == TEXT("define") && Tokens.IsValidIndex(Form.Start + 2))
			{
				const FAnimLangToken& Name = Tokens[Form.Start + 2];
				if (Name.Type == EAnimLangTokenType::String || Name.Type == EAnimLangTokenType::Identifier)
				{
					const FAnimLangToken* Id = FindProperty(Tokens, Form, TEXT("id"));
					AddDefinition(
						Module,
						Name.Value,
						EAnimLispSymbolKind::AnimDefine,
						EAnimLispCapability::AnimRuntimeReference,
						FAnimLispTypeRef(),
						Name.Span,
						Id != nullptr ? Id->Value : FString());
				}
			}
			else if (Form.Head == TEXT("rig-entry") || Form.Head == TEXT("rig-call"))
			{
				const FAnimLangToken* Target = FindProperty(Tokens, Form, TEXT("target"));
				if (Target != nullptr)
				{
					FWorkspaceUse& Use = Module.Uses.AddDefaulted_GetRef();
					Use.QualifiedName = Target->Value;
					Use.Required = Form.Head == TEXT("rig-entry")
						? EAnimLispCapability::AnimRuntimeReference
						: EAnimLispCapability::RigCall;
					Use.ExpectedKind = Form.Head == TEXT("rig-entry")
						? EAnimLispSymbolKind::RigEntry
						: EAnimLispSymbolKind::RigFunction;
					Use.Location = Form.Location;
				}
			}
			else if (Form.Head == TEXT("bind-rig-variable"))
			{
				const FAnimLangToken* AnimVariable = FindProperty(Tokens, Form, TEXT("anim-variable"));
				const FAnimLangToken* RigVariable = FindProperty(Tokens, Form, TEXT("rig-variable"));
				if (AnimVariable != nullptr && RigVariable != nullptr)
				{
					FWorkspaceBinding& Binding = Module.Bindings.AddDefaulted_GetRef();
					Binding.AnimVariable = AnimVariable->Value;
					Binding.RigVariable = RigVariable->Value;
					Binding.Location = Form.Location;
				}
			}
		}
	}

	const FAnimLispDefinition* Resolve(const FWorkspaceModule& From, const FString& QualifiedName) const
	{
		FAnimLispModuleId TargetModule = From.Id;
		FString SymbolName = QualifiedName;
		FString Alias;
		if (SplitQualifiedName(QualifiedName, Alias, SymbolName))
		{
			const FAnimLispImport* Import = From.Imports.FindByPredicate([&Alias](const FAnimLispImport& Candidate)
			{
				return Candidate.Alias == Alias;
			});
			if (Import == nullptr) return nullptr;
			TargetModule = Import->Target;
		}
		if (QuarantinedModuleIdentities.Contains(TargetModule.ToString())) return nullptr;
		const int32* DefinitionIndex = DefinitionByModuleAndName.Find(DefinitionLookupKey(TargetModule, SymbolName));
		return DefinitionIndex != nullptr && Definitions.IsValidIndex(*DefinitionIndex)
			? &Definitions[*DefinitionIndex]
			: nullptr;
	}
};

bool FAnimLispSymbolId::operator==(const FAnimLispSymbolId& Other) const
{
	return Module == Other.Module
		&& QualifiedName == Other.QualifiedName
		&& Kind == Other.Kind;
}

uint32 GetTypeHash(const FAnimLispSymbolId& SymbolId)
{
	return HashCombine(
		HashCombine(GetTypeHash(SymbolId.Module), GetTypeHash(SymbolId.QualifiedName)),
		GetTypeHash(static_cast<uint8>(SymbolId.Kind)));
}

FAnimLispWorkspace::FAnimLispWorkspace()
	: Impl(MakeUnique<FImpl>())
{
}

FAnimLispWorkspace::~FAnimLispWorkspace() = default;

void FAnimLispWorkspace::AddSource(const FString& Path, const FString& Source)
{
	Impl->Sources.Add(Path, Source);
}

bool FAnimLispWorkspace::Build(FAnimLangDiagnostics& OutDiag, const bool bAllowLegacyExternalRigs)
{
	OutDiag.Items.Reset();
	Impl->ResetBuild();
	Impl->bAllowLegacyExternalRigs = bAllowLegacyExternalRigs;
	TArray<FString> SourceFiles;
	Impl->Sources.GetKeys(SourceFiles);
	SourceFiles.Sort();
	for (const FString& SourceFile : SourceFiles)
	{
		const FString& Source = Impl->Sources[SourceFile];
		if (SourceFile.EndsWith(TEXT(".riglang"), ESearchCase::IgnoreCase))
		{
			Impl->ParseRigSource(SourceFile, Source);
		}
		else
		{
			Impl->ParseAnimSource(SourceFile, Source);
		}
	}
	for (const FAnimLangDiagnostic& Diagnostic : Impl->EarlyDiagnostics)
	{
		OutDiag.Add(Diagnostic);
	}
	TMap<FString, int32> FirstDefinitionByName;
	for (int32 DefinitionIndex = 0; DefinitionIndex < Impl->Definitions.Num(); ++DefinitionIndex)
	{
		const FAnimLispDefinition& Definition = Impl->Definitions[DefinitionIndex];
		const FString Key = DefinitionLookupKey(Definition.Id.Module, Definition.Id.QualifiedName);
		if (const int32* FirstIndex = FirstDefinitionByName.Find(Key))
		{
			FAnimLangDiagnostic Diagnostic(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Semantic,
				FString::Printf(TEXT("Duplicate symbol '%s'"), *Definition.Id.QualifiedName),
				Definition.Location);
			Diagnostic.AddRelatedLocation(
				Impl->Definitions[*FirstIndex].Location,
				TEXT("First symbol declared here"));
			OutDiag.Add(Diagnostic);
			Impl->QuarantinedDefinitionIndices.Add(*FirstIndex);
			Impl->QuarantinedDefinitionIndices.Add(DefinitionIndex);
			Impl->DefinitionByModuleAndName.Remove(Key);
		}
		else
		{
			FirstDefinitionByName.Add(Key, DefinitionIndex);
		}
	}

	TMap<FString, int32> FirstModuleByIdentity;
	TMap<FString, int32> FirstIndexableModuleByIdentity;
	for (int32 ModuleIndex = 0; ModuleIndex < Impl->Modules.Num(); ++ModuleIndex)
	{
		const FWorkspaceModule& Module = Impl->Modules[ModuleIndex];
		if (Module.Id.AssetPath.IsEmpty()) continue;
		const FString Identity = Module.Id.ToString();
		if (const int32* FirstIndex = FirstModuleByIdentity.Find(Identity))
		{
			FAnimLangDiagnostic Diagnostic(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Module,
				FString::Printf(TEXT("Duplicate module identity '%s'"), *Identity),
				Module.Location);
			Diagnostic.AddRelatedLocation(Impl->Modules[*FirstIndex].Location, TEXT("First module declared here"));
			OutDiag.Add(Diagnostic);
		}
		else
		{
			FirstModuleByIdentity.Add(Identity, ModuleIndex);
		}
		if (!Module.bIndexable) continue;
		if (FirstIndexableModuleByIdentity.Contains(Identity))
		{
			Impl->QuarantinedModuleIdentities.Add(Identity);
		}
		else
		{
			FirstIndexableModuleByIdentity.Add(Identity, ModuleIndex);
		}
	}
	for (const FWorkspaceModule& Module : Impl->Modules)
	{
		if (!Impl->QuarantinedModuleIdentities.Contains(Module.Id.ToString())) continue;
		for (const int32 DefinitionIndex : Module.DefinitionIndices)
		{
			if (!Impl->Definitions.IsValidIndex(DefinitionIndex)) continue;
			Impl->QuarantinedDefinitionIndices.Add(DefinitionIndex);
			const FAnimLispDefinition& Definition = Impl->Definitions[DefinitionIndex];
			Impl->DefinitionByModuleAndName.Remove(
				DefinitionLookupKey(Definition.Id.Module, Definition.Id.QualifiedName));
		}
	}

	for (const FWorkspaceModule& Module : Impl->Modules)
	{
		for (const FRigLangParseError& Error : Module.ParseErrors)
		{
			OutDiag.Add(
				Error.bWarning ? EAnimLangDiagSeverity::Warning : EAnimLangDiagSeverity::Error,
				Error.Message.Contains(TEXT("Duplicate"))
					? EAnimLangDiagCategory::Semantic
					: EAnimLangDiagCategory::Parse,
				Error.Message,
				Error.Location);
		}
		if (Module.bIndexable && Module.Rig.IsValid()) LintRigModule(*Module.Rig, OutDiag);
	}

	TSet<int32> BlockedModules;
	for (const FWorkspaceModule& Module : Impl->Modules)
	{
		const int32 ModuleIndex = static_cast<int32>(&Module - Impl->Modules.GetData());
		if (!Module.bIndexable) continue;
		const FAnimLispImport* DuplicateDependency = nullptr;
		for (const FAnimLispImport& Import : Module.Imports)
		{
			if (Impl->QuarantinedModuleIdentities.Contains(Import.Target.ToString()))
			{
				DuplicateDependency = &Import;
				break;
			}
		}
		if (DuplicateDependency != nullptr)
		{
			const FString DuplicateIdentity = DuplicateDependency->Target.ToString();
			FAnimLangDiagnostic Diagnostic(
				EAnimLangDiagSeverity::Error,
				EAnimLangDiagCategory::Module,
				FString::Printf(
					TEXT("Module '%s' is blocked by duplicate module identity '%s'"),
					*Module.Id.ToString(),
					*DuplicateIdentity),
				DuplicateDependency->Location);
			TArray<FAnimLangSourceLoc> ConflictLocations;
			for (const FWorkspaceModule& Candidate : Impl->Modules)
			{
				if (Candidate.bIndexable && Candidate.Id.ToString() == DuplicateIdentity)
				{
					ConflictLocations.Add(Candidate.Location);
				}
			}
			ConflictLocations.Sort([](const FAnimLangSourceLoc& A, const FAnimLangSourceLoc& B)
			{
				if (A.SourceFile != B.SourceFile) return A.SourceFile < B.SourceFile;
				return A.Offset < B.Offset;
			});
			for (const FAnimLangSourceLoc& ConflictLocation : ConflictLocations)
			{
				Diagnostic.AddRelatedLocation(ConflictLocation, TEXT("Conflicting dependency module declared here"));
			}
			OutDiag.Add(Diagnostic);
			BlockedModules.Add(ModuleIndex);
			continue;
		}
		for (const FAnimLispImport& Import : Module.Imports)
		{
			if (Module.Id.Kind == EAnimLispModuleKind::Rig
				&& Import.Target.Kind == EAnimLispModuleKind::Anim)
			{
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Module,
					TEXT("Rig module cannot import Anim module"),
					Import.Location);
			}
			const int32* TargetIndex = Impl->ModuleByIdentity.Find(Import.Target.ToString());
			if (TargetIndex == nullptr)
			{
				if (Impl->bAllowLegacyExternalRigs
					&& Import.bLegacyExternal
					&& Import.Target.Kind == EAnimLispModuleKind::Rig
					&& Import.ExpectedHash.IsEmpty())
				{
					OutDiag.Add(
						EAnimLangDiagSeverity::Warning,
						EAnimLangDiagCategory::Module,
						FString::Printf(TEXT("Legacy external Rig module '%s' is not covered by the bundle"),
							*Import.Target.ToString()),
						Import.Location);
					continue;
				}
				if (const int32* FailedIndex = Impl->FailedModuleByIdentity.Find(Import.Target.ToString()))
				{
					const FWorkspaceModule& Target = Impl->Modules[*FailedIndex];
					if (!BlockedModules.Contains(ModuleIndex))
					{
						FAnimLangDiagnostic Diagnostic(
							EAnimLangDiagSeverity::Error,
							EAnimLangDiagCategory::Module,
							FString::Printf(
								TEXT("Module '%s' is blocked by module error in '%s'"),
								*Module.Id.ToString(),
								*Target.Id.ToString()),
							Import.Location);
						if (!Target.ParseErrors.IsEmpty())
						{
							Diagnostic.AddRelatedLocation(Target.ParseErrors[0].Location, TEXT("Dependency parse error"));
						}
						OutDiag.Add(Diagnostic);
						BlockedModules.Add(ModuleIndex);
					}
				}
				else
				{
					OutDiag.Add(
						EAnimLangDiagSeverity::Error,
						EAnimLangDiagCategory::Module,
						FString::Printf(TEXT("Unresolved imported module '%s'"), *Import.Target.ToString()),
						Import.Location);
					BlockedModules.Add(ModuleIndex);
				}
				continue;
			}
			const FWorkspaceModule& Target = Impl->Modules[*TargetIndex];
			if (!Import.ExpectedHash.IsEmpty()
				&& !Target.ContentHash.IsEmpty()
				&& Import.ExpectedHash != Target.ContentHash)
			{
				FAnimLangDiagnostic Diagnostic(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Module,
					FString::Printf(
						TEXT("Import expected hash '%s' but module '%s' has hash '%s'"),
						*Import.ExpectedHash,
						*Import.Target.ToString(),
						*Target.ContentHash),
					Import.Location);
				Diagnostic.AddRelatedLocation(Target.Location, TEXT("Imported module declared here"));
				OutDiag.Add(Diagnostic);
			}
		}
	}

	TArray<uint8> ImportVisitState;
	ImportVisitState.SetNumZeroed(Impl->Modules.Num());
	TArray<int32> ImportVisitStack;
	bool bImportCycleReported = false;
	TFunction<void(int32)> VisitRigImports = [&](const int32 ModuleIndex)
	{
		if (bImportCycleReported || !Impl->Modules.IsValidIndex(ModuleIndex)) return;
		ImportVisitState[ModuleIndex] = 1;
		ImportVisitStack.Add(ModuleIndex);
		for (const FAnimLispImport& Import : Impl->Modules[ModuleIndex].Imports)
		{
			if (Import.Target.Kind != EAnimLispModuleKind::Rig) continue;
			const int32* TargetIndex = Impl->ModuleByIdentity.Find(Import.Target.ToString());
			if (TargetIndex == nullptr) continue;
			if (ImportVisitState[*TargetIndex] == 0)
			{
				VisitRigImports(*TargetIndex);
			}
			else if (ImportVisitState[*TargetIndex] == 1 && !bImportCycleReported)
			{
				const int32 StackStart = ImportVisitStack.Find(*TargetIndex);
				TArray<FString> CycleNames;
				for (int32 StackIndex = StackStart; StackIndex < ImportVisitStack.Num(); ++StackIndex)
				{
					CycleNames.Add(Impl->Modules[ImportVisitStack[StackIndex]].Id.AssetPath);
				}
				CycleNames.Add(Impl->Modules[*TargetIndex].Id.AssetPath);
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Module,
					TEXT("module-cycle: Rig import dependency cycle: ") + FString::Join(CycleNames, TEXT(" -> ")),
					Import.Location);
				bImportCycleReported = true;
			}
		}
		ImportVisitStack.Pop(EAllowShrinking::No);
		ImportVisitState[ModuleIndex] = 2;
	};
	for (int32 ModuleIndex = 0; ModuleIndex < Impl->Modules.Num() && !bImportCycleReported; ++ModuleIndex)
	{
		if (Impl->Modules[ModuleIndex].Id.Kind == EAnimLispModuleKind::Rig
			&& Impl->Modules[ModuleIndex].bIndexable
			&& ImportVisitState[ModuleIndex] == 0)
		{
			VisitRigImports(ModuleIndex);
		}
	}

	struct FRigCallModuleEdge
	{
		int32 TargetModuleIndex = INDEX_NONE;
		FAnimLangSourceLoc CallLocation;
	};
	TArray<TArray<FRigCallModuleEdge>> RigCallEdges;
	RigCallEdges.SetNum(Impl->Modules.Num());
	for (int32 ModuleIndex = 0; ModuleIndex < Impl->Modules.Num(); ++ModuleIndex)
	{
		const FWorkspaceModule& Module = Impl->Modules[ModuleIndex];
		if (!Module.bIndexable || Module.Id.Kind != EAnimLispModuleKind::Rig
			|| BlockedModules.Contains(ModuleIndex)) continue;
		for (const FWorkspaceUse& Use : Module.Uses)
		{
			if (Use.ExpectedKind != EAnimLispSymbolKind::RigFunction) continue;
			const FAnimLispDefinition* Target = Impl->Resolve(Module, Use.QualifiedName);
			if (Target == nullptr || Target->Id.Module.Kind != EAnimLispModuleKind::Rig
				|| Target->Id.Module == Module.Id) continue;
			if (const int32* TargetIndex = Impl->ModuleByIdentity.Find(Target->Id.Module.ToString()))
			{
				FRigCallModuleEdge& Edge = RigCallEdges[ModuleIndex].AddDefaulted_GetRef();
				Edge.TargetModuleIndex = *TargetIndex;
				Edge.CallLocation = Use.Location;
			}
		}
	}

	TArray<uint8> VisitState;
	VisitState.SetNumZeroed(Impl->Modules.Num());
	TArray<int32> VisitStack;
	bool bCycleReported = false;
	TFunction<void(int32)> VisitRigModule = [&](const int32 ModuleIndex)
	{
		if (bCycleReported || !Impl->Modules.IsValidIndex(ModuleIndex)) return;
		VisitState[ModuleIndex] = 1;
		VisitStack.Add(ModuleIndex);
		for (const FRigCallModuleEdge& Edge : RigCallEdges[ModuleIndex])
		{
			if (!Impl->Modules.IsValidIndex(Edge.TargetModuleIndex)) continue;
			if (VisitState[Edge.TargetModuleIndex] == 0)
			{
				VisitRigModule(Edge.TargetModuleIndex);
			}
			else if (VisitState[Edge.TargetModuleIndex] == 1 && !bCycleReported)
			{
				const int32 StackStart = VisitStack.Find(Edge.TargetModuleIndex);
				TArray<FString> CycleNames;
				for (int32 StackIndex = StackStart; StackIndex < VisitStack.Num(); ++StackIndex)
				{
					CycleNames.Add(Impl->Modules[VisitStack[StackIndex]].Id.AssetPath);
				}
				CycleNames.Add(Impl->Modules[Edge.TargetModuleIndex].Id.AssetPath);
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Module,
					TEXT("rig-call-cycle: Rig call cycle: ") + FString::Join(CycleNames, TEXT(" -> ")),
					Edge.CallLocation);
				bCycleReported = true;
			}
		}
		VisitStack.Pop(EAllowShrinking::No);
		VisitState[ModuleIndex] = 2;
	};
	for (int32 ModuleIndex = 0; ModuleIndex < Impl->Modules.Num() && !bCycleReported; ++ModuleIndex)
	{
		if (Impl->Modules[ModuleIndex].Id.Kind == EAnimLispModuleKind::Rig
			&& Impl->Modules[ModuleIndex].bIndexable
			&& VisitState[ModuleIndex] == 0)
		{
			VisitRigModule(ModuleIndex);
		}
	}

	for (int32 ModuleIndex = 0; ModuleIndex < Impl->Modules.Num(); ++ModuleIndex)
	{
		const FWorkspaceModule& Module = Impl->Modules[ModuleIndex];
		if (!Module.bIndexable || BlockedModules.Contains(ModuleIndex)) continue;
		for (const FWorkspaceUse& Use : Module.Uses)
		{
			const bool bRigCall = Use.ExpectedKind == EAnimLispSymbolKind::RigFunction;
			if (bRigCall && Use.FunctionIdentifier.IsComplete())
			{
				FString ExplicitAlias;
				FString ExplicitSymbol;
				if (SplitQualifiedName(Use.QualifiedName, ExplicitAlias, ExplicitSymbol))
				{
					const FAnimLispImport* Import = Module.Imports.FindByPredicate(
						[&ExplicitAlias](const FAnimLispImport& Candidate)
						{
							return Candidate.Alias == ExplicitAlias;
						});
					if (Import != nullptr && !RigFunctionHostMatchesModule(
						Use.FunctionIdentifier.HostObject, Import->Target.AssetPath))
					{
						OutDiag.Add(
							EAnimLangDiagSeverity::Error,
							EAnimLangDiagCategory::Semantic,
							FString::Printf(
								TEXT("rig-call-alias-host-mismatch: alias '%s' targets '%s' but the typed identifier host is '%s'"),
								*ExplicitAlias, *Import->Target.AssetPath, *Use.FunctionIdentifier.HostObject),
							Use.Location);
						continue;
					}
				}
			}
			const FAnimLispDefinition* Definition = nullptr;
			if (bRigCall && Use.bLegacyShortCall
				&& !Use.QualifiedName.Contains(TEXT("/"))
				&& !Use.QualifiedName.Contains(TEXT(".")))
			{
				TSet<FAnimLispModuleId> VisibleRigModules;
				VisibleRigModules.Add(Module.Id);
				for (const FAnimLispImport& Import : Module.Imports)
				{
					if (Import.Target.Kind == EAnimLispModuleKind::Rig) VisibleRigModules.Add(Import.Target);
				}
				TArray<const FAnimLispDefinition*> Candidates;
				for (const FAnimLispDefinition& Candidate : Impl->Definitions)
				{
					if (Candidate.Id.Kind == EAnimLispSymbolKind::RigFunction
						&& Candidate.Id.QualifiedName == Use.QualifiedName
						&& VisibleRigModules.Contains(Candidate.Id.Module)
						&& !Impl->QuarantinedModuleIdentities.Contains(Candidate.Id.Module.ToString()))
					{
						Candidates.Add(&Candidate);
					}
				}
				if (Candidates.Num() > 1)
				{
					FAnimLangDiagnostic Diagnostic(
						EAnimLangDiagSeverity::Error,
						EAnimLangDiagCategory::Semantic,
						FString::Printf(TEXT("Ambiguous Rig call '%s'"), *Use.QualifiedName),
						Use.Location);
					for (const FAnimLispDefinition* Candidate : Candidates)
					{
						Diagnostic.AddRelatedLocation(Candidate->Location,
							FString::Printf(TEXT("Candidate in %s"), *Candidate->Id.Module.AssetPath));
					}
					OutDiag.Add(Diagnostic);
					continue;
				}
				if (Candidates.Num() == 1) Definition = Candidates[0];
			}
			else
			{
				Definition = Impl->Resolve(Module, Use.QualifiedName);
			}

			if (Definition != nullptr)
			{
				if (Definition->Id.Kind != Use.ExpectedKind)
				{
					FAnimLangDiagnostic Diagnostic(
						EAnimLangDiagSeverity::Error,
						EAnimLangDiagCategory::Semantic,
						FString::Printf(
							TEXT("Symbol '%s' has the wrong kind for this Rig operation"),
							*Use.QualifiedName),
						Use.Location);
					Diagnostic.AddRelatedLocation(Definition->Location, TEXT("Symbol declared here"));
					OutDiag.Add(Diagnostic);
					continue;
				}
				if (bRigCall && Use.FunctionIdentifier.IsComplete())
				{
					const FRigFunctionIdentifierAST* DeclaredIdentifier =
						Impl->RigFunctionIdentifierBySymbol.Find(SymbolKey(Definition->Id));
					if (DeclaredIdentifier == nullptr || *DeclaredIdentifier != Use.FunctionIdentifier)
					{
						FAnimLangDiagnostic Diagnostic(
							EAnimLangDiagSeverity::Error,
							EAnimLangDiagCategory::Semantic,
							FString::Printf(TEXT("Rig call '%s' exact function identity does not match its declaration"),
								*Use.QualifiedName),
							Use.Location);
						Diagnostic.AddRelatedLocation(Definition->Location, TEXT("Resolved declaration is here"));
						OutDiag.Add(Diagnostic);
						continue;
					}
				}
				const bool bLocalRigCall = bRigCall && Definition->Id.Module == Module.Id;
				if (!bLocalRigCall && !MatchesCapability(Definition->Capability, Use.Required))
				{
					FAnimLangDiagnostic Diagnostic(
						EAnimLangDiagSeverity::Error,
						EAnimLangDiagCategory::Capability,
						FString::Printf(
							TEXT("Symbol '%s' does not provide the required cross-module capability"),
							*Use.QualifiedName),
						Use.Location);
					Diagnostic.AddRelatedLocation(Definition->Location, TEXT("Symbol defined here"));
					OutDiag.Add(Diagnostic);
					continue;
				}
				FAnimLispReference& Reference = Impl->References.AddDefaulted_GetRef();
				Reference.Target = Definition->Id;
				Reference.Location = Use.Location;
			}
			else
			{
				FString UseAlias;
				FString UseName;
				const bool bLegacyExternalUse = Impl->bAllowLegacyExternalRigs
					&& SplitQualifiedName(Use.QualifiedName, UseAlias, UseName)
					&& Module.Imports.ContainsByPredicate([&](const FAnimLispImport& Import)
					{
						return Import.Alias == UseAlias
							&& Import.bLegacyExternal
							&& Import.Target.Kind == EAnimLispModuleKind::Rig
							&& Import.ExpectedHash.IsEmpty()
							&& !Impl->ModuleByIdentity.Contains(Import.Target.ToString());
					});
				if (!bLegacyExternalUse) OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Semantic,
					bRigCall
						? FString::Printf(TEXT("Unresolved Rig call '%s'"), *Use.QualifiedName)
						: FString::Printf(TEXT("Unresolved symbol '%s'"), *Use.QualifiedName),
					Use.Location);
			}
		}
		for (const FWorkspaceRigNodeUse& RigNode : Module.RigNodes)
		{
			const FAnimLispImport* Import = Module.Imports.FindByPredicate(
				[&RigNode](const FAnimLispImport& Candidate) { return Candidate.Alias == RigNode.ImportAlias; });
			const int32* TargetModuleIndex = Import
				? Impl->ModuleByIdentity.Find(Import->Target.ToString()) : nullptr;
			const FWorkspaceModule* TargetModule = TargetModuleIndex && Impl->Modules.IsValidIndex(*TargetModuleIndex)
				? &Impl->Modules[*TargetModuleIndex] : nullptr;
			if (!Import || !TargetModule || !TargetModule->Rig.IsValid())
			{
				const bool bLegacyExternalRig = Impl->bAllowLegacyExternalRigs
					&& Import
					&& Import->bLegacyExternal
					&& Import->Target.Kind == EAnimLispModuleKind::Rig
					&& Import->ExpectedHash.IsEmpty()
					&& !Impl->ModuleByIdentity.Contains(Import->Target.ToString());
				if (!bLegacyExternalRig) OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Module,
					FString::Printf(TEXT("Control Rig node has unresolved Rig import alias '%s'"), *RigNode.ImportAlias),
					RigNode.Location);
				continue;
			}

			const FAnimLispDefinition* EntryDefinition = RigNode.EntryQualifiedName.IsEmpty()
				? nullptr : Impl->Resolve(Module, RigNode.EntryQualifiedName);
			if (!EntryDefinition || EntryDefinition->Id.Kind != EAnimLispSymbolKind::RigEntry
				|| EntryDefinition->Capability != EAnimLispCapability::AnimRuntimeReference)
			{
				OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Capability,
					RigNode.EntryQualifiedName.IsEmpty()
						? TEXT("Control Rig node requires a public Rig entry")
						: FString::Printf(TEXT("Control Rig entry '%s' is not a public runtime Rig entry"), *RigNode.EntryQualifiedName),
					RigNode.Location);
			}
			else
			{
				FAnimLispReference& Reference = Impl->References.AddDefaulted_GetRef();
				Reference.Target = EntryDefinition->Id;
				Reference.Location = RigNode.Location;
			}

			TMap<FString, const FRigVariableAST*> PublicInputs;
			for (const FRigVariableAST& Variable : TargetModule->Rig->Variables)
			{
				if (Variable.Access == ERigVariableAccess::PublicInput)
				{
					PublicInputs.Add(AnimLispStableRuntimeSymbol(Variable.Name), &Variable);
				}
			}
			TSet<FString> SeenInputs;
			for (const FWorkspaceRigInputUse& Input : RigNode.Inputs)
			{
				FString Alias;
				FString InputName;
				if (!SplitQualifiedName(Input.QualifiedName, Alias, InputName)) InputName = Input.QualifiedName;
				if (SeenInputs.Contains(InputName))
				{
					OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
						FString::Printf(TEXT("Duplicate Control Rig input binding '%s'"), *InputName), Input.Location);
					continue;
				}
				SeenInputs.Add(InputName);
				const FRigVariableAST* RigVariable = PublicInputs.FindRef(InputName);
				const FAnimLispDefinition* Definition = Impl->Resolve(Module, Input.QualifiedName);
				if (!RigVariable || !Definition || Definition->Id.Kind != EAnimLispSymbolKind::RigVariable)
				{
					OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
						FString::Printf(TEXT("Unknown or non-public Control Rig input binding '%s'"), *Input.QualifiedName),
						Input.Location);
					continue;
				}
				if (Input.ExpectedType.CPPType.IsEmpty())
				{
					OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Type,
						FString::Printf(TEXT("Control Rig input binding '%s' has unresolved exact type"), *Input.QualifiedName),
						Input.Location);
					continue;
				}
				if (Input.ExpectedType != RigVariable->Type)
				{
					FAnimLangDiagnostic Diagnostic(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Type,
						FString::Printf(TEXT("Control Rig input binding '%s' has incompatible exact Unreal type"), *Input.QualifiedName),
						Input.Location);
					Diagnostic.AddRelatedLocation(RigVariable->Location, TEXT("Rig public input declared here"));
					OutDiag.Add(Diagnostic);
					continue;
				}
				FAnimLispReference& Reference = Impl->References.AddDefaulted_GetRef();
				Reference.Target = Definition->Id;
				Reference.Location = Input.Location;
			}
		}

		for (const FWorkspaceBinding& Binding : Module.Bindings)
		{
			const FAnimLispDefinition* AnimDefinition = Impl->Resolve(Module, Binding.AnimVariable);
			const FAnimLispDefinition* RigDefinition = Impl->Resolve(Module, Binding.RigVariable);
			if (AnimDefinition == nullptr || RigDefinition == nullptr)
			{
				const FString& MissingName = AnimDefinition == nullptr
					? Binding.AnimVariable
					: Binding.RigVariable;
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Semantic,
					FString::Printf(TEXT("Unresolved binding symbol '%s'"), *MissingName),
					Binding.Location);
				continue;
			}
			if (AnimDefinition->Id.Module != Module.Id
				|| AnimDefinition->Id.Module.Kind != EAnimLispModuleKind::Anim
				|| AnimDefinition->Id.Kind != EAnimLispSymbolKind::AnimVariable)
			{
				FAnimLangDiagnostic Diagnostic(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Semantic,
					FString::Printf(
						TEXT("Binding Anim endpoint '%s' must be a local Anim variable"),
						*Binding.AnimVariable),
					Binding.Location);
				Diagnostic.AddRelatedLocation(AnimDefinition->Location, TEXT("Endpoint declared here"));
				OutDiag.Add(Diagnostic);
				continue;
			}
			FString RigAlias;
			FString RigName;
			const bool bQualifiedRigName = SplitQualifiedName(Binding.RigVariable, RigAlias, RigName);
			const bool bImportedRigModule = bQualifiedRigName
				&& Module.Imports.ContainsByPredicate([RigDefinition](const FAnimLispImport& Import)
				{
					return Import.Target == RigDefinition->Id.Module;
				});
			if (!bImportedRigModule
				|| RigDefinition->Id.Module.Kind != EAnimLispModuleKind::Rig
				|| RigDefinition->Id.Kind != EAnimLispSymbolKind::RigVariable)
			{
				FAnimLangDiagnostic Diagnostic(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Semantic,
					FString::Printf(
						TEXT("Binding Rig endpoint '%s' must be an imported Rig variable"),
						*Binding.RigVariable),
					Binding.Location);
				Diagnostic.AddRelatedLocation(RigDefinition->Location, TEXT("Endpoint declared here"));
				OutDiag.Add(Diagnostic);
				continue;
			}
			const ERigVariableAccess* RigAccess = Impl->RigVariableAccessBySymbol.Find(SymbolKey(RigDefinition->Id));
			if (RigAccess == nullptr || *RigAccess != ERigVariableAccess::PublicInput)
			{
				FAnimLangDiagnostic Diagnostic(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Capability,
					FString::Printf(
						TEXT("Binding Rig variable '%s' must be public-input"),
						*Binding.RigVariable),
					Binding.Location);
				Diagnostic.AddRelatedLocation(RigDefinition->Location, TEXT("Rig variable declared here"));
				OutDiag.Add(Diagnostic);
				continue;
			}
			if (AnimDefinition->TypeSignature != RigDefinition->TypeSignature)
			{
				FAnimLangDiagnostic Diagnostic(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Type,
					FString::Printf(
						TEXT("Binding '%s' to '%s' has incompatible exact Unreal types"),
						*Binding.AnimVariable,
						*Binding.RigVariable),
					Binding.Location);
				Diagnostic.AddRelatedLocation(AnimDefinition->Location, TEXT("Anim variable declared here"));
				Diagnostic.AddRelatedLocation(RigDefinition->Location, TEXT("Rig variable declared here"));
				OutDiag.Add(Diagnostic);
				continue;
			}
			FAnimLispReference& AnimReference = Impl->References.AddDefaulted_GetRef();
			AnimReference.Target = AnimDefinition->Id;
			AnimReference.Location = Binding.Location;
			FAnimLispReference& RigReference = Impl->References.AddDefaulted_GetRef();
			RigReference.Target = RigDefinition->Id;
			RigReference.Location = Binding.Location;
		}
	}
	Impl->References.Sort([](const FAnimLispReference& A, const FAnimLispReference& B)
	{
		if (A.Location.SourceFile != B.Location.SourceFile) return A.Location.SourceFile < B.Location.SourceFile;
		if (A.Location.Offset != B.Location.Offset) return A.Location.Offset < B.Location.Offset;
		return SymbolKey(A.Target) < SymbolKey(B.Target);
	});
	OutDiag.Items.Sort([](const FAnimLangDiagnostic& A, const FAnimLangDiagnostic& B)
	{
		if (A.Location.SourceFile != B.Location.SourceFile) return A.Location.SourceFile < B.Location.SourceFile;
		if (A.Location.Offset != B.Location.Offset) return A.Location.Offset < B.Location.Offset;
		if (A.Category != B.Category) return static_cast<uint8>(A.Category) < static_cast<uint8>(B.Category);
		return A.Message < B.Message;
	});
	Impl->bLastBuildSucceeded = !OutDiag.HasErrors();
	return Impl->bLastBuildSucceeded;
}

bool FAnimLispWorkspace::BuildImportPlan(
	TArray<FAnimLispImportPlanEntry>& OutPlan,
	FAnimLangDiagnostics& OutDiag) const
{
	OutPlan.Reset();
	if (!Impl->bLastBuildSucceeded)
	{
		OutDiag.Add(
			EAnimLangDiagSeverity::Error,
			EAnimLangDiagCategory::Module,
			TEXT("Cannot build an import plan before the workspace passes preflight"));
		return false;
	}

	TArray<int32> StableModules;
	for (int32 ModuleIndex = 0; ModuleIndex < Impl->Modules.Num(); ++ModuleIndex)
	{
		const FWorkspaceModule& Module = Impl->Modules[ModuleIndex];
		if (Module.bIndexable
			&& !Impl->QuarantinedModuleIdentities.Contains(Module.Id.ToString()))
		{
			StableModules.Add(ModuleIndex);
		}
	}
	StableModules.Sort([this](const int32 A, const int32 B)
	{
		const FWorkspaceModule& Left = Impl->Modules[A];
		const FWorkspaceModule& Right = Impl->Modules[B];
		const FString LeftIdentity = Left.Id.ToString();
		const FString RightIdentity = Right.Id.ToString();
		return LeftIdentity != RightIdentity
			? LeftIdentity < RightIdentity
			: Left.SourceFile < Right.SourceFile;
	});

	TArray<uint8> VisitState;
	VisitState.SetNumZeroed(Impl->Modules.Num());
	TFunction<void(int32)> Visit = [&](const int32 ModuleIndex)
	{
		if (!Impl->Modules.IsValidIndex(ModuleIndex) || VisitState[ModuleIndex] == 2) return;
		check(VisitState[ModuleIndex] == 0);
		VisitState[ModuleIndex] = 1;

		TArray<int32> Dependencies;
		for (const FAnimLispImport& Import : Impl->Modules[ModuleIndex].Imports)
		{
			if (const int32* TargetIndex = Impl->ModuleByIdentity.Find(Import.Target.ToString()))
			{
				Dependencies.AddUnique(*TargetIndex);
			}
		}
		Dependencies.Sort([this](const int32 A, const int32 B)
		{
			return Impl->Modules[A].Id.ToString() < Impl->Modules[B].Id.ToString();
		});
		for (const int32 DependencyIndex : Dependencies)
		{
			if (VisitState[DependencyIndex] == 0) Visit(DependencyIndex);
		}

		VisitState[ModuleIndex] = 2;
		const FWorkspaceModule& Module = Impl->Modules[ModuleIndex];
		FAnimLispImportPlanEntry& Entry = OutPlan.AddDefaulted_GetRef();
		Entry.ModuleId = Module.Id;
		Entry.SourceFile = Module.SourceFile;
		Entry.ContentHash = Module.ContentHash;
		Entry.AnimAST = Module.Anim;
		Entry.RigAST = Module.Rig;
	};

	for (const int32 ModuleIndex : StableModules)
	{
		if (VisitState[ModuleIndex] == 0) Visit(ModuleIndex);
	}
	return true;
}

const FAnimLispDefinition* FAnimLispWorkspace::FindDefinition(
	const FString& FromFile,
	const FString& QualifiedName) const
{
	const int32* ModuleIndex = Impl->ModuleBySource.Find(FromFile);
	return ModuleIndex != nullptr && Impl->Modules.IsValidIndex(*ModuleIndex)
		? Impl->Resolve(Impl->Modules[*ModuleIndex], QualifiedName)
		: nullptr;
}

TArray<FAnimLispReference> FAnimLispWorkspace::FindReferences(const FAnimLispSymbolId& Symbol) const
{
	TArray<FAnimLispReference> Result;
	for (const FAnimLispReference& Reference : Impl->References)
	{
		if (Reference.Target == Symbol) Result.Add(Reference);
	}
	return Result;
}

TArray<FAnimLispCompletion> FAnimLispWorkspace::Complete(
	const FString& FromFile,
	const EAnimLispCapability Required) const
{
	TArray<FAnimLispCompletion> Result;
	const int32* ModuleIndex = Impl->ModuleBySource.Find(FromFile);
	if (ModuleIndex == nullptr || !Impl->Modules.IsValidIndex(*ModuleIndex)) return Result;
	const FWorkspaceModule& From = Impl->Modules[*ModuleIndex];
	TArray<FAnimLispModuleId> VisibleModules;
	VisibleModules.Add(From.Id);
	for (const FAnimLispImport& Import : From.Imports) VisibleModules.Add(Import.Target);
	for (int32 DefinitionIndex = 0; DefinitionIndex < Impl->Definitions.Num(); ++DefinitionIndex)
	{
		if (Impl->QuarantinedDefinitionIndices.Contains(DefinitionIndex)) continue;
		const FAnimLispDefinition& Definition = Impl->Definitions[DefinitionIndex];
		if (Impl->QuarantinedModuleIdentities.Contains(Definition.Id.Module.ToString())) continue;
		if (!VisibleModules.Contains(Definition.Id.Module)
			|| !MatchesCapability(Definition.Capability, Required))
		{
			continue;
		}
		FAnimLispCompletion& Completion = Result.AddDefaulted_GetRef();
		Completion.Id = Definition.Id;
		Completion.Capability = Definition.Capability;
		Completion.TypeSignature = Definition.TypeSignature;
		Completion.Location = Definition.Location;
		Completion.Guid = Definition.Guid;
	}
	Result.Sort([](const FAnimLispCompletion& A, const FAnimLispCompletion& B)
	{
		return SymbolKey(A.Id) < SymbolKey(B.Id);
	});
	return Result;
}
