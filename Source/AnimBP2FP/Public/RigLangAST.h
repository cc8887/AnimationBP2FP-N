// RigLangAST.h - Authoritative intermediate representation for RigLang
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangDiagnostics.h"
#include "AnimLispModule.h"

enum class ERigHierarchyElementKind : uint8
{
	Bone,
	Control,
	Null,
	Curve
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
	Visible,
	Hidden,
	Invalid
};

enum class ERigNodeKind : uint8
{
	Unit,
	Call,
	Variable,
	Comment,
	Reroute,
	Entry,
	Return,
	Collapse,
	Dispatch,
	Aggregate,
	InvokeEntry
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

struct ANIMBP2FP_API FRigHierarchyWeightAST
{
	double Location = 0.0;
	double Rotation = 0.0;
	double Scale = 0.0;
};

struct ANIMBP2FP_API FRigHierarchyParentAST
{
	FString StableId;
	FRigHierarchyWeightAST CurrentWeight;
	FRigHierarchyWeightAST InitialWeight;
	FString Label;
	FAnimLangSourceLoc Location;
};

enum class ERigHierarchyTransformRole : uint8
{
	InitialLocal, InitialGlobal, CurrentLocal, CurrentGlobal,
	PoseInitialLocal, PoseInitialGlobal, PoseCurrentLocal, PoseCurrentGlobal,
	OffsetInitialLocal, OffsetInitialGlobal, OffsetCurrentLocal, OffsetCurrentGlobal,
	ShapeInitialLocal, ShapeInitialGlobal, ShapeCurrentLocal, ShapeCurrentGlobal
};

struct ANIMBP2FP_API FRigHierarchyTransformAST
{
	ERigHierarchyTransformRole Role = ERigHierarchyTransformRole::InitialLocal;
	FTransform Value = FTransform::Identity;
	FAnimLangSourceLoc Location;
};

enum class ERigHierarchyStateKind : uint8
{
	BoneType, Curve, ControlSettings, ControlValue, PreferredEuler
};

struct ANIMBP2FP_API FRigHierarchyStateAST
{
	ERigHierarchyStateKind Kind = ERigHierarchyStateKind::BoneType;
	FString Role;
	FString Type;
	/** Exact reflected payload for structured engine state which is not reducible to a scalar value. */
	FString SerializedValue;
	bool bBoolValue = false;
	int64 IntegerValue = 0;
	double NumberValue = 0.0;
	TArray<double> Components;
	FAnimLangSourceLoc Location;
};

enum class ERigHierarchyMetadataValueKind : uint8
{
	Bool, Integer, Float, Name, Vector, Rotator, Quat, Transform, LinearColor, ElementKey,
	BoolArray, IntegerArray, FloatArray, NameArray, VectorArray, RotatorArray, QuatArray, TransformArray, LinearColorArray, ElementKeyArray
};

struct ANIMBP2FP_API FRigHierarchyMetadataAST
{
	FString Name;
	ERigHierarchyMetadataValueKind Kind = ERigHierarchyMetadataValueKind::Name;
	TArray<bool> BoolValues;
	TArray<int64> IntegerValues;
	TArray<double> NumberValues;
	TArray<FString> StringValues;
	TArray<FVector> VectorValues;
	TArray<FRotator> RotatorValues;
	TArray<FQuat> QuatValues;
	TArray<FTransform> TransformValues;
	TArray<FLinearColor> ColorValues;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigHierarchyElementAST
{
	ERigHierarchyElementKind Kind = ERigHierarchyElementKind::Bone;
	FString StableId;
	FString Name;
	FString ParentName;
	TArray<FRigHierarchyParentAST> Parents;
	TArray<FRigHierarchyTransformAST> Transforms;
	TArray<FRigHierarchyStateAST> States;
	TArray<FRigHierarchyMetadataAST> Metadata;
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

struct ANIMBP2FP_API FRigFunctionIdentifierAST
{
	FString HostObject;
	FString LibraryNodePath;

	bool IsSet() const { return !HostObject.IsEmpty() || !LibraryNodePath.IsEmpty(); }
	bool IsComplete() const { return !HostObject.IsEmpty() && !LibraryNodePath.IsEmpty(); }
	bool operator==(const FRigFunctionIdentifierAST& Other) const
	{
		return HostObject == Other.HostObject && LibraryNodePath == Other.LibraryNodePath;
	}
	bool operator!=(const FRigFunctionIdentifierAST& Other) const { return !(*this == Other); }
	FString ToStableId() const;
};

ANIMBP2FP_API bool RigFunctionHostMatchesModule(
	const FString& HostObject,
	const FString& ModuleAssetPath);
ANIMBP2FP_API FString RigFunctionStableRuntimeSymbol(const FString& Value);
ANIMBP2FP_API FString RigFunctionSymbolFromLibraryNodePath(const FString& LibraryNodePath);

struct ANIMBP2FP_API FRigNodeAST
{
	ERigNodeKind Kind = ERigNodeKind::Unit;
	FString StableId;
	FString Guid;
	FString ClassPath;
	FString MethodName;
	FString EventName;
	FString FunctionName;
	FRigFunctionIdentifierAST FunctionIdentifier;
	FString ContainedGraphStableId;
	bool bInjected = false;
	FString InjectionOwnerPin;
	int32 InjectionOrder = INDEX_NONE;
	bool bInjectedAsInput = false;
	FString InjectionInputPin;
	FString InjectionOutputPin;
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

struct ANIMBP2FP_API FRigCallableArgumentAST
{
	FString Name;
	ERigPinDirection Direction = ERigPinDirection::Input;
	FAnimLispTypeRef Type;
	FString DefaultValue;
	bool bExecuteContext = false;
	bool bConstant = false;
	bool bInputVariable = false;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigGraphVariableAST
{
	FString Guid;
	FString Name;
	FAnimLispTypeRef Type;
	FString CPPTypeObjectPath;
	FString DefaultValue;
	/** Culture-stable FTextStringHelper serialization, not display text. */
	FString Category;
	FString Tooltip;
	bool bExposedOnSpawn = false;
	bool bExposeToCinematics = false;
	bool bPublic = false;
	bool bPrivate = true;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigExternalVariableAST
{
	FString Guid;
	FString Name;
	FAnimLispTypeRef Type;
	bool bPublic = false;
	bool bReadOnly = false;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigFunctionDependencyAST
{
	FString HostObject;
	FString LibraryNodePath;
	uint32 Hash = 0;
	FAnimLangSourceLoc Location;
};

/** Nodes and links are owned by exactly one function or entry graph. */
struct ANIMBP2FP_API FRigGraphAST
{
	FString StableId;
	FString EditorGuid;
	FString Role;
	FString ParentStableId;
	TArray<FRigGraphVariableAST> LocalVariables;
	TArray<FRigNodeAST> Nodes;
	TArray<FRigLinkAST> Links;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigFunctionAST
{
	FString StableId;
	FString Name;
	FRigFunctionIdentifierAST FunctionIdentifier;
	FString Visibility;
	FString ReturnCPPType;
	FString GraphStableId;
	TArray<FRigCallableArgumentAST> Arguments;
	TArray<FRigExternalVariableAST> ExternalVariables;
	TArray<FRigFunctionDependencyAST> Dependencies;
	/** Legacy compatibility mirrors. Arguments is authoritative for new canonical text. */
	TArray<FRigCallableArgumentAST> Inputs;
	TArray<FRigCallableArgumentAST> Outputs;
	FRigGraphAST Graph;
	TMap<FString, FString> Properties;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FRigEntryAST
{
	FString StableId;
	FString Name;
	FString EventName;
	FString GraphStableId;
	TArray<FRigCallableArgumentAST> Arguments;
	/** Legacy compatibility mirrors. Arguments is authoritative for new canonical text. */
	TArray<FRigCallableArgumentAST> Inputs;
	TArray<FRigCallableArgumentAST> Outputs;
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
	TArray<FRigGraphAST> Graphs;
	TArray<FRigFunctionAST> Functions;
	TArray<FRigEntryAST> Entries;

	FString ToCanonicalString() const;
	FString ToCanonicalHashInput() const;
	bool SemanticEquals(const FRigModuleAST& Other) const;
	FRigCoverageTotals GetCoverageTotals() const;
};
