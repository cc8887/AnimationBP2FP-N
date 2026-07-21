// AnimLispWorkspace.cpp - Cross-file AnimLang and RigLang symbol workspace
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLispWorkspace.h"

#include "AnimLangParser.h"
#include "AnimLangTokenizer.h"
#include "RigLangParser.h"

namespace
{
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
	TArray<int32> DefinitionIndices;
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
		if (SourcePin != nullptr && TargetPin != nullptr && SourcePin->Type != TargetPin->Type)
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
	for (const FRigHierarchyElementAST& Element : Rig.Hierarchy)
	{
		ElementsByName.FindOrAdd(Element.Name, &Element);
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

	for (const FRigFunctionAST& Function : Rig.Functions) LintRigGraph(Function.Graph, OutDiag);
	for (const FRigEntryAST& Entry : Rig.Entries) LintRigGraph(Entry.Graph, OutDiag);
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
	TSet<FString> QuarantinedModuleIdentities;
	TSet<int32> QuarantinedDefinitionIndices;
	TArray<FAnimLangDiagnostic> EarlyDiagnostics;

	void ResetBuild()
	{
		Modules.Reset();
		Definitions.Reset();
		References.Reset();
		ModuleBySource.Reset();
		ModuleByIdentity.Reset();
		FailedModuleByIdentity.Reset();
		DefinitionByModuleAndName.Reset();
		RigVariableAccessBySymbol.Reset();
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
		for (const FRigImportAST& Import : Rig->Imports) Module.Imports.Add(Import.Import);
		ModuleBySource.Add(SourceFile, Modules.Num() - 1);
		ModuleByIdentity.FindOrAdd(Module.Id.ToString(), Modules.Num() - 1);

		for (const FRigVariableAST& Variable : Rig->Variables)
		{
			const EAnimLispCapability Capability = Variable.Access == ERigVariableAccess::Internal
				? EAnimLispCapability::DefinitionOnly
				: EAnimLispCapability::AnimRuntimeReference;
			const int32 DefinitionIndex = AddDefinition(
				Module,
				Variable.Name,
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
			AddDefinition(Module, Function.Name, EAnimLispSymbolKind::RigFunction, Capability,
				FAnimLispTypeRef(), Function.Location, Function.StableId);
		}
		for (const FRigEntryAST& Entry : Rig->Entries)
		{
			AddDefinition(Module, Entry.Name, EAnimLispSymbolKind::RigEntry,
				EAnimLispCapability::AnimRuntimeReference, FAnimLispTypeRef(), Entry.Location, Entry.StableId);
		}
	}

	void ParseAnimSource(const FString& SourceFile, const FString& Source)
	{
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

bool FAnimLispWorkspace::Build(FAnimLangDiagnostics& OutDiag)
{
	OutDiag.Items.Reset();
	Impl->ResetBuild();
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
				EAnimLangDiagSeverity::Error,
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

	TArray<uint8> VisitState;
	VisitState.SetNumZeroed(Impl->Modules.Num());
	TArray<int32> VisitStack;
	bool bCycleReported = false;
	TFunction<void(int32)> VisitRigModule = [&](const int32 ModuleIndex)
	{
		if (bCycleReported || !Impl->Modules.IsValidIndex(ModuleIndex)) return;
		VisitState[ModuleIndex] = 1;
		VisitStack.Add(ModuleIndex);
		const FWorkspaceModule& Module = Impl->Modules[ModuleIndex];
		for (const FAnimLispImport& Import : Module.Imports)
		{
			if (Import.Target.Kind != EAnimLispModuleKind::Rig) continue;
			const int32* TargetIndex = Impl->ModuleByIdentity.Find(Import.Target.ToString());
			if (TargetIndex == nullptr) continue;
			if (VisitState[*TargetIndex] == 0)
			{
				VisitRigModule(*TargetIndex);
			}
			else if (VisitState[*TargetIndex] == 1 && !bCycleReported)
			{
				const int32 StackStart = VisitStack.Find(*TargetIndex);
				TArray<FString> CycleNames;
				for (int32 StackIndex = StackStart; StackIndex < VisitStack.Num(); ++StackIndex)
				{
					CycleNames.Add(Impl->Modules[VisitStack[StackIndex]].Id.AssetPath);
				}
				CycleNames.Add(Impl->Modules[*TargetIndex].Id.AssetPath);
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Module,
					TEXT("Rig import cycle: ") + FString::Join(CycleNames, TEXT(" -> ")),
					Import.Location);
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
			if (const FAnimLispDefinition* Definition = Impl->Resolve(Module, Use.QualifiedName))
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
				FAnimLispReference& Reference = Impl->References.AddDefaulted_GetRef();
				Reference.Target = Definition->Id;
				Reference.Location = Use.Location;
				if (!MatchesCapability(Definition->Capability, Use.Required))
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
				}
			}
			else
			{
				OutDiag.Add(
					EAnimLangDiagSeverity::Error,
					EAnimLangDiagCategory::Semantic,
					FString::Printf(TEXT("Unresolved symbol '%s'"), *Use.QualifiedName),
					Use.Location);
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
	return !OutDiag.HasErrors();
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
