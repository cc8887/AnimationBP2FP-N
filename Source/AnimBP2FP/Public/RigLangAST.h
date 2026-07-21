// RigLangAST.h - Authoritative intermediate representation for RigLang
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangDiagnostics.h"
#include "AnimLispModule.h"

enum class ERigHierarchyElementKind : uint8
{
	Bone,
	Control
};

enum class ERigVariableAccess : uint8
{
	PublicInput,
	PublicOutput,
	Internal
};

enum class ERigPinDirection : uint8
{
	Input,
	Output,
	IO,
	Hidden
};

enum class ERigNodeKind : uint8
{
	Unit,
	Call
};

enum class ERigNodeCoverage : uint8
{
	Exact,
	Reflected,
	Lossy,
	Unsupported
};

struct ANIMBP2FP_API FRigModuleHeaderAST
{
	FAnimLispModuleId ModuleId;
	FString AssetClassPath;
	int32 Version = INDEX_NONE;
	FString ContentHash;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigImportAST
{
	FAnimLispImport Import;
	TMap<FString, FString> Properties;
};

struct ANIMBP2FP_API FRigHierarchyElementAST
{
	ERigHierarchyElementKind Kind = ERigHierarchyElementKind::Bone;
	FString StableId;
	FString Name;
	FString ParentName;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigVariableAST
{
	FString StableId;
	FString Name;
	ERigVariableAccess Access = ERigVariableAccess::Internal;
	FAnimLispTypeRef Type;
	FString DefaultValue;
	bool bExecuteContext = false;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigPinAST
{
	FString Path;
	ERigPinDirection Direction = ERigPinDirection::Input;
	FAnimLispTypeRef Type;
	FString DefaultValue;
	bool bExecuteContext = false;
	TArray<FRigPinAST> SubPins;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigNodeAST
{
	ERigNodeKind Kind = ERigNodeKind::Unit;
	FString StableId;
	FString Guid;
	FString ClassPath;
	FString MethodName;
	FString EventName;
	FString FunctionName;
	bool bInjected = false;
	ERigNodeCoverage Coverage = ERigNodeCoverage::Exact;
	TArray<FRigPinAST> Pins;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigLinkAST
{
	FString SourceNodeId;
	FString SourcePinPath;
	FString TargetNodeId;
	FString TargetPinPath;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

/** Nodes and links are owned by exactly one function or entry graph. */
struct ANIMBP2FP_API FRigGraphAST
{
	TArray<FRigNodeAST> Nodes;
	TArray<FRigLinkAST> Links;
};

struct ANIMBP2FP_API FRigFunctionAST
{
	FString StableId;
	FString Name;
	FString Visibility;
	FString ReturnCPPType;
	FRigGraphAST Graph;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigEntryAST
{
	FString StableId;
	FString Name;
	FString EventName;
	FRigGraphAST Graph;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigCoverageTotals
{
	int32 Exact = 0;
	int32 Reflected = 0;
	int32 Lossy = 0;
	int32 Unsupported = 0;

	int32 Total() const { return Exact + Reflected + Lossy + Unsupported; }
};

struct ANIMBP2FP_API FRigModuleAST
{
	FRigModuleHeaderAST Header;
	TArray<FRigImportAST> Imports;
	TArray<FRigHierarchyElementAST> Hierarchy;
	TArray<FRigVariableAST> Variables;
	TArray<FRigFunctionAST> Functions;
	TArray<FRigEntryAST> Entries;

	FString ToCanonicalString() const;
	FString ToCanonicalHashInput() const;
	bool SemanticEquals(const FRigModuleAST& Other) const;
	FRigCoverageTotals GetCoverageTotals() const;
};
