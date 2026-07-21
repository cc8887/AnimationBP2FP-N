// RigLangAST.cpp - Canonical RigLang printer and semantic comparison
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "RigLangAST.h"

namespace
{
FString Quote(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"), ESearchCase::CaseSensitive);
	return TEXT("\"") + Escaped + TEXT("\"");
}

bool IsEditorOnlyProperty(const FString& Key)
{
	return Key.Equals(TEXT("editor-position"), ESearchCase::IgnoreCase)
		|| Key.Equals(TEXT("editor-position-x"), ESearchCase::IgnoreCase)
		|| Key.Equals(TEXT("editor-position-y"), ESearchCase::IgnoreCase)
		|| Key.Equals(TEXT("node-position"), ESearchCase::IgnoreCase);
}

void AppendProperties(FString& Out, const TMap<FString, FString>& Properties, const FString& Prefix)
{
	TArray<FString> Keys;
	Properties.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		if (!IsEditorOnlyProperty(Key))
		{
			Out += Prefix + TEXT(":") + Key + TEXT(" ") + Properties[Key];
		}
	}
}

const TCHAR* HierarchyKindText(const ERigHierarchyElementKind Kind)
{
	return Kind == ERigHierarchyElementKind::Bone ? TEXT("bone") : TEXT("control");
}

const TCHAR* AccessText(const ERigVariableAccess Access)
{
	switch (Access)
	{
	case ERigVariableAccess::PublicInput: return TEXT("public-input");
	case ERigVariableAccess::PublicOutput: return TEXT("public-output");
	default: return TEXT("internal");
	}
}

const TCHAR* DirectionText(const ERigPinDirection Direction)
{
	switch (Direction)
	{
	case ERigPinDirection::Output: return TEXT("output");
	case ERigPinDirection::IO: return TEXT("io");
	case ERigPinDirection::Hidden: return TEXT("hidden");
	default: return TEXT("input");
	}
}

const TCHAR* CoverageText(const ERigNodeCoverage Coverage)
{
	switch (Coverage)
	{
	case ERigNodeCoverage::Reflected: return TEXT("reflected");
	case ERigNodeCoverage::Lossy: return TEXT("lossy");
	case ERigNodeCoverage::Unsupported: return TEXT("unsupported");
	default: return TEXT("exact");
	}
}

void AppendPin(FString& Out, const FRigPinAST& Pin, const int32 Indent)
{
	const FString Pad = FString::ChrN(Indent, TEXT(' '));
	Out += Pad + TEXT("(pin")
		+ TEXT(" :path ") + Quote(Pin.Path)
		+ TEXT(" :direction ") + DirectionText(Pin.Direction)
		+ TEXT(" :cpp-type ") + Quote(Pin.Type.CPPType)
		+ TEXT(" :cpp-type-object ") + Quote(Pin.Type.CPPTypeObject)
		+ TEXT(" :container-type ") + Quote(Pin.Type.ContainerType)
		+ TEXT(" :default ") + Quote(Pin.DefaultValue)
		+ TEXT(" :execute-context ") + (Pin.bExecuteContext ? TEXT("true") : TEXT("false"));
	AppendProperties(Out, Pin.Properties, TEXT(" "));
	for (const FRigPinAST& SubPin : Pin.SubPins)
	{
		Out += TEXT("\n");
		AppendPin(Out, SubPin, Indent + 2);
	}
	Out += TEXT(")");
}

void AppendNode(FString& Out, const FRigNodeAST& Node, const int32 Indent)
{
	const FString Pad = FString::ChrN(Indent, TEXT(' '));
	Out += Pad + (Node.Kind == ERigNodeKind::Unit ? TEXT("(rig-unit") : TEXT("(rig-call"))
		+ TEXT(" :id ") + Quote(Node.StableId)
		+ TEXT(" :guid ") + Quote(Node.Guid);
	if (Node.Kind == ERigNodeKind::Call)
	{
		Out += TEXT(" :function ") + Quote(Node.FunctionName);
	}
	Out += TEXT(" :class ") + Quote(Node.ClassPath)
		+ TEXT(" :method ") + Quote(Node.MethodName)
		+ TEXT(" :event ") + Quote(Node.EventName)
		+ TEXT(" :injected ") + (Node.bInjected ? TEXT("true") : TEXT("false"))
		+ TEXT(" :coverage ") + CoverageText(Node.Coverage);
	AppendProperties(Out, Node.Properties, TEXT(" "));
	for (const FRigPinAST& Pin : Node.Pins)
	{
		Out += TEXT("\n");
		AppendPin(Out, Pin, Indent + 2);
	}
	Out += TEXT(")");
}

void AppendLink(FString& Out, const FRigLinkAST& Link, const int32 Indent)
{
	Out += FString::ChrN(Indent, TEXT(' ')) + TEXT("(rig-link :from ")
		+ Quote(Link.SourceNodeId + TEXT(".") + Link.SourcePinPath)
		+ TEXT(" :to ") + Quote(Link.TargetNodeId + TEXT(".") + Link.TargetPinPath);
	AppendProperties(Out, Link.Properties, TEXT(" "));
	Out += TEXT(")");
}

void AppendGraph(FString& Out, const FRigGraphAST& Graph, const int32 Indent)
{
	for (const FRigNodeAST& Node : Graph.Nodes)
	{
		Out += TEXT("\n");
		AppendNode(Out, Node, Indent);
	}
	for (const FRigLinkAST& Link : Graph.Links)
	{
		Out += TEXT("\n");
		AppendLink(Out, Link, Indent);
	}
}

FString BuildCanonical(const FRigModuleAST& Module, const bool bIncludeContentHash)
{
	FString Out = TEXT("(rig-module :asset ") + Quote(Module.Header.ModuleId.AssetPath)
		+ TEXT(" :class ") + Quote(Module.Header.AssetClassPath)
		+ TEXT(" :version ") + FString::FromInt(Module.Header.Version);
	if (bIncludeContentHash)
	{
		Out += TEXT(" :content-hash ") + Quote(Module.Header.ContentHash);
	}
	AppendProperties(Out, Module.Header.Properties, TEXT(" "));
	Out += TEXT(")");

	TArray<FRigImportAST> Imports = Module.Imports;
	Imports.Sort([](const FRigImportAST& A, const FRigImportAST& B)
	{
		if (A.Import.Target.AssetPath != B.Import.Target.AssetPath)
		{
			return A.Import.Target.AssetPath < B.Import.Target.AssetPath;
		}
		return A.Import.Alias < B.Import.Alias;
	});
	for (const FRigImportAST& Import : Imports)
	{
		Out += TEXT("\n(import-rig :asset ") + Quote(Import.Import.Target.AssetPath)
			+ TEXT(" :alias ") + Import.Import.Alias
			+ TEXT(" :content-hash ") + Quote(Import.Import.ExpectedHash);
		AppendProperties(Out, Import.Properties, TEXT(" "));
		Out += TEXT(")");
	}

	if (!Module.Hierarchy.IsEmpty())
	{
		Out += TEXT("\n(rig-hierarchy");
		for (const FRigHierarchyElementAST& Element : Module.Hierarchy)
		{
			Out += TEXT("\n  (") + FString(HierarchyKindText(Element.Kind))
				+ TEXT(" :id ") + Quote(Element.StableId)
				+ TEXT(" :name ") + Quote(Element.Name)
				+ TEXT(" :parent ") + Quote(Element.ParentName);
			AppendProperties(Out, Element.Properties, TEXT(" "));
			Out += TEXT(")");
		}
		Out += TEXT(")");
	}

	if (!Module.Variables.IsEmpty())
	{
		TArray<FRigVariableAST> Variables = Module.Variables;
		Variables.Sort([](const FRigVariableAST& A, const FRigVariableAST& B)
		{
			if (A.Name != B.Name) return A.Name < B.Name;
			return A.StableId < B.StableId;
		});
		Out += TEXT("\n(rig-variables");
		for (const FRigVariableAST& Variable : Variables)
		{
			Out += TEXT("\n  (variable :id ") + Quote(Variable.StableId)
				+ TEXT(" :name ") + Quote(Variable.Name)
				+ TEXT(" :access ") + AccessText(Variable.Access)
				+ TEXT(" :cpp-type ") + Quote(Variable.Type.CPPType)
				+ TEXT(" :cpp-type-object ") + Quote(Variable.Type.CPPTypeObject)
				+ TEXT(" :container-type ") + Quote(Variable.Type.ContainerType)
				+ TEXT(" :default ") + Quote(Variable.DefaultValue)
				+ TEXT(" :execute-context ") + (Variable.bExecuteContext ? TEXT("true") : TEXT("false"));
			AppendProperties(Out, Variable.Properties, TEXT(" "));
			Out += TEXT(")");
		}
		Out += TEXT(")");
	}

	TArray<FRigFunctionAST> Functions = Module.Functions;
	Functions.Sort([](const FRigFunctionAST& A, const FRigFunctionAST& B)
	{
		if (A.Name != B.Name) return A.Name < B.Name;
		return A.StableId < B.StableId;
	});
	for (const FRigFunctionAST& Function : Functions)
	{
		Out += TEXT("\n(define-rig-function ") + Quote(Function.Name)
			+ TEXT(" :id ") + Quote(Function.StableId)
			+ TEXT(" :visibility ") + Function.Visibility
			+ TEXT(" :return-cpp-type ") + Quote(Function.ReturnCPPType);
		AppendProperties(Out, Function.Properties, TEXT(" "));
		AppendGraph(Out, Function.Graph, 2);
		Out += TEXT(")");
	}

	TArray<FRigEntryAST> Entries = Module.Entries;
	Entries.Sort([](const FRigEntryAST& A, const FRigEntryAST& B)
	{
		if (A.Name != B.Name) return A.Name < B.Name;
		return A.StableId < B.StableId;
	});
	for (const FRigEntryAST& Entry : Entries)
	{
		Out += TEXT("\n(define-rig-entry ") + Quote(Entry.Name)
			+ TEXT(" :id ") + Quote(Entry.StableId)
			+ TEXT(" :event ") + Quote(Entry.EventName);
		AppendProperties(Out, Entry.Properties, TEXT(" "));
		AppendGraph(Out, Entry.Graph, 2);
		Out += TEXT(")");
	}

	Out += TEXT("\n");
	return Out;
}

void AddCoverage(const FRigGraphAST& Graph, FRigCoverageTotals& Totals)
{
	for (const FRigNodeAST& Node : Graph.Nodes)
	{
		switch (Node.Coverage)
		{
		case ERigNodeCoverage::Exact: ++Totals.Exact; break;
		case ERigNodeCoverage::Reflected: ++Totals.Reflected; break;
		case ERigNodeCoverage::Lossy: ++Totals.Lossy; break;
		case ERigNodeCoverage::Unsupported: ++Totals.Unsupported; break;
		}
	}
}
}

FString FRigModuleAST::ToCanonicalString() const
{
	return BuildCanonical(*this, true);
}

FString FRigModuleAST::ToCanonicalHashInput() const
{
	return BuildCanonical(*this, false);
}

bool FRigModuleAST::SemanticEquals(const FRigModuleAST& Other) const
{
	return ToCanonicalHashInput() == Other.ToCanonicalHashInput();
}

FRigCoverageTotals FRigModuleAST::GetCoverageTotals() const
{
	FRigCoverageTotals Totals;
	for (const FRigFunctionAST& Function : Functions) AddCoverage(Function.Graph, Totals);
	for (const FRigEntryAST& Entry : Entries) AddCoverage(Entry.Graph, Totals);
	return Totals;
}
